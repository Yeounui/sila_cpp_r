// GrpcTransport.cc
#include "GrpcTransport.h"

#include <chrono>
#include <memory>

namespace sila2 {

std::unique_ptr<CallContext> makeCallContext(grpc::ServerContext* server_ctx) {
    // gRPC C++ offers no cancellation callback, so the context asks the
    // ServerContext on every isCancelled() instead of snapshotting it once at
    // dispatch time — a snapshot can only ever report a call cancelled before
    // the handler started, which is never the case a polling handler cares
    // about. grpc::ServerContext::IsCancelled() also covers deadline expiry
    // and outlives the CallContext, which dispatchToHandler destroys before
    // the service method returns (see CallContext's probe contract).
    auto ctx = std::make_unique<CallContext>(
        [server_ctx] { return server_ctx->IsCancelled(); });

    // gRPC expresses deadlines as system_clock time_points. CallContext uses
    // steady_clock (immune to wall-clock adjustments). Convert by computing
    // the remaining duration and adding it to steady_clock::now().
    // gRPC returns time_point::max() when no deadline was set by the client.
    auto grpc_deadline = server_ctx->deadline();
    if (grpc_deadline != std::chrono::system_clock::time_point::max()) {
        auto remaining = grpc_deadline - std::chrono::system_clock::now();
        ctx->setDeadline(std::chrono::steady_clock::now() + remaining);
    }

    return ctx;
}

}  // namespace sila2
