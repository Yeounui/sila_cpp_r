// Tests for LockControllerImpl: lock lifecycle (Lock → Unlock → Get_IsLocked),
// lock expiry auto-clear, FCPAffectedByMetadata through the InterceptorChain
// table (S32), the LockIdentifier metadata VALUE gate (S33), and its
// inactivity-timeout renewal on a valid gate pass (S34).
//
// KNOWN UNCAUGHT GAP: the lock is not enforced on the BinaryUpload/
// BinaryDownload RPCs, because those are registerService'd without a
// registerFeature (SiLAServerBase.cc), so they never appear in
// registeredFeatureFqis and cannot enter the affected list. Deliberate --
// putting them in would collide with open S26 (the fork's own
// BinaryUploader/BinaryDownloader build bare grpc::ClientContexts and cannot
// attach metadata at all).
#include <sila/server/features/LockControllerImpl.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/transport/InterceptorChain.h>

#include "LockController.grpc.pb.h"
#include "SiLAFramework.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
using sila2::InterceptorChain;
using sila2::LockControllerImpl;
using sila2::kAccessTokenMetadataFqi;
using sila2::kLockIdentifierMetadataFqi;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::FrameworkError;
using sila2::error::SiLAError;

namespace lockcontroller_proto = sila2::org::silastandard::core::lockcontroller::v1;

const std::string kLockId = "test-lock-id";
const std::string kFeatureFqi = "org.example/TestFeature/v1";
const std::string kOtherFeatureFqi = "org.example/OtherFeature/v1";
const std::string kLockControllerFqi = "org.silastandard/core/LockController/v1";
const std::string kSiLAServiceFeatureFqi = "org.silastandard/core/SiLAService/v1";

grpc::Status lockServer(LockControllerImpl& ctrl,
                         const std::string& lockId,
                         int64_t timeoutSeconds) {
    grpc::ServerContext ctx;
    lockcontroller_proto::LockServer_Parameters request;
    request.mutable_lockidentifier()->set_value(lockId);
    request.mutable_timeout()->set_value(timeoutSeconds);
    lockcontroller_proto::LockServer_Responses response;
    return ctrl.LockServer(&ctx, &request, &response);
}

grpc::Status unlockServer(LockControllerImpl& ctrl, const std::string& lockId) {
    grpc::ServerContext ctx;
    lockcontroller_proto::UnlockServer_Parameters request;
    request.mutable_lockidentifier()->set_value(lockId);
    lockcontroller_proto::UnlockServer_Responses response;
    return ctrl.UnlockServer(&ctx, &request, &response);
}

bool getIsLocked(LockControllerImpl& ctrl) {
    grpc::ServerContext ctx;
    lockcontroller_proto::Get_IsLocked_Parameters request;
    lockcontroller_proto::Get_IsLocked_Responses response;
    ctrl.Get_IsLocked(&ctx, &request, &response);
    return response.islocked().value();
}

// Serializes a Metadata_LockIdentifier (LockController.proto:94-99) the way a
// transport would after extracting the raw header value, and hands it
// straight to checkLockMetadata -- the gate under test, called directly
// rather than through a gRPC service method (checkLockMetadata is not one).
void sendLockMetadata(LockControllerImpl& ctrl,
                       const std::string& targetFqi,
                       const std::string& lockId) {
    lockcontroller_proto::Metadata_LockIdentifier metadata;
    metadata.mutable_lockidentifier()->set_value(lockId);
    std::string serialized;
    metadata.SerializeToString(&serialized);
    ctrl.checkLockMetadata(targetFqi, serialized);
}

// Builds a chain declaring `fqis` as affected by the LockIdentifier metadata
// -- both checkLockMetadata and Get_FCPAffectedByMetadata_LockIdentifier read
// this same table (S32/S33), so one helper seeds both kinds of test.
InterceptorChain chainWithLockAffecting(std::vector<std::string> fqis) {
    InterceptorChain chain;
    chain.metadataAffectedCalls[kLockIdentifierMetadataFqi] = std::move(fqis);
    return chain;
}

// ---------------------------------------------------------------------------
// Lock lifecycle — True paths
// ---------------------------------------------------------------------------

