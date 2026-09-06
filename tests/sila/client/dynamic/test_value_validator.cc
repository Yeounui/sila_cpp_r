// Focused first slice for ValueValidator: real FDL constraints are parsed
// first, then exercised against the protobuf reflection shape they describe.
#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>
#include <sila/client/dynamic/ValueValidator.h>

#include "AnyWireValue.h"
#include "SiLAFramework.pb.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
namespace fw = sila2::org::silastandard;
using sila2::dynamic::BasicType;
using sila2::dynamic::ConstraintValue;
using sila2::dynamic::DataType;
using sila2::dynamic::DataTypeResolver;
using sila2::dynamic::DescriptorBuilder;
using sila2::dynamic::Feature;
using sila2::dynamic::ValueValidator;
using sila2::dynamic::parseDataTypeXml;
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

DataType* mutableCommandResponse(Feature& feature, std::string_view command,
                                 std::string_view response) {
    for (auto& item : feature.commands) {
        if (item.identifier != command) continue;
        for (auto& candidate : item.responses) {
            if (candidate.identifier == response) return &candidate.dataType;
        }
    }
    return nullptr;
}

const DataType* commandResponseType(const Feature& feature, std::string_view command,
                                    std::string_view response) {
    for (const auto& item : feature.commands) {
        if (item.identifier != command) continue;
        for (const auto& candidate : item.responses) {
            if (candidate.identifier == response) return &candidate.dataType;
        }
    }
    return nullptr;
}

const DataType* dataTypeDefinition(const Feature& feature, std::string_view identifier) {
    for (const auto& definition : feature.dataTypeDefinitions) {
        if (definition.identifier == identifier) return &definition.dataType;
    }
    return nullptr;
}

DataTypeResolver definitions(const Feature& feature) {
    return [&feature](std::string_view identifier) -> const DataType* {
        return dataTypeDefinition(feature, identifier);
    };
}

std::string packageName(const Feature& feature) {
    const auto major = feature.featureVersion.substr(0, feature.featureVersion.find('.'));
    std::string lower = feature.identifier;
    for (char& character : lower) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return "sila2." + feature.originator + "." + feature.category + "." + lower + ".v" + major;
}

std::unique_ptr<google::protobuf::Message> commandRequest(
    const Feature& feature, std::string_view command, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
    const auto fileProto = DescriptorBuilder{}.build(feature);
    const auto* file = pool.BuildFile(fileProto);
    if (file == nullptr) throw std::runtime_error{"unable to build test descriptor"};
    const auto* descriptor = pool.FindMessageTypeByName(
        packageName(feature) + "." + std::string{command} + "_Parameters");
    if (descriptor == nullptr) throw std::runtime_error{"command request descriptor not found"};
    return std::unique_ptr<google::protobuf::Message>{factory.GetPrototype(descriptor)->New()};
}

std::unique_ptr<google::protobuf::Message> commandResponse(
    const Feature& feature, std::string_view command, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
    const auto fileProto = DescriptorBuilder{}.build(feature);
    const auto* file = pool.BuildFile(fileProto);
    if (file == nullptr) throw std::runtime_error{"unable to build test descriptor"};
    const auto* descriptor = pool.FindMessageTypeByName(
        packageName(feature) + "." + std::string{command} + "_Responses");
    if (descriptor == nullptr) throw std::runtime_error{"command response descriptor not found"};
    return std::unique_ptr<google::protobuf::Message>{factory.GetPrototype(descriptor)->New()};
}

DataType* mutableDataTypeDefinition(Feature& feature, std::string_view identifier) {
    for (auto& definition : feature.dataTypeDefinitions) {
        if (definition.identifier == identifier) return &definition.dataType;
    }
    return nullptr;
}

template <typename Temporal>
void setTimezone(Temporal& value, int hours, std::uint32_t minutes) {
    auto* timezone = value.mutable_timezone();
    timezone->set_hours(hours);
    timezone->set_minutes(minutes);
}

TEST(ValueValidator, UsesRealStringAndFqiFixtures) {
    const auto absorbance = parseFdl(readFile(fixture("sila_base/AbsorbanceReaderService-v1_0.sila.xml")));
    const auto* wellAddress = dataTypeDefinition(absorbance, "WellAddress");
    ASSERT_NE(wellAddress, nullptr);
    const auto& structure = std::get<DataType::Structure>(wellAddress->value);
    ASSERT_GE(structure.elements.size(), 1u);
    ASSERT_NE(structure.elements[0].dataType, nullptr);

    fw::String row;
    const auto* rowValue = row.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(rowValue, nullptr);
    row.set_value("A");
    EXPECT_EQ(ValueValidator::validate(*structure.elements[0].dataType, row, *rowValue),
              std::nullopt);
    row.set_value("한");  // one UTF-8 codepoint, three bytes
    EXPECT_NE(ValueValidator::validate(*structure.elements[0].dataType, row, *rowValue),
              std::nullopt);
    row.set_value("a");
    EXPECT_NE(ValueValidator::validate(*structure.elements[0].dataType, row, *rowValue),
              std::nullopt);

    // Restores the FQI-delegation coverage lost when the raw Codex change-set
    // deleted this block (see sc21.json evidence item 8): this is the only
    // path in the suite that exercises checkFqiConstraint's FQI-kind delegate
    // (ValueValidator.cc) rather than the checker's own direct-call tests.
    const auto gateway = parseFdl(readFile(fixture("sila_base/GatewayService-v1_0.sila.xml")));
    const auto* fqiType = commandParameter(gateway, "GetDeviceIdentifiers", "FeatureIdentifier");
    ASSERT_NE(fqiType, nullptr);
    fw::String fqi;
    const auto* fqiValue = fqi.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(fqiValue, nullptr);
    fqi.set_value("com.tecan/core/GatewayService/v1");
    EXPECT_EQ(ValueValidator::validate(*fqiType, fqi, *fqiValue), std::nullopt);
    fqi.set_value("com.tecan/core/GatewayService/v1/Command/GetDeviceIdentifiers");
    EXPECT_NE(ValueValidator::validate(*fqiType, fqi, *fqiValue), std::nullopt);
}

