#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include <sila/common/types/BasicTypes.h>

#include "ErrorRecoveryService.pb.h"

namespace sila2 {

struct RecoverableErrorInfo {
    std::string errorIdentifier;
    std::string commandExecutionUuid;
    std::string errorMessage;
    // Appended below (commandIdentifier, errorTime; Option.requiredInputData),
    // not inserted among the fields above: mirrors the append-only rule
    // RecoverableErrorGate.h:42-44 already documents for this same reason --
    // no caller in this tree builds either struct by positional aggregate
    // init, but an out-of-tree one might.
    std::string commandIdentifier;  // FDL RecoverableError.CommandIdentifier (S40)
    types::Timestamp errorTime{};   // FDL RecoverableError.ErrorTime (S40)

    struct Option {
        std::string identifier;
        std::string description;
        std::string requiredInputData;  // FDL ContinuationOption.RequiredInputData (S40)
    };
    std::vector<Option> continuationOptions;
    std::string defaultOption;
    int64_t automaticSelectionTimeoutSeconds = 0;
};

using RecoverableErrorCallback = std::function<void(const std::vector<RecoverableErrorInfo>&)>;

class RecoverableErrorSubscriber {
public:
    RecoverableErrorSubscriber(
        std::unique_ptr<grpc::ClientContext> context,
        std::unique_ptr<grpc::ClientReader<
            sila2::org::silastandard::core::errorrecoveryservice::v2::Subscribe_RecoverableErrors_Responses>> reader,
        RecoverableErrorCallback callback);

    ~RecoverableErrorSubscriber();

    [[nodiscard("caller expects to know whether the stream is still delivering updates")]]
    bool isActive() const;

    void cancel();
    void wait();

    RecoverableErrorSubscriber(const RecoverableErrorSubscriber&) = delete;
    RecoverableErrorSubscriber& operator=(const RecoverableErrorSubscriber&) = delete;

private:
    void readLoop();

    std::unique_ptr<grpc::ClientContext> context_;
    std::unique_ptr<grpc::ClientReader<
        sila2::org::silastandard::core::errorrecoveryservice::v2::Subscribe_RecoverableErrors_Responses>> reader_;
    RecoverableErrorCallback callback_;

    std::thread readThread_;
    std::atomic<bool> active_{true};
};

}  // namespace sila2
