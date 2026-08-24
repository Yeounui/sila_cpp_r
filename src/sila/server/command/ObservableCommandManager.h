// ObservableCommandManager.h — UUID → ObservableCommandExecution map (architecture.md §3.3)
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace sila2 {

class ObservableCommandExecution;

/// Manages the set of active Observable Command executions (architecture.md §3.3).
/// Owns the UUID → execution map, generates UUIDs, and provides lifecycle
/// management (lookup, expiration sweep, bulk cancellation for shutdown).
///
/// Thread-safe: all public methods lock an internal mutex.
class ObservableCommandManager {
public:
    ObservableCommandManager();
    // Defined in the .cc: ObservableCommandExecution is only forward-declared
    // above, and unique_ptr's deleter needs the complete type at the point
    // where commands_ is destroyed.
    ~ObservableCommandManager();

    /// Create and register a new command execution with an auto-generated UUID.
    /// @param lifetime Duration after finish before the execution is eligible for GC.
    ///                 Zero means never expires.
    /// @return Reference to the newly created execution. Valid until removeExpired()
    ///         or the manager is destroyed.
    ObservableCommandExecution& addCommand(
        std::chrono::seconds lifetime = std::chrono::seconds{0});

    /// Look up a command execution by UUID.
    /// @throws sila2::error::FrameworkError with InvalidCommandExecutionUuid if not found.
    ObservableCommandExecution& getCommand(const std::string& uuid);

    /// Remove all finished executions whose lifetime has elapsed.
    /// @return Number of executions removed.
    std::size_t removeExpired();

    /// Request interruption on all executions (for server shutdown).
    void interruptAll();

    /// Start a background thread that calls removeExpired() every @p interval.
    /// No-op if auto-GC is already running.
    /// @param interval Sweep period.
    void startAutoGC(std::chrono::seconds interval);

    /// Stop the background GC thread. No-op if not running.
    /// Also called by the destructor.
    void stopAutoGC();

    /// @return true if the auto-GC background thread is running.
    [[nodiscard("caller expects the auto-GC status")]] \
    bool isAutoGCRunning() const;

    /// @return Current number of tracked executions.
    [[nodiscard("caller expects the execution count")]] \
    std::size_t size() const;

private:
    static std::string generateUuid();

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<ObservableCommandExecution>> commands_;

    std::atomic<bool> gcRunning_{false};
    std::thread gcThread_;
    std::mutex gcMu_;
    std::condition_variable gcCv_;
};

}  // namespace sila2
