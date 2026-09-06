// End-to-end tests for SilaClientBase::authenticate()/isAuthenticated()
// driven against a real in-process AuthenticationServiceImpl over a real
// gRPC channel (architecture.md §4.4). Complements test_sila_client_base.cc
// (which covers the no-credentials short-circuit without a live server) and
// test_auth_session_client_e2e.cc (which drives AuthSession directly); this
// file exercises the full authenticate() path: AuthSession construction,
// login(), and the resulting metadataInjector()/isAuthenticated() wiring.
#include <sila/client/SilaClientBase.h>

#include <sila/client/ClientConfig.h>
#include <sila/client/MetadataInjector.h>
#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/CredentialVerifier.h>
#include <sila/server/config/ServerConfig.h>
#include <sila/server/config/TlsConfig.h>
#include <sila/server/features/AuthenticationServiceImpl.h>

// The wire value under kAccessTokenWireKey is now a serialized
// Metadata_AccessToken message (see SilaClientBase.cc), not the bare token.
#include "AuthorizationService.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using sila2::AuthenticationServiceImpl;
using sila2::ClientConfig;
using sila2::InMemoryServerConfig;
using sila2::MetadataInjector;
using sila2::SilaClientBase;
using sila2::auth::AccessPolicy;
using sila2::auth::AuthTokenStore;
using sila2::auth::CredentialVerifier;
namespace auth_proto = sila2::auth_proto;

const std::string kServerUuid = "12345678-1234-1234-1234-123456789abc";
const std::string kFeature1 = "org.silastandard/core/Feature1/v1";
const std::string kAccessTokenWireKey = MetadataInjector::headerKey(
    "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken");

struct KeyCertPem {
    std::string keyPem;
    std::string certPem;
};

// Self-signed: the certificate is its own root, so it can serve as both the
// server's leaf cert and, handed to the peer as pem_root_certs, its own CA.
// Mirrors the generateSelfSigned() convention in test_client_config_e2e.cc.
KeyCertPem generateSelfSigned() {
    const auto key = sila2::generateKey();
    const auto cert = sila2::generateCertificate(key, "SiLA2", "127.0.0.1");
    return {sila2::keyToPem(key), sila2::certificateToPem(cert)};
}

// Shared by the fixture server and every client config in this file so they
// all trust the same self-signed cert (Part B p74: no plaintext, S74).
const KeyCertPem& loopbackCert() {
    static const KeyCertPem pair = generateSelfSigned();
    return pair;
}

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
    std::vector<std::string> allowedFqis(const std::string& /*user*/) const override { return {kFeature1}; }
};

// Captures the client metadata a Login RPC arrived with; used only to read
// back what MetadataInjector::apply() actually put on the wire, since
// MetadataInjector exposes no getter for its stored entries.
class MetadataCapturingService final : public auth_proto::AuthenticationService::Service {
public:
    grpc::Status Login(grpc::ServerContext* context, const auth_proto::Login_Parameters*,
                        auth_proto::Login_Responses*) override {
        std::lock_guard<std::mutex> lock(mu_);
        receivedMetadata_.clear();
        for (const auto& [key, value] : context->client_metadata()) {
            receivedMetadata_.emplace(std::string(key.data(), key.length()), std::string(value.data(), value.length()));
        }
        return grpc::Status::OK;
    }

    grpc::Status Logout(grpc::ServerContext*, const auth_proto::Logout_Parameters*,
                         auth_proto::Logout_Responses*) override {
        return grpc::Status::OK;
    }

    std::multimap<std::string, std::string> receivedMetadata() const {
        std::lock_guard<std::mutex> lock(mu_);
        return receivedMetadata_;
    }

private:
    mutable std::mutex mu_;
    std::multimap<std::string, std::string> receivedMetadata_;
};

ClientConfig makeConfigWithCredentials(const std::string& user, const std::string& password) {
    ClientConfig config;
    // 127.0.0.1 is loopback, not private-range (S69), so with allowInsecure()
    // gone (S74) the only route is an explicit CA trusting the fixture
    // server's self-signed cert.
    sila2::TlsCredentials creds;
    creds.caCertificatePem = loopbackCert().certPem;
    config.setTlsCredentials(creds);
    config.setUserCredentials(user, password);
    return config;
}

// Real local server running AuthenticationServiceImpl, same rationale as
// AuthSessionClientE2E: SilaClientBase/AuthSession build their own stub
// internally, so only a real server exercises the actual RPC wire path.
class SilaClientBaseAuthE2E : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        grpc::SslServerCredentialsOptions sslOpts;
        sslOpts.pem_key_cert_pairs.push_back({loopbackCert().keyPem, loopbackCert().certPem});
        builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(sslOpts), &port);
        builder.RegisterService(&authService_);
        server_ = builder.BuildAndStart();
        port_ = port;
    }

    // Shuts down the server first so any renewal thread's next Login RPC
    // fails and exits before SilaClientBase (and its AuthSession) is
    // destroyed; SilaClientBase exposes no logout(), so this is the only
    // available teardown hook.
    void TearDown() override { server_->Shutdown(); }

    AuthTokenStore store_;
    MockCredentialVerifier verifier_;
    MockAccessPolicy policy_;
    InMemoryServerConfig config_{kServerUuid, "TestServer"};
    AuthenticationServiceImpl authService_{store_, verifier_, policy_, config_};

    std::unique_ptr<grpc::Server> server_;
    int port_ = 0;
};

