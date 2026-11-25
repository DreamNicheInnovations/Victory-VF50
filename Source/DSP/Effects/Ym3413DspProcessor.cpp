#include "DSP/Effects/Ym3413DspProcessor.h"

#include <algorithm>
#include <cmath>

namespace vf50::dsp::effects {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr float  kPiF   = 3.14159265358979323846f;

void configureLowShelf(float sampleRate, float freqHz, float gainDb,
                       float& b0, float& b1, float& b2, float& a1, float& a2) noexcept {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPiF * std::clamp(freqHz, 20.0f, sampleRate * 0.45f) / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw * 0.5f * std::sqrt(2.0f);
    const float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;

    const float a0 = (A + 1.0f) + (A - 1.0f) * cosw + twoSqrtAAlpha;
    const float invA0 = 1.0f / a0;
    b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosw + twoSqrtAAlpha)) * invA0;
    b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw)) * invA0;
    b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosw - twoSqrtAAlpha)) * invA0;
    a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosw)) * invA0;
    a2 = ((A + 1.0f) + (A - 1.0f) * cosw - twoSqrtAAlpha) * invA0;
}

void configureHighShelf(float sampleRate, float freqHz, float gainDb,
                        float& b0, float& b1, float& b2, float& a1, float& a2) noexcept {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPiF * std::clamp(freqHz, 100.0f, sampleRate * 0.45f) / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw * 0.5f * std::sqrt(2.0f);
    const float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;

    const float a0 = (A + 1.0f) - (A - 1.0f) * cosw + twoSqrtAAlpha;
    const float invA0 = 1.0f / a0;
    b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosw + twoSqrtAAlpha)) * invA0;
    b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw)) * invA0;
    b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosw - twoSqrtAAlpha)) * invA0;
    a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosw)) * invA0;
    a2 = ((A + 1.0f) - (A - 1.0f) * cosw - twoSqrtAAlpha) * invA0;
}

void configurePeakingEq(float sampleRate, float freqHz, float q, float gainDb,
                        float& b0, float& b1, float& b2, float& a1, float& a2) noexcept {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPiF * std::clamp(freqHz, 60.0f, sampleRate * 0.45f) / sampleRate;
    const float cosw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * std::max(0.25f, q));

    const float a0 = 1.0f + alpha / A;
    const float invA0 = 1.0f / a0;
    b0 = (1.0f + alpha * A) * invA0;
    b1 = (-2.0f * cosw) * invA0;
    b2 = (1.0f - alpha * A) * invA0;
    a1 = (-2.0f * cosw) * invA0;
    a2 = (1.0f - alpha / A) * invA0;
}

} // namespace

Ym3413DspProcessor::Ym3413DspProcessor() {
    prepare(50000.0);
}

void Ym3413DspProcessor::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(16000.0, sampleRate);

    const std::size_t maxDelaySamples = static_cast<std::size_t>(sampleRate_ * 0.75) + 64u;
    const std::size_t maxPreDelay     = static_cast<std::size_t>(sampleRate_ * 0.20) + 16u;
    const std::size_t maxCombSamples  = static_cast<std::size_t>(sampleRate_ * 0.12) + 32u;
    const std::size_t maxApSamples    = static_cast<std::size_t>(sampleRate_ * 0.04) + 32u;

    preDelayBuf_.assign(maxPreDelay, 0.0f);
    delayLineL_.assign(maxDelaySamples, 0.0f);
    delayLineR_.assign(maxDelaySamples, 0.0f);
    erBuffer_.assign(maxDelaySamples, 0.0f);

    for (auto& ap : reverbAllpass_) {
        ap.init(maxApSamples);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        combsL_[i].init(maxCombSamples);
        combsR_[i].init(maxCombSamples);
    }

    reset();
    recalculateCoefficients();
    smoothBalance_ = targetBalance_;
    smoothOutLevel_ = targetOutLevel_;
    smoothDelaySamplesL_ = targetDelaySamplesL_;
    smoothDelaySamplesR_ = targetDelaySamplesR_;
}

