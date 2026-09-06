// End-to-end tests for the ShakeController example Feature
// (tests/examples/shake_controller/ShakeControllerImpl.cc): a real
// SiLAServerBase with ShakeControllerImpl registered, dialed over gRPC with
// the codegen-generated ShakeController stub. Covers every RPC path the
// implementation actually defines — StartShaking / StopShaking (with its two
// branches), the full ShakeForTime Observable Command lifecycle (initiate ->
// Info -> Result), and the error paths reachable from a real client.
//
// Note: the ShakeController FDL (tests/examples/fdl/teleshake/
// ShakeController.sila.xml) declares no Property — only six Commands — so
// there is no "property read" True path to exercise here, unlike other
// example Features.
#include "ShakeControllerImpl.h"
#include "ShakeControllerMeta.h"

#include <sila/server/SiLAServerBase.h>
#include <sila/server/command/ObservableCommandExecution.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace shake_proto = sila2::org::silastandard::examples::shakecontroller::v1;
namespace fw = sila2::org::silastandard;
namespace gen = sila2::generated::shakecontroller;

using sila2::SiLAServerBase;

// Real local channel dialed against the server's own self-signed
// certificate — same pattern as test_sila_server_base_run_shutdown_e2e.cc's
// dialChannel().
std::shared_ptr<grpc::Channel> dialChannel(const SiLAServerBase& server) {
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    return grpc::CreateChannel("localhost:" + std::to_string(server.port()),
                                grpc::SslCredentials(opts));
}

// Parses a failed RPC's error_details() back into the SiLAError protobuf
// message ErrorTransmitInterceptor.h's guardHandler() serialized
// (SiLAError::toStatus(), sila/common/error/SiLAError.cc:52) — every False
// path below ends up here rather than at a bare grpc::StatusCode, since that
// is the only way to tell a DefinedExecutionError apart from a FrameworkError
// at the client.
fw::SiLAError parseSilaError(const grpc::Status& status) {
    fw::SiLAError error;
    error.ParseFromString(status.error_details());
    return error;
}

