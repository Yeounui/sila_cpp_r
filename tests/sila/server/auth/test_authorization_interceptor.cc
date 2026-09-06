// Tests for AuthorizationInterceptor: unprotected pass-through, missing/expired/
// wrong-FQI token rejection, and sliding-expiry renewal on successful validation.
#include <sila/server/auth/AuthorizationInterceptor.h>

#include <sila/server/auth/AuthTokenStore.h>
#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/server/transport/CallContext.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>

namespace
{
using sila2::CallContext;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;
using sila2::error::DefinedExecutionError;
using sila2::error::FrameworkError;
using namespace std::chrono_literals;

const std::string kProtectedFqi = "sila2.org.Feature/Command";
const std::string kOtherFqi = "sila2.org.Other/Thing";

bool isProtected(const std::string& fqi) {
    return fqi == kProtectedFqi;
}

TEST(AuthorizationInterceptor, UnprotectedFqiPassesThroughWithoutToken) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    CallContext ctx;

    EXPECT_NO_THROW(interceptor.intercept(ctx, kOtherFqi));
}

TEST(AuthorizationInterceptor, ValidTokenPassesThrough) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    CallContext ctx;
    const std::string token = store.issue("alice", {kProtectedFqi}, 60s);
    ctx.setMetadata("access-token", token);

    EXPECT_NO_THROW(interceptor.intercept(ctx, kProtectedFqi));
}

TEST(AuthorizationInterceptor, ValidTokenRefreshesSlidingExpiry) {
    // issue()'s lifetime is std::chrono::seconds, so this is rescaled to
    // 1s/600ms like AuthTokenStore's own sliding-renewal test.
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    CallContext ctx;
    const std::string token = store.issue("alice", {kProtectedFqi}, 1s);
    ctx.setMetadata("access-token", token);

    std::this_thread::sleep_for(600ms);
    EXPECT_NO_THROW(interceptor.intercept(ctx, kProtectedFqi));

    // Without the first intercept()'s renewal, the token would now be past
    // its original 1s lifetime (600ms + 600ms).
    std::this_thread::sleep_for(600ms);
    EXPECT_NO_THROW(interceptor.intercept(ctx, kProtectedFqi));
}

TEST(AuthorizationInterceptor, ProtectedFqiWithoutTokenThrows) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    CallContext ctx;

    try {
        interceptor.intercept(ctx, kProtectedFqi);
        FAIL() << "expected FrameworkError";
    } catch (const FrameworkError& e) {
        EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidMetadata);
    }
}

TEST(AuthorizationInterceptor, ExpiredTokenThrows) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    CallContext ctx;
    const std::string token = store.issue("alice", {kProtectedFqi},
                                           std::chrono::duration_cast<std::chrono::seconds>(1ms));
    ctx.setMetadata("access-token", token);
    std::this_thread::sleep_for(5ms);

    try {
        interceptor.intercept(ctx, kProtectedFqi);
        FAIL() << "expected DefinedExecutionError";
    } catch (const DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(),
                  "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken");
    }
}

TEST(AuthorizationInterceptor, TokenNotAuthorizedForFqiThrows) {
    AuthTokenStore store;
    AuthorizationInterceptor interceptor{store, isProtected};
    CallContext ctx;
    // Token is valid, but was never issued access to kProtectedFqi.
    const std::string token = store.issue("alice", {kOtherFqi}, 60s);
    ctx.setMetadata("access-token", token);

    try {
        interceptor.intercept(ctx, kProtectedFqi);
        FAIL() << "expected DefinedExecutionError";
    } catch (const DefinedExecutionError& e) {
        EXPECT_EQ(e.errorIdentifier(),
                  "org.silastandard/core/AuthorizationService/v1/DefinedExecutionError/InvalidAccessToken");
    }
}

}  // namespace
