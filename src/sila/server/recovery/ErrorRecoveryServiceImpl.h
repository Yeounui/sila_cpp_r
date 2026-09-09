// ErrorRecoveryServiceImpl.h — SiLA2 error recovery feature (architecture.md §3.12)
#pragma once

#include "ErrorRecoveryService.grpc.pb.h"

#include <sila/server/transport/SilaHandler.h>

#include <string_view>
#include <vector>

namespace sila2 {
class ObservablePropertyManager;
struct InterceptorChain;
}

namespace sila2::recovery {
class RecoverableErrorGate;
struct RecoverableError;
}

namespace sila2 {

// FQI for ErrorRecoveryService — used by Builder::Build() to auto-register.
// v1 is not advertised: no v1 gRPC service exists (S8b, architecture-v2.md §3.12).
inline constexpr std::string_view kErrorRecoveryServiceFqi =
    "org.silastandard/core/ErrorRecoveryService/v2";

// Per-RPC granular authorization FQIs. The direct-gRPC auth gate matches a
// call's FQI against protectedFqis; passing these Command/Property-level FQIs
// (not the feature-level one above) lets a command-level protectedFqis entry
// actually gate the matching RPC, keeping direct-gRPC coverage aligned with
// the cloud path. Full-literal string_view, not runtime concatenation, to stay
// constexpr and match kAuthorizationProviderParamFqi's existing style. v2 here,
// matching kErrorRecoveryServiceFqi above — no v2 gRPC service exists at v1.
inline constexpr std::string_view kExecuteContinuationOptionFqi =
    "org.silastandard/core/ErrorRecoveryService/v2/Command/ExecuteContinuationOption";
inline constexpr std::string_view kAbortErrorHandlingFqi =
    "org.silastandard/core/ErrorRecoveryService/v2/Command/AbortErrorHandling";
inline constexpr std::string_view kSetErrorHandlingTimeoutFqi =
    "org.silastandard/core/ErrorRecoveryService/v2/Command/SetErrorHandlingTimeout";
inline constexpr std::string_view kSubscribe_RecoverableErrorsFqi =
    "org.silastandard/core/ErrorRecoveryService/v2/Property/RecoverableErrors";

// Returns the FDL XML for ErrorRecoveryService v2, embedded as a string constant.
const std::string& errorRecoveryServiceFdlXml();

// Namespace alias shortens the generated proto namespace for readability.
namespace errorrecovery_proto = sila2::org::silastandard::core::errorrecoveryservice::v2;

/// Fills a Subscribe_RecoverableErrors_Responses from the gate's published
/// error list. Shared by the direct-gRPC subscription and the cloud observable-
/// property registration in SiLAServerBase.cc so the two transports cannot
/// drift into different wire shapes.
void fillRecoverableErrorsResponse(
    const std::vector<recovery::RecoverableError>& errors,
    errorrecovery_proto::Subscribe_RecoverableErrors_Responses& out);

/// The gRPC-facing ErrorRecoveryService Feature: gives a client the three
/// commands and one @ref gl_observable_property "Observable Property" it
/// needs to resolve a @ref sila2::recovery::RecoverableErrorGate "recoverable error" a
/// Feature raised -- select a ContinuationOption, abort error handling, or
/// change how long the server waits before giving up. Installed
/// automatically by `SiLAServerBase::Builder::WithErrorRecovery()`; a server
/// author does not construct it directly.
class ErrorRecoveryServiceImpl final : public errorrecovery_proto::ErrorRecoveryService::Service {
public:
    // gate and propMgr must outlive this object.
    ErrorRecoveryServiceImpl(recovery::RecoverableErrorGate& gate,
                              ObservablePropertyManager& propMgr,
                              const InterceptorChain* chain = nullptr);

    // ---- Commands ----

    grpc::Status ExecuteContinuationOption(
        grpc::ServerContext* context,
        const errorrecovery_proto::ExecuteContinuationOption_Parameters* request,
        errorrecovery_proto::ExecuteContinuationOption_Responses* response) override;

    grpc::Status AbortErrorHandling(
        grpc::ServerContext* context,
        const errorrecovery_proto::AbortErrorHandling_Parameters* request,
        errorrecovery_proto::AbortErrorHandling_Responses* response) override;

    grpc::Status SetErrorHandlingTimeout(
        grpc::ServerContext* context,
        const errorrecovery_proto::SetErrorHandlingTimeout_Parameters* request,
        errorrecovery_proto::SetErrorHandlingTimeout_Responses* response) override;

    // ---- Observable Property (server-streaming) ----

    grpc::Status Subscribe_RecoverableErrors(
        grpc::ServerContext* context,
        const errorrecovery_proto::Subscribe_RecoverableErrors_Parameters* request,
        grpc::ServerWriter<errorrecovery_proto::Subscribe_RecoverableErrors_Responses>* writer) override;

    /// Transport-neutral handler body shared by the gRPC ExecuteContinuationOption
    /// override above and the cloud transport path; resolves gate_'s matching
    /// raiseAndWait() with the client's choice.
    void executeContinuationOption(
        const errorrecovery_proto::ExecuteContinuationOption_Parameters& request,
        CallContext& ctx,
        ResponseSink<errorrecovery_proto::ExecuteContinuationOption_Responses>& sink);
    void abortErrorHandling(const errorrecovery_proto::AbortErrorHandling_Parameters& request,
                            CallContext& ctx,
                            ResponseSink<errorrecovery_proto::AbortErrorHandling_Responses>& sink);
    void setErrorHandlingTimeout(
        const errorrecovery_proto::SetErrorHandlingTimeout_Parameters& request,
        CallContext& ctx,
        ResponseSink<errorrecovery_proto::SetErrorHandlingTimeout_Responses>& sink);
    void subscribeRecoverableErrors(
        const errorrecovery_proto::Subscribe_RecoverableErrors_Parameters& request,
        CallContext& ctx,
        ResponseSink<errorrecovery_proto::Subscribe_RecoverableErrors_Responses>& sink);

private:
    recovery::RecoverableErrorGate& gate_;
    ObservablePropertyManager& propMgr_;
    const InterceptorChain* chain_;
};

}  // namespace sila2
