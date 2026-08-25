#include "ServerConfig.h"

#include <sila/util/uuid.h>

#include <utility>

namespace sila2 {

InMemoryServerConfig::InMemoryServerConfig(std::string name, std::size_t queueDepth)
    : uuid_(util::generateUuid()), name_(std::move(name)), queueDepth_(queueDepth) {
}

InMemoryServerConfig::InMemoryServerConfig(std::string uuid, std::string name, std::size_t queueDepth)
    : uuid_(std::move(uuid)), name_(std::move(name)), queueDepth_(queueDepth) {
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

std::size_t InMemoryServerConfig::subscriptionQueueDepth() const {
    return queueDepth_;
}

}  // namespace sila2
