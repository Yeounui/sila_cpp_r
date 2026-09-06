// AuthorizationInterceptor.h — access-token gate for protected FQIs (§3.11)
#pragma once

#include <functional>
#include <string>

#include <sila/server/auth/AuthTokenStore.h>

// CallContext lives in the parent namespace sila2, not sila2::auth.
namespace sila2 { class CallContext; }

namespace sila2::auth {

class AuthorizationInterceptor {
public:
    AuthorizationInterceptor(AuthTokenStore& store,
                             std::function<bool(const std::string&)> isProtected);

    void intercept(CallContext& ctx, const std::string& targetFqi);

private:
    AuthTokenStore& store_;
    std::function<bool(const std::string&)> isProtected_;
};

}  // namespace sila2::auth
