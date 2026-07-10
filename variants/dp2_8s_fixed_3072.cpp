// dp2_8s_fixed_widen.cpp — double-loop structure: outer over widen-groups,
// inner exactly 100 iterations. Eliminates iter_count from the hot loop
// entirely (saves 1 GPR + 1 conditional branch per iteration). The fixed
// inner count of 100 lets the compiler unroll the inner loop with -funroll-loops.
// All other parameters identical to dp2_8s_pf4096 (current champion).

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
    const __m256i ascii0 = _mm256_set1_epi8('0');
    __m256i s0 = _mm256_sub_epi8(_mm256_loadu_si256((const __m256i*)p), ascii0);
    __m256i s1 = _mm256_sub_epi8(_mm256_loadu_si256((const __m256i*)(p + 32)), ascii0);
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(s0);
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(s1);
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

// One iteration body (prefetch + mask + process + accumulate).
// Macro to avoid duplicating the inner body three times.
#define ITER_BODY(PFD) \
    _mm_prefetch((const char*)(p0 + (PFD)), _MM_HINT_T1); \
    _mm_prefetch((const char*)(p1 + (PFD)), _MM_HINT_T1); \
    _mm_prefetch((const char*)(p2 + (PFD)), _MM_HINT_T1); \
    _mm_prefetch((const char*)(p3 + (PFD)), _MM_HINT_T1); \
    _mm_prefetch((const char*)(p4 + (PFD)), _MM_HINT_T1); \
    _mm_prefetch((const char*)(p5 + (PFD)), _MM_HINT_T1); \
    _mm_prefetch((const char*)(p6 + (PFD)), _MM_HINT_T1); \
    _mm_prefetch((const char*)(p7 + (PFD)), _MM_HINT_T1); \
    { \
    uint64_t m0 = nl_mask64(p0); \
    uint64_t m1 = nl_mask64(p1); \
    uint64_t m2 = nl_mask64(p2); \
    uint64_t m3 = nl_mask64(p3); \
    uint64_t m4 = nl_mask64(p4); \
    uint64_t m5 = nl_mask64(p5); \
    uint64_t m6 = nl_mask64(p6); \
    uint64_t m7 = nl_mask64(p7); \
    __m128i r0 = process_window_dp(p0, b0, m0); p0 += 64; \
    __m128i r1 = process_window_dp(p1, b1, m1); p1 += 64; \
    __m128i r2 = process_window_dp(p2, b2, m2); p2 += 64; \
    __m128i r3 = process_window_dp(p3, b3, m3); p3 += 64; \
    __m128i r4 = process_window_dp(p4, b4, m4); p4 += 64; \
    __m128i r5 = process_window_dp(p5, b5, m5); p5 += 64; \
    __m128i r6 = process_window_dp(p6, b6, m6); p6 += 64; \
    __m128i r7 = process_window_dp(p7, b7, m7); p7 += 64; \
    acc_u16_add(acc_u16, _mm_add_epi8(r0, r1)); \
    acc_u16_add(acc_u16, _mm_add_epi8(r2, r3)); \
    acc_u16_add(acc_u16, _mm_add_epi8(r4, r5)); \
    acc_u16_add(acc_u16, _mm_add_epi8(r6, r7)); \
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

        size_t min_seg = SIZE_MAX;
        for (int i = 0; i < 8; i++) {
            size_t seg = (size_t)(adj_end[i] - adj_start[i]);
            if (seg < min_seg) min_seg = seg;
        }
        size_t safe_iters = (min_seg > 96) ? (min_seg - 96) / 64 : 0;

        const unsigned char *p0=adj_start[0], *b0=adj_start[0];
        const unsigned char *p1=adj_start[1], *b1=adj_start[1];
        const unsigned char *p2=adj_start[2], *b2=adj_start[2];
        const unsigned char *p3=adj_start[3], *b3=adj_start[3];
        const unsigned char *p4=adj_start[4], *b4=adj_start[4];
        const unsigned char *p5=adj_start[5], *b5=adj_start[5];
        const unsigned char *p6=adj_start[6], *b6=adj_start[6];
        const unsigned char *p7=adj_start[7], *b7=adj_start[7];

        __m256i acc_u16 = _mm256_setzero_si256();

        // Double-loop: outer iterates widen groups, inner is exactly 100 iters.
        // Key: no iter_count variable or conditional in the inner loop.
        // Compiler can unroll the fixed-count inner loop via -funroll-loops.
        // Safety: per iter max u16 contribution = 4 pairs × max_pair_u8(~144) = 576
        // Over 100 iters: 576×100 = 57,600 < 65,535 per lane.
        size_t groups = safe_iters / 100;
        size_t remain = safe_iters % 100;

        for (size_t g = groups; __builtin_expect(g > 0, 1); --g) {
            for (int k = 100; --k >= 0;) {
                ITER_BODY(3072)
            }
            widen_u16(acc_u16, wide_acc);
        }
        // Remainder (< 100 iterations, safe without widening mid-loop)
        for (size_t k = remain; k-- > 0;) {
            ITER_BODY(3072)
        }
        widen_u16(acc_u16, wide_acc);

#undef ITER_BODY

#define STREAM_TAIL(pi, bi, ei) \
        while ((pi) + 96 < (ei)) { \
            acc_u16_add(acc_u16, process_window_dp((pi), (bi), nl_mask64(pi))); \
            (pi) += 64; \
        } \
        widen_u16(acc_u16, wide_acc); \
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
