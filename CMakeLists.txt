cmake_minimum_required(VERSION 3.25)
project(sila2_cpp_foundation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# OpenSSL::Crypto only: TlsConfig uses libcrypto (X509, EVP, PEM) and none of
# libssl's TLS handshake API. gRPC takes the PEM strings as plain text, so the
# certificate path never touches an SSL_CTX. Found here rather than in
# src/sila so the tests, which reach for X509 directly, share the one lookup.
find_package(OpenSSL REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_subdirectory(src/sila)

# CTest defines BUILD_TESTING (default ON) and adds the `test` target. When
# this repository is pulled in with add_subdirectory by a consumer (§1.1),
# the consumer's own BUILD_TESTING=OFF then keeps our tests out of its build.
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
