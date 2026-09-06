// test_grpc_dispatch_e2e.cc — End-to-end tests for GrpcTransport::dispatchToHandler's
// command/property/streaming dispatch and its guardHandler error-routing matrix
// (architecture.md §3.8). Drives the full path over a real socket:
// grpc::ServerContext -> buildHeaders -> MetadataExtractingInterceptor::extract ->
// guardHandler -> the handler body -> back through GrpcUnaryResponseSink /
// GrpcStreamResponseSink -> grpc::Status.
//
// Complements two existing files that each cover one slice of this same
// function: test_grpc_transport_auth_e2e.cc stops at the auth branch (its
// handler is a no-op success), and test_grpc_transport_sink_e2e.cc calls
// sink.fail()/status() directly, bypassing dispatchToHandler and guardHandler
// entirely (and explicitly leaves the streaming sink's writer_ null). This
// file is the one that exercises: a handler that actually reads its request
// and returns real response data, sila-* metadata reaching the handler
// alongside a validated access token, a real streaming RPC through a live
// grpc::ServerWriter, and guardHandler's full six-way catch matrix
// (ValidationError, DefinedExecutionError, UndefinedExecutionError,
// FrameworkError, std::exception, non-std::exception) as thrown by a
// Command/Property handler rather than by the auth interceptor.
//
// TestDispatchService and TestStreamingService below reuse LockController's
// and ErrorRecoveryService's generated grpc::Service/messages purely as wire
// plumbing (existing, already-linked unary + streaming RPCs) — the same
// technique test_grpc_transport_auth_e2e.cc uses with LockController. Neither
// has anything to do with locking or error-recovery semantics here.
#include <sila/server/transport/GrpcTransport.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/transport/InterceptorChain.h>

#include "LockController.grpc.pb.h"
#include "ErrorRecoveryService.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
// GrpcStreamResponseSink<T>::send() (instantiated below via a real streaming
// dispatchToHandler call) calls grpc::ServerWriter<T>::Write(), which needs
// the full ServerWriter definition — grpcpp.h only forward-declares it.
#include <grpcpp/support/sync_stream.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
using sila2::CallContext;
using sila2::GrpcStreamResponseSink;
using sila2::GrpcUnaryResponseSink;
using sila2::InterceptorChain;
using sila2::ResponseSink;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;
using sila2::error::DefinedExecutionError;
using sila2::error::FrameworkError;
using sila2::error::fromGrpcStatus;
using sila2::error::SiLAError;
using sila2::error::UndefinedExecutionError;
using sila2::error::ValidationError;
using namespace std::chrono_literals;

namespace lockcontroller_proto = sila2::org::silastandard::core::lockcontroller::v1;
using lockcontroller_proto::Get_IsLocked_Parameters;
using lockcontroller_proto::Get_IsLocked_Responses;
using lockcontroller_proto::LockController;
using lockcontroller_proto::LockServer_Parameters;
using lockcontroller_proto::LockServer_Responses;
using lockcontroller_proto::UnlockServer_Parameters;
using lockcontroller_proto::UnlockServer_Responses;

namespace recovery_proto = sila2::org::silastandard::core::errorrecoveryservice::v2;
using recovery_proto::ErrorRecoveryService;
using recovery_proto::Subscribe_RecoverableErrors_Parameters;
using recovery_proto::Subscribe_RecoverableErrors_Responses;

const std::string kFqi = "org.test/DispatchFeature/v1";

// Sentinel LockIdentifier values that steer TestDispatchService::LockServer's
// handler toward a specific guardHandler catch clause — the parameter itself
// doubles as the error trigger so no extra RPC surface is needed.
const std::string kTriggerValidation = "throw-validation";
const std::string kTriggerDefined = "throw-defined";
const std::string kTriggerUndefined = "throw-undefined";
const std::string kTriggerFramework = "throw-framework";
const std::string kTriggerStdException = "throw-std";
const std::string kTriggerNonStdException = "throw-nonstd";
const std::string kDefinedErrorId = "org.test/DispatchFeature/v1/DefinedExecutionError/TestError";

// ---------------------------------------------------------------------------
// TestDispatchService — LockServer/UnlockServer/Get_IsLocked wired through
// dispatchToHandler exactly like a codegen-generated ServiceAdapter would
// (service_adapter.h.j2). LockServer's handler branches on LockIdentifier to
// reach every guardHandler catch clause; UnlockServer and Get_IsLocked stay
// simple so they can each cover a distinct RPC shape (command with no
// response data vs. property read with real response data).
// ---------------------------------------------------------------------------
class TestDispatchService final : public LockController::Service {
public:
    explicit TestDispatchService(const InterceptorChain* chain) : chain_{chain} {}

