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

constexpr char kGlyphPlayArrow  = static_cast<char>(0x10u); // '▶' in VF50LcdDisplay
constexpr char kGlyphRightArrow = static_cast<char>(0x7Eu); // '→'
constexpr char kGlyphLeftArrow  = static_cast<char>(0x7Fu); // '←'

void clearLine(std::array<char, 40>& line) noexcept {
    line.fill(' ');
}

void writeSpan(std::array<char, 40>& line, int col, std::string_view sv, int maxLen = -1) noexcept {
    if (col < 0) {
        return;
    }
    const int avail = 40 - col;
    if (avail <= 0) {
        return;
    }
    int n = static_cast<int>(sv.size());
    if (maxLen >= 0) {
        n = std::min(n, maxLen);
    }
    n = std::min(n, avail);
    for (int i = 0; i < n; ++i) {
        const unsigned char raw = static_cast<unsigned char>(sv[static_cast<std::size_t>(i)]);
        if (raw == static_cast<unsigned char>(kGlyphPlayArrow)
            || raw == static_cast<unsigned char>(kGlyphRightArrow)
            || raw == static_cast<unsigned char>(kGlyphLeftArrow)
            || (raw >= 0x20u && raw <= 0x7Eu)) {
            line[static_cast<std::size_t>(col + i)] = static_cast<char>(raw);
        } else {
            line[static_cast<std::size_t>(col + i)] = ' ';
        }
    }
}

void setLineFormatted(std::array<char, 40>& line, std::string_view text) noexcept {
    clearLine(line);
    writeSpan(line, 0, text, 40);
}

[[nodiscard]] char bankLetter(vf50::state::MemoryBank bank, bool cardActive) noexcept {
    if (bank == vf50::state::MemoryBank::FactoryRom) {
        return 'P';
    }
    return cardActive ? 'C' : 'I';
}

[[nodiscard]] std::array<char, 7> formatEffectShort7(const vf50::plugin::VF50AudioProcessor& proc,
                                                     uint8_t effectType,
                                                     bool bypassed) noexcept {
    std::array<char, 7> out{ { ' ','o','f','f',' ',' ',' ' } };
    if (effectType == 0u || bypassed) {
        return out;
    }
    const auto& names = proc.activeRomBank().dspEffectNames;
    if (static_cast<std::size_t>(effectType) < names.size()) {
        const auto sv = names[static_cast<std::size_t>(effectType)];
        out.fill(' ');
        const std::size_t n = std::min<std::size_t>(sv.size(), out.size());
        for (std::size_t i = 0u; i < n; ++i) {
            out[i] = sv[i];
        }
    }
    return out;
}

[[nodiscard]] bool isDiskScreen(ClassicScreen s) noexcept {
    return s == ClassicScreen::DiskRoot
        || s == ClassicScreen::DiskTypeSelect
        || s == ClassicScreen::DiskFileAction
        || s == ClassicScreen::DiskMdrMenu
        || s == ClassicScreen::DiskMdrIntTime
        || s == ClassicScreen::DiskDirectory
        || s == ClassicScreen::DiskJobMenu
        || s == ClassicScreen::DiskStatus
        || s == ClassicScreen::DiskBackupPrompt;
}

void selectSingleWithCombine(vf50::plugin::VF50AudioProcessor& proc,
                             bool cardActive,
                             vf50::state::MemoryBank bank,
                             std::size_t idx,
                             bool combineOn,
                             bool& borrowedOut) noexcept {
    const std::size_t slot = std::min<std::size_t>(idx, 99u);
    auto& mem = proc.hostMemory();
    const auto prevVoice = mem.singleEditSlot().voice;
    const uint8_t prevOpMask = mem.operatorEnableMask();

    proc.setClassicCardBankActive(bank == vf50::state::MemoryBank::UserRam && cardActive);
    proc.selectSinglePresetAtomic(bank, slot, true);
    if (bank == vf50::state::MemoryBank::UserRam && cardActive) {
        mem.singleEditSlot() = proc.virtualCard().activeBank().voices[slot];
    }

    if (!combineOn) {
        auto& v = mem.singleEditSlot().voice;
        v.transpose      = prevVoice.transpose;
        v.monoMode       = prevVoice.monoMode;
        v.pitchBendRange = prevVoice.pitchBendRange;
        v.portaMode      = prevVoice.portaMode;
        v.portaSwitch    = prevVoice.portaSwitch;
        v.portaTime      = prevVoice.portaTime;
        v.sustainSwitch  = prevVoice.sustainSwitch;
        v.chorusSwitch   = prevVoice.chorusSwitch;
        v.reverbRate     = prevVoice.reverbRate;
        v.fcVolume       = prevVoice.fcVolume;
        v.fcPitch        = prevVoice.fcPitch;
        v.fcAmp          = prevVoice.fcAmp;
        v.mwPitch        = prevVoice.mwPitch;
        v.mwAmp          = prevVoice.mwAmp;
        v.bcPitch        = prevVoice.bcPitch;
        v.bcAmp          = prevVoice.bcAmp;
        v.bcPitchBias    = prevVoice.bcPitchBias;
        v.bcEgBias       = prevVoice.bcEgBias;
        v.atPitch        = prevVoice.atPitch;
        v.atAmp          = prevVoice.atAmp;
        v.atPitchBias    = prevVoice.atPitchBias;
        v.atEgBias       = prevVoice.atEgBias;
        v.effect         = prevVoice.effect;
        mem.setOperatorEnableMask(prevOpMask);
        proc.commitHostVoiceEditToAudio(true);
        borrowedOut = true;
    } else {
        if (bank == vf50::state::MemoryBank::UserRam && cardActive) {
            proc.commitHostVoiceEditToAudio(true);
        }
        borrowedOut = false;
    }
}

void selectPerfWithCombine(vf50::plugin::VF50AudioProcessor& proc,
                           bool cardActive,
                           vf50::state::MemoryBank bank,
                           std::size_t idx,
                           bool combineOn,
                           bool& borrowedOut) noexcept {
    const std::size_t slot = std::min<std::size_t>(idx, 99u);
    auto& mem = proc.hostMemory();
    const auto prevPerf = mem.perfEditSlot().perf;

    proc.setClassicCardBankActive(bank == vf50::state::MemoryBank::UserRam && cardActive);
    proc.selectPerformancePresetAtomic(bank, slot, true);
    if (bank == vf50::state::MemoryBank::UserRam && cardActive) {
        mem.perfEditSlot() = proc.virtualCard().activeBank().performances[slot];
    }

    if (!combineOn) {
        auto& pf = mem.perfEditSlot().perf;
        pf.effect           = prevPerf.effect;
        pf.perfEffectSelect = prevPerf.perfEffectSelect;
        pf.microTuneTable   = prevPerf.microTuneTable;
        pf.microTuneRootKey = prevPerf.microTuneRootKey;
        proc.commitHostPerfEditToAudio(true);
        borrowedOut = true;
    } else {
        if (bank == vf50::state::MemoryBank::UserRam && cardActive) {
            proc.commitHostPerfEditToAudio(true);
        }
        borrowedOut = false;
    }
}

} // namespace

VF50ClassicController::VF50ClassicController(vf50::plugin::VF50AudioProcessor& processor,
                                           vf50::disk::VF50DiskManager& diskManager) noexcept
    : processor_(processor),
      diskManager_(diskManager),
      rhythmSongs_(&processor.mutableHostRhythmSongs()) {
    initRhythmDefaults();
    for (std::size_t i = 0u; i < songSlots_.size(); ++i) {
        songSlots_[i].name = { ' ','N','e','w','S','o','n','g' };
        if (i < vf50::generated::kDemoSongCount) {
            songSlots_[i].hasSetupData = false;
        }
    }
    syncSongSlotTitlesFromProcessor();
    resetToBootOrPlay(false);
}

void VF50ClassicController::resetToBootOrPlay(bool showBootSplash) noexcept {
    clearPendingBankDigits();
    clearNotice();
    instMinusArmed_ = false;
    directPcMode_ = 0;
    directPcDigits_.clear();
    clearAuditionOverridesIfNeeded(ClassicScreen::PlaySingle);
    perfHitTicks_.fill(0u);
    perfMuteHitTicks_.fill(0u);
    singleHitTicks_ = 0u;
    syncSongSlotTitlesFromProcessor();

    if (showBootSplash) {
        bootSplashTicks_ = 60; // ~2 seconds at 30 Hz (Spec §4.2, p. 11)
        screen_ = ClassicScreen::PowerOnGreeting;
        return;
    }

    bootSplashTicks_ = 0;
    const auto ws = processor_.hostMemory().workstationMode();
    if (ws == vf50::state::WorkstationMode::PerformancePlay || ws == vf50::state::WorkstationMode::PerformanceEdit) {
        screen_ = ClassicScreen::PlayPerformance;
    } else {
        screen_ = ClassicScreen::PlaySingle;
    }
}

void VF50ClassicController::syncAudioHitTelemetry() noexcept {
    // Consume wait-free audio note hit telemetry for Ch. 1.2 & 1.4 LED behavior (pp. 18, 20)
    const uint8_t activeHits = processor_.consumeActivePartHitMask();
    const uint8_t mutedHits  = processor_.consumeMutedPartHitMask();
    for (std::size_t p = 0u; p < 8u; ++p) {
        if (((activeHits >> p) & 1u) != 0u) {
            perfHitTicks_[p] = 15u; // ~0.5 s BlinkGreen (p. 20)
        }
        if (((mutedHits >> p) & 1u) != 0u) {
            perfMuteHitTicks_[p] = 15u; // ~0.5 s BlinkRed (p. 20)
        }
    }
    if (screen_ == ClassicScreen::PlaySingle && activeHits != 0u) {
        singleHitTicks_ = 6u; // ~0.2 s TR1 BlinkGreen (p. 18)
    }
    syncRhythmTelemetry30Hz();
}

void VF50ClassicController::tick30Hz() noexcept {
    ++rhyBlinkCounter_;
    if (bootSplashTicks_ > 0) {
        --bootSplashTicks_;
        if (bootSplashTicks_ == 0 && screen_ == ClassicScreen::PowerOnGreeting) {
            const auto ws = processor_.hostMemory().workstationMode();
            screen_ = (ws == vf50::state::WorkstationMode::PerformancePlay || ws == vf50::state::WorkstationMode::PerformanceEdit)
                          ? ClassicScreen::PlayPerformance
                          : ClassicScreen::PlaySingle;
        }
    }

    if (noticeTicks_ > 0) {
        --noticeTicks_;
        if (noticeTicks_ == 0) {
            noticeText_.clear();
        }
    }

    if (singleHitTicks_ > 0u) {
        --singleHitTicks_;
    }

    for (std::size_t p = 0u; p < 8u; ++p) {
        if (perfHitTicks_[p] > 0u) {
            --perfHitTicks_[p];
        }
        if (perfMuteHitTicks_[p] > 0u) {
            --perfMuteHitTicks_[p];
        }
    }

    syncAudioHitTelemetry();
    syncSongSlotTitlesFromProcessor();
    tickPhase3Demo30Hz();
}

void VF50ClassicController::enterScreen(ClassicScreen newScreen) noexcept {
    clearPendingBankDigits();
    clearNotice();
    instMinusArmed_ = false;
    screen_ = newScreen;
}

void VF50ClassicController::syncFromWorkstationMode(vf50::state::WorkstationMode wsMode, bool isEditPage) noexcept {
    clearPendingBankDigits();
    if (isEditPage) {
        screen_ = ClassicScreen::DelegatedEdit;
        return;
    }
    if (wsMode == vf50::state::WorkstationMode::PerformancePlay || wsMode == vf50::state::WorkstationMode::PerformanceEdit) {
        screen_ = ClassicScreen::PlayPerformance;
    } else {
        screen_ = ClassicScreen::PlaySingle;
    }
}

void VF50ClassicController::clearPendingBankDigits() noexcept {
    pendingBankSelect_ = -1;
    pendingTensDigit_  = -1;
}

void VF50ClassicController::setNotice(std::string_view text, int ticks30Hz) noexcept {
    noticeText_ = std::string(text);
    noticeTicks_ = std::max(1, ticks30Hz);
}

void VF50ClassicController::clearNotice() noexcept {
    noticeText_.clear();
    noticeTicks_ = 0;
}

bool VF50ClassicController::isCompareModeLocked() const noexcept {
    processor_.pullLatestAudioMemoryToHost();
    const auto& mem = processor_.hostMemory();
    return mem.isSingleCompareActive() || mem.isPerformanceCompareActive();
}

void VF50ClassicController::resetPerfInstrumentMutes() noexcept {
    perfInstMuteMask_ = 0u;
    perfMuteHitTicks_.fill(0u);
    processor_.setClassicPerfPartMuteMask(0u);
}

void VF50ClassicController::setPerfInstrumentMuted(int part0To7, bool muted) noexcept {
    if (part0To7 < 0 || part0To7 >= 8) {
        return;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << part0To7);
    if (muted) {
        perfInstMuteMask_ |= bit;
    } else {
        perfInstMuteMask_ &= static_cast<uint8_t>(~bit);
    }
    processor_.setClassicPerfPartMuteMask(perfInstMuteMask_);
}

void VF50ClassicController::togglePerfInstrumentMute(int part0To7) noexcept {
    if (part0To7 < 0 || part0To7 >= 8) {
        return;
    }
    perfInstMuteMask_ ^= static_cast<uint8_t>(1u << part0To7);
    processor_.setClassicPerfPartMuteMask(perfInstMuteMask_);
}

bool VF50ClassicController::isSequencerMuteContext() const noexcept {
    if (processor_.isDemoPlaying()) {
        return true;
    }
    return screen_ >= ClassicScreen::SeqPlaySong && screen_ <= ClassicScreen::SeqStepBarGraph;
}

void VF50ClassicController::notifySingleKeyActivity() noexcept {
    if (screen_ == ClassicScreen::PlaySingle) {
        singleHitTicks_ = 6u; // ~0.2 s pulse per Manual p. 18
    }
}

std::size_t VF50ClassicController::mappedDemoSongIndex(int slot0To7) const noexcept {
    // User sequencer slots 0..7 map 1:1 to Songs #1..#8 (never hijacked by ROM Demo Songs)
    return static_cast<std::size_t>(std::clamp(slot0To7, 0, 7));
}

void VF50ClassicController::syncSongSlotTitlesFromProcessor() noexcept {
    for (std::size_t i = 0u; i < songSlots_.size(); ++i) {
        if (processor_.hasUserSequenceInSlot(i)) {
            if (const auto* seq = processor_.userSequenceInSlot(i)) {
                const auto sv = seq->title();
                if (!sv.empty()) {
                    songSlots_[i].name.fill(' ');
                    const std::size_t n = std::min<std::size_t>(sv.size(), songSlots_[i].name.size());
                    for (std::size_t c = 0u; c < n; ++c) {
                        songSlots_[i].name[c] = sv[c];
                    }
                } else {
                    songSlots_[i].name = { ' ','N','e','w','S','o','n','g' };
                }
                songSlots_[i].populatedFromDisk = true;
            }
        } else if (songSlots_[i].populatedFromDisk) {
            songSlots_[i].name = { ' ','N','e','w','S','o','n','g' };
            songSlots_[i].populatedFromDisk = false;
        }
    }
}

void VF50ClassicController::syncActiveRhythmSongFromSeqSlot(int seqSlot0To7) noexcept {
    const int clamped = std::clamp(seqSlot0To7, 0, 7);
    if (const auto* seq = processor_.userSequenceInSlot(static_cast<std::size_t>(clamped));
        seq != nullptr && seq->valid && seq->eventCount > 0u) {
        rhyTempoBpm_ = std::clamp<int>(static_cast<int>(seq->initialTempoBpm), 30, 240);
        if (seq->hasRhythmLink) {
            if (seq->rhythmSyncIsSong) {
                activeRhythmSong_ = static_cast<int>(seq->rhythmTargetIdx & 0x07u);
                rhySongStartMeas_ = 1;
                rhySongStartPart_ = 0;
                return;
            }
            if (seq->rhythmTargetIdx >= 100u && seq->rhythmTargetIdx < 200u) {
                rhyPtnIsUser_ = false;
                rhyPtnIdx_    = static_cast<int>(seq->rhythmTargetIdx - 100u);
            } else if (seq->rhythmTargetIdx < 100u) {
                rhyPtnIsUser_ = true;
                rhyPtnIdx_    = static_cast<int>(seq->rhythmTargetIdx);
            }
        }
    }
    // Default 1:1 pairing between SEQ Song slot (1..8) and RHYTHM Song slot (1..8)
    activeRhythmSong_ = clamped;
    rhySongStartMeas_ = 1;
    rhySongStartPart_ = 0;
}

void VF50ClassicController::applyAudioPlaySongIndexForSlot(int slot0To7, bool startPlaying) noexcept {
    const std::size_t mappedIdx = mappedDemoSongIndex(slot0To7);
    if (startPlaying) {
        processor_.requestDemoStart(mappedIdx);
    } else {
        processor_.selectDemoSong(mappedIdx);
    }
}

