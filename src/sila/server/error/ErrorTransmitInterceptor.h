// ErrorTransmitInterceptor.h — SiLA error boundary around handler invocation (architecture.md §3.4)
//
// New component, not a port. sila_cpp has no equivalent interceptor pattern.
#pragma once

#include <functional>

#include <sila/common/error/SiLAError.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/transport/ResponseSink.h>

namespace sila2 {
namespace error {

/// Wraps a handler invocation so that any exception it throws reaches the
/// client as a SiLA error rather than crashing the transport layer or
/// leaking a non-SiLA exception type across the RPC boundary.
///
/// Runs handler and routes any exception it throws to sink.fail().
/// @param handler The Command/Property handler body to run.
/// @param sink The response sink that reports success/failure to the client.
template <typename Resp>
void guardHandler(std::function<void()> handler, ResponseSink<Resp>& sink) {
    try {
        handler();
    } catch (const SiLAError& e) {
        sink.fail(e);
    } catch (const std::exception& e) {
        sink.fail(UndefinedExecutionError{e.what()});
    } catch (...) {
        sink.fail(UndefinedExecutionError{"unknown exception"});
    }
}

}  // namespace error
}  // namespace sila2
