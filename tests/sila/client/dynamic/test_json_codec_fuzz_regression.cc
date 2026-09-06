// Regression companion to tests/fuzz/fuzz_json_codec.cc (audit 4.1l):
// JsonCodec::fromJson() must either return a parsed Message or throw
// std::invalid_argument for every input string -- never crash. This pins
// hand-picked edge cases (empty/single-byte/truncated/oversized/embedded-NUL)
// to a fixed outcome; the exploratory search over the same input space lives
// in the libfuzzer target.
//
// Both target descriptors from the fuzz harness are exercised here:
// google.protobuf.Struct (JSON object surface) and google.protobuf.Value
// (any JSON value -- string/number/bool/null/array/nested object).
#include <sila/client/dynamic/JsonCodec.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/struct.pb.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {
using sila2::dynamic::JsonCodec;

const google::protobuf::Descriptor* structDescriptor() {
    return google::protobuf::Struct::descriptor();
}

const google::protobuf::Descriptor* valueDescriptor() {
    return google::protobuf::Value::descriptor();
}

// Mirrors fuzz_json_codec.cc's LLVMFuzzerTestOneInput body for one target
// descriptor: parse then round-trip through toJson. Returns the round-tripped
// JSON on success; throws std::invalid_argument on failure, matching
// JsonCodec's own contract (never anything else escapes).
std::string parseAndRoundTrip(std::string_view json, const google::protobuf::Descriptor* desc,
                               google::protobuf::MessageFactory* factory) {
    auto msg = JsonCodec::fromJson(json, desc, factory);
    return JsonCodec::toJson(*msg);
}

}  // namespace

// --- True (positive) paths --------------------------------------------------

TEST(JsonCodecFuzzRegression, MinimalEmptyObjectAgainstStructRoundTrips) {
    google::protobuf::DynamicMessageFactory factory;

    const std::string roundTripped = parseAndRoundTrip("{}", structDescriptor(), &factory);

    EXPECT_NE(roundTripped.find('{'), std::string::npos);
}

TEST(JsonCodecFuzzRegression, NestedObjectWithMixedValueTypesAgainstStructRoundTrips) {
    google::protobuf::DynamicMessageFactory factory;
    // Exercises the Struct field's every underlying Value kind: string,
    // number, bool, null, nested list, nested object.
    constexpr char kJson[] =
        R"({"s":"hello","n":42.5,"b":true,"nil":null,"list":[1,2,3],"obj":{"inner":"x"}})";

    const std::string roundTripped = parseAndRoundTrip(kJson, structDescriptor(), &factory);

    EXPECT_NE(roundTripped.find("hello"), std::string::npos);
    EXPECT_NE(roundTripped.find("inner"), std::string::npos);
}

TEST(JsonCodecFuzzRegression, BareScalarAgainstValueRoundTrips) {
    // google.protobuf.Value is the only descriptor of the two that accepts a
    // bare (non-object) top-level JSON token -- a distinct parse path from
    // the Struct tests above, which require an object.
    google::protobuf::DynamicMessageFactory factory;

    const std::string roundTripped = parseAndRoundTrip("\"just-a-string\"", valueDescriptor(), &factory);

    EXPECT_NE(roundTripped.find("just-a-string"), std::string::npos);
}

TEST(JsonCodecFuzzRegression, MaximalLengthStringFieldAgainstStructRoundTrips) {
    // A single field whose value is a long string, to exercise allocation
    // paths a short fixed corpus otherwise never touches.
    google::protobuf::DynamicMessageFactory factory;
    const std::string longValue(64 * 1024, 'a');
    const std::string json = R"({"big":")" + longValue + R"("})";

    const std::string roundTripped = parseAndRoundTrip(json, structDescriptor(), &factory);

    EXPECT_NE(roundTripped.find(longValue), std::string::npos);
}

// --- False (negative) paths — all CAUGHT: fromJson throws std::invalid_argument ---

TEST(JsonCodecFuzzRegression, EmptyInputThrows) {
    google::protobuf::DynamicMessageFactory factory;

    EXPECT_THROW(JsonCodec::fromJson("", structDescriptor(), &factory), std::invalid_argument);
    EXPECT_THROW(JsonCodec::fromJson("", valueDescriptor(), &factory), std::invalid_argument);
}

TEST(JsonCodecFuzzRegression, SingleByteInputsThrow) {
    google::protobuf::DynamicMessageFactory factory;
    const std::string nulByte{'\x00'};
    const std::string ffByte{'\xFF'};

    EXPECT_THROW(JsonCodec::fromJson(nulByte, structDescriptor(), &factory), std::invalid_argument);
    EXPECT_THROW(JsonCodec::fromJson(ffByte, structDescriptor(), &factory), std::invalid_argument);
    // 0xFF is not a valid JSON token under either descriptor, unlike NUL vs.
    // Struct above where the failure is "not an object" rather than
    // "not a token" -- this pins the plain syntax-error branch too.
    EXPECT_THROW(JsonCodec::fromJson(ffByte, valueDescriptor(), &factory), std::invalid_argument);
}

TEST(JsonCodecFuzzRegression, TruncatedJsonObjectThrows) {
    // A valid, complete document with its closing brace and quote cut off.
    google::protobuf::DynamicMessageFactory factory;
    constexpr char kTruncated[] = R"({"key":"val)";

    EXPECT_THROW(JsonCodec::fromJson(kTruncated, structDescriptor(), &factory), std::invalid_argument);
}

TEST(JsonCodecFuzzRegression, RandomGarbageBytesThrow) {
    google::protobuf::DynamicMessageFactory factory;
    const std::string garbage = "\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE\x7F\x1B";

    EXPECT_THROW(JsonCodec::fromJson(garbage, structDescriptor(), &factory), std::invalid_argument);
    EXPECT_THROW(JsonCodec::fromJson(garbage, valueDescriptor(), &factory), std::invalid_argument);
}

// UNCAUGHT: RFC 8259 requires control characters (including a raw NUL) inside
// a JSON string literal to be escaped (e.g. a backslash-u0000 sequence); an
// unescaped one is not valid JSON. google::protobuf::util::JsonStringToMessage
// does not enforce this -- verified by running this exact input through
// fromJson() and observing it return normally instead of throwing. This test
// pins that current (lenient) behavior rather than asserting a rejection the
// implementation does not perform; it exists so a future protobuf upgrade
// that starts rejecting this input is a visible test change, not a silent one.
TEST(JsonCodecFuzzRegression, EmbeddedNullByteInsideStringLiteralParsesWithoutCrashing) {
    google::protobuf::DynamicMessageFactory factory;
    const std::string withRawNul = std::string(R"({"key":"a)") + '\0' + R"(b"})";

    const std::string roundTripped = parseAndRoundTrip(withRawNul, structDescriptor(), &factory);

    EXPECT_NE(roundTripped.find("key"), std::string::npos);
}
