// InterceptorChain.h — Interceptor bundle for request dispatch (architecture.md §3.1)
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sila/common/util/AsciiCase.h>
#include <sila/server/LogCallback.h>

namespace sila2 {

class BinaryStore;
namespace auth { class AuthorizationInterceptor; }

/// Groups the optional interceptors that dispatchToHandler applies around
/// each handler invocation. Assembled by SilaServerBase::Builder::build()
/// and passed by raw pointer to generated service adapters.
struct InterceptorChain {
    auth::AuthorizationInterceptor* auth = nullptr;  ///< null if withAuthentication not called
    BinaryStore* binaryStore = nullptr;              ///< null if withBinaryTransfer not called
    std::chrono::seconds binarySlotLifetime{300};    ///< lifetime for injectBinaryResults slots
    LogCallback logCallback;                         ///< null (empty) if no callback installed

    // Validates the LockIdentifier metadata of one call and renews the lock's
    // inactivity timeout on a match (LockControllerImpl::checkLockMetadata).
    // Empty when the server was built without Builder::withLock().
    //
    // A std::function rather than a LockControllerImpl*: this header is reached
    // from GrpcTransport.h, i.e. from every generated service adapter in the
    // build, and a pointer would need the complete type and drag
    // LockController.grpc.pb.h in behind it -- the cost MetadataPolicy.h:24-27
    // refuses for SiLAService. logCallback above is the same escape hatch.
    //
    // Throws (error::FrameworkError{InvalidMetadata} or
    // error::DefinedExecutionError{InvalidLockIdentifier}); both call sites are
    // already inside a SilaError boundary.
    std::function<void(std::string_view targetFqi,
                       const std::optional<std::string>& serializedLockIdentifier)> lockGate;  ///< Checks/renews the LockIdentifier metadata; empty if withLock() was not called.

    // FQIs of every Feature registered on this server, snapshotted at build().
    // CreateBinary gates auth on the caller's parameterIdentifier
    // (BinaryUploadService.cc:29, CloudEnvelopeRouter.cc:821), so an identifier
    // no registered Feature accounts for is a free pass past that gate.
    // Empty means the chain was assembled outside SilaServerBase::Builder
    // (unit tests wiring only an auth interceptor); validation is then skipped.
    std::vector<std::string> registeredFeatureFqis;  ///< Every Feature FQI registered on this server, snapshotted at build().

    // Every SiLA Client Metadata this server declares: fully qualified
    // Metadata identifier -> the Features / Commands / Properties it affects
    // (Part A's affected list). Snapshotted at build() and never mutated
    // afterwards, which is why neither reader locks -- Part A makes the
    // immutability a MUST ("MUST NOT change during the Lifetime of a SiLA
    // Server"), so a mutex here would guard a value that cannot change.
    // ONE table, TWO readers that the standard requires to agree: the
    // admission gate (MetadataPolicy.h) and cloud FCP discovery
    // (CloudEnvelopeRouter's kMetadataRequest branch). A client sends exactly
    // what discovery names, so a second table would be a way for the server to
    // demand something it never advertised.
    // Empty means nothing is declared and the gate's rule (c) is a no-op.
    std::map<std::string, std::vector<std::string>> metadataAffectedCalls;  ///< Declared SiLA Client Metadata FQI to the calls it affects.

