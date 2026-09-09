// AuthorizationInterceptor.cc
#include "AuthorizationInterceptor.h"

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/transport/CallContext.h>

#include <utility>

namespace sila2::auth {

AuthorizationInterceptor::AuthorizationInterceptor(
    AuthTokenStore& store,
    std::function<bool(const std::string&)> isProtected)
    : store_{store}, isProtected_{std::move(isProtected)} {}

void AuthorizationInterceptor::intercept(CallContext& ctx, const std::string& targetFqi) {
    // Unprotected FQIs skip the token check entirely — no metadata lookup needed.
    if (!isProtected_(targetFqi)) {
        return;
    }

    std::optional<std::string> accessToken = ctx.metadata("access-token");
    if (!accessToken.has_value()) {
        throw error::FrameworkError{
            error::FrameworkError::FrameworkErrorType::InvalidMetadata,
            "No access token provided for protected feature"};
    }

    // validate() checks both locally-issued tokens and cached remote verifications.
    // On hit it also renews the token's sliding expiry (see AuthTokenStore.cc).
    if (store_.validate(*accessToken, targetFqi).has_value()) {
        return;
    }

    // FQI form per SiLAFramework.proto:101 and AuthorizationService-v1_0.sila.xml:26 —
    // <feature FQI>/DefinedExecutionError/<Identifier>, the same shape codegen emits.
    throw error::DefinedExecutionError{
        "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken",
        "Access token is invalid, expired, or not authorized for the requested feature"};
}

}  // namespace sila2::auth
