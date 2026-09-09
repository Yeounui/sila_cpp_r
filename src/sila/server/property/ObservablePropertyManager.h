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

/// What a Subscription does when a new value arrives and its queue is already at capacity.
enum class OverflowPolicy : uint8_t {
    DiscardOldest,   ///< Drop the oldest queued value to make room for the new one.
    TerminateOnFull, ///< Cancel the subscription instead of dropping a value.
};

/// A single client's @ref gl_property_subscription "Property Subscription": a bounded queue of
/// values still waiting to be sent on that subscription's stream. Obtained from
/// ObservablePropertyManager::subscribe(); a Feature implementation's `Subscribe_<Prop>` RPC
/// handler drains it with waitForNext() and writes each value to the client.
///
/// Thread-safe: enqueue() (publisher side) and waitForNext()/cancel()
/// (consumer side) may run concurrently.
class Subscription {
public:
    /// @param maxDepth Maximum number of queued values before policy applies.
    /// @param policy What to do when a new value arrives with the queue at maxDepth.
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

    /// @return true if cancel() has been called on this Subscription.
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

/// Manages the @ref gl_property_subscription "Property Subscriptions" for every
/// @ref gl_observable_property "Observable Property" a Feature implementation exposes, with
/// bounded per-subscriber queues for backpressure (architecture.md §3.3). A Feature calls
/// publish() whenever a property's value changes; its `Subscribe_<Prop>` RPC handler calls
/// subscribe() to register the client and get back the Subscription to stream from. Shared
/// across Feature implementations and the RecoverableErrorGate.
///
/// Thread-safe: all public methods lock an internal mutex.
class ObservablePropertyManager {
public:
    /// @param defaultQueueDepth Maximum number of queued values each Subscription
    ///        created by subscribe() may hold before its OverflowPolicy applies.
    explicit ObservablePropertyManager(std::size_t defaultQueueDepth = 16);
    ~ObservablePropertyManager();

    /// Register a subscriber for propertyId.
    /// A new subscriber receives the property's current value immediately, before
    /// any change (SiLA Part A: an Observable Property "can be read at any time",
    /// yet the only RPC generated for it is Subscribe_<Prop>, so the current value
    /// MUST arrive on subscribe). That value is, in order of preference: an
    /// explicit initialValue if the caller supplies one, else the last value seen
    /// by publish(). Either is enqueued atomically with subscription creation so
    /// no publish can slip between registration and the initial push. Before the
    /// first publish and with no explicit initialValue, nothing is pushed (the
    /// property has no value yet).
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

    /// @return Number of active subscribers for propertyId.
    [[nodiscard("caller expects the subscriber count")]]
    std::size_t subscriberCount(const std::string& propertyId) const;

private:
    const std::size_t defaultQueueDepth_;
    mutable std::mutex mu_;
    bool shuttingDown_{false};
    std::unordered_map<std::string,
                       std::vector<std::shared_ptr<Subscription>>> subscribers_;
    // Last value published per property, replayed to each new subscriber as its
    // initial value. Retained even when a property has zero subscribers -- that
    // is precisely the case the initial push exists for: a value published
    // before anyone subscribed (e.g. a recoverable error raised before a client
    // opened its subscription) would otherwise be lost to a later subscriber.
    std::unordered_map<std::string, std::any> lastValues_;
};

}  // namespace sila2
