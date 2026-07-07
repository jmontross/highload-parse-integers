// avx2_4stream.cpp — 4 spatially-separated DRAM streams, 2 windows each
// Divides input into 4 equal blocks; each stream processes 2×64-byte windows
// per outer iteration = 8 total windows / 512 bytes (same as champion).
//
// WHY 4 vs 8: stuchlik_8stream had 24 pointer variables (p0..p7, b0..b7, s0..s7),
// exceeding x86-64's 16 GP registers → heavy stack spilling. 4 streams = 12 pointer
// variables → fits in registers. Same 8 outstanding DRAM requests, far less spilling.
//
// WHY 4 vs champion (sequential): champion has 8 consecutive 64-byte windows from
// a SINGLE sequential stream. HW prefetcher tracks it as 1 stream. 4-stream exposes
// 4 independent DRAM request sequences → HW prefetcher handles each independently,
// potentially giving 4× the effective MLP on hardware with latency-bound access.
//
// Interleaved: follow champion pattern — compute mask[i+1] while processing window[i]
// across all 4 streams to give AVX2 loads (ports 2/3) and parse ALU (ports 0/1/5)
// concurrent EU utilization.

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

static inline uint64_t nl_mask64(const unsigned char* p) {
    __m256i v0 = _mm256_loadu_si256((const __m256i*)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i nl = _mm256_set1_epi8('\n');
    uint32_t m0 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl));
    uint32_t m1 = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl));
    return (uint64_t)m0 | ((uint64_t)m1 << 32);
}

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* end = data + size;
    uint64_t sum = 0;

    if (size < 4 * 800) return scalar_tail(data, end, sum);

    // Split into 4 equal blocks at newline boundaries.
    const unsigned char* adj_start[4];
    const unsigned char* adj_end[4];

    adj_start[0] = data;
    for (int i = 1; i < 4; i++) {
        const unsigned char* q = data + (size_t)i * (size / 4);
        while (q < end && *q != '\n') ++q;
        adj_start[i] = (q < end) ? q + 1 : end;
    }
    for (int i = 0; i < 3; i++) adj_end[i] = adj_start[i + 1];
    adj_end[3] = end;

    // Per-stream parse pointers (scan pointer p, base pointer b, safe_end s)
    // 4 streams × 3 pointers = 12 registers — fits in x86-64's 15 usable GPRs.
    const unsigned char* p0 = adj_start[0], *b0 = adj_start[0];
    const unsigned char* p1 = adj_start[1], *b1 = adj_start[1];
    const unsigned char* p2 = adj_start[2], *b2 = adj_start[2];
    const unsigned char* p3 = adj_start[3], *b3 = adj_start[3];

    // Safe-end: leave 160 bytes margin for 2-window tail safety
    const unsigned char* s0 = (adj_end[0] > adj_start[0]+160) ? adj_end[0]-160 : adj_start[0];
    const unsigned char* s1 = (adj_end[1] > adj_start[1]+160) ? adj_end[1]-160 : adj_start[1];
    const unsigned char* s2 = (adj_end[2] > adj_start[2]+160) ? adj_end[2]-160 : adj_start[2];
    const unsigned char* s3 = (adj_end[3] > adj_start[3]+160) ? adj_end[3]-160 : adj_start[3];

    // Main loop: 2 windows per stream = 8 windows total = 512 bytes.
    // Interleaved mask/process: compute mask[next] while processing mask[current]
    // across all 4 streams — AVX2 loads (ports 2/3) concurrent with parse ALU (0/1/5).
    while (__builtin_expect(p0 < s0 & p1 < s1 & p2 < s2 & p3 < s3, 1)) {
        // T1 prefetch 1536B ahead per stream (2 cache lines per stream = 8 total hints)
        _mm_prefetch((const char*)(p0 + 1536),      _MM_HINT_T1);
        _mm_prefetch((const char*)(p0 + 1536 + 64), _MM_HINT_T1);
        _mm_prefetch((const char*)(p1 + 1536),      _MM_HINT_T1);
        _mm_prefetch((const char*)(p1 + 1536 + 64), _MM_HINT_T1);
        _mm_prefetch((const char*)(p2 + 1536),      _MM_HINT_T1);
        _mm_prefetch((const char*)(p2 + 1536 + 64), _MM_HINT_T1);
        _mm_prefetch((const char*)(p3 + 1536),      _MM_HINT_T1);
        _mm_prefetch((const char*)(p3 + 1536 + 64), _MM_HINT_T1);

        // Issue all 8 loads before any processing — 8 independent DRAM requests.
        // Stream 0 and stream 1 are ~105MB apart, etc.
        uint64_t m0a = nl_mask64(p0);
        uint64_t m1a = nl_mask64(p1);
        uint64_t m2a = nl_mask64(p2);
        uint64_t m3a = nl_mask64(p3);
        uint64_t m0b = nl_mask64(p0 + 64);
        uint64_t m1b = nl_mask64(p1 + 64);
        uint64_t m2b = nl_mask64(p2 + 64);
        uint64_t m3b = nl_mask64(p3 + 64);

        // Process all 8 windows. Cross-stream dependencies: none.
        // Intra-stream: window b uses base updated by window a (serial dependency
        // within stream, but stream 0 is independent of stream 1/2/3).
        sum += process_window(p0,      b0, m0a);
        sum += process_window(p0 + 64, b0, m0b);
        p0 += 128;

        sum += process_window(p1,      b1, m1a);
        sum += process_window(p1 + 64, b1, m1b);
        p1 += 128;

        sum += process_window(p2,      b2, m2a);
        sum += process_window(p2 + 64, b2, m2b);
        p2 += 128;

        sum += process_window(p3,      b3, m3a);
        sum += process_window(p3 + 64, b3, m3b);
        p3 += 128;
    }

    // Per-stream cleanup: sequential single-window tail for each block
#define STREAM_TAIL(pi, bi, ei) \
    while ((pi) + 96 < (ei)) { \
        sum += process_window((pi), (bi), nl_mask64(pi)); (pi) += 64; \
    } \
    sum = scalar_tail((bi), (ei), sum);

    STREAM_TAIL(p0, b0, adj_end[0])
    STREAM_TAIL(p1, b1, adj_end[1])
    STREAM_TAIL(p2, b2, adj_end[2])
    STREAM_TAIL(p3, b3, adj_end[3])
#undef STREAM_TAIL

    return sum;
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
