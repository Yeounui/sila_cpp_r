// Tests for ActiveCallRegistry::remove() and the ctx capture path (§2.1h-cov).
// test_active_call_registry.cc already covers add/find/cancel/cancelAll;
// this file targets what the §2.1h fix (CallRegistryGuard's explicit remove()
// and CloudEnvelopeRouter's ctx capture) added but left unexercised:
// explicit removal ahead of weak_ptr expiry, add()'s prune-on-insert loop,
// and that cancel() reaches the stored context through requestCancellation()
// specifically (not just some other path that happens to flip isCancelled()).
#include <sila/server/transport/cloud/ActiveCallRegistry.h>
#include <sila/server/transport/CallContext.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{
using sila2::ActiveCallRegistry;
using sila2::CallContext;

// --- TRUE paths ---

TEST(ActiveCallRegistryRemoval, RemoveErasesEntrySoFindReturnsNullptr) {
    ActiveCallRegistry registry;
    auto ctx = std::make_shared<CallContext>();
    registry.add("uuid-1", ctx);

    registry.remove("uuid-1");

    EXPECT_EQ(registry.find("uuid-1"), nullptr);
}

TEST(ActiveCallRegistryRemoval, RemoveDropsLiveContextWithoutWaitingForExpiry) {
    ActiveCallRegistry registry;
    auto ctx = std::make_shared<CallContext>();
    registry.add("uuid-1", ctx);

    registry.remove("uuid-1");

    // ctx is still alive here (this shared_ptr keeps it so) — proves remove()
    // erases the map entry directly, rather than relying on the weak_ptr
    // expiring on its own.
    EXPECT_EQ(registry.find("uuid-1"), nullptr);
    registry.cancel("uuid-1");
    EXPECT_FALSE(ctx->isCancelled());
}

TEST(ActiveCallRegistryRemoval, ExpiredEntryIsPrunedOnNextAddAndSlotIsReusable) {
    ActiveCallRegistry registry;
    {
        auto ctx1 = std::make_shared<CallContext>();
        registry.add("uuid-1", ctx1);
    }  // ctx1 expires; the stale weak_ptr is left in calls_ until the next add().

    auto ctx2 = std::make_shared<CallContext>();
    registry.add("uuid-2", ctx2);  // add()'s prune loop sweeps the dead uuid-1 entry.

    EXPECT_EQ(registry.find("uuid-1"), nullptr);
    ASSERT_NE(registry.find("uuid-2"), nullptr);
    EXPECT_EQ(registry.find("uuid-2").get(), ctx2.get());

    // Re-adding the pruned uuid gets a clean slot, not a stale collision.
    auto ctx3 = std::make_shared<CallContext>();
    registry.add("uuid-1", ctx3);
    ASSERT_NE(registry.find("uuid-1"), nullptr);
    EXPECT_EQ(registry.find("uuid-1").get(), ctx3.get());
}

TEST(ActiveCallRegistryRemoval, CancelReachesStoredContextThroughRequestCancellation) {
    ActiveCallRegistry registry;
    auto ctx = std::make_shared<CallContext>();
    bool callbackFired = false;
    // onCancellation() only fires through requestCancellation() (per
    // CallContext.h) — a probe flipping true would not trigger it. Asserting
    // on the callback, not just isCancelled(), proves cancel() drives the
    // captured ctx through requestCancellation() specifically.
    ctx->onCancellation([&callbackFired] { callbackFired = true; });
    registry.add("uuid-1", ctx);

    registry.cancel("uuid-1");

    EXPECT_TRUE(callbackFired);
    EXPECT_TRUE(ctx->isCancelled());
}

// --- FALSE paths ---

TEST(ActiveCallRegistryRemoval, RemoveNonexistentUuidIsNoop) {
    ActiveCallRegistry registry;

    EXPECT_NO_THROW(registry.remove("no-such-uuid"));

    // The registry is still usable afterward — remove() on a missing key
    // did not corrupt state.
    auto ctx = std::make_shared<CallContext>();
    registry.add("uuid-1", ctx);
    EXPECT_EQ(registry.find("uuid-1").get(), ctx.get());
}

TEST(ActiveCallRegistryRemoval, CancelNonexistentUuidDoesNotThrowOrAffectOtherEntries) {
    ActiveCallRegistry registry;
    auto ctx = std::make_shared<CallContext>();
    registry.add("uuid-1", ctx);

    EXPECT_NO_THROW(registry.cancel("no-such-uuid"));

    EXPECT_FALSE(ctx->isCancelled());
}

// Registry-level overwrite stays deliberate: add() also serves the unary
// dispatch paths, where reusing a requestUUID after CallRegistryGuard::remove
// is legitimate, and the registry cannot know a live pump is attached. Audit
// 2.2n's defect — an orphaned, uncancellable subscription pump — is guarded
// one level up, in CloudEnvelopeRouter::startPump, where liveness is known.
TEST(ActiveCallRegistryRemoval, AddSameUuidTwiceOverwritesFirstEntry) {
    ActiveCallRegistry registry;
    auto ctx1 = std::make_shared<CallContext>();
    auto ctx2 = std::make_shared<CallContext>();
    registry.add("uuid-1", ctx1);

    registry.add("uuid-1", ctx2);  // second add() for the same key overwrites the first.

    ASSERT_NE(registry.find("uuid-1"), nullptr);
    EXPECT_EQ(registry.find("uuid-1").get(), ctx2.get());

    registry.cancel("uuid-1");
    EXPECT_TRUE(ctx2->isCancelled());
    EXPECT_FALSE(ctx1->isCancelled());  // the orphaned first entry is unreachable, not cancelled.
}

}  // namespace
