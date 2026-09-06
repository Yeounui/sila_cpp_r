// test_lock_gate_dual_transport_e2e.cc — End-to-end tests for the
// LockIdentifier metadata VALUE gate (§S33) shared by the gRPC and cloud
// transports (InterceptorChain::lockGate, wired to a real LockControllerImpl
// exactly as SiLAServerBase::Builder::Build() wires it -- see
// chainWithLockAffecting below). Adapted from the sibling admission-gate e2e
// file (test_metadata_gate_dual_transport_e2e.cc): its GrpcMetadataGateHarness
// is reused (renamed), including the observable-follow-up stand-in whose
// dispatchToHandler Req is a bare CommandExecutionUUID.
//
// Caught/uncaught: every rejection case below is CAUGHT -- the gate detects
// and reports INVALID_METADATA / INVALID_LOCK_IDENTIFIER on both transports.
// Known UNCAUGHT gaps, both deliberate: the lock is never enforced on the
// BinaryUpload/BinaryDownload RPCs (registerService'd without a
// registerFeature, so they never enter registeredFeatureFqis -- open S26),
// and cloud follow-up envelopes are never lock-gated, by the same Part A
// design that exempts them from the presence gate (makeFollowupContext).
#include "CloudRouterTestHarness.h"

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/features/LockControllerImpl.h>
#include <sila/server/transport/GrpcTransport.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/cloud/CloudHandlerRegistration.h>

#include "LockController.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sila2::CallContext;
using sila2::GrpcUnaryResponseSink;
using sila2::InterceptorChain;
using sila2::LockControllerImpl;
using sila2::ResponseSink;
using sila2::kLockIdentifierMetadataFqi;
using sila2::metadataHeaderKey;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::FrameworkError;
using sila2::error::SiLAError;

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

const std::string kLockId = "test-lock-id";

// Feature-scoped FQI, standing in for an ordinary Feature the server has
// declared as affected by the LockIdentifier metadata.
const std::string kFeatureFqi = "org.test/LockGate/v1";
const std::string kCommandFqi = kFeatureFqi + "/Command/Do";

// Spelled literally, not via a header constant: pins the wire truth a real
// client would send, independent of which constant the implementation uses.
const std::string kSiLAServiceFqi = "org.silastandard/core/SiLAService/v1";
const std::string kGetFeatureDefinitionFqi = kSiLAServiceFqi + "/Command/GetFeatureDefinition";

const std::string kLockControllerFqi = "org.silastandard/core/LockController/v1";

cloud::Metadata makeMetadata(const std::string& fqi, const std::string& value) {
    cloud::Metadata md;
    md.set_fullyqualifiedmetadataid(fqi);
    md.set_value(value);
    return md;
}

// Builds a chain declaring `fqis` as affected by the LockIdentifier metadata
// -- the same table Build() populates in production (S32).
InterceptorChain chainWithLockAffecting(std::vector<std::string> fqis) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kLockIdentifierMetadataFqi] = std::move(fqis);
    return chain;
}

// Wires chain.lockGate to a real LockControllerImpl's checkLockMetadata, the
// same closure Build() installs (SiLAServerBase.cc) -- every test below pins
// the WIRING, not just the method, by going through this indirection rather
// than calling checkLockMetadata directly.
void wireLockGate(InterceptorChain& chain, LockControllerImpl& lockController) {
    chain.lockGate = [&lockController](std::string_view fqi,
                                       const std::optional<std::string>& serialized) {
        lockController.checkLockMetadata(fqi, serialized);
    };
}

// Locks the server directly through the real gRPC method (bare
// ServerContext, no headers -- LockController is never itself in the
// affected list, so this bypasses the gate under test rather than exercising
// it), mirroring test_lock_controller.cc's own lockServer() helper.
grpc::Status lockTheServer(LockControllerImpl& ctrl, const std::string& lockId,
                           int64_t timeoutSeconds) {
    grpc::ServerContext ctx;
    LockServer_Parameters request;
    request.mutable_lockidentifier()->set_value(lockId);
    request.mutable_timeout()->set_value(timeoutSeconds);
    LockServer_Responses response;
    return ctrl.LockServer(&ctx, &request, &response);
}

