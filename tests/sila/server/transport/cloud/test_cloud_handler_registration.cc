// Integration tests for CloudHandlerRegistration.h (architecture.md §3.9):
// wrapGrpc's dispatch of a transport-neutral service method into a SiLAServerMessage,
// regCmd/regProp's FQI wiring into CloudEnvelopeRouter, and
// detail::setCloudError's status-to-SiLAError translation. Each test drives
// the full path: register via regCmd/regProp -> route() a SiLAClientMessage
// through a real gRPC stream (via CloudRouterFixture) -> inspect the written
// SiLAServerMessage.
#include "CloudRouterTestHarness.h"

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/cloud/CloudHandlerRegistration.h>

#include "SiLACloudConnector.pb.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace cloud = sila2::org::silastandard;

// Adapts the shared harness fixture to this file's test suite name.
class CloudHandlerRegistration : public cloud_test::CloudRouterFixture {};

// Minimal transport-neutral service used to exercise response and error
// dispatch through the cloud registration helpers.
struct EchoService {
    bool invoked = false;

    void Echo(const cloud::CommandExecutionUUID& req, sila2::CallContext&,
              sila2::ResponseSink<cloud::CommandExecutionUUID>& sink) {
        invoked = true;
        cloud::CommandExecutionUUID resp;
        resp.set_value(req.value());
        sink.send(resp);
        sink.finish();
    }

    void FailWithDetails(const cloud::CommandExecutionUUID&, sila2::CallContext&,
                         sila2::ResponseSink<cloud::CommandExecutionUUID>&) {
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid,
            "test-detailed-error"};
    }

    void FailNoParsable(const cloud::CommandExecutionUUID&, sila2::CallContext&,
                        sila2::ResponseSink<cloud::CommandExecutionUUID>&) {
        throw std::runtime_error{"plain-error-message"};
    }

    void FailEmptyDetailsExplicit(const cloud::CommandExecutionUUID&, sila2::CallContext&,
                                  sila2::ResponseSink<cloud::CommandExecutionUUID>&) {
        throw std::runtime_error{"bad-argument"};
    }
};

// Exercises the follow-up sinks through the real regObsCmd wiring: the
// _Intermediate method streams three distinct values, the _Result method sends
// exactly one, matching the codegen/reference contract
// (ShakeControllerImpl.cc:139-201).
struct StreamingFollowupService {
    bool initInvoked = false;

    void Init(const cloud::CommandExecutionUUID&, sila2::CallContext&,
              sila2::ResponseSink<cloud::CommandConfirmation>& sink) {
        initInvoked = true;
        sink.send(cloud::CommandConfirmation{});
        sink.finish();
    }

    void Intermediate(const cloud::CommandExecutionUUID&, sila2::CallContext&,
                      sila2::ResponseSink<cloud::CommandExecutionUUID>& sink) {
        for (const char* payload : {"one", "two", "three"}) {
            cloud::CommandExecutionUUID value;
            value.set_value(payload);
            sink.send(value);
        }
        sink.finish();
    }

    void Result(const cloud::CommandExecutionUUID&, sila2::CallContext&,
                sila2::ResponseSink<cloud::CommandExecutionUUID>& sink) {
        cloud::CommandExecutionUUID value;
        value.set_value("final");
        sink.send(value);
        sink.finish();
    }
};

const std::string kFqi = "org.test/Feature";

// --- Positive (True) paths --------------------------------------------------

// P1: regCmd + command handler OK -> unobservablecommandresponse carries the
// serialized Resp.
TEST_F(CloudHandlerRegistration, RegCmdWithOkStatusWritesUnobservableCommandResponse) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regCmd(router, kFqi, "Echo", svc, &EchoService::Echo);

    cloud::CommandExecutionUUID param;
    param.set_value("hello");
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-p1");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(kFqi + "/Command/Echo");
    exec->mutable_commandparameter()->set_parameters(param.SerializeAsString());

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-p1");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    cloud::CommandExecutionUUID decoded;
    ASSERT_TRUE(decoded.ParseFromString(resp.unobservablecommandresponse().response()));
    EXPECT_EQ(decoded.value(), "hello");
}

