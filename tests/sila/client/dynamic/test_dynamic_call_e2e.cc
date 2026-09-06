// Integration tests for DynamicCall::callServerStream (architecture.md
// §4.2): a GenericStub-based server-streaming call that writes one request,
// reads server messages through a callback, and returns the final status
// once the stream ends or the callback requests an early stop.
#include <sila/client/dynamic/DynamicCall.h>

#include <sila/client/MetadataInjector.h>

#include "GenericDynamicTestServer.h"

#include <gtest/gtest.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/status.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {
using sila2::MetadataInjector;
using sila2::dynamic::callServerStream;
using sila2::test::fromByteBuffer;
using sila2::test::GenericDynamicTestServer;
using sila2::test::ScriptedResponse;

constexpr char kMethod[] = "/test.DynamicStream/Subscribe";

grpc::ByteBuffer bufferFromString(const std::string& value) {
    grpc::Slice slice(value);
    return grpc::ByteBuffer(&slice, 1);
}

// --- True (positive) paths --------------------------------------------------

TEST(DynamicCallServerStream, MultipleMessagesDeliveredThenOkStatus) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {bufferFromString("first"), bufferFromString("second"), bufferFromString("third")},
            grpc::Status::OK};
    });

    std::vector<std::string> received;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer& msg) {
            received.push_back(fromByteBuffer(msg));
            return true;
        });

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(received, (std::vector<std::string>{"first", "second", "third"}));
}

TEST(DynamicCallServerStream, EmptyStreamEndsWithOkStatusAndNoCallbackInvocations) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{}, grpc::Status::OK};
    });

    int callbackCount = 0;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer&) {
            ++callbackCount;
            return true;
        });

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(callbackCount, 0);
}

TEST(DynamicCallServerStream, MetadataInjectorIsAppliedWithoutBreakingTheStream) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{bufferFromString("only")}, grpc::Status::OK};
    });
    MetadataInjector injector;
    injector.setRaw("x-test-header-bin", "value");

    std::vector<std::string> received;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer& msg) {
            received.push_back(fromByteBuffer(msg));
            return true;
        },
        &injector);

    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(received, (std::vector<std::string>{"only"}));
}

// --- False (negative/rejection) paths ---------------------------------------

TEST(DynamicCallServerStream, ServerErrorMidStreamPropagatesNonOkStatus) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {bufferFromString("partial")},
            grpc::Status(grpc::StatusCode::INTERNAL, "simulated mid-stream failure")};
    });

    int callbackCount = 0;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer&) {
            ++callbackCount;
            return true;
        });

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_EQ(callbackCount, 1);
}

TEST(DynamicCallServerStream, ServerRejectsBeforeAnyMessageReturnsNonOkStatus) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{}, grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "rejected")};
    });

    int callbackCount = 0;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer&) {
            ++callbackCount;
            return true;
        });

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(callbackCount, 0);
}

TEST(DynamicCallServerStream, CallbackEarlyStopWithNoRemainingMessagesReturnsPromptly) {
    // Early stop that happens to land on the server's last queued message.
    // TryCancel races with the server's trailing metadata: if the server
    // already finished, Finish returns OK; if TryCancel wins, CANCELLED.
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{bufferFromString("only")}, grpc::Status::OK};
    });

    int callbackCount = 0;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer&) {
            ++callbackCount;
            return false;  // caller requests early stop after the only message
        });

    EXPECT_TRUE(status.ok() || status.error_code() == grpc::StatusCode::CANCELLED);
    EXPECT_EQ(callbackCount, 1);
}

TEST(DynamicCallServerStream, CallbackEarlyStopWithUnreadMessageReturnsCancelled) {
    // Early stop while the server still has an unread message queued.
    // TryCancel unblocks Finish, returning CANCELLED.
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {bufferFromString("first"), bufferFromString("second")}, grpc::Status::OK};
    });

    int callbackCount = 0;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer&) {
            ++callbackCount;
            return false;  // early stop while "second" is still unread
        });

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
}

TEST(DynamicCallServerStream, UnregisteredMethodReturnsUnimplemented) {
    GenericDynamicTestServer server;  // no handler registered for kMethod

    int callbackCount = 0;
    auto status = callServerStream(
        server.channel(), kMethod, bufferFromString("request"),
        [&](const grpc::ByteBuffer&) {
            ++callbackCount;
            return true;
        });

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
    EXPECT_EQ(callbackCount, 0);
}

}  // namespace
