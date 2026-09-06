// LockControllerImpl.cc — SiLA2 core feature (architecture.md §3.10)
#include "LockControllerImpl.h"

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/features/LockControllerFdl.h>
#include <sila/server/transport/GrpcTransport.h>
#include <sila/server/transport/InterceptorChain.h>

#include "SiLAFramework.pb.h"

#include <string>

namespace sila2 {
namespace {
const std::string kFdlXml = generated::kLockControllerFdlXml;

// FDL §DefinedExecutionError declares these three errors for LockController.
// ponytail: codegen will emit these as constants in LockControllerMeta (§2)
const std::string kServerAlreadyLockedErrorId =
    "org.silastandard/core/LockController/v1/DefinedExecutionError/ServerAlreadyLocked";
const std::string kInvalidLockIdentifierErrorId =
    "org.silastandard/core/LockController/v1/DefinedExecutionError/InvalidLockIdentifier";
const std::string kServerNotLockedErrorId =
    "org.silastandard/core/LockController/v1/DefinedExecutionError/ServerNotLocked";

}  // namespace

const std::string& lockControllerFdlXml() { return kFdlXml; }

LockControllerImpl::LockControllerImpl(const InterceptorChain* chain)
    : chain_{chain} {}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

grpc::Status LockControllerImpl::LockServer(
    grpc::ServerContext* context,
    const lockcontroller_proto::LockServer_Parameters* request,
    lockcontroller_proto::LockServer_Responses* response) {
    GrpcUnaryResponseSink<lockcontroller_proto::LockServer_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { lockServer(req, ctx, out); },
        chain_, kLockServerFqi, response);
    return sink.status();
}

void LockControllerImpl::lockServer(const lockcontroller_proto::LockServer_Parameters& request,
                                    CallContext&,
                                    ResponseSink<lockcontroller_proto::LockServer_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    if (lockIdentifier_.has_value() && !isExpired_()) {
        // FDL §DefinedExecutionErrors/ServerAlreadyLocked
        throw error::DefinedExecutionError{
            kServerAlreadyLockedErrorId,
            "Server is already locked"};
    }
    // A previous lock expired without an explicit UnlockServer call — clear
    // it before accepting the new lock (auto-unlock per FDL §Description).
    if (lockIdentifier_.has_value()) {
        clearLock_();
    }

    lockIdentifier_ = request.lockidentifier().value();
    timeout_ = std::chrono::seconds{request.timeout().value()};
    lastActivity_ = std::chrono::steady_clock::now();
    // Timeout of zero seconds specifies an infinite time (no timeout).
    lockExpiry_ = timeout_.count() == 0
        ? std::chrono::steady_clock::time_point::max()
        : lastActivity_ + timeout_;

    sink.send(lockcontroller_proto::LockServer_Responses{});
    sink.finish();
}

grpc::Status LockControllerImpl::UnlockServer(
    grpc::ServerContext* context,
    const lockcontroller_proto::UnlockServer_Parameters* request,
    lockcontroller_proto::UnlockServer_Responses* response) {
    GrpcUnaryResponseSink<lockcontroller_proto::UnlockServer_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { unlockServer(req, ctx, out); },
        chain_, kUnlockServerFqi, response);
    return sink.status();
}

void LockControllerImpl::unlockServer(const lockcontroller_proto::UnlockServer_Parameters& request,
                                      CallContext&,
                                      ResponseSink<lockcontroller_proto::UnlockServer_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    if (!lockIdentifier_.has_value() || isExpired_()) {
        if (lockIdentifier_.has_value()) {
            clearLock_();
        }
        // FDL §DefinedExecutionErrors/ServerNotLocked
        throw error::DefinedExecutionError{
            kServerNotLockedErrorId,
            "Server is not locked"};
    }

    if (request.lockidentifier().value() != *lockIdentifier_) {
        // FDL §DefinedExecutionErrors/InvalidLockIdentifier
        throw error::DefinedExecutionError{
            kInvalidLockIdentifierErrorId,
            "Lock identifier does not match the current lock"};
    }

    clearLock_();
    sink.send(lockcontroller_proto::UnlockServer_Responses{});
    sink.finish();
}

// ---------------------------------------------------------------------------
// Metadata gate (S33)
// ---------------------------------------------------------------------------

