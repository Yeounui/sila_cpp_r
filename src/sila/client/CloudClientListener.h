// CloudClientListener.h — Client-side CloudClientEndpoint service (architecture.md §4.5)
#pragma once

#include "SiLACloudConnector.grpc.pb.h"

#include <sila/common/util/AsciiCase.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace sila2 {

namespace cloud = org::silastandard;

using ServerConnectedCallback = std::function<void(const std::string& serverUuid)>;

/// Runs a gRPC server that implements CloudClientEndpoint so that SiLA
/// servers can dial the client and open a bidi stream (server-initiated
/// connection, architecture.md §3.9 / §4.5). Client code issues requests
/// over an established session via call().
class CloudClientListener final : public cloud::CloudClientEndpoint::Service {
public:
    explicit CloudClientListener(uint16_t listenPort,
                                 std::shared_ptr<grpc::ServerCredentials> creds = {},
                                 std::string clientCaPem = {});
    ~CloudClientListener();

    // Not copyable/movable: owns a live grpc::Server and session state
    // referenced by in-flight stream handler threads.
    CloudClientListener(const CloudClientListener&) = delete;
    CloudClientListener& operator=(const CloudClientListener&) = delete;
    CloudClientListener(CloudClientListener&&) = delete;
    CloudClientListener& operator=(CloudClientListener&&) = delete;

    void start();
    void stop();
    bool isRunning() const;

    // PEM of the server certificate this listener presents to connecting
    // peers. Empty when the caller supplied pre-built ServerCredentials.
    const std::string& certificatePem() const { return certificatePem_; }

    // True when this listener demands a verified client certificate and
    // therefore enforces the certificate-to-uuid binding, INCLUDING that
    // "sila-server-uuid" metadata is mandatory. False under the default
    // credentials, where no client certificate is requested at all and a
    // connecting peer's "sila-server-uuid" is an unverified claim (audit
    // 3.1k). Also false when the caller supplies their own ServerCredentials
    // -- even caller-supplied mutual TLS -- because this constructor never
    // reaches the client-CA branch on that path: such a caller's connecting
    // peers get the certificate-to-uuid binding check in ConnectSiLAServer,
    // but not the mandatory-metadata refusal that this flag would otherwise
    // trigger.
    bool peerBindingEnforced() const { return peerBindingEnforced_; }

    void setServerConnectedCallback(ServerConnectedCallback cb);

    // ConnectSiLAServer bidi stream — called by gRPC when a server connects
    grpc::Status ConnectSiLAServer(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<cloud::SiLAClientMessage, cloud::SiLAServerMessage>* stream) override;

    // Issue a request to a connected server by UUID.
    // metadata maps a fully qualified SiLA metadata identifier (e.g.
    // "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken") to
    // the already-serialized Metadata_<Id> message bytes -- the same value the
    // direct-gRPC path hands to MetadataInjector::set(), NOT the bare token.
    // The cloud transport has no per-request headers, so the repeated Metadata
    // field is the only carrier (SiLACloudConnector.proto:64-72).
    // Returns the response envelope. Blocks until response arrives.
    cloud::SiLAServerMessage call(
        const std::string& serverUuid,
        const std::string& fqi,
        const std::string& parameterBytes,
        bool isCommand,
        const std::map<std::string, std::string>& metadata = {});

private:
    struct Session {
        // Points into the ConnectSiLAServer handler's own stack frame, so it is
        // valid only while that handler has not returned. mu is what makes that
        // safe to use: the handler sets closed under mu before returning, so a
        // call() that holds mu and sees closed == false knows the handler is
        // still waiting for that same mu and its frame is alive (audit 3.2p).
        grpc::ServerReaderWriter<cloud::SiLAClientMessage, cloud::SiLAServerMessage>* stream;
        // Guards stream, pending and closed.
        std::mutex mu;
        // Pending responses: requestUUID → promise
        std::map<std::string, std::promise<cloud::SiLAServerMessage>> pending;
        // Set by the handler in the same critical section as its final pending
        // sweep, so no promise can be inserted after the sweep and then be
        // waited on forever.
        bool closed{false};
    };

    uint16_t listenPort_;
    std::shared_ptr<grpc::ServerCredentials> creds_;
    std::string certificatePem_;
    bool peerBindingEnforced_{false};
    std::unique_ptr<grpc::Server> server_;
    ServerConnectedCallback onConnect_;

    // Lock order: mu_ -> session->mu, never the reverse. stop() is the only
    // place that nests the two.
    mutable std::mutex mu_;
    // Part A p90: UUID comparison MUST ignore case (server uuid key).
    std::map<std::string, std::shared_ptr<Session>, util::CaseInsensitiveLess> sessions_;
    std::atomic<bool> running_{false};
};

}  // namespace sila2
