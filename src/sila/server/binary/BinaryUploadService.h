// BinaryUploadService.h — Binary Upload gRPC service (architecture.md §3.5)
#pragma once

#include <chrono>
#include <string_view>

#include <SiLABinaryTransfer.grpc.pb.h>

#include <sila/server/transport/SilaHandler.h>

namespace sila2 {

class BinaryStore;
struct InterceptorChain;

// S14 option B (defence in depth): UploadChunk and DeleteBinary now gate on
// this feature-level FQI too, mirroring kBinaryDownloadFqi
// (BinaryDownloadService.h:16) — see the comment at those two dispatchToHandler
// call sites in BinaryUploadService.cc for why CreateBinary keeps gating on
// parameterIdentifier instead.
inline constexpr std::string_view kBinaryUploadFqi = "org.silastandard/core/BinaryUpload/v1";

/// Lets a SiLA Client upload a binary parameter too large to fit inline, in
/// chunks, and get back the Binary Transfer UUID
/// (@ref gl_binary_transfer) to reference it in a command call. Installed
/// automatically by sila2::SilaServerBase::Builder::withBinaryTransfer(); a
/// Feature implementer never constructs or calls this class directly, and
/// never sees the UUID itself -- BinaryParameterInterceptor resolves it to
/// bytes before the handler runs.
///
/// gRPC service implementation for SiLA 2 Binary Upload (architecture.md §3.5).
/// Delegates chunk storage and assembly to a BinaryStore; this class only
/// translates gRPC request/response messages to/from BinaryStore calls.
/// @see BinaryStore, BinaryDownloadService, sila2::binary::resolveBinaryParameters
class BinaryUploadService final : public sila2::org::silastandard::BinaryUpload::Service {
public:
    /// @param store Chunk store backing every upload; must outlive this service.
    /// @param defaultLifetime Slot lifetime applied when a request does not extend it.
    /// @param chain Optional interceptor chain applied to every RPC; nullptr skips interception.
    BinaryUploadService(BinaryStore& store, std::chrono::seconds defaultLifetime,
                        const InterceptorChain* chain = nullptr);

    /// Serves the CreateBinary RPC of SiLA Binary Upload.
    grpc::Status CreateBinary(grpc::ServerContext* context,
                              const sila2::org::silastandard::CreateBinaryRequest* request,
                              sila2::org::silastandard::CreateBinaryResponse* response) override;

    /// Serves the UploadChunk RPC of SiLA Binary Upload.
    grpc::Status UploadChunk(grpc::ServerContext* context,
                             grpc::ServerReaderWriter<sila2::org::silastandard::UploadChunkResponse,
                                                      sila2::org::silastandard::UploadChunkRequest>* stream) override;

    /// Serves the DeleteBinary RPC of SiLA Binary Upload.
    grpc::Status DeleteBinary(grpc::ServerContext* context,
                              const sila2::org::silastandard::DeleteBinaryRequest* request,
                              sila2::org::silastandard::DeleteBinaryResponse* response) override;

    /// Transport-neutral handler body shared by the gRPC CreateBinary override
    /// above and the cloud transport path; creates a slot via store_.createSlot()
    /// and returns its Binary Transfer UUID.
    grpc::Status createBinary(const sila2::org::silastandard::CreateBinaryRequest& request,
                              CallContext& ctx,
                              ResponseSink<sila2::org::silastandard::CreateBinaryResponse>& sink);
    /// Transport-neutral handler body shared by the gRPC UploadChunk override
    /// above and the cloud transport path; stores one Binary Chunk via
    /// store_.storeChunk().
    grpc::Status uploadChunk(const sila2::org::silastandard::UploadChunkRequest& request,
                             CallContext& ctx,
                             ResponseSink<sila2::org::silastandard::UploadChunkResponse>& sink);
    /// Transport-neutral handler body shared by the gRPC DeleteBinary override
    /// above and the cloud transport path; removes the slot via store_.remove().
    grpc::Status deleteBinary(const sila2::org::silastandard::DeleteBinaryRequest& request,
                              CallContext& ctx,
                              ResponseSink<sila2::org::silastandard::DeleteBinaryResponse>& sink);

private:
    // Reference, not owned: BinaryStore outlives this service (owned by the server setup).
    BinaryStore& store_;
    std::chrono::seconds defaultLifetime_;
    const InterceptorChain* chain_;
};

}  // namespace sila2
