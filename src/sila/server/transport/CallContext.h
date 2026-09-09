// CallContext.h — per-call context: metadata, cancellation, deadline
// (architecture.md §3.8)
//
// Classic-only minimal version. Will be generalized when §3.9
// (server-initiated connection) is implemented.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace sila2 {

/// Carries per-RPC metadata, a cancellation signal, and a deadline
/// (architecture.md §3.8). Transport adapters fill it — the gRPC adapter
/// from grpc::ServerContext, the cloud adapter (§3.9) from envelope fields.
///
/// Thread-safe: the transport adapter thread writes metadata before dispatch,
/// the serving thread reads it; cancellation and deadline reads are lock-free.
class CallContext {
public:
    /// A predicate the transport installs so isCancelled() can ask the live
    /// transport, not just a flag this class owns. Returns true once the peer
    /// has gone away or the call was cancelled.
    using CancellationProbe = std::function<bool()>;

    CallContext();

    /// Construct with a transport-supplied cancellation probe.
    ///
    /// The probe is captured once and never reassigned, so isCancelled() reads
    /// it without a lock. The probe must not outlive whatever it captures:
    /// the gRPC adapter captures a grpc::ServerContext*, and this is sound
    /// only because dispatchToHandler destroys the CallContext before the
    /// service method returns. A handler that hands the context to a detached
    /// thread would dangle — pass no probe on such a path.
    explicit CallContext(CancellationProbe probe);

    ~CallContext();

    // --- Metadata ---

    /// Store a metadata key-value pair. Called by the transport adapter.
    void setMetadata(const std::string& key, std::string value);

    /// Look up a metadata value by key.
    /// @return The value, or nullopt if the key is absent.
    [[nodiscard("caller expects the metadata value")]] \
    std::optional<std::string> metadata(const std::string& key) const;

    // No whole-map accessor: SiLA 2 addresses metadata per item by fully
    // qualified identifier (one `sila-<fqi>-bin` header, one
    // Get_FCPAffectedByMetadata_<Name> RPC), so key lookup is the only shape
    // the standard needs. A map accessor would also have to copy — returning
    // a reference to metadata_ hands the caller unsynchronized state.

    // --- Cancellation ---

    /// Distinguishes why isCancelled() went true. SiLA 2 Part A ties both
    /// Observable Command continuation and Property Subscription cancellation
    /// to the *Connection* being interrupted/closed, not to any one call
    /// dropping -- so a caller that must keep executing on the latter (§3.3)
    /// needs to tell the two apart. Only the cloud transport can: an explicit
    /// Cancel* envelope (ActiveCallRegistry::cancel) leaves the Connection's
    /// shared stream alive, while cancelAll() fires when that stream itself
    /// breaks. Direct gRPC has no such split -- IsCancelled() reports both
    /// identically -- so its callers must not read this reason at all and
    /// instead treat every isCancelled() as "stop sending, keep executing".
    enum class CancellationReason {
        kClientRequested,  // this one call was asked to stop; Connection is still up
        kConnectionLost,   // the Connection itself is gone; every live call is being swept
    };

    /// Request cancellation of this call. Sets the atomic flag lock-free,
    /// then fires the callback (at most once) under mu_.
    /// @param reason Defaults to kClientRequested: every call site except
    /// ActiveCallRegistry::cancelAll() is either an explicit per-call cancel
    /// or a path (router teardown, direct-gRPC's own cancellationProbe_) that
    /// never reads cancellationReason() back, so the default is a safe no-op
    /// for them.
    void requestCancellation(CancellationReason reason = CancellationReason::kClientRequested);

    /// @return true if the call is over from the caller's point of view:
    /// cancellation was requested, the deadline passed, or the transport's
    /// probe reports the peer gone. Lock-free.
    [[nodiscard("caller expects the cancellation status")]] \
    bool isCancelled() const;

    /// Register a callback to be invoked when cancellation is requested.
    /// If already cancelled at the time of registration, the callback is
    /// invoked immediately (outside any lock).
    ///
    /// Only requestCancellation() fires this callback — a probe going true
    /// does not, because nothing signals it. The cloud transport pushes
    /// cancellation through requestCancellation() (ActiveCallRegistry::cancel)
    /// and so gets callbacks; direct-gRPC handlers must poll isCancelled().
    /// Giving gRPC callbacks too would need a watchdog thread per RPC; add one
    /// only if a gRPC-side caller actually needs push notification.
    using CancellationCallback = std::function<void()>;
    /// Registers `callback` to run once cancellation is requested, invoked
    /// immediately if the call is already cancelled at registration time.
    void onCancellation(CancellationCallback callback);

    /// @return the reason passed to the requestCancellation() call that set
    /// cancelled_. Meaningless while isCancelled() is false via that path --
    /// deadline expiry and the direct-gRPC probe carry no reason of their
    /// own, so this stays at its kClientRequested default until an explicit
    /// requestCancellation() call overwrites it.
    [[nodiscard("caller expects the cancellation reason")]] \
    CancellationReason cancellationReason() const;

    // --- Deadline ---

    /// Set the deadline for this call.
    void setDeadline(std::chrono::steady_clock::time_point deadline);

    /// @return The deadline. time_point::max() if no deadline was set.
    [[nodiscard("caller expects the deadline time point")]] \
    std::chrono::steady_clock::time_point deadline() const;

    /// @return true if the deadline has been exceeded.
    [[nodiscard("caller expects the deadline exceeded status")]] \
    bool isDeadlineExceeded() const;

private:
    std::unordered_map<std::string, std::string> metadata_;
    std::atomic<bool> cancelled_{false};
    // Set before cancelled_ in requestCancellation() so a reader that
    // observes cancelled_ == true never sees the reason from a stale, still
    // in-flight prior call -- matching cancelled_'s own lock-free contract.
    std::atomic<CancellationReason> cancellationReason_{CancellationReason::kClientRequested};
    mutable std::mutex mu_;  // guards metadata_, cancellationCallback_, callbackFired_
    CancellationCallback cancellationCallback_;
    bool callbackFired_{false};
    // Atomic rather than mutex-guarded so isCancelled() stays lock-free on the
    // 64-bit targets this ships on; a time_point is a trivially copyable
    // 64-bit count, so the atomic is lock-free there.
    std::atomic<std::chrono::steady_clock::time_point> deadline_{
        std::chrono::steady_clock::time_point::max()};
    const CancellationProbe cancellationProbe_;
};

}  // namespace sila2
