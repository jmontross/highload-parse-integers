// HighLoad.fun — parse_integers  (VARIANT: avx512_parse_oct)
// 8-at-a-time parse using 512-bit MADDUBS/MADD/MULLO (parse_oct) + 128-byte window.
//
// MOTIVATION:
//   parse_quad processes 4 numbers in 256-bit AVX2.
//   parse_oct extends to 8 numbers in 512-bit AVX-512 — same instruction count,
//   2x throughput for the SIMD parse step.
//
//   This machine is Sapphire Rapids (avx512_vnni, amx_tile, avx512_fp16) which
//   has NO AVX-512 frequency downclocking (unlike Cascade Lake). Previous AVX-512
//   variants only used 512-bit for the newline scan, NOT for parsing.
//
//   128-byte window gives ~12-13 numbers per iteration.
//   Fast path cnt==12: 1×parse_oct(8) + 1×parse_quad(4) = 2 SIMD calls
//   vs current: 2 iterations × (parse_quad + parse_pair) = 4 SIMD calls
//   → 2x reduction in SIMD calls for the dominant case.
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

// parse_pair: 2 numbers at once via 128-bit
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

// parse_quad: 4 numbers at once via 256-bit AVX2
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

    uint64_t a0=(uint64_t)(unsigned char)p_a[0]-'0';
    uint64_t b0=(uint64_t)(unsigned char)p_b[0]-'0';
    uint64_t c0=(uint64_t)(unsigned char)p_c[0]-'0';
    uint64_t d0=(uint64_t)(unsigned char)p_d[0]-'0';
    uint64_t a_is9=(len_a==9), a_is10=(len_a==10);
    uint64_t b_is9=(len_b==9), b_is10=(len_b==10);
    uint64_t c_is9=(len_c==9), c_is10=(len_c==10);
    uint64_t d_is9=(len_d==9), d_is10=(len_d==10);
    uint64_t a1=(uint64_t)(unsigned char)p_a[1]-'0';
    uint64_t b1=(uint64_t)(unsigned char)p_b[1]-'0';
    uint64_t c1=(uint64_t)(unsigned char)p_c[1]-'0';
    uint64_t d1=(uint64_t)(unsigned char)p_d[1]-'0';
    uint64_t ah = a0*a_is9 + (a0*10+a1)*a_is10;
    uint64_t bh = b0*b_is9 + (b0*10+b1)*b_is10;
    uint64_t ch = c0*c_is9 + (c0*10+c1)*c_is10;
    uint64_t dh = d0*d_is9 + (d0*10+d1)*d_is10;
    return (ah*100000000ULL+a8) + (bh*100000000ULL+b8)
         + (ch*100000000ULL+c8) + (dh*100000000ULL+d8);
}

