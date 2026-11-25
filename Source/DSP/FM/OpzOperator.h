#pragma once

#include "DSP/FM/OpzTables.h"
#include "ROM/VF50DataTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vf50::dsp::fm {

struct QuickEditOffsets {
    int attackOffset     = 0; // -99..+99
    int releaseOffset    = 0; // -99..+99
    int brillianceOffset = 0; // -99..+99 (modulators)
    int volumeOffset     = 0; // -99..+99 (carriers)
};

class OpzOperator {
public:
    enum class EgStage : uint8_t {
        Off = 0,
        Attack,
        Decay1,
        Decay2,
        Release
    };

    void reset() noexcept {
        phase32_ = 0u;
        targetPhaseInc32_ = 0u;
        smoothPhaseInc32_ = 0u;
        snapPhaseIncOnNextSet_ = true;
        history_[0] = 0;
        history_[1] = 0;
        env24_ = kMaxEnv24;
        d1lTarget24_ = 0;
        stage_ = EgStage::Off;
        staticAtten10_ = 1023u;
        targetStaticAtten16_ = static_cast<int32_t>(1023u << kAttenSmoothShift);
        smoothStaticAtten16_ = targetStaticAtten16_;
        arEff_ = 0u;
        d1rEff_ = 0u;
        d2rEff_ = 0u;
        rrEff_ = 30u;
        revEff_ = 30u;
        egShift_ = 0u;
        waveform_ = 0u;
        ampModEnable_ = false;
        egBiasSens_ = 0u;
        isCarrier_ = true;
        reverbRate_ = 0u;
    }

    void setLowOutTlOverride(const uint8_t* lowOutTl20) noexcept { lowOutTlOverride_ = lowOutTl20; }

    void noteOn(const vf50::generated::VoiceOperator& op,
                bool isCarrier,
                uint8_t voiceReverbRate,
                int midiNote,
                uint8_t velocity,
                const QuickEditOffsets& qe,
                bool resetPhase = true) noexcept {
        configureRatesAndStaticLevel(op, isCarrier, voiceReverbRate, midiNote, velocity, qe, true);
        snapPhaseIncOnNextSet_ = true;

        if (resetPhase) {
            phase32_ = 0u;
            history_[0] = 0;
            history_[1] = 0;
        }

        if (arEff_ >= 62u) {
            // Instantaneous 0.0 ms hardware attack snap
            env24_ = 0;
            stage_ = (d1lTarget24_ == 0) ? EgStage::Decay2 : EgStage::Decay1;
        } else {
            // Smooth retrigger from current envelope level (legato/steal friendly) or silence
            if (resetPhase || env24_ > kMaxEnv24) {
                env24_ = kMaxEnv24;
            }
            stage_ = EgStage::Attack;
        }
    }

    // Real-time parameter update for currently held/releasing voices (zipper-free)
    void updateLiveParameters(const vf50::generated::VoiceOperator& op,
                              bool isCarrier,
                              uint8_t voiceReverbRate,
                              int midiNote,
                              uint8_t velocity,
                              const QuickEditOffsets& qe) noexcept {
        configureRatesAndStaticLevel(op, isCarrier, voiceReverbRate, midiNote, velocity, qe, false);
    }

    void noteOff() noexcept {
        if (stage_ != EgStage::Off && stage_ != EgStage::Release) {
            stage_ = EgStage::Release;
        }
    }

    void setFrequencyHz(double freqHz) noexcept {
        if (freqHz <= 0.0) {
            targetPhaseInc32_ = 0u; // Exact 0.00 Hz stationary phase (OPZ II Fix Shift = LO, M = 0)
        } else {
            // Clamp below Nyquist (24,500 Hz at 50 kHz bus) to prevent ultrasonic wrap instability
            const double safeHz = std::min(freqHz, 24500.0);
            const double inc = (safeHz / kOpzNativeSampleRate) * 4294967296.0;
            targetPhaseInc32_ = static_cast<uint32_t>(std::clamp(inc, 0.0, 2147483647.0));
        }
        if (snapPhaseIncOnNextSet_) {
            smoothPhaseInc32_ = targetPhaseInc32_;
            snapPhaseIncOnNextSet_ = false;
        }
    }

