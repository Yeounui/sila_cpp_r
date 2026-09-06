// fuzz_mdns_parser.cc — libfuzzer target for mDNS record parsers (audit 4.1e)
//
// Fuzzes mdns_record_parse_ptr, mdns_record_parse_srv, mdns_record_parse_txt
// from the vendored mdns.h with arbitrary data/offset/length combinations.
// These are the functions called by MdnsBrowser::browseCallback on untrusted
// multicast packets.
//
// Build:
//   conda run -n sica clang++ -std=c++20 -fsanitize=fuzzer,address \
//       -I build/vcpkg_installed/x64-linux/include \
//       -DMDNS_IMPLEMENTATION \
//       tests/fuzz/fuzz_mdns_parser.cc \
//       -o build/fuzz_mdns_parser
//
// Run:
//   ./build/fuzz_mdns_parser -max_total_time=300

#define MDNS_IMPLEMENTATION
#include <mdns.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Need at least 4 bytes to derive offset and length parameters.
    if (size < 4) {
        return 0;
    }

    // First 2 bytes → record_offset, next 2 → record_length.
    // Modular arithmetic keeps them within the remaining buffer to exercise
    // both in-range and boundary conditions of the parsers.
    size_t payload = size - 4;
    size_t offset = (data[0] | (static_cast<size_t>(data[1]) << 8)) % (payload + 1);
    size_t length = (data[2] | (static_cast<size_t>(data[3]) << 8)) % (payload + 1);
    const void* buf = data + 4;

    char strbuf[256];
    memset(strbuf, 0, sizeof(strbuf));

    mdns_record_parse_ptr(buf, payload, offset, length, strbuf, sizeof(strbuf));

    memset(strbuf, 0, sizeof(strbuf));
    mdns_record_parse_srv(buf, payload, offset, length, strbuf, sizeof(strbuf));

    mdns_record_txt_t txtbuf[16];
    memset(txtbuf, 0, sizeof(txtbuf));
    mdns_record_parse_txt(buf, payload, offset, length, txtbuf, 16);

    return 0;
}
