#pragma once

#include "DSP/Drums/Ym3602DrumEngine.h"
#include "DSP/Effects/Ym3413DspProcessor.h"
#include "DSP/FM/VF50FmEngine.h"
#include "ROM/VF50RomBank.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace vf50::dsp {

// Pre-allocated, zero-allocation container for imported disk .SEQ (HardwareSeqV1) songs
struct CustomSeqSongBuffer {
    static constexpr std::size_t kMaxEvents = 32768u;

    bool                    valid           = false;
    std::array<char, 16>    titleChars      = { 'D','I','S','K',' ','S','E','Q',' ',' ',' ',' ',' ',' ',' ',' ' };
    uint16_t                initialTempoBpm = 120u;
    uint8_t                 timeSigNum      = 4u;
    uint8_t                 timeSigDen      = 4u;
    uint16_t                ticksPerBar     = 192u;
    uint16_t                totalBars       = 1u;
    uint32_t                totalTicks      = 192u;
    uint8_t                 playMode        = 1u; // 0 = Single, 1 = Performance
    uint8_t                 perfPresetBank  = 1u; // 1 = Factory Preset, 0 = Internal User
    uint8_t                 perfNumber      = 0u;
    bool                    hasRhythmLink   = false; // True when HardwareSeqV1 header provides Rhythm sync link
    bool                    rhythmSyncIsSong = false; // buf[31] bit 0: 0 = Rhythm Pattern, 1 = Rhythm Song
    uint8_t                 rhythmTargetIdx = 0u; // buf[32]: if Song -> 0..7; if Pattern -> 0..99=I00..I99, 100..199=P00..P99
    std::array<uint8_t, 8>  trackChannels   = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
    uint32_t                eventCount      = 0u;
    std::array<vf50::generated::DemoSongEvent, kMaxEvents> events{};

    void clear() noexcept {
        valid           = false;
        titleChars      = { 'D','I','S','K',' ','S','E','Q',' ',' ',' ',' ',' ',' ',' ',' ' };
        initialTempoBpm = 120u;
        timeSigNum      = 4u;
        timeSigDen      = 4u;
        ticksPerBar     = 192u;
        totalBars       = 1u;
        totalTicks      = 192u;
        playMode        = 1u;
        perfPresetBank  = 1u;
        perfNumber      = 0u;
        hasRhythmLink   = false;
        rhythmSyncIsSong = false;
        rhythmTargetIdx = 0u;
        trackChannels   = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
        eventCount      = 0u;
    }

    void copyFromFast(const CustomSeqSongBuffer& src) noexcept {
        valid            = src.valid;
        titleChars       = src.titleChars;
        initialTempoBpm  = src.initialTempoBpm;
        timeSigNum       = src.timeSigNum;
        timeSigDen       = src.timeSigDen;
        ticksPerBar      = src.ticksPerBar;
        totalBars        = src.totalBars;
        totalTicks       = src.totalTicks;
        playMode         = src.playMode;
        perfPresetBank   = src.perfPresetBank;
        perfNumber       = src.perfNumber;
        hasRhythmLink    = src.hasRhythmLink;
        rhythmSyncIsSong = src.rhythmSyncIsSong;
        rhythmTargetIdx  = src.rhythmTargetIdx;
        trackChannels    = src.trackChannels;
        eventCount       = std::min<uint32_t>(src.eventCount, static_cast<uint32_t>(kMaxEvents));
        for (uint32_t i = 0u; i < eventCount; ++i) {
            events[i] = src.events[i];
        }
    }

    [[nodiscard]] std::string_view title() const noexcept {
        std::size_t len = titleChars.size();
        while (len > 0u && (titleChars[len - 1u] == ' ' || titleChars[len - 1u] == '\0')) {
            --len;
        }
        return std::string_view(titleChars.data(), len);
    }

    void setTitle(std::string_view sv) noexcept {
        titleChars.fill(' ');
        const std::size_t n = std::min(sv.size(), titleChars.size());
        for (std::size_t i = 0; i < n; ++i) {
            const char c = sv[i];
            titleChars[i] = (c >= 0x20 && c <= 0x7E) ? c : ' ';
        }
    }
};

class VF50DemoSongPlayer {
public:
    static constexpr std::size_t kUserSongCount       = 8u; // Slots 0..7 = Sequencer Songs #1..#8 (empty at boot)
    static constexpr std::size_t kFactoryDemoSongBase = 8u; // Slots 8..10 = Isolated ROM Demo Songs 1..3
    static constexpr std::size_t kTotalSongSlots      = kFactoryDemoSongBase + vf50::generated::kDemoSongCount;
    static constexpr std::size_t kCustomSongSlot      = 0u; // Song #1 (index 0)
    static constexpr std::size_t kTotalTrackCount     = 9u; // 0..7 = FM Tracks 1..8, 8 = Rhythm Track

    [[nodiscard]] static constexpr bool isUserSongIndex(std::size_t idx) noexcept {
        return idx < kUserSongCount;
    }
    [[nodiscard]] static constexpr bool isFactoryDemoSongIndex(std::size_t idx) noexcept {
        return idx >= kFactoryDemoSongBase && idx < kTotalSongSlots;
    }

    VF50DemoSongPlayer() {
        for (auto& s : userSongs_) {
            s = std::make_unique<CustomSeqSongBuffer>();
            s->clear();
        }
        resetTrackMutes();
    }

    VF50DemoSongPlayer(const VF50DemoSongPlayer&) = delete;
    VF50DemoSongPlayer& operator=(const VF50DemoSongPlayer&) = delete;
    VF50DemoSongPlayer(VF50DemoSongPlayer&&) noexcept = delete;
    VF50DemoSongPlayer& operator=(VF50DemoSongPlayer&&) noexcept = delete;

