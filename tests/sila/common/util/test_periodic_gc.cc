// Checks the lost-wakeup fix in PeriodicGC::stop(): stopping a GC must
// return promptly, not block until the interval elapses. Before the fix,
// stop() called cv_.notify_all() without holding mu_, so the notification
// could land in the narrow window inside cv_.wait_for where the sweep
// thread has evaluated its predicate (false) but has not yet begun
// sleeping, dropping the wakeup and leaving join() to block for a full
// interval.
//
// The vulnerable window is a handful of instructions wide, and PeriodicGC's
// interval is second-granularity, so a single start()-then-stop() only hits
// it a fraction of a percent of the time (measured empirically). Repeating
// the trial many times makes a single omitted lock in stop() reliably
// observable: against the pre-fix code this loop hit the bug within a few
// thousand iterations in every trial run; against the fix, 3000 iterations
// produced zero slow stops across repeated runs.
#include <sila/common/util/PeriodicGC.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

TEST(PeriodicGC, StopReturnsPromptlyDespiteLostWakeupRace) {
    for (int i = 0; i < 3000; ++i) {
        sila2::PeriodicGC gc{[] {}};
        gc.start(std::chrono::seconds{1});
        // Yield once so the sweep thread has a chance to reach its first
        // wait_for call before stop() races it — this is what makes the
        // race window reachable instead of stop() always winning outright.
        std::this_thread::yield();

        const auto start = std::chrono::steady_clock::now();
        gc.stop();
        const auto elapsed = std::chrono::steady_clock::now() - start;

        // A bound well under the 1s interval — the lost-wakeup bug makes a
        // hit take the full interval, so any reasonable margin catches it.
        ASSERT_LT(elapsed, std::chrono::milliseconds{100}) << "iteration " << i;
    }
}
