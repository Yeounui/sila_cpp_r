// FileSpoolBinaryStore.h — disk-backed chunk store implementation (architecture.md §3.5)
#pragma once

#include <sila/common/util/AsciiCase.h>
#include <sila/server/binary/BinaryStore.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sila2 {

/// Holds binary chunks as files on disk, so a large binary never has to sit
/// entirely in RAM. Pick @ref InMemoryBinaryStore for small binaries where
/// disk I/O would be wasted, or @ref HybridBinaryStore to route between the
/// two by size automatically.
///
/// Disk-backed chunk store for SiLA 2 Binary Transfer (architecture.md §3.5).
/// Stores chunk payloads as individual files under a spool directory
/// (tmpDir/uuid/chunk-NNNN.bin), keeping only metadata in memory.
/// Suited for large binaries that should not reside entirely in RAM.
///
/// Thread-safe: all public methods lock an internal mutex.
/// @throws std::runtime_error on I/O failure (in addition to the
/// std::out_of_range / std::logic_error from the base interface contract).
class FileSpoolBinaryStore : public BinaryStore {
public:
    // Returns the bytes currently free on the spool volume. Injectable so a
    // test can force the insufficient-space branch without depending on the
    // host's real free disk.
    using AvailableSpaceFn = std::function<std::uintmax_t()>;

    /// @param tmpDir Directory the store creates per-slot subdirectories under;
    ///        must exist and be writable, and outlive the store.
    // availableSpaceFn defaults to empty, meaning createSlot() queries
    // std::filesystem::space(tmpDir_) directly.
    explicit FileSpoolBinaryStore(std::filesystem::path tmpDir,
                                  AvailableSpaceFn availableSpaceFn = {});

    // Calls stopAutoGC() before the spool directory is torn down, so the GC
    // thread cannot call the derived removeExpired() while this object is
    // being destroyed (base-class destructor runs after this one, too late).
    // Then removes every remaining per-slot subdirectory under tmpDir_.
    ~FileSpoolBinaryStore() override;

    // Disallow copy/move — spool directory ownership is not transferable
    FileSpoolBinaryStore(const FileSpoolBinaryStore&) = delete;
    FileSpoolBinaryStore& operator=(const FileSpoolBinaryStore&) = delete;

    /// @throws std::invalid_argument if binarySize exceeds the space
    ///         reported available on the spool volume (Part B p58).
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
    [[nodiscard("caller expects the presence check result")]]
    bool contains(const std::string& uuid) const override;
    std::size_t removeExpired() override;
    [[nodiscard("caller expects the slot count")]]
    std::size_t size() const override;

private:
    struct Slot {
        std::size_t binarySize;
        std::size_t chunkCount;

        // No chunks vector here (unlike InMemoryBinaryStore::Slot) — chunk
        // payloads live on disk as tmpDir_/uuid/chunk-NNNN.bin files instead
        // of in this in-memory struct, which is the whole point of a
        // file-spool store for binaries too large to hold entirely in RAM.
        std::unordered_map<std::size_t, std::size_t> chunkSizes;

        // Remembered so storeChunk() can refresh expiresAt to "now + lifetime"
        // on every chunk — see InMemoryBinaryStore::Slot::lifetime for why.
        std::chrono::seconds lifetime{0};

        // See InMemoryBinaryStore::Slot::expiresAt for why the default-
        // constructed steady_clock::time_point{} is a safe "no expiry" sentinel.
        std::chrono::steady_clock::time_point expiresAt;
    };

    const Slot& findOrThrow(const std::string& uuid) const;
    Slot& findOrThrow(const std::string& uuid);

    // Returns tmpDir_ / uuid / "chunk-NNNN.bin", zero-padded so that a plain
    // directory listing already sorts chunks in index order (useful when
    // inspecting a spool directory by hand while debugging).
    std::filesystem::path chunkPath(const std::string& uuid, std::size_t index) const;

    std::filesystem::path tmpDir_;
    AvailableSpaceFn availableSpaceFn_;
    mutable std::mutex mu_;
    // Part A p90: BinaryTransferUUID comparison ignores case.
    std::map<std::string, Slot, util::CaseInsensitiveLess> slots_;
};

}  // namespace sila2
