#pragma once

#include <array>
#include <cstdint>

namespace vf50::classic {

// Canonical 4-state (+ Effect Bypass SolidRed) hardware LED vocabulary from Docs/VF50_CLASSIC_SPEC_PHASE1.md & PHASE2.md
enum class LedState : uint8_t {
    Off        = 0,
    SolidGreen = 1,
    BlinkGreen = 2,
    BlinkRed   = 3,
    SolidRed   = 4
};

// Complete front-panel LED snapshot in 1989 Classic fidelity mode
struct ClassicLedSnapshot {
    LedState performance  = LedState::Off;
    LedState single       = LedState::Off;
    LedState seq          = LedState::Off;
    LedState rhythm       = LedState::Off;
    LedState memInt       = LedState::Off;
    LedState memCard      = LedState::Off;
    LedState memPreset    = LedState::Off;
    LedState opSelect     = LedState::Off; // Above [RECORD]
    LedState demo         = LedState::Off;
    LedState compare      = LedState::Off;
    LedState effectBypass = LedState::Off; // Red LED (Off / BlinkRed)

    std::array<LedState, 4> opOnOff{ {
        LedState::SolidGreen, LedState::SolidGreen, LedState::SolidGreen, LedState::SolidGreen
    } };
    std::array<LedState, 8> tr{ {
        LedState::Off, LedState::Off, LedState::Off, LedState::Off,
        LedState::Off, LedState::Off, LedState::Off, LedState::Off
    } };
    LedState rhy = LedState::Off; // RHY LED inside 2x6 EDIT matrix
};

// Authentic VF50 Classic Mode screen states (Chapters 1, 5, and 6)
enum class ClassicScreen : uint8_t {
    PowerOnGreeting = 0, // Ch. 1.1 (p. 11)
    PlaySingle,          // Ch. 1.2 (pp. 12-13, 18)
    PlayPerformance,     // Ch. 1.3-1.4 (pp. 11-12, 19-20)
    DelegatedEdit,       // Voice/Perf/Effect/Rhythm/StoreConfirm edit screens in VF50MainCanvas

    // Chapter 5: Sequencer (pp. 78-94)
    SeqPlaySong,         // Ch. 2.1-2.3 (pp. 82, 87)
    SeqSongSetupPrompt,  // Ch. 2.4 "with SETUP" vs "Song only" (pp. 82, 84, 87)
    SeqStoreSetupConfirm,// Ch. 2.4 "STORE) ... Store SETUP data to song N ?" (p. 94)
    SeqJobSelect,        // Ch. 2.5 "SEQUENCER JOB SELECT)" (p. 82)
    SeqJobSetup,         // Ch. 2.6 "SETUP)>Rch>A.T >Vel >Click >Sync" (pp. 88-89)
    SeqJobTransmitCh,    // Ch. 2.7 "TRANSMIT CHANNEL)" Single vs Dual R/T (p. 90)
    SeqJobSong,          // Ch. 2.8 "SONG) NAME / Song Clear" (p. 90)
    SeqJobEditMenu,      // Ch. 2.9 "EDIT TRACK)" (p. 91)
    SeqJobEditSub,       // Ch. 2.9 7 track/measure edit sub-screens (pp. 91-93)
    SeqJobRecMode,       // Ch. 2.10 "RECORD MODE)" (pp. 82, 88, 94)
    SeqRecStandby,       // Ch. 2.10 "RECORD)" / "STEP REC)" / "PUNCH REC)" (pp. 83-88)
    SeqStepBarGraph,     // Ch. 2.10 "M001" 32nd-note bar graph & GateTime select (pp. 85-86)

    // Chapter 6: Disk LCD Workflow & Memory Protect (pp. 105-109)
    DiskRoot,            // Ch. 3.1 "UT DISK)" (pp. 105-109)
    DiskTypeSelect,      // Ch. 3.2-3.5 "SAVE/LOAD/DELETE/RENAME  Select one !"
    DiskFileAction,      // Ch. 3.2-3.5 file select/name/confirm screen
    DiskMdrMenu,         // Ch. 3.6 "UT DISK) MDR" (p. 107)
    DiskMdrIntTime,      // Ch. 3.6 "Interval Time = N x 100 ms" (p. 107)
    DiskDirectory,       // Ch. 3.7 "UT DISK) DIRECTORY" (p. 108)
    DiskJobMenu,         // Ch. 3.8 "UT DISK) Insert DISK and select one !" (pp. 108-109)
    DiskStatus,          // Ch. 3.8 "UT DISK) STATUS" (p. 109)
    DiskBackupPrompt,    // Ch. 3.8 ">BackUp" multi-step confirmation prompt (pp. 108-109)
    UtilMemoryProtect,   // Ch. 3.9 "UT MEMORY PROTECT)" (p. 109)

