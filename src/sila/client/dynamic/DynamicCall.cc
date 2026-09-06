// DynamicCall.cc — GenericStub-based dynamic gRPC calls (architecture.md §4.2)
#include <sila/client/dynamic/DynamicCall.h>

#include <sila/client/MetadataInjector.h>
#include <sila/client/dynamic/FeatureCatalog.h>

#include <grpcpp/client_context.h>
#include <grpcpp/completion_queue.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/support/async_stream.h>
#include <grpcpp/support/async_unary_call.h>

#include <string_view>

namespace sila2 {
namespace dynamic {

namespace {
// SiLAService must not receive SiLA Client Metadata (SiLA 2 Part A: "every
// call of the SiLA Service Feature MUST NOT contain any SiLA Client
// Metadata"), and SilaClientBase attaches the lock identifier in its
// constructor and the access token after login -- for the client's whole
// lifetime, not per call. So the skip belongs here, the only layer that
// knows which method the injector is about to decorate.
// MetadataInjector::apply() is reached from nowhere else: generated static
// stubs never call it.
//
// Matched on the gRPC method path rather than the SiLA FQI because the path
// is the only identifier these two functions have. Derived from
// SiLAService.proto:5 (package) and :10 (service name).
constexpr std::string_view kSiLAServiceMethodPrefix =
    "/sila2.org.silastandard.core.silaservice.v1.SiLAService/";

bool targetsSiLAService(const std::string& method) {
    return method.starts_with(kSiLAServiceMethodPrefix);
}

grpc::ByteBuffer serialized(const google::protobuf::Message& message) {
    grpc::Slice slice(message.SerializeAsString());
    return grpc::ByteBuffer(&slice, 1);
}

}  // namespace

grpc::Status callUnary(
    const std::shared_ptr<grpc::Channel>& channel,
    const std::string& method,
    const grpc::ByteBuffer& request,
    grpc::ByteBuffer* response,
    MetadataInjector* injector) {
    grpc::GenericStub stub{channel};
    grpc::ClientContext context;
    if (injector && !targetsSiLAService(method)) {
        injector->apply(context);
    }
    // Vendored gRPC's GenericStub exposes only the async unary path
    // (PrepareUnaryCall / Finish), no blocking `Status UnaryCall(...)`
    // overload. Block on a local CompletionQueue to give callers a
    // synchronous call, per architecture.md §4.2 ("synchronous calls first").
    grpc::CompletionQueue cq;
    grpc::Status status;
    auto reader = stub.PrepareUnaryCall(&context, method, request, &cq);
    reader->StartCall();
    reader->Finish(response, &status, nullptr);

    void* tag = nullptr;
    bool ok = false;
    cq.Next(&tag, &ok);
    cq.Shutdown();
    return status;
}

grpc::Status callUnary(
    const std::shared_ptr<grpc::Channel>& channel, const FeatureCatalog& catalog,
    const std::string& fqi, const std::string& rpcName,
    const google::protobuf::Message& request, grpc::ByteBuffer* response,
    ConstraintResolver constraintResolver, MetadataInjector* injector) {
    if (const auto error = catalog.validateRequest(fqi, rpcName, request, constraintResolver)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, *error);
    }
    return callUnary(channel, catalog.grpcMethodName(fqi, rpcName), serialized(request), response, injector);
}

grpc::Status callServerStream(
    const std::shared_ptr<grpc::Channel>& channel,
    const std::string& method,
    const grpc::ByteBuffer& request,
    StreamCallback onMessage,
    MetadataInjector* injector) {
    grpc::GenericStub stub{channel};
    grpc::ClientContext context;
    if (injector && !targetsSiLAService(method)) {
        injector->apply(context);
    }

    grpc::CompletionQueue cq;
    void* tag = nullptr;
    bool ok = false;

    auto stream = stub.PrepareCall(&context, method, &cq);

    stream->StartCall(tag);
    cq.Next(&tag, &ok);

    stream->Write(request, tag);
    cq.Next(&tag, &ok);

    stream->WritesDone(tag);
    cq.Next(&tag, &ok);

    // Read loop: consume server messages until the stream ends
    grpc::ByteBuffer response;
    bool earlyStopped = false;
    while (true) {
        stream->Read(&response, tag);
        if (!cq.Next(&tag, &ok) || !ok) {
            break;
        }
        if (!onMessage(response)) {
            earlyStopped = true;
            break;
        }
    }

    // When the caller stopped early the server may still be writing;
    // TryCancel unblocks Finish which otherwise waits for the server
    // to close the stream.
    if (earlyStopped) {
        context.TryCancel();
    }

    grpc::Status status;
    stream->Finish(&status, tag);
    cq.Next(&tag, &ok);
    cq.Shutdown();
    return status;
}

}  // namespace dynamic
}  // namespace sila2
