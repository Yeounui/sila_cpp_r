// Tests for DescriptorBuilder::build() output structure (architecture.md
// §4.3): verifies the runtime-built FileDescriptorProto has the same shape
// (services, RPCs, streaming flags, message layout) that codegen would
// produce for the same FDL. Complements test_dynamic_pipeline.cc, which
// covers the parse->build pipeline and parser error paths end-to-end.
#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/client/dynamic/FdlIR.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace
{
using sila2::dynamic::DescriptorBuilder;
using sila2::dynamic::Feature;
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

TEST(DescriptorBuilderEquivalence, UnobservableCommandProducesSingleRpc) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
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
    const auto fileProto = makeBuilder().build(ir);
    const std::string pkg = "sila2.org.silastandardtest.test.testfeature.v1";

    EXPECT_EQ(fileProto.package(), pkg);
    ASSERT_EQ(fileProto.service_size(), 1);
    EXPECT_EQ(fileProto.service(0).name(), "TestFeature");
    ASSERT_EQ(fileProto.service(0).method_size(), 1);
    const auto& method = fileProto.service(0).method(0);
    EXPECT_EQ(method.name(), "DoSomething");
    EXPECT_FALSE(method.server_streaming());
    EXPECT_EQ(method.input_type(), "." + pkg + ".DoSomething_Parameters");
    EXPECT_EQ(method.output_type(), "." + pkg + ".DoSomething_Responses");

    ASSERT_EQ(fileProto.message_type_size(), 2);
    EXPECT_EQ(fileProto.message_type(0).name(), "DoSomething_Parameters");
    EXPECT_EQ(fileProto.message_type(1).name(), "DoSomething_Responses");
}

TEST(DescriptorBuilderEquivalence, ObservableCommandWithIntermediateResponsesProducesFourRpcs) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>ObservableFeature</Identifier>
  <DisplayName>Observable feature</DisplayName>
  <Description>Observable feature</Description>
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
    const auto fileProto = makeBuilder().build(ir);
    const std::string pkg = "sila2.org.silastandardtest.test.observablefeature.v1";
    const std::string kUuid = "sila2.org.silastandard.CommandExecutionUUID";

    ASSERT_EQ(fileProto.service_size(), 1);
    ASSERT_EQ(fileProto.service(0).method_size(), 4);

    const auto& initiation = fileProto.service(0).method(0);
    EXPECT_EQ(initiation.name(), "LongRunningTask");
    EXPECT_FALSE(initiation.server_streaming());
    EXPECT_EQ(initiation.input_type(), "." + pkg + ".LongRunningTask_Parameters");
    EXPECT_EQ(initiation.output_type(), ".sila2.org.silastandard.CommandConfirmation");

    const auto& info = fileProto.service(0).method(1);
    EXPECT_EQ(info.name(), "LongRunningTask_Info");
    EXPECT_TRUE(info.server_streaming());
    EXPECT_EQ(info.input_type(), "." + kUuid);
    EXPECT_EQ(info.output_type(), ".sila2.org.silastandard.ExecutionInfo");

    const auto& intermediate = fileProto.service(0).method(2);
    EXPECT_EQ(intermediate.name(), "LongRunningTask_Intermediate");
    EXPECT_TRUE(intermediate.server_streaming());
    EXPECT_EQ(intermediate.input_type(), "." + kUuid);
    EXPECT_EQ(intermediate.output_type(), "." + pkg + ".LongRunningTask_IntermediateResponses");

    const auto& result = fileProto.service(0).method(3);
    EXPECT_EQ(result.name(), "LongRunningTask_Result");
    EXPECT_FALSE(result.server_streaming());
    EXPECT_EQ(result.input_type(), "." + kUuid);
    EXPECT_EQ(result.output_type(), "." + pkg + ".LongRunningTask_Responses");

    ASSERT_EQ(fileProto.message_type_size(), 3);
    EXPECT_EQ(fileProto.message_type(0).name(), "LongRunningTask_Parameters");
    EXPECT_EQ(fileProto.message_type(1).name(), "LongRunningTask_Responses");
    EXPECT_EQ(fileProto.message_type(2).name(), "LongRunningTask_IntermediateResponses");
}

