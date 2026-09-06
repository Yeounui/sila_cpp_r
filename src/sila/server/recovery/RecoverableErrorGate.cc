// RecoverableErrorGate.cc — blocking gate for SiLA 2 Recoverable Execution
// Error handling (Feature Definition Language, ExecutionErrorHandling).
#include "RecoverableErrorGate.h"

#include <sila/common/error/SiLAErrorSubtypes.h>
#include <sila/common/types/Constraints.h>
#include <sila/common/util/AsciiCase.h>
#include <sila/server/property/ObservablePropertyManager.h>

#include <algorithm>
#include <condition_variable>
#include <set>
#include <stdexcept>
#include <utility>

namespace sila2::recovery {

namespace {
// Mirrors ErrorRecoveryServiceImpl.cc's kUuidPattern (inbound check) -- same
// shape, applied here on the way IN so a raise can never publish a
// CommandExecutionUUID that ExecuteContinuationOption/AbortErrorHandling
// would then reject (FDL :204-207); an unvalidated raise would otherwise
// publish an error no client could ever resolve or abort.
const std::string kUuidPattern =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

// FDL :189-191 -- CommandIdentifier carries FullyQualifiedIdentifier, but
// types::checkFullyQualifiedIdentifier() (Constraints.cc) only accepts the
// four-segment Feature FQI form ("org.example/Category/Name/vN") and would
// reject every valid CommandIdentifier, which is six segments
// ("<feature-fqi>/Command/<Name>") -- so a dedicated pattern is used instead
// of reusing that helper.
// Segments mirror Constraints.cc kFqiPattern's DEFINITION grammar (dotted
// Originator/Category, an Identifier) plus the /Command/<Name> suffix. Part A
// p87 requires FQI VALUES to be compared without regard to case, and
// types::checkPattern()'s std::regex compiles with no icase flag (it is
// shared with case-sensitive FDL patterns like UUIDs), so this grammar is
// spelled all-lower and the VALUE is folded at the call site below instead.
const std::string kCommandIdentifierFqiPattern =
    R"([a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*/[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*/[a-z][a-z0-9]*/v[0-9]+/command/[a-z][a-z0-9]*)";
}  // namespace

struct RecoverableErrorGate::PendingEntry {
    RecoverableError error;
    std::optional<RecoveryChoice> choice;
    bool resolved = false;
    std::condition_variable cv;
};

RecoverableErrorGate::RecoverableErrorGate(
    ObservablePropertyManager& propertyManager,
    std::chrono::seconds defaultTimeout)
    : propertyManager_(propertyManager), defaultTimeout_(defaultTimeout) {
    // SiLA Part A: an Observable Property "can be read at any time", yet the only
    // RPC generated for it is Subscribe_RecoverableErrors -- so a client that
    // subscribes before any error is raised must still receive the current value
    // immediately. RecoverableErrors is a List, whose value is always defined:
    // the empty list when nothing is pending. Seed that empty value now so the
    // property has a readable current value from construction onward; every later
    // subscriber replays it (ObservablePropertyManager retains the last publish).
    std::lock_guard<std::mutex> lock{mu_};
    publishPendingErrors();  // pending_ is empty here -> publishes []
}

RecoverableErrorGate::~RecoverableErrorGate() {
    // Contract (see header): callers must ensure no raiseAndWait() is still
    // blocked when this destructor runs, same as
    // ObservablePropertyManager::shutdown().
    releaseAll();
}

std::optional<RecoveryChoice> RecoverableErrorGate::raiseAndWait(RecoverableError error) {
    // Every element of the FDL RecoverableError structure is required, and
    // ContinuationOptions carries MinimalElementCount 1
    // (ErrorRecoveryService-v2_0.sila.xml:231-245). Rejecting here keeps a
    // structurally invalid error off the observable property entirely.
    if (error.commandExecutionUuid.empty()) {
        throw std::invalid_argument{"RecoverableError.commandExecutionUuid is required"};
    }
    if (error.errorIdentifier.empty()) {
        throw std::invalid_argument{"RecoverableError.errorIdentifier is required"};
    }
    // FDL :174-176 -- ErrorIdentifier is a Constrained String, MaximalLength 255.
    if (auto tooLong = types::checkMaximalLength(error.errorIdentifier, 255)) {
        throw std::invalid_argument{"RecoverableError.errorIdentifier: " + *tooLong};
    }
    if (error.commandIdentifier.empty()) {
        throw std::invalid_argument{"RecoverableError.commandIdentifier is required"};
    }
    // FDL :189-191 -- CommandIdentifier carries FullyQualifiedIdentifier. Fold
    // the value before matching the all-lower grammar above (Part A p87:
    // FQIs are compared without regard to case; checkPattern's std::regex has
    // no icase flag here, so the value is folded instead of the pattern).
    if (auto badFqi = types::checkPattern(util::asciiLower(error.commandIdentifier),
                                          kCommandIdentifierFqiPattern)) {
        throw std::invalid_argument{"RecoverableError.commandIdentifier: " + *badFqi};
    }
    // FDL :204-207 -- CommandExecutionUUID Length 36 + lowercase-hex Pattern.
    if (auto lengthError = types::checkLength(error.commandExecutionUuid, 36)) {
        throw std::invalid_argument{"RecoverableError.commandExecutionUuid: " + *lengthError};
    }
    if (auto patternError = types::checkPattern(error.commandExecutionUuid, kUuidPattern)) {
        throw std::invalid_argument{"RecoverableError.commandExecutionUuid: " + *patternError};
    }
    if (error.continuationOptions.empty()) {
        throw std::invalid_argument{
            "RecoverableError.continuationOptions needs at least one entry"};
    }
    std::set<std::string> seenOptionIds;
    bool sawDefault = false;
    for (const auto& option : error.continuationOptions) {
        if (option.identifier.empty()) {
            throw std::invalid_argument{"ContinuationOption.identifier is required"};
        }
        // FDL :299 -- must be unique within the Recoverable Error item.
        if (!seenOptionIds.insert(option.identifier).second) {
            throw std::invalid_argument{
                "duplicate ContinuationOption identifier: " + option.identifier};
        }
        // FDL :250 -- DefaultOption is a single identifier, so at most one
        // option may claim it.
        if (option.isDefault) {
            if (sawDefault) {
                throw std::invalid_argument{"more than one default ContinuationOption"};
            }
            sawDefault = true;
        }
        // FDL :339-340 -- Timeout DataTypeDefinition, MinimalInclusive 0.
        // std::chrono::seconds is signed, so a negative value is reachable.
        if (auto rangeError = types::checkMinimalInclusive<int64_t>(
                option.automaticSelectionTimeout.count(), 0)) {
            throw std::invalid_argument{
                "ContinuationOption.automaticSelectionTimeout: " + *rangeError};
        }
        // The wire carries exactly one AutomaticSelectionTimeout, derived
        // solely from the option flagged isDefault
        // (ErrorRecoveryServiceImpl.cc fillRecoverableErrorsResponse). A
        // timeout on a non-default option would otherwise be silently
        // dropped rather than rejected.
        if (!option.isDefault && option.automaticSelectionTimeout.count() != 0) {
            throw std::invalid_argument{
                "ContinuationOption.automaticSelectionTimeout is only valid on "
                "the option flagged isDefault: " + option.identifier};
        }
    }

    // FDL :212 -- "the point in time when the error occurred" is the moment
    // the gate begins blocking, so the gate stamps it rather than trusting
    // the caller.
    error.errorTime = sila2::types::timestampFromSystemClock(std::chrono::system_clock::now());
    // Copied out before the move: try_emplace's key, the resolved_
    // bookkeeping at the end of the function and pending_.erase all read it
    // after `error` has been moved into the PendingEntry.
    const std::string uuid = error.commandExecutionUuid;

    auto entry = std::make_unique<PendingEntry>();
    entry->error = std::move(error);
    // Raw pointer kept for use after moving the unique_ptr into the map —
    // the map, not this function, owns the entry's lifetime.
    PendingEntry* entryPtr = entry.get();

    std::unique_lock<std::mutex> lock{mu_};
    auto [mapIt, inserted] = pending_.try_emplace(uuid, std::move(entry));
    if (!inserted) {
        // try_emplace does not move from entry on collision — unique_ptr
        // destructor cleans up the PendingEntry when the exception unwinds.
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid,
            "duplicate command execution UUID"};
    }
    publishPendingErrors();

