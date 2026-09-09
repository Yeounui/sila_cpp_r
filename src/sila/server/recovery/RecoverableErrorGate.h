// RecoverableErrorGate.h — blocking gate for SiLA 2 Recoverable Execution
// Error handling (Feature Definition Language, ExecutionErrorHandling).
#pragma once

#include <sila/common/types/BasicTypes.h>

#include <any>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sila2 {
// Forward-declared: this header only stores a reference to it, and pulling
// in ObservablePropertyManager.h here would make every Feature translation
// unit that touches recovery also depend on the subscriber-queue machinery.
class ObservablePropertyManager;
}  // namespace sila2

namespace sila2::recovery {

// ObservablePropertyManager key for the RecoverableErrors property. Shared
// rather than duplicated: the gate publishes under it, ErrorRecoveryServiceImpl
// subscribes with it and SiLAServerBase registers the cloud route with it, and
// the three hand-synchronised copies had already drifted to a v1 spelling.
// const char*, not string_view: publish()/subscribe()/registerObservableProperty()
// all take const std::string& and string_view does not convert implicitly.
inline constexpr const char* kRecoverableErrorsPropertyId =
    "org.silastandard/core/ErrorRecoveryService/v2/Property/RecoverableErrors";

/// One recovery choice offered to the client for a raised error.
struct ContinuationOption {
    std::string identifier;
    bool isDefault = false;
    // Published as the RecoverableError's wire-level AutomaticSelectionTimeout
    // when this option is the one flagged isDefault
    // (ErrorRecoveryService-v2_0.sila.xml:261-272). FDL :264-268: "the client
    // shall select the defined default option automatically" once this
    // duration elapses -- the CLIENT performs that selection, not the gate;
    // the gate never resolves to this option on its own. 0 = no automatic
    // selection.
    std::chrono::seconds automaticSelectionTimeout{0};
    // Appended after automaticSelectionTimeout, not inserted earlier: existing
    // call sites use positional aggregate init ({"retry", false, seconds{0}})
    // and would silently misbind if a std::string landed mid-struct.
    std::string description;        // FDL ContinuationOption.Description
    std::string requiredInputData;  // FDL ContinuationOption.RequiredInputData
};

/// The client's answer to a raised error: which option, plus any option-
/// specific input data (e.g. a corrected parameter value).
struct RecoveryChoice {
    std::string optionIdentifier;
    std::any inputData;
};

/// One raised recoverable error, mirroring the FDL RecoverableError structure
/// (ErrorRecoveryService-v2_0.sila.xml:154). Published through
/// ObservablePropertyManager as a std::vector<RecoverableError>; the transports
/// convert it to the wire message. DefaultOption and AutomaticSelectionTimeout
/// are deliberately absent -- they are derived at conversion time from the
/// option flagged isDefault, so the FDL rule that DefaultOption must name one
/// of the ContinuationOptions holds by construction.
struct RecoverableError {
    std::string commandExecutionUuid;
    std::string errorIdentifier;
    std::string commandIdentifier;
    std::string errorMessage;
    std::vector<ContinuationOption> continuationOptions;
    // Stamped by raiseAndWait(); any value a caller sets is overwritten.
    sila2::types::Timestamp errorTime{};
};

/// Lets a Feature implementation raise a recoverable error mid-execution and
/// wait for the client to choose how to proceed, instead of failing the
/// command outright. A server enables ErrorRecoveryService by calling
/// `SiLAServerBase::Builder::WithErrorRecovery()`, which owns one gate
/// internally; a Feature calls raiseAndWait() with the error and its
/// ContinuationOptions, the client sees it published on the
/// ErrorRecoveryService RecoverableErrors @ref gl_observable_property "Observable Property" , and
/// answers by calling ExecuteContinuationOption
/// (or AbortErrorHandling), which resolves the blocked raiseAndWait() call.
///
/// Blocks a Feature execution thread on a recoverable error until the client
/// selects a ContinuationOption (or the gate is aborted/shut down).
///
/// Thread-safe: all public methods lock an internal mutex.
/// @see SiLAServerBase::Builder::WithErrorRecovery
class RecoverableErrorGate {
public:
    explicit RecoverableErrorGate(
        ObservablePropertyManager& propertyManager,
        std::chrono::seconds defaultTimeout = std::chrono::seconds{0});
    // Defined in the .cc: PendingEntry is only forward-declared below, and
    // unique_ptr's deleter needs the complete type at the point where
    // pending_ is destroyed.
    ~RecoverableErrorGate();

    /// Called from the Feature execution thread. Publishes the error and its
    /// options, then blocks the calling thread until the client responds via
    /// selectOption()/abort(), the timeout elapses, or releaseAll() runs.
    /// error.errorTime is stamped by this call and any caller-set value is
    /// overwritten. Throws std::invalid_argument before publishing anything
    /// if error violates an FDL structural constraint: a required string is
    /// empty; errorIdentifier exceeds MaximalLength 255; commandIdentifier is
    /// not a well-formed CommandIdentifier FQI; commandExecutionUuid is not
    /// Length 36 lowercase-hex UUID shape; continuationOptions is empty; an
    /// option identifier repeats; more than one option claims isDefault; an
    /// option's automaticSelectionTimeout is negative; or a non-default
    /// option carries a nonzero automaticSelectionTimeout.
    /// @return The client's choice, or nullopt on timeout/abort/shutdown --
    /// nullopt also covers the ErrorHandlingTimeout expiring with no client
    /// response (FDL :136-139); there is no server-side fallback to the
    /// default option (FDL :264-268 assigns that to the client).
    std::optional<RecoveryChoice> raiseAndWait(RecoverableError error);

    /// Called from a client (gRPC) thread to resolve a pending raiseAndWait().
    void selectOption(const std::string& commandExecutionUuid,
                     const std::string& optionIdentifier,
                     std::any inputData = {});

    /// Called from a client thread to give up on a pending raiseAndWait()
    /// without selecting an option.
    void abort(const std::string& commandExecutionUuid);

    /// Change ErrorHandlingTimeout (FDL :133-150), the server's only wait
    /// bound for raiseAndWait() -- it applies to every raise, regardless of
    /// any option's automaticSelectionTimeout.
    void setErrorHandlingTimeout(std::chrono::seconds timeout);

    /// Wake every blocked raiseAndWait() with nullopt. Used for server
    /// shutdown so no Feature thread is left blocked forever.
    void releaseAll();

private:
    struct PendingEntry;

    /// Push the current set of pending errors to the ObservablePropertyManager
    /// as an observable property update. Caller must hold mu_.
    void publishPendingErrors();

    struct ResolvedEntry {
        std::optional<std::string> optionId;
        std::chrono::steady_clock::time_point resolvedAt;
    };

    ObservablePropertyManager& propertyManager_;
    std::chrono::seconds defaultTimeout_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<PendingEntry>> pending_;
    // Tombstones for retry-safe resolution: maps a resolved UUID to the
    // chosen option identifier (nullopt if aborted/timed out). Entries
    // expire after kResolvedTtl; the hard cap (1024) evicts expired
    // entries first, then oldest.
    std::unordered_map<std::string, ResolvedEntry> resolved_;
    static constexpr std::chrono::seconds kResolvedTtl{60};
};

}  // namespace sila2::recovery
