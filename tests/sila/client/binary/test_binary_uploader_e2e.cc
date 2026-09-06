// Tests for BinaryUploader::upload(): the full CreateBinary -> UploadChunk
// stream -> UUID pipeline (architecture.md §4.6), including the resume,
// restart, and retry-exhaustion paths a plain unit test on the real server
// can't trigger on demand.
//
// True paths run against the REAL BinaryUploadService + InMemoryBinaryStore
// (same pair test_binary_transfer_service.cc exercises) wherever that's
// enough to drive the scenario. The resume/restart/retry paths need a
// server that can misbehave on command (drop an ack, return a scripted
// BinaryTransferError, ...), which the real service intentionally can't do
// -- those use MockBinaryUploadService, a thin grpc::Service stand-in that
// lets each test script CreateBinary/UploadChunk behavior per call.
//
// NOT COVERED: a stream Write() failure (as opposed to a Read() failure).
// grpc's sync ClientReaderWriter::Write() only returns false once the call
// has already failed at the transport level; there's no deterministic way
// to force that from a scripted in-process service without racing the
// connection teardown, which would make the test flaky. uploadChunks()
// handles a Write() failure with the exact same UploadStreamError path
// exercised below for Read() failures (see BinaryUploader.cc), so this gap
// is a coverage gap in this test file, not an unexercised code path.
#include <sila/client/binary/BinaryUploader.h>
#include <sila/client/MetadataInjector.h>
#include <sila/common/binary/BinaryChunkLimit.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/binary/BinaryUploadService.h>
#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/server/transport/InterceptorChain.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <SiLABinaryTransfer.grpc.pb.h>
#include "AuthorizationService.pb.h"

#include <chrono>
#include <functional>
#include <map>
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

using sila2::InterceptorChain;
using sila2::auth::AuthorizationInterceptor;
using sila2::auth::AuthTokenStore;

// CreateBinary authorizes on the request's parameterIdentifier while
// UploadChunk/DeleteBinary authorize on the feature FQI (BinaryUploadService.h
// kBinaryUploadFqi), so a gated upload crosses two differently-keyed gates.
const std::string kGatedParam = "param";
const std::string kUploadFqi{sila2::kBinaryUploadFqi};

// Same wire shape SilaClientBase::authenticate() injects (SilaClientBase.cc:32-36):
// the header carries a serialized Metadata_AccessToken, not the bare token, and
// MetadataExtractingInterceptor.cc:38-42 parses it back out before the gate reads it.
std::string serializeAccessToken(const std::string& token) {
    sila2::org::silastandard::core::authorizationservice::v1::Metadata_AccessToken metadata;
    metadata.mutable_accesstoken()->set_value(token);
    return metadata.SerializeAsString();
}

// Placed ahead of MockBinaryUploadService (rather than after ackAllChunks() as
// the design draft has it) because UploadChunk's body calls this by name --
// C++'s complete-class deferred lookup covers the class's own members, not a
// free function declared later in the same namespace.
bool hasAccessTokenHeader(const std::multimap<grpc::string_ref, grpc::string_ref>& headers) {
    const std::string key = sila2::metadataHeaderKey(sila2::kAccessTokenMetadataFqi);
    for (const auto& [name, value] : headers) {
        if (std::string{name.data(), name.length()} == key) return true;
    }
    return false;
}

// Scriptable stand-in for BinaryUpload::Service: each test supplies
// CreateBinary/UploadChunk behavior as a callback instead of standing up a
// full BinaryStore. The call counters let tests assert whether a restart
// (fresh CreateBinary) or a resume (same UUID, new stream) actually
// happened.
class MockBinaryUploadService : public bt::BinaryUpload::Service {
public:
    std::function<grpc::Status(const bt::CreateBinaryRequest&, bt::CreateBinaryResponse*)> onCreateBinary;
    std::function<grpc::Status(int callIndex, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>*)>
        onUploadChunk;
    int createBinaryCalls = 0;
    int uploadChunkCalls = 0;

