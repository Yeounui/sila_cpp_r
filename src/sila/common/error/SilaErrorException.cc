// SilaErrorException.cc
#include "SilaErrorException.h"

#include <grpcpp/support/status.h>

#include "SilaErrorSubtypes.h"
#include "SiLAFramework.pb.h"

namespace sila2 {
namespace error {

namespace {
// Reverse of SilaErrorSubtypes.cc's toProtoErrorType — maps the proto enum
// back to the C++ enum.
FrameworkError::FrameworkErrorType
fromProtoErrorType(sila2::org::silastandard::FrameworkError::ErrorType type) {
    using FET = FrameworkError::FrameworkErrorType;
    using PET = sila2::org::silastandard::FrameworkError;
    switch (type) {
    case PET::COMMAND_EXECUTION_NOT_ACCEPTED:
        return FET::CommandExecutionNotAccepted;
    case PET::INVALID_COMMAND_EXECUTION_UUID:
        return FET::InvalidCommandExecutionUuid;
    case PET::COMMAND_EXECUTION_NOT_FINISHED:
        return FET::CommandExecutionNotFinished;
    case PET::INVALID_METADATA:
        return FET::InvalidMetadata;
    case PET::NO_METADATA_ALLOWED:
        return FET::NoMetadataAllowed;
    default:
        // proto3 enums are open: an errortype outside the five named values
        // (a newer spec revision, or a corrupt peer) arrives preserved, not
        // clamped to 0, and lands here. Fold it to the least specific of the
        // five — there is no "unknown" FrameworkErrorType to map it to, by
        // design (SilaErrorSubtypes.h). Mirrors toProtoErrorType's default
        // arm in SilaErrorSubtypes.cc.
        return FET::CommandExecutionNotAccepted;
    }
}
}  // namespace

std::unique_ptr<SilaError> fromGrpcStatus(const grpc::Status& status) {
    if (status.ok()) {
        return nullptr;
    }
    // Only ABORTED carries a serialized SilaError in its binary details — any
    // other code is a gRPC/transport-level failure (architecture.md §3.4).
    if (status.error_code() != grpc::StatusCode::ABORTED) {
        return std::make_unique<ConnectionError>(status);
    }

    sila2::org::silastandard::SiLAError errorProto;
    if (!errorProto.ParseFromString(status.error_details())) {
        // Can't trust the payload as a SiLA error — fall back to the status's
        // plain-text message.
        return std::make_unique<UndefinedExecutionError>(status.error_message());
    }

    switch (errorProto.error_case()) {
    case sila2::org::silastandard::SiLAError::kValidationError:
        return std::make_unique<ValidationError>(
            errorProto.validationerror().parameter(), errorProto.validationerror().message());
    case sila2::org::silastandard::SiLAError::kDefinedExecutionError:
        return std::make_unique<DefinedExecutionError>(
            errorProto.definedexecutionerror().erroridentifier(),
            errorProto.definedexecutionerror().message());
    case sila2::org::silastandard::SiLAError::kUndefinedExecutionError:
        return std::make_unique<UndefinedExecutionError>(
            errorProto.undefinedexecutionerror().message());
    case sila2::org::silastandard::SiLAError::kFrameworkError:
        return std::make_unique<FrameworkError>(
            fromProtoErrorType(errorProto.frameworkerror().errortype()),
            errorProto.frameworkerror().message());
    case sila2::org::silastandard::SiLAError::ERROR_NOT_SET:
        return std::make_unique<UndefinedExecutionError>(status.error_message());
    }
    // ponytail: unreachable if the proto's oneof cases are exhaustively
    // handled above; return to satisfy -Wreturn-type without a default label
    // that would silently swallow a newly added oneof case.
    return std::make_unique<UndefinedExecutionError>(status.error_message());
}

std::string messageFromGrpcStatus(const grpc::Status& status) {
    const auto error = fromGrpcStatus(status);
    return error ? error->what() : "";
}
}  // namespace error
}  // namespace sila2
