// Integration tests for CloudEnvelopeRouter::route()'s Binary Transfer
// paths (architecture.md §3.9/§3.5): CreateBinaryUpload, UploadChunk,
// DeleteUploadedBinary, GetBinaryInfo, GetChunk, DeleteDownloadedBinary.
// Drives route() through a real gRPC stream via CloudRouterFixture and
// inspects the SiLAServerMessage each case writes back.
#include "CloudRouterTestHarness.h"

#include <sila/common/binary/BinaryChunkLimit.h>
#include <sila/server/auth/AuthTokenStore.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/binary/BinaryStore.h>
#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace cloud = sila2::org::silastandard;
using cloud_test::CloudRouterFixture;

class CloudRouterBinary : public CloudRouterFixture {};

class VanishingBinaryStore final : public sila2::BinaryStore {
public:
    enum class Operation { Remove, BinarySize, RemainingLifetime };

    explicit VanishingBinaryStore(Operation operation) : operation_{operation} {}

    std::string createSlot(std::size_t, std::size_t,
                           std::chrono::seconds) override { return {}; }
    void storeChunk(const std::string&, std::size_t, std::vector<uint8_t>) override {}
    bool isComplete(const std::string&) const override { return false; }
    std::vector<uint8_t> assemble(const std::string&) const override { return {}; }
    std::vector<uint8_t> readRange(const std::string&, std::size_t, std::size_t) const override {
        return {};
    }
    void updateLifetime(const std::string&, std::chrono::seconds) override {}
    void remove(const std::string&) override {
        if (operation_ == Operation::Remove) throw std::out_of_range{"vanished"};
    }
    std::size_t binarySize(const std::string&) const override {
        if (operation_ == Operation::BinarySize) throw std::out_of_range{"vanished"};
        return 1;
    }
    std::chrono::seconds remainingLifetime(const std::string&) const override {
        if (operation_ == Operation::RemainingLifetime) throw std::out_of_range{"vanished"};
        return std::chrono::seconds{1};
    }
    bool contains(const std::string&) const override { return true; }
    std::size_t removeExpired() override { return 0; }
    std::size_t size() const override { return 1; }

private:
    Operation operation_;
};

// -- P1: CreateBinaryUploadRequest -> createBinaryResponse -------------------
TEST_F(CloudRouterBinary, CreateBinaryUploadRequestReturnsSlotUuidAndLifetime) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-1");
    auto* createReq = msg.mutable_createbinaryuploadrequest()->mutable_createbinaryrequest();
    createReq->set_binarysize(100);
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier("org.test/Feature/Param/v1");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_createbinaryresponse());
    EXPECT_FALSE(resp.createbinaryresponse().binarytransferuuid().empty());
    EXPECT_EQ(resp.createbinaryresponse().lifetimeofbinary().seconds(), 300);
}

// -- P2: UploadChunkRequest with valid UUID -> uploadChunkResponse -----------
TEST_F(CloudRouterBinary, UploadChunkRequestEchoesUuidAndChunkIndex) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    std::string uuid = binaryStore.createSlot(11, 1, std::chrono::seconds{300});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-2");
    auto* chunk = msg.mutable_uploadchunkrequest();
    chunk->set_binarytransferuuid(uuid);
    chunk->set_chunkindex(0);
    chunk->set_payload("binary-data");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_uploadchunkresponse());
    EXPECT_EQ(resp.uploadchunkresponse().binarytransferuuid(), uuid);
    EXPECT_EQ(resp.uploadchunkresponse().chunkindex(), 0u);
}

// -- P2b: Part B p56 -- exactly kMaxBinaryChunkSize is still a legal chunk --
TEST_F(CloudRouterBinary, UploadChunkRequestAcceptsChunkAtCeiling) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    const std::string payload(sila2::binary::kMaxBinaryChunkSize, 'x');
    std::string uuid = binaryStore.createSlot(payload.size(), 1, std::chrono::seconds{300});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-2b");
    auto* chunk = msg.mutable_uploadchunkrequest();
    chunk->set_binarytransferuuid(uuid);
    chunk->set_chunkindex(0);
    chunk->set_payload(payload);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_uploadchunkresponse());
    EXPECT_EQ(resp.uploadchunkresponse().binarytransferuuid(), uuid);
}

// -- P3: GetBinaryInfoRequest with valid UUID -> getBinaryResponse -----------
TEST_F(CloudRouterBinary, GetBinaryInfoRequestReturnsBinarySize) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    std::string uuid = binaryStore.createSlot(100, 1, std::chrono::seconds{300});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-3");
    msg.mutable_getbinaryinforequest()->set_binarytransferuuid(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_getbinaryresponse());
    EXPECT_EQ(resp.getbinaryresponse().binarysize(), 100u);
}

