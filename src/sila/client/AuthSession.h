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

class AuthSession {
public:
    // user/password: credentials for Login RPC
    // serverUuid: RequestedServer parameter for Login
    // channel: the gRPC channel to the target server (used to create AuthenticationService stub)
    AuthSession(std::string user, std::string password,
                std::string serverUuid,
                std::shared_ptr<grpc::Channel> channel);
    ~AuthSession();

    // Perform Login and obtain token. Returns true on success.
    // On success, accessToken() becomes valid and the auto-renewal timer starts.
    bool login(const std::vector<std::string>& requestedFeatures = {});

    // Current access token (empty if not logged in or expired).
    // By value: a reference into the guarded member would outlive the lock and
    // the renewal thread can rewrite it under the caller's feet.
    std::string accessToken() const;

    // True if we have a valid (non-expired) token
    bool isAuthenticated() const;

    // Stop auto-renewal and clear token
    void logout();

    // Token lifetime from last Login response
    std::chrono::seconds tokenLifetime() const;

    using TokenChangeCallback = std::function<void(const std::string& newToken)>;
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
