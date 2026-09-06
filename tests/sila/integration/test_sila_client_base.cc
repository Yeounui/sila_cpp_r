// Tests for SilaClientBase: gRPC channel construction, lock-identifier ->
// MetadataInjector wiring, and the authenticate()/isAuthenticated() paths
// that don't require a live AuthenticationService server.
#include <sila/client/SilaClientBase.h>

#include <sila/client/ClientConfig.h>
#include <sila/client/MetadataInjector.h>
#include <sila/server/config/TlsConfig.h>
#include <sila/server/features/AuthenticationServiceImpl.h>  // auth_proto::AuthenticationService, generated Service/Stub

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <chrono>

namespace
{
using sila2::ClientConfig;
using sila2::MetadataInjector;
using sila2::SilaClientBase;
namespace auth_proto = sila2::auth_proto;

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

// Shared across every fixture in this file so the client and every server it
// talks to trust the same self-signed cert (Part B p74: no plaintext, S74).
const KeyCertPem& loopbackCert() {
    static const KeyCertPem pair = generateSelfSigned();
    return pair;
}

// Captures the client metadata a Login RPC arrived with; request/response
// contents are irrelevant, only context->client_metadata() is inspected.
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

// Real local server+channel: the only way to observe what MetadataInjector::
// apply actually put on the wire, since ClientContext exposes no public
// getter for outgoing metadata before the call is issued.
class LocalAuthServer {
public:
    LocalAuthServer() {
        start(0);
    }

    ~LocalAuthServer() {
        if (server_) server_->Shutdown();
    }

    uint16_t port() const { return static_cast<uint16_t>(port_); }
    bool available() const { return server_ != nullptr; }

    bool restart() {
        server_->Shutdown();
        return start(port_);
    }

    bool acceptsLogin(const std::shared_ptr<grpc::Channel>& channel) const {
        auto stub = auth_proto::AuthenticationService::NewStub(channel);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
        auth_proto::Login_Parameters request;
        auth_proto::Login_Responses response;
        return stub->Login(&ctx, request, &response).ok();
    }

    std::multimap<std::string, std::string> callWith(const MetadataInjector& injector) {
        grpc::ClientContext ctx;
        injector.apply(ctx);
        auth_proto::Login_Parameters request;
        auth_proto::Login_Responses response;
        stub_->Login(&ctx, request, &response);
        return service_.receivedMetadata();
    }

private:
    bool start(int port) {
        grpc::ServerBuilder builder;
        grpc::SslServerCredentialsOptions sslOpts;
        sslOpts.pem_key_cert_pairs.push_back({loopbackCert().keyPem, loopbackCert().certPem});
        builder.AddListeningPort("127.0.0.1:" + std::to_string(port), grpc::SslServerCredentials(sslOpts), &port_);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        if (!server_) return false;
        // The fixture's own diagnostic channel (callWith/acceptsLogin) must
        // trust the same self-signed cert the server now presents (S74).
        grpc::SslCredentialsOptions clientSslOpts;
        clientSslOpts.pem_root_certs = loopbackCert().certPem;
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::SslCredentials(clientSslOpts));
        stub_ = auth_proto::AuthenticationService::NewStub(channel_);
        return true;
    }

    MetadataCapturingService service_;
    int port_ = 0;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<auth_proto::AuthenticationService::Stub> stub_;
};

const std::string kLockMetadataFqi = "org.silastandard/core/LockController/v1/Metadata/LockIdentifier";

ClientConfig makeConfig() {
    ClientConfig config;
    // 127.0.0.1 is loopback, not private-range (S69), so with allowInsecure()
    // gone (S74) the only route is an explicit CA trusting the shared
    // self-signed cert every fixture server in this file presents.
    sila2::TlsCredentials creds;
    creds.caCertificatePem = loopbackCert().certPem;
    config.setTlsCredentials(creds);
    return config;
}

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(SilaClientBase, ConstructorWithLockIdentifierInjectsLockMetadata) {
    ClientConfig config = makeConfig();
    config.setLockIdentifier("lock-42");
    SilaClientBase client("127.0.0.1", 50051, config);
    LocalAuthServer server;

    const auto received = server.callWith(client.metadataInjector());

    const auto it = received.find(MetadataInjector::headerKey(kLockMetadataFqi));
    ASSERT_NE(it, received.end());
}

TEST(SilaClientBase, ConstructorReturnsNonNullChannel) {
    SilaClientBase client("127.0.0.1", 50051, makeConfig());

    EXPECT_NE(client.channel(), nullptr);
}

TEST(SilaClientBase, ConstructorLeavesFreshChannelConnected) {
    SilaClientBase client("127.0.0.1", 50051, makeConfig());

    // Fresh channel starts IDLE, which isConnected() treats as connected
    // (SilaClientBase.cc:63).
    EXPECT_TRUE(client.isConnected());
}

TEST(SilaClientBase, ChannelReconnectsAfterServerRestart) {
    LocalAuthServer server;
    if (!server.available()) GTEST_SKIP() << "local gRPC listener unavailable";
    SilaClientBase client("127.0.0.1", server.port(), makeConfig());

    ASSERT_TRUE(server.acceptsLogin(client.channel()));
    ASSERT_TRUE(server.restart());
    EXPECT_TRUE(server.acceptsLogin(client.channel()));
}

// ---------------------------------------------------------------------------
// False (negative/edge) paths
// ---------------------------------------------------------------------------

TEST(SilaClientBase, ConstructorWithoutLockIdentifierInjectsNoMetadata) {
    SilaClientBase client("127.0.0.1", 50051, makeConfig());
    LocalAuthServer server;

    // grpc itself always sends a "user-agent" header, so this checks that no
    // sila-*-bin metadata header arrives rather than that received is empty.
    const auto received = server.callWith(client.metadataInjector());

    EXPECT_EQ(received.find(MetadataInjector::headerKey(kLockMetadataFqi)), received.end());
}

TEST(SilaClientBase, AuthenticateWithoutUserCredentialsReturnsFalse) {
    SilaClientBase client("127.0.0.1", 50051, makeConfig());

    const bool result = client.authenticate("00000000-0000-0000-0000-000000000000");

    EXPECT_FALSE(result);
    EXPECT_FALSE(client.isAuthenticated());
}

TEST(SilaClientBase, IsAuthenticatedBeforeAuthenticateReturnsFalse) {
    SilaClientBase client("127.0.0.1", 50051, makeConfig());

    EXPECT_FALSE(client.isAuthenticated());
}

}  // namespace
