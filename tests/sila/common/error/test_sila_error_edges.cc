// Edge-case tests for the SiLA error module: paths not covered by
// test_sila_error.cc (which tests errorTypeToString, empty-message fallback,
// and toStatus/fromGrpcStatus round-trips for all 5 error types).
#include <sila/common/error/SilaError.h>
#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>

#include <gtest/gtest.h>
#include <grpcpp/support/status.h>

#include "SiLAFramework.pb.h"

#include <set>
#include <stdexcept>
#include <string>

namespace {

using sila2::error::ConnectionError;
using sila2::error::FrameworkError;
using sila2::error::SilaError;
using sila2::error::UndefinedExecutionError;
using sila2::error::fromGrpcStatus;

// --- True (positive) paths --------------------------------------------------

TEST(SilaErrorEdges, FrameworkErrorEmptyMessageUsesDefaultForCommandNotAccepted) {
    const FrameworkError error{FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted};
    EXPECT_STREQ(error.what(),
                 "The SiLA Server does not accept the Command Execution.");
    EXPECT_EQ(error.frameworkErrorType(),
              FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted);
}

TEST(SilaErrorEdges, FrameworkErrorEmptyMessageUsesDefaultForInvalidUuid) {
    const FrameworkError error{FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid};
    EXPECT_STREQ(error.what(), "The Command Execution UUID is invalid.");
}

TEST(SilaErrorEdges, FrameworkErrorEmptyMessageUsesDefaultForNotFinished) {
    const FrameworkError error{FrameworkError::FrameworkErrorType::CommandExecutionNotFinished};
    EXPECT_STREQ(error.what(), "The Command Execution is not finished yet.");
}

TEST(SilaErrorEdges, UndefinedExecutionErrorEmptyMessageUsesGenericFallback) {
    const UndefinedExecutionError error{""};
    const std::string msg = error.what();
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find("Undefined Execution Error"), std::string::npos);
}

// Pins toProtoErrorType's switch against SiLAFramework.proto:110-116 --
// exactly five values, each landing on its own distinct proto number. This is
// the case that would have caught the deleted `Invalid` sentinel silently
// mapping to proto value 0 alongside CommandExecutionNotAccepted.
TEST(SilaErrorEdges, FrameworkErrorEveryTypeMapsToItsProtoValue) {
    using FET = FrameworkError::FrameworkErrorType;
    using PET = sila2::org::silastandard::FrameworkError;

    const FrameworkError commandNotAccepted{FET::CommandExecutionNotAccepted};
    const FrameworkError invalidUuid{FET::InvalidCommandExecutionUuid};
    const FrameworkError notFinished{FET::CommandExecutionNotFinished};
    const FrameworkError invalidMetadata{FET::InvalidMetadata};
    const FrameworkError noMetadataAllowed{FET::NoMetadataAllowed};

    EXPECT_EQ(commandNotAccepted.toProto()->frameworkerror().errortype(),
              PET::COMMAND_EXECUTION_NOT_ACCEPTED);
    EXPECT_EQ(invalidUuid.toProto()->frameworkerror().errortype(),
              PET::INVALID_COMMAND_EXECUTION_UUID);
    EXPECT_EQ(notFinished.toProto()->frameworkerror().errortype(),
              PET::COMMAND_EXECUTION_NOT_FINISHED);
    EXPECT_EQ(invalidMetadata.toProto()->frameworkerror().errortype(),
              PET::INVALID_METADATA);
    EXPECT_EQ(noMetadataAllowed.toProto()->frameworkerror().errortype(),
              PET::NO_METADATA_ALLOWED);

    // Built from toProto() results, not proto literals — a set of literals
    // is distinct by construction and would stay green if two enumerators
    // ever mapped to the same wire value.
    const std::set<int> protoValues{
        commandNotAccepted.toProto()->frameworkerror().errortype(),
        invalidUuid.toProto()->frameworkerror().errortype(),
        notFinished.toProto()->frameworkerror().errortype(),
        invalidMetadata.toProto()->frameworkerror().errortype(),
        noMetadataAllowed.toProto()->frameworkerror().errortype()};
    EXPECT_EQ(protoValues.size(), 5u);
}

// Rejection: no enumerator may produce an empty default message. The deleted
// `Invalid` arm was the only one that produced errortype()==0 (proto default)
// with a blank message via defaultMessageForType's fallthrough (:88-89) -- this
// pins that no future sentinel-shaped value can reappear undetected.
TEST(SilaErrorEdges, FrameworkErrorDefaultMessageIsNeverEmpty) {
    using FET = FrameworkError::FrameworkErrorType;

    const FrameworkError commandNotAccepted{FET::CommandExecutionNotAccepted, ""};
    const FrameworkError invalidUuid{FET::InvalidCommandExecutionUuid, ""};
    const FrameworkError notFinished{FET::CommandExecutionNotFinished, ""};
    const FrameworkError invalidMetadata{FET::InvalidMetadata, ""};
    const FrameworkError noMetadataAllowed{FET::NoMetadataAllowed, ""};

    EXPECT_STRNE(commandNotAccepted.what(), "");
    EXPECT_STRNE(invalidUuid.what(), "");
    EXPECT_STRNE(notFinished.what(), "");
    EXPECT_STRNE(invalidMetadata.what(), "");
    EXPECT_STRNE(noMetadataAllowed.what(), "");

    // The one enumerator whose proto errortype() is 0 (the wire default) still
    // carries a real message, not the "" a sentinel fallthrough would leave.
    EXPECT_STREQ(commandNotAccepted.what(),
                 "The SiLA Server does not accept the Command Execution.");
}

// --- False (negative/edge) paths --------------------------------------------

TEST(SilaErrorEdges, FromGrpcStatusAbortedWithUnparseableDetailsReturnsUndefined) {
    const grpc::Status status{grpc::StatusCode::ABORTED, "bad payload",
                              "not-valid-protobuf-bytes"};
    auto error = fromGrpcStatus(status);

    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->errorType(), SilaError::ErrorType::UndefinedExecutionError);
    EXPECT_STREQ(error->what(), "bad payload");
}

TEST(SilaErrorEdges, FromGrpcStatusAbortedWithErrorNotSetReturnsUndefined) {
    sila2::org::silastandard::SiLAError emptyProto;
    const grpc::Status status{grpc::StatusCode::ABORTED, "empty error",
                              emptyProto.SerializeAsString()};
    auto error = fromGrpcStatus(status);

    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->errorType(), SilaError::ErrorType::UndefinedExecutionError);
    EXPECT_STREQ(error->what(), "empty error");
}

TEST(SilaErrorEdges, ConnectionErrorToStatusThrowsLogicError) {
    const ConnectionError error{
        grpc::Status{grpc::StatusCode::UNAVAILABLE, "server down"}};
    EXPECT_THROW(error.toStatus(), std::logic_error);
}

}  // namespace
