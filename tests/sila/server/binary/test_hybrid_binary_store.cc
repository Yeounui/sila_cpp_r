// Tests for HybridBinaryStore: size-based routing between InMemoryBinaryStore
// and FileSpoolBinaryStore, independent coexistence of small/large slots,
// removeExpired() sweeping both inner stores and pruning the routing map, and
// the std::out_of_range / std::logic_error paths shared via storeFor() and
// the inner stores' BinaryStore interface contract.
#include <sila/server/binary/HybridBinaryStore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using sila2::HybridBinaryStore;
using namespace std::chrono_literals;

// Binaries with binarySize > kThreshold route to the file spool; <= kThreshold
// stay in memory (see HybridBinaryStore.cc's createSlot()).
constexpr std::size_t kThreshold = 100;

class HybridBinaryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        tmpDir_ = std::filesystem::temp_directory_path() /
                  ("sila2_hybrid_store_test_" + std::string{testInfo->test_suite_name()} + "_" +
                   std::string{testInfo->name()});
        std::error_code ec;
        std::filesystem::remove_all(tmpDir_, ec);
        store_ = std::make_unique<HybridBinaryStore>(kThreshold, tmpDir_);
    }

    void TearDown() override {
        store_.reset();
        std::error_code ec;
        std::filesystem::remove_all(tmpDir_, ec);
    }

    std::filesystem::path tmpDir_;
    std::unique_ptr<HybridBinaryStore> store_;
};

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST_F(HybridBinaryStoreTest, SmallBinaryRoutesToMemoryAndAssemblesCorrectly) {
    const std::vector<uint8_t> chunk{1, 2, 3, 4, 5};
    const std::string uuid = store_->createSlot(chunk.size(), 1, 60s);

    store_->storeChunk(uuid, 0, chunk);

    ASSERT_TRUE(store_->isComplete(uuid));
    EXPECT_EQ(store_->assemble(uuid), chunk);
    // No per-slot spool subdirectory should have been created for a
    // memory-routed slot.
    EXPECT_FALSE(std::filesystem::exists(tmpDir_ / uuid));
}

TEST_F(HybridBinaryStoreTest, LargeBinaryRoutesToFileSpoolAndAssemblesCorrectly) {
    const std::vector<uint8_t> chunk0(75, 0xAB);
    const std::vector<uint8_t> chunk1(75, 0xCD);
    const std::string uuid = store_->createSlot(chunk0.size() + chunk1.size(), 2, 60s);

    store_->storeChunk(uuid, 0, chunk0);
    store_->storeChunk(uuid, 1, chunk1);

    ASSERT_TRUE(store_->isComplete(uuid));
    std::vector<uint8_t> expected = chunk0;
    expected.insert(expected.end(), chunk1.begin(), chunk1.end());
    EXPECT_EQ(store_->assemble(uuid), expected);
    // FileSpoolBinaryStore creates a per-slot subdirectory under tmpDir_.
    EXPECT_TRUE(std::filesystem::exists(tmpDir_ / uuid));
}

TEST_F(HybridBinaryStoreTest, MixedSmallAndLargeBinariesCoexistIndependently) {
    const std::vector<uint8_t> smallChunk{9, 9};
    const std::vector<uint8_t> largeChunk(150, 0x42);

    const std::string smallUuid = store_->createSlot(smallChunk.size(), 1, 60s);
    const std::string largeUuid = store_->createSlot(largeChunk.size(), 1, 60s);

    store_->storeChunk(smallUuid, 0, smallChunk);
    store_->storeChunk(largeUuid, 0, largeChunk);

    EXPECT_EQ(store_->assemble(smallUuid), smallChunk);
    EXPECT_EQ(store_->assemble(largeUuid), largeChunk);
    EXPECT_EQ(store_->size(), 2u);
}

TEST_F(HybridBinaryStoreTest, ReadRangeUsesTheRoutedStore) {
    const std::string uuid = store_->createSlot(150, 2, 60s);
    store_->storeChunk(uuid, 0, std::vector<uint8_t>(75, 1));
    store_->storeChunk(uuid, 1, std::vector<uint8_t>(75, 2));

    EXPECT_EQ(store_->readRange(uuid, 74, 3), (std::vector<uint8_t>{1, 2, 2}));
    // Spool-routed slot: the out-of-range rejection must reach through
    // HybridBinaryStore's delegation to FileSpoolBinaryStore::readRange.
    EXPECT_THROW((void)store_->readRange(uuid, 149, 5), std::out_of_range);

    // Memory-routed twin: proves the guard reaches through Hybrid for both
    // routing destinations, not just the spool one above.
    const std::string small = store_->createSlot(5, 1, 60s);
    store_->storeChunk(small, 0, {0, 1, 2, 3, 4});
    EXPECT_THROW((void)store_->readRange(small, 4, 2), std::out_of_range);
}

TEST_F(HybridBinaryStoreTest, RoutingIgnoresUuidCase) {
    // Part A p90: BinaryTransferUUID comparison MUST ignore case. Exercises
    // routing_'s CaseInsensitiveLess via storeFor() (find) and contains() (count).
    const std::string uuid = store_->createSlot(3, 1, 60s);
    std::string upper = uuid;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    EXPECT_TRUE(store_->contains(upper));
}

TEST_F(HybridBinaryStoreTest, RemoveExpiredSweepsBothStoresAndPrunesRoutingMap) {
    // Negative lifetime places expiresAt in the past for both slots, so
    // removeExpired() sees them as already expired without sleeping.
    const std::string smallUuid = store_->createSlot(5, 1, -1s);
    const std::string largeUuid = store_->createSlot(150, 1, -1s);

    EXPECT_EQ(store_->removeExpired(), 2u);

    EXPECT_FALSE(store_->contains(smallUuid));
    EXPECT_FALSE(store_->contains(largeUuid));
    EXPECT_EQ(store_->size(), 0u);
    // storeFor() (via routing_) must have been pruned too, or subsequent
    // lookups on these uuids would silently resolve to a stale entry.
    EXPECT_THROW(store_->remove(smallUuid), std::out_of_range);
    EXPECT_THROW(store_->remove(largeUuid), std::out_of_range);
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT (HybridBinaryStore::storeFor()
// throws for unknown uuids; assemble()'s incompleteness check is inherited
// from the routed-to inner store)
// ---------------------------------------------------------------------------

TEST_F(HybridBinaryStoreTest, StoreChunkWithUnknownUuidThrowsOutOfRange) {
    EXPECT_THROW(store_->storeChunk("not-a-known-uuid", 0, {1}), std::out_of_range);
    EXPECT_THROW((void)store_->readRange("not-a-known-uuid", 0, 1), std::out_of_range);
}

TEST_F(HybridBinaryStoreTest, ContainsIsFalseForUnknownUuidEvenWithCaseFolding) {
    // Case folding must not turn a lookup miss into a false hit.
    EXPECT_FALSE(store_->contains("no-such-uuid"));
}

TEST_F(HybridBinaryStoreTest, AssembleOnIncompleteSlotThrowsLogicError) {
    // chunkCount = 2, but only index 0 is ever stored.
    const std::string uuid = store_->createSlot(10, 2, 60s);
    store_->storeChunk(uuid, 0, {1, 2, 3});

    EXPECT_THROW(store_->assemble(uuid), std::logic_error);
    EXPECT_THROW(store_->readRange(uuid, 0, 1), std::logic_error);
}

TEST_F(HybridBinaryStoreTest, RemoveUnknownUuidThrowsOutOfRange) {
    EXPECT_THROW(store_->remove("not-a-known-uuid"), std::out_of_range);
}

}  // namespace
