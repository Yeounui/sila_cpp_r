// MdnsBrowser.h — mDNS browser for _sila._tcp service discovery
#pragma once

#include <sila/common/discovery/MdnsSocketPair.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sila2::discovery {

/// One SiLA server discovered (or re-discovered) via mDNS: the SRV target
/// host/port, the IPv4 address from the A record (dotted literal, empty when
/// the response carried none), the UUID advertised in its TXT record, and
/// the mDNS instance name shown to users. Connect to `address` when it is
/// set: ClientConfig::channelCredentials(host) applies Part B p75's
/// zero-config untrusted-TLS rule only to a private-range IP literal, and
/// the SRV `<hostname>.local.` target never qualifies (Codex finding on
/// SC30/S74, 2026-09-04).
struct ResolveEvent {
    std::string host;     ///< SRV target hostname (`<name>.local.`).
    std::string address;  ///< IPv4 address from the A record, dotted literal; empty if none was sent.
    uint16_t port;         ///< SRV target port.
    std::string uuid;     ///< @ref gl_sila_server_uuid "UUID" advertised in the TXT record.
    std::string name;     ///< mDNS instance name shown to users.
};

/// Client side of @ref gl_sila_server_discovery "SiLA Server Discovery":
/// discovers SiLA servers via mDNS by sending a PTR query for
/// _sila._tcp.local and listening for responses. Runs a receive thread that
/// assembles the
/// PTR/SRV/TXT/A records of each response into a ResolveEvent and
/// invokes the caller-supplied callbacks.
///
/// Thread-safe: onResolve_/onGoodbye_ are guarded by mu_ so browse()/stop()
/// may run concurrently with the receive thread.
class MdnsBrowser {
public:
    MdnsBrowser();

    // Defined in the .cc: stop() joins the receive thread, which must be
    // fully torn down before the socket fds it references go out of scope.
    ~MdnsBrowser();

    MdnsBrowser(const MdnsBrowser&) = delete;
    MdnsBrowser& operator=(const MdnsBrowser&) = delete;

    /// Opens a socket pair, sends a PTR query for _sila._tcp.local, and starts
    /// a receive thread that calls onResolve for each discovered server and
    /// onGoodbye (with the server's UUID) when a goodbye is received.
    void browse(std::function<void(const ResolveEvent&)> onResolve,
                std::function<void(const std::string& uuid)> onGoodbye);

    /// Closes sockets and stops the receive thread. Safe to call more than once.
    void stop();

private:
    // Bundles the state the mdns_socket_listen callback needs without
    // exposing `this` through a raw void* cast at every call site. Also
    // accumulates the PTR/SRV/TXT records of a single response packet so
    // they can be assembled into one ResolveEvent once the packet is done.
    struct BrowseContext {
        std::string instanceName;
        std::string host;
        // Every A record of the packet as (owner name, dotted IPv4); the one
        // whose owner name equals the SRV target becomes ResolveEvent::address.
        std::vector<std::pair<std::string, std::string>> aRecords;
        uint16_t port = 0;
        std::string uuid;
        bool hasPtr = false;
        bool hasSrv = false;
        bool goodbye = false;
    };

    void receiveLoop();
    static int browseCallback(int sock, const struct sockaddr* from, size_t addrlen,
                               mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                               uint16_t rclass, uint32_t ttl, const void* data, size_t size,
                               size_t name_offset, size_t name_length, size_t record_offset,
                               size_t record_length, void* user_data);

    std::function<void(const ResolveEvent&)> onResolve_;
    std::function<void(const std::string&)> onGoodbye_;
    MdnsSocketPair sockets_;
    std::atomic<bool> running_{false};
    std::thread receiveThread_;
    mutable std::mutex mu_;
};

}  // namespace sila2::discovery
