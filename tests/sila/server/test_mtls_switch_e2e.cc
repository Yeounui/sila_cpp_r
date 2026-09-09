// End-to-end tests for SilaServerBase::Builder::withMutualTls, exercised
// through the same run()/build() path a real server takes (architecture.md
// §3.1) — audit finding 3.1f: withMutualTls accept/reject was never driven
// through the framework, only unit-tested against raw grpc::ServerCredentials
// (test_client_config_e2e.cc covers ClientConfig's client-side branch; this
// file covers the server-side switch).
//
// withMutualTls(caCertPem) only ever sets Builder::caCertPem_
// (SilaServerBase.cc:309-312); the actual branch lives in run():
//   caCertPem_.empty()  -> ssl_opts.client_certificate_request left at its
//                          default (GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE)
//                          -> no client cert requested at all.
//   !caCertPem_.empty() -> ssl_opts.pem_root_certs = caCertPem_;
//                          client_certificate_request =
//                          GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
//                          -> every client must present a cert chaining to
//                          caCertPem_, verified by BoringSSL/gRPC.
//
// TlsConfig (config/TlsConfig.h) only offers generateKey/generateCertificate
// for self-signed leaf certificates — there is no "sign this CSR with a
// separate CA key" API in this codebase. A self-signed certificate can still
// serve as its own trust anchor (handed to the peer as pem_root_certs), so
// "signed by CA X" below means "the exact self-signed certificate X, handed
// directly to withMutualTls as the trusted root" — the same substitution
// test_client_config_e2e.cc's generateSelfSigned() already relies on.
// Because of that, "signed by a different CA" and "self-signed cert not in
// the CA" collapse onto the same rejection mechanism (client cert not
// present in pem_root_certs); a third, structurally distinct False input
// (malformed cert bytes) is used instead of a second flavor of the same
// untrusted-issuer case.
#include <sila/server/SilaServerBase.h>

#include <sila/server/SilaServiceImpl.h>
#include <sila/server/config/TlsConfig.h>
#include <sila/server/auth/AccessPolicy.h>
#include <sila/server/auth/CredentialVerifier.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace {

using sila2::SilaServerBase;

// Distinct ports per test, same rationale as
// test_sila_server_base_run_shutdown_e2e.cc: every run()-driving test here
// enables discovery just to control the listening port.
constexpr uint16_t kPortTrustedClient = 50280;
constexpr uint16_t kPortNoMutualTlsBaseline = 50281;
constexpr uint16_t kPortMutualTlsWithAuth = 50282;
constexpr uint16_t kPortNoClientCert = 50283;
constexpr uint16_t kPortWrongCaClientCert = 50284;
constexpr uint16_t kPortMalformedClientCert = 50285;

struct KeyCertPem {
    std::string keyPem;
    std::string certPem;
};

// Self-signed leaf cert — its own root, so it doubles as the CA handed to
// withMutualTls (see file header comment on TlsConfig's capabilities).
KeyCertPem generateSelfSigned() {
    const auto key = sila2::generateKey();
    const auto cert = sila2::generateCertificate(key, "SiLA2", "127.0.0.1");
    return {sila2::keyToPem(key), sila2::certificateToPem(cert)};
}

// Real local channel dialed against the server's own self-signed
// certificate, optionally presenting a client identity — same shape as
// test_sila_server_base_run_shutdown_e2e.cc's dialChannel, extended with the
// two client-credential fields mTLS needs.
std::shared_ptr<grpc::Channel> dialChannel(const SilaServerBase& server, uint16_t port,
                                            const std::string& clientCertPem = {},
                                            const std::string& clientKeyPem = {}) {
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    opts.pem_cert_chain = clientCertPem;
    opts.pem_private_key = clientKeyPem;
    return grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(opts));
}

// Bounded deadline so a rejected handshake fails the RPC instead of hanging
// — same pattern as test_client_config_e2e.cc's WaitForConnected(+3s).
bool getServerUuidSucceeds(const std::shared_ptr<grpc::Channel>& channel) {
    auto stub = sila2::silaservice_proto::SiLAService::NewStub(channel);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    sila2::silaservice_proto::Get_ServerUUID_Parameters req;
    sila2::silaservice_proto::Get_ServerUUID_Responses resp;
    return stub->Get_ServerUUID(&ctx, req, &resp).ok();
}

// Minimal accepting auth stubs, reused from
// test_sila_server_base_run_shutdown_e2e.cc's pattern — only used to prove
// AuthenticationService stays reachable over an mTLS channel, not to
// exercise auth decision logic.
struct AcceptingVerifier : sila2::auth::CredentialVerifier {
    std::optional<std::string> verify(const std::string& user, const std::string&) override {
        return user;
    }
};

struct AllowAllPolicy : sila2::auth::AccessPolicy {
    bool isAllowed(const std::string&, const std::string&) const override { return true; }
    std::vector<std::string> allowedFqis(const std::string&) const override { return {}; }
};

}  // namespace

