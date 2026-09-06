// test_codegen_cloud_e2e.cc — the only test that drives a CODEGEN'D
// ServiceAdapter's registerCloudHandlers through CloudEnvelopeRouter::route().
// Before this file, every cloud observable test hand-registered lambdas and
// called registerExecutionFQI directly, so the emitted regObsCmd/regObsProp
// wiring and the "_Intermediate"/"_Result" suffix contract were exercised by
// nothing (audit 1.2j).
#include "CloudRouterTestHarness.h"

#include "ShakeControllerImpl.h"
#include "ObservableBasicServiceAdapter.h"
#include "ObservableCommandNoIntermediateResponseServiceAdapter.h"
// BinaryTransferTest, added for S71 (below): its EchoBinaryValue Command has a
// Binary parameter, needed to drive the cloud path's Binary Transfer UUID
// resolve now living in validated_<handler> (service_adapter.h.j2).
#include "BinaryTransferTestServiceAdapter.h"
// kFqi (the Feature FQI) moved here: the per-RPC k<Name>Fqi adapter constants
// now carry a "/Command|Property/<Name>" suffix (the granular gate target), so
// these tests build cloud call ids from the Feature FQI + the suffix they
// already append, not from a command constant.
#include "ShakeControllerMeta.h"
#include "ObservableBasicMeta.h"
#include "ObservableCommandNoIntermediateResponseMeta.h"
#include "BinaryTransferTestMeta.h"

#include <sila/server/FeatureRegistry.h>
#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/ResponseSink.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/FqiMatch.h>

#include "AuthorizationService.pb.h"

#include <gtest/gtest.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace cloud = sila2::org::silastandard;
namespace shake_proto = sila2::org::silastandard::examples::shakecontroller::v1;
namespace basic_proto = sila2::org::silastandard::tests::observablebasic::v1;
namespace nointer_proto =
    sila2::org::silastandard::tests::observablecommandnointermediateresponse::v1;
namespace binary_proto = sila2::org::silastandard::test::binarytransfertest::v1;
namespace gen_shake = sila2::generated::shakecontroller;
namespace gen_basic = sila2::generated::observablebasic;
namespace gen_nointer = sila2::generated::observablecommandnointermediateresponse;
namespace gen_binary = sila2::generated::binarytransfertest;

using cloud_test::CloudRouterFixture;
using namespace std::chrono_literals;

namespace authzproto = sila2::org::silastandard::core::authorizationservice::v1;

// Wraps a bare token the way the cloud wire format requires: a serialized
// Metadata_AccessToken message under the AccessToken metadata FQI -- same
// helper as test_fqi_coverage_dual_transport_e2e.cc's, which documents the
// key against CloudEnvelopeRouter.cc's makeCloudCallContext().
cloud::Metadata makeAccessTokenMetadata(const std::string& token) {
    authzproto::Metadata_AccessToken wrapper;
    wrapper.mutable_accesstoken()->set_value(token);
    cloud::Metadata md;
    md.set_fullyqualifiedmetadataid(
        "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken");
    md.set_value(wrapper.SerializeAsString());
    return md;
}

class CodegenCloudE2E : public CloudRouterFixture {};

// _Result no longer blocks until the worker finishes (S47/S68: it reads the
// current state and throws CommandExecutionNotFinished while still Running),
// so a test that wants the final response must wait for the execution to
// reach a terminal state. Polls the manager instead of sleeping a fixed
// interval; the deadline only bounds a hung worker.
void waitUntilFinished(sila2::ObservableCommandManager& manager, const std::string& execUuid) {
    auto exec = manager.getCommand(execUuid);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (exec->state() == sila2::ObservableCommandExecution::State::Waiting ||
           exec->state() == sila2::ObservableCommandExecution::State::Running) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "execution did not finish";
        std::this_thread::sleep_for(10ms);
    }
}

// ---------------------------------------------------------------------------
// 1.2j: the through-line proving registerCloudHandlers wires a production
// generated adapter (ShakeController) end to end over the cloud transport.
// ---------------------------------------------------------------------------

