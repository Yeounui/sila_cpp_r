// test_metadata_gate_dual_transport_e2e.cc — End-to-end tests for the SiLA
// Client Metadata admission gate (§S5) shared by the gRPC and cloud
// transports (MetadataPolicy.h). Adapted from the sibling FQI-coverage e2e
// file (test_fqi_coverage_dual_transport_e2e.cc): the gRPC harness there is
// reused, with the access-token header replaced by an arbitrary
// (key, value) header and a second RPC added whose dispatchToHandler Req is
// a bare sila2::org::silastandard::CommandExecutionUUID, so the observable
// follow-up exemption (GrpcTransport.h's `if constexpr`) is driven for real
// instead of only unit-tested in isolation.
//
// Caught/uncaught: every rejection case below is CAUGHT -- the gate detects
// and reports NO_METADATA_ALLOWED / INVALID_METADATA on both transports.
// Known UNCAUGHT gap, a deliberate non-goal of §S5: metadata VALUE
// validation (declared SiLA Data Type / Constrained Type) -- a bad value is
// the Feature's own DefinedExecutionError, not a framework error. The
// CreateBinary seam is gated on BOTH transports: gRPC as a side effect of
// dispatching under the caller's parameterIdentifier
// (BinaryUploadService.cc:38), cloud at the kCreateBinaryUploadRequest
// branch, where the refusal is degraded to a BinaryTransferError because
// that oneof cannot carry a SilaError (CreateBinary cases below).
#include "CloudRouterTestHarness.h"

#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/GrpcTransport.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include "LockController.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

using sila2::CallContext;
using sila2::GrpcUnaryResponseSink;
using sila2::InterceptorChain;
using sila2::ResponseSink;
using sila2::error::FrameworkError;
using sila2::error::fromGrpcStatus;
using sila2::error::SilaError;
using sila2::metadataHeaderKey;

namespace lockcontroller_proto = sila2::org::silastandard::core::lockcontroller::v1;
using lockcontroller_proto::LockController;
using lockcontroller_proto::LockServer_Parameters;
using lockcontroller_proto::LockServer_Responses;
using lockcontroller_proto::Get_IsLocked_Parameters;
using lockcontroller_proto::Get_IsLocked_Responses;

// SiLACloudConnector.proto and SiLAFramework.proto share one package
// (sila2.org.silastandard), so this single alias covers both cloud::Metadata
// and the bare framework CommandExecutionUUID used below.
namespace cloud = sila2::org::silastandard;

// Feature-scoped FQI and its child Command, standing in for a Feature the
// server declares metadata for.
const std::string kFeatureFqi = "org.test/MetaGate/v1";
const std::string kCommandFqi = kFeatureFqi + "/Command/Do";
const std::string kMetaFqi = kFeatureFqi + "/Metadata/Thing";

// Declared but never presented/expected by these tests -- stands in for
// "some other declared metadata", not the one under test.
const std::string kOtherMetaFqi = kFeatureFqi + "/Metadata/Other";

// Spelled literally, not via sila2::kSiLAServiceFeatureFqi: pins the wire
// truth a real client would send, independent of which header constant the
// implementation happens to use today.
const std::string kSiLAServiceFqi = "org.silastandard/core/SiLAService/v1";
const std::string kGetFeatureDefinitionFqi = kSiLAServiceFqi + "/Command/GetFeatureDefinition";
const std::string kSetServerNameFqi = kSiLAServiceFqi + "/Command/SetServerName";

// Deliberately shares the "SiLAService" prefix without being the SiLAService
// Feature -- FqiMatch.h's segment-boundary rule ("/" required) must not
// treat this as covered.
const std::string kSiLAServiceLookalikeFqi = "org.silastandard/core/SiLAServiceExtra/v1";
const std::string kSiLAServiceLookalikeCommandFqi =
    kSiLAServiceLookalikeFqi + "/Command/DoSomething";

cloud::Metadata makeMetadata(const std::string& fqi, const std::string& value) {
    cloud::Metadata md;
    md.set_fullyqualifiedmetadataid(fqi);
    md.set_value(value);
    return md;
}

