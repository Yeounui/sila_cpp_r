// Checks for the sila_cpp SilaError port: errorTypeToString()'s round trip
// over all 5 ErrorType values, the empty-message fallback that the protected
// constructor applies before handing the message to std::runtime_error, and
// toStatus() → fromGrpcStatus() round-trips for each concrete error subtype.
#include <sila/common/error/SilaError.h>
#include <sila/common/error/SilaErrorException.h>
#include <sila/common/error/SilaErrorSubtypes.h>

#include <gtest/gtest.h>
#include <grpcpp/support/status.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace
{
/// Minimal concrete subclass so the abstract SilaError can be constructed.
/// makeErrorMessage() is never called by these tests (only toStatus()/what()
/// would call it, and neither is exercised here), so its override just needs
/// to satisfy the pure virtual. It aborts rather than returning — a real
/// return would construct a unique_ptr<sila2::org::silastandard::SiLAError>,
/// which needs that (forward-declared, not yet codegen'd) type complete.
class TestError : public sila2::error::SilaError
{
public:
    TestError(ErrorType type, std::string msg) : SilaError{type, std::move(msg)} {}

protected:
    [[nodiscard]] std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const override
    {
        ADD_FAILURE() << "makeErrorMessage() should not be called by this test";
        std::abort();
    }
};
}  // namespace

TEST(SilaError, ErrorTypeToStringRoundTripsAllValues)
{
    using ErrorType = sila2::error::SilaError::ErrorType;
    EXPECT_EQ(sila2::error::SilaError::errorTypeToString(ErrorType::DefinedExecutionError),
              "Defined Execution Error");
    EXPECT_EQ(sila2::error::SilaError::errorTypeToString(ErrorType::UndefinedExecutionError),
              "Undefined Execution Error");
    EXPECT_EQ(sila2::error::SilaError::errorTypeToString(ErrorType::FrameworkError),
              "Framework Error");
    EXPECT_EQ(sila2::error::SilaError::errorTypeToString(ErrorType::ValidationError),
              "Validation Error");
    EXPECT_EQ(sila2::error::SilaError::errorTypeToString(ErrorType::ConnectionError),
              "Connection Error");
}

TEST(SilaError, EmptyMessageFallsBackToGenericMessage)
{
    const TestError error{sila2::error::SilaError::ErrorType::ValidationError, ""};
    EXPECT_STREQ(error.what(),
                 "A Validation Error occurred while executing a SiLA 2 Command or "
                 "reading a Property!");
    EXPECT_EQ(error.errorType(), sila2::error::SilaError::ErrorType::ValidationError);
    EXPECT_EQ(error.errorTypeName(), "Validation Error");
}

TEST(SilaError, NonEmptyMessageIsKeptAsIs)
{
    const TestError error{sila2::error::SilaError::ErrorType::FrameworkError, "custom detail"};
    EXPECT_STREQ(error.what(), "custom detail");
}

// Round-trips below exercise toStatus() -> fromGrpcStatus(): construct a
// concrete SilaError, serialize it into a grpc::Status, then reconstruct it
// and check the reconstructed error carries the same fields as the original.
using namespace sila2::error;

TEST(SilaError, FromGrpcStatusRoundTripsValidationError)
{
    const ValidationError original{"param.name", "bad value"};
    const auto reconstructed = fromGrpcStatus(original.toStatus());

    ASSERT_NE(reconstructed, nullptr);
    EXPECT_EQ(reconstructed->errorType(), SilaError::ErrorType::ValidationError);

    const auto* validationError = dynamic_cast<const ValidationError*>(reconstructed.get());
    ASSERT_NE(validationError, nullptr);
    EXPECT_EQ(validationError->parameter(), "param.name");
    EXPECT_STREQ(validationError->what(), "bad value");
}

TEST(SilaError, FromGrpcStatusRoundTripsDefinedExecutionError)
{
    const DefinedExecutionError original{"org.silastandard/SomeError", "it broke"};
    const auto status = original.toStatus();
    EXPECT_EQ(status.error_message(), "EiYKGm9yZy5zaWxhc3RhbmRhcmQvU29tZUVycm9yEghpdCBicm9rZQ==");
    const auto reconstructed = fromGrpcStatus(status);

    ASSERT_NE(reconstructed, nullptr);
    EXPECT_EQ(reconstructed->errorType(), SilaError::ErrorType::DefinedExecutionError);

    const auto* definedError = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(definedError, nullptr);
    EXPECT_EQ(definedError->errorIdentifier(), "org.silastandard/SomeError");
    EXPECT_STREQ(definedError->what(), "it broke");
}

TEST(SilaError, FromGrpcStatusRoundTripsUndefinedExecutionError)
{
    const UndefinedExecutionError original{"unexpected failure"};
    const auto reconstructed = fromGrpcStatus(original.toStatus());

    ASSERT_NE(reconstructed, nullptr);
    EXPECT_EQ(reconstructed->errorType(), SilaError::ErrorType::UndefinedExecutionError);

    const auto* undefinedError = dynamic_cast<const UndefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(undefinedError, nullptr);
    EXPECT_STREQ(undefinedError->what(), "unexpected failure");
}

TEST(SilaError, FromGrpcStatusRoundTripsFrameworkError)
{
    const FrameworkError original{FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid,
                                   "bad uuid"};
    const auto reconstructed = fromGrpcStatus(original.toStatus());

    ASSERT_NE(reconstructed, nullptr);
    EXPECT_EQ(reconstructed->errorType(), SilaError::ErrorType::FrameworkError);

    const auto* frameworkError = dynamic_cast<const FrameworkError*>(reconstructed.get());
    ASSERT_NE(frameworkError, nullptr);
    EXPECT_EQ(frameworkError->frameworkErrorType(),
              FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid);
    EXPECT_STREQ(frameworkError->what(), "bad uuid");
}

TEST(SilaError, FromGrpcStatusRoundTripsConnectionError)
{
    // Any non-ABORTED status is treated as an infrastructure-level failure,
    // never a serialized SilaError payload.
    const grpc::Status status{grpc::StatusCode::UNAVAILABLE, "server down"};
    const auto reconstructed = fromGrpcStatus(status);

    ASSERT_NE(reconstructed, nullptr);
    EXPECT_EQ(reconstructed->errorType(), SilaError::ErrorType::ConnectionError);

    const auto* connectionError = dynamic_cast<const ConnectionError*>(reconstructed.get());
    ASSERT_NE(connectionError, nullptr);
    EXPECT_EQ(connectionError->statusCode(), grpc::StatusCode::UNAVAILABLE);
}

TEST(SilaError, FromGrpcStatusReturnsNullptrForOkStatus)
{
    const auto reconstructed = fromGrpcStatus(grpc::Status::OK);
    EXPECT_EQ(reconstructed, nullptr);
}
