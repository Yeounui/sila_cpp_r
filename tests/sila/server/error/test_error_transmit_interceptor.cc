// Tests for ErrorTransmitInterceptor::guardHandler: verifies that all
// exception types thrown by handlers are correctly routed to the ResponseSink
// as SiLA errors, preventing non-SiLA exceptions from leaking across the RPC
// boundary.
#include <sila/server/error/ErrorTransmitInterceptor.h>
#include <sila/common/error/SilaError.h>
#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/transport/ResponseSink.h>

#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <string>

namespace {

using sila2::error::SilaError;

class MockResponseSink : public sila2::ResponseSink<std::string> {
public:
    void send(const std::string&) override { sendCalled = true; }
    void finish() override { finishCalled = true; }
    void fail(const SilaError& error) override {
        failCalled = true;
        errorType = error.errorType();
        errorMessage = error.what();
    }

    bool sendCalled = false;
    bool finishCalled = false;
    bool failCalled = false;
    SilaError::ErrorType errorType{};
    std::string errorMessage;
};

// --- True (positive) paths --------------------------------------------------

TEST(ErrorTransmitInterceptor, HandlerSucceedsNoFailCalled) {
    MockResponseSink sink;
    sila2::error::guardHandler<std::string>([] {}, sink);

    EXPECT_FALSE(sink.failCalled);
    EXPECT_FALSE(sink.sendCalled);
    EXPECT_FALSE(sink.finishCalled);
}

TEST(ErrorTransmitInterceptor, ValidationErrorForwardedToSink) {
    MockResponseSink sink;
    sila2::error::guardHandler<std::string>([] {
        throw sila2::error::ValidationError{"param.name", "bad value"};
    }, sink);

    ASSERT_TRUE(sink.failCalled);
    EXPECT_EQ(sink.errorType, SilaError::ErrorType::ValidationError);
    EXPECT_EQ(sink.errorMessage, "bad value");
}

TEST(ErrorTransmitInterceptor, DefinedExecutionErrorForwardedToSink) {
    MockResponseSink sink;
    sila2::error::guardHandler<std::string>([] {
        throw sila2::error::DefinedExecutionError{
            "org.silastandard/SomeFeature/v1/SomeError", "pump jammed"};
    }, sink);

    ASSERT_TRUE(sink.failCalled);
    EXPECT_EQ(sink.errorType, SilaError::ErrorType::DefinedExecutionError);
    EXPECT_EQ(sink.errorMessage, "pump jammed");
}

TEST(ErrorTransmitInterceptor, FrameworkErrorForwardedToSink) {
    MockResponseSink sink;
    sila2::error::guardHandler<std::string>([] {
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidMetadata,
            "missing lock-id"};
    }, sink);

    ASSERT_TRUE(sink.failCalled);
    EXPECT_EQ(sink.errorType, SilaError::ErrorType::FrameworkError);
    EXPECT_EQ(sink.errorMessage, "missing lock-id");
}

// --- False (negative/edge) paths --------------------------------------------

TEST(ErrorTransmitInterceptor, StdRuntimeErrorWrappedAsUndefinedExecutionError) {
    MockResponseSink sink;
    sila2::error::guardHandler<std::string>([] {
        throw std::runtime_error{"disk full"};
    }, sink);

    ASSERT_TRUE(sink.failCalled);
    EXPECT_EQ(sink.errorType, SilaError::ErrorType::UndefinedExecutionError);
    EXPECT_EQ(sink.errorMessage, "disk full");
}

TEST(ErrorTransmitInterceptor, StdLogicErrorWrappedAsUndefinedExecutionError) {
    MockResponseSink sink;
    sila2::error::guardHandler<std::string>([] {
        throw std::logic_error{"out of range"};
    }, sink);

    ASSERT_TRUE(sink.failCalled);
    EXPECT_EQ(sink.errorType, SilaError::ErrorType::UndefinedExecutionError);
    EXPECT_EQ(sink.errorMessage, "out of range");
}

TEST(ErrorTransmitInterceptor, NonStdExceptionWrappedAsUnknown) {
    MockResponseSink sink;
    sila2::error::guardHandler<std::string>([] {
        throw 42;  // NOLINT: intentional non-exception throw
    }, sink);

    ASSERT_TRUE(sink.failCalled);
    EXPECT_EQ(sink.errorType, SilaError::ErrorType::UndefinedExecutionError);
    EXPECT_EQ(sink.errorMessage, "unknown exception");
}

}  // namespace
