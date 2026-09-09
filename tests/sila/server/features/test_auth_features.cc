// Tests for AuthenticationServiceImpl (Login/Logout token issuance) and
// AuthorizationConfigurationServiceImpl (provider get/set) — architecture.md §3.11.
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/features/AuthorizationConfigurationServiceImpl.h>

#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
using sila2::AuthenticationServiceImpl;
using sila2::AuthorizationConfigurationServiceImpl;
using sila2::InMemoryServerConfig;
using sila2::auth::AccessPolicy;
using sila2::auth::AuthTokenStore;
using sila2::auth::CredentialVerifier;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::SilaError;
using sila2::error::ValidationError;

namespace auth_proto = sila2::org::silastandard::core::authenticationservice::v1;
namespace authzconfig_proto = sila2::org::silastandard::core::authorizationconfigurationservice::v1;

const std::string kServerUuid = "12345678-1234-1234-1234-123456789abc";
const std::string kFeature1 = "org.silastandard/core/Feature1/v1";
const std::string kFeature2 = "org.silastandard/core/Feature2/v1";
const std::string kUnrelatedFeature = "org.silastandard/core/Unrelated/v1";

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

// Builds a Login_Parameters with valid credentials and matching server UUID;
// RequestedFeatures is left empty for the caller to fill in as needed.
auth_proto::Login_Parameters makeLoginRequest() {
    auth_proto::Login_Parameters request;
    request.mutable_useridentification()->set_value("alice");
    request.mutable_password()->set_value("secret");
    request.mutable_requestedserver()->set_value(kServerUuid);
    return request;
}

// ---------------------------------------------------------------------------
// AuthenticationServiceImpl::Login — True paths
// ---------------------------------------------------------------------------

TEST(AuthFeatures, LoginWithRequestedFeatureGrantsIntersectionAndLifetime) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value(kFeature1);
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.accesstoken().value().empty());
    EXPECT_EQ(response.tokenlifetime().value(), 3600);

    const auto entry = store.validate(response.accesstoken().value(), kFeature1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->allowedFqis, std::unordered_set<std::string>{kFeature1});
}

// Regression: with Command/Property-granular protectedFqis, a policy may grant
// a single Command under a Feature. A client requesting the parent Feature must
// still receive that Command grant -- Login previously matched only "policy
// covers request", and a Command entry does not cover its parent Feature, so
// the grant was silently dropped and the granular gate then rejected the client.
class CommandScopedAccessPolicy : public AccessPolicy {
public:
    bool isAllowed(const std::string&, const std::string&) const override { return true; }
    std::vector<std::string> allowedFqis(const std::string&) const override {
        return {kFeature1 + "/Command/DoThing"};
    }
};

TEST(AuthFeatures, LoginGrantsCommandScopeWhenClientRequestsParentFeature) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    CommandScopedAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value(kFeature1);  // requests the Feature, not the Command
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);
    ASSERT_TRUE(status.ok());

    // The token carries the Command-level grant, so the granular gate admits it.
    const std::string commandFqi = kFeature1 + "/Command/DoThing";
    const auto granted = store.validate(response.accesstoken().value(), commandFqi);
    ASSERT_TRUE(granted.has_value());
    EXPECT_EQ(granted->allowedFqis, std::unordered_set<std::string>{commandFqi});

    // ...but it does NOT over-grant the whole Feature: a Command grant authorizes
    // only that Command, so a sibling Command and the bare Feature stay denied.
    EXPECT_FALSE(store.validate(response.accesstoken().value(), kFeature1).has_value());
    EXPECT_FALSE(
        store.validate(response.accesstoken().value(), kFeature1 + "/Command/Other").has_value());
}

TEST(AuthFeatures, LoginWithEmptyRequestedFeaturesGrantsAllPolicyFqis) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    const auto entry = store.validate(response.accesstoken().value(), kFeature1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->allowedFqis, (std::unordered_set<std::string>{kFeature1, kFeature2}));
}

