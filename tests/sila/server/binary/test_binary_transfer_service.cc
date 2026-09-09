// Tests for BinaryUploadService and BinaryDownloadService: e2e coverage of
// CreateBinary/UploadChunk/GetBinaryInfo/GetChunk/DeleteBinary through a real
// local gRPC server+stub, including the ABORTED/INVALID_BINARY_TRANSFER_UUID
// rejection path both services share via BinaryUtil::makeBinaryTransferStatus().
#include <sila/server/binary/BinaryDownloadService.h>
#include <sila/server/binary/BinaryUploadService.h>
#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/server/SilaServerBase.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/client/binary/BinaryDownloader.h>
#include <sila/client/binary/BinaryUploader.h>
#include <sila/common/binary/BinaryChunkLimit.h>
#include <sila/common/util/base64.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <SiLABinaryTransfer.grpc.pb.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using sila2::BinaryDownloadService;
using sila2::BinaryUploadService;
using sila2::InMemoryBinaryStore;
namespace bt = sila2::org::silastandard;
using namespace std::chrono_literals;

// Real local server+channel with both services registered, so the ABORTED
// status + serialized BinaryTransferError details actually cross the wire
// (a mock service can't exercise grpc's status/error_details encoding).
class BinaryTransferService : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&uploadService_);
        builder.RegisterService(&downloadService_);
        server_ = builder.BuildAndStart();
        if (!server_) GTEST_SKIP() << "local gRPC listener unavailable";
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        uploadStub_ = bt::BinaryUpload::NewStub(channel_);
        downloadStub_ = bt::BinaryDownload::NewStub(channel_);
    }

    void TearDown() override {
        if (server_) server_->Shutdown();
    }

    // Runs CreateBinary + a full UploadChunk stream for a single-chunk binary
    // and returns the generated UUID. Shared by every test that needs an
    // already-uploaded binary to act on.
    std::string uploadWholeBinary(const std::string& data, const std::string& parameterIdentifier = "param") {
        bt::CreateBinaryRequest createRequest;
        createRequest.set_binarysize(data.size());
        createRequest.set_chunkcount(1);
        createRequest.set_parameteridentifier(parameterIdentifier);
        bt::CreateBinaryResponse createResponse;
        grpc::ClientContext createCtx;
        uploadStub_->CreateBinary(&createCtx, createRequest, &createResponse);
        const std::string uuid = createResponse.binarytransferuuid();

        grpc::ClientContext uploadCtx;
        auto stream = uploadStub_->UploadChunk(&uploadCtx);
        bt::UploadChunkRequest chunkRequest;
        chunkRequest.set_binarytransferuuid(uuid);
        chunkRequest.set_chunkindex(0);
        chunkRequest.set_payload(data);
        stream->Write(chunkRequest);
        bt::UploadChunkResponse chunkResponse;
        stream->Read(&chunkResponse);
        stream->WritesDone();
        stream->Finish();

        return uuid;
    }

    InMemoryBinaryStore store_;
    BinaryUploadService uploadService_{store_, 300s};
    BinaryDownloadService downloadService_{store_, 300s};
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<bt::BinaryUpload::Stub> uploadStub_;
    std::unique_ptr<bt::BinaryDownload::Stub> downloadStub_;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST_F(BinaryTransferService, CreateBinaryReturnsUuidAndLifetime) {
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(1);
    request.set_parameteridentifier("param");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(response.binarytransferuuid().empty());
    EXPECT_EQ(response.lifetimeofbinary().seconds(), 300);
}

TEST_F(BinaryTransferService, CreateBinaryAcceptsChunkCountAtAbsoluteCeiling) {
    // Both bounds InMemoryBinaryStore::createSlot enforces (binarySize-proportional
    // and the absolute kMaxChunkCount ceiling) are satisfied simultaneously here:
    // chunkCount == binarySize == kMaxChunkCount.
    bt::CreateBinaryRequest request;
    request.set_binarysize(InMemoryBinaryStore::kMaxChunkCount);
    request.set_chunkcount(InMemoryBinaryStore::kMaxChunkCount);
    request.set_parameteridentifier("param");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.binarytransferuuid().empty());
}