// Serializes a lock identifier as the Metadata_LockIdentifier message
// (LockController.proto), the shape both transports parse.
std::string serializeLockIdentifier(const std::string& lockId) {
    lockcontroller_proto::Metadata_LockIdentifier metadata;
    metadata.mutable_lockidentifier()->set_value(lockId);
    std::string serialized;
    metadata.SerializeToString(&serialized);
    return serialized;
}

// ---------------------------------------------------------------------------
// gRPC-side harness. Reuses LockController's generated grpc::Service purely
// as wire plumbing, as the sibling metadata-gate e2e file does for the same
// Feature -- the request/response shape of the two RPCs below is irrelevant;
// only which Req type dispatchToHandler is instantiated with matters, and
// fqi_ (not the RPC name) is what the gate checks.
// ---------------------------------------------------------------------------

class GrpcLockGateHarnessService final : public LockController::Service {
public:
    GrpcLockGateHarnessService(const InterceptorChain* chain, std::string fqi)
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

    // Observable-follow-up stand-in: dispatchToHandler is driven with
    // Req = CommandExecutionUUID, the type every real _Info/_Intermediate/
    // _Result adapter uses, so GrpcTransport.h's `if constexpr` exemption
    // (no gate at all on a follow-up) is exercised for real.
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
struct GrpcLockGateHarnessServer {
    GrpcLockGateHarnessServer(const InterceptorChain* chain, std::string fqi)
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

    ~GrpcLockGateHarnessServer() {
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

    GrpcLockGateHarnessService service;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<LockController::Stub> stub;
};

// ---------------------------------------------------------------------------
// Cloud-side fixture
// ---------------------------------------------------------------------------

class LockGateDualTransport : public cloud_test::CloudRouterFixture {
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

TEST_F(LockGateDualTransport, UnlockedServerAcceptsACallWithNoLockMetadataOnBothTransports) {
    // The case that fails under Q2 Option A (unconditional presence) -- no
    // lock exists, so no metadata is required at all.
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);

    GrpcLockGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call();  // no header, no lock
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandFqi, "req-1");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

TEST_F(LockGateDualTransport, LockedServerAcceptsTheMatchingLockIdentifierOnBothTransports) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);
    ASSERT_TRUE(lockTheServer(lockController, kLockId, 0).ok());

    GrpcLockGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const std::string headerKey = metadataHeaderKey(kLockIdentifierMetadataFqi);
    const std::string headerValue = serializeLockIdentifier(kLockId);
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &headerValue);
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi, echoCommandHandler);
    const auto resp = dispatchCommand(router, kCommandFqi, "req-2",
                                      {makeMetadata(kLockIdentifierMetadataFqi,
                                                    serializeLockIdentifier(kLockId))});
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
}

TEST_F(LockGateDualTransport, ObservableCommandFollowupNeedsNoLockMetadataWhileLocked) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);
    ASSERT_TRUE(lockTheServer(lockController, kLockId, 0).ok());

    // The follow-up owner gate (fail-closed) also runs on this stand-in: a real
    // follow-up always follows a gRPC initiation that registered its UUID. The
    // stand-in carries a default (empty) CommandExecutionUUID, so register that
    // as owned by kFeatureFqi -- otherwise the owner gate rejects before the
    // lock exemption under test is reached.
    chain.registerObservableOwner("", kFeatureFqi, std::nullopt);

    // Without GrpcTransport.h's `if constexpr` exemption this would be
    // ABORTED/INVALID_METADATA -- Get_IsLocked's stand-in Req is
    // CommandExecutionUUID.
    GrpcLockGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.callFollowup();  // no header
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();

    sila2::FeatureRegistry registry;
    sila2::ObservableCommandManager mgr;
    auto exec = mgr.addCommand(std::chrono::seconds{300});
    exec->start();

    sila2::CloudEnvelopeRouter router{registry, &chain, nullptr, {&mgr}};
    router.registerCommandHandler(kCommandFqi + "_Result", echoObservableResultHandler);
    router.registerExecutionFQI(exec->uuid(), kCommandFqi);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-3");
    // Follow-up envelopes carry no metadata field on the wire
    // (SiLACloudConnector.proto) -- proven by never setting one here, and by
    // makeFollowupContext never calling admitMetadata at all, so chain.lockGate
    // is never even invoked on this path.
    msg.mutable_observablecommandgetresponse()->mutable_commandexecutionuuid()->set_value(exec->uuid());
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_observablecommandresponse());
    EXPECT_FALSE(resp.has_commanderror());
}

