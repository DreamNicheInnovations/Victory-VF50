#include "Classic/VF50ClassicController.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vf50::classic {

namespace {

constexpr char kGlyphPlayArrow  = static_cast<char>(0x10u); // '▶'
constexpr char kGlyphRightArrow = static_cast<char>(0x7Eu); // '→'
constexpr char kGlyphLeftArrow  = static_cast<char>(0x7Fu); // '←'
constexpr char kGlyphDiamondHit = static_cast<char>(0x0Fu); // '♦' -> '*' in getLineAsString()
constexpr char kGlyphQtrNote    = static_cast<char>(0x0Bu); // '♪' -> 'q' in getLineAsString()

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

// Manual p. 57: 61 PCM Rhythm Instruments in exact firmware order
constexpr std::array<const char*, 61> kRhythmInstNames = {{
    "BD 1",   "BD 2",   "BD 3",   "H. BD",  "GateBD", "E. BD",  "SD 1",   "SD 2",
    "Pic1SD", "H. SD 1","H. SD 2","GateSD", "E. SD",  "Rim 1",  "Rim 2",  "Tom 1",
    "Tom 2",  "Tom 3",  "Tom 4",  "F.Tom1", "F.Tom2", "F.Tom3", "F.Tom4", "E.Tom1",
    "E.Tom2", "E.Tom3", "E.Tom4", "HHclos", "HHopen", "HH1/4o", "HHpdl",  "Ride",
    "Edge",   "Crash",  "FMprc1", "FMprc2", "FMprc3", "GlsCsh", "BellTr", "TimpnH",
    "TimphL", "Claps",  "Shaker", "Cowbel", "Timb1H", "Timb1L", "Whst1S", "Whst1L",
    "CgaHMT", "CgaHOP", "Cga LO", "Bgo HI", "Bgo LO", "CuicaH", "CuicaL", "Ago HI",
    "Ago LO", "Tambrn", "Claves", "Cstnt",  "VbrSlp"
}};

struct TimeSigEntry {
    uint8_t num;
    uint8_t den;
};

constexpr std::array<TimeSigEntry, 56> buildTimeSigTable() noexcept {
    std::array<TimeSigEntry, 56> tbl{};
    std::size_t idx = 0u;
    for (uint8_t n = 1u; n <= 8u; ++n)  tbl[idx++] = { n, 4u };
    for (uint8_t n = 1u; n <= 16u; ++n) tbl[idx++] = { n, 8u };
    for (uint8_t n = 1u; n <= 32u; ++n) tbl[idx++] = { n, 16u };
    return tbl;
}

constexpr std::array<TimeSigEntry, 56> kTimeSigTable = buildTimeSigTable();

constexpr std::array<const char*, 8> kQntzLabels = {{
    "1/4 ", "1/6 ", "1/8 ", "1/12", "1/16", "1/24", "1/32", "off "
}};

constexpr std::array<uint16_t, 8> kQntzTicks = {{
    48u, 32u, 24u, 16u, 12u, 8u, 6u, 1u
}};

constexpr std::array<const char*, 6> kBeatLabels = {{
    "1/4 ", "1/6 ", "1/8 ", "1/12", "1/24", "1/32"
}};

constexpr std::array<uint16_t, 6> kBeatTicks = {{
    48u, 32u, 24u, 16u, 8u, 6u
}};

constexpr std::array<const char*, 7> kPanGraphics = {{
    "L<  R", "L<- R", "L<| R", "L|||R", "L |>R", "L ->R", "L  >R"
}};

[[nodiscard]] int findTimeSigIndex(uint8_t num, uint8_t den) noexcept {
    for (std::size_t i = 0u; i < kTimeSigTable.size(); ++i) {
        if (kTimeSigTable[i].num == num && kTimeSigTable[i].den == den) {
            return static_cast<int>(i);
        }
    }
    return 3; // 4/4
}

void formatMidiNote4(int midiNote, char out[5]) noexcept {
    static constexpr std::array<const char*, 12> kNotes = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int n = std::clamp(midiNote, 0, 127);
    const int oct = (n / 12) - 2;
    std::snprintf(out, 5, "%s%d", kNotes[static_cast<std::size_t>(n % 12)], oct);
}

[[nodiscard]] uint8_t midiVelocityToLevel1To8(uint8_t vel, bool velOn) noexcept {
    if (!velOn) {
        return 5u; // Spec §3.3 (p. 68): Vel=off -> fixed accent 5
    }
    const int scaled = (static_cast<int>(std::clamp<uint8_t>(vel, 1u, 127u)) * 7) / 127;
    return static_cast<uint8_t>(std::clamp(scaled + 1, 1, 8));
}

[[nodiscard]] int collectSongMarks(const vf50::generated::RhythmSongData& song,
                                   std::array<char, 7>& outMarks,
                                   std::array<uint16_t, 7>& outParts) noexcept {
    int count = 0;
    for (uint16_t p = 0u; p < song.partCount && count < 7; ++p) {
        if (song.parts[p].type == vf50::generated::RhythmSongPartType::Mark) {
            outMarks[static_cast<std::size_t>(count)] = song.parts[p].markChar;
            outParts[static_cast<std::size_t>(count)] = p;
            ++count;
        }
    }
    return count;
}

void formatSongPartDesc(const vf50::generated::RhythmSongPart& part,
                        bool hasData,
                        char out[12]) noexcept {
    using T = vf50::generated::RhythmSongPartType;
    switch (part.type) {
        case T::Pattern:
            std::snprintf(out, 12, "PTN %c%02d%c   ",
                          part.ptnIsUser ? 'I' : 'P',
                          std::clamp<int>(part.ptnIndex, 0, 99),
                          hasData ? 'w' : ' ');
            break;
        case T::RepeatBegin:
            std::snprintf(out, 12, "||:        ");
            break;
        case T::RepeatEnd:
            std::snprintf(out, 12, ":|| x %-2d   ", std::clamp<int>(part.repeatCount, 2, 99));
            break;
        case T::Volume:
            std::snprintf(out, 12, "Vol%+3d     ", static_cast<int>(part.volDelta));
            break;
        case T::Tempo:
            std::snprintf(out, 12, "T%+3d/%-2d    ",
                          static_cast<int>(part.tempoDelta),
                          static_cast<int>(part.tempoBeats));
            break;
        case T::Mark:
            std::snprintf(out, 12, "Mark [%c]   ", part.markChar);
            break;
    }
}

} // namespace

bool VF50ClassicController::isRhythmScreen(ClassicScreen s) noexcept {
    switch (s) {
        case ClassicScreen::RhythmPtnPlay:
        case ClassicScreen::RhythmSongPlay:
        case ClassicScreen::RhythmPtnRecSetup:
        case ClassicScreen::RhythmPtnRecReal:
        case ClassicScreen::RhythmPtnRecStep:
        case ClassicScreen::RhythmPtnRecStore:
        case ClassicScreen::RhythmPtnJobSelect:
        case ClassicScreen::RhythmPtnJobCopy:
        case ClassicScreen::RhythmPtnJobClear:
        case ClassicScreen::RhythmSetup:
        case ClassicScreen::RhythmInstSet:
        case ClassicScreen::RhythmKeyAssign:
        case ClassicScreen::RhythmSongJobSelect:
        case ClassicScreen::RhythmSongEdit:
        case ClassicScreen::RhythmSongEditJob:
        case ClassicScreen::RhythmSongJobCopy:
        case ClassicScreen::RhythmSongJobClear:
        case ClassicScreen::RhythmSongJobSearch:
            return true;
        default:
            return false;
    }
}

void VF50ClassicController::initRhythmDefaults() noexcept {
    if (rhythmSongs_ != nullptr) {
        for (auto& s : *rhythmSongs_) {
            s.clear();
        }
    }
    rhyVoiceHitTicks_.fill(0u);
    lastRhythmSubmodeIsSong_ = false;
    rhyPtnIsUser_ = false;
    rhyPtnIdx_ = 0;
    rhyTempoBpm_ = 120;
    rhyKbdIsRhy_ = true;
    rhyAsgnMapIdx_ = 0;
    rhyRch_ = 9; // Ch 10 (1-based)
}

const vf50::generated::RhythmSongData& VF50ClassicController::rhythmSong(std::size_t idx0To7) const noexcept {
    static const vf50::generated::RhythmSongData kEmpty{};
    if (rhythmSongs_ == nullptr) {
        return kEmpty;
    }
    return (*rhythmSongs_)[std::min<std::size_t>(idx0To7, 7u)];
}

void VF50ClassicController::setRhythmTempoNormalized(float norm0To1) noexcept {
    const int nextBpm = 30 + static_cast<int>(std::lround(std::clamp(norm0To1, 0.0f, 1.0f) * 210.0f));
    rhyTempoBpm_ = std::clamp(nextBpm, 30, 240);
    processor_.requestClassicRhythmSetTempo(static_cast<double>(rhyTempoBpm_));
}

bool VF50ClassicController::patternHasData(bool isUser, int ptnIdx) const noexcept {
    const int idx = std::clamp(ptnIdx, 0, 99);
    if (isUser) {
        return processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(idx)).eventCount > 0u;
    }
    return processor_.activeRomBank().factoryRhythmPatterns[static_cast<std::size_t>(idx)].eventCount > 0u;
}

uint8_t VF50ClassicController::patternBars(bool isUser, int ptnIdx) const noexcept {
    const int idx = std::clamp(ptnIdx, 0, 99);
    if (isUser) {
        const auto& p = processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(idx));
        return std::clamp<uint8_t>(p.bars, 1u, 4u);
    }
    return std::clamp<uint8_t>(processor_.activeRomBank().factoryRhythmPatterns[static_cast<std::size_t>(idx)].bars, 1u, 4u);
}

uint8_t VF50ClassicController::patternTimeSigNum(bool isUser, int ptnIdx) const noexcept {
    const int idx = std::clamp(ptnIdx, 0, 99);
    if (isUser) {
        return processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(idx)).timeSigNum;
    }
    return processor_.activeRomBank().factoryRhythmPatterns[static_cast<std::size_t>(idx)].timeSigNum;
}

uint8_t VF50ClassicController::patternTimeSigDen(bool isUser, int ptnIdx) const noexcept {
    const int idx = std::clamp(ptnIdx, 0, 99);
    if (isUser) {
        return processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(idx)).timeSigDen;
    }
    return processor_.activeRomBank().factoryRhythmPatterns[static_cast<std::size_t>(idx)].timeSigDen;
}

uint16_t VF50ClassicController::computeTicksPerBar(uint8_t num, uint8_t den) const noexcept {
    const int d = std::max<int>(den, 4);
    const int perBeat = 192 / d;
    return static_cast<uint16_t>(std::max(12, static_cast<int>(num) * perBeat));
}

uint16_t VF50ClassicController::activeRecQuantizeStepTicks() const noexcept {
    return kQntzTicks[static_cast<std::size_t>(std::clamp(recQntzIdx_, 0, 7))];
}

void VF50ClassicController::commitRhythmKitAndSync() noexcept {
    auto& kit = processor_.hostMemory().mutableActiveRhythmKit();
    processor_.requestClassicRhythmCommitKit(kit);
}

void VF50ClassicController::pushActiveRhythmKeyMapToAudio() noexcept {
    auto& mem = processor_.hostMemory();
    const auto& rom = processor_.activeRomBank();
    if (rhyAsgnMapIdx_ < 3) {
        mem.selectRhythmKeyMap(static_cast<std::size_t>(rhyAsgnMapIdx_));
        processor_.requestClassicRhythmApplyKeyMap(rom.presetDrumKeyMaps[static_cast<std::size_t>(rhyAsgnMapIdx_)], 31u);
    } else {
        const int uIdx = std::clamp(rhyAsgnMapIdx_ - 3, 0, 1);
        mem.selectRhythmKeyMap(static_cast<std::size_t>(3 + uIdx));
        processor_.requestClassicRhythmApplyKeyMap(mem.userKeyMap(static_cast<std::size_t>(uIdx)), 36u);
    }
}

void VF50ClassicController::updateRhythmMetronomeForMode() noexcept {
    const uint16_t interval = kBeatTicks[static_cast<std::size_t>(std::clamp(rhyBeatIdx_, 0, 5))];
    const uint16_t tpb = (screen_ == ClassicScreen::RhythmPtnRecReal)
                             ? computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen)
                             : computeTicksPerBar(patternTimeSigNum(rhyPtnIsUser_, rhyPtnIdx_),
                                                  patternTimeSigDen(rhyPtnIsUser_, rhyPtnIdx_));
    const bool enable = (screen_ == ClassicScreen::RhythmPtnRecReal) || rhyClickPlay_;
    processor_.requestClassicRhythmSetMetronome(enable, interval, tpb);
}

void VF50ClassicController::startRhythmPlayCurrent() noexcept {
    rhyQueuedPtnActive_ = false;
    updateRhythmMetronomeForMode();
    if (rhyPtnIsUser_) {
        const auto& uPtn = processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(rhyPtnIdx_));
        processor_.requestClassicRhythmStartUser(static_cast<std::size_t>(rhyPtnIdx_), uPtn, static_cast<double>(rhyTempoBpm_), true);
    } else {
        processor_.requestClassicRhythmStartFactory(static_cast<std::size_t>(rhyPtnIdx_), static_cast<double>(rhyTempoBpm_), true);
    }
}

void VF50ClassicController::stopRhythmPlayCurrent() noexcept {
    rhyQueuedPtnActive_ = false;
    processor_.requestClassicRhythmSetMetronome(false, 48u, 192u);
    processor_.requestClassicRhythmStop();
    if (rhySongSyncedSeq_ || processor_.isDemoPlaying()) {
        processor_.requestDemoStop();
        processor_.setClassicDemoSuppressDrums(false);
        rhySongSyncedSeq_ = false;
    }
}

void VF50ClassicController::startRhythmSongCurrent(uint16_t startPart, uint16_t startMeas) noexcept {
    const auto& song = rhythmSong(static_cast<std::size_t>(activeRhythmSong_));
    if (!song.hasData()) {
        setNotice("Empty Rhythm Song");
        return;
    }
    processor_.requestClassicRhythmSetMetronome(rhyClickPlay_, kBeatTicks[static_cast<std::size_t>(std::clamp(rhyBeatIdx_, 0, 5))], 192u);
    processor_.requestClassicRhythmStartSong(song, startPart, startMeas, static_cast<double>(rhyTempoBpm_));

    // Spec §3.1 (p. 58): Rhythm Song mode plays matching Sequencer song slot (1..8) simultaneously when populated
    const std::size_t seqIdx = static_cast<std::size_t>(std::clamp(activeRhythmSong_, 0, 7));
    if (processor_.hasUserSequenceInSlot(seqIdx)) {
        processor_.setClassicDemoSuppressDrums(true);
        processor_.requestDemoStart(seqIdx);
        rhySongSyncedSeq_ = true;
    }
}

