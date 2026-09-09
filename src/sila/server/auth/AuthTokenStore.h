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

/// Holds issued access tokens and their per-token allowed-FQI set, with sliding expiry.
///
/// Owned and assembled by SiLAServerBase::Builder::WithAuthentication(); a server
/// author does not construct one directly.
class AuthTokenStore {
public:
    ~AuthTokenStore();

    /// One token's authorization state: which user it belongs to and which FQIs it
    /// authorizes, per @ref AccessPolicy::allowedFqis "the AccessPolicy that granted it".
    struct TokenEntry {
        std::string userIdentifier;                  ///< Identifies the user this token was issued to.
        std::unordered_set<std::string> allowedFqis; ///< FQIs this token authorizes calls against.
    };

    /// Issues a fresh token for `userIdentifier`, scoped to `allowedFqis`, expiring
    /// after `lifetime` unless renewed. Called once per successful Login.
    /// @return the token string to hand back to the client.
    // Issue token — generate UUID, set expiration
    std::string issue(std::string userIdentifier,
                       std::unordered_set<std::string> allowedFqis,
                       std::chrono::seconds lifetime);

    /// Checks `token` exists, has not expired, and covers `targetFqi`
    /// (anyFqiCovers()). A successful validation renews the token's sliding expiry.
    /// @return the token's entry on success, `nullopt` if the token is unknown,
    ///         expired, or not scoped to `targetFqi`.
    // Validate + sliding expiry renewal
    // Invalid/expired/FQI denied → nullopt
    std::optional<TokenEntry> validate(const std::string& token,
                                        const std::string& targetFqi);

    /// Records that `token` is valid for `fqi`, without going through issue() --
    /// for a token this server did not itself grant (e.g. verified by a remote
    /// authorization provider). Adds `fqi` to the token's allowed set and (re)sets
    /// its expiry if the token is already known.
    // Store a remotely-verified token for cache reuse (§3.11 delegation).
    // If the token already exists, adds the FQI to its allowed set and resets expiry.
    void cache(const std::string& token, const std::string& fqi,
               std::chrono::seconds lifetime);

    /// Invalidates one token immediately, e.g. on client Logout.
    /// @return true if `token` was present and removed.
    bool remove(const std::string& token);
    /// Sweeps and removes every token that has already expired.
    /// @return the number of tokens removed.
    std::size_t removeExpired();
    /// Invalidates every issued token at once.
    void clear();  // bulk invalidation on provider swap

    /// Runs removeExpired() on a background timer every `interval`, so expired
    /// tokens are reclaimed without a validate() call happening to hit them first.
    void startAutoGC(std::chrono::seconds interval) { gc_.start(interval); }
    /// Stops the background GC timer started by startAutoGC(), if running.
    void stopAutoGC() { gc_.stop(); }

    /// @return The number of tokens currently stored, including any already
    ///         expired but not yet swept by removeExpired().
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