TEST_F(LockGateDualTransport, LockControllerOwnCallsAreNotGatedWhileLocked) {
    // LockController-v1_0.sila.xml: IsLocked "MUST NOT be lock protected, so
    // that any SiLA Client can query the current lock state" -- driven end to
    // end through the REAL property, not a stand-in.
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});  // never covers LockController itself
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);
    ASSERT_TRUE(lockTheServer(lockController, kLockId, 0).ok());

    grpc::ServerContext grpcCtx;
    Get_IsLocked_Parameters grpcReq;
    Get_IsLocked_Responses grpcResp;
    const grpc::Status grpcStatus = lockController.Get_IsLocked(&grpcCtx, &grpcReq, &grpcResp);
    EXPECT_TRUE(grpcStatus.ok()) << grpcStatus.error_message();
    EXPECT_TRUE(grpcResp.islocked().value());

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    // Non-owning shared_ptr (no-op deleter): regProp needs shared ownership,
    // and lockController's lifetime is this test body, which outlives router.
    std::shared_ptr<LockControllerImpl> lockControllerAlias{&lockController,
                                                            [](LockControllerImpl*) {}};
    sila2::regProp(router, kLockControllerFqi, "IsLocked", lockControllerAlias,
                   &LockControllerImpl::getIsLocked);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-4");
    msg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(
        kLockControllerFqi + "/Property/IsLocked");
    router.route(msg, *writer_, writer_, calls_);
    const auto resp = popResponse();

    ASSERT_TRUE(resp.has_unobservablepropertyvalue());
    Get_IsLocked_Responses decoded;
    ASSERT_TRUE(decoded.ParseFromString(resp.unobservablepropertyvalue().value()));
    EXPECT_TRUE(decoded.islocked().value());
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT
// ---------------------------------------------------------------------------

TEST_F(LockGateDualTransport, LockedServerRejectsAMissingLockIdentifierWithInvalidMetadataOnBothTransports) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);
    ASSERT_TRUE(lockTheServer(lockController, kLockId, 0).ok());

    GrpcLockGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const grpc::Status grpcStatus = grpcServer.call();  // no header at all
    ASSERT_FALSE(grpcStatus.ok());
    EXPECT_EQ(grpcStatus.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the lock gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kCommandFqi, "req-5");  // no metadata
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

TEST_F(LockGateDualTransport, LockedServerRejectsAWrongLockIdentifierWithInvalidLockIdentifierOnBothTransports) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);
    ASSERT_TRUE(lockTheServer(lockController, kLockId, 0).ok());

    GrpcLockGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const std::string headerKey = metadataHeaderKey(kLockIdentifierMetadataFqi);
    const std::string headerValue = serializeLockIdentifier("wrong-id");
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &headerValue);
    ASSERT_FALSE(grpcStatus.ok());
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* grpcErr = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    // This is the case that would catch a gate that raised ServerNotLocked
    // instead (architecture-v2.md:440's superseded reading).
    EXPECT_EQ(grpcErr->errorIdentifier(),
              "org.silastandard/core/LockController/v1/DefinedExecutionError/InvalidLockIdentifier");

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the lock gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kCommandFqi, "req-6",
                                      {makeMetadata(kLockIdentifierMetadataFqi,
                                                    serializeLockIdentifier("wrong-id"))});
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_definedexecutionerror());
    EXPECT_EQ(resp.commanderror().definedexecutionerror().erroridentifier(),
              "org.silastandard/core/LockController/v1/DefinedExecutionError/InvalidLockIdentifier");
}