TEST_F(CodegenCloudE2E, ShakeControllerAdapterDrivesInitiationInfoIntermediateResult) {
    shake_example::ShakeControllerImpl impl{nullptr};
    auto adapter = std::static_pointer_cast<gen_shake::ShakeControllerServiceAdapter>(
        impl.service());
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&impl.commandManager()}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_shake::kFqi};

    shake_proto::ShakeForTime_Parameters params;
    params.mutable_runtime()->set_value(1);
    params.mutable_targetspeed()->set_value(5000.0);
    params.mutable_targetpower()->set_value(50.0);
    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-e2e-init");
    auto* initiation = init.mutable_observablecommandinitiation();
    initiation->set_fullyqualifiedcommandid(fqi + "/Command/ShakeForTime");
    initiation->mutable_commandparameter()->set_parameters(params.SerializeAsString());

    router.route(init, *writer_, writer_, calls_);
    auto confirmResp = popResponse();
    ASSERT_TRUE(confirmResp.has_observablecommandconfirmation());
    const std::string execUuid = confirmResp.observablecommandconfirmation()
                                      .commandconfirmation()
                                      .commandexecutionuuid()
                                      .value();
    ASSERT_FALSE(execUuid.empty());
    // S18: proves CloudHandlerRegistration.h's verbatim pass-through of the
    // app-built CommandConfirmation (sink.response()) carries
    // lifetimeOfExecution -- no cloud-side change was needed for this half.
    ASSERT_TRUE(confirmResp.observablecommandconfirmation()
                    .commandconfirmation()
                    .has_lifetimeofexecution());
    EXPECT_EQ(confirmResp.observablecommandconfirmation()
                  .commandconfirmation()
                  .lifetimeofexecution()
                  .seconds(),
              60);

    // S17: the router now prefers a registered "_Info" handler over its
    // synthesized pump, and registerCloudHandlers (via regObsCmd) registers
    // ShakeControllerImpl's own onShakeForTimeInfo -- so this exercises the
    // app handler reached through registerCloudHandlers, not the router's
    // {&impl.commandManager()} fallback constructor argument.
    cloud::SiLAClientMessage info;
    info.set_requestuuid("req-e2e-info");
    info.mutable_observablecommandexecutioninfosubscription()
        ->mutable_commandexecutionuuid()
        ->set_value(execUuid);
    router.route(info, *writer_, writer_, calls_);
    auto infoResp = popResponse();
    ASSERT_TRUE(infoResp.has_observablecommandexecutioninfo());
    EXPECT_EQ(infoResp.observablecommandexecutioninfo().commandexecutionuuid().value(), execUuid);
    // onShakeForTimeInfo never sets estimatedRemainingTime
    // (ShakeControllerImpl.cc's onShakeForTimeInfo), while the router's
    // synthesized pump always set it -- the one assertion that tells the two
    // producers apart.
    EXPECT_FALSE(infoResp.observablecommandexecutioninfo().executioninfo().has_estimatedremainingtime());
    // S18: onShakeForTimeInfo stamps updatedLifetimeOfExecution too, guarded
    // on the same lifetime() > 0 check as the confirmation above.
    ASSERT_TRUE(infoResp.observablecommandexecutioninfo().executioninfo().has_updatedlifetimeofexecution());
    EXPECT_EQ(infoResp.observablecommandexecutioninfo().executioninfo().updatedlifetimeofexecution().seconds(),
              60);

    // popResponse() drains a single FIFO shared by every requestUUID across
    // all three live pumps, so intermediate/_Info snapshots can arrive
    // out of order relative to other in-flight requests. Pop until the
    // envelope for the requestUUID under test shows up; popResponse's own 2s
    // timeout bounds the loop.
    auto popFor = [&](const std::string& uuid) {
        for (;;) {
            auto m = popResponse();
            if (m.requestuuid() == uuid) {
                return m;
            }
        }
    };

    // Proves BOTH the emitted "_Intermediate" suffix contract AND
    // wrapObsInit's registerExecutionFQI: dispatchObservableByUuid resolves
    // the FQI from that map.
    cloud::SiLAClientMessage intermediate;
    intermediate.set_requestuuid("req-e2e-intermediate");
    intermediate.mutable_observablecommandintermediateresponsesubscription()
        ->mutable_commandexecutionuuid()
        ->set_value(execUuid);
    router.route(intermediate, *writer_, writer_, calls_);
    auto intermediateResp = popFor("req-e2e-intermediate");
    ASSERT_TRUE(intermediateResp.has_observablecommandintermediateresponse());
    shake_proto::ShakeForTime_IntermediateResponses decoded;
    ASSERT_TRUE(decoded.ParseFromString(intermediateResp.observablecommandintermediateresponse().response()));
    EXPECT_TRUE(decoded.has_timeleft());

    ASSERT_NO_FATAL_FAILURE(waitUntilFinished(impl.commandManager(), execUuid));
    cloud::SiLAClientMessage getResponse;
    getResponse.set_requestuuid("req-e2e-result");
    getResponse.mutable_observablecommandgetresponse()
        ->mutable_commandexecutionuuid()
        ->set_value(execUuid);
    router.route(getResponse, *writer_, writer_, calls_);
    auto resultResp = popFor("req-e2e-result");
    ASSERT_TRUE(resultResp.has_observablecommandresponse());
}

