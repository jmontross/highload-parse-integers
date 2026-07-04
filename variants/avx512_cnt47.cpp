// HighLoad.fun — parse_integers  (VARIANT: avx512_cnt47)
// avx512_maddubs (AVX-512 scan + shuffle+add parse) PLUS fast paths for
// cnt==4 and cnt==7 — the two most common cases that currently fall through
// to the slow while(m) loop in avx2_maddubs.
//
// WHY cnt==4 and cnt==7:
//   cnt==4: when a large number spans from the previous block, only 4 numbers
//           complete within this 64-byte block (~10-15% of blocks).
//   cnt==7: when a block contains a short (8-digit) number, 7 fit in 64 bytes;
//           ~4% of numbers are 8-digit → ~15% of blocks are cnt==7 (rough est).
//   Currently both go to `while (m) { parse_num; m &= m-1 }` which is serial
//   and slow (~7 cycles per number vs ~5 cycles for a parse_pair fast path).
//
// All three improvements combined: AVX-512 scan + nohadd + cnt==4/7 paths.
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
    if (__builtin_expect(len <= 8, 0)) {
        uint64_t w0;
        memcpy(&w0, p, 8);
        return parse_8(w0 << (8 * (8 - len)));
    }
    uint64_t chunk;
    memcpy(&chunk, p + len - 8, 8);
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

#if defined(__AVX2__) && !defined(BLOCK_SCALAR_SIM)

static inline uint64_t parse_pair(
    const unsigned char* __restrict__ p_a, size_t len_a,
    const unsigned char* __restrict__ p_b, size_t len_b)
{
    if (__builtin_expect(len_a <= 8 || len_b <= 8, 0))
        return parse_num(p_a, len_a) + parse_num(p_b, len_b);

    __m128i ca = _mm_loadl_epi64((__m128i const*)(p_a + len_a - 8));
    __m128i cb = _mm_loadl_epi64((__m128i const*)(p_b + len_b - 8));
    __m128i chunks = _mm_unpacklo_epi64(ca, cb);

    chunks = _mm_sub_epi8(chunks, _mm_set1_epi8('0'));

    const __m128i W1 = _mm_set_epi8(1,10,1,10,1,10,1,10, 1,10,1,10,1,10,1,10);
    __m128i l1 = _mm_maddubs_epi16(chunks, W1);

    const __m128i W2 = _mm_set_epi16(1,100,1,100, 1,100,1,100);
    __m128i l2 = _mm_madd_epi16(l1, W2);

    const __m128i W3 = _mm_set_epi32(1, 10000, 1, 10000);
    __m128i l3 = _mm_mullo_epi32(l2, W3);

    // shuffle+add replacing hadd (faster throughput on Skylake-like µarchs)
    __m128i l3_hi = _mm_shuffle_epi32(l3, 0x88);
    __m128i l3_lo = _mm_shuffle_epi32(l3, 0xdd);
    __m128i l4    = _mm_add_epi32(l3_hi, l3_lo);

    uint32_t a8 = (uint32_t)_mm_cvtsi128_si32(l4);
    uint32_t b8 = (uint32_t)_mm_extract_epi32(l4, 1);

    uint64_t a0 = (uint64_t)(unsigned char)p_a[0] - '0';
    uint64_t b0 = (uint64_t)(unsigned char)p_b[0] - '0';
    uint64_t a_is10 = (len_a == 10), b_is10 = (len_b == 10);
    uint64_t a1 = (uint64_t)(unsigned char)p_a[1] - '0';
    uint64_t b1 = (uint64_t)(unsigned char)p_b[1] - '0';
    uint64_t a_high = a0 * (1 + 9 * a_is10) + a_is10 * a1;
    uint64_t b_high = b0 * (1 + 9 * b_is10) + b_is10 * b1;

    return (a_high * 100000000ULL + a8) + (b_high * 100000000ULL + b8);
}

static inline uint64_t nl_mask64(const unsigned char* p) {
#ifdef __AVX512BW__
    return (uint64_t)_mm512_cmpeq_epi8_mask(
        _mm512_loadu_si512((const void*)p),
        _mm512_set1_epi8('\n'));
#else
    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i nl = _mm256_set1_epi8('\n');
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
    return (uint64_t)m0 | ((uint64_t)m1 << 32);
#endif
}

// Macro to extract k newline positions from mm into n0..n(k-1) and nl0..nl(k-1)
#define EXTRACT_NLS_4(mm, p, nl0,nl1,nl2,nl3, n0,n1,n2,n3) \
    do { uint64_t _m = (mm); \
         n0=__builtin_ctzll(_m); _m&=_m-1; \
         n1=__builtin_ctzll(_m); _m&=_m-1; \
         n2=__builtin_ctzll(_m); _m&=_m-1; \
         n3=__builtin_ctzll(_m); \
         nl0=(p)+n0; nl1=(p)+n1; nl2=(p)+n2; nl3=(p)+n3; } while(0)

