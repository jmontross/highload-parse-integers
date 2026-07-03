// HighLoad.fun — unique_strings
// Count the number of DISTINCT newline-separated tokens on stdin.
// Tokens: length 1..16, alphabet = [0-9 a-z A-Z @ # % *] (66 chars).
// Scored on wall-time. Limits (assumed, confirm at judge): 30s / 512MB / 1 core.
//
// Seed champion = the owner's baseline: FNV-1a hash each token into a
// std::unordered_set<uint64_t> and print the set size. This trades an
// astronomically small 64-bit collision risk for speed (no string storage).
// NOTE: run.sh's correctness gate compares against the EXACT unique-line count,
// so a collision would be caught on the generated input.

#include <cstdio>
#include <cstdint>
#include <cassert>
#include <unordered_set>

static bool allowedTable[256];

static inline void initAllowed() {
    const char* chars = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ@#%*";
    for (const char* p = chars; *p; ++p) {
        allowedTable[(unsigned char)*p] = true;
    }
}

static inline uint64_t fnv1a(const char* s, int len) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline void validateToken(const char* s, int len) {
    assert(len > 0 && len <= 16);
    for (int i = 0; i < len; i++) {
        assert(allowedTable[(unsigned char)s[i]]);
    }
}

int main() {
    initAllowed();

    std::unordered_set<uint64_t> hashes;
    hashes.reserve(1 << 20);

    char buf[1 << 20];
    int leftover = 0;

    while (true) {
        int n = fread(buf + leftover, 1, sizeof(buf) - leftover, stdin);
        if (n <= 0 && leftover == 0) break;
        int total = leftover + n;
        int start = 0;
        for (int i = 0; i < total; i++) {
            if (buf[i] == '\n') {
                int len = i - start;
                validateToken(buf + start, len);
                hashes.insert(fnv1a(buf + start, len));
                start = i + 1;
            }
        }
        if (n <= 0) {
            // Trailing bytes without newline: malformed.
            assert(start == total);
            break;
        }
        if (start < total) {
            leftover = total - start;
            __builtin_memmove(buf, buf + start, leftover);
        } else {
            leftover = 0;
        }
    }

    printf("%zu\n", hashes.size());
    return 0;
}
