// CloudEnvelopeRouter.cc
#include "CloudEnvelopeRouter.h"

#include <sila/common/binary/BinaryChunkLimit.h>
#include <sila/common/util/MetadataHeaderKey.h>
#include <sila/server/LogCallback.h>
#include <sila/server/auth/AuthorizationInterceptor.h>
#include <sila/server/auth/FqiMatch.h>
#include <sila/server/binary/BinaryStore.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/command/ObservableCommandManager.h>
#include <sila/server/features/AuthenticationServiceImpl.h>
#include <sila/server/metadata/MetadataPolicy.h>
#include <sila/server/property/ObservablePropertyManager.h>
#include <sila/server/transport/InterceptorChain.h>

// SiLA 2 wire format for the standard AccessToken metadata is the serialized
// Metadata_AccessToken message (see SilaClientBase.cc / MetadataExtractingInterceptor.cc),
// not the bare token — makeCloudCallContext has to unwrap it the same way.
#include "AuthorizationService.pb.h"

#ifdef SILA_ENABLE_OTEL
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#endif

#include <algorithm>
#include <optional>
#include <thread>
#include <utility>

namespace sila2 {

namespace {

static constexpr auto kDefaultBinaryLifetime = std::chrono::seconds{300};

// Matches the direct-gRPC _Info handler's poll cadence (ShakeControllerImpl.cc),
// so cancellation latency for the cloud-synthesized _Info stream is the same
// on both transports.
static constexpr auto kExecutionInfoPollInterval = std::chrono::milliseconds{200};

// The gRPC gate reads the access token under this raw key (see
// AuthorizationInterceptor.cc). Cloud envelopes carry metadata under the
// shared kAccessTokenMetadataFqi (MetadataHeaderKey.h), so
// makeCloudCallContext normalizes that FQI to this key.
const std::string kAccessTokenRawKey = "access-token";

/// Serializes err and sends it on the oneof field the calling branch owns —
/// the one shared shape under sendFrameworkError and sendExecutionError.
void sendSiLAErrorEnvelope(StreamWriteSerializer& writer,
                           const std::string& requestUUID,
                           CloudErrorField errorField,
                           const error::SiLAError& err) {
    cloud::SiLAServerMessage errMsg;
    errMsg.set_requestuuid(requestUUID);
    auto silaError = err.toProto();
    switch (errorField) {
        case CloudErrorField::kCommandError:  *errMsg.mutable_commanderror() = *silaError; break;
        case CloudErrorField::kPropertyError: *errMsg.mutable_propertyerror() = *silaError; break;
    }
    writer.write(errMsg);
}

/// Sends an UndefinedExecutionError envelope on the oneof field the calling
/// branch owns. Used where a pump BODY threw: on the gRPC path the identical
/// escape becomes an UndefinedExecutionError (ErrorTransmitInterceptor.h:28-32),
/// so reporting it as a FrameworkError made the two transports disagree about
/// what kind of failure the client just saw.
/// Builds and sends a COMMAND_EXECUTION_NOT_ACCEPTED FrameworkError envelope
/// on the oneof field the calling branch owns. Every call site means the same
/// thing — the server refuses to run this call: no registered handler, a
/// requestUUID already live, the subscription cap, observable commands not
/// configured.
void sendFrameworkError(StreamWriteSerializer& writer,
                        const std::string& requestUUID,
                        CloudErrorField errorField,
                        const std::string& message) {
    sendSiLAErrorEnvelope(writer, requestUUID, errorField,
        error::FrameworkError{error::FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted,
                              message});
}

void sendExecutionError(StreamWriteSerializer& writer,
                        const std::string& requestUUID,
                        CloudErrorField errorField,
                        const std::string& message) {
    sendSiLAErrorEnvelope(writer, requestUUID, errorField, error::UndefinedExecutionError{message});
}

/// Builds and sends the FrameworkError envelope for an FQI with no
/// registered handler. errorField picks commandError vs propertyError.
void sendNoHandlerError(StreamWriteSerializer& writer,
                         const std::string& requestUUID,
                         const std::string& fqi,
                         CloudErrorField errorField) {
    sendFrameworkError(writer, requestUUID, errorField, "no handler registered for: " + fqi);
}

/// Builds and sends the BinaryTransferError envelope for a failed binary op.
void sendBinaryTransferError(StreamWriteSerializer& writer,
                              const std::string& requestUUID,
                              cloud::BinaryTransferError::ErrorType type,
                              const std::string& message) {
    cloud::SiLAServerMessage msg;
    msg.set_requestuuid(requestUUID);
    auto* err = msg.mutable_binarytransfererror();
    err->set_errortype(type);
    err->set_message(message);
    writer.write(msg);
}

/// Removes requestUUID from calls on scope exit, including via exception, so
/// a throwing handler still frees its ActiveCallRegistry entry instead of
/// leaving it for the next add() to prune (§2.1h). Handlers currently never
/// throw past guardHandler (CloudHandlerRegistration.h), but the plain
/// `calls.remove()` right after `handler(...)` only means what it reads if a
/// future handler is guaranteed to reach it.
class CallRegistryGuard {
public:
    CallRegistryGuard(ActiveCallRegistry& calls, std::string requestUUID)
        : calls_{calls}, requestUUID_{std::move(requestUUID)} {}
    ~CallRegistryGuard() { calls_.remove(requestUUID_); }

private:
    ActiveCallRegistry& calls_;
    std::string requestUUID_;
};

/// One reading of an execution's observable state. Compared to decide whether
/// the _Info pump has anything new to send (architecture-v2.md:288,293 —
/// _Info shares the observable-property policy: push on subscribe, then only
/// on change).
struct InfoSnapshot {
    ObservableCommandExecution::State state;
    double progress;
    std::chrono::seconds remaining;
    bool operator==(const InfoSnapshot&) const = default;
};

/// Builds the ObservableCommandExecutionInfo envelope from a snapshot rather
/// than from the execution: state(), progress() and estimatedRemaining() each
/// take the execution's lock separately, so re-reading here could mix a
/// pre-transition state with a post-transition progress.
cloud::SiLAServerMessage buildExecutionInfo(const std::string& requestUUID,
                                            const std::string& executionUuid,
                                            const InfoSnapshot& snap,
                                            std::chrono::seconds lifetime) {
    cloud::SiLAServerMessage resp;
    resp.set_requestuuid(requestUUID);
    auto* body = resp.mutable_observablecommandexecutioninfo();
    body->mutable_commandexecutionuuid()->set_value(executionUuid);

    auto* info = body->mutable_executioninfo();
    switch (snap.state) {
        case ObservableCommandExecution::State::Waiting:
            info->set_commandstatus(cloud::ExecutionInfo::waiting);
            break;
        case ObservableCommandExecution::State::Running:
            info->set_commandstatus(cloud::ExecutionInfo::running);
            // Only in Running: progressInfo and estimatedRemainingTime are
            // Real/Duration messages (SiLAFramework.proto:80-81), so leaving
            // them unset says "not reported" while setting them to zero says
            // "0% done, 0s left" -- what waiting and terminal snapshots claimed.
            info->mutable_progressinfo()->set_value(snap.progress);
            info->mutable_estimatedremainingtime()->set_seconds(snap.remaining.count());
            break;
        case ObservableCommandExecution::State::FinishedSuccessfully:
            info->set_commandstatus(cloud::ExecutionInfo::finishedSuccessfully);
            break;
        case ObservableCommandExecution::State::FinishedWithError:
            info->set_commandstatus(cloud::ExecutionInfo::finishedWithError);
            break;
    }
    if (lifetime > std::chrono::seconds{0}) {
        // SiLAFramework.proto:82, same zero-means-unset rule as
        // ShakeControllerImpl.cc's onShakeForTime[Info]. A parameter, not an
        // InfoSnapshot field: lifetime is constant per execution, so folding
        // it into the snapshot would add a field to operator== that can never
        // differ and so can never defeat the send-only-on-change comparison
        // above -- but it also must never look like it does.
        info->mutable_updatedlifetimeofexecution()->set_seconds(lifetime.count());
    }
    return resp;
}

}  // namespace

CloudEnvelopeRouter::CloudEnvelopeRouter(FeatureRegistry& registry,
                                         const InterceptorChain* chain,
                                         BinaryStore* binaryStore,
                                         std::vector<ObservableCommandManager*> observableCommands,
                                         std::chrono::seconds cloudWriteTimeout,
                                         std::size_t maxConcurrentSubscriptions)
    : registry_{registry}, chain_{chain},
      binaryStore_{binaryStore}, observableCommands_{std::move(observableCommands)},
      cloudWriteTimeout_{cloudWriteTimeout},
      maxConcurrentSubscriptions_{maxConcurrentSubscriptions} {}

CloudEnvelopeRouter::~CloudEnvelopeRouter() {
    std::lock_guard<std::mutex> lock(subscriptionThreadsMu_);

    // Two passes, not one: every pump is parked in Subscription::waitForNext(),
    // which returns only once the subscription is cancelled, so joining before
    // cancelling would deadlock. Cancelling all of them first also lets them
    // unwind in parallel instead of one join at a time.
    for (auto& entry : subscriptionThreads_) {
        // requestCancellation(), not subscription->cancel(): the latter wakes
        // waitForNext() but skips onCancellation, so the pump would exit
        // without ever unsubscribing from ObservablePropertyManager. Falling
        // back to a direct cancel() when ctx is already gone still wakes the
        // pump so this loop cannot hang waiting for it.
        auto ctx = entry.ctx.lock();
        if (ctx) {
            ctx->requestCancellation();
        } else if (auto subscription = entry.subscription.lock()) {
            subscription->cancel();
        }
    }
    // Bounded by the pump's in-flight write(): with cloudWriteTimeout == 0 the
    // serializer runs no watchdog, so a stalled Write() delays shutdown here.
    // Configuring a non-zero cloudWriteTimeout is the fix; a timed join would
    // only trade the stall for a detached thread, which is what §4.2h is about.
    for (auto& entry : subscriptionThreads_) {
        if (entry.thread.joinable()) {
            entry.thread.join();
        }
    }
}

void CloudEnvelopeRouter::reapFinishedSubscriptions() {
    std::lock_guard<std::mutex> lock(subscriptionThreadsMu_);
    for (auto it = subscriptionThreads_.begin(); it != subscriptionThreads_.end();) {
        if (it->finished->load(std::memory_order_acquire)) {
            it->thread.join();
            it = subscriptionThreads_.erase(it);
        } else {
            ++it;
        }
    }
}

bool CloudEnvelopeRouter::rejectDuplicateRequestUUID(ActiveCallRegistry& calls,
                                                     const std::string& requestUUID,
                                                     CloudErrorField errorField,
                                                     StreamWriteSerializer& writer) {
    // Reap before the liveness check: a finished-but-unreaped pump still holds a
    // strong ctx inside its std::thread's captured lambda, so find() would report
    // a dead requestUUID as live -- and startPump's cap check below would count a
    // dead pump against the cap.
    reapFinishedSubscriptions();

    // 2.2n / 2.2o: ActiveCallRegistry::add silently overwrites, which would leave
    // the first pump streaming under a requestUUID no Cancel envelope can reach.
    // Reject instead -- SiLA has no "subscription replaced" wire signal.
    // find()-then-add() needs no atomicity: `calls` belongs to exactly one
    // CloudTransport and only that transport's receive loop ever calls add().
    if (!calls.find(requestUUID)) {
        return false;
    }
    sendFrameworkError(writer, requestUUID, errorField,
                       "requestUUID already has a live subscription: " + requestUUID);
    return true;
}

bool CloudEnvelopeRouter::startPump(PumpStart req, ActiveCallRegistry& calls) {
    if (rejectDuplicateRequestUUID(calls, req.requestUUID, req.errorField, *req.writer)) {
        return false;
    }

    auto finished = std::make_shared<std::atomic<bool>>(false);
    // A copy, not chain_->logCallback by reference: the pump outlives this
    // call and must not reach back into the router or the chain.
    auto log = chain_ ? chain_->logCallback : LogCallback{};

    // Registered before the spawn so a Cancel* arriving on the very next read
    // finds the entry. Never removed by the pump itself: ConnectionConfiguration
    // Service destroys each CloudTransport — and the ActiveCallRegistry it owns —
    // while this router and its pumps are still alive, so a pump thread must
    // not hold a reference to `calls`. The registry stores a weak_ptr and
    // add()'s prune collects it once the joined thread's captures are gone.
    calls.add(req.requestUUID, req.ctx);

    bool admitted = false;
    {
        // One hold covers the size check and the push_back: this router is
        // shared by every CloudTransport (ConnectionConfigurationServiceImpl
        // creates one transport per connected cloud client), so N receive
        // loops can reach this concurrently and a check outside the hold
        // could overshoot the cap. No write() inside the hold — a write can
        // block for cloudWriteTimeout and would stall the destructor's join.
        // Sound to construct the thread here because no pump body re-enters
        // the router's subscription machinery: property pumps touch only the
        // Subscription and the writer, the _Info pump only the execution and
        // the writer, and the follow-up pumps only a registered handler.
        std::lock_guard<std::mutex> lock(subscriptionThreadsMu_);
        if (subscriptionThreads_.size() < maxConcurrentSubscriptions_) {
            std::thread pump([body = std::move(req.body), ctx = req.ctx,
                              writer = req.writer, requestUUID = req.requestUUID,
                              errorField = req.errorField, label = req.label,
                              log, finished] {
                // Reporting must not throw either — it would escape the pump
                // and terminate the process, the very thing §4.2h is about.
                auto report = [&](const std::string& what) {
                    try {
                        logEvent(log, LogLevel::kError, "dispatch", label + " failed: " + what);
                        sendExecutionError(*writer, requestUUID, errorField, what);
                    } catch (...) {
                    }
                };
                try {
                    body();
                } catch (const std::exception& e) {
                    report(e.what());
                } catch (...) {
                    report("unknown exception");
                }
                // Fires the onCancellation callback a property pump installed,
                // which unsubscribes from ObservablePropertyManager. Needed on
                // every exit path, not just the cancelled one (§2.1h's sibling
                // leak). Fire-once, so ~CloudEnvelopeRouter calling it again is
                // a harmless no-op, and a no-op for command pumps.
                ctx->requestCancellation();
                // Last statement on every path: reapFinishedSubscriptions()
                // joins only threads that have set this.
                finished->store(true, std::memory_order_release);
            });
            subscriptionThreads_.push_back(
                SubscriptionThread{std::move(pump), req.subscription, req.ctx, finished});
            admitted = true;
        }
    }

    if (!admitted) {
        calls.remove(req.requestUUID);
        sendFrameworkError(*req.writer, req.requestUUID, req.errorField,
                           "too many concurrent cloud subscriptions (max " +
                               std::to_string(maxConcurrentSubscriptions_) + ")");
        return false;
    }
    return true;
}

void CloudEnvelopeRouter::registerCommandHandler(const std::string& fqi, CloudDispatchFn handler) {
    std::lock_guard<std::mutex> lock(mu_);
    commandHandlers_[fqi] = std::move(handler);
}

void CloudEnvelopeRouter::registerPropertyHandler(const std::string& fqi, CloudDispatchFn handler) {
    std::lock_guard<std::mutex> lock(mu_);
    propertyHandlers_[fqi] = std::move(handler);
}

void CloudEnvelopeRouter::registerObservablePropertyHandler(const std::string& fqi,
                                                            CloudDispatchFn handler) {
    std::lock_guard<std::mutex> lock(mu_);
    observablePropHandlers_[fqi] = std::move(handler);
}

void CloudEnvelopeRouter::registerObservableProperty(
    const std::string& fqi, const std::string& propertyId,
    ObservablePropertyManager* mgr, CloudValueSerializer serializer) {
    std::lock_guard<std::mutex> lock(mu_);
    observableProps_[fqi] = {propertyId, mgr, std::move(serializer)};
}

void CloudEnvelopeRouter::registerExecutionFQI(const std::string& uuid, const std::string& fqi,
                                               std::optional<std::string> accessToken) {
    std::lock_guard<std::mutex> lock(mu_);
    executionFqis_[uuid] = ExecutionEntry{fqi, std::move(accessToken)};
}

void CloudEnvelopeRouter::removeExecutionFQI(const std::string& uuid) {
    std::lock_guard<std::mutex> lock(mu_);
    // Erasing the entry now also drops the execution's stored access token
    // (§S15), not just the FQI bookkeeping it used to be.
    executionFqis_.erase(uuid);
}

std::shared_ptr<ObservableCommandExecution> CloudEnvelopeRouter::findExecution(
    const std::string& uuid) const {
    for (auto* mgr : observableCommands_) {
        try {
            return mgr->getCommand(uuid);
        } catch (const error::SiLAError&) {
        }
    }
    throw error::FrameworkError{
        error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid,
        "unknown execution UUID: " + uuid};
}

void CloudEnvelopeRouter::dispatchObservableByUuid(
    const std::string& executionUuid,
    const std::string& handlerSuffix,
    const std::string& requestUUID,
    std::shared_ptr<StreamWriteSerializer> writer,
    ActiveCallRegistry& calls) {

    std::string fqi;
    std::optional<std::string> accessToken;
    CloudDispatchFn handler;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto fqiIt = executionFqis_.find(executionUuid);
        if (fqiIt != executionFqis_.end()) {
            fqi = fqiIt->second.fqi;
            accessToken = fqiIt->second.accessToken;
            auto hIt = commandHandlers_.find(fqi + handlerSuffix);
            if (hIt != commandHandlers_.end()) {
                handler = hIt->second;
            }
        }
    }

