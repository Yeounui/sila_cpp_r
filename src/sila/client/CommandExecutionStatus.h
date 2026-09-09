// CommandExecutionStatus.h — Observable command execution status types (architecture.md §4)
#pragma once

#include <functional>
#include <string>

#include "SiLAFramework.pb.h"

namespace sila2 {

/// The status of one @ref gl_observable_command "Observable Command"
/// execution as reported by @ref gl_command_execution_info "Command Execution Info" , client-side.
/// kFinishedSuccessfully and
/// kFinishedWithError are terminal: no further update follows.
enum class CommandExecutionStatus {
    kWaiting,
    kRunning,
    kFinishedSuccessfully,
    kFinishedWithError,
};

/// One @ref gl_command_execution_info "Command Execution Info" update
/// delivered to an ExecutionInfoSubscriber callback. `progress` and
/// `statusMessage` are only meaningful while `status` is kRunning; a
/// terminal status set by ExecutionInfoSubscriber itself (rather than
/// received from the server) carries the failure reason in `statusMessage`.
struct ExecutionUpdate {
    CommandExecutionStatus status;
    float progress;
    std::string statusMessage;
};

using ExecutionUpdateCallback = std::function<void(const ExecutionUpdate& update)>;

/// Converts the wire-level SiLA command status to the client-facing
/// CommandExecutionStatus. An unrecognized wire value falls back to kRunning.
inline CommandExecutionStatus toCommandExecutionStatus(
    org::silastandard::ExecutionInfo_CommandStatus status) {
    switch (status) {
    case org::silastandard::ExecutionInfo_CommandStatus_waiting:
        return CommandExecutionStatus::kWaiting;
    case org::silastandard::ExecutionInfo_CommandStatus_running:
        return CommandExecutionStatus::kRunning;
    case org::silastandard::ExecutionInfo_CommandStatus_finishedSuccessfully:
        return CommandExecutionStatus::kFinishedSuccessfully;
    case org::silastandard::ExecutionInfo_CommandStatus_finishedWithError:
        return CommandExecutionStatus::kFinishedWithError;
    default:
        return CommandExecutionStatus::kRunning;
    }
}

}  // namespace sila2