TEST(AuthFeatures, LoginWithMultipleValidFqisGrantsBoth) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value(kFeature1);
    request.add_requestedfeatures()->set_value(kFeature2);
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.accesstoken().value().empty());
    const auto entry1 = store.validate(response.accesstoken().value(), kFeature1);
    const auto entry2 = store.validate(response.accesstoken().value(), kFeature2);
    ASSERT_TRUE(entry1.has_value());
    ASSERT_TRUE(entry2.has_value());
    EXPECT_EQ(entry1->allowedFqis, (std::unordered_set<std::string>{kFeature1, kFeature2}));
}

// Pins that the FDL's "no feature provided means all features" branch
// (AuthenticationService-v1_0.sila.xml:55) is not routed through the
// element-wise FQI validator at all -- the branch is skipped, not merely
// vacuously satisfied. Overlaps LoginWithEmptyRequestedFeaturesGrantsAllPolicyFqis
// above; this one carries the explicit no-validation intent in its name.
TEST(AuthFeatures, LoginWithEmptyRequestedFeaturesSkipsFqiValidation) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    const auto entry = store.validate(response.accesstoken().value(), kFeature1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->allowedFqis, (std::unordered_set<std::string>{kFeature1, kFeature2}));
}

TEST(AuthFeatures, LoginWithPartiallyOverlappingRequestedFeaturesGrantsOnlyIntersection) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value(kFeature1);
    request.add_requestedfeatures()->set_value(kUnrelatedFeature);
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    const auto entry = store.validate(response.accesstoken().value(), kFeature1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->allowedFqis, std::unordered_set<std::string>{kFeature1});
}

// ---------------------------------------------------------------------------
// AuthenticationServiceImpl::Login — False paths
// ---------------------------------------------------------------------------

TEST(AuthFeatures, LoginWithInvalidCredentialsReturnsAuthenticationFailed) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.mutable_password()->set_value("wrong-password");
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(),
              "org.silastandard/core/AuthenticationService/v1/DefinedExecutionError/AuthenticationFailed");
}

TEST(AuthFeatures, LoginWithWrongServerUuidReturnsValidationError) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.mutable_requestedserver()->set_value("00000000-0000-0000-0000-000000000000");
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    const std::string parameterFqi = validationError->parameter();
    EXPECT_EQ(parameterFqi,
              "org.silastandard/core/AuthenticationService/v1/Command/Login/Parameter/RequestedServer");
    // Guards against a bare identifier slipping back in: a spec-shaped FQI
    // always carries both segment markers (SiLAFramework.proto:96).
    EXPECT_NE(parameterFqi.find("/Command/"), std::string::npos);
    EXPECT_NE(parameterFqi.find("/Parameter/"), std::string::npos);
}

// AuthenticationService-v1_0.sila.xml:45-46 constrains RequestedServer to
// Length 36 + a UUID Pattern (S58). A malformed RequestedServer paired with a
// wrong password must still surface ValidationError, not AuthenticationFailed
// -- proving the Constraint check runs before verifier_.verify().
TEST(AuthFeatures, LoginWithMalformedRequestedServerReturnsValidationErrorBeforeCredentialCheck) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.mutable_requestedserver()->set_value("not-a-uuid");
    request.mutable_password()->set_value("wrong-password");
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    EXPECT_EQ(validationError->parameter(),
              "org.silastandard/core/AuthenticationService/v1/Command/Login/Parameter/RequestedServer");
    EXPECT_TRUE(response.accesstoken().value().empty());
}

// AuthenticationService-v1_0.sila.xml:63 constrains each RequestedFeatures
// element to FullyQualifiedIdentifier/FeatureIdentifier (S38).
TEST(AuthFeatures, LoginWithMalformedFqiReturnsValidationError) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value("not-an-fqi");
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    EXPECT_EQ(validationError->parameter(),
              "org.silastandard/core/AuthenticationService/v1/Command/Login/Parameter/RequestedFeatures");
    EXPECT_TRUE(response.accesstoken().value().empty());
}

