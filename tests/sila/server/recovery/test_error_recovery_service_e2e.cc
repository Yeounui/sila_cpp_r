// End-to-end tests for ErrorRecoveryServiceImpl (architecture.md §3.12): the
// full gRPC-facing flow from a client request, through
// RecoverableErrorGate's blocking select/abort logic, to the v2
// DefinedExecutionError translation the FDL requires. test_recoverable_error_gate.cc
// and test_error_gate_property_integration.cc already cover the gate itself
// in isolation; this file follows requests through ErrorRecoveryServiceImpl,
// the actual gRPC entry point, including the gate-error -> gRPC-status
// translation those files don't exercise.
#include <sila/server/recovery/ErrorRecoveryServiceImpl.h>
#include <sila/server/recovery/RecoverableErrorGate.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/SilaServerBase.h>
#include <sila/server/SilaServiceImpl.h>
#include <sila/server/features/ConnectionConfigurationServiceImpl.h>

#include "ConnectionConfigurationService.grpc.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using sila2::ErrorRecoveryServiceImpl;
using sila2::ObservablePropertyManager;
using sila2::recovery::ContinuationOption;
using sila2::recovery::RecoverableErrorGate;
using sila2::recovery::RecoveryChoice;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::SilaError;
using sila2::error::ValidationError;

namespace errorrecovery_proto = sila2::errorrecovery_proto;
// S75 always registers ConnectionConfigurationService now (Part A p80 SHALL),
// so this test's server advertises three features, not two.
namespace connconfig_proto = sila2::org::silastandard::core::connectionconfigurationservice::v1;

const std::string kInvalidUuidErrorId =
    "org.silastandard/core/ErrorRecoveryService/v2/DefinedExecutionError/InvalidCommandExecutionUUID";
const std::string kUnknownOptionErrorId =
    "org.silastandard/core/ErrorRecoveryService/v2/DefinedExecutionError/UnknownContinuationOption";

// Every test below sends the same errorIdentifier/commandIdentifier
// placeholders, mirroring test_recoverable_error_gate.cc's fixture: this file
// exercises the gRPC translation layer, not raiseAndWait()'s own FDL
// structural validation (already covered there), so the identity values here
// are arbitrary but non-empty.
constexpr const char* kErrorIdentifier =
    "org.example/test/Shaker/v1/DefinedExecutionError/Stalled";
constexpr const char* kCommandIdentifier =
    "org.example/test/Shaker/v1/Command/ShakeForTime";

