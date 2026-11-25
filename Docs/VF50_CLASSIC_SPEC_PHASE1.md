# Victory VF50 Classic Mode Firmware Specification — Phase 1 (Gate 1)

**Source of Truth:** `Docs/VF50 Operating Reference Manual` (171 PDF pages; PDF page = Manual page + 14)  
**Scope:** Chapters 1 (pp. 6–20, plus p. 38), 5 (pp. 78–94, plus pp. 59, 71), and 6 Disk LCD Workflow (pp. 105–109).  
**Architectural Boundary (Dual-Mode Contract):**
- In **CLASSIC Mode**, ONLY the top host bar (`MODE: MODERN | CLASSIC`, `SCALE: 75%..200%`, and virtual 3.5" floppy `DISK IN` / `EJECT` slot) exists outside the 1989 Victory VF50 hardware simulation.
- ZERO Modern Mode overlays (`EXPORT .MID`, `DRAG 8 TRACKS TO DAW`, `ROM SETUP`, `PROGRAMMER` drawer, desktop popup `DIR`/`IMPRT` menus, or OS dialogs inside faceplate workflows) may appear or execute in Classic Mode.
- Sequencer transport is decoupled from UI mode switching and continues uninterrupted across `SEQ` <-> `PERFORMANCE` <-> `SINGLE` transitions, stopping ONLY on `[STOP]`, song end, or host DAW transport stop.

---

## 0. Hardware Front Panel, LCD Grid & Conventions (Ch. 1, pp. 6–8, 15–17)

### 0.1 40×2 Dot-Matrix LCD & Softkey `F1`–`F8` Column Alignment
- **Manual Citation:** p. 7 (PDF p. 21), p. 16 (PDF p. 30)
- **LCD Geometry:** 40 columns × 2 rows (`Line 1` = cols `01..40`, `Line 2` = cols `01..40`).
- **Softkey Alignment:** The 8 slim softkeys `F1`–`F8` sit directly beneath columns `01..40` in 5-character spans (`F1`: cols `1–5`, `F2`: `6–10`, `F3`: `11–15`, `F4`: `16–20`, `F5`: `21–25`, `F6`: `26–30`, `F7`: `31–35`, `F8`: `36–40`).
- **Cursor Symbols (p. 16):**
  - `▶` (solid right arrow): Currently selected / editable field or active menu cursor.
  - `>` (open chevron): Selectable item or sub-menu mapped to the softkey `F1`–`F8` directly below it (`>~ ~`).
  - **Mode Switch Lock (p. 16):** Mode switching is locked while `[COMPARE]` is active during voice or performance editing.
  - **Hold `[PERFORMANCE]` or `[SINGLE]` when already active (pp. 13, 16, PDF pp. 27, 30):** Displays `** Sending PC No ---` while held to transmit a MIDI Program Change.

```text
1234567890123456789012345678901234567890
[F1 ][F2 ][F3 ][F4 ][F5 ][F6 ][F7 ][F8 ]
```

### 0.2 Physical Control Groups & LED Inventory (pp. 6–8, PDF pp. 20–22)
- **Left Panel (pp. 6–7):** `PITCH` wheel (spring-return), `MODULATION` wheel, `CARD` slot (`MCD32`/`MCD64`), `VOLUME` fader, `DATA ENTRY/TEMPO` fader.
- **Below Display (p. 7):**
  - `F1`–`F8` Function Softkeys (bracketed `INST ON/OFF` routed to `[-]` key).
  - `SYSTEM` group: `[SEQ]` (Green LED), `[RHYTHM]` (Green LED), `[JOB]`.
  - `[-]` (`LETTER` / `ERASE` / `INST ON/OFF` modifier key).
  - `SEQUENCER` / `OPERATOR` row (5 tactile keys + 5 Green LEDs):
    - `[RECORD]` (`OP SELECT`, Green LED)
    - `[BWD <<]` (`OP 1`, Green LED)
    - `[STOP ■]` (`OP 2`, Green LED, labeled `TOP` between `BWD`/`STOP` for `[STOP]+[BWD]`)
    - `[PLAY ▶]` (`OP 3`, Green LED)
    - `[FWD >>]` (`OP 4`, Green LED)
  - `4×3 Numeric Keypad`: `[7]`..`[0]`, `[-1 / NO <]`, `[+1 / YES >]`. Note: `[-]`+`[7]` enables Bank Hold, `[-]`+`[8]` disables Bank Hold (p. 13).
- **Right of Display (p. 8):**
  - `PLAY` group: `[PERFORMANCE]` (Green LED), `[SINGLE]` (Green LED).
  - `MEMORY` group: `[INT]` (Green LED), `[CARD]` (Green LED), `[PRESET]` (Green LED).
  - `EDIT` 2×6 Matrix (`TR1`–`TR8` & `RHY` have dual-color Red/Green LEDs to their left; bottom-right 2 keys are `OTHERS`/`FUNCTION` and `EFFECT`/`EFFECT`):
    - Left Col: `TR1` (`ASSIGN MODE`/`QUICK EDIT`), `TR2` (`NOTES`/`ALGORITHM`), `TR3` (`VOICE NUMBER`/`LFO`), `TR4` (`RECV CH`/`SENSITIVITY`), `RHY` (`LIMIT / LOW`/`OSCILLATOR`), `LIMIT / HIGH`/`EG`.
    - Right Col: `TR5` (`INST DETUNE`/`PEG`), `TR6` (`NOTE SHIFT`/`OUT LEVEL`), `TR7` (`VOLUME`/`SCALING`), `TR8` (`OUTPUT ASSIGN`/`TRANSPOSE`), `OTHERS`/`FUNCTION`, `EFFECT`/`EFFECT`.
  - `UTILITY` group: `[CARD]`, `[MIDI]`, `[DISK]`, `[MEMORY PROTECT]`, `[SETUP]`, `[OTHERS]`.
  - Action Column: `[DEMO]` (Green LED), `[STORE / COPY]`, `[COMPARE]` (Green LED), `[EFFECT BYPASS]` (Red LED).

### 0.3 Allowed Hardware LED State Vocabulary
Every LED specification in this document uses strictly one of four canonical states:
- `Off`
- `SolidGreen`
- `BlinkGreen`
- `BlinkRed` (available on `TR1`–`TR8` & `EFFECT BYPASS` Red LED; `SolidRed` / `BlinkRed` on Effect Bypass).

---

## 1. Chapter 1 — Front Panel, Play Modes & LEDs (pp. 6–20, 38)

### 1.1 Power-On Greeting Screen
- **(a) Manual Page Citation:** p. 11 (PDF p. 25)
- **(b) Entry Button Sequence:** Power on (auto-transitions after startup delay to last Play screen).
- **(c) Exact 40-Character LCD Template:**
```lcd
**** HARDWARE Digital Synthesizer VF50 ****
>>>>      Nice to meet you !!       <<<<
```
- **(d) Softkeys `F1`–`F8`:**
  - `F1`–`F8`: Inactive during boot splash.
- **(e) LED States:**
  - All mode/bank LEDs initialize to previous saved play mode state upon completion.

---

### 1.2 `PLAY SINGLE` Mode Display & Controller Flags
- **(a) Manual Page Citation:** pp. 12–13, 18 (PDF pp. 26–27, 32)
- **(b) Entry Button Sequence:** Press `[SINGLE]`. Optionally press `[PRESET]`, `[INT]`, or `[CARD]` followed by two digits `00`–`99` (while waiting for digits, Line 2 shows `P??`, `I??`, or `C??`), or press `[-1]` / `[+1]` or move `DATA ENTRY`.
- **(c) Exact 40-Character LCD Template:**
```lcd
PLAY SINGLE      EFCT= off   :  0 Pb= 2 
IXX  XXXXXXXXXX  Fs=sus   [TrPoFcMwBcAt]
```
  - Example with loaded Preset Voice & active flags (p. 12 / p. 18):
```lcd
PLAY SINGLE      EFCT=RevHall: 77 Pb= 2 
I00  Strings 1   Fs=sus   [Tr  FcMw    ]
```
  - Bank-digit entry prompt (e.g. after pressing `[PRESET]` while in Single Play, p. 13):
```lcd
PLAY SINGLE      EFCT=RevHall: 77 Pb= 2 
P??  Strings 1   Fs=sus   [Tr  FcMw    ]
```
  - **Field Breakdown (p. 18):**
    - `EFCT=` (cols `18..30`): Effect type name (e.g., `off`, `RevHall`).
    - `:` (col `30`) + 2-digit depth (`32..33`): Effect Output Level when `>Stereo Mix` is ON, or Effect Balance when `>Stereo Mix` is OFF.
    - `Pb=` (`35..39`): Pitch Bend Range (`0..12`).
    - Voice # (`01..03`): `P00–P99` (Preset), `I00–I99` (Internal), `C00–C99` (Card; requires card in slot).
    - Voice Name (`06..15`): 10-character ASCII name.
    - `Fs=` (`18..23`): Footswitch function (e.g., `sus`).
    - `[TrPoFcMwBcAt]` (`27..40`): Fixed-slot status flags (shown when non-default/assigned, blank spaces otherwise):
      - `Tr`: Transpose set to value other than `C3`.
      - `Po`: Portamento time `> 0`.
      - `Fc`: Foot Controller enabled/routed (p. 18 diagram).
      - `Mw`: Modulation Wheel controls pitch/tone/volume.
      - `Bc`: Breath Controller controls pitch/tone/volume.
      - `At`: Aftertouch controls pitch/tone/volume.
- **(d) Softkeys `F1`–`F8`:**
  - `F1` (`01–05`): Focus Voice Bank/Number (`IXX`/`PXX`/`CXX`) for numeric/data entry.
  - `F2`–`F3` (`06–15`): Voice Name region (no direct edit in Play mode).
  - `F4`–`F5` (`16–25`): `Fs=` Footswitch / `EFCT=` display alignment.
  - `F6`–`F8` (`26–40`): Controller flag readout & `Pb=` region.
- **(e) LED States:**
  - `SINGLE`: `SolidGreen`
  - `PERFORMANCE`: `Off`
  - `SEQ` / `RHYTHM`: `Off` (unless background sequencer/rhythm is running — see §2.3)
  - `INT` / `CARD` / `PRESET`: `SolidGreen` for active voice bank; others `Off`
  - `TR1`: `BlinkGreen` (~0.2 seconds) briefly whenever keyboard notes are played (p. 18)
  - `TR2`–`TR8`, `RHY`: `Off` (unless sequencer/rhythm active)
  - `EFFECT BYPASS`: `BlinkRed` / lit red when bypassed, `Off` when active.

---

### 1.3 `PF` Performance Play Mode Display
- **(a) Manual Page Citation:** pp. 11–12, 19–20 (PDF pp. 25–26, 33–34)
- **(b) Entry Button Sequence:** Press `[PERFORMANCE]`. Select `[PRESET]`, `[INT]`, or `[CARD]` and enter `00`–`99` (shows `PF??` while awaiting digits, p. 12) or press `[-1]` / `[+1]`.
- **(c) Exact 40-Character LCD Template:**
```lcd
PFIXX XXXXXXXXXX EFCT= off   :  0 Tch= 1
  I33/ I34/  */   */   */   */   */   * 
```
  - Example from p. 12 (`P13 12stGuitar`):
```lcd
PFP13 12stGuitar EFCT=E.Ref  : 80 Tch= 1
 P50/ P51/ P51/ P51/  * /  * /  * /  *  
```
  - Bank-digit prompt (`[PRESET]` pressed while in Performance Play, p. 12):
```lcd
PFP?? "V"Lead 1  EFCT=Dly L/R: 40 Tch= 1
 P09/ P09/P20/ P20 /P20/ P21 /P21/ P21  
```
  - **Field Breakdown (p. 20):**
    - `PF` (`01..02`) + Bank/Num (`03..05`, e.g., `I00`, `P13`, `C00`).
    - Performance Name (`07..16`): 10-character ASCII name.
    - `EFCT=` (`18..33`): Effect name + `:` + depth (`0..99`).
    - `Tch=` (`35..40`): Keyboard MIDI Transmit Channel (`1..16`).
    - Line 2 (`01..40`): Voice numbers assigned to Instruments 1–8 (`Ixx`, `Pxx`, `Cxx`, or `*` if unused), separated by `/`, aligned 1:1 above softkeys `F1`–`F8`.
- **(d) Softkeys `F1`–`F8`:**
  - `F1`..`F8`: Mapped 1:1 to Performance Instruments 1–8 (`INST 1`..`INST 8`). Pressing `[-]` + `F1..F8` mutes/unmutes that instrument (see §1.4). (In sequencer/synth workflow, `[-]` + `[TR1]`–`[TR8]` sets keyboard transmit channel to that instrument's receive channel, p. 79.)
- **(e) LED States:**
  - `PERFORMANCE`: `SolidGreen`
  - `SINGLE`: `Off`
  - `INT` / `CARD` / `PRESET`: `SolidGreen` for active performance bank; others `Off`
  - `TR1`–`TR8`: `BlinkGreen` (~0.5 seconds) when the corresponding Instrument 1–8 sounds via keyboard play (p. 20). Unused instruments remain `Off`.

---

### 1.4 Performance Instrument Mute (`[-]` + Softkey `F1`–`F8`)
- **(a) Manual Page Citation:** p. 20 (PDF p. 34), p. 7 (PDF p. 21 — `INST ON/OFF` bracket)
- **(b) Entry Button Sequence:** In Performance Play mode, hold `[-]` (`INST ON/OFF`) and press softkey `F1`–`F8` beneath the instrument to mute/unmute.
- **(c) Exact 40-Character LCD Template:**
```lcd
PFI00 StringsEns EFCT=RevHall: 40 Tch= 1
  xxx/ I34/  */   */   */   */   */   * 
```
  - Muted instrument slot changes from its voice number (e.g., `I33`) to `xxx` and produces no sound.
  - Repeating `[-]` + `F1..F8` restores the assigned voice number. Selecting another performance also resets all instruments back ON (temporary state, p. 20 Note).
- **(d) Softkeys `F1`–`F8`:**
  - `[-]` + `F1`..`F8`: Toggles Instrument `1..8` ON (`Ixx`/`Pxx`/`Cxx`) <-> OFF (`xxx`).
- **(e) LED States:**
  - When playing the keyboard while Instrument `k` (`1..8`) is muted (`xxx`), the LED to the left of `TRk` (`TR1`–`TR8`) enters `BlinkRed` (p. 20).
  - Unmuted sounding instruments continue `BlinkGreen` (~0.5 s).
  - `PERFORMANCE`: `SolidGreen`.

---

### 1.5 Voice Edit Transport Keys Doubling as `OP SELECT` & `OPERATOR ON/OFF` 1–4
- **(a) Manual Page Citation:** p. 7 (PDF p. 21), p. 16 (PDF p. 30), p. 38 (PDF p. 52)
- **(b) Entry Button Sequence:** From `PLAY SINGLE`, press any Single Voice Edit button (e.g., `[SENSITIVITY]`, `[OSCILLATOR]`, `[EG]`, `[SCALING]`, `[OUT LEVEL]`, `[ALGORITHM]`).
- **(c) Exact 40-Character LCD Template:**
```lcd
e1111                                   
                                        
```
  - Full Voice Edit header example (p. 16, `SENSITIVITY`):
```lcd
E1111 SENS)▶PMS(all) >AMS >AME >EBS >KVS
ALG 6 OP1    6         2   on    0   +2 
```
  - **Field Breakdown (p. 38):**
    - Upper-left `e1111` / `E1111` (`01..05`): Prefix `e`/`E` followed by 4 operator status digits for **OP 1, OP 2, OP 3, OP 4** (left to right: `1` = ON, `0` = OFF). E.g., `e1010` means OP1 & OP3 ON, OP2 & OP4 muted.
    - Note (p. 38): Operator ON/OFF is temporary for auditioning; `[STORE]` always saves a voice with all 4 operators ON (`1111`).
- **(d) Softkeys `F1`–`F8` + Transport Dual-Role Mapping (pp. 7, 16, 38):**
  - Softkeys `F1`–`F8`: Select edit fields marked with `>` on Line 1 or Line 2 (e.g., `F8` moves cursor `▶` to `KVS`).
  - `[RECORD]` (`OP SELECT`, p. 7): Cycles selected operator `OP1 -> OP2 -> OP3 -> OP4 -> OP1`.
  - `[BWD <<]`: Toggles **Operator 1** ON (`1`) / OFF (`0`).
  - `[STOP ■]`: Toggles **Operator 2** ON (`1`) / OFF (`0`).
  - `[PLAY ▶]`: Toggles **Operator 3** ON (`1`) / OFF (`0`).
  - `[FWD >>]`: Toggles **Operator 4** ON (`1`) / OFF (`0`).
- **(e) LED States:**
  - `OP SELECT` LED (above `[RECORD]`): `SolidGreen` when in operator-level Voice Edit page.
  - `OPERATOR ON/OFF` LEDs `1..4` (above `[BWD]`, `[STOP]`, `[PLAY]`, `[FWD]`): `SolidGreen` when corresponding Operator `1..4` is `1` (ON); `Off` when `0` (OFF).
  - `SINGLE`: `SolidGreen`.

---

## 2. Chapter 5 — Sequencer (pp. 78–94, plus pp. 59, 71)

### 2.1 `PLAY SONG)` Main Sequencer Screen
- **(a) Manual Page Citation:** pp. 82, 87 (PDF pp. 96, 101)
- **(b) Entry Button Sequence:** From `PERFORMANCE`, `SINGLE`, or `RHYTHM`, press `[SEQ]`.
- **(c) Exact 40-Character LCD Template:**
```lcd
PLAY SONG)          Time    >Meas>Tempo 
▶1:Sunset           4/4      001   120  
```
  - Empty/default song example from p. 82:
```lcd
PLAY SONG)          Time    >Meas>Tempo 
▶1: NewSong         4/4      001   120  
```
  - **Field Breakdown (pp. 82, 87):**
    - Line 2 cols `01..16`: Cursor `▶` + Song number (`1–8`) + `:` + 8-char Song Name.
    - `Time` (`21..24`): Time signature (`1/4–4/4`, `1/8–8/8`, `1/16–16/16`).
    - `>Meas` (`29..33`): Current / starting measure (`001..999`).
    - `>Tempo` (`35..39`): Tempo in BPM (`30..240`).
- **(d) Softkeys `F1`–`F8`:**
  - `F1`–`F3`: Cursor/focus on Song Number (`1–8`); change via numeric keys `[1]`–`[8]`, `[-1]`/`[+1]`, or `DATA ENTRY`.
  - `F4`–`F5`: Time signature display (`Time`).
  - `F6`: Move cursor `▶` to `>Meas` (directly enter starting measure).
  - `F7`–`F8`: Move cursor `▶` to `>Tempo` (`30–240` BPM; holding `[SEQ]` + moving `DATA ENTRY/TEMPO` slider also adjusts tempo during playback, p. 87).
- **(e) LED States:**
  - `SEQ`: `SolidGreen`
  - `TR1`–`TR8`: `SolidGreen` for every track `1..8` containing recorded data (unmuted); `Off` for empty tracks; `BlinkGreen` for muted tracks (p. 87).
  - `RHY`: `SolidGreen` when active/unmuted; `BlinkGreen` when rhythm machine is muted (p. 87).

---

### 2.2 `TR1`–`TR8` & `RHY` Track Mute Behavior
- **(a) Manual Page Citation:** p. 87 (PDF p. 101)
- **(b) Entry Button Sequence:** In Sequencer mode (while stopped, during playback, or before recording), press `[TR1]`–`[TR8]` or `[RHY]` in the 2×6 `EDIT` matrix.
- **(c) Exact 40-Character LCD Template:**
  - The main `PLAY SONG)` display remains on screen:
```lcd
PLAY SONG)          Time    >Meas>Tempo 
▶1:Sunset           4/4      001   120  
```
  - Track mute status is communicated directly via the `TR1`–`TR8` and `RHY` hardware LEDs (p. 87).
- **(d) Softkeys `F1`–`F8` & Track Keys:**
  - `[TR1]`–`[TR8]`: Toggles Track `1..8` mute state (`SolidGreen <-> BlinkGreen`).
  - `[RHY]`: Toggles Rhythm Machine mute state (`SolidGreen <-> BlinkGreen`).
  - **Song-Change Reset Rule (p. 87 Note):** Selecting a different song number (`1–8`) automatically defeats/clears all active track mutes.
  - If all tracks are muted, there is no sound output.
- **(e) LED States:**
  - Active track with data (unmuted): `SolidGreen`.
  - Muted track (silent during playback): `BlinkGreen` (p. 87).
  - Empty track (no data): `Off` (or `BlinkGreen` if muted before recording).
  - `RHY` muted: `BlinkGreen`; unmuted: `SolidGreen`.

---

### 2.3 Sequencer Transport Controls & Cross-Mode Playback Persistence
- **(a) Manual Page Citation:** p. 7 (PDF p. 21), p. 59 (PDF p. 73), p. 71 (PDF p. 85), p. 87 (PDF p. 101)
- **(b) Entry Button Sequence:**
  - Start / Resume: Press `[PLAY ▶]`
  - Pause / Stop retaining current measure: Press `[STOP ■]`
  - Step Measure Backward / Forward: Press `[BWD <<]` or `[FWD >>]` (p. 87)
  - Rewind to Top (`001`): Press `[STOP ■]` + `[BWD <<]` together (`TOP` printed above, p. 7, p. 87)
  - Play from Beginning: Press `[STOP ■]` + `[BWD <<]` together, then `[PLAY ▶]` (p. 87)
  - Switch Mode While Playing: Press `[PERFORMANCE]` or `[SINGLE]` during playback (pp. 59, 71, 79).
- **(c) Exact 40-Character LCD Template:**
  - On pressing `[PERFORMANCE]` during sequencer playback, LCD transitions to `PF` Performance Play (§1.3) while sequencer playback continues in background.
  - On pressing `[SINGLE]` during sequencer playback, LCD transitions to `PLAY SINGLE` (§1.2) while sequencer playback continues in background.
  - Pressing `[STOP ■]` while in `PERFORMANCE` or `SINGLE` Play mode stops active playback; if in Synth Edit mode, return to Play mode (`[PERFORMANCE]`/`[SINGLE]` or `[SEQ]`) then press `[STOP ■]` (pp. 59, 71).
- **(d) Softkeys `F1`–`F8`:**
  - Follow the currently visible screen (`PLAY SONG)`, `PF`, or `PLAY SINGLE`).
- **(e) LED States:**
  - During `SEQ` background playback while viewing `PF` or `PLAY SINGLE`:
    - `SEQ`: `SolidGreen` (or active indicator alongside active Play mode LED `PERFORMANCE`/`SINGLE` `SolidGreen`).
    - `PLAY` transport LED / `TR1`–`TR8`: Reflect active sounding/muted sequencer tracks (`SolidGreen` active, `BlinkGreen` muted).

---

### 2.4 `with SETUP` vs. `Song only` Prompt & Storing Setup Data
- **(a) Manual Page Citation:** pp. 82, 87, 94 (PDF pp. 96, 101, 108)
- **(b) Entry Button Sequence:**
  - **Select Song with Setup:** On `PLAY SONG)`, select song `1–8` that has saved setup data -> LCD prompts `with SETUP` vs `Song only` (pp. 82, 87; p. 84 calls it `withPFM`).
  - **Store Setup Data:** In `SEQ` mode, press `[STORE/COPY]` -> press `[+1/YES]` (p. 94).
- **(c) Exact 40-Character LCD Template:**
  - **Storing Setup Data Confirmation (p. 94):**
```lcd
STORE) ▶Mode           Are you sure ?   
Store SETUP data to song 1 ?            
```
  - **Song Selection Setup Prompt Behavior (pp. 82, 84, 87):**
    - `with SETUP` (Line 2 softkey choice): Loads stored Performance #, Voice #, and Rhythm Song # along with the sequencer song.
    - `Song only` (Line 2 softkey choice): Loads only the sequencer song notes/tempo without overwriting current Performance or Rhythm Song.
    - *(Note: Tempo data is stored inside the sequencer song itself, independent of Setup data, p. 94.)*
- **(d) Softkeys `F1`–`F8`:**
  - During `with SETUP` / `Song only` prompt: Softkeys below `with SETUP` and `Song only` choose whether to recall associated Performance/Voice/Rhythm Song #.
  - During `STORE)` prompt: `[+1/YES]` executes store of Performance/Voice/Rhythm Song # to active Song `1–8`; `[-1/NO]` cancels.
- **(e) LED States:**
  - `SEQ`: `SolidGreen`.

---

### 2.5 `SEQUENCER JOB SELECT)` Root Menu
- **(a) Manual Page Citation:** pp. 82, 88, 90, 91, 94 (PDF pp. 96, 102, 104, 105, 108)
- **(b) Entry Button Sequence:** From `PLAY SONG)` (`[SEQ]`), press `[JOB]`.
- **(c) Exact 40-Character LCD Template:**
```lcd
SEQUENCER JOB SELECT)    Select one!    
>Setup>Tch>Song>Edit>Rec           >EXIT
```
- **(d) Softkeys `F1`–`F8`:**
  - `F1` (`01–06` `>Setup`): Opens Sequencer Setup screen (§2.6, pp. 88–89).
  - `F2` (`07–10` `>Tch`): Opens Transmit Channel assignment (§2.7, p. 90).
  - `F3` (`11–15` `>Song`): Opens Song Name / Song Clear (§2.8, p. 90).
  - `F4` (`16–20` `>Edit`): Opens Edit Track menu (§2.9, pp. 91–93).
  - `F5` (`21–24` `>Rec`): Opens Record Mode selection (§2.10, pp. 82, 88, 94).
  - `F8` (`36–40` `>EXIT`): Returns to `PLAY SONG)`.
- **(e) LED States:**
  - `SEQ`: `SolidGreen`; `TR1`–`TR8`: retain active/muted/empty states.

---

### 2.6 Sequencer Job: `>Setup` Screen
- **(a) Manual Page Citation:** pp. 88–89 (PDF pp. 102–103)
- **(b) Entry Button Sequence:** `[SEQ]` -> `[JOB]` -> `F1` (`>Setup`).
- **(c) Exact 40-Character LCD Template:**
```lcd
SETUP)>Rch>A.T >Vel >Click    >Sync     
       Kbd off   on   Rec      int >EXIT
```
- **(d) Softkeys `F1`–`F8`:**
  - `F2` (`>Rch`): Sequencer Receive Channel (`Kbd`, `1–16`, `omn`).
  - `F3` (`>A.T`): Aftertouch record enable (`off`, `on`).
  - `F4` (`>Vel`): Key Velocity record enable (`off`, `on`).
  - `F5` (`>Click`): Metronome click (`off`, `Rec`, `Play/Rec`, `Always`).
  - `F7` (`>Sync`): Timing clock (`int`, `MIDI` — also applies to Rhythm Machine, p. 89).
  - `F8` (`>EXIT`): Returns to `SEQUENCER JOB SELECT)`.
- **(e) LED States:**
  - `SEQ`: `SolidGreen`.

---

### 2.7 Sequencer Job: `>Tch` (Transmit Channels)
- **(a) Manual Page Citation:** p. 90 (PDF p. 104)
- **(b) Entry Button Sequence:** `[SEQ]` -> `[JOB]` -> `F2` (`>Tch`).
- **(c) Exact 40-Character LCD Templates:**
  - **Single Mode active:**
```lcd
TRANSMIT CHANNEL)  Single Mode  Rch =1  
   1/  2/  1/  1/  3/  4/ off/ off      
```
  - **Performance (Multi) Mode active:**
```lcd
R   1/  2/  3/  4/  5/  6/  7/  8       
T   1/  2/  3/  4/  5/  6/  7/  8       
```
  - `R` row = receive channel of Performance Instruments `1..8`; `T` row = transmit channel (`off`, `1–16`) of Sequencer Tracks `1..8`.
- **(d) Softkeys `F1`–`F8`:**
  - `F1`–`F8`: Focus Track `1..8` transmit channel (`off`, `1–16`) in Line 2. Exit via `[JOB]` or `[SEQ]`.
- **(e) LED States:**
  - `SEQ`: `SolidGreen`.

---

### 2.8 Sequencer Job: `>Song` (Song Name & Song Clear)
- **(a) Manual Page Citation:** p. 90 (PDF p. 104)
- **(b) Entry Button Sequence:** `[SEQ]` -> `[JOB]` -> `F3` (`>Song`).
- **(c) Exact 40-Character LCD Template:**
```lcd
SONG) NAME          Song Clear          
1: NewSong   ←  →      >GO         >EXIT
```
- **(d) Softkeys `F1`–`F8`:**
  - `F1`–`F2`: Select character position / Song Name (`max 8 chars`).
  - `F3` (`←`): Move name cursor left (p. 17).
  - `F4` (`→`): Move name cursor right (p. 17).
  - `F5`–`F6` (`>GO`): Clear song data -> prompts confirmation -> `[+1/YES]` erases all tracks, name, meter, tempo.
  - `F8` (`>EXIT`): Returns to `SEQUENCER JOB SELECT)`.
- **(e) LED States:**
  - `SEQ`: `SolidGreen`.

---

### 2.9 Sequencer Job: `>Edit` (`EDIT TRACK)` & 7 Edit Sub-Screens)
- **(a) Manual Page Citation:** pp. 91–93 (PDF pp. 105–107)
- **(b) Entry Button Sequence:** `[SEQ]` -> `[JOB]` -> `F4` (`>Edit`).
- **(c) Exact 40-Character LCD Templates:**
  - **`EDIT TRACK)` Sub-Menu (p. 91):**
```lcd
EDIT TRACK)               Select one!   
>Mix >Qntz>Del >Ins >Copy>Eras>Remv>EXIT
```
  - **`>Mix` (`TRACK MIX DOWN)`, p. 91):**
```lcd
TRACK MIX DOWN)                         
 Tr ▶1 and Tr >1   to Tr >1    >GO >EXIT
```
  - **`>Qntz` (`QUANTIZE)`, p. 91):**
```lcd
QUANTIZE)        ▶Tr  >Size             
                   1   1/16    >GO >EXIT
```
    - Settings: Track `1–8`; Size `1/48, 1/32, 1/16, 1/12, 1/8, 1/6, 1/4`.
  - **`>Del` (`DELETE)` Measure All Tracks, p. 92):**
```lcd
DELETE) Delete Measure (All tracks)     
 from ▶001  to >001            >GO >EXIT
```
  - **`>Ins` (`INSERT)` Measure All Tracks, p. 92):**
```lcd
INSERT)Insert Measure (All tracks)      
 from ▶001  to >001 Ins >001   >GO >EXIT
```
  - **`>Copy` (`COPY)` Measure Between Tracks, p. 92):**
```lcd
COPY) Copy Measure                      
 Tr ▶1 >001→>001  to Tr >1 >001 >GO>EXIT
```
  - **`>Eras` (`ERASE)` Measure, p. 93):**
```lcd
ERASE) Erase Measure                    
  Tr  ▶1  from >001  to  >001  >GO >EXIT
```
    - Track can be `1–8` or `all` (replaces erased notes with rests).
  - **`>Remv` (`REMOVE EVENT)`, p. 93):**
```lcd
REMOVE EVENT)                           
▶Cnt >  0→>  0 Tr>1 >001→>001  >GO >EXIT
```
    - Event types: `Cnt` (CC `0–127`), `P.B.` (Pitch Bend), `A.T.` (Aftertouch).
- **(d) Softkeys `F1`–`F8`:**
  - On `EDIT TRACK)`: `F1` `>Mix`, `F2` `>Qntz`, `F3` `>Del`, `F4` `>Ins`, `F5` `>Copy`, `F6` `>Eras`, `F7` `>Remv`, `F8` `>EXIT`.
  - Inside each Edit screen: `F1`–`F6` move cursor `▶` among parameter fields; `F7` `>GO` triggers confirmation -> `[+1/YES]` executes; `F8` `>EXIT` returns to `EDIT TRACK)`.
- **(e) LED States:**
  - `SEQ`: `SolidGreen`.

---

### 2.10 Sequencer Job: `>Rec` (`RECORD MODE)`), Realtime `RECORD)`, `STEP REC)` & `PUNCH REC)`
- **(a) Manual Page Citation:** pp. 82–86, 88, 94 (PDF pp. 96–100, 102, 108)
- **(b) Entry Button Sequence:**
  - Record Mode select: `[SEQ]` -> `[JOB]` -> `F5` (`>Rec`).
  - Enter Armed Recording: From `PLAY SONG)`, press `[RECORD]` (`OP SELECT` row button 0).
- **(c) Exact 40-Character LCD Templates:**
  - **`RECORD MODE)` (pp. 82, 88, 94):**
```lcd
RECORD MODE) ▶Mode   >Condition         
free = 100% Realtime   Replace     >EXIT
```
    - Modes: `Realtime`, `Step`, `Punch`. Conditions: `Replace`, `Overdub` (`Step` and `Punch` ignore condition and use Overdub / Replace respectively).
  - **Realtime `RECORD)` Standby (p. 83):**
```lcd
RECORD)  >Mode>Voice>Time    >Meas▶Tempo
Replace   IND  I00   4/4      001   120 
```
    - Pressing `[PLAY ▶]` starts a 2-measure countdown (`- 8, - 7, ... 0`) then records.
  - **Step `STEP REC)` Standby & Bar Graph (pp. 85–86):**
```lcd
STEP REC)▶Mode>Voice>Time    >Meas>Tempo
Overdub   IND  I00   4/4      001   120 
```
    - Pressing `[PLAY ▶]` opens 32nd-note bar-graph entry (`+` = beat, `-` = 32nd, `◆` = note, `p` = pgm change):
```lcd
M001 ♪ |-------+-------+-------+-------|
             ▶GateTime    >PgmChange    
```
    - Pressing `F4`–`F5` (`>GateTime`) opens gate select:
```lcd
M001 ♩ |■■■■■■■--------+-------+-------|
             ▶Norm>Stac>Slur       >EXIT
```
      (`Norm` = 80%, `Stac` = 50%, `Slur` = 99%).
  - **Punch-In `PUNCH REC)` Standby (p. 88):**
```lcd
PUNCH REC)▶Mode>Voice   >Meas >In  >Out 
           IND  P22      001  001  001  
```
    - Constraint: `Meas <= In <= Out`.
- **(d) Softkeys `F1`–`F8`:**
  - On `RECORD MODE)`: `F3` `▶Mode`, `F5` `>Condition`, `F8` `>EXIT` (twice returns to `PLAY SONG)`).
  - On `RECORD)`/`STEP REC)`/`PUNCH REC)`: Softkeys align with `>Mode` (`SGL`/`PFM`/`IND`), `>Voice`, `>Time`, `>Meas`/`>In`/`>Out`, `>Tempo`.
  - `[TR1]`–`[TR8]`: Selects single recording track (`1..8`).
  - `[-]` + `[TR1]`–`[TR8]` or `[RHY]`: Sets keyboard transmit channel to that instrument's receive channel (p. 83).
- **(e) LED States:**
  - `SEQ`: `SolidGreen`
  - `RECORD` LED: Lit/armed during recording standby & recording
  - `TR1`–`TR8`: Selected track to record indicates active selection; tracks containing data light `SolidGreen` (p. 83).

---

## 3. Chapter 6 — Disk LCD Workflow (pp. 105–109)

> **CRITICAL FIDELITY CONSTRAINT:** In Classic Mode, pressing `[DISK]` MUST remain **100% inside the 40×2 LCD softkey tree** shown below. ZERO OS popup menus (`juce::PopupMenu`), folder dialogs, or native file pickers may spawn from `[DISK]` or `F1`–`F8`. Only the top host utility bar `DISK IN` / `EJECT` slot mounts/unmounts `.IMG`/`.DSK` images into the virtual drive.

### 3.1 `UT DISK)` Top-Level Menu
- **(a) Manual Page Citation:** pp. 105–109 (PDF pp. 119–123)
- **(b) Entry Button Sequence:** Press `[DISK]` (after inserting virtual floppy in top host slot).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT DISK)       Select one !             
>Save>Load>Del >Rename  >MDR >Dir  >Job 
```
- **(d) Softkeys `F1`–`F8`:**
  - `F1` (`>Save`, cols `1–5`): Open Save data-type screen (§3.2).
  - `F2` (`>Load`, cols `6–10`): Open Load data-type screen (§3.3).
  - `F3` (`>Del`, cols `11–14`): Open Delete data-type screen (§3.4).
  - `F4`–`F5` (`>Rename`, cols `16–22`): Open Rename data-type screen (§3.5).
  - `F6` (`>MDR`, cols `25–28`): Open MIDI Data Recorder screen (§3.6).
  - `F7` (`>Dir`, cols `30–33`): Open Directory viewer (§3.7).
  - `F8` (`>Job`, cols `36–39`): Open Disk Job menu (`Format`, `BackUp`, `Status`, §3.8).
- **(e) LED States:**
  - Previous play mode LED (`SINGLE`/`PERFORMANCE`) remains as prior or utility active; virtual floppy slot LED in top host strip shows `SolidGreen` when disk mounted.

---

### 3.2 Disk `>Save` Workflow (`ALL`, `SYN`, `SEQ`, `R.SEQ`, `CARD`)
- **(a) Manual Page Citation:** p. 105 (PDF p. 119)
- **(b) Entry Button Sequence:** `[DISK]` -> `F1` (`>Save`) -> select data type (`F1..F5`) -> edit file # / name -> `F8` (`>GO`) -> `[+1/YES]`.
- **(c) Exact 40-Character LCD Templates:**
  - **Step 1: Type Select Screen (p. 105):**
```lcd
UT DISK) SAVE  Select one !             
 ALL  SYN  SEQ  R.SEQ  CARD        >EXIT
