// HighLoad.fun — parse_integers  (VARIANT: avx2_cnt12)
// Extends avx2_cnt8910 with explicit fast paths for cnt==11 and cnt==12.
//
// MOTIVATION:
//   avx2_cnt8910 handles cnt==8,9,10 with 2×parse_quad. The while(m) fallback
//   still triggers for cnt==11,12 (short numbers: avg 5-6 digits per number).
//   This is rare but eliminates the last serial extraction path:
//     cnt=11 → 2×parse_quad + parse_pair + parse_num
//     cnt=12 → 3×parse_quad
//   With 64-byte windows and 6-byte average lines (5-digit numbers), cnt==11-12
//   appears for the minority of 4-6 digit numbers in the dataset (~1-2% of windows).
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

    uint64_t a0 = (uint64_t)(unsigned char)p_a[0] - '0';
    uint64_t b0 = (uint64_t)(unsigned char)p_b[0] - '0';
    uint64_t a_is9=(len_a==9), a_is10=(len_a==10);
    uint64_t b_is9=(len_b==9), b_is10=(len_b==10);
    uint64_t a1 = (uint64_t)(unsigned char)p_a[1] - '0';
    uint64_t b1 = (uint64_t)(unsigned char)p_b[1] - '0';
    uint64_t a_high = a0*a_is9 + (a0*10+a1)*a_is10;
    uint64_t b_high = b0*b_is9 + (b0*10+b1)*b_is10;
    return (a_high * 100000000ULL + a8) + (b_high * 100000000ULL + b8);
}

static inline uint64_t parse_quad(
    const unsigned char* __restrict__ p_a, size_t len_a,
    const unsigned char* __restrict__ p_b, size_t len_b,
    const unsigned char* __restrict__ p_c, size_t len_c,
    const unsigned char* __restrict__ p_d, size_t len_d)
{
    if (__builtin_expect(len_a < 8 || len_b < 8 || len_c < 8 || len_d < 8, 0))
        return parse_pair(p_a,len_a,p_b,len_b) + parse_pair(p_c,len_c,p_d,len_d);

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

    uint64_t a0=(uint64_t)(unsigned char)p_a[0]-'0', b0=(uint64_t)(unsigned char)p_b[0]-'0';
    uint64_t c0=(uint64_t)(unsigned char)p_c[0]-'0', d0=(uint64_t)(unsigned char)p_d[0]-'0';
    uint64_t a_is9=(len_a==9), a_is10=(len_a==10), b_is9=(len_b==9), b_is10=(len_b==10);
    uint64_t c_is9=(len_c==9), c_is10=(len_c==10), d_is9=(len_d==9), d_is10=(len_d==10);
    uint64_t a1=(uint64_t)(unsigned char)p_a[1]-'0', b1=(uint64_t)(unsigned char)p_b[1]-'0';
    uint64_t c1=(uint64_t)(unsigned char)p_c[1]-'0', d1=(uint64_t)(unsigned char)p_d[1]-'0';
    uint64_t ah=a0*a_is9+(a0*10+a1)*a_is10, bh=b0*b_is9+(b0*10+b1)*b_is10;
    uint64_t ch=c0*c_is9+(c0*10+c1)*c_is10, dh=d0*d_is9+(d0*10+d1)*d_is10;
    return (ah*100000000ULL+a8)+(bh*100000000ULL+b8)
          +(ch*100000000ULL+c8)+(dh*100000000ULL+d8);
}

static inline uint64_t nl_mask64(const unsigned char* p) {
    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i nl = _mm256_set1_epi8('\n');
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
    return (uint64_t)m0 | ((uint64_t)m1 << 32);
}

// Helper macro to extract N newline positions from mask mm into n0..n(N-1)
#define EXTRACT2(mm, n0, n1) \
    n0=__builtin_ctzll(mm); mm&=mm-1; \
    n1=__builtin_ctzll(mm)
