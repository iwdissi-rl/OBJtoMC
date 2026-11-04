# Set up OBJtoMC with Claude Code

You are Claude Code. The user wants OBJtoMC working on their machine. When you are done, the `objtomc` program is built at `build-release/objtomc`, the test suite passes, the Stanford bunny has been turned into a Minecraft Java Edition 1.21.5 world, the user knows where to copy that world, and the VOXFORGE web GUI is ready to start on http://localhost:7777. Follow the steps below in order. The README's "Getting started" section is the source for every command here; if something here and the README disagree, trust the README and tell the user.

## How to work

- Before each step, give the user a one or two line update: what you are about to do and why.
- Ask before any `sudo` command or system-wide install (apt, Homebrew, the CUDA toolkit). Show the exact command first.
- Ask before changing shell or service configuration (for example `~/.bashrc`).
- Ask before any download larger than about 1 GB (the CUDA toolkit is one).
- Never print, log or commit secrets. This project needs no API keys or `.env` file. If the user ever pastes a key, do not echo it back.
- Do not modify files outside the project folder, except for installing prerequisites. Copying the finished world into Minecraft's `saves` folder is outside the project: ask first, or give the user the command to run.
- Do not edit tracked project files. Do not run any git command that writes (`add`, `commit`, `push`, `checkout` of other branches).
- If a step fails twice, stop. Show the user the error and ask how to continue. The README's "6. Troubleshooting" list covers the common failures; check it first.

## 1. Detect the system

Run read-only checks and report the results to the user in plain words:

```sh
uname -s -m                                  # Linux or Darwin, and the CPU architecture
cat /etc/os-release 2>/dev/null | head -3    # distro and version on Linux
nvidia-smi 2>/dev/null | head -15            # NVIDIA GPU and driver, if any
nvcc --version 2>/dev/null                   # CUDA toolkit, if any
```

- **Linux (Ubuntu / Debian):** the main supported path. Use the Ubuntu steps.
- **macOS:** use the Homebrew steps. Tell the user the project is built and tested on Linux, so macOS may need small fixes.
- **Windows:** the README says to use WSL2. A native Windows compiler is not supported because the build uses GCC-style flags. Tell the user to open PowerShell as administrator, run `wsl --install`, restart, open the "Ubuntu" app and run Claude Code from there. Then continue with the Ubuntu steps inside WSL2.
- **GPU:** CUDA is optional. Without it the output is exactly the same, just slower on very large models. The CUDA path needs a Turing (RTX 20-series) or newer card. If there is no NVIDIA GPU or no `nvcc`, plan a CPU-only build and say that this is fine.

## 2. Check the prerequisites

Check each tool. Install only what is missing, and ask before installing.

| Tool | Check | Needed |
|---|---|---|
| Git | `git --version` | Yes |
| C++20 compiler (GCC 11 or newer) | `g++ --version` (macOS: `c++ --version`) | Yes |
| CMake 3.20 or newer | `cmake --version` | Yes |
| zlib development files | Ubuntu: `dpkg -s zlib1g-dev` | Yes |
| Ninja | `ninja --version` | Optional. Without it, leave out `-G Ninja` everywhere below. |
| Python 3.12 | `python3.12 --version` | For the VOXFORGE GUI and the Python test oracles |
| CUDA toolkit | `nvcc --version` | Optional |

Install commands from the README:

Ubuntu / Debian (and WSL2):

```sh
sudo apt update
sudo apt install git build-essential cmake ninja-build zlib1g-dev
sudo apt install python3.12 python3.12-venv
```

Python 3.12 comes with Ubuntu 24.04 and newer. On older releases `python3.12` may not be in apt; tell the user and ask how they want to get it.

