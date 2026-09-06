// BinaryUtil.h — shared helpers for Binary Download/Upload services (architecture.md §3.5)
#pragma once

#include <string>

#include <SiLABinaryTransfer.grpc.pb.h>

#include <sila/common/util/base64.h>

#include "BinaryStore.h"

namespace sila2 {

// Mirrors SiLAError::toStatus() (architecture.md §3.4): errors travel as
// ABORTED status with the serialized error proto Base64-encoded into the
// status message (Part B p65) and the raw bytes kept in error_details.
inline grpc::Status makeBinaryTransferStatus(
    sila2::org::silastandard::BinaryTransferError::ErrorType type,
    const std::string& message) {
    sila2::org::silastandard::BinaryTransferError error;
    error.set_errortype(type);
    error.set_message(message);
    // Part B p65: the serialized proto MUST be Base64-encoded into the status
    // message; raw bytes stay in error_details (mirrors SiLAError::toStatus()).
    const std::string serialized = error.SerializeAsString();
    return grpc::Status{grpc::StatusCode::ABORTED, base64Encode(serialized), serialized};
}

inline grpc::Status deleteBinarySlot(BinaryStore& store, const std::string& uuid) {
    if (!store.contains(uuid)) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID,
            "Unknown binaryTransferUUID: " + uuid);
    }
    store.remove(uuid);
    return grpc::Status::OK;
}

}  // namespace sila2