TEST_F(CodegenCloudE2E, GeneratedAdapterRejectsInvalidCommandParameterBeforeCloudHandler) {
    shake_example::ShakeControllerImpl impl{nullptr};
    auto adapter = std::static_pointer_cast<gen_shake::ShakeControllerServiceAdapter>(
        impl.service());
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&impl.commandManager()}};
    adapter->registerCloudHandlers(router, adapter);

    shake_proto::ShakeForTime_Parameters params;
    params.mutable_runtime()->set_value(1);
    params.mutable_targetspeed()->set_value(1.0);  // below MinimalInclusive=4006
    params.mutable_targetpower()->set_value(50.0);
    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-invalid-parameter");
    auto* initiation = init.mutable_observablecommandinitiation();
    initiation->set_fullyqualifiedcommandid(std::string{gen_shake::kFqi} + "/Command/ShakeForTime");
    initiation->mutable_commandparameter()->set_parameters(params.SerializeAsString());

    router.route(init, *writer_, writer_, calls_);
    auto response = popResponse();
    ASSERT_TRUE(response.has_commanderror());
    ASSERT_TRUE(response.commanderror().has_validationerror());
    EXPECT_EQ(response.commanderror().validationerror().parameter(),
              std::string{gen_shake::kFqi} + "/Command/ShakeForTime/Parameter/TargetSpeed");
}

// S15 POSITIVE: wrapObsInit's new ctx.metadata("access-token") snapshot must
// reach the follow-up gate through the REAL codegen'd wiring
// (registerCloudHandlers -> regObsCmd -> wrapObsInit), not only the
// hand-wired registerExecutionFQI fixtures in
// test_fqi_coverage_dual_transport_e2e.cc: initiate WITH a token, then fetch
// the result with NO metadata -- the snapshotted token authorizes it.
TEST_F(CodegenCloudE2E, WrapObsInitSnapshotsTheInitiationToken) {
    sila2::auth::AuthTokenStore store;
    const std::vector<std::string> protectedFqis{
        std::string{gen_shake::ShakeControllerServiceAdapter::kShakeForTimeFqi}};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return sila2::auth::anyFqiCovers(protectedFqis, fqi);
    };
    sila2::auth::AuthorizationInterceptor interceptor{store, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &interceptor;
    // issue() takes a scope SET; protectedFqis stays a vector because
    // anyFqiCovers scans a vector, same as SiLAServerBase::Build().
    const std::string token = store.issue("alice", {protectedFqis.front()}, 60s);

    shake_example::ShakeControllerImpl impl{nullptr};
    auto adapter = std::static_pointer_cast<gen_shake::ShakeControllerServiceAdapter>(
        impl.service());
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&impl.commandManager()}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_shake::kFqi};

    shake_proto::ShakeForTime_Parameters params;
    params.mutable_runtime()->set_value(1);
    params.mutable_targetspeed()->set_value(5000.0);
    params.mutable_targetpower()->set_value(50.0);
    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-s15-init");
    auto* initiation = init.mutable_observablecommandinitiation();
    initiation->set_fullyqualifiedcommandid(fqi + "/Command/ShakeForTime");
    initiation->mutable_commandparameter()->set_parameters(params.SerializeAsString());
    *initiation->mutable_commandparameter()->add_metadata() = makeAccessTokenMetadata(token);

    router.route(init, *writer_, writer_, calls_);
    auto confirmResp = popResponse();
    ASSERT_TRUE(confirmResp.has_observablecommandconfirmation());
    const std::string execUuid = confirmResp.observablecommandconfirmation()
                                      .commandconfirmation()
                                      .commandexecutionuuid()
                                      .value();
    ASSERT_FALSE(execUuid.empty());

    // No metadata slot exists on ObservableCommandGetResponse at all
    // (SiLACloudConnector.proto) -- the ONLY credential this follow-up can
    // ride on is the one wrapObsInit snapshotted at initiation.
    auto popFor = [&](const std::string& uuid) {
        for (;;) {
            auto m = popResponse();
            if (m.requestuuid() == uuid) {
                return m;
            }
        }
    };
    ASSERT_NO_FATAL_FAILURE(waitUntilFinished(impl.commandManager(), execUuid));
    cloud::SiLAClientMessage getResponse;
    getResponse.set_requestuuid("req-s15-result");
    getResponse.mutable_observablecommandgetresponse()
        ->mutable_commandexecutionuuid()
        ->set_value(execUuid);
    router.route(getResponse, *writer_, writer_, calls_);
    auto resultResp = popFor("req-s15-result");
    EXPECT_TRUE(resultResp.has_observablecommandresponse());
    EXPECT_FALSE(resultResp.has_commanderror());
}

