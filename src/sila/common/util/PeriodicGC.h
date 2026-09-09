// PeriodicGC.h — reusable periodic sweep thread (architecture.md §3.3, §3.5)
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace sila2 {

/// Runs a caller-supplied sweep callback on a fixed interval in a background
/// thread.  Used by BinaryStore and ObservableCommandManager to periodically
/// remove expired entries. An internal helper; a library user does not need
/// this directly.
///
/// start() and stop() are NOT safe to call concurrently with each other: a
/// stop() racing a start() can observe running_ == true and reach
/// thread_.joinable() before start() has assigned thread_, leaving the
/// worker thread created already-stopped and every later stop() an
/// early-return — so ~PeriodicGC() would join a still-joinable thread.
/// Every current call site invokes start()/stop() sequentially on one
/// owner; if a future caller needs concurrent start()/stop(), add a mutex
/// serializing the two.
class PeriodicGC {
public:
    /// @param sweep The function to call every interval (e.g. removeExpired).
    explicit PeriodicGC(std::function<void()> sweep)
        : sweep_{std::move(sweep)} {}

    ~PeriodicGC() { stop(); }

    PeriodicGC(const PeriodicGC&) = delete;
    PeriodicGC& operator=(const PeriodicGC&) = delete;

    /// Starts the background sweep thread. A no-op if already running.
    /// @param interval How often to invoke the sweep callback.
    void start(std::chrono::seconds interval) {
        if (running_.exchange(true)) { return; }
        thread_ = std::thread{[this, interval] {
            std::unique_lock<std::mutex> lock{mu_};
            while (running_.load()) {
                cv_.wait_for(lock, interval, [this] { return !running_.load(); });
                if (running_.load()) { sweep_(); }
            }
        }};
    }

    /// Stops the background sweep thread and joins it. A no-op if not
    /// running; also called from the destructor.
    void stop() {
        if (!running_.exchange(false)) { return; }
        {
            // This empty critical section is a rendezvous barrier, not dead
            // code: the sweep thread holds mu_ from the top of its loop
            // until it is actually inside cv_.wait_for (which atomically
            // releases mu_ while waiting). Acquiring mu_ here therefore
            // blocks until the thread has released it — i.e. until the
            // thread is either not yet started or genuinely waiting on cv_ —
            // before we notify. Without this barrier, notify_all() could
            // land between the thread's "running_ still true" read and its
            // wait_for call, while the thread still holds mu_, and be lost;
            // stop() would then block in join() for a full interval instead
            // of waking the sweep immediately.
            std::lock_guard<std::mutex> lock{mu_};
        }
        cv_.notify_all();
        if (thread_.joinable()) { thread_.join(); }
    }

    /// True between a completed start() and the next stop().
    [[nodiscard]] bool isRunning() const { return running_.load(); }

private:
    std::function<void()> sweep_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace sila2
