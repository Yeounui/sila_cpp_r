// AuthorizationInterceptor.h — access-token gate for protected FQIs (§3.11)
#pragma once

#include <functional>
#include <string>

#include <sila/server/auth/AuthTokenStore.h>

// CallContext lives in the parent namespace sila2, not sila2::auth.
namespace sila2 { class CallContext; }

namespace sila2::auth {

/// Gates a protected call on a valid access token before the server's handler runs.
///
/// Assembled by SiLAServerBase::Builder::WithAuthentication() from the AuthTokenStore
/// and the protected-FQI list the caller supplied; not constructed directly by a
/// server author.
class AuthorizationInterceptor {
public:
    AuthorizationInterceptor(AuthTokenStore& store,
                             std::function<bool(const std::string&)> isProtected);

    /// Checks `targetFqi` against the protected list and, if protected, validates the
    /// `access-token` @ref gl_sila_client_metadata "SiLA Client Metadata" on `ctx`
    /// against the token store.
    /// @throws error::FrameworkError (InvalidMetadata) if the call is protected and no
    ///         access token was sent.
    /// @throws error::DefinedExecutionError
    ///         ("org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken")
    ///         if the token is missing from the store, expired, or not scoped to `targetFqi`.
    void intercept(CallContext& ctx, const std::string& targetFqi);

private:
    AuthTokenStore& store_;
    std::function<bool(const std::string&)> isProtected_;
};

}  // namespace sila2::auth
