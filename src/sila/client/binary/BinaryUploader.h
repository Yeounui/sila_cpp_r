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

class BinaryUploader {
public:
    // channel: gRPC channel to the server
    // maxRetries, maxBackoff: from ClientConfig
    // injector: SiLA Client Metadata attached to every RPC this uploader
    //   issues, the access token included. Null means "attach nothing", which
    //   is what an ungated server needs and what every existing caller gets.
    //   Non-owning: the injector must outlive this uploader.
    BinaryUploader(std::shared_ptr<grpc::Channel> channel,
                   int maxRetries = 3,
                   std::chrono::seconds maxBackoff = std::chrono::seconds{60},
                   MetadataInjector* injector = nullptr);

    // Upload data for a given parameter.
    // parameterFqi: fully qualified identifier of the Binary parameter
    // data: full binary content
    // chunkSize: bytes per chunk (default ~2 MiB minus overhead margin)
    // Returns the BinaryTransferUUID to embed in the command parameter.
    // Throws std::runtime_error on unrecoverable failure.
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
