// SimulationControllerImpl.cc — SiLA2 core feature (architecture.md §3.10)
#include "SimulationControllerImpl.h"

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/server/features/SimulationControllerFdl.h>
#include <sila/server/transport/GrpcTransport.h>

#include "SiLAFramework.pb.h"

#include <string>

namespace sila2 {
namespace {
const std::string kFdlXml = generated::kSimulationControllerFdlXml;

// The FDL's <DefinedExecutionErrors> section declares these two errors for
// StartSimulationMode/StartRealMode when the caller-supplied canSwitch_
// callback vetoes the switch.
const std::string kStartSimulationModeFailedErrorId =
    "org.silastandard/core/SimulationController/v1/DefinedExecutionError/StartSimulationModeFailed";
const std::string kStartRealModeFailedErrorId =
    "org.silastandard/core/SimulationController/v1/DefinedExecutionError/StartRealModeFailed";

}  // namespace

const std::string& simulationControllerFdlXml() { return kFdlXml; }

SimulationControllerImpl::SimulationControllerImpl(const InterceptorChain* chain) : chain_{chain} {}

void SimulationControllerImpl::setCanSwitchCallback(std::function<bool(bool)> cb) {
    canSwitch_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

grpc::Status SimulationControllerImpl::StartSimulationMode(
    grpc::ServerContext* context,
    const simctrl_proto::StartSimulationMode_Parameters* request,
    simctrl_proto::StartSimulationMode_Responses* response) {
    GrpcUnaryResponseSink<simctrl_proto::StartSimulationMode_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { startSimulationMode(req, ctx, out); },
        chain_, kStartSimulationModeFqi, response);
    return sink.status();
}

void SimulationControllerImpl::startSimulationMode(
    const simctrl_proto::StartSimulationMode_Parameters&, CallContext&,
    ResponseSink<simctrl_proto::StartSimulationMode_Responses>& sink) {
    // Owning server may veto the switch, e.g. while hardware is mid-operation.
    if (canSwitch_ && !canSwitch_(true)) {
        throw error::DefinedExecutionError{
            kStartSimulationModeFailedErrorId,
            "Cannot switch to Simulation Mode"};
    }
    simulationMode_ = true;
    sink.send(simctrl_proto::StartSimulationMode_Responses{});
    sink.finish();
}

grpc::Status SimulationControllerImpl::StartRealMode(
    grpc::ServerContext* context,
    const simctrl_proto::StartRealMode_Parameters* request,
    simctrl_proto::StartRealMode_Responses* response) {
    GrpcUnaryResponseSink<simctrl_proto::StartRealMode_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { startRealMode(req, ctx, out); },
        chain_, kStartRealModeFqi, response);
    return sink.status();
}

void SimulationControllerImpl::startRealMode(
    const simctrl_proto::StartRealMode_Parameters&, CallContext&,
    ResponseSink<simctrl_proto::StartRealMode_Responses>& sink) {
    if (canSwitch_ && !canSwitch_(false)) {
        throw error::DefinedExecutionError{
            kStartRealModeFailedErrorId,
            "Cannot switch to Real Mode"};
    }
    simulationMode_ = false;
    sink.send(simctrl_proto::StartRealMode_Responses{});
    sink.finish();
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

grpc::Status SimulationControllerImpl::Get_SimulationMode(
    grpc::ServerContext* context,
    const simctrl_proto::Get_SimulationMode_Parameters* request,
    simctrl_proto::Get_SimulationMode_Responses* response) {
    GrpcUnaryResponseSink<simctrl_proto::Get_SimulationMode_Responses> sink(response);
    dispatchToHandler(context, *request, sink,
        [this](const auto& req, auto& ctx, auto& out) { getSimulationMode(req, ctx, out); },
        chain_, kGet_SimulationModeFqi, response);
    return sink.status();
}

void SimulationControllerImpl::getSimulationMode(
    const simctrl_proto::Get_SimulationMode_Parameters&, CallContext&,
    ResponseSink<simctrl_proto::Get_SimulationMode_Responses>& sink) {
    simctrl_proto::Get_SimulationMode_Responses response;
    response.mutable_simulationmode()->set_value(simulationMode_.load());
    sink.send(response);
    sink.finish();
}

}  // namespace sila2