// -- P4: GetChunkRequest with fully uploaded data -> getChunkResponse slice --
TEST_F(CloudRouterBinary, GetChunkRequestReturnsAssembledPayloadSlice) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    std::string payload = "binary-data";
    std::string uuid = binaryStore.createSlot(payload.size(), 1, std::chrono::seconds{300});
    binaryStore.storeChunk(uuid, 0, std::vector<uint8_t>{payload.begin(), payload.end()});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-4");
    auto* getChunk = msg.mutable_getchunkrequest();
    getChunk->set_binarytransferuuid(uuid);
    getChunk->set_offset(0);
    getChunk->set_length(static_cast<uint32_t>(payload.size()));

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_getchunkresponse());
    EXPECT_EQ(resp.getchunkresponse().binarytransferuuid(), uuid);
    EXPECT_EQ(resp.getchunkresponse().payload(), payload);
}

// -- P4b (CAUGHT): GetChunkRequest past end of binary -> downloadFailed ------
TEST_F(CloudRouterBinary, GetChunkRequestPastEndOfBinaryReturnsDownloadFailed) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    std::string payload = "binary-data";  // 11 bytes
    std::string uuid = binaryStore.createSlot(payload.size(), 1, std::chrono::seconds{300});
    binaryStore.storeChunk(uuid, 0, std::vector<uint8_t>{payload.begin(), payload.end()});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-4b");
    auto* getChunk = msg.mutable_getchunkrequest();
    getChunk->set_binarytransferuuid(uuid);
    getChunk->set_offset(5);
    getChunk->set_length(100);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    // InMemoryBinaryStore::readRange now throws std::out_of_range for a
    // range past binarySize; CloudEnvelopeRouter's existing
    // catch(std::exception) around this call already maps that to
    // BINARY_DOWNLOAD_FAILED, so this exercises no router code change.
    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
}

// -- P5: DeleteUploadedBinaryRequest -> deleteBinaryResponse -----------------
TEST_F(CloudRouterBinary, DeleteUploadedBinaryRequestRemovesSlot) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    std::string uuid = binaryStore.createSlot(10, 1, std::chrono::seconds{300});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-5");
    msg.mutable_deleteuploadedbinaryrequest()->set_binarytransferuuid(uuid);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_deletebinaryresponse());
    EXPECT_FALSE(binaryStore.contains(uuid));
}

TEST_F(CloudRouterBinary, VanishedBinaryAfterPresenceCheckReturnsError) {
    auto expectError = [&](VanishingBinaryStore::Operation operation,
                           cloud::BinaryTransferError::ErrorType expected,
                           const std::function<void(cloud::SiLAClientMessage&)>& makeRequest) {
        sila2::FeatureRegistry registry;
        VanishingBinaryStore binaryStore{operation};
        sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);
        cloud::SiLAClientMessage msg;
        msg.set_requestuuid("vanished");
        makeRequest(msg);

        router.route(msg, *writer_, writer_, calls_);
        auto resp = popResponse();
        ASSERT_TRUE(resp.has_binarytransfererror());
        EXPECT_EQ(resp.requestuuid(), "vanished");
        EXPECT_EQ(resp.binarytransfererror().errortype(), expected);
    };

    expectError(VanishingBinaryStore::Operation::Remove,
                cloud::BinaryTransferError::BINARY_UPLOAD_FAILED,
                [](cloud::SiLAClientMessage& msg) {
                    msg.mutable_deleteuploadedbinaryrequest()->set_binarytransferuuid("uuid");
                });
    expectError(VanishingBinaryStore::Operation::Remove,
                cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED,
                [](cloud::SiLAClientMessage& msg) {
                    msg.mutable_deletedownloadedbinaryrequest()->set_binarytransferuuid("uuid");
                });
    expectError(VanishingBinaryStore::Operation::BinarySize,
                cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED,
                [](cloud::SiLAClientMessage& msg) {
                    msg.mutable_getbinaryinforequest()->set_binarytransferuuid("uuid");
                });
    expectError(VanishingBinaryStore::Operation::RemainingLifetime,
                cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED,
                [](cloud::SiLAClientMessage& msg) {
                    msg.mutable_getbinaryinforequest()->set_binarytransferuuid("uuid");
                });
}

