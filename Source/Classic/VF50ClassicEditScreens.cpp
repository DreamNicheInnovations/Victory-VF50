#include "Classic/VF50ClassicController.h"

#include "DSP/FM/OpzTables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vf50::classic {

namespace {

constexpr char kGlyphPlayArrow  = static_cast<char>(0x10u); // '▶'
constexpr char kGlyphRightArrow = static_cast<char>(0x7Eu); // '→'
constexpr char kGlyphLeftArrow  = static_cast<char>(0x7Fu); // '←'
constexpr char kGlyphMultiPage  = static_cast<char>(0x14u); // '‡'

void clearLine40(std::array<char, 40>& line) noexcept {
    line.fill(' ');
}

void writeSpan40(std::array<char, 40>& line, int col, std::string_view sv, int maxLen = -1) noexcept {
    if (col < 0) return;
    const int avail = 40 - col;
    if (avail <= 0) return;
    int n = static_cast<int>(sv.size());
    if (maxLen >= 0) n = std::min(n, maxLen);
    n = std::min(n, avail);
    for (int i = 0; i < n; ++i) {
        const unsigned char raw = static_cast<unsigned char>(sv[static_cast<std::size_t>(i)]);
        if ((raw >= 0x01u && raw <= 0x14u)
            || raw == static_cast<unsigned char>(kGlyphRightArrow)
            || raw == static_cast<unsigned char>(kGlyphLeftArrow)
            || (raw >= 0x20u && raw <= 0x7Eu)) {
            line[static_cast<std::size_t>(col + i)] = static_cast<char>(raw);
        } else {
            line[static_cast<std::size_t>(col + i)] = ' ';
        }
    }
}

void setLine40(std::array<char, 40>& line, std::string_view sv) noexcept {
    clearLine40(line);
    writeSpan40(line, 0, sv, 40);
}

void formatMidiNote4(int midiNote, char out[5]) noexcept {
    static constexpr std::array<const char*, 12> kNotes = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int n = std::clamp(midiNote, 0, 127);
    const int oct = (n / 12) - 2;
    std::snprintf(out, 5, "%s%d", kNotes[static_cast<std::size_t>(n % 12)], oct);
}

enum class FxParamKind : uint8_t {
    None = 0,
    RevTime10,   // 0.3sec .. 10.0sec (raw 0..75)
    RevTime18,   // 0.3sec .. 18.3sec (raw 0..75)
    LpfHz,       // 1.25kHz .. 12.0kHz, thru (raw 0..14)
    HpfHz,       // 160Hz .. 1000Hz, thru (raw 0..10)
    Delay50Ms,   // 0.1ms .. 50ms (raw 0..50)
    Delay300Ms,  // 0ms .. 300ms (raw 0..99)
    Delay152Ms,  // 0ms .. 152ms (raw 0..99)
    Pct99,       // 0% .. 99% (raw 0..99)
    Pct100,      // 0% .. 100% (raw 0..99 -> mapped 0..100)
    Size32,      // 0.5 .. 3.2 (raw 0..27)
    ToneDb,      // -12dB .. +12dB (raw 0..24)
    Bril12       // 0 .. 12 (raw 0..12)
};

struct FxParamSpec {
    const char* label;
    FxParamKind kind;
    int maxRaw;
};

struct DspEffectSpec {
    const char* fullName;
    const char* shortName;
    std::array<FxParamSpec, 3> params;
};

constexpr std::array<DspEffectSpec, 33> kDspEffectSpecs = {{
    { "off",             "off    ", {{ { "",        FxParamKind::None,       0  }, { "",        FxParamKind::None,      0  }, { "",       FxParamKind::None,       0  } }} },
    { "Reverb Hall",     "RevHall", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Reverb Room",     "RevRoom", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Reverb Plate",    "RevPlat", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Delay",           "Delay  ", {{ { "Time",    FxParamKind::Delay300Ms, 75 }, { "FB Delay",FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Delay L/R",       "Dly L/R", {{ { "Lch Dly", FxParamKind::Delay300Ms, 75 }, { "Rch Dly", FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Stereo Echo",     "St Echo", {{ { "Lch Dly", FxParamKind::Delay300Ms, 75 }, { "Rch Dly", FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Distortion Rev.", "Dst Rev", {{ { "Time",    FxParamKind::RevTime18,  75 }, { "Dist.",   FxParamKind::Pct100,    99 }, { "Reverb", FxParamKind::Pct100,     99 } }} },
    { "Distortion Echo", "DstEcho", {{ { "Time",    FxParamKind::Delay300Ms, 75 }, { "FB Gain", FxParamKind::Pct99,     99 }, { "Dist.",  FxParamKind::Pct100,     99 } }} },
    { "Gate Reverb",     "GateRev", {{ { "Size",    FxParamKind::Size32,     27 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Reverse Gate",    "RevGate", {{ { "Size",    FxParamKind::Size32,     27 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Early Ref",       "E.Ref  ", {{ { "Size",    FxParamKind::Size32,     27 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Tone Control 1",  "Tone 1 ", {{ { "Low",     FxParamKind::ToneDb,     24 }, { "Middle",  FxParamKind::ToneDb,    24 }, { "High",   FxParamKind::ToneDb,     24 } }} },
    { "Delay & Rev.",    "Dly&Rev", {{ { "RevTime", FxParamKind::RevTime10,  75 }, { "Delay",   FxParamKind::Delay152Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "DelayL/R & Rev.", "DLR&Rev", {{ { "RevTime", FxParamKind::RevTime10,  75 }, { "Lch Dly", FxParamKind::Delay300Ms,99 }, { "Rch Dly",FxParamKind::Delay300Ms, 99 } }} },
    { "Dist. & Delay",   "Dst&Dly", {{ { "Time",    FxParamKind::Delay300Ms, 75 }, { "FB Gain", FxParamKind::Pct99,     99 }, { "Dist.",  FxParamKind::Pct100,     99 } }} },
    { "Church",          "Church ", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Club",            "Club   ", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Stage",           "Stage  ", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Bath Room",       "BathRm ", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Metal",           "Metal  ", {{ { "Time",    FxParamKind::RevTime10,  75 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "Delay",  FxParamKind::Delay50Ms,  50 } }} },
    { "Tunnel",          "Tunnel ", {{ { "RevTime", FxParamKind::RevTime10,  75 }, { "Delay",   FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Doubler 1",       "Doubl 1", {{ { "DlyTime", FxParamKind::Delay50Ms,  50 }, { "HPF",     FxParamKind::HpfHz,     10 }, { "LPF",    FxParamKind::LpfHz,      14 } }} },
    { "Doubler 2",       "Doubl 2", {{ { "Lch Dly", FxParamKind::Delay50Ms,  50 }, { "Rch Dly", FxParamKind::Delay50Ms, 50 }, { "LPF",    FxParamKind::LpfHz,      14 } }} },
    { "Feed Back Gate",  "FB Gate", {{ { "Size",    FxParamKind::Size32,     27 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "F. Back Reverse", "FB Rev ", {{ { "Size",    FxParamKind::Size32,     27 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Feed Back E/R",   "FB E/R ", {{ { "Size",    FxParamKind::Size32,     27 }, { "LPF",     FxParamKind::LpfHz,     14 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Delay & Tone1",   "Dly&Tn1", {{ { "Bril",    FxParamKind::Bril12,     12 }, { "Delay",   FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Dly L/R & Tone1", "DLR&Tn1", {{ { "Bril",    FxParamKind::Bril12,     12 }, { "Delay",   FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Tone Control 2",  "Tone 2 ", {{ { "HPF",     FxParamKind::HpfHz,      10 }, { "Middle",  FxParamKind::ToneDb,    24 }, { "LPF",    FxParamKind::LpfHz,      14 } }} },
    { "Delay & Tone2",   "Dly&Tn2", {{ { "Bril",    FxParamKind::Bril12,     12 }, { "Delay",   FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Dly L/R & Tone2", "DLR&Tn2", {{ { "Bril",    FxParamKind::Bril12,     12 }, { "Delay",   FxParamKind::Delay300Ms,99 }, { "FB Gain",FxParamKind::Pct99,      99 } }} },
    { "Distortion",      "Distort", {{ { "Dist.",   FxParamKind::Pct100,     75 }, { "HPF",     FxParamKind::HpfHz,     10 }, { "LPF",    FxParamKind::LpfHz,      14 } }} }
}};

void formatFxParamsHeader(char* out,
                          std::size_t cap,
                          const char* prefix,
                          char delim,
                          int cursor,
                          const DspEffectSpec& spec,
                          bool bypassed) noexcept {
    const char c0 = (cursor == 0) ? kGlyphPlayArrow : '>';
    const char c1 = (cursor == 1) ? kGlyphPlayArrow : '>';
    const char c2 = (cursor == 2) ? kGlyphPlayArrow : '>';
    if (bypassed) {
        // Reserve 6 chars for "BYPASS" + 1 char for multi-page glyph within 40 columns
        const int pLen = static_cast<int>(std::strlen(prefix));
        if (pLen <= 4) {
            std::snprintf(out, cap, "%s EFCT%c%c%-6.6s %c%-6.6s %c%-6.6sBYPASS%c",
                          prefix, delim, c0, spec.params[0].label, c1, spec.params[1].label,
                          c2, spec.params[2].label, kGlyphMultiPage);
        } else {
            std::snprintf(out, cap, "%s EFCT%c%c%-6.6s %c%-6.6s %c%-5.5sBYPASS%c",
                          prefix, delim, c0, spec.params[0].label, c1, spec.params[1].label,
                          c2, spec.params[2].label, kGlyphMultiPage);
        }
    } else {
        const int pLen = static_cast<int>(std::strlen(prefix));
        const char* pad = (pLen <= 4) ? "  " : " ";
        std::snprintf(out, cap, "%s EFCT%c%c%-7.7s %c%-7.7s %c%-7.7s%s%c",
                      prefix, delim, c0, spec.params[0].label, c1, spec.params[1].label,
                      c2, spec.params[2].label, pad, kGlyphMultiPage);
    }
}

void formatFxParamValue(const FxParamSpec& spec, int rawVal, char out[10]) noexcept {
    const int v = std::clamp(rawVal, 0, std::max(1, spec.maxRaw));
    switch (spec.kind) {
        case FxParamKind::None:
            std::snprintf(out, 10, "       ");
            break;
        case FxParamKind::RevTime10: {
            const float sec = 0.3f + (static_cast<float>(v) / 75.0f) * 9.7f;
            std::snprintf(out, 10, "%4.1fsec", static_cast<double>(sec));
            break;
        }
        case FxParamKind::RevTime18: {
            const float sec = 0.3f + (static_cast<float>(v) / 75.0f) * 18.0f;
            std::snprintf(out, 10, "%4.1fsec", static_cast<double>(sec));
            break;
        }
        case FxParamKind::LpfHz: {
            static constexpr std::array<const char*, 15> kLpfStr = {
                "1.25kHz", "1.40kHz", "1.60kHz", "2.00kHz", "2.50kHz",
                "3.15kHz", "4.00kHz", "5.00kHz", "6.30kHz", "7.10kHz",
                "8.00kHz", "9.00kHz", "10.0kHz", "12.0kHz", "thru   "
            };
            std::snprintf(out, 10, "%-7.7s", kLpfStr[static_cast<std::size_t>(std::clamp(v, 0, 14))]);
            break;
        }
        case FxParamKind::HpfHz: {
            static constexpr std::array<const char*, 11> kHpfStr = {
                "160Hz  ", "200Hz  ", "250Hz  ", "315Hz  ", "400Hz  ",
                "500Hz  ", "630Hz  ", "710Hz  ", "800Hz  ", "1000Hz ", "thru   "
            };
            std::snprintf(out, 10, "%-7.7s", kHpfStr[static_cast<std::size_t>(std::clamp(v, 0, 10))]);
            break;
        }
        case FxParamKind::Delay50Ms:
            if (v == 0) std::snprintf(out, 10, "0.1ms  ");
            else        std::snprintf(out, 10, "%2dms   ", v);
            break;
        case FxParamKind::Delay300Ms: {
            const int ms = (v * 300) / std::max(1, spec.maxRaw);
            std::snprintf(out, 10, "%3dms  ", ms);
            break;
        }
        case FxParamKind::Delay152Ms: {
            const int ms = (v * 152) / std::max(1, spec.maxRaw);
            std::snprintf(out, 10, "%3dms  ", ms);
            break;
        }
        case FxParamKind::Pct99:
            std::snprintf(out, 10, "%2d%%    ", std::clamp(v, 0, 99));
            break;
        case FxParamKind::Pct100: {
            const int pct = (v * 100) / std::max(1, spec.maxRaw);
            std::snprintf(out, 10, "%3d%%   ", pct);
            break;
        }
        case FxParamKind::Size32: {
            const float sz = 0.5f + static_cast<float>(v) * 0.1f;
            std::snprintf(out, 10, "%3.1f    ", static_cast<double>(sz));
            break;
        }
        case FxParamKind::ToneDb: {
            const int db = v - 12;
            std::snprintf(out, 10, "%+3ddB  ", db);
            break;
        }
        case FxParamKind::Bril12:
            std::snprintf(out, 10, "%2d     ", std::clamp(v, 0, 12));
            break;
    }
}

uint8_t& fxRawParamRef(vf50::generated::DspEffectSettings& fx, int idx0To2) noexcept {
    if (idx0To2 == 0) return fx.param1;
    if (idx0To2 == 1) return fx.param2;
    return fx.param3;
}

uint8_t fxRawParamVal(const vf50::generated::DspEffectSettings& fx, int idx0To2) noexcept {
    if (idx0To2 == 0) return fx.param1;
    if (idx0To2 == 1) return fx.param2;
    return fx.param3;
}

// Engine pef index -> UI linear index (0=off, 1..4=delay1..4, 5..8=pan1..4, 9..12=chord1..4)
constexpr std::array<uint8_t, 13> kPerfFxUiToEngine = {
    0u, 1u, 4u, 7u, 10u, 2u, 5u, 8u, 11u, 3u, 6u, 9u, 12u
};

int perfFxEngineToUi(uint8_t engineVal) noexcept {
    for (int i = 0; i < 13; ++i) {
        if (kPerfFxUiToEngine[static_cast<std::size_t>(i)] == engineVal) {
            return i;
        }
    }
    return 0;
}

constexpr std::array<const char*, 13> kMicroTuneTableNames = {
    " 1:Equal        ",
    " 2:Pure(major)  ",
    " 3:Pure(minor)  ",
    " 4:Mean tone    ",
    " 5:Pythagorean  ",
    " 6:Werckmeister ",
    " 7:Kirnberger   ",
    " 8:Vallotti&Yng ",
    " 9:1/4 shift Eq ",
    "10:1/4 tone     ",
    "11:1/8 tone     ",
    "   User octave  ",
    "   User full    "
};

constexpr std::array<const char*, 12> kKeyNoteNames = {
    "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
};

constexpr std::array<const char*, 8> kAlgRow0 = {
    "4>3>2>1 ", "43>2>1  ", "4+3>2>1 ", "4>3\\2>1 ",
    "4>3 2>1 ", "4>3 4>2 ", "4>3 2 1 ", "4 3 2 1 "
};
constexpr std::array<const char*, 8> kAlgRow1 = {
    "        ", "        ", "        ", "        ",
    "  ->    ", "    4>1 ", "        ", "        "
};

[[nodiscard]] constexpr bool isOperatorCarrierLocal(uint8_t algorithm, std::size_t opIdx) noexcept {
    switch (algorithm & 0x07u) {
        case 0u:
        case 1u:
        case 2u:
        case 3u:
            return opIdx == 0u;
        case 4u:
            return opIdx == 0u || opIdx == 2u;
        case 5u:
        case 6u:
            return opIdx == 0u || opIdx == 1u || opIdx == 2u;
        case 7u:
        default:
            return true;
    }
}

[[nodiscard]] bool isDiskScreenLocal(ClassicScreen s) noexcept {
    return s >= ClassicScreen::DiskRoot && s <= ClassicScreen::DiskBackupPrompt;
}

} // namespace

// =============================================================================
// Screen Classification & Backup / Audio Commit Helpers
// =============================================================================

bool VF50ClassicController::isPerfEditScreen(ClassicScreen s) noexcept {
    return s >= ClassicScreen::PerfEditAssignMode && s <= ClassicScreen::PerfEditEffect;
}

bool VF50ClassicController::isVoiceEditScreen(ClassicScreen s) noexcept {
    return s >= ClassicScreen::VoiceEditQuick && s <= ClassicScreen::VoiceEditEffect;
}

bool VF50ClassicController::isUtilOthersScreen(ClassicScreen s) noexcept {
    return s >= ClassicScreen::UtilOthersVoice && s <= ClassicScreen::UtilOthersMultiVoiceSelect;
}

bool VF50ClassicController::isStoreOrCopyScreen(ClassicScreen s) noexcept {
    return s >= ClassicScreen::StoreVoice && s <= ClassicScreen::CopyEg;
}

bool VF50ClassicController::isPerformanceContextScreen() const noexcept {
    if (multiVoiceEditActive_) return false;
    if (screen_ == ClassicScreen::PlayPerformance || isPerfEditScreen(screen_)
        || screen_ == ClassicScreen::UtilOthersPerf || screen_ == ClassicScreen::UtilOthersPerfInit
        || screen_ == ClassicScreen::UtilOthersMultiVoiceSelect || screen_ == ClassicScreen::StorePerf
        || screen_ == ClassicScreen::StoreMultiVoice || screen_ == ClassicScreen::CopyPerfEffect) {
        return true;
    }
    if (screen_ == ClassicScreen::PlaySingle || isVoiceEditScreen(screen_)
        || screen_ == ClassicScreen::UtilOthersVoice || screen_ == ClassicScreen::StoreVoice
        || screen_ == ClassicScreen::CopyEg) {
        return false;
    }
    const auto ws = processor_.hostMemory().workstationMode();
    return ws == vf50::state::WorkstationMode::PerformancePlay
        || ws == vf50::state::WorkstationMode::PerformanceEdit;
}

bool VF50ClassicController::isPerfInstActive(const vf50::generated::FactoryPerformance& pf, int instIdx) const noexcept {
    if (instIdx < 0 || instIdx >= 8) return false;
    const auto& inst = pf.instruments[static_cast<std::size_t>(instIdx)];
    if (pf.assignMode == 2u) {
        return inst.reserveNotes > 0u || inst.maxNotes > 0u;
    }
    return inst.maxNotes > 0u;
}

void VF50ClassicController::ensurePerfEditBackupInitialized() noexcept {
    auto& mem = processor_.hostMemory();
    if (mem.workstationMode() != vf50::state::WorkstationMode::PerformanceEdit) {
        mem.setWorkstationMode(vf50::state::WorkstationMode::PerformanceEdit);
    }
}

void VF50ClassicController::ensureVoiceEditBackupInitialized() noexcept {
    auto& mem = processor_.hostMemory();
    if (mem.workstationMode() != vf50::state::WorkstationMode::SingleEdit) {
        mem.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
    }
}

void VF50ClassicController::commitCurrentVoiceEdit(bool syncEffects) noexcept {
    if (multiVoiceEditActive_) {
        saveActiveMultiVoiceBackToSlot();
        processor_.commitHostVoiceEditToAudio(false);
    } else {
        processor_.commitHostVoiceEditToAudio(syncEffects);
    }
}

void VF50ClassicController::commitCurrentPerfEdit(bool syncEffects) noexcept {
    processor_.commitHostPerfEditToAudio(syncEffects);
}

void VF50ClassicController::syncQuickEditBase() noexcept {
    quickEditBaseOps_ = processor_.hostMemory().singleEditSlot().voice.operators;
    quickEditRelOffset_ = 0;
}

void VF50ClassicController::applyQuickEditOffset(int newRelOffset) noexcept {
    quickEditRelOffset_ = std::clamp(newRelOffset, -50, 50);
    auto& voice = processor_.hostMemory().singleEditSlot().voice;
    for (std::size_t op = 0u; op < 4u; ++op) {
        const auto& base = quickEditBaseOps_[op];
        auto& dst = voice.operators[op];
        const bool isCarrier = isOperatorCarrierLocal(voice.algorithm, op);

        if (voiceQuickSubPage_ == 0) {
            // ATTACK: AR & D1R simultaneously (Spec §2.1, p. 39)
            const int rateDelta = static_cast<int>(std::lround(static_cast<float>(quickEditRelOffset_) * 0.32f));
            dst.ar  = static_cast<uint8_t>(std::clamp(static_cast<int>(base.ar) + rateDelta, 0, 31));
            dst.d1r = static_cast<uint8_t>(std::clamp(static_cast<int>(base.d1r) + rateDelta, 0, 31));
        } else if (voiceQuickSubPage_ == 1) {
            // RELEAS: RR simultaneously
            const int rrDelta = static_cast<int>(std::lround(static_cast<float>(quickEditRelOffset_) * 0.16f));
            dst.rr = static_cast<uint8_t>(std::clamp(static_cast<int>(base.rr) + rrDelta, 1, 15));
        } else if (voiceQuickSubPage_ == 2) {
            // VOLUME: Carrier Output Levels only
            if (isCarrier) {
                dst.outputLevel = static_cast<uint8_t>(std::clamp(static_cast<int>(base.outputLevel) + quickEditRelOffset_, 0, 99));
            }
        } else {
            // BRIL: Modulator Output Levels only
            if (!isCarrier) {
                dst.outputLevel = static_cast<uint8_t>(std::clamp(static_cast<int>(base.outputLevel) + quickEditRelOffset_, 0, 99));
            }
        }
    }
    processor_.hostMemory().markSingleEditDirty();
    commitCurrentVoiceEdit(false);
}

void VF50ClassicController::enterMultiVoiceSlot(int instIdx) noexcept {
    const int idx = std::clamp(instIdx, 0, 7);
    const auto& perf = processor_.hostMemory().perfEditSlot().perf;
    if (!isPerfInstActive(perf, idx)) {
        setNotice("Slot Inactive (*)", 45);
        return;
    }
    multiVoiceEditActive_ = true;
    multiVoiceEditInst_ = idx;
    const auto& inst = perf.instruments[static_cast<std::size_t>(idx)];
    const bool perfIsCard = (processor_.hostMemory().selectedPerfBank() == vf50::state::MemoryBank::UserRam) && cardBankActive_;

    if (!multiVoiceDirty_[static_cast<std::size_t>(idx)]) {
        if (inst.voiceBank != 0u) {
            multiVoiceBuffers_[static_cast<std::size_t>(idx)].overwriteCleanFromPreset(
                processor_.hostMemory().getFactoryVoice(inst.voiceNumber));
        } else if (perfIsCard) {
            multiVoiceBuffers_[static_cast<std::size_t>(idx)] =
                processor_.virtualCard().activeBank().voices[std::min<std::size_t>(inst.voiceNumber, 99u)];
        } else {
            multiVoiceBuffers_[static_cast<std::size_t>(idx)].overwriteCleanFromPreset(
                processor_.hostMemory().getUserVoice(inst.voiceNumber));
        }
    }

    processor_.hostMemory().singleEditSlot() = multiVoiceBuffers_[static_cast<std::size_t>(idx)];
    processor_.hostMemory().setOperatorEnableMask(0x0Fu);
    processor_.hostMemory().setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
    commitCurrentVoiceEdit(false);
    screen_ = ClassicScreen::VoiceEditAlgorithm;
}

void VF50ClassicController::saveActiveMultiVoiceBackToSlot() noexcept {
    if (!multiVoiceEditActive_) return;
    const int idx = std::clamp(multiVoiceEditInst_, 0, 7);
    multiVoiceBuffers_[static_cast<std::size_t>(idx)] = processor_.hostMemory().singleEditSlot();
    multiVoiceDirty_[static_cast<std::size_t>(idx)] = true;
}

void VF50ClassicController::resetPerfEffectMemoriesToFactory() noexcept {
    processor_.mutablePerfEffectBank() = vf50::state::makeFactoryPerfEffectBank();
    processor_.publishClassicSystemStateToAudio();
}

const vf50::generated::FactoryVoice& VF50ClassicController::getCardVoice(std::size_t idx) const noexcept {
    return processor_.virtualCard().activeBank().voices[std::min<std::size_t>(idx, 99u)].voice;
}

const vf50::generated::FactoryPerformance& VF50ClassicController::getCardPerformance(std::size_t idx) const noexcept {
    return processor_.virtualCard().activeBank().performances[std::min<std::size_t>(idx, 99u)].perf;
}

// =============================================================================
// Top-Level Edit Button Handlers: Edit Matrix, Utility OTHERS, Store, Text
// =============================================================================

bool VF50ClassicController::onPressUtilityOthers() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    utilConfirmPending_ = false;
    multiVoiceExitSglConfirm_ = false;

    if (multiVoiceEditActive_) {
        saveActiveMultiVoiceBackToSlot();
        screen_ = ClassicScreen::UtilOthersMultiVoiceSelect;
        return true;
    }

    if (screen_ == ClassicScreen::UtilOthersVoice || screen_ == ClassicScreen::UtilOthersPerf) {
        screen_ = preUtilityScreen_;
        return true;
    }

    if (!isUtilOthersScreen(screen_) && !isDiskScreenLocal(screen_) && screen_ != ClassicScreen::UtilMemoryProtect) {
        preUtilityScreen_ = screen_;
    }

    if (isPerformanceContextScreen()) {
        screen_ = ClassicScreen::UtilOthersPerf;
    } else {
        screen_ = ClassicScreen::UtilOthersVoice;
    }
    return true;
}

bool VF50ClassicController::onPressEditMatrixKey(int index0To11) noexcept {
    const int idx = std::clamp(index0To11, 0, 11);
    clearNotice();
    clearPendingBankDigits();
    const bool wasInstMinusArmed = instMinusArmed_;
    instMinusArmed_ = false;
    bootSplashTicks_ = 0;

    if (screen_ == ClassicScreen::PlayPerformance && wasInstMinusArmed
        && ((idx >= 0 && idx <= 4) || (idx >= 6 && idx <= 9))) {
        instMinusArmed_ = true;
        if (idx <= 3) return onPressEditTrackKey(idx);
        if (idx >= 6 && idx <= 9) return onPressEditTrackKey((idx - 6) + 4);
        return onPressRhyTrackKey();
    }

    // Sequencer track mute priority when in Sequencer screens
    const bool inSeqMuteScr = (screen_ == ClassicScreen::SeqPlaySong
                               || screen_ == ClassicScreen::SeqJobSelect
                               || screen_ == ClassicScreen::SeqJobSetup
                               || screen_ == ClassicScreen::SeqJobEditMenu
                               || screen_ == ClassicScreen::SeqRecStandby
                               || screen_ == ClassicScreen::SeqStepBarGraph);
    if (inSeqMuteScr || (processor_.isDemoPlaying() && !isPerfEditScreen(screen_) && !isVoiceEditScreen(screen_)
                          && screen_ != ClassicScreen::PlayPerformance && screen_ != ClassicScreen::PlaySingle)) {
        if (idx >= 0 && idx <= 3) return onPressEditTrackKey(idx);
        if (idx >= 6 && idx <= 9) return onPressEditTrackKey((idx - 6) + 4);
        if (idx == 4) return onPressRhyTrackKey();
    }

    // If inside Multi-Voice Edit selector, return to active voice edit
    if (screen_ == ClassicScreen::UtilOthersMultiVoiceSelect) {
        enterMultiVoiceSlot(multiVoiceEditInst_);
    }

    if (isPerformanceContextScreen()) {
        ensurePerfEditBackupInitialized();
        preEditReturnScreen_ = ClassicScreen::PlayPerformance;
        const auto& pf = processor_.hostMemory().perfEditSlot().perf;

        switch (idx) {
            case 0: screen_ = ClassicScreen::PerfEditAssignMode; break;
            case 1: screen_ = ClassicScreen::PerfEditNotes; break;
            case 2: screen_ = ClassicScreen::PerfEditVoiceNo; break;
            case 3: screen_ = ClassicScreen::PerfEditRecvCh; break;
            case 4: screen_ = ClassicScreen::PerfEditLimitLow; break;
            case 5: screen_ = ClassicScreen::PerfEditLimitHigh; break;
            case 6: screen_ = ClassicScreen::PerfEditDetune; break;
            case 7: screen_ = ClassicScreen::PerfEditNoteShift; break;
            case 8: screen_ = ClassicScreen::PerfEditVolume; break;
            case 9: screen_ = ClassicScreen::PerfEditOutAssign; break;
            case 10: // OTHERS 4-press cycle (pp. 28-30)
                if (screen_ == ClassicScreen::PerfEditOthers) {
                    perfOthersSubPage_ = (perfOthersSubPage_ + 1) & 3;
                } else {
                    perfOthersSubPage_ = 0;
                    screen_ = ClassicScreen::PerfEditOthers;
                }
                break;
            case 11: { // EFFECT 4-press cycle (pp. 30-35)
                const int maxPages = (pf.effect.effectType == 0u) ? 2 : 4;
                if (screen_ == ClassicScreen::PerfEditEffect) {
                    perfEffectSubPage_ = (perfEffectSubPage_ + 1) % maxPages;
                } else {
                    perfEffectSubPage_ = 0;
                    perfEffectCursor_ = 0;
                    screen_ = ClassicScreen::PerfEditEffect;
                }
                break;
            }
            default: break;
        }
        return true;
    }

    // Chapter 3 — Voice Edit (pp. 36-56)
    ensureVoiceEditBackupInitialized();
    preEditReturnScreen_ = ClassicScreen::PlaySingle;
    const auto& voice = processor_.hostMemory().singleEditSlot().voice;

    switch (idx) {
        case 0: // TR1 QUICK EDIT 4-press cycle (pp. 39-40)
            if (screen_ == ClassicScreen::VoiceEditQuick) {
                voiceQuickSubPage_ = (voiceQuickSubPage_ + 1) & 3;
            } else {
                voiceQuickSubPage_ = 0;
                screen_ = ClassicScreen::VoiceEditQuick;
            }
            syncQuickEditBase();
            break;
        case 1: screen_ = ClassicScreen::VoiceEditAlgorithm; break;
        case 2: screen_ = ClassicScreen::VoiceEditLfo; break;
        case 3: screen_ = ClassicScreen::VoiceEditSens; break;
        case 4: screen_ = ClassicScreen::VoiceEditOsc; break;
        case 5: screen_ = ClassicScreen::VoiceEditEg; break;
        case 6: screen_ = ClassicScreen::VoiceEditPeg; break;
        case 7: screen_ = ClassicScreen::VoiceEditOutLevel; break;
        case 8: // TR7 SCALING 2-press cycle (p. 48)
            if (screen_ == ClassicScreen::VoiceEditScaling) {
                voiceScalingSubPage_ ^= 1;
            } else {
                voiceScalingSubPage_ = 0;
                screen_ = ClassicScreen::VoiceEditScaling;
            }
            break;
        case 9: screen_ = ClassicScreen::VoiceEditTranspose; break;
        case 10: // FUNCTION root (pp. 49-54)
            voiceFuncCursor_ = 0;
            screen_ = ClassicScreen::VoiceEditFuncRoot;
            break;
        case 11: { // EFFECT 3-press cycle (pp. 54-56), unavailable in Multi-Voice Edit (p. 118)
            if (multiVoiceEditActive_) {
                setNotice("Not Avail in MultiEdit", 45);
                return false;
            }
            const int maxPages = (voice.effect.effectType == 0u) ? 1 : 3;
            if (screen_ == ClassicScreen::VoiceEditEffect) {
                voiceEffectSubPage_ = (voiceEffectSubPage_ + 1) % maxPages;
            } else {
                voiceEffectSubPage_ = 0;
                voiceEffectCursor_ = 0;
                screen_ = ClassicScreen::VoiceEditEffect;
            }
            break;
        }
        default: break;
    }
    return true;
}

bool VF50ClassicController::onPressStore() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;

    if (screen_ == ClassicScreen::SeqPlaySong) {
        // Ch. 2.4 p. 94: STORE) Setup data to active Song 1..8
        screen_ = ClassicScreen::SeqStoreSetupConfirm;
        return true;
    }

    if (isCompareModeLocked()) {
        setNotice("Compare Active - Locked", 45);
        return false;
    }

    // Spec §4.3 (p. 120): Contextual Copy routines
    if (screen_ == ClassicScreen::VoiceEditEg) {
        storeReturnScreen_ = screen_;
        copyEgSrcOp_ = selectedOperator_;
        copyEgDstOp_ = (selectedOperator_ + 1) & 3;
        copyEgCursor_ = 1;
        copyConfirmReady_ = false;
        screen_ = ClassicScreen::CopyEg;
        return true;
    }

    if (screen_ == ClassicScreen::VoiceEditEffect || screen_ == ClassicScreen::PerfEditEffect) {
        if (multiVoiceEditActive_) {
            setNotice("Not Avail in MultiEdit", 45);
            return false;
        }
        storeReturnScreen_ = screen_;
        storeTargetCard_ = cardBankActive_;
        storeTargetSlot_ = static_cast<int>(isPerformanceContextScreen()
                                                ? processor_.hostMemory().selectedPerfIndex()
                                                : processor_.hostMemory().selectedVoiceIndex());
        storePendingTens_ = -1;
        storeConfirmReady_ = false;
        storeIntProtBypassed_ = false;
        screen_ = ClassicScreen::CopyEffect;
        return true;
    }

    if (screen_ == ClassicScreen::SetupDelayEdit ||
        screen_ == ClassicScreen::SetupPanEdit ||
        screen_ == ClassicScreen::SetupChordEdit) {
        storeReturnScreen_ = screen_;
        copyPerfFxSrcIdx_ = std::clamp(setupPerfFxCat_ * 4 + setupPerfFxSlot_, 0, 11);
        copyPerfFxDstSlot_ = ((copyPerfFxSrcIdx_ % 4) + 1) & 3;
        copyConfirmReady_ = false;
        screen_ = ClassicScreen::CopyPerfEffect;
        return true;
    }

    if (screen_ == ClassicScreen::PerfEditOthers && perfOthersSubPage_ == 3) {
        const uint8_t pef = processor_.hostMemory().perfEditSlot().perf.perfEffectSelect;
        if (pef == 0u) {
            setNotice("Perf Effect is off", 45);
            return false;
        }
        storeReturnScreen_ = screen_;
        copyPerfFxSrcIdx_ = perfFxEngineToUi(pef) - 1; // 0..11
        copyPerfFxDstSlot_ = ((copyPerfFxSrcIdx_ % 4) + 1) & 3;
        copyConfirmReady_ = false;
        screen_ = ClassicScreen::CopyPerfEffect;
        return true;
    }

    // Multi-Voice Store (p. 119)
    if (multiVoiceEditActive_ || screen_ == ClassicScreen::UtilOthersMultiVoiceSelect) {
        saveActiveMultiVoiceBackToSlot();
        const bool perfIsCard = (processor_.hostMemory().selectedPerfBank() == vf50::state::MemoryBank::UserRam) && cardBankActive_;
        storeReturnScreen_ = ClassicScreen::UtilOthersMultiVoiceSelect;
        storeTargetCard_ = perfIsCard;
        storeTargetSlot_ = static_cast<int>(processor_.hostMemory().perfEditSlot().perf.instruments[static_cast<std::size_t>(multiVoiceEditInst_)].voiceNumber);
        storePendingTens_ = -1;
        storeConfirmReady_ = false;
        storeIntProtBypassed_ = false;
        screen_ = ClassicScreen::StoreMultiVoice;
        return true;
    }

    // Spec §1.0 / §2.0 / §4.3: On STORE, automatically re-enable muted instruments & disabled operators
    if (isPerformanceContextScreen()) {
        resetPerfInstrumentMutes();
        if (screen_ == ClassicScreen::StorePerf) {
            storeConfirmReady_ = true;
            return true;
        }
        storeReturnScreen_ = screen_;
        storeTargetCard_ = cardBankActive_;
        storeTargetSlot_ = static_cast<int>(processor_.hostMemory().selectedPerfIndex());
        storePendingTens_ = -1;
        storeConfirmReady_ = false;
        storeIntProtBypassed_ = false;
        screen_ = ClassicScreen::StorePerf;
        return true;
    }

    processor_.hostMemory().setOperatorEnableMask(0x0Fu);
    commitCurrentVoiceEdit(false);
    if (screen_ == ClassicScreen::StoreVoice) {
        storeConfirmReady_ = true;
        return true;
    }
    storeReturnScreen_ = screen_;
    storeTargetCard_ = cardBankActive_;
    storeTargetSlot_ = static_cast<int>(processor_.hostMemory().selectedVoiceIndex());
    storePendingTens_ = -1;
    storeConfirmReady_ = false;
    storeIntProtBypassed_ = false;
    screen_ = ClassicScreen::StoreVoice;
    return true;
}

bool VF50ClassicController::isTextEntryScreenActive() const noexcept {
    if (screen_ == ClassicScreen::DiskFileAction
        && (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename)) {
        return true;
    }
    if (screen_ == ClassicScreen::VoiceEditFuncName) {
        return true;
    }
    if (screen_ == ClassicScreen::PerfEditAssignMode && perfAssignCursor_ == 1) {
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSong && !songClearConfirm_) {
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEditJob && songEdJobSub_ == 6) {
        return true;
    }
    if (screen_ == ClassicScreen::UtilEditMessage) {
        return true;
    }
    return false;
}

bool VF50ClassicController::onTextKeyPressed(char asciiChar) noexcept {
    if (asciiChar < 0x20 || asciiChar > 0x7E) return false;
    clearNotice();

    if (screen_ == ClassicScreen::DiskFileAction
        && (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename)) {
        if (diskActionConfirm_) {
            return false;
        }
        // Reject illegal Windows / FAT12 filename characters and '.' separator during stem entry
        if (asciiChar == '\\' || asciiChar == '/' || asciiChar == ':' || asciiChar == '*'
            || asciiChar == '?' || asciiChar == '"' || asciiChar == '<' || asciiChar == '>'
            || asciiChar == '|' || asciiChar == '.') {
            return false;
        }
        const char upperCh = (asciiChar >= 'a' && asciiChar <= 'z')
                                 ? static_cast<char>(asciiChar - 'a' + 'A')
                                 : asciiChar;
        diskEditingName_ = true;
        if (diskOp_ == DiskOperation::Save && diskNameFreshDefault_) {
            diskEditName_.fill(' ');
            diskNameCursorCol_ = 0;
            diskNameFreshDefault_ = false;
        }
        const int col = std::clamp(diskNameCursorCol_, 0, 7);
        diskEditName_[static_cast<std::size_t>(col)] = upperCh;
        diskNameCursorCol_ = std::min(8, col + 1);
        return true;
    }

    if (screen_ == ClassicScreen::SeqJobSong && !songClearConfirm_) {
        auto& nm = songSlots_[static_cast<std::size_t>(activeSongSlot_)].name;
        const int col = std::clamp(songNameCursorCol_, 0, 7);
        nm[static_cast<std::size_t>(col)] = asciiChar;
        songNameCursorCol_ = std::min(8, col + 1);
        syncSeqSlotNameToAudio();
        return true;
    }

    if (isCompareModeLocked()) {
        setNotice("Compare Active - Locked", 45);
        return false;
    }
    if (screen_ == ClassicScreen::PerfEditAssignMode && perfAssignCursor_ == 1) {
        auto& slot = processor_.hostMemory().perfEditSlot();
        auto nm = slot.nameChars;
        const int col = std::clamp(perfNameCursorCol_, 0, 9);
        nm[static_cast<std::size_t>(col)] = asciiChar;
        slot.setName(std::string_view(nm.data(), nm.size()));
        perfNameCursorCol_ = std::min(10, col + 1);
        processor_.hostMemory().markPerfEditDirty();
        commitCurrentPerfEdit(false);
        return true;
    }
    if (screen_ == ClassicScreen::VoiceEditFuncName) {
        auto& slot = processor_.hostMemory().singleEditSlot();
        auto nm = slot.nameChars;
        const int col = std::clamp(voiceNameCursorCol_, 0, 9);
        nm[static_cast<std::size_t>(col)] = asciiChar;
        slot.setName(std::string_view(nm.data(), nm.size()));
        voiceNameCursorCol_ = std::min(10, col + 1);
        processor_.hostMemory().markSingleEditDirty();
        commitCurrentVoiceEdit(false);
        return true;
    }
    if (isRhythmScreen(screen_)) {
        return handleRhythmTextChar(asciiChar);
    }
    if (isPhase3UtilScreen(screen_)) {
        return handleUtilTextChar(asciiChar);
    }
    return false;
}

bool VF50ClassicController::onTextBackspace() noexcept {
    clearNotice();
    if (screen_ == ClassicScreen::DiskFileAction
        && (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename)) {
        if (diskActionConfirm_) return true;
        diskEditingName_ = true;
        diskNameFreshDefault_ = false;
        if (diskNameCursorCol_ > 0) {
            --diskNameCursorCol_;
        }
        diskEditName_[static_cast<std::size_t>(std::clamp(diskNameCursorCol_, 0, 7))] = ' ';
        return true;
    }
    if (screen_ == ClassicScreen::VoiceEditFuncName) {
        if (isCompareModeLocked()) return false;
        if (voiceNameCursorCol_ > 0) {
            --voiceNameCursorCol_;
        }
        const int col = std::clamp(voiceNameCursorCol_, 0, 9);
        auto& slot = processor_.hostMemory().singleEditSlot();
        auto nm = slot.nameChars;
        nm[static_cast<std::size_t>(col)] = ' ';
        slot.setName(std::string_view(nm.data(), nm.size()));
        processor_.hostMemory().markSingleEditDirty();
        commitCurrentVoiceEdit(false);
        return true;
    }
    if (screen_ == ClassicScreen::PerfEditAssignMode && perfAssignCursor_ == 1) {
        if (isCompareModeLocked()) return false;
        if (perfNameCursorCol_ > 0) {
            --perfNameCursorCol_;
        }
        const int col = std::clamp(perfNameCursorCol_, 0, 9);
        auto& slot = processor_.hostMemory().perfEditSlot();
        auto nm = slot.nameChars;
        nm[static_cast<std::size_t>(col)] = ' ';
        slot.setName(std::string_view(nm.data(), nm.size()));
        processor_.hostMemory().markPerfEditDirty();
        commitCurrentPerfEdit(false);
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSong && !songClearConfirm_) {
        if (songNameCursorCol_ > 0) {
            --songNameCursorCol_;
        }
        const int col = std::clamp(songNameCursorCol_, 0, 7);
        songSlots_[static_cast<std::size_t>(activeSongSlot_)].name[static_cast<std::size_t>(col)] = ' ';
        syncSeqSlotNameToAudio();
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEditJob && songEdJobSub_ == 6 && rhythmSongs_ != nullptr) {
        if (songEdNameCol_ > 0) {
            --songEdNameCol_;
        }
        const int col = std::clamp(songEdNameCol_, 0, 7);
        (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)].name[static_cast<std::size_t>(col)] = ' ';
        processor_.publishRhythmSongsToAudio();
        return true;
    }
    if (screen_ == ClassicScreen::UtilEditMessage) {
        if (msgEditCursorCol_ > 0) {
            --msgEditCursorCol_;
        }
        const int col = std::clamp(msgEditCursorCol_, 0, 39);
        auto& sys = processor_.mutableClassicSystemSettings();
        sys.powerOnMessage[static_cast<std::size_t>(col)] = ' ';
        msgMultiTapDigit_ = -1;
        processor_.publishClassicSystemStateToAudio();
        return true;
    }
    return false;
}

bool VF50ClassicController::onTextDelete() noexcept {
    clearNotice();
    if (screen_ == ClassicScreen::DiskFileAction
        && (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename)) {
        if (diskActionConfirm_) return true;
        diskEditingName_ = true;
        diskNameFreshDefault_ = false;
        const int col = std::clamp(diskNameCursorCol_, 0, 7);
        diskNameCursorCol_ = col;
        diskEditName_[static_cast<std::size_t>(col)] = ' ';
        return true;
    }
    if (screen_ == ClassicScreen::VoiceEditFuncName) {
        if (isCompareModeLocked()) return false;
        const int col = std::clamp(voiceNameCursorCol_, 0, 9);
        voiceNameCursorCol_ = col;
        auto& slot = processor_.hostMemory().singleEditSlot();
        auto nm = slot.nameChars;
        nm[static_cast<std::size_t>(col)] = ' ';
        slot.setName(std::string_view(nm.data(), nm.size()));
        processor_.hostMemory().markSingleEditDirty();
        commitCurrentVoiceEdit(false);
        return true;
    }
    if (screen_ == ClassicScreen::PerfEditAssignMode && perfAssignCursor_ == 1) {
        if (isCompareModeLocked()) return false;
        const int col = std::clamp(perfNameCursorCol_, 0, 9);
        perfNameCursorCol_ = col;
        auto& slot = processor_.hostMemory().perfEditSlot();
        auto nm = slot.nameChars;
        nm[static_cast<std::size_t>(col)] = ' ';
        slot.setName(std::string_view(nm.data(), nm.size()));
        processor_.hostMemory().markPerfEditDirty();
        commitCurrentPerfEdit(false);
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSong && !songClearConfirm_) {
        const int col = std::clamp(songNameCursorCol_, 0, 7);
        songNameCursorCol_ = col;
        songSlots_[static_cast<std::size_t>(activeSongSlot_)].name[static_cast<std::size_t>(col)] = ' ';
        syncSeqSlotNameToAudio();
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEditJob && songEdJobSub_ == 6 && rhythmSongs_ != nullptr) {
        const int col = std::clamp(songEdNameCol_, 0, 7);
        songEdNameCol_ = col;
        (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)].name[static_cast<std::size_t>(col)] = ' ';
        processor_.publishRhythmSongsToAudio();
        return true;
    }
    if (screen_ == ClassicScreen::UtilEditMessage) {
        const int col = std::clamp(msgEditCursorCol_, 0, 39);
        msgEditCursorCol_ = col;
        auto& sys = processor_.mutableClassicSystemSettings();
        sys.powerOnMessage[static_cast<std::size_t>(col)] = ' ';
        msgMultiTapDigit_ = -1;
        processor_.publishClassicSystemStateToAudio();
        return true;
    }
    return false;
}

bool VF50ClassicController::onTextMoveCursor(int delta) noexcept {
    if (delta == 0) return false;
    const int step = (delta > 0) ? 1 : -1;
    if (screen_ == ClassicScreen::DiskFileAction
        && (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename)) {
        if (diskActionConfirm_) return true;
        diskEditingName_ = true;
        diskNameFreshDefault_ = false;
        diskNameCursorCol_ = std::clamp(std::min(7, diskNameCursorCol_) + step, 0, 7);
        return true;
    }
    if (screen_ == ClassicScreen::VoiceEditFuncName) {
        voiceNameCursorCol_ = std::clamp(std::min(9, voiceNameCursorCol_) + step, 0, 9);
        return true;
    }
    if (screen_ == ClassicScreen::PerfEditAssignMode && perfAssignCursor_ == 1) {
        perfNameCursorCol_ = std::clamp(std::min(9, perfNameCursorCol_) + step, 0, 9);
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSong && !songClearConfirm_) {
        songNameCursorCol_ = std::clamp(std::min(7, songNameCursorCol_) + step, 0, 7);
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEditJob && songEdJobSub_ == 6) {
        songEdNameCol_ = std::clamp(std::min(7, songEdNameCol_) + step, 0, 7);
        return true;
    }
    if (screen_ == ClassicScreen::UtilEditMessage) {
        msgEditCursorCol_ = std::clamp(std::min(39, msgEditCursorCol_) + step, 0, 39);
        msgMultiTapDigit_ = -1;
        return true;
    }
    return false;
}

bool VF50ClassicController::onTextConfirmEnter() noexcept {
    if (screen_ == ClassicScreen::DiskFileAction
        && (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename)) {
        if (diskActionConfirm_) {
            return onKeypadYes();
        }
        return onPressSoftKey(7); // >GO -> advances to "Are you sure ?" prompt
    }
    if (screen_ == ClassicScreen::VoiceEditFuncName) {
        return onPressSoftKey(7); // >EXIT (keeps edited voice name)
    }
    if (screen_ == ClassicScreen::PerfEditAssignMode && perfAssignCursor_ == 1) {
        perfAssignCursor_ = 0;
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSong) {
        syncSeqSlotNameToAudio();
        return onPressSoftKey(7); // >EXIT back to SeqJobSelect
    }
    if (screen_ == ClassicScreen::RhythmSongEditJob && songEdJobSub_ == 6) {
        processor_.publishRhythmSongsToAudio();
        return onPressSoftKey(7); // >EXIT back to RhythmSongEditJob root
    }
    if (screen_ == ClassicScreen::UtilEditMessage) {
        return onPressSingle();
    }
    return false;
}

bool VF50ClassicController::onTextCancelEscape() noexcept {
    clearNotice();
    if (screen_ == ClassicScreen::DiskFileAction
        && (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename)) {
        if (diskActionConfirm_) {
            diskActionConfirm_ = false;
            return true;
        }
        screen_ = ClassicScreen::DiskTypeSelect;
        return true;
    }
    if (screen_ == ClassicScreen::VoiceEditFuncName) {
        if (!isCompareModeLocked()) {
            auto& slot = processor_.hostMemory().singleEditSlot();
            slot.setName(std::string_view(voiceNameEditBackup_.data(), voiceNameEditBackup_.size()));
            processor_.hostMemory().markSingleEditDirty();
            commitCurrentVoiceEdit(false);
        }
        screen_ = ClassicScreen::VoiceEditFuncRoot;
        return true;
    }
    if (screen_ == ClassicScreen::PerfEditAssignMode && perfAssignCursor_ == 1) {
        if (!isCompareModeLocked()) {
            auto& slot = processor_.hostMemory().perfEditSlot();
            slot.setName(std::string_view(perfNameEditBackup_.data(), perfNameEditBackup_.size()));
            processor_.hostMemory().markPerfEditDirty();
            commitCurrentPerfEdit(false);
        }
        perfAssignCursor_ = 0;
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSong) {
        songClearConfirm_ = false;
        screen_ = ClassicScreen::SeqJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEditJob && songEdJobSub_ == 6) {
        return onPressSoftKey(7);
    }
    if (screen_ == ClassicScreen::UtilEditMessage) {
        return onPressSingle();
    }
    return false;
}

// =============================================================================
// Softkey Handlers (F1..F8) for Ch. 2, Ch. 3, & Ch. 6 Utilities
// =============================================================================

bool VF50ClassicController::handleEditSoftKey(int k) noexcept {
    auto& mem = processor_.hostMemory();

    // Performance Edit screens (pp. 23-35)
    if (isPerfEditScreen(screen_)) {
        if (instMinusArmed_) {
            perfInstMuteMask_ ^= static_cast<uint8_t>(1u << k);
            processor_.setClassicPerfPartMuteMask(perfInstMuteMask_);
            instMinusArmed_ = false;
            return true;
        }
        switch (screen_) {
            case ClassicScreen::PerfEditAssignMode:
                if (k <= 3) perfAssignCursor_ = 0;
                else if (k <= 5) {
                    if (perfAssignCursor_ != 1) {
                        perfNameEditBackup_ = mem.perfEditSlot().nameChars;
                    }
                    perfAssignCursor_ = 1;
                } else if (k == 6) {
                    if (perfAssignCursor_ != 1) {
                        perfNameEditBackup_ = mem.perfEditSlot().nameChars;
                    }
                    perfAssignCursor_ = 1;
                    perfNameCursorCol_ = std::max(0, std::min(9, perfNameCursorCol_) - 1);
                } else if (k == 7) {
                    if (perfAssignCursor_ != 1) {
                        perfNameEditBackup_ = mem.perfEditSlot().nameChars;
                    }
                    perfAssignCursor_ = 1;
                    perfNameCursorCol_ = std::min(9, std::min(9, perfNameCursorCol_) + 1);
                }
                return true;

            case ClassicScreen::PerfEditNotes:
            case ClassicScreen::PerfEditVoiceNo:
            case ClassicScreen::PerfEditRecvCh:
            case ClassicScreen::PerfEditLimitLow:
            case ClassicScreen::PerfEditLimitHigh:
            case ClassicScreen::PerfEditDetune:
            case ClassicScreen::PerfEditNoteShift:
            case ClassicScreen::PerfEditVolume:
            case ClassicScreen::PerfEditOutAssign:
                perfEditInst_ = k;
                return true;

            case ClassicScreen::PerfEditOthers:
                if (perfOthersSubPage_ == 0 || perfOthersSubPage_ == 1) {
                    perfEditInst_ = k;
                } else if (perfOthersSubPage_ == 2) {
                    perfMicroTableCursor_ = (k <= 4) ? 0 : 1;
                }
                return true;

            case ClassicScreen::PerfEditEffect:
                if (perfEffectSubPage_ == 0) {
                    perfEditInst_ = k;
                } else if (perfEffectSubPage_ == 1) {
                    perfEffectCursor_ = (k <= 3) ? 0 : 1;
                } else if (perfEffectSubPage_ == 2) {
                    perfEffectCursor_ = (k <= 3) ? 0 : 1;
                } else {
                    if (k <= 3) perfEffectCursor_ = 0;
                    else if (k <= 5) perfEffectCursor_ = 1;
                    else perfEffectCursor_ = 2;
                }
                return true;

            default:
                return false;
        }
    }

    // Voice Edit screens (pp. 36-56)
    switch (screen_) {
        case ClassicScreen::VoiceEditQuick:
            return true;
        case ClassicScreen::VoiceEditAlgorithm:
            voiceAlgCursor_ = (k <= 3) ? 0 : 1;
            return true;
        case ClassicScreen::VoiceEditLfo:
            if (k <= 2) voiceLfoCursor_ = 0;
            else if (k == 3) voiceLfoCursor_ = 1;
            else if (k == 4) voiceLfoCursor_ = 2;
            else if (k == 5) voiceLfoCursor_ = 3;
            else if (k == 6) voiceLfoCursor_ = 4;
            else voiceLfoCursor_ = 5;
            return true;
        case ClassicScreen::VoiceEditSens:
            if (k <= 3) voiceSensCursor_ = 0;
            else if (k == 4) voiceSensCursor_ = 1;
            else if (k == 5) voiceSensCursor_ = 2;
            else if (k == 6) voiceSensCursor_ = 3;
            else voiceSensCursor_ = 4;
            return true;
        case ClassicScreen::VoiceEditOsc: {
            const bool isFix = mem.singleEditSlot().voice.operators[static_cast<std::size_t>(selectedOperator_)].fixedFreqMode != 0u;
            if (!isFix) {
                if (k <= 3) voiceOscCursor_ = 0;
                else if (k == 4) voiceOscCursor_ = 3;
                else if (k == 5) voiceOscCursor_ = 4;
                else if (k == 6) voiceOscCursor_ = 5;
                else voiceOscCursor_ = 6;
            } else {
                if (k <= 1) voiceOscCursor_ = 0;
                else if (k == 2) voiceOscCursor_ = 1;
                else if (k == 3) voiceOscCursor_ = 2;
                else if (k == 4) voiceOscCursor_ = 3;
                else if (k == 5) voiceOscCursor_ = 4;
                else if (k == 6) voiceOscCursor_ = 5;
                else voiceOscCursor_ = 6;
            }
            return true;
        }
        case ClassicScreen::VoiceEditEg:
            if (k <= 2) voiceEgCursor_ = 0;
            else if (k == 3) voiceEgCursor_ = 1;
            else if (k == 4) voiceEgCursor_ = 2;
            else if (k == 5) voiceEgCursor_ = 3;
            else if (k == 6) voiceEgCursor_ = 4;
            else voiceEgCursor_ = 5;
            return true;
        case ClassicScreen::VoiceEditPeg:
            voicePegCursor_ = std::clamp(k - 2, 0, 5);
            return true;
        case ClassicScreen::VoiceEditOutLevel:
            if (k >= 3 && k <= 6) voiceOutCursor_ = k - 3;
            else if (k < 3) voiceOutCursor_ = 0;
            else voiceOutCursor_ = 3;
            return true;
        case ClassicScreen::VoiceEditScaling:
            if (k >= 3 && k <= 6) voiceScalingCursor_ = k - 3;
            else if (k < 3) voiceScalingCursor_ = 0;
            else voiceScalingCursor_ = 3;
            return true;
        case ClassicScreen::VoiceEditTranspose:
            voiceTransposeCursor_ = (k <= 5) ? 0 : 1;
            return true;
        case ClassicScreen::VoiceEditFuncRoot:
            voiceFuncCursor_ = 0;
            switch (k) {
                case 0: screen_ = ClassicScreen::VoiceEditFuncPbEtc; break;
                case 1: screen_ = ClassicScreen::VoiceEditFuncPort; break;
                case 2: screen_ = ClassicScreen::VoiceEditFuncFc; break;
                case 3: screen_ = ClassicScreen::VoiceEditFuncMw; break;
                case 4: screen_ = ClassicScreen::VoiceEditFuncBc; break;
                case 5: screen_ = ClassicScreen::VoiceEditFuncAt; break;
                case 6: screen_ = ClassicScreen::VoiceEditFuncRevr; break;
                case 7:
                    voiceNameEditBackup_ = mem.singleEditSlot().nameChars;
                    voiceNameCursorCol_ = 0;
                    screen_ = ClassicScreen::VoiceEditFuncName;
                    break;
            }
            return true;
        case ClassicScreen::VoiceEditFuncPbEtc:
            if (k == 7) { screen_ = ClassicScreen::VoiceEditFuncRoot; return true; }
            if (k <= 3) voiceFuncCursor_ = 0;
            else if (k == 4) voiceFuncCursor_ = 1;
            else voiceFuncCursor_ = 2;
            return true;
        case ClassicScreen::VoiceEditFuncPort:
            if (k == 7) { screen_ = ClassicScreen::VoiceEditFuncRoot; return true; }
            voiceFuncCursor_ = (k <= 3) ? 0 : 1;
            return true;
        case ClassicScreen::VoiceEditFuncFc:
            if (k == 7) { screen_ = ClassicScreen::VoiceEditFuncRoot; return true; }
            if (k <= 2) voiceFuncCursor_ = 0;
            else if (k <= 4) voiceFuncCursor_ = 1;
            else voiceFuncCursor_ = 2;
            return true;
        case ClassicScreen::VoiceEditFuncMw:
            if (k == 7) { screen_ = ClassicScreen::VoiceEditFuncRoot; return true; }
            voiceFuncCursor_ = (k <= 3) ? 0 : 1;
            return true;
        case ClassicScreen::VoiceEditFuncBc:
        case ClassicScreen::VoiceEditFuncAt:
            if (k == 7) { screen_ = ClassicScreen::VoiceEditFuncRoot; return true; }
            if (k <= 2) voiceFuncCursor_ = 0;
            else if (k == 3) voiceFuncCursor_ = 1;
            else if (k == 4) voiceFuncCursor_ = 2;
            else voiceFuncCursor_ = 3;
            return true;
        case ClassicScreen::VoiceEditFuncRevr:
            if (k == 7) { screen_ = ClassicScreen::VoiceEditFuncRoot; return true; }
            voiceFuncCursor_ = 0;
            return true;
        case ClassicScreen::VoiceEditFuncName:
            if (k == 7) { screen_ = ClassicScreen::VoiceEditFuncRoot; return true; }
            if (k == 5) voiceNameCursorCol_ = std::max(0, std::min(9, voiceNameCursorCol_) - 1);
            else if (k == 6) voiceNameCursorCol_ = std::min(9, std::min(9, voiceNameCursorCol_) + 1);
            return true;
        case ClassicScreen::VoiceEditEffect:
            if (voiceEffectSubPage_ == 0 || voiceEffectSubPage_ == 1) {
                voiceEffectCursor_ = (k <= 3) ? 0 : 1;
            } else {
                if (k <= 3) voiceEffectCursor_ = 0;
                else if (k <= 5) voiceEffectCursor_ = 1;
                else voiceEffectCursor_ = 2;
            }
            return true;

        // Chapter 6 Utilities (pp. 117-120)
        case ClassicScreen::UtilOthersVoice:
            if (utilConfirmPending_) return false;
            if (k == 0) { utilPendingAction_ = 1; utilConfirmPending_ = true; }
            else if (k == 1) {
                if (!mem.hasSingleRecallBuffer()) { setNotice("Recall Buffer Empty", 45); return true; }
                utilPendingAction_ = 2; utilConfirmPending_ = true;
            } else if (k >= 5) {
                screen_ = ClassicScreen::UtilOthersPresetLoad;
            }
            return true;

        case ClassicScreen::UtilOthersPerf:
            if (utilConfirmPending_) return false;
            if (k == 0) {
                screen_ = ClassicScreen::UtilOthersPerfInit;
            } else if (k == 1) {
                if (!mem.hasPerfRecallBuffer()) { setNotice("Recall Buffer Empty", 45); return true; }
                utilPendingAction_ = 4; utilConfirmPending_ = true;
            } else if (k == 3 || k == 4) {
                multiVoiceDirty_.fill(false);
                screen_ = ClassicScreen::UtilOthersMultiVoiceSelect;
            } else if (k >= 5) {
                screen_ = ClassicScreen::UtilOthersPresetLoad;
            }
            return true;

        case ClassicScreen::UtilOthersPerfInit:
            if (utilConfirmPending_) return false;
            if (k == 7) {
                screen_ = ClassicScreen::UtilOthersPerf;
            } else {
                utilPendingAction_ = 3;
                utilPendingSubArg_ = k; // 0=SNGL,1=DUAL,2=SPLT,3=4LYR,4=8LYR,5=SEQ4,6=SEQ8
                utilConfirmPending_ = true;
            }
            return true;

        case ClassicScreen::UtilOthersPresetLoad:
            if (utilConfirmPending_) return false;
            if (k == 7) {
                screen_ = isPerformanceContextScreen() ? ClassicScreen::UtilOthersPerf : ClassicScreen::UtilOthersVoice;
            } else if (k <= 1) {
                utilPendingAction_ = 5; utilPendingSubArg_ = 0; utilConfirmPending_ = true; // ALL
            } else if (k == 2) {
                utilPendingAction_ = 5; utilPendingSubArg_ = 1; utilConfirmPending_ = true; // PEFCT
            } else if (k == 3) {
                utilPendingAction_ = 5; utilPendingSubArg_ = 2; utilConfirmPending_ = true; // MCT
            }
            return true;

        case ClassicScreen::UtilOthersMultiVoiceSelect:
            if (multiVoiceExitSglConfirm_) return false;
            enterMultiVoiceSlot(k);
            return true;

        case ClassicScreen::StoreVoice:
        case ClassicScreen::StorePerf:
            if (k == 7 && !storeTargetCard_ && processor_.isIntMemoryProtected()) {
                // Spec §4.3 (p. 119): pressing softkey below "Prot?" temporarily defeats internal protect for this store
                storeIntProtBypassed_ = true;
                storeConfirmReady_ = true;
                return true;
            }
            if (k == 6) {
                storeConfirmReady_ = true;
                return true;
            }
            return true;

        case ClassicScreen::StoreMultiVoice:
            saveActiveMultiVoiceBackToSlot();
            if (isPerfInstActive(mem.perfEditSlot().perf, k)) {
                multiVoiceEditInst_ = k;
                storeTargetSlot_ = static_cast<int>(mem.perfEditSlot().perf.instruments[static_cast<std::size_t>(k)].voiceNumber);
                storeConfirmReady_ = true;
            } else {
                setNotice("Slot Inactive (*)", 45);
            }
            return true;

        case ClassicScreen::CopyEffect:
            if (k == 7 && !storeTargetCard_ && processor_.isIntMemoryProtected()) {
                storeIntProtBypassed_ = true;
                storeConfirmReady_ = true;
            } else if (k == 6) {
                storeConfirmReady_ = true;
            }
            return true;

        case ClassicScreen::CopyEg:
            copyEgCursor_ = (k <= 4) ? 0 : 1;
            return true;

        default:
            return false;
    }
}

// =============================================================================
// Parameter Stepping (+1 / -1 / Arrows)
// =============================================================================

bool VF50ClassicController::handleEditStepValue(int deltaSteps) noexcept {
    const int sign = (deltaSteps > 0) ? 1 : -1;
    auto& mem = processor_.hostMemory();

    if (isCompareModeLocked() && (isPerfEditScreen(screen_) || isVoiceEditScreen(screen_))) {
        setNotice("Compare Active - Locked", 45);
        return false;
    }

    if (isPerfEditScreen(screen_)) {
        auto& pfSlot = mem.perfEditSlot();
        auto& pf = pfSlot.perf;
        auto& inst = pf.instruments[static_cast<std::size_t>(perfEditInst_)];
        const bool active = isPerfInstActive(pf, perfEditInst_);

        switch (screen_) {
            case ClassicScreen::PerfEditAssignMode:
                if (perfAssignCursor_ == 0) {
                    const uint8_t prevMode = pf.assignMode;
                    const uint8_t nextMode = static_cast<uint8_t>(std::clamp(static_cast<int>(prevMode) + sign, 0, 2));
                    if (nextMode != prevMode) {
                        pf.assignMode = nextMode;
                        // Spec §1.1 (p. 24): Changing Assign Mode resets Max Notes / Reserved Notes to match active instruments
                        int budget = 16;
                        for (int i = 0; i < 8; ++i) {
                            auto& it = pf.instruments[static_cast<std::size_t>(i)];
                            if (nextMode == 2u) {
                                // To DVA
                                if (it.maxNotes == 0u) {
                                    it.reserveNotes = 0u;
                                } else {
                                    const int res = std::min<int>(it.maxNotes, budget);
                                    it.reserveNotes = static_cast<uint8_t>(1 + res);
                                    budget -= res;
                                }
                            } else {
                                // From DVA (or normal<->alt)
                                const bool wasOn = (prevMode == 2u) ? (it.reserveNotes > 0u || it.maxNotes > 0u) : (it.maxNotes > 0u);
                                if (!wasOn) {
                                    it.maxNotes = 0u;
                                } else {
                                    const int want = (it.maxNotes > 0u) ? static_cast<int>(it.maxNotes)
                                                                        : std::max(1, static_cast<int>(it.reserveNotes) - 1);
                                    const int alloc = std::min(want, budget);
                                    it.maxNotes = static_cast<uint8_t>(alloc);
                                    budget -= alloc;
                                }
                            }
                        }
                    }
                } else {
                    auto nm = pfSlot.nameChars;
                    const int col = std::clamp(perfNameCursorCol_, 0, 9);
                    int ch = static_cast<unsigned char>(nm[static_cast<std::size_t>(col)]);
                    ch = std::clamp(ch + sign, 0x20, 0x7E);
                    nm[static_cast<std::size_t>(col)] = static_cast<char>(ch);
                    pfSlot.setName(std::string_view(nm.data(), nm.size()));
                }
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditNotes:
                if (pf.assignMode == 2u) {
                    // DVA Reserved Notes: 0=off, 1=0 reserved, 2..17=1..16 reserved (sum reserved <= 16)
                    int usedOther = 0;
                    for (int i = 0; i < 8; ++i) {
                        if (i != perfEditInst_ && pf.instruments[static_cast<std::size_t>(i)].reserveNotes > 1u) {
                            usedOther += static_cast<int>(pf.instruments[static_cast<std::size_t>(i)].reserveNotes) - 1;
                        }
                    }
                    const int maxAllowedRaw = 1 + std::max(0, 16 - usedOther);
                    const int cur = static_cast<int>(inst.reserveNotes);
                    const int next = std::clamp(cur + deltaSteps, 0, maxAllowedRaw);
                    inst.reserveNotes = static_cast<uint8_t>(next);
                    if (next == 0) {
                        inst.maxNotes = 0u;
                    } else {
                        if (inst.maxNotes == 0u) inst.maxNotes = 1u;
                        if (inst.limitLow == 0u && inst.limitHigh == 0u && inst.outAssign == 0u) {
                            inst.limitHigh = 127u;
                            inst.outAssign = 3u;
                            if (inst.volume == 0u) inst.volume = 90u;
                        }
                    }
                } else {
                    int usedOther = 0;
                    for (int i = 0; i < 8; ++i) {
                        if (i != perfEditInst_) usedOther += static_cast<int>(pf.instruments[static_cast<std::size_t>(i)].maxNotes);
                    }
                    const int maxAllowed = std::max(0, 16 - usedOther);
                    const int next = std::clamp(static_cast<int>(inst.maxNotes) + deltaSteps, 0, maxAllowed);
                    inst.maxNotes = static_cast<uint8_t>(next);
                    if (next > 0 && inst.limitLow == 0u && inst.limitHigh == 0u && inst.outAssign == 0u) {
                        inst.limitHigh = 127u;
                        inst.outAssign = 3u;
                        if (inst.volume == 0u) inst.volume = 90u;
                    }
                }
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditVoiceNo:
                if (!active) return false;
                inst.voiceNumber = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.voiceNumber) + deltaSteps, 0, 99));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditRecvCh:
                if (!active) return false;
                inst.receiveChannel = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.receiveChannel) + deltaSteps, 0, 16));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditLimitLow:
                if (!active) return false;
                inst.limitLow = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.limitLow) + deltaSteps, 0, static_cast<int>(inst.limitHigh)));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditLimitHigh:
                if (!active) return false;
                inst.limitHigh = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.limitHigh) + deltaSteps, static_cast<int>(inst.limitLow), 127));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditDetune:
                if (!active) return false;
                inst.detune = static_cast<int8_t>(std::clamp(static_cast<int>(inst.detune) + deltaSteps, -7, 7));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditNoteShift:
                if (!active) return false;
                inst.noteShift = static_cast<int8_t>(std::clamp(static_cast<int>(inst.noteShift) + deltaSteps, -24, 24));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditVolume:
                if (!active) return false;
                inst.volume = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.volume) + deltaSteps, 0, 99));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditOutAssign:
                if (!active) return false;
                inst.outAssign = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.outAssign) + sign, 0, 3));
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditOthers:
                if (perfOthersSubPage_ == 0) {
                    if (!active) return false;
                    inst.lfoSelect = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.lfoSelect) + sign, 0, 3));
                } else if (perfOthersSubPage_ == 1) {
                    if (!active) return false;
                    inst.microTuneEnable = (sign > 0) ? 1u : 0u;
                } else if (perfOthersSubPage_ == 2) {
                    if (perfMicroTableCursor_ == 0) {
                        pf.microTuneTable = static_cast<uint8_t>(std::clamp(static_cast<int>(pf.microTuneTable) + sign, 0, 12));
                    } else if (pf.microTuneTable >= 1u && pf.microTuneTable <= 4u) {
                        pf.microTuneRootKey = static_cast<uint8_t>(std::clamp(static_cast<int>(pf.microTuneRootKey) + sign, 0, 11));
                    }
                } else {
                    const int curUi = perfFxEngineToUi(pf.perfEffectSelect);
                    const int nextUi = std::clamp(curUi + sign, 0, 12);
                    pf.perfEffectSelect = kPerfFxUiToEngine[static_cast<std::size_t>(nextUi)];
                }
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(false);
                return true;

            case ClassicScreen::PerfEditEffect:
                if (perfEffectSubPage_ == 0) {
                    if (!active) return false;
                    inst.effectEnable = (sign > 0) ? 1u : 0u;
                } else if (perfEffectSubPage_ == 1) {
                    if (perfEffectCursor_ == 0) {
                        pf.effect.effectType = static_cast<uint8_t>(std::clamp(static_cast<int>(pf.effect.effectType) + sign, 0, 32));
                    } else {
                        pf.effect.balance = static_cast<uint8_t>(std::clamp(static_cast<int>(pf.effect.balance) + deltaSteps, 0, 100));
                    }
                } else if (perfEffectSubPage_ == 2) {
                    if (perfEffectCursor_ == 0) {
                        pf.effect.outLevel = static_cast<uint8_t>(std::clamp(static_cast<int>(pf.effect.outLevel) + deltaSteps, 0, 100));
                    } else {
                        pf.effect.stereoMix = (sign > 0) ? 1u : 0u;
                    }
                } else {
                    const auto& spec = kDspEffectSpecs[std::min<std::size_t>(pf.effect.effectType, 32u)];
                    const int pIdx = std::clamp(perfEffectCursor_, 0, 2);
                    const int maxR = std::max(0, spec.params[static_cast<std::size_t>(pIdx)].maxRaw);
                    auto& rawRef = fxRawParamRef(pf.effect, pIdx);
                    rawRef = static_cast<uint8_t>(std::clamp(static_cast<int>(rawRef) + deltaSteps, 0, maxR));
                }
                mem.markPerfEditDirty();
                commitCurrentPerfEdit(true);
                return true;

            default:
                return false;
        }
    }

    // Chapter 3 — Voice Edit stepping
    if (isVoiceEditScreen(screen_)) {
        auto& vSlot = mem.singleEditSlot();
        auto& v = vSlot.voice;
        auto& op = v.operators[static_cast<std::size_t>(selectedOperator_)];

        switch (screen_) {
            case ClassicScreen::VoiceEditQuick:
                applyQuickEditOffset(quickEditRelOffset_ + deltaSteps);
                return true;

            case ClassicScreen::VoiceEditAlgorithm:
                if (voiceAlgCursor_ == 0) v.algorithm = static_cast<uint8_t>(std::clamp(static_cast<int>(v.algorithm) + sign, 0, 7));
                else                      v.feedback  = static_cast<uint8_t>(std::clamp(static_cast<int>(v.feedback) + sign, 0, 7));
                break;

            case ClassicScreen::VoiceEditLfo:
                if (voiceLfoCursor_ == 0)      v.lfoWave  = static_cast<uint8_t>(std::clamp(static_cast<int>(v.lfoWave) + sign, 0, 3));
                else if (voiceLfoCursor_ == 1) v.lfoSpeed = static_cast<uint8_t>(std::clamp(static_cast<int>(v.lfoSpeed) + deltaSteps, 0, 99));
                else if (voiceLfoCursor_ == 2) v.lfoDelay = static_cast<uint8_t>(std::clamp(static_cast<int>(v.lfoDelay) + deltaSteps, 0, 99));
                else if (voiceLfoCursor_ == 3) v.lfoSync  = (sign > 0) ? 1u : 0u;
                else if (voiceLfoCursor_ == 4) v.pmd      = static_cast<uint8_t>(std::clamp(static_cast<int>(v.pmd) + deltaSteps, 0, 99));
                else                           v.amd      = static_cast<uint8_t>(std::clamp(static_cast<int>(v.amd) + deltaSteps, 0, 99));
                break;

            case ClassicScreen::VoiceEditSens:
                if (voiceSensCursor_ == 0)      v.pms           = static_cast<uint8_t>(std::clamp(static_cast<int>(v.pms) + sign, 0, 7));
                else if (voiceSensCursor_ == 1) v.ams           = static_cast<uint8_t>(std::clamp(static_cast<int>(v.ams) + sign, 0, 3));
                else if (voiceSensCursor_ == 2) op.ampModEnable = (sign > 0) ? 1u : 0u;
                else if (voiceSensCursor_ == 3) op.egBiasSens   = static_cast<uint8_t>(std::clamp(static_cast<int>(op.egBiasSens) + sign, 0, 7));
                else                            op.keyVelSens   = static_cast<int8_t>(std::clamp(static_cast<int>(op.keyVelSens) + sign, -7, 7));
                break;

            case ClassicScreen::VoiceEditOsc:
                if (voiceOscCursor_ == 0)      op.fixedFreqMode = (sign > 0) ? 1u : 0u;
                else if (voiceOscCursor_ == 1) op.fixedRangeLow = (sign > 0) ? 1u : 0u;
                else if (voiceOscCursor_ == 2) op.fixedRange    = static_cast<uint8_t>(std::clamp(static_cast<int>(op.fixedRange) + sign, 0, 7));
                else if (voiceOscCursor_ == 3) op.coarse        = static_cast<uint8_t>(std::clamp(static_cast<int>(op.coarse) + deltaSteps, 0, 63));
                else if (voiceOscCursor_ == 4) op.fine          = static_cast<uint8_t>(std::clamp(static_cast<int>(op.fine) + sign, 0, 15));
                else if (voiceOscCursor_ == 5) op.waveform      = static_cast<uint8_t>(std::clamp(static_cast<int>(op.waveform) + sign, 0, 7));
                else                           op.detune        = static_cast<int8_t>(std::clamp(static_cast<int>(op.detune) + sign, -3, 3));
                break;

            case ClassicScreen::VoiceEditEg:
                if (voiceEgCursor_ == 0)      op.ar  = static_cast<uint8_t>(std::clamp(static_cast<int>(op.ar) + deltaSteps, 0, 31));
                else if (voiceEgCursor_ == 1) op.d1r = static_cast<uint8_t>(std::clamp(static_cast<int>(op.d1r) + deltaSteps, 0, 31));
                else if (voiceEgCursor_ == 2) op.d1l = static_cast<uint8_t>(std::clamp(static_cast<int>(op.d1l) + sign, 0, 15));
                else if (voiceEgCursor_ == 3) op.d2r = static_cast<uint8_t>(std::clamp(static_cast<int>(op.d2r) + deltaSteps, 0, 31));
                else if (voiceEgCursor_ == 4) op.rr  = static_cast<uint8_t>(std::clamp(static_cast<int>(op.rr) + sign, 1, 15));
                else {
                    // Spec §2.6 (p. 47): OP1 SHFT is permanently locked to "off" (0)
                    if (selectedOperator_ == 0) {
                        op.egShift = 0u;
                        setNotice("OP1 Shift Locked off", 45);
                        return false;
                    }
                    op.egShift = static_cast<uint8_t>(std::clamp(static_cast<int>(op.egShift) + sign, 0, 3));
                }
                break;

            case ClassicScreen::VoiceEditPeg:
                if (voicePegCursor_ == 0)      v.peg.pr1 = static_cast<uint8_t>(std::clamp(static_cast<int>(v.peg.pr1) + deltaSteps, 0, 99));
                else if (voicePegCursor_ == 1) v.peg.pl1 = static_cast<uint8_t>(std::clamp(static_cast<int>(v.peg.pl1) + deltaSteps, 0, 99));
                else if (voicePegCursor_ == 2) v.peg.pr2 = static_cast<uint8_t>(std::clamp(static_cast<int>(v.peg.pr2) + deltaSteps, 0, 99));
                else if (voicePegCursor_ == 3) v.peg.pl2 = static_cast<uint8_t>(std::clamp(static_cast<int>(v.peg.pl2) + deltaSteps, 0, 99));
                else if (voicePegCursor_ == 4) v.peg.pr3 = static_cast<uint8_t>(std::clamp(static_cast<int>(v.peg.pr3) + deltaSteps, 0, 99));
                else                           v.peg.pl3 = static_cast<uint8_t>(std::clamp(static_cast<int>(v.peg.pl3) + deltaSteps, 0, 99));
                break;

            case ClassicScreen::VoiceEditOutLevel:
                v.operators[static_cast<std::size_t>(voiceOutCursor_)].outputLevel =
                    static_cast<uint8_t>(std::clamp(static_cast<int>(v.operators[static_cast<std::size_t>(voiceOutCursor_)].outputLevel) + deltaSteps, 0, 99));
                break;

            case ClassicScreen::VoiceEditScaling:
                if (voiceScalingSubPage_ == 0) {
                    v.operators[static_cast<std::size_t>(voiceScalingCursor_)].rateScaling =
                        static_cast<uint8_t>(std::clamp(static_cast<int>(v.operators[static_cast<std::size_t>(voiceScalingCursor_)].rateScaling) + sign, 0, 3));
                } else {
                    v.operators[static_cast<std::size_t>(voiceScalingCursor_)].levelScaling =
                        static_cast<int8_t>(std::clamp(static_cast<int>(v.operators[static_cast<std::size_t>(voiceScalingCursor_)].levelScaling) + deltaSteps, -99, 99));
                }
                break;

            case ClassicScreen::VoiceEditTranspose:
                if (voiceTransposeCursor_ == 0) v.transpose = static_cast<int8_t>(std::clamp(static_cast<int>(v.transpose) + sign, -24, 24));
                else                            voiceTransposeKbdIn_ = (sign > 0);
                break;

            case ClassicScreen::VoiceEditFuncPbEtc:
                if (voiceFuncCursor_ == 0) {
                    v.monoMode = (sign > 0) ? 1u : 0u;
                    if (v.monoMode == 0u) v.portaMode = 0u; // Poly locks portaMode to Full Time Porta
                } else if (voiceFuncCursor_ == 1) {
                    v.pitchBendRange = static_cast<uint8_t>(std::clamp(static_cast<int>(v.pitchBendRange) + sign, 0, 12));
                } else {
                    v.sustainSwitch = (sign > 0) ? 1u : 0u;
                }
                break;

            case ClassicScreen::VoiceEditFuncPort:
                if (voiceFuncCursor_ == 0) {
                    if (v.monoMode == 0u) {
                        v.portaMode = 0u; // Spec §2.8 (p. 50): Full Time Porta only when poly
                    } else {
                        v.portaMode = (sign > 0) ? 1u : 0u;
                    }
                } else {
                    v.portaTime = static_cast<uint8_t>(std::clamp(static_cast<int>(v.portaTime) + deltaSteps, 0, 99));
                    v.portaSwitch = (v.portaTime > 0u) ? 1u : 0u;
                }
                break;

            case ClassicScreen::VoiceEditFuncFc:
                if (voiceFuncCursor_ == 0)      v.fcVolume = static_cast<uint8_t>(std::clamp(static_cast<int>(v.fcVolume) + deltaSteps, 0, 99));
                else if (voiceFuncCursor_ == 1) v.fcPitch  = static_cast<uint8_t>(std::clamp(static_cast<int>(v.fcPitch) + deltaSteps, 0, 99));
                else                            v.fcAmp    = static_cast<uint8_t>(std::clamp(static_cast<int>(v.fcAmp) + deltaSteps, 0, 99));
                break;

            case ClassicScreen::VoiceEditFuncMw:
                if (voiceFuncCursor_ == 0) v.mwPitch = static_cast<uint8_t>(std::clamp(static_cast<int>(v.mwPitch) + deltaSteps, 0, 99));
                else                       v.mwAmp   = static_cast<uint8_t>(std::clamp(static_cast<int>(v.mwAmp) + deltaSteps, 0, 99));
                break;

            case ClassicScreen::VoiceEditFuncBc:
                if (voiceFuncCursor_ == 0)      v.bcPitch     = static_cast<uint8_t>(std::clamp(static_cast<int>(v.bcPitch) + deltaSteps, 0, 99));
                else if (voiceFuncCursor_ == 1) v.bcAmp       = static_cast<uint8_t>(std::clamp(static_cast<int>(v.bcAmp) + deltaSteps, 0, 99));
                else if (voiceFuncCursor_ == 2) v.bcPitchBias = static_cast<int8_t>(std::clamp(static_cast<int>(v.bcPitchBias) + deltaSteps, -50, 50));
                else                            v.bcEgBias    = static_cast<uint8_t>(std::clamp(static_cast<int>(v.bcEgBias) + deltaSteps, 0, 99));
                break;

            case ClassicScreen::VoiceEditFuncAt:
                if (voiceFuncCursor_ == 0)      v.atPitch     = static_cast<uint8_t>(std::clamp(static_cast<int>(v.atPitch) + deltaSteps, 0, 99));
                else if (voiceFuncCursor_ == 1) v.atAmp       = static_cast<uint8_t>(std::clamp(static_cast<int>(v.atAmp) + deltaSteps, 0, 99));
                else if (voiceFuncCursor_ == 2) v.atPitchBias = static_cast<int8_t>(std::clamp(static_cast<int>(v.atPitchBias) + deltaSteps, -50, 50));
                else                            v.atEgBias    = static_cast<uint8_t>(std::clamp(static_cast<int>(v.atEgBias) + deltaSteps, 0, 99));
                break;

            case ClassicScreen::VoiceEditFuncRevr:
                v.reverbRate = static_cast<uint8_t>(std::clamp(static_cast<int>(v.reverbRate) + sign, 0, 7));
                break;

            case ClassicScreen::VoiceEditFuncName: {
                auto nm = vSlot.nameChars;
                const int col = std::clamp(voiceNameCursorCol_, 0, 9);
                int ch = static_cast<unsigned char>(nm[static_cast<std::size_t>(col)]);
                ch = std::clamp(ch + sign, 0x20, 0x7E);
                nm[static_cast<std::size_t>(col)] = static_cast<char>(ch);
                vSlot.setName(std::string_view(nm.data(), nm.size()));
                break;
            }

            case ClassicScreen::VoiceEditEffect:
                if (voiceEffectSubPage_ == 0) {
                    if (voiceEffectCursor_ == 0) {
                        v.effect.effectType = static_cast<uint8_t>(std::clamp(static_cast<int>(v.effect.effectType) + sign, 0, 32));
                    } else {
                        v.effect.balance = static_cast<uint8_t>(std::clamp(static_cast<int>(v.effect.balance) + deltaSteps, 0, 100));
                    }
                } else if (voiceEffectSubPage_ == 1) {
                    if (voiceEffectCursor_ == 0) {
                        v.effect.outLevel = static_cast<uint8_t>(std::clamp(static_cast<int>(v.effect.outLevel) + deltaSteps, 0, 100));
                    } else {
                        v.effect.stereoMix = (sign > 0) ? 1u : 0u;
                    }
                } else {
                    const auto& spec = kDspEffectSpecs[std::min<std::size_t>(v.effect.effectType, 32u)];
                    const int pIdx = std::clamp(voiceEffectCursor_, 0, 2);
                    const int maxR = std::max(0, spec.params[static_cast<std::size_t>(pIdx)].maxRaw);
                    auto& rawRef = fxRawParamRef(v.effect, pIdx);
                    rawRef = static_cast<uint8_t>(std::clamp(static_cast<int>(rawRef) + deltaSteps, 0, maxR));
                }
                mem.markSingleEditDirty();
                commitCurrentVoiceEdit(true);
                return true;

            default:
                return false;
        }

        mem.markSingleEditDirty();
        commitCurrentVoiceEdit(false);
        return true;
    }

    // Store & Copy stepping
    if (screen_ == ClassicScreen::StoreVoice || screen_ == ClassicScreen::StorePerf
        || screen_ == ClassicScreen::StoreMultiVoice || screen_ == ClassicScreen::CopyEffect) {
        storeTargetSlot_ = std::clamp(storeTargetSlot_ + deltaSteps, 0, 99);
        storePendingTens_ = -1;
        return true;
    }
    if (screen_ == ClassicScreen::CopyPerfEffect) {
        copyPerfFxDstSlot_ = std::clamp(copyPerfFxDstSlot_ + sign, 0, 3);
        return true;
    }
    if (screen_ == ClassicScreen::CopyEg) {
        if (copyEgCursor_ == 0) copyEgSrcOp_ = std::clamp(((copyEgSrcOp_ < 0) ? 0 : copyEgSrcOp_) + sign, 0, 3);
        else                    copyEgDstOp_ = std::clamp(((copyEgDstOp_ < 0) ? 0 : copyEgDstOp_) + sign, 0, 3);
        return true;
    }
    return false;
}

bool VF50ClassicController::handleEditDigit(int d) noexcept {
    if (screen_ == ClassicScreen::StoreVoice || screen_ == ClassicScreen::StorePerf
        || screen_ == ClassicScreen::StoreMultiVoice || screen_ == ClassicScreen::CopyEffect) {
        if (storePendingTens_ < 0) {
            storePendingTens_ = d;
        } else {
            storeTargetSlot_ = storePendingTens_ * 10 + d;
            storePendingTens_ = -1;
            storeConfirmReady_ = true;
        }
        return true;
    }
    if (screen_ == ClassicScreen::CopyPerfEffect) {
        if (d >= 1 && d <= 4) {
            copyPerfFxDstSlot_ = d - 1;
            copyConfirmReady_ = true;
            return true;
        }
        return false;
    }
    if (screen_ == ClassicScreen::CopyEg) {
        if (d >= 1 && d <= 4) {
            if (copyEgCursor_ == 0) {
                copyEgSrcOp_ = d - 1;
                copyEgCursor_ = 1;
            } else {
                copyEgDstOp_ = d - 1;
                copyConfirmReady_ = true;
            }
            return true;
        }
        return false;
    }
    if (screen_ == ClassicScreen::PerfEditVoiceNo) {
        if (isCompareModeLocked()) {
            setNotice("Compare Active - Locked", 45);
            return false;
        }
        auto& pf = processor_.hostMemory().perfEditSlot().perf;
        if (!isPerfInstActive(pf, perfEditInst_)) return false;
        if (perfVoiceEditPendingTens_ < 0) {
            perfVoiceEditPendingTens_ = d;
        } else {
            pf.instruments[static_cast<std::size_t>(perfEditInst_)].voiceNumber =
                static_cast<uint8_t>(perfVoiceEditPendingTens_ * 10 + d);
            perfVoiceEditPendingTens_ = -1;
            processor_.hostMemory().markPerfEditDirty();
            commitCurrentPerfEdit(false);
        }
        return true;
    }
    return false;
}

bool VF50ClassicController::handleEditYes() noexcept {
    auto& mem = processor_.hostMemory();

    // Multi-Voice Edit cancel confirmation ("** go to SGL mode Sure?")
    if (screen_ == ClassicScreen::UtilOthersMultiVoiceSelect && multiVoiceExitSglConfirm_) {
        multiVoiceExitSglConfirm_ = false;
        multiVoiceEditActive_ = false;
        multiVoiceDirty_.fill(false);
        mem.setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
        processor_.stateMachine().setClassicTopMode(vf50::state::ClassicTopMode::MODE_VOICE);
        processor_.selectSinglePresetAtomic(mem.selectedVoiceBank(), mem.selectedVoiceIndex(), true);
        screen_ = ClassicScreen::PlaySingle;
        return true;
    }

    // Utility OTHERS confirmation (Init / Recall / PresetLoad)
    if (isUtilOthersScreen(screen_) && utilConfirmPending_) {
        utilConfirmPending_ = false;
        if (utilPendingAction_ == 1) {
            mem.initSingleVoiceEditBuffer();
            combineBorrowedFunction_ = false;
            commitCurrentVoiceEdit(true);
            setNotice("Completed !", 45);
            screen_ = ClassicScreen::PlaySingle;
            return true;
        }
        if (utilPendingAction_ == 2) {
            (void)mem.recallSingleVoiceEditBuffer();
            combineBorrowedFunction_ = false;
            commitCurrentVoiceEdit(true);
            setNotice("Completed !", 45);
            screen_ = ClassicScreen::PlaySingle;
            return true;
        }
        if (utilPendingAction_ == 3) {
            static constexpr std::array<vf50::state::PerfInitTemplate, 7> kInitMap = {
                vf50::state::PerfInitTemplate::Single, // F1 SNGL
                vf50::state::PerfInitTemplate::Dual,   // F2 DUAL
                vf50::state::PerfInitTemplate::Split,  // F3 SPLT
                vf50::state::PerfInitTemplate::Layer4, // F4 4LYR
                vf50::state::PerfInitTemplate::Layer8, // F5 8LYR
                vf50::state::PerfInitTemplate::MCh4x4, // F6 SEQ4
                vf50::state::PerfInitTemplate::MCh8x2  // F7 SEQ8
            };
            const auto tmpl = kInitMap[static_cast<std::size_t>(std::clamp(utilPendingSubArg_, 0, 6))];
            mem.initPerformanceEditBuffer(tmpl);
            combineBorrowedFunction_ = false;
            resetPerfInstrumentMutes();
            commitCurrentPerfEdit(true);
            setNotice("Completed !", 45);
            screen_ = ClassicScreen::PlayPerformance;
            return true;
        }
        if (utilPendingAction_ == 4) {
            (void)mem.recallPerformanceEditBuffer();
            combineBorrowedFunction_ = false;
            resetPerfInstrumentMutes();
            commitCurrentPerfEdit(true);
            setNotice("Completed !", 45);
            screen_ = ClassicScreen::PlayPerformance;
            return true;
        }
        if (utilPendingAction_ == 5) {
            if (utilPendingSubArg_ == 0 || utilPendingSubArg_ == 1) {
                resetPerfEffectMemoriesToFactory();
            }
            if (utilPendingSubArg_ == 0 || utilPendingSubArg_ == 2) {
                mem.resetUserMicrotuningsToFactory();
            }
            commitCurrentPerfEdit(false);
            setNotice("Completed !", 45);
            screen_ = isPerformanceContextScreen() ? ClassicScreen::UtilOthersPerf : ClassicScreen::UtilOthersVoice;
            return true;
        }
    }

    // Store & Copy confirmations (pp. 119-120)
    auto& card = processor_.mutableVirtualCard();
    const bool cardProtectedOrReadOnly = processor_.isCardMemoryProtected() || card.hardwareWriteProtect || card.isReadOnlyMedia();

    if (screen_ == ClassicScreen::StoreVoice) {
        if (storeTargetCard_) {
            if (cardProtectedOrReadOnly) {
                setNotice(card.isReadOnlyMedia() ? "ROM Card!" : "Memory Protected", 60);
                return true;
            }
            mem.setOperatorEnableMask(0x0Fu);
            const std::size_t slot = static_cast<std::size_t>(std::clamp(storeTargetSlot_, 0, 99));
            mem.singleEditSlot().repackRomBytes();
            card.activeBank().voices[slot] = mem.singleEditSlot();
            card.activeBank().voices[slot].voice.index = static_cast<uint8_t>(slot);
            cardBankActive_ = true;
            processor_.setClassicCardBankActive(true);
            combineBorrowedFunction_ = false;
            commitCurrentVoiceEdit(true);
            storeConfirmReady_ = false;
            setNotice("Completed !", 45);
            screen_ = ClassicScreen::PlaySingle;
            return true;
        }
        if (processor_.isIntMemoryProtected() && !storeIntProtBypassed_) {
            setNotice("Memory Protected", 60);
            return true;
        }
        const std::size_t slot = static_cast<std::size_t>(std::clamp(storeTargetSlot_, 0, 99));
        mem.storeEditBufferToUserVoice(slot);
        cardBankActive_ = false;
        processor_.setClassicCardBankActive(false);
        combineBorrowedFunction_ = false;
        storeIntProtBypassed_ = false;
        storeConfirmReady_ = false;
        processor_.selectSinglePresetAtomic(vf50::state::MemoryBank::UserRam, slot, true);
        setNotice("Completed !", 45);
        screen_ = ClassicScreen::PlaySingle;
        return true;
    }

    if (screen_ == ClassicScreen::StorePerf) {
        if (storeTargetCard_) {
            if (cardProtectedOrReadOnly) {
                setNotice(card.isReadOnlyMedia() ? "ROM Card!" : "Memory Protected", 60);
                return true;
            }
            resetPerfInstrumentMutes();
            const std::size_t slot = static_cast<std::size_t>(std::clamp(storeTargetSlot_, 0, 99));
            mem.perfEditSlot().repackRomBytes();
            card.activeBank().performances[slot] = mem.perfEditSlot();
            card.activeBank().performances[slot].perf.index = static_cast<uint8_t>(slot);
            cardBankActive_ = true;
            processor_.setClassicCardBankActive(true);
            combineBorrowedFunction_ = false;
            commitCurrentPerfEdit(true);
            storeConfirmReady_ = false;
            setNotice("Completed !", 45);
            screen_ = ClassicScreen::PlayPerformance;
            return true;
        }
        if (processor_.isIntMemoryProtected() && !storeIntProtBypassed_) {
            setNotice("Memory Protected", 60);
            return true;
        }
        resetPerfInstrumentMutes();
        const std::size_t slot = static_cast<std::size_t>(std::clamp(storeTargetSlot_, 0, 99));
        mem.storeEditBufferToUserPerformance(slot);
        cardBankActive_ = false;
        processor_.setClassicCardBankActive(false);
        combineBorrowedFunction_ = false;
        storeIntProtBypassed_ = false;
        storeConfirmReady_ = false;
        processor_.selectPerformancePresetAtomic(vf50::state::MemoryBank::UserRam, slot, true);
        setNotice("Completed !", 45);
        screen_ = ClassicScreen::PlayPerformance;
        return true;
    }

    if (screen_ == ClassicScreen::StoreMultiVoice) {
        saveActiveMultiVoiceBackToSlot();
        if (storeTargetCard_) {
            if (cardProtectedOrReadOnly) {
                setNotice(card.isReadOnlyMedia() ? "ROM Card!" : "Memory Protected", 60);
                return true;
            }
        } else if (processor_.isIntMemoryProtected() && !storeIntProtBypassed_) {
            setNotice("Memory Protected", 60);
            return true;
        }
        const std::size_t slot = static_cast<std::size_t>(std::clamp(storeTargetSlot_, 0, 99));
        const int instIdx = std::clamp(multiVoiceEditInst_, 0, 7);
        if (storeTargetCard_) {
            card.activeBank().voices[slot] = multiVoiceBuffers_[static_cast<std::size_t>(instIdx)];
            card.activeBank().voices[slot].voice.index = static_cast<uint8_t>(slot);
        } else {
            mem.storeUserVoice(slot, multiVoiceBuffers_[static_cast<std::size_t>(instIdx)].voice);
        }
        mem.perfEditSlot().perf.instruments[static_cast<std::size_t>(instIdx)].voiceBank = 0u;
        mem.perfEditSlot().perf.instruments[static_cast<std::size_t>(instIdx)].voiceNumber = static_cast<uint8_t>(slot);
        multiVoiceDirty_[static_cast<std::size_t>(instIdx)] = false;
        storeConfirmReady_ = false;
        storeIntProtBypassed_ = false;
        commitCurrentPerfEdit(false);
        setNotice("Completed !", 45);
        screen_ = ClassicScreen::UtilOthersMultiVoiceSelect;
        return true;
    }

    if (screen_ == ClassicScreen::CopyEffect) {
        if (storeTargetCard_) {
            if (cardProtectedOrReadOnly) {
                setNotice(card.isReadOnlyMedia() ? "ROM Card!" : "Memory Protected", 60);
                return true;
            }
        } else if (processor_.isIntMemoryProtected() && !storeIntProtBypassed_) {
            setNotice("Memory Protected", 60);
            return true;
        }
        const auto fx = (storeReturnScreen_ == ClassicScreen::PerfEditEffect)
                            ? mem.perfEditSlot().perf.effect
                            : mem.singleEditSlot().voice.effect;
        const std::size_t slot = static_cast<std::size_t>(std::clamp(storeTargetSlot_, 0, 99));
        if (storeReturnScreen_ == ClassicScreen::PerfEditEffect) {
            if (storeTargetCard_) {
                card.activeBank().performances[slot].perf.effect = fx;
                card.activeBank().performances[slot].repackRomBytes();
            } else {
                auto pf = mem.getUserPerformance(slot);
                pf.effect = fx;
                mem.storeUserPerformance(slot, pf, mem.getUserPerformancePartPans(slot));
                processor_.pushHostMemoryToAudio();
            }
        } else {
            if (storeTargetCard_) {
                card.activeBank().voices[slot].voice.effect = fx;
                card.activeBank().voices[slot].repackRomBytes();
            } else {
                auto v = mem.getUserVoice(slot);
                v.effect = fx;
                mem.storeUserVoice(slot, v);
                processor_.pushHostMemoryToAudio();
            }
        }
        storeConfirmReady_ = false;
        storeIntProtBypassed_ = false;
        setNotice("Completed !", 45);
        screen_ = storeReturnScreen_;
        return true;
    }

    if (screen_ == ClassicScreen::CopyPerfEffect) {
        const int famBase = (copyPerfFxSrcIdx_ / 4) * 4;
        const std::size_t srcSlot = static_cast<std::size_t>(std::clamp(copyPerfFxSrcIdx_ % 4, 0, 3));
        const std::size_t dstSlot = static_cast<std::size_t>(std::clamp(copyPerfFxDstSlot_, 0, 3));
        auto& bank = processor_.mutablePerfEffectBank();
        if (famBase == 0) {
            bank.delays[dstSlot] = bank.delays[srcSlot];
        } else if (famBase == 4) {
            bank.pans[dstSlot] = bank.pans[srcSlot];
        } else {
            bank.chords[dstSlot] = bank.chords[srcSlot];
        }
        processor_.publishClassicSystemStateToAudio();
        copyConfirmReady_ = false;
        setNotice("Completed !", 45);
        screen_ = storeReturnScreen_;
        return true;
    }

    if (screen_ == ClassicScreen::CopyEg) {
        if (copyEgSrcOp_ < 0 || copyEgDstOp_ < 0) {
            setNotice("Select OP1-4 First", 45);
            return true;
        }
        auto& ops = mem.singleEditSlot().voice.operators;
        const auto srcOp = ops[static_cast<std::size_t>(copyEgSrcOp_)];
        auto& dstOp = ops[static_cast<std::size_t>(copyEgDstOp_)];
        // Spec §4.3 (p. 120): Copies AR, D1R, D1L, D2R, RR, LS, RS — EXCLUDES egShift (SHFT)!
        dstOp.ar           = srcOp.ar;
        dstOp.d1r          = srcOp.d1r;
        dstOp.d1l          = srcOp.d1l;
        dstOp.d2r          = srcOp.d2r;
        dstOp.rr           = srcOp.rr;
        dstOp.levelScaling = srcOp.levelScaling;
        dstOp.rateScaling  = srcOp.rateScaling;
        mem.markSingleEditDirty();
        commitCurrentVoiceEdit(false);
        copyConfirmReady_ = false;
        setNotice("Completed !", 45);
        screen_ = storeReturnScreen_;
        return true;
    }

    return false;
}

bool VF50ClassicController::handleEditNo() noexcept {
    if (screen_ == ClassicScreen::UtilOthersMultiVoiceSelect && multiVoiceExitSglConfirm_) {
        multiVoiceExitSglConfirm_ = false;
        return true;
    }
    if (isUtilOthersScreen(screen_) && utilConfirmPending_) {
        utilConfirmPending_ = false;
        return true;
    }
    if (isStoreOrCopyScreen(screen_)) {
        storeConfirmReady_ = false;
        storeIntProtBypassed_ = false;
        copyConfirmReady_ = false;
        screen_ = storeReturnScreen_;
        return true;
    }
    return false;
}

void VF50ClassicController::handleEditDataEntry(float norm) noexcept {
    if (isCompareModeLocked()) return;
    auto& mem = processor_.hostMemory();
    if (screen_ == ClassicScreen::VoiceEditQuick) {
        const int rel = static_cast<int>(std::lround(norm * 100.0f)) - 50;
        applyQuickEditOffset(rel);
        return;
    }
    if (screen_ == ClassicScreen::PerfEditVolume) {
        auto& inst = mem.perfEditSlot().perf.instruments[static_cast<std::size_t>(perfEditInst_)];
        if (isPerfInstActive(mem.perfEditSlot().perf, perfEditInst_)) {
            inst.volume = static_cast<uint8_t>(std::lround(norm * 99.0f));
            mem.markPerfEditDirty();
            commitCurrentPerfEdit(false);
        }
        return;
    }
    if (screen_ == ClassicScreen::VoiceEditAlgorithm) {
        auto& v = mem.singleEditSlot().voice;
        const auto val0To7 = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(norm * 7.0f)), 0, 7));
        if (voiceAlgCursor_ == 0) {
            v.algorithm = val0To7;
        } else {
            v.feedback = val0To7;
        }
        mem.markSingleEditDirty();
        commitCurrentVoiceEdit(false);
        return;
    }
    if (screen_ == ClassicScreen::VoiceEditOutLevel) {
        mem.singleEditSlot().voice.operators[static_cast<std::size_t>(voiceOutCursor_)].outputLevel =
            static_cast<uint8_t>(std::lround(norm * 99.0f));
        mem.markSingleEditDirty();
        commitCurrentVoiceEdit(false);
    }
}

// =============================================================================
// 40x2 LCD Rendering (Verbatim VF50_CLASSIC_SPEC_PHASE2.md §§1, 2, 4)
// =============================================================================

bool VF50ClassicController::renderEditLcdFrame(ClassicLcdFrame& frame) const noexcept {
    const auto& mem = processor_.hostMemory();
    const bool bypassed = (processor_.parameterBank().effectBypass->load(std::memory_order_relaxed) >= 0.5f);
    const bool cmpPerf = mem.isPerformanceCompareActive();
    const bool cmpVoice = mem.isSingleCompareActive();
    char tmp[80]{};

    auto perfPfx = [&]() -> const char* { return cmpPerf ? "c.ED" : "P.ED"; };
    auto voicePfx = [&](std::array<char, 6>& out) {
        const uint8_t mask = mem.operatorEnableMask();
        out[0] = cmpVoice ? 'c' : (multiVoiceEditActive_ ? 'M' : 'e');
        for (int i = 0; i < 4; ++i) {
            out[static_cast<std::size_t>(1 + i)] = (((mask >> i) & 1u) != 0u) ? '1' : '0';
        }
        out[5] = '\0';
    };

    auto perfVoiceTag = [&](int instIdx, char outTag[15]) {
        const auto& pf = mem.activePerformance();
        const auto& inst = pf.instruments[static_cast<std::size_t>(instIdx)];
        if (!isPerfInstActive(pf, instIdx)) {
            std::snprintf(outTag, 15, "  0:          ");
            return;
        }
        const bool isCardPerf = (mem.selectedPerfBank() == vf50::state::MemoryBank::UserRam) && cardBankActive_;
        const char bC = (inst.voiceBank != 0u) ? 'P' : (isCardPerf ? 'C' : 'I');
        const auto& vName = (inst.voiceBank != 0u)
                                ? mem.getFactoryVoice(inst.voiceNumber).name
                                : (isCardPerf ? getCardVoice(inst.voiceNumber).name
                                              : mem.getUserVoice(inst.voiceNumber).name);
        std::snprintf(outTag, 15, "%c%02d:%-10.10s", bC, static_cast<int>(inst.voiceNumber), vName.data());
    };

    // -------------------------------------------------------------------------
    // CHAPTER 2: PERFORMANCE EDIT
    // -------------------------------------------------------------------------
    if (isPerfEditScreen(screen_)) {
        const auto& pf = mem.activePerformance();
        const auto& selInst = pf.instruments[static_cast<std::size_t>(perfEditInst_)];
        char vTag[15]{};
        perfVoiceTag(perfEditInst_, vTag);

        switch (screen_) {
            case ClassicScreen::PerfEditAssignMode: {
                static constexpr std::array<const char*, 3> kModes = { "normal   ", "alternate", "DVA      " };
                std::snprintf(tmp, sizeof(tmp), "%s  %cAssignMode    %cname : %-10.10s ",
                              perfPfx(),
                              (perfAssignCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (perfAssignCursor_ == 1) ? kGlyphPlayArrow : '>',
                              pf.name.data());
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "         %-9.9s           %c   %c      ",
                              kModes[std::min<std::size_t>(pf.assignMode, 2u)],
                              kGlyphLeftArrow, kGlyphRightArrow);
                setLine40(frame.line1, tmp);
                if (perfAssignCursor_ == 1) {
                    frame.showCursor = true;
                    frame.cursorRow = 0;
                    frame.cursorCol = 29 + std::clamp(perfNameCursorCol_, 0, 9);
                }
                return true;
            }

            case ClassicScreen::PerfEditNotes: {
                if (pf.assignMode == 2u) {
                    std::snprintf(tmp, sizeof(tmp), "%s  RESERVED NOTES>     %-14.14s", perfPfx(), vTag);
                    setLine40(frame.line0, tmp);
                    clearLine40(frame.line1);
                    for (int p = 0; p < 8; ++p) {
                        const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                        const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                        const char sep = (p < 7) ? '/' : ' ';
                        if (it.reserveNotes == 0u) {
                            std::snprintf(tmp, sizeof(tmp), "%coff%c", cur, sep);
                        } else {
                            std::snprintf(tmp, sizeof(tmp), "%c%3d%c", cur, static_cast<int>(it.reserveNotes) - 1, sep);
                        }
                        writeSpan40(frame.line1, p * 5, tmp, 5);
                    }
                } else {
                    char rchStr[4]{};
                    if (selInst.receiveChannel >= 16u) std::snprintf(rchStr, 4, "om");
                    else std::snprintf(rchStr, 4, "%2d", static_cast<int>(selInst.receiveChannel) + 1);
                    std::snprintf(tmp, sizeof(tmp), "%s  MAX NOTES>  Rch=%2s  %-14.14s", perfPfx(), rchStr, vTag);
                    setLine40(frame.line0, tmp);
                    clearLine40(frame.line1);
                    for (int p = 0; p < 8; ++p) {
                        const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                        const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                        const char sep = (p < 7) ? '/' : ' ';
                        std::snprintf(tmp, sizeof(tmp), "%c%3d%c", cur, static_cast<int>(it.maxNotes), sep);
                        writeSpan40(frame.line1, p * 5, tmp, 5);
                    }
                }
                return true;
            }

            case ClassicScreen::PerfEditVoiceNo: {
                char rchStr[4]{};
                if (selInst.receiveChannel >= 16u) std::snprintf(rchStr, 4, "om");
                else std::snprintf(rchStr, 4, "%2d", static_cast<int>(selInst.receiveChannel) + 1);
                std::snprintf(tmp, sizeof(tmp), "%s  VOICE NO>  Rch=%2s  %-14.14s", perfPfx(), rchStr, vTag);
                setLine40(frame.line0, tmp);
                clearLine40(frame.line1);
                const bool isCardPerf = (mem.selectedPerfBank() == vf50::state::MemoryBank::UserRam) && cardBankActive_;
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        std::snprintf(tmp, sizeof(tmp), "%c  0%c", cur, sep);
                    } else {
                        const char bC = (it.voiceBank != 0u) ? 'P' : (isCardPerf ? 'C' : 'I');
                        std::snprintf(tmp, sizeof(tmp), "%c%c%02d%c", cur, bC, static_cast<int>(it.voiceNumber), sep);
                    }
                    writeSpan40(frame.line1, p * 5, tmp, 5);
                }
                return true;
            }

            case ClassicScreen::PerfEditRecvCh: {
                std::snprintf(tmp, sizeof(tmp), "%s  RECV CH>   Tch= 1  %-14.14s", perfPfx(), vTag);
                setLine40(frame.line0, tmp);
                clearLine40(frame.line1);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                    } else if (it.receiveChannel >= 16u) {
                        std::snprintf(tmp, sizeof(tmp), "%comn%c", cur, sep);
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%c%3d%c", cur, static_cast<int>(it.receiveChannel) + 1, sep);
                    }
                    writeSpan40(frame.line1, p * 5, tmp, 5);
                }
                return true;
            }

