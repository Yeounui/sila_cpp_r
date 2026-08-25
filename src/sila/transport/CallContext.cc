#include "CallContext.h"

namespace sila2 {

CallContext::CallContext() = default;
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

const std::unordered_map<std::string, std::string>& CallContext::allMetadata() const {
    std::lock_guard<std::mutex> lock(mu_);
    return metadata_;
}

void CallContext::requestCancellation() {
    cancelled_ = true;

    std::unique_lock<std::mutex> lock(mu_);
    CancellationCallback callback = cancellationCallback_;
    lock.unlock();

    if (callback) {
        callback();
    }
}

bool CallContext::isCancelled() const {
    return cancelled_;
}

void CallContext::onCancellation(CancellationCallback callback) {
    std::unique_lock<std::mutex> lock(mu_);
    cancellationCallback_ = callback;

    if (cancelled_) {
        lock.unlock();
        callback();
    }
}

void CallContext::setDeadline(std::chrono::steady_clock::time_point deadline) {
    std::lock_guard<std::mutex> lock(mu_);
    deadline_ = deadline;
}

std::chrono::steady_clock::time_point CallContext::deadline() const {
    std::lock_guard<std::mutex> lock(mu_);
    return deadline_;
}

bool CallContext::isDeadlineExceeded() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::chrono::steady_clock::now() >= deadline_;
}

}  // namespace sila2
