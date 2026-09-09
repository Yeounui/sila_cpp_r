// BinaryUploader.h — Client-side binary upload with resume (architecture.md §4.6)
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include <grpcpp/channel.h>

namespace sila2 {

class MetadataInjector;

/// Client side of @ref gl_binary_transfer "Binary Transfer" uploads: splits a
/// large parameter value into chunks, sends them to the server, and retries
/// transient stream breaks, resuming from the last acknowledged chunk. A
/// caller needs this only when a parameter value exceeds the 2 MiB inline
/// threshold, in place of embedding the value directly in the command
/// parameter.
class BinaryUploader {
public:
    /// Wraps `channel` for uploads, retrying up to `maxRetries` times (with
    /// `maxBackoff` as the exponential-backoff ceiling) on a broken stream.
    /// @param channel gRPC channel to the server.
    /// @param maxRetries Attempts before giving up on a broken stream.
    /// @param maxBackoff Ceiling for the exponential retry delay.
    /// @param injector @ref gl_sila_client_metadata "SiLA Client Metadata"
    /// attached to every RPC this uploader issues, the access token
    /// included. Null means "attach nothing", which is what an ungated
    /// server needs. Non-owning: must outlive this uploader.
    BinaryUploader(std::shared_ptr<grpc::Channel> channel,
                   int maxRetries = 3,
                   std::chrono::seconds maxBackoff = std::chrono::seconds{60},
                   MetadataInjector* injector = nullptr);

    /// Uploads `data` as the value of the Binary parameter identified by
    /// `parameterFqi`, in chunks of at most `chunkSize` bytes (clamped to the
    /// 2 MiB @ref gl_binary_transfer "Binary Transfer" ceiling), retrying up
    /// to `maxRetries` times (with exponential backoff) on a broken stream --
    /// resuming from the last chunk the server acknowledged, or starting a
    /// fresh upload if the server reports the transfer UUID as no longer
    /// valid.
    /// @throws std::runtime_error if the retry budget is exhausted.
    /// @return the BinaryTransferUUID to embed in the command parameter in
    /// place of the value itself.
    [[nodiscard("caller needs the BinaryTransferUUID")]]
    std::string upload(const std::string& parameterFqi,
                      const std::string& data,
                      std::size_t chunkSize = 2 * 1024 * 1024 - 1024);

private:
    std::string createBinary(const std::string& parameterFqi,
                            uint64_t binarySize, uint32_t chunkCount);
    void uploadChunks(const std::string& uuid, const std::string& data,
                     std::size_t chunkSize, uint32_t chunkCount);

    std::shared_ptr<grpc::Channel> channel_;
    int maxRetries_;
    std::chrono::seconds maxBackoff_;
    MetadataInjector* injector_;

    // Indices of chunks successfully ack'd by server; used for resume.
    std::set<uint32_t> ackedIndices_;
};

}  // namespace sila2
