// MdnsPublisher.cc
#include "MdnsPublisher.h"

#include <sila/server/LogCallback.h>

#include <mdns.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace sila2::discovery {

namespace {

// SiLA 2 Version (Part A: "the SiLA 2 Version SHALL be '1.1'"). One constant referenced
// once from buildTxtEntries() below, shared by the announce and query-response paths, so
// the two cannot independently drift the way the former duplicated "2" literals did
// (audit S55).
constexpr const char* kSilaVersion = "1.1";

// Truncates value to fit the DNS-SD 255-byte key=value limit (Part B p77 MUST:
// "Key/value pairs exceeding the 255 bytes MUST be truncated"), leaving room for key
// and the '=' separator. Shared by every TXT entry buildTxtEntries() produces.
std::string truncatedTxtValue(const std::string& key, const std::string& value) {
    const std::size_t budget = key.size() + 1 >= 255 ? 0 : 255 - key.size() - 1;
    return MdnsPublisher::truncateUtf8(value, budget);
}

// Set by probeCallback() when an existing SRV answer for the probed name is found.
struct ProbeResult {
    bool taken = false;
};

int probeCallback(int, const struct sockaddr*, size_t, mdns_entry_type_t entry, uint16_t,
                   uint16_t rtype, uint16_t, uint32_t, const void*, size_t, size_t, size_t, size_t,
                   size_t, void* user_data) {
    if (entry == MDNS_ENTRYTYPE_ANSWER && rtype == MDNS_RECORDTYPE_SRV) {
        static_cast<ProbeResult*>(user_data)->taken = true;
        return 1;  // stop parsing, we already have our answer
    }
    return 0;
}

}  // namespace

// Shared record-building logic for announceRecords(), goodbyeRecords(), and the static
// listenCallback(). Static member (not a free function) so it is unit-testable without
// opening a socket (test_mdns_publisher.cc), the same buildTxtEntries()-style seam. All
// string arguments are owned by the caller and must outlive this call plus the subsequent
// send, since the built records only hold pointers into them.
void MdnsPublisher::appendServiceRecords(mdns_record_t& ptrRecord,
                                         std::vector<mdns_record_t>& additional,
                                         const std::string& serviceType,
                                         const std::string& instanceQualified,
                                         const std::string& hostnameLocal, uint16_t port,
                                         const std::vector<TxtEntry>& txtEntries,
                                         const std::vector<struct sockaddr_in>& addrsV4) {
    ptrRecord = mdns_record_t{};
    ptrRecord.name = {serviceType.data(), serviceType.size()};
    ptrRecord.type = MDNS_RECORDTYPE_PTR;
    ptrRecord.data.ptr.name = {instanceQualified.data(), instanceQualified.size()};

    mdns_record_t srvRecord{};
    srvRecord.name = {instanceQualified.data(), instanceQualified.size()};
    srvRecord.type = MDNS_RECORDTYPE_SRV;
    srvRecord.data.srv.priority = 0;
    srvRecord.data.srv.weight = 0;
    srvRecord.data.srv.port = port;
    srvRecord.data.srv.name = {hostnameLocal.data(), hostnameLocal.size()};
    additional.push_back(srvRecord);

    // One TXT record per entry (uuid, version, server_name, description, ca<l>=...) --
    // see buildTxtEntries() (Part B p77 SHOULD / p75-76 MUST).
    for (const auto& txt : txtEntries) {
        mdns_record_t txtRecord{};
        txtRecord.name = {instanceQualified.data(), instanceQualified.size()};
        txtRecord.type = MDNS_RECORDTYPE_TXT;
        txtRecord.data.txt.key = {txt.key.data(), txt.key.size()};
        txtRecord.data.txt.value = {txt.value.data(), txt.value.size()};
        additional.push_back(txtRecord);
    }

    for (const auto& addr : addrsV4) {
        mdns_record_t aRecord{};
        aRecord.name = {hostnameLocal.data(), hostnameLocal.size()};
        aRecord.type = MDNS_RECORDTYPE_A;
        aRecord.data.a.addr = addr;
        additional.push_back(aRecord);
    }
    // No AAAA loop: IPv6 addresses are not advertised (audit S76 -- the gRPC server
    // binds IPv4-only, Part B p75).
}

