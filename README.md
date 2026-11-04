# OBJtoMC

Turn large OBJ and glTF meshes into vanilla Minecraft Java Edition 1.21.5 worlds, with GPU-accelerated perceptual block-colour matching.

<!-- hero: docs/images/hero.png -->
<!-- caption: a converted model loaded in an unmodified 1.21.5 client -->

## What it does

- Reads a Wavefront OBJ (+MTL) or glTF 2.0 / GLB file and voxelises its surface at a size you choose (`--voxel-size` or `--target-size`).
- Samples diffuse, emissive and alpha textures per voxel, then picks the closest Minecraft block for each voxel from a curated palette of 414 full-cube blocks.
- Writes a complete world directory (`level.dat` plus Anvil `region/*.mca` files, DataVersion 4325) that a vanilla client opens directly.
- Streams very large meshes one region at a time, so memory is bounded by a tile rather than the whole model.
- Ships with VOXFORGE, a local web GUI for converting, previewing, installing and merging models into existing saves.

## Highlights

- **Scale.** Region-aligned tiled streaming (`--tiling auto`) voxelises, matches and writes one 512x512-block region at a time; a 19.6M-voxel photogrammetry city produced 11 region files in one run.
- **Perceptual matching.** Colours are converted sRGB to Oklab and matched by Euclidean distance, with translucency-aware pool partitioning so solid surfaces never pick glass and vice versa.
- **GPU and CPU paths.** Optional CUDA voxeliser (vendored cuda_voxelizer kernel) and CUDA matcher; both are byte-identical to the CPU path and are tested for it (`--fmad=false` keeps FMA rounding out of the comparison).
- **Deterministic output.** Same input and options produce byte-identical region files, on CPU or GPU, tiled or monolithic.
- **Multi-region worlds.** Chunks are routed across positive and negative region coordinates; a world can span as many `.mca` files as the model needs.
- **Emissive and transparent branches.** Materials with emission become light-emitting blocks (froglights and similar); low-alpha materials become stained glass and other translucent blocks.
- **Extended world height.** Models taller than 384 blocks get an embedded `objtomc_height` datapack that raises the dimension limits (`--world-height`), up to the 1.21.5 maximum of -2032..2032.
- **VOXFORGE GUI.** Flask + three.js front end: drag in a model or zip, pick a size, watch the live log, view a sampled voxel showcase, install to your saves folder, or merge into an existing world at chosen coordinates.

## Quick start

Requirements: CMake >= 3.20, a C++20 compiler (GCC 11+), zlib, optionally Ninja and the CUDA toolkit.

```sh
# Build (CUDA is detected automatically; force with -DWITH_CUDA=ON or disable with -DWITH_CUDA=OFF)
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_CUDA=AUTO
cmake --build build-release

# Convert a mesh: exactly one of --voxel-size / --target-size is required
./build-release/objtomc --target-size 128 model.glb model-world
./build-release/objtomc --voxel-size 0.002 --verbose bunny.obj bunny-world

# Optional flags
#   --axis-convention {y_up,z_up}   --tiling {auto,on,off}   --cleanup {on,off}
#   --align {off,auto,<degrees>}    --semantic {off,trees,buildings,all}
#   --world-height <min_y>,<height> --world-origin <x,y,z>   --level-name <str>
#   --palette-csv <path>            --max-grid-cells <int64> --verbose
```

Copy `model-world/` into your Minecraft saves folder and open it in 1.21.5. Exit codes map to pipeline stages (0 ok, 2 usage, 3 mesh, 4 voxel, 5 palette, 6 matcher, 7 chunk, 8 anvil, 9 orchestration).

To launch VOXFORGE (expects the Release binary at `build-release/objtomc`):

```sh
pip install flask
python3 -m venv tests/oracle_venv && tests/oracle_venv/bin/pip install NBT==1.5.1 numpy   # world preview + merge
studio/start.sh          # serves http://localhost:7777
```

## Pipeline

1. **Mesh** - `src/mesh.cpp` / `src/gltf.cpp` load OBJ+MTL or glTF/GLB into one struct-of-arrays mesh with Kd / Ke / alpha and texture references.
2. **Voxel** - `src/voxel.cpp` (and `voxel_gpu.cu`) run a conservative surface voxelisation (Schwarz-Seidel) with a closest-triangle disambiguation rule; `src/tiling.cpp` splits the job into region-sized tiles.
3. **Palette** - `src/palette.cpp` loads `data/block_palette_db.csv` (average texture colour, light filtering, emission, translucency per block) and version-checks it against the vendored minecraft-data.
4. **Match** - `src/texture.cpp` samples per-voxel colour, emission and alpha; `src/matcher.cpp` (and `matcher_gpu.cu`) choose a block in Oklab with opaque / emissive / transparent branches.
5. **Chunk** - `src/chunk.cpp` builds 1.21.5 chunk NBT: paletted bit-packed sections, heightmaps, optional extended height.
6. **Region** - `src/anvil.cpp` writes zlib-compressed Anvil region files over the in-tree NBT writer (`src/nbt.cpp`).
7. **World** - `src/orchestrate.cpp` routes chunks to regions, writes `level.dat`, and embeds the height datapack when needed.

