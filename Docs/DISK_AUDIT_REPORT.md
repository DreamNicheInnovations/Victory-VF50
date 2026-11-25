# Victory VF50 Floppy Disk & Rhythm Engine Binary Audit Report

**Date:** 2026-11-23  
**Audited Read-Only Fixtures:**
- `"Workspace\VF50_TEST_DISK_DIR\"`: `DISK-A5.IMG`, `Disk-A2.img`, `Disk-A3.IMG`, `Disk-A4.IMG`, plus loose `Disk A1\` (`17` valid VF50 files + `3` MS-DOS `._DD` recovery files + `1` `.zip`)
- `"Workspace\EPROM Images\Firmware EPROMs\"`: `IC712 XF805 C0.BIN`, `IC723 XF810 C0.BIN`

**Audited Codebase Targets:**
- `Source/Disk/VF50DiskManager.h` / `Source/Disk/VF50DiskManager.cpp`
- `Source/Classic/VF50ClassicController.cpp` & `Source/Classic/VF50ClassicRhythmScreens.cpp` (`VF50ClassicStateMachine`)
- `Source/DSP/Drums/Ym3602DrumEngine.h` / `Source/DSP/Drums/Ym3602DrumEngine.cpp`
- `Source/DSP/VF50DemoSongPlayer.h`
- `Source/ROM/VF50RomLoader.cpp`
- `Source/State/VF50MemoryManager.h` / `Source/State/VF50MemoryManager.cpp`
- `Source/Plugin/PluginProcessor.h` / `Source/Plugin/PluginProcessor.cpp` & `Source/Plugin/PluginEditor.cpp`
- `Source/GUI/VF50MainCanvas.cpp`

---

## 1. Executive Summary

Full binary inspection of all four real Victory VF50 720 KB (2DD) floppy images (`81` FAT12 root directory entries, including three `>64 KB` files up to `74,752` bytes), all `21` files in `Disk A1\`, and HD6303 firmware disassembly of `IC712 XF805 C0.BIN` (`$BBAF = "VIARCB"`) and `IC723 XF810 C0.BIN` (`$A824`, `$A96C`, `$AA9C`, `$0F0AA`) uncovered **9 concrete defects** spanning disk extension filtering, directory KB size formatting, FAT12 non-VF50 entry exclusion, Rhythm Song decoding, Rhythm Pattern accent/sync handling, and Sequencer-to-Rhythm lockstep playback:

1. **Unrecognized MS-DOS recovery files (`._DD`) & `.V00`/`.S##` still leaked through the extension classifier and FAT12 directory scanner** (`VF50DiskManager.cpp`, `VF50DiskManager.h`, `PluginEditor.cpp`).
2. **`enumerateFat12Directory()` added `VF50FileKind::Unknown` entries** to `entries_`, allowing non-VF50 files on FAT12 floppies to appear in Classic and Modern directories.
3. **Directory KB size formatting used incorrect rounding/truncation** (`(bytes + 512) / 1024` in `VF50ClassicController.cpp`, `bytes / 1024` in `VF50MainCanvas.cpp`) instead of 32-bit ceiling KB `(bytes + 1023) / 1024`.
4. **Rhythm Songs (`1–8`) were completely undecoded in `VF50DiskManager::applyHardwareRhythmRamBlock()`** — real hardware `.V##` and `.R##` files containing Rhythm Songs (`LONELY.V09`, `CLUBSONG.V08`, `OLDDRUMS.R02`, `DRUMZ.R01`–`R03`, `AURISTIC.V05`, `YIPYSKPY.V07`, `FAR OUT!.V08`, `GOOFIN'.V09`, `B.L.DEAD.V10`) had their song arrangements completely discarded, and `VF50MemoryManager` did not own the 8 Rhythm Songs.
5. **Rhythm Song bytecode length bug in `VF50RomLoader.cpp` (`0xC9` & `0xCB`)**: HD6303 firmware table `$A824 = [1, 1, 3, 3, 4, 3]` proves that `0xC9` (`:|| x N`), `0xCA` (`Vol`), `0xCB` (`Tempo`), and `0xCC` (`Mark`) are **bookended** opcodes (`C9 <extraRepeats> C9` = **3 bytes**, `CA <volDelta> CA` = **3 bytes**, `CB <tempoDelta> <beats> CB` = **4 bytes**, `CC <mark> CC` = **3 bytes**). `VF50RomLoader.cpp` advanced `0xC9` by 2 bytes and `0xCB` by 3 bytes, and lacked nested repeat stack handling.
6. **Repeat count off-by-one semantics (`0xC9 <N> 0xC9`)**: HD6303 `$AA9C` (`handler_C9`) proves `<N>` is the count of **extra repeats** (total plays = `(N & 0x7F) + 1`, e.g., `CLUBSONG.V08` `c9 3b c9` -> `59 + 1 = 60` plays; `LONELY.V09` `c9 03 c9` -> `3 + 1 = 4` plays). `Ym3602DrumEngine` expects `part.repeatCount` as total plays (`2..99`).
7. **`HardwareSeqV1` Rhythm Sync Link (`buf[31]` / `buf[32]`) was ignored** by `parseSingleVF50SeqV1Chunk()` and `PluginProcessor::startAccompanyingRhythmPatternForSong()`: `buf[31] == 1` selects Rhythm Song `buf[32] & 0x07` (`1–8`) while `buf[31] == 0` selects Rhythm Pattern `buf[32]` (`0..99 = I00..I99`, `100..199 = P00..P99`). `ALL` loads blindly selected the first populated pattern instead of the song's authentic linked Rhythm Song or Pattern.
8. **Rhythm Setup global header fields (`0x20` Rhythm Volume, `0x2A` Active Pattern, `0x2D` Active Song) were ignored** in `applyHardwareRhythmSetupHeader()`.
9. **Save/Reload symmetry**: `buildRhythmDataBytes()` and `buildAllDataBytes()` did not serialize/restore the 8 Rhythm Songs alongside the 100 User Rhythm Patterns.

