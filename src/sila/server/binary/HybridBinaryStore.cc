// HybridBinaryStore.cc
#include "HybridBinaryStore.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sila2 {

HybridBinaryStore::HybridBinaryStore(std::size_t spoolThreshold,
                                       std::filesystem::path tmpDir)
    : spoolThreshold_{spoolThreshold}, spool_{std::move(tmpDir)} {}

HybridBinaryStore::~HybridBinaryStore() {
    // Stop our GC thread before inner stores are destroyed — the base-class
    // destructor runs after this one, too late.
    stopAutoGC();
}

BinaryStore& HybridBinaryStore::storeFor(const std::string& uuid) const {
    auto it = routing_.find(uuid);
    if (it == routing_.end()) {
        throw std::out_of_range{"HybridBinaryStore: unknown UUID: " + uuid};
    }
    return *it->second;
}

std::string HybridBinaryStore::createSlot(std::size_t binarySize, std::size_t chunkCount,
                                          std::chrono::seconds lifetime) {
    // InMemoryBinaryStore::createSlot bounds chunkCount, but that guard only
    // runs for binaries routed to mem_. FileSpoolBinaryStore::createSlot has
    // no chunkCount-proportional allocation of its own and never validates
    // chunkCount, so a binarySize above spoolThreshold_ would otherwise let a
    // client declare an arbitrary chunkCount (e.g. SIZE_MAX). Enforcing the
    // same bound here, before routing, closes that gap for both destinations.
    std::size_t maxPlausibleChunkCount = binarySize > 0 ? binarySize : std::size_t{1};
    if (chunkCount > maxPlausibleChunkCount || chunkCount > InMemoryBinaryStore::kMaxChunkCount) {
        throw std::invalid_argument{
            "HybridBinaryStore: chunkCount " + std::to_string(chunkCount) +
            " exceeds bound for binarySize " + std::to_string(binarySize) + " (max " +
            std::to_string(std::min(maxPlausibleChunkCount, InMemoryBinaryStore::kMaxChunkCount)) +
            ")"};
    }

    std::lock_guard<std::mutex> lock{mu_};
    // Route by declared binary size: large binaries go to disk.
    BinaryStore* target = (binarySize > spoolThreshold_)
        ? static_cast<BinaryStore*>(&spool_)
        : static_cast<BinaryStore*>(&mem_);
    auto uuid = target->createSlot(binarySize, chunkCount, lifetime);
    routing_[uuid] = target;
    return uuid;
}

void HybridBinaryStore::storeChunk(const std::string& uuid, std::size_t index,
                                    std::vector<uint8_t> payload) {
    std::lock_guard<std::mutex> lock{mu_};
    storeFor(uuid).storeChunk(uuid, index, std::move(payload));
}

bool HybridBinaryStore::isComplete(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return storeFor(uuid).isComplete(uuid);
}

std::vector<uint8_t> HybridBinaryStore::assemble(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return storeFor(uuid).assemble(uuid);
}

std::vector<uint8_t> HybridBinaryStore::readRange(const std::string& uuid,
                                                   std::size_t offset,
                                                   std::size_t length) const {
    BinaryStore* target;
    {
        std::lock_guard<std::mutex> lock{mu_};
        target = &storeFor(uuid);
    }
    return target->readRange(uuid, offset, length);
}

void HybridBinaryStore::updateLifetime(const std::string& uuid,
                                        std::chrono::seconds lifetime) {
    std::lock_guard<std::mutex> lock{mu_};
    storeFor(uuid).updateLifetime(uuid, lifetime);
}

void HybridBinaryStore::remove(const std::string& uuid) {
    std::lock_guard<std::mutex> lock{mu_};
    storeFor(uuid).remove(uuid);
    routing_.erase(uuid);
}

std::size_t HybridBinaryStore::binarySize(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return storeFor(uuid).binarySize(uuid);
}

std::chrono::seconds HybridBinaryStore::remainingLifetime(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return storeFor(uuid).remainingLifetime(uuid);
}

bool HybridBinaryStore::contains(const std::string& uuid) const {
    std::lock_guard<std::mutex> lock{mu_};
    return routing_.count(uuid) > 0;
}

std::size_t HybridBinaryStore::removeExpired() {
    std::lock_guard<std::mutex> lock{mu_};
    std::size_t removed = mem_.removeExpired() + spool_.removeExpired();
    // Prune routing entries whose slots were just expired by the inner stores.
    std::erase_if(routing_, [](const auto& pair) {
        return !pair.second->contains(pair.first);
    });
    return removed;
}

std::size_t HybridBinaryStore::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return mem_.size() + spool_.size();
}

}  // namespace sila2