// Polls ShakeForTime_Info until the stream reports a terminal CommandStatus,
// returning that final ExecutionInfo. Mirrors ShakeControllerImpl.cc's own
// onShakeForTimeInfo poll loop (200ms) on the client side.
fw::ExecutionInfo pollUntilFinished(shake_proto::ShakeController::Stub& stub,
                                     const std::string& uuid) {
    fw::CommandExecutionUUID req;
    req.set_value(uuid);
    fw::ExecutionInfo info;
    while (true) {
        grpc::ClientContext ctx;
        auto reader = stub.ShakeForTime_Info(&ctx, req);
        while (reader->Read(&info)) {
            const bool isFinished =
                info.commandstatus() == fw::ExecutionInfo_CommandStatus_finishedSuccessfully ||
                info.commandstatus() == fw::ExecutionInfo_CommandStatus_finishedWithError;
            if (isFinished) {
                reader->Finish();
                return info;
            }
        }
        reader->Finish();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

// Opens ShakeForTime_Info on its own ClientContext, reads one ExecutionInfo,
// then cancels and drops the reader without calling Finish() — the point
// under test is what the server does when the stream is severed (§3.3), not
// the client-side status this particular RPC returns.
void cancelInfoStreamAfterFirstRead(shake_proto::ShakeController::Stub& stub, const std::string& uuid) {
    grpc::ClientContext infoCtx;
    fw::CommandExecutionUUID infoReq;
    infoReq.set_value(uuid);
    auto reader = stub.ShakeForTime_Info(&infoCtx, infoReq);
    fw::ExecutionInfo info;
    ASSERT_TRUE(reader->Read(&info));
    infoCtx.TryCancel();
}

}  // namespace

// ---------------------------------------------------------------------------
// ShakeController — True (positive) paths
// ---------------------------------------------------------------------------

// Every test in this suite stands up the same embedded server; the fixture
// replaces nine copies of the eleven-line Builder dance (SC8 review
// cleanup). A startShakeServer() free helper cannot return the server --
// SiLAServerBase deletes its move operations -- so the server member is
// initialized straight from Build()'s prvalue, which constructs it in place
// (guaranteed elision, no move involved).
class ShakeControllerRun : public ::testing::Test {
protected:
    sila2::SiLAServerBase::Builder builder;
    shake_example::ShakeControllerImpl impl;
    sila2::SiLAServerBase server;
    std::unique_ptr<shake_proto::ShakeController::Stub> stub;

    ShakeControllerRun()
        : impl{certifiedChain()},
          server{builder
                     .AddFeature(std::string{gen::kFqi}, std::string{gen::kFdlXml}, impl.service())
                     .RegisterCommandManager(&impl.commandManager())
                     .WithDiscovery(0)
                     .Build()} {
        server.Run(false);
        stub = shake_proto::ShakeController::NewStub(dialChannel(server));
    }

    // The certificate has to be configured before chain() is handed to the
    // impl, preserving the order the tests used inline.
    const sila2::InterceptorChain* certifiedChain() {
        builder.WithSelfSignedCertificate("localhost", "127.0.0.1");
        builder.WithConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"));
        return builder.chain();
    }
};

TEST_F(ShakeControllerRun, StartShakingSucceeds) {
    grpc::ClientContext ctx;
    shake_proto::StartShaking_Parameters req;
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    shake_proto::StartShaking_Responses resp;
    auto status = stub->StartShaking(&ctx, req, &resp);

    EXPECT_TRUE(status.ok()) << status.error_message();

    server.Shutdown();
}

// Exercises StopShaking's positive branch (shaking_ == true) — distinct from
// the "not currently shaking" rejection branch covered by
// StopShakingWithoutStartFailsWithDefinedExecutionError below.
TEST_F(ShakeControllerRun, StopShakingAfterStartSucceeds) {
    {
        grpc::ClientContext startCtx;
        shake_proto::StartShaking_Parameters startReq;
        startReq.mutable_targetspeed()->set_value(5000.0);
        startReq.mutable_targetpower()->set_value(50.0);
        shake_proto::StartShaking_Responses startResp;
        ASSERT_TRUE(stub->StartShaking(&startCtx, startReq, &startResp).ok());
    }

    grpc::ClientContext stopCtx;
    shake_proto::StopShaking_Parameters stopReq;
    shake_proto::StopShaking_Responses stopResp;
    auto status = stub->StopShaking(&stopCtx, stopReq, &stopResp);

    EXPECT_TRUE(status.ok()) << status.error_message();

    server.Shutdown();
}

TEST_F(ShakeControllerRun, ShakeForTimeCompletesInitiateInfoResult) {

    // --- initiate: returns a CommandConfirmation carrying a UUID ---
    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(1);  // 1s: keeps the worker thread's sleep short
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());
    const std::string uuid = confirmation.commandexecutionuuid().value();
    EXPECT_FALSE(uuid.empty());

    // --- info: poll until the worker thread finishes ---
    const auto finalInfo = pollUntilFinished(*stub, uuid);
    EXPECT_EQ(finalInfo.commandstatus(), fw::ExecutionInfo_CommandStatus_finishedSuccessfully);

    // --- result: available once Info reports a terminal state ---
    grpc::ClientContext resultCtx;
    fw::CommandExecutionUUID resultReq;
    resultReq.set_value(uuid);
    shake_proto::ShakeForTime_Responses resultResp;
    auto resultStatus = stub->ShakeForTime_Result(&resultCtx, resultReq, &resultResp);
    EXPECT_TRUE(resultStatus.ok()) << resultStatus.error_message();

    server.Shutdown();
}

// ---------------------------------------------------------------------------
// ShakeController — False (negative/rejection) paths
// ---------------------------------------------------------------------------

// CAUGHT: ShakeControllerImpl.cc's onStopShaking checks shaking_ and raises a
// DefinedExecutionError (FDL identifier CancelledError) when nothing is
// shaking — a fresh server's shaking_ starts false, so no StartShaking is
// needed to reach this branch.
TEST_F(ShakeControllerRun, StopShakingWithoutStartFailsWithDefinedExecutionError) {
    grpc::ClientContext ctx;
    shake_proto::StopShaking_Parameters req;
    shake_proto::StopShaking_Responses resp;
    auto status = stub->StopShaking(&ctx, req, &resp);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto error = parseSilaError(status);
    ASSERT_TRUE(error.has_definedexecutionerror());
    EXPECT_EQ(error.definedexecutionerror().erroridentifier(), std::string{gen::kError_CancelledError});

    server.Shutdown();
}

// CAUGHT: ShakeForTime_Result delegates UUID lookup to
// ObservableCommandManager::getCommand(), which throws FrameworkError
// (InvalidCommandExecutionUuid) for a UUID it never issued — before touching
// the states_ map ShakeControllerImpl.cc keys by UUID.
TEST_F(ShakeControllerRun, ShakeForTimeResultWithUnknownUuidFailsWithFrameworkError) {
    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value("not-a-real-command-execution-uuid");
    shake_proto::ShakeForTime_Responses resp;
    auto status = stub->ShakeForTime_Result(&ctx, req, &resp);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto error = parseSilaError(status);
    ASSERT_TRUE(error.has_frameworkerror());
    EXPECT_EQ(error.frameworkerror().errortype(),
              fw::FrameworkError_ErrorType_INVALID_COMMAND_EXECUTION_UUID);

    server.Shutdown();
}

