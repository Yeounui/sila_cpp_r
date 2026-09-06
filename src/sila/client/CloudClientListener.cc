// CloudClientListener.cc
#include "CloudClientListener.h"

#include <sila/common/util/AsciiCase.h>
#include <sila/common/util/uuid.h>
#include <sila/server/SiLAServerBase.h>
#include <sila/server/config/TlsConfig.h>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sila2 {

namespace {

std::string readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error{
            std::string{"CloudClientListener: cannot read "} + path};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Builds SslServerCredentials from a PEM cert/key pair. A non-empty
// clientCaPem switches the listener to mutual TLS: gRPC then demands a
// client certificate and verifies it against that CA before the handler
// runs, which is the only configuration where ConnectSiLAServer's
// certificate-to-uuid binding is an actual control rather than a check that
// passes because no certificate was ever requested (audit 3.1k).
std::shared_ptr<grpc::ServerCredentials> makeSslCreds(
    const std::string& certPem, const std::string& keyPem,
    const std::string& clientCaPem) {
    const auto requestType = clientCaPem.empty()
        ? GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE
        : GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;

    grpc::SslServerCredentialsOptions ssl_opts{requestType};
    ssl_opts.pem_root_certs = clientCaPem;
    ssl_opts.pem_key_cert_pairs.push_back({keyPem, certPem});
    return grpc::SslServerCredentials(ssl_opts);
}

// SiLA 2 Part B puts the server UUID in a private X.509 extension under the
// SiLA IANA PEN, not in the certificate's CN -- see TlsConfig.cc's
// generateCertificate, which writes the UUID there and leaves CN as the host
// name ("SiLA2"). A CN-only comparison therefore never matches a conformant
// SiLA server certificate, which is why the original check could not have
// worked even with mutual TLS turned on (audit 3.1k).
constexpr auto kSiLA2IanaPen = "1.3.6.1.4.1.58583";

// Grace before gRPC force-cancels in-flight ConnectSiLAServer streams on
// stop(). Only paid when a peer is still connected. An order of magnitude
// below the mDNS shutdown bound (5s) so the whole teardown stays sub-second.
constexpr auto kStopGrace = std::chrono::milliseconds{200};

std::string readSiLAServerUuid(const std::string& certPem) {
    auto* certBio = BIO_new_mem_buf(certPem.data(), static_cast<int>(certPem.size()));
    if (certBio == nullptr) {
        return {};
    }
    const auto cert = X509Ptr{
        PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr), X509_free};
    BIO_free(certBio);
    if (cert == nullptr) {
        return {};
    }

    // no_name=1: look the OID up as dotted digits rather than by short name.
    // The short name is only registered once generateCertificate has run in
    // this process, which a listener that received its certificate from a
    // file never does.
    const auto uuidOid = std::unique_ptr<ASN1_OBJECT, void (*)(ASN1_OBJECT*)>{
        OBJ_txt2obj(kSiLA2IanaPen, 1), ASN1_OBJECT_free};
    const auto extensionIndex = X509_get_ext_by_OBJ(cert.get(), uuidOid.get(), -1);
    if (extensionIndex < 0) {
        return {};
    }

    const auto* extensionValue =
        X509_EXTENSION_get_data(X509_get_ext(cert.get(), extensionIndex));
    if (extensionValue == nullptr) {
        return {};
    }
    const auto valueLength = ASN1_STRING_length(extensionValue);
    if (valueLength <= 0) {
        return {};
    }
    return std::string{
        reinterpret_cast<const char*>(ASN1_STRING_get0_data(extensionValue)),
        static_cast<std::size_t>(valueLength)};
}