// S15 REJECTION: an initiation that the gate rejects (protected feature, no
// token) must leave NOTHING behind -- wrapObsInit never runs, so a
// manager-created execution the router was never told about stays unnamed
// and its follow-up fails with "no command FQI registered", not by silently
// serving through the pre-S15 ungated path.
TEST_F(CodegenCloudE2E, ProtectedFollowupAfterUnauthenticatedInitiationIsDenied) {
    sila2::auth::AuthTokenStore store;
    const std::vector<std::string> protectedFqis{
        std::string{gen_shake::ShakeControllerServiceAdapter::kShakeForTimeFqi}};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return sila2::auth::anyFqiCovers(protectedFqis, fqi);
    };
    sila2::auth::AuthorizationInterceptor interceptor{store, isProtected};
    sila2::InterceptorChain chain;
    chain.auth = &interceptor;

    shake_example::ShakeControllerImpl impl{nullptr};
    auto adapter = std::static_pointer_cast<gen_shake::ShakeControllerServiceAdapter>(
        impl.service());
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&impl.commandManager()}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_shake::kFqi};

    shake_proto::ShakeForTime_Parameters params;
    params.mutable_runtime()->set_value(1);
    params.mutable_targetspeed()->set_value(5000.0);
    params.mutable_targetpower()->set_value(50.0);
    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-s15-noauth");
    auto* initiation = init.mutable_observablecommandinitiation();
    initiation->set_fullyqualifiedcommandid(fqi + "/Command/ShakeForTime");
    initiation->mutable_commandparameter()->set_parameters(params.SerializeAsString());
    // no metadata at all

    router.route(init, *writer_, writer_, calls_);
    auto initResp = popResponse();
    ASSERT_TRUE(initResp.has_commanderror());
    ASSERT_TRUE(initResp.commanderror().has_frameworkerror());
    EXPECT_EQ(initResp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::INVALID_METADATA);

    // A manager-created execution never routed through wrapObsInit resolves
    // no FQI -- same probe shape as test_cloud_handler_registration.cc's N6.
    auto exec = impl.commandManager().addCommand(std::chrono::seconds{300});
    exec->start();

    cloud::SiLAClientMessage getResponse;
    getResponse.set_requestuuid("req-s15-noauth-result");
    getResponse.mutable_observablecommandgetresponse()
        ->mutable_commandexecutionuuid()
        ->set_value(exec->uuid());
    router.route(getResponse, *writer_, writer_, calls_);
    auto resultResp = popResponse();
    ASSERT_TRUE(resultResp.has_commanderror());
    ASSERT_TRUE(resultResp.commanderror().has_frameworkerror());
    EXPECT_NE(resultResp.commanderror().frameworkerror().message().find(
                  "no command FQI registered"),
              std::string::npos);
}

TEST_F(CodegenCloudE2E, ShakeControllerAdapterRegistersUnobservableCommandsOnRouter) {
    shake_example::ShakeControllerImpl impl{nullptr};
    auto adapter = std::static_pointer_cast<gen_shake::ShakeControllerServiceAdapter>(
        impl.service());
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&impl.commandManager()}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_shake::kFqi};

    cloud::SiLAClientMessage exec;
    exec.set_requestuuid("req-e2e-gohome");
    exec.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(fqi + "/Command/GoHome");

    router.route(exec, *writer_, writer_, calls_);
    auto resp = popResponse();
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

// S17: proves wrapObsFollowup's "_Info" registration runs on a pump reachable
// by ActiveCallRegistry::cancel -- the same contract service_adapter.h.j2's
// comment documents for regObsProp.
TEST_F(CodegenCloudE2E, CodegendInfoHandlerIsCancellable) {
    sila2::ObservableCommandManager mgr;
    auto adapter = std::make_shared<gen_basic::ObservableBasicServiceAdapter>();
    adapter->onTestCommand = [&mgr](const basic_proto::TestCommand_Parameters&, sila2::CallContext&,
                                    sila2::ResponseSink<cloud::CommandConfirmation>& sink) {
        auto exec = mgr.addCommand(std::chrono::seconds{300});
        exec->start();
        cloud::CommandConfirmation confirmation;
        confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
        sink.send(confirmation);
        sink.finish();
    };
    std::atomic<bool> handlerExited{false};
    adapter->onTestCommandInfo = [&handlerExited](
            const cloud::CommandExecutionUUID&, sila2::CallContext& ctx,
            sila2::ResponseSink<cloud::ExecutionInfo>& sink) {
        while (!ctx.isCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        handlerExited = true;
        sink.finish();
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&mgr}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_basic::kFqi};

    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-e2e-info-cancel-init");
    basic_proto::TestCommand_Parameters params;
    params.mutable_param1()->set_value(1);
    params.mutable_param2()->set_value("valid");
    auto* initiation = init.mutable_observablecommandinitiation();
    initiation->set_fullyqualifiedcommandid(fqi + "/Command/TestCommand");
    initiation->mutable_commandparameter()->set_parameters(params.SerializeAsString());
    router.route(init, *writer_, writer_, calls_);
    auto confirmResp = popResponse();
    ASSERT_TRUE(confirmResp.has_observablecommandconfirmation());
    const std::string execUuid = confirmResp.observablecommandconfirmation()
                                      .commandconfirmation()
                                      .commandexecutionuuid()
                                      .value();

    cloud::SiLAClientMessage info;
    info.set_requestuuid("req-e2e-info-cancel");
    info.mutable_observablecommandexecutioninfosubscription()
        ->mutable_commandexecutionuuid()
        ->set_value(execUuid);
    router.route(info, *writer_, writer_, calls_);

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-e2e-info-cancel");
    cancel.mutable_cancelobservablecommandexecutioninfosubscription();
    router.route(cancel, *writer_, writer_, calls_);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!handlerExited.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    EXPECT_TRUE(handlerExited.load());
}