MdnsPublisher::MdnsPublisher(const std::string& uuid, const std::string& serverName,
                              const std::string& description, const std::string& caCertPem,
                              uint16_t port, std::chrono::seconds readvertiseInterval,
                              std::chrono::seconds recordTtl, std::chrono::milliseconds probeWait)
    : instanceName_{uuid},  // Part B p76 MUST: the mDNS instance name is the SiLA Server UUID
      uuid_{uuid},
      serverName_{serverName},
      description_{description},
      caCertPem_{caCertPem},
      port_{port},
      readvertiseInterval_{readvertiseInterval},
      recordTtl_{recordTtl},
      probeWait_{probeWait} {
    // Reject a CA that cannot be advertised instead of letting every announce fail with
    // mdns.h's -1 (Codex review of fd62b52). Part B p76 mandates the whole PEM in TXT
    // lines, so the only correct answer to an oversize CA is to refuse it up front.
    const std::size_t txtBytes = txtWireBytes(uuid, buildTxtEntries(uuid, serverName, description, caCertPem));
    if (txtBytes + kFixedRecordsBudget > kPacketCapacity) {
        throw std::invalid_argument{"MdnsPublisher: TXT records (" + std::to_string(txtBytes) +
                                    " bytes, mostly ca<l>= lines) exceed the " +
                                    std::to_string(kPacketCapacity) + "-byte mDNS packet limit"};
    }
    char nameBuf[256]{};
    if (gethostname(nameBuf, sizeof(nameBuf)) != 0) {
        hostname_ = "localhost.local.";
        return;
    }
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    hostname_ = std::string{nameBuf} + ".local.";
}

MdnsPublisher::~MdnsPublisher() {
    shutdown();
}

void MdnsPublisher::stopListening() {
    readvertiseTimer_.stop();
    running_ = false;
    if (listenThread_.joinable()) {
        listenThread_.join();
    }
}

void MdnsPublisher::publish() {
    // Whole body under restartMu_, same as setName()/shutdown() — see the class doc.
    std::lock_guard<std::mutex> restart{restartMu_};
    // A second publish() call (or one after setName()/shutdown()) must not assign over
    // a joinable listenThread_ — assigning to a joinable std::thread calls std::terminate.
    // stopListening() is a no-op if nothing is running yet.
    stopListening();
    collectLocalAddresses();

    std::string current;
    {
        std::lock_guard<std::mutex> lock{mu_};
        current = instanceName_;
    }
    std::string resolved = resolveInstanceName(current);
    {
        std::lock_guard<std::mutex> lock{mu_};
        instanceName_ = resolved;
    }

    sockets_.open();
    if (sockets_.ipv4 < 0 && sockets_.ipv6 < 0) {
        throw std::runtime_error{"MdnsPublisher: failed to open mDNS sockets"};
    }
    if (!announceRecords()) {
        // running_ is still false here, so a later shutdown() would return early and
        // leak the fds until destruction; release them on this path ourselves.
        sockets_.close();
        throw std::runtime_error{"MdnsPublisher: mDNS announce was not sent on any socket"};
    }
    socketsOpen_ = true;
    // running_ must be set before starting the listen thread, since listenLoop() exits its
    // loop as soon as it observes running_ == false.
    running_ = true;
    listenThread_ = std::thread{&MdnsPublisher::listenLoop, this};
    readvertiseTimer_.start(readvertiseInterval_);
}

void MdnsPublisher::setName(const std::string& name) {
    // Whole body under restartMu_, same as publish()/shutdown() — see the class doc.
    // Not for listenThread_ here (setName() no longer restarts it) but so a
    // concurrent publish()/shutdown() cannot observe serverName_ half-updated.
    std::lock_guard<std::mutex> restart{restartMu_};

    // Part B p76 MUST fixes the mDNS instance name to the SiLA Server UUID; only the
    // human-readable server_name TXT entry changes here (Part B p77 SHOULD). No goodbye,
    // no re-probe, no listen-thread restart: the instance never moves. The re-announce
    // (MDNS_CACHE_FLUSH) is a no-op before publish(), since no sockets are open yet.
    {
        std::lock_guard<std::mutex> lock{mu_};
        serverName_ = name;
    }
    announceRecords();
}

