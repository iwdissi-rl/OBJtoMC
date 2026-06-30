add_executable(test_toolchain test_toolchain.cpp)
target_link_libraries(test_toolchain PRIVATE ZLIB::ZLIB)
add_test(NAME toolchain_smoke COMMAND test_toolchain)

add_executable(test_nbt_worked_example test_nbt_worked_example.cpp)
target_link_libraries(test_nbt_worked_example PRIVATE nbt)
add_test(NAME nbt_worked_example COMMAND test_nbt_worked_example)

# Independent Python oracle (twoolie/NBT, pinned 1.5.1).  Parses the C++
# writer's --dump output back to a logical tree and asserts it matches the
# proposal §5 contract.  See tests/oracle/test_nbt_worked_example.py.
add_test(
    NAME nbt_python_oracle
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_nbt_worked_example>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_nbt_worked_example.py
)
set_tests_properties(nbt_python_oracle PROPERTIES LABELS "oracle")

# Complete tag-type coverage: one entry per spec tag id 1..12 plus TAG_End
# terminators on the wire, exercising all 13 NBT tag types in one tree.
# Logical verification lives in the Python oracle; the C++ side only smoke-checks
# the envelope bytes (no hand-computed 13-tag fixture).
add_executable(test_nbt_complete_coverage test_nbt_complete_coverage.cpp)
target_link_libraries(test_nbt_complete_coverage PRIVATE nbt)
add_test(NAME nbt_complete_coverage COMMAND test_nbt_complete_coverage)

add_test(
    NAME nbt_complete_coverage_oracle
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_nbt_complete_coverage>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_nbt_complete_coverage.py
)
set_tests_properties(nbt_complete_coverage_oracle PROPERTIES LABELS "oracle")

# Anvil framing layer (Session 10).
#
# Tier-1: pure C++ smoke check that an empty RegionWriter produces a
# valid 8192-byte all-zero region (location table + timestamp table,
# no chunk-data sectors).
add_executable(test_anvil_empty_region test_anvil_empty_region.cpp)
target_link_libraries(test_anvil_empty_region PRIVATE anvil)
add_test(NAME anvil_empty_region COMMAND test_anvil_empty_region)

# Tier-2: helper binary that reads decompressed NBT bytes for one chunk
# from a file, writes them through our RegionWriter to an .mca, and
# verifies the output is 4 KiB-aligned. Driven by the Python oracle
# below; not registered as a CTest itself (the oracle invokes it).
add_executable(test_anvil_single_chunk_writer
               test_anvil_single_chunk_writer.cpp)
target_link_libraries(test_anvil_single_chunk_writer PRIVATE anvil)

