// ErrorRecoveryServiceImpl.cc — SiLA2 error recovery feature (architecture.md §3.12)
#include "ErrorRecoveryServiceImpl.h"

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/types/BasicTypes.h>
#include <sila/common/types/Constraints.h>
#include <sila/server/recovery/ErrorRecoveryFdl.h>
#include <sila/server/recovery/RecoverableErrorGate.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/server/transport/GrpcTransport.h>

#include <any>
#include <chrono>
#include <string>
#include <vector>

namespace sila2 {
namespace {

const std::string kInvalidUuidErrorId =
    "org.silastandard/core/ErrorRecoveryService/v2/DefinedExecutionError/InvalidCommandExecutionUUID";

const std::string kFdlXml = generated::kErrorRecoveryFdlXml;

// ErrorRecoveryService-v2_0.sila.xml:71-74 and :121-124 constrain
// CommandExecutionUUID to Length 36 plus this lowercase-hex UUID Pattern —
// same shape and rationale as AuthorizationConfigurationServiceImpl.cc:27-28
// (hyphens unescaped: std::regex's ECMAScript grammar doesn't need \- outside
// a character class the way the FDL's XSD spelling does).
const std::string kUuidPattern =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

const std::string kExecuteContinuationOptionUuidParamFqi =
    "org.silastandard/core/ErrorRecoveryService/v2/Command/"
    "ExecuteContinuationOption/Parameter/CommandExecutionUUID";

const std::string kAbortErrorHandlingUuidParamFqi =
    "org.silastandard/core/ErrorRecoveryService/v2/Command/"
    "AbortErrorHandling/Parameter/CommandExecutionUUID";

const std::string kSetErrorHandlingTimeoutParamFqi =
    "org.silastandard/core/ErrorRecoveryService/v2/Command/"
    "SetErrorHandlingTimeout/Parameter/Timeout";

}  // namespace

const std::string& errorRecoveryServiceFdlXml() { return kFdlXml; }

void fillRecoverableErrorsResponse(
    const std::vector<recovery::RecoverableError>& errors,
    errorrecovery_proto::Subscribe_RecoverableErrors_Responses& out) {
    out.clear_recoverableerrors();
    for (const auto& error : errors) {
        auto* item = out.add_recoverableerrors()->mutable_recoverableerror();
        item->mutable_erroridentifier()->set_value(error.errorIdentifier);
        item->mutable_commandidentifier()->set_value(error.commandIdentifier);
        item->mutable_commandexecutionuuid()->set_value(error.commandExecutionUuid);
        *item->mutable_errortime() = types::toProto(error.errorTime);
        item->mutable_errormessage()->set_value(error.errorMessage);
        // FDL RecoverableError is a Structure — DefaultOption and
        // AutomaticSelectionTimeout are always-present elements, and
        // DefaultOption "must be an identifier of one of the elements of the
        // Continuation Options list", so an empty placeholder is not valid
        // either. With no flagged option, point DefaultOption at the first
        // option and encode "no automatic selection" as timeout 0 ("A value
        // of 0 means, that no automatic selection shall be done by the
        // client", ErrorRecoveryService-v2_0.sila.xml:249-271); a
        // default-flagged option below overwrites both.
        if (!error.continuationOptions.empty()) {
            item->mutable_defaultoption()->set_value(
                error.continuationOptions.front().identifier);
        }
        item->mutable_automaticselectiontimeout()->mutable_timeout()->set_value(0);
        for (const auto& option : error.continuationOptions) {
            auto* wire = item->add_continuationoptions()->mutable_continuationoption();
            wire->mutable_identifier()->set_value(option.identifier);
            wire->mutable_description()->set_value(option.description);
            wire->mutable_requiredinputdata()->set_value(option.requiredInputData);
            // DefaultOption and AutomaticSelectionTimeout are single-valued on
            // the wire (FDL :250, :262) but the gate keeps them per option, so
            // the flagged option supplies both.
            if (option.isDefault) {
                item->mutable_defaultoption()->set_value(option.identifier);
                item->mutable_automaticselectiontimeout()->mutable_timeout()
                    ->set_value(option.automaticSelectionTimeout.count());
            }
        }
    }
}

ErrorRecoveryServiceImpl::ErrorRecoveryServiceImpl(recovery::RecoverableErrorGate& gate,
                                                    ObservablePropertyManager& propMgr,
                                                    const InterceptorChain* chain)
    : gate_{gate}, propMgr_{propMgr}, chain_{chain} {}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

grpc::Status ErrorRecoveryServiceImpl::ExecuteContinuationOption(
    grpc::ServerContext* context,
    const errorrecovery_proto::ExecuteContinuationOption_Parameters* request,
    errorrecovery_proto::ExecuteContinuationOption_Responses* response) {
    GrpcUnaryResponseSink<errorrecovery_proto::ExecuteContinuationOption_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { executeContinuationOption(req, ctx, out); },
        chain_, kExecuteContinuationOptionFqi, response);
    return sink.status();
}

