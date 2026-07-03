// HighLoad.fun — parse_integers  (VARIANT: avx2_64b)
// Process 64 bytes per outer loop iteration using TWO AVX2 32-byte loads,
// combined into a 64-bit newline mask. Advance p by 64 bytes (fixed stride)
// and track 'base' as an outer variable. This cuts outer-loop iterations by
// ~2× vs avx2_blockparse (~8M vs ~15M), reducing nl_mask + popcount overhead.
// Uses the same count-unroll fast path from avx2_unroll for the inner
// processing: popcount to detect how many newlines are in the 64-byte window,
// then handle the common cases (5-6 newlines) without a loop.
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

// Inline: process one newline at position nlp; update base and sum.
#define PROC_NL(nlp_expr)                       \
    do {                                        \
        const unsigned char* _nlp = (nlp_expr); \
        sum += parse_num(base, (size_t)(_nlp - base)); \
        base = _nlp + 1;                        \
    } while (0)

#if (defined(__AVX2__) || defined(BLOCK_SCALAR_SIM))
#ifdef BLOCK_SCALAR_SIM
  #define BLK 64
  static inline uint64_t nl_mask64(const unsigned char* p) {
      uint64_t m = 0;
      for (int i = 0; i < BLK; ++i) if (p[i] == '\n') m |= (uint64_t)1 << i;
      return m;
  }
#else
  static inline uint64_t nl_mask64(const unsigned char* p) {
      __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
      __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
      __m256i nl = _mm256_set1_epi8('\n');
      uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
      uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
      return (uint64_t)m0 | ((uint64_t)m1 << 32);
  }
#endif

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0;

    if (size < 128) return scalar_tail(p, end, sum);

    const unsigned char* base = data;
    const unsigned char* safe_end = end - 96;   // 64B load + 32B over-read slack

    while (p < safe_end) {
        uint64_t m = nl_mask64(p);

        // Unroll for common case of 5 or 6 newlines per 64-byte block
        // (10-digit numbers: 64/11 ≈ 5.8; 9-digit: 64/10 = 6.4)
        int cnt = __builtin_popcountll(m);

        if (__builtin_expect(cnt == 6, 1)) {
            uint64_t mm = m;
            unsigned n0 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n1 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n2 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n3 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n4 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n5 = __builtin_ctzll(mm);
            const unsigned char *nl0=p+n0, *nl1=p+n1, *nl2=p+n2,
                                *nl3=p+n3, *nl4=p+n4, *nl5=p+n5;
            PROC_NL(nl0); PROC_NL(nl1); PROC_NL(nl2);
            PROC_NL(nl3); PROC_NL(nl4); PROC_NL(nl5);
        } else if (__builtin_expect(cnt == 5, 1)) {
            uint64_t mm = m;
            unsigned n0 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n1 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n2 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n3 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n4 = __builtin_ctzll(mm);
            const unsigned char *nl0=p+n0, *nl1=p+n1, *nl2=p+n2,
                                *nl3=p+n3, *nl4=p+n4;
            PROC_NL(nl0); PROC_NL(nl1); PROC_NL(nl2);
            PROC_NL(nl3); PROC_NL(nl4);
        } else {
            // Fallback loop for rare counts (4, 7, etc.)
            while (m) {
                unsigned nlpos = __builtin_ctzll(m);
                const unsigned char* nlp = p + nlpos;
                size_t len = (size_t)(nlp - base);
                if (len) sum += parse_num(base, len);
                base = nlp + 1;
                m &= m - 1;
            }
        }
        p += 64;
    }
    return scalar_tail(base, end, sum);
}
#else
// Scalar fallback == champion v5.
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
