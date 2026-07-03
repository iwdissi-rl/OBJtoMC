// ---------------------------------------------------------------------------
// util_cuda.h shim (Session 40) — checkCudaErrors macro.
//
// The vendored third_party/cuda_voxelizer/voxelize.cuh includes "util_cuda.h"
// to get the upstream project's error-check macro. Upstream's util_cuda.h
// aborts on failure via fprintf + exit(EXIT_FAILURE) — inappropriate for a
// library callable from tests or long-running orchestration. Our shim
// therefore throws a std::runtime_error carrying the cuda error string so
// callers in src/voxel_gpu.cu can wrap it as objtomc::voxel::voxel_error per
// research/gpu_port_proposal.md §3.10 (A/B/C failure classes).
//
// Rationale for a shim (not vendoring util_cuda.h): we would have to modify
// upstream to change the abort-on-failure behaviour, breaking the "vendored
// verbatim; only shim adapts" discipline (SESSION_LOG Session 39 STOP+resolve
// record).
// ---------------------------------------------------------------------------
#pragma once

#include <cuda_runtime.h>

#include <stdexcept>
#include <string>

#define checkCudaErrors(call)                                                  \
    do {                                                                       \
        const cudaError_t _cudaErr = (call);                                   \
        if (_cudaErr != cudaSuccess) {                                         \
            throw std::runtime_error(                                          \
                std::string("cuda_voxelizer: CUDA error at ")                  \
                    + __FILE__ + ":" + std::to_string(__LINE__) + ": "         \
                    + cudaGetErrorString(_cudaErr));                           \
        }                                                                      \
    } while (0)
