#pragma once

#include "DSP/FM/OpzLfo.h"
#include "DSP/FM/OpzOperator.h"
#include "DSP/FM/OpzTables.h"
#include "ROM/VF50DataTypes.h"

#include <array>
#include <cstdint>

namespace vf50::dsp::fm {

struct ChannelControllerState {
    float pitchBendNorm  = 0.0f; // [-1.0, +1.0]
    float modWheel       = 0.0f; // [0.0, 1.0] (CC#1)
    float breathCtrl     = 0.0f; // [0.0, 1.0] (CC#2)
    float footCtrl       = 1.0f; // [0.0, 1.0] (CC#4, defaults to 1.0 when unplugged)
    float expression     = 1.0f; // [0.0, 1.0] (CC#11)
    float channelVolume  = 1.0f; // [0.0, 1.0] (CC#7)
    float aftertouch     = 0.0f; // [0.0, 1.0] (Channel Pressure)
    float panNorm        = 0.5f; // [0.0=L, 0.5=C, 1.0=R] (CC#10)
    bool  sustainPedal   = false;// CC#64
};

struct VoiceRenderSample {
    float left         = 0.0f;
    float right        = 0.0f;
    bool  effectEnable = true;
};

struct VoiceNoteConfig {
    int     partIndex        = 0;     // 0..7 in Performance mode, 0 in Single mode
    uint8_t midiChannel      = 0;     // 0..15
    uint8_t midiNote         = 60;    // 0..127 sounding MIDI note
    uint8_t triggerMidiNote  = 255;   // 0..127 originating key (255 = same as midiNote)
    uint8_t velocity         = 100;   // 1..127
    int8_t  partNoteShift    = 0;     // -24..+24 semitones
    int8_t  partDetune       = 0;     // -7..+7 fine detune steps
    int8_t  partPan          = 0;     // -64..+63 fine stereo pan
    uint8_t partVolume       = 99;    // 0..99
    uint8_t outAssign        = 3;     // 0=off, 1=L, 2=R, 3=L+R
    uint8_t lfoSelect        = 1;     // 0=off, 1=LFO1, 2=LFO2, 3=Vibrato
    bool    effectEnable     = true;  // MIX A wet send enable
    double  basePitchCents   = 6000.0;// Microtuned or equal-tempered base pitch in cents (6000 = C4)
    double  prevGlideCents   = -1.0;  // >= 0 when portamento glides from previous note
    double  masterTuneCents  = 0.0;   // Master tuning offset (-75..+75 cents)
};

class OpzVoice {
public:
    void setRomLutOverrides(const uint8_t* coarsePerm64, const uint8_t* lowOutTl20) noexcept {
        coarsePermOverride_ = coarsePerm64;
        for (auto& op : ops_) {
            op.setLowOutTlOverride(lowOutTl20);
        }
    }

    void reset() noexcept;

    void startNote(const vf50::generated::FactoryVoice& patch,
                   const VoiceNoteConfig& cfg,
                   const QuickEditOffsets& qe,
                   uint64_t allocationOrder,
                   bool retriggerSameSlot = false) noexcept;

    // Live audio-thread parameter updates for held/releasing voices (zero zipper, C0 de-click)
    void updateLivePatchParameters(const vf50::generated::FactoryVoice& newPatch,
                                   const QuickEditOffsets& qe,
                                   double masterTuneCents,
                                   uint8_t newOpMask,
                                   bool effectSendEnable) noexcept;

    void updateLivePartParameters(const vf50::generated::PerformanceInstrument& inst,
                                  int8_t partPan,
                                  double basePitchCents,
                                  double masterTuneCents) noexcept;

    void releaseKey(bool sustainPedalDown) noexcept;
    void setSustainPedal(bool sustainPedalDown) noexcept;
    void forceReleaseQuick() noexcept;
    void forceReleaseWithDampSpeed(uint8_t speed0To3) noexcept;

    [[nodiscard]] VoiceRenderSample renderSample(const LfoOutput& lfo1,
                                                 const LfoOutput& lfo2,
                                                 const LfoOutput& vibLfo,
                                                 const ChannelControllerState& ctrl) noexcept;

