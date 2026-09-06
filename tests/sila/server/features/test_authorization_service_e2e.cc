// End-to-end tests for the "Discovery -> Authentication -> Authorization"
// flow (architecture.md §3.11): a client first calls
// AuthorizationServiceImpl::Get_FCPAffectedByMetadata_AccessToken to learn
// which FQIs require an access token, then obtains a token via
// AuthenticationServiceImpl::Login scoped to (some of) those FQIs, and
// finally AuthorizationInterceptor enforces that scope on the call path.
// All three components share the same AccessPolicy/AuthTokenStore, the way
// a real server would wire them.
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/features/AuthorizationServiceImpl.h>

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/transport/CallContext.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
using sila2::AuthenticationServiceImpl;
using sila2::AuthorizationServiceImpl;
using sila2::CallContext;
using sila2::InMemoryServerConfig;
using sila2::auth::AccessPolicy;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;
using sila2::auth::CredentialVerifier;
using sila2::error::DefinedExecutionError;
using sila2::error::FrameworkError;

namespace auth_proto = sila2::org::silastandard::core::authenticationservice::v1;
namespace authz_proto = sila2::org::silastandard::core::authorizationservice::v1;

const std::string kServerUuid = "12345678-1234-1234-1234-123456789abc";
const std::string kFeature1 = "org.silastandard/core/Feature1/v1";
const std::string kFeature2 = "org.silastandard/core/Feature2/v1";
// The one list all three parties read, the way a real server derives all
// three from Builder::WithAuthentication's protectedFqis: discovery
// (AuthorizationServiceImpl), the gate (isProtected), and the token scope
// Login grants (MockAccessPolicy).
const std::vector<std::string> kProtectedFqis{kFeature1, kFeature2};
// Absent from kProtectedFqis, so discovery never reports it and
// isProtected() below treats it as unprotected.
const std::string kUnprotectedFeature = "org.silastandard/core/Unprotected/v1";
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
        return kProtectedFqis;
    }
};

// Reads the same kProtectedFqis discovery reports, the way a real server
// derives the gate predicate from the list it was configured with.
bool isProtected(const std::string& fqi) {
    return std::find(kProtectedFqis.begin(), kProtectedFqis.end(), fqi)
           != kProtectedFqis.end();
}

// Runs Get_FCPAffectedByMetadata_AccessToken and returns the affected FQIs
// as a set — this is the discovery step every test in this file starts
// with, standing in for a client that has not yet hard-coded which FQIs
// need a token.
std::unordered_set<std::string> discoverAffectedFqis(AuthorizationServiceImpl& service) {
    authz_proto::Get_FCPAffectedByMetadata_AccessToken_Parameters request;
    authz_proto::Get_FCPAffectedByMetadata_AccessToken_Responses response;
    grpc::ServerContext ctx;
    const grpc::Status status = service.Get_FCPAffectedByMetadata_AccessToken(&ctx, &request, &response);
    EXPECT_TRUE(status.ok());

    std::unordered_set<std::string> fqis;
    for (const auto& call : response.affectedcalls()) {
        fqis.insert(call.value());
    }
    return fqis;
}

// Builds a Login_Parameters with valid credentials for `user` and a server
// UUID matching kServerUuid; RequestedFeatures is left empty for the caller
// to fill in as needed.
auth_proto::Login_Parameters makeLoginRequest(const std::string& user, const std::string& password) {
    auth_proto::Login_Parameters request;
    request.mutable_useridentification()->set_value(user);
    request.mutable_password()->set_value(password);
    request.mutable_requestedserver()->set_value(kServerUuid);
    return request;
}

// Runs Login through `service` and returns the issued access token.
std::string login(AuthenticationServiceImpl& service, const auth_proto::Login_Parameters& request) {
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;
    const grpc::Status status = service.Login(&ctx, &request, &response);
    EXPECT_TRUE(status.ok());
    return response.accesstoken().value();
}

// ---------------------------------------------------------------------------
// Discovery -> Authentication -> Authorization — True paths
// ---------------------------------------------------------------------------

TEST(AuthorizationServiceE2E, DiscoveryThenLoginThenInterceptAcceptsAffectedFqi) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationServiceImpl authzService{kProtectedFqis};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::unordered_set<std::string> affected = discoverAffectedFqis(authzService);
    ASSERT_EQ(affected, (std::unordered_set<std::string>{kFeature1, kFeature2}));

    // Scope the token to one of the FQIs discovery just reported.
    auth_proto::Login_Parameters request = makeLoginRequest("alice", "secret");
    request.add_requestedfeatures()->set_value(kFeature1);
    const std::string token = login(authService, request);
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kFeature1));
}

