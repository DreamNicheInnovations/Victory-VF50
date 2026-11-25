#include "DSP/VF50SynthEngine.h"

#include <algorithm>
#include <cmath>

namespace vf50::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float  kPiF = 3.14159265358979323846f;
constexpr float  kHalfPiF = 1.57079632679489661923f;

void configureShelfLow(float sampleRate, float freqHz, float gainDb,
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

void configureShelfHigh(float sampleRate, float freqHz, float gainDb,
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

// Modified Bessel function I0(x) for Kaiser window generation
[[nodiscard]] double besselI0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    const double halfXSq = 0.25 * x * x;
    for (int k = 1; k <= 20; ++k) {
        term *= halfXSq / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-14 * sum) {
            break;
        }
    }
    return sum;
}
} // namespace

VF50SynthEngine::VF50SynthEngine() {
    for (std::size_t ch = 0u; ch < 9u; ++ch) {
        targetBalance_[ch].store(0.0f, std::memory_order_relaxed);
        targetFxSend_[ch].store((ch < 8u) ? 99u : 50u, std::memory_order_relaxed);
        targetEqEnabled_[ch].store(false, std::memory_order_relaxed);
        targetEqLowDb_[ch].store(0.0f, std::memory_order_relaxed);
        targetEqHighDb_[ch].store(0.0f, std::memory_order_relaxed);
        smoothBal_[ch] = 0.0f;
        smoothFxSend_[ch] = (ch < 8u) ? 1.0f : (50.0f / 99.0f);
    }
    dspProcessor_.prepare(kNativeBusSampleRate);
    prepare(48000.0, 2048u);
    selectSingleVoice(0u, false, true);
}

void VF50SynthEngine::setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadDefaults) noexcept {
    fmEngine_.setRuntimeRomBank(romBank, reloadDefaults);
    drumEngine_.setRuntimeRomBank(romBank, reloadDefaults);
    demoPlayer_.setRuntimeRomBank(romBank);
}

void VF50SynthEngine::prepare(double hostSampleRate, std::size_t maxBlockSize) noexcept {
    (void)maxBlockSize;
    hostSampleRate_ = std::clamp(hostSampleRate, 8000.0, 384000.0);
    stepRatio50kPerHost_ = kNativeBusSampleRate / hostSampleRate_;

    dspProcessor_.prepare(kNativeBusSampleRate);
    drumEngine_.prepareHostResampler(hostSampleRate_);

    dcBlock50kL_.coeffR = static_cast<float>(std::exp(-2.0 * kPi * 8.0 / kNativeBusSampleRate));
    dcBlock50kR_.coeffR = dcBlock50kL_.coeffR;

    dcBlockHostL_.coeffR = static_cast<float>(std::exp(-2.0 * kPi * 5.0 / hostSampleRate_));
    dcBlockHostR_.coeffR = dcBlockHostL_.coeffR;
    for (std::size_t ch = 0u; ch < 9u; ++ch) {
        auxDcBlockHostL_[ch].coeffR = dcBlockHostL_.coeffR;
        auxDcBlockHostR_[ch].coeffR = dcBlockHostR_.coeffR;
    }

    rebuildResamplerTable();
    reset();
}

void VF50SynthEngine::reset() noexcept {
    fmEngine_.reset();
    drumEngine_.reset();
    dspProcessor_.reset();
    demoPlayer_.reset();
    lastDrumTelemetry_ = {};

    ring50k_.fill({});
    for (auto& auxRing : auxRing50k_) {
        auxRing.fill({});
    }
    ringWrite_ = 0u;
    resamplerFrac_ = 0.0;
    smoothMasterVolume_ = masterVolume_;

    dcBlock50kL_.clear();
    dcBlock50kR_.clear();
    dcBlockHostL_.clear();
    dcBlockHostR_.clear();
    for (std::size_t ch = 0u; ch < 9u; ++ch) {
        auxDcBlockHostL_[ch].clear();
        auxDcBlockHostR_[ch].clear();
    }

    for (std::size_t ch = 0u; ch < 9u; ++ch) {
        smoothBal_[ch] = std::clamp(targetBalance_[ch].load(std::memory_order_relaxed), -1.0f, 1.0f);
        smoothFxSend_[ch] = static_cast<float>(std::min<uint8_t>(targetFxSend_[ch].load(std::memory_order_relaxed), 99u)) / 99.0f;
        channelEq_[ch].resetToTargets(
            targetEqEnabled_[ch].load(std::memory_order_relaxed),
            std::clamp(targetEqLowDb_[ch].load(std::memory_order_relaxed), -12.0f, 12.0f),
            std::clamp(targetEqHighDb_[ch].load(std::memory_order_relaxed), -12.0f, 12.0f));
    }

    for (auto& pk : instPeakL_) pk.store(0.0f, std::memory_order_relaxed);
    for (auto& pk : instPeakR_) pk.store(0.0f, std::memory_order_relaxed);
    rhyPeakL_.store(0.0f, std::memory_order_relaxed);
    rhyPeakR_.store(0.0f, std::memory_order_relaxed);
    masterPeakL_.store(0.0f, std::memory_order_relaxed);
    masterPeakR_.store(0.0f, std::memory_order_relaxed);
    limiterGr_.store(0.0f, std::memory_order_relaxed);
    for (auto& vc : instActiveVoices_) vc.store(0u, std::memory_order_relaxed);
    totalActiveFmVoices_.store(0u, std::memory_order_relaxed);
    totalActiveRhyVoices_.store(0u, std::memory_order_relaxed);
    blockInstPeakL_.fill(0.0f);
    blockInstPeakR_.fill(0.0f);
    blockRhyPeakL_ = 0.0f;
    blockRhyPeakR_ = 0.0f;
    lastFrameFmVoices_.fill(0u);
    lastFrameTotalFmVoices_ = 0u;
    lastFrameTotalRhyVoices_ = 0u;
}