#ifdef __AVX512BW__
// parse_oct: 8 numbers at once via 512-bit MADDUBS/MADD/MULLO
// Layout in 512-bit register: [a,b] lane0, [c,d] lane1, [e,f] lane2, [g,h] lane3
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

    // Load last-8 bytes of each number
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

    // 512-bit MADDUBS/MADD/MULLO tree — independent within each 128-bit lane
    const __m512i W1 = _mm512_set1_epi16((int16_t)0x010a); // [10,1] × 32 pairs
    __m512i l1 = _mm512_maddubs_epi16(chunks, W1);

    const __m512i W2 = _mm512_set1_epi32(0x00010064); // [100,1] × 16 pairs
    __m512i l2 = _mm512_madd_epi16(l1, W2);

    // l2: [hi4, lo4] interleaved for each of 8 numbers (16 int32 total)
    const __m512i W3 = _mm512_set_epi32(
        1,10000, 1,10000, 1,10000, 1,10000,
        1,10000, 1,10000, 1,10000, 1,10000);
    __m512i l3 = _mm512_mullo_epi32(l2, W3);

    // nohadd: shuffle within each 128-bit lane (0xB1 = swap adjacent pairs) + add
    __m512i l3s = _mm512_shuffle_epi32(l3, (_MM_PERM_ENUM)0xB1);
    __m512i l4  = _mm512_add_epi32(l3, l3s);

    // Extract 8 values at positions 0,2,4,6,8,10,12,14 of l4
    __m128i lane0 = _mm512_castsi512_si128(l4);
    __m128i lane1 = _mm512_extracti32x4_epi32(l4, 1);
    __m128i lane2 = _mm512_extracti32x4_epi32(l4, 2);
    __m128i lane3 = _mm512_extracti32x4_epi32(l4, 3);
    uint32_t a8 = (uint32_t)_mm_cvtsi128_si32(lane0);
    uint32_t b8 = (uint32_t)_mm_extract_epi32(lane0, 2);
    uint32_t c8 = (uint32_t)_mm_cvtsi128_si32(lane1);
    uint32_t d8 = (uint32_t)_mm_extract_epi32(lane1, 2);
    uint32_t e8 = (uint32_t)_mm_cvtsi128_si32(lane2);
    uint32_t f8 = (uint32_t)_mm_extract_epi32(lane2, 2);
    uint32_t g8 = (uint32_t)_mm_cvtsi128_si32(lane3);
    uint32_t h8 = (uint32_t)_mm_extract_epi32(lane3, 2);

    // Branchless high digits for len=8 (high=0), 9, 10
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
#endif // __AVX512BW__

