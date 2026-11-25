# Victory VF50 Classic Mode Firmware Specification — Phase 2 (Gate 3)

**Source of Truth:** `Docs/VF50 Operating Reference Manual` (171 PDF pages; PDF page = Manual page + 14)  
**Scope:** Chapters 2 (Performance Edit, pp. 23–35), 3 (Voice Edit, pp. 36–56), 4 (Rhythm Machine, pp. 57–77), and 6 Buffer Utilities (Memory Protect p. 109, Utility Others pp. 117–118, Store p. 119, Copy p. 120, Compare pp. 16 & 121).  
**Architectural Contract:**
- Extends `Docs/VF50_CLASSIC_SPEC_PHASE1.md` (Chapters 1, 5, 6 Disk) with full editing, rhythm machine, and edit-buffer fidelity.
- Every 40×2 LCD template below fits strictly within **40 characters per line**.
- Conventions carried forward from Phase 1: `▶` (active cursor), `>` (softkey-selectable item/submenu), `←`/`→` (cursor/character navigation), `‡` (multi-press page indicator at col 40 on Performance/Voice Edit buttons with multiple cycles).
- Hardware LED vocabulary: `Off`, `SolidGreen`, `BlinkGreen`, `BlinkRed` (plus `SolidRed` on `EFFECT BYPASS`).

---

## 1. Chapter 2 — Performance Edit (pp. 23–35)

### 1.0 Mode Entry, Instrument Mute, Edit Buffer & Store Rules
- **(a) Manual Page Citation:** pp. 23, 29–32, 117, 119, 121 (PDF pp. 37, 43–46, 131, 133, 135)
- **(b) Entry Button Sequence:**
  - From Performance Play (`PERFORMANCE`), press any of the 12 Edit keys (`TR1`–`TR8`, `RHY`, `LIMIT/HIGH`, `OTHERS`, `EFFECT`) to enter Performance Edit (`P.ED` prefix).
  - Alternatively, start a new performance from scratch via `[OTHERS]` -> `>Init` (p. 117).
- **(c) Rules & Buffer Lifecycle:**
  - **Temporary Instrument On/Off (`[-]` + Softkey, p. 23):** While editing a performance, hold `[-]` (`INST ON/OFF`) and press softkeys `F1`–`F8` to temporarily mute/unmute instruments `1–8` (exactly as in Performance Play, p. 20). It is **not** possible to store a performance with an instrument muted; executing `STORE` automatically turns all instruments back on.
  - **Unstored Edit Behavior (p. 23):** Selecting another performance before storing discards unstored buffer changes from the active performance (the previously edited performance remains in the edit recall buffer and can be recovered via `[OTHERS]` -> `>Recall`, p. 117).
  - **Disabled Instrument Slot Display (`*` vs `0`):** Instruments whose Max Notes = `0` (or DVA Reserved Notes = `off`) display `*` on channel/limit/detune/shift/volume/output/LFO/micro-tune/effect screens and cannot be edited there; in `VOICE NO>`, unused slots display `0`.
  - **Multi-Press Cycling (`‡`):** Pressing `OTHERS` or `EFFECT` repeatedly cycles through their sub-pages (marked by `‡` in col 40). Pressing `[STORE/COPY]` during `EFFECT` enters `EFFECT COPY>` (p. 120); pressing `[COMPARE]` toggles `P.ED` <-> `c.ED` (p. 121).
- **(e) LED States:**
  - `PERFORMANCE`: `SolidGreen` (locks to Compare indicator behavior when `[COMPARE]` active, p. 121).
  - `TR1`–`TR8`: `SolidGreen` for unmuted instruments; `BlinkRed` for instruments muted via `[-]` + `F1`–`F8` (per Ch. 1 p. 20 / Ch. 2 p. 23); active Edit parameter LED or `OFF` per matrix state.
  - `EFFECT BYPASS`: `SolidRed` when DSP bypass is engaged (`EFFECT` LCD upper line shows `BYPASS`, p. 30).

---

### 1.1 `TR1` (`ASSIGN MODE`) — Assign Mode & Performance Name
- **(a) Manual Page Citation:** p. 24 (PDF p. 38)
- **(b) Entry Button Sequence:** In Performance Play or Performance Edit, press `[TR1]` (`ASSIGN MODE`).
- **(c) Exact 40-Character LCD Template:**
```lcd
P.ED  ▶AssignMode    >name : E.ORGAN    
         normal              ←   →      
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F1`–`F4`: Selects `▶AssignMode` — settings: `normal`, `alternate`, `DVA`.
    - `normal`: Each instrument plays on its own `Recv Ch`, up to its `Max Notes` polyphony limit.
    - `alternate`: Instruments sharing Inst 1's `Recv Ch` alternate voice allocation per note-on (requires keyboard `Transmit Ch` == Inst 1 `Recv Ch`; controllers CC/PB/AT on Inst 1 channel apply to all alternate instruments).
    - `DVA`: Dynamic Voice Allocation allocates up to 16 voices dynamically across instruments as needed (recommended for sequencer playback).
    - **Side Effect on Mode Change:** Changing Assign Mode resets `Max Notes` / `Reserved Notes` to match active instruments.
  - `F5`–`F6`: Moves cursor to `>name :` (10 characters max, entered via p. 17 character entry rules).
  - `F7` (`←`) / `F8` (`→`): Moves character edit cursor left/right within the 10-character Performance Name.
- **(e) LED States:**
  - `PERFORMANCE`: `SolidGreen`. Muted instruments keep `BlinkRed` on `TR1`–`TR8`.

---

### 1.2 `TR2` (`NOTES`) — Max Notes (`normal`/`alternate`) vs. Reserved Notes (`DVA`)
- **(a) Manual Page Citation:** pp. 24–25 (PDF pp. 38–39)
- **(b) Entry Button Sequence:** Press `[TR2]` (`NOTES`). Screen layout depends on `AssignMode`.
- **(c) Exact 40-Character LCD Templates:**
  - When `AssignMode` = `normal` or `alternate` (p. 24):
```lcd
P.ED  MAX NOTES>  Rch= 1  I10:E.ORGAN   
▶  4/  12/   0/   0/   0/   0/   0/   0 
```
  - When `AssignMode` = `DVA` (p. 25):
```lcd
P.ED  RESERVED NOTES>     I10:E.ORGAN   
▶  4/  2/   4/   4/   2/ off/ off/ off  
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F1`–`F8`: Selects Instruments 1–8 (`▶` moves to selected column). Line 1 dynamically shows the selected instrument's voice (`I10:E.ORGAN`) and (in `normal`/`alternate`) `Rch= n`.
  - **Settings (`MAX NOTES>`):** `0–16` per instrument. Total allocated notes across all 8 instruments **<= 16** (cannot exceed 16). Assigning `0` turns an instrument off.
  - **Settings (`RESERVED NOTES>` in DVA):** `off`, `0–16` per instrument. Total reserved notes across all 8 instruments **<= 16**. `0` disables the instrument's reserve guarantee while remaining active in DVA pool; `off` turns the instrument off.
- **(e) LED States:**
  - `PERFORMANCE`: `SolidGreen`.

---

### 1.3 `TR3` (`VOICE NUMBER`) — Instrument Voice Number & Bank Restrictions
- **(a) Manual Page Citation:** p. 25 (PDF p. 39)
- **(b) Entry Button Sequence:** Press `[TR3]` (`VOICE NUMBER`). Use `[INT]`, `[CARD]`, `[PRESET]`, numeric keys, `[-1]`/`[+1]`, or `DATA ENTRY`.
- **(c) Exact 40-Character LCD Template:**
```lcd
P.ED  VOICE NO>  Rch= 1  I10:E.Piano 2A 
▶I10/ I12/   0/   0/   0/   0/   0/   0 
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F1`–`F8`: Selects Instruments 1–8 (`0` shown when Max Notes = `0` or Reserved = `off`; cannot set voice number on inactive slots).
  - **Settings:** `I00–I99` (Internal), `P00–P99` (Preset), `C00–C99` (Card).
  - **Bank Restriction Rules (p. 25):**
    - An **Internal** performance (`I00–I99`) can assign **Preset** (`P00–P99`) and **Internal** (`I00–I99`) voices ONLY (cannot select Card voices).
    - A **Card** performance (`C00–C99`) can assign **Preset** (`P00–P99`) and **Card** (`C00–C99`) voices ONLY (cannot select Internal voices).
- **(e) LED States:**
  - `[INT]`/`[PRESET]`/`[CARD]` bank LEDs reflect the selected voice bank (`SolidGreen`) while editing `TR3`.

---

### 1.4 `TR4`, `RHY`, `LIMIT/HIGH`, `TR5`–`TR8` — Per-Instrument Matrix Parameters
- **(a) Manual Page Citation:** pp. 25–27 (PDF pp. 39–41)
- **(b) Entry Button Sequence & Multi-Screen Behavior:** Single press per button (`TR4`..`TR8`, `RHY`, `LIMIT/HIGH`). Upper right of Line 1 shows selected instrument's voice (`I10:E.Piano 2A`) plus its companion context field.
- **(c) Exact 40-Character LCD Templates:**
  - **`TR4` (`RECV CH`, p. 25):** Displays keyboard transmit channel `Tch= 1` on Line 1.