```
  - **Step 2: File Number & Name Screen (`ALL` example, p. 105):**
```lcd
UT DISK) SAVE ALL   Set name & push GO  
>File 01 ▶NewFile (ALL)   ←   →     >GO 
```
  - **Confirmation & Guards (p. 105):**
    - Pressing `F8` (`>GO`) displays `"Are you sure?"`; pressing `[+1/YES]` executes write and displays remaining free kilobytes.
    - If disk is near capacity: warning prompt allows `>EXIT`.
    - **Unformatted / Write-Protected Checks (p. 105 Note):** Unformatted disk cannot save; write-protect slider ON prevents save/format/delete/rename.
- **(d) Softkeys `F1`–`F8`:**
  - **Type Select Screen:** `F1`=`ALL` (Synth+Seq+Rhythm), `F2`=`SYN` (Internal Voices+Perfs+Setup), `F3`=`SEQ` (single active Sequencer Song), `F4`=`R.SEQ` (entire Rhythm Song/Patterns), `F5`=`CARD` (specified Card bank to disk), `F8`=`>EXIT` (back to `UT DISK)`).
  - **Set Name Screen:** `F1`=`>File` (select slot/overwrite existing file), `F2`–`F4`=filename edit (`p. 17`), `F6`=`←` (cursor left), `F7`=`→` (cursor right), `F8`=`>GO` (execute confirmation).
- **(e) LED States:**
  - Top floppy activity indicator active during save; hardware panel LEDs unchanged.

---

### 3.3 Disk `>Load` Workflow & `Memory Protected` Guard
- **(a) Manual Page Citation:** pp. 105–106 (PDF pp. 119–120)
- **(b) Entry Button Sequence:** `[DISK]` -> `F2` (`>Load`) -> select data type (`F1..F5`) -> select file via `[-1]`/`[+1]`/`DATA ENTRY`/numeric keys -> `F7` (`>GO`) -> `[+1/YES]`.
- **(c) Exact 40-Character LCD Templates:**
  - **Step 1: Type Select Screen (p. 106):**
```lcd
UT DISK) LOAD  Select one !             
 ALL  SYN  SEQ  R.SEQ  CARD        >EXIT
