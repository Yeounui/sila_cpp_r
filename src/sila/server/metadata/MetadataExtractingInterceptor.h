// MetadataExtractingInterceptor.h — pulls SiLA Client Metadata out of
// raw transport headers into the call context (architecture.md §3.6)
//
// Classic-only minimal version. Header parsing (transport-specific) stays
// in the transport adapter; this interceptor only knows the "sila-" prefix
// convention, not any SiLA Client Metadata semantics.
#pragma once

#include <map>
#include <string>

namespace sila2 {

class CallContext;

/// Reads @ref gl_sila_client_metadata "SiLA Client Metadata" out of the raw
/// gRPC headers of one call and stores it on that call's CallContext, so a
/// Feature implementation and MetadataPolicy can look it up by FQI instead
/// of parsing headers themselves. Wired in automatically for every server
/// built with SilaServerBase::Builder -- a server author never calls it
/// directly.
class MetadataExtractingInterceptor {
public:
    /// Copies every header whose key starts with "sila-", plus the
    /// well-known "access-token" key, into ctx. Parsing the metadata
    /// values is left to the Feature implementation, except for the
    /// standard AccessToken header: its value is a serialized
    /// Metadata_AccessToken message, so it is unwrapped here into the
    /// normalized "access-token" key (a value that fails to parse is
    /// dropped, not passed through raw).
    static void extract(
        const std::multimap<std::string, std::string>& headers,
        CallContext& ctx);
};

}  // namespace sila2
