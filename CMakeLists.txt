cmake_minimum_required(VERSION 3.16)
project(device-agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(GEN_DIR "${CMAKE_SOURCE_DIR}/gen")
set(COMMON_SOURCES
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

set(COMMON_HEADERS
    src/client/device_client.h
    src/client/command_handler.h
    src/config/config.h
    src/logger/logger.h
    src/executor/executor.h
    src/reboot_state/reboot_state.h
)

# ─── Android: build as JNI shared library ─────────────────
if(ANDROID OR __ANDROID__)
    message(STATUS "=== Building device-agent for Android ===")

    add_definitions(-D__ANDROID__)
    list(APPEND COMMON_SOURCES src/executor/android_executor.cc)

    # Exclude desktop executors from Android build
    list(FILTER COMMON_SOURCES EXCLUDE REGEX "src/executor/linux_executor.cc$")
    list(FILTER COMMON_SOURCES EXCLUDE REGEX "src/executor/macos_executor.cc$")

    # Pre-built gRPC C++ for Android (built from grpc v1.62.2 source for NDK)
    # Each ABI has its own install directory
    set(GRPC_ANDROID_INSTALL_DIR "$ENV{GRPC_ANDROID_INSTALL_DIR}")
    if(NOT GRPC_ANDROID_INSTALL_DIR)
        # Detect from ANDROID_ABI: each ABI has its own install directory
        # Libraries are prebuilt at:
        #   ${CMAKE_SOURCE_DIR}/android/grpc-android-prebuilt/<ABI>/
        if(ANDROID_ABI STREQUAL "arm64-v8a")
            set(GRPC_ANDROID_INSTALL_DIR "${CMAKE_SOURCE_DIR}/android/grpc-android-prebuilt/arm64-v8a")
        elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
            set(GRPC_ANDROID_INSTALL_DIR "${CMAKE_SOURCE_DIR}/android/grpc-android-prebuilt/armeabi-v7a")
        elseif(ANDROID_ABI STREQUAL "x86_64")
            set(GRPC_ANDROID_INSTALL_DIR "${CMAKE_SOURCE_DIR}/android/grpc-android-prebuilt/x86_64")
        elseif(ANDROID_ABI STREQUAL "x86")
            set(GRPC_ANDROID_INSTALL_DIR "${CMAKE_SOURCE_DIR}/android/grpc-android-prebuilt/x86")
        else()
            message(FATAL_ERROR "Unknown ANDROID_ABI: ${ANDROID_ABI}")
        endif()
    endif()
    message(STATUS "gRPC Android install dir: ${GRPC_ANDROID_INSTALL_DIR}")

    # Prepend our install prefix so it takes priority over prefab CMAKE_FIND_ROOT_PATH
    list(PREPEND CMAKE_PREFIX_PATH "${GRPC_ANDROID_INSTALL_DIR}")
    find_package(Threads REQUIRED)

    # protobuf: config is at lib/cmake/protobuf/protobuf-config.cmake (kebab-case)
    # find_package(Foo CONFIG) looks for FooConfig.cmake (PascalCase).
    # We use _DIR variables to point directly to the cmake dirs.
    # Set these BEFORE find_package so transitive find_package calls inside
    # protobuf/gRPC cmake configs also use the correct paths.
    set(absl_DIR "${GRPC_ANDROID_INSTALL_DIR}/lib/cmake/absl" CACHE PATH "" FORCE)
    set(utf8_range_DIR "${GRPC_ANDROID_INSTALL_DIR}/lib/cmake/utf8_range" CACHE PATH "" FORCE)
    set(c-ares_DIR "${GRPC_ANDROID_INSTALL_DIR}/lib/cmake/c-ares" CACHE PATH "" FORCE)
    set(re2_DIR "${GRPC_ANDROID_INSTALL_DIR}/lib/cmake/re2" CACHE PATH "" FORCE)
    set(protobuf_DIR "${GRPC_ANDROID_INSTALL_DIR}/lib/cmake/protobuf" CACHE PATH "" FORCE)
    set(gRPC_DIR "${GRPC_ANDROID_INSTALL_DIR}/lib/cmake/grpc" CACHE PATH "" FORCE)
    # Bypass CMake version check for abseil (uses non-standard version "20240116")
    # find_dependency(absl) checks for absl_ABSL_NO_VERSION_CHECK
    set(absl_ABSL_NO_VERSION_CHECK TRUE CACHE BOOL "")
    set(utf8_range_UTF8_RANGE_NO_VERSION_CHECK TRUE CACHE BOOL "")
    find_package(protobuf CONFIG REQUIRED)

    # gRPC: config is at lib/cmake/grpc/gRPCConfig.cmake
    set(gRPC_DIR "${GRPC_ANDROID_INSTALL_DIR}/lib/cmake/grpc")
    find_package(gRPC CONFIG REQUIRED)

    add_library(device-agent SHARED
        ${COMMON_SOURCES}
        ${COMMON_HEADERS}
    )

    set_target_properties(device-agent PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
    )

    target_include_directories(device-agent PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/gen
        ${GRPC_ANDROID_INSTALL_DIR}/include
    )

    target_link_libraries(device-agent PRIVATE
        android
        log
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++_unsecure
        gRPC::grpc_unsecure
    )

# ─── Desktop (Linux/macOS): build as standalone executable ─
else()
    message(STATUS "=== Building device-agent for desktop ===")

    # Proto files in gen/ are v4.25.1 (for Android).
    # Desktop needs v7.34.1 for system protobuf v34.1.
    # Copy desktop proto files (v7.34.1) from gen-desktop/ to gen/.
    set(DESKTOP_GEN_DIR "${CMAKE_SOURCE_DIR}/gen-desktop")
    set(ANDROID_GEN_DIR "${CMAKE_SOURCE_DIR}/gen")
    if(EXISTS "${DESKTOP_GEN_DIR}/terminal_agent/v1/service.pb.h")
        message(STATUS "Copying desktop proto files from gen-desktop/")
        file(COPY
            "${DESKTOP_GEN_DIR}/terminal_agent/v1/device.pb.h"
            "${DESKTOP_GEN_DIR}/terminal_agent/v1/device.pb.cc"
            "${DESKTOP_GEN_DIR}/terminal_agent/v1/service.grpc.pb.h"
            "${DESKTOP_GEN_DIR}/terminal_agent/v1/service.grpc.pb.cc"
            "${DESKTOP_GEN_DIR}/terminal_agent/v1/service.pb.h"
            "${DESKTOP_GEN_DIR}/terminal_agent/v1/service.pb.cc"
            DESTINATION "${ANDROID_GEN_DIR}/terminal_agent/v1/"
        )
    endif()

    add_executable(device-agent
        ${COMMON_SOURCES}
        ${COMMON_HEADERS}
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

    # Library paths
    set(GRPC_LIB_DIR "/opt/homebrew/Cellar/grpc/1.80.0/lib")
    set(PROTOBUF_LIB_DIR "/opt/homebrew/Cellar/protobuf/34.1/lib")
    set(ABSEIL_LIB_DIR "/opt/homebrew/Cellar/abseil/20260107.1/lib")
    set(CARES_LIB_DIR "/opt/homebrew/Cellar/c-ares/1.34.6/lib")
    set(OPENSSL_LIB_DIR "/opt/homebrew/Cellar/openssl@3/3.6.1/lib")
    set(RE2_LIB_DIR "/opt/homebrew/Cellar/re2/2025-11-05/lib")

    link_directories(
        ${GRPC_LIB_DIR} ${PROTOBUF_LIB_DIR} ${ABSEIL_LIB_DIR}
        ${CARES_LIB_DIR} ${OPENSSL_LIB_DIR} ${RE2_LIB_DIR}
    )

    # Link libraries
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

    # Build examples
    option(BUILD_EXAMPLES "Build example business apps" ON)
    if(BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()
endif()