// Distinct branch from the test above: an observable command with no
// IntermediateResponse must omit both the _Intermediate RPC and its message.
TEST(DescriptorBuilderEquivalence, ObservableCommandWithoutIntermediateResponsesOmitsIntermediateRpc) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>ObservableNoIntermediateFeature</Identifier>
  <DisplayName>Observable feature</DisplayName>
  <Description>Observable feature</Description>
  <Command>
    <Identifier>Wait</Identifier>
    <DisplayName>Wait</DisplayName>
    <Description>Wait</Description>
    <Observable>Yes</Observable>
    <Response>
      <Identifier>Done</Identifier>
      <DisplayName>Done</DisplayName>
      <Description>Done</Description>
      <DataType><Basic>Boolean</Basic></DataType>
    </Response>
  </Command>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    const auto fileProto = makeBuilder().build(ir);

    ASSERT_EQ(fileProto.service(0).method_size(), 3);
    EXPECT_EQ(fileProto.service(0).method(0).name(), "Wait");
    EXPECT_EQ(fileProto.service(0).method(1).name(), "Wait_Info");
    EXPECT_EQ(fileProto.service(0).method(2).name(), "Wait_Result");

    ASSERT_EQ(fileProto.message_type_size(), 2);
    EXPECT_EQ(fileProto.message_type(0).name(), "Wait_Parameters");
    EXPECT_EQ(fileProto.message_type(1).name(), "Wait_Responses");
}

TEST(DescriptorBuilderEquivalence, PropertiesProduceGetAndSubscribeRpcs) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>PropertyFeature</Identifier>
  <DisplayName>Property feature</DisplayName>
  <Description>Property feature</Description>
  <Property>
    <Identifier>Temperature</Identifier>
    <DisplayName>Temperature</DisplayName>
    <Description>Temperature</Description>
    <Observable>Yes</Observable>
    <DataType><Basic>Real</Basic></DataType>
  </Property>
  <Property>
    <Identifier>SerialNumber</Identifier>
    <DisplayName>Serial number</DisplayName>
    <Description>Serial number</Description>
    <Observable>No</Observable>
    <DataType><Basic>String</Basic></DataType>
  </Property>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    const auto fileProto = makeBuilder().build(ir);
    const std::string pkg = "sila2.org.silastandardtest.test.propertyfeature.v1";

    ASSERT_EQ(fileProto.service(0).method_size(), 2);
    const auto& subscribe = fileProto.service(0).method(0);
    EXPECT_EQ(subscribe.name(), "Subscribe_Temperature");
    EXPECT_TRUE(subscribe.server_streaming());
    EXPECT_EQ(subscribe.input_type(), "." + pkg + ".Subscribe_Temperature_Parameters");
    EXPECT_EQ(subscribe.output_type(), "." + pkg + ".Subscribe_Temperature_Responses");

    const auto& get = fileProto.service(0).method(1);
    EXPECT_EQ(get.name(), "Get_SerialNumber");
    EXPECT_FALSE(get.server_streaming());
    EXPECT_EQ(get.input_type(), "." + pkg + ".Get_SerialNumber_Parameters");
    EXPECT_EQ(get.output_type(), "." + pkg + ".Get_SerialNumber_Responses");

    ASSERT_EQ(fileProto.message_type_size(), 4);
    const auto* subscribeParams = findMessage(fileProto, "Subscribe_Temperature_Parameters");
    ASSERT_NE(subscribeParams, nullptr);
    EXPECT_EQ(subscribeParams->field_size(), 0);
    const auto* subscribeResponses = findMessage(fileProto, "Subscribe_Temperature_Responses");
    ASSERT_NE(subscribeResponses, nullptr);
    ASSERT_EQ(subscribeResponses->field_size(), 1);
    EXPECT_EQ(subscribeResponses->field(0).name(), "Temperature");
    const auto* getParams = findMessage(fileProto, "Get_SerialNumber_Parameters");
    ASSERT_NE(getParams, nullptr);
    EXPECT_EQ(getParams->field_size(), 0);
    const auto* getResponses = findMessage(fileProto, "Get_SerialNumber_Responses");
    ASSERT_NE(getResponses, nullptr);
    ASSERT_EQ(getResponses->field_size(), 1);
    EXPECT_EQ(getResponses->field(0).name(), "SerialNumber");
}

