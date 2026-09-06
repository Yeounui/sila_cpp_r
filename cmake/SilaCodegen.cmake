# SilaCodegen.cmake — FDL -> .proto -> C++ stub pipeline
#
# sila_generate_feature(
#     TARGET     <name>
#     FDL_FILES  <list of .sila.xml paths>
#     OUTPUT_DIR <dir>   # default: ${CMAKE_CURRENT_BINARY_DIR}/generated
# )
#
# Sets in caller scope:
#   ${TARGET}_GENERATED_SRCS  — .pb.cc and .grpc.pb.cc files for add_library()
#   ${TARGET}_INCLUDE_DIR     — include path for generated .pb.h headers

function(sila_generate_feature)
    cmake_parse_arguments(ARG "" "TARGET;OUTPUT_DIR" "FDL_FILES" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "sila_generate_feature: TARGET is required")
    endif()
    if(NOT ARG_FDL_FILES)
        message(FATAL_ERROR "sila_generate_feature: FDL_FILES is required")
    endif()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()

    # --- codegen tool paths ---------------------------------------------------

    set(_codegen_python "${CMAKE_SOURCE_DIR}/src/codegen/.venv/bin/python")
    set(_codegen_cli    "${CMAKE_SOURCE_DIR}/src/codegen/cli.py")

    # Rebuild when codegen sources or templates change.
    file(GLOB_RECURSE _codegen_sources "${CMAKE_SOURCE_DIR}/src/codegen/*.py"
         "${CMAKE_SOURCE_DIR}/src/codegen/templates/*.j2")
    list(FILTER _codegen_sources EXCLUDE REGEX ".venv/")

    # --- proto import paths (same structure as src/sila/CMakeLists.txt) --------

    set(_proto_output_dir "${ARG_OUTPUT_DIR}/proto")

    set(_proto_import_dirs
        "${CMAKE_SOURCE_DIR}/third_party/sila_base/protobuf"
        "${_proto_output_dir}"
    )
    set(_proto_import_args)
    foreach(_dir IN LISTS _proto_import_dirs)
        list(APPEND _proto_import_args "--proto_path=${_dir}")
    endforeach()

    # --- per-FDL generation ---------------------------------------------------

    set(_all_generated_srcs)

    foreach(_fdl IN LISTS ARG_FDL_FILES)
        # MyFeature-v1_0.sila.xml -> MyFeature
        # MyFeature.sila.xml      -> MyFeature  (example FDLs without version suffix)
        get_filename_component(_fdl_basename "${_fdl}" NAME)
        string(REGEX REPLACE "-v[0-9]+_[0-9]+\\.sila\\.xml$" "" _feature "${_fdl_basename}")
        if(_feature STREQUAL _fdl_basename)
            string(REGEX REPLACE "\\.sila\\.xml$" "" _feature "${_fdl_basename}")
        endif()

        set(_proto "${_proto_output_dir}/${_feature}.proto")
        set(_service_adapter "${_proto_output_dir}/${_feature}ServiceAdapter.h")
        set(_meta_output_dir "${ARG_OUTPUT_DIR}/meta")
        set(_meta_h "${_meta_output_dir}/${_feature}Meta.h")
        set(_meta_cc "${_meta_output_dir}/${_feature}Meta.cc")

        # Step 1: FDL -> .proto + ServiceAdapter.h + Meta.h/cc via codegen
        add_custom_command(
            OUTPUT  "${_proto}" "${_service_adapter}" "${_meta_h}" "${_meta_cc}"
            COMMAND "${_codegen_python}" "${_codegen_cli}" "${_fdl}"
                    -o "${ARG_OUTPUT_DIR}"
            DEPENDS "${_fdl}" ${_codegen_sources}
            COMMENT "codegen ${_feature}.sila.xml -> .proto + ServiceAdapter.h + Meta.h/cc"
        )

        # Step 2: .proto -> C++ via protoc + grpc_cpp_plugin
        set(_pb_cc   "${_proto_output_dir}/${_feature}.pb.cc")
        set(_pb_h    "${_proto_output_dir}/${_feature}.pb.h")
        set(_grpc_cc "${_proto_output_dir}/${_feature}.grpc.pb.cc")
        set(_grpc_h  "${_proto_output_dir}/${_feature}.grpc.pb.h")

        add_custom_command(
            OUTPUT  "${_pb_cc}" "${_pb_h}" "${_grpc_cc}" "${_grpc_h}"
            COMMAND $<TARGET_FILE:protobuf::protoc>
            ARGS    ${_proto_import_args}
                    "--cpp_out=${_proto_output_dir}"
                    "--grpc_out=${_proto_output_dir}"
                    "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
                    "${_proto}"
            DEPENDS "${_proto}"
            COMMENT "protoc ${_feature}.proto -> C++"
        )

        list(APPEND _all_generated_srcs "${_pb_cc}" "${_grpc_cc}" "${_meta_cc}")
    endforeach()

    # --- export to caller scope -----------------------------------------------

    set(${ARG_TARGET}_GENERATED_SRCS "${_all_generated_srcs}" PARENT_SCOPE)
    set(${ARG_TARGET}_INCLUDE_DIR    "${_proto_output_dir}"   PARENT_SCOPE)
    set(${ARG_TARGET}_META_INCLUDE_DIR "${ARG_OUTPUT_DIR}/meta" PARENT_SCOPE)
endfunction()
