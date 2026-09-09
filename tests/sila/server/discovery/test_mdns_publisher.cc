// Covers MdnsPublisher::truncateUtf8 and MdnsPublisher::buildTxtEntries: the
// pieces of the mDNS publisher testable without live network sockets
// (SiLA 2 §5.4 Discovery, architecture.md §6). buildTxtEntries is the shared
// TXT-record builder behind both the announce path and the query-response
// path (audit S54/S55/S56), so exercising it here also runs on WSL, where
// test_mdns_browser.cc's loopback round-trip is env-skipped.
#include <sila/server/discovery/MdnsPublisher.h>

#include <gtest/gtest.h>

#include <mdns.h>

#include <netinet/in.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using sila2::discovery::MdnsPublisher;

namespace {

// Finds the TxtEntry with the given key, or nullopt if none matches -- lets
// each test assert on one entry's value without depending on push_back order.
std::optional<MdnsPublisher::TxtEntry> findTxtEntry(
    const std::vector<MdnsPublisher::TxtEntry>& entries, const std::string& key) {
    auto it = std::find_if(entries.begin(), entries.end(),
                            [&](const MdnsPublisher::TxtEntry& e) { return e.key == key; });
    return it == entries.end() ? std::nullopt : std::make_optional(*it);
}

}  // namespace

TEST(MdnsPublisherTruncateUtf8, ReturnsAsciiStringUnchangedWhenUnderLimit)
{
    EXPECT_EQ(MdnsPublisher::truncateUtf8("hello", 63), "hello");
}

TEST(MdnsPublisherTruncateUtf8, TruncatesAsciiStringToExactlyMaxBytes)
{
    const std::string s(100, 'a');
    const auto result = MdnsPublisher::truncateUtf8(s, 63);
    EXPECT_EQ(result.size(), 63u);
    EXPECT_EQ(result, std::string(63, 'a'));
}

TEST(MdnsPublisherTruncateUtf8, WalksBackBeforeSplittingTwoByteCodePoint)
{
    // 30 'a's, then U+00E9 (é = 0xC3 0xA9), then 30 'b's. maxBytes=31 lands
    // the cut on the second byte of é; the result must drop the whole
    // code point rather than keep a lone 0xC3.
    const std::string s = std::string(30, 'a') + "\xC3\xA9" + std::string(30, 'b');
    EXPECT_EQ(MdnsPublisher::truncateUtf8(s, 31), std::string(30, 'a'));
}

TEST(MdnsPublisherTruncateUtf8, ReturnsEmptyStringUnchanged)
{
    EXPECT_EQ(MdnsPublisher::truncateUtf8("", 63), "");
}

TEST(MdnsPublisherTruncateUtf8, ReturnsEmptyWhenMaxBytesIsZero)
{
    EXPECT_EQ(MdnsPublisher::truncateUtf8("hello", 0), "");
}

TEST(MdnsPublisherTruncateUtf8, WalksBackAcrossThreeByteCodePointsToLandOnBoundary)
{
    // 10 copies of U+3042 (あ = 0xE3 0x81 0x82), 30 bytes total. maxBytes=29
    // is not a multiple of 3, so the cut falls inside the 10th character;
    // both of its continuation bytes must be walked back over.
    const std::string chr = "\xE3\x81\x82";
    std::string s;
    for (int i = 0; i < 10; ++i) s += chr;
    std::string expected;
    for (int i = 0; i < 9; ++i) expected += chr;

    EXPECT_EQ(MdnsPublisher::truncateUtf8(s, 29), expected);
}

TEST(MdnsPublisherTruncateUtf8, WalksBackPastAllContinuationBytesOfFourByteEmoji)
{
    // "ab" + U+1F52C (🔬 = 0xF0 0x9F 0x94 0xAC) + "cd". maxBytes=5 lands
    // inside the emoji's third continuation byte; all three continuation
    // bytes must be walked back over to land before the leading byte.
    const std::string s = "ab\xF0\x9F\x94\xAC" "cd";
    EXPECT_EQ(MdnsPublisher::truncateUtf8(s, 5), "ab");
}

