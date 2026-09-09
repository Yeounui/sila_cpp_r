// SilaClientBase.h
//
// Client-side counterpart to SiLAServerBase: owns the gRPC channel built
// from ClientConfig's mTLS credentials, and hands out typed stubs for
// generated services (§4.4).
#pragma once

#include <sila/client/ClientConfig.h>
#include <sila/client/MetadataInjector.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace grpc { class Channel; }

namespace sila2 {

class AuthSession;
class ExecutionStore;

/// Owns the connection to a single @ref gl_sila_server "SiLA Server": the
/// gRPC channel, the SiLA Client Metadata attached to every call, and (once
/// authenticate() succeeds) the access token that authorizes them. Obtain
/// typed stubs from it with createStub() and call its Features.
///
/// @code{.cpp}
/// namespace tc = sila2::org::silastandard::examples::temperaturecontroller::v1;  // from the generated .proto
///
/// sila2::ClientConfig config;
/// config.setTlsCredentials({.caCertificatePem = serverCaPem});
/// config.setUserCredentials("alice", "secret");
///
/// sila2::SilaClientBase client{"127.0.0.1", 50052, std::move(config)};
/// auto stub = client.createStub<tc::TemperatureController::Stub>();
/// client.authenticate(serverUuid);
/// @endcode
///
/// Non-copyable and non-movable: the channel is bound to this object's
/// ClientConfig, and a stub obtained via createStub() would outlive a
/// moved-from channel.
class SilaClientBase {
public:
    /// Opens a gRPC channel to host:port using config's TLS credentials.
    /// If config has a @ref gl_lock "lock" identifier set, it is attached
    /// as @ref gl_sila_client_metadata "SiLA Client Metadata" to every call
    /// from construction on (no separate lock step needed).
    SilaClientBase(std::string host, uint16_t port, ClientConfig config);

    // Defined in .cc where grpc::Channel is a complete type, same pattern
    // as SiLAServerBase's destructor.
    ~SilaClientBase();

    /// @return The gRPC channel, for static stubs and dynamic calls.
    std::shared_ptr<grpc::Channel> channel() const;

    /// Creates a typed stub for a generated service over this client's
    /// channel. Defined inline: template definitions must be visible at
    /// the call site.
    template <typename ServiceStub>
    std::unique_ptr<ServiceStub> createStub() const {
        return ServiceStub::NewStub(channel_);
    }

    const ClientConfig& config() const;
    ClientConfig& config();

    /// @return True if the underlying grpc::Channel reports a connected
    /// state.
    bool isConnected() const;

    /// @return The @ref gl_sila_client_metadata "SiLA Client Metadata"
    /// injector for this connection. A caller invoking a generated static
    /// stub directly must call apply() on it against the stub call's
    /// grpc::ClientContext before every RPC; sila2::dynamic::DynamicCall
    /// does this automatically.
    [[nodiscard]] MetadataInjector& metadataInjector();
    [[nodiscard]] const MetadataInjector& metadataInjector() const;

    /// Logs in to the AuthenticationService with the username/password set
    /// on this client's ClientConfig, then attaches the resulting access
    /// token as @ref gl_sila_client_metadata "SiLA Client Metadata" to every
    /// later call and keeps it renewed until it would expire.
    /// @param serverUuid The target's @ref gl_sila_server_uuid "Server UUID".
    /// @param requestedFeatures Feature identifiers to scope the token to;
    ///        empty requests every Feature the server offers.
    /// @return False if ClientConfig::setUserCredentials() was never called,
    ///         or if the Login RPC fails; true once the token is attached.
    bool authenticate(const std::string& serverUuid,
                      const std::vector<std::string>& requestedFeatures = {});
    /// @return True if authenticate() has succeeded and the token has not
    /// yet expired.
    [[nodiscard]] bool isAuthenticated() const;

    /// @return The Observable Command execution store built from
    /// config.executionStorePath(), or nullptr when no path was configured
    /// (persistence off — Part A p33, see ExecutionStore.h). This is the
    /// assembly seam a production caller uses: pass this pointer and the
    /// server's UUID to executeObservableCommand.
    [[nodiscard]] ExecutionStore* executionStore();

    // Non-copyable: the channel holds a reference to config_'s credentials.
    SilaClientBase(const SilaClientBase&) = delete;
    SilaClientBase& operator=(const SilaClientBase&) = delete;

    // Non-movable, matching SiLAServerBase's pattern for objects that hold
    // credential-bound gRPC state.
    SilaClientBase(SilaClientBase&&) = delete;
    SilaClientBase& operator=(SilaClientBase&&) = delete;

private:
    ClientConfig config_;
    std::shared_ptr<grpc::Channel> channel_;
    MetadataInjector metadataInjector_;
    std::unique_ptr<AuthSession> authSession_;
    // unique_ptr with only a forward-declared ExecutionStore is fine: the
    // destructor that needs the complete type is defined in the .cc, same
    // pattern as ~SilaClientBase() itself needing grpc::Channel complete.
    std::unique_ptr<ExecutionStore> executionStore_;
};

}  // namespace sila2
