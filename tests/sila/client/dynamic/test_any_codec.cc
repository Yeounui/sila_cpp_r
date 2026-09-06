// Integration tests for AnyCodec::decode()/encode() (architecture.md §4.2):
// the SiLA Any <-> protobuf Message bridge. decode() drives the full
// typeXml -> DescriptorBuilder::buildFromTypeXml() -> FdlRuntimeParser ->
// DescriptorPool::BuildFile() pipeline before parsing the payload bytes, so
// these tests build the encode-side fixture message through that very same
// pipeline to keep the wire shape consistent with what decode() expects.
#include <sila/client/dynamic/AnyCodec.h>

#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/common/types/BasicTypes.h>

#include "SiLAFramework.pb.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {
using sila2::dynamic::AnyCodec;
using sila2::dynamic::DescriptorBuilder;
using sila2::types::AnyValue;
namespace fw = sila2::org::silastandard;

// Builds the "DataType_Payload" message prototype that buildFromTypeXml()
// derives from `typeXml`, registered into `pool` (which must outlive the
// returned prototype and `factory`).
const google::protobuf::Message* payloadPrototype(
    const std::string& typeXml, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
    DescriptorBuilder builder;
    auto fileProto = builder.buildFromTypeXml(typeXml);
    const auto* file = pool.BuildFile(fileProto);
    if (file == nullptr) return nullptr;
    const auto* desc = pool.FindMessageTypeByName(fileProto.package() + ".DataType_Payload");
    if (desc == nullptr) return nullptr;
    return factory.GetPrototype(desc);
}

// --- True (positive) paths --------------------------------------------------

TEST(AnyCodec, DecodeValidBasicTypePayload) {
    google::protobuf::DescriptorPool encodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory encodeFactory{&encodePool};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic></DataType>";
    const auto* prototype = payloadPrototype(typeXml, encodePool, encodeFactory);
    ASSERT_NE(prototype, nullptr);
    std::unique_ptr<google::protobuf::Message> wrapper{prototype->New()};
    const auto* payloadField = wrapper->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(payloadField, nullptr);
    auto* inner = wrapper->GetReflection()->MutableMessage(wrapper.get(), payloadField);
    const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
    inner->GetReflection()->SetString(inner, valueField, "hello");

    std::string serialized = wrapper->SerializeAsString();
    AnyValue any{typeXml, std::vector<uint8_t>{serialized.begin(), serialized.end()}};

    google::protobuf::DescriptorPool decodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory decodeFactory{&decodePool};
    auto decoded = AnyCodec::decode(any, decodePool, &decodeFactory);

    const auto* decodedPayloadField = decoded->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(decodedPayloadField, nullptr);
    const auto& decodedInner = decoded->GetReflection()->GetMessage(*decoded, decodedPayloadField);
    const auto* decodedValueField = decodedInner.GetDescriptor()->FindFieldByName("value");
    EXPECT_EQ(decodedInner.GetReflection()->GetString(decodedInner, decodedValueField), "hello");
}

// Distinct branch from the basic-type case above: List resolves through
// resolveType()'s LABEL_REPEATED path, producing a repeated message field
// instead of a singular one.
TEST(AnyCodec, DecodeListTypePayloadProducesRepeatedField) {
    google::protobuf::DescriptorPool encodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory encodeFactory{&encodePool};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><List><DataType><Basic>Integer</Basic>"
        "</DataType></List></DataType>";
    const auto* prototype = payloadPrototype(typeXml, encodePool, encodeFactory);
    ASSERT_NE(prototype, nullptr);
    std::unique_ptr<google::protobuf::Message> wrapper{prototype->New()};
    const auto* payloadField = wrapper->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(payloadField, nullptr);
    ASSERT_TRUE(payloadField->is_repeated());
    for (int64_t v : {10, 20, 30}) {
        auto* entry = wrapper->GetReflection()->AddMessage(wrapper.get(), payloadField);
        const auto* valueField = entry->GetDescriptor()->FindFieldByName("value");
        entry->GetReflection()->SetInt64(entry, valueField, v);
    }

    std::string serialized = wrapper->SerializeAsString();
    AnyValue any{typeXml, std::vector<uint8_t>{serialized.begin(), serialized.end()}};

    google::protobuf::DescriptorPool decodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory decodeFactory{&decodePool};
    auto decoded = AnyCodec::decode(any, decodePool, &decodeFactory);

    const auto* decodedPayloadField = decoded->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(decodedPayloadField, nullptr);
    EXPECT_EQ(decoded->GetReflection()->FieldSize(*decoded, decodedPayloadField), 3);
}

TEST(AnyCodec, EncodeProducesAnyValueWithGivenTypeXmlAndSerializedPayload) {
    fw::String msg;
    msg.set_value("encoded-value");

    AnyValue result = AnyCodec::encode(msg, "<DataType><Basic>String</Basic></DataType>");

    EXPECT_EQ(result.typeXml, "<DataType><Basic>String</Basic></DataType>");
    std::string expectedPayload = msg.SerializeAsString();
    EXPECT_EQ(result.payload, (std::vector<uint8_t>{expectedPayload.begin(), expectedPayload.end()}));
}

// --- False (negative/error) paths -------------------------------------------

TEST(AnyCodec, DecodeMalformedTypeXmlThrows) {
    AnyValue any{"<not closed", {}};
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};

    EXPECT_THROW(AnyCodec::decode(any, pool, &factory), std::invalid_argument);
}

TEST(AnyCodec, DecodeEmptyTypeXmlMissingDataTypeElementThrows) {
    // An empty typeXml cannot form a standalone <DataType> document.
    AnyValue any{"", {}};
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};

    EXPECT_THROW(AnyCodec::decode(any, pool, &factory), std::invalid_argument);
}

TEST(AnyCodec, DecodeMalformedPayloadThrows) {
    AnyValue any{"<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic></DataType>",
                 std::vector<uint8_t>{0x0a, 0x01}};
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};

    EXPECT_THROW(AnyCodec::decode(any, pool, &factory), std::invalid_argument);
}

TEST(AnyCodec, DecodeNullFactoryThrows) {
    AnyValue any{"<DataType><Basic>String</Basic></DataType>", {}};
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};

    EXPECT_THROW(AnyCodec::decode(any, pool, nullptr), std::invalid_argument);
}

TEST(AnyCodec, DecodePoolBuildFailureForUnresolvableTypeReferenceThrows) {
    // References a DataTypeIdentifier that is never defined anywhere in the
    // synthetic single-type file buildFromTypeXml() produces, so
    // DescriptorPool::BuildFile() cannot resolve the field's type_name and
    // returns nullptr -- the "failed to build descriptor" branch, distinct
    // from the parseFdl()-level failures above.
    //
    // NOTE: AnyCodec::decode()'s "DataType_Payload message not found" branch
    // (reached when BuildFile succeeds but the expected message is absent)
    // is not exercised here: buildFromTypeXml() always names the synthetic
    // DataTypeDefinition "Payload", so whenever BuildFile succeeds the
    // message is guaranteed to exist. That branch is unreachable through the
    // public decode() API as currently implemented.
    AnyValue any{"<DataType xmlns=\"http://www.sila-standard.org\"><DataTypeIdentifier>Undefined</DataTypeIdentifier>"
                 "</DataType>",
                 {}};
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};

    EXPECT_THROW(AnyCodec::decode(any, pool, &factory), std::invalid_argument);
}

}  // namespace
