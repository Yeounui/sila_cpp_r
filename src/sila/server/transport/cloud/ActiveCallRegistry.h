// ActiveCallRegistry.h — Maps requestUUID to in-progress call contexts (architecture.md §3.9)
#pragma once

#include <sila/server/transport/CallContext.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace sila2 {

class ActiveCallRegistry {
public:
    ActiveCallRegistry() = default;
    ActiveCallRegistry(const ActiveCallRegistry&) = delete;
    ActiveCallRegistry& operator=(const ActiveCallRegistry&) = delete;

    /// Registers a call. Also drops entries whose context has already died,
    /// so a long-lived stream does not accumulate one expired weak_ptr per
    /// completed RPC (§2.1h).
    void add(const std::string& requestUUID, std::shared_ptr<CallContext> ctx);
    void remove(const std::string& requestUUID);
    std::shared_ptr<CallContext> find(const std::string& requestUUID);
    void cancel(const std::string& requestUUID);
    void cancelAll();

private:
    std::map<std::string, std::weak_ptr<CallContext>> calls_;
    std::mutex mu_;
};

}  // namespace sila2
