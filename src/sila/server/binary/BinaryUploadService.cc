// BinaryUploadService.cc — Binary Upload gRPC service (architecture.md §3.5)
#include "BinaryUploadService.h"

#include "BinaryStore.h"
#include "BinaryUtil.h"
#include <sila/common/binary/BinaryChunkLimit.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/transport/GrpcTransport.h>
#include <sila/server/transport/InterceptorChain.h>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace sila2 {

BinaryUploadService::BinaryUploadService(BinaryStore& store, std::chrono::seconds defaultLifetime,
                                           const InterceptorChain* chain)
    : store_{store}, defaultLifetime_{defaultLifetime}, chain_{chain} {}

grpc::Status BinaryUploadService::CreateBinary(
    grpc::ServerContext* context,
    const sila2::org::silastandard::CreateBinaryRequest* request,
    sila2::org::silastandard::CreateBinaryResponse* response) {
    GrpcUnaryResponseSink<sila2::org::silastandard::CreateBinaryResponse> sink(response);
    grpc::Status status;
    // CreateBinary keeps gating on the request's parameterIdentifier, not
    // kBinaryUploadFqi: per-parameter granularity is what
    // SiLABinaryTransfer.proto:24 exists for, and S13 is what makes that
    // string trustworthy (isKnownParameterFqi below). UploadChunk and
    // DeleteBinary below gate on the coarser feature-level FQI instead,
    // because the chunk stream carries no parameterIdentifier to check.
    dispatchToHandler(context, *request, sink,
        [this, &status](const auto& req, auto& ctx, auto& out) {
            status = createBinary(req, ctx, out);
        },
        chain_, request->parameteridentifier(), response);
    return sink.status().ok() ? status : sink.status();
}

grpc::Status BinaryUploadService::createBinary(
    const sila2::org::silastandard::CreateBinaryRequest& request, CallContext&,
    ResponseSink<sila2::org::silastandard::CreateBinaryResponse>& sink) {
    std::size_t binarySize = request.binarysize();
    std::size_t chunkCount = request.chunkcount();

    // ponytail: empty list means no Builder-assembled chain, so no validation.
    // A real server always registers SiLAService (SilaServerBase.cc:398) before
    // the snapshot at build()'s end, so the list is non-empty in production.
    if (chain_ && !chain_->registeredFeatureFqis.empty() &&
        !auth::isKnownParameterFqi(chain_->registeredFeatureFqis,
                                   request.parameteridentifier())) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::BINARY_UPLOAD_FAILED,
            "parameterIdentifier does not name a parameter of a registered Feature: "
                + request.parameteridentifier());
    }

    // chunkCount bounds (binarySize-proportional and the absolute
    // kMaxChunkCount ceiling) are enforced by the store, at the resize() site
    // that actually pays for an oversized chunkCount — see
    // InMemoryBinaryStore::createSlot. That single guard also covers the
    // cloud transport, which calls createSlot() directly, not through this
    // service. Translate the store's rejection into the same SiLA error
    // uploadChunk() already uses for a rejected store call below.
    std::string uuid;
    try {
        uuid = store_.createSlot(binarySize, chunkCount, defaultLifetime_);
    } catch (const std::exception& e) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::BINARY_UPLOAD_FAILED, e.what());
    }

    sila2::org::silastandard::CreateBinaryResponse response;
    response.set_binarytransferuuid(uuid);
    response.mutable_lifetimeofbinary()->set_seconds(defaultLifetime_.count());
    sink.send(response);
    sink.finish();
    return grpc::Status::OK;
}

