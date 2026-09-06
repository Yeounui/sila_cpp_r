// Runtime FDL conformance corpus.
//
// The codegen tests already exercise the Python parser against the same
// corpus.  This test is deliberately a separate C++ gate: every fixture must
// have the standard corpus classification and the runtime parseFdl() result
// must agree with it.
#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
using sila2::dynamic::parseFdl;
using sila2::dynamic::ConstraintValue;
using sila2::dynamic::DataType;
using sila2::dynamic::Feature;

#ifndef SILA2_SOURCE_ROOT
#error "SILA2_SOURCE_ROOT must be supplied by tests/CMakeLists.txt"
#endif

struct CorpusFile {
    fs::path absolutePath;
    std::string relativePath;
    bool expectedValid;
    enum class Kind { OfficialValid, OfficialInvalid, RealWorld } kind;
};

fs::path corpusRoot() {
    return fs::path{SILA2_SOURCE_ROOT} / "tests" / "examples" / "fdl";
}

std::string readFile(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"Unable to open FDL fixture: " + path.string()};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::vector<CorpusFile> corpusFiles() {
    const fs::path root = corpusRoot();
    if (!fs::is_directory(root)) {
        throw std::runtime_error{"FDL corpus directory does not exist: " + root.string()};
    }

    std::vector<CorpusFile> files;
    for (const auto& entry : fs::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".xml") {
            continue;
        }

        const std::string relative = fs::relative(entry.path(), root).generic_string();
        const bool officialValid = relative.rfind("sila_base/valid-fdl/", 0) == 0;
        const bool officialInvalid = relative.rfind("sila_base/invalid-fdl/", 0) == 0;
        if (officialValid && officialInvalid) {
            throw std::runtime_error{"Fixture has ambiguous corpus classification: " + relative};
        }

        CorpusFile::Kind kind = CorpusFile::Kind::RealWorld;
        bool expectedValid = true;
        if (officialValid) {
            kind = CorpusFile::Kind::OfficialValid;
        } else if (officialInvalid) {
            kind = CorpusFile::Kind::OfficialInvalid;
            expectedValid = false;
        }
        files.push_back(CorpusFile{entry.path(), relative, expectedValid, kind});
    }

    std::sort(files.begin(), files.end(), [](const CorpusFile& lhs, const CorpusFile& rhs) {
        return lhs.relativePath < rhs.relativePath;
    });
    return files;
}

