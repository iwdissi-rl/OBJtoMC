#pragma once

// ---------------------------------------------------------------------------
// GpuPreference — shared opt-in GPU dispatch selector (v0.2 work item #3).
//
// Session 40 introduced this enum in include/objtomc/voxel.hpp for the
// voxelizer's opts.prefer_gpu field. Session 41 needs the same enum on
// MatcherOptions::prefer_gpu, so we extract to a small shared header
// per the maintainer's Session 41 AskUserQuestion choice
// (2026-07-03 "include/objtomc/gpu_preference.hpp — single source of truth").
//
// Only present when the library is built with -DWITH_CUDA=ON (which sets
// OBJTOMC_WITH_CUDA=1). On CPU-only builds this file's only symbol does not
// exist; both voxel.hpp and matcher.hpp guard their prefer_gpu fields with
// the same #ifdef so their public API stays ABI-identical to Session 37.
//
// See research/gpu_port_proposal.md §3.4 (voxelizer dispatch semantics) +
// §3.5 (matcher dispatch semantics) + §3.11 (public API preservation
// contract).
// ---------------------------------------------------------------------------

#ifdef OBJTOMC_WITH_CUDA

namespace objtomc {

// Dispatch preference — same three-way semantics across layers. AUTO means
// "use GPU when the workload is big enough to amortise kernel-launch
// overhead"; the per-layer thresholds are documented in each consumer
// (voxel: grid cells >= 64^3; matcher: batch size >= 1000).
enum class GpuPreference {
    AUTO,
    ON,
    OFF,
};

}  // namespace objtomc

#endif  // OBJTOMC_WITH_CUDA
