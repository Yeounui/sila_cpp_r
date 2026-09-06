// SiLAError.h
//
// Ported from sila_cpp v0.3.11
// src/include/sila_cpp/framework/error_handling/SiLAError.h (MIT License,
// Copyright 2020 SiLA2). The original derives from QException and keeps its
// state behind a polymorphic_value PIMPL (SiLAError_p.h) for Qt shared-library
// ABI stability and QtConcurrent's cross-thread exception propagation, neither
// of which this project uses. This port derives from std::runtime_error
// instead (this repo's existing convention — see CryptoError in
// src/sila/server/config/TlsConfig.h) and drops the PIMPL entirely: state lives
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

namespace sila2 {
namespace error {
/// Abstract base class for all SiLA 2 error types (architecture.md §3.4).
/// Never constructed directly — always through a derived
/// ValidationError/ExecutionError/FrameworkError.

/*  std::runtime_error을 상속해 SiLA 2 에러 계층의 기반 클래스를 구성.
    std::runtime_error가 메시지 저장과 what()을 제공하므로, ErrorType 등 SiLA 고유 상태만 추가하면 됨.

    std::runtime_error 상속 패턴 예시:
    1. 생성자에서 사용자 정의 에러 추가
        class MyError : public std::runtime_error {
        public:
            MyError(int code, const std::string& msg): std::runtime_error(msg), code_(code) {}
            int code() const { return code_; }
        private:
            int code_;
        };
    2. 사용자 정의 에러 사용.
        throw MyError(42, "something broke");

        try { ... }
        catch (const MyError& e) {
            e.what();  // "something broke"
            e.code();  // 42
        }
*/
class SiLAError : public std::runtime_error {
public:
    /// Defines all the different SiLA 2 error types.
    enum class ErrorType : uint8_t {
        DefinedExecutionError,
        UndefinedExecutionError,
        FrameworkError,
        ValidationError,
        ConnectionError,
    };

    /// @return This error's type.
    [[nodiscard("the error type drives dispatch — ignoring it misroutes error handling")]] \
    ErrorType errorType() const;

    /// Convenience method, equivalent to
    /// SiLAError::errorTypeToString(someError.errorType()).
    /// @return This error's type's human-readable string representation.
    [[nodiscard("caller expects the type name string")]] \
    std::string errorTypeName() const;

    /// @param type The ErrorType to convert.
    /// @return type's human-readable string representation.
    [[nodiscard("caller expects the type name string")]] \
    static std::string errorTypeToString(ErrorType type);

    /// Converts the SiLA error to a grpc::Status that can be sent along with an RPC.
    /// @return The gRPC status that corresponds to this particular SiLA error.
    /*  grpc::Status toStatus() const = delete;
            다른 곳에서 error.toStatus() 호출 시 컴파일 에러.
            "이 함수는 존재하지만 쓰면 안 된다"를 명시적으로 표현.
        가장 흔한 용도는 복사 금지:
        MyClass(const MyClass&) = delete;            // 복사 생성 금지
        MyClass& operator=(const MyClass&) = delete; // 복사 대입 금지*/
    [[nodiscard("the gRPC Status carries the error — dropping it silently loses the failure")]] \
    grpc::Status toStatus() const;

    [[nodiscard("the protobuf message owns the serialized error — dropping it leaks the allocation")]] \
    std::unique_ptr<sila2::org::silastandard::SiLAError> toProto() const;

protected:
    /// C'tor for derived classes. Falls back to a generic message when msg is empty.
    /// @param type This error's type.
    /// @param msg This error's message, or empty for a generic fallback.
    SiLAError(ErrorType type, std::string msg);

    /// Builds this error's SiLA Error protobuf message.
    /// The caller takes ownership of the returned message.
    /// @return A pointer to the SiLA Error protobuf message.
    [[nodiscard("the protobuf message owns the serialized error — dropping it leaks the allocation")]] \
    virtual std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const = 0;

    ErrorType type_;
};
}  // namespace error
}  // namespace sila2
