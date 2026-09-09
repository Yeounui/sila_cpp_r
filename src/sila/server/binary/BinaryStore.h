// BinaryStore.h — chunk store interface for Binary Transfer (architecture.md §3.5)
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <sila/common/util/PeriodicGC.h>

namespace sila2 {

/// Holds the chunks of an in-flight @ref gl_binary_transfer "Binary Transfer"
/// from the moment a slot is created until the binary is assembled and
/// removed or its Lifetime of Binary expires.
///
/// Pure virtual interface for SiLA 2 Binary Transfer chunk stores
/// (architecture.md §3.5). Concrete implementations (InMemoryBinaryStore,
/// FileSpoolBinaryStore, HybridBinaryStore) hold the actual storage; this
/// base class owns only the periodic GC thread, which all implementations
/// reuse identically. Installed by
/// sila2::SiLAServerBase::Builder::WithBinaryTransfer().
///
/// Thread safety: implementations must synchronise their own state.
/// @see BinaryUploadService, BinaryDownloadService
class BinaryStore {
public:
    virtual ~BinaryStore();

    /// Create a slot for incoming chunks and return its generated UUID.
    /// @param lifetime Time until the slot expires and becomes eligible for GC.
    ///                 Zero means no automatic expiry.
    /// @throws std::invalid_argument if an implementation cannot guarantee
    ///         the required space is available (Part B p58: the SiLA Server
    ///         MUST validate available space before issuing the UUID).
    [[nodiscard("caller expects the generated BinaryTransferUUID")]]
    virtual std::string createSlot(std::size_t binarySize, std::size_t chunkCount,
                                   std::chrono::seconds lifetime) = 0;

    /// Store a chunk at the given index. Idempotent — a repeated index overwrites
    /// the previously stored payload.
    /// @throws std::out_of_range if uuid is unknown or index >= chunkCount.
    virtual void storeChunk(const std::string& uuid, std::size_t index,
                             std::vector<uint8_t> payload) = 0;

    /// @throws std::out_of_range if uuid is unknown.
    [[nodiscard("caller expects the completion status")]]
    virtual bool isComplete(const std::string& uuid) const = 0;

    /// Concatenate all chunks in index order into a single buffer.
    /// @throws std::out_of_range if uuid is unknown.
    /// @throws std::logic_error if not all chunks have been received yet.
    [[nodiscard("caller expects the assembled binary")]]
    virtual std::vector<uint8_t> assemble(const std::string& uuid) const = 0;

    /// Read a byte range from a complete binary.
    /// @throws std::out_of_range if uuid is unknown, or if offset > binarySize
    ///         or length > binarySize - offset (an empty range at
    ///         offset == binarySize is accepted) -- both transports map that
    ///         onto BINARY_DOWNLOAD_FAILED.
    /// @throws std::logic_error if not all chunks have been received yet.
    [[nodiscard("caller expects the requested binary range")]]
    virtual std::vector<uint8_t> readRange(const std::string& uuid, std::size_t offset,
                                           std::size_t length) const = 0;

    /// Reset expiry to now + lifetime. Zero means no expiry.
    /// @throws std::out_of_range if uuid is unknown.
    virtual void updateLifetime(const std::string& uuid, std::chrono::seconds lifetime) = 0;

    /// Remove the slot for @p uuid.
    /// @throws std::out_of_range if uuid is unknown.
    virtual void remove(const std::string& uuid) = 0;

    /// @throws std::out_of_range if uuid is unknown.
    [[nodiscard("caller expects the binary size")]]
    virtual std::size_t binarySize(const std::string& uuid) const = 0;

    /// @return Remaining time until the slot expires, or zero if no expiry is set.
    /// @throws std::out_of_range if uuid is unknown.
    [[nodiscard("caller expects the remaining lifetime")]]
    virtual std::chrono::seconds remainingLifetime(const std::string& uuid) const = 0;

    [[nodiscard("caller expects the presence check result")]]
    virtual bool contains(const std::string& uuid) const = 0;

    /// Remove all expired slots.
    /// @return Number of slots removed.
    virtual std::size_t removeExpired() = 0;

    [[nodiscard("caller expects the slot count")]]
    virtual std::size_t size() const = 0;

    // -- Non-virtual GC wrappers (PeriodicGC member) --------------------------
    // Both implementations reuse the same periodic-sweep thread; the callback
    // dispatches through the virtual removeExpired().

    /// Start a background thread that calls removeExpired() every @p interval.
    /// No-op if auto-GC is already running.
    void startAutoGC(std::chrono::seconds interval) { gc_.start(interval); }

    /// Stop the background GC thread. No-op if not running. Also called by the destructor.
    void stopAutoGC() { gc_.stop(); }

    [[nodiscard("caller expects the auto-GC status")]]
    bool isAutoGCRunning() const { return gc_.isRunning(); }

private:
    PeriodicGC gc_{[this] { removeExpired(); }};
};

}  // namespace sila2
