# Victory VF50 — Bring-Your-Own-ROM (BYOR) Setup Guide

**Victory VF50** (`Victory VF50.vst3`) is a clean-room workstation plugin containing **zero** embedded firmware ROMs, factory presets, rhythm patterns, demo songs, or drum waveforms inside the binary. Load your own legally obtained EPROM dumps at runtime.

---

## Required EPROM Images

| # | Canonical Filename | Exact Size | Contents Provided |
|---:|---|---:|---|
| 1 | `IC712 XF805 C0.BIN` | **131,072 bytes** (128 KB) | **Main-CPU ROM A**: 100 Factory FM Voices, 100 Performances, 11 Microtuning Tables, 33 DSP Effect Names, OPZ II Pitch Permutation LUT |
| 2 | `IC723 XF810 C0.BIN` | **131,072 bytes** (128 KB) | **Main-CPU ROM B**: 61 Drum Names, Choke Groups, 3 Preset Key Maps, 100 Rhythm Patterns, 3 Factory Demo Songs |
| 3 | `IC731 XF804 B0.BIN` | **32,768 bytes** (32 KB) | **Sub-CPU ROM**: OPRW Drum Start/Gate Tables, 32 YM3413 DSP Presets, Low-End Operator Attenuation Curve |
| 4 | `XF653A0-101 IC738.BIN` | **524,288 bytes** (512 KB) | **YM3602 Wave ROM**: 61 Lossless 12-Bit PCM/DPCM Percussion Samples *(requires `IC731` for waveform indexing)* |

> **Automatic Content Identification:** Images do **not** need exact filenames—matching `.BIN` / `.ROM` dumps are identified by their internal binary content signatures and exact byte counts.

---

## How to Load the Firmware After Opening the Plugin

When `Victory VF50.vst3` launches without ROMs configured, the in-plugin **External EPROM Setup Guide** opens automatically (and can be toggled anytime via the **`[LOAD ROMS]` / `[ROM SETUP]`** button in the bottom utility bar).

### Method 1 — Drag & Drop (Fastest)
- Drag and drop all 4 `.BIN`/`.ROM` images (or their containing folder) anywhere onto the VF50 plugin window.
- All 4 chips load immediately, updating the status badge to **`4 / 4 ROM CHIPS ACTIVE (FULL VF50)`**.

### Method 2 — In-Plugin File / Folder Buttons
1. Click **`LOAD ROM FOLDER...`** to select the folder containing all 4 EPROM images (or click **`SELECT .BIN FILES...`** to multi-select individual images).
2. Click **`DONE - PLAY SYNTH`** once all 4 chips show **`LOADED`**.

### Method 3 — Auto-Load Folder (Recommended)
Copy the 4 `.BIN`/`.ROM` images into:
```text
%USERPROFILE%\Documents\Victory Audio\VF50\ROMs\
```
(You can open this folder directly with the **`OPEN AUTO-LOAD FOLDER`** button inside the plugin.)
Every new instance of `Victory VF50.vst3` scans this folder automatically on startup.

Saved paths are persisted in `%APPDATA%\Victory Audio\VF50\vf50_rom_paths.cfg` and can be reset anytime with **`CLEAR EXTERNAL ROMS`**.

---

## Using `Victory VF50.vst3` Without ROMs

With `0 / 4` ROM chips loaded:
- Play and program clean-room **Init Voice** patches and **INT 00–99** memory slots using the Pop-Down **PG Programmer** sliders, **Visual Mixer**, and front-panel 40x2 LCD controls.
- Drag & Drop external 4-Op **`.SYX`** dumps or mount 720KB **`.IMG` / `.DSK`** floppy disk images (`.ALL`, `.VOI`, `.PFM`, `.SEQ`).
- Rhythm drum voices stay safely silent until `IC731` and `IC738` are loaded.

---

## Installation

Install `Victory VF50.vst3` to your system VST3 folder:
- `C:\Program Files\Common Files\VST3\Victory VF50.vst3`
