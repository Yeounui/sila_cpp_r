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

/// Owns the gRPC channel to a single SiLA2 server. Non-copyable and
/// non-movable: the channel is bound to this object's ClientConfig, and a
/// stub obtained via createStub() would outlive a moved-from channel.
class SilaClientBase {
public:
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

    [[nodiscard]] MetadataInjector& metadataInjector();
    [[nodiscard]] const MetadataInjector& metadataInjector() const;

    bool authenticate(const std::string& serverUuid,
                      const std::vector<std::string>& requestedFeatures = {});
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
