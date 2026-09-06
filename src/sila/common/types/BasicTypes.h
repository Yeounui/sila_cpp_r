// BasicTypes.h — SiLA 2 Basic type conversions (architecture.md §2.1)
//
// Header-only toProto/fromProto free functions mapping SiLA Basic types
// (SiLAFramework.proto) to their native C++ representations.
#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include "SiLAFramework.pb.h"

namespace sila2 {
namespace types {

/// A SiLA 2 Timezone offset from UTC.
struct Timezone {
    int32_t hours;
    uint32_t minutes;
};

/// A SiLA 2 calendar date, with an attached Timezone.
struct Date {
    uint32_t day;
    uint32_t month;
    uint32_t year;
    Timezone timezone;
};

/// A SiLA 2 time-of-day, with an attached Timezone.
struct Time {
    uint32_t second;
    uint32_t minute;
    uint32_t hour;
    Timezone timezone;
    uint32_t millisecond;
};

/// A SiLA 2 combined date and time-of-day, with an attached Timezone.
struct Timestamp {
    uint32_t second;
    uint32_t minute;
    uint32_t hour;
    uint32_t day;
    uint32_t month;
    uint32_t year;
    Timezone timezone;
    uint32_t millisecond;
};

/// An opaque SiLA 2 Any value — a type descriptor (XML) plus an
/// undecoded payload. Interpreting the payload is the caller's job.
struct AnyValue {
    std::string typeXml;
    std::vector<uint8_t> payload;
};

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::String toProto(const std::string& val) {
    sila2::org::silastandard::String msg;
    msg.set_value(val);
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline std::string fromProto(const sila2::org::silastandard::String& msg) {
    return msg.value();
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Integer toProto(int64_t val) {
    sila2::org::silastandard::Integer msg;
    msg.set_value(val);
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline int64_t fromProto(const sila2::org::silastandard::Integer& msg) {
    return msg.value();
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Real toProto(double val) {
    sila2::org::silastandard::Real msg;
    msg.set_value(val);
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline double fromProto(const sila2::org::silastandard::Real& msg) {
    return msg.value();
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Boolean toProto(bool val) {
    sila2::org::silastandard::Boolean msg;
    msg.set_value(val);
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline bool fromProto(const sila2::org::silastandard::Boolean& msg) {
    return msg.value();
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Date toProto(const Date& val) {
    sila2::org::silastandard::Date msg;
    msg.set_day(val.day);
    msg.set_month(val.month);
    msg.set_year(val.year);
    auto* timezone = msg.mutable_timezone();
    timezone->set_hours(val.timezone.hours);
    timezone->set_minutes(val.timezone.minutes);
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline Date fromProto(const sila2::org::silastandard::Date& msg) {
    Date val;
    val.day = msg.day();
    val.month = msg.month();
    val.year = msg.year();
    val.timezone.hours = msg.timezone().hours();
    val.timezone.minutes = msg.timezone().minutes();
    return val;
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Time toProto(const Time& val) {
    sila2::org::silastandard::Time msg;
    msg.set_second(val.second);
    msg.set_minute(val.minute);
    msg.set_hour(val.hour);
    msg.set_millisecond(val.millisecond);
    auto* timezone = msg.mutable_timezone();
    timezone->set_hours(val.timezone.hours);
    timezone->set_minutes(val.timezone.minutes);
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline Time fromProto(const sila2::org::silastandard::Time& msg) {
    Time val;
    val.second = msg.second();
    val.minute = msg.minute();
    val.hour = msg.hour();
    val.millisecond = msg.millisecond();
    val.timezone.hours = msg.timezone().hours();
    val.timezone.minutes = msg.timezone().minutes();
    return val;
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Timestamp toProto(const Timestamp& val) {
    sila2::org::silastandard::Timestamp msg;
    msg.set_second(val.second);
    msg.set_minute(val.minute);
    msg.set_hour(val.hour);
    msg.set_day(val.day);
    msg.set_month(val.month);
    msg.set_year(val.year);
    msg.set_millisecond(val.millisecond);
    auto* timezone = msg.mutable_timezone();
    timezone->set_hours(val.timezone.hours);
    timezone->set_minutes(val.timezone.minutes);
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline Timestamp fromProto(const sila2::org::silastandard::Timestamp& msg) {
    Timestamp val;
    val.second = msg.second();
    val.minute = msg.minute();
    val.hour = msg.hour();
    val.day = msg.day();
    val.month = msg.month();
    val.year = msg.year();
    val.millisecond = msg.millisecond();
    val.timezone.hours = msg.timezone().hours();
    val.timezone.minutes = msg.timezone().minutes();
    return val;
}

/// Converts a wall-clock instant to a SiLA Timestamp in UTC. gmtime_r, not
/// localtime_r: the SiLA Timestamp carries an explicit Timezone, and UTC is
/// the only offset that needs no host configuration.
[[nodiscard("caller expects the converted SiLA timestamp")]]
inline Timestamp timestampFromSystemClock(std::chrono::system_clock::time_point point) {
    const auto wholeSeconds = std::chrono::time_point_cast<std::chrono::seconds>(point);
    const auto millisPart =
        std::chrono::duration_cast<std::chrono::milliseconds>(point - wholeSeconds).count();
    const std::time_t asTimeT = std::chrono::system_clock::to_time_t(wholeSeconds);
    std::tm utc{};
    gmtime_r(&asTimeT, &utc);
    Timestamp value{};
    value.second = static_cast<uint32_t>(utc.tm_sec);
    value.minute = static_cast<uint32_t>(utc.tm_min);
    value.hour = static_cast<uint32_t>(utc.tm_hour);
    value.day = static_cast<uint32_t>(utc.tm_mday);
    value.month = static_cast<uint32_t>(utc.tm_mon + 1);
    value.year = static_cast<uint32_t>(utc.tm_year + 1900);
    value.millisecond = static_cast<uint32_t>(millisPart);
    value.timezone = Timezone{0, 0};  // gmtime_r produced UTC
    return value;
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Duration toProto(std::chrono::nanoseconds val) {
    sila2::org::silastandard::Duration msg;
    // Split into whole seconds and the remaining sub-second nanos, matching
    // the proto's separate seconds/nanos fields (google.protobuf.Duration convention).
    constexpr int64_t kNanosPerSecond = 1'000'000'000;
    msg.set_seconds(val.count() / kNanosPerSecond);
    msg.set_nanos(static_cast<int32_t>(val.count() % kNanosPerSecond));
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline std::chrono::nanoseconds fromProto(const sila2::org::silastandard::Duration& msg) {
    constexpr int64_t kNanosPerSecond = 1'000'000'000;
    return std::chrono::nanoseconds{msg.seconds() * kNanosPerSecond + msg.nanos()};
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Binary toProto(const std::vector<uint8_t>& val) {
    sila2::org::silastandard::Binary msg;
    msg.set_value(std::string{val.begin(), val.end()});
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline std::vector<uint8_t> fromProto(const sila2::org::silastandard::Binary& msg) {
    // Only the inline bytes case is handled here; a set binaryTransferUUID
    // means the payload went through chunked transfer, which the
    // BinaryStore reassembles separately — this function has no access to it.
    if (!msg.has_value()) {
        throw std::invalid_argument{"Binary has no inline value; binaryTransferUUID must be resolved via the BinaryStore"};
    }
    const std::string& bytes = msg.value();
    return std::vector<uint8_t>{bytes.begin(), bytes.end()};
}

[[nodiscard("caller expects the converted proto message")]]
inline sila2::org::silastandard::Any toProto(const AnyValue& val) {
    sila2::org::silastandard::Any msg;
    msg.set_type(val.typeXml);
    msg.set_payload(std::string{val.payload.begin(), val.payload.end()});
    return msg;
}

[[nodiscard("caller expects the converted native value")]]
inline AnyValue fromProto(const sila2::org::silastandard::Any& msg) {
    AnyValue val;
    val.typeXml = msg.type();
    const std::string& payload = msg.payload();
    val.payload = std::vector<uint8_t>{payload.begin(), payload.end()};
    return val;
}
}  // namespace types
}  // namespace sila2