    // FDL :136-139 (SetErrorHandlingTimeout) is the server's only wait bound.
    // AutomaticSelectionTimeout (FDL :264-268) is published FOR the client --
    // "the client shall select the defined default option automatically" --
    // and is never folded into the server's own wait here; ContinuationOption
    // ::automaticSelectionTimeout above is read only by
    // ErrorRecoveryServiceImpl.cc when it builds the wire message.
    const auto effectiveTimeout = defaultTimeout_;

    if (effectiveTimeout.count() > 0) {
        entryPtr->cv.wait_for(lock, effectiveTimeout,
                              [entryPtr] { return entryPtr->resolved; });
    } else {
        // FDL :139 -- "A value of zero specifies an indefinite time" — block
        // until the client resolves this error or the gate is aborted/shut down.
        entryPtr->cv.wait(lock, [entryPtr] { return entryPtr->resolved; });
    }

    std::optional<RecoveryChoice> result;
    if (entryPtr->resolved) {
        result = std::move(entryPtr->choice);
    }
    // No else: an unresolved wait yields nullopt here. The caller (a blocked
    // Feature execution) turns that into an unrecoverable SiLA error -- FDL
    // :29-33 -- exactly by removing this item below and returning nullopt.
    // There is deliberately no server-side fallback to the default option.

