// SimulationControllerImpl.h — SiLA2 core feature (architecture.md §3.10)
//
// New component, not a port. SimulationController lets a client switch a
// SiLA Server between Simulation Mode and Real Mode. This implementation has
// no FeatureRegistry dependency: the mode flag is self-contained state.
#pragma once

// Generated proto header provides the service base class and all message types.
// protoc output goes to CMAKE_CURRENT_BINARY_DIR which is on the include path.
#include "SimulationController.grpc.pb.h"

#include <sila/server/transport/SilaHandler.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>

namespace sila2 {

struct InterceptorChain;

// FQI constant for SimulationController — used by Builder::Build() to auto-register.
inline constexpr std::string_view kSimulationControllerFqi =
    "org.silastandard/core/SimulationController/v1";

// Per-RPC granular authorization FQIs. The direct-gRPC auth gate matches a
// call's FQI against protectedFqis; passing these Command/Property-level FQIs
// (not the feature-level one above) lets a command-level protectedFqis entry
// actually gate the matching RPC, keeping direct-gRPC coverage aligned with
// the cloud path. Full-literal string_view, not runtime concatenation, to stay
// constexpr and match kAuthorizationProviderParamFqi's existing style.
inline constexpr std::string_view kStartSimulationModeFqi =
    "org.silastandard/core/SimulationController/v1/Command/StartSimulationMode";
inline constexpr std::string_view kStartRealModeFqi =
    "org.silastandard/core/SimulationController/v1/Command/StartRealMode";
inline constexpr std::string_view kGet_SimulationModeFqi =
    "org.silastandard/core/SimulationController/v1/Property/SimulationMode";

// Returns the FDL XML for SimulationController, embedded as a string constant.
// ponytail: codegen will generate SimulationControllerMeta.cc with this constant (§2);
// until then, a raw string literal in the .cc file serves the same purpose.
const std::string& simulationControllerFdlXml();

// Namespace alias shortens the generated proto namespace for readability.
namespace simctrl_proto = sila2::org::silastandard::core::simulationcontroller::v1;

/// Implements the @ref gl_feature "Feature" `org.silastandard/core/SimulationController/v1`,
/// letting a @ref gl_sila_client "SiLA Client" switch the server between
/// Simulation Mode and Real Mode.
///
/// Not installed by any `SiLAServerBase::Builder::WithX()` call; a server
/// author registers it like a custom Feature, via
/// `Builder::AddFeature(std::string{kSimulationControllerFqi}, simulationControllerFdlXml(),
/// impl.service())`.
class SimulationControllerImpl final : public simctrl_proto::SimulationController::Service {
public:
    // No registry dependency — mode state is entirely local to this object.
    explicit SimulationControllerImpl(const InterceptorChain* chain = nullptr);

    /// Lets the owning server veto a mode switch, e.g. while hardware is
    /// mid-operation: `cb` receives `true` for a switch to Simulation Mode,
    /// `false` for Real Mode, and returning `false` fails the command with a
    /// SimulationController Defined Execution Error. Unset, switches always
    /// succeed.
    void setCanSwitchCallback(std::function<bool(bool)> cb);

    // ---- Commands ----

    /// Serves the StartSimulationMode command.
    /// @throws error::DefinedExecutionError{StartSimulationModeFailed} if the
    ///         canSwitchCallback vetoes the switch.
    grpc::Status StartSimulationMode(
        grpc::ServerContext* context,
        const simctrl_proto::StartSimulationMode_Parameters* request,
        simctrl_proto::StartSimulationMode_Responses* response) override;

    /// Serves the StartRealMode command.
    /// @throws error::DefinedExecutionError{StartRealModeFailed} if the
    ///         canSwitchCallback vetoes the switch.
    grpc::Status StartRealMode(
        grpc::ServerContext* context,
        const simctrl_proto::StartRealMode_Parameters* request,
        simctrl_proto::StartRealMode_Responses* response) override;

    // ---- Properties ----

    /// Serves the SimulationMode property: `true` while the server is in
    /// Simulation Mode.
    grpc::Status Get_SimulationMode(
        grpc::ServerContext* context,
        const simctrl_proto::Get_SimulationMode_Parameters* request,
        simctrl_proto::Get_SimulationMode_Responses* response) override;

    void startSimulationMode(const simctrl_proto::StartSimulationMode_Parameters& request,
                             CallContext& ctx,
                             ResponseSink<simctrl_proto::StartSimulationMode_Responses>& sink);
    void startRealMode(const simctrl_proto::StartRealMode_Parameters& request,
                       CallContext& ctx,
                       ResponseSink<simctrl_proto::StartRealMode_Responses>& sink);
    void getSimulationMode(const simctrl_proto::Get_SimulationMode_Parameters& request,
                           CallContext& ctx,
                           ResponseSink<simctrl_proto::Get_SimulationMode_Responses>& sink);

private:
    // atomic, not mutex-guarded — a single bool flag read/written across
    // threads needs no critical section beyond what atomic<bool> gives.
    std::atomic<bool> simulationMode_{false};
    std::function<bool(bool toSimulation)> canSwitch_;
    const InterceptorChain* chain_;
};

}  // namespace sila2
