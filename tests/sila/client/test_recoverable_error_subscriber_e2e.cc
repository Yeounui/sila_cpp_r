// Integration tests for RecoverableErrorSubscriber: a background thread
// that drains a server-streaming Subscribe_RecoverableErrors subscription
// (SiLA2 Error Recovery Service) and delivers vector<RecoverableErrorInfo>
// via callback until the stream ends or is cancelled.
//
// RecoverableErrorSubscriber takes a real grpc::ClientReader<
// Subscribe_RecoverableErrors_Responses>, which normally comes from
// generated stub code. grpc::internal::ClientReaderFactory<R>::Create(...)
// is exactly what generated stubs call under the hood, so it lets this test
// build one directly against the shared GenericDynamicTestServer fake
// without needing a generated .proto service — the wire protocol is
// identical either way.
#include <sila/client/RecoverableErrorSubscriber.h>

#include "dynamic/GenericDynamicTestServer.h"

#include "ErrorRecoveryService.pb.h"

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
#include <tuple>
#include <vector>

namespace {
using sila2::RecoverableErrorInfo;
using sila2::RecoverableErrorSubscriber;
using sila2::test::GenericDynamicTestServer;
using sila2::test::ScriptedResponse;
using sila2::test::toByteBuffer;
namespace erp = sila2::org::silastandard::core::errorrecoveryservice::v2;

constexpr char kMethod[] = "/test.ErrorRecoveryService/Subscribe_RecoverableErrors";

// Each tuple: (errorIdentifier, commandExecutionUuid, errorMessage).
erp::Subscribe_RecoverableErrors_Responses makeResponse(
    std::vector<std::tuple<std::string, std::string, std::string>> errors) {
    erp::Subscribe_RecoverableErrors_Responses resp;
    for (const auto& [id, uuid, msg] : errors) {
        auto* wrapper = resp.add_recoverableerrors();
        auto* re = wrapper->mutable_recoverableerror();
        re->mutable_erroridentifier()->set_value(id);
        re->mutable_commandexecutionuuid()->set_value(uuid);
        re->mutable_errormessage()->set_value(msg);
    }
    return resp;
}

// Builds a ClientReader<Subscribe_RecoverableErrors_Responses> against
// `channel` for kMethod, the same construction generated stub code
// performs internally.
std::unique_ptr<grpc::ClientReader<erp::Subscribe_RecoverableErrors_Responses>> makeReader(
    const std::shared_ptr<grpc::Channel>& channel, grpc::ClientContext* context) {
    erp::Subscribe_RecoverableErrors_Parameters request;
    grpc::internal::RpcMethod method(kMethod, grpc::internal::RpcMethod::SERVER_STREAMING);
    return std::unique_ptr<grpc::ClientReader<erp::Subscribe_RecoverableErrors_Responses>>{
        grpc::internal::ClientReaderFactory<erp::Subscribe_RecoverableErrors_Responses>::Create(
            channel.get(), method, context, request)};
}

// Thread-safe sink for callback invocations, since readLoop runs on
// RecoverableErrorSubscriber's own background thread.
class ErrorSink {
public:
    void record(const std::vector<RecoverableErrorInfo>& errors) {
        std::lock_guard<std::mutex> lock(mu_);
        calls_.push_back(errors);
    }
    std::vector<std::vector<RecoverableErrorInfo>> calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::vector<RecoverableErrorInfo>> calls_;
};

// --- True (positive) paths --------------------------------------------------

TEST(RecoverableErrorSubscriber, SingleErrorWithContinuationOptionsIsConverted) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        erp::Subscribe_RecoverableErrors_Responses resp =
            makeResponse({{"err1", "uuid-1", "something failed"}});
        auto* re = resp.mutable_recoverableerrors(0)->mutable_recoverableerror();
        auto* opt1 = re->add_continuationoptions();
        opt1->mutable_continuationoption()->mutable_identifier()->set_value("retry");
        opt1->mutable_continuationoption()->mutable_description()->set_value("Retry the operation");
        auto* opt2 = re->add_continuationoptions();
        opt2->mutable_continuationoption()->mutable_identifier()->set_value("abort");
        opt2->mutable_continuationoption()->mutable_description()->set_value("Abort the command");
        re->mutable_defaultoption()->set_value("retry");
        re->mutable_automaticselectiontimeout()->mutable_timeout()->set_value(30);
        return ScriptedResponse{{toByteBuffer(resp)}, grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0].size(), 1u);
    const auto& info = calls[0][0];
    EXPECT_EQ(info.errorIdentifier, "err1");
    EXPECT_EQ(info.commandExecutionUuid, "uuid-1");
    EXPECT_EQ(info.errorMessage, "something failed");
    ASSERT_EQ(info.continuationOptions.size(), 2u);
    EXPECT_EQ(info.continuationOptions[0].identifier, "retry");
    EXPECT_EQ(info.continuationOptions[0].description, "Retry the operation");
    EXPECT_EQ(info.continuationOptions[1].identifier, "abort");
    EXPECT_EQ(info.continuationOptions[1].description, "Abort the command");
    EXPECT_EQ(info.defaultOption, "retry");
    EXPECT_EQ(info.automaticSelectionTimeoutSeconds, 30);
    EXPECT_FALSE(subscriber.isActive());
}