// ---------------------------------------------------------------------------
// gRPC-side harness. Reuses LockController's generated grpc::Service purely
// as wire plumbing, as test_fqi_coverage_dual_transport_e2e.cc does for
// LockServer -- Get_IsLocked's actual request/response shape is irrelevant
// here; only the Req type dispatchToHandler is instantiated with matters,
// and that is chosen explicitly below, not inherited from the wire message.
// ---------------------------------------------------------------------------

class GrpcMetadataGateHarnessService final : public LockController::Service {
public:
    GrpcMetadataGateHarnessService(const InterceptorChain* chain, std::string fqi)
        : chain_{chain}, fqi_{std::move(fqi)} {}

    // Ordinary (non-follow-up) call: Req is the real generated parameter
    // type, exercising the gate's normal path.
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

    // Observable-follow-up stand-in: the actual Get_IsLocked_Parameters off
    // the wire is discarded and a bare CommandExecutionUUID is passed to
    // dispatchToHandler instead, so template deduction picks
    // Req = org::silastandard::CommandExecutionUUID -- the same type every
    // real _Info/_Intermediate/_Result adapter uses (meta_emitter.py) -- and
    // drives GrpcTransport.h's `if constexpr` exemption for real.
    grpc::Status Get_IsLocked(grpc::ServerContext* ctx,
                              const Get_IsLocked_Parameters*,
                              Get_IsLocked_Responses* resp) override {
        GrpcUnaryResponseSink<Get_IsLocked_Responses> sink(resp);
        sila2::SilaHandler<cloud::CommandExecutionUUID, Get_IsLocked_Responses> handler =
            [](const cloud::CommandExecutionUUID&, CallContext&,
               ResponseSink<Get_IsLocked_Responses>& s) {
                Get_IsLocked_Responses r;
                r.mutable_islocked()->set_value(true);
                s.send(r);
                s.finish();
            };
        cloud::CommandExecutionUUID followupRequest;
        dispatchToHandler(ctx, followupRequest, sink, handler, chain_, fqi_, resp);
        return sink.status();
    }

private:
    const InterceptorChain* chain_;
    std::string fqi_;
};

// Boots a real gRPC server on an ephemeral loopback port and connects a stub.
struct GrpcMetadataGateHarnessServer {
    GrpcMetadataGateHarnessServer(const InterceptorChain* chain, std::string fqi)
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

    ~GrpcMetadataGateHarnessServer() {
        if (server) server->Shutdown();
    }

    grpc::Status call(const std::string* headerKey = nullptr,
                      const std::string* headerValue = nullptr) {
        grpc::ClientContext ctx;
        if (headerKey) ctx.AddMetadata(*headerKey, *headerValue);
        LockServer_Parameters req;
        LockServer_Responses resp;
        return stub->LockServer(&ctx, req, &resp);
    }

    grpc::Status callFollowup(const std::string* headerKey = nullptr,
                              const std::string* headerValue = nullptr) {
        grpc::ClientContext ctx;
        if (headerKey) ctx.AddMetadata(*headerKey, *headerValue);
        Get_IsLocked_Parameters req;
        Get_IsLocked_Responses resp;
        return stub->Get_IsLocked(&ctx, req, &resp);
    }

    GrpcMetadataGateHarnessService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<LockController::Stub> stub;
};

// ---------------------------------------------------------------------------
// Cloud-side fixture
// ---------------------------------------------------------------------------

class MetadataGateDualTransport : public cloud_test::CloudRouterFixture {
protected:
    cloud::SiLAServerMessage dispatchCommand(sila2::CloudEnvelopeRouter& router,
                                             const std::string& fqi,
                                             const std::string& requestUuid,
                                             std::vector<cloud::Metadata> metadata = {}) {
        cloud::SiLAClientMessage msg;
        msg.set_requestuuid(requestUuid);
        auto* exec = msg.mutable_unobservablecommandexecution();
        exec->set_fullyqualifiedcommandid(fqi);
        for (auto& md : metadata) {
            *exec->mutable_commandparameter()->add_metadata() = std::move(md);
        }
        router.route(msg, *writer_, writer_, calls_);
        return popResponse();
    }
};