```lcd
P.ED  RECV CH>   Tch= 1  I10:E.Piano 2A 
▶  1/   1/  */  */  */  */  */  *       
```
  - **`RHY` (`LIMIT / LOW`, p. 25):** Displays selected instrument's upper note limit `H= G8` on Line 1.
```lcd
P.ED  LIMIT/LOW>  H= G8  I10:E.Piano 2A 
▶ C-2/ C-2/  */  */  */  */  */  *      
```
  - **`LIMIT / HIGH` (p. 26):** Displays selected instrument's lower note limit `L= C-2` on Line 1.
```lcd
P.ED  LIMIT/HIGH> L= C-2 I10:E.Piano 2A 
▶ G8/  G8/  */  */  */  */  */  *       
```
  - **`TR5` (`INST DETUNE`, p. 26):**
```lcd
P.ED  INST DETUNE>Tch= 1 I10:E.Piano 2A 
▶ -2/  +2/  */  */  */  */  */  *       
```
  - **`TR6` (`NOTE SHIFT`, p. 27):**
```lcd
P.ED  NOTE SHIFT> Tch= 1 I10:E.Piano 2A 
▶ +0/  +0/  */  */  */  */  */  *       
```
  - **`TR7` (`VOLUME`, p. 27):** Upper right displays numeric volumes `74/ 96/ * /...`; lower row shows 8 vertical bar-meter levels `■` across cols `06..40`.
```lcd
P.ED   VOLUME>    74/ 96/  */  */ */ */ 
P.ED   ■   ■                            
```
    *(Note: p. 27 screenshot crops upper-right numeric list `74/ 96/ */...` alongside 8 bar meters `■` per instrument `1–8` on both rows).*
  - **`TR8` (`OUT ASSIGN`, p. 27):**
```lcd
P.ED OUT ASSIGN> Tch= 1  I10:E.Piano 2A 
▶L+R/  R/  */  */  */  */  */  *        
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - All 7 screens map `F1`–`F8` directly to Instruments 1–8 (`*` marks inactive slots where Max Notes = `0` or Reserved = `off`, which cannot be edited):
    - **`TR4` Receive Channel:** `1–16`, `omn`.
    - **`RHY` Limit Low:** `C-2–G8` (also selectable by playing a key while holding softkey/cursor if `Note On` entry enabled; cannot exceed `Limit High`).
    - **`LIMIT/HIGH` Limit High:** `C-2–G8` (cannot go below `Limit Low`).
    - **`TR5` Inst Detune:** `-7–+7` (fine pitch detune).
    - **`TR6` Note Shift:** `-24–+24` (semitone transpose, +/-2 octaves).
    - **`TR7` Volume:** `0–99` (adjusts relative balance between instruments).
    - **`TR8` Output Assign:** `off`, `L`, `R`, `L+R` (`off` silences instrument while keeping DSP/performance state; when DSP Effect `StereoMix` = `off`, L/R routing is summed mono).
- **(e) LED States:**
  - `PERFORMANCE`: `SolidGreen`. Active instrument LEDs remain `SolidGreen` (or `BlinkRed` if temporarily muted via `[-]` + `F1`–`F8`).

---

### 1.5 `OTHERS` (Performance Edit) — 4-Press Multi-Page Cycle
- **(a) Manual Page Citation:** pp. 28–30 (PDF pp. 42–44)
- **(b) Entry Button Sequence:** Press `[OTHERS]` (bottom-left of right Utility/Edit `OTHERS`/`FUNCTION` button — Edit Matrix bottom-row left key, labeled `OTHERS` above / `FUNCTION` below on faceplate) repeatedly:
  1. **1st press:** `LFO SELECT>`
  2. **2nd press:** `MICRO TUNE>` (per-instrument On/Off)
  3. **3rd press:** `MICRO TUNE)` (table & key select)
  4. **4th press:** `PFM EFCT SEL)` (performance effect select)
  5. **5th press:** wraps back to **1st press** (`LFO SELECT>`).
- **(c) Exact 40-Character LCD Templates:**
  - **Press 1 (`LFO SELECT>`, p. 28):**
```lcd
P.ED  LFO SELECT>                      ‡
▶  1/   2/  */  */  */  */  */  *       
```
  - **Press 2 (`MICRO TUNE>` On/Off, p. 28):** Line 1 displays active table # and key (`Sel= 2     C`).
```lcd
P.ED  MICRO TUNE> Sel= 2     C         ‡
▶ on/  on/  */  */  */  */  */  *       
```
  - **Press 3 (`MICRO TUNE)` Table & Key Select, p. 29):**
```lcd
P.ED MICRO TUNE)                       ‡
▶Table= 3:Pure(minor)   >Key= A         
```
  - **Press 4 (`PFM EFCT SEL)`, p. 30):**
```lcd
P.ED  PFM EFCT SEL)                    ‡
Effect =delay1(time=1.50s ps=+22 fb=0)  
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - **Press 1 (`LFO SELECT>`):** `F1`–`F8` = Instruments 1–8. Settings: `off`, `1`, `2`, `vib` (selects Voice 1 LFO, Voice 2 LFO, or fixed `vib` sine LFO).
    - *Edge Rules (p. 28):* Unavailable (`*`) on inactive slots (`Max Notes = 0` or `Reserved = off`). If only one instrument is active (`> 0`), selecting another LFO number displays `"-"` (acts as `off`). If Inst 1 or 2 has `0` notes, `1`/`2` map to the lowest-numbered active instruments.
  - **Press 2 (`MICRO TUNE>` On/Off):** `F1`–`F8` = Instruments 1–8. Settings: `off` (Equal Temperament), `on` (uses tuning selected on Press 3; only one non-equal micro tuning allowed per performance).
  - **Press 3 (`MICRO TUNE)` Select):**
    - `F1`–`F5`: Selects `▶Table=` — **2 User + 11 Preset Micro Tunings**:
      - `User octave` (`Octave`, user-edited 12-note octave tuning, p. 114)
      - `User full` (`Full Kbd.`, user-edited full-keyboard tuning, p. 115)
      - `1: Equal` (Equal temperament — no Key)
      - `2: Pure (major)` (Key: `C–B`)
      - `3: Pure (minor)` (Key: `A–G#`)
      - `4: Mean tone` (Key: `C–B`)
      - `5: Pythagorean` (Key: `C–B`)
      - `6: Werckmeister` (no Key)
      - `7: Kirnberger` (no Key)
      - `8: Vallotti & Young` (no Key)
      - `9: 1/4 shifted equal` (no Key)
      - `10: 1/4 tone` (24 notes/octave — no Key)
      - `11: 1/8 tone` (48 notes/octave — no Key)
    - `F6`–`F8`: Selects `>Key=` (`C–B` / `A–G#` for tables `2–5`; hidden/N/A otherwise).
  - **Press 4 (`PFM EFCT SEL)`):**
    - `F1`–`F8` / slider / keypad: Selects `Effect =` from `off`, `delay1..delay4`, `pan1..pan4`, `chord1..chord4` (12 user-editable memories configured via `[SETUP]` p. 111; displays summary parameters inline, e.g. `(time=1.50s ps=+22 fb=0)`).
    - *Rules (p. 30):* `delay1–4` & `chord1–4` apply ONLY to the lowest-numbered sounding instrument (`chord` fails if Max Notes < notes in chord). `pan1–4` applies to all instruments whose Output Assign is `L` or `R` (`off` and `L+R` unaffected). Distinct from DSP Effect unit (`EFFECT`).
- **(e) LED States:**
  - `PERFORMANCE`: `SolidGreen`.

---

### 1.6 `EFFECT` (Performance Edit) — 4-Press Multi-Page Cycle & All 32 DSP Effects
- **(a) Manual Page Citation:** pp. 30–35 (PDF pp. 44–49)
- **(b) Entry Button Sequence:** Press `[EFFECT]` repeatedly:
  1. **1st press:** `P.ED EFCT) No.= ...` (Per-Instrument Effect On/Off)
  2. **2nd press:** `P.ED EFCT)▶Select   >Balance`
  3. **3rd press:** `P.ED EFCT)▶OutLevel >StereoMix` (skipped/unavailable if Effect Select = `0: off`, p. 31)
  4. **4th press:** `P.ED EFCT)` Effect-specific parameter page (skipped/unavailable if Effect Select = `0: off`, p. 32)
  - Pressing `[STORE/COPY]` while on any `EFFECT` screen jumps to `EFFECT COPY>` (p. 120).
  - If `[EFFECT BYPASS]` LED is lit, upper line displays `BYPASS` (p. 30).
- **(c) Exact 40-Character LCD Templates:**
  - **Press 1 (Inst Effect On/Off, p. 30):**
```lcd
P.ED EFCT) No.=  1:RevHall             ‡
▶ on/  on/  */  */  */  */  */  *       
```
  - **Press 2 (Effect Select & Balance, p. 30):**
```lcd
P.ED EFCT)▶Select   >Balance           ‡
     1:Reverb Hall     50 %             
```
  - **Press 3 (Out Level & Stereo Mix, p. 31):**
