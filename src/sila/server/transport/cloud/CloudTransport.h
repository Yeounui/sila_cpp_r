// CloudTransport.h — Server-initiated connection stream management (architecture.md §3.9)
#pragma once

#include <sila/server/transport/cloud/ActiveCallRegistry.h>
#include <sila/server/transport/cloud/CloudEnvelopeRouter.h>
#include <sila/server/transport/cloud/StreamWriteSerializer.h>

#include "SiLACloudConnector.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sila2 {

namespace cloud = org::silastandard;

/// Manages one @ref gl_connection_method "Server-Initiated Connection"
/// (cloud connectivity) to a SiLA Client's CloudClientEndpoint: this server
/// connects out to the client's cloud endpoint and opens a bidi stream
/// (architecture.md §3.9). A server author never constructs this directly —
/// it is created and owned by SiLAServerBase for each host/port pair the
/// ConnectionConfigurationService is told to connect to.
class CloudTransport {
public:
    /// Configures the outbound target; call connect() to actually open the stream.
    /// @param host The SiLA Client's cloud endpoint host to connect out to.
    /// @param port The SiLA Client's cloud endpoint port.
    /// @param creds TLS credentials this server presents when connecting out.
    /// @param router Dispatches every envelope read from the opened stream; must outlive this
    /// object.
    CloudTransport(std::string host,
                   uint16_t port,
                   std::shared_ptr<grpc::ChannelCredentials> creds,
                   CloudEnvelopeRouter& router)
        : host_(std::move(host)), port_(port), creds_(std::move(creds)), router_(router) {}

    ~CloudTransport() { disconnect(); }

    CloudTransport(const CloudTransport&) = delete;
    CloudTransport& operator=(const CloudTransport&) = delete;
    CloudTransport(CloudTransport&&) = delete;
    CloudTransport& operator=(CloudTransport&&) = delete;

    /// Opens the outbound stream and starts the background thread that reads
    /// and dispatches incoming envelopes on it. Reconnects with backoff on
    /// its own if the stream later breaks. A no-op if already connected or
    /// still tearing down from a prior disconnect().
    void connect();
    /// Cancels the outbound stream, cancels every call still in flight on
    /// it, joins the receive thread, and releases the channel. A no-op if
    /// not currently connected.
    void disconnect();
    /// Whether the outbound stream is currently open. False while a broken
    /// stream is being retried in the background.
    bool isConnected() const { return connected_.load(); }

private:
    void receiveLoop();
    void reconnect();
    void openStream();

    std::string host_;
    uint16_t port_;
    std::shared_ptr<grpc::ChannelCredentials> creds_;
    CloudEnvelopeRouter& router_;
    ActiveCallRegistry calls_;

    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<cloud::CloudClientEndpoint::Stub> stub_;
    // Shared, not unique: the writer (and through it a detached subscription
    // thread) can outlive a disconnect/reconnect that resets these members,
    // and a write in flight then still needs both objects alive (§2.2l).
    std::shared_ptr<grpc::ClientContext> context_;
    std::shared_ptr<grpc::ClientReaderWriter<cloud::SiLAServerMessage, cloud::SiLAClientMessage>> stream_;
    std::shared_ptr<StreamWriteSerializer> writer_;

    std::thread receiveThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace sila2