    // S28: the gate answers before findExecution and the two guards below.
    // Their InvalidCommandExecutionUuid / "no FQI" / "no handler" replies
    // would otherwise let an unauthorized caller holding a UUID enumerate
    // execution existence and handler registration. An execution with no
    // registered FQI has nothing to match protectedFqis against, so only
    // the registered case gates -- and gates first.
    std::shared_ptr<CallContext> ctx;
    if (!fqi.empty()) {
        // Re-runs the gate that ObservableCommandInitiation already passed, on
        // the base command FQI carried in executionFqis_ (§S15) -- never on
        // fqi + handlerSuffix, which a protectedFqis entry naming the exact
        // command would not cover (FqiMatch.h's '/' boundary rule).
        ctx = makeFollowupContext(fqi, accessToken, requestUUID, *writer);
        if (!ctx) {
            return;
        }
    }

    try {
        findExecution(executionUuid);
    } catch (const error::SiLAError& e) {
        cloud::SiLAServerMessage errMsg;
        errMsg.set_requestuuid(requestUUID);
        *errMsg.mutable_commanderror() = *e.toProto();
        writer->write(errMsg);
        return;
    }

    if (fqi.empty()) {
        sendFrameworkError(*writer, requestUUID, CloudErrorField::kCommandError,
            "no command FQI registered for execution: " + executionUuid);
        return;
    }
    if (!handler) {
        sendNoHandlerError(*writer, requestUUID,
            fqi + handlerSuffix, CloudErrorField::kCommandError);
        return;
    }

