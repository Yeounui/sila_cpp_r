// ServerConfig.h — server identity and runtime tuning (architecture.md §3.7)
//
// New component, not a port. sila_java's ServerConfiguration +
// IServerConfigWrapper + PersistentServerConfigWrapper provide the reference
// pattern — immutable value object + interface + file-backed implementation.
// This project merges the value object into the interface (getters return
// directly) and defers the persistent implementation until a JSON library
// is introduced.
#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace sila2 {

/// Server identity and runtime tuning values (architecture.md §3.7).
class ServerConfig {
public:
    virtual ~ServerConfig() = default;

    /// @return The server's UUID, stable across restarts.
    [[nodiscard("caller expects the server UUID")]] \
    virtual std::string uuid() const = 0;

    /// @return The server's human-readable name.
    [[nodiscard("caller expects the server name")]] \
    virtual std::string name() const = 0;

    /// Update the server name at runtime (SiLAService.SetServerName).
    virtual void setName(std::string name) = 0;

    /// @return The per-subscription queue depth for observable properties
    ///         and command execution info streams (§3.3, default 16).
    [[nodiscard("caller expects the subscription queue depth")]] \
    virtual std::size_t subscriptionQueueDepth() const = 0;

    // TODO(owner): add when consumers are implemented:
    // virtual std::size_t binarySpoolThreshold() const = 0;    // §3.5
    // virtual std::chrono::seconds binarySlotLifetime() const = 0;  // §3.5
    // virtual std::chrono::seconds cloudWriteTimeout() const = 0;  // §3.9
    // virtual std::string authorizationProviderUuid() const = 0;  // §3.11
    // virtual std::chrono::seconds errorHandlingTimeout() const = 0;  // §3.12
    // virtual std::chrono::seconds mdnsReadvertiseInterval() const = 0;  // §6
    // virtual std::chrono::seconds mdnsRecordTtl() const = 0;  // §6
    // virtual std::chrono::milliseconds mdnsProbeWait() const = 0;  // §6
};

/// Non-persistent ServerConfig, for tests. UUID and name live in memory only
/// and are lost on destruction.
class InMemoryServerConfig : public ServerConfig {
public:
    /// Generates a UUID automatically.
    /// @param name The server's human-readable name.
    /// @param queueDepth Per-subscription queue depth (default 16).
    explicit InMemoryServerConfig(std::string name,
                                  std::size_t queueDepth = 16);

    /// Explicit UUID (for deterministic tests).
    /// @param uuid The server UUID to use.
    /// @param name The server's human-readable name.
    /// @param queueDepth Per-subscription queue depth (default 16).
    InMemoryServerConfig(std::string uuid, std::string name,
                         std::size_t queueDepth = 16);

    [[nodiscard("caller expects the server UUID")]] \
    std::string uuid() const override;

    [[nodiscard("caller expects the server name")]] \
    std::string name() const override;

    void setName(std::string name) override;

    [[nodiscard("caller expects the subscription queue depth")]] \
    std::size_t subscriptionQueueDepth() const override;

private:
    const std::string uuid_;
    std::string name_;
    const std::size_t queueDepth_;
    mutable std::mutex mu_;
};

}  // namespace sila2
