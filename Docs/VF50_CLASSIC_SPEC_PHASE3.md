# Victory VF50 Classic Mode Firmware Specification — Phase 3 (Gate 5)

**Source of Truth:** `Docs/VF50 Operating Reference Manual` (171 PDF pages; PDF page = Manual page + 14)  
**Scope:** Chapter 6 Card Operations (pp. 95–98), Chapter 6 MIDI Functions (pp. 99–104), Chapter 6 Utility Setup Functions (pp. 110–116), and Chapter 1 Demo Playback, Power-On Greeting Message & Shortcuts (pp. 11, 13–15, 22).  
**Architectural Contract:**
- Completes the Classic Mode specification alongside `Docs/VF50_CLASSIC_SPEC_PHASE1.md` (Chapters 1, 5, 6 Disk) and `Docs/VF50_CLASSIC_SPEC_PHASE2.md` (Chapters 2, 3, 4, 6 Buffer Utilities).
- Every 40×2 LCD template below fits strictly within **40 characters per line**.
- **Display & Navigation Conventions:** `▶` (active cursor / editable field), `>` (softkey-selectable item or submenu mapped to `F1`–`F8` directly below it), `←`/`→`/`↑`/`↓` (navigation softkeys).
- **Hardware LED Vocabulary (pp. 7–8, PDF pp. 21–22):** `Off`, `SolidGreen`, `BlinkGreen`, `BlinkRed`, `SolidRed`. Note from the front-panel diagram (p. 8) that the 6 Utility keys (`[CARD]`, `[MIDI]`, `[DISK]`, `[MEMORY PROTECT]`, `[SETUP]`, `[OTHERS]`) and `[DEMO]` do **not** have dedicated panel LEDs; Utility screens preserve the underlying Play/Memory/Track LED state unless explicitly noted.

---

## 1. Chapter 6 — Card Operations (pp. 95–98)

### 1.0 Virtual RAM/ROM Card Hardware Model & Lockout Rules
- **(a) Manual Page Citation:** pp. 22, 95, 97–98, 109, 119 (PDF pp. 36, 109, 111–112, 123, 133)
- **(b) Hardware Card Types & Bank Geometry:**
  - **ROM Card:** Voice and performance data stored on ROM cards can **only be read** (cannot be formatted or written to, pp. 95, 97 Note).
  - **RAM Card (`MCD32` or `MCD64`):** Stores voices, performances, synthesizer setup data, or rhythm machine data.
    - `MCD32`: **1 bank** (`1`).
    - `MCD64`: **2 banks** (`1` or `2`); formatting, saving, and loading operate independently per bank (p. 95).
  - **Capacity per Bank (p. 97):**
    - Synthesizer data (`VF50SYN`) occupies **1 entire bank** of a card.
    - Rhythm data (`VF50RHY`) fits **two complete sets** (`A` and `B`, each containing 100 patterns, 8 songs, and rhythm setup data) in a single bank.
  - **DX11 Cartridges via ADP2 Adaptor (p. 22):** Read-only when inserted via the optional ADP2 adaptor; VF50 data cannot be saved to a cartridge.
- **(c) Lockouts, Write-Protect & Warnings:**
  - **RAM Card Write-Protect Slider vs. Utility `CARD` Memory Protect (pp. 95, 97, 109, 119):**
    - Each RAM card has a physical write-protect slider switch (`on` / `off`).
    - Utility `MEMORY PROTECT)` (`[MEMORY PROTECT]`, p. 109) also provides a software `CARD` protect (`off` / `on`).
    - If **either** the physical card write-protect slider is `on` **or** `MEMORY PROTECT)` `CARD` is `on`, attempting to Save displays `"Memory Protected"` and refuses execution. Unlike `INT` Store (`"Prot?"` bypass, p. 119), Card protect cannot be temporarily bypassed from the Store/Save prompt.
  - **Format & Media Lockouts on Save (p. 97 Note):** Cannot save to an unformatted bank (`unfmtd`), a bank formatted for the wrong data type (`VF50SYN` vs. `VF50RHY`), or a ROM card.
  - **Sequencer Exclusion (p. 97 Note):** Sequencer data **cannot** be saved to card (must be saved to floppy disk or transmitted via MIDI bulk dump).
  - **Internal Memory Protect on Card Load (p. 98 Note):** If Internal Memory Protect (`MEMORY PROTECT)` `INT = on`) is `on`, loading from card displays `"Memory Protected"` and is blocked — **with the explicit exception of `R.SEQ`**, which loads even when `INT` Memory Protect is `on`.
  - **Backup Battery Warning (p. 95 Note):** Card backup batteries last approximately 5 years; when low, selecting a card voice or performance displays `"Change card battery"`.
- **(d) Softkey / Data Rules:** Individual voices or performances cannot be saved via `UT CARD)` `>Save` (use `[STORE/COPY]` -> `C00–C99`, p. 119).
- **(e) LED States:** `[CARD]` bank LED (`SolidGreen` when Card voice/performance bank is active); `UT CARD)` itself has no dedicated LED and preserves underlying Play/Track LEDs.

---

### 1.1 `UT CARD)` Top-Level Screen
- **(a) Manual Page Citation:** pp. 95, 97–98 (PDF pp. 109, 111–112)
- **(b) Entry Button Sequence:** Press Utility `[CARD]` from any Play or Utility mode.
- **(c) Exact 40-Character LCD Templates:**
  - Unformatted RAM card / bank (p. 95):
```lcd
UT  CARD) ▶BANK Format                  
             1  unfmtd   >Form>Save>Load
```
  - Formatted bank example (`VF50SYN` on p. 97 / p. 98, or `VF50RHY`):
```lcd
UT  CARD) ▶BANK Format                  
             1  VF50SYN   >Form>Save>Load
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3` (`▶BANK`): Selects card bank (`1` on `MCD32`; `1` or `2` on `MCD64`) via `[-1]` / `[+1]`, numeric keys, or `DATA ENTRY`.
  - `Format` (read-only status, cols `17–22`): `unfmtd`, `VF50SYN`, or `VF50RHY`.
  - `F6` (`>Form`): Opens Card Format screen (`UT CARD) FORMAT`).
  - `F7` (`>Save`): Opens Card Save category screen (`UT CARD) SAVE`).
  - `F8` (`>Load`): Opens Card Load category screen (`UT CARD) LOAD`).
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - **No `>EXIT` on Top Level:** `UT CARD)` has no `>EXIT` softkey on Line 2; leave by pressing any Play Mode key (`[SINGLE]`, `[PERFORMANCE]`, `[RHYTHM]`, `[SEQ]`) or another Utility key. All Card sub-screens place `>EXIT` at `F8` to return to `UT CARD)`.

---

