#include "ClientConfig.h"

#include <sila/common/tls/UntrustedTlsCredentials.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sila2 {

namespace {

}  // namespace

void ClientConfig::setTlsCredentials(TlsCredentials creds) { tlsCredentials_ = std::move(creds); }

const TlsCredentials& ClientConfig::tlsCredentials() const { return tlsCredentials_; }

bool isPrivateAddress(std::string_view host) {
    // The rule itself lives in sila2::tls so the server's default outbound
    // credentials (Part A p32) apply exactly the same Part B p75 test.
    return tls::isPrivateAddress(host);
}

std::shared_ptr<grpc::ChannelCredentials> ClientConfig::channelCredentials(std::string_view host) const {
    // (a) An explicit trust anchor always wins, for any host: strict server
    // verification is exactly what the operator configured. Part B p75's
    // private-IP acceptance is a zero-config fallback, never an override.
    if (!tlsCredentials_.caCertificatePem.empty()) {
        grpc::SslCredentialsOptions ssl_opts;
        ssl_opts.pem_root_certs = tlsCredentials_.caCertificatePem;
        // Client cert/key stay optional: server-only TLS leaves them empty.
        ssl_opts.pem_private_key = tlsCredentials_.privateKeyPem;
        ssl_opts.pem_cert_chain = tlsCredentials_.certificatePem;
        return grpc::SslCredentials(ssl_opts);
    }
    // (b) Part B p75 SHALL: accept the server's untrusted certificate when the
    // target is a private-range IP literal, so zero-config TLS interoperates on
    // a private network without a pre-shared CA.
    if (isPrivateAddress(host)) {
        return tls::untrustedTlsChannelCredentials(
            tlsCredentials_.certificatePem, tlsCredentials_.privateKeyPem);
    }
    // (c) Part B p74: outside the private-IP rule a client MUST NOT implicitly
    // accept untrusted certificates — fail closed for every public target.
    throw std::logic_error{
        "ClientConfig: no CA certificate configured for a non-private target"
        " — supply a CA or target a private-range IP"
        " (RFC1918/RFC4193) for zero-config TLS"};
}

void ClientConfig::setUserCredentials(std::string user, std::string password) {
    user_ = std::move(user);
    password_ = std::move(password);
}

const std::string& ClientConfig::user() const { return user_; }

const std::string& ClientConfig::password() const { return password_; }

bool ClientConfig::hasUserCredentials() const { return !user_.empty(); }

void ClientConfig::setLockIdentifier(std::string lockId) { lockIdentifier_ = std::move(lockId); }

const std::optional<std::string>& ClientConfig::lockIdentifier() const { return lockIdentifier_; }

void ClientConfig::setExecutionStorePath(std::filesystem::path path) {
    executionStorePath_ = std::move(path);
}

const std::filesystem::path& ClientConfig::executionStorePath() const {
    return executionStorePath_;
}

}  // namespace sila2
