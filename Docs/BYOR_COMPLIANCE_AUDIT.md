# Gate 7 — Strict BYOR & Trademark Compliance Audit Report

**Date:** 2026-11-21  
**Product:** Victory VF50 (`VF50` / `Victory VF50.vst3`)  
**Manufacturer:** Victory Audio (`VA50`)  
**Plugin Code:** `VF50`  
**Build Configuration:** Windows x64 `Release` (MSVC 17.14 / CMake 3.22 / JUCE 8.0.4)

---

## 1. Zero Embedded ROM Assets Audit

### 1.1 Build System & Preprocessor Configuration (`CMakeLists.txt`)
- Audited `Workspace/CMakeLists.txt`:
  - `juce_add_binary_data` (`BinaryData`) is **not used anywhere** in the project.
  - All build targets (`VF50_DSP`, `VF50_DSP_NoROM`, `VF50_VSTi`, `VF50_VSTi_NoROM`, `VF50_HeadlessTest`, `VF50_HeadlessTest_NoROM`) explicitly compile with `VF50_EMBEDDED_ROMS=0`.
  - No post-build scripts copy `.BIN`, `.ROM`, or `.IMG` files into the `.vst3` bundle or `Release_Package/`.

### 1.2 Source Tree & Generated Headers (`Source/ROM/` & `Source/Generated/`)
- Audited `Source/ROM/VF50RomBank.h` and `Source/ROM/VF50RomBank.cpp`:
  - `VF50_EMBEDDED_ROMS` is fixed to `0`.
  - `getDefaultRomImage()` exclusively calls `initCleanRoomBlank()`, which algorithmically constructs:
    - 100 clean-room `INIT VOICE` patches (Sine Algo 1, Op 1 TL=0 / Op 2–4 TL=99, playable out of the box).
    - 100 clean-room `INIT PERF` performances.
    - Synthesized sine/click fallback drum placeholders and silent 1-bar empty demo song placeholders.
- Audited `Source/Generated/`:
  - `VF50FactoryPresets.h`: Clean-room type-forwarding stub (`506 bytes`, zero embedded preset byte arrays).
  - `VF50DrumWaveforms.h`: Clean-room type-forwarding stub (`498 bytes`, zero embedded PCM drum samples).
  - `VF50DemoSong.h`: Clean-room type-forwarding stub (`483 bytes`, zero embedded demo song event arrays).
  - `VF50ParameterLayout.h`: Removed legacy `#if VF50_EMBEDDED_ROMS` dead preprocessor branch so all 647 APVTS parameter definitions are 100% clean-room generated.

### 1.3 Graceful Empty-ROM Boot Verification
- Verified in `VF50_HeadlessTest` (`[SelfTest:NoROM]` and `Gate 7 Test 1`):
  - Launching `VF50AudioProcessor` and `VF50AudioProcessorEditor` with zero external ROMs configured (`hasExternalRomsLoaded() == false`, `loadedExternalRomChipMask() == 0x00`) boots cleanly with zero crashes, zero exceptions, and zero silence-locks.
  - Triggering MIDI notes in empty-ROM mode renders clean 4-Op FM `INIT VOICE` audio (`peak = 0.2512f`).
  - `VF50MainCanvas` automatically opens the `VF50RomGuideOverlay` (`isRomGuideVisible() == true`) directing the user to supply their own legally obtained EPROM dumps (`IC712`/`XF805`, `IC713`/`XF810`, `IC731`/`XF804`, `IC738`/`XF653`) via drag-and-drop, folder browser, or `Documents\Victory Audio\VF50\ROMs`.

---

## 2. Trademark & Metadata Scrub Audit

### 2.1 VST3 Bundle Metadata & Windows Version Info
- Audited `CMakeLists.txt`, `Source/Plugin/PluginProcessor.h`, `VF50_VSTi_resources.rc`, and `Contents/Resources/moduleinfo.json`:
  - `PLUGIN_NAME` / `JucePlugin_Name`: `"Victory VF50"`
  - `PRODUCT_NAME` / `FileDescription` / `ProductName`: `"VF50_VSTi"`
  - `COMPANY_NAME` / `Vendor`: `"Victory Audio"`
  - `BUNDLE_ID`: `"com.victoryaudio.vf50"`
  - `PLUGIN_MANUFACTURER_CODE`: `VA50`
  - `PLUGIN_CODE`: `VF50`
  - Auto-load ROM folder path (`VF50RomLoader.cpp`): `Documents\Victory Audio\VF50\ROMs`

