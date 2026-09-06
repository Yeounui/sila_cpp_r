// Tests for the dynamic pipeline (architecture.md §4.2): FDL XML string ->
// parseFdl() -> Feature IR -> DescriptorBuilder::build() ->
// FileDescriptorProto.
#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/client/dynamic/FdlIR.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>
#include <sila/client/dynamic/FeatureCatalog.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace
{
using sila2::dynamic::DescriptorBuilder;
using sila2::dynamic::Feature;
using sila2::dynamic::FeatureCatalog;
using sila2::dynamic::parseFdl;

DescriptorBuilder makeBuilder() {
    return DescriptorBuilder{};
}

const google::protobuf::DescriptorProto* findMessage(
    const google::protobuf::FileDescriptorProto& file, const std::string& name) {
    for (const auto& msg : file.message_type()) {
        if (msg.name() == name) return &msg;
    }
    return nullptr;
}

// --- True (positive) paths --------------------------------------------------

TEST(DynamicPipeline, SimpleUnobservableCommand) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="core">
  <Identifier>TestFeature</Identifier>
  <DisplayName>Test feature</DisplayName>
  <Description>Test feature description</Description>
  <Command>
    <Identifier>DoSomething</Identifier>
    <DisplayName>Do something</DisplayName>
    <Description>Do something</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>Input</Identifier>
      <DisplayName>Input</DisplayName>
      <Description>Input</Description>
      <DataType><Basic>String</Basic></DataType>
    </Parameter>
    <Response>
      <Identifier>Output</Identifier>
      <DisplayName>Output</DisplayName>
      <Description>Output</Description>
      <DataType><Basic>String</Basic></DataType>
    </Response>
  </Command>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    EXPECT_EQ(ir.identifier, "TestFeature");
    EXPECT_EQ(ir.originator, "org.silastandard");
    EXPECT_EQ(ir.category, "core");
    EXPECT_EQ(ir.featureVersion, "1.0");
    ASSERT_EQ(ir.commands.size(), 1u);
    EXPECT_FALSE(ir.commands[0].observable);

    auto builder = makeBuilder();
    const auto fileProto = builder.build(ir);

    EXPECT_EQ(fileProto.package(), "sila2.org.silastandard.core.testfeature.v1");
    ASSERT_EQ(fileProto.service_size(), 1);
    EXPECT_EQ(fileProto.service(0).name(), "TestFeature");
    ASSERT_EQ(fileProto.service(0).method_size(), 1);
    EXPECT_EQ(fileProto.service(0).method(0).name(), "DoSomething");
    EXPECT_EQ(fileProto.service(0).method(0).input_type(),
              ".sila2.org.silastandard.core.testfeature.v1.DoSomething_Parameters");
    EXPECT_EQ(fileProto.service(0).method(0).output_type(),
              ".sila2.org.silastandard.core.testfeature.v1.DoSomething_Responses");
}

TEST(DynamicPipeline, ObservableCommandWithIntermediateResponses) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>ObservableFeature</Identifier>
  <DisplayName>Observable feature</DisplayName>
  <Description>Observable feature description</Description>
  <Command>
    <Identifier>LongRunningTask</Identifier>
    <DisplayName>Long running task</DisplayName>
    <Description>Long running task</Description>
    <Observable>Yes</Observable>
    <Parameter>
      <Identifier>Duration</Identifier>
      <DisplayName>Duration</DisplayName>
      <Description>Duration</Description>
      <DataType><Basic>Integer</Basic></DataType>
    </Parameter>
    <Response>
      <Identifier>Result</Identifier>
      <DisplayName>Result</DisplayName>
      <Description>Result</Description>
      <DataType><Basic>String</Basic></DataType>
    </Response>
    <IntermediateResponse>
      <Identifier>Progress</Identifier>
      <DisplayName>Progress</DisplayName>
      <Description>Progress</Description>
      <DataType><Basic>Real</Basic></DataType>
    </IntermediateResponse>
  </Command>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    ASSERT_EQ(ir.commands.size(), 1u);
    EXPECT_TRUE(ir.commands[0].observable);
    EXPECT_EQ(ir.commands[0].intermediateResponses.size(), 1u);

    auto builder = makeBuilder();
    const auto fileProto = builder.build(ir);

    ASSERT_EQ(fileProto.service_size(), 1);
    ASSERT_EQ(fileProto.service(0).method_size(), 4);
    EXPECT_EQ(fileProto.service(0).method(0).name(), "LongRunningTask");
    EXPECT_EQ(fileProto.service(0).method(1).name(), "LongRunningTask_Info");
    EXPECT_EQ(fileProto.service(0).method(2).name(), "LongRunningTask_Intermediate");
    EXPECT_EQ(fileProto.service(0).method(3).name(), "LongRunningTask_Result");
}