    // Observable Command follow-up ownership (Batch C, High-2).
    //
    // A follow-up RPC (subscribeCommandExecutionInfo, subscribeCommandIntermediateResponses,
    // getCommandExecutionInfo, ...) carries only a CommandExecutionUUID, not the command's
    // FQI, so the auth interceptor cannot re-derive which Command the UUID belongs to
    // without this table -- and without it, a token authorized for Command A could be
    // replayed with Command B's UUID to observe B's execution. registerObservableOwner is
    // called once when the observable command starts; every follow-up RPC then looks the
    // owner FQI up here and checks it against the caller's token instead of trusting the
    // UUID alone.
    //
    // Held behind a shared_ptr, not as value members: Login builds a per-call chain
    // COPY with auth disabled (AuthenticationServiceImpl.cc:80), so InterceptorChain must
    // stay copyable -- a std::mutex value member would delete the copy ctor. A copied chain
    // then SHARES this one server-scoped table (the pointee), which is exactly right: the
    // owner records belong to the server, not to a transient per-call chain variant. The
    // shared_ptr is default-constructed so every chain (including `InterceptorChain{}` in
    // unit tests) starts with its own empty registry, and copies share it.
    //
    // const methods mutating through the pointer: the chain is reached only through
    // `const InterceptorChain*` at every call site, and shared_ptr's operator-> yields
    // non-const access to the pointee even from a const shared_ptr, so no `mutable` is
    // needed. The mutex guards concurrent gRPC dispatch threads; unordered_map is fine
    // because every access is an exact-UUID key lookup with no ordering requirement.
    //
    // Stores the owner FQI AND the access token snapshotted at initiation, exactly like
    // CloudEnvelopeRouter's executionFqis_. SiLA Part A requires an Observable Command's
    // SiLA Client Metadata -- the access token included -- "only with the Command initiation
    // ... not with" the Info / Intermediate / Result follow-ups, so a conformant client sends
    // NO token on a follow-up RPC. The follow-up auth gate therefore replays this snapshot
    // instead of demanding a fresh wire token (GrpcTransport.h), matching the cloud transport
    // whose follow-up envelopes have no metadata field at all. Trade-off inherited from the
    // cloud twin: a token that expires mid-execution cannot be refreshed over the wire
    // (CloudEnvelopeRouter.h:238-248).
    /// One Observable Command execution's authorization owner: the Command that
    /// initiated it, and the access token to replay on its follow-up RPCs.
    struct ObservableOwnerEntry {
        std::string fqi;                    ///< owning Command's FQI
        std::optional<std::string> token;   ///< access token snapshotted at initiation; nullopt if the initiation was unprotected
    };
    /// Thread-safe table of ObservableOwnerEntry, keyed by lower-cased Command Execution UUID.
    struct ObservableOwnerRegistry {
        std::mutex mu;  ///< guards owners
        std::unordered_map<std::string, ObservableOwnerEntry> owners;  ///< entries, keyed by lower-cased UUID
    };
    std::shared_ptr<ObservableOwnerRegistry> observableOwners_ =
        std::make_shared<ObservableOwnerRegistry>();  ///< Authorization-owner table shared by every copy of this chain.

    /// Records `ownerFqi` as the owner of the Observable Command execution identified by
    /// `uuid`. Called once, when the observable command is created. Overwrites silently if
    /// the UUID is somehow reused, since UUID collision is outside this table's remit.
    void registerObservableOwner(const std::string& uuid, std::string ownerFqi,
                                 std::optional<std::string> token) const {
        std::lock_guard<std::mutex> lock{observableOwners_->mu};
        observableOwners_->owners[uuid] = ObservableOwnerEntry{std::move(ownerFqi), std::move(token)};
    }

    /// Looks up the owner FQI and snapshotted token for `uuid`. std::nullopt means this UUID
    /// was never registered here -- callers treat that as "not one of ours" and defer to the
    /// existing unknown-UUID error path rather than treating it as an authorization failure.
    std::optional<ObservableOwnerEntry> observableOwnerEntry(const std::string& uuid) const {
        // Part A p90 / Part B p88: UUID comparison MUST ignore case. `uuid` here
        // is the client-supplied CommandExecutionUUID (GrpcTransport.h follow-up
        // path); registerObservableOwner stores the server's own lower-case
        // UUID, so lowering the lookup key matches without touching the map.
        std::lock_guard<std::mutex> lock{observableOwners_->mu};
        auto it = observableOwners_->owners.find(util::asciiLower(uuid));
        if (it == observableOwners_->owners.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// Drops the ownership record for `uuid`, e.g. once the execution has finished and no
    /// further follow-up RPCs are expected. Erasing an absent key is a harmless no-op.
    void eraseObservableOwner(const std::string& uuid) const {
        // Same case-folding as observableOwnerEntry above, for the same reason.
        std::lock_guard<std::mutex> lock{observableOwners_->mu};
        observableOwners_->owners.erase(util::asciiLower(uuid));
    }
};

}  // namespace sila2
