# --- Proto code generation ------------------------------------------------
# protoc and grpc_cpp_plugin are imported executables from vcpkg's protobuf
# and gRPC packages. Generator expressions resolve their paths at build time.

set(_sila_proto_import_dirs
    ${CMAKE_SOURCE_DIR}/third_party/sila_base/protobuf
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto
)

set(_sila_proto_files
    ${CMAKE_SOURCE_DIR}/third_party/sila_base/protobuf/SiLAFramework.proto
    ${CMAKE_SOURCE_DIR}/third_party/sila_base/protobuf/SiLABinaryTransfer.proto
    ${CMAKE_SOURCE_DIR}/third_party/sila_base/protobuf/SiLACloudConnector.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/SiLAService.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/SimulationController.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/LockController.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/ErrorRecoveryService.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/AuthenticationService.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/AuthorizationService.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/AuthorizationConfigurationService.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/AuthorizationProviderService.proto
    ${CMAKE_SOURCE_DIR}/src/sila/common/proto/ConnectionConfigurationService.proto
)

set(_sila_proto_import_args)
foreach(_dir IN LISTS _sila_proto_import_dirs)
    list(APPEND _sila_proto_import_args --proto_path=${_dir})
endforeach()

set(_sila_proto_generated_srcs)
foreach(_proto IN LISTS _sila_proto_files)
    get_filename_component(_name ${_proto} NAME_WE)
    set(_pb_cc   ${CMAKE_CURRENT_BINARY_DIR}/${_name}.pb.cc)
    set(_pb_h    ${CMAKE_CURRENT_BINARY_DIR}/${_name}.pb.h)
    set(_grpc_cc ${CMAKE_CURRENT_BINARY_DIR}/${_name}.grpc.pb.cc)
    set(_grpc_h  ${CMAKE_CURRENT_BINARY_DIR}/${_name}.grpc.pb.h)

    add_custom_command(
        OUTPUT ${_pb_cc} ${_pb_h} ${_grpc_cc} ${_grpc_h}
        COMMAND $<TARGET_FILE:protobuf::protoc>
        ARGS    ${_sila_proto_import_args}
                --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
                --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
                --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
                ${_proto}
        DEPENDS ${_sila_proto_files}
        COMMENT "protoc ${_name}.proto"
    )

    list(APPEND _sila_proto_generated_srcs ${_pb_cc} ${_grpc_cc})
endforeach()

# --- FDL-embedded constants -------------------------------------------------
# Each entry: "FDL-filename|output-subdir|header-basename|variable-name"
set(_fdl_embed_entries
    "ErrorRecoveryService-v2_0|server/recovery|ErrorRecoveryFdl|kErrorRecoveryFdlXml"
    "SiLAService-v1_0|server|SiLAServiceFdl|kSiLAServiceFdlXml"
    "SimulationController-v1_0|server/features|SimulationControllerFdl|kSimulationControllerFdlXml"
    "LockController-v1_0|server/features|LockControllerFdl|kLockControllerFdlXml"
    "AuthenticationService-v1_0|server/features|AuthenticationServiceFdl|kAuthenticationServiceFdlXml"
    "AuthorizationService-v1_0|server/features|AuthorizationServiceFdl|kAuthorizationServiceFdlXml"
    "AuthorizationConfigurationService-v1_0|server/features|AuthorizationConfigurationServiceFdl|kAuthorizationConfigurationServiceFdlXml"
    "ConnectionConfigurationService-v1_1|server/features|ConnectionConfigurationServiceFdl|kConnectionConfigurationServiceFdlXml"
)
set(_fdl_headers "")
foreach(_entry IN LISTS _fdl_embed_entries)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _fdl_name)
    list(GET _parts 1 _subdir)
    list(GET _parts 2 _basename)
    list(GET _parts 3 _var)

    set(_src "${CMAKE_SOURCE_DIR}/third_party/sila_base/feature_definitions/org/silastandard/core/${_fdl_name}.sila.xml")
    set(_hdr "${CMAKE_CURRENT_BINARY_DIR}/generated/sila/${_subdir}/${_basename}.h")

    add_custom_command(
        OUTPUT  ${_hdr}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT_FILE=${_src}
                -DOUTPUT_FILE=${_hdr}
                -DVAR_NAME=${_var}
                -DNAMESPACE=sila2::generated
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFileAsString.cmake
        DEPENDS ${_src} ${CMAKE_SOURCE_DIR}/cmake/EmbedFileAsString.cmake
        COMMENT "Embedding ${_fdl_name}.sila.xml as C++ constant"
    )
    list(APPEND _fdl_headers ${_hdr})
