// MdnsPublisher.h — mDNS publisher for _sila._tcp service advertisement
#pragma once

#include <sila/common/discovery/MdnsSocketPair.h>
#include <sila/common/util/PeriodicGC.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sila2::discovery {

/// This library's implementation of @ref gl_sila_server_discovery "SiLA Server Discovery" : lets a
/// @ref gl_sila_client "SiLA Client" find this
/// server on the local network with no prior configuration. Installed and
/// owned by `SilaServerBase` -- always on (Part B p75 MUST) -- so a server
/// author never constructs one directly; `SilaServerBase::mdnsPublisher()`
/// exposes this one for inspection.
///
/// Advertises a SiLA server via mDNS multicast on the well-known _sila._tcp
/// service type (SiLA 2 §5.4 Discovery). The mDNS Service Instance Name is
/// the SiLA Server UUID (Part B p76 MUST, RFC 4122 string) -- globally
/// unique, so no conflict-retry suffix is ever needed for it in practice.
/// Announces SRV/TXT/A records, keeps a listen thread alive to answer
/// PTR/SRV queries, and periodically re-announces so late-joining clients
/// still discover the server without waiting for the initial burst.
///
/// Thread-safe: instanceName_, serverName_, and the local address caches are
/// guarded by mu_, so the listen thread and a caller of
/// publish()/setName()/shutdown() may run concurrently. restartMu_
/// additionally serializes the bodies of publish()/setName()/shutdown()
/// against each other, so it is safe to call any of the three from any
/// thread (e.g. SetServerName's setName() racing a concurrent shutdown())
/// without two of them assigning to the joinable listenThread_ at once,
/// which would call std::terminate.
class MdnsPublisher {
public:
    /// One DNS-SD TXT record key/value pair (RFC 6763 6.3).
    struct TxtEntry {
        std::string key;    ///< DNS-SD TXT record key.
        std::string value;  ///< DNS-SD TXT record value.
    };

    /// Constructs the publisher without opening any sockets; publish() starts advertising.
    ///
    /// @param uuid mDNS Service Instance Name source (Part B p76 MUST).
    /// @param serverName Feeds the server_name TXT entry (Part B p77 SHOULD).
    /// @param description Feeds the description TXT entry (Part B p77 SHOULD).
    /// @param caCertPem The untrusted certificate's own CA, split into ca<l>=
    ///        TXT entries (Part B p75-76 MUST) when non-empty; pass "" for a
    ///        trusted certificate.
    /// @param port TCP port advertised in the SRV record (the gRPC listen port).
    /// @param readvertiseInterval How often the records are re-announced.
    /// @param recordTtl Time-to-live of the published records.
    /// @param probeWait How long to wait for conflicting probe responses before claiming the name.
    /// @throws std::invalid_argument if the resulting TXT records would not
    ///         fit in one mDNS packet (see txtWireBytes()).
    MdnsPublisher(const std::string& uuid, const std::string& serverName,
                  const std::string& description, const std::string& caCertPem,
                  uint16_t port, std::chrono::seconds readvertiseInterval,
                  std::chrono::seconds recordTtl,
                  std::chrono::milliseconds probeWait);

    // Defined in the .cc: shutdown() joins the listen thread and stops the
    // readvertise timer, both of which must be fully torn down before the
    // socket fds and callback context they reference go out of scope.
    ~MdnsPublisher();

    MdnsPublisher(const MdnsPublisher&) = delete;
    MdnsPublisher& operator=(const MdnsPublisher&) = delete;

    /// Probes for a unique instance name, announces the service records, and
    /// starts the listen thread plus the periodic readvertise timer.
    void publish();

    /// Updates the server_name TXT record to `name` and re-announces
    /// (MDNS_CACHE_FLUSH, so browsers replace the cached TXT). Does not
    /// touch the mDNS Service Instance Name -- that stays the SiLA Server
    /// UUID for the object's lifetime (Part B p76 MUST). A no-op beyond
    /// recording the name when called before publish(), since no sockets
    /// are open yet to send anything on.
    void setName(const std::string& name);

    /// Sends a goodbye, stops the listen thread and readvertise timer, and
    /// closes the sockets. Safe to call more than once.
    void shutdown();

    /// Sets the port the SRV record advertises. Must be called before publish():
    /// SilaServerBase::run() calls it once BuildAndStart has written back the
    /// port the OS actually bound, which is the only moment that number exists
    /// for a server built with withDiscovery(0). port_ is atomic so a later
    /// call cannot race the listen thread's read in listenCallback().
    void setPort(uint16_t port);

    /// @return The port the SRV record currently advertises.
    [[nodiscard]]
    uint16_t port() const;

