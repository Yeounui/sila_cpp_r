#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>
#include <sila/client/dynamic/ValueValidator.h>

#include "AnyWireValue.h"
#include "SiLAFramework.pb.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace fw = sila2::org::silastandard;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using sila2::dynamic::BasicType;
using sila2::dynamic::ConstraintResolver;
using sila2::dynamic::ConstraintValue;
using sila2::dynamic::DataType;
using sila2::dynamic::DescriptorBuilder;
using sila2::dynamic::Feature;
using sila2::dynamic::ValueValidator;
using sila2::dynamic::parseFdl;

#ifndef SILA2_SOURCE_ROOT
#error "SILA2_SOURCE_ROOT must be supplied by tests/CMakeLists.txt"
#endif

fs::path fixture(std::string_view relative) {
    return fs::path{SILA2_SOURCE_ROOT} / "tests" / "examples" / "fdl" / relative;
}

std::string readFile(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"unable to open FDL fixture: " + path.string()};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

DataType* propertyType(Feature& feature, std::string_view identifier) {
    for (auto& property : feature.properties) {
        if (property.identifier == identifier) return &property.dataType;
    }
    return nullptr;
}

DataType* propertyElementType(Feature& feature, std::string_view property,
                              std::string_view element) {
    auto* type = propertyType(feature, property);
    if (type == nullptr) return nullptr;
    auto* structure = std::get_if<DataType::Structure>(&type->value);
    if (structure == nullptr) return nullptr;
    for (auto& item : structure->elements) {
        if (item.identifier == element) return item.dataType.get();
    }
    return nullptr;
}

DataType* commandParameter(Feature& feature, std::string_view command,
                           std::string_view parameter) {
    for (auto& item : feature.commands) {
        if (item.identifier != command) continue;
        for (auto& candidate : item.parameters) {
            if (candidate.identifier == parameter) return &candidate.dataType;
        }
    }
    return nullptr;
}

const ConstraintValue* findConstraint(const DataType& type, ConstraintValue::Kind kind) {
    const auto* constrained = std::get_if<DataType::Constrained>(&type.value);
    if (constrained == nullptr) return nullptr;
    for (const auto& constraint : constrained->constraints) {
        if (constraint.kind == kind) return &constraint;
    }
    return nullptr;
}

DataType constrained(std::unique_ptr<DataType> inner, ConstraintValue constraint) {
    DataType result;
    DataType::Constrained value;
    value.inner = std::move(inner);
    value.constraints.push_back(std::move(constraint));
    result.value = std::move(value);
    return result;
}

std::string packageName(const Feature& feature) {
    const auto major = feature.featureVersion.substr(0, feature.featureVersion.find('.'));
    std::string lower = feature.identifier;
    for (char& character : lower) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return "sila2." + feature.originator + "." + feature.category + "." + lower + ".v" + major;
}

std::unique_ptr<Message> commandRequest(const Feature& feature, std::string_view command,
                                         google::protobuf::DescriptorPool& pool,
                                         google::protobuf::DynamicMessageFactory& factory) {
    const auto fileProto = DescriptorBuilder{}.build(feature);
    if (pool.BuildFile(fileProto) == nullptr) {
        throw std::runtime_error{"unable to build test descriptor"};
    }
    const auto* descriptor = pool.FindMessageTypeByName(
        packageName(feature) + "." + std::string{command} + "_Parameters");
    if (descriptor == nullptr) throw std::runtime_error{"command request descriptor not found"};
    return std::unique_ptr<Message>{factory.GetPrototype(descriptor)->New()};
}