// REJECTION: pins that ObsFollowupField::kExecutionInfo takes the commandError
// leg of CloudFollowupResponseSink::fail (not propertyError), and that
// guardHandler wraps the throw into an UndefinedExecutionError.
TEST_F(CodegenCloudE2E, InfoHandlerFailureBecomesACommandError) {
    sila2::ObservableCommandManager mgr;
    auto adapter = std::make_shared<gen_basic::ObservableBasicServiceAdapter>();
    adapter->onTestCommand = [&mgr](const basic_proto::TestCommand_Parameters&, sila2::CallContext&,
                                    sila2::ResponseSink<cloud::CommandConfirmation>& sink) {
        auto exec = mgr.addCommand(std::chrono::seconds{300});
        exec->start();
        cloud::CommandConfirmation confirmation;
        confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
        sink.send(confirmation);
        sink.finish();
    };
    adapter->onTestCommandInfo = [](const cloud::CommandExecutionUUID&, sila2::CallContext&,
                                    sila2::ResponseSink<cloud::ExecutionInfo>&) {
        throw std::runtime_error{"info handler exploded"};
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&mgr}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_basic::kFqi};

    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-e2e-info-throw-init");
    basic_proto::TestCommand_Parameters params;
    params.mutable_param1()->set_value(1);
    params.mutable_param2()->set_value("valid");
    auto* initiation = init.mutable_observablecommandinitiation();
    initiation->set_fullyqualifiedcommandid(fqi + "/Command/TestCommand");
    initiation->mutable_commandparameter()->set_parameters(params.SerializeAsString());
    router.route(init, *writer_, writer_, calls_);
    auto confirmResp = popResponse();
    ASSERT_TRUE(confirmResp.has_observablecommandconfirmation());
    const std::string execUuid = confirmResp.observablecommandconfirmation()
                                      .commandconfirmation()
                                      .commandexecutionuuid()
                                      .value();

    cloud::SiLAClientMessage info;
    info.set_requestuuid("req-e2e-info-throw");
    info.mutable_observablecommandexecutioninfosubscription()
        ->mutable_commandexecutionuuid()
        ->set_value(execUuid);
    router.route(info, *writer_, writer_, calls_);
    auto errResp = popResponse();

    ASSERT_TRUE(errResp.has_commanderror());
    ASSERT_TRUE(errResp.commanderror().has_undefinedexecutionerror());
    EXPECT_FALSE(errResp.has_propertyerror());
}

// REJECTION (S18): a fallback-answered execution with a ZERO lifetime must
// leave updatedLifetimeOfExecution unset. buildExecutionInfo's guard mirrors
// the app-handler guard the two tests above exercise indirectly through
// ShakeControllerImpl -- an unconditional set here would have the router
// claim a lifetime for an execution the manager will never GC.
TEST_F(CodegenCloudE2E, FallbackInfoForAZeroLifetimeExecutionLeavesTheFieldUnset) {
    sila2::ObservableCommandManager mgr;
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&mgr}};

    // addCommand's default lifetime is already zero; spelled out here so this
    // test's "unset" assertion reads without cross-referencing addCommand.
    auto exec = mgr.addCommand(std::chrono::seconds{0});
    exec->start();

    // Never routed through an initiation, so no adapter FQI is registered for
    // this UUID: executionFqis_ has no entry, hasInfoHandler is false, and
    // the router's synthesized pump (buildExecutionInfo) answers, not an app
    // handler.
    cloud::SiLAClientMessage info;
    info.set_requestuuid("req-e2e-zero-lifetime-info");
    info.mutable_observablecommandexecutioninfosubscription()
        ->mutable_commandexecutionuuid()
        ->set_value(exec->uuid());
    router.route(info, *writer_, writer_, calls_);

    auto infoResp = popResponse();
    ASSERT_TRUE(infoResp.has_observablecommandexecutioninfo());
    EXPECT_FALSE(
        infoResp.observablecommandexecutioninfo().executioninfo().has_updatedlifetimeofexecution());
}

// ---------------------------------------------------------------------------
// 1.2i: an observable command with no IntermediateResponse.
// ---------------------------------------------------------------------------

