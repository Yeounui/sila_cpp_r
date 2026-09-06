// Integration tests for ExecutionInfoSubscriber: a background thread that
// drains a server-streaming ExecutionInfo subscription (SiLA2 §6.3.4) and
// delivers updates via callback until the stream ends or is cancelled.
//
// ExecutionInfoSubscriber takes a real grpc::ClientReader<ExecutionInfo>,
// which normally comes from generated stub code. grpc::internal::
// ClientReaderFactory<R>::Create(...) is exactly what generated stubs call
// under the hood, so it lets this test build one directly against the
// shared GenericDynamicTestServer fake without needing a generated .proto
// service — the wire protocol is identical either way.
#include <sila/client/ExecutionInfoSubscriber.h>

#include <sila/client/CommandExecutionStatus.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

#include "dynamic/GenericDynamicTestServer.h"

#include "SiLAFramework.pb.h"

#include <grpcpp/client_context.h>
#include <grpcpp/impl/codegen/proto_utils.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/support/sync_stream.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using sila2::CommandExecutionStatus;
using sila2::ExecutionInfoSubscriber;
using sila2::ExecutionUpdate;
using sila2::test::GenericDynamicTestServer;
using sila2::test::ScriptedResponse;
using sila2::test::toByteBuffer;
namespace fw = sila2::org::silastandard;

constexpr char kMethod[] = "/test.ExecutionInfoStream/Subscribe";

fw::ExecutionInfo makeInfo(fw::ExecutionInfo_CommandStatus status, double progress = 0.0) {
    fw::ExecutionInfo info;
    info.set_commandstatus(status);
    if (progress > 0.0) {
        info.mutable_progressinfo()->set_value(progress);
    }
    return info;
}

// Builds a ClientReader<ExecutionInfo> against `channel` for kMethod, the
// same construction generated stub code performs internally.
std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>> makeReader(
    const std::shared_ptr<grpc::Channel>& channel, grpc::ClientContext* context) {
    fw::CommandExecutionUUID request;
    request.set_value("subscriber-test-uuid");
    grpc::internal::RpcMethod method(kMethod, grpc::internal::RpcMethod::SERVER_STREAMING);
    return std::unique_ptr<grpc::ClientReader<fw::ExecutionInfo>>{
        grpc::internal::ClientReaderFactory<fw::ExecutionInfo>::Create(
            channel.get(), method, context, request)};
}

// Thread-safe sink for callback invocations, since readLoop runs on
// ExecutionInfoSubscriber's own background thread.
class UpdateSink {
public:
    void record(const ExecutionUpdate& update) {
        std::lock_guard<std::mutex> lock(mu_);
        updates_.push_back(update);
    }
    std::vector<ExecutionUpdate> updates() const {
        std::lock_guard<std::mutex> lock(mu_);
        return updates_;
    }

private:
    mutable std::mutex mu_;
    std::vector<ExecutionUpdate> updates_;
};

// --- True (positive) paths --------------------------------------------------

TEST(ExecutionInfoSubscriber, StreamOfUpdatesInvokesCallbackThenEndsOnOkFinish) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_running, 0.5)),
             toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedSuccessfully))},
            grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    UpdateSink sink;
    ExecutionInfoSubscriber subscriber{
        std::move(context), std::move(reader), [&](const ExecutionUpdate& u) { sink.record(u); }};
    subscriber.wait();

    const auto updates = sink.updates();
    ASSERT_EQ(updates.size(), 2u);
    EXPECT_EQ(updates[0].status, CommandExecutionStatus::kRunning);
    EXPECT_EQ(updates[1].status, CommandExecutionStatus::kFinishedSuccessfully);
    EXPECT_EQ(subscriber.lastStatus(), CommandExecutionStatus::kFinishedSuccessfully);
    EXPECT_FALSE(subscriber.isActive());
}

TEST(ExecutionInfoSubscriber, ProgressValueIsPropagatedWhenPresent) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_running, 0.75)),
             toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedSuccessfully))},
            grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    UpdateSink sink;
    ExecutionInfoSubscriber subscriber{
        std::move(context), std::move(reader), [&](const ExecutionUpdate& u) { sink.record(u); }};
    subscriber.wait();

    const auto updates = sink.updates();
    ASSERT_FALSE(updates.empty());
    EXPECT_FLOAT_EQ(updates[0].progress, 0.75F);
}