void Ym3413DspProcessor::clearDelayMemory() noexcept {
    std::fill(preDelayBuf_.begin(), preDelayBuf_.end(), 0.0f);
    std::fill(delayLineL_.begin(), delayLineL_.end(), 0.0f);
    std::fill(delayLineR_.begin(), delayLineR_.end(), 0.0f);
    std::fill(erBuffer_.begin(), erBuffer_.end(), 0.0f);

    preDelayWrite_ = 0u;
    delayWrite_ = 0u;
    erWrite_ = 0u;

    for (auto& ap : reverbAllpass_) ap.clear();
    for (auto& c : combsL_) c.clear();
    for (auto& c : combsR_) c.clear();

    reverbInLpf_.clear();
    delayLpfL_.clear();
    delayLpfR_.clear();
    erLpf_.clear();

    for (int ch = 0; ch < 2; ++ch) {
        distHpfPrevIn_[ch] = 0.0f;
        distHpfPrevOut_[ch] = 0.0f;
        distToneLpf_[ch].clear();
    }
    for (std::size_t b = 0; b < 3; ++b) {
        eqL_[b].clear();
        eqR_[b].clear();
    }

    gateEnv_ = 0.0f;
    gateGain_ = 0.0f;
    gateHoldSamples_ = 0u;
    modPhase_ = 0.0;
    buffersDirty_ = false;
}

void Ym3413DspProcessor::reset() noexcept {
    if (hasPendingTypeSwitch_) {
        settings_ = stagedSettings_;
        hasPendingTypeSwitch_ = false;
        recalculateCoefficients();
    }
    clearDelayMemory();
    transitionState_ = TransitionState::Idle;
    transitionGain_ = 1.0f;
    smoothBalance_ = targetBalance_;
    smoothOutLevel_ = targetOutLevel_;
    smoothDelaySamplesL_ = targetDelaySamplesL_;
    smoothDelaySamplesR_ = targetDelaySamplesR_;
}

void Ym3413DspProcessor::setSettings(const vf50::generated::DspEffectSettings& newSettings) noexcept {
    stagedSettings_ = newSettings;

    const bool typeChanged = (settings_.effectType != newSettings.effectType);
    if (typeChanged) {
        if (!buffersDirty_) {
            settings_ = newSettings;
            hasPendingTypeSwitch_ = false;
            clearDelayMemory();
            recalculateCoefficients();
            smoothBalance_ = targetBalance_;
            smoothOutLevel_ = targetOutLevel_;
            smoothDelaySamplesL_ = targetDelaySamplesL_;
            smoothDelaySamplesR_ = targetDelaySamplesR_;
            transitionState_ = TransitionState::Idle;
            transitionGain_ = 1.0f;
        } else {
            hasPendingTypeSwitch_ = true;
            transitionState_ = TransitionState::FadingOut;
        }
        return;
    }

    hasPendingTypeSwitch_ = false;
    settings_ = newSettings;
    recalculateCoefficients();
    if (!buffersDirty_) {
        smoothBalance_ = targetBalance_;
        smoothOutLevel_ = targetOutLevel_;
    }
}

void Ym3413DspProcessor::loadPatchSettingsImmediate(const vf50::generated::DspEffectSettings& newSettings) noexcept {
    settings_ = newSettings;
    stagedSettings_ = newSettings;
    hasPendingTypeSwitch_ = false;
    clearDelayMemory();
    recalculateCoefficients();
    transitionState_ = TransitionState::Idle;
    transitionGain_ = 1.0f;
    smoothBalance_ = targetBalance_;
    smoothOutLevel_ = targetOutLevel_;
    smoothDelaySamplesL_ = targetDelaySamplesL_;
    smoothDelaySamplesR_ = targetDelaySamplesR_;
}

float Ym3413DspProcessor::lpfIndexToAlpha(uint8_t idx0To14) const noexcept {
    static constexpr std::array<float, 15> kCutoffHz = {
        1000.0f, 1250.0f, 1600.0f, 2000.0f, 2500.0f,
        3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f,
        10000.0f, 12500.0f, 15000.0f, 18000.0f, 22000.0f
    };
    const uint8_t clamped = std::min<uint8_t>(idx0To14, 14u);
    if (clamped >= 14u) {
        return 1.0f; // Thru
    }
    const float fc = std::min(kCutoffHz[clamped], static_cast<float>(sampleRate_ * 0.45));
    const float a = 1.0f - std::exp(-2.0f * kPiF * fc / static_cast<float>(sampleRate_));
    return std::clamp(a, 0.02f, 1.0f);
}

