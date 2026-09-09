// CloudHandlerRegistration.h — Helpers to register gRPC methods as cloud dispatch handlers (architecture.md §3.9)
#pragma once

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/error/ErrorTransmitInterceptor.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/ResponseSink.h>
#include <sila/server/transport/SilaHandler.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/cloud/StreamWriteSerializer.h>

#include "SiLACloudConnector.pb.h"
#include "SiLAFramework.pb.h"

#include <google/protobuf/message.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace sila2 {

namespace detail {

template <typename Resp>
class CloudUnaryResponseSink final : public ResponseSink<Resp> {
public:
    void send(const Resp& response) override { response_ = response; }
    void finish() override {}
    void fail(const error::SiLAError& error) override { status_ = error.toStatus(); }

    const Resp& response() const { return response_; }
    const grpc::Status& status() const { return status_; }

private:
    Resp response_;
    grpc::Status status_ = grpc::Status::OK;
};

inline void setErrorField(cloud::SiLAServerMessage& msg,
                          cloud::SiLAError silaErr,
                          CloudErrorField field) {
    switch (field) {
        case CloudErrorField::kCommandError:
            *msg.mutable_commanderror() = std::move(silaErr);
            break;
        case CloudErrorField::kPropertyError:
            *msg.mutable_propertyerror() = std::move(silaErr);
            break;
    }
}

inline void setCloudError(cloud::SiLAServerMessage& msg,
                          const grpc::Status& status,
                          CloudErrorField field) {
    cloud::SiLAError silaErr;
    bool parsed = !status.error_details().empty() &&
                  silaErr.ParseFromString(status.error_details());
    if (!parsed) {
        // A gRPC status whose details hold no parsable SiLAError did not come out of
        // guardHandler. The gRPC sibling reports exactly this as an
        // UndefinedExecutionError (ErrorTransmitInterceptor.h:28-32).
        error::UndefinedExecutionError err{status.error_message()};
        silaErr = *err.toProto();
    }
    setErrorField(msg, std::move(silaErr), field);
}

/// Parses the envelope's parameter bytes into `request`, and on failure writes
/// the rejection envelope itself and returns false.
/// 3.1t: the wrappers below used to discard ParseFromString's bool, so
/// malformed bytes ran the handler with a default-constructed request while
/// direct gRPC rejected the identical bytes at protobuf framing -- the two
/// transports diverged at a trust boundary.
/// FrameworkError, not ValidationError: ValidationError's first argument is the
/// FQI of the parameter that failed (SiLAErrorSubtypes.h), and a framing failure
/// has no single parameter to name. CommandExecutionNotAccepted is the only one
/// of SiLAFramework.proto's five values that means "the server will not run
/// this call".
/// No `if (!parameterBytes.empty())` pre-guard: proto3 ParseFromString("")
/// returns true and leaves the message default-constructed, so the old guard
/// never changed an outcome.
inline bool parseCloudParameters(google::protobuf::Message& request,
                                 const std::string& parameterBytes,
                                 const std::string& requestUUID,
                                 CloudErrorField field,
                                 StreamWriteSerializer& writer) {
    if (request.ParseFromString(parameterBytes)) {
        return true;
    }
    cloud::SiLAServerMessage msg;
    msg.set_requestuuid(requestUUID);
    error::FrameworkError fw{error::FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted,
                             "malformed parameter bytes"};
    setErrorField(msg, *fw.toProto(), field);
    writer.write(msg);
    return false;
}

}  // namespace detail

/// Builds the CloudDispatchFn for one unobservable command or property: it
/// parses the envelope's parameter bytes into `Req`, runs `method` on `svc`
/// through the same error::guardHandler every gRPC handler runs through, and
/// writes the response (or error) as a single envelope. Used by a generated
/// Feature adapter through regCmd/regProp below, on the
/// @ref gl_connection_method "Server-Initiated Connection" (cloud connectivity)
/// path; not called directly by a server author.
template <typename Req, typename Resp, typename Service, typename Handler>
CloudDispatchFn wrapGrpc(
    std::shared_ptr<Service> svc,
    Handler method,
    CloudErrorField field) {
    return [svc = std::move(svc), method, field](
               const std::string& parameterBytes,
               CallContext& ctx,
               StreamWriteSerializer& writer,
               const std::string& requestUUID) {
        Req request;
        if (!detail::parseCloudParameters(request, parameterBytes, requestUUID, field, writer)) {
            return;
        }
        detail::CloudUnaryResponseSink<Resp> sink;
        error::guardHandler<Resp>(
            [&] { (svc.get()->*method)(request, ctx, sink); }, sink);
        cloud::SiLAServerMessage msg;
        msg.set_requestuuid(requestUUID);
        if (sink.status().ok()) {
            if (field == CloudErrorField::kCommandError) {
                msg.mutable_unobservablecommandresponse()->set_response(
                    sink.response().SerializeAsString());
            } else {
                msg.mutable_unobservablepropertyvalue()->set_value(
                    sink.response().SerializeAsString());
            }
        } else {
            detail::setCloudError(msg, sink.status(), field);
        }
        writer.write(msg);
    };
}

