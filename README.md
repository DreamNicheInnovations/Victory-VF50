# Victory VF50

**Victory VF50** is an open-source, clean-room 4-operator FM music workstation synthesizer plugin and standalone instrument written in C++20 and JUCE 8. It contains **zero** embedded proprietary firmware, factory banks, rhythm patterns, demo sequences, or drum samples—users load legally obtained hardware EPROM dumps at runtime (**Bring-Your-Own-ROM / BYOR**) or operate directly in ROM-free Init synthesis mode.

## Features

- **4-Operator OPZ II FM Engine:** 16-voice polyphony, 8 synthesis algorithms, 8 oscillator waveforms, feedback, dual D1R/D2R envelope decay, fixed-frequency mode, LFO with multi-waveform modulation, and portamento.
- **8-Part Multitimbral Performance Mode:** Independent part key limits, note shift, detune, channel balance, FX sends, polyphony reserve/max limits, and per-part voice routing.
- **YM3602 OPRW PCM/DPCM Rhythm Section:** 61 drum voices with choke groups, 3 assignable key maps, step/pattern/song playback, and accompanying rhythm sync when external EPROMs are loaded.
- **YM3413 DSP Multi-Effect Processor:** Stereo reverb, delay, distortion + reverb, and modulation effect algorithms with per-channel send levels in the Visual Mixer.
- **8-Track + Rhythm Sequencer & MIDI Export:** Plays factory demo sequences and imported multi-track hardware sequence files, with format-1 SMF drag-and-drop and disk export.
- **Modern & Classic Workflows:** 40x2 hardware-style dot-matrix LCD with selectable color themes, Classic multi-page state machine, pop-down 63-slider PG Programmer drawer, 9-channel Visual Mixer with 2-band EQ and master safety limiter, and virtual 61-key keyboard.
- **Virtual Disk & SysEx Support:** Mounts 720 KB `.IMG`/`.DSK` FAT12 disk images, User Disk directories (`<Documents>/Victory VF50/User Disk`), `.V##`/`.I##`/`.A##`/`.R##`/`.C##`/`.B##` workstation files, and 4-op `.SYX` bulk/edit SysEx dumps.

## Supported Formats

| Platform | Architecture | Formats |
|---|---|---|
| **Windows** | `x64` | `VST3`, `CLAP`, `Standalone` |
| **macOS** | Universal (`arm64` + `x86_64`) | `AU`, `VST3`, `CLAP`, `Standalone` |
| **Linux** | `x64` | `VST3`, `LV2`, `CLAP`, `Standalone` |

Two companion plugin editions are built side-by-side:
- **`Victory VF50`**: Standard stereo output (`Output` L/R + Standalone app).
- **`Victory VF50 Multi`**: 10-stereo-bus edition (`Main / FX Out`, `Inst 1 Out`–`Inst 8 Out`, `Rhythm Out`) with per-strip `BUS:AUX` / `BUS:MAIN` routing pills in the Visual Mixer (wet YM3413 DSP FX return remains on `Main / FX Out`; disabled aux buses sum automatically into `Main / FX Out`).

> **macOS Gatekeeper Note:** Unsigned community or CI builds require clearing quarantine attributes before DAWs or macOS will load them:
> ```bash
> xattr -cr "Victory VF50.vst3" "Victory VF50.clap" "Victory VF50.component" "Victory VF50.app" "Victory VF50 Multi.vst3" "Victory VF50 Multi.clap" "Victory VF50 Multi.component"
> ```

## Bring-Your-Own-ROM (BYOR) Setup

Without external EPROM images, **Victory VF50** runs cleanly in **ROM-Free Init Mode** (`0 / 4 ROMs`) with clean-room init voices, programmable INT/CARD slots, Visual Mixer, PG Programmer, `.SYX` import, and disk loading (drum voices remain silent until `IC731` and `IC738` are provided).

When supplying your own dumped EPROM images, **Victory VF50** identifies each chip by exact byte size and content signature rather than filename:

| Chip | Expected Image | Exact Size | SHA-256 Checksum |
|---|---|---:|---|
| Main-CPU ROM A (`IC712`) | `IC712 XF805 C0.BIN` | `131,072` bytes | `59092B9462BD50FBF3CA75BBDDE3EE27EE30E08AFB8475D86D6470619813C7C2` |
| Main-CPU ROM B (`IC723`) | `IC723 XF810 C0.BIN` | `131,072` bytes | `01DC36A698DF73746EA143159908553B3C355307E974F64BEDD2579E1C04AFD2` |
| Sub-CPU ROM (`IC731`) | `IC731 XF804 B0.BIN` | `32,768` bytes | `4738D00DB371FCCBDBDFF4A6829A390600992AFE0D833A92250DA830351D252C` |
| Drum Wave ROM (`IC738`) | `XF653A0-101 IC738.BIN` | `524,288` bytes | `CE1872955311A6185FC3E5C4727D586A5AB412E0DC37A25B9F2A50C44F15F3D0` |

### Loading EPROM Images in the Plugin
1. **Drag & Drop:** Drop the `.BIN`/`.ROM` images (or their folder) onto the plugin window.
2. **In-Plugin Setup Overlay:** Click `[LOAD ROMS]` / `[ROM SETUP]` and choose `LOAD ROM FOLDER...` or `SELECT .BIN FILES...`.
3. **Auto-Load Folder:** Copy the 4 images into `<Documents>/Victory Audio/VF50/ROMs/` for automatic discovery on every plugin launch. Resolved paths are saved in `<AppData>/Victory Audio/VF50/vf50_rom_paths.cfg`.

## Building from Source

See [BUILDING.md](BUILDING.md) for CMake prerequisites, platform commands (Windows, macOS Universal, Linux), `VF50_HeadlessTest`, and `pluginval` strictness-10 validation instructions.

## License

**Victory VF50** is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)** matching JUCE 8 open-source licensing. See [LICENSE](LICENSE) for full terms. `clap-juce-extensions` is incorporated under the MIT License.
