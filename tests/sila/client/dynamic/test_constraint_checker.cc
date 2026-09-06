// Integration tests for checkFqiConstraint() (architecture.md §4.2): the
// FullyQualifiedIdentifier grammar and kind routing the runtime value
// validator delegates to ConstraintChecker.cc. The other constraint kinds
// are validated by ValueValidator's own engine and covered in
// test_value_validator.cc.
#include <sila/client/dynamic/ConstraintChecker.h>

#include <sila/client/dynamic/FdlIR.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {
using sila2::dynamic::checkFqiConstraint;
using sila2::dynamic::ConstraintValue;
using sila2::dynamic::DataType;
using sila2::dynamic::Feature;
using sila2::dynamic::parseFdl;

#ifndef SILA2_SOURCE_ROOT
#error "SILA2_SOURCE_ROOT must be supplied by tests/CMakeLists.txt"
#endif

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"unable to open FDL fixture: " + path.string()};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

const DataType* commandParameter(const Feature& feature, std::string_view command,
                                 std::string_view parameter) {
    for (const auto& item : feature.commands) {
        if (item.identifier != command) continue;
        for (const auto& candidate : item.parameters) {
            if (candidate.identifier == parameter) return &candidate.dataType;
        }
    }
    return nullptr;
}

ConstraintValue fqiKind(ConstraintValue::FqiKind kind) {
    ConstraintValue cv;
    cv.kind = ConstraintValue::FullyQualifiedIdentifier;
    cv.fqiKind = kind;
    return cv;
}

ConstraintValue sizeText(std::string lexical) {
    ConstraintValue cv;
    cv.lexicalValue = std::move(lexical);
    return cv;
}

TEST(ConstraintCheckerSize, AcceptsTheSignedXsdIntegerLexicalForms) {
    // XML Schema Part 2 3.3.13: an xs:integer literal may carry a leading sign,
    // so the bounds Constraints.xsd declares (xs:nonNegativeInteger for Length,
    // MinimalLength and the ElementCount trio, xs:positiveInteger for MaximalLength) all
    // admit "+N", and the nonNegative ones also admit "-0" for the value zero.
    // std::from_chars' unsigned overload rejects every sign on its own, so a
    // schema-valid FDL used to fail every runtime value validation (SC28 Codex).
    EXPECT_EQ(parseSizeConstraint(sizeText("0")), std::optional<std::size_t>{0});
    EXPECT_EQ(parseSizeConstraint(sizeText("+0")), std::optional<std::size_t>{0});
    EXPECT_EQ(parseSizeConstraint(sizeText("-0")), std::optional<std::size_t>{0});
    EXPECT_EQ(parseSizeConstraint(sizeText("-000")), std::optional<std::size_t>{0});
    EXPECT_EQ(parseSizeConstraint(sizeText("+7")), std::optional<std::size_t>{7});
    EXPECT_EQ(parseSizeConstraint(sizeText("007")), std::optional<std::size_t>{7});
}

TEST(ConstraintCheckerSize, IgnoresThePaddingTheCollapseFacetWouldRemove) {
    // These types also fix whiteSpace to "collapse" (XML Schema Part 2 4.3.6),
    // so "<ElementCount> 0 </ElementCount>" is schema-valid with the value 0.
    // FdlRuntimeParser stores element text verbatim, so the padding reaches
    // this parser and must not turn a valid bound into a violation (SC28 Codex).
    EXPECT_EQ(parseSizeConstraint(sizeText(" 0 ")), std::optional<std::size_t>{0});
    EXPECT_EQ(parseSizeConstraint(sizeText("\n  42\n")), std::optional<std::size_t>{42});
    EXPECT_EQ(parseSizeConstraint(sizeText("\t+7\r\n")), std::optional<std::size_t>{7});
    EXPECT_EQ(parseSizeConstraint(sizeText("  -0  ")), std::optional<std::size_t>{0});
    // Whitespace inside the numeral is not padding; collapse would leave it.
    EXPECT_EQ(parseSizeConstraint(sizeText("4 2")), std::nullopt);
    EXPECT_EQ(parseSizeConstraint(sizeText("+ 7")), std::nullopt);
    EXPECT_EQ(parseSizeConstraint(sizeText("   ")), std::nullopt);
}

