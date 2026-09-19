# Third-Party Licenses

This project bundles the following third-party components. See the
vendoring contract at `third_party/README.md` and the per-dependency
provenance files at `third_party/<libname>/VERSION.txt`.

The project's own license is currently **TBD** — see
`OPEN_QUESTIONS.md` 2026-06-18 "Project license declaration". Every
dependency listed below is permissive and compatible with whatever
the project eventually declares.

## tinyobjloader

- **Path in repo:** `third_party/tinyobjloader/`
- **Upstream:** https://github.com/tinyobjloader/tinyobjloader
- **Pinned commit:** `62ff207968f3dc14a64a1e2378dce67b760e7c4a`
  (2026-05-21 HEAD of the `release` branch — the upstream-
  recommended pin target per the project's README)
- **Acquired:** 2026-06-18 (Session 18)
- **Audited:** 2026-06-18 (Session 17 — see
  `research/obj_loading_proposal.md` §3)
- **License:** MIT (SPDX: `MIT`). Full text at
  `third_party/tinyobjloader/LICENSE`.
- **Copyright:** Copyright (c) 2012-Present, Syoyo Fujita and many
  contributors.
- **Embedded sub-dependency:** the header bundles **`fast_float`
  v8.0.2** for SIMD-accelerated float parsing. `fast_float` is
  tri-licensed: **MIT OR Apache-2.0 OR BSL-1.0** (all permissive,
  all compatible). The bundled copy carries its own license
  preamble inside `tiny_obj_loader.h`.
- **Used by:** the OBJ Loading layer
  (`include/objtomc/mesh.hpp` + `src/mesh.cpp`).

## stb (stb_image)

- **Path in repo:** `third_party/stb/`
- **Upstream:** https://github.com/nothings/stb
- **Pinned commit:** `013ac3beddff3dbffafd5177e7972067cd2b5083`
  (last upstream commit touching `stb_image.h` as of vendor-time;
  `stb_image.h` v2.30, 2024-05-31)
- **Acquired:** 2026-09-14 (Session 48)
- **Audited:** 2026-09-14 (Session 48 — single-header, no
  sub-dependencies, dual-licensed)
- **License:** dual — public domain (Unlicense-style) **OR** MIT,
  recipient's choice (SPDX: `MIT OR Unlicense`). Full text at
  `third_party/stb/LICENSE`.
- **Copyright:** Copyright (c) 2017 Sean Barrett (MIT alternative);
  public-domain dedication under Alternative B.
- **Vendored content:** `stb_image.h` only (PNG/JPG/BMP/TGA/GIF/
  PSD/PIC/PNM decoder; we compile with `STBI_ONLY_*` selectors —
  see `src/texture.cpp`).
- **Embedded sub-dependency:** none.
- **Used by:** the texture layer (`include/objtomc/texture.hpp` +
  `src/texture.cpp`) for per-voxel `map_Kd` diffuse-texture
  sampling (v0.3 item #1, Session 48).

## cgltf

- **Path in repo:** `third_party/cgltf/`
- **Upstream:** https://github.com/jkuhlmann/cgltf
- **Pinned commit:** `85cd62382dfea638278962690cf515023f33ed00`
  (upstream `master` HEAD at acquisition, committed 2026-02-02;
  `cgltf.h` v1.15)
- **Acquired:** 2026-09-14 (Session 49)
- **Audited:** 2026-09-14 (Session 49 — single-header, zero
  sub-dependencies)
- **License:** MIT (SPDX: `MIT`). Full text at
  `third_party/cgltf/LICENSE`.
- **Copyright:** Copyright (c) 2018-2021 Johannes Kuhlmann.
- **Vendored content:** `cgltf.h` only (C99 glTF 2.0 parser:
  `.gltf` JSON + `.glb` binary; buffer loading from external
  `.bin` files, GLB BIN chunks, and base64 data URIs).
- **Embedded sub-dependency:** none (the reason it was chosen over
  tinygltf, which bundles nlohmann/json — see DECISIONS.md
  2026-09-14).
- **Used by:** the glTF loading layer (`include/objtomc/gltf.hpp` +
  `src/gltf.cpp`) for Sketchfab `.gltf`/`.glb` import (v0.3
  item #2, Session 49).

## cuda_voxelizer

- **Path in repo:** `third_party/cuda_voxelizer/`
- **Upstream:** https://github.com/Forceflow/cuda_voxelizer
- **Pinned commit:** `f9fc9a936e3f3bb50fc001de638b36b94a87b5ca`
  (upstream `main` HEAD, 2026-06-16)
- **Acquired:** 2026-07-04 (Session 39)
- **Audited:** Session 23 (`research/voxel_proposal.md` §1.5(a))
  + Session 38 (`research/gpu_port_proposal.md` §1.5(a) SHA +
  LICENSE refresh)
- **License:** MIT (SPDX: `MIT`). Full text at
  `third_party/cuda_voxelizer/LICENSE`.
- **Copyright:** Copyright (c) 2019 Jeroen Baert.
- **Vendored content:** 4 source files (`voxelize.cu`,
  `voxelize.cuh`, `morton_LUTs.h`, `util.h`) — the Schwarz-Seidel
  surface-voxelisation kernel subset only. Upstream CLI, mesh IO,
  CPU fallback, solid voxelisation, and Trimesh2 submodule
  intentionally NOT vendored; per-file disposition table in
  `third_party/cuda_voxelizer/VERSION.txt`.
- **Embedded sub-dependency:** none vendored (upstream's Trimesh2 /
  helper-header dependencies are satisfied by the in-tree adapter
  shim in `src/voxel_gpu.cu`, not by vendoring).
- **Used by:** the GPU voxelizer path (`src/voxel_gpu.cu`, gated on
  `WITH_CUDA`; v0.2 work item #3, Sessions 39-42).

## PrismarineJS/minecraft-data

- **Path in repo:** `third_party/minecraft-data/`
- **Upstream:** https://github.com/PrismarineJS/minecraft-data
- **Pinned commit:** `d4d349218383bc0befcdd8bb0adac135234fe5d7`
  (Release 3.111.0, 2026-06-18 — master HEAD at vendor-time)
- **Acquired:** 2026-06-20 (Session 20a)
- **Audited:** 2026-06-19 (Session 19 — see
  `research/palette_db_proposal.md` §1.5(a))
- **License:** MIT (SPDX: `MIT`). Upstream does **not** commit a
  standalone `LICENSE` file at the pinned SHA; the licence is
  declared inline in `README.md §License`. Our vendored
  `third_party/minecraft-data/LICENSE` reproduces that declaration
  verbatim with re-derivation provenance (curl + sed command).
- **Copyright:** Not declared by upstream beyond the README's MIT
  reference. PrismarineJS is a multi-contributor open-source
  community project.
- **Vendored content:** JSON data files only — no source code.
  - `pc/1.21.5/blocks.json` (794 021 bytes; 1 104 block entries
    with `name`, `displayName`, `emitLight`, `filterLight`,
    `boundingBox`, `transparent`, `states`, etc.)
  - `pc/1.21.5/tints.json` (8 837 bytes; biome-tint reference for
    `grass` / `foliage` / `water` / `redstone` / `constant`)
  - `pc/1.21.5/version.json` (106 bytes;
    `minecraftVersion="1.21.5"`, `version=770`)
- **Embedded sub-dependency:** none. JSON data only.
- **Used by:** the Block Palette DB layer
  (`include/objtomc/palette.hpp` + `src/palette.cpp`) for version
  assert + per-block opacity/luminance cross-check at load time;
  `scripts/extract_block_colors.py` for the per-block name list
  and texture-tint applicability at extract time.
