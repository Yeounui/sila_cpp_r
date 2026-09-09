// SilaHandler.h — typed handler signature Feature implementations define
// (architecture.md §3.8)
//
// New component. GrpcTransport and CloudEnvelopeRouter each adapt their
// native call model to invoke this signature.
#pragma once

#include <functional>

namespace sila2 {

class CallContext;
template <typename T>
class ResponseSink;

/// The callback signature a @ref gl_feature "Feature" implementer writes for
/// each @ref gl_command "Command" or @ref gl_property "Property" RPC.
/// Codegen generates one `SilaHandler` member per RPC on the Feature's
/// ServiceAdapter (e.g. `adapter_->onShakeForTime`); the implementer assigns
/// a lambda or bound member function to it. The transport adapter
/// (GrpcTransport.h's dispatchToHandler, or the cloud router) invokes the
/// handler with the parsed request, a CallContext for reading metadata and
/// cancellation, and a ResponseSink for returning the result.
///
/// @code{.cpp}
/// adapter_->onStopShaking = [this](
///         const shake_proto::StopShaking_Parameters& req,
///         sila2::CallContext& ctx,
///         sila2::ResponseSink<shake_proto::StopShaking_Responses>& sink) {
///     if (!shaking_) {
///         sink.fail(sila2::error::DefinedExecutionError{
///             std::string{gen::kError_CancelledError}, "Not currently shaking"});
///         return;
///     }
///     shaking_ = false;
///     sink.send(shake_proto::StopShaking_Responses{});
///     sink.finish();
/// };
/// @endcode
/// (tests/examples/shake_controller/ShakeControllerImpl.cc)
template <typename Req, typename Resp>
using SilaHandler = std::function<void(const Req&, CallContext&, ResponseSink<Resp>&)>;

}  // namespace sila2