    cloud::CommandExecutionUUID uuidProto;
    uuidProto.set_value(executionUuid);
    std::string paramBytes;
    uuidProto.SerializeToString(&paramBytes);

    const std::string label = fqi + handlerSuffix;
    auto log = chain_ ? chain_->logCallback : LogCallback{};

    // §1.2g: the codegen/reference contract has _Result block until the
    // command completes (ShakeControllerImpl.cc:180-201), and _Intermediate
    // stream until cancelled — running either inline would freeze every other
    // cloud dispatch on this stream, including the Cancel envelope that is the
    // only way to interrupt it.
    startPump(PumpStart{requestUUID, ctx, writer, nullptr,
                        CloudErrorField::kCommandError, label,
                        [handler, paramBytes, ctx, writer, requestUUID, label, log] {
                            handler(paramBytes, *ctx, *writer, requestUUID);
                            // After the handler, matching dispatchTo: the event
                            // means the call completed, not that it was received.
                            logEvent(log, LogLevel::kInfo, "dispatch", label);
                        }},
              calls);
    // startPump already wrote the rejection envelope on failure and there is
    // nothing to unwind here, so its return value is deliberately unused.
}

std::shared_ptr<CallContext> CloudEnvelopeRouter::makeCloudCallContext(
    const google::protobuf::RepeatedPtrField<cloud::Metadata>& metadata) {
    auto ctx = std::make_shared<CallContext>();
    for (const auto& md : metadata) {
        ctx->setMetadata(md.fullyqualifiedmetadataid(), md.value());
        if (md.fullyqualifiedmetadataid() == kAccessTokenMetadataFqi) {
            // md.value() is the serialized Metadata_AccessToken message, not the
            // bare token — same wire format the gRPC path unwraps in
            // MetadataExtractingInterceptor.cc. A value that fails to parse is
            // dropped rather than passed through as the raw token: falling back
            // would let arbitrary bytes reach AuthTokenStore::validate() under
            // the trusted "access-token" key (same policy as the gRPC path).
            org::silastandard::core::authorizationservice::v1::Metadata_AccessToken accessToken;
            if (accessToken.ParseFromString(md.value())) {
                ctx->setMetadata(kAccessTokenRawKey, accessToken.accesstoken().value());
            }
        }
    }
    return ctx;
}