TEST(DynamicPipeline, PropertyObservableAndNonObservable) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>PropertyFeature</Identifier>
  <DisplayName>Property feature</DisplayName>
  <Description>Property feature description</Description>
  <Property>
    <Identifier>ObservableProp</Identifier>
    <DisplayName>Observable property</DisplayName>
    <Description>Observable property</Description>
    <Observable>Yes</Observable>
    <DataType><Basic>String</Basic></DataType>
  </Property>
  <Property>
    <Identifier>PlainProp</Identifier>
    <DisplayName>Plain property</DisplayName>
    <Description>Plain property</Description>
    <Observable>No</Observable>
    <DataType><Basic>Integer</Basic></DataType>
  </Property>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    ASSERT_EQ(ir.properties.size(), 2u);

    auto builder = makeBuilder();
    const auto fileProto = builder.build(ir);

    ASSERT_EQ(fileProto.service(0).method_size(), 2);
    EXPECT_EQ(fileProto.service(0).method(0).name(), "Subscribe_ObservableProp");
    EXPECT_TRUE(fileProto.service(0).method(0).server_streaming());
    EXPECT_EQ(fileProto.service(0).method(1).name(), "Get_PlainProp");
    EXPECT_FALSE(fileProto.service(0).method(1).server_streaming());
}

TEST(DynamicPipeline, NestedStructureType) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>StructFeature</Identifier>
  <DisplayName>Structure feature</DisplayName>
  <Description>Structure feature description</Description>
  <Command>
    <Identifier>Compute</Identifier>
    <DisplayName>Compute</DisplayName>
    <Description>Compute</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>Point</Identifier>
      <DisplayName>Point</DisplayName>
      <Description>Point</Description>
      <DataType>
        <Structure>
          <Element>
            <Identifier>X</Identifier>
            <DisplayName>X</DisplayName>
            <Description>X</Description>
            <DataType><Basic>Real</Basic></DataType>
          </Element>
          <Element>
            <Identifier>Y</Identifier>
            <DisplayName>Y</DisplayName>
            <Description>Y</Description>
            <DataType><Basic>Real</Basic></DataType>
          </Element>
        </Structure>
      </DataType>
    </Parameter>
    <Response>
      <Identifier>Output</Identifier>
      <DisplayName>Output</DisplayName>
      <Description>Output</Description>
      <DataType><Basic>String</Basic></DataType>
    </Response>
  </Command>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    auto builder = makeBuilder();
    const auto fileProto = builder.build(ir);

    const auto* parametersMsg = findMessage(fileProto, "Compute_Parameters");
    ASSERT_NE(parametersMsg, nullptr);
    ASSERT_EQ(parametersMsg->field_size(), 1);
    EXPECT_EQ(parametersMsg->field(0).name(), "Point");

    ASSERT_EQ(parametersMsg->nested_type_size(), 1);
    const auto& nested = parametersMsg->nested_type(0);
    EXPECT_EQ(nested.name(), "Point_Struct");
    ASSERT_EQ(nested.field_size(), 2);
    EXPECT_EQ(nested.field(0).name(), "X");
    EXPECT_EQ(nested.field(1).name(), "Y");
}

// Proves FeatureCatalog::messageDescriptor() closes the reachability gap:
// Metadata_Meta1 is neither an RPC input nor an output, so without this
// accessor it cannot be reached from requestDescriptor/responseDescriptor.
TEST(DynamicPipeline, CatalogResolvesMetadataMessageAndFcpRpc) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="tests">
  <Identifier>Metadata</Identifier>
  <DisplayName>Metadata</DisplayName>
  <Description>Metadata</Description>
  <Metadata>
    <Identifier>Meta1</Identifier>
    <DisplayName>Meta1</DisplayName>
    <Description>Meta1</Description>
    <DataType><Basic>String</Basic></DataType>
  </Metadata>
