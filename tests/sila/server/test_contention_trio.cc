// Concurrent-access tests for the three classes flagged by audit 2.2f/2.2h/
// 2.2i as having no std::thread coverage: ObservablePropertyManager,
// ActiveCallRegistry, and ServerRegistry. Each is hammered from several
// threads through its public API to surface races the single-threaded tests
// in test_observable_property.cc / test_active_call_registry.cc /
// test_server_registry.cc cannot see. Run under ThreadSanitizer for the
// strongest signal; a plain build only catches races that actually corrupt
// state or crash within the run.
#include <sila/client/ServerRegistry.h>
#include <sila/client/ClientConfig.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/cloud/ActiveCallRegistry.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using sila2::ActiveCallRegistry;
using sila2::CallContext;
using sila2::ClientConfig;
using sila2::ConnectionState;
using sila2::ObservablePropertyManager;
using sila2::ServerRegistry;
using sila2::Subscription;

constexpr int kNumThreads = 6;
constexpr int kIterations = 300;
constexpr auto kStressTimeout = std::chrono::seconds(4);

// Runs `stress` on a background thread and polls up to `timeout` for it to
// finish. If a real deadlock ever creeps in, joining the worker threads
// directly would hang the whole test binary; polling a flag instead lets the
// test report failure and move on. On timeout the runner thread is detached
// rather than joined -- everything it touches must be reachable only via
// shared_ptr (never a stack reference), so it stays valid if it outlives
// this function.
bool RunWithTimeout(std::function<void()> stress, std::chrono::milliseconds timeout) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread runner([stress = std::move(stress), done] {
        stress();
        done->store(true, std::memory_order_release);
    });
    runner.detach();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done->load(std::memory_order_acquire);
}

// Spawns `numThreads` threads each running fn(threadIndex), then joins all.
void RunThreads(int numThreads, const std::function<void(int)>& fn) {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(numThreads));
    for (int i = 0; i < numThreads; ++i) { threads.emplace_back(fn, i); }
    for (auto& t : threads) { t.join(); }
}

// --- True (positive): each class's public API survives concurrent use -----

TEST(ContentionTrio, ObservablePropertyManagerConcurrentSubscribePublishUnsubscribe) {
    auto manager = std::make_shared<ObservablePropertyManager>();
    const std::string propertyId = "temperature";

    bool completed = RunWithTimeout([manager, propertyId] {
        RunThreads(kNumThreads, [manager, propertyId](int threadIdx) {
            for (int i = 0; i < kIterations; ++i) {
                auto sub = manager->subscribe(propertyId);
                manager->publish(propertyId, std::any{threadIdx * kIterations + i});
                manager->unsubscribe(propertyId, sub);
            }
        });
    }, kStressTimeout);

    ASSERT_TRUE(completed) << "concurrent subscribe/publish/unsubscribe did not finish in time";
    // Every subscriber unsubscribed itself, so none should be left dangling
    // regardless of interleaving -- a leaked entry means the vector mutation
    // under mu_ lost an erase.
    EXPECT_EQ(manager->subscriberCount(propertyId), 0u);
}

TEST(ContentionTrio, ActiveCallRegistryConcurrentAddCancelRemove) {
    auto registry = std::make_shared<ActiveCallRegistry>();

    bool completed = RunWithTimeout([registry] {
        RunThreads(kNumThreads, [registry](int threadIdx) {
            for (int i = 0; i < kIterations; ++i) {
                auto ctx = std::make_shared<CallContext>();
                const std::string uuid = "uuid-" + std::to_string(threadIdx) + "-" + std::to_string(i);
                registry->add(uuid, ctx);
                registry->cancel(uuid);
                // ctx is still alive here (in scope), so cancel() must have
                // found the live weak_ptr and propagated.
                EXPECT_TRUE(ctx->isCancelled());
                registry->remove(uuid);
            }
        });
    }, kStressTimeout);

    ASSERT_TRUE(completed) << "concurrent add/cancel/remove did not finish in time";
}

