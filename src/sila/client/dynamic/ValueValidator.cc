// ValueValidator.cc — the first runtime value-validation slice.
#include <sila/client/dynamic/ValueValidator.h>

#include <sila/client/dynamic/AnyCodec.h>
#include <sila/client/dynamic/ConstraintChecker.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>
#include <sila/client/dynamic/JsonSchemaSupport.h>
#include <sila/common/types/BasicTypes.h>
#include <sila/common/types/Constraints.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <google/protobuf/dynamic_message.h>

namespace sila2 {
namespace dynamic {

namespace {

using FieldDescriptor = google::protobuf::FieldDescriptor;
using Message = google::protobuf::Message;

struct FieldRef {
    const Message* owner = nullptr;
    const FieldDescriptor* field = nullptr;
    int index = -1;  // Set for one item of a repeated field.
};

std::optional<std::string> unsupported(std::string_view what) {
    return std::string{"Unsupported value validation: "} + std::string{what};
}

bool validFieldIndex(const FieldRef& ref, std::string& error) {
    if (ref.owner == nullptr || ref.field == nullptr) {
        error = "Value field reference is incomplete";
        return false;
    }
    if (ref.field->containing_type() != ref.owner->GetDescriptor()) {
        error = "field descriptor does not belong to the protobuf message";
        return false;
    }
    const auto* reflection = ref.owner->GetReflection();
    if (!ref.field->is_repeated() && ref.index >= 0) {
        error = "Singular field has a repeated index";
        return false;
    }
    if (ref.field->is_repeated() &&
        (ref.index < 0 || ref.index >= reflection->FieldSize(*ref.owner, ref.field))) {
        error = "Repeated field index is out of range";
        return false;
    }
    return true;
}

const Message* messageAt(const FieldRef& ref, std::string* error) {
    std::string reason;
    if (!validFieldIndex(ref, reason)) {
        if (error != nullptr) *error = reason;
        return nullptr;
    }
    if (ref.field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
        if (error != nullptr) *error = "field is not a protobuf message";
        return nullptr;
    }
    const auto* reflection = ref.owner->GetReflection();
    if (ref.field->is_repeated()) {
        return &reflection->GetRepeatedMessage(*ref.owner, ref.field, ref.index);
    }
    // Proto3 gives singular message fields explicit presence, and every FDL
    // Structure Element is mandatory (DataTypes.xsd:54-58), so an absent
    // submessage is a missing SiLA value rather than an empty one.
    if (!reflection->HasField(*ref.owner, ref.field)) {
        if (error != nullptr) *error = "message field is not present";
        return nullptr;
    }
    return &reflection->GetMessage(*ref.owner, ref.field);
}

// The IR keeps the original XML spelling because numeric constraints can be
// wider than the legacy double member.  Decimal is therefore deliberately a
// small lexical decimal (signed digits times 10^scale), not a native number.
struct Decimal {
    bool negative = false;
    std::string digits = "0";  // no leading or trailing zeroes, except "0"
    std::int64_t scale = 0;
};

std::int64_t saturatingAdd(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

std::optional<std::int64_t> parseExponent(std::string_view text) {
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    // from_chars stops at the first non-digit instead of failing, so the digit
    // check has to be explicit; an absurd exponent saturates rather than wraps
    // because the FDL text is attacker-controlled.
    if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t magnitude = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), magnitude);
    if (parsed.ec == std::errc::result_out_of_range) {
        return negative ? std::numeric_limits<std::int64_t>::min()
                        : std::numeric_limits<std::int64_t>::max();
    }
    if (parsed.ec != std::errc{}) return std::nullopt;
    return negative ? -magnitude : magnitude;
}

void normalizeDecimal(Decimal& value) {
    const auto first = value.digits.find_first_not_of('0');
    if (first == std::string::npos) {
        value.negative = false;
        value.digits = "0";
        value.scale = 0;
        return;
    }
    if (first != 0) value.digits.erase(0, first);
    std::size_t trailing = value.digits.size();
    while (trailing > 1 && value.digits[trailing - 1] == '0') --trailing;
    if (trailing != value.digits.size()) {
        const auto removed = value.digits.size() - trailing;
        value.digits.resize(trailing);
        value.scale = saturatingAdd(value.scale, static_cast<std::int64_t>(removed));
    }
}

std::optional<Decimal> parseDecimal(std::string_view text) {
    if (text.empty()) return std::nullopt;
    Decimal result;
    if (text.front() == '+' || text.front() == '-') {
        result.negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) return std::nullopt;

    const auto exponentMarker = text.find_first_of("eE");
    std::string_view mantissa = text;
    std::optional<std::int64_t> exponent;
    if (exponentMarker != std::string_view::npos) {
        mantissa = text.substr(0, exponentMarker);
        exponent = parseExponent(text.substr(exponentMarker + 1));
        if (!exponent.has_value()) return std::nullopt;
    }
    const auto dot = mantissa.find('.');
    if (dot != mantissa.rfind('.')) return std::nullopt;
    const auto integerPart = dot == std::string_view::npos ? mantissa : mantissa.substr(0, dot);
    const auto fractionPart = dot == std::string_view::npos ? std::string_view{}
                                                              : mantissa.substr(dot + 1);
    if (integerPart.empty() && fractionPart.empty()) return std::nullopt;
    for (const char character : integerPart) {
        if (character < '0' || character > '9') return std::nullopt;
    }
    for (const char character : fractionPart) {
        if (character < '0' || character > '9') return std::nullopt;
    }
    result.digits.reserve(integerPart.size() + fractionPart.size());
    result.digits.append(integerPart);
    result.digits.append(fractionPart);
    if (result.digits.empty()) return std::nullopt;
    result.scale = -static_cast<std::int64_t>(fractionPart.size());
    if (exponent.has_value()) result.scale = saturatingAdd(result.scale, *exponent);
    normalizeDecimal(result);
    return result;
}

std::optional<Decimal> parseInteger(std::string_view text) {
    if (text.empty()) return std::nullopt;
    Decimal result;
    if (text.front() == '+' || text.front() == '-') {
        result.negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) return std::nullopt;
    for (const char character : text) {
        if (character < '0' || character > '9') return std::nullopt;
    }
    result.digits = std::string{text};
    normalizeDecimal(result);
    return result;
}

std::int64_t decimalPosition(const Decimal& value) {
    const auto size = static_cast<std::int64_t>(value.digits.size());
    return saturatingAdd(value.scale, size);
}

int compareMagnitude(const Decimal& left, const Decimal& right) {
    const auto leftPosition = decimalPosition(left);
    const auto rightPosition = decimalPosition(right);
    if (leftPosition != rightPosition) return leftPosition < rightPosition ? -1 : 1;
    const auto length = std::max(left.digits.size(), right.digits.size());
    for (std::size_t index = 0; index < length; ++index) {
        const char leftDigit = index < left.digits.size() ? left.digits[index] : '0';
        const char rightDigit = index < right.digits.size() ? right.digits[index] : '0';
        if (leftDigit != rightDigit) return leftDigit < rightDigit ? -1 : 1;
    }
    return 0;
}

int compareDecimal(const Decimal& left, const Decimal& right) {
    const bool leftZero = left.digits == "0";
    const bool rightZero = right.digits == "0";
    if (leftZero && rightZero) return 0;
    if (left.negative != right.negative) return left.negative ? -1 : 1;
    const int magnitude = compareMagnitude(left, right);
    return left.negative ? -magnitude : magnitude;
}

enum class SpecialNumber { Finite, PositiveInfinity, NegativeInfinity, NaN };

struct NumericLexical {
    SpecialNumber special = SpecialNumber::Finite;
    Decimal decimal;
};

std::optional<NumericLexical> parseNumericLexical(std::string_view text) {
    if (text == "INF" || text == "+INF") {
        return NumericLexical{SpecialNumber::PositiveInfinity, {}};
    }
    if (text == "-INF") return NumericLexical{SpecialNumber::NegativeInfinity, {}};
    if (text == "NaN") return NumericLexical{SpecialNumber::NaN, {}};
    const auto decimal = parseDecimal(text);
    if (!decimal.has_value()) return std::nullopt;
    return NumericLexical{SpecialNumber::Finite, *decimal};
}

struct RealLexical {
    SpecialNumber special = SpecialNumber::Finite;
    double value = 0;
};

std::optional<RealLexical> parseRealLexical(std::string_view text) {
    if (text == "INF" || text == "+INF") {
        return RealLexical{SpecialNumber::PositiveInfinity,
                           std::numeric_limits<double>::infinity()};
    }
    if (text == "-INF") {
        return RealLexical{SpecialNumber::NegativeInfinity,
                           -std::numeric_limits<double>::infinity()};
    }
    if (text == "NaN") {
        return RealLexical{SpecialNumber::NaN, std::numeric_limits<double>::quiet_NaN()};
    }
    const auto decimal = parseDecimal(text);
    if (!decimal.has_value()) return std::nullopt;
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return std::nullopt;
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (parsed.ptr != text.data() + text.size()) return std::nullopt;
    if (parsed.ec == std::errc{}) {
        if (!std::isfinite(value)) return std::nullopt;
        return RealLexical{SpecialNumber::Finite, value};
    }
    if (parsed.ec != std::errc::result_out_of_range) return std::nullopt;

    // XML Schema's doubleLexicalMap rounds every valid numeral: too large
    // becomes an infinity, too small a signed zero.  from_chars reports both as
    // result_out_of_range without exposing which, and those are the only two
    // cases it can mean.  Overflow needs |v| >= ~1.798e308, whose decimal
    // position is always >= 309, and any representable value at that position
    // already returned above -- so the position alone separates them.
    if (decimalPosition(*decimal) >= 309) {
        return RealLexical{decimal->negative ? SpecialNumber::NegativeInfinity
                                             : SpecialNumber::PositiveInfinity,
                           decimal->negative ? -std::numeric_limits<double>::infinity()
                                             : std::numeric_limits<double>::infinity()};
    }
    return RealLexical{SpecialNumber::Finite, decimal->negative ? -0.0 : 0.0};
}

struct TemporalValue {
    BasicType type = BasicType::Date;
    std::int64_t instantMilliseconds = 0;
};

bool fixedDigits(std::string_view text, std::size_t position, std::size_t count,
                 std::uint32_t& result) {
    if (position + count > text.size()) return false;
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char character = text[position + index];
        if (character < '0' || character > '9') return false;
        value = value * 10U + static_cast<std::uint32_t>(character - '0');
    }
    result = value;
    return true;
}

bool leapYear(std::uint32_t year) {
    return year % 400U == 0U || (year % 4U == 0U && year % 100U != 0U);
}

bool validDate(std::uint32_t year, std::uint32_t month, std::uint32_t day) {
    if (year == 0 || year > 9999U || month == 0 || month > 12 || day == 0) return false;
    constexpr std::uint32_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const auto maximum = days[month - 1] + (month == 2 && leapYear(year) ? 1U : 0U);
    return day <= maximum;
}

std::optional<std::int32_t> parseTimezone(std::string_view text) {
    if (text == "Z") return 0;
    if (text.size() != 6 || (text.front() != '+' && text.front() != '-') || text[3] != ':') {
        return std::nullopt;
    }
    std::uint32_t hours = 0;
    std::uint32_t minutes = 0;
    if (!fixedDigits(text, 1, 2, hours) || !fixedDigits(text, 4, 2, minutes) ||
        hours > 14U || minutes > 59U || (hours == 14U && minutes != 0U)) {
        return std::nullopt;
    }
    const auto total = static_cast<std::int32_t>(hours * 60U + minutes);
    return text.front() == '-' ? -total : total;
}

std::int64_t daysFromCivil(std::uint32_t year, std::uint32_t month, std::uint32_t day) {
    // Howard Hinnant's proleptic Gregorian conversion.
    const std::int64_t y = static_cast<std::int64_t>(year) - (month <= 2U ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yearOfEra = static_cast<std::uint32_t>(y - era * 400);
    const auto shiftedMonth = static_cast<std::int64_t>(month) + (month > 2U ? -3 : 9);
    const auto dayOfYear = static_cast<std::uint32_t>((153 * shiftedMonth + 2) / 5 + day - 1);
    const auto dayOfEra = yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

constexpr std::int64_t kMillisecondsPerDay = 86400000;

std::optional<TemporalValue> parseDateLexical(std::string_view text) {
    std::uint32_t year = 0, month = 0, day = 0;
    if (text.size() != 11 && text.size() != 16) return std::nullopt;
    if (!fixedDigits(text, 0, 4, year) || text[4] != '-' || !fixedDigits(text, 5, 2, month) ||
        text[7] != '-' || !fixedDigits(text, 8, 2, day) || !validDate(year, month, day)) {
        return std::nullopt;
    }
    const auto timezone = parseTimezone(text.substr(10));
    if (!timezone.has_value()) return std::nullopt;
    const auto milliseconds = daysFromCivil(year, month, day) * kMillisecondsPerDay -
                              static_cast<std::int64_t>(*timezone) * 60000;
    return TemporalValue{BasicType::Date, milliseconds};
}

bool parseClock(std::string_view text, std::uint32_t& hour, std::uint32_t& minute,
                std::uint32_t& second, std::uint32_t& millisecond) {
    // ponytail: 24:00:00 (xs:time's midnight alias) and fractional seconds
    // beyond 3 digits are rejected -- SiLAFramework.proto:43 stores only a
    // uint32 millisecond, so neither is representable on the wire.  Upgrade
    // path is a wider temporal value type, not a looser parser.
    if (text.size() != 8 && text.size() != 12) return false;
    if (!fixedDigits(text, 0, 2, hour) || text[2] != ':' || !fixedDigits(text, 3, 2, minute) ||
        text[5] != ':' || !fixedDigits(text, 6, 2, second) || hour > 23U || minute > 59U ||
        second > 59U) {
        return false;
    }
    millisecond = 0;
    if (text.size() == 12) {
        if (text[8] != '.' || !fixedDigits(text, 9, 3, millisecond)) return false;
    }
    return true;
}

std::optional<TemporalValue> parseTimeLexical(std::string_view text) {
    if (text.size() != 9 && text.size() != 14 && text.size() != 13 && text.size() != 18) {
        return std::nullopt;
    }
    const auto timezoneStart = text.back() == 'Z' ? text.size() - 1 : text.size() - 6;
    std::uint32_t hour = 0, minute = 0, second = 0, millisecond = 0;
    if (!parseClock(text.substr(0, timezoneStart), hour, minute, second, millisecond)) {
        return std::nullopt;
    }
    const auto timezone = parseTimezone(text.substr(timezoneStart));
    if (!timezone.has_value()) return std::nullopt;
    const auto milliseconds = ((static_cast<std::int64_t>(hour) * 60 + minute) * 60 + second) *
                                  1000 + millisecond - static_cast<std::int64_t>(*timezone) * 60000;
    return TemporalValue{BasicType::Time, milliseconds};
}

std::optional<TemporalValue> parseTimestampLexical(std::string_view text) {
    if (text.size() != 20 && text.size() != 25 && text.size() != 24 && text.size() != 29) {
        return std::nullopt;
    }
    if (text[10] != 'T') return std::nullopt;
    std::uint32_t year = 0, month = 0, day = 0;
    if (!fixedDigits(text, 0, 4, year) || text[4] != '-' || !fixedDigits(text, 5, 2, month) ||
        text[7] != '-' || !fixedDigits(text, 8, 2, day) || !validDate(year, month, day)) {
        return std::nullopt;
    }
    const auto timezoneStart = text.back() == 'Z' ? text.size() - 1 : text.size() - 6;
    std::uint32_t hour = 0, minute = 0, second = 0, millisecond = 0;
    if (!parseClock(text.substr(11, timezoneStart - 11), hour, minute, second, millisecond)) {
        return std::nullopt;
    }
    const auto timezone = parseTimezone(text.substr(timezoneStart));
    if (!timezone.has_value()) return std::nullopt;
    const auto dateMilliseconds = daysFromCivil(year, month, day) * kMillisecondsPerDay;
    const auto timeMilliseconds = ((static_cast<std::int64_t>(hour) * 60 + minute) * 60 + second) *
                                      1000 + millisecond;
    return TemporalValue{BasicType::Timestamp,
                         dateMilliseconds + timeMilliseconds - static_cast<std::int64_t>(*timezone) * 60000};
}

struct BasicValue {
    std::string string;
    std::int64_t integer = 0;
    double real = 0;
    bool boolean = false;
    TemporalValue temporal;
};

const Message* wrapperFor(const FieldRef& ref, std::string_view fullName, std::string& error) {
    if (!validFieldIndex(ref, error)) return nullptr;
    if (ref.owner->GetDescriptor()->full_name() == fullName && ref.index < 0) return ref.owner;
    const auto* wrapper = messageAt(ref, &error);
    if (wrapper == nullptr) return nullptr;
    if (wrapper->GetDescriptor()->full_name() != fullName) {
        error = "Protobuf message type does not match the FDL basic type";
        return nullptr;
    }
    return wrapper;
}

const FieldDescriptor* wrapperValueField(const Message& wrapper, BasicType expected,
                                         std::string& error) {
    const auto* valueField = wrapper.GetDescriptor()->FindFieldByName("value");
    if (valueField == nullptr || valueField->is_repeated()) {
        error = "Protobuf message has no value field for the FDL basic type";
        return nullptr;
    }
    const auto expectedType = [&] {
        switch (expected) {
        case BasicType::String: return FieldDescriptor::TYPE_STRING;
        case BasicType::Binary: return FieldDescriptor::TYPE_BYTES;
        case BasicType::Integer: return FieldDescriptor::TYPE_INT64;
        case BasicType::Real: return FieldDescriptor::TYPE_DOUBLE;
        case BasicType::Boolean: return FieldDescriptor::TYPE_BOOL;
        default: return FieldDescriptor::TYPE_MESSAGE;
        }
    }();
    if (valueField->type() != expectedType) {
        error = "Protobuf message value field does not match the FDL basic type";
        return nullptr;
    }
    return valueField;
}

std::optional<std::int32_t> timezoneMinutes(const Message& wrapper, std::string_view fieldName,
                                            std::string& error) {
    const auto* timezoneField = wrapper.GetDescriptor()->FindFieldByName(std::string{fieldName});
    if (timezoneField == nullptr || timezoneField->is_repeated() ||
        timezoneField->type() != FieldDescriptor::TYPE_MESSAGE) {
        error = "Temporal protobuf message has no Timezone field";
        return std::nullopt;
    }
    const auto* reflection = wrapper.GetReflection();
    if (!reflection->HasField(wrapper, timezoneField)) {
        error = "Temporal value has no timezone";
        return std::nullopt;
    }
    const auto& timezone = reflection->GetMessage(wrapper, timezoneField);
    if (timezone.GetDescriptor()->full_name() != "sila2.org.silastandard.Timezone") {
        error = "Temporal timezone has an unexpected protobuf type";
        return std::nullopt;
    }
    const auto* hoursField = timezone.GetDescriptor()->FindFieldByName("hours");
    const auto* minutesField = timezone.GetDescriptor()->FindFieldByName("minutes");
    if (hoursField == nullptr || minutesField == nullptr || hoursField->is_repeated() ||
        minutesField->is_repeated() || hoursField->type() != FieldDescriptor::TYPE_INT32 ||
        minutesField->type() != FieldDescriptor::TYPE_UINT32) {
        error = "Temporal timezone has an invalid protobuf shape";
        return std::nullopt;
    }
    const auto hours = timezone.GetReflection()->GetInt32(timezone, hoursField);
    const auto minutes = timezone.GetReflection()->GetUInt32(timezone, minutesField);
    // SiLAFramework.proto:123-126 makes minutes unsigned, so a negative offset
    // has to carry its sign in hours alone: -02:30 is {hours:-3, minutes:30}.
    // The XML Schema +-14:00 limit therefore applies to the sum, not to hours.
    const auto total = static_cast<std::int64_t>(hours) * 60 + minutes;
    if (minutes > 59U || total < -14 * 60 || total > 14 * 60) {
        error = "Temporal timezone is outside the XML Schema range";
        return std::nullopt;
    }
    return static_cast<std::int32_t>(total);
}

const FieldDescriptor* temporalField(const Message& wrapper, std::string_view name,
                                     FieldDescriptor::Type type, std::string& error) {
    const auto* field = wrapper.GetDescriptor()->FindFieldByName(std::string{name});
    if (field == nullptr || field->is_repeated() || field->type() != type) {
        error = "Temporal protobuf message has an invalid field shape";
        return nullptr;
    }
    return field;
}

std::optional<TemporalValue> readTemporal(const FieldRef& ref, BasicType expected,
                                          std::string& error) {
    const std::string_view fullName = expected == BasicType::Date
                                          ? "sila2.org.silastandard.Date"
                                          : expected == BasicType::Time
                                                ? "sila2.org.silastandard.Time"
                                                : "sila2.org.silastandard.Timestamp";
    const auto* wrapper = wrapperFor(ref, fullName, error);
    if (wrapper == nullptr) return std::nullopt;
    const auto* reflection = wrapper->GetReflection();
    const auto getUint32 = [&](std::string_view name, std::uint32_t& result) {
        const auto* field = temporalField(*wrapper, name, FieldDescriptor::TYPE_UINT32, error);
        if (field == nullptr) return false;
        result = reflection->GetUInt32(*wrapper, field);
        return true;
    };
    std::uint32_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millisecond = 0;
    std::int32_t offset = 0;
    if (expected == BasicType::Date) {
        if (!getUint32("year", year) || !getUint32("month", month) || !getUint32("day", day)) {
            return std::nullopt;
        }
        const auto timezone = timezoneMinutes(*wrapper, "timezone", error);
        if (!timezone.has_value()) return std::nullopt;
        offset = *timezone;
        if (!validDate(year, month, day)) {
            error = "Date value is outside the XML Schema calendar range";
            return std::nullopt;
        }
        return TemporalValue{BasicType::Date,
                             daysFromCivil(year, month, day) * kMillisecondsPerDay -
                                 static_cast<std::int64_t>(offset) * 60000};
    }
    if (!getUint32("hour", hour) || !getUint32("minute", minute) ||
        !getUint32("second", second) || !getUint32("millisecond", millisecond)) {
        return std::nullopt;
    }
    const auto timezone = timezoneMinutes(*wrapper, "timezone", error);
    if (!timezone.has_value()) return std::nullopt;
    offset = *timezone;
    if (hour > 23U || minute > 59U || second > 59U || millisecond > 999U) {
        error = "Time value is outside the XML Schema clock range";
        return std::nullopt;
    }
    const auto clock = ((static_cast<std::int64_t>(hour) * 60 + minute) * 60 + second) * 1000 +
                       millisecond - static_cast<std::int64_t>(offset) * 60000;
    if (expected == BasicType::Time) return TemporalValue{BasicType::Time, clock};
    if (!getUint32("year", year) || !getUint32("month", month) || !getUint32("day", day) ||
        !validDate(year, month, day)) {
        error = "Timestamp value is outside the XML Schema calendar range";
        return std::nullopt;
    }
    return TemporalValue{BasicType::Timestamp, daysFromCivil(year, month, day) * kMillisecondsPerDay + clock};
}

std::optional<std::string> readBasicValue(const FieldRef& ref, BasicType expected, BasicValue& value) {
    std::string error;
    if (!validFieldIndex(ref, error)) return error;
    const auto* reflection = ref.owner->GetReflection();
    const auto getString = [&]() {
        value.string = ref.field->is_repeated()
                           ? reflection->GetRepeatedString(*ref.owner, ref.field, ref.index)
                           : reflection->GetString(*ref.owner, ref.field);
    };
    const auto getInteger = [&]() {
        value.integer = ref.field->is_repeated()
                            ? reflection->GetRepeatedInt64(*ref.owner, ref.field, ref.index)
                            : reflection->GetInt64(*ref.owner, ref.field);
    };
    const auto getReal = [&]() {
        value.real = ref.field->is_repeated()
                         ? reflection->GetRepeatedDouble(*ref.owner, ref.field, ref.index)
                         : reflection->GetDouble(*ref.owner, ref.field);
    };
    const auto getBoolean = [&]() {
        value.boolean = ref.field->is_repeated()
                            ? reflection->GetRepeatedBool(*ref.owner, ref.field, ref.index)
                            : reflection->GetBool(*ref.owner, ref.field);
    };

    if (expected == BasicType::Date || expected == BasicType::Time || expected == BasicType::Timestamp) {
        if (const auto temporal = readTemporal(ref, expected, error); temporal.has_value()) {
            value.temporal = *temporal;
            return std::nullopt;
        }
        return error;
    }

    if (ref.field->cpp_type() == FieldDescriptor::CPPTYPE_STRING) {
        const auto expectedType = expected == BasicType::Binary ? FieldDescriptor::TYPE_BYTES
                                                                  : FieldDescriptor::TYPE_STRING;
        if (ref.field->type() != expectedType ||
            (expected != BasicType::String && expected != BasicType::Binary)) {
            return std::string{"Protobuf scalar type does not match the FDL basic type"};
        }
        if (expected == BasicType::Binary && ref.field->containing_oneof() != nullptr) {
            const auto* selected = reflection->GetOneofFieldDescriptor(
                *ref.owner, ref.field->containing_oneof());
            if (selected == nullptr) {
                return std::string{"Binary value has neither inline bytes nor a transfer UUID"};
            }
            if (selected != ref.field) {
                return std::string{"Binary value is represented by a transfer UUID, not inline bytes"};
            }
        }
        getString();
        if (expected == BasicType::String) {
            return types::checkMaximalLength(value.string, 2U * 1024U * 1024U);
        }
        return std::nullopt;
    }
    if (ref.field->type() == FieldDescriptor::TYPE_INT64 && expected == BasicType::Integer) {
        getInteger();
        return std::nullopt;
    }
    if (ref.field->type() == FieldDescriptor::TYPE_DOUBLE && expected == BasicType::Real) {
        getReal();
        return std::nullopt;
    }
    if (ref.field->type() == FieldDescriptor::TYPE_BOOL && expected == BasicType::Boolean) {
        getBoolean();
        return std::nullopt;
    }

    const std::string_view fullName = expected == BasicType::String
                                          ? "sila2.org.silastandard.String"
                                          : expected == BasicType::Integer
                                                ? "sila2.org.silastandard.Integer"
                                                : expected == BasicType::Real
                                                      ? "sila2.org.silastandard.Real"
                                                      : expected == BasicType::Boolean
                                                            ? "sila2.org.silastandard.Boolean"
                                                            : "sila2.org.silastandard.Binary";
    const auto* wrapper = wrapperFor(ref, fullName, error);
    if (wrapper == nullptr) return error;
    const auto* valueField = wrapperValueField(*wrapper, expected, error);
    if (valueField == nullptr) return error;
    if (expected == BasicType::Binary && valueField->containing_oneof() == nullptr) {
        return std::string{"Binary value field is not part of the Binary oneof"};
    }
    if (valueField->containing_oneof() != nullptr) {
        const auto* selected = wrapper->GetReflection()->GetOneofFieldDescriptor(
            *wrapper, valueField->containing_oneof());
        if (expected == BasicType::Binary && selected == nullptr) {
            return std::string{"Binary value has neither inline bytes nor a transfer UUID"};
        }
        if (selected != nullptr && selected != valueField) {
            return std::string{"Binary value is represented by a transfer UUID, not inline bytes"};
        }
    }
    const auto* wrapperReflection = wrapper->GetReflection();
    if (expected == BasicType::String || expected == BasicType::Binary) {
        value.string = wrapperReflection->GetString(*wrapper, valueField);
        if (expected == BasicType::String) {
            return types::checkMaximalLength(value.string, 2U * 1024U * 1024U);
        }
    } else if (expected == BasicType::Integer) {
        value.integer = wrapperReflection->GetInt64(*wrapper, valueField);
    } else if (expected == BasicType::Real) {
        value.real = wrapperReflection->GetDouble(*wrapper, valueField);
    } else {
        value.boolean = wrapperReflection->GetBool(*wrapper, valueField);
    }
    return std::nullopt;
}

const DataType* resolveBase(const DataType& dataType, const DataTypeResolver& resolver,
                            std::size_t depth, std::string& error) {
    if (depth > 64) {
        error = "DataType identifier resolution is cyclic or too deep";
        return nullptr;
    }
    return std::visit(
        [&](const auto& value) -> const DataType* {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, DataType::Constrained>) {
                if (value.inner == nullptr) {
                    error = "Constrained DataType has no inner DataType";
                    return nullptr;
                }
                return resolveBase(*value.inner, resolver, depth + 1, error);
            } else if constexpr (std::is_same_v<T, DataType::Identifier>) {
                if (!resolver) {
                    error = "DataType identifier has no resolver: " + value.typeId;
                    return nullptr;
                }
                const auto* resolved = resolver(value.typeId);
                if (resolved == nullptr) {
                    error = "DataType identifier is not defined: " + value.typeId;
                    return nullptr;
                }
                return resolveBase(*resolved, resolver, depth + 1, error);
            } else {
                return &dataType;
            }
        },
        dataType.value);
}

std::optional<std::string> unwrapIdentifier(const DataType::Identifier& identifier,
                                            const FieldRef& ref, FieldRef& unwrapped) {
    std::string error;
    const Message* wrapper = messageAt(ref, &error);
    if (wrapper == nullptr) {
        // A direct scalar descriptor is a useful small resolver target in
        // tests and in callers that do not use DescriptorBuilder wrappers.
        if (ref.field != nullptr && ref.field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
            unwrapped = ref;
            return std::nullopt;
        }
        return error;
    }
    if (const auto* named = wrapper->GetDescriptor()->FindFieldByName(identifier.typeId);
        named != nullptr) {
        unwrapped = FieldRef{wrapper, named, -1};
        return std::nullopt;
    }
    // A resolver can intentionally map an identifier to a framework basic
    // type while the caller supplies a compiled-in String/Binary wrapper.
    if (wrapper->GetDescriptor()->FindFieldByName("value") != nullptr) {
        unwrapped = ref;
        return std::nullopt;
    }
    return std::string{"DataType identifier wrapper has no field named "} + identifier.typeId;
}

std::optional<std::string> checkBinaryLength(std::size_t actual, ConstraintValue::Kind kind,
                                             std::size_t expected) {
    if ((kind == ConstraintValue::Length && actual == expected) ||
        (kind == ConstraintValue::MinLength && actual >= expected) ||
        (kind == ConstraintValue::MaxLength && actual <= expected)) {
        return std::nullopt;
    }
    if (kind == ConstraintValue::Length) {
        return "Binary length is " + std::to_string(actual) + ", expected exactly " +
               std::to_string(expected);
    }
    if (kind == ConstraintValue::MinLength) {
        return "Binary length is " + std::to_string(actual) + ", minimum is " +
               std::to_string(expected);
    }
    return "Binary length is " + std::to_string(actual) + ", maximum is " +
           std::to_string(expected);
}

// Mirrors parseSizeConstraint's priority (ConstraintChecker.h): numericValue
// is a lossy legacy double view of the bound, so an empty lexical/string pair
// fails parsing below rather than silently reformatting through %.6f and
// shifting the bound.
std::string_view constraintText(const ConstraintValue& constraint) {
    return constraint.lexicalValue.empty() ? std::string_view{constraint.stringValue}
                                           : std::string_view{constraint.lexicalValue};
}

bool realSetEqual(double actual, const RealLexical& candidate) {
    if (std::isnan(actual)) return candidate.special == SpecialNumber::NaN;
    if (candidate.special == SpecialNumber::NaN) return false;
    return actual == candidate.value;  // XML Schema treats +0 and -0 as equal.
}

std::optional<std::string> realBoundViolation(double actual, ConstraintValue::Kind kind,
                                              const RealLexical& bound) {
    // XML Schema orders NaN against nothing, so any bound involving it fails.
    if (std::isnan(actual) || bound.special == SpecialNumber::NaN) {
        return std::string{"Real value violates its numeric bound"};
    }
    switch (kind) {
    case ConstraintValue::MinInclusive: return types::checkMinimalInclusive(actual, bound.value);
    case ConstraintValue::MaxInclusive: return types::checkMaximalInclusive(actual, bound.value);
    case ConstraintValue::MinExclusive: return types::checkMinimalExclusive(actual, bound.value);
    default: return types::checkMaximalExclusive(actual, bound.value);
    }
}

bool decimalBoundPass(const Decimal& actual, ConstraintValue::Kind kind,
                      const NumericLexical& bound) {
    if (bound.special == SpecialNumber::NaN) return false;
    if (bound.special == SpecialNumber::PositiveInfinity) {
        return kind == ConstraintValue::MaxInclusive || kind == ConstraintValue::MaxExclusive;
    }
    if (bound.special == SpecialNumber::NegativeInfinity) {
        return kind == ConstraintValue::MinInclusive || kind == ConstraintValue::MinExclusive;
    }
    const auto comparison = compareDecimal(actual, bound.decimal);
    switch (kind) {
    case ConstraintValue::MinInclusive: return comparison >= 0;
    case ConstraintValue::MaxInclusive: return comparison <= 0;
    case ConstraintValue::MinExclusive: return comparison > 0;
    case ConstraintValue::MaxExclusive: return comparison < 0;
    default: return false;
    }
}

std::optional<TemporalValue> parseTemporalLexical(std::string_view text, BasicType type) {
    switch (type) {
    case BasicType::Date: return parseDateLexical(text);
    case BasicType::Time: return parseTimeLexical(text);
    case BasicType::Timestamp: return parseTimestampLexical(text);
    default: return std::nullopt;
    }
}

// Constraints.xsd:28-33 lists Set as applicable to String, Integer, Real, Date,
// Time and Timestamp only -- Binary, Boolean and Any fall through to the
// unsupported tail.  Integer uses the xs:integer lexical form there, unlike the
// xs:double form the numeric bounds use (Constraints.xsd:42-73).
std::optional<std::string> setViolation(BasicType type, const BasicValue& value,
                                        const std::vector<std::string>& allowed) {
    if (type == BasicType::String) return types::checkSet(value.string, allowed);
    if (type == BasicType::Integer) {
        const auto actual = parseInteger(std::to_string(value.integer));
        if (!actual.has_value()) return std::string{"Invalid protobuf Integer value"};
        for (const auto& item : allowed) {
            const auto candidate = parseInteger(item);
            if (!candidate.has_value()) return std::string{"Invalid Integer Set value"};
            if (compareDecimal(*actual, *candidate) == 0) return std::nullopt;
        }
        return std::string{"Integer value is not contained in Set"};
    }
    if (type == BasicType::Real) {
        for (const auto& item : allowed) {
            const auto candidate = parseRealLexical(item);
            if (!candidate.has_value()) return std::string{"Invalid Real Set value"};
            if (realSetEqual(value.real, *candidate)) return std::nullopt;
        }
        return std::string{"Real value is not contained in Set"};
    }
    if (type == BasicType::Date || type == BasicType::Time || type == BasicType::Timestamp) {
        for (const auto& item : allowed) {
            const auto candidate = parseTemporalLexical(item, type);
            if (!candidate.has_value()) return std::string{"Invalid temporal Set value"};
            if (candidate->instantMilliseconds == value.temporal.instantMilliseconds) {
                return std::nullopt;
            }
        }
        return std::string{"Temporal value is not contained in Set"};
    }
    return unsupported("Set on unsupported basic type");
}

std::optional<std::string> checkConstraints(const DataType& base,
                                            const std::vector<ConstraintValue>& constraints,
                                            const FieldRef& ref,
                                            const ConstraintResolver& constraintResolver) {
    const auto* basic = std::get_if<DataType::Basic>(&base.value);
    const bool isList = std::holds_alternative<DataType::List>(base.value);
    bool delegatedConstraintSeen = false;

    BasicValue value;
    bool valueRead = false;
    const auto readValue = [&]() -> std::optional<std::string> {
        if (!valueRead) {
            if (const auto error = readBasicValue(ref, basic->type, value)) return error;
            valueRead = true;
        }
        return std::nullopt;
    };

    for (const auto& constraint : constraints) {
        switch (constraint.kind) {
        case ConstraintValue::Unit:
        case ConstraintValue::ContentType:
        case ConstraintValue::Schema:
        case ConstraintValue::AllowedTypes: {
            // No base-type applicability gate here, unlike the arms below:
            // applicability of the four caller-resolved kinds is the caller's
            // contract, not this validator's. parseFdl output has already
            // passed fdl-validation.xsl (which terminates on a mis-applied
            // Unit/ContentType/Schema/AllowedTypes); the only IR that skips
            // that check -- hand-built IR and the XSLT-skipping parseDataTypeXml
            // entry -- is not routed here in production.
            delegatedConstraintSeen = true;
            if (!constraintResolver) {
                return unsupported("constraint requires a ConstraintResolver");
            }
            std::string referenceError;
            if (!validFieldIndex(ref, referenceError)) return referenceError;
            if (const auto error = constraintResolver(
                    constraint, *ref.owner, *ref.field, ref.index)) {
                return error;
            }
            continue;
        }
        default:
            break;
        }
        if (basic == nullptr && !isList) return unsupported("constraint target data type");
        if (isList) {
            if (constraint.kind != ConstraintValue::ElementCount &&
                constraint.kind != ConstraintValue::MinElementCount &&
                constraint.kind != ConstraintValue::MaxElementCount) {
                return unsupported("constraint on List");
            }
            if (!ref.field->is_repeated() || ref.index >= 0) {
                return std::string{"List value is not represented by a repeated protobuf field"};
            }
            const auto count = static_cast<std::size_t>(
                ref.owner->GetReflection()->FieldSize(*ref.owner, ref.field));
            const auto expected = parseSizeConstraint(constraint);
            if (!expected.has_value()) return std::string{"Invalid element-count constraint"};
            std::optional<std::string> error;
            if (constraint.kind == ConstraintValue::ElementCount) {
                error = types::checkMinimalElementCount(count, *expected);
                if (!error) error = types::checkMaximalElementCount(count, *expected);
            } else if (constraint.kind == ConstraintValue::MinElementCount) {
                error = types::checkMinimalElementCount(count, *expected);
            } else {
                error = types::checkMaximalElementCount(count, *expected);
            }
            if (error) return error;
            continue;
        }

        switch (constraint.kind) {
        case ConstraintValue::Length:
        case ConstraintValue::MinLength:
        case ConstraintValue::MaxLength: {
            if (basic->type != BasicType::String && basic->type != BasicType::Binary) {
                return unsupported("length constraint on a non-string/non-binary type");
            }
            if (const auto error = readValue()) return error;
            const auto expected = parseSizeConstraint(constraint);
            if (!expected.has_value()) return std::string{"Invalid length constraint"};
            if (basic->type == BasicType::Binary) {
                if (const auto error = checkBinaryLength(value.string.size(), constraint.kind, *expected)) {
                    return error;
                }
            } else if (constraint.kind == ConstraintValue::Length) {
                if (const auto error = types::checkLength(value.string, *expected)) return error;
            } else if (constraint.kind == ConstraintValue::MinLength) {
                if (const auto error = types::checkMinimalLength(value.string, *expected)) return error;
            } else if (const auto error = types::checkMaximalLength(value.string, *expected)) {
                return error;
            }
            break;
        }
        case ConstraintValue::Pattern:
            if (basic->type != BasicType::String) return unsupported("Pattern on Binary");
            if (const auto error = readValue()) return error;
            // preparedPattern is compiled once per constraint (FdlRuntimeParser.cc)
            // and shared across every element of a repeated field; fall back to
            // the uncompiled path for hand-built ConstraintValue (e.g. tests) or
            // a pattern that failed to compile.
            if (constraint.preparedPattern) {
                if (const auto error = types::checkPattern(value.string, *constraint.preparedPattern,
                                                            constraint.stringValue)) {
                    return error;
                }
            } else if (const auto error = types::checkPattern(value.string, constraint.stringValue)) {
                return error;
            }
            break;
        case ConstraintValue::Set:
            if (const auto error = readValue()) return error;
            if (const auto error = setViolation(basic->type, value, constraint.stringValues)) {
                return error;
            }
            break;
        case ConstraintValue::FullyQualifiedIdentifier:
            if (basic->type != BasicType::String) return unsupported("FQI on Binary");
            if (const auto error = readValue()) return error;
            if (const auto error = checkFqiConstraint(constraint, value.string)) {
                return error;
            }
            break;
        case ConstraintValue::MinInclusive:
        case ConstraintValue::MaxInclusive:
        case ConstraintValue::MinExclusive:
        case ConstraintValue::MaxExclusive: {
            if (basic->type == BasicType::Boolean || basic->type == BasicType::String ||
                basic->type == BasicType::Binary) {
                return unsupported("numeric bound on a non-numeric type");
            }
            if (const auto error = readValue()) return error;
            const auto text = constraintText(constraint);
            if (basic->type == BasicType::Integer) {
                const auto actual = parseInteger(std::to_string(value.integer));
                const auto bound = parseNumericLexical(text);
                if (!actual.has_value() || !bound.has_value()) return std::string{"Invalid Integer bound"};
                if (!decimalBoundPass(*actual, constraint.kind, *bound)) {
                    return std::string{"Integer value violates its numeric bound"};
                }
            } else if (basic->type == BasicType::Real) {
                const auto bound = parseRealLexical(text);
                if (!bound.has_value()) return std::string{"Invalid Real bound"};
                if (const auto error = realBoundViolation(value.real, constraint.kind, *bound)) {
                    return error;
                }
            } else {
                const auto bound = parseTemporalLexical(text, basic->type);
                if (!bound.has_value()) return std::string{"Invalid temporal bound"};
                const auto actual = value.temporal.instantMilliseconds;
                const auto limit = bound->instantMilliseconds;
                // Both operands carry a timezone, so the UTC-normalised instant
                // is XML Schema's total order for date/time/dateTime.
                std::optional<std::string> violation;
                switch (constraint.kind) {
                case ConstraintValue::MinInclusive: violation = types::checkMinimalInclusive(actual, limit); break;
                case ConstraintValue::MaxInclusive: violation = types::checkMaximalInclusive(actual, limit); break;
                case ConstraintValue::MinExclusive: violation = types::checkMinimalExclusive(actual, limit); break;
                default: violation = types::checkMaximalExclusive(actual, limit); break;
                }
                if (violation) return violation;
            }
            break;
        }
        default:
            return unsupported("constraint kind is outside the runtime value-validation slice");
        }
    }
    if (basic == nullptr && !isList && !delegatedConstraintSeen) {
        return unsupported("constraint target data type");
    }
    return std::nullopt;
}

// Part A p64/p66: a SiLA Any Type MUST NOT carry a Custom Data Type, and a
// Constrained Type MUST be based on a Basic or List type (never a Structure).
// AnyTypeDataType.xsd (DataTypes.xsd DataTypeType choice) admits both a bare
// DataTypeIdentifier and a Constrained-over-Structure, so parseDataTypeXml
// alone is not enough -- reject them here before decoding the payload.
std::optional<std::string> rejectDisallowedAnyType(const DataType& type, std::size_t depth) {
    if (depth > 64) return std::string{"Any type XML nesting is too deep"};  // mirrors FdlRuntimeParser.cc:518
    return std::visit(
        [&](const auto& value) -> std::optional<std::string> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, DataType::Identifier>) {
                // A DataTypeIdentifier is a Custom Data Type reference (Part A p64).
                return std::string{"Any type must not reference a Custom Data Type: "} + value.typeId;
            } else if constexpr (std::is_same_v<T, DataType::Basic>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, DataType::List>) {
                if (value.elementType == nullptr) return std::string{"Any List type has no element type"};
                return rejectDisallowedAnyType(*value.elementType, depth + 1);
            } else if constexpr (std::is_same_v<T, DataType::Structure>) {
                for (const auto& element : value.elements) {
                    if (element.dataType == nullptr) return std::string{"Any Structure element has no type"};
                    if (const auto rejected = rejectDisallowedAnyType(*element.dataType, depth + 1)) return rejected;
                }
                return std::nullopt;
            } else {  // DataType::Constrained
                if (value.inner == nullptr) return std::string{"Any Constrained type has no base"};
                // Part A p66: peel Constrained layers to the type they apply to;
                // that base must not be a Structure.
                const DataType* base = value.inner.get();
                while (const auto* constrained = std::get_if<DataType::Constrained>(&base->value)) {
                    if (constrained->inner == nullptr) return std::string{"Any Constrained type has no base"};
                    base = constrained->inner.get();
                }
                if (std::holds_alternative<DataType::Structure>(base->value)) {
                    return std::string{"Any Constrained type must not be based on a Structure"};
                }
                return rejectDisallowedAnyType(*value.inner, depth + 1);
            }
        },
        type.value);
}

