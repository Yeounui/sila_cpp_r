include(${CMAKE_SOURCE_DIR}/cmake/SilaCodegen.cmake)

set(_authentication_test_fdl
    ${CMAKE_SOURCE_DIR}/third_party/sila_base/feature_definitions/org/silastandard/test/AuthenticationTest-v1_0.sila.xml)
set(_binary_transfer_test_fdl
    ${CMAKE_SOURCE_DIR}/third_party/sila_base/feature_definitions/org/silastandard/test/BinaryTransferTest-v1_0.sila.xml)

# --- FDL -> proto -> C++ codegen --------------------------------------------

sila_generate_feature(
    TARGET     interop_server
    FDL_FILES  ${_authentication_test_fdl} ${_binary_transfer_test_fdl}
)

# --- sila2_interop_server executable ----------------------------------------

add_executable(sila2_interop_server
    server_main.cc
    ${interop_server_GENERATED_SRCS}
)
target_include_directories(sila2_interop_server PRIVATE
    ${interop_server_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(sila2_interop_server PRIVATE sila2::core)

# --- integration test client (§7) -------------------------------------------

add_executable(test_interop
    test_interop.cc
    ${interop_server_GENERATED_SRCS}
)
target_include_directories(test_interop PRIVATE
    ${interop_server_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(test_interop PRIVATE sila2::dynamic GTest::gtest_main)
# Register as a single ctest entry so all TEST() cases share one process —
# the static server() singleton starts once and every test acts as a separate
# client, matching the real single-server / multi-client deployment model.
# gtest_discover_tests would fork a process per case, causing port conflicts
# under ctest -j.
add_test(NAME interop COMMAND test_interop)
