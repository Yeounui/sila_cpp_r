// SiLAErrorSubtypes.cc — concrete SiLA 2 error types implementation
//
// Ported from sila_cpp v0.3.11
// src/lib/framework/error_handling/ (MIT License, Copyright 2020 SiLA2).
// Each derived class's makeErrorMessage() throws std::logic_error — the
// SiLAFramework.pb.h codegen is not wired into the build yet.
#include "SiLAErrorSubtypes.h"

#include <stdexcept>
#include <utility>

namespace sila2 {
namespace error {

// ---------------------------------------------------------------------------
// ValidationError
// ---------------------------------------------------------------------------

ValidationError::ValidationError(std::string parameter, std::string message)
    : SiLAError{ErrorType::ValidationError, std::move(message)}, parameter_{std::move(parameter)} {}

std::string ValidationError::parameter() const { return parameter_; }

std::unique_ptr<sila2::org::silastandard::SiLAError> ValidationError::makeErrorMessage() const {
    throw std::logic_error{"ValidationError::makeErrorMessage: SiLAFramework.pb.h codegen not wired into the build yet"};
}

// ---------------------------------------------------------------------------
// ExecutionError / DefinedExecutionError / UndefinedExecutionError
// ---------------------------------------------------------------------------

ExecutionError::ExecutionError(std::string msg)
    : SiLAError{ErrorType::UndefinedExecutionError, std::move(msg)} {}

ExecutionError::ExecutionError(std::string identifier, std::string msg)
    : SiLAError{ErrorType::DefinedExecutionError, std::move(msg)},
      errorIdentifier_{std::move(identifier)} {}

std::string ExecutionError::errorIdentifier() const { return errorIdentifier_; }

std::unique_ptr<sila2::org::silastandard::SiLAError> ExecutionError::makeErrorMessage() const {
    throw std::logic_error{"ExecutionError::makeErrorMessage: not implemented yet"};
}

DefinedExecutionError::DefinedExecutionError(std::string identifier, std::string message)
    : ExecutionError{std::move(identifier), std::move(message)} {}

UndefinedExecutionError::UndefinedExecutionError(std::string message)
    : ExecutionError{std::move(message)} {}

// ---------------------------------------------------------------------------
// FrameworkError
// ---------------------------------------------------------------------------

namespace {
// Falls back to a per-type default message when message is empty,
// matching the reference's PrivateImpl::defaultMessageForType behavior.
std::string defaultMessageForType(FrameworkError::FrameworkErrorType type) {
    switch (type) {
    case FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted:
        return "The SiLA Server does not accept the Command Execution.";
    case FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid:
        return "The Command Execution UUID is invalid.";
    case FrameworkError::FrameworkErrorType::CommandExecutionNotFinished:
        return "The Command Execution is not finished yet.";
    case FrameworkError::FrameworkErrorType::InvalidMetadata:
        return "The required SiLA Client Metadata has not been sent along or "
               "is invalid.";
    case FrameworkError::FrameworkErrorType::NoMetadataAllowed:
        return "The SiLA Service Feature does not allow the use of SiLA "
               "Client Metadata.";
    case FrameworkError::FrameworkErrorType::Invalid:
        break;
    }
    return "";
}
}  // namespace

FrameworkError::FrameworkError(FrameworkErrorType type, std::string message)
    : SiLAError{ErrorType::FrameworkError, message.empty()
                                                ? defaultMessageForType(type)
                                                : std::move(message)},
      frameworkErrorType_{type} {}

FrameworkError::FrameworkErrorType FrameworkError::frameworkErrorType() const {
    return frameworkErrorType_;
}

std::string FrameworkError::frameworkErrorTypeName() const {
    return frameworkErrorTypeToString(frameworkErrorType_);
}

std::unique_ptr<sila2::org::silastandard::SiLAError>
FrameworkError::makeErrorMessage() const {
    throw std::logic_error{
        "FrameworkError::makeErrorMessage() requires SiLAFramework.pb.h codegen"};
}

std::string FrameworkError::frameworkErrorTypeToString(FrameworkErrorType type) {
    switch (type) {
    case FrameworkErrorType::CommandExecutionNotAccepted:
        return "Command Execution Not Accepted";
    case FrameworkErrorType::InvalidCommandExecutionUuid:
        return "Invalid Command Execution UUID";
    case FrameworkErrorType::CommandExecutionNotFinished:
        return "Command Execution Not Finished";
    case FrameworkErrorType::InvalidMetadata:
        return "Invalid Metadata";
    case FrameworkErrorType::NoMetadataAllowed:
        return "No Metadata Allowed";
    case FrameworkErrorType::Invalid:
        break;
    }
    return "";
}

// ---------------------------------------------------------------------------
// ConnectionError
// ---------------------------------------------------------------------------

ConnectionError::ConnectionError(grpc::Status status)
    : SiLAError{ErrorType::ConnectionError, status.error_message()}, status_{std::move(status)} {}

grpc::StatusCode ConnectionError::statusCode() const { return status_.error_code(); }

std::string ConnectionError::statusCodeName() const {
    return statusCodeToString(status_.error_code());
}

std::string ConnectionError::statusCodeToString(grpc::StatusCode code) {
    switch (code) {
    case grpc::StatusCode::OK:
        return "OK";
    case grpc::StatusCode::CANCELLED:
        return "CANCELLED";
    case grpc::StatusCode::UNKNOWN:
        return "UNKNOWN";
    case grpc::StatusCode::INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case grpc::StatusCode::DEADLINE_EXCEEDED:
        return "DEADLINE_EXCEEDED";
    case grpc::StatusCode::NOT_FOUND:
        return "NOT_FOUND";
    case grpc::StatusCode::ALREADY_EXISTS:
        return "ALREADY_EXISTS";
    case grpc::StatusCode::PERMISSION_DENIED:
        return "PERMISSION_DENIED";
    case grpc::StatusCode::UNAUTHENTICATED:
        return "UNAUTHENTICATED";
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
        return "RESOURCE_EXHAUSTED";
    case grpc::StatusCode::FAILED_PRECONDITION:
        return "FAILED_PRECONDITION";
    case grpc::StatusCode::ABORTED:
        return "ABORTED";
    case grpc::StatusCode::OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case grpc::StatusCode::UNIMPLEMENTED:
        return "UNIMPLEMENTED";
    case grpc::StatusCode::INTERNAL:
        return "INTERNAL";
    case grpc::StatusCode::UNAVAILABLE:
        return "UNAVAILABLE";
    case grpc::StatusCode::DATA_LOSS:
        return "DATA_LOSS";
    case grpc::StatusCode::DO_NOT_USE:
        return "DO_NOT_USE";
    }
    // ponytail: no logging subsystem yet, add calls back when one exists
    return "";
}

std::unique_ptr<sila2::org::silastandard::SiLAError> ConnectionError::makeErrorMessage() const {
    throw std::logic_error{
        "ConnectionError::makeErrorMessage() requires SiLAFramework.pb.h codegen"};
}
}  // namespace error
}  // namespace sila2
