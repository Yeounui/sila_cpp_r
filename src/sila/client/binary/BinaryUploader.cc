// BinaryUploader.cc — Client-side binary upload with resume (architecture.md §4.6)
#include "BinaryUploader.h"
#include "BinaryRetry.h"

#include <sila/client/MetadataInjector.h>
#include <sila/common/binary/BinaryChunkLimit.h>
#include <sila/common/error/SiLAErrorException.h>

#include <SiLABinaryTransfer.grpc.pb.h>

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace sila2 {

namespace {

bool isInvalidUuid(const grpc::Status& status) {
    auto type = parseBinaryTransferError(status);
    return type == sila2::org::silastandard::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID;
}

// Carries the isInvalidUuid() verdict out of uploadChunks()/resumeUpload() so
// upload()'s retry loop can tell "server dropped this UUID, start over" apart
// from an ordinary transient stream break.
struct UploadStreamError : std::runtime_error {
    UploadStreamError(const std::string& message, bool invalidUuidArg)
        : std::runtime_error{message}, invalidUuid{invalidUuidArg} {}
    bool invalidUuid;
};

}  // namespace

BinaryUploader::BinaryUploader(std::shared_ptr<grpc::Channel> channel,
                                int maxRetries,
                                std::chrono::seconds maxBackoff,
                                MetadataInjector* injector)
    : channel_{std::move(channel)}, maxRetries_{maxRetries}, maxBackoff_{maxBackoff},
      injector_{injector} {}

std::string BinaryUploader::upload(const std::string& parameterFqi,
                                   const std::string& data,
                                   std::size_t chunkSize) {
    // Part B p56: a Binary Chunk MUST not exceed 2 MiB. Clamp a caller-supplied
    // override down to the shared ceiling (the public default is already below
    // it); the server rejects an oversized chunk too (S63), this stops the
    // client from ever producing one.
    chunkSize = std::min(chunkSize, binary::kMaxBinaryChunkSize);

    // Server expects at least one chunk slot even for an empty binary.
    uint32_t chunkCount = data.empty()
        ? 1
        : static_cast<uint32_t>((data.size() + chunkSize - 1) / chunkSize);

    std::string uuid = createBinary(parameterFqi, data.size(), chunkCount);
    ackedIndices_.clear();

    try {
        uploadChunks(uuid, data, chunkSize, chunkCount);
        return uuid;
    } catch (const std::exception&) {
        // Fall through to the retry loop below.
    }

    for (int attempt = 0; attempt < maxRetries_; ++attempt) {
        std::this_thread::sleep_for(backoffDelay(attempt, maxBackoff_));
        try {
            uploadChunks(uuid, data, chunkSize, chunkCount);
            return uuid;
        } catch (const UploadStreamError& error) {
            if (error.invalidUuid) {
                // Server no longer knows this UUID (e.g. lifetime expired):
                // start over with a fresh CreateBinary but keep the attempt
                // counter running so persistent INVALID_UUID exhausts the
                // retry budget instead of looping forever.
                uuid = createBinary(parameterFqi, data.size(), chunkCount);
                ackedIndices_.clear();
            }
        } catch (const std::exception&) {
            // Ordinary transient failure: just retry the resume.
        }
    }

    throw std::runtime_error{"BinaryUploader: upload of " + parameterFqi +
                              " failed after " + std::to_string(maxRetries_) +
                              " retries"};
}

std::string BinaryUploader::createBinary(const std::string& parameterFqi,
                                         uint64_t binarySize, uint32_t chunkCount) {
    auto stub = sila2::org::silastandard::BinaryUpload::NewStub(channel_);

    sila2::org::silastandard::CreateBinaryRequest request;
    request.set_binarysize(binarySize);
    request.set_chunkcount(chunkCount);
    request.set_parameteridentifier(parameterFqi);

    sila2::org::silastandard::CreateBinaryResponse response;
    grpc::ClientContext context;
    // Unlike DynamicCall.cc, no SiLAService skip is needed here: these RPCs
    // are BinaryUpload/BinaryDownload, never SiLAService, so the Part A rule
    // that bans metadata on SiLAService calls cannot apply.
    if (injector_) {
        injector_->apply(context);
    }
    grpc::Status status = stub->CreateBinary(&context, request, &response);
    if (!status.ok()) {
        throw std::runtime_error{"BinaryUploader::CreateBinary failed: " +
                                  error::messageFromGrpcStatus(status)};
    }
    return response.binarytransferuuid();
}

void BinaryUploader::uploadChunks(const std::string& uuid, const std::string& data,
                                  std::size_t chunkSize, uint32_t chunkCount) {
    auto stub = sila2::org::silastandard::BinaryUpload::NewStub(channel_);
    grpc::ClientContext context;
    if (injector_) {
        injector_->apply(context);
    }
    auto stream = stub->UploadChunk(&context);

    for (uint32_t index = 0; index < chunkCount; ++index) {
        // Resume support: chunks already confirmed by a prior stream attempt
        // are not resent.
        if (ackedIndices_.count(index) > 0) continue;

        sila2::org::silastandard::UploadChunkRequest request;
        request.set_binarytransferuuid(uuid);
        request.set_chunkindex(index);
        request.set_payload(data.substr(index * chunkSize, chunkSize));

        if (!stream->Write(request)) {
            grpc::Status status = stream->Finish();
            throw UploadStreamError{"BinaryUploader: stream write failed at chunk " +
                                     std::to_string(index) + ": " + error::messageFromGrpcStatus(status),
                                     isInvalidUuid(status)};
        }

        sila2::org::silastandard::UploadChunkResponse response;
        if (!stream->Read(&response)) {
            grpc::Status status = stream->Finish();
            throw UploadStreamError{"BinaryUploader: stream read failed at chunk " +
                                     std::to_string(index) + ": " + error::messageFromGrpcStatus(status),
                                     isInvalidUuid(status)};
        }
        if (response.chunkindex() != index) {
            throw UploadStreamError{
                "BinaryUploader: server acked chunk " +
                std::to_string(response.chunkindex()) + " but expected " +
                std::to_string(index), false};
        }
        ackedIndices_.insert(index);
    }

    stream->WritesDone();
    grpc::Status status = stream->Finish();
    if (!status.ok()) {
        throw UploadStreamError{"BinaryUploader::UploadChunk failed: " +
                                 error::messageFromGrpcStatus(status), isInvalidUuid(status)};
    }
}


}  // namespace sila2