// Success handler: writes a recognizable response, so a positive test can
// tell "the handler ran" from "the gate silently swallowed the call".
void echoCommandHandler(const std::string&, sila2::CallContext&,
                        sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
    cloud::SiLAServerMessage resp;
    resp.set_requestuuid(requestUUID);
    resp.mutable_unobservablecommandresponse()->set_response("ok");
    w.write(resp);
}

// Observable-command _Result handler for the follow-up positive case.
void echoObservableResultHandler(const std::string&, sila2::CallContext&,
                                 sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
    cloud::SiLAServerMessage resp;
    resp.set_requestuuid(requestUUID);
    resp.mutable_observablecommandresponse()->set_response("result-data");
    w.write(resp);
}

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST_F(MetadataGateDualTransport, UndeclaredMetadataIsAcceptedOnBothTransports) {
    InterceptorChain chain;  // empty table -- nothing declared
    GrpcMetadataGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const std::string headerKey = metadataHeaderKey(kOtherMetaFqi);
    const std::string headerValue = "undeclared-value";
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &headerValue);
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandFqi, "req-1",
                                      {makeMetadata(kOtherMetaFqi, "undeclared-value")});
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    EXPECT_EQ(resp.unobservablecommandresponse().response(), "ok");
}

TEST_F(MetadataGateDualTransport, SiLAServiceCallWithoutMetadataSucceedsOnBothTransports) {
    InterceptorChain chain;
    GrpcMetadataGateHarnessServer grpcServer{&chain, kSiLAServiceFqi};
    const grpc::Status grpcStatus = grpcServer.call();
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kGetFeatureDefinitionFqi, echoCommandHandler);
    const auto resp = dispatchCommand(router, kGetFeatureDefinitionFqi, "req-2");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

TEST_F(MetadataGateDualTransport, AffectedCallWithTheRequiredMetadataPresentSucceedsOnBothTransports) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};
    GrpcMetadataGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const std::string headerKey = metadataHeaderKey(kMetaFqi);
    const std::string headerValue = "thing-value";
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &headerValue);
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandFqi, "req-3",
                                      {makeMetadata(kMetaFqi, "thing-value")});
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

TEST_F(MetadataGateDualTransport, ObservableFollowupServesWithoutMetadataOnBothTransports) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};  // declared, but never on a follow-up

    // The follow-up owner gate (fail-closed) also runs on this stand-in: a real
    // follow-up always follows a gRPC initiation that registered its UUID. The
    // stand-in carries a default (empty) CommandExecutionUUID, so register that
    // as owned by kFeatureFqi -- otherwise the owner gate rejects before the
    // metadata exemption under test is reached.
    chain.registerObservableOwner("", kFeatureFqi, std::nullopt);

    // Without GrpcTransport.h's `if constexpr` exemption this would be
    // ABORTED/INVALID_METADATA -- Get_IsLocked's Req is CommandExecutionUUID.
    GrpcMetadataGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.callFollowup();  // no header
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    router.registerCommandHandler(kCommandFqi + "_Result", echoObservableResultHandler);
    router.registerExecutionFQI(exec->uuid(), kCommandFqi);  // no access token needed, no chain->auth

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-4");
    // Follow-up envelopes carry no metadata field on the wire
    // (SiLACloudConnector.proto:79-87) -- proven by never setting one here.
    msg.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_observablecommandresponse());
    EXPECT_FALSE(resp.has_commanderror());
}

TEST_F(MetadataGateDualTransport, UnaffectedCallWithoutTheDeclaredMetadataSucceedsOnBothTransports) {
    InterceptorChain chain;
    const std::string kOtherFeatureFqi = "org.test/OtherFeature/v1";
    chain.metadataAffectedCalls[kMetaFqi] = {kOtherFeatureFqi};  // does not cover kFeatureFqi

    GrpcMetadataGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call();  // no header
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandFqi, "req-5");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

