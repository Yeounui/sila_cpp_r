// BinaryDownloader.h — Client-side binary download with resume (architecture.md §4.6)
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/channel.h>

namespace sila2 {

class MetadataInjector;

class BinaryDownloader {
public:
    // channel: gRPC channel to the server
    // maxRetries, maxBackoff: from ClientConfig
    // injector: SiLA Client Metadata attached to every RPC this downloader
    //   issues, the access token included. Null means "attach nothing", which
    //   is what an ungated server needs and what every existing caller gets.
    //   Non-owning: the injector must outlive this downloader.
    BinaryDownloader(std::shared_ptr<grpc::Channel> channel,
                     int maxRetries = 3,
                     std::chrono::seconds maxBackoff = std::chrono::seconds{60},
                     MetadataInjector* injector = nullptr);

    // Download binary result identified by uuid.
    // Returns the full binary content.
    // Throws std::runtime_error on unrecoverable failure.
    [[nodiscard("caller needs the downloaded binary content")]]
    std::string download(const std::string& uuid);

private:
    std::shared_ptr<grpc::Channel> channel_;
    int maxRetries_;
    std::chrono::seconds maxBackoff_;
    MetadataInjector* injector_;
};

}  // namespace sila2
