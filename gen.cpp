// Generate N lines of random integers in [0, 2147483647] to stdout.
// Build: c++ -O3 gen.cpp -o gen ; Run: ./gen 50000000 > input.txt
#include <cstdio>
#include <cstdint>
#include <cstdlib>
int main(int argc, char** argv) {
    uint64_t N = argc > 1 ? strtoull(argv[1], nullptr, 10) : 50000000ULL;
    uint64_t s = 0x9e3779b97f4a7c15ULL;               // fixed seed → reproducible
    static char out[1 << 22]; int o = 0;
    for (uint64_t i = 0; i < N; ++i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;      // xorshift64
        uint32_t v = (uint32_t)(s % 2147483648ULL);   // [0, 2147483647]
        char tmp[12]; int t = 0;
        do { tmp[t++] = '0' + v % 10; v /= 10; } while (v);
        while (t) out[o++] = tmp[--t];
        out[o++] = '\n';
        if (o > (1 << 22) - 16) { fwrite(out, 1, o, stdout); o = 0; }
    }
    fwrite(out, 1, o, stdout);
    return 0;
}
