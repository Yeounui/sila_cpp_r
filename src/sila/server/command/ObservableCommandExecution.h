// ObservableCommandExecution.h — SiLA 2 Observable Command state machine
// (architecture.md §3.3)
//
// New component, not a port. sila_cpp's ObservableCommandWrapper
// (reference/sila_cpp/src/include/sila_cpp/server/command/ObservableCommandWrapper.h)
// and sila_java's ObservableCommandWrapper
// (reference/sila_java/library/server_base/src/main/java/sila_java/library/server_base/command/observable/ObservableCommandWrapper.java)
// both bundle the state machine together with gRPC StreamObserver subscriber
// sets, a task runner, and a Future — this class keeps only the state
// machine. Transport plumbing stays out until an actual transport-coupled
// caller shows why it belongs here, matching the separation architecture.md
// §3.3 already draws for ObservablePropertyManager (subscribers are a
// transport-neutral ResponseSink set, not a held grpc::ServerWriter).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace sila2 {
/// Tracks the lifecycle of a single @ref gl_observable_command "Observable Command" execution
/// (architecture.md §3.3): its state machine (Waiting → Running → one of the two Finished
/// states), progress, and error message -- everything a `_Info` stream RPC reports as
/// @ref gl_command_execution_info "Command Execution Info". Obtained from
/// ObservableCommandManager::addCommand() or ObservableCommandManager::getCommand(); never
/// constructed directly by a Feature implementation.
///
/// Thread-safe: the executor thread calls start()/setProgress()/finish()/fail(),
/// while the gRPC serving thread reads state()/progress()/isExpired().
class ObservableCommandExecution {
public:
    /// The Command Execution Status reported in
    /// @ref gl_command_execution_info "Command Execution Info". Transitions only forward,
    /// never reverts: Waiting → Running → FinishedSuccessfully or FinishedWithError.
    enum class State : uint8_t {
        Waiting,
        Running,
        FinishedSuccessfully,
        FinishedWithError,
    };

    /// @param uuid The @ref gl_command_execution_uuid "Command Execution UUID" identifying
    ///             this execution.
    /// @param lifetime The @ref gl_lifetime_of_execution "Lifetime of Execution": duration
    ///                 after finish before this execution becomes eligible for GC.
    ///                 Zero means never expires (manual removal only).
    ObservableCommandExecution(std::string uuid, std::chrono::seconds lifetime);

    // --- Identity ---

    [[nodiscard("caller expects the command execution UUID")]]
    const std::string& uuid() const;

    [[nodiscard("the state drives dispatch — ignoring it misroutes command handling")]]
    State state() const;

    [[nodiscard("caller expects the state name string")]]
    static std::string stateToString(State state);

    // --- State transitions (called by executor thread) ---

    /// Waiting → Running.
    /// @throws std::logic_error if not in Waiting state.
    void start();

    /// Running → FinishedSuccessfully.
    /// @throws std::logic_error if not in Running state.
    void finish();

    /// Running → FinishedWithError.
    /// @param errorMessage Description of the error that occurred.
    /// @throws std::logic_error if not in Running state.
    void fail(std::string errorMessage);

    // --- Progress (meaningful only in Running state) ---

    /// @param fraction Progress fraction, 0.0 to 1.0.
    /// @param remaining Estimated time remaining.
    void setProgress(double fraction, std::chrono::seconds remaining);

    [[nodiscard("caller expects the progress fraction")]]
    double progress() const;

    [[nodiscard("caller expects the estimated remaining time")]]
    std::chrono::seconds estimatedRemaining() const;

    // --- Cancellation ---
    // §3.3: _Info stream termination is the only cancellation signal in SiLA 2.

    /// Request the executor to stop. Lock-free (atomic).
    void requestInterruption();

    /// @return true if interruption has been requested. Lock-free (atomic).
    [[nodiscard("caller expects the interruption status")]]
    bool isInterruptionRequested() const;

    // --- Lifetime / expiration ---

    /// @return The lifetime this execution was constructed with. Zero means it
    ///         never expires, which is also SiLA 2's meaning for an UNSET
    ///         CommandConfirmation.lifetimeOfExecution -- so a caller stamping
    ///         the wire field must SKIP it when this is zero rather than send
    ///         Duration{0}, which states "already expired".
    [[nodiscard("caller expects the configured lifetime")]]
    // Defined inline unlike the sibling accessors: lifetime_ is const and
    // read lock-free, so there is no locking detail to hide in the .cc.
    std::chrono::seconds lifetime() const { return lifetime_; }

    /// @return true if this execution has finished AND the lifetime has elapsed.
    ///         Always false if lifetime is zero (never expires).
    [[nodiscard("caller needs to know if this execution is eligible for GC")]]
    bool isExpired() const;

    // --- Error info (valid only in FinishedWithError state) ---

    [[nodiscard("caller expects the error message")]]
    std::string errorMessage() const;

private:
    // Set once at construction, never reassigned — safe to read from either
    // thread without mu_.
    const std::string uuid_;
    const std::chrono::seconds lifetime_;

    mutable std::mutex mu_;
    State state_{State::Waiting};
    double progress_{0.0};
    std::chrono::seconds remaining_{0};
    std::string errorMessage_;
    std::chrono::steady_clock::time_point finishedAt_;

    // Separate atomic, not folded under mu_: requestInterruption() must stay
    // callable from a stream-cancellation callback without waiting on
    // whatever the executor thread is doing with the state lock.
    std::atomic<bool> interruptionRequested_{false};
};
}  // namespace sila2