TEST(ConstraintCheckerSize, RejectsNegativeMagnitudesAndMalformedText) {
    // A real negative is out of a size_t bound's range, and a bare or repeated
    // sign is not an integer literal at all.
    EXPECT_EQ(parseSizeConstraint(sizeText("-1")), std::nullopt);
    EXPECT_EQ(parseSizeConstraint(sizeText("-0001")), std::nullopt);
    EXPECT_EQ(parseSizeConstraint(sizeText("+")), std::nullopt);
    EXPECT_EQ(parseSizeConstraint(sizeText("-")), std::nullopt);
    EXPECT_EQ(parseSizeConstraint(sizeText("++0")), std::nullopt);
    EXPECT_EQ(parseSizeConstraint(sizeText("+1.0")), std::nullopt);
}

TEST(ConstraintCheckerFqi, ViolationReturnsError) {
    const auto constraint = fqiKind(ConstraintValue::FqiKind::FeatureIdentifier);

    auto error = checkFqiConstraint(constraint, "not-an-fqi");

    ASSERT_TRUE(error.has_value());
}

TEST(ConstraintCheckerFqi, UnknownKindReturnsError) {
    const auto constraint = fqiKind(ConstraintValue::FqiKind::Unknown);

    EXPECT_NE(checkFqiConstraint(constraint, "org.example/category/Name/v1"), std::nullopt);
}

TEST(ConstraintCheckerFqi, ComponentLengthBoundary) {
    const auto constraint = fqiKind(ConstraintValue::FqiKind::FeatureIdentifier);

    // DataTypes.xsd IdentifierType and FeatureDefinition.xsd Originator both
    // cap components at 255 characters; 256 must be rejected.
    const std::string feature255 = "A" + std::string(254, 'a');
    EXPECT_EQ(checkFqiConstraint(constraint,
                                 std::string(255, 'o') + "/category/" + feature255 + "/v1"),
              std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint,
                                 "org.example/category/A" + std::string(255, 'a') + "/v1"),
              std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint, std::string(256, 'o') + "/category/Name/v1"),
              std::nullopt);
}

TEST(ConstraintCheckerFqi, SlashFloodReturnsError) {
    const auto constraint = fqiKind(ConstraintValue::FqiKind::CommandParameterIdentifier);

    // splitFqi gives up after the longest valid shape (8 components) instead
    // of allocating one entry per slash.
    EXPECT_NE(checkFqiConstraint(constraint, std::string(100000, '/')), std::nullopt);
}

TEST(ConstraintCheckerFqi, ChecksAllKindsFromRealFdl) {
    const auto feature = parseFdl(readFile(
        std::filesystem::path{SILA2_SOURCE_ROOT} / "third_party" / "sila_base" /
        "feature_definitions" / "org" / "silastandard" / "test" /
        "ParameterConstraintsTest-v1_0.sila.xml"));
    const std::string base = feature.originator + "/" + feature.category + "/" +
                             feature.identifier + "/v" +
                             feature.featureVersion.substr(0, feature.featureVersion.find('.'));

    struct FqiCase {
        ConstraintValue::FqiKind kind;
        std::string_view parameter;
        std::string_view validSuffix;
        std::string_view invalid;
    };
    constexpr FqiCase cases[]{
        {ConstraintValue::FqiKind::FeatureIdentifier,
         "FeatureIdentifier", "", "/Command/CheckStringConstraintFullyQualifiedIdentifier"},
        {ConstraintValue::FqiKind::CommandIdentifier,
         "CommandIdentifier", "/Command/CheckStringConstraintFullyQualifiedIdentifier", ""},
        {ConstraintValue::FqiKind::CommandParameterIdentifier,
         "CommandParameterIdentifier",
         "/Command/CheckStringConstraintFullyQualifiedIdentifier/Parameter/FeatureIdentifier",
         "/Command/CheckStringConstraintFullyQualifiedIdentifier/Response/FeatureIdentifier"},
        {ConstraintValue::FqiKind::CommandResponseIdentifier,
         "CommandResponseIdentifier",
         "/Command/CheckStringConstraintFullyQualifiedIdentifier/Response/Result",
         "/Command/CheckStringConstraintFullyQualifiedIdentifier/Parameter/Result"},
        {ConstraintValue::FqiKind::IntermediateCommandResponseIdentifier,
         "IntermediateCommandResponseIdentifier",
         "/Command/CheckStringConstraintFullyQualifiedIdentifier/IntermediateResponse/Result",
         "/Command/CheckStringConstraintFullyQualifiedIdentifier/IntermediateCommandResponse/Result"},
        {ConstraintValue::FqiKind::DefinedExecutionErrorIdentifier,
         "ExecutionErrorIdentifier", "/DefinedExecutionError/SomeError", "/ExecutionError/SomeError"},
        {ConstraintValue::FqiKind::PropertyIdentifier,
         "PropertyIdentifier", "/Property/SomeProperty", "/Command/SomeProperty"},
        {ConstraintValue::FqiKind::TypeIdentifier,
         "CustomDataTypeIdentifier", "/DataType/SomeType", "/Type/SomeType"},
        {ConstraintValue::FqiKind::MetadataIdentifier,
         "MetadataIdentifier", "/Metadata/SomeMetadata", "/Property/SomeMetadata"},
    };

    for (const auto& testCase : cases) {
        const auto* type = commandParameter(
            feature, "CheckStringConstraintFullyQualifiedIdentifier", testCase.parameter);
        ASSERT_NE(type, nullptr) << testCase.parameter;
        const auto* constrained = std::get_if<DataType::Constrained>(&type->value);
        ASSERT_NE(constrained, nullptr) << testCase.parameter;
        ASSERT_EQ(constrained->constraints.size(), 1u) << testCase.parameter;
        // The FDL text -> FqiKind mapping (FdlRuntimeParser.cc parseFqiKind) is
        // only pinned here; Constraints.xsd enumerates exactly these nine.
        ASSERT_EQ(constrained->constraints[0].fqiKind, testCase.kind) << testCase.parameter;

        // std::string has no operator+(string_view): std::string(...) makes the
        // concatenation build (base + testCase.validSuffix alone does not compile).
        EXPECT_EQ(checkFqiConstraint(constrained->constraints[0],
                                     base + std::string(testCase.validSuffix)),
                  std::nullopt)
            << testCase.parameter;
        EXPECT_NE(checkFqiConstraint(constrained->constraints[0],
                                     base + std::string(testCase.invalid)),
                  std::nullopt)
            << testCase.parameter;
    }
}