            case ClassicScreen::PerfEditLimitLow: {
                char hiStr[5]{};
                formatMidiNote4(selInst.limitHigh, hiStr);
                std::snprintf(tmp, sizeof(tmp), "%s  LIMIT/LOW>  H=%-4.4s %-14.14s", perfPfx(), hiStr, vTag);
                setLine40(frame.line0, tmp);
                clearLine40(frame.line1);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                    } else {
                        char nStr[5]{};
                        formatMidiNote4(it.limitLow, nStr);
                        std::snprintf(tmp, sizeof(tmp), "%c%3.3s%c", cur, nStr, sep);
                    }
                    writeSpan40(frame.line1, p * 5, tmp, 5);
                }
                return true;
            }

            case ClassicScreen::PerfEditLimitHigh: {
                char loStr[5]{};
                formatMidiNote4(selInst.limitLow, loStr);
                std::snprintf(tmp, sizeof(tmp), "%s  LIMIT/HIGH> L=%-4.4s %-14.14s", perfPfx(), loStr, vTag);
                setLine40(frame.line0, tmp);
                clearLine40(frame.line1);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                    } else {
                        char nStr[5]{};
                        formatMidiNote4(it.limitHigh, nStr);
                        std::snprintf(tmp, sizeof(tmp), "%c%3.3s%c", cur, nStr, sep);
                    }
                    writeSpan40(frame.line1, p * 5, tmp, 5);
                }
                return true;
            }

            case ClassicScreen::PerfEditDetune: {
                std::snprintf(tmp, sizeof(tmp), "%s  INST DETUNE>Tch= 1 %-14.14s", perfPfx(), vTag);
                setLine40(frame.line0, tmp);
                clearLine40(frame.line1);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%c%+3d%c", cur, static_cast<int>(it.detune), sep);
                    }
                    writeSpan40(frame.line1, p * 5, tmp, 5);
                }
                return true;
            }

            case ClassicScreen::PerfEditNoteShift: {
                std::snprintf(tmp, sizeof(tmp), "%s  NOTE SHIFT> Tch= 1 %-14.14s", perfPfx(), vTag);
                setLine40(frame.line0, tmp);
                clearLine40(frame.line1);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%c%+3d%c", cur, static_cast<int>(it.noteShift), sep);
                    }
                    writeSpan40(frame.line1, p * 5, tmp, 5);
                }
                return true;
            }

            case ClassicScreen::PerfEditVolume: {
                clearLine40(frame.line0);
                std::snprintf(tmp, sizeof(tmp), "%s   VOLUME>  ", perfPfx());
                writeSpan40(frame.line0, 0, tmp, 16);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) std::snprintf(tmp, sizeof(tmp), " *%c", sep);
                    else                          std::snprintf(tmp, sizeof(tmp), "%2d%c", static_cast<int>(it.volume), sep);
                    writeSpan40(frame.line0, 16 + p * 3, tmp, 3);
                }
                clearLine40(frame.line1);
                writeSpan40(frame.line1, 0, perfPfx(), 4);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const int col = 6 + p * 4;
                    frame.line1[static_cast<std::size_t>(col)] = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        frame.line1[static_cast<std::size_t>(col + 1)] = '*';
                    } else if (it.volume == 0u) {
                        frame.line1[static_cast<std::size_t>(col + 1)] = '_';
                    } else {
                        const int bar = std::clamp(1 + (static_cast<int>(it.volume) * 7) / 99, 1, 8);
                        frame.line1[static_cast<std::size_t>(col + 1)] = static_cast<char>(bar);
                    }
                }
                return true;
            }

            case ClassicScreen::PerfEditOutAssign: {
                static constexpr std::array<const char*, 4> kOutStr = { "off", " L ", " R ", "L+R" };
                std::snprintf(tmp, sizeof(tmp), "%s OUT ASSIGN> Tch= 1  %-14.14s", perfPfx(), vTag);
                setLine40(frame.line0, tmp);
                clearLine40(frame.line1);
                for (int p = 0; p < 8; ++p) {
                    const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                    const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                    const char sep = (p < 7) ? '/' : ' ';
                    if (!isPerfInstActive(pf, p)) {
                        std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%c%-3.3s%c", cur, kOutStr[it.outAssign & 0x03u], sep);
                    }
                    writeSpan40(frame.line1, p * 5, tmp, 5);
                }
                return true;
            }

            case ClassicScreen::PerfEditOthers: {
                if (perfOthersSubPage_ == 0) {
                    static constexpr std::array<const char*, 4> kLfoStr = { "off", " 1 ", " 2 ", "vib" };
                    std::snprintf(tmp, sizeof(tmp), "%s  LFO SELECT>                      %c", perfPfx(), kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    clearLine40(frame.line1);
                    for (int p = 0; p < 8; ++p) {
                        const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                        const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                        const char sep = (p < 7) ? '/' : ' ';
                        if (!isPerfInstActive(pf, p)) std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                        else std::snprintf(tmp, sizeof(tmp), "%c%-3.3s%c", cur, kLfoStr[it.lfoSelect & 0x03u], sep);
                        writeSpan40(frame.line1, p * 5, tmp, 5);
                    }
                } else if (perfOthersSubPage_ == 1) {
                    char tblCode[4]{};
                    if (pf.microTuneTable <= 10u) std::snprintf(tblCode, 4, "%2d", static_cast<int>(pf.microTuneTable) + 1);
                    else if (pf.microTuneTable == 11u) std::snprintf(tblCode, 4, "Oc");
                    else std::snprintf(tblCode, 4, "Fl");
                    const char* keyStr = (pf.microTuneTable >= 1u && pf.microTuneTable <= 4u)
                                             ? kKeyNoteNames[std::min<std::size_t>(pf.microTuneRootKey, 11u)]
                                             : "  ";
                    std::snprintf(tmp, sizeof(tmp), "%s  MICRO TUNE> Sel=%2s    %-2s        %c",
                                  perfPfx(), tblCode, keyStr, kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    clearLine40(frame.line1);
                    for (int p = 0; p < 8; ++p) {
                        const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                        const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                        const char sep = (p < 7) ? '/' : ' ';
                        if (!isPerfInstActive(pf, p)) std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                        else std::snprintf(tmp, sizeof(tmp), "%c%-3.3s%c", cur, (it.microTuneEnable != 0u) ? " on" : "off", sep);
                        writeSpan40(frame.line1, p * 5, tmp, 5);
                    }
                } else if (perfOthersSubPage_ == 2) {
                    std::snprintf(tmp, sizeof(tmp), "%s MICRO TUNE)                       %c", perfPfx(), kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    const int tIdx = std::clamp(static_cast<int>(pf.microTuneTable), 0, 12);
                    const bool hasKey = (tIdx >= 1 && tIdx <= 4);
                    if (hasKey) {
                        std::snprintf(tmp, sizeof(tmp), "%cTable=%-16.16s  %cKey= %-2s       ",
                                      (perfMicroTableCursor_ == 0) ? kGlyphPlayArrow : '>',
                                      kMicroTuneTableNames[static_cast<std::size_t>(tIdx)],
                                      (perfMicroTableCursor_ == 1) ? kGlyphPlayArrow : '>',
                                      kKeyNoteNames[std::min<std::size_t>(pf.microTuneRootKey, 11u)]);
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%cTable=%-16.16s                 ",
                                      kGlyphPlayArrow,
                                      kMicroTuneTableNames[static_cast<std::size_t>(tIdx)]);
                    }
                    setLine40(frame.line1, tmp);
                } else {
                    std::snprintf(tmp, sizeof(tmp), "%s  PFM EFCT SEL)                    %c", perfPfx(), kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    const int uiIdx = perfFxEngineToUi(pf.perfEffectSelect);
                    if (uiIdx == 0) {
                        setLine40(frame.line1, "Effect = off                            ");
                    } else {
                        const auto& bank = processor_.perfEffectBank();
                        const int slot0To3 = (uiIdx - 1) % 4;
                        const int slot1To4 = slot0To3 + 1;
                        if (uiIdx <= 4) {
                            const auto& pm = bank.delays[static_cast<std::size_t>(slot0To3)];
                            std::snprintf(tmp, sizeof(tmp), "Effect =delay%d(time=%.2fs ps=%+d fb=%d)",
                                          slot1To4, static_cast<double>(pm.delayTimeCs) * 0.01,
                                          static_cast<int>(pm.pitchShift), static_cast<int>(pm.feedback));
                        } else if (uiIdx <= 8) {
                            const auto& pm = bank.pans[static_cast<std::size_t>(slot0To3)];
                            std::snprintf(tmp, sizeof(tmp), "Effect =pan%d(sel=%d dir=%d rng=%d)     ",
                                          slot1To4, static_cast<int>(pm.select),
                                          static_cast<int>(pm.direction), static_cast<int>(pm.panRange));
                        } else {
                            const auto& ch = bank.chords[static_cast<std::size_t>(slot0To3)].chords[0];
                            std::snprintf(tmp, sizeof(tmp), "Effect =chord%d(%d/%d/%d)              ",
                                          slot1To4, static_cast<int>(ch[0]), static_cast<int>(ch[1]), static_cast<int>(ch[2]));
                        }
                        setLine40(frame.line1, tmp);
                    }
                }
                return true;
            }

            case ClassicScreen::PerfEditEffect: {
                const auto& fx = pf.effect;
                const auto& spec = kDspEffectSpecs[std::min<std::size_t>(fx.effectType, 32u)];
                const char* byp = bypassed ? "BYPASS" : "      ";
                if (perfEffectSubPage_ == 0) {
                    std::snprintf(tmp, sizeof(tmp), "%s EFCT) No.=%2d:%-16.16s %6.6s %c",
                                  perfPfx(), static_cast<int>(fx.effectType), spec.shortName, byp, kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    clearLine40(frame.line1);
                    for (int p = 0; p < 8; ++p) {
                        const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                        const char cur = (p == perfEditInst_) ? kGlyphPlayArrow : ' ';
                        const char sep = (p < 7) ? '/' : ' ';
                        if (!isPerfInstActive(pf, p)) std::snprintf(tmp, sizeof(tmp), "%c * %c", cur, sep);
                        else std::snprintf(tmp, sizeof(tmp), "%c%-3.3s%c", cur, (it.effectEnable != 0u) ? " on" : "off", sep);
                        writeSpan40(frame.line1, p * 5, tmp, 5);
                    }
                } else if (perfEffectSubPage_ == 1) {
                    std::snprintf(tmp, sizeof(tmp), "%s EFCT)%cSelect   %cBalance    %6.6s %c",
                                  perfPfx(),
                                  (perfEffectCursor_ == 0) ? kGlyphPlayArrow : '>',
                                  (perfEffectCursor_ == 1) ? kGlyphPlayArrow : '>',
                                  byp, kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    std::snprintf(tmp, sizeof(tmp), "    %2d:%-15.15s%3d %%             ",
                                  static_cast<int>(fx.effectType), spec.fullName, static_cast<int>(fx.balance));
                    setLine40(frame.line1, tmp);
                } else if (perfEffectSubPage_ == 2) {
                    std::snprintf(tmp, sizeof(tmp), "%s EFCT)%cOutLevel %cStereoMix  %6.6s %c",
                                  perfPfx(),
                                  (perfEffectCursor_ == 0) ? kGlyphPlayArrow : '>',
                                  (perfEffectCursor_ == 1) ? kGlyphPlayArrow : '>',
                                  byp, kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    std::snprintf(tmp, sizeof(tmp), "           %3d %%       %-3.3s              ",
                                  static_cast<int>(fx.outLevel), (fx.stereoMix != 0u) ? "on" : "off");
                    setLine40(frame.line1, tmp);
                } else {
                    formatFxParamsHeader(tmp, sizeof(tmp), perfPfx(), ')', perfEffectCursor_, spec, bypassed);
                    setLine40(frame.line0, tmp);
                    char v0[10]{}, v1[10]{}, v2[10]{};
                    formatFxParamValue(spec.params[0], fx.param1, v0);
                    formatFxParamValue(spec.params[1], fx.param2, v1);
                    formatFxParamValue(spec.params[2], fx.param3, v2);
                    std::snprintf(tmp, sizeof(tmp), "          %-7.7s %-7.7s %-7.7s       ", v0, v1, v2);
                    setLine40(frame.line1, tmp);
                }
                return true;
            }

            default:
                break;
        }
    }

    // -------------------------------------------------------------------------
    // CHAPTER 3: VOICE EDIT
    // -------------------------------------------------------------------------
    if (isVoiceEditScreen(screen_)) {
        const auto& vSlot = mem.singleEditSlot();
        const auto& v = mem.activeSingleVoice();
        const auto& op = v.operators[static_cast<std::size_t>(selectedOperator_)];
        std::array<char, 6> pfx{};
        voicePfx(pfx);
        const int alg1 = static_cast<int>(v.algorithm & 0x07u) + 1;
        const int op1 = selectedOperator_ + 1;

        switch (screen_) {
            case ClassicScreen::VoiceEditQuick: {
                static constexpr std::array<const char*, 4> kQNames = { "ATTACK", "RELEAS", "VOLUME", "BRIL  " };
                std::snprintf(tmp, sizeof(tmp), "%s QUICK>   OP1  OP2  OP3  OP4      %c", pfx.data(), kGlyphMultiPage);
                setLine40(frame.line0, tmp);
                int vals[4]{};
                for (int i = 0; i < 4; ++i) {
                    const auto& o = v.operators[static_cast<std::size_t>(i)];
                    if (voiceQuickSubPage_ == 0)      vals[i] = o.ar;
                    else if (voiceQuickSubPage_ == 1) vals[i] = o.rr;
                    else                              vals[i] = o.outputLevel;
                }
                const char dirSym = (quickEditRelOffset_ == 0) ? '=' : ((quickEditRelOffset_ > 0) ? '+' : '-');
                std::snprintf(tmp, sizeof(tmp), "ALG %d %-6.6s->%c %2d   %2d   %2d   %2d       ",
                              alg1, kQNames[static_cast<std::size_t>(voiceQuickSubPage_)], dirSym,
                              vals[0], vals[1], vals[2], vals[3]);
                setLine40(frame.line1, tmp);
                return true;
            }

            case ClassicScreen::VoiceEditAlgorithm:
                std::snprintf(tmp, sizeof(tmp), "%s ALG>    %-8.8s %cFeedback(OP4)   ",
                              pfx.data(), kAlgRow0[static_cast<std::size_t>(alg1 - 1)],
                              (voiceAlgCursor_ == 1) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "     %cALG=%d   %-8.8s     %d            ",
                              (voiceAlgCursor_ == 0) ? kGlyphPlayArrow : '>',
                              alg1, kAlgRow1[static_cast<std::size_t>(alg1 - 1)], static_cast<int>(v.feedback));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditLfo: {
                static constexpr std::array<const char*, 4> kWaves = { "saw up ", "square ", "triangl", "S/Hold " };
                std::snprintf(tmp, sizeof(tmp), "%s LFO>%cWAVE %cSPD%cDLY%cSYNC %cPMD %cAMD ",
                              pfx.data(),
                              (voiceLfoCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceLfoCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceLfoCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voiceLfoCursor_ == 3) ? kGlyphPlayArrow : '>',
                              (voiceLfoCursor_ == 4) ? kGlyphPlayArrow : '>',
                              (voiceLfoCursor_ == 5) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d    %-7.7s %2d  %2d  %-3.3s   %2d   %2d  ",
                              alg1, kWaves[v.lfoWave & 0x03u], static_cast<int>(v.lfoSpeed),
                              static_cast<int>(v.lfoDelay), (v.lfoSync != 0u) ? "on " : "off",
                              static_cast<int>(v.pmd), static_cast<int>(v.amd));
                setLine40(frame.line1, tmp);
                return true;
            }

            case ClassicScreen::VoiceEditSens:
                std::snprintf(tmp, sizeof(tmp), "%s SENS>%cPMS(all) %cAMS %cAME %cEBS %cKVS",
                              pfx.data(),
                              (voiceSensCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceSensCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceSensCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voiceSensCursor_ == 3) ? kGlyphPlayArrow : '>',
                              (voiceSensCursor_ == 4) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d OP%d      %d       %d   %-3.3s   %d   %+2d ",
                              alg1, op1, static_cast<int>(v.pms), static_cast<int>(v.ams),
                              (op.ampModEnable != 0u) ? "on " : "off",
                              static_cast<int>(op.egBiasSens), static_cast<int>(op.keyVelSens));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditOsc: {
                if (op.fixedFreqMode == 0u) {
                    const double ratio = vf50::dsp::fm::computeOpzRatioMultiplier(op.coarse, op.fine);
                    std::snprintf(tmp, sizeof(tmp), "%s %cMODE         %cCRS%cFINE%cOSW %cDET  ",
                                  pfx.data(),
                                  (voiceOscCursor_ == 0) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 3) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 4) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 5) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 6) ? kGlyphPlayArrow : '>');
                    setLine40(frame.line0, tmp);
                    std::snprintf(tmp, sizeof(tmp), "OP%d   ratio         %5.2f  W%d     %+2d   ",
                                  op1, ratio, static_cast<int>(op.waveform) + 1, static_cast<int>(op.detune));
                    setLine40(frame.line1, tmp);
                } else {
                    const double hz = vf50::dsp::fm::computeOpzFixedFreqHz(op.coarse, op.fine, op.fixedRange, op.fixedRangeLow);
                    const double rangeMin = vf50::dsp::fm::computeOpzFixedFreqHz(0u, 0u, op.fixedRange, op.fixedRangeLow);
                    char rngStr[7]{}, hzStr[7]{};
                    if (rangeMin >= 1000.0) std::snprintf(rngStr, 7, "%2dKHz", static_cast<int>(std::lround(rangeMin / 1000.0)));
                    else                    std::snprintf(rngStr, 7, "%3dHz", static_cast<int>(std::lround(rangeMin)));
                    if (hz >= 1000.0) std::snprintf(hzStr, 7, "%4.1fK", hz / 1000.0);
                    else if (hz < 10.0) std::snprintf(hzStr, 7, "%4.1fH", hz);
                    else std::snprintf(hzStr, 7, "%3dHz", static_cast<int>(std::lround(hz)));

                    std::snprintf(tmp, sizeof(tmp), "%s %cMODE%cSHFT%cRANGE%cCRS%cFINE%cOSW %cDET",
                                  pfx.data(),
                                  (voiceOscCursor_ == 0) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 1) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 2) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 3) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 4) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 5) ? kGlyphPlayArrow : '>',
                                  (voiceOscCursor_ == 6) ? kGlyphPlayArrow : '>');
                    setLine40(frame.line0, tmp);
                    std::snprintf(tmp, sizeof(tmp), "OP%d    fix  %-2.2s  %5.5s %5.5s W%d     %+2d  ",
                                  op1, (op.fixedRangeLow != 0u) ? "Lo" : "Hi", rngStr, hzStr,
                                  static_cast<int>(op.waveform) + 1, static_cast<int>(op.detune));
                    setLine40(frame.line1, tmp);
                }
                return true;
            }

            case ClassicScreen::VoiceEditEg: {
                static constexpr std::array<const char*, 4> kShftStr = { "off", "48 ", "24 ", "12 " };
                std::snprintf(tmp, sizeof(tmp), "%s EG> %cAR  %cD1R %cD1L %cD2R %cRR %cSHFT ",
                              pfx.data(),
                              (voiceEgCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceEgCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceEgCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voiceEgCursor_ == 3) ? kGlyphPlayArrow : '>',
                              (voiceEgCursor_ == 4) ? kGlyphPlayArrow : '>',
                              (voiceEgCursor_ == 5) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                const uint8_t effShift = (selectedOperator_ == 0) ? 0u : (op.egShift & 0x03u);
                std::snprintf(tmp, sizeof(tmp), "ALG %d OP%d  %2d   %2d   %2d   %2d  %2d   %-3.3s  ",
                              alg1, op1, static_cast<int>(op.ar), static_cast<int>(op.d1r),
                              static_cast<int>(op.d1l), static_cast<int>(op.d2r),
                              static_cast<int>(op.rr), kShftStr[effShift]);
                setLine40(frame.line1, tmp);
                return true;
            }

            case ClassicScreen::VoiceEditPeg:
                std::snprintf(tmp, sizeof(tmp), "%s PEG> %cPR1 %cPL1 %cPR2 %cPL2 %cPR3 %cPL3",
                              pfx.data(),
                              (voicePegCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voicePegCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voicePegCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voicePegCursor_ == 3) ? kGlyphPlayArrow : '>',
                              (voicePegCursor_ == 4) ? kGlyphPlayArrow : '>',
                              (voicePegCursor_ == 5) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d OP%d  %2d   %2d   %2d   %2d   %2d   %2d ",
                              alg1, op1, static_cast<int>(v.peg.pr1), static_cast<int>(v.peg.pl1),
                              static_cast<int>(v.peg.pr2), static_cast<int>(v.peg.pl2),
                              static_cast<int>(v.peg.pr3), static_cast<int>(v.peg.pl3));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditOutLevel:
                std::snprintf(tmp, sizeof(tmp), "%s OUT LEVEL>%cOP1 %cOP2 %cOP3 %cOP4     ",
                              pfx.data(),
                              (voiceOutCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceOutCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceOutCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voiceOutCursor_ == 3) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d            %2d   %2d   %2d   %2d      ",
                              alg1, static_cast<int>(v.operators[0].outputLevel),
                              static_cast<int>(v.operators[1].outputLevel),
                              static_cast<int>(v.operators[2].outputLevel),
                              static_cast<int>(v.operators[3].outputLevel));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditScaling:
                std::snprintf(tmp, sizeof(tmp), "%s SCALING>  %cOP1 %cOP2 %cOP3 %cOP4    %c",
                              pfx.data(),
                              (voiceScalingCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceScalingCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceScalingCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voiceScalingCursor_ == 3) ? kGlyphPlayArrow : '>',
                              kGlyphMultiPage);
                setLine40(frame.line0, tmp);
                if (voiceScalingSubPage_ == 0) {
                    std::snprintf(tmp, sizeof(tmp), "ALG %d RATE        %d    %d    %d    %d      ",
                                  alg1, static_cast<int>(v.operators[0].rateScaling),
                                  static_cast<int>(v.operators[1].rateScaling),
                                  static_cast<int>(v.operators[2].rateScaling),
                                  static_cast<int>(v.operators[3].rateScaling));
                } else {
                    std::snprintf(tmp, sizeof(tmp), "ALG %d LEVEL     %+3d  %+3d  %+3d  %+3d     ",
                                  alg1, static_cast<int>(v.operators[0].levelScaling),
                                  static_cast<int>(v.operators[1].levelScaling),
                                  static_cast<int>(v.operators[2].levelScaling),
                                  static_cast<int>(v.operators[3].levelScaling));
                }
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditTranspose: {
                char midC[5]{};
                formatMidiNote4(60 + static_cast<int>(v.transpose), midC);
                std::snprintf(tmp, sizeof(tmp), "%s TRANSPOSE> %cMiddle C   %cKBD       ",
                              pfx.data(),
                              (voiceTransposeCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceTransposeCursor_ == 1) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d                %-3.3s      %-3.3s       ",
                              alg1, midC, voiceTransposeKbdIn_ ? "in " : "off");
                setLine40(frame.line1, tmp);
                return true;
            }

            case ClassicScreen::VoiceEditFuncRoot:
                std::snprintf(tmp, sizeof(tmp), "%s FUNCTION> Select one !            ", pfx.data());
                setLine40(frame.line0, tmp);
                setLine40(frame.line1, ">PBetc>Port>FC >MW  >BC  >AT  >REVR>NAME");
                return true;

            case ClassicScreen::VoiceEditFuncPbEtc:
                std::snprintf(tmp, sizeof(tmp), "%s P.BEND> %cMode   %cPBR %cFSW         ",
                              pfx.data(),
                              (voiceFuncCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 2) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d     %-9.9s  %2d   %-3.3s    >EXIT",
                              alg1, (v.monoMode != 0u) ? "mono mode" : "poly mode",
                              static_cast<int>(v.pitchBendRange), (v.sustainSwitch != 0u) ? "sus" : "por");
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditFuncPort: {
                const char* pmStr = (v.monoMode != 0u && v.portaMode != 0u) ? "Fingerd Porta" : "Full Time Porta";
                std::snprintf(tmp, sizeof(tmp), "%s PORTA>   %cMode     %cTime          ",
                              pfx.data(),
                              (voiceFuncCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 1) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d    %-15.15s %2d     >EXIT",
                              alg1, pmStr, static_cast<int>(v.portaTime));
                setLine40(frame.line1, tmp);
                return true;
            }

            case ClassicScreen::VoiceEditFuncFc:
                std::snprintf(tmp, sizeof(tmp), "%s FC>   %cVol %cPitch  %cAmplitude     ",
                              pfx.data(),
                              (voiceFuncCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 2) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d       %2d   %2d        %2d     >EXIT",
                              alg1, static_cast<int>(v.fcVolume), static_cast<int>(v.fcPitch), static_cast<int>(v.fcAmp));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditFuncMw:
                std::snprintf(tmp, sizeof(tmp), "%s MW>   %cPitch  %cAmplitude          ",
                              pfx.data(),
                              (voiceFuncCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 1) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d        %2d       %2d          >EXIT",
                              alg1, static_cast<int>(v.mwPitch), static_cast<int>(v.mwAmp));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditFuncBc:
                std::snprintf(tmp, sizeof(tmp), "%s BC>%cPitch%cAmpli%cP.Bias  %cEG Bias  ",
                              pfx.data(),
                              (voiceFuncCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 3) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d      %2d   %2d   %+3d       %2d  >EXIT",
                              alg1, static_cast<int>(v.bcPitch), static_cast<int>(v.bcAmp),
                              static_cast<int>(v.bcPitchBias), static_cast<int>(v.bcEgBias));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditFuncAt:
                std::snprintf(tmp, sizeof(tmp), "%s AT>%cPitch%cAmpli%cP.Bias  %cEG Bias  ",
                              pfx.data(),
                              (voiceFuncCursor_ == 0) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 1) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 2) ? kGlyphPlayArrow : '>',
                              (voiceFuncCursor_ == 3) ? kGlyphPlayArrow : '>');
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d      %2d   %2d   %+3d       %2d  >EXIT",
                              alg1, static_cast<int>(v.atPitch), static_cast<int>(v.atAmp),
                              static_cast<int>(v.atPitchBias), static_cast<int>(v.atEgBias));
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditFuncRevr:
                std::snprintf(tmp, sizeof(tmp), "%s REVERB RATE> >Rate                ", pfx.data());
                setLine40(frame.line0, tmp);
                if (v.reverbRate == 0u) {
                    std::snprintf(tmp, sizeof(tmp), "ALG %d               off           >EXIT", alg1);
                } else {
                    std::snprintf(tmp, sizeof(tmp), "ALG %d                %d            >EXIT", alg1, static_cast<int>(v.reverbRate));
                }
                setLine40(frame.line1, tmp);
                return true;

            case ClassicScreen::VoiceEditFuncName:
                std::snprintf(tmp, sizeof(tmp), "%s VOICE NAME>                       ", pfx.data());
                setLine40(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "ALG %d   NAME : %-10.10s  <-   -> >EXIT", alg1, vSlot.nameChars.data());
                setLine40(frame.line1, tmp);
                frame.showCursor = true;
                frame.cursorRow = 1;
                frame.cursorCol = 15 + std::clamp(voiceNameCursorCol_, 0, 9);
                return true;

            case ClassicScreen::VoiceEditEffect: {
                const auto& fx = v.effect;
                const auto& spec = kDspEffectSpecs[std::min<std::size_t>(fx.effectType, 32u)];
                const char* byp = bypassed ? "BYPASS" : "      ";
                if (voiceEffectSubPage_ == 0) {
                    std::snprintf(tmp, sizeof(tmp), "%s EFCT>%cSelect   %cBalance    %6.6s %c",
                                  pfx.data(),
                                  (voiceEffectCursor_ == 0) ? kGlyphPlayArrow : '>',
                                  (voiceEffectCursor_ == 1) ? kGlyphPlayArrow : '>',
                                  byp, kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    std::snprintf(tmp, sizeof(tmp), "     %2d:%-15.15s%3d %%            ",
                                  static_cast<int>(fx.effectType), spec.fullName, static_cast<int>(fx.balance));
                    setLine40(frame.line1, tmp);
                } else if (voiceEffectSubPage_ == 1) {
                    std::snprintf(tmp, sizeof(tmp), "%s EFCT>%cOutLevel %cStereoMix  %6.6s %c",
                                  pfx.data(),
                                  (voiceEffectCursor_ == 0) ? kGlyphPlayArrow : '>',
                                  (voiceEffectCursor_ == 1) ? kGlyphPlayArrow : '>',
                                  byp, kGlyphMultiPage);
                    setLine40(frame.line0, tmp);
                    std::snprintf(tmp, sizeof(tmp), "            %3d %%      %-3.3s              ",
                                  static_cast<int>(fx.outLevel), (fx.stereoMix != 0u) ? "on" : "off");
                    setLine40(frame.line1, tmp);
                } else {
                    formatFxParamsHeader(tmp, sizeof(tmp), pfx.data(), '>', voiceEffectCursor_, spec, bypassed);
                    setLine40(frame.line0, tmp);
                    char v0[10]{}, v1[10]{}, v2[10]{};
                    formatFxParamValue(spec.params[0], fx.param1, v0);
                    formatFxParamValue(spec.params[1], fx.param2, v1);
                    formatFxParamValue(spec.params[2], fx.param3, v2);
                    std::snprintf(tmp, sizeof(tmp), "           %-7.7s %-7.7s %-7.7s      ", v0, v1, v2);
                    setLine40(frame.line1, tmp);
                }
                return true;
            }

            default:
                break;
        }
    }

    // -------------------------------------------------------------------------
    // CHAPTER 6: UTILITY OTHERS, STORE & COPY
    // -------------------------------------------------------------------------
    switch (screen_) {
        case ClassicScreen::UtilOthersVoice:
            if (utilConfirmPending_) {
                setLine40(frame.line0, "UT  OTHERS> VOICE    Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else {
                setLine40(frame.line0, "UT  OTHERS> VOICE       Select one !    ");
                setLine40(frame.line1, ">Init >Recall                >PresetLoad");
            }
            return true;

        case ClassicScreen::UtilOthersPerf:
            if (utilConfirmPending_) {
                setLine40(frame.line0, "UT  OTHERS> PFM      Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else {
                setLine40(frame.line0, "UT  OTHERS> PFM         Select one !    ");
                setLine40(frame.line1, ">Init >Recall   >VoiceEdit   >PresetLoad");
            }
            return true;

        case ClassicScreen::UtilOthersPerfInit:
            if (utilConfirmPending_) {
                setLine40(frame.line0, "UT  OTHERS> PFM INIT Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else {
                setLine40(frame.line0, "UT  OTHERS> PFM  INIT   Select one !    ");
                setLine40(frame.line1, "SNGL DUAL SPLT 4LYR 8LYR SEQ4 SEQ8 >EXIT");
            }
            return true;

        case ClassicScreen::UtilOthersPresetLoad:
            if (utilConfirmPending_) {
                setLine40(frame.line0, "UT  OTHERS> PRESET   Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else {
                setLine40(frame.line0, "UT  OTHERS> PRESET LOAD Select one !    ");
                setLine40(frame.line1, "ALL      PEFCT MCT                 >EXIT");
            }
            return true;

        case ClassicScreen::UtilOthersMultiVoiceSelect: {
            if (multiVoiceExitSglConfirm_) {
                setLine40(frame.line0, "UT  OTHERS> VOICE EDIT                  ");
                setLine40(frame.line1, "** go to SGL mode Sure?                 ");
                return true;
            }
            setLine40(frame.line0, "UT  OTHERS> VOICE EDIT   which voice ?  ");
            clearLine40(frame.line1);
            const auto& pf = mem.perfEditSlot().perf;
            const bool isCardPerf = (mem.selectedPerfBank() == vf50::state::MemoryBank::UserRam) && cardBankActive_;
            for (int p = 0; p < 8; ++p) {
                const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                const char sep = (p < 7) ? '/' : ' ';
                if (!isPerfInstActive(pf, p)) {
                    std::snprintf(tmp, sizeof(tmp), "  *%c", sep);
                } else {
                    const char bC = (it.voiceBank != 0u) ? 'P' : (multiVoiceDirty_[static_cast<std::size_t>(p)] ? 'i' : (isCardPerf ? 'C' : 'I'));
                    std::snprintf(tmp, sizeof(tmp), "%c%02d%c", bC, static_cast<int>(it.voiceNumber), sep);
                }
                writeSpan40(frame.line1, 1 + p * 4, tmp, 4);
            }
            return true;
        }

        case ClassicScreen::StoreVoice: {
            setLine40(frame.line0, "SINGLE MODE>                            ");
            const char srcB = (mem.selectedVoiceBank() == vf50::state::MemoryBank::FactoryRom) ? 'P' : (cardBankActive_ ? 'C' : 'I');
            const char dstB = storeTargetCard_ ? 'C' : 'I';
            const bool protShown = !storeTargetCard_ && processor_.isIntMemoryProtected() && !storeIntProtBypassed_;
            const char* okStr = storeConfirmReady_ ? "OK?" : "   ";
            const char* prStr = protShown ? "Prot?" : "     ";
            if (storePendingTens_ >= 0) {
                std::snprintf(tmp, sizeof(tmp), "     Mem Store %c%02d -> %c%d?  %s   %s ",
                              srcB, static_cast<int>(mem.selectedVoiceIndex()), dstB, storePendingTens_, okStr, prStr);
            } else {
                std::snprintf(tmp, sizeof(tmp), "     Mem Store %c%02d -> %c%02d %s   %s ",
                              srcB, static_cast<int>(mem.selectedVoiceIndex()), dstB, storeTargetSlot_, okStr, prStr);
            }
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::StorePerf: {
            setLine40(frame.line0, "PERFORMANCE MODE>                       ");
            const char srcB = (mem.selectedPerfBank() == vf50::state::MemoryBank::FactoryRom) ? 'P' : (cardBankActive_ ? 'C' : 'I');
            const char dstB = storeTargetCard_ ? 'C' : 'I';
            const bool protShown = !storeTargetCard_ && processor_.isIntMemoryProtected() && !storeIntProtBypassed_;
            const char* okStr = storeConfirmReady_ ? "OK?" : "   ";
            const char* prStr = protShown ? "Prot?" : "     ";
            if (storePendingTens_ >= 0) {
                std::snprintf(tmp, sizeof(tmp), "     PFM Store %c%02d -> %c%d?  %s   %s ",
                              srcB, static_cast<int>(mem.selectedPerfIndex()), dstB, storePendingTens_, okStr, prStr);
            } else {
                std::snprintf(tmp, sizeof(tmp), "     PFM Store %c%02d -> %c%02d %s   %s ",
                              srcB, static_cast<int>(mem.selectedPerfIndex()), dstB, storeTargetSlot_, okStr, prStr);
            }
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::StoreMultiVoice: {
            setLine40(frame.line0, "UT OTHERS>  STORE VOICE  which voice ?  ");
            clearLine40(frame.line1);
            const auto& pf = mem.perfEditSlot().perf;
            for (int p = 0; p < 8; ++p) {
                const auto& it = pf.instruments[static_cast<std::size_t>(p)];
                const char sep = (p < 7) ? '/' : ' ';
                if (!isPerfInstActive(pf, p)) {
                    std::snprintf(tmp, sizeof(tmp), "  *%c", sep);
                } else {
                    const char bC = (it.voiceBank != 0u) ? 'P' : (multiVoiceDirty_[static_cast<std::size_t>(p)] ? 'i' : (storeTargetCard_ ? 'C' : 'I'));
                    std::snprintf(tmp, sizeof(tmp), "%c%02d%c", bC, static_cast<int>(it.voiceNumber), sep);
                }
                writeSpan40(frame.line1, 1 + p * 4, tmp, 4);
            }
            if (storeConfirmReady_) writeSpan40(frame.line1, 35, "OK?", 3);
            return true;
        }

        case ClassicScreen::CopyEffect: {
            const auto& fx = (storeReturnScreen_ == ClassicScreen::PerfEditEffect)
                                 ? mem.perfEditSlot().perf.effect
                                 : mem.singleEditSlot().voice.effect;
            const auto& spec = kDspEffectSpecs[std::min<std::size_t>(fx.effectType, 32u)];
            std::snprintf(tmp, sizeof(tmp), "EFFECT COPY>  EFCT=%d:%-16.16s  ",
                          static_cast<int>(fx.effectType), spec.fullName);
            setLine40(frame.line0, tmp);
            const char dstB = storeTargetCard_ ? 'C' : 'I';
            const bool protShown = !storeTargetCard_ && processor_.isIntMemoryProtected() && !storeIntProtBypassed_;
            std::snprintf(tmp, sizeof(tmp), "      current effect data -> %c%02d %s  ",
                          dstB, storeTargetSlot_, protShown ? "Prot?" : "     ");
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::CopyPerfEffect: {
            setLine40(frame.line0, "PFM EFFECT COPY>                        ");
            const int fam = copyPerfFxSrcIdx_ / 4;
            const int src1To4 = (copyPerfFxSrcIdx_ % 4) + 1;
            const char* famName = (fam == 0) ? "delay" : ((fam == 1) ? "pan  " : "chord");
            std::snprintf(tmp, sizeof(tmp), "      %-5.5s %d -->  %-5.5s %d              ",
                          famName, src1To4, famName, copyPerfFxDstSlot_ + 1);
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::CopyEg: {
            setLine40(frame.line0, "EG COPY>  set source & destination OP ! ");
            char sCh = (copyEgSrcOp_ >= 0) ? static_cast<char>('1' + copyEgSrcOp_) : '?';
            char dCh = (copyEgDstOp_ >= 0) ? static_cast<char>('1' + copyEgDstOp_) : '?';
            std::snprintf(tmp, sizeof(tmp), "          %cOP%c --> %cOP%c                 ",
                          (copyEgCursor_ == 0) ? kGlyphPlayArrow : '>', sCh,
                          (copyEgCursor_ == 1) ? kGlyphPlayArrow : '>', dCh);
            setLine40(frame.line1, tmp);
            return true;
        }

        default:
            return false;
    }
}

void VF50ClassicController::applyEditLedSnapshot(ClassicLedSnapshot& leds) const noexcept {
    const auto& mem = processor_.hostMemory();
    if (isPerfEditScreen(screen_)) {
        const auto& pf = mem.activePerformance();
        for (std::size_t p = 0u; p < 8u; ++p) {
            if (((perfInstMuteMask_ >> p) & 1u) != 0u) {
                leds.tr[p] = LedState::BlinkRed;
            } else if (isPerfInstActive(pf, static_cast<int>(p))) {
                leds.tr[p] = LedState::SolidGreen;
            } else {
                leds.tr[p] = LedState::Off;
            }
        }
        if (screen_ == ClassicScreen::PerfEditVoiceNo) {
            const auto& inst = pf.instruments[static_cast<std::size_t>(perfEditInst_)];
            leds.memInt = LedState::Off;
            leds.memCard = LedState::Off;
            leds.memPreset = LedState::Off;
            if (inst.voiceBank != 0u) leds.memPreset = LedState::SolidGreen;
            else if (cardBankActive_) leds.memCard = LedState::SolidGreen;
            else leds.memInt = LedState::SolidGreen;
        }
    } else if (screen_ == ClassicScreen::StoreVoice || screen_ == ClassicScreen::StorePerf
               || screen_ == ClassicScreen::StoreMultiVoice || screen_ == ClassicScreen::CopyEffect) {
        leds.memPreset = LedState::Off;
        leds.memCard = storeTargetCard_ ? LedState::SolidGreen : LedState::Off;
        leds.memInt  = storeTargetCard_ ? LedState::Off : LedState::SolidGreen;
    }
}

bool VF50ClassicController::onProgrammerVoiceParamEdited(vf50::params::ParamIndex paramIndex) noexcept {
    using vf50::params::ParamIndex;

    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;

    if (isCompareModeLocked()) {
        setNotice("Compare Active - Locked", 45);
        return false;
    }

    ensureVoiceEditBackupInitialized();
    processor_.stateMachine().setClassicTopMode(vf50::state::ClassicTopMode::MODE_VOICE);

    if (multiVoiceEditActive_) {
        multiVoiceDirty_[static_cast<std::size_t>(std::clamp(multiVoiceEditInst_, 0, 7))] = true;
        saveActiveMultiVoiceBackToSlot();
    }

    const auto raw = static_cast<std::size_t>(paramIndex);
    if (raw >= vf50::params::kOperatorsStart && raw < vf50::params::kPerfPartsStart) {
        const std::size_t rel   = raw - vf50::params::kOperatorsStart;
        const int opIdx         = static_cast<int>(rel / vf50::params::kParamsPerOperator);
        const std::size_t field = rel % vf50::params::kParamsPerOperator;
        selectedOperator_       = std::clamp(opIdx, 0, 3);

        if (field == 0u) {
            screen_ = ClassicScreen::VoiceEditAlgorithm;
            voiceAlgCursor_ = 0;
        } else if (field >= 1u && field <= 7u) {
            screen_ = ClassicScreen::VoiceEditOsc;
            if (field == 1u)      voiceOscCursor_ = 0; // Wave
            else if (field == 2u) voiceOscCursor_ = 1; // Mode (Ratio/Fix)
            else if (field == 5u) voiceOscCursor_ = 2; // Coarse
            else if (field == 6u) voiceOscCursor_ = 3; // Fine
            else if (field == 7u) voiceOscCursor_ = 4; // Detune
            else                  voiceOscCursor_ = 1;
        } else if (field >= 8u && field <= 13u) {
            screen_ = ClassicScreen::VoiceEditEg;
            voiceEgCursor_ = static_cast<int>(field - 8u);
        } else if (field == 14u) {
            screen_ = ClassicScreen::VoiceEditOutLevel;
            voiceOutCursor_ = selectedOperator_;
        } else if (field == 15u || field == 16u) {
            screen_ = ClassicScreen::VoiceEditScaling;
            voiceScalingSubPage_ = (field == 15u) ? 0 : 1;
            voiceScalingCursor_  = selectedOperator_;
        } else {
            screen_ = ClassicScreen::VoiceEditSens;
            if (field == 17u)      voiceSensCursor_ = 2; // KVS
            else if (field == 18u) voiceSensCursor_ = 3; // EBS
            else                   voiceSensCursor_ = 4; // AME
        }
        return true;
    }

    switch (paramIndex) {
        case ParamIndex::VoiceAlgorithm:
            screen_ = ClassicScreen::VoiceEditAlgorithm;
            voiceAlgCursor_ = 0;
            break;
        case ParamIndex::VoiceFeedback:
            screen_ = ClassicScreen::VoiceEditAlgorithm;
            voiceAlgCursor_ = 1;
            break;
        case ParamIndex::QuickAttack:
            screen_ = ClassicScreen::VoiceEditQuick;
            voiceQuickSubPage_ = 0;
            break;
        case ParamIndex::QuickRelease:
            screen_ = ClassicScreen::VoiceEditQuick;
            voiceQuickSubPage_ = 1;
            break;
        case ParamIndex::QuickTotalLevel:
            screen_ = ClassicScreen::VoiceEditQuick;
            voiceQuickSubPage_ = 2;
            break;
        case ParamIndex::QuickBrilliance:
            screen_ = ClassicScreen::VoiceEditQuick;
            voiceQuickSubPage_ = 3;
            break;
        case ParamIndex::VoiceTranspose:
            screen_ = ClassicScreen::VoiceEditTranspose;
            voiceTransposeCursor_ = 0;
            break;
        case ParamIndex::PegPr1:
        case ParamIndex::PegPr2:
        case ParamIndex::PegPr3:
        case ParamIndex::PegPl1:
        case ParamIndex::PegPl2:
        case ParamIndex::PegPl3:
            screen_ = ClassicScreen::VoiceEditPeg;
            voicePegCursor_ = static_cast<int>(raw - static_cast<std::size_t>(ParamIndex::PegPr1));
            break;
        case ParamIndex::Lfo1Wave:
        case ParamIndex::Lfo1Speed:
        case ParamIndex::Lfo1Delay:
        case ParamIndex::Lfo1Pmd:
        case ParamIndex::Lfo1Amd:
        case ParamIndex::Lfo1Sync:
            screen_ = ClassicScreen::VoiceEditLfo;
            voiceLfoCursor_ = static_cast<int>(raw - static_cast<std::size_t>(ParamIndex::Lfo1Wave));
            break;
        case ParamIndex::Lfo1Pms:
            screen_ = ClassicScreen::VoiceEditSens;
            voiceSensCursor_ = 0;
            break;
        case ParamIndex::Lfo1Ams:
            screen_ = ClassicScreen::VoiceEditSens;
            voiceSensCursor_ = 1;
            break;
        default:
            break;
    }
    return true;
}

} // namespace vf50::classic