// TargetSpeed is validated by the generated adapter: the application handler
// deliberately ignores it, so this cannot be its hand-written Runtime check.
TEST_F(ShakeControllerRun, GeneratedAdapterRejectsInvalidTargetSpeed) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(1);
    req.mutable_targetspeed()->set_value(1.0);  // below MinimalInclusive=4006
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    auto status = stub->ShakeForTime(&initiateCtx, req, &confirmation);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto error = parseSilaError(status);
    ASSERT_TRUE(error.has_validationerror());
    EXPECT_EQ(error.validationerror().parameter(),
              "org.silastandard/examples/ShakeController/v1/Command/ShakeForTime/Parameter/TargetSpeed");

    server.Shutdown();
}

// ---------------------------------------------------------------------------
// ShakeController — Info stream cancellation (S16, revised per
// architecture-v2.md:286-288 / §3.3 ②)
//
// Part A ties Observable Command continuation to the *Connection*, not to
// any one stream dropping, and direct gRPC's grpc::ServerContext::IsCancelled()
// cannot tell "client explicitly dropped this _Info RPC" apart from "the
// whole connection died" -- both surface identically at the API level. With
// no reliable way to split them, onShakeForTimeInfo now treats every
// isCancelled() as "stop sending, keep executing": it never calls
// exec->requestInterruption(). These exercise that end to end: the gRPC
// transport marks the RPC cancelled, onShakeForTimeInfo's cancelled exit
// only breaks its own send loop, and the worker thread runs to completion
// untouched.
// ---------------------------------------------------------------------------

TEST_F(ShakeControllerRun, InfoStreamCancellationDoesNotInterruptShakeForTime) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(2);  // long enough that the worker is still looping when cancelled
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());
    const std::string uuid = confirmation.commandexecutionuuid().value();

    cancelInfoStreamAfterFirstRead(*stub, uuid);

    auto exec = impl.commandManager().getCommand(uuid);
    // Give the worker's 1s poll of isInterruptionRequested() a couple of
    // chances to (wrongly) fire before asserting it never does.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    EXPECT_FALSE(exec->isInterruptionRequested());

    // pollUntilFinished opens a fresh _Info stream (its own ClientContext) --
    // distinct from the one cancelled above -- and confirms the worker ran
    // to completion instead of being cut short by that earlier cancel.
    const auto finalInfo = pollUntilFinished(*stub, uuid);
    EXPECT_EQ(finalInfo.commandstatus(), fw::ExecutionInfo_CommandStatus_finishedSuccessfully);
    EXPECT_EQ(exec->state(), sila2::ObservableCommandExecution::State::FinishedSuccessfully);
    EXPECT_FALSE(exec->isInterruptionRequested());

    server.Shutdown();
}

// REJECTION: guards against requestInterruption() placed on the shared exit
// path (rather than the cancelled-only branch), which would turn every
// completed command into a cancelled one.
TEST_F(ShakeControllerRun, CompletedShakeForTimeIsNotMarkedInterrupted) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(1);  // 1s: keeps the worker thread's sleep short
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());
    const std::string uuid = confirmation.commandexecutionuuid().value();

    // Drains ShakeForTime_Info to its terminal message without ever cancelling.
    const auto finalInfo = pollUntilFinished(*stub, uuid);
    EXPECT_EQ(finalInfo.commandstatus(), fw::ExecutionInfo_CommandStatus_finishedSuccessfully);

    auto exec = impl.commandManager().getCommand(uuid);
    EXPECT_FALSE(exec->isInterruptionRequested());
    EXPECT_EQ(exec->state(), sila2::ObservableCommandExecution::State::FinishedSuccessfully);

    server.Shutdown();
}

// The old premise ("cancelling _Info stops the worker") is gone, so the
// DefinedExecutionError this used to assert can no longer happen here --
// shaking_ never gets cleared by the worker on this path. Rewritten to
// prove the inverse: the worker (and therefore shaking_ == true) survives
// the _Info cancel, so StopShaking still takes its normal success branch
// instead of StopShakingWithoutStartFailsWithDefinedExecutionError's.
TEST_F(ShakeControllerRun, StopShakingAfterCancelledInfoStreamStillSucceeds) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(3);  // long enough that the worker is still looping when StopShaking is called
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());
    const std::string uuid = confirmation.commandexecutionuuid().value();

    cancelInfoStreamAfterFirstRead(*stub, uuid);

    auto exec = impl.commandManager().getCommand(uuid);
    EXPECT_FALSE(exec->isInterruptionRequested());

    grpc::ClientContext stopCtx;
    shake_proto::StopShaking_Parameters stopReq;
    shake_proto::StopShaking_Responses stopResp;
    auto status = stub->StopShaking(&stopCtx, stopReq, &stopResp);

    EXPECT_TRUE(status.ok()) << status.error_message();

    server.Shutdown();
}