std::shared_ptr<CallContext> CloudEnvelopeRouter::makeFollowupContext(
    const std::string& fqi, const std::optional<std::string>& accessToken,
    const std::string& requestUUID, StreamWriteSerializer& writer) {
    // No metadata admission here, by design: Part A requires SiLA Client
    // Metadata only with the Command initiation, never with _Info /
    // _Intermediate / _Result. The gRPC twin skips the same three RPCs
    // (GrpcTransport.h's CommandExecutionUUID check). Distinct from the S15
    // token replay this function does perform.
    auto ctx = std::make_shared<CallContext>();
    if (accessToken) {
        ctx->setMetadata(kAccessTokenRawKey, *accessToken);
    }
    if (!chain_ || !chain_->auth) {
        return ctx;
    }
    // Gated on the base command FQI, never on fqi + "_Result"/"_Info"/etc.:
    // FqiMatch.h's anyFqiCovers only matches on a '/' boundary, so a
    // protectedFqis entry naming the exact command would cover "<cmd>" but
    // not "<cmd>_Result" (underscore, not slash) and the follow-ups would
    // silently stay open for that operator's list (§S15).
    try {
        chain_->auth->intercept(*ctx, fqi);
    } catch (const error::SiLAError& e) {
        // Same event dispatchTo's auth gate emits below, so a follow-up
        // denial is visible in the same log stream as an initiation denial.
        logEvent(chain_->logCallback, LogLevel::kWarning, "auth", "access denied: " + fqi);
        cloud::SiLAServerMessage errMsg;
        errMsg.set_requestuuid(requestUUID);
        *errMsg.mutable_commanderror() = *e.toProto();
        writer.write(errMsg);
        return nullptr;
    }
    return ctx;
}

bool CloudEnvelopeRouter::admitMetadata(
    const std::string& fqi,
    const google::protobuf::RepeatedPtrField<cloud::Metadata>& metadata,
    CloudErrorField errorField, const std::string& requestUUID,
    StreamWriteSerializer& writer) {
    try {
        // Cloud metadata is keyed by the FQI verbatim on the wire
        // (makeCloudCallContext), so presence is a scan, not a header-key
        // derivation -- the gRPC seam's forward derivation has no counterpart
        // here.
        enforceMetadataPolicy(
            chain_, fqi, !metadata.empty(),
            [&metadata](const std::string& metadataFqi) {
                return std::any_of(metadata.begin(), metadata.end(),
                                   [&metadataFqi](const cloud::Metadata& entry) {
                                       return entry.fullyqualifiedmetadataid() == metadataFqi;
                                   });
            });

        // Same gate the gRPC seam runs, in the same position relative to the
        // presence check. One edit covers both cloud entry points: dispatchTo
        // and the observable-property-subscription branch both come through
        // here. Follow-ups do not, and must not -- makeFollowupContext:504-508.
        if (chain_ && chain_->lockGate) {
            std::optional<std::string> lockValue;
            for (const auto& entry : metadata) {
                if (entry.fullyqualifiedmetadataid() == kLockIdentifierMetadataFqi) {
                    lockValue = entry.value();
                    break;
                }
            }
            chain_->lockGate(fqi, lockValue);
        }
    } catch (const error::SiLAError& e) {
        // Tag "metadata", not "auth": a metadata refusal and an access denial
        // are different verdicts and an operator reading the log must not have
        // to guess which gate fired.
        logEvent(chain_ ? chain_->logCallback : LogCallback{}, LogLevel::kWarning,
                 "metadata", "rejected: " + fqi);
        sendSiLAErrorEnvelope(writer, requestUUID, errorField, e);
        return false;
    }
    return true;
}

