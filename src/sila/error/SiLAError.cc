// SiLAError.cc
//
// Ported from sila_cpp v0.3.11
// src/lib/framework/error_handling/SiLAError.cpp (MIT License, Copyright 2020
// SiLA2). toStatus() and what() are "= delete"d / not overridden in SiLAError.h —
// both depend on SerializeAsString()/DebugString() of the generated
// sila2::org::silastandard::SiLAError protobuf message, and that codegen
// (third_party/sila_base/protobuf/SiLAFramework.proto) is not wired into the build yet.
#include "SiLAError.h"

#include <utility>

namespace sila2 {
namespace error {
namespace {
// Falls back to a generic message when msg is empty,
// matching the reference's PrivateImpl::PrivateImpl behavior.
// Computed ahead of the base std::runtime_error construction below,
// since the base has to be initialized with the final message in the member-initializer list.
std::string resolveMessage(SiLAError::ErrorType type, std::string msg) {
    if (!msg.empty()) {
        return msg;
    }
    // ponytail: no logging subsystem yet, add calls back when one exists
    return "A " + SiLAError::errorTypeToString(type)
           + " occurred while executing a SiLA 2 Command or reading a "
             "Property!";
}
}  // namespace

SiLAError::SiLAError(ErrorType type, std::string msg)
    : std::runtime_error{resolveMessage(type, std::move(msg))}, type_{type} {}

std::string SiLAError::message() const {
    // SiLAError does not declare its own what() (see the ctor's TODO), so
    // this reads runtime_error's own stored message directly rather than going through a virtual call.
    return std::runtime_error::what();
}

SiLAError::ErrorType SiLAError::errorType() const { return type_; }

/*  errorTypeName()과 errorTypeToString()을 나눈 이유:

    errorTypeName()은 에러 인스턴스 내에서 errorTypeToString(type_)을 내부 호출.
        catch (const SiLAError& e) {
            log(e.errorTypeName());  // errorTypeToString(e.errorType())와 동일
        }
*/
std::string SiLAError::errorTypeName() const { return errorTypeToString(type_); }
/*
    errorTypeToString()은 static — 에러 객체 없이 타입 이름만 필요할 때 (로깅, UI 표시 등)
    ErrorType 값만으로 문자열 변환 가능.
        log("expected: " + SiLAError::errorTypeToString(ErrorType::ValidationError));
*/
std::string SiLAError::errorTypeToString(ErrorType type) {
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