    // One entry per UploadChunk stream, recording whether the client attached
    // the access-token header. Lets a test assert the token is re-attached on
    // a RESUMED stream, not just on the first attempt.
    std::vector<bool> uploadChunkHadToken;

    grpc::Status CreateBinary(grpc::ServerContext*, const bt::CreateBinaryRequest* request,
                              bt::CreateBinaryResponse* response) override {
        ++createBinaryCalls;
        return onCreateBinary(*request, response);
    }

    grpc::Status UploadChunk(grpc::ServerContext* context,
                             grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) override {
        uploadChunkHadToken.push_back(hasAccessTokenHeader(context->client_metadata()));
        return onUploadChunk(uploadChunkCalls++, stream);
    }
};

// Acks whatever chunk request it reads, echoing UUID and chunk index back
// (the two fields BinaryUploader actually inspects). Shared by the mock
// handlers below wherever a chunk should just succeed.
grpc::Status ackAllChunks(grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
    bt::UploadChunkRequest request;
    while (stream->Read(&request)) {
        bt::UploadChunkResponse response;
        response.set_binarytransferuuid(request.binarytransferuuid());
        response.set_chunkindex(request.chunkindex());
        stream->Write(response);
    }
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(BinaryUploaderE2E, SingleChunkUploadAgainstRealServer) {
    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s};
    auto [server, channel] = startServer({&uploadService});

    sila2::BinaryUploader uploader{channel};
    std::string uuid = uploader.upload("param", "hello");

    EXPECT_FALSE(uuid.empty());
    ASSERT_TRUE(store.isComplete(uuid));
    auto assembled = store.assemble(uuid);
    EXPECT_EQ(std::string(assembled.begin(), assembled.end()), "hello");
    server->Shutdown();
}

// A small chunkSize forces multiple UploadChunk requests within the same
// stream, exercising uploadChunks()'s per-index send loop and the server's
// chunk-count math together.
TEST(BinaryUploaderE2E, MultiChunkUploadAgainstRealServer) {
    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s};
    auto [server, channel] = startServer({&uploadService});

    sila2::BinaryUploader uploader{channel};
    std::string uuid = uploader.upload("param", "abcdefg", /*chunkSize=*/3);  // -> 3 chunks

    ASSERT_TRUE(store.isComplete(uuid));
    auto assembled = store.assemble(uuid);
    EXPECT_EQ(std::string(assembled.begin(), assembled.end()), "abcdefg");
    server->Shutdown();
}

// A transient (non-UUID) failure on chunk 1's ack breaks the first stream;
// upload() must resume on the SAME UUID via a fresh stream, resending only
// the unacked chunks (ackedIndices_ is preserved, no restart).
TEST(BinaryUploaderE2E, TransientChunkFailureThenResumeWithoutRestart) {
    const std::string data = "abcdef";  // 3 chunks of 2 bytes with chunkSize=2
    MockBinaryUploadService service;
    service.onCreateBinary = [](const bt::CreateBinaryRequest&, bt::CreateBinaryResponse* response) {
        response->set_binarytransferuuid("fixed-uuid");
        return grpc::Status::OK;
    };
    service.onUploadChunk = [](int callIndex, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
        if (callIndex == 0) {
            bt::UploadChunkRequest request;
            if (!stream->Read(&request)) return grpc::Status::OK;
            bt::UploadChunkResponse response;  // ack chunk 0
            response.set_binarytransferuuid(request.binarytransferuuid());
            response.set_chunkindex(request.chunkindex());
            stream->Write(response);
            stream->Read(&request);  // absorb chunk 1's write, then drop it
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "simulated transient drop");
        }
        return ackAllChunks(stream);  // resumed stream: ack whatever's left (chunks 1, 2)
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryUploader uploader{channel, /*maxRetries=*/3, /*maxBackoff=*/0s};
    std::string uuid = uploader.upload("param", data, /*chunkSize=*/2);

    EXPECT_EQ(uuid, "fixed-uuid");
    EXPECT_EQ(service.createBinaryCalls, 1);  // no restart: same UUID throughout
    EXPECT_EQ(service.uploadChunkCalls, 2);
    server->Shutdown();
}

