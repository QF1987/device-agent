cmake_minimum_required(VERSION 3.16)

add_executable(rd_vnc_standalone
    main.cpp
)

target_link_libraries(rd_vnc_standalone PRIVATE remote_desktop)
if(WIN32)
    target_link_libraries(rd_vnc_standalone PRIVATE ws2_32)
endif()
