// CloudEnvelopeRouter.h — Routes incoming cloud envelope messages to SiLA handlers (architecture.md §3.9)
#pragma once

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/util/AsciiCase.h>
#include <sila/server/FeatureRegistry.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/cloud/ActiveCallRegistry.h>
#include <sila/server/transport/cloud/StreamWriteSerializer.h>

#include "SiLACloudConnector.pb.h"

#include <any>
#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sila2 {

class BinaryStore;
class ObservableCommandExecution;
class ObservableCommandManager;
class ObservablePropertyManager;
class Subscription;
struct InterceptorChain;
namespace cloud = org::silastandard;

enum class CloudErrorField {
    kCommandError,
    kPropertyError,
};

/// Type-erased Command/Property handler for the cloud path: the envelope
/// only carries request bytes, so the concrete request/response types are
/// hidden behind deserialize-dispatch-serialize closures built at
/// registration time (see CloudTransport, which owns the typed handlers).
using CloudDispatchFn = std::function<void(
    const std::string& parameterBytes,
    CallContext& ctx,
    StreamWriteSerializer& writer,
    const std::string& requestUUID)>;

// FQI handler maps compare keys case-insensitively (Part A p87): a client may
// send a case variant of the canonical FQI the adapter registered.
using CloudDispatchMap = std::map<std::string, CloudDispatchFn, util::CaseInsensitiveLess>;

/// Converts an ObservablePropertyManager publish value (std::any) to the
/// serialized bytes for ObservablePropertyValue on the cloud wire.
using CloudValueSerializer = std::function<std::string(const std::any&)>;

/// Dispatches incoming envelopes on a @ref gl_connection_method "Server-Initiated Connection"
/// (cloud connectivity) stream to the same
/// Command/Property handlers the direct-gRPC path runs (architecture.md
/// §3.9). One router is shared by every CloudTransport a server has open. A
/// server author does not construct or call this directly: SiLAServerBase
/// builds and populates it from the Features registered via
/// SiLAServerBase::Builder::AddFeature, and CloudTransport is what drives it
/// per connection.
class CloudEnvelopeRouter {
public:
    /// Constructs the router for one server. Installed automatically by
    /// SiLAServerBase::Builder::Build() -- a server author never constructs one directly.
    /// @param registry Feature registry this server exposes; must outlive this object.
    /// @param chain Interceptor bundle for auth/metadata admission checks; must outlive this
    /// object.
    /// @param binaryStore Backing store for inline Binary Transfer values on the cloud path,
    ///        or nullptr when WithBinaryTransfer() was not called.
    /// @param observableCommands Every registered ObservableCommandManager, so findExecution()
    ///        can resolve a Command Execution UUID across all of them.
    /// @param cloudWriteTimeout Per-write timeout applied to every StreamWriteSerializer this
    ///        router creates; see cloudWriteTimeout().
    /// @param maxConcurrentSubscriptions Cap on simultaneous long-running cloud pumps
    ///        (subscriptions and Observable Command follow-ups) this router allows.
    explicit CloudEnvelopeRouter(FeatureRegistry& registry,
                                 const InterceptorChain* chain = nullptr,
                                 BinaryStore* binaryStore = nullptr,
                                 std::vector<ObservableCommandManager*> observableCommands = {},
                                 std::chrono::seconds cloudWriteTimeout = std::chrono::seconds{0},
                                 // Default lets tests construct the router directly with a
                                 // small cap; production wires ServerConfig's value instead.
                                 std::size_t maxConcurrentSubscriptions = 64);

    /// Cancels every live observable-property subscription and joins its pump
    /// thread. Not defaulted: those threads used to be detached, so an
    /// exception escaping one terminated the process and nothing drained
    /// them at shutdown (§4.2h).
    ~CloudEnvelopeRouter();