TEST(LockController, LockServerThenGetIsLockedReturnsTrue) {
    InterceptorChain chain;
    LockControllerImpl ctrl{&chain};

    ASSERT_TRUE(lockServer(ctrl, kLockId, 30).ok());
    EXPECT_TRUE(getIsLocked(ctrl));
}

TEST(LockController, UnlockServerWithCorrectIdSucceeds) {
    InterceptorChain chain;
    LockControllerImpl ctrl{&chain};

    ASSERT_TRUE(lockServer(ctrl, kLockId, 30).ok());
    ASSERT_TRUE(unlockServer(ctrl, kLockId).ok());
    EXPECT_FALSE(getIsLocked(ctrl));
}

TEST(LockController, LockWithZeroTimeoutNeverExpires) {
    InterceptorChain chain;
    LockControllerImpl ctrl{&chain};

    ASSERT_TRUE(lockServer(ctrl, kLockId, 0).ok());
    EXPECT_TRUE(getIsLocked(ctrl));
}

// ---------------------------------------------------------------------------
// FCPAffectedByMetadata_LockIdentifier — reads chain_->metadataAffectedCalls
// (S32). Rewritten from the FeatureRegistry-backed version: the property no
// longer has a registry to ask, so every case seeds the chain table instead.
// ---------------------------------------------------------------------------

TEST(LockController, FCPAffectedByMetadataListsOrdinaryFeatures) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi, kOtherFeatureFqi});
    LockControllerImpl ctrl{&chain};

    grpc::ServerContext ctx;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters request;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses response;

    ASSERT_TRUE(ctrl.Get_FCPAffectedByMetadata_LockIdentifier(&ctx, &request, &response).ok());
    ASSERT_EQ(response.affectedcalls_size(), 2);
    EXPECT_EQ(response.affectedcalls(0).value(), kFeatureFqi);
    EXPECT_EQ(response.affectedcalls(1).value(), kOtherFeatureFqi);
}

TEST(LockController, FCPAffectedByMetadataAnswersFromTheChainNotTheRegistry) {
    // The case that would have caught the two-transport divergence S32 fixes:
    // exactly one FQI declared, exactly one FQI answered -- containment alone
    // would not tell the two apart from a stale registry-derived list.
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};

    grpc::ServerContext ctx;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters request;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses response;

    ASSERT_TRUE(ctrl.Get_FCPAffectedByMetadata_LockIdentifier(&ctx, &request, &response).ok());
    ASSERT_EQ(response.affectedcalls_size(), 1);
    EXPECT_EQ(response.affectedcalls(0).value(), kFeatureFqi);
}

TEST(LockController, FCPAffectedByMetadataIsEmptyWhenNoLockRowIsDeclared) {
    InterceptorChain chain;  // metadataAffectedCalls has no LockIdentifier row.
    LockControllerImpl ctrl{&chain};

    grpc::ServerContext ctx;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters request;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses response;

    ASSERT_TRUE(ctrl.Get_FCPAffectedByMetadata_LockIdentifier(&ctx, &request, &response).ok());
    EXPECT_EQ(response.affectedcalls_size(), 0);
}

TEST(LockController, FCPAffectedByMetadataIsEmptyWithoutAChain) {
    // A null chain is the documented unit-test wiring (MetadataPolicy.h) --
    // the property must answer an empty list, not crash on a null deref.
    LockControllerImpl ctrl{nullptr};

    grpc::ServerContext ctx;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters request;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses response;

    ASSERT_TRUE(ctrl.Get_FCPAffectedByMetadata_LockIdentifier(&ctx, &request, &response).ok());
    EXPECT_EQ(response.affectedcalls_size(), 0);
}

TEST(LockController, FCPAffectedByMetadataIgnoresOtherMetadataRows) {
    // The lookup is keyed by kLockIdentifierMetadataFqi, not a flatten of the
    // whole metadataAffectedCalls table.
    InterceptorChain chain;
    chain.metadataAffectedCalls[kAccessTokenMetadataFqi] = {kFeatureFqi};
    LockControllerImpl ctrl{&chain};

    grpc::ServerContext ctx;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters request;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses response;

    ASSERT_TRUE(ctrl.Get_FCPAffectedByMetadata_LockIdentifier(&ctx, &request, &response).ok());
    EXPECT_EQ(response.affectedcalls_size(), 0);
}

