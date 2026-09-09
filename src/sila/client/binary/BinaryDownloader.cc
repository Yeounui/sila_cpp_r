// BinaryDownloader.cc — Client-side binary download with resume (architecture.md §4.6)
#include "BinaryDownloader.h"
#include "BinaryRetry.h"

#include <sila/client/MetadataInjector.h>
#include <sila/common/error/SilaErrorException.h>

#include <SiLABinaryTransfer.grpc.pb.h>

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace sila2 {

namespace {

bool isFatalBinaryError(const grpc::Status& status) {
    auto type = parseBinaryTransferError(status);
    return type == sila2::org::silastandard::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID ||
           type == sila2::org::silastandard::BinaryTransferError::BINARY_DOWNLOAD_FAILED;
}

}  // namespace

BinaryDownloader::BinaryDownloader(std::shared_ptr<grpc::Channel> channel,
                                    int maxRetries,
                                    std::chrono::seconds maxBackoff,
                                    MetadataInjector* injector)
    : channel_{std::move(channel)}, maxRetries_{maxRetries}, maxBackoff_{maxBackoff},
      injector_{injector} {}

std::string BinaryDownloader::download(const std::string& uuid) {
    // Same chunk size default as BinaryUploader, kept below the 2 MiB gRPC
    // message limit minus a margin for the rest of the message fields.
    constexpr std::size_t chunkSize = 2 * 1024 * 1024 - 1024;

    auto infoStub = sila2::org::silastandard::BinaryDownload::NewStub(channel_);
    sila2::org::silastandard::GetBinaryInfoRequest infoRequest;
    infoRequest.set_binarytransferuuid(uuid);

    sila2::org::silastandard::GetBinaryInfoResponse infoResponse;
    grpc::ClientContext infoContext;
    // Unlike DynamicCall.cc, no SiLAService skip is needed here: these RPCs
    // are BinaryUpload/BinaryDownload, never SiLAService, so the Part A rule
    // that bans metadata on SiLAService calls cannot apply.
    if (injector_) {
        injector_->apply(infoContext);
    }
    grpc::Status infoStatus = infoStub->GetBinaryInfo(&infoContext, infoRequest, &infoResponse);
    if (!infoStatus.ok()) {
        throw std::runtime_error{"BinaryDownloader::GetBinaryInfo failed: " +
                                  error::messageFromGrpcStatus(infoStatus)};
    }
    uint64_t binarySize = infoResponse.binarysize();

    std::string result;
    result.reserve(binarySize);
    uint64_t offset = 0;

    // First attempt + up to maxRetries_ retries, matching BinaryUploader's
    // total attempt count of (1 + maxRetries_).
    for (int attempt = 0; attempt <= maxRetries_; ++attempt) {
        auto stub = sila2::org::silastandard::BinaryDownload::NewStub(channel_);
        grpc::ClientContext context;
        if (injector_) {
            injector_->apply(context);
        }
        auto stream = stub->GetChunk(&context);

        while (offset < binarySize) {
            sila2::org::silastandard::GetChunkRequest request;
            request.set_binarytransferuuid(uuid);
            request.set_offset(offset);
            request.set_length(static_cast<uint32_t>(std::min<uint64_t>(chunkSize, binarySize - offset)));

            if (!stream->Write(request)) break;

            sila2::org::silastandard::GetChunkResponse response;
            if (!stream->Read(&response)) break;

            if (response.binarytransferuuid() != uuid) {
                throw std::runtime_error{
                    "BinaryDownloader: server responded with UUID " +
                    response.binarytransferuuid() + " but expected " + uuid};
            }
            if (response.offset() != offset) {
                throw std::runtime_error{
                    "BinaryDownloader: server responded with offset " +
                    std::to_string(response.offset()) + " but expected " +
                    std::to_string(offset)};
            }
            result += response.payload();
            offset += response.payload().size();
        }

        stream->WritesDone();
        grpc::Status status = stream->Finish();

        if (offset >= binarySize) return result;

        // Stream broke mid-transfer: distinguish an unrecoverable server-side
        // error from an ordinary transient failure worth retrying.
        if (isFatalBinaryError(status)) {
            throw std::runtime_error{"BinaryDownloader: unrecoverable download error for " +
                                      uuid + ": " + error::messageFromGrpcStatus(status)};
        }

        if (attempt < maxRetries_) {
            std::this_thread::sleep_for(backoffDelay(attempt, maxBackoff_));
        }
        // result and offset already hold the partial progress, so the next
        // attempt resumes from where this one broke off.
    }

    throw std::runtime_error{"BinaryDownloader: download of " + uuid +
                              " failed after " + std::to_string(maxRetries_) +
                              " retries"};
}

}  // namespace sila2
