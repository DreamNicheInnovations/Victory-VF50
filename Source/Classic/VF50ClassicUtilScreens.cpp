#include "Classic/VF50ClassicController.h"
#include "MIDI/VF50SysExHandler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vf50::classic {
namespace {

constexpr char kCursorRight = '\x10';
constexpr char kArrowRight  = '\x7E';
constexpr char kArrowLeft   = '\x7F';
constexpr char kArrowUp     = '\x09';
constexpr char kArrowDown   = '\x0A';

void setLineUtil(std::array<char, 40>& dst, std::string_view src) noexcept {
    dst.fill(' ');
    const std::size_t n = std::min<std::size_t>(src.size(), dst.size());
    for (std::size_t i = 0u; i < n; ++i) {
        dst[i] = src[i];
    }
}

void formatMidiNote4(int midiNote, char out[5]) noexcept {
    static constexpr std::array<const char*, 12> kNames{ {
        "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
    } };
    const int n = std::clamp(midiNote, 0, 127);
    const int oct = (n / 12) - 2; // Hardware C3 = MIDI 60
    const int pc = n % 12;
    if (oct < 0) {
        std::snprintf(out, 5, "%s%d", kNames[static_cast<std::size_t>(pc)], oct);
    } else {
        std::snprintf(out, 5, "%s%d ", kNames[static_cast<std::size_t>(pc)], oct);
    }
}

void formatMidiNote3(int midiNote, char out[4]) noexcept {
    static constexpr std::array<const char*, 12> kNames{ {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "B"
    } };
    static constexpr std::array<const char*, 12> kFullNames{ {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    } };
    (void) kNames;
    if (midiNote < 0 || midiNote > 127) {
        out[0] = ' ';
        out[1] = '*';
        out[2] = ' ';
        out[3] = '\0';
        return;
    }
    const int oct = (midiNote / 12) - 2;
    const int pc = midiNote % 12;
    const char* nStr = kFullNames[static_cast<std::size_t>(pc)];
    if (std::strlen(nStr) == 2u) {
        if (oct < 0) {
            out[0] = nStr[0];
            out[1] = '#';
            out[2] = '-';
            out[3] = '\0';
        } else {
            out[0] = nStr[0];
            out[1] = nStr[1];
            out[2] = static_cast<char>('0' + std::clamp(oct, 0, 9));
            out[3] = '\0';
        }
    } else {
        if (oct < 0) {
            out[0] = nStr[0];
            out[1] = '-';
            out[2] = static_cast<char>('0' + std::clamp(-oct, 0, 9));
            out[3] = '\0';
        } else {
            out[0] = ' ';
            out[1] = nStr[0];
            out[2] = static_cast<char>('0' + std::clamp(oct, 0, 9));
            out[3] = '\0';
        }
    }
}

void formatGlobalOrNormMode(uint8_t mode, char out[5]) noexcept {
    if (mode == 0u) {
        std::snprintf(out, 5, "off ");
    } else if (mode == 1u) {
        std::snprintf(out, 5, "norm");
    } else {
        const int g = std::clamp(static_cast<int>(mode) - 1, 1, 16);
        std::snprintf(out, 5, "G%-2d ", g);
    }
}

std::string_view dataEntryCcLabel(uint8_t cc) noexcept {
    switch (cc) {
        case 1u:  return "Mod.W";
        case 2u:  return "BC   ";
        case 4u:  return "FC   ";
        case 5u:  return "Por.T";
        case 6u:  return "D.Ent";
        case 7u:  return "Vol. ";
        case 8u:  return "Bal. ";
        case 10u: return "Pan  ";
        case 11u: return "Exp  ";
        default:  return "undef";
    }
}

std::string_view velCurveLabel(uint8_t c) noexcept {
    static constexpr std::array<std::string_view, 8> kLabels{ {
        "0(norm)  ", "1(curve1)", "2(curve2)", "3(curve3)",
        "4(curve4)", "5(curve5)", "6(flat)  ", "7(revers)"
    } };
    return kLabels[std::min<std::size_t>(c, 7u)];
}

std::string_view egDampLabel(uint8_t speed) noexcept {
    static constexpr std::array<std::string_view, 4> kLabels{ {
        "veryslow", "slow    ", "medium  ", "fast    "
    } };
    return kLabels[std::min<std::size_t>(speed, 3u)];
}

std::string_view pgmCngModeLabel(uint8_t m) noexcept {
    static constexpr std::array<std::string_view, 5> kLabels{ {
        "off       ", "common    ", "individual", "direct    ", "TrnsFilter"
    } };
    return kLabels[std::min<std::size_t>(m, 4u)];
}

std::string_view microTableShortName(int idx0To10) noexcept {
    static constexpr std::array<std::string_view, 11> kNames{ {
        "Equal        ",
        "Pure(major)  ",
        "Pure(minor)  ",
        "Mean tone    ",
        "Pythagorean  ",
        "Werckmeister ",
        "Kirnberger   ",
        "Vallotti&yng ",
        "1/4 Shift eql",
        "1/4 Tone     ",
        "1/8 Tone     "
    } };
    return kNames[static_cast<std::size_t>(std::clamp(idx0To10, 0, 10))];
}

int wrapRange(int v, int delta, int lo, int hi) noexcept {
    const int span = (hi - lo) + 1;
    if (span <= 0) {
        return lo;
    }
    int r = (v - lo + delta) % span;
    if (r < 0) {
        r += span;
    }
    return lo + r;
}

uint8_t sysExDeviceIdFromSetting(uint8_t devSetting) noexcept {
    if (devSetting >= 1u && devSetting <= 16u) {
        return static_cast<uint8_t>(devSetting - 1u);
    }
    return 0u; // 'all' transmits on channel 1 (ID 0)
}

void copyFactoryMicroTableToUser(const vf50::state::VF50MemoryManager& mem,
                                 int tableIdx0To10,
                                 int tonic0To11,
                                 bool isFull,
                                 vf50::state::VF50MemoryManager& dstMem) noexcept {
    const auto& rom = mem.runtimeRomBank();
    const std::size_t ti = static_cast<std::size_t>(std::clamp(tableIdx0To10, 0, 10));
    const auto& srcTbl = rom.factoryMicrotunings[ti];
    if (!isFull) {
        std::array<vf50::generated::MicrotuningEntry, 12> outOct{};
        for (int i = 0; i < 12; ++i) {
            const int srcIdx = (i - tonic0To11 + 12) % 12;
            outOct[static_cast<std::size_t>(i)] = srcTbl.baseOctave[static_cast<std::size_t>(srcIdx)];
            outOct[static_cast<std::size_t>(i)].coarseNote = static_cast<uint8_t>(60 + i);
        }
        dstMem.setUserOctaveMicrotuning(outOct);
    } else {
        std::array<vf50::generated::MicrotuningEntry, 128> outFull{};
        for (int n = 0; n < 128; ++n) {
            const int pc = n % 12;
            const int srcPc = (pc - tonic0To11 + 12) % 12;
            const auto& base = srcTbl.keys[static_cast<std::size_t>(std::clamp(60 + srcPc, 0, 127))];
            outFull[static_cast<std::size_t>(n)].coarseNote = static_cast<uint8_t>(std::clamp(n, 0, 127));
            outFull[static_cast<std::size_t>(n)].fineStep64 = base.fineStep64;
        }
        dstMem.setUserFullMicrotuning(outFull);
    }
}

constexpr std::array<std::string_view, 10> kMsgMultiTapSets{ {
    "0+-.,!?",
    "1ABCabc",
    "2DEFdef",
    "3GHIghi",
    "4JKLjkl",
    "5MNOmno",
    "6PQRpqr",
    "7STUstu",
    "8VWXvwx",
    "9 YZyz "
} };

} // namespace

bool VF50ClassicController::isCardScreen(ClassicScreen s) noexcept {
    switch (s) {
        case ClassicScreen::CardRoot:
        case ClassicScreen::CardFormat:
        case ClassicScreen::CardSaveCategory:
        case ClassicScreen::CardSaveSub:
        case ClassicScreen::CardLoadCategory:
        case ClassicScreen::CardLoadSub:
            return true;
        default:
            return false;
    }
}

bool VF50ClassicController::isMidiScreen(ClassicScreen s) noexcept {
    switch (s) {
        case ClassicScreen::MidiRoot:
        case ClassicScreen::MidiChInfo:
        case ClassicScreen::MidiSwitch:
        case ClassicScreen::MidiCond:
        case ClassicScreen::MidiPgmCng:
        case ClassicScreen::MidiPgmInit:
        case ClassicScreen::MidiPgmEdit:
        case ClassicScreen::MidiExclusive:
        case ClassicScreen::MidiBulkRoot:
        case ClassicScreen::MidiBulkSub:
            return true;
        default:
            return false;
    }
}

bool VF50ClassicController::isSetupScreen(ClassicScreen s) noexcept {
    switch (s) {
        case ClassicScreen::SetupRoot:
        case ClassicScreen::SetupTune:
        case ClassicScreen::SetupCombine:
        case ClassicScreen::SetupCtrlReset:
        case ClassicScreen::SetupPerfEfctCat:
        case ClassicScreen::SetupPerfEfctSlot:
        case ClassicScreen::SetupDelayEdit:
        case ClassicScreen::SetupPanEdit:
        case ClassicScreen::SetupChordEdit:
        case ClassicScreen::SetupMicroRoot:
        case ClassicScreen::SetupMicroOctEdit:
        case ClassicScreen::SetupMicroOctInit:
        case ClassicScreen::SetupMicroFullEdit:
        case ClassicScreen::SetupMicroFullInit:
        case ClassicScreen::SetupVel:
        case ClassicScreen::SetupDamp:
            return true;
        default:
            return false;
    }
}

bool VF50ClassicController::isDemoOrMsgScreen(ClassicScreen s) noexcept {
    switch (s) {
        case ClassicScreen::DemoRoot:
        case ClassicScreen::DemoConfirm:
        case ClassicScreen::DemoPlay:
        case ClassicScreen::DemoVoiceEdit:
        case ClassicScreen::UtilEditMessage:
            return true;
        default:
            return false;
    }
}

bool VF50ClassicController::isPhase3UtilScreen(ClassicScreen s) noexcept {
    return isCardScreen(s) || isMidiScreen(s) || isSetupScreen(s) || isDemoOrMsgScreen(s);
}

void VF50ClassicController::clearAuditionOverridesIfNeeded(ClassicScreen nextScreen) noexcept {
    if (nextScreen != ClassicScreen::SetupDelayEdit &&
        nextScreen != ClassicScreen::SetupPanEdit &&
        nextScreen != ClassicScreen::SetupChordEdit) {
        processor_.setClassicAuditionPerfEffect(0u);
    }
    if (nextScreen != ClassicScreen::SetupMicroOctEdit &&
        nextScreen != ClassicScreen::SetupMicroFullEdit) {
        processor_.setClassicAuditionMicroTuneMode(0u);
    }
    if (nextScreen != ClassicScreen::SetupChordEdit) {
        setupChordKbdIn_ = false;
    }
}

bool VF50ClassicController::onPressCard() noexcept {
    bootSplashTicks_ = 0;
    clearNotice();
    directPcMode_ = 0;
    directPcDigits_.clear();
    cardConfirmPending_ = false;
    cardPendingAction_ = 0;
    clearAuditionOverridesIfNeeded(ClassicScreen::CardRoot);
    if (!isPhase3UtilScreen(screen_) && !isPerfEditScreen(screen_) && !isVoiceEditScreen(screen_) &&
        !isUtilOthersScreen(screen_) && !isStoreOrCopyScreen(screen_) &&
        screen_ != ClassicScreen::DiskRoot && screen_ != ClassicScreen::UtilMemoryProtect &&
        screen_ != ClassicScreen::PowerOnGreeting) {
        preUtilityScreen_ = screen_;
    }
    screen_ = ClassicScreen::CardRoot;
    return true;
}

bool VF50ClassicController::onPressMidi() noexcept {
    bootSplashTicks_ = 0;
    clearNotice();
    directPcMode_ = 0;
    directPcDigits_.clear();
    midiPgmInitConfirm_ = false;
    midiBulkConfirm_ = false;
    clearAuditionOverridesIfNeeded(ClassicScreen::MidiRoot);
    if (!isPhase3UtilScreen(screen_) && !isPerfEditScreen(screen_) && !isVoiceEditScreen(screen_) &&
        !isUtilOthersScreen(screen_) && !isStoreOrCopyScreen(screen_) &&
        screen_ != ClassicScreen::DiskRoot && screen_ != ClassicScreen::UtilMemoryProtect &&
        screen_ != ClassicScreen::PowerOnGreeting) {
        preUtilityScreen_ = screen_;
    }
    screen_ = ClassicScreen::MidiRoot;
    return true;
}

