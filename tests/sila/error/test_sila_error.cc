// Checks for the sila_cpp SiLAError port: errorTypeToString()'s round trip
// over all 5 ErrorType values, and the empty-message fallback that the
// protected constructor applies before handing the message to
// std::runtime_error. toStatus() is "= delete"d (depends on protobuf codegen
// not yet wired into the build) and is intentionally not exercised here.
#include <sila/error/SiLAError.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace
{
/// Minimal concrete subclass so the abstract SiLAError can be constructed.
/// makeErrorMessage() is never called by these tests (only toStatus()/what()
/// would call it, and neither is exercised here), so its override just needs
/// to satisfy the pure virtual. It aborts rather than returning — a real
/// return would construct a unique_ptr<sila2::org::silastandard::SiLAError>,
/// which needs that (forward-declared, not yet codegen'd) type complete.
class TestError : public sila2::error::SiLAError
{
public:
    TestError(ErrorType type, std::string msg) : SiLAError{type, std::move(msg)} {}

protected:
    [[nodiscard]] std::unique_ptr<sila2::org::silastandard::SiLAError>
    makeErrorMessage() const override
    {
        ADD_FAILURE() << "makeErrorMessage() should not be called by this test";
        std::abort();
    }
};
}  // namespace

TEST(SiLAError, ErrorTypeToStringRoundTripsAllValues)
{
    using ErrorType = sila2::error::SiLAError::ErrorType;
    EXPECT_EQ(sila2::error::SiLAError::errorTypeToString(ErrorType::DefinedExecutionError),
              "Defined Execution Error");
    EXPECT_EQ(sila2::error::SiLAError::errorTypeToString(ErrorType::UndefinedExecutionError),
              "Undefined Execution Error");
    EXPECT_EQ(sila2::error::SiLAError::errorTypeToString(ErrorType::FrameworkError),
              "Framework Error");
    EXPECT_EQ(sila2::error::SiLAError::errorTypeToString(ErrorType::ValidationError),
              "Validation Error");
    EXPECT_EQ(sila2::error::SiLAError::errorTypeToString(ErrorType::ConnectionError),
              "Connection Error");
}

TEST(SiLAError, EmptyMessageFallsBackToGenericMessage)
{
    const TestError error{sila2::error::SiLAError::ErrorType::ValidationError, ""};
    EXPECT_EQ(error.message(),
              "A Validation Error occurred while executing a SiLA 2 Command or "
              "reading a Property!");
    EXPECT_EQ(error.errorType(), sila2::error::SiLAError::ErrorType::ValidationError);
    EXPECT_EQ(error.errorTypeName(), "Validation Error");
}

TEST(SiLAError, NonEmptyMessageIsKeptAsIs)
{
    const TestError error{sila2::error::SiLAError::ErrorType::FrameworkError, "custom detail"};
    EXPECT_EQ(error.message(), "custom detail");
}