void VF50ClassicController::loadWorkPatternFromSelection() noexcept {
    recWorkPattern_.clear();
    recSourceWasPreset_ = !recPtnIsUser_;
    recModified_ = false;

    if (recPtnIsUser_) {
        const auto& src = processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(recPtnIdx_));
        if (src.eventCount > 0u) {
            recWorkPattern_ = src;
            recTimeSigIdx_ = findTimeSigIndex(src.timeSigNum, src.timeSigDen);
            recBars_ = std::clamp<int>(src.bars, 1, 4);
            return;
        }
    } else {
        const auto& rom = processor_.activeRomBank();
        const auto& fPtn = rom.factoryRhythmPatterns[static_cast<std::size_t>(recPtnIdx_)];
        const auto evs = rom.getRhythmPatternEvents(static_cast<std::size_t>(recPtnIdx_));
        if (!evs.empty()) {
            recWorkPattern_.bars = std::clamp<uint8_t>(fPtn.bars, 1u, 4u);
            recWorkPattern_.timeSigNum = fPtn.timeSigNum;
            recWorkPattern_.timeSigDen = fPtn.timeSigDen;
            recWorkPattern_.totalTicks = std::max<uint16_t>(fPtn.totalTicks, 12u);
            const std::size_t cnt = std::min<std::size_t>(evs.size(), vf50::state::UserRhythmPattern::kMaxEvents);
            for (std::size_t i = 0u; i < cnt; ++i) {
                recWorkPattern_.events[i] = evs[i];
            }
            recWorkPattern_.eventCount = static_cast<uint16_t>(cnt);
            recTimeSigIdx_ = findTimeSigIndex(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
            recBars_ = std::clamp<int>(recWorkPattern_.bars, 1, 4);
            return;
        }
    }

    const auto& ts = kTimeSigTable[static_cast<std::size_t>(std::clamp(recTimeSigIdx_, 0, 55))];
    recWorkPattern_.bars = static_cast<uint8_t>(std::clamp(recBars_, 1, 4));
    recWorkPattern_.timeSigNum = ts.num;
    recWorkPattern_.timeSigDen = ts.den;
    recWorkPattern_.totalTicks = static_cast<uint16_t>(computeTicksPerBar(ts.num, ts.den) * recWorkPattern_.bars);
}

void VF50ClassicController::commitWorkPatternToSlot(int userSlot0To99) noexcept {
    const int slot = std::clamp(userSlot0To99, 0, 99);
    processor_.hostMemory().mutableUserRhythmPattern(static_cast<std::size_t>(slot)) = recWorkPattern_;
    processor_.requestClassicRhythmCommitUserPattern(static_cast<std::size_t>(slot), recWorkPattern_);
}

void VF50ClassicController::syncRhythmTelemetry30Hz() noexcept {
    std::array<vf50::plugin::VF50AudioProcessor::RhythmUiNoteEvent, 32> events{};
    const std::size_t n = processor_.consumeRhythmUiNoteEvents(events.data(), events.size());
    for (std::size_t i = 0u; i < n; ++i) {
        (void)onRhythmKeyNoteOn(events[i].midiNote, events[i].velocity, heldEraseMod_, heldInstSoftkey_);
    }

    const uint8_t hitMask = processor_.consumeRhythmVoiceHitMask();
    const uint8_t actMask = processor_.rhythmActiveVoiceMask();
    for (std::size_t v = 0u; v < 8u; ++v) {
        if (((hitMask >> v) & 1u) != 0u) {
            rhyVoiceHitTicks_[v] = 8u;
        } else if (((actMask >> v) & 1u) != 0u) {
            rhyVoiceHitTicks_[v] = std::max<uint8_t>(rhyVoiceHitTicks_[v], 2u);
        } else if (rhyVoiceHitTicks_[v] > 0u) {
            --rhyVoiceHitTicks_[v];
        }
    }

    const uint32_t curWrap = processor_.rhythmPatternWrapCount();
    if (rhyQueuedPtnActive_ && curWrap != rhyQueuedStartWrap_ && !processor_.isRhythmNextPatternQueued()) {
        rhyPtnIsUser_ = rhyQueuedIsUser_;
        rhyPtnIdx_ = rhyQueuedIdx_;
        rhyQueuedPtnActive_ = false;
    }

    if (recQntzPendingBlink_ && curWrap != recQntzWrapAnchor_) {
        recQntzIdx_ = recPendingQntzIdx_;
        recQntzPendingBlink_ = false;
    }

    if (rhySongSyncedSeq_ && !processor_.isRhythmSongPlaying()) {
        processor_.requestDemoStop();
        processor_.setClassicDemoSuppressDrums(false);
        rhySongSyncedSeq_ = false;
    }
}

bool VF50ClassicController::onRhythmKeyNoteOn(uint8_t midiNote,
                                             uint8_t velocity,
                                             bool eraseHeld,
                                             bool instSelectHeld) noexcept {
    if (velocity == 0u) {
        return false;
    }
    const uint8_t cleanNote = midiNote & 0x7Fu;
    const auto& rom = processor_.activeRomBank();

    auto resolveDrumForKey = [&]() -> int {
        if (rhyAsgnMapIdx_ < 3) {
            if (cleanNote >= 31u && cleanNote <= 91u) {
                return static_cast<int>(rom.presetDrumKeyMaps[static_cast<std::size_t>(rhyAsgnMapIdx_)][cleanNote - 31u]);
            }
            return -1;
        }
        if (cleanNote >= 36u && cleanNote <= 96u) {
            const int uIdx = std::clamp(rhyAsgnMapIdx_ - 3, 0, 1);
            return static_cast<int>(processor_.hostMemory().userKeyMap(static_cast<std::size_t>(uIdx))[cleanNote - 36u]);
        }
        return -1;
    };

    const bool useErase = eraseHeld || instMinusArmed_;

    if (screen_ == ClassicScreen::RhythmInstSet) {
        const int d = resolveDrumForKey();
        if (d >= 0 && d < 61) {
            rhyInstSelected_ = d;
            return true;
        }
        return false;
    }

    if (screen_ == ClassicScreen::RhythmKeyAssign) {
        if (rhyAsgnMapIdx_ < 3 && cleanNote >= 31u && cleanNote <= 91u) {
            rhyAsgnKeyIdx_ = static_cast<int>(cleanNote - 31u);
            return true;
        }
        if (rhyAsgnMapIdx_ >= 3 && cleanNote >= 36u && cleanNote <= 96u) {
            rhyAsgnKeyIdx_ = static_cast<int>(cleanNote - 36u);
            return true;
        }
        return false;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecStep) {
        const int d = resolveDrumForKey();
        if (d < 0 || d >= 61) {
            return false;
        }
        if (instSelectHeld) {
            // Spec §3.2 (p. 64): hold softkey under instrument + press key switches instrument without recording
            stepSelectedDrum_ = d;
            return true;
        }
        stepSelectedDrum_ = d;
        if (useErase) {
            if (recWorkPattern_.eraseEventsAtTick(stepAbsTick_, static_cast<uint8_t>(d), 0u)) {
                recModified_ = true;
            }
            instMinusArmed_ = false;
            return true;
        }
        const uint8_t velLvl = midiVelocityToLevel1To8(velocity, rhyVelOn_);
        if (auto* existing = recWorkPattern_.findMutableEventAtTick(stepAbsTick_, static_cast<uint8_t>(d))) {
            existing->velocityLevel = velLvl;
            recModified_ = true;
        } else if (recWorkPattern_.addEventSorted(stepAbsTick_, static_cast<uint8_t>(d), velLvl)) {
            recModified_ = true;
        }
        processor_.enqueueUiMidi3(static_cast<uint8_t>(0x90u | (rhyRch_ & 0x0Fu)), cleanNote, velocity);
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecReal) {
        const int d = resolveDrumForKey();
        if (d < 0 || d >= 61) {
            return false;
        }
        const uint16_t rawTick = processor_.rhythmCurrentTick();
        const uint16_t total = std::max<uint16_t>(recWorkPattern_.totalTicks, 12u);
        const uint16_t step = activeRecQuantizeStepTicks();
        uint16_t qTick = rawTick;
        if (step > 1u) {
            const uint32_t rounded = ((static_cast<uint32_t>(rawTick) + (step / 2u)) / step) * step;
            qTick = static_cast<uint16_t>(rounded % total);
        } else {
            qTick = static_cast<uint16_t>(rawTick % total);
        }

        if (useErase) {
            const uint16_t tol = static_cast<uint16_t>(std::max<int>(6, static_cast<int>(step) / 2));
            if (recWorkPattern_.eraseEventsAtTick(qTick, static_cast<uint8_t>(d), tol)) {
                recModified_ = true;
                processor_.requestClassicRhythmUpdateUserInPlace(static_cast<std::size_t>(recPtnIdx_), recWorkPattern_);
            }
            instMinusArmed_ = false;
            return true;
        }

        const uint8_t velLvl = midiVelocityToLevel1To8(velocity, rhyVelOn_);
        if (auto* existing = recWorkPattern_.findMutableEventAtTick(qTick, static_cast<uint8_t>(d))) {
            existing->velocityLevel = velLvl;
            recModified_ = true;
        } else if (recWorkPattern_.addEventSorted(qTick, static_cast<uint8_t>(d), velLvl)) {
            recModified_ = true;
        }
        processor_.requestClassicRhythmUpdateUserInPlace(static_cast<std::size_t>(recPtnIdx_), recWorkPattern_);
        return true;
    }

    return false;
}

bool VF50ClassicController::handleRhythmJobPress() noexcept {
    if (screen_ == ClassicScreen::RhythmPtnPlay) {
        if (processor_.isRhythmEnginePlaying()) {
            stopRhythmPlayCurrent();
        }
        screen_ = ClassicScreen::RhythmPtnJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongPlay) {
        if (processor_.isRhythmEnginePlaying() || processor_.isRhythmSongPlaying()) {
            stopRhythmPlayCurrent();
        }
        screen_ = ClassicScreen::RhythmSongJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmPtnJobCopy || screen_ == ClassicScreen::RhythmPtnJobClear) {
        rhyCopyConfirm_ = false;
        rhyClearConfirm_ = false;
        screen_ = ClassicScreen::RhythmPtnJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSetup
        || screen_ == ClassicScreen::RhythmInstSet
        || screen_ == ClassicScreen::RhythmKeyAssign) {
        processor_.setClassicRhythmCaptureNotes(false);
        screen_ = rhySharedJobReturnToSong_ ? ClassicScreen::RhythmSongJobSelect : ClassicScreen::RhythmPtnJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmPtnJobSelect) {
        screen_ = ClassicScreen::RhythmPtnPlay;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEdit
        || screen_ == ClassicScreen::RhythmSongEditJob
        || screen_ == ClassicScreen::RhythmSongJobCopy
        || screen_ == ClassicScreen::RhythmSongJobClear
        || screen_ == ClassicScreen::RhythmSongJobSearch) {
        songCopyConfirm_ = false;
        rhySongClearConfirm_ = false;
        songEdCopyConfirm_ = false;
        screen_ = ClassicScreen::RhythmSongJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongJobSelect) {
        screen_ = ClassicScreen::RhythmSongPlay;
        return true;
    }
    return false;
}

bool VF50ClassicController::handleRhythmMemBank(int bIdx) noexcept {
    const bool wantUser = (bIdx != 2);
    if (bIdx == 1) {
        setNotice("Card N/A for Rhythm");
        return false;
    }

    if (screen_ == ClassicScreen::RhythmPtnPlay) {
        if (processor_.isRhythmEnginePlaying()) {
            rhyQueuedPtnActive_ = true;
            rhyQueuedIsUser_ = wantUser;
            rhyQueuedIdx_ = rhyPtnIdx_;
            rhyQueuedStartWrap_ = processor_.rhythmPatternWrapCount();
            if (wantUser) {
                processor_.requestClassicRhythmQueueUser(static_cast<std::size_t>(rhyQueuedIdx_),
                                                         processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(rhyQueuedIdx_)));
            } else {
                processor_.requestClassicRhythmQueueFactory(static_cast<std::size_t>(rhyQueuedIdx_));
            }
        } else {
            rhyPtnIsUser_ = wantUser;
        }
        rhyPtnPendingBank_ = wantUser ? 0 : 1;
        rhyPtnPendingTens_ = -1;
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecSetup && recSetupCursor_ == 1) {
        recPtnIsUser_ = wantUser;
        rhyPtnPendingTens_ = -1;
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecStore) {
        if (!wantUser) {
            setNotice("Preset Read-Only!");
            return false;
        }
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnJobCopy) {
        if (rhyCopyCursor_ == 0) {
            rhyCopySrc1IsUser_ = wantUser;
        } else if (rhyCopyCursor_ == 1) {
            rhyCopyHasSrc2_ = true;
            rhyCopySrc2IsUser_ = wantUser;
        } else {
            if (!wantUser) {
                setNotice("Preset Read-Only!");
                return false;
            }
        }
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnJobClear && (rhyClearSubMode_ == 1 || rhyClearSubMode_ == 2)) {
        if (!wantUser) {
            setNotice("Preset Read-Only!");
            return false;
        }
        return true;
    }

    if (screen_ == ClassicScreen::RhythmSongEdit && rhythmSongs_ != nullptr) {
        auto& song = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)];
        if (songEditPartIdx_ < static_cast<int>(song.partCount)) {
            auto& pt = song.parts[static_cast<std::size_t>(songEditPartIdx_)];
            if (pt.type == vf50::generated::RhythmSongPartType::Pattern) {
                pt.ptnIsUser = wantUser;
                return true;
            }
        }
    }

    return false;
}

bool VF50ClassicController::handleRhythmStopAndBwd() noexcept {
    if (screen_ == ClassicScreen::RhythmPtnPlay) {
        if (processor_.isRhythmEnginePlaying()) {
            stopRhythmPlayCurrent();
        }
        processor_.requestClassicRhythmRewind();
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongPlay) {
        if (processor_.isRhythmSongPlaying() || processor_.isRhythmEnginePlaying() || processor_.isDemoPlaying()) {
            stopRhythmPlayCurrent();
        }
        rhySongStartMeas_ = 1;
        rhySongStartPart_ = 0;
        processor_.requestClassicRhythmRewind();
        if (processor_.isDemoPlaying() || processor_.activeDemoMeasure() > 1) {
            processor_.requestDemoRewindToTop();
        }
        return true;
    }
    if (screen_ == ClassicScreen::RhythmPtnRecStep) {
        stepAbsTick_ = 0u;
        stepQntzBlink_ = false;
        return true;
    }
    return false;
}