    /// Whether at least one ObservableCommandManager was registered at
    /// construction, i.e. this server has @ref gl_observable_command "Observable Commands" the
    /// router can look executions up on.
    bool hasObservableCommands() const { return !observableCommands_.empty(); }
    /// Looks up a running @ref gl_observable_command "Observable Command"
    /// execution by its @ref gl_command_execution_uuid "Command Execution UUID".
    /// @throws error::FrameworkError (InvalidCommandExecutionUuid) if no
    /// registered ObservableCommandManager knows this UUID.
    std::shared_ptr<ObservableCommandExecution> findExecution(const std::string& uuid) const;

    /// The write timeout this router was constructed with, applied to every
    /// StreamWriteSerializer it creates for a cloud stream.
    std::chrono::seconds cloudWriteTimeout() const { return cloudWriteTimeout_; }

    /// Registers the dispatch closure for one unobservable command's fully
    /// qualified identifier, e.g.
    /// `org.silastandard/core/SiLAService/v1/Command/GetFeatureDefinition`.
    /// Called by CloudHandlerRegistration.h's regCmd for each command a
    /// Feature adapter exposes; not called directly by a server author.
    void registerCommandHandler(const std::string& fqi, CloudDispatchFn handler);
    /// Registers the dispatch closure for one unobservable property's fully
    /// qualified identifier. Called by CloudHandlerRegistration.h's regProp.
    void registerPropertyHandler(const std::string& fqi, CloudDispatchFn handler);

    /// Registers a codegen'd Subscribe_X handler for an observable property
    /// (1.2k). Distinct from registerObservableProperty below: that one hands
    /// the router an ObservablePropertyManager and a value serializer and the
    /// router owns the subscribe loop; this one hands over the application's
    /// own streaming handler, which the router runs on a pump thread. A generated
    /// adapter can only supply the latter. Manager-based entries take precedence
    /// when the same FQI is registered both ways.
    void registerObservablePropertyHandler(const std::string& fqi, CloudDispatchFn handler);

    /// Registers an @ref gl_observable_property "Observable Property" whose
    /// @ref gl_property_subscription "Property Subscription" the router owns
    /// end to end: it takes new values from `mgr`, converts them with
    /// `serializer`, and writes one envelope per value itself. Takes
    /// precedence over registerObservablePropertyHandler for the same fqi.
    void registerObservableProperty(const std::string& fqi,
                                    const std::string& propertyId,
                                    ObservablePropertyManager* mgr,
                                    CloudValueSerializer serializer);

    // accessToken defaulted to nullopt: keeps every pre-S15 2-arg call site
    // compiling. wrapObsInit (CloudHandlerRegistration.h) is the only
    // production caller and always passes the initiation's token.
    /// Records which command fqi an @ref gl_observable_command "Observable Command" execution
    /// belongs to, and (when given) the access token its
    /// initiating call presented, so a later _Info/_Intermediate/_Result
    /// envelope for the same @ref gl_command_execution_uuid "Command Execution UUID" can be routed
    /// and re-authorized without carrying its
    /// own SiLA Client Metadata. Called by CloudHandlerRegistration.h's
    /// wrapObsInit when an execution is initiated.
    void registerExecutionFQI(const std::string& uuid, const std::string& fqi,
                              std::optional<std::string> accessToken = std::nullopt);
    /// Drops an execution's fqi/access-token entry, e.g. once its result has
    /// been delivered.
    void removeExecutionFQI(const std::string& uuid);

    /// Dispatches a single incoming SiLAClientMessage envelope to the
    /// matching Command or Property handler, and writes the response (or
    /// error) envelope back through `writer`. A subscription or a
    /// long-running Observable Command follow-up runs on its own pump
    /// thread instead of blocking this call. Safe to call concurrently with
    /// registerCommandHandler/registerPropertyHandler, and from multiple
    /// threads at once (e.g. one CloudTransport receive thread and its own
    /// reconnect() verification read). Called by CloudTransport for every
    /// envelope it reads off the stream.
    void route(const cloud::SiLAClientMessage& msg,
               StreamWriteSerializer& writer,
               std::shared_ptr<StreamWriteSerializer> writerOwnership,
               ActiveCallRegistry& calls);

private:
    std::shared_ptr<CallContext> makeCloudCallContext(
        const google::protobuf::RepeatedPtrField<cloud::Metadata>& metadata);

