// Tests for toCommandExecutionStatus (CommandExecutionStatus.h): maps the
// protobuf ExecutionInfo_CommandStatus enum to the client-facing
// CommandExecutionStatus enum, defaulting unrecognized values to kRunning
// instead of throwing.
#include <sila/client/CommandExecutionStatus.h>

#include <gtest/gtest.h>

namespace {
using sila2::CommandExecutionStatus;
using sila2::toCommandExecutionStatus;
namespace fw = sila2::org::silastandard;

// --- True (positive) paths --------------------------------------------------

TEST(CommandExecutionStatusMapping, WaitingMapsToKWaiting) {
    EXPECT_EQ(toCommandExecutionStatus(fw::ExecutionInfo_CommandStatus_waiting),
              CommandExecutionStatus::kWaiting);
}

TEST(CommandExecutionStatusMapping, RunningMapsToKRunning) {
    EXPECT_EQ(toCommandExecutionStatus(fw::ExecutionInfo_CommandStatus_running),
              CommandExecutionStatus::kRunning);
}

TEST(CommandExecutionStatusMapping, FinishedSuccessfullyMapsToKFinishedSuccessfully) {
    EXPECT_EQ(toCommandExecutionStatus(fw::ExecutionInfo_CommandStatus_finishedSuccessfully),
              CommandExecutionStatus::kFinishedSuccessfully);
}

TEST(CommandExecutionStatusMapping, FinishedWithErrorMapsToKFinishedWithError) {
    EXPECT_EQ(toCommandExecutionStatus(fw::ExecutionInfo_CommandStatus_finishedWithError),
              CommandExecutionStatus::kFinishedWithError);
}

// --- False (negative/edge) paths ---------------------------------------------
// CAUGHT: none — this switch has no rejection path, only a fallback.
// UNCAUGHT: an out-of-range enum value is silently accepted as kRunning
// rather than rejected; documented below rather than asserted as an error.

TEST(CommandExecutionStatusMapping, UnrecognizedValueDefaultsToKRunningRatherThanThrowing) {
    const auto bogus = static_cast<fw::ExecutionInfo_CommandStatus>(99);

    EXPECT_NO_THROW({
        const CommandExecutionStatus result = toCommandExecutionStatus(bogus);
        EXPECT_EQ(result, CommandExecutionStatus::kRunning);
    });
}

TEST(CommandExecutionStatusMapping, NegativeValueDefaultsToKRunning) {
    const auto bogus = static_cast<fw::ExecutionInfo_CommandStatus>(-1);

    EXPECT_EQ(toCommandExecutionStatus(bogus), CommandExecutionStatus::kRunning);
}

TEST(CommandExecutionStatusMapping, FarOutOfRangeValueDefaultsToKRunning) {
    const auto bogus = static_cast<fw::ExecutionInfo_CommandStatus>(1000000);

    EXPECT_EQ(toCommandExecutionStatus(bogus), CommandExecutionStatus::kRunning);
}

}  // namespace