void MdnsPublisher::shutdown() {
    std::lock_guard<std::mutex> restart{restartMu_};
    if (!running_.exchange(false)) {
        return;  // already shut down
    }
    stopListening();  // running_ is already false, but the timer/thread still need stopping
    goodbyeRecords();
    sockets_.close();
    socketsOpen_ = false;
}

void MdnsPublisher::setPort(uint16_t port) { port_ = port; }
uint16_t MdnsPublisher::port() const { return port_.load(); }
bool MdnsPublisher::isPublishing() const {
    // running_ alone would suffice now that setName() no longer touches it (S67: it
    // only updates the server_name TXT record), but socketsOpen_ is kept as the
    // data-race-free signal for the cross-thread read either way: sockets_'s raw
    // fds are plain ints guarded by restartMu_, which isPublishing() must not take.
    return running_.load() && socketsOpen_.load();
}

std::string MdnsPublisher::truncateUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) {
        return s;
    }
    std::size_t boundary = maxBytes;
    // Walk back to a byte that isn't a UTF-8 continuation byte (0b10xxxxxx), so we don't
    // split a multi-byte code point in half.
    while (boundary > 0 && (static_cast<unsigned char>(s[boundary]) & 0xC0) == 0x80) {
        --boundary;
    }
    return s.substr(0, boundary);
}

std::vector<MdnsPublisher::TxtEntry> MdnsPublisher::buildTxtEntries(
    const std::string& uuid, const std::string& serverName, const std::string& description,
    const std::string& caCertPem) {
    std::vector<TxtEntry> entries;
    entries.push_back({"uuid", uuid});
    entries.push_back({"version", kSilaVersion});
    // Part B p77 SHOULD: server_name/description are published as best-effort properties.
    // An empty description is published as an empty value rather than omitted -- the spec
    // treats an empty value as "ignored as if not present", so this stays branch-free.
    entries.push_back({"server_name", truncatedTxtValue("server_name", serverName)});
    entries.push_back({"description", truncatedTxtValue("description", description)});

    // Part B p75-76 MUST: an untrusted certificate's CA is published as one ca<l>=
    // TXT entry per PEM line, l being the 0-indexed line number -- NOT fixed-size
    // chunking, since the standard is explicit that lines are "stored separately".
    if (!caCertPem.empty()) {
        std::size_t line = 0;
        std::size_t start = 0;
        while (start < caCertPem.size()) {
            std::size_t nl = caCertPem.find('\n', start);
            std::size_t end = (nl == std::string::npos) ? caCertPem.size() : nl;
            std::string content = caCertPem.substr(start, end - start);
            // Blank lines (interior or the trailing-newline artifact) are skipped rather
            // than published as an empty ca<l>=, so l stays the consecutive count of the
            // certificate's non-empty lines.
            if (!content.empty()) {
                const std::string key = "ca" + std::to_string(line);
                entries.push_back({key, truncatedTxtValue(key, content)});
                ++line;
            }
            if (nl == std::string::npos) {
                break;
            }
            start = nl + 1;
        }
    }
    return entries;
}

// 63 bytes and the suffix arithmetic below: see truncateUtf8's contract in
// MdnsPublisher.h -- the divergence from the 255-character ServerName is
// deliberate.
std::string MdnsPublisher::resolveInstanceName(const std::string& serverName) {
    std::string candidate = truncateUtf8(serverName, 63);
    if (probeName(candidate)) {
        return candidate;
    }

    for (int attempt = 2; attempt <= 10; ++attempt) {
        std::string suffix = " (" + std::to_string(attempt) + ")";
        candidate = truncateUtf8(serverName, 63 - suffix.size()) + suffix;
        if (probeName(candidate)) {
            return candidate;
        }
    }
    return candidate;  // give up after 10 attempts, use the last candidate tried
}