    // writer is a shared_ptr, not a reference: route()'s writerOwnership
    // always aliases its writer reference (CloudTransport.cc and every test
    // harness call pass the same object as both), so carrying one instead of
    // the pair states that assumption once instead of twice. The pump this
    // dispatches into outlives the route() call, so it needs shared, not
    // borrowed, ownership.
    void dispatchObservableByUuid(const std::string& executionUuid,
                                  const std::string& handlerSuffix,
                                  const std::string& requestUUID,
                                  std::shared_ptr<StreamWriteSerializer> writer,
                                  ActiveCallRegistry& calls);

    void dispatchTo(const CloudDispatchMap& handlers,
                    const std::string& fqi,
                    const std::string& parameterBytes,
                    const google::protobuf::RepeatedPtrField<cloud::Metadata>& metadata,
                    CloudErrorField errorField,
                    const std::string& requestUUID,
                    StreamWriteSerializer& writer,
                    ActiveCallRegistry& calls);

    bool guardBinaryOp(StreamWriteSerializer& writer,
                       const std::string& requestUUID,
                       const std::string& uuid,
                       cloud::BinaryTransferError::ErrorType failType);

    /// Builds the follow-up CallContext for an execution's initiation token
    /// and runs the auth gate under `fqi` (the base command FQI, never
    /// fqi + a follow-up suffix — see the makeFollowupContext definition).
    /// Returns nullptr — having already written the access-denied
    /// commandError envelope — when the gate rejects.
    std::shared_ptr<CallContext> makeFollowupContext(const std::string& fqi,
                                                     const std::optional<std::string>& accessToken,
                                                     const std::string& requestUUID,
                                                     StreamWriteSerializer& writer);

    /// Runs the SiLA Client Metadata admission gate for one cloud envelope.
    /// Returns false -- having already written the rejection envelope -- when
    /// the call is refused. Two call sites, so the try/catch and the envelope
    /// shape are written once.
    bool admitMetadata(const std::string& fqi,
                       const google::protobuf::RepeatedPtrField<cloud::Metadata>& metadata,
                       CloudErrorField errorField,
                       const std::string& requestUUID,
                       StreamWriteSerializer& writer);

    /// Everything startPump() needs that differs per envelope kind.
    struct PumpStart {
        std::string requestUUID;
        std::shared_ptr<CallContext> ctx;
        // Shared, not a reference: the pump outlives the route() call that
        // spawned it, and CloudTransport resets its writer_ on reconnect.
        std::shared_ptr<StreamWriteSerializer> writer;
        // Null for command pumps; only property pumps own a Subscription.
        std::shared_ptr<Subscription> subscription;
        CloudErrorField errorField;
        // Names this pump in its failure log/envelope (an FQI, or the
        // execution UUID for the router-synthesized ExecutionInfo stream).
        std::string label;
        std::function<void()> body;
    };

    /// Registers requestUUID in `calls` and runs `body` on a joined pump
    /// thread, so no long-running handler ever occupies the receive loop.
    /// Returns false — having already written the rejection envelope — when
    /// the requestUUID already has a live pump (§2.2n) or the concurrent-pump
    /// cap is reached (§3.2q). The caller must undo whatever it set up before
    /// calling (the property branch cancels its ctx, which unsubscribes).
    bool startPump(PumpStart req, ActiveCallRegistry& calls);

    /// Writes the duplicate-requestUUID rejection and returns true when
    /// requestUUID already has a live call context. Reaps first: a
    /// finished-but-unreaped pump still holds a strong ctx in its thread's
    /// captured lambda, so find() alone reports a dead requestUUID as live.
    bool rejectDuplicateRequestUUID(ActiveCallRegistry& calls,
                                    const std::string& requestUUID,
                                    CloudErrorField errorField,
                                    StreamWriteSerializer& writer);