// ---------------------------------------------------------------------------
// ShakeController — _Info stream change detection (S17)
//
// onShakeForTimeInfo now dedups like ShakeForTime_Intermediate: push on
// subscribe, then only on a state/progress transition. Without this, a
// codegen'd cloud subscription (which now runs this same handler, S17) would
// regress from the router's push-on-change cadence to one envelope per 200ms
// poll.
// ---------------------------------------------------------------------------

TEST_F(ShakeControllerRun, InfoStreamSendsOnlyOnChange) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(2);  // 2s: enough 200ms polls to show the difference
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());
    const std::string uuid = confirmation.commandexecutionuuid().value();

    grpc::ClientContext infoCtx;
    fw::CommandExecutionUUID infoReq;
    infoReq.set_value(uuid);
    auto reader = stub->ShakeForTime_Info(&infoCtx, infoReq);
    fw::ExecutionInfo info;
    int messageCount = 0;
    while (reader->Read(&info)) {
        ++messageCount;
        const bool isFinished =
            info.commandstatus() == fw::ExecutionInfo_CommandStatus_finishedSuccessfully ||
            info.commandstatus() == fw::ExecutionInfo_CommandStatus_finishedWithError;
        if (isFinished) {
            break;
        }
    }
    reader->Finish();

    // Pre-fix the handler emits one envelope per 200ms poll (~11 for a 2s
    // run, no dedup). Waiting + one per per-second progress change + terminal
    // stays well under this bound with the fix.
    EXPECT_LE(messageCount, 6);
    // Lower bound + terminal state: without these a stream that dies on the
    // first Read would pass the upper bound vacuously (SC9 review).
    EXPECT_GE(messageCount, 2);
    EXPECT_EQ(info.commandstatus(), fw::ExecutionInfo_CommandStatus_finishedSuccessfully);

    server.Shutdown();
}

// ---------------------------------------------------------------------------
// ShakeController — lifetimeOfExecution / updatedLifetimeOfExecution (S18)
//
// ShakeControllerImpl.cc's onShakeForTime creates every execution with
// addCommand(60s), so both wire fields must carry that exact 60 on the real
// gRPC path — the FrameworkError-only signal a client had before told it
// nothing about when the UUID stops resolving.
// ---------------------------------------------------------------------------

TEST_F(ShakeControllerRun, CommandConfirmationCarriesLifetimeOfExecution) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(1);
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());

    // has_ before the value: proto3 also reports 0 for an UNSET Duration
    // submessage, so a value-only assertion would pass vacuously against the
    // pre-fix tree. 60 is pinned to ShakeControllerImpl.cc:47's addCommand
    // argument deliberately -- do not loosen this to > 0.
    ASSERT_TRUE(confirmation.has_lifetimeofexecution());
    EXPECT_EQ(confirmation.lifetimeofexecution().seconds(), 60);

    server.Shutdown();
}

TEST_F(ShakeControllerRun, InfoStreamCarriesUpdatedLifetimeOfExecution) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(1);
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());
    const std::string uuid = confirmation.commandexecutionuuid().value();

    grpc::ClientContext infoCtx;
    fw::CommandExecutionUUID infoReq;
    infoReq.set_value(uuid);
    auto reader = stub->ShakeForTime_Info(&infoCtx, infoReq);
    fw::ExecutionInfo info;
    int messagesSeen = 0;
    while (reader->Read(&info)) {
        ++messagesSeen;
        ASSERT_TRUE(info.has_updatedlifetimeofexecution());
        EXPECT_EQ(info.updatedlifetimeofexecution().seconds(), 60);
        const bool isFinished =
            info.commandstatus() == fw::ExecutionInfo_CommandStatus_finishedSuccessfully ||
            info.commandstatus() == fw::ExecutionInfo_CommandStatus_finishedWithError;
        if (isFinished) {
            break;
        }
    }
    reader->Finish();
    EXPECT_GT(messagesSeen, 0);

    server.Shutdown();
}

// REJECTION: fails against the pre-fix tree, where lifetimeOfExecution was
// never set and confirmation.lifetimeofexecution().seconds() reads back the
// proto3 default of 0 for an unset submessage.
TEST_F(ShakeControllerRun, LifetimeOfExecutionIsNotTheProto3Default) {

    grpc::ClientContext initiateCtx;
    shake_proto::ShakeForTime_Parameters req;
    req.mutable_runtime()->set_value(1);
    req.mutable_targetspeed()->set_value(5000.0);
    req.mutable_targetpower()->set_value(50.0);
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(stub->ShakeForTime(&initiateCtx, req, &confirmation).ok());

    EXPECT_NE(confirmation.lifetimeofexecution().seconds(), 0);

    server.Shutdown();
}
