#include "CallContext.h"

namespace sila2 {

CallContext::CallContext() = default;

CallContext::CallContext(CancellationProbe probe)
    : cancellationProbe_{std::move(probe)} {}

CallContext::~CallContext() = default;

void CallContext::setMetadata(const std::string& key, std::string value) {
    std::lock_guard<std::mutex> lock(mu_);
    metadata_[key] = std::move(value);
}

std::optional<std::string> CallContext::metadata(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = metadata_.find(key);
    if (it == metadata_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void CallContext::requestCancellation(CancellationReason reason) {
    cancellationReason_ = reason;
    cancelled_ = true;

    std::unique_lock<std::mutex> lock(mu_);
    if (callbackFired_ || !cancellationCallback_) return;
    callbackFired_ = true;
    CancellationCallback callback = cancellationCallback_;
    lock.unlock();

    callback();
}

CallContext::CancellationReason CallContext::cancellationReason() const {
    return cancellationReason_;
}

bool CallContext::isCancelled() const {
    // Three independent sources, cheapest first. cancelled_ is the cloud
    // transport's only signal (ActiveCallRegistry pushes it); the probe is the
    // direct-gRPC transport's only one, because gRPC C++ offers no callback
    // and a snapshot taken at dispatch time can never go true afterwards.
    if (cancelled_) {
        return true;
    }
    if (isDeadlineExceeded()) {
        return true;
    }
    // cancellationProbe_ is const after construction, so reading it needs no
    // lock; it is called with mu_ unheld because it reaches into the transport
    // (§2.2c: never hold mu_ across a call that leaves this class).
    return cancellationProbe_ && cancellationProbe_();
}

void CallContext::onCancellation(CancellationCallback callback) {
    std::unique_lock<std::mutex> lock(mu_);
    cancellationCallback_ = callback;

    if (cancelled_ && !callbackFired_) {
        callbackFired_ = true;
        lock.unlock();
        callback();
    }
}

void CallContext::setDeadline(std::chrono::steady_clock::time_point deadline) {
    deadline_ = deadline;
}

std::chrono::steady_clock::time_point CallContext::deadline() const {
    return deadline_;
}

bool CallContext::isDeadlineExceeded() const {
    // Goes through the public accessor so deadline_ has exactly one read path
    // rather than two that could drift apart.
    return std::chrono::steady_clock::now() >= deadline();
}

}  // namespace sila2
