// test_fqi_coverage_dual_transport_e2e.cc — End-to-end tests for the FQI
// coverage gate shared by the gRPC and cloud transports (§3.1s, FqiMatch.h).
//
// The bug this closes: gRPC hands AuthorizationInterceptor::intercept a
// *feature* FQI (meta_emitter.py's kRequiresTokenFqi), while the cloud path
// hands it "<feature>/Command/<Name>" (CloudHandlerRegistration.h:102). An
// exact-match protectedFqis set can only ever close one of the two
// transports, whichever granularity the operator wrote into the list. Every
// existing cloud auth test (test_cloud_router_dispatch.cc,
// test_cloud_router_binary.cc) used isProtected = [](const std::string&) {
// return true; }, so no test ever drove a real protectedFqis list through
// both transports at once — that is exactly what this file adds.
//
// Each test below builds `isProtected` the same way SilaServerBase::build()
// does (SilaServerBase.cc:416-418): a std::vector<std::string> protectedFqis
// scanned via auth::anyFqiCovers, not a std::unordered_set with exact
// membership. It then drives the SAME protectedFqis list through a real gRPC
// unary call (GrpcAuthHarnessServer, adapted from
// test_grpc_transport_auth_e2e.cc's TestServer) and a real
// CloudEnvelopeRouter::route() call (via cloud_test::CloudRouterFixture).
//
// Caught/uncaught: every False (rejection) case below is CAUGHT — the fixed
// anyFqiCovers-based gate detects and rejects all of them on both
// transports. No known uncaught gap exists in FQI coverage matching itself;
// SiLA Client Metadata admission (NO_METADATA_ALLOWED / INVALID_METADATA) is
// a separate gate covered by test_metadata_gate_dual_transport_e2e.cc, and is
// out of scope for this file.
#include "CloudRouterTestHarness.h"

#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/GrpcTransport.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include "AuthorizationService.pb.h"
#include "LockController.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using sila2::CallContext;
using sila2::GrpcUnaryResponseSink;
using sila2::InterceptorChain;
using sila2::ResponseSink;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;
using sila2::auth::anyFqiCovers;
using sila2::error::DefinedExecutionError;
using sila2::error::FrameworkError;
using sila2::error::fromGrpcStatus;
using sila2::error::SilaError;
using namespace std::chrono_literals;

namespace lockcontroller_proto = sila2::org::silastandard::core::lockcontroller::v1;
using lockcontroller_proto::LockController;
using lockcontroller_proto::LockServer_Parameters;
using lockcontroller_proto::LockServer_Responses;

namespace cloud = sila2::org::silastandard;
namespace authzproto = sila2::org::silastandard::core::authorizationservice::v1;

// Feature-scoped FQI (what the gRPC path hands intercept()) and its two
// per-RPC children (what the cloud path hands intercept(), per
// CloudHandlerRegistration.h:102's `fqi + "/Command/" + name`).
const std::string kFeatureFqi = "org.test/DualGate/v1";
const std::string kCommandA = kFeatureFqi + "/Command/A";
const std::string kCommandB = kFeatureFqi + "/Command/B";

// Deliberately differs from kFeatureFqi only in the trailing version digit,
// to exercise FqiMatch.h's segment-boundary rule: ".../v1" must not cover
// ".../v10" via plain prefix matching.
const std::string kSiblingVersionFqi = "org.test/DualGate/v10";

// An entirely different feature, standing in for "not on the protected list at all".
const std::string kOtherFeatureFqi = "org.test/OtherFeature/v1";
const std::string kOtherFeatureCommand = kOtherFeatureFqi + "/Command/X";

// Must match CloudEnvelopeRouter.cc's private kAccessTokenMetadataFqi — the
// wire key makeCloudCallContext() recognizes and unwraps into the
// "access-token" raw key AuthorizationInterceptor reads (CloudEnvelopeRouter.cc:31).
const std::string kAccessTokenMetadataFqi =
    "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken";

// Wraps a bare token the way the cloud wire format requires: a serialized
// Metadata_AccessToken message, not the raw string (CloudEnvelopeRouter.cc:13-15).
cloud::Metadata makeAccessTokenMetadata(const std::string& token) {
    authzproto::Metadata_AccessToken wrapper;
    wrapper.mutable_accesstoken()->set_value(token);
    cloud::Metadata md;
    md.set_fullyqualifiedmetadataid(kAccessTokenMetadataFqi);
    md.set_value(wrapper.SerializeAsString());
    return md;
}

// Minimal successful command handler: echoes the parameter bytes back.
void echoCommandHandler(const std::string& params, sila2::CallContext&,
                         sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
    cloud::SiLAServerMessage resp;
    resp.set_requestuuid(requestUUID);
    resp.mutable_unobservablecommandresponse()->set_response(params);
    w.write(resp);
}