### 1.2 Card Format (`>Form`)
- **(a) Manual Page Citation:** pp. 95–96 (PDF pp. 109–110)
- **(b) Entry Button Sequence:** `[CARD]` -> select `▶BANK` (`1` or `2`) -> `F6` (`>Form`) -> select format (`F4`–`F5` or `F6`–`F7`) -> `[+1]`.
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  CARD)  FORMAT   Select one !        
               VF50SYN    VF50RHY    >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F4`–`F5` (`VF50SYN`): Formats selected bank to store synthesizer data (voices, performances, synthesizer setup data).
  - `F6`–`F7` (`VF50RHY`): Formats selected bank to store rhythm machine data (rhythm patterns, rhythm songs, rhythm setup data).
  - `F8` (`>EXIT`): Returns to `UT CARD)` top-level display.
  - **Confirmation Flow (pp. 95–96):** Pressing `VF50SYN` or `VF50RHY` prompts `"Are you sure?"`; pressing `[+1]` formats the selected bank.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - **Manual Preservation Note (p. 95):** *"Even if a card is formatted, the old data in the card will not be lost"* (existing data remains intact until overwritten by a subsequent Save).
  - **Lockouts:** Blocked on ROM cards and write-protected / `CARD`-protected RAM cards.

---

### 1.3 Card Save (`>Save`)
- **(a) Manual Page Citation:** pp. 96–97 (PDF pp. 110–111)
- **(b) Entry Button Sequence:** `[CARD]` -> select `▶BANK` (`1` or `2`) -> `F7` (`>Save`) -> select category (`Synth`, `Rhythm(toA)`, `Rhythm(toB)`) -> select data type -> `[+1]` at `"Are you sure?"`.
- **(c) Exact 40-Character LCD Templates:**
  - **Step 1 — Category Select (p. 97):**
```lcd
UT  CARD)  SAVE   Select one !          
Synth       Rhythm(toA) Rhythm(toB)>EXIT
```
  - **Step 2a — Synthesizer Data Save (`Synth`, p. 97):**
```lcd
UT  CARD)  SAVE Synth    select one !   
SynAL V&PF SetAL PCT PEFCT MCT SYS >EXIT
```
  - **Step 2b — Rhythm Machine Data Save (`Rhythm(toA)` / `Rhythm(toB)`, p. 97):**
```lcd
UT  CARD)  SAVE Rhythm(toA) Select one !
      RhyALL      R.SEQ      SETUP >EXIT
```
  *(Selecting `Rhythm(toB)` substitutes `Rhythm(toB)` on Line 1.)*
- **(d) Softkeys `F1`–`F8` & Exact Data Contents Table (pp. 96–97):**
  - **Step 1 Softkeys:** `F1`=`Synth`, `F3`–`F4`=`Rhythm(toA)` (bank section A), `F5`–`F7`=`Rhythm(toB)` (bank section B), `F8`=`>EXIT`.
  - **Step 2a (`Synth`) Softkeys:** `F1`=`SynAL` (table p. 96: `SynALL`), `F2`=`V&PF`, `F3`=`SetAL` (table p. 96: `setAL`), `F4`=`PCT`, `F5`=`PEFCT`, `F6`=`MCT`, `F7`=`SYS`, `F8`=`>EXIT`.

| Data Type | Manual Table Abbr. (p. 96) | LCD Softkey | Exact Saved/Loaded Contents (p. 96) |
|---|---|---|---|
| All data | `SynALL` | `SynAL` (`F1`) | Voice data (100 voices), Performance data (100 performances), Program change table data, Performance effect data, Micro tuning data (`"oct"` and `"full"` only), System data |
| Voice & performance data | `V&PF` | `V&PF` (`F2`) | Voice data (100 voices), Performance data (100 performances) |
| Setup All Data | `setAL` | `SetAL` (`F3`) | Program change table data, Performance effect data, Micro tuning data (`"oct"` and `"full"` only) |
| Program change table data | `PCT` | `PCT` (`F4`) | Program change table data |
| Performance effect data | `PEFCT` | `PEFCT` (`F5`) | Performance effect data (`delay1–4`, `pan1–4`, `chord1–4`) |
| Micro tuning data | `MCT` | `MCT` (`F6`) | Micro tuning data (`"oct"` and `"full"` only) |
| System data | `SYS` | `SYS` (`F7`) | All 20 items: Master tuning, Basic receive channel, Transmit channel, Program change mode, Control change mode, Aftertouch mode, Pitch bend mode, Note on/off, Device number, Memory protect (`INT/CRT`), Combine, Power on message, MIDI on/off, Local on/off, Data entry assign, Controller reset, Fixed velocity, Velocity curve, EG forced damp, Voice dump *(manual typo on p. 96 for Voice damp, p. 116)* |

  - **Step 2b (`Rhythm(toA)` / `Rhythm(toB)`) Softkeys:** `F2`–`F3`=`RhyALL` (table p. 97: `ALL`), `F4`–`F5`=`R.SEQ`, `F7`=`SETUP`, `F8`=`>EXIT`.

| Data Type | Manual Table Abbr. (p. 97) | LCD Softkey | Exact Saved/Loaded Contents (p. 97) |
|---|---|---|---|
| Rhythm all data | `ALL` | `RhyALL` (`F2`–`F3`) | Rhythm pattern data (100 patterns), Rhythm song data (8 songs), Rhythm setup data |
| Rhythm sequence data | `R.SEQ` | `R.SEQ` (`F4`–`F5`) | Rhythm pattern data (100 patterns), Rhythm song data (8 songs) |
| Rhythm setup data | `SETUP` | `SETUP` (`F7`) | Rhythm setup data |

  - **Confirmation:** Pressing any data-type softkey prompts `"Are you sure?"`; pressing `[+1]` overwrites the selected bank (or section A/B) on the card.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - **Lockouts (p. 97 Note):**
    - Physical RAM card write-protect slider `on` OR Utility `MEMORY PROTECT)` `CARD = on` -> displays `"Memory Protected"`, save refused.
    - Bank is `unfmtd`, formatted for wrong mode (`VF50RHY` when saving `Synth`, `VF50SYN` when saving `Rhythm`), or card is ROM -> save refused.
    - Sequencer data cannot be saved to card.
  - `F8` (`>EXIT`) returns to the previous `UT CARD)` screen.

---

### 1.4 Card Load (`>Load`)
- **(a) Manual Page Citation:** p. 98 (PDF p. 112)
- **(b) Entry Button Sequence:** `[CARD]` -> select `▶BANK` (`1` or `2`) -> `F8` (`>Load`) -> select category (`Synth`, `Rhythm(A)`, `Rhythm(B)`, `SEQ(YS)`) -> (for `Synth`/`Rhythm`, select sub-type) -> `[+1]` at `"Are you sure?"`.
- **(c) Exact 40-Character LCD Templates:**
  - **Step 1 — Category Select (p. 98):**
```lcd
UT  CARD)  LOAD    Select one !         
Synth   Rhythm(A) Rhythm(B) SEQ(YS)>EXIT
```
  - **Step 2a — Synthesizer Data Load (`Synth`, p. 98):**
```lcd
UT  CARD)  LOAD Synth    select one !   
SynAL V&PF SetAL PCT PEFCT MCT SYS >EXIT
```
  - **Step 2b — Rhythm Machine Data Load (`Rhythm(A)` / `Rhythm(B)`, p. 98):**
```lcd
UT  CARD)  LOAD Rhythm(A) select one !  
      RhyALL     R.SEQ    SETUP    >EXIT
