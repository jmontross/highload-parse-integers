// HighLoad.fun — parse_integers  (VARIANT: AVX-512 block newline-mask parse)
// The widest lever. Same block algorithm as avx2_blockparse (find ALL newline
// positions in a block at once so per-number parses pipeline), but AVX-512BW
// compares 64 bytes per instruction and yields the mask NATIVELY as a __mmask64
// (no movemask round-trip) — ~6 numbers per load.
//
// TIERED so one submission auto-picks the best path the judge CPU offers, and
// so the tricky loop stays testable on ARM:
//   • __AVX512BW__ :          64-byte  _mm512_cmpeq_epi8_mask   (fast path)
//   • else __AVX2__ :         32-byte  vpmovmskb                (mid fallback,
//                             also avoids AVX-512 downclock on CPUs w/o AVX512BW)
//   • -DBLOCK_SCALAR_SIM :    identical loop, scalar 64-byte mask → validate
//                             the algorithm on ANY arch (ARM included)
//   • else :                  champion v5 (SWAR) scalar fallback
// The block loop is byte-width-agnostic (BLK ∈ {32,64}); each number is parsed
// with the verified v5 SWAR routine (parse_num).
#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

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

// Parse one number of length len (1..10) at p, branchless on the 9–10 digit path.
static inline uint64_t parse_num(const unsigned char* p, size_t len) {
    if (len <= 8) {
        uint64_t w0;
        memcpy(&w0, p, 8);
        return parse_8(w0 << (8 * (8 - len)));
    }
    uint64_t w0, w1;
    memcpy(&w0, p, 8);
    memcpy(&w1, p + 8, 8);
    size_t h = len - 8;                                 // 1 or 2
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

// nl_mask(p): bit i set iff p[i]=='\n', over BLK bytes.
#if defined(__AVX512BW__) && !defined(BLOCK_SCALAR_SIM)
  #include <immintrin.h>
  #define HAVE_BLOCK 1
  #define BLK 64
  static inline uint64_t nl_mask(const unsigned char* p) {
      return (uint64_t)_mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void*)p),
                                              _mm512_set1_epi8('\n'));
  }
#elif defined(__AVX2__) && !defined(BLOCK_SCALAR_SIM)
  #include <immintrin.h>
  #define HAVE_BLOCK 1
  #define BLK 32
  static inline uint64_t nl_mask(const unsigned char* p) {
      __m256i v = _mm256_loadu_si256((const __m256i*)p);
      return (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n')));
  }
#elif defined(BLOCK_SCALAR_SIM)
  #define HAVE_BLOCK 1
  #define BLK 64
  static inline uint64_t nl_mask(const unsigned char* p) {
      uint64_t m = 0;
      for (int i = 0; i < BLK; ++i) if (p[i] == '\n') m |= (uint64_t)1 << i;
      return m;
  }
#endif

#ifdef HAVE_BLOCK
static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0;
    if (size >= (size_t)BLK + 32) {
        const unsigned char* safe_end = end - (BLK + 32);   // BLK load + 16B parse over-read slack
        while (p < safe_end) {
            uint64_t m = nl_mask(p);
            if (!m) break;                             // no newline in BLK bytes (never for valid ≤10-digit input)
            const unsigned char* base = p;
            do {
                unsigned nlpos = (unsigned)__builtin_ctzll(m);
                const unsigned char* nlp = p + nlpos;
                size_t len = (size_t)(nlp - base);
                if (len) sum += parse_num(base, len);  // guard empty line (not in spec, but safe)
                base = nlp + 1;
                m &= m - 1;
            } while (m);
            p = base;
        }
    }
    return scalar_tail(p, end, sum);
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
        uint64_t w0;
        memcpy(&w0, p, 8);
        unsigned idx = nl_pos8(w0);
        if (idx < 8) { sum += parse_num(p, idx); p += idx + 1; continue; }
        uint64_t w1;
        memcpy(&w1, p + 8, 8);
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