    // Captured so tests can assert the handler actually ran with the
    // correctly-deserialized request and the metadata dispatchToHandler
    // extracted for it.
    std::string lastLockIdentifier;
    std::string lastMetadataMarker;
    bool throwOnGetIsLocked = false;

    grpc::Status LockServer(grpc::ServerContext* ctx,
                             const LockServer_Parameters* req,
                             LockServer_Responses* resp) override {
        GrpcUnaryResponseSink<LockServer_Responses> sink(resp);
        sila2::SilaHandler<LockServer_Parameters, LockServer_Responses> handler =
            [this](const LockServer_Parameters& r, CallContext& c,
                   ResponseSink<LockServer_Responses>& s) {
                const std::string& id = r.lockidentifier().value();
                lastLockIdentifier = id;
                lastMetadataMarker = c.metadata("sila-test-marker").value_or("<missing>");

                if (id == kTriggerValidation) {
                    throw ValidationError{"LockIdentifier", "sentinel-triggered validation failure"};
                }
                if (id == kTriggerDefined) {
                    throw DefinedExecutionError{kDefinedErrorId, "sentinel-triggered defined failure"};
                }
                if (id == kTriggerUndefined) {
                    throw UndefinedExecutionError{"sentinel-triggered undefined failure"};
                }
                if (id == kTriggerFramework) {
                    // InvalidMetadata, not the Invalid sentinel: Invalid is
                    // documented (SiLAErrorSubtypes.cc) as never meant to
                    // reach serialization, so it wouldn't round-trip here.
                    throw FrameworkError{FrameworkError::FrameworkErrorType::InvalidMetadata,
                                          "sentinel-triggered framework failure"};
                }
                if (id == kTriggerStdException) {
                    throw std::runtime_error{"sentinel-triggered std::exception"};
                }
                if (id == kTriggerNonStdException) {
                    throw 42;  // not a std::exception subclass
                }
                s.send(LockServer_Responses{});
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, chain_, kFqi, resp);
        return sink.status();
    }

    grpc::Status UnlockServer(grpc::ServerContext* ctx,
                               const UnlockServer_Parameters* req,
                               UnlockServer_Responses* resp) override {
        GrpcUnaryResponseSink<UnlockServer_Responses> sink(resp);
        sila2::SilaHandler<UnlockServer_Parameters, UnlockServer_Responses> handler =
            [this](const UnlockServer_Parameters& r, CallContext&,
                   ResponseSink<UnlockServer_Responses>& s) {
                lastLockIdentifier = r.lockidentifier().value();
                s.send(UnlockServer_Responses{});
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, chain_, kFqi, resp);
        return sink.status();
    }

    grpc::Status Get_IsLocked(grpc::ServerContext* ctx,
                               const Get_IsLocked_Parameters* req,
                               Get_IsLocked_Responses* resp) override {
        GrpcUnaryResponseSink<Get_IsLocked_Responses> sink(resp);
        sila2::SilaHandler<Get_IsLocked_Parameters, Get_IsLocked_Responses> handler =
            [this](const Get_IsLocked_Parameters&, CallContext&,
                   ResponseSink<Get_IsLocked_Responses>& s) {
                if (throwOnGetIsLocked) {
                    throw DefinedExecutionError{kDefinedErrorId, "property handler failure"};
                }
                Get_IsLocked_Responses r;
                r.mutable_islocked()->set_value(true);
                s.send(r);
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, chain_, kFqi, resp);
        return sink.status();
    }

private:
    const InterceptorChain* chain_;
};

// Boots a real gRPC server hosting TestDispatchService on an ephemeral
// loopback port and connects a stub. RAII shuts the server down on
// destruction.
struct DispatchTestServer {
    explicit DispatchTestServer(const InterceptorChain* chain) : service{chain} {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        channel = grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub = LockController::NewStub(channel);
    }

    ~DispatchTestServer() {
        if (server) server->Shutdown();
    }

    TestDispatchService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<LockController::Stub> stub;
};

// ---------------------------------------------------------------------------
// TestStreamingService — Subscribe_RecoverableErrors wired through
// dispatchToHandler with a real grpc::ServerWriter, covering the
// GrpcStreamResponseSink half of dispatchToHandler over a live streaming RPC
// (test_grpc_transport_sink_e2e.cc deliberately leaves writer_ null; see that
// file's header comment).
// ---------------------------------------------------------------------------
class TestStreamingService final : public ErrorRecoveryService::Service {
public:
    TestStreamingService(const InterceptorChain* chain, int sendCount, bool throwAfterSend)
        : chain_{chain}, sendCount_{sendCount}, throwAfterSend_{throwAfterSend} {}

