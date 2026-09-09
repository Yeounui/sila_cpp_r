// SiLAErrorSubtypes.h — concrete SiLA 2 error types (architecture.md §3.4)
//
// Ported from sila_cpp v0.3.11
// src/include/sila_cpp/framework/error_handling/ (MIT License,
// Copyright 2020 SiLA2). The originals keep state behind a
// polymorphic_value PIMPL; this port stores members directly, matching
// SiLAError.h's PIMPL-free convention. raise()/clone() (QException)
// and fromErrorMessage() (protobuf-dependent) are dropped across the
// board.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <grpcpp/support/status.h>

#include "SiLAError.h"

namespace sila2
{
namespace org
{
namespace silastandard
{
class SiLAError;
}  // namespace silastandard
}  // namespace org
}  // namespace sila2

namespace sila2 {
namespace error {

// ---------------------------------------------------------------------------
// ValidationError
// ---------------------------------------------------------------------------

/// Throw this from a Feature Command implementation when a Command
/// Parameter fails validation before the Command runs — the wrong SiLA
/// Data Type, out of range, or a violated declared @ref gl_constraint "Constraint".
///
/// Thrown when a Command parameter fails validation before the Command
/// executes (architecture.md §3.4). The SiLA Client receives this as a
/// @ref gl_validation_error "Validation Error" on the wire.
/// @see CommandParameterValidator::validate
class ValidationError : public SiLAError {
public:
    /// @param parameter The fully qualified identifier of the parameter that
    /// failed validation.
    /// @param message This error's message, or empty for a generic fallback.
    ValidationError(std::string parameter, std::string message);

    /// @return The fully qualified identifier of the parameter that failed
    /// validation.
    [[nodiscard("caller expects the failing parameter's identifier")]] \
    std::string parameter() const;

protected:
    /*  base의 toStatus()처럼 "= delete"로 막고 싶었으나, 순가상함수(= 0)를 재정의하면서
        동시에 삭제하는 것은 표준 C++에서 금지됨 ([class.virtual]) — "삭제되지 않은 함수를
        삭제된 함수로 재정의"는 컴파일 에러. g++ -std=c++20으로 직접 재현해 확인.
        그래서 이 재정의는 본문에서 곧장 throw하는 방식으로 같은 의도("아직 호출 불가")를 표현. */
    [[nodiscard("the protobuf message owns the serialized error — dropping it leaks the allocation")]] \
    std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const override;

private:
    std::string parameter_;
};

// ---------------------------------------------------------------------------
// ExecutionError / DefinedExecutionError / UndefinedExecutionError
// ---------------------------------------------------------------------------

/// Common base for the two SiLA 2 Execution Error kinds (architecture.md §3.4):
/// a DefinedExecutionError (declared in the FDL, identified by an
/// errorIdentifier) or an UndefinedExecutionError (unexpected, identifier
/// stays empty). Never constructed directly — always through one of those
/// two subclasses.
class ExecutionError : public SiLAError {
public:
    /// @return The FQI of the Defined Error this instance represents, or
    /// empty for an Undefined Execution Error.
    [[nodiscard("caller expects the error identifier string")]] \
    std::string errorIdentifier() const;

protected:
    /// C'tor for an Undefined Execution Error.
    /// @param msg This error's message, or empty for a generic fallback.
    explicit ExecutionError(std::string msg);

    /// C'tor for a Defined Execution Error.
    /// @param identifier The FQI of the Defined Error declared in the FDL.
    /// @param msg This error's message, or empty for a generic fallback.
    ExecutionError(std::string identifier, std::string msg);

    /// Builds a DefinedExecutionError or UndefinedExecutionError protobuf
    /// message (selected by errorType()) with errorIdentifier/message fields.
    [[nodiscard("the protobuf message owns the serialized error — dropping it leaks the allocation")]] \
    std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const override;

