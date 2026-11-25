# Victory VF50 — Build, Multi-Out, Portability & Validation Status

**Working Root:** `Workspace/`  
**Editions:**
- **`Victory VF50`** (`VF50_MULTI_OUT=0`, code `VF50`): Standard stereo plugin (`Victory VF50.vst3`, `Victory VF50.clap`, `Victory VF50.exe` Standalone)
- **`Victory VF50 Multi`** (`VF50_MULTI_OUT=1`, code `VF5M`): 10-stereo-bus companion plugin (`Victory VF50 Multi.vst3`, `Victory VF50 Multi.clap`)
**Manufacturer:** `Victory Audio` (`VA50`)  
**License:** GNU AGPLv3 (`LICENSE`)  
**Toolchain:** CMake 3.22+ / MSVC 19.44.35230.0 (Visual Studio 17 2022) / C++20 (`/W4 /WX /permissive-`) / JUCE 8.0.4 + `clap-juce-extensions` (`CLAP 1.2.10`)  
**ROM Policy:** Bring-Your-Own-ROM (`VF50_EMBEDDED_ROMS=0`) — zero embedded proprietary firmware or waveform data.

---

## Multi-Output 10-Bus Architecture (`VF50_MULTI_OUT=1`)
- **Output Buses:** Bus 0 `Main / FX Out` (stereo, enabled by default), Buses 1–8 `Inst 1 Out`–`Inst 8 Out` (stereo, aux, disabled by default), Bus 9 `Rhythm Out` (stereo, aux, disabled by default).
- **Smart Bus Routing (`VF50SynthEngine::renderMultiBusBlock`):**
  - Channels whose individual host aux bus is disabled fall back automatically into `Main / FX Out`.
  - Enabled aux buses receive post-fader, post-balance, post-EQ stereo audio phase-aligned with `Main / FX Out` with zero audio-thread heap allocations.
  - Per-strip `BUS:AUX` / `BUS:MAIN` pill in `VF50MixerDrawer` (`VF50_MULTI_OUT=1` builds only) selects isolated `AUX` routing (wet YM3413 DSP FX return remains on `Main / FX Out`) vs. parallel `MAIN` + `AUX` summing; routing mask persists in the `VF5M` v3 state trailer while staying backward-compatible with v1/v2 stereo states.

---

## Build, `pluginval`, Deploy & Audit Results

| Check | Scope / Target | Result |
|---|---|---|
| Release Multi-Format Build (`/W4 /WX`) | `VictoryVF50_VST3`, `VictoryVF50_CLAP`, `VictoryVF50_Standalone`, `VictoryVF50_Multi_VST3`, `VictoryVF50_Multi_CLAP`, `VF50_HeadlessTest`, `VF50_HeadlessTest_Multi` | **PASSED** (`0` warnings/errors) |
| `pluginval` v1.0.4 (`--strictness-level 10 --timeout-ms 60000`) | `build\VictoryVF50_artefacts\Release\VST3\Victory VF50.vst3` | **PASSED** (`PLUGINVAL_STEREO_EXIT_CODE=0`, `SUCCESS`) |
| `pluginval` v1.0.4 (`--strictness-level 10 --timeout-ms 60000`) | `build\VictoryVF50_Multi_artefacts\Release\VST3\Victory VF50 Multi.vst3` | **PASSED** (`PLUGINVAL_MULTI_EXIT_CODE=0`, `SUCCESS`, 10 stereo output buses verified) |
| `VF50_HeadlessTest.exe` (`Release`) | Env vars unset + external fixtures (`VF50_TEST_ROM_DIR` + `VF50_TEST_DISK_DIR`) | **PASSED** (`STEREO_UNSET_RC=0`, `STEREO_FULL_RC=0`) |
| `VF50_HeadlessTest_Multi.exe` (`Release`) | 10-bus layouts, isolated `AUX` / parallel `MAIN` / disabled fallback, `BUS` pill toggle + state recall, env unset + full fixtures | **PASSED** (`MULTI_UNSET_RC=0`, `MULTI_FULL_RC=0`) |
| CI & Open-Source Packaging | `.gitignore`, GNU AGPLv3 `LICENSE`, `README.md`, `BUILDING.md`, `.github/workflows/build.yml` | **COMPLETE** (builds, tests & `pluginval` level-10 checks on both editions for Windows, macOS Universal, Linux) |
| Staged (`Release_Package/`) | `Victory VF50.vst3`, `Victory VF50.clap`, `Victory VF50.exe`, `Victory VF50 Multi.vst3`, `Victory VF50 Multi.clap`, `LICENSE`, `README.md`, `BUILDING.md` | **STAGED** |
| Local Deploy (`C:\Program Files\Common Files\`) | `VST3` -> `Victory VF50.vst3` & `Victory VF50 Multi.vst3` / `CLAP` -> `.clap` bundles | **VST3 DEPLOYED (`VST3_DEPLOY=OK` for both)**; **CLAP BLOCKED (`Access to the path 'CLAP' is denied.`, non-elevated shell `IS_ADMIN=False`)** |
| Zero-Leak Text Regex Audit | `Source/`, `Tests/`, `CMakeLists.txt`, `.gitignore`, `LICENSE`, `README.md`, `BUILDING.md`, `.github/`, `Docs/**/*.md`, root `*.md` | **0 matches** across all 7 forbidden patterns |
| Zero-Leak Binary String Audit | Release `Victory VF50 Multi.vst3` (`8,088,064` B), `Victory VF50 Multi.clap` (`7,830,016` B), `Victory VF50.vst3` (`8,081,920` B), `Victory VF50.clap` (`7,823,360` B), `Victory VF50.exe` (`9,138,176` B) | **0 brand/path leaks**; only 1 hit per binary = mandatory 8-byte hardware sequence header (`0x56 0x35 0x30 0x53 0x45 0x51 0x56 0x31`) |

### Completing Local CLAP Deployment (Elevated PowerShell)
Because creating `C:\Program Files\Common Files\CLAP\` requires Administrator privileges on this machine, run the following commands from `Workspace\` in an elevated Administrator PowerShell prompt:
```powershell
New-Item -ItemType Directory -Force -Path "C:\Program Files\Common Files\CLAP" | Out-Null
Copy-Item -Force ".\Release_Package\Victory VF50.clap" "C:\Program Files\Common Files\CLAP\Victory VF50.clap"
Copy-Item -Force ".\Release_Package\Victory VF50 Multi.clap" "C:\Program Files\Common Files\CLAP\Victory VF50 Multi.clap"
```
