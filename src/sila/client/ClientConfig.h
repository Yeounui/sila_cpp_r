// ClientConfig.h — client connection settings: TLS, auth, lock ID
// (architecture.md §4.4, §3.10, §4.6)
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Forward-declared to avoid pulling grpcpp into every translation unit that
// only needs to read/write config values.
namespace grpc {
class ChannelCredentials;
}  // namespace grpc

namespace sila2 {

/// The client-side mTLS certificate/key pair and, optionally, the server's
/// CA certificate, used to build the gRPC channel for one connection.
/// Obtained by the caller and passed to ClientConfig::setTlsCredentials().
struct TlsCredentials {
    std::string certificatePem;    // client cert (mTLS)
    std::string privateKeyPem;     // client key
    std::string caCertificatePem;  // server CA cert (for verification)
};

/// Settings for one connection to a @ref gl_sila_server "SiLA Server": TLS
/// credentials, login credentials, the @ref gl_lock "lock" identifier, and
/// where to persist Observable Command executions. Built by the caller and
/// passed to SilaClientBase's constructor.
///
/// Per-connection settings for a SiLA2 client (architecture.md §4.4, §3.10).
/// Concrete value/settings holder — unlike ServerConfig, there is only one
/// way to configure a client connection, so no interface is warranted.
class ClientConfig {
public:
    /// Store the mTLS credentials used to build the gRPC channel.
    void setTlsCredentials(TlsCredentials creds);

    /// @return The mTLS credentials currently configured.
    const TlsCredentials& tlsCredentials() const;

    /// @param host The connection target (a host or a bracketed IP literal).
    ///        Part B p75: when it is a private-range IP (RFC1918/RFC4193) and
    ///        no CA is configured, the returned credentials accept the
    ///        server's untrusted certificate so zero-config TLS works.
    /// @return gRPC channel credentials built from the configured TLS material.
    /// @throws std::logic_error if no CA certificate is configured and the
    ///         target is not a private-range IP.
    std::shared_ptr<grpc::ChannelCredentials> channelCredentials(std::string_view host) const;

    /// Store the username/password SilaClientBase::authenticate() logs in
    /// with.
    /// @see SilaClientBase::authenticate
    void setUserCredentials(std::string user, std::string password);

    /// @return The configured username.
    const std::string& user() const;

    /// @return The configured password.
    const std::string& password() const;

    /// @return True if setUserCredentials() has been called.
    bool hasUserCredentials() const;

    /// Store the lock identifier this client chose when calling `LockServer`, so
    /// SilaClientBase attaches it as @ref gl_sila_client_metadata "SiLA Client Metadata" to every
    /// call it makes.
    void setLockIdentifier(std::string lockId);

    /// @return The configured @ref gl_lock "lock" identifier, or nullopt if
    /// this client has not locked the server.
    const std::optional<std::string>& lockIdentifier() const;

    /// Set the file SilaClientBase persists issued Observable Command
    /// executions to (Part A p33, extended to the client by owner ruling
    /// 2026-09-03 — see ExecutionStore.h). An empty path (the default) keeps
    /// persistence off, matching the pre-existing behaviour.
    void setExecutionStorePath(std::filesystem::path path);

    /// @return The configured execution store path, or empty if unset.
    const std::filesystem::path& executionStorePath() const;

private:
    TlsCredentials tlsCredentials_;
    std::string user_;
    std::string password_;
    std::optional<std::string> lockIdentifier_;
    std::filesystem::path executionStorePath_;
};

/// Part B p75: true only for the private IP ranges whose untrusted server
/// certificates a client SHALL accept — RFC1918 (10/8, 172.16/12, 192.168/16)
/// and RFC4193 ULA (fc00::/7), plus IPv4-mapped IPv6 of those. Loopback,
/// link-local and hostnames are NOT private and no DNS resolution is done.
bool isPrivateAddress(std::string_view host);

}  // namespace sila2
