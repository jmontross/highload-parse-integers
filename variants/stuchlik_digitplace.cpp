// stuchlik_digitplace.cpp — Digit-place accumulation
// place_sum[k] = sum of all digits at decimal place k (k=0: units) across all numbers.
// Hot loop: no per-number multiply; just scatter each digit to place_sum[len-1-i].
// Reconstruct with 10 multiplications at the end: Σ place_sum[k] × 10^k.
// AVX2: nl_mask64 finds newlines in 64-byte chunks; digits scattered by switch table.
// Scalar fallback: back-to-front carry-tracking loop (no multiply, serial carry++).

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

static inline uint64_t reconstruct(const uint64_t* ps) {
    return ps[0]
         + ps[1] * 10ULL
         + ps[2] * 100ULL
         + ps[3] * 1000ULL
         + ps[4] * 10000ULL
         + ps[5] * 100000ULL
         + ps[6] * 1000000ULL
         + ps[7] * 10000000ULL
         + ps[8] * 100000000ULL
         + ps[9] * 1000000000ULL;
}

// Scatter digits of number starting at `start` with `len` digits to place_sum.
// digit[0] (most significant) → place len-1; digit[len-1] (units) → place 0.
static __attribute__((always_inline)) void
scatter_digits(const unsigned char* start, unsigned len, uint64_t* __restrict__ ps)
{
    switch (len) {
        default: return;
        // no break needed: each case falls through from high to low places
        case 10: ps[9] += start[0] - '0'; start++;
        /* fall through */
        case 9:  ps[8] += start[0] - '0'; start++;
        /* fall through */
        case 8:  ps[7] += start[0] - '0'; start++;
        /* fall through */
        case 7:  ps[6] += start[0] - '0'; start++;
        /* fall through */
        case 6:  ps[5] += start[0] - '0'; start++;
        /* fall through */
        case 5:  ps[4] += start[0] - '0'; start++;
        /* fall through */
        case 4:  ps[3] += start[0] - '0'; start++;
        /* fall through */
        case 3:  ps[2] += start[0] - '0'; start++;
        /* fall through */
        case 2:  ps[1] += start[0] - '0'; start++;
        /* fall through */
        case 1:  ps[0] += start[0] - '0';
    }
}

#if defined(__AVX2__) && !defined(BLOCK_SCALAR_SIM)

static inline uint64_t nl_mask64(const unsigned char* p) {
    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i nl  = _mm256_set1_epi8('\n');
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
    return (uint64_t)m0 | ((uint64_t)m1 << 32);
}

// Inline: drain all newlines in mask `m` (window starts at `win`), updating base.
#define DRAIN_MASK(win, mm) do { \
    while (mm) { \
        unsigned _nlpos = __builtin_ctzll(mm); \
        const unsigned char* _nlp = (win) + _nlpos; \
        unsigned _len = (unsigned)(_nlp - base); \
        if (__builtin_expect(_len - 1u < 10u, 1)) \
            scatter_digits(base, _len, place_sum); \
        base = _nlp + 1; \
        mm &= mm - 1; \
    } \
} while (0)

static uint64_t solve(const unsigned char* data, size_t size) {
    uint64_t place_sum[10] = {};
    if (size == 0) return 0;

    const unsigned char* end      = data + size;
    const unsigned char* base     = data;
    const unsigned char* p        = data;
    const unsigned char* safe_end = end - 544;  // same guard as champion

    while (__builtin_expect(p < safe_end, 1)) {
        _mm_prefetch((const char*)(p + 1536),       _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 64),  _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 128), _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 192), _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 256), _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 320), _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 384), _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 448), _MM_HINT_T1);

        uint64_t m0 = nl_mask64(p);
        uint64_t m1 = nl_mask64(p + 64);
        DRAIN_MASK(p,       m0);
        uint64_t m2 = nl_mask64(p + 128);
        DRAIN_MASK(p + 64,  m1);
        uint64_t m3 = nl_mask64(p + 192);
        DRAIN_MASK(p + 128, m2);
        uint64_t m4 = nl_mask64(p + 256);
        DRAIN_MASK(p + 192, m3);
        uint64_t m5 = nl_mask64(p + 320);
        DRAIN_MASK(p + 256, m4);
        uint64_t m6 = nl_mask64(p + 384);
        DRAIN_MASK(p + 320, m5);
        uint64_t m7 = nl_mask64(p + 448);
        DRAIN_MASK(p + 384, m6);
        DRAIN_MASK(p + 448, m7);
        p += 512;
    }

    // Single-window tail
    while (p + 96 < end) {
        uint64_t m = nl_mask64(p);
        DRAIN_MASK(p, m);
        p += 64;
    }

    // Byte-by-byte tail (last <96 bytes)
    while (p < end) {
        if (*p == '\n') {
            unsigned len = (unsigned)(p - base);
            if (len - 1u < 10u) scatter_digits(base, len, place_sum);
            base = p + 1;
        }
        p++;
    }
    // Final number with no trailing newline
    if (base < end) {
        unsigned len = (unsigned)(end - base);
        if (len - 1u < 10u) scatter_digits(base, len, place_sum);
    }

    return reconstruct(place_sum);
}

#undef DRAIN_MASK

#else  // scalar ARM fallback: back-to-front, no multiply

static uint64_t solve(const unsigned char* data, size_t size) {
    uint64_t place_sum[10] = {};
    if (size == 0) return 0;
    int carry = 0;
    for (size_t i = size; i-- > 0; ) {
        unsigned b = data[i];
        if (b == '\n') carry = 0;
        else if (__builtin_expect(carry < 10, 1)) { place_sum[carry++] += b - '0'; }
        else carry++;
    }
    return reconstruct(place_sum);
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
