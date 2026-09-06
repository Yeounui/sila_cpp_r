// ConnectionConfigurationServiceImpl.cc — SiLA2 core feature (architecture.md §3.9)
#include "ConnectionConfigurationServiceImpl.h"

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/types/Constraints.h>
#include <sila/server/features/ConnectionConfigurationServiceFdl.h>
#include <sila/server/transport/GrpcTransport.h>
#include <sila/server/transport/cloud/CloudTransport.h>

#include "SiLAFramework.pb.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace sila2 {
namespace {
const std::string kFdlXml = generated::kConnectionConfigurationServiceFdlXml;

// FDL §DefinedExecutionError declares this error for
// ConnectSiLAClient/DisconnectSiLAClient.
const std::string kInvalidSiLAClientErrorId =
    "org.silastandard/core/ConnectionConfigurationService/v1/DefinedExecutionError/InvalidSiLAClient";

// SiLAFramework.proto:96 requires a fully qualified parameter identifier here,
// not the bare <Parameter><Identifier> from the FDL.
const std::string kClientPortParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient/Parameter/SiLAClientPort";

// FDL ConnectSiLAClient/ClientName and /SiLAClientHost both carry
// MaximalLength 255 (ConnectionConfigurationService-v1_1.sila.xml:42, :57).
const std::string kClientNameParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient/Parameter/ClientName";
const std::string kClientHostParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/ConnectSiLAClient/Parameter/SiLAClientHost";

// FDL DisconnectSiLAClient/ClientName also carries MaximalLength 255
// (ConnectionConfigurationService-v1_1.sila.xml:105).
const std::string kDisconnectClientNameParamFqi =
    "org.silastandard/core/ConnectionConfigurationService/v1/Command/DisconnectSiLAClient/Parameter/ClientName";

}  // namespace

const std::string& connectionConfigurationServiceFdlXml() { return kFdlXml; }

ConnectionConfigurationServiceImpl::ConnectionConfigurationServiceImpl(
    CloudEnvelopeRouter& router,
    std::shared_ptr<grpc::ChannelCredentials> defaultCreds,
    const InterceptorChain* chain,
    std::filesystem::path storePath)
    // A fixed credential is the degenerate provider: the same object for every
    // host. Checked for null below, before it is wrapped.
    : credentialsFor_(defaultCreds
                          ? tls::OutboundCredentialsProvider{
                                [creds = defaultCreds](std::string_view) { return creds; }}
                          : tls::OutboundCredentialsProvider{}),
      router_(router), chain_{chain}, storePath_(std::move(storePath)) {
    // Fail closed. grpc::CreateChannel(target, nullptr) does not fail: it
    // returns a lame channel that answers every RPC with INVALID_ARGUMENT
    // (grpc create_channel.cc). ConnectSiLAClient would therefore report
    // success and register a client whose outbound stream is already dead,
    // and the operator would learn of it only from the client's silence.
    // Refusing here is the only point where the missing credential is still
    // attributable to the configuration that caused it.
    if (!credentialsFor_) {
        throw std::invalid_argument{
            "ConnectionConfigurationServiceImpl: defaultCreds must not be null"
            " — server-initiated connections carry access tokens and refuse to"
            " form without channel credentials"};
    }
    // The FDL requires the connection MODE (not just Persist clients) to survive
    // a restart (ConnectionConfigurationService-v1_1.sila.xml:11-13), so a server
    // that offers this Feature with nowhere to persist that state would violate
    // the spec — and enableServerInitiatedConnectionMode would report success
    // while the mode silently failed to persist. Reject an empty path here so
    // the whole service, not just individual Persist=true requests, is durable.
    if (storePath_.empty()) {
        throw std::invalid_argument{
            "ConnectionConfigurationServiceImpl: storePath must not be empty"
            " — the connection mode and persistent clients must survive a"
            " restart, which requires a file to persist them to"};
    }
}