// Shared Any-read preamble (Part B p66: message Any{string type; bytes
// payload}). Reads the SiLAFramework.Any at ref, parses its type XML into S
// and applies the same semantic rejection as the value path (Part A
// p64/p66: no Custom, no Constrained-over-Structure). A Validation Error
// string on any malformed input, else fills out and returns nullopt.
// validateAnyValue (value-vs-S) and validateAllowedTypesImpl (S-vs-list)
// both call this so neither re-implements the preamble.
struct AnyContents {
    std::string typeXml;
    std::string payloadBytes;
    DataType type;  // S
};
std::optional<std::string> readWireAnyType(const FieldRef& ref, AnyContents& out) {
    std::string error;
    const Message* anyMessage = messageAt(ref, &error);
    if (anyMessage == nullptr) return error;
    // One shared Any-read (AnyCodec::readAnyFields): the server binary
    // interceptor reads an Any's type/payload the same way.
    if (!AnyCodec::readAnyFields(*anyMessage, out.typeXml, out.payloadBytes)) {
        return std::string{"Any value is not a SiLAFramework.Any message"};
    }
    try {
        out.type = parseDataTypeXml(out.typeXml);
    } catch (const std::exception& parseError) {
        return std::string{"Any type XML is malformed: "} + parseError.what();
    }
    if (const auto rejected = rejectDisallowedAnyType(out.type, 0)) return rejected;
    return std::nullopt;
}