bool VF50ClassicController::handleRhythmSeqOp(int btn) noexcept {
    switch (btn) {
        case 0: // [RECORD]
            if (screen_ == ClassicScreen::RhythmPtnPlay && !processor_.isRhythmEnginePlaying()) {
                recPtnIsUser_ = rhyPtnIsUser_;
                recPtnIdx_ = rhyPtnIdx_;
                recTimeSigIdx_ = findTimeSigIndex(patternTimeSigNum(recPtnIsUser_, recPtnIdx_),
                                                  patternTimeSigDen(recPtnIsUser_, recPtnIdx_));
                recBars_ = std::clamp<int>(patternBars(recPtnIsUser_, recPtnIdx_), 1, 4);
                recSetupCursor_ = 0;
                screen_ = ClassicScreen::RhythmPtnRecSetup;
                return true;
            }
            return false;

        case 1: // [BWD <<]
            if (screen_ == ClassicScreen::RhythmSongPlay && !processor_.isDemoPlaying() && !processor_.isRhythmSongPlaying()) {
                rhySongStartMeas_ = std::max(1, rhySongStartMeas_ - 1);
                return true;
            }
            if (screen_ == ClassicScreen::RhythmPtnRecStep) {
                const uint16_t step32 = 6u; // 1/32 note = 6 ticks at 48 PPQN
                const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
                stepAbsTick_ = (stepAbsTick_ >= step32) ? static_cast<uint16_t>(stepAbsTick_ - step32) : 0u;
                if ((stepAbsTick_ % tpb) == 0u) {
                    stepQntzBlink_ = false;
                }
                return true;
            }
            if (screen_ == ClassicScreen::RhythmSongEdit) {
                songEditTempoSub_ = 0;
                return true;
            }
            return false;

        case 2: // [STOP ■]
            if (screen_ == ClassicScreen::RhythmPtnRecReal) {
                stopRhythmPlayCurrent();
                processor_.setClassicRhythmCaptureNotes(false);
                recQntzPendingBlink_ = false;
                if (recModified_) {
                    if (recSourceWasPreset_) {
                        recStoreSlot_ = rhyPtnIdx_;
                        recStorePendingTens_ = -1;
                        recStoreConfirm_ = false;
                        screen_ = ClassicScreen::RhythmPtnRecStore;
                        return true;
                    }
                    commitWorkPatternToSlot(recPtnIdx_);
                    rhyPtnIsUser_ = true;
                    rhyPtnIdx_ = recPtnIdx_;
                }
                screen_ = ClassicScreen::RhythmPtnPlay;
                return true;
            }
            if (screen_ == ClassicScreen::RhythmPtnRecStep) {
                processor_.setClassicRhythmCaptureNotes(false);
                stepQntzBlink_ = false;
                if (recModified_) {
                    if (recSourceWasPreset_) {
                        recStoreSlot_ = rhyPtnIdx_;
                        recStorePendingTens_ = -1;
                        recStoreConfirm_ = false;
                        screen_ = ClassicScreen::RhythmPtnRecStore;
                        return true;
                    }
                    commitWorkPatternToSlot(recPtnIdx_);
                    rhyPtnIsUser_ = true;
                    rhyPtnIdx_ = recPtnIdx_;
                }
                screen_ = ClassicScreen::RhythmPtnPlay;
                return true;
            }
            if (screen_ == ClassicScreen::RhythmPtnRecSetup || screen_ == ClassicScreen::RhythmPtnRecStore) {
                recStoreConfirm_ = false;
                screen_ = ClassicScreen::RhythmPtnPlay;
                return true;
            }
            if (screen_ == ClassicScreen::RhythmPtnPlay || screen_ == ClassicScreen::RhythmSongPlay) {
                const bool anyPlaying = processor_.isRhythmEnginePlaying()
                                     || processor_.isRhythmSongPlaying()
                                     || processor_.isDemoPlaying();
                if (anyPlaying) {
                    stopRhythmPlayCurrent();
                    if (processor_.isDemoPlaying()) {
                        processor_.requestDemoStop();
                    }
                    return true;
                }
                return onPressStopAndBwdTogether();
            }
            stopRhythmPlayCurrent();
            return true;

        case 3: // [PLAY ▶]
            if (screen_ == ClassicScreen::RhythmPtnPlay) {
                if (!processor_.isRhythmEnginePlaying()) {
                    startRhythmPlayCurrent();
                }
                return true;
            }
            if (screen_ == ClassicScreen::RhythmSongPlay) {
                if (!processor_.isRhythmSongPlaying()) {
                    startRhythmSongCurrent(static_cast<uint16_t>(std::max(0, rhySongStartPart_)),
                                           static_cast<uint16_t>(std::max(1, rhySongStartMeas_)));
                }
                return true;
            }
            if (screen_ == ClassicScreen::RhythmPtnRecSetup) {
                loadWorkPatternFromSelection();
                if (recIsStep_) {
                    stepAbsTick_ = 0u;
                    stepCursorField_ = 0;
                    stepQntzBlink_ = false;
                    processor_.setClassicRhythmCaptureNotes(true);
                    screen_ = ClassicScreen::RhythmPtnRecStep;
                } else {
                    recQntzPendingBlink_ = false;
                    processor_.setClassicRhythmCaptureNotes(true);
                    screen_ = ClassicScreen::RhythmPtnRecReal;
                    updateRhythmMetronomeForMode();
                    processor_.requestClassicRhythmStartUser(static_cast<std::size_t>(recPtnIdx_),
                                                             recWorkPattern_,
                                                             static_cast<double>(rhyTempoBpm_),
                                                             true);
                }
                return true;
            }
            return false;

        case 4: // [FWD >>]
            if (screen_ == ClassicScreen::RhythmSongPlay && !processor_.isDemoPlaying() && !processor_.isRhythmSongPlaying()) {
                rhySongStartMeas_ = std::min(999, rhySongStartMeas_ + 1);
                return true;
            }
            if (screen_ == ClassicScreen::RhythmPtnRecStep) {
                const uint16_t step32 = 6u;
                const uint16_t total = std::max<uint16_t>(recWorkPattern_.totalTicks, 12u);
                const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
                stepAbsTick_ = static_cast<uint16_t>((stepAbsTick_ + step32) % total);
                if ((stepAbsTick_ % tpb) == 0u) {
                    stepQntzBlink_ = false;
                }
                return true;
            }
            if (screen_ == ClassicScreen::RhythmSongEdit) {
                songEditTempoSub_ = 1;
                return true;
            }
            return false;

        default:
            return false;
    }
}

bool VF50ClassicController::handleRhythmSoftKey(int k) noexcept {
    switch (screen_) {
        case ClassicScreen::RhythmPtnPlay:
            if (k == 0) {
                if (!processor_.isDemoPlaying() && !processor_.isRhythmEnginePlaying() && !processor_.isRhythmSongPlaying()) {
                    lastRhythmSubmodeIsSong_ = true;
                    screen_ = ClassicScreen::RhythmSongPlay;
                }
                return true;
            }
            if (k == 1 || k == 2) {
                rhyPtnCursor_ = 0;
            } else if (k == 5 || k == 6) {
                rhyPtnCursor_ = 1;
            } else if (k == 7) {
                rhyPtnCursor_ = 2;
                rhyKbdIsRhy_ = !rhyKbdIsRhy_;
                processor_.setClassicRhythmKbdOverride(rhyKbdIsRhy_ ? 1 : 0);
            }
            return true;

        case ClassicScreen::RhythmSongPlay:
            if (k == 0) {
                if (!processor_.isDemoPlaying() && !processor_.isRhythmSongPlaying() && !processor_.isRhythmEnginePlaying()) {
                    lastRhythmSubmodeIsSong_ = false;
                    screen_ = ClassicScreen::RhythmPtnPlay;
                }
                return true;
            }
            if (k >= 1 && k <= 3) {
                rhySongCursor_ = 0; // SONG
            } else if (k == 4) {
                rhySongCursor_ = 1; // >Meas
            } else if (k == 5) {
                rhySongCursor_ = 2; // >Part
            } else if (k == 6) {
                rhySongCursor_ = 3; // >Tempo
            } else if (k == 7) {
                rhySongCursor_ = 4; // >KBD
                rhyKbdIsRhy_ = !rhyKbdIsRhy_;
                processor_.setClassicRhythmKbdOverride(rhyKbdIsRhy_ ? 1 : 0);
            }
            return true;

        case ClassicScreen::RhythmPtnRecSetup:
            if (k == 0) {
                if (recSetupCursor_ == 0) recIsStep_ = !recIsStep_;
                recSetupCursor_ = 0;
            } else if (k == 1) {
                recSetupCursor_ = 1;
            } else if (k == 2) {
                recSetupCursor_ = 2;
            } else if (k == 3) {
                recSetupCursor_ = 3;
            } else if (k == 4) {
                recSetupCursor_ = 4;
            } else if (k == 5 || k == 6) {
                recSetupCursor_ = 5;
            } else if (k == 7) {
                screen_ = ClassicScreen::RhythmPtnPlay;
            }
            return true;

        case ClassicScreen::RhythmPtnRecReal:
            if (k == 4 || k == 5) {
                recSetupCursor_ = 4; // Tempo
            } else if (k == 6 || k == 7) {
                recSetupCursor_ = 5; // Qntz
            }
            return true;

        case ClassicScreen::RhythmPtnRecStep:
            if (k <= 1) {
                stepCursorField_ = 0; // >Qntz
            } else if (k == 2 || k == 3) {
                stepCursorField_ = 1; // >Inst
            } else if (k == 4 || k == 5) {
                const auto* ev = recWorkPattern_.findEventAtTick(stepAbsTick_, static_cast<uint8_t>(stepSelectedDrum_));
                if (ev != nullptr) {
                    stepCursorField_ = 2; // >Acc
                }
            } else if (k == 6) { // <- step back by quantize
                const uint16_t step = activeRecQuantizeStepTicks();
                const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
                stepAbsTick_ = (stepAbsTick_ >= step) ? static_cast<uint16_t>(stepAbsTick_ - step) : 0u;
                if ((stepAbsTick_ % tpb) == 0u) {
                    stepQntzBlink_ = false;
                }
            } else if (k == 7) { // -> step forward by quantize
                const uint16_t step = activeRecQuantizeStepTicks();
                const uint16_t total = std::max<uint16_t>(recWorkPattern_.totalTicks, 12u);
                const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
                stepAbsTick_ = static_cast<uint16_t>((stepAbsTick_ + step) % total);
                if ((stepAbsTick_ % tpb) == 0u) {
                    stepQntzBlink_ = false;
                }
            }
            return true;

        case ClassicScreen::RhythmPtnRecStore:
            if (k == 6) {
                recStoreConfirm_ = true;
            } else if (k == 7) {
                recStoreConfirm_ = false;
                screen_ = ClassicScreen::RhythmPtnPlay;
            }
            return true;

        case ClassicScreen::RhythmPtnJobSelect:
            rhySharedJobReturnToSong_ = false;
            if (k == 0) {
                rhyCopyCursor_ = 0;
                rhyCopyConfirm_ = false;
                screen_ = ClassicScreen::RhythmPtnJobCopy;
            } else if (k == 1) {
                rhyClearSubMode_ = 0;
                rhyClearConfirm_ = false;
                screen_ = ClassicScreen::RhythmPtnJobClear;
            } else if (k == 2) {
                rhySetupCursor_ = 0;
                screen_ = ClassicScreen::RhythmSetup;
            } else if (k == 3) {
                rhyInstCursor_ = 0;
                processor_.setClassicRhythmCaptureNotes(true);
                screen_ = ClassicScreen::RhythmInstSet;
            } else if (k == 4) {
                rhyAsgnCursor_ = 0;
                processor_.setClassicRhythmCaptureNotes(true);
                screen_ = ClassicScreen::RhythmKeyAssign;
            } else if (k == 7) {
                screen_ = ClassicScreen::RhythmPtnPlay;
            }
            return true;

        case ClassicScreen::RhythmPtnJobCopy:
            if (k == 7) {
                rhyCopyConfirm_ = false;
                screen_ = ClassicScreen::RhythmPtnJobSelect;
            } else if (k == 6) {
                const bool src1Ok = patternHasData(rhyCopySrc1IsUser_, rhyCopySrc1Idx_);
                if (!src1Ok) {
                    setNotice("Source 1 Empty!");
                    return true;
                }
                if (rhyCopyHasSrc2_) {
                    if (!patternHasData(rhyCopySrc2IsUser_, rhyCopySrc2Idx_)) {
                        setNotice("Source 2 Empty!");
                        return true;
                    }
                    const uint8_t n1 = patternTimeSigNum(rhyCopySrc1IsUser_, rhyCopySrc1Idx_);
                    const uint8_t d1 = patternTimeSigDen(rhyCopySrc1IsUser_, rhyCopySrc1Idx_);
                    const uint8_t n2 = patternTimeSigNum(rhyCopySrc2IsUser_, rhyCopySrc2Idx_);
                    const uint8_t d2 = patternTimeSigDen(rhyCopySrc2IsUser_, rhyCopySrc2Idx_);
                    if (n1 != n2 || d1 != d2) {
                        setNotice("Time Sig Mismatch!");
                        return true;
                    }
                    const int b1 = static_cast<int>(patternBars(rhyCopySrc1IsUser_, rhyCopySrc1Idx_));
                    const int b2 = static_cast<int>(patternBars(rhyCopySrc2IsUser_, rhyCopySrc2Idx_));
                    if (b1 + b2 > 4) {
                        setNotice("Length > 4 Bars!");
                        return true;
                    }
                }
                rhyCopyConfirm_ = true;
            } else if (k <= 1) {
                rhyCopyCursor_ = 0;
            } else if (k <= 3) {
                rhyCopyCursor_ = 1;
            } else {
                rhyCopyCursor_ = 2;
            }
            return true;

        case ClassicScreen::RhythmPtnJobClear:
            if (k == 7) {
                if (rhyClearSubMode_ != 0) {
                    rhyClearConfirm_ = false;
                    rhyClearSubMode_ = 0;
                } else {
                    screen_ = ClassicScreen::RhythmPtnJobSelect;
                }
                return true;
            }
            if (rhyClearSubMode_ == 0) {
                if (k <= 1)      { rhyClearSubMode_ = 1; rhyClearCursor_ = 0; }
                else if (k <= 3) { rhyClearSubMode_ = 2; rhyClearCursor_ = 0; }
                else if (k <= 5) { rhyClearSubMode_ = 3; rhyClearConfirm_ = true; }
            } else if (rhyClearSubMode_ == 1) {
                if (k == 6) rhyClearConfirm_ = true;
                else        rhyClearCursor_ = 0;
            } else if (rhyClearSubMode_ == 2) {
                if (k == 6) rhyClearConfirm_ = true;
                else if (k <= 1) rhyClearCursor_ = 0;
                else             rhyClearCursor_ = 1;
            }
            return true;

        case ClassicScreen::RhythmSetup:
            if (k == 7) {
                screen_ = rhySharedJobReturnToSong_ ? ClassicScreen::RhythmSongJobSelect : ClassicScreen::RhythmPtnJobSelect;
            } else if (k <= 1) {
                rhySetupCursor_ = 0; // Rch
            } else if (k == 2) {
                rhySetupCursor_ = 1; // Tch
            } else if (k == 3) {
                rhySetupCursor_ = 2; // Vel
            } else if (k == 4) {
                rhySetupCursor_ = 3; // Click
            } else if (k == 5) {
                rhySetupCursor_ = 4; // Beat
            } else if (k == 6) {
                rhySetupCursor_ = 5; // Sync
            }
            return true;

        case ClassicScreen::RhythmInstSet:
            if (k == 7) {
                processor_.setClassicRhythmCaptureNotes(false);
                screen_ = rhySharedJobReturnToSong_ ? ClassicScreen::RhythmSongJobSelect : ClassicScreen::RhythmPtnJobSelect;
            } else if (k <= 1) {
                rhyInstCursor_ = 0; // Inst
            } else if (k == 2) {
                rhyInstCursor_ = 1; // Volume
            } else if (k == 3) {
                rhyInstCursor_ = 2; // Pan
            } else if (k == 4) {
                rhyInstCursor_ = 3; // Note
            } else if (k == 5) {
                rhyInstCursor_ = 4; // R.Vol
            } else if (k == 6) {
                rhyInstCursor_ = 5; // Efct
            }
            return true;

        case ClassicScreen::RhythmKeyAssign:
            if (k == 7) {
                processor_.setClassicRhythmCaptureNotes(false);
                screen_ = rhySharedJobReturnToSong_ ? ClassicScreen::RhythmSongJobSelect : ClassicScreen::RhythmPtnJobSelect;
            } else if (k <= 2) {
                rhyAsgnCursor_ = 0; // Map
            } else if (k == 3 || k == 4) {
                rhyAsgnCursor_ = 1; // Key
            } else {
                rhyAsgnCursor_ = 2; // Inst
            }
            return true;

        case ClassicScreen::RhythmSongJobSelect:
            rhySharedJobReturnToSong_ = true;
            if (k == 0) {
                songEditPartIdx_ = 0;
                screen_ = ClassicScreen::RhythmSongEdit;
            } else if (k == 1) {
                songCopyConfirm_ = false;
                songCopyCursor_ = 0;
                screen_ = ClassicScreen::RhythmSongJobCopy;
            } else if (k == 2) {
                songClearSub_ = 0;
                rhySongClearConfirm_ = false;
                screen_ = ClassicScreen::RhythmSongJobClear;
            } else if (k == 3) {
                rhySetupCursor_ = 0;
                screen_ = ClassicScreen::RhythmSetup;
            } else if (k == 4) {
                rhyInstCursor_ = 0;
                processor_.setClassicRhythmCaptureNotes(true);
                screen_ = ClassicScreen::RhythmInstSet;
            } else if (k == 5) {
                rhyAsgnCursor_ = 0;
                processor_.setClassicRhythmCaptureNotes(true);
                screen_ = ClassicScreen::RhythmKeyAssign;
            } else if (k == 6) {
                screen_ = ClassicScreen::RhythmSongJobSearch;
            } else if (k == 7) {
                screen_ = ClassicScreen::RhythmSongPlay;
            }
            return true;

        case ClassicScreen::RhythmSongEdit:
            if (k == 7) {
                screen_ = ClassicScreen::RhythmSongPlay;
            } else if (k == 6) {
                songEdJobSub_ = 0;
                screen_ = ClassicScreen::RhythmSongEditJob;
            } else if (k == 2 || k == 3) {
                const auto& song = rhythmSong(static_cast<std::size_t>(activeRhythmSong_));
                const int maxP = std::min<int>(static_cast<int>(song.partCount), 998);
                if (k == 2) songEditPartIdx_ = std::max(0, songEditPartIdx_ - 1);
                else        songEditPartIdx_ = std::min(maxP, songEditPartIdx_ + 1);
                songEditPendingTens_ = -1;
            } else if ((k == 4 || k == 5) && rhythmSongs_ != nullptr) {
                auto& song = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)];
                if (songEditPartIdx_ >= static_cast<int>(song.partCount)) {
                    if (song.partCount < vf50::generated::RhythmSongData::kMaxParts) {
                        song.parts[song.partCount++] = vf50::generated::RhythmSongPart{};
                    }
                } else {
                    auto& pt = song.parts[static_cast<std::size_t>(songEditPartIdx_)];
                    const int dir = (k == 4) ? -1 : +1;
                    const int nextType = (static_cast<int>(pt.type) + 6 + dir) % 6;
                    pt.type = static_cast<vf50::generated::RhythmSongPartType>(nextType);
                }
            }
            return true;

        case ClassicScreen::RhythmSongEditJob:
            if (k == 7) {
                if (songEdJobSub_ != 0) {
                    songEdCopyConfirm_ = false;
                    songEdJobSub_ = 0;
                } else {
                    screen_ = ClassicScreen::RhythmSongEdit;
                }
                return true;
            }
            if (songEdJobSub_ == 0) {
                if (k == 0) {
                    songEdJobSub_ = 1; // >Part
                    songEdJumpTarget_ = songEditPartIdx_ + 1;
                } else if (k == 1) {
                    songEdJobSub_ = 2; // >Ins
                } else if (k == 2) {
                    songEdJobSub_ = 3; // >Del
                } else if (k == 3) {
                    songEdJobSub_ = 4; // >Copy
                    songEdCopyCursor_ = 0;
                    songEdCopyConfirm_ = false;
                } else if (k == 4) {
                    std::array<char, 7> marks{};
                    std::array<uint16_t, 7> parts{};
                    if (collectSongMarks(rhythmSong(static_cast<std::size_t>(activeRhythmSong_)), marks, parts) == 0) {
                        setNotice("No Marks in Song");
                    } else {
                        songEdJobSub_ = 5; // >Search
                    }
                } else if (k == 5 || k == 6) {
                    songEdNameCol_ = 0;
                    songEdJobSub_ = 6; // >Name
                }
            } else if (songEdJobSub_ == 1) { // >Part
                if (k == 6) {
                    const auto& song = rhythmSong(static_cast<std::size_t>(activeRhythmSong_));
                    const int maxP = static_cast<int>(song.partCount);
                    songEditPartIdx_ = std::clamp(songEdJumpTarget_ - 1, 0, maxP);
                    songEdJobSub_ = 0;
                    screen_ = ClassicScreen::RhythmSongEdit;
                }
            } else if (songEdJobSub_ == 4) { // >Copy
                if (k == 6)      songEdCopyConfirm_ = true;
                else if (k <= 1) songEdCopyCursor_ = 0;
                else if (k <= 3) songEdCopyCursor_ = 1;
                else             songEdCopyCursor_ = 2;
            } else if (songEdJobSub_ == 5) { // >Search
                std::array<char, 7> marks{};
                std::array<uint16_t, 7> parts{};
                const int mCnt = collectSongMarks(rhythmSong(static_cast<std::size_t>(activeRhythmSong_)), marks, parts);
                if (k < mCnt) {
                    songEditPartIdx_ = static_cast<int>(parts[static_cast<std::size_t>(k)]);
                    songEdJobSub_ = 0;
                    screen_ = ClassicScreen::RhythmSongEdit;
                }
            } else if (songEdJobSub_ == 6) { // >Name
                if (k == 4)      songEdNameCol_ = std::max(0, std::min(7, songEdNameCol_) - 1);
                else if (k == 5) songEdNameCol_ = std::min(7, std::min(7, songEdNameCol_) + 1);
            }
            return true;

        case ClassicScreen::RhythmSongJobCopy:
            if (k == 7) {
                songCopyConfirm_ = false;
                screen_ = ClassicScreen::RhythmSongJobSelect;
            } else if (k == 6) {
                if (!rhythmSong(static_cast<std::size_t>(songCopySrc_)).hasData()) {
                    setNotice("Source Song Empty!");
                    return true;
                }
                songCopyConfirm_ = true;
            } else if (k <= 2) {
                songCopyCursor_ = 0;
            } else {
                songCopyCursor_ = 1;
            }
            return true;

        case ClassicScreen::RhythmSongJobClear:
            if (k == 7) {
                if (songClearSub_ != 0) {
                    rhySongClearConfirm_ = false;
                    songClearSub_ = 0;
                } else {
                    screen_ = ClassicScreen::RhythmSongJobSelect;
                }
                return true;
            }
            if (songClearSub_ == 0) {
                if (k <= 1)      { songClearSub_ = 1; songClearTarget_ = activeRhythmSong_; }
                else if (k <= 3) { songClearSub_ = 2; rhySongClearConfirm_ = true; }
            } else if (songClearSub_ == 1 && k == 6) {
                rhySongClearConfirm_ = true;
            }
            return true;

        case ClassicScreen::RhythmSongJobSearch: {
            if (k == 7) {
                screen_ = ClassicScreen::RhythmSongJobSelect;
                return true;
            }
            std::array<char, 7> marks{};
            std::array<uint16_t, 7> parts{};
            const int mCnt = collectSongMarks(rhythmSong(static_cast<std::size_t>(activeRhythmSong_)), marks, parts);
            if (k < mCnt) {
                rhySongStartPart_ = static_cast<int>(parts[static_cast<std::size_t>(k)]);
                // Compute measure offset up to target part
                const auto& song = rhythmSong(static_cast<std::size_t>(activeRhythmSong_));
                int meas = 1;
                for (int p = 0; p < rhySongStartPart_ && p < static_cast<int>(song.partCount); ++p) {
                    const auto& pt = song.parts[static_cast<std::size_t>(p)];
                    if (pt.type == vf50::generated::RhythmSongPartType::Pattern) {
                        meas += static_cast<int>(patternBars(pt.ptnIsUser, pt.ptnIndex));
                    }
                }
                rhySongStartMeas_ = meas;
                screen_ = ClassicScreen::RhythmSongPlay;
            }
            return true;
        }

        default:
            return false;
    }
}

