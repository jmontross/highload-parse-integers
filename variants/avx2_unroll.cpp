// HighLoad.fun — parse_integers  (VARIANT: avx2_unroll)
// Build on avx2_blockparse: same 32-byte movemask scan, but replace the
// do-while(m) loop with a popcount-based fast path for the two common cases
// (exactly 2 or exactly 3 numbers per 32-byte block). The `while(m)` exit
// branch fires once per block and its iteration-count varies (2 or 3), making
// it a mild misprediction source. Unrolling eliminates that branch entirely
// for the common paths. Also removes the `if (len)` dead guard (len is always
// ≥1 for valid input).
// Additionally uses p+=32 fixed stride (outer `base` variable) instead of
// `p=base`, so the scan window always advances 32 bytes regardless of where
// the last newline was — eliminating the 30% redundant re-scanning bytes.
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

static inline uint32_t parse_8(uint64_t chunk) {
    uint64_t lo = (chunk & 0x0f000f000f000f00ULL) >> 8;
    uint64_t hi = (chunk & 0x000f000f000f000fULL) * 10;
    chunk = lo + hi;
    lo = (chunk & 0x00ff000000ff0000ULL) >> 16;
    hi = (chunk & 0x000000ff000000ffULL) * 100;
    chunk = lo + hi;
    lo = (chunk & 0x0000ffff00000000ULL) >> 32;
    hi = (chunk & 0x000000000000ffffULL) * 10000;
    chunk = lo + hi;
    return (uint32_t)chunk;
}

static inline uint64_t parse_num(const unsigned char* p, size_t len) {
    if (len <= 8) {
        uint64_t w0;
        memcpy(&w0, p, 8);
        return parse_8(w0 << (8 * (8 - len)));
    }
    uint64_t w0, w1;
    memcpy(&w0, p, 8);
    memcpy(&w1, p + 8, 8);
    size_t h = len - 8;
    uint64_t chunk = (w0 >> (8 * h)) | (w1 << (8 * (8 - h)));
    uint64_t d0 = (w0 & 0xff) - '0';
    uint64_t d1 = ((w0 >> 8) & 0xff) - '0';
    uint64_t h2 = (h == 2);
    uint64_t high = d0 * (1 + 9 * h2) + h2 * d1;
    return high * 100000000ULL + parse_8(chunk);
}

static inline uint64_t scalar_tail(const unsigned char* p, const unsigned char* end, uint64_t sum) {
    uint64_t v = 0;
    for (; p < end; ++p) {
        unsigned d = (unsigned)*p - '0';
        if (d <= 9) v = v * 10 + d;
        else { sum += v; v = 0; }
    }
    return sum + v;
}

#if (defined(__AVX2__) || defined(BLOCK_SCALAR_SIM))
#ifdef BLOCK_SCALAR_SIM
static inline uint32_t nl_mask32(const unsigned char* p) {
    uint32_t m = 0;
    for (int i = 0; i < 32; ++i) if (p[i] == '\n') m |= (uint32_t)1 << i;
    return m;
}
#else
static inline uint32_t nl_mask32(const unsigned char* p) {
    __m256i v = _mm256_loadu_si256((const __m256i*)p);
    return (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n')));
}
#endif

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0;

    if (size < 80) return scalar_tail(p, end, sum);

    // p advances by exactly 32 bytes per outer iteration (stride-32).
    // base is an OUTER variable persisting across iterations: it points to the
    // start of the current (possibly incomplete) number.
    const unsigned char* base = data;
    const unsigned char* safe_end = end - 64;   // 32B load + 16B over-read slack

    while (p < safe_end) {
        uint32_t m = nl_mask32(p);

        // For valid ≤10-digit input there is always ≥1 newline in 32 bytes.
        // popcount tells us exactly how many numbers end in this window.
        int cnt = __builtin_popcount(m);

        if (__builtin_expect(cnt == 3, 1)) {
            // Fast path: 3 newlines — extract positions without a loop
            unsigned n0 = __builtin_ctz(m); m &= m - 1;
            unsigned n1 = __builtin_ctz(m); m &= m - 1;
            unsigned n2 = __builtin_ctz(m);
            const unsigned char* nl0 = p + n0;
            const unsigned char* nl1 = p + n1;
            const unsigned char* nl2 = p + n2;
            sum += parse_num(base,    (size_t)(nl0 - base));
            sum += parse_num(nl0 + 1, (size_t)(nl1 - nl0 - 1));
            sum += parse_num(nl1 + 1, (size_t)(nl2 - nl1 - 1));
            base = nl2 + 1;
        } else if (__builtin_expect(cnt == 2, 1)) {
            unsigned n0 = __builtin_ctz(m); m &= m - 1;
            unsigned n1 = __builtin_ctz(m);
            const unsigned char* nl0 = p + n0;
            const unsigned char* nl1 = p + n1;
            sum += parse_num(base,    (size_t)(nl0 - base));
            sum += parse_num(nl0 + 1, (size_t)(nl1 - nl0 - 1));
            base = nl1 + 1;
        } else {
            // Rare: 1, 4, or more newlines (very short or very long numbers)
            while (m) {
                unsigned nlpos = __builtin_ctz(m);
                const unsigned char* nlp = p + nlpos;
                size_t len = (size_t)(nlp - base);
                if (len) sum += parse_num(base, len);
                base = nlp + 1;
                m &= m - 1;
            }
        }
        p += 32;   // fixed stride — always advance 32 bytes
    }
    return scalar_tail(base, end, sum);
}
#else
// Scalar fallback == champion v5 (SWAR newline-find + branchless parse).
static inline unsigned nl_pos8(uint64_t w) {
    const uint64_t NLW = 0x0a0a0a0a0a0a0a0aULL;
    uint64_t x = w ^ NLW;
    uint64_t m = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
    return m ? (unsigned)(__builtin_ctzll(m) >> 3) : 8u;
}
static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    const unsigned char* safe_end = (size >= 32) ? end - 32 : data;
    uint64_t sum = 0;
    while (p < safe_end) {
        uint64_t w0; memcpy(&w0, p, 8);
        unsigned idx = nl_pos8(w0);
        if (idx < 8) { sum += parse_num(p, idx); p += idx + 1; continue; }
        uint64_t w1; memcpy(&w1, p + 8, 8);
        unsigned idx2 = nl_pos8(w1);
        if (idx2 == 8) break;
        size_t len = 8 + idx2;
        sum += parse_num(p, len);
        p += len + 1;
    }
    return scalar_tail(p, end, sum);
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