TEST(ValueValidator, UsesRealListFixtureAndIdentifierResolver) {
    const auto robot = parseFdl(readFile(fixture("panda/RobotController.sila.xml")));
    const auto* effortType = commandParameter(robot, "SetArmEffort", "Effort");
    ASSERT_NE(effortType, nullptr);
    const auto* frameListType = commandParameter(robot, "FollowFrames", "FrameList");
    ASSERT_NE(frameListType, nullptr);

    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    auto effort = commandRequest(robot, "SetArmEffort", pool, factory);
    const auto* effortField = effort->GetDescriptor()->FindFieldByName("Effort");
    ASSERT_NE(effortField, nullptr);
    for (int i = 0; i < 7; ++i) {
        auto* value = effort->GetReflection()->AddMessage(effort.get(), effortField);
        const auto* valueField = value->GetDescriptor()->FindFieldByName("value");
        ASSERT_NE(valueField, nullptr);
        value->GetReflection()->SetDouble(value, valueField, static_cast<double>(i));
    }
    EXPECT_EQ(ValueValidator::validate(*effortType, *effort, *effortField), std::nullopt);
    effort->GetReflection()->RemoveLast(effort.get(), effortField);
    EXPECT_NE(ValueValidator::validate(*effortType, *effort, *effortField), std::nullopt);

    // FrameList is a constrained List<DataTypeIdentifier>; resolving Frame
    // enters its generated DataType_Frame wrapper and validates its own exact
    // seven-value list as well.
    google::protobuf::DescriptorPool framePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory frameFactory{&framePool};
    auto frames = commandRequest(robot, "FollowFrames", framePool, frameFactory);
    const auto* framesField = frames->GetDescriptor()->FindFieldByName("FrameList");
    ASSERT_NE(framesField, nullptr);
    auto* frame = frames->GetReflection()->AddMessage(frames.get(), framesField);
    const auto* valuesField = frame->GetDescriptor()->FindFieldByName("Frame");
    ASSERT_NE(valuesField, nullptr);
    for (int i = 0; i < 7; ++i) {
        auto* value = frame->GetReflection()->AddMessage(frame, valuesField);
        const auto* valueField = value->GetDescriptor()->FindFieldByName("value");
        ASSERT_NE(valueField, nullptr);
        value->GetReflection()->SetDouble(value, valueField, static_cast<double>(i));
    }
    EXPECT_EQ(ValueValidator::validate(*frameListType, *frames, *framesField, definitions(robot)),
              std::nullopt);
    frames->GetReflection()->ClearField(frames.get(), framesField);
    EXPECT_NE(ValueValidator::validate(*frameListType, *frames, *framesField, definitions(robot)),
              std::nullopt);
}

TEST(ValueValidator, ChecksIntegerLexicallyAndWithAllBounds) {
    const auto integerFeature = parseFdl(readFile(fixture("sila_base/valid-fdl/IntegerSet.sila.xml")));
    const auto* integerType = dataTypeDefinition(integerFeature, "TestType");
    ASSERT_NE(integerType, nullptr);
    fw::Integer integer;
    const auto* integerValue = integer.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(integerValue, nullptr);
    integer.set_value(-1);
    EXPECT_EQ(ValueValidator::validate(*integerType, integer, *integerValue), std::nullopt);
    integer.set_value(2);
    EXPECT_EQ(ValueValidator::validate(*integerType, integer, *integerValue), std::nullopt);
    integer.set_value(4);
    EXPECT_NE(ValueValidator::validate(*integerType, integer, *integerValue), std::nullopt);

    auto constrainedFeature = parseFdl(readFile(fixture("sila_base/valid-fdl/Constrained.sila.xml")));
    auto* constrainedType = mutableCommandResponse(constrainedFeature, "TestCommand", "Response1");
    ASSERT_NE(constrainedType, nullptr);
    auto* constrained = std::get_if<DataType::Constrained>(&constrainedType->value);
    ASSERT_NE(constrained, nullptr);
    ASSERT_EQ(constrained->constraints.size(), 1u);
    auto& bound = constrained->constraints.front();
    const auto max = std::numeric_limits<std::int64_t>::max();
    integer.set_value(max);
    auto setBound = [&bound](ConstraintValue::Kind kind, std::string text) {
        bound.kind = kind;
        bound.lexicalValue = std::move(text);
        bound.stringValue = bound.lexicalValue;
    };
    setBound(ConstraintValue::MaxExclusive, "9223372036854775808");
    EXPECT_EQ(ValueValidator::validate(*constrainedType, integer, *integerValue), std::nullopt);
    setBound(ConstraintValue::MaxExclusive, "9223372036854775807");
    EXPECT_NE(ValueValidator::validate(*constrainedType, integer, *integerValue), std::nullopt);
    setBound(ConstraintValue::MinInclusive, "9223372036854775807");
    EXPECT_EQ(ValueValidator::validate(*constrainedType, integer, *integerValue), std::nullopt);
    setBound(ConstraintValue::MinExclusive, "9223372036854775807");
    EXPECT_NE(ValueValidator::validate(*constrainedType, integer, *integerValue), std::nullopt);
    setBound(ConstraintValue::MaxInclusive, "9223372036854775807");
    EXPECT_EQ(ValueValidator::validate(*constrainedType, integer, *integerValue), std::nullopt);
}

