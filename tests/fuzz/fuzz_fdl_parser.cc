// fuzz_fdl_parser.cc — libfuzzer target for FDL XML parser (audit 3.1l/3.1j)
//
// build:
//   cmake --preset fuzz
//   cmake --build build/fuzz --target fuzz_fdl_parser
//
// Run:
//   ./build/fuzz_fdl_parser -max_total_time=300

#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        sila2::dynamic::parseFdl(
            {reinterpret_cast<const char*>(data), size});
    } catch (const std::exception&) {
        // parseFdl throws std::invalid_argument on malformed input — expected.
    }
    return 0;
}