TEST_F(CodegenCloudE2E, NoIntermediateObservableCommandIsReachableOverCloud) {
    // Declared before the router so the router (and its pump join) destructs
    // first.
    sila2::ObservableCommandManager mgr;
    auto adapter = std::make_shared<gen_nointer::ObservableCommandNoIntermediateResponseServiceAdapter>();
    adapter->onTestCommand = [&mgr](const nointer_proto::TestCommand_Parameters&, sila2::CallContext&,
                                    sila2::ResponseSink<cloud::CommandConfirmation>& sink) {
        auto exec = mgr.addCommand(std::chrono::seconds{300});
        exec->start();
        cloud::CommandConfirmation confirmation;
        confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
        sink.send(confirmation);
        sink.finish();
    };
    adapter->onTestCommandResult = [&mgr](const cloud::CommandExecutionUUID& req, sila2::CallContext&,
                                          sila2::ResponseSink<nointer_proto::TestCommand_Responses>& sink) {
        auto exec = mgr.getCommand(req.value());
        sink.send(nointer_proto::TestCommand_Responses{});
        exec->finish();
        sink.finish();
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&mgr}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_nointer::kFqi};

    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-e2e-noint-init");
    init.mutable_observablecommandinitiation()->set_fullyqualifiedcommandid(fqi + "/Command/TestCommand");
    router.route(init, *writer_, writer_, calls_);
    auto confirmResp = popResponse();
    ASSERT_TRUE(confirmResp.has_observablecommandconfirmation());
    const std::string execUuid = confirmResp.observablecommandconfirmation()
                                      .commandconfirmation()
                                      .commandexecutionuuid()
                                      .value();

    cloud::SiLAClientMessage getResponse;
    getResponse.set_requestuuid("req-e2e-noint-result");
    getResponse.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(execUuid);
    router.route(getResponse, *writer_, writer_, calls_);
    auto resultResp = popResponse();
    ASSERT_TRUE(resultResp.has_observablecommandresponse());
}

TEST_F(CodegenCloudE2E, NoIntermediateObservableCommandRejectsIntermediateSubscription) {
    sila2::ObservableCommandManager mgr;
    auto adapter = std::make_shared<gen_nointer::ObservableCommandNoIntermediateResponseServiceAdapter>();
    adapter->onTestCommand = [&mgr](const nointer_proto::TestCommand_Parameters&, sila2::CallContext&,
                                    sila2::ResponseSink<cloud::CommandConfirmation>& sink) {
        auto exec = mgr.addCommand(std::chrono::seconds{300});
        exec->start();
        cloud::CommandConfirmation confirmation;
        confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
        sink.send(confirmation);
        sink.finish();
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, nullptr, nullptr, {&mgr}};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_nointer::kFqi};

    cloud::SiLAClientMessage init;
    init.set_requestuuid("req-e2e-noint-neg-init");
    init.mutable_observablecommandinitiation()->set_fullyqualifiedcommandid(fqi + "/Command/TestCommand");
    router.route(init, *writer_, writer_, calls_);
    auto confirmResp = popResponse();
    ASSERT_TRUE(confirmResp.has_observablecommandconfirmation());
    const std::string execUuid = confirmResp.observablecommandconfirmation()
                                      .commandconfirmation()
                                      .commandexecutionuuid()
                                      .value();

    // The two-handler overload deliberately registers nothing under
    // "_Intermediate", matching the FDL having no IntermediateResponse.
    cloud::SiLAClientMessage intermediate;
    intermediate.set_requestuuid("req-e2e-noint-neg-intermediate");
    intermediate.mutable_observablecommandintermediateresponsesubscription()
        ->mutable_commandexecutionuuid()
        ->set_value(execUuid);
    router.route(intermediate, *writer_, writer_, calls_);
    auto rejected = popResponse();
    ASSERT_TRUE(rejected.has_commanderror());
    ASSERT_TRUE(rejected.commanderror().has_frameworkerror());
    EXPECT_EQ(rejected.commanderror().frameworkerror().message(),
              "no handler registered for: " + fqi + "/Command/TestCommand_Intermediate");
}

// ---------------------------------------------------------------------------
// 1.2k: a codegen'd observable property, streaming through the SAME Subscribe_
// SilaHandler the direct-gRPC path runs, pumped by the router.
// ---------------------------------------------------------------------------

const std::string kPropId = "TestProperty";

