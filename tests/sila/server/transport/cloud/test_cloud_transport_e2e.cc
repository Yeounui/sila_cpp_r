// End-to-end tests for CloudTransport (architecture.md §3.9): the
// receiveLoop() that reads SiLAClientMessages off the server-initiated
// bidi stream and routes each through CloudEnvelopeRouter, the reconnect()
// exponential-backoff retry loop that runs after a stream break, and
// disconnect()'s teardown of the connection and in-flight calls.
//
// CloudTransport connects OUT to a SiLA Client's CloudClientEndpoint, so the
// fixture below hosts a local gRPC server implementing that service —
// playing the role of the remote cloud client — and drives/observes the
// bidi stream from that side.
#include <sila/server/FeatureRegistry.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/cloud/CloudTransport.h>
#include <sila/server/transport/cloud/StreamWriteSerializer.h>

#include "CloudRouterTestHarness.h"  // reuses cloud_test::ResponseQueue
#include "SiLACloudConnector.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace {

namespace cloud = sila2::org::silastandard;

// Polls `pred` until it returns true or `timeout` elapses. Used instead of a
// fixed sleep so tests don't race the background receive/reconnect threads.
bool waitUntil(const std::function<bool()>& pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

// One accepted connection from CloudTransport into the fake cloud-client
// endpoint below. `send` pushes a SiLAClientMessage as if the cloud client
// issued a request; `responses` accumulates SiLAServerMessages written by
// the router; `close` breaks the stream from this side.
//
// The RPC handler thread (FakeCloudClientEndpoint::ConnectSiLAServer) is the
// one that reads and pushes into `responses` — it must own `stream`'s
// read loop itself and only return once Read() fails, because gRPC
// invalidates `stream` (and `ctx`) as soon as the handler function returns.
// `close()` therefore doesn't return from the handler directly; it cancels
// the RPC's ServerContext, which is what makes the handler's own Read()
// unblock and fail — mirroring how CloudTransport::disconnect() unblocks its
// own stream_->Read() via context_->TryCancel(). `ended` (set right before
// the handler returns) guards against calling TryCancel() on a ctx that's
// already been torn down, e.g. when TearDown()'s releaseAll() runs after a
// test already closed the session itself.
struct Session {
    using StreamT = grpc::ServerReaderWriter<cloud::SiLAClientMessage, cloud::SiLAServerMessage>;

    Session(StreamT* s, grpc::ServerContext* c) : stream(s), ctx(c) {}

    void send(const cloud::SiLAClientMessage& msg) {
        std::lock_guard<std::mutex> lock(writeMu);
        stream->Write(msg);
    }

    void close() {
        std::lock_guard<std::mutex> lock(stateMu);
        if (ended) return;
        ctx->TryCancel();
    }

    // Marks the RPC as finished; must be called under stateMu right before
    // the handler function returns, so a concurrent close() either observes
    // `ended` and skips TryCancel(), or runs TryCancel() strictly before ctx
    // is invalidated. Either ordering is safe.
    void markEnded() {
        std::lock_guard<std::mutex> lock(stateMu);
        ended = true;
    }

    StreamT* stream;
    grpc::ServerContext* ctx;
    cloud_test::ResponseQueue responses;
    std::mutex writeMu;
    std::mutex stateMu;
    bool ended = false;
};

// Fake CloudClientEndpoint: accepts every incoming connection from
// CloudTransport and hands each one to the test as a Session, so a test can
// drive multiple connect/reconnect cycles against the same server.
class FakeCloudClientEndpoint final : public cloud::CloudClientEndpoint::Service {
public:
    grpc::Status ConnectSiLAServer(grpc::ServerContext* ctx, Session::StreamT* stream) override {
        auto session = std::make_shared<Session>(stream, ctx);
        {
            std::lock_guard<std::mutex> lock(mu_);
            all_.push_back(session);
            pending_.push(session);
        }
        cv_.notify_all();

        // This handler thread owns the read loop for the lifetime of the
        // RPC; it ends (and the function returns) once Read() fails, either
        // because session->close() cancelled ctx or the peer disconnected.
        cloud::SiLAServerMessage msg;
        while (stream->Read(&msg)) {
            session->responses.push(msg);
        }
        session->markEnded();
        return grpc::Status::OK;
    }

    // Blocks until the next not-yet-claimed connection arrives.
    std::shared_ptr<Session> waitForSession(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
        std::unique_lock<std::mutex> lock(mu_);
        if (!cv_.wait_for(lock, timeout, [this] { return !pending_.empty(); })) {
            throw std::runtime_error("waitForSession timed out");
        }
        auto s = pending_.front();
        pending_.pop();
        return s;
    }

    // Releases every session so ConnectSiLAServer() handlers can return,
    // letting grpc::Server::Shutdown() complete without hanging.
    void releaseAll() {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& s : all_) s->close();
    }

private:
    std::vector<std::shared_ptr<Session>> all_;
    std::queue<std::shared_ptr<Session>> pending_;
    std::mutex mu_;
    std::condition_variable cv_;
};

