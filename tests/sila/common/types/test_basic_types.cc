// Tests for the toProto/fromProto free-function pairs in BasicTypes.h: every
// Basic type (String, Integer, Real, Boolean, Date, Time, Timestamp,
// Duration, Binary, Any) is round-tripped C++ -> proto -> C++, plus the
// error/edge paths reachable when a caller hands fromProto() a proto message
// that did not originate from toProto() (missing oneof, unset fields, or
// out-of-range calendar/clock values the struct itself never validates).
#include <sila/common/types/BasicTypes.h>

#include "SiLAFramework.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace sila2::types;

// ---------------------------------------------------------------------------
// TRUE: round-trip every conversion pair.
// ---------------------------------------------------------------------------

TEST(BasicTypes, StringRoundTrips) {
    const std::string original = "hello, SiLA";
    EXPECT_EQ(fromProto(toProto(original)), original);
}

TEST(BasicTypes, IntegerRoundTrips) {
    const int64_t original = -42;
    EXPECT_EQ(fromProto(toProto(original)), original);
}

TEST(BasicTypes, RealRoundTrips) {
    const double original = 3.14159;
    EXPECT_EQ(fromProto(toProto(original)), original);
}

TEST(BasicTypes, BooleanRoundTrips) {
    EXPECT_EQ(fromProto(toProto(true)), true);
    EXPECT_EQ(fromProto(toProto(false)), false);
}

TEST(BasicTypes, DateRoundTrips) {
    const Date original{/*day=*/29, /*month=*/2, /*year=*/2024, Timezone{/*hours=*/2, /*minutes=*/30}};
    const Date result = fromProto(toProto(original));
    EXPECT_EQ(result.day, original.day);
    EXPECT_EQ(result.month, original.month);
    EXPECT_EQ(result.year, original.year);
    EXPECT_EQ(result.timezone.hours, original.timezone.hours);
    EXPECT_EQ(result.timezone.minutes, original.timezone.minutes);
}

TEST(BasicTypes, TimeRoundTrips) {
    const Time original{/*second=*/45, /*minute=*/30, /*hour=*/13, Timezone{1, 0}, /*millisecond=*/500};
    const Time result = fromProto(toProto(original));
    EXPECT_EQ(result.second, original.second);
    EXPECT_EQ(result.minute, original.minute);
    EXPECT_EQ(result.hour, original.hour);
    EXPECT_EQ(result.millisecond, original.millisecond);
    EXPECT_EQ(result.timezone.hours, original.timezone.hours);
    EXPECT_EQ(result.timezone.minutes, original.timezone.minutes);
}

TEST(BasicTypes, TimestampRoundTrips) {
    const Timestamp original{/*second=*/1, /*minute=*/2, /*hour=*/3, /*day=*/4,
                              /*month=*/5, /*year=*/2025, Timezone{-3, 45}, /*millisecond=*/999};
    const Timestamp result = fromProto(toProto(original));
    EXPECT_EQ(result.second, original.second);
    EXPECT_EQ(result.minute, original.minute);
    EXPECT_EQ(result.hour, original.hour);
    EXPECT_EQ(result.day, original.day);
    EXPECT_EQ(result.month, original.month);
    EXPECT_EQ(result.year, original.year);
    EXPECT_EQ(result.millisecond, original.millisecond);
    EXPECT_EQ(result.timezone.hours, original.timezone.hours);
    EXPECT_EQ(result.timezone.minutes, original.timezone.minutes);
}

TEST(BasicTypes, DurationWithFractionalSecondsRoundTrips) {
    // 2.5s exercises the seconds/nanos split (google.protobuf.Duration
    // convention) rather than just the whole-second happy path.
    const auto original = std::chrono::nanoseconds{2'500'000'000};
    const auto msg = toProto(original);
    EXPECT_EQ(msg.seconds(), 2);
    EXPECT_EQ(msg.nanos(), 500'000'000);
    EXPECT_EQ(fromProto(msg), original);
}

TEST(BasicTypes, BinaryWithInlineValueRoundTrips) {
    const std::vector<uint8_t> original{0x00, 0xFF, 0x10, 0x20};
    EXPECT_EQ(fromProto(toProto(original)), original);
}

TEST(BasicTypes, AnyRoundTrips) {
    const AnyValue original{"<DataType><Basic>String</Basic></DataType>", {0x01, 0x02, 0x03}};
    const AnyValue result = fromProto(toProto(original));
    EXPECT_EQ(result.typeXml, original.typeXml);
    EXPECT_EQ(result.payload, original.payload);
}

// ---------------------------------------------------------------------------
// FALSE: rejection and edge paths.
//
// CAUGHT   — the implementation detects and rejects the input.
// UNCAUGHT — BasicTypes.h performs no validation for this input; the test
//            documents the current (silent pass-through) behavior as a known
//            gap rather than asserting a rejection that does not exist.
// ---------------------------------------------------------------------------

// CAUGHT: Binary::fromProto requires the inline `value` field. A message
// that instead carries a binaryTransferUUID (chunked transfer) has no
// inline bytes to return, so fromProto must refuse to synthesize a result.
TEST(BasicTypes, BinaryFromProtoCaughtWithoutInlineValueThrows) {
    sila2::org::silastandard::Binary msg;
    msg.set_binarytransferuuid("11111111-1111-1111-1111-111111111111");
    ASSERT_FALSE(msg.has_value());
    EXPECT_THROW(fromProto(msg), std::invalid_argument);
}

// CAUGHT: same rejection for the fully-default (never-set) message, which is
// the shape a caller gets from a default-constructed proto before either
// oneof branch is populated.
TEST(BasicTypes, BinaryFromProtoCaughtOnDefaultMessageThrows) {
    sila2::org::silastandard::Binary msg;
    EXPECT_THROW(fromProto(msg), std::invalid_argument);
}

// UNCAUGHT: Date does not validate calendar ranges. A month of 13 and a day
// of 32 are not real calendar values, but fromProto has no invariant check
// and passes them through unchanged.
TEST(BasicTypes, DateFromProtoUncaughtWithOutOfRangeFieldsPassesThrough) {
    sila2::org::silastandard::Date msg;
    msg.set_day(32);
    msg.set_month(13);
    msg.set_year(2025);
    const Date result = fromProto(msg);
    EXPECT_EQ(result.day, 32u);
    EXPECT_EQ(result.month, 13u);
}

// UNCAUGHT: Time does not validate clock ranges either — hour=25/minute=61
// are not real times of day, but fromProto passes them through unchanged.
TEST(BasicTypes, TimeFromProtoUncaughtWithOutOfRangeFieldsPassesThrough) {
    sila2::org::silastandard::Time msg;
    msg.set_hour(25);
    msg.set_minute(61);
    msg.set_second(0);
    const Time result = fromProto(msg);
    EXPECT_EQ(result.hour, 25u);
    EXPECT_EQ(result.minute, 61u);
}

// UNCAUGHT: a fully default (all-zero, never-set) message is not an error
// for the scalar types — proto3 has no concept of "absent" for these
// fields, so fromProto returns the zero value rather than rejecting it.
// Documented here as the boundary between "graceful default" and "invalid
// input" for types with no explicit validation.
TEST(BasicTypes, ScalarFromProtoUncaughtOnDefaultMessageReturnsZeroValue) {
    EXPECT_EQ(fromProto(sila2::org::silastandard::String{}), std::string{});
    EXPECT_EQ(fromProto(sila2::org::silastandard::Integer{}), 0);
    EXPECT_EQ(fromProto(sila2::org::silastandard::Boolean{}), false);
}

}  // namespace