/// Registers `svc`'s handler for an @ref gl_unobservable_command "Unobservable Command" so it is
/// also reachable over the @ref gl_connection_method "Server-Initiated Connection" (cloud
/// connectivity)
/// path, under `fqi + "/Command/" + name`. Emitted by codegen's generated
/// Feature adapters; a server author does not call this directly.
template <typename Svc, typename Req, typename Resp>
void regCmd(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
            std::shared_ptr<Svc> svc,
            void (Svc::*m)(const Req&, CallContext&, ResponseSink<Resp>&)) {
    r.registerCommandHandler(fqi + "/Command/" + name,
                             wrapGrpc<Req, Resp>(std::move(svc), m,
                                                 CloudErrorField::kCommandError));
}

/// Registers `svc`'s handler for an @ref gl_property "Property" so it is
/// also reachable over the cloud connectivity path, under `fqi + "/Property/" + name`.
template <typename Svc, typename Req, typename Resp>
void regProp(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
             std::shared_ptr<Svc> svc,
             void (Svc::*m)(const Req&, CallContext&, ResponseSink<Resp>&)) {
    r.registerPropertyHandler(fqi + "/Property/" + name,
                             wrapGrpc<Req, Resp>(std::move(svc), m,
                                                  CloudErrorField::kPropertyError));
}

/// Overload of regCmd for a codegen'd adapter's SilaHandler member instead
/// of a plain member function.
template <typename Svc, typename Req, typename Resp>
void regCmd(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
            std::shared_ptr<Svc> svc, SilaHandler<Req, Resp> Svc::*m) {
    r.registerCommandHandler(fqi + "/Command/" + name,
                             wrapGrpc<Req, Resp>(std::move(svc), m,
                                                 CloudErrorField::kCommandError));
}

/// Overload of regProp for a codegen'd adapter's SilaHandler member.
template <typename Svc, typename Req, typename Resp>
void regProp(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
             std::shared_ptr<Svc> svc, SilaHandler<Req, Resp> Svc::*m) {
    r.registerPropertyHandler(fqi + "/Property/" + name,
                              wrapGrpc<Req, Resp>(std::move(svc), m,
                                                  CloudErrorField::kPropertyError));
}

// kPropertyValue reuses this sink for codegen'd observable properties (1.2k):
// it differs from the follow-up streams only in which body field send() fills
// and which oneof fail() writes, and shares the write-failure -> cancel policy.
enum class ObsFollowupField { kExecutionInfo, kIntermediateResponse, kResult, kPropertyValue };

namespace detail {

/// One envelope per send(), for the observable-command follow-up streams.
/// CloudUnaryResponseSink cannot serve these: it overwrites response_, so a
/// handler that streams N intermediate values (ShakeControllerImpl.cc:139-178
/// sends one per timeLeft change) emitted a single envelope carrying only the
/// last (§1.2h). No queue and no drops — back-pressure from a slow stream
/// lands on the per-request pump thread, which is architecture-v2.md:293's
/// rule for IntermediateResponse satisfied by construction.
template <typename Resp>
class CloudFollowupResponseSink final : public ResponseSink<Resp> {
public:
    CloudFollowupResponseSink(StreamWriteSerializer& writer, CallContext& ctx,
                              std::string requestUUID,
                              cloud::CommandExecutionUUID execUuid,
                              ObsFollowupField field)
        : writer_{writer}, ctx_{ctx}, requestUUID_{std::move(requestUUID)},
          execUuid_{std::move(execUuid)}, field_{field} {}

