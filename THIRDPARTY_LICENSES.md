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
