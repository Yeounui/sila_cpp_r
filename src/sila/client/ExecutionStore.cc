// ExecutionStore.cc
#include <sila/client/ExecutionStore.h>

#include <sila/common/util/AsciiCase.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sila2 {

ExecutionStore::ExecutionStore(std::filesystem::path storePath)
    : storePath_{std::move(storePath)} {
    // Constructor runs single-threaded (the object doesn't exist yet for any
    // other thread to reach), so load() needs no lock even though it touches
    // executions_ directly.
    load();
}

namespace {

// One process-wide lock for every ExecutionStore: instances sharing a path
// must serialise their load-mutate-save cycles with each other, and a
// per-instance mutex cannot do that. ponytail: single global lock (N is a
// handful of clients); a per-path lock table or flock() if it ever contends
// or two processes share a store.
std::mutex& storeMutex() {
    static std::mutex mutex;
    return mutex;
}

// The TSV rows are tab/newline delimited, so a field carrying either would
// be read back as a malformed record and make the whole store unloadable.
void rejectDelimiters(const PersistedExecution& execution) {
    for (const std::string* field : {&execution.serverUuid, &execution.featureFqi,
                                     &execution.commandId, &execution.executionUuid,
                                     &execution.issueTime, &execution.lastStatus}) {
        if (field->find_first_of("\t\n") != std::string::npos) {
            throw std::invalid_argument{
                "ExecutionStore::record: field contains a tab or newline: " + *field};
        }
    }
}

}  // namespace

void ExecutionStore::record(const PersistedExecution& execution) {
    rejectDelimiters(execution);
    std::lock_guard<std::mutex> lock{storeMutex()};
    // The file is the source of truth: several SilaClientBase instances (one
    // per registered server) may share one configured path, so re-read it
    // before mutating instead of overwriting the file from a stale snapshot.
    // ponytail: no cross-process file lock; add flock() if two processes ever
    // share a store.
    load();
    // Case-insensitive UUID keying (Part A p90) — a reattach or a duplicate
    // issue callback with a differently-cased UUID must update the same row,
    // not create a second one.
    const auto lowered = util::asciiLower(execution.executionUuid);
    auto it = std::find_if(executions_.begin(), executions_.end(),
        [&lowered](const PersistedExecution& e) {
            return util::asciiLower(e.executionUuid) == lowered;
        });
    if (it != executions_.end()) {
        *it = execution;
    } else {
        executions_.push_back(execution);
    }
    save();
}

void ExecutionStore::prune(const std::string& executionUuid) {
    std::lock_guard<std::mutex> lock{storeMutex()};
    load();  // see record()
    const auto lowered = util::asciiLower(executionUuid);
    std::erase_if(executions_, [&lowered](const PersistedExecution& e) {
        return util::asciiLower(e.executionUuid) == lowered;
    });
    save();
}

std::vector<PersistedExecution> ExecutionStore::list() const {
    std::lock_guard<std::mutex> lock{storeMutex()};
    load();  // see record()
    return executions_;
}

std::vector<PersistedExecution> ExecutionStore::list(const std::string& serverUuid) const {
    std::lock_guard<std::mutex> lock{storeMutex()};
    load();  // see record()
    const auto lowered = util::asciiLower(serverUuid);
    std::vector<PersistedExecution> result;
    for (const auto& e : executions_) {
        if (util::asciiLower(e.serverUuid) == lowered) {
            result.push_back(e);
        }
    }
    return result;
}

void ExecutionStore::save() const {
    // Same temp-file + atomic-rename discipline as
    // ConnectionConfigurationServiceImpl::saveState (server/features/
    // ConnectionConfigurationServiceImpl.cc:393-440): the rename is atomic
    // within one filesystem, so a crash mid-write never corrupts the
    // previously-committed file.
    const auto tmpPath = std::filesystem::path{storePath_} += ".tmp";
    {
        std::ofstream out{tmpPath, std::ios::trunc};
        if (!out) {
            throw std::runtime_error{
                "ExecutionStore::save: could not open " + tmpPath.string()};
        }
        for (const auto& e : executions_) {
            out << e.serverUuid << '\t' << e.featureFqi << '\t' << e.commandId << '\t'
                << e.executionUuid << '\t' << e.issueTime << '\t' << e.lastStatus << '\n';
        }
        out.flush();
        if (!out) {
            throw std::runtime_error{
                "ExecutionStore::save: could not write " + tmpPath.string()};
        }
    }  // close the stream before renaming

    std::error_code ec;
    std::filesystem::rename(tmpPath, storePath_, ec);
    if (ec) {
        throw std::runtime_error{
            "ExecutionStore::save: could not replace " + storePath_.string() + ": " +
            ec.message()};
    }
}

void ExecutionStore::load() const {
    // A missing file just means a first run with nothing persisted yet — not
    // an error, matching ConnectionConfigurationServiceImpl::loadState.
    if (!std::filesystem::exists(storePath_)) {
        executions_.clear();  // another instance may have emptied the store
        return;
    }

    std::ifstream in{storePath_};
    if (!in) {
        throw std::runtime_error{
            "ExecutionStore::load: could not open " + storePath_.string()};
    }

    // Parse into a local vector and commit to executions_ only once the whole
    // file has been read cleanly, so a corrupt record mid-file leaves
    // executions_ untouched rather than half-populated.
    std::vector<PersistedExecution> loaded;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        // Tab-split, mirroring ConnectionConfigurationServiceImpl::loadState
        // (:476-485) exactly, so both stores share one parsing idiom.
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab - start));
            if (tab == std::string::npos) {
                break;
            }
            start = tab + 1;
        }

        // One record kind, exactly 6 fields: serverUuid, featureFqi,
        // commandId, executionUuid, issueTime, lastStatus.
        if (fields.size() != 6) {
            throw std::runtime_error{
                "ExecutionStore::load: corrupt state file " + storePath_.string()};
        }
        loaded.push_back(PersistedExecution{
            fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]});
    }

    executions_ = std::move(loaded);
}

}  // namespace sila2
