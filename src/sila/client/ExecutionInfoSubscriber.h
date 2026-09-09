#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <grpcpp/grpcpp.h>  // grpc::ClientReader is only fully declared via the sync_stream / grpcpp.h headers

#include "CommandExecutionStatus.h"

namespace sila2 {

/// Delivers @ref gl_command_execution_info "Command Execution Info" updates
/// for one issued @ref gl_observable_command "Observable Command" execution
/// to a caller-supplied callback, without the caller having to block on a
/// gRPC stream itself. Obtained from a client-side command execution helper
/// that opened the subscription; not constructed directly by application
/// code.
///
// Wraps a gRPC server-streaming ExecutionInfo subscription (SiLA2 §6.3.4
// Observable Command Execution) with a background reader thread so callers
// receive updates via callback instead of blocking on Read().
class ExecutionInfoSubscriber {
public:
    /// Takes ownership of `context` and `reader` and immediately starts a
    /// background thread that reads the stream and invokes `callback` for
    /// every update until the stream ends.
    ExecutionInfoSubscriber(
        std::unique_ptr<grpc::ClientContext> context,
        std::unique_ptr<grpc::ClientReader<sila2::org::silastandard::ExecutionInfo>> reader,
        ExecutionUpdateCallback callback);

    /// Cancels the subscription (if still active) and joins the reader
    /// thread before the context/reader it depends on are destroyed.
    ~ExecutionInfoSubscriber();

    /// @return false once the stream has ended, whether it ended normally,
    /// with an error, or via cancel().
    [[nodiscard("caller expects to know whether the stream is still delivering updates")]]
    bool isActive() const;

    /// Cancels the subscription. This tears down the gRPC stream, which the
    /// server interprets as the client's signal to cancel the underlying
    /// @ref gl_observable_command "Observable Command" execution.
    void cancel();

    /// Blocks until the stream has ended (normally, with an error, or via
    /// cancel()).
    void wait();

    /// The status carried by the most recently received update, or
    /// CommandExecutionStatus::kWaiting before the first one arrives.
    [[nodiscard("the status drives dispatch of command completion handling")]]
    CommandExecutionStatus lastStatus() const;

    // Non-copyable, non-movable: the background thread captures `this`,
    // so relocating or duplicating the object would invalidate that binding.
    ExecutionInfoSubscriber(const ExecutionInfoSubscriber&) = delete;
    ExecutionInfoSubscriber& operator=(const ExecutionInfoSubscriber&) = delete;

private:
    // Runs on readThread_: consumes the stream until it ends, invoking
    // callback_ for every message and updating lastStatus_/active_.
    void readLoop();

    std::unique_ptr<grpc::ClientContext> context_;
    std::unique_ptr<grpc::ClientReader<sila2::org::silastandard::ExecutionInfo>> reader_;
    ExecutionUpdateCallback callback_;

    std::thread readThread_;
    std::atomic<bool> active_{true};
    std::atomic<CommandExecutionStatus> lastStatus_{CommandExecutionStatus::kWaiting};
};

}  // namespace sila2