# Tier-2 oracle: extract chunk (0,0) raw NBT bytes from the MC 1.21.5
# fixture, round-trip them through our writer, then verify with
# anvil-parser==0.9.0 (independent reader) that the logical fields
# (DataVersion, xPos, yPos, zPos, Status, len(sections)) match.
# See research/anvil_api_proposal.md §3.10 tier 2.
add_test(
    NAME anvil_single_chunk_roundtrip
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_anvil_single_chunk_writer>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_anvil_single_chunk_roundtrip.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(anvil_single_chunk_roundtrip PROPERTIES LABELS "oracle")

# Tier-3 (Session 13): helper binary that reads a tab-separated manifest
# of (chunk_x, chunk_z, nbt_path) entries, pre-loads every chunk's bytes,
# builds ONE RegionWriter(0, 0), and emits the full multi-chunk region.
# Driven by the Python oracle below; not registered as a CTest itself.
# Wall-clock timing inside the binary covers add_chunk + write() only.
add_executable(test_anvil_full_region_writer
               test_anvil_full_region_writer.cpp)
target_link_libraries(test_anvil_full_region_writer PRIVATE anvil)

# Tier-3 oracle: extract every populated chunk's raw NBT from the MC
# 1.21.5 fixture (960 chunks per research/anvil_ground_truth.md),
# round-trip the whole set through our writer, then assert logical
# equivalence chunk-by-chunk via anvil-parser==0.9.0 + twoolie/NBT.
# Stress test for multi-chunk allocation; not a risk gate (Tier 4 has
# already verified format compatibility against vanilla MC 1.21.5 — see
# DECISIONS.md 2026-06-08).
add_test(
    NAME anvil_full_region_roundtrip
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_FULL_REGION_BINARY=$<TARGET_FILE:test_anvil_full_region_writer>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_anvil_full_region_roundtrip.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(anvil_full_region_roundtrip PROPERTIES LABELS "oracle")

# Chunk schema layer (Session 15a — all-air path only).
#
# Single binary, two test entries:
#   chunk_all_air_error_paths : run with no args; checks that empty biome_id,
#     to_bytes-before-build, and double-build all throw chunk_error.
#   chunk_all_air_oracle      : Python oracle drives the binary with
#     --out <tempfile>, parses with twoolie/NBT==1.5.1 (NOT anvil-parser;
#     anvil-parser 0.9.0 doesn't support the 1.21 flat-root chunk format —
#     research/chunk_schema_proposal.md §3.12 (c)), asserts the §3.12 (a)
#     all-air structural contract.
add_executable(test_chunk_all_air test_chunk_all_air.cpp)
target_link_libraries(test_chunk_all_air PRIVATE chunk)
add_test(NAME chunk_all_air_error_paths COMMAND test_chunk_all_air)

add_test(
    NAME chunk_all_air_oracle
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_chunk_all_air>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_chunk_all_air_oracle.py
)
set_tests_properties(chunk_all_air_oracle PROPERTIES LABELS "oracle")

# Session 15b — single-stone byte-pattern test (§3.12.b).
#
# Helper binary dumps a chunk with one minecraft:stone at world (8, -61, 8),
# everything else air. The Python oracle parses the output with twoolie/NBT
# and asserts the §3.12.b contract: 2-entry palette [air, stone] in section
# Y=-4, bit-packed data field of length 256 longs, with data[56] equal to
# 0x0000_0001_0000_0000 (slot 8 of a 4-bits-per-block long, holding the
# palette index 1 for stone). All other sections (Y=-3..19) remain all-air.
# This is the bit-packing contract from research/chunk_schema_proposal.md
# §3.4 — get it wrong and MC silently rejects chunks.
add_executable(test_chunk_single_stone test_chunk_single_stone.cpp)
target_link_libraries(test_chunk_single_stone PRIVATE chunk)

add_test(
    NAME chunk_single_stone_oracle
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_chunk_single_stone>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_chunk_single_stone_oracle.py
)
set_tests_properties(chunk_single_stone_oracle PROPERTIES LABELS "oracle")

# Session 15c — heightmap predicate-scan stress test (§3.5 + §3.12).
#
# Helper binary builds a chunk with stone in a triangular footprint within
# section Y=-4 (world Y in [-60, -50], lx + lz <= world_y + 64). The Python
# oracle re-derives the expected per-column heightmap values, unpacks the
# packed 9-bit-LSB-first-non-straddling longs, and asserts byte-by-byte
# equivalence across all 256 columns × 4 heightmaps. This is the first
# cross-check on the (lz * 16 + lx) heightmap-layout convention from
# proposal §3.5 — the Superflat fixture is uniform and can't disambiguate
# (lz, lx) vs (lx, lz) without a non-uniform pattern.
add_executable(test_chunk_heightmap test_chunk_heightmap.cpp)
target_link_libraries(test_chunk_heightmap PRIVATE chunk)

add_test(
    NAME chunk_heightmap_oracle
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_chunk_heightmap>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_chunk_heightmap_oracle.py
)
set_tests_properties(chunk_heightmap_oracle PROPERTIES LABELS "oracle")

# Session 15d — chunk-schema Tier-4 manual-verification helper.
#
# Build target only; NOT registered as a CTest test. The Python generator
# scripts in tests/manual/ orchestrate the manual-test artifact build
# (positive + negative coords, plus timestamp-table zeroing for
# deterministic SHA-256). Same pattern as Session 11's
# test_anvil_single_chunk_writer (which is a CTest-buildable target but
# not itself a CTest case — it's driven by the Tier-2 oracle).
add_executable(chunkschema_tier4_helper manual/chunkschema_tier4_helper.cpp)
target_link_libraries(chunkschema_tier4_helper PRIVATE chunk anvil)

# ---------------------------------------------------------------------------
# Session 18 — OBJ Loading layer tests (research/obj_loading_proposal.md §5.10).
# Four C++ unit tests covering: hand-cube (a), MTL-bearing (b), malformed
# input throws mesh_error (c), Stanford bunny load (d). No Python oracle —
# tinyobjloader is the external reference parser (proposal §5.9 wraps it),
# and the OBJ format itself has no version-cliff comparable to the chunk-NBT
# layer's MC-DataVersion sensitivity.
# ---------------------------------------------------------------------------
set(MESH_FIXTURES_DIR ${CMAKE_SOURCE_DIR}/tests/fixtures/mesh)

add_executable(test_mesh_cube test_mesh_cube.cpp)
target_link_libraries(test_mesh_cube PRIVATE mesh)
add_test(
    NAME mesh_cube
    COMMAND test_mesh_cube ${MESH_FIXTURES_DIR}/cube.obj
)

add_executable(test_mesh_materials test_mesh_materials.cpp)
target_link_libraries(test_mesh_materials PRIVATE mesh)
add_test(
    NAME mesh_materials
    COMMAND test_mesh_materials ${MESH_FIXTURES_DIR}/materials.obj
)

add_executable(test_mesh_malformed test_mesh_malformed.cpp)
target_link_libraries(test_mesh_malformed PRIVATE mesh)
add_test(
    NAME mesh_malformed
    COMMAND test_mesh_malformed
            ${MESH_FIXTURES_DIR}/does_not_exist.obj
            ${MESH_FIXTURES_DIR}/malformed.obj
)

add_executable(test_mesh_bunny test_mesh_bunny.cpp)
target_link_libraries(test_mesh_bunny PRIVATE mesh)
add_test(
    NAME mesh_bunny
    COMMAND test_mesh_bunny ${MESH_FIXTURES_DIR}/bunny.obj
)

# ---------------------------------------------------------------------------
# Session 20a — Block Palette DB layer tests (research/palette_db_proposal.md
# §3.11). Five C++ unit tests covering: shape (a), spot-check (b),
# exclusions (c), malformed CSV (d), version mismatch (e). All five run
# against the hand-curated fixture at tests/fixtures/palette/minimal_palette.csv;
# the real data/block_palette_db.csv is produced by Session 20b's maintainer-
# run of scripts/extract_block_colors.py and the test bounds are
# fixture-scoped until then. Vendored blocks.json supplies the
# cross-check + version-assert source.
# ---------------------------------------------------------------------------
set(PALETTE_FIXTURES_DIR ${CMAKE_SOURCE_DIR}/tests/fixtures/palette)
set(PALETTE_BLOCKS_JSON
    ${CMAKE_SOURCE_DIR}/third_party/minecraft-data/pc/1.21.5/blocks.json)

add_executable(test_palette_shape test_palette_shape.cpp)
target_link_libraries(test_palette_shape PRIVATE palette)
add_test(
    NAME palette_shape
    COMMAND test_palette_shape
            ${PALETTE_FIXTURES_DIR}/minimal_palette.csv
            ${PALETTE_BLOCKS_JSON}
)

add_executable(test_palette_spot_check test_palette_spot_check.cpp)
target_link_libraries(test_palette_spot_check PRIVATE palette)
add_test(
    NAME palette_spot_check
    COMMAND test_palette_spot_check
            ${PALETTE_FIXTURES_DIR}/minimal_palette.csv
            ${PALETTE_BLOCKS_JSON}
)

add_executable(test_palette_exclusions test_palette_exclusions.cpp)
target_link_libraries(test_palette_exclusions PRIVATE palette)
add_test(
    NAME palette_exclusions
    COMMAND test_palette_exclusions
            ${PALETTE_FIXTURES_DIR}/minimal_palette.csv
            ${PALETTE_BLOCKS_JSON}
)

add_executable(test_palette_malformed test_palette_malformed.cpp)
target_link_libraries(test_palette_malformed PRIVATE palette)
add_test(
    NAME palette_malformed
    COMMAND test_palette_malformed
            ${PALETTE_FIXTURES_DIR}/does_not_exist.csv
            ${PALETTE_FIXTURES_DIR}/malformed_wrong_columns.csv
            ${PALETTE_FIXTURES_DIR}/malformed_opacity_oob.csv
            ${PALETTE_FIXTURES_DIR}/malformed_nonnumeric_luminance.csv
            ${PALETTE_BLOCKS_JSON}
)

add_executable(test_palette_version_mismatch test_palette_version_mismatch.cpp)
target_link_libraries(test_palette_version_mismatch PRIVATE palette)
add_test(
    NAME palette_version_mismatch
    COMMAND test_palette_version_mismatch
            ${PALETTE_FIXTURES_DIR}/version_mismatch_palette.csv
            ${PALETTE_BLOCKS_JSON}
)

# Session 20a.7 — real-catalog shape test. Exercises load_default_palette()
# against data/block_palette_db.csv (the 414-row catalog committed by
# Session 20a.7). No CLI args: the loader resolves the path via the
# OBJTOMC_PALETTE_DATA_DIR + OBJTOMC_VENDOR_MINECRAFT_DATA_DIR macros baked
# into src/palette.cpp at compile time. Bound [400, 450] is empirically
# calibrated per DECISIONS.md 2026-06-20 "Block Palette DB §3.11(a) bound
# recalibration".
add_executable(test_palette_default_shape test_palette_default_shape.cpp)
target_link_libraries(test_palette_default_shape PRIVATE palette)
add_test(
    NAME palette_default_shape
    COMMAND test_palette_default_shape
)

# ---------------------------------------------------------------------------
# Session 22 — Color Matching layer tests (research/matcher_proposal.md
# §3.13). Seven C++ unit tests covering: default-palette smoke (a),
# emission branch (b), transparency branch (c), determinism (d),
# alphabetical tie-break (e), single-vs-batch consistency (f), error model
# (g). Tests a/b/c/d/f exercise the real 414-entry catalog via
# load_default_palette() (the same default-path resolution path the
# voxelisation layer will use); test (e) drives load_palette_csv() against
# a 2-row fixture with identical avg_color forcing the §3.12 alphabetical
# tie-break; test (g) constructs an empty PaletteDB directly + NaN/Inf
# Materials + invalid MatcherOptions. No Python oracle — the empirical
# §1.5 harness IS the spec; tests are CI-time assertions of that spec
# (proposal §6 verification posture).
# ---------------------------------------------------------------------------
add_executable(test_matcher_default_smoke test_matcher_default_smoke.cpp)
target_link_libraries(test_matcher_default_smoke PRIVATE matcher palette mesh chunk)
add_test(NAME matcher_default_smoke COMMAND test_matcher_default_smoke)

add_executable(test_matcher_emission_branch test_matcher_emission_branch.cpp)
target_link_libraries(test_matcher_emission_branch PRIVATE matcher palette mesh chunk)
add_test(NAME matcher_emission_branch COMMAND test_matcher_emission_branch)

add_executable(test_matcher_transparency_branch test_matcher_transparency_branch.cpp)
target_link_libraries(test_matcher_transparency_branch PRIVATE matcher palette mesh chunk)
add_test(NAME matcher_transparency_branch COMMAND test_matcher_transparency_branch)

add_executable(test_matcher_determinism test_matcher_determinism.cpp)
target_link_libraries(test_matcher_determinism PRIVATE matcher palette mesh chunk)
add_test(NAME matcher_determinism COMMAND test_matcher_determinism)

add_executable(test_matcher_tie_break test_matcher_tie_break.cpp)
target_link_libraries(test_matcher_tie_break PRIVATE matcher palette mesh chunk)
add_test(
    NAME matcher_tie_break
    COMMAND test_matcher_tie_break
            ${PALETTE_FIXTURES_DIR}/tie_break_palette.csv
            ${PALETTE_BLOCKS_JSON}
)

add_executable(test_matcher_batch_consistency test_matcher_batch_consistency.cpp)
target_link_libraries(test_matcher_batch_consistency PRIVATE matcher palette mesh chunk)
add_test(NAME matcher_batch_consistency COMMAND test_matcher_batch_consistency)

add_executable(test_matcher_errors test_matcher_errors.cpp)
target_link_libraries(test_matcher_errors PRIVATE matcher palette mesh chunk)
add_test(NAME matcher_errors COMMAND test_matcher_errors)

# ---------------------------------------------------------------------------
# Session 24 — Voxelisation layer tests (research/voxel_proposal.md §3.12).
# Seven C++ unit tests covering: unit-cube smoke (a), Stanford bunny (b),
# multi-material disambiguate (c), axis convention Z-up (d), determinism (e),
# oversize error (f), general errors (g). No Python oracle — voxelisation
# produces a 3D int32 array; the cuda_voxelizer empirical-reference baseline
# named in DECISIONS.md 2026-06-29 is for cross-check at implementation time,
# not vendored. No Tier-4-style external verification (in-memory algorithm
# code, not a format layer).
# ---------------------------------------------------------------------------
add_executable(test_voxel_unit_cube_smoke test_voxel_unit_cube_smoke.cpp)
target_link_libraries(test_voxel_unit_cube_smoke PRIVATE voxel mesh)
add_test(NAME voxel_unit_cube_smoke COMMAND test_voxel_unit_cube_smoke)

add_executable(test_voxel_stanford_bunny test_voxel_stanford_bunny.cpp)
target_link_libraries(test_voxel_stanford_bunny PRIVATE voxel mesh)
add_test(
    NAME voxel_stanford_bunny
    COMMAND test_voxel_stanford_bunny ${MESH_FIXTURES_DIR}/bunny.obj
)

add_executable(test_voxel_multi_material_disambiguate
               test_voxel_multi_material_disambiguate.cpp)
target_link_libraries(test_voxel_multi_material_disambiguate PRIVATE voxel mesh)
add_test(NAME voxel_multi_material_disambiguate
         COMMAND test_voxel_multi_material_disambiguate)

add_executable(test_voxel_axis_convention_zup
               test_voxel_axis_convention_zup.cpp)
target_link_libraries(test_voxel_axis_convention_zup PRIVATE voxel mesh)
add_test(NAME voxel_axis_convention_zup
         COMMAND test_voxel_axis_convention_zup)

add_executable(test_voxel_determinism test_voxel_determinism.cpp)
target_link_libraries(test_voxel_determinism PRIVATE voxel mesh)
add_test(NAME voxel_determinism COMMAND test_voxel_determinism)

add_executable(test_voxel_oversize_error test_voxel_oversize_error.cpp)
target_link_libraries(test_voxel_oversize_error PRIVATE voxel mesh)
add_test(NAME voxel_oversize_error COMMAND test_voxel_oversize_error)

add_executable(test_voxel_errors test_voxel_errors.cpp)
target_link_libraries(test_voxel_errors PRIVATE voxel mesh)
add_test(NAME voxel_errors COMMAND test_voxel_errors)

# ---------------------------------------------------------------------------
# Session 26 — Orchestration layer tests (research/orchestration_proposal.md
# §3.13). Seven C++ unit tests covering: unit-cube smoke (a), Stanford bunny
# (b), level.dat Python oracle (c), region-count prediction (d), determinism
# (e), CLI error-code mapping (f), plus the Tier-4 helper (g) which is built
# but NOT a CTest case — it's a manual artifact generator the maintainer runs
# before Session 28's Tier-4 vanilla-MC verification.
# ---------------------------------------------------------------------------
add_executable(test_orchestrate_unit_cube_smoke test_orchestrate_unit_cube_smoke.cpp)
target_link_libraries(test_orchestrate_unit_cube_smoke PRIVATE orchestrate)
add_test(NAME orchestrate_unit_cube_smoke COMMAND test_orchestrate_unit_cube_smoke)

add_executable(test_orchestrate_stanford_bunny test_orchestrate_stanford_bunny.cpp)
target_link_libraries(test_orchestrate_stanford_bunny PRIVATE orchestrate)
add_test(
    NAME orchestrate_stanford_bunny
    COMMAND test_orchestrate_stanford_bunny ${MESH_FIXTURES_DIR}/bunny.obj
)

# C++ writer side of the level.dat Python oracle: emits a level.dat with
# deterministic LastPlayed=0 at a path passed via PATH_TO_LEVELDAT env var.
add_executable(test_orchestrate_level_dat_writer test_orchestrate_level_dat_writer.cpp)
target_link_libraries(test_orchestrate_level_dat_writer PRIVATE orchestrate)

# The oracle: Python script that drives the writer + asserts every field
# per orchestration_proposal §3.5 + §3.5 implementation-time-verification
# additions (Border tuning, WanderingTrader*, DragonFight, etc.).
add_test(
    NAME orchestrate_level_dat_oracle
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_orchestrate_level_dat_writer>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_orchestrate_level_dat_oracle.py
)
set_tests_properties(orchestrate_level_dat_oracle PROPERTIES LABELS "oracle")

add_executable(test_orchestrate_region_count test_orchestrate_region_count.cpp)
target_link_libraries(test_orchestrate_region_count PRIVATE orchestrate)
add_test(NAME orchestrate_region_count COMMAND test_orchestrate_region_count)

add_executable(test_orchestrate_determinism test_orchestrate_determinism.cpp)
target_link_libraries(test_orchestrate_determinism PRIVATE orchestrate)
add_test(NAME orchestrate_determinism COMMAND test_orchestrate_determinism)

add_executable(test_orchestrate_cli_errors test_orchestrate_cli_errors.cpp)
target_link_libraries(test_orchestrate_cli_errors PRIVATE orchestrate)
add_test(
    NAME orchestrate_cli_errors
    COMMAND test_orchestrate_cli_errors
            $<TARGET_FILE:objtomc>
            ${MESH_FIXTURES_DIR}/cube.obj
)

# Tier-4 artifact generator. Build target only; NOT a CTest case — see
# DECISIONS.md 2026-07-01 "Orchestration scope (v0.1)" + proposal §3.13(g).
# maintainer runs this manually to (re)generate
# tests/manual/orchestrate_tier4_world/ before Session 28.
add_executable(orchestrate_tier4_helper manual/orchestrate_tier4_helper.cpp)
target_link_libraries(orchestrate_tier4_helper PRIVATE orchestrate)
