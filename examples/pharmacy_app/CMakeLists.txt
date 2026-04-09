cmake_minimum_required(VERSION 3.16)

add_executable(pharmacy_app
    main.cpp
    pharmacy_app.h
)

target_link_libraries(pharmacy_app pthread)