void Ym3413DspProcessor::recalculateCoefficients() noexcept {
    targetBalance_  = static_cast<float>(std::min<uint8_t>(settings_.balance, 100u)) * 0.01f;
    targetOutLevel_ = static_cast<float>(std::min<uint8_t>(settings_.outLevel, 100u)) * 0.01f;

    const uint8_t type = settings_.effectType;
    const float sr = static_cast<float>(sampleRate_);
    const float srScale = sr / 50000.0f;

    const uint8_t p1 = std::min<uint8_t>(settings_.param1, 99u);
    const uint8_t p2 = std::min<uint8_t>(settings_.param2, 99u);
    const uint8_t p3 = std::min<uint8_t>(settings_.param3, 99u);

    std::array<float, 4> baseCombsL = {1483.0f, 1657.0f, 1879.0f, 2137.0f};
    std::array<float, 4> baseCombsR = {1531.0f, 1709.0f, 1933.0f, 2203.0f};
    std::array<float, 4> baseAp     = {223.0f,  349.0f,  491.0f,  643.0f};
    float roomScale = 1.0f;
    float dampCoeff = 0.35f;
    float apFeedback = 0.52f;

    switch (type) {
        case 2u: // Reverb Room
            roomScale = 0.72f;
            dampCoeff = 0.42f;
            break;
        case 3u: // Reverb Plate
            roomScale = 0.86f;
            dampCoeff = 0.62f;
            apFeedback = 0.62f;
            break;
        case 16u: // Church
            roomScale = 1.32f;
            dampCoeff = 0.28f;
            apFeedback = 0.58f;
            break;
        case 17u: // Club
            roomScale = 0.64f;
            dampCoeff = 0.48f;
            break;
        case 18u: // Stage
            roomScale = 1.08f;
            dampCoeff = 0.44f;
            break;
        case 19u: // Bath Room
            roomScale = 0.42f;
            dampCoeff = 0.72f;
            apFeedback = 0.65f;
            break;
        case 20u: // Metal
            roomScale = 0.55f;
            dampCoeff = 0.88f;
            apFeedback = 0.70f;
            break;
        default: // Reverb Hall & multi-effects
            roomScale = 1.00f;
            dampCoeff = 0.36f;
            break;
    }

    float rt60Sec = 2.2f;
    if (type == 13u || type == 14u || type == 21u) {
        rt60Sec = 0.3f + (static_cast<float>(std::min<uint8_t>(p3, 75u)) / 75.0f) * 7.5f;
    } else {
        rt60Sec = 0.3f + (static_cast<float>(std::min<uint8_t>(p1, 75u)) / 75.0f) * 8.5f;
    }

    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t lenAp = std::clamp<std::size_t>(
            static_cast<std::size_t>(baseAp[i] * roomScale * srScale),
            8u, reverbAllpass_[i].buf.size() - 4u);
        reverbAllpass_[i].delayLen = lenAp;
        reverbAllpass_[i].feedback = apFeedback;

        const std::size_t lenL = std::clamp<std::size_t>(
            static_cast<std::size_t>(baseCombsL[i] * roomScale * srScale),
            32u, combsL_[i].buf.size() - 4u);
        const std::size_t lenR = std::clamp<std::size_t>(
            static_cast<std::size_t>(baseCombsR[i] * roomScale * srScale),
            32u, combsR_[i].buf.size() - 4u);

        combsL_[i].delayLen = lenL;
        combsR_[i].delayLen = lenR;
        combsL_[i].dampCoeff = dampCoeff;
        combsR_[i].dampCoeff = dampCoeff;

        const float tauL = static_cast<float>(lenL) / sr;
        const float tauR = static_cast<float>(lenR) / sr;
        combsL_[i].feedback = std::clamp(std::pow(10.0f, -3.0f * tauL / rt60Sec), 0.10f, 0.982f);
        combsR_[i].feedback = std::clamp(std::pow(10.0f, -3.0f * tauR / rt60Sec), 0.10f, 0.982f);
    }

    const float initDelayMs = (type <= 3u || (type >= 9u && type <= 11u) || (type >= 16u && type <= 20u)
                               || (type >= 24u && type <= 26u))
                                  ? static_cast<float>(p3) * 1.0f
                                  : 12.0f;
    preDelaySamples_ = std::clamp<std::size_t>(
        static_cast<std::size_t>(initDelayMs * 0.001f * sr),
        1u, preDelayBuf_.size() - 2u);

    inputLpfAlpha_ = lpfIndexToAlpha((type == 7u) ? p3 : p2);
    delayLpfAlpha_ = lpfIndexToAlpha((type == 4u || type == 6u || type == 22u) ? p3 : 11u);

    const float maxDelSamp = static_cast<float>(delayLineL_.size() - 8u);
    auto msToSamples = [sr, maxDelSamp](float ms) noexcept -> float {
        return std::clamp(ms * 0.001f * sr, 4.0f, maxDelSamp);
    };

    if (type == 5u || type == 14u || type == 23u || type == 28u || type == 31u) {
        const float msL = 4.0f + static_cast<float>(p1) * 3.8f;
        const float msR = 4.0f + static_cast<float>(p2) * 3.8f;
        targetDelaySamplesL_ = msToSamples(msL);
        targetDelaySamplesR_ = msToSamples(msR);
        delayFeedback_ = (type == 5u || type == 28u)
                             ? std::clamp(static_cast<float>(p3) * 0.0088f, 0.0f, 0.88f)
                             : 0.35f;
    } else {
        const float ms = 4.0f + static_cast<float>(p1) * 3.8f;
        targetDelaySamplesL_ = msToSamples(ms);
        targetDelaySamplesR_ = msToSamples(ms * 1.015f);
        delayFeedback_ = std::clamp(static_cast<float>(p2) * 0.0088f, 0.0f, 0.88f);
    }

    static constexpr std::array<float, 12> kErBaseMs = {
        7.3f, 11.9f, 17.1f, 23.7f, 31.3f, 39.7f,
        48.9f, 59.3f, 71.1f, 83.9f, 97.7f, 113.3f
    };
    const float erSizeScale = 0.55f + (static_cast<float>(std::min<uint8_t>(p1, 40u)) / 40.0f) * 1.35f;
    for (std::size_t t = 0; t < 12; ++t) {
        const float tapMs = initDelayMs + kErBaseMs[t] * erSizeScale;
        erTapDelays_[t] = std::clamp<std::size_t>(
            static_cast<std::size_t>(tapMs * 0.001f * sr),
            2u, erBuffer_.size() - 4u);

        const float normIdx = static_cast<float>(t) / 11.0f;
        if (type == 10u || type == 25u) {
            erTapGains_[t] = 0.12f + 0.88f * (normIdx * normIdx);
        } else if (type == 9u || type == 24u) {
            erTapGains_[t] = 0.85f - 0.15f * normIdx;
        } else {
            erTapGains_[t] = std::exp(-2.1f * normIdx);
        }
    }
    erFeedback_ = (type >= 24u && type <= 26u)
                      ? std::clamp(static_cast<float>(p2) * 0.0075f, 0.0f, 0.72f)
                      : 0.0f;
    gateHoldDuration_ = static_cast<uint32_t>((0.075f + 0.16f * erSizeScale) * sr);

    const uint8_t rawDrive = (type == 7u) ? p2 : ((type == 8u || type == 15u) ? p3 : p1);
    distDrive_ = 1.5f + (static_cast<float>(rawDrive) / 99.0f) * 18.0f;
    const uint8_t rawCabTone = (type == 7u) ? p3 : ((type == 32u) ? p2 : 8u);
    distToneAlpha_ = lpfIndexToAlpha(std::min<uint8_t>(rawCabTone, 12u));
    distOutTrim_ = (type == 32u) ? (0.25f + 0.65f * (static_cast<float>(std::max<uint8_t>(p3, 25u)) / 99.0f)) : 0.55f;

    const float lowDb  = (type == 12u || type == 29u)
                             ? std::clamp((static_cast<float>(p1) - 12.0f), -12.0f, 12.0f)
                             : 2.5f;
    const float highDb = (type == 12u || type == 29u)
                             ? std::clamp((static_cast<float>(p2) - 12.0f), -12.0f, 12.0f)
                             : std::clamp((static_cast<float>(p3) - 12.0f) * 0.5f, -9.0f, 9.0f);
    const float midFreq = 500.0f + static_cast<float>(p3) * 35.0f;
    const float midDb   = (type == 29u) ? 3.5f : 1.5f;

    configureLowShelf(sr, 180.0f, lowDb, eqL_[0].b0, eqL_[0].b1, eqL_[0].b2, eqL_[0].a1, eqL_[0].a2);
    configurePeakingEq(sr, midFreq, 1.1f, midDb, eqL_[1].b0, eqL_[1].b1, eqL_[1].b2, eqL_[1].a1, eqL_[1].a2);
    configureHighShelf(sr, 4200.0f, highDb, eqL_[2].b0, eqL_[2].b1, eqL_[2].b2, eqL_[2].a1, eqL_[2].a2);
    for (std::size_t b = 0; b < 3; ++b) {
        eqR_[b].b0 = eqL_[b].b0;
        eqR_[b].b1 = eqL_[b].b1;
        eqR_[b].b2 = eqL_[b].b2;
        eqR_[b].a1 = eqL_[b].a1;
        eqR_[b].a2 = eqL_[b].a2;
    }

    modPhaseInc_ = (0.65 + 0.03 * static_cast<double>(p1)) / sampleRate_;
    modDepthSamples_ = (12.0f + 0.65f * static_cast<float>(p2)) * srScale;
}

