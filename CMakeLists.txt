cmake_minimum_required(VERSION 3.25)
project(sila_cpp_r VERSION 0.81.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# OpenSSL::Crypto only: TlsConfig uses libcrypto (X509, EVP, PEM) and none of
# libssl's TLS handshake API. gRPC takes the PEM strings as plain text, so the
# certificate path never touches an SSL_CTX. Found here rather than in
# src/sila so the tests, which reach for X509 directly, share the one lookup.
#
# The provider is BoringSSL, which vcpkg installs under the OpenSSL name. Pin
# the root so FindOpenSSL cannot prefer a system OpenSSL 3, where OPENSSL_free,
# BIO_pending and EVP_MD_CTX_create are macros rather than the real symbols
# libgrpc.a was compiled against -- that mismatch links but leaves them undefined.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(OPENSSL_ROOT_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
find_package(OpenSSL REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(LibXml2 CONFIG REQUIRED)
find_package(LibXslt CONFIG REQUIRED)
# JsonSchemaSupport.cc (src/sila) is the sole consumer; found here to mirror
# LibXml2/LibXslt, which are likewise core-private-linked yet looked up
# top-level (R10-9g2, D2=a).
find_package(nlohmann_json_schema_validator CONFIG REQUIRED)

add_subdirectory(src/sila)

# CTest's own module auto-detects MEMORYCHECK_COMMAND by probing for several
# tools (valgrind, drmemory, cuda-memcheck, compute-sanitizer, ...). This
# machine has no valgrind, so it silently latches onto NVIDIA's
# compute-sanitizer (a CUDA GPU checker -- this project has no CUDA code) and
# `ctest -T memcheck` then "succeeds" while checking nothing (audit 4.1c).
# The `asan` preset covers the same ground and better, so pin the variable to
# a path that does not exist -- `ctest -T memcheck` then fails loudly.
# Two things this pin depends on, both learned the hard way:
#   - it must run BEFORE include(CTest): that module writes MEMORYCHECK_COMMAND
#     into DartConfiguration.tcl as it runs, and that file (not the cache) is
#     what `ctest -T memcheck` reads. Setting it afterwards changes nothing.
#   - the value must not end in `-NOTFOUND`: that is exactly the state in which
#     find_program re-runs its probe, so the sentinel would undo itself on
#     every reconfigure.
# Use `cmake --preset asan` + `ctest --test-dir build/asan` for memory checking.
set(MEMORYCHECK_COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/.no-memcheck-here" CACHE FILEPATH
    "Unusable here (no valgrind); use the 'asan' preset instead" FORCE)

# CTest defines BUILD_TESTING (default ON) and adds the `test` target. When
# this repository is pulled in with add_subdirectory by a consumer (§1.1),
# the consumer's own BUILD_TESTING=OFF then keeps our tests out of its build.
include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()


# --- install / export (§1.1) ------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS sila2_core sila2_dynamic
    EXPORT Sila2Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Source headers (sila/**/*.h)
install(DIRECTORY src/sila/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sila
    FILES_MATCHING PATTERN "*.h"
)

# Proto generated headers (*.pb.h, *.grpc.pb.h)
install(DIRECTORY ${CMAKE_BINARY_DIR}/src/sila/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.pb.h"
)

# FDL-embedded generated headers
install(DIRECTORY ${CMAKE_BINARY_DIR}/src/sila/generated/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT Sila2Targets
    NAMESPACE Sila2::
    FILE Sila2Targets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sila2
)

if(SILA_ENABLE_OTEL)
    set(SILA_OTEL_FIND_DEPENDENCY "find_dependency(opentelemetry-cpp CONFIG)")
else()
    set(SILA_OTEL_FIND_DEPENDENCY "")
endif()

configure_package_config_file(
    cmake/Sila2Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/Sila2Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sila2
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/Sila2ConfigVersion.cmake
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/Sila2Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/Sila2ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sila2
)


# --- API docs (Doxygen) -----------------------------------------------------
find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    set(DOXYGEN_JAVADOC_AUTOBRIEF YES)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
    set(DOXYGEN_QUIET YES)
    set(DOXYGEN_EXCLUDE_SYMBOLS "*::detail" "*::detail::*")
    set(DOXYGEN_FILE_PATTERNS "*.h" "*.md")
    set(DOXYGEN_STRIP_FROM_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(DOXYGEN_GENERATE_TREEVIEW YES)
    doxygen_add_docs(docs src/sila docs README.md reference/docs/README_ko.md COMMENT "Generating API documentation")
endif()