void VF50ClassicController::selectSeqSongSlot(int slot0To7, bool applyStoredSetup) noexcept {
    const int clamped = std::clamp(slot0To7, 0, 7);
    const bool changed = (clamped != activeSongSlot_);
    activeSongSlot_ = clamped;
    if (changed) {
        processor_.resetSequencerTrackMutes(); // Manual p. 87: changing song number defeats mutes
        syncActiveRhythmSongFromSeqSlot(activeSongSlot_);
    }
    const bool wasPlaying = processor_.isDemoPlaying();
    applyAudioPlaySongIndexForSlot(activeSongSlot_, wasPlaying);

    if (applyStoredSetup && songSlots_[static_cast<std::size_t>(activeSongSlot_)].hasSetupData) {
        const auto& meta = songSlots_[static_cast<std::size_t>(activeSongSlot_)];
        const auto pBank = (meta.setupPerfBank == 0u)
                               ? vf50::state::MemoryBank::UserRam
                               : vf50::state::MemoryBank::FactoryRom;
        const auto vBank = (meta.setupVoiceBank == 0u)
                               ? vf50::state::MemoryBank::UserRam
                               : vf50::state::MemoryBank::FactoryRom;
        if (meta.setupPlayMode == 0u) {
            processor_.selectSinglePresetAtomic(vBank, meta.setupVoiceNum, true);
        } else {
            processor_.selectPerformancePresetAtomic(pBank, meta.setupPerfNum, true);
        }
    }
}

bool VF50ClassicController::onPressPerformance() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (isCompareModeLocked()) {
        setNotice("** COMPARE ACTIVE - MODE LOCKED **");
        return false;
    }
    if (multiVoiceEditActive_) {
        saveActiveMultiVoiceBackToSlot();
        multiVoiceEditActive_ = false;
        multiVoiceExitSglConfirm_ = false;
        multiVoiceDirty_.fill(false);
    }
    bootSplashTicks_ = 0;
    clearAuditionOverridesIfNeeded(ClassicScreen::PlayPerformance);
    processor_.setClassicRhythmKbdOverride(0); // Return keyboard to Synth (p. 59)
    processor_.setClassicRhythmCaptureNotes(false);
    processor_.hostMemory().setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
    processor_.stateMachine().setClassicTopMode(vf50::state::ClassicTopMode::MODE_PERFORMANCE);
    processor_.commitHostPerfEditToAudio(true);
    screen_ = ClassicScreen::PlayPerformance;
    return true;
}

bool VF50ClassicController::onPressSingle() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (isCompareModeLocked()) {
        setNotice("** COMPARE ACTIVE - MODE LOCKED **");
        return false;
    }
    if (multiVoiceEditActive_) {
        bool anyDirty = false;
        for (bool d : multiVoiceDirty_) {
            if (d) { anyDirty = true; break; }
        }
        if (anyDirty) {
            multiVoiceExitSglConfirm_ = true;
            screen_ = ClassicScreen::UtilOthersMultiVoiceSelect;
            return false;
        }
        multiVoiceEditActive_ = false;
        multiVoiceExitSglConfirm_ = false;
    }
    bootSplashTicks_ = 0;
    clearAuditionOverridesIfNeeded(ClassicScreen::PlayPerformance);
    processor_.setClassicRhythmKbdOverride(0); // Return keyboard to Synth (p. 59)
    processor_.setClassicRhythmCaptureNotes(false);
    processor_.hostMemory().setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
    processor_.stateMachine().setClassicTopMode(vf50::state::ClassicTopMode::MODE_VOICE);
    processor_.commitHostVoiceEditToAudio(true);
    screen_ = ClassicScreen::PlaySingle;
    return true;
}

bool VF50ClassicController::onPressSeq() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (isCompareModeLocked()) {
        setNotice("** COMPARE ACTIVE - MODE LOCKED **");
        return false;
    }
    bootSplashTicks_ = 0;
    clearAuditionOverridesIfNeeded(ClassicScreen::SeqPlaySong);
    processor_.setClassicRhythmKbdOverride(0);
    processor_.setClassicRhythmCaptureNotes(false);
    syncSongSlotTitlesFromProcessor();
    screen_ = ClassicScreen::SeqPlaySong;
    return true;
}

bool VF50ClassicController::onPressRhythm() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (isCompareModeLocked()) {
        setNotice("** COMPARE ACTIVE - MODE LOCKED **");
        return false;
    }
    bootSplashTicks_ = 0;
    clearAuditionOverridesIfNeeded(ClassicScreen::RhythmPtnPlay);
    processor_.setClassicRhythmKbdOverride(rhyKbdIsRhy_ ? 1 : 0);
    processor_.setClassicRhythmCaptureNotes(false);
    if (processor_.isRhythmSongPlaying()) {
        lastRhythmSubmodeIsSong_ = true;
    }
    screen_ = lastRhythmSubmodeIsSong_ ? ClassicScreen::RhythmSongPlay : ClassicScreen::RhythmPtnPlay;
    return true;
}

bool VF50ClassicController::onPressJob() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (handleRhythmJobPress()) {
        return true;
    }
    if (screen_ == ClassicScreen::SeqPlaySong
        || screen_ == ClassicScreen::SeqSongSetupPrompt
        || screen_ == ClassicScreen::SeqStoreSetupConfirm) {
        screen_ = ClassicScreen::SeqJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobTransmitCh
        || screen_ == ClassicScreen::SeqJobSetup
        || screen_ == ClassicScreen::SeqJobSong
        || screen_ == ClassicScreen::SeqJobEditMenu
        || screen_ == ClassicScreen::SeqJobEditSub
        || screen_ == ClassicScreen::SeqJobRecMode) {
        screen_ = ClassicScreen::SeqJobSelect;
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSelect) {
        screen_ = ClassicScreen::SeqPlaySong;
        return true;
    }
    if (screen_ == ClassicScreen::SeqRecStandby || screen_ == ClassicScreen::SeqStepBarGraph) {
        screen_ = ClassicScreen::SeqJobSelect;
        return true;
    }
    return false;
}

bool VF50ClassicController::onPressDisk() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (isDiskScreen(screen_)) {
        screen_ = (isDiskScreen(preUtilityScreen_) || preUtilityScreen_ == ClassicScreen::UtilMemoryProtect)
                      ? ClassicScreen::PlaySingle
                      : preUtilityScreen_;
        return true;
    }
    if (!isDiskScreen(screen_) && screen_ != ClassicScreen::UtilMemoryProtect) {
        preUtilityScreen_ = screen_;
    }
    diskActionConfirm_ = false;
    diskFormatConfirm_ = false;
    screen_ = ClassicScreen::DiskRoot;
    return true;
}

bool VF50ClassicController::onPressMemoryProtect() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (screen_ == ClassicScreen::UtilMemoryProtect) {
        screen_ = (isDiskScreen(preUtilityScreen_) || preUtilityScreen_ == ClassicScreen::UtilMemoryProtect)
                      ? ClassicScreen::PlaySingle
                      : preUtilityScreen_;
        return true;
    }
    if (!isDiskScreen(screen_) && screen_ != ClassicScreen::UtilMemoryProtect && !isUtilOthersScreen(screen_)) {
        preUtilityScreen_ = screen_;
    }
    memProtectCursor_ = 0;
    screen_ = ClassicScreen::UtilMemoryProtect;
    return true;
}

bool VF50ClassicController::onPressCompare(bool /*toggledOn*/) noexcept {
    clearNotice();
    return true;
}

bool VF50ClassicController::onPressCompare() noexcept {
    clearNotice();
    clearPendingBankDigits();
    instMinusArmed_ = false;
    if (multiVoiceEditActive_) {
        setNotice("Not Avail in MultiEdit", 45);
        return false;
    }
    processor_.pullLatestAudioMemoryToHost();
    if (isPerformanceContextScreen()) {
        ensurePerfEditBackupInitialized();
        (void)processor_.hostMemory().togglePerformanceCompare();
        processor_.commitHostPerfEditToAudio(true);
    } else {
        ensureVoiceEditBackupInitialized();
        (void)processor_.hostMemory().toggleSingleCompare();
        processor_.commitHostVoiceEditToAudio(true);
    }
    return true;
}

bool VF50ClassicController::onHoldPlayModeKey(bool isPerformance, bool heldDown) noexcept {
    if (screen_ == ClassicScreen::MidiPgmEdit && isPerformance) {
        midiPgmEditPerfHeld_ = heldDown;
        return true;
    }
    if (heldDown) {
        if ((screen_ == ClassicScreen::PlaySingle || screen_ == ClassicScreen::PlayPerformance) &&
            !isCompareModeLocked()) {
            directPcMode_ = isPerformance ? 2 : 1;
            directPcDigits_.clear();
            return true;
        }
        return false;
    }

    if (directPcMode_ != 0) {
        const bool hadDigits = !directPcDigits_.empty();
        if (directPcDigits_.size() == 3u) {
            const int pc = (directPcDigits_[0] - '0') * 100
                         + (directPcDigits_[1] - '0') * 10
                         + (directPcDigits_[2] - '0');
            if (pc >= 1 && pc <= 128) {
                const auto& sys = processor_.classicSystemSettings();
                if (sys.midiOn && sys.pgmChangeMode != 0u) {
                    const uint8_t status = static_cast<uint8_t>(0xC0u | (sys.transmitCh & 0x0Fu));
                    const uint8_t data1  = static_cast<uint8_t>(pc - 1);
                    processor_.enqueueMidiOutShort(status, data1, 0u, 2u);
                    setNotice("PC Transmitted", 30);
                }
            } else {
                setNotice("Illegal PC No !", 45);
            }
        } else if (hadDigits) {
            setNotice("Canceled PC", 30);
        }
        directPcMode_ = 0;
        directPcDigits_.clear();
        if (!hadDigits) {
            return isPerformance ? onPressPerformance() : onPressSingle();
        }
        return true;
    }
    return isPerformance ? onPressPerformance() : onPressSingle();
}

bool VF50ClassicController::onPressInstMinus() noexcept {
    clearNotice();
    instMinusArmed_ = !instMinusArmed_;
    return true;
}

bool VF50ClassicController::onPressMemBank(int bank0Int1Card2Preset) noexcept {
    clearNotice();
    instMinusArmed_ = false;
    const int bIdx = std::clamp(bank0Int1Card2Preset, 0, 2);

    if (isRhythmScreen(screen_)) {
        return handleRhythmMemBank(bIdx);
    }
    if (isPhase3UtilScreen(screen_)) {
        return handleUtilMemBank(bIdx);
    }

    if (screen_ == ClassicScreen::PerfEditVoiceNo) {
        if (isCompareModeLocked()) {
            setNotice("Compare Active - Locked", 45);
            return false;
        }
        auto& pf = processor_.hostMemory().perfEditSlot().perf;
        if (!isPerfInstActive(pf, perfEditInst_)) {
            return false;
        }
        // Spec §1.3 (p. 25): Internal perf -> INT & PRESET only; Card perf -> CARD & PRESET only
        const bool perfIsCard = (processor_.hostMemory().selectedPerfBank() == vf50::state::MemoryBank::UserRam) && cardBankActive_;
        if (bIdx == 1 && !perfIsCard) {
            setNotice("Card Voice Not Allowed", 45);
            return false;
        }
        if (bIdx == 0 && perfIsCard) {
            setNotice("Int Voice Not Allowed", 45);
            return false;
        }
        pf.instruments[static_cast<std::size_t>(perfEditInst_)].voiceBank = (bIdx == 2) ? 1u : 0u;
        processor_.hostMemory().markPerfEditDirty();
        commitCurrentPerfEdit(false);
        return true;
    }

    if (screen_ == ClassicScreen::StoreVoice || screen_ == ClassicScreen::StorePerf || screen_ == ClassicScreen::CopyEffect) {
        if (bIdx == 2) {
            setNotice("Preset ROM Read-Only", 45);
            return false;
        }
        storeTargetCard_ = (bIdx == 1);
        storeIntProtBypassed_ = false;
        storePendingTens_ = -1;
        storeConfirmReady_ = false;
        return true;
    }

    if (screen_ == ClassicScreen::StoreMultiVoice) {
        const bool perfIsCard = (processor_.hostMemory().selectedPerfBank() == vf50::state::MemoryBank::UserRam) && cardBankActive_;
        if (bIdx == 2 || (bIdx == 1 && !perfIsCard) || (bIdx == 0 && perfIsCard)) {
            setNotice("Bank Locked to Perf", 45);
            return false;
        }
        storeTargetCard_ = perfIsCard;
        storePendingTens_ = -1;
        storeConfirmReady_ = false;
        return true;
    }

    if (screen_ != ClassicScreen::PlaySingle && screen_ != ClassicScreen::PlayPerformance) {
        return false;
    }
    if (bankHoldEnabled_) {
        const bool combineOn = processor_.classicSystemSettings().combineWithFunction;
        cardBankActive_ = (bIdx == 1);
        const auto targetBank = (bIdx == 2)
                                    ? vf50::state::MemoryBank::FactoryRom
                                    : vf50::state::MemoryBank::UserRam;
        if (screen_ == ClassicScreen::PlayPerformance) {
            resetPerfInstrumentMutes();
            const std::size_t idx = processor_.hostMemory().selectedPerfIndex();
            selectPerfWithCombine(processor_, cardBankActive_, targetBank, idx, combineOn, combineBorrowedFunction_);
        } else {
            const std::size_t idx = processor_.hostMemory().selectedVoiceIndex();
            selectSingleWithCombine(processor_, cardBankActive_, targetBank, idx, combineOn, combineBorrowedFunction_);
        }
        clearPendingBankDigits();
        return true;
    }
    pendingBankSelect_ = bIdx;
    pendingTensDigit_  = -1;
    return true;
}

bool VF50ClassicController::onPressEditTrackKey(int track0To7) noexcept {
    const int trk = std::clamp(track0To7, 0, 7);
    if (screen_ == ClassicScreen::PlayPerformance && instMinusArmed_) {
        instMinusArmed_ = false;
        const auto& inst = processor_.hostMemory().activePerformance().instruments[static_cast<std::size_t>(trk)];
        const uint8_t ch = (inst.receiveChannel < 16u) ? inst.receiveChannel : 0u;
        processor_.mutableClassicSystemSettings().transmitCh = ch;
        processor_.publishClassicSystemStateToAudio();
        setNotice("KbdTch Updated", 30);
        return true;
    }
    if (screen_ == ClassicScreen::SeqRecStandby || screen_ == ClassicScreen::SeqStepBarGraph) {
        clearNotice();
        if (instMinusArmed_) {
            instMinusArmed_ = false;
            // [-] + TR1..8 sets keyboard transmit channel to that track's receive channel (p. 83)
            recSelectedTrack_ = trk;
            return true;
        }
        recSelectedTrack_ = trk;
        return true;
    }
    if (screen_ == ClassicScreen::SeqPlaySong
        || screen_ == ClassicScreen::SeqJobSelect
        || screen_ == ClassicScreen::SeqJobSetup
        || screen_ == ClassicScreen::SeqJobEditMenu
        || processor_.isDemoPlaying()) {
        clearNotice();
        processor_.toggleSequencerTrackMute(static_cast<std::size_t>(trk));
        return true;
    }
    return false;
}

bool VF50ClassicController::onPressRhyTrackKey() noexcept {
    if (screen_ == ClassicScreen::PlayPerformance && instMinusArmed_) {
        instMinusArmed_ = false;
        const uint8_t ch = (rhyRch_ < 16) ? static_cast<uint8_t>(rhyRch_) : 9u;
        processor_.mutableClassicSystemSettings().transmitCh = ch;
        processor_.publishClassicSystemStateToAudio();
        setNotice("KbdTch Updated", 30);
        return true;
    }
    if (screen_ == ClassicScreen::SeqPlaySong
        || screen_ == ClassicScreen::SeqJobSelect
        || screen_ == ClassicScreen::SeqJobSetup
        || screen_ == ClassicScreen::SeqJobEditMenu
        || processor_.isDemoPlaying()) {
        clearNotice();
        processor_.toggleSequencerRhythmMute();
        return true;
    }
    return false;
}

bool VF50ClassicController::onPressStopAndBwdTogether() noexcept {
    clearNotice();
    if (isRhythmScreen(screen_)) {
        return handleRhythmStopAndBwd();
    }
    if (isPhase3UtilScreen(screen_) && handleUtilStopAndBwd()) {
        return true;
    }
    if (processor_.isRhythmEnginePlaying() || processor_.isRhythmSongPlaying()) {
        stopRhythmPlayCurrent();
    }
    rhySongStartMeas_ = 1;
    rhySongStartPart_ = 0;
    processor_.requestClassicRhythmRewind();
    processor_.requestDemoRewindToTop();
    if (screen_ == ClassicScreen::SeqStepBarGraph) {
        stepBarTick_ = 0;
        screen_ = ClassicScreen::SeqRecStandby;
    }
    return true;
}