```lcd
P.ED EFCT)▶OutLevel >StereoMix         ‡
            70 %       on               
```
  - **Press 4 (Effect Parameters, p. 32 / format shown on p. 56):** Up to 3 effect-dependent parameters across `F3`–`F8` (example for Reverb Hall):
```lcd
P.ED EFCT)▶Time    >LPF     >Delay     ‡
           2.2sec  1.25kHz  30ms        
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - **Press 1:** `F1`–`F8` = Instruments 1–8 (`off`, `on`; `*` on inactive slots).
  - **Press 2:** `F3`–`F4` (`▶Select`: `0–32`); `F5`–`F8` (`>Balance`: `0%–100%`, dry vs wet mix; note: direct 2-digit keypad stops at `99`, use `[+1]` or Data Entry slider for `100%`, p. 31).
  - **Press 3:** `F3`–`F4` (`▶OutLevel`: `0%–100%`, overall DSP output level; `0%` yields silence if `StereoMix=off`, dry-only if `StereoMix=on`); `F5`–`F8` (`>StereoMix`: `off`, `on` — adds mono DSP wet/dry to unprocessed stereo mix).
  - **Press 4 — Complete Table of All 32 DSP Effects (`0–32`, pp. 30–35):**

| No. | Effect Name | Param 1 & Range | Param 2 & Range | Param 3 & Range |
|---:|---|---|---|---|
| **0** | `off` | *(none — Presses 3 & 4 disabled)* | — | — |
| **1** | `Reverb Hall` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **2** | `Reverb Room` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **3** | `Reverb Plate` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **4** | `Delay` | `Time`: `0 ms – 300 ms` | `FB Delay`: `0 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **5** | `Delay L/R` | `Lch Dly`: `0 ms – 300 ms` | `Rch Dly`: `0 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **6** | `Stereo Echo` | `Lch Dly`: `0 ms – 300 ms` | `Rch Dly`: `0 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **7** | `Distortion Rev.` | `Time`: `0.3 sec – 18.3 sec` | `Dist.`: `0% – 100%` | `Reverb`: `0% – 100%` |
| **8** | `Distortion Echo` | `Time`: `0 ms – 300 ms` | `FB Gain`: `0% – 99%` | `Dist.`: `0% – 100%` |
| **9** | `Gate Reverb` | `Size`: `0.5 – 3.2` | `LPF`: `1.25 KHz – 12.0 KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **10** | `Reverse Gate` | `Size`: `0.5 – 3.2` | `LPF`: `1.25 KHz – 12.0 KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **11** | `Early Ref` | `Size`: `0.5 – 3.2` | `LPF`: `1.25 KHz – 12.0 KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **12** | `Tone Control 1` | `Low` (800Hz shelf): `-12dB – +12dB` | `Middle` (1260Hz BPF): `-12dB – +12dB` | `High` (3KHz shelf): `-12dB – +12dB` |
| **13** | `Delay & Rev.` | `RevTime`: `0.3 sec. – 10.0 sec` | `Delay`: `0 ms. – 152 ms` | `FB Gain`: `0%. – 99%` |
| **14** | `DelayL/R & Rev.` | `RevTime`: `0.3 sec. – 10.0 sec` | `Lch Dly`: `0 ms. – 300 ms` | `Rch Dly`: `0 ms. – 300 ms` |
| **15** | `Dist. & Delay` | `Time`: `0 ms – 300 ms` | `FB Gain`: `0% – 99%` | `Dist.`: `0% – 100%` |
| **16** | `Church` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **17** | `Club` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **18** | `Stage` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **19** | `Bath Room` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **20** | `Metal` | `Time`: `0.3 sec – 10.0 sec` | `LPF`: `1.25 KHz – 12.0KHz, thru` | `Delay`: `0.1 ms – 50 ms` |
| **21** | `Tunnel` | `RevTime`: `0.3 ms – 10.0 ms` *(printed typo in p. 34 table; sec)* | `Delay`: `0.1 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **22** | `Doubler 1` | `DlyTime`: `0.1 ms – 50 ms` | `HPF`: `160 Hz – 1000 Hz, thru` | `LPF`: `1.25KHz – 12.0KHz` |
| **23** | `Doubler 2` | `Lch Dly`: `0.1 ms – 50 ms` | `Rch Dly`: `0.1 ms – 50 ms` | `LPF`: `1.25KHz – 12.0KHz` |
| **24** | `Feed Back Gate` | `Size`: `0.5 – 3.2` | `LPF`: `1.25KHz – 12.0KHz` | `FB Gain`: `0% – 99%` |
| **25** | `F. Back Reverse` | `Size`: `0.5 – 3.2` | `LPF`: `1.25KHz – 12.0KHz` | `FB Gain`: `0% – 99%` |
| **26** | `Feed Back E/R` | `Size`: `0.5 – 3.2` | `LPF`: `1.25KHz – 12.0KHz` | `FB Gain`: `0% – 99%` |
| **27** | `Delay & Tone1` | `Bril`: `0 – 12` | `Delay`: `0.1 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **28** | `Dly L/R & Tone1` | `Bril`: `0 – 12` | `Delay`: `0.1 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **29** | `Tone Control 2` | `HPF`: `160Hz – 1000Hz, thru` | `Middle` (1260Hz BPF): `-12dB – +12dB` | `LPF`: `1.25KHz – 12.0KHz` |
| **30** | `Delay & Tone2` | `Bril`: `0 – 12` | `Delay`: `0.1 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **31** | `Dly L/R & Tone2` | `Bril`: `0 – 12` | `Delay`: `0.1 ms – 300 ms` | `FB Gain`: `0% – 99%` |
| **32** | `Distortion` | `Dist.`: `0% – 100%` | `HPF`: `160 Hz – 1000 Hz, thru` | `LPF`: `1.25KHz – 12.0KHz` |

- **(e) LED States:**
  - `EFFECT BYPASS`: `SolidRed` when bypassed (upper LCD shows `BYPASS`); `Off` when active.

---

## 2. Chapter 3 — Voice Edit (pp. 36–56)

### 2.0 Mode Entry, `e1111` / `M1111` Prefix, Operator Select & On/Off Rules
- **(a) Manual Page Citation:** pp. 16, 38, 117–121 (PDF pp. 30, 52, 131–135)
- **(b) Entry Button Sequence:**
  - From Single Play (`SINGLE`), press any of the 12 Edit keys (`TR1`–`TR8`, `RHY`, `EG` [`LIMIT/HIGH`], `FUNCTION` [`OTHERS`], `EFFECT`).
  - Or enter **Multi-Voice Edit** from Performance Play/Edit via `[OTHERS]` -> `>VoiceEdit` -> softkey `F1`–`F8` (p. 118).
  - Or start from an initialized basic 1-OP sine voice via `[OTHERS]` -> `>Init` -> `[+1]` (p. 117).
- **(c) Prefix, Operator Mask & Buffer Lifecycle:**
  - **Edit Prefix (`e1111` / `E1111` / `M1111` / `c1111`):** Upper-left cols `1–5` show edit status + 4-operator mask (`1` = operator ON, `0` = operator OFF):
    - `e1111` (printed lowercase in Ch. 3 pp. 38–56; printed uppercase `E1111` on p. 16): Normal Single Voice Edit.
    - `M1111`: Multi-Voice Edit entered from Performance (`>VoiceEdit`, p. 118; EFCT edit/copy, Compare, and Operator On/Off behave differently / unavailable per p. 118; pressing `[OTHERS]` then `[STORE/COPY]` stores modified voices individually).
    - `c1111`: Voice Compare active (`e` changes to `c`, editing locked, p. 121).
  - **Operator Select & On/Off (`SEQUENCER` row in Voice Edit, pp. 7, 38, 43–46):**
    - `[RECORD]` (`OP SELECT`): Steps active edited operator `OP1 -> OP2 -> OP3 -> OP4 -> OP1` across operator-specific screens (`TR4` AME/EBS/KVS, `RHY` Oscillator, `EG`).
    - `[BWD <<]` = `OP 1` On/Off, `[STOP ■]` = `OP 2` On/Off, `[PLAY ▶]` = `OP 3` On/Off, `[FWD >>]` = `OP 4` On/Off. All 4 OFF produces silence.
    - **Store Rule (p. 38):** Cannot store a voice with operators turned off — executing `STORE` automatically restores all 4 operators to `1` (`1111`).
  - **Line 2 Bottom-Left Algorithm Context:** Almost all Voice Edit screens display `ALG n` (or `ALG n OPx`) at Line 2 cols `1–10`.
- **(e) LED States:**
  - `SINGLE`: `SolidGreen` (indicates compare when `[COMPARE]` is active, p. 121).
  - `[BWD <<]`..`[FWD >>]` Operator LEDs: Reflect OP1–OP4 active/selected states during Voice Edit (exact green LEDs on `RECORD` / `OP 1–4` row).
  - `[COMPARE]`: Active during Compare (`c1111`).

---

### 2.1 `TR1` (`QUICK EDIT`) — 4-Press Multi-Page Cycle
- **(a) Manual Page Citation:** pp. 39–40 (PDF pp. 53–54)
- **(b) Entry Button Sequence:** Press `[TR1]` (`QUICK EDIT`) repeatedly:
  1. **1st press:** `QUICK> ATTACK` (simultaneously offsets `AR` and `D1R` of all operators; display shows `AR` values)
  2. **2nd press:** `QUICK> RELEAS` (simultaneously offsets `RR` of all operators)
  3. **3rd press:** `QUICK> VOLUME` (simultaneously offsets `Out Level` of **carrier** operators only)
  4. **4th press:** `QUICK> BRIL` (simultaneously offsets `Out Level` of **modulator** operators only)
  5. **5th press:** wraps back to **1st press**.
- **(c) Exact 40-Character LCD Templates:**
```lcd
e1111 QUICK>   OP1  OP2  OP3  OP4      ‡
ALG 4 ATTACK->+ 15   29   29   10       
```
```lcd
e1111 QUICK>   OP1  OP2  OP3  OP4      ‡
ALG 4 RELEAS->+  8    5    5   11       
```
```lcd
e1111 QUICK>   OP1  OP2  OP3  OP4      ‡
ALG 4 VOLUME->+ 99   89   58   99       
```
```lcd
e1111 QUICK>   OP1  OP2  OP3  OP4      ‡
ALG 4 BRIL  ->+ 99   89   58   99       
```
- **(d) Softkeys `F1`–`F8` & Parameter Rules:**
  - Adjusted directly via `DATA ENTRY` slider or `[-1]`/`[+1]` (relative indicator at `-> +` walks through 7 bar-height glyphs from `-50` Lower to `+50` Higher; resets to center "Original setting" icon upon leaving and re-entering Quick Edit).
  - **Non-Stored Relative Offset Rule (p. 39):** Quick Edit is NOT a stored voice parameter — it permanently modifies underlying operator `AR`/`D1R`/`RR`/`Out Level` in the edit buffer (~`±50` range; individual operators clamp at parameter min/max).

---

### 2.2 `TR2` (`ALGORITHM`) — Algorithm & Feedback
- **(a) Manual Page Citation:** pp. 40–41 (PDF pp. 54–55)
- **(b) Entry Button Sequence:** Press `[TR2]` (`ALGORITHM`).
- **(c) Exact 40-Character LCD Template:**
```lcd
e1111 ALG>    4->3\    >Feedback(OP4)   
     ▶ALG=4    2->1->      7            