    // Render 1 sample at 50 kHz with phase modulation `phaseMod10` (in 10-bit phase units)
    [[nodiscard]] int32_t tick(int32_t phaseMod10, int32_t lfoAmpAtten10, int32_t egBiasAtten10) noexcept {
        if (stage_ == EgStage::Off) {
            return 0;
        }

        const auto& tables = getOpzTables();
        advanceEnvelope(tables);

        // Slew static attenuation in Q6 fixed-point (~64-sample zipper-free glide)
        if (smoothStaticAtten16_ != targetStaticAtten16_) {
            const int32_t diff = targetStaticAtten16_ - smoothStaticAtten16_;
            int32_t step = diff >> kAttenSmoothShift;
            if (step == 0) {
                step = (diff > 0) ? 1 : -1;
            }
            smoothStaticAtten16_ += step;
            staticAtten10_ = static_cast<uint16_t>(std::clamp<int32_t>(
                (smoothStaticAtten16_ + (1 << (kAttenSmoothShift - 1))) >> kAttenSmoothShift, 0, 1023));
        }

        // Slew phase increment continuously across live frequency/ratio sweeps (~32-sample glide)
        if (smoothPhaseInc32_ != targetPhaseInc32_) {
            const int64_t diff = static_cast<int64_t>(targetPhaseInc32_) - static_cast<int64_t>(smoothPhaseInc32_);
            int64_t step = diff >> 5;
            if (step == 0) {
                step = (diff > 0) ? 1 : -1;
            }
            smoothPhaseInc32_ = static_cast<uint32_t>(static_cast<int64_t>(smoothPhaseInc32_) + step);
        }

        const uint16_t rawEg10 = static_cast<uint16_t>(std::clamp<int32_t>(env24_ >> kEnvFracBits, 0, 1023));
        const uint16_t shiftedEg10 = static_cast<uint16_t>(rawEg10 >> egShift_);

        int32_t totalAtten10 = static_cast<int32_t>(staticAtten10_) + static_cast<int32_t>(shiftedEg10);
        if (ampModEnable_) {
            totalAtten10 += lfoAmpAtten10;
        }
        if (egBiasSens_ > 0u) {
            totalAtten10 += (egBiasAtten10 * static_cast<int32_t>(egBiasSens_)) >> 3;
        }

        const uint16_t clampedAtten10 = static_cast<uint16_t>(std::clamp<int32_t>(totalAtten10, 0, 1023));
        const uint16_t phase10 = static_cast<uint16_t>(((phase32_ >> 22u) + static_cast<uint32_t>(phaseMod10)) & 0x3FFu);

        phase32_ += smoothPhaseInc32_;

        const int32_t out = tables.lookupWaveform(waveform_, phase10, clampedAtten10);
        // Store calibrated 10-bit feedback accumulator sample ([-511, +511] at TL=0 -> pi rad max at FB=7)
        history_[1] = history_[0];
        history_[0] = out >> 3;
        return out;
    }

    // Render self-feedback operator (OP4) with calibrated two-sample averaged feedback:
    //   fbMod10 = (history[0] + history[1]) >> (8 - FB)
    [[nodiscard]] int32_t tickWithFeedback(uint8_t feedback, int32_t lfoAmpAtten10, int32_t egBiasAtten10) noexcept {
        int32_t fbMod10 = 0;
        const uint8_t fb = feedback & 0x07u;
        if (fb > 0u) {
            fbMod10 = (history_[0] + history_[1]) >> (8u - fb);
        }
        return tick(fbMod10, lfoAmpAtten10, egBiasAtten10);
    }

    [[nodiscard]] bool isSilent() const noexcept {
        return stage_ == EgStage::Off || (stage_ == EgStage::Release && (env24_ >> kEnvFracBits) >= 1016);
    }

    [[nodiscard]] uint16_t currentEnvAtten10() const noexcept {
        return static_cast<uint16_t>(std::clamp<int32_t>(env24_ >> kEnvFracBits, 0, 1023));
    }

private:
    static constexpr int32_t kEnvFracBits      = 14;
    static constexpr int32_t kMaxEnv24         = 1023 << kEnvFracBits; // 16,760,832
    static constexpr int32_t kAttenSmoothShift = 6;                    // Q6 fixed-point (~64-sample slew)

