cmake_minimum_required(VERSION 3.16)

add_executable(rd_vnc_standalone
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/remotedesktop/test/test_pattern.cc
    ${CMAKE_SOURCE_DIR}/src/remotedesktop/tunnel/tunnel_client.cc
)

target_link_libraries(rd_vnc_standalone PRIVATE remote_desktop Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(rd_vnc_standalone PRIVATE ws2_32)
endif()