TEST(ValueValidator, ChecksRealSetSpecialValuesAndBounds) {
    auto feature = parseFdl(readFile(fixture("sila_base/valid-fdl/RealSet.sila.xml")));
    auto* type = mutableDataTypeDefinition(feature, "TestType");
    ASSERT_NE(type, nullptr);
    fw::Real real;
    const auto* value = real.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(value, nullptr);
    real.set_value(-0.32);
    EXPECT_EQ(ValueValidator::validate(*type, real, *value), std::nullopt);
    real.set_value(-std::numeric_limits<double>::infinity());
    EXPECT_EQ(ValueValidator::validate(*type, real, *value), std::nullopt);
    real.set_value(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(ValueValidator::validate(*type, real, *value), std::nullopt);
    real.set_value(std::numeric_limits<double>::infinity());
    EXPECT_NE(ValueValidator::validate(*type, real, *value), std::nullopt);

    auto* constrained = std::get_if<DataType::Constrained>(&type->value);
    ASSERT_NE(constrained, nullptr);
    ASSERT_EQ(constrained->constraints.size(), 1u);
    auto& bound = constrained->constraints.front();
    auto setBound = [&bound](ConstraintValue::Kind kind, std::string text) {
        bound.kind = kind;
        bound.lexicalValue = std::move(text);
        bound.stringValue = bound.lexicalValue;
    };
    setBound(ConstraintValue::MinInclusive, "0");
    real.set_value(0);
    EXPECT_EQ(ValueValidator::validate(*type, real, *value), std::nullopt);
    real.set_value(-0.32);
    EXPECT_NE(ValueValidator::validate(*type, real, *value), std::nullopt);
    real.set_value(std::numeric_limits<double>::infinity());
    EXPECT_EQ(ValueValidator::validate(*type, real, *value), std::nullopt);
    real.set_value(std::numeric_limits<double>::quiet_NaN());
    EXPECT_NE(ValueValidator::validate(*type, real, *value), std::nullopt);
    setBound(ConstraintValue::MaxExclusive, "0");
    real.set_value(-std::numeric_limits<double>::infinity());
    EXPECT_EQ(ValueValidator::validate(*type, real, *value), std::nullopt);
    real.set_value(0);
    EXPECT_NE(ValueValidator::validate(*type, real, *value), std::nullopt);
}

TEST(ValueValidator, MapsExtremeRealConstraintLexicalsToDoubleValues) {
    auto feature = parseFdl(readFile(fixture("sila_base/valid-fdl/RealSet.sila.xml")));
    auto* type = mutableDataTypeDefinition(feature, "TestType");
    ASSERT_NE(type, nullptr);
    auto* constrained = std::get_if<DataType::Constrained>(&type->value);
    ASSERT_NE(constrained, nullptr);
    ASSERT_EQ(constrained->constraints.size(), 1u);
    auto& set = constrained->constraints.front();
    set.stringValues = {"1e309", "-1e309", "1e-400", "-1e-400", "3e-324", "-3e-324"};

    fw::Real real;
    const auto* value = real.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(value, nullptr);
    for (const double candidate : {std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(), 0.0, -0.0,
                                   std::numeric_limits<double>::denorm_min(),
                                   -std::numeric_limits<double>::denorm_min()}) {
        real.set_value(candidate);
        EXPECT_EQ(ValueValidator::validate(*type, real, *value), std::nullopt);
    }
    real.set_value(1.0);
    EXPECT_NE(ValueValidator::validate(*type, real, *value), std::nullopt);
}

TEST(ValueValidator, RejectsBooleanConstraintsAndRecursesThroughBooleanList) {
    const auto listFeature = parseFdl(readFile(fixture("sila_base/valid-fdl/List.sila.xml")));
    const auto* listType = commandResponseType(listFeature, "TestCommand", "Response1");
    ASSERT_NE(listType, nullptr);
    const auto* listDataType = std::get_if<DataType::List>(&listType->value);
    ASSERT_NE(listDataType, nullptr);
    DataType type{DataType::Constrained{
        std::make_unique<DataType>(DataType{DataType::Basic{BasicType::Boolean}}), {}}};
    auto* constrained = std::get_if<DataType::Constrained>(&type.value);
    ASSERT_NE(constrained, nullptr);
    ConstraintValue booleanSet;
    booleanSet.kind = ConstraintValue::Set;
    booleanSet.stringValues = {"true"};
    constrained->constraints.push_back(std::move(booleanSet));
    fw::Boolean boolean;
    const auto* value = boolean.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(value, nullptr);
    boolean.set_value(true);
    EXPECT_NE(ValueValidator::validate(type, boolean, *value), std::nullopt);

    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    auto listMessage = commandResponse(listFeature, "TestCommand", pool, factory);
    const auto* listField = listMessage->GetDescriptor()->FindFieldByName("Response1");
    ASSERT_NE(listField, nullptr);
    for (const bool item : {true, false}) {
        auto* element = listMessage->GetReflection()->AddMessage(listMessage.get(), listField);
        const auto* elementValue = element->GetDescriptor()->FindFieldByName("value");
        ASSERT_NE(elementValue, nullptr);
        element->GetReflection()->SetBool(element, elementValue, item);
    }
    EXPECT_EQ(ValueValidator::validate(*listType, *listMessage, *listField), std::nullopt);
}

TEST(ValueValidator, ChecksDateSetAndTemporalBounds) {
    auto feature = parseFdl(readFile(fixture("sila_base/valid-fdl/DateSet.sila.xml")));
    auto* type = mutableDataTypeDefinition(feature, "TestType");
    ASSERT_NE(type, nullptr);
    fw::Date date;
    const auto* field = date.GetDescriptor()->FindFieldByName("timezone");
    ASSERT_NE(field, nullptr);
    date.set_year(2020);
    date.set_month(1);
    date.set_day(1);
    setTimezone(date, 0, 0);
    EXPECT_EQ(ValueValidator::validate(*type, date, *field), std::nullopt);
    setTimezone(date, 1, 0);
    EXPECT_NE(ValueValidator::validate(*type, date, *field), std::nullopt);
    date.set_month(2);
    date.set_day(30);
    setTimezone(date, 0, 0);
    EXPECT_NE(ValueValidator::validate(*type, date, *field), std::nullopt);
    date.set_year(0);
    date.set_month(1);
    date.set_day(1);
    setTimezone(date, 2, 30);
    EXPECT_NE(ValueValidator::validate(*type, date, *field), std::nullopt);
    // D1 fix: minutes has no sign of its own (SiLAFramework.proto:123-126), so
    // the +-14:00 limit applies to hours*60+minutes, not to hours alone.
    // {-14, 0} is exactly -14:00 (valid); {-15, 0} is -15:00 (out of range).
    date.set_year(1990);
    date.set_month(1);
    date.set_day(13);
    setTimezone(date, -14, 0);
    EXPECT_EQ(ValueValidator::validate(*type, date, *field), std::nullopt);
    setTimezone(date, -15, 0);
    EXPECT_NE(ValueValidator::validate(*type, date, *field), std::nullopt);

    auto* constrained = std::get_if<DataType::Constrained>(&type->value);
    ASSERT_NE(constrained, nullptr);
    ASSERT_EQ(constrained->constraints.size(), 1u);
    auto& bound = constrained->constraints.front();
    bound.kind = ConstraintValue::MinInclusive;
    bound.lexicalValue = "2020-01-01Z";
    bound.stringValue = bound.lexicalValue;
    date.set_year(2020);
    date.set_month(1);
    date.set_day(1);
    setTimezone(date, 0, 0);
    EXPECT_EQ(ValueValidator::validate(*type, date, *field), std::nullopt);
    date.set_year(2019);
    EXPECT_NE(ValueValidator::validate(*type, date, *field), std::nullopt);
}

TEST(ValueValidator, ChecksTimeSetAndTimezoneOrdering) {
    auto feature = parseFdl(readFile(fixture("sila_base/valid-fdl/TimeSet.sila.xml")));
    auto* type = mutableDataTypeDefinition(feature, "TestType");
    ASSERT_NE(type, nullptr);
    fw::Time time;
    const auto* field = time.GetDescriptor()->FindFieldByName("timezone");
    ASSERT_NE(field, nullptr);
    time.set_hour(0);
    time.set_minute(0);
    time.set_second(59);
    setTimezone(time, -3, 30);  // protobuf's canonical representation of -02:30
    EXPECT_EQ(ValueValidator::validate(*type, time, *field), std::nullopt);
    time.set_millisecond(998);
    EXPECT_NE(ValueValidator::validate(*type, time, *field), std::nullopt);
    time.set_millisecond(999);
    time.set_hour(23);
    time.set_minute(59);
    time.set_second(0);
    setTimezone(time, 14, 0);
    time.set_millisecond(0);
    EXPECT_EQ(ValueValidator::validate(*type, time, *field), std::nullopt);

    auto* constrained = std::get_if<DataType::Constrained>(&type->value);
    ASSERT_NE(constrained, nullptr);
    auto& bound = constrained->constraints.front();
    bound.kind = ConstraintValue::MinInclusive;
    bound.lexicalValue = "23:59:00Z";
    bound.stringValue = bound.lexicalValue;
    setTimezone(time, 0, 0);
    EXPECT_EQ(ValueValidator::validate(*type, time, *field), std::nullopt);
    time.set_hour(0);
    EXPECT_NE(ValueValidator::validate(*type, time, *field), std::nullopt);
}

TEST(ValueValidator, ChecksTimestampSetAndMilliseconds) {
    auto feature = parseFdl(readFile(fixture("sila_base/valid-fdl/TimestampSet.sila.xml")));
    auto* type = mutableDataTypeDefinition(feature, "TestType");
    ASSERT_NE(type, nullptr);
    fw::Timestamp timestamp;
    const auto* field = timestamp.GetDescriptor()->FindFieldByName("timezone");
    ASSERT_NE(field, nullptr);
    timestamp.set_year(2021);
    timestamp.set_month(2);
    timestamp.set_day(28);
    timestamp.set_hour(0);
    timestamp.set_minute(0);
    timestamp.set_second(0);
    timestamp.set_millisecond(0);
    setTimezone(timestamp, 2, 30);  // 2021-02-28T00:00:00+02:30
    EXPECT_EQ(ValueValidator::validate(*type, timestamp, *field), std::nullopt);
    setTimezone(timestamp, 0, 0);
    timestamp.set_year(2021);
    timestamp.set_month(2);
    timestamp.set_day(27);
    timestamp.set_hour(21);
    timestamp.set_minute(30);
    EXPECT_EQ(ValueValidator::validate(*type, timestamp, *field), std::nullopt);
    timestamp.set_day(28);
    timestamp.set_hour(0);
    timestamp.set_minute(0);
    timestamp.set_millisecond(998);
    EXPECT_NE(ValueValidator::validate(*type, timestamp, *field), std::nullopt);
    timestamp.set_millisecond(999);
    EXPECT_EQ(ValueValidator::validate(*type, timestamp, *field), std::nullopt);

    auto* constrained = std::get_if<DataType::Constrained>(&type->value);
    ASSERT_NE(constrained, nullptr);
    auto& bound = constrained->constraints.front();
    bound.kind = ConstraintValue::MaxExclusive;
    bound.lexicalValue = "2021-02-28T00:00:00.999Z";
    bound.stringValue = bound.lexicalValue;
    EXPECT_NE(ValueValidator::validate(*type, timestamp, *field), std::nullopt);
    timestamp.set_millisecond(998);
    EXPECT_EQ(ValueValidator::validate(*type, timestamp, *field), std::nullopt);
}

TEST(ValueValidator, EnforcesBasicDateAndTimestampYearRange) {
    const DataType dateType{DataType::Basic{BasicType::Date}};
    fw::Date date;
    const auto* dateField = date.GetDescriptor()->FindFieldByName("timezone");
    ASSERT_NE(dateField, nullptr);
    date.set_month(1);
    date.set_day(1);
    setTimezone(date, 0, 0);
    for (const std::uint32_t year : {1U, 9999U}) {
        date.set_year(year);
        EXPECT_EQ(ValueValidator::validate(dateType, date, *dateField), std::nullopt);
    }
    date.set_year(0);
    EXPECT_NE(ValueValidator::validate(dateType, date, *dateField), std::nullopt);

    const DataType timestampType{DataType::Basic{BasicType::Timestamp}};
    fw::Timestamp timestamp;
    const auto* timestampField = timestamp.GetDescriptor()->FindFieldByName("timezone");
    ASSERT_NE(timestampField, nullptr);
    timestamp.set_month(1);
    timestamp.set_day(1);
    timestamp.set_hour(0);
    timestamp.set_minute(0);
    timestamp.set_second(0);
    timestamp.set_millisecond(0);
    setTimezone(timestamp, 0, 0);
    for (const std::uint32_t year : {1U, 9999U}) {
        timestamp.set_year(year);
        EXPECT_EQ(ValueValidator::validate(timestampType, timestamp, *timestampField), std::nullopt);
    }
    timestamp.set_year(0);
    EXPECT_NE(ValueValidator::validate(timestampType, timestamp, *timestampField), std::nullopt);
}

TEST(ValueValidator, EnforcesBasicStringMaximumUnicodeCharacterCount) {
    constexpr std::size_t kMaxCharacters = 2U * 1024U * 1024U;
    const DataType type{DataType::Basic{BasicType::String}};
    fw::String value;
    const auto* field = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(field, nullptr);

    std::string text;
    text.reserve(kMaxCharacters * 2U);
    for (std::size_t index = 0; index < kMaxCharacters; ++index) text += "\xC3\xA9";
    value.set_value(text);
    EXPECT_EQ(ValueValidator::validate(type, value, *field), std::nullopt);
    value.mutable_value()->append("\xC3\xA9");
    EXPECT_NE(ValueValidator::validate(type, value, *field), std::nullopt);
}

TEST(ValueValidator, RecursesThroughRealStructureAndListFixture) {
    const auto feature = parseFdl(
        readFile(fixture("sila_base/valid-fdl/ListOfStructure.sila.xml")));
    const auto* responseType = [&feature]() -> const DataType* {
        for (const auto& command : feature.commands) {
            if (command.identifier != "TestCommand") continue;
            for (const auto& response : command.responses) {
                if (response.identifier == "Response1") return &response.dataType;
            }
        }
        return nullptr;
    }();
    ASSERT_NE(responseType, nullptr);
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    auto response = commandResponse(feature, "TestCommand", pool, factory);
    const auto* responseField = response->GetDescriptor()->FindFieldByName("Response1");
    ASSERT_NE(responseField, nullptr);
    auto* outer = response->GetReflection()->AddMessage(response.get(), responseField);
    const auto* element1Field = outer->GetDescriptor()->FindFieldByName("Element1");
    const auto* element2Field = outer->GetDescriptor()->FindFieldByName("Element2");
    ASSERT_NE(element1Field, nullptr);
    ASSERT_NE(element2Field, nullptr);
    auto* element1 = outer->GetReflection()->MutableMessage(outer, element1Field);
    const auto* integerField = element1->GetDescriptor()->FindFieldByName("SubstructureElement1");
    const auto* booleanField = element1->GetDescriptor()->FindFieldByName("SubstructureElement2");
    ASSERT_NE(integerField, nullptr);
    ASSERT_NE(booleanField, nullptr);
    auto* integer = element1->GetReflection()->MutableMessage(element1, integerField);
    auto* boolean = element1->GetReflection()->MutableMessage(element1, booleanField);
    integer->GetReflection()->SetInt64(integer, integer->GetDescriptor()->FindFieldByName("value"), 7);
    boolean->GetReflection()->SetBool(boolean, boolean->GetDescriptor()->FindFieldByName("value"), true);
    auto* string = outer->GetReflection()->MutableMessage(outer, element2Field);
    string->GetReflection()->SetString(string, string->GetDescriptor()->FindFieldByName("value"), "ok");
    EXPECT_EQ(ValueValidator::validate(*responseType, *response, *responseField), std::nullopt);
    outer->GetReflection()->ClearField(outer, element1Field);
    EXPECT_NE(ValueValidator::validate(*responseType, *response, *responseField), std::nullopt);
}

TEST(ValueValidator, RejectsMissingBinaryValueAndForeignFieldDescriptor) {
    const DataType binaryType{DataType::Basic{BasicType::Binary}};
    fw::Binary binary;
    const auto* binaryValue = binary.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(binaryValue, nullptr);
    EXPECT_NE(ValueValidator::validate(binaryType, binary, *binaryValue), std::nullopt);

    const DataType stringType{DataType::Basic{BasicType::String}};
    fw::String string;
    const auto* foreignField = binary.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(foreignField, nullptr);
    EXPECT_NE(ValueValidator::validate(stringType, string, *foreignField), std::nullopt);
}

TEST(ValueValidator, CountsBinaryBytesAndFailsClosedForUnsupportedConstraints) {
    const auto binaryType = parseDataTypeXml(R"xml(
<DataType xmlns="http://www.sila-standard.org">
  <Constrained>
    <DataType><Basic>Binary</Basic></DataType>
    <Constraints><Length>3</Length></Constraints>
  </Constrained>
</DataType>)xml");
    fw::Binary binary;
    const auto* binaryValue = binary.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(binaryValue, nullptr);
    binary.set_value(std::string{'\0', static_cast<char>(0x80), 'A'});
    EXPECT_EQ(ValueValidator::validate(binaryType, binary, *binaryValue), std::nullopt);
    binary.set_value(std::string{"\x00A", 2});
    EXPECT_NE(ValueValidator::validate(binaryType, binary, *binaryValue), std::nullopt);

    const auto unsupportedType = parseDataTypeXml(R"xml(
<DataType xmlns="http://www.sila-standard.org">
  <Constrained>
    <DataType><Basic>String</Basic></DataType>
    <Constraints><ContentType><Type>application</Type><Subtype>xml</Subtype></ContentType></Constraints>
  </Constrained>
</DataType>)xml");
    fw::String string;
    const auto* stringValue = string.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(stringValue, nullptr);
    string.set_value("anything");
    EXPECT_NE(ValueValidator::validate(unsupportedType, string, *stringValue), std::nullopt);

    const auto absorbance = parseFdl(readFile(fixture("sila_base/AbsorbanceReaderService-v1_0.sila.xml")));
    const auto* row = dataTypeDefinition(absorbance, "WellAddress");
    ASSERT_NE(row, nullptr);
    const auto& elements = std::get<DataType::Structure>(row->value).elements;
    ASSERT_NE(elements[0].dataType, nullptr);
    DataType identifier{DataType::Identifier{"Row"}};
    string.set_value("A");
    EXPECT_EQ(ValueValidator::validate(identifier, string, *stringValue,
                                       [&elements](std::string_view name) -> const DataType* {
                                           return name == "Row" ? elements[0].dataType.get() : nullptr;
                                       }),
              std::nullopt);
    EXPECT_NE(ValueValidator::validate(identifier, string, *stringValue), std::nullopt);
}

// --- S45: shared constraint preparation (finding 1) and the size-parser
// duplication fix (finding 2) --------------------------------------------------

TEST(ValueValidator, MinimalLengthBeyondDoubleRangeRejectsAnyString) {
    // FdlRuntimeParser.cc's numericValueOrZero stores numericValue=0 for
    // lexical text outside double's range; Length/MinLength/MaxLength must
    // resolve the bound from the lexical text (via the ConstraintChecker.h
    // parseSizeConstraint both ValueValidator and ConstraintChecker now share),
    // not silently accept everything through that lossy 0 (S45 finding 2).
    const std::string oversizedBound = "1" + std::string(400, '0');
    const auto minimalLengthType = parseDataTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained><DataType><Basic>String</Basic></DataType>"
        "<Constraints><MinimalLength>" + oversizedBound + "</MinimalLength></Constraints>"
        "</Constrained></DataType>");
    fw::String value;
    const auto* valueField = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    value.set_value("");
    EXPECT_NE(ValueValidator::validate(minimalLengthType, value, *valueField), std::nullopt);
    value.set_value("short but nonempty");
    EXPECT_NE(ValueValidator::validate(minimalLengthType, value, *valueField), std::nullopt);

    const auto exactLengthType = parseDataTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained><DataType><Basic>String</Basic></DataType>"
        "<Constraints><Length>" + oversizedBound + "</Length></Constraints>"
        "</Constrained></DataType>");
    value.set_value("");
    EXPECT_NE(ValueValidator::validate(exactLengthType, value, *valueField), std::nullopt);
}

