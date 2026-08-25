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
/// Thread-safe: the transport adapter thread writes metadata/deadline before
/// dispatch, the serving thread reads them; cancellation is lock-free.
class CallContext {
public:
    CallContext();
    ~CallContext();

    // --- Metadata ---

    /// Store a metadata key-value pair. Called by the transport adapter.
    void setMetadata(const std::string& key, std::string value);

    /// Look up a metadata value by key.
    /// @return The value, or nullopt if the key is absent.
    [[nodiscard("caller expects the metadata value")]] \
    std::optional<std::string> metadata(const std::string& key) const;

    /// @return All metadata as a const reference.
    [[nodiscard("caller expects the metadata map")]] \
    const std::unordered_map<std::string, std::string>& allMetadata() const;

    // --- Cancellation ---

    /// Request cancellation of this call. Lock-free (atomic).
    void requestCancellation();

    /// @return true if cancellation has been requested. Lock-free (atomic).
    [[nodiscard("caller expects the cancellation status")]] \
    bool isCancelled() const;

    /// Register a callback to be invoked when cancellation is requested.
    /// If already cancelled at the time of registration, the callback is
    /// invoked immediately (outside any lock).
    using CancellationCallback = std::function<void()>;
    void onCancellation(CancellationCallback callback);

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
    mutable std::mutex mu_;
    CancellationCallback cancellationCallback_;
    std::chrono::steady_clock::time_point deadline_{
        std::chrono::steady_clock::time_point::max()};
};

}  // namespace sila2
