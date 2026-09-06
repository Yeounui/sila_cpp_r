// Tests for BinaryDownloader::download(): the full GetBinaryInfo -> GetChunk
// -> assembled-result pipeline (architecture.md §4.6), including the retry
// and fatal-error paths that a plain unit test on the real server can't
// trigger on demand.
//
// True paths run against the REAL BinaryDownloadService + InMemoryBinaryStore
// (same pair test_binary_transfer_service.cc exercises) wherever that's
// enough to drive the scenario, so the wire encoding is genuinely exercised.
// The retry/fatal-error paths need a server that can misbehave on command
// (drop a chunk, return a scripted BinaryTransferError, ...), which the real
// service intentionally can't do — those use MockBinaryDownloadService, a
// thin grpc::Service stand-in that lets each test script GetBinaryInfo/
// GetChunk behavior per call.
#include <sila/client/binary/BinaryDownloader.h>
#include <sila/client/MetadataInjector.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/binary/BinaryDownloadService.h>
#include <sila/server/binary/BinaryUploadService.h>
#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/server/transport/InterceptorChain.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <SiLABinaryTransfer.grpc.pb.h>
#include "AuthorizationService.pb.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace bt = sila2::org::silastandard;
using namespace std::chrono_literals;

// Starts an in-process server hosting `services` on an ephemeral port and
// returns it together with a channel to it. Shared by every test below to
// avoid repeating the ServerBuilder/CreateChannel boilerplate.
std::pair<std::unique_ptr<grpc::Server>, std::shared_ptr<grpc::Channel>>
startServer(std::vector<grpc::Service*> services) {
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    for (auto* service : services) builder.RegisterService(service);
    auto server = builder.BuildAndStart();
    auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    return {std::move(server), channel};
}