### 2.2 User-Facing GUI & Classic Mode 40x2 LCD Boot Splash
- Audited `Source/GUI/` (`VF50MainCanvas.cpp`, `VF50ProgrammerDrawer.cpp`, `VF50RomGuideOverlay.cpp`, `VF50LookAndFeel.cpp`):
  - Top panel hardware badge: `"VICTORY"` / `"VF50"` / `"DIGITAL SYNTHESIZER WORKSTATION"`.
  - Programmer drawer header: `"PG-50  FM & EFFECTS PROGRAMMER"`.
  - BYOR Setup Guide header: `"VICTORY VF50  -  BRING-YOUR-OWN-ROM (BYOR) SETUP GUIDE"`.
- Audited Classic Mode Boot Splash (`Source/Classic/VF50ClassicController.cpp` — `ClassicScreen::PowerOnGreeting`):
  - **Line 1 (`line0`, 40 chars):** `"******     Victory VF50 Synth     ******"` (neutral branding, zero Hardware strings).
  - **Line 2 (`line1`, 40 chars):** `">>>>       Nice to meet you !!      <<<<"` (user-editable 40-character greeting via `[BANK]` hold at power-on / `[CLASSIC]` boot, persisted in DAW state).

### 2.3 Compiled Binary String Scan (`Victory VF50.vst3`)
Scanned `Victory VF50.vst3\Contents\x86_64-win\Victory VF50.vst3` (`7,883,776 bytes`) across both ASCII and UTF-16LE encodings:

| Search Pattern | ASCII Matches | UTF-16LE Matches | Audit Result |
| :--- | :---: | :---: | :--- |
| `HARDWARE` | `0` | `0` | **PASS** (Zero occurrences) |
| `Hardware` | `0` | `0` | **PASS** (Zero occurrences) |
| `hardware` | `0` | `0` | **PASS** (Zero occurrences) |
| `Strings 1 ` (Factory ROM Voice 00) | `0` | `0` | **PASS** (Zero embedded voice ROM data) |

---

## 3. Release Archive & VST3 Validation Audit

### 3.1 Zip Archive Content Inspection (`Release_Package/VF50_v1.0_Windows_VST3_x64.zip`)
Archive size: `3,324,059 bytes` (`3.17 MB`). Verified exact entry list:
1. `Victory VF50.vst3\Contents\Resources\moduleinfo.json` (`1,445 bytes`)
2. `Victory VF50.vst3\Contents\x86_64-win\Victory VF50.vst3` (`7,883,776 bytes`)
3. `README.txt` (`5,578 bytes`)
4. `CHANGELOG.md` (`2,359 bytes`)
5. `LICENSE.txt` (`2,349 bytes`)

- **Zero** `.pdb` debug symbols, `.lib`/`.exp` import libraries, `.bin`/`.rom` firmware files, `.img`/`.dsk` floppy dumps, or temporary files exist in the archive.

### 3.2 VST3 Validator & Headless Stress Suite
- Checked system `PATH` and workspace for `pluginval` and Steinberg `validator.exe` (neither external tool is installed on this build machine).
- Executed the comprehensive `VF50_HeadlessTest` suite (`Release/VF50_HeadlessTest.exe`), covering all Stages 2–7, Gates 1–6, and the new Gate 7 state save/reload round-trip and multi-rate/multi-buffer audio engine stress suite (`44100`/`48000`/`88200`/`96000 Hz`, buffer sizes `0`/`32`/`64`/`137`/`512`/`2048`, 56-note polyphonic bursts, live `MODERN <-> CLASSIC` and `SEQ -> PERFORMANCE` switching): **100% PASS (`0` errors, `0` NaN/Inf/denormals, max peak `0.99f <= 4.0f`)**.
