// InMemoryBinaryStore.cc
#include "InMemoryBinaryStore.h"

#include <sila/common/util/uuid.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sila2 {

InMemoryBinaryStore::InMemoryBinaryStore(std::size_t maxBytes) : maxBytes_{maxBytes} {}

InMemoryBinaryStore::~InMemoryBinaryStore() {
    stopAutoGC();
}

const InMemoryBinaryStore::Slot& InMemoryBinaryStore::findOrThrow(const std::string& uuid) const {
    auto it = slots_.find(uuid);
    if (it == slots_.end()) {
        throw std::out_of_range{"BinaryStore: unknown UUID"};
    }
    return it->second;
}

InMemoryBinaryStore::Slot& InMemoryBinaryStore::findOrThrow(const std::string& uuid) {
    return const_cast<Slot&>(static_cast<const InMemoryBinaryStore*>(this)->findOrThrow(uuid));
}

std::string InMemoryBinaryStore::createSlot(std::size_t binarySize, std::size_t chunkCount,
                                            std::chrono::seconds lifetime) {
    // Guard right before the resize() that actually pays for chunkCount.
    // HybridBinaryStore::createSlot enforces the same bound before routing,
    // so callers going through it (direct gRPC, cloud transport) are covered
    // before this store is ever reached; this check stays here too since
    // InMemoryBinaryStore is public and constructible standalone, so it
    // can't rely on a caller to bound its input. Empty binaries still need
    // exactly one slot (see BinaryUploader::upload).
    std::size_t maxPlausibleChunkCount = binarySize > 0 ? binarySize : std::size_t{1};
    if (chunkCount > maxPlausibleChunkCount || chunkCount > kMaxChunkCount) {
        throw std::invalid_argument{
            "InMemoryBinaryStore: chunkCount " + std::to_string(chunkCount) +
            " exceeds bound for binarySize " + std::to_string(binarySize) +
            " (max " + std::to_string(std::min(maxPlausibleChunkCount, kMaxChunkCount)) + ")"};
    }

    // Part B p58: validate the required space before issuing the UUID, so an
    // over-budget binary is refused at CreateBinary rather than mid-upload.
    // maxBytes_ == 0 means no budget (unbounded), the default.
    if (maxBytes_ != 0 && binarySize > maxBytes_) {
        throw std::invalid_argument{
            "InMemoryBinaryStore: binarySize " + std::to_string(binarySize) +
            " exceeds byte budget " + std::to_string(maxBytes_)};
    }

    std::string uuid = util::generateUuid();

    Slot slot{};
    slot.binarySize = binarySize;
    slot.chunkCount = chunkCount;
    slot.chunks.resize(chunkCount);
    slot.lifetime = lifetime;
    slot.expiresAt = lifetime.count() == 0
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now() + lifetime;

    std::lock_guard<std::mutex> lock{mu_};
    slots_.emplace(uuid, std::move(slot));
    return uuid;
}

void InMemoryBinaryStore::storeChunk(const std::string& uuid, std::size_t index,
                              std::vector<uint8_t> payload) {
    std::lock_guard<std::mutex> lock{mu_};
    Slot& slot = findOrThrow(uuid);
    if (index >= slot.chunkCount) {
        throw std::out_of_range{"BinaryStore: chunk index out of range"};
    }
    slot.chunks[index] = std::move(payload);
    slot.receivedIndices.insert(index);

    // Renew the slot's expiry the same way updateLifetime() does, using the
    // lifetime recorded at createSlot()/updateLifetime() time — a multi-chunk
    // upload must not have its slot GC'd mid-stream while every
    // UploadChunkResponse tells the client the lifetime was just refreshed.
    slot.expiresAt = slot.lifetime.count() == 0
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now() + slot.lifetime;
}

bool InMemoryBinaryStore::isComplete(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    const Slot& slot = findOrThrow(uuid);
    return slot.receivedIndices.size() == slot.chunkCount;
}