```
  - **Step 2: File Selection Screen (`ALL` example, p. 106):**
```lcd
UT DISK) LOAD ALL  Sel file & push GO   
▶File 01 :SunShine(ALL)  20K  >GO  >EXIT
```
  - **Step 3: Confirmation & Memory Protect Rule (pp. 105–106):**
    - **Memory Protect Check (p. 105 Note):** When Internal Memory Protect (`UT MEMORY PROTECT)` -> `INT: on`) is ON, attempting to load displays `"Memory Protected"` and blocks loading (`ALL`, `SYN`, `R.SEQ` are blocked).
    - **Exception for `SEQ` (p. 105 Note):** Loading `SEQ` (sequencer song) is **exempt** from Internal Memory Protect and succeeds even when `INT` Memory Protect is ON.
    - Type-match rule (p. 106): Only files saved with the matching type (`ALL`, `SYN`, `SEQ`, `R.SEQ`, `CARD`) can be selected under that filter (e.g., cannot load `SYN` out of an `ALL` file via `SYN` menu).
    - Pressing `F7` (`>GO`) shows `"Are you sure?"`; pressing `[+1/YES]` loads data into VF50 memory (overwriting prior internal data of that type).
- **(d) Softkeys `F1`–`F8`:**
  - **Type Select Screen:** `F1`=`ALL`, `F2`=`SYN`, `F3`=`SEQ`, `F4`=`R.SEQ`, `F5`=`CARD` (loads disk file into inserted RAM Card), `F8`=`>EXIT`.
  - **File Select Screen:** `F1`=`▶File` (select file #), `F7`=`>GO` (prompts `"Are you sure?"`), `F8`=`>EXIT` (back).
- **(e) LED States:**
  - Top floppy slot LED lit; after loading `ALL`/`SYN`, active voice/perf bank LEDs update to reflect loaded memory state.

---

### 3.4 Disk `>Del` (Delete File)
- **(a) Manual Page Citation:** p. 106 (PDF p. 120)
- **(b) Entry Button Sequence:** `[DISK]` -> `F3` (`>Del`) -> select type (`F1..F5`) -> select file -> `F7` (`>GO`) -> `[+1/YES]`.
- **(c) Exact 40-Character LCD Templates:**
  - **Step 1: Type Select Screen (p. 106):**
```lcd
UT DISK) DELETE   Select one !          
 ALL  SYN  SEQ  R.SEQ  CARD        >EXIT
