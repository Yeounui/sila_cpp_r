// test_command_granular_auth_e2e.cc — Proves the direct-gRPC access-token
// gate (FqiMatch.h §3.1s, architecture.md §3.11) enforces protectedFqis at
// COMMAND granularity: a Command-level protectedFqis entry gates exactly that
// command's RPC, and neither leaks to nor is satisfied by a sibling command
// in the same feature. Mirrors test_grpc_transport_auth_e2e.cc's
// TestServer/dispatchToHandler scaffold, but hosts BOTH LockController RPCs
// (LockServer, UnlockServer) on one service so a single test server can
// exercise cross-command scoping in-process.
#include <sila/server/transport/GrpcTransport.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/transport/InterceptorChain.h>

#include "LockController.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace
{
using sila2::CallContext;
using sila2::GrpcUnaryResponseSink;
using sila2::InterceptorChain;
using sila2::ResponseSink;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;
using sila2::error::DefinedExecutionError;
using sila2::error::FrameworkError;
using sila2::error::fromGrpcStatus;
using sila2::error::SiLAError;
using namespace std::chrono_literals;

namespace lockcontroller_proto = sila2::org::silastandard::core::lockcontroller::v1;
using lockcontroller_proto::LockController;
using lockcontroller_proto::LockServer_Parameters;
using lockcontroller_proto::LockServer_Responses;
using lockcontroller_proto::UnlockServer_Parameters;
using lockcontroller_proto::UnlockServer_Responses;

const std::string kFeatureFqi = "org.silastandard/core/LockController/v1";
const std::string kLockServerFqi = kFeatureFqi + "/Command/LockServer";
const std::string kUnlockServerFqi = kFeatureFqi + "/Command/UnlockServer";
const std::string kInvalidAccessTokenErrorId =
    "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken";

// Only LockServer is listed as protected. anyFqiCovers, not ==, matches
// production SiLAServerBase (SiLAServerBase.cc:636): an operator's
// protectedFqis vector may list a command FQI or a feature FQI, and either
// must gate LockServer -- but neither should ever gate UnlockServer, since
// UnlockServer's FQI shares no prefix relationship with a bare LockServer
// entry.
bool isProtectedLockServerOnly(const std::string& fqi) {
    static const std::vector<std::string> protectedFqis{kLockServerFqi};
    return sila2::auth::anyFqiCovers(protectedFqis, fqi);
}

// Both LockServer and UnlockServer are hosted on one grpc::Service, each
// dispatched through dispatchToHandler with its OWN command-granular fqi --
// exactly as codegen's service_adapter.h.j2 wires each RPC method
// independently. Neither handler touches lock state; only the gate matters.
class TestLockControllerService final : public LockController::Service {
public:
    explicit TestLockControllerService(const InterceptorChain* chain) : chain_{chain} {}

    grpc::Status LockServer(grpc::ServerContext* ctx,
                             const LockServer_Parameters* req,
                             LockServer_Responses* resp) override {
        GrpcUnaryResponseSink<LockServer_Responses> sink(resp);
        sila2::SilaHandler<LockServer_Parameters, LockServer_Responses> handler =
            [](const LockServer_Parameters&, CallContext&, ResponseSink<LockServer_Responses>& s) {
                LockServer_Responses r;
                s.send(r);
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, chain_, kLockServerFqi, resp);
        return sink.status();
    }

    grpc::Status UnlockServer(grpc::ServerContext* ctx,
                               const UnlockServer_Parameters* req,
                               UnlockServer_Responses* resp) override {
        GrpcUnaryResponseSink<UnlockServer_Responses> sink(resp);
        sila2::SilaHandler<UnlockServer_Parameters, UnlockServer_Responses> handler =
            [](const UnlockServer_Parameters&, CallContext&, ResponseSink<UnlockServer_Responses>& s) {
                UnlockServer_Responses r;
                s.send(r);
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, chain_, kUnlockServerFqi, resp);
        return sink.status();
    }

private:
    const InterceptorChain* chain_;
};

// Boots a real gRPC server hosting both commands on an ephemeral loopback
// port and connects a stub to it. RAII shuts the server down on destruction.
struct TestServer {
    explicit TestServer(const InterceptorChain* chain) : service{chain} {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        channel = grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub = LockController::NewStub(channel);
    }

    ~TestServer() {
        if (server) server->Shutdown();
    }

    grpc::Status callLockServer(const std::string* accessToken = nullptr) {
        grpc::ClientContext ctx;
        if (accessToken) {
            ctx.AddMetadata("access-token", *accessToken);
        }
        LockServer_Parameters req;
        LockServer_Responses resp;
        return stub->LockServer(&ctx, req, &resp);
    }

    grpc::Status callUnlockServer(const std::string* accessToken = nullptr) {
        grpc::ClientContext ctx;
        if (accessToken) {
            ctx.AddMetadata("access-token", *accessToken);
        }
        UnlockServer_Parameters req;
        UnlockServer_Responses resp;
        return stub->UnlockServer(&ctx, req, &resp);
    }

    TestLockControllerService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<LockController::Stub> stub;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(CommandGranularAuthE2E, LockServerWithOwnCommandTokenSucceeds) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtectedLockServerOnly};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain};
    const std::string token = store.issue("alice", {kLockServerFqi}, 60s);

    const grpc::Status status = server.callLockServer(&token);

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(CommandGranularAuthE2E, UnlockServerWithoutTokenSucceedsSiblingNotProtected) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtectedLockServerOnly};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain};

    const grpc::Status status = server.callUnlockServer();  // no access-token metadata at all

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(CommandGranularAuthE2E, LockServerWithFeatureLevelTokenSucceeds) {
    // A token issued for the bare feature FQI still covers LockServer via
    // fqiCovers's prefix-on-segment-boundary rule -- feature-level
    // protectedFqis entries must keep closing every command under them.
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtectedLockServerOnly};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain};
    const std::string token = store.issue("alice", {kFeatureFqi}, 60s);

    const grpc::Status status = server.callLockServer(&token);

    EXPECT_TRUE(status.ok()) << status.error_message();
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths
// All CAUGHT: AuthorizationInterceptor detects and rejects them, and
// dispatchToHandler's guardHandler+sink.fail() round-trips the rejection
// through grpc::Status without losing its type or identifier.
// ---------------------------------------------------------------------------

TEST(CommandGranularAuthE2E, LockServerWithoutTokenReturnsAbortedWithInvalidMetadata) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtectedLockServerOnly};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain};

    const grpc::Status status = server.callLockServer();  // no access-token metadata at all

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::FrameworkError);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
}

TEST(CommandGranularAuthE2E, LockServerWithBogusTokenReturnsInvalidAccessToken) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtectedLockServerOnly};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain};
    const std::string bogusToken = "bogus_token_12345";

    const grpc::Status status = server.callLockServer(&bogusToken);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidAccessTokenErrorId);
}

TEST(CommandGranularAuthE2E, LockServerWithSiblingCommandTokenReturnsInvalidAccessToken) {
    // THE command-granularity proof: a token scoped only to the sibling
    // command UnlockServer must NOT cover LockServer. If protectedFqis
    // enforcement were feature-granular (or the gate matched loosely), this
    // token would wrongly authorize LockServer too.
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtectedLockServerOnly};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain};
    const std::string siblingToken = store.issue("alice", {kUnlockServerFqi}, 60s);

    const grpc::Status status = server.callLockServer(&siblingToken);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidAccessTokenErrorId);
}

}  // namespace