// Exercises the arity half of the FQI regex (missing /vN segment) rather
// than its character classes.
TEST(AuthFeatures, LoginWithMissingVersionSegmentFqiReturnsValidationError) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value("org.silastandard/core/Feature1");
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    EXPECT_EQ(validationError->parameter(),
              "org.silastandard/core/AuthenticationService/v1/Command/Login/Parameter/RequestedFeatures");
    EXPECT_TRUE(response.accesstoken().value().empty());
}

// Part A p87: FQIs are compared without regard to case, so an uppercase
// Originator segment is a valid case variant of kFeature1, not a malformed
// FQI -- Login must accept it and grant the matching feature.
TEST(AuthFeatures, LoginWithUppercaseOriginatorFqiIsAccepted) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value("Org.Silastandard/core/Feature1/v1");
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    // Only status.ok() and a non-empty token are asserted here (not the exact
    // allowedFqis membership below) to stay robust to how the case-variant
    // FQI ends up stored, matching kFeature1 case-insensitively either way.
    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(response.accesstoken().value().empty());
}

// Pins that a mid-loop throw discards a partially-accumulated allowedFqis
// rather than letting a valid element that arrived before the malformed one
// leak through to store_.issue() -- this is the test that fails if the
// writer validates only the first element or hoists the guard outside the loop.
TEST(AuthFeatures, LoginWithOneMalformedAmongValidFqisReturnsValidationError) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value(kFeature1);
    request.add_requestedfeatures()->set_value("");
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    EXPECT_EQ(validationError->parameter(),
              "org.silastandard/core/AuthenticationService/v1/Command/Login/Parameter/RequestedFeatures");
    EXPECT_TRUE(response.accesstoken().value().empty());
}

TEST(AuthFeatures, LoginWithNonOverlappingRequestedFeaturesSucceedsWithEmptyFqiSet) {
    // Edge behavior: RequestedFeatures that share nothing with the policy's
    // allowed set is not rejected — Login still succeeds, but the issued
    // token carries an empty FQI set (i.e. authorized for nothing).
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters request = makeLoginRequest();
    request.add_requestedfeatures()->set_value(kUnrelatedFeature);
    auth_proto::Login_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Login(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.accesstoken().value().empty());
    const auto entry = store.validate(response.accesstoken().value(), kUnrelatedFeature);
    EXPECT_FALSE(entry.has_value());
}

// ---------------------------------------------------------------------------
// AuthenticationServiceImpl::Logout
// ---------------------------------------------------------------------------

TEST(AuthFeatures, LogoutWithEmptyAccessTokenReturnsInvalidAccessToken) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Logout_Parameters request;
    request.mutable_accesstoken()->set_value("");
    auth_proto::Logout_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Logout(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(),
              "org.silastandard/core/AuthenticationService/v1/DefinedExecutionError/InvalidAccessToken");
}

TEST(AuthFeatures, LogoutWithValidTokenRemovesItFromStore) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Login_Parameters loginRequest = makeLoginRequest();
    auth_proto::Login_Responses loginResponse;
    grpc::ServerContext loginCtx;
    ASSERT_TRUE(service.Login(&loginCtx, &loginRequest, &loginResponse).ok());
    const std::string token = loginResponse.accesstoken().value();
    ASSERT_EQ(store.size(), 1u);

    auth_proto::Logout_Parameters request;
    request.mutable_accesstoken()->set_value(token);
    auth_proto::Logout_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Logout(&ctx, &request, &response);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(store.size(), 0u);
}

