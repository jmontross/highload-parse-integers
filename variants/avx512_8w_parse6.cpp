// HighLoad.fun — parse_integers  (VARIANT: avx512_8w_parse6)
// Champion avx2_8w_pf3_interleaved with TWO changes:
// 1. nl_mask64: 1 AVX-512 instruction instead of 5 AVX2 µops.
// 2. parse_oct (512-bit): processes cnt=6,7,8 in one 512-bit SIMD pipeline
//    instead of parse_quad(4)+parse_pair(2). For cnt<8, pad with zero-dummy lanes.
//
// The zero-dummy technique: pass a static 8-byte array of '0' chars with len=8.
// After sub('0') the dummy lane is all-zeros → contributes 0 to the sum.
// High-digit for dummy: len==8 → is9=0, is10=0, high=0. Zero contribution. ✓
//
// Expected win: AVX-512 scan saves 40 µops/512B iteration (56→16).
//               parse_oct replaces parse_quad+parse_pair (2 pipelines → 1 pipeline)
//               for cnt=6 (~64% of windows).
//
// This requires __AVX512BW__ (enabled by -march=native on Sapphire Rapids).
// Falls back to full AVX2 champion code if __AVX512BW__ is not available.
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
    uint64_t a0=(uint64_t)(unsigned char)p_a[0]-'0', b0=(uint64_t)(unsigned char)p_b[0]-'0';
    uint64_t a_is9=(len_a==9), a_is10=(len_a==10), b_is9=(len_b==9), b_is10=(len_b==10);
    uint64_t a1=(uint64_t)(unsigned char)p_a[1]-'0', b1=(uint64_t)(unsigned char)p_b[1]-'0';
    uint64_t ah=a0*a_is9+(a0*10+a1)*a_is10, bh=b0*b_is9+(b0*10+b1)*b_is10;
    return (ah*100000000ULL+a8)+(bh*100000000ULL+b8);
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
    uint32_t a8=(uint32_t)_mm_cvtsi128_si32(lo4), b8=(uint32_t)_mm_extract_epi32(lo4,2);
    uint32_t c8=(uint32_t)_mm_cvtsi128_si32(hi4), d8=(uint32_t)_mm_extract_epi32(hi4,2);
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

#ifdef __AVX512BW__
// parse_oct: 8 numbers at once via 512-bit MADDUBS/MADD/MULLO.
// For cnt<8, pass dummy lanes with pointer=DIGIT_ZEROS and len=8:
//   DIGIT_ZEROS is all '0' bytes → sub→0 → parse_value=0 → contributes 0 to sum.
static const unsigned char DIGIT_ZEROS[8] = {'0','0','0','0','0','0','0','0'};

