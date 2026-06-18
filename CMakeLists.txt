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

# ---------------------------------------------------------------------------
# third_party/tinyobjloader/ — vendored single-header OBJ/MTL parser (MIT).
# Vendoring contract: third_party/README.md. Pin + provenance:
# third_party/tinyobjloader/VERSION.txt.
#
# Exposed as a header-only INTERFACE library per the vendoring pattern from
# research/obj_loading_proposal.md §2.8 (DECISIONS.md 2026-06-18 "Vendoring
# pattern for third-party C++ deps"). The mesh layer links against it
# PRIVATE so downstream code (anvil, chunk, objtomc, tests) never gets
# tinyobjloader's headers transitively — wrapping discipline from proposal
# §5.9.
#
# We mark the include directory SYSTEM so the project-wide
# -Wall -Wextra -Wpedantic -Wconversion flags do NOT apply to warnings
# emitted from inside tiny_obj_loader.h. tinyobjloader is upstream code we
# vendor read-only (third_party/README.md "Modification policy"); silencing
# warnings on it is correct — silencing them on our own code would not be.
# ---------------------------------------------------------------------------
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader SYSTEM
    INTERFACE ${CMAKE_SOURCE_DIR}/third_party/tinyobjloader)

add_library(nbt STATIC src/nbt.cpp)
target_include_directories(nbt PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_library(anvil STATIC src/anvil.cpp)
target_link_libraries(anvil PUBLIC nbt PRIVATE ZLIB::ZLIB)

add_library(chunk STATIC src/chunk.cpp)
target_link_libraries(chunk PUBLIC nbt)

# ---------------------------------------------------------------------------
# mesh — OBJ loading layer. Public header is pure-stdlib (no tinyobjloader
# leakage); tinyobjloader is linked PRIVATE so anvil/chunk/objtomc/tests
# do not transitively see it. See proposal §5.9.
# ---------------------------------------------------------------------------
add_library(mesh STATIC src/mesh.cpp)
target_include_directories(mesh PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(mesh PRIVATE tinyobjloader)

add_executable(objtomc src/main.cpp)
target_link_libraries(objtomc PRIVATE ZLIB::ZLIB nbt anvil chunk mesh)

enable_testing()
add_subdirectory(tests)
