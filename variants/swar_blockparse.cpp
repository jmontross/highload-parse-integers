// Variant: SWAR (SIMD-within-a-register) number parse.
// Motivation: the scalar champion is latency-bound on the serial v=v*10+d chain
// (one dependent multiply per DIGIT). SWAR parses a whole <=8-digit number in a
// fixed, shallow tree of shifts/adds — one short dependency chain per NUMBER
// instead of per digit. Fully portable (no intrinsics): builds & runs on ARM
// too, so its CORRECTNESS is verifiable on the owner's Mac; the x86 routine
// benchmarks it. Think of it as the scalar stepping stone before AVX2.
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

// Parse 8 ASCII digits held in `chunk` (byte lane 0 = most-significant digit,
// little-endian load order). Classic pair/quad/oct reduction. Leading lanes
// that are 0x00 contribute nothing, so right-aligned shorter numbers work too.
static inline uint32_t parse_8(uint64_t chunk) {
    uint64_t lo = (chunk & 0x0f000f000f000f00ULL) >> 8;
    uint64_t hi = (chunk & 0x000f000f000f000fULL) * 10;
    chunk = lo + hi;                                   // 8 digits -> 4 pairs
    lo = (chunk & 0x00ff000000ff0000ULL) >> 16;
    hi = (chunk & 0x000000ff000000ffULL) * 100;
    chunk = lo + hi;                                   // 4 -> 2
    lo = (chunk & 0x0000ffff00000000ULL) >> 32;
    hi = (chunk & 0x000000000000ffffULL) * 10000;
    chunk = lo + hi;                                   // 2 -> 1
    return (uint32_t)chunk;
}

static uint64_t solve(const unsigned char* data, size_t size) {
    const unsigned char* p   = data;
    const unsigned char* end = data + size;
    // SWAR loads read 8 bytes at p; keep a 16-byte guard so the last few
    // numbers (and any without a trailing newline) go through the scalar tail.
    const unsigned char* safe_end = (size >= 16) ? end - 16 : data;
    uint64_t sum = 0;

    while (p < safe_end) {
        const unsigned char* nl =
            (const unsigned char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;                                // no newline -> tail
        size_t len = (size_t)(nl - p);
        uint64_t v;
        if (len <= 8) {
            uint64_t chunk;
            memcpy(&chunk, p, 8);                       // over-reads harmlessly
            chunk <<= 8 * (8 - len);                    // right-align: LSD -> lane 7
            v = parse_8(chunk);
        } else {                                        // 9 or 10 digits
            size_t h = len - 8;                         // 1 or 2 high digits
            uint64_t high = 0;
            for (size_t k = 0; k < h; ++k) high = high * 10 + (p[k] - '0');
            uint64_t chunk;
            memcpy(&chunk, p + h, 8);
            v = high * 100000000ULL + parse_8(chunk);
        }
        sum += v;
        p = nl + 1;
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
    // Pipe fallback: read into a buffer, then reuse the same solver.
    static unsigned char buf[1 << 26];
    size_t total = 0; ssize_t n;
    while (total < sizeof(buf) && (n = read(0, buf + total, sizeof(buf) - total)) > 0)
        total += (size_t)n;
    printf("%" PRIu64 "\n", solve(buf, total));
    return 0;
}
