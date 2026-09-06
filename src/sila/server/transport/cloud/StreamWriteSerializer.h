// StreamWriteSerializer.h — Thread-safe writer for a single bidi gRPC stream (architecture.md §3.9)
#pragma once

#include "SiLACloudConnector.grpc.pb.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sila2 {

namespace cloud = org::silastandard;

class StreamWriteSerializer {
public:
    // cancelFn: called when a write exceeds writeTimeout — typically
    // context_->TryCancel(), which unblocks the hung Write() and tears
    // down the stream so CloudTransport can reconnect.
    StreamWriteSerializer(
        grpc::ClientReaderWriter<cloud::SiLAServerMessage, cloud::SiLAClientMessage>* stream,
        std::chrono::seconds writeTimeout = std::chrono::seconds{0},
        std::function<void()> cancelFn = {});

    // Shared-ownership overload — the one CloudTransport uses. The serializer
    // outlives the code that opened the stream (a detached subscription thread
    // keeps a shared_ptr to it), and CloudTransport resets stream_ on
    // disconnect/reconnect, so the serializer must keep the stream alive
    // itself (§2.2l). The raw-pointer overload above is for callers that
    // themselves own the stream for at least the serializer's lifetime — that
    // must hold by construction (a scope the caller controls, e.g.
    // test_stream_write_serializer.cc's stack-local stream_), not by
    // assumption: CloudRouterTestHarness.h used to build from a unique_ptr
    // member's .get() while handing the serializer's shared_ptr to route()'s
    // detached thread, which could outlive TearDown()'s stream_ destruction.
    // Fixed there by switching that harness to this overload too.
    StreamWriteSerializer(
        std::shared_ptr<grpc::ClientReaderWriter<cloud::SiLAServerMessage, cloud::SiLAClientMessage>> stream,
        std::chrono::seconds writeTimeout = std::chrono::seconds{0},
        std::function<void()> cancelFn = {})
        : StreamWriteSerializer(stream.get(), writeTimeout, std::move(cancelFn)) {
        streamOwnership_ = std::move(stream);
    }

    ~StreamWriteSerializer();

    StreamWriteSerializer(const StreamWriteSerializer&) = delete;
    StreamWriteSerializer& operator=(const StreamWriteSerializer&) = delete;
    StreamWriteSerializer(StreamWriteSerializer&&) = delete;
    StreamWriteSerializer& operator=(StreamWriteSerializer&&) = delete;

    bool write(const cloud::SiLAServerMessage& msg);

private:
    friend struct StreamWriteSerializerTestAccess;

    grpc::ClientReaderWriter<cloud::SiLAServerMessage, cloud::SiLAClientMessage>* stream_;
    std::mutex mu_;
    std::chrono::seconds writeTimeout_;
    // Declared before streamOwnership_ and therefore destroyed after it:
    // cancelFn_ can hold the last shared_ptr to the ClientContext/Channel that
    // the ClientReaderWriter still points at internally by raw pointer, so the
    // stream must be torn down first — reversing this would free the context
    // out from under a still-live stream. Members are destroyed in reverse
    // declaration order.
    std::function<void()> cancelFn_;
    // Null when constructed from a raw pointer; otherwise keeps *stream_ alive.
    std::shared_ptr<grpc::ClientReaderWriter<cloud::SiLAServerMessage, cloud::SiLAClientMessage>>
        streamOwnership_;

    // Reusable watchdog thread: one thread per serializer instead of one
    // per write() call, parked on wdCv_ between writes.
    std::mutex wdMu_;
    std::condition_variable wdCv_;
    enum class WdState { kIdle, kWatching, kDone, kShutdown };
    WdState wdState_{WdState::kIdle};
    uint64_t wdGen_{0};
    std::thread watchdog_;
};

}  // namespace sila2