---

## 2. Real Floppy Disk Binary Audit Results

### 2.1 FAT12 Geometry & Boot Sector Findings

All four `.IMG` images (`DISK-A5.IMG`, `Disk-A2.img`, `Disk-A3.IMG`, `Disk-A4.IMG`) are authentic Victory VF50-formatted 3.5" 2DD 720 KB raw floppy dumps:

| Property | Observed Hardware Value (All 4 `.IMG` Files) |
|---|---|
| Total Image Size | `737,280` bytes (`1,440` sectors × `512` bytes) |
| Sector 0 (`0x0000–0x01FF`) | Filled uniformly with `0xE5` (no DOS BPB, no `0x55 0xAA`) |
| FAT1 (`0x0200–0x07FF`) & FAT2 (`0x0800–0x0DFF`) | Identical (`FAT1 == FAT2`), `3` sectors/FAT, media header `F9 FF FF` |
| Root Directory (`0x0E00–0x1BFF`) | `112` entries × `32` bytes (`3,584` B = `7` sectors), DOS 8.3 |
| Data Area (`Sector 14`, `0x1C00`) | `2` sectors/cluster (`1,024` B/cluster), `713` clusters (`2..714`) |
| 32-Bit File Sizes (`0x1C..0x1F`) | Range `2,048` B (`2 KB`) to **`74,752` B (`73 KB`)**; **3 files exceed 64 KB** (`DISCOMAN.V07` = `71,680` B, `INDSTRL1.V10` = `73,728` B, `PARTYSNG.V11` = `74,752` B) |
| Cluster Chain Extraction | **81 / 81** files extract exact 32-bit byte sizes (`0` truncations, all terminate at `0xFFF`) |

### 2.2 File Type Inventory Across All Read-Only Fixtures