    // Chapter 2: Performance Edit (pp. 23-35)
    PerfEditAssignMode,  // TR1: AssignMode + 10-char Perf Name (p. 24)
    PerfEditNotes,       // TR2: MAX NOTES> (normal/alt) vs RESERVED NOTES> (DVA) (pp. 24-25)
    PerfEditVoiceNo,     // TR3: VOICE NO> + INT/CARD bank restrictions (p. 25)
    PerfEditRecvCh,      // TR4: RECV CH> (1..16, omn) (p. 25)
    PerfEditLimitLow,    // RHY: LIMIT/LOW> (C-2..G8) (p. 25)
    PerfEditLimitHigh,   // LIMIT/HIGH> (C-2..G8) (p. 26)
    PerfEditDetune,      // TR5: INST DETUNE> (-7..+7) (p. 26)
    PerfEditNoteShift,   // TR6: NOTE SHIFT> (-24..+24) (p. 27)
    PerfEditVolume,      // TR7: VOLUME> (0..99 + bar meters) (p. 27)
    PerfEditOutAssign,   // TR8: OUT ASSIGN> (off, L, R, L+R) (p. 27)
    PerfEditOthers,      // OTHERS 4-press cycle: LFO, MicroTune On/Off, MicroTune Table/Key, PerfEfct (pp. 28-30)
    PerfEditEffect,      // EFFECT 4-press cycle + 32 DSP effects (pp. 30-35)

    // Chapter 3: Voice Edit (pp. 36-56)
    VoiceEditQuick,      // TR1: QUICK> ATTACK/RELEAS/VOLUME/BRIL 4-press cycle (pp. 39-40)
    VoiceEditAlgorithm,  // TR2: ALG> 1-8 + diagram + Feedback (pp. 40-41)
    VoiceEditLfo,        // TR3: LFO> Wave, Speed, Delay, Sync, PMD, AMD (pp. 41-42)
    VoiceEditSens,       // TR4: SENS> PMS, AMS, AME, EBS, KVS (pp. 43-44)
    VoiceEditOsc,        // RHY: OSCILLATOR ratio vs fix dual LCD layouts (pp. 44-45)
    VoiceEditEg,         // LIMIT/HIGH: EG> AR, D1R, D1L, D2R, RR, SHFT (pp. 45-47)
    VoiceEditPeg,        // TR5: PEG> PR1..3, PL1..3 (p. 47)
    VoiceEditOutLevel,   // TR6: OUT LEVEL> OP1..OP4 (p. 48)
    VoiceEditScaling,    // TR7: SCALING> RATE <-> LEVEL 2-press cycle (p. 48)
    VoiceEditTranspose,  // TR8: TRANSPOSE> Middle C + KBD (p. 49)
    VoiceEditFuncRoot,   // FUNCTION> root selector (pp. 49-54)
    VoiceEditFuncPbEtc,  // F1: P.BEND> (p. 49)
    VoiceEditFuncPort,   // F2: PORTA> (p. 50)
    VoiceEditFuncFc,     // F3: FC> (pp. 50-51)
    VoiceEditFuncMw,     // F4: MW> (p. 51)
    VoiceEditFuncBc,     // F5: BC> (p. 52)
    VoiceEditFuncAt,     // F6: AT> (pp. 53-54)
    VoiceEditFuncRevr,   // F7: REVERB RATE> (p. 54)
    VoiceEditFuncName,   // F8: VOICE NAME> (p. 54)
    VoiceEditEffect,     // EFFECT 3-press cycle (pp. 54-56)

    // Chapter 6: Edit Buffer Utilities, Store, Copy (pp. 117-120)
    UtilOthersVoice,            // UT OTHERS> VOICE (>Init, >Recall, >PresetLoad) (pp. 117-118)
    UtilOthersPerf,             // UT OTHERS> PFM (>Init, >Recall, >VoiceEdit, >PresetLoad) (pp. 117-118)
    UtilOthersPerfInit,         // UT OTHERS> PFM INIT (SNGL..SEQ8) (p. 117)
    UtilOthersPresetLoad,       // UT OTHERS> PRESET LOAD (ALL, PEFCT, MCT) (p. 118)
    UtilOthersMultiVoiceSelect, // UT OTHERS> VOICE EDIT which voice ? (p. 118)
    StoreVoice,                 // SINGLE MODE> Mem Store (p. 119)
    StorePerf,                  // PERFORMANCE MODE> PFM Store (p. 119)
    StoreMultiVoice,            // UT OTHERS> STORE VOICE (p. 119)
    CopyEffect,                 // EFFECT COPY> (p. 120)
    CopyPerfEffect,             // PFM EFFECT COPY> (p. 120)
    CopyEg,                     // EG COPY> (p. 120)

