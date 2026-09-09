// End-to-end tests for the SiLA 2 authentication session lifecycle: Login
// issues a token that AuthorizationInterceptor accepts (Flow 1), and
// AuthorizationConfigurationServiceImpl::SetAuthorizationProvider invalidates
// every token sharing that AuthTokenStore, whether locally issued before or
// after the provider change (Flow 2). All three components — Authentication,
// AuthorizationConfiguration, and the interceptor — are wired to the same
// store, the way a real server would share it (architecture.md §3.11).
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/features/AuthorizationConfigurationServiceImpl.h>

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/transport/CallContext.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <optional>
#include <string>
#include <vector>

namespace
{
using sila2::CallContext;
using sila2::AuthenticationServiceImpl;
using sila2::AuthorizationConfigurationServiceImpl;
using sila2::InMemoryServerConfig;
using sila2::auth::AccessPolicy;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;
using sila2::auth::CredentialVerifier;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::SilaError;

namespace auth_proto = sila2::org::silastandard::core::authenticationservice::v1;
namespace authzconfig_proto = sila2::org::silastandard::core::authorizationconfigurationservice::v1;

const std::string kServerUuid = "12345678-1234-1234-1234-123456789abc";
const std::string kFeature1 = "org.silastandard/core/Feature1/v1";
const std::string kFeature2 = "org.silastandard/core/Feature2/v1";
const std::string kUnrelatedFeature = "org.silastandard/core/Unrelated/v1";
const std::string kInvalidAccessTokenErrorId =
    "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken";

class MockCredentialVerifier : public CredentialVerifier {
public:
    std::optional<std::string> verify(const std::string& user, const std::string& password) override {
        if (user == "alice" && password == "secret") return "alice";
        if (user == "bob" && password == "bobsecret") return "bob";
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

// Both Feature1 and Feature2 are protected; everything else passes the
// interceptor unchecked.
bool isProtected(const std::string& fqi) {
    return fqi == kFeature1 || fqi == kFeature2;
}

// Builds a Login_Parameters with valid credentials for `user` and a
// server UUID matching kServerUuid; RequestedFeatures is left empty for
// the caller to fill in as needed.
auth_proto::Login_Parameters makeLoginRequest(const std::string& user, const std::string& password) {
    auth_proto::Login_Parameters request;
    request.mutable_useridentification()->set_value(user);
    request.mutable_password()->set_value(password);
    request.mutable_requestedserver()->set_value(kServerUuid);
    return request;
}

// Runs Login through `service` and returns the issued access token.
// Asserts the RPC succeeded — a helper failure here would otherwise be
// silently swallowed by the caller reading an empty token string.
std::string login(AuthenticationServiceImpl& service, const auth_proto::Login_Parameters& request) {
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;
    const grpc::Status status = service.Login(&ctx, &request, &response);
    EXPECT_TRUE(status.ok());
    return response.accesstoken().value();
}

// ---------------------------------------------------------------------------
// Flow 1: Login -> Interceptor — True paths
// ---------------------------------------------------------------------------

TEST(AuthSessionE2E, LoginThenInterceptAllowsProtectedFqi) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature1));
    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature2));
}

TEST(AuthSessionE2E, LoginScopedFeatureThenInterceptAllowsMatchingFqi) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    auth_proto::Login_Parameters request = makeLoginRequest("alice", "secret");
    request.add_requestedfeatures()->set_value(kFeature1);
    const std::string token = login(authService, request);
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature1));
}

TEST(AuthSessionE2E, LoginLogoutLoginRenewsSession) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::string firstToken = login(authService, makeLoginRequest("alice", "secret"));
    auth_proto::Logout_Parameters logoutRequest;
    logoutRequest.mutable_accesstoken()->set_value(firstToken);
    auth_proto::Logout_Responses logoutResponse;
    grpc::ServerContext logoutCtx;
    ASSERT_TRUE(authService.Logout(&logoutCtx, &logoutRequest, &logoutResponse).ok());

    const std::string secondToken = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", secondToken);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature1));
}

// ---------------------------------------------------------------------------
// Flow 1: Login -> Interceptor — False paths
// ---------------------------------------------------------------------------

