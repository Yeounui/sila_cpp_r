// AuthorizationServiceImpl.cc — AccessToken metadata scope (architecture.md §3.11)
#include "AuthorizationServiceImpl.h"

#include <sila/server/features/AuthorizationServiceFdl.h>
#include <sila/server/transport/GrpcTransport.h>

#include "SiLAFramework.pb.h"

#include <string>
#include <utility>
#include <vector>

namespace sila2 {
namespace {
const std::string kFdlXml = generated::kAuthorizationServiceFdlXml;

}  // namespace

const std::string& authorizationServiceFdlXml() { return kFdlXml; }

AuthorizationServiceImpl::AuthorizationServiceImpl(std::vector<std::string> protectedFqis,
                                                     const InterceptorChain* chain)
    : protectedFqis_{std::move(protectedFqis)}, chain_{chain} {}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

grpc::Status AuthorizationServiceImpl::Get_FCPAffectedByMetadata_AccessToken(
    grpc::ServerContext* context,
    const authz_proto::Get_FCPAffectedByMetadata_AccessToken_Parameters* request,
    authz_proto::Get_FCPAffectedByMetadata_AccessToken_Responses* response) {
    GrpcUnaryResponseSink<authz_proto::Get_FCPAffectedByMetadata_AccessToken_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) {
            getFcpAffectedByMetadataAccessToken(req, ctx, out);
        },
        chain_, kAuthorizationServiceFqi, response);
    return sink.status();
}

void AuthorizationServiceImpl::getFcpAffectedByMetadataAccessToken(
    const authz_proto::Get_FCPAffectedByMetadata_AccessToken_Parameters&, CallContext&,
    ResponseSink<authz_proto::Get_FCPAffectedByMetadata_AccessToken_Responses>& sink) {
    // Reports the explicit protected list, not AccessPolicy::allowedFqis().
    // Same reason the gate is derived from that list and not from the policy
    // (SiLAServerBase.cc: "a permissive policy must not disable the auth
    // gate"): the policy answers per user, and discovery is anonymous.
    // DenyByDefaultAccessPolicy returns {} for an empty user, so asking it
    // here made this property report "no call needs a token". A Feature FQI
    // entry stands for every call under it, so the list is reported verbatim
    // — nothing to expand here.
    authz_proto::Get_FCPAffectedByMetadata_AccessToken_Responses response;
    for (const auto& fqi : protectedFqis_) {
        response.add_affectedcalls()->set_value(fqi);
    }
    sink.send(response);
    sink.finish();
}

}  // namespace sila2
