// dp2_8s_2w.cpp — dp2_8stream with 2 windows per stream per iteration.
// Each of 8 streams advances 128B per main loop iteration (vs 64B in dp2_8stream).
// All 16 nl_mask64 loads (8 streams × 2 windows) are issued before any processing,
// giving the OOO engine 16 outstanding DRAM requests from 8 different DRAM regions.
// Current champion issues 8 outstanding loads; doubling may saturate more line-fill
// buffers (LFB cap ~12-16 on Sapphire Rapids).
//
// Key: the 2nd window of each stream (p+64) is independent of the 1st window's
// base-pointer update — we know the address without processing window 0a first.
// So all 16 loads can be in flight simultaneously.
// Window 0b's process_window_dp needs updated b0 from window 0a — but while we
// wait for b0 (~20cy), we can overlap with processing streams 1a, 2a, ... 7a.

#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__AVX2__) && !defined(BLOCK_SCALAR_SIM)
#include <immintrin.h>
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#if defined(__AVX2__) && !defined(BLOCK_SCALAR_SIM)

static const __m128i place_ctrl[11] = {
    _mm_setzero_si128(),
    _mm_setr_epi8(0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(7,6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(8,7,6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1),
    _mm_setr_epi8(9,8,7,6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1),
};

#define PSHUF(p, len) \
    _mm_shuffle_epi8( \
        _mm_sub_epi8(_mm_loadu_si128((const __m128i*)(p)), _mm_set1_epi8('0')), \
        place_ctrl[(len)])

static inline __m128i tree6(
    __m128i s0, __m128i s1, __m128i s2,
    __m128i s3, __m128i s4, __m128i s5)
{
    __m128i t01   = _mm_add_epi8(s0, s1);
    __m128i t23   = _mm_add_epi8(s2, s3);
    __m128i t45   = _mm_add_epi8(s4, s5);
    __m128i t0123 = _mm_add_epi8(t01, t23);
    return _mm_add_epi8(t0123, t45);
}

static inline __m128i tree5(
    __m128i s0, __m128i s1, __m128i s2, __m128i s3, __m128i s4)
{
    __m128i t01   = _mm_add_epi8(s0, s1);
    __m128i t23   = _mm_add_epi8(s2, s3);
    __m128i t0123 = _mm_add_epi8(t01, t23);
    return _mm_add_epi8(t0123, s4);
}

static inline __m128i tree4(
    __m128i s0, __m128i s1, __m128i s2, __m128i s3)
{
    return _mm_add_epi8(_mm_add_epi8(s0, s1), _mm_add_epi8(s2, s3));
}

static __attribute__((always_inline)) void
acc_u16_add(__m256i& acc_u16, __m128i contrib_u8) {
    acc_u16 = _mm256_add_epi16(acc_u16, _mm256_cvtepu8_epi16(contrib_u8));
}

static __attribute__((noinline)) void
widen_u16(__m256i& acc_u16, uint64_t* wide_acc) {
    alignas(32) uint16_t lanes[16];
    _mm256_store_si256((__m256i*)lanes, acc_u16);
    for (int k = 0; k < 10; k++) wide_acc[k] += lanes[k];
    acc_u16 = _mm256_setzero_si256();
}

static inline uint64_t nl_mask64(const unsigned char* p) {
    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i nl = _mm256_set1_epi8('\n');
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
    return (uint64_t)m0 | ((uint64_t)m1 << 32);
}

static __attribute__((always_inline)) __m128i process_window_dp(
    const unsigned char* __restrict__ p,
    const unsigned char* __restrict__& base,
    uint64_t m)
{
    int cnt = __builtin_popcountll(m);

    if (__builtin_expect(cnt == 6, 1)) {
        uint64_t mm = m;
        unsigned n0,n1,n2,n3,n4,n5;
        n0=__builtin_ctzll(mm); mm&=mm-1;
        n1=__builtin_ctzll(mm); mm&=mm-1;
        n2=__builtin_ctzll(mm); mm&=mm-1;
        n3=__builtin_ctzll(mm); mm&=mm-1;
        n4=__builtin_ctzll(mm); mm&=mm-1;
        n5=__builtin_ctzll(mm);
        const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,
                            *nl3=p+n3,*nl4=p+n4,*nl5=p+n5;
        __m128i r = tree6(
            PSHUF(base,  (unsigned)(nl0-base)),
            PSHUF(nl0+1, (unsigned)(nl1-nl0-1)),
            PSHUF(nl1+1, (unsigned)(nl2-nl1-1)),
            PSHUF(nl2+1, (unsigned)(nl3-nl2-1)),
            PSHUF(nl3+1, (unsigned)(nl4-nl3-1)),
            PSHUF(nl4+1, (unsigned)(nl5-nl4-1)));
        base = nl5+1;
        return r;
    } else if (__builtin_expect(cnt == 5, 1)) {
        uint64_t mm = m;
        unsigned n0,n1,n2,n3,n4;
        n0=__builtin_ctzll(mm); mm&=mm-1;
        n1=__builtin_ctzll(mm); mm&=mm-1;
        n2=__builtin_ctzll(mm); mm&=mm-1;
        n3=__builtin_ctzll(mm); mm&=mm-1;
        n4=__builtin_ctzll(mm);
        const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,
                            *nl3=p+n3,*nl4=p+n4;
        __m128i r = tree5(
            PSHUF(base,  (unsigned)(nl0-base)),
            PSHUF(nl0+1, (unsigned)(nl1-nl0-1)),
            PSHUF(nl1+1, (unsigned)(nl2-nl1-1)),
            PSHUF(nl2+1, (unsigned)(nl3-nl2-1)),
            PSHUF(nl3+1, (unsigned)(nl4-nl3-1)));
        base = nl4+1;
        return r;
    } else if (__builtin_expect(cnt == 7, 0)) {
        uint64_t mm = m;
        unsigned n0,n1,n2,n3,n4,n5,n6;
        n0=__builtin_ctzll(mm); mm&=mm-1;
        n1=__builtin_ctzll(mm); mm&=mm-1;
        n2=__builtin_ctzll(mm); mm&=mm-1;
        n3=__builtin_ctzll(mm); mm&=mm-1;
        n4=__builtin_ctzll(mm); mm&=mm-1;
        n5=__builtin_ctzll(mm); mm&=mm-1;
        n6=__builtin_ctzll(mm);
        const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3,
                            *nl4=p+n4,*nl5=p+n5,*nl6=p+n6;
        __m128i t6 = tree6(
            PSHUF(base,  (unsigned)(nl0-base)),
            PSHUF(nl0+1, (unsigned)(nl1-nl0-1)),
            PSHUF(nl1+1, (unsigned)(nl2-nl1-1)),
            PSHUF(nl2+1, (unsigned)(nl3-nl2-1)),
            PSHUF(nl3+1, (unsigned)(nl4-nl3-1)),
            PSHUF(nl4+1, (unsigned)(nl5-nl4-1)));
        __m128i r = _mm_add_epi8(t6, PSHUF(nl5+1, (unsigned)(nl6-nl5-1)));
        base = nl6+1;
        return r;
    } else if (__builtin_expect(cnt == 4, 0)) {
        uint64_t mm = m;
        unsigned n0,n1,n2,n3;
        n0=__builtin_ctzll(mm); mm&=mm-1;
        n1=__builtin_ctzll(mm); mm&=mm-1;
        n2=__builtin_ctzll(mm); mm&=mm-1;
        n3=__builtin_ctzll(mm);
        const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
        __m128i r = tree4(
            PSHUF(base,  (unsigned)(nl0-base)),
            PSHUF(nl0+1, (unsigned)(nl1-nl0-1)),
            PSHUF(nl1+1, (unsigned)(nl2-nl1-1)),
            PSHUF(nl2+1, (unsigned)(nl3-nl2-1)));
        base = nl3+1;
        return r;
    } else if (__builtin_expect(cnt == 3, 0)) {
        uint64_t mm = m;
        unsigned n0, n1, n2;
        n0=__builtin_ctzll(mm); mm&=mm-1;
        n1=__builtin_ctzll(mm); mm&=mm-1;
        n2=__builtin_ctzll(mm);
        const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2;
        __m128i s0=PSHUF(base,  (unsigned)(nl0-base));
        __m128i s1=PSHUF(nl0+1, (unsigned)(nl1-nl0-1));
        __m128i s2=PSHUF(nl1+1, (unsigned)(nl2-nl1-1));
        __m128i r = _mm_add_epi8(_mm_add_epi8(s0, s1), s2);
        base = nl2+1;
        return r;
    } else if (__builtin_expect(cnt == 8, 0)) {
        uint64_t mm = m;
        unsigned n0,n1,n2,n3,n4,n5,n6,n7;
        n0=__builtin_ctzll(mm); mm&=mm-1;
        n1=__builtin_ctzll(mm); mm&=mm-1;
        n2=__builtin_ctzll(mm); mm&=mm-1;
        n3=__builtin_ctzll(mm); mm&=mm-1;
        n4=__builtin_ctzll(mm); mm&=mm-1;
        n5=__builtin_ctzll(mm); mm&=mm-1;
        n6=__builtin_ctzll(mm); mm&=mm-1;
        n7=__builtin_ctzll(mm);
        const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3,
                            *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7;
        __m128i t6 = tree6(
            PSHUF(base,  (unsigned)(nl0-base)),
            PSHUF(nl0+1, (unsigned)(nl1-nl0-1)),
            PSHUF(nl1+1, (unsigned)(nl2-nl1-1)),
            PSHUF(nl2+1, (unsigned)(nl3-nl2-1)),
            PSHUF(nl3+1, (unsigned)(nl4-nl3-1)),
            PSHUF(nl4+1, (unsigned)(nl5-nl4-1)));
        __m128i t2 = _mm_add_epi8(
            PSHUF(nl5+1, (unsigned)(nl6-nl5-1)),
            PSHUF(nl6+1, (unsigned)(nl7-nl6-1)));
        __m128i r = _mm_add_epi8(t6, t2);
        base = nl7+1;
        return r;
    } else {
        __m128i r = _mm_setzero_si128();
        while (m) {
            unsigned nlpos = __builtin_ctzll(m);
            const unsigned char* nlp = p + nlpos;
            unsigned len = (unsigned)(nlp - base);
            if (__builtin_expect(len > 0 && len <= 10, 1))
                r = _mm_add_epi8(r, PSHUF(base, len));
            base = nlp + 1;
            m &= m - 1;
        }
        return r;
    }
}

static void scalar_tail(const unsigned char* from, const unsigned char* end,
                        uint64_t* wide_acc)
{
    uint64_t ps[10] = {};
    int carry = 0;
    for (const unsigned char* q = end; q-- > from; ) {
        unsigned b = *q;
        if (b == '\n') carry = 0;
        else if (carry < 10) ps[carry++] += b - '0';
        else carry++;
    }
    for (int k = 0; k < 10; k++) wide_acc[k] += ps[k];
}

static uint64_t solve(const unsigned char* data, size_t size) {
    if (size == 0) return 0;

    uint64_t wide_acc[10] = {};

    if (size < 8 * 800) {
        scalar_tail(data, data + size, wide_acc);
        goto reconstruct;
    }

    {
        const unsigned char* end = data + size;
        const unsigned char* adj_start[8];
        const unsigned char* adj_end[8];

        adj_start[0] = data;
        for (int i = 1; i < 8; i++) {
            const unsigned char* q = data + (size_t)i * (size / 8);
            while (q < end && *q != '\n') ++q;
            adj_start[i] = (q < end) ? q + 1 : end;
        }
        for (int i = 0; i < 7; i++) adj_end[i] = adj_start[i + 1];
        adj_end[7] = end;

        const unsigned char *p0=adj_start[0], *b0=adj_start[0];
        const unsigned char *p1=adj_start[1], *b1=adj_start[1];
        const unsigned char *p2=adj_start[2], *b2=adj_start[2];
        const unsigned char *p3=adj_start[3], *b3=adj_start[3];
        const unsigned char *p4=adj_start[4], *b4=adj_start[4];
        const unsigned char *p5=adj_start[5], *b5=adj_start[5];
        const unsigned char *p6=adj_start[6], *b6=adj_start[6];
        const unsigned char *p7=adj_start[7], *b7=adj_start[7];

        // Safe-end: leave 160 bytes margin for 2-window tail (2×64+32)
        const unsigned char *s0=(adj_end[0]>adj_start[0]+160)?adj_end[0]-160:adj_start[0];
        const unsigned char *s1=(adj_end[1]>adj_start[1]+160)?adj_end[1]-160:adj_start[1];
        const unsigned char *s2=(adj_end[2]>adj_start[2]+160)?adj_end[2]-160:adj_start[2];
        const unsigned char *s3=(adj_end[3]>adj_start[3]+160)?adj_end[3]-160:adj_start[3];
        const unsigned char *s4=(adj_end[4]>adj_start[4]+160)?adj_end[4]-160:adj_start[4];
        const unsigned char *s5=(adj_end[5]>adj_start[5]+160)?adj_end[5]-160:adj_start[5];
        const unsigned char *s6=(adj_end[6]>adj_start[6]+160)?adj_end[6]-160:adj_start[6];
        const unsigned char *s7=(adj_end[7]>adj_start[7]+160)?adj_end[7]-160:adj_start[7];

        __m256i acc_u16 = _mm256_setzero_si256();
        int iter_count = 0;

        // Main loop: 8 streams × 2 windows = 16 outstanding DRAM requests per iteration.
        // Prefetch 2 iterations (256B) × 8 streams ahead (at 1536B = 12 iterations ahead).
        // All 16 mask loads are issued before any processing call.
        while (__builtin_expect(p0 < s0 & p1 < s1 & p2 < s2 & p3 < s3 &
                                p4 < s4 & p5 < s5 & p6 < s6 & p7 < s7, 1)) {
            // Prefetch 2 windows ahead per stream at 1536B lookahead
            _mm_prefetch((const char*)(p0 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p0 + 1600), _MM_HINT_T1);
            _mm_prefetch((const char*)(p1 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p1 + 1600), _MM_HINT_T1);
            _mm_prefetch((const char*)(p2 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p2 + 1600), _MM_HINT_T1);
            _mm_prefetch((const char*)(p3 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p3 + 1600), _MM_HINT_T1);
            _mm_prefetch((const char*)(p4 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p4 + 1600), _MM_HINT_T1);
            _mm_prefetch((const char*)(p5 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p5 + 1600), _MM_HINT_T1);
            _mm_prefetch((const char*)(p6 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p6 + 1600), _MM_HINT_T1);
            _mm_prefetch((const char*)(p7 + 1536), _MM_HINT_T1);
            _mm_prefetch((const char*)(p7 + 1600), _MM_HINT_T1);

            // Issue all 16 mask loads BEFORE processing — 16 independent DRAM requests.
            // The 'b' windows (p+64) are independent of 'a' processing:
            // their address is p+64 which is known before any base update.
            uint64_t m0a = nl_mask64(p0);
            uint64_t m0b = nl_mask64(p0 + 64);
            uint64_t m1a = nl_mask64(p1);
            uint64_t m1b = nl_mask64(p1 + 64);
            uint64_t m2a = nl_mask64(p2);
            uint64_t m2b = nl_mask64(p2 + 64);
            uint64_t m3a = nl_mask64(p3);
            uint64_t m3b = nl_mask64(p3 + 64);
            uint64_t m4a = nl_mask64(p4);
            uint64_t m4b = nl_mask64(p4 + 64);
            uint64_t m5a = nl_mask64(p5);
            uint64_t m5b = nl_mask64(p5 + 64);
            uint64_t m6a = nl_mask64(p6);
            uint64_t m6b = nl_mask64(p6 + 64);
            uint64_t m7a = nl_mask64(p7);
            uint64_t m7b = nl_mask64(p7 + 64);

            // Process: 'a' window first (updates b), then 'b' window (uses updated b)
            __m128i r0a = process_window_dp(p0,    b0, m0a);
            __m128i r0b = process_window_dp(p0+64, b0, m0b); p0 += 128;
            __m128i r1a = process_window_dp(p1,    b1, m1a);
            __m128i r1b = process_window_dp(p1+64, b1, m1b); p1 += 128;
            __m128i r2a = process_window_dp(p2,    b2, m2a);
            __m128i r2b = process_window_dp(p2+64, b2, m2b); p2 += 128;
            __m128i r3a = process_window_dp(p3,    b3, m3a);
            __m128i r3b = process_window_dp(p3+64, b3, m3b); p3 += 128;
            __m128i r4a = process_window_dp(p4,    b4, m4a);
            __m128i r4b = process_window_dp(p4+64, b4, m4b); p4 += 128;
            __m128i r5a = process_window_dp(p5,    b5, m5a);
            __m128i r5b = process_window_dp(p5+64, b5, m5b); p5 += 128;
            __m128i r6a = process_window_dp(p6,    b6, m6a);
            __m128i r6b = process_window_dp(p6+64, b6, m6b); p6 += 128;
            __m128i r7a = process_window_dp(p7,    b7, m7a);
            __m128i r7b = process_window_dp(p7+64, b7, m7b); p7 += 128;

            // Accumulate: pair 4 windows in u8 (max 4×72=288 > 255, overflow!), so pair 2.
            // 2 windows per pair: max 2×72=144 ≤ 255 (safe).
            acc_u16_add(acc_u16, _mm_add_epi8(r0a, r0b));
            acc_u16_add(acc_u16, _mm_add_epi8(r1a, r1b));
            acc_u16_add(acc_u16, _mm_add_epi8(r2a, r2b));
            acc_u16_add(acc_u16, _mm_add_epi8(r3a, r3b));
            acc_u16_add(acc_u16, _mm_add_epi8(r4a, r4b));
            acc_u16_add(acc_u16, _mm_add_epi8(r5a, r5b));
            acc_u16_add(acc_u16, _mm_add_epi8(r6a, r6b));
            acc_u16_add(acc_u16, _mm_add_epi8(r7a, r7b));

            // Widen every 50 iterations (8 acc_u16_add per iter; 50×8=400 calls ×max_u8=144 < 65535).
            // Safety: 50 × 8 × 144 = 57,600 < 65,535 ✓
            if (__builtin_expect(++iter_count >= 50, 0)) {
                widen_u16(acc_u16, wide_acc);
                iter_count = 0;
            }
        }

        // Per-stream tail cleanup (single-window)
#define STREAM_TAIL(pi, bi, ei) \
        while ((pi) + 96 < (ei)) { \
            acc_u16_add(acc_u16, process_window_dp((pi), (bi), nl_mask64(pi))); \
            (pi) += 64; \
            if (__builtin_expect(++iter_count >= 50, 0)) { \
                widen_u16(acc_u16, wide_acc); iter_count = 0; } \
        } \
        widen_u16(acc_u16, wide_acc); \
        iter_count = 0; \
        scalar_tail((bi), (ei), wide_acc);

        STREAM_TAIL(p0, b0, adj_end[0])
        STREAM_TAIL(p1, b1, adj_end[1])
        STREAM_TAIL(p2, b2, adj_end[2])
        STREAM_TAIL(p3, b3, adj_end[3])
        STREAM_TAIL(p4, b4, adj_end[4])
        STREAM_TAIL(p5, b5, adj_end[5])
        STREAM_TAIL(p6, b6, adj_end[6])
        STREAM_TAIL(p7, b7, adj_end[7])
#undef STREAM_TAIL
    }

reconstruct:
    {
        static const uint64_t POW10[10] = {
            1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL,
            100000ULL, 1000000ULL, 10000000ULL, 100000000ULL, 1000000000ULL
        };
        uint64_t sum = 0;
        for (int k = 0; k < 10; k++) sum += wide_acc[k] * POW10[k];
        return sum;
    }
}

#else
static uint64_t solve(const unsigned char* data, size_t size) {
    uint64_t ps[10] = {};
    if (size == 0) return 0;
    int carry = 0;
    for (size_t i = size; i-- > 0; ) {
        unsigned b = data[i];
        if (b == '\n') carry = 0;
        else if (carry < 10) ps[carry++] += b - '0';
        else carry++;
    }
    static const uint64_t P[10] = {
        1,10,100,1000,10000,100000,1000000,10000000,100000000,1000000000
    };
    uint64_t sum = 0;
    for (int k = 0; k < 10; k++) sum += ps[k] * P[k];
    return sum;
}
#endif

int main() {
    struct stat st;
    if (fstat(0, &st) != 0) return 1;
    size_t size = (size_t)st.st_size;
    if (size > 0) {
        const unsigned char* data = (const unsigned char*)
            mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, 0, 0);
        if (data != MAP_FAILED) {
            madvise((void*)data, size, MADV_HUGEPAGE);
            madvise((void*)data, size, MADV_COLLAPSE);
            madvise((void*)data, size, MADV_SEQUENTIAL);
            printf("%" PRIu64 "\n", solve(data, size));
            return 0;
        }
    }
    static unsigned char buf[1 << 26];
    size_t total = 0; ssize_t n;
    while (total < sizeof(buf) && (n = read(0, buf + total, sizeof(buf) - total)) > 0)
        total += (size_t)n;
    printf("%" PRIu64 "\n", solve(buf, total));
    return 0;
}
