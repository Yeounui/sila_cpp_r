// ServerConfig.h — server identity and runtime tuning (architecture.md §3.7)
//
// New component, not a port. sila_java's ServerConfiguration + IServerConfigWrapper
// and its file-backed implementation provide the reference pattern — immutable
// value object + interface + file-backed implementation. This project merges the
// value object into the interface (getters return directly) and defers the
// persistent implementation.
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

namespace sila2 {

/// Server identity and runtime tuning values (architecture.md §3.7).
class ServerConfig {
public:
    virtual ~ServerConfig() = default;

    /// Build-time identity strings — immutable after construction.
    struct Identity {
        // Compliant placeholder, not a real vendor attribution: makes a
        // default-assembled Identity{} satisfy SiLAService-v1_0.sila.xml:126's
        // ServerType Pattern ([A-Z][a-zA-Z0-9]*) out of the box. Callers are
        // expected to override it; Builder::Build() rejects a non-conformant
        // explicit value (SiLAServerBase.cc).
        std::string serverType = "SiLAServer";
        // SiLAService-v1_0.sila.xml:152-160 -- ServerDescription carries no
        // <Constrained> wrapper, so it gets no compliance-driven default.
        std::string description;
        // SiLAService-v1_0.sila.xml:177's ServerVersion Pattern. Nothing
        // keeps it in sync with the project's own version -- servers built on
        // this library are expected to set their own.
        std::string version = "0.1.0";
        // SiLAService-v1_0.sila.xml:197's ServerVendorURL Pattern (https?://.+).
        std::string vendorUrl = "https://sila-standard.org";
    };

    /// Runtime tuning knobs — all have sensible defaults.
    struct Tuning {
        std::size_t subscriptionQueueDepth = 16;
        std::size_t binarySpoolThreshold = 2 * 1024 * 1024;
        std::chrono::seconds binarySlotLifetime{300};
        std::chrono::seconds cloudWriteTimeout{30};
        std::size_t maxConcurrentCloudSubscriptions = 64;
        std::chrono::seconds errorHandlingTimeout{60};
        std::chrono::seconds mdnsReadvertiseInterval{60};
        std::chrono::seconds mdnsRecordTtl{120};
        std::chrono::milliseconds mdnsProbeWait{250};
    };

    /// @return The server's UUID, stable across restarts.
    [[nodiscard]]
    virtual std::string uuid() const = 0;

    /// @return The server's human-readable name.
    [[nodiscard]]
    virtual std::string name() const = 0;

    /// Update the server name at runtime (SiLAService.SetServerName).
    virtual void setName(std::string name) = 0;

    /// @return The server type (e.g. model name), set at construction.
    [[nodiscard]]
    virtual std::string serverType() const = 0;

    /// @return A human-readable description of the server's purpose.
    [[nodiscard]]
    virtual std::string description() const = 0;

    /// @return The server version string (e.g. "1.0").
    [[nodiscard]]
    virtual std::string version() const = 0;

    /// @return The vendor URL for this server or product.
    [[nodiscard]]
    virtual std::string vendorUrl() const = 0;

    /// @return The per-subscription queue depth for observable properties
    ///         and command execution info streams (§3.3, default 16).
    [[nodiscard]]
    virtual std::size_t subscriptionQueueDepth() const = 0;

    /// @return The UUID of the authorization provider that verifies this
    ///         server's access tokens. Defaults to the server's own UUID
    ///         (InMemoryServerConfig's constructors) until
    ///         SetAuthorizationProvider points it elsewhere.
    [[nodiscard]]
    virtual std::string authorizationProviderUuid() const = 0;

    /// Store the UUID of the authorization provider (§3.11).
    virtual void setAuthorizationProviderUuid(std::string uuid) = 0;

    /// @return Byte threshold above which BinaryStore spools chunks to disk (§3.5, default 2 MiB).
    [[nodiscard]]
    virtual std::size_t binarySpoolThreshold() const = 0;

    /// @return Lifetime of an idle binary slot before GC reclaims it (§3.5, default 300s).
    [[nodiscard]]
    virtual std::chrono::seconds binarySlotLifetime() const = 0;

    /// @return Write timeout for server-initiated cloud connections (§3.9, default 30s).
    [[nodiscard]]
    virtual std::chrono::seconds cloudWriteTimeout() const = 0;

    /// @return Max concurrent long-running cloud pumps: observable property / ExecutionInfo / IntermediateResponse subscriptions plus in-flight _Result fetches (§3.9, default 64).
    [[nodiscard]]
    virtual std::size_t maxConcurrentCloudSubscriptions() const = 0;

    /// @return Default error handling timeout before automatic recovery (§3.12, default 60s).
    [[nodiscard]]
    virtual std::chrono::seconds errorHandlingTimeout() const = 0;

    /// @return Interval between mDNS re-advertisements (§6, default 60s).
    [[nodiscard]]
    virtual std::chrono::seconds mdnsReadvertiseInterval() const = 0;

    /// @return TTL for mDNS resource records (§6, default 120s).
    [[nodiscard]]
    virtual std::chrono::seconds mdnsRecordTtl() const = 0;

    /// @return Wait time for mDNS probe responses before declaring no conflict (§6, default 250ms).
    [[nodiscard]]
    virtual std::chrono::milliseconds mdnsProbeWait() const = 0;
};

/// Non-persistent ServerConfig, for tests. UUID and name live in memory only
/// and are lost on destruction.
class InMemoryServerConfig : public ServerConfig {
public:
    /// Generates a UUID automatically.
    explicit InMemoryServerConfig(std::string name,
                                  Identity identity = {},
                                  Tuning tuning = {});

    /// Explicit UUID, e.g. one loaded from the caller's own storage or fixed
    /// for a deterministic test. Must satisfy the SiLAService ServerUUID
    /// constraint (SiLAService-v1_0.sila.xml:144-147): exactly 36 characters,
    /// lowercase hex, canonically hyphenated.
    /// @throws std::invalid_argument if it does not.
    InMemoryServerConfig(std::string uuid, std::string name,
                         Identity identity = {},
                         Tuning tuning = {});

    [[nodiscard]]
    std::string uuid() const override;

    [[nodiscard]]
    std::string name() const override;

    void setName(std::string name) override;

    [[nodiscard]]
    std::string serverType() const override;

    [[nodiscard]]
    std::string description() const override;

    [[nodiscard]]
    std::string version() const override;

    [[nodiscard]]
    std::string vendorUrl() const override;

    [[nodiscard]]
    std::size_t subscriptionQueueDepth() const override;

    [[nodiscard]]
    std::string authorizationProviderUuid() const override;

    void setAuthorizationProviderUuid(std::string uuid) override;

    [[nodiscard]]
    std::size_t binarySpoolThreshold() const override;

    [[nodiscard]]
    std::chrono::seconds binarySlotLifetime() const override;

    [[nodiscard]]
    std::chrono::seconds cloudWriteTimeout() const override;

    [[nodiscard]]
    std::size_t maxConcurrentCloudSubscriptions() const override;

    [[nodiscard]]
    std::chrono::seconds errorHandlingTimeout() const override;

    [[nodiscard]]
    std::chrono::seconds mdnsReadvertiseInterval() const override;

    [[nodiscard]]
    std::chrono::seconds mdnsRecordTtl() const override;

    [[nodiscard]]
    std::chrono::milliseconds mdnsProbeWait() const override;

private:
    const std::string uuid_;
    std::string name_;
    const Identity identity_;
    std::string authorizationProviderUuid_;
    const Tuning tuning_;
    mutable std::mutex mu_;
};

}  // namespace sila2
