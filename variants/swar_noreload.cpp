// HighLoad.fun — parse_integers  (VARIANT: SWAR nlfind, zero-reload hot path)
// Portable (no intrinsics). Builds on v3 (SWAR newline-find). Key change: the
// MAJORITY of inputs are 9–10 digit numbers (values up to 2^31), which take the
// len>8 path. v3 loads a THIRD time there (memcpy at p+h) to get the low-8
// window. But we already hold w0 (bytes 0–7) and w1 (bytes 8–15) from the
// newline scan, and every digit of a <=10-digit number lives inside those 16
// bytes — so we reconstruct the low-8 chunk by shifting w0/w1 together instead
// of touching memory again. High 1–2 digits come straight out of w0. Net: one
// fewer load per number on the hot path, no new branches.
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
        uint64_t v;
        if (idx < 8) {                                 // <=8 digits: w0 is the chunk
            v = parse_8(w0 << (8 * (8 - idx)));
            sum += v;
            p += idx + 1;
        } else {                                       // 9–10 digits (the common case)
            uint64_t w1;
            memcpy(&w1, p + 8, 8);
            unsigned idx2 = nl_pos8(w1);
            if (idx2 == 8) break;                      // >16 chars w/o newline → tail
            size_t len = 8 + idx2;                      // 8, 9 or 10 (newline at pos 8 => len 8)
            size_t h = len - 8;                         // 0, 1 or 2 high digits
            // Reconstruct the 8 bytes at p+h from w0/w1 — no memory touch.
            // h==0 (exactly 8 digits): the chunk is w0 itself; shifting by 64
            // would be UB, so special-case it.
            uint64_t chunk = h ? ((w0 >> (8 * h)) | (w1 << (8 * (8 - h)))) : w0;
            uint64_t high = 0;                          // high digits from w0's low bytes
            if (h >= 1) {
                high = (w0 & 0xff) - '0';
                if (h == 2) high = high * 10 + (((w0 >> 8) & 0xff) - '0');
            }
            v = high * 100000000ULL + parse_8(chunk);
            sum += v;
            p += len + 1;
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