// Mirrors tests/examples/fdl/sila_base/valid-fdl/Metadata.sila.xml, so the
// expected output is exactly tests/codegen/golden/Metadata.proto.
TEST(DescriptorBuilderEquivalence, MetadataProducesFcpRpcAndThreeMessages) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="tests">
  <Identifier>Metadata</Identifier>
  <DisplayName>Metadata</DisplayName>
  <Description>Metadata</Description>
  <Metadata>
    <Identifier>Meta1</Identifier>
    <DisplayName>Meta 1</DisplayName>
    <Description>Metadata 1</Description>
    <DataType><Basic>String</Basic></DataType>
  </Metadata>
  <Metadata>
    <Identifier>Meta2</Identifier>
    <DisplayName>Meta 2</DisplayName>
    <Description>Metadata 2</Description>
    <DataType><Basic>String</Basic></DataType>
  </Metadata>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    const auto fileProto = makeBuilder().build(ir);
    const std::string pkg = "sila2.org.silastandard.tests.metadata.v1";

    ASSERT_EQ(fileProto.service(0).method_size(), 2);
    const auto& meta1 = fileProto.service(0).method(0);
    EXPECT_EQ(meta1.name(), "Get_FCPAffectedByMetadata_Meta1");
    EXPECT_FALSE(meta1.server_streaming());
    EXPECT_EQ(meta1.input_type(), "." + pkg + ".Get_FCPAffectedByMetadata_Meta1_Parameters");
    EXPECT_EQ(meta1.output_type(), "." + pkg + ".Get_FCPAffectedByMetadata_Meta1_Responses");
    const auto& meta2 = fileProto.service(0).method(1);
    EXPECT_EQ(meta2.name(), "Get_FCPAffectedByMetadata_Meta2");

    const auto* meta1Params = findMessage(fileProto, "Get_FCPAffectedByMetadata_Meta1_Parameters");
    ASSERT_NE(meta1Params, nullptr);
    EXPECT_EQ(meta1Params->field_size(), 0);

    const auto* meta1Responses = findMessage(fileProto, "Get_FCPAffectedByMetadata_Meta1_Responses");
    ASSERT_NE(meta1Responses, nullptr);
    ASSERT_EQ(meta1Responses->field_size(), 1);
    EXPECT_EQ(meta1Responses->field(0).name(), "AffectedCalls");
    EXPECT_EQ(meta1Responses->field(0).number(), 1);
    EXPECT_EQ(meta1Responses->field(0).label(), google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
    EXPECT_EQ(meta1Responses->field(0).type_name(), ".sila2.org.silastandard.String");

    const auto* metaMessage1 = findMessage(fileProto, "Metadata_Meta1");
    ASSERT_NE(metaMessage1, nullptr);
    ASSERT_EQ(metaMessage1->field_size(), 1);
    EXPECT_EQ(metaMessage1->field(0).name(), "Meta1");
    EXPECT_EQ(metaMessage1->field(0).type_name(), ".sila2.org.silastandard.String");
}

// Pins the fdl2proto.xsl:52-60 emission order: Command RPCs, then Property
// RPCs, then Metadata RPCs -- last in the service.
TEST(DescriptorBuilderEquivalence, MetadataRpcsFollowCommandAndPropertyRpcs) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>MixedFeature</Identifier>
  <DisplayName>Mixed feature</DisplayName>
  <Description>Mixed feature</Description>
  <Command>
    <Identifier>DoSomething</Identifier>
    <DisplayName>Do something</DisplayName>
    <Description>Do something</Description>
    <Observable>No</Observable>
  </Command>
  <Property>
    <Identifier>SerialNumber</Identifier>
    <DisplayName>Serial number</DisplayName>
    <Description>Serial number</Description>
    <Observable>No</Observable>
    <DataType><Basic>String</Basic></DataType>
  </Property>
  <Metadata>
    <Identifier>Meta1</Identifier>
    <DisplayName>Meta 1</DisplayName>
    <Description>Metadata 1</Description>
    <DataType><Basic>String</Basic></DataType>
  </Metadata>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    const auto fileProto = makeBuilder().build(ir);

    ASSERT_EQ(fileProto.service(0).method_size(), 3);
    EXPECT_EQ(fileProto.service(0).method(0).name(), "DoSomething");
    EXPECT_EQ(fileProto.service(0).method(1).name(), "Get_SerialNumber");
    EXPECT_EQ(fileProto.service(0).method(2).name(), "Get_FCPAffectedByMetadata_Meta1");
}

// --- False (negative/boundary) paths ----------------------------------------
//
// DescriptorBuilder::build() is a pure IR->proto transform with no
// validation branches of its own (invalid input is already rejected by
// FdlRuntimeParser, covered in test_dynamic_pipeline.cc). The cases below
// are therefore boundary/degenerate inputs verifying build() stays well-
// defined at the edges of its input space, not thrown exceptions.

TEST(DescriptorBuilderEquivalence, EmptyFeatureProducesEmptyService) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>EmptyFeature</Identifier>
  <DisplayName>Empty feature</DisplayName>
  <Description>Empty feature</Description>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    ASSERT_TRUE(ir.commands.empty());
    ASSERT_TRUE(ir.properties.empty());
    ASSERT_TRUE(ir.dataTypeDefinitions.empty());

    const auto fileProto = makeBuilder().build(ir);

    EXPECT_EQ(fileProto.package(), "sila2.org.silastandardtest.test.emptyfeature.v1");
    ASSERT_EQ(fileProto.dependency_size(), 1);
    EXPECT_EQ(fileProto.dependency(0), "SiLAFramework.proto");
    ASSERT_EQ(fileProto.service_size(), 1);
    EXPECT_EQ(fileProto.service(0).name(), "EmptyFeature");
    EXPECT_EQ(fileProto.service(0).method_size(), 0);
    EXPECT_EQ(fileProto.message_type_size(), 0);
}