// Part A p87: FQI comparison is case-insensitive. Every variant below folds
// to the same canonical CommandIdentifier
// "org.silastandard/instruments/LabwareTransferManipulatorController/v1/Command/PutLabware".
TEST(ConstraintCheckerFqi, AcceptsCaseVariants) {
    const auto constraint = fqiKind(ConstraintValue::FqiKind::CommandIdentifier);

    EXPECT_EQ(checkFqiConstraint(constraint,
                                 "ORG.SILASTANDARD/INSTRUMENTS/LabwareTransferManipulatorController/"
                                 "v1/Command/PutLabware"),
              std::nullopt);
    EXPECT_EQ(checkFqiConstraint(constraint,
                                 "org.silastandard/instruments/LabwareTransferManipulatorController/"
                                 "V1/Command/PutLabware"),
              std::nullopt);
    EXPECT_EQ(checkFqiConstraint(constraint,
                                 "org.silastandard/instruments/LabwareTransferManipulatorController/"
                                 "v1/command/PutLabware"),
              std::nullopt);
    // Lower-first Feature identifier -- structurally an Identifier under
    // Part B p88, but Part A p87 folds letter case, so it is accepted too.
    EXPECT_EQ(checkFqiConstraint(constraint,
                                 "org.silastandard/instruments/labwareTransferManipulatorController/"
                                 "v1/Command/PutLabware"),
              std::nullopt);
}

// Folding case must not relax the grammar's structure -- only letter case is
// ignored (Part A p87), everything else about the shape stays strict.
TEST(ConstraintCheckerFqi, RejectsFormatViolations) {
    const auto constraint = fqiKind(ConstraintValue::FqiKind::CommandIdentifier);
    constexpr std::string_view base =
        "org.silastandard/instruments/LabwareTransferManipulatorController";

    EXPECT_NE(checkFqiConstraint(constraint, std::string(base) + "/v1.0/Command/PutLabware"),
              std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint, std::string(base) + "/v1/Command/PutLabware/"),
              std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint, std::string(base) + "/v1/Command/Put_Labware"),
              std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint, std::string(base) + "/v1/Command/Put Labware"),
              std::nullopt);
    // 4-segment value: a valid FeatureIdentifier, but the wrong kind here.
    EXPECT_NE(checkFqiConstraint(constraint, std::string(base) + "/v1"), std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint, "PutLabware"), std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint, std::string(base) + "/v1/Command/0Labware"),
              std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint,
                                 "org-silastandard/instruments/"
                                 "LabwareTransferManipulatorController/v1/Command/PutLabware"),
              std::nullopt);
    EXPECT_NE(checkFqiConstraint(constraint, " " + std::string(base) + "/v1/Command/PutLabware"),
              std::nullopt);
}

}  // namespace
