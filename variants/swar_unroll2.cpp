// HighLoad.fun — parse_integers  (VARIANT: v5 logic, 2 numbers/iter, dual accum)
// Portable (no intrinsics). Builds on champion v5 (branchless high digits).
// After removing the branch mispredict, the bottleneck is LATENCY on the serial
// per-number chain (load → SWAR newline-scan → pointer advance, carried across
// numbers). This variant processes TWO numbers per iteration into two separate
// accumulators so the two independent parses (and their adds) can overlap in the
// out-of-order window, and the second number's work isn't gated behind the first
// number's accumulate. The boundary scans stay serial (number 2 starts where
// number 1 ends), so whether this actually hides enough latency to win is for
// the gate to decide.
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

// Parse one <=10-digit number starting at p (guaranteed to contain a newline
// within 16 bytes by the caller's guard). Returns value; sets *consumed to the
// bytes including the trailing newline. Branchless on the common 9–10 digit path.
static inline uint64_t parse_one(const unsigned char* p, size_t* consumed) {
    uint64_t w0;
    memcpy(&w0, p, 8);
    unsigned idx = nl_pos8(w0);
    if (idx < 8) {                                     // <=7 digits (rare)
        *consumed = idx + 1;
        return parse_8(w0 << (8 * (8 - idx)));
    }
    uint64_t w1;
    memcpy(&w1, p + 8, 8);
    unsigned idx2 = nl_pos8(w1);                        // guaranteed <8 within guard
    size_t len = 8 + idx2;
    size_t h = len - 8;
    uint64_t chunk = h ? ((w0 >> (8 * h)) | (w1 << (8 * (8 - h)))) : w0;
    uint64_t d0 = (w0 & 0xff) - '0';
    uint64_t d1 = ((w0 >> 8) & 0xff) - '0';
    uint64_t h1 = (h >= 1);
    uint64_t h2 = (h == 2);
    uint64_t high = h1 * (d0 * (1 + 9 * h2) + h2 * d1);
    *consumed = len + 1;
    return high * 100000000ULL + parse_8(chunk);
}

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    uint64_t sum = 0, sum2 = 0;

    // Two numbers per iteration; 48-byte guard covers two <=16-byte spans + the
    // 8-byte parse over-read.
    if (size >= 48) {
        const unsigned char* safe_end2 = end - 48;
        while (p < safe_end2) {
            size_t c1, c2;
            uint64_t v1 = parse_one(p, &c1);
            uint64_t v2 = parse_one(p + c1, &c2);
            sum  += v1;
            sum2 += v2;
            p += c1 + c2;
        }
    }
    sum += sum2;

    // Single-number path for the last numbers (uses the guarded parse while a
    // 16-byte window is safe, then the scalar tail handles the very end).
    if (size >= 16) {
        const unsigned char* safe_end1 = end - 16;
        while (p < safe_end1) {
            const unsigned char* nl = p;
            while (nl < end && *nl != '\n') ++nl;       // simple; only runs a few times
            if (nl >= safe_end1) break;
            size_t c;
            sum += parse_one(p, &c);
            p += c;
        }
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