```
  - **Step 2: File Selection Screen (p. 106):**
```lcd
UT DISK) DELETE ALL Sel file & push GO  
▶File 01 :SunShine(ALL)  20K  >GO  >EXIT
```
  - **Guard:** Blocked if disk write-protect slider is ON (p. 106 Note). Pressing `>GO` asks `"Are you sure?"`; `[+1/YES]` deletes file.
- **(d) Softkeys `F1`–`F8`:**
  - Type Select: `F1`=`ALL`, `F2`=`SYN`, `F3`=`SEQ`, `F4`=`R.SEQ`, `F5`=`CARD`, `F8`=`>EXIT`.
  - File Select: `F1`=`▶File`, `F7`=`>GO`, `F8`=`>EXIT`.
- **(e) LED States:**
  - Unchanged.

---

### 3.5 Disk `>Rename`
- **(a) Manual Page Citation:** p. 107 (PDF p. 121)
- **(b) Entry Button Sequence:** `[DISK]` -> `F4`/`F5` (`>Rename`) -> select type (`F1..F5`) -> select file & edit name -> `F8` (`>GO`) -> `[+1/YES]`.
- **(c) Exact 40-Character LCD Templates:**
  - **Step 1: Type Select Screen (p. 107):**
```lcd
UT DISK) RENAME   Select one !          
 ALL  SYN  SEQ  R.SEQ  CARD        >EXIT