TEST(AuthorizationServiceE2E, DiscoveryThenLoginAllFeaturesThenInterceptAcceptsEach) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationServiceImpl authzService{kProtectedFqis};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::unordered_set<std::string> affected = discoverAffectedFqis(authzService);
    ASSERT_EQ(affected, (std::unordered_set<std::string>{kFeature1, kFeature2}));

    // Empty RequestedFeatures grants every FQI the policy allows (Login's
    // "all features" branch), so the interceptor should accept every FQI
    // discovery reported, not just one.
    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", token);

    for (const std::string& fqi : affected) {
        EXPECT_NO_THROW(interceptor.intercept(ctx, fqi));
    }
}

TEST(AuthorizationServiceE2E, UnaffectedFqiPassesInterceptWithoutToken) {
    AuthTokenStore store;
    AuthorizationServiceImpl authzService{kProtectedFqis};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::unordered_set<std::string> affected = discoverAffectedFqis(authzService);
    ASSERT_EQ(affected.count(kUnprotectedFeature), 0u);

    // No Login, no access-token metadata at all — an FQI discovery never
    // listed as affected must still pass the interceptor.
    CallContext ctx;
    EXPECT_NO_THROW(interceptor.intercept(ctx, kUnprotectedFeature));
}

// ---------------------------------------------------------------------------
// Discovery -> Authentication -> Authorization — False paths
// ---------------------------------------------------------------------------

// CAUGHT: discovery reports Feature1 as requiring a token; calling it with
// no access-token metadata at all is rejected by the interceptor.
TEST(AuthorizationServiceE2E, AffectedFqiRejectedWithoutToken) {
    AuthTokenStore store;
    AuthorizationServiceImpl authzService{kProtectedFqis};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::unordered_set<std::string> affected = discoverAffectedFqis(authzService);
    ASSERT_EQ(affected.count(kFeature1), 1u);

    CallContext ctx;
    try {
        interceptor.intercept(ctx, kFeature1);
        FAIL() << "expected FrameworkError";
    } catch (const FrameworkError& e) {
        EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
    }
}

// CAUGHT: discovery reports both features as affected, but the token was
// scoped to Feature2 only at Login time — the interceptor rejects Feature1
// even though it is a token-protected FQI the client already knew about.
TEST(AuthorizationServiceE2E, AffectedFqiRejectedWithWrongScopeToken) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationServiceImpl authzService{kProtectedFqis};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::unordered_set<std::string> affected = discoverAffectedFqis(authzService);
    ASSERT_EQ(affected, (std::unordered_set<std::string>{kFeature1, kFeature2}));

    auth_proto::Login_Parameters request = makeLoginRequest("alice", "secret");
    request.add_requestedfeatures()->set_value(kFeature2);
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

// CAUGHT: Logout removes the token from the shared store, so a call to an
// FQI discovery reported as affected is rejected on the next call, even
// though the token was valid moments earlier.
TEST(AuthorizationServiceE2E, AffectedFqiRejectedAfterLogout) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationServiceImpl authzService{kProtectedFqis};
    AuthenticationServiceImpl authService{store, verifier, policy, config};
    AuthorizationInterceptor interceptor{store, isProtected};

    const std::unordered_set<std::string> affected = discoverAffectedFqis(authzService);
    ASSERT_EQ(affected.count(kFeature1), 1u);

    const std::string token = login(authService, makeLoginRequest("alice", "secret"));
    CallContext ctx;
    ctx.setMetadata("access-token", token);
    ASSERT_NO_THROW(interceptor.intercept(ctx, kFeature1));

    auth_proto::Logout_Parameters logoutRequest;
    logoutRequest.mutable_accesstoken()->set_value(token);
    auth_proto::Logout_Responses logoutResponse;
    grpc::ServerContext logoutCtx;
    ASSERT_TRUE(authService.Logout(&logoutCtx, &logoutRequest, &logoutResponse).ok());

    try {
        interceptor.intercept(ctx, kFeature1);
        FAIL() << "expected DefinedExecutionError";
    } catch (const DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(), kInvalidAccessTokenErrorId);
    }
}

}  // namespace