TEST(ValueValidatorExternalResolvers, DelegatesUnitFromAbsorbanceFixture) {
    auto feature = parseFdl(readFile(fixture("sila_base/AbsorbanceReaderService-v1_0.sila.xml")));
    auto* type = propertyType(feature, "MeasurementFilter");
    ASSERT_NE(type, nullptr);
    const auto* unit = findConstraint(*type, ConstraintValue::Unit);
    ASSERT_NE(unit, nullptr);
    ASSERT_TRUE(unit->unit.has_value());

    fw::Integer value;
    value.set_value(405);
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    bool exact = false;
    bool reject = false;
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message& message,
                                      const FieldDescriptor& actualField, int index)
        -> std::optional<std::string> {
        exact = &constraint == unit && constraint.kind == ConstraintValue::Unit &&
                constraint.unit.has_value() && constraint.unit->label == "nanometer" &&
                &message == &value && &actualField == field && index == -1 &&
                message.GetReflection()->GetInt64(message, &actualField) == 405;
        if (reject) return std::string{"unit rejected"};
        return std::nullopt;
    };

    EXPECT_EQ(ValueValidator::validate(*type, value, *field, {}, resolver), std::nullopt);
    EXPECT_TRUE(exact);
    reject = true;
    EXPECT_EQ(ValueValidator::validate(*type, value, *field, {}, resolver), "unit rejected");
}

TEST(ValueValidatorExternalResolvers, DelegatesContentTypeFromOt2Fixture) {
    auto feature = parseFdl(readFile(fixture("ot2/Ot2Controller.sila.xml")));
    auto* type = propertyElementType(feature, "CameraPicture", "ImageData");
    ASSERT_NE(type, nullptr);
    const auto* content = findConstraint(*type, ConstraintValue::ContentType);
    ASSERT_NE(content, nullptr);
    ASSERT_TRUE(content->contentType.has_value());

    fw::Binary value;
    value.set_value("jpeg-bytes");
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    bool exact = false;
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message& message,
                                      const FieldDescriptor& actualField, int index)
        -> std::optional<std::string> {
        exact = &constraint == content && constraint.kind == ConstraintValue::ContentType &&
                constraint.contentType.has_value() && constraint.contentType->type == "image" &&
                constraint.contentType->subtype == "jpeg" && &message == &value &&
                &actualField == field && index == -1 &&
                message.GetReflection()->GetString(message, &actualField) == "jpeg-bytes";
        return std::nullopt;
    };

    EXPECT_EQ(ValueValidator::validate(*type, value, *field, {}, resolver), std::nullopt);
    EXPECT_TRUE(exact);
}

TEST(ValueValidatorExternalResolvers, DelegatesSchemaAddedToParsedOt2Fixture) {
    auto feature = parseFdl(readFile(fixture("ot2/Ot2Controller.sila.xml")));
    auto* type = propertyElementType(feature, "CameraPicture", "ImageData");
    ASSERT_NE(type, nullptr);
    auto* constrainedType = std::get_if<DataType::Constrained>(&type->value);
    ASSERT_NE(constrainedType, nullptr);

    ConstraintValue schema;
    schema.kind = ConstraintValue::Schema;
    ConstraintValue::SchemaValue schemaValue;
    schemaValue.type = ConstraintValue::SchemaValue::Type::Json;
    schemaValue.source = ConstraintValue::SchemaValue::Source::Inline;
    schemaValue.value = R"({"type":"object"})";
    schema.schema = std::move(schemaValue);
    constrainedType->constraints.push_back(schema);

    fw::Binary value;
    value.set_value("json-payload");
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    bool exact = false;
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message& message,
                                      const FieldDescriptor& actualField, int index)
        -> std::optional<std::string> {
        if (constraint.kind != ConstraintValue::Schema) return std::nullopt;
        exact = constraint.schema.has_value() &&
                constraint.schema->type == ConstraintValue::SchemaValue::Type::Json &&
                constraint.schema->source == ConstraintValue::SchemaValue::Source::Inline &&
                constraint.schema->value == R"({"type":"object"})" && &message == &value &&
                &actualField == field && index == -1;
        return std::nullopt;
    };

    EXPECT_EQ(ValueValidator::validate(*type, value, *field, {}, resolver), std::nullopt);
    EXPECT_TRUE(exact);
}

