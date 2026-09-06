// Typed DynamicCall validation reaches transport only after FDL validation.
#include <sila/client/dynamic/DynamicCall.h>
#include <sila/client/dynamic/FeatureCatalog.h>
#include <sila/client/dynamic/ObservableCommandRunner.h>

#include "GenericDynamicTestServer.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace {
using sila2::dynamic::ConstraintResolver;
using sila2::dynamic::FeatureCatalog;
using sila2::dynamic::callUnary;
using sila2::dynamic::executeObservableCommand;
using sila2::test::GenericDynamicTestServer;
using sila2::test::ScriptedResponse;

constexpr char kShakeFqi[] = "org.silastandard/examples/ShakeController/v1";
constexpr char kFlowFqi[] = "de.cetoni/pumps.contiflowpumps/ContinuousFlowConfigurationService/v1";

std::string readFdl(const char* relativePath) {
    std::ifstream file{std::string{SILA2_SOURCE_ROOT} + "/" + relativePath};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::unique_ptr<google::protobuf::Message> request(FeatureCatalog& catalog,
                                                    const std::string& fqi,
                                                    const std::string& rpc) {
    const auto* descriptor = catalog.requestDescriptor(fqi, rpc);
    return std::unique_ptr<google::protobuf::Message>{
        catalog.messageFactory().GetPrototype(descriptor)->New()};
}

void setNumber(google::protobuf::Message& request, const std::string& name, double value) {
    const auto* field = request.GetDescriptor()->FindFieldByName(name);
    auto* wrapped = request.GetReflection()->MutableMessage(&request, field);
    const auto* valueField = wrapped->GetDescriptor()->FindFieldByName("value");
    wrapped->GetReflection()->SetDouble(wrapped, valueField, value);
}

void setInteger(google::protobuf::Message& request, const std::string& name, std::int64_t value) {
    const auto* field = request.GetDescriptor()->FindFieldByName(name);
    auto* wrapped = request.GetReflection()->MutableMessage(&request, field);
    const auto* valueField = wrapped->GetDescriptor()->FindFieldByName("value");
    wrapped->GetReflection()->SetInt64(wrapped, valueField, value);
}

void setString(google::protobuf::Message& request, const std::string& name, const std::string& value) {
    const auto* field = request.GetDescriptor()->FindFieldByName(name);
    auto* wrapped = request.GetReflection()->MutableMessage(&request, field);
    const auto* valueField = wrapped->GetDescriptor()->FindFieldByName("value");
    wrapped->GetReflection()->SetString(wrapped, valueField, value);
}

TEST(DynamicRequestValidation, FdlConstraintsRejectBeforeTransport) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    catalog.add(kShakeFqi, readFdl("tests/examples/fdl/teleshake/ShakeController.sila.xml"));
    catalog.add(kFlowFqi, readFdl("tests/examples/fdl/cetoni/ContinuousFlowConfigurationService.sila.xml"));
    GenericDynamicTestServer server;
    std::atomic<int> calls{0};
    const auto handler = [&](const grpc::ByteBuffer&) {
        ++calls;
        return ScriptedResponse{{}, grpc::Status::OK};
    };
    server.on(catalog.grpcMethodName(kShakeFqi, "StartShaking"), handler);
    server.on(catalog.grpcMethodName(kFlowFqi, "SetSwitchingMode"), handler);

    const ConstraintResolver acceptExternal =
        [](const sila2::dynamic::ConstraintValue&, const google::protobuf::Message&,
           const google::protobuf::FieldDescriptor&, int) { return std::nullopt; };
    grpc::ByteBuffer response;

    auto valid = request(catalog, kShakeFqi, "StartShaking");
    setNumber(*valid, "TargetSpeed", 5000);
    setNumber(*valid, "TargetPower", 50);
    EXPECT_TRUE(callUnary(server.channel(), catalog, kShakeFqi, "StartShaking", *valid,
                          &response, acceptExternal).ok());
    EXPECT_EQ(calls.load(), 1);

    auto missingExternalResolver = request(catalog, kShakeFqi, "StartShaking");
    setNumber(*missingExternalResolver, "TargetSpeed", 5000);
    setNumber(*missingExternalResolver, "TargetPower", 50);
    EXPECT_EQ(callUnary(server.channel(), catalog, kShakeFqi, "StartShaking",
                        *missingExternalResolver, &response).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);

    auto lowSpeed = request(catalog, kShakeFqi, "StartShaking");
    setNumber(*lowSpeed, "TargetSpeed", 4005);
    setNumber(*lowSpeed, "TargetPower", 50);
    EXPECT_EQ(callUnary(server.channel(), catalog, kShakeFqi, "StartShaking", *lowSpeed,
                        &response, acceptExternal).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    auto highPower = request(catalog, kShakeFqi, "StartShaking");
    setNumber(*highPower, "TargetSpeed", 5000);
    setNumber(*highPower, "TargetPower", 100);
    EXPECT_EQ(callUnary(server.channel(), catalog, kShakeFqi, "StartShaking", *highPower,
                        &response, acceptExternal).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    auto invalidSet = request(catalog, kFlowFqi, "SetSwitchingMode");
    setString(*invalidSet, "SwitchingMode", "WrongMode");
    EXPECT_EQ(callUnary(server.channel(), catalog, kFlowFqi, "SetSwitchingMode", *invalidSet,
                        &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(calls.load(), 1);
}

TEST(DynamicRequestValidation, ObservableRunnerRejectsRealFdlRequestBeforeTransport) {
    FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
    catalog.add(kShakeFqi, readFdl("tests/examples/fdl/teleshake/ShakeController.sila.xml"));
    GenericDynamicTestServer server;
    std::atomic<int> calls{0};
    server.on(catalog.grpcMethodName(kShakeFqi, "ShakeForTime"), [&](const grpc::ByteBuffer&) {
        ++calls;
        return ScriptedResponse{{}, grpc::Status::OK};
    });
    auto invalid = request(catalog, kShakeFqi, "ShakeForTime");
    setInteger(*invalid, "Runtime", 10);
    setNumber(*invalid, "TargetSpeed", 4005);
    setNumber(*invalid, "TargetPower", 50);
    const ConstraintResolver acceptExternal =
        [](const sila2::dynamic::ConstraintValue&, const google::protobuf::Message&,
           const google::protobuf::FieldDescriptor&, int) { return std::nullopt; };

    const auto result = executeObservableCommand(
        server.channel(), catalog, kShakeFqi, "ShakeForTime", *invalid, {}, acceptExternal);
    EXPECT_EQ(result.status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(calls.load(), 0);
}

}  // namespace