bool VF50ClassicController::handleRhythmStepValue(int deltaSteps) noexcept {
    const int sign = (deltaSteps > 0) ? 1 : -1;

    switch (screen_) {
        case ClassicScreen::RhythmPtnPlay:
            if (rhyPtnCursor_ == 0) {
                if (processor_.isRhythmEnginePlaying()) {
                    const int baseIdx = rhyQueuedPtnActive_ ? rhyQueuedIdx_ : rhyPtnIdx_;
                    rhyQueuedPtnActive_ = true;
                    rhyQueuedIsUser_ = rhyPtnIsUser_;
                    rhyQueuedIdx_ = std::clamp(baseIdx + deltaSteps, 0, 99);
                    rhyQueuedStartWrap_ = processor_.rhythmPatternWrapCount();
                    if (rhyQueuedIsUser_) {
                        processor_.requestClassicRhythmQueueUser(static_cast<std::size_t>(rhyQueuedIdx_),
                                                                 processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(rhyQueuedIdx_)));
                    } else {
                        processor_.requestClassicRhythmQueueFactory(static_cast<std::size_t>(rhyQueuedIdx_));
                    }
                } else {
                    rhyPtnIdx_ = std::clamp(rhyPtnIdx_ + deltaSteps, 0, 99);
                }
            } else if (rhyPtnCursor_ == 1) {
                rhyTempoBpm_ = std::clamp(rhyTempoBpm_ + deltaSteps, 30, 240);
                processor_.requestClassicRhythmSetTempo(static_cast<double>(rhyTempoBpm_));
            } else {
                rhyKbdIsRhy_ = (sign > 0);
                processor_.setClassicRhythmKbdOverride(rhyKbdIsRhy_ ? 1 : 0);
            }
            return true;

        case ClassicScreen::RhythmSongPlay: {
            const bool anySongPlaying = processor_.isDemoPlaying() || processor_.isRhythmSongPlaying();
            if (rhySongCursor_ == 0 && !anySongPlaying) {
                activeRhythmSong_ = std::clamp(activeRhythmSong_ + sign, 0, 7);
                rhySongStartMeas_ = 1;
                rhySongStartPart_ = 0;
            } else if (rhySongCursor_ == 1 && !anySongPlaying) {
                rhySongStartMeas_ = std::clamp(rhySongStartMeas_ + deltaSteps, 1, 999);
            } else if (rhySongCursor_ == 2 && !anySongPlaying) {
                const int maxP = std::max(0, static_cast<int>(rhythmSong(static_cast<std::size_t>(activeRhythmSong_)).partCount) - 1);
                rhySongStartPart_ = std::clamp(rhySongStartPart_ + deltaSteps, 0, maxP);
            } else if (rhySongCursor_ == 3) {
                rhyTempoBpm_ = std::clamp(rhyTempoBpm_ + deltaSteps, 30, 240);
                processor_.requestClassicRhythmSetTempo(static_cast<double>(rhyTempoBpm_));
            } else if (rhySongCursor_ == 4) {
                rhyKbdIsRhy_ = (sign > 0);
                processor_.setClassicRhythmKbdOverride(rhyKbdIsRhy_ ? 1 : 0);
            }
            return true;
        }

        case ClassicScreen::RhythmPtnRecSetup: {
            const bool existingLocked = patternHasData(recPtnIsUser_, recPtnIdx_);
            if (recSetupCursor_ == 0) {
                recIsStep_ = (sign > 0);
            } else if (recSetupCursor_ == 1) {
                recPtnIdx_ = std::clamp(recPtnIdx_ + deltaSteps, 0, 99);
                if (patternHasData(recPtnIsUser_, recPtnIdx_)) {
                    recTimeSigIdx_ = findTimeSigIndex(patternTimeSigNum(recPtnIsUser_, recPtnIdx_),
                                                      patternTimeSigDen(recPtnIsUser_, recPtnIdx_));
                    recBars_ = std::clamp<int>(patternBars(recPtnIsUser_, recPtnIdx_), 1, 4);
                }
            } else if (recSetupCursor_ == 2) {
                if (existingLocked) {
                    setNotice("Pattern Data Locked");
                    return false;
                }
                recTimeSigIdx_ = std::clamp(recTimeSigIdx_ + sign, 0, static_cast<int>(kTimeSigTable.size()) - 1);
            } else if (recSetupCursor_ == 3) {
                if (existingLocked) {
                    setNotice("Pattern Data Locked");
                    return false;
                }
                recBars_ = std::clamp(recBars_ + sign, 1, 4);
            } else if (recSetupCursor_ == 4) {
                rhyTempoBpm_ = std::clamp(rhyTempoBpm_ + deltaSteps, 30, 240);
                processor_.requestClassicRhythmSetTempo(static_cast<double>(rhyTempoBpm_));
            } else {
                recQntzIdx_ = std::clamp(recQntzIdx_ + sign, 0, 7);
            }
            return true;
        }

        case ClassicScreen::RhythmPtnRecReal:
            if (recSetupCursor_ == 5) {
                // Spec §3.2 (p. 62): mid-playback quantize change blinks until next repetition
                const int baseQ = recQntzPendingBlink_ ? recPendingQntzIdx_ : recQntzIdx_;
                const int nextQ = std::clamp(baseQ + sign, 0, 7);
                if (processor_.rhythmCurrentTick() > 0u) {
                    recPendingQntzIdx_ = nextQ;
                    recQntzPendingBlink_ = true;
                    recQntzWrapAnchor_ = processor_.rhythmPatternWrapCount();
                } else {
                    recQntzIdx_ = nextQ;
                }
            } else {
                rhyTempoBpm_ = std::clamp(rhyTempoBpm_ + deltaSteps, 30, 240);
                processor_.requestClassicRhythmSetTempo(static_cast<double>(rhyTempoBpm_));
            }
            return true;

        case ClassicScreen::RhythmPtnRecStep:
            if (stepCursorField_ == 0) {
                const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
                recQntzIdx_ = std::clamp(recQntzIdx_ + sign, 0, 7);
                if ((stepAbsTick_ % tpb) != 0u) {
                    stepQntzBlink_ = true;
                }
            } else if (stepCursorField_ == 1) {
                stepSelectedDrum_ = std::clamp(stepSelectedDrum_ + sign, 0, 60);
            } else if (stepCursorField_ == 2) {
                if (auto* ev = recWorkPattern_.findMutableEventAtTick(stepAbsTick_, static_cast<uint8_t>(stepSelectedDrum_))) {
                    const int curAcc = std::clamp(static_cast<int>(ev->velocityLevel) - 1, 0, 7);
                    const int nextAcc = std::clamp(curAcc + sign, 0, 7);
                    ev->velocityLevel = static_cast<uint8_t>(nextAcc + 1);
                    recModified_ = true;
                }
            }
            return true;

        case ClassicScreen::RhythmPtnRecStore:
            recStoreSlot_ = std::clamp(recStoreSlot_ + deltaSteps, 0, 99);
            return true;

        case ClassicScreen::RhythmPtnJobCopy:
            if (rhyCopyCursor_ == 0) {
                rhyCopySrc1Idx_ = std::clamp(rhyCopySrc1Idx_ + deltaSteps, 0, 99);
            } else if (rhyCopyCursor_ == 1) {
                if (!rhyCopyHasSrc2_ && sign > 0) {
                    rhyCopyHasSrc2_ = true;
                    rhyCopySrc2Idx_ = 0;
                } else if (rhyCopyHasSrc2_ && rhyCopySrc2Idx_ == 0 && sign < 0) {
                    rhyCopyHasSrc2_ = false;
                } else if (rhyCopyHasSrc2_) {
                    rhyCopySrc2Idx_ = std::clamp(rhyCopySrc2Idx_ + deltaSteps, 0, 99);
                }
            } else {
                rhyCopyDstIdx_ = std::clamp(rhyCopyDstIdx_ + deltaSteps, 0, 99);
            }
            return true;

        case ClassicScreen::RhythmPtnJobClear:
            if (rhyClearSubMode_ == 1) {
                rhyClearTargetIdx_ = std::clamp(rhyClearTargetIdx_ + deltaSteps, 0, 99);
            } else if (rhyClearSubMode_ == 2) {
                if (rhyClearCursor_ == 0) rhyClearTargetIdx_ = std::clamp(rhyClearTargetIdx_ + deltaSteps, 0, 99);
                else                      rhyClearInstIdx_   = std::clamp(rhyClearInstIdx_ + sign, 0, 60);
            }
            return true;

        case ClassicScreen::RhythmSetup: {
            if (rhySetupCursor_ == 0) {
                rhyRch_ = std::clamp(rhyRch_ + sign, 0, 16);
                vf50::params::setApvtsRawValue(processor_.apvts(),
                                              vf50::params::ParamIndex::RhythmReceiveCh,
                                              static_cast<float>(rhyRch_));
            } else if (rhySetupCursor_ == 1) {
                rhyTch_ = std::clamp(rhyTch_ + sign, 0, 16);
            } else if (rhySetupCursor_ == 2) {
                rhyVelOn_ = (sign > 0);
            } else if (rhySetupCursor_ == 3) {
                rhyClickPlay_ = (sign > 0);
                updateRhythmMetronomeForMode();
            } else if (rhySetupCursor_ == 4) {
                rhyBeatIdx_ = std::clamp(rhyBeatIdx_ + sign, 0, 5);
                updateRhythmMetronomeForMode();
            } else {
                seqSyncMidi_ = (sign > 0); // Shared with Sequencer Setup Sync!
                processor_.setClassicRhythmSyncMidi(seqSyncMidi_);
            }
            return true;
        }

        case ClassicScreen::RhythmInstSet: {
            auto& kit = processor_.hostMemory().mutableActiveRhythmKit();
            auto& inst = kit.instruments[static_cast<std::size_t>(std::clamp(rhyInstSelected_, 0, 60))];
            if (rhyInstCursor_ == 0) {
                rhyInstSelected_ = std::clamp(rhyInstSelected_ + sign, 0, 60);
            } else if (rhyInstCursor_ == 1) {
                inst.volume = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.volume) + sign, 0, 15));
                commitRhythmKitAndSync();
            } else if (rhyInstCursor_ == 2) {
                inst.pan = static_cast<int8_t>(std::clamp(static_cast<int>(inst.pan) + sign, -3, 3));
                commitRhythmKitAndSync();
            } else if (rhyInstCursor_ == 3) {
                inst.midiNote = static_cast<uint8_t>(std::clamp(static_cast<int>(inst.midiNote) + sign, 0, 127));
                commitRhythmKitAndSync();
            } else if (rhyInstCursor_ == 4) {
                kit.rhythmVolume = static_cast<uint8_t>(std::clamp(static_cast<int>(kit.rhythmVolume) + deltaSteps, 0, 99));
                commitRhythmKitAndSync();
            } else {
                const int m = std::clamp(static_cast<int>(kit.effectMode) + sign, 0, 2);
                kit.effectMode = static_cast<vf50::dsp::drums::RhythmEffectMode>(m);
                commitRhythmKitAndSync();
            }
            return true;
        }

        case ClassicScreen::RhythmKeyAssign:
            if (rhyAsgnCursor_ == 0) {
                rhyAsgnMapIdx_ = std::clamp(rhyAsgnMapIdx_ + sign, 0, 4);
                pushActiveRhythmKeyMapToAudio();
            } else if (rhyAsgnCursor_ == 1) {
                rhyAsgnKeyIdx_ = std::clamp(rhyAsgnKeyIdx_ + sign, 0, 60);
            } else {
                if (rhyAsgnMapIdx_ < 3) {
                    setNotice("Preset Map Read-Only");
                    return false;
                }
                const int uIdx = std::clamp(rhyAsgnMapIdx_ - 3, 0, 1);
                auto map = processor_.hostMemory().userKeyMap(static_cast<std::size_t>(uIdx));
                const int cur = static_cast<int>(map[static_cast<std::size_t>(rhyAsgnKeyIdx_)]);
                map[static_cast<std::size_t>(rhyAsgnKeyIdx_)] = static_cast<int8_t>(std::clamp(cur + sign, -1, 60));
                processor_.hostMemory().setUserKeyMap(static_cast<std::size_t>(uIdx), map);
                pushActiveRhythmKeyMapToAudio();
            }
            return true;

        case ClassicScreen::RhythmSongEdit:
            if (rhythmSongs_ != nullptr) {
                auto& song = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)];
                if (songEditPartIdx_ >= static_cast<int>(song.partCount)
                    && song.partCount < vf50::generated::RhythmSongData::kMaxParts) {
                    song.parts[song.partCount++] = vf50::generated::RhythmSongPart{};
                }
                if (songEditPartIdx_ < static_cast<int>(song.partCount)) {
                    auto& pt = song.parts[static_cast<std::size_t>(songEditPartIdx_)];
                    using T = vf50::generated::RhythmSongPartType;
                    switch (pt.type) {
                        case T::Pattern:
                            pt.ptnIndex = static_cast<uint8_t>(std::clamp(static_cast<int>(pt.ptnIndex) + deltaSteps, 0, 99));
                            break;
                        case T::RepeatBegin:
                            break;
                        case T::RepeatEnd:
                            pt.repeatCount = static_cast<uint8_t>(std::clamp(static_cast<int>(pt.repeatCount) + sign, 2, 99));
                            break;
                        case T::Volume: {
                            const int rVol = static_cast<int>(processor_.hostMemory().activeRhythmKit().rhythmVolume);
                            pt.volDelta = static_cast<int8_t>(std::clamp(static_cast<int>(pt.volDelta) + sign, -rVol, rVol));
                            break;
                        }
                        case T::Tempo:
                            if (songEditTempoSub_ == 0) {
                                pt.tempoDelta = static_cast<int16_t>(std::clamp(static_cast<int>(pt.tempoDelta) + deltaSteps, -210, 210));
                            } else {
                                pt.tempoBeats = static_cast<uint8_t>(std::clamp(static_cast<int>(pt.tempoBeats) + sign, 0, 32));
                            }
                            break;
                        case T::Mark: {
                            int ch = std::clamp(static_cast<int>(pt.markChar) + sign, static_cast<int>('A'), static_cast<int>('Z'));
                            pt.markChar = static_cast<char>(ch);
                            break;
                        }
                    }
                }
            }
            return true;

        case ClassicScreen::RhythmSongEditJob:
            if (songEdJobSub_ == 1) {
                const int maxP = std::max(1, static_cast<int>(rhythmSong(static_cast<std::size_t>(activeRhythmSong_)).partCount) + 1);
                songEdJumpTarget_ = std::clamp(songEdJumpTarget_ + deltaSteps, 1, maxP);
            } else if (songEdJobSub_ == 4) {
                if (songEdCopyCursor_ == 0) {
                    songEdCopyFrom_ = std::clamp(songEdCopyFrom_ + deltaSteps, 1, 999);
                    songEdCopyTo_ = std::max(songEdCopyTo_, songEdCopyFrom_);
                } else if (songEdCopyCursor_ == 1) {
                    songEdCopyTo_ = std::clamp(songEdCopyTo_ + deltaSteps, songEdCopyFrom_, 999);
                } else {
                    songEdCopyDst_ = std::clamp(songEdCopyDst_ + deltaSteps, 1, 999);
                }
            } else if (songEdJobSub_ == 6 && rhythmSongs_ != nullptr) {
                auto& nm = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)].name;
                const int col = std::clamp(songEdNameCol_, 0, 7);
                int cur = static_cast<unsigned char>(nm[static_cast<std::size_t>(col)]);
                nm[static_cast<std::size_t>(col)] = static_cast<char>(std::clamp(cur + sign, 0x20, 0x7A));
            }
            return true;

        case ClassicScreen::RhythmSongJobCopy:
            if (songCopyCursor_ == 0) songCopySrc_ = std::clamp(songCopySrc_ + sign, 0, 7);
            else                      songCopyDst_ = std::clamp(songCopyDst_ + sign, 0, 7);
            return true;

        case ClassicScreen::RhythmSongJobClear:
            if (songClearSub_ == 1) {
                songClearTarget_ = std::clamp(songClearTarget_ + sign, 0, 7);
            }
            return true;

        default:
            return false;
    }
}

