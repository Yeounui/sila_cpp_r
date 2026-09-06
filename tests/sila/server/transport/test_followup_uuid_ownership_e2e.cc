// test_followup_uuid_ownership_e2e.cc — Proves the Observable Command
// follow-up owner gate (GrpcTransport.h dispatchToHandler, "Batch C, High-2")
// rejects a CommandExecutionUUID whose registered owner differs from the
// follow-up RPC's own command FQI, over a real gRPC connection.
//
// Mirrors test_command_granular_auth_e2e.cc's scaffold (a hand-rolled
// grpc::Service on a plain insecure grpc::ServerBuilder -- no
// SiLAServerBase/TLS/FeatureRegistry needed, since only dispatchToHandler's
// owner-check branch is under test) crossed with
// test_observable_command_grpc_e2e.cc's hand-rolled Observable Command RPCs
// (grpc::internal::RpcServiceMethod machinery, since no FDL-codegen'd
// feature in this tree has an Observable Command yet). TWO commands (A, B)
// share ONE ObservableCommandManager and ONE real InterceptorChain (default
// -constructed, so auth stays null and no token is needed to exercise the
// owner gate) -- exactly the setup dispatchToHandler needs to both register
// an owner on Initiate and check it on each follow-up.
#include <sila/server/transport/GrpcTransport.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/InterceptorChain.h>

#include <SiLAFramework.pb.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/client_unary_call.h>
#include <grpcpp/impl/proto_utils.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/support/sync_stream.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using sila2::CallContext;
using sila2::GrpcStreamResponseSink;
using sila2::GrpcUnaryResponseSink;
using sila2::InterceptorChain;
using sila2::ObservableCommandManager;
using sila2::ResponseSink;
using sila2::SilaHandler;
using sila2::error::fromGrpcStatus;
using sila2::error::FrameworkError;

namespace fw = sila2::org::silastandard;

// One Feature, two sibling Commands -- the per-RPC FQI a generated adapter
// would pass is Command-granular (GrpcTransport.h:185), so A and B's owner
// records differ only in this suffix.
const std::string kFeatureFqi = "org.test/test/TwoCmd/v1";
const std::string kFqiA = kFeatureFqi + "/Command/A";
const std::string kFqiB = kFeatureFqi + "/Command/B";

const char kMethodInitiateA[] = "/org.test.TwoCmd/InitiateA";
const char kMethodInfoA[] = "/org.test.TwoCmd/InfoA";
const char kMethodResultA[] = "/org.test.TwoCmd/ResultA";
const char kMethodInitiateB[] = "/org.test.TwoCmd/InitiateB";
const char kMethodInfoB[] = "/org.test.TwoCmd/InfoB";
const char kMethodResultB[] = "/org.test.TwoCmd/ResultB";