// The peer's uuid may be carried either by the SiLA 2 extension above or, for
// a non-SiLA certificate an operator issued themselves, by the CN -- both come
// out of the same certificate gRPC already verified, so accepting either
// widens where the name is read from, not who is trusted.
bool certificateBindsUuid(const grpc::AuthContext& authContext,
                          const std::string& serverUuid) {
    // Part A p90: UUID comparison MUST ignore case; a peer may present an
    // upper-case UUID (RFC 4122 permits it).
    const std::string wantUuid = util::asciiLower(serverUuid);
    // GRPC_X509_PEM_CERT_PROPERTY_NAME is the peer *leaf* certificate;
    // x509_pem_cert_chain excludes the leaf on the server side.
    for (const auto& pem : authContext.FindPropertyValues("x509_pem_cert")) {
        if (util::asciiLower(readSiLAServerUuid(std::string(pem.data(), pem.size()))) == wantUuid) {
            return true;
        }
    }
    for (const auto& cn : authContext.FindPropertyValues("x509_common_name")) {
        if (util::asciiLower(std::string(cn.data(), cn.size())) == wantUuid) {
            return true;
        }
    }
    return false;
}

// CommandParameter and UnobservablePropertyRead carry the same repeated
// Metadata field (SiLACloudConnector.proto:64-68, :112-115), so one filler
// serves both envelope kinds.
void fillCloudMetadata(const std::map<std::string, std::string>& metadata,
                       google::protobuf::RepeatedPtrField<cloud::Metadata>* out) {
    for (const auto& [metadataFqi, value] : metadata) {
        auto* entry = out->Add();
        entry->set_fullyqualifiedmetadataid(metadataFqi);
        entry->set_value(value);
    }
}

}  // namespace

CloudClientListener::CloudClientListener(uint16_t listenPort,
                                          std::shared_ptr<grpc::ServerCredentials> creds,
                                          std::string clientCaPem)
    : listenPort_{listenPort} {
    if (creds) {
        // Caller-supplied credentials are opaque: whether they request a
        // client certificate is not observable from here, so a client CA
        // passed alongside them cannot be installed and would silently do
        // nothing -- exactly the failure mode audit 3.1k is about. This
        // fail-closed check is for an explicitly passed clientCaPem only --
        // the SILA_LISTENER_CLIENT_CA env var below is read after this
        // branch returns, so it can never surprise an existing caller who
        // only ever passed creds explicitly.
        if (!clientCaPem.empty()) {
            throw std::logic_error{
                "CloudClientListener: clientCaPem cannot be applied to "
                "caller-supplied ServerCredentials; build the credentials "
                "with GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY "
                "instead"};
        }
        creds_ = std::move(creds);
        return;
    }

    // Priority: explicit argument → env var. Same shape as the cert/key
    // pair below, which this class already configures that way. Read only
    // here, on the auto-credentials path: an ambient env var must not turn
    // every existing CloudClientListener(port, myCreds) call into a throw.
    if (clientCaPem.empty()) {
        const char* clientCaPath = std::getenv("SILA_LISTENER_CLIENT_CA");
        if (clientCaPath) {
            clientCaPem = readFile(clientCaPath);
        }
    }

    peerBindingEnforced_ = !clientCaPem.empty();

    // Priority: env-var cert files → auto-generated self-signed cert.
    // Never fall back to InsecureServerCredentials.
    const char* certPath = std::getenv("SILA_LISTENER_CERT");
    const char* keyPath  = std::getenv("SILA_LISTENER_KEY");
    if (certPath && keyPath) {
        certificatePem_ = readFile(certPath);
        creds_ = makeSslCreds(certificatePem_, readFile(keyPath), clientCaPem);
    } else {
        auto key  = generateKey();
        auto cert = generateCertificate(key, "localhost", "127.0.0.1");
        certificatePem_ = certificateToPem(cert);
        creds_ = makeSslCreds(certificatePem_, keyToPem(key), clientCaPem);
    }
}

CloudClientListener::~CloudClientListener() {
    stop();
}

