// Fuzz regression companion for the mDNS record parsers (audit 4.1e).
//
// mdns_record_parse_ptr / mdns_record_parse_srv / mdns_record_parse_txt come
// from the vendored, header-only mdns.h C library and are called directly by
// MdnsBrowser::browseCallback (MdnsBrowser.cc:110-156) on raw, untrusted
// multicast packets. They are plain C functions: they never throw, they
// silently reject bad input by returning an empty/zeroed result. This file
// pins that "return, don't crash" contract for hand-picked inputs; the
// exploratory search over the same input space lives in the libfuzzer target
// at tests/fuzz/fuzz_mdns_parser.cc.
#include <mdns.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

// Appends one DNS label (length-prefixed, uncompressed) to a wire buffer.
void appendLabel(Bytes& buffer, const std::string& label) {
    buffer.push_back(static_cast<uint8_t>(label.size()));
    for (char c : label) buffer.push_back(static_cast<uint8_t>(c));
}

std::string toString(const mdns_string_t& s) {
    return std::string{s.str, s.length};
}

}  // namespace

// ---------------------------------------------------------------------------
// True: valid-ish wire records that must parse without crashing.
// ---------------------------------------------------------------------------

TEST(MdnsParserFuzzRegression, ParsePtrValidUncompressedName) {
    Bytes buf;
    appendLabel(buf, "MyInstance");
    appendLabel(buf, "_sila");
    appendLabel(buf, "_tcp");
    appendLabel(buf, "local");
    buf.push_back(0);  // root label

    char strbuf[256] = {0};
    mdns_string_t name =
        mdns_record_parse_ptr(buf.data(), buf.size(), 0, buf.size(), strbuf, sizeof(strbuf));

    EXPECT_EQ(toString(name), "MyInstance._sila._tcp.local.");
}

TEST(MdnsParserFuzzRegression, ParsePtrValidCompressedNamePointer) {
    // Same name as above, but the record refers to a target name stored
    // earlier in the packet via a DNS compression pointer (top two bits set).
    Bytes buf;
    size_t targetOffset = buf.size();
    appendLabel(buf, "_sila");
    appendLabel(buf, "_tcp");
    appendLabel(buf, "local");
    buf.push_back(0);

    size_t recordOffset = buf.size();
    appendLabel(buf, "MyInstance");
    buf.push_back(static_cast<uint8_t>(0xC0 | (targetOffset >> 8)));
    buf.push_back(static_cast<uint8_t>(targetOffset & 0xFF));
    size_t recordLength = buf.size() - recordOffset;

    char strbuf[256] = {0};
    mdns_string_t name = mdns_record_parse_ptr(buf.data(), buf.size(), recordOffset, recordLength,
                                                strbuf, sizeof(strbuf));

    EXPECT_EQ(toString(name), "MyInstance._sila._tcp.local.");
}

TEST(MdnsParserFuzzRegression, ParseSrvValidRecord) {
    Bytes buf;
    buf.push_back(0);
    buf.push_back(0);  // priority = 0
    buf.push_back(0);
    buf.push_back(0);  // weight = 0
    buf.push_back(0x1F);
    buf.push_back(0x90);  // port = 8080
    appendLabel(buf, "host");
    appendLabel(buf, "local");
    buf.push_back(0);

    char strbuf[256] = {0};
    mdns_record_srv_t srv =
        mdns_record_parse_srv(buf.data(), buf.size(), 0, buf.size(), strbuf, sizeof(strbuf));

    EXPECT_EQ(srv.port, 8080);
    EXPECT_EQ(toString(srv.name), "host.local.");
}

TEST(MdnsParserFuzzRegression, ParseTxtKeyValuePair) {
    Bytes buf;
    std::string entry = "uuid=1234";
    buf.push_back(static_cast<uint8_t>(entry.size()));
    for (char c : entry) buf.push_back(static_cast<uint8_t>(c));

    mdns_record_txt_t records[4];
    std::memset(records, 0, sizeof(records));
    size_t parsed = mdns_record_parse_txt(buf.data(), buf.size(), 0, buf.size(), records, 4);

    ASSERT_EQ(parsed, 1u);
    EXPECT_EQ(toString(records[0].key), "uuid");
    EXPECT_EQ(toString(records[0].value), "1234");
}

