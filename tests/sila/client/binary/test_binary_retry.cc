// Tests for backoffDelay() (exponential backoff with attempt clamp and max
// cap) and parseBinaryTransferError() (ABORTED status -> BinaryTransferError
// decoding), the two free functions BinaryRetry.h provides.
#include <sila/client/binary/BinaryRetry.h>

#include <gtest/gtest.h>

namespace {

namespace bt = sila2::org::silastandard;
using namespace std::chrono_literals;

TEST(BackoffDelay, ExponentialGrowthUpToClamp) {
    auto maxBackoff = 120s;
    EXPECT_EQ(sila2::backoffDelay(0, maxBackoff), 1000ms);
    EXPECT_EQ(sila2::backoffDelay(1, maxBackoff), 2000ms);
    EXPECT_EQ(sila2::backoffDelay(2, maxBackoff), 4000ms);
    EXPECT_EQ(sila2::backoffDelay(3, maxBackoff), 8000ms);
    EXPECT_EQ(sila2::backoffDelay(4, maxBackoff), 16000ms);
    EXPECT_EQ(sila2::backoffDelay(5, maxBackoff), 32000ms);
    EXPECT_EQ(sila2::backoffDelay(6, maxBackoff), 64000ms);
}

TEST(BackoffDelay, AttemptClampedAtSix) {
    auto maxBackoff = 120s;
    EXPECT_EQ(sila2::backoffDelay(7, maxBackoff), 64000ms);
    EXPECT_EQ(sila2::backoffDelay(100, maxBackoff), 64000ms);
}

TEST(BackoffDelay, MaxBackoffCapsResult) {
    EXPECT_EQ(sila2::backoffDelay(6, 10s), 10000ms);
    EXPECT_EQ(sila2::backoffDelay(3, 5s), 5000ms);
}

TEST(BackoffDelay, ZeroMaxBackoffReturnsZero) {
    EXPECT_EQ(sila2::backoffDelay(0, 0s), 0ms);
    EXPECT_EQ(sila2::backoffDelay(5, 0s), 0ms);
}

TEST(BackoffDelay, MaxBackoffExactlyMatchesDelayReturnsDelay) {
    EXPECT_EQ(sila2::backoffDelay(2, 4s), 4000ms);
}

TEST(BackoffDelay, FirstAttemptWithOneSecondMaxReturnsOneSecond) {
    EXPECT_EQ(sila2::backoffDelay(0, 1s), 1000ms);
}

namespace {
grpc::Status makeAbortedStatus(bt::BinaryTransferError_ErrorType type) {
    bt::BinaryTransferError error;
    error.set_errortype(type);
    return grpc::Status{grpc::StatusCode::ABORTED, "binary transfer error", error.SerializeAsString()};
}
}  // namespace

TEST(ParseBinaryTransferError, AbortedWithInvalidUuidReturnsThatType) {
    grpc::Status status = makeAbortedStatus(bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);

    EXPECT_EQ(sila2::parseBinaryTransferError(status), bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

TEST(ParseBinaryTransferError, AbortedWithBinaryDownloadFailedReturnsThatType) {
    grpc::Status status = makeAbortedStatus(bt::BinaryTransferError::BINARY_DOWNLOAD_FAILED);

    EXPECT_EQ(sila2::parseBinaryTransferError(status), bt::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
}

TEST(ParseBinaryTransferError, AbortedWithBinaryUploadFailedReturnsThatType) {
    grpc::Status status = makeAbortedStatus(bt::BinaryTransferError::BINARY_UPLOAD_FAILED);

    EXPECT_EQ(sila2::parseBinaryTransferError(status), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

TEST(ParseBinaryTransferError, NonAbortedStatusReturnsNullopt) {
    grpc::Status status{grpc::StatusCode::INTERNAL, "some other error"};

    EXPECT_EQ(sila2::parseBinaryTransferError(status), std::nullopt);
}

TEST(ParseBinaryTransferError, AbortedWithUnparseableDetailsReturnsNullopt) {
    // A lone continuation-bit byte is an incomplete varint tag: ParseFromString
    // must fail on it rather than silently accept it as an unknown field.
    grpc::Status status{grpc::StatusCode::ABORTED, "aborted", "\xff"};

    EXPECT_EQ(sila2::parseBinaryTransferError(status), std::nullopt);
}

TEST(ParseBinaryTransferError, OkStatusReturnsNullopt) {
    EXPECT_EQ(sila2::parseBinaryTransferError(grpc::Status::OK), std::nullopt);
}

}  // namespace
