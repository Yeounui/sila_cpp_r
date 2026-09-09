// AuthorizationConfigurationServiceImpl.h — authorization provider configuration (architecture.md §3.11)
#pragma once

#include "AuthorizationConfigurationService.grpc.pb.h"

#include <sila/server/transport/SilaHandler.h>

#include <string>
#include <string_view>

namespace sila2 {

class ServerConfig;
struct InterceptorChain;

namespace auth {
class AuthTokenStore;
}  // namespace auth

inline constexpr std::string_view kAuthorizationConfigurationServiceFqi =
    "org.silastandard/core/AuthorizationConfigurationService/v1";

// Per-RPC granular authorization FQIs. The direct-gRPC auth gate matches a
// call's FQI against protectedFqis; passing these Command/Property-level FQIs
// (not the feature-level one above) lets a command-level protectedFqis entry
// actually gate the matching RPC, keeping direct-gRPC coverage aligned with
// the cloud path. Full-literal string_view, not runtime concatenation, to stay
// constexpr and match kAuthorizationProviderParamFqi's existing style.
inline constexpr std::string_view kSetAuthorizationProviderFqi =
    "org.silastandard/core/AuthorizationConfigurationService/v1/Command/SetAuthorizationProvider";
inline constexpr std::string_view kGet_AuthorizationProviderFqi =
    "org.silastandard/core/AuthorizationConfigurationService/v1/Property/AuthorizationProvider";

const std::string& authorizationConfigurationServiceFdlXml();

namespace authzconfig_proto = sila2::org::silastandard::core::authorizationconfigurationservice::v1;

/// Implements the @ref gl_feature "Feature"
/// `org.silastandard/core/AuthorizationConfigurationService/v1`, letting a
/// @ref gl_sila_client "SiLA Client" set or read the server's external
/// authorization provider UUID.
///
/// Installed by @ref SilaServerBase::Builder::withAuthentication().
class AuthorizationConfigurationServiceImpl final
    : public authzconfig_proto::AuthorizationConfigurationService::Service {
public:
    /// `store` and `config` must outlive this object.
    AuthorizationConfigurationServiceImpl(auth::AuthTokenStore& store, ServerConfig& config,
                                          const InterceptorChain* chain = nullptr);

    // ---- Commands ----

    /// Serves the SetAuthorizationProvider command: sets the provider UUID
    /// and invalidates every access token issued so far.
    /// @throws error::ValidationError if the provider is not a 36-character
    ///         lowercase-hex UUID.
    grpc::Status SetAuthorizationProvider(
        grpc::ServerContext* context,
        const authzconfig_proto::SetAuthorizationProvider_Parameters* request,
        authzconfig_proto::SetAuthorizationProvider_Responses* response) override;

    // ---- Properties ----

    /// Serves the AuthorizationProvider property.
    grpc::Status Get_AuthorizationProvider(
        grpc::ServerContext* context,
        const authzconfig_proto::Get_AuthorizationProvider_Parameters* request,
        authzconfig_proto::Get_AuthorizationProvider_Responses* response) override;

    /// Handler body shared by the gRPC override and the cloud path for the
    /// SetAuthorizationProvider command.
    void setAuthorizationProvider(
        const authzconfig_proto::SetAuthorizationProvider_Parameters& request,
        CallContext& ctx,
        ResponseSink<authzconfig_proto::SetAuthorizationProvider_Responses>& sink);
    /// Handler body shared by the gRPC override and the cloud path for the
    /// AuthorizationProvider property.
    void getAuthorizationProvider(
        const authzconfig_proto::Get_AuthorizationProvider_Parameters& request,
        CallContext& ctx,
        ResponseSink<authzconfig_proto::Get_AuthorizationProvider_Responses>& sink);

private:
    auth::AuthTokenStore& store_;
    ServerConfig& config_;
    const InterceptorChain* chain_;
};

}  // namespace sila2