bool VF50ClassicController::onPressSeqOp(int btn0Rec1Bwd2Stop3Play4Fwd) noexcept {
    clearNotice();
    if (isRhythmScreen(screen_)) {
        return handleRhythmSeqOp(btn0Rec1Bwd2Stop3Play4Fwd);
    }
    if (isPhase3UtilScreen(screen_) && handleUtilSeqOp(btn0Rec1Bwd2Stop3Play4Fwd)) {
        return true;
    }
    if (isVoiceEditScreen(screen_)) {
        if (btn0Rec1Bwd2Stop3Play4Fwd == 0) {
            selectedOperator_ = (selectedOperator_ + 1) & 3;
            return true;
        }
        if (btn0Rec1Bwd2Stop3Play4Fwd >= 1 && btn0Rec1Bwd2Stop3Play4Fwd <= 4) {
            if (multiVoiceEditActive_) {
                setNotice("Op Mute Off in MultiEd", 45);
                return false;
            }
            if (isCompareModeLocked()) {
                setNotice("Compare Active - Locked", 45);
                return false;
            }
            const int opIdx = btn0Rec1Bwd2Stop3Play4Fwd - 1;
            const uint8_t curMask = processor_.hostMemory().operatorEnableMask();
            const uint8_t nextMask = static_cast<uint8_t>(curMask ^ static_cast<uint8_t>(1u << opIdx));
            processor_.hostMemory().setOperatorEnableMask(nextMask);
            commitCurrentVoiceEdit(false);
            return true;
        }
    }
    switch (btn0Rec1Bwd2Stop3Play4Fwd) {
        case 0: // [RECORD] / [OP SELECT]
            if (screen_ == ClassicScreen::SeqPlaySong) {
                recStandbyCursor_ = 0;
                recMeas_          = std::max(1, processor_.activeDemoMeasure());
                recPunchIn_       = std::max(recPunchIn_, recMeas_);
                recPunchOut_      = std::max(recPunchOut_, recPunchIn_);
                screen_           = ClassicScreen::SeqRecStandby;
                return true;
            }
            if (screen_ == ClassicScreen::SeqRecStandby || screen_ == ClassicScreen::SeqStepBarGraph) {
                screen_ = ClassicScreen::SeqPlaySong;
                return true;
            }
            return false;

        case 1: // [BWD <<] / [OP 1]
            if (screen_ == ClassicScreen::SeqPlaySong) {
                processor_.requestDemoSeekMeasures(-1);
                return true;
            }
            if (screen_ == ClassicScreen::SeqStepBarGraph) {
                stepBarTick_ = std::max(0, stepBarTick_ - 1);
                return true;
            }
            return false;

        case 2: // [STOP ■] / [OP 2]
            if (screen_ == ClassicScreen::SeqStepBarGraph) {
                stepGateSubView_ = false;
                screen_ = ClassicScreen::SeqRecStandby;
                return true;
            }
            if (screen_ == ClassicScreen::SeqRecStandby) {
                screen_ = ClassicScreen::SeqPlaySong;
                return true;
            }
            if (screen_ == ClassicScreen::PlayPerformance || screen_ == ClassicScreen::PlaySingle) {
                if (processor_.isRhythmEnginePlaying() || processor_.isRhythmSongPlaying()) {
                    stopRhythmPlayCurrent();
                }
                if (processor_.isDemoPlaying()) {
                    processor_.requestDemoStop();
                }
                return true;
            }
            if (screen_ == ClassicScreen::SeqPlaySong || processor_.isDemoPlaying()) {
                if (processor_.isDemoPlaying()) {
                    processor_.requestDemoStop();
                } else if (screen_ == ClassicScreen::SeqPlaySong) {
                    return onPressStopAndBwdTogether();
                }
                return true;
            }
            return false;

        case 3: // [PLAY ▶] / [OP 3]
            if (screen_ == ClassicScreen::SeqRecStandby) {
                if (seqRecMode_ == SeqRecMode::Step) {
                    stepGateSubView_ = false;
                    stepBarTick_     = 0;
                    screen_          = ClassicScreen::SeqStepBarGraph;
                    return true;
                }
                applyAudioPlaySongIndexForSlot(activeSongSlot_, true);
                return true;
            }
            if (screen_ == ClassicScreen::SeqStepBarGraph) {
                stepBarTick_ = (stepBarTick_ + 1) & 31;
                return true;
            }
            if (screen_ == ClassicScreen::SeqPlaySong || processor_.isDemoPlaying()) {
                if (!processor_.isDemoPlaying()) {
                    applyAudioPlaySongIndexForSlot(activeSongSlot_, true);
                }
                return true;
            }
            return false;

        case 4: // [FWD >>] / [OP 4]
            if (screen_ == ClassicScreen::SeqPlaySong) {
                processor_.requestDemoSeekMeasures(+1);
                return true;
            }
            if (screen_ == ClassicScreen::SeqStepBarGraph) {
                stepBarTick_ = std::min(31, stepBarTick_ + 1);
                return true;
            }
            return false;

        default:
            return false;
    }
}

bool VF50ClassicController::onPressSoftKey(int f0To7) noexcept {
    const int k = std::clamp(f0To7, 0, 7);
    clearNotice();

    if (isRhythmScreen(screen_)) {
        return handleRhythmSoftKey(k);
    }
    if (isPhase3UtilScreen(screen_)) {
        return handleUtilSoftKey(k);
    }

    switch (screen_) {
        case ClassicScreen::PowerOnGreeting:
            return true;

        case ClassicScreen::PlaySingle:
            return true;

        case ClassicScreen::PlayPerformance:
            if (instMinusArmed_) {
                perfInstMuteMask_ ^= static_cast<uint8_t>(1u << k);
                processor_.setClassicPerfPartMuteMask(perfInstMuteMask_);
                instMinusArmed_ = false;
                return true;
            }
            return true;

        case ClassicScreen::DelegatedEdit:
            return false;

        case ClassicScreen::SeqPlaySong:
            if (k <= 2) {
                playSongCursorField_ = 0; // Song select
            } else if (k == 5) {
                playSongCursorField_ = 1; // >Meas
            } else if (k >= 6) {
                playSongCursorField_ = 2; // >Tempo
            }
            return true;

        case ClassicScreen::SeqSongSetupPrompt:
            if (k <= 2) { // >with SETUP
                selectSeqSongSlot(pendingSongSlot_, true);
                screen_ = ClassicScreen::SeqPlaySong;
            } else if (k >= 4 && k <= 6) { // >Song only
                selectSeqSongSlot(pendingSongSlot_, false);
                screen_ = ClassicScreen::SeqPlaySong;
            } else if (k == 7) { // >EXIT
                screen_ = ClassicScreen::SeqPlaySong;
            }
            return true;

        case ClassicScreen::SeqStoreSetupConfirm:
            if (k == 7) {
                screen_ = ClassicScreen::SeqPlaySong;
            }
            return true;

        case ClassicScreen::SeqJobSelect:
            if (k == 0) {
                seqSetupCursorField_ = 0;
                screen_ = ClassicScreen::SeqJobSetup;
            } else if (k == 1) {
                tchCursorTrack_ = 0;
                screen_ = ClassicScreen::SeqJobTransmitCh;
            } else if (k == 2) {
                songNameCursorCol_ = 0;
                songClearConfirm_  = false;
                screen_ = ClassicScreen::SeqJobSong;
            } else if (k == 3) {
                screen_ = ClassicScreen::SeqJobEditMenu;
            } else if (k == 4) {
                recModeCursor_ = 0;
                screen_ = ClassicScreen::SeqJobRecMode;
            } else if (k == 7) {
                screen_ = ClassicScreen::SeqPlaySong;
            }
            return true;

        case ClassicScreen::SeqJobSetup:
            if (k == 1)      seqSetupCursorField_ = 0; // >Rch
            else if (k == 2) seqSetupCursorField_ = 1; // >A.T
            else if (k == 3) seqSetupCursorField_ = 2; // >Vel
            else if (k == 4) seqSetupCursorField_ = 3; // >Click
            else if (k == 6) seqSetupCursorField_ = 4; // >Sync
            else if (k == 7) screen_ = ClassicScreen::SeqJobSelect;
            return true;

        case ClassicScreen::SeqJobTransmitCh:
            tchCursorTrack_ = k;
            return true;

        case ClassicScreen::SeqJobSong:
            if (k <= 1) {
                songClearConfirm_ = false;
            } else if (k == 2) {
                songNameCursorCol_ = std::max(0, std::min(7, songNameCursorCol_) - 1);
            } else if (k == 3) {
                songNameCursorCol_ = std::min(7, std::min(7, songNameCursorCol_) + 1);
            } else if (k == 4 || k == 5) {
                songClearConfirm_ = true;
            } else if (k == 7) {
                songClearConfirm_ = false;
                screen_ = ClassicScreen::SeqJobSelect;
            }
            return true;

        case ClassicScreen::SeqJobEditMenu:
            if (k == 7) {
                screen_ = ClassicScreen::SeqJobSelect;
            } else {
                seqEditSubKind_     = static_cast<SeqEditSubKind>(k);
                seqEditCursorField_ = 0;
                seqEditConfirm_     = false;
                screen_             = ClassicScreen::SeqJobEditSub;
            }
            return true;

        case ClassicScreen::SeqJobEditSub:
            if (k == 7) {
                seqEditConfirm_ = false;
                screen_ = ClassicScreen::SeqJobEditMenu;
            } else if (k == 6) {
                seqEditConfirm_ = true;
            } else {
                seqEditConfirm_ = false;
                seqEditCursorField_ = k;
            }
            return true;

        case ClassicScreen::SeqJobRecMode:
            if (k == 1 || k == 2) {
                recModeCursor_ = 0;
            } else if (k >= 3 && k <= 5) {
                recModeCursor_ = 1;
            } else if (k == 7) {
                screen_ = ClassicScreen::SeqJobSelect;
            }
            return true;

        case ClassicScreen::SeqRecStandby:
            if (seqRecMode_ == SeqRecMode::Punch) {
                if (k <= 1)      recStandbyCursor_ = 0; // >Mode
                else if (k <= 3) recStandbyCursor_ = 1; // >Voice
                else if (k == 4) recStandbyCursor_ = 2; // >Meas
                else if (k == 5) recStandbyCursor_ = 3; // >In
                else             recStandbyCursor_ = 4; // >Out
            } else {
                if (k <= 1)      recStandbyCursor_ = 0; // >Mode
                else if (k == 2) recStandbyCursor_ = 1; // >Voice
                else if (k == 3) recStandbyCursor_ = 2; // >Time
                else if (k <= 5) recStandbyCursor_ = 3; // >Meas
                else             recStandbyCursor_ = 4; // >Tempo
            }
            return true;

        case ClassicScreen::SeqStepBarGraph:
            if (!stepGateSubView_) {
                if (k >= 2 && k <= 4) {
                    stepGateSubView_ = true;
                } else if (k == 7) {
                    screen_ = ClassicScreen::SeqRecStandby;
                }
            } else {
                if (k == 2)      stepGatePreset_ = 0; // Norm (80%)
                else if (k == 3) stepGatePreset_ = 1; // Stac (50%)
                else if (k == 4) stepGatePreset_ = 2; // Slur (99%)
                else if (k == 7) stepGateSubView_ = false;
            }
            return true;

        case ClassicScreen::DiskRoot:
            diskActionConfirm_ = false;
            diskFormatConfirm_ = false;
            if (k == 0) {
                diskOp_ = DiskOperation::Save;
                screen_ = ClassicScreen::DiskTypeSelect;
            } else if (k == 1) {
                diskOp_ = DiskOperation::Load;
                screen_ = ClassicScreen::DiskTypeSelect;
            } else if (k == 2) {
                diskOp_ = DiskOperation::Delete;
                screen_ = ClassicScreen::DiskTypeSelect;
            } else if (k == 3 || k == 4) {
                diskOp_ = DiskOperation::Rename;
                screen_ = ClassicScreen::DiskTypeSelect;
            } else if (k == 5) {
                screen_ = ClassicScreen::DiskMdrMenu;
            } else if (k == 6) {
                diskFileIndex_ = 0;
                screen_ = ClassicScreen::DiskDirectory;
            } else if (k == 7) {
                screen_ = ClassicScreen::DiskJobMenu;
            }
            return true;

        case ClassicScreen::DiskTypeSelect:
            if (k == 7) {
                screen_ = ClassicScreen::DiskRoot;
                return true;
            }
            if (k <= 4) {
                diskDataType_      = static_cast<DiskDataType>(k);
                diskFileIndex_     = 0;
                diskActionConfirm_ = false;
                diskActionOverwrite_ = false;
                diskEditingName_   = (diskOp_ == DiskOperation::Save);
                diskNameFreshDefault_ = (diskOp_ == DiskOperation::Save);
                diskNameCursorCol_ = 0;
                if (diskOp_ == DiskOperation::Rename) {
                    const auto matches = filteredDiskEntryIndices(diskDataType_);
                    if (!matches.empty()) {
                        const auto& ename = diskManager_.entries()[matches[0]].name;
                        diskEditName_.fill(' ');
                        const auto dot = ename.find_last_of('.');
                        const std::size_t stemLen = (dot == std::string::npos) ? ename.size() : dot;
                        for (std::size_t c = 0u; c < std::min<std::size_t>(stemLen, 8u); ++c) {
                            diskEditName_[c] = ename[c];
                        }
                    }
                } else if (diskOp_ == DiskOperation::Save) {
                    diskEditName_ = { 'N','e','w','F','i','l','e',' ' };
                }
                screen_ = ClassicScreen::DiskFileAction;
            }
            return true;

        case ClassicScreen::DiskFileAction:
            if (diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename) {
                if (k == 0) {
                    diskEditingName_ = false;
                    diskNameFreshDefault_ = false;
                    diskActionConfirm_ = false;
                } else if (k >= 1 && k <= 3) {
                    diskEditingName_ = true;
                    diskActionConfirm_ = false;
                } else if (k == 5) {
                    diskEditingName_ = true;
                    diskNameFreshDefault_ = false;
                    diskNameCursorCol_ = std::max(0, std::min(7, diskNameCursorCol_) - 1);
                } else if (k == 6) {
                    diskEditingName_ = true;
                    diskNameFreshDefault_ = false;
                    diskNameCursorCol_ = std::min(7, std::min(7, diskNameCursorCol_) + 1);
                } else if (k == 7) {
                    if (!diskManager_.isMounted()) {
                        setNotice("Insert DISK !");
                        return true;
                    }
                    if (diskOp_ == DiskOperation::Rename && diskManager_.isMountedSourceReadOnly()) {
                        setNotice("Read-Only Disk Image");
                        return true;
                    }
                    if (diskOp_ == DiskOperation::Rename && diskManager_.isWriteProtected()) {
                        setNotice("Disk Write Protected");
                        return true;
                    }
                    if (diskOp_ == DiskOperation::Save && !diskManager_.isMountedSourceReadOnly()
                        && diskManager_.isWriteProtected()) {
                        setNotice("Disk Write Protected");
                        return true;
                    }
                    std::string stem;
                    for (char c : diskEditName_) {
                        stem.push_back(c);
                    }
                    while (!stem.empty() && stem.back() == ' ') {
                        stem.pop_back();
                    }
                    vf50::disk::VF50FileKind saveKind = vf50::disk::VF50FileKind::AllData;
                    if (diskDataType_ == DiskDataType::Syn)       saveKind = vf50::disk::VF50FileKind::VoiceBank;
                    else if (diskDataType_ == DiskDataType::Seq)  saveKind = vf50::disk::VF50FileKind::Sequence;
                    else if (diskDataType_ == DiskDataType::RSeq) saveKind = vf50::disk::VF50FileKind::RhythmData;
                    else if (diskDataType_ == DiskDataType::Card) saveKind = vf50::disk::VF50FileKind::CardData;
                    diskActionOverwrite_ = (diskOp_ == DiskOperation::Save)
                                        && diskManager_.hasExistingFileInSaveTarget(stem, saveKind);
                    diskActionConfirm_ = true;
                }
            } else {
                // Load / Delete (F1=File, F7=>GO, F8=>EXIT)
                if (k == 0) {
                    diskActionConfirm_ = false;
                } else if (k == 6) {
                    if (!diskManager_.isMounted()) {
                        setNotice("Insert DISK !");
                        return true;
                    }
                    const auto matches = filteredDiskEntryIndices(diskDataType_);
                    if (matches.empty()) {
                        setNotice("No Matching File");
                        return true;
                    }
                    if (diskOp_ == DiskOperation::Delete && diskManager_.isMountedSourceReadOnly()) {
                        setNotice("Read-Only Disk Image");
                        return true;
                    }
                    if (diskOp_ == DiskOperation::Delete && diskManager_.isWriteProtected()) {
                        setNotice("Disk Write Protected");
                        return true;
                    }
                    diskActionConfirm_ = true;
                } else if (k == 7) {
                    diskActionConfirm_ = false;
                    screen_ = ClassicScreen::DiskTypeSelect;
                }
            }
            return true;

        case ClassicScreen::DiskMdrMenu:
            if (k == 7) {
                screen_ = ClassicScreen::DiskRoot;
            } else if (k == 4 || k == 5) {
                screen_ = ClassicScreen::DiskMdrIntTime;
            } else {
                setNotice("MDR Buffer Empty");
            }
            return true;

        case ClassicScreen::DiskMdrIntTime:
            if (k == 7) {
                screen_ = ClassicScreen::DiskMdrMenu;
            }
            return true;

        case ClassicScreen::DiskDirectory:
            if (k == 7) {
                screen_ = ClassicScreen::DiskRoot;
            }
            return true;

        case ClassicScreen::DiskJobMenu:
            diskFormatConfirm_ = false;
            if (k <= 1) {
                if (!diskManager_.isMounted()) {
                    setNotice("Insert DISK !");
                    return true;
                }
                if (diskManager_.isWriteProtected()) {
                    setNotice("Disk Write Protected");
                    return true;
                }
                diskFormatConfirm_ = true;
            } else if (k == 2 || k == 3) {
                diskBackupStep_ = 0;
                screen_ = ClassicScreen::DiskBackupPrompt;
            } else if (k == 4 || k == 5) {
                screen_ = ClassicScreen::DiskStatus;
            } else if (k == 7) {
                screen_ = ClassicScreen::DiskRoot;
            }
            return true;

        case ClassicScreen::DiskStatus:
            if (k == 7) {
                screen_ = ClassicScreen::DiskJobMenu;
            }
            return true;

        case ClassicScreen::DiskBackupPrompt:
            if (k == 7) {
                screen_ = ClassicScreen::DiskJobMenu;
            }
            return true;

        case ClassicScreen::UtilMemoryProtect:
            if (k == 3 || k == 4) {
                memProtectCursor_ = 0;
            } else if (k == 5 || k == 6) {
                memProtectCursor_ = 1;
            } else if (k == 7) {
                screen_ = preUtilityScreen_;
            }
            return true;

        default:
            return handleEditSoftKey(k);
    }
}

