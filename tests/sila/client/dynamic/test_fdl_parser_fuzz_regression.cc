// Regression companion to tests/fuzz/fuzz_fdl_parser.cc: parseFdl() must
// either return a valid Feature IR or throw std::invalid_argument for every
// input — never crash, hang, or exhibit UB. Positive cases exercise each
// DataType variant and constraint kind; negative cases pin down inputs the
// libfuzzer target has flagged as edge cases (empty/garbage/truncated/deep
// nesting/embedded NUL) to a fixed expected outcome.
#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <gtest/gtest.h>

#include <string>
#include <variant>

namespace
{
using sila2::dynamic::BasicType;
using sila2::dynamic::DataType;
using sila2::dynamic::Feature;
using sila2::dynamic::parseFdl;

// Builds a <DataType> containing `depth` nested <List> levels around a
// <Basic>String</Basic> leaf, wrapped in a minimal Feature/DataTypeDefinition.
std::string BuildDeeplyNestedListXml(int depth) {
    std::string open;
    std::string close;
    for (int i = 0; i < depth; ++i) {
        open += "<List><DataType>";
        close = "</DataType></List>" + close;
    }
    return "<Feature xmlns=\"http://www.sila-standard.org\" SiLA2Version=\"1.0\" FeatureVersion=\"1.0\" Originator=\"org.test\" Category=\"test\">\n"
           "  <Identifier>TestFeature</Identifier>\n"
           "  <DisplayName>Test feature</DisplayName>\n"
           "  <Description>Test feature</Description>\n"
           "  <DataTypeDefinition>\n"
           "    <Identifier>Nested</Identifier>\n"
           "    <DisplayName>Nested</DisplayName>\n"
           "    <Description>Nested</Description>\n"
           "    <DataType>" + open + "<Basic>String</Basic>" + close + "</DataType>\n"
           "  </DataTypeDefinition>\n"
           "</Feature>\n";
}

// Inserts a NUL byte into the root element name so the tag can never match
// <Feature>, while keeping the input otherwise well-formed XML.
std::string BuildNullByteXml() {
    std::string xml =
        "<Feature FeatureVersion=\"1.0\" Originator=\"org.test\">\n"
        "  <Identifier>TestFeature</Identifier>\n"
        "</Feature>\n";
    xml.insert(1, 1, '\0');
    return xml;
}

}  // namespace

// --- True (positive) paths --------------------------------------------------

TEST(FdlParserFuzzRegression, MinimalFeatureWithoutOptionalCategoryParses) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.test">
  <Identifier>TestFeature</Identifier>
  <DisplayName>Test feature</DisplayName>
  <Description>Test feature</Description>
</Feature>
)xml";

    const Feature feature = parseFdl(kFdl);

    EXPECT_EQ(feature.identifier, "TestFeature");
    EXPECT_EQ(feature.featureVersion, "1.0");
    EXPECT_EQ(feature.originator, "org.test");
    EXPECT_TRUE(feature.category.empty());
    EXPECT_TRUE(feature.commands.empty());
}

TEST(FdlParserFuzzRegression, CommandWithParametersResponsesAndErrorsParses) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.test" Category="test">
  <Identifier>TestFeature</Identifier>
  <DisplayName>Test feature</DisplayName>
  <Description>Test feature</Description>
  <Command>
    <Identifier>RunTask</Identifier>
    <DisplayName>Run task</DisplayName>
    <Description>Run task</Description>
    <Observable>Yes</Observable>
    <Parameter>
      <Identifier>Input</Identifier>
      <DisplayName>Input</DisplayName>
      <Description>Input</Description>
      <DataType><Basic>Integer</Basic></DataType>
    </Parameter>
    <Response>
      <Identifier>Output</Identifier>
      <DisplayName>Output</DisplayName>
      <Description>Output</Description>
      <DataType>
        <List><DataType><Basic>Real</Basic></DataType></List>
      </DataType>
    </Response>
    <IntermediateResponse>
      <Identifier>Progress</Identifier>
      <DisplayName>Progress</DisplayName>
      <Description>Progress</Description>
      <DataType><Basic>Real</Basic></DataType>
    </IntermediateResponse>
    <DefinedExecutionErrors>
      <Identifier>TaskFailed</Identifier>
    </DefinedExecutionErrors>
  </Command>
  <DefinedExecutionError>
    <Identifier>TaskFailed</Identifier>
    <DisplayName>Task failed</DisplayName>
    <Description>Task failed</Description>
  </DefinedExecutionError>
</Feature>
)xml";

    const Feature feature = parseFdl(kFdl);

    ASSERT_EQ(feature.commands.size(), 1u);
    const auto& command = feature.commands[0];
    EXPECT_EQ(command.identifier, "RunTask");
    EXPECT_TRUE(command.observable);
    ASSERT_EQ(command.parameters.size(), 1u);
    EXPECT_EQ(std::get<DataType::Basic>(command.parameters[0].dataType.value).type,
              BasicType::Integer);
    ASSERT_EQ(command.responses.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<DataType::List>(command.responses[0].dataType.value));
    ASSERT_EQ(command.intermediateResponses.size(), 1u);
    ASSERT_EQ(command.definedExecutionErrors.size(), 1u);
    EXPECT_EQ(command.definedExecutionErrors[0], "TaskFailed");
}

