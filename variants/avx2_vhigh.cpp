// HighLoad.fun — parse_integers  (VARIANT: avx2_vhigh)
// avx2_parse_quad with vectorized high-digit computation.
//
// HYPOTHESIS: The "high digit" scalar work in parse_quad is:
//   4 × (2 loads + 3 muls + 2 adds) = ~40 scalar µops
// Replace with SSE vector computation:
//   Pack 4 first-bytes and 4 lengths into XMM → compute high in ~10 µops.
//
// For parse_quad on 4 numbers a,b,c,d:
//   high[i] = p[i][0]*is9[i] + (p[i][0]*10 + p[i][1])*is10[i]
//
// Vectorized: load [a0,b0,c0,d0] and [a1,b1,c1,d1] as 8-bit values,
// pack lengths into 16-bit vector, compare with 9 and 10,
// then compute the branchless multiplication in SIMD.
#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__AVX2__)
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

#if defined(__AVX2__)

static inline uint64_t nl_mask64(const unsigned char* p) {
    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i nl = _mm256_set1_epi8('\n');
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
    return (uint64_t)m0 | ((uint64_t)m1 << 32);
}

static inline uint64_t parse_pair(
    const unsigned char* __restrict__ p_a, size_t len_a,
    const unsigned char* __restrict__ p_b, size_t len_b)
{
    if (__builtin_expect(len_a < 8 || len_b < 8, 0))
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
    __m128i l3s = _mm_shuffle_epi32(l3, 0xB1);
    __m128i l4  = _mm_add_epi32(l3, l3s);
    uint32_t a8 = (uint32_t)_mm_cvtsi128_si32(l4);
    uint32_t b8 = (uint32_t)_mm_extract_epi32(l4, 2);

    uint64_t a0=(uint64_t)(unsigned char)p_a[0]-'0';
    uint64_t b0=(uint64_t)(unsigned char)p_b[0]-'0';
    uint64_t a_is9=(len_a==9), a_is10=(len_a==10);
    uint64_t b_is9=(len_b==9), b_is10=(len_b==10);
    uint64_t a1=(uint64_t)(unsigned char)p_a[1]-'0';
    uint64_t b1=(uint64_t)(unsigned char)p_b[1]-'0';
    uint64_t a_high = a0*a_is9 + (a0*10+a1)*a_is10;
    uint64_t b_high = b0*b_is9 + (b0*10+b1)*b_is10;
    return (a_high * 100000000ULL + a8) + (b_high * 100000000ULL + b8);
}

