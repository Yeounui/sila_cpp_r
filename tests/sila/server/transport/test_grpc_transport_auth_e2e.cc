// test_grpc_transport_auth_e2e.cc — End-to-end tests for dispatchToHandler's
// auth path (architecture.md §3.8, §3.11). A real gRPC unary call reaches
// dispatchToHandler, which runs AuthorizationInterceptor::intercept before
// the handler; a rejection throws a SilaError that guardHandler routes to
// sink.fail(), and GrpcUnaryResponseSink converts it via SilaError::toStatus()
// into a gRPC ABORTED grpc::Status carrying the serialized SiLA error in the
// binary details field. These tests drive that whole path over a real socket
// with a real grpc::Stub and decode the returned Status back into a SilaError
// via fromGrpcStatus — the wire-level conversion no existing test exercises.
// AuthorizationInterceptor's own unit tests (test_authorization_interceptor.cc)
// stop at the thrown C++ exception, one layer before dispatchToHandler and the
// gRPC transport.
//
// TestProtectedFeatureService below reuses LockController's generated
// grpc::Service/messages purely as wire plumbing (an existing, already-linked
// unary RPC) — it has nothing to do with lock semantics; it is the minimal
// RPC surface needed to drive dispatchToHandler over a socket.
#include <sila/server/transport/GrpcTransport.h>

#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/binary/BinaryUploadService.h>
#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/server/transport/InterceptorChain.h>

#include "LockController.grpc.pb.h"
#include <SiLABinaryTransfer.grpc.pb.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>

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
using sila2::error::SilaError;
using sila2::BinaryUploadService;
using sila2::InMemoryBinaryStore;
using sila2::kBinaryUploadFqi;
using namespace std::chrono_literals;

namespace lockcontroller_proto = sila2::org::silastandard::core::lockcontroller::v1;
using lockcontroller_proto::LockController;
using lockcontroller_proto::LockServer_Parameters;
using lockcontroller_proto::LockServer_Responses;
namespace bt = sila2::org::silastandard;

const std::string kProtectedFqi = "org.test/ProtectedFeature/v1/Command/DoThing";
const std::string kUnprotectedFqi = "org.test/OpenFeature/v1/Command/DoThing";
const std::string kInvalidAccessTokenErrorId =
    "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken";

// CreateBinary's dispatchToHandler call (BinaryUploadService.cc) uses the
// request's parameterIdentifier as the fqi handed to AuthorizationInterceptor
// — the same field CloudEnvelopeRouter's kCreateBinaryUploadRequest gate
// authorizes (SiLACloudConnector.proto:133-137, the only binary envelope with
// a metadata field). This constant plays the role kProtectedFqi plays above,
// just for the binary-upload flow.
const std::string kProtectedBinaryParam = "protectedBinaryParam";

bool isProtectedBinaryParam(const std::string& parameterIdentifier) {
    return parameterIdentifier == kProtectedBinaryParam;
}

bool isProtected(const std::string& fqi) {
    return fqi == kProtectedFqi;
}

// S14 option B: UploadChunk and DeleteBinary gate on this feature-level FQI
// (BinaryUploadService.h) instead of a parameterIdentifier, since neither
// carries one.
bool isProtectedBinaryUploadFqi(const std::string& fqi) {
    return fqi == std::string{kBinaryUploadFqi};
}

// Minimal grpc::Service wired through dispatchToHandler exactly like a
// codegen-generated ServiceAdapter (service_adapter.h.j2) does. The LockServer
// RPC is repurposed here as "the one RPC under test" — the handler below
// never touches lock state.
class TestProtectedFeatureService final : public LockController::Service {
public:
    TestProtectedFeatureService(const InterceptorChain* chain, std::string fqi)
        : chain_{chain}, fqi_{std::move(fqi)} {}

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
        dispatchToHandler(ctx, *req, sink, handler, chain_, fqi_, resp);
        return sink.status();
    }

private:
    const InterceptorChain* chain_;
    std::string fqi_;
};

