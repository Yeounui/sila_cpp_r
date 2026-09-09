// AuthSession.h — client-side auth token lifecycle (architecture.md §4.4)

#pragma once

#include <grpcpp/channel.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sila2 {

/// The access-token lifecycle for one SilaClientBase connection: logs in to
/// the AuthenticationService, holds the resulting token, and renews it in a
/// background thread before it expires. Normally driven by
/// SilaClientBase::authenticate(); a caller does not construct or call this
/// directly.
class AuthSession {
public:
    /// @param user,password Login RPC credentials.
    /// @param serverUuid RequestedServer parameter for Login.
    /// @param channel The gRPC channel to the target server, used to create
    ///        the AuthenticationService stub.
    AuthSession(std::string user, std::string password,
                std::string serverUuid,
                std::shared_ptr<grpc::Channel> channel);
    ~AuthSession();

    /// Performs Login and obtains a token. On success, accessToken()
    /// becomes valid and the auto-renewal timer starts.
    /// @return True on success.
    bool login(const std::vector<std::string>& requestedFeatures = {});

    /// @return The current access token, or empty if not logged in or expired.
    // By value: a reference into the guarded member would outlive the lock and
    // the renewal thread can rewrite it under the caller's feet.
    std::string accessToken() const;

    /// @return True if we have a valid (non-expired) token.
    bool isAuthenticated() const;

    /// Stops auto-renewal and clears the token.
    void logout();

    /// @return The token lifetime reported by the last Login response.
    std::chrono::seconds tokenLifetime() const;

    /// Callback signature for setTokenChangeCallback(): invoked with the new
    /// access token whenever auto-renewal replaces it.
    using TokenChangeCallback = std::function<void(const std::string& newToken)>;
    /// Registers a callback invoked with the new token each time
    /// auto-renewal replaces it. SilaClientBase uses this to keep
    /// MetadataInjector's access-token entry current.
    void setTokenChangeCallback(TokenChangeCallback cb);

private:
    void renewToken();

    std::string user_;
    std::string password_;
    std::string serverUuid_;
    std::shared_ptr<grpc::Channel> channel_;

    // Stored from last login() call so renewToken() can replay the request
    std::vector<std::string> requestedFeatures_;

    mutable std::mutex mu_;
    std::string accessToken_;
    std::chrono::seconds tokenLifetime_{0};
    std::chrono::steady_clock::time_point tokenExpiry_;

    TokenChangeCallback onTokenChange_;

    // Renewal thread — wakes up before token expires
    std::atomic<bool> stop_{false};
    std::thread renewalThread_;
    std::condition_variable cv_;
};

}  // namespace sila2
