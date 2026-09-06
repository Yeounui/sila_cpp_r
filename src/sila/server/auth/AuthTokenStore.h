// AuthTokenStore.h — thread-safe token store with sliding expiry (§3.11)
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <sila/common/util/PeriodicGC.h>

namespace sila2::auth {

class AuthTokenStore {
public:
    ~AuthTokenStore();

    struct TokenEntry {
        std::string userIdentifier;
        std::unordered_set<std::string> allowedFqis;
    };

    // Issue token — generate UUID, set expiration
    std::string issue(std::string userIdentifier,
                       std::unordered_set<std::string> allowedFqis,
                       std::chrono::seconds lifetime);

    // Validate + sliding expiry renewal
    // Invalid/expired/FQI denied → nullopt
    std::optional<TokenEntry> validate(const std::string& token,
                                        const std::string& targetFqi);

    // Store a remotely-verified token for cache reuse (§3.11 delegation).
    // If the token already exists, adds the FQI to its allowed set and resets expiry.
    void cache(const std::string& token, const std::string& fqi,
               std::chrono::seconds lifetime);

    bool remove(const std::string& token);
    std::size_t removeExpired();
    void clear();  // bulk invalidation on provider swap

    void startAutoGC(std::chrono::seconds interval) { gc_.start(interval); }
    void stopAutoGC() { gc_.stop(); }

    [[nodiscard("caller expects the token count")]]
    std::size_t size() const;

private:
    // Internal record adds expiry bookkeeping the public TokenEntry does not need.
    struct InternalEntry {
        TokenEntry entry;
        std::chrono::steady_clock::time_point expiresAt;
        std::chrono::seconds lifetime;  // kept per-entry to support sliding renewal on validate()
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, InternalEntry> tokens_;
    PeriodicGC gc_{[this] { removeExpired(); }};
};

}  // namespace sila2::auth
