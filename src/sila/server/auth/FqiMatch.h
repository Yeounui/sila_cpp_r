// FqiMatch.h — FQI prefix matching for the access-token gate (§3.11)
//
// Both transports now name a call at Command/Property granularity: the gRPC
// path gates on "<feature>/Command/<Name>" / "<feature>/Property/<Name>" from
// the generated adapters, symmetric with the cloud path. Matching by coverage
// rather than exact set membership lets a feature-level protectedFqis entry
// still close every call under it (fqiCovers), while a Command/Property entry
// gates just that one call (§3.1s).
#pragma once

#include <sila/common/util/AsciiCase.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace sila2::auth {

/// True when `entry` authorizes `targetFqi`: the same
/// @ref gl_fully_qualified_identifier "Fully Qualified Identifier" (FQI), or `entry` is a
/// prefix of `targetFqi` that ends on a '/' segment boundary.
inline bool fqiCovers(std::string_view entry, std::string_view targetFqi) {
    // Part A p87: FQIs "MUST always be checked without taking lower and upper
    // case into account". The server registers canonical-case FQIs but a peer
    // may send a case variant, so both operands are lowered before the
    // equality/prefix/boundary test below.
    const std::string entryLower = util::asciiLower(entry);
    const std::string targetLower = util::asciiLower(targetFqi);
    if (entryLower == targetLower) {
        return true;
    }
    // The '/' is the whole point of the helper. A plain starts_with would let
    // ".../v1" cover ".../v10", which is a different feature version, and
    // ".../Feature1" cover ".../Feature10".
    const bool isPrefix = targetLower.starts_with(entryLower);
    const bool endsOnSegmentBoundary =
        targetLower.size() > entryLower.size() && targetLower[entryLower.size()] == '/';
    return isPrefix && endsOnSegmentBoundary;
}

/// True when any entry in `entries` covers `targetFqi`. Exists so the three
/// gate sites write the entry/target argument order once instead of three
/// times — reversing it silently inverts the gate.
template <typename Range>
bool anyFqiCovers(const Range& entries, std::string_view targetFqi) {
    return std::any_of(entries.begin(), entries.end(),
                       [targetFqi](const auto& entry) {
                           return fqiCovers(entry, targetFqi);
                       });
}

/// True when `entry` names a Metadata item -- its parent segment is "Metadata".
/// A Binary-typed Metadata FQI IS a real gate target on CreateBinary
/// (isKnownParameterFqi accepts "/Metadata/"), but UploadChunk/DeleteBinary gate
/// on the coarser BinaryUpload FQI, so a Metadata entry in protectedFqis would
/// enforce only partially and inconsistently. withAuthentication uses this to
/// refuse Metadata entries until that lifecycle is defined, while accepting
/// Command/Property.
inline bool isMetadataFqi(std::string_view entry) {
    const auto nameStart = entry.rfind('/');
    if (nameStart == std::string_view::npos) {
        return false;
    }
    return entry.substr(0, nameStart).ends_with("/Metadata");
}

/// True when `parameterIdentifier` names a Command Parameter or a Metadata
/// item under one of `featureFqis`.
///
/// SiLABinaryTransfer.proto:24 declares CreateBinaryRequest.parameterIdentifier
/// as a "fully qualified parameter identifier", and both transports hand that
/// string to AuthorizationInterceptor as the FQI to gate on. Accepting an
/// arbitrary string therefore lets a caller name an FQI that protectedFqis does
/// not cover and skip the gate entirely (AuthorizationInterceptor.cc:17-19).
/// Reuses fqiCovers so the segment-boundary rule (".../v1" must not cover
/// ".../v10") is written once.
inline bool isKnownParameterFqi(const std::vector<std::string>& featureFqis,
                                std::string_view parameterIdentifier) {
    // Stops a bare Feature FQI passing: fqiCovers returns true on exact
    // equality, and a Feature FQI is not a parameter identifier. Metadata is
    // accepted because FDL permits a Binary-typed Metadata item, whose
    // identifier is the Metadata FQI rather than a Command parameter FQI.
    const bool namesAnItem =
        parameterIdentifier.find("/Parameter/") != std::string_view::npos ||
        parameterIdentifier.find("/Metadata/") != std::string_view::npos;
    return namesAnItem && anyFqiCovers(featureFqis, parameterIdentifier);
}

}  // namespace sila2::auth