TEST(ValueValidator, ElementCountBeyondDoubleRangeRejectsEmptyList) {
    // Same S45 finding 2 guard as the length test above, for the element-count
    // arm: the bound must resolve through the shared lexical parser
    // (parseSizeConstraint), not the lossy numericValue 0 the FDL parser
    // stores for xs:nonNegativeInteger text outside double's range.
    const std::string oversizedBound = "1" + std::string(400, '0');
    const auto listType = parseDataTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained>"
        "<DataType><List><DataType><Basic>Boolean</Basic></DataType></List></DataType>"
        "<Constraints><ElementCount>" + oversizedBound + "</ElementCount></Constraints>"
        "</Constrained></DataType>");

    const auto listFeature = parseFdl(readFile(fixture("sila_base/valid-fdl/List.sila.xml")));
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    auto listMessage = commandResponse(listFeature, "TestCommand", pool, factory);
    const auto* listField = listMessage->GetDescriptor()->FindFieldByName("Response1");
    ASSERT_NE(listField, nullptr);
    EXPECT_NE(ValueValidator::validate(listType, *listMessage, *listField), std::nullopt);
}

TEST(ValueValidator, ElementCountZeroAcceptsEmptyListRejectsNonEmpty) {
    // Part B p85: an ElementCount Constraint Value MUST be an integer equal or
    // greater than zero (0). The src/schema/Constraints.xsd overlay retypes it
    // xs:nonNegativeInteger, so <ElementCount>0</ElementCount> now parses (the
    // pinned xs:positiveInteger rejected it at XSD validation). A 0 exact count
    // means the list MUST be empty: empty is valid, any element is a violation.
    const auto listType = parseDataTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained>"
        "<DataType><List><DataType><Basic>Boolean</Basic></DataType></List></DataType>"
        "<Constraints><ElementCount>0</ElementCount></Constraints>"
        "</Constrained></DataType>");
    const auto listFeature = parseFdl(readFile(fixture("sila_base/valid-fdl/List.sila.xml")));
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    auto listMessage = commandResponse(listFeature, "TestCommand", pool, factory);
    const auto* listField = listMessage->GetDescriptor()->FindFieldByName("Response1");
    ASSERT_NE(listField, nullptr);
    // Empty list satisfies ElementCount 0.
    EXPECT_EQ(ValueValidator::validate(listType, *listMessage, *listField), std::nullopt);
    // One element violates ElementCount 0.
    auto* element = listMessage->GetReflection()->AddMessage(listMessage.get(), listField);
    const auto* elementValue = element->GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(elementValue, nullptr);
    element->GetReflection()->SetBool(element, elementValue, true);
    EXPECT_NE(ValueValidator::validate(listType, *listMessage, *listField), std::nullopt);
}