float Ym3413DspProcessor::readDelayLinear(const std::vector<float>& line,
                                          std::size_t writeIdx,
                                          float delaySamples) const noexcept {
    const std::size_t sz = line.size();
    const float clamped = std::clamp(delaySamples, 1.0f, static_cast<float>(sz - 3u));
    const std::size_t dInt = static_cast<std::size_t>(clamped);
    const float frac = clamped - static_cast<float>(dInt);

    const std::size_t idx0 = (writeIdx + sz - dInt) % sz;
    const std::size_t idx1 = (idx0 + sz - 1u) % sz;
    return line[idx0] + frac * (line[idx1] - line[idx0]);
}

StereoFrame Ym3413DspProcessor::runReverbCore(float monoIn) noexcept {
    const std::size_t pdSz = preDelayBuf_.size();
    const std::size_t pdRead = (preDelayWrite_ + pdSz - preDelaySamples_) % pdSz;
    const float preDelayed = preDelayBuf_[pdRead];
    preDelayBuf_[preDelayWrite_] = monoIn;
    if (++preDelayWrite_ >= pdSz) {
        preDelayWrite_ = 0u;
    }

    float diffused = reverbInLpf_.process(preDelayed * 0.35f, inputLpfAlpha_);
    for (auto& ap : reverbAllpass_) {
        diffused = ap.process(diffused);
    }

    float accL = 0.0f;
    float accR = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        accL += combsL_[i].process(diffused);
        accR += combsR_[i].process(diffused);
    }

    const float outL = (accL * 0.34f) - (accR * 0.08f);
    const float outR = (accR * 0.34f) - (accL * 0.08f);
    return {outL, outR};
}