void CloudClientListener::start() {
    if (running_.load()) return;

    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(listenPort_), creds_);
    // Same cap as CloudTransport::openStream's outbound channel, which dials
    // the very ConnectSiLAServer stream this server terminates: an envelope
    // accepted there and rejected here would split the transport in two.
    builder.SetMaxReceiveMessageSize(kMaxReceiveMessageSizeBytes);
    // Server half of CloudTransport::openStream's 60 s KEEPALIVE_TIME
    // (SiLAServerBase.cc carries the matching values for its own peer,
    // inbound clients). MIN_RECV_PING_INTERVAL_WITHOUT_DATA must stay <= that
    // 60 s or gRPC answers CloudTransport's pings with GOAWAY/ENHANCE_YOUR_CALM
    // after 2 strikes.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 60000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(
        GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 30000);
    builder.RegisterService(this);
    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error{"CloudClientListener: failed to bind port " + std::to_string(listenPort_)};
    }

    running_ = true;
}

void CloudClientListener::stop() {
    if (!running_.load()) return;
    running_ = false;

    // Deadline, not the no-deadline overload: a settled peer keeps its
    // ConnectSiLAServer stream open, so the handler stays parked in
    // stream->Read() and the graceful phase never completes on its own --
    // stop() then waits for the connected server to go away by itself (audit
    // 3.2n). Once the deadline passes gRPC cancels every in-flight call, which
    // is what makes Read() return false. Shutdown still returns only after all
    // handler threads have returned, so the sweep below is uncontended. It
    // must stay AHEAD of the mu_ lock: a handler tearing down takes mu_
    // itself, so holding mu_ across Shutdown would deadlock.
    server_->Shutdown(std::chrono::system_clock::now() + kStopGrace);

    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [uuid, session] : sessions_) {
        std::lock_guard<std::mutex> sessionLock(session->mu);
        for (auto& [requestUuid, promise] : session->pending) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error{"listener stopped"}));
        }
        session->pending.clear();
    }
    sessions_.clear();

    server_.reset();
}

bool CloudClientListener::isRunning() const {
    return running_.load();
}

void CloudClientListener::setServerConnectedCallback(ServerConnectedCallback cb) {
    onConnect_ = std::move(cb);
}

grpc::Status CloudClientListener::ConnectSiLAServer(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<cloud::SiLAClientMessage, cloud::SiLAServerMessage>* stream) {
    // "sila-server-uuid" is a well-known metadata key the connecting SiLA
    // server is expected to set; fall back to a generated id if it's absent
    // so a session can still be tracked.
    auto metadata = context->client_metadata();
    auto uuidEntry = metadata.find("sila-server-uuid");
    const bool uuidClaimed = (uuidEntry != metadata.end());

    auto authContext = context->auth_context();
    const bool peerAuthenticated =
        authContext != nullptr && authContext->IsPeerAuthenticated();

    // Under the default credentials no client certificate is requested, so
    // peerAuthenticated is always false and the binding below cannot be
    // enforced -- the uuid is then an unverified claim and this listener does
    // not pretend otherwise. Constructing with a client CA turns on mutual TLS
    // and makes the two refusals here mandatory (audit 3.1k).
    if (peerBindingEnforced_ && !peerAuthenticated) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "CloudClientListener: a verified client certificate is required");
    }
    if (peerBindingEnforced_ && !uuidClaimed) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "CloudClientListener: sila-server-uuid metadata is required "
                            "when the certificate binding is enforced");
    }

    std::string serverUuid;
    if (uuidClaimed) {
        serverUuid = std::string(uuidEntry->second.data(), uuidEntry->second.size());
    } else {
        serverUuid = sila2::util::generateUuid();
    }

    // Runs whenever a certificate actually arrived, enforced mode or not: a
    // caller who supplied their own mutual-TLS credentials gets the binding
    // too, and a peer that presents a certificate never gets a weaker check
    // than one that presents none.
    if (peerAuthenticated && uuidClaimed
        && !certificateBindsUuid(*authContext, serverUuid)) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "sila-server-uuid does not match the client certificate");
    }

    auto session = std::make_shared<Session>();
    session->stream = stream;

    {
        std::lock_guard<std::mutex> lock(mu_);
        sessions_[serverUuid] = session;
    }

    grpc::Status status = grpc::Status::OK;
    try {
        if (onConnect_) {
            onConnect_(serverUuid);
        }
    } catch (const std::exception& e) {
        status = grpc::Status{grpc::StatusCode::INTERNAL, e.what()};
    } catch (...) {
        status = grpc::Status{grpc::StatusCode::INTERNAL, "server connected callback failed"};
    }

    cloud::SiLAServerMessage msg;
    while (status.ok() && stream->Read(&msg)) {
        std::string requestUuid = msg.requestuuid();

        std::lock_guard<std::mutex> sessionLock(session->mu);
        auto pendingIt = session->pending.find(requestUuid);
        if (pendingIt != session->pending.end()) {
            pendingIt->second.set_value(std::move(msg));
            session->pending.erase(pendingIt);
        }
    }

    // Erased before the sweep, not after: this keeps the handler's two locks in
    // the same mu_ -> session->mu order stop() nests them in, and it makes the
    // pending failures below a happens-after signal for the erase decision.
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(serverUuid);
        // Identity, not key: with auto-reconnect a new handler may already have
        // replaced this entry under the same uuid, and erasing by key alone
        // would delete the live session, leaving a connected server whose every
        // call() gets "unknown server" (audit 3.2o).
        if (it != sessions_.end() && it->second == session) {
            sessions_.erase(it);
        }
    }

    // closed and the sweep share one critical section: a call() either takes
    // session->mu first and has its promise failed here, or takes it after and
    // sees closed. There is no third interleaving where a promise is inserted
    // after the sweep and waited on forever (audit 3.2p). This also runs when
    // the identity check above lost -- failing this handler's own pendings is
    // independent of who owns the map entry.
    {
        std::lock_guard<std::mutex> sessionLock(session->mu);
        session->closed = true;
        for (auto& [requestUuid, promise] : session->pending) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error{"server disconnected"}));
        }
        session->pending.clear();
    }

    return status;
}

