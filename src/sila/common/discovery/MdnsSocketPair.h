// MdnsSocketPair.h — Shared IPv4/IPv6 mDNS socket management
#pragma once

#include <mdns.h>
#include <netinet/in.h>

namespace sila2::discovery {

/// The pair of UDP sockets an @ref gl_sila_server_discovery "SiLA Server Discovery" publisher sends
/// and listens on. Used internally by
/// sila2::discovery::MdnsPublisher; a library user does not construct this
/// directly.
///
/// RAII pair of IPv4 + IPv6 mDNS sockets bound to INADDR_ANY on MDNS_PORT.
/// IPv6 failure is tolerated (may be unavailable on some hosts).
struct MdnsSocketPair {
    int ipv4{-1}; ///< -1 when not open.
    int ipv6{-1}; ///< -1 when not open (IPv6 unavailable is tolerated).

    /// Opens both sockets, closing any already open first.
    void open() {
        close();  // avoid leaking fds from a prior open()

        struct sockaddr_in saddr4{};
        saddr4.sin_family = AF_INET;
        saddr4.sin_port = htons(MDNS_PORT);
        saddr4.sin_addr.s_addr = INADDR_ANY;
        ipv4 = mdns_socket_open_ipv4(&saddr4);

        struct sockaddr_in6 saddr6{};
        saddr6.sin6_family = AF_INET6;
        saddr6.sin6_port = htons(MDNS_PORT);
        saddr6.sin6_addr = in6addr_any;
        ipv6 = mdns_socket_open_ipv6(&saddr6);
    }

    /// Closes both sockets, if open. Safe to call when already closed.
    void close() {
        if (ipv4 >= 0) {
            mdns_socket_close(ipv4);
            ipv4 = -1;
        }
        if (ipv6 >= 0) {
            mdns_socket_close(ipv6);
            ipv6 = -1;
        }
    }

    ~MdnsSocketPair() { close(); }

    MdnsSocketPair() = default;
    MdnsSocketPair(const MdnsSocketPair&) = delete;
    MdnsSocketPair& operator=(const MdnsSocketPair&) = delete;
};

}  // namespace sila2::discovery