```
  *(Selecting `Rhythm(B)` substitutes `Rhythm(B)` on Line 1.)*
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - **Step 1 Softkeys:** `F1`=`Synth`, `F2`–`F3`=`Rhythm(A)`, `F4`–`F5`=`Rhythm(B)`, `F6`–`F7`=`SEQ(YS)` (loads sequence data from a **YS200/B200** synthesizer card; skips Step 2 and proceeds straight to `"Are you sure?"` -> `[+1]`), `F8`=`>EXIT`.
  - **Step 2a / 2b Softkeys:** Identical data definitions to Section 1.3 (`SynAL`, `V&PF`, `SetAL`, `PCT`, `PEFCT`, `MCT`, `SYS` for `Synth`; `RhyALL`, `R.SEQ`, `SETUP` for `Rhythm(A)`/`Rhythm(B)`), plus `F8` (`>EXIT`).
  - **Confirmation:** `"Are you sure?"` -> `[+1]` overwrites VF50 internal memory with card data.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - **Internal Memory Protect Lockout & `R.SEQ` Exception (p. 98 Note):** If `MEMORY PROTECT)` `INT = on`, attempting to load displays `"Memory Protected"` and blocks loading — **except `R.SEQ`**, which is explicitly exempt and loads even when `INT` Memory Protect is `on`.
  - `F8` (`>EXIT`) returns to the previous `UT CARD)` screen.

---

## 2. Chapter 6 — MIDI Functions (pp. 99–104)

### 2.0 `UT MIDI)` Top-Level Menu
- **(a) Manual Page Citation:** pp. 99–103 (PDF pp. 113–117)
- **(b) Entry Button Sequence:** Press Utility `[MIDI]`.
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  MIDI)          Select one !         
>ChInfo>Switch >Cond>PgmCng   >Exclusive
```
- **(d) Softkeys `F1`–`F8` & Submenus:**
  - `F1`–`F2`: `>ChInfo` — Channel information (`Midi`, `BasicRch`, `KbdTch`, `LocaL`).
  - `F2`–`F3`: `>Switch` — MIDI Control Change, Aftertouch, Pitch Bend reception/transmission.
  - `F4`: `>Cond` — Note On/Off odd/even filter and Data Entry Assign.
  - `F5`–`F6`: `>PgmCng` — Program Change mode, Table Init, Table Edit.
  - `F7`–`F8`: `>Exclusive` — SysEx Device Number & Bulk Dump.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Top-level `UT MIDI)` has no `>EXIT` softkey; exit via any Play Mode key or another Utility key. All sub-screens map `F8` (`>EXIT`) back to `UT MIDI)` (and `UT DUMP)` exits back to `UT MIDI)` Device No.). Preserves current Play/Memory/Track LEDs.

---

### 2.1 Channel Information (`>ChInfo`)
- **(a) Manual Page Citation:** p. 99 (PDF p. 113)
- **(b) Entry Button Sequence:** `[MIDI]` -> `F1`–`F2` (`>ChInfo`).
  - **Performance Quick Transmit-Channel Shortcut (p. 99):** While in Performance mode, hold **`[-]`** and press **`[TR1]`–`[TR8]`** or **`[RHY]`** to set `KbdTch` (Transmit Channel) immediately to match that instrument's (or rhythm part's) receive channel.
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  MIDI) ▶Midi>BasicRch >KbdTch>LocaL  
            on   omn       1    on >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3` (`▶Midi`): **MIDI on/off** — `off`, `on`. Determines whether the VF50 transmits and receives MIDI channel/SysEx data with external devices. Realtime messages (`F8`, `FA`, `FB`, `FC`, `FE`) and System Common messages are **not** affected.
  - `F4`–`F5` (`>BasicRch`): **Basic receive channel** — `1–16`, `omni` (displayed `omn`). Sets the Single Play receive channel (`omn` receives on all 16 channels) and the `common` Program Change receive channel. *(In Performance Play mode, per-instrument receive channels are set in Performance Edit `TR4` `RECV CH`, p. 25.)*
  - `F6` (`>KbdTch`): **Transmit channel** — `1–16`. Sets the MIDI channel transmitted by the VF50 keyboard.
    - In **Single Play mode**, the VF50 internal tone generator sounds regardless of `KbdTch`.
    - In **Performance Play mode**, playing the keyboard sounds only instruments whose `TR4` `RECV CH` matches `KbdTch`.
  - `F7` (`>LocaL`): **Local on/off** — `off`, `on`. Connects (`on`) or disconnects (`off`) the physical keyboard from the internal tone generator. **Always defaults to `on` at power-on.**
  - `F8` (`>EXIT`): Returns to `UT MIDI)` top-level menu.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` returns to `UT MIDI)`.

---

### 2.2 Switch (`>Switch`)
- **(a) Manual Page Citation:** p. 100 (PDF p. 114)
- **(b) Entry Button Sequence:** `[MIDI]` -> `F2`–`F3` (`>Switch`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  MIDI)▶Cont.Change>A.Touch >PitchBend
           norm        G16     off >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F4` (`▶Cont.Change`): **MIDI Control Change** — `off`, `norm`, `G1`–`G16`.
  - `F5`–`F6` (`>A.Touch`): **MIDI Aftertouch** — `off`, `norm`, `G1`–`G16`.
  - `F7` (`>PitchBend`): **MIDI Pitch Bend** — `off`, `norm`, `G1`–`G16`.
  - `F8` (`>EXIT`): Returns to `UT MIDI)` top-level menu.
  - **Setting Semantics (identical across all three parameters, p. 100):**
    - `off`: Messages are ignored on reception (even from internal keyboard or sequencer) **and are not transmitted**.
    - `norm`: Messages are transmitted, and received messages affect instruments/Single Play only when the incoming MIDI channel matches the receive channel (Basic Rch in Single Play; `TR4` `RECV CH` in Performance Play).
    - `G1`–`G16` (**Global Channel**): Messages are transmitted and received normally on matching channels **plus** any message arriving on the designated Global channel (`G1`–`G16`) affects **all** instruments simultaneously regardless of their individual `RECV CH`.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` returns to `UT MIDI)`.

---

