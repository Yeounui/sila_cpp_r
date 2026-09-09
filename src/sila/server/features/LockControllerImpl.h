// LockControllerImpl.h — SiLA2 core feature (architecture.md §3.10)
//
// New component, not a port. The LockController Feature lets a SiLA Client
// lock a SiLA Server for exclusive use via a lock identifier.
//
#pragma once

// Generated proto header provides the service base class and all message types.
// protoc output goes to CMAKE_CURRENT_BINARY_DIR which is on the include path.
#include "LockController.grpc.pb.h"

#include <sila/server/transport/SilaHandler.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace sila2 {

struct InterceptorChain;

// FQI constant for LockController — used by Builder::WithLock()/Build() to
// register the Feature (opt-in since audit S32; nothing auto-registers).
inline constexpr std::string_view kLockControllerFqi =
    "org.silastandard/core/LockController/v1";

// Per-RPC granular authorization FQIs. The direct-gRPC auth gate matches a
// call's FQI against protectedFqis; passing these Command/Property-level FQIs
// (not the feature-level one above) lets a command-level protectedFqis entry
// actually gate the matching RPC, keeping direct-gRPC coverage aligned with
// the cloud path. Full-literal string_view, not runtime concatenation, to stay
// constexpr and match kAuthorizationProviderParamFqi's existing style.
// Get_FCPAffectedByMetadata_LockIdentifier deliberately keeps the feature-level
// FQI: it is a metadata-discovery query, not a protectable Command/Property.
inline constexpr std::string_view kLockServerFqi =
    "org.silastandard/core/LockController/v1/Command/LockServer";
inline constexpr std::string_view kUnlockServerFqi =
    "org.silastandard/core/LockController/v1/Command/UnlockServer";
inline constexpr std::string_view kGet_IsLockedFqi =
    "org.silastandard/core/LockController/v1/Property/IsLocked";

// Returns the FDL XML for LockController, embedded as a string constant.
// ponytail: codegen will generate LockControllerMeta.cc with this constant
// (§2); until then, a raw string literal in the .cc file serves the same
// purpose.
const std::string& lockControllerFdlXml();

// Namespace alias shortens the generated proto namespace for readability.
namespace lockcontroller_proto = sila2::org::silastandard::core::lockcontroller::v1;

/// Implements the @ref gl_feature "Feature" `org.silastandard/core/LockController/v1`,
/// letting a @ref gl_sila_client "SiLA Client" take exclusive @ref gl_lock "lock"
/// of the server so other clients' calls are refused until it unlocks.
///
/// Installed by @ref SiLAServerBase::Builder::WithLock().
class LockControllerImpl final : public lockcontroller_proto::LockController::Service {
public:
    // chain outlives this object (owned by SiLAServerBase, which constructs
    // LockControllerImpl during Builder::Build()). Dropped the FeatureRegistry
    // parameter this constructor used to take: the FCP property below now
    // reads chain->metadataAffectedCalls instead of the registry (S32), so
    // registry_ has no reader left.
    explicit LockControllerImpl(const InterceptorChain* chain = nullptr);

    // ---- Commands ----

    /// Serves the LockServer command: locks the server under the given lock
    /// identifier and inactivity timeout. Dispatched by gRPC; a server author
    /// does not call this directly.
    /// @throws error::DefinedExecutionError{ServerAlreadyLocked} if a
    ///         non-expired lock is already held.
    grpc::Status LockServer(
        grpc::ServerContext* context,
        const lockcontroller_proto::LockServer_Parameters* request,
        lockcontroller_proto::LockServer_Responses* response) override;

    /// Serves the UnlockServer command: releases the current lock.
    /// @throws error::DefinedExecutionError{ServerNotLocked} if the server is
    ///         not locked; {InvalidLockIdentifier} if the given identifier
    ///         does not match the current lock.
    grpc::Status UnlockServer(
        grpc::ServerContext* context,
        const lockcontroller_proto::UnlockServer_Parameters* request,
        lockcontroller_proto::UnlockServer_Responses* response) override;

