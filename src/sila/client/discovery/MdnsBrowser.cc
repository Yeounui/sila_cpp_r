// MdnsBrowser.cc
#include "MdnsBrowser.h"

#include <sila/common/util/AsciiCase.h>

#include <mdns.h>

#include <arpa/inet.h>
#include <poll.h>

#include <cstring>

namespace sila2::discovery {

MdnsBrowser::MdnsBrowser() = default;

MdnsBrowser::~MdnsBrowser() {
    stop();
}

void MdnsBrowser::browse(std::function<void(const ResolveEvent&)> onResolve,
                          std::function<void(const std::string& uuid)> onGoodbye) {
    stop();
    {
        std::lock_guard<std::mutex> lock{mu_};
        onResolve_ = std::move(onResolve);
        onGoodbye_ = std::move(onGoodbye);
    }

    sockets_.open();

    alignas(4) uint8_t buf[2048];
    std::string serviceType = "_sila._tcp.local.";
    if (sockets_.ipv4 >= 0) {
        mdns_query_send(sockets_.ipv4, MDNS_RECORDTYPE_PTR, serviceType.c_str(), serviceType.size(),
                        buf, sizeof(buf), 0);
    }

    // running_ must be set before starting the receive thread, since receiveLoop() exits its
    // loop as soon as it observes running_ == false.
    running_ = true;
    receiveThread_ = std::thread{&MdnsBrowser::receiveLoop, this};
}

void MdnsBrowser::stop() {
    running_ = false;
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    sockets_.close();
}

void MdnsBrowser::receiveLoop() {
    alignas(4) uint8_t buf[2048];
    BrowseContext ctx{};

    while (running_.load()) {
        struct pollfd fds[2];
        int nfds = 0;
        if (sockets_.ipv4 >= 0) {
            fds[nfds] = {sockets_.ipv4, POLLIN, 0};
            ++nfds;
        }
        if (sockets_.ipv6 >= 0) {
            fds[nfds] = {sockets_.ipv6, POLLIN, 0};
            ++nfds;
        }
        if (nfds == 0) {
            break;
        }

        int ret = poll(fds, nfds, 1000);  // 1 second timeout so running_ is rechecked periodically
        if (ret <= 0) {
            continue;
        }

        for (int i = 0; i < nfds; ++i) {
            if (!(fds[i].revents & POLLIN)) {
                continue;
            }

            // Reset accumulation state before parsing this packet's records.
            ctx.instanceName.clear();
            ctx.host.clear();
            ctx.aRecords.clear();
            ctx.port = 0;
            ctx.uuid.clear();
            ctx.hasPtr = false;
            ctx.hasSrv = false;
            ctx.goodbye = false;

            mdns_socket_listen(fds[i].fd, buf, sizeof(buf), browseCallback, &ctx);

            if (ctx.goodbye && !ctx.uuid.empty()) {
                std::lock_guard<std::mutex> lock{mu_};
                if (onGoodbye_) {
                    onGoodbye_(ctx.uuid);
                }
            } else if (ctx.hasPtr && ctx.hasSrv && !ctx.uuid.empty()) {
                ResolveEvent event;
                event.host = std::move(ctx.host);
                // Only the A record owned by the SRV target names this
                // service's socket; a response may also carry A records for
                // other hosts. DNS names compare case-insensitively.
                for (auto& [owner, address] : ctx.aRecords) {
                    if (util::asciiLower(owner) == util::asciiLower(event.host)) {
                        event.address = std::move(address);
                        break;
                    }
                }
                event.port = ctx.port;
                event.uuid = std::move(ctx.uuid);
                event.name = std::move(ctx.instanceName);

                std::lock_guard<std::mutex> lock{mu_};
                if (onResolve_) {
                    onResolve_(event);
                }
            }
        }
    }
}

int MdnsBrowser::browseCallback(int /*sock*/, const struct sockaddr* /*from*/, size_t /*addrlen*/,
                                 mdns_entry_type_t entry, uint16_t /*query_id*/, uint16_t rtype,
                                 uint16_t /*rclass*/, uint32_t ttl, const void* data, size_t size,
                                 size_t name_offset, size_t /*name_length*/,
                                 size_t record_offset, size_t record_length, void* user_data) {
    // We're only browsing (not answering), so questions from other browsers are irrelevant.
    if (entry != MDNS_ENTRYTYPE_ANSWER && entry != MDNS_ENTRYTYPE_ADDITIONAL) {
        return 0;
    }

    auto* ctx = static_cast<BrowseContext*>(user_data);

    if (rtype == MDNS_RECORDTYPE_PTR) {
        char nameBuf[256];
        mdns_string_t ptrName = mdns_record_parse_ptr(data, size, record_offset, record_length,
                                                       nameBuf, sizeof(nameBuf));
        std::string fullPtr{ptrName.str, ptrName.length};
        auto pos = fullPtr.find("._sila._tcp.local.");
        if (pos != std::string::npos) {
            ctx->instanceName = fullPtr.substr(0, pos);
            ctx->hasPtr = true;
        }
        if (ttl == 0) {
            ctx->goodbye = true;  // TTL 0 is the mDNS convention for a goodbye/withdrawal
        }
    } else if (rtype == MDNS_RECORDTYPE_SRV) {
        char nameBuf[256];
        mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length,
                                                       nameBuf, sizeof(nameBuf));
        ctx->host = std::string{srv.name.str, srv.name.length};
        ctx->port = srv.port;
        ctx->hasSrv = true;
    } else if (rtype == MDNS_RECORDTYPE_TXT) {
        mdns_record_txt_t txtbuf[16];
        size_t parsed = mdns_record_parse_txt(data, size, record_offset, record_length, txtbuf,
                                              sizeof(txtbuf) / sizeof(txtbuf[0]));
        for (size_t i = 0; i < parsed; ++i) {
            std::string key{txtbuf[i].key.str, txtbuf[i].key.length};
            if (key == "uuid") {
                ctx->uuid = std::string{txtbuf[i].value.str, txtbuf[i].value.length};
            }
        }
    } else if (rtype == MDNS_RECORDTYPE_A) {
        // The A record carries the IPv4 socket the server listens on (S76:
        // the publisher advertises A records only). Keep it as a dotted
        // literal so the caller can hand it to ClientConfig::channelCredentials,
        // whose private-range zero-config TLS rule needs an IP literal, not
        // the SRV hostname. Record it with its owner name: receiveLoop picks
        // the one owned by the SRV target (Codex P2 on 91513e6 — a packet may
        // carry A records for unrelated hosts).
        char nameBuf[256];
        size_t nameOffset = name_offset;
        mdns_string_t owner = mdns_string_extract(data, size, &nameOffset, nameBuf, sizeof(nameBuf));
        struct sockaddr_in addr{};
        if (mdns_record_parse_a(data, size, record_offset, record_length, &addr) != nullptr) {
            char text[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text)) != nullptr) {
                ctx->aRecords.emplace_back(std::string{owner.str, owner.length}, text);
            }
        }
    }
    // AAAA records are ignored: the server binds IPv4 only and stopped advertising them (S76).

    return 0;
}

}  // namespace sila2::discovery