std::string parseError(const fs::path& path) {
    try {
        (void)parseFdl(readFile(path));
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "non-standard exception";
    }
    return {};
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool containsAnyCaseInsensitive(const std::string& text,
                                std::initializer_list<const char*> needles) {
    const std::string lower = lowerAscii(text);
    for (const char* needle : needles) {
        if (lower.find(lowerAscii(needle)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void collectConstraints(const DataType& dataType,
                        std::vector<const ConstraintValue*>& result) {
    std::visit(
        [&result](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, DataType::Constrained>) {
                for (const auto& constraint : value.constraints) {
                    result.push_back(&constraint);
                }
                if (value.inner != nullptr) collectConstraints(*value.inner, result);
            } else if constexpr (std::is_same_v<Value, DataType::List>) {
                if (value.elementType != nullptr) collectConstraints(*value.elementType, result);
            } else if constexpr (std::is_same_v<Value, DataType::Structure>) {
                for (const auto& element : value.elements) {
                    if (element.dataType != nullptr) {
                        collectConstraints(*element.dataType, result);
                    }
                }
            }
        },
        dataType.value);
}

void collectConstraints(const Feature& feature,
                        std::vector<const ConstraintValue*>& result) {
    for (const auto& command : feature.commands) {
        for (const auto& parameter : command.parameters) {
            collectConstraints(parameter.dataType, result);
        }
        for (const auto& response : command.responses) {
            collectConstraints(response.dataType, result);
        }
        for (const auto& response : command.intermediateResponses) {
            collectConstraints(response.dataType, result);
        }
    }
    for (const auto& property : feature.properties) {
        collectConstraints(property.dataType, result);
    }
    for (const auto& metadata : feature.metadata) {
        collectConstraints(metadata.dataType, result);
    }
    for (const auto& definition : feature.dataTypeDefinitions) {
        collectConstraints(definition.dataType, result);
    }
}

const ConstraintValue* findConstraint(const Feature& feature, ConstraintValue::Kind kind) {
    std::vector<const ConstraintValue*> constraints;
    collectConstraints(feature, constraints);
    for (const auto* constraint : constraints) {
        if (constraint->kind == kind) return constraint;
    }
    return nullptr;
}

template <typename Predicate>
const ConstraintValue* findConstraintIf(const Feature& feature, ConstraintValue::Kind kind,
                                        Predicate predicate) {
    std::vector<const ConstraintValue*> constraints;
    collectConstraints(feature, constraints);
    for (const auto* constraint : constraints) {
        if (constraint->kind == kind && predicate(*constraint)) return constraint;
    }
    return nullptr;
}

std::string replaceOnce(std::string text, std::string_view needle, std::string_view replacement) {
    const auto position = text.find(needle);
    if (position == std::string::npos) {
        throw std::runtime_error{"fixture mutation target was not found"};
    }
    text.replace(position, needle.size(), replacement);
    return text;
}

}  // namespace

TEST(FdlRuntimeConformance, CorpusInventoryAndParseResultsMatchStandardClassification) {
    const std::vector<CorpusFile> files = corpusFiles();

    std::size_t officialValidCount = 0;
    std::size_t officialInvalidCount = 0;
    std::size_t realWorldCount = 0;
    for (const CorpusFile& file : files) {
        switch (file.kind) {
        case CorpusFile::Kind::OfficialValid:
            ++officialValidCount;
            break;
        case CorpusFile::Kind::OfficialInvalid:
            ++officialInvalidCount;
            break;
        case CorpusFile::Kind::RealWorld:
            ++realWorldCount;
            break;
        }

        SCOPED_TRACE(file.relativePath);
        bool accepted = false;
        std::string error;
        try {
            const auto feature = parseFdl(readFile(file.absolutePath));
            accepted = true;
            if (file.expectedValid) {
                EXPECT_FALSE(feature.identifier.empty());
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "non-standard exception";
        }

        if (file.expectedValid) {
            EXPECT_TRUE(accepted) << "parseFdl rejected a fixture expected to be valid: " << error;
        } else {
            EXPECT_FALSE(accepted) << "parseFdl accepted an official invalid fixture";
            EXPECT_FALSE(error.empty()) << "parseFdl produced no rejection diagnostic";
        }
    }

    EXPECT_EQ(officialValidCount, 22u);
    EXPECT_EQ(officialInvalidCount, 31u);
    EXPECT_EQ(realWorldCount, 54u);
    EXPECT_EQ(files.size(), 107u);
}

TEST(FdlRuntimeConformance, XsdFailureIncludesStageDiagnostic) {
    const fs::path fixture = corpusRoot() / "sila_base" / "invalid-fdl" /
                             "EmptyDataType.sila.xml";
    const std::string error = parseError(fixture);

    ASSERT_FALSE(error.empty()) << "parseFdl accepted the XSD-invalid fixture";
    EXPECT_TRUE(containsAnyCaseInsensitive(error, {"xsd", "schema"}))
        << "expected an XSD/schema stage diagnostic, got: " << error;
}

TEST(FdlRuntimeConformance, NormativeXsltFailureIncludesStageDiagnostic) {
    const fs::path fixture = corpusRoot() / "sila_base" / "invalid-fdl" /
                             "NestedList.sila.xml";
    const std::string error = parseError(fixture);

    ASSERT_FALSE(error.empty()) << "parseFdl accepted the XSLT-invalid fixture";
    EXPECT_TRUE(containsAnyCaseInsensitive(error, {"xslt", "normative"}))
        << "expected a normative XSLT stage diagnostic, got: " << error;
}

TEST(FdlRuntimeConformance, CorpusConstraintsArePreservedLosslessly) {
    const auto expectScalar = [](const Feature& feature, ConstraintValue::Kind kind,
                                 std::string_view lexical, double numeric) {
        const auto* constraint = findConstraintIf(
            feature, kind, [lexical](const auto& value) { return value.lexicalValue == lexical; });
        ASSERT_NE(constraint, nullptr);
        EXPECT_EQ(constraint->lexicalValue, lexical);
        EXPECT_EQ(constraint->stringValue, lexical);
        EXPECT_DOUBLE_EQ(constraint->numericValue, numeric);
    };

    const auto absorbance = parseFdl(readFile(corpusRoot() / "sila_base" /
                                              "AbsorbanceReaderService-v1_0.sila.xml"));
    expectScalar(absorbance, ConstraintValue::Length, "1", 1);
    expectScalar(absorbance, ConstraintValue::Pattern, "[A-Z]", 0);
    expectScalar(absorbance, ConstraintValue::MinInclusive, "0.000", 0);
    expectScalar(absorbance, ConstraintValue::MaxInclusive, "5.000", 5);

    const auto* unit = findConstraint(absorbance, ConstraintValue::Unit);
    ASSERT_NE(unit, nullptr);
    ASSERT_TRUE(unit->unit.has_value());
    EXPECT_EQ(unit->unit->label, "nanometer");
    EXPECT_EQ(unit->unit->factor, "0.000000001");
    EXPECT_EQ(unit->unit->offset, "0");
    ASSERT_EQ(unit->unit->components.size(), 1u);
    EXPECT_EQ(unit->unit->components[0].siUnit, "Meter");
    EXPECT_EQ(unit->unit->components[0].exponent, "1");

    const auto date = parseFdl(readFile(corpusRoot() / "sila_base" / "valid-fdl" /
                                        "DateSet.sila.xml"));
    const auto* dateSet = findConstraint(date, ConstraintValue::Set);
    ASSERT_NE(dateSet, nullptr);
    ASSERT_GE(dateSet->stringValues.size(), 2u);
    EXPECT_EQ(dateSet->stringValues[0], "2020-01-01Z");
    EXPECT_EQ(dateSet->stringValues[1], "2020-02-29-00:00");

    const auto panda = parseFdl(readFile(corpusRoot() / "panda" / "RobotController.sila.xml"));
    expectScalar(panda, ConstraintValue::ElementCount, "7", 7);
    expectScalar(panda, ConstraintValue::MinElementCount, "1", 1);

    const auto siteManager = parseFdl(readFile(corpusRoot() / "panda" / "SiteManager.sila.xml"));
    expectScalar(siteManager, ConstraintValue::MaxLength, "255", 255);
    expectScalar(siteManager, ConstraintValue::MaxElementCount, "1", 1);

    const auto axis = parseFdl(readFile(corpusRoot() / "cetoni" /
                                        "AxisSystemPositionController.sila.xml"));
    expectScalar(axis, ConstraintValue::MinExclusive, "0", 0);

    const auto maxExclusive = parseFdl(readFile(corpusRoot() / "sila_base" / "valid-fdl" /
                                                "Constrained.sila.xml"));
    expectScalar(maxExclusive, ConstraintValue::MaxExclusive, "-1e+50", -1e50);

    const auto fqi = parseFdl(readFile(corpusRoot() / "sila_base" /
                                       "GatewayService-v1_0.sila.xml"));
    const auto* fqiConstraint = findConstraintIf(
        fqi, ConstraintValue::FullyQualifiedIdentifier, [](const auto& constraint) {
            return constraint.lexicalValue == "FeatureIdentifier";
        });
    ASSERT_NE(fqiConstraint, nullptr);
    EXPECT_EQ(fqiConstraint->lexicalValue, "FeatureIdentifier");
    EXPECT_EQ(fqiConstraint->stringValue, "FeatureIdentifier");
    EXPECT_EQ(fqiConstraint->fqiKind, ConstraintValue::FqiKind::FeatureIdentifier);

    auto minLengthXml = readFile(corpusRoot() / "sila_base" /
                                 "AbsorbanceReaderService-v1_0.sila.xml");
    minLengthXml = replaceOnce(std::move(minLengthXml), "<Length>1</Length>",
                               "<MinimalLength>1</MinimalLength>");
    const auto minLength = parseFdl(minLengthXml);
    expectScalar(minLength, ConstraintValue::MinLength, "1", 1);

    // The corpus has no valid Schema or AllowedTypes example. Start with real
    // fixtures and make the smallest standard-valid substitutions in memory.
    auto extendedXml = readFile(corpusRoot() / "ot2" / "Ot2Controller.sila.xml");
    extendedXml = replaceOnce(
        std::move(extendedXml),
        "<ContentType>\n                  <Type>image</Type>\n                  <Subtype>jpeg</Subtype>\n                </ContentType>",
        "<ContentType>\n                  <Type>image</Type>\n                  <Subtype>jpeg</Subtype>\n                  <Parameters>\n                    <Parameter>\n                      <Attribute>charset</Attribute>\n                      <Value>utf-8</Value>\n                    </Parameter>\n                  </Parameters>\n                </ContentType>\n                <Schema>\n                  <Type>Json</Type>\n                  <Inline>{&quot;type&quot;:&quot;object&quot;}</Inline>\n                </Schema>");
    extendedXml = replaceOnce(
        std::move(extendedXml),
        "<ContentType>\n              <Type>video</Type>\n              <Subtype>mp4</Subtype>\n            </ContentType>",
        "<ContentType>\n              <Type>video</Type>\n              <Subtype>mp4</Subtype>\n            </ContentType>\n            <Schema>\n              <Type>Xml</Type>\n              <Url>https://gitlab.com/SiLA2/sila_base/-/raw/master/schema/FeatureDefinition.xsd</Url>\n            </Schema>");
    const auto extendedFeature = parseFdl(extendedXml);

    const auto* content = findConstraintIf(
        extendedFeature, ConstraintValue::ContentType, [](const auto& constraint) {
            return constraint.contentType.has_value() && constraint.contentType->type == "image";
        });
    ASSERT_NE(content, nullptr);
    ASSERT_TRUE(content->contentType.has_value());
    EXPECT_EQ(content->contentType->type, "image");
    EXPECT_EQ(content->contentType->subtype, "jpeg");
    ASSERT_EQ(content->contentType->parameters.size(), 1u);
    EXPECT_EQ(content->contentType->parameters[0].attribute, "charset");
    EXPECT_EQ(content->contentType->parameters[0].value, "utf-8");

    const auto* inlineSchema = findConstraintIf(
        extendedFeature, ConstraintValue::Schema, [](const auto& constraint) {
            return constraint.schema.has_value() &&
                   constraint.schema->source == ConstraintValue::SchemaValue::Source::Inline;
        });
    ASSERT_NE(inlineSchema, nullptr);
    ASSERT_TRUE(inlineSchema->schema.has_value());
    EXPECT_EQ(inlineSchema->schema->type, ConstraintValue::SchemaValue::Type::Json);
    EXPECT_EQ(inlineSchema->schema->value, R"({"type":"object"})");

    const auto* urlSchema = findConstraintIf(
        extendedFeature, ConstraintValue::Schema, [](const auto& constraint) {
            return constraint.schema.has_value() &&
                   constraint.schema->source == ConstraintValue::SchemaValue::Source::Url;
        });
    ASSERT_NE(urlSchema, nullptr);
    ASSERT_TRUE(urlSchema->schema.has_value());
    EXPECT_EQ(urlSchema->schema->type, ConstraintValue::SchemaValue::Type::Xml);
    EXPECT_EQ(urlSchema->schema->value,
              "https://gitlab.com/SiLA2/sila_base/-/raw/master/schema/FeatureDefinition.xsd");

    auto allowedTypesXml = readFile(corpusRoot() / "sila_base" / "invalid-fdl" /
                                    "AllowedTypesWithDataTypeDefinition.sila.xml");
    allowedTypesXml = replaceOnce(std::move(allowedTypesXml),
                                  "<DataTypeIdentifier>Int</DataTypeIdentifier>",
                                  "<List>\n                            <DataType>\n                                <Basic>Integer</Basic>\n                            </DataType>\n                        </List>");
    const auto allowedTypesFeature = parseFdl(allowedTypesXml);
    const auto* allowedTypes = findConstraint(allowedTypesFeature, ConstraintValue::AllowedTypes);
    ASSERT_NE(allowedTypes, nullptr);
    ASSERT_EQ(allowedTypes->allowedTypes.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<DataType::Basic>(allowedTypes->allowedTypes[0]->value));
    EXPECT_EQ(std::get<DataType::Basic>(allowedTypes->allowedTypes[0]->value).type,
              sila2::dynamic::BasicType::Boolean);
    ASSERT_TRUE(std::holds_alternative<DataType::List>(allowedTypes->allowedTypes[1]->value));
    const auto& list = std::get<DataType::List>(allowedTypes->allowedTypes[1]->value);
    ASSERT_NE(list.elementType, nullptr);
    ASSERT_TRUE(std::holds_alternative<DataType::Basic>(list.elementType->value));
    EXPECT_EQ(std::get<DataType::Basic>(list.elementType->value).type,
              sila2::dynamic::BasicType::Integer);
}

// SiLA 2 Part B p84-85 (R2-9): Conversion Factor/Offset are IEEE 754 doubles.
// The pinned sila_base Constraints.xsd types them xs:decimal, which rejects
// exponent notation and INF/NaN; the in-repo overlay (src/schema/Constraints.xsd,
// embedded as kConstraintsXsd via src/sila/CMakeLists.txt) retypes them
// xs:double and would have thrown here under the old xs:decimal schema.
// libxml2 is XSD 1.0, so -INF (not +INF) is the valid special-value literal.
TEST(FdlRuntimeConformance, UnitFactorAcceptsIeee754DoubleExponent) {
    constexpr char kExponentUnitFdl[] = R"xml(<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>UnitFactor</Identifier>
    <DisplayName>Unit Factor</DisplayName>
    <Description>Feature with a Unit constraint exercising Factor/Offset</Description>
    <Property>
        <Identifier>Measurement</Identifier>
        <DisplayName>Measurement</DisplayName>
        <Description>A constrained real with a Unit</Description>
        <Observable>No</Observable>
        <DataType>
            <Constrained>
                <DataType>
                    <Basic>Real</Basic>
                </DataType>
                <Constraints>
                    <Unit>
                        <Label>testunit</Label>
                        <Factor>6.022e23</Factor>
                        <Offset>-INF</Offset>
                        <UnitComponent>
                            <SIUnit>Meter</SIUnit>
                            <Exponent>1</Exponent>
                        </UnitComponent>
                    </Unit>
                </Constraints>
            </Constrained>
        </DataType>
    </Property>
</Feature>
)xml";

    const Feature feature = parseFdl(kExponentUnitFdl);

    const ConstraintValue* unit = findConstraint(feature, ConstraintValue::Unit);
    ASSERT_NE(unit, nullptr);
    ASSERT_TRUE(unit->unit.has_value());
    EXPECT_EQ(unit->unit->factor, "6.022e23");
    EXPECT_EQ(unit->unit->offset, "-INF");
}
