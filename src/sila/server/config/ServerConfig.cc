#include "ServerConfig.h"

#include <sila/common/types/Constraints.h>
#include <sila/common/util/uuid.h>

#include <stdexcept>
#include <utility>

namespace sila2 {

namespace {
// SiLAService-v1_0.sila.xml:144-147 constrains ServerUUID to Length 36 plus
// this lowercase-hex Pattern. Third in-tree copy of the same literal, matching
// ErrorRecoveryServiceImpl.cc:30-31 and AuthorizationConfigurationServiceImpl.cc:27-28
// (hyphens unescaped: std::regex's ECMAScript grammar does not need the FDL's
// XSD `\-` outside a character class). Hoisting the three into Constraints.h is
// a cleanup outside this batch's scope.
const std::string kServerUuidPattern =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";
}  // namespace

// authorizationProviderUuid_ defaults to this server's own uuid_, not empty:
// AuthorizationConfigurationService-v1_0.sila.xml:12 describes the property
// as "the UUID of the SiLA server that this server uses to verify access
// tokens", and until SetAuthorizationProvider says otherwise this server
// verifies its own tokens locally out of AuthTokenStore -- so the default is
// a true statement, not a placeholder. uuid_ is listed before
// authorizationProviderUuid_ in ServerConfig.h, so it is already initialized
// here.
InMemoryServerConfig::InMemoryServerConfig(
    std::string name, Identity identity, Tuning tuning)
    : uuid_(util::generateUuid()),
      name_(std::move(name)),
      identity_(std::move(identity)),
      authorizationProviderUuid_(uuid_),
      tuning_(tuning) {
}

InMemoryServerConfig::InMemoryServerConfig(
    std::string uuid, std::string name, Identity identity, Tuning tuning)
    : uuid_(std::move(uuid)),
      name_(std::move(name)),
      identity_(std::move(identity)),
      authorizationProviderUuid_(uuid_),
      tuning_(tuning) {
    // Get_ServerUUID returns this string verbatim, so an unchecked value here
    // becomes an advertised property that violates SiLAService-v1_0.sila.xml:144-147
    // (audit S30). Checked at construction, not at the RPC: the RPC has no way to
    // fail usefully, and the caller's stack is where the mistake is.
    // std::invalid_argument, not a SilaError -- this is an assembly-time programming
    // error, not a wire call; it matches Builder::withAuthentication's null-argument
    // rejection (SilaServerBase.h) rather than the ValidationError sites in the
    // Feature implementations.
    // uuid_ (the member), not uuid (the parameter): the parameter was moved from above.
    if (auto lengthError = types::checkLength(uuid_, 36)) {
        throw std::invalid_argument{"InMemoryServerConfig: ServerUUID " + *lengthError};
    }
    if (auto patternError = types::checkPattern(uuid_, kServerUuidPattern)) {
        throw std::invalid_argument{"InMemoryServerConfig: ServerUUID " + *patternError};
    }
}

std::string InMemoryServerConfig::uuid() const {
    return uuid_;
}

std::string InMemoryServerConfig::name() const {
    std::lock_guard<std::mutex> lock{mu_};
    return name_;
}

void InMemoryServerConfig::setName(std::string name) {
    std::lock_guard<std::mutex> lock{mu_};
    name_ = std::move(name);
}

// build-time identity — immutable after construction, no lock needed.
std::string InMemoryServerConfig::serverType() const { return identity_.serverType; }
std::string InMemoryServerConfig::description() const { return identity_.description; }
std::string InMemoryServerConfig::version() const { return identity_.version; }
std::string InMemoryServerConfig::vendorUrl() const { return identity_.vendorUrl; }

std::size_t InMemoryServerConfig::subscriptionQueueDepth() const {
    return tuning_.subscriptionQueueDepth;
}

std::string InMemoryServerConfig::authorizationProviderUuid() const {
    std::lock_guard<std::mutex> lock{mu_};
    return authorizationProviderUuid_;
}

void InMemoryServerConfig::setAuthorizationProviderUuid(std::string uuid) {
    std::lock_guard<std::mutex> lock{mu_};
    authorizationProviderUuid_ = std::move(uuid);
}

std::size_t InMemoryServerConfig::binarySpoolThreshold() const {
    return tuning_.binarySpoolThreshold;
}

std::chrono::seconds InMemoryServerConfig::binarySlotLifetime() const {
    return tuning_.binarySlotLifetime;
}

std::chrono::seconds InMemoryServerConfig::cloudWriteTimeout() const {
    return tuning_.cloudWriteTimeout;
}

std::size_t InMemoryServerConfig::maxConcurrentCloudSubscriptions() const {
    return tuning_.maxConcurrentCloudSubscriptions;
}

std::chrono::seconds InMemoryServerConfig::errorHandlingTimeout() const {
    return tuning_.errorHandlingTimeout;
}

std::chrono::seconds InMemoryServerConfig::mdnsReadvertiseInterval() const {
    return tuning_.mdnsReadvertiseInterval;
}

std::chrono::seconds InMemoryServerConfig::mdnsRecordTtl() const {
    return tuning_.mdnsRecordTtl;
}

std::chrono::milliseconds InMemoryServerConfig::mdnsProbeWait() const {
    return tuning_.mdnsProbeWait;
}

}  // namespace sila2
