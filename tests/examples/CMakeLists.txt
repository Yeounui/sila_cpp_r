include(${CMAKE_SOURCE_DIR}/cmake/SilaCodegen.cmake)

set(_shake_fdl
    ${CMAKE_SOURCE_DIR}/tests/examples/fdl/teleshake/ShakeController.sila.xml)

sila_generate_feature(
    TARGET     shake_example
    FDL_FILES  ${_shake_fdl}
)

add_executable(shake_controller_example
    shake_controller/ShakeControllerImpl.cc
    shake_controller/main.cc
    ${shake_example_GENERATED_SRCS}
)

target_include_directories(shake_controller_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shake_controller
    ${shake_example_INCLUDE_DIR}
    ${shake_example_META_INCLUDE_DIR}
)
target_link_libraries(shake_controller_example PRIVATE sila_cpp_r::core)

add_executable(test_shake_controller_e2e
    test_shake_controller_e2e.cc
    shake_controller/ShakeControllerImpl.cc
    ${shake_example_GENERATED_SRCS}
)
target_include_directories(test_shake_controller_e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shake_controller
    ${shake_example_INCLUDE_DIR}
    ${shake_example_META_INCLUDE_DIR}
)
target_link_libraries(test_shake_controller_e2e PRIVATE sila_cpp_r::core GTest::gtest_main)
gtest_discover_tests(test_shake_controller_e2e)

set(_cloud_fixture_fdl_dir ${CMAKE_SOURCE_DIR}/tests/examples/fdl/sila_base/valid-fdl)

# Second generation call, with its own OUTPUT_DIR so the two feature sets never
# share a custom-command output tree. Proto packages are disjoint
# (examples.shakecontroller.v1 / tests.observablebasic.v1 /
# tests.observablecommandnointermediateresponse.v1 / test.binarytransfertest.v1),
# so linking all four into one binary cannot collide in the descriptor pool.
# BinaryTransferTest-v1_0 added for S71: it has a Command with a Binary
# parameter, needed to drive the cloud path's Binary Transfer UUID resolve.
sila_generate_feature(
    TARGET     cloud_codegen_fixture
    FDL_FILES  ${_cloud_fixture_fdl_dir}/ObservableBasic.sila.xml
               ${_cloud_fixture_fdl_dir}/ObservableCommandNoIntermediateResponse.sila.xml
               ${CMAKE_SOURCE_DIR}/third_party/sila_base/feature_definitions/org/silastandard/test/BinaryTransferTest-v1_0.sila.xml
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated_cloud_fixture
)

add_executable(test_codegen_cloud_e2e
    test_codegen_cloud_e2e.cc
    shake_controller/ShakeControllerImpl.cc
    ${shake_example_GENERATED_SRCS}
    ${cloud_codegen_fixture_GENERATED_SRCS}
)
target_include_directories(test_codegen_cloud_e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shake_controller
    ${shake_example_INCLUDE_DIR}
    ${shake_example_META_INCLUDE_DIR}
    ${cloud_codegen_fixture_INCLUDE_DIR}
    ${cloud_codegen_fixture_META_INCLUDE_DIR}
    # CloudRouterTestHarness.h -- the same in-process CapturingEndpoint fixture
    # the cloud router tests use, reused rather than re-declared here.
    ${CMAKE_SOURCE_DIR}/tests/sila/server/transport/cloud
)
target_link_libraries(test_codegen_cloud_e2e PRIVATE sila_cpp_r::core GTest::gtest_main)
gtest_discover_tests(test_codegen_cloud_e2e)

if(SILA_ENABLE_OTEL)
    find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS sdk exporters_in_memory)
    target_sources(test_codegen_cloud_e2e PRIVATE test_otel_span_smoke.cc)
    target_link_libraries(test_codegen_cloud_e2e PRIVATE
        opentelemetry-cpp::sdk opentelemetry-cpp::in_memory_span_exporter)
endif()