    std::string errorIdentifier_;
};

/// Throw this from a Feature Command implementation for an execution
/// failure the Feature Designer anticipated and declared in the FDL under
/// that Command, identified by its Fully Qualified Defined Execution Error
/// Identifier.
///
/// A SiLA 2 Execution Error declared in the FDL by the Feature designer.
/// Its errorIdentifier() lets a SiLA Client react to the specific error, since
/// the error's nature and recovery options are known ahead of time. The
/// SiLA Client receives this as a @ref gl_defined_execution_error "Defined Execution Error" on the wire.
/// @see LockControllerImpl::lockServer
class DefinedExecutionError : public ExecutionError {
public:
    /// @param identifier The FQI of the Defined Error declared in the FDL.
    /// @param message This error's message, or empty for a generic fallback.
    DefinedExecutionError(std::string identifier, std::string message);
};

/// Throw this from a Feature Command implementation for an execution
/// failure that was not declared in the FDL — but a Feature implementation
/// usually never needs to construct one explicitly: any other exception
/// that escapes a Command or Property implementation is caught and wrapped
/// into one automatically (error::guardHandler), so it is enough to
/// let a std::exception (or any exception) propagate.
///
/// A SiLA 2 Execution Error that was not declared in the FDL — unexpected,
/// implementation-dependent, and not foreseeable by the Feature designer.
/// The SiLA Client receives this as an @ref gl_undefined_execution_error "Undefined Execution Error" on the wire.
/// @see error::guardHandler
class UndefinedExecutionError : public ExecutionError {
public:
    /// @param message This error's message, or empty for a generic fallback.
    explicit UndefinedExecutionError(std::string message = "");
};

// ---------------------------------------------------------------------------
// FrameworkError
// ---------------------------------------------------------------------------

/// This library's own interceptors already throw FrameworkError for the
/// protocol-level violations they can detect (an unlocked required
/// @ref gl_lock "Lock", missing SiLA Client Metadata, an invalid access
/// token). A Feature implementation should throw it directly only for a
/// protocol violation the library cannot see on its own — e.g. metadata the
/// Feature itself defines and requires but that was not sent.
///
/// A Framework Error occurs when a SiLA Client accesses a SiLA Server in a
/// way that violates the SiLA 2 specification (architecture.md §3.4), e.g. an
/// invalid Command Execution UUID or unsupported SiLA Client Metadata. The
/// SiLA Client receives this as a @ref gl_framework_error "Framework Error"
/// on the wire.
/// @see LockControllerImpl::checkLockMetadata
class FrameworkError : public SiLAError {
public:
    /// The different types of SiLA 2 Framework Errors.
    // Exactly the five values of SiLAFramework.proto:110-116. No "unset"
    // sentinel: proto3 would serialize one as 0, which is not "unknown" but
    // COMMAND_EXECUTION_NOT_ACCEPTED. Without it toProtoErrorType's switch is
    // exhaustive, so -Wswitch turns a future sixth value into a warning
    // instead of a silent 0 on the wire.
    enum class FrameworkErrorType : uint8_t {
        CommandExecutionNotAccepted,
        InvalidCommandExecutionUuid,
        CommandExecutionNotFinished,
        InvalidMetadata,
        NoMetadataAllowed,
    };

    /// @param type This framework error's type.
    /// @param message This error's message, or empty for a default message
    /// derived from type.
    explicit FrameworkError(FrameworkErrorType type, std::string message = "");

    /// @return This framework error's type.
    [[nodiscard("the framework error type drives dispatch — ignoring it misroutes error handling")]] \
    FrameworkErrorType frameworkErrorType() const;

protected:
    /// Builds a FrameworkError protobuf sub-message (errorType + message)
    /// and attaches it to the base SiLA Error message.
    [[nodiscard("the protobuf message owns the serialized error — dropping it leaks the allocation")]] \
    std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const override;

private:
    FrameworkErrorType frameworkErrorType_;
};

// ---------------------------------------------------------------------------
// ConnectionError
// ---------------------------------------------------------------------------

/// A Connection Error represents an infrastructure-level failure between a
/// SiLA Client and a SiLA Server (architecture.md §3.4) — not issued by
/// either side's SiLA code, but by the underlying gRPC transport.
class ConnectionError : public SiLAError {
public:
    /// @param status The grpc::Status that indicates a Connection Error.
    explicit ConnectionError(grpc::Status status);

    /// @return This error's gRPC status code.
    [[nodiscard("caller expects the gRPC status code")]] \
    grpc::StatusCode statusCode() const;

protected:
    /// Connection Errors are infrastructure-level gRPC failures, not SiLA
    /// protocol errors — the SiLAError oneof has no ConnectionError variant.
    /// Always throws std::logic_error; callers should use status_ directly.
    [[nodiscard("the protobuf message owns the serialized error — dropping it leaks the allocation")]] \
    std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const override;

private:
    grpc::Status status_;
};
}  // namespace error
}  // namespace sila2