// Defined here, not defaulted in the header: ClientEntry holds a
// unique_ptr<CloudTransport>, and CloudTransport's full definition (needed by
// the implicitly-generated destructor) is only visible once CloudTransport.h
// is included above.
ConnectionConfigurationServiceImpl::ConnectionConfigurationServiceImpl(
    CloudEnvelopeRouter& router,
    tls::OutboundCredentialsProvider credentialsFor,
    const InterceptorChain* chain,
    std::filesystem::path storePath)
    : credentialsFor_(std::move(credentialsFor)),
      router_(router), chain_{chain}, storePath_(std::move(storePath)) {
    // Same fail-closed rules as the fixed-credential constructor above.
    if (!credentialsFor_) {
        throw std::invalid_argument{
            "ConnectionConfigurationServiceImpl: credentialsFor must not be empty"};
    }
    if (storePath_.empty()) {
        throw std::invalid_argument{
            "ConnectionConfigurationServiceImpl: storePath must not be empty"
            " — the connection mode and persistent clients must survive a"
            " restart, which requires a file to persist them to"};
    }
}

ConnectionConfigurationServiceImpl::~ConnectionConfigurationServiceImpl() = default;

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

grpc::Status ConnectionConfigurationServiceImpl::EnableServerInitiatedConnectionMode(
    grpc::ServerContext* context,
    const connconfig_proto::EnableServerInitiatedConnectionMode_Parameters* request,
    connconfig_proto::EnableServerInitiatedConnectionMode_Responses* response) {
    GrpcUnaryResponseSink<connconfig_proto::EnableServerInitiatedConnectionMode_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) {
            enableServerInitiatedConnectionMode(req, ctx, out);
        },
        chain_, kEnableServerInitiatedConnectionModeFqi, response);
    return sink.status();
}

void ConnectionConfigurationServiceImpl::enableServerInitiatedConnectionMode(
    const connconfig_proto::EnableServerInitiatedConnectionMode_Parameters&, CallContext&,
    ResponseSink<connconfig_proto::EnableServerInitiatedConnectionMode_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    // Commit to memory only if the store commits. Capture the prior value, apply
    // the change (saveState() persists the current member, so it must be set
    // first), and restore it if the save throws — otherwise a disk failure would
    // return an error to the caller while modeEnabled_ silently led the
    // persisted state, so a later query and a restart would disagree.
    const bool previousMode = modeEnabled_;
    modeEnabled_ = true;
    try {
        saveState();
    } catch (...) {
        modeEnabled_ = previousMode;
        throw;
    }
    sink.send(connconfig_proto::EnableServerInitiatedConnectionMode_Responses{});
    sink.finish();
}

grpc::Status ConnectionConfigurationServiceImpl::DisableServerInitiatedConnectionMode(
    grpc::ServerContext* context,
    const connconfig_proto::DisableServerInitiatedConnectionMode_Parameters* request,
    connconfig_proto::DisableServerInitiatedConnectionMode_Responses* response) {
    GrpcUnaryResponseSink<connconfig_proto::DisableServerInitiatedConnectionMode_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) {
            disableServerInitiatedConnectionMode(req, ctx, out);
        },
        chain_, kDisableServerInitiatedConnectionModeFqi, response);
    return sink.status();
}

void ConnectionConfigurationServiceImpl::disableServerInitiatedConnectionMode(
    const connconfig_proto::DisableServerInitiatedConnectionMode_Parameters&, CallContext&,
    ResponseSink<connconfig_proto::DisableServerInitiatedConnectionMode_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    // Same commit-only-if-persisted discipline as the Enable handler: set the
    // new value, persist it, and roll back to the prior value if the save fails.
    const bool previousMode = modeEnabled_;
    modeEnabled_ = false;
    try {
        saveState();
    } catch (...) {
        modeEnabled_ = previousMode;
        throw;
    }
    sink.send(connconfig_proto::DisableServerInitiatedConnectionMode_Responses{});
    sink.finish();
}