TEST_F(CodegenCloudE2E, CodegendObservablePropertyStreamsValuesOverCloud) {
    // Declared before the router so the router (and its pump join) destructs
    // first.
    sila2::ObservablePropertyManager propMgr;
    auto adapter = std::make_shared<gen_basic::ObservableBasicServiceAdapter>();
    // Reference pattern -- copied verbatim, it is what device repos will imitate.
    adapter->onSubscribeTestProperty = [&propMgr](
            const basic_proto::Subscribe_TestProperty_Parameters&, sila2::CallContext& ctx,
            sila2::ResponseSink<basic_proto::Subscribe_TestProperty_Responses>& sink) {
        auto sub = propMgr.subscribe(kPropId);
        // The pump thread is joined at router teardown, so the handler must be
        // wakeable from outside: onCancellation cancels the Subscription, which
        // makes waitForNext() return nullopt. Polling isCancelled() alone would
        // leave this parked in waitForNext() forever.
        ctx.onCancellation([&propMgr, sub] { propMgr.unsubscribe(kPropId, sub); sub->cancel(); });
        while (auto value = sub->waitForNext()) {
            basic_proto::Subscribe_TestProperty_Responses resp;
            resp.mutable_testproperty()->set_value(std::any_cast<std::int64_t>(*value));
            sink.send(resp);
        }
        sink.finish();
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_basic::kFqi};

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-e2e-prop-sub");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(fqi + "/Property/TestProperty");
    router.route(sub, *writer_, writer_, calls_);

    // Unlike the manager path (subscribe happens synchronously inside
    // route()), the handler-path subscribe runs on the pump thread, so it
    // lands microseconds after route() returns. publish() only enqueues to
    // already-registered subscribers, so publishing before the pump has
    // subscribed would drop the value. This wait doubles as the assertion
    // that regObsProp actually reached the handler.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (propMgr.subscriberCount(kPropId) != 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(propMgr.subscriberCount(kPropId), 1u);

    propMgr.publish(kPropId, std::int64_t{1});
    propMgr.publish(kPropId, std::int64_t{2});

    for (std::int64_t expected : {1, 2}) {
        auto resp = popResponse();
        ASSERT_TRUE(resp.has_observablepropertyvalue());
        basic_proto::Subscribe_TestProperty_Responses decoded;
        ASSERT_TRUE(decoded.ParseFromString(resp.observablepropertyvalue().value()));
        EXPECT_EQ(decoded.testproperty().value(), expected);
    }
}

TEST_F(CodegenCloudE2E, CodegendObservablePropertySubscriptionIsCancellable) {
    sila2::ObservablePropertyManager propMgr;
    auto adapter = std::make_shared<gen_basic::ObservableBasicServiceAdapter>();
    adapter->onSubscribeTestProperty = [&propMgr](
            const basic_proto::Subscribe_TestProperty_Parameters&, sila2::CallContext& ctx,
            sila2::ResponseSink<basic_proto::Subscribe_TestProperty_Responses>& sink) {
        auto sub = propMgr.subscribe(kPropId);
        ctx.onCancellation([&propMgr, sub] { propMgr.unsubscribe(kPropId, sub); sub->cancel(); });
        while (auto value = sub->waitForNext()) {
            basic_proto::Subscribe_TestProperty_Responses resp;
            resp.mutable_testproperty()->set_value(std::any_cast<std::int64_t>(*value));
            sink.send(resp);
        }
        sink.finish();
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_basic::kFqi};

    cloud::SiLAClientMessage sub;
    sub.set_requestuuid("req-e2e-prop-cancel");
    sub.mutable_observablepropertysubscription()->set_fullyqualifiedpropertyid(fqi + "/Property/TestProperty");
    router.route(sub, *writer_, writer_, calls_);

    // See CodegendObservablePropertyStreamsValuesOverCloud: the handler-path
    // subscribe happens on the pump thread, not synchronously inside route(),
    // so wait for it before publishing or the value is dropped.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (propMgr.subscriberCount(kPropId) != 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_EQ(propMgr.subscriberCount(kPropId), 1u);

    propMgr.publish(kPropId, std::int64_t{1});
    auto delivered = popResponse();
    ASSERT_TRUE(delivered.has_observablepropertyvalue());

    cloud::SiLAClientMessage cancel;
    cancel.set_requestuuid("req-e2e-prop-cancel");
    cancel.mutable_cancelobservablepropertysubscription();
    router.route(cancel, *writer_, writer_, calls_);

    // Guards the pump-join path in ~CloudEnvelopeRouter: a handler that
    // ignored cancellation would hang the destructor and this test would time
    // out rather than fail cleanly.
    propMgr.publish(kPropId, std::int64_t{2});
    EXPECT_TRUE(noResponse(std::chrono::milliseconds{200}));
    EXPECT_EQ(propMgr.subscriberCount(kPropId), 0u);
}

// ---------------------------------------------------------------------------
// S71: the cloud path must resolve a Binary Transfer UUID Command parameter
// the same way the direct-gRPC path does (Part B p57). Proves the resolve
// service_adapter.h.j2 now performs inside validated_<handler> -- the point
// both wrapGrpc (cloud) and GrpcTransport::dispatchToHandler (gRPC) funnel a
// Command through -- actually runs on the cloud transport, using a real
// generated adapter (BinaryTransferTest) with a Binary Command parameter.
// ---------------------------------------------------------------------------