class CloudTransportFixture : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&endpoint_);
        server_ = builder.BuildAndStart();
        port_ = static_cast<uint16_t>(port);
    }

    void TearDown() override {
        endpoint_.releaseAll();
        if (server_) server_->Shutdown();
    }

    FakeCloudClientEndpoint endpoint_;
    std::unique_ptr<grpc::Server> server_;
    uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
// Positive (True) paths
// ---------------------------------------------------------------------------

// server-cloud-receive-loop happy path: one message in, one routed response out.
TEST_F(CloudTransportFixture, ReceiveLoopRoutesMessageAndWritesResponse) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler("org.test/Feature/Command/v1",
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(params);
            w.write(resp);
        });

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session = endpoint_.waitForSession();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-1");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid("org.test/Feature/Command/v1");
    exec->mutable_commandparameter()->set_parameters("hello");
    session->send(msg);

    auto resp = session->responses.pop();
    EXPECT_EQ(resp.requestuuid(), "req-1");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    EXPECT_EQ(resp.unobservablecommandresponse().response(), "hello");

    session->close();
    transport.disconnect();
}

// server-cloud-receive-loop happy path, second condition: the loop keeps
// dispatching correctly across multiple message kinds on one connection.
TEST_F(CloudTransportFixture, ReceiveLoopHandlesSequentialCommandAndPropertyMessages) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler("org.test/Feature/Command/v1",
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(params);
            w.write(resp);
        });
    router.registerPropertyHandler("org.test/Feature/Property/v1",
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablepropertyvalue()->set_value("prop-value");
            w.write(resp);
        });

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session = endpoint_.waitForSession();

    cloud::SiLAClientMessage cmdMsg;
    cmdMsg.set_requestuuid("req-cmd");
    auto* exec = cmdMsg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid("org.test/Feature/Command/v1");
    exec->mutable_commandparameter()->set_parameters("payload");
    session->send(cmdMsg);

    cloud::SiLAClientMessage propMsg;
    propMsg.set_requestuuid("req-prop");
    propMsg.mutable_unobservablepropertyread()->set_fullyqualifiedpropertyid(
        "org.test/Feature/Property/v1");
    session->send(propMsg);

    auto first = session->responses.pop();
    auto second = session->responses.pop();
    EXPECT_EQ(first.requestuuid(), "req-cmd");
    EXPECT_EQ(second.requestuuid(), "req-prop");
    ASSERT_TRUE(second.has_unobservablepropertyvalue());
    EXPECT_EQ(second.unobservablepropertyvalue().value(), "prop-value");

    session->close();
    transport.disconnect();
}

// server-cloud-reconnect happy path: after the first stream breaks,
// reconnect() re-establishes a new stream to the same target and routing
// resumes on it.
TEST_F(CloudTransportFixture, ReconnectAfterStreamBreakResumesRouting) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler("org.test/Feature/Command/v1",
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(params);
            w.write(resp);
        });

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session1 = endpoint_.waitForSession();
    EXPECT_TRUE(transport.isConnected());

    session1->close();  // break the stream

    // reconnect()'s backoff for the first attempt is backoffDelay(0, ...) == 1s.
    auto session2 = endpoint_.waitForSession(std::chrono::milliseconds{5000});

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-after-reconnect");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid("org.test/Feature/Command/v1");
    exec->mutable_commandparameter()->set_parameters("still-here");
    session2->send(msg);

    auto resp = session2->responses.pop();
    EXPECT_EQ(resp.requestuuid(), "req-after-reconnect");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    EXPECT_EQ(resp.unobservablecommandresponse().response(), "still-here");
    EXPECT_TRUE(waitUntil([&] { return transport.isConnected(); }));

    session2->close();
    transport.disconnect();
}

// server-cloud-disconnect happy path: a full connect/disconnect cycle tears
// resources down cleanly enough that a fresh connect() succeeds afterward.
TEST_F(CloudTransportFixture, DisconnectAfterConnectAllowsReconnecting) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session1 = endpoint_.waitForSession();
    EXPECT_TRUE(transport.isConnected());

    transport.disconnect();
    EXPECT_FALSE(transport.isConnected());

    transport.connect();
    auto session2 = endpoint_.waitForSession();
    EXPECT_TRUE(transport.isConnected());

    session1->close();
    session2->close();
    transport.disconnect();
}