// Boots a real gRPC server hosting TestProtectedFeatureService on an
// ephemeral loopback port and connects a stub to it. RAII shuts the server
// down on destruction.
struct TestServer {
    TestServer(const InterceptorChain* chain, std::string fqi = kProtectedFqi)
        : service{chain, std::move(fqi)} {
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

    // Issues the RPC, optionally attaching an access-token metadata entry.
    grpc::Status call(const std::string* accessToken = nullptr) {
        grpc::ClientContext ctx;
        if (accessToken) {
            ctx.AddMetadata("access-token", *accessToken);
        }
        LockServer_Parameters req;
        LockServer_Responses resp;
        return stub->LockServer(&ctx, req, &resp);
    }

    TestProtectedFeatureService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<LockController::Stub> stub;
};

// Boots a real BinaryUploadService (not the LockController stand-in above) so
// the access-token header is checked exactly where CreateBinary, UploadChunk
// and DeleteBinary each really check it.
//
// S14 (owner ruling, option B — defence in depth): UploadChunk and
// DeleteBinary gate on kBinaryUploadFqi via chain_ now, the same as
// CreateBinary gates on parameterIdentifier and as BinaryDownloadService's
// GetChunk/DeleteBinary gate on kBinaryDownloadFqi
// (BinaryDownloadService.cc:64-68, :113-117). This is deliberate asymmetry
// with the cloud transport, not an oversight: only CreateBinary's envelope
// carries a metadata field there (SiLACloudConnector.proto:50-55 vs
// :133-137), so the cloud transport structurally cannot gate the chunk
// stream or DeleteBinary at any cost — see the comment at
// CloudEnvelopeRouter.cc's kUploadChunkRequest case. The tests below pass
// the chain built with an isProtected that opts individual FQIs in or out,
// proving the gate does not fire unless an operator lists kBinaryUploadFqi
// as protected.
struct BinaryUploadServer {
    explicit BinaryUploadServer(const InterceptorChain* chain)
        : uploadService{store, 300s, chain} {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&uploadService);
        server = builder.BuildAndStart();
        channel = grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub = bt::BinaryUpload::NewStub(channel);
    }

    ~BinaryUploadServer() {
        if (server) server->Shutdown();
    }

    // Issues CreateBinary, optionally attaching an access-token metadata entry.
    grpc::Status createBinary(const std::string& parameterIdentifier,
                              bt::CreateBinaryResponse& response,
                              const std::string* accessToken = nullptr) {
        grpc::ClientContext ctx;
        if (accessToken) {
            ctx.AddMetadata("access-token", *accessToken);
        }
        bt::CreateBinaryRequest request;
        request.set_binarysize(5);
        request.set_chunkcount(1);
        request.set_parameteridentifier(parameterIdentifier);
        return stub->CreateBinary(&ctx, request, &response);
    }

    InMemoryBinaryStore store;
    BinaryUploadService uploadService;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<bt::BinaryUpload::Stub> stub;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(GrpcTransportAuthE2E, NoInterceptorChainSkipsAuthEntirely) {
    // chain == nullptr: dispatchToHandler's "if (chain && chain->auth)" guard
    // short-circuits, so no token is required even for a call that would
    // otherwise be protected.
    TestServer server{nullptr};

    const grpc::Status status = server.call();

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(GrpcTransportAuthE2E, UnprotectedFqiPassesWithoutToken) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain, kUnprotectedFqi};

    const grpc::Status status = server.call();

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(GrpcTransportAuthE2E, ProtectedFqiWithValidTokenSucceeds) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain, kProtectedFqi};
    const std::string token = store.issue("alice", {kProtectedFqi}, 60s);

    const grpc::Status status = server.call(&token);

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(GrpcTransportAuthE2E, CreateBinaryWithValidAccessTokenSucceeds) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryParam};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};
    const std::string token = tokenStore.issue("alice", {kProtectedBinaryParam}, 60s);

    bt::CreateBinaryResponse response;
    const grpc::Status status = server.createBinary(kProtectedBinaryParam, response, &token);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.binarytransferuuid().empty());
}