endforeach()

# Runtime FDL validation resources. Keep these pinned to the checked-out
# sila_base revision and embed them so validation has no filesystem or network
# dependency at runtime. The XSDs refer to one another by these basenames;
# FdlRuntimeParser.cc supplies libxml2's in-memory input callback for them.
# Constraints.xsd is the exception: it is served from the in-repo overlay at
# src/schema/Constraints.xsd, not the submodule copy, for three Part B fixes:
# Conversion Factor/Offset are xs:double, not xs:decimal (p84-85: the submodule
# rejects valid double literals like 6.022e23/-INF/NaN), and MinimalLength plus
# the ElementCount trio are xs:nonNegativeInteger, not xs:positiveInteger (Part
# A p67 / Part B p83 / p85: a 0 minimal-length or element-count bound MUST be
# accepted). fdl-validation.xsl is likewise served from the in-repo overlay at
# src/schema/fdl-validation.xsl, not the submodule copy: Part A p66 / Part B
# p83 allow a SiLA Constrained Type to be based on another SiLA Constrained
# Type, but the pinned stylesheet's detect-invalid-constraint-base-type
# template wrongly rejects that nesting. Both overlay entries below
# (Constraints.xsd, fdl-validation.xsl) point at src/schema/; the remaining
# set(...) embed entries stay pointed at the submodule.
set(_fdl_standard_embed_entries
    "${CMAKE_SOURCE_DIR}/third_party/sila_base/schema/FeatureDefinition.xsd|client/dynamic|FeatureDefinitionXsd|kFeatureDefinitionXsd"
    "${CMAKE_SOURCE_DIR}/third_party/sila_base/schema/DataTypes.xsd|client/dynamic|DataTypesXsd|kDataTypesXsd"
    "${CMAKE_SOURCE_DIR}/src/schema/Constraints.xsd|client/dynamic|ConstraintsXsd|kConstraintsXsd"
    "${CMAKE_SOURCE_DIR}/third_party/sila_base/schema/AnyTypeDataType.xsd|client/dynamic|AnyTypeDataTypeXsd|kAnyTypeDataTypeXsd"
    "${CMAKE_SOURCE_DIR}/src/schema/fdl-validation.xsl|client/dynamic|FdlValidationXslt|kFdlValidationXslt"
)
foreach(_entry IN LISTS _fdl_standard_embed_entries)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _src)
    list(GET _parts 1 _subdir)
    list(GET _parts 2 _basename)
    list(GET _parts 3 _var)

    set(_hdr "${CMAKE_CURRENT_BINARY_DIR}/generated/sila/${_subdir}/${_basename}.h")
    add_custom_command(
        OUTPUT  ${_hdr}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT_FILE=${_src}
                -DOUTPUT_FILE=${_hdr}
                -DVAR_NAME=${_var}
                -DNAMESPACE=sila2::generated
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFileAsString.cmake
        DEPENDS ${_src} ${CMAKE_SOURCE_DIR}/cmake/EmbedFileAsString.cmake
        COMMENT "Embedding ${_src} as C++ constant"
    )
    list(APPEND _fdl_headers ${_hdr})
endforeach()
add_custom_target(sila2_generated_headers DEPENDS ${_fdl_headers})

# --- sila2_core library ----------------------------------------------------

