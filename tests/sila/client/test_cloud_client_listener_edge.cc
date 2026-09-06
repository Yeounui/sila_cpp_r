// Integration tests for CloudClientListener edge cases not covered by
// test_cloud_client_listener.cc: port bind failure, start/stop idempotency,
// isRunning transitions, and stream write failure during call().
#include <sila/client/CloudClientListener.h>

#include "SiLACloudConnector.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <future>
#include <string>
#include <thread>

namespace {

namespace cloud = sila2::org::silastandard;

uint16_t findFreePort() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    close(sock);
    return port;
}

std::unique_ptr<cloud::CloudClientEndpoint::Stub> makeStub(
    uint16_t port, const std::string& rootCertPem) {
    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = rootCertPem;
    auto channel = grpc::CreateChannel(
        "localhost:" + std::to_string(port),
        grpc::SslCredentials(ssl_opts));
    return cloud::CloudClientEndpoint::NewStub(channel);
}

// --- True (positive) paths ---------------------------------------------------

TEST(CloudClientListenerEdge, StartSucceedsAndIsRunningReturnsTrue) {
    uint16_t port = findFreePort();
    sila2::CloudClientListener listener{port};

    EXPECT_FALSE(listener.isRunning());
    listener.start();
    EXPECT_TRUE(listener.isRunning());

    listener.stop();
}

TEST(CloudClientListenerEdge, DoubleStartIsIdempotent) {
    uint16_t port = findFreePort();
    sila2::CloudClientListener listener{port};

    listener.start();
    EXPECT_TRUE(listener.isRunning());

    // Second start() must be a no-op, not rebind/throw.
    EXPECT_NO_THROW(listener.start());
    EXPECT_TRUE(listener.isRunning());

    listener.stop();
}

TEST(CloudClientListenerEdge, StopTransitionsIsRunningFromTrueToFalse) {
    uint16_t port = findFreePort();
    sila2::CloudClientListener listener{port};

    listener.start();
    EXPECT_TRUE(listener.isRunning());

    listener.stop();
    EXPECT_FALSE(listener.isRunning());
}

// --- False (negative/edge) paths ---------------------------------------------

TEST(CloudClientListenerEdge, StartThrowsWhenPortAlreadyBound) {
    uint16_t port = findFreePort();

    // Occupy the port with a raw socket so grpc::ServerBuilder::BuildAndStart
    // fails to bind it.
    int blocker = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    ASSERT_EQ(bind(blocker, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(blocker, 1), 0);

    sila2::CloudClientListener listener{port};
    EXPECT_THROW(listener.start(), std::runtime_error);
    EXPECT_FALSE(listener.isRunning());

    close(blocker);
}

TEST(CloudClientListenerEdge, StopWhenNotRunningIsNoOp) {
    uint16_t port = findFreePort();
    sila2::CloudClientListener listener{port};

    EXPECT_FALSE(listener.isRunning());
    EXPECT_NO_THROW(listener.stop());
    EXPECT_FALSE(listener.isRunning());
}

TEST(CloudClientListenerEdge, CallThrowsWhenStreamWriteFails) {
    uint16_t port = findFreePort();
    sila2::CloudClientListener listener{port};

    std::promise<void> connectedPromise;
    std::promise<void> releaseHandlerPromise;
    listener.setServerConnectedCallback([&](const std::string&) {
        connectedPromise.set_value();
        // Park the handler thread here, before it ever reaches stream->Read().
        // That keeps the session registered in sessions_ even after the
        // client cancels below, since erase() only runs once Read() loop exits.
        releaseHandlerPromise.get_future().wait();
    });
    listener.start();

    auto stub = makeStub(port, listener.certificatePem());
    grpc::ClientContext ctx;
    ctx.AddMetadata("sila-server-uuid", "write-fail-server");
    auto stream = stub->ConnectSiLAServer(&ctx);

    // Finish() blocks until the server handler returns, which won't happen
    // until releaseHandlerPromise is set below, so run it in the background.
    auto finishFuture = std::async(std::launch::async, [&stream] {
        return stream->Finish();
    });

    connectedPromise.get_future().wait();

    // Cancel the RPC while the handler thread is still parked in the
    // callback (never having called Read()), so the session stays looked-up-able
    // in call() while the underlying stream is already dead for writes.
    ctx.TryCancel();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    EXPECT_THROW(
        listener.call("write-fail-server", "org.test/Cmd/v1", "", true),
        std::runtime_error);

    releaseHandlerPromise.set_value();
    finishFuture.get();
    listener.stop();
}

TEST(CloudClientListenerEdge, ThrowingConnectedCallbackCleansUpSession) {
    uint16_t port = findFreePort();
    sila2::CloudClientListener listener{port};
    listener.setServerConnectedCallback([](const std::string&) {
        throw std::runtime_error{"callback failed"};
    });
    listener.start();

    auto stub = makeStub(port, listener.certificatePem());
    grpc::ClientContext ctx;
    ctx.AddMetadata("sila-server-uuid", "callback-fail-server");
    auto stream = stub->ConnectSiLAServer(&ctx);

    auto status = stream->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_THROW(listener.call("callback-fail-server", "org.test/Cmd/v1", "", true),
                 std::runtime_error);

    listener.stop();
}

}  // namespace