TEST_F(BinaryTransferService, FullUploadViaChunkStream) {
    bt::CreateBinaryRequest createRequest;
    createRequest.set_binarysize(5);
    createRequest.set_chunkcount(1);
    createRequest.set_parameteridentifier("param");
    bt::CreateBinaryResponse createResponse;
    grpc::ClientContext createCtx;
    uploadStub_->CreateBinary(&createCtx, createRequest, &createResponse);
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;
    auto stream = uploadStub_->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunkRequest;
    chunkRequest.set_binarytransferuuid(uuid);
    chunkRequest.set_chunkindex(0);
    chunkRequest.set_payload("hello");
    ASSERT_TRUE(stream->Write(chunkRequest));

    bt::UploadChunkResponse chunkResponse;
    ASSERT_TRUE(stream->Read(&chunkResponse));
    stream->WritesDone();
    grpc::Status status = stream->Finish();

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(chunkResponse.binarytransferuuid(), uuid);
    EXPECT_EQ(chunkResponse.chunkindex(), 0u);
}

TEST_F(BinaryTransferService, StoreChunkRefreshesLifetimeSoSlowUploadSurvivesGcSweep) {
    // A 3s lifetime instead of the fixture's 300s default keeps this test's
    // real sleeps bounded, while the 2s gaps below leave a wide margin
    // against scheduling jitter on either side of the pass/fail line. Set
    // directly on the store rather than via CreateBinary, since CreateBinary
    // always applies BinaryUploadService's fixed defaultLifetime_.
    const std::string payload0 = "AAAAA";
    const std::string payload1 = "BBBBB";
    const std::string uuid = store_.createSlot(payload0.size() + payload1.size(), 2, 3s);

    grpc::ClientContext uploadCtx;
    auto stream = uploadStub_->UploadChunk(&uploadCtx);

    bt::UploadChunkRequest chunk0;
    chunk0.set_binarytransferuuid(uuid);
    chunk0.set_chunkindex(0);
    chunk0.set_payload(payload0);
    ASSERT_TRUE(stream->Write(chunk0));
    bt::UploadChunkResponse resp0;
    ASSERT_TRUE(stream->Read(&resp0));

    std::this_thread::sleep_for(2s);
    // Simulates SilaServerBase's periodic GC sweep firing mid-upload. Both
    // the fixed and the buggy behavior are still under the original 3s
    // deadline here, so this is a sanity check, not yet a discriminator.
    EXPECT_EQ(store_.removeExpired(), 0u);

    bt::UploadChunkRequest chunk1;
    chunk1.set_binarytransferuuid(uuid);
    chunk1.set_chunkindex(1);
    chunk1.set_payload(payload1);
    ASSERT_TRUE(stream->Write(chunk1));
    bt::UploadChunkResponse resp1;
    ASSERT_TRUE(stream->Read(&resp1));
    stream->WritesDone();
    ASSERT_TRUE(stream->Finish().ok());

    std::this_thread::sleep_for(2s);
    // ~4s have now passed since createSlot(), past the original 3s lifetime.
    // Without a refresh on every storeChunk(), expiresAt would never have
    // moved past createSlot()-time + 3s and this slot would already be gone.
    // With the fix, chunk1's storeChunk() (at ~t=2s) renewed it to ~t=5s.
    EXPECT_EQ(store_.removeExpired(), 0u);
    ASSERT_TRUE(store_.contains(uuid));
    ASSERT_TRUE(store_.isComplete(uuid));
    const auto assembled = store_.assemble(uuid);
    EXPECT_EQ(std::string(assembled.begin(), assembled.end()), payload0 + payload1);
}