TEST(FdlParserFuzzRegression, DataTypeDefinitionWithStructureConstraintsAndIdentifierRefParses) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.test" Category="test">
  <Identifier>TestFeature</Identifier>
  <DisplayName>Test feature</DisplayName>
  <Description>Test feature</Description>
  <DataTypeDefinition>
    <Identifier>Sample</Identifier>
    <DisplayName>Sample</DisplayName>
    <Description>Sample</Description>
    <DataType>
      <Structure>
        <Element>
          <Identifier>Bounded</Identifier>
          <DisplayName>Bounded</DisplayName>
          <Description>Bounded</Description>
          <DataType>
            <Constrained>
              <DataType><Basic>Integer</Basic></DataType>
              <Constraints>
                <MinimalInclusive>0</MinimalInclusive>
                <MaximalInclusive>100</MaximalInclusive>
                <Set>
                  <Value>1</Value>
                  <Value>2</Value>
                </Set>
              </Constraints>
            </Constrained>
          </DataType>
        </Element>
        <Element>
          <Identifier>Ref</Identifier>
          <DisplayName>Reference</DisplayName>
          <Description>Reference</Description>
          <DataType><DataTypeIdentifier>Sample</DataTypeIdentifier></DataType>
        </Element>
      </Structure>
    </DataType>
  </DataTypeDefinition>
</Feature>
)xml";

    const Feature feature = parseFdl(kFdl);

    ASSERT_EQ(feature.dataTypeDefinitions.size(), 1u);
    const auto& structure =
        std::get<DataType::Structure>(feature.dataTypeDefinitions[0].dataType.value);
    ASSERT_EQ(structure.elements.size(), 2u);

    const auto& constrained =
        std::get<DataType::Constrained>(structure.elements[0].dataType->value);
    EXPECT_EQ(constrained.constraints.size(), 3u);

    const auto& ref = std::get<DataType::Identifier>(structure.elements[1].dataType->value);
    EXPECT_EQ(ref.typeId, "Sample");
}

TEST(FdlParserFuzzRegression, ThreeNodeDataTypeReferenceCycleThrowsSemanticError) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.test" Category="test">
  <Identifier>ThreeNodeCycle</Identifier>
  <DisplayName>Three node cycle</DisplayName>
  <Description>Three node cycle</Description>
  <DataTypeDefinition>
    <Identifier>A</Identifier>
    <DisplayName>A</DisplayName>
    <Description>A</Description>
    <DataType><DataTypeIdentifier>B</DataTypeIdentifier></DataType>
  </DataTypeDefinition>
  <DataTypeDefinition>
    <Identifier>B</Identifier>
    <DisplayName>B</DisplayName>
    <Description>B</Description>
    <DataType><DataTypeIdentifier>C</DataTypeIdentifier></DataType>
  </DataTypeDefinition>
  <DataTypeDefinition>
    <Identifier>C</Identifier>
    <DisplayName>C</DisplayName>
    <Description>C</Description>
    <DataType><DataTypeIdentifier>A</DataTypeIdentifier></DataType>
  </DataTypeDefinition>
</Feature>
)xml";

    try {
        (void)parseFdl(kFdl);
        FAIL() << "expected a semantic cycle validation failure";
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("FDL semantic validation failed"), std::string::npos);
        EXPECT_NE(message.find("cyclic DataTypeDefinition reference"), std::string::npos);
    } catch (const std::exception& error) {
        FAIL() << "unexpected exception: " << error.what();
    }
}

// --- False (negative) paths — all CAUGHT: parseFdl throws std::invalid_argument ---

TEST(FdlParserFuzzRegression, EmptyInputThrows) {
    EXPECT_THROW(parseFdl(""), std::invalid_argument);
}

TEST(FdlParserFuzzRegression, RandomGarbageBytesThrows) {
    const std::string garbage = "\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE\x7F\x1B";
    EXPECT_THROW(parseFdl(garbage), std::invalid_argument);
}

TEST(FdlParserFuzzRegression, ValidXmlWithoutFeatureRootThrows) {
    EXPECT_THROW(parseFdl("<NotFeature/>"), std::invalid_argument);
}

TEST(FdlParserFuzzRegression, FeatureWithoutIdentifierThrows) {
    constexpr char kFdl[] = R"xml(
<Feature xmlns="http://www.sila-standard.org" SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.test">
  <DisplayName>Invalid feature</DisplayName>
  <Description>Invalid feature</Description>
</Feature>
)xml";
    EXPECT_THROW(parseFdl(kFdl), std::invalid_argument);
}

TEST(FdlParserFuzzRegression, TruncatedXmlThrows) {
    constexpr char kFdl[] = R"xml(<Feature FeatureVersion="1.0" Originator="org.te)xml";
    EXPECT_THROW(parseFdl(kFdl), std::invalid_argument);
}

TEST(FdlParserFuzzRegression, DataTypeNestingBeyondMaxDepthThrows) {
    const std::string deeplyNested = BuildDeeplyNestedListXml(70);
    EXPECT_THROW(parseFdl(deeplyNested), std::invalid_argument);
}

TEST(FdlParserFuzzRegression, EmbeddedNullByteThrows) {
    const std::string withNull = BuildNullByteXml();
    EXPECT_THROW(parseFdl(withNull), std::invalid_argument);
}