grpc::Status ConnectionConfigurationServiceImpl::ConnectSiLAClient(
    grpc::ServerContext* context,
    const connconfig_proto::ConnectSiLAClient_Parameters* request,
    connconfig_proto::ConnectSiLAClient_Responses* response) {
    GrpcUnaryResponseSink<connconfig_proto::ConnectSiLAClient_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { connectSiLAClient(req, ctx, out); },
        chain_, kConnectSiLAClientFqi, response);
    return sink.status();
}

void ConnectionConfigurationServiceImpl::connectSiLAClient(
    const connconfig_proto::ConnectSiLAClient_Parameters& request, CallContext&,
    ResponseSink<connconfig_proto::ConnectSiLAClient_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    const auto clientName = request.clientname().value();
    if (auto nameError = types::checkMaximalLength(clientName, 255)) {
        throw error::ValidationError{kClientNameParamFqi, *nameError};
    }
    if (clients_.count(clientName) > 0) {
        // FDL §DefinedExecutionErrors/InvalidSiLAClient
        throw error::DefinedExecutionError{
            kInvalidSiLAClientErrorId,
            "Client name already in use"};
    }

    auto host = request.silaclienthost().value();
    if (auto hostError = types::checkMaximalLength(host, 255)) {
        throw error::ValidationError{kClientHostParamFqi, *hostError};
    }
    if (host.empty()) {
        throw error::DefinedExecutionError{
            kInvalidSiLAClientErrorId,
            "SiLAClientHost must not be empty"};
    }

    // Storage-format integrity guard: the FDL only constrains MaximalLength
    // 255 on ClientName/SiLAClientHost, but saveState()'s tab-delimited
    // persistence format additionally forbids the delimiters themselves —
    // a name or host containing one would corrupt the stored file.
    const auto containsDelimiterChar = [](const std::string& value) {
        return value.find_first_of("\t\n\r") != std::string::npos;
    };
    if (containsDelimiterChar(clientName) || containsDelimiterChar(host)) {
        throw error::DefinedExecutionError{
            kInvalidSiLAClientErrorId,
            "ClientName and SiLAClientHost must not contain tab, newline, or"
            " carriage-return control characters"};
    }

    // FDL ConnectSiLAClient/SiLAClientPort constraint
    // (ConnectionConfigurationService-v1_1.sila.xml:72-73): MinimalExclusive 0,
    // MaximalInclusive 65536. Only values outside that range are constraint
    // violations, so only they may be reported as ValidationError
    // (SiLAFramework.proto:96).
    const auto rawPort = request.silaclientport().value();
    if (rawPort <= 0 || rawPort > 65536) {
        throw error::ValidationError{
            kClientPortParamFqi,
            "SiLAClientPort must be greater than 0 and at most 65536"};
    }
    // 65536 clears the constraint but is one past the last addressable TCP port,
    // and ClientEntry stores the port as uint16_t, where it would wrap to 0 and
    // produce a client whose transport silently points nowhere. Refuse it as a
    // bad client definition rather than as a malformed parameter.
    if (rawPort > 65535) {
        throw error::DefinedExecutionError{
            kInvalidSiLAClientErrorId,
            "SiLAClientPort 65536 is within the FDL constraint but is not an"
            " addressable TCP port"};
    }
    const auto port = static_cast<uint16_t>(rawPort);
    const bool persist = request.persist().value();

    // Part B p74/p75 on the outbound leg: the provider hands back null for a
    // host this server may not connect to under its trust configuration (a
    // public host or a hostname while no trusted CA is configured). Refuse
    // before anything is persisted or dialled -- otherwise any caller of this
    // Command could point the server at an arbitrary endpoint whose
    // certificate is then accepted unverified.
    auto creds = credentialsFor_(host);
    if (!creds) {
        throw error::DefinedExecutionError{
            kInvalidSiLAClientErrorId,
            "SiLAClientHost '" + host + "' is not a private-range IP address"
            " (RFC1918/RFC4193) and this SiLA Server has no trusted CA"
            " configured for it -- Part B p75 allows an untrusted certificate"
            " only inside a private network; configure WithMutualTls or"
            " WithConnectionConfiguration to reach other hosts"};
    }

    // Persist before connecting: no observable side effect until the record is
    // durable. Construct the transport WITHOUT connecting (construction and
    // connect() are separate calls), insert it, and save. saveState() serializes
    // the current in-memory state, so the entry must be present before the save;
    // if the save throws, no stream was opened, so erasing the entry and
    // rethrowing leaves nothing behind.
    auto transport = std::make_unique<CloudTransport>(host, port, std::move(creds), router_);
    auto insertedIt = clients_.emplace(clientName, ClientEntry{
        std::move(host), port, persist, std::move(transport)}).first;
    try {
        saveState();
    } catch (...) {
        clients_.erase(insertedIt);
        throw;
    }

    // The record is durable; now open the stream. If connect() throws, roll the
    // persist back too — erase the entry and re-save — so a failed RPC leaves
    // neither a connection side effect nor a persisted ghost that would reconnect
    // on the next restart. Erasing destroys the ClientEntry, whose
    // ~CloudTransport tears down any partially-opened stream.
    try {
        insertedIt->second.transport->connect();
    } catch (...) {
        clients_.erase(insertedIt);
        // ponytail: if this rollback re-save also throws (disk fault moments
        // after the save above succeeded), that error masks the connect() one
        // and the just-persisted ghost outlives the erase — a double disk fault
        // is rare enough to leave; revisit if the store lives on flaky storage.
        saveState();
        throw;
    }

    sink.send(connconfig_proto::ConnectSiLAClient_Responses{});
    sink.finish();
}

