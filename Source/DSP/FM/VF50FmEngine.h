#pragma once

#include "DSP/FM/OpzLfo.h"
#include "DSP/FM/OpzVoice.h"
#include "ROM/VF50RomBank.h"
#include "State/VF50ClassicSystemTypes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vf50::dsp::fm {

enum class SynthPlayMode : uint8_t {
    Single = 0,
    Performance = 1
};

struct FmBusFrame {
    float dryOnlyL  = 0.0f; // Instruments with EFCTE == 0 (bypass YM3413 wet/balance)
    float dryOnlyR  = 0.0f;
    float efctDryL  = 0.0f; // Instruments with EFCTE == 1 (MIX B dry side)
    float efctDryR  = 0.0f;
    float wetSendL  = 0.0f; // Instruments with EFCTE == 1 (MIX A wet send into YM3413)
    float wetSendR  = 0.0f;
    std::array<float, 8> partPeak{}; // Per-part peak magnitude (Single mode maps to index 0)
    std::array<float, 8> partL{};    // Per-part un-weighted dry stereo left (Single mode maps to index 0)
    std::array<float, 8> partR{};    // Per-part un-weighted dry stereo right (Single mode maps to index 0)
    std::array<uint8_t, 8> partActiveVoices{}; // Active voice count per part (0..16)
    uint8_t totalActiveVoices = 0u;            // Total active FM voices (0..16)
};

struct Lfo2OverrideConfig {
    bool    enabled = false;
    uint8_t wave    = 2u;
    uint8_t speed   = 35u;
    uint8_t delay   = 0u;
    uint8_t pmd     = 0u;
    uint8_t amd     = 0u;
    uint8_t sync    = 0u;
};

class VF50FmEngine {
public:
    static constexpr std::size_t kTotalVoices = 16;
    static constexpr std::size_t kMaxPerformanceParts = 8;

    VF50FmEngine() noexcept;