```
  *(Note: Diagram chars at cols 15–22 dynamically render the active FM topology of operators `1–4` using arrows/bars, e.g. `4->3↘ 2->1->` for ALG 4).*
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F1`–`F4`: `▶ALG=` (`1–8`, FM operator connection algorithm; `1` = 4-OP vertical stack, `8` = 4 parallel carriers).
  - `F5`–`F8`: `>Feedback(OP4)` (`0–7`, self-feedback depth on `OP4`; has no effect if `OP4` Out Level = `0`).

---

### 2.3 `TR3` (`LFO`) — Wave, Speed, Delay, Key Sync, PMD, AMD
- **(a) Manual Page Citation:** pp. 41–42 (PDF pp. 55–56)
- **(b) Entry Button Sequence:** Press `[TR3]` (`LFO`).
- **(c) Exact 40-Character LCD Template:**
```lcd
e1111 LFO>▶WAVE >SPD>DLY>SYNC >PMD >AMD 
ALG 4    triangl 29  17  off   17   51  
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3` (`▶WAVE`): `saw up`, `square`, `triangl`, `S/Hold` (sample & hold).
  - `F4` (`>SPD`): `0–99` (LFO rate).
  - `F5` (`>DLY`): `0–99` (onset delay after key-on; `0` = immediate).
  - `F6` (`>SYNC`): `off`, `on` (key sync resets LFO phase on note-on).
  - `F7` (`>PMD`): `0–99` (constant Pitch Modulation Depth; requires `TR4` `PMS > 0`).
  - `F8` (`>AMD`): `0–99` (constant Amplitude Modulation Depth; requires `TR4` `AMS > 0` and per-operator `AME = on`).

---

### 2.4 `TR4` (`SENSITIVITY`) — PMS, AMS, AME, EBS, KVS
- **(a) Manual Page Citation:** pp. 16, 43–44 (PDF pp. 30, 57–58)
- **(b) Entry Button Sequence:** Press `[TR4]` (`SENSITIVITY`). Repeatedly press `[RECORD]` (`OP SELECT`) to step `OP1 -> OP2 -> OP3 -> OP4` for per-operator fields (`AME`, `EBS`, `KVS`).
- **(c) Exact 40-Character LCD Template:**
```lcd
e1111 SENS>▶PMS(all) >AMS >AME >EBS >KVS
ALG 4 OP1      5       1   off   0   +2 
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F4` (`▶PMS(all)`): `0–7` (Pitch Modulation Sensitivity across all 4 operators).
  - `F5` (`>AMS`): `0–3` (Amplitude Modulation Sensitivity, applied to operators with `AME = on`).
  - `F6` (`>AME`): `off`, `on` (per-operator Amplitude Modulation Enable; tremolo on carriers, wah on modulators).
  - `F7` (`>EBS`): `0–7` (per-operator EG Bias Sensitivity for Aftertouch/Breath Controller; raising EBS lowers base operator output level so controller raises/lowers it).
  - `F8` (`>KVS`): `-7–+7` (per-operator Key Velocity Sensitivity).

---

### 2.5 `RHY` (`OSCILLATOR`) — Mode (`ratio` vs. `fix`), Freq, Wave, Detune, Shift, Range
- **(a) Manual Page Citation:** pp. 44–45 (PDF pp. 58–59)
- **(b) Entry Button Sequence:** Press `[RHY]` (`OSCILLATOR`). Press `[RECORD]` (`OP SELECT`) to step `OP1 -> OP2 -> OP3 -> OP4`. Display format switches dynamically based on `MODE`.
- **(c) Exact 40-Character LCD Templates:**
  - **`ratio` mode (p. 44):**
```lcd
e1111 ▶MODE         >CRS>FINE>OSW >DET  
OP1   ratio          1.00  W1(^v)  +0   
```
  - **`fix` mode (p. 44):**
```lcd
e1111 ▶MODE>SHFT>RANGE>CRS>FINE>OSW >DET
OP1    fix  Hi  255Hz 176Hz W1(^v)  +0  
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - **`ratio` mode Softkeys:** `F2` (`▶MODE`), `F5` (`>CRS`), `F6` (`>FINE`), `F7` (`>OSW`), `F8` (`>DET`).
  - **`fix` mode Softkeys:** `F2` (`▶MODE`), `F3` (`>SHFT`), `F4` (`>RANGE`), `F5` (`>CRS`), `F6` (`>FINE`), `F7` (`>OSW`), `F8` (`>DET`).
  - **Parameter Ranges:**
    - `MODE`: `fix`, `ratio`.
    - `CRS` (Coarse) & `FINE` (Fine): `0.50 – 27.57` in `ratio` mode; `8 Hz – 32640 Hz` in `fix` mode (within selected `SHFT`/`RANGE`).
    - `OSW` (Oscillator Wave): `W1 – W8` (TX81Z/VF50 8 FM waveforms shown with miniature waveform icons).
    - `DET` (Detune): `-3 – +3` (at C3, `-3` = `-2.6` cents, `+3` = `+2.6` cents).
    - `SHFT` (`fix` only): `LO` (`1Hz–100Hz`), `HI` (`255Hz–32KHz`).
    - `RANGE` (`fix` only): Fixed frequency sub-band selection within `1Hz–100Hz` (`LO`) or `255Hz–32KHz` (`HI`).

---

### 2.6 `LIMIT / HIGH` (`EG`) — Operator Envelope Generator
- **(a) Manual Page Citation:** pp. 45–47, 120 (PDF pp. 59–61, 134)
- **(b) Entry Button Sequence:** Press `[LIMIT/HIGH]` (`EG`). Step operators `OP1 -> OP2 -> OP3 -> OP4` with `[RECORD]` (`OP SELECT`). Press `[STORE/COPY]` here to enter `EG COPY>` (p. 120).
- **(c) Exact 40-Character LCD Template:**
```lcd
e1111 EG> ▶AR  >D1R >D1L >D2R >RR >SHFT 
ALG 4 OP1  15   31   15    0   0   off  
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3` (`▶AR` Attack Rate): `0–31` (`0` = infinitely slow, `31` = fastest).
  - `F4` (`>D1R` Decay 1 Rate): `0–31`.
  - `F5` (`>D1L` Decay 1 Level): `0–15`.
  - `F6` (`>D2R` Decay 2 Rate): `0–31` (`0` sustains at `D1L`).
  - `F7` (`>RR` Release Rate): `1–15` (`15` = fastest release).
  - `F8` (`>SHFT` EG Shift): `off` (-96dB), `48` (-48dB), `24` (-24dB), `12` (-12dB).
    - **OP1 Lockout Rule (p. 47):** **Operator 1** is permanently locked to `"off"`; `12/24/48` on carriers sustains continuously even without key press.

---

### 2.7 `TR5` (`PEG`), `TR6` (`OUT LEVEL`), `TR7` (`SCALING`), `TR8` (`TRANSPOSE`)
- **(a) Manual Page Citation:** pp. 47–49 (PDF pp. 61–63)
- **(b) Entry Button Sequence & Cycles:** `TR5`, `TR6`, `TR8` are single-page screens; `TR7` (`SCALING`) cycles 2 pages (`RATE` <-> `LEVEL`).
- **(c) Exact 40-Character LCD Templates:**
  - **`TR5` (`PEG`, p. 47):**
```lcd
e1111 PEG> ▶PR1 >PL1 >PR2 >PL2 >PR3 >PL3
ALG 4 OP1   99   50   99   50   99   50 
```
  - **`TR6` (`OUT LEVEL`, p. 48):**
```lcd
e1111 OUT LEVEL>▶OP1 >OP2 >OP3 >OP4     
ALG 4            99   89   58   99      
```
  - **`TR7` (`SCALING`) — Press 1 (`RATE`, p. 48):**
```lcd
e1111 SCALING>  ▶OP1 >OP2 >OP3 >OP4    ‡
ALG 4 RATE        1    0    0    1      
```
    *(Manual graphic quirk: the diagram on p. 48 prints `>OP2 >OP2 >OP2` due to an original Hardware artwork typo).*
  - **`TR7` (`SCALING`) — Press 2 (`LEVEL`, p. 48):**
```lcd
e1111 SCALING>  ▶OP1 >OP2 >OP3 >OP4    ‡
ALG 4 LEVEL      +17  +36   +0  -20     
```
  - **`TR8` (`TRANSPOSE`, p. 49):**
```lcd
e1111 TRANSPOSE> ▶Middle C   >KBD       
ALG 4                C3       in        
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - **`TR5` PEG (`F3`..`F8`):** `PR1, PR2, PR3` (`0–99` rate), `PL1, PL2, PL3` (`0–99` level; `50` = normal pitch, `99` = +4 octaves, `0` = -4 octaves). Applies to all 4 operators.
  - **`TR6` OUT LEVEL (`F4`..`F7`):** `OP1–OP4` output level `0–99`.
  - **`TR7` SCALING (`F4`..`F7`):** Press 1 Keyboard Rate Scaling `0–3`; Press 2 Keyboard Level Scaling `-99–+99`.
  - **`TR8` TRANSPOSE:** `F4`–`F6` (`▶Middle C`: `C1–C5`, also selectable by playing C1–C5 key); `F7`–`F8` (`>KBD in` toggle / keyboard input active indicator).

