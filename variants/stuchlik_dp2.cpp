// stuchlik_dp2.cpp — Proper SIMD pshufb digit-place accumulation (Change A, v2)
// For each number of length L: pshufb routes digits to place lanes 0..L-1.
// No per-number multiply. Reconstruct as Σ acc[k] × 10^k at the end.
//
// Key correctness insight: each window's tree-reduced contribution fits in u8
// (max 8 numbers × 9 digits/place = 72 ≤ 255). But accumulating across many
// windows overflows u8. Fix: zero-extend each window's u8 contribution to u16
// and accumulate into a __m256i u16 accumulator. Widen to u64 every 7000
// numbers (7000 × 9 = 63000 < 65535 = u16 max). Correct and efficient.
//
// Parallelism: TREE macros combine 6 numbers' contributions in 4 paddb latency
// cycles instead of 6. Loads and pshufb are independent and overlap via OOO.
// Same 8-window interleaved structure + T1@1536B prefetch as champion.

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

// ctrl[L]: 16-byte pshufb ctrl: place 0 gets byte[L-1], place 1 gets byte[L-2], ...
// place L-1 gets byte[0]. Places >= L get 0x80 → pshufb zeroes those output lanes.
// We use -1 (= 0xFF, high bit set) for "zero this output lane".
static const __m128i place_ctrl[11] = {
    _mm_setzero_si128(),  // len=0 unused
    _mm_setr_epi8(0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),  // len=1
    _mm_setr_epi8(1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),   // len=2
    _mm_setr_epi8(2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),    // len=3
    _mm_setr_epi8(3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),     // len=4
    _mm_setr_epi8(4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),      // len=5
    _mm_setr_epi8(5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1),       // len=6
    _mm_setr_epi8(6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1,-1),        // len=7
    _mm_setr_epi8(7,6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1,-1),         // len=8
    _mm_setr_epi8(8,7,6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1,-1),          // len=9
    _mm_setr_epi8(9,8,7,6,5,4,3,2,1,0,-1,-1,-1,-1,-1,-1),           // len=10
};

// Produce a pshufb-scattered u8 contribution for one number.
// Loads 16 bytes from p (garbage past len is masked by 0xFF in ctrl).
#define PSHUF(p, len) \
    _mm_shuffle_epi8( \
        _mm_sub_epi8(_mm_loadu_si128((const __m128i*)(p)), _mm_set1_epi8('0')), \
        place_ctrl[(len)])

// Tree-reduce N contributions → single u8 result (all intermediates fit in u8:
// max path is 6 contributions × 9 = 54 ≤ 255).
// Caller zero-extends the result to u16 and adds to acc_u16.

static inline __m128i tree6(
    __m128i s0, __m128i s1, __m128i s2,
    __m128i s3, __m128i s4, __m128i s5)
{
    __m128i t01   = _mm_add_epi8(s0, s1);
    __m128i t23   = _mm_add_epi8(s2, s3);
    __m128i t45   = _mm_add_epi8(s4, s5);
    __m128i t0123 = _mm_add_epi8(t01, t23);
    return _mm_add_epi8(t0123, t45);  // max 54 per lane ✓ (fits u8)
}

static inline __m128i tree5(
    __m128i s0, __m128i s1, __m128i s2, __m128i s3, __m128i s4)
{
    __m128i t01   = _mm_add_epi8(s0, s1);
    __m128i t23   = _mm_add_epi8(s2, s3);
    __m128i t0123 = _mm_add_epi8(t01, t23);
    return _mm_add_epi8(t0123, s4);  // max 45 per lane ✓
}

static inline __m128i tree4(
    __m128i s0, __m128i s1, __m128i s2, __m128i s3)
{
    return _mm_add_epi8(_mm_add_epi8(s0, s1), _mm_add_epi8(s2, s3));  // max 36 ✓
}

// Add a window's u8 contribution to the u16 accumulator via zero-extension.
// _mm256_cvtepu8_epi16 (VPMOVZXBW): zero-extends 16 u8 lanes → 16 u16 lanes.
// acc_u16 is a 256-bit register with 16 u16 lanes; we use lanes 0..9 for places.
static __attribute__((always_inline)) void
acc_u16_add(__m256i& acc_u16, __m128i contrib_u8) {
    acc_u16 = _mm256_add_epi16(acc_u16, _mm256_cvtepu8_epi16(contrib_u8));
}

// Widen acc_u16 (places 0..9 in u16 lanes 0..9) into wide_acc[10] (u64).
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