TEST_F(CodegenCloudE2E, CloudCommandResolvesBinaryTransferUuidParameter) {
    // One store wired to BOTH the adapter's InterceptorChain (so
    // validated_<handler> can resolve) and the router's own binaryStore (so
    // CreateBinaryUpload/UploadChunk have somewhere to write) -- the same
    // "same store" wiring SiLAServerBase::Builder::Build() uses in production.
    sila2::InMemoryBinaryStore store;
    sila2::InterceptorChain chain;
    chain.binaryStore = &store;
    chain.binarySlotLifetime = 300s;

    auto adapter = std::make_shared<gen_binary::BinaryTransferTestServiceAdapter>(&chain);
    std::string received;
    adapter->onEchoBinaryValue = [&received](
            const binary_proto::EchoBinaryValue_Parameters& req, sila2::CallContext&,
            sila2::ResponseSink<binary_proto::EchoBinaryValue_Responses>& sink) {
        // If validated_<handler> did NOT resolve the parameter, req.binaryvalue()
        // would still carry the raw binaryTransferUUID union arm here.
        received = req.binaryvalue().value();
        binary_proto::EchoBinaryValue_Responses resp;
        resp.mutable_receivedvalue()->set_value(received);
        sink.send(resp);
        sink.finish();
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain, &store};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_binary::kFqi};
    const std::string uploaded = "hello";

    cloud::SiLAClientMessage createMsg;
    createMsg.set_requestuuid("req-s71-create");
    auto* createReq = createMsg.mutable_createbinaryuploadrequest()->mutable_createbinaryrequest();
    createReq->set_binarysize(uploaded.size());
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier(fqi + "/Command/EchoBinaryValue/Parameter/BinaryValue");
    router.route(createMsg, *writer_, writer_, calls_);
    auto createResp = popResponse();
    ASSERT_TRUE(createResp.has_createbinaryresponse());
    const std::string uuid = createResp.createbinaryresponse().binarytransferuuid();
    ASSERT_FALSE(uuid.empty());

    cloud::SiLAClientMessage chunkMsg;
    chunkMsg.set_requestuuid("req-s71-chunk");
    auto* chunk = chunkMsg.mutable_uploadchunkrequest();
    chunk->set_binarytransferuuid(uuid);
    chunk->set_chunkindex(0);
    chunk->set_payload(uploaded);
    router.route(chunkMsg, *writer_, writer_, calls_);
    ASSERT_TRUE(popResponse().has_uploadchunkresponse());

    binary_proto::EchoBinaryValue_Parameters params;
    params.mutable_binaryvalue()->set_binarytransferuuid(uuid);
    cloud::SiLAClientMessage cmdMsg;
    cmdMsg.set_requestuuid("req-s71-cmd");
    auto* exec = cmdMsg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(fqi + "/Command/EchoBinaryValue");
    exec->mutable_commandparameter()->set_parameters(params.SerializeAsString());
    router.route(cmdMsg, *writer_, writer_, calls_);
    auto cmdResp = popResponse();

    ASSERT_TRUE(cmdResp.has_unobservablecommandresponse());
    binary_proto::EchoBinaryValue_Responses decoded;
    ASSERT_TRUE(decoded.ParseFromString(cmdResp.unobservablecommandresponse().response()));
    EXPECT_EQ(decoded.receivedvalue().value(), uploaded);
    EXPECT_EQ(received, uploaded);
}

// REJECTION: a Binary Transfer UUID that was never uploaded must fail the
// resolve inside validated_<handler> BEFORE the handler runs -- proves the
// resolve is not skipped/best-effort on the cloud path.
TEST_F(CodegenCloudE2E, CloudCommandWithUnknownBinaryUuidIsRejected) {
    sila2::InMemoryBinaryStore store;
    sila2::InterceptorChain chain;
    chain.binaryStore = &store;
    chain.binarySlotLifetime = 300s;

    auto adapter = std::make_shared<gen_binary::BinaryTransferTestServiceAdapter>(&chain);
    std::string received;
    adapter->onEchoBinaryValue = [&received](
            const binary_proto::EchoBinaryValue_Parameters& req, sila2::CallContext&,
            sila2::ResponseSink<binary_proto::EchoBinaryValue_Responses>& sink) {
        received = req.binaryvalue().value();
        binary_proto::EchoBinaryValue_Responses resp;
        resp.mutable_receivedvalue()->set_value(received);
        sink.send(resp);
        sink.finish();
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain, &store};
    adapter->registerCloudHandlers(router, adapter);

    std::string fqi{gen_binary::kFqi};

    binary_proto::EchoBinaryValue_Parameters params;
    params.mutable_binaryvalue()->set_binarytransferuuid("never-uploaded-uuid");
    cloud::SiLAClientMessage cmdMsg;
    cmdMsg.set_requestuuid("req-s71-unknown-uuid");
    auto* exec = cmdMsg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid(fqi + "/Command/EchoBinaryValue");
    exec->mutable_commandparameter()->set_parameters(params.SerializeAsString());
    router.route(cmdMsg, *writer_, writer_, calls_);
    auto resp = popResponse();

    EXPECT_FALSE(resp.has_unobservablecommandresponse());
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::COMMAND_EXECUTION_NOT_ACCEPTED);
    // The handler never ran: resolveBinaryParameters throws before
    // onEchoBinaryValue is invoked inside validated_onEchoBinaryValue.
    EXPECT_TRUE(received.empty());
}

}  // namespace