TEST_F(BinaryTransferService, ConcurrentLargeUploadsRemainComplete) {
    constexpr int kUploadCount = 8;
    constexpr std::size_t kPayloadSize = 256 * 1024;
    std::vector<std::string> uuids(kUploadCount);
    std::vector<grpc::Status> statuses(
        kUploadCount, grpc::Status(grpc::StatusCode::UNKNOWN, "upload did not finish"));
    std::vector<std::thread> uploads;

    for (int i = 0; i < kUploadCount; ++i) {
        uploads.emplace_back([&, i] {
            const std::string payload(kPayloadSize, static_cast<char>('a' + i));
            auto stub = bt::BinaryUpload::NewStub(channel_);
            bt::CreateBinaryRequest createRequest;
            createRequest.set_binarysize(payload.size());
            createRequest.set_chunkcount(1);
            createRequest.set_parameteridentifier("parallel");
            bt::CreateBinaryResponse createResponse;
            grpc::ClientContext createContext;
            statuses[i] = stub->CreateBinary(&createContext, createRequest, &createResponse);
            if (!statuses[i].ok()) return;

            grpc::ClientContext uploadContext;
            auto stream = stub->UploadChunk(&uploadContext);
            bt::UploadChunkRequest request;
            request.set_binarytransferuuid(createResponse.binarytransferuuid());
            request.set_chunkindex(0);
            request.set_payload(payload);
            if (!stream->Write(request)) {
                statuses[i] = grpc::Status(grpc::StatusCode::UNKNOWN, "chunk write failed");
                return;
            }
            bt::UploadChunkResponse response;
            if (!stream->Read(&response)) {
                statuses[i] = grpc::Status(grpc::StatusCode::UNKNOWN, "chunk response missing");
                return;
            }
            stream->WritesDone();
            statuses[i] = stream->Finish();
            if (statuses[i].ok()) uuids[i] = response.binarytransferuuid();
        });
    }
    for (auto& upload : uploads) upload.join();

    for (const auto& status : statuses) ASSERT_TRUE(status.ok()) << status.error_message();
    for (int i = 0; i < kUploadCount; ++i) {
        ASSERT_TRUE(store_.isComplete(uuids[i]));
        const auto assembled = store_.assemble(uuids[i]);
        EXPECT_EQ(std::string(assembled.begin(), assembled.end()),
                  std::string(kPayloadSize, static_cast<char>('a' + i)));
    }
}

