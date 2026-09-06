// ObservableCommandRunner.cc — Observable Command orchestration (architecture.md §5)
#include <sila/client/dynamic/ObservableCommandRunner.h>

#include <sila/client/ExecutionStore.h>
#include <sila/client/SilaClientBase.h>
#include <sila/client/dynamic/DynamicCall.h>
#include <sila/client/dynamic/FeatureCatalog.h>

#include <grpcpp/impl/proto_utils.h>

#include "SiLAFramework.pb.h"

#include <chrono>
#include <utility>

namespace sila2 {
namespace dynamic {

namespace {

// Opaque to ExecutionStore — only used to give a persisted row an
// informational issue time, never parsed back. A plain epoch-seconds string
// avoids pulling a timestamp-formatting header into this TU for a field the
// store never inspects.
std::string isoNow() {
    const auto epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(epochSeconds);
}

// Steps 3-5 of the original pipeline, factored out so both
// executeObservableCommand (after issuing a new execution) and
// reattachObservableCommand (given a UUID from a prior run) can subscribe to
// the same _Info/_Result pair. `store`/`uuid` are consumed by value: the
// caller either just moved a freshly-issued uuid in, or passed a
// caller-owned executionUuid it doesn't need back.
ObservableCommandResult watchExecution(
    const std::shared_ptr<grpc::Channel>& channel,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    std::string uuid,
    ExecutionStore* store,
    ExecutionUpdateCallback onUpdate) {
    // Step 3: build the CommandExecutionUUID request reused by both _Info and _Result
    sila2::org::silastandard::CommandExecutionUUID uuidMsg;
    uuidMsg.set_value(uuid);
    grpc::ByteBuffer uuidBuf;
    bool ownBuffer = false;
    grpc::SerializationTraits<sila2::org::silastandard::CommandExecutionUUID>::Serialize(
        uuidMsg, &uuidBuf, &ownBuffer);

    // Step 4: subscribe to the _Info stream, forwarding updates to the caller
    CommandExecutionStatus lastStatus = CommandExecutionStatus::kWaiting;

    auto streamCb = [&](const grpc::ByteBuffer& message) -> bool {
        sila2::org::silastandard::ExecutionInfo info;
        // Copy needed: Deserialize takes a non-const ByteBuffer*
        grpc::ByteBuffer copy = message;
        auto status = grpc::SerializationTraits<
            sila2::org::silastandard::ExecutionInfo>::Deserialize(&copy, &info);
        if (!status.ok()) {
            return false;
        }

        ExecutionUpdate update;
        update.status = toCommandExecutionStatus(info.commandstatus());
        update.progress = info.has_progressinfo()
                               ? static_cast<float>(info.progressinfo().value())
                               : 0.0F;

        lastStatus = update.status;
        if (onUpdate) {
            onUpdate(update);
        }

        // Terminal states end the stream
        bool terminal = (update.status == CommandExecutionStatus::kFinishedSuccessfully
                      || update.status == CommandExecutionStatus::kFinishedWithError);
        return !terminal;
    };

    // Part A: an Observable Command's SiLA Client Metadata travels only with the
    // initiation call, never with _Info / _Result. Pass nullptr, not `injector`,
    // so no access token or other metadata is attached to the follow-up legs; the
    // server authorizes these against the token it snapshotted at initiation.
    grpc::Status infoStatus = callServerStream(
        channel, catalog.grpcMethodName(fqi, commandId + "_Info"),
        uuidBuf, streamCb, nullptr);

    // Step 5: fetch the result only if the command reached a successful terminal state
    if (lastStatus == CommandExecutionStatus::kFinishedSuccessfully) {
        grpc::ByteBuffer resultBuf;
        grpc::Status resultStatus = callUnary(
            channel, catalog.grpcMethodName(fqi, commandId + "_Result"),
            uuidBuf, &resultBuf, nullptr);  // follow-up leg: no metadata (see above)
        // Part A p33: the persisted row exists so a restart can still fetch
        // the result. Drop it only once the result is actually in hand; a
        // failed _Result leg (e.g. the connection dropped between _Info and
        // _Result) keeps the UUID recoverable via reattachObservableCommand.
        if (store != nullptr && resultStatus.ok()) {
            store->prune(uuid);
        }
        return ObservableCommandResult{std::move(uuid), std::move(resultBuf), resultStatus};
    }

    // An errored execution has no result to recover, so its row can go now
    // rather than growing the store forever.
    if (store != nullptr && lastStatus == CommandExecutionStatus::kFinishedWithError) {
        store->prune(uuid);
    }

    // Command did not finish successfully — return the info stream status
    return ObservableCommandResult{std::move(uuid), {}, infoStatus};
}

}  // namespace

ObservableCommandResult executeObservableCommand(
    const std::shared_ptr<grpc::Channel>& channel,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const google::protobuf::Message& request,
    ExecutionUpdateCallback onUpdate,
    ConstraintResolver constraintResolver,
    MetadataInjector* injector,
    ExecutionStore* store,
    std::string serverUuid) {
    // Step 1: execute the observable command (unary call returning a CommandConfirmation)
    grpc::ByteBuffer confirmationBuf;
    grpc::Status executeStatus = callUnary(
        channel, catalog, fqi, commandId, request, &confirmationBuf,
        std::move(constraintResolver), injector);
    if (!executeStatus.ok()) {
        return ObservableCommandResult{{}, {}, executeStatus};
    }

    // Step 2: deserialize CommandConfirmation, extract the execution UUID
    sila2::org::silastandard::CommandConfirmation confirmation;
    auto deserializeStatus = grpc::SerializationTraits<
        sila2::org::silastandard::CommandConfirmation>::Deserialize(
        &confirmationBuf, &confirmation);
    if (!deserializeStatus.ok()) {
        return ObservableCommandResult{{}, {}, deserializeStatus};
    }
    std::string uuid = confirmation.commandexecutionuuid().value();

    // Part A p33: record the UUID before the blocking watch below, not after,
    // so a client process that dies mid-watch still has this execution on
    // disk to reattach to once it restarts.
    if (store != nullptr) {
        PersistedExecution record{
            std::move(serverUuid), fqi, commandId, uuid, /*issueTime=*/isoNow(),
            /*lastStatus=*/"waiting"};
        store->record(record);
    }

    return watchExecution(channel, catalog, fqi, commandId, std::move(uuid), store,
                          std::move(onUpdate));
}

ObservableCommandResult reattachObservableCommand(
    const std::shared_ptr<grpc::Channel>& channel,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const std::string& executionUuid,
    ExecutionUpdateCallback onUpdate,
    ExecutionStore* store,
    std::string serverUuid) {
    // serverUuid is accepted for signature symmetry with executeObservableCommand
    // (and so a future caller has somewhere to pass it), but reattaching never
    // re-records: the row already exists from the original issue, and
    // watchExecution prunes it on terminal exactly as the first call would.
    (void)serverUuid;
    return watchExecution(channel, catalog, fqi, commandId, executionUuid, store,
                          std::move(onUpdate));
}

ObservableCommandResult executeObservableCommand(
    SilaClientBase& client,
    const std::string& serverUuid,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const google::protobuf::Message& request,
    ExecutionUpdateCallback onUpdate,
    ConstraintResolver constraintResolver) {
    return executeObservableCommand(client.channel(), catalog, fqi, commandId, request,
                                    std::move(onUpdate), std::move(constraintResolver),
                                    &client.metadataInjector(), client.executionStore(),
                                    serverUuid);
}

ObservableCommandResult reattachObservableCommand(
    SilaClientBase& client,
    const std::string& serverUuid,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const std::string& executionUuid,
    ExecutionUpdateCallback onUpdate) {
    return reattachObservableCommand(client.channel(), catalog, fqi, commandId, executionUuid,
                                     std::move(onUpdate), client.executionStore(), serverUuid);
}

}  // namespace dynamic
}  // namespace sila2
