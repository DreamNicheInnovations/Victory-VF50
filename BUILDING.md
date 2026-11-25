# Building Victory VF50

**Victory VF50** uses CMake (`>= 3.22`) and C++20. By default, CMake fetches **JUCE 8.0.4** and **clap-juce-extensions** automatically via `FetchContent`.

## CMake Options

| Option | Default | Description |
|---|---:|---|
| `-DVF50_JUCE_DIR=<path>` | *(empty)* | Optional path to a pre-cloned JUCE 8 source tree (or place in `./JUCE`) for offline builds |
| `-DVF50_CLAP_EXT_DIR=<path>` | *(empty)* | Optional path to a pre-cloned `clap-juce-extensions` tree (or `./clap-juce-extensions`) |
| `-DVF50_DEPLOY_LOCAL=ON\|OFF` | `OFF` | Copy built plugins to system VST3/CLAP/AU/LV2 directories after build |

Optional environment variables for `VF50_HeadlessTest`:
- `VF50_TEST_ROM_DIR`: directory containing external BYOR EPROM `.BIN` dumps (ROM-dependent suites skip cleanly when unset)
- `VF50_TEST_DISK_DIR`: directory containing `.IMG`/`.DSK` disk images for FAT12 disk audit tests
- `VF50_TEST_SYX_DIR`: directory containing external `.SYX` SysEx test dumps

---

## Windows (`x64`, MSVC / Visual Studio 2022)

Prerequisites: CMake >= 3.22, Git, Visual Studio 2022 (Desktop C++ workload with MSVC v143 toolset).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVF50_DEPLOY_LOCAL=OFF
cmake --build build --config Release --target VictoryVF50_VST3 VictoryVF50_CLAP VictoryVF50_Standalone VictoryVF50_Multi_VST3 VictoryVF50_Multi_CLAP VF50_HeadlessTest VF50_HeadlessTest_Multi --parallel
```

To deploy `Victory VF50` and `Victory VF50 Multi` VST3/CLAP bundles automatically during the build (requires Administrator privileges for `C:\Program Files\Common Files\`):
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVF50_DEPLOY_LOCAL=ON
cmake --build build --config Release --target VictoryVF50_VST3 VictoryVF50_CLAP VictoryVF50_Standalone VictoryVF50_Multi_VST3 VictoryVF50_Multi_CLAP --parallel
```

Outputs:
- `build\VictoryVF50_artefacts\Release\VST3\Victory VF50.vst3`
- `build\VictoryVF50_artefacts\Release\CLAP\Victory VF50.clap`
- `build\VictoryVF50_artefacts\Release\Standalone\Victory VF50.exe`
- `build\VictoryVF50_Multi_artefacts\Release\VST3\Victory VF50 Multi.vst3`
- `build\VictoryVF50_Multi_artefacts\Release\CLAP\Victory VF50 Multi.clap`
- `build\VF50_HeadlessTest_artefacts\Release\VF50_HeadlessTest.exe`
- `build\VF50_HeadlessTest_Multi_artefacts\Release\VF50_HeadlessTest_Multi.exe`

Run tests & `pluginval` (level 10):
```powershell
.\build\VF50_HeadlessTest_artefacts\Release\VF50_HeadlessTest.exe
.\build\VF50_HeadlessTest_Multi_artefacts\Release\VF50_HeadlessTest_Multi.exe
.\tools\pluginval\pluginval.exe --strictness-level 10 --timeout-ms 60000 --validate ".\build\VictoryVF50_artefacts\Release\VST3\Victory VF50.vst3"
.\tools\pluginval\pluginval.exe --strictness-level 10 --timeout-ms 60000 --validate ".\build\VictoryVF50_Multi_artefacts\Release\VST3\Victory VF50 Multi.vst3"
```

---

## macOS (Universal `arm64;x86_64`)

Prerequisites: Xcode Command Line Tools, CMake >= 3.22, Git.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DVF50_DEPLOY_LOCAL=OFF
cmake --build build --config Release --target VictoryVF50_AU VictoryVF50_VST3 VictoryVF50_CLAP VictoryVF50_Standalone VictoryVF50_Multi_AU VictoryVF50_Multi_VST3 VictoryVF50_Multi_CLAP VF50_HeadlessTest VF50_HeadlessTest_Multi --parallel
```

Run tests & `pluginval`:
```bash
./build/VF50_HeadlessTest_artefacts/Release/VF50_HeadlessTest
./build/VF50_HeadlessTest_Multi_artefacts/Release/VF50_HeadlessTest_Multi
/path/to/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --timeout-ms 60000 --validate "./build/VictoryVF50_artefacts/Release/VST3/Victory VF50.vst3"
/path/to/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --timeout-ms 60000 --validate "./build/VictoryVF50_Multi_artefacts/Release/VST3/Victory VF50 Multi.vst3"
```

---

## Linux (`x64`)

Prerequisites (Ubuntu/Debian):
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  libasound2-dev libjack-jackd2-dev ladspa-sdk \
  libcurl4-openssl-dev libfreetype6-dev libx11-dev libxcomposite-dev \
  libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
  libgl1-mesa-dev libglu1-mesa-dev libfontconfig1-dev libxml2-utils xvfb
```

Configure & build:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVF50_DEPLOY_LOCAL=OFF
cmake --build build --config Release --target VictoryVF50_VST3 VictoryVF50_LV2 VictoryVF50_CLAP VictoryVF50_Standalone VictoryVF50_Multi_VST3 VictoryVF50_Multi_LV2 VictoryVF50_Multi_CLAP VF50_HeadlessTest VF50_HeadlessTest_Multi --parallel
```

Run headless tests & `pluginval` under `xvfb-run`:
```bash
xvfb-run -a ./build/VF50_HeadlessTest_artefacts/Release/VF50_HeadlessTest
xvfb-run -a ./build/VF50_HeadlessTest_Multi_artefacts/Release/VF50_HeadlessTest_Multi
xvfb-run -a ./tools/pluginval/pluginval --strictness-level 10 --timeout-ms 60000 --validate "./build/VictoryVF50_artefacts/Release/VST3/Victory VF50.vst3"
xvfb-run -a ./tools/pluginval/pluginval --strictness-level 10 --timeout-ms 60000 --validate "./build/VictoryVF50_Multi_artefacts/Release/VST3/Victory VF50 Multi.vst3"
```
