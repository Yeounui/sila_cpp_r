# CONFIG mode: vcpkg ships GTestConfig.cmake, and gtest_discover_tests comes
# from the GoogleTest module CMake itself provides.
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(test_sila
    # server
    sila/server/test_feature_registry.cc
    sila/server/test_sila_server_base.cc
    sila/server/test_sila_server_base_run_shutdown_e2e.cc
    sila/server/test_observable_command.cc
    sila/server/test_observable_command_concurrent.cc
    sila/server/test_multi_client_concurrent_e2e.cc
    sila/server/test_stress_connections_e2e.cc
    sila/server/test_sila_service_impl.cc
    sila/server/test_observable_command_grpc_e2e.cc
    sila/server/test_contention_trio.cc
    sila/server/test_mtls_switch_e2e.cc
    sila/server/test_server_operational_e2e.cc
    sila/server/test_command_parameter_validator.cc
    sila/server/test_any_codec_core_link.cc
    sila/server/property/test_observable_property.cc
    sila/server/auth/test_auth_token_store.cc
    sila/server/auth/test_auth_token_store_concurrent.cc
    sila/server/auth/test_authorization_interceptor.cc
    sila/server/auth/test_auth_session_e2e.cc
    sila/server/binary/test_binary_parameter_interceptor.cc
    sila/server/binary/test_binary_store.cc
    sila/server/binary/test_file_spool_binary_store.cc
    sila/server/binary/test_binary_transfer_service.cc
    sila/server/binary/test_hybrid_binary_store.cc
    sila/server/config/test_tls_config.cc
    sila/server/config/test_server_config.cc
    sila/server/error/test_error_transmit_interceptor.cc
    sila/server/features/test_auth_features.cc
    sila/server/features/test_authorization_service_e2e.cc
    sila/server/features/test_lock_controller.cc
    sila/server/features/test_simulation_controller.cc
    sila/server/features/test_connection_configuration_service.cc
    sila/server/features/test_connection_configuration_persistence.cc
    sila/server/metadata/test_metadata_interceptor.cc
    sila/server/metadata/test_metadata_policy.cc
    sila/server/recovery/test_recoverable_error_gate.cc
    sila/server/recovery/test_error_gate_property_integration.cc
    sila/server/recovery/test_error_recovery_service_e2e.cc
    sila/server/transport/test_grpc_transport_sink_e2e.cc
    sila/server/transport/test_grpc_transport_auth_e2e.cc
    sila/server/transport/test_command_granular_auth_e2e.cc
    sila/server/transport/test_followup_uuid_ownership_e2e.cc
    sila/server/transport/test_grpc_dispatch_e2e.cc
    sila/server/transport/test_call_context.cc
    sila/server/transport/cloud/test_active_call_registry.cc
    sila/server/transport/cloud/test_cloud_router_dispatch.cc
    sila/server/transport/cloud/test_cloud_router_observable.cc
    sila/server/transport/cloud/test_cloud_router_binary.cc
    sila/server/transport/cloud/test_cloud_handler_registration.cc
    sila/server/transport/cloud/test_fqi_coverage_dual_transport_e2e.cc
    sila/server/transport/cloud/test_metadata_gate_dual_transport_e2e.cc
    sila/server/transport/cloud/test_lock_gate_dual_transport_e2e.cc
    sila/server/transport/cloud/test_observable_gc_cleanup.cc
    sila/server/transport/cloud/test_cloud_transport_e2e.cc
    sila/server/transport/cloud/test_stream_write_serializer.cc
    sila/server/transport/cloud/test_active_call_registry_removal.cc
    sila/server/transport/cloud/test_cloud_router_observable_property.cc
    sila/server/transport/cloud/test_cloud_router_fuzz_regression.cc
    sila/server/discovery/test_mdns_publisher.cc
    # client
    sila/client/test_cloud_client_listener.cc
    sila/client/test_cloud_client_listener_edge.cc
    sila/client/test_client_config_e2e.cc
    sila/client/binary/test_binary_retry.cc
    sila/client/binary/test_binary_downloader_e2e.cc
    sila/client/binary/test_binary_uploader_e2e.cc
    sila/client/discovery/test_mdns_browser.cc
    sila/client/discovery/test_mdns_parser_fuzz_regression.cc
    # common
    sila/common/error/test_sila_error.cc
    sila/common/error/test_sila_error_edges.cc
    sila/common/types/test_constraints.cc
    sila/common/types/test_basic_types.cc
    sila/common/util/test_periodic_gc.cc
    # integration (server ↔ client)
    sila/integration/test_metadata_injector.cc
    sila/integration/test_auth_session_client_e2e.cc
    sila/integration/test_auth_session_renewal_e2e.cc
    sila/integration/test_sila_client_base.cc
    sila/integration/test_sila_client_base_auth_e2e.cc
)
target_link_libraries(test_sila PRIVATE Sila2::core GTest::gtest_main OpenSSL::Crypto)
target_compile_definitions(test_sila PRIVATE SILA2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(test_sila)

add_executable(test_sila_dynamic
    sila/client/dynamic/test_dynamic_pipeline.cc
    sila/client/dynamic/test_fdl_namespace_and_identity.cc
    sila/client/dynamic/test_descriptor_builder.cc
    sila/client/dynamic/test_descriptor_builder_type_xml.cc
    sila/client/dynamic/test_fdl_parser_fuzz_regression.cc
    sila/client/dynamic/test_fdl_corpus_conformance.cc
    sila/client/dynamic/test_dynamic_call_e2e.cc
    sila/client/dynamic/test_dynamic_request_validation_e2e.cc
    sila/client/dynamic/test_observable_command_runner_e2e.cc
    sila/client/dynamic/test_constraint_checker.cc
    sila/client/dynamic/test_json_codec.cc
    sila/client/dynamic/test_json_codec_fuzz_regression.cc
    sila/client/dynamic/test_any_codec.cc
    sila/client/dynamic/test_value_validator.cc
    sila/client/dynamic/test_value_validator_external_resolvers.cc
    sila/client/test_execution_info_subscriber_e2e.cc
    sila/client/test_execution_store.cc
    sila/client/test_recoverable_error_subscriber_e2e.cc
    sila/client/test_command_execution_status_e2e.cc
    sila/client/test_server_registry.cc
    sila/client/dynamic/test_dynamic_call_silaservice_metadata.cc)
target_link_libraries(test_sila_dynamic PRIVATE Sila2::dynamic GTest::gtest_main)
target_compile_definitions(test_sila_dynamic PRIVATE SILA2_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(test_sila_dynamic)

add_subdirectory(interop)
add_subdirectory(examples)
add_subdirectory(fuzz)

# --- codegen / validation pytest suites (audit 1.1f, 3.3a) ------------------
# Not called anywhere else in this tree.
find_package(Python3 COMPONENTS Interpreter)

# codegen's deps (lxml, xsdata, Jinja2) and validation's (sila2) live in the
# uv-managed venv at src/codegen, not the system interpreter. Prefer that
# venv's python when `uv sync` has populated it; otherwise fall back to the
# system interpreter only if it can actually import pytest, so a fresh
# checkout without the venv still configures (just without these two tests)
# instead of registering a test that is guaranteed to fail.
set(_sila2_codegen_venv_python "${CMAKE_CURRENT_SOURCE_DIR}/../src/codegen/.venv/bin/python")
if(EXISTS "${_sila2_codegen_venv_python}")
    set(_sila2_pytest_candidate "${_sila2_codegen_venv_python}")
elseif(Python3_FOUND)
    set(_sila2_pytest_candidate "${Python3_EXECUTABLE}")
endif()

# The probe gates whichever interpreter was picked, the venv included: pytest
# sits in pyproject's dev/validation dependency-groups, never in
# [project].dependencies, so a `uv sync --no-dev` tree has a .venv/bin/python
# with no pytest in it.
if(_sila2_pytest_candidate)
    execute_process(
        COMMAND "${_sila2_pytest_candidate}" -c "import pytest"
        RESULT_VARIABLE _sila2_pytest_missing
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_sila2_pytest_missing EQUAL 0)
        set(_sila2_pytest_python "${_sila2_pytest_candidate}")
    endif()
endif()

if(_sila2_pytest_python)
    add_test(NAME codegen
        COMMAND "${_sila2_pytest_python}" -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/codegen")
    # test_driver_validation.py uses pytest.importorskip("sila2"), and
    # conftest.py's sila_server fixture pytest.skip()s when the
    # sila2_interop_server binary hasn't been built yet -- so this stays
    # green (skipped, not failed) on a tree without that target built.
    add_test(NAME validation
        COMMAND "${_sila2_pytest_python}" -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/validation")
    # Empty out the inherited PYTHONPATH. pytest autoloads setuptools
    # entry-point plugins from everything on sys.path, so a shell that sourced
    # ROS drags in launch_testing, whose import chain dies before collection.
    # Neither suite needs anything from outside its own interpreter.
    #
    # SILA2_INTEROP_SERVER: conftest.py's sila_server fixture falls back to a
    # hard-coded build/gcc path when this is unset, which is wrong under any
    # preset but default. $<TARGET_FILE:...> resolves the real per-preset
    # path, so validation targets whichever build directory this ctest run
    # is actually in. Harmless on codegen, which never reads it.
    set_tests_properties(codegen validation PROPERTIES ENVIRONMENT
        "PYTHONPATH=;SILA2_INTEROP_SERVER=$<TARGET_FILE:sila2_interop_server>")
else()
    message(STATUS "tests: skipping codegen/validation ctest registration (no Python interpreter with pytest found)")
endif()