StereoFrame Ym3413DspProcessor::runDelayCore(float inL, float inR, bool pingPong, bool dualLR) noexcept {
    smoothDelaySamplesL_ += 0.002f * (targetDelaySamplesL_ - smoothDelaySamplesL_);
    smoothDelaySamplesR_ += 0.002f * (targetDelaySamplesR_ - smoothDelaySamplesR_);

    const float tapL = delayLpfL_.process(readDelayLinear(delayLineL_, delayWrite_, smoothDelaySamplesL_), delayLpfAlpha_);
    const float tapR = delayLpfR_.process(readDelayLinear(delayLineR_, delayWrite_, smoothDelaySamplesR_), delayLpfAlpha_);

    float writeL = 0.0f;
    float writeR = 0.0f;
    if (pingPong) {
        const float mono = 0.5f * (inL + inR);
        writeL = mono + tapR * delayFeedback_;
        writeR = tapL * delayFeedback_;
    } else if (dualLR) {
        writeL = inL + tapL * delayFeedback_;
        writeR = inR + tapR * delayFeedback_;
    } else {
        writeL = inL + tapL * delayFeedback_;
        writeR = inR + tapR * delayFeedback_;
    }

    delayLineL_[delayWrite_] = (std::abs(writeL) < 1e-18f) ? 0.0f : std::clamp(writeL, -4.0f, 4.0f);
    delayLineR_[delayWrite_] = (std::abs(writeR) < 1e-18f) ? 0.0f : std::clamp(writeR, -4.0f, 4.0f);
    if (++delayWrite_ >= delayLineL_.size()) {
        delayWrite_ = 0u;
    }

    return {tapL, tapR};
}