cloud::SiLAServerMessage CloudClientListener::call(
    const std::string& serverUuid,
    const std::string& fqi,
    const std::string& parameterBytes,
    bool isCommand,
    const std::map<std::string, std::string>& metadata) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(serverUuid);
        if (it == sessions_.end()) {
            throw std::runtime_error{"unknown server: " + serverUuid};
        }
        session = it->second;
    }

    for (const auto& [metadataFqi, value] : metadata) {
        // CloudEnvelopeRouter::makeCloudCallContext calls setMetadata
        // unconditionally on this id (CloudEnvelopeRouter.cc:426-427), so a
        // blank one is stored under the empty key and matched by no
        // interceptor -- a silently ignored credential. Refuse it here rather
        // than shipping an entry the server cannot act on.
        if (metadataFqi.empty()) {
            throw std::invalid_argument{
                "metadata entry with empty fully qualified identifier"};
        }
    }

    std::string requestUuid = sila2::util::generateUuid();

    cloud::SiLAClientMessage envelope;
    envelope.set_requestuuid(requestUuid);
    if (isCommand) {
        auto* execution = envelope.mutable_unobservablecommandexecution();
        execution->set_fullyqualifiedcommandid(fqi);
        auto* parameter = execution->mutable_commandparameter();
        parameter->set_parameters(parameterBytes);
        fillCloudMetadata(metadata, parameter->mutable_metadata());
    } else {
        auto* read = envelope.mutable_unobservablepropertyread();
        read->set_fullyqualifiedpropertyid(fqi);
        fillCloudMetadata(metadata, read->mutable_metadata());
    }

    std::unique_lock<std::mutex> sessionLock(session->mu);
    // The session may have been torn down between the snapshot above and this
    // lock: stream would then point into a handler frame that is about to die,
    // and nothing would ever complete a promise inserted here (audit 3.2p).
    if (session->closed) {
        throw std::runtime_error{"server disconnected: " + serverUuid};
    }
    std::promise<cloud::SiLAServerMessage> promise;
    std::future<cloud::SiLAServerMessage> future = promise.get_future();
    session->pending[requestUuid] = std::move(promise);

    if (!session->stream->Write(envelope)) {
        session->pending.erase(requestUuid);
        sessionLock.unlock();
        throw std::runtime_error{"stream write failed"};
    }
    sessionLock.unlock();

    return future.get();
}

}  // namespace sila2