    void send(const Resp& value) override {
        cloud::SiLAServerMessage msg;
        msg.set_requestuuid(requestUUID_);
        if (field_ == ObsFollowupField::kPropertyValue) {
            // No execution UUID on this envelope: an observable property
            // subscription has no command execution behind it, so execUuid_
            // stays unused on this path.
            msg.mutable_observablepropertyvalue()->set_value(value.SerializeAsString());
        } else if (field_ == ObsFollowupField::kExecutionInfo) {
            auto* body = msg.mutable_observablecommandexecutioninfo();
            *body->mutable_commandexecutionuuid() = execUuid_;
            // The one follow-up body carrying a nested message rather than
            // `bytes response` (SiLACloudConnector.proto:94-97). Serialize-then-
            // parse, not a direct assignment: this branch is chosen at runtime,
            // so a direct assignment would have to compile for every Resp this
            // template is instantiated with, not just cloud::ExecutionInfo.
            body->mutable_executioninfo()->ParseFromString(value.SerializeAsString());
        } else if (field_ == ObsFollowupField::kIntermediateResponse) {
            auto* body = msg.mutable_observablecommandintermediateresponse();
            *body->mutable_commandexecutionuuid() = execUuid_;
            body->set_response(value.SerializeAsString());
        } else {
            auto* body = msg.mutable_observablecommandresponse();
            *body->mutable_commandexecutionuuid() = execUuid_;
            body->set_response(value.SerializeAsString());
        }
        if (!writer_.write(msg)) {
            // Peer gone: stop the handler now instead of letting it stream into
            // a dead stream for the rest of the command, holding a pump slot
            // against the router's concurrency cap for zero output.
            ctx_.requestCancellation();
        }
    }

    // The envelope stream's end is the stream end — there is no per-call
    // half-close on the multiplexed cloud stream to signal here.
    void finish() override {}

    void fail(const error::SiLAError& error) override {
        // Straight to the proto, not through grpc::Status + setCloudError:
        // that round-trip only exists to recover a SiLAError that a gRPC
        // handler had already serialized into error_details. guardHandler
        // wraps every non-SiLA exception in UndefinedExecutionError, so this
        // covers every failure the handler can produce.
        cloud::SiLAServerMessage msg;
        msg.set_requestuuid(requestUUID_);
        if (field_ == ObsFollowupField::kPropertyValue) {
            *msg.mutable_propertyerror() = *error.toProto();
        } else {
            *msg.mutable_commanderror() = *error.toProto();
        }
        writer_.write(msg);
    }

private:
    StreamWriteSerializer& writer_;
    CallContext& ctx_;
    std::string requestUUID_;
    cloud::CommandExecutionUUID execUuid_;
    ObsFollowupField field_;
};

}  // namespace detail

/// Builds the CloudDispatchFn that initiates an @ref gl_observable_command "Observable Command"
/// execution over the cloud connectivity path and
/// confirms it to the client with its @ref gl_command_execution_uuid "Command Execution UUID".
// Observable command initiation: wraps as observableCommandConfirmation and
// registers the execution UUID→FQI mapping for follow-up dispatch.
template <typename Req, typename Resp, typename Service, typename Handler>
CloudDispatchFn wrapObsInit(std::shared_ptr<Service> svc, Handler method,
                            CloudEnvelopeRouter* router, const std::string& cmdFqi) {
    return [svc = std::move(svc), method, router, cmdFqi](
               const std::string& parameterBytes,
               CallContext& ctx,
               StreamWriteSerializer& writer,
               const std::string& requestUUID) {
        Req request;
        if (!detail::parseCloudParameters(request, parameterBytes, requestUUID,
                                          CloudErrorField::kCommandError, writer)) {
            return;
        }
        detail::CloudUnaryResponseSink<Resp> sink;
        error::guardHandler<Resp>(
            [&] { (svc.get()->*method)(request, ctx, sink); }, sink);

        cloud::SiLAServerMessage msg;
        msg.set_requestuuid(requestUUID);
        if (sink.status().ok()) {
            *msg.mutable_observablecommandconfirmation()->mutable_commandconfirmation() =
                sink.response();
            auto uuid = sink.response().commandexecutionuuid().value();
            // Snapshots the initiating call's access token against the
            // execution: the follow-up envelopes (_Info/_Intermediate/_Result)
            // carry no metadata field (SiLACloudConnector.proto:79-87), so this
            // is the only point where the router can capture a credential to
            // replay on them later (§S15, owner option 1).
            router->registerExecutionFQI(uuid, cmdFqi, ctx.metadata("access-token"));
        } else {
            detail::setCloudError(msg, sink.status(), CloudErrorField::kCommandError);
        }
        writer.write(msg);
    };
}

