// Concurrency stress test for AuthTokenStore (architecture.md §3.11):
// proves the mutex actually protects issue()/validate()/removeExpired()/
// size() under real contention, not just single-threaded correctness
// (already covered by test_auth_token_store.cc).
#include <sila/server/auth/AuthTokenStore.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
using sila2::auth::AuthTokenStore;
using namespace std::chrono_literals;

TEST(AuthTokenStoreConcurrent, ParallelIssueValidateGC) {
    AuthTokenStore store;
    const std::unordered_set<std::string> fqis{"sila2.org.Feature/Command"};
    const auto lifetime = std::chrono::duration_cast<std::chrono::seconds>(100ms);

    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    // Threads 1-2: issue + validate in a loop.
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            while (!go.load()) {}
            for (int i = 0; i < 100; ++i) {
                const std::string token = store.issue("user", fqis, lifetime);
                store.validate(token, "sila2.org.Feature/Command");
            }
        });
    }

    // Thread 3: removeExpired() in a loop.
    threads.emplace_back([&] {
        while (!go.load()) {}
        for (int i = 0; i < 100; ++i) { store.removeExpired(); }
    });

    // Thread 4: size() in a loop.
    threads.emplace_back([&] {
        while (!go.load()) {}
        for (int i = 0; i < 100; ++i) { [[maybe_unused]] auto n = store.size(); }
    });

    go = true;
    for (auto& th : threads) { th.join(); }

    // No crash / no exception propagated out of join() above is the pass
    // condition; this final call just confirms the store is still usable.
    EXPECT_NO_THROW([[maybe_unused]] auto n = store.size());
}

}  // namespace
