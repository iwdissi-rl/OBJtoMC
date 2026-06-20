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
