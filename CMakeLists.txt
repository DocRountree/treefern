cmake_minimum_required(VERSION 3.16)
project(treefern LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
add_executable(treefern WIN32 MACOSX_BUNDLE main.cpp)  # GUI app: no console window on Windows
target_link_libraries(treefern PRIVATE Qt6::Widgets Qt6::Svg)
if(WIN32)
    target_sources(treefern PRIVATE packaging/treefern.rc)  # .exe icon
endif()

# Deep trees recurse once per level; Windows fixes the stack size at link time (Unix raises it at startup).
if(MSVC)
    target_link_options(treefern PRIVATE /STACK:536870912)
elseif(MINGW)
    target_link_options(treefern PRIVATE -Wl,--stack,536870912)
endif()
