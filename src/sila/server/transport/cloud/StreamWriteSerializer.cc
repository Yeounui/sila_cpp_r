// StreamWriteSerializer.cc
#include "StreamWriteSerializer.h"

#include <utility>

namespace sila2 {

StreamWriteSerializer::StreamWriteSerializer(
    grpc::ClientReaderWriter<cloud::SiLAServerMessage, cloud::SiLAClientMessage>* stream,
    std::chrono::seconds writeTimeout,
    std::function<void()> cancelFn)
    : stream_(stream), writeTimeout_(writeTimeout), cancelFn_(std::move(cancelFn)) {
    // gRPC sync Write() has no per-call timeout. A long-lived watchdog thread
    // calls cancelFn_ (context->TryCancel) if a write blocks too long, which
    // makes Write() return false and tears down the stream. The thread parks
    // on wdCv_ between writes instead of being spawned/joined per write().
    if (writeTimeout_.count() > 0 && cancelFn_) {
        watchdog_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(wdMu_);
            while (true) {
                wdCv_.wait(lk, [this] { return wdState_ != WdState::kIdle; });
                if (wdState_ == WdState::kShutdown) return;

                // Capture the generation so the timer is bound to THIS write.
                // If a fast write finishes and the next write sets kWatching
                // before we reacquire wdMu_, the gen mismatch wakes us and we
                // restart with a full timer for the new write.
                uint64_t gen = wdGen_;
                bool timedOut = !wdCv_.wait_for(lk, writeTimeout_,
                    [this, gen] {
                        return wdState_ != WdState::kWatching || wdGen_ != gen;
                    });
                if (wdState_ == WdState::kShutdown) return;

                if (wdGen_ != gen) {
                    // New write replaced this one — loop to start a fresh timer.
                    continue;
                }

                if (!timedOut) {
                    // write() already finished (kDone) before the timeout.
                    wdState_ = WdState::kIdle;
                    continue;
                }

                lk.unlock();
                cancelFn_();
                lk.lock();

                // Wait for the in-flight write() (unblocked by cancelFn_) to
                // record its outcome before rearming for the next write().
                wdCv_.wait(lk, [this, gen] {
                    return wdState_ != WdState::kWatching || wdGen_ != gen;
                });
                if (wdState_ == WdState::kShutdown) return;
                if (wdGen_ != gen) continue;
                wdState_ = WdState::kIdle;
            }
        });
    }
}

StreamWriteSerializer::~StreamWriteSerializer() {
    {
        std::lock_guard<std::mutex> lk(wdMu_);
        wdState_ = WdState::kShutdown;
    }
    wdCv_.notify_one();
    if (watchdog_.joinable()) watchdog_.join();
}

bool StreamWriteSerializer::write(const cloud::SiLAServerMessage& msg) {
    std::lock_guard<std::mutex> lock(mu_);

    if (writeTimeout_.count() <= 0 || !cancelFn_) {
        return stream_->Write(msg);
    }

    {
        std::lock_guard<std::mutex> wdLock(wdMu_);
        ++wdGen_;
        wdState_ = WdState::kWatching;
    }
    wdCv_.notify_one();

    bool ok = stream_->Write(msg);

    {
        std::lock_guard<std::mutex> wdLock(wdMu_);
        wdState_ = WdState::kDone;
    }
    wdCv_.notify_one();

    return ok;
}

}  // namespace sila2