```
  - **Step 2: File Name Edit Screen (p. 107):**
```lcd
UT DISK) RENAME ALL Set name & push GO  
▶File 01 >SunShine(ALL)   ←   →     >GO 
```
  - **Guard:** Blocked if disk write-protect slider is ON (p. 107 Note). Pressing `>GO` asks `"Are you sure?"`; `[+1/YES]` commits rename.
- **(d) Softkeys `F1`–`F8`:**
  - Type Select: `F1`=`ALL`, `F2`=`SYN`, `F3`=`SEQ`, `F4`=`R.SEQ`, `F5`=`CARD`, `F8`=`>EXIT`.
  - Rename Edit: `F1`=`▶File`, `F2`–`F4`=edit filename (`p. 17`), `F6`=`←`, `F7`=`→`, `F8`=`>GO`.
- **(e) LED States:**
  - Unchanged.

---

### 3.6 Disk `>MDR` (MIDI Data Recorder) & `>IntTime`
- **(a) Manual Page Citation:** p. 107 (PDF p. 121)
- **(b) Entry Button Sequence:** `[DISK]` -> `F6` (`>MDR`) -> `F1..F5` / `F8`.
- **(c) Exact 40-Character LCD Templates:**
  - **`UT DISK) MDR` Menu (p. 107):**
```lcd
UT DISK) MDR      Select one !          
 >In  >Out >Del >Rename >IntTime   >EXIT
