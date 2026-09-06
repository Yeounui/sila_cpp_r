// MetadataPolicy.h — SiLA Client Metadata admission gate (architecture.md §3.6)
//
// Part A > "SiLA Client Metadata" states three rules. Two are enforced here:
//   (a) "every call of the SiLA Service Feature MUST NOT contain any SiLA
//       Client Metadata ... it MUST issue a No Metadata Allowed Error."
//   (c) "If expected SiLA Client Metadata is not received, an Invalid Metadata
//       Error MUST be issued. This MUST be checked before parameter
//       validation."
// The third is a rule about what NOT to write: metadata that no affected list
// names "the SiLA Server MUST ignore". There is deliberately no undeclared-key
// branch below. Rejecting an unknown sila-*-bin header is a spec violation,
// not hardening -- see test_metadata_policy.cc's
// UndeclaredMetadataOnAnOrdinaryCallIsIgnored, which exists to stop one being
// added back.
#pragma once

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/transport/InterceptorChain.h>

#include <string>
#include <string_view>

namespace sila2 {

// Spelled here rather than included from SiLAServiceImpl.h:26-27: that header
// pulls in SiLAService.grpc.pb.h, and this one is included by GrpcTransport.h,
// i.e. by every generated service adapter in the build.
inline constexpr std::string_view kSiLAServiceFeatureFqi =
    "org.silastandard/core/SiLAService/v1";

/// Runs the SiLA Client Metadata admission rules for one incoming call.
///
/// @param chain     May be null (unit-test wiring, or a server built without
///                  SiLAServerBase::Builder). The declared table is then empty
///                  and only rule (a) applies -- (a) is a property of the
///                  SiLAService Feature, not of the server's configuration.
/// @param targetFqi The call being dispatched. gRPC names it at Feature
///                  granularity, cloud as "<feature>/Command/<Name>"; fqiCovers
///                  absorbs both (FqiMatch.h).
/// @param anyMetadataReceived  True when the request carried ANY SiLA Client
///                  Metadata, declared or not. Only rule (a) reads it.
/// @param hasMetadata  Answers "did this call carry <metadata FQI>?". A
///                  predicate rather than a received-set argument because the
///                  derivation only runs forwards: metadataHeaderKey()
///                  lowercases, so a gRPC header key cannot be turned back into
///                  an FQI (MetadataHeaderKey.h:14-20). Called only after a
///                  declaration matches, so an unaffected call allocates
///                  nothing.
/// @throws error::FrameworkError{NoMetadataAllowed} on rule (a);
///         error::FrameworkError{InvalidMetadata} on rule (c). Both reach the
///         client as gRPC ABORTED + base64(SiLAError) via SiLAError::toStatus(),
///         which Part B requires -- no new transmission code.
template <typename HasMetadataFn>
void enforceMetadataPolicy(const InterceptorChain* chain,
                           std::string_view targetFqi,
                           bool anyMetadataReceived,
                           const HasMetadataFn& hasMetadata) {
    if (auth::fqiCovers(kSiLAServiceFeatureFqi, targetFqi)) {
        if (anyMetadataReceived) {
            throw error::FrameworkError{
                error::FrameworkError::FrameworkErrorType::NoMetadataAllowed,
                "Calls to the SiLAService Feature must not carry SiLA Client Metadata"};
        }
        // Returns instead of falling through to the loop: the affected list
        // "MUST NOT contain ... the SiLA Service Feature Identifier itself", so
        // a Feature that declares it anyway must not be able to make
        // SiLAService unanswerable. SiLAService-v1_0.sila.xml:9 requires every
        // server to implement it unconditionally.
        return;
    }
    if (!chain) {
        return;
    }
    // ponytail: no major-version equivalence. The 2025-01-27 Part A working
    // draft says two metadata whose FQMIs differ only in the feature's major
    // version must be interchangeable ("it MUST be sufficient that the client
    // only provides one version"); release v1.1 does not say so, and this fork
    // serves no multi-version metadata. Upgrade path: normalize the "/vN/"
    // segment out of both sides before comparing, once a server declares two
    // versions of one metadata.
    for (const auto& [metadataFqi, affectedCalls] : chain->metadataAffectedCalls) {
        if (metadataFqi == kAccessTokenMetadataFqi) {
            // AuthorizationInterceptor owns the access token end to end: it
            // checks presence AND validity and answers with a verdict a client
            // can act on (AuthorizationInterceptor.cc:22-39). Re-checking mere
            // presence here would shadow that with a weaker one, and would make
            // Login demand the token it issues. The row stays in the table
            // because FCP discovery has to report it.
            continue;
        }
        if (metadataFqi == kLockIdentifierMetadataFqi) {
            // Same arrangement as the access token above, one layer over:
            // InterceptorChain::lockGate owns this metadata's presence AND its
            // value, because whether it is required at all depends on runtime
            // state this function cannot see. LockController-v1_0.sila.xml:18 --
            // "After the timeout has expired or after explicit unlock no lock
            // identifier has to be sent any more" -- so demanding it here, from a
            // static list, would refuse every call to an UNLOCKED server. The row
            // stays in the table because FCP discovery has to report it.
            continue;
        }
        if (!auth::anyFqiCovers(affectedCalls, targetFqi)) {
            continue;
        }
        if (hasMetadata(metadataFqi)) {
            continue;
        }
        throw error::FrameworkError{
            error::FrameworkError::FrameworkErrorType::InvalidMetadata,
            "Missing required SiLA Client Metadata: " + metadataFqi};
    }
}

}  // namespace sila2
