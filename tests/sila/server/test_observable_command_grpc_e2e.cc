// End-to-end tests for the Observable Command 4-RPC quartet (Initiate /
// _Info / _Intermediate / _Result, architecture.md §3.3) driven over a real
// gRPC connection to a SilaServerBase, exactly as a SiLA client would talk
// to it: Builder::addFeature() registers a feature's grpc::Service, run()
// starts a TLS-secured listener, and the test dials it with a real
// grpc::Channel.
//
// No FDL-codegen'd feature in this tree has an Observable Command yet (grep
// confirms none of src/sila's generated *.grpc.pb.h declare an "_Info"
// method), so LongRunningTestService below is a hand-rolled grpc::Service —
// built from the same grpc::internal::RpcServiceMethod / RpcMethodHandler /
// ServerStreamingHandler machinery protoc emits (compare
// LockController.grpc.pb.cc's Service::Service()), just written by hand for
// one throwaway test feature instead of generated from a .proto. It reuses
// already-compiled SiLAFramework message types (String, CommandConfirmation,
// CommandExecutionUUID, ExecutionInfo) as request/response shapes, so this
// file needs no new .proto/codegen step and CMakeLists.txt stays untouched.
// LongRunningTestClient is the client-side mirror, built from the same
// BlockingUnaryCall/ClientReaderFactory primitives a generated Stub uses.
#include <sila/server/SilaServerBase.h>

#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/GrpcTransport.h>

#include <SiLAFramework.pb.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/client_unary_call.h>
#include <grpcpp/impl/proto_utils.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/support/sync_stream.h>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using sila2::CallContext;
using sila2::GrpcStreamResponseSink;
using sila2::GrpcUnaryResponseSink;
using sila2::ObservableCommandExecution;
using sila2::ObservableCommandManager;
using sila2::ResponseSink;
using sila2::SilaServerBase;
using sila2::SilaHandler;
using sila2::error::fromGrpcStatus;
using sila2::error::ExecutionError;
using sila2::error::FrameworkError;
using sila2::error::UndefinedExecutionError;

namespace fw = sila2::org::silastandard;

// FeatureRegistry (S46) derives the registered FQI from kFdl's own root
// <Feature> identity below (Originator="org.test" Category="test"), so this
// must be the four-segment form the FDL implies, not a shorter placeholder.
const char kFqi[] = "org.test/test/LongRunningTestFeature/v1";

const char kMethodInitiate[] = "/org.test.LongRunningTestFeature/LongRunning";
const char kMethodInfo[] = "/org.test.LongRunningTestFeature/LongRunning_Info";
const char kMethodIntermediate[] = "/org.test.LongRunningTestFeature/LongRunning_Intermediate";
const char kMethodResult[] = "/org.test.LongRunningTestFeature/LongRunning_Result";

// FeatureRegistry::registerFeature (S46) now checks this FDL's root
// <Feature> identity against kFqi above -- Originator/Category/FeatureVersion
// here must agree with it, not just read for readability.
const char kFdl[] = R"xml(
<Feature FeatureVersion="1.0" Originator="org.test" Category="test">
  <Identifier>LongRunningTestFeature</Identifier>
  <Command>
    <Identifier>LongRunning</Identifier>
    <Observable>Yes</Observable>
    <Parameter>
      <Identifier>Input</Identifier>
      <DataType><Basic>String</Basic></DataType>
    </Parameter>
    <IntermediateResponse>
      <Identifier>Intermediate</Identifier>
      <DataType><Basic>String</Basic></DataType>
    </IntermediateResponse>
    <Response>
      <Identifier>Output</Identifier>
      <DataType><Basic>String</Basic></DataType>
    </Response>
  </Command>
</Feature>
)xml";