| Fixture | `.V##` (`ALL`) | `.I##` (`SYN`) | `.A##` (`SEQ`) | `.R##` (`R.SEQ`) | Non-VF50 (Must Exclude) | Total Valid VF50 |
|---|---:|---:|---:|---:|---:|---:|
| `DISK-A5.IMG` | 9 | 4 | 11 | 2 | 0 | 26 |
| `Disk-A2.img` | 10 | 2 | 9 | 4 | 0 | 25 |
| `Disk-A3.IMG` | 9 | 0 | 0 | 2 | 0 | 11 |
| `Disk-A4.IMG` | 11 | 0 | 3 | 5 | 0 | 19 |
| **Subtotal (`.IMG`)** | **39** | **6** | **23** | **13** | **0** | **81** |
| `Disk A1\` (loose) | 10 | 2 | 4 | 1 | 4 (`3×._DD`, `1×.zip`) | 17 |

### 2.3 Verified Binary Chunk Layouts

1. **`.I01–.I99` (`SYN` — Internal Synth Data, exact `26,624` B = `0x6800`):**
   - `0x0000–0x03FF` (`1,024` B): Synth Setup header (40-char greeting split at `0x2B..0x3A` and `0x4B..0x62`).
   - `0x0400–0x35FF` (`12,800` B): `100 × 128` B Internal Performances (`PFI00–PFI99`, 97 B ROM + 31 B pad, name `+66..+75`).
   - `0x3600–0x67FF` (`12,800` B): `100 × 128` B Internal Voices (`I00–I99`, 88 B base + 6 B pad + 7 B YM3413 DSP `+94..+100` + 27 B pad, name `+57..+66`).
   - Contains **NO** Rhythm data and **NO** Sequencer songs.

2. **`.A01–.A99` (`SEQ` — Single Sequencer Song, `2,048`–`29,696` B):**
   - Offset `0x0000`: `"HardwareSeqV1"` (or `"QX5FD "`):
     - `+0x09`: Time signature code (`0..3` = `1/4..4/4`, `4..11` = `1/8..8/8`, `12..27` = `1/16..16/16`).
     - `+0x0A..+0x0B`: 16-bit big-endian initial BPM (`76`–`298` observed).
     - `+0x0C..+0x13`: 8-byte song title.
     - `+0x14..+0x1B`: 8 track MIDI channels (`0..15`).
     - `+0x1C`: Play mode (`0` = Single, `1` = Performance).
     - `+0x1D`: Bank (`0` = INT, `1/2` = Preset), `+0x1E`: Patch number (`0..99`).
     - `+0x1F` (`buf[31]`): **Rhythm Sync Mode** (`0` = Rhythm Pattern, `1` = Rhythm Song).
     - `+0x20` (`buf[32]`): **Rhythm Target Index** (if `buf[31] == 1`: Rhythm Song `0..7`; if `buf[31] == 0`: `0..99` = Internal `I00..I99`, `100..199` = Preset `P00..P99`).
     - `+0x22..`: Event stream terminated by `0xF9`.
   - Contains **NO** Synth voices/performances and **NO** Rhythm RAM.

3. **`.R01–.R99` (`R.SEQ` — Rhythm Setup + Patterns & Songs, `2,048`–`11,264` B):**
   - `0x0000–0x03FF` (`1,024` B): Rhythm Setup Header:
     - `+0x20`: Rhythm Master Volume (`0..99`, observed `85`–`95`).
     - `+0x2A`: Active Rhythm Pattern (`0..99 = I00..I99`, `100..199 = P00..P99`, e.g., `KISS ME.R04` = `0x40` = `I64`; `HEAVEN!!.V02` = `0xC1` = `P93`; `HIP-HOP!.V04` = `0x95` = `P49`).
     - `+0x2D`: Active Rhythm Song (`0..7`).
     - `+0x100..0x13C`: 61 drum volumes (`0..15`).
     - `+0x13D..0x179`: 61 drum pans (`0..6`, center `3` -> `-3..+3`).
     - `+0x17A..0x1B6`: 61 drum MIDI note assignments (`0..127`).
   - `0x0400..`: Hardware `$3800` Rhythm RAM:
     - `+0x000..0x0C7`: `100 × uint16_be` Internal Pattern pointers (`I00–I99`, bit 15 = populated).
     - `+0x0C8..0x0D7`: `8 × uint16_be` Rhythm Song pointers (`S1–S8`, bit 15 = populated).
     - `+0x0D8..0x0D9`: `uint16_be` `streamEnd` (total stream length after `+0x1E2`).
     - `+0x0DA..0x13D`: `100` pattern time-signature codes (`0..27`).
     - `+0x13E..0x1A1`: `100` pattern length codes (`(val & 0x0F) + 1` bars; observed `1, 2, 4` bars).
     - `+0x1A2..0x1E1`: `8 × 8` B Rhythm Song names (`"NewSong "`, etc.).
     - `+0x1E2..+(0x1E2 + streamEnd)`: Packed Pattern + Song byte stream.

4. **`.V01–.V99` (`ALL` — Complete Workstation Dump, `38,912`–`74,752` B):**
   - `0x0000–0x67FF`: Identical to `.I##` (Synth Setup + 100 Perfs + 100 Voices).
   - `0x6800–0x6BFF`: 1,024 B Rhythm Setup header (identical to `.R##` `0x0000–0x03FF`).
   - `0x6C00..(rhyEndAligned)`: Hardware `$3800` Rhythm Pattern/Song RAM.
   - `(rhyEndAligned)..(+0x400)`: 1,024 B Sequencer Setup header.
   - `(rhyEndAligned + 0x400)..EOF`: 8 consecutive 1KB-aligned `HardwareSeqV1` Song chunks (Songs 7 down to 0).

