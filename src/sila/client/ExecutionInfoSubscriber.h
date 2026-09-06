#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <grpcpp/grpcpp.h>  // grpc::ClientReader is only fully declared via the sync_stream / grpcpp.h headers

#include "CommandExecutionStatus.h"

namespace sila2 {

// Wraps a gRPC server-streaming ExecutionInfo subscription (SiLA2 §6.3.4
// Observable Command Execution) with a background reader thread so callers
// receive updates via callback instead of blocking on Read().
class ExecutionInfoSubscriber {
public:
    // Takes ownership of the ClientContext and reader, runs a background thread
    // to consume the stream.
    ExecutionInfoSubscriber(
        std::unique_ptr<grpc::ClientContext> context,
        std::unique_ptr<grpc::ClientReader<sila2::org::silastandard::ExecutionInfo>> reader,
        ExecutionUpdateCallback callback);

    // Ensures the read thread is stopped and joined before the context/reader
    // it depends on are destroyed.
    ~ExecutionInfoSubscriber();

    // Check if the subscription is still active (stream not ended)
    [[nodiscard("caller expects to know whether the stream is still delivering updates")]]
    bool isActive() const;

    // Cancel the subscription (triggers stream cancellation = command cancel signal §3.3)
    void cancel();

    // Block until the stream ends (finished or error)
    void wait();

    // Last received status
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
