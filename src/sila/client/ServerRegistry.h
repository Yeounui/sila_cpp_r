#pragma once

#include <sila/client/ClientConfig.h>
#include <sila/common/util/AsciiCase.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sila2 {

class SilaClientBase;

/// Connection status of one registered @ref gl_sila_server "SiLA Server", as
/// tracked by ServerRegistry.
enum class ConnectionState {
    kDisconnected,
    kConnecting,
    kConnected,
    kTransientFailure,
};

using ConnectionStateCallback = std::function<void(const std::string& serverUuid, ConnectionState state)>;

/// Tracks the @ref gl_sila_server "SiLA Servers" a client knows about (their
/// @ref gl_sila_server_uuid "UUID", address, and connection state) and,
/// optionally, persists that identity across process restarts so a client
/// can reconnect to the same servers automatically.
///
/// Constructed by the application, one per client process; SilaClientBase
/// instances are created and owned by this registry as servers are
/// registered.
class ServerRegistry {
public:
    /// Creates an empty registry. `defaultConfig` is used for every
    /// SilaClientBase this registry creates in registerServer(). When
    /// `storePath` is non-empty, previously persisted entries are loaded
    /// immediately, each coming back in ConnectionState::kDisconnected with
    /// no client until re-registered.
    ///
    // storePath: empty (default) = no persistence, current in-memory-only
    // behaviour. When non-empty, registerServer/removeServer write the
    // identity fields (uuid/host/port/serverName) to that file, and entries
    // loaded from it at construction come back in ConnectionState::kDisconnected
    // with a null client (Part A p33 reconnection guarantee, extended to the
    // client by owner ruling 2026-09-03 — see S36/G2).
    explicit ServerRegistry(ClientConfig defaultConfig = {}, std::filesystem::path storePath = {});
    ~ServerRegistry();

    /// One @ref gl_sila_server "SiLA Server" this registry knows about.
    struct ServerEntry {
        std::string uuid;         ///< @ref gl_sila_server_uuid "UUID" of the server.
        std::string host;         ///< Host or IP literal to connect to.
        uint16_t port;            ///< Port to connect to.
        std::string serverName;   ///< The server's human-readable Server Name.
        ConnectionState state{ConnectionState::kDisconnected};  ///< Current connection state; see updateState().
        /// The live connection, or nullptr for an entry restored from the
        /// store that has not been re-registered yet.
        std::shared_ptr<SilaClientBase> client;
        // FeatureCatalog is populated on first dynamic call (lazy, §4.2)
    };

    /// Registers a server (or re-registers one already known under `uuid`,
    /// e.g. after it rebooted with a new address), creating a fresh
    /// SilaClientBase for it and setting its state to
    /// ConnectionState::kConnecting. Persists the identity fields when this
    /// registry was constructed with a store path.
    /// @throws std::invalid_argument if `uuid`, `host`, or `serverName`
    /// contains a tab or newline (the on-disk store is tab-delimited) and
    /// persistence is enabled.
    void registerServer(std::string uuid, std::string host, uint16_t port,
                       std::string serverName = {});

    /// Drops the entry for `uuid`, if present, and persists the removal when
    /// this registry was constructed with a store path.
    void removeServer(const std::string& uuid);

    /// Looks up a server by @ref gl_sila_server_uuid "UUID" (case-insensitive,
    /// Part A p90).
    /// @return std::nullopt when no such server is registered.
    std::optional<ServerEntry> findByUuid(const std::string& uuid) const;
    /// @return every registered server, in no particular order.
    std::vector<ServerEntry> allServers() const;

    /// Installs a callback invoked whenever a server's ConnectionState
    /// changes, from registerServer() or updateState().
    void setConnectionStateCallback(ConnectionStateCallback cb);

    /// Updates the connection state of a known server, e.g. from a gRPC
    /// channel state watcher or an mDNS goodbye, and notifies the callback
    /// installed via setConnectionStateCallback(). No-op if `uuid` is not
    /// registered.
    void updateState(const std::string& uuid, ConnectionState state);

private:
    // Persists uuid/host/port/serverName to storePath_. No-op when storePath_
    // is empty. Caller must already hold mu_ (mirrors
    // ConnectionConfigurationServiceImpl::saveState, server-side precedent).
    void saveState() const;
    // Reconstructs servers_ from storePath_ as kDisconnected/null-client
    // entries. No-op when storePath_ is empty or the file doesn't exist yet.
    // Called from the constructor, before any other thread can see this
    // object, so no locking is needed here.
    void loadState();

    mutable std::mutex mu_;
    ClientConfig defaultConfig_;
    std::filesystem::path storePath_;
    // Part A p90: UUID keys compare without case.
    std::map<std::string, ServerEntry, util::CaseInsensitiveLess> servers_;
    ConnectionStateCallback stateCallback_;
};
}  // namespace sila2