macOS (Homebrew from https://brew.sh):

```sh
xcode-select --install
brew install cmake ninja python@3.12
```

CUDA (optional): the user installs the CUDA toolkit from developer.nvidia.com/cuda-downloads. It is a large download, so ask first. If they skip it, continue CPU-only.

If CMake is older than 3.20, the configure step fails with `CMake 3.20 or higher is required`. Tell the user to install a newer CMake from cmake.org/download.

## 3. Make sure you are inside a clone

Check that the current folder is the project:

```sh
git remote get-url origin 2>/dev/null    # should mention iwdissi-rl/OBJtoMC
ls CMakeLists.txt studio/voxforge.py tests/fixtures/mesh/bunny.obj
```

If those files are not there, ask the user where to put the project, then clone it and work from that folder:

```sh
git clone https://github.com/iwdissi-rl/OBJtoMC.git
cd OBJtoMC
```

All later commands run from the project root.

## 4. Build the program

Keep the folder name `build-release`. The GUI looks for the program there.

```sh
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/objtomc --help
```

- The first command prints a line like `-- objtomc: WITH_CUDA=AUTO, effective=OFF (GPU acceleration disabled; build proceeds CPU-only)`, or `effective=ON` if CUDA was found, then `-- Build files have been written to: ...`. Tell the user which one they got.
- The compile takes a few minutes the first time.
- `--help` prints text that starts with `Usage: ./build-release/objtomc [options] <input.{obj,gltf,glb}> <world_dir>`.

To force CUDA on or off, add `-DWITH_CUDA=ON` or `-DWITH_CUDA=OFF` to the first command. If the build fails with CUDA errors or `nvcc` not found, build CPU-only:

```sh
rm -rf build-release
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_CUDA=OFF
cmake --build build-release
```

## 5. Make the Python environment

The VOXFORGE GUI and the test oracles share one private environment at `tests/oracle_venv` (it is in `.gitignore`).

```sh
python3.12 -m venv tests/oracle_venv
tests/oracle_venv/bin/pip install flask NBT==1.5.1 numpy
tests/oracle_venv/bin/pip install anvil-parser==0.9.0
```

Use `tests/oracle_venv/bin/pip`, never the system `pip`. If you see `externally-managed-environment` or `No module named 'flask'`, you used the wrong pip or the venv was not created.

If the user chose not to install Python 3.12, skip this step and the GUI. Tell them the Python oracle tests in step 6 will fail without it.

## 6. Run the tests

The tests use a separate Debug build in `build`, with AddressSanitizer and UBSan turned on. It is another compile of a few minutes.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

There are 72 CTest cases when CUDA is enabled. Three of them (`voxel_cpu_gpu_equality`, `voxel_gpu_determinism`, `matcher_cpu_gpu_equality`) only exist in CUDA builds, so a CPU-only build has 69. Report the pass count to the user. If a test fails, show its output and ask before trying fixes.

## 7. First run: the Stanford bunny

```sh
./build-release/objtomc --target-size 64 --level-name "My first bunny" --verbose tests/fixtures/mesh/bunny.obj bunny-world
```

The output should end with lines like:

```
[anvil]   wrote bunny-world/region/r.0.0.mca
[level]   wrote bunny-world/level.dat
[orch]    done
done
```

Show the user the last lines and the contents of `bunny-world/`. If you see `world_dir already exists and is non-empty` (exit code 9), the folder is left over from an earlier run. Ask before deleting it, or use a new folder name.

## 8. Put the world into Minecraft

Tell the user where their Minecraft `saves` folder is, based on the OS from step 1, and give them the copy command. Run it only if they say yes.

| System | Saves folder | Copy command |
|---|---|---|
| Linux | `~/.minecraft/saves` | `cp -r bunny-world ~/.minecraft/saves/` |
| macOS | `~/Library/Application Support/minecraft/saves` | `cp -r bunny-world ~/Library/Application\ Support/minecraft/saves/` |
| Windows (from WSL2) | `%APPDATA%\.minecraft\saves` | `cp -r bunny-world /mnt/c/Users/<your Windows user name>/AppData/Roaming/.minecraft/saves/` |

Then tell them how to open it:

1. Start the Minecraft Launcher. Under **Installations**, make one for **release 1.21.5** if they do not have it.
2. Press **Play**, then **Singleplayer**.
3. Pick **My first bunny** and press **Play Selected World**.

The world is an empty void in Creative mode with cheats on. They spawn at the corner where the model starts, so the bunny is right next to them.

## 9. Offer to start VOXFORGE

Ask the user if they want the GUI now. If yes, start it in the background so it keeps running:

```sh
source tests/oracle_venv/bin/activate
studio/start.sh
```

It prints `VOXFORGE :: objtomc = .../build-release/objtomc`, then Flask says it is running on `http://127.0.0.1:7777`. Tell the user to open **http://localhost:7777** in their browser (on WSL2 the normal Windows browser usually works).

If you see `Address already in use`, something else is on port 7777, often an older VOXFORGE. Tell the user; do not edit `studio/voxforge.py` unless they ask.

How to use it, for the user:

1. Drag a `.obj`, `.gltf`, `.glb` or `.zip` onto **DRAG MODEL HERE**, or type a full path into the box below it.
2. Keep **FORGE NEW WORLD** selected and pick a size in blocks (try `128`).
3. Click **▶ FORGE WORLD** and watch the log.
4. Click **INSTALL TO MINECRAFT**. This copies into `~/.minecraft/saves`, the Linux location. On macOS or WSL2 they copy the world from `studio/worlds/<name>` themselves, using the table in step 8.

**IMPORT INTO WORLD** places a model inside one of their existing saves at chosen X/Y/Z. They must close that world in Minecraft first. `Ctrl+C` stops VOXFORGE.

## 10. Tell the user what to do next

Give a short summary: what was installed, whether the build is CPU-only or CUDA, the test result, and where the bunny world is. Then give them these commands, run from the project folder:

```sh
./build-release/objtomc --target-size 128 model.glb model-world   # convert your own .obj, .gltf or .glb
./build-release/objtomc --help                                   # all options
source tests/oracle_venv/bin/activate && studio/start.sh         # start VOXFORGE, then open http://localhost:7777
ctest --test-dir build --output-on-failure                       # rerun the tests
```

Remind them: exactly one of `--target-size` (blocks along the longest side) or `--voxel-size` (mesh units per block) is required, and the program never overwrites an existing world folder. Exit codes name the failed stage: 0 ok, 1 unexpected, 2 usage, 3 mesh, 4 voxel, 5 palette, 6 matcher, 7 chunk, 8 anvil, 9 orchestration.
