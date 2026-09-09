// ResponseSink.h — transport-neutral response emitter (architecture.md §3.8)
//
// New component, not a port. gRPC-Java provides this seam natively via
// StreamObserver; gRPC-C++ does not, so this project builds its own.
// sila_cpp has no equivalent — it holds grpc::ServerWriter directly.
#pragma once

#include <sila/common/error/SilaError.h>

namespace sila2 {

/// How a @ref gl_feature "Feature" handler returns its result, or an error, to the
/// @ref gl_sila_client "SiLA Client" ; a handler receives one as its last argument (see
/// SilaHandler).
///
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

    /// Signal an error and terminate the response stream: the client receives
    /// @p error as the SiLA error it represents (see sila2::error::SilaError and its subtypes).
    virtual void fail(const error::SilaError& error) = 0;
};

}  // namespace sila2