    /// @return True once publish() has opened sockets and announced; false
    /// before publish() -- even after a setName() that preceded any
    /// publish() -- and after shutdown(). Exposed because
    /// the only other way to observe an advertisement is a multicast round
    /// trip, which cannot run on hosts where mDNS loopback is unavailable
    /// (see test_mdns_browser.cc:31-41).
    [[nodiscard]]
    bool isPublishing() const;

    /// Appends the SRV/TXT/A records for one announce or query-response to
    /// `additional` and fills `ptrRecord`. Public and static like
    /// buildTxtEntries() so the emitted record set is unit-testable without a
    /// live socket (test_mdns_publisher.cc); shared by buildRecords() (announce)
    /// and listenCallback() (query response). No AAAA record is emitted: the
    /// gRPC server binds IPv4-only, and Part B p75 requires the advertised
    /// address to represent the serving socket (audit S76).
    static void appendServiceRecords(mdns_record_t& ptrRecord,
                                     std::vector<mdns_record_t>& additional,
                                     const std::string& serviceType,
                                     const std::string& instanceQualified,
                                     const std::string& hostnameLocal, uint16_t port,
                                     const std::vector<TxtEntry>& txtEntries,
                                     const std::vector<struct sockaddr_in>& addrsV4);

    /// Builds the full TXT entry set for one announce/query-response: uuid,
    /// version (SiLA 2 Version, Part A "SHALL be 1.1"), server_name and
    /// description (Part B p77 SHOULD, each truncated to the 255-byte
    /// key=value limit), and -- when caCertPem is non-empty, i.e. the
    /// server's certificate is untrusted -- one ca<l>= entry per non-blank
    /// PEM line, l being the 0-indexed line number (Part B p75-76 MUST).
    /// Public and static so it is unit-testable without opening a socket
    /// (test_mdns_publisher.cc); shared by buildRecords() (announce path)
    /// and listenCallback() (query-response path) so the two TXT sets
    /// cannot drift apart (audit S54/S55/S56).
    static std::vector<TxtEntry> buildTxtEntries(const std::string& uuid,
                                                  const std::string& serverName,
                                                  const std::string& description,
                                                  const std::string& caCertPem);

    /// Cuts s to at most maxBytes without splitting a UTF-8 code point.
    ///
    /// Caps DNS-SD TXT record values -- server_name, description, and the
    /// per-line ca<l>= certificate entries -- at the 255-byte key=value
    /// limit (Part B p77 MUST: "Key/value pairs exceeding the 255 bytes MUST
    /// be truncated"). It also still backs resolveInstanceName()'s 63-byte
    /// DNS label cap (RFC 6763 4.1.1), though that path is effectively a
    /// no-op for the mDNS instance name now: Part B p76 fixes the instance
    /// to the SiLA Server UUID (36 ASCII bytes, well under 63), so the
    /// conflict-retry suffix below is unreachable for a real RFC 4122 UUID.
    static std::string truncateUtf8(const std::string& s, std::size_t maxBytes);

    /// Largest mDNS message we ever serialize (RFC 6762 17: "MUST NOT exceed 9000
    /// bytes"). mdns.h returns -1 once a packet outgrows its buffer, so every
    /// send buffer is exactly this size and the constructor rejects a TXT set
    /// that cannot fit (see txtWireBytes()).
    static constexpr std::size_t kPacketCapacity = 9000;

    /// Bytes the fixed PTR/SRV/A records plus the DNS header may take:
    /// hostname (<= 255) and up to ~20 interface addresses. The TXT records get
    /// the rest of kPacketCapacity.
    static constexpr std::size_t kFixedRecordsBudget = 2048;

    /// Upper bound of the wire size of the TXT records for `entries` when the
    /// service instance is `uuid`: per record, the uncompressed owner name
    /// "<uuid>._sila._tcp.local." plus the 10-byte RR header, the 1-byte
    /// string length, and key '=' value. mdns.h compresses repeated names, so
    /// the real packet is smaller; the bound only has to be safe. Public and
    /// static so the oversized-CA rejection is unit-testable without sockets.
    static std::size_t txtWireBytes(const std::string& uuid, const std::vector<TxtEntry>& entries);

private:
    // Bundles the state the mdns_socket_listen callback needs without
    // exposing `this` through a raw void* cast at every call site.
    struct ListenContext {
        MdnsPublisher* self;
        void* sendBuffer;
        std::size_t sendCapacity;
    };

