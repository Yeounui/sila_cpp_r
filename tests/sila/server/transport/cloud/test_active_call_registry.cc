// Tests for ActiveCallRegistry: add/find/cancel/cancelAll/remove over the
// requestUUID -> weak_ptr<CallContext> map (architecture.md §3.9).
#include <sila/server/transport/cloud/ActiveCallRegistry.h>
#include <sila/server/transport/CallContext.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{
using sila2::ActiveCallRegistry;
using sila2::CallContext;

TEST(ActiveCallRegistry, AddThenFindReturnsSameContext) {
    ActiveCallRegistry registry;
    auto ctx = std::make_shared<CallContext>();

    registry.add("uuid-1", ctx);
    auto found = registry.find("uuid-1");

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), ctx.get());
}

TEST(ActiveCallRegistry, CancelPropagatesToContext) {
    ActiveCallRegistry registry;
    auto ctx = std::make_shared<CallContext>();
    registry.add("uuid-1", ctx);

    registry.cancel("uuid-1");

    EXPECT_TRUE(ctx->isCancelled());
}

TEST(ActiveCallRegistry, CancelAllCancelsMultipleAndClears) {
    ActiveCallRegistry registry;
    auto ctxA = std::make_shared<CallContext>();
    auto ctxB = std::make_shared<CallContext>();
    registry.add("uuid-a", ctxA);
    registry.add("uuid-b", ctxB);

    registry.cancelAll();

    EXPECT_TRUE(ctxA->isCancelled());
    EXPECT_TRUE(ctxB->isCancelled());
    EXPECT_EQ(registry.find("uuid-a"), nullptr);
    EXPECT_EQ(registry.find("uuid-b"), nullptr);
}

TEST(ActiveCallRegistry, FindUnknownUuidReturnsNullptr) {
    ActiveCallRegistry registry;

    EXPECT_EQ(registry.find("no-such-uuid"), nullptr);
}

TEST(ActiveCallRegistry, FindAfterContextExpiresReturnsNullptr) {
    ActiveCallRegistry registry;
    {
        auto ctx = std::make_shared<CallContext>();
        registry.add("uuid-1", ctx);
    }  // ctx's last shared_ptr goes out of scope; the registry only holds a weak_ptr.

    EXPECT_EQ(registry.find("uuid-1"), nullptr);
}

TEST(ActiveCallRegistry, CancelUnknownUuidIsNoop) {
    ActiveCallRegistry registry;

    EXPECT_NO_THROW(registry.cancel("no-such-uuid"));
}


}  // namespace