grpc::Status BinaryUploadService::UploadChunk(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<sila2::org::silastandard::UploadChunkResponse,
                              sila2::org::silastandard::UploadChunkRequest>* stream) {
    using Request = sila2::org::silastandard::UploadChunkRequest;
    using Response = sila2::org::silastandard::UploadChunkResponse;
    GrpcStreamResponseSink<Response, grpc::ServerReaderWriter<Response, Request>> sink(stream);
    sila2::org::silastandard::UploadChunkRequest request;
    // Each UploadChunkRequest carries its own binaryTransferUUID
    // (SiLABinaryTransfer.proto:32-36), not the stream, so one stream may
    // legally interleave chunks of several transfers. Remember every UUID
    // the stream actually wrote to; a last-UUID-wins check would silently
    // skip the others.
    std::set<std::string> touchedUuids;
    while (stream->Read(&request)) {
        grpc::Status status;
        // S14 option B: gate on kBinaryUploadFqi (chain_), mirroring
        // BinaryDownloadService::GetChunk (BinaryDownloadService.cc:64-68) —
        // defence in depth alongside CreateBinary's parameterIdentifier gate.
        // Two consequences accepted per the S14 ruling: dispatchToHandler
        // copies the request once chain_->binaryStore is set
        // (GrpcTransport.h:173-176), and its dispatch log line
        // (GrpcTransport.h:187-189) fires once per chunk, not once per upload
        // — GetChunk already pays both costs on the download side.
        dispatchToHandler(context, request, sink,
            [this, &status](const auto& req, auto& ctx, auto& out) {
                status = uploadChunk(req, ctx, out);
            },
            chain_, kBinaryUploadFqi);
        if (!sink.status().ok()) return sink.status();
        if (!status.ok()) return status;
        touchedUuids.insert(request.binarytransferuuid());
    }

    // SiLABinaryTransfer.proto:23 -- a slot is usable only once all chunkCount
    // chunks have arrived. Returning OK on a short stream tells the client its
    // upload succeeded; the truncation then surfaces somewhere else entirely,
    // as BINARY_DOWNLOAD_FAILED on a later GetChunk or as a parameter resolve
    // failure on the command that references the UUID. The uploader, the only
    // party that can retry, never hears about it.
    // gRPC's sync API cannot tell a clean WritesDone from a dropped connection
    // -- Read() returns false for both -- and it does not need to: on a dropped
    // connection this status is undeliverable anyway, so gating on
    // ServerContext::IsCancelled() would change nothing the client can observe.
    for (const std::string& uuid : touchedUuids) {
        try {
            if (!store_.isComplete(uuid)) {
                return makeBinaryTransferStatus(
                    sila2::org::silastandard::BinaryTransferError::BINARY_UPLOAD_FAILED,
                    "Upload stream closed before all chunks arrived for " + uuid);
            }
        } catch (const std::out_of_range&) {
            // BinaryStore.h:38 -- isComplete throws for an unknown uuid. The
            // slot can be GC-swept (BinaryStore.h:96, 60s sweep) between the
            // last chunk and this loop, so a contains() pre-check would only
            // move the race, not close it. Nothing left to judge complete;
            // the next request on this UUID gets INVALID_BINARY_TRANSFER_UUID
            // from uploadChunk()'s own contains() check.
        }
    }
    return sink.status();
}

grpc::Status BinaryUploadService::uploadChunk(
    const sila2::org::silastandard::UploadChunkRequest& request, CallContext&,
    ResponseSink<sila2::org::silastandard::UploadChunkResponse>& sink) {
    const std::string& uuid = request.binarytransferuuid();
    std::size_t index = request.chunkindex();
    const std::string& payload = request.payload();

    if (!store_.contains(uuid)) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID,
            "Unknown binaryTransferUUID: " + uuid);
    }

    // Part B p56: a Binary Chunk MUST not exceed 2 MiB. Reject before the store,
    // mirroring the cloud transport (CloudEnvelopeRouter kUploadChunkRequest).
    if (payload.size() > binary::kMaxBinaryChunkSize) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::BINARY_UPLOAD_FAILED,
            "Binary Chunk exceeds the 2 MiB ceiling: " + std::to_string(payload.size()));
    }

    try {
        store_.storeChunk(uuid, index, std::vector<uint8_t>{payload.begin(), payload.end()});
    } catch (const std::exception& e) {
        return makeBinaryTransferStatus(
            sila2::org::silastandard::BinaryTransferError::BINARY_UPLOAD_FAILED, e.what());
    }

    sila2::org::silastandard::UploadChunkResponse response;
    response.set_binarytransferuuid(uuid);
    response.set_chunkindex(static_cast<uint32_t>(index));
    response.mutable_lifetimeofbinary()->set_seconds(defaultLifetime_.count());
    sink.send(response);
    sink.finish();
    return grpc::Status::OK;
}

grpc::Status BinaryUploadService::DeleteBinary(
    grpc::ServerContext* context, const sila2::org::silastandard::DeleteBinaryRequest* request,
    sila2::org::silastandard::DeleteBinaryResponse* response) {
    GrpcUnaryResponseSink<sila2::org::silastandard::DeleteBinaryResponse> sink(response);
    grpc::Status status;
    // S14 option B: gate on kBinaryUploadFqi (chain_), matching
    // BinaryDownloadService::DeleteBinary (BinaryDownloadService.cc:113-117)
    // argument for argument — the same store operation is no longer gated
    // differently depending on which service's DeleteBinary is called.
    dispatchToHandler(context, *request, sink,
        [this, &status](const auto& req, auto& ctx, auto& out) {
            status = deleteBinary(req, ctx, out);
        },
        chain_, kBinaryUploadFqi, response);
    return sink.status().ok() ? status : sink.status();
}

grpc::Status BinaryUploadService::deleteBinary(
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
