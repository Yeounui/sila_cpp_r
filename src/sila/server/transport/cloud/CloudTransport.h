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

/// Manages one server-initiated connection to a SiLA Client's
/// CloudClientEndpoint: this server connects out to the client's cloud
/// endpoint and opens a bidi stream (architecture.md §3.9).
class CloudTransport {
public:
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

    void connect();
    void disconnect();
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
