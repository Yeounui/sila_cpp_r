// MetadataInjector.h — attaches SiLA Client Metadata to outgoing gRPC
// calls as binary headers (architecture.md §3.6)
#pragma once

#include <map>
#include <mutex>
#include <string>

namespace grpc {
class ClientContext;
}  // namespace grpc

namespace sila2 {

/// Holds the @ref gl_sila_client_metadata "SiLA Client Metadata" one
/// SilaClientBase connection sends with every call, and writes it onto a
/// gRPC call's headers on request. Obtained from
/// SilaClientBase::metadataInjector(). The dynamic client
/// (sila2::dynamic::DynamicCall) applies it automatically; a caller using a
/// generated static stub directly must call apply() itself before each RPC.
class MetadataInjector {
public:
    /// Register a metadata value to be attached to every subsequent call.
    /// @param metadataFqi Fully qualified metadata identifier, e.g.
    ///     "org.silastandard/core/LockController/v1/Metadata/LockIdentifier".
    /// @param serializedValue Already-serialized protobuf bytes for this
    ///     metadata's value message.
    void set(const std::string& metadataFqi, std::string serializedValue);

    /// Stop attaching the metadata identified by metadataFqi.
    void remove(const std::string& metadataFqi);

    /// Register a raw header key/value without FQI-to-key transformation.
    void setRaw(const std::string& key, std::string value);

    /// Stop attaching the raw header identified by key.
    void removeRaw(const std::string& key);

    /// Attach every registered metadata entry to ctx as a binary header.
    /// Call this on a static stub's grpc::ClientContext before each RPC;
    /// sila2::dynamic::DynamicCall already does this for you.
    void apply(grpc::ClientContext& ctx) const;

    /// Remove all registered metadata entries.
    void clear();

    /// Derive the gRPC binary header key from a metadata FQI.
    static std::string headerKey(const std::string& metadataFqi);

private:
    // mutable: apply() is a logically const observer but still needs to
    // take the lock, since entries_ is shared with set()/remove()/clear()
    // across threads.
    mutable std::mutex mu_;
    // std::map (not unordered_map): entries are attached to ctx in a fixed,
    // reproducible order, which keeps apply()'s header sequence deterministic
    // for logging/debugging; entry count is small so ordering costs nothing.
    std::map<std::string, std::string> entries_;  // headerKey → serializedValue
};

}  // namespace sila2
