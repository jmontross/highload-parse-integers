// HighLoad.fun — parse_integers
// Sum 50,000,000 newline-separated uint32 values. Scored on wall-time.
// Compiler: g++ 10.5.0   Flags: -O3 -march=native   Limits: 30s / 512MB / 1 core
//
// Strategy: zero-copy the whole input with mmap, then run one tight,
// branch-light pass. No per-byte function call, no asserts, no count.
//
// Only two byte classes appear in the input: digits ('0'..'9') and '\n'.
// So `(unsigned)byte - '0' <= 9` cleanly separates "digit" from "separator"
// with a single compare — no need to test for '\n' explicitly.

#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifndef MAP_POPULATE   // Linux-only; harmless no-op elsewhere (e.g. macOS)
#define MAP_POPULATE 0
#endif

int main() {
    struct stat st;
    if (fstat(0, &st) != 0) return 1;
    size_t size = (size_t)st.st_size;

    uint64_t sum = 0;   // 64-bit: overflow of the sum is expected/allowed
    uint64_t v   = 0;   // current number being parsed

    if (size > 0) {
        // MAP_POPULATE prefaults the pages so we don't pay page-fault
        // latency inside the hot loop.
        const unsigned char* data = (const unsigned char*)
            mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, 0, 0);

        if (data != MAP_FAILED) {
            madvise((void*)data, size, MADV_SEQUENTIAL);
            for (size_t i = 0; i < size; ++i) {
                unsigned d = (unsigned)data[i] - '0';
                if (d <= 9) v = v * 10 + d;        // digit
                else { sum += v; v = 0; }          // '\n' — flush the number
            }
            sum += v;                              // in case there's no final '\n'
            printf("%" PRIu64 "\n", sum);
            return 0;
        }
    }

    // Fallback for when stdin isn't an mmap-able file (e.g. a pipe).
    // `v` carries across chunk boundaries, so numbers may span reads safely.
    static unsigned char buf[1 << 22];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            unsigned d = (unsigned)buf[i] - '0';
            if (d <= 9) v = v * 10 + d;
            else { sum += v; v = 0; }
        }
    }
    sum += v;
    printf("%" PRIu64 "\n", sum);
    return 0;
}