bool MdnsPublisher::probeName(const std::string& candidateName) {
    std::string fullName = candidateName + "._sila._tcp.local.";
    int probeSock = mdns_socket_open_ipv4(nullptr);
    if (probeSock < 0) {
        logEvent(defaultLogCallback(), LogLevel::kWarning, "discovery",
                 "mDNS probe socket unavailable; publishing without conflict probe");
        return true;
    }

    alignas(4) uint8_t buffer[256];
    mdns_query_send(probeSock, MDNS_RECORDTYPE_SRV, fullName.c_str(), fullName.size(), buffer,
                    sizeof(buffer), 0);

    ProbeResult result;
    struct pollfd fd {
        probeSock, POLLIN, 0
    };
    int ret = poll(&fd, 1, static_cast<int>(probeWait_.count()));
    if (ret > 0 && (fd.revents & POLLIN)) {
        mdns_query_recv(probeSock, buffer, sizeof(buffer), probeCallback, &result, 0);
    }

    mdns_socket_close(probeSock);
    return !result.taken;
}

void MdnsPublisher::collectLocalAddresses() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return;
    }

    std::vector<struct sockaddr_in> addrsV4;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || (ifa->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }
        // Only IPv4: the gRPC server binds 0.0.0.0, so an AAAA address would point
        // clients at a socket nothing listens on (audit S76).
        if (ifa->ifa_addr->sa_family == AF_INET) {
            addrsV4.push_back(*reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr));
        }
    }
    freeifaddrs(ifaddr);

    std::lock_guard<std::mutex> lock{mu_};
    addrsV4_ = std::move(addrsV4);
}

void MdnsPublisher::buildRecords(Records& out) {
    static_assert(!std::is_copy_constructible_v<Records> && !std::is_move_constructible_v<Records>,
                  "Records holds mdns_string_t pointers into its own storage; a copy or move "
                  "leaves them pointing at the source object");
    // appendServiceRecords() appends rather than assigns, so a reused Records would
    // otherwise accumulate a second SRV/TXT/A set on top of the stale one.
    out.additional.clear();
    out.serviceType = "_sila._tcp.local.";
    out.hostnameLocal = hostname_;

    std::string uuid;
    std::string serverName;
    {
        std::lock_guard<std::mutex> lock{mu_};
        out.instanceQualified = instanceName_ + "._sila._tcp.local.";
        uuid = uuid_;
        serverName = serverName_;
        out.addrsV4 = addrsV4_;
    }
    // description_/caCertPem_ are set once at construction and never modified after, so
    // they can be read here without mu_.
    out.txtEntries = buildTxtEntries(uuid, serverName, description_, caCertPem_);

    appendServiceRecords(out.ptr, out.additional, out.serviceType, out.instanceQualified,
                         out.hostnameLocal, port_.load(), out.txtEntries,
                         out.addrsV4);
}

std::size_t MdnsPublisher::txtWireBytes(const std::string& uuid, const std::vector<TxtEntry>& entries) {
    // Uncompressed owner name: each label is length-prefixed and the name ends in a 0 byte,
    // so "<uuid>._sila._tcp.local." costs uuid + strlen("._sila._tcp.local.") + 1.
    const std::size_t ownerName = uuid.size() + sizeof("._sila._tcp.local.");
    // RR header: type(2) class(2) ttl(4) rdlength(2); the TXT string adds a 1-byte length.
    constexpr std::size_t kRecordOverhead = 10 + 1;
    std::size_t total = 0;
    for (const auto& entry : entries) {
        total += ownerName + kRecordOverhead + entry.key.size() + 1 + entry.value.size();
    }
    return total;
}

bool MdnsPublisher::announceRecords() {
    Records records;
    buildRecords(records);

    alignas(4) uint8_t buffer[kPacketCapacity];
    auto ttlSec = static_cast<uint32_t>(recordTtl_.count());
    bool sent = false;
    // mdns_announce_multicast() hardcodes TTL to 60, so we call the underlying rclass/ttl
    // function directly to honor the configured recordTtl_.
    for (int sock : {sockets_.ipv4, sockets_.ipv6}) {
        if (sock < 0) {
            continue;
        }
        const int rc = mdns_answer_multicast_rclass_ttl(sock, buffer, sizeof(buffer), records.ptr, nullptr, 0,
                                                        records.additional.data(), records.additional.size(),
                                                        MDNS_CLASS_IN | MDNS_CACHE_FLUSH, ttlSec);
        if (rc < 0) {
            logEvent(defaultLogCallback(), LogLevel::kWarning, "discovery",
                     "mDNS announce failed (packet too large or send error)");
        } else {
            sent = true;
        }
    }
    return sent;
}

