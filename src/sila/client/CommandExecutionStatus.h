// CommandExecutionStatus.h — Observable command execution status types (architecture.md §4)
#pragma once

#include <functional>
#include <string>

#include "SiLAFramework.pb.h"

namespace sila2 {

enum class CommandExecutionStatus {
    kWaiting,
    kRunning,
    kFinishedSuccessfully,
    kFinishedWithError,
};

struct ExecutionUpdate {
    CommandExecutionStatus status;
    float progress;
    std::string statusMessage;
};

using ExecutionUpdateCallback = std::function<void(const ExecutionUpdate& update)>;

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