### 2.4 HD6303 Firmware-Verified Rhythm Pattern & Song Bytecode (`IC723 XF810 C0.BIN`)

#### Rhythm Pattern Event Stream (`IC723 @0x0F0AA`, 48 PPQN)
Audited across all `1,802` user patterns (max `177` events/pattern, `0` exceeding `256`) and all `100` factory preset patterns (`IC723 0x11F88..0x13105`):
- `0x80..0xFF`: Advance tick by `(b & 0x7F)` ticks (`48 PPQN`, so `4/4` bar = `192` ticks).
- `0x00..0x3E`: Trigger drum ID `b` (`0..60`) at default accent `5`. (`0x3F` = erased-slot marker -> skip).
- `0x40..0x7E` followed by `<param>`: Trigger drum ID `b & 0x3F` (`0..60`) with signed accent offset:
  ```cpp
  const int offset = ((param & 0x20u) != 0u)
      ? (static_cast<int>(param | 0x80u) - 256)
      : static_cast<int>(param & 0x07u);
  // Observed hardware param values: 0x7D (-3), 0x7E (-2), 0x7F (-1), 0x01 (+1), 0x02 (+2)
  // Resulting hardware accent = clamp(5 + offset, 0, 7) (mapped to 1..8 velocity level in Ym3602DrumEngine)
  ```
  (`0x7F` + param has drum `0x3F` -> erased marker, skip both bytes).

#### Rhythm Song Bytecode (`IC723 $A824` & `$82CA` length table = `[1, 1, 3, 3, 4, 3]`)
Verified on real hardware songs (`LONELY.V09` S1/S2, `CLUBSONG.V08` S1, `OLDDRUMS.R02` S1, `DRUMZ.R01`–`R03` S2, `AURISTIC.V05` S2, and ROM Demo `MoonLite` S3):
| Byte | Meaning | Length | Exact Hardware Format |
|---|---|---:|---|
| `0x00..0x63` | Internal Pattern `I00..I99` | 1 B | `<ptnIdx>` |
| `0x64..0xC7` | Factory Preset Pattern `P00..P99` | 1 B | `<0x64 + ptnIdx>` |
| `0xC8` | Repeat Begin (`\|\|:`) | 1 B | `C8` |
| `0xC9` | Repeat End (`:\|\| x N`) | **3 B** | `C9 <extraRepeats> C9` (total plays = `(extraRepeats & 0x7F) + 1`, per `$AA9C`) |
| `0xCA` | Volume Delta (`Vol +/-d`) | **3 B** | `CA <int8_t delta> CA` (per `$A9A2`) |
| `0xCB` | Tempo Ramp (`T +/-d / beats`) | **4 B** | `CB <int8_t delta> <uint8_t beats> CB` (per `$A9C3`) |
| `0xCC` | Search Mark (`Mark [A..G]`) | **3 B** | `CC <markByte> CC` (per `$A96C` / `$102E5`) |

---

## 3. Codebase Mismatch Matrix & Required Fixes