```
  - **`>IntTime` Interval Setting (p. 107):**
```lcd
UT DISK) MDR                            
 Interval Time = 1 x 100 ms        >EXIT
```
  - **Behavior Notes (p. 107):**
    - Uses internal 64 KB buffer (clears sequencer data).
    - `>In`: Set file name -> `>GO` -> `"Are you sure?"` -> `[+1/YES]` -> receives external MIDI bulk dump -> press `>GO` to save to disk.
    - `>Out`: Select MDR file -> `>GO` -> `[+1/YES]` -> transmits bulk dump with `Interval Time = N x 100 ms` delay per 1 KB block.
- **(d) Softkeys `F1`–`F8`:**
  - `MDR` Menu: `F1`=`>In`, `F2`=`>Out`, `F3`=`>Del`, `F4`=`>Rename`, `F5`–`F6`=`>IntTime`, `F8`=`>EXIT`.
  - `IntTime` Screen: `[-1]`/`[+1]`/`DATA ENTRY` adjusts interval multiplier; `F8`=`>EXIT`.
- **(e) LED States:**
  - Unchanged.

---

### 3.7 Disk `>Dir` (Directory Listing — Pure 40×2 LCD, ZERO OS Popups)
- **(a) Manual Page Citation:** p. 108 (PDF p. 122)
- **(b) Entry Button Sequence:** `[DISK]` -> `F7` (`>Dir`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT DISK) DIRECTORY     Total: 52 Files  
▶File 01 :SunShine(ALL)   20K      >EXIT
```
  - **Field Breakdown (p. 108):**
    - Line 1 `Total: NN Files`: Total enumerated files on disk.
    - Line 2 `▶File NN`: File index (`01..99`) scrolled via `[-1]` / `[+1]` / `DATA ENTRY` / numeric keypad.
    - `:SunShine(ALL)`: Filename + VF50 file type tag in parentheses (`ALL`, `SYN`, `SEQ`, `R.SEQ`, `MDR`, or `ETC` for non-VF50 files).
    - `20K`: File size in kilobytes.
