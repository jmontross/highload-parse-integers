// HighLoad.fun — parse_integers  (VARIANT: avx2_fastload)
// avx2_64b but with a reformed parse_num that ELIMINATES the two variable-count
// shifts (w0>>(8*h), w1<<(8*(8-h))) in the 9/10-digit hot path.
//
// Key insight: for a len-digit number at p, the last 8 digits live at bytes
// p[len-8..len-1].  Loading directly from p+(len-8) gives the same 8-digit tail
// chunk as the (w0>>shift | w1<<shift) expression, but without ANY shift — just
// one unaligned 8-byte load.  Variable-count shifts use only port 0 on x86 with
// 1-cycle throughput; 6 independent parse_nums fight for 12 variable-shift slots
// on port 0, costing ~12 cycles.  The direct load is 4-cycle L1D latency (no
// port 0 pressure) and can all issue in 3 cycles (2 load ports).
//
// High-digit path uses byte loads (p[0], p[1]) — again no 64-bit loads/shifts.
//
// The avx2_parse6 explicit-parallel fast path for cnt==6 is also applied:
// compute all 6 (start, len) pairs before any parse_num call so the compiler
// sees all 6 as truly independent (no false dependency through 'base').
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

// Eliminates variable shifts: load last-8 digits directly from p+(len-8).
// For len>=9: p[len-8..len-1] are exactly the last 8 digit bytes (newline at
// p[len] is outside the load window).  No variable shift, no w1 load.
static inline uint64_t parse_num(const unsigned char* p, size_t len) {
    if (__builtin_expect(len <= 8, 0)) {
        uint64_t w0;
        memcpy(&w0, p, 8);
        return parse_8(w0 << (8 * (8 - len)));
    }
    uint64_t chunk;
    memcpy(&chunk, p + len - 8, 8);   // direct load, no variable shift
    uint64_t d0 = (uint64_t)(unsigned char)p[0] - '0';
    uint64_t is10 = (len == 10);
    uint64_t d1 = (uint64_t)(unsigned char)p[1] - '0';
    uint64_t high = d0 * (1 + 9 * is10) + is10 * d1;
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
    const unsigned char* safe_end = end - 96;

    while (p < safe_end) {
        uint64_t m = nl_mask64(p);
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
            // Compute ALL start/length pairs before any parse_num — makes all 6
            // calls visibly independent to the compiler/OOO engine.
            const unsigned char *s0=base, *s1=nl0+1, *s2=nl1+1, *s3=nl2+1, *s4=nl3+1, *s5=nl4+1;
            size_t l0=(size_t)(nl0-base), l1=(size_t)(nl1-nl0-1), l2=(size_t)(nl2-nl1-1);
            size_t l3=(size_t)(nl3-nl2-1), l4=(size_t)(nl4-nl3-1), l5=(size_t)(nl5-nl4-1);
            uint64_t v0=parse_num(s0,l0), v1=parse_num(s1,l1), v2=parse_num(s2,l2);
            uint64_t v3=parse_num(s3,l3), v4=parse_num(s4,l4), v5=parse_num(s5,l5);
            sum += v0+v1+v2+v3+v4+v5;
            base = nl5+1;
        } else if (__builtin_expect(cnt == 5, 1)) {
            uint64_t mm = m;
            unsigned n0 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n1 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n2 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n3 = __builtin_ctzll(mm); mm &= mm-1;
            unsigned n4 = __builtin_ctzll(mm);
            const unsigned char *nl0=p+n0, *nl1=p+n1, *nl2=p+n2, *nl3=p+n3, *nl4=p+n4;
            const unsigned char *s0=base, *s1=nl0+1, *s2=nl1+1, *s3=nl2+1, *s4=nl3+1;
            size_t l0=(size_t)(nl0-base), l1=(size_t)(nl1-nl0-1), l2=(size_t)(nl2-nl1-1);
            size_t l3=(size_t)(nl3-nl2-1), l4=(size_t)(nl4-nl3-1);
            uint64_t v0=parse_num(s0,l0), v1=parse_num(s1,l1), v2=parse_num(s2,l2);
            uint64_t v3=parse_num(s3,l3), v4=parse_num(s4,l4);
            sum += v0+v1+v2+v3+v4;
            base = nl4+1;
        } else {
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