// P2: regProp + property handler OK -> unobservablepropertyvalue carries the
// serialized Resp.
TEST_F(CloudHandlerRegistration, RegPropWithOkStatusWritesUnobservablePropertyValue) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regProp(router, kFqi, "ReadVal", svc, &EchoService::Echo);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-p2");
    msg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(
        kFqi + "/Property/ReadVal");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    EXPECT_EQ(resp.requestuuid(), "req-p2");
    ASSERT_TRUE(resp.has_unobservablepropertyvalue());
    cloud::CommandExecutionUUID decoded;
    ASSERT_TRUE(decoded.ParseFromString(resp.unobservablepropertyvalue().value()));
    // Empty parameterBytes -> Req left default-constructed -> empty value echoed back.
    EXPECT_EQ(decoded.value(), "");
}

// P3: SiLAError from a transport-neutral handler is preserved on the cloud wire.
TEST_F(CloudHandlerRegistration, WrapGrpcWithParsableErrorDetailsPreservesOriginalError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regCmd(router, kFqi, "FailWithDetails", svc, &EchoService::FailWithDetails);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-p3");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        kFqi + "/Command/FailWithDetails");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    const auto& fwErr = resp.commanderror().frameworkerror();
    EXPECT_EQ(fwErr.errortype(), cloud::FrameworkError::INVALID_COMMAND_EXECUTION_UUID);
    EXPECT_EQ(fwErr.message(), "test-detailed-error");
}

// --- Negative (False) paths -------------------------------------------------
// Standard exceptions are translated by guardHandler to UndefinedExecutionError.

// N1: command handler throws a standard exception.
TEST_F(CloudHandlerRegistration, RegCmdWithStdExceptionWritesUndefinedExecutionError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regCmd(router, kFqi, "FailNoParsable", svc, &EchoService::FailNoParsable);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n1");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        kFqi + "/Command/FailNoParsable");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_undefinedexecutionerror());
    EXPECT_EQ(resp.commanderror().undefinedexecutionerror().message(), "plain-error-message");
}

// N2: property handler throws a standard exception.
TEST_F(CloudHandlerRegistration, RegPropWithStdExceptionWritesUndefinedExecutionError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regProp(router, kFqi, "FailProp", svc, &EchoService::FailNoParsable);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n2");
    msg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(
        kFqi + "/Property/FailProp");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_undefinedexecutionerror());
    EXPECT_EQ(resp.propertyerror().undefinedexecutionerror().message(), "plain-error-message");
}

// N3: a second standard-exception path keeps its message.
TEST_F(CloudHandlerRegistration, RegCmdWithStdExceptionKeepsMessage) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regCmd(router, kFqi, "FailEmptyDetails", svc, &EchoService::FailEmptyDetailsExplicit);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n3");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        kFqi + "/Command/FailEmptyDetails");

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_undefinedexecutionerror());
    EXPECT_EQ(resp.commanderror().undefinedexecutionerror().message(), "bad-argument");
}

// --- Streaming follow-up sink (audit 1.2h, CloudHandlerRegistration.h:27) --

// P4: an _Intermediate handler that calls sink.send() three times must
// produce three envelopes, in order. Before this batch,
// CloudUnaryResponseSink::send overwrote response_, so this emitted ONE
// envelope carrying only the last value ("three").
TEST_F(CloudHandlerRegistration, IntermediateSubscriptionDeliversEveryValue) {
    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager manager;
    auto exec = manager.addCommand(std::chrono::seconds{300});
    exec->start();
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&manager}};
    sila2::regObsCmd(router, kFqi, "Streamed", std::make_shared<StreamingFollowupService>(),
                     &StreamingFollowupService::Init, &StreamingFollowupService::Intermediate,
                     &StreamingFollowupService::Result);
    router.registerExecutionFQI(exec->uuid(), kFqi + "/Command/Streamed");

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-stream");
    msg.mutable_observablecommandintermediateresponsesubscription()
       ->mutable_commandexecutionuuid()->set_value(exec->uuid());

    router.route(msg, *writer_, writer_, calls_);

    for (const std::string& expected : {"one", "two", "three"}) {
        auto resp = popResponse();
        ASSERT_TRUE(resp.has_observablecommandintermediateresponse());
        const auto& body = resp.observablecommandintermediateresponse();
        EXPECT_EQ(body.commandexecutionuuid().value(), exec->uuid());
        cloud::CommandExecutionUUID decoded;
        ASSERT_TRUE(decoded.ParseFromString(body.response()));
        EXPECT_EQ(decoded.value(), expected);
    }
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{200}));
}