// -- P6: full upload -> download -> delete cycle through one router ---------
TEST_F(CloudRouterBinary, FullUploadDownloadDeleteCycle) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);
    std::string payload = "full-cycle-data";

    cloud::SiLAClientMessage createMsg;
    createMsg.set_requestuuid("cycle-create");
    auto* createReq = createMsg.mutable_createbinaryuploadrequest()->mutable_createbinaryrequest();
    createReq->set_binarysize(payload.size());
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier("org.test/Feature/Param/v1");
    router.route(createMsg, *writer_, writer_, calls_);
    std::string uuid = popResponse().createbinaryresponse().binarytransferuuid();

    cloud::SiLAClientMessage chunkMsg;
    chunkMsg.set_requestuuid("cycle-chunk");
    auto* chunk = chunkMsg.mutable_uploadchunkrequest();
    chunk->set_binarytransferuuid(uuid);
    chunk->set_chunkindex(0);
    chunk->set_payload(payload);
    router.route(chunkMsg, *writer_, writer_, calls_);
    ASSERT_TRUE(popResponse().has_uploadchunkresponse());

    cloud::SiLAClientMessage infoMsg;
    infoMsg.set_requestuuid("cycle-info");
    infoMsg.mutable_getbinaryinforequest()->set_binarytransferuuid(uuid);
    router.route(infoMsg, *writer_, writer_, calls_);
    EXPECT_EQ(popResponse().getbinaryresponse().binarysize(), payload.size());

    cloud::SiLAClientMessage getChunkMsg;
    getChunkMsg.set_requestuuid("cycle-getchunk");
    auto* getChunk = getChunkMsg.mutable_getchunkrequest();
    getChunk->set_binarytransferuuid(uuid);
    getChunk->set_offset(0);
    getChunk->set_length(static_cast<uint32_t>(payload.size()));
    router.route(getChunkMsg, *writer_, writer_, calls_);
    EXPECT_EQ(popResponse().getchunkresponse().payload(), payload);

    cloud::SiLAClientMessage deleteMsg;
    deleteMsg.set_requestuuid("cycle-delete");
    deleteMsg.mutable_deletedownloadedbinaryrequest()->set_binarytransferuuid(uuid);
    router.route(deleteMsg, *writer_, writer_, calls_);
    ASSERT_TRUE(popResponse().has_deletebinaryresponse());
    EXPECT_FALSE(binaryStore.contains(uuid));
}

// -- N1 (CAUGHT): CreateBinaryUploadRequest with no binaryStore --------------
TEST_F(CloudRouterBinary, CreateBinaryUploadRequestWithoutStoreReturnsUploadFailed) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router(registry);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n1");
    auto* createReq = msg.mutable_createbinaryuploadrequest()->mutable_createbinaryrequest();
    createReq->set_binarysize(10);
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier("org.test/Feature/Param/v1");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

// -- N2 (CAUGHT): UploadChunkRequest with unknown UUID -----------------------
TEST_F(CloudRouterBinary, UploadChunkRequestWithUnknownUuidReturnsInvalidUuid) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n2");
    auto* chunk = msg.mutable_uploadchunkrequest();
    chunk->set_binarytransferuuid("no-such-uuid");
    chunk->set_chunkindex(0);
    chunk->set_payload("data");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

// -- N2b (CAUGHT): Part B p56 -- an oversized UploadChunk payload MUST be
// rejected, mirroring the gRPC transport (BinaryUploadService::uploadChunk).
TEST_F(CloudRouterBinary, UploadChunkRequestRejectsOversizedPayload) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    const std::string payload(sila2::binary::kMaxBinaryChunkSize + 1, 'x');
    std::string uuid = binaryStore.createSlot(payload.size(), 1, std::chrono::seconds{300});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n2b");
    auto* chunk = msg.mutable_uploadchunkrequest();
    chunk->set_binarytransferuuid(uuid);
    chunk->set_chunkindex(0);
    chunk->set_payload(payload);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

// -- N4b (CAUGHT): Part B p56 -- a GetChunk length above the ceiling MUST be
// rejected, mirroring the gRPC transport (BinaryDownloadService::getChunk).
TEST_F(CloudRouterBinary, GetChunkRequestRejectsOversizedLength) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    std::string payload = "binary-data";
    std::string uuid = binaryStore.createSlot(payload.size(), 1, std::chrono::seconds{300});
    binaryStore.storeChunk(uuid, 0, std::vector<uint8_t>{payload.begin(), payload.end()});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n4b");
    auto* getChunk = msg.mutable_getchunkrequest();
    getChunk->set_binarytransferuuid(uuid);
    getChunk->set_offset(0);
    getChunk->set_length(static_cast<uint32_t>(sila2::binary::kMaxBinaryChunkSize + 1));

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
}

// -- N3 (CAUGHT): GetBinaryInfoRequest with no binaryStore -------------------
TEST_F(CloudRouterBinary, GetBinaryInfoRequestWithoutStoreReturnsDownloadFailed) {
    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router(registry);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n3");
    msg.mutable_getbinaryinforequest()->set_binarytransferuuid("some-uuid");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
}

