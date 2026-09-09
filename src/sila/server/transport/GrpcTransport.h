// GrpcTransport.h — gRPC-side ResponseSink implementations and
// interceptor-chain dispatch (architecture.md §3.8)
//
// New component. gRPC-Java provides this seam natively via StreamObserver;
// gRPC-C++ does not, so this project builds its own transport adapter.
#pragma once

#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/binary/BinaryParameterInterceptor.h>
#include <sila/server/error/ErrorTransmitInterceptor.h>
#include <sila/common/error/SilaError.h>
#include <sila/server/metadata/MetadataExtractingInterceptor.h>
#include <sila/server/metadata/MetadataPolicy.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/InterceptorChain.h>
#include <sila/server/transport/ResponseSink.h>
#include <sila/server/transport/SilaHandler.h>

#include "SiLAFramework.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#ifdef SILA_ENABLE_OTEL
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#endif

namespace sila2 {

// ---------------------------------------------------------------------------
// GrpcUnaryResponseSink — ResponseSink<T> for unary (request-response) RPCs
// ---------------------------------------------------------------------------

/// Adapts gRPC's unary response model (fill a response pointer, return Status)
/// to the transport-neutral ResponseSink interface.
/// The caller retrieves status() after the handler returns and passes it back
/// to gRPC as the method's return value.
template <typename T>
class GrpcUnaryResponseSink : public ResponseSink<T> {
public:
    /// Wraps the gRPC response message this sink fills via send().
    explicit GrpcUnaryResponseSink(T* response)
        : response_{response} {}

    void send(const T& value) override {
        *response_ = value;
    }

    // gRPC completes unary RPCs when the service method returns;
    // nothing to do here.
    void finish() override {}

    void fail(const error::SilaError& error) override {
        status_ = error.toStatus();
    }

    /// @return The gRPC status to return from the service method.
    [[nodiscard("the gRPC status carries the error — dropping it silently loses the failure")]] \
    grpc::Status status() const { return status_; }

private:
    T* response_;
    grpc::Status status_ = grpc::Status::OK;
};

// ---------------------------------------------------------------------------
// GrpcStreamResponseSink — ResponseSink<T> for server-streaming RPCs
// ---------------------------------------------------------------------------

/// Adapts gRPC's ServerWriter to the transport-neutral ResponseSink interface.
/// Used by Observable Properties and Observable Command execution-info streams.
template <typename T, typename Writer = grpc::ServerWriter<T>>
class GrpcStreamResponseSink : public ResponseSink<T> {
public:
    /// Wraps the gRPC ServerWriter this sink streams values to via send().
    explicit GrpcStreamResponseSink(Writer* writer)
        : writer_{writer} {}

    void send(const T& value) override {
        writer_->Write(value);
    }

    // gRPC C++ server-streaming completion is via the Status returned from
    // the service method; nothing to do here.
    void finish() override {}

    void fail(const error::SilaError& error) override {
        status_ = error.toStatus();
    }

