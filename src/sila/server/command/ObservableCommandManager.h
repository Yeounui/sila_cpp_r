// ObservableCommandManager.h — UUID → ObservableCommandExecution map (architecture.md §3.3)
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sila/common/util/PeriodicGC.h>

namespace sila2 {

class ObservableCommandExecution;

/// Tracks every running or recently finished @ref gl_observable_command "Observable Command"
/// execution on a SiLA Server, keyed by @ref gl_command_execution_uuid "Command Execution UUID".
/// A Feature implementation calls addCommand() when a client invokes an Observable Command,
/// then getCommand() to look the execution back up for its `_Info` stream. One instance is
/// shared by every Feature that has an Observable Command; it is installed on the server via
/// `SilaServerBase::Builder::registerCommandManager()`.
///
/// @code{.cpp}
/// namespace fw = sila2::org::silastandard;  // SiLAFramework.pb.h
///
/// // Inside a Feature's Observable Command handler:
/// auto exec = cmdManager_.addCommand(std::chrono::seconds{60});  // 60s lifetime
/// std::thread([exec] {
///     exec->start();
///     // ... do the work, calling exec->setProgress(...) as it proceeds ...
///     exec->finish();
/// }).detach();
/// fw::CommandConfirmation confirmation;
/// confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
///
/// // Inside the paired `_Info` handler:
/// auto exec = cmdManager_.getCommand(req.value());
/// @endcode
///
/// Owns the UUID → execution map, generates UUIDs, and provides lifecycle
/// management (lookup, expiration sweep, bulk cancellation for shutdown).
///
/// Thread-safe: all public methods lock an internal mutex.
class ObservableCommandManager {
public:
    ObservableCommandManager();
    // Defined in the .cc, not defaulted here, to call stopAutoGC() explicitly
    // before commands_ tears down.
    ~ObservableCommandManager();

    /// Create and register a new command execution with an auto-generated
    /// @ref gl_command_execution_uuid "Command Execution UUID".
    /// @param lifetime The @ref gl_lifetime_of_execution "Lifetime of Execution": duration
    ///                 after finish before the execution is eligible for GC.
    ///                 Zero means never expires.
    /// @return Shared pointer keeping the execution alive for as long as the
    ///         caller holds it, even if a concurrent removeExpired() sweep
    ///         erases it from commands_ in the meantime (architecture.md §4.2d) —
    ///         same lifetime guarantee as getCommand().
    std::shared_ptr<ObservableCommandExecution> addCommand(
        std::chrono::seconds lifetime = std::chrono::seconds{0});

    /// Look up a command execution by its @ref gl_command_execution_uuid "Command Execution UUID",
    /// typically to serve the `_Info` stream RPC a client opens to poll or subscribe for
    /// @ref gl_command_execution_info "Command Execution Info".
    /// @return Shared pointer keeping the execution alive for as long as the
    ///         caller holds it, even if a concurrent removeExpired() sweep
    ///         erases it from commands_ in the meantime (architecture.md §4.2d).
    /// @throws sila2::error::FrameworkError with InvalidCommandExecutionUuid if not found.
    std::shared_ptr<ObservableCommandExecution> getCommand(const std::string& uuid);

    /// Remove all finished executions whose lifetime has elapsed.
    /// @return Number of executions removed.
    std::size_t removeExpired();

    /// Request interruption on all executions (for server shutdown).
    /// @see ObservableCommandExecution::requestInterruption
    void interruptAll();

    /// Start a background thread that calls removeExpired() every @p interval.
    /// No-op if auto-GC is already running.
    /// @param interval Sweep period.
    void startAutoGC(std::chrono::seconds interval) { gc_.start(interval); }

    /// Stop the background GC thread. No-op if not running.
    /// Also called by the destructor.
    void stopAutoGC() { gc_.stop(); }

    /// @return true if the auto-GC background thread is running.
    [[nodiscard("caller expects the auto-GC status")]]
    bool isAutoGCRunning() const { return gc_.isRunning(); }

    /// Callback signature for addRemovalObserver(): invoked with the
    /// @ref gl_command_execution_uuid "Command Execution UUID" of each execution
    /// removeExpired() erases.
    using RemovalCallback = std::function<void(const std::string& uuid)>;

    /// Register an observer invoked per-UUID when removeExpired() erases an
    /// execution. Multiple observers may be registered; all fire on each removal.
    /// Used by CloudEnvelopeRouter (executionFqis_ cleanup) and the gRPC
    /// InterceptorChain owner registry, which independently track UUID->FQI.
    void addRemovalObserver(RemovalCallback cb);

    /// Drop all registered removal observers. Called at shutdown so observers
    /// that captured now-dying pointers (e.g. CloudEnvelopeRouter) are not invoked
    /// afterwards.
    void clearRemovalObservers();

    /// @return Current number of tracked executions.
    [[nodiscard("caller expects the execution count")]]
    std::size_t size() const;

private:
    mutable std::mutex mu_;
    /*  std::unordered_map:
    map의 이진 트리 구조가 아닌 hash table로 pair 정렬. 비결정적 순회 순서.
    조회가 빠르고, 순서에 의존하는 코드가 없으면 이득.
        key:    uuid std::string,
        value:  commands_ *ObservableCommandExecution

    shared_ptr가 ObservableCommandExecution을 해제(delete)하는 경우 (참조 카운트 0 도달 시):
      1. 매니저 소멸 → commands_ 맵 소멸 → 이 맵이 쥔 참조 해제.
      2. 맵에서 erase (예: removeExpired()) → 이 맵이 쥔 참조 해제.
      단, getCommand()가 내준 shared_ptr 사본이 살아있는 동안은
      1·2가 일어나도 실제 해제는 그 사본이 마지막으로 소멸할 때까지 유예됨
      (architecture.md §4.2d — GC 스윕과 경합하는 bare reference dangling 방지).
    */
    std::unordered_map<std::string, std::shared_ptr<ObservableCommandExecution>> commands_;
    std::vector<RemovalCallback> removalObservers_;

    PeriodicGC gc_{[this] { removeExpired(); }};
};

}  // namespace sila2