    [[nodiscard]] bool isActive() const noexcept { return active_; }
    [[nodiscard]] bool isKeyDown() const noexcept { return keyDown_; }
    [[nodiscard]] bool isSustained() const noexcept { return sustainedByPedal_; }
    [[nodiscard]] bool isQuickReleasing() const noexcept { return quickFadeStep_ > 0.0f; }
    [[nodiscard]] int partIndex() const noexcept { return cfg_.partIndex; }
    [[nodiscard]] uint8_t midiChannel() const noexcept { return cfg_.midiChannel; }
    [[nodiscard]] uint8_t midiNote() const noexcept { return cfg_.midiNote; }
    [[nodiscard]] uint8_t triggerMidiNote() const noexcept {
        return (cfg_.triggerMidiNote <= 127u) ? cfg_.triggerMidiNote : cfg_.midiNote;
    }
    [[nodiscard]] uint8_t lastVelocity() const noexcept { return cfg_.velocity; }
    [[nodiscard]] float dynamicPanOffset() const noexcept { return dynamicPanOffset_; }
    [[nodiscard]] uint64_t allocationOrder() const noexcept { return allocationOrder_; }
    [[nodiscard]] double currentPitchCents() const noexcept { return currentGlideCents_; }
    [[nodiscard]] uint16_t carrierEnvelopeAtten10() const noexcept;

    void setOperatorMask(uint8_t mask) noexcept { opMask_ = mask & 0x0Fu; }
    void setDynamicPanOffset(float panOffsetBipolar) noexcept { dynamicPanOffset_ = panOffsetBipolar; }
    void setEgDampSpeed(uint8_t speed0To3) noexcept { egDampSpeed_ = (speed0To3 <= 3u) ? speed0To3 : 3u; }

private:
    enum class PegStage : uint8_t {
        Stage1 = 0,
        Stage2,
        Hold,
        Release
    };

    [[nodiscard]] static constexpr bool isOperatorCarrier(uint8_t algorithm, std::size_t opIdx) noexcept {
        switch (algorithm & 0x07u) {
            case 0u:
            case 1u:
            case 2u:
            case 3u:
                return opIdx == 0u; // OP1
            case 4u:
                return opIdx == 0u || opIdx == 2u; // OP1, OP3
            case 5u:
            case 6u:
                return opIdx == 0u || opIdx == 1u || opIdx == 2u; // OP1, OP2, OP3
            case 7u:
            default:
                return true; // OP1, OP2, OP3, OP4
        }
    }

    void configureBaseGainAndPan(bool snapImmediate) noexcept;
    void initPitchEnvelope(const vf50::generated::PitchEnvelope& peg) noexcept;
    void updatePitchEnvelopeTargets(const vf50::generated::PitchEnvelope& peg) noexcept;
    void advancePitchEnvelope() noexcept;
    void updateOperatorFrequencies(const LfoOutput& activeLfo, const ChannelControllerState& ctrl) noexcept;

    vf50::generated::FactoryVoice patch_{};
    VoiceNoteConfig cfg_{};
    std::array<OpzOperator, 4> ops_{};
    VoiceLfoDelayRamp lfoDelayRamp_{};

    uint64_t allocationOrder_ = 0u;
    uint32_t ctrlSubCounter_ = 0u;
    uint8_t opMask_ = 0x0Fu;
    const uint8_t* coarsePermOverride_ = nullptr;

    // Pitch EG state (in semitones)
    PegStage pegStage_ = PegStage::Hold;
    double pegCurrentSemi_ = 0.0;
    double pegTarget1Semi_ = 0.0;
    double pegTarget2Semi_ = 0.0;
    double pegTarget3Semi_ = 0.0;
    double pegRate1Inc_ = 0.0;
    double pegRate2Inc_ = 0.0;
    double pegRate3Inc_ = 0.0;

    // Portamento state (in cents)
    double targetPitchCents_ = 6000.0;
    double currentGlideCents_ = 6000.0;
    double glideStepCents_ = 0.0;

    // Cached LFO/controller modulation & smoothed voice mix
    int32_t currentLfoAmpAtten10_ = 0;
    int32_t currentEgBiasAtten10_ = 0;
    float targetGainScale_ = 1.0f;
    float smoothGainScale_ = 1.0f;
    float targetPanLeft_ = 0.70710678f;
    float targetPanRight_ = 0.70710678f;
    float panLeft_ = 0.70710678f;
    float panRight_ = 0.70710678f;
    float dynamicPanOffset_ = 0.0f;
    float quickFadeGain_ = 1.0f;
    float quickFadeStep_ = 0.0f;

    // C0 continuity de-click ramp for mid-note discrete algorithm/waveform/opMask transitions
    float lastMonoOut_ = 0.0f;
    float declickOffset_ = 0.0f;
    float declickStep_ = 0.0f;
    uint32_t declickSamplesLeft_ = 0u;
    bool pendingDeclickCapture_ = false;
    uint8_t egDampSpeed_ = 3u;

    bool active_ = false;
    bool keyDown_ = false;
    bool sustainedByPedal_ = false;
};

} // namespace vf50::dsp::fm
