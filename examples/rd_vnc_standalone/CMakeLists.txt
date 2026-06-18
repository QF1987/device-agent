cmake_minimum_required(VERSION 3.16)

add_executable(rd_vnc_standalone
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/logger/logger.cc
    ${CMAKE_SOURCE_DIR}/src/remotedesktop/test/test_pattern.cc
    ${CMAKE_SOURCE_DIR}/src/remotedesktop/tunnel/tunnel_client.cc
)

if(WIN32)
    target_link_libraries(rd_vnc_standalone PRIVATE remote_desktop Threads::Threads ws2_32 secur32 crypt32)
else()
    target_link_libraries(rd_vnc_standalone PRIVATE remote_desktop Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
endif()