### 2.3 Condition (`>Cond`)
- **(a) Manual Page Citation:** p. 101 (PDF p. 115)
- **(b) Entry Button Sequence:** `[MIDI]` -> `F4` (`>Cond`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  MIDI) ▶Note on/off   >D.EntryAssign 
            all           0(undef)>EXIT 
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F5` (`▶Note on/off`): `all`, `odd`, `even`.
    - Filters MIDI Note On/Off messages arriving from MIDI IN or the internal sequencer (`all` = all notes; `odd` = odd MIDI note numbers only; `even` = even MIDI note numbers only) to chain two VF50s / TX81Z for 32-note polyphony. Notes played directly on the local keyboard are **not** filtered by this setting.
  - `F6`–`F7` (`>D.EntryAssign`): `0`–`31`.
    - Selects the MIDI Control Change number transmitted via MIDI OUT when moving the `DATA ENTRY` slider during Performance Play or Single Play (transmission only; does not directly drive internal receiving voice without external/loopback routing).
    - Displays the assigned controller abbreviation in parentheses per the VF50 controller table (p. 101 & IC712 firmware table):

| CC # | LCD Display | Meaning | CC # | LCD Display | Meaning |
|---|---|---|---|---|---|
| `0` | `0(undef)` | Undefined | `7` | `7(Vol. )` | Main Volume |
| `1` | `1(Mod.W)` | Modulation Wheel | `8` | `8(Bal. )` | Balance |
| `2` | `2(BC   )` | Breath Controller | `9` | `9(undef)` | Undefined |
| `3` | `3(undef)` | Undefined | `10` | `10(Pan  )` | Pan |
| `4` | `4(FC   )` | Foot Controller | `11` | `11(Exp  )` | Expression |
| `5` | `5(Por.T)` | Portamento Time | `12–31` | `12(undef)`..`31(undef)` | Undefined |
| `6` | `6(D.Ent)` | Data Entry | | | |

  - `F8` (`>EXIT`): Returns to `UT MIDI)` top-level menu.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` returns to `UT MIDI)`.

---

### 2.4 Program Change (`>PgmCng`) & Direct Program Change Transmit
- **(a) Manual Page Citation:** pp. 13, 16, 101–103 (PDF pp. 27, 30, 115–117)
- **(b) Entry Button Sequences:**
  - **Utility Submenu:** `[MIDI]` -> `F5`–`F6` (`>PgmCng`).
  - **Table Initialize:** `[MIDI]` -> `F5`–`F6` (`>PgmCng`) -> `F6` (`>Init`).
  - **Table Edit:** `[MIDI]` -> `F5`–`F6` (`>PgmCng`) -> `F7` (`>Edit`).
  - **Direct Program Change Transmit (pp. 13, 16, 103):** While already in Performance Play or Single Play (not locked by Compare), **press and hold `[PERFORMANCE]` or `[SINGLE]`**, enter 3 digits (`001`–`128`) on `[0]`–`[9]`, then **release** `[PERFORMANCE]`/`[SINGLE]` to transmit.
- **(c) Exact 40-Character LCD Templates:**
  - **1. Program Change Mode Select (p. 101):**
```lcd
UT  MIDI) ▶Pgm Change                   
           individual    >Init>Edit>EXIT
```
  - **2. Program Change Table Initialize (p. 102):**
```lcd
UT  MIDI)  Initialize pgm change table ?
                                   >EXIT
```
  - **3. Program Change Table Edit (p. 103):**
```lcd
UT  MIDI)  Edit pgm change table        
 ↓ pgm ↑ PGM   1=  I00(Strings 1 ) >EXIT
```
  - **4. Direct Program Change Transmit Overlay (pp. 13, 16, 103):**
```lcd
** Sending PC No ---                    
                                        
```
  *(Note: The p. 16 LCD diagram prints `** Sending PC No ---`, while prose on p. 13 prints `"Sending PC No.---"` and prose on p. 103 prints `"Sending Pc No.---"`. Line 2 retains or clears play status while waiting for digits `001`–`128`.)*
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - **Mode Select (`>PgmCng`, pp. 101–102):**
    - `F3`–`F5` (`▶Pgm Change`):
      - `off`: Incoming PC messages are ignored **and** outgoing PC messages are suppressed.
      - `common`: In both Single Play and Performance Play, a PC received on `BasicRch` selects the voice or performance mapped in the Program Change Table.
      - `individual`: In Performance Play, a PC received on an instrument's `RECV CH` selects the voice mapped in the Program Change Table for that instrument. *(Table entries pointing to a Performance are ignored in `individual` mode, p. 102 Note.)*
      - `direct`: In Performance Play, incoming PCs directly select the matching voice number on the corresponding instrument's `RECV CH` (bypasses the Program Change Table).
      - `TransFilter` (LCD: `TrnsFilter` in firmware): The VF50 sequencer records **two** PC messages per program change (1st = bank Internal/Card/Preset, 2nd = voice number). `TransFilter` strips the 1st message so only the 2nd PC transmits via MIDI OUT; reception behaves identically to `individual`.
    - **PC vs. SysEx Transmission Rule (p. 102):** If `Device No.` != `off` (and `Pgm Change` != `off`), changing a voice/performance transmits SysEx instead of a MIDI Program Change. If `Device No.` = `off` and `Pgm Change` != `off`, it transmits a MIDI Program Change on `KbdTch`.
    - `F6` (`>Init`): Opens Program Change Table Initialize.
    - `F7` (`>Edit`): Opens Program Change Table Edit.
    - `F8` (`>EXIT`): Returns to `UT MIDI)`.
  - **Table Initialize (`>Init`, pp. 102–103):**
    - Press `[+1]` -> displays `"Are you sure?"` -> press `[+1]` again to reset slots `1..100` to `I00..I99` and slots `101..128` to `P00..P27`.
    - `F8` (`>EXIT`): Returns to `>PgmCng`.
  - **Table Edit (`>Edit`, p. 103):**
    - `F1` (`↓`) / `F2` (`↑`): Decrements / increments Program Change slot `1`–`128` (left of `=`).
    - Right of `=`: Target memory (`I00–I99`, `C00–C99`, `P00–P99` voices or performances). Press `[INT]`, `[CARD]`, or `[PRESET]` for voices; **hold `[PERFORMANCE]` + press `[INT]`, `[CARD]`, or `[PRESET]`** for performances. Pressing `[+1]` past `99` advances to the next memory bank.
    - `F8` (`>EXIT`): Returns to `>PgmCng`.
  - **Direct PC Transmit (`Sending PC No ---`, p. 103):**
    - Valid entry range: `001`–`128` (3 digits). Transmits on `KbdTch` at the instant `[PERFORMANCE]` or `[SINGLE]` is released without changing the VF50's own active patch.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Direct PC Transmit is blocked while `[COMPARE]` mode-switch lock is active (p. 16).
  - In `>Edit`, `[INT]`/`[CARD]`/`[PRESET]` bank LEDs reflect the target voice/performance bank.

---

### 2.5 Exclusive (`>Exclusive` & `>Bulk`)
- **(a) Manual Page Citation:** pp. 103–104, 133, 136 (PDF pp. 117–118, 147, 150)
- **(b) Entry Button Sequence:**
  - Device Number: `[MIDI]` -> `F7`–`F8` (`>Exclusive`).
  - Bulk Dump: `[MIDI]` -> `F7`–`F8` (`>Exclusive`) -> `F7` (`>Bulk`).
- **(c) Exact 40-Character LCD Templates:**
  - **1. Device Number (`>Exclusive`, p. 103):**
```lcd
UT  MIDI)      ▶Device No.              
                    1         >Bulk>EXIT
```
  - **2. Bulk Dump Category Select (`>Bulk`, p. 103):**
```lcd
UT  DUMP) Bulk Dump      Select one !   
     VOICE PFM SETUP      SEQ RHY  >EXIT
```
  *(Note: The p. 103 LCD figure prints `RHY`, while p. 104 explanatory text labels this option `R.SEQ`.)*