// Below: the boundary the seven cases above don't cover -- the FDL's own
// ServerName MaximalLength (255 characters) crossed against the 63-byte mDNS
// label. These are pins over existing, deliberate behaviour (audit S25):
// truncateUtf8 and resolveInstanceName's suffix arithmetic are not changing,
// only gaining doc comments, so none of these three is expected to fail
// before or after that change.

TEST(MdnsPublisherTruncateUtf8, FdlMaximumAsciiServerNameTruncatesToExactlySixtyThreeBytes)
{
    // 255 is the FDL's ServerName MaximalLength in characters
    // (SiLAService-v1_0.sila.xml:106-108); ASCII makes bytes == characters.
    const std::string name(255, 'a');
    EXPECT_EQ(MdnsPublisher::truncateUtf8(name, 63).size(), 63u);
}

TEST(MdnsPublisherTruncateUtf8, FdlMaximumMultiByteServerNameTruncatesOnACodePointBoundary)
{
    // 255 copies of U+1F52C (🔬, 4 bytes) = 1020 bytes -- the byte ceiling
    // S22's character-counting switch (SC6, 5787722) opened up, since the FDL
    // limit is counted in code points, not bytes.
    const std::string chr = "\xF0\x9F\x94\xAC";
    std::string name;
    for (int i = 0; i < 255; ++i) name += chr;
    ASSERT_EQ(name.size(), 1020u);

    const auto result = MdnsPublisher::truncateUtf8(name, 63);
    // 63 isn't a multiple of 4, so the walk-back drops the partial 16th
    // code point: 15 whole emoji = 60 bytes, not 63.
    EXPECT_EQ(result.size(), 60u);
    EXPECT_EQ(result.size() % 4, 0u);
}

TEST(MdnsPublisherTruncateUtf8, SuffixedCandidateStillFitsTheSixtyThreeByteLabel)
{
    // Pins resolveInstanceName's suffix arithmetic (MdnsPublisher.cc:222) without
    // needing a socket: for every conflict-retry suffix " (2)".." (10)", the
    // truncated name plus the suffix must still total exactly 63 bytes. The
    // arithmetic itself is unchanged and still exercised by resolveInstanceName()
    // in publish() -- but since S67 makes the mDNS instance the SiLA Server UUID
    // (36 ASCII bytes, well under 63), this suffix path is unreachable in practice
    // for a real RFC 4122 UUID; the test remains a pin on the pure function.
    const std::string name(255, 'a');
    for (int attempt = 2; attempt <= 10; ++attempt) {
        const std::string suffix = " (" + std::to_string(attempt) + ")";
        const auto truncated = MdnsPublisher::truncateUtf8(name, 63 - suffix.size());
        EXPECT_EQ(truncated.size() + suffix.size(), 63u) << "attempt=" << attempt;
    }
}

// Below: MdnsPublisher::buildTxtEntries, the shared TXT-record builder behind
// both the announce path (buildRecords) and the query-response path
// (listenCallback) (audit S54/S55/S56/S67). A pure static function, so these
// run without a socket and are not WSL-skipped.

TEST(MdnsPublisherBuildTxtEntries, EmitsServerNameAndDescription)
{
    const auto entries = MdnsPublisher::buildTxtEntries("u", "MyServer", "a lab robot", "");

    const auto serverName = findTxtEntry(entries, "server_name");
    const auto description = findTxtEntry(entries, "description");
    ASSERT_TRUE(serverName.has_value());
    ASSERT_TRUE(description.has_value());
    EXPECT_EQ(serverName->value, "MyServer");
    EXPECT_EQ(description->value, "a lab robot");
}