bool VF50ClassicController::onPressSetup() noexcept {
    bootSplashTicks_ = 0;
    clearNotice();
    directPcMode_ = 0;
    directPcDigits_.clear();
    setupMicroInitConfirm_ = false;
    clearAuditionOverridesIfNeeded(ClassicScreen::SetupRoot);
    if (!isPhase3UtilScreen(screen_) && !isPerfEditScreen(screen_) && !isVoiceEditScreen(screen_) &&
        !isUtilOthersScreen(screen_) && !isStoreOrCopyScreen(screen_) &&
        screen_ != ClassicScreen::DiskRoot && screen_ != ClassicScreen::UtilMemoryProtect &&
        screen_ != ClassicScreen::PowerOnGreeting) {
        preUtilityScreen_ = screen_;
    }
    screen_ = ClassicScreen::SetupRoot;
    return true;
}

bool VF50ClassicController::onPressDemo() noexcept {
    bootSplashTicks_ = 0;
    clearNotice();
    directPcMode_ = 0;
    directPcDigits_.clear();
    demoBusyTicks_ = 0;
    clearAuditionOverridesIfNeeded(ClassicScreen::DemoRoot);
    if (!isPhase3UtilScreen(screen_) && !isPerfEditScreen(screen_) && !isVoiceEditScreen(screen_) &&
        !isUtilOthersScreen(screen_) && !isStoreOrCopyScreen(screen_) &&
        screen_ != ClassicScreen::DiskRoot && screen_ != ClassicScreen::UtilMemoryProtect &&
        screen_ != ClassicScreen::PowerOnGreeting) {
        preUtilityScreen_ = screen_;
    }
    screen_ = ClassicScreen::DemoRoot;
    return true;
}

bool VF50ClassicController::onPressDemoAndStore() noexcept {
    bootSplashTicks_ = 0;
    clearNotice();
    directPcMode_ = 0;
    directPcDigits_.clear();
    msgEditCursorCol_ = 0;
    msgMultiTapDigit_ = -1;
    msgMultiTapCycle_ = 0;
    clearAuditionOverridesIfNeeded(ClassicScreen::UtilEditMessage);
    screen_ = ClassicScreen::UtilEditMessage;
    return true;
}

void VF50ClassicController::onChordKbdNoteOn(uint8_t midiNote) noexcept {
    if (screen_ != ClassicScreen::SetupChordEdit || !setupChordKbdIn_) {
        return;
    }
    auto& chordMem = processor_.mutablePerfEffectBank().chords[static_cast<std::size_t>(std::clamp(setupPerfFxSlot_, 0, 3))];
    auto& notes = chordMem.chords[static_cast<std::size_t>(std::clamp(setupChordKey_, 0, 11))];
    if (setupChordKbdWriteIdx_ == 0) {
        notes = { static_cast<int8_t>(std::clamp<int>(midiNote, 0, 127)), -1, -1, -1 };
    } else {
        notes[static_cast<std::size_t>(setupChordKbdWriteIdx_ & 3)] = static_cast<int8_t>(std::clamp<int>(midiNote, 0, 127));
    }
    setupChordSlot_ = setupChordKbdWriteIdx_ & 3;
    setupChordKbdWriteIdx_ = (setupChordKbdWriteIdx_ + 1) & 3;
    processor_.publishClassicSystemStateToAudio();
}

void VF50ClassicController::onMicroKbdNoteOn(uint8_t midiNote) noexcept {
    if (screen_ == ClassicScreen::SetupMicroOctEdit) {
        setupMicroKey_ = static_cast<int>(midiNote % 12u);
    } else if (screen_ == ClassicScreen::SetupMicroFullEdit) {
        setupMicroKey_ = static_cast<int>(std::clamp<uint8_t>(midiNote, 0u, 127u));
    }
}

void VF50ClassicController::tickPhase3Demo30Hz() noexcept {
    if (demoBusyTicks_ > 0) {
        --demoBusyTicks_;
    }
    if (screen_ == ClassicScreen::DemoVoiceEdit) {
        const int intervalTicks = std::max(2, static_cast<int>(std::lround(18.0f - demoVEditSpeed_ * 15.0f)));
        ++demoVEditAccum_;
        if (demoVEditAccum_ >= intervalTicks) {
            demoVEditAccum_ = 0;
            demoVEditStep_ = (demoVEditStep_ + 1) % 32;
            auto& slot = processor_.hostMemory().singleEditSlot();
            const uint8_t alg = static_cast<uint8_t>((slot.voice.algorithm + 1u) % 8u);
            slot.voice.algorithm = alg;
            processor_.pushHostMemoryToAudio();
        }
    }
}

