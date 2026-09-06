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

enum class ConnectionState {
    kDisconnected,
    kConnecting,
    kConnected,
    kTransientFailure,
};

using ConnectionStateCallback = std::function<void(const std::string& serverUuid, ConnectionState state)>;

class ServerRegistry {
public:
    // storePath: empty (default) = no persistence, current in-memory-only
    // behaviour. When non-empty, registerServer/removeServer write the
    // identity fields (uuid/host/port/serverName) to that file, and entries
    // loaded from it at construction come back in ConnectionState::kDisconnected
    // with a null client (Part A p33 reconnection guarantee, extended to the
    // client by owner ruling 2026-09-03 — see S36/G2).
    explicit ServerRegistry(ClientConfig defaultConfig = {}, std::filesystem::path storePath = {});
    ~ServerRegistry();

    struct ServerEntry {
        std::string uuid;
        std::string host;
        uint16_t port;
        std::string serverName;
        ConnectionState state{ConnectionState::kDisconnected};
        std::shared_ptr<SilaClientBase> client;
        // FeatureCatalog is populated on first dynamic call (lazy, §4.2)
    };

    // Manual registration
    void registerServer(std::string uuid, std::string host, uint16_t port,
                       std::string serverName = {});

    // Remove a server entry
    void removeServer(const std::string& uuid);

    // Lookup
    std::optional<ServerEntry> findByUuid(const std::string& uuid) const;
    std::vector<ServerEntry> allServers() const;

    // Connection state callback
    void setConnectionStateCallback(ConnectionStateCallback cb);

    // Update connection state (called by channel state watcher or mDNS goodbye)
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