// Starts gate.raiseAndWait(...) on a background thread and sleeps briefly to
// let it reach the blocking wait — mirrors test_recoverable_error_gate.cc's
// pattern. Caller must resolve/abort uuid and join the returned thread.
std::thread raisePendingError(RecoverableErrorGate& gate, std::optional<RecoveryChoice>& result,
                               const std::string& uuid, std::string message,
                               std::vector<ContinuationOption> options) {
    std::thread worker{[&gate, &result, uuid, message = std::move(message), options = std::move(options)]() mutable {
        // S6 changed raiseAndWait() to a single RecoverableError argument
        // (RecoverableErrorGate.h) — designated-initializer form matches
        // test_recoverable_error_gate.cc's migrated call sites.
        result = gate.raiseAndWait({
            .commandExecutionUuid = uuid,
            .errorIdentifier = kErrorIdentifier,
            .commandIdentifier = kCommandIdentifier,
            .errorMessage = std::move(message),
            .continuationOptions = std::move(options),
        });
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    return worker;
}

// Reads a DefinedExecutionError's identifier out of a non-OK status the same
// way a real SiLA client would via fromGrpcStatus(), asserting the dynamic
// type along the way rather than trusting the caller got the right subclass.
std::string definedErrorIdentifier(const grpc::Status& status) {
    EXPECT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    EXPECT_NE(reconstructed, nullptr);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    EXPECT_NE(definedError, nullptr);
    return definedError != nullptr ? definedError->errorIdentifier() : std::string{};
}

// Real local server+stub, used only by the Subscribe_RecoverableErrors tests
// since grpc::ServerWriter (unlike the unary methods) can't be driven without
// an actual streaming RPC. Constructed fresh per test — no shared state.
struct RecoveryFixture {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<errorrecovery_proto::ErrorRecoveryService::Stub> stub;

    RecoveryFixture() {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub = errorrecovery_proto::ErrorRecoveryService::NewStub(channel);
    }

    ~RecoveryFixture() { server->Shutdown(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// ExecuteContinuationOption / AbortErrorHandling / SetErrorHandlingTimeout
// — True paths
// ---------------------------------------------------------------------------

TEST(ErrorRecoveryServiceE2E, ExecuteContinuationOptionResolvesPendingErrorReturnsOk) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    std::optional<RecoveryChoice> result;
    std::thread worker = raisePendingError(gate, result, "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "Pump failure",
        {{"retry", false, std::chrono::seconds{0}}});

    grpc::ServerContext ctx;
    errorrecovery_proto::ExecuteContinuationOption_Parameters request;
    request.mutable_commandexecutionuuid()->set_value("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
    request.mutable_continuationoption()->set_value("retry");
    errorrecovery_proto::ExecuteContinuationOption_Responses response;

    const grpc::Status status = service.ExecuteContinuationOption(&ctx, &request, &response);
    worker.join();

    EXPECT_TRUE(status.ok());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");
}

TEST(ErrorRecoveryServiceE2E, AbortErrorHandlingResolvesPendingErrorReturnsOk) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    std::optional<RecoveryChoice> result{RecoveryChoice{"placeholder", {}}};
    std::thread worker = raisePendingError(gate, result, "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "Valve stuck",
        {{"retry", false, std::chrono::seconds{0}}});

    grpc::ServerContext ctx;
    errorrecovery_proto::AbortErrorHandling_Parameters request;
    request.mutable_commandexecutionuuid()->set_value("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
    errorrecovery_proto::AbortErrorHandling_Responses response;

    const grpc::Status status = service.AbortErrorHandling(&ctx, &request, &response);
    worker.join();

    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(result.has_value());
}

TEST(ErrorRecoveryServiceE2E, SetErrorHandlingTimeoutForwardsToGateAndReturnsOk) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    grpc::ServerContext ctx;
    errorrecovery_proto::SetErrorHandlingTimeout_Parameters request;
    request.mutable_errorhandlingtimeout()->mutable_timeout()->set_value(1);
    errorrecovery_proto::SetErrorHandlingTimeout_Responses response;

    const grpc::Status status = service.SetErrorHandlingTimeout(&ctx, &request, &response);
    EXPECT_TRUE(status.ok());

    // Verify the timeout actually reached the gate, not just that the RPC
    // returned OK: an error raised with no auto-execute option now times out
    // after ~1s instead of blocking indefinitely.
    const auto start = std::chrono::steady_clock::now();
    const auto result = gate.raiseAndWait({
        // S39: commandExecutionUuid must be Length-36 lowercase-hex.
        .commandExecutionUuid = "33333333-3333-3333-3333-333333333333",
        .errorIdentifier = kErrorIdentifier,
        .commandIdentifier = kCommandIdentifier,
        .errorMessage = "Sensor fault",
        .continuationOptions = {{"retry", false, std::chrono::seconds{0}}},
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.has_value());
    EXPECT_GE(elapsed, std::chrono::seconds{1});
}

// ---------------------------------------------------------------------------
// Subscribe_RecoverableErrors — True paths (needs a real server: unlike the
// unary methods above, grpc::ServerWriter can't be driven without an actual
// streaming RPC).
// ---------------------------------------------------------------------------

TEST(ErrorRecoveryServiceE2E, SubscribeRecoverableErrorsStreamsPendingErrorThenEndsOkOnPropertyManagerShutdown) {
    RecoveryFixture fx;
    const std::string propertyId = sila2::recovery::kRecoverableErrorsPropertyId;

    grpc::ClientContext ctx;
    errorrecovery_proto::Subscribe_RecoverableErrors_Parameters request;
    auto stream = fx.stub->Subscribe_RecoverableErrors(&ctx, request);

    // Subscribe_RecoverableErrors() registers with the property manager on the
    // server's own RPC thread once the request lands there — wait for that
    // registration so the initial empty snapshot below is read before the raise.
    // (A raise racing ahead of subscribe() is no longer lost: the manager
    // retains the last value and replays it to a late subscriber.)
    for (int attempt = 0; attempt < 100 && fx.propMgr.subscriberCount(propertyId) == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(fx.propMgr.subscriberCount(propertyId), 1u);

    // The subscription's first message is the property's current value: an empty
    // list (SiLA Part A -- readable at any time; nothing is pending yet).
    errorrecovery_proto::Subscribe_RecoverableErrors_Responses response;
    ASSERT_TRUE(stream->Read(&response));
    ASSERT_EQ(response.recoverableerrors_size(), 0);

    std::optional<RecoveryChoice> result;
    // S39: commandExecutionUuid must be Length-36 lowercase-hex.
    std::thread worker = raisePendingError(fx.gate, result, "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee", "Stirrer jam",
        {{"retry", false, std::chrono::seconds{0}}});

    ASSERT_TRUE(stream->Read(&response));
    ASSERT_EQ(response.recoverableerrors_size(), 1);
    const auto& jam = response.recoverableerrors(0).recoverableerror();
    EXPECT_EQ(jam.errormessage().value(), "Stirrer jam");
    // This raise flags no default option: the FDL Structure still requires
    // both elements on the wire — DefaultOption falls back to the first
    // option (it must name a real one), and timeout 0 encodes "no automatic
    // selection" (ErrorRecoveryService-v2_0.sila.xml:249-271).
    EXPECT_EQ(jam.defaultoption().value(), "retry");
    EXPECT_EQ(jam.automaticselectiontimeout().timeout().value(), 0);

    fx.gate.abort("eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee");
    worker.join();
    // Drain the second publish (empty list) abort() produces, so it isn't
    // mistaken for a fresh error by a test reading further.
    ASSERT_TRUE(stream->Read(&response));
    EXPECT_EQ(response.recoverableerrors_size(), 0);

    // Ends the stream the way a server shutdown would: cancelAll() makes
    // sub->waitForNext() return nullopt (ErrorRecoveryServiceImpl.cc:110),
    // breaking the loop and returning Status::OK without the client ever
    // cancelling.
    fx.propMgr.cancelAll(propertyId);

    EXPECT_FALSE(stream->Read(&response));
    const grpc::Status status = stream->Finish();
    EXPECT_TRUE(status.ok());
}

// S6 batch 1 gave RecoverableError/ContinuationOption every FDL-required
// field (ErrorIdentifier, CommandIdentifier, ErrorTime, Description,
// RequiredInputData, DefaultOption, AutomaticSelectionTimeout) and batch 2a
// wired the cloud transport through the same fillRecoverableErrorsResponse()
// as this gRPC path — but the stream test above only ever asserted
// errormessage(), so a regression in any of the other seven fields would
// have gone uncaught on the wire (SC7 review, S6 2b).
TEST(ErrorRecoveryServiceE2E, SubscribeRecoverableErrorsEmitsFullStructure) {
    RecoveryFixture fx;
    const std::string propertyId = sila2::recovery::kRecoverableErrorsPropertyId;

    grpc::ClientContext ctx;
    errorrecovery_proto::Subscribe_RecoverableErrors_Parameters request;
    auto stream = fx.stub->Subscribe_RecoverableErrors(&ctx, request);

    for (int attempt = 0; attempt < 100 && fx.propMgr.subscriberCount(propertyId) == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(fx.propMgr.subscriberCount(propertyId), 1u);

    std::optional<RecoveryChoice> result;
    // Two fully-populated options, one flagged isDefault with a non-zero
    // automaticSelectionTimeout: DefaultOption/AutomaticSelectionTimeout on
    // the wire are derived from that flagged option (RecoverableErrorGate.h),
    // so this is the only shape that exercises the derivation. S39: the uuid
    // must be Length-36 lowercase-hex.
    std::thread worker = raisePendingError(fx.gate, result, "ffffffff-ffff-ffff-ffff-ffffffffffff", "Mixer stalled",
        {{"retry", false, std::chrono::seconds{0}, "Retry the operation", "SampleId"},
         {"skip", true, std::chrono::seconds{30}, "Skip this step", ""}});

    // First message is the property's current value on subscribe: an empty list.
    errorrecovery_proto::Subscribe_RecoverableErrors_Responses response;
    ASSERT_TRUE(stream->Read(&response));
    ASSERT_EQ(response.recoverableerrors_size(), 0);

    ASSERT_TRUE(stream->Read(&response));
    ASSERT_EQ(response.recoverableerrors_size(), 1);
    const auto& error = response.recoverableerrors(0).recoverableerror();
    EXPECT_EQ(error.erroridentifier().value(), kErrorIdentifier);
    EXPECT_EQ(error.commandidentifier().value(), kCommandIdentifier);
    EXPECT_EQ(error.commandexecutionuuid().value(), "ffffffff-ffff-ffff-ffff-ffffffffffff");
    EXPECT_EQ(error.errormessage().value(), "Mixer stalled");
    // year() alone proves errorTime was stamped from wall-clock time rather
    // than left at its zero-initialized default, without pinning an exact
    // timestamp the test would need to keep in sync with the clock.
    EXPECT_GE(error.errortime().year(), 2026u);
    ASSERT_EQ(error.continuationoptions_size(), 2);
    EXPECT_EQ(error.continuationoptions(0).continuationoption().identifier().value(), "retry");
    EXPECT_FALSE(error.continuationoptions(0).continuationoption().description().value().empty());
    // RequiredInputData was the one field this test wrote but never read back
    // on the wire (SC8 review).
    EXPECT_EQ(error.continuationoptions(0).continuationoption().requiredinputdata().value(), "SampleId");
    EXPECT_EQ(error.defaultoption().value(), "skip");
    EXPECT_EQ(error.automaticselectiontimeout().timeout().value(), 30);

    fx.gate.abort("ffffffff-ffff-ffff-ffff-ffffffffffff");
    worker.join();
    // Drain the second publish (empty list) abort() produces, so it isn't
    // mistaken for a fresh error by a test reading further.
    ASSERT_TRUE(stream->Read(&response));
    EXPECT_EQ(response.recoverableerrors_size(), 0);

    fx.propMgr.cancelAll(propertyId);
    EXPECT_FALSE(stream->Read(&response));
    const grpc::Status finishStatus = stream->Finish();
    EXPECT_TRUE(finishStatus.ok());
}

// S39: raiseAndWait() now enforces the FDL CommandExecutionUUID shape before
// publishing, so whatever the gate puts on the wire must always be something
// the inbound validators (ExecuteContinuationOption/AbortErrorHandling) will
// accept -- this is the closing, end-to-end assertion for that guarantee.
TEST(ErrorRecoveryServiceE2E, RaisedUuidRoundTripsThroughExecuteContinuationOption) {
    RecoveryFixture fx;
    const std::string propertyId = sila2::recovery::kRecoverableErrorsPropertyId;
    const std::string kCanonicalUuid = "12121212-1212-1212-1212-121212121212";

    grpc::ClientContext subscribeCtx;
    errorrecovery_proto::Subscribe_RecoverableErrors_Parameters subscribeRequest;
    auto stream = fx.stub->Subscribe_RecoverableErrors(&subscribeCtx, subscribeRequest);

    for (int attempt = 0; attempt < 100 && fx.propMgr.subscriberCount(propertyId) == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(fx.propMgr.subscriberCount(propertyId), 1u);

    std::optional<RecoveryChoice> result;
    std::thread worker = raisePendingError(fx.gate, result, kCanonicalUuid, "Round trip",
        {{"retry", false, std::chrono::seconds{0}}});

    // First message is the property's current value on subscribe: an empty list.
    errorrecovery_proto::Subscribe_RecoverableErrors_Responses response;
    ASSERT_TRUE(stream->Read(&response));
    ASSERT_EQ(response.recoverableerrors_size(), 0);

    ASSERT_TRUE(stream->Read(&response));
    ASSERT_EQ(response.recoverableerrors_size(), 1);
    const auto& error = response.recoverableerrors(0).recoverableerror();
    EXPECT_EQ(error.commandexecutionuuid().value(), kCanonicalUuid);

    // Send the exact wire value straight into ExecuteContinuationOption --
    // the real gRPC entry point, via the fixture's own stub.
    grpc::ClientContext execCtx;
    errorrecovery_proto::ExecuteContinuationOption_Parameters execRequest;
    execRequest.mutable_commandexecutionuuid()->set_value(error.commandexecutionuuid().value());
    execRequest.mutable_continuationoption()->set_value("retry");
    errorrecovery_proto::ExecuteContinuationOption_Responses execResponse;
    const grpc::Status execStatus = fx.stub->ExecuteContinuationOption(&execCtx, execRequest, &execResponse);
    EXPECT_TRUE(execStatus.ok());

    worker.join();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->optionIdentifier, "retry");

    // Drain the second publish (empty list) so it isn't mistaken for a fresh
    // error by a later read.
    ASSERT_TRUE(stream->Read(&response));
    EXPECT_EQ(response.recoverableerrors_size(), 0);

    fx.propMgr.cancelAll(propertyId);
    EXPECT_FALSE(stream->Read(&response));
    const grpc::Status finishStatus = stream->Finish();
    EXPECT_TRUE(finishStatus.ok());
}

// ---------------------------------------------------------------------------
// ExecuteContinuationOption — False paths (all three exercise the gate-error
// -> v2 DefinedExecutionError translation in ErrorRecoveryServiceImpl.cc's
// catch clauses; CAUGHT — the service translates every one of them.)
// ---------------------------------------------------------------------------

TEST(ErrorRecoveryServiceE2E, ExecuteContinuationOptionUnknownUuidReturnsInvalidCommandExecutionUUID) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    grpc::ServerContext ctx;
    errorrecovery_proto::ExecuteContinuationOption_Parameters request;
    request.mutable_commandexecutionuuid()->set_value("00000000-0000-0000-0000-000000000000");
    request.mutable_continuationoption()->set_value("retry");
    errorrecovery_proto::ExecuteContinuationOption_Responses response;

    const grpc::Status status = service.ExecuteContinuationOption(&ctx, &request, &response);

    EXPECT_EQ(definedErrorIdentifier(status), kInvalidUuidErrorId);
}

TEST(ErrorRecoveryServiceE2E, ExecuteContinuationOptionUnknownOptionReturnsUnknownContinuationOption) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    std::optional<RecoveryChoice> result;
    std::thread worker = raisePendingError(gate, result, "cccccccc-cccc-cccc-cccc-cccccccccccc", "Filter clogged",
        {{"retry", false, std::chrono::seconds{0}}});

    grpc::ServerContext ctx;
    errorrecovery_proto::ExecuteContinuationOption_Parameters request;
    request.mutable_commandexecutionuuid()->set_value("cccccccc-cccc-cccc-cccc-cccccccccccc");
    request.mutable_continuationoption()->set_value("does-not-exist");
    errorrecovery_proto::ExecuteContinuationOption_Responses response;

    const grpc::Status status = service.ExecuteContinuationOption(&ctx, &request, &response);

    EXPECT_EQ(definedErrorIdentifier(status), kUnknownOptionErrorId);

    // The failed call left raiseAndWait() still blocked; abort so the worker
    // thread can finish and be joined.
    gate.abort("cccccccc-cccc-cccc-cccc-cccccccccccc");
    worker.join();
}

TEST(ErrorRecoveryServiceE2E, ExecuteContinuationOptionOnResolvedUuidWithDifferentOptionReturnsInvalidCommandExecutionUUID) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    std::optional<RecoveryChoice> result;
    std::thread worker = raisePendingError(gate, result, "dddddddd-dddd-dddd-dddd-dddddddddddd", "Lid open",
        {{"retry", false, std::chrono::seconds{0}}, {"skip", false, std::chrono::seconds{0}}});

    grpc::ServerContext firstCtx;
    errorrecovery_proto::ExecuteContinuationOption_Parameters firstRequest;
    firstRequest.mutable_commandexecutionuuid()->set_value("dddddddd-dddd-dddd-dddd-dddddddddddd");
    firstRequest.mutable_continuationoption()->set_value("retry");
    errorrecovery_proto::ExecuteContinuationOption_Responses firstResponse;
    ASSERT_TRUE(service.ExecuteContinuationOption(&firstCtx, &firstRequest, &firstResponse).ok());
    worker.join();

    // uuid-5 is now resolved with "retry" — selecting a *different* option on
    // the same, already-resolved uuid hits the gate's own
    // InvalidCommandExecutionUUID DefinedExecutionError (RecoveryAlreadyResolved
    // is not FDL-declared, so the gate throws the same identifier a client
    // would see for an unknown UUID) and reaches the wire unremapped.
    grpc::ServerContext secondCtx;
    errorrecovery_proto::ExecuteContinuationOption_Parameters secondRequest;
    secondRequest.mutable_commandexecutionuuid()->set_value("dddddddd-dddd-dddd-dddd-dddddddddddd");
    secondRequest.mutable_continuationoption()->set_value("skip");
    errorrecovery_proto::ExecuteContinuationOption_Responses secondResponse;

    const grpc::Status status = service.ExecuteContinuationOption(&secondCtx, &secondRequest, &secondResponse);

    EXPECT_EQ(definedErrorIdentifier(status), kInvalidUuidErrorId);
}

// ---------------------------------------------------------------------------
// AbortErrorHandling — False path (CAUGHT)
// ---------------------------------------------------------------------------

TEST(ErrorRecoveryServiceE2E, AbortErrorHandlingUnknownUuidReturnsInvalidCommandExecutionUUID) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    grpc::ServerContext ctx;
    errorrecovery_proto::AbortErrorHandling_Parameters request;
    request.mutable_commandexecutionuuid()->set_value("00000000-0000-0000-0000-000000000000");
    errorrecovery_proto::AbortErrorHandling_Responses response;

    const grpc::Status status = service.AbortErrorHandling(&ctx, &request, &response);

    EXPECT_EQ(definedErrorIdentifier(status), kInvalidUuidErrorId);
}

// ---------------------------------------------------------------------------
// ExecuteContinuationOption / AbortErrorHandling / SetErrorHandlingTimeout
// — Parameter validation (S22-style: FDL Length/Pattern/MinimalInclusive
// constraints, checked before the request ever reaches the gate)
// ---------------------------------------------------------------------------

TEST(ErrorRecoveryServiceE2E, ExecuteContinuationOptionMalformedUuidReturnsValidationError) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    grpc::ServerContext ctx;
    errorrecovery_proto::ExecuteContinuationOption_Parameters request;
    // Not 36 characters and not lowercase-hex UUID shape.
    request.mutable_commandexecutionuuid()->set_value("not-a-uuid");
    request.mutable_continuationoption()->set_value("retry");
    errorrecovery_proto::ExecuteContinuationOption_Responses response;

    const grpc::Status status = service.ExecuteContinuationOption(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(),
              "org.silastandard/core/ErrorRecoveryService/v2/Command/"
              "ExecuteContinuationOption/Parameter/CommandExecutionUUID");
}

TEST(ErrorRecoveryServiceE2E, AbortErrorHandlingMalformedUuidReturnsValidationError) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    grpc::ServerContext ctx;
    errorrecovery_proto::AbortErrorHandling_Parameters request;
    request.mutable_commandexecutionuuid()->set_value("not-a-uuid");
    errorrecovery_proto::AbortErrorHandling_Responses response;

    const grpc::Status status = service.AbortErrorHandling(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(),
              "org.silastandard/core/ErrorRecoveryService/v2/Command/"
              "AbortErrorHandling/Parameter/CommandExecutionUUID");
}

// ErrorRecoveryService-v2_0.sila.xml:339-340: Timeout DataTypeDefinition
// constrains this to MinimalInclusive 0.
TEST(ErrorRecoveryServiceE2E, SetErrorHandlingTimeoutNegativeReturnsValidationError) {
    ObservablePropertyManager propMgr;
    RecoverableErrorGate gate{propMgr};
    ErrorRecoveryServiceImpl service{gate, propMgr};

    grpc::ServerContext ctx;
    errorrecovery_proto::SetErrorHandlingTimeout_Parameters request;
    request.mutable_errorhandlingtimeout()->mutable_timeout()->set_value(-1);
    errorrecovery_proto::SetErrorHandlingTimeout_Responses response;

    const grpc::Status status = service.SetErrorHandlingTimeout(&ctx, &request, &response);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);
    const auto* err = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->parameter(),
              "org.silastandard/core/ErrorRecoveryService/v2/Command/"
              "SetErrorHandlingTimeout/Parameter/Timeout");
}