bool VF50ClassicController::handleUtilSoftKey(int k) noexcept {
    clearNotice();
    auto& card = processor_.mutableVirtualCard();
    auto& sys = processor_.mutableClassicSystemSettings();
    auto& pef = processor_.mutablePerfEffectBank();

    switch (screen_) {
        // =====================================================================
        // Chapter 6: CARD (pp. 95-98)
        // =====================================================================
        case ClassicScreen::CardRoot: {
            cardConfirmPending_ = false;
            if (k >= 1 && k <= 3) {
                if (card.bankCount() > 1) {
                    card.activeBankIndex = static_cast<uint8_t>((card.activeBankIndex + 1u) & 1u);
                }
                return true;
            }
            if (k == 5) { // >Form
                screen_ = ClassicScreen::CardFormat;
                return true;
            }
            if (k == 6) { // >Save
                screen_ = ClassicScreen::CardSaveCategory;
                return true;
            }
            if (k == 7) { // >Load
                screen_ = ClassicScreen::CardLoadCategory;
                return true;
            }
            return true;
        }
        case ClassicScreen::CardFormat: {
            if (k == 7) { // >EXIT
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardRoot;
                return true;
            }
            if (k == 3 || k == 4 || k == 5 || k == 6) {
                if (card.hardwareWriteProtect || processor_.isCardMemoryProtected()) {
                    setNotice("Memory Protected", 60);
                    return true;
                }
                if (card.isReadOnlyMedia()) {
                    setNotice("ROM Card!", 60);
                    return true;
                }
                cardPendingAction_ = 1;
                cardPendingSubType_ = (k == 3 || k == 4) ? 1 : 2; // 1=VF50SYN, 2=VF50RHY
                cardConfirmPending_ = true;
                return true;
            }
            return true;
        }
        case ClassicScreen::CardSaveCategory: {
            if (k == 7) {
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardRoot;
                return true;
            }
            if (k == 0) {
                cardSubCategory_ = 0; // Synth
                screen_ = ClassicScreen::CardSaveSub;
                return true;
            }
            if (k == 2 || k == 3) {
                cardSubCategory_ = 1; // Rhythm(toA)
                screen_ = ClassicScreen::CardSaveSub;
                return true;
            }
            if (k >= 4 && k <= 6) {
                cardSubCategory_ = 2; // Rhythm(toB)
                screen_ = ClassicScreen::CardSaveSub;
                return true;
            }
            return true;
        }
        case ClassicScreen::CardSaveSub: {
            if (k == 7) {
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardSaveCategory;
                return true;
            }
            int chosenSub = -1;
            if (cardSubCategory_ == 0) {
                if (k >= 0 && k <= 6) {
                    chosenSub = k; // 0=SynAL, 1=V&PF, 2=SetAL, 3=PCT, 4=PEFCT, 5=MCT, 6=SYS
                }
            } else {
                if (k == 1 || k == 2) chosenSub = 0;      // RhyALL
                else if (k == 3 || k == 4) chosenSub = 1; // R.SEQ
                else if (k == 5 || k == 6) chosenSub = 2; // SETUP
            }
            if (chosenSub >= 0) {
                if (card.hardwareWriteProtect || processor_.isCardMemoryProtected()) {
                    setNotice("Memory Protected", 60);
                    return true;
                }
                if (card.isReadOnlyMedia()) {
                    setNotice("ROM Card!", 60);
                    return true;
                }
                const auto fmt = card.activeBank().format;
                const bool fmtOk = (cardSubCategory_ == 0) ? (fmt == vf50::state::CardBankFormat::VF50Syn)
                                                          : (fmt == vf50::state::CardBankFormat::VF50Rhy);
                if (!fmtOk) {
                    setNotice(fmt == vf50::state::CardBankFormat::Unformatted ? "Unformatted!" : "Illegal format!", 60);
                    return true;
                }
                cardPendingAction_ = 2;
                cardPendingSubType_ = chosenSub;
                cardConfirmPending_ = true;
            }
            return true;
        }
        case ClassicScreen::CardLoadCategory: {
            if (k == 7) {
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardRoot;
                return true;
            }
            if (k == 0) {
                cardSubCategory_ = 0; // Synth
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardLoadSub;
                return true;
            }
            if (k == 1 || k == 2) {
                cardSubCategory_ = 1; // Rhythm(A)
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardLoadSub;
                return true;
            }
            if (k == 3 || k == 4) {
                cardSubCategory_ = 2; // Rhythm(B)
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardLoadSub;
                return true;
            }
            if (k == 5 || k == 6) {
                if (processor_.isIntMemoryProtected()) {
                    setNotice("Memory Protected", 60);
                    return true;
                }
                cardSubCategory_ = 3; // SEQ(YS) -> immediate confirmation
                cardPendingAction_ = 3;
                cardPendingSubType_ = 0;
                cardConfirmPending_ = true;
                return true;
            }
            return true;
        }
        case ClassicScreen::CardLoadSub: {
            if (k == 7) {
                cardConfirmPending_ = false;
                screen_ = ClassicScreen::CardLoadCategory;
                return true;
            }
            int chosenSub = -1;
            if (cardSubCategory_ == 0) {
                if (k >= 0 && k <= 6) {
                    chosenSub = k;
                }
            } else {
                if (k == 1 || k == 2) chosenSub = 0;      // RhyALL
                else if (k == 3 || k == 4) chosenSub = 1; // R.SEQ
                else if (k == 5 || k == 6) chosenSub = 2; // SETUP
            }
            if (chosenSub >= 0) {
                const bool isRSeqException = (cardSubCategory_ != 0 && chosenSub == 1);
                if (processor_.isIntMemoryProtected() && !isRSeqException) {
                    setNotice("Memory Protected", 60);
                    return true;
                }
                cardPendingAction_ = 3;
                cardPendingSubType_ = chosenSub;
                cardConfirmPending_ = true;
            }
            return true;
        }

        // =====================================================================
        // Chapter 6: MIDI (pp. 99-104)
        // =====================================================================
        case ClassicScreen::MidiRoot: {
            if (k == 0) { screen_ = ClassicScreen::MidiChInfo; return true; }
            if (k == 1 || k == 2) { screen_ = ClassicScreen::MidiSwitch; return true; }
            if (k == 3) { screen_ = ClassicScreen::MidiCond; return true; }
            if (k == 4 || k == 5) { screen_ = ClassicScreen::MidiPgmCng; return true; }
            if (k == 6 || k == 7) { screen_ = ClassicScreen::MidiExclusive; return true; }
            return true;
        }
        case ClassicScreen::MidiChInfo: {
            if (k == 7) { screen_ = ClassicScreen::MidiRoot; return true; }
            if (k == 2) { midiChInfoCursor_ = 0; return true; }
            if (k == 3 || k == 4) { midiChInfoCursor_ = 1; return true; }
            if (k == 5) { midiChInfoCursor_ = 2; return true; }
            if (k == 6) { midiChInfoCursor_ = 3; return true; }
            return true;
        }
        case ClassicScreen::MidiSwitch: {
            if (k == 7) { screen_ = ClassicScreen::MidiRoot; return true; }
            if (k == 2 || k == 3) { midiSwitchCursor_ = 0; return true; }
            if (k == 4 || k == 5) { midiSwitchCursor_ = 1; return true; }
            if (k == 6) { midiSwitchCursor_ = 2; return true; }
            return true;
        }
        case ClassicScreen::MidiCond: {
            if (k == 7) { screen_ = ClassicScreen::MidiRoot; return true; }
            if (k >= 2 && k <= 4) { midiCondCursor_ = 0; return true; }
            if (k == 5 || k == 6) { midiCondCursor_ = 1; return true; }
            return true;
        }
        case ClassicScreen::MidiPgmCng: {
            if (k == 7) { screen_ = ClassicScreen::MidiRoot; return true; }
            if (k == 5) {
                midiPgmInitConfirm_ = false;
                screen_ = ClassicScreen::MidiPgmInit;
                return true;
            }
            if (k == 6) {
                midiPgmEditPendingTens_ = -1;
                screen_ = ClassicScreen::MidiPgmEdit;
                return true;
            }
            return true;
        }
        case ClassicScreen::MidiPgmInit: {
            if (k == 7) {
                midiPgmInitConfirm_ = false;
                screen_ = ClassicScreen::MidiPgmCng;
                return true;
            }
            if (k == 0 || k == 1) {
                return handleUtilYes();
            }
            return true;
        }
        case ClassicScreen::MidiPgmEdit: {
            if (k == 7) {
                midiPgmEditPendingTens_ = -1;
                screen_ = ClassicScreen::MidiPgmCng;
                return true;
            }
            if (k == 0) {
                midiPgmEditSlot_ = wrapRange(midiPgmEditSlot_, -1, 0, 127);
                midiPgmEditPendingTens_ = -1;
                return true;
            }
            if (k == 1) {
                midiPgmEditSlot_ = wrapRange(midiPgmEditSlot_, 1, 0, 127);
                midiPgmEditPendingTens_ = -1;
                return true;
            }
            return true;
        }
        case ClassicScreen::MidiExclusive: {
            if (k == 7) { screen_ = ClassicScreen::MidiRoot; return true; }
            if (k == 6) {
                midiBulkConfirm_ = false;
                screen_ = ClassicScreen::MidiBulkRoot;
                return true;
            }
            return true;
        }
        case ClassicScreen::MidiBulkRoot: {
            if (k == 7) {
                midiBulkConfirm_ = false;
                screen_ = ClassicScreen::MidiExclusive;
                return true;
            }
            int chosenCat = -1;
            if (k == 1) chosenCat = 0;      // VOICE
            else if (k == 2) chosenCat = 1; // PFM
            else if (k == 3) chosenCat = 2; // SETUP
            else if (k == 5) chosenCat = 3; // SEQ
            else if (k == 6) chosenCat = 4; // RHY
            if (chosenCat >= 0) {
                midiBulkCategory_ = chosenCat;
                midiBulkSubIdx_ = 0;
                midiBulkCursor_ = 0;
                midiBulkConfirm_ = (chosenCat >= 2); // SETUP/SEQ/RHY can confirm directly or switch sub
                screen_ = ClassicScreen::MidiBulkSub;
                return true;
            }
            return true;
        }
        case ClassicScreen::MidiBulkSub: {
            if (k == 7) {
                midiBulkConfirm_ = false;
                screen_ = ClassicScreen::MidiBulkRoot;
                return true;
            }
            if (midiBulkCategory_ <= 1) { // VOICE or PFM: F1..F2 bank, F3..F5 range, F6..F7 Transmit?
                if (k == 0 || k == 1) {
                    midiBulkCursor_ = 0;
                    midiBulkBank_ = (midiBulkBank_ + 1) % 3;
                } else if (k >= 2 && k <= 4) {
                    midiBulkCursor_ = 1;
                    midiBulkSubIdx_ = (midiBulkSubIdx_ + 1) % 5;
                } else if (k == 5 || k == 6) {
                    midiBulkConfirm_ = true;
                }
            } else {
                const int count = (midiBulkCategory_ == 2) ? 5 : 3;
                if (k >= 0 && k < count) {
                    midiBulkSubIdx_ = k;
                    midiBulkConfirm_ = true;
                } else if (k == 5 || k == 6) {
                    midiBulkConfirm_ = true;
                }
            }
            return true;
        }

        // =====================================================================
        // Chapter 6: UTILITY SETUP (pp. 110-116)
        // =====================================================================
        case ClassicScreen::SetupRoot: {
            if (k == 0) { screen_ = ClassicScreen::SetupTune; return true; }
            if (k == 1) { screen_ = ClassicScreen::SetupCombine; return true; }
            if (k == 2) { screen_ = ClassicScreen::SetupCtrlReset; return true; }
            if (k == 3) { screen_ = ClassicScreen::SetupPerfEfctCat; return true; }
            if (k == 4 || k == 5) { screen_ = ClassicScreen::SetupMicroRoot; return true; }
            if (k == 6) { screen_ = ClassicScreen::SetupVel; return true; }
            if (k == 7) { screen_ = ClassicScreen::SetupDamp; return true; }
            return true;
        }
        case ClassicScreen::SetupTune: {
            if (k == 7) { screen_ = ClassicScreen::SetupRoot; return true; }
            if (k >= 2 && k <= 4) { setupTuneCursor_ = 0; return true; }
            if (k == 5 || k == 6) { setupTuneCursor_ = 1; return true; }
            return true;
        }
        case ClassicScreen::SetupCombine: {
            if (k == 7) { screen_ = ClassicScreen::SetupRoot; return true; }
            if (k >= 2 && k <= 6) {
                sys.combineWithFunction = !sys.combineWithFunction;
                if (sys.combineWithFunction) {
                    combineBorrowedFunction_ = false;
                }
                processor_.publishClassicSystemStateToAudio();
                return true;
            }
            return true;
        }
        case ClassicScreen::SetupCtrlReset: {
            if (k == 7) { screen_ = ClassicScreen::SetupRoot; return true; }
            if (k >= 2 && k <= 5) {
                sys.controllerReset = !sys.controllerReset;
                processor_.publishClassicSystemStateToAudio();
                return true;
            }
            return true;
        }
        case ClassicScreen::SetupPerfEfctCat: {
            if (k == 7) { screen_ = ClassicScreen::SetupRoot; return true; }
            if (k == 0 || k == 1) {
                setupPerfFxCat_ = 0;
                screen_ = ClassicScreen::SetupPerfEfctSlot;
                return true;
            }
            if (k == 2 || k == 3) {
                setupPerfFxCat_ = 1;
                screen_ = ClassicScreen::SetupPerfEfctSlot;
                return true;
            }
            if (k == 4 || k == 5) {
                setupPerfFxCat_ = 2;
                screen_ = ClassicScreen::SetupPerfEfctSlot;
                return true;
            }
            return true;
        }
        case ClassicScreen::SetupPerfEfctSlot: {
            if (k == 7) {
                processor_.setClassicAuditionPerfEffect(0u);
                screen_ = ClassicScreen::SetupPerfEfctCat;
                return true;
            }
            int slot = -1;
            if (k == 0 || k == 1) slot = 0;
            else if (k == 2 || k == 3) slot = 1;
            else if (k == 4 || k == 5) slot = 2;
            else if (k == 6) slot = 3;
            if (slot >= 0) {
                setupPerfFxSlot_ = slot;
                const uint8_t audId = static_cast<uint8_t>(1 + setupPerfFxCat_ * 4 + slot);
                processor_.setClassicAuditionPerfEffect(audId);
                if (setupPerfFxCat_ == 0) {
                    setupDelayPage_ = 0;
                    setupDelayCursor_ = 0;
                    screen_ = ClassicScreen::SetupDelayEdit;
                } else if (setupPerfFxCat_ == 1) {
                    setupPanCursor_ = 0;
                    screen_ = ClassicScreen::SetupPanEdit;
                } else {
                    setupChordKbdIn_ = false;
                    setupChordKbdWriteIdx_ = 0;
                    screen_ = ClassicScreen::SetupChordEdit;
                }
                return true;
            }
            return true;
        }
        case ClassicScreen::SetupDelayEdit: {
            if (k == 7) {
                processor_.setClassicAuditionPerfEffect(0u);
                screen_ = ClassicScreen::SetupPerfEfctSlot;
                return true;
            }
            if (k == 6) {
                setupDelayPage_ = (setupDelayPage_ == 0) ? 1 : 0;
                setupDelayCursor_ = 0;
                return true;
            }
            if (k == 2 || k == 3) { setupDelayCursor_ = 0; return true; }
            if (k == 4 || k == 5) { setupDelayCursor_ = 1; return true; }
            return true;
        }
        case ClassicScreen::SetupPanEdit: {
            if (k == 7) {
                processor_.setClassicAuditionPerfEffect(0u);
                screen_ = ClassicScreen::SetupPerfEfctSlot;
                return true;
            }
            if (k == 2) { setupPanCursor_ = 0; return true; }
            if (k == 3 || k == 4) { setupPanCursor_ = 1; return true; }
            if (k == 5 || k == 6) { setupPanCursor_ = 2; return true; }
            return true;
        }
        case ClassicScreen::SetupChordEdit: {
            if (k == 7) {
                setupChordKbdIn_ = false;
                processor_.setClassicAuditionPerfEffect(0u);
                screen_ = ClassicScreen::SetupPerfEfctSlot;
                return true;
            }
            if (k == 0) { setupChordKey_ = wrapRange(setupChordKey_, -1, 0, 11); setupChordKbdWriteIdx_ = 0; return true; }
            if (k == 1) { setupChordKey_ = wrapRange(setupChordKey_, 1, 0, 11); setupChordKbdWriteIdx_ = 0; return true; }
            if (k >= 2 && k <= 5) {
                setupChordSlot_ = k - 2;
                return true;
            }
            if (k == 6) {
                setupChordKbdIn_ = !setupChordKbdIn_;
                setupChordKbdWriteIdx_ = 0;
                return true;
            }
            (void) pef;
            return true;
        }
        case ClassicScreen::SetupMicroRoot: {
            if (k == 7) {
                processor_.setClassicAuditionMicroTuneMode(0u);
                screen_ = ClassicScreen::SetupRoot;
                return true;
            }
            if (k == 3) {
                setupMicroKey_ = std::clamp(setupMicroKey_, 0, 11);
                setupMicroCursor_ = 0;
                processor_.setClassicAuditionMicroTuneMode(1u);
                screen_ = ClassicScreen::SetupMicroOctEdit;
                return true;
            }
            if (k == 4) {
                setupMicroInitConfirm_ = false;
                setupMicroInitCursor_ = 0;
                screen_ = ClassicScreen::SetupMicroOctInit;
                return true;
            }
            if (k == 5) {
                setupMicroKey_ = std::clamp(setupMicroKey_, 0, 127);
                setupMicroCursor_ = 0;
                processor_.setClassicAuditionMicroTuneMode(2u);
                screen_ = ClassicScreen::SetupMicroFullEdit;
                return true;
            }
            if (k == 6) {
                setupMicroInitConfirm_ = false;
                setupMicroInitCursor_ = 0;
                screen_ = ClassicScreen::SetupMicroFullInit;
                return true;
            }
            return true;
        }
        case ClassicScreen::SetupMicroOctEdit:
        case ClassicScreen::SetupMicroFullEdit: {
            if (k == 7) {
                processor_.setClassicAuditionMicroTuneMode(0u);
                screen_ = ClassicScreen::SetupMicroRoot;
                return true;
            }
            const int maxKey = (screen_ == ClassicScreen::SetupMicroOctEdit) ? 11 : 127;
            if (k == 0) { setupMicroCursor_ = 0; return true; }
            if (k == 1) { setupMicroCursor_ = 1; return true; }
            if (k == 5) { setupMicroKey_ = wrapRange(setupMicroKey_, -1, 0, maxKey); return true; }
            if (k == 6) { setupMicroKey_ = wrapRange(setupMicroKey_, 1, 0, maxKey); return true; }
            return true;
        }
        case ClassicScreen::SetupMicroOctInit:
        case ClassicScreen::SetupMicroFullInit: {
            if (k == 7) {
                setupMicroInitConfirm_ = false;
                screen_ = ClassicScreen::SetupMicroRoot;
                return true;
            }
            if (k == 0) {
                setupMicroInitConfirm_ = true;
                return true;
            }
            if (k >= 3 && k <= 5) {
                setupMicroInitCursor_ = 0;
                return true;
            }
            if (k == 6) {
                setupMicroInitCursor_ = 1;
                return true;
            }
            return true;
        }
        case ClassicScreen::SetupVel: {
            if (k == 7) { screen_ = ClassicScreen::SetupRoot; return true; }
            if (k >= 2 && k <= 4) { setupVelCursor_ = 0; return true; }
            if (k == 5 || k == 6) { setupVelCursor_ = 1; return true; }
            return true;
        }
        case ClassicScreen::SetupDamp: {
            if (k == 7) { screen_ = ClassicScreen::SetupRoot; return true; }
            if (k == 2 || k == 3) { setupDampCursor_ = 0; return true; }
            if (k >= 4 && k <= 6) { setupDampCursor_ = 1; return true; }
            return true;
        }

        // =====================================================================
        // Chapter 1: DEMO & EDIT MESSAGE (pp. 11, 13-15, 22)
        // =====================================================================
        case ClassicScreen::DemoRoot: {
            if (k == 0 || k == 1) demoSubMode_ = 0;
            else if (k == 2 || k == 3) demoSubMode_ = 1;
            else if (k == 4 || k == 5) demoSubMode_ = 2;
            else demoSubMode_ = 3;
            screen_ = ClassicScreen::DemoConfirm;
            return true;
        }
        case ClassicScreen::DemoConfirm: {
            if (k == 6) {
                return handleUtilNo();
            }
            if (k == 7) {
                return handleUtilYes();
            }
            return true;
        }
        case ClassicScreen::DemoPlay: {
            const std::size_t demoIdx = vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase
                                      + static_cast<std::size_t>(std::clamp(demoSelectedSong_, 0, 2));
            if (k == 4 || k == 5) { // >START
                processor_.requestDemoStart(demoIdx);
                return true;
            }
            if (k == 6) { // >STOP
                processor_.requestDemoStop();
                return true;
            }
            if (k == 7) { // >EXIT
                processor_.requestDemoStop();
                processor_.setDemoChainAll(false);
                processor_.selectDemoSong(static_cast<std::size_t>(std::clamp(activeSongSlot_, 0, 7)));
                screen_ = preUtilityScreen_;
                return true;
            }
            return true;
        }
        case ClassicScreen::DemoVoiceEdit: {
            if (k == 7) { // >EXIT
                screen_ = ClassicScreen::PlaySingle;
                return true;
            }
            return true;
        }
        case ClassicScreen::UtilEditMessage: {
            if (k == 6) { // <-
                msgEditCursorCol_ = std::max(0, std::min(39, msgEditCursorCol_) - 1);
                msgMultiTapDigit_ = -1;
                return true;
            }
            if (k == 7) { // ->
                msgEditCursorCol_ = std::min(39, std::min(39, msgEditCursorCol_) + 1);
                msgMultiTapDigit_ = -1;
                return true;
            }
            return true;
        }
        default:
            return false;
    }
}