TEST(GrpcTransportAuthE2E, UploadChunkWithValidTokenSucceeds) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryUploadFqi};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};
    const std::string token = tokenStore.issue("alice", {std::string{kBinaryUploadFqi}}, 60s);

    // CreateBinary's own gate checks parameterIdentifier, not
    // kBinaryUploadFqi (isProtectedBinaryUploadFqi above), so it stays
    // pre-auth here regardless of the token issued for UploadChunk below.
    bt::CreateBinaryResponse createResponse;
    ASSERT_TRUE(server.createBinary(kProtectedBinaryParam, createResponse).ok());
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;
    uploadCtx.AddMetadata("access-token", token);
    auto stream = server.stub->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunkRequest;
    chunkRequest.set_binarytransferuuid(uuid);
    chunkRequest.set_chunkindex(0);
    chunkRequest.set_payload("hello");
    ASSERT_TRUE(stream->Write(chunkRequest));
    bt::UploadChunkResponse chunkResponse;
    ASSERT_TRUE(stream->Read(&chunkResponse));
    stream->WritesDone();

    const grpc::Status status = stream->Finish();

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(chunkResponse.binarytransferuuid(), uuid);
}

TEST(GrpcTransportAuthE2E, UploadChunkOnUnprotectedBinaryUploadFqiSucceedsWithoutToken) {
    AuthTokenStore tokenStore;
    // isProtected only protects kProtectedFqi (the LockController fixture's
    // FQI); neither CreateBinary's parameterIdentifier nor kBinaryUploadFqi
    // match it, so the gate stays opt-in and this whole flow stays pre-auth
    // — proof that the default deployment (no operator opt-in) is unaffected.
    AuthorizationInterceptor interceptor{tokenStore, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};

    bt::CreateBinaryResponse createResponse;
    ASSERT_TRUE(server.createBinary(kProtectedBinaryParam, createResponse).ok());
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;  // no access-token metadata: kBinaryUploadFqi is not protected here
    auto stream = server.stub->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunkRequest;
    chunkRequest.set_binarytransferuuid(uuid);
    chunkRequest.set_chunkindex(0);
    chunkRequest.set_payload("hello");
    ASSERT_TRUE(stream->Write(chunkRequest));
    bt::UploadChunkResponse chunkResponse;
    ASSERT_TRUE(stream->Read(&chunkResponse));
    stream->WritesDone();

    const grpc::Status status = stream->Finish();

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(chunkResponse.binarytransferuuid(), uuid);
}

TEST(GrpcTransportAuthE2E, UploadDeleteBinaryWithValidTokenSucceeds) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryUploadFqi};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};
    const std::string token = tokenStore.issue("alice", {std::string{kBinaryUploadFqi}}, 60s);
    bt::CreateBinaryResponse createResponse;
    ASSERT_TRUE(server.createBinary(kProtectedBinaryParam, createResponse).ok());
    const std::string uuid = createResponse.binarytransferuuid();

    bt::DeleteBinaryRequest deleteRequest;
    deleteRequest.set_binarytransferuuid(uuid);
    bt::DeleteBinaryResponse deleteResponse;
    grpc::ClientContext deleteCtx;
    deleteCtx.AddMetadata("access-token", token);

    const grpc::Status status =
        server.stub->DeleteBinary(&deleteCtx, deleteRequest, &deleteResponse);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(server.store.contains(uuid));
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths
// All CAUGHT: AuthorizationInterceptor detects and rejects them,
// and dispatchToHandler's guardHandler+sink.fail() round-trips the rejection
// through grpc::Status without losing its type or identifier.
// ---------------------------------------------------------------------------

TEST(GrpcTransportAuthE2E, ProtectedFqiWithoutTokenReturnsAbortedWithInvalidMetadata) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain, kProtectedFqi};

    const grpc::Status status = server.call();  // no access-token metadata at all

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::FrameworkError);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
}

TEST(GrpcTransportAuthE2E, ProtectedFqiWithInvalidTokenReturnsAbortedWithInvalidAccessToken) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    TestServer server{&chain, kProtectedFqi};
    const std::string bogusToken = "bogus_token_12345";

    const grpc::Status status = server.call(&bogusToken);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidAccessTokenErrorId);
}

TEST(GrpcTransportAuthE2E, CreateBinaryWithoutTokenForProtectedParamReturnsAborted) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryParam};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};

    bt::CreateBinaryResponse response;
    const grpc::Status status = server.createBinary(kProtectedBinaryParam, response);  // no access-token metadata at all

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::FrameworkError);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
    EXPECT_TRUE(response.binarytransferuuid().empty());
}

