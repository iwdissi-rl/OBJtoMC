# Build

## Prerequisites (Ubuntu)

```
sudo apt install build-essential cmake ninja-build zlib1g-dev
```

Minimum versions: CMake ≥ 3.20, GCC with C++20 support (Ubuntu 22.04's stock `g++` 11.4 is sufficient), zlib (any modern). See `DECISIONS.md` 2026-05-25 "Build toolchain" for the rationale.

## Build & test

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

`Debug` builds enable AddressSanitizer and UndefinedBehaviorSanitizer; warnings are stop-the-line (`-Wall -Wextra -Wpedantic -Wconversion`). For a release build, swap `-DCMAKE_BUILD_TYPE=Debug` for `-DCMAKE_BUILD_TYPE=Release` (sanitizers are off in Release).

The smoke binary can be run directly:

```
./build/objtomc
```

It prints the project name, the C++ language standard in effect (`__cplusplus`), and the linked zlib version, then exits 0.

## Directory layout

```
CMakeLists.txt          Top-level build configuration.
src/                    C++ sources for the main objtomc binary.
include/objtomc/        Project public headers (currently empty).
tests/                  Test sources and tests/CMakeLists.txt.
cmake/                  Reserved for CMake helper modules.
third_party/            Reserved for vendored dependencies (none today).
research/               Notes from research sessions (see e.g. libs.md).
docs/                   Project documentation (this page).
build/                  Build artefacts (gitignored).
```

See `PROJECT.md` for what this project does, `PIPELINE.md` for stage status, and `DECISIONS.md` for the trail of why things are the way they are.