    /// @return The gRPC status to return from the service method.
    [[nodiscard("the gRPC status carries the error — dropping it silently loses the failure")]] \
    grpc::Status status() const { return status_; }

private:
    Writer* writer_;
    grpc::Status status_ = grpc::Status::OK;
};

// ---------------------------------------------------------------------------
// CallContext factory
// ---------------------------------------------------------------------------

/// Builds a transport-neutral CallContext from a gRPC ServerContext.
/// Converts the deadline into steady_clock and installs a cancellation probe
/// so mid-call cancellation is visible to the handler; metadata extraction is
/// handled separately by MetadataExtractingInterceptor (§3.6).
///
/// @note The returned context holds server_ctx by pointer. dispatchToHandler
///       destroys it before the service method returns; any other caller must
///       guarantee the same ordering.
std::unique_ptr<CallContext> makeCallContext(grpc::ServerContext* server_ctx);

/// Converts gRPC metadata (grpc::string_ref keys/values) to a plain
/// std::multimap that the interceptor chain expects.
inline std::multimap<std::string, std::string> buildHeaders(grpc::ServerContext* server_ctx) {
    std::multimap<std::string, std::string> headers;
    for (const auto& [key, value] : server_ctx->client_metadata()) {
        headers.emplace(
            std::string(key.data(), key.size()),
            std::string(value.data(), value.size()));
    }
    return headers;
}

// ---------------------------------------------------------------------------
// Interceptor-chain dispatch helper
// ---------------------------------------------------------------------------

/// Dispatch with interceptor chain: runs auth check, binary
/// parameter resolution before the handler, and binary result injection
/// after (unary only). All steps run inside guardHandler's error boundary.
///
/// @param unaryResponse  For unary RPCs, pass the raw response pointer so
///                       injectBinaryResults can operate on it after the
///                       handler fills it via the sink. Pass nullptr for
///                       streaming RPCs (Binary inject is skipped per spec).
template <typename Req, typename Resp, typename Handler>
void dispatchToHandler(
    grpc::ServerContext* server_ctx,
    const Req& request,
    ResponseSink<Resp>& sink,
    const Handler& handler,
    const InterceptorChain* chain,
    std::string_view fqi,
    google::protobuf::Message* unaryResponse = nullptr) {

    auto ctx = makeCallContext(server_ctx);
    auto headers = buildHeaders(server_ctx);
    MetadataExtractingInterceptor::extract(headers, *ctx);

#ifdef SILA_ENABLE_OTEL
    auto tracer = opentelemetry::trace::Provider::GetTracerProvider()
                      ->GetTracer("sila2");
    auto span = tracer->StartSpan(std::string{fqi});
    auto scope = opentelemetry::trace::Scope{span};
#endif

    error::guardHandler<Resp>(
        [&] {
            // SiLA Client Metadata admission, ahead of everything else in the
            // chain: Part A requires the required-metadata check to run before
            // parameter validation, and the SiLAService carve-out is a
            // protocol-shape verdict that must not depend on credentials --
            // SiLAService must answer any client. Nothing leaks by running it
            // first: Get_FCPAffectedByMetadata is an anonymous read
            // (AuthorizationServiceImpl.cc:45-52), so which metadata a call
            // needs is already public.
            //
            // Reads `headers` (built above), not *ctx: CallContext has no
            // whole-map accessor by design (CallContext.h:55-59).
            //
            // The if constexpr excludes an Observable Command's follow-up RPCs.
            // Part A requires an affected Command's metadata "only with the
            // Command initiation ... not with Subscribe- or
            // GetCommandExecutionInfo, Subscribe- or GetIntermediateResponse or
            // GetResponse", so gating them would reject a conformant client.
            // The request type is the only discriminator available here: the
            // generated adapter now assembles a per-RPC Command/Property FQI
            // (service_adapter.h.j2:26-28), but an Observable Command's
            // follow-ups share their Command's FQI, so only their bare
            // CommandExecutionUUID request type sets them apart
            // (meta_emitter.py:99,117,130).
            // ponytail: request-type discriminator. If codegen ever gives a
            // follow-up its own request message, pass an explicit flag down
            // from the adapter instead.
            // Generated adapters now pass the per-RPC Command/Property call FQI,
            // so the gRPC gate is granular and symmetric with the cloud path; a
            // feature-level protectedFqis entry still covers every call under it
            // via fqiCovers.
            //
            // Named once because two gates share it: an Observable Command's
            // follow-up RPCs carry no SiLA Client Metadata, so neither the
            // presence gate nor the lock gate may run on them.
            constexpr bool kCanCarryMetadata =
                !std::is_same_v<Req, org::silastandard::CommandExecutionUUID>;
            if constexpr (kCanCarryMetadata) {
                const bool anyMetadataReceived = std::any_of(
                    headers.begin(), headers.end(), [](const auto& entry) {
                        return entry.first.starts_with("sila-")
                            && entry.first.ends_with("-bin");
                    });
                enforceMetadataPolicy(
                    chain, fqi, anyMetadataReceived,
                    [&headers](const std::string& metadataFqi) {
                        return headers.count(metadataHeaderKey(metadataFqi)) > 0;
                    });

                // Lock identifier VALUE gate, immediately after the presence
                // gate: LockController is the metadata axis's second consumer
                // and sits next to the token check by design
                // (architecture-v2.md §3.10). Handed the RAW header value --
                // parsing, and the wrong-data-type verdict that goes with it,
                // belong to the one function both transports call.
                if (chain && chain->lockGate) {
                    const auto lockIt = headers.find(metadataHeaderKey(kLockIdentifierMetadataFqi));
                    chain->lockGate(fqi, lockIt != headers.end()
                                             ? std::optional<std::string>{lockIt->second}
                                             : std::nullopt);
                }
            }

            // Auth gate. Two shapes, split on whether this RPC can carry SiLA Client
            // Metadata on the wire (kCanCarryMetadata, defined above).
            if constexpr (kCanCarryMetadata) {
                // Initiation and every non-follow-up RPC: authorize the token the client
                // presented on THIS call's headers (already parsed into *ctx).
                if (chain && chain->auth) {
                    try {
                        chain->auth->intercept(*ctx, std::string{fqi});
                    } catch (...) {
                        logEvent(chain->logCallback, LogLevel::kWarning, "auth",
                                 std::string{"access denied: "} + std::string{fqi});
                        throw;
                    }
                }
            } else {
                // Observable Command follow-up (Info / Intermediate / Result): a bare
                // CommandExecutionUUID request. Part A forbids the client from resending
                // SiLA Client Metadata -- the access token included -- on these legs, so
                // *ctx carries no token to check. Two gates, in this order:
                //
                //   (1) Owner check (Batch C, High-2): resolve the UUID's registered owner
                //       and require it to equal this RPC's `fqi`. Fail CLOSED on an
                //       unregistered UUID (nullopt) so a cloud-initiated UUID cannot be
                //       replayed through a gRPC follow-up it was never authorized for.
                //   (2) Snapshot auth: replay the token captured at initiation and re-run
                //       the auth interceptor against the owning command's `fqi` -- the
                //       direct-gRPC twin of CloudEnvelopeRouter::makeFollowupContext. A
                //       nullopt snapshot means the initiation was unprotected; intercept
                //       then short-circuits on the same unprotected FQI.
                if (chain) {
                    const auto entry = chain->observableOwnerEntry(request.value());
                    if (!entry || entry->fqi != fqi) {
                        throw error::FrameworkError{
                            error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid};
                    }
                    if (entry->token) {
                        ctx->setMetadata("access-token", *entry->token);
                    }
                    if (chain->auth) {
                        try {
                            chain->auth->intercept(*ctx, std::string{fqi});
                        } catch (...) {
                            logEvent(chain->logCallback, LogLevel::kWarning, "auth",
                                     std::string{"access denied: "} + std::string{fqi});
                            throw;
                        }
                    }
                }
            }

            // Binary resolve now happens inside the generated validated_<handler>
            // (service_adapter.h.j2), the one point both the direct-gRPC and
            // cloud transports funnel a Command through (Part B p57, S71).
            // Handlers reached here that are NOT validated_ wrappers --
            // core-service commands, property RPCs, and the binary-transfer
            // RPCs themselves -- carry no Binary parameter, so nothing is lost.
            handler(request, *ctx, sink);

            // Binary inject — replaces large inline values with transfer UUIDs (unary only)
            if (unaryResponse && chain && chain->binaryStore) {
                binary::injectBinaryResults(*chain->binaryStore, unaryResponse,
                                            chain->binarySlotLifetime);
            }

            // Register this execution's owner for later follow-up owner checks (Batch C,
            // High-2), mirroring CloudHandlerRegistration's wrapObsInit. Only the unary
            // initiation RPC returns a CommandConfirmation; its response carries the freshly
            // minted CommandExecutionUUID, and `fqi` here is the initiating command's FQI.
            // if constexpr keeps every non-observable RPC from paying for this branch.
            if constexpr (std::is_same_v<Resp, org::silastandard::CommandConfirmation>) {
                if (chain && unaryResponse) {
                    const auto* confirmation = static_cast<const Resp*>(unaryResponse);
                    // Snapshot the initiation token alongside the owner FQI so follow-up
                    // legs (which carry no token, per Part A) can be authorized against it.
                    // nullopt when the initiation was unprotected -- no token was presented.
                    chain->registerObservableOwner(
                        confirmation->commandexecutionuuid().value(), std::string{fqi},
                        ctx->metadata("access-token"));
                }
            }

            if (chain) {
                logEvent(chain->logCallback, LogLevel::kInfo, "dispatch", fqi);
            }
        },
        sink);

#ifdef SILA_ENABLE_OTEL
    span->End();
#endif
}

}  // namespace sila2