| # | File(s) | Defect Found | Required Fix |
|---|---|---|---|
| **1** | `VF50DiskManager.h/.cpp`, `PluginEditor.cpp` | `._DD` (MS-DOS recovery), `.V00`/`.I00`/etc., and `.S##` accepted; `PluginEditor::isInterestedInFileDrag` duplicates extension logic. | Enforce one shared strict classifier in `VF50DiskManager`: `.V01–.V99`/`.ALL`, `.I01–.I99`/`.SYN`/`.VOI`/`.SYX`, `.A01–.A99`/`.SEQ`, `.R01–.R99`/`.RHY`, `.C01–.C99`/`.CRD`, `.B01–.B99`/`.MDR`. Reject `._DD`, `00`, `.S##`, `.ZIP`, and all unrecognized extensions everywhere. |
| **2** | `VF50DiskManager.cpp:825-833` | `enumerateFat12Directory()` pushed `VF50FileKind::Unknown` files into `entries_`. | Skip entries where `classifyFileKind(baseName) == VF50FileKind::Unknown` so non-VF50 files never appear in Classic or Modern directories. |
| **3** | `VF50ClassicController.cpp:2483,2540`, `VF50MainCanvas.cpp:2437`, `VF50DiskManager.cpp:3281,3291` | KB sizes computed with `(bytes + 512) / 1024` or `bytes / 1024` instead of 32-bit ceiling `(bytes + 1023) / 1024`. | Add shared `VF50DiskManager::bytesToKbCeil(uint32_t)` using `(static_cast<uint64_t>(bytes) + 1023ULL) / 1024ULL` and use it everywhere. |
| **4** | `VF50DiskManager.cpp:1341-1428`, `VF50MemoryManager.h/.cpp`, `VF50ClassicController.cpp` | `applyHardwareRhythmRamBlock()` never decoded Rhythm Songs `1–8`; `VF50MemoryManager` did not store Rhythm Songs; `VF50ClassicController` kept a disconnected private array. | Add `std::array<RhythmSongData, 8>` + active Rhythm Pattern/Song selection to `VF50MemoryManager` (synced wait-free to audio & `VF50ClassicController`), decode all 8 Rhythm Songs in `applyHardwareRhythmRamBlock()` using the exact `[1,1,3,3,4,3]` bytecode table, and preserve them in save/serialize paths. |
| **5** | `VF50RomLoader.cpp:616-645` | `0xC9` advanced by 2 B instead of 3 B; `0xCB` advanced by 3 B instead of 4 B; single `repeatMark` instead of nested stack. | Fix `0xC9` (`pos += 3u`, total plays = `(cnt & 0x7F) + 1`), `0xCB` (`pos += 4u`), `0xCC` (`pos += 3u`), and nested repeat stack in `VF50RomLoader.cpp`. |
| **6** | `VF50DiskManager.cpp:1590-1594`, `VF50DemoSongPlayer.h`, `PluginProcessor.cpp:1338-1357` | `parseSingleVF50SeqV1Chunk()` ignored `buf[31]` (`rhythmSyncIsSong`) and `buf[32]` (`rhythmTargetIdx`); `startAccompanyingRhythmPatternForSong()` never played the linked Rhythm Song (`1–8`) or Preset (`P##`) / Internal (`I##`) pattern in lockstep with Sequencer Songs. | Store `rhythmSyncIsSong` & `rhythmTargetIndex` on `CustomSeqSongBuffer` (and decode Rhythm Setup `0x20`/`0x2A`/`0x2D`); start the exact linked Rhythm Song (`Ym3602DrumEngine::startRhythmSong`) or Pattern (`I##` or `P##` from `EPROM Images/`) in lockstep with Sequencer playback and tempo! |
| **7** | `VF50DiskManager.cpp:1310-1339` | `applyHardwareRhythmSetupHeader()` ignored global Rhythm Volume (`0x20`) and active Pattern/Song (`0x2A`/`0x2D`). | Apply `hdr1024[0x20]` (`0..99` rhythm master volume) and `hdr1024[0x2A]` (active `I00..I99` / `P00..P99` pattern selection when valid). |
| **8** | `VF50DiskManager.cpp:304-328` & `VF50ClassicController.cpp` | Classic Mode `DiskDirectory` showed Modern fallback extensions (e.g., `.SYX`) if mounted from a local folder; `classifyFileKind` did not distinguish Classic vs Modern mode allowlists. | Provide `classifyFileKind(fileName, bool classicHardwareOnly)` so Classic Mode strictly recognizes `.V01–.V99`, `.I01–.I99`, `.A01–.A99`, `.R01–.R99`, `.C01–.C99`, `.B01–.B99`, while Modern Mode additionally supports `.ALL`, `.SYN`/`.VOI`/`.SYX`, `.PFM`, `.SEQ`, `.RHY`, `.CRD`, `.MDR`, `.MID`, `.IMG`/`.DSK`/`.FLP`/`.IMA`. |
| **9** | `VF50DiskManager.cpp:1417`, `VF50RomLoader.cpp:349` | Accent decoding clamped `5 + offset` to `1..9` without documenting `0..7` hardware accent mapping and `0x3F` erased marker handling. | Decode signed offset identically to HD6303 `@0x0F0AA`, skip `0x3F` erased slots, clamp hardware accent `0..7` -> `1..8` velocity level for `Ym3602DrumEngine`. |