---

### 2.8 `FUNCTION` (`OTHERS`/`FUNCTION`) — 8-Item Submenu Tree
- **(a) Manual Page Citation:** pp. 49–54 (PDF pp. 63–68)
- **(b) Entry Button Sequence & Navigation:**
  - Press `[FUNCTION]` (bottom-row left Edit key, shared with Performance Edit `OTHERS`) to open the Voice Function submenu selector.
  - Press softkey `F1`–`F8` beneath `"PBetc"`, `"Port"`, `"FC"`, `"MW"`, `"BC"`, `"AT"`, `"REVR"`, or `"NAME"` to enter that submenu; press `F8` (`>EXIT`) inside any submenu to return to the `FUNCTION` selector.
- **(c) Exact 40-Character LCD Templates:**
  - **Root `FUNCTION` Selector Screen:** *(Note: The manual prose on pp. 49–54 quotes the exact 8 softkey labels `"PBetc"`, `"Port"`, `"FC"`, `"MW"`, `"BC"`, `"AT"`, `"REVR"`, `"NAME"` across `F1`–`F8`, but omits a graphic of the top-level selector screen itself; softkey labels are verbatim from pp. 49–54 and header Line 1 below is inferred to match Ch. 3 style).*
```lcd
e1111 FUNCTION> Select one !            
>PBetc>Port>FC >MW  >BC  >AT  >REVR>NAME
```
  - **`F1` -> `PBetc` (`P.BEND>`, p. 49):**
```lcd
e1111 P.BEND> ▶Mode   >PBR >FSW         
ALG 4      poly mode   2    sus    >EXIT
```
    - Ranges: `Mode` (`poly mode` [last note priority], `mono mode`), `PBR` (`0–12` semitones), `FSW` (`por` [portamento hold], `sus` [sustain]).
  - **`F2` -> `Port` (`PORTA>`, p. 50):**
```lcd
e1111 PORTA>   Mode      ▶Time          
ALG 4     Full Time Porta  0       >EXIT
```
    - Ranges: `Mode` (`Full Time Porta` only when poly; `Full Time Porta` or `Fingerd Porta` when mono — note manual spelling `Fingerd Porta` in LCD), `Time` (`0–99`).
  - **`F3` -> `FC` (`FC>`, pp. 50–51):**
```lcd
e1111 FC>   ▶Vol >Pitch  >Amplitude     
ALG 4        30    0         0     >EXIT
```
    - Ranges: `Vol` (`0–99`), `Pitch` (`0–99`), `Amplitude` (`0–99`).
  - **`F4` -> `MW` (`MW>`, p. 51):**
```lcd
e1111 MW>   ▶Pitch  >Amplitude          
ALG 4         55       14          >EXIT
```
    - Ranges: `Pitch` (`0–99`), `Amplitude` (`0–99`).
  - **`F5` -> `BC` (`BC>`, p. 52):**
```lcd
e1111 BC>▶Pitch>Ampli>P.Bias  >EG Bias  
ALG 4       0    0    +0        0  >EXIT
```
    - Ranges: `Pitch` (`0–99`), `Ampli` (`0–99`), `P.Bias` (`-50–+50`, ~`±4` oct), `EG Bias` (`0–99`).
  - **`F6` -> `AT` (`AT>`, pp. 53–54):**
```lcd
e1111 AT>▶Pitch>Ampli>P.Bias  >EG Bias  
ALG 4       0   20    +0        0  >EXIT
```
    - Ranges: `Pitch` (`0–99`), `Ampli` (`0–99`), `P.Bias` (`-50–+50`), `EG Bias` (`0–99`).
  - **`F7` -> `REVR` (`REVERB RATE>`, p. 54):**
```lcd
e1111 REVERB RATE> >Rate                
ALG 4                2             >EXIT
```
    - Ranges: `Rate` (`off`, `0–7` — simulated envelope reverb slowing `OP1` release below `-18dB`; `1` = longest, `7` = shortest; distinct from DSP unit).
  - **`F8` -> `NAME` (`VOICE NAME>`, p. 54):**
```lcd
e1111 VOICE NAME>                       
ALG 4   NAME : E.ORGAN     <-   -> >EXIT
```
    - Ranges: Up to 10 characters (p. 17 character entry rules; `F6`/`F7` `<-`/`->` cursor movement; `F8` `>EXIT`).

---

### 2.9 `EFFECT` (Voice Edit) — 3-Press Multi-Page Cycle
- **(a) Manual Page Citation:** pp. 32–35, 54–56, 120 (PDF pp. 46–49, 68–70, 134)
- **(b) Entry Button Sequence:** Press `[EFFECT]` repeatedly (3 screens, NO per-instrument on/off page in Single Voice Edit):
  1. **1st press:** `e1111 EFCT>▶Select   >Balance`
  2. **2nd press:** `e1111 EFCT>▶OutLevel >StereoMix` (unavailable if Effect Select = `0: off`)
  3. **3rd press:** `e1111 EFCT>` Effect parameters (unavailable if Effect Select = `0: off`)
  - Press `[STORE/COPY]` on any `EFCT>` screen to enter `EFFECT COPY>` (p. 120).