bool VF50ClassicController::handleRhythmDigit(int digit0To9) noexcept {
    const int d = std::clamp(digit0To9, 0, 9);

    if (screen_ == ClassicScreen::RhythmPtnPlay && rhyPtnCursor_ == 0) {
        if (rhyPtnPendingTens_ < 0) {
            rhyPtnPendingTens_ = d;
            return true;
        }
        const int num = rhyPtnPendingTens_ * 10 + d;
        rhyPtnPendingTens_ = -1;
        if (processor_.isRhythmEnginePlaying()) {
            rhyQueuedPtnActive_ = true;
            if (rhyPtnPendingBank_ >= 0) {
                rhyQueuedIsUser_ = (rhyPtnPendingBank_ == 0);
            } else {
                rhyQueuedIsUser_ = rhyPtnIsUser_;
            }
            rhyQueuedIdx_ = num;
            rhyQueuedStartWrap_ = processor_.rhythmPatternWrapCount();
            if (rhyQueuedIsUser_) {
                processor_.requestClassicRhythmQueueUser(static_cast<std::size_t>(num),
                                                         processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(num)));
            } else {
                processor_.requestClassicRhythmQueueFactory(static_cast<std::size_t>(num));
            }
        } else {
            if (rhyPtnPendingBank_ >= 0) {
                rhyPtnIsUser_ = (rhyPtnPendingBank_ == 0);
            }
            rhyPtnIdx_ = num;
        }
        rhyPtnPendingBank_ = -1;
        return true;
    }

    if (screen_ == ClassicScreen::RhythmSongPlay && rhySongCursor_ == 0 && d >= 1 && d <= 8
        && !processor_.isDemoPlaying() && !processor_.isRhythmSongPlaying()) {
        activeRhythmSong_ = d - 1;
        rhySongStartMeas_ = 1;
        rhySongStartPart_ = 0;
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecSetup && recSetupCursor_ == 1) {
        if (rhyPtnPendingTens_ < 0) {
            rhyPtnPendingTens_ = d;
        } else {
            recPtnIdx_ = rhyPtnPendingTens_ * 10 + d;
            rhyPtnPendingTens_ = -1;
            if (patternHasData(recPtnIsUser_, recPtnIdx_)) {
                recTimeSigIdx_ = findTimeSigIndex(patternTimeSigNum(recPtnIsUser_, recPtnIdx_),
                                                  patternTimeSigDen(recPtnIsUser_, recPtnIdx_));
                recBars_ = std::clamp<int>(patternBars(recPtnIsUser_, recPtnIdx_), 1, 4);
            }
        }
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecStep) {
        // Spec §3.2 (p. 64): numeric keys [3]=1/4, [4]=1/8, [5]=1/16, [6]=1/32, [9]=triplet, [0]=off
        int nextQ = -1;
        if (d == 3)      nextQ = 0; // 1/4
        else if (d == 4) nextQ = 2; // 1/8
        else if (d == 5) nextQ = 4; // 1/16
        else if (d == 6) nextQ = 6; // 1/32
        else if (d == 0) nextQ = 7; // off (1/6th of 32nd)
        else if (d == 9) {
            if (recQntzIdx_ == 1)      nextQ = 3; // 1/6 -> 1/12
            else if (recQntzIdx_ == 3) nextQ = 5; // 1/12 -> 1/24
            else if (recQntzIdx_ == 5) nextQ = 1; // 1/24 -> 1/6
            else                       nextQ = stepLastTripletIdx_;
            stepLastTripletIdx_ = nextQ;
        }
        if (nextQ >= 0) {
            const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
            recQntzIdx_ = nextQ;
            if ((stepAbsTick_ % tpb) != 0u) {
                stepQntzBlink_ = true;
            }
            return true;
        }
        return false;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecStore) {
        if (recStorePendingTens_ < 0) {
            recStorePendingTens_ = d;
        } else {
            recStoreSlot_ = recStorePendingTens_ * 10 + d;
            recStorePendingTens_ = -1;
        }
        return true;
    }

    if (screen_ == ClassicScreen::RhythmSongEdit && rhythmSongs_ != nullptr) {
        auto& song = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)];
        if (songEditPartIdx_ >= static_cast<int>(song.partCount)
            && song.partCount < vf50::generated::RhythmSongData::kMaxParts) {
            song.parts[song.partCount++] = vf50::generated::RhythmSongPart{};
        }
        if (songEditPartIdx_ < static_cast<int>(song.partCount)) {
            auto& pt = song.parts[static_cast<std::size_t>(songEditPartIdx_)];
            if (pt.type == vf50::generated::RhythmSongPartType::Pattern) {
                if (songEditPendingTens_ < 0) {
                    songEditPendingTens_ = d;
                } else {
                    pt.ptnIndex = static_cast<uint8_t>(songEditPendingTens_ * 10 + d);
                    songEditPendingTens_ = -1;
                }
                return true;
            }
        }
    }

    return false;
}