// Uploads `data` as a single chunk through the real BinaryUploadService and
// returns the generated UUID, so download tests have a real slot to read.
std::string uploadWholeBinary(std::shared_ptr<grpc::Channel> channel, const std::string& data) {
    auto stub = bt::BinaryUpload::NewStub(channel);

    bt::CreateBinaryRequest createRequest;
    createRequest.set_binarysize(data.size());
    createRequest.set_chunkcount(1);
    createRequest.set_parameteridentifier("param");
    bt::CreateBinaryResponse createResponse;
    grpc::ClientContext createCtx;
    stub->CreateBinary(&createCtx, createRequest, &createResponse);
    const std::string uuid = createResponse.binarytransferuuid();

    grpc::ClientContext uploadCtx;
    auto stream = stub->UploadChunk(&uploadCtx);
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

using sila2::InterceptorChain;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;

const std::string kDownloadFqi{sila2::kBinaryDownloadFqi};

// Same wire shape SilaClientBase::authenticate() injects (SilaClientBase.cc:32-36):
// the header carries a serialized Metadata_AccessToken, not the bare token, and
// MetadataExtractingInterceptor.cc:38-42 parses it back out before the gate reads it.
// Duplicated from test_binary_uploader_e2e.cc deliberately: the two files
// already duplicate startServer(), and hoisting a shared header for six lines
// would be a new file for one caller each.
std::string serializeAccessToken(const std::string& token) {
    sila2::org::silastandard::core::authorizationservice::v1::Metadata_AccessToken metadata;
    metadata.mutable_accesstoken()->set_value(token);
    return metadata.SerializeAsString();
}

// Scriptable stand-in for BinaryDownload::Service: each test supplies
// GetBinaryInfo/GetChunk behavior as a callback instead of standing up a
// full BinaryStore. getChunkCalls counts RPCs (one per BinaryDownloader
// attempt), so tests can assert whether a retry actually happened.
class MockBinaryDownloadService : public bt::BinaryDownload::Service {
public:
    std::function<grpc::Status(const bt::GetBinaryInfoRequest&, bt::GetBinaryInfoResponse*)> onGetBinaryInfo;
    std::function<grpc::Status(int callIndex, grpc::ServerReaderWriter<bt::GetChunkResponse, bt::GetChunkRequest>*)>
        onGetChunk;
    int getChunkCalls = 0;

    grpc::Status GetBinaryInfo(grpc::ServerContext*, const bt::GetBinaryInfoRequest* request,
                               bt::GetBinaryInfoResponse* response) override {
        return onGetBinaryInfo(*request, response);
    }

    grpc::Status GetChunk(grpc::ServerContext*,
                          grpc::ServerReaderWriter<bt::GetChunkResponse, bt::GetChunkRequest>* stream) override {
        return onGetChunk(getChunkCalls++, stream);
    }
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(BinaryDownloaderE2E, SingleChunkDownloadAgainstRealServer) {
    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s};
    sila2::BinaryDownloadService downloadService{store, 300s};
    auto [server, channel] = startServer({&uploadService, &downloadService});
    const std::string uuid = uploadWholeBinary(channel, "hello");

    sila2::BinaryDownloader downloader{channel};
    std::string result = downloader.download(uuid);

    EXPECT_EQ(result, "hello");
    server->Shutdown();
}

// A compliant server may answer GetChunk with fewer bytes than requested
// (the wire protocol never promises otherwise); download()'s inner
// while(offset < binarySize) loop is what keeps re-requesting until the
// whole binary has arrived. This exercises that loop running more than once
// within a single attempt, distinct from the single-round-trip case above.
TEST(BinaryDownloaderE2E, MultiRoundTripDownloadWithinOneAttempt) {
    const std::string data = "abcdef";
    MockBinaryDownloadService service;
    service.onGetBinaryInfo = [&](const bt::GetBinaryInfoRequest&, bt::GetBinaryInfoResponse* response) {
        response->set_binarysize(data.size());
        return grpc::Status::OK;
    };
    service.onGetChunk = [&](int, grpc::ServerReaderWriter<bt::GetChunkResponse, bt::GetChunkRequest>* stream) {
        bt::GetChunkRequest request;
        while (stream->Read(&request)) {
            std::size_t offset = request.offset();
            std::size_t sliceLen = std::min<std::size_t>(2, data.size() - offset);  // always under-serve
            bt::GetChunkResponse response;
            response.set_binarytransferuuid(request.binarytransferuuid());
            response.set_offset(offset);
            response.set_payload(data.substr(offset, sliceLen));
            stream->Write(response);
        }
        return grpc::Status::OK;
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryDownloader downloader{channel};
    std::string result = downloader.download("any-uuid");

    EXPECT_EQ(result, data);
    EXPECT_EQ(service.getChunkCalls, 1);  // all three round trips fit in one attempt
    server->Shutdown();
}

// The first attempt's stream is dropped after delivering a partial chunk;
// the second attempt must resume from the partial offset (not restart from
// zero) and produce the byte-exact original content.
TEST(BinaryDownloaderE2E, TransientStreamBreakThenSuccessfulResume) {
    const std::string data = "abcdef";
    MockBinaryDownloadService service;
    service.onGetBinaryInfo = [&](const bt::GetBinaryInfoRequest&, bt::GetBinaryInfoResponse* response) {
        response->set_binarysize(data.size());
        return grpc::Status::OK;
    };
    service.onGetChunk = [&](int callIndex, grpc::ServerReaderWriter<bt::GetChunkResponse, bt::GetChunkRequest>* stream) {
        bt::GetChunkRequest request;
        if (callIndex == 0) {
            if (!stream->Read(&request)) return grpc::Status::OK;
            bt::GetChunkResponse response;
            response.set_binarytransferuuid(request.binarytransferuuid());
            response.set_offset(request.offset());
            response.set_payload(data.substr(request.offset(), 3));  // "abc"
            stream->Write(response);
            stream->Read(&request);  // absorb the client's next request, then drop it
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "simulated transient drop");
        }
        // Second attempt: serve everything from the resumed offset onward.
        while (stream->Read(&request)) {
            bt::GetChunkResponse response;
            response.set_binarytransferuuid(request.binarytransferuuid());
            response.set_offset(request.offset());
            response.set_payload(data.substr(request.offset(), data.size() - request.offset()));
            stream->Write(response);
        }
        return grpc::Status::OK;
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryDownloader downloader{channel, /*maxRetries=*/3, /*maxBackoff=*/0s};
    std::string result = downloader.download("any-uuid");

    EXPECT_EQ(result, data);
    EXPECT_EQ(service.getChunkCalls, 2);
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT
// ---------------------------------------------------------------------------

TEST(BinaryDownloaderE2E, GetBinaryInfoFailureThrowsRuntimeError) {
    sila2::InMemoryBinaryStore store;
    sila2::BinaryDownloadService downloadService{store, 300s};
    auto [server, channel] = startServer({&downloadService});

    sila2::BinaryDownloader downloader{channel};

    EXPECT_THROW(downloader.download("not-a-known-uuid"), std::runtime_error);
    server->Shutdown();
}

TEST(BinaryDownloaderE2E, InvalidUuidDuringGetChunkIsFatalWithNoRetry) {
    MockBinaryDownloadService service;
    service.onGetBinaryInfo = [](const bt::GetBinaryInfoRequest&, bt::GetBinaryInfoResponse* response) {
        response->set_binarysize(5);
        return grpc::Status::OK;
    };
    service.onGetChunk = [](int, grpc::ServerReaderWriter<bt::GetChunkResponse, bt::GetChunkRequest>*) {
        bt::BinaryTransferError error;
        error.set_errortype(bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
        error.set_message("uuid unknown");
        return grpc::Status(grpc::StatusCode::ABORTED, "uuid unknown", error.SerializeAsString());
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryDownloader downloader{channel, /*maxRetries=*/3, /*maxBackoff=*/0s};

    EXPECT_THROW(downloader.download("any-uuid"), std::runtime_error);
    EXPECT_EQ(service.getChunkCalls, 1);  // fatal error: no retry attempted
    server->Shutdown();
}

TEST(BinaryDownloaderE2E, BinaryDownloadFailedDuringGetChunkIsFatalWithNoRetry) {
    MockBinaryDownloadService service;
    service.onGetBinaryInfo = [](const bt::GetBinaryInfoRequest&, bt::GetBinaryInfoResponse* response) {
        response->set_binarysize(5);
        return grpc::Status::OK;
    };
    service.onGetChunk = [](int, grpc::ServerReaderWriter<bt::GetChunkResponse, bt::GetChunkRequest>*) {
        bt::BinaryTransferError error;
        error.set_errortype(bt::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
        error.set_message("assembly failed");
        return grpc::Status(grpc::StatusCode::ABORTED, "assembly failed", error.SerializeAsString());
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryDownloader downloader{channel, /*maxRetries=*/3, /*maxBackoff=*/0s};

    EXPECT_THROW(downloader.download("any-uuid"), std::runtime_error);
    EXPECT_EQ(service.getChunkCalls, 1);  // fatal error: no retry attempted
    server->Shutdown();
}

TEST(BinaryDownloaderE2E, RetriesExhaustedOnPersistentTransientFailureThrows) {
    MockBinaryDownloadService service;
    service.onGetBinaryInfo = [](const bt::GetBinaryInfoRequest&, bt::GetBinaryInfoResponse* response) {
        response->set_binarysize(5);
        return grpc::Status::OK;
    };
    service.onGetChunk = [](int, grpc::ServerReaderWriter<bt::GetChunkResponse, bt::GetChunkRequest>* stream) {
        bt::GetChunkRequest request;
        stream->Read(&request);  // absorb the request, then drop without answering
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "always transient");
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryDownloader downloader{channel, /*maxRetries=*/2, /*maxBackoff=*/0s};

    EXPECT_THROW(downloader.download("any-uuid"), std::runtime_error);
    EXPECT_EQ(service.getChunkCalls, 3);  // 1 initial attempt + 2 retries
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// S26: MetadataInjector plumbing (auth token on every ClientContext)
// ---------------------------------------------------------------------------

// Covers BOTH downloader ClientContext sites (GetBinaryInfo and GetChunk) in
// one flow -- BinaryDownloadService gates all three of its RPCs on
// kBinaryDownloadFqi, so a successful download necessarily crossed the gate
// twice (info lookup, then the chunk stream).
TEST(BinaryDownloaderE2E, InjectedTokenPassesGetBinaryInfoAndGetChunkGates) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, [](const std::string& fqi) {
        return fqi == kDownloadFqi;
    }};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::InMemoryBinaryStore store;
    // Upload side deliberately UNGATED: uploadWholeBinary() below seeds the
    // store with bare ClientContexts, and gating it would test the uploader.
    sila2::BinaryUploadService uploadService{store, 300s};
    sila2::BinaryDownloadService downloadService{store, 300s, &chain};
    auto [server, channel] = startServer({&uploadService, &downloadService});
    const std::string uuid = uploadWholeBinary(channel, "hello");

    sila2::MetadataInjector injector;
    injector.set(sila2::kAccessTokenMetadataFqi,
                 serializeAccessToken(tokenStore.issue("alice", {kDownloadFqi}, 60s)));

    sila2::BinaryDownloader downloader{channel, 3, 0s, &injector};

    EXPECT_EQ(downloader.download(uuid), "hello");
    server->Shutdown();
}

// Rejection at the GetBinaryInfo gate (BinaryDownloader.cc's first
// ClientContext site): no injector, so the request never reaches the retry
// loop -- download() throws before even entering it.
//
// NOT ADDED: a separate "GetChunk rejected but GetBinaryInfo accepted" case.
// Both RPCs gate on the identical FQI (kBinaryDownloadFqi), so no token can
// pass one and fail the other; the positive test above already proves the
// second site carries the token.
TEST(BinaryDownloaderE2E, MissingTokenRejectedAtGetBinaryInfoGate) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, [](const std::string& fqi) {
        return fqi == kDownloadFqi;
    }};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s};
    sila2::BinaryDownloadService downloadService{store, 300s, &chain};
    auto [server, channel] = startServer({&uploadService, &downloadService});
    const std::string uuid = uploadWholeBinary(channel, "hello");

    sila2::BinaryDownloader downloader{channel, 0, 0s};

    try {
        (void)downloader.download(uuid);
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find("GetBinaryInfo"), std::string::npos);
    }
    server->Shutdown();
}

}  // namespace
