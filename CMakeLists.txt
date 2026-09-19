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
    # C++20 is required so nvcc can parse std::span, which appears in
    # objtomc/voxel.hpp's VoxelGrid::materials() public API. Session 38
    # proposal §3.1 tentatively suggested C++17 "for NVCC compatibility";
    # Session 40 empirical: nvcc 13.0 handles C++20 well (matches the
    # top-level CMAKE_CXX_STANDARD 20), and dropping the CUDA path to 17
    # would require an API-observable retreat from std::span.
    #
    # CMake 3.22 (the version pinned by cmake_minimum_required 3.20)
    # doesn't natively know the CUDA20 dialect — the CUDA_STANDARD 20
    # value wasn't added until CMake 3.25. We teach CMake how to enable
    # CUDA20 by setting the compile-option variables manually, then
    # set(CMAKE_CUDA_STANDARD 20). This produces the same nvcc command
    # line CMake >= 3.25 would generate.
    set(CMAKE_CUDA20_STANDARD_COMPILE_OPTION "-std=c++20")
    set(CMAKE_CUDA20_EXTENSION_COMPILE_OPTION "-std=c++20")
    set(CMAKE_CUDA_STANDARD 20)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    # Turing (SM_75) minimum; Ampere (SM_80) and Ada (SM_89) explicit;
    # SM_90+ hosts PTX-JIT upward from SM_89 at runtime.
    #
    # Session 40 raise-from-70-to-75: proposal §3.1 originally called for
    # SM_70 (Volta) minimum, but CUDA 13.0 dropped Volta support (nvcc
    # --list-gpu-arch on the target host reports SM_75 as the floor). The
    # Volta minimum was an aspirational number in Session 38's proposal;
    # the target host is 8.9 (RTX 4090 D / Ada Lovelace) and no other
    # supported GPU tier lands between 7.0 and 7.5. Documented in
    # DECISIONS.md Session 40 SHA-pin annotation as an empirical raise.
    set(CMAKE_CUDA_ARCHITECTURES "75;80;89")

    # ---------------------------------------------------------------------
    # Session 40 conversion: cuda_voxelizer_vendor is now a STATIC library
    # with voxelize.cu as its source (was INTERFACE in Session 39 while we
    # resolved the transitive header chain — SESSION_LOG.md Session 39
    # STOP+resolve record). The shim symbols the header chain needs live
    # under src/voxel_gpu_shim/ (TriMesh.h stub + util_cuda.h checkCudaErrors
    # + libs/cuda/helper_math.h operators + dot/cross/normalize/fminf/
    # fmaxf/clamp), empirically enumerated during Session 40 pre-impl
    # nvcc test-compile per proposal §3.10 open risk 6 mitigation.
    #
    # Vendored voxelize.cu is compiled with -w to suppress its warnings
    # (upstream code we vendor read-only per the tinyobjloader precedent
    # in DECISIONS.md 2026-06-18 "Vendoring pattern for third-party C++
    # deps"). SYSTEM include suppresses HEADER warnings but not warnings
    # that fire on the vendored .cu source itself (nvcc's cudafe stitching
    # emits -Wpedantic "style of line directive is a GCC extension" on
    # every line — a nvcc peculiarity, not our code).
    # ---------------------------------------------------------------------
    add_library(cuda_voxelizer_vendor STATIC
        ${CMAKE_SOURCE_DIR}/third_party/cuda_voxelizer/voxelize.cu)
    target_include_directories(cuda_voxelizer_vendor SYSTEM
        PUBLIC  ${CMAKE_SOURCE_DIR}/third_party/cuda_voxelizer)
    target_include_directories(cuda_voxelizer_vendor SYSTEM
        PRIVATE ${CMAKE_SOURCE_DIR}/src/voxel_gpu_shim)
    target_link_libraries(cuda_voxelizer_vendor PUBLIC CUDA::cudart)
    # Vendored file must compile clean per the "no source edits to vendored
    # code" discipline. Silence all host-side warnings on the vendored TU
    # only; our own .cu files still enforce -Wall -Wextra -Wpedantic
    # -Wconversion (see the voxel target below).
    target_compile_options(cuda_voxelizer_vendor PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-w>)

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

# ---------------------------------------------------------------------------
# third_party/stb/ — vendored single-header image decoder (dual public
# domain / MIT). Same INTERFACE + SYSTEM-include pattern as tinyobjloader;
# pin + provenance in third_party/stb/VERSION.txt. Linked PRIVATE by the
# texture layer only — no downstream target sees stb_image.h transitively.
# ---------------------------------------------------------------------------
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM
    INTERFACE ${CMAKE_SOURCE_DIR}/third_party/stb)

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
# texture — map_Kd diffuse-texture decode + nearest-neighbour UV sampling
# for per-voxel colour (Session 48, v0.3 item #1). Public header is
# pure-stdlib; stb_image is linked PRIVATE per the mesh/tinyobjloader
# wrapping discipline (proposal §5.9 precedent).
# ---------------------------------------------------------------------------
add_library(texture STATIC src/texture.cpp)
target_include_directories(texture PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(texture PRIVATE stb)

# ---------------------------------------------------------------------------
# third_party/cgltf/ — vendored single-header glTF 2.0 parser (MIT). Same
# INTERFACE + SYSTEM-include pattern as tinyobjloader/stb; pin + provenance
# in third_party/cgltf/VERSION.txt. Linked PRIVATE by the gltf layer only.
# ---------------------------------------------------------------------------
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM
    INTERFACE ${CMAKE_SOURCE_DIR}/third_party/cgltf)