StereoFrame Ym3413DspProcessor::runEarlyRefOrGateCore(float monoIn, int mode0Er1Gate2Rev, bool withFeedback) noexcept {
    const float filteredIn = erLpf_.process(monoIn, inputLpfAlpha_);
    const std::size_t sz = erBuffer_.size();

    float sumL = 0.0f;
    float sumR = 0.0f;
    for (std::size_t t = 0; t < 12; ++t) {
        const std::size_t readPos = (erWrite_ + sz - erTapDelays_[t]) % sz;
        const float tap = erBuffer_[readPos] * erTapGains_[t];
        if ((t & 1u) == 0u) {
            sumL += tap;
            sumR += tap * 0.35f;
        } else {
            sumR += tap;
            sumL += tap * 0.35f;
        }
    }

    const float fbSignal = withFeedback ? ((sumL + sumR) * 0.16f * erFeedback_) : 0.0f;
    const float writeVal = filteredIn + fbSignal;
    erBuffer_[erWrite_] = (std::abs(writeVal) < 1e-18f) ? 0.0f : std::clamp(writeVal, -4.0f, 4.0f);
    if (++erWrite_ >= sz) {
        erWrite_ = 0u;
    }

    float scale = 0.32f;
    if (mode0Er1Gate2Rev == 1) {
        const float absIn = std::abs(monoIn);
        if (absIn > 0.015f) {
            gateHoldSamples_ = gateHoldDuration_;
        }
        if (gateHoldSamples_ > 0u) {
            --gateHoldSamples_;
            gateGain_ += 0.02f * (1.0f - gateGain_);
        } else {
            gateGain_ *= 0.9965f;
        }
        scale = 0.34f * gateGain_;
    }

    return {sumL * scale, sumR * scale};
}

float Ym3413DspProcessor::runDistortionSample(float x, int channel) noexcept {
    const float hpfR = 0.985f;
    const float hpf = x - distHpfPrevIn_[channel] + hpfR * distHpfPrevOut_[channel];
    distHpfPrevIn_[channel] = x;
    distHpfPrevOut_[channel] = (std::abs(hpf) < 1e-18f) ? 0.0f : hpf;

    const float driven = hpf * distDrive_;
    const float shaped = std::tanh(driven + 0.08f * driven * driven) - std::tanh(0.08f);
    return distToneLpf_[channel].process(shaped, distToneAlpha_) * distOutTrim_;
}

StereoFrame Ym3413DspProcessor::runToneEq(float inL, float inR) noexcept {
    float yL = inL;
    float yR = inR;
    for (std::size_t b = 0; b < 3; ++b) {
        yL = eqL_[b].process(yL);
        yR = eqR_[b].process(yR);
    }
    return {yL, yR};
}

