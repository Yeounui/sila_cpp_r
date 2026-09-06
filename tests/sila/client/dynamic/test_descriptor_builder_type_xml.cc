// Integration tests for DescriptorBuilder::buildFromTypeXml() (architecture.md
// §4.2): parses a bare <DataType> fragment (as carried by a SiLA Any value's
// typeXml) into a synthetic single-type Feature IR, then runs it through the
// same build() pipeline as a full feature definition. Complements
// test_descriptor_builder.cc (which covers build() on parsed Features) and
// test_any_codec.cc (which exercises buildFromTypeXml() indirectly through
// AnyCodec::decode()).
#include <sila/client/dynamic/DescriptorBuilder.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {
using sila2::dynamic::DescriptorBuilder;

DescriptorBuilder makeBuilder() {
    return DescriptorBuilder{};
}

// --- True (positive) paths --------------------------------------------------

TEST(DescriptorBuilderTypeXml, BasicTypeProducesDataTypePayloadMessage) {
    auto fileProto = makeBuilder().buildFromTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic></DataType>");

    ASSERT_EQ(fileProto.message_type_size(), 1);
    EXPECT_EQ(fileProto.message_type(0).name(), "DataType_Payload");
    ASSERT_EQ(fileProto.message_type(0).field_size(), 1);
    EXPECT_EQ(fileProto.message_type(0).field(0).name(), "Payload");
    EXPECT_EQ(fileProto.message_type(0).field(0).type_name(), ".sila2.org.silastandard.String");
}

TEST(DescriptorBuilderTypeXml, BareDataTypeFragmentIsAccepted) {
    auto fileProto = makeBuilder().buildFromTypeXml(
        "<DataType><Basic>String</Basic></DataType>");

    ASSERT_EQ(fileProto.message_type_size(), 1);
    ASSERT_EQ(fileProto.message_type(0).field_size(), 1);
    EXPECT_EQ(fileProto.message_type(0).field(0).type_name(),
              ".sila2.org.silastandard.String");
}

TEST(DescriptorBuilderTypeXml, StructureTypeProducesNestedType) {
    auto fileProto = makeBuilder().buildFromTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Structure>"
        "<Element><Identifier>X</Identifier><DisplayName>X</DisplayName>"
        "<Description>X</Description><DataType><Basic>Real</Basic></DataType></Element>"
        "</Structure></DataType>");

    ASSERT_EQ(fileProto.message_type_size(), 1);
    const auto& payloadMsg = fileProto.message_type(0);
    ASSERT_EQ(payloadMsg.nested_type_size(), 1);
    EXPECT_EQ(payloadMsg.nested_type(0).name(), "Payload_Struct");
    ASSERT_EQ(payloadMsg.nested_type(0).field_size(), 1);
    EXPECT_EQ(payloadMsg.nested_type(0).field(0).name(), "X");
}

// Distinct typeXml values must hash to distinct synthetic package names, so
// that repeated AnyCodec::decode() calls with different payload types never
// collide inside a shared DescriptorPool.
TEST(DescriptorBuilderTypeXml, DistinctTypeXmlValuesProduceDistinctPackages) {
    auto builder = makeBuilder();

    auto stringFile = builder.buildFromTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic></DataType>");
    auto integerFile = builder.buildFromTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>");

    EXPECT_NE(stringFile.package(), integerFile.package());
}

// --- False (negative/error) paths -------------------------------------------

TEST(DescriptorBuilderTypeXml, MalformedXmlThrows) {
    EXPECT_THROW(makeBuilder().buildFromTypeXml("<not closed"), std::invalid_argument);
}

TEST(DescriptorBuilderTypeXml, EmptyTypeXmlMissingDataTypeElementThrows) {
    EXPECT_THROW(makeBuilder().buildFromTypeXml(""), std::invalid_argument);
}

TEST(DescriptorBuilderTypeXml, UnknownBasicTypeThrows) {
    EXPECT_THROW(
        makeBuilder().buildFromTypeXml(
            "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>NotARealType</Basic>"
            "</DataType>"),
        std::invalid_argument);
}

TEST(DescriptorBuilderTypeXml, WrapperEscapeIsRejected) {
    EXPECT_THROW(makeBuilder().buildFromTypeXml(
                     "</DataTypeDefinition><DataTypeDefinition>"
                     "<Identifier>Injected</Identifier><DisplayName>Injected</DisplayName>"
                     "<Description></Description><DataType><Basic>String</Basic></DataType>"
                     "</DataTypeDefinition>"),
                 std::invalid_argument);
}

TEST(DescriptorBuilderTypeXml, MultipleRootDataTypesAreRejected) {
    EXPECT_THROW(makeBuilder().buildFromTypeXml(
                     "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic>"
                     "</DataType><DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer"
                     "</Basic></DataType>"),
                 std::invalid_argument);
}

}  // namespace
