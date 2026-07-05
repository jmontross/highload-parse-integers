// avx2_directload: PDEP parallel bit extraction + direct load-address computation.
//
// KEY INSIGHT: for number i with newline at position n_i in the 64-byte window,
// the SIMD parse-load address is always p + n_i - 8 (independent of `base`).
//
// Proof: parse_quad loads from p_a + len_a - 8.
//   For number 0: p_a=base, len_a=n0-(base-p) → p_a+len_a-8 = base+(n0-base+p)-8 = p+n0-8
//   For number 1: p_b=nl0+1=p+n0+1, len_b=n1-n0-1 → p_b+len_b-8 = p+n0+1+n1-n0-1-8 = p+n1-8
//   For number i: load_i = p + n_i - 8  (always, for all i).
//
// Benefits:
// 1. PDEP: n_i all available at cycle 6 (vs 24 for serial CTZ chain) → loads can start 18cy sooner.
// 2. Direct: load_i = p + n_i - 8 eliminates the {nl_i + 1} and {p_a + len_a - 8} chain.
//    Each load address has one fewer pointer addition in its dependency chain.
// 3. For numbers 1-5: lengths l_i = n_i - n_{i-1} - 1 are independent of `base`,
//    so high-digit computation for those can also start earlier.
//
// The restructured parse_quad_dl takes pre-computed load addresses instead of
// (start_pointer, length) pairs, eliminating 4 additions inside the hot function.
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

// Extract k-th set bit position using PDEP (all independent from same m).
#define NTH_BIT(k, m) ((unsigned)__builtin_ctzll(_pdep_u64(1ULL << (k), (m))))