TEST(MdnsPublisherBuildTxtEntries, TruncatesOversizeServerNameToTheTxtLimit)
{
    // Part B p77 MUST: "Key/value pairs exceeding the 255 bytes MUST be
    // truncated." Budget for key "server_name" is 255 - 11 - 1 ('=') = 243.
    const std::string oversizeName(300, 'a');
    const auto entries = MdnsPublisher::buildTxtEntries("u", oversizeName, "", "");

    const auto serverName = findTxtEntry(entries, "server_name");
    ASSERT_TRUE(serverName.has_value());
    EXPECT_LE(serverName->value.size(), 255u - std::string("server_name").size() - 1);
    EXPECT_EQ(serverName->value, std::string(serverName->value.size(), 'a'))
        << "truncation must not introduce or drop any byte other than the cut";
}

TEST(MdnsPublisherBuildTxtEntries, TruncatesMultiByteServerNameOnACodePointBoundary)
{
    // REJECTION: a 243-byte budget is not a multiple of 4, so a naive fixed-size
    // cut would split the emoji's last code point. buildTxtEntries must reuse
    // truncateUtf8's UTF-8-aware walk-back (S54 shares it via truncatedTxtValue).
    const std::string emoji = "\xF0\x9F\x94\xAC";  // U+1F52C, 4 bytes
    std::string oversizeName;
    for (int i = 0; i < 100; ++i) oversizeName += emoji;  // 400 bytes, over budget

    const auto entries = MdnsPublisher::buildTxtEntries("u", oversizeName, "", "");
    const auto serverName = findTxtEntry(entries, "server_name");
    ASSERT_TRUE(serverName.has_value());
    EXPECT_EQ(serverName->value.size() % 4, 0u)
        << "a split code point would leave a size not divisible by 4";
}

TEST(MdnsPublisherBuildTxtEntries, UsesSilaVersionOnePointOne)
{
    // Part A: "the SiLA 2 Version SHALL be '1.1'."
    const auto entries = MdnsPublisher::buildTxtEntries("u", "n", "", "");
    const auto version = findTxtEntry(entries, "version");
    ASSERT_TRUE(version.has_value());
    EXPECT_EQ(version->value, "1.1");
}

TEST(MdnsPublisherBuildTxtEntries, NeverEmitsVersionTwo)
{
    // REJECTION: pins the corrected literal so a regression back to the old
    // hardcoded "2" (announce and query-response used to duplicate it) fails.
    const auto entries = MdnsPublisher::buildTxtEntries("u", "n", "", "");
    const auto version = findTxtEntry(entries, "version");
    ASSERT_TRUE(version.has_value());
    EXPECT_NE(version->value, "2");
}

TEST(MdnsPublisherBuildTxtEntries, UuidMatchesInstanceIdentity)
{
    // The TXT uuid entry and the mDNS instance name (S67) both derive from the
    // same config_->uuid() at the call site; pinning the TXT value here is the
    // socket-free half of that identity (the instance-name half is covered by
    // test_mdns_browser.cc's UUID-instance assertion).
    const std::string uuid = "f81d4fae-7dec-11d0-a765-00a0c91e6bf6";
    const auto entries = MdnsPublisher::buildTxtEntries(uuid, "SomeServer", "", "");
    const auto uuidEntry = findTxtEntry(entries, "uuid");
    ASSERT_TRUE(uuidEntry.has_value());
    EXPECT_EQ(uuidEntry->value, uuid);
}