    // PTR record plus its SRV/TXT/A additionals, filled by buildRecords().
    // Every mdns_string_t in ptr/additional points into this same object's strings:
    // serviceType/instanceQualified/hostnameLocal here directly, and each TXT
    // key/value in the TxtEntry objects held by txtEntries' heap-allocated vector
    // array. A copy or move would therefore hand back an object whose records
    // point at the source's storage, so both are deleted: buildRecords() fills a
    // caller-owned Records instead of returning one, which also drops the type's
    // reliance on NRVO at the return statement.
    struct Records {
        Records() = default;
        Records(const Records&) = delete;
        Records& operator=(const Records&) = delete;
        // Move is deleted explicitly rather than left implicitly suppressed by the
        // deleted copy, so the compiler names the move at the offending line.
        Records(Records&&) = delete;
        Records& operator=(Records&&) = delete;

        std::string serviceType;
        std::string instanceQualified;
        std::string hostnameLocal;
        std::vector<TxtEntry> txtEntries;
        std::vector<struct sockaddr_in> addrsV4;
        mdns_record_t ptr{};
        std::vector<mdns_record_t> additional;
    };

    std::string resolveInstanceName(const std::string& serverName);
    bool probeName(const std::string& candidateName);
    // Stops the readvertise timer and, if the listen thread is running, joins it.
    // Must be called with restartMu_ already held. Shared by publish()/setName()/
    // shutdown() so none of them can assign to a still-joinable listenThread_.
    void stopListening();
    void collectLocalAddresses();
    // Shared preamble for announceRecords()/goodbyeRecords(): locks mu_, copies the
    // current instance name and local addresses, and fills `out` from them. `out` is
    // filled in place because Records is neither copyable nor movable.
    void buildRecords(Records& out);
    // Returns false when no socket accepted the announcement (mdns.h returns -1 on
    // a send failure or a packet larger than kPacketCapacity); publish() turns that
    // into an exception so a caller never believes an unsent record is published.
    bool announceRecords();
    void goodbyeRecords();
    void listenLoop();
    static int listenCallback(int sock, const struct sockaddr* from, size_t addrlen,
                               mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                               uint16_t rclass, uint32_t ttl, const void* data, size_t size,
                               size_t name_offset, size_t name_length, size_t record_offset,
                               size_t record_length, void* user_data);

    // Holds the RFC 4122 UUID string: the mDNS Service Instance Name (Part B p76
    // MUST). publish() resolves it through resolveInstanceName() before the first
    // announce, but for a globally-unique UUID that probe always accepts it
    // unchanged; setName() never touches it (S67).
    std::string instanceName_;
    std::string uuid_;
    // Human-readable name published as the server_name TXT entry (Part B p77
    // SHOULD). Distinct from instanceName_: setName() updates this and re-announces,
    // it does not re-register the mDNS instance.
    std::string serverName_;
    std::string description_;
    // Non-empty only when the server's certificate is untrusted (Part B p75-76
    // MUST); split per-line into ca<l>= TXT entries by buildTxtEntries(). Set once
    // at construction and never modified after, so buildRecords()/listenCallback()
    // may read it without mu_.
    std::string caCertPem_;
    // Written by setPort() from run() after the port is finally known, read
    // by buildRecords() and the static listenCallback() on the listen
    // thread -- atomic so a setPort()-after-publish() cannot race that read.
    std::atomic<uint16_t> port_;
    std::chrono::seconds readvertiseInterval_;
    std::chrono::seconds recordTtl_;
    std::chrono::milliseconds probeWait_;
    std::string hostname_;
    MdnsSocketPair sockets_;
    // Mirrors "publish() opened the sockets" for lock-free reads: sockets_'s
    // fds are plain ints guarded by restartMu_, which isPublishing() must not
    // take (publish()/setName() hold it across thread joins).
    std::atomic<bool> socketsOpen_{false};
    std::atomic<bool> running_{false};
    std::thread listenThread_;
    // Sweep callback re-announces on the interval given to the constructor;
    // start()/stop() are called from publish()/shutdown() rather than here.
    sila2::PeriodicGC readvertiseTimer_{[this] { announceRecords(); }};
    mutable std::mutex mu_;
    // Separate from mu_: publish()/setName()/shutdown() must be serialized against
    // each other for their whole body, but their bodies join the listen thread and
    // call buildRecords(), both of which take mu_ — holding mu_ across them would
    // self-deadlock. None of the three calls another, so a plain mutex suffices.
    // Guards the whole body of publish()/setName()/shutdown(): the listenThread_
    // assignment in publish()/shutdown() and the stopListening() call that precedes
    // each of those two (setName() no longer touches listenThread_ -- S67).
    std::mutex restartMu_;
    std::vector<struct sockaddr_in> addrsV4_;
};

}  // namespace sila2::discovery
