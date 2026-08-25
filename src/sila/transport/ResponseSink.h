// ResponseSink.h — transport-neutral response emitter (architecture.md §3.8)
//
// New component, not a port. gRPC-Java provides this seam natively via
// StreamObserver; gRPC-C++ does not, so this project builds its own.
// sila_cpp has no equivalent — it holds grpc::ServerWriter directly.
#pragma once

#include <sila/error/SiLAError.h>

namespace sila2 {

/// Transport-neutral interface for sending responses back to a SiLA Client
/// (architecture.md §3.8). Unobservable RPCs call send() once then finish();
/// streaming RPCs call send() repeatedly.
template <typename T>
class ResponseSink {
public:
    virtual ~ResponseSink() = default;

    /// Send a response value to the client.
    virtual void send(const T& value) = 0;

    /// Signal successful completion of the response stream.
    virtual void finish() = 0;

    /// Signal an error and terminate the response stream.
    virtual void fail(const error::SiLAError& error) = 0;
};

}  // namespace sila2