// Deserializes the payload under S into the synthetic DataType_Payload
// message (Part B p66). pool/factory are caller-owned because each Any
// needs a fresh pool: DescriptorBuilder names the synthetic file by a hash
// of the type XML and BuildFile refuses a second file of the same name.
std::optional<std::string> decodeWireAnyPayload(
    const AnyContents& any, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory, std::unique_ptr<Message>& decoded,
    const FieldDescriptor*& payloadField) {
    try {
        sila2::types::AnyValue value;
        value.typeXml = any.typeXml;
        value.payload.assign(any.payloadBytes.begin(), any.payloadBytes.end());
        decoded = AnyCodec::decode(value, pool, &factory);
    } catch (const std::exception& decodeError) {
        return std::string{"Any payload does not match its type XML: "} + decodeError.what();
    }
    payloadField = decoded->GetDescriptor()->FindFieldByName("Payload");
    if (payloadField == nullptr) return std::string{"decoded Any is missing its Payload field"};
    return std::nullopt;
}

// Part A p66: Constrained layers do not change the underlying type, so peel
// them before comparing an Any's type against an allowed entry.
const DataType* stripConstrained(const DataType& type) {
    const DataType* current = &type;
    while (const auto* constrained = std::get_if<DataType::Constrained>(&current->value)) {
        if (constrained->inner == nullptr) break;
        current = constrained->inner.get();
    }
    return current;
}

