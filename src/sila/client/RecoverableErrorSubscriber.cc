// RecoverableErrorSubscriber.cc
#include "RecoverableErrorSubscriber.h"

#include <utility>

// grpcpp.h (included by the header) only forward-declares ClientReader<T>;
// the full definition (Read/Finish) lives in this header.
#include <grpcpp/support/sync_stream.h>
// Registers SerializationTraits<Message> for protobuf types so Read()/Finish()
// can (de)serialize Subscribe_RecoverableErrors_Responses; normally pulled in
// transitively by a generated *.grpc.pb.h, which ErrorRecoveryService.pb.h
// alone does not provide.
#include <grpcpp/impl/codegen/proto_utils.h>

namespace sila2 {

namespace errorrecovery_proto = sila2::org::silastandard::core::errorrecoveryservice::v2;

RecoverableErrorSubscriber::RecoverableErrorSubscriber(
    std::unique_ptr<grpc::ClientContext> context,
    std::unique_ptr<grpc::ClientReader<errorrecovery_proto::Subscribe_RecoverableErrors_Responses>> reader,
    RecoverableErrorCallback callback)
    : context_{std::move(context)},
      reader_{std::move(reader)},
      callback_{std::move(callback)},
      readThread_{[this] { readLoop(); }} {}

RecoverableErrorSubscriber::~RecoverableErrorSubscriber() {
    // Must cancel before joining: Read() blocks until data or stream end,
    // TryCancel() is what unblocks it if the server never sends more.
    cancel();
    wait();
}

bool RecoverableErrorSubscriber::isActive() const { return active_; }

void RecoverableErrorSubscriber::cancel() {
    // Flip active_ first so readLoop sees the cancellation was ours and
    // suppresses the synthetic empty-vector callback for the CANCELLED status.
    active_ = false;
    context_->TryCancel();
}

void RecoverableErrorSubscriber::wait() {
    if (readThread_.joinable()) {
        readThread_.join();
    }
}

void RecoverableErrorSubscriber::readLoop() {
    errorrecovery_proto::Subscribe_RecoverableErrors_Responses response;
    while (reader_->Read(&response)) {
        std::vector<RecoverableErrorInfo> errors;
        errors.reserve(response.recoverableerrors_size());
        for (const auto& wrapper : response.recoverableerrors()) {
            const auto& re = wrapper.recoverableerror();
            RecoverableErrorInfo info;
            info.errorIdentifier = re.erroridentifier().value();
            info.commandExecutionUuid = re.commandexecutionuuid().value();
            info.errorMessage = re.errormessage().value();
            info.commandIdentifier = re.commandidentifier().value();
            info.errorTime = types::fromProto(re.errortime());
            for (const auto& optWrapper : re.continuationoptions()) {
                const auto& co = optWrapper.continuationoption();
                info.continuationOptions.push_back(
                    {co.identifier().value(), co.description().value(),
                     co.requiredinputdata().value()});
            }
            info.defaultOption = re.defaultoption().value();
            info.automaticSelectionTimeoutSeconds =
                re.automaticselectiontimeout().timeout().value();
            errors.push_back(std::move(info));
        }
        callback_(errors);
    }

    const grpc::Status status = reader_->Finish();
    if (!status.ok() && active_) {
        // Only synthesize an empty-vector callback for failures the caller
        // didn't request themselves via cancel() (which already set active_ false).
        callback_({});
    }
    active_ = false;
}

}  // namespace sila2
