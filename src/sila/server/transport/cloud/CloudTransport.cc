// CloudTransport.cc
#include "CloudTransport.h"

#include <sila/client/binary/BinaryRetry.h>
#include <sila/server/SilaServerBase.h>

namespace sila2 {

void CloudTransport::openStream() {
    std::string target = host_ + ":" + std::to_string(port_);
    // This channel — not the inbound listener in SilaServerBase::Run — is the
    // one holding a stream open for hours in server-initiated mode, so it is
    // where keepalive has to live. Values mirror that listener's; change both
    // together. GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA is left at its default
    // of 2, which caps pings on an idle stream: a dead peer is detected within
    // ~2 ping periods without accumulating enough strikes to earn an
    // ENHANCE_YOUR_CALM GOAWAY from a default-configured endpoint.
    // MIN_RECV_PING_INTERVAL_WITHOUT_DATA is deliberately absent — it is a
    // server-side argument and has no effect on a client channel.
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 60000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    // Same cap as the direct gRPC path: an envelope carrying an inlined binary
    // must not be accepted on one transport and rejected on the other.
    args.SetMaxReceiveMessageSize(kMaxReceiveMessageSizeBytes);
    channel_ = grpc::CreateCustomChannel(target, creds_, args);
    stub_ = cloud::CloudClientEndpoint::NewStub(channel_);
    context_ = std::make_shared<grpc::ClientContext>();
    stream_ = stub_->ConnectSiLAServer(context_.get());

    auto timeout = router_.cloudWriteTimeout();
    // The serializer takes shared ownership of the stream, and the cancel
    // callback carries the context and channel it needs — so a writer that
    // outlives this transport's members (detached subscription thread) still
    // has every object the write path touches (§2.2l).
    writer_ = std::make_shared<StreamWriteSerializer>(
        stream_, timeout,
        [ctx = context_, chan = channel_] { ctx->TryCancel(); });
}

void CloudTransport::connect() {
    std::lock_guard<std::mutex> lock(mu_);
    // receiveThread_ stays joinable (still running reconnect()'s backoff loop)
    // for a window after a stream break sets connected_ = false, so checking
    // connected_ alone lets a connect() call here assign over a live thread,
    // which calls std::terminate. Mirrors disconnect()'s existing guard.
    if (connected_ || receiveThread_.joinable()) return;
    stop_ = false;

    openStream();

    connected_ = true;
    receiveThread_ = std::thread{&CloudTransport::receiveLoop, this};
}

void CloudTransport::disconnect() {
    // Snapshot under mu_ so reconnect() cannot replace context_ between our
    // unlock and TryCancel (§3.2b race). Holding a shared_ptr rather than a
    // raw pointer also keeps the context alive across that window.
    std::shared_ptr<grpc::ClientContext> ctxSnapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!connected_ && !receiveThread_.joinable()) return;
        stop_ = true;
        ctxSnapshot = context_;
    }
    cv_.notify_all();

    // TryCancel wakes up any blocking Read
    if (ctxSnapshot) {
        ctxSnapshot->TryCancel();
    }

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    calls_.cancelAll();

    std::lock_guard<std::mutex> lock(mu_);
    writer_.reset();
    stream_.reset();
    context_.reset();
    stub_.reset();
    channel_.reset();
    connected_ = false;
}

void CloudTransport::receiveLoop() {
    cloud::SiLAClientMessage msg;
    while (!stop_.load()) {
        if (stream_->Read(&msg)) {
            try {
                router_.route(msg, *writer_, writer_, calls_);
            } catch (...) {
                // Safety net: never let a single bad message kill the server thread.
                // Binary-store errors are caught inside route() and converted to
                // BinaryTransferError envelopes; this catches anything else.
            }
        } else {
            // Stream broken
            connected_ = false;
            calls_.cancelAll();
            if (!stop_.load()) {
                reconnect();
            }
        }
    }
}

void CloudTransport::reconnect() {
    using namespace std::chrono;
    constexpr auto maxBackoff = seconds{60};
    int attempt = 0;

    while (!stop_.load()) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, backoffDelay(attempt, maxBackoff),
                         [this] { return stop_.load(); });
        }
        if (stop_.load()) break;

        // Hold mu_ across openStream() so disconnect()'s snapshot sees a
        // consistent context_ (mirrors connect(), which also locks).
        // Re-check stop_ under mu_ to close the TOCTOU gap with disconnect():
        // without it, disconnect could snapshot context_ between line 98's
        // check and this lock acquisition, then openStream() would destroy
        // the snapshotted pointer.
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_.load()) return;
            openStream();
        }

        // Try a Read to verify the stream is alive
        cloud::SiLAClientMessage msg;
        if (stream_->Read(&msg)) {
            connected_ = true;
            try {
                router_.route(msg, *writer_, writer_, calls_);
            } catch (...) {}
            return;  // back to the outer receiveLoop
        }

        ++attempt;
    }
}

}  // namespace sila2