    grpc::Status Subscribe_RecoverableErrors(
            grpc::ServerContext* ctx,
            const Subscribe_RecoverableErrors_Parameters* req,
            grpc::ServerWriter<Subscribe_RecoverableErrors_Responses>* writer) override {
        GrpcStreamResponseSink<Subscribe_RecoverableErrors_Responses> sink(writer);
        sila2::SilaHandler<Subscribe_RecoverableErrors_Parameters,
                            Subscribe_RecoverableErrors_Responses> handler =
            [this](const Subscribe_RecoverableErrors_Parameters&, CallContext&,
                   ResponseSink<Subscribe_RecoverableErrors_Responses>& s) {
                for (int i = 0; i < sendCount_; ++i) {
                    s.send(Subscribe_RecoverableErrors_Responses{});
                }
                if (throwAfterSend_) {
                    throw UndefinedExecutionError{"sentinel-triggered mid-stream failure"};
                }
                s.finish();
            };
        dispatchToHandler(ctx, *req, sink, handler, chain_, kFqi);
        return sink.status();
    }

private:
    const InterceptorChain* chain_;
    int sendCount_;
    bool throwAfterSend_;
};

// Boots a real gRPC server hosting TestStreamingService and connects a stub.
// RAII shuts the server down on destruction.
struct StreamingTestServer {
    StreamingTestServer(const InterceptorChain* chain, int sendCount, bool throwAfterSend)
        : service{chain, sendCount, throwAfterSend} {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        channel = grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub = ErrorRecoveryService::NewStub(channel);
    }

    ~StreamingTestServer() {
        if (server) server->Shutdown();
    }

    TestStreamingService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ErrorRecoveryService::Stub> stub;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(GrpcDispatchE2E, LockServerSucceedsAndHandlerReceivesRequestAndSilaMetadata) {
    DispatchTestServer server{nullptr};

    grpc::ClientContext ctx;
    ctx.AddMetadata("sila-test-marker", "marker-value-1");
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value("lock-abc");
    req.mutable_timeout()->set_value(30);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(server.service.lastLockIdentifier, "lock-abc");
    EXPECT_EQ(server.service.lastMetadataMarker, "marker-value-1");
}

TEST(GrpcDispatchE2E, UnlockServerSucceedsWithDifferentRequestShape) {
    DispatchTestServer server{nullptr};

    grpc::ClientContext ctx;
    UnlockServer_Parameters req;
    req.mutable_lockidentifier()->set_value("lock-xyz");
    UnlockServer_Responses resp;

    const grpc::Status status = server.stub->UnlockServer(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(server.service.lastLockIdentifier, "lock-xyz");
}

TEST(GrpcDispatchE2E, GetIsLockedPropertyReadReturnsHandlerValue) {
    DispatchTestServer server{nullptr};

    grpc::ClientContext ctx;
    Get_IsLocked_Parameters req;
    Get_IsLocked_Responses resp;

    const grpc::Status status = server.stub->Get_IsLocked(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(resp.islocked().value());
}

TEST(GrpcDispatchE2E, SubscribeRecoverableErrorsStreamsAllHandlerSends) {
    StreamingTestServer server{nullptr, /*sendCount=*/3, /*throwAfterSend=*/false};

    grpc::ClientContext ctx;
    Subscribe_RecoverableErrors_Parameters req;
    auto reader = server.stub->Subscribe_RecoverableErrors(&ctx, req);

    int received = 0;
    Subscribe_RecoverableErrors_Responses resp;
    while (reader->Read(&resp)) {
        ++received;
    }
    const grpc::Status status = reader->Finish();

    EXPECT_EQ(received, 3);
    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(GrpcDispatchE2E, ProtectedFqiWithValidTokenAndSilaMetadataBothReachHandler) {
    // Proves buildHeaders()/MetadataExtractingInterceptor::extract() and
    // AuthorizationInterceptor both act on the same real header set within
    // one dispatchToHandler call, not just in isolation.
    AuthTokenStore store;
    auto isProtected = [](const std::string&) { return true; };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    DispatchTestServer server{&chain};
    const std::string token = store.issue("alice", {kFqi}, 60s);

    grpc::ClientContext ctx;
    ctx.AddMetadata("access-token", token);
    ctx.AddMetadata("sila-test-marker", "marker-value-2");
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value("lock-authed");
    req.mutable_timeout()->set_value(10);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(server.service.lastLockIdentifier, "lock-authed");
    EXPECT_EQ(server.service.lastMetadataMarker, "marker-value-2");
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths
// All nine are CAUGHT: guardHandler's three catch clauses (SiLAError,
// std::exception, ...) and AuthorizationInterceptor detect and convert every
// one of these into a well-formed ABORTED grpc::Status.
// ---------------------------------------------------------------------------

TEST(GrpcDispatchE2E, HandlerThrowsValidationErrorReturnsAbortedValidationError) {
    DispatchTestServer server{nullptr};
    grpc::ClientContext ctx;
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value(kTriggerValidation);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(), "LockIdentifier");
}

TEST(GrpcDispatchE2E, HandlerThrowsDefinedExecutionErrorReturnsAbortedWithDeclaredIdentifier) {
    DispatchTestServer server{nullptr};
    grpc::ClientContext ctx;
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value(kTriggerDefined);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kDefinedErrorId);
}

TEST(GrpcDispatchE2E, HandlerThrowsUndefinedExecutionErrorReturnsAbortedUndefinedExecutionError) {
    DispatchTestServer server{nullptr};
    grpc::ClientContext ctx;
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value(kTriggerUndefined);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::UndefinedExecutionError);
    const auto* err = dynamic_cast<const UndefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(std::string{err->what()}, "sentinel-triggered undefined failure");
}

TEST(GrpcDispatchE2E, HandlerThrowsFrameworkErrorReturnsAbortedFrameworkError) {
    DispatchTestServer server{nullptr};
    grpc::ClientContext ctx;
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value(kTriggerFramework);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::FrameworkError);
    const auto* err = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
}

TEST(GrpcDispatchE2E, HandlerThrowsStdExceptionWrapsAsUndefinedExecutionErrorWithWhatMessage) {
    DispatchTestServer server{nullptr};
    grpc::ClientContext ctx;
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value(kTriggerStdException);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::UndefinedExecutionError);
    const auto* err = dynamic_cast<const UndefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(std::string{err->what()}, "sentinel-triggered std::exception");
}

