// ObservableCommandRunner.h — Observable Command orchestration (architecture.md §5)
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/status.h>

#include <sila/client/CommandExecutionStatus.h>
#include <sila/client/dynamic/ValueValidator.h>

namespace sila2 { class MetadataInjector; }
namespace sila2 { class ExecutionStore; class SilaClientBase; }  // client/ExecutionStore.h; pointer param only, no include needed here
namespace google { namespace protobuf { class Message; }}

namespace sila2 {
namespace dynamic {

class FeatureCatalog;

struct ObservableCommandResult {
    std::string commandExecutionUuid;
    grpc::ByteBuffer result;
    grpc::Status status;
};

// `store`/`serverUuid` are trailing defaulted params (store=nullptr means the
// pre-existing no-persistence behaviour) so every pre-S36 caller compiles
// unchanged. When a store is supplied, the UUID is recorded on issue and
// pruned on terminal status (Part A p33 — see ObservableCommandRunner.cc).
[[nodiscard]]
ObservableCommandResult executeObservableCommand(
    const std::shared_ptr<grpc::Channel>& channel,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const google::protobuf::Message& request,
    ExecutionUpdateCallback onUpdate = {},
    ConstraintResolver constraintResolver = {},
    MetadataInjector* injector = nullptr,
    ExecutionStore* store = nullptr,
    std::string serverUuid = {});

// Re-subscribes to <Command>_Info and fetches <Command>_Result for a UUID
// already obtained — e.g. one returned by ExecutionStore::list() after a
// client restart (Part A p33). Does not re-initiate the command and carries
// no SiLA Client Metadata: a follow-up leg for an already-issued execution
// (same as the tail of executeObservableCommand after Step 2).
[[nodiscard]]
ObservableCommandResult reattachObservableCommand(
    const std::shared_ptr<grpc::Channel>& channel,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const std::string& executionUuid,
    ExecutionUpdateCallback onUpdate = {},
    ExecutionStore* store = nullptr,
    std::string serverUuid = {});

// Production wiring (Part A p33): the SilaClientBase overloads take the
// channel, the metadata injector and the ExecutionStore the client owns
// (ClientConfig::setExecutionStorePath), so a caller cannot configure
// persistence and then forget to pass the store. `serverUuid` is the UUID of
// the server behind `client` (SilaClientBase knows only host:port).
[[nodiscard]]
ObservableCommandResult executeObservableCommand(
    SilaClientBase& client,
    const std::string& serverUuid,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const google::protobuf::Message& request,
    ExecutionUpdateCallback onUpdate = {},
    ConstraintResolver constraintResolver = {});

[[nodiscard]]
ObservableCommandResult reattachObservableCommand(
    SilaClientBase& client,
    const std::string& serverUuid,
    FeatureCatalog& catalog,
    const std::string& fqi,
    const std::string& commandId,
    const std::string& executionUuid,
    ExecutionUpdateCallback onUpdate = {});

}  // namespace dynamic
}  // namespace sila2