void MdnsPublisher::goodbyeRecords() {
    Records records;
    buildRecords(records);

    alignas(4) uint8_t buffer[kPacketCapacity];
    if (sockets_.ipv4 >= 0) {
        mdns_goodbye_multicast(sockets_.ipv4, buffer, sizeof(buffer), records.ptr, nullptr, 0,
                               records.additional.data(), records.additional.size());
    }
    if (sockets_.ipv6 >= 0) {
        mdns_goodbye_multicast(sockets_.ipv6, buffer, sizeof(buffer), records.ptr, nullptr, 0,
                               records.additional.data(), records.additional.size());
    }
}

void MdnsPublisher::listenLoop() {
    alignas(4) uint8_t recvBuf[2048];
    alignas(4) uint8_t sendBuf[kPacketCapacity];
    ListenContext ctx{this, sendBuf, sizeof(sendBuf)};

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
            if (fds[i].revents & POLLIN) {
                mdns_socket_listen(fds[i].fd, recvBuf, sizeof(recvBuf), listenCallback, &ctx);
            }
        }
    }
}

int MdnsPublisher::listenCallback(int sock, const struct sockaddr* /*from*/, size_t /*addrlen*/,
                                   mdns_entry_type_t entry, uint16_t /*query_id*/, uint16_t rtype,
                                   uint16_t /*rclass*/, uint32_t /*ttl*/, const void* data,
                                   size_t size, size_t name_offset, size_t /*name_length*/,
                                   size_t /*record_offset*/, size_t /*record_length*/,
                                   void* user_data) {
    if (entry != MDNS_ENTRYTYPE_QUESTION) {
        return 0;
    }

    auto* ctx = static_cast<ListenContext*>(user_data);
    auto* self = ctx->self;

    char nameBuf[256];
    size_t offset = name_offset;
    mdns_string_t name = mdns_string_extract(data, size, &offset, nameBuf, sizeof(nameBuf));
    std::string questionName{name.str, name.length};

    std::lock_guard<std::mutex> lock{self->mu_};

    std::string serviceType = "_sila._tcp.local.";
    std::string instanceQualified = self->instanceName_ + "._sila._tcp.local.";

    bool isServiceQuery = (questionName == serviceType) &&
                          (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY);
    bool isInstanceQuery = (questionName == instanceQualified) &&
                           (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_ANY);
    if (!isServiceQuery && !isInstanceQuery) {
        return 0;
    }

    std::string hostnameLocal = self->hostname_;
    // description_/caCertPem_ need no lock (S54): set once at construction, never
    // modified after. uuid_/serverName_ are read here under mu_, already held above.
    std::vector<MdnsPublisher::TxtEntry> txtEntries =
        buildTxtEntries(self->uuid_, self->serverName_, self->description_, self->caCertPem_);

    mdns_record_t ptrRecord{};
    std::vector<mdns_record_t> additional;
    appendServiceRecords(ptrRecord, additional, serviceType, instanceQualified, hostnameLocal,
                         self->port_.load(), txtEntries, self->addrsV4_);

    auto ttlSec = static_cast<uint32_t>(self->recordTtl_.count());
    // Query responses use plain MDNS_CLASS_IN (no CACHE_FLUSH). Only unsolicited
    // announcements set CACHE_FLUSH to tell receivers to replace stale cache entries.
    const int rc = mdns_answer_multicast_rclass_ttl(sock, ctx->sendBuffer, ctx->sendCapacity, ptrRecord,
                                                    nullptr, 0, additional.data(), additional.size(),
                                                    MDNS_CLASS_IN, ttlSec);
    if (rc < 0) {
        logEvent(defaultLogCallback(), LogLevel::kWarning, "discovery",
                 "mDNS query response failed (packet too large or send error)");
    }
    return 0;
}

}  // namespace sila2::discovery