// ---------------------------------------------------------------------------
// Regression guard: advertised features must actually be served
// (architecture-v2.md §3.12, S8b). This drives a real SilaServerBase rather
// than the bare ErrorRecoveryServiceImpl fixture above, since the invariant
// is about what Get_ImplementedFeatures reports versus what gRPC actually
// serves on the wire. Note gRPC returns UNIMPLEMENTED for an unknown METHOD
// too, so this calls one real RPC per advertised feature rather than a
// made-up method name, which would prove nothing.
// ---------------------------------------------------------------------------

TEST(ErrorRecoveryServiceE2E, EveryAdvertisedFeatureIsActuallyServedNotUnimplemented) {
    constexpr uint16_t kPort = 50264;
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withDiscovery(kPort)
                      .withErrorRecovery()
                      .build();
    server.run(false);

    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    auto channel = grpc::CreateChannel("localhost:" + std::to_string(kPort), grpc::SslCredentials(opts));

    auto siLAServiceStub = sila2::silaservice_proto::SiLAService::NewStub(channel);
    grpc::ClientContext listCtx;
    sila2::silaservice_proto::Get_ImplementedFeatures_Parameters listRequest;
    sila2::silaservice_proto::Get_ImplementedFeatures_Responses listResponse;
    ASSERT_TRUE(siLAServiceStub->Get_ImplementedFeatures(&listCtx, listRequest, &listResponse).ok());

    std::vector<std::string> fqis;
    for (const auto& feature : listResponse.implementedfeatures()) {
        fqis.push_back(feature.value());
    }
    // This test server enables SiLAService + ErrorRecoveryService, plus
    // ConnectionConfigurationService which build() now registers
    // unconditionally (S75, Part A p80 SHALL) — fail loudly rather than
    // silently under-covering if build() ever starts advertising something
    // else by default.
    ASSERT_EQ(fqis.size(), 3u);

    for (const auto& fqi : fqis) {
        if (fqi == std::string{sila2::kSiLAServiceFqi}) {
            // Real RPC on SiLAService itself: Get_ServerUUID.
            grpc::ClientContext ctx;
            sila2::silaservice_proto::Get_ServerUUID_Parameters request;
            sila2::silaservice_proto::Get_ServerUUID_Responses response;
            const grpc::Status status = siLAServiceStub->Get_ServerUUID(&ctx, request, &response);
            EXPECT_NE(status.error_code(), grpc::StatusCode::UNIMPLEMENTED)
                << "advertised FQI " << fqi << " is not actually served";
        } else if (fqi == std::string{sila2::kErrorRecoveryServiceFqi}) {
            // Real RPC on ErrorRecoveryService: SetErrorHandlingTimeout
            // always returns OK regardless of gate state.
            auto recoveryStub = errorrecovery_proto::ErrorRecoveryService::NewStub(channel);
            grpc::ClientContext ctx;
            errorrecovery_proto::SetErrorHandlingTimeout_Parameters request;
            request.mutable_errorhandlingtimeout()->mutable_timeout()->set_value(60);
            errorrecovery_proto::SetErrorHandlingTimeout_Responses response;
            const grpc::Status status = recoveryStub->SetErrorHandlingTimeout(&ctx, request, &response);
            EXPECT_NE(status.error_code(), grpc::StatusCode::UNIMPLEMENTED)
                << "advertised FQI " << fqi << " is not actually served";
        } else if (fqi == std::string{sila2::kConnectionConfigurationServiceFqi}) {
            // Real RPC on ConnectionConfigurationService: this server never
            // called withConnectionConfiguration, but Part A p32 (SHALL
            // support) means build() derived working defaults, so Enable
            // succeeds -- the check here is still just "not UNIMPLEMENTED".
            auto connConfigStub = connconfig_proto::ConnectionConfigurationService::NewStub(channel);
            grpc::ClientContext ctx;
            connconfig_proto::EnableServerInitiatedConnectionMode_Parameters request;
            connconfig_proto::EnableServerInitiatedConnectionMode_Responses response;
            const grpc::Status status = connConfigStub->EnableServerInitiatedConnectionMode(&ctx, request, &response);
            EXPECT_NE(status.error_code(), grpc::StatusCode::UNIMPLEMENTED)
                << "advertised FQI " << fqi << " is not actually served";
        } else {
            ADD_FAILURE() << "unexpected advertised FQI " << fqi
                           << " has no real-RPC check wired up in this test";
        }
    }

    server.shutdown();
    // Enable above wrote the default store file this server derived (no
    // withPersistentUuid was given, so build() keys it by the server's UUID
    // under the temp directory -- SilaServerBase.cc's default derivation).
    std::filesystem::remove(std::filesystem::temp_directory_path() /
        ("sila2-connections-" + server.serverConfig().uuid()));
}