- **(d) Softkeys `F1`–`F8` & Bulk Dump Sub-Ranges (pp. 103–104, 133, 136):**
  - **Device Number Screen:**
    - `F4`–`F5` (`▶Device No.`): `off`, `1–16`, `all`.
      - `off`: SysEx messages will not be transmitted (voice/perf selection sends Program Change instead if `Pgm Change` != `off`).
      - `1–16`: Transmits and receives SysEx on the specified channel (`n = dev - 1` in `F0 43 0n ...`).
      - `all`: Receives SysEx on **any** channel (`1–16`); transmits on channel `1`.
    - `F7` (`>Bulk`): Opens `UT DUMP) Bulk Dump`.
    - `F8` (`>EXIT`): Returns to `UT MIDI)`.
  - **Bulk Dump Screen (`UT DUMP)`):**
    - `F2` (`VOICE`): Select bank (`INT`, `PRESET`, or `CARD`) -> select range `00–24`, `25–49`, `50–74`, `75–99`, or `ALL` (`00–99`) -> `"Transmit ready?"` -> `[+1]`.
    - `F3` (`PFM`): Select bank (`INT`, `PRESET`, or `CARD`) -> select range `00–24`, `25–49`, `50–74`, `75–99`, or `ALL` (`00–99`) -> `"Transmit ready?"` -> `[+1]`.
    - `F4` (`SETUP`): Select `SetALL` (`PCT`, `P.EFCT` EFG1–4, `MCT` — excludes `SYS` per p. 133), `PCT`, `P.EFCT`, `MCT`, or `SYS` (`SYS3 -> SYS2 -> SYS`) -> `"Transmit ready?"` -> `[+1]`.
    - `F6` (`SEQ`): Select `SeqALL` (`SSONG -> NSEQ -> SETUP`), `SEQ` (sequence data), or `SETUP` (`SYSQ`) -> `"Transmit ready?"` -> `[+1]`.
    - `F7` (`RHY` / `R.SEQ`): Select `RhyALL` (`SYSR -> RINST -> RKAT1 -> RKAT2 -> RSEQ`), `R.SEQ` (rhythm sequence data), or `SETUP` (`SYSR + RINST + RKAT1,2`) -> `"Transmit ready?"` -> `[+1]`.
    - `F8` (`>EXIT`): Returns to `UT MIDI)` Device Number screen.
  - **Confirmation Flow:** After choosing the sub-range, LCD shows `"Transmit ready?"`; pressing `[+1]` transmits the SysEx bulk packet(s) (`"transmitting !"`).
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - **Playback Reception Lockout (p. 133 §4.5–4.6):** Incoming `SEQ` or `RHYTHM` SysEx bulk dumps are **not** received while Sequencer/Rhythm playback is active (`SEQ` bulk dumps also load only if the target song is empty).

---

## 3. Chapter 6 — Utility Setup Functions (pp. 110–116)

### 3.0 `UT SETUP)` Top-Level Menu
- **(a) Manual Page Citation:** pp. 110–116 (PDF pp. 124–130)
- **(b) Entry Button Sequence:** Press Utility `[SETUP]`.
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  SETUP)       Select one !           
>Tune>Comb>Ctrl>P.Efct  >Micro>Vel >Damp
```
- **(d) Softkeys `F1`–`F8` & Submenus:**
  - `F1`: `>Tune` — Master Tuning (`-64..+64`) & Synthesizer Volume (`0–99`).
  - `F2`: `>Comb` — Combine with FUNCTION (`off` / `on`).
  - `F3`: `>Ctrl` — Controller Reset (`hold` / `reset`).
  - `F4`: `>P.Efct` — Performance Effects (`delay1–4`, `pan1–4`, `chord1–4`).
  - `F5`–`F6`: `>Micro` — User Microtuning (`OCT.` Edit/Init & `FULL.` Edit/Init).
  - `F7`: `>Vel` — Fixed Velocity (`off`, `1–127`) & Velocity Curve (`0–7`).
  - `F8`: `>Damp` — EG Forced Damp (`veryslow`/`slow`/`medium`/`fast`) & Voice Damp (`off`/`on`).
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Top-level `UT SETUP)` has no `>EXIT` softkey; exit via any Play Mode key or another Utility key. All sub-screens map `F8` (`>EXIT`) to return to the previous Setup menu level. Preserves active Play/Memory/Track LEDs.

---

### 3.1 Master Tuning & Synthesizer Volume (`>Tune`)
- **(a) Manual Page Citation:** p. 110 (PDF p. 124)
- **(b) Entry Button Sequence:** `[SETUP]` -> `F1` (`>Tune`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  SETUP)▶Master Tuning >Synth Vol     
           +0 (440.0Hz)    90      >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F5` (`▶Master Tuning`): `-64` – `+64` (default `+0` = `440.0Hz`).
    - Step resolution: `1.5625` cents/step (`-64` = `-100` cents, one semitone down; `+64` = `+98.4` cents, ~one semitone up). Affects both Single Play and all instruments in Performance Play.
  - `F6`–`F7` (`>Synth Vol`): `0` – `99` (`0` = silent / minimum, `99` = maximum). Adjusts overall synthesizer output balance relative to the rhythm machine.
  - `F8` (`>EXIT`): Returns to `UT SETUP)`.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` exits to `UT SETUP)`.

---

### 3.2 Combine (`>Comb`)
- **(a) Manual Page Citation:** p. 110 (PDF p. 124)
- **(b) Entry Button Sequence:** `[SETUP]` -> `F2` (`>Comb`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  SETUP)  ▶Combine(with FUNCTION)     
                  on               >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F7` (`▶Combine(with FUNCTION)`): `off`, `on` (default `on`).
    - `on`: Switching voices or performances loads both Voice data and Function/Effect data normally.
    - `off`: Disconnects Function data from Voice data. Selecting a different voice or performance loads only the new Voice/Instrument data while **preserving** the previous Voice Function and Effect settings.
    - **Visual Indicator Rule (p. 110):** Whenever `Combine = off` and a different voice or performance is selected, the **first character of the voice or performance name is displayed in lower case** (e.g., `e.Piano` instead of `E.Piano`) to signify borrowed Function settings. *(Note: `UT OTHERS>` `>Init` still initializes Function parameters even when `Combine = off`, p. 117.)*
  - `F8` (`>EXIT`): Returns to `UT SETUP)`.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` exits to `UT SETUP)`.

---

### 3.3 Controller Reset (`>Ctrl`)
- **(a) Manual Page Citation:** pp. 110–111 (PDF pp. 124–125)
- **(b) Entry Button Sequence:** `[SETUP]` -> `F3` (`>Ctrl`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  SETUP) ▶Controller Reset            
                hold               >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F6` (`▶Controller Reset`): **`hold`**, **`reset`** *(exact manual setting labels — NOT `off`/`on`)*.
    - `hold`: Preserves current controller state (Modulation Wheel, Pitch Bend Wheel, Breath Controller, Foot Controller, etc.) across voice or performance changes.
    - `reset`: Zeroes/initializes controller state whenever a new voice or performance is selected; moving a physical controller slightly immediately applies the new position using the newly selected patch's controller sensitivity.
  - `F8` (`>EXIT`): Returns to `UT SETUP)`.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` exits to `UT SETUP)`.

---

### 3.4 Performance Effects (`>P.Efct` — `delay1–4`, `pan1–4`, `chord1–4`)
- **(a) Manual Page Citation:** pp. 29, 111–114, 120 (PDF pp. 43, 125–128, 134)
- **(b) Entry Button Sequence:** `[SETUP]` -> `F4` (`>P.Efct`) -> select `delay`, `pan`, or `chord` -> select memory `1–4`.
  - **Shared Performance Effect Rules (pp. 111–114, 120):**
    - Only **1 of the 12** performance effects (`delay1–4`, `pan1–4`, `chord1–4`) can be active at a time.
    - Can be auditioned in **Single Play mode** only while actively editing that performance effect screen.
    - Assigned to a Performance in Performance Edit via `[OTHERS]` (4th press -> `P.ED PFM EFCT>`, p. 29).
    - **Internal-Only Edit Lockout (p. 114 Note):** Modifying a performance effect parameter affects **Internal** performance data only; Preset or Card performances must be stored to Internal memory before edited performance effects will apply.
    - **Copy Shortcut (p. 120):** Pressing `[STORE/COPY]` inside any Performance Effect edit screen opens `PFM EFFECT COPY>` to copy between memories `1–4` of the same type.

#### 3.4a Category Select & Delay (`delay1`–`delay4`, pp. 111–112)
- **(c) Exact 40-Character LCD Templates:**
  - **Category Select (inferred from pp. 111–113 & IC712 firmware):**
```lcd
UT  SETUP)  EDIT PFM EFCT  Select one ! 
    delay       pan        chord   >EXIT
