// ---------------------------------------------------------------------------
// libs/cuda/helper_math.h shim (Session 40) — CUDA vector math operators.
//
// Substitute for the upstream NVIDIA CUDA samples header of the same name
// (originally at NVIDIA/cuda-samples/Common/helper_math.h, BSD-3-Clause).
// The vendored third_party/cuda_voxelizer/voxelize.cuh includes it as
// "libs/cuda/helper_math.h" (upstream cuda_voxelizer git-submodules the
// samples repo there; we don't). We reproduce only the operator surface that
// voxelize.cu actually exercises, empirically enumerated during Session 40
// pre-implementation nvcc test-compile:
//
//   - float3 arithmetic: + - * / on float3 x float3, scalar * float3,
//     unary -, +=, -=
//   - float2 arithmetic: + - on float2 x float2, scalar * float2, unary -
//   - dot(float2, float2), dot(float3, float3)
//   - cross(float3, float3)
//   - length(float3), normalize(float3)
//   - fminf/fmaxf component-wise on float3 (upstream cuda_voxelizer relies on
//     these having float3 overloads distinct from the stdlib scalar fminf)
//   - clamp(int3, int3, int3) component-wise
//
// All symbols are __host__ __device__ inline; no state; no cross-header
// dependencies beyond <cuda_runtime.h> (for float3 / int3 types) and <math.h>
// (for scalar sqrtf / rsqrtf / fminf / fmaxf).
//
// Kept minimal per research/gpu_port_proposal.md §3.10 open risk 6 — expand
// only if a future upstream sync surfaces new call sites.
// ---------------------------------------------------------------------------
#pragma once

#include <cuda_runtime.h>
#include <math.h>

// float3 element-wise arithmetic
__host__ __device__ inline float3 operator-(float3 a, float3 b) {
    return make_float3(a.x - b.x, a.y - b.y, a.z - b.z);
}
__host__ __device__ inline float3 operator+(float3 a, float3 b) {
    return make_float3(a.x + b.x, a.y + b.y, a.z + b.z);
}
__host__ __device__ inline float3 operator*(float3 a, float3 b) {
    return make_float3(a.x * b.x, a.y * b.y, a.z * b.z);
}
__host__ __device__ inline float3 operator/(float3 a, float3 b) {
    return make_float3(a.x / b.x, a.y / b.y, a.z / b.z);
}

// float3 scalar arithmetic
__host__ __device__ inline float3 operator*(float s, float3 a) {
    return make_float3(s * a.x, s * a.y, s * a.z);
}
__host__ __device__ inline float3 operator*(float3 a, float s) {
    return make_float3(s * a.x, s * a.y, s * a.z);
}
__host__ __device__ inline float3 operator-(float3 a) {
    return make_float3(-a.x, -a.y, -a.z);
}
__host__ __device__ inline float3& operator+=(float3& a, float3 b) {
    a.x += b.x; a.y += b.y; a.z += b.z; return a;
}
__host__ __device__ inline float3& operator-=(float3& a, float3 b) {
    a.x -= b.x; a.y -= b.y; a.z -= b.z; return a;
}

// float2 arithmetic
__host__ __device__ inline float2 operator-(float2 a, float2 b) {
    return make_float2(a.x - b.x, a.y - b.y);
}
__host__ __device__ inline float2 operator+(float2 a, float2 b) {
    return make_float2(a.x + b.x, a.y + b.y);
}
__host__ __device__ inline float2 operator*(float s, float2 a) {
    return make_float2(s * a.x, s * a.y);
}
__host__ __device__ inline float2 operator-(float2 a) {
    return make_float2(-a.x, -a.y);
}

// Dot / cross / length / normalize
__host__ __device__ inline float dot(float2 a, float2 b) {
    return a.x * b.x + a.y * b.y;
}
__host__ __device__ inline float dot(float3 a, float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
__host__ __device__ inline float3 cross(float3 a, float3 b) {
    return make_float3(a.y * b.z - a.z * b.y,
                       a.z * b.x - a.x * b.z,
                       a.x * b.y - a.y * b.x);
}
__host__ __device__ inline float length(float3 a) {
    return sqrtf(dot(a, a));
}
__host__ __device__ inline float3 normalize(float3 a) {
    const float r = rsqrtf(dot(a, a));
    return make_float3(a.x * r, a.y * r, a.z * r);
}

// Component-wise float3 min / max (voxelize.cu passes float3 args to these).
__host__ __device__ inline float3 fminf(float3 a, float3 b) {
    return make_float3(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z));
}
__host__ __device__ inline float3 fmaxf(float3 a, float3 b) {
    return make_float3(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z));
}

// Component-wise int3 clamp.
__host__ __device__ inline int3 clamp(int3 v, int3 lo, int3 hi) {
    return make_int3(
        v.x < lo.x ? lo.x : (v.x > hi.x ? hi.x : v.x),
        v.y < lo.y ? lo.y : (v.y > hi.y ? hi.y : v.y),
        v.z < lo.z ? lo.z : (v.z > hi.z ? hi.z : v.z));
}
