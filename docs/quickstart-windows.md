# Gateway quickstart: Windows

The Linux/macOS path (`gateway/README.md`) gets MAVSDK and spdlog from system
packages or Homebrew and protobuf from apt/Homebrew too. Windows has no
equivalent package manager for any of these, so `gateway/CMakeLists.txt`
prepends vendored install trees under `gateway/third_party/` to
`CMAKE_PREFIX_PATH` instead (see the `if(WIN32)` block). This doc is the
complete path from a bare Windows machine to a running `karshipta_gateway.exe`
and a passing test suite.

`gateway/third_party/` is gitignored: it does not travel with the repo and
must be rebuilt by hand on every new Windows machine. Treat it as
irreplaceable once built; see the "never delete gitignored vendor dirs" rule
if you're touching build config near it.

## 0. Install prerequisites

- **Visual Studio Build Tools** (MSVC, C++20 support) with the "Desktop
  development with C++" workload:
  ```
  winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  ```
- **CMake 3.20+**, **Ninja**, **Git**:
  ```
  winget install Kitware.CMake Ninja-build.Ninja Git.Git
  ```
- **Docker Desktop** (for PX4 SITL, needed to run the gateway against a
  ward; WSL2 backend):
  ```
  winget install Docker.DockerDesktop
  ```
- `curl` and `tar` ship with Windows 10 1803+/11 by default; no install
  needed.

Restart your shell after installing so `PATH` picks up the new tools.

All commands below assume a Developer shell with MSVC on `PATH`:

```
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

(adjust the path to match your actual VS Build Tools install location), and
that `<repo>` is the absolute path to this repository checkout.

## 1. Clone the repo

```
git clone https://github.com/NIKX-Tech/karshipta.git
cd karshipta
```

## 2. MAVSDK (prebuilt release)

```
curl -LO https://github.com/mavlink/MAVSDK/releases/download/v3.17.1/mavsdk-windows-x64.zip
tar -xf mavsdk-windows-x64.zip -C <repo>/gateway/third_party/mavsdk
```

Match the release tag to whatever version the rest of the team is on; check
https://github.com/mavlink/MAVSDK/releases for newer tags. The extracted tree
should end up with `include/`, `lib/`, and `Debug/`/`Release/` directly under
`third_party/mavsdk`.

## 3. spdlog (build from source)

```
git clone --branch v1.14.1 --depth 1 https://github.com/gabime/spdlog.git spdlog-src
cmake -S spdlog-src -B spdlog-src/build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX=<repo>/gateway/third_party/spdlog
cmake --build spdlog-src/build
cmake --install spdlog-src/build
```

spdlog defaults to a static library, which is what's vendored (no `bin/`
directory, just `include/` and `lib/`); don't pass `-DSPDLOG_BUILD_SHARED=ON`.
`spdlog-src` can be deleted once installed.

## 4. protobuf (build from source)

```
git clone --branch v21.12 --recurse-submodules --depth 1 https://github.com/protocolbuffers/protobuf.git protobuf-src
cmake -S protobuf-src -B protobuf-src/build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -Dprotobuf_BUILD_TESTS=OFF ^
  -Dprotobuf_BUILD_SHARED_LIBS=ON ^
  -Dprotobuf_MSVC_STATIC_RUNTIME=OFF ^
  -DCMAKE_INSTALL_PREFIX=<repo>/gateway/third_party/protobuf
