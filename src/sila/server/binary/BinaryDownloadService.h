// BinaryDownloadService.h — Binary Download gRPC service (architecture.md §3.5)
#pragma once

#include <chrono>
#include <string_view>

#include <SiLABinaryTransfer.grpc.pb.h>

#include <sila/server/transport/SilaHandler.h>

namespace sila2 {

class BinaryStore;
struct InterceptorChain;

inline constexpr std::string_view kBinaryDownloadFqi = "org.silastandard/core/BinaryDownload/v1";

/// Lets a SiLA Client fetch a binary result too large to fit inline, in
/// chunks, by its Binary Transfer UUID (@ref gl_binary_transfer). Installed
/// automatically by sila2::SilaServerBase::Builder::withBinaryTransfer(); a
/// Feature implementer never constructs or calls this class directly.
///
/// gRPC service implementation for SiLA 2 Binary Download (architecture.md §3.5).
/// Delegates lookup and chunk retrieval to a BinaryStore; this class only
/// translates gRPC request/response messages to/from BinaryStore calls.
/// @see BinaryStore, BinaryUploadService
class BinaryDownloadService final : public sila2::org::silastandard::BinaryDownload::Service {
public:
    /// @param store Chunk store backing every download; must outlive this service.
    /// @param defaultLifetime Slot lifetime applied when a request does not extend it.
    /// @param chain Optional interceptor chain applied to every RPC; nullptr skips interception.
    BinaryDownloadService(BinaryStore& store, std::chrono::seconds defaultLifetime,
                          const InterceptorChain* chain = nullptr);

    /// Serves the GetBinaryInfo RPC of SiLA Binary Download.
    grpc::Status GetBinaryInfo(grpc::ServerContext* context,
                               const sila2::org::silastandard::GetBinaryInfoRequest* request,
                               sila2::org::silastandard::GetBinaryInfoResponse* response) override;

    /// Serves the GetChunk RPC of SiLA Binary Download.
    grpc::Status GetChunk(grpc::ServerContext* context,
                          grpc::ServerReaderWriter<sila2::org::silastandard::GetChunkResponse,
                                                    sila2::org::silastandard::GetChunkRequest>* stream) override;

    /// Serves the DeleteBinary RPC of SiLA Binary Download.
    grpc::Status DeleteBinary(grpc::ServerContext* context,
                              const sila2::org::silastandard::DeleteBinaryRequest* request,
                              sila2::org::silastandard::DeleteBinaryResponse* response) override;

    /// Transport-neutral handler body shared by the gRPC GetBinaryInfo override
    /// above and the cloud transport path; reports the binary's size and
    /// remaining lifetime via store_.binarySize()/remainingLifetime().
    grpc::Status getBinaryInfo(const sila2::org::silastandard::GetBinaryInfoRequest& request,
                               CallContext& ctx,
                               ResponseSink<sila2::org::silastandard::GetBinaryInfoResponse>& sink);
    /// Transport-neutral handler body shared by the gRPC GetChunk override
    /// above and the cloud transport path; returns one byte range via
    /// store_.readRange() and renews the slot's lifetime.
    grpc::Status getChunk(const sila2::org::silastandard::GetChunkRequest& request,
                          CallContext& ctx,
                          ResponseSink<sila2::org::silastandard::GetChunkResponse>& sink);
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