// Part B p70: AllowedTypes equivalence is structural. After stripping
// Constrained on both sides, S matches T when both are the same Basic type,
// both are Lists with matching elements, or both are Structures with the
// same element identifiers in the same order (protobuf field numbers derive
// from element order, DescriptorBuilder.cc). Neither side can be a Custom
// identifier -- rejectDisallowedAnyType removes it from S and
// fdl-validation.xsl:368 forbids it in T -- so an Identifier or any other
// mismatch is a non-match.
bool skeletonMatch(const DataType& s, const DataType& t) {
    const DataType* left = stripConstrained(s);
    const DataType* right = stripConstrained(t);
    const auto* leftBasic = std::get_if<DataType::Basic>(&left->value);
    const auto* rightBasic = std::get_if<DataType::Basic>(&right->value);
    if (leftBasic != nullptr && rightBasic != nullptr) {
        return leftBasic->type == rightBasic->type;
    }
    const auto* leftList = std::get_if<DataType::List>(&left->value);
    const auto* rightList = std::get_if<DataType::List>(&right->value);
    if (leftList != nullptr && rightList != nullptr) {
        if (leftList->elementType == nullptr || rightList->elementType == nullptr) return false;
        return skeletonMatch(*leftList->elementType, *rightList->elementType);
    }
    const auto* leftStruct = std::get_if<DataType::Structure>(&left->value);
    const auto* rightStruct = std::get_if<DataType::Structure>(&right->value);
    if (leftStruct != nullptr && rightStruct != nullptr) {
        if (leftStruct->elements.size() != rightStruct->elements.size()) return false;
        for (std::size_t index = 0; index < leftStruct->elements.size(); ++index) {
            const auto& leftElement = leftStruct->elements[index];
            const auto& rightElement = rightStruct->elements[index];
            if (leftElement.identifier != rightElement.identifier) return false;
            if (leftElement.dataType == nullptr || rightElement.dataType == nullptr) return false;
            if (!skeletonMatch(*leftElement.dataType, *rightElement.dataType)) return false;
        }
        return true;
    }
    return false;
}

