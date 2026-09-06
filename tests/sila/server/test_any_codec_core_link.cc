// R10-9a proof: AnyCodec/DescriptorBuilder must be reachable from sila2_core
// alone, because the server-side Constraint validator (Part A p67 A224) has
// to decode an Any to check AllowedTypes without linking sila2::dynamic.
// This file is compiled into test_sila, which links sila2::core only
// (tests/CMakeLists.txt:88) -- it fails to LINK before the R10-9a source-list
// move and passes after. Mirrors payloadPrototype() from
// tests/sila/client/dynamic/test_any_codec.cc to build the encode-side
// fixture through the same DescriptorBuilder pipeline decode() expects.
#include <sila/client/dynamic/AnyCodec.h>

#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/common/types/BasicTypes.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using sila2::dynamic::AnyCodec;
using sila2::dynamic::DescriptorBuilder;
using sila2::types::AnyValue;

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

// --- True (positive) path ---------------------------------------------------

TEST(AnyCodecCoreLink, DecodeBasicIntegerReachableFromCore) {
    google::protobuf::DescriptorPool encodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory encodeFactory{&encodePool};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>";
    const auto* prototype = payloadPrototype(typeXml, encodePool, encodeFactory);
    ASSERT_NE(prototype, nullptr);
    std::unique_ptr<google::protobuf::Message> wrapper{prototype->New()};
    const auto* payloadField = wrapper->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(payloadField, nullptr);
    auto* inner = wrapper->GetReflection()->MutableMessage(wrapper.get(), payloadField);
    const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
    inner->GetReflection()->SetInt64(inner, valueField, 42);

    std::string serialized = wrapper->SerializeAsString();
    AnyValue any{typeXml, std::vector<uint8_t>{serialized.begin(), serialized.end()}};

    // Decode via a fresh pool/factory pair, same as the client-side AnyCodec
    // tests, to prove decode() does not depend on the encode-side pool state.
    google::protobuf::DescriptorPool decodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory decodeFactory{&decodePool};
    auto decoded = AnyCodec::decode(any, decodePool, &decodeFactory);

    const auto* decodedPayloadField = decoded->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(decodedPayloadField, nullptr);
    const auto& decodedInner = decoded->GetReflection()->GetMessage(*decoded, decodedPayloadField);
    const auto* decodedValueField = decodedInner.GetDescriptor()->FindFieldByName("value");
    EXPECT_EQ(decodedInner.GetReflection()->GetInt64(decodedInner, decodedValueField), 42);
}

// --- False (negative) path --------------------------------------------------

TEST(AnyCodecCoreLink, DecodeMalformedTypeXmlThrows) {
    AnyValue any{"<not closed", {}};
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};

    EXPECT_THROW(AnyCodec::decode(any, pool, &factory), std::invalid_argument);
}

}  // namespace
