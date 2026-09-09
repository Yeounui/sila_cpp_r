// HybridBinaryStore.h — size-based routing store (architecture.md §3.5)
#pragma once

#include <sila/common/util/AsciiCase.h>
#include <sila/server/binary/FileSpoolBinaryStore.h>
#include <sila/server/binary/InMemoryBinaryStore.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace sila2 {

/// Keeps small binaries in RAM and spools large ones to disk, so callers get
/// @ref InMemoryBinaryStore speed for the common small case and
/// @ref FileSpoolBinaryStore's bounded memory use for large ones without
/// choosing between them. This is the store
/// sila2::SiLAServerBase::Builder::WithBinaryTransfer() installs.
///
/// Routes binary slots to InMemoryBinaryStore or FileSpoolBinaryStore
/// based on binarySize vs spoolThreshold (architecture.md §3.5).
/// Binaries <= threshold stay in memory; larger ones spool to disk.
///
/// Thread-safe: routing map is guarded by its own mutex; inner stores
/// provide their own synchronisation.
class HybridBinaryStore : public BinaryStore {
public:
    /// @param spoolThreshold Binaries at most this many bytes stay in memory;
    ///        larger ones spool to disk.
    /// @param tmpDir Root directory for the disk-backed store's per-slot
    ///        subdirectories; must exist and be writable, and outlive this store.
    // spoolThreshold: binaries with binarySize > this value are spooled to disk.
    // tmpDir: root directory for FileSpoolBinaryStore's per-slot subdirectories.
    HybridBinaryStore(std::size_t spoolThreshold, std::filesystem::path tmpDir);
    ~HybridBinaryStore() override;

    HybridBinaryStore(const HybridBinaryStore&) = delete;
    HybridBinaryStore& operator=(const HybridBinaryStore&) = delete;

    std::string createSlot(std::size_t binarySize, std::size_t chunkCount,
                           std::chrono::seconds lifetime) override;
    void storeChunk(const std::string& uuid, std::size_t index,
                     std::vector<uint8_t> payload) override;
    bool isComplete(const std::string& uuid) const override;
    std::vector<uint8_t> assemble(const std::string& uuid) const override;
    std::vector<uint8_t> readRange(const std::string& uuid, std::size_t offset,
                                   std::size_t length) const override;
    void updateLifetime(const std::string& uuid, std::chrono::seconds lifetime) override;
    void remove(const std::string& uuid) override;
    std::size_t binarySize(const std::string& uuid) const override;
    std::chrono::seconds remainingLifetime(const std::string& uuid) const override;
    bool contains(const std::string& uuid) const override;
    std::size_t removeExpired() override;
    std::size_t size() const override;

private:
    // Caller must hold mu_. Throws std::out_of_range if uuid is unknown.
    BinaryStore& storeFor(const std::string& uuid) const;

    std::size_t spoolThreshold_;
    InMemoryBinaryStore mem_;
    FileSpoolBinaryStore spool_;
    mutable std::mutex mu_;
    // Part A p90: BinaryTransferUUID comparison ignores case.
    std::map<std::string, BinaryStore*, util::CaseInsensitiveLess> routing_;
};

}  // namespace sila2