    void setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank) noexcept {
        romBank_ = (romBank != nullptr) ? romBank : &vf50::rom::getDefaultRomImage();
    }
    [[nodiscard]] const vf50::rom::VF50RomImage& runtimeRomBank() const noexcept {
        return (romBank_ != nullptr) ? *romBank_ : vf50::rom::getDefaultRomImage();
    }

    void reset() noexcept {
        playing_            = false;
        hasSavedState_      = false;
        currentTickPos_     = 0.0;
        nextEventCursor_    = 0u;
        tempoRampTicksLeft_ = 0.0;
        tempoRampStepPerTick_ = 0.0;
        keyRefCounts_.fill(0u);
        for (auto& chNotes : channelActiveNoteCounts_) {
            chNotes.fill(0u);
        }
        channelTotalActiveNotes_.fill(0u);
        trackPeak_.fill(0.0f);
        visualKeyMask61_    = 0ULL;
        dispatchedNoteOns_  = 0u;
        needsAccompanyingRhythmPattern_ = false;
        pausedWithRetainedPos_          = false;
        hasTempoOverride_               = false;
        tempoOverrideBpm_               = 120.0;
        lastPreparedSongIdx_            = SIZE_MAX;
        pendingMidiNoteOffCount_        = 0u;
        prevMuted_.fill(false);
    }

    // Thread-safe Track 1..8 (0..7) & Rhythm Track (8) mute controls
    void setTrackMuted(std::size_t trackIndex0To8, bool muted) noexcept {
        if (trackIndex0To8 < kTotalTrackCount) {
            trackMuted_[trackIndex0To8].store(muted, std::memory_order_release);
        }
    }

    void toggleTrackMute(std::size_t trackIndex0To8) noexcept {
        if (trackIndex0To8 < kTotalTrackCount) {
            const bool cur = trackMuted_[trackIndex0To8].load(std::memory_order_acquire);
            trackMuted_[trackIndex0To8].store(!cur, std::memory_order_release);
        }
    }

    void toggleRhythmTrackMute() noexcept {
        toggleTrackMute(8u);
    }

    [[nodiscard]] bool isTrackMuted(std::size_t trackIndex0To8) const noexcept {
        return (trackIndex0To8 < kTotalTrackCount)
                   ? trackMuted_[trackIndex0To8].load(std::memory_order_acquire)
                   : false;
    }

    [[nodiscard]] uint8_t trackMeterLevel(std::size_t trackIndex0To8) const noexcept {
        if (trackIndex0To8 >= kTotalTrackCount || isTrackMuted(trackIndex0To8) || !playing_) {
            return 0u;
        }
        const float val = trackPeak_[trackIndex0To8];
        if (val <= 0.02f) return 0u;
        if (val <= 0.15f) return 1u;
        if (val <= 0.28f) return 2u;
        if (val <= 0.42f) return 3u;
        if (val <= 0.56f) return 4u;
        if (val <= 0.70f) return 5u;
        if (val <= 0.82f) return 6u;
        if (val <= 0.93f) return 7u;
        return 8u;
    }

    void resetTrackMutes() noexcept {
        for (auto& m : trackMuted_) {
            m.store(false, std::memory_order_release);
        }
        prevMuted_.fill(false);
    }

    // Strictly output-gate muting: never pauses, stops, or restarts the rhythm clock when RHY is muted/unmuted.
    void syncTrackMuteTransitions(fm::VF50FmEngine& fm, drums::Ym3602DrumEngine& drums) noexcept {
        for (std::size_t t = 0u; t < 8u; ++t) {
            const bool nowMuted = trackMuted_[t].load(std::memory_order_acquire);
            if (nowMuted && !prevMuted_[t]) {
                const uint8_t ch = activeTrackChannels_[t] & 0x0Fu;
                channelTotalActiveNotes_[ch] = 0u;
                trackPeak_[t] = 0.0f;
                fm.channelNotesOff(ch);
                bool emittedAnyNoteOff = false;
                for (std::size_t n = 0u; n < 128u; ++n) {
                    if (channelActiveNoteCounts_[ch][n] > 0u) {
                        channelActiveNoteCounts_[ch][n] = 0u;
                        emittedAnyNoteOff = true;
                        if (pendingMidiNoteOffCount_ < pendingMidiNoteOffs_.size()) {
                            pendingMidiNoteOffs_[pendingMidiNoteOffCount_++] = { ch, static_cast<uint8_t>(n) };
                        }
                        if (n >= 36u && n <= 96u) {
                            const std::size_t k = n - 36u;
                            if (keyRefCounts_[k] > 0u) {
                                --keyRefCounts_[k];
                                if (keyRefCounts_[k] == 0u) {
                                    visualKeyMask61_ &= ~(uint64_t{ 1 } << k);
                                }
                            }
                        }
                    }
                }
                if (!emittedAnyNoteOff && pendingMidiNoteOffCount_ < pendingMidiNoteOffs_.size()) {
                    // Sentinel 0xFFu -> emit CC 123 (All Notes Off) on channel
                    pendingMidiNoteOffs_[pendingMidiNoteOffCount_++] = { ch, static_cast<uint8_t>(0xFFu) };
                }
            }
            prevMuted_[t] = nowMuted;
        }

        const bool rhyNowMuted = trackMuted_[8u].load(std::memory_order_acquire);
        if (rhyNowMuted != prevMuted_[8u]) {
            if (rhyNowMuted) {
                trackPeak_[8u] = 0.0f;
                drums.setOutputTriggerMuted(true);
                drums.silenceActiveDrumVoices();
            } else {
                drums.setOutputTriggerMuted(false);
            }
            prevMuted_[8u] = rhyNowMuted;
        }
        if (rhyNowMuted && !drums.isOutputTriggerMuted()) {
            drums.setOutputTriggerMuted(true);
        }
    }

    std::size_t consumePendingMuteNoteOffs(std::pair<uint8_t, uint8_t>* outPairs,
                                           std::size_t maxPairs) noexcept {
        const std::size_t cnt = std::min(pendingMidiNoteOffCount_, maxPairs);
        for (std::size_t i = 0u; i < cnt; ++i) {
            outPairs[i] = pendingMidiNoteOffs_[i];
        }
        pendingMidiNoteOffCount_ = 0u;
        return cnt;
    }

    // Called when the user switches Performance/Voice presets or play mode mid-playback:
    // cleanly releases any currently sounding notes so zero notes hang while keeping playback running!
    void notifyExternalPatchOrModeSwitch(fm::VF50FmEngine& fm) noexcept {
        if (!playing_) {
            return;
        }
        fm.allNotesOff();
        keyRefCounts_.fill(0u);
        visualKeyMask61_ = 0ULL;
        // Preserve the user's newly auditioned patch/mode if they later press STOP
        hasSavedState_ = false;
    }

    [[nodiscard]] uint64_t dispatchedNoteOnCount() const noexcept { return dispatchedNoteOns_; }

    void loadUserSequenceSlot(std::size_t slot0To7, const CustomSeqSongBuffer& seq) noexcept {
        if (slot0To7 < kUserSongCount && userSongs_[slot0To7] != nullptr) {
            userSongs_[slot0To7]->copyFromFast(seq);
        }
    }

    void clearUserSequenceSlot(std::size_t slot0To7) noexcept {
        if (slot0To7 < kUserSongCount && userSongs_[slot0To7] != nullptr) {
            userSongs_[slot0To7]->clear();
        }
    }

    void clearAllUserSequences() noexcept {
        for (std::size_t s = 0u; s < kUserSongCount; ++s) {
            clearUserSequenceSlot(s);
        }
    }

    [[nodiscard]] bool hasUserSequence(std::size_t slot0To7) const noexcept {
        return slot0To7 < kUserSongCount
            && userSongs_[slot0To7] != nullptr
            && userSongs_[slot0To7]->valid
            && userSongs_[slot0To7]->eventCount > 0u;
    }

    [[nodiscard]] const CustomSeqSongBuffer& userSequence(std::size_t slot0To7) const noexcept {
        return *userSongs_[std::min<std::size_t>(slot0To7, kUserSongCount - 1u)];
    }

    void loadCustomSequence(const CustomSeqSongBuffer& seq) noexcept {
        loadUserSequenceSlot(0u, seq);
    }

    [[nodiscard]] bool hasCustomSequence() const noexcept {
        if (isUserSongIndex(selectedSongIndex_)) {
            return hasUserSequence(selectedSongIndex_);
        }
        return hasUserSequence(0u);
    }

    [[nodiscard]] const CustomSeqSongBuffer& customSequence() const noexcept {
        return userSequence(isUserSongIndex(selectedSongIndex_) ? selectedSongIndex_ : 0u);
    }

    void setChainAllFactorySongs(bool chainAll) noexcept { chainAllFactory_ = chainAll; }
    [[nodiscard]] bool chainAllFactorySongs() const noexcept { return chainAllFactory_; }

    void setSuppressInternalDrums(bool suppress) noexcept { suppressInternalDrums_.store(suppress, std::memory_order_release); }
    [[nodiscard]] bool isSuppressingInternalDrums() const noexcept { return suppressInternalDrums_.load(std::memory_order_acquire); }

    void selectSongIndex(std::size_t idx, bool resetMutesOnChange = true) noexcept {
        const std::size_t clamped = std::min(idx, kTotalSongSlots - 1u);
        if (clamped != selectedSongIndex_) {
            selectedSongIndex_     = clamped;
            if (resetMutesOnChange) {
                resetTrackMutes(); // Manual p. 87: changing song number defeats track muting
            }
            hasTempoOverride_      = false;
            currentTickPos_        = 0.0;
            nextEventCursor_       = 0u;
            pausedWithRetainedPos_ = false;
            activeTicksPerBar_     = resolveSelectedSongTicksPerBar();
            activeTotalTicks_      = resolveSelectedSongTotalTicks();
        }
    }

    [[nodiscard]] std::size_t selectedSongIndex() const noexcept { return selectedSongIndex_; }
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }
    [[nodiscard]] bool needsAccompanyingRhythmPattern() const noexcept { return needsAccompanyingRhythmPattern_; }
    [[nodiscard]] double currentTickPosition() const noexcept { return currentTickPos_; }

    [[nodiscard]] uint8_t timeSigNum() const noexcept {
        if (isUserSongIndex(selectedSongIndex_)) {
            if (hasUserSequence(selectedSongIndex_)) {
                const uint8_t n = userSongs_[selectedSongIndex_]->timeSigNum;
                return (n > 0u) ? n : 4u;
            }
            return 4u;
        }
        const auto& rom = runtimeRomBank();
        const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
        const uint8_t num = rom.factoryDemoSongs[idx].timeSigNum;
        return (num > 0u) ? num : 4u;
    }

    [[nodiscard]] uint8_t timeSigDen() const noexcept {
        if (isUserSongIndex(selectedSongIndex_)) {
            if (hasUserSequence(selectedSongIndex_)) {
                const uint8_t d = userSongs_[selectedSongIndex_]->timeSigDen;
                return (d > 0u) ? d : 4u;
            }
            return 4u;
        }
        const auto& rom = runtimeRomBank();
        const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
        const uint8_t den = rom.factoryDemoSongs[idx].timeSigDen;
        return (den > 0u) ? den : 4u;
    }

    [[nodiscard]] static uint16_t computeTrackDataMaskForEvents(
        std::span<const vf50::generated::DemoSongEvent> evs,
        const std::array<uint8_t, 8>& chMap,
        bool isCustom) noexcept {
        uint16_t mask = 0u;
        bool hasEmbeddedDrums = false;
        for (const auto& ev : evs) {
            if (ev.type == vf50::generated::DemoEventType::FmNoteOn && ev.data2 > 0u) {
                const uint8_t ch = ev.channel & 0x0Fu;
                std::size_t resolved = static_cast<std::size_t>(ch & 0x07u);
                for (std::size_t t = 0u; t < 8u; ++t) {
                    if ((chMap[t] & 0x0Fu) == ch) {
                        resolved = t;
                        break;
                    }
                }
                mask |= static_cast<uint16_t>(1u << resolved);
            } else if (ev.type == vf50::generated::DemoEventType::DrumHit) {
                mask |= static_cast<uint16_t>(1u << 8u);
                hasEmbeddedDrums = true;
            }
        }
        if (isCustom && !hasEmbeddedDrums && !evs.empty()) {
            mask |= static_cast<uint16_t>(1u << 8u);
        }
        return mask;
    }

    // Bits 0..7 = TR1..TR8 have note data, Bit 8 = RHY track has rhythm data/pattern
    [[nodiscard]] uint16_t computeTrackDataMaskForSong() const noexcept {
        if (isUserSongIndex(selectedSongIndex_)) {
            if (!hasUserSequence(selectedSongIndex_)) {
                return 0u;
            }
            const auto& u = *userSongs_[selectedSongIndex_];
            return computeTrackDataMaskForEvents(
                std::span<const vf50::generated::DemoSongEvent>(u.events.data(), u.eventCount),
                u.trackChannels,
                true);
        }
        const auto& rom = runtimeRomBank();
        const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
        return computeTrackDataMaskForEvents(
            rom.getDemoSongEvents(idx),
            rom.factoryDemoSongs[idx].trackChannels,
            false);
    }

    void setTempoOverrideBpm(double bpm) noexcept {
        tempoRampTicksLeft_   = 0.0;
        tempoRampStepPerTick_ = 0.0;
        setTempoImmediate(bpm);
        tempoOverrideBpm_ = currentTempoBpm_;
        hasTempoOverride_ = true;
    }

    [[nodiscard]] std::string_view currentSongTitle() const noexcept {
        if (isUserSongIndex(selectedSongIndex_)) {
            return hasUserSequence(selectedSongIndex_) ? userSongs_[selectedSongIndex_]->title() : std::string_view("NewSong");
        }
        const auto& rom = runtimeRomBank();
        const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
        return rom.factoryDemoSongs[idx].title;
    }

    [[nodiscard]] int currentTempoBpmInt() const noexcept {
        if (!playing_ && !hasTempoOverride_) {
            if (isUserSongIndex(selectedSongIndex_)) {
                return hasUserSequence(selectedSongIndex_) ? static_cast<int>(userSongs_[selectedSongIndex_]->initialTempoBpm) : 120;
            }
            const auto& rom = runtimeRomBank();
            const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
            return static_cast<int>(rom.factoryDemoSongs[idx].initialTempoBpm);
        }
        return std::clamp(static_cast<int>(std::lround(currentTempoBpm_)), 30, 300);
    }

    [[nodiscard]] int currentMeasureOneBased() const noexcept {
        const uint32_t tpb = std::max<uint32_t>(1u, activeTicksPerBar_);
        if (!playing_ && !pausedWithRetainedPos_) {
            return 1;
        }
        return static_cast<int>(static_cast<uint32_t>(std::max(0.0, currentTickPos_ + 1e-6)) / tpb) + 1;
    }

    [[nodiscard]] int totalMeasures() const noexcept {
        if (isUserSongIndex(selectedSongIndex_)) {
            return hasUserSequence(selectedSongIndex_) ? static_cast<int>(std::max<uint16_t>(1u, userSongs_[selectedSongIndex_]->totalBars)) : 1;
        }
        const auto& rom = runtimeRomBank();
        const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
        return static_cast<int>(std::max<uint16_t>(1u, rom.factoryDemoSongs[idx].totalBars));
    }

    [[nodiscard]] uint64_t visualKeyMask61() const noexcept { return visualKeyMask61_; }

    void startSong(std::size_t songIdx,
                   fm::VF50FmEngine& fm,
                   drums::Ym3602DrumEngine& drums,
                   effects::Ym3413DspProcessor& fx) noexcept {
        if (!hasSavedState_) {
            savedPlayMode_       = fm.playMode();
            savedSingleIdx_      = fm.currentSingleVoiceIndex();
            savedSingleUserBank_ = fm.currentSingleVoiceFromUserBank();
            savedPerfIdx_        = fm.currentPerformanceIndex();
            savedPerfUserBank_   = fm.currentPerformanceFromUserBank();
            savedRhythmVol_      = drums.rhythmMasterVolume();
            savedFxSettings_     = fx.settings();
            hasSavedState_       = true;
        }

        selectSongIndex(songIdx);
        if (pausedWithRetainedPos_ && songIdx == lastPreparedSongIdx_ && !activeEvents_.empty() && currentTickPos_ > 0.0) {
            resumeFromRetainedPosition(fm, drums, fx);
            return;
        }
        prepareSelectedSongOnEngines(fm, drums, fx);
    }

    void stopSong(fm::VF50FmEngine& fm,
                  drums::Ym3602DrumEngine& drums,
                  effects::Ym3413DspProcessor& fx,
                  bool restorePreDemoPatch = true,
                  bool rewindToStart = false) noexcept {
        if (!playing_ && !hasSavedState_) {
            if (rewindToStart || pausedWithRetainedPos_) {
                currentTickPos_        = 0.0;
                nextEventCursor_       = 0u;
                pausedWithRetainedPos_ = false;
            }
            return;
        }

        const bool wasPlaying = playing_;
        playing_              = false;
        tempoRampTicksLeft_   = 0.0;
        tempoRampStepPerTick_ = 0.0;
        keyRefCounts_.fill(0u);
        for (auto& chNotes : channelActiveNoteCounts_) {
            chNotes.fill(0u);
        }
        channelTotalActiveNotes_.fill(0u);
        trackPeak_.fill(0.0f);
        visualKeyMask61_      = 0ULL;

        if (rewindToStart || !wasPlaying) {
            currentTickPos_        = 0.0;
            nextEventCursor_       = 0u;
            pausedWithRetainedPos_ = false;
        } else {
            pausedWithRetainedPos_ = true;
        }

        resetAllChannelControllers(fm);
        fm.allNotesOff();
        if (needsAccompanyingRhythmPattern_) {
            drums.stopPattern();
            if (rewindToStart || !wasPlaying) {
                needsAccompanyingRhythmPattern_ = false;
            }
        }
        if (!isSuppressingInternalDrums()) {
            drums.allDrumsOff();
        }

        if (restorePreDemoPatch && hasSavedState_) {
            if (!isSuppressingInternalDrums()) {
                drums.setRhythmMasterVolume(savedRhythmVol_);
            }
            fm.setPlayMode(savedPlayMode_);
            fm.selectSingleVoice(savedSingleIdx_, savedSingleUserBank_);
            fm.selectPerformance(savedPerfIdx_, savedPerfUserBank_);
            fx.setSettings(savedFxSettings_);
            hasSavedState_ = false;
        }
    }

    // Synchronize active 8-track sequence tempo and bar/beat phase to DAW AudioPlayHead (lock-free)
    void syncToHostPlayHead(double ppqPosition,
                            double tempoBpm,
                            fm::VF50FmEngine& fm,
                            drums::Ym3602DrumEngine& drums) noexcept {
        if (tempoBpm >= 30.0 && tempoBpm <= 240.0 && tempoRampTicksLeft_ <= 0.0) {
            setTempoImmediate(tempoBpm);
            drums.setTempoBpm(tempoBpm);
        }
        if (!playing_ || !std::isfinite(ppqPosition) || activeTotalTicks_ == 0u) {
            return;
        }

        const double totalD = static_cast<double>(activeTotalTicks_);
        const double targetTick = std::fmod(std::max(0.0, ppqPosition * static_cast<double>(vf50::generated::kDemoSongPpqn)), totalD);
        double diff = std::abs(targetTick - currentTickPos_);
        diff = std::min(diff, totalD - diff);
        if (diff > 12.0) {
            fm.allNotesOff();
            keyRefCounts_.fill(0u);
            visualKeyMask61_ = 0ULL;
            currentTickPos_  = targetTick;
            nextEventCursor_ = 0u;
            while (nextEventCursor_ < activeEvents_.size() &&
                   static_cast<double>(activeEvents_[nextEventCursor_].tick) < currentTickPos_) {
                const auto& ev = activeEvents_[nextEventCursor_];
                if (ev.type == vf50::generated::DemoEventType::FmProgram) {
                    if (fm.playMode() == fm::SynthPlayMode::Performance) {
                        fm.setPerformanceChannelVoice(ev.channel, ev.data1 != 0u, ev.data2);
                    } else {
                        fm.selectSingleVoice(ev.data2, ev.data1 == 0u);
                    }
                } else if (ev.type == vf50::generated::DemoEventType::FmControl) {
                    fm.controlChange(ev.channel, ev.data1, ev.data2);
                }
                ++nextEventCursor_;
            }
            if (needsAccompanyingRhythmPattern_ && drums.isSlavedToSequencer()) {
                drums.seekSlavedToMasterTick(currentTickPos_);
            }
        }
    }

    void rewindToTop(fm::VF50FmEngine& fm,
                     drums::Ym3602DrumEngine& drums,
                     effects::Ym3413DspProcessor& fx) noexcept {
        if (playing_) {
            stopSong(fm, drums, fx, true, true);
        } else {
            fm.allNotesOff();
            if (!isSuppressingInternalDrums()) {
                drums.allDrumsOff();
            }
            keyRefCounts_.fill(0u);
            for (auto& chNotes : channelActiveNoteCounts_) {
                chNotes.fill(0u);
            }
            channelTotalActiveNotes_.fill(0u);
            trackPeak_.fill(0.0f);
            visualKeyMask61_       = 0ULL;
            currentTickPos_        = 0.0;
            nextEventCursor_       = 0u;
            pausedWithRetainedPos_ = false;
            hasSavedState_         = false;
        }
    }

    void seekToMeasureOneBased(int measureOneBased, fm::VF50FmEngine& fm, drums::Ym3602DrumEngine* drums = nullptr) noexcept {
        const uint32_t tpb = resolveSelectedSongTicksPerBar();
        if (tpb == 0u) {
            return;
        }
        const int clampedMeas = std::max(1, measureOneBased);
        const double targetTick = static_cast<double>(clampedMeas - 1) * static_cast<double>(tpb);
        applySeekToTargetTick(targetTick, false, fm, drums);
    }

    void seekByMeasures(int deltaMeasures, fm::VF50FmEngine& fm, drums::Ym3602DrumEngine* drums = nullptr) noexcept {
        const uint32_t tpb = resolveSelectedSongTicksPerBar();
        if (tpb == 0u) {
            return;
        }
        const double stepTicks = static_cast<double>(deltaMeasures) * static_cast<double>(tpb);
        applySeekToTargetTick(currentTickPos_ + stepTicks, true, fm, drums);
    }

    void applySeekToTargetTick(double rawTargetTick, bool snapBarWhenStopped, fm::VF50FmEngine& fm,
                               drums::Ym3602DrumEngine* drums = nullptr) noexcept {
        const uint32_t tpb = resolveSelectedSongTicksPerBar();
        const uint32_t tot = resolveSelectedSongTotalTicks();
        if (tpb == 0u || tot == 0u) {
            return;
        }
        activeTicksPerBar_ = tpb;
        activeTotalTicks_  = tot;

        if (playing_) {
            fm.allNotesOff();
        }
        keyRefCounts_.fill(0u);
        for (auto& chNotes : channelActiveNoteCounts_) {
            chNotes.fill(0u);
        }
        channelTotalActiveNotes_.fill(0u);
        trackPeak_.fill(0.0f);
        visualKeyMask61_ = 0ULL;

        const double maxTick = static_cast<double>(std::max<uint32_t>(1u, activeTotalTicks_) - 1u);
        currentTickPos_ = std::clamp(rawTargetTick, 0.0, maxTick);
        // Snap cleanly to measure boundary when seeking while stopped
        if (!playing_) {
            if (snapBarWhenStopped) {
                const uint32_t measIdx = static_cast<uint32_t>(std::max(0.0, currentTickPos_)) / activeTicksPerBar_;
                currentTickPos_ = static_cast<double>(measIdx * activeTicksPerBar_);
            }
            pausedWithRetainedPos_ = (currentTickPos_ > 0.0);
            lastPreparedSongIdx_   = selectedSongIndex_;
            if (isUserSongIndex(selectedSongIndex_)) {
                if (hasUserSequence(selectedSongIndex_)) {
                    const auto& u = *userSongs_[selectedSongIndex_];
                    activeEvents_ = std::span<const vf50::generated::DemoSongEvent>(u.events.data(), u.eventCount);
                } else {
                    activeEvents_ = {};
                }
            } else {
                const auto& rom = runtimeRomBank();
                const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
                activeEvents_ = rom.getDemoSongEvents(idx);
            }
        }
        nextEventCursor_ = 0u;
        while (nextEventCursor_ < activeEvents_.size() &&
               static_cast<double>(activeEvents_[nextEventCursor_].tick) < currentTickPos_) {
            ++nextEventCursor_;
        }
        if (playing_ && drums != nullptr && needsAccompanyingRhythmPattern_ && drums->isSlavedToSequencer()) {
            drums->seekSlavedToMasterTick(currentTickPos_);
        }
    }

    // Advances 1 sample at native 50,000 Hz synchronous bus rate (lock-free, 0 allocations)
    void advanceFrame50kHz(fm::VF50FmEngine& fm,
                           drums::Ym3602DrumEngine& drums,
                           effects::Ym3413DspProcessor& fx) noexcept {
        if (!playing_) {
            return;
        }

        syncTrackMuteTransitions(fm, drums);

        const double prevTick = currentTickPos_;
        currentTickPos_ += ticksPer50kSample_;
        const double deltaTick = currentTickPos_ - prevTick;

        if (tempoRampTicksLeft_ > 0.0) {
            const double stepTicks = std::min(deltaTick, tempoRampTicksLeft_);
            setTempoImmediate(currentTempoBpm_ + tempoRampStepPerTick_ * stepTicks);
            tempoRampTicksLeft_ -= stepTicks;
        }

        const uint32_t targetTickInt = static_cast<uint32_t>(std::max(0.0, currentTickPos_));
        dispatchEventsThroughTick(targetTickInt, fm, drums);

        // Single shared sample-accurate master clock: advance slaved Rhythm Song/Pattern
        // by the exact same deltaTick as Sequencer Tracks 1-8 (even when RHY track is muted!).
        if (needsAccompanyingRhythmPattern_ && drums.isSlavedToSequencer()) {
            drums.setTempoBpm(currentTempoBpm_);
            drums.advancePatternClockByDeltaTicks(deltaTick);
        }

        for (std::size_t i = 0u; i < kTotalTrackCount; ++i) {
            trackPeak_[i] *= 0.99975f;
            if (trackPeak_[i] < 0.005f) {
                trackPeak_[i] = 0.0f;
            }
        }
        for (std::size_t t = 0u; t < 8u; ++t) {
            const uint8_t ch = activeTrackChannels_[t] & 0x0Fu;
            if (channelTotalActiveNotes_[ch] > 0u && !isTrackMuted(t)) {
                trackPeak_[t] = std::max(trackPeak_[t], 0.25f);
            }
        }
        if (needsAccompanyingRhythmPattern_ && drums.activeVoiceCount() > 0u && !isTrackMuted(8u)) {
            trackPeak_[8u] = std::max(trackPeak_[8u], 0.80f);
        }

        if (currentTickPos_ >= static_cast<double>(activeTotalTicks_)) {
            if (chainAllFactory_ && isFactoryDemoSongIndex(selectedSongIndex_)) {
                const std::size_t rel = selectedSongIndex_ - kFactoryDemoSongBase;
                selectedSongIndex_ = kFactoryDemoSongBase + ((rel + 1u) % vf50::generated::kDemoSongCount);
                prepareSelectedSongOnEngines(fm, drums, fx);
            } else {
                stopSong(fm, drums, fx, true, true);
            }
        }
    }