TEST(GrpcTransportAuthE2E, CreateBinaryWithInvalidTokenReturnsAborted) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryParam};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};
    const std::string bogusToken = "bogus_binary_token_67890";

    bt::CreateBinaryResponse response;
    const grpc::Status status = server.createBinary(kProtectedBinaryParam, response, &bogusToken);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidAccessTokenErrorId);
}

TEST(GrpcTransportAuthE2E, CreateBinaryWithTokenScopedToDifferentParamReturnsAborted) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryParam};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};
    // Token is valid and unexpired, but its allowedFqis set does not contain
    // kProtectedBinaryParam — exercises AuthTokenStore::validate()'s targetFqi
    // check, not just "does a token with this string exist".
    const std::string token = tokenStore.issue("alice", {"someOtherParam"}, 60s);

    bt::CreateBinaryResponse response;
    const grpc::Status status = server.createBinary(kProtectedBinaryParam, response, &token);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidAccessTokenErrorId);
}

TEST(GrpcTransportAuthE2E, UploadChunkWithoutTokenOnProtectedFqiReturnsInvalidMetadata) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryUploadFqi};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};

    bt::CreateBinaryResponse createResponse;
    ASSERT_TRUE(server.createBinary(kProtectedBinaryParam, createResponse).ok());
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;  // no access-token metadata at all
    auto stream = server.stub->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunkRequest;
    chunkRequest.set_binarytransferuuid(uuid);
    chunkRequest.set_chunkindex(0);
    chunkRequest.set_payload("hello");
    ASSERT_TRUE(stream->Write(chunkRequest));
    stream->WritesDone();

    const grpc::Status status = stream->Finish();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::FrameworkError);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
}

TEST(GrpcTransportAuthE2E, UploadDeleteBinaryWithoutTokenIsRejected) {
    // This is the RPC the earlier audit round missed: DeleteBinary hardcoded
    // chain=nullptr while CreateBinary already gated, so this call used to
    // succeed unauthenticated even under a protected chain.
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryUploadFqi};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};

    bt::CreateBinaryResponse createResponse;
    ASSERT_TRUE(server.createBinary(kProtectedBinaryParam, createResponse).ok());
    const std::string uuid = createResponse.binarytransferuuid();

    bt::DeleteBinaryRequest deleteRequest;
    deleteRequest.set_binarytransferuuid(uuid);
    bt::DeleteBinaryResponse deleteResponse;
    grpc::ClientContext deleteCtx;  // no access-token metadata at all

    const grpc::Status status =
        server.stub->DeleteBinary(&deleteCtx, deleteRequest, &deleteResponse);

    EXPECT_FALSE(status.ok());
    EXPECT_TRUE(server.store.contains(uuid));  // rejected before the store is touched
}

TEST(GrpcTransportAuthE2E, UploadChunkWithTokenScopedToAnotherFqiReturnsInvalidAccessToken) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, isProtectedBinaryUploadFqi};
    InterceptorChain chain;
    chain.auth = &interceptor;
    BinaryUploadServer server{&chain};
    // Token is valid and unexpired, but its allowedFqis set does not contain
    // kBinaryUploadFqi — same targetFqi check as
    // CreateBinaryWithTokenScopedToDifferentParamReturnsAborted above, just
    // for the feature-level FQI UploadChunk now gates on.
    const std::string token = tokenStore.issue("alice", {"org.silastandard/core/SomeOtherFeature/v1"}, 60s);

    bt::CreateBinaryResponse createResponse;
    ASSERT_TRUE(server.createBinary(kProtectedBinaryParam, createResponse).ok());
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;
    uploadCtx.AddMetadata("access-token", token);
    auto stream = server.stub->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunkRequest;
    chunkRequest.set_binarytransferuuid(uuid);
    chunkRequest.set_chunkindex(0);
    chunkRequest.set_payload("hello");
    ASSERT_TRUE(stream->Write(chunkRequest));
    stream->WritesDone();

    const grpc::Status status = stream->Finish();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kInvalidAccessTokenErrorId);
}

}  // namespace