TEST(MdnsPublisherBuildTxtEntries, SplitsUntrustedCertPemIntoZeroIndexedCaLines)
{
    // Part B p75-76 MUST: an untrusted certificate's CA PEM is split into
    // ca<l>= entries, one per line, l being the 0-indexed line number -- NOT
    // fixed-size chunking. The trailing newline must not produce a ca3.
    const std::string caCertPem = "line0\nline1\nline2\n";
    const auto entries = MdnsPublisher::buildTxtEntries("u", "n", "", caCertPem);

    const auto ca0 = findTxtEntry(entries, "ca0");
    const auto ca1 = findTxtEntry(entries, "ca1");
    const auto ca2 = findTxtEntry(entries, "ca2");
    ASSERT_TRUE(ca0.has_value());
    ASSERT_TRUE(ca1.has_value());
    ASSERT_TRUE(ca2.has_value());
    EXPECT_EQ(ca0->value, "line0");
    EXPECT_EQ(ca1->value, "line1");
    EXPECT_EQ(ca2->value, "line2");
    EXPECT_FALSE(findTxtEntry(entries, "ca3").has_value());
}

TEST(MdnsPublisherBuildTxtEntries, EmitsNoCaLinesForTrustedCertificate)
{
    // REJECTION: an empty caCertPem (trusted certificate) must publish no ca<l>=
    // entries at all.
    const auto entries = MdnsPublisher::buildTxtEntries("u", "n", "", "");
    const bool hasCaEntry = std::any_of(
        entries.begin(), entries.end(),
        [](const MdnsPublisher::TxtEntry& e) { return e.key.rfind("ca", 0) == 0; });
    EXPECT_FALSE(hasCaEntry);
}

TEST(MdnsPublisherBuildTxtEntries, CaLineIndexIsConsecutiveAcrossBlankLines)
{
    // Edge case: a blank interior line (or the trailing-newline artifact) is
    // skipped rather than published as an empty ca<l>=, so l stays the
    // consecutive 0-indexed count of the certificate's non-empty lines.
    const std::string caCertPem = "a\n\nb\n";
    const auto entries = MdnsPublisher::buildTxtEntries("u", "n", "", caCertPem);

    const auto ca0 = findTxtEntry(entries, "ca0");
    const auto ca1 = findTxtEntry(entries, "ca1");
    ASSERT_TRUE(ca0.has_value());
    ASSERT_TRUE(ca1.has_value());
    EXPECT_EQ(ca0->value, "a");
    EXPECT_EQ(ca1->value, "b");
}

// Below: MdnsPublisher::appendServiceRecords, the shared record-set builder
// behind both the announce path (buildRecords) and the query-response path
// (listenCallback). A public static function, so this runs without a socket
// (audit S76 -- Part B p75 B095 MUST: the advertised address must represent
// the serving socket, but the gRPC server binds IPv4-only).

TEST(MdnsPublisherAppendServiceRecords, AdvertisesAnARecordForEachIpv4Address)
{
    mdns_record_t ptr{};
    std::vector<mdns_record_t> additional;
    // Two zero-valued sockaddr_in entries: appendServiceRecords only reads
    // the vector's size to decide how many A records to emit, not the
    // address bytes, so default-constructed entries are enough here.
    MdnsPublisher::appendServiceRecords(ptr, additional, "_sila._tcp.local.",
                                        "uuid._sila._tcp.local.", "host.local.", 50051, {},
                                        std::vector<struct sockaddr_in>(2));

    const auto aRecordCount = std::count_if(
        additional.begin(), additional.end(),
        [](const mdns_record_t& r) { return r.type == MDNS_RECORDTYPE_A; });
    EXPECT_EQ(aRecordCount, 2);
}

TEST(MdnsPublisherAppendServiceRecords, NeverAdvertisesAnAaaaRecord)
{
    // REJECTION: Part B p75 (B095) requires the advertised address to represent
    // the socket the HTTP server actually serves on; the gRPC server binds
    // IPv4-only (SilaServerBase.cc), so no AAAA record may ever be emitted here
    // regardless of how many addresses are passed in.
    mdns_record_t ptr{};
    std::vector<mdns_record_t> additional;
    MdnsPublisher::appendServiceRecords(ptr, additional, "_sila._tcp.local.",
                                        "uuid._sila._tcp.local.", "host.local.", 50051, {},
                                        std::vector<struct sockaddr_in>(1));

    EXPECT_FALSE(std::any_of(additional.begin(), additional.end(),
                             [](const mdns_record_t& r) { return r.type == MDNS_RECORDTYPE_AAAA; }));
}