// Helper: extract N newline positions from mask starting at offset, return updated mask
// positions are relative to the block start (p), not base
#define EXTRACT1(mm, n0)                 n0=__builtin_ctzll(mm); mm&=mm-1
#define EXTRACT2(mm, n0,n1)              EXTRACT1(mm,n0); EXTRACT1(mm,n1)
#define EXTRACT4(mm, n0,n1,n2,n3)        EXTRACT2(mm,n0,n1); EXTRACT2(mm,n2,n3)
#define EXTRACT8(mm,n0,n1,n2,n3,n4,n5,n6,n7) EXTRACT4(mm,n0,n1,n2,n3); EXTRACT4(mm,n4,n5,n6,n7)

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0;
    if (size < 256) return scalar_tail(p, end, sum);
    const unsigned char* base = data;
    // 128-byte window; need 128 + 96 = 224 bytes of safe margin
    const unsigned char* safe_end = end - 224;

    while (p < safe_end) {
        uint64_t m0 = nl_mask64(p);
        uint64_t m1 = nl_mask64(p + 64);
        int c0 = __builtin_popcountll(m0);
        int c1 = __builtin_popcountll(m1);
        int cnt = c0 + c1;

        // For positions in m1, add 64 to get offset within 128-byte window
        // We use a combined extraction trick: process m0 first, then m1 with +64 offset

#ifdef __AVX512BW__
        if (__builtin_expect(cnt == 12, 1)) {
            // 12 numbers: 1×parse_oct(8) + 1×parse_quad(4)
            unsigned nv[12]; int idx=0;
            {uint64_t mm=m0; while(mm){nv[idx++]=(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            {uint64_t mm=m1; while(mm){nv[idx++]=64+(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            unsigned n0=nv[0],n1=nv[1],n2=nv[2],n3=nv[3],n4=nv[4],n5=nv[5],n6=nv[6],n7=nv[7];
            unsigned n8=nv[8],n9=nv[9],n10=nv[10],n11=nv[11];

            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
            const unsigned char *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7;
            const unsigned char *nl8=p+n8,*nl9=p+n9,*nl10=p+n10,*nl11=p+n11;

            const unsigned char *s0=base,   *s1=nl0+1,  *s2=nl1+1,  *s3=nl2+1;
            const unsigned char *s4=nl3+1,  *s5=nl4+1,  *s6=nl5+1,  *s7=nl6+1;
            const unsigned char *s8=nl7+1,  *s9=nl8+1,  *s10=nl9+1, *s11=nl10+1;
            size_t l0=(nl0-base),    l1=(nl1-nl0-1), l2=(nl2-nl1-1), l3=(nl3-nl2-1);
            size_t l4=(nl4-nl3-1),   l5=(nl5-nl4-1), l6=(nl6-nl5-1), l7=(nl7-nl6-1);
            size_t l8=(nl8-nl7-1),   l9=(nl9-nl8-1), l10=(nl10-nl9-1), l11=(nl11-nl10-1);

            sum += parse_oct(s0,l0, s1,l1, s2,l2, s3,l3, s4,l4, s5,l5, s6,l6, s7,l7)
                 + parse_quad(s8,l8, s9,l9, s10,l10, s11,l11);
            base = nl11+1;
        } else if (__builtin_expect(cnt == 13, 1)) {
            // 13 numbers: 1×parse_oct(8) + 1×parse_quad(4) + 1×parse_num(1)
            unsigned nv[13]; int idx=0;
            {uint64_t mm=m0; while(mm){nv[idx++]=(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            {uint64_t mm=m1; while(mm){nv[idx++]=64+(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            unsigned n0=nv[0],n1=nv[1],n2=nv[2],n3=nv[3],n4=nv[4],n5=nv[5],n6=nv[6],n7=nv[7];
            unsigned n8=nv[8],n9=nv[9],n10=nv[10],n11=nv[11],n12=nv[12];

            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
            const unsigned char *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7;
            const unsigned char *nl8=p+n8,*nl9=p+n9,*nl10=p+n10,*nl11=p+n11;
            const unsigned char *nl12=p+n12;

            const unsigned char *s0=base,  *s1=nl0+1, *s2=nl1+1, *s3=nl2+1;
            const unsigned char *s4=nl3+1, *s5=nl4+1, *s6=nl5+1, *s7=nl6+1;
            const unsigned char *s8=nl7+1, *s9=nl8+1, *s10=nl9+1,*s11=nl10+1;
            const unsigned char *s12=nl11+1;
            size_t l0=(nl0-base),   l1=(nl1-nl0-1), l2=(nl2-nl1-1), l3=(nl3-nl2-1);
            size_t l4=(nl4-nl3-1),  l5=(nl5-nl4-1), l6=(nl6-nl5-1), l7=(nl7-nl6-1);
            size_t l8=(nl8-nl7-1),  l9=(nl9-nl8-1), l10=(nl10-nl9-1),l11=(nl11-nl10-1);
            size_t l12=(nl12-nl11-1);

            sum += parse_oct(s0,l0, s1,l1, s2,l2, s3,l3, s4,l4, s5,l5, s6,l6, s7,l7)
                 + parse_quad(s8,l8, s9,l9, s10,l10, s11,l11)
                 + parse_num(s12,l12);
            base = nl12+1;
        } else if (__builtin_expect(cnt == 11, 0)) {
            // 11 numbers: 1×parse_oct(8) + 1×parse_pair(2) + 1×parse_num(1)
            unsigned nv[11]; int idx=0;
            {uint64_t mm=m0; while(mm){nv[idx++]=(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            {uint64_t mm=m1; while(mm){nv[idx++]=64+(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            unsigned n0=nv[0],n1=nv[1],n2=nv[2],n3=nv[3],n4=nv[4],n5=nv[5],n6=nv[6],n7=nv[7];
            unsigned n8=nv[8],n9=nv[9],n10=nv[10];

            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
            const unsigned char *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7;
            const unsigned char *nl8=p+n8,*nl9=p+n9,*nl10=p+n10;

            const unsigned char *s0=base,  *s1=nl0+1, *s2=nl1+1, *s3=nl2+1;
            const unsigned char *s4=nl3+1, *s5=nl4+1, *s6=nl5+1, *s7=nl6+1;
            const unsigned char *s8=nl7+1, *s9=nl8+1, *s10=nl9+1;
            size_t l0=(nl0-base),  l1=(nl1-nl0-1), l2=(nl2-nl1-1), l3=(nl3-nl2-1);
            size_t l4=(nl4-nl3-1), l5=(nl5-nl4-1), l6=(nl6-nl5-1), l7=(nl7-nl6-1);
            size_t l8=(nl8-nl7-1), l9=(nl9-nl8-1), l10=(nl10-nl9-1);

            sum += parse_oct(s0,l0, s1,l1, s2,l2, s3,l3, s4,l4, s5,l5, s6,l6, s7,l7)
                 + parse_pair(s8,l8, s9,l9) + parse_num(s10,l10);
            base = nl10+1;
        } else if (__builtin_expect(cnt == 14, 0)) {
            // 14 numbers: 1×parse_oct(8) + 1×parse_quad(4) + 1×parse_pair(2)
            unsigned nv[14]; int idx=0;
            {uint64_t mm=m0; while(mm){nv[idx++]=(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            {uint64_t mm=m1; while(mm){nv[idx++]=64+(unsigned)__builtin_ctzll(mm); mm&=mm-1;}}
            unsigned n0=nv[0],n1=nv[1],n2=nv[2],n3=nv[3],n4=nv[4],n5=nv[5],n6=nv[6],n7=nv[7];
            unsigned n8=nv[8],n9=nv[9],n10=nv[10],n11=nv[11],n12=nv[12],n13=nv[13];

            const unsigned char *nl0=p+n0,*nl1=p+n1,*nl2=p+n2,*nl3=p+n3;
            const unsigned char *nl4=p+n4,*nl5=p+n5,*nl6=p+n6,*nl7=p+n7;
            const unsigned char *nl8=p+n8,*nl9=p+n9,*nl10=p+n10,*nl11=p+n11;
            const unsigned char *nl12=p+n12,*nl13=p+n13;

            const unsigned char *s0=base,  *s1=nl0+1, *s2=nl1+1, *s3=nl2+1;
            const unsigned char *s4=nl3+1, *s5=nl4+1, *s6=nl5+1, *s7=nl6+1;
            const unsigned char *s8=nl7+1, *s9=nl8+1, *s10=nl9+1,*s11=nl10+1;
            const unsigned char *s12=nl11+1, *s13=nl12+1;
            size_t l0=(nl0-base),   l1=(nl1-nl0-1), l2=(nl2-nl1-1), l3=(nl3-nl2-1);
            size_t l4=(nl4-nl3-1),  l5=(nl5-nl4-1), l6=(nl6-nl5-1), l7=(nl7-nl6-1);
            size_t l8=(nl8-nl7-1),  l9=(nl9-nl8-1), l10=(nl10-nl9-1),l11=(nl11-nl10-1);
            size_t l12=(nl12-nl11-1), l13=(nl13-nl12-1);

            sum += parse_oct(s0,l0, s1,l1, s2,l2, s3,l3, s4,l4, s5,l5, s6,l6, s7,l7)
                 + parse_quad(s8,l8, s9,l9, s10,l10, s11,l11)
                 + parse_pair(s12,l12, s13,l13);
            base = nl13+1;
        } else {
#endif // __AVX512BW__
            // Fallback: process m0 and m1 separately using the AVX2 quad/pair paths
            // Handle m0
            {
                uint64_t mm = m0;
                while (mm) {
                    unsigned nlpos = __builtin_ctzll(mm);
                    const unsigned char* nlp = p + nlpos;
                    size_t len = (size_t)(nlp - base);
                    if (len) sum += parse_num(base, len);
                    base = nlp + 1;
                    mm &= mm - 1;
                }
            }
            // Handle m1 (offset +64)
            {
                uint64_t mm = m1;
                while (mm) {
                    unsigned nlpos = __builtin_ctzll(mm);
                    const unsigned char* nlp = p + 64 + nlpos;
                    size_t len = (size_t)(nlp - base);
                    if (len) sum += parse_num(base, len);
                    base = nlp + 1;
                    mm &= mm - 1;
                }
            }
#ifdef __AVX512BW__
        }
#endif
        p += 128;
    }
    return scalar_tail(base, end, sum);
}

#else // no AVX2
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