grpc::Status ConnectionConfigurationServiceImpl::DisconnectSiLAClient(
    grpc::ServerContext* context,
    const connconfig_proto::DisconnectSiLAClient_Parameters* request,
    connconfig_proto::DisconnectSiLAClient_Responses* response) {
    GrpcUnaryResponseSink<connconfig_proto::DisconnectSiLAClient_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { disconnectSiLAClient(req, ctx, out); },
        chain_, kDisconnectSiLAClientFqi, response);
    return sink.status();
}

void ConnectionConfigurationServiceImpl::disconnectSiLAClient(
    const connconfig_proto::DisconnectSiLAClient_Parameters& request, CallContext&,
    ResponseSink<connconfig_proto::DisconnectSiLAClient_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    const auto clientName = request.clientname().value();
    if (auto nameError = types::checkMaximalLength(clientName, 255)) {
        throw error::ValidationError{kDisconnectClientNameParamFqi, *nameError};
    }
    auto it = clients_.find(clientName);
    if (it == clients_.end()) {
        // FDL §DefinedExecutionErrors/InvalidSiLAClient
        throw error::DefinedExecutionError{
            kInvalidSiLAClientErrorId,
            "Unknown client name"};
    }

    // Only a removing disconnect changes persisted content: saveState() writes
    // mode plus each persist==true client's name/host/port, none of which a
    // non-removing disconnect touches (connection status is not persisted). The
    // two cases therefore split, and the split is what makes each one atomic.
    if (request.remove().value()) {
        // Persist the removal before tearing the stream down. Lift the entry out
        // of the map — its transport stays alive in the local, still connected —
        // then save the now-smaller state. If the save throws, move the entry
        // back so the client stays exactly as it was (still connected, still
        // configured) rather than vanishing from memory while the store keeps
        // it. On success, the local goes out of scope and ~CloudTransport
        // disconnects the stream, so teardown happens only after the removal is
        // durable.
        ClientEntry entry = std::move(it->second);
        clients_.erase(it);
        try {
            saveState();
        } catch (...) {
            clients_.emplace(clientName, std::move(entry));
            throw;
        }
    } else {
        // Nothing to persist, so nothing to roll back: the disconnect is the
        // only mutation, and it either completes or throws on its own. The entry
        // stays in clients_ as a configured-but-disconnected client.
        it->second.transport->disconnect();
    }

    sink.send(connconfig_proto::DisconnectSiLAClient_Responses{});
    sink.finish();
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

grpc::Status ConnectionConfigurationServiceImpl::Get_ServerInitiatedConnectionModeStatus(
    grpc::ServerContext* context,
    const connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Parameters* request,
    connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses* response) {
    GrpcUnaryResponseSink<connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) {
            getServerInitiatedConnectionModeStatus(req, ctx, out);
        },
        chain_, kGet_ServerInitiatedConnectionModeStatusFqi, response);
    return sink.status();
}

