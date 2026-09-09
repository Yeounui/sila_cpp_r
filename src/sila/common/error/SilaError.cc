// SilaError.cc
//
// Ported from sila_cpp v0.3.11
// src/lib/framework/error_handling/SilaError.cpp (MIT License, Copyright 2020
// SiLA2).
#include "SilaError.h"

#include <utility>

#include <grpcpp/support/status.h>
#include <openssl/evp.h>

#include <sila/common/util/base64.h>

#include "SiLAFramework.pb.h"

namespace sila2 {
namespace error {
namespace {
// Falls back to a generic message when msg is empty,
// matching the reference's PrivateImpl::PrivateImpl behavior.
// Computed ahead of the base std::runtime_error construction below,
// since the base has to be initialized with the final message in the member-initializer list.
std::string resolveMessage(SilaError::ErrorType type, std::string msg) {
    if (!msg.empty()) {
        return msg;
    }
    // ponytail: no logging subsystem yet, add calls back when one exists
    return "A " + SilaError::errorTypeToString(type)
           + " occurred while executing a SiLA 2 Command or reading a "
             "Property!";
}
}  // namespace

SilaError::SilaError(ErrorType type, std::string msg)
    : std::runtime_error{resolveMessage(type, std::move(msg))}, type_{type} {}

SilaError::ErrorType SilaError::errorType() const { return type_; }

/*  errorTypeName()과 errorTypeToString()을 나눈 이유:

    errorTypeName()은 에러 인스턴스 내에서 errorTypeToString(type_)을 내부 호출.
        catch (const SilaError& e) {
            log(e.errorTypeName());  // errorTypeToString(e.errorType())와 동일
        }
*/
std::string SilaError::errorTypeName() const { return errorTypeToString(type_); }

// SiLA 2 §3.4: gRPC's error message carries the base64-encoded SilaError
// payload for clients such as sila2-python; retain raw bytes in error_details
// for the native status round-trip.
grpc::Status SilaError::toStatus() const {
    auto errorMsg = makeErrorMessage();
    std::string serialized = errorMsg->SerializeAsString();
    return grpc::Status{grpc::StatusCode::ABORTED, base64Encode(serialized), serialized};
}

std::unique_ptr<sila2::org::silastandard::SiLAError> SilaError::toProto() const {
    return makeErrorMessage();
}

/*
    errorTypeToString()은 static — 에러 객체 없이 타입 이름만 필요할 때 (로깅, UI 표시 등)
    ErrorType 값만으로 문자열 변환 가능.
        log("expected: " + SilaError::errorTypeToString(ErrorType::ValidationError));
*/
std::string SilaError::errorTypeToString(ErrorType type) {
    switch (type) {
    case ErrorType::DefinedExecutionError:
        return "Defined Execution Error";
    case ErrorType::UndefinedExecutionError:
        return "Undefined Execution Error";
    case ErrorType::FrameworkError:
        return "Framework Error";
    case ErrorType::ValidationError:
        return "Validation Error";
    case ErrorType::ConnectionError:
        return "Connection Error";
    }
    // ponytail: no logging subsystem yet, add calls back when one exists
    return "";
}
}  // namespace error
}  // namespace sila2
