// BinaryRetry.h — Shared retry utilities for binary transfer (architecture.md §4.6)
#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

#include <SiLABinaryTransfer.grpc.pb.h>

namespace sila2 {

inline std::chrono::milliseconds backoffDelay(int attempt, std::chrono::seconds maxBackoff) {
    auto delay = std::chrono::seconds{1 << std::min(attempt, 6)};
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::min(delay, maxBackoff));
}

inline std::optional<sila2::org::silastandard::BinaryTransferError_ErrorType>
parseBinaryTransferError(const grpc::Status& status) {
    if (status.error_code() != grpc::StatusCode::ABORTED) return std::nullopt;
    sila2::org::silastandard::BinaryTransferError error;
    if (!error.ParseFromString(status.error_details())) return std::nullopt;
    return error.errortype();
}

}  // namespace sila2