cmake --build protobuf-src/build
cmake --install protobuf-src/build
```

Tag `v21.12` on the protobuf repo corresponds to the `3.21.12` version string
CMake's `find_package(Protobuf)` reports (protobuf dropped the `3.` prefix
from its release tags starting around v21). `--recurse-submodules` is
required: protobuf vendors abseil as a submodule and the build fails without
it. `protobuf_BUILD_SHARED_LIBS=ON` matches the vendored tree, which has a
`bin/` directory (DLLs) alongside `lib/` (import libs). `protobuf-src` can be
deleted once installed. This step also produces `protoc.exe`
(`third_party/protobuf/bin/protoc.exe`), which `gateway/CMakeLists.txt` uses
at configure time to generate C++ bindings from `proto/karshipta/v1/*.proto`
into `gateway/gen/`; there is no separate manual codegen step.

## 5. GTest (vcpkg, classic mode; only needed to build tests)

Unlike the three libraries above, GTest is not vendored by hand; it's
installed through vcpkg in classic mode (not the project's manifest, which
only lists `spdlog` for a different purpose). If `gateway/third_party/vcpkg`
doesn't already exist:

```
git clone https://github.com/microsoft/vcpkg <repo>/gateway/third_party/vcpkg
<repo>/gateway/third_party/vcpkg/bootstrap-vcpkg.bat
```

Then install gtest:

```
<repo>/gateway/third_party/vcpkg/vcpkg.exe install gtest:x64-windows --classic
```

This populates `gateway/third_party/vcpkg/installed/x64-windows`, which is
already one of the `CMAKE_PREFIX_PATH` entries `gateway/CMakeLists.txt` adds
on `WIN32`, so no further wiring is needed. GTest is only required when
configuring with `-DKARSHIPTA_GATEWAY_BUILD_TESTS=ON`; skip this step if you
only need to build `karshipta_gateway` itself.

ixwebsocket needs no manual step on Windows: `CMakeLists.txt` looks for a
vcpkg/Homebrew config first and, finding none here, falls back to
`FetchContent` (downloaded automatically at configure time) with TLS and
zlib both off.

## CMAKE_PREFIX_PATH wiring (for reference, already automatic)

None of the steps above needs manual `CMAKE_PREFIX_PATH` flags at configure
time. `gateway/CMakeLists.txt` already does this under `if(WIN32)`:

```cmake
list(APPEND CMAKE_PREFIX_PATH
  "${CMAKE_SOURCE_DIR}/third_party/mavsdk"
  "${CMAKE_SOURCE_DIR}/third_party/spdlog"
  "${CMAKE_SOURCE_DIR}/third_party/protobuf"
  "${CMAKE_SOURCE_DIR}/third_party/vcpkg/installed/x64-windows"
)
```

As long as the four directories above exist with the layouts described,
`find_package(MAVSDK REQUIRED)`, `find_package(spdlog REQUIRED)`,
`find_package(Protobuf CONFIG QUIET)`, and (when building tests)
`find_package(GTest REQUIRED)` all resolve without extra configure flags.

## 6. Run PX4 SITL

Separate terminal, needs Docker Desktop running:

```
docker run --rm -it -p 14550:14550/udp -p 14540:14540/udp px4io/px4-sitl:latest
```

`main.cpp` connects to `udpin://0.0.0.0:14540` (PX4's SDK port; 14550 is for
QGroundControl). If the gateway doesn't see the ward, from the PX4 shell
(`pxh>`): `param set MAV_0_BROADCAST 1`, `mavlink start -u 14540 -o 14540 -r 4000000`,
`reboot`.

## 7. Build and run the gateway

```
cmake -S gateway -B gateway/build -G Ninja
cmake --build gateway/build
gateway/build/src/karshipta_gateway.exe
```

The post-build step copies MAVSDK's and protobuf's runtime DLLs next to the
executable automatically (`$<TARGET_RUNTIME_DLLS:...>` in
`gateway/src/CMakeLists.txt`); no manual DLL copying is needed. Expected
output once the ward connects: one log line per second with `lat`, `lon`,
`alt_m`, and `battery_pct`.

## 8. Tests

Requires GTest from step 5.

```
cmake -S gateway -B gateway/build_tests -G Ninja -DKARSHIPTA_GATEWAY_BUILD_TESTS=ON
cmake --build gateway/build_tests
ctest --test-dir gateway/build_tests
```
