// ExecutionInfoSubscriber.cc
#include "ExecutionInfoSubscriber.h"

#include <sila/common/error/SilaErrorException.h>

#include <utility>

// grpcpp.h (included by the header) only forward-declares ClientReader<T>;
// the full definition (Read/Finish) lives in this header.
#include <grpcpp/support/sync_stream.h>
// Registers SerializationTraits<Message> for protobuf types so Read()/Finish()
// can (de)serialize ExecutionInfo; normally pulled in transitively by a
// generated *.grpc.pb.h, which SiLAFramework.pb.h alone does not provide.
#include <grpcpp/impl/codegen/proto_utils.h>

namespace sila2 {

ExecutionInfoSubscriber::ExecutionInfoSubscriber(
    std::unique_ptr<grpc::ClientContext> context,
    std::unique_ptr<grpc::ClientReader<sila2::org::silastandard::ExecutionInfo>> reader,
    ExecutionUpdateCallback callback)
    : context_{std::move(context)},
      reader_{std::move(reader)},
      callback_{std::move(callback)},
      readThread_{[this] { readLoop(); }} {}

ExecutionInfoSubscriber::~ExecutionInfoSubscriber() {
    // Must cancel before joining: Read() blocks until data or stream end,
    // TryCancel() is what unblocks it if the server never sends more.
    cancel();
    wait();
}

bool ExecutionInfoSubscriber::isActive() const { return active_; }

void ExecutionInfoSubscriber::cancel() {
    // Flip active_ first so readLoop sees the cancellation was ours and
    // suppresses the synthetic error callback for the CANCELLED status.
    active_ = false;
    context_->TryCancel();
}

void ExecutionInfoSubscriber::wait() {
    if (readThread_.joinable()) {
        readThread_.join();
    }
}

CommandExecutionStatus ExecutionInfoSubscriber::lastStatus() const { return lastStatus_; }

void ExecutionInfoSubscriber::readLoop() {
    sila2::org::silastandard::ExecutionInfo info;
    while (reader_->Read(&info)) {
        ExecutionUpdate update;
        update.status = toCommandExecutionStatus(info.commandstatus());
        update.progress = info.has_progressinfo()
                               ? static_cast<float>(info.progressinfo().value())
                               : 0.0F;
        callback_(update);
        lastStatus_ = update.status;
    }

    const grpc::Status status = reader_->Finish();
    if (!status.ok() && active_) {
        // Only synthesize an error callback for failures the caller didn't
        // request themselves via cancel() (which already set active_ false).
        ExecutionUpdate update;
        update.status = CommandExecutionStatus::kFinishedWithError;
        update.progress = 0.0F;
        update.statusMessage = error::messageFromGrpcStatus(status);
        callback_(update);
        lastStatus_ = update.status;
    }
    active_ = false;
}

}  // namespace sila2