TEST(ValueValidator, MinimalLengthZeroAcceptsAnyStringAndRejectsNegativeAtSchema) {
    // Part A p67 / Part B p83 (R9-6): a MinimalLength Constraint Value MUST be an
    // integer equal or greater than zero. The src/schema/Constraints.xsd overlay
    // retypes it xs:nonNegativeInteger, so <MinimalLength>0</MinimalLength> now
    // passes the AnyTypeDataType.xsd->DataTypes.xsd->Constraints.xsd chain that
    // parseDataTypeXml validates against (the pinned xs:positiveInteger rejected 0).
    const auto zeroType = parseDataTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained><DataType><Basic>String</Basic></DataType>"
        "<Constraints><MinimalLength>0</MinimalLength></Constraints>"
        "</Constrained></DataType>");
    fw::String value;
    const auto* valueField = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    value.set_value("");
    EXPECT_EQ(ValueValidator::validate(zeroType, value, *valueField), std::nullopt);  // empty string satisfies min 0
    value.set_value("nonempty");
    EXPECT_EQ(ValueValidator::validate(zeroType, value, *valueField), std::nullopt);
    // xs:nonNegativeInteger still rejects -1 at the schema stage.
    EXPECT_THROW(parseDataTypeXml(
                     "<DataType xmlns=\"http://www.sila-standard.org\">"
                     "<Constrained><DataType><Basic>String</Basic></DataType>"
                     "<Constraints><MinimalLength>-1</MinimalLength></Constraints>"
                     "</Constrained></DataType>"),
                 std::invalid_argument);
}