void ErrorRecoveryServiceImpl::executeContinuationOption(
    const errorrecovery_proto::ExecuteContinuationOption_Parameters& request, CallContext&,
    ResponseSink<errorrecovery_proto::ExecuteContinuationOption_Responses>& sink) {
    const auto& uuid = request.commandexecutionuuid().value();
    const auto& optionId = request.continuationoption().value();
    // Validate the UUID shape before it ever reaches the gate: a malformed
    // UUID must surface as ValidationError, not collapse into the gate's
    // InvalidCommandExecutionUUID DefinedExecutionError below.
    if (auto lengthError = types::checkLength(uuid, 36)) {
        throw error::ValidationError{kExecuteContinuationOptionUuidParamFqi, *lengthError};
    }
    if (auto patternError = types::checkPattern(uuid, kUuidPattern)) {
        throw error::ValidationError{kExecuteContinuationOptionUuidParamFqi, *patternError};
    }
    try {
        gate_.selectOption(uuid, optionId, request.inputdata());
    } catch (const error::FrameworkError&) {
        // The gate raises a generic FrameworkError/v1 InvalidCommandExecutionUuid
        // for an unknown UUID; the FDL declares this as a v2 DefinedExecutionError
        // instead, so translate here rather than changing the gate's error type.
        throw error::DefinedExecutionError{
            kInvalidUuidErrorId,
            "No pending recoverable error for UUID: " + uuid};
    }
    sink.send(errorrecovery_proto::ExecuteContinuationOption_Responses{});
    sink.finish();
}

grpc::Status ErrorRecoveryServiceImpl::AbortErrorHandling(
    grpc::ServerContext* context,
    const errorrecovery_proto::AbortErrorHandling_Parameters* request,
    errorrecovery_proto::AbortErrorHandling_Responses* response) {
    GrpcUnaryResponseSink<errorrecovery_proto::AbortErrorHandling_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { abortErrorHandling(req, ctx, out); },
        chain_, kAbortErrorHandlingFqi, response);
    return sink.status();
}

void ErrorRecoveryServiceImpl::abortErrorHandling(
    const errorrecovery_proto::AbortErrorHandling_Parameters& request, CallContext&,
    ResponseSink<errorrecovery_proto::AbortErrorHandling_Responses>& sink) {
    const auto& uuid = request.commandexecutionuuid().value();
    // Same UUID-shape validation as ExecuteContinuationOption above.
    if (auto lengthError = types::checkLength(uuid, 36)) {
        throw error::ValidationError{kAbortErrorHandlingUuidParamFqi, *lengthError};
    }
    if (auto patternError = types::checkPattern(uuid, kUuidPattern)) {
        throw error::ValidationError{kAbortErrorHandlingUuidParamFqi, *patternError};
    }
    try {
        gate_.abort(uuid);
    } catch (const error::FrameworkError&) {
        throw error::DefinedExecutionError{
            kInvalidUuidErrorId,
            "No pending recoverable error for UUID: " + uuid};
    }
    sink.send(errorrecovery_proto::AbortErrorHandling_Responses{});
    sink.finish();
}

grpc::Status ErrorRecoveryServiceImpl::SetErrorHandlingTimeout(
    grpc::ServerContext* context,
    const errorrecovery_proto::SetErrorHandlingTimeout_Parameters* request,
    errorrecovery_proto::SetErrorHandlingTimeout_Responses* response) {
    GrpcUnaryResponseSink<errorrecovery_proto::SetErrorHandlingTimeout_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { setErrorHandlingTimeout(req, ctx, out); },
        chain_, kSetErrorHandlingTimeoutFqi, response);
    return sink.status();
}

void ErrorRecoveryServiceImpl::setErrorHandlingTimeout(
    const errorrecovery_proto::SetErrorHandlingTimeout_Parameters& request, CallContext&,
    ResponseSink<errorrecovery_proto::SetErrorHandlingTimeout_Responses>& sink) {
    const auto timeoutValue = request.errorhandlingtimeout().timeout().value();
    // ErrorRecoveryService-v2_0.sila.xml:339-340: Timeout DataTypeDefinition
    // constrains this to MinimalInclusive 0.
    if (auto rangeError = types::checkMinimalInclusive<int64_t>(timeoutValue, 0)) {
        throw error::ValidationError{kSetErrorHandlingTimeoutParamFqi, *rangeError};
    }
    const auto seconds = std::chrono::seconds{timeoutValue};
    gate_.setErrorHandlingTimeout(seconds);
    sink.send(errorrecovery_proto::SetErrorHandlingTimeout_Responses{});
    sink.finish();
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

grpc::Status ErrorRecoveryServiceImpl::Subscribe_RecoverableErrors(
    grpc::ServerContext* context,
    const errorrecovery_proto::Subscribe_RecoverableErrors_Parameters* request,
    grpc::ServerWriter<errorrecovery_proto::Subscribe_RecoverableErrors_Responses>* writer) {
    GrpcStreamResponseSink<errorrecovery_proto::Subscribe_RecoverableErrors_Responses> sink(writer);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { subscribeRecoverableErrors(req, ctx, out); },
        chain_, kSubscribe_RecoverableErrorsFqi);
    return sink.status();
}

void ErrorRecoveryServiceImpl::subscribeRecoverableErrors(
    const errorrecovery_proto::Subscribe_RecoverableErrors_Parameters&, CallContext& ctx,
    ResponseSink<errorrecovery_proto::Subscribe_RecoverableErrors_Responses>& sink) {
    auto sub = propMgr_.subscribe(recovery::kRecoverableErrorsPropertyId);
    while (!ctx.isCancelled()) {
        auto value = sub->waitForNext();
        if (!value.has_value()) {
            break;
        }
        const auto& errors =
            std::any_cast<const std::vector<recovery::RecoverableError>&>(*value);
        errorrecovery_proto::Subscribe_RecoverableErrors_Responses response;
        fillRecoverableErrorsResponse(errors, response);
        sink.send(response);
    }
    propMgr_.unsubscribe(recovery::kRecoverableErrorsPropertyId, sub);
    sub->cancel();
    sink.finish();
}

}  // namespace sila2