    // Chapter 4: Rhythm Machine (pp. 57-77)
    RhythmPtnPlay,              // PTN PLAY> (pp. 58-59)
    RhythmSongPlay,             // SONG PLAY) (p. 71)
    RhythmPtnRecSetup,          // P.REC> setup (pp. 60-62)
    RhythmPtnRecReal,           // P.REC> Realtime live recording (pp. 61-62)
    RhythmPtnRecStep,           // Bar1..4 Step Recording Bar Graph (pp. 62-65)
    RhythmPtnRecStore,          // P.REC> Store Pattern (pp. 62-63)
    RhythmPtnJobSelect,         // PATTERN JOB SELECT) (p. 66)
    RhythmPtnJobCopy,           // PTN COPY> (p. 66)
    RhythmPtnJobClear,          // PTN CLEAR> (p. 67)
    RhythmSetup,                // SETUP> shared (pp. 67-68)
    RhythmInstSet,              // INST SET> shared (pp. 68-69)
    RhythmKeyAssign,            // RHYTHM ASSIGN> shared (pp. 69-70)
    RhythmSongJobSelect,        // SONG JOB SELECT) (pp. 74-76)
    RhythmSongEdit,             // SONG EDIT) (pp. 72-74)
    RhythmSongEditJob,          // SONG ED JOB) (pp. 75-76)
    RhythmSongJobCopy,          // SONG COPY> (p. 76)
    RhythmSongJobClear,         // SONG CLEAR> (p. 77)
    RhythmSongJobSearch,        // SEARCH> mark jump (p. 77)

    // Chapter 6: Card Operations (pp. 95-98)
    CardRoot,                   // UT CARD) (p. 95)
    CardFormat,                 // >Form -> VF50SYN / VF50RHY (p. 95)
    CardSaveCategory,           // >Save -> Synth / Rhythm(toA) / Rhythm(toB) (pp. 96-97)
    CardSaveSub,                // >Save sub-selection (p. 97)
    CardLoadCategory,           // >Load -> Synth / Rhythm(A) / Rhythm(B) / SEQ(YS) (p. 98)
    CardLoadSub,                // >Load sub-selection (p. 98)

    // Chapter 6: MIDI Functions (pp. 99-104)
    MidiRoot,                   // UT MIDI) (p. 99)
    MidiChInfo,                 // >ChInfo (p. 99)
    MidiSwitch,                 // >Switch (p. 100)
    MidiCond,                   // >Cond (p. 101)
    MidiPgmCng,                 // >PgmCng (p. 101)
    MidiPgmInit,                // >Init table (p. 102)
    MidiPgmEdit,                // >Edit table (p. 102)
    MidiExclusive,              // >Exclusive (p. 103)
    MidiBulkRoot,               // UT DUMP) >Bulk category (p. 104)
    MidiBulkSub,                // UT DUMP) >Bulk sub-selector (p. 104)

    // Chapter 6: Utility Setup Functions (pp. 110-116)
    SetupRoot,                  // UT SETUP) (p. 110)
    SetupTune,                  // >Tune (p. 110)
    SetupCombine,               // >Comb (p. 111)
    SetupCtrlReset,             // >Ctrl (p. 111)
    SetupPerfEfctCat,           // >P.Efct category (p. 112)
    SetupPerfEfctSlot,          // >P.Efct 1-4 slot (p. 112)
    SetupDelayEdit,             // Delay 1-4 edit (p. 112)
    SetupPanEdit,               // Pan 1-4 edit (p. 113)
    SetupChordEdit,             // Chord 1-4 edit (p. 113)
    SetupMicroRoot,             // >Micro root (p. 114)
    SetupMicroOctEdit,          // OCT.Edit (p. 114)
    SetupMicroOctInit,          // OCT.Init (p. 114)
    SetupMicroFullEdit,         // FULL.Edit (p. 115)
    SetupMicroFullInit,         // FULL.Init (p. 115)
    SetupVel,                   // >Vel (p. 115)
    SetupDamp,                  // >Damp (p. 116)

    // Chapter 1: Demo & Power-On Greeting (pp. 11, 13-15)
    DemoRoot,                   // UT DEMO) (p. 13)
    DemoConfirm,                // Demo ROM/Disk/Chain load confirmation (p. 13)
    DemoPlay,                   // Demo Song Play (p. 14)
    DemoVoiceEdit,              // Demo >V. Edit (p. 15)
    UtilEditMessage             // UT EDIT MESSAGE) (p. 11)
};

enum class DiskOperation : uint8_t {
    Save   = 0,
    Load   = 1,
    Delete = 2,
    Rename = 3
};

enum class DiskDataType : uint8_t {
    All  = 0,
    Syn  = 1,
    Seq  = 2,
    RSeq = 3,
    Card = 4
};

enum class SeqEditSubKind : uint8_t {
    Mix         = 0,
    Quantize    = 1,
    DeleteMeas  = 2,
    InsertMeas  = 3,
    CopyMeas    = 4,
    EraseMeas   = 5,
    RemoveEvent = 6
};

enum class SeqRecMode : uint8_t {
    Realtime = 0,
    Step     = 1,
    Punch    = 2
};

enum class SeqRecCondition : uint8_t {
    Replace = 0,
    Overdub = 1
};

struct ClassicLcdFrame {
    std::array<char, 40> line0{};
    std::array<char, 40> line1{};
    bool showCursor = false;
    int  cursorRow  = 0;
    int  cursorCol  = 0;
};

} // namespace vf50::classic