// P5: guards the sink swap against silently duplicating the unary case —
// _Result must still emit exactly one envelope, not one per send().
TEST_F(CloudHandlerRegistration, ResultDispatchStillEmitsExactlyOneEnvelope) {
    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager manager;
    auto exec = manager.addCommand(std::chrono::seconds{300});
    exec->start();
    exec->finish();
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&manager}};
    sila2::regObsCmd(router, kFqi, "Streamed", std::make_shared<StreamingFollowupService>(),
                     &StreamingFollowupService::Init, &StreamingFollowupService::Intermediate,
                     &StreamingFollowupService::Result);
    router.registerExecutionFQI(exec->uuid(), kFqi + "/Command/Streamed");

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-result");
    msg.mutable_observablecommandgetresponse()
       ->mutable_commandexecutionuuid()->set_value(exec->uuid());

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_observablecommandresponse());
    const auto& body = resp.observablecommandresponse();
    EXPECT_EQ(body.commandexecutionuuid().value(), exec->uuid());
    cloud::CommandExecutionUUID decoded;
    ASSERT_TRUE(decoded.ParseFromString(body.response()));
    EXPECT_EQ(decoded.value(), "final");
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{200}));
}

// --- Malformed parameter bytes (audit 3.1t, batch B2) -----------------------
// Four continuation bytes are an incomplete varint, so ParseFromString fails
// on any message type used below.
const std::string kGarbage{"\xff\xff\xff\xff", 4};

// N4: a command handler must never see a default-constructed request when the
// wire bytes were unparsable — the guard rejects before the handler runs.
TEST_F(CloudHandlerRegistration, RegCmdWithMalformedParameterBytesRejectsBeforeHandler) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regCmd(router, kFqi, "Echo", svc, &EchoService::Echo);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n4");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(kFqi + "/Command/Echo");
    exec->mutable_commandparameter()->set_parameters(kGarbage);

    router.route(msg, *writer_, writer_, calls_);

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().message(), "malformed parameter bytes");
    // Pins the wire value as chosen rather than inherited from proto's
    // default 0 (the deleted `Invalid` sentinel's accidental landing spot).
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::COMMAND_EXECUTION_NOT_ACCEPTED);
    // The point of the test: pre-fix, the handler ran with a
    // default-constructed request instead of never running at all.
    EXPECT_FALSE(svc->invoked);
}

// N5: the property wrapper's own guard, exercised directly rather than
// through a client envelope — no client envelope kind carries parameter bytes
// on the property path (route() passes "" for both kUnobservablePropertyRead
// and kObservablePropertySubscription), so this is defence for a wrapper a
// downstream repo could register elsewhere, not a reachable wire case.
TEST_F(CloudHandlerRegistration, RegPropWithMalformedParameterBytesWritesPropertyFrameworkError) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    auto svc = std::make_shared<EchoService>();
    sila2::regProp(router, kFqi, "ReadVal", svc, &EchoService::Echo);

    auto fn = sila2::wrapGrpc<cloud::CommandExecutionUUID, cloud::CommandExecutionUUID>(
        svc, &EchoService::Echo, sila2::CloudErrorField::kPropertyError);
    sila2::CallContext ctx;
    fn(kGarbage, ctx, *writer_, "req-n5");

    auto resp = popResponse();
    ASSERT_TRUE(resp.has_propertyerror());
    ASSERT_TRUE(resp.propertyerror().has_frameworkerror());
    EXPECT_EQ(resp.propertyerror().frameworkerror().message(), "malformed parameter bytes");
    EXPECT_EQ(resp.propertyerror().frameworkerror().errortype(),
              cloud::FrameworkError::COMMAND_EXECUTION_NOT_ACCEPTED);
    EXPECT_FALSE(svc->invoked);
}