TEST(MdnsParserFuzzRegression, ParseTxtKeyOnlyNoValue) {
    // A TXT string with no '=' separator is valid: key present, no value.
    Bytes buf;
    std::string entry = "solo";
    buf.push_back(static_cast<uint8_t>(entry.size()));
    for (char c : entry) buf.push_back(static_cast<uint8_t>(c));

    mdns_record_txt_t records[4];
    std::memset(records, 0, sizeof(records));
    size_t parsed = mdns_record_parse_txt(buf.data(), buf.size(), 0, buf.size(), records, 4);

    ASSERT_EQ(parsed, 1u);
    EXPECT_EQ(toString(records[0].key), "solo");
    EXPECT_EQ(records[0].value.length, 0u);
}

// ---------------------------------------------------------------------------
// False: malformed/adversarial inputs. All are CAUGHT — the parsers reject
// them internally and return an empty/zeroed result without touching memory
// outside `buffer`. Verified with buffer contents built by this file plus
// manual AddressSanitizer probes (see report).
// ---------------------------------------------------------------------------

TEST(MdnsParserFuzzRegression, ParsePtrZeroLengthBuffer) {
    // Covers both "zero-length buffer" and "null buffer pointer with size=0":
    // size=0 means the guard rejects before `buffer` is ever dereferenced.
    char strbuf[16] = {0};
    mdns_string_t name = mdns_record_parse_ptr(nullptr, 0, 0, 0, strbuf, sizeof(strbuf));
    EXPECT_EQ(name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParsePtrLengthBelowMinimum) {
    // PTR records need length >= 2; length=1 must be rejected outright.
    Bytes buf(10, 0x41);
    char strbuf[16] = {0};
    mdns_string_t name = mdns_record_parse_ptr(buf.data(), buf.size(), 0, 1, strbuf, sizeof(strbuf));
    EXPECT_EQ(name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParsePtrOffsetExceedsSize) {
    Bytes buf(10, 0x41);
    char strbuf[16] = {0};
    mdns_string_t name =
        mdns_record_parse_ptr(buf.data(), buf.size(), 100, 5, strbuf, sizeof(strbuf));
    EXPECT_EQ(name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParsePtrAllFFBytes) {
    // 0xFF's top two bits mark a DNS compression pointer at every position;
    // this maximizes pointer-chasing inside mdns_get_next_substring.
    Bytes buf(32, 0xFF);
    char strbuf[16] = {0};
    mdns_string_t name =
        mdns_record_parse_ptr(buf.data(), buf.size(), 0, buf.size(), strbuf, sizeof(strbuf));
    EXPECT_EQ(name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParsePtrOffsetNearSizeMax) {
    // offset + length overflows size_t and wraps past the `size >= offset +
    // length` guard, but mdns_get_next_substring's own `offset >= size` check
    // catches the out-of-range offset before any dereference.
    Bytes buf(16, 0x41);
    char strbuf[16] = {0};
    size_t offset = SIZE_MAX - 1;
    mdns_string_t name =
        mdns_record_parse_ptr(buf.data(), buf.size(), offset, 2, strbuf, sizeof(strbuf));
    EXPECT_EQ(name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParseSrvZeroLengthBuffer) {
    char strbuf[16] = {0};
    mdns_record_srv_t srv = mdns_record_parse_srv(nullptr, 0, 0, 0, strbuf, sizeof(strbuf));
    EXPECT_EQ(srv.port, 0);
    EXPECT_EQ(srv.name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParseSrvLengthBelowMinimum) {
    // SRV records need length >= 8 (3 uint16 fields + minimum name); 7 must
    // be rejected outright.
    Bytes buf(20, 0x41);
    char strbuf[16] = {0};
    mdns_record_srv_t srv =
        mdns_record_parse_srv(buf.data(), buf.size(), 0, 7, strbuf, sizeof(strbuf));
    EXPECT_EQ(srv.port, 0);
    EXPECT_EQ(srv.name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParseSrvOffsetExceedsSize) {
    Bytes buf(10, 0x41);
    char strbuf[16] = {0};
    mdns_record_srv_t srv =
        mdns_record_parse_srv(buf.data(), buf.size(), 100, 8, strbuf, sizeof(strbuf));
    EXPECT_EQ(srv.port, 0);
    EXPECT_EQ(srv.name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParseSrvLengthExceedsSize) {
    Bytes buf(8, 0x41);
    char strbuf[16] = {0};
    mdns_record_srv_t srv =
        mdns_record_parse_srv(buf.data(), buf.size(), 0, 20, strbuf, sizeof(strbuf));
    EXPECT_EQ(srv.port, 0);
    EXPECT_EQ(srv.name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParseSrvAllFFBytes) {
    Bytes buf(32, 0xFF);
    char strbuf[16] = {0};
    mdns_record_srv_t srv =
        mdns_record_parse_srv(buf.data(), buf.size(), 0, buf.size(), strbuf, sizeof(strbuf));
    EXPECT_EQ(srv.name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParseSrvOffsetNearSizeMax) {
    // offset near SIZE_MAX previously overflowed the `size >= offset + length`
    // guard (wrapping to a small value) and reached the memcpy/ntohs block —
    // heap-buffer-overflow under ASan. Fixed by overflow-safe guard:
    // `offset <= size && length <= size - offset`.
    Bytes buf(16, 0x41);
    char strbuf[16] = {0};
    size_t offset = SIZE_MAX - 4;
    mdns_record_srv_t srv =
        mdns_record_parse_srv(buf.data(), buf.size(), offset, 8, strbuf, sizeof(strbuf));
    EXPECT_EQ(srv.port, 0);
    EXPECT_EQ(srv.name.length, 0u);
}

TEST(MdnsParserFuzzRegression, ParseTxtZeroLengthBuffer) {
    mdns_record_txt_t records[4];
    std::memset(records, 0, sizeof(records));
    size_t parsed = mdns_record_parse_txt(nullptr, 0, 0, 0, records, 4);
    EXPECT_EQ(parsed, 0u);
}

TEST(MdnsParserFuzzRegression, ParseTxtOffsetExceedsSize) {
    Bytes buf(10, 0x41);
    mdns_record_txt_t records[4];
    std::memset(records, 0, sizeof(records));
    size_t parsed = mdns_record_parse_txt(buf.data(), buf.size(), 100, 5, records, 4);
    EXPECT_EQ(parsed, 0u);
}

TEST(MdnsParserFuzzRegression, ParseTxtLengthExceedsSize) {
    // Sub-length byte claims 200 bytes follow, but the buffer holds 3.
    Bytes buf{200, 'a', 'b'};
    mdns_record_txt_t records[4];
    std::memset(records, 0, sizeof(records));
    size_t parsed = mdns_record_parse_txt(buf.data(), buf.size(), 0, buf.size(), records, 4);
    EXPECT_EQ(parsed, 0u);
}

TEST(MdnsParserFuzzRegression, ParseTxtAllFFBytes) {
    // Every sub-length byte is 0xFF (255), which is >= any remaining span,
    // so the parser must break out on the very first iteration.
    Bytes buf(32, 0xFF);
    mdns_record_txt_t records[4];
    std::memset(records, 0, sizeof(records));
    size_t parsed = mdns_record_parse_txt(buf.data(), buf.size(), 0, buf.size(), records, 4);
    EXPECT_EQ(parsed, 0u);
}

TEST(MdnsParserFuzzRegression, ParseTxtOffsetNearSizeMax) {
    // offset + length overflows size_t; the wrapped `end` stays below the
    // (huge) offset, so the `offset < end` loop guard rejects it up front.
    Bytes buf(16, 0x41);
    mdns_record_txt_t records[4];
    std::memset(records, 0, sizeof(records));
    size_t offset = SIZE_MAX - 1;
    size_t parsed = mdns_record_parse_txt(buf.data(), buf.size(), offset, 4, records, 4);
    EXPECT_EQ(parsed, 0u);
}
