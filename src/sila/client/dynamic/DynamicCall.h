// DynamicCall.h — GenericStub-based dynamic gRPC calls (architecture.md §4.2)
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/status.h>

#include <sila/client/dynamic/ValueValidator.h>

namespace sila2 { class MetadataInjector; }
namespace google { namespace protobuf { class Message; }}

namespace sila2 {
namespace dynamic {

class FeatureCatalog;

/// Makes one unary gRPC call by its raw gRPC method path, without a
/// FeatureCatalog and without @ref gl_constraint "Constraint" validation.
/// @param method gRPC method path, e.g. as returned by
/// FeatureCatalog::grpcMethodName().
/// @param injector when non-null, attaches @ref gl_sila_client_metadata "SiLA Client Metadata" to
/// the call.
/// @return the RPC's status; response is populated only on grpc::Status::OK.
[[nodiscard]]
grpc::Status callUnary(
    const std::shared_ptr<grpc::Channel>& channel,
    const std::string& method,
    const grpc::ByteBuffer& request,
    grpc::ByteBuffer* response,
    MetadataInjector* injector = nullptr);

/// Makes one unary gRPC call for a @ref gl_command "Command" or @ref gl_property "Property"
/// registered in catalog: validates request against
/// its FDL @ref gl_constraint "Constraints", serializes it, and sends it.
/// @return grpc::StatusCode::INVALID_ARGUMENT (without a transport round trip)
/// when constraint validation fails; otherwise the RPC's status.
/// @see FeatureCatalog::validateRequest for the validation this wraps.
[[nodiscard]]
grpc::Status callUnary(
    const std::shared_ptr<grpc::Channel>& channel,
    const FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& rpcName,
    const google::protobuf::Message& request,
    grpc::ByteBuffer* response,
    ConstraintResolver constraintResolver = {},
    MetadataInjector* injector = nullptr);

/// Invoked once per message a server-streaming call delivers. Return true to
/// keep receiving, false to end the stream early.
using StreamCallback = std::function<bool(const grpc::ByteBuffer& message)>;

/// Makes one server-streaming gRPC call by its raw gRPC method path, such as
/// an @ref gl_observable_property "Observable Property" subscription or an
/// @ref gl_observable_command "Observable Command" execution info /
/// @ref gl_intermediate_command_response "Intermediate Response" stream.
/// @param onMessage called for each streamed message; see StreamCallback.
/// @return the stream's final status once it ends or onMessage stops it early.
[[nodiscard]]
grpc::Status callServerStream(
    const std::shared_ptr<grpc::Channel>& channel,
    const std::string& method,
    const grpc::ByteBuffer& request,
    StreamCallback onMessage,
    MetadataInjector* injector = nullptr);

}  // namespace dynamic
}  // namespace sila2