// N6: malformed bytes on an observable command initiation must not register
// the execution FQI — proven by an immediate _Result lookup on that
// (never-registered) execution UUID failing with "no command FQI registered",
// not "no handler registered". A "no handler registered" result there would
// mean registerExecutionFQI ran anyway with the default-constructed request.
TEST_F(CloudHandlerRegistration, ObsInitWithMalformedParameterBytesDoesNotRegisterExecutionFqi) {
    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager manager;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&manager}};
    auto svc = std::make_shared<StreamingFollowupService>();
    sila2::regObsCmd(router, kFqi, "Streamed", svc, &StreamingFollowupService::Init,
                     &StreamingFollowupService::Intermediate, &StreamingFollowupService::Result);

    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-n6");
    auto* initiation = init.mutable_observablecommandinitiation();
    initiation->set_fullyqualifiedcommandid(kFqi + "/Command/Streamed");
    initiation->mutable_commandparameter()->set_parameters(kGarbage);

    router.route(init, *writer_, writer_, calls_);

    auto initResp = popResponse();
    ASSERT_TRUE(initResp.has_commanderror());
    ASSERT_TRUE(initResp.commanderror().has_frameworkerror());
    EXPECT_EQ(initResp.commanderror().frameworkerror().message(), "malformed parameter bytes");
    EXPECT_FALSE(svc->initInvoked);

    // Proves wrapObsInit's registerExecutionFQI never ran: a real execution,
    // never routed through the guarded wrapper, still resolves no FQI.
    auto exec = manager.addCommand(std::chrono::seconds{300});
    exec->start();

    cloud::SiLAClientMessage getResponse;
    getResponse.set_requestuuid("req-n6b");
    getResponse.mutable_observablecommandgetresponse()
        ->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(getResponse, *writer_, writer_, calls_);

    auto resultResp = popResponse();
    ASSERT_TRUE(resultResp.has_commanderror());
    ASSERT_TRUE(resultResp.commanderror().has_frameworkerror());
    EXPECT_EQ(resultResp.commanderror().frameworkerror().message(),
              "no command FQI registered for execution: " + exec->uuid());
}

// --- setCloudError's status-to-SiLAError fallback (S2) ----------------------
// Called directly, not through a handler: CloudUnaryResponseSink::fail()
// always builds status via SiLAError::toStatus(), which always populates
// error_details, so the !parsed branch is unreachable end to end through
// route(). Direct is the only way to exercise it.

// N7 (REJECTION): a gRPC status with no parsable SiLAError in error_details
// falls back to UndefinedExecutionError, matching the gRPC sibling's identical
// escape (ErrorTransmitInterceptor.h:28-32) rather than the deleted `Invalid`
// FrameworkError sentinel.
TEST_F(CloudHandlerRegistration, UnparsableStatusDetailsBecomeUndefinedExecutionError) {
    cloud::SiLAServerMessage msg;
    const grpc::Status status{grpc::StatusCode::INTERNAL, "boom"};  // empty error_details

    sila2::detail::setCloudError(msg, status, sila2::CloudErrorField::kCommandError);

    ASSERT_TRUE(msg.commanderror().has_undefinedexecutionerror());
    EXPECT_EQ(msg.commanderror().undefinedexecutionerror().message(), "boom");
    EXPECT_FALSE(msg.commanderror().has_frameworkerror());
}

// P6 (POSITIVE): a gRPC status whose error_details IS a parsable SiLAError is
// passed through unchanged -- the fallback above must not fire when there is a
// real error to recover.
TEST_F(CloudHandlerRegistration, ParsableStatusDetailsArePassedThroughUnchanged) {
    cloud::SiLAError original;
    original.mutable_definedexecutionerror()->set_erroridentifier(
        "org.test/Feature/v1/DefinedExecutionError/Sample");
    original.mutable_definedexecutionerror()->set_message("original message");
    const grpc::Status status{grpc::StatusCode::ABORTED, "unused",
                              original.SerializeAsString()};

    cloud::SiLAServerMessage msg;
    sila2::detail::setCloudError(msg, status, sila2::CloudErrorField::kCommandError);

    ASSERT_TRUE(msg.commanderror().has_definedexecutionerror());
    EXPECT_EQ(msg.commanderror().definedexecutionerror().erroridentifier(),
              "org.test/Feature/v1/DefinedExecutionError/Sample");
    EXPECT_FALSE(msg.commanderror().has_undefinedexecutionerror());
}

}  // namespace