</Feature>
)xml";
    // v1, not "1.0": FeatureCatalog::add() now checks the caller's FQI
    // against the identity the FDL itself declares (S44), which truncates
    // FeatureVersion to its major segment and prefixes it with "v".
    const std::string fqi = "org.silastandard/tests/Metadata/v1";

    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    catalog.add(fqi, kFdl);

    EXPECT_EQ(catalog.grpcMethodName(fqi, "Get_FCPAffectedByMetadata_Meta1"),
              "/sila2.org.silastandard.tests.metadata.v1.Metadata/Get_FCPAffectedByMetadata_Meta1");

    const auto* responseDesc = catalog.responseDescriptor(fqi, "Get_FCPAffectedByMetadata_Meta1");
    ASSERT_NE(responseDesc, nullptr);
    const auto* affectedField = responseDesc->FindFieldByName("AffectedCalls");
    ASSERT_NE(affectedField, nullptr);
    EXPECT_TRUE(affectedField->is_repeated());

    const auto* metadataDesc = catalog.messageDescriptor(fqi, "Metadata_Meta1");
    ASSERT_NE(metadataDesc, nullptr);
    ASSERT_EQ(metadataDesc->field_count(), 1);
    EXPECT_EQ(metadataDesc->field(0)->name(), "Meta1");

    // The accessor is only useful if the descriptor it returns can actually
    // build and serialize a message -- the whole point of closing the gap.
    std::unique_ptr<google::protobuf::Message> metadataMsg{
        catalog.messageFactory().GetPrototype(metadataDesc)->New()};
    auto* valueMsg = metadataMsg->GetReflection()->MutableMessage(
        metadataMsg.get(), metadataDesc->field(0));
    const auto* valueField = valueMsg->GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    valueMsg->GetReflection()->SetString(valueMsg, valueField, "secret");
    EXPECT_FALSE(metadataMsg->SerializeAsString().empty());
}

TEST(DynamicPipeline, UnknownMessageNameThrows) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="tests">
  <Identifier>Metadata</Identifier>
  <DisplayName>Metadata</DisplayName>
  <Description>Metadata</Description>
  <Metadata>
    <Identifier>Meta1</Identifier>
    <DisplayName>Meta1</DisplayName>
    <Description>Meta1</Description>
    <DataType><Basic>String</Basic></DataType>
  </Metadata>
</Feature>
)xml";
    // See the comment on the same literal above (S44 identity check).
    const std::string fqi = "org.silastandard/tests/Metadata/v1";

    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    catalog.add(fqi, kFdl);

    try {
        (void)catalog.messageDescriptor(fqi, "Metadata_NoSuchThing");
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string{e.what()}.find("sila2.org.silastandard.tests.metadata.v1.Metadata_NoSuchThing"),
                  std::string::npos);
    }
}

// --- False (negative/error) paths -------------------------------------------

TEST(DynamicPipeline, MalformedXmlThrows) {
    EXPECT_THROW(parseFdl("<this is not valid xml"), std::invalid_argument);
}

TEST(DynamicPipeline, MissingIdentifierThrows) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="core">
  <DisplayName>Invalid feature</DisplayName>
  <Description>Invalid feature</Description>
  <Command>
    <Identifier>DoSomething</Identifier>
    <Observable>No</Observable>
  </Command>
</Feature>
)xml";

    try {
        parseFdl(kFdl);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string{e.what()}.find("XSD"), std::string::npos);
    }
}

TEST(DynamicPipeline, UnknownBasicTypeThrows) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="core">
  <Identifier>TestFeature</Identifier>
  <DisplayName>Test feature</DisplayName>
  <Description>Test feature</Description>
  <Command>
    <Identifier>DoSomething</Identifier>
    <DisplayName>Do something</DisplayName>
    <Description>Do something</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>Input</Identifier>
      <DisplayName>Input</DisplayName>
      <Description>Input</Description>
      <DataType><Basic>FooBar</Basic></DataType>
    </Parameter>
  </Command>
</Feature>
)xml";

    EXPECT_THROW(parseFdl(kFdl), std::invalid_argument);
}

TEST(DynamicPipeline, ExcessiveDataTypeNestingThrows) {
    std::string fdl =
        "<Feature xmlns=\"http://www.sila-standard.org\" SiLA2Version=\"1.0\" FeatureVersion=\"1.0\" Originator=\"org.silastandard\" Category=\"core\">"
        "<Identifier>TestFeature</Identifier><DisplayName>Test feature</DisplayName>"
        "<Description>Test feature</Description><Command><Identifier>DoSomething</Identifier>"
        "<DisplayName>Do something</DisplayName><Description>Do something</Description>"
        "<Observable>No</Observable><Parameter><Identifier>Input</Identifier>"
        "<DisplayName>Input</DisplayName><Description>Input</Description><DataType>";
    for (int i = 0; i != 65; ++i) fdl += "<List><DataType>";
    fdl += "<Basic>String</Basic>";
    for (int i = 0; i != 65; ++i) fdl += "</DataType></List>";
    fdl += "</DataType></Parameter></Command></Feature>";

    EXPECT_THROW(parseFdl(fdl), std::invalid_argument);
}

}  // namespace
