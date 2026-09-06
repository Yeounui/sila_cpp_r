// MetadataInjector.cc
#include <sila/client/MetadataInjector.h>

#include <sila/common/util/MetadataHeaderKey.h>

#include <grpcpp/client_context.h>

namespace sila2 {

void MetadataInjector::set(const std::string& metadataFqi, std::string serializedValue) {
    const std::string key = headerKey(metadataFqi);
    std::lock_guard<std::mutex> lock(mu_);
    entries_[key] = std::move(serializedValue);
}

void MetadataInjector::remove(const std::string& metadataFqi) {
    const std::string key = headerKey(metadataFqi);
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(key);
}

void MetadataInjector::setRaw(const std::string& key, std::string value) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[key] = std::move(value);
}

void MetadataInjector::removeRaw(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(key);
}

void MetadataInjector::apply(grpc::ClientContext& ctx) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [key, value] : entries_) {
        ctx.AddMetadata(key, value);
    }
}

void MetadataInjector::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
}

std::string MetadataInjector::headerKey(const std::string& metadataFqi) {
    // Derivation lives in MetadataHeaderKey.h so the server can compute the
    // same key without including a client header (see that file's comment).
    return metadataHeaderKey(metadataFqi);
}

}  // namespace sila2