TEST(LockController, FCPAffectedByMetadataExcludesSelfAndSiLAService) {
    // LockController-v1_0.sila.xml:95-99: IsLocked "MUST NOT be lock
    // protected" -- a Feature-granular LockController entry would cover it,
    // so LockController itself must never appear here. SiLAService is
    // excluded separately because it rejects all client metadata outright.
    // Both exclusions are Build()'s job now (S32); this case documents the
    // contract the property must never violate: a correctly-built row never
    // names either FQI.
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};

    grpc::ServerContext ctx;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters request;
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses response;

    ASSERT_TRUE(ctrl.Get_FCPAffectedByMetadata_LockIdentifier(&ctx, &request, &response).ok());
    for (const auto& affected : response.affectedcalls()) {
        EXPECT_NE(affected.value(), kLockControllerFqi);
        EXPECT_NE(affected.value(), kSiLAServiceFeatureFqi);
    }
}

// ---------------------------------------------------------------------------
// Lock lifecycle — False paths
// ---------------------------------------------------------------------------

TEST(LockController, LockServerWhenAlreadyLockedReturnsServerAlreadyLocked) {
    InterceptorChain chain;
    LockControllerImpl ctrl{&chain};

    ASSERT_TRUE(lockServer(ctrl, kLockId, 30).ok());
    const grpc::Status status = lockServer(ctrl, "another-id", 30);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(),
              "org.silastandard/core/LockController/v1/DefinedExecutionError/ServerAlreadyLocked");
}

TEST(LockController, UnlockServerWhenNotLockedReturnsServerNotLocked) {
    InterceptorChain chain;
    LockControllerImpl ctrl{&chain};

    const grpc::Status status = unlockServer(ctrl, kLockId);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(),
              "org.silastandard/core/LockController/v1/DefinedExecutionError/ServerNotLocked");
}

TEST(LockController, UnlockServerWithWrongIdReturnsInvalidLockIdentifier) {
    InterceptorChain chain;
    LockControllerImpl ctrl{&chain};

    ASSERT_TRUE(lockServer(ctrl, kLockId, 30).ok());
    const grpc::Status status = unlockServer(ctrl, "wrong-id");

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(),
              "org.silastandard/core/LockController/v1/DefinedExecutionError/InvalidLockIdentifier");
}

// ---------------------------------------------------------------------------
// checkLockMetadata — the LockIdentifier metadata VALUE gate (S33)
//
// Only the unit half: this file drives checkLockMetadata directly. The
// dual-transport e2e half (gRPC header + cloud envelope, both reaching this
// same gate through InterceptorChain::lockGate) lives in writer 2's test file.
// ---------------------------------------------------------------------------

TEST(LockController, MatchingLockIdentifierIsAdmitted) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 0).ok());

    EXPECT_NO_THROW(sendLockMetadata(ctrl, kFeatureFqi, kLockId));
}

TEST(LockController, UnlockedServerAdmitsACallCarryingALockIdentifier) {
    // Pins Q2 Option B in both directions -- a stale or absent identifier is
    // ignored, not refused, when there is no lock to protect anything.
    // LockController-v1_0.sila.xml:18: "After the timeout has expired or
    // after explicit unlock no lock identifier has to be sent any more."
    // This is the case that fails on Q2 Option A (unconditional presence).
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};

    EXPECT_NO_THROW(sendLockMetadata(ctrl, kFeatureFqi, "stale"));
    EXPECT_NO_THROW(ctrl.checkLockMetadata(kFeatureFqi, std::nullopt));
}

TEST(LockController, UnaffectedCallIsNotGatedWhileLocked) {
    // The gate consults the affected-calls table rather than gating every
    // call once any lock exists -- kOtherFeatureFqi is absent from it.
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 0).ok());

    EXPECT_NO_THROW(ctrl.checkLockMetadata(kOtherFeatureFqi, std::nullopt));
}

TEST(LockController, LockedServerRejectsMissingLockIdentifier) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 0).ok());

    EXPECT_THROW(
        {
            try {
                ctrl.checkLockMetadata(kFeatureFqi, std::nullopt);
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
                throw;
            }
        },
        FrameworkError);
}

