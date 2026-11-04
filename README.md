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

## Getting started

This walks you from nothing to a converted model standing in your own Minecraft world. Each command goes in a terminal. Copy one line at a time.

### 1. What you need

| Tool | What it is | Required? |
|---|---|---|
| Git | Downloads the code. | Yes |
| A C++20 compiler | Turns the source code into a program. GCC 11 or newer works. | Yes |
| CMake 3.20 or newer | Sets up the build. | Yes |
| zlib (development files) | A compression library the world writer needs. | Yes |
| Ninja | Makes building faster. Without it, just leave out `-G Ninja` below. | Optional |
| Minecraft Java Edition 1.21.5 | The game that opens the worlds. | Yes, to look at results |
| Python 3.12 | Runs the VOXFORGE web GUI. | Only for the GUI |
| NVIDIA GPU + CUDA toolkit | Speeds up very large models. Without it you get the exact same output on the CPU, just slower. Needs a Turing (RTX 20-series) or newer card. | Optional |

**Ubuntu / Debian**

```sh
sudo apt update
sudo apt install git build-essential cmake ninja-build zlib1g-dev   # compiler, CMake, Ninja, zlib
sudo apt install python3.12 python3.12-venv                         # only if you want the GUI
```

**macOS** (with [Homebrew](https://brew.sh)). The project is built and tested on Linux, so macOS may need small fixes.

```sh
xcode-select --install                       # Apple's compiler and Git
brew install cmake ninja python@3.12         # CMake, Ninja, Python for the GUI
```

**Windows.** Use WSL2 (Windows Subsystem for Linux). Open PowerShell as administrator, run `wsl --install`, restart, then open the "Ubuntu" app and follow the Ubuntu steps above. The build uses GCC-style flags, so a native Windows compiler is not supported.

**GPU (optional).** Install the CUDA toolkit from NVIDIA's site (developer.nvidia.com/cuda-downloads). Check it with `nvcc --version`. If you skip this, everything still works on the CPU.

### 2. Download the code

```sh
git clone https://github.com/iwdissi-rl/OBJtoMC.git   # download the project
cd OBJtoMC                                            # go into the project folder
```

No Git? On the GitHub page click **Code → Download ZIP**, unzip it, and open a terminal in that folder.

### 3. Build the program

```sh
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release   # set up the build (finds CUDA by itself if installed)
cmake --build build-release                                   # compile; takes a few minutes the first time
./build-release/objtomc --help                                # check that the program runs
```

What you should see:
- After the first command, a line like `-- objtomc: WITH_CUDA=AUTO, effective=OFF (GPU acceleration disabled; build proceeds CPU-only)`, or `effective=ON` if CUDA was found. Then `-- Build files have been written to: ...`.
- After the last command, help text that starts with `Usage: ./build-release/objtomc [options] <input.{obj,gltf,glb}> <world_dir>`.

Keep the folder name `build-release`. The GUI looks for the program there.

To turn CUDA on or off by hand, add `-DWITH_CUDA=ON` or `-DWITH_CUDA=OFF` to the first command.

### 4. Your first run

The repo ships the Stanford bunny at `tests/fixtures/mesh/bunny.obj`. Turn it into a world 64 blocks across:

```sh
./build-release/objtomc --target-size 64 --level-name "My first bunny" --verbose tests/fixtures/mesh/bunny.obj bunny-world
```

What you should see: progress lines ending with something like

```
[anvil]   wrote bunny-world/region/r.0.0.mca
[level]   wrote bunny-world/level.dat
[orch]    done
done
```

You now have a folder called `bunny-world`. Copy it into Minecraft's `saves` folder:

| System | Saves folder | Copy command |
|---|---|---|
| Linux | `~/.minecraft/saves` | `cp -r bunny-world ~/.minecraft/saves/` |
| macOS | `~/Library/Application Support/minecraft/saves` | `cp -r bunny-world ~/Library/Application\ Support/minecraft/saves/` |
| Windows | `%APPDATA%\.minecraft\saves` | From WSL2: `cp -r bunny-world /mnt/c/Users/<your Windows user name>/AppData/Roaming/.minecraft/saves/` |

Then open it:
1. Start the Minecraft Launcher. Under **Installations**, make one for **release 1.21.5** if you don't have it.
2. Press **Play**, then **Singleplayer**.
3. Pick **My first bunny** and press **Play Selected World**.

The world is an empty void in Creative mode with cheats on. You spawn at the corner where the model starts, so the bunny is right next to you. Fly around to look at it.

Try your own model next. Exactly one of `--target-size` (size in blocks along the longest side) or `--voxel-size` (mesh units per block) is required:

```sh
./build-release/objtomc --target-size 128 model.glb model-world        # any .obj, .gltf or .glb file
```

More options (run `./build-release/objtomc --help` for details):

```
--axis-convention {y_up,z_up}   --tiling {auto,on,off}   --cleanup {on,off}
--align {off,auto,<degrees>}    --semantic {off,trees,buildings,all}
--world-height <min_y>,<height> --world-origin <x,y,z>   --level-name <str>
--palette-csv <path>            --max-grid-cells <int64> --verbose
```

Exit codes tell you which stage failed: 0 ok, 1 unexpected, 2 usage, 3 mesh, 4 voxel, 5 palette, 6 matcher, 7 chunk, 8 anvil, 9 orchestration.

### 5. Using the GUI (VOXFORGE)

VOXFORGE is a web page that runs on your own computer. It needs the program from step 3 at `build-release/objtomc`.

```sh
python3.12 -m venv tests/oracle_venv                       # make a private Python environment
tests/oracle_venv/bin/pip install flask NBT==1.5.1 numpy   # Flask runs the page; NBT + numpy power preview and merge
source tests/oracle_venv/bin/activate                      # use that environment in this terminal
studio/start.sh                                            # start VOXFORGE
```

What you should see: a line `VOXFORGE :: objtomc = .../build-release/objtomc`, then Flask saying it is running on `http://127.0.0.1:7777`.

Open **http://localhost:7777** in your browser. On WSL2 your normal Windows browser usually works.

1. Drag a `.obj`, `.gltf`, `.glb` or `.zip` onto **DRAG MODEL HERE**, or type a full path into the box below it.
2. Keep **FORGE NEW WORLD** selected. Pick a size (blocks along the longest side). For a first try, type `128`.
3. Click **▶ FORGE WORLD** and watch the log.
4. When it finishes, click **INSTALL TO MINECRAFT**. This copies the world into `~/.minecraft/saves`, which is the Linux location. On macOS or WSL2, copy the world yourself from `studio/worlds/<name>` using the table in step 4.

**IMPORT INTO WORLD** places a model inside one of your existing saves at the X/Y/Z you choose. Close that world in Minecraft first.

Press `Ctrl+C` in the terminal to stop VOXFORGE. Next time you only need the `source ...` and `studio/start.sh` lines.

### 6. Troubleshooting

- **`cmake: command not found`, `ninja: command not found` or `Could NOT find ZLIB`.** Install the step 1 packages. If you don't want Ninja, remove `-G Ninja` from the command.
- **`CMake 3.20 or higher is required`.** Your CMake is too old. Install a newer one from cmake.org/download.
- **CUDA errors during the build, or `nvcc` not found.** Build CPU-only: `rm -rf build-release`, then `cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_CUDA=OFF` and build again.
- **`world_dir already exists and is non-empty` (exit code 9).** The program won't overwrite a world. Use a new output folder name, or delete the old folder.
- **`python3.12: command not found`, `No module named 'flask'` or `externally-managed-environment`.** Install Python 3.12 (step 1; it comes with Ubuntu 24.04 and newer), then redo the step 5 lines. Use the `tests/oracle_venv/bin/pip` from the venv, not the system `pip`.
- **`Address already in use` when starting VOXFORGE.** Something else is using port 7777, often an older VOXFORGE. Close it, or change `port=7777` on the last line of `studio/voxforge.py`.

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