TEST(ExecutionInfoSubscriber, ProgressAbsentDefaultsToZero) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_waiting)),
             toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_finishedSuccessfully))},
            grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    UpdateSink sink;
    ExecutionInfoSubscriber subscriber{
        std::move(context), std::move(reader), [&](const ExecutionUpdate& u) { sink.record(u); }};
    subscriber.wait();

    const auto updates = sink.updates();
    ASSERT_FALSE(updates.empty());
    EXPECT_EQ(updates[0].status, CommandExecutionStatus::kWaiting);
    EXPECT_FLOAT_EQ(updates[0].progress, 0.0F);
}

// --- False (negative/rejection) paths ---------------------------------------

// ExecutionInfoSubscriber.cc:62 — non-OK finish while still active
// synthesizes a kFinishedWithError update carrying the status message.
TEST(ExecutionInfoSubscriber, NonOkFinishAfterUpdatesSynthesizesErrorCallback) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_running, 0.2))},
            grpc::Status(grpc::StatusCode::INTERNAL, "server crashed mid-execution")};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    UpdateSink sink;
    ExecutionInfoSubscriber subscriber{
        std::move(context), std::move(reader), [&](const ExecutionUpdate& u) { sink.record(u); }};
    subscriber.wait();

    const auto updates = sink.updates();
    ASSERT_EQ(updates.size(), 2u);
    EXPECT_EQ(updates[0].status, CommandExecutionStatus::kRunning);
    EXPECT_EQ(updates[1].status, CommandExecutionStatus::kFinishedWithError);
    EXPECT_EQ(updates[1].statusMessage, "server crashed mid-execution");
    EXPECT_EQ(subscriber.lastStatus(), CommandExecutionStatus::kFinishedWithError);
}

// ExecutionInfoSubscriber.cc:62 (zero-reads variant) — the server rejects
// the stream immediately, so the terminal callback fires with no prior
// successful reads.
TEST(ExecutionInfoSubscriber, ImmediateNonOkFinishStillSynthesizesErrorCallback) {
    GenericDynamicTestServer server;  // no handler registered -> UNIMPLEMENTED, zero messages

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    UpdateSink sink;
    ExecutionInfoSubscriber subscriber{
        std::move(context), std::move(reader), [&](const ExecutionUpdate& u) { sink.record(u); }};
    subscriber.wait();

    const auto updates = sink.updates();
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].status, CommandExecutionStatus::kFinishedWithError);
    EXPECT_EQ(subscriber.lastStatus(), CommandExecutionStatus::kFinishedWithError);
}

TEST(ExecutionInfoSubscriber, SilaErrorFinishUsesDecodedMessage) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {},
            sila2::error::DefinedExecutionError{"org.silastandard/test/Error", "token rejected"}.toStatus()};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    UpdateSink sink;
    ExecutionInfoSubscriber subscriber{
        std::move(context), std::move(reader), [&](const ExecutionUpdate& u) { sink.record(u); }};
    subscriber.wait();

    const auto updates = sink.updates();
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].statusMessage, "token rejected");
}

// cancel() suppresses the synthesized error callback (active_ is already
// false by the time Finish() observes the CANCELLED status).
TEST(ExecutionInfoSubscriber, CancelSuppressesSynthesizedErrorCallback) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        // Held back long enough for the test to cancel before any message
        // is sent, so the read loop's first Read() is the one interrupted.
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        return ScriptedResponse{
            {toByteBuffer(makeInfo(fw::ExecutionInfo_CommandStatus_running))}, grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    UpdateSink sink;
    ExecutionInfoSubscriber subscriber{
        std::move(context), std::move(reader), [&](const ExecutionUpdate& u) { sink.record(u); }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    subscriber.cancel();
    subscriber.wait();

    EXPECT_TRUE(sink.updates().empty());
    EXPECT_FALSE(subscriber.isActive());
}

}  // namespace
