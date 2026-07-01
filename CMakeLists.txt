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
# WITH_CUDA — optional GPU acceleration (v0.2 work item #3, Session 39+).
#
# Three-state STRING cache variable: ON / OFF / AUTO (default AUTO).
# CMake's native option() is boolean-only, so we use the STRING-cache-var
# idiom + STRINGS property (approved Session 39 as reconciliation vs proposal
# §3.1's technically-invalid option(... AUTO) syntax; see DECISIONS.md
# 2026-07-04 "GPU port scope (v0.2 work item #3)" Session 39 SHA-pin
# annotation).
#
#   AUTO — find_package(CUDAToolkit QUIET); enable if found, disable otherwise
#   ON   — find_package(CUDAToolkit REQUIRED); fail loud on missing NVCC
#   OFF  — skip CUDA setup entirely; identical build to today's baseline
#
# WITH_CUDA_ENABLED is the effective boolean set from WITH_CUDA + detection
# outcome. It gates every CUDA-related add_library / target_link_libraries
# below.
#
# When WITH_CUDA_ENABLED, we declare cuda_voxelizer_vendor as an INTERFACE
# library (matches the tinyobjloader precedent above; SYSTEM-include so the
# project's -Wall -Wextra -Wpedantic -Wconversion doesn't fire on vendored
# code). Session 39 does NOT compile voxelize.cu — its transitive header
# chain (voxelize.cuh -> util_cuda.h + libs/cuda/helper_math.h; util.h ->
# TriMesh.h) requires shim symbols documented in
# research/gpu_port_proposal.md §3.10 open risk item 6. The shims land in
# Session 40's src/voxel_gpu.cu adapter, alongside voxelize.cu-as-a-source.
# See third_party/cuda_voxelizer/{LICENSE,VERSION.txt} for provenance.
# ---------------------------------------------------------------------------
set(WITH_CUDA "AUTO" CACHE STRING
    "CUDA GPU acceleration: ON (require) / OFF (disable) / AUTO (detect)")
set_property(CACHE WITH_CUDA PROPERTY STRINGS ON OFF AUTO)

if(NOT WITH_CUDA MATCHES "^(ON|OFF|AUTO)$")
    message(FATAL_ERROR
        "WITH_CUDA must be ON, OFF, or AUTO (got \"${WITH_CUDA}\")")
endif()

set(WITH_CUDA_ENABLED OFF)
if(WITH_CUDA STREQUAL "ON")
    find_package(CUDAToolkit REQUIRED)
    set(WITH_CUDA_ENABLED ON)
elseif(WITH_CUDA STREQUAL "AUTO")
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(WITH_CUDA_ENABLED ON)
    endif()
endif()

if(WITH_CUDA_ENABLED)
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    # Volta (SM_70) minimum per proposal §3.1; Ampere (SM_80) and Ada (SM_89)
    # explicit; SM_90+ hosts PTX-JIT upward from SM_89 at runtime.
    set(CMAKE_CUDA_ARCHITECTURES "70;80;89")

    add_library(cuda_voxelizer_vendor INTERFACE)
    target_include_directories(cuda_voxelizer_vendor SYSTEM
        INTERFACE ${CMAKE_SOURCE_DIR}/third_party/cuda_voxelizer)
    target_link_libraries(cuda_voxelizer_vendor INTERFACE CUDA::cudart)

    message(STATUS
        "objtomc: WITH_CUDA=${WITH_CUDA}, effective=${WITH_CUDA_ENABLED}, "
        "CUDA toolkit version=${CUDAToolkit_VERSION}")
else()
    message(STATUS
        "objtomc: WITH_CUDA=${WITH_CUDA}, effective=${WITH_CUDA_ENABLED} "
        "(GPU acceleration disabled; build proceeds CPU-only)")
endif()

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

# ---------------------------------------------------------------------------
# palette — Block Palette DB layer. Public header is pure-stdlib; the loader
# reads two files at runtime:
#   - data/block_palette_db.csv             (derived from MC textures)
#   - third_party/minecraft-data/pc/1.21.5/blocks.json  (vendored MIT)
# Paths are baked into the binary via OBJTOMC_PALETTE_DATA_DIR +
# OBJTOMC_VENDOR_MINECRAFT_DATA_DIR so load_default_palette() works from any
# cwd. The compile defs are PRIVATE so they don't pollute downstream TUs.
# See research/palette_db_proposal.md §3.9 + DECISIONS.md 2026-06-19.
# ---------------------------------------------------------------------------
add_library(palette STATIC src/palette.cpp)
target_include_directories(palette PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(palette PRIVATE
    OBJTOMC_PALETTE_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
    OBJTOMC_VENDOR_MINECRAFT_DATA_DIR="${CMAKE_SOURCE_DIR}/third_party/minecraft-data")

# ---------------------------------------------------------------------------
# matcher — Color Matching layer. Consumes a palette::PaletteDB + mesh::Material
# and produces a chunk::BlockState via Oklab perceptual matching. Header is
# pure-stdlib; sRGB transfer function (IEC 61966-2-1) and Oklab math (Björn
# Ottosson reference, dual public domain / MIT) are inlined in src/matcher.cpp
# with citation comments — no third_party/ entry. See proposal §1.5 +
# DECISIONS.md 2026-06-20 "Color Matching scope (v0.1)".
# ---------------------------------------------------------------------------
add_library(matcher STATIC src/matcher.cpp)
target_include_directories(matcher PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(matcher PUBLIC palette mesh chunk)

# ---------------------------------------------------------------------------
# voxel — Voxelisation layer. Consumes a mesh::Mesh and produces a dense
# VoxelGrid (int32 cells, EMPTY = -1 sentinel) per the Schwarz-Seidel 2010
# §3 conservative surface algorithm. Akenine-Möller 2001 SAT triangle-AABB
# overlap test + Eberly 1999 closest-point-on-triangle for the §3.5
# closest-to-voxel-centre disambiguation rule — all inlined in src/voxel.cpp
# with citation comments; no third_party/ entry. See research/voxel_proposal.md
# §1.5 + DECISIONS.md 2026-06-29 "Voxelisation scope (v0.1)".
# ---------------------------------------------------------------------------
add_library(voxel STATIC src/voxel.cpp)
target_include_directories(voxel PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(voxel PUBLIC mesh)

# ---------------------------------------------------------------------------
# orchestrate — Orchestration layer. Ties OBJ Loading → Voxelisation →
# Color Matching → Chunk Schema → Region File Writing → level.dat into a
# single C++ entry point. In-tree level.dat writer using Session 8's
# nbt::Writer + a zlib deflateInit2(..., 15+16, ...) gzip wrapper for the
# MC level.dat format. No new third_party/ entry; cxxopts + CLI11 both
# audited 2026-07-01 and rejected on code-size grounds (DECISIONS.md
# 2026-07-01 "Orchestration scope (v0.1)").
# ---------------------------------------------------------------------------
add_library(orchestrate STATIC src/orchestrate.cpp)
target_include_directories(orchestrate PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(orchestrate
    PUBLIC  nbt anvil chunk mesh palette matcher voxel
    PRIVATE ZLIB::ZLIB)

# ---------------------------------------------------------------------------
# objtomc — the CLI binary. Hand-rolled argv parser per proposal §1.5(b);
# no cxxopts/CLI11 vendoring.
# ---------------------------------------------------------------------------
add_executable(objtomc src/main.cpp)
target_link_libraries(objtomc PRIVATE orchestrate)

enable_testing()
add_subdirectory(tests)
