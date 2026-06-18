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