void CloudEnvelopeRouter::dispatchTo(
    const CloudDispatchMap& handlers,
    const std::string& fqi,
    const std::string& parameterBytes,
    const google::protobuf::RepeatedPtrField<cloud::Metadata>& metadata,
    CloudErrorField errorField,
    const std::string& requestUUID,
    StreamWriteSerializer& writer,
    ActiveCallRegistry& calls) {

    CloudDispatchFn handler;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = handlers.find(fqi);
        if (it != handlers.end()) {
            handler = it->second;
        }
    }
    if (!handler) {
        sendNoHandlerError(writer, requestUUID, fqi, errorField);
        return;
    }

    auto ctx = makeCloudCallContext(metadata);

    if (!admitMetadata(fqi, metadata, errorField, requestUUID, writer)) {
        return;
    }

    // Login is exempt by construction on both transports -- it issues the
    // token the gate checks for. Mirrors AuthenticationServiceImpl::Login
    // (AuthenticationServiceImpl.h:42-44), which builds its own auth-free
    // interceptor chain for the direct-gRPC path; the cloud path shares
    // dispatchTo for every command, so the exemption has to live here too.
    const bool isLoginCommand =
        fqi == std::string{kAuthenticationServiceFqi} + "/Command/Login";

    // Auth gate — same check as the gRPC path (§3.1 chain order)
    if (chain_ && chain_->auth && !isLoginCommand) {
        try {
            chain_->auth->intercept(*ctx, fqi);
        } catch (const error::SiLAError& e) {
            // Same event the gRPC auth gate emits (GrpcTransport.h:161), so an
            // access denial is visible whichever transport carried the call (§3.4f).
            logEvent(chain_->logCallback, LogLevel::kWarning, "auth",
                     "access denied: " + fqi);
            cloud::SiLAServerMessage errMsg;
            errMsg.set_requestuuid(requestUUID);
            auto silaError = e.toProto();
            switch (errorField) {
                case CloudErrorField::kCommandError:
                    *errMsg.mutable_commanderror() = *silaError;
                    break;
                case CloudErrorField::kPropertyError:
                    *errMsg.mutable_propertyerror() = *silaError;
                    break;
            }
            writer.write(errMsg);
            return;
        }
    }

    // 2.2o: add() silently overwrites and CallRegistryGuard below removes
    // unconditionally, so a unary call reusing a LIVE pump's requestUUID would
    // erase that pump's registry entry and put it out of Cancel*'s reach --
    // it would stream until teardown with no way to stop it. Placed here rather
    // than at the head of dispatchTo: a call that returns early (no handler,
    // access denied) never reaches add() and clobbers nothing, so answering it
    // with a duplicate error would replace an accurate diagnostic with a
    // misleading one. Sequential reuse after completion stays legal -- the guard
    // reaps, and find() returns null once the previous call is gone.
    if (rejectDuplicateRequestUUID(calls, requestUUID, errorField, writer)) {
        return;
    }

    calls.add(requestUUID, ctx);
    {
        CallRegistryGuard guard{calls, requestUUID};
        handler(parameterBytes, *ctx, writer, requestUUID);
    }

    // After the handler, matching dispatchToHandler's placement: the event
    // means the call completed, not that it was received. A throwing handler
    // therefore emits none, same as on the gRPC path.
    if (chain_) {
        logEvent(chain_->logCallback, LogLevel::kInfo, "dispatch", fqi);
    }
}

bool CloudEnvelopeRouter::guardBinaryOp(StreamWriteSerializer& writer,
                                        const std::string& requestUUID,
                                        const std::string& uuid,
                                        cloud::BinaryTransferError::ErrorType failType) {
    if (!binaryStore_) {
        sendBinaryTransferError(writer, requestUUID, failType,
            "binary transfer not configured");
        return false;
    }
    if (!binaryStore_->contains(uuid)) {
        sendBinaryTransferError(writer, requestUUID,
            cloud::BinaryTransferError::INVALID_BINARY_TRANSFER_UUID,
            "unknown binaryTransferUUID: " + uuid);
        return false;
    }
    return true;
}