void ConnectionConfigurationServiceImpl::getServerInitiatedConnectionModeStatus(
    const connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Parameters&, CallContext&,
    ResponseSink<connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    connconfig_proto::Get_ServerInitiatedConnectionModeStatus_Responses response;
    response.mutable_serverinitiatedconnectionmodestatus()->set_value(modeEnabled_);
    sink.send(response);
    sink.finish();
}

grpc::Status ConnectionConfigurationServiceImpl::Get_ConfiguredSiLAClients(
    grpc::ServerContext* context,
    const connconfig_proto::Get_ConfiguredSiLAClients_Parameters* request,
    connconfig_proto::Get_ConfiguredSiLAClients_Responses* response) {
    GrpcUnaryResponseSink<connconfig_proto::Get_ConfiguredSiLAClients_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getConfiguredSiLAClients(req, ctx, out); },
        chain_, kGet_ConfiguredSiLAClientsFqi, response);
    return sink.status();
}

void ConnectionConfigurationServiceImpl::getConfiguredSiLAClients(
    const connconfig_proto::Get_ConfiguredSiLAClients_Parameters&, CallContext&,
    ResponseSink<connconfig_proto::Get_ConfiguredSiLAClients_Responses>& sink) {
    std::lock_guard<std::mutex> lock{mu_};

    connconfig_proto::Get_ConfiguredSiLAClients_Responses response;
    for (const auto& [clientName, entry] : clients_) {
        auto* configuredClient = response.add_configuredsilaclients();
        configuredClient->mutable_clientname()->set_value(clientName);
        configuredClient->mutable_silaclienthost()->set_value(entry.host);
        configuredClient->mutable_silaclientport()->set_value(static_cast<int64_t>(entry.port));
    }

    sink.send(response);
    sink.finish();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void ConnectionConfigurationServiceImpl::saveState() {
    // storePath_ is guaranteed non-empty by the constructor.
    // Full rewrite, not incremental: the file always reflects the current
    // in-memory state exactly. Write to a sibling temp file first and rename
    // into place, so a write failure leaves the previous valid state intact
    // rather than truncating the only copy — std::filesystem::rename is atomic
    // within one filesystem, and the temp sits in the same directory.
    // Serialize first so the file is written in one go.
    std::string content = "mode\t" + std::string{modeEnabled_ ? "1" : "0"} + '\n';
    for (const auto& [clientName, entry] : clients_) {
        if (!entry.persist) {
            continue;
        }
        content += "client\t" + clientName + '\t' + entry.host + '\t' +
                   std::to_string(entry.port) + '\n';
    }

    // The default store may live in the shared temp directory (SiLAServerBase
    // Build() fallback), where another local user can plant or swap anything
    // at a PREDICTABLE name: a symlink to a file this process can write, or a
    // victim file renamed onto a stale temp we are about to delete. So the
    // temp name is not predictable at all: mkstemp creates a fresh, unique,
    // owner-only (0600) file with O_EXCL and never follows a link, and no
    // pathname is ever deleted -- not even ours on a failed save, because a
    // name that is public can be swapped under us in a shared non-sticky
    // directory (Codex reviews of SC32, 2026-09-04). The system temp
    // directory the default store falls back to is sticky, so there the only
    // cost is a leftover file named in the error.
    std::string tmpTemplate = storePath_.string() + ".XXXXXX";
    const int fd = ::mkstemp(tmpTemplate.data());
    if (fd < 0) {
        throw std::runtime_error{
            "ConnectionConfigurationServiceImpl::saveState: could not create a temp file next to " +
            storePath_.string() + ": " + std::strerror(errno)};
    }
    const std::filesystem::path tmpPath{tmpTemplate};
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            // Not unlinked: once the name is public another user in a shared
            // non-sticky directory could swap a victim file onto it, and a
            // pathname unlink would delete that instead. The message names
            // the leftover so the operator can remove it.
            throw std::runtime_error{
                "ConnectionConfigurationServiceImpl::saveState: could not write " +
                tmpPath.string() + ": " + std::strerror(err) + " (temp file left in place)"};
        }
        written += static_cast<std::size_t>(n);
    }
    ::close(fd);

    std::error_code ec;
    std::filesystem::rename(tmpPath, storePath_, ec);
    if (ec) {
        // Same reasoning as the write-failure path: no pathname unlink.
        throw std::runtime_error{
            "ConnectionConfigurationServiceImpl::saveState: could not replace " +
            storePath_.string() + ": " + ec.message() + " (temp file " + tmpPath.string() +
            " left in place)"};
    }
    // A throw here (open/write/rename failure) unwinds into the calling command
    // handler, which rolls its in-memory mutation back before returning the
    // error (see connectSiLAClient / the mode handlers / disconnectSiLAClient).
    // Two layers keep the store and memory in agreement: the temp+rename above
    // makes the file itself all-or-nothing, and the per-handler rollback makes
    // memory match whatever the file ended up holding — so a successful RPC and
    // the persisted state never disagree.
}