    void configureRatesAndStaticLevel(const vf50::generated::VoiceOperator& op,
                                      bool isCarrier,
                                      uint8_t voiceReverbRate,
                                      int midiNote,
                                      uint8_t velocity,
                                      const QuickEditOffsets& qe,
                                      bool snapImmediate) noexcept {
        isCarrier_ = isCarrier;
        reverbRate_ = voiceReverbRate & 0x07u;
        waveform_ = op.waveform & 0x07u;
        egShift_ = op.egShift & 0x03u;
        ampModEnable_ = op.ampModEnable != 0u;
        egBiasSens_ = op.egBiasSens & 0x07u;

        // 1. Key Code & Rate Scaling (RS 0..3) via OPZ II base rate lookup tables (rates 0..31)
        const int kc5 = std::clamp((midiNote - 12) / 3, 0, 31);
        const int ksr = kc5 >> (3 - (op.rateScaling & 0x03u));

        const auto adjAr = static_cast<std::size_t>(std::clamp(static_cast<int>(op.ar) + (qe.attackOffset / 4), 0, 31));
        const auto d1rIdx = static_cast<std::size_t>(op.d1r & 0x1Fu);
        const auto d2rIdx = static_cast<std::size_t>(op.d2r & 0x1Fu);
        const auto adjRr = static_cast<std::size_t>(
            isCarrier ? std::clamp(static_cast<int>(op.rr) + (qe.releaseOffset / 6), 1, 15)
                      : std::clamp(static_cast<int>(op.rr), 0, 15));

        arEff_  = (adjAr == 0u) ? 0u : static_cast<uint8_t>(std::min<int>(63, kOpzRateBaseTable[adjAr] + ksr));
        d1rEff_ = (d1rIdx == 0u) ? 0u : static_cast<uint8_t>(std::min<int>(63, kOpzRateBaseTable[d1rIdx] + ksr));
        d2rEff_ = (d2rIdx == 0u) ? 0u : static_cast<uint8_t>(std::min<int>(63, kOpzRateBaseTable[d2rIdx] + ksr));
        rrEff_  = static_cast<uint8_t>(std::min<int>(63, kOpzReleaseRateBaseTable[adjRr] + ksr));
        revEff_ = (reverbRate_ > 0u)
                      ? static_cast<uint8_t>(std::min<int>(63, kOpzReleaseRateBaseTable[reverbRate_] + (ksr >> 1)))
                      : rrEff_;

        d1lTarget24_ = static_cast<int32_t>(d1lToAtten10(op.d1l)) << kEnvFracBits;

        // 2. Output Level (0..99 -> 7-bit TL 0..127 @ exact 0.75 dB/step) + Quick Edit Brilliance/Volume
        const int levelMacro = isCarrier ? (qe.volumeOffset / 2) : (qe.brillianceOffset / 2);
        const int effOutLevel = std::clamp(static_cast<int>(op.outputLevel) + levelMacro, 0, 99);
        int totalStaticAtten10 = static_cast<int>(outputLevelToAtten10(effOutLevel, lowOutTlOverride_));

        // Hardware Sub-CPU modulator trim for double-frequency waveforms (W5..W8) to prevent ultrasonic harshness
        if (!isCarrier && waveform_ >= 4u) {
            static constexpr std::array<int, 4> kDoubleFreqModAtten10 = { 24, 36, 36, 48 }; // 2.25 .. 4.5 dB
            totalStaticAtten10 += kDoubleFreqModAtten10[waveform_ - 4u];
        }

        // 3. Bipolar Key Velocity Sensitivity (KVS -7..+7)
        if (op.keyVelSens != 0) {
            const float velNorm = static_cast<float>(std::clamp<int>(velocity, 1, 127)) / 127.0f;
            const float curvedDeficit = (op.keyVelSens > 0)
                                            ? std::pow(1.0f - velNorm, 1.35f)
                                            : std::pow(velNorm, 1.35f);
            const int kvsMag = std::abs(static_cast<int>(op.keyVelSens));
            const int velAtten10 = static_cast<int>(std::lround(curvedDeficit * static_cast<float>(kvsMag * 56)));
            totalStaticAtten10 += velAtten10;
        }

        // 4. Bipolar Keyboard Level Scaling (LS -99..+99)
        if (op.levelScaling > 0) {
            const double octAbove = std::max(0.0, (static_cast<double>(midiNote) - 48.0) / 12.0);
            const double attenDb = octAbove * (static_cast<double>(op.levelScaling) / 99.0) * 9.0;
            totalStaticAtten10 += static_cast<int>(std::lround(attenDb * (64.0 / 6.0)));
        } else if (op.levelScaling < 0) {
            const double octBelow = std::max(0.0, (84.0 - static_cast<double>(midiNote)) / 12.0);
            const double attenDb = octBelow * (static_cast<double>(-op.levelScaling) / 99.0) * 12.0;
            totalStaticAtten10 += static_cast<int>(std::lround(attenDb * (64.0 / 6.0)));
        }

        const uint16_t target10 = static_cast<uint16_t>(std::clamp(totalStaticAtten10, 0, 1023));
        targetStaticAtten16_ = static_cast<int32_t>(target10) << kAttenSmoothShift;
        if (snapImmediate) {
            staticAtten10_ = target10;
            smoothStaticAtten16_ = targetStaticAtten16_;
        }
    }