TEST_F(BinaryTransferService, FullUploadThenGetBinaryInfoReturnsCorrectSize) {
    const std::string uuid = uploadWholeBinary("hello");

    bt::GetBinaryInfoRequest request;
    request.set_binarytransferuuid(uuid);
    bt::GetBinaryInfoResponse response;
    grpc::ClientContext ctx;
    grpc::Status status = downloadStub_->GetBinaryInfo(&ctx, request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(response.binarysize(), 5u);
}

TEST_F(BinaryTransferService, FullUploadThenGetChunkReturnsCorrectPayload) {
    const std::string uuid = uploadWholeBinary("hello");

    grpc::ClientContext ctx;
    auto stream = downloadStub_->GetChunk(&ctx);
    bt::GetChunkRequest request;
    request.set_binarytransferuuid(uuid);
    request.set_offset(0);
    request.set_length(5);
    ASSERT_TRUE(stream->Write(request));

    bt::GetChunkResponse response;
    ASSERT_TRUE(stream->Read(&response));
    stream->WritesDone();
    grpc::Status status = stream->Finish();

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(response.payload(), "hello");
}

TEST_F(BinaryTransferService, UploadChunkStreamClosedAfterAllChunksReturnsOk) {
    bt::CreateBinaryRequest createRequest;
    createRequest.set_binarysize(4);
    createRequest.set_chunkcount(2);
    createRequest.set_parameteridentifier("param");
    bt::CreateBinaryResponse createResponse;
    grpc::ClientContext createCtx;
    uploadStub_->CreateBinary(&createCtx, createRequest, &createResponse);
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;
    auto stream = uploadStub_->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunk0;
    chunk0.set_binarytransferuuid(uuid);
    chunk0.set_chunkindex(0);
    chunk0.set_payload("ab");
    ASSERT_TRUE(stream->Write(chunk0));
    bt::UploadChunkResponse resp0;
    ASSERT_TRUE(stream->Read(&resp0));

    bt::UploadChunkRequest chunk1;
    chunk1.set_binarytransferuuid(uuid);
    chunk1.set_chunkindex(1);
    chunk1.set_payload("cd");
    ASSERT_TRUE(stream->Write(chunk1));
    bt::UploadChunkResponse resp1;
    ASSERT_TRUE(stream->Read(&resp1));
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(store_.isComplete(uuid));
}

// Pins that the post-loop completeness guard walks only the UUIDs the stream
// actually touched, not every slot in the store -- a stream that writes
// nothing must not fail merely because some unrelated slot elsewhere is short.
TEST_F(BinaryTransferService, UploadChunkStreamWithNoChunksAtAllReturnsOk) {
    grpc::ClientContext uploadCtx;
    auto stream = uploadStub_->UploadChunk(&uploadCtx);
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST_F(BinaryTransferService, FullUploadThenDeleteRemovesSlot) {
    const std::string uuid = uploadWholeBinary("hello");
    ASSERT_TRUE(store_.contains(uuid));

    bt::DeleteBinaryRequest request;
    request.set_binarytransferuuid(uuid);
    bt::DeleteBinaryResponse response;
    grpc::ClientContext ctx;
    grpc::Status status = uploadStub_->DeleteBinary(&ctx, request, &response);

    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(store_.contains(uuid));
}

TEST_F(BinaryTransferService, GetChunkAtExactTailReturnsRemainingBytes) {
    const std::string uuid = uploadWholeBinary("hello");

    grpc::ClientContext ctx;
    auto stream = downloadStub_->GetChunk(&ctx);
    bt::GetChunkRequest request;
    request.set_binarytransferuuid(uuid);
    request.set_offset(3);
    request.set_length(2);
    ASSERT_TRUE(stream->Write(request));

    bt::GetChunkResponse response;
    ASSERT_TRUE(stream->Read(&response));
    stream->WritesDone();
    grpc::Status status = stream->Finish();

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(response.payload(), "lo");
}

TEST(BinaryTransferServerBaseE2E, LargeAndConcurrentClientRoundTrips) {
    auto server = sila2::SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withDiscovery(0)
                      .withBinaryTransfer()
                      .build();
    server.run(false);

    auto dial = [&server] {
        grpc::SslCredentialsOptions options;
        options.pem_root_certs = server.certificatePem();
        return grpc::CreateChannel("localhost:" + std::to_string(server.port()),
                                   grpc::SslCredentials(options));
    };
    auto pattern = [](std::size_t size, unsigned char salt) {
        std::string data(size, '\0');
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<char>((i * 31 + salt) & 0xff);
        }
        return data;
    };

    // S13: a real SilaServerBase always registerFeature()s SiLAService/v1, so
    // its FQI gate (BinaryUploadService.cc) is active here — a bare
    // "org.test/Large" placeholder (no registered Feature, no /Parameter/
    // segment) is now rejected. The Synthetic* Command/Parameter segments
    // below are deliberately fake: the gate checks Feature coverage plus a
    // /Parameter/ segment, not item existence, and a made-up name cannot be
    // mistaken for a real SiLAService parameter. This test exercises
    // large/concurrent transfer, not gate rejection (that's
    // BinaryTransferServiceFqiGate below).
    const std::string large = pattern(6 * 1024 * 1024, 17);
    sila2::BinaryUploader uploader{dial()};
    const std::string uuid = uploader.upload(
        "org.silastandard/core/SiLAService/v1/Command/SyntheticLargeUpload/Parameter/Payload", large);
    sila2::BinaryDownloader downloader{dial()};
    EXPECT_EQ(downloader.download(uuid), large);

    constexpr int kClients = 3;
    std::vector<std::string> payloads;
    std::vector<std::string> results(kClients);
    std::vector<std::string> errors(kClients);
    payloads.reserve(kClients);
    for (int i = 0; i < kClients; ++i) payloads.push_back(pattern(5 * 1024 * 1024, i));

    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([&, i] {
            try {
                sila2::BinaryUploader parallelUploader{dial()};
                const auto parallelUuid = parallelUploader.upload(
                    "org.silastandard/core/SiLAService/v1/Command/SyntheticConcurrentUpload/Parameter/Payload",
                    payloads[i]);
                sila2::BinaryDownloader parallelDownloader{dial()};
                results[i] = parallelDownloader.download(parallelUuid);
            } catch (const std::exception& e) {
                errors[i] = e.what();
            }
        });
    }
    for (auto& client : clients) client.join();

    for (int i = 0; i < kClients; ++i) {
        EXPECT_TRUE(errors[i].empty()) << errors[i];
        EXPECT_EQ(results[i], payloads[i]);
    }
    server.shutdown();
}