TEST(ValueValidatorExternalResolvers, DelegatesAllowedTypesAddedToParsedAbsorbanceFixture) {
    // Flipped for R10-9b: the removed Constrained-Any shortcut used to let
    // this fixture pass the Any's scalar 'type' subfield straight to
    // validate() (no message field is ever dereferenced). With the shortcut
    // gone, the Basic-Any branch decodes a real wire Any, so this is rebuilt
    // as one (Boolean payload) while staying a delegation SUCCESS test:
    // AllowedTypes is still delegated and still accepted here (R10-9c does
    // the allowed-list match itself).
    auto feature = parseFdl(readFile(fixture("sila_base/AbsorbanceReaderService-v1_0.sila.xml")));
    auto* type = propertyType(feature, "MeasurementFilter");
    ASSERT_NE(type, nullptr);

    ConstraintValue allowed;
    allowed.kind = ConstraintValue::AllowedTypes;
    allowed.allowedTypes.push_back(
        std::make_shared<DataType>(DataType{DataType::Basic{BasicType::Boolean}}));
    *type = constrained(std::make_unique<DataType>(DataType{DataType::Basic{BasicType::Any}}),
                        allowed);

    auto wireValue = anytest::wireAny(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Boolean</Basic></DataType>",
        [](google::protobuf::Message& payloadWrapper,
           const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = payloadWrapper.GetReflection()->MutableMessage(&payloadWrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetBool(inner, valueField, true);
        });
    bool exact = false;
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message& message,
                                      const FieldDescriptor& actualField, int index)
        -> std::optional<std::string> {
        exact = constraint.kind == ConstraintValue::AllowedTypes &&
                constraint.allowedTypes.size() == 1u && &message == wireValue.host.get() &&
                &actualField == wireValue.anyField && index == -1;
        return std::nullopt;
    };

    EXPECT_EQ(ValueValidator::validate(*type, *wireValue.host, *wireValue.anyField, {}, resolver),
              std::nullopt);
    EXPECT_TRUE(exact);
    // AllowedTypes still fails closed with no resolver (unchanged).
    EXPECT_NE(ValueValidator::validate(*type, *wireValue.host, *wireValue.anyField), std::nullopt);
}

TEST(ValueValidatorExternalResolvers, RejectsNestedConstrainedAnyViolatingInnerConstraintDespiteResolver) {
    // Proves the removed Constrained-Any shortcut (ValueValidator.cc:1062-1067
    // before this batch) actually mattered: before removal, a resolver-backed
    // AllowedTypes on a Constrained-Any short-circuited straight to nullopt,
    // never checking the decoded value against its own embedded Constraints.
    // S = Constrained<Integer, MinimalInclusive 10>; the resolver accepts
    // AllowedTypes, so validation must now fail on the inner Integer bound.
    ConstraintValue allowed;
    allowed.kind = ConstraintValue::AllowedTypes;
    allowed.allowedTypes.push_back(
        std::make_shared<DataType>(DataType{DataType::Basic{BasicType::Integer}}));
    DataType type = constrained(std::make_unique<DataType>(DataType{DataType::Basic{BasicType::Any}}),
                                allowed);

    auto wireValue = anytest::wireAny(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained><DataType><Basic>Integer</Basic></DataType>"
        "<Constraints><MinimalInclusive>10</MinimalInclusive></Constraints>"
        "</Constrained></DataType>",
        [](google::protobuf::Message& payloadWrapper,
           const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = payloadWrapper.GetReflection()->MutableMessage(&payloadWrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetInt64(inner, valueField, 5);
        });
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message&,
                                      const FieldDescriptor&, int) -> std::optional<std::string> {
        if (constraint.kind != ConstraintValue::AllowedTypes) return std::nullopt;
        return std::nullopt;  // AllowedTypes accepts; the inner bound must still reject.
    };

    EXPECT_NE(ValueValidator::validate(type, *wireValue.host, *wireValue.anyField, {}, resolver),
              std::nullopt);
}

