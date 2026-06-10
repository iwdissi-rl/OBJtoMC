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
