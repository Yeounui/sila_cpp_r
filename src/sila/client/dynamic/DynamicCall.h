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

[[nodiscard]]
grpc::Status callUnary(
    const std::shared_ptr<grpc::Channel>& channel,
    const std::string& method,
    const grpc::ByteBuffer& request,
    grpc::ByteBuffer* response,
    MetadataInjector* injector = nullptr);

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

using StreamCallback = std::function<bool(const grpc::ByteBuffer& message)>;

[[nodiscard]]
grpc::Status callServerStream(
    const std::shared_ptr<grpc::Channel>& channel,
    const std::string& method,
    const grpc::ByteBuffer& request,
    StreamCallback onMessage,
    MetadataInjector* injector = nullptr);

}  // namespace dynamic
}  // namespace sila2