// ---------------------------------------------------------------------------
// LongRunningTestService -- server side. One Observable Command ("LongRunning")
// backed by ObservableCommandManager, wired through dispatchToHandler exactly
// like a generated ServiceAdapter would (same reuse test_grpc_dispatch_e2e.cc
// makes of dispatchToHandler/guardHandler), so ObservableCommandManager's
// InvalidCommandExecutionUuid FrameworkError converts to a well-formed
// ABORTED status instead of crashing the process as an uncaught exception.
// ---------------------------------------------------------------------------
class LongRunningTestService final : public grpc::Service {
public:
    explicit LongRunningTestService(ObservableCommandManager& cmdManager)
        : cmdManager_{cmdManager} {
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodInitiate, grpc::internal::RpcMethod::NORMAL_RPC,
            new grpc::internal::RpcMethodHandler<LongRunningTestService, fw::String, fw::CommandConfirmation>(
                [](LongRunningTestService* service, grpc::ServerContext* ctx,
                   const fw::String* req, fw::CommandConfirmation* resp) {
                    return service->handleInitiate(ctx, req, resp);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodInfo, grpc::internal::RpcMethod::SERVER_STREAMING,
            new grpc::internal::ServerStreamingHandler<LongRunningTestService, fw::CommandExecutionUUID, fw::ExecutionInfo>(
                [](LongRunningTestService* service, grpc::ServerContext* ctx,
                   const fw::CommandExecutionUUID* req, grpc::ServerWriter<fw::ExecutionInfo>* writer) {
                    return service->handleInfo(ctx, req, writer);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodIntermediate, grpc::internal::RpcMethod::SERVER_STREAMING,
            new grpc::internal::ServerStreamingHandler<LongRunningTestService, fw::CommandExecutionUUID, fw::String>(
                [](LongRunningTestService* service, grpc::ServerContext* ctx,
                   const fw::CommandExecutionUUID* req, grpc::ServerWriter<fw::String>* writer) {
                    return service->handleIntermediate(ctx, req, writer);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodResult, grpc::internal::RpcMethod::NORMAL_RPC,
            new grpc::internal::RpcMethodHandler<LongRunningTestService, fw::CommandExecutionUUID, fw::String>(
                [](LongRunningTestService* service, grpc::ServerContext* ctx,
                   const fw::CommandExecutionUUID* req, fw::String* resp) {
                    return service->handleResult(ctx, req, resp);
                }, this)));
    }

    // Joins every worker thread spawned by handleInitiate before the service
    // (and the ObservableCommandManager it references) tears down -- a
    // detached thread still sleeping past process/library teardown risked an
    // intermittent "double free or corruption" observed during manual test
    // runs, most likely a race with grpc global cleanup. Same fix
    // GenericDynamicTestServer.h applies to its own handler threads.
    ~LongRunningTestService() override {
        std::lock_guard<std::mutex> lock(threadsMu_);
        for (auto& t : workerThreads_) {
            if (t.joinable()) t.join();
        }
    }

private:
    // Per-execution data the worker thread produces and the _Intermediate/
    // _Result handlers read -- same shape as server_main.cc's EchoBinariesState.
    struct ExecutionState {
        std::mutex mu;
        std::condition_variable cv;
        bool intermediateReady = false;
        std::string intermediateValue;
        bool done = false;
        std::string result;
    };

    grpc::Status handleInitiate(grpc::ServerContext* ctx, const fw::String* req, fw::CommandConfirmation* resp) {
        GrpcUnaryResponseSink<fw::CommandConfirmation> sink(resp);
        SilaHandler<fw::String, fw::CommandConfirmation> handler =
            [this](const fw::String& req, CallContext&, ResponseSink<fw::CommandConfirmation>& s) {
                auto exec = cmdManager_.addCommand(std::chrono::seconds{60});
                auto state = std::make_shared<ExecutionState>();
                {
                    std::lock_guard<std::mutex> lock(statesMu_);
                    states_[exec->uuid()] = state;
                }

                // S68 test hook: the initiate payload "fail" drives this
                // execution to FinishedWithError instead of
                // FinishedSuccessfully, so handleResult's error branch has
                // something to exercise end-to-end.
                const bool shouldFail = (req.value() == "fail");

                // Worker thread: captures exec/state by value (shared_ptr
                // copies), so it stays valid on its own regardless of this
                // RPC's lifetime -- same capture pattern server_main.cc's
                // EchoBinariesObservably worker uses. Tracked and joined by
                // ~LongRunningTestService() rather than detached, so no
                // worker outlives the service/manager it was spawned from.
                // Timings (50/150/150ms) are chosen so Result-before-finish
                // (called with ~0ms delay in tests) and Info's first read
                // (also ~0ms delay) both land deterministically in Waiting.
                std::thread worker([exec, state, shouldFail] {
                    std::this_thread::sleep_for(std::chrono::milliseconds{50});
                    exec->start();
                    std::this_thread::sleep_for(std::chrono::milliseconds{150});
                    {
                        std::lock_guard<std::mutex> lock(state->mu);
                        state->intermediateValue = "intermediate-value";
                        state->intermediateReady = true;
                    }
                    state->cv.notify_all();
                    std::this_thread::sleep_for(std::chrono::milliseconds{150});
                    {
                        std::lock_guard<std::mutex> lock(state->mu);
                        state->result = "final-result";
                        state->done = true;
                    }
                    state->cv.notify_all();
                    if (shouldFail) {
                        exec->fail("worker failed");
                    } else {
                        exec->finish();
                    }
                });
                {
                    std::lock_guard<std::mutex> lock(threadsMu_);
                    workerThreads_.push_back(std::move(worker));
                }

                fw::CommandConfirmation confirmation;
                confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
                s.send(confirmation);
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, nullptr, kFqi, resp);
        return sink.status();
    }

    grpc::Status handleInfo(grpc::ServerContext* ctx, const fw::CommandExecutionUUID* req,
                             grpc::ServerWriter<fw::ExecutionInfo>* writer) {
        GrpcStreamResponseSink<fw::ExecutionInfo> sink(writer);
        SilaHandler<fw::CommandExecutionUUID, fw::ExecutionInfo> handler =
            [this](const fw::CommandExecutionUUID& r, CallContext& c, ResponseSink<fw::ExecutionInfo>& s) {
                // Unknown UUID: ObservableCommandManager raises the standard
                // InvalidCommandExecutionUuid FrameworkError, which
                // dispatchToHandler's guardHandler converts to an ABORTED
                // status below -- no duplicated UUID-checking policy here.
                auto exec = cmdManager_.getCommand(r.value());
                while (true) {
                    const auto execState = exec->state();
                    fw::ExecutionInfo info;
                    switch (execState) {
                    case ObservableCommandExecution::State::Waiting:
                        info.set_commandstatus(fw::ExecutionInfo_CommandStatus_waiting);
                        break;
                    case ObservableCommandExecution::State::Running:
                        info.set_commandstatus(fw::ExecutionInfo_CommandStatus_running);
                        break;
                    case ObservableCommandExecution::State::FinishedSuccessfully:
                        info.set_commandstatus(fw::ExecutionInfo_CommandStatus_finishedSuccessfully);
                        break;
                    case ObservableCommandExecution::State::FinishedWithError:
                        info.set_commandstatus(fw::ExecutionInfo_CommandStatus_finishedWithError);
                        break;
                    }
                    s.send(info);

                    const bool finished =
                        execState == ObservableCommandExecution::State::FinishedSuccessfully ||
                        execState == ObservableCommandExecution::State::FinishedWithError;
                    if (finished || c.isCancelled()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                }
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, nullptr, kFqi);
        return sink.status();
    }

    grpc::Status handleIntermediate(grpc::ServerContext* ctx, const fw::CommandExecutionUUID* req,
                                     grpc::ServerWriter<fw::String>* writer) {
        GrpcStreamResponseSink<fw::String> sink(writer);
        SilaHandler<fw::CommandExecutionUUID, fw::String> handler =
            [this](const fw::CommandExecutionUUID& r, CallContext&, ResponseSink<fw::String>& s) {
                cmdManager_.getCommand(r.value());  // throws FrameworkError if unknown
                std::shared_ptr<ExecutionState> state;
                {
                    std::lock_guard<std::mutex> lock(statesMu_);
                    state = states_.at(r.value());
                }

                std::unique_lock<std::mutex> lock(state->mu);
                state->cv.wait(lock, [&] { return state->intermediateReady || state->done; });
                if (state->intermediateReady) {
                    fw::String value;
                    value.set_value(state->intermediateValue);
                    lock.unlock();
                    s.send(value);
                }
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, nullptr, kFqi);
        return sink.status();
    }

    grpc::Status handleResult(grpc::ServerContext* ctx, const fw::CommandExecutionUUID* req, fw::String* resp) {
        GrpcUnaryResponseSink<fw::String> sink(resp);
        SilaHandler<fw::CommandExecutionUUID, fw::String> handler =
            [this](const fw::CommandExecutionUUID& r, CallContext&, ResponseSink<fw::String>& s) {
                auto exec = cmdManager_.getCommand(r.value());  // throws FrameworkError if unknown
                switch (exec->state()) {
                case ObservableCommandExecution::State::Waiting:
                case ObservableCommandExecution::State::Running:
                    // This test feature's own precondition check, not a
                    // manager-level one: Part A p51 requires a Command
                    // Execution Not Finished Error for a _Result asked
                    // before completion.
                    throw FrameworkError{FrameworkError::FrameworkErrorType::CommandExecutionNotFinished,
                                         "command execution has not finished yet"};
                case ObservableCommandExecution::State::FinishedWithError:
                    // Part A p51 (S68): a failed execution MUST surface its
                    // error, not the (never-populated) result value.
                    throw UndefinedExecutionError{exec->errorMessage()};
                case ObservableCommandExecution::State::FinishedSuccessfully: {
                    std::shared_ptr<ExecutionState> state;
                    {
                        std::lock_guard<std::mutex> lock(statesMu_);
                        state = states_.at(r.value());
                    }
                    fw::String value;
                    {
                        std::lock_guard<std::mutex> lock(state->mu);
                        value.set_value(state->result);
                    }
                    s.send(value);
                    s.finish();
                    break;
                }
                }
            };
        dispatchToHandler(ctx, *req, sink, handler, nullptr, kFqi, resp);
        return sink.status();
    }

    ObservableCommandManager& cmdManager_;
    std::mutex statesMu_;
    std::unordered_map<std::string, std::shared_ptr<ExecutionState>> states_;

    std::mutex threadsMu_;
    std::vector<std::thread> workerThreads_;
};

// ---------------------------------------------------------------------------
// LongRunningTestClient -- client side. Hand-rolled the same way: plain
// grpc::internal::RpcMethod + BlockingUnaryCall/ClientReaderFactory calls,
// the primitives a generated Stub's methods themselves call into (compare
// ErrorRecoveryService.grpc.pb.cc's Stub::Subscribe_RecoverableErrors).
// ---------------------------------------------------------------------------
class LongRunningTestClient {
public:
    explicit LongRunningTestClient(std::shared_ptr<grpc::Channel> channel)
        : channel_{channel},
          initiate_{kMethodInitiate, grpc::internal::RpcMethod::NORMAL_RPC, channel},
          info_{kMethodInfo, grpc::internal::RpcMethod::SERVER_STREAMING, channel},
          intermediate_{kMethodIntermediate, grpc::internal::RpcMethod::SERVER_STREAMING, channel},
          result_{kMethodResult, grpc::internal::RpcMethod::NORMAL_RPC, channel} {}

    grpc::Status initiate(grpc::ClientContext* ctx, const fw::String& req, fw::CommandConfirmation* resp) {
        return grpc::internal::BlockingUnaryCall<fw::String, fw::CommandConfirmation>(
            channel_.get(), initiate_, ctx, req, resp);
    }

    std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>> info(
            grpc::ClientContext* ctx, const fw::CommandExecutionUUID& req) {
        return std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>>(
            grpc::internal::ClientReaderFactory<fw::ExecutionInfo>::Create(channel_.get(), info_, ctx, req));
    }

    std::unique_ptr<grpc::ClientReader<fw::String>> intermediate(
            grpc::ClientContext* ctx, const fw::CommandExecutionUUID& req) {
        return std::unique_ptr<grpc::ClientReader<fw::String>>(
            grpc::internal::ClientReaderFactory<fw::String>::Create(channel_.get(), intermediate_, ctx, req));
    }

    grpc::Status result(grpc::ClientContext* ctx, const fw::CommandExecutionUUID& req, fw::String* resp) {
        return grpc::internal::BlockingUnaryCall<fw::CommandExecutionUUID, fw::String>(
            channel_.get(), result_, ctx, req, resp);
    }

private:
    std::shared_ptr<grpc::Channel> channel_;
    grpc::internal::RpcMethod initiate_;
    grpc::internal::RpcMethod info_;
    grpc::internal::RpcMethod intermediate_;
    grpc::internal::RpcMethod result_;
};

// Boots a real SilaServerBase hosting LongRunningTestService on an ephemeral
// TLS port (withDiscovery(0): the only port control run() exposes, per
// test_sila_server_base_run_shutdown_e2e.cc) and dials it with a real
// grpc::Channel over the server's own self-signed certificate.
struct LongRunningTestServer {
    LongRunningTestServer()
        : service{std::make_shared<LongRunningTestService>(cmdManager)},
          server{SilaServerBase::Builder()
                     .withSelfSignedCertificate("localhost", "127.0.0.1")
                     .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                     .withDiscovery(0)
                     .addFeature(kFqi, kFdl, service)
                     .registerCommandManager(&cmdManager)
                     .build()} {
        server.run(false);

        grpc::SslCredentialsOptions opts;
        opts.pem_root_certs = server.certificatePem();
        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(server.port()), grpc::SslCredentials(opts));
        client = std::make_unique<LongRunningTestClient>(channel);
    }

    ~LongRunningTestServer() { server.shutdown(); }

    ObservableCommandManager cmdManager;
    std::shared_ptr<LongRunningTestService> service;
    SilaServerBase server;
    std::unique_ptr<LongRunningTestClient> client;
};

// Initiates a command and returns its UUID; asserts the call succeeded.
// `payload` defaults to "unused" so existing callers are unaffected; passing
// "fail" drives the worker to FinishedWithError (see handleInitiate's
// shouldFail hook), which the S68 rejection test below needs.
std::string initiateAndGetUuid(LongRunningTestServer& server, const std::string& payload = "unused") {
    grpc::ClientContext ctx;
    fw::String req;
    req.set_value(payload);
    fw::CommandConfirmation resp;
    const grpc::Status status = server.client->initiate(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return resp.commandexecutionuuid().value();
}

}  // namespace

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(ObservableCommandGrpcE2E, InitiateReturnsCommandConfirmationWithUuid) {
    LongRunningTestServer server;

    const std::string uuid = initiateAndGetUuid(server);

    EXPECT_FALSE(uuid.empty());
}

TEST(ObservableCommandGrpcE2E, InitiateTwiceReturnsTwoIndependentUuids) {
    LongRunningTestServer server;

    const std::string first = initiateAndGetUuid(server);
    const std::string second = initiateAndGetUuid(server);

    EXPECT_NE(first, second);

    // Both are independently resolvable via _Info -- proves the manager
    // tracks them as two distinct executions, not just two distinct strings.
    grpc::ClientContext ctxFirst;
    fw::CommandExecutionUUID reqFirst;
    reqFirst.set_value(first);
    auto readerFirst = server.client->info(&ctxFirst, reqFirst);
    fw::ExecutionInfo infoFirst;
    ASSERT_TRUE(readerFirst->Read(&infoFirst));

    grpc::ClientContext ctxSecond;
    fw::CommandExecutionUUID reqSecond;
    reqSecond.set_value(second);
    auto readerSecond = server.client->info(&ctxSecond, reqSecond);
    fw::ExecutionInfo infoSecond;
    ASSERT_TRUE(readerSecond->Read(&infoSecond));

    ctxFirst.TryCancel();
    ctxSecond.TryCancel();
    readerFirst->Finish();
    readerSecond->Finish();
}

TEST(ObservableCommandGrpcE2E, SubscribeToExecutionInfoObservesNonTerminalStatusThenFinishes) {
    LongRunningTestServer server;
    const std::string uuid = initiateAndGetUuid(server);

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(uuid);
    auto reader = server.client->info(&ctx, req);

    fw::ExecutionInfo info;
    ASSERT_TRUE(reader->Read(&info));
    EXPECT_TRUE(info.commandstatus() == fw::ExecutionInfo_CommandStatus_waiting ||
                info.commandstatus() == fw::ExecutionInfo_CommandStatus_running)
        << "first status observed: " << info.commandstatus();

    fw::ExecutionInfo::CommandStatus last = info.commandstatus();
    while (reader->Read(&info)) {
        last = info.commandstatus();
    }
    const grpc::Status status = reader->Finish();

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(last, fw::ExecutionInfo_CommandStatus_finishedSuccessfully);
}

TEST(ObservableCommandGrpcE2E, IntermediateWithValidUuidReturnsWorkerProducedValue) {
    LongRunningTestServer server;
    const std::string uuid = initiateAndGetUuid(server);

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(uuid);
    auto reader = server.client->intermediate(&ctx, req);

    fw::String value;
    ASSERT_TRUE(reader->Read(&value));
    EXPECT_EQ(value.value(), "intermediate-value");
    const grpc::Status status = reader->Finish();
    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(ObservableCommandGrpcE2E, ResultWithValidUuidAfterFinishReturnsFinalValue) {
    LongRunningTestServer server;
    const std::string uuid = initiateAndGetUuid(server);

    // Drain _Info to completion so the command is guaranteed finished before
    // _Result is called.
    grpc::ClientContext infoCtx;
    fw::CommandExecutionUUID infoReq;
    infoReq.set_value(uuid);
    auto infoReader = server.client->info(&infoCtx, infoReq);
    fw::ExecutionInfo info;
    while (infoReader->Read(&info)) {}
    ASSERT_TRUE(infoReader->Finish().ok());

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(uuid);
    fw::String resp;
    const grpc::Status status = server.client->result(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.value(), "final-result");
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths -- all four CAUGHT.
// The first three by ObservableCommandManager::getCommand (shared across all
// three UUID-taking RPCs, matching server_main.cc's "let the manager raise
// the standard error" comment); the fourth by this test feature's own
// precondition check in handleResult.
// ---------------------------------------------------------------------------

TEST(ObservableCommandGrpcE2E, InfoWithUnknownUuidReturnsAbortedInvalidCommandExecutionUuid) {
    LongRunningTestServer server;

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value("does-not-exist");
    auto reader = server.client->info(&ctx, req);

    fw::ExecutionInfo info;
    EXPECT_FALSE(reader->Read(&info));
    const grpc::Status status = reader->Finish();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid);
}

TEST(ObservableCommandGrpcE2E, IntermediateWithUnknownUuidReturnsAbortedInvalidCommandExecutionUuid) {
    LongRunningTestServer server;

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value("does-not-exist");
    auto reader = server.client->intermediate(&ctx, req);

    fw::String value;
    EXPECT_FALSE(reader->Read(&value));
    const grpc::Status status = reader->Finish();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid);
}

TEST(ObservableCommandGrpcE2E, ResultWithUnknownUuidReturnsAbortedInvalidCommandExecutionUuid) {
    LongRunningTestServer server;

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value("does-not-exist");
    fw::String resp;
    const grpc::Status status = server.client->result(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid);
}

TEST(ObservableCommandGrpcE2E, ResultBeforeFinishReturnsAbortedCommandExecutionNotFinished) {
    LongRunningTestServer server;
    const std::string uuid = initiateAndGetUuid(server);

    // Called immediately after Initiate: the worker's first state
    // transition (Waiting -> Running) is delayed 50ms, so this reaches
    // handleResult while still Waiting.
    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(uuid);
    fw::String resp;
    const grpc::Status status = server.client->result(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::CommandExecutionNotFinished);
}

TEST(ObservableCommandGrpcE2E, ResultAfterFinishWithErrorReturnsExecutionError) {
    LongRunningTestServer server;
    const std::string uuid = initiateAndGetUuid(server, "fail");

    // Drain _Info to completion so the command is guaranteed
    // FinishedWithError before _Result is called.
    grpc::ClientContext infoCtx;
    fw::CommandExecutionUUID infoReq;
    infoReq.set_value(uuid);
    auto infoReader = server.client->info(&infoCtx, infoReq);
    fw::ExecutionInfo info;
    fw::ExecutionInfo::CommandStatus last = fw::ExecutionInfo_CommandStatus_waiting;
    while (infoReader->Read(&info)) {
        last = info.commandstatus();
    }
    ASSERT_TRUE(infoReader->Finish().ok());
    ASSERT_EQ(last, fw::ExecutionInfo_CommandStatus_finishedWithError);

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(uuid);
    fw::String resp;
    const grpc::Status status = server.client->result(&ctx, req, &resp);

    // Part A p51 (S68): a failed execution's _Result MUST return the
    // error, not the (never-populated) "final-result" value.
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* err = dynamic_cast<const ExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
}
