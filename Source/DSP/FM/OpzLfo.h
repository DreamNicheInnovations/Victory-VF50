#pragma once

#include "DSP/FM/OpzTables.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vf50::dsp::fm {

struct LfoOutput {
    float pitchBipolar = 0.0f; // [-1.0, +1.0] scaled by PMD curve
    float ampUnipolar  = 0.0f; // [0.0, +1.0] scaled by AMD
    float rawBipolar   = 0.0f; // [-1.0, +1.0] unscaled waveform for controllers
};

class OpzLfo {
public:
    void reset(uint32_t rngSeed = 0x5635304Cu) noexcept {
        phase_ = 0.0;
        shValue_ = 0.0f;
        rngState_ = (rngSeed != 0u) ? rngSeed : 0x5635304Cu;
    }

    void configure(uint8_t speed, uint8_t waveform, uint8_t pmd, uint8_t amd, uint8_t sync) noexcept {
        speed_ = static_cast<uint8_t>(std::min<uint8_t>(speed, 99u));
        waveform_ = waveform & 0x03u;
        pmd_ = static_cast<uint8_t>(std::min<uint8_t>(pmd, 99u));
        amd_ = static_cast<uint8_t>(std::min<uint8_t>(amd, 99u));
        sync_ = sync != 0u;

        const double hz = getOpzTables().lfoHz[speed_];
        phaseInc_ = hz / kOpzNativeSampleRate;
    }

    void triggerNoteOn() noexcept {
        if (sync_) {
            phase_ = 0.0;
            if (waveform_ == 3u) {
                shValue_ = nextRandomBipolar();
            }
        }
    }

    [[nodiscard]] LfoOutput tick() noexcept {
        phase_ += phaseInc_;
        if (phase_ >= 1.0) {
            phase_ -= 1.0;
            if (waveform_ == 3u) {
                shValue_ = nextRandomBipolar();
            }
        }

        float rawBipolar = 0.0f;
        switch (waveform_) {
            case 0u: // Saw Up: ramps -1 -> +1
                rawBipolar = static_cast<float>(2.0 * phase_ - 1.0);
                break;
            case 1u: // Square: +1 for first half, -1 for second half
                rawBipolar = (phase_ < 0.5) ? 1.0f : -1.0f;
                break;
            case 2u: { // Triangle: 0 -> +1 -> 0 -> -1 -> 0
                if (phase_ < 0.25) {
                    rawBipolar = static_cast<float>(phase_ * 4.0);
                } else if (phase_ < 0.75) {
                    rawBipolar = static_cast<float>(2.0 - phase_ * 4.0);
                } else {
                    rawBipolar = static_cast<float>(phase_ * 4.0 - 4.0);
                }
                break;
            }
            case 3u: // Sample & Hold
                rawBipolar = shValue_;
                break;
        }

        // Exponential PMD taper: hardware PMD 0..25 adds subtle shimmer without excessive warble,
        // while PMD 70..99 delivers full pitch excursion for sweeps and SFX.
        const float pmdNorm = static_cast<float>(pmd_) / 99.0f;
        const float pmdScale = std::pow(pmdNorm, 2.2f);
        const float amdScale = static_cast<float>(amd_) / 99.0f;
        const float rawUnipolar = 0.5f * (1.0f - rawBipolar); // 0 at peak, 1 at trough (attenuates downward)

        return LfoOutput{
            .pitchBipolar = rawBipolar * pmdScale,
            .ampUnipolar  = rawUnipolar * amdScale,
            .rawBipolar   = rawBipolar
        };
    }

    [[nodiscard]] float rawBipolar() const noexcept {
        if (waveform_ == 3u) {
            return shValue_;
        }
        if (waveform_ == 0u) {
            return static_cast<float>(2.0 * phase_ - 1.0);
        }
        if (waveform_ == 1u) {
            return (phase_ < 0.5) ? 1.0f : -1.0f;
        }
        if (phase_ < 0.25) {
            return static_cast<float>(phase_ * 4.0);
        }
        if (phase_ < 0.75) {
            return static_cast<float>(2.0 - phase_ * 4.0);
        }
        return static_cast<float>(phase_ * 4.0 - 4.0);
    }

private:
    [[nodiscard]] float nextRandomBipolar() noexcept {
        uint32_t x = rngState_;
        x ^= x << 13u;
        x ^= x >> 17u;
        x ^= x << 5u;
        rngState_ = x;
        const float norm = static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f;
        return std::clamp(norm, -1.0f, 1.0f);
    }

    double phase_ = 0.0;
    double phaseInc_ = 0.0001;
    float shValue_ = 0.0f;
    uint32_t rngState_ = 0x5635304Cu;
    uint8_t speed_ = 33u;
    uint8_t waveform_ = 2u;
    uint8_t pmd_ = 0u;
    uint8_t amd_ = 0u;
    bool sync_ = false;
};

// Per-voice LFO delay ramp (LFD 0..99)
class VoiceLfoDelayRamp {
public:
    void reset() noexcept {
        level_ = 1.0f;
        rampInc_ = 1.0f;
        holdSamples_ = 0u;
    }

    void trigger(uint8_t delayParam) noexcept {
        if (delayParam == 0u) {
            level_ = 1.0f;
            holdSamples_ = 0u;
            rampInc_ = 1.0f;
            return;
        }
        level_ = 0.0f;
        const double norm = static_cast<double>(std::min<uint8_t>(delayParam, 99u)) / 99.0;
        // Delay hold time: 20 ms up to ~3.6 seconds across 1..99 (audible hold before fade-in)
        const double delaySec = 0.020 + 1.15 * norm + 2.45 * (norm * norm);
        holdSamples_ = static_cast<uint32_t>(delaySec * kOpzNativeSampleRate * 0.65);
        const double rampSamples = std::max(64.0, delaySec * kOpzNativeSampleRate * 0.35);
        rampInc_ = static_cast<float>(1.0 / rampSamples);
    }

    [[nodiscard]] float tick() noexcept {
        if (level_ >= 1.0f) {
            return 1.0f;
        }
        if (holdSamples_ > 0u) {
            --holdSamples_;
            return 0.0f;
        }
        level_ = std::min(1.0f, level_ + rampInc_);
        return level_;
    }

private:
    float level_ = 1.0f;
    float rampInc_ = 1.0f;
    uint32_t holdSamples_ = 0u;
};

} // namespace vf50::dsp::fm
