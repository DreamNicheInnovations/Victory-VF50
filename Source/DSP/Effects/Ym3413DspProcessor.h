#pragma once

#include "ROM/VF50DataTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vf50::dsp::effects {

struct StereoFrame {
    float left  = 0.0f;
    float right = 0.0f;
};

class Ym3413DspProcessor {
public:
    Ym3413DspProcessor();

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void setSettings(const vf50::generated::DspEffectSettings& settings) noexcept;
    void loadPatchSettingsImmediate(const vf50::generated::DspEffectSettings& newSettings) noexcept;
    [[nodiscard]] const vf50::generated::DspEffectSettings& settings() const noexcept {
        return hasPendingTypeSwitch_ ? stagedSettings_ : settings_;
    }

    void setBypass(bool bypass) noexcept { bypass_ = bypass; }
    [[nodiscard]] bool isBypassed() const noexcept { return bypass_; }

    // Process pure wet input -> stereo wet output (before MIX A/B balance)
    [[nodiscard]] StereoFrame processWetStereo(float wetInL, float wetInR) noexcept;

    // Complete VF50 MIX A + MIX B + YM3413 Wet/Dry/StereoMix router
    [[nodiscard]] StereoFrame processMixBus(float dryOnlyL, float dryOnlyR,
                                            float efctDryL, float efctDryR,
                                            float wetSendL, float wetSendR) noexcept;

private:
    enum class TransitionState : uint8_t {
        Idle = 0,
        FadingOut,
        FadingIn
    };

    struct OnePoleLpf {
        float z1 = 0.0f;
        void clear() noexcept { z1 = 0.0f; }
        [[nodiscard]] float process(float x, float coeffA) noexcept {
            z1 += coeffA * (x - z1);
            if (!std::isfinite(z1) || (z1 > -1e-18f && z1 < 1e-18f)) {
                z1 = 0.0f;
            }
            return z1;
        }
    };

    struct AllpassSection {
        std::vector<float> buf;
        std::size_t writePos = 0u;
        std::size_t delayLen = 128u;
        float feedback = 0.5f;

        void init(std::size_t maxLen) {
            buf.assign(maxLen, 0.0f);
            writePos = 0u;
        }
        void clear() noexcept {
            std::fill(buf.begin(), buf.end(), 0.0f);
            writePos = 0u;
        }
        [[nodiscard]] float process(float input) noexcept {
            const std::size_t sz = buf.size();
            const std::size_t readPos = (writePos + sz - delayLen) % sz;
            const float delayed = buf[readPos];
            const float v = input + delayed * feedback;
            buf[writePos] = (!std::isfinite(v) || (v > -1e-18f && v < 1e-18f)) ? 0.0f : v;
            if (++writePos >= sz) {
                writePos = 0u;
            }
            const float out = delayed - v * feedback;
            return std::isfinite(out) ? out : 0.0f;
        }
    };

    struct DampedCombFilter {
        std::vector<float> buf;
        std::size_t writePos = 0u;
        std::size_t delayLen = 1024u;
        float feedback = 0.78f;
        float dampCoeff = 0.35f;
        float filterState = 0.0f;

        void init(std::size_t maxLen) {
            buf.assign(maxLen, 0.0f);
            writePos = 0u;
            filterState = 0.0f;
        }
        void clear() noexcept {
            std::fill(buf.begin(), buf.end(), 0.0f);
            writePos = 0u;
            filterState = 0.0f;
        }
        [[nodiscard]] float process(float input) noexcept {
            const std::size_t sz = buf.size();
            const std::size_t readPos = (writePos + sz - delayLen) % sz;
            const float out = buf[readPos];
            filterState += dampCoeff * (out - filterState);
            if (!std::isfinite(filterState) || (filterState > -1e-18f && filterState < 1e-18f)) {
                filterState = 0.0f;
            }
            const float next = input + filterState * feedback;
            buf[writePos] = (!std::isfinite(next) || (next > -1e-18f && next < 1e-18f)) ? 0.0f : next;
            if (++writePos >= sz) {
                writePos = 0u;
            }
            return std::isfinite(out) ? out : 0.0f;
        }
    };

    struct BiquadFilter {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;

