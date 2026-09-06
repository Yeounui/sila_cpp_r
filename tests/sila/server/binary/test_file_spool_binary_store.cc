// Tests for FileSpoolBinaryStore: disk-backed chunk storage, order-independent
// assembly against files under the spool directory, removeExpired() cleanup of
// per-slot subdirectories, and the std::out_of_range / std::logic_error paths
// shared with the BinaryStore interface contract.
#include <sila/server/binary/FileSpoolBinaryStore.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using sila2::FileSpoolBinaryStore;
using namespace std::chrono_literals;

// Mirrors FileSpoolBinaryStore::chunkPath()'s private naming scheme so tests
// can check for chunk files on disk without exposing the method.
std::filesystem::path ChunkPath(const std::filesystem::path& spoolDir, const std::string& uuid,
                                 std::size_t index) {
    char name[24];
    std::snprintf(name, sizeof(name), "chunk-%04zu.bin", index);
    return spoolDir / uuid / name;
}

class FileSpoolBinaryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        tmpDir_ = std::filesystem::temp_directory_path() /
                  ("sila2_file_spool_test_" + std::string{testInfo->test_suite_name()} + "_" +
                   std::string{testInfo->name()});
        std::error_code ec;
        std::filesystem::remove_all(tmpDir_, ec);
        store_ = std::make_unique<FileSpoolBinaryStore>(tmpDir_);
    }

    void TearDown() override {
        store_.reset();
        std::error_code ec;
        std::filesystem::remove_all(tmpDir_, ec);
    }

    std::filesystem::path tmpDir_;
    std::unique_ptr<FileSpoolBinaryStore> store_;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST_F(FileSpoolBinaryStoreTest, StoreChunksInOrderThenAssembleRoundTripsThroughDisk) {
    const std::vector<uint8_t> chunk0{1, 2, 3};
    const std::vector<uint8_t> chunk1{4, 5};
    const std::string uuid = store_->createSlot(chunk0.size() + chunk1.size(), 2, 60s);

    store_->storeChunk(uuid, 0, chunk0);
    ASSERT_TRUE(std::filesystem::exists(ChunkPath(tmpDir_, uuid, 0)));
    store_->storeChunk(uuid, 1, chunk1);
    ASSERT_TRUE(std::filesystem::exists(ChunkPath(tmpDir_, uuid, 1)));

    ASSERT_TRUE(store_->isComplete(uuid));
    EXPECT_EQ(store_->assemble(uuid), (std::vector<uint8_t>{1, 2, 3, 4, 5}));
}

TEST_F(FileSpoolBinaryStoreTest, ReadRangeCrossesSpoolChunksWithoutReturningWholePayload) {
    const std::string uuid = store_->createSlot(9, 3, 60s);
    store_->storeChunk(uuid, 0, {0, 1, 2});
    store_->storeChunk(uuid, 1, {3, 4, 5});
    store_->storeChunk(uuid, 2, {6, 7, 8});

    const auto range = store_->readRange(uuid, 2, 4);
    EXPECT_EQ(range, (std::vector<uint8_t>{2, 3, 4, 5}));
    EXPECT_LT(range.size(), 9u);
    EXPECT_EQ(store_->readRange(uuid, 8, 1), (std::vector<uint8_t>{8}));

    // A range past the tail must be rejected, not clamped to what's left —
    // see InMemoryBinaryStore::readRange for why.
    EXPECT_THROW((void)store_->readRange(uuid, 8, 10), std::out_of_range);
    EXPECT_THROW((void)store_->readRange(uuid, 100, 1), std::out_of_range);
}

TEST_F(FileSpoolBinaryStoreTest, StoreChunksOutOfOrderThenAssembleConcatenatesInIndexOrder) {
    const std::vector<uint8_t> chunk0{1, 2};
    const std::vector<uint8_t> chunk1{3, 4};
    const std::vector<uint8_t> chunk2{5, 6};
    const std::string uuid = store_->createSlot(6, 3, 60s);

    store_->storeChunk(uuid, 2, chunk2);
    store_->storeChunk(uuid, 0, chunk0);
    store_->storeChunk(uuid, 1, chunk1);

    ASSERT_TRUE(store_->isComplete(uuid));
    EXPECT_EQ(store_->assemble(uuid), (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST_F(FileSpoolBinaryStoreTest, CreateSlotWithinAvailableSpaceSucceeds) {
    // Injected availableSpaceFn_ removes the dependency on real free disk.
    FileSpoolBinaryStore store{tmpDir_, [] { return std::uintmax_t{1000}; }};

    const std::string uuid = store.createSlot(50, 1, 60s);

    EXPECT_FALSE(uuid.empty());
}

TEST_F(FileSpoolBinaryStoreTest, RemoveExpiredDeletesSpoolSubdirectory) {
    // Negative lifetime places expiresAt in the past, so removeExpired() sees
    // this slot as already expired without needing to sleep in the test.
    const std::string uuid = store_->createSlot(1, 1, -1s);
    const std::filesystem::path slotDir = tmpDir_ / uuid;
    ASSERT_TRUE(std::filesystem::exists(slotDir));

    EXPECT_EQ(store_->removeExpired(), 1u);

    EXPECT_FALSE(store_->contains(uuid));
    EXPECT_EQ(store_->size(), 0u);
    EXPECT_FALSE(std::filesystem::exists(slotDir));
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT (explicit throws in
// FileSpoolBinaryStore.cc's shared findOrThrow() / storeChunk() / assemble())
// ---------------------------------------------------------------------------

TEST_F(FileSpoolBinaryStoreTest, StoreChunkWithUnknownUuidThrowsOutOfRange) {
    EXPECT_THROW(store_->storeChunk("not-a-known-uuid", 0, {1}), std::out_of_range);
    EXPECT_THROW((void)store_->readRange("not-a-known-uuid", 0, 1), std::out_of_range);
}

TEST_F(FileSpoolBinaryStoreTest, StoreChunkWithIndexAtChunkCountThrowsOutOfRange) {
    const std::string uuid = store_->createSlot(1, 1, 60s);

    EXPECT_THROW(store_->storeChunk(uuid, 1, {1}), std::out_of_range);
}

TEST_F(FileSpoolBinaryStoreTest, AssembleOnIncompleteSlotThrowsLogicError) {
    // chunkCount = 2, but only index 0 is ever stored.
    const std::string uuid = store_->createSlot(10, 2, 60s);
    store_->storeChunk(uuid, 0, {1, 2, 3});

    EXPECT_THROW(store_->assemble(uuid), std::logic_error);
    EXPECT_THROW(store_->readRange(uuid, 0, 1), std::logic_error);
}

TEST_F(FileSpoolBinaryStoreTest, CreateSlotOverAvailableSpaceThrows) {
    // Part B p58: the space check must be enforced before the UUID is issued,
    // using the injected query rather than the host's real free disk.
    FileSpoolBinaryStore store{tmpDir_, [] { return std::uintmax_t{100}; }};

    EXPECT_THROW((void)store.createSlot(200, 1, 60s), std::invalid_argument);
}

TEST_F(FileSpoolBinaryStoreTest, CreateSlotFailsClosedWhenSpaceQueryFails) {
    // Part B p58: a space query that cannot run must not be treated as
    // unlimited space. Removing the spool root makes std::filesystem::space
    // fail on the default (non-injected) path.
    std::error_code ec;
    std::filesystem::remove_all(tmpDir_, ec);

    EXPECT_THROW((void)store_->createSlot(200, 1, 60s), std::runtime_error);
}

}  // namespace
