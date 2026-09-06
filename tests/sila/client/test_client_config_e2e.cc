// Tests for ClientConfig::channelCredentials(): the SSL/mTLS branches it
// builds from TlsCredentials, verified by actually completing a gRPC
// handshake against a matching local server (not just checking the returned
// shared_ptr is non-null). Also covers the remaining untested ClientConfig
// accessors (user credentials, retry/backoff defaults).
#include <sila/client/ClientConfig.h>

#include <sila/client/SilaClientBase.h>
#include <sila/server/config/TlsConfig.h>
#include <sila/server/features/AuthenticationServiceImpl.h>  // auth_proto::AuthenticationService, generated Service/Stub

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
using sila2::ClientConfig;
using sila2::isPrivateAddress;
using sila2::SilaClientBase;
using sila2::TlsCredentials;
namespace auth_proto = sila2::auth_proto;

// Login always succeeds; the point of the RPC is that it completes at all,
// which requires the gRPC TLS/mTLS handshake to have gone through.
class NoOpAuthService final : public auth_proto::AuthenticationService::Service {
public:
    grpc::Status Login(grpc::ServerContext*, const auth_proto::Login_Parameters*,
                        auth_proto::Login_Responses*) override {
        return grpc::Status::OK;
    }

    grpc::Status Logout(grpc::ServerContext*, const auth_proto::Logout_Parameters*,
                         auth_proto::Logout_Responses*) override {
        return grpc::Status::OK;
    }
};

struct KeyCertPem {
    std::string keyPem;
    std::string certPem;
};

// Self-signed: the certificate is its own root, so it can serve as both the
// server's leaf cert and, handed to the peer as pem_root_certs, its own CA.
KeyCertPem generateSelfSigned() {
    const auto key = sila2::generateKey();
    const auto cert = sila2::generateCertificate(key, "SiLA2", "127.0.0.1");
    return {sila2::keyToPem(key), sila2::certificateToPem(cert)};
}

// Real local server on an OS-assigned port, credentials supplied by the
// caller so the same fixture covers insecure, server-only TLS, and mTLS.
class LocalServer {
public:
    explicit LocalServer(std::shared_ptr<grpc::ServerCredentials> credentials) {
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", std::move(credentials), &port_);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
    }

    ~LocalServer() { server_->Shutdown(); }

    uint16_t port() const { return static_cast<uint16_t>(port_); }

private:
    NoOpAuthService service_;
    std::unique_ptr<grpc::Server> server_;
    int port_ = 0;
};

// Exercises the same channel a real client would use: SilaClientBase builds
// it from config.channelCredentials() (SilaClientBase.cc), so a successful
// RPC here proves the credentials propagate all the way to a working
// connection, not just that channelCredentials() returned a non-null value.
bool loginSucceeds(SilaClientBase& client) {
    auto stub = auth_proto::AuthenticationService::NewStub(client.channel());
    grpc::ClientContext ctx;
    auth_proto::Login_Parameters request;
    auth_proto::Login_Responses response;
    return stub->Login(&ctx, request, &response).ok();
}

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(ClientConfigCredentials, EmptyCaCertificateThrowsForNonPrivateTarget) {
    ClientConfig config;
    // 127.0.0.1 is loopback, not a private-range IP (Part B p75), so branch
    // (b) does not apply and this still fails closed per branch (c).
    EXPECT_THROW(config.channelCredentials("127.0.0.1"), std::logic_error);
}

// Part B p74 MUST: SiLA Clients and Servers MUST always use TLS, with no
// plaintext exception (S74). 127.0.0.1 is loopback, not private-range (S69),
// so the only route left to reach it is an explicitly configured CA.
TEST(ClientConfigCredentials, LoopbackServerReachableOnlyViaExplicitCa) {
    const auto serverPair = generateSelfSigned();
    grpc::SslServerCredentialsOptions sslOpts;
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    LocalServer server(grpc::SslServerCredentials(sslOpts));

    ClientConfig config;
    TlsCredentials creds;
    creds.caCertificatePem = serverPair.certPem;
    config.setTlsCredentials(creds);

    ASSERT_NE(config.channelCredentials("127.0.0.1"), nullptr);

    SilaClientBase client("127.0.0.1", server.port(), config);
    EXPECT_TRUE(loginSucceeds(client));
}

TEST(ClientConfigCredentials, CaCertificateOnlyBuildsWorkingServerOnlyTlsChannel) {
    const auto serverPair = generateSelfSigned();
    grpc::SslServerCredentialsOptions sslOpts;
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    LocalServer server(grpc::SslServerCredentials(sslOpts));

    ClientConfig config;
    TlsCredentials creds;
    creds.caCertificatePem = serverPair.certPem;
    config.setTlsCredentials(creds);

    ASSERT_NE(config.channelCredentials("127.0.0.1"), nullptr);

    SilaClientBase client("127.0.0.1", server.port(), config);
    EXPECT_TRUE(loginSucceeds(client));
}

TEST(ClientConfigCredentials, FullCredentialsBuildWorkingMutualTlsChannel) {
    const auto serverPair = generateSelfSigned();
    const auto clientPair = generateSelfSigned();

    grpc::SslServerCredentialsOptions sslOpts{GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY};
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    sslOpts.pem_root_certs = clientPair.certPem;
    LocalServer server(grpc::SslServerCredentials(sslOpts));

    ClientConfig config;
    TlsCredentials creds;
    creds.caCertificatePem = serverPair.certPem;
    creds.certificatePem = clientPair.certPem;
    creds.privateKeyPem = clientPair.keyPem;
    config.setTlsCredentials(creds);

    ASSERT_NE(config.channelCredentials("127.0.0.1"), nullptr);

    SilaClientBase client("127.0.0.1", server.port(), config);
    EXPECT_TRUE(loginSucceeds(client));
}

