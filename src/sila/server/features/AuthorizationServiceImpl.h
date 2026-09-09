// AuthorizationServiceImpl.h — AccessToken metadata scope (architecture.md §3.11)
#pragma once

#include "AuthorizationService.grpc.pb.h"

#include <sila/server/transport/SilaHandler.h>

#include <string>
#include <string_view>
#include <vector>

namespace sila2 {

struct InterceptorChain;

inline constexpr std::string_view kAuthorizationServiceFqi =
    "org.silastandard/core/AuthorizationService/v1";

const std::string& authorizationServiceFdlXml();

namespace authz_proto = sila2::org::silastandard::core::authorizationservice::v1;

/// Implements the @ref gl_feature "Feature" `org.silastandard/core/AuthorizationService/v1`,
/// letting a @ref gl_sila_client "SiLA Client" discover which Commands and
/// Properties require the AccessToken @ref gl_sila_client_metadata "SiLA Client Metadata"
/// (i.e. which ones @ref SiLAServerBase::Builder::WithAuthentication() protected).
///
/// Installed by @ref SiLAServerBase::Builder::WithAuthentication().
class AuthorizationServiceImpl final : public authz_proto::AuthorizationService::Service {
public:
    /// Constructs the service reporting the given protected list.
    ///
    /// protectedFqis: the same explicit list passed to
    /// SiLAServerBase::Builder::WithAuthentication.
    // Taken by value: the caller's list lives in Builder::authConfig_, and the
    // Builder dies when Build() returns by value — a reference into it would
    // outlive its owner.
    explicit AuthorizationServiceImpl(std::vector<std::string> protectedFqis,
                                      const InterceptorChain* chain = nullptr);

    // ---- Properties ----

    /// Serves the FCPAffectedByMetadata_AccessToken query: lists the
    /// Commands and Properties that require the AccessToken metadata.
    grpc::Status Get_FCPAffectedByMetadata_AccessToken(
        grpc::ServerContext* context,
        const authz_proto::Get_FCPAffectedByMetadata_AccessToken_Parameters* request,
        authz_proto::Get_FCPAffectedByMetadata_AccessToken_Responses* response) override;

    void getFcpAffectedByMetadataAccessToken(
        const authz_proto::Get_FCPAffectedByMetadata_AccessToken_Parameters& request,
        CallContext& ctx,
        ResponseSink<authz_proto::Get_FCPAffectedByMetadata_AccessToken_Responses>& sink);

    // Builder::authConfig_ (the source of protectedFqis_) dies with the
    // Builder when Build() returns, so initCloudRouter() — which runs after
    // Build() — reaches the same list through this instance instead.
    const std::vector<std::string>& protectedFqis() const { return protectedFqis_; }

private:
    std::vector<std::string> protectedFqis_;
    const InterceptorChain* chain_;
};

}  // namespace sila2
