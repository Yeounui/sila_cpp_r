#include "ObservablePropertyManager.h"

#include <algorithm>
#include <utility>

namespace sila2 {

Subscription::Subscription(std::size_t maxDepth, OverflowPolicy policy)
    : maxDepth_{maxDepth}, policy_{policy} {}

bool Subscription::enqueue(std::any value) {
    std::lock_guard<std::mutex> lock{mu_};
    if (policy_ == OverflowPolicy::DiscardOldest) {
        while (queue_.size() >= maxDepth_) { queue_.pop_front(); }
        queue_.push_back(std::move(value));
        cv_.notify_one();
        return true;
    }

    // TerminateOnFull
    if (queue_.size() >= maxDepth_) { return false; }

    queue_.push_back(std::move(value));
    cv_.notify_one();
    return true;
}

std::optional<std::any> Subscription::waitForNext() {
    std::unique_lock<std::mutex> lock{mu_};
    cv_.wait(lock, [this] { return cancelled_.load() || !queue_.empty(); });
    if (cancelled_.load()) { return std::nullopt; }
    auto value = std::move(queue_.front());
    queue_.pop_front();
    return value;
}

void Subscription::cancel() {
    // The flag must be set under mu_ even though it is atomic: waitForNext()
    // evaluates it as a cv_ predicate while holding mu_, so a store landing
    // between that check and the atomic unlock-and-block would be missed and
    // the waiter would block forever (cv_.wait has no timeout).
    {
        std::lock_guard<std::mutex> lock{mu_};
        cancelled_.store(true);
    }
    cv_.notify_all();
}

bool Subscription::isCancelled() const { return cancelled_.load(); }

ObservablePropertyManager::ObservablePropertyManager(std::size_t defaultQueueDepth)
    : defaultQueueDepth_{defaultQueueDepth} {}

ObservablePropertyManager::~ObservablePropertyManager() {
    shutdown();
}

std::shared_ptr<Subscription> ObservablePropertyManager::subscribe(
    const std::string& propertyId, std::any initialValue, OverflowPolicy policy) {
    std::lock_guard<std::mutex> lock{mu_};
    auto sub = std::make_shared<Subscription>(defaultQueueDepth_, policy);
    if (shuttingDown_) {
        sub->cancel();
        return sub;
    }
    // Push the current value before any change. Explicit initialValue wins;
    // otherwise replay the last published value. Both run under mu_ together
    // with the push_back below, so a concurrent publish() cannot land between
    // the initial push and registration.
    if (initialValue.has_value()) {
        sub->enqueue(std::move(initialValue));
    } else if (auto it = lastValues_.find(propertyId); it != lastValues_.end()) {
        sub->enqueue(it->second);
    }
    subscribers_[propertyId].push_back(sub);
    return sub;
}

void ObservablePropertyManager::publish(const std::string& propertyId, std::any value) {
    std::lock_guard<std::mutex> lock{mu_};
    // Retain the current value first, before the no-subscribers early return:
    // a value published with zero subscribers must still reach whoever
    // subscribes next (subscribe() replays it).
    lastValues_[propertyId] = value;
    auto it = subscribers_.find(propertyId);
    if (it == subscribers_.end()) {
        return;
    }

    auto& subs = it->second;
    for (auto subIt = subs.begin(); subIt != subs.end();) {
        // Prune subscribers cancelled elsewhere (e.g. CloudEnvelopeRouter's
        // pump exit or ~CloudEnvelopeRouter): unsubscribe() is the normal
        // removal path, but nothing guarantees it always runs before the
        // manager outlives the subscriber, so publish() self-heals here too.
        if ((*subIt)->isCancelled()) {
            subIt = subs.erase(subIt);
            continue;
        }
        // Each subscriber gets its own std::any: the queue stores by value and
        // consumers pop independently, so the value must be copied per subscriber.
        if (!(*subIt)->enqueue(value)) {
            (*subIt)->cancel();
            subIt = subs.erase(subIt);
        } else {
            ++subIt;
        }
    }
}

void ObservablePropertyManager::unsubscribe(const std::string& propertyId,
                                             const std::shared_ptr<Subscription>& sub) {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = subscribers_.find(propertyId);
    if (it == subscribers_.end()) {
        return;
    }
    auto& subs = it->second;
    subs.erase(std::remove(subs.begin(), subs.end(), sub), subs.end());
}

void ObservablePropertyManager::cancelAll(const std::string& propertyId) {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = subscribers_.find(propertyId);
    if (it == subscribers_.end()) {
        return;
    }
    for (auto& sub : it->second) { sub->cancel(); }
    subscribers_.erase(it);
}

void ObservablePropertyManager::shutdown() {
    std::lock_guard<std::mutex> lock{mu_};
    shuttingDown_ = true;
    for (auto& [propertyId, subs] : subscribers_) {
        for (auto& sub : subs) { sub->cancel(); }
    }
    subscribers_.clear();
    lastValues_.clear();
}

std::size_t ObservablePropertyManager::subscriberCount(const std::string& propertyId) const {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = subscribers_.find(propertyId);
    return it == subscribers_.end() ? 0 : it->second.size();
}

}  // namespace sila2
