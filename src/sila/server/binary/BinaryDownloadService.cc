// BinaryDownloadService.cc — Binary Download gRPC service (architecture.md §3.5)
#include "BinaryDownloadService.h"

#include "BinaryStore.h"
#include "BinaryUtil.h"
#include <sila/common/binary/BinaryChunkLimit.h>
#include <sila/server/transport/GrpcTransport.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace sila2 {

BinaryDownloadService::BinaryDownloadService(BinaryStore& store, std::chrono::seconds defaultLifetime,
                                                 const InterceptorChain* chain)
    : store_{store}, defaultLifetime_{defaultLifetime}, chain_{chain} {}

grpc::Status BinaryDownloadService::GetBinaryInfo(
    grpc::ServerContext* context,
    const sila2::org::silastandard::GetBinaryInfoRequest* request,
    sila2::org::silastandard::GetBinaryInfoResponse* response) {
    GrpcUnaryResponseSink<sila2::org::silastandard::GetBinaryInfoResponse> sink(response);
    grpc::Status status;
    dispatchToHandler(context, *request, sink,
        [this, &status](const auto& req, auto& ctx, auto& out) {
            status = getBinaryInfo(req, ctx, out);
        },
        chain_, kBinaryDownloadFqi, response);
    return sink.status().ok() ? status : sink.status();
}

grpc::Status BinaryDownloadService::getBinaryInfo(
    const sila2::org::silastandard::GetBinaryInfoRequest& request, CallContext&,
    ResponseSink<sila2::org::silastandard::GetBinaryInfoResponse>& sink) {
    const std::string& uuid = request.binarytransferuuid();

    if (!store_.contains(uuid)) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID,
            "Unknown binaryTransferUUID: " + uuid);
    }

    std::size_t size = store_.binarySize(uuid);
    std::chrono::seconds lifetime = store_.remainingLifetime(uuid);

    sila2::org::silastandard::GetBinaryInfoResponse response;
    response.set_binarysize(size);
    response.mutable_lifetimeofbinary()->set_seconds(lifetime.count());
    sink.send(response);
    sink.finish();
    return grpc::Status::OK;
}

grpc::Status BinaryDownloadService::GetChunk(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<sila2::org::silastandard::GetChunkResponse,
                              sila2::org::silastandard::GetChunkRequest>* stream) {
    using Request = sila2::org::silastandard::GetChunkRequest;
    using Response = sila2::org::silastandard::GetChunkResponse;
    GrpcStreamResponseSink<Response, grpc::ServerReaderWriter<Response, Request>> sink(stream);
    sila2::org::silastandard::GetChunkRequest request;
    while (stream->Read(&request)) {
        grpc::Status status;
        dispatchToHandler(context, request, sink,
            [this, &status](const auto& req, auto& ctx, auto& out) {
                status = getChunk(req, ctx, out);
            },
            chain_, kBinaryDownloadFqi);
        if (!sink.status().ok()) return sink.status();
        if (!status.ok()) return status;
    }
    return sink.status();
}

grpc::Status BinaryDownloadService::getChunk(
    const sila2::org::silastandard::GetChunkRequest& request, CallContext&,
    ResponseSink<sila2::org::silastandard::GetChunkResponse>& sink) {
    const std::string& uuid = request.binarytransferuuid();
    std::size_t offset = request.offset();
    std::size_t length = request.length();

    if (!store_.contains(uuid)) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID,
            "Unknown binaryTransferUUID: " + uuid);
    }

    // Part B p56: a requested chunk length above 2 MiB would return a
    // non-conformant chunk. Mirror the cloud transport (kGetChunkRequest).
    if (length > binary::kMaxBinaryChunkSize) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::BINARY_DOWNLOAD_FAILED,
            "Requested chunk length exceeds the 2 MiB ceiling: " + std::to_string(length));
    }

    std::vector<uint8_t> payload;
    try {
        payload = store_.readRange(uuid, offset, length);
    } catch (const std::exception& e) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::BINARY_DOWNLOAD_FAILED, e.what());
    }

    store_.updateLifetime(uuid, defaultLifetime_);

    sila2::org::silastandard::GetChunkResponse response;
    response.set_binarytransferuuid(uuid);
    response.set_offset(offset);
    response.set_payload(payload.data(), payload.size());
    response.mutable_lifetimeofbinary()->set_seconds(store_.remainingLifetime(uuid).count());
    sink.send(response);
    sink.finish();
    return grpc::Status::OK;
}

grpc::Status BinaryDownloadService::DeleteBinary(
    grpc::ServerContext* context, const sila2::org::silastandard::DeleteBinaryRequest* request,
    sila2::org::silastandard::DeleteBinaryResponse* response) {
    GrpcUnaryResponseSink<sila2::org::silastandard::DeleteBinaryResponse> sink(response);
    grpc::Status status;
    dispatchToHandler(context, *request, sink,
        [this, &status](const auto& req, auto& ctx, auto& out) {
            status = deleteBinary(req, ctx, out);
        },
        chain_, kBinaryDownloadFqi, response);
    return sink.status().ok() ? status : sink.status();
}

grpc::Status BinaryDownloadService::deleteBinary(
    const sila2::org::silastandard::DeleteBinaryRequest& request, CallContext&,
    ResponseSink<sila2::org::silastandard::DeleteBinaryResponse>& sink) {
    const auto status = deleteBinarySlot(store_, request.binarytransferuuid());
    if (status.ok()) {
        sink.send(sila2::org::silastandard::DeleteBinaryResponse{});
        sink.finish();
    }
    return status;
}
}  // namespace sila2