    /// One live long-running cloud pump. The weak_ptr is the cancel target at
    /// shutdown; `finished` is set by the pump as its last act so
    /// reapFinishedSubscriptions() can join it without blocking. `subscription`
    /// is empty for command pumps (ExecutionInfo / _Intermediate / _Result),
    /// which cancel purely through `ctx`.
    struct SubscriptionThread {
        std::thread thread;
        std::weak_ptr<Subscription> subscription;
        // Shutdown cancels through this, not `subscription` directly, so it
        // takes the same requestCancellation() -> onCancellation -> unsubscribe
        // path a normal pump exit does, instead of bypassing the unsubscribe
        // hop (§2.1h's sibling leak: ObservablePropertyManager never shrank
        // its subscriber vector because nothing ever called unsubscribe()).
        std::weak_ptr<CallContext> ctx;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    /// Joins the pumps that have already exited. Called before each spawn so
    /// a long-lived cloud stream does not accumulate one unjoined thread per
    /// completed subscription.
    void reapFinishedSubscriptions();

    struct CloudObservablePropEntry {
        std::string propertyId;
        ObservablePropertyManager* mgr;
        CloudValueSerializer serializer;
    };

    FeatureRegistry& registry_;
    const InterceptorChain* chain_;
    BinaryStore* binaryStore_;
    std::vector<ObservableCommandManager*> observableCommands_;
    CloudDispatchMap commandHandlers_;
    CloudDispatchMap propertyHandlers_;
    std::map<std::string, CloudObservablePropEntry, util::CaseInsensitiveLess> observableProps_;
    // 1.2k: codegen'd Subscribe_ handlers. Kept apart from propertyHandlers_,
    // which kUnobservablePropertyRead also reads -- a forever-streaming handler
    // in that map would run inline on the receive loop for a one-shot read.
    CloudDispatchMap observablePropHandlers_;

    // What a follow-up envelope needs to re-run the auth gate. The token is
    // the one the ObservableCommandInitiation presented: the follow-up
    // messages carry no metadata field (SiLACloudConnector.proto:79-87), so
    // this is the only credential the gate can ever see for _Info,
    // _Intermediate and _Result (§S15, owner option 1).
    //
    // ponytail: this binds the gate to the EXECUTION, not the requester. The
    // router is shared by every CloudTransport, so a second end-client behind
    // the same cloud endpoint that learns a CommandExecutionUUID inherits the
    // first client's authorization for that execution's follow-ups -- the
    // wire gives it no slot to present its own credential. Same root,
    // second limit (S27): a snapshot token that expires mid-execution cannot
    // be refreshed over the wire either, so a long-running execution whose
    // follow-up traffic stops sliding the expiry loses its result for good.
    // Not closable at this layer. Upgrade path: per-connection execution
    // ownership, once a cloud client identity outlives a single
    // CloudTransport -- it removes both limits.
    struct ExecutionEntry {
        std::string fqi;
        std::optional<std::string> accessToken;
    };
    // Part A p90 / Part B p88: UUID comparison MUST ignore case.
    std::map<std::string, ExecutionEntry, util::CaseInsensitiveLess> executionFqis_;
    std::chrono::seconds cloudWriteTimeout_;
    // Bounds every long-running cloud pump, not only the wire kinds the proto
    // calls subscriptions: an in-flight ObservableCommandGetResponse holds a
    // pump slot too, and it has no Cancel message (SiLACloudConnector.proto:126-131).
    // Per-router, i.e. shared by every CloudTransport the server has open.
    const std::size_t maxConcurrentSubscriptions_;
    std::mutex mu_;

    // Its own mutex, not mu_: reap/destroy join threads while holding it, and
    // mu_ is taken on every route() dispatch — sharing one would stall all
    // dispatch behind a join.
    std::vector<SubscriptionThread> subscriptionThreads_;
    std::mutex subscriptionThreadsMu_;
};

}  // namespace sila2