// Reconstructs clients_ from storePath_ as UNCONNECTED transports; the actual
// connect() calls happen afterward, in connectPersistentClients().
void ConnectionConfigurationServiceImpl::loadState() {
    // storePath_ is guaranteed non-empty by the constructor; an absent file is
    // still valid — it just means a first run with nothing persisted yet.
    if (!std::filesystem::exists(storePath_)) {
        return;
    }
    // Trust only a file this process owns and nobody else can write: the
    // records are endpoints the server dials with its own identity, and the
    // default store path under the temp directory is predictable from the
    // advertised UUID, so a file planted or widened by another local user must
    // not be honoured (Codex review of SC32, 2026-09-04).
    struct stat storeStat {};
    if (::lstat(storePath_.c_str(), &storeStat) != 0 || !S_ISREG(storeStat.st_mode) ||
        storeStat.st_uid != ::geteuid() || (storeStat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw std::runtime_error{
            "ConnectionConfigurationServiceImpl::loadState: refusing state file " +
            storePath_.string() +
            " -- it must be a regular file owned by this user and not writable by others"};
    }

    std::ifstream in{storePath_};
    if (!in) {
        throw std::runtime_error{
            "ConnectionConfigurationServiceImpl::loadState: could not open " +
            storePath_.string()};
    }

    const auto corrupt = [this]() {
        return std::runtime_error{
            "ConnectionConfigurationServiceImpl::loadState: corrupt state file " +
            storePath_.string()};
    };

    // Parse into locals and commit to members only once the whole file is read
    // cleanly: a corrupt record mid-file must leave modeEnabled_/clients_ as
    // they were, not half-populated.
    bool loadedMode = modeEnabled_;
    std::map<std::string, ClientEntry> loaded;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab - start));
            if (tab == std::string::npos) {
                break;
            }
            start = tab + 1;
        }

        const auto& recordType = fields[0];
        if (recordType == "mode") {
            if (fields.size() != 2 || (fields[1] != "0" && fields[1] != "1")) {
                throw corrupt();
            }
            loadedMode = (fields[1] == "1");
        } else if (recordType == "client") {
            if (fields.size() != 4) {
                throw corrupt();
            }
            const auto& clientName = fields[1];
            auto host = fields[2];
            const auto& portField = fields[3];

            // std::stol would accept "80x" (trailing junk) and leading spaces,
            // silently admitting a corrupt field. The store is machine-written
            // as plain digits, so require exactly that and fail loud otherwise.
            const bool allDigits = !portField.empty() &&
                std::all_of(portField.begin(), portField.end(),
                    [](unsigned char c) { return std::isdigit(c); });
            if (!allDigits) {
                throw corrupt();
            }
            long rawPort = 0;
            try {
                rawPort = std::stol(portField);
            } catch (const std::exception&) {
                throw corrupt();  // out_of_range for an overlong digit run
            }
            if (rawPort < 1 || rawPort > 65535) {
                throw corrupt();
            }
            const auto port = static_cast<uint16_t>(rawPort);

            // Skip names already live in the service or already seen earlier in
            // this same file (a duplicate record).
            if (clients_.count(clientName) > 0 || loaded.count(clientName) > 0) {
                continue;
            }
            // The trust configuration may have changed since the record was
            // accepted (a CA removed); a client that can no longer be dialled
            // is a configuration conflict the operator must resolve, so fail
            // loud like a corrupt record rather than restore a dead entry.
            auto creds = credentialsFor_(host);
            if (!creds) {
                throw std::runtime_error{
                    "ConnectionConfigurationServiceImpl::loadState: persisted client '" +
                    clientName + "' targets " + host +
                    ", which the current trust configuration does not allow"
                    " (Part B p75) -- remove the record from " + storePath_.string() +
                    " or configure a trusted CA"};
            }
            auto transport = std::make_unique<CloudTransport>(host, port, std::move(creds), router_);
            loaded.emplace(clientName, ClientEntry{
                std::move(host), port, /*persist=*/true, std::move(transport)});
        } else {
            throw corrupt();
        }
    }

    // Whole file parsed cleanly — commit.
    modeEnabled_ = loadedMode;
    for (auto& [clientName, entry] : loaded) {
        clients_.emplace(clientName, std::move(entry));
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ConnectionConfigurationServiceImpl::connectPersistentClients() {
    std::lock_guard<std::mutex> lock{mu_};

    // loadState() repopulates clients_ from disk (nothing to load on a first
    // run); the loop below then connects every persist==true entry.
    // ponytail: this holds mu_ across every transport->connect() (channel/stream
    // setup), so service commands block for the whole startup reconnect. This is
    // a startup hook that runs before the server takes traffic, so the stall is
    // not observed; split the lock if it ever runs against a live service.
    loadState();
    // The mode gates restart reconnection. The FDL states the reconnect promise
    // ("The SiLA Server will reconnect to the persistent SiLA Clients after a
    // restart") directly under the persistent-mode description
    // (ConnectionConfigurationService-v1_1.sila.xml:11-13), so a server that
    // loaded modeEnabled_==false must not re-open server-initiated streams. The
    // clients stay configured — loadState() has already populated clients_, so
    // ConfiguredSiLAClients still lists them and DisconnectSiLAClient can remove
    // them — only the outbound connect is suppressed. (Only restart is gated;
    // ConnectSiLAClient carries no mode precondition in the FDL, so it is left
    // untouched.)
    if (!modeEnabled_) {
        return;
    }
    for (auto& [clientName, entry] : clients_) {
        if (entry.persist) {
            entry.transport->connect();
        }
    }
}

void ConnectionConfigurationServiceImpl::shutdown() {
    std::lock_guard<std::mutex> lock{mu_};

    for (auto& [clientName, entry] : clients_) {
        entry.transport->disconnect();
    }
    clients_.clear();
}

}  // namespace sila2
