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

/// One outstanding recoverable error reported by the server's
/// ErrorRecoveryService: what went wrong, which
/// @ref gl_command_execution_uuid "Command Execution UUID" it blocks, and the
/// options the client can choose from to continue.
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

    /// One way the client can continue past a RecoverableErrorInfo.
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

/// Delivers the server's currently outstanding recoverable errors to a
/// caller-supplied callback, without the caller having to block on a gRPC
/// stream itself. This is the client side of the ErrorRecoveryService: the
/// server-side counterpart is RecoverableErrorGate. Obtained from a
/// client-side subscription helper that opened the stream; not constructed
/// directly by application code.
class RecoverableErrorSubscriber {
public:
    /// Takes ownership of `context` and `reader` and immediately starts a
    /// background thread that reads the stream and invokes `callback` with
    /// the current set of recoverable errors for every update until the
    /// stream ends.
    RecoverableErrorSubscriber(
        std::unique_ptr<grpc::ClientContext> context,
        std::unique_ptr<grpc::ClientReader<
            sila2::org::silastandard::core::errorrecoveryservice::v2::Subscribe_RecoverableErrors_Responses>> reader,
        RecoverableErrorCallback callback);

    /// Cancels the subscription (if still active) and joins the reader
    /// thread before the context/reader it depends on are destroyed.
    ~RecoverableErrorSubscriber();

    /// @return false once the stream has ended, whether it ended normally,
    /// with an error, or via cancel().
    [[nodiscard("caller expects to know whether the stream is still delivering updates")]]
    bool isActive() const;

    /// Cancels the subscription. Safe to call more than once.
    void cancel();
    /// Blocks until the stream has ended (normally, with an error, or via
    /// cancel()).
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
