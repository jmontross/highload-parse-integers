// HighLoad.fun — parse_integers  (VARIANT: v4 hot path, branchless high digits)
// Portable (no intrinsics). Builds on champion v4 (SWAR nlfind, zero-reload).
// Profiling insight: on this input almost every branch is predictable — 99.5% of
// values are 9–10 digits so the `idx<8` (<=7 digit) path is ~never taken, and
// `idx2==8` (>=16 digit) never. The ONE hostile branch is `if (h == 2)`, which
// splits 10-digit (~53%) from 9-digit (~47%) numbers — a near coin-flip that
// mispredicts ~47% of the time. Here the high-digit value is computed
// branchlessly from h∈{0,1,2} (comparisons → setcc, no jump), so the common
// 9/10-digit path has no data-dependent branch left.
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

static inline unsigned nl_pos8(uint64_t w) {
    const uint64_t NL = 0x0a0a0a0a0a0a0a0aULL;
    uint64_t x = w ^ NL;
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
        if (idx < 8) {                                 // <=7 digits (rare): w0 is the chunk
            sum += parse_8(w0 << (8 * (8 - idx)));
            p += idx + 1;
            continue;
        }
        uint64_t w1;
        memcpy(&w1, p + 8, 8);
        unsigned idx2 = nl_pos8(w1);
        if (idx2 == 8) break;                          // >16 chars w/o newline → tail
        size_t len = 8 + idx2;                          // 8, 9 or 10
        size_t h = len - 8;                             // 0, 1 or 2 high digits
        // low-8 chunk reconstructed from w0/w1 (h==0 → w0; ternary → cmov).
        uint64_t chunk = h ? ((w0 >> (8 * h)) | (w1 << (8 * (8 - h)))) : w0;
        // Branchless high digits. d0,d1 = w0's first two ASCII digits.
        uint64_t d0 = (w0 & 0xff) - '0';
        uint64_t d1 = ((w0 >> 8) & 0xff) - '0';
        uint64_t h1 = (h >= 1);                         // 1 if any high digit
        uint64_t h2 = (h == 2);                         // 1 if two high digits
        uint64_t high = h1 * (d0 * (1 + 9 * h2) + h2 * d1);  // h==1→d0; h==2→d0*10+d1; h==0→0
        sum += high * 100000000ULL + parse_8(chunk);
        p += len + 1;
    }

    uint64_t v = 0;
    for (; p < end; ++p) {
        unsigned d = (unsigned)*p - '0';
        if (d <= 9) v = v * 10 + d;
        else { sum += v; v = 0; }
    }
    sum += v;
    return sum;
}

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
