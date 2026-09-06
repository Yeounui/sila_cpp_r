// SiLAErrorSubtypes.cc — concrete SiLA 2 error types implementation
//
// Ported from sila_cpp v0.3.11
// src/lib/framework/error_handling/ (MIT License, Copyright 2020 SiLA2).
#include "SiLAErrorSubtypes.h"

#include <stdexcept>
#include <utility>

#include "SiLAFramework.pb.h"

namespace sila2 {
namespace error {

// ---------------------------------------------------------------------------
// ValidationError
// ---------------------------------------------------------------------------

ValidationError::ValidationError(std::string parameter, std::string message)
    : SiLAError{ErrorType::ValidationError, std::move(message)}, parameter_{std::move(parameter)} {}

std::string ValidationError::parameter() const { return parameter_; }

std::unique_ptr<sila2::org::silastandard::SiLAError> ValidationError::makeErrorMessage() const {
    auto error = std::make_unique<sila2::org::silastandard::SiLAError>();
    auto* validation = error->mutable_validationerror();
    validation->set_parameter(parameter_);
    validation->set_message(what());
    return error;
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
    auto error = std::make_unique<sila2::org::silastandard::SiLAError>();
    // errorType() distinguishes Defined from Undefined — set by the two-arg
    // vs one-arg ExecutionError ctor, so the subclass identity is already baked
    // into the base SiLAError::type_ field.
    if (errorType() == ErrorType::DefinedExecutionError) {
        auto* defined = error->mutable_definedexecutionerror();
        defined->set_erroridentifier(errorIdentifier_);
        defined->set_message(what());
    } else {
        auto* undefined = error->mutable_undefinedexecutionerror();
        undefined->set_message(what());
    }
    return error;
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

namespace {
// Maps the C++ enum to the proto enum, one arm per value.
sila2::org::silastandard::FrameworkError::ErrorType
toProtoErrorType(FrameworkError::FrameworkErrorType type) {
    using FET = FrameworkError::FrameworkErrorType;
    using PET = sila2::org::silastandard::FrameworkError;
    switch (type) {
    case FET::CommandExecutionNotAccepted:
        return PET::COMMAND_EXECUTION_NOT_ACCEPTED;
    case FET::InvalidCommandExecutionUuid:
        return PET::INVALID_COMMAND_EXECUTION_UUID;
    case FET::CommandExecutionNotFinished:
        return PET::COMMAND_EXECUTION_NOT_FINISHED;
    case FET::InvalidMetadata:
        return PET::INVALID_METADATA;
    case FET::NoMetadataAllowed:
        return PET::NO_METADATA_ALLOWED;
    }
    // Unreachable: every enumerator returns above. Kept because GCC's
    // -Wreturn-type (src/sila/CMakeLists.txt:156 builds -Wall -Wextra) still
    // warns on a non-void function whose body is only a switch.
    return PET::COMMAND_EXECUTION_NOT_ACCEPTED;
}
}  // namespace

std::unique_ptr<sila2::org::silastandard::SiLAError>
FrameworkError::makeErrorMessage() const {
    auto error = std::make_unique<sila2::org::silastandard::SiLAError>();
    auto* framework = error->mutable_frameworkerror();
    framework->set_errortype(toProtoErrorType(frameworkErrorType_));
    framework->set_message(what());
    return error;
}

// ---------------------------------------------------------------------------
// ConnectionError
// ---------------------------------------------------------------------------

ConnectionError::ConnectionError(grpc::Status status)
    : SiLAError{ErrorType::ConnectionError, status.error_message()}, status_{std::move(status)} {}

grpc::StatusCode ConnectionError::statusCode() const { return status_.error_code(); }

std::unique_ptr<sila2::org::silastandard::SiLAError> ConnectionError::makeErrorMessage() const {
    // The SiLAError oneof has no ConnectionError variant — this is an
    // infrastructure error, not a SiLA protocol error.
    throw std::logic_error{
        "ConnectionError is not a SiLA protocol error — use statusCode() directly"};
}
}  // namespace error
}  // namespace sila2