TEST(ValueValidator, PreparedAndUnpreparedPatternsAgree) {
    // A hand-built ConstraintValue (as any caller outside the FDL runtime
    // parser would construct one) has no preparedPattern; only
    // parseFdl/parseDataTypeXml fill it in. Both
    // must reach the same verdict for the same input (S45 finding 1).
    ConstraintValue handBuiltPattern;
    handBuiltPattern.kind = ConstraintValue::Pattern;
    handBuiltPattern.stringValue = "[a-z]+";
    ASSERT_EQ(handBuiltPattern.preparedPattern, nullptr);
    DataType handBuiltType{DataType::Constrained{
        std::make_unique<DataType>(DataType{DataType::Basic{BasicType::String}}),
        {handBuiltPattern}}};

    const auto parsedType = parseDataTypeXml(R"xml(
<DataType xmlns="http://www.sila-standard.org">
  <Constrained>
    <DataType><Basic>String</Basic></DataType>
    <Constraints><Pattern>[a-z]+</Pattern></Constraints>
  </Constrained>
</DataType>)xml");
    const auto* parsedConstrained = std::get_if<DataType::Constrained>(&parsedType.value);
    ASSERT_NE(parsedConstrained, nullptr);
    ASSERT_EQ(parsedConstrained->constraints.size(), 1u);
    ASSERT_NE(parsedConstrained->constraints[0].preparedPattern, nullptr);

    fw::String value;
    const auto* valueField = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    for (const std::string_view candidate : {"hello", "Hello", "123", ""}) {
        value.set_value(std::string{candidate});
        EXPECT_EQ(ValueValidator::validate(handBuiltType, value, *valueField).has_value(),
                  ValueValidator::validate(parsedType, value, *valueField).has_value())
            << "candidate: " << candidate;
    }
}

