// AuthenticationServiceImpl.h — Login/Logout token management (architecture.md §3.11)
#pragma once

#include "AuthenticationService.grpc.pb.h"

#include <sila/server/transport/SilaHandler.h>

#include <string>
#include <string_view>

namespace sila2 {

class ServerConfig;
struct InterceptorChain;

namespace auth {

class AuthTokenStore;
class CredentialVerifier;
class AccessPolicy;

}  // namespace auth

inline constexpr std::string_view kAuthenticationServiceFqi =
    "org.silastandard/core/AuthenticationService/v1";

// Per-RPC granular authorization FQIs. The direct-gRPC auth gate matches a
// call's FQI against protectedFqis; passing these Command-level FQIs (not the
// feature-level one above) lets a command-level protectedFqis entry actually
// gate the matching RPC, keeping direct-gRPC coverage aligned with the cloud
// path. Full-literal string_view, not runtime concatenation, to stay constexpr
// and match kAuthorizationProviderParamFqi's existing style. (Login still
// clears the auth gate via its local loginChain copy; the granular FQI only
// changes the coverage name, not the exemption — see the .cc.)
inline constexpr std::string_view kLoginFqi =
    "org.silastandard/core/AuthenticationService/v1/Command/Login";
inline constexpr std::string_view kLogoutFqi =
    "org.silastandard/core/AuthenticationService/v1/Command/Logout";

const std::string& authenticationServiceFdlXml();

namespace auth_proto = sila2::org::silastandard::core::authenticationservice::v1;

/// Implements the @ref gl_feature "Feature" `org.silastandard/core/AuthenticationService/v1`,
/// letting a @ref gl_sila_client "SiLA Client" log in for an AccessToken
/// (@ref gl_sila_client_metadata "SiLA Client Metadata") scoped to the
/// Features it requested, and log back out to invalidate that token.
///
/// Installed by @ref SiLAServerBase::Builder::WithAuthentication().
class AuthenticationServiceImpl final : public auth_proto::AuthenticationService::Service {
public:
    // Dependencies by reference, not owned. All must outlive this object.
    AuthenticationServiceImpl(auth::AuthTokenStore& store,
                               auth::CredentialVerifier& verifier,
                               const auth::AccessPolicy& policy,
                               const ServerConfig& config,
                               const InterceptorChain* chain = nullptr);

    // ---- Commands ----

    /// Serves the Login command: verifies the credentials and issues an
    /// AccessToken scoped to the requested Features (or every Feature the
    /// user's policy allows, if none were requested).
    ///
    /// Login never runs the access-token gate, even when the operator lists
    /// this feature's FQI in protectedFqis — it is the command that issues
    /// the token the gate checks for. See the rationale in the .cc.
    /// @throws error::ValidationError if RequestedServer is not a
    ///         36-character lowercase-hex UUID, if it names a different
    ///         server, or if a RequestedFeatures entry is not a fully
    ///         qualified identifier.
    /// @throws error::DefinedExecutionError{AuthenticationFailed}
    ///         if the credentials do not verify.
    grpc::Status Login(
        grpc::ServerContext* context,
        const auth_proto::Login_Parameters* request,
        auth_proto::Login_Responses* response) override;

    /// Serves the Logout command: invalidates the given AccessToken.
    /// @throws error::DefinedExecutionError{InvalidAccessToken} if the token
    ///         is not currently valid.
    grpc::Status Logout(
        grpc::ServerContext* context,
        const auth_proto::Logout_Parameters* request,
        auth_proto::Logout_Responses* response) override;

    void login(const auth_proto::Login_Parameters& request, CallContext& ctx,
               ResponseSink<auth_proto::Login_Responses>& sink);
    void logout(const auth_proto::Logout_Parameters& request, CallContext& ctx,
                ResponseSink<auth_proto::Logout_Responses>& sink);

private:
    auth::AuthTokenStore& store_;
    auth::CredentialVerifier& verifier_;
    const auth::AccessPolicy& policy_;
    const ServerConfig& config_;
    const InterceptorChain* chain_;
};

}  // namespace sila2
