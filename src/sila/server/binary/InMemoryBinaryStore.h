// InMemoryBinaryStore.h — in-memory chunk store implementation (architecture.md §3.5)
#pragma once

#include <sila/common/util/AsciiCase.h>
#include <sila/server/binary/BinaryStore.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace sila2 {

/// Holds binary chunks in RAM. Fits small binaries and low-traffic servers;
/// pick @ref FileSpoolBinaryStore for large binaries that should not sit
/// entirely in memory, or @ref HybridBinaryStore to route between the two by
/// size automatically.
///
/// Thread-safe in-memory chunk store for SiLA 2 Binary Transfer (architecture.md §3.5).
/// Owns the UUID → Slot map: generates UUIDs for CreateBinary, accepts chunks by
/// index (idempotent, order-independent), and assembles the completed binary for
/// a BinaryParameterInterceptor to resolve.
///
/// Thread-safe: all public methods lock an internal mutex.
class InMemoryBinaryStore : public BinaryStore {
public:
    /// @param maxBytes Ceiling on a single binary's size in bytes.
    // maxBytes == 0 (default) means unbounded, i.e. pre-S70 behaviour: a
    // small in-RAM binary practically always fits, and no ServerConfig field
    // expresses a RAM ceiling. HybridBinaryStore routes large binaries to
    // FileSpoolBinaryStore, whose createSlot does the meaningful disk-space
    // check, so in production InMemory only ever holds small binaries.
    explicit InMemoryBinaryStore(std::size_t maxBytes = 0);

    // Calls stopAutoGC() before the Slot map is torn down, so the GC thread
    // cannot call the derived removeExpired() while this object is being
    // destroyed (base-class destructor runs after this one, too late).
    ~InMemoryBinaryStore() override;

    // Absolute ceiling on chunkCount, independent of binarySize. The
    // binarySize-proportional bound alone (chunkCount <= binarySize) still
    // lets a binarySize at HybridBinaryStore's spoolThreshold (2 MiB default)
    // request millions of one-byte chunks — each a separate std::vector<uint8_t>
    // in chunks.resize(chunkCount), so header overhead alone is a DoS lever.
    // No minimum chunk size is defined anywhere in this codebase or the SiLA
    // spec, so this picks a ceiling generous enough for any real client
    // (BinaryUploader's default chunk size is ~2 MiB, i.e. far fewer chunks
    // for any binary this store is realistically asked to hold) while
    // keeping worst-case resize() overhead bounded to a low single-digit MB.
    static constexpr std::size_t kMaxChunkCount = 65536;  ///< Upper bound on chunkCount per slot, independent of binarySize.

    /// @throws std::invalid_argument if chunkCount exceeds either binarySize
    ///         (no chunk can carry less than one byte) or kMaxChunkCount, or
    ///         if binarySize exceeds the configured byte budget (Part B p58).
    std::string createSlot(std::size_t binarySize, std::size_t chunkCount,
                           std::chrono::seconds lifetime) override;
    void storeChunk(const std::string& uuid, std::size_t index,
                     std::vector<uint8_t> payload) override;
    [[nodiscard("caller expects the completion status")]]
    bool isComplete(const std::string& uuid) const override;
    [[nodiscard("caller expects the assembled binary")]]
    std::vector<uint8_t> assemble(const std::string& uuid) const override;
    [[nodiscard("caller expects the requested binary range")]]
    std::vector<uint8_t> readRange(const std::string& uuid, std::size_t offset,
                                   std::size_t length) const override;
    void updateLifetime(const std::string& uuid, std::chrono::seconds lifetime) override;
    void remove(const std::string& uuid) override;
    [[nodiscard("caller expects the binary size")]]
    std::size_t binarySize(const std::string& uuid) const override;
    [[nodiscard("caller expects the remaining lifetime")]]
    std::chrono::seconds remainingLifetime(const std::string& uuid) const override;
    /// See BinaryStore::contains.
    [[nodiscard("caller expects the presence check result")]]
    bool contains(const std::string& uuid) const override;
    std::size_t removeExpired() override;
    /// See BinaryStore::size.
    [[nodiscard("caller expects the slot count")]]
    std::size_t size() const override;

private:
    struct Slot {
        std::size_t binarySize;
        std::size_t chunkCount;
        std::vector<std::vector<uint8_t>> chunks;

        /*  std::set<std::size_t> vs bitset/vector<bool>:
            bitset<N>은 크기가 컴파일 타임 상수라 런타임 값인 chunkCount에 못 씀.
            vector<bool>은 비트 압축 특수화라 인덱싱은 되지만 원소 참조(&elem)가 불가능하고
            순회 시 일반 bool보다 접근이 느림.
            set은 삽입이 O(log n)으로 더 느리지만, 정렬 상태 유지 + size() 비교만으로
            완료 판정(receivedIndices.size() == chunkCount)이 끝나는 여기 쓰임에는
            그 차이가 무의미함.
        */
        std::set<std::size_t> receivedIndices;

        // Remembered so storeChunk() can refresh expiresAt to "now + lifetime"
        // on every chunk, the same way updateLifetime() would — otherwise a
        // slow multi-chunk upload outlives its lifetime mid-stream even
        // though each UploadChunkResponse just told the client it was renewed.
        std::chrono::seconds lifetime{0};

        /*  std::chrono::steady_clock::time_point{}:
            기본 생성자는 내부 duration을 0으로 값 초기화 — 이 타입의 "epoch" 시각.
            steady_clock의 epoch은 표준상 미정의(보통 부팅 시각 등)라 실제 만료 시각으로
            나올 일이 없어, "수명 없음"을 나타내는 안전한 sentinel 값으로 재사용.
            CallContext::deadline_이 반대 극단인 time_point::max()를 "무제한"으로 쓰는 것과
            대응되는 관용구.
        */
        std::chrono::steady_clock::time_point expiresAt;
    };

    const Slot& findOrThrow(const std::string& uuid) const;
    Slot& findOrThrow(const std::string& uuid);

    mutable std::mutex mu_;
    // Part A p90: BinaryTransferUUID comparison ignores case.
    std::map<std::string, Slot, util::CaseInsensitiveLess> slots_;
    std::size_t maxBytes_;
};

}  // namespace sila2
