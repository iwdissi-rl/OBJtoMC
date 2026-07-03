// ---------------------------------------------------------------------------
// TriMesh.h shim (Session 40) — empty stub.
//
// The vendored third_party/cuda_voxelizer/util.h includes "TriMesh.h" so it
// can define two template helpers that convert between the upstream Trimesh2
// library's vec3-like types and CUDA float3. cuda_voxelizer's upstream
// depends on the Trimesh2 submodule (Szymon Rusinkiewicz;
// https://github.com/Forceflow/trimesh2, GPLv2) which we do NOT vendor: our
// mesh layer is objtomc::mesh::Mesh (SoA std::vector<std::array<float, 3>>),
// not Trimesh2.
//
// The two template helpers in util.h are only instantiated when called by
// name with a concrete Trimesh2 type; our adapter (src/voxel_gpu.cu) never
// calls them. Because they are templates the compiler only checks their body
// on instantiation, so an empty TriMesh.h satisfies the #include chain
// (util.h -> TriMesh.h) with zero risk.
//
// See research/gpu_port_proposal.md §3.10 open risk 6 mitigation.
// ---------------------------------------------------------------------------
#pragma once
