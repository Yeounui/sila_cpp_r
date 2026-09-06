// End-to-end tests for AuthSession's background renewal thread (§4.4,
// AuthSession.cc:118-152: renewToken()). test_auth_session_client_e2e.cc
// documents the renewal thread as out of scope because
// AuthenticationServiceImpl grants a hardcoded 3600s token lifetime (80% of
// that is a 2880s wait). This file makes renewal testable in real time by
// running a scripted fake AuthenticationService whose Login RPC returns a
// short, test-controlled token lifetime — AuthSession only depends on the
// service's response, not on AuthenticationServiceImpl specifically.
#include <sila/client/AuthSession.h>

#include "AuthenticationService.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using sila2::AuthSession;
// AuthenticationService.grpc.pb.h alone doesn't define the sila2::auth_proto
// alias (that lives in AuthenticationServiceImpl.h, which this file doesn't
// need since it drives a scripted fake service instead of the real impl).
namespace auth_proto = sila2::org::silastandard::core::authenticationservice::v1;

const std::string kServerUuid = "12345678-1234-1234-1234-123456789abc";

// One scripted Login RPC outcome: either a successful grant (with a
// caller-chosen token/lifetime) or an RPC-level failure.
struct ScriptedStep {
    bool ok = true;
    std::string token;
    std::chrono::seconds lifetime{0};
};

// Replays a fixed sequence of Login outcomes, one per call, in order: the
// first call is the initial login(), every subsequent call is a renewal
// attempt from AuthSession's background thread. Also records each request's
// requestedfeatures so tests can confirm renewal replays them unchanged.
class ScriptedAuthService final : public auth_proto::AuthenticationService::Service {
public:
    void pushStep(ScriptedStep step) {
        std::lock_guard<std::mutex> lock(mu_);
        steps_.push_back(std::move(step));
    }

    grpc::Status Login(grpc::ServerContext*, const auth_proto::Login_Parameters* request,
                        auth_proto::Login_Responses* response) override {
        std::lock_guard<std::mutex> lock(mu_);
        lastRequestedFeatures_.clear();
        for (const auto& feature : request->requestedfeatures()) {
            lastRequestedFeatures_.push_back(feature.value());
        }
        const std::size_t idx = callCount_++;
        if (idx >= steps_.size()) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no scripted step left");
        }
        const ScriptedStep& step = steps_[idx];
        if (!step.ok) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "scripted renewal failure");
        }
        response->mutable_accesstoken()->set_value(step.token);
        response->mutable_tokenlifetime()->set_value(step.lifetime.count());
        return grpc::Status::OK;
    }

    grpc::Status Logout(grpc::ServerContext*, const auth_proto::Logout_Parameters*,
                         auth_proto::Logout_Responses*) override {
        return grpc::Status::OK;
    }

    std::size_t callCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return callCount_;
    }

    std::vector<std::string> lastRequestedFeatures() const {
        std::lock_guard<std::mutex> lock(mu_);
        return lastRequestedFeatures_;
    }

private:
    mutable std::mutex mu_;
    std::vector<ScriptedStep> steps_;
    std::size_t callCount_ = 0;
    std::vector<std::string> lastRequestedFeatures_;
};

// Polls pred at a short fixed interval until it's true or timeout elapses;
// avoids a single blind sleep racing the renewal thread's own timing.
template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

class AuthSessionRenewalE2E : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    }

    void TearDown() override { server_->Shutdown(); }

    ScriptedAuthService service_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

// kRenewalFraction (AuthSession.cc) is 0.8: a 1s grant schedules the renewal
// attempt at ~0.8s.
TEST_F(AuthSessionRenewalE2E, RenewalSuccessRotatesTokenAndInvokesCallback) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{1}});   // initial login()
    service_.pushStep({true, "tok-b", std::chrono::seconds{5}});   // renewal, long enough to not re-fire

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    std::string callbackToken;
    session.setTokenChangeCallback([&](const std::string& newToken) { callbackToken = newToken; });

    ASSERT_TRUE(session.login());
    ASSERT_EQ(session.accessToken(), "tok-a");

    const bool renewed = waitUntil([&] { return session.accessToken() == "tok-b"; }, std::chrono::milliseconds(3000));

    EXPECT_TRUE(renewed);
    EXPECT_EQ(callbackToken, "tok-b");
    EXPECT_TRUE(session.isAuthenticated());

    session.logout();
}

