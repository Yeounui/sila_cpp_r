// FileSpoolBinaryStore.cc
#include "FileSpoolBinaryStore.h"

#include <sila/common/util/uuid.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sila2 {

FileSpoolBinaryStore::FileSpoolBinaryStore(std::filesystem::path tmpDir,
                                            AvailableSpaceFn availableSpaceFn)
    : tmpDir_{std::move(tmpDir)}, availableSpaceFn_{std::move(availableSpaceFn)} {
    // create_directories is a no-op (returns false, does not throw) if the
    // path already exists, so a shared spool root can be reused across
    // multiple FileSpoolBinaryStore instances.
    std::error_code ec;
    std::filesystem::create_directories(tmpDir_, ec);
    if (ec) {
        throw std::runtime_error{"FileSpoolBinaryStore: failed to create spool directory: " +
                                  ec.message()};
    }
}

FileSpoolBinaryStore::~FileSpoolBinaryStore() {
    // Stop the GC thread before this object's state (tmpDir_, slots_) is torn
    // down — the base-class destructor that would otherwise do this runs
    // only after this derived destructor finishes, too late to prevent a
    // concurrent removeExpired() from touching a half-destroyed object.
    stopAutoGC();

    // Destructors must not throw: use the error_code overload and ignore
    // failures (e.g. a slot directory already removed by hand).
    std::error_code ec;
    for (const auto& [uuid, slot] : slots_) {
        (void)slot;
        std::filesystem::remove_all(tmpDir_ / uuid, ec);
    }
}

const FileSpoolBinaryStore::Slot& FileSpoolBinaryStore::findOrThrow(const std::string& uuid) const {
    auto it = slots_.find(uuid);
    if (it == slots_.end()) {
        throw std::out_of_range{"BinaryStore: unknown UUID"};
    }
    return it->second;
}

FileSpoolBinaryStore::Slot& FileSpoolBinaryStore::findOrThrow(const std::string& uuid) {
    return const_cast<Slot&>(static_cast<const FileSpoolBinaryStore*>(this)->findOrThrow(uuid));
}

std::filesystem::path FileSpoolBinaryStore::chunkPath(const std::string& uuid,
                                                        std::size_t index) const {
    // %04zu zero-pads up to 4 digits; chunk indices beyond 9999 still work,
    // snprintf simply widens the field instead of truncating.
    char name[24];
    std::snprintf(name, sizeof(name), "chunk-%04zu.bin", index);
    return tmpDir_ / uuid / name;
}

std::string FileSpoolBinaryStore::createSlot(std::size_t binarySize, std::size_t chunkCount,
                                              std::chrono::seconds lifetime) {
    // Part B p58: refuse a binary that will not fit on the spool volume before
    // issuing its UUID. availableSpaceFn_ is injectable for tests; empty means
    // query the real filesystem.
    std::uintmax_t available;
    if (availableSpaceFn_) {
        available = availableSpaceFn_();
    } else {
        std::error_code spaceEc;
        const auto space = std::filesystem::space(tmpDir_, spaceEc);
        // Fail closed: without a space figure the MUST-validate step cannot
        // run, so no UUID is issued. Callers map this to BINARY_UPLOAD_FAILED.
        if (spaceEc) {
            throw std::runtime_error{"FileSpoolBinaryStore: cannot query spool space: " +
                                     spaceEc.message()};
        }
        available = space.available;
    }
    if (binarySize > available) {
        throw std::invalid_argument{
            "FileSpoolBinaryStore: binarySize " + std::to_string(binarySize) +
            " exceeds available spool space " + std::to_string(available)};
    }

    std::string uuid = util::generateUuid();

    // Create the per-slot subdirectory before taking the lock: filesystem
    // I/O does not touch slots_, so there is no need to serialize it with
    // other slots' operations.
    std::error_code ec;
    std::filesystem::create_directory(tmpDir_ / uuid, ec);
    if (ec) {
        throw std::runtime_error{"FileSpoolBinaryStore: failed to create slot directory: " +
                                  ec.message()};
    }

    Slot slot{};
    slot.binarySize = binarySize;
    slot.chunkCount = chunkCount;
    slot.lifetime = lifetime;
    slot.expiresAt = lifetime.count() == 0
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now() + lifetime;

    std::lock_guard<std::mutex> lock{mu_};
    slots_.emplace(uuid, std::move(slot));
    return uuid;
}

