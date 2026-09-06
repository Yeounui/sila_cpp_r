// End-to-end tests for StreamWriteSerializer::write() (architecture.md
// §3.9): the write-timeout watchdog that cancels a stuck gRPC Write() via
// cancelFn_, and the passthrough path taken when no timeout is configured.
//
// Drives write() against a real bidi stream to a local gRPC server so the
// watchdog's flow-control-backpressure scenario is genuine, not mocked.
#include <sila/server/transport/cloud/StreamWriteSerializer.h>

#include "SiLACloudConnector.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace sila2 {

struct StreamWriteSerializerTestAccess {
    static void watch(StreamWriteSerializer& serializer, uint64_t generation) {
        std::lock_guard<std::mutex> lock{serializer.wdMu_};
        serializer.wdGen_ = generation;
        serializer.wdState_ = StreamWriteSerializer::WdState::kWatching;
        serializer.wdCv_.notify_one();
    }
};

}  // namespace sila2

namespace {

namespace cloud = sila2::org::silastandard;

// Minimal CloudClientEndpoint peer used to drive a real bidi stream.
// `drain` controls whether the fake peer actively reads incoming writes:
// disabling reads is what makes Write() eventually block on HTTP/2 flow
// control, which the watchdog-timeout tests below rely on.
class StubEndpoint final : public cloud::CloudClientEndpoint::Service {
public:
    explicit StubEndpoint(bool drain) : drain_(drain) {}

    grpc::Status ConnectSiLAServer(
        grpc::ServerContext*,
        grpc::ServerReaderWriter<cloud::SiLAClientMessage, cloud::SiLAServerMessage>* stream) override {
        if (drain_) {
            cloud::SiLAServerMessage msg;
            while (stream->Read(&msg)) {}
        } else {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return release_; });
        }
        return grpc::Status::OK;
    }

    void release() {
        std::lock_guard<std::mutex> lock(mu_);
        release_ = true;
        cv_.notify_all();
    }

private:
    bool drain_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool release_ = false;
};

// StubEndpoint that records every message it reads, for the concurrent-writes
// test below: a drain=true peer that just discards can't tell whether
// messages arrived intact or interleaved/corrupted.
class RecordingEndpoint final : public cloud::CloudClientEndpoint::Service {
public:
    grpc::Status ConnectSiLAServer(
        grpc::ServerContext*,
        grpc::ServerReaderWriter<cloud::SiLAClientMessage, cloud::SiLAServerMessage>* stream) override {
        cloud::SiLAServerMessage msg;
        while (stream->Read(&msg)) {
            std::lock_guard<std::mutex> lock(mu_);
            received_.push_back(msg.requestuuid());
        }
        return grpc::Status::OK;
    }

    std::vector<std::string> received() const {
        std::lock_guard<std::mutex> lock(mu_);
        return received_;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> received_;
};

class StreamWriteSerializerFixture : public ::testing::Test {
protected:
    // drain=true: peer reads and discards every write (normal-write tests).
    // drain=false: peer never reads, so a large enough write blocks on flow
    // control until something cancels the stream (timeout-watchdog tests).
    void start(bool drain) {
        endpoint_ = std::make_unique<StubEndpoint>(drain);
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(endpoint_.get());
        server_ = builder.BuildAndStart();

        channel_ = grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub_ = cloud::CloudClientEndpoint::NewStub(channel_);
        ctx_ = std::make_unique<grpc::ClientContext>();
        stream_ = stub_->ConnectSiLAServer(ctx_.get());
    }

    void TearDown() override {
        if (endpoint_) endpoint_->release();
        if (ctx_) ctx_->TryCancel();
        if (server_) server_->Shutdown();
    }