// Observable-command _Result handler: fixed body, for the S15 follow-up-gate
// tests below, which only need something recognizable to distinguish "the
// handler ran" from "the gate rejected before it".
void echoObservableResultHandler(const std::string&, sila2::CallContext&,
                                 sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
    cloud::SiLAServerMessage resp;
    resp.set_requestuuid(requestUUID);
    resp.mutable_observablecommandresponse()->set_response("result-data");
    w.write(resp);
}

// ---------------------------------------------------------------------------
// gRPC-side harness — adapted from test_grpc_transport_auth_e2e.cc's
// TestProtectedFeatureService/TestServer. Reuses LockController's generated
// grpc::Service purely as wire plumbing; it has nothing to do with lock
// semantics.
// ---------------------------------------------------------------------------

class GrpcAuthHarnessService final : public LockController::Service {
public:
    GrpcAuthHarnessService(const InterceptorChain* chain, std::string fqi)
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

// Boots a real gRPC server on an ephemeral loopback port and connects a stub.
// RAII shuts the server down on destruction.
struct GrpcAuthHarnessServer {
    GrpcAuthHarnessServer(const InterceptorChain* chain, std::string fqi)
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

    ~GrpcAuthHarnessServer() {
        if (server) server->Shutdown();
    }

    grpc::Status call(const std::string* accessToken = nullptr) {
        grpc::ClientContext ctx;
        if (accessToken) {
            ctx.AddMetadata("access-token", *accessToken);
        }
        LockServer_Parameters req;
        LockServer_Responses resp;
        return stub->LockServer(&ctx, req, &resp);
    }

    GrpcAuthHarnessService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<LockController::Stub> stub;
};

// ---------------------------------------------------------------------------
// Cloud-side fixture — adds a route()-and-pop helper on top of the shared
// CloudRouterFixture (CloudRouterTestHarness.h) so each test can dispatch a
// single unobservable command with or without an access-token metadata entry.
// ---------------------------------------------------------------------------

class DualTransportGate : public cloud_test::CloudRouterFixture {
protected:
    cloud::SiLAServerMessage dispatchCommand(sila2::CloudEnvelopeRouter& router,
                                              const std::string& fqi,
                                              const std::string& requestUuid,
                                              const std::string* accessToken = nullptr) {
        cloud::SiLAClientMessage msg;
        msg.set_requestuuid(requestUuid);
        auto* exec = msg.mutable_unobservablecommandexecution();
        exec->set_fullyqualifiedcommandid(fqi);
        if (accessToken) {
            *exec->mutable_commandparameter()->add_metadata() = makeAccessTokenMetadata(*accessToken);
        }
        router.route(msg, *writer_, writer_, calls_);
        return popResponse();
    }
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

// Core claim of §3.1s's fix: a single feature-level protectedFqis entry,
// combined with a valid token, authorizes the call on BOTH transports even
// though each hands intercept() a different FQI granularity.
TEST_F(DualTransportGate, FeatureLevelEntryWithValidTokenSucceedsOnBothTransports) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    const std::string token = store.issue("alice", {kFeatureFqi}, 60s);

    GrpcAuthHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call(&token);
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandA, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandA, "req-1", &token);
    EXPECT_TRUE(resp.has_unobservablecommandresponse());
}

// Per-RPC entries close only the RPC they name — a sibling command under the
// same feature is untouched.
TEST_F(DualTransportGate, PerRpcEntryLeavesSiblingCommandOpen) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kCommandA};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandB, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandB, "req-2");  // no token

    EXPECT_TRUE(resp.has_unobservablecommandresponse());
}

// An FQI absent from protectedFqis entirely is pre-auth: no token required on
// either transport. Evidence that the gate isn't accidentally locking
// everything (the flip side of the coverage bug).
TEST_F(DualTransportGate, UnregisteredFqiPassesWithoutTokenOnBothTransports) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    GrpcAuthHarnessServer grpcServer{&chain, kOtherFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call();  // no token
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kOtherFeatureCommand, echoCommandHandler);
    const auto resp = dispatchCommand(router, kOtherFeatureCommand, "req-3");  // no token
    EXPECT_TRUE(resp.has_unobservablecommandresponse());
}

