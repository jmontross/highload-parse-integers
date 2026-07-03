// HighLoad.fun — parse_integers  (VARIANT: SWAR newline-find, memchr-free)
// Portable (no intrinsics; builds on ARM + x86). Hypothesis #3 from SCOREBOARD:
// the champion calls memchr() once PER NUMBER to find each newline — a function
// call + byte scan on the critical path. Here we derive the newline offset with
// a SWAR compare over a 16-byte window (covers any valid <=10-digit number plus
// its newline), so boundary-finding is branch-light and inline. Same SWAR digit
// parse as the champion. Whether this actually beats memchr is for the gate to
// decide — on ARM it likely lands within noise (HOLD); the point is a correct,
// buildable variant that exercises the loop end-to-end.
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

// SWAR: byte-position (0..7) of the first 0x0a in an 8-byte little-endian word,
// or 8 if none. haszero-style: a zero byte in (w ^ NL) marks a newline.
static inline unsigned nl_pos8(uint64_t w) {
    const uint64_t NL = 0x0a0a0a0a0a0a0a0aULL;
    uint64_t x = w ^ NL;
    uint64_t m = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
    return m ? (unsigned)(__builtin_ctzll(m) >> 3) : 8u;
}

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    // 16-byte newline window + 8-byte parse over-read → 32-byte guard.
    const unsigned char* safe_end = (size >= 32) ? end - 32 : data;
    uint64_t sum = 0;

    while (p < safe_end) {
        uint64_t w0, w1;
        memcpy(&w0, p, 8);
        unsigned idx = nl_pos8(w0);
        if (idx == 8) {                       // newline is in the second 8 bytes
            memcpy(&w1, p + 8, 8);
            unsigned idx2 = nl_pos8(w1);
            if (idx2 == 8) break;             // >16 chars w/o newline → tail (never for valid input)
            idx = 8 + idx2;
        }
        size_t len = idx;
        uint64_t v;
        if (len <= 8) {
            uint64_t chunk;
            memcpy(&chunk, p, 8);
            chunk <<= 8 * (8 - len);
            v = parse_8(chunk);
        } else {
            size_t h = len - 8;
            uint64_t high = 0;
            for (size_t k = 0; k < h; ++k) high = high * 10 + (p[k] - '0');
            uint64_t chunk;
            memcpy(&chunk, p + h, 8);
            v = high * 100000000ULL + parse_8(chunk);
        }
        sum += v;
        p += len + 1;
    }

    // Scalar tail: last bytes / no-trailing-newline / tiny inputs.
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