bool VF50ClassicController::onKeypadDigit(int digit0To9) noexcept {
    const int d = std::clamp(digit0To9, 0, 9);
    clearNotice();

    if (directPcMode_ != 0) {
        if (directPcDigits_.size() < 3u) {
            directPcDigits_.push_back(static_cast<char>('0' + d));
        }
        return true;
    }

    if (instMinusArmed_ && (d == 7 || d == 8)) {
        // Manual p. 13: [-] + 7 enables Bank Hold, [-] + 8 disables Bank Hold
        bankHoldEnabled_ = (d == 7);
        instMinusArmed_  = false;
        setNotice(bankHoldEnabled_ ? "Bank Hold : on" : "Bank Hold : off", 45);
        return true;
    }
    instMinusArmed_ = false;

    if (screen_ == ClassicScreen::PlaySingle || screen_ == ClassicScreen::PlayPerformance) {
        const bool combineOn = processor_.classicSystemSettings().combineWithFunction;
        if (pendingBankSelect_ >= 0) {
            if (pendingTensDigit_ < 0) {
                pendingTensDigit_ = d;
                return true;
            }
            const std::size_t patchNum = static_cast<std::size_t>(pendingTensDigit_ * 10 + d);
            const auto bank = (pendingBankSelect_ == 2)
                                  ? vf50::state::MemoryBank::FactoryRom
                                  : vf50::state::MemoryBank::UserRam;
            cardBankActive_ = (pendingBankSelect_ == 1);
            clearPendingBankDigits();
            if (screen_ == ClassicScreen::PlayPerformance) {
                resetPerfInstrumentMutes();
                selectPerfWithCombine(processor_, cardBankActive_, bank, patchNum, combineOn, combineBorrowedFunction_);
            } else {
                selectSingleWithCombine(processor_, cardBankActive_, bank, patchNum, combineOn, combineBorrowedFunction_);
            }
            return true;
        }

        if (bankHoldEnabled_) {
            pendingTensDigit_ = -1;
            if (screen_ == ClassicScreen::PlayPerformance) {
                const std::size_t curIdx = processor_.hostMemory().selectedPerfIndex();
                const std::size_t patchNum = (curIdx / 10u) * 10u + static_cast<std::size_t>(d);
                if (patchNum != curIdx) {
                    resetPerfInstrumentMutes();
                }
                selectPerfWithCombine(processor_, cardBankActive_, processor_.hostMemory().selectedPerfBank(),
                                      patchNum, combineOn, combineBorrowedFunction_);
            } else {
                const std::size_t curIdx = processor_.hostMemory().selectedVoiceIndex();
                const std::size_t patchNum = (curIdx / 10u) * 10u + static_cast<std::size_t>(d);
                selectSingleWithCombine(processor_, cardBankActive_, processor_.hostMemory().selectedVoiceBank(),
                                        patchNum, combineOn, combineBorrowedFunction_);
            }
            return true;
        }

        if (pendingTensDigit_ < 0) {
            pendingTensDigit_ = d;
        } else {
            const std::size_t patchNum = static_cast<std::size_t>(pendingTensDigit_ * 10 + d);
            pendingTensDigit_ = -1;
            if (screen_ == ClassicScreen::PlayPerformance) {
                resetPerfInstrumentMutes();
                selectPerfWithCombine(processor_, cardBankActive_, processor_.hostMemory().selectedPerfBank(),
                                      patchNum, combineOn, combineBorrowedFunction_);
            } else {
                selectSingleWithCombine(processor_, cardBankActive_, processor_.hostMemory().selectedVoiceBank(),
                                        patchNum, combineOn, combineBorrowedFunction_);
            }
        }
        return true;
    }

    if (screen_ == ClassicScreen::SeqPlaySong) {
        if (playSongCursorField_ == 0 && d >= 1 && d <= 8) {
            const int targetSlot = d - 1;
            if (songSlots_[static_cast<std::size_t>(targetSlot)].hasSetupData && targetSlot != activeSongSlot_) {
                pendingSongSlot_ = targetSlot;
                screen_ = ClassicScreen::SeqSongSetupPrompt;
            } else {
                selectSeqSongSlot(targetSlot, false);
            }
            return true;
        }
        if (playSongCursorField_ == 1 && d >= 1) {
            processor_.requestDemoSeekToMeasure(d);
            return true;
        }
        return false;
    }

    if (screen_ == ClassicScreen::DiskDirectory || screen_ == ClassicScreen::DiskFileAction) {
        if (d >= 1) {
            const int maxCount = (screen_ == ClassicScreen::DiskDirectory)
                                     ? static_cast<int>(classicDirectoryEntryIndices().size())
                                     : static_cast<int>(filteredDiskEntryIndices(diskDataType_).size());
            if (maxCount > 0) {
                diskFileIndex_ = std::clamp(d - 1, 0, maxCount - 1);
            }
        }
        return true;
    }

    if (isRhythmScreen(screen_)) {
        return handleRhythmDigit(d);
    }
    if (isPhase3UtilScreen(screen_)) {
        return handleUtilDigit(d);
    }

    return handleEditDigit(d);
}

bool VF50ClassicController::executeConfirmedDiskOperation() noexcept {
    juce::String status;
    if (diskOp_ == DiskOperation::Save) {
        std::string stem;
        for (char c : diskEditName_) {
            stem.push_back(c);
        }
        while (!stem.empty() && stem.back() == ' ') {
            stem.pop_back();
        }
        if (stem.empty()) {
            stem = "NewFile";
        }
        vf50::disk::VF50FileKind k = vf50::disk::VF50FileKind::AllData;
        if (diskDataType_ == DiskDataType::Syn)       k = vf50::disk::VF50FileKind::VoiceBank;
        else if (diskDataType_ == DiskDataType::Seq)  k = vf50::disk::VF50FileKind::Sequence;
        else if (diskDataType_ == DiskDataType::RSeq) k = vf50::disk::VF50FileKind::RhythmData;
        else if (diskDataType_ == DiskDataType::Card) k = vf50::disk::VF50FileKind::CardData;

        bool routedToUserDisk = false;
        const bool ok = diskManager_.saveTypedToMountedSource(stem, k, processor_, status, &routedToUserDisk);
        diskActionConfirm_ = false;
        diskActionOverwrite_ = false;
        diskNameFreshDefault_ = false;
        setNotice(ok ? (routedToUserDisk ? "Saved -> User Disk" : "Completed !") : status.toStdString(),
                  routedToUserDisk ? 90 : 60);
        return ok;
    }

    const auto matches = filteredDiskEntryIndices(diskDataType_);
    if (matches.empty()) {
        diskActionConfirm_ = false;
        setNotice("No Matching File");
        return false;
    }
    const std::size_t safeIdx = matches[static_cast<std::size_t>(std::clamp(diskFileIndex_, 0, static_cast<int>(matches.size()) - 1))];

    if (diskOp_ == DiskOperation::Load) {
        diskActionConfirm_ = false;
        // Manual p. 105: Memory Protect blocks ALL, SYN, R.SEQ (and CARD protect blocks CARD); SEQ is exempt!
        if (diskDataType_ == DiskDataType::Card) {
            if (processor_.isCardMemoryProtected()
                || processor_.virtualCard().isReadOnlyMedia()
                || processor_.virtualCard().hardwareWriteProtect) {
                setNotice("Memory Protected");
                return false;
            }
        } else if (diskDataType_ != DiskDataType::Seq && processor_.isIntMemoryProtected()) {
            setNotice("Memory Protected");
            return false;
        }

        vf50::disk::VF50LoadFilter filt = vf50::disk::VF50LoadFilter::AllData;
        if (diskDataType_ == DiskDataType::Syn)       filt = vf50::disk::VF50LoadFilter::SynthOnly;
        else if (diskDataType_ == DiskDataType::Seq)  filt = vf50::disk::VF50LoadFilter::SequenceOnly;
        else if (diskDataType_ == DiskDataType::RSeq) filt = vf50::disk::VF50LoadFilter::RhythmOnly;
        else if (diskDataType_ == DiskDataType::Card) filt = vf50::disk::VF50LoadFilter::CardOnly;

        const bool ok = diskManager_.loadMountedEntry(safeIdx, processor_, filt, status);
        if (ok) {
            if (diskDataType_ == DiskDataType::Seq) {
                // SEQ (.A##) loads strictly into the currently selected song slot (1..8)
                syncSongSlotTitlesFromProcessor();
                syncActiveRhythmSongFromSeqSlot(activeSongSlot_);
            } else if (diskDataType_ == DiskDataType::All) {
                for (auto& sl : songSlots_) {
                    sl.name = { ' ','N','e','w','S','o','n','g' };
                    sl.populatedFromDisk = false;
                    sl.hasSetupData = false;
                }
                const int firstPop = processor_.firstPopulatedUserSeqSlot();
                activeSongSlot_ = (firstPop >= 0) ? firstPop : 0;
                syncSongSlotTitlesFromProcessor();

                const auto& mem = processor_.hostMemory();
                rhyPtnIsUser_ = (mem.selectedRhythmPatternBank() == vf50::state::MemoryBank::UserRam);
                rhyPtnIdx_    = static_cast<int>(mem.selectedRhythmPatternIndex());

                syncActiveRhythmSongFromSeqSlot(activeSongSlot_);
                if (firstPop < 0 && rhythmSongs_ != nullptr && !(*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)].hasData()) {
                    for (int s = 0; s < 8; ++s) {
                        if ((*rhythmSongs_)[static_cast<std::size_t>(s)].hasData()) {
                            activeRhythmSong_ = s;
                            break;
                        }
                    }
                }
            } else if (diskDataType_ == DiskDataType::RSeq) {
                const auto& mem = processor_.hostMemory();
                rhyPtnIsUser_ = (mem.selectedRhythmPatternBank() == vf50::state::MemoryBank::UserRam);
                rhyPtnIdx_    = static_cast<int>(mem.selectedRhythmPatternIndex());
                if (rhythmSongs_ != nullptr && !(*rhythmSongs_)[static_cast<std::size_t>(activeRhythmSong_)].hasData()) {
                    for (int s = 0; s < 8; ++s) {
                        if ((*rhythmSongs_)[static_cast<std::size_t>(s)].hasData()) {
                            activeRhythmSong_ = s;
                            break;
                        }
                    }
                }
            }
        }
        setNotice(ok ? "Completed !" : status.toStdString());
        return ok;
    }

    if (diskOp_ == DiskOperation::Delete) {
        const bool ok = diskManager_.deleteMountedEntry(safeIdx, status);
        diskActionConfirm_ = false;
        diskFileIndex_ = 0;
        setNotice(ok ? "Completed !" : status.toStdString());
        return ok;
    }

    if (diskOp_ == DiskOperation::Rename) {
        std::string stem;
        for (char c : diskEditName_) {
            stem.push_back(c);
        }
        while (!stem.empty() && stem.back() == ' ') {
            stem.pop_back();
        }
        if (stem.empty()) {
            stem = "RENAMED";
        }
        const bool ok = diskManager_.renameMountedEntry(safeIdx, stem, status);
        diskActionConfirm_ = false;
        setNotice(ok ? "Completed !" : status.toStdString());
        return ok;
    }
    return false;
}

bool VF50ClassicController::executeConfirmedSeqEdit() noexcept {
    seqEditConfirm_ = false;
    setNotice("Completed !", 45);
    return true;
}

bool VF50ClassicController::onKeypadYes() noexcept {
    clearPendingBankDigits();
    instMinusArmed_ = false;

    if (screen_ == ClassicScreen::SeqStoreSetupConfirm) {
        processor_.pullLatestAudioMemoryToHost();
        const auto& mem = processor_.hostMemory();
        auto& slot = songSlots_[static_cast<std::size_t>(activeSongSlot_)];
        slot.hasSetupData   = true;
        const auto ws       = mem.workstationMode();
        slot.setupPlayMode  = (ws == vf50::state::WorkstationMode::SinglePlay || ws == vf50::state::WorkstationMode::SingleEdit) ? 0u : 1u;
        slot.setupPerfBank  = (mem.selectedPerfBank() == vf50::state::MemoryBank::UserRam) ? 0u : 1u;
        slot.setupPerfNum   = static_cast<uint8_t>(mem.selectedPerfIndex());
        slot.setupVoiceBank = (mem.selectedVoiceBank() == vf50::state::MemoryBank::UserRam) ? 0u : 1u;
        slot.setupVoiceNum  = static_cast<uint8_t>(mem.selectedVoiceIndex());
        setNotice("Completed !", 45);
        screen_ = ClassicScreen::SeqPlaySong;
        return true;
    }

    if (screen_ == ClassicScreen::SeqJobSong && songClearConfirm_) {
        songClearConfirm_ = false;
        auto& slot = songSlots_[static_cast<std::size_t>(activeSongSlot_)];
        slot.name = { ' ','N','e','w','S','o','n','g' };
        slot.populatedFromDisk = false;
        slot.hasSetupData = false;
        processor_.clearUserSequenceSlot(static_cast<std::size_t>(activeSongSlot_));
        processor_.requestDemoRewindToTop();
        setNotice("Completed !", 45);
        return true;
    }

    if (screen_ == ClassicScreen::SeqJobEditSub && seqEditConfirm_) {
        return executeConfirmedSeqEdit();
    }

    if (screen_ == ClassicScreen::DiskFileAction && diskActionConfirm_) {
        return executeConfirmedDiskOperation();
    }

    if (screen_ == ClassicScreen::DiskJobMenu && diskFormatConfirm_) {
        diskFormatConfirm_ = false;
        juce::String st;
        const bool ok = diskManager_.formatMountedDisk(st);
        setNotice(ok ? "Completed !" : st.toStdString());
        return ok;
    }

    if (screen_ == ClassicScreen::DiskBackupPrompt) {
        if (diskBackupStep_ < 3) {
            ++diskBackupStep_;
        } else {
            screen_ = ClassicScreen::DiskJobMenu;
        }
        return true;
    }

    if (isRhythmScreen(screen_)) {
        if (handleRhythmYes()) {
            return true;
        }
        return onStepValue(+1);
    }
    if (isPhase3UtilScreen(screen_)) {
        return handleUtilYes();
    }

    if (handleEditYes()) {
        return true;
    }

    return onStepValue(+1);
}

bool VF50ClassicController::onKeypadNo() noexcept {
    clearPendingBankDigits();
    clearNotice();
    instMinusArmed_ = false;

    if (screen_ == ClassicScreen::SeqStoreSetupConfirm) {
        screen_ = ClassicScreen::SeqPlaySong;
        return true;
    }
    if (screen_ == ClassicScreen::SeqSongSetupPrompt) {
        screen_ = ClassicScreen::SeqPlaySong;
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobSong && songClearConfirm_) {
        songClearConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::SeqJobEditSub && seqEditConfirm_) {
        seqEditConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::DiskFileAction && diskActionConfirm_) {
        diskActionConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::DiskJobMenu && diskFormatConfirm_) {
        diskFormatConfirm_ = false;
        return true;
    }
    if (screen_ == ClassicScreen::DiskBackupPrompt) {
        screen_ = ClassicScreen::DiskJobMenu;
        return true;
    }

    if (isRhythmScreen(screen_)) {
        if (handleRhythmNo()) {
            return true;
        }
        return onStepValue(-1);
    }
    if (isPhase3UtilScreen(screen_)) {
        return handleUtilNo();
    }

    if (handleEditNo()) {
        return true;
    }

    return onStepValue(-1);
}