    if (resolved_.size() >= 1024) {
        auto now = std::chrono::steady_clock::now();
        std::erase_if(resolved_, [now](const auto& pair) {
            return (now - pair.second.resolvedAt) > kResolvedTtl;
        });
    }
    resolved_.emplace(uuid,
                      ResolvedEntry{result ? std::optional{result->optionIdentifier}
                                           : std::nullopt,
                                    std::chrono::steady_clock::now()});
    pending_.erase(uuid);
    publishPendingErrors();
    return result;
}

void RecoverableErrorGate::selectOption(const std::string& commandExecutionUuid,
                                       const std::string& optionIdentifier,
                                       std::any inputData) {
    std::lock_guard<std::mutex> lock{mu_};
    auto resolvedIt = resolved_.find(commandExecutionUuid);
    if (resolvedIt != resolved_.end()) {
        auto age = std::chrono::steady_clock::now() - resolvedIt->second.resolvedAt;
        if (age <= kResolvedTtl) {
            // Idempotent retry: same option chosen before — silently succeed.
            if (resolvedIt->second.optionId.has_value() &&
                *resolvedIt->second.optionId == optionIdentifier) {
                return;
            }
            // The FDL declares only InvalidCommandExecutionUUID and UnknownContinuationOption
            // (ErrorRecoveryService-v2_0.sila.xml:357-370). To the client an already-resolved
            // UUID has no pending error, which is exactly InvalidCommandExecutionUUID.
            throw sila2::error::DefinedExecutionError{
                "org.silastandard/core/ErrorRecoveryService/v2/DefinedExecutionError/InvalidCommandExecutionUUID",
                "command already resolved with a different option"};
        }
        // Expired — treat as absent, fall through to pending_ lookup.
        resolved_.erase(resolvedIt);
    }
    auto it = pending_.find(commandExecutionUuid);
    if (it == pending_.end()) {
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid};
    }
    PendingEntry& entry = *it->second;

    const bool optionExists =
        std::any_of(entry.error.continuationOptions.begin(), entry.error.continuationOptions.end(),
                    [&optionIdentifier](const ContinuationOption& option) {
                        return option.identifier == optionIdentifier;
                    });
    if (!optionExists) {
        // FDL name is UnknownContinuationOption, not InvalidContinuationOptionIdentifier
        // (ErrorRecoveryService-v2_0.sila.xml:366-367) — the latter is not FDL-declared.
        throw sila2::error::DefinedExecutionError{
            "org.silastandard/core/ErrorRecoveryService/v2/DefinedExecutionError/UnknownContinuationOption",
            "Unknown continuation option: " + optionIdentifier};
    }

    entry.choice = RecoveryChoice{optionIdentifier, std::move(inputData)};
    entry.resolved = true;
    entry.cv.notify_one();
}

void RecoverableErrorGate::abort(const std::string& commandExecutionUuid) {
    std::lock_guard<std::mutex> lock{mu_};
    auto resolvedIt = resolved_.find(commandExecutionUuid);
    if (resolvedIt != resolved_.end()) {
        // Already resolved — nothing to abort.
        auto age = std::chrono::steady_clock::now() - resolvedIt->second.resolvedAt;
        if (age <= kResolvedTtl) {
            return;
        }
        resolved_.erase(resolvedIt);
    }
    auto it = pending_.find(commandExecutionUuid);
    if (it == pending_.end()) {
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid};
    }
    PendingEntry& entry = *it->second;
    entry.resolved = true;
    entry.cv.notify_one();
}

void RecoverableErrorGate::setErrorHandlingTimeout(std::chrono::seconds timeout) {
    std::lock_guard<std::mutex> lock{mu_};
    defaultTimeout_ = timeout;
}

void RecoverableErrorGate::releaseAll() {
    std::lock_guard<std::mutex> lock{mu_};
    // Do NOT clear pending_ here — each blocked raiseAndWait() erases its own
    // entry after waking, since PendingEntry::cv must outlive the wait call.
    for (auto& [uuid, entry] : pending_) {
        entry->resolved = true;
        entry->cv.notify_one();
    }
    resolved_.clear();
}

void RecoverableErrorGate::publishPendingErrors() {
    std::vector<RecoverableError> errors;
    errors.reserve(pending_.size());
    for (const auto& [uuid, entry] : pending_) {
        errors.push_back(entry->error);
    }
    propertyManager_.publish(kRecoverableErrorsPropertyId, std::any{std::move(errors)});
}

}  // namespace sila2::recovery
