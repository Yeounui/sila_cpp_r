// Tests for InMemoryBinaryStore: chunk storage, order-independent assembly, and the
// std::out_of_range / std::logic_error paths from findOrThrow() lookup and
// the assemble()-only completeness/size checks.
#include <sila/server/binary/InMemoryBinaryStore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using sila2::InMemoryBinaryStore;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(InMemoryBinaryStore, CreateSlotReturnsUuidAndStartsIncomplete) {
    InMemoryBinaryStore store;

    const std::string uuid = store.createSlot(5, 1, 60s);

    EXPECT_FALSE(uuid.empty());
    EXPECT_FALSE(store.isComplete(uuid));
    EXPECT_EQ(store.size(), 1u);
}

TEST(InMemoryBinaryStore, StoreChunksInOrderThenAssembleConcatenatesCorrectly) {
    InMemoryBinaryStore store;
    const std::vector<uint8_t> chunk0{1, 2, 3};
    const std::vector<uint8_t> chunk1{4, 5};
    const std::string uuid = store.createSlot(chunk0.size() + chunk1.size(), 2, 60s);

    store.storeChunk(uuid, 0, chunk0);
    store.storeChunk(uuid, 1, chunk1);

    EXPECT_TRUE(store.isComplete(uuid));
    EXPECT_EQ(store.assemble(uuid), (std::vector<uint8_t>{1, 2, 3, 4, 5}));
}