TEST_F(MetadataGateDualTransport, MetadataOnASiLAServiceLookalikeFeatureIsAccepted) {
    InterceptorChain chain;  // no declarations needed to isolate the boundary rule
    GrpcMetadataGateHarnessServer grpcServer{&chain, kSiLAServiceLookalikeFqi};
    const std::string headerKey = metadataHeaderKey(kOtherMetaFqi);
    const std::string headerValue = "anything";
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &headerValue);
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kSiLAServiceLookalikeCommandFqi, echoCommandHandler);
    const auto resp = dispatchCommand(router, kSiLAServiceLookalikeCommandFqi, "req-6",
                                      {makeMetadata(kOtherMetaFqi, "anything")});
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT
// ---------------------------------------------------------------------------

TEST_F(MetadataGateDualTransport, SiLAServiceCallWithMetadataIsNoMetadataAllowedOnBothTransports) {
    InterceptorChain chain;
    GrpcMetadataGateHarnessServer grpcServer{&chain, kSiLAServiceFqi};
    const std::string headerKey = metadataHeaderKey(kOtherMetaFqi);
    const std::string headerValue = "anything";
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &headerValue);
    ASSERT_FALSE(grpcStatus.ok());
    EXPECT_EQ(grpcStatus.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    // Also proves the throw lands inside guardHandler and reaches sink.fail():
    // a ABORTED status only appears here via SilaError::toStatus().
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::NoMetadataAllowed);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    std::atomic<int> handlerRunCount{0};
    router.registerCommandHandler(kGetFeatureDefinitionFqi,
        [&handlerRunCount](const std::string&, sila2::CallContext&,
                           sila2::StreamWriteSerializer&, const std::string&) {
            handlerRunCount.fetch_add(1);
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kGetFeatureDefinitionFqi, "req-7",
                                      {makeMetadata(kOtherMetaFqi, "anything")});
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::NO_METADATA_ALLOWED);
    EXPECT_EQ(handlerRunCount.load(), 0);
}

// Cloud-only: a command-granular SiLAService target, which gRPC can never
// present (every generated adapter hands intercept() the feature-level FQI).
// Pins fqiCovers (prefix + "/" boundary) rather than exact equality behind
// the SiLAService carve-out.
TEST_F(MetadataGateDualTransport, SiLAServiceCommandGranularityIsRejectedOnCloud) {
    InterceptorChain chain;
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kSetServerNameFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kSetServerNameFqi, "req-8",
                                      {makeMetadata(kOtherMetaFqi, "anything")});
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::NO_METADATA_ALLOWED);
}

TEST_F(MetadataGateDualTransport, AffectedCallMissingRequiredMetadataIsInvalidMetadataOnBothTransports) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};
    GrpcMetadataGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call();  // no header at all
    ASSERT_FALSE(grpcStatus.ok());
    EXPECT_EQ(grpcStatus.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
    EXPECT_NE(std::string{grpcErr->what()}.find(kMetaFqi), std::string::npos);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kCommandFqi, "req-9");  // no metadata
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_NE(resp.commanderror().frameworkerror().message().find(kMetaFqi), std::string::npos);
}

// Separates check (c) from the forbidden branch (b): sending SOME metadata
// (just not the declared one) must still be reported as the declared FQI
// missing, never as a complaint about the unrelated key that was sent.
TEST_F(MetadataGateDualTransport, WrongMetadataDoesNotSatisfyTheDeclarationOnBothTransports) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};
    GrpcMetadataGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const std::string headerKey = metadataHeaderKey(kOtherMetaFqi);
    const std::string headerValue = "not-the-declared-one";
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &headerValue);
    ASSERT_FALSE(grpcStatus.ok());
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
    EXPECT_NE(std::string{grpcErr->what()}.find(kMetaFqi), std::string::npos);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kCommandFqi, "req-10",
                                      {makeMetadata(kOtherMetaFqi, "not-the-declared-one")});
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_NE(resp.commanderror().frameworkerror().message().find(kMetaFqi), std::string::npos);
}