TEST(ValueValidatorExternalResolvers, DelegatesRepeatedElementWithItsIndex) {
    auto robot = parseFdl(readFile(fixture("panda/RobotController.sila.xml")));
    auto absorbance = parseFdl(readFile(fixture("sila_base/AbsorbanceReaderService-v1_0.sila.xml")));
    auto* unitType = propertyType(absorbance, "MeasurementFilter");
    ASSERT_NE(unitType, nullptr);
    const auto* unit = findConstraint(*unitType, ConstraintValue::Unit);
    ASSERT_NE(unit, nullptr);

    auto* effortType = commandParameter(robot, "SetArmEffort", "Effort");
    ASSERT_NE(effortType, nullptr);
    auto* outer = std::get_if<DataType::Constrained>(&effortType->value);
    ASSERT_NE(outer, nullptr);
    auto* list = std::get_if<DataType::List>(&outer->inner->value);
    ASSERT_NE(list, nullptr);
    ASSERT_NE(list->elementType, nullptr);
    list->elementType = std::make_unique<DataType>(
        constrained(std::move(list->elementType), *unit));

    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    auto request = commandRequest(robot, "SetArmEffort", pool, factory);
    const auto* field = request->GetDescriptor()->FindFieldByName("Effort");
    ASSERT_NE(field, nullptr);
    for (double item : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0}) {
        auto* element = request->GetReflection()->AddMessage(request.get(), field);
        const auto* valueField = element->GetDescriptor()->FindFieldByName("value");
        ASSERT_NE(valueField, nullptr);
        element->GetReflection()->SetDouble(element, valueField, item);
    }

    std::vector<int> indexes;
    std::vector<double> values;
    bool rejectSecond = false;
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message& message,
                                      const FieldDescriptor& actualField, int index)
        -> std::optional<std::string> {
        EXPECT_EQ(constraint.kind, ConstraintValue::Unit);
        EXPECT_EQ(&message, request.get());
        EXPECT_EQ(&actualField, field);
        indexes.push_back(index);
        const auto& element = message.GetReflection()->GetRepeatedMessage(
            message, &actualField, index);
        const auto* valueField = element.GetDescriptor()->FindFieldByName("value");
        EXPECT_NE(valueField, nullptr);
        if (valueField == nullptr) return std::string{"repeated value field missing"};
        values.push_back(element.GetReflection()->GetDouble(element, valueField));
        if (rejectSecond && index == 1) return std::string{"second element rejected"};
        return std::nullopt;
    };

    EXPECT_EQ(ValueValidator::validate(*effortType, *request, *field, {}, resolver), std::nullopt);
    EXPECT_EQ(indexes, (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(values, (std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0}));

    indexes.clear();
    values.clear();
    rejectSecond = true;
    EXPECT_EQ(ValueValidator::validate(*effortType, *request, *field, {}, resolver),
              "second element rejected");
    EXPECT_EQ(indexes, (std::vector<int>{0, 1}));
}

TEST(ValueValidatorExternalResolvers, MissingConstraintResolverFailsClosed) {
    auto feature = parseFdl(readFile(fixture("sila_base/AbsorbanceReaderService-v1_0.sila.xml")));
    const auto* type = propertyType(feature, "MeasurementFilter");
    ASSERT_NE(type, nullptr);
    fw::Integer value;
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    EXPECT_NE(ValueValidator::validate(*type, value, *field), std::nullopt);
}

