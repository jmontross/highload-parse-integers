// Local harness: slurp stdin once, then time several parse strategies over the
// SAME in-memory buffer so we compare *parse* cost (not I/O). Prints ns/line and
// GB/s for each, and asserts all strategies agree on the sum.
//
// Build: c++ -O3 -march=native bench_parsers.cpp -o bench_parsers
// Run:   ./bench_parsers < input.txt
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

static unsigned char* slurp(size_t* out) {
    size_t cap = 1 << 20, len = 0;
    unsigned char* b = (unsigned char*)malloc(cap);
    ssize_t n;
    while ((n = read(0, b + len, cap - len)) > 0) {
        len += (size_t)n;
        if (len == cap) { cap *= 2; b = (unsigned char*)realloc(b, cap); }
    }
    *out = len;
    return b;
}

// --- strategies ---------------------------------------------------------------
static uint64_t parse_branch(const unsigned char* p, size_t n) {
    uint64_t sum = 0, v = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned d = (unsigned)p[i] - '0';
        if (d <= 9) v = v * 10 + d;
        else { sum += v; v = 0; }
    }
    return sum + v;
}

static uint64_t parse_branchless(const unsigned char* p, size_t n) {
    uint64_t sum = 0, v = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned d = (unsigned)p[i] - '0';
        bool isd = d <= 9;
        sum += isd ? 0 : v;          // flush completed number on separator (cmov)
        v    = isd ? v * 10 + d : 0; // extend or reset (cmov)
    }
    return sum + v;
}

static uint64_t parse_memchr(const unsigned char* p, size_t n) {
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

static double now_ns() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

typedef uint64_t (*fn)(const unsigned char*, size_t);
static void run(const char* name, fn f, const unsigned char* p, size_t n, size_t lines) {
    uint64_t s = 0; double best = 1e30;
    for (int r = 0; r < 5; ++r) {
        double t0 = now_ns();
        s = f(p, n);
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
    }
    printf("%-14s sum=%llu  %.2f ms  %.3f ns/line  %.2f GB/s\n",
           name, (unsigned long long)s, best / 1e6,
           best / (double)lines, (double)n / best);
}

int main() {
    size_t n; unsigned char* p = slurp(&n);
    size_t lines = 0; for (size_t i = 0; i < n; ++i) lines += (p[i] == '\n');
    printf("bytes=%zu lines=%zu\n", n, lines);
    run("branch",     parse_branch,     p, n, lines);
    run("branchless", parse_branchless, p, n, lines);
    run("memchr",     parse_memchr,     p, n, lines);
    return 0;
}