    // ---- Properties ----

    /// Serves the IsLocked property: whether the server currently has an
    /// active, non-expired lock.
    grpc::Status Get_IsLocked(
        grpc::ServerContext* context,
        const lockcontroller_proto::Get_IsLocked_Parameters* request,
        lockcontroller_proto::Get_IsLocked_Responses* response) override;

    /// Serves the FCPAffectedByMetadata_LockIdentifier query: lists the
    /// Commands and Properties that require the LockIdentifier
    /// @ref gl_sila_client_metadata "SiLA Client Metadata" while the server
    /// is locked.
    grpc::Status Get_FCPAffectedByMetadata_LockIdentifier(
        grpc::ServerContext* context,
        const lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters* request,
        lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses* response) override;

    void lockServer(const lockcontroller_proto::LockServer_Parameters& request, CallContext& ctx,
                    ResponseSink<lockcontroller_proto::LockServer_Responses>& sink);
    void unlockServer(const lockcontroller_proto::UnlockServer_Parameters& request, CallContext& ctx,
                      ResponseSink<lockcontroller_proto::UnlockServer_Responses>& sink);
    void getIsLocked(const lockcontroller_proto::Get_IsLocked_Parameters& request, CallContext& ctx,
                     ResponseSink<lockcontroller_proto::Get_IsLocked_Responses>& sink);
    void getFcpAffectedByMetadataLockIdentifier(
        const lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Parameters& request,
        CallContext& ctx,
        ResponseSink<lockcontroller_proto::Get_FCPAffectedByMetadata_LockIdentifier_Responses>& sink);

    /// Validates the LockIdentifier metadata of ONE incoming call and, on a
    /// match, renews the lock's inactivity timeout (S34).
    ///
    /// Reached through InterceptorChain::lockGate, not called directly by any
    /// Feature: the lock is a metadata-axis consumer no Feature implementation
    /// participates in (architecture-v2.md §3.10), like the access token next
    /// to it.
    ///
    /// @param targetFqi  The call being dispatched, at whatever granularity
    ///        the transport names it. Feature-level on gRPC, "<feature>/Command/
    ///        <Name>" on cloud; anyFqiCovers absorbs both (FqiMatch.h).
    /// @param serializedLockIdentifier  The raw metadata value as it arrived
    ///        (a serialized Metadata_LockIdentifier, LockController.proto:94-99),
    ///        or nullopt when the call carried none. Deliberately raw rather
    ///        than pre-parsed: parsing here gives both transports one parse
    ///        site and one wrong-data-type verdict.
    /// @throws error::FrameworkError{InvalidMetadata} when the server is locked,
    ///         the call is affected, and the metadata is absent or does not
    ///         parse; error::DefinedExecutionError{InvalidLockIdentifier} when
    ///         it parses but does not match the current lock.
    void checkLockMetadata(std::string_view targetFqi,
                           const std::optional<std::string>& serializedLockIdentifier);

private:
    // Returns true if the lock has expired. Caller must hold mu_.
    bool isExpired_() const;

    // Clears all lock state. Caller must hold mu_.
    void clearLock_();

    // Recomputes lockExpiry_ from timeout_ against now(), for a request
    // carrying a valid (matching) lock identifier (S34). Caller must hold mu_.
    void renew_();

    // Guards all lock state below — LockServer/UnlockServer/Get_IsLocked
    // are invoked concurrently from different gRPC call threads.
    mutable std::mutex mu_;
    const InterceptorChain* chain_;
    std::optional<std::string> lockIdentifier_;
    // max() sentinel means "no lock active" without a separate bool flag,
    // matching CallContext's deadline_ convention.
    std::chrono::steady_clock::time_point lockExpiry_{
        std::chrono::steady_clock::time_point::max()};
    std::chrono::steady_clock::time_point lastActivity_;
    std::chrono::seconds timeout_{};
};

}  // namespace sila2