// ---------------------------------------------------------------------------
// withMutualTls — True (positive) paths
// ---------------------------------------------------------------------------

TEST(MutualTlsSwitch, ClientWithTrustedCertConnectsAndCallsRpc) {
    const auto clientPair = generateSelfSigned();

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withMutualTls(clientPair.certPem)
                      .withDiscovery(kPortTrustedClient)
                      .build();
    server.run(false);

    auto channel = dialChannel(server, kPortTrustedClient, clientPair.certPem, clientPair.keyPem);
    EXPECT_TRUE(getServerUuidSucceeds(channel));

    server.shutdown();
}

TEST(MutualTlsSwitch, ServerWithoutMutualTlsAcceptsClientWithoutCert) {
    // No withMutualTls() call — caCertPem_ stays empty, so run() must leave
    // client_certificate_request at its default and never require a cert.
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withDiscovery(kPortNoMutualTlsBaseline)
                      .build();
    server.run(false);

    auto channel = dialChannel(server, kPortNoMutualTlsBaseline);  // no client cert
    EXPECT_TRUE(getServerUuidSucceeds(channel));

    server.shutdown();
}

TEST(MutualTlsSwitch, ClientWithTrustedCertCanMutateServerNameOverMutualTlsChannel) {
    // Distinct code path from the read-only Get_ServerUUID above: a mutating
    // command (SetServerName -> ServerConfig::setName), and composed with a
    // second Builder axis (withAuthentication) to prove mTLS does not
    // interfere with the rest of run()'s registeredServices() loop.
    const auto clientPair = generateSelfSigned();

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withMutualTls(clientPair.certPem)
                      .withAuthentication(std::make_unique<AcceptingVerifier>(),
                                          std::make_unique<AllowAllPolicy>(), {})
                      .withDiscovery(kPortMutualTlsWithAuth)
                      .build();
    server.run(false);

    auto channel = dialChannel(server, kPortMutualTlsWithAuth, clientPair.certPem, clientPair.keyPem);
    auto stub = sila2::silaservice_proto::SiLAService::NewStub(channel);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    sila2::silaservice_proto::SetServerName_Parameters req;
    req.mutable_servername()->set_value("mTLS Test Server");
    sila2::silaservice_proto::SetServerName_Responses resp;
    auto status = stub->SetServerName(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(server.serverConfig().name(), "mTLS Test Server");

    server.shutdown();
}

// ---------------------------------------------------------------------------
// withMutualTls — False (negative/rejection) paths
// All three funnel through the same rejection mechanism inside BoringSSL/
// gRPC's client-certificate verification (see file header comment): the
// server never accepts the connection, so the RPC fails.
// ---------------------------------------------------------------------------

// CAUGHT: no client certificate presented at all — the server's
// GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY setting refuses
// the handshake outright.
TEST(MutualTlsSwitch, ClientWithoutCertIsRejectedWhenMutualTlsRequired) {
    const auto clientPair = generateSelfSigned();

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withMutualTls(clientPair.certPem)
                      .withDiscovery(kPortNoClientCert)
                      .build();
    server.run(false);

    auto channel = dialChannel(server, kPortNoClientCert);  // no client cert presented
    EXPECT_FALSE(getServerUuidSucceeds(channel));

    server.shutdown();
}

// CAUGHT: client presents a well-formed, real certificate — just not the one
// (nor signed by the one) withMutualTls trusted.
TEST(MutualTlsSwitch, ClientWithUntrustedCaCertIsRejected) {
    const auto trustedPair = generateSelfSigned();
    const auto untrustedPair = generateSelfSigned();  // different key/cert entirely

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withMutualTls(trustedPair.certPem)
                      .withDiscovery(kPortWrongCaClientCert)
                      .build();
    server.run(false);

    auto channel = dialChannel(server, kPortWrongCaClientCert,
                                untrustedPair.certPem, untrustedPair.keyPem);
    EXPECT_FALSE(getServerUuidSucceeds(channel));

    server.shutdown();
}

// CAUGHT: client cert content is truncated garbage rather than a real,
// merely-untrusted certificate — a structurally distinct rejection input
// from the previous case, mirroring
// test_sila_server_base_run_shutdown_e2e.cc's TruncatedCertificateThrows
// test for the server's own cert.
TEST(MutualTlsSwitch, ClientWithMalformedCertIsRejected) {
    const auto trustedPair = generateSelfSigned();
    const auto malformedClientPair = generateSelfSigned();
    const std::string truncatedCert =
        malformedClientPair.certPem.substr(0, malformedClientPair.certPem.size() / 2);

    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withMutualTls(trustedPair.certPem)
                      .withDiscovery(kPortMalformedClientCert)
                      .build();
    server.run(false);

    auto channel = dialChannel(server, kPortMalformedClientCert,
                                truncatedCert, malformedClientPair.keyPem);
    EXPECT_FALSE(getServerUuidSucceeds(channel));

    server.shutdown();
}