static inline uint64_t parse_oct(
    const unsigned char* __restrict__ pa, size_t la,
    const unsigned char* __restrict__ pb, size_t lb,
    const unsigned char* __restrict__ pc, size_t lc,
    const unsigned char* __restrict__ pd, size_t ld,
    const unsigned char* __restrict__ pe, size_t le,
    const unsigned char* __restrict__ pf, size_t lf,
    const unsigned char* __restrict__ pg, size_t lg,
    const unsigned char* __restrict__ ph, size_t lh)
{
    if (__builtin_expect(
        la<8||lb<8||lc<8||ld<8||le<8||lf<8||lg<8||lh<8, 0))
        return parse_quad(pa,la,pb,lb,pc,lc,pd,ld)
             + parse_quad(pe,le,pf,lf,pg,lg,ph,lh);

    __m128i ca = _mm_loadl_epi64((__m128i const*)(pa + la - 8));
    __m128i cb = _mm_loadl_epi64((__m128i const*)(pb + lb - 8));
    __m128i lo_ab = _mm_unpacklo_epi64(ca, cb);
    __m128i cc = _mm_loadl_epi64((__m128i const*)(pc + lc - 8));
    __m128i cd = _mm_loadl_epi64((__m128i const*)(pd + ld - 8));
    __m128i lo_cd = _mm_unpacklo_epi64(cc, cd);
    __m128i ce = _mm_loadl_epi64((__m128i const*)(pe + le - 8));
    __m128i cf = _mm_loadl_epi64((__m128i const*)(pf + lf - 8));
    __m128i lo_ef = _mm_unpacklo_epi64(ce, cf);
    __m128i cg = _mm_loadl_epi64((__m128i const*)(pg + lg - 8));
    __m128i ch = _mm_loadl_epi64((__m128i const*)(ph + lh - 8));
    __m128i lo_gh = _mm_unpacklo_epi64(cg, ch);

    __m256i lo256_abcd = _mm256_set_m128i(lo_cd, lo_ab);
    __m256i lo256_efgh = _mm256_set_m128i(lo_gh, lo_ef);
    __m512i chunks = _mm512_inserti64x4(
        _mm512_castsi256_si512(lo256_abcd), lo256_efgh, 1);

    chunks = _mm512_sub_epi8(chunks, _mm512_set1_epi8('0'));

    const __m512i W1 = _mm512_set1_epi16((int16_t)0x010a);
    __m512i l1 = _mm512_maddubs_epi16(chunks, W1);

    const __m512i W2 = _mm512_set1_epi32(0x00010064);
    __m512i l2 = _mm512_madd_epi16(l1, W2);

    const __m512i W3 = _mm512_set_epi32(
        1,10000, 1,10000, 1,10000, 1,10000,
        1,10000, 1,10000, 1,10000, 1,10000);
    __m512i l3 = _mm512_mullo_epi32(l2, W3);

    __m512i l3s = _mm512_shuffle_epi32(l3, (_MM_PERM_ENUM)0xB1);
    __m512i l4  = _mm512_add_epi32(l3, l3s);

    __m128i lane0 = _mm512_castsi512_si128(l4);
    __m128i lane1 = _mm512_extracti32x4_epi32(l4, 1);
    __m128i lane2 = _mm512_extracti32x4_epi32(l4, 2);
    __m128i lane3 = _mm512_extracti32x4_epi32(l4, 3);
    uint32_t a8=(uint32_t)_mm_cvtsi128_si32(lane0), b8=(uint32_t)_mm_extract_epi32(lane0,2);
    uint32_t c8=(uint32_t)_mm_cvtsi128_si32(lane1), d8=(uint32_t)_mm_extract_epi32(lane1,2);
    uint32_t e8=(uint32_t)_mm_cvtsi128_si32(lane2), f8=(uint32_t)_mm_extract_epi32(lane2,2);
    uint32_t g8=(uint32_t)_mm_cvtsi128_si32(lane3), h8=(uint32_t)_mm_extract_epi32(lane3,2);

    uint64_t xa0=(uint64_t)(unsigned char)pa[0]-'0', a_is9=(la==9), a_is10=(la==10);
    uint64_t xb0=(uint64_t)(unsigned char)pb[0]-'0', b_is9=(lb==9), b_is10=(lb==10);
    uint64_t xc0=(uint64_t)(unsigned char)pc[0]-'0', c_is9=(lc==9), c_is10=(lc==10);
    uint64_t xd0=(uint64_t)(unsigned char)pd[0]-'0', d_is9=(ld==9), d_is10=(ld==10);
    uint64_t xe0=(uint64_t)(unsigned char)pe[0]-'0', e_is9=(le==9), e_is10=(le==10);
    uint64_t xf0=(uint64_t)(unsigned char)pf[0]-'0', f_is9=(lf==9), f_is10=(lf==10);
    uint64_t xg0=(uint64_t)(unsigned char)pg[0]-'0', g_is9=(lg==9), g_is10=(lg==10);
    uint64_t xh0=(uint64_t)(unsigned char)ph[0]-'0', h_is9=(lh==9), h_is10=(lh==10);
    uint64_t xa1=(uint64_t)(unsigned char)pa[1]-'0', xb1=(uint64_t)(unsigned char)pb[1]-'0';
    uint64_t xc1=(uint64_t)(unsigned char)pc[1]-'0', xd1=(uint64_t)(unsigned char)pd[1]-'0';
    uint64_t xe1=(uint64_t)(unsigned char)pe[1]-'0', xf1=(uint64_t)(unsigned char)pf[1]-'0';
    uint64_t xg1=(uint64_t)(unsigned char)pg[1]-'0', xh1=(uint64_t)(unsigned char)ph[1]-'0';
    uint64_t ah = xa0*a_is9 + (xa0*10+xa1)*a_is10;
    uint64_t bh = xb0*b_is9 + (xb0*10+xb1)*b_is10;
    uint64_t rch = xc0*c_is9 + (xc0*10+xc1)*c_is10;
    uint64_t dh = xd0*d_is9 + (xd0*10+xd1)*d_is10;
    uint64_t eh = xe0*e_is9 + (xe0*10+xe1)*e_is10;
    uint64_t fh = xf0*f_is9 + (xf0*10+xf1)*f_is10;
    uint64_t gh = xg0*g_is9 + (xg0*10+xg1)*g_is10;
    uint64_t hh = xh0*h_is9 + (xh0*10+xh1)*h_is10;

    return (ah*100000000ULL+a8) + (bh*100000000ULL+b8)
         + (rch*100000000ULL+c8) + (dh*100000000ULL+d8)
         + (eh*100000000ULL+e8) + (fh*100000000ULL+f8)
         + (gh*100000000ULL+g8) + (hh*100000000ULL+h8);
}