StereoFrame Ym3413DspProcessor::runDoublerCore(float inL, float inR, bool wideStereo) noexcept {
    modPhase_ += modPhaseInc_;
    if (modPhase_ >= 1.0) {
        modPhase_ -= 1.0;
    }

    const float sinL = static_cast<float>(std::sin(modPhase_ * kTwoPi));
    const float cosR = static_cast<float>(std::cos(modPhase_ * kTwoPi));

    const float baseSamples = static_cast<float>(sampleRate_ * 0.014);
    const float delL = baseSamples + sinL * modDepthSamples_;
    const float delR = (wideStereo ? baseSamples * 1.45f : baseSamples) + (wideStereo ? cosR : -sinL) * modDepthSamples_;

    const float outL = delayLpfL_.process(readDelayLinear(delayLineL_, delayWrite_, delL), delayLpfAlpha_);
    const float outR = delayLpfR_.process(readDelayLinear(delayLineR_, delayWrite_, delR), delayLpfAlpha_);

    delayLineL_[delayWrite_] = inL;
    delayLineR_[delayWrite_] = inR;
    if (++delayWrite_ >= delayLineL_.size()) {
        delayWrite_ = 0u;
    }

    return {outL, outR};
}

StereoFrame Ym3413DspProcessor::processWetStereo(float wetInL, float wetInR) noexcept {
    const uint8_t type = settings_.effectType;
    if (type == 0u || bypass_) {
        return {0.0f, 0.0f};
    }

    if (std::abs(wetInL) > 1e-9f || std::abs(wetInR) > 1e-9f) {
        buffersDirty_ = true;
    }

    const float monoIn = 0.5f * (wetInL + wetInR);
    StereoFrame out{};

    switch (type) {
        case 1u:  // Reverb Hall
        case 2u:  // Reverb Room
        case 3u:  // Reverb Plate
        case 16u: // Church
        case 17u: // Club
        case 18u: // Stage
        case 19u: // Bath Room
        case 20u: // Metal
            out = runReverbCore(monoIn);
            break;

        case 4u:  // Delay
            out = runDelayCore(wetInL, wetInR, false, false);
            break;
        case 5u:  // Delay L/R
            out = runDelayCore(wetInL, wetInR, false, true);
            break;
        case 6u:  // Stereo Echo
            out = runDelayCore(wetInL, wetInR, true, false);
            break;

        case 7u: { // Distortion Rev.
            const float dL = runDistortionSample(wetInL, 0);
            const float dR = runDistortionSample(wetInR, 1);
            const StereoFrame rev = runReverbCore(0.5f * (dL + dR));
            out = {dL * 0.65f + rev.left * 0.75f, dR * 0.65f + rev.right * 0.75f};
            break;
        }

        case 8u: { // Distortion Echo
            const float dL = runDistortionSample(wetInL, 0);
            const float dR = runDistortionSample(wetInR, 1);
            const StereoFrame echo = runDelayCore(dL, dR, true, false);
            out = {dL * 0.65f + echo.left * 0.65f, dR * 0.65f + echo.right * 0.65f};
            break;
        }

        case 9u:  // Gate Reverb
            out = runEarlyRefOrGateCore(monoIn, 1, false);
            break;
        case 10u: // Reverse Gate
            out = runEarlyRefOrGateCore(monoIn, 2, false);
            break;
        case 11u: // Early Ref
            out = runEarlyRefOrGateCore(monoIn, 0, false);
            break;

        case 12u: // Tone Control 1
        case 29u: // Tone Control 2
            out = runToneEq(wetInL, wetInR);
            break;

        case 13u: // Delay & Reverb
        case 21u: { // Tunnel
            const StereoFrame dly = runDelayCore(wetInL, wetInR, false, false);
            const StereoFrame rev = runReverbCore(monoIn + 0.5f * (dly.left + dly.right));
            out = {dly.left * 0.5f + rev.left, dly.right * 0.5f + rev.right};
            break;
        }

        case 14u: { // Delay L/R & Rev.
            const StereoFrame dly = runDelayCore(wetInL, wetInR, false, true);
            const StereoFrame rev = runReverbCore(monoIn + 0.45f * (dly.left + dly.right));
            out = {dly.left * 0.55f + rev.left, dly.right * 0.55f + rev.right};
            break;
        }

        case 15u: { // Dist. & Delay
            const float dL = runDistortionSample(wetInL, 0);
            const float dR = runDistortionSample(wetInR, 1);
            const StereoFrame dly = runDelayCore(dL, dR, false, false);
            out = {dL * 0.7f + dly.left * 0.55f, dR * 0.7f + dly.right * 0.55f};
            break;
        }

        case 22u: // Doubler 1
            out = runDoublerCore(wetInL, wetInR, false);
            break;
        case 23u: // Doubler 2
            out = runDoublerCore(wetInL, wetInR, true);
            break;

        case 24u: // Feed Back Gate
            out = runEarlyRefOrGateCore(monoIn, 1, true);
            break;
        case 25u: // F. Back Reverse
            out = runEarlyRefOrGateCore(monoIn, 2, true);
            break;
        case 26u: // Feed Back E/R
            out = runEarlyRefOrGateCore(monoIn, 0, true);
            break;

        case 27u: // Delay & Tone1
        case 30u: { // Delay & Tone2
            const StereoFrame dly = runDelayCore(wetInL, wetInR, false, false);
            out = runToneEq(wetInL * 0.5f + dly.left, wetInR * 0.5f + dly.right);
            break;
        }

        case 28u: // Dly L/R & Tone1
        case 31u: { // Dly L/R & Tone2
            const StereoFrame dly = runDelayCore(wetInL, wetInR, false, true);
            out = runToneEq(wetInL * 0.5f + dly.left, wetInR * 0.5f + dly.right);
            break;
        }

        case 32u: { // Distortion
            const float dL = runDistortionSample(wetInL, 0);
            const float dR = runDistortionSample(wetInR, 1);
            out = {dL, dR};
            break;
        }

        default:
            out = runReverbCore(monoIn);
            break;
    }

    if (std::abs(out.left) > 1e-9f || std::abs(out.right) > 1e-9f) {
        buffersDirty_ = true;
    }
    return out;
}