bool VF50ClassicController::handleUtilStepValue(int deltaSteps) noexcept {
    if (deltaSteps == 0) {
        return true;
    }
    clearNotice();
    auto& card = processor_.mutableVirtualCard();
    auto& sys = processor_.mutableClassicSystemSettings();
    auto& pef = processor_.mutablePerfEffectBank();

    switch (screen_) {
        case ClassicScreen::CardRoot: {
            if (card.bankCount() > 1) {
                card.activeBankIndex = static_cast<uint8_t>((card.activeBankIndex + 1u) & 1u);
            }
            return true;
        }
        case ClassicScreen::MidiChInfo: {
            if (midiChInfoCursor_ == 0) {
                sys.midiOn = !sys.midiOn;
            } else if (midiChInfoCursor_ == 1) {
                sys.basicReceiveCh = static_cast<uint8_t>(wrapRange(sys.basicReceiveCh, deltaSteps, 0, 16));
            } else if (midiChInfoCursor_ == 2) {
                sys.transmitCh = static_cast<uint8_t>(wrapRange(sys.transmitCh, deltaSteps, 0, 15));
            } else {
                sys.localOn = !sys.localOn;
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::MidiSwitch: {
            uint8_t* field = &sys.controlChangeMode;
            if (midiSwitchCursor_ == 1) field = &sys.aftertouchMode;
            else if (midiSwitchCursor_ == 2) field = &sys.pitchBendMode;
            *field = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(*field) + deltaSteps, 0, 17));
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::MidiCond: {
            if (midiCondCursor_ == 0) {
                sys.noteOnOffFilter = static_cast<uint8_t>(wrapRange(sys.noteOnOffFilter, deltaSteps, 0, 2));
            } else {
                sys.dataEntryAssignCc = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(sys.dataEntryAssignCc) + deltaSteps, 0, 31));
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::MidiPgmCng: {
            sys.pgmChangeMode = static_cast<uint8_t>(wrapRange(sys.pgmChangeMode, deltaSteps, 0, 4));
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::MidiPgmEdit: {
            auto& entry = processor_.mutablePgmChangeTable().entries[static_cast<std::size_t>(midiPgmEditSlot_)];
            int num = static_cast<int>(entry.number) + deltaSteps;
            if (num > 99) {
                entry.bank = static_cast<uint8_t>((entry.bank + 1u) % 3u);
                num -= 100;
            } else if (num < 0) {
                entry.bank = static_cast<uint8_t>((entry.bank + 2u) % 3u);
                num += 100;
            }
            entry.number = static_cast<uint8_t>(std::clamp(num, 0, 99));
            if (midiPgmEditPerfHeld_) {
                entry.isPerformance = true;
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::MidiExclusive: {
            sys.deviceNumber = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(sys.deviceNumber) + deltaSteps, 0, 17));
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::MidiBulkSub: {
            if (midiBulkCategory_ <= 1) {
                if (midiBulkCursor_ == 0) {
                    midiBulkBank_ = wrapRange(midiBulkBank_, deltaSteps, 0, 2);
                } else {
                    midiBulkSubIdx_ = wrapRange(midiBulkSubIdx_, deltaSteps, 0, 4);
                }
            } else {
                const int maxIdx = (midiBulkCategory_ == 2) ? 4 : 2;
                midiBulkSubIdx_ = wrapRange(midiBulkSubIdx_, deltaSteps, 0, maxIdx);
            }
            midiBulkConfirm_ = (deltaSteps == 0);
            return true;
        }
        case ClassicScreen::SetupTune: {
            if (setupTuneCursor_ == 0) {
                sys.masterTuningSteps = static_cast<int8_t>(std::clamp<int>(static_cast<int>(sys.masterTuningSteps) + deltaSteps, -64, 64));
            } else {
                sys.synthVolume = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(sys.synthVolume) + deltaSteps, 0, 99));
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::SetupCombine: {
            sys.combineWithFunction = (deltaSteps > 0);
            if (sys.combineWithFunction) {
                combineBorrowedFunction_ = false;
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::SetupCtrlReset: {
            sys.controllerReset = (deltaSteps > 0);
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::SetupDelayEdit: {
            auto& d = pef.delays[static_cast<std::size_t>(std::clamp(setupPerfFxSlot_, 0, 3))];
            if (setupDelayPage_ == 0) {
                if (setupDelayCursor_ == 0) {
                    d.delayTimeCs = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(d.delayTimeCs) + deltaSteps, 1, 128));
                } else {
                    d.pitchShift = static_cast<int8_t>(std::clamp<int>(static_cast<int>(d.pitchShift) + deltaSteps, -24, 24));
                }
            } else {
                if (setupDelayCursor_ == 0) {
                    d.feedback = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(d.feedback) + deltaSteps, 0, 7));
                } else {
                    d.effectLevel = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(d.effectLevel) + deltaSteps, 0, 99));
                }
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::SetupPanEdit: {
            auto& p = pef.pans[static_cast<std::size_t>(std::clamp(setupPerfFxSlot_, 0, 3))];
            if (setupPanCursor_ == 0) {
                p.select = static_cast<uint8_t>(wrapRange(p.select, deltaSteps, 0, 2));
            } else if (setupPanCursor_ == 1) {
                p.direction = static_cast<uint8_t>(wrapRange(p.direction, deltaSteps, 0, 1));
            } else {
                p.panRange = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(p.panRange) + deltaSteps, 0, 99));
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::SetupChordEdit: {
            auto& ch = pef.chords[static_cast<std::size_t>(std::clamp(setupPerfFxSlot_, 0, 3))];
            auto& notes = ch.chords[static_cast<std::size_t>(std::clamp(setupChordKey_, 0, 11))];
            int v = static_cast<int>(notes[static_cast<std::size_t>(std::clamp(setupChordSlot_, 0, 3))]);
            if (v < 0 && deltaSteps > 0) v = 60;
            else v = std::clamp(v + deltaSteps, -1, 127);
            notes[static_cast<std::size_t>(std::clamp(setupChordSlot_, 0, 3))] = static_cast<int8_t>(v);
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::SetupMicroOctEdit: {
            auto tbl = processor_.hostMemory().userOctaveMicrotuning();
            auto& entry = tbl[static_cast<std::size_t>(std::clamp(setupMicroKey_, 0, 11))];
            if (setupMicroCursor_ == 0) {
                entry.coarseNote = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(entry.coarseNote) + deltaSteps, 0, 127));
            } else {
                const int total = std::clamp((static_cast<int>(entry.coarseNote) - 13) * 64 + static_cast<int>(entry.fineStep64) + deltaSteps, -13 * 64, 114 * 64 + 63);
                const int shifted = total + 13 * 64;
                entry.coarseNote = static_cast<uint8_t>(std::clamp(shifted / 64, 0, 127));
                entry.fineStep64 = static_cast<uint8_t>(shifted % 64);
            }
            processor_.hostMemory().setUserOctaveMicrotuning(tbl);
            processor_.pushHostMemoryToAudio();
            return true;
        }
        case ClassicScreen::SetupMicroFullEdit: {
            auto tbl = processor_.hostMemory().userFullMicrotuning();
            auto& entry = tbl[static_cast<std::size_t>(std::clamp(setupMicroKey_, 0, 127))];
            if (setupMicroCursor_ == 0) {
                entry.coarseNote = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(entry.coarseNote) + deltaSteps, 0, 127));
            } else {
                const int total = std::clamp((static_cast<int>(entry.coarseNote) - 13) * 64 + static_cast<int>(entry.fineStep64) + deltaSteps, -13 * 64, 114 * 64 + 63);
                const int shifted = total + 13 * 64;
                entry.coarseNote = static_cast<uint8_t>(std::clamp(shifted / 64, 0, 127));
                entry.fineStep64 = static_cast<uint8_t>(shifted % 64);
            }
            processor_.hostMemory().setUserFullMicrotuning(tbl);
            processor_.pushHostMemoryToAudio();
            return true;
        }
        case ClassicScreen::SetupMicroOctInit:
        case ClassicScreen::SetupMicroFullInit: {
            setupMicroInitConfirm_ = false;
            if (setupMicroInitCursor_ == 0) {
                setupMicroInitTable_ = wrapRange(setupMicroInitTable_, deltaSteps, 0, 10);
            } else {
                setupMicroInitKey_ = wrapRange(setupMicroInitKey_, deltaSteps, 0, 11);
            }
            return true;
        }
        case ClassicScreen::SetupVel: {
            if (setupVelCursor_ == 0) {
                sys.fixedVelocity = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(sys.fixedVelocity) + deltaSteps, 0, 127));
            } else {
                sys.velocityCurve = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(sys.velocityCurve) + deltaSteps, 0, 7));
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::SetupDamp: {
            if (setupDampCursor_ == 0) {
                sys.egDampSpeed = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(sys.egDampSpeed) + deltaSteps, 0, 3));
            } else {
                sys.voiceDampOn = (deltaSteps > 0);
            }
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        case ClassicScreen::DemoPlay: {
            const int count = (demoSubMode_ == 1 && !diskManager_.entries().empty())
                                ? static_cast<int>(diskManager_.entries().size())
                                : 3;
            demoSelectedSong_ = wrapRange(demoSelectedSong_, deltaSteps, 0, std::max(0, count - 1));
            const std::size_t demoIdx = vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase
                                      + static_cast<std::size_t>(std::clamp(demoSelectedSong_, 0, 2));
            processor_.selectDemoSong(demoIdx);
            return true;
        }
        case ClassicScreen::UtilEditMessage: {
            const int col = std::clamp(msgEditCursorCol_, 0, 39);
            int ch = static_cast<unsigned char>(sys.powerOnMessage[static_cast<std::size_t>(col)]);
            if (ch < 32 || ch > 126) ch = 32;
            ch = wrapRange(ch, deltaSteps, 32, 126);
            sys.powerOnMessage[static_cast<std::size_t>(col)] = static_cast<char>(ch);
            msgMultiTapDigit_ = -1;
            processor_.publishClassicSystemStateToAudio();
            return true;
        }
        default:
            return false;
    }
}