// Process one 64-byte window; returns u8 contribution (to be zero-extended by caller).
// Also returns number of numbers via ret_cnt for widen scheduling.
static __attribute__((always_inline)) __m128i process_window_dp(
    const unsigned char* __restrict__ p,
    const unsigned char* __restrict__& base,
    uint64_t m,
    int& ret_cnt)
{
    int cnt = __builtin_popcountll(m);
    ret_cnt = cnt;

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
        // t6 max 54, s6 max 9, sum max 63 ≤ 255 ✓
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
        __m128i r = _mm_add_epi8(_mm_add_epi8(s0, s1), s2);  // max 27 ✓
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
        // t6 ≤ 54, t2 ≤ 18, sum ≤ 72 ✓
        __m128i r = _mm_add_epi8(t6, t2);
        base = nl7+1;
        return r;
    } else {
        // Generic: while loop with scalar pshufb per number
        __m128i r = _mm_setzero_si128();
        ret_cnt = 0;
        while (m) {
            unsigned nlpos = __builtin_ctzll(m);
            const unsigned char* nlp = p + nlpos;
            unsigned len = (unsigned)(nlp - base);
            if (__builtin_expect(len > 0 && len <= 10, 1)) {
                // Linear add: each number adds ≤9 per lane; with ≤12 numbers in
                // while loop (edge case), max 12×9=108 ≤ 255 ✓ for intermediate.
                r = _mm_add_epi8(r, PSHUF(base, len));
                ret_cnt++;
            }
            base = nlp + 1;
            m &= m - 1;
        }
        return r;
    }
}

// Scalar back-to-front tail for correctness on unaligned bytes.
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

    if (size < 800) {
        scalar_tail(data, data + size, wide_acc);
        goto reconstruct;
    }

    {
        __m256i acc_u16 = _mm256_setzero_si256();
        int num_count = 0;

        const unsigned char* p    = data;
        const unsigned char* end  = data + size;
        const unsigned char* base = data;
        const unsigned char* safe_end = end - 544;

        while (p < safe_end) {
            _mm_prefetch((const char*)(p + 1536),       _MM_HINT_T1);
            _mm_prefetch((const char*)(p + 1536 + 64),  _MM_HINT_T1);
            _mm_prefetch((const char*)(p + 1536 + 128), _MM_HINT_T1);
            _mm_prefetch((const char*)(p + 1536 + 192), _MM_HINT_T1);
            _mm_prefetch((const char*)(p + 1536 + 256), _MM_HINT_T1);
            _mm_prefetch((const char*)(p + 1536 + 320), _MM_HINT_T1);
            _mm_prefetch((const char*)(p + 1536 + 384), _MM_HINT_T1);
            _mm_prefetch((const char*)(p + 1536 + 448), _MM_HINT_T1);

            int c0,c1,c2,c3,c4,c5,c6,c7;
            uint64_t m0 = nl_mask64(p);
            uint64_t m1 = nl_mask64(p + 64);
            __m128i r0 = process_window_dp(p,       base, m0, c0);
            uint64_t m2 = nl_mask64(p + 128);
            acc_u16_add(acc_u16, r0);
            __m128i r1 = process_window_dp(p + 64,  base, m1, c1);
            uint64_t m3 = nl_mask64(p + 192);
            acc_u16_add(acc_u16, r1);
            __m128i r2 = process_window_dp(p + 128, base, m2, c2);
            uint64_t m4 = nl_mask64(p + 256);
            acc_u16_add(acc_u16, r2);
            __m128i r3 = process_window_dp(p + 192, base, m3, c3);
            uint64_t m5 = nl_mask64(p + 320);
            acc_u16_add(acc_u16, r3);
            __m128i r4 = process_window_dp(p + 256, base, m4, c4);
            uint64_t m6 = nl_mask64(p + 384);
            acc_u16_add(acc_u16, r4);
            __m128i r5 = process_window_dp(p + 320, base, m5, c5);
            uint64_t m7 = nl_mask64(p + 448);
            acc_u16_add(acc_u16, r5);
            __m128i r6 = process_window_dp(p + 384, base, m6, c6);
            acc_u16_add(acc_u16, r6);
            __m128i r7 = process_window_dp(p + 448, base, m7, c7);
            acc_u16_add(acc_u16, r7);
            p += 512;

            num_count += c0+c1+c2+c3+c4+c5+c6+c7;
            // u16 max = 65535; 7000 numbers × 9 per lane = 63000 < 65535 ✓
            if (__builtin_expect(num_count >= 7000, 0)) {
                widen_u16(acc_u16, wide_acc);
                num_count = 0;
            }
        }

        // Single-window tail
        while (p + 96 < end) {
            uint64_t m = nl_mask64(p);
            int c;
            acc_u16_add(acc_u16, process_window_dp(p, base, m, c));
            p += 64;
        }

        // Flush acc_u16
        widen_u16(acc_u16, wide_acc);

        // Scalar tail for remaining bytes (last <96 bytes)
        scalar_tail(base, end, wide_acc);
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
// Scalar fallback (ARM / non-AVX2) — back-to-front carry, no multiply
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
