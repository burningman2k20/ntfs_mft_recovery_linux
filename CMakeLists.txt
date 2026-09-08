cmake_minimum_required(VERSION 3.16)
project(ntfs_recovery LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(ntfs_recovery
    src/main.cpp
    src/mft_parser.hpp
    src/mft_parser.cpp
    src/recoverer.hpp
    src/recoverer.cpp
    src/mainwindow.hpp
    src/mainwindow.cpp
    src/ntfs_types.hpp
)

target_link_libraries(ntfs_recovery PRIVATE Qt6::Widgets)