std::vector<uint8_t> InMemoryBinaryStore::assemble(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    const Slot& slot = findOrThrow(uuid);
    if (slot.receivedIndices.size() != slot.chunkCount) {
        throw std::logic_error{"BinaryStore: not all chunks received"};
    }

    std::vector<uint8_t> assembled;
    /*  std::vector::reserve(slot.binarySize):
        reserve는 capacity만 미리 확보하고 size는 그대로 0 — 그 뒤 insert가 용량을 넘길
        때마다 발생하는 재할당(realloc) + 기존 원소 전체 복사/이동을 없앰.
        assemble()은 청크 개수만큼 insert를 반복하는데, 최종 크기(binarySize)를 이미
        알고 있으므로 재할당 없이 한 번에 채울 수 있음.
    */
    assembled.reserve(slot.binarySize);
    for (const auto& chunk : slot.chunks) {
        assembled.insert(assembled.end(), chunk.begin(), chunk.end());
    }
    if (assembled.size() != slot.binarySize) {
        throw std::logic_error{"BinaryStore: assembled size mismatch (expected " +
                                std::to_string(slot.binarySize) + ", got " +
                                std::to_string(assembled.size()) + ")"};
    }
    return assembled;
}

std::vector<uint8_t> InMemoryBinaryStore::readRange(const std::string& uuid,
                                                     std::size_t offset,
                                                     std::size_t length) const {
    std::lock_guard<std::mutex> lock{mu_};
    const Slot& slot = findOrThrow(uuid);
    if (slot.receivedIndices.size() != slot.chunkCount) {
        throw std::logic_error{"BinaryStore: not all chunks received"};
    }

    // SiLABinaryTransfer.proto:73-78 gives BINARY_DOWNLOAD_FAILED for a range
    // the binary cannot satisfy. Clamping instead returned a short payload
    // under status OK, and GetChunkResponse (:66-71) has no length field that
    // would let a client tell that apart from a legitimate final chunk.
    // Written as length > binarySize - offset rather than offset + length >
    // binarySize so the sum cannot wrap.
    if (offset > slot.binarySize || length > slot.binarySize - offset) {
        throw std::out_of_range{"BinaryStore: requested range offset " + std::to_string(offset) +
                                " length " + std::to_string(length) + " exceeds binarySize " +
                                std::to_string(slot.binarySize)};
    }
    std::vector<uint8_t> range;
    range.reserve(length);
    std::size_t chunkBegin = 0;
    for (const auto& chunk : slot.chunks) {
        if (chunk.size() > slot.binarySize - chunkBegin) {
            throw std::logic_error{"BinaryStore: assembled size mismatch (expected " +
                                   std::to_string(slot.binarySize) + ", got more)"};
        }
        const std::size_t chunkEnd = chunkBegin + chunk.size();
        const std::size_t copyBegin = std::max(offset, chunkBegin);
        const std::size_t copyEnd = std::min(offset + length, chunkEnd);
        if (copyBegin < copyEnd) {
            range.insert(range.end(), chunk.begin() + static_cast<std::ptrdiff_t>(copyBegin - chunkBegin),
                         chunk.begin() + static_cast<std::ptrdiff_t>(copyEnd - chunkBegin));
        }
        chunkBegin = chunkEnd;
    }
    if (chunkBegin != slot.binarySize) {
        throw std::logic_error{"BinaryStore: assembled size mismatch (expected " +
                               std::to_string(slot.binarySize) + ", got " +
                               std::to_string(chunkBegin) + ")"};
    }
    return range;
}

void InMemoryBinaryStore::updateLifetime(const std::string& uuid, std::chrono::seconds lifetime) {
    std::lock_guard<std::mutex> lock{mu_};
    Slot& slot = findOrThrow(uuid);
    slot.lifetime = lifetime;
    slot.expiresAt = lifetime.count() == 0
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now() + lifetime;
}

void InMemoryBinaryStore::remove(const std::string& uuid) {
    std::lock_guard<std::mutex> lock{mu_};
    findOrThrow(uuid);
    slots_.erase(uuid);
}

std::size_t InMemoryBinaryStore::binarySize(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return findOrThrow(uuid).binarySize;
}

std::chrono::seconds InMemoryBinaryStore::remainingLifetime(const std::string& uuid) const {
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

bool InMemoryBinaryStore::contains(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return slots_.find(uuid) != slots_.end();
}

std::size_t InMemoryBinaryStore::removeExpired() {
    std::lock_guard<std::mutex> lock{mu_};
    auto now = std::chrono::steady_clock::now();
    std::size_t removed = 0;
    for (auto it = slots_.begin(); it != slots_.end();) {
        bool hasExpiry = it->second.expiresAt != std::chrono::steady_clock::time_point{};
        if (hasExpiry && now >= it->second.expiresAt) {
            it = slots_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t InMemoryBinaryStore::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return slots_.size();
}

}  // namespace sila2