// -- N4 (CAUGHT): GetChunkRequest with unknown UUID --------------------------
TEST_F(CloudRouterBinary, GetChunkRequestWithUnknownUuidReturnsInvalidUuid) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n4");
    auto* getChunk = msg.mutable_getchunkrequest();
    getChunk->set_binarytransferuuid("no-such-uuid");
    getChunk->set_offset(0);
    getChunk->set_length(10);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID);
}

// -- N5 (CAUGHT): CreateBinaryUploadRequest rejected by auth interceptor ----
TEST_F(CloudRouterBinary, CreateBinaryUploadRequestWithMissingTokenReturnsAuthMessage) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::auth::AuthTokenStore tokenStore;
    auto isProtected = [](const std::string&) { return true; };
    sila2::auth::AuthorizationInterceptor authInterceptor(tokenStore, isProtected);
    sila2::InterceptorChain chain;
    chain.auth = &authInterceptor;
    chain.binarySlotLifetime = std::chrono::seconds{600};
    sila2::CloudEnvelopeRouter router(registry, &chain, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n5");
    auto* uploadReq = msg.mutable_createbinaryuploadrequest();
    auto* createReq = uploadReq->mutable_createbinaryrequest();
    createReq->set_binarysize(10);
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier("org.test/Feature/Param/v1");
    // No access-token metadata -> AuthorizationInterceptor::intercept() throws.

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_UPLOAD_FAILED);
    EXPECT_FALSE(resp.binarytransfererror().message().empty());
}

// -- N6 (CAUGHT): GetChunkRequest before all chunks are uploaded -------------
TEST_F(CloudRouterBinary, GetChunkRequestWithIncompleteUploadReturnsDownloadFailed) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    // chunkCount=2 but only chunk 0 is stored -> assemble() throws std::logic_error.
    std::string uuid = binaryStore.createSlot(20, 2, std::chrono::seconds{300});
    binaryStore.storeChunk(uuid, 0, std::vector<uint8_t>{'a', 'b'});
    sila2::CloudEnvelopeRouter router(registry, nullptr, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-n6");
    auto* getChunk = msg.mutable_getchunkrequest();
    getChunk->set_binarytransferuuid(uuid);
    getChunk->set_offset(0);
    getChunk->set_length(20);

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED);
}

// -- S13: CreateBinaryUploadRequest's parameterIdentifier-vs-registered-
// Feature gate. SIBLING TRANSPORT of the gRPC gate in
// test_binary_transfer_service.cc -- same InterceptorChain field, same
// isKnownParameterFqi() call, so this router pins that both transports agree.

// -- S13-P: registered parameter FQI succeeds --------------------------------
TEST_F(CloudRouterBinary, CreateBinaryUploadRequestWithRegisteredParameterFqiSucceeds) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::InterceptorChain chain;
    chain.registeredFeatureFqis = {"org.test/Feature/v1"};
    sila2::CloudEnvelopeRouter router(registry, &chain, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-s13-p");
    auto* createReq = msg.mutable_createbinaryuploadrequest()->mutable_createbinaryrequest();
    createReq->set_binarysize(100);
    createReq->set_chunkcount(1);
    createReq->set_parameteridentifier("org.test/Feature/v1/Command/C/Parameter/P");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_createbinaryresponse());
    EXPECT_FALSE(resp.createbinaryresponse().binarytransferuuid().empty());
}

// -- S13-N: parameterIdentifier that names no item of a registered Feature --
TEST_F(CloudRouterBinary, CreateBinaryUploadRequestWithUnknownParameterFqiReturnsUploadFailed) {
    sila2::FeatureRegistry registry;
    sila2::InMemoryBinaryStore binaryStore;
    sila2::InterceptorChain chain;
    chain.registeredFeatureFqis = {"org.test/Feature/v1"};
    sila2::CloudEnvelopeRouter router(registry, &chain, &binaryStore);

    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-s13-n");
    auto* createReq = msg.mutable_createbinaryuploadrequest()->mutable_createbinaryrequest();
    createReq->set_binarysize(100);
    createReq->set_chunkcount(1);
    // No /Command/ or /Metadata/ segment -- names no item, matching the shape
    // the pre-S13 tests above use for an unrelated Feature.
    createReq->set_parameteridentifier("org.test/Feature/Param/v1");

    router.route(msg, *writer_, writer_, calls_);
    auto resp = popResponse();

    ASSERT_TRUE(resp.has_binarytransfererror());
    EXPECT_EQ(resp.binarytransfererror().errortype(),
              cloud::BinaryTransferError::BINARY_UPLOAD_FAILED);
}

}  // namespace
