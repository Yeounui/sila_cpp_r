// ActiveCallRegistry.cc
#include "ActiveCallRegistry.h"

#include <iterator>
#include <vector>

namespace sila2 {

void ActiveCallRegistry::add(const std::string& requestUUID, std::shared_ptr<CallContext> ctx) {
    std::lock_guard<std::mutex> lock(mu_);
    // Prune before inserting: callers that finish synchronously drop their
    // context on return, and a detached subscription thread drops its own
    // when it exits, so expired entries are the only ones left behind.
    // The scan stays O(live calls) because every add() clears all the dead.
    for (auto it = calls_.begin(); it != calls_.end();) {
        it = it->second.expired() ? calls_.erase(it) : std::next(it);
    }
    calls_[requestUUID] = ctx;
}

void ActiveCallRegistry::remove(const std::string& requestUUID) {
    std::lock_guard<std::mutex> lock(mu_);
    calls_.erase(requestUUID);
}

std::shared_ptr<CallContext> ActiveCallRegistry::find(const std::string& requestUUID) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = calls_.find(requestUUID);
    if (it == calls_.end()) {
        return nullptr;
    }
    return it->second.lock();
}

void ActiveCallRegistry::cancel(const std::string& requestUUID) {
    auto ctx = find(requestUUID);
    if (ctx) {
        ctx->requestCancellation();
    }
}

void ActiveCallRegistry::cancelAll() {
    // Collect live shared_ptrs under lock, invoke outside (§2.2c).
    // Mirrors cancel() which already calls requestCancellation() unlocked.
    std::vector<std::shared_ptr<CallContext>> active;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [requestUUID, weakCtx] : calls_) {
            if (auto ctx = weakCtx.lock()) {
                active.push_back(std::move(ctx));
            }
        }
        calls_.clear();
    }
    // kConnectionLost, not the kClientRequested cancel() uses: this fires when
    // the shared Connection itself broke (CloudTransport::receiveLoop), sweeping
    // every live call at once -- not a per-call cancel any one of them asked
    // for. Callers that must keep executing across a lost Connection (§3.3)
    // read this reason back via CallContext::cancellationReason().
    for (auto& ctx : active) {
        ctx->requestCancellation(CallContext::CancellationReason::kConnectionLost);
    }
}

}  // namespace sila2
