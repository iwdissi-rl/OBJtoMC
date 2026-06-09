cmake_minimum_required(VERSION 3.20)
project(objtomc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(ZLIB REQUIRED)

add_library(nbt STATIC src/nbt.cpp)
target_include_directories(nbt PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_library(anvil STATIC src/anvil.cpp)
target_link_libraries(anvil PUBLIC nbt PRIVATE ZLIB::ZLIB)

add_library(chunk STATIC src/chunk.cpp)
target_link_libraries(chunk PUBLIC nbt)

add_executable(objtomc src/main.cpp)
target_link_libraries(objtomc PRIVATE ZLIB::ZLIB nbt anvil chunk)

enable_testing()
add_subdirectory(tests)