        void clear() noexcept { z1 = 0.0f; z2 = 0.0f; }
        [[nodiscard]] float process(float x) noexcept {
            const float y = b0 * x + z1;
            if (!std::isfinite(y)) {
                clear();
                return 0.0f;
            }
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            if (!std::isfinite(z1) || (z1 > -1e-18f && z1 < 1e-18f)) z1 = 0.0f;
            if (!std::isfinite(z2) || (z2 > -1e-18f && z2 < 1e-18f)) z2 = 0.0f;
            return y;
        }
    };

    void clearDelayMemory() noexcept;
    void recalculateCoefficients() noexcept;
    [[nodiscard]] float lpfIndexToAlpha(uint8_t idx0To14) const noexcept;
    [[nodiscard]] float readDelayLinear(const std::vector<float>& line, std::size_t writeIdx, float delaySamples) const noexcept;
    [[nodiscard]] StereoFrame runReverbCore(float monoIn) noexcept;
    [[nodiscard]] StereoFrame runDelayCore(float inL, float inR, bool pingPong, bool dualLR) noexcept;
    [[nodiscard]] StereoFrame runEarlyRefOrGateCore(float monoIn, int mode0Er1Gate2Rev, bool withFeedback) noexcept;
    [[nodiscard]] float runDistortionSample(float x, int channel) noexcept;
    [[nodiscard]] StereoFrame runToneEq(float inL, float inR) noexcept;
    [[nodiscard]] StereoFrame runDoublerCore(float inL, float inR, bool wideStereo) noexcept;

    double sampleRate_ = 50000.0;
    vf50::generated::DspEffectSettings settings_{ 1u, 50u, 100u, 1u, 20u, 10u, 15u };
    vf50::generated::DspEffectSettings stagedSettings_{ 1u, 50u, 100u, 1u, 20u, 10u, 15u };
    bool bypass_ = false;
    bool buffersDirty_ = false;
    bool hasPendingTypeSwitch_ = false;
    TransitionState transitionState_ = TransitionState::Idle;
    float transitionGain_ = 1.0f;

    float targetBalance_ = 0.50f;
    float smoothBalance_ = 0.50f;
    float targetOutLevel_ = 1.00f;
    float smoothOutLevel_ = 1.00f;

    std::vector<float> preDelayBuf_;
    std::size_t preDelayWrite_ = 0u;
    std::size_t preDelaySamples_ = 1u;

    std::vector<float> delayLineL_;
    std::vector<float> delayLineR_;
    std::size_t delayWrite_ = 0u;
    float targetDelaySamplesL_ = 8000.0f;
    float targetDelaySamplesR_ = 8000.0f;
    float smoothDelaySamplesL_ = 8000.0f;
    float smoothDelaySamplesR_ = 8000.0f;
    float delayFeedback_ = 0.35f;

    std::vector<float> erBuffer_;
    std::size_t erWrite_ = 0u;
    std::array<std::size_t, 12> erTapDelays_{};
    std::array<float, 12> erTapGains_{};
    float erFeedback_ = 0.0f;
    float gateEnv_ = 0.0f;
    float gateGain_ = 0.0f;
    uint32_t gateHoldSamples_ = 0u;
    uint32_t gateHoldDuration_ = 6000u;

    std::array<AllpassSection, 4> reverbAllpass_{};
    std::array<DampedCombFilter, 4> combsL_{};
    std::array<DampedCombFilter, 4> combsR_{};
    OnePoleLpf reverbInLpf_{};
    OnePoleLpf delayLpfL_{};
    OnePoleLpf delayLpfR_{};
    OnePoleLpf erLpf_{};
    float inputLpfAlpha_ = 0.75f;
    float delayLpfAlpha_ = 0.65f;

    float distDrive_ = 4.0f;
    float distHpfPrevIn_[2] = {0.0f, 0.0f};
    float distHpfPrevOut_[2] = {0.0f, 0.0f};
    OnePoleLpf distToneLpf_[2]{};
    float distToneAlpha_ = 0.35f;
    float distOutTrim_ = 0.6f;

    std::array<BiquadFilter, 3> eqL_{};
    std::array<BiquadFilter, 3> eqR_{};

    double modPhase_ = 0.0;
    double modPhaseInc_ = 0.8 / 50000.0;
    float modDepthSamples_ = 25.0f;
};

} // namespace vf50::dsp::effects
