// BinaryRetry.h — Shared retry utilities for binary transfer (architecture.md §4.6)
#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

#include <SiLABinaryTransfer.grpc.pb.h>

namespace sila2 {

/// Delay before the given retry attempt of a @ref gl_binary_transfer "Binary Transfer" RPC,
/// doubling per attempt (1s, 2s, 4s, ...) up to
/// `maxBackoff`. Shared by BinaryDownloader and BinaryUploader so both back
/// off the same way.
inline std::chrono::milliseconds backoffDelay(int attempt, std::chrono::seconds maxBackoff) {
    auto delay = std::chrono::seconds{1 << std::min(attempt, 6)};
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::min(delay, maxBackoff));
}

/// Extracts the @ref gl_binary_transfer "Binary Transfer" error type from a
/// failed RPC's status, if the failure carries one.
/// @return std::nullopt when `status` is not ABORTED or its error details do
/// not parse as a BinaryTransferError (an ordinary transient/network
/// failure, worth retrying as such).
inline std::optional<sila2::org::silastandard::BinaryTransferError_ErrorType>
parseBinaryTransferError(const grpc::Status& status) {
    if (status.error_code() != grpc::StatusCode::ABORTED) return std::nullopt;
    sila2::org::silastandard::BinaryTransferError error;
    if (!error.ParseFromString(status.error_details())) return std::nullopt;
    return error.errortype();
}

}  // namespace sila2