// CAUGHT: Logout removes the token from the shared store, so the interceptor
// rejects it on the next call.
TEST(AuthSessionE2E, LoginLogoutThenInterceptRejectsOldToken) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    auth_proto::Logout_Parameters logoutRequest;
    logoutRequest.mutable_accesstoken()->set_value(token);
    auth_proto::Logout_Responses logoutResponse;
    grpc::ServerContext logoutCtx;
    ASSERT_TRUE(authService.Logout(&logoutCtx, &logoutRequest, &logoutResponse).ok());

    CallContext ctx;
    ctx.setMetadata("access-token", token);
    try {
        interceptor.intercept(ctx, kFeature1);
        FAIL() << "expected DefinedExecutionError";
    } catch (const DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(), kInvalidAccessTokenErrorId);
    }
}

// CAUGHT: a token scoped to Feature1 at Login time is not in the FQI set
// AuthTokenStore::validate() checks for Feature2.
TEST(AuthSessionE2E, LoginScopedThenInterceptRejectsUnauthorizedFqi) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    auth_proto::Login_Parameters request = makeLoginRequest("alice", "secret");
    request.add_requestedfeatures()->set_value(kFeature1);
    const std::string token = login(authService, request);
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    try {
        interceptor.intercept(ctx, kFeature2);
        FAIL() << "expected DefinedExecutionError";
    } catch (const DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(), kInvalidAccessTokenErrorId);
    }
}

// CAUGHT: RequestedFeatures naming an FQI outside the policy's allowed set
// is silently dropped by Login (intersection filter), so the issued token
// carries an empty FQI set — every protected FQI is then rejected.
TEST(AuthSessionE2E, LoginNonOverlappingFeaturesThenInterceptRejectsAll) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    auth_proto::Login_Parameters request = makeLoginRequest("alice", "secret");
    request.add_requestedfeatures()->set_value(kUnrelatedFeature);
    const std::string token = login(authService, request);
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    try {
        interceptor.intercept(ctx, kFeature1);
        FAIL() << "expected DefinedExecutionError";
    } catch (const DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(), kInvalidAccessTokenErrorId);
    }
}

// ---------------------------------------------------------------------------
// Flow 2: SetAuthorizationProvider -> Token Invalidation — True paths
// ---------------------------------------------------------------------------

TEST(AuthSessionE2E, SetProviderThenLoginProducesValidSession) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationConfigurationServiceImpl authzConfigService{store, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    authzconfig_proto::SetAuthorizationProvider_Parameters providerRequest;
    providerRequest.mutable_authorizationprovider()->set_value("87654321-4321-4321-4321-cba987654321");
    authzconfig_proto::SetAuthorizationProvider_Responses providerResponse;
    grpc::ServerContext providerCtx;
    ASSERT_TRUE(authzConfigService.SetAuthorizationProvider(&providerCtx, &providerRequest, &providerResponse).ok());

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature1));
}

TEST(AuthSessionE2E, MultipleProviderChangesThenLoginStillWorks) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationConfigurationServiceImpl authzConfigService{store, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    for (const std::string& providerUuid :
         {std::string{"aaaaaaaa-0000-0000-0000-000000000000"}, std::string{"bbbbbbbb-0000-0000-0000-000000000000"}}) {
        authzconfig_proto::SetAuthorizationProvider_Parameters providerRequest;
        providerRequest.mutable_authorizationprovider()->set_value(providerUuid);
        authzconfig_proto::SetAuthorizationProvider_Responses providerResponse;
        grpc::ServerContext providerCtx;
        ASSERT_TRUE(authzConfigService.SetAuthorizationProvider(&providerCtx, &providerRequest, &providerResponse).ok());
    }

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature1));
}

TEST(AuthSessionE2E, SetProviderToSelfThenLoginWorks) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationConfigurationServiceImpl authzConfigService{store, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    // Setting the provider to this server's own UUID is a degenerate but
    // valid configuration (self-authorization) — SetAuthorizationProvider
    // does not reject it.
    authzconfig_proto::SetAuthorizationProvider_Parameters providerRequest;
    providerRequest.mutable_authorizationprovider()->set_value(kServerUuid);
    authzconfig_proto::SetAuthorizationProvider_Responses providerResponse;
    grpc::ServerContext providerCtx;
    ASSERT_TRUE(authzConfigService.SetAuthorizationProvider(&providerCtx, &providerRequest, &providerResponse).ok());

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature1));
}