namespace {

// A PEM body of `lines` 64-character base64 lines between the usual markers.
std::string fakePem(std::size_t lines) {
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (std::size_t i = 0; i < lines; ++i) {
        pem += std::string(64, 'A') + "\n";
    }
    return pem + "-----END CERTIFICATE-----\n";
}

MdnsPublisher makePublisher(const std::string& caCertPem) {
    return MdnsPublisher{"3fa85f64-5717-4562-b3fc-2c963f66afa6", "name", "desc", caCertPem, 50052,
                         std::chrono::seconds{60}, std::chrono::seconds{120},
                         std::chrono::milliseconds{50}};
}

}  // namespace

// POSITIVE: a real-world untrusted certificate (a 2 KiB PEM is ~35 lines) must still
// fit one mDNS packet after Part B p76's per-line ca<l>= expansion.
TEST(MdnsPublisherPacketBudget, TypicalCertificateFitsOnePacket)
{
    const std::string pem = fakePem(35);
    const auto entries = MdnsPublisher::buildTxtEntries("3fa85f64-5717-4562-b3fc-2c963f66afa6", "name", "desc", pem);
    EXPECT_LE(MdnsPublisher::txtWireBytes("3fa85f64-5717-4562-b3fc-2c963f66afa6", entries) +
                  MdnsPublisher::kFixedRecordsBudget,
              MdnsPublisher::kPacketCapacity);
    EXPECT_NO_THROW(makePublisher(pem));
}

// REJECTION: a CA whose ca<l>= lines cannot fit kPacketCapacity is refused at
// construction instead of failing silently on every announce (mdns.h returns -1
// past the buffer; Codex review of fd62b52).
TEST(MdnsPublisherPacketBudget, OversizedCertificateIsRejectedAtConstruction)
{
    EXPECT_THROW(makePublisher(fakePem(120)), std::invalid_argument);
}

// The bound counts every TXT record's owner name, RR header, and key=value, so
// adding one 64-byte line must grow it by more than the 68 payload bytes alone.
TEST(MdnsPublisherPacketBudget, WireBytesGrowPerCaLineWithRecordOverhead)
{
    const std::string uuid = "3fa85f64-5717-4562-b3fc-2c963f66afa6";
    const auto one = MdnsPublisher::buildTxtEntries(uuid, "", "", "A\n");
    const auto two = MdnsPublisher::buildTxtEntries(uuid, "", "", "A\nA\n");
    const std::size_t delta = MdnsPublisher::txtWireBytes(uuid, two) - MdnsPublisher::txtWireBytes(uuid, one);
    // "ca1" + '=' + "A" = 5 payload bytes; the rest is the owner name and RR header.
    EXPECT_GT(delta, 5u);
    EXPECT_EQ(delta, uuid.size() + sizeof("._sila._tcp.local.") + 11 + 5);
}

// Regression for audit 4.2b: publish() used to assign a new std::thread over
// listenThread_ without checking whether the previous one was still joinable,
// so a second call would call std::terminate. This doesn't depend on multicast
// loopback actually working (sockets_.open()/announceRecords() tolerate a failed
// bind), so unlike test_mdns_browser.cc it isn't skipped on WSL2 — it only
// exercises the thread-restart discipline, not mDNS delivery.
TEST(MdnsPublisher, PublishCalledTwiceDoesNotTerminate)
{
    MdnsPublisher publisher{"audit-4-2b-uuid", "audit-4-2b-test", "", "", 12345,
                            std::chrono::seconds{60}, std::chrono::seconds{120},
                            std::chrono::milliseconds{50}};
    publisher.publish();
    publisher.publish();
    publisher.shutdown();
}