static inline uint64_t process_window(
    const unsigned char* __restrict__ p,
    const unsigned char* __restrict__ & base,
    uint64_t m)
{
    uint64_t sum = 0;
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
        // parse_oct with 2 zero-dummy lanes (contribute 0 each)
        sum = parse_oct(base,  nl0-base,  nl0+1, nl1-nl0-1,
                        nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1,
                        nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                        DIGIT_ZEROS, 8, DIGIT_ZEROS, 8);
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
        // parse_quad + parse_num (unchanged — fewer dummies = more overhead for cnt=5)
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                         nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
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
        // parse_oct with 1 zero-dummy lane
        sum = parse_oct(base,  nl0-base,  nl0+1, nl1-nl0-1,
                        nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1,
                        nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                        nl5+1, nl6-nl5-1,
                        DIGIT_ZEROS, 8);
        base = nl6+1;
    } else if (__builtin_expect(cnt == 4, 0)) {
        uint64_t mm = m;
        unsigned n0,n1,n2,n3;
        n0=__builtin_ctzll(mm); mm&=mm-1;
        n1=__builtin_ctzll(mm); mm&=mm-1;
        n2=__builtin_ctzll(mm); mm&=mm-1;
        n3=__builtin_ctzll(mm);
        const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
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
        // parse_oct for all 8: saves 1 full SIMD pipeline vs 2×parse_quad
        sum = parse_oct(base,  nl0-base,  nl0+1, nl1-nl0-1,
                        nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1,
                        nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
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
        // parse_oct(8) + parse_num(1): saves 1 parse_quad vs 2×parse_quad+parse_num
        sum = parse_oct(base,  nl0-base,  nl0+1, nl1-nl0-1,
                        nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1,
                        nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                        nl5+1, nl6-nl5-1, nl6+1, nl7-nl6-1)
            + parse_num(nl7+1, nl8-nl7-1);
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
        sum = parse_oct(base,  nl0-base,  nl0+1, nl1-nl0-1,
                        nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1,
                        nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                        nl5+1, nl6-nl5-1, nl6+1, nl7-nl6-1)
            + parse_pair(nl7+1, nl8-nl7-1, nl8+1, nl9-nl8-1);
        base = nl9+1;
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
    return sum;
}

#else // no __AVX512BW__: fall back to AVX2 champion process_window
static inline uint64_t process_window(
    const unsigned char* __restrict__ p,
    const unsigned char* __restrict__ & base,
    uint64_t m)
{
    uint64_t sum = 0;
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
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                         nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
            + parse_pair(nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1);
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
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                         nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
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
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
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
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
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
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
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
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
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
        sum = parse_quad(base,  nl0-base,  nl0+1, nl1-nl0-1,
                         nl1+1, nl2-nl1-1, nl2+1, nl3-nl2-1)
            + parse_quad(nl3+1, nl4-nl3-1, nl4+1, nl5-nl4-1,
                         nl5+1, nl6-nl5-1, nl6+1, nl7-nl6-1)
            + parse_pair(nl7+1, nl8-nl7-1, nl8+1, nl9-nl8-1);
        base = nl9+1;
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
    return sum;
}
#endif // __AVX512BW__

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

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0;
    if (size < 800) return scalar_tail(p, end, sum);
    const unsigned char* base = data;
    const unsigned char* safe_end = end - 544;

    while (p < safe_end) {
        _mm_prefetch((const char*)(p + 1536),      _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 64), _MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 128),_MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 192),_MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 256),_MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 320),_MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 384),_MM_HINT_T1);
        _mm_prefetch((const char*)(p + 1536 + 448),_MM_HINT_T1);

        uint64_t m0 = nl_mask64(p);
        uint64_t m1 = nl_mask64(p + 64);
        sum += process_window(p,       base, m0);
        uint64_t m2 = nl_mask64(p + 128);
        sum += process_window(p + 64,  base, m1);
        uint64_t m3 = nl_mask64(p + 192);
        sum += process_window(p + 128, base, m2);
        uint64_t m4 = nl_mask64(p + 256);
        sum += process_window(p + 192, base, m3);
        uint64_t m5 = nl_mask64(p + 320);
        sum += process_window(p + 256, base, m4);
        uint64_t m6 = nl_mask64(p + 384);
        sum += process_window(p + 320, base, m5);
        uint64_t m7 = nl_mask64(p + 448);
        sum += process_window(p + 384, base, m6);
        sum += process_window(p + 448, base, m7);
        p += 512;
    }
    while (p + 96 < end) {
        uint64_t m = nl_mask64(p);
        sum += process_window(p, base, m);
        p += 64;
    }
    return scalar_tail(base, end, sum);
}

#else // no AVX2
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
#endif // AVX2

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