bool VF50ClassicController::handleRhythmYes() noexcept {
    if (screen_ == ClassicScreen::RhythmPtnRecStore && recStoreConfirm_) {
        commitWorkPatternToSlot(recStoreSlot_);
        recStoreConfirm_ = false;
        rhyPtnIsUser_ = true;
        rhyPtnIdx_ = recStoreSlot_;
        setNotice("Completed !", 45);
        screen_ = ClassicScreen::RhythmPtnPlay;
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnJobCopy && rhyCopyConfirm_) {
        rhyCopyConfirm_ = false;
        vf50::state::UserRhythmPattern dst{};
        dst.clear();

        auto appendPattern = [&](bool isUser, int idx, uint16_t tickOffset) {
            if (isUser) {
                const auto& src = processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(idx));
                if (tickOffset == 0u) {
                    dst.timeSigNum = src.timeSigNum;
                    dst.timeSigDen = src.timeSigDen;
                    dst.bars = src.bars;
                    dst.totalTicks = src.totalTicks;
                } else {
                    dst.bars = static_cast<uint8_t>(std::min<int>(4, static_cast<int>(dst.bars) + static_cast<int>(src.bars)));
                    dst.totalTicks = static_cast<uint16_t>(dst.totalTicks + src.totalTicks);
                }
                for (uint16_t i = 0u; i < src.eventCount; ++i) {
                    const auto& ev = src.events[i];
                    (void)dst.addEventSorted(static_cast<uint16_t>(ev.tick + tickOffset), ev.drumIndex, ev.velocityLevel);
                }
            } else {
                const auto& rom = processor_.activeRomBank();
                const auto& fPtn = rom.factoryRhythmPatterns[static_cast<std::size_t>(idx)];
                const auto evs = rom.getRhythmPatternEvents(static_cast<std::size_t>(idx));
                if (tickOffset == 0u) {
                    dst.timeSigNum = fPtn.timeSigNum;
                    dst.timeSigDen = fPtn.timeSigDen;
                    dst.bars = fPtn.bars;
                    dst.totalTicks = fPtn.totalTicks;
                } else {
                    dst.bars = static_cast<uint8_t>(std::min<int>(4, static_cast<int>(dst.bars) + static_cast<int>(fPtn.bars)));
                    dst.totalTicks = static_cast<uint16_t>(dst.totalTicks + fPtn.totalTicks);
                }
                for (const auto& ev : evs) {
                    (void)dst.addEventSorted(static_cast<uint16_t>(ev.tick + tickOffset), ev.drumIndex, ev.velocityLevel);
                }
            }
        };

        appendPattern(rhyCopySrc1IsUser_, rhyCopySrc1Idx_, 0u);
        if (rhyCopyHasSrc2_) {
            appendPattern(rhyCopySrc2IsUser_, rhyCopySrc2Idx_, dst.totalTicks);
        }

        processor_.hostMemory().mutableUserRhythmPattern(static_cast<std::size_t>(rhyCopyDstIdx_)) = dst;
        processor_.requestClassicRhythmCommitUserPattern(static_cast<std::size_t>(rhyCopyDstIdx_), dst);
        setNotice("Completed !", 45);
        return true;
    }

    if (screen_ == ClassicScreen::RhythmPtnJobClear && rhyClearConfirm_) {
        rhyClearConfirm_ = false;
        if (rhyClearSubMode_ == 1) {
            auto& ptn = processor_.hostMemory().mutableUserRhythmPattern(static_cast<std::size_t>(rhyClearTargetIdx_));
            ptn.clear();
            processor_.requestClassicRhythmCommitUserPattern(static_cast<std::size_t>(rhyClearTargetIdx_), ptn);
        } else if (rhyClearSubMode_ == 2) {
            auto& ptn = processor_.hostMemory().mutableUserRhythmPattern(static_cast<std::size_t>(rhyClearTargetIdx_));
            (void)ptn.eraseInstrument(static_cast<uint8_t>(std::clamp(rhyClearInstIdx_, 0, 60)));
            processor_.requestClassicRhythmCommitUserPattern(static_cast<std::size_t>(rhyClearTargetIdx_), ptn);
        } else if (rhyClearSubMode_ == 3) {
            for (std::size_t i = 0u; i < 100u; ++i) {
                auto& ptn = processor_.hostMemory().mutableUserRhythmPattern(i);
                ptn.clear();
                processor_.requestClassicRhythmCommitUserPattern(i, ptn);
            }
        }
        rhyClearSubMode_ = 0;
        setNotice("Completed !", 45);
        return true;
    }

    if (screen_ == ClassicScreen::RhythmSongEditJob && rhythmSongs_ != nullptr) {
        auto& song = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)];
        if (songEdJobSub_ == 2) { // >Ins
            if (song.partCount < vf50::generated::RhythmSongData::kMaxParts) {
                const int pos = std::clamp(songEditPartIdx_, 0, static_cast<int>(song.partCount));
                for (int i = static_cast<int>(song.partCount); i > pos; --i) {
                    song.parts[static_cast<std::size_t>(i)] = song.parts[static_cast<std::size_t>(i - 1)];
                }
                song.parts[static_cast<std::size_t>(pos)] = vf50::generated::RhythmSongPart{};
                ++song.partCount;
                setNotice("Completed !", 45);
            }
            songEdJobSub_ = 0;
            return true;
        }
        if (songEdJobSub_ == 3) { // >Del
            if (songEditPartIdx_ < static_cast<int>(song.partCount)) {
                for (int i = songEditPartIdx_; i + 1 < static_cast<int>(song.partCount); ++i) {
                    song.parts[static_cast<std::size_t>(i)] = song.parts[static_cast<std::size_t>(i + 1)];
                }
                --song.partCount;
                song.parts[song.partCount] = vf50::generated::RhythmSongPart{};
                if (songEditPartIdx_ > static_cast<int>(song.partCount)) {
                    songEditPartIdx_ = static_cast<int>(song.partCount);
                }
                setNotice("Completed !", 45);
            }
            songEdJobSub_ = 0;
            return true;
        }
        if (songEdJobSub_ == 4 && songEdCopyConfirm_) { // >Copy
            songEdCopyConfirm_ = false;
            const int fromIdx = std::clamp(songEdCopyFrom_ - 1, 0, static_cast<int>(song.partCount) - 1);
            const int toIdx = std::clamp(songEdCopyTo_ - 1, fromIdx, static_cast<int>(song.partCount) - 1);
            const int count = (song.partCount > 0u) ? (toIdx - fromIdx + 1) : 0;
            if (count > 0 && static_cast<int>(song.partCount) + count <= static_cast<int>(vf50::generated::RhythmSongData::kMaxParts)) {
                std::vector<vf50::generated::RhythmSongPart> tmp;
                tmp.reserve(static_cast<std::size_t>(count));
                for (int i = fromIdx; i <= toIdx; ++i) {
                    tmp.push_back(song.parts[static_cast<std::size_t>(i)]);
                }
                const int dstIdx = std::clamp(songEdCopyDst_ - 1, 0, static_cast<int>(song.partCount));
                for (int i = static_cast<int>(song.partCount) - 1; i >= dstIdx; --i) {
                    song.parts[static_cast<std::size_t>(i + count)] = song.parts[static_cast<std::size_t>(i)];
                }
                for (int i = 0; i < count; ++i) {
                    song.parts[static_cast<std::size_t>(dstIdx + i)] = tmp[static_cast<std::size_t>(i)];
                }
                song.partCount = static_cast<uint16_t>(song.partCount + count);
                setNotice("Completed !", 45);
            }
            songEdJobSub_ = 0;
            return true;
        }
    }

    if (screen_ == ClassicScreen::RhythmSongJobCopy && songCopyConfirm_ && rhythmSongs_ != nullptr) {
        songCopyConfirm_ = false;
        (*rhythmSongs_)[static_cast<std::size_t>(songCopyDst_)] = (*rhythmSongs_)[static_cast<std::size_t>(songCopySrc_)];
        setNotice("Completed !", 45);
        return true;
    }

    if (screen_ == ClassicScreen::RhythmSongJobClear && rhySongClearConfirm_ && rhythmSongs_ != nullptr) {
        rhySongClearConfirm_ = false;
        if (songClearSub_ == 1) {
            (*rhythmSongs_)[static_cast<std::size_t>(songClearTarget_)].clear();
        } else if (songClearSub_ == 2) {
            for (auto& s : *rhythmSongs_) {
                s.clear();
            }
        }
        songClearSub_ = 0;
        setNotice("Completed !", 45);
        return true;
    }

    return false;
}

bool VF50ClassicController::handleRhythmNo() noexcept {
    if (screen_ == ClassicScreen::RhythmPtnRecStore && recStoreConfirm_) {
        recStoreConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmPtnJobCopy && rhyCopyConfirm_) {
        rhyCopyConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmPtnJobClear && rhyClearConfirm_) {
        rhyClearConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEditJob && (songEdJobSub_ == 2 || songEdJobSub_ == 3 || songEdCopyConfirm_)) {
        songEdCopyConfirm_ = false;
        if (songEdJobSub_ == 2 || songEdJobSub_ == 3) songEdJobSub_ = 0;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongJobCopy && songCopyConfirm_) {
        songCopyConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongJobClear && rhySongClearConfirm_) {
        rhySongClearConfirm_ = false;
        return true;
    }
    return false;
}

bool VF50ClassicController::handleRhythmTextChar(char asciiChar) noexcept {
    if (screen_ == ClassicScreen::RhythmSongEditJob && songEdJobSub_ == 6 && rhythmSongs_ != nullptr) {
        auto& nm = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)].name;
        const int col = std::clamp(songEdNameCol_, 0, 7);
        nm[static_cast<std::size_t>(col)] = asciiChar;
        songEdNameCol_ = std::min(8, col + 1);
        processor_.publishRhythmSongsToAudio();
        return true;
    }
    if (screen_ == ClassicScreen::RhythmSongEdit && rhythmSongs_ != nullptr) {
        auto& song = (*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)];
        if (songEditPartIdx_ < static_cast<int>(song.partCount)) {
            auto& pt = song.parts[static_cast<std::size_t>(songEditPartIdx_)];
            if (pt.type == vf50::generated::RhythmSongPartType::Mark && asciiChar >= 'A' && asciiChar <= 'z') {
                pt.markChar = static_cast<char>(std::toupper(static_cast<unsigned char>(asciiChar)));
                return true;
            }
        }
    }
    return false;
}

void VF50ClassicController::handleRhythmDataEntry(float norm0To1) noexcept {
    const float c = std::clamp(norm0To1, 0.0f, 1.0f);
    if (screen_ == ClassicScreen::RhythmPtnPlay) {
        if (rhyPtnCursor_ == 0) {
            const int target = static_cast<int>(std::lround(c * 99.0f));
            if (processor_.isRhythmEnginePlaying()) {
                rhyQueuedPtnActive_ = true;
                rhyQueuedIsUser_ = rhyPtnIsUser_;
                rhyQueuedIdx_ = target;
                rhyQueuedStartWrap_ = processor_.rhythmPatternWrapCount();
                if (rhyQueuedIsUser_) {
                    processor_.requestClassicRhythmQueueUser(static_cast<std::size_t>(target),
                                                             processor_.hostMemory().userRhythmPattern(static_cast<std::size_t>(target)));
                } else {
                    processor_.requestClassicRhythmQueueFactory(static_cast<std::size_t>(target));
                }
            } else {
                rhyPtnIdx_ = target;
            }
        } else if (rhyPtnCursor_ == 1) {
            setRhythmTempoNormalized(c);
        } else {
            rhyKbdIsRhy_ = (c >= 0.5f);
            processor_.setClassicRhythmKbdOverride(rhyKbdIsRhy_ ? 1 : 0);
        }
    } else if (screen_ == ClassicScreen::RhythmSongPlay) {
        if (rhySongCursor_ == 0 && !processor_.isDemoPlaying() && !processor_.isRhythmSongPlaying()) {
            activeRhythmSong_ = static_cast<int>(std::lround(c * 7.0f));
        } else if (rhySongCursor_ == 3) {
            setRhythmTempoNormalized(c);
        } else if (rhySongCursor_ == 4) {
            rhyKbdIsRhy_ = (c >= 0.5f);
            processor_.setClassicRhythmKbdOverride(rhyKbdIsRhy_ ? 1 : 0);
        }
    } else if (screen_ == ClassicScreen::RhythmInstSet) {
        auto& kit = processor_.hostMemory().mutableActiveRhythmKit();
        auto& inst = kit.instruments[static_cast<std::size_t>(std::clamp(rhyInstSelected_, 0, 60))];
        if (rhyInstCursor_ == 0) {
            rhyInstSelected_ = static_cast<int>(std::lround(c * 60.0f));
        } else if (rhyInstCursor_ == 1) {
            inst.volume = static_cast<uint8_t>(std::lround(c * 15.0f));
            commitRhythmKitAndSync();
        } else if (rhyInstCursor_ == 2) {
            inst.pan = static_cast<int8_t>(static_cast<int>(std::lround(c * 6.0f)) - 3);
            commitRhythmKitAndSync();
        } else if (rhyInstCursor_ == 3) {
            inst.midiNote = static_cast<uint8_t>(std::lround(c * 127.0f));
            commitRhythmKitAndSync();
        } else if (rhyInstCursor_ == 4) {
            kit.rhythmVolume = static_cast<uint8_t>(std::lround(c * 99.0f));
            commitRhythmKitAndSync();
        } else {
            kit.effectMode = static_cast<vf50::dsp::drums::RhythmEffectMode>(std::lround(c * 2.0f));
            commitRhythmKitAndSync();
        }
    }
}