// INVALID_BINARY_TRANSFER_UUID on chunk 1 forces a full restart: a fresh
// CreateBinary call and a cleared ack set, not just a resumed stream.
//
// The invalid-UUID special case only lives in the retry for-loop's catch
// (BinaryUploader.cc lines 58-66); upload()'s very first uploadChunks() call
// (before that loop) is wrapped in a plain catch(std::exception&) that
// always just falls through to an ordinary retry, regardless of error type
// (BinaryUploader.cc lines 46-51). So the first uploadChunks() failure can
// never trigger a restart -- only the second failure onward can. This test
// scripts a transient failure on the very first attempt (ordinary retry,
// same UUID) and puts the INVALID_BINARY_TRANSFER_UUID rejection on the
// second attempt, which is the first one actually able to restart.
TEST(BinaryUploaderE2E, InvalidUuidDuringUploadTriggersFullRestart) {
    const std::string data = "abcdef";  // 3 chunks of 2 bytes with chunkSize=2
    MockBinaryUploadService service;
    service.onCreateBinary = [&](const bt::CreateBinaryRequest&, bt::CreateBinaryResponse* response) {
        response->set_binarytransferuuid(service.createBinaryCalls == 1 ? "uuid-1" : "uuid-2");
        return grpc::Status::OK;
    };
    service.onUploadChunk = [](int callIndex, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
        bt::UploadChunkRequest request;
        if (callIndex == 0) {
            // First attempt (outside the retry loop): ack chunk 0, then fail
            // chunk 1 transiently. Falls through to an ordinary retry on the
            // same UUID -- ackedIndices_={0} is preserved, not cleared.
            if (!stream->Read(&request)) return grpc::Status::OK;
            bt::UploadChunkResponse response;
            response.set_binarytransferuuid(request.binarytransferuuid());
            response.set_chunkindex(request.chunkindex());
            stream->Write(response);
            if (!stream->Read(&request)) return grpc::Status::OK;  // absorb chunk 1's write
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "simulated transient drop");
        }
        if (callIndex == 1) {
            // Resumed stream: chunk 0 is already acked, so the client sends
            // only chunk 1 here. Reject it as an unknown UUID to trigger the
            // restart branch.
            if (!stream->Read(&request)) return grpc::Status::OK;
            bt::BinaryTransferError error;
            error.set_errortype(bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
            error.set_message("uuid expired");
            return grpc::Status(grpc::StatusCode::ABORTED, "uuid expired", error.SerializeAsString());
        }
        return ackAllChunks(stream);  // fresh stream on the restarted UUID: ack chunks 0, 1, 2
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryUploader uploader{channel, /*maxRetries=*/3, /*maxBackoff=*/0s};
    std::string uuid = uploader.upload("param", data, /*chunkSize=*/2);

    EXPECT_EQ(uuid, "uuid-2");
    EXPECT_EQ(service.createBinaryCalls, 2);  // original + restart
    EXPECT_EQ(service.uploadChunkCalls, 3);
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// S72: BinaryUploader::upload() clamps a caller-supplied chunkSize down to
// the shared Part B p56 ceiling (sila2::binary::kMaxBinaryChunkSize) so it
// never sends an oversized Binary Chunk over the wire.
// ---------------------------------------------------------------------------

// A caller-supplied chunkSize above the ceiling must be clamped: every
// UploadChunkRequest.payload() actually sent stays at or below the ceiling,
// and the data still gets split into more than one chunk (proving the
// clamp lowered chunkSize rather than the server silently truncating it).
TEST(BinaryUploaderE2E, UploadClampsOversizedChunkSizeToCeiling) {
    const std::string data(3 * 1024 * 1024, 'z');  // 3 MiB, above the 2 MiB ceiling
    std::vector<std::size_t> payloadSizes;
    MockBinaryUploadService service;
    service.onCreateBinary = [](const bt::CreateBinaryRequest&, bt::CreateBinaryResponse* response) {
        response->set_binarytransferuuid("fixed-uuid");
        return grpc::Status::OK;
    };
    service.onUploadChunk = [&payloadSizes](int, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
        bt::UploadChunkRequest request;
        while (stream->Read(&request)) {
            payloadSizes.push_back(request.payload().size());
            bt::UploadChunkResponse response;
            response.set_binarytransferuuid(request.binarytransferuuid());
            response.set_chunkindex(request.chunkindex());
            stream->Write(response);
        }
        return grpc::Status::OK;
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryUploader uploader{channel};
    // chunkSize is an override far above the ceiling; upload() must clamp it.
    (void)uploader.upload("param", data, /*chunkSize=*/8 * 1024 * 1024);

    ASSERT_GT(payloadSizes.size(), 1u);  // split into multiple chunks, not one 3 MiB chunk
    for (std::size_t size : payloadSizes) {
        EXPECT_LE(size, sila2::binary::kMaxBinaryChunkSize);
    }
    server->Shutdown();
}

// Pins that the clamp only ever lowers an oversized override: a small
// chunkSize (well below the ceiling) still produces the same chunking as
// before S72.
TEST(BinaryUploaderE2E, SmallChunkSizeOverrideUnaffected) {
    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s};
    auto [server, channel] = startServer({&uploadService});

    sila2::BinaryUploader uploader{channel};
    std::string uuid = uploader.upload("param", "abcdefg", /*chunkSize=*/3);  // -> 3 chunks

    ASSERT_TRUE(store.isComplete(uuid));
    auto assembled = store.assemble(uuid);
    EXPECT_EQ(std::string(assembled.begin(), assembled.end()), "abcdefg");
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT
// ---------------------------------------------------------------------------

TEST(BinaryUploaderE2E, CreateBinaryFailureThrowsRuntimeErrorImmediately) {
    MockBinaryUploadService service;
    service.onCreateBinary = [](const bt::CreateBinaryRequest&, bt::CreateBinaryResponse*) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "server down");
    };
    service.onUploadChunk = [](int, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
        return ackAllChunks(stream);
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryUploader uploader{channel};

    EXPECT_THROW(uploader.upload("param", "hello"), std::runtime_error);
    EXPECT_EQ(service.uploadChunkCalls, 0);  // never got past CreateBinary
    server->Shutdown();
}

TEST(BinaryUploaderE2E, RetriesExhaustedOnPersistentReadFailureThrows) {
    MockBinaryUploadService service;
    service.onCreateBinary = [](const bt::CreateBinaryRequest&, bt::CreateBinaryResponse* response) {
        response->set_binarytransferuuid("fixed-uuid");
        return grpc::Status::OK;
    };
    service.onUploadChunk = [](int, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
        bt::UploadChunkRequest request;
        stream->Read(&request);  // absorb chunk 0's write, then drop without acking
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "always transient");
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryUploader uploader{channel, /*maxRetries=*/2, /*maxBackoff=*/0s};

    EXPECT_THROW(uploader.upload("param", "hello"), std::runtime_error);
    EXPECT_EQ(service.createBinaryCalls, 1);       // never treated as an invalid-UUID restart
    EXPECT_EQ(service.uploadChunkCalls, 3);         // 1 initial attempt + 2 retries
    server->Shutdown();
}

// A single restart (INVALID_BINARY_TRANSFER_UUID) followed by persistent
// transient failures on the restarted UUID: combines the restart branch
// with retry exhaustion. As in InvalidUuidDuringUploadTriggersFullRestart
// above, the invalid-UUID rejection has to land on the SECOND uploadChunks()
// failure (callIndex == 1) since the first one can never trigger a restart.
//
// UNCAUGHT gap found while writing this test: upload()'s retry loop
// (BinaryUploader.cc lines 53-70) resets `attempt` to -1 (-> 0 after the
// loop's ++attempt) on every INVALID_BINARY_TRANSFER_UUID response. If the
// server returned that error on EVERY attempt from callIndex 1 onward,
// `attempt` would never reach maxRetries_ and the loop would never
// terminate -- an infinite retry loop, not a std::runtime_error. That
// scenario is deliberately NOT reproduced here (it would hang this test
// binary); this test instead has the server return
// INVALID_BINARY_TRANSFER_UUID only once, then switches to an ordinary
// transient failure so the retry budget is actually exhausted.
TEST(BinaryUploaderE2E, RestartThenExhaustedRetriesThrows) {
    MockBinaryUploadService service;
    service.onCreateBinary = [](const bt::CreateBinaryRequest&, bt::CreateBinaryResponse* response) {
        response->set_binarytransferuuid("fixed-uuid");
        return grpc::Status::OK;
    };
    service.onUploadChunk = [](int callIndex, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
        bt::UploadChunkRequest request;
        if (!stream->Read(&request)) return grpc::Status::OK;
        if (callIndex == 1) {
            bt::BinaryTransferError error;  // second attempt: restart-triggering rejection
            error.set_errortype(bt::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
            error.set_message("uuid expired");
            return grpc::Status(grpc::StatusCode::ABORTED, "uuid expired", error.SerializeAsString());
        }
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "always transient");
    };
    auto [server, channel] = startServer({&service});

    sila2::BinaryUploader uploader{channel, /*maxRetries=*/2, /*maxBackoff=*/0s};

    EXPECT_THROW(uploader.upload("param", "hello"), std::runtime_error);
    EXPECT_EQ(service.createBinaryCalls, 2);  // original + the one restart
    server->Shutdown();
}

// ---------------------------------------------------------------------------
// S26: MetadataInjector plumbing (auth token on every ClientContext)
// ---------------------------------------------------------------------------

// Covers BOTH uploader ClientContext sites (BinaryUploader.cc createBinary()
// and uploadChunks()) in one flow against the real gated server.
TEST(BinaryUploaderE2E, InjectedTokenPassesCreateBinaryAndUploadChunkGates) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, [](const std::string& fqi) {
        return fqi == kGatedParam || fqi == kUploadFqi;
    }};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s, &chain};
    auto [server, channel] = startServer({&uploadService});

    sila2::MetadataInjector injector;
    injector.set(sila2::kAccessTokenMetadataFqi,
                 serializeAccessToken(tokenStore.issue("alice", {kGatedParam, kUploadFqi}, 60s)));

    sila2::BinaryUploader uploader{channel, 3, 0s, &injector};
    const std::string uuid = uploader.upload(kGatedParam, "hello");

    EXPECT_FALSE(uuid.empty());
    EXPECT_TRUE(store.isComplete(uuid));
    server->Shutdown();
}