- **(c) Exact 40-Character LCD Templates:**
```lcd
e1111 EFCT>▶Select   >Balance          ‡
      1:Reverb Hall    50 %             
```
```lcd
e1111 EFCT>▶OutLevel >StereoMix        ‡
              70 %      on              
```
```lcd
e1111 EFCT>▶Time     >LPF    >Delay    ‡
            2.2sec   1.25kHz 30ms       
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:** Identical `0–32` DSP types, `Balance` (`0–100%`), `OutLevel` (`0–100%`), `StereoMix` (`off`, `on`), and Effect Parameter ranges as §1.6 table (pp. 32–35). Note: If `StereoMix = off` and DSP Effect is on, Performance L/R assignments and `PAN` performance effects have no stereo effect (p. 56).
- **(e) LED States:** `[EFFECT BYPASS]`: `SolidRed` when bypassed (upper LCD displays `BYPASS`).

---

## 3. Chapter 4 — Rhythm Machine (pp. 57–77)

### 3.1 `PTN PLAY>` & `SONG PLAY)` — Play Modes, `toSONG`/`toPTN`, Continuity
- **(a) Manual Page Citation:** pp. 57–59, 71 (PDF pp. 71–73, 85)
- **(b) Entry Button Sequence:** Press `[RHYTHM]` to enter Rhythm Machine (returns to last submode: `PTN PLAY>` or `SONG PLAY)`). Press `F1` (`>toSONG` / `>toPTN`) to toggle between Rhythm Pattern Play and Rhythm Song Play.
- **(c) Exact 40-Character LCD Templates:**
  - **Pattern Play (`PTN PLAY>`, pp. 58–59):**
```lcd
PTN PLAY> ▶Pattern  Time  Bar>Tempo>KBD 
>toSONG     P10w    4/4    1   120  RHY 
```
    *(While playing back, `>toSONG` disappears from Line 2 cols `1–7`; queued next pattern number blinks until current pattern finishes).*
  - **Rhythm Song Play (`SONG PLAY)`, p. 71):**
```lcd
SONG PLAY)▶SONG   >Meas >Part>Tempo>KBD 
>toPTN   1w:Moon   001   001   120  RHY 
```
    *(While playing back, `>toPTN` disappears and the currently playing measure number + pattern name are displayed).*
- **(d) Softkeys `F1`–`F8`, Transport & Playback Rules:**
  - **`PTN PLAY>` Softkeys:**
    - `F1`: `>toSONG` (switches to `SONG PLAY)`, hidden during playback).
    - `F2`–`F3`: `▶Pattern` (`P00–P99` Preset via `[PRESET]`, `I00–I99` Internal via `[INT]`; trailing **`w`** marks patterns containing written data, e.g. `P10w`; blank patterns omit `w`).
    - *(Cols 20–29 display pattern `Time` signature & `Bar` count `1–4`, read-only).*
    - `F6`–`F7`: `>Tempo` (`30–240` BPM; also adjusted live anywhere in Rhythm mode by holding `[RHYTHM]` + moving `DATA ENTRY`).
    - `F8`: `>KBD` (`RHY` = keyboard plays mapped rhythm sounds regardless of Tx/Rx channels; `SYN` = keyboard plays previously selected Performance or Single voice).
  - **`SONG PLAY)` Softkeys:**
    - `F1`: `>toPTN` (switches to `PTN PLAY>`, hidden during playback).
    - `F2`–`F4`: `▶SONG` (`1–8`, trailing `w` if written + 8-char song name e.g. `1w:Moon`).
    - `F5`: `>Meas` (`001–...`, start measure; also adjusted by `[BWD <<]` / `[FWD >>]` when stopped).
    - `F6`: `Part` (`001–999`, part index).
    - `F7`: `>Tempo` (`30–240`).
    - `F8`: `>KBD` (`RHY` / `SYN`).
  - **Transport & Cross-Mode Continuity (pp. 58–59, 71):**
    - `[PLAY ▶]`: Starts loop playback of pattern (or rhythm song; in Rhythm Song mode, any Sequencer song with matching song # plays simultaneously unless sequencer `TR1–TR8` are muted in `[SEQ]`, p. 58). In Pattern mode, `[PLAY ▶]` does NOT start the sequencer song (p. 59).
    - `[STOP ■]`: Stops immediately; `[STOP ■] + [BWD <<]` together rewinds to pattern/song top before pressing `[PLAY ▶]`.
    - **Mode Persistence (pp. 59, 71):** While Rhythm Machine is playing, pressing `[PERFORMANCE]` or `[SINGLE]` switches to synth play/edit WITHOUT stopping rhythm playback. Press `[STOP ■]` in `[PERFORMANCE]`, `[SINGLE]`, or `[RHYTHM]` to stop (cannot stop directly while inside synth edit mode — return to play mode first per p. 71).
- **(e) LED States:**
  - `[RHYTHM]`: `SolidGreen`.
  - `[PLAY ▶]` / `[STOP ■]`: Active transport LEDs (`SolidGreen`).
  - `TR1`–`TR8`: Blink/light during `PTN PLAY>` playback to reflect active rhythm instruments (`p. 59`).

---

### 3.2 `P.REC>` — Pattern Realtime & Step Recording + Bar Graph
- **(a) Manual Page Citation:** pp. 60–65 (PDF pp. 74–79)
- **(b) Entry Button Sequence:** From `PTN PLAY>` (stopped), press `[RECORD]` (`o`) to enter `P.REC>` setup, then `[PLAY ▶]` to start recording (either `Real` or `Step`). Press `[STOP ■]` to finish recording and return to `PTN PLAY>`.
- **(c) Exact 40-Character LCD Templates:**
  - **Record Setup (`P.REC>`, pp. 61–62):**
```lcd
P.REC>>PTN >Time >Bar >Tempo >Qntz      
▶Real  I00  4/4    2   120   1/16  >EXIT
```
  - **Preset Pattern Store Prompt (on `[STOP ■]` after recording a `P00–P99` pattern, pp. 62–63):**
```lcd
P.REC> Store Pattern                    
       ▶ptnI**                >GO  >EXIT
```
    *(Cannot overwrite preset ROM `P00–P99`; choose `I00–I99` -> `F7` `>GO` -> `"Are you sure?"` -> `[+1]`).*
  - **Step Recording Bar Graph (`USING THE BAR GRAPH`, pp. 62–65):**
```lcd
Bar1:q |--------+--------+--------+----|
ptnI00  ▶Qntz >BD1              <-   -> 
```
    *(Header shows `Bar1:` through `Bar4:`, active quantize note glyph, and up to 1 measure divided by `+` beat markers into `-` 32nd-note steps; cols `1–3` show Pattern `ptnI00`, `>Acc=n` appears on cols `25–30` whenever step cursor `_` sits on an entered note diamond).*
- **(d) Softkeys `F1`–`F8`, Ranges & Controls:**
  - **`P.REC>` Setup:** `F1` (`▶Real`/`Step` mode), `F2` (`>PTN`: `I00–I99`, `P00–P99`), `F3` (`>Time`: `1/4–8/4`, `1/8–16/8`, `1/16–32/16` — locked if pattern already has data), `F4` (`>Bar`: `1–4` — locked if pattern already has data), `F5` (`>Tempo`: `30–240`), `F6` (`>Qntz`: `1/4, 1/6, 1/8, 1/12, 1/16, 1/24, 1/32, off`), `F8` (`>EXIT`).
  - **Realtime Erase (p. 62):** Hold `[-]` (`ERASE`) + play key at desired beat (or hold key across entire pattern) to erase that instrument note.
  - **Step Bar Graph Controls (pp. 63–65):**
    - `F2` (`>Qntz`): Select quantize via numeric keys `[3]`=`1/4`, `[4]`=`1/8`, `[5]`=`1/16`, `[6]`=`1/32`, `[9]`=triplet (`1/6, 1/12, 1/24`, press again to revert), `[0]`=`off` (enables 1/6th-of-32nd micro-step `#1..#6` + `X` marker). *Blink rule:* Changing quantize while step cursor `_` is mid-measure blinks the quantize icon until returned to beat 1.
    - `F3`–`F4` (`>BD1` / inst): Select rhythm instrument via keypad/slider, or **hold `F4` + play a key** to switch visible instrument track without recording a note.
    - `F5`–`F6` (`>Acc=0..7`): Appears only when `_` is on an existing note; sets dynamic accent `0` (min) – `7` (max).
    - `F7` (`<-`) / `F8` (`->`): Advances step cursor `_` by current quantize (or 1/6th of 32nd when `Qntz=off`). `[BWD <<]`/`[FWD >>]` always step by one 32nd note.
    - Hold `[-]` (`ERASE`) + key at `_` erases the note on that step.
- **(e) LED States:**
  - `[RECORD]`: `SolidGreen` while in `P.REC>`. `[PLAY ▶]`: `SolidGreen` during Realtime or Step bar graph.

---

### 3.3 `PATTERN JOB SELECT)` — Pattern Jobs, 61 Instruments & `RHYTHM ASSIGN)`
- **(a) Manual Page Citation:** pp. 57, 66–70 (PDF pp. 71, 80–84)
- **(b) Entry Button Sequence:** From `PTN PLAY>`, press `[JOB]` to open `PATTERN JOB SELECT)`. Note: `SETUP>`, `INST SET>`, and `RHYTHM ASSIGN>` settings are **shared** with `SONG JOB SELECT)`.
- **(c) Exact 40-Character LCD Templates:**
  - **Pattern Job Menu (p. 66):**
```lcd
PATTERN JOB SELECT)     Select one !    
 >Copy>Clr>Setup>Inst>Asgn         >EXIT
```
  - **`>Copy` (`PTN COPY>`, p. 66):**
```lcd
PTN COPY>                               
▶ptnI00w+ ptn***  ->  ptnI**   >GO >EXIT
```
    *(Copies 1 pattern or concatenates 2 same-time-signature patterns `<= 4` bars total to `ptnI**`; preset destinations blocked. `[+1]` confirms `"Are you sure?"`).*
  - **`>Clr` (`PTN CLEAR>`, p. 67):**
```lcd
PTN CLEAR>              Select one !    
 >Pattern   >Inst    >All          >EXIT
```
    *(Clears single internal pattern, single instrument inside internal pattern, or all `I00–I99` patterns; cannot clear presets. Confirms via `"Are you sure?"` -> `[+1]`).*
  - **`>Setup` (`SETUP>`, pp. 67–68):**
```lcd
SETUP>▶Rch >Tch  >Vel >Click>Beat >Sync 
        9    9    on   rec   1/4   int  
```
    *(With `>EXIT` at `F8` per manual screenshot — col 36–40: `SETUP>▶Rch>Tch >Vel>Click>Beat>Sync` / `       9   9   on  rec   1/4  int >EXIT`). Exact 40-char Line 1/2:*
