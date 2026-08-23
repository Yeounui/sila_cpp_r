# CONFIG mode: vcpkg ships GTestConfig.cmake, and gtest_discover_tests comes
# from the GoogleTest module CMake itself provides.
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(test_sila sila/test_feature_registry.cc sila/test_sila_server_base.cc sila/test_tls_config.cc)
target_link_libraries(test_sila PRIVATE sila2::core GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(test_sila)