TEST(RecoverableErrorSubscriber, MultipleErrorsInOneResponseAreAllDelivered) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeResponse({
                {"err1", "uuid-1", "message 1"},
                {"err2", "uuid-2", "message 2"},
                {"err3", "uuid-3", "message 3"},
            }))},
            grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0].size(), 3u);
    EXPECT_EQ(calls[0][0].errorIdentifier, "err1");
    EXPECT_EQ(calls[0][1].errorIdentifier, "err2");
    EXPECT_EQ(calls[0][2].errorIdentifier, "err3");
}

TEST(RecoverableErrorSubscriber, MultipleResponsesInvokeCallbackTwice) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeResponse({{"err1", "uuid-1", "first"}})),
             toByteBuffer(makeResponse({{"err2", "uuid-2", "second"}}))},
            grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 2u);
    ASSERT_EQ(calls[0].size(), 1u);
    EXPECT_EQ(calls[0][0].errorIdentifier, "err1");
    ASSERT_EQ(calls[1].size(), 1u);
    EXPECT_EQ(calls[1][0].errorIdentifier, "err2");
}

// RecoverableErrorSubscriber.cc:63-64 — CommandIdentifier is decoded
// alongside the pre-existing fields, not swapped in for one of them.
TEST(RecoverableErrorSubscriber, EmptyErrorListDecodesToEmptyVector) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        // Zero recoverableerrors entries: the wire shape the gate publishes
        // after every resolution (publishPendingErrors on the erase path) --
        // distinct from a one-error response whose optional fields are unset.
        return ScriptedResponse{{toByteBuffer(makeResponse({}))}, grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    // Exactly one DECODED callback. size()==1 is the load-bearing half: it
    // separates a real "nothing pending" update from the synthesized empty
    // callback the failure path emits, which no other test pins apart.
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(calls[0].empty());
}

TEST(RecoverableErrorSubscriber, CommandIdentifierIsSurfaced) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        erp::Subscribe_RecoverableErrors_Responses resp =
            makeResponse({{"err1", "uuid-1", "something failed"}});
        auto* re = resp.mutable_recoverableerrors(0)->mutable_recoverableerror();
        re->mutable_commandidentifier()->set_value("org.example/test/Shaker/v1/Command/ShakeForTime");
        return ScriptedResponse{{toByteBuffer(resp)}, grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0].size(), 1u);
    const auto& info = calls[0][0];
    EXPECT_EQ(info.commandIdentifier, "org.example/test/Shaker/v1/Command/ShakeForTime");
    // The pre-existing fields still decode -- guards against a mis-ordered
    // aggregate init in the decode loop.
    EXPECT_EQ(info.errorIdentifier, "err1");
}

// RecoverableErrorSubscriber.cc:64 — ErrorTime round-trips through
// types::fromProto (BasicTypes.h:174), including the millisecond field a
// hand-rolled converter would most easily drop.
TEST(RecoverableErrorSubscriber, ErrorTimeIsSurfaced) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        erp::Subscribe_RecoverableErrors_Responses resp =
            makeResponse({{"err1", "uuid-1", "something failed"}});
        auto* re = resp.mutable_recoverableerrors(0)->mutable_recoverableerror();
        auto* time = re->mutable_errortime();
        time->set_year(2026);
        time->set_month(8);
        time->set_day(31);
        time->set_hour(12);
        time->set_minute(34);
        time->set_second(56);
        time->set_millisecond(789);
        time->mutable_timezone()->set_hours(0);
        time->mutable_timezone()->set_minutes(0);
        return ScriptedResponse{{toByteBuffer(resp)}, grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0].size(), 1u);
    const auto& info = calls[0][0];
    EXPECT_EQ(info.errorTime.year, 2026u);
    EXPECT_EQ(info.errorTime.month, 8u);
    EXPECT_EQ(info.errorTime.day, 31u);
    EXPECT_EQ(info.errorTime.hour, 12u);
    EXPECT_EQ(info.errorTime.minute, 34u);
    EXPECT_EQ(info.errorTime.second, 56u);
    EXPECT_EQ(info.errorTime.millisecond, 789u);
}

