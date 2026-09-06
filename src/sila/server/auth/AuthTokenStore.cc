// AuthTokenStore.cc — token issuance, validation, sliding expiry (§3.11)
#include "AuthTokenStore.h"

#include <sila/common/util/uuid.h>
#include <sila/server/auth/FqiMatch.h>

#include <utility>

namespace sila2::auth {

AuthTokenStore::~AuthTokenStore() {
    stopAutoGC();
}

std::string AuthTokenStore::issue(std::string userIdentifier,
                                   std::unordered_set<std::string> allowedFqis,
                                   std::chrono::seconds lifetime) {
    std::string token = util::generateSecureToken();
    InternalEntry internal{
        TokenEntry{std::move(userIdentifier), std::move(allowedFqis)},
        std::chrono::steady_clock::now() + lifetime,
        lifetime};
    std::lock_guard<std::mutex> lock{mu_};
    tokens_.emplace(token, std::move(internal));
    return token;
}

std::optional<AuthTokenStore::TokenEntry> AuthTokenStore::validate(const std::string& token,
                                                                    const std::string& targetFqi) {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= it->second.expiresAt) {
        // Expired token found early — evict now instead of waiting for the next GC pass.
        tokens_.erase(it);
        return std::nullopt;
    }
    // A token granted a feature FQI must also validate the per-RPC FQIs the
    // cloud transport gates on, so this scans for coverage instead of asking
    // for exact membership (FqiMatch.h). The set is one entry per protected
    // feature the token was scoped to, so the scan is a handful of short
    // string compares under a mutex the call already holds.
    if (!anyFqiCovers(it->second.entry.allowedFqis, targetFqi)) {
        return std::nullopt;
    }
    // Sliding expiry: a successful validation renews the token's lifetime.
    it->second.expiresAt = std::chrono::steady_clock::now() + it->second.lifetime;
    return it->second.entry;
}

void AuthTokenStore::cache(const std::string& token, const std::string& fqi,
                            std::chrono::seconds lifetime) {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = tokens_.find(token);
    if (it != tokens_.end()) {
        it->second.entry.allowedFqis.insert(fqi);
        it->second.expiresAt = std::chrono::steady_clock::now() + lifetime;
        it->second.lifetime = lifetime;
        return;
    }
    InternalEntry internal{
        TokenEntry{{}, {fqi}},
        std::chrono::steady_clock::now() + lifetime,
        lifetime};
    tokens_.emplace(token, std::move(internal));
}

bool AuthTokenStore::remove(const std::string& token) {
    std::lock_guard<std::mutex> lock{mu_};
    return tokens_.erase(token) > 0;
}

std::size_t AuthTokenStore::removeExpired() {
    std::lock_guard<std::mutex> lock{mu_};
    std::size_t removed = 0;
    for (auto it = tokens_.begin(); it != tokens_.end();) {
        if (std::chrono::steady_clock::now() >= it->second.expiresAt) {
            it = tokens_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void AuthTokenStore::clear() {
    std::lock_guard<std::mutex> lock{mu_};
    tokens_.clear();
}

std::size_t AuthTokenStore::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return tokens_.size();
}

}  // namespace sila2::auth