// Sends one RPC through injector's metadata and returns what the server
// received, using a throwaway server distinct from the real auth server so
// that inspecting client_metadata() doesn't interfere with login().
std::multimap<std::string, std::string> captureMetadata(const MetadataInjector& injector) {
    MetadataCapturingService capturingService;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&capturingService);
    auto server = builder.BuildAndStart();
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = auth_proto::AuthenticationService::NewStub(channel);

    grpc::ClientContext ctx;
    injector.apply(ctx);
    auth_proto::Login_Parameters request;
    auth_proto::Login_Responses response;
    stub->Login(&ctx, request, &response);

    server->Shutdown();
    return capturingService.receivedMetadata();
}

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST_F(SilaClientBaseAuthE2E, AuthenticateWithValidCredentialsReturnsTrue) {
    SilaClientBase client{"127.0.0.1", static_cast<uint16_t>(port_), makeConfigWithCredentials("alice", "secret")};

    const bool result = client.authenticate(kServerUuid);

    EXPECT_TRUE(result);
    EXPECT_TRUE(client.isAuthenticated());
}

TEST_F(SilaClientBaseAuthE2E, AuthenticateStoresAccessTokenInMetadataInjector) {
    SilaClientBase client{"127.0.0.1", static_cast<uint16_t>(port_), makeConfigWithCredentials("alice", "secret")};

    ASSERT_TRUE(client.authenticate(kServerUuid));
    const auto received = captureMetadata(client.metadataInjector());

    const auto it = received.find(kAccessTokenWireKey);
    ASSERT_NE(it, received.end());
    EXPECT_FALSE(it->second.empty());
}

TEST_F(SilaClientBaseAuthE2E, AuthenticateWithRequestedFeatureValidatesOnServer) {
    SilaClientBase client{"127.0.0.1", static_cast<uint16_t>(port_), makeConfigWithCredentials("alice", "secret")};

    ASSERT_TRUE(client.authenticate(kServerUuid, {kFeature1}));
    const auto received = captureMetadata(client.metadataInjector());
    const auto it = received.find(kAccessTokenWireKey);
    ASSERT_NE(it, received.end());

    // Verify on the server-side store that the wired-up token is actually
    // scoped to the requested feature, not just that authenticate() and the
    // metadata wiring each returned/reported success independently. The wire
    // value is a serialized Metadata_AccessToken, so unwrap it the same way
    // MetadataExtractingInterceptor does before handing it to the store.
    sila2::org::silastandard::core::authorizationservice::v1::Metadata_AccessToken metadata;
    ASSERT_TRUE(metadata.ParseFromString(it->second));
    const auto entry = store_.validate(metadata.accesstoken().value(), kFeature1);
    EXPECT_TRUE(entry.has_value());
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT: authenticate() surfaces
// AuthSession::login()'s rejection as a plain `false` return, resets the
// AuthSession, and isAuthenticated() reflects the cleared state.
// ---------------------------------------------------------------------------

TEST_F(SilaClientBaseAuthE2E, AuthenticateWithWrongPasswordReturnsFalse) {
    SilaClientBase client{"127.0.0.1", static_cast<uint16_t>(port_), makeConfigWithCredentials("alice", "wrong-password")};

    const bool result = client.authenticate(kServerUuid);

    EXPECT_FALSE(result);
    EXPECT_FALSE(client.isAuthenticated());
}

TEST_F(SilaClientBaseAuthE2E, AuthenticateWithUnknownUserReturnsFalse) {
    SilaClientBase client{"127.0.0.1", static_cast<uint16_t>(port_), makeConfigWithCredentials("mallory", "secret")};

    const bool result = client.authenticate(kServerUuid);

    EXPECT_FALSE(result);
    EXPECT_FALSE(client.isAuthenticated());
}

TEST_F(SilaClientBaseAuthE2E, ReauthenticateWithWrongPasswordAfterSuccessClearsAuthState) {
    SilaClientBase client{"127.0.0.1", static_cast<uint16_t>(port_), makeConfigWithCredentials("alice", "secret")};
    ASSERT_TRUE(client.authenticate(kServerUuid));
    ASSERT_TRUE(client.isAuthenticated());

    client.config().setUserCredentials("alice", "wrong-password");
    const bool result = client.authenticate(kServerUuid);

    EXPECT_FALSE(result);
    EXPECT_FALSE(client.isAuthenticated());
}

}  // namespace