TEST(ValueValidator, ChecksPatternAndSetAcrossFdlParsedListElements) {
    // Pattern/Set on a List<Constrained<String>> element type is validated
    // once per element (List elements are checked one at a time); this drives
    // that through real FDL-parsed constraints, so Pattern's preparedPattern
    // (shared across elements, S45 finding 1) is exercised end to end.
    const auto feature = parseFdl(R"xml(
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard"
         Category="tests"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
    <Identifier>ListPatternSetTest</Identifier>
    <DisplayName>List Pattern Set Test</DisplayName>
    <Description>Command response with a Pattern- and Set-constrained String list.</Description>
    <Command>
        <Identifier>TestCommand</Identifier>
        <DisplayName>Test Command</DisplayName>
        <Description>Command for testing</Description>
        <Observable>No</Observable>
        <Response>
            <Identifier>Response1</Identifier>
            <DisplayName>Response 1</DisplayName>
            <Description>First response</Description>
            <DataType>
                <List>
                    <DataType>
                        <Constrained>
                            <DataType><Basic>String</Basic></DataType>
                            <Constraints>
                                <Pattern>[a-z]+</Pattern>
                                <Set>
                                    <Value>red</Value>
                                    <Value>green</Value>
                                    <Value>blue</Value>
                                </Set>
                            </Constraints>
                        </Constrained>
                    </DataType>
                </List>
            </DataType>
        </Response>
    </Command>
</Feature>)xml");
    const auto* responseType = commandResponseType(feature, "TestCommand", "Response1");
    ASSERT_NE(responseType, nullptr);
    const auto* listType = std::get_if<DataType::List>(&responseType->value);
    ASSERT_NE(listType, nullptr);
    ASSERT_NE(listType->elementType, nullptr);
    const auto* elementConstrained = std::get_if<DataType::Constrained>(&listType->elementType->value);
    ASSERT_NE(elementConstrained, nullptr);
    ASSERT_EQ(elementConstrained->constraints.size(), 2u);
    ASSERT_NE(elementConstrained->constraints[0].preparedPattern, nullptr);

    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    auto response = commandResponse(feature, "TestCommand", pool, factory);
    const auto* responseField = response->GetDescriptor()->FindFieldByName("Response1");
    ASSERT_NE(responseField, nullptr);

    const auto setElements = [&](const std::vector<std::string>& values) {
        response->GetReflection()->ClearField(response.get(), responseField);
        for (const auto& value : values) {
            auto* element = response->GetReflection()->AddMessage(response.get(), responseField);
            const auto* elementValueField = element->GetDescriptor()->FindFieldByName("value");
            ASSERT_NE(elementValueField, nullptr);
            element->GetReflection()->SetString(element, elementValueField, value);
        }
    };

    setElements({"red", "green", "blue"});
    EXPECT_EQ(ValueValidator::validate(*responseType, *response, *responseField), std::nullopt);

    setElements({"red", "GREEN", "blue"});  // second element violates Pattern
    EXPECT_NE(ValueValidator::validate(*responseType, *response, *responseField), std::nullopt);

    setElements({"red", "green", "purple"});  // third element matches Pattern, not Set
    EXPECT_NE(ValueValidator::validate(*responseType, *response, *responseField), std::nullopt);
}

// --- R10-9b: SiLA Any values are decoded and validated against their own
// embedded type (Part B p66), instead of always rejected -----------------