    std::unique_ptr<StubEndpoint> endpoint_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<cloud::CloudClientEndpoint::Stub> stub_;
    std::unique_ptr<grpc::ClientContext> ctx_;
    std::unique_ptr<grpc::ClientReaderWriter<cloud::SiLAServerMessage, cloud::SiLAClientMessage>> stream_;
};

// ---------------------------------------------------------------------------
// Positive (True) paths
// ---------------------------------------------------------------------------

// A write that completes well within the configured timeout succeeds and
// never triggers the watchdog's cancelFn_.
TEST_F(StreamWriteSerializerFixture, SmallWriteWithinTimeoutSucceedsWithoutCancelling) {
    start(/*drain=*/true);
    std::atomic<bool> cancelCalled{false};
    sila2::StreamWriteSerializer sws{stream_.get(), std::chrono::seconds{2},
                                      [&] { cancelCalled = true; }};

    cloud::SiLAServerMessage msg;
    msg.set_requestuuid("req-small");
    msg.mutable_unobservablecommandresponse()->set_response("ok");

    EXPECT_TRUE(sws.write(msg));
    EXPECT_FALSE(cancelCalled.load());
}

// writeTimeout <= 0 disables the watchdog entirely (falls through to a raw
// stream_->Write), even though a cancelFn_ is supplied.
TEST_F(StreamWriteSerializerFixture, ZeroTimeoutDisablesWatchdogAndWriteSucceeds) {
    start(/*drain=*/true);
    std::atomic<bool> cancelCalled{false};
    sila2::StreamWriteSerializer sws{stream_.get(), std::chrono::seconds{0},
                                      [&] { cancelCalled = true; }};

    cloud::SiLAServerMessage msg;
    msg.set_requestuuid("req-zero-timeout");

    EXPECT_TRUE(sws.write(msg));
    EXPECT_FALSE(cancelCalled.load());
}

// An empty cancelFn_ also disables the watchdog, independent of the
// configured timeout value (the `!cancelFn_` half of the guard clause).
TEST_F(StreamWriteSerializerFixture, EmptyCancelFnDisablesWatchdogEvenWithTimeoutSet) {
    start(/*drain=*/true);
    sila2::StreamWriteSerializer sws{stream_.get(), std::chrono::seconds{2}, /*cancelFn=*/{}};

    cloud::SiLAServerMessage msg;
    msg.set_requestuuid("req-no-cancelfn");

    EXPECT_TRUE(sws.write(msg));
}

// ---------------------------------------------------------------------------
// Negative (False) paths
// ---------------------------------------------------------------------------

// CAUGHT: a write on an already-cancelled context fails immediately —
// stream_->Write() itself returns false, so the watchdog never needs to fire.
TEST_F(StreamWriteSerializerFixture, WriteOnCancelledContextFailsFastWithoutWatchdog) {
    start(/*drain=*/true);
    ctx_->TryCancel();
    // Let cancellation propagate before the write attempt observes it.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    std::atomic<bool> cancelCalled{false};
    sila2::StreamWriteSerializer sws{stream_.get(), std::chrono::seconds{5},
                                      [&] { cancelCalled = true; }};

    cloud::SiLAServerMessage msg;
    msg.set_requestuuid("req-cancelled");

    auto start_time = std::chrono::steady_clock::now();
    bool ok = sws.write(msg);
    auto elapsed = std::chrono::steady_clock::now() - start_time;

    EXPECT_FALSE(ok);
    EXPECT_FALSE(cancelCalled.load());
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
}

// CAUGHT: the documented timeout scenario — a write that blocks on flow
// control longer than writeTimeout_ triggers cancelFn_, which unblocks the
// stuck Write() and makes it return false.
TEST_F(StreamWriteSerializerFixture, WriteBlockingPastTimeoutTriggersCancelFn) {
    start(/*drain=*/false);  // peer never reads -> flow control fills up
    std::atomic<bool> cancelCalled{false};
    sila2::StreamWriteSerializer sws{stream_.get(), std::chrono::seconds{1},
                                      [&] { cancelCalled = true; ctx_->TryCancel(); }};

    // Large enough to exceed the stream's flow-control window with nobody
    // draining it, so the single Write() call genuinely blocks.
    std::string bigPayload(8 * 1024 * 1024, 'x');
    cloud::SiLAServerMessage msg;
    msg.set_requestuuid("req-blocked");
    msg.mutable_binarytransfererror()->set_message(bigPayload);

    auto start_time = std::chrono::steady_clock::now();
    bool ok = sws.write(msg);
    auto elapsed = std::chrono::steady_clock::now() - start_time;

    EXPECT_FALSE(ok);
    EXPECT_TRUE(cancelCalled.load());
    EXPECT_GE(elapsed, std::chrono::milliseconds{900});
    EXPECT_LT(elapsed, std::chrono::milliseconds{5000});
}

TEST(StreamWriteSerializerWatchdog, TimeoutRearmsWhenTheNextGenerationIsWatching) {
    std::atomic<int> cancellations{0};
    sila2::StreamWriteSerializer* serializer = nullptr;
    sila2::StreamWriteSerializer sws{nullptr, std::chrono::seconds{1}, [&] {
        if (cancellations.fetch_add(1) == 0) {
            sila2::StreamWriteSerializerTestAccess::watch(*serializer, 2);
        }
    }};
    serializer = &sws;
    sila2::StreamWriteSerializerTestAccess::watch(sws, 1);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (cancellations.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    EXPECT_EQ(cancellations.load(), 2);
}

// CAUGHT (documents a real gap): an empty cancelFn_ disables the watchdog
// (see EmptyCancelFnDisablesWatchdogEvenWithTimeoutSet above), so a blocked
// write has nothing to unblock it on its own — writeTimeout_ is configured
// but silently ineffective. Only an external cancellation (as
// CloudTransport::disconnect() performs) unblocks it.
TEST_F(StreamWriteSerializerFixture, WriteWithoutCancelFnBlocksUntilExternallyCancelled) {
    start(/*drain=*/false);
    sila2::StreamWriteSerializer sws{stream_.get(), std::chrono::seconds{1}, /*cancelFn=*/{}};

    std::string bigPayload(8 * 1024 * 1024, 'x');
    cloud::SiLAServerMessage msg;
    msg.set_requestuuid("req-no-cancelfn-blocked");
    msg.mutable_binarytransfererror()->set_message(bigPayload);

    std::promise<bool> resultPromise;
    auto resultFuture = resultPromise.get_future();
    std::thread writer([&] { resultPromise.set_value(sws.write(msg)); });

    // The configured 1s timeout has no effect without a cancelFn_: the write
    // is still blocked well past it.
    EXPECT_EQ(resultFuture.wait_for(std::chrono::milliseconds{1500}), std::future_status::timeout);

    // Only an external cancellation (standing in for CloudTransport's own
    // ctx->TryCancel() during disconnect()) unblocks it.
    ctx_->TryCancel();
    EXPECT_FALSE(resultFuture.get());
    writer.join();
}

// ---------------------------------------------------------------------------
// Concurrency: two threads calling write() on the same serializer at once.
// Not using the StubEndpoint fixture: StubEndpoint discards what it reads,
// but this test needs the peer to record every requestuuid it saw.
// ---------------------------------------------------------------------------

TEST(StreamWriteSerializerConcurrent, ConcurrentWritesAllDelivered) {
    RecordingEndpoint endpoint;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&endpoint);
    auto server = builder.BuildAndStart();

    auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = cloud::CloudClientEndpoint::NewStub(channel);
    grpc::ClientContext ctx;
    auto stream = stub->ConnectSiLAServer(&ctx);

    sila2::StreamWriteSerializer sws{stream.get(), std::chrono::seconds{2}, /*cancelFn=*/{}};

    constexpr int kMessagesPerThread = 50;
    auto writer = [&](int threadId) {
        for (int seq = 0; seq < kMessagesPerThread; ++seq) {
            cloud::SiLAServerMessage msg;
            msg.set_requestuuid("t" + std::to_string(threadId) + "-" + std::to_string(seq));
            msg.mutable_unobservablecommandresponse()->set_response("ok");
            EXPECT_TRUE(sws.write(msg));
        }
    };
    std::thread writerA([&] { writer(0); });
    std::thread writerB([&] { writer(1); });
    writerA.join();
    writerB.join();

    // Half-close cleanly and wait for the server's read loop to drain
    // everything already in flight, instead of TryCancel()'ing the stream
    // out from under messages the client already considers written.
    stream->WritesDone();
    ASSERT_TRUE(stream->Finish().ok());
    server->Shutdown();

    const std::vector<std::string> received = endpoint.received();
    ASSERT_EQ(received.size(), static_cast<size_t>(2 * kMessagesPerThread));

    const std::unordered_set<std::string> uniqueIds(received.begin(), received.end());
    EXPECT_EQ(uniqueIds.size(), received.size()) << "duplicate or corrupted requestuuid seen";
}

}  // namespace