- **(d) Softkeys `F1`–`F8`:**
  - `F1`–`F2`: File number cursor `▶File` (scroll entries directly on the 40×2 LCD).
  - `F8`: `>EXIT` (returns to `UT DISK)`).
- **(e) LED States:**
  - Unchanged.

---

### 3.8 Disk `>Job` (`>Format`, `>BackUp`, `>Status`)
- **(a) Manual Page Citation:** pp. 108–109 (PDF pp. 122–123)
- **(b) Entry Button Sequence:** `[DISK]` -> `F8` (`>Job`) -> `F1` (`>Format`), `F3` (`>BackUp`), or `F5` (`>Status`).
- **(c) Exact 40-Character LCD Templates:**
  - **`UT DISK)` Job Select Screen (pp. 108–109):**
```lcd
UT DISK) Insert DISK and select one !   
>Format   >BackUp   >Status        >EXIT
```
  - **`>Format` (p. 108):**
    - Pressing `F1` (`>Format`) prompts `"Are you sure?"`; pressing `[+1/YES]` formats disk (`720 KB 2DD` on hardware). Blocked if disk write-protect is ON.
  - **`>BackUp` (pp. 108–109):**
    - Pressing `F3` (`>BackUp`) displays `"** SEQ/R.SEQ data will be cleared. Sure?"` -> `[+1/YES]` -> `"** Set original disk & push YES"` -> `[+1/YES]` -> `"** Set duplicate disk & push YES"` -> repeats showing % copied on Line 1 -> `"Backup completed !"`.
  - **`>Status` (p. 109):**