// Boundary condition FqiMatch.h exists for: ".../v1" must not cover
// ".../v10" via a bare starts_with — only a "/"-delimited child counts.
// Plain TEST, not TEST_F: this case only needs the gRPC harness plus a
// direct anyFqiCovers() check, so it skips DualTransportGate's cloud socket setup.
TEST(FqiCoverageDualTransportE2E, V1EntryDoesNotCoverV10SiblingFeature) {
    const std::vector<std::string> protectedFqis{kFeatureFqi};

    // Direct coverage check, alongside the e2e call below: the exact
    // boundary FqiMatch.h's header comment calls out.
    EXPECT_FALSE(anyFqiCovers(protectedFqis, kSiblingVersionFqi));
    EXPECT_TRUE(anyFqiCovers(protectedFqis, kCommandA));

    AuthTokenStore store;
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    GrpcAuthHarnessServer grpcServer{&chain, kSiblingVersionFqi};
    const grpc::Status grpcStatus = grpcServer.call();  // no token

    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();
}

// Part A p87: FQI comparison "MUST always be checked without taking lower and
// upper case into account". Plain TEST, not TEST_F: a direct anyFqiCovers()
// check needs no harness.
TEST(FqiCoverageDualTransportE2E, CaseVariantOfCoveredFqiIsStillCovered) {
    const std::vector<std::string> protectedFqis{kFeatureFqi};

    // An upper-case variant of a covered FQI is still covered...
    EXPECT_TRUE(anyFqiCovers(protectedFqis, "ORG.TEST/DUALGATE/V1/COMMAND/A"));
    // ...but case folding must not weaken the v1-vs-v10 segment-boundary
    // rule: the sibling version is still not covered, uppercased or not.
    EXPECT_FALSE(anyFqiCovers(protectedFqis, "ORG.TEST/DUALGATE/V10"));
}

// §S15 (owner option 1): the cloud follow-up envelopes (_Info/_Intermediate/
// _Result) carry no metadata field, so the gate can only ever see the token
// snapshotted at ObservableCommandInitiation. This proves that snapshot
// authorizes a protected command's _Result follow-up on the SAME token that
// authorized initiation, with no metadata on the follow-up envelope itself.
TEST_F(DualTransportGate, FollowupInheritsInitiationToken) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    const std::string token = store.issue("alice", {kFeatureFqi}, 60s);

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    router.registerCommandHandler(kCommandA + "_Result", echoObservableResultHandler);
    router.registerExecutionFQI(exec->uuid(), kCommandA, token);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-8");
    msg.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    EXPECT_TRUE(resp.has_observablecommandresponse());
    EXPECT_FALSE(resp.has_commanderror());
}

// Regression guard for the failure mode a bare-context gate (mirroring
// dispatchTo exactly, the rejected option) would produce: the new gate must
// not close a follow-up whose base FQI was never on protectedFqis at all.
TEST_F(DualTransportGate, UnprotectedFollowupStillServesWithNoToken) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kOtherFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    router.registerCommandHandler(kCommandA + "_Result", echoObservableResultHandler);
    router.registerExecutionFQI(exec->uuid(), kCommandA);  // 2-arg form: no token snapshotted

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-9");
    msg.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    EXPECT_TRUE(resp.has_observablecommandresponse());
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT by the anyFqiCovers-based gate.
// ---------------------------------------------------------------------------

// Core claim's rejection half: the SAME feature-level entry that authorized a
// valid token above rejects both transports when no token is presented —
// proof neither transport's gate is left open by the granularity mismatch.
TEST_F(DualTransportGate, FeatureLevelEntryWithoutTokenRejectedOnBothTransports) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    GrpcAuthHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call();  // no token
    ASSERT_FALSE(grpcStatus.ok());
    EXPECT_EQ(grpcStatus.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandA, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandA, "req-4");  // no token
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

// Per-RPC entries do close the RPC they name (the other half of the sibling
// test above).
TEST_F(DualTransportGate, PerRpcEntryRejectsThatCommandWithoutToken) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kCommandA};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandA, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandA, "req-5");  // no token

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

// A token that has expired must not authorize either transport, even though
// it was issued for exactly the protected FQI.
TEST_F(DualTransportGate, ExpiredTokenRejectedOnBothTransports) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    const std::string token = store.issue(
        "alice", {kFeatureFqi}, std::chrono::duration_cast<std::chrono::seconds>(1ms));
    std::this_thread::sleep_for(5ms);

    GrpcAuthHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call(&token);
    ASSERT_FALSE(grpcStatus.ok());
    EXPECT_EQ(grpcStatus.error_code(), grpc::StatusCode::ABORTED);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandA, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandA, "req-6", &token);
    ASSERT_TRUE(resp.has_commanderror());
    EXPECT_TRUE(resp.commanderror().has_definedexecutionerror());
}