// parse_quad_dl: takes pre-computed load pointers (p + n_i - 8) and lengths.
// Eliminates the p_a+len_a-8 address computation from inside the function.
static inline uint64_t parse_quad_dl(
    const unsigned char* __restrict__ la, size_t len_a,
    const unsigned char* __restrict__ lb, size_t len_b,
    const unsigned char* __restrict__ lc, size_t len_c,
    const unsigned char* __restrict__ ld, size_t len_d,
    const unsigned char* __restrict__ p_a,  // for high digit p_a[0], p_a[1]
    const unsigned char* __restrict__ p_b,
    const unsigned char* __restrict__ p_c,
    const unsigned char* __restrict__ p_d)
{
    if (__builtin_expect(len_a < 8 || len_b < 8 || len_c < 8 || len_d < 8, 0))
        return parse_num(p_a, len_a) + parse_num(p_b, len_b)
             + parse_num(p_c, len_c) + parse_num(p_d, len_d);

    // Load directly from pre-computed addresses (no addition inside hot path)
    __m128i ca = _mm_loadl_epi64((__m128i const*)la);
    __m128i cb = _mm_loadl_epi64((__m128i const*)lb);
    __m128i lo128 = _mm_unpacklo_epi64(ca, cb);
    __m128i cc = _mm_loadl_epi64((__m128i const*)lc);
    __m128i cd = _mm_loadl_epi64((__m128i const*)ld);
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

// parse_pair_dl: same pattern — pre-computed load addresses.
static inline uint64_t parse_pair_dl(
    const unsigned char* __restrict__ la, size_t len_a,
    const unsigned char* __restrict__ lb, size_t len_b,
    const unsigned char* __restrict__ p_a,
    const unsigned char* __restrict__ p_b)
{
    if (__builtin_expect(len_a < 8 || len_b < 8, 0))
        return parse_num(p_a, len_a) + parse_num(p_b, len_b);

    __m128i ca = _mm_loadl_epi64((__m128i const*)la);
    __m128i cb = _mm_loadl_epi64((__m128i const*)lb);
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
    return (a_high*100000000ULL+a8) + (b_high*100000000ULL+b8);
}

static inline uint64_t nl_mask64(const unsigned char* p) {
    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i nl = _mm256_set1_epi8('\n');
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
    return (uint64_t)m0 | ((uint64_t)m1 << 32);
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
            // PDEP: all 6 positions extracted independently (6cy latency total)
            unsigned n0 = NTH_BIT(0, m), n1 = NTH_BIT(1, m), n2 = NTH_BIT(2, m);
            unsigned n3 = NTH_BIT(3, m), n4 = NTH_BIT(4, m), n5 = NTH_BIT(5, m);

            // Direct load addresses: load_i = p + n_i - 8 (no dependency on base!)
            // For the fallback (len<8), we need start pointers anyway.
            const unsigned char *s0=base, *s1=p+n0+1, *s2=p+n1+1, *s3=p+n2+1,
                                *s4=p+n3+1, *s5=p+n4+1;
            size_t l0=(size_t)(p+n0-base), l1=(size_t)(n1-n0-1);
            size_t l2=(size_t)(n2-n1-1),   l3=(size_t)(n3-n2-1);
            size_t l4=(size_t)(n4-n3-1),    l5=(size_t)(n5-n4-1);

            // Pre-computed load addresses (p+n_i-8): no base dependency, no addition inside parse
            const unsigned char *ld0=p+n0-8, *ld1=p+n1-8, *ld2=p+n2-8;
            const unsigned char *ld3=p+n3-8, *ld4=p+n4-8, *ld5=p+n5-8;

            sum += parse_quad_dl(ld0,l0, ld1,l1, ld2,l2, ld3,l3, s0,s1,s2,s3)
                 + parse_pair_dl(ld4,l4, ld5,l5, s4,s5);
            base = p+n5+1;

        } else if (__builtin_expect(cnt == 5, 1)) {
            unsigned n0=NTH_BIT(0,m), n1=NTH_BIT(1,m), n2=NTH_BIT(2,m);
            unsigned n3=NTH_BIT(3,m), n4=NTH_BIT(4,m);
            const unsigned char *s0=base,*s1=p+n0+1,*s2=p+n1+1,*s3=p+n2+1,*s4=p+n3+1;
            size_t l0=(size_t)(p+n0-base),l1=(size_t)(n1-n0-1);
            size_t l2=(size_t)(n2-n1-1),l3=(size_t)(n3-n2-1),l4=(size_t)(n4-n3-1);
            const unsigned char *ld0=p+n0-8,*ld1=p+n1-8,*ld2=p+n2-8,*ld3=p+n3-8;
            sum += parse_quad_dl(ld0,l0,ld1,l1,ld2,l2,ld3,l3,s0,s1,s2,s3) + parse_num(s4,l4);
            base = p+n4+1;

        } else if (__builtin_expect(cnt == 7, 0)) {
            unsigned n0=NTH_BIT(0,m),n1=NTH_BIT(1,m),n2=NTH_BIT(2,m),n3=NTH_BIT(3,m);
            unsigned n4=NTH_BIT(4,m),n5=NTH_BIT(5,m),n6=NTH_BIT(6,m);
            const unsigned char *s0=base,*s1=p+n0+1,*s2=p+n1+1,*s3=p+n2+1;
            const unsigned char *s4=p+n3+1,*s5=p+n4+1,*s6=p+n5+1;
            size_t l0=(size_t)(p+n0-base),l1=(size_t)(n1-n0-1),l2=(size_t)(n2-n1-1);
            size_t l3=(size_t)(n3-n2-1),l4=(size_t)(n4-n3-1),l5=(size_t)(n5-n4-1);
            size_t l6=(size_t)(n6-n5-1);
            const unsigned char *ld0=p+n0-8,*ld1=p+n1-8,*ld2=p+n2-8,*ld3=p+n3-8;
            const unsigned char *ld4=p+n4-8,*ld5=p+n5-8;
            sum += parse_quad_dl(ld0,l0,ld1,l1,ld2,l2,ld3,l3,s0,s1,s2,s3)
                 + parse_pair_dl(ld4,l4,ld5,l5,s4,s5) + parse_num(s6,l6);
            base = p+n6+1;

        } else if (__builtin_expect(cnt == 4, 0)) {
            unsigned n0=NTH_BIT(0,m),n1=NTH_BIT(1,m),n2=NTH_BIT(2,m),n3=NTH_BIT(3,m);
            const unsigned char *s0=base,*s1=p+n0+1,*s2=p+n1+1,*s3=p+n2+1;
            size_t l0=(size_t)(p+n0-base),l1=(size_t)(n1-n0-1);
            size_t l2=(size_t)(n2-n1-1),l3=(size_t)(n3-n2-1);
            const unsigned char *ld0=p+n0-8,*ld1=p+n1-8,*ld2=p+n2-8,*ld3=p+n3-8;
            sum += parse_quad_dl(ld0,l0,ld1,l1,ld2,l2,ld3,l3,s0,s1,s2,s3);
            base = p+n3+1;

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