// RecoverableErrorSubscriber.cc:65-66 — RequiredInputData decodes per
// option, not smeared across the whole list, and leaves the neighbouring
// Description field unshifted.
TEST(RecoverableErrorSubscriber, RequiredInputDataIsSurfacedPerOption) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        erp::Subscribe_RecoverableErrors_Responses resp =
            makeResponse({{"err1", "uuid-1", "something failed"}});
        auto* re = resp.mutable_recoverableerrors(0)->mutable_recoverableerror();
        auto* opt1 = re->add_continuationoptions()->mutable_continuationoption();
        opt1->mutable_identifier()->set_value("retry");
        opt1->mutable_description()->set_value("Retry the operation");
        opt1->mutable_requiredinputdata()->set_value("<DataType><Basic>String</Basic></DataType>");
        auto* opt2 = re->add_continuationoptions()->mutable_continuationoption();
        opt2->mutable_identifier()->set_value("abort");
        opt2->mutable_description()->set_value("Abort the command");
        // requiredinputdata deliberately left unset on this option.
        return ScriptedResponse{{toByteBuffer(resp)}, grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0].size(), 1u);
    const auto& info = calls[0][0];
    ASSERT_EQ(info.continuationOptions.size(), 2u);
    EXPECT_EQ(info.continuationOptions[0].requiredInputData,
              "<DataType><Basic>String</Basic></DataType>");
    EXPECT_TRUE(info.continuationOptions[1].requiredInputData.empty());
    EXPECT_EQ(info.continuationOptions[0].description, "Retry the operation");
}

// --- False (negative/rejection) paths ---------------------------------------

// RecoverableErrorSubscriber.cc:62-68 — a response built by the pre-existing
// makeResponse() helper alone (no commandidentifier/errortime/
// requiredinputdata touched) decodes the three new S40 fields to their
// value-initialised defaults, not garbage.
TEST(RecoverableErrorSubscriber, AbsentWireFieldsDecodeToEmptyNotGarbage) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeResponse({{"err1", "uuid-1", "something failed"}}))},
            grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(calls[0].size(), 1u);
    const auto& info = calls[0][0];
    EXPECT_TRUE(info.commandIdentifier.empty());
    // Value-initialised via `{}` on the struct member, not left uninitialised.
    EXPECT_EQ(info.errorTime.year, 0u);
    EXPECT_TRUE(info.continuationOptions.empty());
}

// RecoverableErrorSubscriber.cc:75 — non-OK finish with zero prior reads
// still synthesizes one empty-vector callback.
TEST(RecoverableErrorSubscriber, NonOkFinishWithNoMessagesSynthesizesEmptyCallback) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{{}, grpc::Status(grpc::StatusCode::INTERNAL, "server crashed")};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(calls[0].empty());
    EXPECT_FALSE(subscriber.isActive());
}

// RecoverableErrorSubscriber.cc:75 — non-OK finish after some prior reads
// still synthesizes a trailing empty-vector callback.
TEST(RecoverableErrorSubscriber, NonOkFinishAfterMessagesSynthesizesEmptyCallback) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        return ScriptedResponse{
            {toByteBuffer(makeResponse({{"err1", "uuid-1", "message"}}))},
            grpc::Status(grpc::StatusCode::UNAVAILABLE, "connection lost")};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};
    subscriber.wait();

    const auto calls = sink.calls();
    ASSERT_EQ(calls.size(), 2u);
    ASSERT_EQ(calls[0].size(), 1u);
    EXPECT_EQ(calls[0][0].errorIdentifier, "err1");
    EXPECT_TRUE(calls[1].empty());
}

// cancel() suppresses the synthesized empty-vector callback (active_ is
// already false by the time Finish() observes the CANCELLED status).
TEST(RecoverableErrorSubscriber, CancelSuppressesSynthesizedEmptyCallback) {
    GenericDynamicTestServer server;
    server.on(kMethod, [](const grpc::ByteBuffer&) {
        // Held back long enough for the test to cancel before any message
        // is sent, so the read loop's first Read() is the one interrupted.
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        return ScriptedResponse{
            {toByteBuffer(makeResponse({{"err1", "uuid-1", "message"}}))}, grpc::Status::OK};
    });

    auto channel = server.channel();
    auto context = std::make_unique<grpc::ClientContext>();
    auto reader = makeReader(channel, context.get());
    ErrorSink sink;
    RecoverableErrorSubscriber subscriber{
        std::move(context), std::move(reader),
        [&](const std::vector<RecoverableErrorInfo>& e) { sink.record(e); }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    subscriber.cancel();
    subscriber.wait();

    EXPECT_TRUE(sink.calls().empty());
    EXPECT_FALSE(subscriber.isActive());
}

}  // namespace