```
  - **Delay Slot Select (p. 111):**
```lcd
UT  SETUP)   ▶EDIT DELAY  Select one !  
delay1     delay2   delay3   delay4>EXIT
```
  - **Delay Edit Page 1 (`UT DELAY1)`–`UT DELAY4)`, p. 111):**
```lcd
UT  DELAY1) ▶DelayTime >PitchShift      
             1.28sec       +0 >NEXT>EXIT
```
  - **Delay Edit Page 2 (`UT DELAY1)`–`UT DELAY4)`, p. 111):**
```lcd
UT  DELAY1) ▶Feedback  >EffectLevel     
                6          90 >NEXT>EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges (pp. 111–112):**
  - Category Select: `F1`–`F2`=`delay`, `F3`–`F4`=`pan`, `F6`=`chord`, `F8`=`>EXIT`.
  - Slot Select: `F1`=`delay1`, `F3`=`delay2`, `F5`=`delay3`, `F7`=`delay4`, `F8`=`>EXIT`.
  - Page 1: `F3`–`F4` (`▶DelayTime`): `0.01` – `1.28` sec; `F5`–`F6` (`>PitchShift`): `-24` – `+24` semitones per echo (`0` = unison); `F7` (`>NEXT`) toggles to Page 2; `F8` (`>EXIT`) returns to Slot Select.
  - Page 2: `F3`–`F4` (`▶Feedback`): `0` – `7` (repeat count, also scaled by velocity & EffectLevel); `F5`–`F6` (`>EffectLevel`): `0` – `99` (`0` = silent, `99` = ~equal to dry note when carrier velocity sensitivity > 0); `F7` (`>NEXT`) toggles back to Page 1; `F8` (`>EXIT`) returns to Slot Select.
  - **Polyphony & Routing Limits (p. 111 Note):** Maximum **4** delayed notes concurrently (playing a 5th note cancels the 1st note's delay). In Performance Play mode, delay applies **only to Instrument 1**.

#### 3.4b Pan (`pan1`–`pan4`, pp. 112–113)
- **(c) Exact 40-Character LCD Templates:**
  - **Pan Slot Select (p. 112):**
```lcd
UT  SETUP)    ▶EDIT PAN   Select one !  
pan1        pan2      pan3     pan4>EXIT
```
  - **Pan Edit (`UT PAN1)`–`UT PAN4)`, p. 112):**
```lcd
UT  PAN1)  ▶Select >Direction>PanRange  
             LFO      L→R       99 >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges (pp. 112–113):**
  - Slot Select: `F1`=`pan1`, `F3`=`pan2`, `F5`=`pan3`, `F7`=`pan4`, `F8`=`>EXIT`.
  - `F3` (`▶Select`): `LFO` (modulated by voice LFO), `VEL` (key velocity determines stereo position), `NOTE` (key pitch determines stereo position).
  - `F4`–`F5` (`>Direction`): `L→R`, `L←R`.
    - `LFO` + `Sync=on`: `L→R` starts left, `L←R` starts right.
    - `VEL`: `L→R` pans soft->left / hard->right; `L←R` inverts.
    - `NOTE`: `L→R` pans low->left / high->right; `L←R` inverts.
  - `F6`–`F7` (`>PanRange`): `0` – `99` (`0` = off, `99` = maximum stereo sweep).
  - `F8` (`>EXIT`): Returns to Slot Select.
  - **Polyphony & Routing Limits (p. 112 Note):** Single Play max polyphony drops to **8 notes**. In Performance Play mode, pan applies **only to instruments whose `TR8` (`OUTPUT ASSIGN`) is `L` or `R`** (instruments set to `L + R` are unaffected). For `VEL`/`NOTE` with simultaneous chords, the first note of the lowest-numbered instrument sets position (p. 113 Note).

#### 3.4c Chord (`chord1`–`chord4`, pp. 113–114)
- **(c) Exact 40-Character LCD Templates:**
  - **Chord Slot Select (p. 113):**
```lcd
UT  SETUP)   ▶EDIT CHORD  Select one !  
chord1     chord2   chord3   chord4>EXIT
```
  - **Chord Edit (`UT CHORD1)`–`UT CHORD4)`, p. 113 & p. 114 example):**
```lcd
UT  CHORD1)   C3 is .....      >KBD     
 ← key →   ▶ C3/  E3/  * /  *   in >EXIT
```
```lcd
UT  CHORD1)   C3 is .....      >KBD     
 ← key →   ▶ C3/  E3/  G3/  *   in >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges (pp. 113–114):**
  - Slot Select: `F1`=`chord1`, `F3`=`chord2`, `F5`=`chord3`, `F7`=`chord4`, `F8`=`>EXIT`.
  - `F1` (`←`) / `F2` (`→`): Selects the trigger key (`C3`–`B3`, 12 pitch classes) shown on Line 1.
  - `F3`–`F6`: Selects and edits up to **4** chord notes (`C-2`–`G8` in any octave, or `*` = unused).
  - `F7` (`>KBD` / `in`): Arms keyboard chord capture — playing a chord on the keyboard directly fills the 4 note slots for the selected trigger key.
  - `F8` (`>EXIT`): Returns to Slot Select.
  - **Routing Limit (p. 113 Note):** In Performance Play mode, the chord effect applies **only to the lowest-numbered sounding instrument**.
