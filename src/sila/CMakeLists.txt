add_library(sila2_core
    server/FeatureRegistry.cc
    server/TlsConfig.cc
)
add_library(sila2::core ALIAS sila2_core)

# PUBLIC so both this target and its consumers spell includes the same way:
# <sila/server/TlsConfig.h>, rooted at src/.
target_include_directories(sila2_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sila2_core PRIVATE OpenSSL::Crypto)