// A token scoped to a different feature entirely must not authorize the
// protected one on either transport.
TEST_F(DualTransportGate, TokenScopedToDifferentFqiRejectedOnBothTransports) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    const std::string token = store.issue("alice", {kOtherFeatureFqi}, 60s);

    GrpcAuthHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call(&token);
    ASSERT_FALSE(grpcStatus.ok());
    EXPECT_EQ(grpcStatus.error_code(), grpc::StatusCode::ABORTED);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandA, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandA, "req-7", &token);
    ASSERT_TRUE(resp.has_commanderror());
    EXPECT_TRUE(resp.commanderror().has_definedexecutionerror());
}

// Core §S15 rejection: an execution registered with no access token (e.g. one
// initiated before the owner's fix, or hand-wired without one) must not let
// its _Result follow-up through on a protected FQI, and the underlying
// handler must never run -- proof the gate runs before dispatch, not after.
TEST_F(DualTransportGate, FollowupWithoutInitiationTokenRejected) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    std::atomic<bool> handlerRan{false};
    router.registerCommandHandler(kCommandA + "_Result",
        [&handlerRan](const std::string&, sila2::CallContext&,
                      sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            handlerRan.store(true);
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablecommandresponse()->set_response("result-data");
            w.write(resp);
        });
    router.registerExecutionFQI(exec->uuid(), kCommandA);  // no token registered

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-10");
    msg.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_EQ(resp.commanderror().frameworkerror().message(),
              "No access token provided for protected feature");
    EXPECT_FALSE(handlerRan.load());
}

// A token snapshotted at initiation but since expired must reject the
// follow-up on _Intermediate too, not just _Result -- both dispatch through
// the same makeFollowupContext gate.
TEST_F(DualTransportGate, FollowupWithExpiredInitiationTokenRejected) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;
    const std::string token = store.issue(
        "alice", {kFeatureFqi}, std::chrono::duration_cast<std::chrono::seconds>(1ms));
    std::this_thread::sleep_for(5ms);

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    router.registerCommandHandler(kCommandA + "_Intermediate",
        [](const std::string&, sila2::CallContext&, sila2::StreamWriteSerializer& w,
           const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_observablecommandintermediateresponse()->set_response("intermediate-data");
            w.write(resp);
        });
    router.registerExecutionFQI(exec->uuid(), kCommandA, token);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-11");
    msg.mutable_observablecommandintermediateresponsesubscription()
       ->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_definedexecutionerror());
    // The live erroridentifier includes the "/DefinedExecutionError/" segment
    // (AuthorizationInterceptor.cc) -- the spec's cited string omitted it.
    EXPECT_EQ(resp.commanderror().definedexecutionerror().erroridentifier(),
              "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken");
}

// S15's second gate site: the router-synthesized _Info pump. Every other
// case in this file reaches the gate through dispatchObservableByUuid
// (_Result / _Intermediate); the ExecutionInfoSubscription fallback -- an
// entry but no "_Info" handler registered -- has its own makeFollowupContext
// call and would keep passing those tests even if only that branch regressed.
TEST_F(DualTransportGate, SynthesizedInfoFallbackWithoutInitiationTokenRejected) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    // Deliberately NO kCommandA + "_Info" handler: with an entry present but
    // no handler, route() takes the synthesized-pump fallback branch instead
    // of dispatchObservableByUuid.
    router.registerExecutionFQI(exec->uuid(), kCommandA);  // no token registered

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-12");
    msg.mutable_observablecommandexecutioninfosubscription()
       ->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    // The denial must be the only envelope -- no synthesized executionInfo
    // pump may have started behind the error.
    EXPECT_TRUE(noResponse());
}

// S28: the follow-up gate must answer before the handler-registration probe.
// Entry registered, NO "_Result" handler, no token, protected feature: the
// pre-S28 order replied "no handler registered" (a
// COMMAND_EXECUTION_NOT_ACCEPTED FrameworkError), telling an unauthorized
// caller whether the handler exists; the gate must deny with
// INVALID_METADATA instead, indistinguishable from any other registered UUID.
TEST_F(DualTransportGate, UnauthorizedFollowupDeniedBeforeHandlerProbe) {
    AuthTokenStore store;
    const std::vector<std::string> protectedFqis{kFeatureFqi};
    auto isProtected = [&protectedFqis](const std::string& fqi) {
        return anyFqiCovers(protectedFqis, fqi);
    };
    AuthorizationInterceptor interceptor{store, isProtected};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    // Deliberately NO kCommandA + "_Result" handler registered.
    router.registerExecutionFQI(exec->uuid(), kCommandA);  // no token registered

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-13");
    msg.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_EQ(resp.commanderror().frameworkerror().message(),
              "No access token provided for protected feature");
    // The denial must be the only envelope -- the no-handler reply must not
    // follow it.
    EXPECT_TRUE(noResponse());
}

}  // namespace
