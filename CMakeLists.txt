cmake_minimum_required(VERSION 3.16)
project(device-agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEVICE_AGENT_VERSION "0.0.0-dev" CACHE STRING
    "DeviceAgent SemVer without a leading v")
option(DEVICE_AGENT_RELEASE_BUILD
    "Require an explicit non-development DeviceAgent version" OFF)
if(NOT DEVICE_AGENT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([.-][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR
        "DEVICE_AGENT_VERSION must be SemVer without a leading v; got '${DEVICE_AGENT_VERSION}'")
endif()
if(DEVICE_AGENT_RELEASE_BUILD AND DEVICE_AGENT_VERSION STREQUAL "0.0.0-dev")
    message(FATAL_ERROR
        "Release/test package builds must set -DDEVICE_AGENT_VERSION=<semver>")
endif()
add_compile_definitions(DEVICE_AGENT_VERSION="${DEVICE_AGENT_VERSION}")

include(GNUInstallDirs)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

function(find_device_agent_protobuf)
    find_package(protobuf CONFIG QUIET)
    if(NOT protobuf_FOUND)
        find_package(Protobuf REQUIRED)
    endif()
endfunction()

set(COMMON_SOURCES
    src/main.cpp
    src/client/device_client.cc
    src/capability/capability_manifest.cc
    src/client/network_info.cc
    src/observability/observability.cc
    src/observability/windows_observability.cc
    src/client/command_handler.cc
    src/config/config.cc
    src/enrollment/enrollment_manager.cc
    src/config/p2p_config_store.cc
    src/logger/logger.cc
    src/bridge/bridge.cc
    src/executor/linux_executor.cc
    src/executor/macos_executor.cc
    src/executor/installer_classifier.cc
    src/reboot_state/reboot_state.cc
    src/download/android_download_manager.cc
    src/download/noop_download_manager.cc
    src/download/network_policy.cc
    src/download/p2p_upload_counters.cc
)

# Windows 下载管理器（WinHTTP + BCrypt 兜底层）。仅在 Windows 编译，
# 其它平台不引入。
if(WIN32)
    list(APPEND COMMON_SOURCES src/download/windows_download_manager.cc)
endif()

set(COMMON_HEADERS
    src/client/device_client.h
    src/capability/capability_manifest.h
    src/client/network_info.h
    src/observability/observability.h
    src/observability/windows_observability.h
    src/version/build_info.h
    src/client/command_handler.h
    src/config/config.h
    src/enrollment/enrollment_manager.h
    src/config/p2p_config_store.h
    src/logger/logger.h
    src/executor/executor.h
    src/executor/installer_classifier.h
    src/download/idownload_manager.h
    src/download/android_download_manager.h
    src/download/noop_download_manager.h
    src/download/network_policy.h
    src/reboot_state/reboot_state.h
    src/jni_bridge.h
)

if(WIN32)
    list(APPEND COMMON_HEADERS src/download/windows_download_manager.h)
endif()

set(REMOTE_DESKTOP_SOURCES
    src/remotedesktop/screen_capturer.cc
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
        src/remotedesktop/platform/windows/windows_session_launcher.cc
    )
    list(APPEND REMOTE_DESKTOP_HEADERS
        src/remotedesktop/platform/windows/windows_badge.h
        src/remotedesktop/platform/windows/windows_input_injector.h
        src/remotedesktop/platform/windows/windows_screen_capturer.h
        src/remotedesktop/platform/windows/windows_session_launcher.h
    )
endif()

add_library(remote_desktop STATIC
    ${REMOTE_DESKTOP_SOURCES}
    ${REMOTE_DESKTOP_HEADERS}
)
target_include_directories(remote_desktop PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_compile_definitions(remote_desktop PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(remote_desktop PUBLIC /utf-8)
    target_link_libraries(remote_desktop PUBLIC user32 gdi32 dxgi d3d11)

    add_executable(gdi_capture_probe
        src/remotedesktop/platform/windows/gdi_capture_probe.cc
    )
    target_link_libraries(gdi_capture_probe PRIVATE remote_desktop)
endif()

if(NOT ANDROID AND NOT __ANDROID__)
    add_executable(rfb_protocol_test
        src/remotedesktop/rfb/rfb_protocol_test.cc
    )
    target_link_libraries(rfb_protocol_test PRIVATE remote_desktop)
    if(MSVC)
        target_compile_options(rfb_protocol_test PRIVATE
            $<$<CONFIG:Release>:/UNDEBUG>)
    else()
        target_compile_options(rfb_protocol_test PRIVATE
            $<$<CONFIG:Release>:-UNDEBUG>)
    endif()
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
    find_device_agent_protobuf()

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

elseif(WIN32)
    message(STATUS "=== Building device-agent Windows remote desktop targets ===")
    find_package(Threads REQUIRED)

    add_library(remote_desktop_runtime STATIC
        src/remotedesktop/remote_desktop_runtime.cc
        src/remotedesktop/tunnel/tunnel_client.cc
    )
    target_include_directories(remote_desktop_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(remote_desktop_runtime PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(remote_desktop_runtime PUBLIC /utf-8)
    target_link_libraries(remote_desktop_runtime PUBLIC
        remote_desktop
        ws2_32
        secur32
        crypt32
        userenv
        wtsapi32
    )

    option(BUILD_EXAMPLES "Build example business apps" ON)
    if(BUILD_EXAMPLES)
        add_subdirectory(examples/rd_vnc_standalone)
    endif()

    option(DEVICE_AGENT_BUILD_WINDOWS_AGENT "Build full Windows device-agent executable" OFF)
    if(DEVICE_AGENT_BUILD_WINDOWS_AGENT)
        find_device_agent_protobuf()
        find_package(gRPC CONFIG REQUIRED)

        set(GEN_WINDOWS_DIR "${CMAKE_BINARY_DIR}/gen-windows")
        set(WINDOWS_PROTO_ROOT "${CMAKE_BINARY_DIR}/proto-src")
        set(PROTO_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/proto")
        set(PROTO_SOURCE_ROOT_COMMAND "${PROTO_SOURCE_ROOT}")
        if(PROTO_SOURCE_ROOT_COMMAND MATCHES "^/Mac/Home/")
            string(REGEX REPLACE "^/Mac/Home/" "//Mac/Home/" PROTO_SOURCE_ROOT_COMMAND "${PROTO_SOURCE_ROOT_COMMAND}")
        endif()
        set(PROTO_DEVICE_SOURCE "${PROTO_SOURCE_ROOT}/terminal_agent/v1/device.proto")
        set(PROTO_SERVICE_SOURCE "${PROTO_SOURCE_ROOT}/terminal_agent/v1/service.proto")
        set(PROTO_DEVICE "${WINDOWS_PROTO_ROOT}/terminal_agent/v1/device.proto")
        set(PROTO_SERVICE "${WINDOWS_PROTO_ROOT}/terminal_agent/v1/service.proto")
        file(TO_NATIVE_PATH "${WINDOWS_PROTO_ROOT}" PROTO_ROOT_NATIVE)
        file(TO_NATIVE_PATH "${PROTO_DEVICE}" PROTO_DEVICE_NATIVE)
        file(TO_NATIVE_PATH "${PROTO_SERVICE}" PROTO_SERVICE_NATIVE)
        if(TARGET gRPC::grpc_cpp_plugin)
            set(GRPC_CPP_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
        else()
            set(GRPC_CPP_PLUGIN
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/grpc/grpc_cpp_plugin.exe")
        endif()
        set(WINDOWS_PROTO_SRCS
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/device.pb.cc"
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/device.grpc.pb.cc"
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/service.pb.cc"
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/service.grpc.pb.cc"
        )
        set(WINDOWS_PROTO_HDRS
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/device.pb.h"
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/device.grpc.pb.h"
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/service.pb.h"
            "${GEN_WINDOWS_DIR}/terminal_agent/v1/service.grpc.pb.h"
        )
        add_custom_command(
            OUTPUT ${WINDOWS_PROTO_SRCS} ${WINDOWS_PROTO_HDRS}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_WINDOWS_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROTO_SOURCE_ROOT_COMMAND}"
                "${WINDOWS_PROTO_ROOT}"
            COMMAND ${Protobuf_PROTOC_EXECUTABLE}
                --proto_path=${PROTO_ROOT_NATIVE}
                --cpp_out=${GEN_WINDOWS_DIR}
                --grpc_out=${GEN_WINDOWS_DIR}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
                "${PROTO_DEVICE_NATIVE}"
                "${PROTO_SERVICE_NATIVE}"
            DEPENDS "${PROTO_DEVICE_SOURCE}" "${PROTO_SERVICE_SOURCE}"
            COMMENT "Generating Windows protobuf/gRPC sources"
            VERBATIM
        )

        set(WINDOWS_AGENT_SOURCES ${COMMON_SOURCES})
        list(FILTER WINDOWS_AGENT_SOURCES EXCLUDE REGEX "src/executor/linux_executor.cc$")
        list(FILTER WINDOWS_AGENT_SOURCES EXCLUDE REGEX "src/executor/macos_executor.cc$")
        list(FILTER WINDOWS_AGENT_SOURCES EXCLUDE REGEX "src/download/android_download_manager.cc$")
        list(APPEND WINDOWS_AGENT_SOURCES
            src/executor/windows_executor.cc
            ${WINDOWS_PROTO_SRCS}
        )

        add_executable(device-agent
            ${WINDOWS_AGENT_SOURCES}
            ${COMMON_HEADERS}
        )
        target_include_directories(device-agent PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/include
            ${GEN_WINDOWS_DIR}
        )
        target_compile_definitions(device-agent PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(device-agent PRIVATE /utf-8)
        target_link_libraries(device-agent PRIVATE
            remote_desktop
            remote_desktop_runtime
            Threads::Threads
            protobuf::libprotobuf
            gRPC::grpc++
            ws2_32
            secur32
            crypt32
            userenv
            wtsapi32
            winhttp
            bcrypt
            iphlpapi
            advapi32
        )

        # ADR-20260831-01 B2：Windows production P2P（混合 manager + WinHTTP
        # fallback + runtime p2p_config 动态声明）。默认 OFF——既有 Windows
        # build/package 不新增 libtorrent 依赖；ON 时 production exe 链接
        # libtorrent/OpenSSL 并编译混合 manager（B3 kit closure 显式校验
        # torrent-rasterbar.dll + libcrypto-3-x64.dll）。与 Phase A 测试 option
        # DEVICE_AGENT_BUILD_WINDOWS_P2P 语义互不相干。
        option(DEVICE_AGENT_ENABLE_WINDOWS_P2P
            "Enable Windows production P2P hybrid download manager" OFF)
        if(DEVICE_AGENT_ENABLE_WINDOWS_P2P)
            find_package(OpenSSL REQUIRED)
            find_package(LibtorrentRasterbar CONFIG REQUIRED)
            target_sources(device-agent PRIVATE
                src/download/p2p_download_manager.cc
                src/download/windows_p2p_download_manager.cc
            )
            target_compile_definitions(device-agent PRIVATE
                DEVICE_AGENT_ENABLE_WINDOWS_P2P)
            target_link_libraries(device-agent PRIVATE
                LibtorrentRasterbar::torrent-rasterbar
                OpenSSL::Crypto
            )
        endif()

        add_executable(windows_observability_test
            src/observability/windows_observability_test.cc
            src/observability/observability.cc
            src/observability/windows_observability.cc
            src/download/p2p_upload_counters.cc
            src/download/network_policy.cc
            src/enrollment/enrollment_manager.cc
            src/config/config.cc
            src/logger/logger.cc
            ${WINDOWS_PROTO_SRCS}
        )
        target_include_directories(windows_observability_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${GEN_WINDOWS_DIR}
        )
        target_compile_definitions(windows_observability_test PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(windows_observability_test PRIVATE
            /utf-8
            $<$<CONFIG:Release>:/UNDEBUG>
        )
        target_link_libraries(windows_observability_test PRIVATE
            Threads::Threads
            protobuf::libprotobuf
            gRPC::grpc++
            ws2_32
            iphlpapi
            bcrypt
            advapi32
        )

        add_executable(capability_manifest_test
            src/capability/capability_manifest_test.cc
            src/capability/capability_manifest.cc
            ${WINDOWS_PROTO_SRCS}
        )
        target_include_directories(capability_manifest_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${GEN_WINDOWS_DIR}
        )
        target_compile_options(capability_manifest_test PRIVATE
            /utf-8
            $<$<CONFIG:Release>:/UNDEBUG>
        )
        target_link_libraries(capability_manifest_test PRIVATE
            protobuf::libprotobuf
            gRPC::grpc++
        )
    endif()

    # Windows P2P Phase A：libtorrent P2PDownloadManager 原生编译 + 离线测试。
    # 仅测试目标；不接入生产 device-agent（仍用 WindowsDownloadManager），
    # 不改变 capability manifest（Windows 不声明 p2p_config）。
    # 依赖 vcpkg x64-windows：vcpkg install libtorrent（传递 openssl/boost）。
    # config package/target 名 = vcpkg libtorrent port / libtorrent 上游 CMake
    # 实际导出（RV-20260831-WIN-P2P-A-01）：LibtorrentRasterbar /
    # LibtorrentRasterbar::torrent-rasterbar；ABI 定义（TORRENT_LINKING_SHARED、
    # TORRENT_USE_OPENSSL 等）由 imported target 传播，不做手工 ABI override。
    option(DEVICE_AGENT_BUILD_WINDOWS_P2P
        "Build Windows libtorrent P2P core + native offline tests (Phase A)" OFF)
    if(DEVICE_AGENT_BUILD_WINDOWS_P2P)
        find_package(OpenSSL REQUIRED)
        find_package(LibtorrentRasterbar CONFIG REQUIRED)

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
            WIN32_LEAN_AND_MEAN NOMINMAX
        )
        target_compile_options(p2p_download_manager_test PRIVATE
            /utf-8
            $<$<CONFIG:Release>:/UNDEBUG>
        )
        target_link_libraries(p2p_download_manager_test PRIVATE
            Threads::Threads
            OpenSSL::Crypto
            LibtorrentRasterbar::torrent-rasterbar
        )

        add_executable(network_policy_test
            src/download/network_policy_test.cc
            src/download/network_policy.cc
        )
        target_include_directories(network_policy_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_options(network_policy_test PRIVATE
            /utf-8
            $<$<CONFIG:Release>:/UNDEBUG>
        )

        add_executable(p2p_config_store_test
            src/config/p2p_config_store_test.cc
            src/config/p2p_config_store.cc
        )
        target_include_directories(p2p_config_store_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_options(p2p_config_store_test PRIVATE
            /utf-8
            $<$<CONFIG:Release>:/UNDEBUG>
        )

        # Windows 混合 manager（ADR-20260831-01 B1）：核心路由平台中立，
        # 生产 WinHTTP 适配经 windows_download_manager.cc 引入。
        add_executable(windows_p2p_download_manager_test
            src/download/windows_p2p_download_manager_test.cc
            src/download/windows_p2p_download_manager.cc
            src/download/windows_download_manager.cc
            src/download/p2p_download_manager.cc src/download/p2p_upload_counters.cc
            src/download/network_policy.cc
            src/config/p2p_config_store.cc
            src/logger/logger.cc
        )
        target_include_directories(windows_p2p_download_manager_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_definitions(windows_p2p_download_manager_test PRIVATE
            DEVICE_AGENT_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX
        )
        target_compile_options(windows_p2p_download_manager_test PRIVATE
            /utf-8
            $<$<CONFIG:Release>:/UNDEBUG>
        )
        target_link_libraries(windows_p2p_download_manager_test PRIVATE
            Threads::Threads
            OpenSSL::Crypto
            LibtorrentRasterbar::torrent-rasterbar
            ws2_32
        )

        # ADR-20260901-01 B5-S1：独立 seeding owner 离线测试（Windows 原生
        # gate 复用；owner 不链接前台 p2p_download_manager.cc）。
        add_executable(p2p_seeding_owner_test
            src/download/p2p_seeding_owner_test.cc
            src/download/p2p_seeding_owner.cc src/download/p2p_upload_counters.cc
            src/download/network_policy.cc
            src/config/p2p_config_store.cc
            src/logger/logger.cc
        )
        target_include_directories(p2p_seeding_owner_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_definitions(p2p_seeding_owner_test PRIVATE
            DEVICE_AGENT_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX
        )
        target_compile_options(p2p_seeding_owner_test PRIVATE
            /utf-8
            $<$<CONFIG:Release>:/UNDEBUG>
        )
        target_link_libraries(p2p_seeding_owner_test PRIVATE
            Threads::Threads
            OpenSSL::Crypto
            LibtorrentRasterbar::torrent-rasterbar
            ws2_32
        )
    endif()

# ─── Desktop (Linux/macOS): build as standalone executable ─
else()
    message(STATUS "=== Building device-agent for desktop ===")
    option(BUILD_P2P_DOCKER_TOOLS "Build Linux P2P Docker spike tools" ON)

    find_package(Threads REQUIRED)
    find_device_agent_protobuf()
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

    add_executable(capability_manifest_test
        src/capability/capability_manifest_test.cc
        src/capability/capability_manifest.cc
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.pb.cc"
    )
    target_include_directories(capability_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GEN_DESKTOP_DIR}
    )
    target_compile_options(capability_manifest_test PRIVATE
        $<$<CONFIG:Release>:-UNDEBUG>
    )
    target_link_libraries(capability_manifest_test PRIVATE protobuf::libprotobuf)

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

    add_executable(windows_observability_test
        src/observability/windows_observability_test.cc
        src/observability/observability.cc
        src/observability/windows_observability.cc
        src/download/p2p_upload_counters.cc
        src/download/network_policy.cc
        src/enrollment/enrollment_manager.cc
        src/config/config.cc
        src/logger/logger.cc
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.grpc.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/service.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/service.grpc.pb.cc"
    )
    target_include_directories(windows_observability_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GEN_DESKTOP_DIR}
    )
    # RV-20260901-WIN-P2P-B2-03：非 MSVC Release 也保留 assert（target-local），
    # production device-agent flags 不变。
    target_compile_options(windows_observability_test PRIVATE
        $<$<CONFIG:Release>:-UNDEBUG>
    )
    target_link_libraries(windows_observability_test PRIVATE
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
    )

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
        src/executor/installer_classifier.cc
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

    add_executable(installer_classifier_test
        src/executor/installer_classifier_test.cc
        src/executor/installer_classifier.cc
    )
    target_include_directories(installer_classifier_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    add_executable(enrollment_manager_test
        src/enrollment/enrollment_manager_test.cc
        src/enrollment/enrollment_manager.cc
        src/config/config.cc
        src/logger/logger.cc
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/device.grpc.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/service.pb.cc"
        "${GEN_DESKTOP_DIR}/terminal_agent/v1/service.grpc.pb.cc"
    )
    target_include_directories(enrollment_manager_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${GEN_DESKTOP_DIR}
    )
    target_link_libraries(enrollment_manager_test PRIVATE
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
    )

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

        # Windows 混合 manager 核心路由为平台中立（ADR-20260831-01 B1），
        # Linux 侧同样离线跑路由/回退/guard 逻辑（生产 WinHTTP 适配仅 _WIN32）。
        add_executable(windows_p2p_download_manager_test
            src/download/windows_p2p_download_manager_test.cc
            src/download/windows_p2p_download_manager.cc
            src/download/p2p_download_manager.cc src/download/p2p_upload_counters.cc
            src/download/network_policy.cc
            src/config/p2p_config_store.cc
            src/logger/logger.cc
        )
        target_include_directories(windows_p2p_download_manager_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_definitions(windows_p2p_download_manager_test PRIVATE
            DEVICE_AGENT_TESTING
        )
        target_link_libraries(windows_p2p_download_manager_test PRIVATE
            Threads::Threads
            OpenSSL::Crypto
            PkgConfig::LIBTORRENT_RASTERBAR
        )

        # ADR-20260901-01 B5-S1：独立 seeding owner（seed-only session）离线
        # 测试。owner 不链接前台 p2p_download_manager.cc（live_policy 直接读
        # P2PConfigStore；前台注入 seam 属 B5-S2）。
        add_executable(p2p_seeding_owner_test
            src/download/p2p_seeding_owner_test.cc
            src/download/p2p_seeding_owner.cc src/download/p2p_upload_counters.cc
            src/download/network_policy.cc
            src/config/p2p_config_store.cc
            src/logger/logger.cc
        )
        target_include_directories(p2p_seeding_owner_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_definitions(p2p_seeding_owner_test PRIVATE
            DEVICE_AGENT_TESTING
        )
        target_compile_options(p2p_seeding_owner_test PRIVATE
            $<$<CONFIG:Release>:-UNDEBUG>
        )
        target_link_libraries(p2p_seeding_owner_test PRIVATE
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