    void advanceEnvelope(const OpzHardwareTables& tables) noexcept {
        switch (stage_) {
            case EgStage::Off:
                env24_ = kMaxEnv24;
                break;

            case EgStage::Attack: {
                if (arEff_ == 0u) {
                    break;
                }
                if (arEff_ >= 62u) {
                    env24_ = 0;
                    stage_ = (d1lTarget24_ == 0) ? EgStage::Decay2 : EgStage::Decay1;
                    break;
                }
                // Hardware OPZ II exponential attack curve using lookup table egAttackStep24
                const int32_t baseInc = tables.egAttackStep24[arEff_ & 0x3Fu];
                const int32_t env10 = env24_ >> kEnvFracBits;
                const int32_t accel = ((env10 + 96) * baseInc) >> 6;
                env24_ -= std::max<int32_t>(1, accel);
                if (env24_ <= 0) {
                    env24_ = 0;
                    stage_ = (d1lTarget24_ == 0) ? EgStage::Decay2 : EgStage::Decay1;
                }
                break;
            }

            case EgStage::Decay1: {
                if (env24_ >= d1lTarget24_) {
                    env24_ = d1lTarget24_;
                    stage_ = EgStage::Decay2;
                    break;
                }
                const int32_t step = tables.egDecayStep24[d1rEff_ & 0x3Fu];
                env24_ = std::min(d1lTarget24_, env24_ + step);
                if (env24_ >= d1lTarget24_) {
                    stage_ = EgStage::Decay2;
                }
                break;
            }

            case EgStage::Decay2: {
                if (d2rEff_ == 0u) {
                    // Infinite sustain hold at D1L while key is held
                    break;
                }
                env24_ = std::min(kMaxEnv24, env24_ + tables.egDecayStep24[d2rEff_ & 0x3Fu]);
                break;
            }

            case EgStage::Release: {
                uint8_t activeRr = rrEff_;
                // Simulated FM Reverb (REV 1..7): gentler acoustic tail below -18 dB (192 in 10-bit units)
                if (isCarrier_ && reverbRate_ > 0u && (env24_ >> kEnvFracBits) >= 192) {
                    activeRr = std::min(activeRr, revEff_);
                }
                env24_ += std::max<int32_t>(8, tables.egDecayStep24[activeRr & 0x3Fu]);
                if (env24_ >= kMaxEnv24) {
                    env24_ = kMaxEnv24;
                    stage_ = EgStage::Off;
                }
                break;
            }
        }
    }

    uint32_t phase32_ = 0u;
    uint32_t targetPhaseInc32_ = 0u;
    uint32_t smoothPhaseInc32_ = 0u;
    int32_t history_[2] = {0, 0};
    int32_t env24_ = kMaxEnv24;
    int32_t d1lTarget24_ = 0;
    int32_t targetStaticAtten16_ = static_cast<int32_t>(1023u << kAttenSmoothShift);
    int32_t smoothStaticAtten16_ = static_cast<int32_t>(1023u << kAttenSmoothShift);

    uint16_t staticAtten10_ = 1023u;
    uint8_t arEff_ = 0u;
    uint8_t d1rEff_ = 0u;
    uint8_t d2rEff_ = 0u;
    uint8_t rrEff_ = 30u;
    uint8_t revEff_ = 30u;
    uint8_t reverbRate_ = 0u;
    uint8_t egShift_ = 0u;
    uint8_t waveform_ = 0u;
    uint8_t egBiasSens_ = 0u;
    bool snapPhaseIncOnNextSet_ = true;
    bool ampModEnable_ = false;
    bool isCarrier_ = true;
    EgStage stage_ = EgStage::Off;
    const uint8_t* lowOutTlOverride_ = nullptr;
};

} // namespace vf50::dsp::fm
