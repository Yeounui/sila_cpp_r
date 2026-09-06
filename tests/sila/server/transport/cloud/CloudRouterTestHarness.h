// CloudRouterTestHarness.h — shared test infrastructure for CloudEnvelopeRouter
// integration tests. Sets up an in-process gRPC server implementing
// CloudClientEndpoint so route()'s StreamWriteSerializer writes are captured
// and inspectable via a blocking queue.
#pragma once

#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/ActiveCallRegistry.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/cloud/StreamWriteSerializer.h>

#include "SiLACloudConnector.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace cloud_test {

namespace cloud = sila2::org::silastandard;

// Thread-safe queue for captured SiLAServerMessages written by route().
class ResponseQueue {
public:
    void push(cloud::SiLAServerMessage msg) {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push(std::move(msg));
        cv_.notify_one();
    }

    cloud::SiLAServerMessage pop(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        std::unique_lock<std::mutex> lock(mu_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            throw std::runtime_error("ResponseQueue::pop timed out");
        }
        auto val = std::move(queue_.front());
        queue_.pop();
        return val;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return queue_.empty();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        while (!queue_.empty()) queue_.pop();
    }

private:
    std::queue<cloud::SiLAServerMessage> queue_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
};

// Local gRPC server implementing CloudClientEndpoint. Reads all
// SiLAServerMessages written by route() and pushes them into captured.
class CapturingEndpoint final : public cloud::CloudClientEndpoint::Service {
public:
    grpc::Status ConnectSiLAServer(
        grpc::ServerContext*,
        grpc::ServerReaderWriter<cloud::SiLAClientMessage,
                                  cloud::SiLAServerMessage>* stream) override {
        cloud::SiLAServerMessage msg;
        while (stream->Read(&msg)) {
            captured.push(std::move(msg));
        }
        return grpc::Status::OK;
    }

    ResponseQueue captured;
};

// Base fixture: boots a local gRPC server and connects a bidi stream so
// route() can write through a real StreamWriteSerializer. Subclass or use
// directly.
class CloudRouterFixture : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort(
            "127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&endpoint_);
        server_ = builder.BuildAndStart();

        channel_ = grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port),
            grpc::InsecureChannelCredentials());
        stub_ = cloud::CloudClientEndpoint::NewStub(channel_);
        clientCtx_ = std::make_shared<grpc::ClientContext>();
        stream_ = stub_->ConnectSiLAServer(clientCtx_.get());
        // Cancel callback carries the ClientContext and Channel it needs, same
        // as CloudTransport::openStream() — otherwise a detached subscription
        // thread outliving TearDown() writes through a stream whose context
        // and channel are already gone (§2.2l).
        writer_ = std::make_shared<sila2::StreamWriteSerializer>(
            stream_, std::chrono::seconds{0},
            [ctx = clientCtx_, chan = channel_] { ctx->TryCancel(); });
    }

    void TearDown() override {
        if (stream_) {
            stream_->WritesDone();
            stream_->Finish();
        }
        if (server_) {
            server_->Shutdown();
        }
    }

    // Pop the next response written by route(). Throws on timeout.
    cloud::SiLAServerMessage popResponse() {
        return endpoint_.captured.pop();
    }

    // Returns true if no response was written within a short window.
    bool noResponse(std::chrono::milliseconds wait = std::chrono::milliseconds{100}) {
        std::this_thread::sleep_for(wait);
        return endpoint_.captured.empty();
    }

    CapturingEndpoint endpoint_;
    sila2::ActiveCallRegistry calls_;

    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<cloud::CloudClientEndpoint::Stub> stub_;
    // Shared, not unique: the cancelFn lambda passed to StreamWriteSerializer
    // above holds a copy, so a detached subscription thread keeps this alive
    // past TearDown() too, matching stream_ below.
    std::shared_ptr<grpc::ClientContext> clientCtx_;
    // Shared, not unique: writer_ takes shared ownership below (matching
    // CloudTransport's production wiring), so a detached subscription thread
    // holding a copy of writer_ past TearDown keeps the stream alive too,
    // instead of writing through a dangling pointer.
    std::shared_ptr<grpc::ClientReaderWriter<cloud::SiLAServerMessage,
                                              cloud::SiLAClientMessage>> stream_;
    std::shared_ptr<sila2::StreamWriteSerializer> writer_;
};

}  // namespace cloud_test
