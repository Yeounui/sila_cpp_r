// BinaryDownloader.h — Client-side binary download with resume (architecture.md §4.6)
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/channel.h>

namespace sila2 {

class MetadataInjector;

/// Client side of @ref gl_binary_transfer "Binary Transfer" downloads:
/// fetches a large parameter or response value from the server in chunks and
/// reassembles it, retrying transient stream breaks and resuming from the
/// last successfully received byte. A caller needs this only when a value
/// crosses the 2 MiB inline threshold and arrived as a Binary Transfer UUID
/// instead of inline bytes.
class BinaryDownloader {
public:
    /// Wraps `channel` for downloads, retrying up to `maxRetries` times (with
    /// `maxBackoff` as the exponential-backoff ceiling) on a broken stream.
    /// @param channel gRPC channel to the server.
    /// @param maxRetries Attempts before giving up on a broken stream.
    /// @param maxBackoff Ceiling for the exponential retry delay.
    /// @param injector @ref gl_sila_client_metadata "SiLA Client Metadata"
    /// attached to every RPC this downloader issues, the access token
    /// included. Null means "attach nothing", which is what an ungated
    /// server needs. Non-owning: must outlive this downloader.
    BinaryDownloader(std::shared_ptr<grpc::Channel> channel,
                     int maxRetries = 3,
                     std::chrono::seconds maxBackoff = std::chrono::seconds{60},
                     MetadataInjector* injector = nullptr);

    /// Downloads the full content of the binary identified by `uuid`,
    /// fetching it in chunks and retrying up to `maxRetries` times (with
    /// exponential backoff) on a broken stream, resuming from the offset
    /// already received rather than restarting from zero.
    /// @throws std::runtime_error if the initial GetBinaryInfo call fails, if
    /// the server reports an unrecoverable Binary Transfer error (an invalid
    /// UUID or a download failure), or if the retry budget is exhausted.
    /// @return the full binary content.
    [[nodiscard("caller needs the downloaded binary content")]]
    std::string download(const std::string& uuid);

private:
    std::shared_ptr<grpc::Channel> channel_;
    int maxRetries_;
    std::chrono::seconds maxBackoff_;
    MetadataInjector* injector_;
};

}  // namespace sila2