TEST(ValueValidator, DecodesBasicIntegerAnyAndValidates) {
    // Part B p66: "A receiver MUST parse the XML string and deserialize the
    // payload accordingly." A Basic-Any value now decodes instead of always
    // failing with "Any values are not supported".
    auto wireValue = anytest::wireAny(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>",
        [](google::protobuf::Message& payloadWrapper,
           const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = payloadWrapper.GetReflection()->MutableMessage(&payloadWrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetInt64(inner, valueField, 42);
        });
    const DataType anyType{DataType::Basic{BasicType::Any}};
    EXPECT_EQ(ValueValidator::validate(anyType, *wireValue.host, *wireValue.anyField), std::nullopt);
}

TEST(ValueValidator, ValidatesConstrainedAnyAgainstItsOwnBounds) {
    // Part A p67/A224 + Part B p70 (the v1 hole this batch closes): the
    // Constrained-Any shortcut used to accept ANY payload once a resolver was
    // present, without checking the Any's own embedded Constraints. S =
    // Constrained<Integer, MinimalInclusive 10>.
    const std::string constrainedIntegerXml =
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained><DataType><Basic>Integer</Basic></DataType>"
        "<Constraints><MinimalInclusive>10</MinimalInclusive></Constraints>"
        "</Constrained></DataType>";
    const auto setInteger = [](std::int64_t requested) {
        return [requested](google::protobuf::Message& payloadWrapper,
                           const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = payloadWrapper.GetReflection()->MutableMessage(&payloadWrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetInt64(inner, valueField, requested);
        };
    };
    const DataType anyType{DataType::Basic{BasicType::Any}};

    auto accepted = anytest::wireAny(constrainedIntegerXml, setInteger(42));
    EXPECT_EQ(ValueValidator::validate(anyType, *accepted.host, *accepted.anyField), std::nullopt);

    auto rejected = anytest::wireAny(constrainedIntegerXml, setInteger(5));
    EXPECT_NE(ValueValidator::validate(anyType, *rejected.host, *rejected.anyField), std::nullopt);
}

TEST(ValueValidator, RecursesIntoNestedAny) {
    // S = Basic{Any} whose own decoded payload is itself a wire Any (type
    // String, value "hi"). Proves validateAnyValue's re-entry into
    // validateValue actually reaches a second, nested Any rather than
    // stopping once the outer one is decoded.
    const std::string innerStringTypeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic></DataType>";
    google::protobuf::DescriptorPool innerPool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory innerFactory{&innerPool};
    const auto* innerPrototype = anytest::payloadPrototype(innerStringTypeXml, innerPool, innerFactory);
    std::unique_ptr<google::protobuf::Message> innerWrapper{innerPrototype->New()};
    const auto* innerPayloadField = innerWrapper->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(innerPayloadField, nullptr);
    auto* innerValue = innerWrapper->GetReflection()->MutableMessage(innerWrapper.get(), innerPayloadField);
    innerValue->GetReflection()->SetString(
        innerValue, innerValue->GetDescriptor()->FindFieldByName("value"), "hi");
    const std::string innerStringBytes = innerWrapper->SerializeAsString();

    auto outer = anytest::wireAny(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Any</Basic></DataType>",
        [&](google::protobuf::Message& payloadWrapper,
            const google::protobuf::FieldDescriptor& payloadField) {
            auto* nestedAny = payloadWrapper.GetReflection()->MutableMessage(&payloadWrapper, &payloadField);
            const auto* typeField = nestedAny->GetDescriptor()->FindFieldByName("type");
            const auto* payloadBytesField = nestedAny->GetDescriptor()->FindFieldByName("payload");
            nestedAny->GetReflection()->SetString(nestedAny, typeField, innerStringTypeXml);
            nestedAny->GetReflection()->SetString(nestedAny, payloadBytesField, innerStringBytes);
        });
    const DataType anyType{DataType::Basic{BasicType::Any}};
    EXPECT_EQ(ValueValidator::validate(anyType, *outer.host, *outer.anyField), std::nullopt);
}

TEST(ValueValidator, RejectsAnyReferencingCustomDataType) {
    // Part A p64: "the SiLA Any Type MUST NOT represent information of a
    // Custom Data Type" -- a DataTypeIdentifier in the type XML must be
    // rejected before any decode is attempted.
    auto wireValue = anytest::rawWireAny(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<DataTypeIdentifier>CustomType</DataTypeIdentifier></DataType>",
        "");
    const DataType anyType{DataType::Basic{BasicType::Any}};
    EXPECT_NE(ValueValidator::validate(anyType, *wireValue.host, *wireValue.anyField), std::nullopt);
}

TEST(ValueValidator, RejectsAnyConstrainedOverStructure) {
    // Part A p66: "The SiLA Constrained Type MUST be based on either a SiLA
    // Basic Type or a SiLA List Type" -- never a Structure.
    // AnyTypeDataType.xsd admits this shape (it is XSD-valid), so it can only
    // be rejected semantically, after parsing.
    auto wireValue = anytest::rawWireAny(
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained><DataType><Structure><Element><Identifier>A</Identifier>"
        "<DisplayName>A</DisplayName><Description>d</Description>"
        "<DataType><Basic>Integer</Basic></DataType></Element></Structure></DataType>"
        "<Constraints><ElementCount>1</ElementCount></Constraints></Constrained></DataType>",
        "");
    const DataType anyType{DataType::Basic{BasicType::Any}};
    EXPECT_NE(ValueValidator::validate(anyType, *wireValue.host, *wireValue.anyField), std::nullopt);
}

TEST(ValueValidator, RejectsMalformedAnyTypeXml) {
    // parseDataTypeXml throws on unparsable XML; validateAnyValue must catch
    // it and return a diagnostic (a Validation Error) instead of letting the
    // throw escape ValueValidator::validate on the RPC path.
    auto wireValue = anytest::rawWireAny("<not closed", "");
    const DataType anyType{DataType::Basic{BasicType::Any}};
    EXPECT_NE(ValueValidator::validate(anyType, *wireValue.host, *wireValue.anyField), std::nullopt);
}

TEST(ValueValidator, RejectsAnyPayloadNotMatchingType) {
    // AnyCodec::decode throws when the payload bytes cannot be parsed under
    // the declared type (mirrors test_any_codec.cc's
    // DecodeMalformedPayloadThrows fixture); that throw must also become a
    // diagnostic here, not an uncaught exception.
    std::string malformedPayload;
    malformedPayload.push_back(static_cast<char>(0x0a));
    malformedPayload.push_back(static_cast<char>(0x01));
    auto wireValue = anytest::rawWireAny(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>",
        malformedPayload);
    const DataType anyType{DataType::Basic{BasicType::Any}};
    EXPECT_NE(ValueValidator::validate(anyType, *wireValue.host, *wireValue.anyField), std::nullopt);
}

TEST(ValueValidator, ConstrainedOnConstrainedAppliesBothConstraintLayers) {
    // Part A p66 / Part B p83 (R9-47): a Constrained type MAY be based on a Constrained
    // type; the inner and outer Constraints act as a logical AND. Inner bounds a
    // String to MaximalLength 10; outer applies Pattern [A-Z]+ to the same String
    // base (resolveBase looks through the inner Constrained).
    const auto nested = parseDataTypeXml(
        "<DataType xmlns=\"http://www.sila-standard.org\"><Constrained><DataType><Constrained>"
        "<DataType><Basic>String</Basic></DataType>"
        "<Constraints><MaximalLength>10</MaximalLength></Constraints>"
        "</Constrained></DataType><Constraints><Pattern>[A-Z]+</Pattern></Constraints></Constrained></DataType>");
    fw::String value;
    const auto* valueField = value.GetDescriptor()->FindFieldByName("value");
    ASSERT_NE(valueField, nullptr);
    value.set_value("AB");
    EXPECT_EQ(ValueValidator::validate(nested, value, *valueField), std::nullopt);  // satisfies both
    value.set_value("ab");
    EXPECT_NE(ValueValidator::validate(nested, value, *valueField), std::nullopt);  // violates outer Pattern
    value.set_value("ABCDEFGHIJK");
    EXPECT_NE(ValueValidator::validate(nested, value, *valueField), std::nullopt);  // 11 chars: violates inner MaximalLength 10
}

}  // namespace
