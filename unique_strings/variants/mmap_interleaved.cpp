// unique_strings — mmap input + INTERLEAVED 16-byte slots.
// Two wins over champion v2:
//  1) One flat array of {lo,hi} 16-byte slots → each probe touches ONE cache
//     line, vs v2's separate lo[]/hi[] arrays = 2 misses per probe.
//  2) mmap the whole input (zero-copy) instead of fread + leftover memmove.
// Same exact 128-bit key encoding + fmix64 hash + linear probing → identical
// (exact) result, just faster. Portable (mmap guarded; read() fallback).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

static inline uint64_t mix(uint64_t x) {          // fmix64 (murmur3 finalizer)
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

struct Slot { uint64_t lo, hi; };

static uint64_t solve(const unsigned char* data, size_t size) {
    const uint64_t BITS = 24, SIZE = 1ULL << BITS, MASK = SIZE - 1;
    Slot* tab = (Slot*)calloc(SIZE, sizeof(Slot));   // 2^24 * 16B = 256 MB
    if (!tab) return 0;
    uint64_t count = 0;
    const unsigned char* p = data;
    const unsigned char* end = data + size;
    while (p < end) {
        const unsigned char* nl = (const unsigned char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;                              // spec: newline-terminated
        size_t len = (size_t)(nl - p);               // 1..16
        unsigned char kb[16] = {0};
        memcpy(kb, p, len);                          // zero-padded key (exact len, no over-read)
        uint64_t klo, khi;
        memcpy(&klo, kb, 8); memcpy(&khi, kb + 8, 8);
        uint64_t h = (mix(klo) ^ (mix(khi) + 0x9e3779b97f4a7c15ULL)) & MASK;
        while (true) {
            Slot& s = tab[h];
            if (s.lo == 0 && s.hi == 0) { s.lo = klo; s.hi = khi; count++; break; }
            if (s.lo == klo && s.hi == khi) break;   // duplicate
            h = (h + 1) & MASK;
        }
        p = nl + 1;
    }
    free(tab);
    return count;
}

int main() {
    struct stat st;
    if (fstat(0, &st) == 0) {
        size_t size = (size_t)st.st_size;
        if (size > 0) {
            const unsigned char* data = (const unsigned char*)
                mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, 0, 0);
            if (data != MAP_FAILED) {
                madvise((void*)data, size, MADV_SEQUENTIAL);
                printf("%llu\n", (unsigned long long)solve(data, size));
                return 0;
            }
        }
    }
    size_t cap = 1 << 20, len = 0; unsigned char* buf = (unsigned char*)malloc(cap);
    ssize_t n;
    while ((n = read(0, buf + len, cap - len)) > 0) { len += (size_t)n; if (len == cap) { cap *= 2; buf = (unsigned char*)realloc(buf, cap); } }
    printf("%llu\n", (unsigned long long)solve(buf, len));
    return 0;
}
