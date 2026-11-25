#pragma once

#include "ROM/VF50RomBank.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace vf50::dsp::drums {

enum class RhythmEffectMode : uint8_t {
    Off = 0, // Dry MIX B only (bypasses YM3413)
    On  = 1, // Routed through MIX A + MIX B with Effect Balance
    Mix = 2  // Full dry MIX B + MIX A wet send
};

struct DrumBusFrame {
    float   dryOnlyL     = 0.0f;
    float   dryOnlyR     = 0.0f;
    float   efctDryL     = 0.0f;
    float   efctDryR     = 0.0f;
    float   wetSendL     = 0.0f;
    float   wetSendR     = 0.0f;
    float   rawDrumL     = 0.0f;
    float   rawDrumR     = 0.0f;
    float   clickSample  = 0.0f;
    uint8_t activeVoices = 0u;
};

struct DrumInstrumentState {
    uint8_t volume         = 15u;   // 0..15 (15 = 0 dB, 0 = mute)
    int8_t  pan            = 0;     // -3..+3 (L3..C..R3)
    float   pitchSemitones = 0.0f;  // -24.0..+24.0 semitones (0.0 = native 25 kHz)
    uint8_t accent         = 5u;    // 1..9 accent sensitivity (5 = nominal 0 dB)
    uint8_t outAssign      = 3u;    // 0=Off, 1=Out I (L), 2=Out II (R), 3=Out I+II (Stereo)
    uint8_t midiNote       = 31u;   // 0..127 assigned trigger note (default 31..91 = G0..G5)
    uint8_t chokeGroup     = 7u;    // 0 = exclusive Hi-Hat choke group
};

class Ym3602DrumEngine {
public:
    static constexpr std::size_t kMaxVoices     = 8;
    static constexpr std::size_t kNumDrums      = vf50::generated::kDrumInstrumentCount; // 61
    static constexpr double      kNativeBusRate = 50000.0;

    Ym3602DrumEngine() noexcept;

