// End-to-end tests for the client-side sila2::AuthSession (architecture.md
// §4.4): login()/logout()/isAuthenticated()/accessToken() driven against a
// real in-process AuthenticationServiceImpl over a real gRPC channel, since
// AuthSession builds its own stub and makes real RPCs rather than taking an
// injectable stub. The background renewal thread (fires at 80% of the
// hardcoded 3600s token lifetime) is out of scope — see note above the test
// bodies below.
#include <sila/client/AuthSession.h>
#include <sila/server/features/AuthenticationServiceImpl.h>

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/config/ServerConfig.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using sila2::AuthenticationServiceImpl;
using sila2::AuthSession;
using sila2::InMemoryServerConfig;
using sila2::auth::AccessPolicy;
using sila2::auth::AuthTokenStore;
using sila2::auth::CredentialVerifier;

const std::string kServerUuid = "12345678-1234-1234-1234-123456789abc";
const std::string kFeature1 = "org.silastandard/core/Feature1/v1";
const std::string kFeature2 = "org.silastandard/core/Feature2/v1";

class MockCredentialVerifier : public CredentialVerifier {
public:
    std::optional<std::string> verify(const std::string& user, const std::string& password) override {
        if (user == "alice" && password == "secret") return "alice";
        return std::nullopt;
    }
};

class MockAccessPolicy : public AccessPolicy {
public:
    bool isAllowed(const std::string& /*user*/, const std::string& /*fqi*/) const override { return true; }
    std::vector<std::string> allowedFqis(const std::string& /*user*/) const override {
        return {kFeature1, kFeature2};
    }
};

// Real local server running AuthenticationServiceImpl so AuthSession's
// internally-created stub (it does not accept an injected one) exercises the
// actual RPC wire path.
class AuthSessionClientE2E : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&authService_);
        server_ = builder.BuildAndStart();
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    }

    void TearDown() override { server_->Shutdown(); }

    AuthTokenStore store_;
    MockCredentialVerifier verifier_;
    MockAccessPolicy policy_;
    InMemoryServerConfig config_{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService_{store_, verifier_, policy_, config_};

    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST_F(AuthSessionClientE2E, LoginSucceedsWithValidCredentials) {
    AuthSession session{"alice", "secret", kServerUuid, channel_};

    const bool result = session.login();

    EXPECT_TRUE(result);
    EXPECT_TRUE(session.isAuthenticated());
    EXPECT_FALSE(session.accessToken().empty());
    EXPECT_EQ(session.tokenLifetime(), std::chrono::seconds{3600});

    session.logout();  // stop the renewal thread before the fixture tears down the server
}

TEST_F(AuthSessionClientE2E, LoginWithRequestedFeaturesSucceeds) {
    AuthSession session{"alice", "secret", kServerUuid, channel_};

    const bool result = session.login({kFeature1});

    EXPECT_TRUE(result);
    EXPECT_TRUE(session.isAuthenticated());
    // Validate on the server-side store that the token is scoped to the
    // requested feature, not just that login() returned true.
    const auto entry = store_.validate(session.accessToken(), kFeature1);
    EXPECT_TRUE(entry.has_value());

    session.logout();
}

TEST_F(AuthSessionClientE2E, LogoutThenReloginSucceeds) {
    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login());
    const std::string firstToken = session.accessToken();

    session.logout();
    const bool result = session.login();

    EXPECT_TRUE(result);
    EXPECT_TRUE(session.isAuthenticated());
    EXPECT_NE(session.accessToken(), firstToken);

    session.logout();
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT: login() surfaces the
// server's rejection as a plain `false` return (AuthSession does not
// propagate the underlying grpc::Status), and the object never transitions
// into an authenticated state.
// ---------------------------------------------------------------------------

TEST_F(AuthSessionClientE2E, BeforeLoginNotAuthenticated) {
    AuthSession session{"alice", "secret", kServerUuid, channel_};

    EXPECT_FALSE(session.isAuthenticated());
    EXPECT_TRUE(session.accessToken().empty());
}

TEST_F(AuthSessionClientE2E, LoginWithInvalidCredentialsFails) {
    AuthSession session{"alice", "wrong-password", kServerUuid, channel_};

    const bool result = session.login();

    EXPECT_FALSE(result);
    EXPECT_FALSE(session.isAuthenticated());
    EXPECT_TRUE(session.accessToken().empty());
}

TEST_F(AuthSessionClientE2E, LogoutClearsSession) {
    AuthSession session{"alice", "secret", kServerUuid, channel_};
    ASSERT_TRUE(session.login());
    ASSERT_TRUE(session.isAuthenticated());

    session.logout();

    EXPECT_FALSE(session.isAuthenticated());
    EXPECT_TRUE(session.accessToken().empty());
    EXPECT_EQ(session.tokenLifetime(), std::chrono::seconds{0});
}

// Note: the background renewal thread wakes at 80% of tokenLifetime, which is
// 2880s with the server's hardcoded 3600s grant — too long to exercise here
// without changing the implementation. Not tested; see CLAUDE.md §22.

}  // namespace