TEST(DescriptorBuilderEquivalence, DataTypeDefinitionProducesTypeMessage) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>TypeDefFeature</Identifier>
  <DisplayName>Type definition feature</DisplayName>
  <Description>Type definition feature</Description>
  <DataTypeDefinition>
    <Identifier>CustomString</Identifier>
    <DisplayName>Custom string</DisplayName>
    <Description>Custom string</Description>
    <DataType><Basic>String</Basic></DataType>
  </DataTypeDefinition>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    ASSERT_EQ(ir.dataTypeDefinitions.size(), 1u);

    const auto fileProto = makeBuilder().build(ir);

    ASSERT_EQ(fileProto.message_type_size(), 1);
    EXPECT_EQ(fileProto.message_type(0).name(), "DataType_CustomString");
    ASSERT_EQ(fileProto.message_type(0).field_size(), 1);
    EXPECT_EQ(fileProto.message_type(0).field(0).name(), "CustomString");
    EXPECT_EQ(fileProto.service(0).method_size(), 0);
}

TEST(DescriptorBuilderEquivalence, DefinedExecutionErrorDoesNotAffectDescriptorStructure) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>ErrorFeature</Identifier>
  <DisplayName>Error feature</DisplayName>
  <Description>Error feature</Description>
  <Command>
    <Identifier>Risky</Identifier>
    <DisplayName>Risky</DisplayName>
    <Description>Risky</Description>
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
    <DefinedExecutionErrors>
      <Identifier>SomethingWentWrong</Identifier>
    </DefinedExecutionErrors>
  </Command>
  <DefinedExecutionError>
    <Identifier>SomethingWentWrong</Identifier>
    <DisplayName>Something went wrong</DisplayName>
    <Description>Something went wrong</Description>
  </DefinedExecutionError>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    ASSERT_EQ(ir.commands.size(), 1u);
    EXPECT_EQ(ir.commands[0].definedExecutionErrors.size(), 1u);

    const auto fileProto = makeBuilder().build(ir);

    // Same shape as a plain unobservable command with no declared errors:
    // one RPC, two messages, nothing named after the error.
    ASSERT_EQ(fileProto.service(0).method_size(), 1);
    EXPECT_EQ(fileProto.service(0).method(0).name(), "Risky");
    ASSERT_EQ(fileProto.message_type_size(), 2);
    EXPECT_EQ(findMessage(fileProto, "SomethingWentWrong"), nullptr);
}

// Proves the metadata loops in build() are inert for a metadata-free
// feature -- no stray RPC or message named after the FCP convention.
TEST(DescriptorBuilderEquivalence, FeatureWithoutMetadataEmitsNoFcpRpc) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>CommandOnlyFeature</Identifier>
  <DisplayName>Command only feature</DisplayName>
  <Description>Command only feature</Description>
  <Command>
    <Identifier>DoSomething</Identifier>
    <DisplayName>Do something</DisplayName>
    <Description>Do something</Description>
    <Observable>No</Observable>
  </Command>
</Feature>
)xml";

    const Feature ir = parseFdl(kFdl);
    ASSERT_TRUE(ir.metadata.empty());
    const auto fileProto = makeBuilder().build(ir);

    ASSERT_EQ(fileProto.service(0).method_size(), 1);
    EXPECT_EQ(fileProto.service(0).method(0).name(), "DoSomething");
    ASSERT_EQ(fileProto.message_type_size(), 2);
    for (const auto& msg : fileProto.message_type()) {
        EXPECT_EQ(msg.name().find("Get_FCPAffectedByMetadata_"), std::string::npos);
    }
}

TEST(DescriptorBuilderEquivalence, MetadataMissingDataTypeThrows) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandardtest" Category="test">
  <Identifier>BrokenMetadataFeature</Identifier>
  <DisplayName>Broken metadata feature</DisplayName>
  <Description>Broken metadata feature</Description>
  <Metadata>
    <Identifier>Meta1</Identifier>
    <DisplayName>Meta 1</DisplayName>
    <Description>Metadata 1</Description>
  </Metadata>
</Feature>
)xml";

    EXPECT_THROW(parseFdl(kFdl), std::invalid_argument);
}

}  // namespace
