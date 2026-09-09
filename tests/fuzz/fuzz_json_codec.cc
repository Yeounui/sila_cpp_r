// fuzz_json_codec.cc — libfuzzer target for dynamic JSON codec (audit §4.2)
//
// Fuzzes JsonCodec::fromJson with arbitrary data as JSON, using
// google.protobuf.Struct (accepts any JSON object) and
// google.protobuf.Value (accepts any JSON value) as target descriptors
// to maximize parse-path coverage.  Successful parses are round-tripped
// through toJson to exercise serialization as well.
//
// build:
//   conda run -n sica clang++ -std=c++20 -fsanitize=fuzzer,address \
//       -I src -I build/vcpkg_installed/x64-linux/include \
//       tests/fuzz/fuzz_json_codec.cc \
//       src/sila/client/dynamic/JsonCodec.cc \
//       -L build/vcpkg_installed/x64-linux/lib \
//       -lprotobuf -labsl_log_internal_check_op -labsl_log_internal_message \
//       -labsl_status -labsl_cord -labsl_strings -labsl_str_format_internal \
//       -labsl_string_view -labsl_raw_logging_internal -labsl_spinlock_wait \
//       -labsl_base -lutf8_validity \
//       -o build/fuzz_json_codec
//
// Run:
//   ./build/fuzz_json_codec -max_total_time=300

#include <sila/client/dynamic/JsonCodec.h>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/struct.pb.h>

// Persistent state — allocated once, reused across invocations.
static google::protobuf::DynamicMessageFactory* g_factory = nullptr;
static const google::protobuf::Descriptor* g_struct_desc = nullptr;
static const google::protobuf::Descriptor* g_value_desc  = nullptr;

static void ensureInit() {
    if (g_factory) return;
    g_factory = new google::protobuf::DynamicMessageFactory;
    g_struct_desc =
        google::protobuf::Struct::descriptor();
    g_value_desc =
        google::protobuf::Value::descriptor();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ensureInit();

    std::string_view json{reinterpret_cast<const char*>(data), size};

    // Fuzz against Struct (JSON object surface).
    try {
        auto msg = sila2::dynamic::JsonCodec::fromJson(
            json, g_struct_desc, g_factory);
        // Round-trip: exercise toJson on the parsed message. The result is
        // bound rather than discarded because toJson is [[nodiscard]] -- the
        // fuzzer wants the call's side effects (and any crash inside it), not
        // the string, so there is nothing to assert on it here.
        const std::string structRoundTrip = sila2::dynamic::JsonCodec::toJson(*msg);
        (void)structRoundTrip;
    } catch (const std::exception&) {
        // Expected for malformed input.
    }

    // Fuzz against Value (any JSON value — string, number, bool, null, nested).
    try {
        auto msg = sila2::dynamic::JsonCodec::fromJson(
            json, g_value_desc, g_factory);
        const std::string valueRoundTrip = sila2::dynamic::JsonCodec::toJson(*msg);
        (void)valueRoundTrip;
    } catch (const std::exception&) {
        // Expected for malformed input.
    }

    return 0;
}
