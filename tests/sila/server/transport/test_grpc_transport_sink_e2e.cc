// End-to-end tests for GrpcUnaryResponseSink::fail() and
// GrpcStreamResponseSink::fail() (architecture.md §3.8): the SiLAError ->
// grpc::Status conversion every gRPC service method relies on to report a
// rejected/thrown Command or Property as a spec-conformant ABORTED status.
// fail() itself has no branches (errorPaths: [] for both classes) — the
// "false" cases here instead probe the sink's own invariants via direct
// construction (calling status() before fail(), calling fail() twice), the
// way the caller's methodology calls for on flows with no explicit
// rejection logic.
//
// writer_ is left null in the GrpcStreamResponseSink tests: fail()/status()
// never touch it (only send() does), so a real streaming RPC isn't needed to
// exercise this flow.
#include <sila/server/transport/GrpcTransport.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

#include "SiLAFramework.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
// GrpcStreamResponseSink<T>::send() is a virtual override, so its body
// (which calls grpc::ServerWriter<T>::Write()) is instantiated even though
// these tests never call send() — needs the full ServerWriter definition,
// which grpcpp.h itself only forward-declares.
#include <grpcpp/support/sync_stream.h>

#include <memory>
#include <string>

namespace {

using sila2::GrpcStreamResponseSink;
using sila2::GrpcUnaryResponseSink;
using sila2::error::DefinedExecutionError;
using sila2::error::FrameworkError;
using sila2::error::SiLAError;
using sila2::error::ValidationError;
using sila2::error::fromGrpcStatus;

using SilaString = sila2::org::silastandard::String;

}  // namespace

// ---------------------------------------------------------------------------
// True paths — fail() converts each SiLAError subtype to an ABORTED status
// that round-trips back to the same information via fromGrpcStatus().
// ---------------------------------------------------------------------------

TEST(GrpcTransportSinkE2E, UnarySinkFailWithDefinedExecutionErrorProducesAbortedStatus) {
    SilaString response;
    GrpcUnaryResponseSink<SilaString> sink{&response};

    sink.fail(DefinedExecutionError{"org.example/Feature/v1/DefinedExecutionError/Broken", "It broke"});
    const grpc::Status status = sink.status();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(), "org.example/Feature/v1/DefinedExecutionError/Broken");
    EXPECT_EQ(std::string{definedError->what()}, "It broke");
}

TEST(GrpcTransportSinkE2E, UnarySinkFailWithValidationErrorProducesAbortedStatus) {
    SilaString response;
    GrpcUnaryResponseSink<SilaString> sink{&response};

    sink.fail(ValidationError{"org.example/Feature/v1/Command/Foo/Parameter/Bar", "Out of range"});
    const grpc::Status status = sink.status();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::ValidationError);
    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    EXPECT_EQ(validationError->parameter(), "org.example/Feature/v1/Command/Foo/Parameter/Bar");
}

TEST(GrpcTransportSinkE2E, StreamSinkFailWithFrameworkErrorProducesAbortedStatus) {
    // writer_ is never used by fail()/status() — see file header.
    GrpcStreamResponseSink<SilaString> sink{static_cast<grpc::ServerWriter<SilaString>*>(nullptr)};

    sink.fail(FrameworkError{FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid});
    const grpc::Status status = sink.status();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    const auto* frameworkError = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(frameworkError, nullptr);
    EXPECT_EQ(frameworkError->frameworkErrorType(), FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid);
}

// ---------------------------------------------------------------------------
// False paths — sink-level invariants, via direct construction (fail() has
// no rejection logic of its own to trigger through a bad input).
// ---------------------------------------------------------------------------

// CAUGHT: status_ defaults to grpc::Status::OK, so a sink that fail() never
// touched reports success rather than an undefined/garbage status.
TEST(GrpcTransportSinkE2E, UnarySinkStatusDefaultsToOkWithoutFail) {
    SilaString response;
    GrpcUnaryResponseSink<SilaString> sink{&response};

    EXPECT_TRUE(sink.status().ok());
}

// UNCAUGHT: fail() has no guard against being called more than once — the
// second call silently overwrites the first with no diagnostic, which could
// hide the original failure if a handler ever called fail() twice.
TEST(GrpcTransportSinkE2E, UnarySinkSecondFailCallSilentlyOverwritesFirst) {
    SilaString response;
    GrpcUnaryResponseSink<SilaString> sink{&response};

    sink.fail(DefinedExecutionError{"org.example/Feature/v1/DefinedExecutionError/First", "first"});
    sink.fail(DefinedExecutionError{"org.example/Feature/v1/DefinedExecutionError/Second", "second"});
    const grpc::Status status = sink.status();

    const auto reconstructed = fromGrpcStatus(status);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(), "org.example/Feature/v1/DefinedExecutionError/Second");
}

// CAUGHT: an empty error message is not rejected by the sink — it flows
// through to SiLAError's own generic-message fallback (SiLAError.cc's
// resolveMessage()), so the resulting status is still well-formed.
TEST(GrpcTransportSinkE2E, StreamSinkFailWithEmptyMessageFallsBackToGenericMessage) {
    GrpcStreamResponseSink<SilaString> sink{static_cast<grpc::ServerWriter<SilaString>*>(nullptr)};

    sink.fail(DefinedExecutionError{"org.example/Feature/v1/DefinedExecutionError/Broken", ""});
    const grpc::Status status = sink.status();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    EXPECT_FALSE(status.error_message().empty());
    const auto reconstructed = fromGrpcStatus(status);
    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_FALSE(std::string{definedError->what()}.empty());
}