StereoFrame Ym3413DspProcessor::processMixBus(float dryOnlyL, float dryOnlyR,
                                              float efctDryL, float efctDryR,
                                              float wetSendL, float wetSendR) noexcept {
    // Advance click-free algorithm transition ramp if scheduled
    constexpr float kTransitionStep = 1.0f / 192.0f; // ~3.84 ms @ 50 kHz
    if (transitionState_ == TransitionState::FadingOut) {
        transitionGain_ -= kTransitionStep;
        if (transitionGain_ <= 0.0f) {
            transitionGain_ = 0.0f;
            settings_ = stagedSettings_;
            hasPendingTypeSwitch_ = false;
            clearDelayMemory();
            recalculateCoefficients();
            smoothDelaySamplesL_ = targetDelaySamplesL_;
            smoothDelaySamplesR_ = targetDelaySamplesR_;
            transitionState_ = TransitionState::FadingIn;
        }
    } else if (transitionState_ == TransitionState::FadingIn) {
        transitionGain_ += kTransitionStep;
        if (transitionGain_ >= 1.0f) {
            transitionGain_ = 1.0f;
            transitionState_ = TransitionState::Idle;
        }
    }

    smoothBalance_  += 0.004f * (targetBalance_ - smoothBalance_);
    smoothOutLevel_ += 0.004f * (targetOutLevel_ - smoothOutLevel_);

    if (bypass_ || settings_.effectType == 0u) {
        return {dryOnlyL + efctDryL, dryOnlyR + efctDryR};
    }

    const StereoFrame wet = processWetStereo(wetSendL, wetSendR);
    const float wetGain = smoothBalance_ * smoothOutLevel_ * transitionGain_;

    if (settings_.stereoMix != 0u) {
        return {
            dryOnlyL + efctDryL + wetGain * wet.left,
            dryOnlyR + efctDryR + wetGain * wet.right
        };
    }

    const float dryKeep = 1.0f - (smoothBalance_ * transitionGain_);
    return {
        dryOnlyL + dryKeep * efctDryL + wetGain * wet.left,
        dryOnlyR + dryKeep * efctDryR + wetGain * wet.right
    };
}

} // namespace vf50::dsp::effects