void FileSpoolBinaryStore::storeChunk(const std::string& uuid, std::size_t index,
                                       std::vector<uint8_t> payload) {
    std::lock_guard<std::mutex> lock{mu_};
    Slot& slot = findOrThrow(uuid);
    if (index >= slot.chunkCount) {
        throw std::out_of_range{"BinaryStore: chunk index out of range"};
    }

    // Held under mu_: the lock granularity matches InMemoryBinaryStore, and
    // per-chunk writes are small enough that serializing them is acceptable.
    std::ofstream out{chunkPath(uuid, index), std::ios::binary | std::ios::trunc};
    if (!out) {
        throw std::runtime_error{"FileSpoolBinaryStore: failed to open chunk file for writing"};
    }
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
    if (!out) {
        throw std::runtime_error{"FileSpoolBinaryStore: failed to write chunk file"};
    }

    slot.chunkSizes[index] = payload.size();

    // Renew the slot's expiry the same way updateLifetime() does — see
    // InMemoryBinaryStore::storeChunk for why this must happen on every chunk.
    slot.expiresAt = slot.lifetime.count() == 0
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now() + slot.lifetime;
}

bool FileSpoolBinaryStore::isComplete(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    const Slot& slot = findOrThrow(uuid);
    return slot.chunkSizes.size() == slot.chunkCount;
}

std::vector<uint8_t> FileSpoolBinaryStore::assemble(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    const Slot& slot = findOrThrow(uuid);
    if (slot.chunkSizes.size() != slot.chunkCount) {
        throw std::logic_error{"BinaryStore: not all chunks received"};
    }

    std::vector<uint8_t> assembled;
    assembled.reserve(slot.binarySize);
    for (std::size_t index = 0; index < slot.chunkCount; ++index) {
        std::ifstream in{chunkPath(uuid, index), std::ios::binary};
        if (!in) {
            throw std::runtime_error{"FileSpoolBinaryStore: failed to open chunk file for reading"};
        }
        // Read the whole file via stream iterators rather than seeking for
        // a size first — chunk files are small, and this keeps the read
        // path free of a second syscall round-trip per chunk.
        assembled.insert(assembled.end(), std::istreambuf_iterator<char>{in},
                          std::istreambuf_iterator<char>{});
        if (!in.eof() && in.fail()) {
            throw std::runtime_error{"FileSpoolBinaryStore: failed to read chunk file"};
        }
    }
    if (assembled.size() != slot.binarySize) {
        throw std::logic_error{"BinaryStore: assembled size mismatch (expected " +
                                std::to_string(slot.binarySize) + ", got " +
                                std::to_string(assembled.size()) + ")"};
    }
    return assembled;
}