void LockControllerImpl::checkLockMetadata(
    std::string_view targetFqi,
    const std::optional<std::string>& serializedLockIdentifier) {
    std::lock_guard<std::mutex> lock{mu_};

    // An unlocked server protects nothing, so a lock identifier sent to one is
    // ignored rather than refused. Two reasons, and the FDL is the first:
    // "After the timeout has expired or after explicit unlock no lock
    // identifier has to be sent any more" (LockController-v1_0.sila.xml:18) --
    // the obligation is conditioned on the lock existing, not on the Feature
    // being registered. Second, SilaClientBase.cc attaches the identifier in
    // the client's constructor and never removes it, so refusing it after a
    // timeout would break this fork's own client on the call right after the
    // lock lapsed. Reaping here as well as in getIsLocked keeps the two paths
    // from disagreeing about whether the lock is still live.
    if (!lockIdentifier_.has_value()) {
        return;
    }
    if (isExpired_()) {
        clearLock_();
        return;
    }
    if (!chain_) {
        return;
    }
    // The affected list is the same table FCP discovery answers from, so a
    // call this server never advertised as lock protected is never gated --
    // Part A makes the client send exactly what the list names.
    const auto affected = chain_->metadataAffectedCalls.find(kLockIdentifierMetadataFqi);
    if (affected == chain_->metadataAffectedCalls.end()
        || !auth::anyFqiCovers(affected->second, targetFqi)) {
        return;
    }

    if (!serializedLockIdentifier) {
        // Framework error, not InvalidLockIdentifier: Part A assigns "a
        // required SiLA Client Metadata has not been sent" to the Invalid
        // Metadata Error, and the FDL's InvalidLockIdentifier describes a
        // value that WAS sent ("The sent lock identifier is not valid.").
        throw error::FrameworkError{
            error::FrameworkError::FrameworkErrorType::InvalidMetadata,
            "Server is locked; this call requires the LockIdentifier metadata"};
    }
    lockcontroller_proto::Metadata_LockIdentifier metadata;
    if (!metadata.ParseFromString(*serializedLockIdentifier)) {
        // Same clause: bytes that are not a Metadata_LockIdentifier are the
        // "wrong SiLA Data Type" case, which Part A also routes to Invalid
        // Metadata. Not passed through as a raw string -- the same reasoning
        // MetadataExtractingInterceptor applies to the access token.
        throw error::FrameworkError{
            error::FrameworkError::FrameworkErrorType::InvalidMetadata,
            "LockIdentifier metadata is not a serialized Metadata_LockIdentifier"};
    }
    if (metadata.lockidentifier().value() != *lockIdentifier_) {
        // The ONLY DefinedExecutionError this gate may raise. The FDL attaches
        // InvalidLockIdentifier to the Metadata element itself, which is what
        // licenses it on an arbitrary affected Command; ServerNotLocked is
        // declared under UnlockServer alone and would be an undeclared error
        // for every other call. sila_python's interceptor raises the same one
        // and only that one (lockcontroller_impl.py:79-82).
        throw error::DefinedExecutionError{
            kInvalidLockIdentifierErrorId,
            "Lock identifier does not match the current lock"};
    }
    renew_();  // S34
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

grpc::Status LockControllerImpl::Get_IsLocked(
    grpc::ServerContext* context,
    const lockcontroller_proto::Get_IsLocked_Parameters* request,
    lockcontroller_proto::Get_IsLocked_Responses* response) {
    GrpcUnaryResponseSink<lockcontroller_proto::Get_IsLocked_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getIsLocked(req, ctx, out); },
        chain_, kGet_IsLockedFqi, response);
    return sink.status();
}

void LockControllerImpl::getIsLocked(const lockcontroller_proto::Get_IsLocked_Parameters&,
                                     CallContext&,
                                     ResponseSink<lockcontroller_proto::Get_IsLocked_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    const bool wasLocked = lockIdentifier_.has_value();
    const bool locked = wasLocked && !isExpired_();
    if (wasLocked && !locked) {
        // Observing IsLocked is a convenient point to reap an expired lock.
        clearLock_();
    }

    lockcontroller_proto::Get_IsLocked_Responses response;
    response.mutable_islocked()->set_value(locked);
    sink.send(response);
    sink.finish();
}

grpc::Status LockControllerImpl::Get_FCPAffectedByMetadata_LockIdentifier(
    grpc::ServerContext* context,
    const lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters* request,
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses* response) {
    GrpcUnaryResponseSink<lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) {
            getFcpAffectedByMetadataLockIdentifier(req, ctx, out);
        },
        chain_, kLockControllerFqi, response);
    return sink.status();
}

void LockControllerImpl::getFcpAffectedByMetadataLockIdentifier(
    const lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters&, CallContext&,
    ResponseSink<lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses>& sink) {
    // The gate's table, verbatim -- the same source the cloud kMetadataRequest
    // branch already answers from (CloudEnvelopeRouter.cc's kMetadataRequest
    // case). Deriving it from the FeatureRegistry instead, as this function
    // used to, let the two transports advertise different affected calls for
    // one metadata, and Part A requires the client to send exactly what the
    // list names. It also makes the list a Build()-time snapshot, which is
    // what Part A's "MUST NOT change during the Lifetime of a SiLA Server"
    // asks for structurally rather than by convention. The two exclusions
    // this function used to apply here (LockController itself, because
    // IsLocked "MUST NOT be lock protected", LockController-v1_0.sila.xml:
    // 95-99 / v2_0.sila.xml:97; and SiLAService, which rejects all client
    // metadata outright) now live where the table is built, in
    // SiLAServerBase::Builder::Build().
    lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses response;
    if (chain_) {
        const auto it = chain_->metadataAffectedCalls.find(kLockIdentifierMetadataFqi);
        if (it != chain_->metadataAffectedCalls.end()) {
            for (const auto& fqi : it->second) {
                response.add_affectedcalls()->set_value(fqi);
            }
        }
    }
    sink.send(response);
    sink.finish();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool LockControllerImpl::isExpired_() const {
    // time_point::max() means no timeout — never expired.
    if (lockExpiry_ == std::chrono::steady_clock::time_point::max()) {
        return false;
    }
    return std::chrono::steady_clock::now() > lockExpiry_;
}

void LockControllerImpl::clearLock_() {
    lockIdentifier_.reset();
    lockExpiry_ = std::chrono::steady_clock::time_point::max();
    lastActivity_ = {};
    timeout_ = std::chrono::seconds{};
}

void LockControllerImpl::renew_() {
    // The FDL timeout is an INACTIVITY timeout: "the time after which the
    // SiLA Server will be automatically unlocked if no request with a valid
    // lock identifier has been received meanwhile"
    // (LockController-v1_0.sila.xml:16-18, :43-45). Recomputed from timeout_
    // rather than shifted by a delta so a renewal always grants the full
    // configured duration, whatever the previous expiry was.
    lastActivity_ = std::chrono::steady_clock::now();
    // Same max() sentinel LockServer uses: timeout zero is infinite, and
    // renewing must not turn an infinite lock into a finite one.
    lockExpiry_ = timeout_.count() == 0
        ? std::chrono::steady_clock::time_point::max()
        : lastActivity_ + timeout_;
}

}  // namespace sila2