// ---------------------------------------------------------------------------
// Flow 2: SetAuthorizationProvider -> Token Invalidation — False paths
// ---------------------------------------------------------------------------

// CAUGHT: SetAuthorizationProvider calls AuthTokenStore::clear() (§3.11),
// wiping a token that was valid moments earlier.
TEST(AuthSessionE2E, LoginThenSetProviderInvalidatesToken) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationConfigurationServiceImpl authzConfigService{store, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", token);
    ASSERT_NO_THROW(interceptor.intercept(ctx, kFeature1));

    authzconfig_proto::SetAuthorizationProvider_Parameters providerRequest;
    providerRequest.mutable_authorizationprovider()->set_value("87654321-4321-4321-4321-cba987654321");
    authzconfig_proto::SetAuthorizationProvider_Responses providerResponse;
    grpc::ServerContext providerCtx;
    ASSERT_TRUE(authzConfigService.SetAuthorizationProvider(&providerCtx, &providerRequest, &providerResponse).ok());

    try {
        interceptor.intercept(ctx, kFeature1);
        FAIL() << "expected DefinedExecutionError";
    } catch (const DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(), kInvalidAccessTokenErrorId);
    }
}

// CAUGHT: a provider change is a bulk invalidation — every session sharing
// the store is rejected, not just the one that triggered the change.
TEST(AuthSessionE2E, MultipleLoginsAllInvalidatedByProviderChange) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationConfigurationServiceImpl authzConfigService{store, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::string aliceToken = login(authService, makeLoginRequest("alice", "secret"));
    const std::string bobToken = login(authService, makeLoginRequest("bob", "bobsecret"));
    ASSERT_EQ(store.size(), 2u);

    authzconfig_proto::SetAuthorizationProvider_Parameters providerRequest;
    providerRequest.mutable_authorizationprovider()->set_value("87654321-4321-4321-4321-cba987654321");
    authzconfig_proto::SetAuthorizationProvider_Responses providerResponse;
    grpc::ServerContext providerCtx;
    ASSERT_TRUE(authzConfigService.SetAuthorizationProvider(&providerCtx, &providerRequest, &providerResponse).ok());

    for (const std::string& token : {aliceToken, bobToken}) {
        CallContext ctx;
        ctx.setMetadata("access-token", token);
        try {
            interceptor.intercept(ctx, kFeature1);
            FAIL() << "expected DefinedExecutionError for token " << token;
        } catch (const DefinedExecutionError& e) {
            EXPECT_EQ(e.errorIdentifier(), kInvalidAccessTokenErrorId);
        }
    }
}

// CAUGHT: a provider change also removes the token Logout would otherwise
// find, so Logout on the now-stale token reports InvalidAccessToken instead
// of succeeding.
TEST(AuthSessionE2E, LoginThenSetProviderThenLogoutOldTokenFails) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationConfigurationServiceImpl authzConfigService{store, config};

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));

    authzconfig_proto::SetAuthorizationProvider_Parameters providerRequest;
    providerRequest.mutable_authorizationprovider()->set_value("87654321-4321-4321-4321-cba987654321");
    authzconfig_proto::SetAuthorizationProvider_Responses providerResponse;
    grpc::ServerContext providerCtx;
    ASSERT_TRUE(authzConfigService.SetAuthorizationProvider(&providerCtx, &providerRequest, &providerResponse).ok());

    auth_proto::Logout_Parameters logoutRequest;
    logoutRequest.mutable_accesstoken()->set_value(token);
    auth_proto::Logout_Responses logoutResponse;
    grpc::ServerContext logoutCtx;
    const grpc::Status status = authService.Logout(&logoutCtx, &logoutRequest, &logoutResponse);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(),
              "org.silastandard/core/AuthenticationService/v1/DefinedExecutionError/InvalidAccessToken");
}

}  // namespace