#define EXTRACT4(mm, n0, n1, n2, n3) \
    n0=__builtin_ctzll(mm); mm&=mm-1; \
    n1=__builtin_ctzll(mm); mm&=mm-1; \
    n2=__builtin_ctzll(mm); mm&=mm-1; \
    n3=__builtin_ctzll(mm)

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
            sum += parse_quad(base,    nl0-base,   nl0+1, nl1-nl0-1,
                              nl1+1,   nl2-nl1-1,  nl2+1, nl3-nl2-1)
                 + parse_pair(nl3+1,   nl4-nl3-1,  nl4+1, nl5-nl4-1);
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
            sum += parse_quad(base,  nl0-base,   nl0+1, nl1-nl0-1,
                              nl1+1, nl2-nl1-1,  nl2+1, nl3-nl2-1)
                 + parse_num(nl3+1,  nl4-nl3-1);
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
            sum += parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                              nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
                 + parse_pair(nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1)
                 + parse_num(nl5+1,  nl6-nl5-1);
            base = nl6+1;
        } else if (__builtin_expect(cnt == 4, 0)) {
            uint64_t mm = m;
            unsigned n0,n1,n2,n3;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
            sum += parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                              nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1);
            base = nl3+1;
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
            sum += parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                              nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
                 + parse_quad(nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                              nl5+1, nl6-nl5-1, nl6+1, nl7-nl6-1);
            base = nl7+1;
        } else if (__builtin_expect(cnt == 9, 0)) {
            uint64_t mm = m;
            unsigned n0,n1,n2,n3,n4,n5,n6,n7,n8;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm); mm&=mm-1;
            n4=__builtin_ctzll(mm); mm&=mm-1;
            n5=__builtin_ctzll(mm); mm&=mm-1;
            n6=__builtin_ctzll(mm); mm&=mm-1;
            n7=__builtin_ctzll(mm); mm&=mm-1;
            n8=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3,
                                *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7,*nl8=p+n8;
            sum += parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                              nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
                 + parse_quad(nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                              nl5+1, nl6-nl5-1, nl6+1, nl7-nl6-1)
                 + parse_num(nl7+1,  nl8-nl7-1);
            base = nl8+1;
        } else if (__builtin_expect(cnt == 10, 0)) {
            uint64_t mm = m;
            unsigned n0,n1,n2,n3,n4,n5,n6,n7,n8,n9;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm); mm&=mm-1;
            n4=__builtin_ctzll(mm); mm&=mm-1;
            n5=__builtin_ctzll(mm); mm&=mm-1;
            n6=__builtin_ctzll(mm); mm&=mm-1;
            n7=__builtin_ctzll(mm); mm&=mm-1;
            n8=__builtin_ctzll(mm); mm&=mm-1;
            n9=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3,
                                *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7,
                                *nl8=p+n8,*nl9=p+n9;
            sum += parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                              nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
                 + parse_quad(nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                              nl5+1, nl6-nl5-1, nl6+1, nl7-nl6-1)
                 + parse_pair(nl7+1, nl8-nl7-1, nl8+1, nl9-nl8-1);
            base = nl9+1;
        } else if (__builtin_expect(cnt == 11, 0)) {
            // 2×parse_quad + parse_pair + parse_num
            uint64_t mm = m;
            unsigned n0,n1,n2,n3,n4,n5,n6,n7,n8,n9,n10;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm); mm&=mm-1;
            n4=__builtin_ctzll(mm); mm&=mm-1;
            n5=__builtin_ctzll(mm); mm&=mm-1;
            n6=__builtin_ctzll(mm); mm&=mm-1;
            n7=__builtin_ctzll(mm); mm&=mm-1;
            n8=__builtin_ctzll(mm); mm&=mm-1;
            n9=__builtin_ctzll(mm); mm&=mm-1;
            n10=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3,
                                *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7,
                                *nl8=p+n8,*nl9=p+n9,*nl10=p+n10;
            sum += parse_quad(base,   nl0-base,   nl0+1, nl1-nl0-1,
                              nl1+1,  nl2-nl1-1,  nl2+1, nl3-nl2-1)
                 + parse_quad(nl3+1,  nl4-nl3-1,  nl4+1, nl5-nl4-1,
                              nl5+1,  nl6-nl5-1,  nl6+1, nl7-nl6-1)
                 + parse_pair(nl7+1,  nl8-nl7-1,  nl8+1, nl9-nl8-1)
                 + parse_num(nl9+1,   nl10-nl9-1);
            base = nl10+1;
        } else if (__builtin_expect(cnt == 12, 0)) {
            // 3×parse_quad covers all 12 numbers
            uint64_t mm = m;
            unsigned n0,n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11;
            n0=__builtin_ctzll(mm); mm&=mm-1;
            n1=__builtin_ctzll(mm); mm&=mm-1;
            n2=__builtin_ctzll(mm); mm&=mm-1;
            n3=__builtin_ctzll(mm); mm&=mm-1;
            n4=__builtin_ctzll(mm); mm&=mm-1;
            n5=__builtin_ctzll(mm); mm&=mm-1;
            n6=__builtin_ctzll(mm); mm&=mm-1;
            n7=__builtin_ctzll(mm); mm&=mm-1;
            n8=__builtin_ctzll(mm); mm&=mm-1;
            n9=__builtin_ctzll(mm); mm&=mm-1;
            n10=__builtin_ctzll(mm); mm&=mm-1;
            n11=__builtin_ctzll(mm);
            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3,
                                *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7,
                                *nl8=p+n8,*nl9=p+n9,*nl10=p+n10,*nl11=p+n11;
            sum += parse_quad(base,   nl0-base,   nl0+1, nl1-nl0-1,
                              nl1+1,  nl2-nl1-1,  nl2+1, nl3-nl2-1)
                 + parse_quad(nl3+1,  nl4-nl3-1,  nl4+1, nl5-nl4-1,
                              nl5+1,  nl6-nl5-1,  nl6+1, nl7-nl6-1)
                 + parse_quad(nl7+1,  nl8-nl7-1,  nl8+1, nl9-nl8-1,
                              nl9+1,  nl10-nl9-1, nl10+1, nl11-nl10-1);
            base = nl11+1;
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
