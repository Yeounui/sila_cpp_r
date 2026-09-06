// Tests for CallContext::onCancellation/requestCancellation fire-once
// semantics, plus the metadata round-trip (architecture.md §3.8).
#include <sila/server/transport/CallContext.h>

#include <gtest/gtest.h>

#include <string>

namespace
{
using sila2::CallContext;

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(CallContext, OnCancellationFiresWhenRequestCancellationIsCalled) {
    CallContext ctx;
    bool fired = false;
    ctx.onCancellation([&fired] { fired = true; });

    ctx.requestCancellation();

    EXPECT_TRUE(fired);
}

TEST(CallContext, DoubleRequestCancellationFiresCallbackOnlyOnce) {
    CallContext ctx;
    int callCount = 0;
    ctx.onCancellation([&callCount] { ++callCount; });

    ctx.requestCancellation();
    ctx.requestCancellation();

    EXPECT_EQ(callCount, 1);
}

TEST(CallContext, IsCancelledReturnsTrueAfterRequestCancellation) {
    CallContext ctx;

    EXPECT_FALSE(ctx.isCancelled());
    ctx.requestCancellation();

    EXPECT_TRUE(ctx.isCancelled());
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths
// CAUGHT: onCancellation's already-cancelled branch and requestCancellation's
// callbackFired_ guard both detect and handle these cases, per the header
// comment (CallContext.h:73-82) and the implementation (CallContext.cc:26-64).
// UNCAUGHT: none — this flow has no rejection surface (no invalid-argument
// path); the "false" cases below are boundary/absent-state conditions, all
// of which the implementation already handles without crashing.
// ---------------------------------------------------------------------------

TEST(CallContext, OnCancellationRegisteredAfterCancelFiresImmediately) {
    // CAUGHT: onCancellation checks cancelled_ && !callbackFired_ at
    // registration time and invokes the callback right away (CallContext.cc:59-63)
    // rather than silently dropping it.
    CallContext ctx;
    ctx.requestCancellation();
    bool fired = false;

    ctx.onCancellation([&fired] { fired = true; });

    EXPECT_TRUE(fired);
}

TEST(CallContext, RequestCancellationWithNoCallbackRegisteredDoesNotCrash) {
    // CAUGHT: requestCancellation's `if (callbackFired_ || !cancellationCallback_)
    // return;` guard (CallContext.cc:30) skips invocation when no callback was
    // ever registered.
    CallContext ctx;

    ctx.requestCancellation();

    EXPECT_TRUE(ctx.isCancelled());
}

TEST(CallContext, MetadataLookupForAbsentKeyReturnsNulloptInsteadOfValue) {
    // CAUGHT: metadata() returns std::nullopt for a key that was never set
    // (CallContext.cc:20-21), and a key that was set round-trips correctly.
    CallContext ctx;

    EXPECT_EQ(ctx.metadata("sila-missing"), std::nullopt);

    ctx.setMetadata("sila-test-key", "test-value");

    ASSERT_TRUE(ctx.metadata("sila-test-key").has_value());
    EXPECT_EQ(*ctx.metadata("sila-test-key"), "test-value");
}

}  // namespace
