// SiLAErrorException.h
#pragma once

#include <memory>
#include <string>

namespace grpc
{
class Status;
}  // namespace grpc

namespace sila2 {
namespace error {
class SiLAError;

/// Reconstructs a typed SiLAError from a grpc::Status received over the wire
/// (architecture.md §3.4). ABORTED statuses carrying a valid serialized
/// SiLAError protobuf in their binary details yield the matching
/// ValidationError/DefinedExecutionError/UndefinedExecutionError/
/// FrameworkError; any other status code is treated as an infrastructure-level
/// failure and yields a ConnectionError. A details parse failure on an
/// ABORTED status falls back to an UndefinedExecutionError built from
/// status.error_message(), since the payload can't be trusted as a SiLA error.
/// @param status The grpc::Status to reconstruct a SiLAError from.
/// @return The reconstructed SiLAError, owned by the caller.
[[nodiscard("the reconstructed error carries the failure — dropping it silently loses the error")]] \
std::unique_ptr<SiLAError> fromGrpcStatus(const grpc::Status& status);

/// Returns a human-readable SiLA error message, preserving transport messages.
std::string messageFromGrpcStatus(const grpc::Status& status);
}  // namespace error
}  // namespace sila2