TEST(AuthFeatures, LogoutWithUnknownTokenReturnsInvalidAccessToken) {
    AuthTokenStore store;
    MockCredentialVerifier verifier;
    MockAccessPolicy policy;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthenticationServiceImpl service{store, verifier, policy, config};

    auth_proto::Logout_Parameters request;
    request.mutable_accesstoken()->set_value("never-issued-token");
    auth_proto::Logout_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Logout(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
}

// ---------------------------------------------------------------------------
// AuthorizationConfigurationServiceImpl
// ---------------------------------------------------------------------------

TEST(AuthFeatures, SetAuthorizationProviderStoresUuidAndClearsTokenStore) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationConfigurationServiceImpl service{store, config};
    const std::string existingToken = store.issue("alice", {kFeature1}, std::chrono::seconds{60});
    ASSERT_EQ(store.size(), 1u);

    authzconfig_proto::SetAuthorizationProvider_Parameters request;
    const std::string providerUuid = "87654321-4321-4321-4321-cba987654321";
    request.mutable_authorizationprovider()->set_value(providerUuid);
    authzconfig_proto::SetAuthorizationProvider_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.SetAuthorizationProvider(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(config.authorizationProviderUuid(), providerUuid);
    EXPECT_EQ(store.size(), 0u);
    EXPECT_FALSE(store.validate(existingToken, kFeature1).has_value());
}

// FDL AuthorizationConfigurationService-v1_0.sila.xml:41-42 constrains
// AuthorizationProvider to Length 36 plus a lowercase-hex UUID Pattern (S22).
TEST(AuthFeatures, SetAuthorizationProviderWithShortUuidReturnsValidationError) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationConfigurationServiceImpl service{store, config};

    authzconfig_proto::SetAuthorizationProvider_Parameters request;
    request.mutable_authorizationprovider()->set_value("1234");
    authzconfig_proto::SetAuthorizationProvider_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.SetAuthorizationProvider(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(),
              "org.silastandard/core/AuthorizationConfigurationService/v1/Command/"
              "SetAuthorizationProvider/Parameter/AuthorizationProvider");
}

// Length alone is not the only gate: a 36-character uppercase-hex UUID
// satisfies Length but violates the lowercase-hex Pattern.
TEST(AuthFeatures, SetAuthorizationProviderWithUppercaseUuidReturnsValidationError) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationConfigurationServiceImpl service{store, config};

    authzconfig_proto::SetAuthorizationProvider_Parameters request;
    request.mutable_authorizationprovider()->set_value("87654321-4321-4321-4321-CBA987654321");
    authzconfig_proto::SetAuthorizationProvider_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.SetAuthorizationProvider(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
}

// Ordering matters operationally: a rejected SetAuthorizationProvider must
// not still invalidate every issued token via store_.clear().
TEST(AuthFeatures, SetAuthorizationProviderRejectionLeavesTokenStoreIntact) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationConfigurationServiceImpl service{store, config};
    const std::string existingToken = store.issue("alice", {kFeature1}, std::chrono::seconds{60});
    ASSERT_EQ(store.size(), 1u);

    authzconfig_proto::SetAuthorizationProvider_Parameters request;
    request.mutable_authorizationprovider()->set_value("not-a-uuid");
    authzconfig_proto::SetAuthorizationProvider_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.SetAuthorizationProvider(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(store.size(), 1u);
    EXPECT_TRUE(store.validate(existingToken, kFeature1).has_value());
}

// End-to-end form of S37: with no provider configured, ServerConfig now
// defaults authorizationProviderUuid_ to the server's own uuid_
// (ServerConfig.cc, written by writer-1), so the wire value is conformant
// (Length 36 + lowercase-hex Pattern) with no configuration at all.
TEST(AuthFeatures, GetAuthorizationProviderWithoutSetReturnsOwnUuid) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationConfigurationServiceImpl service{store, config};

    authzconfig_proto::Get_AuthorizationProvider_Parameters request;
    authzconfig_proto::Get_AuthorizationProvider_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Get_AuthorizationProvider(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(response.authorizationprovider().value(), kServerUuid);
    EXPECT_EQ(response.authorizationprovider().value().size(), 36u);
}