void CloudEnvelopeRouter::route(
    const cloud::SiLAClientMessage& msg,
    StreamWriteSerializer& writer,
    std::shared_ptr<StreamWriteSerializer> writerOwnership,
    ActiveCallRegistry& calls) {
#ifdef SILA_ENABLE_OTEL
    auto tracer = opentelemetry::trace::Provider::GetTracerProvider()
                      ->GetTracer("sila2");
    auto span = tracer->StartSpan("cloud.route");
    span->SetAttribute("sila.cloud.message_case", static_cast<int32_t>(msg.message_case()));
    auto scope = opentelemetry::trace::Scope{span};
#endif
    switch (msg.message_case()) {
        case cloud::SiLAClientMessage::kUnobservableCommandExecution: {
            const auto& exec = msg.unobservablecommandexecution();
#ifdef SILA_ENABLE_OTEL
            span->SetAttribute("sila.fqi", exec.fullyqualifiedcommandid());
#endif
            dispatchTo(commandHandlers_, exec.fullyqualifiedcommandid(),
                       exec.commandparameter().parameters(),
                       exec.commandparameter().metadata(),
                       CloudErrorField::kCommandError, msg.requestuuid(), writer, calls);
            break;
        }
        case cloud::SiLAClientMessage::kObservableCommandInitiation: {
            const auto& init = msg.observablecommandinitiation();
#ifdef SILA_ENABLE_OTEL
            span->SetAttribute("sila.fqi", init.fullyqualifiedcommandid());
#endif
            dispatchTo(commandHandlers_, init.fullyqualifiedcommandid(),
                       init.commandparameter().parameters(),
                       init.commandparameter().metadata(),
                       CloudErrorField::kCommandError, msg.requestuuid(), writer, calls);
            break;
        }
        case cloud::SiLAClientMessage::kUnobservablePropertyRead: {
            const auto& read = msg.unobservablepropertyread();
#ifdef SILA_ENABLE_OTEL
            span->SetAttribute("sila.fqi", read.fullyqualifiedpropertyid());
#endif
            dispatchTo(propertyHandlers_, read.fullyqualifiedpropertyid(),
                       "", read.metadata(),
                       CloudErrorField::kPropertyError, msg.requestuuid(), writer, calls);
            break;
        }
        case cloud::SiLAClientMessage::kObservablePropertySubscription: {
            const auto& sub = msg.observablepropertysubscription();
            const std::string& fqi = sub.fullyqualifiedpropertyid();
#ifdef SILA_ENABLE_OTEL
            span->SetAttribute("sila.fqi", fqi);
#endif
            const std::string& reqUuid = msg.requestuuid();

            CloudObservablePropEntry entry;
            bool found = false;
            CloudDispatchFn streamHandler;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = observableProps_.find(fqi);
                if (it != observableProps_.end()) {
                    entry = it->second;
                    found = true;
                } else {
                    // 1.2k: codegen'd adapters register their generated Subscribe_
                    // handler here. Manager entries keep priority so the hand-wired
                    // RecoverableErrors path is bit-for-bit unchanged.
                    auto hIt = observablePropHandlers_.find(fqi);
                    if (hIt != observablePropHandlers_.end()) {
                        streamHandler = hIt->second;
                    }
                }
            }

            if (!found && !streamHandler) {
                // Fallback: one-shot read via property handler
                dispatchTo(propertyHandlers_, fqi, "", sub.metadata(),
                           CloudErrorField::kPropertyError, reqUuid, writer, calls);
                break;
            }

            auto ctx = makeCloudCallContext(sub.metadata());

            // Same gate dispatchTo runs. Not reachable through it: this branch
            // builds its own context and runs its own auth gate, exactly as it
            // used to skip the auth gate entirely before 3.1s.
            if (!admitMetadata(fqi, sub.metadata(), CloudErrorField::kPropertyError,
                               reqUuid, writer)) {
                break;
            }

            // Auth gate — same check as dispatchTo (§3.1 chain order). This
            // branch used to skip it entirely: it does not funnel through
            // dispatchTo like every other cloud call, so a subscription here
            // could open with no auth / access-denied event ever possible,
            // unlike the same property subscribed over direct gRPC
            // (dispatchToHandler, GrpcTransport.h:162).
            if (chain_ && chain_->auth) {
                try {
                    chain_->auth->intercept(*ctx, fqi);
                } catch (const error::SiLAError& e) {
                    logEvent(chain_->logCallback, LogLevel::kWarning, "auth",
                             "access denied: " + fqi);
                    cloud::SiLAServerMessage errMsg;
                    errMsg.set_requestuuid(reqUuid);
                    *errMsg.mutable_propertyerror() = *e.toProto();
                    writer.write(errMsg);
                    break;
                }
            }

            auto log = chain_ ? chain_->logCallback : LogCallback{};

            if (streamHandler) {
                startPump(PumpStart{reqUuid, ctx, writerOwnership, nullptr,
                                    CloudErrorField::kPropertyError, fqi,
                                    [streamHandler, ctx, writerPtr = writerOwnership,
                                     reqUuid, fqi, log] {
                                        // Empty parameterBytes:
                                        // ObservablePropertySubscription carries none,
                                        // matching Subscribe_X's empty _Parameters
                                        // message on the gRPC path.
                                        streamHandler("", *ctx, *writerPtr, reqUuid);
                                        logEvent(log, LogLevel::kInfo, "dispatch", fqi);
                                    }},
                          calls);
                // No return value to act on: unlike the manager path below, nothing
                // has been subscribed yet, so a rejection (which startPump already
                // wrote) leaves no state to unwind.
                break;
            }

            auto subscription = entry.mgr->subscribe(entry.propertyId);
            auto serializer = entry.serializer;

            ctx->onCancellation([mgr = entry.mgr, propId = entry.propertyId,
                                 s = subscription] {
                mgr->unsubscribe(propId, s);
                s->cancel();
            });
            // ctx is captured by the pump's body so the registry's weak_ptr
            // stays valid for as long as the subscription runs — without it
            // the context dies at the end of this block and
            // CancelObservablePropertySubscription finds nothing (§3.2f).
            // Joined, not detached (§4.2h): a detached thread let an
            // exception escape into std::terminate, and nothing drained the
            // threads at shutdown.
            const bool started = startPump(
                PumpStart{reqUuid, ctx, writerOwnership, subscription,
                          CloudErrorField::kPropertyError, fqi,
                          [subscription, serializer, writerPtr = writerOwnership, reqUuid] {
                              while (auto value = subscription->waitForNext()) {
                                  cloud::SiLAServerMessage resp;
                                  resp.set_requestuuid(reqUuid);
                                  resp.mutable_observablepropertyvalue()->set_value(
                                      serializer(*value));
                                  if (!writerPtr->write(resp)) {
                                      break;
                                  }
                              }
                          }},
                calls);
            if (!started) {
                // subscribe() already ran, so a rejection must undo it or the
                // manager keeps a subscriber nothing will ever drain (§2.1h's
                // sibling leak). requestCancellation drives the onCancellation
                // callback installed above — the same unsubscribe + cancel path
                // a normal pump exit takes — instead of a second hand-written
                // teardown that can drift from it.
                ctx->requestCancellation();
                break;
            }
            // Same event dispatchToHandler emits (GrpcTransport.h:183). Moved
            // below the admission check: a rejected subscription is not a
            // dispatch. Concrete throw the pump's catch guards: the registered
            // CloudValueSerializer does an std::any_cast, which throws
            // std::bad_any_cast when a publisher enqueues the wrong type.
            logEvent(log, LogLevel::kInfo, "dispatch", fqi);
            break;
        }
        // Cancel stays inline on the receive loop, deliberately: it is only a
        // map lookup plus a flag store, and it only works because §1.2g moved
        // every blocking body off this thread onto a pump.
        case cloud::SiLAClientMessage::kCancelObservableCommandExecutionInfoSubscription:
        case cloud::SiLAClientMessage::kCancelObservableCommandIntermediateResponseSubscription:
        case cloud::SiLAClientMessage::kCancelObservablePropertySubscription:
            calls.cancel(msg.requestuuid());
            break;
        case cloud::SiLAClientMessage::kObservableCommandExecutionInfoSubscription: {
            if (!hasObservableCommands()) {
                sendFrameworkError(writer, msg.requestuuid(), CloudErrorField::kCommandError,
                    "observable commands not configured");
                break;
            }
            const auto& sub = msg.observablecommandexecutioninfosubscription();
            const std::string& uuid = sub.commandexecutionuuid().value();
            const std::string reqUuid = msg.requestuuid();
            // Prefer the Feature's own _Info handler -- the same one the direct
            // gRPC path runs (generated adapters route <Cmd>_Info through
            // dispatchToHandler). The synthesized pump below stays as the
            // fallback for hand-wired features that register no _Info handler
            // and for executions with no registered FQI (a command initiated
            // over gRPC, or created directly on the manager). A codegen'd
            // adapter always registers the "_Info" key, so for it the fallback
            // is unreachable and an unset onXInfo surfaces as an
            // UndefinedExecutionError on both transports rather than silently
            // falling back -- the same contract as the _Result slot.
            bool hasInfoHandler = false;
            // Copied out of the map rather than read via a dangling iterator:
            // the lock releases at the end of this block, and the fallback
            // pump below (§S15) needs the entry's fqi/accessToken outside it.
            std::optional<ExecutionEntry> entry;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto fqiIt = executionFqis_.find(uuid);
                if (fqiIt != executionFqis_.end()) {
                    entry = fqiIt->second;
                    hasInfoHandler = commandHandlers_.count(entry->fqi + "_Info") > 0;
                }
            }
            if (hasInfoHandler) {
                dispatchObservableByUuid(uuid, "_Info", reqUuid, writerOwnership, calls);
                break;
            }
            // No metadata field on ObservableCommandExecutionInfoSubscription
            // (SiLACloudConnector.proto:79-81), so there is nothing for
            // makeCloudCallContext to read -- the follow-up gate instead
            // replays the token snapshotted at ObservableCommandInitiation
            // (§S15, owner option 1).
            std::shared_ptr<CallContext> ctx;
            if (entry) {
                // A registered FQI can be matched against protectedFqis, so
                // gate this synthesized pump exactly like
                // dispatchObservableByUuid's _Intermediate/_Result path does.
                ctx = makeFollowupContext(entry->fqi, entry->accessToken, reqUuid, writer);
                if (!ctx) {
                    break;
                }
            } else {
                // No registered FQI: a command initiated over gRPC, created
                // directly on the manager, or otherwise unnamed to this
                // router. An unnamed execution cannot be matched against
                // protectedFqis at all, so gating it here would be a coin
                // flip -- this is the hand-wired / cross-transport-initiated
                // case the fallback pump exists for.
                ctx = std::make_shared<CallContext>();
            }
            // S28: looked up only after the gate above -- findExecution's
            // InvalidCommandExecutionUuid reply must not tell an unauthorized
            // caller whether the UUID names a live execution.
            std::shared_ptr<ObservableCommandExecution> exec;
            try {
                // The shared_ptr, not a raw pointer: a concurrent
                // ObservableCommandManager::removeExpired() sweep may erase the
                // map entry while the pump still holds this (§4.2d).
                exec = findExecution(uuid);
            } catch (const error::SiLAError& e) {
                cloud::SiLAServerMessage errMsg;
                errMsg.set_requestuuid(reqUuid);
                *errMsg.mutable_commanderror() = *e.toProto();
                writer.write(errMsg);
                break;
            }
            startPump(PumpStart{reqUuid, ctx, writerOwnership, nullptr,
                                CloudErrorField::kCommandError,
                                "execution info: " + uuid,
                                [exec, ctx, writerPtr = writerOwnership, reqUuid, uuid] {
                                    // Cancellation ends this subscription only. The
                                    // command execution continues independently.
                                    std::optional<InfoSnapshot> lastSent;
                                    while (true) {
                                        InfoSnapshot now{exec->state(), exec->progress(),
                                                         exec->estimatedRemaining()};
                                        const bool terminal =
                                            now.state == ObservableCommandExecution::State::FinishedSuccessfully ||
                                            now.state == ObservableCommandExecution::State::FinishedWithError;
                                        // First snapshot always goes out, then
                                        // only transitions (architecture-v2.md:288,293).
                                        if (!lastSent || *lastSent != now) {
                                            if (!writerPtr->write(buildExecutionInfo(reqUuid, uuid, now, exec->lifetime()))) {
                                                // write() returning false means the shared
                                                // Connection stream itself is broken -- the
                                                // same "Connection lost" case cancelAll()
                                                // handles, not a deliberate per-call cancel,
                                                // so this must not interrupt the execution
                                                // (§3.3 ②). Just stop sending; the manager
                                                // keeps the execution and its UUID alive for
                                                // a reconnect to resume watching.
                                                break;
                                            }
                                            lastSent = now;
                                        }
                                        // Terminal is written before exiting, so
                                        // a client always observes the last state
                                        // (architecture-v2.md:666-679). Cancellation
                                        // is re-checked after the sleep so a cancelled
                                        // subscription never emits another snapshot.
                                        if (terminal) {
                                            break;
                                        }
                                        std::this_thread::sleep_for(kExecutionInfoPollInterval);
                                        if (ctx->isCancelled()) {
                                            break;
                                        }
                                    }
                                }},
                      calls);
            break;
        }
        case cloud::SiLAClientMessage::kObservableCommandIntermediateResponseSubscription: {
            if (!hasObservableCommands()) {
                sendFrameworkError(writer, msg.requestuuid(), CloudErrorField::kCommandError,
                    "observable commands not configured");
                break;
            }
            dispatchObservableByUuid(
                msg.observablecommandintermediateresponsesubscription().commandexecutionuuid().value(),
                "_Intermediate", msg.requestuuid(), writerOwnership, calls);
            break;
        }
        case cloud::SiLAClientMessage::kObservableCommandGetResponse: {
            if (!hasObservableCommands()) {
                sendFrameworkError(writer, msg.requestuuid(), CloudErrorField::kCommandError,
                    "observable commands not configured");
                break;
            }
            dispatchObservableByUuid(
                msg.observablecommandgetresponse().commandexecutionuuid().value(),
                "_Result", msg.requestuuid(), writerOwnership, calls);
            break;
        }
        case cloud::SiLAClientMessage::kCreateBinaryUploadRequest: {
            if (!binaryStore_) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED,
                    "binary transfer not configured");
                break;
            }
            const auto& uploadReq = msg.createbinaryuploadrequest();
            // Metadata admission gate for the binary seam. The gRPC twin is
            // gated as a side effect of dispatching CreateBinary under the
            // caller's parameterIdentifier (BinaryUploadService.cc:38), so a
            // declared metadata whose affected list covers that parameter FQI
            // is enforced there; without this call the two transports disagree
            // the moment anything is declared at feature granularity. Not
            // admitMetadata(): this branch's oneof cannot carry a SiLAError
            // envelope, so the refusal is degraded to a BinaryTransferError,
            // like the auth gate below.
            {
                const auto& mdList = uploadReq.metadata();
                try {
                    enforceMetadataPolicy(
                        chain_, uploadReq.createbinaryrequest().parameteridentifier(),
                        !mdList.empty(),
                        [&mdList](const std::string& metadataFqi) {
                            return std::any_of(mdList.begin(), mdList.end(),
                                               [&metadataFqi](const cloud::Metadata& entry) {
                                                   return entry.fullyqualifiedmetadataid() == metadataFqi;
                                               });
                        });
                } catch (const error::SiLAError& e) {
                    logEvent(chain_ ? chain_->logCallback : LogCallback{},
                             LogLevel::kWarning, "metadata",
                             "rejected: " + uploadReq.createbinaryrequest().parameteridentifier());
                    sendBinaryTransferError(writer, msg.requestuuid(),
                        cloud::BinaryTransferError::BINARY_UPLOAD_FAILED, e.what());
                    break;
                }
            }
            if (chain_ && chain_->auth) {
                auto ctx = makeCloudCallContext(uploadReq.metadata());
                try {
                    chain_->auth->intercept(*ctx, uploadReq.createbinaryrequest().parameteridentifier());
                } catch (const error::SiLAError& e) {
                    // Second auth gate in this file — logged identically so the
                    // binary-upload path is not the one hole in the audit trail.
                    logEvent(chain_->logCallback, LogLevel::kWarning, "auth",
                             "access denied: "
                             + uploadReq.createbinaryrequest().parameteridentifier());
                    sendBinaryTransferError(writer, msg.requestuuid(),
                        cloud::BinaryTransferError::BINARY_UPLOAD_FAILED,
                        e.what());
                    break;
                }
            }
            const auto& req = uploadReq.createbinaryrequest();
            if (chain_ && !chain_->registeredFeatureFqis.empty() &&
                !auth::isKnownParameterFqi(chain_->registeredFeatureFqis,
                                           req.parameteridentifier())) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED,
                    "parameterIdentifier does not name a parameter of a registered Feature: "
                        + req.parameteridentifier());
                break;
            }
            auto lifetime = chain_ ? chain_->binarySlotLifetime : kDefaultBinaryLifetime;
            std::string uuid;
            try {
                uuid = binaryStore_->createSlot(
                    req.binarysize(), req.chunkcount(), lifetime);
            } catch (const std::exception& e) {
                // Mirror kUploadChunkRequest below: a store-side rejection (e.g. an
                // oversized chunkCount, InMemoryBinaryStore.cc's chunkCount guard)
                // must become an envelope, not escape route() to be swallowed by
                // CloudTransport::receiveLoop's catch(...), which would leave the
                // client's requestUUID with no response at all.
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED, e.what());
                break;
            }

            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(msg.requestuuid());
            auto* body = resp.mutable_createbinaryresponse();
            body->set_binarytransferuuid(uuid);
            body->mutable_lifetimeofbinary()->set_seconds(lifetime.count());
            writer.write(resp);
            break;
        }
        case cloud::SiLAClientMessage::kUploadChunkRequest: {
            // S14 option B gates the gRPC transport's UploadChunk/DeleteBinary
            // on kBinaryUploadFqi (BinaryUploadService.cc), but this cloud
            // envelope cannot mirror that gate: only createBinaryUploadRequest
            // carries a metadata field (SiLACloudConnector.proto:133-137);
            // uploadChunkRequest and the other four binary envelopes below
            // reuse the bare SiLABinaryTransfer messages (:50-55), which have
            // none. CreateBinary is the only gate the cloud transport can
            // hold, so the binaryTransferUUID is the capability for
            // everything after it on this transport. Do not invent a
            // metadata channel here.
            const auto& req = msg.uploadchunkrequest();
            const std::string& uuid = req.binarytransferuuid();
            if (!guardBinaryOp(writer, msg.requestuuid(), uuid,
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED)) break;
            const std::string& payload = req.payload();
            // Part B p56: a Binary Chunk MUST not exceed 2 MiB. Mirror the
            // gRPC transport's gate (BinaryUploadService::uploadChunk).
            if (payload.size() > binary::kMaxBinaryChunkSize) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED,
                    "Binary Chunk exceeds the 2 MiB ceiling: " + std::to_string(payload.size()));
                break;
            }
            try {
                binaryStore_->storeChunk(uuid, req.chunkindex(),
                    std::vector<uint8_t>{payload.begin(), payload.end()});
            } catch (const std::exception& e) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED, e.what());
                break;
            }

            auto lifetime = chain_ ? chain_->binarySlotLifetime : kDefaultBinaryLifetime;
            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(msg.requestuuid());
            auto* body = resp.mutable_uploadchunkresponse();
            body->set_binarytransferuuid(uuid);
            body->set_chunkindex(req.chunkindex());
            body->mutable_lifetimeofbinary()->set_seconds(lifetime.count());
            writer.write(resp);
            break;
        }
        case cloud::SiLAClientMessage::kDeleteUploadedBinaryRequest: {
            const std::string& uuid = msg.deleteuploadedbinaryrequest().binarytransferuuid();
            if (!guardBinaryOp(writer, msg.requestuuid(), uuid,
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED)) break;
            try {
                binaryStore_->remove(uuid);
            } catch (const std::exception& e) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_UPLOAD_FAILED, e.what());
                break;
            }

            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(msg.requestuuid());
            resp.mutable_deletebinaryresponse();
            writer.write(resp);
            break;
        }
        case cloud::SiLAClientMessage::kGetBinaryInfoRequest: {
            const std::string& uuid = msg.getbinaryinforequest().binarytransferuuid();
            if (!guardBinaryOp(writer, msg.requestuuid(), uuid,
                    cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED)) break;

            try {
                cloud::SiLAServerMessage resp;
                resp.set_requestuuid(msg.requestuuid());
                auto* body = resp.mutable_getbinaryresponse();
                body->set_binarysize(binaryStore_->binarySize(uuid));
                body->mutable_lifetimeofbinary()->set_seconds(
                    binaryStore_->remainingLifetime(uuid).count());
                writer.write(resp);
            } catch (const std::exception& e) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED, e.what());
            }
            break;
        }
        case cloud::SiLAClientMessage::kGetChunkRequest: {
            const auto& req = msg.getchunkrequest();
            const std::string& uuid = req.binarytransferuuid();
            if (!guardBinaryOp(writer, msg.requestuuid(), uuid,
                    cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED)) break;

            // Part B p56: a requested chunk length above 2 MiB would return a
            // non-conformant chunk. Mirror the gRPC transport's gate
            // (BinaryDownloadService::getChunk). Kept before the try so that
            // break exits the switch case, matching kUploadChunkRequest above.
            if (req.length() > binary::kMaxBinaryChunkSize) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED,
                    "Requested chunk length exceeds the 2 MiB ceiling: " + std::to_string(req.length()));
                break;
            }

            try {
                std::size_t offset = req.offset();
                std::size_t length = req.length();
                auto payload = binaryStore_->readRange(uuid, offset, length);

                auto lifetime = chain_ ? chain_->binarySlotLifetime : kDefaultBinaryLifetime;
                binaryStore_->updateLifetime(uuid, lifetime);

                cloud::SiLAServerMessage resp;
                resp.set_requestuuid(msg.requestuuid());
                auto* body = resp.mutable_getchunkresponse();
                body->set_binarytransferuuid(uuid);
                body->set_offset(offset);
                body->set_payload(payload.data(), payload.size());
                body->mutable_lifetimeofbinary()->set_seconds(
                    binaryStore_->remainingLifetime(uuid).count());
                writer.write(resp);
            } catch (const std::exception& e) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED, e.what());
            }
            break;
        }
        case cloud::SiLAClientMessage::kDeleteDownloadedBinaryRequest: {
            const std::string& uuid = msg.deletedownloadedbinaryrequest().binarytransferuuid();
            if (!guardBinaryOp(writer, msg.requestuuid(), uuid,
                    cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED)) break;
            try {
                binaryStore_->remove(uuid);
            } catch (const std::exception& e) {
                sendBinaryTransferError(writer, msg.requestuuid(),
                    cloud::BinaryTransferError::BINARY_DOWNLOAD_FAILED, e.what());
                break;
            }

            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(msg.requestuuid());
            resp.mutable_deletebinaryresponse();
            writer.write(resp);
            break;
        }
        case cloud::SiLAClientMessage::kMetadataRequest: {
            const std::string& metadataFQI =
                msg.metadatarequest().fullyqualifiedmetadataid();

            cloud::SiLAServerMessage resp;
            resp.set_requestuuid(msg.requestuuid());
            auto* body = resp.mutable_getfcpaffectedbymetadataresponse();

            // The gate's table, verbatim: Part A requires the client to send
            // exactly what this list names, so discovery answering from a
            // second source would let the server demand something it never
            // advertised. No lock: the chain is immutable after Build().
            if (chain_) {
                auto it = chain_->metadataAffectedCalls.find(metadataFQI);
                if (it != chain_->metadataAffectedCalls.end()) {
                    for (const auto& fcp : it->second) {
                        body->add_affectedcalls(fcp);
                    }
                }
            }

            writer.write(resp);
            break;
        }
        case cloud::SiLAClientMessage::MESSAGE_NOT_SET:
            break;
    }
}

}  // namespace sila2