- **(e) LED States, Lockouts & `>EXIT` Behavior (All `>P.Efct`):**
  - Preserves Play/Memory/Track LEDs; `F8` (`>EXIT`) unwinds one menu level (`Edit` -> `Slot Select` -> `EDIT PFM EFCT` -> `UT SETUP)`).

---

### 3.5 Microtuning (`>Micro` — User Octave & User Full Keyboard)
- **(a) Manual Page Citation:** pp. 29, 114–115 (PDF pp. 43, 128–129)
- **(b) Entry Button Sequence:** `[SETUP]` -> `F5`–`F6` (`>Micro`) -> `OCT. >Edit` / `OCT. >Init` / `FULL >Edit` / `FULL >Init`. *(Note: The manual prose on pp. 114–115 contains a printing typo reading `"Press [STEP]"` instead of `"Press [SETUP]"`, corrected on p. 115 Initialize.)*
- **(c) Exact 40-Character LCD Templates:**
  - **1. Microtuning Submenu (verified from IC712 firmware & pp. 114–115):**
```lcd
UT  MICRO TUNE) OCT.      FULL          
               >Edit>Init>Edit>Init>EXIT
```
  - **2. Octave Edit (`OCT. Edit`, p. 114):**
```lcd
UT  MICRO) OCT.    C3      key set      
▶CRS >FINE   C3 +10( 3018)  ←    → >EXIT
```
  - **3. Octave Initialize (`OCT. Init`, p. 115) & Full Keyboard Initialize (`FULL Init`, p. 115):**
```lcd
UT  MICRO) INIT    ▶Table      >key     
>GO             2:Pure(major)   C  >EXIT
```
  - **4. Full Keyboard Edit (`FULL Edit`, p. 115):**
```lcd
UT  MICRO) FULL.  C-2      key set      
▶CRS >FINE  C#-1 +10(   10) ←    → >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges (pp. 29, 114–115):**
  - **Submenu:** `F4`=`OCT. >Edit`, `F5`=`OCT. >Init`, `F6`=`FULL >Edit`, `F7`=`FULL >Init`, `F8`=`>EXIT`.
  - **Pitch Step Units (pp. 114–115):**
    - `1` fine step = **1/64 semitone = `1.5625` cents**.
    - Parenthesized integer `(3018)` / `(10)` = total pitch in `1/64`-semitone steps starting from `C#-1` (`0` = `C#-1`; `C3` = `47 * 64 = 3008`, so `C3 +10` = `3018`).
  - **Octave Edit (`OCT. Edit`, 12 keys `C3–B3`, replicated to all octaves):**
    - `F1` (`▶CRS`): Selects coarse note name (`C-2`–`G8`) for the target key.
    - `F2` (`>FINE`): Selects fine offset (`-63`..`+63` steps of `1/64` semitone).
    - `F6` (`←`) / `F7` (`→`) (or playing a key `C3–B3` on the keyboard): Steps target key `C3–B3` shown on Line 1.
    - `F8` (`>EXIT`): Returns to Microtuning Submenu.
  - **Full Keyboard Edit (`FULL Edit`, 128 MIDI notes `C-2–G8`):**
    - Identical softkeys (`F1`=`▶CRS`, `F2`=`>FINE`, `F6`=`←`, `F7`=`→`, `F8`=`>EXIT`), covering all MIDI notes `C-2`–`G8` independently.
  - **Octave / Full Initialize (`OCT. Init` / `FULL Init`, pp. 29, 115):**
    - `F1` (`>GO`, referred to as `"yes"` in p. 115 prose): Press `F1` then **`[+1]`** to copy the chosen Preset Microtuning (`1–11`, p. 29) into the User Octave or User Full Keyboard buffer.
    - `F4`–`F6` (`▶Table`): `1:Equal`, `2:Pure(major)`, `3:Pure(minor)`, `4:Mean tone`, `5:Pythagorean`, `6:Werckmeister`, `7:Kirnberger`, `8:Vallotti&young`, `9:1/4 Shift eql`, `10:1/4 Tone`, `11:1/8 Tone`.
    - `F7` (`>key`): Tonic key (`C`–`B`; `A`–`G#` for `3:Pure(minor)`); active for tables `2–5`.
    - `F8` (`>EXIT`): Returns to Microtuning Submenu.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - User Octave and User Full tunings are battery-backed across power cycles (p. 114 Note); active in Performance Play via Edit `[OTHERS]` (3rd press, p. 29) and in Single Play while inside Microtuning Edit.

---

### 3.6 Velocity (`>Vel`)
- **(a) Manual Page Citation:** p. 115 (PDF p. 129)
- **(b) Entry Button Sequence:** `[SETUP]` -> `F7` (`>Vel`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  SETUP)▶FixedVelocity >VelCurve      
               127       0(norm)   >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F5` (`▶FixedVelocity`): `off`, `1` – `127` (default `off`). When `1–127`, every key press transmits and sounds at the fixed velocity regardless of playing force.
  - `F6`–`F7` (`>VelCurve`): `0` – `7` (power-on default `0(norm)`):
    - `0(norm)`: Normal linear velocity curve.
    - `1(curve1)` – `5(curve5)`: Progressively higher output velocity for soft playing (`5` = most sensitive).
    - `6(flat)`: Nearly flat response capped at **maximum velocity `80`**.
    - `7(revers)`: Inverse velocity (stronger playing produces lower velocity).
  - `F8` (`>EXIT`): Returns to `UT SETUP)`.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` exits to `UT SETUP)`.

---