std::vector<uint8_t> FileSpoolBinaryStore::readRange(const std::string& uuid,
                                                      std::size_t offset,
                                                      std::size_t length) const {
    std::lock_guard<std::mutex> lock{mu_};
    const Slot& slot = findOrThrow(uuid);
    if (slot.chunkSizes.size() != slot.chunkCount) {
        throw std::logic_error{"BinaryStore: not all chunks received"};
    }
    // See InMemoryBinaryStore::readRange for why an out-of-range request is
    // rejected instead of clamped, and why the comparison is written as
    // length > binarySize - offset rather than offset + length > binarySize.
    if (offset > slot.binarySize || length > slot.binarySize - offset) {
        throw std::out_of_range{"BinaryStore: requested range offset " + std::to_string(offset) +
                                " length " + std::to_string(length) + " exceeds binarySize " +
                                std::to_string(slot.binarySize)};
    }
    std::vector<uint8_t> range(length);
    std::size_t chunkBegin = 0;
    std::size_t writeAt = 0;
    for (std::size_t index = 0; index < slot.chunkCount; ++index) {
        const auto sizeIt = slot.chunkSizes.find(index);
        if (sizeIt == slot.chunkSizes.end() || sizeIt->second > slot.binarySize - chunkBegin) {
            throw std::logic_error{"BinaryStore: assembled size mismatch"};
        }
        const std::size_t chunkEnd = chunkBegin + sizeIt->second;
        const std::size_t readBegin = std::max(offset, chunkBegin);
        const std::size_t readEnd = std::min(offset + length, chunkEnd);
        if (readBegin < readEnd) {
            const std::size_t bytes = readEnd - readBegin;
            const std::size_t seekOffset = readBegin - chunkBegin;
            if (bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) ||
                seekOffset > static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max())) {
                throw std::runtime_error{"FileSpoolBinaryStore: requested range is too large"};
            }
            std::ifstream in{chunkPath(uuid, index), std::ios::binary};
            if (!in) {
                throw std::runtime_error{"FileSpoolBinaryStore: failed to open chunk file for reading"};
            }
            in.seekg(static_cast<std::streamoff>(seekOffset));
            in.read(reinterpret_cast<char*>(range.data() + writeAt),
                    static_cast<std::streamsize>(bytes));
            if (in.gcount() != static_cast<std::streamsize>(bytes)) {
                throw std::runtime_error{"FileSpoolBinaryStore: failed to read chunk file"};
            }
            writeAt += bytes;
        }
        chunkBegin = chunkEnd;
    }
    if (chunkBegin != slot.binarySize || writeAt != length) {
        throw std::logic_error{"BinaryStore: assembled size mismatch"};
    }
    return range;
}

void FileSpoolBinaryStore::updateLifetime(const std::string& uuid, std::chrono::seconds lifetime) {
    std::lock_guard<std::mutex> lock{mu_};
    Slot& slot = findOrThrow(uuid);
    slot.lifetime = lifetime;
    slot.expiresAt = lifetime.count() == 0
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now() + lifetime;
}

void FileSpoolBinaryStore::remove(const std::string& uuid) {
    std::lock_guard<std::mutex> lock{mu_};
    findOrThrow(uuid);

    // error_code overload: the map erase below is what matters for
    // correctness, a stray leftover directory on disk is not fatal.
    std::error_code ec;
    std::filesystem::remove_all(tmpDir_ / uuid, ec);
    slots_.erase(uuid);
}

std::size_t FileSpoolBinaryStore::binarySize(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return findOrThrow(uuid).binarySize;
}

std::chrono::seconds FileSpoolBinaryStore::remainingLifetime(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    const Slot& slot = findOrThrow(uuid);
    if (slot.expiresAt == std::chrono::steady_clock::time_point{}) {
        return std::chrono::seconds{0};
    }
    auto remaining = slot.expiresAt - std::chrono::steady_clock::now();
    if (remaining.count() <= 0) {
        return std::chrono::seconds{0};
    }
    return std::chrono::duration_cast<std::chrono::seconds>(remaining);
}

bool FileSpoolBinaryStore::contains(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return slots_.find(uuid) != slots_.end();
}

std::size_t FileSpoolBinaryStore::removeExpired() {
    std::lock_guard<std::mutex> lock{mu_};
    auto now = std::chrono::steady_clock::now();
    std::size_t removed = 0;
    for (auto it = slots_.begin(); it != slots_.end();) {
        bool hasExpiry = it->second.expiresAt != std::chrono::steady_clock::time_point{};
        if (hasExpiry && now >= it->second.expiresAt) {
            std::error_code ec;
            std::filesystem::remove_all(tmpDir_ / it->first, ec);
            it = slots_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t FileSpoolBinaryStore::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return slots_.size();
}

}  // namespace sila2