// Part B p56: exactly kMaxBinaryChunkSize is still a legal chunk -- the
// ceiling is an upper bound ("MUST not be larger than"), not an exclusive one.
TEST_F(BinaryTransferService, UploadChunkAcceptsChunkAtCeiling) {
    const std::string payload(sila2::binary::kMaxBinaryChunkSize, 'x');
    const std::string uuid = store_.createSlot(payload.size(), 1, 300s);

    grpc::ClientContext ctx;
    auto stream = uploadStub_->UploadChunk(&ctx);
    bt::UploadChunkRequest request;
    request.set_binarytransferuuid(uuid);
    request.set_chunkindex(0);
    request.set_payload(payload);
    ASSERT_TRUE(stream->Write(request));
    bt::UploadChunkResponse response;
    ASSERT_TRUE(stream->Read(&response));
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(store_.isComplete(uuid));
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT (store_.contains() guards
// every RPC before it touches BinaryStore, returning ABORTED +
// INVALID_BINARY_TRANSFER_UUID via BinaryUtil::makeBinaryTransferStatus())
// ---------------------------------------------------------------------------

TEST_F(BinaryTransferService, UploadChunkWithUnknownUuidReturnsAborted) {
    grpc::ClientContext ctx;
    auto stream = uploadStub_->UploadChunk(&ctx);
    bt::UploadChunkRequest request;
    request.set_binarytransferuuid("not-a-known-uuid");
    request.set_chunkindex(0);
    request.set_payload("x");
    stream->Write(request);
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

// S52 / Part B p65: the gRPC status *message* MUST carry the serialized
// BinaryTransferError Base64-encoded, not plaintext -- error_details keeps
// the raw serialized bytes so both fields must decode to the same proto.
TEST_F(BinaryTransferService, UploadChunkUnknownUuidStatusMessageIsBase64OfDetails) {
    grpc::ClientContext ctx;
    auto stream = uploadStub_->UploadChunk(&ctx);
    bt::UploadChunkRequest request;
    request.set_binarytransferuuid("not-a-known-uuid");
    request.set_chunkindex(0);
    request.set_payload("x");
    stream->Write(request);
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    EXPECT_EQ(status.error_message(), sila2::base64Encode(status.error_details()));
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

// S11: a stream that WritesDone/Finish's before all chunkCount chunks arrive
// must fail with BINARY_UPLOAD_FAILED instead of OK -- the uploader is the
// only party that can retry, and it must be told.
TEST_F(BinaryTransferService, UploadChunkStreamClosedShortReturnsBinaryUploadFailed) {
    bt::CreateBinaryRequest createRequest;
    createRequest.set_binarysize(4);
    createRequest.set_chunkcount(2);
    createRequest.set_parameteridentifier("param");
    bt::CreateBinaryResponse createResponse;
    grpc::ClientContext createCtx;
    uploadStub_->CreateBinary(&createCtx, createRequest, &createResponse);
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;
    auto stream = uploadStub_->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunk0;
    chunk0.set_binarytransferuuid(uuid);
    chunk0.set_chunkindex(0);
    chunk0.set_payload("ab");
    ASSERT_TRUE(stream->Write(chunk0));
    bt::UploadChunkResponse resp0;
    ASSERT_TRUE(stream->Read(&resp0));
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    EXPECT_FALSE(status.ok());
    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
    EXPECT_NE(error.message().find(uuid), std::string::npos);
}

// REJECTION: the BINARY_UPLOAD_FAILED path also goes through
// makeBinaryTransferStatus, so its message must be Base64 too, not the raw
// proto bytes -- pins that the encoding applies to every ErrorType, not just
// INVALID_BINARY_TRANSFER_UUID.
TEST_F(BinaryTransferService, UploadChunkStreamClosedShortStatusMessageIsBase64OfDetails) {
    bt::CreateBinaryRequest createRequest;
    createRequest.set_binarysize(4);
    createRequest.set_chunkcount(2);
    createRequest.set_parameteridentifier("param");
    bt::CreateBinaryResponse createResponse;
    grpc::ClientContext createCtx;
    uploadStub_->CreateBinary(&createCtx, createRequest, &createResponse);
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;
    auto stream = uploadStub_->UploadChunk(&uploadCtx);
    bt::UploadChunkRequest chunk0;
    chunk0.set_binarytransferuuid(uuid);
    chunk0.set_chunkindex(0);
    chunk0.set_payload("ab");
    ASSERT_TRUE(stream->Write(chunk0));
    bt::UploadChunkResponse resp0;
    ASSERT_TRUE(stream->Read(&resp0));
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    EXPECT_EQ(status.error_message(), sila2::base64Encode(status.error_details()));
    // The message is the encoded form, not the raw serialized proto itself.
    EXPECT_NE(status.error_message(), status.error_details());
}

TEST_F(BinaryTransferService, GetBinaryInfoWithUnknownUuidReturnsAborted) {
    bt::GetBinaryInfoRequest request;
    request.set_binarytransferuuid("not-a-known-uuid");
    bt::GetBinaryInfoResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = downloadStub_->GetBinaryInfo(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

TEST_F(BinaryTransferService, GetChunkWithUnknownUuidReturnsAborted) {
    grpc::ClientContext ctx;
    auto stream = downloadStub_->GetChunk(&ctx);
    bt::GetChunkRequest request;
    request.set_binarytransferuuid("not-a-known-uuid");
    request.set_offset(0);
    request.set_length(1);
    stream->Write(request);
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

TEST_F(BinaryTransferService, GetChunkPastEndReturnsDownloadFailed) {
    const std::string uuid = uploadWholeBinary("hello");

    grpc::ClientContext ctx;
    auto stream = downloadStub_->GetChunk(&ctx);
    bt::GetChunkRequest request;
    request.set_binarytransferuuid(uuid);
    request.set_offset(3);
    request.set_length(10);
    stream->Write(request);
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    // InMemoryBinaryStore::readRange now rejects a range past binarySize
    // instead of silently truncating it, since GetChunkResponse has no
    // length field a client could use to detect a short payload.
    ASSERT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
}

// Part B p56: a chunk one byte over the 2 MiB ceiling MUST be rejected.
TEST_F(BinaryTransferService, UploadChunkRejectsChunkAboveCeiling) {
    const std::string payload(sila2::binary::kMaxBinaryChunkSize + 1, 'x');
    const std::string uuid = store_.createSlot(payload.size(), 1, 300s);

    grpc::ClientContext ctx;
    auto stream = uploadStub_->UploadChunk(&ctx);
    bt::UploadChunkRequest request;
    request.set_binarytransferuuid(uuid);
    request.set_chunkindex(0);
    request.set_payload(payload);
    stream->Write(request);
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

// Part B p56: the ceiling binds a requested GetChunk length too, not just an
// UploadChunk payload -- a length above 2 MiB would ask the store for a
// non-conformant chunk.
TEST_F(BinaryTransferService, GetChunkRejectsLengthAboveCeiling) {
    const std::string uuid = uploadWholeBinary("hello");

    grpc::ClientContext ctx;
    auto stream = downloadStub_->GetChunk(&ctx);
    bt::GetChunkRequest request;
    request.set_binarytransferuuid(uuid);
    request.set_offset(0);
    request.set_length(sila2::binary::kMaxBinaryChunkSize + 1);
    stream->Write(request);
    stream->WritesDone();

    grpc::Status status = stream->Finish();

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
}

TEST_F(BinaryTransferService, DeleteBinaryUploadWithUnknownUuidReturnsAborted) {
    bt::DeleteBinaryRequest request;
    request.set_binarytransferuuid("not-a-known-uuid");
    bt::DeleteBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->DeleteBinary(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

TEST_F(BinaryTransferService, CreateBinaryRejectsChunkCountExceedingBinarySize) {
    // chunkCount > binarySize: no chunk can carry less than one byte.
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(6);
    request.set_parameteridentifier("param");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

TEST_F(BinaryTransferService, CreateBinaryRejectsChunkCountPastAbsoluteCeiling) {
    // Past InMemoryBinaryStore::kMaxChunkCount even though the
    // binarySize-proportional bound alone would still allow it.
    bt::CreateBinaryRequest request;
    request.set_binarysize(InMemoryBinaryStore::kMaxChunkCount + 1);
    request.set_chunkcount(InMemoryBinaryStore::kMaxChunkCount + 1);
    request.set_parameteridentifier("param");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

// ---------------------------------------------------------------------------
// S13: CreateBinary's parameterIdentifier-vs-registered-Feature gate. Only
// turned on when chain->registeredFeatureFqis is non-empty, which is why
// every fixture above (chain == nullptr) needed no changes -- that absence
// is itself the proof the gate is opt-in.
// ---------------------------------------------------------------------------

class BinaryTransferServiceFqiGate : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&uploadService_);
        server_ = builder.BuildAndStart();
        if (!server_) GTEST_SKIP() << "local gRPC listener unavailable";
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        uploadStub_ = bt::BinaryUpload::NewStub(channel_);
    }

    void TearDown() override {
        if (server_) server_->Shutdown();
    }

    InMemoryBinaryStore store_;
    // A non-empty registeredFeatureFqis is what turns the S13 gate on; no
    // auth interceptor is needed to exercise it.
    sila2::InterceptorChain chain_{
        .registeredFeatureFqis = {"org.silastandard/core/SiLAService/v1", "org.test/MyFeature/v1"}};
    BinaryUploadService uploadService_{store_, 300s, &chain_};
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<bt::BinaryUpload::Stub> uploadStub_;
};

TEST_F(BinaryTransferServiceFqiGate, CreateBinaryAcceptsRegisteredCommandParameterFqi) {
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(1);
    request.set_parameteridentifier("org.test/MyFeature/v1/Command/DoThing/Parameter/Payload");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.binarytransferuuid().empty());
}

TEST_F(BinaryTransferServiceFqiGate, CreateBinaryAcceptsRegisteredMetadataFqi) {
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(1);
    request.set_parameteridentifier("org.test/MyFeature/v1/Metadata/Tag");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_TRUE(status.ok()) << status.error_message();
}

TEST_F(BinaryTransferServiceFqiGate, CreateBinaryRejectsNonFqiParameterIdentifier) {
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(1);
    request.set_parameteridentifier("param");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

TEST_F(BinaryTransferServiceFqiGate, CreateBinaryRejectsParameterFqiOfUnregisteredFeature) {
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(1);
    request.set_parameteridentifier("org.test/OtherFeature/v1/Command/DoThing/Parameter/Payload");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

TEST_F(BinaryTransferServiceFqiGate, CreateBinaryRejectsBareFeatureFqi) {
    // No Command/Parameter or Metadata segment -- a Feature FQI names the
    // Feature, not one of its items.
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(1);
    request.set_parameteridentifier("org.test/MyFeature/v1");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

TEST_F(BinaryTransferServiceFqiGate, CreateBinaryRejectsNeighbouringVersionFqi) {
    // Pins that the gate reuses fqiCovers' segment-boundary rule: v10 is a
    // different Feature version from the registered v1, not a suffix match.
    bt::CreateBinaryRequest request;
    request.set_binarysize(5);
    request.set_chunkcount(1);
    request.set_parameteridentifier("org.test/MyFeature/v10/Command/DoThing/Parameter/Payload");
    bt::CreateBinaryResponse response;
    grpc::ClientContext ctx;

    grpc::Status status = uploadStub_->CreateBinary(&ctx, request, &response);

    ASSERT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
    bt::BinaryTransferError error;
    ASSERT_TRUE(error.ParseFromString(status.error_details()));
    EXPECT_EQ(error.errortype(), bt::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

}  // namespace