// server-cloud-disconnect happy path, second condition: disconnect() after
// successfully exchanging messages (calls_ has already-completed contexts,
// not just fresh ones) still tears down cleanly.
TEST_F(CloudTransportFixture, DisconnectAfterExchangingMessagesTearsDownCleanly) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler("org.test/Feature/Command/v1",
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(params);
            w.write(resp);
        });

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session = endpoint_.waitForSession();

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-before-disconnect");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid("org.test/Feature/Command/v1");
    exec->mutable_commandparameter()->set_parameters("done");
    session->send(msg);
    session->responses.pop();

    session->close();
    transport.disconnect();

    EXPECT_FALSE(transport.isConnected());
}

// ---------------------------------------------------------------------------
// Negative (False) paths
// ---------------------------------------------------------------------------

// server-cloud-receive-loop error path (CAUGHT): a handler exception is
// swallowed by receiveLoop()'s catch(...) safety net and the loop keeps
// processing subsequent messages.
TEST_F(CloudTransportFixture, ReceiveLoopSwallowsHandlerExceptionAndContinues) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    router.registerCommandHandler("org.test/Feature/Throws/v1",
        [](const std::string&, sila2::CallContext&,
           sila2::StreamWriteSerializer&, const std::string&) {
            throw std::runtime_error("boom");
        });
    router.registerCommandHandler("org.test/Feature/Echo/v1",
        [](const std::string& params, sila2::CallContext&,
           sila2::StreamWriteSerializer& w, const std::string& requestUUID) {
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(requestUUID);
            resp.mutable_unobservablecommandresponse()->set_response(params);
            w.write(resp);
        });

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session = endpoint_.waitForSession();

    cloud::SiLAClientMessage throwMsg;
    throwMsg.set_requestuuid("req-throw");
    throwMsg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(
        "org.test/Feature/Throws/v1");
    session->send(throwMsg);

    cloud::SiLAClientMessage echoMsg;
    echoMsg.set_requestuuid("req-echo");
    auto* exec = echoMsg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid("org.test/Feature/Echo/v1");
    exec->mutable_commandparameter()->set_parameters("still-alive");
    session->send(echoMsg);

    // The throwing handler wrote nothing to the stream; the first response
    // observed must be the echo, proving the loop survived the exception.
    auto resp = session->responses.pop();
    EXPECT_EQ(resp.requestuuid(), "req-echo");
    ASSERT_TRUE(resp.has_unobservablecommandresponse());
    EXPECT_EQ(resp.unobservablecommandresponse().response(), "still-alive");

    session->close();
    transport.disconnect();
}

// server-cloud-receive-loop error path (CAUGHT): stream_->Read() returning
// false marks the transport disconnected and hands off to reconnect().
TEST_F(CloudTransportFixture, StreamBreakSetsDisconnectedBeforeReconnectSucceeds) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session1 = endpoint_.waitForSession();
    ASSERT_TRUE(transport.isConnected());

    session1->close();

    EXPECT_TRUE(waitUntil([&] { return !transport.isConnected(); }));

    auto session2 = endpoint_.waitForSession(std::chrono::milliseconds{5000});
    session2->close();
    transport.disconnect();
}

// server-cloud-reconnect error path (CAUGHT): disconnect() called while
// reconnect() is inside its backoff wait returns promptly instead of
// blocking for the full backoff duration.
TEST_F(CloudTransportFixture, DisconnectDuringReconnectBackoffExitsCleanly) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};

    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session1 = endpoint_.waitForSession();
    session1->close();  // stream break -> receiveLoop enters reconnect() backoff (~1s)

    // Give receiveLoop a moment to observe the broken Read and enter the
    // backoff wait before we race it with disconnect().
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    auto start = std::chrono::steady_clock::now();
    transport.disconnect();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(transport.isConnected());
    // Well under the >=1s backoff delay: proves the wait was interrupted by
    // stop_, not that it ran to completion before disconnect() returned.
    EXPECT_LT(elapsed, std::chrono::milliseconds{900});
}

// server-cloud-disconnect error path (CAUGHT no-op): disconnect() without a
// prior connect() is a safe early return.
TEST_F(CloudTransportFixture, DisconnectWithoutPriorConnectIsNoop) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};

    EXPECT_NO_THROW(transport.disconnect());
    EXPECT_FALSE(transport.isConnected());
}

// server-cloud-disconnect error path (CAUGHT no-op): a second disconnect()
// call after a completed disconnect is idempotent.
TEST_F(CloudTransportFixture, DisconnectTwiceIsIdempotent) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    sila2::CloudTransport transport{"127.0.0.1", port_, grpc::InsecureChannelCredentials(), router};
    transport.connect();
    auto session = endpoint_.waitForSession();
    session->close();

    transport.disconnect();
    EXPECT_NO_THROW(transport.disconnect());
    EXPECT_FALSE(transport.isConnected());
}

}  // namespace
