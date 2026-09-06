// SilaClientBase.cc
#include "SilaClientBase.h"

#include "AuthSession.h"

// SiLA 2 requires each sila-...-bin header to carry the serialized
// Metadata_<Name> message, not the bare value.
#include "AuthorizationService.pb.h"
#include "LockController.pb.h"

#include <sila/client/ExecutionStore.h>
#include <sila/common/types/BasicTypes.h>
#include <sila/common/util/MetadataHeaderKey.h>

#include <grpcpp/grpcpp.h>

#include <string>
#include <utility>

namespace sila2 {

namespace {

/// Serializes a lock identifier as the Metadata_LockIdentifier message
/// (LockController.proto), the shape a standard SiLA server parses.
std::string serializeLockIdentifierMetadata(const std::string& lockIdentifier) {
    sila2::org::silastandard::core::lockcontroller::v1::Metadata_LockIdentifier metadata;
    *metadata.mutable_lockidentifier() = types::toProto(lockIdentifier);
    return metadata.SerializeAsString();
}

/// Same for the access token (Metadata_AccessToken, AuthorizationService.proto).
std::string serializeAccessTokenMetadata(const std::string& accessToken) {
    sila2::org::silastandard::core::authorizationservice::v1::Metadata_AccessToken metadata;
    metadata.mutable_accesstoken()->set_value(accessToken);
    return metadata.SerializeAsString();
}

}  // namespace

SilaClientBase::SilaClientBase(std::string host, uint16_t port, ClientConfig config)
    : config_{std::move(config)} {
    auto target = host + ":" + std::to_string(port);
    auto credentials = config_.channelCredentials(host);
    channel_ = grpc::CreateChannel(target, credentials);
    if (config_.lockIdentifier()) {
        metadataInjector_.set(kLockIdentifierMetadataFqi,
                              serializeLockIdentifierMetadata(*config_.lockIdentifier()));
    }
    // Assembly seam for S36/G1 (Part A p33): only build the store when a path
    // was actually configured, so persistence stays off by default.
    if (!config_.executionStorePath().empty()) {
        executionStore_ = std::make_unique<ExecutionStore>(config_.executionStorePath());
    }
}

// Defined here (not in the header) so that grpc::Channel is a complete type
// at the point its destructor runs, matching SiLAServerBase's pattern.
SilaClientBase::~SilaClientBase() = default;

std::shared_ptr<grpc::Channel> SilaClientBase::channel() const { return channel_; }

const ClientConfig& SilaClientBase::config() const { return config_; }

ClientConfig& SilaClientBase::config() { return config_; }

bool SilaClientBase::isConnected() const {
    // false: query current state only, don't force a connection attempt.
    auto state = channel_->GetState(false);
    // IDLE counts as connected: a fresh channel starts idle and only moves to
    // CONNECTING on first RPC, so treating it as "disconnected" would be misleading.
    return state == GRPC_CHANNEL_READY || state == GRPC_CHANNEL_IDLE;
}

MetadataInjector& SilaClientBase::metadataInjector() { return metadataInjector_; }

const MetadataInjector& SilaClientBase::metadataInjector() const { return metadataInjector_; }

bool SilaClientBase::authenticate(const std::string& serverUuid,
                                  const std::vector<std::string>& requestedFeatures) {
    if (!config_.hasUserCredentials()) {
        return false;
    }
    authSession_ = std::make_unique<AuthSession>(
        config_.user(), config_.password(), serverUuid, channel_);
    if (!authSession_->login(requestedFeatures)) {
        authSession_.reset();
        return false;
    }
    metadataInjector_.set(kAccessTokenMetadataFqi,
                          serializeAccessTokenMetadata(authSession_->accessToken()));
    authSession_->setTokenChangeCallback([this](const std::string& newToken) {
        metadataInjector_.set(kAccessTokenMetadataFqi, serializeAccessTokenMetadata(newToken));
    });
    return true;
}

bool SilaClientBase::isAuthenticated() const {
    return authSession_ && authSession_->isAuthenticated();
}

ExecutionStore* SilaClientBase::executionStore() { return executionStore_.get(); }

}  // namespace sila2