void VF50ClassicController::adjustFilenameChar(int delta) noexcept {
    diskNameFreshDefault_ = false;
    const int col = std::clamp(diskNameCursorCol_, 0, 7);
    diskNameCursorCol_ = col;
    int cur = static_cast<unsigned char>(diskEditName_[static_cast<std::size_t>(col)]);
    cur = std::clamp(cur + delta, 0x20, 0x7A);
    diskEditName_[static_cast<std::size_t>(col)] = static_cast<char>(cur);
}

void VF50ClassicController::syncSeqSlotNameToAudio() noexcept {
    const auto slotIdx = static_cast<std::size_t>(std::clamp(activeSongSlot_, 0, 7));
    if (!processor_.hasUserSequenceInSlot(slotIdx)) {
        return;
    }
    const auto* existing = processor_.userSequenceInSlot(slotIdx);
    if (existing == nullptr) {
        return;
    }
    auto copy = std::make_unique<vf50::dsp::CustomSeqSongBuffer>(*existing);
    const auto& nm = songSlots_[slotIdx].name;
    copy->setTitle(std::string_view(nm.data(), nm.size()));
    processor_.publishUserSequenceToSlot(slotIdx, *copy);
}

void VF50ClassicController::adjustSongNameChar(int delta) noexcept {
    const int col = std::clamp(songNameCursorCol_, 0, 7);
    songNameCursorCol_ = col;
    auto& nm = songSlots_[static_cast<std::size_t>(activeSongSlot_)].name;
    int cur = static_cast<unsigned char>(nm[static_cast<std::size_t>(col)]);
    cur = std::clamp(cur + delta, 0x20, 0x7A);
    nm[static_cast<std::size_t>(col)] = static_cast<char>(cur);
    syncSeqSlotNameToAudio();
}

bool VF50ClassicController::onStepValue(int deltaSteps) noexcept {
    if (deltaSteps == 0) {
        return false;
    }
    const int stepSign = (deltaSteps > 0) ? 1 : -1;
    clearNotice();

    if (isRhythmScreen(screen_)) {
        return handleRhythmStepValue(deltaSteps);
    }
    if (isPhase3UtilScreen(screen_)) {
        return handleUtilStepValue(deltaSteps);
    }

    switch (screen_) {
        case ClassicScreen::PlaySingle: {
            const auto bank = processor_.hostMemory().selectedVoiceBank();
            const int curIdx = static_cast<int>(processor_.hostMemory().selectedVoiceIndex());
            const int nextIdx = std::clamp(curIdx + deltaSteps, 0, 99);
            const bool combineOn = processor_.classicSystemSettings().combineWithFunction;
            selectSingleWithCombine(processor_, cardBankActive_, bank, static_cast<std::size_t>(nextIdx),
                                    combineOn, combineBorrowedFunction_);
            return true;
        }

        case ClassicScreen::PlayPerformance: {
            const auto bank = processor_.hostMemory().selectedPerfBank();
            const int curIdx = static_cast<int>(processor_.hostMemory().selectedPerfIndex());
            const int nextIdx = std::clamp(curIdx + deltaSteps, 0, 99);
            if (nextIdx != curIdx) {
                resetPerfInstrumentMutes(); // Manual p. 20: switching perf resets mutes
            }
            const bool combineOn = processor_.classicSystemSettings().combineWithFunction;
            selectPerfWithCombine(processor_, cardBankActive_, bank, static_cast<std::size_t>(nextIdx),
                                  combineOn, combineBorrowedFunction_);
            return true;
        }

        case ClassicScreen::SeqPlaySong: {
            if (playSongCursorField_ == 0) {
                const int target = std::clamp(activeSongSlot_ + stepSign, 0, 7);
                if (target != activeSongSlot_) {
                    if (songSlots_[static_cast<std::size_t>(target)].hasSetupData) {
                        pendingSongSlot_ = target;
                        screen_ = ClassicScreen::SeqSongSetupPrompt;
                    } else {
                        selectSeqSongSlot(target, false);
                    }
                }
            } else if (playSongCursorField_ == 1) {
                processor_.requestDemoSeekMeasures(deltaSteps);
            } else {
                const int nextBpm = std::clamp(processor_.activeDemoTempoBpm() + deltaSteps, 30, 240);
                processor_.requestDemoSetTempoBpm(nextBpm);
            }
            return true;
        }

        case ClassicScreen::SeqJobSetup:
            if (seqSetupCursorField_ == 0)      seqRch_ = std::clamp(seqRch_ + stepSign, 0, 17);
            else if (seqSetupCursorField_ == 1) seqAftertouch_ = (stepSign > 0);
            else if (seqSetupCursorField_ == 2) seqVelocity_ = (stepSign > 0);
            else if (seqSetupCursorField_ == 3) seqClickMode_ = std::clamp(seqClickMode_ + stepSign, 0, 3);
            else                                seqSyncMidi_ = (stepSign > 0);
            return true;

        case ClassicScreen::SeqJobTransmitCh: {
            auto& ch = songSlots_[static_cast<std::size_t>(activeSongSlot_)].trackTxCh[static_cast<std::size_t>(tchCursorTrack_)];
            ch = static_cast<uint8_t>(std::clamp(static_cast<int>(ch) + stepSign, 0, 16));
            return true;
        }

        case ClassicScreen::SeqJobSong:
            adjustSongNameChar(stepSign);
            return true;

        case ClassicScreen::SeqJobEditSub:
            switch (seqEditSubKind_) {
                case SeqEditSubKind::Mix:
                    if (seqEditCursorField_ <= 1)      mixSrcA_ = std::clamp(mixSrcA_ + stepSign, 1, 8);
                    else if (seqEditCursorField_ <= 3) mixSrcB_ = std::clamp(mixSrcB_ + stepSign, 1, 8);
                    else                               mixDst_  = std::clamp(mixDst_ + stepSign, 1, 8);
                    break;
                case SeqEditSubKind::Quantize:
                    if (seqEditCursorField_ <= 3) quantizeTrk_     = std::clamp(quantizeTrk_ + stepSign, 1, 8);
                    else                          quantizeSizeIdx_ = std::clamp(quantizeSizeIdx_ + stepSign, 0, 6);
                    break;
                case SeqEditSubKind::DeleteMeas:
                    if (seqEditCursorField_ <= 1) delFromMeas_ = std::clamp(delFromMeas_ + stepSign, 1, 999);
                    else                          delToMeas_   = std::clamp(delToMeas_ + stepSign, delFromMeas_, 999);
                    delToMeas_ = std::max(delToMeas_, delFromMeas_);
                    break;
                case SeqEditSubKind::InsertMeas:
                    if (seqEditCursorField_ <= 1)      insFromMeas_ = std::clamp(insFromMeas_ + stepSign, 1, 999);
                    else if (seqEditCursorField_ <= 3) insToMeas_   = std::clamp(insToMeas_ + stepSign, insFromMeas_, 999);
                    else                               insCount_    = std::clamp(insCount_ + stepSign, 1, 99);
                    insToMeas_ = std::max(insToMeas_, insFromMeas_);
                    break;
                case SeqEditSubKind::CopyMeas:
                    if (seqEditCursorField_ == 0)      copySrcTrk_   = std::clamp(copySrcTrk_ + stepSign, 1, 8);
                    else if (seqEditCursorField_ == 1) copyFromMeas_ = std::clamp(copyFromMeas_ + stepSign, 1, 999);
                    else if (seqEditCursorField_ == 2) copyToMeas_   = std::clamp(copyToMeas_ + stepSign, copyFromMeas_, 999);
                    else if (seqEditCursorField_ <= 4) copyDstTrk_   = std::clamp(copyDstTrk_ + stepSign, 1, 8);
                    else                               copyDstMeas_  = std::clamp(copyDstMeas_ + stepSign, 1, 999);
                    copyToMeas_ = std::max(copyToMeas_, copyFromMeas_);
                    break;
                case SeqEditSubKind::EraseMeas:
                    if (seqEditCursorField_ <= 1)      eraseTrk_      = std::clamp(eraseTrk_ + stepSign, 1, 9);
                    else if (seqEditCursorField_ <= 3) eraseFromMeas_ = std::clamp(eraseFromMeas_ + stepSign, 1, 999);
                    else                               eraseToMeas_   = std::clamp(eraseToMeas_ + stepSign, eraseFromMeas_, 999);
                    eraseToMeas_ = std::max(eraseToMeas_, eraseFromMeas_);
                    break;
                case SeqEditSubKind::RemoveEvent:
                    if (seqEditCursorField_ == 0)      remvEventType_ = std::clamp(remvEventType_ + stepSign, 0, 2);
                    else if (seqEditCursorField_ == 1) remvRangeLo_   = std::clamp(remvRangeLo_ + stepSign, 0, 127);
                    else if (seqEditCursorField_ == 2) remvRangeHi_   = std::clamp(remvRangeHi_ + stepSign, remvRangeLo_, 127);
                    else if (seqEditCursorField_ == 3) remvTrk_       = std::clamp(remvTrk_ + stepSign, 1, 9);
                    else if (seqEditCursorField_ == 4) remvFromMeas_  = std::clamp(remvFromMeas_ + stepSign, 1, 999);
                    else                               remvToMeas_    = std::clamp(remvToMeas_ + stepSign, remvFromMeas_, 999);
                    remvToMeas_ = std::max(remvToMeas_, remvFromMeas_);
                    break;
            }
            return true;

        case ClassicScreen::SeqJobRecMode:
            if (recModeCursor_ == 0) {
                const int m = std::clamp(static_cast<int>(seqRecMode_) + stepSign, 0, 2);
                seqRecMode_ = static_cast<SeqRecMode>(m);
                if (seqRecMode_ == SeqRecMode::Step)       seqRecCondition_ = SeqRecCondition::Overdub;
                else if (seqRecMode_ == SeqRecMode::Punch) seqRecCondition_ = SeqRecCondition::Replace;
            } else {
                seqRecCondition_ = (stepSign > 0) ? SeqRecCondition::Overdub : SeqRecCondition::Replace;
            }
            return true;

        case ClassicScreen::SeqRecStandby:
            if (recStandbyCursor_ == 0) {
                seqRecCondition_ = (stepSign > 0) ? SeqRecCondition::Overdub : SeqRecCondition::Replace;
            } else if (recStandbyCursor_ == 1) {
                recVoiceAssign_ = std::clamp(recVoiceAssign_ + stepSign, 0, 2);
            } else if (seqRecMode_ == SeqRecMode::Punch) {
                if (recStandbyCursor_ == 2) {
                    recMeas_ = std::clamp(recMeas_ + stepSign, 1, 999);
                    recPunchIn_  = std::max(recPunchIn_, recMeas_);
                    recPunchOut_ = std::max(recPunchOut_, recPunchIn_);
                } else if (recStandbyCursor_ == 3) {
                    recPunchIn_  = std::clamp(recPunchIn_ + stepSign, recMeas_, 999);
                    recPunchOut_ = std::max(recPunchOut_, recPunchIn_);
                } else {
                    recPunchOut_ = std::clamp(recPunchOut_ + stepSign, recPunchIn_, 999);
                }
            } else {
                if (recStandbyCursor_ == 3) {
                    recMeas_ = std::clamp(recMeas_ + stepSign, 1, 999);
                } else if (recStandbyCursor_ == 4) {
                    const int nextBpm = std::clamp(processor_.activeDemoTempoBpm() + deltaSteps, 30, 240);
                    processor_.requestDemoSetTempoBpm(nextBpm);
                }
            }
            return true;

        case ClassicScreen::DiskFileAction:
            if ((diskOp_ == DiskOperation::Save || diskOp_ == DiskOperation::Rename) && diskEditingName_) {
                adjustFilenameChar(stepSign);
            } else {
                const int maxCount = static_cast<int>(filteredDiskEntryIndices(diskDataType_).size());
                if (maxCount > 0) {
                    diskFileIndex_ = std::clamp(diskFileIndex_ + deltaSteps, 0, maxCount - 1);
                }
            }
            return true;

        case ClassicScreen::DiskMdrIntTime:
            diskMdrIntTime_ = std::clamp(diskMdrIntTime_ + deltaSteps, 1, 9);
            return true;

        case ClassicScreen::DiskDirectory: {
            const int maxCount = static_cast<int>(classicDirectoryEntryIndices().size());
            if (maxCount > 0) {
                diskFileIndex_ = std::clamp(diskFileIndex_ + deltaSteps, 0, maxCount - 1);
            }
            return true;
        }

        case ClassicScreen::UtilMemoryProtect:
            if (memProtectCursor_ == 0) {
                processor_.setIntMemoryProtected(stepSign > 0);
            } else {
                processor_.setCardMemoryProtected(stepSign > 0);
            }
            return true;

        default:
            return handleEditStepValue(deltaSteps);
    }
}

void VF50ClassicController::onDataEntryNormalized(float norm0To1) noexcept {
    const float clamped = std::clamp(norm0To1, 0.0f, 1.0f);
    if (screen_ == ClassicScreen::PlaySingle) {
        const auto target = static_cast<std::size_t>(std::lround(clamped * 99.0f));
        const bool combineOn = processor_.classicSystemSettings().combineWithFunction;
        selectSingleWithCombine(processor_, cardBankActive_, processor_.hostMemory().selectedVoiceBank(),
                                target, combineOn, combineBorrowedFunction_);
    } else if (screen_ == ClassicScreen::PlayPerformance) {
        const auto target = static_cast<std::size_t>(std::lround(clamped * 99.0f));
        if (target != processor_.hostMemory().selectedPerfIndex()) {
            resetPerfInstrumentMutes();
        }
        const bool combineOn = processor_.classicSystemSettings().combineWithFunction;
        selectPerfWithCombine(processor_, cardBankActive_, processor_.hostMemory().selectedPerfBank(),
                              target, combineOn, combineBorrowedFunction_);
    } else if (screen_ == ClassicScreen::SeqPlaySong) {
        if (playSongCursorField_ == 0) {
            selectSeqSongSlot(static_cast<int>(std::lround(clamped * 7.0f)), false);
        } else if (playSongCursorField_ == 1) {
            const int totM = std::max(1, processor_.activeDemoTotalMeasures());
            processor_.requestDemoSeekToMeasure(1 + static_cast<int>(std::lround(clamped * static_cast<float>(totM - 1))));
        } else {
            processor_.requestDemoSetTempoBpm(30 + static_cast<int>(std::lround(clamped * 210.0f)));
        }
    } else if (screen_ == ClassicScreen::DiskMdrIntTime) {
        diskMdrIntTime_ = 1 + static_cast<int>(std::lround(clamped * 8.0f));
    } else if (screen_ == ClassicScreen::UtilMemoryProtect) {
        const bool on = (clamped >= 0.5f);
        if (memProtectCursor_ == 0) processor_.setIntMemoryProtected(on);
        else                        processor_.setCardMemoryProtected(on);
    } else if (screen_ == ClassicScreen::DiskDirectory) {
        const auto dirIdx = classicDirectoryEntryIndices();
        if (!dirIdx.empty()) {
            diskFileIndex_ = static_cast<int>(std::lround(clamped * static_cast<float>(dirIdx.size() - 1u)));
        }
    } else if (screen_ == ClassicScreen::DiskFileAction) {
        const auto matches = filteredDiskEntryIndices(diskDataType_);
        if (!matches.empty() && !diskEditingName_) {
            diskFileIndex_ = static_cast<int>(std::lround(clamped * static_cast<float>(matches.size() - 1u)));
        }
    } else if (isRhythmScreen(screen_)) {
        handleRhythmDataEntry(clamped);
    } else if (isPhase3UtilScreen(screen_)) {
        handleUtilDataEntry(clamped);
    } else {
        handleEditDataEntry(clamped);
    }
}

bool VF50ClassicController::matchesDiskType(const vf50::disk::DiskFileEntry& entry, DiskDataType type) const noexcept {
    using vf50::disk::VF50DiskManager;
    using vf50::disk::VF50FileKind;
    // Strict hardware extension filtering per VF50 IC712 $BBAF "VIARCB":
    //   ALL=.V01-.V99, SYN=.I01-.I99, SEQ=.A01-.A99, R.SEQ=.R01-.R99, CARD=.C01-.C99
    switch (type) {
        case DiskDataType::All:  return VF50DiskManager::isHardwareExtensionForKind(entry.name, VF50FileKind::AllData);
        case DiskDataType::Syn:  return VF50DiskManager::isHardwareExtensionForKind(entry.name, VF50FileKind::VoiceBank);
        case DiskDataType::Seq:  return VF50DiskManager::isHardwareExtensionForKind(entry.name, VF50FileKind::Sequence);
        case DiskDataType::RSeq: return VF50DiskManager::isHardwareExtensionForKind(entry.name, VF50FileKind::RhythmData);
        case DiskDataType::Card: return VF50DiskManager::isHardwareExtensionForKind(entry.name, VF50FileKind::CardData);
    }
    return false;
}