```lcd
UT DISK) STATUS                         
Total=52files, Used= 50K,Free=663K >EXIT
```
- **(d) Softkeys `F1`–`F8`:**
  - Job Select: `F1`–`F2`=`>Format`, `F3`–`F4`=`>BackUp`, `F5`–`F6`=`>Status`, `F8`=`>EXIT`.
  - Status Screen: `F8`=`>EXIT` (returns to `UT DISK)` Job Select).
- **(e) LED States:**
  - Top virtual floppy slot LED active while disk mounted.

---

### 3.9 Cross-Reference: `UT MEMORY PROTECT)` Screen
- **(a) Manual Page Citation:** p. 109 (PDF p. 123)
- **(b) Entry Button Sequence:** Press `[MEMORY PROTECT]`.
- **(c) Exact 40-Character LCD Template:**
```lcd
UT MEMORY PROTECT) ▶INT      >CARD      
                    off        on       
```
  - **Power-On Hardware Default (p. 109):** Both `INT` and `CARD` default to `on` at power-on; must be set to `off` before storing voices/performances or loading `ALL`/`SYN`/`R.SEQ` from disk (p. 105). Card also has a physical write-protect switch.
- **(d) Softkeys `F1`–`F8`:**
  - `F4`–`F5` (`▶INT`): Select `INT` protect (`off` / `on` via `[-1]`/`[+1]`/`DATA ENTRY`).
  - `F6`–`F7` (`>CARD`): Select `CARD` protect (`off` / `on`).
- **(e) LED States:**
  - Unchanged.