```lcd
SETUP>▶Rch>Tch >Vel>Click>Beat>Sync     
        9   9   on  rec   1/4  int >EXIT
```
    - Ranges: `Rch` (`1–16, omn`), `Tch` (`off, 1–16`), `Vel` (`off` [fixed accent 5], `on`), `Click` (`rec`, `play`), `Beat` (`1/4, 1/6, 1/8, 1/12, 1/24, 1/32`), `Sync` (`int`, `MIDI` — shared with Sequencer Sync on p. 89).
  - **`>Inst` (`INST SET>`, pp. 68–69):**
```lcd
INST SET>>Volume>Pan>Note>R.Vol>Efct    
▶BD 1       15   L|||R G#1  99   on>EXIT
```
    *(61 instruments listed below; Pan displays a 7-step `L...R` slider graphic).*
    - Ranges: `Inst` (`BD 1`..`VbrSlp`, also selectable via keyboard), `Volume` (`0–15` per inst), `Pan` (7 steps `L..R` per inst), `Note` (`C-2–G8` / printed `C-2 - C8` MIDI note # per inst), `R.Vol` (`0–99` **global** master rhythm volume), `Efct` (`off`, `on` [mono centered], `mix` [DSP + stereo pan mix] — **global** rhythm DSP routing).
    - **61 Rhythm Instruments Inventory (p. 57, exact order):**
      1. `BD 1`, `BD 2`, `BD 3`, `H. BD`, `GateBD`, `E. BD`, `SD 1`, `SD 2`, `Pic1SD`, `H. SD 1`, `H. SD 2`, `GateSD`, `E. SD`, `Rim 1`, `Rim 2`, `Tom 1`, `Tom 2`, `Tom 3`, `Tom 4`, `F.Tom1`, `F.Tom2`, `F.Tom3`, `F.Tom4`, `E.Tom1`, `E.Tom2`, `E.Tom3`, `E.Tom4`, `HHclos`, `HHopen`, `HH1/4o`, `HHpdl`, `Ride`, `Edge`, `Crash`, `FMprc1`, `FMprc2`, `FMprc3`, `GlsCsh`, `BellTr`, `TimpnH`, `TimphL`, `Claps`, `Shaker`, `Cowbel`, `Timb1H`, `Timb1L`, `Whst1S`, `Whst1L`, `CgaHMT`, `CgaHOP`, `Cga LO`, `Bgo HI`, `Bgo LO`, `CuicaH`, `CuicaL`, `Ago HI`, `Ago LO`, `Tambrn`, `Claves`, `Cstnt`, `VbrSlp`.
  - **`>Asgn` (`RHYTHM ASSIGN>`, pp. 69–70):**
```lcd
RHYTHM ASSIGN>     >Key  Inst           
▶PRESET1            C1 = BD 1      >EXIT
```
    - Settings: `PRESET1` (Hardware RX layout within 61 keys), `PRESET2` (C3 basic kit / white keys BD/SD/Toms, black percussion), `PRESET3` (Clavinova layout), `USER 1`, `USER 2` (editable across 61 keys `C1–C6` with any of the 61 instruments or `none`).
- **(e) LED States:** `[RHYTHM]` `SolidGreen`; `[JOB]` active context while inside Job submenu.

---

### 3.4 `SONG JOB SELECT)` — Rhythm Song Editing & Part Jobs
- **(a) Manual Page Citation:** pp. 71–77 (PDF pp. 85–91)
- **(b) Entry Button Sequence:** From `SONG PLAY)`, press `[JOB]` to open `SONG JOB SELECT)`.
- **(c) Exact 40-Character LCD Templates:**
  - **Rhythm Song Job Menu (p. 74 / p. 76):**
```lcd
SONG JOB SELECT)     Select one !       
>Edit>Copy>Clr>Setup>Inst>Asgn>Srch>EXIT
```
  - **Song Part Edit (`>Edit` -> `SONG EDIT>`, pp. 72–74):**
```lcd
SONG EDIT)  Part 001 = PTN ***          
song1w      <-    ->   <-    -> >JOB>EXT
```
    *(Exact col alignment of `>JOB` and `>EXIT` on p. 74/76: `song1w      <-   ->   <-   -> >JOB >EXIT` — trimmed spacing so `>JOB >EXIT` fits at cols `31–40`)*:
```lcd
SONG EDIT)   Part 001 = PTN I01w        
song1w       <-   ->   <-  -> >JOB >EXIT
```
    - **Special Non-Pattern Part Types (selected via right `<-`/`->`, `F5`–`F6`, pp. 72–74):** Cycles through `= PTN***` (`P00–P99`/`I00–I99`), `||:` (repeat begin), `:|| x N` (repeat end + count, supports nesting `[||: [part] [||: [part] :||] :||]`), `Vol+N` / `Vol-N` (relative R.Vol delta, cannot exceed R.Vol), `T+ N/B` / `T- N/B` (tempo change amount / beats `B`, clamped `30–240`), and `Mark [X]` (single char; first 7 marks searchable).
  - **In-Edit Sub-Job Menu (`F7` `>JOB` -> `SONG ED JOB)`, pp. 75–76):**
```lcd
SONG ED JOB) Part 001 = PTN I00w        
>Part>Ins >Del>Copy>Search>Name    >EXIT
```
    - **`>Part` (Jump):** `Input  Part ***          >GO >EXIT` (`F7` `>GO` jumps to part or last+1 if empty).
    - **`>Ins` (Insert):** `Insert Part ?                  >EXIT` (`[+1]` inserts blank `=PTN***` before current part, shifts rest down).
    - **`>Del` (Delete):** `Delete Part ?                  >EXIT` (`[+1]` deletes current part, shifts rest up).
    - **`>Copy` (Copy Parts):** `( Part *** - ***)-> Part ***  >GO >EXIT` (copies part range before destination -> `>GO` -> `[+1]`).
    - **`>Search` (Mark Search):** `[A]  [B]  [C]                      >EXIT` (up to 7 marks across `F1`–`F7`; unavailable if no marks).
    - **`>Name` (Song Name):** `name : NewSong         <-   ->     >EXIT` (max 8 chars, p. 17 char entry).
  - **Song Copy (`SONG JOB SELECT)` -> `>Copy`, p. 76):**
```lcd
SONG COPY>                              
▶from SONG 1w  to SONG *       >GO >EXIT
```
  - **Song Clear (`SONG JOB SELECT)` -> `>Clr`, p. 77):**
```lcd
SONG CLEAR>            Select one !     
>Song    >All                      >EXIT
```
  - **Song Play Mark Search (`SONG JOB SELECT)` -> `>Srch`, p. 77):**
```lcd
SEARCH>                Select mark !    
[A]  [B]  [C]                      >EXIT
```
  - **`>Setup`, `>Inst`, `>Asgn`:** Identical screens and shared state with §3.3 (`SETUP>`, `INST SET>`, `RHYTHM ASSIGN>`).
- **(e) LED States:** `[RHYTHM]` `SolidGreen`; blinking `= PTN ***` on empty Song Edit parts.

---

## 4. Chapter 6 — Edit Buffer Utilities, Store, Compare, Copy & Init (pp. 16, 109, 117–121)

### 4.1 `MEMORY PROTECT` — Internal & Card Write Protection
- **(a) Manual Page Citation:** p. 109 (PDF p. 123)
- **(b) Entry Button Sequence:** Press `[MEMORY PROTECT]` (in right Utility block).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  MEMORY PROTECT> ▶INT      >CARD     
                     off        on      
```
- **(d) Softkeys `F1`–`F8` & Protection Rules:**
  - `F4`: `▶INT` (`off`, `on`) — blocks Voice/Performance `STORE`, Card/Disk/MIDI bulk loads into Internal memory. Defaults to **`on`** on every power-up. Can be temporarily bypassed per-store via `"Prot?"` prompt on `[STORE/COPY]` (p. 119).
  - `F6`: `>CARD` (`off`, `on`) — blocks `STORE` and Internal->Card copies. Defaults to **`on`** on power-up. Additionally, if the physical memory card's hardware write-protect switch is `on` (or card is unformatted), saving to card is impossible even if LCD `CARD` = `off` (no `"Prot?"` bypass).
- **(e) LED States:** Utility mode active (last Play LED preserved or Utility indicator); no dedicated LED on `[MEMORY PROTECT]`.

---

### 4.2 Utility `OTHERS` — Init, Recall, Preset Load & Multi-Voice Edit (`M1111`)
- **(a) Manual Page Citation:** pp. 117–118 (PDF pp. 131–132)
- **(b) Entry Button Sequence:**
  - Press Utility `[OTHERS]` (right-hand Utility block, distinct from Edit-matrix `[OTHERS]`/`[FUNCTION]`). Menu adapts automatically to Single vs. Performance context:
    - From **Single Play** or **Voice Edit**: shows `UT OTHERS> VOICE`.
    - From **Performance Play** or **Performance Edit**: shows `UT OTHERS> PFM`.
- **(c) Exact 40-Character LCD Templates & Subtrees:**
  - **Voice Utility Menu (`UT OTHERS> VOICE`, pp. 117–118):**
```lcd
UT  OTHERS> VOICE       Select one !    
>Init >Recall                >PresetLoad
```
    - `F1` (`>Init`): Prompts `"Are you sure?"`; press `[+1]` to reset edit buffer to basic 1-OP sine wave voice (`Note:` initializes voice `FUNCTION` parameters too even if Utility `COMBINE` = `off`, p. 117).
    - `F2` (`>Recall`): Prompts `"Are you sure?"`; press `[+1]` to restore most recently edited voice buffer lost when changing voice numbers without storing.
    - `F6`–`F7` (`>PresetLoad`): Enters `UT OTHERS> PRESET LOAD`.
  - **Performance Utility Menu (`UT OTHERS> PFM`, pp. 117–118):**
```lcd
UT  OTHERS> PFM         Select one !    
>Init >Recall   >VoiceEdit   >PresetLoad
```
    - `F1` (`>Init`): Opens `UT OTHERS> PFM INIT` template selector:
```lcd
UT  OTHERS> PFM  INIT   Select one !    
SNGL DUAL SPLT 4LYR 8LYR SEQ4 SEQ8 >EXIT
```
      - `F1` `SNGL` (1 inst), `F2` `DUAL` (2 layered insts), `F3` `SPLT` (2 insts split at `B2`/`C3`), `F4` `4LYR` (4 layered insts), `F5` `8LYR` (8 layered insts), `F6` `SEQ4` (4 seq multitimbral insts), `F7` `SEQ8` (8 seq multitimbral insts), `F8` `>EXIT`. Pressing `F1`–`F7` prompts `"Are you sure?"` -> `[+1]`.
    - `F2` (`>Recall`): Prompts `"Are you sure?"` -> `[+1]` to recover last edited unstored performance.
    - `F6`–`F7` (`>PresetLoad`): Opens shared `UT OTHERS> PRESET LOAD`:
```lcd
UT  OTHERS> PRESET LOAD Select one !    
ALL      PEFCT MCT                 >EXIT
```
      - `F1` (`ALL`: reloads preset performance effects & preset micro tunings), `F3` (`PEFCT`: performance effects only), `F4` (`MCT`: micro tunings only), `F8` (`>EXIT`). Prompts `"Are you sure?"` -> `[+1]`. *Warning:* Erases user performance effects / micro tunings in internal memory.
    - **`F4`–`F5` (`>VoiceEdit`) — Multi-Voice Edit Mode (`M1111`, p. 118):**
```lcd
UT  OTHERS> VOICE EDIT   which voice ?  
 i01/ i03/ I13/  */  */  */  */  *      
```
      - Press `F1`–`F8` below an instrument's voice to jump straight into editing that voice inside the performance (`M1111` prefix at cols `1–5` instead of `e1111`; lower-case `i` / upper-case `I` tracks bank/edit state across up to 8 performance voices simultaneously).
      - Pressing a **non-edit** key returns to `UT OTHERS> VOICE EDIT`.
      - **Cancel Warning:** Pressing `[SINGLE]` before storing modified voices displays `"** go to SGL mode Sure?"`; pressing `[+1]` discards multi-voice edits and enters Single Play.
      - **Behavioral Differences vs. Normal Voice Edit (p. 118):** (1) DSP Effect edit/copy, (2) Compare mode, and (3) Operator On/Off behave differently / are restricted; setting an instrument's `MAX NOTES = 0` silences the voice being edited.
- **(e) LED States:** Preserves host play mode LED; confirmation prompts blink until `[+1]` or cancel.

---

### 4.3 `STORE / COPY` — Voice Store, Performance Store, Multi-Voice Store & Copy Functions
- **(a) Manual Page Citation:** pp. 119–120 (PDF pp. 133–134)
- **(b) Entry Button Sequence & Hold-To-Specify UX:**
  - Press and **hold** `[STORE/COPY]` while specifying destination (`[INT]` / `[CARD]` + 2-digit destination number `00–99`), then **release** `[STORE/COPY]` -> confirms `"OK?"` -> press `[+1]` to commit.
  - Note: Pressing `[STORE/COPY]` commits any active muted instruments (`[-]` + `F1..F8`) or turned-off FM operators (`1111`) back ON automatically (pp. 23, 38).
- **(c) Exact 40-Character LCD Templates:**
  - **1. Voice Store (`SINGLE MODE>`, from Single Play / Voice Edit, p. 119):**
```lcd
SINGLE MODE>                            
     Mem Store I23 -> I  ?              
```
    - If Internal `Memory Protect = on`, right edge shows `"Prot?"`; pressing the softkey (`F8`) below `"Prot?"` temporarily defeats internal protect for this store. Card `Memory Protect = on` or hardware protect switch `on` cannot be bypassed.
  - **2. Performance Store (`PERFORMANCE MODE>`, from Performance Play / Edit, p. 119):**
```lcd
PERFORMANCE MODE>                       
     PFM Store I04 -> I  ?              
```
  - **3. Multi-Voice Store (`UT OTHERS> STORE VOICE`, after `>VoiceEdit`, p. 119):**
    - Entry: `[OTHERS]` -> press/hold `[STORE/COPY]` -> tap `F1`–`F8` below voice to store + specify destination + release `[STORE/COPY]` -> `"OK?"` -> `[+1]`. Repeatable for each edited voice.
    - Bank rule: Entered from Internal perf -> Internal destinations only; Card perf -> Card destinations only. Only available inside Voice Edit / Multi-Voice Edit context.
```lcd
UT OTHERS>  STORE VOICE  which voice ?  
 i01/ i03/ I13/  */  */  */  */  *      
```
    *(Where `"which voice ?"` blinks in the upper right).*
  - **4. Effect Copy (`EFFECT COPY>`, while in Voice or Performance `EFFECT` edit, p. 120):**
```lcd
EFFECT COPY>  EFCT=1:Reverb Hall        
      current effect data -> I  ?       
```
    - Copies current DSP effect parameters to another Voice or Performance (`INT`/`CARD`), blocked by Memory Protect.
  - **5. Performance Effect Copy (`PFM EFFECT COPY>`, while in `[SETUP]` -> Performance Effect edit, p. 120):**
```lcd
PFM EFFECT COPY>                        
      delay 1 -->  delay ?              
```
    - Can copy ONLY between memories `1–4` of the **same** performance effect type (`delay`, `pan`, or `chord`).
  - **6. Envelope Generator Copy (`EG COPY>`, while in Voice `EG` edit, p. 120):**
```lcd
EG COPY>  set source & destination OP ! 
          >OP? --> >OP?                 
```
    - Hold `[STORE/COPY]`, select source `OP` (`1–4`), select destination `OP` (`1–4`), release `[STORE/COPY]`.
    - **Copies:** `AR`, `D1R`, `D2R`, `RR`, `LS` (Keyboard Level Scaling), `RS` (Keyboard Rate Scaling). **Excludes:** `EG Shift` (`SHFT`).
- **(e) LED States:** `[STORE/COPY]` hold triggers confirmation prompt; bank LED (`[INT]`/`[CARD]`) reflects destination selection.

---

### 4.4 `COMPARE` — Original vs. Edited A/B Comparison & Lockout Rules
- **(a) Manual Page Citation:** pp. 16, 121 (PDF pp. 30, 135)
- **(b) Entry Button Sequence:**
  - While in **Voice Edit** (or **Single Play** with unstored edit) or **Performance Edit** (or **Performance Play** with unstored edit), press `[COMPARE]` to toggle to the unedited original buffer. Press `[COMPARE]` again to return to the edited buffer.
- **(c) Exact 40-Character LCD Templates:**
  - **Voice Compare (`c1111`, p. 121):** Upper-left `e` switches to `c` (`c1111`):
```lcd
c1111 ALG>    4->3\    >Feedback(OP4)   
     ▶ALG=4    2->1->      7            
```
  - **Performance Compare (`c.ED`, p. 121):** Prefix switches from `P.ED` to `c.ED`:
```lcd
c.ED  ▶AssignMode     >name : SunLight  
         DVA                  <-   ->   
```
    *(Manual text on p. 121 states `"the 'e' in the upper left of the display will change to a 'c'"`, while the Performance Compare LCD illustration prints `c.ED`).*
  - **Hold `[SINGLE]` / `[PERFORMANCE]` PC Transmit Check (p. 16, cross-referenced from Phase 1):** While already in Single or Performance mode (and not locked by Compare), holding `[SINGLE]` or `[PERFORMANCE]` shows:
```lcd
** Sending PC No ---                    
                                        
```
- **(d) Parameter Editing & Lockout Rules:**
  - **Editing Locked (p. 121):** While Compare (`c`) is active, **all parameter editing is disabled** until `[COMPARE]` is pressed again to restore `e` / `P.ED`.
  - **Mode-Switch Lockout (p. 16):** You **cannot** switch modes (`[SINGLE]`, `[PERFORMANCE]`, `[RHYTHM]`, `[SEQ]`) while `[COMPARE]` is active during Voice or Performance editing.
- **(e) LED States:**
  - **Voice Compare:** LED left of `[SINGLE]` illuminates (`SolidGreen`, p. 121) + `[COMPARE]` LED (`SolidGreen`) indicates active Compare state.
  - **Performance Compare:** LED left of `[PERFORMANCE]` illuminates (`SolidGreen`, p. 121) + `[COMPARE]` LED (`SolidGreen`) indicates active Compare state.