#define EXTRACT_NLS_5(mm, p, nl0,nl1,nl2,nl3,nl4, n0,n1,n2,n3,n4) \
    do { uint64_t _m = (mm); \
         n0=__builtin_ctzll(_m); _m&=_m-1; \
         n1=__builtin_ctzll(_m); _m&=_m-1; \
         n2=__builtin_ctzll(_m); _m&=_m-1; \
         n3=__builtin_ctzll(_m); _m&=_m-1; \
         n4=__builtin_ctzll(_m); \
         nl0=(p)+n0; nl1=(p)+n1; nl2=(p)+n2; nl3=(p)+n3; nl4=(p)+n4; } while(0)

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
            unsigned n0,n1,n2,n3,n4,n5;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm); mm&=mm-1;
            n4=__builtin_ctzll(mm); mm&=mm-1;
            n5=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,
                                *nl3=p+n3,*nl4=p+n4,*nl5=p+n5;
            const unsigned char *s0=base,*s1=nl0+1,*s2=nl1+1,
                                *s3=nl2+1,*s4=nl3+1,*s5=nl4+1;
            size_t l0=(size_t)(nl0-base), l1=(size_t)(nl1-nl0-1);
            size_t l2=(size_t)(nl2-nl1-1),l3=(size_t)(nl3-nl2-1);
            size_t l4=(size_t)(nl4-nl3-1),l5=(size_t)(nl5-nl4-1);
            sum += parse_pair(s0,l0,s1,l1) + parse_pair(s2,l2,s3,l3) + parse_pair(s4,l4,s5,l5);
            base = nl5+1;
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
            const unsigned char *s0=base,*s1=nl0+1,*s2=nl1+1,
                                *s3=nl2+1,*s4=nl3+1;
            size_t l0=(size_t)(nl0-base), l1=(size_t)(nl1-nl0-1);
            size_t l2=(size_t)(nl2-nl1-1),l3=(size_t)(nl3-nl2-1);
            size_t l4=(size_t)(nl4-nl3-1);
            sum += parse_pair(s0,l0,s1,l1) + parse_pair(s2,l2,s3,l3) + parse_num(s4,l4);
            base = nl4+1;
        } else if (__builtin_expect(cnt == 7, 0)) {
            // 7 numbers: 3 pairs + 1 scalar — avoids the serial while(m) loop
            uint64_t mm = m;
            unsigned n0,n1,n2,n3,n4,n5,n6;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm); mm&=mm-1;
            n4=__builtin_ctzll(mm); mm&=mm-1;
            n5=__builtin_ctzll(mm); mm&=mm-1;
            n6=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,
                                *nl3=p+n3,*nl4=p+n4,*nl5=p+n5,*nl6=p+n6;
            const unsigned char *s0=base,*s1=nl0+1,*s2=nl1+1,
                                *s3=nl2+1,*s4=nl3+1,*s5=nl4+1,*s6=nl5+1;
            size_t l0=(size_t)(nl0-base), l1=(size_t)(nl1-nl0-1);
            size_t l2=(size_t)(nl2-nl1-1),l3=(size_t)(nl3-nl2-1);
            size_t l4=(size_t)(nl4-nl3-1),l5=(size_t)(nl5-nl4-1);
            size_t l6=(size_t)(nl6-nl5-1);
            sum += parse_pair(s0,l0,s1,l1) + parse_pair(s2,l2,s3,l3)
                 + parse_pair(s4,l4,s5,l5) + parse_num(s6,l6);
            base = nl6+1;
        } else if (__builtin_expect(cnt == 4, 0)) {
            // 4 numbers: 2 pairs — avoids the serial while(m) loop
            uint64_t mm = m;
            unsigned n0,n1,n2,n3;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
            const unsigned char *s0=base,*s1=nl0+1,*s2=nl1+1,*s3=nl2+1;
            size_t l0=(size_t)(nl0-base), l1=(size_t)(nl1-nl0-1);
            size_t l2=(size_t)(nl2-nl1-1),l3=(size_t)(nl3-nl2-1);
            sum += parse_pair(s0,l0,s1,l1) + parse_pair(s2,l2,s3,l3);
            base = nl3+1;
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

#elif defined(BLOCK_SCALAR_SIM)
static inline uint64_t nl_mask64_sim(const unsigned char* p) {
    uint64_t m = 0;
    for (int i = 0; i < 64; ++i) if (p[i] == '\n') m |= (uint64_t)1 << i;
    return m;
}
static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0;
    if (size < 128) return scalar_tail(p, end, sum);
    const unsigned char* base = data;
    const unsigned char* safe_end = end - 96;
    while (p < safe_end) {
        uint64_t m = nl_mask64_sim(p);
        while (m) {
            unsigned nlpos = __builtin_ctzll(m);
            const unsigned char* nlp = p + nlpos;
            size_t len = (size_t)(nlp - base);
            if (len) sum += parse_num(base, len);
            base = nlp + 1;
            m &= m - 1;
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