TEST_F(AuthSessionRenewalE2E, RenewalLoopContinuesAcrossMultipleCycles) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{1}});  // initial login()
    service_.pushStep({true, "tok-b", std::chrono::seconds{1}});  // renewal 1
    service_.pushStep({true, "tok-c", std::chrono::seconds{5}});  // renewal 2

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login());

    const bool firstRenewed =
        waitUntil([&] { return session.accessToken() == "tok-b"; }, std::chrono::milliseconds(3000));
    ASSERT_TRUE(firstRenewed);

    const bool secondRenewed =
        waitUntil([&] { return session.accessToken() == "tok-c"; }, std::chrono::milliseconds(3000));

    EXPECT_TRUE(secondRenewed);
    EXPECT_EQ(service_.callCount(), 3u);

    session.logout();
}

TEST_F(AuthSessionRenewalE2E, RenewalReplaysOriginalRequestedFeatures) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{1}});
    service_.pushStep({true, "tok-b", std::chrono::seconds{5}});

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login({"featA", "featB"}));

    const bool renewed = waitUntil([&] { return service_.callCount() >= 2; }, std::chrono::milliseconds(3000));
    ASSERT_TRUE(renewed);

    const std::vector<std::string> expected{"featA", "featB"};
    EXPECT_EQ(service_.lastRequestedFeatures(), expected);

    session.logout();
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT: AuthSession.cc:133-135
// clears accessToken_ and lets the renewal thread exit whenever the renewal
// Login RPC itself fails; AuthSession.cc:126-128 exits the thread before
// issuing any RPC once stop_ is set.
// ---------------------------------------------------------------------------

TEST_F(AuthSessionRenewalE2E, FirstRenewalFailureClearsToken) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{1}});  // initial login()
    service_.pushStep({false, "", std::chrono::seconds{0}});      // renewal fails

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login());

    const bool cleared = waitUntil([&] { return session.accessToken().empty(); }, std::chrono::milliseconds(3000));

    EXPECT_TRUE(cleared);
    EXPECT_FALSE(session.isAuthenticated());

    session.logout();
}

TEST_F(AuthSessionRenewalE2E, LogoutBeforeRenewalDelayStopsThreadWithoutRpc) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{5}});  // renewal would fire at ~4s

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login());

    session.logout();  // well before the 4s renewal delay elapses

    EXPECT_EQ(service_.callCount(), 1u);  // only the initial login(), no renewal attempt
    EXPECT_TRUE(session.accessToken().empty());
    EXPECT_FALSE(session.isAuthenticated());
}

TEST_F(AuthSessionRenewalE2E, CallbackCanLogoutItsRenewalSession) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{1}});
    service_.pushStep({true, "tok-b", std::chrono::seconds{5}});
    service_.pushStep({true, "tok-c", std::chrono::seconds{5}});

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    std::atomic<bool> callbackReturned{false};
    session.setTokenChangeCallback([&](const std::string&) {
        session.logout();
        callbackReturned = true;
    });

    ASSERT_TRUE(session.login());
    ASSERT_TRUE(waitUntil([&] { return callbackReturned.load(); }, std::chrono::milliseconds(3000)));
    EXPECT_TRUE(session.accessToken().empty());
    EXPECT_FALSE(session.isAuthenticated());

    ASSERT_TRUE(session.login());
    session.logout();
}

TEST_F(AuthSessionRenewalE2E, ZeroLifetimeDoesNotSpinRenewals) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{0}});
    service_.pushStep({true, "tok-b", std::chrono::seconds{0}});

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login());
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    EXPECT_EQ(service_.callCount(), 1u);
    session.logout();
}

TEST_F(AuthSessionRenewalE2E, SecondRenewalFailureAfterFirstSuccessClearsToken) {
    service_.pushStep({true, "tok-a", std::chrono::seconds{1}});  // initial login()
    service_.pushStep({true, "tok-b", std::chrono::seconds{1}});  // renewal 1 succeeds
    service_.pushStep({false, "", std::chrono::seconds{0}});      // renewal 2 fails

    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login());

    const bool firstRenewed =
        waitUntil([&] { return session.accessToken() == "tok-b"; }, std::chrono::milliseconds(3000));
    ASSERT_TRUE(firstRenewed);

    const bool cleared = waitUntil([&] { return session.accessToken().empty(); }, std::chrono::milliseconds(3000));

    EXPECT_TRUE(cleared);
    EXPECT_FALSE(session.isAuthenticated());

    session.logout();
}

}  // namespace
