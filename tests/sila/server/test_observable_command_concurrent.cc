// Concurrency regression tests for ObservableCommandManager (architecture.md
// §4.2d, §2.2c): getCommand() must keep returning a live object across a
// concurrent GC sweep, and removeExpired() must not deadlock against a
// removalCallback_ that takes its own lock.
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>

#include <sila/common/error/SiLAErrorSubtypes.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
using sila2::ObservableCommandExecution;
using sila2::ObservableCommandManager;

// §4.2d: getCommand() returns a shared_ptr specifically so a caller's copy
// outlives a concurrent removeExpired() sweep that erases the entry from
// commands_.
TEST(ObservableCommandManagerConcurrent, GetCommandSurvivesGCSweep) {
    ObservableCommandManager manager;
    auto exec = manager.addCommand(std::chrono::seconds{1});
    exec->start();
    exec->finish();

    // Take a shared_ptr copy before the GC sweep runs.
    std::shared_ptr<ObservableCommandExecution> ptr = manager.getCommand(exec->uuid());
    const std::string uuid = ptr->uuid();

    std::thread gcThread([&] {
        std::this_thread::sleep_for(std::chrono::seconds{2});
        manager.removeExpired();
    });
    gcThread.join();

    // The GC sweep erased commands_'s own reference, but ptr's copy keeps
    // the execution alive: this dereference must not be use-after-free.
    EXPECT_FALSE(ptr->uuid().empty());
    EXPECT_THROW(manager.getCommand(uuid), sila2::error::FrameworkError);
}

// §2.2c: removeExpired() must release mu_ before invoking removalCallback_.
// A separate thread holds callbackMu (the callback's own lock) while calling
// manager.size() (which needs mu_) — the AB-BA lock order a pre-fix
// removeExpired() (holding mu_ while invoking the callback) would deadlock on.
TEST(ObservableCommandManagerConcurrent, RemovalCallbackDoesNotDeadlock) {
    // Zero-second lifetime means "never expires" (ObservableCommandExecution
    // docs), so this needs a genuine non-zero lifetime, not the sub-second
    // duration_cast-to-zero trick other stores' expiry tests use.
    ObservableCommandManager manager;
    auto exec = manager.addCommand(std::chrono::seconds{1});
    exec->start();
    exec->finish();
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});  // let it become expired

    std::mutex callbackMu;
    std::atomic<bool> callbackFired{false};
    manager.addRemovalObserver([&](const std::string&) {
        std::lock_guard<std::mutex> lock{callbackMu};
        callbackFired = true;
    });

    // holder grabs callbackMu, then repeatedly acquires mu_ via manager.size()
    // *while still holding callbackMu*, for a bounded window before releasing
    // callbackMu on its own. If removeExpired() ever held mu_ while invoking
    // removalCallback_, this ordering would deadlock: removeExpired() holds
    // mu_ wanting callbackMu, holder holds callbackMu wanting mu_.
    std::atomic<bool> holderReady{false};
    std::thread holder([&] {
        std::lock_guard<std::mutex> lock{callbackMu};
        holderReady = true;
        for (int i = 0; i < 5; ++i) {
            [[maybe_unused]] auto n = manager.size();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    });

    while (!holderReady.load()) {}
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    auto fut = std::async(std::launch::async, [&] { return manager.removeExpired(); });
    const auto status = fut.wait_for(std::chrono::seconds{3});

    // Detach rather than join: if this ever times out (a real deadlock),
    // holder is stuck inside manager.size() forever and joining would hang
    // the test binary too. The ASSERT below is what surfaces the failure.
    holder.detach();

    ASSERT_EQ(status, std::future_status::ready)
        << "removeExpired() deadlocked against removalCallback_ contending on callbackMu";
    EXPECT_EQ(fut.get(), 1u);
    EXPECT_TRUE(callbackFired.load());
}

}  // namespace
