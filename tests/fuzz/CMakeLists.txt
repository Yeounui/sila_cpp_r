# libFuzzer harnesses (audit 1.1g+1.3g, 4.1l). libFuzzer (-fsanitize=fuzzer)
# is a clang/LLVM feature that GCC does not implement, and the 'default'
# preset builds with GCC -- skip these targets there instead of failing
# configuration, since the goal is that the harnesses compile under the
# 'fuzz' preset, not that every preset builds them.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "tests/fuzz: skipping libFuzzer harnesses (requires clang, found ${CMAKE_CXX_COMPILER_ID})")
    return()
endif()

# A libFuzzer target is only worth building where the library under test is
# instrumented too. With sila2_core/sila2_dynamic compiled as plain archives,
# libFuzzer sees only this harness TU's own edges: coverage never grows and
# the mutator degenerates to blind random input (audit 4.1l). The 'fuzz'
# preset puts -fsanitize=fuzzer-no-link into CMAKE_CXX_FLAGS, which is
# SanitizerCoverage instrumentation without libFuzzer's main() -- so every
# TU in that build directory, library included, feeds the mutator, while
# only the harnesses below add -fsanitize=fuzzer to link main() in.
# Skipping rather than warning: an uninstrumented harness that still builds
# is exactly the state 4.1l describes, and it should not be reachable.
string(FIND "${CMAKE_CXX_FLAGS}" "-fsanitize=fuzzer-no-link" _sila2_sancov_position)
if(_sila2_sancov_position EQUAL -1)
    message(STATUS "tests/fuzz: skipping libFuzzer harnesses (library not instrumented); use `cmake --preset fuzz`")
    return()
endif()

set(_sila2_fuzz_harnesses
    fuzz_cloud_router
    fuzz_fdl_parser
    fuzz_json_codec
    fuzz_mdns_parser
)

foreach(_harness ${_sila2_fuzz_harnesses})
    add_executable(${_harness} ${_harness}.cc)
    # Sila2::dynamic pulls in Sila2::core (PUBLIC), which covers all four
    # harnesses' needs: the cloud .pb.h types and mdns.h from core, plus
    # JsonCodec.h/FdlRuntimeParser.h from dynamic itself.
    target_link_libraries(${_harness} PRIVATE Sila2::dynamic)
    target_compile_options(${_harness} PRIVATE -fsanitize=fuzzer,address)
    target_link_options(${_harness} PRIVATE -fsanitize=fuzzer,address)
    # No add_test: these are exploratory fuzzers with no fixed pass/fail
    # exit, not a bounded ctest check. The point here is that they compile
    # in CI-like runs; fuzzing itself stays a manual/long-running exercise.
endforeach()
