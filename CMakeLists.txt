cmake_minimum_required(VERSION 3.16)
project(device-agent CXX)

# Option to build examples
option(BUILD_EXAMPLES "Build example business apps" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(GEN_DIR "${CMAKE_SOURCE_DIR}/gen")
set(SOURCES
    src/main.cpp
    src/client/device_client.cc
    src/client/command_handler.cc
    src/config/config.cc
    src/logger/logger.cc
    src/bridge/bridge.cc
    src/executor/linux_executor.cc
    src/executor/macos_executor.cc
    src/reboot_state/reboot_state.cc
    ${GEN_DIR}/terminal_agent/v1/device.pb.cc
    ${GEN_DIR}/terminal_agent/v1/device.grpc.pb.cc
    ${GEN_DIR}/terminal_agent/v1/service.pb.cc
    ${GEN_DIR}/terminal_agent/v1/service.grpc.pb.cc
)

set(HEADERS
    src/client/device_client.h
    src/client/command_handler.h
    src/config/config.h
    src/logger/logger.h
    src/executor/executor.h
    src/reboot_state/reboot_state.h
)

# ─── Android: build as shared library (.so) ───────────────
# Java Service 通过 System.loadLibrary("device-agent") 加载
# JNI_OnLoad 注册 native 方法
if(ANDROID)
    message(STATUS "Building device-agent as Android JNI library (.so)")
    add_definitions(-D__ANDROID__)
    list(APPEND SOURCES src/executor/android_executor.cc)

    add_library(device-agent SHARED ${SOURCES} ${HEADERS})
    set_target_properties(device-agent PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
    )

# ─── Desktop (Linux/macOS/Windows): build as executable ───
else()
    message(STATUS "Building device-agent as standalone daemon")
    add_executable(device-agent ${SOURCES} ${HEADERS})
endif()

# ─── gRPC / protobuf libs（desktop only）────────────────
# Android NDK 不需要这些，它们通过 Gradle 依赖引入
if(NOT ANDROID)
    # Find libraries explicitly (macOS ld doesn't handle special chars in -l flags well)
    set(GRPC_LIB_DIR "/opt/homebrew/Cellar/grpc/1.80.0/lib")
    set(PROTOBUF_LIB_DIR "/opt/homebrew/Cellar/protobuf/34.1/lib")
    set(ABSEIL_LIB_DIR "/opt/homebrew/Cellar/abseil/20260107.1/lib")
    set(CARES_LIB_DIR "/opt/homebrew/Cellar/c-ares/1.34.6/lib")
    set(OPENSSL_LIB_DIR "/opt/homebrew/Cellar/openssl@3/3.6.1/lib")
    set(RE2_LIB_DIR "/opt/homebrew/Cellar/re2/2025-11-05/lib")

    link_directories(
        ${GRPC_LIB_DIR}
        ${PROTOBUF_LIB_DIR}
        ${ABSEIL_LIB_DIR}
        ${CARES_LIB_DIR}
        ${OPENSSL_LIB_DIR}
        ${RE2_LIB_DIR}
    )

    # Include dirs
    target_include_directories(device-agent PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/gen
        /opt/homebrew/Cellar/grpc/1.80.0/include
        /opt/homebrew/Cellar/protobuf/34.1/include
        /opt/homebrew/Cellar/abseil/20260107.1/include
    )

    # Link libraries explicitly (no special chars in flag names)
    target_link_libraries(device-agent PRIVATE
        ${GRPC_LIB_DIR}/libgrpc++_unsecure.dylib
        ${GRPC_LIB_DIR}/libgrpc_unsecure.dylib
        ${PROTOBUF_LIB_DIR}/libprotobuf.dylib
        ${ABSEIL_LIB_DIR}/libabsl_statusor.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_check_op.dylib
        ${ABSEIL_LIB_DIR}/libabsl_flags_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_flags_marshalling.dylib
        ${ABSEIL_LIB_DIR}/libabsl_flags_reflection.dylib
        ${ABSEIL_LIB_DIR}/libabsl_flags_commandlineflag.dylib
        ${ABSEIL_LIB_DIR}/libabsl_flags_commandlineflag_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_flags_config.dylib
        ${ABSEIL_LIB_DIR}/libabsl_flags_program_name.dylib
        ${ABSEIL_LIB_DIR}/libabsl_raw_hash_set.dylib
        ${ABSEIL_LIB_DIR}/libabsl_hashtablez_sampler.dylib
        ${ABSEIL_LIB_DIR}/libabsl_exponential_biased.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_distributions.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_seed_sequences.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_internal_entropy_pool.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_internal_randen.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_internal_randen_hwaes.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_internal_randen_hwaes_impl.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_internal_randen_slow.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_internal_platform.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_internal_seed_material.dylib
        ${ABSEIL_LIB_DIR}/libabsl_random_seed_gen_exception.dylib
        ${ABSEIL_LIB_DIR}/libabsl_status.dylib
        ${ABSEIL_LIB_DIR}/libabsl_cord.dylib
        ${ABSEIL_LIB_DIR}/libabsl_cordz_info.dylib
        ${ABSEIL_LIB_DIR}/libabsl_cord_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_cordz_functions.dylib
        ${ABSEIL_LIB_DIR}/libabsl_exponential_biased.dylib
        ${ABSEIL_LIB_DIR}/libabsl_cordz_handle.dylib
        ${ABSEIL_LIB_DIR}/libabsl_crc_cord_state.dylib
        ${ABSEIL_LIB_DIR}/libabsl_crc32c.dylib
        ${ABSEIL_LIB_DIR}/libabsl_crc_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_crc_cpu_detect.dylib
        ${ABSEIL_LIB_DIR}/libabsl_leak_check.dylib
        ${ABSEIL_LIB_DIR}/libabsl_strerror.dylib
        ${ABSEIL_LIB_DIR}/libabsl_str_format_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_synchronization.dylib
        ${ABSEIL_LIB_DIR}/libabsl_graphcycles_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_kernel_timeout_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_stacktrace.dylib
        ${ABSEIL_LIB_DIR}/libabsl_borrowed_fixup_buffer.dylib
        ${ABSEIL_LIB_DIR}/libabsl_hash.dylib
        ${ABSEIL_LIB_DIR}/libabsl_city.dylib
        ${ABSEIL_LIB_DIR}/libabsl_symbolize.dylib
        ${ABSEIL_LIB_DIR}/libabsl_debugging_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_demangle_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_decode_rust_punycode.dylib
        ${ABSEIL_LIB_DIR}/libabsl_utf8_for_code_point.dylib
        ${ABSEIL_LIB_DIR}/libabsl_malloc_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_time.dylib
        ${ABSEIL_LIB_DIR}/libabsl_civil_time.dylib
        ${ABSEIL_LIB_DIR}/libabsl_strings.dylib
        ${ABSEIL_LIB_DIR}/libabsl_strings_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_int128.dylib
        ${ABSEIL_LIB_DIR}/libabsl_throw_delegate.dylib
        ${ABSEIL_LIB_DIR}/libabsl_time_zone.dylib
        ${ABSEIL_LIB_DIR}/libabsl_tracing_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_base.dylib
        ${ABSEIL_LIB_DIR}/libabsl_raw_logging_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_severity.dylib
        ${ABSEIL_LIB_DIR}/libabsl_spinlock_wait.dylib
        ${ABSEIL_LIB_DIR}/libabsl_vlog_config_internal.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_globals.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_globals.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_initialize.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_fnmatch.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_format.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_message.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_conditions.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_nullguard.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_structured_proto.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_log_sink_set.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_sink.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_entry.dylib
        ${ABSEIL_LIB_DIR}/libabsl_log_internal_proto.dylib
        ${CARES_LIB_DIR}/libcares.dylib
        ${OPENSSL_LIB_DIR}/libssl.dylib
        ${OPENSSL_LIB_DIR}/libcrypto.dylib
        ${RE2_LIB_DIR}/libre2.dylib
        ${GRPC_LIB_DIR}/libgpr.dylib
        pthread
        z
    )

    # Proto generation
    find_program(PROTOC protoc)
    if(NOT PROTOC)
        message(FATAL_ERROR "protoc not found")
    endif()

    set(PROTO_OUT_DIR ${CMAKE_SOURCE_DIR}/gen)
    file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

    add_custom_command(
        OUTPUT ${PROTO_OUT_DIR}/terminal_agent/v1/device.pb.h
               ${PROTO_OUT_DIR}/terminal_agent/v1/device.pb.cc
               ${PROTO_OUT_DIR}/terminal_agent/v1/service.grpc.pb.h
               ${PROTO_OUT_DIR}/terminal_agent/v1/service.grpc.pb.cc
               ${PROTO_OUT_DIR}/terminal_agent/v1/service.pb.h
               ${PROTO_OUT_DIR}/terminal_agent/v1/service.pb.cc
        COMMAND ${PROTOC}
            --cpp_out=${PROTO_OUT_DIR}
            --grpc_out=${PROTO_OUT_DIR}
            --plugin=protoc-gen-grpc=/opt/homebrew/opt/grpc/bin/grpc_cpp_plugin
            -I ${CMAKE_SOURCE_DIR}/proto
            ${CMAKE_SOURCE_DIR}/proto/terminal_agent/v1/device.proto
            ${CMAKE_SOURCE_DIR}/proto/terminal_agent/v1/service.proto
        DEPENDS ${PROTOC} ${CMAKE_SOURCE_DIR}/proto/terminal_agent/v1/*.proto
        COMMENT "Generating gRPC C++ code from proto files"
    )

    add_custom_target(generate_protos ALL
        DEPENDS ${PROTO_OUT_DIR}/terminal_agent/v1/device.pb.h
    )

    add_dependencies(device-agent generate_protos)
endif()

# Build examples (desktop only)
if(BUILD_EXAMPLES AND NOT ANDROID)
    add_subdirectory(examples)
endif()