// Forward declaration: validateValue's Basic-Any arm calls validateAnyValue,
// and validateAnyValue re-enters validateValue for the decoded Any payload --
// the two are mutually recursive.
std::optional<std::string> validateAnyValue(const FieldRef& ref, const DataTypeResolver& resolver,
                                            const ConstraintResolver& constraintResolver,
                                            std::size_t depth);

std::optional<std::string> validateValue(const DataType& dataType, const FieldRef& ref,
                                         const DataTypeResolver& resolver,
                                         const ConstraintResolver& constraintResolver,
                                         std::size_t depth) {
    if (depth > 64) return std::string{"DataType resolution is cyclic or too deep"};
    return std::visit(
        [&](const auto& value) -> std::optional<std::string> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, DataType::Identifier>) {
                if (!resolver) return std::string{"DataType identifier has no resolver: "} + value.typeId;
                const auto* resolved = resolver(value.typeId);
                if (resolved == nullptr) return std::string{"DataType identifier is not defined: "} + value.typeId;
                FieldRef targetRef;
                if (const auto error = unwrapIdentifier(value, ref, targetRef)) return error;
                return validateValue(*resolved, targetRef, resolver, constraintResolver, depth + 1);
            } else if constexpr (std::is_same_v<T, DataType::Constrained>) {
                if (value.inner == nullptr) return std::string{"Constrained DataType has no inner DataType"};
                std::string baseError;
                const auto* base = resolveBase(*value.inner, resolver, depth + 1, baseError);
                if (base == nullptr) return baseError;
                FieldRef targetRef = ref;
                const DataType* target = value.inner.get();
                std::size_t targetDepth = depth;
                while (const auto* identifier = std::get_if<DataType::Identifier>(&target->value)) {
                    if (!resolver) return std::string{"DataType identifier has no resolver: "} + identifier->typeId;
                    const auto* resolved = resolver(identifier->typeId);
                    if (resolved == nullptr) return std::string{"DataType identifier is not defined: "} + identifier->typeId;
                    if (const auto error = unwrapIdentifier(*identifier, targetRef, targetRef)) return error;
                    target = resolved;
                    if (++targetDepth > 64) return std::string{"DataType resolution is cyclic or too deep"};
                }
                if (const auto error = checkConstraints(
                        *base, value.constraints, targetRef, constraintResolver)) {
                    return error;
                }
                return validateValue(*value.inner, ref, resolver, constraintResolver, depth + 1);
            } else if constexpr (std::is_same_v<T, DataType::Basic>) {
                if (value.type == BasicType::Any) {
                    return validateAnyValue(ref, resolver, constraintResolver, depth);
                }
                BasicValue ignored;
                return readBasicValue(ref, value.type, ignored);
            } else if constexpr (std::is_same_v<T, DataType::List>) {
                if (value.elementType == nullptr) return std::string{"List has no element DataType"};
                if (!ref.field->is_repeated() || ref.index >= 0) {
                    return std::string{"List value is not represented by a repeated protobuf field"};
                }
                const auto count = ref.owner->GetReflection()->FieldSize(*ref.owner, ref.field);
                for (int index = 0; index < count; ++index) {
                    if (const auto error = validateValue(
                            *value.elementType, FieldRef{ref.owner, ref.field, index}, resolver,
                            constraintResolver, depth + 1)) {
                        return error;
                    }
                }
                return std::nullopt;
            } else {
                std::string structureError;
                const auto* structure = messageAt(ref, &structureError);
                if (structure == nullptr) return structureError;
                for (const auto& element : value.elements) {
                    if (element.dataType == nullptr) {
                        return std::string{"Structure element has no DataType"};
                    }
                    const auto* elementField = structure->GetDescriptor()->FindFieldByName(element.identifier);
                    if (elementField == nullptr) {
                        return std::string{"Structure protobuf message has no field named "} +
                               element.identifier;
                    }
                    if (const auto error = validateValue(
                            *element.dataType, FieldRef{structure, elementField}, resolver,
                            constraintResolver, depth + 1)) {
                        return error;
                    }
                }
                return std::nullopt;
            }
        },
        dataType.value);
}

