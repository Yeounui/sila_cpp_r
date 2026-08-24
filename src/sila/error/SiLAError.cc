// SiLAError.cc
//
// Ported from sila_cpp v0.3.11
// src/lib/framework/error_handling/SiLAError.cpp (MIT License, Copyright 2020
// SiLA2). toStatus() and what() are "= delete"d in SiLAError.h, not defined
// here — both depend on SerializeAsString()/DebugString() of the generated
// sila2::org::silastandard::SiLAError protobuf message, and that codegen
// (third_party/sila_base/protobuf/SiLAFramework.proto) is not wired into the
// build yet.
#include "SiLAError.h"

#include <utility>

namespace sila2
{
namespace error
{
namespace
{
// Falls back to a generic message when msg is empty, matching the
// reference's PrivateImpl::PrivateImpl behavior. Computed ahead of the base
// std::runtime_error construction below, since the base has to be
// initialized with the final message in the member-initializer list.
std::string resolveMessage(SiLAError::ErrorType type, std::string msg)
{
    if (!msg.empty())
    {
        return msg;
    }
    // ponytail: no logging subsystem yet, add calls back when one exists
    return "A " + SiLAError::errorTypeToString(type)
           + " occurred while executing a SiLA 2 Command or reading a "
             "Property!";
}
}  // namespace

SiLAError::SiLAError(ErrorType type, std::string msg)
    : std::runtime_error{resolveMessage(type, std::move(msg))}, type_{type}
{}

std::string SiLAError::message() const
{
    // SiLAError does not declare its own what() (see the ctor's TODO), so
    // this reads runtime_error's own stored message directly rather than
    // going through a virtual call.
    return std::runtime_error::what();
}

SiLAError::ErrorType SiLAError::errorType() const { return type_; }

std::string SiLAError::errorTypeName() const { return errorTypeToString(type_); }

std::string SiLAError::errorTypeToString(ErrorType type)
{
    switch (type)
    {
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