bool VF50ClassicController::handleUtilDigit(int digit0To9) noexcept {
    clearNotice();
    if (screen_ == ClassicScreen::CardRoot) {
        const auto& card = processor_.virtualCard();
        if (digit0To9 >= 1 && digit0To9 <= card.bankCount()) {
            processor_.mutableVirtualCard().activeBankIndex = static_cast<uint8_t>(digit0To9 - 1);
        }
        return true;
    }
    if (screen_ == ClassicScreen::MidiPgmEdit) {
        auto& entry = processor_.mutablePgmChangeTable().entries[static_cast<std::size_t>(midiPgmEditSlot_)];
        if (midiPgmEditPendingTens_ < 0) {
            midiPgmEditPendingTens_ = digit0To9;
        } else {
            entry.number = static_cast<uint8_t>(std::clamp(midiPgmEditPendingTens_ * 10 + digit0To9, 0, 99));
            midiPgmEditPendingTens_ = -1;
            processor_.publishClassicSystemStateToAudio();
        }
        return true;
    }
    if (screen_ == ClassicScreen::UtilEditMessage) {
        auto& sys = processor_.mutableClassicSystemSettings();
        const int col = std::clamp(msgEditCursorCol_, 0, 39);
        const std::string_view chars = kMsgMultiTapSets[static_cast<std::size_t>(digit0To9)];
        if (msgMultiTapDigit_ == digit0To9) {
            msgMultiTapCycle_ = (msgMultiTapCycle_ + 1) % static_cast<int>(chars.size());
        } else {
            msgMultiTapDigit_ = digit0To9;
            msgMultiTapCycle_ = 0;
        }
        char c = chars[static_cast<std::size_t>(msgMultiTapCycle_)];
        if (msgLowerCase_ && c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        } else if (!msgLowerCase_ && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        sys.powerOnMessage[static_cast<std::size_t>(col)] = c;
        processor_.publishClassicSystemStateToAudio();
        return true;
    }
    return false;
}

bool VF50ClassicController::handleUtilYes() noexcept {
    clearNotice();
    auto& card = processor_.mutableVirtualCard();
    auto& mem = processor_.hostMemory();
    auto& sys = processor_.mutableClassicSystemSettings();

    // Card Format / Save / Load confirmations
    if (isCardScreen(screen_) && cardConfirmPending_) {
        auto& bank = card.activeBank();
        if (cardPendingAction_ == 1) { // Format
            if (card.hardwareWriteProtect || processor_.isCardMemoryProtected()) {
                setNotice("Memory Protected", 60);
                cardConfirmPending_ = false;
                return true;
            }
            if (card.isReadOnlyMedia()) {
                setNotice("ROM Card!", 60);
                cardConfirmPending_ = false;
                return true;
            }
            // Per manual p. 95 Note: old data is not cleared until overwritten by Save.
            bank.format = (cardPendingSubType_ == 1)
                              ? vf50::state::CardBankFormat::VF50Syn
                              : vf50::state::CardBankFormat::VF50Rhy;
            cardConfirmPending_ = false;
            screen_ = ClassicScreen::CardRoot;
            return true;
        }

        if (cardPendingAction_ == 2) { // Save
            if (card.hardwareWriteProtect || processor_.isCardMemoryProtected()) {
                setNotice("Memory Protected", 60);
                cardConfirmPending_ = false;
                return true;
            }
            if (card.isReadOnlyMedia()) {
                setNotice("ROM Card!", 60);
                cardConfirmPending_ = false;
                return true;
            }
            if (cardSubCategory_ == 0) {
                if (bank.format != vf50::state::CardBankFormat::VF50Syn) {
                    setNotice("Illegal format!", 60);
                    cardConfirmPending_ = false;
                    return true;
                }
                const int t = cardPendingSubType_;
                const bool doVPf   = (t == 0 || t == 1);
                const bool doPct   = (t == 0 || t == 2 || t == 3);
                const bool doPefct = (t == 0 || t == 2 || t == 4);
                const bool doMct   = (t == 0 || t == 2 || t == 5);
                const bool doSys   = (t == 0 || t == 6);
                if (doVPf) {
                    for (std::size_t i = 0u; i < 100u; ++i) {
                        bank.voices[i].voice = mem.getUserVoice(i);
                        bank.performances[i].perf = mem.getUserPerformance(i);
                        bank.performances[i].partPans = mem.getUserPerformancePartPans(i);
                    }
                    bank.hasVoicesPerfs = true;
                }
                if (doPct) {
                    bank.pct = processor_.pgmChangeTable();
                    bank.hasPct = true;
                }
                if (doPefct) {
                    bank.pefct = processor_.perfEffectBank();
                    bank.hasPefct = true;
                }
                if (doMct) {
                    bank.microOct = mem.userOctaveMicrotuning();
                    bank.microFull = mem.userFullMicrotuning();
                    bank.hasMct = true;
                }
                if (doSys) {
                    bank.sys = sys;
                    bank.sysIntProtect = processor_.isIntMemoryProtected();
                    bank.sysCardProtect = processor_.isCardMemoryProtected();
                    bank.hasSys = true;
                }
            } else {
                if (bank.format != vf50::state::CardBankFormat::VF50Rhy) {
                    setNotice("Illegal format!", 60);
                    cardConfirmPending_ = false;
                    return true;
                }
                auto& rSet = bank.rhythmSets[static_cast<std::size_t>(cardSubCategory_ == 2 ? 1 : 0)];
                const int t = cardPendingSubType_;
                const bool doSeq = (t == 0 || t == 1);
                const bool doSet = (t == 0 || t == 2);
                if (doSeq) {
                    for (std::size_t i = 0u; i < 100u; ++i) {
                        rSet.patterns[i] = mem.userRhythmPattern(i);
                    }
                    rSet.songs = *rhythmSongs_;
                    rSet.hasRSeq = true;
                }
                if (doSet) {
                    rSet.kit = mem.activeRhythmKit();
                    rSet.userKeyMaps[0] = mem.userKeyMap(0);
                    rSet.userKeyMaps[1] = mem.userKeyMap(1);
                    rSet.rhythmRch = static_cast<uint8_t>(rhyRch_);
                    rSet.rhythmTch = static_cast<uint8_t>(rhyTch_);
                    rSet.rhythmVelOn = rhyVelOn_;
                    rSet.rhythmSyncMidi = (processor_.isDawSyncEnabled());
                    rSet.rhythmTempo = static_cast<uint16_t>(rhyTempoBpm_);
                    rSet.hasSetup = true;
                }
                if (doSeq && doSet) {
                    rSet.hasAll = true;
                }
            }
            cardConfirmPending_ = false;
            screen_ = ClassicScreen::CardRoot;
            return true;
        }

        if (cardPendingAction_ == 3) { // Load
            const bool isRSeqException = (cardSubCategory_ >= 1 && cardSubCategory_ <= 2 && cardPendingSubType_ == 1);
            if (processor_.isIntMemoryProtected() && !isRSeqException) {
                setNotice("Memory Protected", 60);
                cardConfirmPending_ = false;
                return true;
            }
            if (cardSubCategory_ == 0) {
                if (bank.format != vf50::state::CardBankFormat::VF50Syn && !bank.hasVoicesPerfs && !bank.hasSys) {
                    setNotice("Illegal format!", 60);
                    cardConfirmPending_ = false;
                    return true;
                }
                const int t = cardPendingSubType_;
                const bool doVPf   = (t == 0 || t == 1);
                const bool doPct   = (t == 0 || t == 2 || t == 3);
                const bool doPefct = (t == 0 || t == 2 || t == 4);
                const bool doMct   = (t == 0 || t == 2 || t == 5);
                const bool doSys   = (t == 0 || t == 6);
                if (doVPf) {
                    for (std::size_t i = 0u; i < 100u; ++i) {
                        mem.storeUserVoice(i, bank.voices[i].voice);
                        mem.storeUserPerformance(i, bank.performances[i].perf, bank.performances[i].partPans);
                    }
                    mem.selectSingleVoice(mem.selectedVoiceBank(), mem.selectedVoiceIndex());
                    mem.selectPerformance(mem.selectedPerfBank(), mem.selectedPerfIndex());
                    processor_.pushHostMemoryToAudio();
                }
                if (doPct) {
                    processor_.mutablePgmChangeTable() = bank.pct;
                }
                if (doPefct) {
                    processor_.mutablePerfEffectBank() = bank.pefct;
                }
                if (doMct) {
                    mem.setUserOctaveMicrotuning(bank.microOct);
                    mem.setUserFullMicrotuning(bank.microFull);
                    processor_.pushHostMemoryToAudio();
                }
                if (doSys && bank.hasSys) {
                    sys = bank.sys;
                    processor_.setIntMemoryProtected(bank.sysIntProtect);
                    processor_.setCardMemoryProtected(bank.sysCardProtect);
                }
                processor_.publishClassicSystemStateToAudio();
            } else if (cardSubCategory_ == 1 || cardSubCategory_ == 2) {
                const auto& rSet = bank.rhythmSets[static_cast<std::size_t>(cardSubCategory_ == 2 ? 1 : 0)];
                const int t = cardPendingSubType_;
                const bool doSeq = (t == 0 || t == 1);
                const bool doSet = (t == 0 || t == 2);
                if (doSeq) {
                    for (std::size_t i = 0u; i < 100u; ++i) {
                        mem.mutableUserRhythmPattern(i) = rSet.patterns[i];
                    }
                    *rhythmSongs_ = rSet.songs;
                    processor_.publishRhythmSongsToAudio();
                }
                if (doSet) {
                    mem.mutableActiveRhythmKit() = rSet.kit;
                    mem.setUserKeyMap(0, rSet.userKeyMaps[0]);
                    mem.setUserKeyMap(1, rSet.userKeyMaps[1]);
                    rhyRch_ = rSet.rhythmRch;
                    rhyTch_ = rSet.rhythmTch;
                    rhyVelOn_ = rSet.rhythmVelOn;
                    rhyTempoBpm_ = std::clamp<int>(rSet.rhythmTempo, 40, 250);
                    commitRhythmKitAndSync();
                    pushActiveRhythmKeyMapToAudio();
                }
                processor_.pushHostMemoryToAudio();
            } else if (cardSubCategory_ == 3) { // SEQ(YS)
                const std::size_t dstSlot = static_cast<std::size_t>(std::clamp(activeSongSlot_, 0, 7));
                if (bank.hasYsSeq) {
                    processor_.publishUserSequenceToSlot(dstSlot, bank.ysSeq);
                } else {
                    processor_.clearUserSequenceSlot(dstSlot);
                }
                syncSongSlotTitlesFromProcessor();
            }
            cardConfirmPending_ = false;
            screen_ = ClassicScreen::CardRoot;
            return true;
        }
    }

    if (screen_ == ClassicScreen::MidiPgmInit) {
        if (!midiPgmInitConfirm_) {
            midiPgmInitConfirm_ = true;
            return true;
        }
        processor_.mutablePgmChangeTable().resetToFactory();
        processor_.publishClassicSystemStateToAudio();
        midiPgmInitConfirm_ = false;
        screen_ = ClassicScreen::MidiPgmCng;
        setNotice("Completed !", 60);
        return true;
    }

    if (screen_ == ClassicScreen::MidiBulkSub) {
        if (!midiBulkConfirm_) {
            midiBulkConfirm_ = true;
            return true;
        }
        if (sys.deviceNumber == 0u) {
            setNotice("Device No. off!", 60);
            midiBulkConfirm_ = false;
            return true;
        }
        vf50::midi::VF50SysExHandler sysex;
        sysex.setDeviceChannel(sysExDeviceIdFromSetting(sys.deviceNumber));
        sysex.setDeviceEnabled(true);
        vf50::midi::SysExReplyBuffer reply{};

        bool ok = false;
        if (midiBulkCategory_ == 0 || midiBulkCategory_ == 1) { // VOICE or PFM
            static constexpr std::array<std::size_t, 5> kStarts{ { 0u, 25u, 50u, 75u, 0u } };
            static constexpr std::array<std::size_t, 5> kCounts{ { 25u, 25u, 25u, 25u, 100u } };
            const std::size_t st = kStarts[static_cast<std::size_t>(std::clamp(midiBulkSubIdx_, 0, 4))];
            const std::size_t cnt = kCounts[static_cast<std::size_t>(std::clamp(midiBulkSubIdx_, 0, 4))];
            if (midiBulkCategory_ == 0) {
                std::array<vf50::state::StoredVoiceSlot, 100> src{};
                for (std::size_t i = 0u; i < 100u; ++i) {
                    if (midiBulkBank_ == 0) src[i].voice = mem.getUserVoice(i);
                    else if (midiBulkBank_ == 1) src[i].voice = mem.getFactoryVoice(i);
                    else src[i] = card.activeBank().voices[i];
                }
                ok = sysex.emitVoiceRangeBulkDump(src, st, cnt, reply) && processor_.enqueueBulkSysEx(reply);
            } else {
                std::array<vf50::state::StoredPerformanceSlot, 100> src{};
                for (std::size_t i = 0u; i < 100u; ++i) {
                    if (midiBulkBank_ == 0) src[i].perf = mem.getUserPerformance(i);
                    else if (midiBulkBank_ == 1) src[i].perf = mem.getFactoryPerformance(i);
                    else src[i] = card.activeBank().performances[i];
                }
                ok = sysex.emitPerfRangeBulkDump(src, st, cnt, reply) && processor_.enqueueBulkSysEx(reply);
            }
        } else if (midiBulkCategory_ == 2) { // SETUP: 0=SetALL(PCT+P.EFCT+MCT, no SYS), 1=PCT, 2=P.EFCT, 3=MCT, 4=SYS
            ok = true;
            if (midiBulkSubIdx_ == 0 || midiBulkSubIdx_ == 1) {
                ok &= sysex.emitProgramChangeTableDump(processor_.pgmChangeTable(), reply) && processor_.enqueueBulkSysEx(reply);
            }
            if (midiBulkSubIdx_ == 0 || midiBulkSubIdx_ == 2) {
                ok &= sysex.emitPerfEffectBankDump(processor_.perfEffectBank(), reply) && processor_.enqueueBulkSysEx(reply);
            }
            if (midiBulkSubIdx_ == 0 || midiBulkSubIdx_ == 3) {
                ok &= sysex.emitMicrotuningOctaveDump(mem.userOctaveMicrotuning(), reply) && processor_.enqueueBulkSysEx(reply);
                ok &= sysex.emitMicrotuningFullDump(mem.userFullMicrotuning(), reply) && processor_.enqueueBulkSysEx(reply);
            }
            if (midiBulkSubIdx_ == 4) {
                ok &= sysex.emitSystemDataDump(sys, processor_.isIntMemoryProtected(), processor_.isCardMemoryProtected(), reply)
                      && processor_.enqueueBulkSysEx(reply);
            }
        } else if (midiBulkCategory_ == 3) { // SEQ
            ok = sysex.emitSequenceBulkDump(processor_.hostCustomSequence(), reply) && processor_.enqueueBulkSysEx(reply);
        } else { // RHY
            ok = true;
            if (midiBulkSubIdx_ == 0 || midiBulkSubIdx_ == 2) {
                ok &= sysex.emitRhythmSystemDump(mem, processor_.synthEngine(), reply) && processor_.enqueueBulkSysEx(reply);
            }
            if (midiBulkSubIdx_ == 0 || midiBulkSubIdx_ == 1) {
                ok &= sysex.emitRhythmSeqBulkDump(mem, reply) && processor_.enqueueBulkSysEx(reply);
            }
        }
        midiBulkConfirm_ = false;
        setNotice(ok ? "transmitting !" : "Queue full!", 30);
        return true;
    }

    if (screen_ == ClassicScreen::SetupMicroOctInit || screen_ == ClassicScreen::SetupMicroFullInit) {
        if (!setupMicroInitConfirm_) {
            setupMicroInitConfirm_ = true;
            return true;
        }
        const bool isFull = (screen_ == ClassicScreen::SetupMicroFullInit);
        copyFactoryMicroTableToUser(mem, setupMicroInitTable_, setupMicroInitKey_, isFull, mem);
        processor_.pushHostMemoryToAudio();
        setupMicroInitConfirm_ = false;
        screen_ = ClassicScreen::SetupMicroRoot;
        setNotice("Completed !", 60);
        return true;
    }

    if (screen_ == ClassicScreen::DemoConfirm) {
        demoBusyTicks_ = 12;
        if (demoSubMode_ == 3) { // >V. Edit
            demoVEditStep_ = 0;
            demoVEditAccum_ = 0;
            screen_ = ClassicScreen::DemoVoiceEdit;
            return true;
        }
        processor_.setDemoChainAll(demoSubMode_ == 2);
        demoSelectedSong_ = 0;
        const std::size_t demo0 = vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase;
        processor_.selectDemoSong(demo0);
        if (demoSubMode_ == 2) {
            processor_.requestDemoStart(demo0);
        }
        screen_ = ClassicScreen::DemoPlay;
        return true;
    }

    return handleUtilStepValue(1);
}

bool VF50ClassicController::handleUtilNo() noexcept {
    clearNotice();
    if (isCardScreen(screen_) && cardConfirmPending_) {
        cardConfirmPending_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::MidiPgmInit && midiPgmInitConfirm_) {
        midiPgmInitConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::MidiBulkSub && midiBulkConfirm_) {
        midiBulkConfirm_ = false;
        return true;
    }
    if ((screen_ == ClassicScreen::SetupMicroOctInit || screen_ == ClassicScreen::SetupMicroFullInit) &&
        setupMicroInitConfirm_) {
        setupMicroInitConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::DemoConfirm) {
        screen_ = ClassicScreen::DemoRoot;
        return true;
    }
    return handleUtilStepValue(-1);
}

bool VF50ClassicController::handleUtilTextChar(char asciiChar) noexcept {
    if (screen_ == ClassicScreen::UtilEditMessage) {
        if (asciiChar >= 32 && asciiChar <= 126) {
            auto& sys = processor_.mutableClassicSystemSettings();
            const int col = std::clamp(msgEditCursorCol_, 0, 39);
            sys.powerOnMessage[static_cast<std::size_t>(col)] = asciiChar;
            msgEditCursorCol_ = std::min(40, col + 1);
            msgMultiTapDigit_ = -1;
            processor_.publishClassicSystemStateToAudio();
        }
        return true;
    }
    return false;
}

void VF50ClassicController::handleUtilDataEntry(float norm0To1) noexcept {
    const float n = std::clamp(norm0To1, 0.0f, 1.0f);
    auto& sys = processor_.mutableClassicSystemSettings();
    auto& pef = processor_.mutablePerfEffectBank();

    switch (screen_) {
        case ClassicScreen::CardRoot: {
            const auto& card = processor_.virtualCard();
            if (card.bankCount() > 1) {
                processor_.mutableVirtualCard().activeBankIndex = (n >= 0.5f) ? 1u : 0u;
            }
            break;
        }
        case ClassicScreen::MidiChInfo: {
            if (midiChInfoCursor_ == 0) sys.midiOn = (n >= 0.5f);
            else if (midiChInfoCursor_ == 1) sys.basicReceiveCh = static_cast<uint8_t>(std::lround(n * 16.0f));
            else if (midiChInfoCursor_ == 2) sys.transmitCh = static_cast<uint8_t>(std::lround(n * 15.0f));
            else sys.localOn = (n >= 0.5f);
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::MidiSwitch: {
            const uint8_t v = static_cast<uint8_t>(std::lround(n * 17.0f));
            if (midiSwitchCursor_ == 0) sys.controlChangeMode = v;
            else if (midiSwitchCursor_ == 1) sys.aftertouchMode = v;
            else sys.pitchBendMode = v;
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::MidiCond: {
            if (midiCondCursor_ == 0) sys.noteOnOffFilter = static_cast<uint8_t>(std::lround(n * 2.0f));
            else sys.dataEntryAssignCc = static_cast<uint8_t>(std::lround(n * 31.0f));
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::MidiPgmCng: {
            sys.pgmChangeMode = static_cast<uint8_t>(std::lround(n * 4.0f));
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::MidiPgmEdit: {
            auto& entry = processor_.mutablePgmChangeTable().entries[static_cast<std::size_t>(midiPgmEditSlot_)];
            entry.number = static_cast<uint8_t>(std::lround(n * 99.0f));
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::MidiExclusive: {
            sys.deviceNumber = static_cast<uint8_t>(std::lround(n * 17.0f));
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::SetupTune: {
            if (setupTuneCursor_ == 0) {
                sys.masterTuningSteps = static_cast<int8_t>(std::lround(n * 128.0f) - 64);
            } else {
                sys.synthVolume = static_cast<uint8_t>(std::lround(n * 99.0f));
            }
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::SetupDelayEdit: {
            auto& d = pef.delays[static_cast<std::size_t>(std::clamp(setupPerfFxSlot_, 0, 3))];
            if (setupDelayPage_ == 0) {
                if (setupDelayCursor_ == 0) d.delayTimeCs = static_cast<uint8_t>(1 + std::lround(n * 127.0f));
                else d.pitchShift = static_cast<int8_t>(std::lround(n * 48.0f) - 24);
            } else {
                if (setupDelayCursor_ == 0) d.feedback = static_cast<uint8_t>(std::lround(n * 7.0f));
                else d.effectLevel = static_cast<uint8_t>(std::lround(n * 99.0f));
            }
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::SetupPanEdit: {
            auto& p = pef.pans[static_cast<std::size_t>(std::clamp(setupPerfFxSlot_, 0, 3))];
            if (setupPanCursor_ == 0) p.select = static_cast<uint8_t>(std::lround(n * 2.0f));
            else if (setupPanCursor_ == 1) p.direction = (n >= 0.5f) ? 1u : 0u;
            else p.panRange = static_cast<uint8_t>(std::lround(n * 99.0f));
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::SetupVel: {
            if (setupVelCursor_ == 0) sys.fixedVelocity = static_cast<uint8_t>(std::lround(n * 127.0f));
            else sys.velocityCurve = static_cast<uint8_t>(std::lround(n * 7.0f));
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::SetupDamp: {
            if (setupDampCursor_ == 0) sys.egDampSpeed = static_cast<uint8_t>(std::lround(n * 3.0f));
            else sys.voiceDampOn = (n >= 0.5f);
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        case ClassicScreen::DemoVoiceEdit: {
            demoVEditSpeed_ = n;
            break;
        }
        case ClassicScreen::UtilEditMessage: {
            const int col = std::clamp(msgEditCursorCol_, 0, 39);
            const char ch = static_cast<char>(32 + std::lround(n * 94.0f));
            sys.powerOnMessage[static_cast<std::size_t>(col)] = ch;
            msgMultiTapDigit_ = -1;
            processor_.publishClassicSystemStateToAudio();
            break;
        }
        default:
            break;
    }
}

bool VF50ClassicController::handleUtilMemBank(int bIdx) noexcept {
    if (screen_ == ClassicScreen::MidiPgmEdit) {
        auto& entry = processor_.mutablePgmChangeTable().entries[static_cast<std::size_t>(midiPgmEditSlot_)];
        entry.bank = static_cast<uint8_t>(std::clamp(bIdx, 0, 2));
        entry.isPerformance = midiPgmEditPerfHeld_;
        midiPgmEditPendingTens_ = -1;
        processor_.publishClassicSystemStateToAudio();
        return true;
    }
    if (screen_ == ClassicScreen::MidiBulkSub && midiBulkCategory_ <= 1) {
        // 0=INT, 1=CARD -> 2, 2=PRESET -> 1
        midiBulkBank_ = (bIdx == 0) ? 0 : (bIdx == 2 ? 1 : 2);
        return true;
    }
    return false;
}

bool VF50ClassicController::handleUtilSeqOp(int btn) noexcept {
    if (screen_ != ClassicScreen::DemoPlay) {
        return false;
    }
    if (btn == 2) { // STOP
        processor_.requestDemoStop();
        return true;
    }
    if (btn == 3) { // PLAY
        const std::size_t demoIdx = vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase
                                  + static_cast<std::size_t>(std::clamp(demoSelectedSong_, 0, 2));
        processor_.requestDemoStart(demoIdx);
        return true;
    }
    if (btn == 1) { // BWD
        processor_.requestDemoSeekMeasures(-1);
        return true;
    }
    if (btn == 4) { // FWD
        processor_.requestDemoSeekMeasures(1);
        return true;
    }
    return true;
}

bool VF50ClassicController::handleUtilStopAndBwd() noexcept {
    if (screen_ == ClassicScreen::DemoPlay) {
        processor_.requestDemoRewindToTop();
        return true;
    }
    return false;
}

bool VF50ClassicController::renderUtilLcdFrame(ClassicLcdFrame& frame) const noexcept {
    if (!isPhase3UtilScreen(screen_)) {
        return false;
    }
    const auto& card = processor_.virtualCard();
    const auto& bank = card.activeBank();
    const auto& sys  = processor_.classicSystemSettings();
    const auto& pef  = processor_.perfEffectBank();
    const auto& mem  = processor_.hostMemory();
    char tmp[96]{};

    switch (screen_) {
        // =====================================================================
        // Card Operations (pp. 95-98)
        // =====================================================================
        case ClassicScreen::CardRoot: {
            const char* fmtStr = "unfmtd";
            if (bank.format == vf50::state::CardBankFormat::VF50Syn) fmtStr = "VF50SYN";
            else if (bank.format == vf50::state::CardBankFormat::VF50Rhy) fmtStr = "VF50RHY";
            std::snprintf(tmp, sizeof(tmp), "UT  CARD) %cBANK Format                  ", kCursorRight);
            setLineUtil(frame.line0, tmp);
            const int bankDisp = (card.mediaType == vf50::state::CardMediaType::Mcd64Ram)
                                     ? (static_cast<int>(card.activeBankIndex & 1u) + 1)
                                     : 1;
            std::snprintf(tmp, sizeof(tmp), "             %d  %-6s   >Form>Save>Load", bankDisp, fmtStr);
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::CardFormat: {
            if (cardConfirmPending_) {
                const char* tgt = (cardPendingSubType_ == 1) ? "VF50SYN" : "VF50RHY";
                std::snprintf(tmp, sizeof(tmp), "UT  CARD)  FORMAT %-6s Are you sure?  ", tgt);
                setLineUtil(frame.line0, tmp);
                setLineUtil(frame.line1, "Push YES to Format / NO to Cancel  >EXIT");
            } else {
                setLineUtil(frame.line0, "UT  CARD)  FORMAT   Select one !        ");
                setLineUtil(frame.line1, "              VF50SYN   VF50RHY    >EXIT");
            }
            return true;
        }
        case ClassicScreen::CardSaveCategory: {
            setLineUtil(frame.line0, "UT  CARD)  SAVE   Select one !          ");
            setLineUtil(frame.line1, "Synth       Rhythm(toA) Rhythm(toB)>EXIT");
            return true;
        }
        case ClassicScreen::CardSaveSub: {
            if (cardConfirmPending_) {
                setLineUtil(frame.line0, "UT  CARD)  SAVE         Are you sure?   ");
                setLineUtil(frame.line1, "Push YES to Save / NO to Cancel    >EXIT");
                return true;
            }
            if (cardSubCategory_ == 0) {
                setLineUtil(frame.line0, "UT  CARD)  SAVE Synth    select one !   ");
                setLineUtil(frame.line1, "SynAL V&PF SetAL PCT PEFCT MCT SYS >EXIT");
            } else {
                const char* tgt = (cardSubCategory_ == 2) ? "toB" : "toA";
                std::snprintf(tmp, sizeof(tmp), "UT  CARD)  SAVE Rhythm(%s) Select one !", tgt);
                setLineUtil(frame.line0, tmp);
                setLineUtil(frame.line1, "      RhyALL      R.SEQ      SETUP >EXIT");
            }
            return true;
        }
        case ClassicScreen::CardLoadCategory: {
            if (cardConfirmPending_) {
                setLineUtil(frame.line0, "UT  CARD)  LOAD SEQ(YS) Are you sure?   ");
                setLineUtil(frame.line1, "Push YES to Load / NO to Cancel    >EXIT");
                return true;
            }
            setLineUtil(frame.line0, "UT  CARD)  LOAD    Select one !         ");
            setLineUtil(frame.line1, "Synth   Rhythm(A) Rhythm(B) SEQ(YS)>EXIT");
            return true;
        }
        case ClassicScreen::CardLoadSub: {
            if (cardConfirmPending_) {
                setLineUtil(frame.line0, "UT  CARD)  LOAD         Are you sure?   ");
                setLineUtil(frame.line1, "Push YES to Load / NO to Cancel    >EXIT");
                return true;
            }
            if (cardSubCategory_ == 0) {
                setLineUtil(frame.line0, "UT  CARD)  LOAD Synth    select one !   ");
                setLineUtil(frame.line1, "SynAL V&PF SetAL PCT PEFCT MCT SYS >EXIT");
            } else {
                const char ab = (cardSubCategory_ == 2) ? 'B' : 'A';
                std::snprintf(tmp, sizeof(tmp), "UT  CARD)  LOAD Rhythm(%c) select one !  ", ab);
                setLineUtil(frame.line0, tmp);
                setLineUtil(frame.line1, "      RhyALL     R.SEQ    SETUP    >EXIT");
            }
            return true;
        }

        // =====================================================================
        // MIDI Functions (pp. 99-104)
        // =====================================================================
        case ClassicScreen::MidiRoot: {
            setLineUtil(frame.line0, "UT  MIDI)          Select one !         ");
            setLineUtil(frame.line1, ">ChInfo>Switch >Cond>PgmCng   >Exclusive");
            return true;
        }
        case ClassicScreen::MidiChInfo: {
            const char c0 = (midiChInfoCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (midiChInfoCursor_ == 1) ? kCursorRight : '>';
            const char c2 = (midiChInfoCursor_ == 2) ? kCursorRight : '>';
            const char c3 = (midiChInfoCursor_ == 3) ? kCursorRight : '>';
            std::snprintf(tmp, sizeof(tmp), "UT  MIDI) %cMidi%cBasicRch %cKbdTch%cLocaL  ", c0, c1, c2, c3);
            setLineUtil(frame.line0, tmp);

            char rch[8]{};
            if (sys.basicReceiveCh >= 16u) std::snprintf(rch, sizeof(rch), "omn");
            else std::snprintf(rch, sizeof(rch), "%2d ", static_cast<int>(sys.basicReceiveCh) + 1);

            std::snprintf(tmp, sizeof(tmp), "            %-3s  %-3s      %2d   %-3s >EXIT",
                          sys.midiOn ? "on " : "off",
                          rch,
                          static_cast<int>(sys.transmitCh) + 1,
                          sys.localOn ? "on " : "off");
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::MidiSwitch: {
            const char c0 = (midiSwitchCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (midiSwitchCursor_ == 1) ? kCursorRight : '>';
            const char c2 = (midiSwitchCursor_ == 2) ? kCursorRight : '>';
            std::snprintf(tmp, sizeof(tmp), "UT  MIDI)%cCont.Change%cA.Touch %cPitchBend", c0, c1, c2);
            setLineUtil(frame.line0, tmp);

            char cc[5]{}, at[5]{}, pb[5]{};
            formatGlobalOrNormMode(sys.controlChangeMode, cc);
            formatGlobalOrNormMode(sys.aftertouchMode, at);
            formatGlobalOrNormMode(sys.pitchBendMode, pb);
            std::snprintf(tmp, sizeof(tmp), "           %-4s        %-4s    %-4s>EXIT", cc, at, pb);
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::MidiCond: {
            const char c0 = (midiCondCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (midiCondCursor_ == 1) ? kCursorRight : '>';
            std::snprintf(tmp, sizeof(tmp), "UT  MIDI) %cNote on/off   %cD.EntryAssign ", c0, c1);
            setLineUtil(frame.line0, tmp);

            const char* noteMode = (sys.noteOnOffFilter == 1u) ? "odd " : (sys.noteOnOffFilter == 2u ? "even" : "all ");
            const std::string_view ccName = dataEntryCcLabel(sys.dataEntryAssignCc);
            std::snprintf(tmp, sizeof(tmp), "            %-4s         %2d(%.*s)>EXIT",
                          noteMode,
                          static_cast<int>(sys.dataEntryAssignCc),
                          static_cast<int>(ccName.size()),
                          ccName.data());
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::MidiPgmCng: {
            std::snprintf(tmp, sizeof(tmp), "UT  MIDI) %cPgm Change                   ", kCursorRight);
            setLineUtil(frame.line0, tmp);
            const std::string_view m = pgmCngModeLabel(sys.pgmChangeMode);
            std::snprintf(tmp, sizeof(tmp), "           %.*s    >Init>Edit>EXIT",
                          static_cast<int>(m.size()), m.data());
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::MidiPgmInit: {
            if (midiPgmInitConfirm_) {
                setLineUtil(frame.line0, "UT  MIDI) Init Pgm Table Are you sure?  ");
                setLineUtil(frame.line1, "Push YES to Init / NO to Cancel    >EXIT");
            } else {
                setLineUtil(frame.line0, "UT  MIDI)  Initialize pgm change table ?");
                setLineUtil(frame.line1, "                                   >EXIT");
            }
            return true;
        }
        case ClassicScreen::MidiPgmEdit: {
            setLineUtil(frame.line0, "UT  MIDI)  Edit pgm change table        ");
            const auto& entry = processor_.pgmChangeTable().entries[static_cast<std::size_t>(midiPgmEditSlot_)];
            const char kind = entry.isPerformance ? 'P' : ' ';
            const char bChar = (entry.bank == 0u) ? 'I' : ((entry.bank == 1u) ? 'C' : 'P');
            char nameBuf[11]{ ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','\0' };
            if (entry.isPerformance) {
                std::string nm;
                if (entry.bank == 0u) nm = std::string(mem.getUserPerformance(entry.number).name);
                else if (entry.bank == 1u) nm = std::string(card.activeBank().performances[entry.number].perf.name);
                else nm = std::string(mem.getFactoryPerformance(entry.number).name);
                for (std::size_t i = 0u; i < std::min<std::size_t>(nm.size(), 10u); ++i) nameBuf[i] = nm[i];
            } else {
                std::string nm;
                if (entry.bank == 0u) nm = std::string(mem.getUserVoice(entry.number).name);
                else if (entry.bank == 1u) nm = std::string(card.activeBank().voices[entry.number].voice.name);
                else nm = std::string(mem.getFactoryVoice(entry.number).name);
                for (std::size_t i = 0u; i < std::min<std::size_t>(nm.size(), 10u); ++i) nameBuf[i] = nm[i];
            }
            std::snprintf(tmp, sizeof(tmp), " %c pgm %c PGM %3d=%c%c%02d(%-10s)  >EXIT",
                          kArrowDown, kArrowUp, midiPgmEditSlot_ + 1, kind, bChar,
                          static_cast<int>(entry.number), nameBuf);
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::MidiExclusive: {
            std::snprintf(tmp, sizeof(tmp), "UT  MIDI)      %cDevice No.              ", kCursorRight);
            setLineUtil(frame.line0, tmp);
            char dev[4]{};
            if (sys.deviceNumber == 0u) std::snprintf(dev, sizeof(dev), "off");
            else if (sys.deviceNumber >= 17u) std::snprintf(dev, sizeof(dev), "all");
            else std::snprintf(dev, sizeof(dev), "%-2d ", static_cast<int>(sys.deviceNumber));
            std::snprintf(tmp, sizeof(tmp), "                    %-3s       >Bulk>EXIT", dev);
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::MidiBulkRoot: {
            setLineUtil(frame.line0, "UT  DUMP) Bulk Dump      Select one !   ");
            setLineUtil(frame.line1, "     VOICE PFM SETUP      SEQ RHY  >EXIT");
            return true;
        }
        case ClassicScreen::MidiBulkSub: {
            if (midiBulkConfirm_) {
                setLineUtil(frame.line0, "UT  DUMP) Transmit ready?               ");
                setLineUtil(frame.line1, "Push YES to Transmit / NO to Cancel>EXIT");
                return true;
            }
            if (midiBulkCategory_ == 0 || midiBulkCategory_ == 1) {
                static constexpr std::array<const char*, 3> kBanks{ { "INT   ", "PRESET", "CARD  " } };
                static constexpr std::array<const char*, 5> kRanges{ { "00-24", "25-49", "50-74", "75-99", "ALL  " } };
                const char* cat = (midiBulkCategory_ == 0) ? "VOICE" : "PFM  ";
                std::snprintf(tmp, sizeof(tmp), "UT  DUMP) %-5s %cBank   %cRange         ",
                              cat, (midiBulkCursor_ == 0 ? kCursorRight : '>'), (midiBulkCursor_ == 1 ? kCursorRight : '>'));
                setLineUtil(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "          %-6s %-5s  >Transmit  >EXIT",
                              kBanks[static_cast<std::size_t>(std::clamp(midiBulkBank_, 0, 2))],
                              kRanges[static_cast<std::size_t>(std::clamp(midiBulkSubIdx_, 0, 4))]);
                setLineUtil(frame.line1, tmp);
            } else if (midiBulkCategory_ == 2) {
                setLineUtil(frame.line0, "UT  DUMP) SETUP          Select one !   ");
                setLineUtil(frame.line1, "SetALL PCT P.EFCT MCT SYS          >EXIT");
            } else if (midiBulkCategory_ == 3) {
                setLineUtil(frame.line0, "UT  DUMP) SEQ            Select one !   ");
                setLineUtil(frame.line1, "SeqALL  SEQ   SETUP                >EXIT");
            } else {
                setLineUtil(frame.line0, "UT  DUMP) RHYTHM         Select one !   ");
                setLineUtil(frame.line1, "RhyALL  R.SEQ SETUP                >EXIT");
            }
            return true;
        }

        // =====================================================================
        // Utility Setup (pp. 110-116)
        // =====================================================================
        case ClassicScreen::SetupRoot: {
            setLineUtil(frame.line0, "UT  SETUP)       Select one !           ");
            setLineUtil(frame.line1, ">Tune>Comb>Ctrl>P.Efct  >Micro>Vel >Damp");
            return true;
        }
        case ClassicScreen::SetupTune: {
            const char c0 = (setupTuneCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (setupTuneCursor_ == 1) ? kCursorRight : '>';
            std::snprintf(tmp, sizeof(tmp), "UT  SETUP)%cMaster Tuning %cSynth Vol     ", c0, c1);
            setLineUtil(frame.line0, tmp);
            const double hz = 440.0 * std::pow(2.0, (static_cast<double>(sys.masterTuningSteps) * 1.5625) / 1200.0);
            std::snprintf(tmp, sizeof(tmp), "          %+3d (%5.1fHz)    %2d      >EXIT",
                          static_cast<int>(sys.masterTuningSteps), hz, static_cast<int>(sys.synthVolume));
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupCombine: {
            std::snprintf(tmp, sizeof(tmp), "UT  SETUP)  %cCombine(with FUNCTION)     ", kCursorRight);
            setLineUtil(frame.line0, tmp);
            std::snprintf(tmp, sizeof(tmp), "                  %-3s              >EXIT",
                          sys.combineWithFunction ? "on " : "off");
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupCtrlReset: {
            std::snprintf(tmp, sizeof(tmp), "UT  SETUP) %cController Reset            ", kCursorRight);
            setLineUtil(frame.line0, tmp);
            std::snprintf(tmp, sizeof(tmp), "                %-5s              >EXIT",
                          sys.controllerReset ? "reset" : "hold ");
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupPerfEfctCat: {
            setLineUtil(frame.line0, "UT  SETUP)  EDIT PFM EFCT  Select one ! ");
            setLineUtil(frame.line1, "    delay       pan        chord   >EXIT");
            return true;
        }
        case ClassicScreen::SetupPerfEfctSlot: {
            if (setupPerfFxCat_ == 0) {
                std::snprintf(tmp, sizeof(tmp), "UT  SETUP)   %cEDIT DELAY  Select one !  ", kCursorRight);
                setLineUtil(frame.line0, tmp);
                setLineUtil(frame.line1, "delay1     delay2   delay3   delay4>EXIT");
            } else if (setupPerfFxCat_ == 1) {
                std::snprintf(tmp, sizeof(tmp), "UT  SETUP)    %cEDIT PAN   Select one !  ", kCursorRight);
                setLineUtil(frame.line0, tmp);
                setLineUtil(frame.line1, "pan1        pan2      pan3     pan4>EXIT");
            } else {
                std::snprintf(tmp, sizeof(tmp), "UT  SETUP)   %cEDIT CHORD  Select one !  ", kCursorRight);
                setLineUtil(frame.line0, tmp);
                setLineUtil(frame.line1, "chord1     chord2   chord3   chord4>EXIT");
            }
            return true;
        }
        case ClassicScreen::SetupDelayEdit: {
            const int slot = std::clamp(setupPerfFxSlot_, 0, 3);
            const auto& d = pef.delays[static_cast<std::size_t>(slot)];
            const char c0 = (setupDelayCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (setupDelayCursor_ == 1) ? kCursorRight : '>';
            if (setupDelayPage_ == 0) {
                std::snprintf(tmp, sizeof(tmp), "UT  DELAY%d) %cDelayTime %cPitchShift      ", slot + 1, c0, c1);
                setLineUtil(frame.line0, tmp);
                const int sec = static_cast<int>(d.delayTimeCs) / 100;
                const int cs  = static_cast<int>(d.delayTimeCs) % 100;
                std::snprintf(tmp, sizeof(tmp), "             %1d.%02dsec      %+3d >NEXT>EXIT",
                              sec, cs, static_cast<int>(d.pitchShift));
                setLineUtil(frame.line1, tmp);
            } else {
                std::snprintf(tmp, sizeof(tmp), "UT  DELAY%d) %cFeedback  %cEffectLevel     ", slot + 1, c0, c1);
                setLineUtil(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "                %d          %2d >NEXT>EXIT",
                              static_cast<int>(d.feedback), static_cast<int>(d.effectLevel));
                setLineUtil(frame.line1, tmp);
            }
            return true;
        }
        case ClassicScreen::SetupPanEdit: {
            const int slot = std::clamp(setupPerfFxSlot_, 0, 3);
            const auto& p = pef.pans[static_cast<std::size_t>(slot)];
            const char c0 = (setupPanCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (setupPanCursor_ == 1) ? kCursorRight : '>';
            const char c2 = (setupPanCursor_ == 2) ? kCursorRight : '>';
            std::snprintf(tmp, sizeof(tmp), "UT  PAN%d)  %cSelect %cDirection%cPanRange  ", slot + 1, c0, c1, c2);
            setLineUtil(frame.line0, tmp);
            static constexpr std::array<const char*, 3> kSel{ { "LFO ", "VEL ", "NOTE" } };
            const char dirArrow = (p.direction == 0u) ? kArrowRight : kArrowLeft;
            std::snprintf(tmp, sizeof(tmp), "             %-4s     L%cR       %2d >EXIT",
                          kSel[std::min<std::size_t>(p.select, 2u)], dirArrow, static_cast<int>(p.panRange));
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupChordEdit: {
            const int slot = std::clamp(setupPerfFxSlot_, 0, 3);
            const auto& ch = pef.chords[static_cast<std::size_t>(slot)];
            const auto& notes = ch.chords[static_cast<std::size_t>(std::clamp(setupChordKey_, 0, 11))];
            char trigStr[4]{};
            formatMidiNote3(60 + setupChordKey_, trigStr);
            std::snprintf(tmp, sizeof(tmp), "UT  CHORD%d)   %-3sis .....      >KBD     ", slot + 1, trigStr);
            setLineUtil(frame.line0, tmp);
            char n0[4]{}, n1[4]{}, n2[4]{}, n3[4]{};
            formatMidiNote3(notes[0], n0);
            formatMidiNote3(notes[1], n1);
            formatMidiNote3(notes[2], n2);
            formatMidiNote3(notes[3], n3);
            std::snprintf(tmp, sizeof(tmp), " %c key %c   %c%3s/%3s/%3s/%3s  %2s >EXIT",
                          kArrowLeft, kArrowRight, kCursorRight,
                          n0, n1, n2, n3, setupChordKbdIn_ ? "in" : "  ");
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupMicroRoot: {
            setLineUtil(frame.line0, "UT  MICRO TUNE) OCT.      FULL          ");
            setLineUtil(frame.line1, "               >Edit>Init>Edit>Init>EXIT");
            return true;
        }
        case ClassicScreen::SetupMicroOctEdit:
        case ClassicScreen::SetupMicroFullEdit: {
            const bool isOct = (screen_ == ClassicScreen::SetupMicroOctEdit);
            const auto& entry = isOct
                                    ? mem.userOctaveMicrotuning()[static_cast<std::size_t>(std::clamp(setupMicroKey_, 0, 11))]
                                    : mem.userFullMicrotuning()[static_cast<std::size_t>(std::clamp(setupMicroKey_, 0, 127))];
            char keyStr[5]{};
            formatMidiNote4(isOct ? (60 + setupMicroKey_) : setupMicroKey_, keyStr);
            char crsStr[5]{};
            formatMidiNote4(entry.coarseNote, crsStr);
            const int totalSteps = (static_cast<int>(entry.coarseNote) - 13) * 64 + static_cast<int>(entry.fineStep64);
            std::snprintf(tmp, sizeof(tmp), "UT  MICRO) %-5s  %-4s     key set      ",
                          isOct ? "OCT. " : "FULL.", keyStr);
            setLineUtil(frame.line0, tmp);
            std::snprintf(tmp, sizeof(tmp), "%cCRS %cFINE  %4s %+3d(%5d) %c    %c >EXIT",
                          (setupMicroCursor_ == 0 ? kCursorRight : '>'),
                          (setupMicroCursor_ == 1 ? kCursorRight : '>'),
                          crsStr, static_cast<int>(entry.fineStep64), totalSteps,
                          kArrowLeft, kArrowRight);
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupMicroOctInit:
        case ClassicScreen::SetupMicroFullInit: {
            if (setupMicroInitConfirm_) {
                setLineUtil(frame.line0, "UT  MICRO) INIT     Are you sure?       ");
                setLineUtil(frame.line1, "Push YES to Init / NO to Cancel    >EXIT");
                return true;
            }
            static constexpr std::array<const char*, 12> kKeyNames{ {
                "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
            } };
            std::snprintf(tmp, sizeof(tmp), "UT  MICRO) INIT    %cTable      %ckey     ",
                          (setupMicroInitCursor_ == 0 ? kCursorRight : '>'),
                          (setupMicroInitCursor_ == 1 ? kCursorRight : '>'));
            setLineUtil(frame.line0, tmp);
            const std::string_view tblName = microTableShortName(setupMicroInitTable_);
            std::snprintf(tmp, sizeof(tmp), ">GO           %2d:%.*s %-2s >EXIT",
                          setupMicroInitTable_ + 1,
                          static_cast<int>(tblName.size()), tblName.data(),
                          kKeyNames[static_cast<std::size_t>(std::clamp(setupMicroInitKey_, 0, 11))]);
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupVel: {
            const char c0 = (setupVelCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (setupVelCursor_ == 1) ? kCursorRight : '>';
            std::snprintf(tmp, sizeof(tmp), "UT  SETUP)%cFixedVelocity %cVelCurve      ", c0, c1);
            setLineUtil(frame.line0, tmp);
            char fixedStr[4]{};
            if (sys.fixedVelocity == 0u) std::snprintf(fixedStr, sizeof(fixedStr), "off");
            else std::snprintf(fixedStr, sizeof(fixedStr), "%3d", static_cast<int>(sys.fixedVelocity));
            const std::string_view vc = velCurveLabel(sys.velocityCurve);
            std::snprintf(tmp, sizeof(tmp), "               %-3s      %.*s >EXIT",
                          fixedStr, static_cast<int>(vc.size()), vc.data());
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::SetupDamp: {
            const char c0 = (setupDampCursor_ == 0) ? kCursorRight : '>';
            const char c1 = (setupDampCursor_ == 1) ? kCursorRight : '>';
            std::snprintf(tmp, sizeof(tmp), "UT  SETUP) %cEG Damp  %cVoice Damp        ", c0, c1);
            setLineUtil(frame.line0, tmp);
            const std::string_view eg = egDampLabel(sys.egDampSpeed);
            std::snprintf(tmp, sizeof(tmp), "            %.*s      %-3s      >EXIT",
                          static_cast<int>(eg.size()), eg.data(),
                          sys.voiceDampOn ? "on " : "off");
            setLineUtil(frame.line1, tmp);
            return true;
        }

        // =====================================================================
        // Demo & Power-On Greeting Message (pp. 11, 13-15, 22)
        // =====================================================================
        case ClassicScreen::DemoRoot: {
            setLineUtil(frame.line0, "UT  DEMO)     Select one !              ");
            setLineUtil(frame.line1, ">ROM Play >Disk Play >ChainPlay >V. Edit");
            return true;
        }
        case ClassicScreen::DemoConfirm: {
            static constexpr std::array<const char*, 4> kModes{ {
                "ROM Play ", "Disk Play", "ChainPlay", "V. Edit  "
            } };
            std::snprintf(tmp, sizeof(tmp), "UT  DEMO)  %-9s    Are you sure?   ",
                          kModes[static_cast<std::size_t>(std::clamp(demoSubMode_, 0, 3))]);
            setLineUtil(frame.line0, tmp);
            setLineUtil(frame.line1, "All data will be changed !   [No]  [Yes]");
            return true;
        }
        case ClassicScreen::DemoPlay: {
            if (demoBusyTicks_ > 0) {
                setLineUtil(frame.line0, "            *** BUSY ***                ");
                setLineUtil(frame.line1, "                                        ");
                return true;
            }
            setLineUtil(frame.line0, "UT  DEMO)  Select song & push  START    ");
            static constexpr std::array<const char*, 3> kRomTitles{ {
                "Victory !    ", "Route 50     ", "MoonLite     "
            } };
            std::string titleStr(kRomTitles[static_cast<std::size_t>(std::clamp(demoSelectedSong_, 0, 2))]);
            if (demoSubMode_ == 1 && !diskManager_.entries().empty()) {
                const std::size_t di = static_cast<std::size_t>(demoSelectedSong_) % diskManager_.entries().size();
                titleStr = diskManager_.entries()[di].name;
            }
            std::snprintf(tmp, sizeof(tmp), "%cSong %d : %-13.13s >START>STOP>EXIT",
                          kCursorRight, demoSelectedSong_ + 1, titleStr.c_str());
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::DemoVoiceEdit: {
            if (demoBusyTicks_ > 0) {
                setLineUtil(frame.line0, "            *** BUSY ***                ");
                setLineUtil(frame.line1, "                                        ");
                return true;
            }
            const int pct = std::clamp(static_cast<int>(std::lround(demoVEditSpeed_ * 100.0f)), 1, 100);
            std::snprintf(tmp, sizeof(tmp), "UT  DEMO)  V.Edit Demo  Spd:%3d%%        ", pct);
            setLineUtil(frame.line0, tmp);
            std::snprintf(tmp, sizeof(tmp), "Step %02d  Use DATA ENTRY            >EXIT", demoVEditStep_ + 1);
            setLineUtil(frame.line1, tmp);
            return true;
        }
        case ClassicScreen::UtilEditMessage: {
            std::snprintf(tmp, sizeof(tmp), "UT EDIT MESSAGE)                 %c    %c ", kArrowLeft, kArrowRight);
            setLineUtil(frame.line0, tmp);
            frame.line1 = sys.powerOnMessage;
            frame.cursorRow = 1;
            frame.cursorCol = std::clamp(msgEditCursorCol_, 0, 39);
            return true;
        }
        default:
            return false;
    }
}

void VF50ClassicController::applyUtilLedSnapshot(ClassicLedSnapshot& leds) const noexcept {
    // Spec §0: Card, Midi, Setup have NO dedicated LEDs; preserve underlying Play/Memory/Track LEDs.
    // DEMO LED lights green whenever any DEMO / UT EDIT MESSAGE screen is active.
    leds.demo = demoLedState() ? LedState::SolidGreen : LedState::Off;
    if (screen_ == ClassicScreen::MidiPgmEdit) {
        const auto& entry = processor_.pgmChangeTable().entries[static_cast<std::size_t>(midiPgmEditSlot_)];
        leds.memInt    = (entry.bank == 0u) ? LedState::SolidGreen : LedState::Off;
        leds.memCard   = (entry.bank == 1u) ? LedState::SolidGreen : LedState::Off;
        leds.memPreset = (entry.bank == 2u) ? LedState::SolidGreen : LedState::Off;
    } else if (screen_ == ClassicScreen::DemoPlay) {
        leds.seq = processor_.isDemoPlaying() ? LedState::SolidGreen : LedState::Off;
    }
}

} // namespace vf50::classic