    void setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadDefaults = false) noexcept;
    [[nodiscard]] const vf50::rom::VF50RomImage& runtimeRomBank() const noexcept {
        return (romBank_ != nullptr) ? *romBank_ : vf50::rom::getDefaultRomImage();
    }

    void reset() noexcept;
    void resetInstrumentDefaults() noexcept;

    // Per-instrument configuration (all 61 drums)
    void setInstrumentVolume(std::size_t drumIndex, uint8_t volume0To15) noexcept;
    void setInstrumentPan(std::size_t drumIndex, int8_t panNeg3ToPos3) noexcept;
    void setInstrumentPitchSemitones(std::size_t drumIndex, float semitones) noexcept;
    void setInstrumentAccent(std::size_t drumIndex, uint8_t accent1To9) noexcept;
    void setInstrumentOutAssign(std::size_t drumIndex, uint8_t outAssign0To3) noexcept;
    void setInstrumentMidiNote(std::size_t drumIndex, uint8_t midiNote) noexcept;
    void setInstrumentState(std::size_t drumIndex, const DrumInstrumentState& state) noexcept;
    void loadPresetKeyMap(std::size_t mapIndex0To2) noexcept;
    void loadCustomKeyMap(const std::array<int8_t, 61>& keyToDrumMap) noexcept;
    void loadCustomKeyMapAtBaseNote(const std::array<int8_t, 61>& keyToDrumMap, uint8_t baseMidiNote) noexcept;
    void applyKeyAssignLayout(uint8_t layout0To3) noexcept;
    void setActiveKeyAssignLayoutTag(uint8_t layout0To3) noexcept { activeKeyAssignLayout_ = (layout0To3 > 3u) ? 3u : layout0To3; }
    [[nodiscard]] uint8_t activeKeyAssignLayout() const noexcept { return activeKeyAssignLayout_; }
    [[nodiscard]] const DrumInstrumentState& instrumentState(std::size_t drumIndex) const noexcept;
    [[nodiscard]] std::string_view instrumentName(std::size_t drumIndex) const noexcept;
    [[nodiscard]] int drumForMidiNote(uint8_t midiNote) const noexcept;

    // Global Rhythm Setup
    void setRhythmReceiveChannel(uint8_t chZeroBasedOrOff) noexcept { receiveChannel_ = chZeroBasedOrOff; }
    [[nodiscard]] uint8_t rhythmReceiveChannel() const noexcept { return receiveChannel_; }
    void setRhythmMasterVolume(uint8_t vol0To99) noexcept;
    [[nodiscard]] uint8_t rhythmMasterVolume() const noexcept { return masterVolume0To99_; }
    void adjustRhythmMasterVolumeDelta(int delta) noexcept;
    void setEffectMode(RhythmEffectMode mode) noexcept { effectMode_ = mode; }
    [[nodiscard]] RhythmEffectMode effectMode() const noexcept { return effectMode_; }

    // Triggering via MIDI or direct Drum Index (0..60)
    void noteOn(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept;
    [[nodiscard]] int noteOnWithResult(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept;
    [[nodiscard]] int triggerKeyDirect(uint8_t midiNote, uint8_t velocity) noexcept;
    void triggerDrum(std::size_t drumIndex, uint8_t midiVelocity) noexcept;
    void triggerDrumAccentLevel(std::size_t drumIndex, uint8_t vf50VelLevel1To9) noexcept;
    void allDrumsOff() noexcept;
    void silenceActiveDrumVoices() noexcept;
    void setOutputTriggerMuted(bool muted) noexcept { outputTriggerMuted_ = muted; }
    [[nodiscard]] bool isOutputTriggerMuted() const noexcept { return outputTriggerMuted_; }
    void setSlavedToSequencer(bool slaved) noexcept { slavedToSequencer_ = slaved; }
    [[nodiscard]] bool isSlavedToSequencer() const noexcept { return slavedToSequencer_; }
    void advancePatternClockByDeltaTicks(double deltaTicks) noexcept;
    void seekSlavedToMasterTick(double masterTick48) noexcept;

    // Built-in 48 PPQN Factory & User Rhythm Pattern Sequencer
    struct RhythmSongPatternRef {
        const vf50::generated::RhythmNoteEvent* events = nullptr;
        uint16_t eventCount  = 0u;
        uint16_t totalTicks  = 192u;
        uint16_t ticksPerBar = 192u;
        uint8_t  bars        = 1u;
    };

    void startPattern(std::size_t patternIndex0To99, double tempoBpm, bool loop = true) noexcept;
    void startCustomPattern(const vf50::generated::RhythmNoteEvent* events,
                            std::size_t eventCount,
                            uint16_t totalTicks,
                            double tempoBpm,
                            bool loop = true) noexcept;
    void startOwnedCustomPattern(const vf50::generated::RhythmNoteEvent* events,
                                 std::size_t eventCount,
                                 uint16_t totalTicks,
                                 uint16_t ticksPerBar,
                                 double tempoBpm,
                                 bool loop = true) noexcept;
    void updateActiveCustomPatternInPlace(const vf50::generated::RhythmNoteEvent* events,
                                          std::size_t eventCount,
                                          uint16_t totalTicks,
                                          uint16_t ticksPerBar) noexcept;
    void queueNextPattern(std::size_t factoryIdx0To99) noexcept;
    void queueNextOwnedCustomPattern(const vf50::generated::RhythmNoteEvent* events,
                                     std::size_t eventCount,
                                     uint16_t totalTicks,
                                     uint16_t ticksPerBar) noexcept;
    void clearQueuedPattern() noexcept { hasQueuedPattern_ = false; }
    void rewindPattern() noexcept;
    void stopPattern() noexcept;

    void setMetronomeClick(bool enabled, uint16_t intervalTicks48Ppqn, uint16_t ticksPerBar48Ppqn) noexcept;

    void startRhythmSong(const vf50::generated::RhythmSongData& song,
                         const std::array<RhythmSongPatternRef, 100>& userPatterns,
                         uint16_t startPartIdx,
                         uint16_t startMeasure,
                         double initialTempoBpm) noexcept;
    void updateSongUserPatternRefs(const std::array<RhythmSongPatternRef, 100>& userPatterns) noexcept {
        songUserPatternRefs_ = userPatterns;
    }
    void stopRhythmSong() noexcept;

    void setTempoBpm(double tempoBpm) noexcept;
    void syncPatternToPpq(double ppqPosition, double tempoBpm) noexcept;
    [[nodiscard]] bool isPatternPlaying() const noexcept { return patternPlaying_; }
    [[nodiscard]] bool isSongPlaying() const noexcept { return songPlaying_; }
    [[nodiscard]] bool isUsingCustomPattern() const noexcept { return usingCustomPattern_; }
    [[nodiscard]] bool isNextPatternQueued() const noexcept { return hasQueuedPattern_; }
    [[nodiscard]] uint16_t currentPatternTickInt() const noexcept {
        return static_cast<uint16_t>(std::max(0.0, currentTickPos_));
    }
    [[nodiscard]] double currentPatternTickDouble() const noexcept { return currentTickPos_; }
    [[nodiscard]] double totalElapsedTicks() const noexcept { return totalElapsedTicks_; }
    [[nodiscard]] uint32_t patternWrapCount() const noexcept { return patternWrapCount_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t currentSongPartOneBased() const noexcept { return songPlaying_ ? static_cast<uint16_t>(songPartIdx_ + 1u) : 1u; }
    [[nodiscard]] uint16_t currentSongMeasureOneBased() const noexcept { return songCurrentMeasure_; }
    [[nodiscard]] bool currentSongPtnIsUser() const noexcept { return songCurrentPtnIsUser_; }
    [[nodiscard]] uint8_t currentSongPtnIndex() const noexcept { return songCurrentPtnIdx_; }
    [[nodiscard]] uint32_t songBarsPlayedCount() const noexcept { return songBarsPlayedCount_.load(std::memory_order_relaxed); }
    [[nodiscard]] int currentTempoBpmInt() const noexcept { return static_cast<int>(std::lround(tempoBpm_)); }

    [[nodiscard]] uint8_t consumeRecentVoiceHitMask() noexcept {
        return recentVoiceHitMask_.exchange(0u, std::memory_order_acq_rel);
    }
    [[nodiscard]] uint8_t activeVoiceSlotMask() const noexcept;
    [[nodiscard]] uint64_t totalTriggerCount() const noexcept {
        return totalTriggerCount_.load(std::memory_order_relaxed);
    }

    // Native 50,000 Hz synchronous bus frame (25 kHz -> 50 kHz 4-point cubic Hermite interpolation)
    [[nodiscard]] DrumBusFrame renderFrame50kHz() noexcept;

    // Standalone arbitrary host sample-rate rendering helper (44.1 / 48 / 88.2 / 96 kHz via 4-point cubic Hermite)
    void prepareHostResampler(double hostSampleRate) noexcept;
    [[nodiscard]] DrumBusFrame renderFrameAtHostRate() noexcept;

    [[nodiscard]] std::size_t activeVoiceCount() const noexcept;

private:
    struct OprwVoice {
        bool        active        = false;
        std::size_t drumIndex     = 0u;
        uint8_t     chokeGroup    = 7u;
        uint64_t    triggerOrder  = 0u;
        double      phase25k      = 0.0;   // Sample position in 25 kHz ROM space
        double      stepPer50k    = 0.5;   // 0.5 at 0 semitones (25 kHz / 50 kHz)
        float       gain          = 1.0f;
        float       panL          = 0.70710678f;
        float       panR          = 0.70710678f;
        float       envGain       = 1.0f;
        float       envDecayMul   = 1.0f;
        float       chokeGain     = 1.0f;
        float       chokeStep     = 0.0f;
        float       aaFilterZ1    = 0.0f;
    };

    void rebuildMidiNoteMap() noexcept;
    [[nodiscard]] std::size_t allocateVoice(std::size_t drumIndex, uint8_t chokeGroup) noexcept;
    void advancePatternClock50kHz() noexcept;
    bool advanceToNextPlayableSongPart() noexcept;
    void loadSongPatternForCurrentPart() noexcept;
    void triggerClickSound(bool isDownbeat) noexcept;

    const vf50::rom::VF50RomImage* romBank_ = &vf50::rom::getDefaultRomImage();

    std::array<DrumInstrumentState, kNumDrums> instruments_{};
    std::array<int8_t, 128> midiNoteToDrum_{};
    std::array<OprwVoice, kMaxVoices> voices_{};

    uint8_t receiveChannel_ = 9u; // Default MIDI Ch 10 (0-based 9); 16 = Omni; >=17 = Off
    uint8_t activeKeyAssignLayout_ = 0u; // 0=PRESET 1 (RX), 1=PRESET 2 (C3), 2=PRESET 3 (Clavinova), 3=USER/SEQ (C1..C6)
    uint8_t masterVolume0To99_ = 85u;
    float targetMasterGain_ = 0.85f * 0.85f;
    float smoothMasterGain_ = 0.85f * 0.85f;
    RhythmEffectMode effectMode_ = RhythmEffectMode::Mix;
    uint64_t triggerCounter_ = 1u;
    std::atomic<uint8_t> recentVoiceHitMask_{ 0u };
    std::atomic<uint64_t> totalTriggerCount_{ 0u };

    // 48 PPQN Pattern Player state
    bool patternPlaying_ = false;
    bool patternLoop_ = true;
    bool usingCustomPattern_ = false;
    bool outputTriggerMuted_ = false;
    bool slavedToSequencer_ = false;
    std::size_t currentPatternIndex_ = 0u;
    const vf50::generated::RhythmNoteEvent* customEvents_ = nullptr;
    std::size_t customEventCount_ = 0u;
    uint16_t customTotalTicks_ = 192u;
    uint16_t customTicksPerBar_ = 192u;
    std::size_t nextEventCursor_ = 0u;
    double tempoBpm_ = 120.0;
    double ticksPer50kSample_ = (120.0 * 48.0) / (60.0 * 50000.0);
    double currentTickPos_ = 0.0;
    double totalElapsedTicks_ = 0.0;

    std::array<vf50::generated::RhythmNoteEvent, 256> ownedCustomEvents_{};
    bool hasQueuedPattern_ = false;
    bool queuedIsCustom_ = false;
    std::size_t queuedFactoryIdx_ = 0u;
    std::array<vf50::generated::RhythmNoteEvent, 256> queuedCustomEvents_{};
    std::size_t queuedCustomCount_ = 0u;
    uint16_t queuedTotalTicks_ = 192u;
    uint16_t queuedTicksPerBar_ = 192u;
    std::atomic<uint32_t> patternWrapCount_{ 0u };

    // Metronome click synthesizer (zero PCM voice stealing)
    bool clickEnabled_ = false;
    uint16_t clickIntervalTicks_ = 48u;
    uint16_t clickTicksPerBar_ = 192u;
    double nextClickTick_ = 0.0;
    float clickEnv_ = 0.0f;
    double clickPhase_ = 0.0;
    double clickStep_ = 0.0;

    // Rhythm Song state (allocated once in ctor, zero audio-thread allocs)
    struct RepeatFrame {
        uint16_t startPartIdx = 0u;
        uint16_t endPartIdx   = 0xFFFFu;
        uint8_t  remaining    = 0u;
    };
    std::unique_ptr<vf50::generated::RhythmSongData> activeSong_{};
    std::array<RhythmSongPatternRef, 100> songUserPatternRefs_{};
    bool songPlaying_ = false;
    uint16_t songPartIdx_ = 0u;
    uint16_t songMeasureAtPartStart_ = 1u;
    uint16_t songCurrentMeasure_ = 1u;
    uint8_t songBaseVolume_ = 85u;
    int songEffectiveVolOffset_ = 0;
    double songTempoRampRemainingTicks_ = 0.0;
    double songTempoRampStepPerTick_ = 0.0;
    bool songCurrentPtnIsUser_ = false;
    uint8_t songCurrentPtnIdx_ = 0u;
    uint8_t songCurrentPtnBars_ = 1u;
    uint16_t songCurrentTicksPerBar_ = 192u;
    std::array<RepeatFrame, 16> repeatStack_{};
    std::size_t repeatDepth_ = 0u;
    std::atomic<uint32_t> songBarsPlayedCount_{ 0u };

    // Host rate 4-point cubic Hermite resampler state (when used standalone outside VF50SynthEngine)
    double hostSampleRate_ = 48000.0;
    double hostPhaseStep_ = 50000.0 / 48000.0;
    double hostFracPos_ = 0.0;
    std::array<DrumBusFrame, 4> hist50k_{};
};

} // namespace vf50::dsp::drums
