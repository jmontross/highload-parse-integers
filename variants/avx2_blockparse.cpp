// HighLoad.fun — parse_integers  (VARIANT: AVX2 block newline-mask parse)
// This is the FIRST SIMD variant — the lever the scalar/SWAR champions can't
// reach. It targets the one thing left after v5: the per-number boundary scan
// is SERIAL (number N+1 starts where number N's newline is), so newline-finding
// latency is on the critical path for all 50M numbers.
//
// AVX2 breaks that: one `vpcmpeqb` + `vpmovmskb` over 32 bytes yields a 32-bit
// mask of ALL newline positions in the block at once (~3 numbers per load), so
// every boundary in the block is known from a single instruction and the
// per-number parses can pipeline instead of waiting on the next scan. Each
// number is still parsed with the verified v5 SWAR routine (parse_num).
//
// Portable: gated behind #ifdef __AVX2__ with the v5 scalar loop as the #else
// fallback, so it builds AND runs everywhere (on ARM it's just v5; on the x86
// judge/routine box it uses AVX2). AVX-512 (64-byte, native k-mask) is the
// obvious next step once this is validated on x86.
#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __AVX2__
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

// Parse a single number of length len (1..10) at p, branchless on the common
// 9–10 digit path. Same arithmetic as champion v5. (len==0 must not be passed.)
static inline uint64_t parse_num(const unsigned char* p, size_t len) {
    if (len <= 8) {
        uint64_t w0;
        memcpy(&w0, p, 8);
        return parse_8(w0 << (8 * (8 - len)));
    }
    uint64_t w0, w1;
    memcpy(&w0, p, 8);
    memcpy(&w1, p + 8, 8);
    size_t h = len - 8;                                 // 1 or 2 here
    uint64_t chunk = (w0 >> (8 * h)) | (w1 << (8 * (8 - h)));
    uint64_t d0 = (w0 & 0xff) - '0';
    uint64_t d1 = ((w0 >> 8) & 0xff) - '0';
    uint64_t h2 = (h == 2);
    uint64_t high = d0 * (1 + 9 * h2) + h2 * d1;        // h==1→d0; h==2→d0*10+d1
    return high * 100000000ULL + parse_8(chunk);
}

// Scalar tail shared by both paths: byte-accumulate to the end. Handles the
// last partial block, no-trailing-newline, and tiny inputs correctly.
static inline uint64_t scalar_tail(const unsigned char* p, const unsigned char* end, uint64_t sum) {
    uint64_t v = 0;
    for (; p < end; ++p) {
        unsigned d = (unsigned)*p - '0';
        if (d <= 9) v = v * 10 + d;
        else { sum += v; v = 0; }
    }
    return sum + v;
}

#ifdef __AVX2__
static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0;
    const __m256i nl = _mm256_set1_epi8('\n');
    // Guard: load 32 at p, and parse_num over-reads up to 16 from a number start
    // that can sit near the block end → keep 64 bytes of slack.
    if (size >= 64) {
        const unsigned char* safe_end = end - 64;
        while (p < safe_end) {
            __m256i v = _mm256_loadu_si256((const __m256i*)p);
            uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, nl));
            if (!mask) break;                          // no newline in 32B (never for valid ≤10-digit input)
            const unsigned char* base = p;
            uint32_t m = mask;
            do {
                unsigned nlpos = (unsigned)__builtin_ctz(m);
                const unsigned char* nlp = p + nlpos;
                size_t len = (size_t)(nlp - base);
                if (len) sum += parse_num(base, len);  // guard empty line (not in spec, but safe)
                base = nlp + 1;
                m &= m - 1;
            } while (m);
            p = base;                                  // start of the next (possibly cross-block) number
        }
    }
    return scalar_tail(p, end, sum);
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