// gRPC-only: pins the header-key derivation (MetadataHeaderKey.h). A raw FQI
// sent under any key form other than metadataHeaderKey()'s exact output is
// still "missing" from the gate's point of view -- there is no fallback
// lookup, deliberately: a fallback would let a client guess its way past (c).
TEST_F(MetadataGateDualTransport, RequiredMetadataUnderTheWrongKeyFormIsStillMissingOnGrpc) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};
    GrpcMetadataGateHarnessServer grpcServer{&chain, kFeatureFqi};

    const std::string correctKey = metadataHeaderKey(kMetaFqi);
    ASSERT_TRUE(correctKey.ends_with("-bin"));
    const std::string wrongKey = correctKey.substr(0, correctKey.size() - 4);  // drop "-bin"
    const std::string headerValue = "thing-value";
    const grpc::Status grpcStatus = grpcServer.call(&wrongKey, &headerValue);

    ASSERT_FALSE(grpcStatus.ok());
    EXPECT_EQ(grpcStatus.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
}

TEST_F(MetadataGateDualTransport, MetadataRejectionLogsUnderTheMetadataTagNotAuth) {
    InterceptorChain chain;  // no chain.auth installed
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};
    bool sawMetadataTag = false;
    bool sawAuthTag = false;
    chain.logCallback = [&](sila2::LogLevel level, std::string_view category,
                            std::string_view message) {
        if (category == "metadata") {
            sawMetadataTag = level == sila2::LogLevel::kWarning
                           && message == "rejected: " + kCommandFqi;
        }
        if (category == "auth") {
            sawAuthTag = true;
        }
    };

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the metadata gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kCommandFqi, "req-11");  // no metadata

    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
    EXPECT_TRUE(sawMetadataTag);
    EXPECT_FALSE(sawAuthTag);
}


// ---------------------------------------------------------------------------
// Cloud CreateBinary seam (review SC13 nonblocking #1)
// ---------------------------------------------------------------------------

// The refusal arrives as a BinaryTransferError, not a SilaError envelope:
// this oneof has no commandError/propertyError arm, so INVALID_METADATA is
// degraded the same way the auth gate on this branch degrades its verdict.
TEST_F(MetadataGateDualTransport, CreateBinaryMissingRequiredMetadataIsRejectedOnCloud) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::CloudEnvelopeRouter router{registry, &chain, &binaryStore};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-bin-1");
    auto* createReq = msg.mutable_createbinaryuploadrequest()->mutable_createbinaryrequest();
    createReq->set_binarysize(10);
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier(kCommandFqi + "/Parameter/Blob");
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_UPLOAD_FAILED);
    EXPECT_NE(resp.binarytransfererror().message().find(kMetaFqi), std::string::npos);
    EXPECT_EQ(binaryStore.size(), 0u);  // refused before a slot was created
}

TEST_F(MetadataGateDualTransport, CreateBinaryWithRequiredMetadataPresentCreatesSlotOnCloud) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kMetaFqi] = {kFeatureFqi};
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::CloudEnvelopeRouter router{registry, &chain, &binaryStore};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-bin-2");
    auto* uploadReq = msg.mutable_createbinaryuploadrequest();
    *uploadReq->add_metadata() = makeMetadata(kMetaFqi, "value");
    auto* createReq = uploadReq->mutable_createbinaryrequest();
    createReq->set_binarysize(10);
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier(kCommandFqi + "/Parameter/Blob");
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_createbinaryresponse());
    EXPECT_FALSE(resp.createbinaryresponse().binarytransferuuid().empty());
    EXPECT_EQ(binaryStore.size(), 1u);
}

// Rule (b) on this seam: undeclared metadata attached to an upload must be
// ignored, never rejected.
TEST_F(MetadataGateDualTransport, CreateBinaryWithUndeclaredMetadataIsAcceptedOnCloud) {
    InterceptorChain chain;  // nothing declared
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::CloudEnvelopeRouter router{registry, &chain, &binaryStore};

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-bin-3");
    auto* uploadReq = msg.mutable_createbinaryuploadrequest();
    *uploadReq->add_metadata() = makeMetadata(kOtherMetaFqi, "whatever");
    auto* createReq = uploadReq->mutable_createbinaryrequest();
    createReq->set_binarysize(10);
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier(kCommandFqi + "/Parameter/Blob");
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_createbinaryresponse());
    EXPECT_EQ(binaryStore.size(), 1u);
}

}  // namespace