void VF50SynthEngine::rebuildResamplerTable() noexcept {
    const double nyquistScale = std::min(1.0, hostSampleRate_ / kNativeBusSampleRate);
    const double cutoffNorm = 0.455 * nyquistScale;

    constexpr double kKaiserBeta = 6.5;
    const double invI0Beta = 1.0 / besselI0(kKaiserBeta);
    constexpr double halfSpan = static_cast<double>(kSincTaps) * 0.5;

    for (std::size_t p = 0; p <= kSincPhases; ++p) {
        const double frac = static_cast<double>(p) / static_cast<double>(kSincPhases);
        double weightSum = 0.0;
        std::array<double, kSincTaps> rawWeights{};

        for (std::size_t k = 0; k < kSincTaps; ++k) {
            const double d = (static_cast<double>(k) - 7.0) - frac;
            const double normPos = std::clamp(d / halfSpan, -1.0, 1.0);
            const double window = besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - normPos * normPos))) * invI0Beta;

            double sincVal = 0.0;
            const double arg = 2.0 * cutoffNorm * d;
            if (std::abs(arg) < 1e-9) {
                sincVal = 2.0 * cutoffNorm;
            } else {
                sincVal = std::sin(kPi * arg) / (kPi * d);
            }

            rawWeights[k] = sincVal * window;
            weightSum += rawWeights[k];
        }

        const double invSum = (std::abs(weightSum) > 1e-12) ? (1.0 / weightSum) : 1.0;
        for (std::size_t k = 0; k < kSincTaps; ++k) {
            sincTable_[p][k] = static_cast<float>(rawWeights[k] * invSum);
        }
    }
}

void VF50SynthEngine::selectSingleVoice(std::size_t voiceIndex, bool fromInternalBank, bool syncEffects) noexcept {
    fmEngine_.selectSingleVoice(voiceIndex, fromInternalBank);
    if (syncEffects) {
        dspProcessor_.setSettings(fmEngine_.currentPresetEffectSettings());
    }
}

void VF50SynthEngine::selectPerformance(std::size_t perfIndex, bool fromInternalBank, bool syncEffects) noexcept {
    fmEngine_.selectPerformance(perfIndex, fromInternalBank);
    if (syncEffects) {
        dspProcessor_.setSettings(fmEngine_.currentPresetEffectSettings());
    }
}

void VF50SynthEngine::applyAtomicSingleVoicePatch(const vf50::generated::FactoryVoice& voice,
                                                 std::size_t presetIndex,
                                                 bool fromInternalBank,
                                                 const fm::QuickEditOffsets& qe,
                                                 uint8_t opEnableMask,
                                                 bool singleEffectSend,
                                                 bool syncEffects,
                                                 bool dampExistingVoices) noexcept {
    if (fmEngine_.playMode() != fm::SynthPlayMode::Single) {
        fmEngine_.setPlayMode(fm::SynthPlayMode::Single);
    }
    fmEngine_.applyAtomicSingleVoicePatch(voice, presetIndex, fromInternalBank, qe,
                                          opEnableMask, singleEffectSend, dampExistingVoices);
    if (syncEffects) {
        dspProcessor_.loadPatchSettingsImmediate(voice.effect);
    }
    if ((dampExistingVoices || fmEngine_.isVoiceDampOn())
        && !demoPlayer_.isPlaying() && drumEngine_.activeVoiceCount() == 0u && !drumEngine_.isPatternPlaying()) {
        ring50k_.fill({});
        ringWrite_ = 0u;
        resamplerFrac_ = 0.0;
        smoothMasterVolume_ = masterVolume_;
        dcBlock50kL_.clear();
        dcBlock50kR_.clear();
        dcBlockHostL_.clear();
        dcBlockHostR_.clear();
    }
}

void VF50SynthEngine::applyAtomicPerformancePatch(const vf50::generated::FactoryPerformance& perf,
                                                 std::size_t perfIndex,
                                                 bool fromInternalBank,
                                                 const std::array<int8_t, fm::VF50FmEngine::kMaxPerformanceParts>& pans,
                                                 bool syncEffects) noexcept {
    if (fmEngine_.playMode() != fm::SynthPlayMode::Performance) {
        fmEngine_.setPlayMode(fm::SynthPlayMode::Performance);
    }
    fmEngine_.applyAtomicPerformancePatch(perf, perfIndex, fromInternalBank, pans);
    if (syncEffects) {
        dspProcessor_.loadPatchSettingsImmediate(perf.effect);
    }
    if (!demoPlayer_.isPlaying() && drumEngine_.activeVoiceCount() == 0u && !drumEngine_.isPatternPlaying()) {
        ring50k_.fill({});
        ringWrite_ = 0u;
        resamplerFrac_ = 0.0;
        smoothMasterVolume_ = masterVolume_;
        dcBlock50kL_.clear();
        dcBlock50kR_.clear();
        dcBlockHostL_.clear();
        dcBlockHostR_.clear();
    }
}