TEST(InMemoryBinaryStore, StoreChunksOutOfOrderThenAssembleConcatenatesInIndexOrder) {
    InMemoryBinaryStore store;
    const std::vector<uint8_t> chunk0{1, 2};
    const std::vector<uint8_t> chunk1{3, 4};
    const std::vector<uint8_t> chunk2{5, 6};
    const std::string uuid = store.createSlot(6, 3, 60s);

    store.storeChunk(uuid, 2, chunk2);
    store.storeChunk(uuid, 0, chunk0);
    store.storeChunk(uuid, 1, chunk1);

    ASSERT_TRUE(store.isComplete(uuid));
    EXPECT_EQ(store.assemble(uuid), (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST(InMemoryBinaryStore, ReadRangeCrossesChunksAndRejectsPastEof) {
    InMemoryBinaryStore store;
    const std::string uuid = store.createSlot(6, 3, 60s);
    store.storeChunk(uuid, 0, {0, 1});
    store.storeChunk(uuid, 1, {2, 3, 4});
    store.storeChunk(uuid, 2, {5});

    EXPECT_EQ(store.readRange(uuid, 1, 4), (std::vector<uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(store.readRange(uuid, 5, 1), (std::vector<uint8_t>{5}));
    EXPECT_TRUE(store.readRange(uuid, 6, 0).empty());

    // A short read past the tail must fail loudly rather than silently
    // truncate: GetChunkResponse carries no length field, so a clamped
    // result is indistinguishable on the wire from a legitimate last chunk.
    EXPECT_THROW((void)store.readRange(uuid, 5, 10), std::out_of_range);
    EXPECT_THROW((void)store.readRange(uuid, 99, 1), std::out_of_range);
    // offset + length would wrap past SIZE_MAX if computed by addition; the
    // guard is written as length > binarySize - offset specifically to catch
    // this without wrapping.
    EXPECT_THROW((void)store.readRange(uuid, 1, std::numeric_limits<std::size_t>::max()),
                 std::out_of_range);
}

TEST(InMemoryBinaryStore, StoreChunkIsIdempotentAndOverwritesPreviousPayload) {
    InMemoryBinaryStore store;
    const std::string uuid = store.createSlot(2, 1, 60s);

    store.storeChunk(uuid, 0, {9, 9});
    store.storeChunk(uuid, 0, {1, 2});

    ASSERT_TRUE(store.isComplete(uuid));
    EXPECT_EQ(store.assemble(uuid), (std::vector<uint8_t>{1, 2}));
}

TEST(InMemoryBinaryStore, LookupIgnoresUuidCase) {
    // Part A p90: BinaryTransferUUID comparison MUST ignore case. The server
    // issues a lower-case UUID; a client may legally echo an upper-case
    // RFC-4122 variant, and slots_'s CaseInsensitiveLess must still find it.
    InMemoryBinaryStore store;
    const std::string uuid = store.createSlot(3, 1, 60s);
    std::string upper = uuid;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    EXPECT_TRUE(store.contains(upper));
    EXPECT_EQ(store.binarySize(upper), 3u);
}

TEST(InMemoryBinaryStore, UnboundedByDefaultAcceptsLargeBinarySize) {
    // maxBytes_ == 0 (the default) pins pre-S70 behaviour: no byte budget.
    InMemoryBinaryStore store;
    const std::size_t large = 100u * 1024u * 1024u;

    const std::string uuid = store.createSlot(large, 1, 60s);

    EXPECT_FALSE(uuid.empty());
}

TEST(InMemoryBinaryStore, CreateSlotWithinByteBudgetSucceeds) {
    InMemoryBinaryStore store{100};

    const std::string uuid = store.createSlot(50, 1, 60s);

    EXPECT_FALSE(uuid.empty());
}

TEST(InMemoryBinaryStore, RemoveDeletesSlotAndDecrementsSize) {
    InMemoryBinaryStore store;
    const std::string uuid = store.createSlot(1, 1, 60s);
    ASSERT_TRUE(store.contains(uuid));

    store.remove(uuid);

    EXPECT_FALSE(store.contains(uuid));
    EXPECT_EQ(store.size(), 0u);
}

// ---------------------------------------------------------------------------
// False (negative/rejection) paths — all CAUGHT (explicit throws in BinaryStore.cc)
// ---------------------------------------------------------------------------

TEST(InMemoryBinaryStore, StoreChunkWithUnknownUuidThrowsOutOfRange) {
    InMemoryBinaryStore store;

    EXPECT_THROW(store.storeChunk("not-a-known-uuid", 0, {1}), std::out_of_range);
}

TEST(InMemoryBinaryStore, ContainsIsFalseForUnknownUuidEvenWithCaseFolding) {
    // Case folding must not turn a lookup miss into a false hit.
    InMemoryBinaryStore store;

    EXPECT_FALSE(store.contains("no-such-uuid"));
}

TEST(InMemoryBinaryStore, StoreChunkWithIndexAtChunkCountThrowsOutOfRange) {
    InMemoryBinaryStore store;
    const std::string uuid = store.createSlot(1, 1, 60s);

    EXPECT_THROW(store.storeChunk(uuid, 1, {1}), std::out_of_range);
}

TEST(InMemoryBinaryStore, IsCompleteWithUnknownUuidThrowsOutOfRange) {
    InMemoryBinaryStore store;

    EXPECT_THROW((void)store.isComplete("not-a-known-uuid"), std::out_of_range);
    EXPECT_THROW((void)store.readRange("not-a-known-uuid", 0, 1), std::out_of_range);
}

TEST(InMemoryBinaryStore, AssembleOnIncompleteSlotThrowsLogicError) {
    InMemoryBinaryStore store;
    // chunkCount = 2, but only index 0 is ever stored.
    const std::string uuid = store.createSlot(10, 2, 60s);
    store.storeChunk(uuid, 0, {1, 2, 3});

    EXPECT_THROW(store.assemble(uuid), std::logic_error);
    EXPECT_THROW(store.readRange(uuid, 0, 1), std::logic_error);
}

TEST(InMemoryBinaryStore, AssembleWithDeclaredSizeMismatchThrowsLogicError) {
    InMemoryBinaryStore store;
    // All chunks received (isComplete() would be true), but the assembled
    // byte count disagrees with the binarySize declared at createSlot() —
    // a distinct throw site from the incompleteness check above.
    const std::string uuid = store.createSlot(10, 1, 60s);
    store.storeChunk(uuid, 0, {1, 2, 3});

    EXPECT_THROW(store.assemble(uuid), std::logic_error);
}

TEST(InMemoryBinaryStore, RemoveWithUnknownUuidThrowsOutOfRange) {
    InMemoryBinaryStore store;

    EXPECT_THROW(store.remove("not-a-known-uuid"), std::out_of_range);
}

TEST(InMemoryBinaryStore, CreateSlotOverByteBudgetThrows) {
    // Part B p58: the byte budget must be enforced before the UUID is issued.
    InMemoryBinaryStore store{100};

    EXPECT_THROW((void)store.createSlot(200, 1, 60s), std::invalid_argument);
}

}  // namespace