    void setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadFactoryDefaults = false) noexcept;
    [[nodiscard]] const vf50::rom::VF50RomImage& runtimeRomBank() const noexcept {
        return (romBank_ != nullptr) ? *romBank_ : vf50::rom::getDefaultRomImage();
    }

    void reset() noexcept;

    // Mode & Preset Selection
    void setPlayMode(SynthPlayMode mode) noexcept;
    [[nodiscard]] SynthPlayMode playMode() const noexcept { return playMode_; }

    void selectSingleVoice(std::size_t presetIndex, bool fromInternalBank = false) noexcept;
    void applyAtomicSingleVoicePatch(const vf50::generated::FactoryVoice& voice,
                                     std::size_t presetIndex,
                                     bool fromInternalBank,
                                     const QuickEditOffsets& qe,
                                     uint8_t opEnableMask = 0x0Fu,
                                     bool singleEffectSend = true,
                                     bool dampExistingVoices = true) noexcept;
    [[nodiscard]] std::size_t currentSingleVoiceIndex() const noexcept { return currentVoiceIndex_; }
    [[nodiscard]] bool currentSingleVoiceFromUserBank() const noexcept { return currentVoiceUserBank_; }
    [[nodiscard]] const vf50::generated::FactoryVoice& activeSingleVoice() const noexcept { return activeVoice_; }
    vf50::generated::FactoryVoice& mutableActiveSingleVoice() noexcept { return activeVoice_; }
    void setActiveSingleVoiceDirect(const vf50::generated::FactoryVoice& voice) noexcept;

    void selectPerformance(std::size_t perfIndex, bool fromInternalBank = false) noexcept;
    void applyAtomicPerformancePatch(const vf50::generated::FactoryPerformance& perf,
                                     std::size_t perfIndex,
                                     bool fromInternalBank,
                                     const std::array<int8_t, kMaxPerformanceParts>& pans) noexcept;
    void purgeVoicesAndLfoForPatchSwitch() noexcept;
    [[nodiscard]] std::size_t currentPerformanceIndex() const noexcept { return currentPerfIndex_; }
    [[nodiscard]] bool currentPerformanceFromUserBank() const noexcept { return currentPerfUserBank_; }
    [[nodiscard]] const vf50::generated::FactoryPerformance& activePerformance() const noexcept { return activePerformance_; }
    vf50::generated::FactoryPerformance& mutableActivePerformance() noexcept { return activePerformance_; }
    void setActivePerformanceDirect(const vf50::generated::FactoryPerformance& perf) noexcept;
    void setPerformanceChannelVoice(uint8_t midiChannel, bool presetBank, uint8_t voiceIndex) noexcept;

    // Internal RAM banks (100 Single Voices + 100 Performances, initialized from Factory ROM)
    void storeInternalVoice(std::size_t slotIndex, const vf50::generated::FactoryVoice& voice) noexcept;
    void storeInternalPerformance(std::size_t slotIndex, const vf50::generated::FactoryPerformance& perf) noexcept;
    [[nodiscard]] const vf50::generated::FactoryVoice& getVoicePatch(uint8_t bank, uint8_t number) const noexcept;
    [[nodiscard]] const vf50::generated::FactoryPerformance& getPerformancePatch(uint8_t bank, uint8_t number) const noexcept;

    // User Microtuning (Int Oct = table 11, Int Full = table 12)
    void setUserOctaveMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 12>& entries) noexcept;
    void setUserFullMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 128>& entries) noexcept;

    // Global Tuning, Global Utility Transpose, Quick Edit Macros, Dual LFO Override & Fine Part Panning
    void setMasterTuneCents(double cents) noexcept;
    [[nodiscard]] double masterTuneCents() const noexcept { return masterTuneCents_; }

    void setGlobalTransposeSemitones(int semitones) noexcept;
    [[nodiscard]] int globalTransposeSemitones() const noexcept { return globalTransposeSemitones_; }
    [[nodiscard]] double effectiveGlobalTuneCents() const noexcept {
        return masterTuneCents_ + static_cast<double>(globalTransposeSemitones_) * 100.0;
    }

    void setQuickEditOffsets(const QuickEditOffsets& offsets) noexcept;
    [[nodiscard]] const QuickEditOffsets& quickEditOffsets() const noexcept { return quickEdit_; }

    void setSingleReceiveChannel(uint8_t chZeroBasedOrOmni16) noexcept { singleReceiveChannel_ = chZeroBasedOrOmni16; }
    [[nodiscard]] uint8_t singleReceiveChannel() const noexcept { return singleReceiveChannel_; }

    void setOperatorEnableMask(uint8_t mask0To15) noexcept;
    [[nodiscard]] uint8_t operatorEnableMask() const noexcept { return opEnableMask_; }

    void setSingleEffectSend(bool enabled) noexcept;
    [[nodiscard]] bool singleEffectSend() const noexcept { return singleEffectSend_; }

    void setLfo2Override(const Lfo2OverrideConfig& cfg) noexcept;
    [[nodiscard]] const Lfo2OverrideConfig& lfo2Override() const noexcept { return lfo2Override_; }

    void setPartPan(std::size_t partIndex, int8_t panNeg64ToPos63) noexcept;
    [[nodiscard]] int8_t partPan(std::size_t partIndex) const noexcept;
    [[nodiscard]] const std::array<int8_t, kMaxPerformanceParts>& partPans() const noexcept { return partPans_; }

    // Performance Effect Bank, Utility Setup & MIDI Switch Controls
    void setPerfEffectBank(const vf50::state::PerfEffectBank& bank) noexcept;
    [[nodiscard]] const vf50::state::PerfEffectBank& perfEffectBank() const noexcept { return perfEffects_; }
    void setAuditionPerfEffect(uint8_t pef0To12) noexcept { auditionPerfEffect_ = (pef0To12 <= 12u) ? pef0To12 : 0u; }
    [[nodiscard]] uint8_t auditionPerfEffect() const noexcept { return auditionPerfEffect_; }
    void setAuditionMicroTuneMode(uint8_t mode0Off1Oct2Full) noexcept { auditionMicroTuneMode_ = (mode0Off1Oct2Full <= 2u) ? mode0Off1Oct2Full : 0u; }
    void setEgDampSpeed(uint8_t speed0To3) noexcept { egDampSpeed_ = (speed0To3 <= 3u) ? speed0To3 : 3u; }
    [[nodiscard]] uint8_t egDampSpeed() const noexcept { return egDampSpeed_; }
    void setVoiceDampOn(bool enabled) noexcept { voiceDampOn_ = enabled; }
    [[nodiscard]] bool isVoiceDampOn() const noexcept { return voiceDampOn_; }
    void setMidiSwitchModes(uint8_t ccMode, uint8_t atMode, uint8_t pbMode) noexcept {
        ccMode_ = ccMode;
        atMode_ = atMode;
        pbMode_ = pbMode;
    }
    void resetAllChannelControllers() noexcept;
    [[nodiscard]] const ChannelControllerState& channelControllerState(std::size_t ch) const noexcept {
        return channelCtrl_[ch & 0x0Fu];
    }
    [[nodiscard]] const OpzVoice& voiceAt(std::size_t idx) const noexcept {
        return voices_[std::min(idx, kTotalVoices - 1u)];
    }
    [[nodiscard]] uint8_t lastTriggeredVelocity() const noexcept { return lastTriggeredVelocity_; }

    // Thread-safe Classic Performance Instrument mute mask (bits 0..7 = INST 1..8) & hit telemetry
    void setPerfPartMuteMask(uint8_t mask) noexcept {
        perfPartMuteMask_.store(mask, std::memory_order_release);
    }
    [[nodiscard]] uint8_t perfPartMuteMask() const noexcept {
        return perfPartMuteMask_.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint8_t consumeActivePartHitMask() noexcept {
        return activePartHitMask_.exchange(0u, std::memory_order_acq_rel);
    }
    [[nodiscard]] uint8_t consumeMutedPartHitMask() noexcept {
        return mutedPartHitMask_.exchange(0u, std::memory_order_acq_rel);
    }

    // Live audio-thread parameter propagation to active voices
    void notifyLiveVoiceParametersChanged() noexcept;
    void notifyLivePerformanceParametersChanged() noexcept;

    // MIDI Event Handling (at 50 kHz sample resolution)
    void noteOn(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept;
    void noteOff(uint8_t midiChannel, uint8_t midiNote) noexcept;
    void controlChange(uint8_t midiChannel, uint8_t ccNumber, uint8_t ccValue) noexcept;
    void pitchBend(uint8_t midiChannel, int bendValue14Bit) noexcept; // 0..16383, 8192 = center
    void channelPressure(uint8_t midiChannel, uint8_t pressure) noexcept;
    void channelNotesOff(uint8_t midiChannel) noexcept;
    void allNotesOff() noexcept;
    void releaseAllVoicesSmooth() noexcept;
    void allSoundOff() noexcept;

    // Render 1 synchronous 50,000 Hz stereo frame across all 16 YM2424 voices
    [[nodiscard]] FmBusFrame renderFrame50kHz(uint16_t soloMask = 0u) noexcept;

    // Diagnostics / Verification Queries
    [[nodiscard]] std::size_t activeVoiceCount() const noexcept;
    [[nodiscard]] std::size_t keyDownVoiceCount() const noexcept;
    [[nodiscard]] vf50::generated::DspEffectSettings currentPresetEffectSettings() const noexcept;

private:
    struct ScheduledMidiEcho {
        bool     active          = false;
        uint32_t samplesRemain   = 0u;
        uint32_t intervalSamples = 9000u;
        uint8_t  midiChannel     = 0u;
        uint8_t  midiNote        = 60u;
        uint8_t  triggerNote     = 60u;
        uint8_t  velocity        = 64u;
        uint8_t  repeatsLeft     = 0u;
        uint8_t  effectLevel     = 90u;
        int8_t   pitchShift      = 0;
        int8_t   targetPart      = -1; // -1=any/single, 0=Inst 1 only
        uint64_t order           = 0u;
    };

    void triggerNoteInternal(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity,
                             bool fromEcho, uint8_t triggerMidiNoteOverride = 255u,
                             int8_t forcedPartIndex = -1) noexcept;
    void triggerForPart(int partIndex,
                        const vf50::generated::FactoryVoice& patch,
                        const vf50::generated::PerformanceInstrument& inst,
                        int8_t finePan,
                        uint8_t midiChannel,
                        uint8_t midiNote,
                        uint8_t velocity,
                        uint8_t assignMode,
                        uint8_t triggerMidiNoteOverride = 255u) noexcept;

    [[nodiscard]] std::size_t allocateVoiceForPart(int partIndex,
                                                   uint8_t midiChannel,
                                                   uint8_t midiNote,
                                                   const vf50::generated::FactoryVoice& patch,
                                                   const vf50::generated::PerformanceInstrument& inst,
                                                   uint8_t assignMode,
                                                   bool& outRetriggerSameSlot) noexcept;

    [[nodiscard]] bool isInstrumentEnabled(const vf50::generated::PerformanceInstrument& inst,
                                           uint8_t assignMode) const noexcept;
    [[nodiscard]] static uint8_t reservedCountForInstrument(const vf50::generated::PerformanceInstrument& inst) noexcept;
    [[nodiscard]] std::size_t countActiveVoicesForPart(int partIndex) const noexcept;
    [[nodiscard]] double computeNotePitchCents(uint8_t midiNote, bool microTuneEnabled,
                                               uint8_t tableId, uint8_t rootKey) const noexcept;
    void refreshLfoConfigs() noexcept;
    void tickScheduledEchoes() noexcept;

    const vf50::rom::VF50RomImage* romBank_ = &vf50::rom::getDefaultRomImage();

    SynthPlayMode playMode_ = SynthPlayMode::Single;
    std::size_t currentVoiceIndex_ = 0u;
    std::size_t currentPerfIndex_ = 0u;
    bool currentVoiceUserBank_ = false;
    bool currentPerfUserBank_ = false;
    uint8_t singleReceiveChannel_ = 16u; // Default Omni in Single Mode
    uint8_t opEnableMask_ = 0x0Fu;
    bool singleEffectSend_ = true;

    vf50::generated::FactoryVoice activeVoice_{};
    vf50::generated::FactoryPerformance activePerformance_{};

    std::array<vf50::generated::FactoryVoice, vf50::generated::kFactoryVoiceCount> internalVoices_{};
    std::array<vf50::generated::FactoryPerformance, vf50::generated::kFactoryPerformanceCount> internalPerformances_{};
    std::array<vf50::generated::MicrotuningEntry, 128> userOctaveExpanded_{};
    std::array<vf50::generated::MicrotuningEntry, 128> userFullMicrotuning_{};

    std::array<OpzVoice, kTotalVoices> voices_{};
    std::array<ChannelControllerState, 16> channelCtrl_{};
    std::array<double, kMaxPerformanceParts> lastPartPitchCents_{};
    std::array<int8_t, kMaxPerformanceParts> partPans_{};
    std::array<ScheduledMidiEcho, 32> echoQueue_{};

    OpzLfo lfo1_{};   // YM2424 #1 hardware LFO (Single voice / Perf Inst 1)
    OpzLfo lfo2_{};   // YM2424 #2 hardware LFO (Perf Inst 2 / LFO 2 Override)
    OpzLfo vibLfo_{}; // Dedicated ~6.2 Hz controller vibrato LFO
    Lfo2OverrideConfig lfo2Override_{};
    vf50::state::PerfEffectBank perfEffects_{ vf50::state::makeFactoryPerfEffectBank() };
    uint8_t auditionPerfEffect_ = 0u;
    uint8_t auditionMicroTuneMode_ = 0u;
    uint8_t egDampSpeed_ = 3u;
    bool    voiceDampOn_ = true;
    uint8_t ccMode_ = 1u; // 0=off, 1=norm, 2..17=G1..G16
    uint8_t atMode_ = 1u;
    uint8_t pbMode_ = 1u;
    uint8_t lastTriggeredVelocity_ = 0u;
    uint64_t echoOrderCounter_ = 1u;

    QuickEditOffsets quickEdit_{};
    double masterTuneCents_ = 0.0;
    int globalTransposeSemitones_ = 0;
    uint64_t allocationCounter_ = 1u;
    std::size_t alternateRoundRobin_ = 0u;

    std::atomic<uint8_t> perfPartMuteMask_{ 0u };
    std::atomic<uint8_t> activePartHitMask_{ 0u };
    std::atomic<uint8_t> mutedPartHitMask_{ 0u };
    uint8_t prevPerfPartMuteMask_ = 0u;
};

} // namespace vf50::dsp::fm