void VF50SynthEngine::setPlayMode(fm::SynthPlayMode mode, bool syncEffects) noexcept {
    fmEngine_.setPlayMode(mode);
    if (syncEffects) {
        dspProcessor_.setSettings(fmEngine_.currentPresetEffectSettings());
    }
}

void VF50SynthEngine::setMasterVolume(float linearGain0To1) noexcept {
    masterVolume_ = std::clamp(linearGain0To1, 0.0f, 1.5f);
}

void VF50SynthEngine::setSynthVolume(uint8_t synthVol0To99) noexcept {
    synthVolume_ = std::min<uint8_t>(synthVol0To99, 99u);
    synthVolumeGain_ = static_cast<float>(synthVolume_) / 99.0f;
}

namespace {

void setAtomicBit9(std::atomic<uint16_t>& atom, std::size_t ch0To8, bool value) noexcept {
    if (ch0To8 >= 9u) {
        return;
    }
    const uint16_t bit = static_cast<uint16_t>(uint16_t{ 1u } << ch0To8);
    uint16_t cur = atom.load(std::memory_order_acquire);
    for (;;) {
        const uint16_t next = static_cast<uint16_t>((value ? (cur | bit) : (cur & ~bit)) & 0x01FFu);
        if (atom.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
}

void toggleAtomicBit9(std::atomic<uint16_t>& atom, std::size_t ch0To8) noexcept {
    if (ch0To8 >= 9u) {
        return;
    }
    const uint16_t bit = static_cast<uint16_t>(uint16_t{ 1u } << ch0To8);
    uint16_t cur = atom.load(std::memory_order_acquire);
    for (;;) {
        const uint16_t next = static_cast<uint16_t>((cur ^ bit) & 0x01FFu);
        if (atom.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
}

} // namespace

void VF50SynthEngine::setChannelSoloed(std::size_t ch0To8, bool soloed) noexcept {
    setAtomicBit9(soloMask_, ch0To8, soloed);
}

void VF50SynthEngine::toggleChannelSolo(std::size_t ch0To8) noexcept {
    toggleAtomicBit9(soloMask_, ch0To8);
}

void VF50SynthEngine::setChannelAuxExclusive(std::size_t ch0To8, bool auxExclusive) noexcept {
    setAtomicBit9(auxExclusiveMask_, ch0To8, auxExclusive);
}

void VF50SynthEngine::toggleChannelAuxExclusive(std::size_t ch0To8) noexcept {
    toggleAtomicBit9(auxExclusiveMask_, ch0To8);
}

void VF50SynthEngine::setChannelBalance(std::size_t ch0To8, float balanceNeg1ToPos1) noexcept {
    if (ch0To8 < 9u) {
        const float clamped = std::isfinite(balanceNeg1ToPos1)
                                  ? std::clamp(balanceNeg1ToPos1, -1.0f, 1.0f)
                                  : 0.0f;
        targetBalance_[ch0To8].store(clamped, std::memory_order_relaxed);
    }
}

float VF50SynthEngine::channelBalance(std::size_t ch0To8) const noexcept {
    return (ch0To8 < 9u) ? targetBalance_[ch0To8].load(std::memory_order_relaxed) : 0.0f;
}

void VF50SynthEngine::setChannelFxSend(std::size_t ch0To8, uint8_t send0To99) noexcept {
    if (ch0To8 < 9u) {
        targetFxSend_[ch0To8].store(std::min<uint8_t>(send0To99, 99u), std::memory_order_relaxed);
    }
}

uint8_t VF50SynthEngine::channelFxSend(std::size_t ch0To8) const noexcept {
    return (ch0To8 < 9u) ? targetFxSend_[ch0To8].load(std::memory_order_relaxed) : 0u;
}

void VF50SynthEngine::setChannelEqEnabled(std::size_t ch0To8, bool enabled) noexcept {
    if (ch0To8 < 9u) {
        targetEqEnabled_[ch0To8].store(enabled, std::memory_order_relaxed);
    }
}

bool VF50SynthEngine::channelEqEnabled(std::size_t ch0To8) const noexcept {
    return (ch0To8 < 9u) ? targetEqEnabled_[ch0To8].load(std::memory_order_relaxed) : false;
}

void VF50SynthEngine::setChannelEqLowDb(std::size_t ch0To8, float dbNeg12ToPos12) noexcept {
    if (ch0To8 < 9u) {
        const float clamped = std::isfinite(dbNeg12ToPos12)
                                  ? std::clamp(dbNeg12ToPos12, -12.0f, 12.0f)
                                  : 0.0f;
        targetEqLowDb_[ch0To8].store(clamped, std::memory_order_relaxed);
    }
}

float VF50SynthEngine::channelEqLowDb(std::size_t ch0To8) const noexcept {
    return (ch0To8 < 9u) ? targetEqLowDb_[ch0To8].load(std::memory_order_relaxed) : 0.0f;
}

void VF50SynthEngine::setChannelEqHighDb(std::size_t ch0To8, float dbNeg12ToPos12) noexcept {
    if (ch0To8 < 9u) {
        const float clamped = std::isfinite(dbNeg12ToPos12)
                                  ? std::clamp(dbNeg12ToPos12, -12.0f, 12.0f)
                                  : 0.0f;
        targetEqHighDb_[ch0To8].store(clamped, std::memory_order_relaxed);
    }
}

float VF50SynthEngine::channelEqHighDb(std::size_t ch0To8) const noexcept {
    return (ch0To8 < 9u) ? targetEqHighDb_[ch0To8].load(std::memory_order_relaxed) : 0.0f;
}

void VF50SynthEngine::resetConsoleMixExtensions(const vf50::generated::FactoryPerformance& perf,
                                               drums::RhythmEffectMode rhyEfctMode,
                                               bool clearSoloToo) noexcept {
    for (std::size_t ch = 0u; ch < 8u; ++ch) {
        targetBalance_[ch].store(0.0f, std::memory_order_relaxed);
        targetFxSend_[ch].store((perf.instruments[ch].effectEnable != 0u) ? 99u : 0u, std::memory_order_relaxed);
        targetEqEnabled_[ch].store(false, std::memory_order_relaxed);
        targetEqLowDb_[ch].store(0.0f, std::memory_order_relaxed);
        targetEqHighDb_[ch].store(0.0f, std::memory_order_relaxed);
    }
    if (fmEngine_.playMode() == fm::SynthPlayMode::Single) {
        targetFxSend_[0].store(fmEngine_.singleEffectSend() ? 99u : 0u, std::memory_order_relaxed);
    }
    targetBalance_[8].store(0.0f, std::memory_order_relaxed);
    uint8_t rhySend = 50u;
    if (rhyEfctMode == drums::RhythmEffectMode::Off) {
        rhySend = 0u;
    } else if (rhyEfctMode == drums::RhythmEffectMode::On) {
        rhySend = 99u;
    }
    targetFxSend_[8].store(rhySend, std::memory_order_relaxed);
    targetEqEnabled_[8].store(false, std::memory_order_relaxed);
    targetEqLowDb_[8].store(0.0f, std::memory_order_relaxed);
    targetEqHighDb_[8].store(0.0f, std::memory_order_relaxed);

    if (clearSoloToo) {
        clearSolo();
    }
}

void VF50SynthEngine::ChannelEqState::resetToTargets(bool enabled, float lowDb, float highDb) noexcept {
    lowShelf.clear();
    highShelf.clear();
    smoothLowDb = lowDb;
    smoothHighDb = highDb;
    smoothWet = enabled ? 1.0f : 0.0f;
    lastCalcLowDb = 999.0f;
    lastCalcHighDb = 999.0f;
    subCounter = 0u;
}

void VF50SynthEngine::ChannelEqState::process(float& l,
                                             float& r,
                                             bool enabled,
                                             float targetLowDb,
                                             float targetHighDb,
                                             float sampleRateHz) noexcept {
    const float wetTarget = enabled ? 1.0f : 0.0f;
    if (!enabled && smoothWet < 1e-4f) {
        smoothWet = 0.0f;
        smoothLowDb = targetLowDb;
        smoothHighDb = targetHighDb;
        return;
    }

    smoothWet += 0.012f * (wetTarget - smoothWet);
    smoothLowDb += 0.010f * (targetLowDb - smoothLowDb);
    smoothHighDb += 0.010f * (targetHighDb - smoothHighDb);

    if ((subCounter++ & 31u) == 0u || lastCalcLowDb > 100.0f) {
        if (std::abs(smoothLowDb - lastCalcLowDb) > 0.01f) {
            if (std::abs(smoothLowDb) < 0.01f) {
                lowShelf.setIdentity();
            } else {
                configureShelfLow(sampleRateHz, 120.0f, smoothLowDb,
                                  lowShelf.b0, lowShelf.b1, lowShelf.b2, lowShelf.a1, lowShelf.a2);
            }
            lastCalcLowDb = smoothLowDb;
        }
        if (std::abs(smoothHighDb - lastCalcHighDb) > 0.01f) {
            if (std::abs(smoothHighDb) < 0.01f) {
                highShelf.setIdentity();
            } else {
                configureShelfHigh(sampleRateHz, 4500.0f, smoothHighDb,
                                   highShelf.b0, highShelf.b1, highShelf.b2, highShelf.a1, highShelf.a2);
            }
            lastCalcHighDb = smoothHighDb;
        }
    }

    const float dryL = l;
    const float dryR = r;
    lowShelf.process(l, r);
    highShelf.process(l, r);
    if (smoothWet < 0.9995f) {
        l = dryL + smoothWet * (l - dryL);
        r = dryR + smoothWet * (r - dryR);
    }
}

uint8_t VF50SynthEngine::transformNoteVelocity(uint8_t rawVelocity,
                                              uint8_t fixedVelocity0Off,
                                              uint8_t velCurve0To7) noexcept {
    if (rawVelocity == 0u) {
        return 0u;
    }
    if (fixedVelocity0Off >= 1u) {
        return std::clamp<uint8_t>(fixedVelocity0Off, 1u, 127u);
    }
    const int raw = std::clamp<int>(rawVelocity & 0x7Fu, 1, 127);
    const uint8_t curve = std::min<uint8_t>(velCurve0To7, 7u);
    if (curve == 0u) {
        return static_cast<uint8_t>(raw);
    }
    if (curve <= 5u) {
        const double exp = 1.0 - static_cast<double>(curve) * 0.14;
        const double norm = static_cast<double>(raw) / 127.0;
        const int scaled = static_cast<int>(std::lround(127.0 * std::pow(norm, exp)));
        return static_cast<uint8_t>(std::clamp(scaled, 1, 127));
    }
    if (curve == 6u) {
        // Spec §3.6 (p. 115): 6(flat) — nearly flat response capped at maximum velocity 80
        const int flat = 40 + (raw * 40) / 127;
        return static_cast<uint8_t>(std::clamp(flat, 1, 80));
    }
    // 7(revers): inverse velocity
    return static_cast<uint8_t>(std::clamp(128 - raw, 1, 127));
}

void VF50SynthEngine::noteOn(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept {
    if (velocity == 0u) {
        noteOff(midiChannel, midiNote);
        return;
    }

    const uint8_t cleanNote = static_cast<uint8_t>(midiNote & 0x7Fu);

    // In Rhythm Play / Rhythm Edit mode, all keyboard & MIDI notes trigger the YM3602 Rhythm Engine directly
    if (rhythmKeyboardMode_) {
        const int drumIdx = drumEngine_.triggerKeyDirect(cleanNote, velocity);
        ++lastDrumTelemetry_.sequence;
        lastDrumTelemetry_.midiNote  = cleanNote;
        lastDrumTelemetry_.drumIndex = static_cast<int8_t>(drumIdx);
        lastDrumTelemetry_.velocity  = velocity;
        return;
    }

    const uint8_t ch = static_cast<uint8_t>(midiChannel & 0x0Fu);
    const uint8_t drumCh = drumEngine_.rhythmReceiveChannel();

    if (drumsEnabled_ && (drumCh == ch || drumCh == 16u)) {
        const int drumIdx = drumEngine_.noteOnWithResult(ch, cleanNote, velocity);
        if (drumIdx >= 0 || drumCh == ch) {
            ++lastDrumTelemetry_.sequence;
            lastDrumTelemetry_.midiNote  = cleanNote;
            lastDrumTelemetry_.drumIndex = static_cast<int8_t>(drumIdx);
            lastDrumTelemetry_.velocity  = velocity;
        }
        if (fmEngine_.playMode() == fm::SynthPlayMode::Single && drumCh == ch) {
            return;
        }
    }

    fmEngine_.noteOn(ch, cleanNote, velocity);
}

void VF50SynthEngine::noteOff(uint8_t midiChannel, uint8_t midiNote) noexcept {
    if (rhythmKeyboardMode_) {
        return;
    }
    fmEngine_.noteOff(static_cast<uint8_t>(midiChannel & 0x0Fu), static_cast<uint8_t>(midiNote & 0x7Fu));
}

void VF50SynthEngine::controlChange(uint8_t midiChannel, uint8_t ccNumber, uint8_t ccValue) noexcept {
    fmEngine_.controlChange(static_cast<uint8_t>(midiChannel & 0x0Fu), ccNumber, ccValue);
}

void VF50SynthEngine::pitchBend(uint8_t midiChannel, int bendValue14Bit) noexcept {
    fmEngine_.pitchBend(static_cast<uint8_t>(midiChannel & 0x0Fu), bendValue14Bit);
}

void VF50SynthEngine::channelPressure(uint8_t midiChannel, uint8_t pressure) noexcept {
    fmEngine_.channelPressure(static_cast<uint8_t>(midiChannel & 0x0Fu), pressure);
}

void VF50SynthEngine::programChange(uint8_t midiChannel, uint8_t programNumber) noexcept {
    const uint8_t ch = static_cast<uint8_t>(midiChannel & 0x0Fu);
    if (drumsEnabled_ && drumEngine_.rhythmReceiveChannel() == ch) {
        return;
    }
    const std::size_t idx = static_cast<std::size_t>(programNumber) % 100u;
    if (fmEngine_.playMode() == fm::SynthPlayMode::Single) {
        const bool fromUser = fmEngine_.currentSingleVoiceFromUserBank();
        const auto& voice = fmEngine_.getVoicePatch(fromUser ? 0u : 1u, static_cast<uint8_t>(idx));
        applyAtomicSingleVoicePatch(voice, idx, fromUser, fm::QuickEditOffsets{ 0, 0, 0, 0 }, 0x0Fu, true, true);
    } else {
        const bool fromUser = fmEngine_.currentPerformanceFromUserBank();
        const auto& perf = fmEngine_.getPerformancePatch(fromUser ? 0u : 1u, static_cast<uint8_t>(idx));
        applyAtomicPerformancePatch(perf, idx, fromUser, fmEngine_.partPans(), true);
    }
}

void VF50SynthEngine::handleRawMidiMessage(const uint8_t* data, std::size_t numBytes) noexcept {
    if (data == nullptr || numBytes == 0u) {
        return;
    }
    const uint8_t status = data[0];
    if (status >= 0xF0u) {
        return;
    }

    const uint8_t msgType = static_cast<uint8_t>(status & 0xF0u);
    const uint8_t ch      = static_cast<uint8_t>(status & 0x0Fu);

    switch (msgType) {
        case 0x80u:
            if (numBytes >= 3u) {
                noteOff(ch, static_cast<uint8_t>(data[1] & 0x7Fu));
            }
            break;
        case 0x90u:
            if (numBytes >= 3u) {
                noteOn(ch, static_cast<uint8_t>(data[1] & 0x7Fu), static_cast<uint8_t>(data[2] & 0x7Fu));
            }
            break;
        case 0xB0u:
            if (numBytes >= 3u) {
                controlChange(ch, static_cast<uint8_t>(data[1] & 0x7Fu), static_cast<uint8_t>(data[2] & 0x7Fu));
            }
            break;
        case 0xC0u:
            if (numBytes >= 2u) {
                programChange(ch, static_cast<uint8_t>(data[1] & 0x7Fu));
            }
            break;
        case 0xD0u:
            if (numBytes >= 2u) {
                channelPressure(ch, static_cast<uint8_t>(data[1] & 0x7Fu));
            }
            break;
        case 0xE0u:
            if (numBytes >= 3u) {
                const int bend = static_cast<int>(data[1] & 0x7Fu) | (static_cast<int>(data[2] & 0x7Fu) << 7);
                pitchBend(ch, bend);
            }
            break;
        default:
            break;
    }
}

void VF50SynthEngine::allNotesOff() noexcept {
    fmEngine_.allNotesOff();
}

void VF50SynthEngine::allSoundOff() noexcept {
    fmEngine_.allSoundOff();
    drumEngine_.allDrumsOff();
    dspProcessor_.reset();
}

float VF50SynthEngine::softCeiling(float x) noexcept {
    if (!std::isfinite(x)) {
        return 0.0f;
    }
    constexpr float kKnee = 0.72f;
    constexpr float kHeadroom = 0.24605f; // Asymptote = 0.96605f (-0.3 dBFS)
    const float absX = std::abs(x);
    if (absX <= kKnee) {
        return x;
    }
    const float compressed = kKnee + kHeadroom * std::tanh((absX - kKnee) / kHeadroom);
    return (x >= 0.0f) ? compressed : -compressed;
}

effects::StereoFrame VF50SynthEngine::renderOneNative50kHzFrame(
    uint16_t hostEnabledAuxMask,
    std::array<effects::StereoFrame, 9>* outAuxFrames) noexcept {
    demoPlayer_.advanceFrame50kHz(fmEngine_, drumEngine_, dspProcessor_);

    smoothMasterVolume_ += 0.004f * (masterVolume_ - smoothMasterVolume_);

    const uint16_t solo = static_cast<uint16_t>(soloMask_.load(std::memory_order_acquire) & 0x01FFu);
    const uint16_t auxExcl = static_cast<uint16_t>(auxExclusiveMask_.load(std::memory_order_relaxed) & 0x01FFu);
    const fm::FmBusFrame fmBus = fmEngine_.renderFrame50kHz(solo);
    const bool renderDrums = drumsEnabled_ || demoPlayer_.isPlaying();
    drums::DrumBusFrame drumBus = renderDrums ? drumEngine_.renderFrame50kHz() : drums::DrumBusFrame{};
    const bool rhySoloBlocked = (solo != 0u) && ((solo & (uint16_t{ 1u } << 8u)) == 0u);
    const bool rhyTrackMuted  = demoPlayer_.isTrackMuted(8u);

    lastFrameFmVoices_ = fmBus.partActiveVoices;
    lastFrameTotalFmVoices_ = fmBus.totalActiveVoices;
    lastFrameTotalRhyVoices_ = (renderDrums && !rhySoloBlocked && !rhyTrackMuted) ? drumBus.activeVoices : 0u;

    const float sg = synthVolumeGain_;
    const bool isSingleMode = (fmEngine_.playMode() == fm::SynthPlayMode::Single);
    const bool singleSendOn = fmEngine_.singleEffectSend();

    float dryOnlyL = 0.0f;
    float dryOnlyR = 0.0f;
    float efctDryL = 0.0f;
    float efctDryR = 0.0f;
    float wetSendL = 0.0f;
    float wetSendR = 0.0f;

    auto processStrip = [&](std::size_t ch, float& sL, float& sR) noexcept {
        const float tBal = std::clamp(targetBalance_[ch].load(std::memory_order_relaxed), -1.0f, 1.0f);
        const float tSend = static_cast<float>(std::min<uint8_t>(targetFxSend_[ch].load(std::memory_order_relaxed), 99u)) / 99.0f;
        const bool isQuiet = (std::abs(sL) + std::abs(sR)) < 1e-7f;
        if (isQuiet) {
            smoothBal_[ch] = tBal;
            smoothFxSend_[ch] = tSend;
        } else {
            smoothBal_[ch] += 0.015f * (tBal - smoothBal_[ch]);
            if (std::abs(tBal - smoothBal_[ch]) < 1e-5f) {
                smoothBal_[ch] = tBal;
            }
            smoothFxSend_[ch] += 0.015f * (tSend - smoothFxSend_[ch]);
            if (std::abs(tSend - smoothFxSend_[ch]) < 1e-5f) {
                smoothFxSend_[ch] = tSend;
            }
        }

        const float b = std::clamp(smoothBal_[ch], -1.0f, 1.0f);
        if (b != 0.0f) {
            const float gL = (b <= 0.0f) ? 1.0f : ((b >= 0.9999f) ? 0.0f : std::cos(b * kHalfPiF));
            const float gR = (b >= 0.0f) ? 1.0f : ((b <= -0.9999f) ? 0.0f : std::cos((-b) * kHalfPiF));
            sL *= gL;
            sR *= gR;
        }

        const bool eqOn = targetEqEnabled_[ch].load(std::memory_order_relaxed);
        const float eqLo = std::clamp(targetEqLowDb_[ch].load(std::memory_order_relaxed), -12.0f, 12.0f);
        const float eqHi = std::clamp(targetEqHighDb_[ch].load(std::memory_order_relaxed), -12.0f, 12.0f);
        channelEq_[ch].process(sL, sR, eqOn, eqLo, eqHi, static_cast<float>(kNativeBusSampleRate));
    };

    for (std::size_t p = 0u; p < 8u; ++p) {
        float chL = fmBus.partL[p] * sg;
        float chR = fmBus.partR[p] * sg;
        processStrip(p, chL, chR);

        const float absL = std::abs(chL);
        const float absR = std::abs(chR);
        if (absL > blockInstPeakL_[p]) blockInstPeakL_[p] = absL;
        if (absR > blockInstPeakR_[p]) blockInstPeakR_[p] = absR;

        const bool auxActive = ((hostEnabledAuxMask >> p) & 1u) != 0u;
        const bool removeFromMain = auxActive && (((auxExcl >> p) & 1u) != 0u);
        if (auxActive && outAuxFrames != nullptr) {
            (*outAuxFrames)[p] = { chL, chR };
        }

        float sendGain = smoothFxSend_[p];
        if (isSingleMode && p == 0u && !singleSendOn) {
            sendGain = 0.0f;
        }
        if (!removeFromMain) {
            dryOnlyL += chL * (1.0f - sendGain);
            dryOnlyR += chR * (1.0f - sendGain);
            efctDryL += chL * sendGain;
            efctDryR += chR * sendGain;
        }
        wetSendL += chL * sendGain;
        wetSendR += chR * sendGain;
    }

    float rhyL = (rhySoloBlocked || rhyTrackMuted) ? 0.0f : drumBus.rawDrumL;
    float rhyR = (rhySoloBlocked || rhyTrackMuted) ? 0.0f : drumBus.rawDrumR;
    processStrip(8u, rhyL, rhyR);

    const float absRhyL = std::abs(rhyL);
    const float absRhyR = std::abs(rhyR);
    if (absRhyL > blockRhyPeakL_) blockRhyPeakL_ = absRhyL;
    if (absRhyR > blockRhyPeakR_) blockRhyPeakR_ = absRhyR;

    const bool rhyAuxActive = ((hostEnabledAuxMask >> 8u) & 1u) != 0u;
    const bool rhyRemoveFromMain = rhyAuxActive && (((auxExcl >> 8u) & 1u) != 0u);
    if (rhyAuxActive && outAuxFrames != nullptr) {
        (*outAuxFrames)[8] = { rhyL, rhyR };
    }

    const float rhySend = smoothFxSend_[8];
    if (!rhyRemoveFromMain) {
        if (drumEngine_.effectMode() == drums::RhythmEffectMode::On) {
            dryOnlyL += rhyL * (1.0f - rhySend);
            dryOnlyR += rhyR * (1.0f - rhySend);
            efctDryL += rhyL * rhySend;
            efctDryR += rhyR * rhySend;
        } else {
            dryOnlyL += rhyL;
            dryOnlyR += rhyR;
        }
    }
    wetSendL += rhyL * rhySend;
    wetSendR += rhyR * rhySend;
    if (!rhySoloBlocked && !rhyTrackMuted) {
        dryOnlyL += drumBus.clickSample;
        dryOnlyR += drumBus.clickSample;
    }

    const effects::StereoFrame mixed = dspProcessor_.processMixBus(
        dryOnlyL, dryOnlyR, efctDryL, efctDryR, wetSendL, wetSendR);

    const float acL = dcBlock50kL_.process(mixed.left * smoothMasterVolume_);
    const float acR = dcBlock50kR_.process(mixed.right * smoothMasterVolume_);

    return { acL, acR };
}

void VF50SynthEngine::renderStereoBlock(float* outLeft, float* outRight, std::size_t numSamples) noexcept {
    MultiBusOutputSpec spec{};
    spec.mainL = outLeft;
    spec.mainR = outRight;
    spec.hostEnabledAuxMask = 0u;
    renderMultiBusBlock(spec, numSamples);
}

void VF50SynthEngine::renderMultiBusBlock(const MultiBusOutputSpec& outputs, std::size_t numSamples) noexcept {
    if (outputs.mainL == nullptr || outputs.mainR == nullptr || numSamples == 0u) {
        return;
    }

    uint16_t activeMask = 0u;
    for (std::size_t ch = 0u; ch < 9u; ++ch) {
        if (((outputs.hostEnabledAuxMask >> ch) & 1u) != 0u &&
            outputs.auxL[ch] != nullptr &&
            outputs.auxR[ch] != nullptr) {
            activeMask |= static_cast<uint16_t>(uint16_t{ 1u } << ch);
        }
    }

    blockInstPeakL_.fill(0.0f);
    blockInstPeakR_.fill(0.0f);
    blockRhyPeakL_ = 0.0f;
    blockRhyPeakR_ = 0.0f;
    float blockMasterL = 0.0f;
    float blockMasterR = 0.0f;
    float blockGr = 0.0f;
    const bool useLimiter = safetyLimiterEnabled_.load(std::memory_order_relaxed);
    std::array<effects::StereoFrame, 9> stepAux{};

    for (std::size_t i = 0; i < numSamples; ++i) {
        resamplerFrac_ += stepRatio50kPerHost_;
        const int wholeSteps = static_cast<int>(resamplerFrac_);
        resamplerFrac_ -= static_cast<double>(wholeSteps);

        for (int s = 0; s < wholeSteps; ++s) {
            ringWrite_ = (ringWrite_ + 1u) & kRingMask;
            ring50k_[ringWrite_] = renderOneNative50kHzFrame(
                activeMask, (activeMask != 0u) ? &stepAux : nullptr);
            if (activeMask != 0u) {
                for (std::size_t ch = 0u; ch < 9u; ++ch) {
                    if (((activeMask >> ch) & 1u) != 0u) {
                        auxRing50k_[ch][ringWrite_] = stepAux[ch];
                    }
                }
            }
        }

        const double scaledPhase = resamplerFrac_ * static_cast<double>(kSincPhases);
        const auto phaseIdx = std::min<std::size_t>(static_cast<std::size_t>(scaledPhase), kSincPhases - 1u);
        const float alpha = static_cast<float>(scaledPhase - static_cast<double>(phaseIdx));
        const auto& row0 = sincTable_[phaseIdx];
        const auto& row1 = sincTable_[phaseIdx + 1u];

        float accL = 0.0f;
        float accR = 0.0f;
        const std::size_t baseIdx = (ringWrite_ + kRingSize - (kSincTaps - 1u)) & kRingMask;
        for (std::size_t k = 0; k < kSincTaps; ++k) {
            const float w = row0[k] + alpha * (row1[k] - row0[k]);
            const auto& sample = ring50k_[(baseIdx + k) & kRingMask];
            accL += sample.left * w;
            accR += sample.right * w;
        }

        const float dcL = dcBlockHostL_.process(std::isfinite(accL) ? accL : 0.0f);
        const float dcR = dcBlockHostR_.process(std::isfinite(accR) ? accR : 0.0f);
        float outSampleL = dcL;
        float outSampleR = dcR;
        if (useLimiter) {
            const float limL = softCeiling(dcL);
            const float limR = softCeiling(dcR);
            const float absInL = std::abs(dcL);
            const float absInR = std::abs(dcR);
            if (absInL > 0.72f) {
                blockGr = std::max(blockGr, 1.0f - std::abs(limL) / absInL);
            }
            if (absInR > 0.72f) {
                blockGr = std::max(blockGr, 1.0f - std::abs(limR) / absInR);
            }
            outSampleL = std::clamp(limL, -0.9661f, 0.9661f);
            outSampleR = std::clamp(limR, -0.9661f, 0.9661f);
        } else {
            outSampleL = std::clamp(dcL, -4.0f, 4.0f);
            outSampleR = std::clamp(dcR, -4.0f, 4.0f);
        }
        outputs.mainL[i] = outSampleL;
        outputs.mainR[i] = outSampleR;
        blockMasterL = std::max(blockMasterL, std::abs(outSampleL));
        blockMasterR = std::max(blockMasterR, std::abs(outSampleR));

        if (activeMask != 0u) {
            for (std::size_t ch = 0u; ch < 9u; ++ch) {
                if (((activeMask >> ch) & 1u) == 0u) {
                    continue;
                }
                float auxAccL = 0.0f;
                float auxAccR = 0.0f;
                const auto& chRing = auxRing50k_[ch];
                for (std::size_t k = 0; k < kSincTaps; ++k) {
                    const float w = row0[k] + alpha * (row1[k] - row0[k]);
                    const auto& sample = chRing[(baseIdx + k) & kRingMask];
                    auxAccL += sample.left * w;
                    auxAccR += sample.right * w;
                }
                const float auxDcL = auxDcBlockHostL_[ch].process(std::isfinite(auxAccL) ? auxAccL : 0.0f);
                const float auxDcR = auxDcBlockHostR_[ch].process(std::isfinite(auxAccR) ? auxAccR : 0.0f);
                outputs.auxL[ch][i] = std::clamp(auxDcL, -4.0f, 4.0f);
                outputs.auxR[ch][i] = std::clamp(auxDcR, -4.0f, 4.0f);
            }
        }
    }

    const float decay = std::exp(-static_cast<float>(numSamples) / (static_cast<float>(hostSampleRate_) * 0.06f));
    auto updatePeakAtomic = [decay](std::atomic<float>& atom, float curBlockPeak) noexcept {
        const float decayed = atom.load(std::memory_order_relaxed) * decay;
        float next = std::max(curBlockPeak, decayed);
        if (next < 1e-5f) {
            next = 0.0f;
        }
        atom.store(next, std::memory_order_relaxed);
    };
    for (std::size_t p = 0u; p < 8u; ++p) {
        updatePeakAtomic(instPeakL_[p], blockInstPeakL_[p]);
        updatePeakAtomic(instPeakR_[p], blockInstPeakR_[p]);
        instActiveVoices_[p].store(lastFrameFmVoices_[p], std::memory_order_relaxed);
    }
    updatePeakAtomic(rhyPeakL_, blockRhyPeakL_);
    updatePeakAtomic(rhyPeakR_, blockRhyPeakR_);
    updatePeakAtomic(masterPeakL_, blockMasterL);
    updatePeakAtomic(masterPeakR_, blockMasterR);
    updatePeakAtomic(limiterGr_, blockGr);
    totalActiveFmVoices_.store(lastFrameTotalFmVoices_, std::memory_order_relaxed);
    totalActiveRhyVoices_.store(lastFrameTotalRhyVoices_, std::memory_order_relaxed);
}

} // namespace vf50::dsp
