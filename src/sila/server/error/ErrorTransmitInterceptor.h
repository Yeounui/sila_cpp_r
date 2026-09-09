// ErrorTransmitInterceptor.h — SiLA error boundary around handler invocation (architecture.md §3.4)
//
// New component, not a port. sila_cpp has no equivalent interceptor pattern.
#pragma once

#include <functional>

#include <sila/common/error/SilaError.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/transport/ResponseSink.h>

namespace sila2 {
namespace error {

/// Wraps a handler invocation so that any exception it throws reaches the
/// client as a SiLA error rather than crashing the transport layer or
/// leaking a non-SiLA exception type across the RPC boundary.
///
/// A Feature implementation throws sila2::error::ValidationError,
/// DefinedExecutionError, UndefinedExecutionError, or FrameworkError to
/// report a failure in SiLA vocabulary; guardHandler catches it here and
/// forwards it to the client as that exact @ref gl_validation_error "Validation Error" /
/// @ref gl_defined_execution_error "Defined Execution Error" /
/// @ref gl_undefined_execution_error "Undefined Execution Error" /
/// @ref gl_framework_error "Framework Error", unchanged. A plain
/// std::exception (or any other thrown value) is not SiLA vocabulary, so it
/// is downgraded to an Undefined Execution Error carrying `what()` (or a
/// fixed "unknown exception" message) rather than reaching the client as-is.
///
/// Runs handler and routes any exception it throws to sink.fail().
/// @param handler The Command/Property handler body to run.
/// @param sink The response sink that reports success/failure to the client.
template <typename Resp>
void guardHandler(std::function<void()> handler, ResponseSink<Resp>& sink) {
    try {
        handler();
    } catch (const SilaError& e) {
        sink.fail(e);
    } catch (const std::exception& e) {
        sink.fail(UndefinedExecutionError{e.what()});
    } catch (...) {
        sink.fail(UndefinedExecutionError{"unknown exception"});
    }
}

}  // namespace error
}  // namespace sila2