// ---------------------------------------------------------------------------
// TwoCmdTestService -- server side. Two Observable Commands (A, B), each
// exposing Initiate (-> CommandConfirmation) plus _Info and _Result
// follow-ups (both take a bare CommandExecutionUUID). Every RPC is wired
// through dispatchToHandler with `chain_` (never nullptr, unlike
// test_observable_command_grpc_e2e.cc) so the owner-register/owner-check
// branches actually fire. No worker thread: each command finishes
// synchronously inside Initiate, since only the owner gate -- not the state
// machine -- is under test here, and _Result's own "not finished yet"
// precondition would otherwise be a second, irrelevant failure mode for the
// positive _Result case.
// ---------------------------------------------------------------------------
class TwoCmdTestService final : public grpc::Service {
public:
    TwoCmdTestService(ObservableCommandManager& cmdManager, const InterceptorChain* chain)
        : cmdManager_{cmdManager}, chain_{chain} {
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodInitiateA, grpc::internal::RpcMethod::NORMAL_RPC,
            new grpc::internal::RpcMethodHandler<TwoCmdTestService, fw::String, fw::CommandConfirmation>(
                [](TwoCmdTestService* service, grpc::ServerContext* ctx,
                   const fw::String* req, fw::CommandConfirmation* resp) {
                    return service->handleInitiate(ctx, req, resp, kFqiA);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodInitiateB, grpc::internal::RpcMethod::NORMAL_RPC,
            new grpc::internal::RpcMethodHandler<TwoCmdTestService, fw::String, fw::CommandConfirmation>(
                [](TwoCmdTestService* service, grpc::ServerContext* ctx,
                   const fw::String* req, fw::CommandConfirmation* resp) {
                    return service->handleInitiate(ctx, req, resp, kFqiB);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodInfoA, grpc::internal::RpcMethod::SERVER_STREAMING,
            new grpc::internal::ServerStreamingHandler<TwoCmdTestService, fw::CommandExecutionUUID, fw::ExecutionInfo>(
                [](TwoCmdTestService* service, grpc::ServerContext* ctx,
                   const fw::CommandExecutionUUID* req, grpc::ServerWriter<fw::ExecutionInfo>* writer) {
                    return service->handleInfo(ctx, req, writer, kFqiA);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodInfoB, grpc::internal::RpcMethod::SERVER_STREAMING,
            new grpc::internal::ServerStreamingHandler<TwoCmdTestService, fw::CommandExecutionUUID, fw::ExecutionInfo>(
                [](TwoCmdTestService* service, grpc::ServerContext* ctx,
                   const fw::CommandExecutionUUID* req, grpc::ServerWriter<fw::ExecutionInfo>* writer) {
                    return service->handleInfo(ctx, req, writer, kFqiB);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodResultA, grpc::internal::RpcMethod::NORMAL_RPC,
            new grpc::internal::RpcMethodHandler<TwoCmdTestService, fw::CommandExecutionUUID, fw::String>(
                [](TwoCmdTestService* service, grpc::ServerContext* ctx,
                   const fw::CommandExecutionUUID* req, fw::String* resp) {
                    return service->handleResult(ctx, req, resp, kFqiA);
                }, this)));
        AddMethod(new grpc::internal::RpcServiceMethod(
            kMethodResultB, grpc::internal::RpcMethod::NORMAL_RPC,
            new grpc::internal::RpcMethodHandler<TwoCmdTestService, fw::CommandExecutionUUID, fw::String>(
                [](TwoCmdTestService* service, grpc::ServerContext* ctx,
                   const fw::CommandExecutionUUID* req, fw::String* resp) {
                    return service->handleResult(ctx, req, resp, kFqiB);
                }, this)));
    }

private:
    grpc::Status handleInitiate(grpc::ServerContext* ctx, const fw::String* req,
                                 fw::CommandConfirmation* resp, const std::string& fqi) {
        GrpcUnaryResponseSink<fw::CommandConfirmation> sink(resp);
        SilaHandler<fw::String, fw::CommandConfirmation> handler =
            [this](const fw::String&, CallContext&, ResponseSink<fw::CommandConfirmation>& s) {
                // Finished synchronously (no worker thread): the only thing
                // under test is the owner gate, so the command must already
                // be resolvable and finished by the time a follow-up RPC
                // runs, with no timing dependency.
                auto exec = cmdManager_.addCommand();
                exec->start();
                exec->finish();

                fw::CommandConfirmation confirmation;
                confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
                s.send(confirmation);
                s.finish();
            };
        // dispatchToHandler registers this UUID's owner as `fqi` (GrpcTransport.h:280-286)
        // because Resp is CommandConfirmation and chain_ + resp are both non-null.
        dispatchToHandler(ctx, *req, sink, handler, chain_, fqi, resp);
        return sink.status();
    }

    grpc::Status handleInfo(grpc::ServerContext* ctx, const fw::CommandExecutionUUID* req,
                             grpc::ServerWriter<fw::ExecutionInfo>* writer, const std::string& fqi) {
        GrpcStreamResponseSink<fw::ExecutionInfo> sink(writer);
        SilaHandler<fw::CommandExecutionUUID, fw::ExecutionInfo> handler =
            [this](const fw::CommandExecutionUUID& r, CallContext&, ResponseSink<fw::ExecutionInfo>& s) {
                // Reached only when the owner gate let the call through, so
                // the UUID is always known and already finished here.
                cmdManager_.getCommand(r.value());
                fw::ExecutionInfo info;
                info.set_commandstatus(fw::ExecutionInfo_CommandStatus_finishedSuccessfully);
                s.send(info);
                s.finish();
            };
        // dispatchToHandler checks the owner BEFORE `handler` runs
        // (GrpcTransport.h:250-258): a cross-owner UUID never reaches the
        // getCommand() call above at all.
        dispatchToHandler(ctx, *req, sink, handler, chain_, fqi);
        return sink.status();
    }

    grpc::Status handleResult(grpc::ServerContext* ctx, const fw::CommandExecutionUUID* req,
                               fw::String* resp, const std::string& fqi) {
        GrpcUnaryResponseSink<fw::String> sink(resp);
        SilaHandler<fw::CommandExecutionUUID, fw::String> handler =
            [this](const fw::CommandExecutionUUID& r, CallContext&, ResponseSink<fw::String>& s) {
                cmdManager_.getCommand(r.value());
                fw::String value;
                value.set_value("result-for-" + r.value());
                s.send(value);
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, chain_, fqi, resp);
        return sink.status();
    }

    ObservableCommandManager& cmdManager_;
    const InterceptorChain* chain_;
};

// ---------------------------------------------------------------------------
// TwoCmdTestClient -- client side, hand-rolled from the same
// RpcMethod/BlockingUnaryCall/ClientReaderFactory primitives a generated
// Stub uses (compare test_observable_command_grpc_e2e.cc's
// LongRunningTestClient), since no generated Stub exists for this
// throwaway two-command service.
// ---------------------------------------------------------------------------
class TwoCmdTestClient {
public:
    explicit TwoCmdTestClient(std::shared_ptr<grpc::Channel> channel)
        : channel_{channel},
          initiateA_{kMethodInitiateA, grpc::internal::RpcMethod::NORMAL_RPC, channel},
          initiateB_{kMethodInitiateB, grpc::internal::RpcMethod::NORMAL_RPC, channel},
          infoA_{kMethodInfoA, grpc::internal::RpcMethod::SERVER_STREAMING, channel},
          infoB_{kMethodInfoB, grpc::internal::RpcMethod::SERVER_STREAMING, channel},
          resultA_{kMethodResultA, grpc::internal::RpcMethod::NORMAL_RPC, channel},
          resultB_{kMethodResultB, grpc::internal::RpcMethod::NORMAL_RPC, channel} {}

    grpc::Status initiateA(grpc::ClientContext* ctx, fw::CommandConfirmation* resp) {
        fw::String req;
        req.set_value("unused");
        return grpc::internal::BlockingUnaryCall<fw::String, fw::CommandConfirmation>(
            channel_.get(), initiateA_, ctx, req, resp);
    }

    grpc::Status initiateB(grpc::ClientContext* ctx, fw::CommandConfirmation* resp) {
        fw::String req;
        req.set_value("unused");
        return grpc::internal::BlockingUnaryCall<fw::String, fw::CommandConfirmation>(
            channel_.get(), initiateB_, ctx, req, resp);
    }

    std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>> infoA(
            grpc::ClientContext* ctx, const fw::CommandExecutionUUID& req) {
        return std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>>(
            grpc::internal::ClientReaderFactory<fw::ExecutionInfo>::Create(channel_.get(), infoA_, ctx, req));
    }

    std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>> infoB(
            grpc::ClientContext* ctx, const fw::CommandExecutionUUID& req) {
        return std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>>(
            grpc::internal::ClientReaderFactory<fw::ExecutionInfo>::Create(channel_.get(), infoB_, ctx, req));
    }

    grpc::Status resultA(grpc::ClientContext* ctx, const fw::CommandExecutionUUID& req, fw::String* resp) {
        return grpc::internal::BlockingUnaryCall<fw::CommandExecutionUUID, fw::String>(
            channel_.get(), resultA_, ctx, req, resp);
    }

    grpc::Status resultB(grpc::ClientContext* ctx, const fw::CommandExecutionUUID& req, fw::String* resp) {
        return grpc::internal::BlockingUnaryCall<fw::CommandExecutionUUID, fw::String>(
            channel_.get(), resultB_, ctx, req, resp);
    }

private:
    std::shared_ptr<grpc::Channel> channel_;
    grpc::internal::RpcMethod initiateA_;
    grpc::internal::RpcMethod initiateB_;
    grpc::internal::RpcMethod infoA_;
    grpc::internal::RpcMethod infoB_;
    grpc::internal::RpcMethod resultA_;
    grpc::internal::RpcMethod resultB_;
};

// Boots a plain insecure gRPC server on an ephemeral loopback port -- no
// SiLAServerBase/TLS/FeatureRegistry, matching test_command_granular_auth_e2e.cc:
// only dispatchToHandler's owner-check branch is under test, and that branch
// runs the same way regardless of transport security or feature registration.
struct TwoCmdTestServer {
    explicit TwoCmdTestServer() : service{cmdManager, &chain} {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        channel = grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        client = std::make_unique<TwoCmdTestClient>(channel);
    }

    ~TwoCmdTestServer() {
        if (server) server->Shutdown();
    }

    ObservableCommandManager cmdManager;
    InterceptorChain chain;  // default-constructed: auth == nullptr, empty owner registry
    TwoCmdTestService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<TwoCmdTestClient> client;
};

// Reconstructs the SiLAError from a rejected gRPC status and asserts it is
// FrameworkError{InvalidCommandExecutionUuid} -- the owner gate's rejection
// (GrpcTransport.h:253-256), same error type the pre-existing unknown-UUID
// path already produces, but reached here via a MANAGER-KNOWN UUID whose
// registered owner simply isn't the calling command.
void expectRejectedByOwnerGate(const grpc::Status& status) {
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid);
}

}  // namespace

// ---------------------------------------------------------------------------
// True (positive) paths -- owner matches the invoking command, call proceeds.
// ---------------------------------------------------------------------------

TEST(FollowupUuidOwnershipE2E, InfoAWithOwnUuidSucceeds) {
    TwoCmdTestServer server;
    grpc::ClientContext initCtx;
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(server.client->initiateA(&initCtx, &confirmation).ok());

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(confirmation.commandexecutionuuid().value());
    auto reader = server.client->infoA(&ctx, req);
    fw::ExecutionInfo info;
    ASSERT_TRUE(reader->Read(&info));
    const grpc::Status status = reader->Finish();

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(FollowupUuidOwnershipE2E, InfoBWithOwnUuidSucceeds) {
    TwoCmdTestServer server;
    grpc::ClientContext initCtx;
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(server.client->initiateB(&initCtx, &confirmation).ok());

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(confirmation.commandexecutionuuid().value());
    auto reader = server.client->infoB(&ctx, req);
    fw::ExecutionInfo info;
    ASSERT_TRUE(reader->Read(&info));
    const grpc::Status status = reader->Finish();

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(FollowupUuidOwnershipE2E, ResultAWithOwnUuidSucceeds) {
    // A different follow-up kind than _Info, same owner gate -- proves the
    // gate is not specific to one RPC shape.
    TwoCmdTestServer server;
    grpc::ClientContext initCtx;
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(server.client->initiateA(&initCtx, &confirmation).ok());

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(confirmation.commandexecutionuuid().value());
    fw::String resp;
    const grpc::Status status = server.client->resultA(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.value(), "result-for-" + req.value());
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths -- all CAUGHT by the owner gate, not by
// ObservableCommandManager's unknown-UUID path. The first three below use a
// UUID that IS known to the shared cmdManager (getCommand would happily
// resolve it), so the rejection proves the OWNER check, not mere UUID
// validity. The fourth is the fail-closed case: a UUID the gate has never
// seen at all.
// ---------------------------------------------------------------------------

TEST(FollowupUuidOwnershipE2E, InfoAWithBUuidRejectedByOwnerGate) {
    // THE headline cross-UUID case: B's UUID, correctly resolvable by
    // cmdManager, presented to A's _Info follow-up.
    TwoCmdTestServer server;
    grpc::ClientContext initCtx;
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(server.client->initiateB(&initCtx, &confirmation).ok());

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(confirmation.commandexecutionuuid().value());
    auto reader = server.client->infoA(&ctx, req);
    fw::ExecutionInfo info;
    EXPECT_FALSE(reader->Read(&info));
    const grpc::Status status = reader->Finish();

    expectRejectedByOwnerGate(status);
}

TEST(FollowupUuidOwnershipE2E, InfoBWithAUuidRejectedByOwnerGate) {
    // Symmetric: A's UUID presented to B's _Info follow-up.
    TwoCmdTestServer server;
    grpc::ClientContext initCtx;
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(server.client->initiateA(&initCtx, &confirmation).ok());

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(confirmation.commandexecutionuuid().value());
    auto reader = server.client->infoB(&ctx, req);
    fw::ExecutionInfo info;
    EXPECT_FALSE(reader->Read(&info));
    const grpc::Status status = reader->Finish();

    expectRejectedByOwnerGate(status);
}

TEST(FollowupUuidOwnershipE2E, ResultAWithBUuidRejectedByOwnerGate) {
    // Same gate, a different follow-up kind (_Result instead of _Info) --
    // proves the check is not coincidentally tied to one RPC's plumbing.
    TwoCmdTestServer server;
    grpc::ClientContext initCtx;
    fw::CommandConfirmation confirmation;
    ASSERT_TRUE(server.client->initiateB(&initCtx, &confirmation).ok());

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value(confirmation.commandexecutionuuid().value());
    fw::String resp;
    const grpc::Status status = server.client->resultA(&ctx, req, &resp);

    expectRejectedByOwnerGate(status);
}

TEST(FollowupUuidOwnershipE2E, InfoAWithNeverRegisteredUuidRejectedByOwnerGateFailClosed) {
    // Fail-closed case: this UUID was never returned by any Initiate call on
    // this server, so chain.observableOwnerEntry() resolves nullopt -- the gate
    // now rejects on nullopt too (GrpcTransport.h `if (!entry || ...)`),
    // never reaching cmdManager_.getCommand() at all. This matters because
    // ObservableCommandManager is shared with the cloud transport
    // (CloudEnvelopeRouter): without failing closed, an unregistered-here
    // UUID could be a legitimate cloud-origin execution the gRPC gate would
    // otherwise let a direct-gRPC caller observe with no ownership check.
    TwoCmdTestServer server;

    grpc::ClientContext ctx;
    fw::CommandExecutionUUID req;
    req.set_value("never-registered-uuid");
    auto reader = server.client->infoA(&ctx, req);
    fw::ExecutionInfo info;
    EXPECT_FALSE(reader->Read(&info));
    const grpc::Status status = reader->Finish();

    expectRejectedByOwnerGate(status);
}