/// Builds the CloudDispatchFn for one @ref gl_observable_command "Observable Command" follow-up
/// stream (@ref gl_command_execution_info "Command Execution Info",
/// @ref gl_intermediate_command_response "Intermediate Command Response" , or the final result),
/// keyed by an
/// already-initiated execution's @ref gl_command_execution_uuid "Command Execution UUID".
// Observable command follow-up (_Intermediate / _Result): wraps as
// observableCommandIntermediateResponse or observableCommandResponse.
template <typename Req, typename Resp, typename Service, typename Handler>
CloudDispatchFn wrapObsFollowup(std::shared_ptr<Service> svc, Handler method,
                                ObsFollowupField field) {
    return [svc = std::move(svc), method, field](
               const std::string& parameterBytes,
               CallContext& ctx,
               StreamWriteSerializer& writer,
               const std::string& requestUUID) {
        Req request;
        if (!detail::parseCloudParameters(request, parameterBytes, requestUUID,
                                          CloudErrorField::kCommandError, writer)) {
            return;
        }
        // request is the CommandExecutionUUID the router serialized into
        // parameterBytes; the previous code assigned it into the envelope the
        // same way, so Req is statically constrained to that type either way.
        detail::CloudFollowupResponseSink<Resp> sink{writer, ctx, requestUUID, request, field};
        error::guardHandler<Resp>(
            [&] { (svc.get()->*method)(request, ctx, sink); }, sink);
    };
}

/// Registers `svc`'s init/intermediate/result handlers for one
/// @ref gl_observable_command "Observable Command" `name` under `fqi`, so its
/// whole lifetime is reachable over the cloud connectivity path.
// Plain-member-function overload: registers no "_Info" handler. Its only
// callers are test_cloud_handler_registration.cc's three regObsCmd sites,
// which bind &StreamingFollowupService::Init/Intermediate/Result -- plain
// methods, not SilaHandler members -- so giving this overload an infoMethod
// slot too would buy no production behaviour. An execution initiated through
// it falls back to CloudEnvelopeRouter's synthesized _Info pump, a supported
// configuration (CloudEnvelopeRouter.cc's kObservableCommandExecutionInfoSubscription case).
template <typename Svc, typename InitReq, typename InitResp,
          typename IntReq, typename IntResp,
          typename ResReq, typename ResResp>
void regObsCmd(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
               std::shared_ptr<Svc> svc,
               void (Svc::*initMethod)(const InitReq&, CallContext&, ResponseSink<InitResp>&),
               void (Svc::*intMethod)(const IntReq&, CallContext&, ResponseSink<IntResp>&),
               void (Svc::*resMethod)(const ResReq&, CallContext&, ResponseSink<ResResp>&)) {
    std::string cmdFqi = fqi + "/Command/" + name;
    r.registerCommandHandler(cmdFqi,
        wrapObsInit<InitReq, InitResp>(svc, initMethod, &r, cmdFqi));
    r.registerCommandHandler(cmdFqi + "_Intermediate",
        wrapObsFollowup<IntReq, IntResp>(svc, intMethod, ObsFollowupField::kIntermediateResponse));
    r.registerCommandHandler(cmdFqi + "_Result",
        wrapObsFollowup<ResReq, ResResp>(svc, resMethod, ObsFollowupField::kResult));
}

/// Overload of regObsCmd for a codegen'd adapter's SilaHandler members,
/// also registering `name`'s @ref gl_command_execution_info "Command Execution Info" follow-up
/// stream.
// The shape codegen emits (service_adapter.h.j2): init, info, [intermediate],
// result. infoMethod sits right after initMethod so the with- and
// without-intermediate overloads below share the same prefix, differing only
// in whether intMethod is present -- distinct arity (4 vs 3 handler
// arguments), so no ambiguous redeclaration between them. The FDL only admits
// with/without IntermediateResponse, so this pair is exhaustive.
template <typename Svc, typename InitReq, typename InitResp,
          typename InfoReq, typename InfoResp,
          typename IntReq, typename IntResp,
          typename ResReq, typename ResResp>
