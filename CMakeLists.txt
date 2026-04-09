cmake_minimum_required(VERSION 3.16)
project(device-agent CXX)

# C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Directory structure
include(GNUInstallDirs)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Source files
set(SOURCES
    src/main.cpp
    src/client/device_client.cc
    src/config/config.cc
    src/logger/logger.cc
)

# Header files
set(HEADERS
    src/client/device_client.h
    src/client/command_handler.h
    src/config/config.h
    src/logger/logger.h
)

add_executable(device-agent ${SOURCES} ${HEADERS})

# gRPC and Protobuf
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

target_include_directories(device-agent
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/gen
)

target_link_libraries(device-agent
    PRIVATE
        protobuf::libprotobuf
        grpc++_unsecure
        grpc++_reflection
)

# Proto generation
 protobuf_generate(TARGET device-agent PROTOS
    proto/terminal_agent/v1/device.proto
    proto/terminal_agent/v1/service.proto
    PROTOC_OUT_DIR ${CMAKE_SOURCE_DIR}/gen
)

# Copy generated files to gen/ after build
add_custom_command(TARGET device-agent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/gen/terminal_agent
    ${CMAKE_BINARY_DIR}/gen/terminal_agent
    COMMENT "Copying generated proto files to build dir"
)