bool VF50ClassicController::renderRhythmLcdFrame(ClassicLcdFrame& frame) const noexcept {
    if (!isRhythmScreen(screen_)) {
        return false;
    }

    char tmp[80]{};
    const bool blinkHide = ((rhyBlinkCounter_ & 4u) != 0u);

    switch (screen_) {
        case ClassicScreen::RhythmPtnPlay: {
            // Spec §3.1 (pp. 58-59):
            // "PTN PLAY> ▶Pattern  Time  Bar>Tempo>KBD "
            // ">toSONG     P10w    4/4    1   120  RHY "
            // Strictly hide >toSONG whenever ANY playback (Sequencer or Rhythm) is running!
            const bool playing = processor_.isDemoPlaying()
                              || processor_.isRhythmEnginePlaying()
                              || processor_.isRhythmSongPlaying();
            const char pCur = (rhyPtnCursor_ == 0) ? kGlyphPlayArrow : ' ';
            const char tCur = (rhyPtnCursor_ == 1) ? kGlyphPlayArrow : '>';
            const char kCur = (rhyPtnCursor_ == 2) ? kGlyphPlayArrow : '>';
            std::snprintf(tmp, sizeof(tmp), "PTN PLAY> %cPattern  Time  Bar%cTempo%cKBD ", pCur, tCur, kCur);
            setLine40(frame.line0, tmp);

            const bool dispIsUser = rhyQueuedPtnActive_ ? rhyQueuedIsUser_ : rhyPtnIsUser_;
            const int dispIdx = rhyQueuedPtnActive_ ? rhyQueuedIdx_ : rhyPtnIdx_;
            const char bankC = (rhyPtnPendingBank_ >= 0)
                                   ? (rhyPtnPendingBank_ == 0 ? 'I' : 'P')
                                   : (dispIsUser ? 'I' : 'P');
            char d0 = static_cast<char>('0' + (dispIdx / 10));
            char d1 = static_cast<char>('0' + (dispIdx % 10));
            if (rhyPtnPendingTens_ >= 0) {
                d0 = static_cast<char>('0' + rhyPtnPendingTens_);
                d1 = '?';
            } else if (rhyPtnPendingBank_ >= 0) {
                d0 = '?';
                d1 = '?';
            }
            const char wChar = patternHasData(dispIsUser, dispIdx) ? 'w' : ' ';
            std::array<char, 5> ptnTag{ { bankC, d0, d1, wChar, '\0' } };
            if (rhyQueuedPtnActive_ && blinkHide) {
                ptnTag = { ' ', ' ', ' ', ' ', '\0' };
            }

            const uint8_t tsN = patternTimeSigNum(rhyPtnIsUser_, rhyPtnIdx_);
            const uint8_t tsD = patternTimeSigDen(rhyPtnIsUser_, rhyPtnIdx_);
            const uint8_t bars = patternBars(rhyPtnIsUser_, rhyPtnIdx_);
            char tsBuf[6]{};
            std::snprintf(tsBuf, sizeof(tsBuf), "%d/%d", static_cast<int>(tsN), static_cast<int>(tsD));
            const int liveBpm = processor_.isDemoPlaying()
                                    ? std::clamp(processor_.activeDemoTempoBpm(), 30, 240)
                                    : (playing ? std::clamp(processor_.rhythmLiveTempoBpm(), 30, 240) : rhyTempoBpm_);

            std::snprintf(tmp, sizeof(tmp), "%-7.7s     %-4.4s   %-5.5s  %2d   %3d  %s ",
                          playing ? "       " : ">toSONG",
                          ptnTag.data(),
                          tsBuf,
                          static_cast<int>(bars),
                          liveBpm,
                          rhyKbdIsRhy_ ? "RHY" : "SYN");
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmSongPlay: {
            // Spec §3.1 (p. 71):
            // "SONG PLAY)▶SONG   >Meas >Part>Tempo>KBD "
            // ">toPTN   1w:Moon   001   001   120  RHY "
            // Strictly hide >toPTN and track live master Meas/Part whenever playback is running!
            const bool seqPlaying     = processor_.isDemoPlaying();
            const bool rhySongPlaying = processor_.isRhythmSongPlaying();
            const bool playing        = seqPlaying || rhySongPlaying || processor_.isRhythmEnginePlaying();
            auto mk = [&](int idx, char def) -> char { return (rhySongCursor_ == idx) ? kGlyphPlayArrow : def; };
            std::snprintf(tmp, sizeof(tmp), "SONG PLAY)%cSONG   %cMeas %cPart%cTempo%cKBD ",
                          mk(0, ' '), mk(1, '>'), mk(2, '>'), mk(3, '>'), mk(4, '>'));
            setLine40(frame.line0, tmp);

            const auto& song = rhythmSong(static_cast<std::size_t>(activeRhythmSong_));
            const char wChar = song.hasData() ? 'w' : ' ';
            const int dispMeas = seqPlaying
                                     ? std::clamp(processor_.activeDemoMeasure(), 1, 999)
                                     : (rhySongPlaying ? std::clamp<int>(processor_.rhythmSongCurrentMeasure(), 1, 999)
                                                       : rhySongStartMeas_);
            const int dispPart = rhySongPlaying
                                     ? std::clamp<int>(processor_.rhythmSongCurrentPart(), 1, 999)
                                     : (rhySongStartPart_ + 1);
            const int liveBpm  = seqPlaying
                                     ? std::clamp(processor_.activeDemoTempoBpm(), 30, 240)
                                     : (playing ? std::clamp(processor_.rhythmLiveTempoBpm(), 30, 240) : rhyTempoBpm_);

            char leftCol[8]{ ">toPTN " };
            if (rhySongPlaying) {
                const bool u = processor_.rhythmSongCurrentPtnIsUser();
                const int pIdx = processor_.rhythmSongCurrentPtnIndex();
                std::snprintf(leftCol, sizeof(leftCol), "PTN %c%02d", u ? 'I' : 'P', pIdx);
            } else if (playing) {
                std::snprintf(leftCol, sizeof(leftCol), "       ");
            } else {
                std::snprintf(leftCol, sizeof(leftCol), ">toPTN ");
            }
            std::snprintf(tmp, sizeof(tmp), "%-7.7s %d%c:%-8.8s %03d   %03d   %3d  %s ",
                          leftCol,
                          activeRhythmSong_ + 1,
                          wChar,
                          song.name.data(),
                          dispMeas,
                          dispPart,
                          liveBpm,
                          rhyKbdIsRhy_ ? "RHY" : "SYN");
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmPtnRecSetup: {
            // Spec §3.2 (pp. 61-62):
            // "P.REC>>PTN >Time >Bar >Tempo >Qntz      "
            // "▶Real  I00  4/4    2   120   1/16  >EXIT"
            auto mk = [&](int idx) -> char { return (recSetupCursor_ == idx) ? kGlyphPlayArrow : '>'; };
            std::snprintf(tmp, sizeof(tmp), "P.REC>%cPTN %cTime %cBar %cTempo %cQntz      ",
                          mk(1), mk(2), mk(3), mk(4), mk(5));
            setLine40(frame.line0, tmp);

            const auto& ts = kTimeSigTable[static_cast<std::size_t>(std::clamp(recTimeSigIdx_, 0, 55))];
            char tsBuf[6]{};
            std::snprintf(tsBuf, sizeof(tsBuf), "%d/%d", static_cast<int>(ts.num), static_cast<int>(ts.den));
            const char* qStr = kQntzLabels[static_cast<std::size_t>(std::clamp(recQntzIdx_, 0, 7))];
            char ptnBuf[4]{
                recPtnIsUser_ ? 'I' : 'P',
                (rhyPtnPendingTens_ >= 0) ? static_cast<char>('0' + rhyPtnPendingTens_) : static_cast<char>('0' + (recPtnIdx_ / 10)),
                (rhyPtnPendingTens_ >= 0) ? '?' : static_cast<char>('0' + (recPtnIdx_ % 10)),
                '\0'
            };
            std::snprintf(tmp, sizeof(tmp), "%c%-4.4s  %.3s  %-5.5s  %d   %3d   %-4.4s  >EXIT",
                          (recSetupCursor_ == 0) ? kGlyphPlayArrow : ' ',
                          recIsStep_ ? "Step" : "Real",
                          ptnBuf,
                          tsBuf,
                          recBars_,
                          rhyTempoBpm_,
                          qStr);
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmPtnRecReal: {
            // Realtime Recording live view
            const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
            const int curBar = 1 + static_cast<int>(processor_.rhythmCurrentTick() / std::max<uint16_t>(tpb, 1u));
            std::snprintf(tmp, sizeof(tmp), "P.REC> Real %c%02d  Bar:%d/%d  %cTempo  %cQntz ",
                          recPtnIsUser_ ? 'I' : 'P', recPtnIdx_,
                          std::clamp(curBar, 1, 4), static_cast<int>(recWorkPattern_.bars),
                          (recSetupCursor_ == 4) ? kGlyphPlayArrow : '>',
                          (recSetupCursor_ == 5) ? kGlyphPlayArrow : '>');
            setLine40(frame.line0, tmp);

            const int dispQIdx = recQntzPendingBlink_ ? recPendingQntzIdx_ : recQntzIdx_;
            const char* qStr = (recQntzPendingBlink_ && blinkHide)
                                   ? "    "
                                   : kQntzLabels[static_cast<std::size_t>(std::clamp(dispQIdx, 0, 7))];
            std::snprintf(tmp, sizeof(tmp), "Events:%3u  [-]ERASE+Key   %3d   %-4.4s  ",
                          static_cast<unsigned>(recWorkPattern_.eventCount),
                          rhyTempoBpm_,
                          qStr);
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmPtnRecStep: {
            // Spec §3.2 (pp. 62-65):
            // "Bar1:q |--------+--------+--------+----|"
            // "ptnI00  ▶Qntz >BD 1     >Acc=7  <-   -> "
            const uint16_t tpb = computeTicksPerBar(recWorkPattern_.timeSigNum, recWorkPattern_.timeSigDen);
            const int barIdx = std::clamp(static_cast<int>(stepAbsTick_ / std::max<uint16_t>(tpb, 1u)), 0, 3);
            const uint16_t barStartTick = static_cast<uint16_t>(barIdx * tpb);
            const uint16_t tickInBar = static_cast<uint16_t>(stepAbsTick_ - barStartTick);

            clearLine40(frame.line0);
            char qIcon = kGlyphQtrNote;
            if (recQntzIdx_ == 7) {
                qIcon = '#';
            } else if (stepQntzBlink_ && blinkHide) {
                qIcon = ' ';
            }
            if (recQntzIdx_ == 7) {
                const int subStep = 1 + static_cast<int>(stepAbsTick_ % 6u);
                std::snprintf(tmp, sizeof(tmp), "Bar%d:#%d|", barIdx + 1, subStep);
            } else {
                std::snprintf(tmp, sizeof(tmp), "Bar%d:%c |", barIdx + 1, qIcon);
            }
            writeSpan40(frame.line0, 0, tmp, 8);

            const int stepsInBar = std::max(1, static_cast<int>(tpb) / 6);
            const int beatTicks = 192 / std::max<int>(recWorkPattern_.timeSigDen, 4);
            const int stepsPerBeat = std::max(1, beatTicks / 6);

            for (int c = 0; c < 31; ++c) {
                const int step32 = (c * stepsInBar) / 31;
                char ch = (step32 > 0 && (step32 % stepsPerBeat) == 0) ? '+' : '-';
                const uint16_t sLo = static_cast<uint16_t>(barStartTick + step32 * 6);
                const uint16_t sHi = static_cast<uint16_t>(sLo + 5u);
                bool hasExact = false;
                bool hasSub = false;
                for (uint16_t e = 0u; e < recWorkPattern_.eventCount; ++e) {
                    const auto& ev = recWorkPattern_.events[e];
                    if (ev.drumIndex == static_cast<uint8_t>(stepSelectedDrum_) && ev.tick >= sLo && ev.tick <= sHi) {
                        if ((ev.tick % 6u) == 0u) hasExact = true;
                        else                       hasSub = true;
                    }
                }
                if (hasSub)        ch = 'X';
                else if (hasExact) ch = kGlyphDiamondHit;
                frame.line0[static_cast<std::size_t>(8 + c)] = ch;
            }
            frame.line0[39] = '|';

            const int cursorCol = 8 + std::clamp((static_cast<int>(tickInBar) * 31) / std::max<int>(tpb, 1), 0, 30);
            frame.showCursor = true;
            frame.cursorRow = 0;
            frame.cursorCol = cursorCol;

            const auto* curEv = recWorkPattern_.findEventAtTick(stepAbsTick_, static_cast<uint8_t>(stepSelectedDrum_));
            char accPart[10]{ "         " };
            if (curEv != nullptr) {
                const int acc = std::clamp(static_cast<int>(curEv->velocityLevel) - 1, 0, 7);
                std::snprintf(accPart, sizeof(accPart), "%cAcc=%d   ",
                              (stepCursorField_ == 2) ? kGlyphPlayArrow : '>',
                              acc);
            }
            std::snprintf(tmp, sizeof(tmp), "ptn%c%02d %cQntz %c%-7.7s %-8.8s  %c    %c  ",
                          recPtnIsUser_ ? 'I' : 'P',
                          recPtnIdx_,
                          (stepCursorField_ == 0) ? kGlyphPlayArrow : '>',
                          (stepCursorField_ == 1) ? kGlyphPlayArrow : '>',
                          kRhythmInstNames[static_cast<std::size_t>(std::clamp(stepSelectedDrum_, 0, 60))],
                          accPart,
                          kGlyphLeftArrow,
                          kGlyphRightArrow);
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmPtnRecStore: {
            // Spec §3.2 (pp. 62-63):
            if (recStoreConfirm_) {
                setLine40(frame.line0, "P.REC> Store Pattern  Are you sure ?    ");
                setLine40(frame.line1, "Push YES to Store / NO to Cancel        ");
            } else {
                setLine40(frame.line0, "P.REC> Store Pattern                    ");
                char d0 = (recStorePendingTens_ >= 0) ? static_cast<char>('0' + recStorePendingTens_) : static_cast<char>('0' + (recStoreSlot_ / 10));
                char d1 = (recStorePendingTens_ >= 0) ? '?' : static_cast<char>('0' + (recStoreSlot_ % 10));
                std::snprintf(tmp, sizeof(tmp), "       %cptnI%c%c                 >GO  >EXIT", kGlyphPlayArrow, d0, d1);
                setLine40(frame.line1, tmp);
            }
            return true;
        }

        case ClassicScreen::RhythmPtnJobSelect: {
            // Spec §3.3 (p. 66):
            setLine40(frame.line0, "PATTERN JOB SELECT)     Select one !    ");
            setLine40(frame.line1, " >Copy>Clr>Setup>Inst>Asgn         >EXIT");
            return true;
        }

        case ClassicScreen::RhythmPtnJobCopy: {
            // Spec §3.3 (p. 66):
            if (rhyCopyConfirm_) {
                setLine40(frame.line0, "PTN COPY>            Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else {
                setLine40(frame.line0, "PTN COPY>                               ");
                char s2[5]{ "*** " };
                if (rhyCopyHasSrc2_) {
                    std::snprintf(s2, sizeof(s2), "%c%02d%c",
                                  rhyCopySrc2IsUser_ ? 'I' : 'P',
                                  rhyCopySrc2Idx_,
                                  patternHasData(rhyCopySrc2IsUser_, rhyCopySrc2Idx_) ? 'w' : ' ');
                }
                std::snprintf(tmp, sizeof(tmp), "%cptn%c%02d%c+%cptn%-4.4s %c %cptnI%02d    >GO >EXIT",
                              (rhyCopyCursor_ == 0) ? kGlyphPlayArrow : ' ',
                              rhyCopySrc1IsUser_ ? 'I' : 'P',
                              rhyCopySrc1Idx_,
                              patternHasData(rhyCopySrc1IsUser_, rhyCopySrc1Idx_) ? 'w' : ' ',
                              (rhyCopyCursor_ == 1) ? kGlyphPlayArrow : ' ',
                              s2,
                              kGlyphRightArrow,
                              (rhyCopyCursor_ == 2) ? kGlyphPlayArrow : ' ',
                              rhyCopyDstIdx_);
                setLine40(frame.line1, tmp);
            }
            return true;
        }

        case ClassicScreen::RhythmPtnJobClear: {
            // Spec §3.3 (p. 67):
            if (rhyClearConfirm_) {
                setLine40(frame.line0, "PTN CLEAR>           Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else if (rhyClearSubMode_ == 0) {
                setLine40(frame.line0, "PTN CLEAR>              Select one !    ");
                setLine40(frame.line1, " >Pattern   >Inst    >All          >EXIT");
            } else if (rhyClearSubMode_ == 1) {
                setLine40(frame.line0, "PTN CLEAR> Pattern                      ");
                std::snprintf(tmp, sizeof(tmp), " %cptnI%02d%c                      >GO  >EXIT",
                              kGlyphPlayArrow, rhyClearTargetIdx_,
                              patternHasData(true, rhyClearTargetIdx_) ? 'w' : ' ');
                setLine40(frame.line1, tmp);
            } else {
                setLine40(frame.line0, "PTN CLEAR> Inst                         ");
                std::snprintf(tmp, sizeof(tmp), " %cptnI%02d%c  %c%-7.7s            >GO  >EXIT",
                              (rhyClearCursor_ == 0) ? kGlyphPlayArrow : ' ',
                              rhyClearTargetIdx_,
                              patternHasData(true, rhyClearTargetIdx_) ? 'w' : ' ',
                              (rhyClearCursor_ == 1) ? kGlyphPlayArrow : '>',
                              kRhythmInstNames[static_cast<std::size_t>(std::clamp(rhyClearInstIdx_, 0, 60))]);
                setLine40(frame.line1, tmp);
            }
            return true;
        }

        case ClassicScreen::RhythmSetup: {
            // Spec §3.3 (pp. 67-68):
            auto mk = [&](int idx) -> char { return (rhySetupCursor_ == idx) ? kGlyphPlayArrow : '>'; };
            std::snprintf(tmp, sizeof(tmp), "SETUP>%cRch%cTch %cVel%cClick%cBeat%cSync     ",
                          mk(0), mk(1), mk(2), mk(3), mk(4), mk(5));
            setLine40(frame.line0, tmp);

            char rchBuf[4]{};
            if (rhyRch_ >= 16) std::snprintf(rchBuf, sizeof(rchBuf), "omn");
            else               std::snprintf(rchBuf, sizeof(rchBuf), "%2d ", rhyRch_ + 1);
            char tchBuf[4]{};
            if (rhyTch_ == 0) std::snprintf(tchBuf, sizeof(tchBuf), "off");
            else              std::snprintf(tchBuf, sizeof(tchBuf), "%2d ", rhyTch_);

            std::snprintf(tmp, sizeof(tmp), "       %-3.3s %-3.3s  %-3.3s %-4.4s  %-4.4s %-4.4s>EXIT",
                          rchBuf,
                          tchBuf,
                          rhyVelOn_ ? "on " : "off",
                          rhyClickPlay_ ? "play" : "rec ",
                          kBeatLabels[static_cast<std::size_t>(std::clamp(rhyBeatIdx_, 0, 5))],
                          seqSyncMidi_ ? "MIDI" : "int ");
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmInstSet: {
            // Spec §3.3 (pp. 68-69):
            // "INST SET>>Volume>Pan>Note>R.Vol>Efct    "
            // "▶BD 1       15   L|||R G#1  99   on>EXIT"
            auto mk = [&](int idx) -> char { return (rhyInstCursor_ == idx) ? kGlyphPlayArrow : '>'; };
            std::snprintf(tmp, sizeof(tmp), "INST SET>%cVolume%cPan%cNote%cR.Vol%cEfct    ",
                          mk(1), mk(2), mk(3), mk(4), mk(5));
            setLine40(frame.line0, tmp);

            const auto& kit = processor_.hostMemory().activeRhythmKit();
            const auto& inst = kit.instruments[static_cast<std::size_t>(std::clamp(rhyInstSelected_, 0, 60))];
            char noteBuf[5]{};
            formatMidiNote4(inst.midiNote, noteBuf);
            const char* panG = kPanGraphics[static_cast<std::size_t>(std::clamp(static_cast<int>(inst.pan) + 3, 0, 6))];
            const char* fxStr = (kit.effectMode == vf50::dsp::drums::RhythmEffectMode::Off) ? "off"
                              : ((kit.effectMode == vf50::dsp::drums::RhythmEffectMode::On) ? "on " : "mix");

            std::snprintf(tmp, sizeof(tmp), "%c%-7.7s    %2d   %.5s %-4.4s %2d  %.3s>EXIT",
                          (rhyInstCursor_ == 0) ? kGlyphPlayArrow : ' ',
                          kRhythmInstNames[static_cast<std::size_t>(std::clamp(rhyInstSelected_, 0, 60))],
                          static_cast<int>(inst.volume),
                          panG,
                          noteBuf,
                          static_cast<int>(kit.rhythmVolume),
                          fxStr);
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmKeyAssign: {
            // Spec §3.3 (pp. 69-70):
            // "RHYTHM ASSIGN>     >Key  Inst           "
            // "▶PRESET1            C1 = BD 1      >EXIT"
            std::snprintf(tmp, sizeof(tmp), "RHYTHM ASSIGN>     %cKey  %cInst          ",
                          (rhyAsgnCursor_ == 1) ? kGlyphPlayArrow : '>',
                          (rhyAsgnCursor_ == 2) ? kGlyphPlayArrow : ' ');
            setLine40(frame.line0, tmp);

            static constexpr std::array<const char*, 5> kMapNames = {{
                "PRESET1", "PRESET2", "PRESET3", "USER 1 ", "USER 2 "
            }};
            const int baseNote = (rhyAsgnMapIdx_ < 3) ? 31 : 36;
            char keyNote[5]{};
            formatMidiNote4(baseNote + std::clamp(rhyAsgnKeyIdx_, 0, 60), keyNote);

            int mappedDrum = -1;
            if (rhyAsgnMapIdx_ < 3) {
                mappedDrum = processor_.activeRomBank().presetDrumKeyMaps[static_cast<std::size_t>(rhyAsgnMapIdx_)][static_cast<std::size_t>(rhyAsgnKeyIdx_)];
            } else {
                const int uIdx = std::clamp(rhyAsgnMapIdx_ - 3, 0, 1);
                mappedDrum = processor_.hostMemory().userKeyMap(static_cast<std::size_t>(uIdx))[static_cast<std::size_t>(rhyAsgnKeyIdx_)];
            }
            const char* drumName = (mappedDrum >= 0 && mappedDrum < 61)
                                       ? kRhythmInstNames[static_cast<std::size_t>(mappedDrum)]
                                       : "none   ";

            std::snprintf(tmp, sizeof(tmp), "%c%-7.7s           %-3.3s = %-7.7s   >EXIT",
                          (rhyAsgnCursor_ == 0) ? kGlyphPlayArrow : ' ',
                          kMapNames[static_cast<std::size_t>(std::clamp(rhyAsgnMapIdx_, 0, 4))],
                          keyNote,
                          drumName);
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmSongJobSelect: {
            // Spec §3.4 (p. 74):
            setLine40(frame.line0, "SONG JOB SELECT)     Select one !       ");
            setLine40(frame.line1, ">Edit>Copy>Clr>Setup>Inst>Asgn>Srch>EXIT");
            return true;
        }

        case ClassicScreen::RhythmSongEdit: {
            // Spec §3.4 (pp. 72-74):
            const auto& song = rhythmSong(static_cast<std::size_t>(activeRhythmSong_));
            char partDesc[12]{ "PTN ***    " };
            if (songEditPartIdx_ < static_cast<int>(song.partCount)) {
                const auto& pt = song.parts[static_cast<std::size_t>(songEditPartIdx_)];
                const bool hasD = (pt.type == vf50::generated::RhythmSongPartType::Pattern)
                                      ? patternHasData(pt.ptnIsUser, pt.ptnIndex)
                                      : false;
                formatSongPartDesc(pt, hasD, partDesc);
            } else if (blinkHide) {
                std::snprintf(partDesc, sizeof(partDesc), "           ");
            }
            std::snprintf(tmp, sizeof(tmp), "SONG EDIT)   Part %03d = %-11.11s     ",
                          songEditPartIdx_ + 1, partDesc);
            setLine40(frame.line0, tmp);

            std::snprintf(tmp, sizeof(tmp), "song%d%c       %c    %c    %c   %c >JOB >EXIT",
                          activeRhythmSong_ + 1,
                          song.hasData() ? 'w' : ' ',
                          kGlyphLeftArrow, kGlyphRightArrow,
                          kGlyphLeftArrow, kGlyphRightArrow);
            setLine40(frame.line1, tmp);
            return true;
        }

        case ClassicScreen::RhythmSongEditJob: {
            // Spec §3.4 (pp. 75-76):
            const auto& song = rhythmSong(static_cast<std::size_t>(activeRhythmSong_));
            if (songEdJobSub_ == 0) {
                char partDesc[12]{ "PTN ***    " };
                if (songEditPartIdx_ < static_cast<int>(song.partCount)) {
                    const auto& pt = song.parts[static_cast<std::size_t>(songEditPartIdx_)];
                    const bool hasD = (pt.type == vf50::generated::RhythmSongPartType::Pattern)
                                          ? patternHasData(pt.ptnIsUser, pt.ptnIndex)
                                          : false;
                    formatSongPartDesc(pt, hasD, partDesc);
                }
                std::snprintf(tmp, sizeof(tmp), "SONG ED JOB) Part %03d = %-11.11s     ",
                              songEditPartIdx_ + 1, partDesc);
                setLine40(frame.line0, tmp);
                setLine40(frame.line1, ">Part>Ins >Del>Copy>Search>Name    >EXIT");
            } else if (songEdJobSub_ == 1) {
                setLine40(frame.line0, "SONG ED JOB) Jump Part                  ");
                std::snprintf(tmp, sizeof(tmp), "Input  Part %03d              >GO   >EXIT", songEdJumpTarget_);
                setLine40(frame.line1, tmp);
            } else if (songEdJobSub_ == 2) {
                setLine40(frame.line0, "SONG ED JOB) Insert                     ");
                setLine40(frame.line1, "Insert Part ?                      >EXIT");
            } else if (songEdJobSub_ == 3) {
                setLine40(frame.line0, "SONG ED JOB) Delete                     ");
                setLine40(frame.line1, "Delete Part ?                      >EXIT");
            } else if (songEdJobSub_ == 4) {
                if (songEdCopyConfirm_) {
                    setLine40(frame.line0, "SONG ED JOB) Copy    Are you sure ?     ");
                    setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
                } else {
                    setLine40(frame.line0, "SONG ED JOB) Copy Parts                 ");
                    std::snprintf(tmp, sizeof(tmp), "(Part%03d-%03d)%cPart%03d        >GO   >EXIT",
                                  songEdCopyFrom_, songEdCopyTo_, kGlyphRightArrow, songEdCopyDst_);
                    setLine40(frame.line1, tmp);
                }
            } else if (songEdJobSub_ == 5) {
                setLine40(frame.line0, "SONG ED JOB) Search Mark                ");
                clearLine40(frame.line1);
                std::array<char, 7> marks{};
                std::array<uint16_t, 7> parts{};
                const int mCnt = collectSongMarks(song, marks, parts);
                for (int i = 0; i < mCnt; ++i) {
                    std::snprintf(tmp, sizeof(tmp), "[%c]  ", marks[static_cast<std::size_t>(i)]);
                    writeSpan40(frame.line1, i * 5, tmp, 5);
                }
                writeSpan40(frame.line1, 35, ">EXIT", 5);
            } else {
                setLine40(frame.line0, "SONG ED JOB) Song Name                  ");
                std::snprintf(tmp, sizeof(tmp), "name : %.8s      %c   %c           >EXIT",
                              song.name.data(), kGlyphLeftArrow, kGlyphRightArrow);
                setLine40(frame.line1, tmp);
                frame.showCursor = true;
                frame.cursorRow = 1;
                frame.cursorCol = 7 + std::clamp(songEdNameCol_, 0, 7);
            }
            return true;
        }

        case ClassicScreen::RhythmSongJobCopy: {
            // Spec §3.4 (p. 76):
            if (songCopyConfirm_) {
                setLine40(frame.line0, "SONG COPY>           Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else {
                setLine40(frame.line0, "SONG COPY>                              ");
                std::snprintf(tmp, sizeof(tmp), "%cfrom SONG %d%c  %cto SONG %d      >GO >EXIT",
                              (songCopyCursor_ == 0) ? kGlyphPlayArrow : ' ',
                              songCopySrc_ + 1,
                              rhythmSong(static_cast<std::size_t>(songCopySrc_)).hasData() ? 'w' : ' ',
                              (songCopyCursor_ == 1) ? kGlyphPlayArrow : ' ',
                              songCopyDst_ + 1);
                setLine40(frame.line1, tmp);
            }
            return true;
        }

        case ClassicScreen::RhythmSongJobClear: {
            // Spec §3.4 (p. 77):
            if (rhySongClearConfirm_) {
                setLine40(frame.line0, "SONG CLEAR>          Are you sure ?     ");
                setLine40(frame.line1, "Push YES to Execute / NO to Cancel      ");
            } else if (songClearSub_ == 0) {
                setLine40(frame.line0, "SONG CLEAR>            Select one !     ");
                setLine40(frame.line1, ">Song    >All                      >EXIT");
            } else {
                setLine40(frame.line0, "SONG CLEAR> Song                        ");
                std::snprintf(tmp, sizeof(tmp), "%cSONG %d%c                      >GO  >EXIT",
                              kGlyphPlayArrow,
                              songClearTarget_ + 1,
                              rhythmSong(static_cast<std::size_t>(songClearTarget_)).hasData() ? 'w' : ' ');
                setLine40(frame.line1, tmp);
            }
            return true;
        }

        case ClassicScreen::RhythmSongJobSearch: {
            // Spec §3.4 (p. 77):
            setLine40(frame.line0, "SEARCH>                Select mark !    ");
            clearLine40(frame.line1);
            std::array<char, 7> marks{};
            std::array<uint16_t, 7> parts{};
            const int mCnt = collectSongMarks(rhythmSong(static_cast<std::size_t>(activeRhythmSong_)), marks, parts);
            for (int i = 0; i < mCnt; ++i) {
                std::snprintf(tmp, sizeof(tmp), "[%c]  ", marks[static_cast<std::size_t>(i)]);
                writeSpan40(frame.line1, i * 5, tmp, 5);
            }
            writeSpan40(frame.line1, 35, ">EXIT", 5);
            return true;
        }

        default:
            return false;
    }
}

void VF50ClassicController::applyRhythmLedSnapshot(ClassicLedSnapshot& leds) const noexcept {
    const bool inRhy = isRhythmScreen(screen_);
    const bool rhyPlaying = processor_.isRhythmEnginePlaying() || processor_.isRhythmSongPlaying();
    if (!inRhy && !rhyPlaying) {
        return;
    }

    leds.rhythm = LedState::SolidGreen;

    if (!inRhy) {
        return;
    }

    // Bank LEDs reflect active pattern bank on Pattern Play / Record screens
    if (screen_ == ClassicScreen::RhythmPtnPlay || screen_ == ClassicScreen::RhythmPtnRecSetup
        || screen_ == ClassicScreen::RhythmPtnRecReal || screen_ == ClassicScreen::RhythmPtnRecStep) {
        const bool isUser = (screen_ == ClassicScreen::RhythmPtnPlay) ? rhyPtnIsUser_ : recPtnIsUser_;
        leds.memPreset = isUser ? LedState::Off : LedState::SolidGreen;
        leds.memInt    = isUser ? LedState::SolidGreen : LedState::Off;
        leds.memCard   = LedState::Off;
    }

    if (screen_ == ClassicScreen::RhythmPtnRecSetup || screen_ == ClassicScreen::RhythmPtnRecReal
        || screen_ == ClassicScreen::RhythmPtnRecStep || screen_ == ClassicScreen::RhythmPtnRecStore) {
        leds.opSelect = LedState::SolidGreen;
    }

    // Spec §3.1 (p. 59): TR1..TR8 reflect active rhythm instruments during playback
    if (rhyPlaying) {
        for (std::size_t t = 0u; t < 8u; ++t) {
            leds.tr[t] = (rhyVoiceHitTicks_[t] > 0u) ? LedState::BlinkGreen : LedState::Off;
        }
    } else {
        for (std::size_t t = 0u; t < 8u; ++t) {
            leds.tr[t] = LedState::Off;
        }
    }
}

} // namespace vf50::classic
