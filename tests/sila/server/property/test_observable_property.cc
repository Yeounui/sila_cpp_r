// Checks ObservablePropertyManager subscription, publish/waitForNext, both
// overflow policies, and lifecycle cleanup (unsubscribe/cancelAll/shutdown).
#include <sila/server/property/ObservablePropertyManager.h>

#include <gtest/gtest.h>

#include <any>
#include <chrono>
#include <string>
#include <thread>

using sila2::ObservablePropertyManager;
using sila2::OverflowPolicy;
using sila2::Subscription;

TEST(ObservablePropertyManager, SubscribeAndPublish) {
    ObservablePropertyManager manager;
    auto sub = manager.subscribe("prop1");
    manager.publish("prop1", std::any{42});

    auto value = sub->waitForNext();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::any_cast<int>(*value), 42);
}

TEST(ObservablePropertyManager, InitialValue) {
    ObservablePropertyManager manager;
    auto sub = manager.subscribe("prop1", std::any{99});

    auto value = sub->waitForNext();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::any_cast<int>(*value), 99);
}

TEST(ObservablePropertyManager, MultipleSubscribers) {
    ObservablePropertyManager manager;
    auto sub1 = manager.subscribe("prop1");
    auto sub2 = manager.subscribe("prop1");
    manager.publish("prop1", std::any{7});

    auto value1 = sub1->waitForNext();
    auto value2 = sub2->waitForNext();
    ASSERT_TRUE(value1.has_value());
    ASSERT_TRUE(value2.has_value());
    EXPECT_EQ(std::any_cast<int>(*value1), 7);
    EXPECT_EQ(std::any_cast<int>(*value2), 7);
}

TEST(ObservablePropertyManager, DiscardOldestOverflow) {
    ObservablePropertyManager manager{2};
    auto sub = manager.subscribe("prop1", {}, OverflowPolicy::DiscardOldest);
    manager.publish("prop1", std::any{10});
    manager.publish("prop1", std::any{20});
    manager.publish("prop1", std::any{30});

    auto first = sub->waitForNext();
    auto second = sub->waitForNext();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(std::any_cast<int>(*first), 20);
    EXPECT_EQ(std::any_cast<int>(*second), 30);
}

TEST(ObservablePropertyManager, TerminateOnFullOverflow) {
    ObservablePropertyManager manager{2};
    auto sub = manager.subscribe("prop1", {}, OverflowPolicy::TerminateOnFull);
    manager.publish("prop1", std::any{10});
    manager.publish("prop1", std::any{20});
    manager.publish("prop1", std::any{30});

    EXPECT_EQ(manager.subscriberCount("prop1"), 0);
    EXPECT_TRUE(sub->isCancelled());
}

TEST(ObservablePropertyManager, CancelSubscription) {
    ObservablePropertyManager manager;
    auto sub = manager.subscribe("prop1");
    sub->cancel();

    auto value = sub->waitForNext();
    EXPECT_FALSE(value.has_value());
}

TEST(ObservablePropertyManager, Unsubscribe) {
    ObservablePropertyManager manager;
    auto sub = manager.subscribe("prop1");
    EXPECT_EQ(manager.subscriberCount("prop1"), 1);

    manager.unsubscribe("prop1", sub);
    EXPECT_EQ(manager.subscriberCount("prop1"), 0);
}

TEST(ObservablePropertyManager, CancelAll) {
    ObservablePropertyManager manager;
    auto sub1 = manager.subscribe("prop1");
    auto sub2 = manager.subscribe("prop1");

    manager.cancelAll("prop1");
    EXPECT_TRUE(sub1->isCancelled());
    EXPECT_TRUE(sub2->isCancelled());
}

TEST(ObservablePropertyManager, ShutdownCancelsAll) {
    std::shared_ptr<Subscription> sub;
    {
        ObservablePropertyManager manager;
        sub = manager.subscribe("prop1");
    }
    EXPECT_TRUE(sub->isCancelled());
}

TEST(ObservablePropertyManager, SubscriberCount) {
    ObservablePropertyManager manager;
    EXPECT_EQ(manager.subscriberCount("unknown"), 0);

    auto sub1 = manager.subscribe("prop1");
    auto sub2 = manager.subscribe("prop1");
    EXPECT_EQ(manager.subscriberCount("prop1"), 2);

    manager.unsubscribe("prop1", sub1);
    EXPECT_EQ(manager.subscriberCount("prop1"), 1);
}
