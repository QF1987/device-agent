cmake_minimum_required(VERSION 3.16)
project(device-agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(COMMON_SOURCES
    src/main.cpp
    src/client/device_client.cc
    src/client/network_info.cc
    src/client/command_handler.cc
    src/config/config.cc
    src/config/p2p_config_store.cc
    src/logger/logger.cc
    src/bridge/bridge.cc
    src/executor/linux_executor.cc
    src/executor/macos_executor.cc
    src/reboot_state/reboot_state.cc
    src/download/android_download_manager.cc
    src/download/network_policy.cc
    src/download/p2p_upload_counters.cc
)

set(COMMON_HEADERS
    src/client/device_client.h
    src/client/network_info.h
    src/client/command_handler.h
    src/config/config.h
    src/config/p2p_config_store.h
    src/logger/logger.h
    src/executor/executor.h
    src/download/idownload_manager.h
    src/download/android_download_manager.h
    src/download/network_policy.h
    src/reboot_state/reboot_state.h
    src/jni_bridge.h
)

set(REMOTE_DESKTOP_SOURCES
    src/remotedesktop/rfb/rfb_protocol.cc
    src/remotedesktop/rfb/rfb_server.cc
    src/remotedesktop/tunnel/tunnel_protocol.cc
)

set(REMOTE_DESKTOP_HEADERS
    src/remotedesktop/input_injector.h
    src/remotedesktop/screen_capturer.h
    src/remotedesktop/rfb/rfb_protocol.h
    src/remotedesktop/rfb/rfb_server.h
    src/remotedesktop/tunnel/tunnel_protocol.h
)

if(WIN32)
    list(APPEND REMOTE_DESKTOP_SOURCES
        src/remotedesktop/platform/windows/windows_badge.cc
        src/remotedesktop/platform/windows/windows_input_injector.cc
        src/remotedesktop/platform/windows/windows_screen_capturer.cc
    )
    list(APPEND REMOTE_DESKTOP_HEADERS
        src/remotedesktop/platform/windows/windows_badge.h
        src/remotedesktop/platform/windows/windows_input_injector.h
        src/remotedesktop/platform/windows/windows_screen_capturer.h
    )
endif()

add_library(remote_desktop STATIC
    ${REMOTE_DESKTOP_SOURCES}
    ${REMOTE_DESKTOP_HEADERS}
)
target_include_directories(remote_desktop PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(remote_desktop PUBLIC user32 gdi32)
endif()

if(NOT ANDROID AND NOT __ANDROID__)
    add_executable(rfb_protocol_test
        src/remotedesktop/rfb/rfb_protocol_test.cc
    )
    target_link_libraries(rfb_protocol_test PRIVATE remote_desktop)
endif()

# ─── Android: build as JNI shared library ─────────────────
if(ANDROID OR __ANDROID__)
    message(STATUS "=== Building device-agent for Android ===")

    add_definitions(-D__ANDROID__)
    list(APPEND COMMON_SOURCES src/executor/android_executor.cc)
    list(APPEND COMMON_SOURCES src/jni_wrapper.cpp)
    list(APPEND COMMON_SOURCES src/download/p2p_download_manager.cc src/download/p2p_upload_counters.cc)

    # Exclude desktop executors from Android build
    list(FILTER COMMON_SOURCES EXCLUDE REGEX "src/executor/linux_executor.cc$")
    list(FILTER COMMON_SOURCES EXCLUDE REGEX "src/executor/macos_executor.cc$")

    # Proto files for Android: gen-android/ (v4.25.1)
    set(GEN_ANDROID_DIR "${CMAKE_SOURCE_DIR}/gen-android")
    file(MAKE_DIRECTORY "${GEN_ANDROID_DIR}/terminal_agent/v1/")
    list(APPEND COMMON_SOURCES
        "${GEN_ANDROID_DIR}/terminal_agent/v1/device.pb.cc"
        "${GEN_ANDROID_DIR}/terminal_agent/v1/device.grpc.pb.cc"
        "${GEN_ANDROID_DIR}/terminal_agent/v1/service.pb.cc"
        "${GEN_ANDROID_DIR}/terminal_agent/v1/service.grpc.pb.cc"
    )

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

    # Pre-built libtorrent for Android. Built by:
    #   bash scripts/build-libtorrent-android.sh
    set(LIBTORRENT_ANDROID_INSTALL_DIR "$ENV{LIBTORRENT_ANDROID_INSTALL_DIR}")
    if(NOT LIBTORRENT_ANDROID_INSTALL_DIR)
        if(ANDROID_ABI STREQUAL "arm64-v8a")
            set(LIBTORRENT_ANDROID_INSTALL_DIR "${CMAKE_SOURCE_DIR}/android/libtorrent-android-prebuilt/arm64-v8a")
        elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
            set(LIBTORRENT_ANDROID_INSTALL_DIR "${CMAKE_SOURCE_DIR}/android/libtorrent-android-prebuilt/armeabi-v7a")
        else()
            message(FATAL_ERROR "libtorrent prebuilt is only required for M3-A on arm64-v8a and armeabi-v7a; got ${ANDROID_ABI}")
        endif()
    endif()
    message(STATUS "libtorrent Android install dir: ${LIBTORRENT_ANDROID_INSTALL_DIR}")

    if(NOT EXISTS "${LIBTORRENT_ANDROID_INSTALL_DIR}/lib/libtorrent-rasterbar.a")
        message(FATAL_ERROR "Missing libtorrent prebuilt: ${LIBTORRENT_ANDROID_INSTALL_DIR}/lib/libtorrent-rasterbar.a. Run scripts/build-libtorrent-android.sh first.")
    endif()
    if(NOT EXISTS "${LIBTORRENT_ANDROID_INSTALL_DIR}/include/libtorrent/version.hpp")
        message(FATAL_ERROR "Missing libtorrent headers in ${LIBTORRENT_ANDROID_INSTALL_DIR}/include")
    endif()

    file(GLOB LIBTORRENT_BOOST_STATIC_LIBS
        "${LIBTORRENT_ANDROID_INSTALL_DIR}/lib/libboost_*.a"
    )
    if(NOT LIBTORRENT_BOOST_STATIC_LIBS)
        message(FATAL_ERROR "Missing Boost static libraries in ${LIBTORRENT_ANDROID_INSTALL_DIR}/lib")
    endif()

    set(LIBTORRENT_SYMBOL_CHECK "${CMAKE_BINARY_DIR}/libtorrent_symbol_check.cc")
    file(WRITE "${LIBTORRENT_SYMBOL_CHECK}" [=[
#include <libtorrent/version.hpp>

extern "C" const char* device_agent_libtorrent_version() {
    return lt::version();
}
]=])
    list(APPEND COMMON_SOURCES "${LIBTORRENT_SYMBOL_CHECK}")

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
        ${GEN_ANDROID_DIR}
        ${GRPC_ANDROID_INSTALL_DIR}/include
        ${CMAKE_SOURCE_DIR}/android/libtorrent-android-prebuilt/include
        ${LIBTORRENT_ANDROID_INSTALL_DIR}/include
    )
    target_compile_definitions(device-agent PRIVATE TORRENT_ABI_VERSION=3)

    target_link_libraries(device-agent PRIVATE
        android
        log
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc
        "${LIBTORRENT_ANDROID_INSTALL_DIR}/lib/libtorrent-rasterbar.a"
        ${LIBTORRENT_BOOST_STATIC_LIBS}
    )

    add_executable(p2p_download_manager_test
        src/download/p2p_download_manager_test.cc
        src/download/p2p_download_manager.cc src/download/p2p_upload_counters.cc
        src/download/network_policy.cc
        src/config/p2p_config_store.cc
        src/logger/logger.cc
    )
    target_include_directories(p2p_download_manager_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/android/libtorrent-android-prebuilt/include
        ${LIBTORRENT_ANDROID_INSTALL_DIR}/include
    )
    target_compile_definitions(p2p_download_manager_test PRIVATE
        TORRENT_ABI_VERSION=3
        DEVICE_AGENT_TESTING
    )
    target_link_libraries(p2p_download_manager_test PRIVATE
        log
        "${LIBTORRENT_ANDROID_INSTALL_DIR}/lib/libtorrent-rasterbar.a"
        ${LIBTORRENT_BOOST_STATIC_LIBS}
    )

    add_executable(network_policy_test
        src/download/network_policy_test.cc
        src/download/network_policy.cc
    )
    target_include_directories(network_policy_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    add_executable(p2p_config_store_test
        src/config/p2p_config_store_test.cc
        src/config/p2p_config_store.cc
    )
    target_include_directories(p2p_config_store_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    add_executable(network_info_test
        src/client/network_info_test.cc
        src/client/network_info.cc
        "${GEN_ANDROID_DIR}/terminal_agent/v1/device.pb.cc"
    )
    target_include_directories(network_info_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GEN_ANDROID_DIR}
        ${GRPC_ANDROID_INSTALL_DIR}/include
    )
    target_link_libraries(network_info_test PRIVATE protobuf::libprotobuf)

    add_executable(p2p_upload_counters_test
        src/download/p2p_upload_counters_test.cc
        src/download/p2p_upload_counters.cc
        src/download/network_policy.cc
    )
    target_include_directories(p2p_upload_counters_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ─── Desktop (Linux/macOS): build as standalone executable ─
else()
    message(STATUS "=== Building device-agent for desktop ===")
    option(BUILD_P2P_DOCKER_TOOLS "Build Linux P2P Docker spike tools" ON)

    find_package(Threads REQUIRED)
    find_package(protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBTORRENT_RASTERBAR REQUIRED IMPORTED_TARGET libtorrent-rasterbar)

    # Proto files for Desktop: gen-desktop/ (v34.1, pre-generated)
    # gen/ is only used by Android (gen-android/)
    set(GEN_DESKTOP_DIR "${CMAKE_SOURCE_DIR}/gen-desktop")
    list(APPEND COMMON_SOURCES
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.grpc.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/service.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/service.grpc.pb.cc"
        src/download/p2p_download_manager.cc src/download/p2p_upload_counters.cc
    )

    add_executable(device-agent
        ${COMMON_SOURCES}
        ${COMMON_HEADERS}
    )

    # Include dirs
    target_include_directories(device-agent PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${GEN_DESKTOP_DIR}
    )

    target_link_libraries(device-agent PRIVATE
        Threads::Threads
        OpenSSL::Crypto
        PkgConfig::LIBTORRENT_RASTERBAR
        protobuf::libprotobuf
        gRPC::grpc++
        z
    )

    add_executable(network_info_test
        src/client/network_info_test.cc
        src/client/network_info.cc
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.pb.cc"
    )
    target_include_directories(network_info_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GEN_DESKTOP_DIR}
    )
    target_link_libraries(network_info_test PRIVATE protobuf::libprotobuf)

    add_executable(p2p_upload_counters_test
        src/download/p2p_upload_counters_test.cc
        src/download/p2p_upload_counters.cc
        src/download/network_policy.cc
    )
    target_include_directories(p2p_upload_counters_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_executable(command_handler_download_test
        src/client/command_handler_download_test.cc
        src/client/command_handler.cc
        src/config/p2p_config_store.cc
        src/executor/linux_executor.cc
        src/logger/logger.cc
        src/reboot_state/reboot_state.cc
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/service.pb.cc"
    )
    target_include_directories(command_handler_download_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GEN_DESKTOP_DIR}
    )
    target_link_libraries(command_handler_download_test PRIVATE protobuf::libprotobuf)

    if(UNIX AND NOT APPLE AND BUILD_P2P_DOCKER_TOOLS)
        add_executable(p2p_download_manager_test
            src/download/p2p_download_manager_test.cc
            src/download/p2p_download_manager.cc src/download/p2p_upload_counters.cc
            src/download/network_policy.cc
            src/config/p2p_config_store.cc
            src/logger/logger.cc
        )
        target_include_directories(p2p_download_manager_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_definitions(p2p_download_manager_test PRIVATE
            DEVICE_AGENT_TESTING
        )
        target_link_libraries(p2p_download_manager_test PRIVATE
            Threads::Threads
            OpenSSL::Crypto
            PkgConfig::LIBTORRENT_RASTERBAR
        )

        add_executable(p2p_docker_runner
            src/download/p2p_docker_runner.cc
            src/download/p2p_download_manager.cc src/download/p2p_upload_counters.cc
            src/download/network_policy.cc
            src/config/p2p_config_store.cc
            src/logger/logger.cc
            src/logger/runner_logger.cc
        )
        target_include_directories(p2p_docker_runner PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(p2p_docker_runner PRIVATE
            Threads::Threads
            OpenSSL::Crypto
            PkgConfig::LIBTORRENT_RASTERBAR
        )
    endif()

    # Build examples
    option(BUILD_EXAMPLES "Build example business apps" ON)
    if(BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()
endif()