## Tested

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # ASan + UBSan, -Wall -Wextra -Wpedantic -Wconversion
cmake --build build
tests/oracle_venv/bin/pip install anvil-parser==0.9.0   # Python oracles for NBT and Anvil output
ctest --test-dir build --output-on-failure
```

72 CTest cases cover every layer, from NBT byte layouts through end-to-end world generation; three of them (`voxel_cpu_gpu_equality`, `voxel_gpu_determinism`, `matcher_cpu_gpu_equality`) only build when CUDA is enabled. Independent Python parsers re-read the generated NBT and region files. Fixture meshes include the **Stanford bunny** (`tests/fixtures/mesh/bunny.obj`), the **Cornell box** (`tests/fixtures/cornell_box`, CC BY 3.0) for multi-material and emissive coverage, **Crytek Sponza** (`tests/fixtures/sponza`, CC BY 3.0) for multi-region scale and tiling equality, plus a hand-written transparent scene and textured OBJ/glTF/GLB quads. Generated worlds for each fixture have been loaded and inspected in an unmodified 1.21.5 client.

## Project layout

```
CMakeLists.txt            Build: library targets per stage, objtomc CLI, WITH_CUDA option
include/objtomc/          Public headers (nbt, anvil, chunk, mesh, gltf, texture, palette,
                          matcher, voxel, tiling, cleanup, alignment, semantic, orchestrate)
src/                      Implementations; *_gpu.cu are the CUDA paths; voxel_gpu_shim/ adapts cuda_voxelizer
data/block_palette_db.csv Block palette (1.21.5, 414 entries)
scripts/                  extract_block_colors.py (regenerate the palette from a client jar), bench script
studio/                   VOXFORGE: voxforge.py (Flask), static/ (three.js showcase), mesh_sampler.py,
                          world_sampler.py, world_merge.py, start.sh
tests/                    CTest sources, Python oracles, fixtures (bunny, Cornell box, Sponza, ...)
third_party/              Vendored dependencies, each with LICENSE + VERSION.txt
docs/                     build.md
```

## Status / roadmap

- **v0.1** - OBJ to vanilla 1.21.5 world end to end on the CPU (single-material bunny).
- **v0.2** - Multi-material meshes with emissive and transparent branches; multi-region output across negative coordinates; CUDA voxeliser and matcher with byte-equality to CPU; translucency-aware palette pools and per-voxel texture sampling.
- **v0.3** - glTF 2.0 / GLB import (base colour, emissive and alpha textures), `--target-size`, textured models verified in-game.
- **v0.4** - Tiled streaming core with bounded memory, extended world height via embedded datapack, first city-scale conversion.
- **In progress** - Scan clean-up (`--cleanup`), facade alignment (`--align`) and semantic replacement of trees and buildings (`--semantic`) exist but are experimental; glTF MASK cutouts still voxelise solid; verification of worlds above 4096 blocks tall is ongoing.

## Third-party

Vendored under `third_party/` (see `THIRDPARTY_LICENSES.md` for pins and provenance):

- **tinyobjloader** - MIT (bundles fast_float, MIT / Apache-2.0 / BSL-1.0) - OBJ/MTL parsing.
- **stb_image** - MIT or public domain - texture decoding.
- **cgltf** - MIT - glTF 2.0 / GLB parsing.
- **cuda_voxelizer** (Jeroen Baert) - MIT - surface voxelisation CUDA kernel subset.
- **PrismarineJS/minecraft-data** - MIT - 1.21.5 block metadata (JSON only).
- **three.js** - MIT - `studio/static/vendor/three.min.js` for the VOXFORGE viewer.

Oklab conversion follows Bjorn Ottosson's public-domain reference and is inlined in `src/matcher.cpp`. Test fixtures carry their own licences (Cornell box and Crytek Sponza: CC BY 3.0, attribution in each fixture directory).

## License

MIT. See `LICENSE`.

---

made by IWDissi and the fabulous claude assistant 😋
