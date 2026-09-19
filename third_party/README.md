# third_party/

Vendored third-party C++ dependencies. The vendoring contract is set by
`research/obj_loading_proposal.md` §2 and locked by DECISIONS.md
2026-06-18 "Vendoring pattern for third-party C++ deps". Summary of the
rules in force:

## Layout

Each dependency lives at `third_party/<libname>/` with:

- `LICENSE` — verbatim from upstream.
- `VERSION.txt` — provenance: `upstream:` URL, `license:` /
  `license-spdx:`, `git-sha:` (full 40-char SHA — load-bearing),
  `git-ref:` (branch or tag — informational), `acquired:` date,
  `acquired-by:` session, `audited-by:` session, plus the
  `source-url:` + `file-sha256:` of each vendored file so a future
  reader can re-fetch upstream and `diff` byte-for-byte.
- The vendored source files (e.g. `tiny_obj_loader.h`).

## Modification policy (contract — read-only source)

Vendored source is **read-only**. If behaviour needs to change:

- **Configuration knobs** (compile-time `#define`s the upstream
  library itself exposes — e.g. `TINYOBJLOADER_IMPLEMENTATION`)
  go in our wrapper translation unit (e.g. `src/mesh.cpp`),
  before the `#include`. This is upstream-blessed customisation.
- **Behavioural changes not exposed as knobs** go in our wrapper
  code that adapts the upstream type shape to our public API
  (e.g. `src/mesh.cpp` translates `tinyobj::attrib_t` →
  `objtomc::mesh::Mesh`). **Wrap, don't patch.**
- **Bug fixes** that upstream hasn't merged yet are a hard case.
  Policy: fork upstream, open a PR, and pin our `git-sha` to our
  fork's SHA. Do **not** silently edit the vendored copy — the
  silent-patch path breaks the "reproducible upstream snapshot"
  guarantee. A future reader must be able to `wget` the SHA in
  `VERSION.txt` and `diff` it byte-for-byte against our copy.

## Update procedure

Updating a vendored dep to a new upstream SHA is a deliberate
session, not a quiet bump:

1. Re-audit the upstream license at the new SHA (it may have
   changed; record the audit in DECISIONS.md).
2. Re-run our entire test suite against the new SHA.
3. Add a DECISIONS.md entry recording the upgrade — old SHA,
   new SHA, audit outcome, any tests that needed to change.
4. Update `VERSION.txt` (`git-sha:`, `acquired:`, `acquired-by:`,
   `audited-by:`, `file-sha256:`).

## When to vendor vs FetchContent vs submodule

(Decision policy from the proposal §2.2.)

- **Vendor source** — default for **single-header libs** (header-only,
  < ~10 kLOC). Hermetic builds, no network at build time, exact
  version via git. tinyobjloader fits this case.
- **CMake `FetchContent`** — default for **multi-file libs with active
  maintenance and a CMake build**. Cache lives under
  `third_party/_fetched/<libname>/`. Pin to a full SHA `GIT_TAG`,
  never to a moving ref. No multi-file dep yet in v0.1.
- **Git submodule** — reserved for libs we expect to upstream
  patches to. None planned for v0.1.

## Currently vendored

- `tinyobjloader/` — single-header OBJ/MTL parser, MIT.
  Pinned to `release`-branch SHA
  `62ff207968f3dc14a64a1e2378dce67b760e7c4a` (2026-05-21 upstream
  HEAD). Acquired 2026-06-18 (Session 18). Used by the OBJ
  Loading layer (`src/mesh.cpp`). Embeds `fast_float` v8.0.2
  (tri-licensed MIT / Apache-2.0 / BSL-1.0) inline — see
  DECISIONS.md 2026-06-18 "Adopt tinyobjloader (pinned,
  vendored) for OBJ parsing".
- `stb/` — single-header image decoder (`stb_image.h` v2.30),
  dual public-domain / MIT. Pinned to `master`-branch SHA
  `013ac3beddff3dbffafd5177e7972067cd2b5083` (last upstream commit
  touching `stb_image.h`, 2024-05-31). Acquired 2026-09-14
  (Session 48). Used by the texture layer (`src/texture.cpp`) to
  decode `map_Kd` diffuse textures for per-voxel sampling (v0.3
  item #1).
- `cgltf/` — single-header C99 glTF 2.0 parser (`cgltf.h` v1.15),
  MIT. Pinned to `master`-branch SHA
  `85cd62382dfea638278962690cf515023f33ed00` (upstream HEAD at
  acquisition, committed 2026-02-02). Acquired 2026-09-14
  (Session 49). Used by the glTF loading layer (`src/gltf.cpp`) to
  import Sketchfab `.gltf`/`.glb` downloads into `mesh::Mesh`
  (v0.3 item #2). Zero sub-dependencies — chosen over tinygltf,
  which bundles nlohmann/json (see DECISIONS.md 2026-09-14).
- `cuda_voxelizer/` — vendored subset (5 files: `voxelize.cu`,
  `voxelize.cuh`, `morton_LUTs.h`, `util.h`, `LICENSE`) of the
  Schwarz-Seidel surface-voxelisation CUDA kernel, MIT. Pinned to
  `main`-branch SHA `f9fc9a936e3f3bb50fc001de638b36b94a87b5ca`
  (2026-06-16 upstream HEAD). Acquired 2026-07-04 (Session 39).
  Used by the GPU voxelizer path (`src/voxel_gpu.cu` adapter shim;
  gated on `WITH_CUDA`). Upstream CLI/IO/CPU-fallback files
  intentionally NOT vendored — see the per-file disposition table
  in `cuda_voxelizer/VERSION.txt`.
- `minecraft-data/` — JSON data feed (no source code), MIT.
  Pinned to `master`-branch SHA
  `d4d349218383bc0befcdd8bb0adac135234fe5d7` (Release 3.111.0,
  2026-06-18). Acquired 2026-06-20 (Session 20a). Used by the
  Block Palette DB layer (`src/palette.cpp`) for version assert
  + per-block opacity/luminance cross-check, and by the
  out-of-tree extractor `scripts/extract_block_colors.py`. Three
  vendored files under `pc/1.21.5/`: `blocks.json`, `tints.json`,
  `version.json`. Upstream does NOT commit a standalone LICENSE
  file; our vendored `LICENSE` reproduces the upstream README's
  inline MIT declaration with re-derivation provenance. See
  DECISIONS.md 2026-06-19 "Block Palette DB scope (v0.1)".