TEST(LockController, LockedServerRejectsWrongLockIdentifier) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 0).ok());

    EXPECT_THROW(
        {
            try {
                sendLockMetadata(ctrl, kFeatureFqi, "wrong-id");
            } catch (const DefinedExecutionError& e) {
                // Literal, spelled out, so a typo in kInvalidLockIdentifierErrorId
                // cannot pass.
                EXPECT_EQ(e.errorIdentifier(),
                          "org.silastandard/core/LockController/v1/DefinedExecutionError/InvalidLockIdentifier");
                throw;
            }
        },
        DefinedExecutionError);
}

TEST(LockController, LockedServerRejectsUnparseableLockIdentifierAsInvalidMetadata) {
    // Pins the Part A wrong-data-type reading: unparseable bytes are Invalid
    // Metadata, NOT InvalidLockIdentifier, so a later "simplification" cannot
    // collapse the two errors.
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 0).ok());

    // Every byte has its continuation bit set, so protobuf parsing runs off
    // the end of the buffer looking for a complete varint tag -- not a valid
    // serialized Metadata_LockIdentifier.
    const std::string garbage = "\xFF\xFF\xFF";
    EXPECT_THROW(
        {
            try {
                ctrl.checkLockMetadata(kFeatureFqi, garbage);
            } catch (const FrameworkError& e) {
                EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
                throw;
            }
        },
        FrameworkError);
}

// ---------------------------------------------------------------------------
// checkLockMetadata — inactivity-timeout renewal (S34)
// ---------------------------------------------------------------------------

TEST(LockController, ValidLockMetadataRenewsTheTimeout) {
    // ponytail: wall-clock test, inject a now() function if this flakes
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 1).ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    sendLockMetadata(ctrl, kFeatureFqi, kLockId);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    // Without renewal the lock is 1.4s old against a 1s timeout and would
    // already have expired.
    EXPECT_TRUE(getIsLocked(ctrl));
}

TEST(LockController, RenewalGrantsTheFullOriginalTimeoutNotADelta) {
    // Pins that renew_() recomputes lockExpiry_ from timeout_ rather than
    // extending it by the elapsed slice -- two renewals, each granting the
    // full 1s again, outlast three 700ms steps (2.1s total).
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 1).ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    sendLockMetadata(ctrl, kFeatureFqi, kLockId);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    sendLockMetadata(ctrl, kFeatureFqi, kLockId);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    EXPECT_TRUE(getIsLocked(ctrl));
}

TEST(LockController, ZeroTimeoutStaysInfiniteAfterRenewal) {
    // A naive `lastActivity_ + timeout_` would set lockExpiry_ to now() for a
    // zero timeout and expire the lock immediately -- pins the max() sentinel
    // branch survives renew_().
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 0).ok());

    sendLockMetadata(ctrl, kFeatureFqi, kLockId);
    EXPECT_TRUE(getIsLocked(ctrl));
    sendLockMetadata(ctrl, kFeatureFqi, kLockId);
    EXPECT_TRUE(getIsLocked(ctrl));
}

TEST(LockController, MismatchedIdentifierDoesNotRenew) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 1).ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_THROW(sendLockMetadata(ctrl, kFeatureFqi, "wrong-id"), DefinedExecutionError);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    // The failed call must not have extended anything.
    EXPECT_FALSE(getIsLocked(ctrl));
}

TEST(LockController, MissingLockMetadataDoesNotRenew) {
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 1).ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_THROW(ctrl.checkLockMetadata(kFeatureFqi, std::nullopt), FrameworkError);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    EXPECT_FALSE(getIsLocked(ctrl));
}

TEST(LockController, RenewalAfterExpiryDoesNotResurrectTheLock) {
    // The expiry check precedes the match/renew inside checkLockMetadata, so
    // a correct identifier arriving one tick late cannot revive a lock the
    // FDL already auto-unlocked. An expired lock means the server is
    // unlocked, so the call is admitted (no throw), not refused.
    InterceptorChain chain = chainWithLockAffecting({kFeatureFqi});
    LockControllerImpl ctrl{&chain};
    ASSERT_TRUE(lockServer(ctrl, kLockId, 1).ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    EXPECT_NO_THROW(sendLockMetadata(ctrl, kFeatureFqi, kLockId));
    EXPECT_FALSE(getIsLocked(ctrl));
}

}  // namespace