void regObsCmd(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
               std::shared_ptr<Svc> svc,
               SilaHandler<InitReq, InitResp> Svc::*initMethod,
               SilaHandler<InfoReq, InfoResp> Svc::*infoMethod,
               SilaHandler<IntReq, IntResp> Svc::*intMethod,
               SilaHandler<ResReq, ResResp> Svc::*resMethod) {
    std::string cmdFqi = fqi + "/Command/" + name;
    r.registerCommandHandler(cmdFqi,
        wrapObsInit<InitReq, InitResp>(svc, initMethod, &r, cmdFqi));
    r.registerCommandHandler(cmdFqi + "_Info",
        wrapObsFollowup<InfoReq, InfoResp>(svc, infoMethod, ObsFollowupField::kExecutionInfo));
    r.registerCommandHandler(cmdFqi + "_Intermediate",
        wrapObsFollowup<IntReq, IntResp>(svc, intMethod, ObsFollowupField::kIntermediateResponse));
    r.registerCommandHandler(cmdFqi + "_Result",
        wrapObsFollowup<ResReq, ResResp>(svc, resMethod, ObsFollowupField::kResult));
}

/// Overload of regObsCmd for a command whose FDL definition has no
/// @ref gl_intermediate_command_response "Intermediate Command Response" (1.2i):
/// registers no `_Intermediate` handler.
// Observable command without IntermediateResponse (1.2i): the same wiring minus
// the _Intermediate stream, which the FDL does not define for this shape. Not
// registering that suffix is deliberate -- an intermediate subscription then
// gets sendNoHandlerError, the wire equivalent of the RPC not existing on gRPC.
template <typename Svc, typename InitReq, typename InitResp,
          typename InfoReq, typename InfoResp,
          typename ResReq, typename ResResp>
void regObsCmd(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
               std::shared_ptr<Svc> svc,
               SilaHandler<InitReq, InitResp> Svc::*initMethod,
               SilaHandler<InfoReq, InfoResp> Svc::*infoMethod,
               SilaHandler<ResReq, ResResp> Svc::*resMethod) {
    std::string cmdFqi = fqi + "/Command/" + name;
    r.registerCommandHandler(cmdFqi,
        wrapObsInit<InitReq, InitResp>(svc, initMethod, &r, cmdFqi));
    r.registerCommandHandler(cmdFqi + "_Info",
        wrapObsFollowup<InfoReq, InfoResp>(svc, infoMethod, ObsFollowupField::kExecutionInfo));
    r.registerCommandHandler(cmdFqi + "_Result",
        wrapObsFollowup<ResReq, ResResp>(svc, resMethod, ObsFollowupField::kResult));
}

/// Builds the CloudDispatchFn for a codegen'd @ref gl_observable_property "Observable Property"
/// @ref gl_property_subscription "Subscription" .
// Observable property subscription for codegen'd adapters (1.2k): runs the SAME
// Subscribe_X SilaHandler the direct-gRPC path runs, on a router pump thread,
// emitting one observablePropertyValue envelope per sink.send(). The generated
// adapter owns neither an ObservablePropertyManager nor a CloudValueSerializer
// -- both live inside the application's handler -- so registerObservableProperty's
// manager-based path is unreachable from codegen and stays for hand-wired
// callers (SiLAServerBase.cc's RecoverableErrors).
template <typename Req, typename Resp, typename Service, typename Handler>
CloudDispatchFn wrapObsProp(std::shared_ptr<Service> svc, Handler method) {
    return [svc = std::move(svc), method](
               const std::string& parameterBytes,
               CallContext& ctx,
               StreamWriteSerializer& writer,
               const std::string& requestUUID) {
        Req request;
        if (!detail::parseCloudParameters(request, parameterBytes, requestUUID,
                                          CloudErrorField::kPropertyError, writer)) {
            return;
        }
        detail::CloudFollowupResponseSink<Resp> sink{
            writer, ctx, requestUUID, cloud::CommandExecutionUUID{},
            ObsFollowupField::kPropertyValue};
        error::guardHandler<Resp>(
            [&] { (svc.get()->*method)(request, ctx, sink); }, sink);
    };
}

/// Registers `svc`'s Subscribe_X handler for an @ref gl_observable_property "Observable Property"
/// `name` under `fqi`, so it is reachable over the
/// cloud connectivity path.
/// @see CloudEnvelopeRouter::registerObservableProperty for the
/// manager-based alternative used by hand-wired (non-codegen'd) Features.
template <typename Svc, typename Req, typename Resp>
void regObsProp(CloudEnvelopeRouter& r, const std::string& fqi, const char* name,
                std::shared_ptr<Svc> svc, SilaHandler<Req, Resp> Svc::*m) {
    r.registerObservablePropertyHandler(fqi + "/Property/" + name,
                                        wrapObsProp<Req, Resp>(std::move(svc), m));
}

}  // namespace sila2
