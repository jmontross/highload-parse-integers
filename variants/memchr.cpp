// Variant template: memchr-based line boundaries + serial digit parse.
// Full standalone program (same I/O shape as champion). Copy this as a starting
// point for new experiments — swap out the inner parse for SIMD, etc.
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

static uint64_t solve(const unsigned char* p, size_t n) {
    const unsigned char* end = p + n;
    uint64_t sum = 0;
    while (p < end) {
        const unsigned char* nl = (const unsigned char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        uint64_t v = 0;
        for (const unsigned char* q = p; q < nl; ++q) v = v * 10 + (*q - '0');
        sum += v;
        p = nl + 1;
    }
    return sum;
}

int main() {
    struct stat st;
    if (fstat(0, &st) != 0) return 1;
    size_t size = (size_t)st.st_size;
    uint64_t sum = 0;
    if (size > 0) {
        const unsigned char* data = (const unsigned char*)
            mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, 0, 0);
        if (data != MAP_FAILED) {
            madvise((void*)data, size, MADV_SEQUENTIAL);
            sum = solve(data, size);
            printf("%" PRIu64 "\n", sum);
            return 0;
        }
    }
    static unsigned char buf[1 << 22];
    // simple fallback: accumulate across reads
    uint64_t v = 0; ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0)
        for (ssize_t i = 0; i < n; ++i) {
            unsigned d = (unsigned)buf[i] - '0';
            if (d <= 9) v = v * 10 + d; else { sum += v; v = 0; }
        }
    sum += v;
    printf("%" PRIu64 "\n", sum);
    return 0;
}