// The one case that distinguishes a correct fix from one that builds the
// ClientContext once outside the retry path: the first UploadChunk stream
// breaks after acking one chunk, forcing a resume on a fresh stream, and
// the token must be attached again on that fresh stream too.
TEST(BinaryUploaderE2E, ResumedUploadStreamReattachesToken) {
    MockBinaryUploadService service;
    service.onCreateBinary = [](const bt::CreateBinaryRequest&, bt::CreateBinaryResponse* response) {
        response->set_binarytransferuuid("fixed-uuid");
        return grpc::Status::OK;
    };
    service.onUploadChunk = [](int callIndex, grpc::ServerReaderWriter<bt::UploadChunkResponse, bt::UploadChunkRequest>* stream) {
        if (callIndex == 0) {
            bt::UploadChunkRequest request;
            if (!stream->Read(&request)) return grpc::Status::OK;
            bt::UploadChunkResponse response;  // ack chunk 0
            response.set_binarytransferuuid(request.binarytransferuuid());
            response.set_chunkindex(request.chunkindex());
            stream->Write(response);
            stream->Read(&request);  // absorb chunk 1's write, then drop it
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "simulated transient drop");
        }
        return ackAllChunks(stream);  // resumed stream: ack whatever's left
    };
    auto [server, channel] = startServer({&service});

    sila2::MetadataInjector injector;
    injector.set(sila2::kAccessTokenMetadataFqi, serializeAccessToken("any-token"));

    sila2::BinaryUploader uploader{channel, /*maxRetries=*/3, /*maxBackoff=*/0s, &injector};
    std::string uuid = uploader.upload("param", "abcdef", /*chunkSize=*/2);  // 3 chunks

    EXPECT_EQ(uuid, "fixed-uuid");
    ASSERT_EQ(service.uploadChunkHadToken.size(), 2u);
    EXPECT_TRUE(service.uploadChunkHadToken[0]);
    EXPECT_TRUE(service.uploadChunkHadToken[1]);
    server->Shutdown();
}