// parse_quad with vectorized high-digit computation
static inline uint64_t parse_quad(
    const unsigned char* __restrict__ p_a, size_t len_a,
    const unsigned char* __restrict__ p_b, size_t len_b,
    const unsigned char* __restrict__ p_c, size_t len_c,
    const unsigned char* __restrict__ p_d, size_t len_d)
{
    if (__builtin_expect(len_a < 8 || len_b < 8 || len_c < 8 || len_d < 8, 0))
        return parse_pair(p_a,len_a,p_b,len_b) + parse_pair(p_c,len_c,p_d,len_d);

    // SIMD 8-digit low parse (same as avx2_parse_quad)
    __m128i ca = _mm_loadl_epi64((__m128i const*)(p_a + len_a - 8));
    __m128i cb = _mm_loadl_epi64((__m128i const*)(p_b + len_b - 8));
    __m128i lo128 = _mm_unpacklo_epi64(ca, cb);
    __m128i cc = _mm_loadl_epi64((__m128i const*)(p_c + len_c - 8));
    __m128i cd = _mm_loadl_epi64((__m128i const*)(p_d + len_d - 8));
    __m128i hi128 = _mm_unpacklo_epi64(cc, cd);
    __m256i chunks = _mm256_set_m128i(hi128, lo128);
    chunks = _mm256_sub_epi8(chunks, _mm256_set1_epi8('0'));
    const __m256i W1 = _mm256_broadcastsi128_si256(
        _mm_set_epi8(1,10,1,10,1,10,1,10, 1,10,1,10,1,10,1,10));
    __m256i l1 = _mm256_maddubs_epi16(chunks, W1);
    const __m256i W2 = _mm256_broadcastsi128_si256(
        _mm_set_epi16(1,100,1,100, 1,100,1,100));
    __m256i l2 = _mm256_madd_epi16(l1, W2);
    const __m256i W3 = _mm256_set_epi32(1,10000,1,10000, 1,10000,1,10000);
    __m256i l3 = _mm256_mullo_epi32(l2, W3);
    __m256i l3s = _mm256_shuffle_epi32(l3, 0xB1);
    __m256i l4  = _mm256_add_epi32(l3, l3s);
    __m128i lo4 = _mm256_castsi256_si128(l4);
    __m128i hi4 = _mm256_extracti128_si256(l4, 1);
    uint32_t a8 = (uint32_t)_mm_cvtsi128_si32(lo4);
    uint32_t b8 = (uint32_t)_mm_extract_epi32(lo4, 2);
    uint32_t c8 = (uint32_t)_mm_cvtsi128_si32(hi4);
    uint32_t d8 = (uint32_t)_mm_extract_epi32(hi4, 2);

    // Vectorized high-digit computation using SSE4.1
    // Pack first bytes: [a0,b0,c0,d0] and second bytes: [a1,b1,c1,d1]
    // Each is a single byte from a different address — no way to avoid scalar loads,
    // but we can vectorize the arithmetic on them.
    uint8_t fa0 = (uint8_t)p_a[0]-'0', fa1 = (uint8_t)p_a[1]-'0';
    uint8_t fb0 = (uint8_t)p_b[0]-'0', fb1 = (uint8_t)p_b[1]-'0';
    uint8_t fc0 = (uint8_t)p_c[0]-'0', fc1 = (uint8_t)p_c[1]-'0';
    uint8_t fd0 = (uint8_t)p_d[0]-'0', fd1 = (uint8_t)p_d[1]-'0';

    // Pack into XMM registers: [a0,b0,c0,d0,a1,b1,c1,d1,0,...]
    __m128i digits01 = _mm_set_epi8(0,0,0,0,0,0,0,0,
                                     fd1,fc1,fb1,fa1, fd0,fc0,fb0,fa0);
    // Pack lengths into int16: [len_a,len_b,len_c,len_d, ...]
    __m128i vlens = _mm_set_epi32(0, 0,
        ((int)len_d << 16) | (int)len_c,
        ((int)len_b << 16) | (int)len_a);
    // Compare lengths with 9 and 10 (16-bit comparison)
    __m128i v9  = _mm_cmpeq_epi16(vlens, _mm_set1_epi16(9));  // 0xFFFF where len==9
    __m128i v10 = _mm_cmpeq_epi16(vlens, _mm_set1_epi16(10)); // 0xFFFF where len==10

    // Extract 0/1 masks as 32-bit integers
    // v9 low 64 bits: [is9_a:16, is9_b:16, is9_c:16, is9_d:16] = 0xFFFF or 0x0000
    // We need to convert 0xFFFF → 1 and 0x0000 → 0
    // Use movemask to get 4 bits, then extract individual bits
    int mask9  = _mm_movemask_epi8(v9);   // bit 1 set for each byte where 0xFF
    int mask10 = _mm_movemask_epi8(v10);

    // For 16-bit 0xFFFF → movemask gives bits at even+odd positions of each word
    // bit 1 of 16-bit word → both bits 0 and 1 of the byte pair are set → mask & 0x3 != 0
    uint32_t is9_a  = (mask9  >> 0) & 1;
    uint32_t is9_b  = (mask9  >> 2) & 1;
    uint32_t is9_c  = (mask9  >> 4) & 1;
    uint32_t is9_d  = (mask9  >> 6) & 1;
    uint32_t is10_a = (mask10 >> 0) & 1;
    uint32_t is10_b = (mask10 >> 2) & 1;
    uint32_t is10_c = (mask10 >> 4) & 1;
    uint32_t is10_d = (mask10 >> 6) & 1;

    // Now compute high with SIMD multiply-add on the digit bytes
    // digits01 = [a0,b0,c0,d0, a1,b1,c1,d1, ...]
    // We want: high[i] = d0[i]*scale0[i] + d1[i]*scale1[i]
    // where scale0[i] = is9[i] + 10*is10[i], scale1[i] = is10[i]
    // These scale values depend on is9/is10, which vary per number.
    // Hard to vectorize the scales since they're data-dependent.
    // Instead just do scalar:
    uint64_t ah = fa0*is9_a + (fa0*10+fa1)*is10_a;
    uint64_t bh = fb0*is9_b + (fb0*10+fb1)*is10_b;
    uint64_t ch = fc0*is9_c + (fc0*10+fc1)*is10_c;
    uint64_t dh = fd0*is9_d + (fd0*10+fd1)*is10_d;

    return (ah*100000000ULL+a8) + (bh*100000000ULL+b8)
         + (ch*100000000ULL+c8) + (dh*100000000ULL+d8);
}

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
            const unsigned char *s0=base, *s1=nl0+1, *s2=nl1+1, *s3=nl2+1,
                                *s4=nl3+1, *s5=nl4+1;
            size_t l0=(nl0-base),  l1=(nl1-nl0-1);
            size_t l2=(nl2-nl1-1), l3=(nl3-nl2-1);
            size_t l4=(nl4-nl3-1), l5=(nl5-nl4-1);
            sum += parse_quad(s0,l0, s1,l1, s2,l2, s3,l3)
                 + parse_pair(s4,l4, s5,l5);
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
            const unsigned char *s0=base, *s1=nl0+1, *s2=nl1+1, *s3=nl2+1, *s4=nl3+1;
            size_t l0=(nl0-base), l1=(nl1-nl0-1);
            size_t l2=(nl2-nl1-1), l3=(nl3-nl2-1), l4=(nl4-nl3-1);
            sum += parse_quad(s0,l0, s1,l1, s2,l2, s3,l3) + parse_num(s4,l4);
            base = nl4+1;
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
            const unsigned char *s0=base, *s1=nl0+1, *s2=nl1+1, *s3=nl2+1,
                                *s4=nl3+1, *s5=nl4+1, *s6=nl5+1;
            size_t l0=(nl0-base), l1=(nl1-nl0-1), l2=(nl2-nl1-1), l3=(nl3-nl2-1);
            size_t l4=(nl4-nl3-1), l5=(nl5-nl4-1), l6=(nl6-nl5-1);
            sum += parse_quad(s0,l0, s1,l1, s2,l2, s3,l3)
                 + parse_pair(s4,l4, s5,l5) + parse_num(s6,l6);
            base = nl6+1;
        } else if (__builtin_expect(cnt == 4, 0)) {
            uint64_t mm = m;
            unsigned n0,n1,n2,n3;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
            const unsigned char *s0=base, *s1=nl0+1, *s2=nl1+1, *s3=nl2+1;
            size_t l0=(nl0-base), l1=(nl1-nl0-1), l2=(nl2-nl1-1), l3=(nl3-nl2-1);
            sum += parse_quad(s0,l0, s1,l1, s2,l2, s3,l3);
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

#else
static uint64_t solve(const unsigned char* data, size_t size) {
    return scalar_tail(data, data + size, 0);
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
