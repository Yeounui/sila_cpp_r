// AuthSession.cc — client-side auth token lifecycle (architecture.md §4.4)
#include "AuthSession.h"

#include "AuthenticationService.grpc.pb.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace sila2 {

namespace {
namespace auth_proto = sila2::org::silastandard::core::authenticationservice::v1;

// Renewing at the last second races with the server-side expiry check; 80%
// of the granted lifetime leaves headroom for the RPC round trip.
constexpr double kRenewalFraction = 0.8;

// Shared by login() and renewToken(), which both issue the same Login RPC.
auth_proto::Login_Parameters buildLoginParams(const std::string& user, const std::string& password,
                                               const std::string& serverUuid,
                                               const std::vector<std::string>& requestedFeatures) {
    auth_proto::Login_Parameters params;
    params.mutable_useridentification()->set_value(user);
    params.mutable_password()->set_value(password);
    params.mutable_requestedserver()->set_value(serverUuid);
    for (const auto& feature : requestedFeatures) {
        params.add_requestedfeatures()->set_value(feature);
    }
    return params;
}
}  // namespace

AuthSession::AuthSession(std::string user, std::string password,
                          std::string serverUuid,
                          std::shared_ptr<grpc::Channel> channel)
    : user_{std::move(user)},
      password_{std::move(password)},
      serverUuid_{std::move(serverUuid)},
      channel_{std::move(channel)} {}

AuthSession::~AuthSession() {
    logout();
}

bool AuthSession::login(const std::vector<std::string>& requestedFeatures) {
    logout();  // stop any prior renewal thread before starting a new session
    requestedFeatures_ = requestedFeatures;

    // Login is infrequent (once per session plus renewals); creating the
    // stub per call avoids keeping one alive for the object's whole lifetime.
    auto stub = auth_proto::AuthenticationService::NewStub(channel_);

    const auth_proto::Login_Parameters params = buildLoginParams(user_, password_, serverUuid_, requestedFeatures_);

    grpc::ClientContext context;
    auth_proto::Login_Responses response;
    const grpc::Status status = stub->Login(&context, params, &response);
    if (!status.ok()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock{mu_};
        accessToken_ = response.accesstoken().value();
        tokenLifetime_ = std::chrono::seconds{response.tokenlifetime().value()};
        tokenExpiry_ = std::chrono::steady_clock::now() + tokenLifetime_;
    }

    stop_ = false;
    renewalThread_ = std::thread([this] { renewToken(); });
    return true;
}

std::string AuthSession::accessToken() const {
    std::lock_guard<std::mutex> lock{mu_};
    return accessToken_;
}

bool AuthSession::isAuthenticated() const {
    std::lock_guard<std::mutex> lock{mu_};
    return !accessToken_.empty() && std::chrono::steady_clock::now() < tokenExpiry_;
}

std::chrono::seconds AuthSession::tokenLifetime() const {
    std::lock_guard<std::mutex> lock{mu_};
    return tokenLifetime_;
}

void AuthSession::logout() {
    {
        // Must hold mu_ while flipping stop_: renewToken() holds mu_ across
        // cv_.wait_for's predicate check, so a notify_all() landing outside
        // this lock can arrive between the thread's "stop_ still false" read
        // and its wait_for call, and get lost — logout() would then block in
        // join() for a full renewalDelay (minutes, per tokenLifetime_)
        // instead of waking the renewal thread immediately.
        std::lock_guard<std::mutex> lock{mu_};
        stop_ = true;
    }
    cv_.notify_all();
    const bool calledByRenewalThread = renewalThread_.joinable() &&
                                       renewalThread_.get_id() == std::this_thread::get_id();
    if (renewalThread_.joinable() && !calledByRenewalThread) {
        renewalThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock{mu_};
        accessToken_.clear();
        tokenLifetime_ = std::chrono::seconds{0};
        tokenExpiry_ = std::chrono::steady_clock::time_point{};
    }

    if (!calledByRenewalThread) {
        stop_ = false;  // allow a subsequent login() to schedule a fresh thread
    }
}

void AuthSession::setTokenChangeCallback(TokenChangeCallback cb) {
    std::lock_guard<std::mutex> lock{mu_};
    onTokenChange_ = std::move(cb);
}

void AuthSession::renewToken() {
    while (!stop_) {
        std::unique_lock<std::mutex> lock{mu_};
        const auto renewalDelay = std::max(
            std::chrono::duration_cast<std::chrono::seconds>(tokenLifetime_ * kRenewalFraction),
            std::chrono::seconds{1});
        cv_.wait_for(lock, renewalDelay, [this] { return stop_.load(); });
        if (stop_) {
            return;
        }
        lock.unlock();

        auto stub = auth_proto::AuthenticationService::NewStub(channel_);

        const auth_proto::Login_Parameters params =
            buildLoginParams(user_, password_, serverUuid_, requestedFeatures_);

        grpc::ClientContext context;
        auth_proto::Login_Responses response;
        const grpc::Status status = stub->Login(&context, params, &response);

        // Copy the callback and the new token under mu_, then invoke after
        // unlocking (same shape as ObservableCommandManager::removeExpired):
        // onTokenChange_ is user code that may call accessToken() /
        // isAuthenticated() / tokenLifetime(), all of which take this same
        // non-recursive mu_ and would deadlock the renewal thread.
        TokenChangeCallback callback;
        std::string newToken;
        {
            std::lock_guard<std::mutex> renewLock{mu_};
            if (!status.ok()) {
                accessToken_.clear();
                return;
            }

            accessToken_ = response.accesstoken().value();
            tokenLifetime_ = std::chrono::seconds{response.tokenlifetime().value()};
            tokenExpiry_ = std::chrono::steady_clock::now() + tokenLifetime_;

            callback = onTokenChange_;
            newToken = accessToken_;
        }

        if (callback) {
            callback(newToken);
        }
    }
}

}  // namespace sila2
