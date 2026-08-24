#include "ObservableCommandManager.h"

#include "ObservableCommandExecution.h"

#include <sila/error/SiLAErrorSubtypes.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <utility>

namespace sila2 {

ObservableCommandManager::ObservableCommandManager() = default;

// Defined here, not defaulted in the header: at the point the header is
// parsed, ObservableCommandExecution is only forward-declared, so
// unique_ptr's default deleter (which needs sizeof(T)) cannot be
// instantiated yet. By the time this .cc includes ObservableCommandExecution.h,
// the type is complete and commands_'s implicit destruction is well-formed.
ObservableCommandManager::~ObservableCommandManager() {
    stopAutoGC();
}

std::string ObservableCommandManager::generateUuid() {
    // thread_local: each thread seeds its own engine once, on first call,
    // instead of contending on mu_ (or a separate lock) just to draw random
    // bits. random_device is only used as a one-time seed, not per-call,
    // since it can be slow and some platforms limit how often it may be read.
    thread_local std::mt19937_64 engine{std::random_device{}()};
    uint64_t hi = engine();
    uint64_t lo = engine();

    // RFC 4122 §4.4 UUID v4 layout: hi holds time_low/time_mid/time_hi_and_version,
    // lo holds clock_seq_hi_and_reserved/clock_seq_low/node. Force the version
    // nibble (bits 12-15 of hi) to 0100, and the variant bits (top 2 bits of
    // lo) to 10, leaving every other bit random.
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(hi >> 32),
        static_cast<unsigned>((hi >> 16) & 0xFFFF),
        static_cast<unsigned>(hi & 0xFFFF),
        static_cast<unsigned>(lo >> 48),
        static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

ObservableCommandExecution& ObservableCommandManager::addCommand(std::chrono::seconds lifetime) {
    std::string uuid = generateUuid();
    // Construct the execution before taking mu_: the constructor does no
    // shared-state work, so there is nothing to protect yet, and keeping it
    // out of the critical section means the lock is only ever held for the
    // map mutation itself.
    auto execution = std::make_unique<ObservableCommandExecution>(uuid, lifetime);

    std::lock_guard<std::mutex> lock{mu_};
    auto [it, inserted] = commands_.emplace(std::move(uuid), std::move(execution));
    return *it->second;
}

ObservableCommandExecution& ObservableCommandManager::getCommand(const std::string& uuid) {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = commands_.find(uuid);
    if (it == commands_.end()) {
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid};
    }
    return *it->second;
}

std::size_t ObservableCommandManager::removeExpired() {
    std::lock_guard<std::mutex> lock{mu_};
    std::size_t removed = 0;
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (it->second->isExpired()) {
            it = commands_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void ObservableCommandManager::interruptAll() {
    std::lock_guard<std::mutex> lock{mu_};
    for (auto& [uuid, execution] : commands_) { execution->requestInterruption(); }
}

void ObservableCommandManager::startAutoGC(std::chrono::seconds interval) {
    if (gcRunning_.exchange(true)) {
        return;
    }
    gcThread_ = std::thread{[this, interval] {
        std::unique_lock<std::mutex> lock{gcMu_};
        while (gcRunning_.load()) {
            gcCv_.wait_for(lock, interval, [this] { return !gcRunning_.load(); });
            if (gcRunning_.load()) {
                removeExpired();
            }
        }
    }};
}

void ObservableCommandManager::stopAutoGC() {
    if (!gcRunning_.exchange(false)) {
        return;
    }
    gcCv_.notify_all();
    if (gcThread_.joinable()) {
        gcThread_.join();
    }
}

bool ObservableCommandManager::isAutoGCRunning() const {
    return gcRunning_.load();
}

std::size_t ObservableCommandManager::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return commands_.size();
}

}  // namespace sila2