// The nullable default must stay a no-op: an ungated server + an uploader
// built without an injector (the pre-S26 call shape) still succeeds.
TEST(BinaryUploaderE2E, UngatedServerStillAcceptsUploaderWithNoInjector) {
    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s};
    auto [server, channel] = startServer({&uploadService});

    sila2::BinaryUploader uploader{channel};
    std::string uuid = uploader.upload("param", "hello");

    EXPECT_FALSE(uuid.empty());
    EXPECT_TRUE(store.isComplete(uuid));
    server->Shutdown();
}

// Rejection at the CreateBinary gate (BinaryUploader.cc's first ClientContext
// site): no injector, so a protected parameterIdentifier is denied before any
// chunk stream opens.
TEST(BinaryUploaderE2E, MissingTokenRejectedAtCreateBinaryGate) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, [](const std::string& fqi) {
        return fqi == kGatedParam;
    }};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s, &chain};
    auto [server, channel] = startServer({&uploadService});

    sila2::BinaryUploader uploader{channel, 0, 0s};

    try {
        (void)uploader.upload(kGatedParam, "hello");
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find("CreateBinary"), std::string::npos);
    }
    server->Shutdown();
}

// Rejection at the UploadChunk gate (BinaryUploader.cc's second ClientContext
// site): CreateBinary passes pre-auth (it gates on a different, unprotected
// FQI) so the stream itself is what gets rejected.
TEST(BinaryUploaderE2E, MissingTokenRejectedAtUploadChunkGate) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, [](const std::string& fqi) {
        return fqi == kUploadFqi;
    }};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s, &chain};
    auto [server, channel] = startServer({&uploadService});

    sila2::BinaryUploader uploader{channel, 0, 0s};

    try {
        (void)uploader.upload(kGatedParam, "hello");
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find("failed after 0 retries"), std::string::npos);
    }
    server->Shutdown();
}

// Proves the injector is plumbing, not a bypass: a token issued for an
// unrelated FQI is present but still denied by AuthTokenStore::validate().
TEST(BinaryUploaderE2E, TokenForUnrelatedFqiStillRejected) {
    AuthTokenStore tokenStore;
    AuthorizationInterceptor interceptor{tokenStore, [](const std::string& fqi) {
        return fqi == kGatedParam;
    }};
    InterceptorChain chain;
    chain.auth = &interceptor;

    sila2::InMemoryBinaryStore store;
    sila2::BinaryUploadService uploadService{store, 300s, &chain};
    auto [server, channel] = startServer({&uploadService});

    sila2::MetadataInjector injector;
    injector.set(sila2::kAccessTokenMetadataFqi,
                 serializeAccessToken(tokenStore.issue("alice", {"org.test/Other/v1/Command/X"}, 60s)));

    sila2::BinaryUploader uploader{channel, 0, 0s, &injector};

    EXPECT_THROW({ (void)uploader.upload(kGatedParam, "hello"); }, std::runtime_error);
    server->Shutdown();
}

}  // namespace