# ---------------------------------------------------------------------------
# gltf — glTF/GLB loading layer (Session 49, v0.3 item #2). Emits the same
# mesh::Mesh model as the mesh layer so all downstream layers are format-
# agnostic. Public header is pure-stdlib; cgltf is linked PRIVATE per the
# wrapping discipline (proposal §5.9 precedent).
# ---------------------------------------------------------------------------
add_library(gltf STATIC src/gltf.cpp)
target_include_directories(gltf PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(gltf PRIVATE cgltf)

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
target_include_directories(matcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(matcher PUBLIC palette mesh chunk)

# ---------------------------------------------------------------------------
# GPU matcher path — Session 41 (v0.2 work item #3, implementation slice 3).
#
# When WITH_CUDA_ENABLED, add src/matcher_gpu.cu as an additional source to
# the matcher library and link CUDA::cudart. OBJTOMC_WITH_CUDA is defined
# PUBLIC on the matcher library so callers see the guarded prefer_gpu field
# on MatcherOptions when they include objtomc/matcher.hpp.
#
# --fmad=false MUST be applied to matcher_gpu.cu for byte-equality — the
# Oklab arithmetic (sRGB decode + linear→Oklab + score computation) is
# heavily multiply-add; nvcc's default FMA emission diverges from GCC on
# host at low-order bits. Session 40 SHA-pin (DECISIONS.md 2026-07-04)
# documents the same finding for the voxelizer. --fmad=false is a
# per-.cu-file setting; we apply it via target_compile_options at the
# matcher library level so both this file and any future .cu additions
# inherit it.
# ---------------------------------------------------------------------------
if(WITH_CUDA_ENABLED)
    target_sources(matcher PRIVATE src/matcher_gpu.cu)
    target_link_libraries(matcher PRIVATE CUDA::cudart)
    target_compile_definitions(matcher PUBLIC OBJTOMC_WITH_CUDA=1)
    target_compile_options(matcher PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra,-Wpedantic,-Wconversion>
        $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>)
endif()

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
target_include_directories(voxel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(voxel PUBLIC mesh)

# ---------------------------------------------------------------------------
# GPU voxelizer path — Session 40 (v0.2 work item #3, implementation slice 2).
#
# When WITH_CUDA_ENABLED, add src/voxel_gpu.cu as an additional source to
# the voxel library and link the vendored cuda_voxelizer_vendor + CUDA
# runtime. OBJTOMC_WITH_CUDA is defined PUBLIC on the voxel library so
# transitively-consuming targets (matcher, orchestrate, tests, objtomc CLI)
# see the GPU-guarded fields on VoxelizerOptions when they include
# objtomc/voxel.hpp.
#
# The shim include path (src/voxel_gpu_shim/) is added SYSTEM so shim
# warnings don't fire under our -Wall -Wextra -Wpedantic -Wconversion
# posture. Our own src/voxel_gpu.cu compiles under strict flags — see
# target_compile_options below (nvcc forwards to host via -Xcompiler=...).
# ---------------------------------------------------------------------------
if(WITH_CUDA_ENABLED)
    target_sources(voxel PRIVATE src/voxel_gpu.cu)
    target_include_directories(voxel SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/src/voxel_gpu_shim
        ${CMAKE_SOURCE_DIR}/third_party/cuda_voxelizer)
    target_link_libraries(voxel PRIVATE cuda_voxelizer_vendor CUDA::cudart)
    target_compile_definitions(voxel PUBLIC OBJTOMC_WITH_CUDA=1)
    target_compile_options(voxel PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra,-Wpedantic,-Wconversion>
        # Disable nvcc's default fused multiply-add generation. nvcc emits
        # FMA for `a * b + c` patterns, GCC on host does not by default
        # (no -mfma). The single-rounding vs double-rounding difference
        # tips CPU-vs-GPU byte-equality on close-tied disambiguation
        # comparisons at high resolutions (empirically detected Session 40:
        # 64^3 byte-identical, 256^3 diverges at ~1 voxel per 500k because
        # multiple triangles have distances within an FMA-rounding of one
        # another). --fmad=false gives IEEE-754 mult-then-add semantics on
        # device, matching host. See proposal §3.10 risk 1.
        $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>)
endif()

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
    PRIVATE gltf texture ZLIB::ZLIB)

# ---------------------------------------------------------------------------
# objtomc — the CLI binary. Hand-rolled argv parser per proposal §1.5(b);
# no cxxopts/CLI11 vendoring.
# ---------------------------------------------------------------------------
add_executable(objtomc src/main.cpp)
target_link_libraries(objtomc PRIVATE orchestrate)

enable_testing()
add_subdirectory(tests)
