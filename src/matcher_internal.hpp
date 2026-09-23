// ---------------------------------------------------------------------------
// Project-internal header — NOT part of the public include tree.
//
// Shared between src/matcher.cpp (CPU dispatcher + CPU kernel) and, when
// -DWITH_CUDA=ON, src/matcher_gpu.cu (GPU kernel).
//
// Session 41 (v0.2 work item #3, GPU matcher slice) added this header to
// give both TUs a shared prototype for detail::fill_matches_gpu without
// leaking the internal implementation into the public matcher.hpp.
//
// Byte-equality contract (proposal §3.6): fill_matches_gpu must produce
// the same BlockState output as the CPU path in src/matcher.cpp for the
// same input on the same host. Achieved by porting the exact CPU
// arithmetic (sRGB→Oklab, three-branch dispatch, proximity bonus, four-key
// lex tie-break) to CUDA device code + compiling with --fmad=false to
// suppress the nvcc-vs-GCC fused-multiply-add divergence documented in
// Session 40 SHA-pin (DECISIONS.md 2026-07-04).
// ---------------------------------------------------------------------------
#pragma once

#include <span>

#include "objtomc/chunk.hpp"
#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

namespace objtomc::matcher::detail {

#ifdef OBJTOMC_WITH_CUDA
// GPU cell-filling: same semantics as the loop in src/matcher.cpp
// Matcher::match_batch — byte-identical output on the same input.
// Defined in src/matcher_gpu.cu.
//
// The Oklab cache is passed in rather than recomputed on device — Matcher
// already has it, and the GPU path benefits from an already-Oklab'd
// palette in __constant__ memory. `db` supplies palette metadata (opacity,
// luminance, alphabetical rank, name/properties for the final BlockState
// materialisation on host).
//
// Throws matcher_error on any CUDA runtime failure. No CUDA state leaks
// on the throw path (RAII device-buffer wrappers per proposal §3.10).
void fill_matches_gpu(std::span<const MatchInput>              in,
                      std::span<::objtomc::chunk::BlockState>  out,
                      const ::objtomc::palette::PaletteDB&      db,
                      std::span<const Oklab>                     palette_oklab,
                      const MatcherOptions&                      opts);
#endif  // OBJTOMC_WITH_CUDA

}  // namespace objtomc::matcher::detail