TEST(ValueValidatorExternalResolvers, DelegatesContentTypeRejectFromOt2Fixture) {
    auto feature = parseFdl(readFile(fixture("ot2/Ot2Controller.sila.xml")));
    auto* type = propertyElementType(feature, "CameraPicture", "ImageData");
    ASSERT_NE(type, nullptr);
    const auto* content = findConstraint(*type, ConstraintValue::ContentType);
    ASSERT_NE(content, nullptr);
    ASSERT_TRUE(content->contentType.has_value());

    fw::Binary value;
    value.set_value("jpeg-bytes");
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message&,
                                      const FieldDescriptor&, int) -> std::optional<std::string> {
        if (constraint.kind != ConstraintValue::ContentType) return std::nullopt;
        return std::string{"content-type rejected"};
    };

    EXPECT_EQ(ValueValidator::validate(*type, value, *field, {}, resolver),
              "content-type rejected");
}

TEST(ValueValidatorExternalResolvers, ContentTypeMissingConstraintResolverFailsClosed) {
    auto feature = parseFdl(readFile(fixture("ot2/Ot2Controller.sila.xml")));
    const auto* type = propertyElementType(feature, "CameraPicture", "ImageData");
    ASSERT_NE(type, nullptr);
    fw::Binary value;
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    EXPECT_NE(ValueValidator::validate(*type, value, *field), std::nullopt);
}

TEST(ValueValidatorExternalResolvers, DelegatesSchemaRejectAddedToParsedOt2Fixture) {
    auto feature = parseFdl(readFile(fixture("ot2/Ot2Controller.sila.xml")));
    auto* type = propertyElementType(feature, "CameraPicture", "ImageData");
    ASSERT_NE(type, nullptr);
    auto* constrainedType = std::get_if<DataType::Constrained>(&type->value);
    ASSERT_NE(constrainedType, nullptr);

    ConstraintValue schema;
    schema.kind = ConstraintValue::Schema;
    ConstraintValue::SchemaValue schemaValue;
    schemaValue.type = ConstraintValue::SchemaValue::Type::Json;
    schemaValue.source = ConstraintValue::SchemaValue::Source::Inline;
    schemaValue.value = R"({"type":"object"})";
    schema.schema = std::move(schemaValue);
    constrainedType->constraints.push_back(schema);

    fw::Binary value;
    value.set_value("json-payload");
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message&,
                                      const FieldDescriptor&, int) -> std::optional<std::string> {
        if (constraint.kind != ConstraintValue::Schema) return std::nullopt;
        return std::string{"schema rejected"};
    };

    EXPECT_EQ(ValueValidator::validate(*type, value, *field, {}, resolver), "schema rejected");
}

TEST(ValueValidatorExternalResolvers, SchemaMissingConstraintResolverFailsClosed) {
    // A fresh Schema-only type, not the ot2 ImageData type: that type already
    // carries a ContentType constraint, which would fail closed first and
    // prove nothing about the Schema path specifically.
    ConstraintValue schema;
    schema.kind = ConstraintValue::Schema;
    ConstraintValue::SchemaValue schemaValue;
    schemaValue.type = ConstraintValue::SchemaValue::Type::Json;
    schemaValue.source = ConstraintValue::SchemaValue::Source::Inline;
    schemaValue.value = R"({"type":"object"})";
    schema.schema = std::move(schemaValue);
    DataType schemaOnlyType = constrained(
        std::make_unique<DataType>(DataType{DataType::Basic{BasicType::Binary}}), schema);

    fw::Binary value;
    value.set_value("json-payload");
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);
    EXPECT_NE(ValueValidator::validate(schemaOnlyType, value, *field), std::nullopt);
}

