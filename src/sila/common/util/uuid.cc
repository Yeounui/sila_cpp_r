// uuid.cc — RFC 4122 v4 UUID generation
#include "uuid.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include <openssl/rand.h>

namespace sila2::util {

std::string generateUuid() {
    // CSPRNG via RAND_bytes — mt19937_64 was predictable (audit 1.3a)
    return generateSecureToken();
}

std::string generateSecureToken() {
    // RAND_bytes draws from BoringSSL's CSPRNG (kernel entropy pool on Linux,
    // CryptGenRandom on Windows) — unpredictable output suitable for auth tokens,
    // unlike mt19937_64 which is deterministic once the seed is known.
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        throw std::runtime_error{"generateSecureToken: RAND_bytes failed"};
    }

    // Interpret the 16 random bytes as two 64-bit halves, then stamp the
    // RFC 4122 v4 version/variant bits — same layout as generateUuid().
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    for (int i = 0; i < 8; ++i) {
        hi = (hi << 8) | buf[i];
        lo = (lo << 8) | buf[i + 8];
    }

    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char out[37];
    std::snprintf(out, sizeof(out), "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(hi >> 32),
        static_cast<unsigned>((hi >> 16) & 0xFFFF),
        static_cast<unsigned>(hi & 0xFFFF),
        static_cast<unsigned>(lo >> 48),
        static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return std::string(out);
}

}  // namespace sila2::util