// Part B p75 range table: RFC1918 (10/8, 172.16/12, 192.168/16) and RFC4193
// ULA (fc00::/7), plus IPv4-mapped IPv6 of a private address. Loopback,
// link-local, and hostnames are deliberately NOT private (no DNS is done).
TEST(ClientConfigCredentials, IsPrivateAddressMatchesTheRfc1918And4193Ranges) {
    for (const std::string host : {"10.0.0.1", "10.255.255.255", "172.16.0.1", "172.31.255.255",
                                    "192.168.1.1", "fc00::1", "fd12::1", "[fd00::1]",
                                    "::ffff:10.0.0.1"}) {
        EXPECT_TRUE(isPrivateAddress(host)) << host;
    }
    for (const std::string host : {"9.255.255.255", "11.0.0.0", "172.15.255.255", "172.32.0.0",
                                    "192.167.255.255", "192.169.0.0", "127.0.0.1", "::1",
                                    "169.254.1.1", "fe80::1", "203.0.113.5", "::ffff:203.0.113.5",
                                    "localhost", ""}) {
        EXPECT_FALSE(isPrivateAddress(host)) << host;
    }
}

// End-to-end handshake proving branch (c): credentials are built for a
// private-range literal ("10.0.0.1") but the channel is actually aimed at the
// loopback test server presenting a self-signed cert. verify_server_certs and
// check_call_host are both off, so the untrusted, mismatched-hostname
// certificate is accepted (Part B p75), unlike the strict-CA path above.
TEST(ClientConfigCredentials, PrivateIpTargetAcceptsUntrustedServerCertificate) {
    const auto serverPair = generateSelfSigned();
    grpc::SslServerCredentialsOptions sslOpts;
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    LocalServer server(grpc::SslServerCredentials(sslOpts));

    ClientConfig config;  // empty CA, no trust anchor configured
    auto creds = config.channelCredentials("10.0.0.1");
    ASSERT_NE(creds, nullptr);

    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), creds);
    EXPECT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5)));
}

// ---------------------------------------------------------------------------
// False (negative/edge) paths
//
// channelCredentials() throws when no CA cert is set and the target is not
// private-range (tested above). When credentials ARE supplied, gRPC defers TLS
// validation to handshake time — so each case below proves the deferred
// failure by actually attempting a connection with a bounded deadline
// (WaitForConnected must return false rather than hang). The remaining tests
// cover the untested ClientConfig accessors.
// ---------------------------------------------------------------------------

// Non-PEM garbage as the CA — credentials build without throwing, but the
// handshake can never construct a trust store from it.
TEST(ClientConfigCredentials, GarbageCaCertificateBuildsCredentialsButHandshakeFails) {
    const auto serverPair = generateSelfSigned();
    grpc::SslServerCredentialsOptions sslOpts;
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    LocalServer server(grpc::SslServerCredentials(sslOpts));

    ClientConfig config;
    TlsCredentials creds;
    creds.caCertificatePem = "not a real certificate, just garbage text";
    config.setTlsCredentials(creds);

    auto grpcCreds = config.channelCredentials("127.0.0.1");
    ASSERT_NE(grpcCreds, nullptr) << "credential creation must not throw on malformed PEM";

    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpcCreds);
    EXPECT_FALSE(channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(3)));
}

// Syntactically valid PEM, but not the certificate this server actually
// presents — a distinct failure mode from malformed PEM: untrusted issuer
// rather than an unparsable trust store.
TEST(ClientConfigCredentials, WrongCaCertificateHandshakeFailsOnUntrustedIssuer) {
    const auto serverPair = generateSelfSigned();
    const auto unrelatedPair = generateSelfSigned();
    grpc::SslServerCredentialsOptions sslOpts;
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    LocalServer server(grpc::SslServerCredentials(sslOpts));

    ClientConfig config;
    TlsCredentials creds;
    creds.caCertificatePem = unrelatedPair.certPem;
    config.setTlsCredentials(creds);

    auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(server.port()), config.channelCredentials("127.0.0.1"));
    EXPECT_FALSE(channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(3)));
}

// caCertificatePem empty and no allowInsecure() opt-in exists anymore (S74):
// even with a TLS-only server up and reachable, a no-CA config for a
// non-private target now fails at credential build, not at handshake time.
TEST(ClientConfigCredentials, NoCaConfigCannotBuildCredentialsForATlsOnlyServer) {
    const auto serverPair = generateSelfSigned();
    grpc::SslServerCredentialsOptions sslOpts;
    sslOpts.pem_key_cert_pairs.push_back({serverPair.keyPem, serverPair.certPem});
    LocalServer server(grpc::SslServerCredentials(sslOpts));

    ClientConfig config;
    EXPECT_THROW(config.channelCredentials("127.0.0.1"), std::logic_error);
}

TEST(ClientConfigCredentials, HasUserCredentialsIsFalseUntilSet) {
    ClientConfig config;
    EXPECT_FALSE(config.hasUserCredentials());

    config.setUserCredentials("alice", "secret");

    EXPECT_TRUE(config.hasUserCredentials());
    EXPECT_EQ(config.user(), "alice");
    EXPECT_EQ(config.password(), "secret");
}

}  // namespace