TEST(ContentionTrio, ServerRegistryConcurrentRegisterUpdateStateRemove) {
    ClientConfig config;
    // No allowInsecure(): every target is a private-range IP (10.0.0.x), so
    // channelCredentials() takes the Part B p75 untrusted-TLS path (S74).
    auto registry = std::make_shared<ServerRegistry>(std::move(config));

    bool completed = RunWithTimeout([registry] {
        RunThreads(kNumThreads, [registry](int threadIdx) {
            // uuids collide across threads (mod 3) so add/update/remove race
            // on the *same* map entry, not just on disjoint keys.
            const std::string uuid = "uuid-" + std::to_string(threadIdx % 3);
            for (int i = 0; i < kIterations / 6; ++i) {
                registry->registerServer(uuid, "10.0.0." + std::to_string(threadIdx), 50051,
                                         "Server" + std::to_string(threadIdx));
                registry->updateState(uuid, ConnectionState::kConnected);
                auto entry = registry->findByUuid(uuid);
                if (entry.has_value()) {
                    // findByUuid copies the entry under lock, so uuid/host/port
                    // must agree -- a torn read would show a mismatched uuid.
                    EXPECT_EQ(entry->uuid, uuid);
                }
                registry->removeServer(uuid);
            }
        });
    }, kStressTimeout);

    ASSERT_TRUE(completed) << "concurrent registerServer/updateState/removeServer did not finish in time";
}

// --- False (negative): specific correctness properties under contention ---

TEST(ContentionTrio, ObservablePropertyManagerChurnDoesNotLoseSubscriberNotification) {
    // Queue depth large enough that DiscardOldest never has to evict --
    // this test is about lost notifications from a race, not backpressure.
    constexpr int kPublishCount = 500;
    auto manager = std::make_shared<ObservablePropertyManager>(kPublishCount + 1);
    const std::string propertyId = "level";
    auto longLived = manager->subscribe(propertyId);
    auto received = std::make_shared<std::vector<int>>();

    bool completed = RunWithTimeout([manager, propertyId, longLived, received] {
        // Single producer, single consumer on longLived: queue order alone
        // determines what "no loss" means, independent of the churn noise.
        std::thread consumer([longLived, received] {
            for (int i = 0; i < kPublishCount; ++i) {
                auto value = longLived->waitForNext();
                if (!value.has_value()) { break; }
                received->push_back(std::any_cast<int>(*value));
            }
        });
        std::thread producer([manager, propertyId] {
            for (int i = 0; i < kPublishCount; ++i) { manager->publish(propertyId, std::any{i}); }
        });

        // Other subscribers churn on the same propertyId concurrently,
        // contending for the same mutex/vector longLived lives in, but must
        // not affect longLived's own queue.
        RunThreads(kNumThreads, [manager, propertyId](int) {
            for (int i = 0; i < kIterations; ++i) {
                auto sub = manager->subscribe(propertyId);
                manager->unsubscribe(propertyId, sub);
            }
        });

        producer.join();
        consumer.join();
    }, kStressTimeout);

    ASSERT_TRUE(completed) << "producer/consumer/churn did not finish in time";
    ASSERT_EQ(received->size(), static_cast<std::size_t>(kPublishCount))
        << "longLived subscriber lost a notification while other subscribers churned";
    EXPECT_TRUE(std::is_sorted(received->begin(), received->end()))
        << "FIFO order was violated under contention";
    EXPECT_EQ(received->front(), 0);
    EXPECT_EQ(received->back(), kPublishCount - 1);
}

TEST(ContentionTrio, ActiveCallRegistryAddCancelSameUuidDoesNotDeadlock) {
    auto registry = std::make_shared<ActiveCallRegistry>();
    const std::string sharedUuid = "shared-uuid";

    bool completed = RunWithTimeout([registry, sharedUuid] {
        // Half the threads keep re-adding to the same key (exercising add()'s
        // expired-entry sweep over calls_), the other half keep cancelling it
        // (exercising find()-then-unlock-then-callback) -- both under one
        // mutex, so this should never contend beyond simple lock waits.
        RunThreads(kNumThreads, [registry, sharedUuid](int threadIdx) {
            for (int i = 0; i < kIterations; ++i) {
                if (threadIdx % 2 == 0) {
                    registry->add(sharedUuid, std::make_shared<CallContext>());
                } else {
                    registry->cancel(sharedUuid);
                }
            }
        });
    }, kStressTimeout);

    ASSERT_TRUE(completed) << "add()/cancel() on the same uuid deadlocked or exceeded the timeout";
}

}  // namespace
