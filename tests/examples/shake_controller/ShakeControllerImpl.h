// ShakeControllerImpl.h — example Feature implementation (architecture.md §8).
//
// This is teaching material: it shows how a Feature owner wires the
// codegen-generated ShakeControllerServiceAdapter (RPC plumbing, defined by
// the FDL) together with a hand-written domain implementation. The pattern
// generalizes to any Feature:
//   1. codegen produces a ServiceAdapter with one SilaHandler<Req, Resp>
//      member per command/property RPC.
//   2. the implementation class owns the adapter, registers its own member
//      functions as handlers in the constructor, and exposes the adapter +
//      an ObservableCommandManager for Builder to wire into the server.
#pragma once

#include "ShakeControllerServiceAdapter.h"
#include "ShakeControllerMeta.h"

#include <sila/server/command/ObservableCommandManager.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace shake_example {

namespace shake_proto = sila2::org::silastandard::examples::shakecontroller::v1;
namespace fw = sila2::org::silastandard;
namespace gen = sila2::generated::shakecontroller;

/// Per-execution state for the observable ShakeForTime command.
///
/// ShakeForTime runs on its own worker thread (the actual "shake for N
/// seconds" loop) while the ShakeForTimeIntermediate RPC handler is called
/// concurrently, from a different gRPC thread, to report progress back to
/// the client. ShakeState is the piece of shared state both sides touch:
/// the worker updates timeLeft/done under mu_ and notifies cv_, and the
/// _Intermediate handler reads timeLeft/done under the same mutex (and can
/// wait on cv_ for the next update). Same producer/consumer shape as the
/// interop test's observable-command fixtures.
struct ShakeState {
    int totalSeconds;
    double timeLeft;
    bool done = false;
    std::mutex mu;
    std::condition_variable cv;
};

/// Encapsulates ShakeController domain logic (the actual shaking, homing,
/// locking behavior) and wires it to the generated ServiceAdapter by
/// registering member functions as SilaHandler callbacks.
///
/// Usage with Builder:
///   ShakeControllerImpl impl(builder.chain());
///   builder.addFeature(std::string{gen::kFqi}, std::string{gen::kFdlXml}, impl.service())
///          .registerCommandManager(&impl.commandManager());
class ShakeControllerImpl {
public:
    /// @param chain Interceptor chain shared by the server (auth, logging,
    ///               metadata handling); forwarded to the generated adapter.
    explicit ShakeControllerImpl(const sila2::InterceptorChain* chain);

    /// Returns the configured adapter, ready for Builder::addFeature to
    /// register as the gRPC service for this Feature.
    std::shared_ptr<grpc::Service> service() const;

    /// Returns the manager tracking in-flight observable command executions
    /// (ShakeForTime), for Builder::registerCommandManager.
    sila2::ObservableCommandManager& commandManager();

private:
    std::shared_ptr<gen::ShakeControllerServiceAdapter> adapter_;
    sila2::ObservableCommandManager cmdManager_;

    // Guards against overlapping shake/home/lock operations on the
    // (simulated) hardware; set for the duration of an active shake.
    std::atomic<bool> shaking_{false};

    // ShakeState is created per ShakeForTime execution and looked up by
    // execution UUID from the _Intermediate handler, so it must outlive the
    // call that created it — hence keyed storage instead of a stack local.
    std::mutex statesMu_;
    std::unordered_map<std::string, std::shared_ptr<ShakeState>> states_;
};

}  // namespace shake_example
