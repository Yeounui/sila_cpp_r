// SiLAError.h
//
// Ported from sila_cpp v0.3.11
// src/include/sila_cpp/framework/error_handling/SiLAError.h (MIT License,
// Copyright 2020 SiLA2). The original derives from QException and keeps its
// state behind a polymorphic_value PIMPL (SiLAError_p.h) for Qt shared-library
// ABI stability and QtConcurrent's cross-thread exception propagation, neither
// of which this project uses. This port derives from std::runtime_error
// instead (this repo's existing convention — see OpenSslError in
// src/sila/config/TlsConfig.h) and drops the PIMPL entirely: state lives
// directly as protected members, with the message stored by
// std::runtime_error itself rather than duplicated in a second member.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace grpc
{
class Status;
}  // namespace grpc

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

namespace sila2
{
namespace error
{
/// Abstract base class for all SiLA 2 error types (architecture.md §3.4).
/// Never constructed directly — always through a derived
/// ValidationError/ExecutionError/FrameworkError.
class SiLAError : public std::runtime_error
{
public:
    /// Defines all the different SiLA 2 error types.
    enum class ErrorType : uint8_t
    {
        DefinedExecutionError,
        UndefinedExecutionError,
        FrameworkError,
        ValidationError,
        ConnectionError,
    };

    /// @return This error's message with details about the occurred error.
    [[nodiscard]] std::string message() const;

    /// @return This error's type.
    [[nodiscard]] ErrorType errorType() const;

    /// Convenience method, equivalent to
    /// SiLAError::errorTypeToString(someError.errorType()).
    /// @return This error's type's human-readable string representation.
    [[nodiscard]] std::string errorTypeName() const;

    /// @param type The ErrorType to convert.
    /// @return type's human-readable string representation.
    [[nodiscard]] static std::string errorTypeToString(ErrorType type);

    /// Converts the SiLA error to a grpc::Status that can be sent along with
    /// an RPC.
    /// @return The gRPC status that corresponds to this particular SiLA
    /// error.
    /// TODO(owner): implement once SiLAFramework.pb.h codegen is wired into
    /// the build, then remove the "= delete". Must return
    /// grpc::StatusCode::ABORTED with the error message's
    /// SerializeAsString() base64-encoded as its detail string
    /// (internal::base64Encode, from Base64.h, is not ported yet either).
    [[nodiscard]] grpc::Status toStatus() const = delete;

protected:
    /// C'tor for derived classes. Falls back to a generic message when msg
    /// is empty.
    /// @param type This error's type.
    /// @param msg This error's message, or empty for a generic fallback.
    /// TODO(owner): once SiLAFramework.pb.h codegen is wired into the build,
    /// add `const char* what() const noexcept override` returning
    /// DebugString() of makeErrorMessage() — for now this class inherits
    /// std::runtime_error::what(), which already returns the message set
    /// below.
    SiLAError(ErrorType type, std::string msg);

    /// Builds this error's SiLA Error protobuf message. The caller takes
    /// ownership of the returned message.
    /// @return A pointer to the SiLA Error protobuf message.
    [[nodiscard]] virtual std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const = 0;

    ErrorType type_;
};
}  // namespace error
}  // namespace sila2