### 3.7 Damp (`>Damp` — EG Forced Damp & Voice Damp)
- **(a) Manual Page Citation:** p. 116 (PDF p. 130)
- **(b) Entry Button Sequence:** `[SETUP]` -> `F8` (`>Damp`).
- **(c) Exact 40-Character LCD Template:**
```lcd
UT  SETUP) ▶EG Damp  >Voice Damp        
             fast         on       >EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - `F3`–`F4` (`▶EG Damp`): **`veryslow`**, **`slow`**, **`medium`**, **`fast`** *(4 speed levels — NOT `off`/`on`)*.
    - Controls how quickly the oldest sounding note's Envelope Generator is damped when polyphony overflows (>16 notes in Single Play, or >`Max Notes` in Performance Play). `fast` minimizes attack delay on percussive sounds; `veryslow` minimizes click noise.
  - `F5`–`F7` (`>Voice Damp`): **`off`**, **`on`** *(manual screen name is `Voice Damp`, NOT `Volume Damp`)*.
    - `on`: Switching voices in Single Play immediately silences currently sounding notes.
    - `off`: Notes already sounding continue to sustain through a Single Play voice change while the new voice applies to subsequent notes (however, LFO data updates immediately upon selecting the new voice).
  - `F8` (`>EXIT`): Returns to `UT SETUP)`.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Preserves active Play/Memory/Track LEDs; `F8` exits to `UT SETUP)`.

---

## 4. Chapter 1 — Demo Playback, Power-On Message & Shortcuts (pp. 11, 13–15, 22)

### 4.1 `DEMO` Button & `UT DEMO)` Sub-Modes
- **(a) Manual Page Citation:** pp. 7, 14–15 (PDF pp. 21, 28–29)
- **(b) Entry Button Sequence:** Press `[DEMO]` -> select mode (`>ROM Play`, `>Disk Play`, `>ChainPlay`, `>V. Edit`) -> confirm `[Yes]` (`F8`).
- **(c) Exact 40-Character LCD Templates:**
  - **1. Demo Top-Level Menu (p. 14):**
```lcd
UT  DEMO)     Select one !              
>ROM Play >Disk Play >ChainPlay >V. Edit
```
  - **2. Destructive Data-Change Warning Prompt (p. 14):**
```lcd
UT  DEMO)  ROM Play     Are you sure?   
All data will be changed !   [No]  [Yes]
```
  *(Substitutes `Disk Play`, `ChainPlay`, or `V. Edit` on Line 1 according to selection.)*
  - **3. Busy Loading Indicator (p. 14):** Brief `"*** BUSY ***"` display while demo data is loaded into internal memory.
  - **4. Song Select & Transport (`ROM Play` / `Disk Play`, p. 14):**
```lcd
UT  DEMO)  Select song & push  START    
▶Song 1 : Victory !     >START>STOP>EXIT
```
  - Disk Play example (p. 14):
```lcd
UT  DEMO)  Select song & push  START    
▶Song 1 : Fusion        >START>STOP>EXIT
```
- **(d) Softkeys `F1`–`F8` & Parameter/Transport Behavior:**
  - **Top-Level Menu:** `F1`–`F2`=`>ROM Play` (internal ROM demo songs), `F3`–`F4`=`>Disk Play` (demo or `ALL` files at beginning of floppy disk), `F5`–`F6`=`>ChainPlay` (plays all ROM/disk demo songs successively starting from Song 1), `F7`–`F8`=`>V. Edit` (automated Voice Edit demonstration).
  - **Confirmation Prompt:** `F7` (`[No]`) cancels back to `UT DEMO)`; `F8` (`[Yes]`) loads demo data.
    - **Destructive Memory Rule (p. 14):** Loading any demo replaces current VF50 data; `Disk Play`, `ChainPlay`, and `V.Edit` also overwrite **internal voice and performance memory**.
  - **Song Select & Transport (pp. 14–15):**
    - `[-1]` / `[+1]`: Selects demo song (`Song 1..3` in ROM: `Victory !`, `Route 50`, `MoonLite`).
    - `F5`–`F6` (`>START`) or `[PLAY ▶]`: Starts or resumes demo playback.
    - `F7` (`>STOP`) or `[STOP ■]`: Stops playback (retains position; pressing `>START` / `[PLAY ▶]` resumes).
    - **Return to Beginning of Song (pp. 14–15 vs. p. 7):** Manual pp. 14–15 prose prints `[■]` + `[▶]` (`[STOP]` + `[PLAY]`) to return to song top, whereas the front-panel diagram on p. 7 labels `TOP` between `[BWD <<]` and `[STOP ■]` (`[STOP]` + `[BWD]`). Classic Mode should support `[STOP]+[BWD]` (matching Ch. 5 p. 78) and may also alias `[STOP]+[PLAY]` inside `UT DEMO)`.
    - `F8` (`>EXIT`): Stops demo playback (if active) and exits `UT DEMO)` Song Select.
  - **Voice Edit Demo (`>V. Edit`, p. 15):** After `[Yes]` (`F8`), runs automated Voice Edit sequence; moving the **`DATA ENTRY` slider** controls the speed of the Voice Edit demo.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - `[DEMO]` button has no panel LED; Sequencer `[PLAY ▶]` LED lights (`SolidGreen`) while demo song playback is active.

---

### 4.2 Power-On Greeting Message & Edit Shortcut (`UT EDIT MESSAGE)`)
- **(a) Manual Page Citation:** pp. 11, 17, 22, 96 (PDF pp. 25, 31, 36, 110)
- **(b) Entry Button Sequence:**
  - **Boot Display:** Automatic at power-on for **~2 seconds**, then transitions to the previously active Play screen (e.g., `PFI00 "V"Lead 1 ...`, p. 11).
  - **Edit Greeting Message:** **Hold `[DEMO]` and press `[STORE/COPY]`**.
- **(c) Exact 40-Character LCD Templates:**
  - **1. Power-On Greeting Splash (~2 seconds, pp. 11, 22):**
```lcd
**** HARDWARE Digital Synthesizer VF50 ****
>>>>       Nice to meet you !!      <<<<
```
  - **2. Edit Message Screen (`UT EDIT MESSAGE)`, p. 22):**
```lcd
UT EDIT MESSAGE)                 ←    → 
>>>>       Nice to meet you !!      <<<<
```
- **(d) Softkeys `F1`–`F8` & Character Entry Rules (pp. 17, 22, 96):**
  - **Boot Splash:** `F1`–`F8` inactive during the 2-second splash.
  - **`UT EDIT MESSAGE)`:**
    - `F7` (`←`) / `F8` (`→`): Moves the underline edit cursor left/right across all **40 characters** of Line 2.
    - **Character Input (p. 17):** Multi-tap `[0]`–`[9]` (`[-]` `LETTER` toggles upper/lower case; `[9]` enters space), OR scroll full ASCII table via `DATA ENTRY` slider or `[-1]` / `[+1]`.
    - **Persistence (pp. 22, 96):** Stored in `SYS` system data (`Power on message`) and displayed on subsequent power-on boots.
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Exit `UT EDIT MESSAGE)` by pressing any Play Mode or Utility key; message updates live in system memory.

---

### 4.3 Bank Hold & Direct Program Change Shortcuts
- **(a) Manual Page Citation:** pp. 13, 16 (PDF pp. 27, 30)
- **(b) Entry Button Sequences:**
  - **Enable Bank Hold:** While holding **`[-]`**, press **`[7]`**.
  - **Disable Bank Hold:** While holding **`[-]`**, press **`[8]`**.
  - **Direct PC Transmit (cross-ref §2.4d):** While already in Performance Play or Single Play, hold **`[PERFORMANCE]`** or **`[SINGLE]`** -> enter `001`–`128` -> release.
- **(c) LCD Behavior (p. 13):**
  - Bank Hold alters memory selection in Play/Select screens without a dedicated modal LCD screen:
    - With Bank Hold **OFF** (default): Selecting memory `00–99` requires entering 2 numeric digits (pressing `[INT]`/`[PRESET]`/`[CARD]` shows `I??`/`P??`/`C??`).
    - With Bank Hold **ON**: Pressing a single numeric digit `0`–`9` immediately selects that units digit within the **currently active tens group** (e.g., if `23` is active, pressing `[9]` immediately selects `29`, and pressing `[4]` immediately selects `24`).
- **(d) Softkeys `F1`–`F8` & Parameter Ranges:**
  - Not applicable (direct front-panel keypad modifier shortcut).
- **(e) LED States, Lockouts & `>EXIT` Behavior:**
  - Bank LED (`[INT]`, `[PRESET]`, `[CARD]`) and Play Mode LED (`[SINGLE]`, `[PERFORMANCE]`) update immediately to reflect the newly selected memory.
