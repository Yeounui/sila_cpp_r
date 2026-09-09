// ExecutionStore.h — client-side persistence for issued Observable Command
// executions (Part A p33).
//
// Part A p33 (l.1750-1755): "In case a Connection has been closed or lost,
// the SiLA Server MUST ... continue processing Observable Commands that the
// SiLA Client has initiated ... Command Execution UUIDs MUST remain valid as
// specified, so that a SiLA Client is still able to use it after having
// re-connected to the SiLA Server." The owner's binding directive (2026-09-03)
// extends this guarantee to the client: a client process that restarts while
// a command is still running must be able to recover the UUID it issued and
// re-attach, rather than losing it because it only ever lived in a local
// variable.
//
// An empty store path means persistence is off: unlike the server side
// (ConnectionConfigurationServiceImpl), there is no request-level "Persist"
// flag forcing storage here, so the pre-existing no-persistence behaviour
// stays the default and this class is opt-in via ClientConfig.
//
// Mirrors ConnectionConfigurationServiceImpl's on-disk discipline (temp file
// + atomic rename, fail loud on a corrupt file, a missing file is just an
// empty store) rather than inventing a second format.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sila2 {

/// One issued-but-not-yet-terminal Observable Command execution.
struct PersistedExecution {
    std::string serverUuid;
    std::string featureFqi;
    std::string commandId;
    std::string executionUuid;
    std::string issueTime;
    std::string lastStatus;
};

/// File-backed record of @ref gl_observable_command "Observable Command"
/// executions this client has issued and not yet seen reach a terminal
/// state. Enabled by ClientConfig::setExecutionStorePath() and obtained from
/// SilaClientBase::executionStore(). Thread-safe: record()/prune()/list()
/// all take the internal lock.
class ExecutionStore {
public:
    /// Loads any existing state at `storePath` immediately. `storePath` must
    /// be non-empty — the assembly layer (SilaClientBase) only constructs an
    /// ExecutionStore when a path is actually configured, so an empty path
    /// here would be a caller bug, not the "persistence off" case (that case
    /// is expressed by not constructing the store at all).
    /// @throws std::runtime_error if the file exists but is corrupt.
    explicit ExecutionStore(std::filesystem::path storePath);

    /// Upserts `execution`, keyed by executionUuid compared case-insensitively
    /// (Part A p90: UUIDs, like FQIs, are compared without regard to case).
    void record(const PersistedExecution& execution);

    /// Removes the execution with this UUID, if present. Called once the
    /// runner observes a terminal status (kFinishedSuccessfully or
    /// kFinishedWithError) for it.
    void prune(const std::string& executionUuid);

    [[nodiscard]] std::vector<PersistedExecution> list() const;

    /// Same as list(), filtered to executions issued against `serverUuid`
    /// (case-insensitive, Part A p90).
    [[nodiscard]] std::vector<PersistedExecution> list(const std::string& serverUuid) const;

private:
    // Full rewrite to a sibling ".tmp" file then atomic rename, matching
    // ConnectionConfigurationServiceImpl::saveState (server/features/
    // ConnectionConfigurationServiceImpl.cc:393-440) — a write failure leaves
    // the previous valid state intact instead of truncating the only copy.
    void save() const;

    // Reconstructs executions_ from storePath_. A missing file means a first
    // run with nothing persisted yet, not an error, matching
    // ConnectionConfigurationServiceImpl::loadState (:444-539).
    void load() const;

    // No per-instance mutex: every instance serialises on one process-wide
    // lock in the .cc, because several instances may share one path and
    // their load-mutate-save cycles must not interleave.
    std::filesystem::path storePath_;
    mutable std::vector<PersistedExecution> executions_;
};

}  // namespace sila2
