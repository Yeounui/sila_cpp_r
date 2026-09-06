// ServerRegistry.cc
#include "ServerRegistry.h"

#include "SilaClientBase.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace sila2 {

ServerRegistry::ServerRegistry(ClientConfig defaultConfig, std::filesystem::path storePath)
    : defaultConfig_{std::move(defaultConfig)}, storePath_{std::move(storePath)} {
    // Empty storePath_ (the default) means persistence is off; loadState()
    // itself also no-ops on an empty path, but skipping the call keeps the
    // common in-memory-only case free of the filesystem::exists() probe.
    if (!storePath_.empty()) {
        loadState();
    }
}
ServerRegistry::~ServerRegistry() = default;

void ServerRegistry::registerServer(std::string uuid, std::string host, uint16_t port,
                                   std::string serverName) {
    // Copy callback + args under lock, invoke after unlock (§2.2c).
    ConnectionStateCallback callbackCopy;
    std::string callbackUuid;
    ConnectionState callbackState{};
    // The store is tab/newline-delimited TSV; a field carrying either would
    // be read back as a corrupt record and make the whole registry unloadable.
    if (!storePath_.empty()) {
        for (const std::string* field : {&uuid, &host, &serverName}) {
            if (field->find_first_of("\t\n") != std::string::npos) {
                throw std::invalid_argument{
                    "ServerRegistry::registerServer: field contains a tab or newline: " + *field};
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock{mu_};

        auto client = std::make_shared<SilaClientBase>(host, port, defaultConfig_);

        auto it = servers_.find(uuid);
        if (it != servers_.end()) {
            // Re-bind: address may have changed after the server rebooted (§3.7).
            it->second.host = std::move(host);
            it->second.port = port;
            it->second.serverName = std::move(serverName);
            it->second.client = std::move(client);
            it->second.state = ConnectionState::kConnecting;

            callbackCopy = stateCallback_;
            callbackUuid = it->second.uuid;
            callbackState = it->second.state;
        } else {
            ServerEntry entry;
            entry.uuid = uuid;
            entry.host = std::move(host);
            entry.port = port;
            entry.serverName = std::move(serverName);
            entry.state = ConnectionState::kConnecting;
            entry.client = std::move(client);

            auto [inserted, _ok] = servers_.emplace(std::move(uuid), std::move(entry));

            callbackCopy = stateCallback_;
            callbackUuid = inserted->second.uuid;
            callbackState = inserted->second.state;
        }

        // Persist the identity fields while still holding mu_, so a concurrent
        // save/load can't observe servers_ and the store file disagreeing.
        if (!storePath_.empty()) {
            saveState();
        }
    }
    if (callbackCopy) {
        callbackCopy(callbackUuid, callbackState);
    }
}

void ServerRegistry::removeServer(const std::string& uuid) {
    std::lock_guard<std::mutex> lock{mu_};
    servers_.erase(uuid);
    if (!storePath_.empty()) {
        saveState();
    }
}

std::optional<ServerRegistry::ServerEntry> ServerRegistry::findByUuid(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = servers_.find(uuid);
    if (it != servers_.end()) return it->second;
    return std::nullopt;
}

std::vector<ServerRegistry::ServerEntry> ServerRegistry::allServers() const {
    std::lock_guard<std::mutex> lock{mu_};
    std::vector<ServerEntry> result;
    result.reserve(servers_.size());
    for (const auto& [uuid, entry] : servers_) {
        result.push_back(entry);
    }
    return result;
}

void ServerRegistry::setConnectionStateCallback(ConnectionStateCallback cb) {
    std::lock_guard<std::mutex> lock{mu_};
    stateCallback_ = std::move(cb);
}

void ServerRegistry::updateState(const std::string& uuid, ConnectionState state) {
    ConnectionStateCallback callbackCopy;
    {
        std::lock_guard<std::mutex> lock{mu_};
        auto it = servers_.find(uuid);
        if (it == servers_.end()) return;
        it->second.state = state;
        callbackCopy = stateCallback_;
    }
    if (callbackCopy) {
        callbackCopy(uuid, state);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// Mirrors ConnectionConfigurationServiceImpl::saveState
// (src/sila/server/features/ConnectionConfigurationServiceImpl.cc:393-440):
// temp-file-then-rename so a write failure leaves the previous valid store
// intact instead of truncating the only copy. Caller holds mu_.
void ServerRegistry::saveState() const {
    const auto tmpPath = std::filesystem::path{storePath_} += ".tmp";
    {
        std::ofstream out{tmpPath, std::ios::trunc};
        if (!out) {
            throw std::runtime_error{
                "ServerRegistry::saveState: could not open " + tmpPath.string()};
        }

        // Identity fields only (uuid/host/port/serverName) — state and client
        // are runtime-only and are never persisted (§9.1 lists them out of
        // scope; a restart always comes back kDisconnected/null-client).
        for (const auto& [uuid, entry] : servers_) {
            out << entry.uuid << '\t' << entry.host << '\t' << entry.port << '\t'
                << entry.serverName << '\n';
        }

        out.flush();
        if (!out) {
            throw std::runtime_error{
                "ServerRegistry::saveState: could not write " + tmpPath.string()};
        }
    }  // close the stream before renaming

    std::error_code ec;
    std::filesystem::rename(tmpPath, storePath_, ec);
    if (ec) {
        throw std::runtime_error{
            "ServerRegistry::saveState: could not replace " + storePath_.string() +
            ": " + ec.message()};
    }
    // Unlike the server (ConnectionConfigurationServiceImpl), there is no RPC
    // whose rollback keeps memory and file in agreement after a throw here:
    // registerServer/removeServer have already mutated servers_ by this point.
    // A disk failure therefore leaves the in-memory registry ahead of the
    // store (fail-loud, not rolled back) — acceptable because the caller sees
    // the exception and the next successful save reconciles the file.
}

// Mirrors ConnectionConfigurationServiceImpl::loadState
// (src/sila/server/features/ConnectionConfigurationServiceImpl.cc:444-539):
// parse into a local map and commit to servers_ only after the whole file
// reads cleanly, so a corrupt record mid-file doesn't leave servers_
// half-populated. Called from the constructor, before any other thread can
// observe this object, so no locking is needed here.
void ServerRegistry::loadState() {
    if (!std::filesystem::exists(storePath_)) {
        return;  // first run: nothing persisted yet, not an error.
    }

    std::ifstream in{storePath_};
    if (!in) {
        throw std::runtime_error{
            "ServerRegistry::loadState: could not open " + storePath_.string()};
    }

    std::map<std::string, ServerEntry, util::CaseInsensitiveLess> loaded;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab - start));
            if (tab == std::string::npos) {
                break;
            }
            start = tab + 1;
        }

        if (fields.size() != 4) {
            throw std::runtime_error{
                "ServerRegistry::loadState: corrupt state file " + storePath_.string()};
        }
        const auto& uuid = fields[0];
        auto host = fields[1];
        const auto& portField = fields[2];
        auto serverName = fields[3];

        // std::stol would accept "80x" (trailing junk) and leading spaces,
        // silently admitting a corrupt field. The store is machine-written as
        // plain digits, so require exactly that and fail loud otherwise
        // (same guard as ConnectionConfigurationServiceImpl.cc:517-529).
        const bool allDigits = !portField.empty() &&
            std::all_of(portField.begin(), portField.end(),
                [](unsigned char c) { return std::isdigit(c); });
        if (!allDigits) {
            throw std::runtime_error{
                "ServerRegistry::loadState: corrupt state file " + storePath_.string()};
        }
        long rawPort = 0;
        try {
            rawPort = std::stol(portField);
        } catch (const std::exception&) {
            throw std::runtime_error{
                "ServerRegistry::loadState: corrupt state file " +
                storePath_.string()};  // out_of_range for an overlong digit run
        }
        if (rawPort < 1 || rawPort > 65535) {
            throw std::runtime_error{
                "ServerRegistry::loadState: corrupt state file " + storePath_.string()};
        }
        const auto port = static_cast<uint16_t>(rawPort);

        // Skip a uuid already live in servers_ or already seen earlier in this
        // same file (a duplicate record) — case-insensitively, per Part A p90.
        if (servers_.count(uuid) > 0 || loaded.count(uuid) > 0) {
            continue;
        }

        ServerEntry entry;
        entry.uuid = uuid;
        entry.host = std::move(host);
        entry.port = port;
        entry.serverName = std::move(serverName);
        entry.state = ConnectionState::kDisconnected;  // client=nullptr until it re-registers/connects.
        loaded.emplace(uuid, std::move(entry));
    }

    // Whole file parsed cleanly — commit.
    for (auto& [uuid, entry] : loaded) {
        servers_.emplace(uuid, std::move(entry));
    }
}

}  // namespace sila2
