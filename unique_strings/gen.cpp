// Generate N newline-separated tokens for the unique_strings challenge.
// Alphabet = [0-9 a-z A-Z @ # % *] (66 chars). Token length 6..16.
// Duplicates are created on purpose: each token encodes a key drawn from a
// bounded universe M (default 4N/5), so the EXACT distinct-token count is
// stable and reproducible. The key->string map is injective (base-66 of the
// key, zero-padded to a key-derived length), so #unique strings == #distinct keys.
//
// Build: c++ -O3 gen.cpp -o gen ; Run: ./gen 10000000 [M] > input.txt
#include <cstdio>
#include <cstdint>
#include <cstdlib>

static const char* ALPHA =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ@#%*"; // 66 chars

static inline uint64_t splitmix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

int main(int argc, char** argv) {
    uint64_t N = argc > 1 ? strtoull(argv[1], nullptr, 10) : 10000000ULL;
    uint64_t M = argc > 2 ? strtoull(argv[2], nullptr, 10) : (N * 4) / 5; // key universe
    if (M == 0) M = 1;

    uint64_t s = 0x0123456789abcdefULL;   // fixed seed → reproducible
    static char out[1 << 22]; int o = 0;
    char tmp[16];

    for (uint64_t i = 0; i < N; ++i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;     // xorshift64
        uint64_t key = s % M;
        // length depends ONLY on key so the key->string map stays injective
        int L = 6 + (int)(splitmix(key) % 11);       // length in [6,16]
        uint64_t k = key;
        for (int j = L - 1; j >= 0; --j) { tmp[j] = ALPHA[k % 66]; k /= 66; } // 66^6 >> M
        for (int j = 0; j < L; ++j) out[o++] = tmp[j];
        out[o++] = '\n';
        if (o > (1 << 22) - 32) { fwrite(out, 1, o, stdout); o = 0; }
    }
    fwrite(out, 1, o, stdout);
    return 0;
}
