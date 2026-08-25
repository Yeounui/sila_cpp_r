// ObservableCommandExecution.cc
#include "ObservableCommandExecution.h"

#include <stdexcept>
#include <utility>

namespace sila2 {
ObservableCommandExecution::ObservableCommandExecution(std::string uuid,
                                                         std::chrono::seconds lifetime)
    : uuid_{std::move(uuid)}, lifetime_{lifetime} {}

const std::string& ObservableCommandExecution::uuid() const { return uuid_; }

ObservableCommandExecution::State ObservableCommandExecution::state() const {
    std::lock_guard<std::mutex> lock{mu_};
    return state_;
}

std::string ObservableCommandExecution::stateToString(State state) {
    switch (state) {
    case State::Waiting:
        return "Waiting";
    case State::Running:
        return "Running";
    case State::FinishedSuccessfully:
        return "Finished Successfully";
    case State::FinishedWithError:
        return "Finished With Error";
    }
    // ponytail: no logging subsystem yet, add calls back when one exists
    return "";
}

void ObservableCommandExecution::start() {
    std::lock_guard<std::mutex> lock{mu_};
    if (state_ != State::Waiting) {
        throw std::logic_error{"ObservableCommandExecution::start: not in Waiting state"};
    }
    state_ = State::Running;
}

void ObservableCommandExecution::finish() {
    std::lock_guard<std::mutex> lock{mu_};
    if (state_ != State::Running) {
        throw std::logic_error{"ObservableCommandExecution::finish: not in Running state"};
    }
    state_ = State::FinishedSuccessfully;
    finishedAt_ = std::chrono::steady_clock::now();
}

void ObservableCommandExecution::fail(std::string errorMessage) {
    std::lock_guard<std::mutex> lock{mu_};
    if (state_ != State::Running) {
        throw std::logic_error{"ObservableCommandExecution::fail: not in Running state"};
    }
    state_ = State::FinishedWithError;
    errorMessage_ = std::move(errorMessage);
    finishedAt_ = std::chrono::steady_clock::now();
}

void ObservableCommandExecution::setProgress(double fraction, std::chrono::seconds remaining) {
    // No state check here, unlike start()/finish()/fail(): a progress hint
    // arriving slightly before Running is observed, or after finish() has
    // already landed, is still just a stale hint, not a state violation.
    std::lock_guard<std::mutex> lock{mu_};
    progress_ = fraction;
    remaining_ = remaining;
}

double ObservableCommandExecution::progress() const {
    std::lock_guard<std::mutex> lock{mu_};
    return progress_;
}

std::chrono::seconds ObservableCommandExecution::estimatedRemaining() const {
    std::lock_guard<std::mutex> lock{mu_};
    return remaining_;
}

void ObservableCommandExecution::requestInterruption() { interruptionRequested_ = true; }

bool ObservableCommandExecution::isInterruptionRequested() const {
    return interruptionRequested_;
}

bool ObservableCommandExecution::isExpired() const {
    std::lock_guard<std::mutex> lock{mu_};
    if (lifetime_.count() == 0) { return false; }
    if (state_ != State::FinishedSuccessfully && \
        state_ != State::FinishedWithError) { return false; }
    return std::chrono::steady_clock::now() - finishedAt_ >= lifetime_;
}

std::string ObservableCommandExecution::errorMessage() const {
    std::lock_guard<std::mutex> lock{mu_};
    return errorMessage_;
}
}  // namespace sila2