private:
    static void resetAllChannelControllers(fm::VF50FmEngine& fm) noexcept {
        for (uint8_t ch = 0u; ch < 16u; ++ch) {
            fm.controlChange(ch, 64u, 0u);   // Sustain off
            fm.controlChange(ch, 1u, 0u);    // Mod wheel 0
            fm.controlChange(ch, 7u, 127u);  // Channel volume full
            fm.controlChange(ch, 11u, 127u); // Expression full
            fm.channelPressure(ch, 0u);      // Aftertouch 0
            fm.pitchBend(ch, 8192);          // Center pitch bend
        }
    }

    void setTempoImmediate(double bpm) noexcept {
        currentTempoBpm_ = std::clamp(bpm, 30.0, 300.0);
        ticksPer50kSample_ = (currentTempoBpm_ * static_cast<double>(vf50::generated::kDemoSongPpqn))
                           / (60.0 * 50000.0);
    }

    [[nodiscard]] std::size_t resolveTrackForChannel(uint8_t ch0To15) const noexcept {
        const uint8_t c = ch0To15 & 0x0Fu;
        for (std::size_t t = 0u; t < 8u; ++t) {
            if ((activeTrackChannels_[t] & 0x0Fu) == c) {
                return t;
            }
        }
        return static_cast<std::size_t>(c & 0x07u);
    }

    [[nodiscard]] uint32_t resolveSelectedSongTicksPerBar() const noexcept {
        if (isUserSongIndex(selectedSongIndex_)) {
            return hasUserSequence(selectedSongIndex_)
                       ? std::max<uint32_t>(48u, userSongs_[selectedSongIndex_]->ticksPerBar)
                       : 192u;
        }
        const auto& rom = runtimeRomBank();
        const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
        return std::max<uint32_t>(48u, rom.factoryDemoSongs[idx].ticksPerBar);
    }

    [[nodiscard]] uint32_t resolveSelectedSongTotalTicks() const noexcept {
        const uint32_t tpb = resolveSelectedSongTicksPerBar();
        if (isUserSongIndex(selectedSongIndex_)) {
            return hasUserSequence(selectedSongIndex_)
                       ? std::max<uint32_t>(tpb, userSongs_[selectedSongIndex_]->totalTicks + 96u)
                       : tpb;
        }
        const auto& rom = runtimeRomBank();
        const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
        return std::max<uint32_t>(tpb, rom.factoryDemoSongs[idx].totalTicks + 96u);
    }

    void resumeFromRetainedPosition(fm::VF50FmEngine& fm,
                                    drums::Ym3602DrumEngine& drums,
                                    effects::Ym3413DspProcessor& fx) noexcept {
        const bool suppressDrums = isSuppressingInternalDrums();
        if (!suppressDrums) {
            drums.stopPattern();
            drums.allDrumsOff();
            drums.setRhythmMasterVolume(85u);
        }
        fm.allNotesOff();
        resetAllChannelControllers(fm);
        keyRefCounts_.fill(0u);
        for (auto& chNotes : channelActiveNoteCounts_) {
            chNotes.fill(0u);
        }
        channelTotalActiveNotes_.fill(0u);
        trackPeak_.fill(0.0f);
        visualKeyMask61_       = 0ULL;
        pausedWithRetainedPos_ = false;
        tempoRampTicksLeft_    = 0.0;
        tempoRampStepPerTick_  = 0.0;

        if (isUserSongIndex(selectedSongIndex_)) {
            if (!hasUserSequence(selectedSongIndex_)) {
                playing_ = false;
                hasSavedState_ = false;
                needsAccompanyingRhythmPattern_ = false;
                return;
            }
            const auto& u = *userSongs_[selectedSongIndex_];
            activeEvents_ = std::span<const vf50::generated::DemoSongEvent>(u.events.data(), u.eventCount);
            activeTrackChannels_ = u.trackChannels;
            bool hasEmbeddedDrums = false;
            for (const auto& ev : activeEvents_) {
                if (ev.type == vf50::generated::DemoEventType::DrumHit) {
                    hasEmbeddedDrums = true;
                    break;
                }
            }
            needsAccompanyingRhythmPattern_ = !hasEmbeddedDrums;
            if (u.playMode == 0u) {
                fm.setPlayMode(fm::SynthPlayMode::Single);
                fm.selectSingleVoice(u.perfNumber, u.perfPresetBank == 0u);
            } else {
                fm.setPlayMode(fm::SynthPlayMode::Performance);
                fm.selectPerformance(u.perfNumber, u.perfPresetBank == 0u);
            }
            fx.setSettings(fm.currentPresetEffectSettings());
            setTempoImmediate(hasTempoOverride_ ? tempoOverrideBpm_ : static_cast<double>(u.initialTempoBpm));
        } else {
            needsAccompanyingRhythmPattern_ = false;
            const auto& rom = runtimeRomBank();
            const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
            const auto& song = rom.factoryDemoSongs[idx];
            activeEvents_ = rom.getDemoSongEvents(idx);
            activeTrackChannels_ = song.trackChannels;
            fm.setPlayMode(fm::SynthPlayMode::Performance);
            fm.selectPerformance(song.perfNumber, song.perfPresetBank == 0u);
            fx.setSettings(fm.currentPresetEffectSettings());
            setTempoImmediate(hasTempoOverride_ ? tempoOverrideBpm_ : static_cast<double>(song.initialTempoBpm));
        }

        // Re-apply program/control/tempo state up to the retained cursor
        const std::size_t resumeLimit = std::min(nextEventCursor_, activeEvents_.size());
        for (std::size_t i = 0u; i < resumeLimit; ++i) {
            const auto& ev = activeEvents_[i];
            if (ev.type == vf50::generated::DemoEventType::FmProgram) {
                if (fm.playMode() == fm::SynthPlayMode::Performance) {
                    fm.setPerformanceChannelVoice(ev.channel, ev.data1 != 0u, ev.data2);
                } else {
                    fm.selectSingleVoice(ev.data2, ev.data1 == 0u);
                }
            } else if (ev.type == vf50::generated::DemoEventType::FmControl) {
                fm.controlChange(ev.channel, ev.data1, ev.data2);
            } else if (ev.type == vf50::generated::DemoEventType::TempoChange && ev.data1 == 0u) {
                setTempoImmediate(static_cast<double>(ev.data2));
            }
        }

        playing_ = true;
        const uint32_t targetTickInt = static_cast<uint32_t>(std::max(0.0, currentTickPos_));
        dispatchEventsThroughTick(targetTickInt, fm, drums);
    }

    void prepareSelectedSongOnEngines(fm::VF50FmEngine& fm,
                                      drums::Ym3602DrumEngine& drums,
                                      effects::Ym3413DspProcessor& fx) noexcept {
        const bool suppressDrums = isSuppressingInternalDrums();
        if (!suppressDrums) {
            drums.stopPattern();
            drums.allDrumsOff();
            drums.setRhythmMasterVolume(85u);
        }
        fm.allNotesOff();
        resetAllChannelControllers(fm);

        keyRefCounts_.fill(0u);
        for (auto& chNotes : channelActiveNoteCounts_) {
            chNotes.fill(0u);
        }
        channelTotalActiveNotes_.fill(0u);
        trackPeak_.fill(0.0f);
        visualKeyMask61_       = 0ULL;
        currentTickPos_        = 0.0;
        nextEventCursor_       = 0u;
        pausedWithRetainedPos_ = false;
        lastPreparedSongIdx_   = selectedSongIndex_;
        tempoRampTicksLeft_    = 0.0;
        tempoRampStepPerTick_  = 0.0;

        if (isUserSongIndex(selectedSongIndex_)) {
            if (!hasUserSequence(selectedSongIndex_)) {
                playing_ = false;
                hasSavedState_ = false;
                needsAccompanyingRhythmPattern_ = false;
                activeEvents_ = {};
                return;
            }
            const auto& u        = *userSongs_[selectedSongIndex_];
            activeEvents_        = std::span<const vf50::generated::DemoSongEvent>(u.events.data(), u.eventCount);
            activeTrackChannels_ = u.trackChannels;
            activeTicksPerBar_   = std::max<uint32_t>(48u, u.ticksPerBar);
            activeTotalTicks_    = std::max<uint32_t>(activeTicksPerBar_, u.totalTicks + 96u);

            bool hasEmbeddedDrums = false;
            for (const auto& ev : activeEvents_) {
                if (ev.type == vf50::generated::DemoEventType::DrumHit) {
                    hasEmbeddedDrums = true;
                    break;
                }
            }
            needsAccompanyingRhythmPattern_ = !hasEmbeddedDrums;

            if (u.playMode == 0u) {
                fm.setPlayMode(fm::SynthPlayMode::Single);
                fm.selectSingleVoice(u.perfNumber, u.perfPresetBank == 0u);
            } else {
                fm.setPlayMode(fm::SynthPlayMode::Performance);
                fm.selectPerformance(u.perfNumber, u.perfPresetBank == 0u);
            }
            fx.setSettings(fm.currentPresetEffectSettings());
            setTempoImmediate(hasTempoOverride_ ? tempoOverrideBpm_ : static_cast<double>(u.initialTempoBpm));
        } else {
            needsAccompanyingRhythmPattern_ = false;
            const auto& rom = runtimeRomBank();
            const std::size_t idx = std::min(selectedSongIndex_ - kFactoryDemoSongBase, rom.factoryDemoSongs.size() - 1u);
            const auto& song = rom.factoryDemoSongs[idx];
            activeEvents_        = rom.getDemoSongEvents(idx);
            activeTrackChannels_ = song.trackChannels;
            activeTicksPerBar_   = std::max<uint32_t>(48u, song.ticksPerBar);
            activeTotalTicks_    = std::max<uint32_t>(activeTicksPerBar_, song.totalTicks + 96u);

            if (activeEvents_.empty()) {
                playing_ = false;
                hasSavedState_ = false;
                return;
            }

            fm.setPlayMode(fm::SynthPlayMode::Performance);
            fm.selectPerformance(song.perfNumber, song.perfPresetBank == 0u);
            fx.setSettings(fm.currentPresetEffectSettings());
            setTempoImmediate(hasTempoOverride_ ? tempoOverrideBpm_ : static_cast<double>(song.initialTempoBpm));
        }

        playing_ = true;
        dispatchEventsThroughTick(0u, fm, drums);
    }

    void dispatchEventsThroughTick(uint32_t targetTick,
                                   fm::VF50FmEngine& fm,
                                   drums::Ym3602DrumEngine& drums) noexcept {
        using vf50::generated::DemoEventType;

        while (nextEventCursor_ < activeEvents_.size()) {
            const auto& ev = activeEvents_[nextEventCursor_];
            if (ev.tick > targetTick) {
                break;
            }
            ++nextEventCursor_;

            const std::size_t trkIdx = resolveTrackForChannel(ev.channel);
            const uint8_t ch         = ev.channel & 0x0Fu;

            switch (ev.type) {
                case DemoEventType::FmNoteOn: {
                    if (isTrackMuted(trkIdx)) {
                        break;
                    }
                    fm.noteOn(ev.channel, ev.data1, ev.data2);
                    const uint8_t note = ev.data1 & 0x7Fu;
                    if (ev.data2 > 0u) {
                        ++dispatchedNoteOns_;
                        if (channelActiveNoteCounts_[ch][note] < 255u) {
                            ++channelActiveNoteCounts_[ch][note];
                        }
                        if (channelTotalActiveNotes_[ch] < 255u) {
                            ++channelTotalActiveNotes_[ch];
                        }
                        if (trkIdx < 8u) {
                            const float velNorm = static_cast<float>(ev.data2) / 127.0f;
                            trackPeak_[trkIdx] = std::max(trackPeak_[trkIdx], velNorm);
                        }
                    } else {
                        if (channelActiveNoteCounts_[ch][note] > 0u) {
                            --channelActiveNoteCounts_[ch][note];
                        }
                        if (channelTotalActiveNotes_[ch] > 0u) {
                            --channelTotalActiveNotes_[ch];
                        }
                    }
                    if (ev.data1 >= 36u && ev.data1 <= 96u && ev.data2 > 0u) {
                        const std::size_t k = static_cast<std::size_t>(ev.data1 - 36u);
                        if (keyRefCounts_[k] < 255u) {
                            ++keyRefCounts_[k];
                        }
                        visualKeyMask61_ |= (uint64_t{ 1 } << k);
                    }
                    break;
                }
                case DemoEventType::FmNoteOff: {
                    fm.noteOff(ev.channel, ev.data1);
                    const uint8_t note = ev.data1 & 0x7Fu;
                    if (channelActiveNoteCounts_[ch][note] > 0u) {
                        --channelActiveNoteCounts_[ch][note];
                    }
                    if (channelTotalActiveNotes_[ch] > 0u) {
                        --channelTotalActiveNotes_[ch];
                    }
                    if (ev.data1 >= 36u && ev.data1 <= 96u) {
                        const std::size_t k = static_cast<std::size_t>(ev.data1 - 36u);
                        if (keyRefCounts_[k] > 0u) {
                            --keyRefCounts_[k];
                            if (keyRefCounts_[k] == 0u) {
                                visualKeyMask61_ &= ~(uint64_t{ 1 } << k);
                            }
                        }
                    }
                    break;
                }
                case DemoEventType::FmControl:
                    if (!isTrackMuted(trkIdx)) {
                        fm.controlChange(ev.channel, ev.data1, ev.data2);
                    }
                    break;
                case DemoEventType::FmProgram:
                    if (fm.playMode() == fm::SynthPlayMode::Performance) {
                        fm.setPerformanceChannelVoice(ev.channel, ev.data1 != 0u, ev.data2);
                    } else {
                        fm.selectSingleVoice(ev.data2, ev.data1 == 0u);
                    }
                    break;
                case DemoEventType::FmPitchBend: {
                    if (isTrackMuted(trkIdx)) {
                        break;
                    }
                    const int bend14 = static_cast<int>(ev.data1 & 0x7Fu)
                                     | (static_cast<int>(ev.data2 & 0x7Fu) << 7);
                    fm.pitchBend(ev.channel, bend14);
                    break;
                }
                case DemoEventType::FmAftertouch:
                    if (!isTrackMuted(trkIdx)) {
                        fm.channelPressure(ev.channel, ev.data1);
                    }
                    break;
                case DemoEventType::DrumHit:
                    if (!isSuppressingInternalDrums() && !isTrackMuted(8u)) {
                        drums.triggerDrumAccentLevel(ev.data1, ev.data2);
                        const float velNorm = (ev.data2 <= 9u)
                                                  ? (static_cast<float>(ev.data2) / 9.0f)
                                                  : (static_cast<float>(ev.data2) / 127.0f);
                        trackPeak_[8u] = std::max(trackPeak_[8u], velNorm);
                    }
                    break;
                case DemoEventType::TempoChange:
                    if (ev.data1 == 0u) {
                        tempoRampTicksLeft_   = 0.0;
                        tempoRampStepPerTick_ = 0.0;
                        setTempoImmediate(static_cast<double>(ev.data2));
                    } else {
                        const double deltaBpm = static_cast<double>(static_cast<int8_t>(ev.data1));
                        if (ev.data2 == 0u) {
                            tempoRampTicksLeft_   = 0.0;
                            tempoRampStepPerTick_ = 0.0;
                            setTempoImmediate(currentTempoBpm_ + deltaBpm);
                        } else {
                            tempoRampTicksLeft_   = static_cast<double>(ev.data2) * 48.0;
                            tempoRampStepPerTick_ = deltaBpm / tempoRampTicksLeft_;
                        }
                    }
                    break;
                case DemoEventType::RhythmVolDelta:
                    if (!isSuppressingInternalDrums() && !isTrackMuted(8u)) {
                        drums.adjustRhythmMasterVolumeDelta(static_cast<int>(static_cast<int8_t>(ev.data1)));
                    }
                    break;
            }
        }
    }

    bool playing_                        = false;
    bool pausedWithRetainedPos_          = false;
    bool chainAllFactory_                = false;
    bool needsAccompanyingRhythmPattern_ = false;
    std::atomic<bool> suppressInternalDrums_{ false };
    std::size_t selectedSongIndex_       = 0u;
    std::size_t lastPreparedSongIdx_     = SIZE_MAX;
    const vf50::rom::VF50RomImage* romBank_ = &vf50::rom::getDefaultRomImage();

    std::span<const vf50::generated::DemoSongEvent> activeEvents_{};
    std::array<uint8_t, 8> activeTrackChannels_{ 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
    uint32_t activeTicksPerBar_ = 192u;
    uint32_t activeTotalTicks_  = 192u;
    std::size_t nextEventCursor_ = 0u;

    double currentTempoBpm_       = 120.0;
    double ticksPer50kSample_     = (120.0 * 48.0) / (60.0 * 50000.0);
    double currentTickPos_        = 0.0;
    double tempoRampTicksLeft_    = 0.0;
    double tempoRampStepPerTick_  = 0.0;
    bool   hasTempoOverride_      = false;
    double tempoOverrideBpm_      = 120.0;

    std::array<std::atomic<bool>, kTotalTrackCount> trackMuted_{};
    std::array<bool, kTotalTrackCount> prevMuted_{};
    std::array<std::array<uint8_t, 128>, 16> channelActiveNoteCounts_{};
    std::array<uint8_t, 16> channelTotalActiveNotes_{};
    std::array<float, kTotalTrackCount> trackPeak_{};
    std::array<std::pair<uint8_t, uint8_t>, 256> pendingMidiNoteOffs_{};
    std::size_t pendingMidiNoteOffCount_ = 0u;

    std::array<uint8_t, 61> keyRefCounts_{};
    uint64_t visualKeyMask61_   = 0ULL;
    uint64_t dispatchedNoteOns_ = 0ULL;

    // Saved pre-Demo state so stopping Demo restores exact user patch/mode
    bool hasSavedState_ = false;
    fm::SynthPlayMode savedPlayMode_ = fm::SynthPlayMode::Single;
    std::size_t savedSingleIdx_ = 0u;
    bool savedSingleUserBank_ = false;
    std::size_t savedPerfIdx_ = 0u;
    bool savedPerfUserBank_ = false;
    uint8_t savedRhythmVol_ = 85u;
    vf50::generated::DspEffectSettings savedFxSettings_{};

    std::array<std::unique_ptr<CustomSeqSongBuffer>, kUserSongCount> userSongs_{};
};

} // namespace vf50::dsp