add_library(sila2_core
    server/binary/BinaryStore.cc
    server/binary/InMemoryBinaryStore.cc
    server/binary/FileSpoolBinaryStore.cc
    server/binary/HybridBinaryStore.cc
    server/binary/BinaryUploadService.cc
    server/binary/BinaryDownloadService.cc
    server/FeatureRegistry.cc
    server/CommandParameterValidator.cc
    server/SilaServerBase.cc
    server/SilaServiceImpl.cc
    server/config/TlsConfig.cc
    common/tls/UntrustedTlsCredentials.cc
    server/config/ServerConfig.cc
    common/error/SilaError.cc
    common/error/SilaErrorException.cc
    common/error/SilaErrorSubtypes.cc
    server/metadata/MetadataExtractingInterceptor.cc
    client/ClientConfig.cc
    client/MetadataInjector.cc
    client/SilaClientBase.cc
    client/ExecutionStore.cc
    client/ExecutionInfoSubscriber.cc
    client/RecoverableErrorSubscriber.cc
    server/command/ObservableCommandExecution.cc
    server/command/ObservableCommandManager.cc
    server/auth/AuthTokenStore.cc
    server/auth/AuthorizationInterceptor.cc
    common/util/uuid.cc
    server/transport/CallContext.cc
    server/transport/GrpcTransport.cc
    server/transport/cloud/StreamWriteSerializer.cc
    server/transport/cloud/ActiveCallRegistry.cc
    server/transport/cloud/CloudEnvelopeRouter.cc
    server/transport/cloud/CloudTransport.cc
    client/CloudClientListener.cc
    server/property/ObservablePropertyManager.cc
    server/recovery/RecoverableErrorGate.cc
    server/recovery/ErrorRecoveryServiceImpl.cc
    server/features/SimulationControllerImpl.cc
    server/features/LockControllerImpl.cc
    server/features/AuthenticationServiceImpl.cc
    server/features/AuthorizationServiceImpl.cc
    server/features/AuthorizationConfigurationServiceImpl.cc
    server/features/ConnectionConfigurationServiceImpl.cc
    client/AuthSession.cc
    client/binary/BinaryUploader.cc
    client/binary/BinaryDownloader.cc
    common/types/Constraints.cc
    client/dynamic/FdlRuntimeParser.cc
    client/dynamic/ConstraintChecker.cc
    client/dynamic/ValueValidator.cc
    client/dynamic/JsonSchemaSupport.cc
    # R10-9a: the Any decoder joins the parser/validator siblings in core so
    # the server library can decode an Any to check AllowedTypes (Part A p67).
    client/dynamic/DescriptorBuilder.cc
    client/dynamic/AnyCodec.cc
    server/discovery/MdnsPublisher.cc
    client/discovery/MdnsBrowser.cc
    client/ServerRegistry.cc
    ${_sila_proto_generated_srcs}
)
add_library(Sila2::core ALIAS sila2_core)
add_dependencies(sila2_core sila2_generated_headers)

set_target_properties(sila2_core PROPERTIES EXPORT_NAME core)

# -Wall/-Wextra on our own targets only, never via global CMAKE_CXX_FLAGS,
# so vcpkg/protobuf-generated third-party sources don't drown out our own
# warnings (audit 4.1f). Not -Werror: this tree has never built with these
# flags, so the warning count is unknown -- turning them into errors is a
# deliberate follow-up once that count is seen.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sila2_core PRIVATE -Wall -Wextra)
    # The protoc/grpc-generated sources above are compiled as part of this
    # target; silence warnings on those specific files rather than on all of
    # sila2_core, since they're vendor-generated code we don't own.
    set_source_files_properties(${_sila_proto_generated_srcs} PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_include_directories(sila2_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(sila2_core
    PRIVATE OpenSSL::Crypto gRPC::grpc++_reflection LibXml2::LibXml2 LibXslt::LibXslt nlohmann_json_schema_validator::validator
    PUBLIC  gRPC::grpc++ protobuf::libprotobuf
)

option(SILA_ENABLE_OTEL "Enable OpenTelemetry tracing" OFF)
if(SILA_ENABLE_OTEL)
    find_package(opentelemetry-cpp CONFIG REQUIRED)
    target_link_libraries(sila2_core
        PUBLIC  opentelemetry-cpp::api
        PRIVATE opentelemetry-cpp::sdk)
    target_compile_definitions(sila2_core PUBLIC SILA_ENABLE_OTEL)
endif()

# --- sila2_dynamic library (§1.1, §4.2) -----------------------------------
# Device-side builds link only Sila2::core, which now includes descriptor
# assembly and the Any codec (R10-9a); orchestrators add Sila2::dynamic
# for the runtime dynamic-call path (FeatureCatalog, GenericStub, JSON).

add_library(sila2_dynamic
    client/dynamic/FeatureCatalog.cc
    client/dynamic/DynamicCall.cc
    client/dynamic/ObservableCommandRunner.cc
    client/dynamic/JsonCodec.cc
)
add_library(Sila2::dynamic ALIAS sila2_dynamic)
set_target_properties(sila2_dynamic PROPERTIES EXPORT_NAME dynamic)
add_dependencies(sila2_dynamic sila2_generated_headers)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sila2_dynamic PRIVATE -Wall -Wextra)
endif()

target_link_libraries(sila2_dynamic
    PUBLIC  Sila2::core
    PRIVATE LibXml2::LibXml2 LibXslt::LibXslt
)