std::optional<std::string> validateAnyValue(const FieldRef& ref, const DataTypeResolver& resolver,
                                            const ConstraintResolver& constraintResolver,
                                            std::size_t depth) {
    AnyContents any;
    if (const auto error = readWireAnyType(ref, any)) return error;
    // A fresh pool per Any value (see decodeWireAnyPayload).
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    std::unique_ptr<Message> decoded;
    const FieldDescriptor* payloadField = nullptr;
    if (const auto error = decodeWireAnyPayload(any, pool, factory, decoded, payloadField)) {
        return error;
    }
    // Validate the value against S -- all its own Constrained layers
    // (Part A p67 / Part B p70) and any nested Any -- by re-entering
    // validateValue on the decoded Payload field.
    return validateValue(any.type, FieldRef{decoded.get(), payloadField},
                         resolver, constraintResolver, depth + 1);
}

// Implementation policy, not a proven ceiling: Part A p70 says the officially
// supported SiLA Content Types MUST be handled by every server, but that list
// is external to the PDF. These media types denote character data whose
// encoding MUST be UTF-8 (Part A p70/p63). type/subtype are matched
// case-insensitively (Part A p70): text/*, */xml, */*+xml, application/json,
// */*+json.
bool isTextualContentType(std::string_view type, std::string_view subtype) {
    const auto lower = [](std::string_view text) {
        std::string out{text};
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    const std::string lowerType = lower(type);
    const std::string lowerSubtype = lower(subtype);
    const auto endsWith = [](const std::string& value, std::string_view suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (lowerType == "text") return true;                                    // text/*
    if (lowerSubtype == "xml") return true;                                  // */xml
    if (lowerType == "application" && lowerSubtype == "json") return true;   // application/json
    if (endsWith(lowerSubtype, "+xml")) return true;                         // */*+xml
    if (endsWith(lowerSubtype, "+json")) return true;                        // */*+json
    return false;
}

// Part A p67 A224 / p69: the server decides AllowedTypes rather than
// delegating it -- the wire Any carries its own type. Read S, require it to
// structurally match at least one allowed entry T, then require the decoded
// value to satisfy that T's Constraints. depth caps mutual recursion
// through a nested Any whose own AllowedTypes re-enters here.
std::optional<std::string> validateAllowedTypesImpl(const ConstraintValue& constraint,
                                                    const FieldRef& ref, std::size_t depth) {
    if (depth > 64) return std::string{"AllowedTypes nesting is too deep"};
    AnyContents any;
    if (const auto error = readWireAnyType(ref, any)) return error;

    // Entries whose skeleton matches S; none -> the Any's type is not in
    // the allowed list (Part A p69).
    std::vector<const DataType*> candidates;
    for (const auto& allowed : constraint.allowedTypes) {
        if (allowed == nullptr) continue;
        if (skeletonMatch(any.type, *allowed)) candidates.push_back(allowed.get());
    }
    if (candidates.empty()) {
        return std::string{"Any type is not one of the allowed types"};
    }

    // Decode ONCE under S (the sender serialized the payload under S, not T);
    // every candidate T skeleton-matches S and walks the same message.
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    std::unique_ptr<Message> decoded;
    const FieldDescriptor* payloadField = nullptr;
    if (const auto error = decodeWireAnyPayload(any, pool, factory, decoded, payloadField)) {
        return error;
    }

    // Constraints inside a T re-enter the same server policy: AllowedTypes
    // recurses (depth-guarded); Unit is a no-op; ContentType (Part A p70,
    // R10-9e) and an inline W3C XML Schema OR inline JSON Schema (Part A p70,
    // R10-9f/g2) are decided here. A Url-sourced Schema is decided after
    // CommandParameterValidator rewrites it to Inline against the codegen-
    // provisioned table (R10-9g1/g2); the runtime client path (no such table)
    // still delegates an unresolved Url to its caller's ConstraintResolver.
    const ConstraintResolver nested =
        [depth](const ConstraintValue& inner, const Message& owner,
                const FieldDescriptor& field, int index) -> std::optional<std::string> {
        if (inner.kind == ConstraintValue::AllowedTypes) {
            return validateAllowedTypesImpl(inner, FieldRef{&owner, &field, index}, depth + 1);
        }
        if (inner.kind == ConstraintValue::ContentType) {
            return ValueValidator::validateContentType(inner, owner, field, index);
        }
        if (inner.kind == ConstraintValue::Schema) {
            return ValueValidator::validateSchema(inner, owner, field, index);
        }
        return std::nullopt;
    };

    std::optional<std::string> lastError;
    for (const auto* candidate : candidates) {
        const auto error = validateValue(*candidate, FieldRef{decoded.get(), payloadField},
                                         {}, nested, 0);
        if (!error) return std::nullopt;  // Part A p69: one fully-passing T accepts.
        lastError = error;
    }
    return lastError;  // Every matching T rejected the value against its Constraints.
}

}  // namespace

std::optional<std::string> ValueValidator::validate(
    const DataType& dataType, const google::protobuf::Message& message,
    const google::protobuf::FieldDescriptor& field, DataTypeResolver resolver,
    ConstraintResolver constraintResolver) {
    if (field.containing_type() != message.GetDescriptor()) {
        return std::string{"field descriptor does not belong to the protobuf message"};
    }
    return validateValue(dataType, FieldRef{&message, &field}, resolver, constraintResolver, 0);
}

std::optional<std::string> ValueValidator::validateAllowedTypes(
    const ConstraintValue& constraint, const google::protobuf::Message& owner,
    const google::protobuf::FieldDescriptor& field, int index) {
    return validateAllowedTypesImpl(constraint, FieldRef{&owner, &field, index}, 0);
}

std::optional<std::string> ValueValidator::validateContentType(
    const ConstraintValue& constraint, const google::protobuf::Message& owner,
    const google::protobuf::FieldDescriptor& field, int index) {
    if (!constraint.contentType.has_value()) return std::nullopt;  // parser filled nothing
    const auto& contentType = *constraint.contentType;
    // Non-textual media (image/png, video/mp4, ...) may carry arbitrary bytes
    // (Part A p70 restricts only character data), so accept without reading.
    if (!isTextualContentType(contentType.type, contentType.subtype)) return std::nullopt;
    const FieldRef ref{&owner, &field, index};
    // A ContentType on a String is a no-op: proto3 string is already UTF-8 on
    // the wire (Part A p63) and Part A p70 gives no in-band type/subtype label
    // to check the value against. Only a SiLA Binary carries raw bytes to
    // verify -- a wire Binary message (SiLAFramework.proto:24) or, for
    // hand-built test messages, a scalar bytes field.
    const bool fieldIsBinary =
        field.cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE
            ? field.message_type()->full_name() == "sila2.org.silastandard.Binary"
            : field.type() == FieldDescriptor::TYPE_BYTES;
    if (!fieldIsBinary) return std::nullopt;
    // After R10-9d the adapter resolves Binary Transfer UUIDs before validate
    // (service_adapter.h.j2), so a textual Binary here holds inline bytes;
    // readBasicValue rejects a value still on the transfer-UUID arm -- a
    // deliberate Validation Error, since an unresolved handle has no bytes to
    // UTF-8-check.
    BasicValue value;
    if (const auto error = readBasicValue(ref, BasicType::Binary, value)) return error;
    if (!types::isValidUtf8(value.string)) {
        return std::string{"Binary value declared as textual Content Type "} + contentType.type +
               "/" + contentType.subtype + " is not valid UTF-8";
    }
    return std::nullopt;
}

std::optional<std::string> ValueValidator::validateSchema(
    const ConstraintValue& constraint, const google::protobuf::Message& owner,
    const google::protobuf::FieldDescriptor& field, int index) {
    if (!constraint.schema.has_value()) return std::nullopt;  // parser filled nothing
    const auto& schema = *constraint.schema;
    // Only an Inline source is decided here. On the generated server path
    // CommandParameterValidator rewrites every Xml/Url AND Json/Url Schema to
    // Inline against the codegen-provisioned table before validate (R10-9g1/g2),
    // so a Url reaching HERE is the runtime client path (which has no table):
    // accept (nullopt) rather than fail an unprovisioned check -- the documented
    // asymmetry, since a real server always constructs from generated code.
    if (schema.source != ConstraintValue::SchemaValue::Source::Inline) return std::nullopt;
    // Schema applies to String and Binary only (Constraints.xsd; fdl-validation
    // .xsl:94,136 enforce it). Detect Binary the same way validateContentType
    // does so readBasicValue reads the right arm; everything else is a String.
    const bool fieldIsBinary =
        field.cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE
            ? field.message_type()->full_name() == "sila2.org.silastandard.Binary"
            : field.type() == FieldDescriptor::TYPE_BYTES;
    const BasicType baseType = fieldIsBinary ? BasicType::Binary : BasicType::String;
    const FieldRef ref{&owner, &field, index};
    // readBasicValue rejects a Binary still on the transfer-UUID arm; the
    // adapter resolves the UUID before validate (R10-9d), so a value here holds
    // bytes. For a String it yields the wire text.
    BasicValue value;
    if (const auto error = readBasicValue(ref, baseType, value)) return error;
    // Part A p70 MUST: XML or JSON data in a Binary MUST be UTF-8. A String is
    // already UTF-8 on the wire (Part A p63), so it is only checked for Binary.
    if (fieldIsBinary && !types::isValidUtf8(value.string)) {
        return std::string{"Binary value declared with an inline Schema is not valid UTF-8"};
    }
    // Each validator lives in its own third-party TU (libxml2 in
    // FdlRuntimeParser, json-schema in JsonSchemaSupport); this validator stays
    // free of both. Neither does any external resolution -- the inline schema
    // must be self-contained (Part A p70).
    if (schema.type == ConstraintValue::SchemaValue::Type::Xml) {
        return validateXmlAgainstInlineSchema(schema.value, value.string);
    }
    // Part A p70: schema type is Xml or Json; Json = a Json Schema.
    return validateJsonAgainstInlineSchema(schema.value, value.string);
}

}  // namespace dynamic
}  // namespace sila2