std::vector<std::size_t> VF50ClassicController::filteredDiskEntryIndices(DiskDataType type) const {
    std::vector<std::size_t> out;
    const auto& ents = diskManager_.entries();
    for (std::size_t i = 0u; i < ents.size(); ++i) {
        if (matchesDiskType(ents[i], type)) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<std::size_t> VF50ClassicController::classicDirectoryEntryIndices() const {
    std::vector<std::size_t> out;
    const auto& ents = diskManager_.entries();
    for (std::size_t i = 0u; i < ents.size(); ++i) {
        if (vf50::disk::VF50DiskManager::classifyFileKind(ents[i].name, true) != vf50::disk::VF50FileKind::Unknown) {
            out.push_back(i);
        }
    }
    return out;
}

std::string_view VF50ClassicController::diskTypeTag(DiskDataType type) noexcept {
    switch (type) {
        case DiskDataType::All:  return "ALL";
        case DiskDataType::Syn:  return "SYN";
        case DiskDataType::Seq:  return "SEQ";
        case DiskDataType::RSeq: return "R.SEQ";
        case DiskDataType::Card: return "CARD";
    }
    return "ALL";
}

std::string_view VF50ClassicController::diskEntryShortType(const vf50::disk::DiskFileEntry& entry) noexcept {
    using vf50::disk::VF50FileKind;
    switch (entry.kind) {
        case VF50FileKind::AllData:    return "ALL";
        case VF50FileKind::VoiceBank:
        case VF50FileKind::PerfBank:   return "SYN";
        case VF50FileKind::Sequence:   return "SEQ";
        case VF50FileKind::RhythmData: return "R.SEQ";
        case VF50FileKind::CardData:   return "CARD";
        case VF50FileKind::SysEx:      return "MDR";
        default:                      return "ETC";
    }
}

ClassicLcdFrame VF50ClassicController::renderLcdFrame() const noexcept {
    ClassicLcdFrame frame{};
    clearLine(frame.line0);
    clearLine(frame.line1);

    processor_.pullLatestAudioMemoryToHost();
    const auto& mem     = processor_.hostMemory();
    const auto& voice   = mem.activeSingleVoice();
    const auto& perf    = mem.activePerformance();
    const bool bypassed = (processor_.parameterBank().effectBypass->load(std::memory_order_relaxed) >= 0.5f);
    const bool cardIns  = cardBankActive_;
    char tmp[80]{};

    switch (screen_) {
        case ClassicScreen::PowerOnGreeting: {
            // Spec §4.2 (pp. 11, 22): Neutral Victory VF50 / VF50 Line 1 + user-editable 40-char Line 2
            setLineFormatted(frame.line0, "******     Victory VF50 Synth     ******");
            frame.line1 = processor_.classicSystemSettings().powerOnMessage;
            break;
        }

        case ClassicScreen::PlaySingle: {
            if (directPcMode_ != 0) {
                const char c0 = (directPcDigits_.size() >= 1u) ? directPcDigits_[0] : '-';
                const char c1 = (directPcDigits_.size() >= 2u) ? directPcDigits_[1] : '-';
                const char c2 = (directPcDigits_.size() >= 3u) ? directPcDigits_[2] : '-';
                std::snprintf(tmp, sizeof(tmp), "** Sending PC No %c%c%c                    ", c0, c1, c2);
                setLineFormatted(frame.line0, tmp);
                clearLine(frame.line1);
                break;
            }
            // Spec §1.2 (pp. 12-13, 18):
            // "PLAY SINGLE      EFCT=RevHall: 77 Pb= 2 "
            // "I00  Strings 1   Fs=sus   [TrPoFcMwBcAt]"
            const auto fx7 = formatEffectShort7(processor_, voice.effect.effectType, bypassed);
            const int fxVal = (voice.effect.effectType == 0u || bypassed)
                                  ? 0
                                  : std::clamp(static_cast<int>(voice.effect.stereoMix != 0u ? voice.effect.outLevel : voice.effect.balance), 0, 99);
            const int pb = std::clamp(static_cast<int>(voice.pitchBendRange), 0, 12);
            std::snprintf(tmp, sizeof(tmp), "PLAY SINGLE      EFCT=%.7s:%3d Pb=%2d ",
                          fx7.data(), fxVal, pb);
            setLineFormatted(frame.line0, tmp);

            char bankChar = bankLetter(mem.selectedVoiceBank(), cardIns);
            char d0 = '0', d1 = '0';
            if (pendingBankSelect_ >= 0) {
                bankChar = (pendingBankSelect_ == 2) ? 'P' : (pendingBankSelect_ == 1 ? 'C' : 'I');
                if (pendingTensDigit_ < 0) {
                    d0 = '?';
                    d1 = '?';
                } else {
                    d0 = static_cast<char>('0' + pendingTensDigit_);
                    d1 = '?';
                }
            } else if (pendingTensDigit_ >= 0) {
                d0 = static_cast<char>('0' + pendingTensDigit_);
                d1 = '?';
            } else {
                const int idx = std::clamp(static_cast<int>(mem.selectedVoiceIndex()), 0, 99);
                d0 = static_cast<char>('0' + (idx / 10));
                d1 = static_cast<char>('0' + (idx % 10));
            }

            const char* flgTr = (voice.transpose != 0) ? "Tr" : "  ";
            const char* flgPo = (voice.portaSwitch != 0u && voice.portaTime > 0u) ? "Po" : "  ";
            const char* flgFc = (voice.fcVolume > 0u || voice.fcPitch > 0u || voice.fcAmp > 0u) ? "Fc" : "  ";
            const char* flgMw = (voice.mwPitch > 0u || voice.mwAmp > 0u) ? "Mw" : "  ";
            const char* flgBc = (voice.bcPitch > 0u || voice.bcAmp > 0u || voice.bcPitchBias != 0 || voice.bcEgBias > 0u) ? "Bc" : "  ";
            const char* flgAt = (voice.atPitch > 0u || voice.atAmp > 0u || voice.atPitchBias != 0 || voice.atEgBias > 0u) ? "At" : "  ";

            char dispName[11]{ ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','\0' };
            for (std::size_t i = 0u; i < std::min<std::size_t>(voice.name.size(), 10u); ++i) {
                dispName[i] = voice.name[i];
            }
            if (combineBorrowedFunction_ && dispName[0] >= 'A' && dispName[0] <= 'Z') {
                dispName[0] = static_cast<char>(dispName[0] - 'A' + 'a');
            }

            std::snprintf(tmp, sizeof(tmp), "%c%c%c  %-10.10s  Fs=sus   [%s%s%s%s%s%s]",
                          bankChar, d0, d1,
                          dispName,
                          flgTr, flgPo, flgFc, flgMw, flgBc, flgAt);
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::PlayPerformance: {
            if (directPcMode_ != 0) {
                const char c0 = (directPcDigits_.size() >= 1u) ? directPcDigits_[0] : '-';
                const char c1 = (directPcDigits_.size() >= 2u) ? directPcDigits_[1] : '-';
                const char c2 = (directPcDigits_.size() >= 3u) ? directPcDigits_[2] : '-';
                std::snprintf(tmp, sizeof(tmp), "** Sending PC No %c%c%c                    ", c0, c1, c2);
                setLineFormatted(frame.line0, tmp);
                clearLine(frame.line1);
                break;
            }
            // Spec §1.3-1.4 (pp. 12, 19-20):
            // "PFP13 12stGuitar EFCT=E.Ref  : 80 Tch= 1"
            // " P50/ P51/ P51/ P51/  * /  * /  * /  *  "
            const auto fx7 = formatEffectShort7(processor_, perf.effect.effectType, bypassed);
            const int fxVal = (perf.effect.effectType == 0u || bypassed)
                                  ? 0
                                  : std::clamp(static_cast<int>(perf.effect.stereoMix != 0u ? perf.effect.outLevel : perf.effect.balance), 0, 99);
            const int tch = static_cast<int>(processor_.classicSystemSettings().transmitCh) + 1;

            char bankChar = bankLetter(mem.selectedPerfBank(), cardIns);
            char d0 = '0', d1 = '0';
            if (pendingBankSelect_ >= 0) {
                bankChar = (pendingBankSelect_ == 2) ? 'P' : (pendingBankSelect_ == 1 ? 'C' : 'I');
                d0 = (pendingTensDigit_ < 0) ? '?' : static_cast<char>('0' + pendingTensDigit_);
                d1 = '?';
            } else if (pendingTensDigit_ >= 0) {
                d0 = static_cast<char>('0' + pendingTensDigit_);
                d1 = '?';
            } else {
                const int idx = std::clamp(static_cast<int>(mem.selectedPerfIndex()), 0, 99);
                d0 = static_cast<char>('0' + (idx / 10));
                d1 = static_cast<char>('0' + (idx % 10));
            }

            char dispPerfName[11]{ ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','\0' };
            for (std::size_t i = 0u; i < std::min<std::size_t>(perf.name.size(), 10u); ++i) {
                dispPerfName[i] = perf.name[i];
            }
            if (combineBorrowedFunction_ && dispPerfName[0] >= 'A' && dispPerfName[0] <= 'Z') {
                dispPerfName[0] = static_cast<char>(dispPerfName[0] - 'A' + 'a');
            }

            std::snprintf(tmp, sizeof(tmp), "PF%c%c%c %-10.10s EFCT=%.7s:%3d Tch=%2d",
                          bankChar, d0, d1, dispPerfName, fx7.data(), fxVal, tch);
            setLineFormatted(frame.line0, tmp);

            // 8 x 5-char columns directly aligned with F1..F8
            clearLine(frame.line1);
            for (int p = 0; p < 8; ++p) {
                const auto& inst = perf.instruments[static_cast<std::size_t>(p)];
                const bool isMuted = ((perfInstMuteMask_ >> p) & 1u) != 0u;
                const bool isActive = isPerfInstActive(perf, p) && (inst.outAssign != 0u);
                const char sep = (p < 7) ? '/' : ' ';
                if (isMuted) {
                    std::snprintf(tmp, sizeof(tmp), " xxx%c", sep);
                } else if (!isActive) {
                    std::snprintf(tmp, sizeof(tmp), "  * %c", sep);
                } else {
                    const char vb = (inst.voiceBank == 0u) ? (cardIns ? 'C' : 'I') : 'P';
                    const int vn = std::clamp(static_cast<int>(inst.voiceNumber), 0, 99);
                    std::snprintf(tmp, sizeof(tmp), " %c%02d%c", vb, vn, sep);
                }
                writeSpan(frame.line1, p * 5, tmp, 5);
            }
            break;
        }

        case ClassicScreen::SeqPlaySong: {
            // Spec §2.1 (pp. 82, 87):
            // "PLAY SONG)          Time    >Meas>Tempo "
            // "▶1:Sunset           4/4      001   120  "
            const char mCur = (playSongCursorField_ == 1) ? kGlyphPlayArrow : '>';
            const char tCur = (playSongCursorField_ == 2) ? kGlyphPlayArrow : '>';
            std::snprintf(tmp, sizeof(tmp), "PLAY SONG)          Time    %cMeas%cTempo ", mCur, tCur);
            setLineFormatted(frame.line0, tmp);

            const char sCur = (playSongCursorField_ == 0) ? kGlyphPlayArrow : ' ';
            const auto& nm = songSlots_[static_cast<std::size_t>(activeSongSlot_)].name;
            const int tsN = std::clamp(processor_.activeDemoTimeSigNum(), 1, 16);
            const int tsD = std::clamp(processor_.activeDemoTimeSigDen(), 1, 16);
            const int meas = std::clamp(processor_.activeDemoMeasure(), 1, 999);
            const int bpm  = std::clamp(processor_.activeDemoTempoBpm(), 30, 240);
            std::array<char, 8> tsBuf{};
            tsBuf.fill(' ');
            std::snprintf(tsBuf.data(), tsBuf.size(), "%d/%d", tsN, tsD);
            for (std::size_t i = 0u; i < tsBuf.size(); ++i) {
                if (tsBuf[i] == '\0') tsBuf[i] = ' ';
            }
            std::snprintf(tmp, sizeof(tmp), "%c%d:%.8s         %.5s    %03d   %3d  ",
                          sCur, activeSongSlot_ + 1, nm.data(), tsBuf.data(), meas, bpm);
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::SeqSongSetupPrompt: {
            // Spec §2.4 (pp. 82, 84, 87)
            const auto& nm = songSlots_[static_cast<std::size_t>(pendingSongSlot_)].name;
            std::snprintf(tmp, sizeof(tmp), "SONG %d:%.8s     Load Setup Data ?   ", pendingSongSlot_ + 1, nm.data());
            setLineFormatted(frame.line0, tmp);
            setLineFormatted(frame.line1, " >with SETUP         >Song only    >EXIT");
            break;
        }

        case ClassicScreen::SeqStoreSetupConfirm: {
            // Spec §2.4 (p. 94)
            std::snprintf(tmp, sizeof(tmp), "STORE) %cMode           Are you sure ?   ", kGlyphPlayArrow);
            setLineFormatted(frame.line0, tmp);
            std::snprintf(tmp, sizeof(tmp), "Store SETUP data to song %d ?            ", activeSongSlot_ + 1);
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::SeqJobSelect: {
            // Spec §2.5 (p. 82)
            setLineFormatted(frame.line0, "SEQUENCER JOB SELECT)    Select one!    ");
            setLineFormatted(frame.line1, ">Setup>Tch>Song>Edit>Rec           >EXIT");
            break;
        }

        case ClassicScreen::SeqJobSetup: {
            // Spec §2.6 (pp. 88-89)
            auto mk = [&](int idx) -> char { return (seqSetupCursorField_ == idx) ? kGlyphPlayArrow : '>'; };
            std::snprintf(tmp, sizeof(tmp), "SETUP)%cRch%cA.T %cVel %cClick    %cSync     ",
                          mk(0), mk(1), mk(2), mk(3), mk(4));
            setLineFormatted(frame.line0, tmp);

            std::array<char, 4> rchStr{ { 'K','b','d',' ' } };
            if (seqRch_ == 17) {
                rchStr = { 'o','m','n',' ' };
            } else if (seqRch_ >= 1 && seqRch_ <= 16) {
                std::snprintf(tmp, sizeof(tmp), "%2d ", seqRch_);
                rchStr = { tmp[0], tmp[1], ' ', ' ' };
            }
            static constexpr std::array<const char*, 4> kClickNames = { "off  ", "Rec  ", "PlyRc", "Alwys" };
            const char* atStr   = seqAftertouch_ ? "on " : "off";
            const char* velStr  = seqVelocity_ ? "on " : "off";
            const char* clkStr  = kClickNames[static_cast<std::size_t>(std::clamp(seqClickMode_, 0, 3))];
            const char* syncStr = seqSyncMidi_ ? "MIDI" : "int ";
            std::snprintf(tmp, sizeof(tmp), "       %.3s %s   %s   %-5.5s    %-4.4s>EXIT",
                          rchStr.data(), atStr, velStr, clkStr, syncStr);
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::SeqJobTransmitCh: {
            // Spec §2.7 (p. 90)
            const auto ws = mem.workstationMode();
            const bool isSingle = (ws == vf50::state::WorkstationMode::SinglePlay || ws == vf50::state::WorkstationMode::SingleEdit);
            if (isSingle) {
                std::snprintf(tmp, sizeof(tmp), "TRANSMIT CHANNEL)  Single Mode  Rch = 1 ");
                setLineFormatted(frame.line0, tmp);
            } else {
                clearLine(frame.line0);
                writeSpan(frame.line0, 0, "R", 1);
                for (int p = 0; p < 8; ++p) {
                    const uint8_t rch = perf.instruments[static_cast<std::size_t>(p)].receiveChannel;
                    const char sep = (p < 7) ? '/' : ' ';
                    if (rch >= 16u) {
                        std::snprintf(tmp, sizeof(tmp), "omn%c", sep);
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%3d%c", static_cast<int>(rch) + 1, sep);
                    }
                    writeSpan(frame.line0, 1 + p * 4, tmp, 4);
                }
            }
            clearLine(frame.line1);
            writeSpan(frame.line1, 0, isSingle ? " " : "T", 1);
            const auto& tx = songSlots_[static_cast<std::size_t>(activeSongSlot_)].trackTxCh;
            for (int t = 0; t < 8; ++t) {
                const uint8_t c = tx[static_cast<std::size_t>(t)];
                const char sep = (t < 7) ? '/' : ' ';
                if (c == 0u) {
                    std::snprintf(tmp, sizeof(tmp), "off%c", sep);
                } else {
                    std::snprintf(tmp, sizeof(tmp), "%3d%c", static_cast<int>(c), sep);
                }
                writeSpan(frame.line1, 1 + t * 4, tmp, 4);
            }
            frame.showCursor = true;
            frame.cursorRow  = 1;
            frame.cursorCol  = 1 + tchCursorTrack_ * 4;
            break;
        }

        case ClassicScreen::SeqJobSong: {
            // Spec §2.8 (p. 90)
            if (songClearConfirm_) {
                setLineFormatted(frame.line0, "SONG) Song Clear     Are you sure ?     ");
                setLineFormatted(frame.line1, "Push YES to Clear / NO to Cancel        ");
            } else {
                setLineFormatted(frame.line0, "SONG) NAME          Song Clear          ");
                const auto& nm = songSlots_[static_cast<std::size_t>(activeSongSlot_)].name;
                std::snprintf(tmp, sizeof(tmp), "%d:%.8s   %c  %c      >GO         >EXIT",
                              activeSongSlot_ + 1, nm.data(), kGlyphLeftArrow, kGlyphRightArrow);
                setLineFormatted(frame.line1, tmp);
                frame.showCursor = true;
                frame.cursorRow  = 1;
                frame.cursorCol  = 2 + std::clamp(songNameCursorCol_, 0, 7);
            }
            break;
        }

        case ClassicScreen::SeqJobEditMenu: {
            // Spec §2.9 (p. 91)
            setLineFormatted(frame.line0, "EDIT TRACK)               Select one!   ");
            setLineFormatted(frame.line1, ">Mix >Qntz>Del >Ins >Copy>Eras>Remv>EXIT");
            break;
        }

        case ClassicScreen::SeqJobEditSub: {
            // Spec §2.9 7 Edit sub-screens (pp. 91-93)
            if (seqEditConfirm_) {
                setLineFormatted(frame.line0, "EDIT TRACK)          Are you sure ?     ");
                setLineFormatted(frame.line1, "Push YES to Execute / NO to Cancel      ");
                break;
            }
            switch (seqEditSubKind_) {
                case SeqEditSubKind::Mix:
                    setLineFormatted(frame.line0, "TRACK MIX DOWN)                         ");
                    std::snprintf(tmp, sizeof(tmp), " Tr %c%d and Tr %c%d   to Tr %c%d    >GO >EXIT",
                                  (seqEditCursorField_ <= 1) ? kGlyphPlayArrow : '>', mixSrcA_,
                                  (seqEditCursorField_ >= 2 && seqEditCursorField_ <= 3) ? kGlyphPlayArrow : '>', mixSrcB_,
                                  (seqEditCursorField_ >= 4) ? kGlyphPlayArrow : '>', mixDst_);
                    setLineFormatted(frame.line1, tmp);
                    break;
                case SeqEditSubKind::Quantize: {
                    static constexpr std::array<const char*, 7> kQntzNames = {
                        "1/48", "1/32", "1/16", "1/12", "1/8 ", "1/6 ", "1/4 "
                    };
                    std::snprintf(tmp, sizeof(tmp), "QUANTIZE)        %cTr  %cSize             ",
                                  (seqEditCursorField_ <= 3) ? kGlyphPlayArrow : '>',
                                  (seqEditCursorField_ >= 4) ? kGlyphPlayArrow : '>');
                    setLineFormatted(frame.line0, tmp);
                    std::snprintf(tmp, sizeof(tmp), "                   %d   %-4.4s    >GO >EXIT",
                                  quantizeTrk_, kQntzNames[static_cast<std::size_t>(std::clamp(quantizeSizeIdx_, 0, 6))]);
                    setLineFormatted(frame.line1, tmp);
                    break;
                }
                case SeqEditSubKind::DeleteMeas:
                    setLineFormatted(frame.line0, "DELETE) Delete Measure (All tracks)     ");
                    std::snprintf(tmp, sizeof(tmp), " from %c%03d  to %c%03d            >GO >EXIT",
                                  (seqEditCursorField_ <= 1) ? kGlyphPlayArrow : '>', delFromMeas_,
                                  (seqEditCursorField_ >= 2) ? kGlyphPlayArrow : '>', delToMeas_);
                    setLineFormatted(frame.line1, tmp);
                    break;
                case SeqEditSubKind::InsertMeas:
                    setLineFormatted(frame.line0, "INSERT)Insert Measure (All tracks)      ");
                    std::snprintf(tmp, sizeof(tmp), " from %c%03d  to %c%03d Ins %c%03d   >GO >EXIT",
                                  (seqEditCursorField_ <= 1) ? kGlyphPlayArrow : '>', insFromMeas_,
                                  (seqEditCursorField_ >= 2 && seqEditCursorField_ <= 3) ? kGlyphPlayArrow : '>', insToMeas_,
                                  (seqEditCursorField_ >= 4) ? kGlyphPlayArrow : '>', insCount_);
                    setLineFormatted(frame.line1, tmp);
                    break;
                case SeqEditSubKind::CopyMeas:
                    setLineFormatted(frame.line0, "COPY) Copy Measure                      ");
                    std::snprintf(tmp, sizeof(tmp), " Tr %c%d %c%03d%c%c%03d  to Tr %c%d %c%03d >GO>EXIT",
                                  (seqEditCursorField_ == 0) ? kGlyphPlayArrow : '>', copySrcTrk_,
                                  (seqEditCursorField_ == 1) ? kGlyphPlayArrow : '>', copyFromMeas_,
                                  kGlyphRightArrow,
                                  (seqEditCursorField_ == 2) ? kGlyphPlayArrow : '>', copyToMeas_,
                                  (seqEditCursorField_ >= 3 && seqEditCursorField_ <= 4) ? kGlyphPlayArrow : '>', copyDstTrk_,
                                  (seqEditCursorField_ >= 5) ? kGlyphPlayArrow : '>', copyDstMeas_);
                    setLineFormatted(frame.line1, tmp);
                    break;
                case SeqEditSubKind::EraseMeas: {
                    std::array<char, 4> trTxt{};
                    if (eraseTrk_ >= 9) {
                        std::snprintf(trTxt.data(), trTxt.size(), "all");
                    } else {
                        std::snprintf(trTxt.data(), trTxt.size(), " %d ", eraseTrk_);
                    }
                    setLineFormatted(frame.line0, "ERASE) Erase Measure                    ");
                    std::snprintf(tmp, sizeof(tmp), "  Tr %c%c%c%c from %c%03d  to  %c%03d  >GO >EXIT",
                                  (seqEditCursorField_ <= 1) ? kGlyphPlayArrow : '>',
                                  trTxt[0], trTxt[1], trTxt[2],
                                  (seqEditCursorField_ >= 2 && seqEditCursorField_ <= 3) ? kGlyphPlayArrow : '>', eraseFromMeas_,
                                  (seqEditCursorField_ >= 4) ? kGlyphPlayArrow : '>', eraseToMeas_);
                    setLineFormatted(frame.line1, tmp);
                    break;
                }
                case SeqEditSubKind::RemoveEvent: {
                    static constexpr std::array<const char*, 3> kEvtNames = { "Cnt", "P.B", "A.T" };
                    setLineFormatted(frame.line0, "REMOVE EVENT)                           ");
                    const char tChar = (remvTrk_ >= 9) ? 'a' : static_cast<char>('0' + remvTrk_);
                    std::snprintf(tmp, sizeof(tmp), "%c%s %c%3d%c%c%3d Tr%c%c %c%03d%c%c%03d  >GO >EXIT",
                                  (seqEditCursorField_ == 0) ? kGlyphPlayArrow : '>', kEvtNames[static_cast<std::size_t>(std::clamp(remvEventType_, 0, 2))],
                                  (seqEditCursorField_ == 1) ? kGlyphPlayArrow : '>', remvRangeLo_,
                                  kGlyphRightArrow,
                                  (seqEditCursorField_ == 2) ? kGlyphPlayArrow : '>', remvRangeHi_,
                                  (seqEditCursorField_ == 3) ? kGlyphPlayArrow : '>', tChar,
                                  (seqEditCursorField_ == 4) ? kGlyphPlayArrow : '>', remvFromMeas_,
                                  kGlyphRightArrow,
                                  (seqEditCursorField_ == 5) ? kGlyphPlayArrow : '>', remvToMeas_);
                    setLineFormatted(frame.line1, tmp);
                    break;
                }
            }
            break;
        }

        case ClassicScreen::SeqJobRecMode: {
            // Spec §2.10 (pp. 82, 88, 94)
            std::snprintf(tmp, sizeof(tmp), "RECORD MODE) %cMode   %cCondition         ",
                          (recModeCursor_ == 0) ? kGlyphPlayArrow : '>',
                          (recModeCursor_ == 1) ? kGlyphPlayArrow : '>');
            setLineFormatted(frame.line0, tmp);
            static constexpr std::array<const char*, 3> kModeStr = { "Realtime", "Step    ", "Punch   " };
            const char* condStr = (seqRecCondition_ == SeqRecCondition::Overdub) ? "Overdub" : "Replace";
            std::snprintf(tmp, sizeof(tmp), "free = 100%% %.8s   %.7s     >EXIT",
                          kModeStr[static_cast<std::size_t>(seqRecMode_)], condStr);
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::SeqRecStandby: {
            // Spec §2.10 (pp. 83-88)
            static constexpr std::array<const char*, 3> kAssignStr = { "SGL", "PFM", "IND" };
            const char* asg = kAssignStr[static_cast<std::size_t>(std::clamp(recVoiceAssign_, 0, 2))];
            const char bankC = bankLetter(mem.selectedVoiceBank(), cardIns);
            const int vIdx = std::clamp(static_cast<int>(mem.selectedVoiceIndex()), 0, 99);
            const int tsN  = std::clamp(processor_.activeDemoTimeSigNum(), 1, 16);
            const int tsD  = std::clamp(processor_.activeDemoTimeSigDen(), 1, 16);
            const int bpm  = std::clamp(processor_.activeDemoTempoBpm(), 30, 240);
            auto cur = [&](int idx) -> char { return (recStandbyCursor_ == idx) ? kGlyphPlayArrow : '>'; };

            if (seqRecMode_ == SeqRecMode::Punch) {
                std::snprintf(tmp, sizeof(tmp), "PUNCH REC)%cMode%cVoice   %cMeas %cIn  %cOut ",
                              cur(0), cur(1), cur(2), cur(3), cur(4));
                setLineFormatted(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "           %s  %c%02d      %03d  %03d  %03d  ",
                              asg, bankC, vIdx, recMeas_, recPunchIn_, recPunchOut_);
                setLineFormatted(frame.line1, tmp);
            } else {
                const char* hdr = (seqRecMode_ == SeqRecMode::Step) ? "STEP REC)" : "RECORD)  ";
                const char* cnd = (seqRecCondition_ == SeqRecCondition::Overdub) ? "Overdub" : "Replace";
                std::snprintf(tmp, sizeof(tmp), "%s%cMode%cVoice%cTime    %cMeas%cTempo",
                              hdr, cur(0), cur(1), cur(2), cur(3), cur(4));
                setLineFormatted(frame.line0, tmp);
                std::snprintf(tmp, sizeof(tmp), "%-7.7s   %s  %c%02d   %d/%-2d    %03d   %3d ",
                              cnd, asg, bankC, vIdx, tsN, tsD, recMeas_, bpm);
                setLineFormatted(frame.line1, tmp);
            }
            break;
        }

        case ClassicScreen::SeqStepBarGraph: {
            // Spec §2.10 (pp. 85-86)
            clearLine(frame.line0);
            std::snprintf(tmp, sizeof(tmp), "M%03d %c |", recMeas_, stepGateSubView_ ? '4' : '8');
            writeSpan(frame.line0, 0, tmp, 8);
            const int fillLen = (stepGatePreset_ == 0) ? 7 : ((stepGatePreset_ == 1) ? 4 : 9);
            for (int c = 0; c < 31; ++c) {
                char ch = ((c % 8) == 7) ? '+' : '-';
                if (stepGateSubView_ && c < fillLen) {
                    ch = '#';
                } else if (!stepGateSubView_ && c == stepBarTick_) {
                    ch = '*';
                }
                frame.line0[static_cast<std::size_t>(8 + c)] = ch;
            }
            frame.line0[39] = '|';

            if (stepGateSubView_) {
                auto mk = [&](int idx) -> char { return (stepGatePreset_ == idx) ? kGlyphPlayArrow : '>'; };
                std::snprintf(tmp, sizeof(tmp), "             %cNorm%cStac%cSlur       >EXIT",
                              mk(0), mk(1), mk(2));
                setLineFormatted(frame.line1, tmp);
            } else {
                std::snprintf(tmp, sizeof(tmp), "             %cGateTime    >PgmChange    ", kGlyphPlayArrow);
                setLineFormatted(frame.line1, tmp);
            }
            break;
        }

        case ClassicScreen::DiskRoot: {
            // Spec §3.1 (pp. 105-109)
            setLineFormatted(frame.line0, "UT DISK)       Select one !             ");
            setLineFormatted(frame.line1, ">Save>Load>Del >Rename  >MDR >Dir  >Job ");
            break;
        }

        case ClassicScreen::DiskTypeSelect: {
            // Spec §3.2-3.5
            const char* opName = "LOAD  ";
            if (diskOp_ == DiskOperation::Save)        opName = "SAVE  ";
            else if (diskOp_ == DiskOperation::Delete) opName = "DELETE";
            else if (diskOp_ == DiskOperation::Rename) opName = "RENAME";
            std::snprintf(tmp, sizeof(tmp), "UT DISK) %-6.6s  Select one !           ", opName);
            setLineFormatted(frame.line0, tmp);
            setLineFormatted(frame.line1, " ALL  SYN  SEQ  R.SEQ  CARD        >EXIT");
            break;
        }

        case ClassicScreen::DiskFileAction: {
            const auto tag = diskTypeTag(diskDataType_);
            if (diskActionConfirm_) {
                const char* opName = (diskOp_ == DiskOperation::Save)
                                         ? (diskActionOverwrite_ ? "OVWRT" : "SAVE")
                                         : ((diskOp_ == DiskOperation::Load) ? "LOAD"
                                         : ((diskOp_ == DiskOperation::Delete) ? "DELETE" : "RENAME"));
                std::snprintf(tmp, sizeof(tmp), "UT DISK) %-6.6s %-5.5s  Are you sure ?  ", opName, tag.data());
                setLineFormatted(frame.line0, tmp);
                setLineFormatted(frame.line1, "Push YES to Execute / NO to Cancel      ");
                break;
            }

            const auto matches = filteredDiskEntryIndices(diskDataType_);
            if (diskOp_ == DiskOperation::Save) {
                std::snprintf(tmp, sizeof(tmp), "UT DISK) SAVE %-5.5s Set name & push GO  ", tag.data());
                setLineFormatted(frame.line0, tmp);
                if (tag.size() >= 5u) {
                    std::snprintf(tmp, sizeof(tmp), "%cFile %02d %c%.8s(%.5s) %c   %c     >GO ",
                                  diskEditingName_ ? '>' : kGlyphPlayArrow,
                                  diskFileIndex_ + 1,
                                  diskEditingName_ ? kGlyphPlayArrow : '>',
                                  diskEditName_.data(),
                                  tag.data(),
                                  kGlyphLeftArrow,
                                  kGlyphRightArrow);
                } else if (tag.size() == 4u) {
                    std::snprintf(tmp, sizeof(tmp), "%cFile %02d %c%.8s(%.4s)  %c   %c     >GO ",
                                  diskEditingName_ ? '>' : kGlyphPlayArrow,
                                  diskFileIndex_ + 1,
                                  diskEditingName_ ? kGlyphPlayArrow : '>',
                                  diskEditName_.data(),
                                  tag.data(),
                                  kGlyphLeftArrow,
                                  kGlyphRightArrow);
                } else {
                    std::snprintf(tmp, sizeof(tmp), "%cFile %02d %c%.8s(%.3s)   %c   %c     >GO ",
                                  diskEditingName_ ? '>' : kGlyphPlayArrow,
                                  diskFileIndex_ + 1,
                                  diskEditingName_ ? kGlyphPlayArrow : '>',
                                  diskEditName_.data(),
                                  tag.data(),
                                  kGlyphLeftArrow,
                                  kGlyphRightArrow);
                }
                setLineFormatted(frame.line1, tmp);
                if (diskEditingName_) {
                    frame.showCursor = true;
                    frame.cursorRow  = 1;
                    frame.cursorCol  = 10 + std::clamp(diskNameCursorCol_, 0, 7);
                }
            } else if (diskOp_ == DiskOperation::Rename) {
                if (tag.size() >= 5u) {
                    std::snprintf(tmp, sizeof(tmp), "UT DISK) RENAME %-5.5s Set name&push GO  ", tag.data());
                } else {
                    std::snprintf(tmp, sizeof(tmp), "UT DISK) RENAME %-4.4sSet name & push GO  ", tag.data());
                }
                setLineFormatted(frame.line0, tmp);
                if (matches.empty()) {
                    std::snprintf(tmp, sizeof(tmp), "No Matching File (%.5s)          >EXIT", tag.data());
                    setLineFormatted(frame.line1, tmp);
                } else {
                    if (tag.size() >= 5u) {
                        std::snprintf(tmp, sizeof(tmp), "%cFile %02d %c%.8s(%.5s) %c   %c     >GO ",
                                      diskEditingName_ ? ' ' : kGlyphPlayArrow,
                                      diskFileIndex_ + 1,
                                      diskEditingName_ ? kGlyphPlayArrow : '>',
                                      diskEditName_.data(),
                                      tag.data(),
                                      kGlyphLeftArrow,
                                      kGlyphRightArrow);
                    } else if (tag.size() == 4u) {
                        std::snprintf(tmp, sizeof(tmp), "%cFile %02d %c%.8s(%.4s)  %c   %c     >GO ",
                                      diskEditingName_ ? ' ' : kGlyphPlayArrow,
                                      diskFileIndex_ + 1,
                                      diskEditingName_ ? kGlyphPlayArrow : '>',
                                      diskEditName_.data(),
                                      tag.data(),
                                      kGlyphLeftArrow,
                                      kGlyphRightArrow);
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%cFile %02d %c%.8s(%.3s)   %c   %c     >GO ",
                                      diskEditingName_ ? ' ' : kGlyphPlayArrow,
                                      diskFileIndex_ + 1,
                                      diskEditingName_ ? kGlyphPlayArrow : '>',
                                      diskEditName_.data(),
                                      tag.data(),
                                      kGlyphLeftArrow,
                                      kGlyphRightArrow);
                    }
                    setLineFormatted(frame.line1, tmp);
                    if (diskEditingName_) {
                        frame.showCursor = true;
                        frame.cursorRow  = 1;
                        frame.cursorCol  = 10 + std::clamp(diskNameCursorCol_, 0, 7);
                    }
                }
            } else {
                if (diskOp_ == DiskOperation::Delete) {
                    if (tag.size() >= 5u) {
                        std::snprintf(tmp, sizeof(tmp), "UT DISK) DELETE %-5.5s Sel file&push GO  ", tag.data());
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "UT DISK) DELETE %-4.4sSel file & push GO  ", tag.data());
                    }
                } else {
                    std::snprintf(tmp, sizeof(tmp), "UT DISK) LOAD %-5.5s Sel file & push GO  ", tag.data());
                }
                setLineFormatted(frame.line0, tmp);
                if (matches.empty()) {
                    std::snprintf(tmp, sizeof(tmp), "No Matching File (%.5s)          >EXIT", tag.data());
                    setLineFormatted(frame.line1, tmp);
                } else {
                    const std::size_t idx = matches[static_cast<std::size_t>(std::clamp(diskFileIndex_, 0, static_cast<int>(matches.size()) - 1))];
                    const auto& e = diskManager_.entries()[idx];
                    const int kb = static_cast<int>(std::max<uint32_t>(1u, vf50::disk::VF50DiskManager::bytesToKbCeil(e.sizeBytes)));
                    const auto dot = e.name.find_last_of('.');
                    const std::size_t stemLen = (dot == std::string::npos) ? e.name.size() : dot;
                    std::array<char, 9> dispName{};
                    dispName.fill(' ');
                    for (std::size_t c = 0u; c < std::min<std::size_t>(stemLen, 8u); ++c) {
                        dispName[c] = e.name[c];
                    }
                    const auto shortType = diskEntryShortType(e);
                    if (shortType.size() >= 5u) {
                        std::snprintf(tmp, sizeof(tmp), "%cFile %02d :%.8s(%.5s)%3dK >GO  >EXIT",
                                      kGlyphPlayArrow, diskFileIndex_ + 1, dispName.data(),
                                      shortType.data(), std::min(kb, 999));
                    } else if (shortType.size() == 4u) {
                        std::snprintf(tmp, sizeof(tmp), "%cFile %02d :%.8s(%.4s)%3dK  >GO  >EXIT",
                                      kGlyphPlayArrow, diskFileIndex_ + 1, dispName.data(),
                                      shortType.data(), std::min(kb, 999));
                    } else {
                        std::snprintf(tmp, sizeof(tmp), "%cFile %02d :%.8s(%.3s) %3dK  >GO  >EXIT",
                                      kGlyphPlayArrow, diskFileIndex_ + 1, dispName.data(),
                                      shortType.data(), std::min(kb, 999));
                    }
                    setLineFormatted(frame.line1, tmp);
                }
            }
            break;
        }

        case ClassicScreen::DiskMdrMenu: {
            // Spec §3.6 (p. 107)
            setLineFormatted(frame.line0, "UT DISK) MDR      Select one !          ");
            setLineFormatted(frame.line1, " >In  >Out >Del >Rename >IntTime   >EXIT");
            break;
        }

        case ClassicScreen::DiskMdrIntTime: {
            // Spec §3.6 (p. 107)
            setLineFormatted(frame.line0, "UT DISK) MDR                            ");
            std::snprintf(tmp, sizeof(tmp), " Interval Time = %d x 100 ms        >EXIT", diskMdrIntTime_);
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::DiskDirectory: {
            // Spec §3.7 (p. 108)
            const auto& allEnts = diskManager_.entries();
            const auto dirIndices = classicDirectoryEntryIndices();
            const int total = static_cast<int>(dirIndices.size());
            std::snprintf(tmp, sizeof(tmp), "UT DISK) DIRECTORY     Total:%3d Files  ", total);
            setLineFormatted(frame.line0, tmp);

            if (!diskManager_.isMounted()) {
                setLineFormatted(frame.line1, " Insert DISK                       >EXIT");
            } else if (dirIndices.empty()) {
                setLineFormatted(frame.line1, " No Files Found                    >EXIT");
            } else {
                const int idx = std::clamp(diskFileIndex_, 0, total - 1);
                const auto& e = allEnts[dirIndices[static_cast<std::size_t>(idx)]];
                const int kb = static_cast<int>(std::max<uint32_t>(1u, vf50::disk::VF50DiskManager::bytesToKbCeil(e.sizeBytes)));
                const auto dot = e.name.find_last_of('.');
                const std::size_t stemLen = (dot == std::string::npos) ? e.name.size() : dot;
                std::array<char, 9> dispName{};
                dispName.fill(' ');
                for (std::size_t c = 0u; c < std::min<std::size_t>(stemLen, 8u); ++c) {
                    dispName[c] = e.name[c];
                }
                const auto shortType = diskEntryShortType(e);
                if (shortType.size() >= 5u) {
                    std::snprintf(tmp, sizeof(tmp), "%cFile %02d :%.8s(%.5s)%3dK      >EXIT",
                                  kGlyphPlayArrow, idx + 1, dispName.data(),
                                  shortType.data(), std::min(kb, 999));
                } else if (shortType.size() == 4u) {
                    std::snprintf(tmp, sizeof(tmp), "%cFile %02d :%.8s(%.4s) %3dK      >EXIT",
                                  kGlyphPlayArrow, idx + 1, dispName.data(),
                                  shortType.data(), std::min(kb, 999));
                } else {
                    std::snprintf(tmp, sizeof(tmp), "%cFile %02d :%.8s(%.3s)  %3dK      >EXIT",
                                  kGlyphPlayArrow, idx + 1, dispName.data(),
                                  shortType.data(), std::min(kb, 999));
                }
                setLineFormatted(frame.line1, tmp);
            }
            break;
        }

        case ClassicScreen::DiskJobMenu: {
            // Spec §3.8 (pp. 108-109)
            if (diskFormatConfirm_) {
                setLineFormatted(frame.line0, "UT DISK) FORMAT       Are you sure ?    ");
                setLineFormatted(frame.line1, "Push YES to Format / NO to Cancel       ");
            } else {
                setLineFormatted(frame.line0, "UT DISK) Insert DISK and select one !   ");
                setLineFormatted(frame.line1, ">Format   >BackUp   >Status        >EXIT");
            }
            break;
        }

        case ClassicScreen::DiskStatus: {
            // Spec §3.8 (p. 109)
            setLineFormatted(frame.line0, "UT DISK) STATUS                         ");
            int usedKb = 0, freeKb = 0;
            diskManager_.computeSaveTargetUsageKb(usedKb, freeKb);
            const int files = std::min(static_cast<int>(diskManager_.entries().size()), 99);
            std::snprintf(tmp, sizeof(tmp), "Total=%2dfiles, Used=%3dK,Free=%3dK >EXIT",
                          files, std::min(usedKb, 999), std::min(freeKb, 999));
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::DiskBackupPrompt: {
            // Spec §3.8 (pp. 108-109)
            setLineFormatted(frame.line0, "UT DISK) BACKUP                         ");
            if (diskBackupStep_ == 0) {
                setLineFormatted(frame.line1, "** SEQ/R.SEQ data will be cleared. Sure?");
            } else if (diskBackupStep_ == 1) {
                setLineFormatted(frame.line1, "** Set original disk & push YES         ");
            } else if (diskBackupStep_ == 2) {
                setLineFormatted(frame.line1, "** Set duplicate disk & push YES        ");
            } else {
                setLineFormatted(frame.line1, "Backup completed !                 >EXIT");
            }
            break;
        }

        case ClassicScreen::UtilMemoryProtect: {
            // Spec §3.9 (p. 109)
            std::snprintf(tmp, sizeof(tmp), "UT MEMORY PROTECT) %cINT      %cCARD      ",
                          (memProtectCursor_ == 0) ? kGlyphPlayArrow : '>',
                          (memProtectCursor_ == 1) ? kGlyphPlayArrow : '>');
            setLineFormatted(frame.line0, tmp);
            std::snprintf(tmp, sizeof(tmp), "                    %s        %s      ",
                          processor_.isIntMemoryProtected() ? "on " : "off",
                          processor_.isCardMemoryProtected() ? "on " : "off");
            setLineFormatted(frame.line1, tmp);
            break;
        }

        case ClassicScreen::DelegatedEdit:
            break;

        default:
            if (!renderRhythmLcdFrame(frame) && !renderUtilLcdFrame(frame)) {
                (void)renderEditLcdFrame(frame);
            }
            break;
    }

    if (noticeTicks_ > 0 && !noticeText_.empty()) {
        std::snprintf(tmp, sizeof(tmp), "%-34.34s      ", noticeText_.c_str());
        setLineFormatted(frame.line1, tmp);
    }

    return frame;
}

ClassicLedSnapshot VF50ClassicController::computeLedSnapshot() const noexcept {
    ClassicLedSnapshot leds{};
    processor_.pullLatestAudioMemoryToHost();
    const auto& mem = processor_.hostMemory();
    const auto ws   = mem.workstationMode();
    const bool seqPlaying = processor_.isDemoPlaying();

    // Bank LEDs reflect active voice or performance memory bank
    const bool perfModeActive = (screen_ == ClassicScreen::PlayPerformance)
                             || isPerfEditScreen(screen_)
                             || screen_ == ClassicScreen::UtilOthersPerf
                             || screen_ == ClassicScreen::UtilOthersPerfInit
                             || screen_ == ClassicScreen::UtilOthersMultiVoiceSelect
                             || screen_ == ClassicScreen::StorePerf
                             || screen_ == ClassicScreen::StoreMultiVoice
                             || screen_ == ClassicScreen::CopyPerfEffect
                             || multiVoiceEditActive_
                             || ((screen_ == ClassicScreen::DelegatedEdit
                                  || isDiskScreen(screen_)
                                  || screen_ == ClassicScreen::UtilMemoryProtect
                                  || screen_ == ClassicScreen::UtilOthersPresetLoad
                                  || screen_ == ClassicScreen::CopyEffect)
                                 && (ws == vf50::state::WorkstationMode::PerformancePlay
                                     || ws == vf50::state::WorkstationMode::PerformanceEdit));
    const auto activeBank = perfModeActive ? mem.selectedPerfBank() : mem.selectedVoiceBank();
    if (activeBank == vf50::state::MemoryBank::FactoryRom) {
        leds.memPreset = LedState::SolidGreen;
    } else if (cardBankActive_) {
        leds.memCard = LedState::SolidGreen;
    } else {
        leds.memInt = LedState::SolidGreen;
    }

    // Play & System LEDs
    const bool inSeqScreen = (screen_ == ClassicScreen::SeqPlaySong
                              || screen_ == ClassicScreen::SeqSongSetupPrompt
                              || screen_ == ClassicScreen::SeqStoreSetupConfirm
                              || screen_ == ClassicScreen::SeqJobSelect
                              || screen_ == ClassicScreen::SeqJobSetup
                              || screen_ == ClassicScreen::SeqJobTransmitCh
                              || screen_ == ClassicScreen::SeqJobSong
                              || screen_ == ClassicScreen::SeqJobEditMenu
                              || screen_ == ClassicScreen::SeqJobEditSub
                              || screen_ == ClassicScreen::SeqJobRecMode
                              || screen_ == ClassicScreen::SeqRecStandby
                              || screen_ == ClassicScreen::SeqStepBarGraph);

    const bool inRhythmScr = isRhythmScreen(screen_);
    if (!inRhythmScr) {
        if (screen_ == ClassicScreen::PlayPerformance || (!inSeqScreen && perfModeActive)) {
            leds.performance = LedState::SolidGreen;
        } else if (screen_ == ClassicScreen::PlaySingle || (!inSeqScreen && !perfModeActive)) {
            leds.single = LedState::SolidGreen;
        }
    }

    if (inSeqScreen || seqPlaying) {
        leds.seq = LedState::SolidGreen;
    }

    const bool rhyPlaying = processor_.isRhythmEnginePlaying() || processor_.isRhythmSongPlaying();
    const bool rhyLit = (ws == vf50::state::WorkstationMode::RhythmPlay)
                     || (processor_.parameterBank().rhythmPlay->load(std::memory_order_relaxed) >= 0.5f)
                     || rhyPlaying
                     || inRhythmScr;
    if (rhyLit) {
        leds.rhythm = LedState::SolidGreen;
    }

    leds.effectBypass = (processor_.parameterBank().effectBypass->load(std::memory_order_relaxed) >= 0.5f)
                            ? LedState::SolidRed
                            : LedState::Off;
    leds.demo         = demoLedState() ? LedState::SolidGreen : LedState::Off;
    leds.compare      = isCompareModeLocked() ? LedState::SolidGreen : LedState::Off;

    // Operator On/Off 1-4 & OP SELECT (RECORD) LEDs (Ch. 1.5, pp. 7, 38)
    const uint8_t opMask = mem.operatorEnableMask();
    for (std::size_t op = 0u; op < 4u; ++op) {
        const bool opOn = (((opMask >> op) & 1u) != 0u);
        leds.opOnOff[op] = opOn ? LedState::SolidGreen : LedState::Off;
    }
    if ((screen_ == ClassicScreen::DelegatedEdit && delegatedVoiceEdit_) || isVoiceEditScreen(screen_)) {
        leds.opSelect = LedState::SolidGreen;
    } else if (screen_ == ClassicScreen::SeqRecStandby || screen_ == ClassicScreen::SeqStepBarGraph) {
        leds.opSelect = LedState::BlinkGreen;
    } else {
        leds.opSelect = LedState::Off;
    }

    // Track TR1..TR8 & RHY LEDs (Ch. 1.2/1.3/1.4/2/5)
    if (inSeqScreen || (seqPlaying && !isPerfEditScreen(screen_) && !isVoiceEditScreen(screen_))) {
        // Sequencer view / background sequencer playback (Spec §2.1-2.3, p. 87):
        // SolidGreen = track has data & unmuted, BlinkGreen = muted, Off = empty & unmuted
        const uint16_t dataMask = processor_.sequencerTrackDataMask();
        for (std::size_t t = 0u; t < 8u; ++t) {
            const bool muted = processor_.isSequencerTrackMuted(t);
            const bool hasData = (((dataMask >> t) & 1u) != 0u);
            if (muted) {
                leds.tr[t] = LedState::BlinkGreen;
            } else if (hasData) {
                leds.tr[t] = LedState::SolidGreen;
            } else {
                leds.tr[t] = LedState::Off;
            }
        }
        const bool rhyMuted = processor_.isSequencerTrackMuted(8u);
        const bool rhyHasData = (((dataMask >> 8u) & 1u) != 0u) || rhyLit;
        leds.rhy = rhyMuted ? LedState::BlinkGreen : (rhyHasData ? LedState::SolidGreen : LedState::Off);
    } else if (screen_ == ClassicScreen::PlayPerformance) {
        // Spec §1.3-1.4 (p. 20):
        // BlinkRed on muted instrument when played, BlinkGreen on unmuted sounding instrument
        for (std::size_t p = 0u; p < 8u; ++p) {
            const bool isMuted = ((perfInstMuteMask_ >> p) & 1u) != 0u;
            if (isMuted && perfMuteHitTicks_[p] > 0u) {
                leds.tr[p] = LedState::BlinkRed;
            } else if (!isMuted && perfHitTicks_[p] > 0u) {
                leds.tr[p] = LedState::BlinkGreen;
            } else {
                leds.tr[p] = LedState::Off;
            }
        }
        const bool rhyMuted = processor_.isSequencerTrackMuted(8u);
        leds.rhy = rhyMuted ? LedState::BlinkGreen : (rhyLit ? LedState::SolidGreen : LedState::Off);
    } else if (screen_ == ClassicScreen::PlaySingle) {
        // Spec §1.2 (p. 18): TR1 blinks briefly (~0.2s) whenever keys are played
        leds.tr[0] = (singleHitTicks_ > 0u) ? LedState::BlinkGreen : LedState::Off;
        const bool rhyMuted = processor_.isSequencerTrackMuted(8u);
        leds.rhy = rhyMuted ? LedState::BlinkGreen : (rhyLit ? LedState::SolidGreen : LedState::Off);
    } else {
        if (processor_.isSequencerTrackMuted(8u)) {
            leds.rhy = LedState::BlinkGreen;
        }
    }

    applyEditLedSnapshot(leds);
    applyRhythmLedSnapshot(leds);
    applyUtilLedSnapshot(leds);

    return leds;
}

} // namespace vf50::classic
