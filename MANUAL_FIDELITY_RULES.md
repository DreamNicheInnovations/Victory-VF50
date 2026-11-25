\# DUAL-MODE ARCHITECTURE \& VF50 MANUAL FIDELITY CONTRACT

Source of Truth for Classic Mode: `Docs/VF50 Operating Reference Manual`



1\. PRESERVE MODERN MODE (ZERO REGRESSION):

&#x20;  - Snapshot and preserve the existing plugin layout, PG-style programmer drawer, `EXPORT .MID`, `ROM SETUP`, and folder/floppy quick browser strictly inside `MODE == MODERN`.

&#x20;  - Do not remove, restyle, or restrict Modern Mode features when implementing Classic Mode.

&#x20;  - Fix existing Modern bugs (such as ASCII mojibake and popup menu scaling) without altering its layout structure.



2\. ZERO IMPROVISATION IN CLASSIC MODE:

&#x20;  - When `MODE == CLASSIC`, route all faceplate buttons, softkeys (`F1`–`F8`), LEDs, and the 40x2 LCD through a dedicated `VF50ClassicController` / `VF50ClassicStateMachine` that obeys the VF50 Operating Reference Manual verbatim.

&#x20;  - NEVER spawn OS popup menus, dropdowns, or custom dialog boxes inside Classic Mode's faceplate workflows (especially `DISK`, `SEQ`, `RHYTHM`, `VOICE`, `PERFORMANCE`, and `UTILITY`).

&#x20;  - Only the top host bar (`MODE` switch, `SCALE`, and virtual floppy insert/eject slot) exists outside the 1989 hardware simulation.



3\. SHARED AUDIO ENGINE, ISOLATED UI ROUTING:

&#x20;  - Both modes share the same FM/PCM/DSP engine, internal/preset/card memory banks, and sequencer audio transport.

&#x20;  - Switching between Modern and Classic must be click-free and must NEVER stop audio or sequencer playback.



4\. CITE BEFORE CODING \& TEST EVERY WORKFLOW:

&#x20;  - Every Classic Mode state, 40x2 LCD string, softkey mapping, and LED state must cite the exact manual page number before code is written.

&#x20;  - Every Classic Mode chapter implementation must include headless walkthrough tests in `VF50\_HeadlessTest.cpp` verifying exact button sequences, 40-character LCD lines, LED states (`Off`, `SolidGreen`, `BlinkGreen`, `BlinkRed`), and MIDI/audio output.