TEST(ValueValidatorExternalResolvers, DelegatesAllowedTypesRejectAddedToParsedAbsorbanceFixture) {
    auto feature = parseFdl(readFile(fixture("sila_base/AbsorbanceReaderService-v1_0.sila.xml")));
    auto* type = propertyType(feature, "MeasurementFilter");
    ASSERT_NE(type, nullptr);

    ConstraintValue allowed;
    allowed.kind = ConstraintValue::AllowedTypes;
    allowed.allowedTypes.push_back(
        std::make_shared<DataType>(DataType{DataType::Basic{BasicType::Boolean}}));
    *type = constrained(std::make_unique<DataType>(DataType{DataType::Basic{BasicType::Any}}),
                        allowed);

    fw::Any value;
    value.set_type("custom.Type");
    value.set_payload("payload");
    const auto* field = value.GetDescriptor()->FindFieldByName("type");
    ASSERT_NE(field, nullptr);
    ConstraintResolver resolver = [&](const ConstraintValue& constraint, const Message&,
                                      const FieldDescriptor&, int) -> std::optional<std::string> {
        if (constraint.kind != ConstraintValue::AllowedTypes) return std::nullopt;
        return std::string{"type not allowed"};
    };

    EXPECT_EQ(ValueValidator::validate(*type, value, *field, {}, resolver), "type not allowed");
}

TEST(ValueValidatorExternalResolvers, ParsesSchemaConstraintFromRealFdl) {
    // The Schema/AllowedTypes delegation tests above build their IR by hand, so a
    // parser that dropped or misread those constraints would still pass them.
    // Pin parser fidelity against the normative ParameterConstraintsTest FDL,
    // whose CheckStringConstraintSchema parameter carries an Xml/Url Schema.
    auto feature = parseFdl(readFile(
        fs::path{SILA2_SOURCE_ROOT} / "third_party" / "sila_base" / "feature_definitions" /
        "org" / "silastandard" / "test" / "ParameterConstraintsTest-v1_0.sila.xml"));
    auto* type = commandParameter(feature, "CheckStringConstraintSchema", "ConstrainedParameter");
    ASSERT_NE(type, nullptr);
    const auto* schema = findConstraint(*type, ConstraintValue::Schema);
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->schema.has_value());
    EXPECT_EQ(schema->schema->type, ConstraintValue::SchemaValue::Type::Xml);
    EXPECT_EQ(schema->schema->source, ConstraintValue::SchemaValue::Source::Url);
    EXPECT_EQ(schema->schema->value,
              "https://gitlab.com/SiLA2/sila_base/-/raw/master/schema/FeatureDefinition.xsd");
}

TEST(ValueValidatorExternalResolvers, ParsesAllowedTypesConstraintFromRealFdl) {
    // Companion fidelity test: CheckAllowedTypesConstraint declares an Any
    // parameter whose AllowedTypes lists exactly Integer and List<Integer>.
    auto feature = parseFdl(readFile(
        fs::path{SILA2_SOURCE_ROOT} / "third_party" / "sila_base" / "feature_definitions" /
        "org" / "silastandard" / "test" / "ParameterConstraintsTest-v1_0.sila.xml"));
    auto* type = commandParameter(feature, "CheckAllowedTypesConstraint", "ConstrainedParameter");
    ASSERT_NE(type, nullptr);
    const auto* allowed = findConstraint(*type, ConstraintValue::AllowedTypes);
    ASSERT_NE(allowed, nullptr);
    ASSERT_EQ(allowed->allowedTypes.size(), 2u);
    ASSERT_NE(allowed->allowedTypes[0], nullptr);
    ASSERT_NE(allowed->allowedTypes[1], nullptr);
    const auto* firstBasic = std::get_if<DataType::Basic>(&allowed->allowedTypes[0]->value);
    ASSERT_NE(firstBasic, nullptr);
    EXPECT_EQ(firstBasic->type, BasicType::Integer);
    const auto* secondList = std::get_if<DataType::List>(&allowed->allowedTypes[1]->value);
    ASSERT_NE(secondList, nullptr);
    ASSERT_NE(secondList->elementType, nullptr);
    const auto* elementBasic = std::get_if<DataType::Basic>(&secondList->elementType->value);
    ASSERT_NE(elementBasic, nullptr);
    EXPECT_EQ(elementBasic->type, BasicType::Integer);
}

}  // namespace
