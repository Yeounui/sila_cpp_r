// MetadataHeaderKey.h — SiLA Client Metadata gRPC header key derivation (§3.6)
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace sila2 {

/// Derives the gRPC binary header key that carries the SiLA Client Metadata
/// named by metadataFqi, e.g.
///   org.silastandard/core/LockController/v1/Metadata/LockIdentifier
///   -> sila-org.silastandard-core-lockcontroller-v1-metadata-lockidentifier-bin
/// Only '/' is a separator. The originator's dots belong to the identifier and
/// stay: gRPC accepts '.' in a header key (validate_metadata.cc's
/// LegalHeaderKeyBits sets '-', '_' and '.'), and a normative server restores
/// '-' to '/' and matches the result against an FQI grammar whose originator
/// segment is dotted -- a dot-folded key fails that match and is dropped, not
/// mis-mapped. Lives here rather than on MetadataInjector so the server can
/// derive the same key without a server TU including a client header.
inline std::string metadataHeaderKey(const std::string& metadataFqi) {
    std::string key = metadataFqi;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        if (c == '/') {
            return '-';
        }
        return static_cast<char>(std::tolower(c));
    });
    // "-bin" marks this as a binary-valued gRPC metadata header.
    return "sila-" + key + "-bin";
}

/// The standard AccessToken metadata FQI (AuthorizationService feature).
/// Spelled once for everything that handles the header: the client injects
/// under it (SilaClientBase), the server extracts it
/// (MetadataExtractingInterceptor), the cloud router normalizes envelope
/// metadata to it, and SiLAServerBase advertises it for FCP discovery.
/// Tests keep their own literals on purpose — they pin the wire truth.
inline const std::string kAccessTokenMetadataFqi =
    "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken";

/// The standard LockIdentifier metadata FQI (LockController Feature).
/// Spelled once for everything that handles the header, exactly as the access
/// token above: the client injects under it (SilaClientBase), the gate parses
/// it (LockControllerImpl::checkLockMetadata), SiLAServerBase advertises it for
/// FCP discovery, and MetadataPolicy skips it because that gate owns both its
/// presence and its value. Lives here rather than in LockControllerImpl.h
/// because MetadataPolicy.h needs it and is included by GrpcTransport.h, i.e.
/// by every generated service adapter -- LockController.grpc.pb.h must not
/// follow it in.
inline const std::string kLockIdentifierMetadataFqi =
    "org.silastandard/core/LockController/v1/Metadata/LockIdentifier";

}  // namespace sila2
