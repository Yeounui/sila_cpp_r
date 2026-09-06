// Integration tests for JsonCodec::toJson()/fromJson() (architecture.md
// §4.2): the JSON <-> protobuf Message bridge used to marshal dynamically
// typed SiLA values. Exercises both the fixed SiLAFramework::String message
// and, for the dynamic-message case, the full
// FdlRuntimeParser -> DescriptorBuilder -> DescriptorPool pipeline that
// produces the Descriptor JsonCodec parses into.
//
// NOTE: toJson()'s "invalid data" error branch is not exercised below.
// google::protobuf::util::MessageToJsonString in the linked protobuf version
// sanitizes invalid UTF-8 in string fields (replacing bad bytes) instead of
// returning a non-OK status, and SiLA framework messages don't use
// google.protobuf.Any/Timestamp/Duration well-known types (whose JSON
// printers can fail on out-of-range values). No input reachable through a
// normally-constructed protobuf::Message was found that drives toJson()'s
// throw; this is a currently-unreachable branch, not an untested one.
#include <sila/client/dynamic/JsonCodec.h>

#include <sila/client/dynamic/DescriptorBuilder.h>

#include "SiLAFramework.pb.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {
using sila2::dynamic::DescriptorBuilder;
using sila2::dynamic::JsonCodec;
namespace fw = sila2::org::silastandard;

// --- True (positive) paths --------------------------------------------------

TEST(JsonCodec, ToJsonProducesValidJsonForPopulatedMessage) {
    fw::String msg;
    msg.set_value("hello");

    std::string json = JsonCodec::toJson(msg);

    EXPECT_NE(json.find("hello"), std::string::npos);
}

TEST(JsonCodec, FromJsonParsesValidJsonIntoMessage) {
    google::protobuf::DynamicMessageFactory factory;

    auto msg = JsonCodec::fromJson(R"({"value":"hello"})", fw::String::descriptor(), &factory);

    // A plain DynamicMessageFactory doesn't delegate to the compiled-in
    // fw::String class (that requires SetDelegateToGeneratedFactory(true)),
    // so the returned Message must be inspected through reflection rather
    // than dynamic_cast<fw::String*>.
    const auto* valueField = msg->GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    EXPECT_EQ(msg->GetReflection()->GetString(*msg, valueField), "hello");
}

// Round trip through a message assembled at runtime via the same
// FdlRuntimeParser -> DescriptorBuilder pipeline AnyCodec uses, verifying
// JsonCodec works against dynamically built descriptors, not just
// compiled-in generated types.
TEST(JsonCodec, RoundTripThroughDynamicallyBuiltDescriptor) {
    DescriptorBuilder builder;
    auto fileProto = builder.buildFromTypeXml("<DataType><Basic>String</Basic></DataType>");

    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    const auto* file = pool.BuildFile(fileProto);
    ASSERT_NE(file, nullptr);
    const auto* desc = pool.FindMessageTypeByName(fileProto.package() + ".DataType_Payload");
    ASSERT_NE(desc, nullptr);
    google::protobuf::DynamicMessageFactory factory{&pool};

    auto parsed = JsonCodec::fromJson(R"({"payload":{"value":"integration"}})", desc, &factory);
    std::string json = JsonCodec::toJson(*parsed);

    auto roundTripped = JsonCodec::fromJson(json, desc, &factory);
    const auto* payloadField = desc->FindFieldByName("Payload");
    ASSERT_NE(payloadField, nullptr);
    const auto& reflection = *roundTripped->GetReflection();
    const auto& payload = reflection.GetMessage(*roundTripped, payloadField);
    const auto* valueField = payload.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    EXPECT_EQ(payload.GetReflection()->GetString(payload, valueField), "integration");
}

// --- False (negative/error) paths -------------------------------------------

TEST(JsonCodec, FromJsonMalformedSyntaxThrows) {
    google::protobuf::DynamicMessageFactory factory;

    EXPECT_THROW(
        JsonCodec::fromJson("{ not valid json", fw::String::descriptor(), &factory),
        std::invalid_argument);
}

TEST(JsonCodec, FromJsonUnknownFieldNameThrows) {
    google::protobuf::DynamicMessageFactory factory;

    // fw::String only declares "value"; an unrecognized field name is
    // rejected because JsonStringToMessage defaults ignore_unknown_fields
    // to false.
    EXPECT_THROW(
        JsonCodec::fromJson(R"({"noSuchField":"x"})", fw::String::descriptor(), &factory),
        std::invalid_argument);
}

TEST(JsonCodec, FromJsonWrongFieldTypeThrows) {
    google::protobuf::DynamicMessageFactory factory;

    // "value" is declared as a string field; a JSON number is not
    // convertible, a distinct failure mode from a bad field name.
    EXPECT_THROW(
        JsonCodec::fromJson(R"({"value":123})", fw::String::descriptor(), &factory),
        std::invalid_argument);
}

}  // namespace
