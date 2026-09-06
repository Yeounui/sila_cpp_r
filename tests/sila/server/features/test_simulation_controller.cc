// Tests for SimulationControllerImpl: mode switch lifecycle (Real ↔ Simulation),
// canSwitch callback veto, and default state.
#include <sila/server/features/SimulationControllerImpl.h>

#include <sila/common/error/SiLAErrorException.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

#include "SimulationController.grpc.pb.h"
#include "SiLAFramework.pb.h"

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

namespace
{
using sila2::SimulationControllerImpl;
using sila2::error::DefinedExecutionError;
using sila2::error::fromGrpcStatus;
using sila2::error::SiLAError;

namespace simctrl_proto = sila2::org::silastandard::core::simulationcontroller::v1;

bool getSimulationMode(SimulationControllerImpl& ctrl) {
    grpc::ServerContext ctx;
    simctrl_proto::Get_SimulationMode_Parameters request;
    simctrl_proto::Get_SimulationMode_Responses response;
    ctrl.Get_SimulationMode(&ctx, &request, &response);
    return response.simulationmode().value();
}

grpc::Status startSimulation(SimulationControllerImpl& ctrl) {
    grpc::ServerContext ctx;
    simctrl_proto::StartSimulationMode_Parameters request;
    simctrl_proto::StartSimulationMode_Responses response;
    return ctrl.StartSimulationMode(&ctx, &request, &response);
}

grpc::Status startReal(SimulationControllerImpl& ctrl) {
    grpc::ServerContext ctx;
    simctrl_proto::StartRealMode_Parameters request;
    simctrl_proto::StartRealMode_Responses response;
    return ctrl.StartRealMode(&ctx, &request, &response);
}

// ---------------------------------------------------------------------------
// Mode switch — True paths
// ---------------------------------------------------------------------------

TEST(SimulationController, DefaultStartsInRealMode) {
    SimulationControllerImpl ctrl;
    EXPECT_FALSE(getSimulationMode(ctrl));
}

TEST(SimulationController, StartSimulationModeSetsTrue) {
    SimulationControllerImpl ctrl;

    ASSERT_TRUE(startSimulation(ctrl).ok());
    EXPECT_TRUE(getSimulationMode(ctrl));
}

TEST(SimulationController, StartRealModeSetsFalse) {
    SimulationControllerImpl ctrl;

    ASSERT_TRUE(startSimulation(ctrl).ok());
    EXPECT_TRUE(getSimulationMode(ctrl));

    ASSERT_TRUE(startReal(ctrl).ok());
    EXPECT_FALSE(getSimulationMode(ctrl));
}

TEST(SimulationController, NoCallbackMeansSwitchesAlwaysSucceed) {
    SimulationControllerImpl ctrl;

    EXPECT_TRUE(startSimulation(ctrl).ok());
    EXPECT_TRUE(startReal(ctrl).ok());
    EXPECT_TRUE(startSimulation(ctrl).ok());
}

// ---------------------------------------------------------------------------
// canSwitch veto — False paths
// ---------------------------------------------------------------------------

TEST(SimulationController, CanSwitchVetoBlocksSimulationMode) {
    SimulationControllerImpl ctrl;
    ctrl.setCanSwitchCallback([](bool toSimulation) { return !toSimulation; });

    const grpc::Status status = startSimulation(ctrl);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(),
              "org.silastandard/core/SimulationController/v1/DefinedExecutionError/StartSimulationModeFailed");
    EXPECT_FALSE(getSimulationMode(ctrl));
}

TEST(SimulationController, CanSwitchVetoBlocksRealMode) {
    SimulationControllerImpl ctrl;
    ASSERT_TRUE(startSimulation(ctrl).ok());

    ctrl.setCanSwitchCallback([](bool toSimulation) { return toSimulation; });

    const grpc::Status status = startReal(ctrl);

    ASSERT_FALSE(status.ok());
    const auto reconstructed = fromGrpcStatus(status);
    ASSERT_NE(reconstructed, nullptr);
    ASSERT_EQ(reconstructed->errorType(), SiLAError::ErrorType::DefinedExecutionError);
    const auto* err = dynamic_cast<const DefinedExecutionError*>(reconstructed.get());
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->errorIdentifier(),
              "org.silastandard/core/SimulationController/v1/DefinedExecutionError/StartRealModeFailed");
    EXPECT_TRUE(getSimulationMode(ctrl));
}

TEST(SimulationController, VetoedSwitchLeavsModeUnchanged) {
    SimulationControllerImpl ctrl;
    ctrl.setCanSwitchCallback([](bool /*toSimulation*/) { return false; });

    EXPECT_FALSE(startSimulation(ctrl).ok());
    EXPECT_FALSE(getSimulationMode(ctrl));

    // Temporarily allow simulation, then block real
    ctrl.setCanSwitchCallback([](bool toSimulation) { return toSimulation; });
    ASSERT_TRUE(startSimulation(ctrl).ok());
    EXPECT_TRUE(getSimulationMode(ctrl));

    EXPECT_FALSE(startReal(ctrl).ok());
    EXPECT_TRUE(getSimulationMode(ctrl));
}

}  // namespace
