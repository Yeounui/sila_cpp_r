// ObservablePropertyManager.h — per-property subscriber management
// (architecture.md §3.3)
//
// New component, not a port. sila_cpp's ObservablePropertyWrapper
// (reference/sila_cpp/src/include/sila_cpp/server/property/) and sila_java's
// server_base both bundle transport (gRPC StreamObserver / ServerWriter)
// directly into the property wrapper. This class keeps only subscriber
// management and queuing — transport plumbing stays out until an actual
// transport-coupled caller shows why it belongs here, matching
// ObservableCommandExecution.h's separation.
#pragma once

#include <any>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sila2 {

enum class OverflowPolicy : uint8_t {
    DiscardOldest,
    TerminateOnFull,
};

/// A single subscriber's bounded value queue.
///
/// Thread-safe: enqueue() (publisher side) and waitForNext()/cancel()
/// (consumer side) may run concurrently.
class Subscription {
public:
    Subscription(std::size_t maxDepth, OverflowPolicy policy);

    /// Publisher-side, non-blocking.
    /// DiscardOldest: drops oldest if full, always returns true.
    /// TerminateOnFull: returns false if full (caller must terminate this subscription).
    bool enqueue(std::any value);

    /// Consumer-side, blocking.
    /// Returns nullopt if cancelled.
    std::optional<std::any> waitForNext();

    /// Signal cancellation, wake all blocked waiters.
    void cancel();

    [[nodiscard("caller expects the cancellation status")]]
    bool isCancelled() const;

private:
    const std::size_t maxDepth_;
    const OverflowPolicy policy_;
    std::deque<std::any> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> cancelled_{false};
};

/// Manages per-property subscriber sets with bounded queues for backpressure
/// (architecture.md §3.3). Shared across Feature implementations and the
/// RecoverableErrorGate.
///
/// Thread-safe: all public methods lock an internal mutex.
class ObservablePropertyManager {
public:
    explicit ObservablePropertyManager(std::size_t defaultQueueDepth = 16);
    ~ObservablePropertyManager();

    /// Register a subscriber for propertyId.
    /// If initialValue has a value, it is enqueued atomically with subscription
    /// creation so no publish can slip between registration and the initial push.
    std::shared_ptr<Subscription> subscribe(
        const std::string& propertyId,
        std::any initialValue = {},
        OverflowPolicy policy = OverflowPolicy::DiscardOldest);

    /// Enqueue value to all subscribers of propertyId. Non-blocking.
    /// TerminateOnFull subscribers that overflow are cancelled and removed.
    void publish(const std::string& propertyId, std::any value);

    /// Remove a specific subscriber.
    void unsubscribe(const std::string& propertyId,
                     const std::shared_ptr<Subscription>& sub);

    /// Cancel all subscriptions for a property.
    void cancelAll(const std::string& propertyId);

    /// Cancel everything. Also called by the destructor.
    void shutdown();

    [[nodiscard("caller expects the subscriber count")]]
    std::size_t subscriberCount(const std::string& propertyId) const;

private:
    const std::size_t defaultQueueDepth_;
    mutable std::mutex mu_;
    std::unordered_map<std::string,
                       std::vector<std::shared_ptr<Subscription>>> subscribers_;
};

}  // namespace sila2