TEST(GrpcDispatchE2E, HandlerThrowsNonStdExceptionWrapsAsUndefinedExecutionErrorUnknownException) {
    DispatchTestServer server{nullptr};
    grpc::ClientContext ctx;
    LockServer_Parameters req;
    req.mutable_lockidentifier()->set_value(kTriggerNonStdException);
    LockServer_Responses resp;

    const grpc::Status status = server.stub->LockServer(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::UndefinedExecutionError);
    const auto* err = dynamic_cast<const UndefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(std::string{err->what()}, "unknown exception");
}

TEST(GrpcDispatchE2E, GetIsLockedHandlerThrowsReturnsAbortedDefinedExecutionError) {
    DispatchTestServer server{nullptr};
    server.service.throwOnGetIsLocked = true;
    grpc::ClientContext ctx;
    Get_IsLocked_Parameters req;
    Get_IsLocked_Responses resp;

    const grpc::Status status = server.stub->Get_IsLocked(&ctx, req, &resp);

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(), kDefinedErrorId);
}

TEST(GrpcDispatchE2E, SubscribeRecoverableErrorsHandlerThrowsMidStreamReturnsAborted) {
    StreamingTestServer server{nullptr, /*sendCount=*/2, /*throwAfterSend=*/true};

    grpc::ClientContext ctx;
    Subscribe_RecoverableErrors_Parameters req;
    auto reader = server.stub->Subscribe_RecoverableErrors(&ctx, req);

    int received = 0;
    Subscribe_RecoverableErrors_Responses resp;
    while (reader->Read(&resp)) {
        ++received;
    }
    const grpc::Status status = reader->Finish();

    EXPECT_EQ(received, 2);
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    EXPECT_EQ(reconstructed->errorType(), SiLAError::ErrorType::UndefinedExecutionError);
}

TEST(GrpcDispatchE2E, SubscribeRecoverableErrorsRejectedByAuthReturnsAbortedBeforeAnySend) {
    AuthTokenStore store;
    auto isProtected = [](const std::string&) { return true; };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    std::atomic_bool loggedAuthWarning = false;
    chain.logCallback = [&loggedAuthWarning](sila2::LogLevel level,
                                             std::string_view category,
                                             std::string_view message) {
        loggedAuthWarning = level == sila2::LogLevel::kWarning && category == "auth" &&
                            message == "access denied: " + kFqi;
    };
    StreamingTestServer server{&chain, /*sendCount=*/3, /*throwAfterSend=*/false};

    grpc::ClientContext ctx;  // no access-token metadata
    Subscribe_RecoverableErrors_Parameters req;
    auto reader = server.stub->Subscribe_RecoverableErrors(&ctx, req);

    Subscribe_RecoverableErrors_Responses resp;
    EXPECT_FALSE(reader->Read(&resp));  // stream ends before the handler ever sends
    const grpc::Status status = reader->Finish();

    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    EXPECT_EQ(reconstructed->errorType(), SiLAError::ErrorType::FrameworkError);
    const auto* error = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
    EXPECT_TRUE(loggedAuthWarning.load());
}

}  // namespace