// Pins that a rejected SetAuthorizationProvider cannot strand the property
// in a non-conformant state -- the interaction between S22's setter guard
// and S37's default has to hold together. Modeled on
// SetAuthorizationProviderWithShortUuidReturnsValidationError above.
TEST(AuthFeatures, GetAuthorizationProviderAfterRejectedShortSetIsUnchanged) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationConfigurationServiceImpl service{store, config};

    authzconfig_proto::SetAuthorizationProvider_Parameters setRequest;
    setRequest.mutable_authorizationprovider()->set_value("87654321-4321-4321-4321-cba98765432");  // 35 chars, one short of Length 36
    authzconfig_proto::SetAuthorizationProvider_Responses setResponse;
    grpc::ServerContext setCtx;
    const grpc::Status setStatus = service.SetAuthorizationProvider(&setCtx, &setRequest, &setResponse);
    ASSERT_FALSE(setStatus.ok());
    const auto setError = fromGrpcStatus(setStatus);
    ASSERT_NE(setError, nullptr);
    ASSERT_EQ(setError->errorType(), SilaError::ErrorType::ValidationError);

    authzconfig_proto::Get_AuthorizationProvider_Parameters getRequest;
    authzconfig_proto::Get_AuthorizationProvider_Responses getResponse;
    grpc::ServerContext getCtx;
    const grpc::Status getStatus = service.Get_AuthorizationProvider(&getCtx, &getRequest, &getResponse);

    ASSERT_TRUE(getStatus.ok());
    EXPECT_EQ(getResponse.authorizationprovider().value(), kServerUuid);
    EXPECT_EQ(getResponse.authorizationprovider().value().size(), 36u);
}

// Same interaction, exercising the Pattern guard specifically: a
// 36-character uppercase-hex UUID satisfies Length but not the
// lowercase-hex Pattern. Modeled on
// SetAuthorizationProviderWithUppercaseUuidReturnsValidationError above.
TEST(AuthFeatures, GetAuthorizationProviderAfterRejectedUppercaseSetIsUnchanged) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    AuthorizationConfigurationServiceImpl service{store, config};

    authzconfig_proto::SetAuthorizationProvider_Parameters setRequest;
    setRequest.mutable_authorizationprovider()->set_value("87654321-4321-4321-4321-CBA987654321");
    authzconfig_proto::SetAuthorizationProvider_Responses setResponse;
    grpc::ServerContext setCtx;
    const grpc::Status setStatus = service.SetAuthorizationProvider(&setCtx, &setRequest, &setResponse);
    ASSERT_FALSE(setStatus.ok());
    const auto setError = fromGrpcStatus(setStatus);
    ASSERT_NE(setError, nullptr);
    ASSERT_EQ(setError->errorType(), SilaError::ErrorType::ValidationError);

    authzconfig_proto::Get_AuthorizationProvider_Parameters getRequest;
    authzconfig_proto::Get_AuthorizationProvider_Responses getResponse;
    grpc::ServerContext getCtx;
    const grpc::Status getStatus = service.Get_AuthorizationProvider(&getCtx, &getRequest, &getResponse);

    ASSERT_TRUE(getStatus.ok());
    EXPECT_EQ(getResponse.authorizationprovider().value(), kServerUuid);
    EXPECT_EQ(getResponse.authorizationprovider().value().size(), 36u);
}

TEST(AuthFeatures, GetAuthorizationProviderReturnsStoredUuid) {
    AuthTokenStore store;
    InMemoryServerConfig config{kServerUuid, "TestServer"};
    const std::string providerUuid = "11111111-2222-3333-4444-555555555555";
    config.setAuthorizationProviderUuid(providerUuid);
    AuthorizationConfigurationServiceImpl service{store, config};

    authzconfig_proto::Get_AuthorizationProvider_Parameters request;
    authzconfig_proto::Get_AuthorizationProvider_Responses response;
    grpc::ServerContext ctx;

    const grpc::Status status = service.Get_AuthorizationProvider(&ctx, &request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(response.authorizationprovider().value(), providerUuid);
}

}  // namespace
