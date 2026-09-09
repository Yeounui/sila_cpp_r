// ActiveCallRegistry.h — Maps requestUUID to in-progress call contexts (architecture.md §3.9)
#pragma once

#include <sila/server/transport/CallContext.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace sila2 {

/// Tracks the calls currently in flight on one
/// @ref gl_connection_method "Server-Initiated Connection" (cloud connectivity) stream, keyed by
/// the
/// envelope's requestUUID, so the stream's transport can cancel all of them
/// when the underlying connection breaks. Owned by CloudTransport; not part
/// of the server author's API.
class ActiveCallRegistry {
public:
    ActiveCallRegistry() = default;
    ActiveCallRegistry(const ActiveCallRegistry&) = delete;
    ActiveCallRegistry& operator=(const ActiveCallRegistry&) = delete;

    /// Registers a call. Also drops entries whose context has already died,
    /// so a long-lived stream does not accumulate one expired weak_ptr per
    /// completed RPC (§2.1h).
    void add(const std::string& requestUUID, std::shared_ptr<CallContext> ctx);
    /// Drops the requestUUID's entry, e.g. once its call has finished.
    void remove(const std::string& requestUUID);
    /// Returns the call's CallContext, or nullptr if requestUUID is unknown
    /// or its call has already finished.
    std::shared_ptr<CallContext> find(const std::string& requestUUID);
    /// Requests cancellation of one call, as CallContext::CancellationReason::kClientRequested.
    void cancel(const std::string& requestUUID);
    /// Requests cancellation of every call currently registered, as
    /// CallContext::CancellationReason::kConnectionLost. Called when the
    /// cloud stream itself breaks, not for a single call's own cancellation.
    void cancelAll();

private:
    std::map<std::string, std::weak_ptr<CallContext>> calls_;
    std::mutex mu_;
};

}  // namespace sila2