TEST_F(LockGateDualTransport, LockedServerRejectsUnparseableLockMetadataAsInvalidMetadataOnBothTransports) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);
    ASSERT_TRUE(lockTheServer(lockController, kLockId, 0).ok());

    // Every byte has its continuation bit set, so protobuf varint parsing
    // runs off the end of the buffer -- not a valid serialized
    // Metadata_LockIdentifier (mirrors test_lock_controller.cc's unit case).
    const std::string garbage = "\xFF\xFF\xFF";

    GrpcLockGateHarnessServer grpcServer{&chain, kFeatureFqi};
    const std::string headerKey = metadataHeaderKey(kLockIdentifierMetadataFqi);
    const grpc::Status grpcStatus = grpcServer.call(&headerKey, &garbage);
    ASSERT_FALSE(grpcStatus.ok());
    const auto reconstructed = fromGrpcStatus(grpcStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    // NOT InvalidLockIdentifier: Part A routes a wrong SiLA Data Type to
    // Invalid Metadata, distinct from a value that parsed but did not match.
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kCommandFqi,
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            FAIL() << "handler must not run when the lock gate rejects the call";
        });
    const auto resp = dispatchCommand(router, kCommandFqi, "req-7",
                                      {makeMetadata(kLockIdentifierMetadataFqi, garbage)});
    ASSERT_TRUE(resp.has_commanderror());
    ASSERT_TRUE(resp.commanderror().has_frameworkerror());
    EXPECT_EQ(resp.commanderror().frameworkerror().errortype(), cloud::FrameworkError::INVALID_METADATA);
}

TEST_F(LockGateDualTransport, SiLAServiceStaysAnswerableWhileLocked) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl lockController{&chain};
    wireLockGate(chain, lockController);
    ASSERT_TRUE(lockTheServer(lockController, kLockId, 0).ok());

    // (1) No metadata at all: the lock row does not cover SiLAService (it is
    // never declared for it, per Part A and WithMetadata's own refusal), so
    // this must succeed exactly as an unlocked server would.
    GrpcLockGateHarnessServer grpcServer{&chain, kSiLAServiceFqi};
    const grpc::Status okStatus = grpcServer.call();
    EXPECT_TRUE(okStatus.ok()) << okStatus.error_message();

    // (2) The lock metadata attached anyway, even with the CORRECT value:
    // MetadataPolicy.h's SiLAService branch returns/throws before the loop
    // that carries the lock skip, so this must still be NO_METADATA_ALLOWED,
    // never routed into the lock gate.
    const std::string headerKey = metadataHeaderKey(kLockIdentifierMetadataFqi);
    const std::string headerValue = serializeLockIdentifier(kLockId);
    const grpc::Status rejectedStatus = grpcServer.call(&headerKey, &headerValue);
    ASSERT_FALSE(rejectedStatus.ok());
    const auto reconstructed = fromGrpcStatus(rejectedStatus);
    ASSERT_NE(reconstructed, nullptr);
    const auto* grpcErr = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(grpcErr, nullptr);
    EXPECT_EQ(grpcErr->frameworkErrorType(), FrameworkError::FrameworkErrorType::NoMetadataAllowed);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry, &chain};
    router.registerCommandHandler(kGetFeatureDefinitionFqi, echoCommandHandler);

    const auto okResp = dispatchCommand(router, kGetFeatureDefinitionFqi, "req-8a");
    ASSERT_TRUE(okResp.has_unobservablecommandresponse());

    const auto rejectedResp = dispatchCommand(
        router, kGetFeatureDefinitionFqi, "req-8b",
        {makeMetadata(kLockIdentifierMetadataFqi, serializeLockIdentifier(kLockId))});
    ASSERT_TRUE(rejectedResp.has_commanderror());
    ASSERT_TRUE(rejectedResp.commanderror().has_frameworkerror());
    EXPECT_EQ(rejectedResp.commanderror().frameworkerror().errortype(),
              cloud::FrameworkError::NO_METADATA_ALLOWED);
}

}  // namespace
