// ShakeControllerImpl.cc — example Feature implementation (architecture.md §8).
//
// Teaching material: wires each codegen-generated SilaHandler member of
// ShakeControllerServiceAdapter to a lambda that implements the Feature's
// domain logic. ShakeForTime is the interesting case — an Observable Command
// with its own worker thread, mirrored in the ShakeForTime_Info/_Intermediate
// /_Result RPCs that read that thread's progress concurrently. See ShakeState
// in the header for the shared-state contract between the worker and the
// _Intermediate handler.
#include "ShakeControllerImpl.h"

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/command/ObservableCommandExecution.h>
#include <sila/server/transport/CallContext.h>
#include <sila/server/transport/ResponseSink.h>

#include "SiLAFramework.pb.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace shake_example {

using namespace std::chrono_literals;

ShakeControllerImpl::ShakeControllerImpl(const sila2::InterceptorChain* chain)
    : adapter_{std::make_shared<gen::ShakeControllerServiceAdapter>(chain)} {
    // --- ShakeForTime: Observable Command (initiation) ---
    // Registers the shake, then hands the actual "wait N seconds" work to a
    // detached worker thread so this handler returns immediately with a
    // CommandExecutionUUID, per SiLA 2's Observable Command RPC pattern.
    adapter_->onShakeForTime = [this](
            const shake_proto::ShakeForTime_Parameters& req,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<fw::CommandConfirmation>& sink) {
        if (req.runtime().value() < 1) {
            // SiLAFramework.proto:96: parameter must be the FQI, not the bare
            // identifier. Built from gen::kFqi so it survives a Feature rename
            // in a repository that copies this file.
            const std::string runtimeParamFqi =
                std::string{gen::kFqi} + "/Command/ShakeForTime/Parameter/Runtime";
            sink.fail(sila2::error::ValidationError{runtimeParamFqi, "Runtime must be at least 1"});
            return;
        }
        auto exec = cmdManager_.addCommand(std::chrono::seconds{60});

        auto state = std::make_shared<ShakeState>();
        state->totalSeconds = req.runtime().value();
        state->timeLeft = static_cast<double>(state->totalSeconds);

        {
            std::lock_guard<std::mutex> lock{statesMu_};
            states_[exec->uuid()] = state;
        }
        shaking_ = true;
        exec->start();

        const int totalSeconds = state->totalSeconds;
        // Captures the shared_ptr by value (not a raw reference): the worker
        // thread outlives this handler and must keep the execution alive on
        // its own, independent of cmdManager_'s GC sweep (architecture.md §4.2d).
        std::thread([this, exec, state, totalSeconds] {
            for (int elapsed = 0; elapsed < totalSeconds; ++elapsed) {
                std::this_thread::sleep_for(1s);

                double timeLeft;
                {
                    std::lock_guard<std::mutex> lock{state->mu};
                    state->timeLeft -= 1.0;
                    timeLeft = state->timeLeft;
                }
                state->cv.notify_all();

                const double fraction = static_cast<double>(elapsed + 1) / totalSeconds;
                exec->setProgress(fraction, std::chrono::seconds{static_cast<int>(timeLeft)});

                // Direct gRPC's _Info stream no longer sets this (§3.3 ②,
                // architecture-v2.md:286) -- the only caller left is
                // ObservableCommandManager::interruptAll() on server shutdown
                // (SilaServerBase.cc), which this poll still has to honor.
                if (exec->isInterruptionRequested()) {
                    exec->fail("Cancelled");
                    {
                        std::lock_guard<std::mutex> lock{state->mu};
                        state->done = true;
                    }
                    shaking_ = false;
                    state->cv.notify_all();
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> lock{state->mu};
                state->done = true;
            }
            shaking_ = false;
            exec->finish();
            state->cv.notify_all();
        }).detach();

        fw::CommandConfirmation confirmation;
        confirmation.mutable_commandexecutionuuid()->set_value(exec->uuid());
        if (exec->lifetime() > std::chrono::seconds{0}) {
            // SiLAFramework.proto:69 -- the client needs to know how long the
            // UUID stays resolvable, which is exactly the lifetime handed to
            // addCommand above and enforced by ObservableCommandManager's GC.
            // Skipped for a zero lifetime: Duration{0} on the wire means
            // "already expired", not "never expires" (addCommand's default IS
            // zero, ObservableCommandManager.h:36).
            confirmation.mutable_lifetimeofexecution()->set_seconds(exec->lifetime().count());
        }
        sink.send(confirmation);
        sink.finish();
    };

    // --- ShakeForTime_Info: Observable Command execution info stream ---
    adapter_->onShakeForTimeInfo = [this](
            const fw::CommandExecutionUUID& req,
            sila2::CallContext& ctx,
            sila2::ResponseSink<fw::ExecutionInfo>& sink) {
        auto exec = cmdManager_.getCommand(req.value());
        std::optional<fw::ExecutionInfo> lastSent;
        while (true) {
            const auto state = exec->state();
            fw::ExecutionInfo info;
            switch (state) {
            case sila2::ObservableCommandExecution::State::Waiting:
                info.set_commandstatus(fw::ExecutionInfo_CommandStatus_waiting);
                break;
            case sila2::ObservableCommandExecution::State::Running:
                info.set_commandstatus(fw::ExecutionInfo_CommandStatus_running);
                info.mutable_progressinfo()->set_value(exec->progress());
                break;
            case sila2::ObservableCommandExecution::State::FinishedSuccessfully:
                info.set_commandstatus(fw::ExecutionInfo_CommandStatus_finishedSuccessfully);
                break;
            case sila2::ObservableCommandExecution::State::FinishedWithError:
                info.set_commandstatus(fw::ExecutionInfo_CommandStatus_finishedWithError);
                break;
            }
            if (exec->lifetime() > std::chrono::seconds{0}) {
                // SiLAFramework.proto:82, same zero-means-unset rule as the
                // confirmation above. Reports the configured window, not a
                // countdown: isExpired() measures the lifetime from finishedAt_,
                // so while the command runs the remaining validity IS the
                // constant, and it therefore does not defeat the send-only-on-
                // change comparison.
                info.mutable_updatedlifetimeofexecution()->set_seconds(exec->lifetime().count());
            }
            // First snapshot always goes out, then only transitions -- the same
            // policy the _Intermediate handler below follows via lastSent, and
            // the one the router's synthesized pump honours for _Info
            // (CloudEnvelopeRouter.cc's push-on-subscribe-then-only-on-change
            // comment). SerializeAsString comparison rather than a hand-written
            // field list: proto3 messages have no operator==, and this file is
            // teaching material -- an explicit field list would silently stop
            // detecting a field a later FDL adds.
            if (!lastSent || lastSent->SerializeAsString() != info.SerializeAsString()) {
                sink.send(info);
                lastSent = info;
            }

            const bool isFinished =
                state == sila2::ObservableCommandExecution::State::FinishedSuccessfully ||
                state == sila2::ObservableCommandExecution::State::FinishedWithError;
            if (ctx.isCancelled()) {
                // architecture-v2.md:286: direct gRPC's IsCancelled() cannot
                // tell "client explicitly dropped this _Info RPC" apart from
                // "the whole connection died" -- both surface identically, and
                // the standard requires the server to keep executing across the
                // latter (§3.3 ②). With no reliable way to tell them apart,
                // this stream drop must never interrupt the run: stop sending,
                // let the worker thread finish on its own. Only the cloud
                // transport's explicit Cancel* envelope can request interruption
                // (CloudEnvelopeRouter.cc), because only it can distinguish the
                // two cases.
                break;
            }
            if (isFinished) {
                break;
            }
            std::this_thread::sleep_for(200ms);
        }
        sink.finish();
    };

    // --- ShakeForTime_Intermediate: TimeLeft stream ---
    // Streams a new IntermediateResponse each time the worker thread updates
    // ShakeState::timeLeft, tracked via lastSent so a wakeup with no actual
    // change (spurious or the 200ms poll timeout) sends nothing.
    adapter_->onShakeForTimeIntermediate = [this](
            const fw::CommandExecutionUUID& req,
            sila2::CallContext& ctx,
            sila2::ResponseSink<shake_proto::ShakeForTime_IntermediateResponses>& sink) {
        std::shared_ptr<ShakeState> state;
        {
            std::lock_guard<std::mutex> lock{statesMu_};
            const auto it = states_.find(req.value());
            if (it != states_.end()) {
                state = it->second;
            }
        }
        if (!state) {
            // Not a UUID this handler created — delegate to getCommand()
            // purely to raise the standard FrameworkError for an unknown
            // Command Execution UUID.
            cmdManager_.getCommand(req.value());
            sink.finish();
            return;
        }

        double lastSent = -1.0;
        std::unique_lock<std::mutex> lock{state->mu};
        while (true) {
            state->cv.wait_for(lock, 200ms, [&] {
                return state->timeLeft != lastSent || state->done;
            });
            if (state->timeLeft != lastSent) {
                const double timeLeft = state->timeLeft;
                lock.unlock();
                shake_proto::ShakeForTime_IntermediateResponses resp;
                resp.mutable_timeleft()->set_value(timeLeft);
                sink.send(resp);
                lock.lock();
                lastSent = timeLeft;
            }
            if (state->done || ctx.isCancelled()) {
                break;
            }
        }
        sink.finish();
    };

    // --- ShakeForTime_Result: returns the response, or the standard error, for the current state ---
    adapter_->onShakeForTimeResult = [this](
            const fw::CommandExecutionUUID& req,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<shake_proto::ShakeForTime_Responses>& sink) {
        // Raises FrameworkError for an unknown Command Execution UUID.
        auto exec = cmdManager_.getCommand(req.value());
        switch (exec->state()) {
        case sila2::ObservableCommandExecution::State::Waiting:
        case sila2::ObservableCommandExecution::State::Running:
            // Part A p51: _Result before completion MUST return a Command
            // Execution Not Finished Error, not block the serving thread
            // (direct gRPC has no other thread to serve the next request).
            throw sila2::error::FrameworkError{
                sila2::error::FrameworkError::FrameworkErrorType::CommandExecutionNotFinished,
                "ShakeForTime has not finished yet"};
        case sila2::ObservableCommandExecution::State::FinishedWithError:
            // Part A p51 (S68): a failed execution MUST return its error, not
            // an empty OK response.
            throw sila2::error::UndefinedExecutionError{exec->errorMessage()};
        case sila2::ObservableCommandExecution::State::FinishedSuccessfully:
            sink.send(shake_proto::ShakeForTime_Responses{});
            sink.finish();
            break;
        }
    };

    // --- StopShaking: Unobservable Command ---
    // Demonstrates the DefinedExecutionError pattern: calling StopShaking
    // while nothing is shaking is a Feature-declared error, not a no-op.
    adapter_->onStopShaking = [this](
            const shake_proto::StopShaking_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<shake_proto::StopShaking_Responses>& sink) {
        if (!shaking_) {
            sink.fail(sila2::error::DefinedExecutionError{
                std::string{gen::kError_CancelledError}, "Not currently shaking"});
            return;
        }
        shaking_ = false;
        sink.send(shake_proto::StopShaking_Responses{});
        sink.finish();
    };

    // --- StartShaking: Unobservable Command, indefinite shake ---
    adapter_->onStartShaking = [this](
            const shake_proto::StartShaking_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<shake_proto::StartShaking_Responses>& sink) {
        shaking_ = true;
        sink.send(shake_proto::StartShaking_Responses{});
        sink.finish();
    };

    // --- GoHome / UnlockPlate / LockPlate: trivial simulated hardware moves ---
    adapter_->onGoHome = [](
            const shake_proto::GoHome_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<shake_proto::GoHome_Responses>& sink) {
        sink.send(shake_proto::GoHome_Responses{});
        sink.finish();
    };
    adapter_->onUnlockPlate = [](
            const shake_proto::UnlockPlate_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<shake_proto::UnlockPlate_Responses>& sink) {
        sink.send(shake_proto::UnlockPlate_Responses{});
        sink.finish();
    };
    adapter_->onLockPlate = [](
            const shake_proto::LockPlate_Parameters& /*req*/,
            sila2::CallContext& /*ctx*/,
            sila2::ResponseSink<shake_proto::LockPlate_Responses>& sink) {
        sink.send(shake_proto::LockPlate_Responses{});
        sink.finish();
    };
}

std::shared_ptr<grpc::Service> ShakeControllerImpl::service() const {
    return adapter_;
}

sila2::ObservableCommandManager& ShakeControllerImpl::commandManager() {
    return cmdManager_;
}

}  // namespace shake_example
