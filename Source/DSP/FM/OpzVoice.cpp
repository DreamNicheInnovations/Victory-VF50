#include "DSP/FM/OpzVoice.h"

#include <algorithm>
#include <cmath>

namespace vf50::dsp::fm {

namespace {

[[nodiscard]] double pegRateToSemitonesPerControlStep(uint8_t rate, double controlRateHz) noexcept {
    const int clamped = std::clamp<int>(rate, 0, 99);
    if (clamped >= 99) {
        return 96.0; // Instantaneous jump
    }
    // Exponential rate from 0.5 semitones/sec (PR=0) to ~400 semitones/sec (PR=98)
    const double semiPerSec = 0.5 * std::pow(800.0, static_cast<double>(clamped) / 98.0);
    return semiPerSec / controlRateHz;
}

// Calibrated OPZ II phase modulation scaling:
// Converts signed 13-bit modulator output ([-4094, +4094] at TL=0, or [-8188, +8188] at 2-modulator junctions)
// into 10-bit phase modulation units (1024 = 2*pi rad).
// Single modulator at TL=0 reaches ~3*pi rad (+-1535), with smooth soft-knee saturation above +-1536
// so stacked/summed modulators in ALG 2/3/4 never fold into harsh ultrasonic over-modulation buzz.
[[nodiscard]] inline int32_t scalePhaseMod10(int32_t rawMod) noexcept {
    const int32_t scaled = (rawMod * 5) >> 3; // 0.625x -> ~2558 at TL=0 (~5*pi/2 rad), responsive mid-range PM
    constexpr int32_t kKnee = 2048;
    if (scaled > kKnee) {
        const int32_t excess = scaled - kKnee;
        return kKnee + std::min<int32_t>(512, (excess * 512) / (excess + 512));
    }
    if (scaled < -kKnee) {
        const int32_t excess = -scaled - kKnee;
        return -(kKnee + std::min<int32_t>(512, (excess * 512) / (excess + 512)));
    }
    return scaled;
}

// Calibrated multi-carrier output headroom per FM Algorithm (ALG 1..8), inspired by VF50 Sub-CPU ROM ($AA9B)
inline constexpr std::array<float, 8> kAlgorithmCarrierHeadroom = {
    1.00f, // ALG 1: 1 carrier
    1.00f, // ALG 2: 1 carrier
    1.00f, // ALG 3: 1 carrier
    1.00f, // ALG 4: 1 carrier
    0.72f, // ALG 5: 2 carriers
    0.58f, // ALG 6: 3 carriers
    0.58f, // ALG 7: 3 carriers
    0.50f  // ALG 8: 4 carriers
};

} // namespace

void OpzVoice::reset() noexcept {
    for (auto& op : ops_) {
        op.reset();
    }
    lfoDelayRamp_.reset();
    active_ = false;
    keyDown_ = false;
    sustainedByPedal_ = false;
    allocationOrder_ = 0u;
    ctrlSubCounter_ = 0u;
    pegStage_ = PegStage::Hold;
    pegCurrentSemi_ = 0.0;
    pegTarget1Semi_ = 0.0;
    pegTarget2Semi_ = 0.0;
    pegTarget3Semi_ = 0.0;
    pegRate1Inc_ = 0.0;
    pegRate2Inc_ = 0.0;
    pegRate3Inc_ = 0.0;
    targetPitchCents_ = 6000.0;
    currentGlideCents_ = 6000.0;
    glideStepCents_ = 0.0;
    currentLfoAmpAtten10_ = 0;
    currentEgBiasAtten10_ = 0;
    targetGainScale_ = 1.0f;
    smoothGainScale_ = 1.0f;
    dynamicPanOffset_ = 0.0f;
    quickFadeGain_ = 1.0f;
    quickFadeStep_ = 0.0f;
    lastMonoOut_ = 0.0f;
    declickOffset_ = 0.0f;
    declickStep_ = 0.0f;
    declickSamplesLeft_ = 0u;
    pendingDeclickCapture_ = false;
}

void OpzVoice::configureBaseGainAndPan(bool snapImmediate) noexcept {
    const float partGain = static_cast<float>(std::clamp<int>(cfg_.partVolume, 0, 99)) / 99.0f;
    targetGainScale_ = partGain * partGain; // Musical quadratic volume taper

    switch (cfg_.outAssign) {
        case 0u:
            targetPanLeft_ = 0.0f;
            targetPanRight_ = 0.0f;
            break;
        case 1u: // Out I (Left)
            targetPanLeft_ = 1.0f;
            targetPanRight_ = 0.0f;
            break;
        case 2u: // Out II (Right)
            targetPanLeft_ = 0.0f;
            targetPanRight_ = 1.0f;
            break;
        case 3u: // Out I + II (Stereo Center + fine partPan -64..+63)
        default: {
            const float norm = std::clamp(0.5f + static_cast<float>(cfg_.partPan) / 128.0f, 0.0f, 1.0f);
            const float angle = norm * static_cast<float>(kPi * 0.5);
            targetPanLeft_ = std::cos(angle);
            targetPanRight_ = std::sin(angle);
            break;
        }
    }

    if (snapImmediate) {
        smoothGainScale_ = targetGainScale_;
        panLeft_ = targetPanLeft_;
        panRight_ = targetPanRight_;
    }
}

void OpzVoice::startNote(const vf50::generated::FactoryVoice& patch,
                         const VoiceNoteConfig& cfg,
                         const QuickEditOffsets& qe,
                         uint64_t allocationOrder,
                         bool retriggerSameSlot) noexcept {
    // Capture smooth C0 boundary if stealing an active audible slot
    const bool wasActive = active_;
    if (wasActive) {
        pendingDeclickCapture_ = true;
    } else {
        lastMonoOut_ = 0.0f;
        declickOffset_ = 0.0f;
        declickStep_ = 0.0f;
        declickSamplesLeft_ = 0u;
        pendingDeclickCapture_ = false;
    }

    patch_ = patch;
    cfg_ = cfg;
    allocationOrder_ = allocationOrder;
    active_ = true;
    keyDown_ = true;
    sustainedByPedal_ = false;
    ctrlSubCounter_ = 0u;
    quickFadeGain_ = 1.0f;
    quickFadeStep_ = 0.0f;

    // Combine raw MIDI note + Voice transpose (-24..+24) + Performance Instrument noteShift (-24..+24)
    const double shiftCents = static_cast<double>(static_cast<int>(patch_.transpose) + static_cast<int>(cfg_.partNoteShift)) * 100.0;
    const double partDetuneCents = static_cast<double>(cfg_.partDetune) * 2.5;
    targetPitchCents_ = cfg_.basePitchCents + shiftCents + partDetuneCents + cfg_.masterTuneCents;

    // Portamento setup
    const bool portaActive = (patch_.portaSwitch != 0u) && (patch_.portaTime > 0u) && (cfg_.prevGlideCents >= 0.0);
    if (portaActive) {
        currentGlideCents_ = cfg_.prevGlideCents + shiftCents + partDetuneCents + cfg_.masterTuneCents;
        const double normTime = static_cast<double>(std::min<uint8_t>(patch_.portaTime, 99u)) / 99.0;
        const double glideSec = 0.008 + 2.5 * (normTime * normTime);
        const double controlSteps = std::max(1.0, glideSec * (kOpzNativeSampleRate / 16.0));
        glideStepCents_ = std::max(0.25, std::abs(targetPitchCents_ - currentGlideCents_) / controlSteps);
    } else {
        currentGlideCents_ = targetPitchCents_;
        glideStepCents_ = 0.0;
    }

    initPitchEnvelope(patch_.peg);
    lfoDelayRamp_.trigger(patch_.lfoDelay);
    configureBaseGainAndPan(true);

    const int effectiveScalingNote = std::clamp(
        static_cast<int>(cfg_.midiNote) + static_cast<int>(patch_.transpose) + static_cast<int>(cfg_.partNoteShift),
        0, 127);

    const bool resetPhase = !retriggerSameSlot || (patch_.monoMode == 0u);
    for (std::size_t i = 0; i < 4; ++i) {
        const bool isCarrier = isOperatorCarrier(patch_.algorithm, i);
        ops_[i].noteOn(patch_.operators[i], isCarrier, patch_.reverbRate,
                       effectiveScalingNote, cfg_.velocity, qe, resetPhase);
    }

    ChannelControllerState initCtrl{};
    updateOperatorFrequencies(LfoOutput{}, initCtrl);
}

void OpzVoice::updateLivePatchParameters(const vf50::generated::FactoryVoice& newPatch,
                                         const QuickEditOffsets& qe,
                                         double masterTuneCents,
                                         uint8_t newOpMask,
                                         bool effectSendEnable) noexcept {
    const uint8_t cleanMask = newOpMask & 0x0Fu;
    if (!active_) {
        opMask_ = cleanMask;
        return;
    }

    bool discreteTopologySwitch = ((patch_.algorithm & 0x07u) != (newPatch.algorithm & 0x07u))
                               || ((opMask_ & 0x0Fu) != cleanMask);
    for (std::size_t i = 0; i < 4; ++i) {
        if ((patch_.operators[i].waveform & 0x07u) != (newPatch.operators[i].waveform & 0x07u)) {
            discreteTopologySwitch = true;
            break;
        }
    }
    if (discreteTopologySwitch) {
        pendingDeclickCapture_ = true;
    }

    patch_ = newPatch;
    opMask_ = cleanMask;
    cfg_.masterTuneCents = masterTuneCents;
    cfg_.effectEnable = effectSendEnable;

    const double shiftCents = static_cast<double>(static_cast<int>(patch_.transpose) + static_cast<int>(cfg_.partNoteShift)) * 100.0;
    const double partDetuneCents = static_cast<double>(cfg_.partDetune) * 2.5;
    const double newTargetCents = cfg_.basePitchCents + shiftCents + partDetuneCents + cfg_.masterTuneCents;
    if (std::abs(newTargetCents - targetPitchCents_) > 1e-4) {
        targetPitchCents_ = newTargetCents;
        if (glideStepCents_ <= 0.0) {
            glideStepCents_ = std::max(0.5, std::abs(targetPitchCents_ - currentGlideCents_) / 24.0);
        }
    }

    updatePitchEnvelopeTargets(patch_.peg);

    const int effectiveScalingNote = std::clamp(
        static_cast<int>(cfg_.midiNote) + static_cast<int>(patch_.transpose) + static_cast<int>(cfg_.partNoteShift),
        0, 127);

    for (std::size_t i = 0; i < 4; ++i) {
        const bool isCarrier = isOperatorCarrier(patch_.algorithm, i);
        ops_[i].updateLiveParameters(patch_.operators[i], isCarrier, patch_.reverbRate,
                                     effectiveScalingNote, cfg_.velocity, qe);
    }

    // Force immediate control-rate frequency recalculation on next rendered sample
    ctrlSubCounter_ = 0u;
}

void OpzVoice::updateLivePartParameters(const vf50::generated::PerformanceInstrument& inst,
                                        int8_t partPan,
                                        double basePitchCents,
                                        double masterTuneCents) noexcept {
    if (!active_) {
        return;
    }
    cfg_.partNoteShift   = inst.noteShift;
    cfg_.partDetune      = inst.detune;
    cfg_.partPan         = partPan;
    cfg_.partVolume      = inst.volume;
    cfg_.outAssign       = inst.outAssign;
    cfg_.lfoSelect       = inst.lfoSelect;
    cfg_.effectEnable    = inst.effectEnable != 0u;
    cfg_.basePitchCents  = basePitchCents;
    cfg_.masterTuneCents = masterTuneCents;

    configureBaseGainAndPan(false);

    const double shiftCents = static_cast<double>(static_cast<int>(patch_.transpose) + static_cast<int>(cfg_.partNoteShift)) * 100.0;
    const double partDetuneCents = static_cast<double>(cfg_.partDetune) * 2.5;
    const double newTargetCents = cfg_.basePitchCents + shiftCents + partDetuneCents + cfg_.masterTuneCents;
    if (std::abs(newTargetCents - targetPitchCents_) > 1e-4) {
        targetPitchCents_ = newTargetCents;
        if (glideStepCents_ <= 0.0) {
            glideStepCents_ = std::max(0.5, std::abs(targetPitchCents_ - currentGlideCents_) / 24.0);
        }
    }
    ctrlSubCounter_ = 0u;
}

void OpzVoice::releaseKey(bool sustainPedalDown) noexcept {
    if (!active_ || !keyDown_) {
        return;
    }
    keyDown_ = false;

    if (sustainPedalDown && patch_.sustainSwitch != 0u) {
        sustainedByPedal_ = true;
        return;
    }

    sustainedByPedal_ = false;
    pegStage_ = PegStage::Release;
    for (auto& op : ops_) {
        op.noteOff();
    }
}

void OpzVoice::setSustainPedal(bool sustainPedalDown) noexcept {
    if (!sustainPedalDown && sustainedByPedal_) {
        sustainedByPedal_ = false;
        pegStage_ = PegStage::Release;
        for (auto& op : ops_) {
            op.noteOff();
        }
    }
}

void OpzVoice::forceReleaseQuick() noexcept {
    if (!active_) {
        return;
    }
    keyDown_ = false;
    sustainedByPedal_ = false;
    cfg_.partIndex = -1;     // Detach from active part polyphony counts during quick fade
    quickFadeStep_ = 1.0f / 64.0f; // 1.28 ms click-free voice-steal / mode-switch ramp
}

void OpzVoice::forceReleaseWithDampSpeed(uint8_t speed0To3) noexcept {
    if (!active_) {
        return;
    }
    keyDown_ = false;
    sustainedByPedal_ = false;
    cfg_.partIndex = -1;
    // 0=veryslow (~41ms), 1=slow (~10ms), 2=medium (~2.5ms), 3=fast (~0.64ms)
    static constexpr std::array<float, 4> kDampSteps = {
        1.0f / 2048.0f, 1.0f / 512.0f, 1.0f / 128.0f, 1.0f / 32.0f
    };
    quickFadeStep_ = kDampSteps[static_cast<std::size_t>(std::min<uint8_t>(speed0To3, 3u))];
}

uint16_t OpzVoice::carrierEnvelopeAtten10() const noexcept {
    uint16_t minAtten = 1023u;
    for (std::size_t i = 0; i < 4; ++i) {
        if (isOperatorCarrier(patch_.algorithm, i)) {
            minAtten = std::min(minAtten, ops_[i].currentEnvAtten10());
        }
    }
    return minAtten;
}

void OpzVoice::updatePitchEnvelopeTargets(const vf50::generated::PitchEnvelope& peg) noexcept {
    constexpr double kCtrlRate = kOpzNativeSampleRate / 16.0;
    const auto& tables = getOpzTables();

    pegTarget1Semi_ = tables.pegSemi[std::min<uint8_t>(peg.pl1, 99u)];
    pegTarget2Semi_ = tables.pegSemi[std::min<uint8_t>(peg.pl2, 99u)];
    pegTarget3Semi_ = tables.pegSemi[std::min<uint8_t>(peg.pl3, 99u)];

    pegRate1Inc_ = pegRateToSemitonesPerControlStep(peg.pr1, kCtrlRate);
    pegRate2Inc_ = pegRateToSemitonesPerControlStep(peg.pr2, kCtrlRate);
    pegRate3Inc_ = pegRateToSemitonesPerControlStep(peg.pr3, kCtrlRate);
}

void OpzVoice::initPitchEnvelope(const vf50::generated::PitchEnvelope& peg) noexcept {
    updatePitchEnvelopeTargets(peg);
    pegCurrentSemi_ = pegTarget3Semi_;
    if (pegTarget1Semi_ == pegTarget3Semi_ && pegTarget2Semi_ == pegTarget3Semi_) {
        pegStage_ = PegStage::Hold;
    } else {
        pegStage_ = PegStage::Stage1;
    }
}

void OpzVoice::advancePitchEnvelope() noexcept {
    auto stepToward = [](double current, double target, double step) noexcept -> double {
        if (current < target) {
            return std::min(target, current + step);
        }
        if (current > target) {
            return std::max(target, current - step);
        }
        return target;
    };

    switch (pegStage_) {
        case PegStage::Stage1:
            pegCurrentSemi_ = stepToward(pegCurrentSemi_, pegTarget1Semi_, pegRate1Inc_);
            if (std::abs(pegCurrentSemi_ - pegTarget1Semi_) < 1e-6) {
                pegStage_ = PegStage::Stage2;
            }
            break;

        case PegStage::Stage2:
            pegCurrentSemi_ = stepToward(pegCurrentSemi_, pegTarget2Semi_, pegRate2Inc_);
            if (std::abs(pegCurrentSemi_ - pegTarget2Semi_) < 1e-6) {
                pegStage_ = PegStage::Hold;
            }
            break;

        case PegStage::Hold:
            break;

        case PegStage::Release:
            pegCurrentSemi_ = stepToward(pegCurrentSemi_, pegTarget3Semi_, pegRate3Inc_);
            break;
    }
}

void OpzVoice::updateOperatorFrequencies(const LfoOutput& activeLfo,
                                         const ChannelControllerState& ctrl) noexcept {
    // 1. Advance portamento / live transpose glide
    if (glideStepCents_ > 0.0 && currentGlideCents_ != targetPitchCents_) {
        if (currentGlideCents_ < targetPitchCents_) {
            currentGlideCents_ = std::min(targetPitchCents_, currentGlideCents_ + glideStepCents_);
        } else {
            currentGlideCents_ = std::max(targetPitchCents_, currentGlideCents_ - glideStepCents_);
        }
        if (std::abs(currentGlideCents_ - targetPitchCents_) < 1e-6 && patch_.portaSwitch == 0u) {
            currentGlideCents_ = targetPitchCents_;
            glideStepCents_ = 0.0;
        }
    }

    // 2. Advance Pitch EG
    advancePitchEnvelope();

    // 3. LFO delay envelope + continuous controller pitch/amp matrix
    const float delayFactor = lfoDelayRamp_.tick();

    // Pitch Bend (0..12 semitones)
    const double pbCents = static_cast<double>(ctrl.pitchBendNorm)
                           * static_cast<double>(std::min<uint8_t>(patch_.pitchBendRange, 12u)) * 100.0;

    // Controller-driven vibrato depth (MW, FC, BC, AT) + Voice LFO PMD
    double lfoPitchCents = 0.0;
    double lfoAmpAtten10 = 0.0;
    if (cfg_.lfoSelect != 0u) {
        const double basePmsCents = pmsToMaxCents(patch_.pms) * 0.5; // Convert total range to +- peak cents
        const double ctrlVibratoDepth = (static_cast<double>(patch_.mwPitch) * ctrl.modWheel
                                       + static_cast<double>(patch_.fcPitch) * (1.0 - ctrl.footCtrl)
                                       + static_cast<double>(patch_.bcPitch) * ctrl.breathCtrl
                                       + static_cast<double>(patch_.atPitch) * ctrl.aftertouch) / 99.0;

        if (cfg_.lfoSelect == 3u) {
            lfoPitchCents = static_cast<double>(activeLfo.rawBipolar) * (ctrlVibratoDepth * 150.0);
        } else {
            lfoPitchCents = static_cast<double>(activeLfo.pitchBipolar) * (basePmsCents * delayFactor)
                          + static_cast<double>(activeLfo.rawBipolar) * (ctrlVibratoDepth * 100.0);
        }

        const double baseAmsAtten = amsToMaxAtten10(patch_.ams);
        const double ctrlTremolo = (static_cast<double>(patch_.mwAmp) * ctrl.modWheel
                                  + static_cast<double>(patch_.fcAmp) * (1.0 - ctrl.footCtrl)
                                  + static_cast<double>(patch_.bcAmp) * ctrl.breathCtrl
                                  + static_cast<double>(patch_.atAmp) * ctrl.aftertouch) / 99.0;
        lfoAmpAtten10 = static_cast<double>(activeLfo.ampUnipolar) * delayFactor * baseAmsAtten
                        + std::abs(static_cast<double>(activeLfo.rawBipolar)) * ctrlTremolo * 128.0;
    }

    // Breath & Aftertouch Pitch Bias (-50..+50 -> +-12 semitones) and EG Bias (0..99)
    const double pitchBiasCents = (static_cast<double>(patch_.bcPitchBias) * ctrl.breathCtrl
                                 + static_cast<double>(patch_.atPitchBias) * ctrl.aftertouch) * 24.0;

    const double egBiasActive = std::max(
        (patch_.bcEgBias > 0u) ? (static_cast<double>(patch_.bcEgBias) / 99.0) * (1.0 - ctrl.breathCtrl) : 0.0,
        (patch_.atEgBias > 0u) ? (static_cast<double>(patch_.atEgBias) / 99.0) * (1.0 - ctrl.aftertouch) : 0.0);

    currentLfoAmpAtten10_ = static_cast<int32_t>(std::clamp(lfoAmpAtten10, 0.0, 512.0));
    currentEgBiasAtten10_ = static_cast<int32_t>(std::clamp(egBiasActive * 512.0, 0.0, 512.0));

    const double totalPitchCents = currentGlideCents_ + (pegCurrentSemi_ * 100.0)
                                 + pbCents + lfoPitchCents + pitchBiasCents;
    // Concert A4 = 6900 cents = 440.0 Hz
    const double noteFreqHz = 440.0 * std::exp2((totalPitchCents - 6900.0) / 1200.0);
    const double approxMidiNote = totalPitchCents / 100.0;

    for (std::size_t i = 0; i < 4; ++i) {
        const auto& opSpec = patch_.operators[i];
        double opHz = 0.0;
        if (opSpec.fixedFreqMode != 0u) {
            opHz = computeOpzFixedFreqHz(opSpec.coarse, opSpec.fine, opSpec.fixedRange, opSpec.fixedRangeLow);
            if (opHz > 0.0 && opSpec.detune != 0) {
                if (opSpec.fixedRangeLow != 0u) {
                    const double loDetHz = computeOpzDetuneHz(60.0, opSpec.detune) * (171.0 / 51200.0);
                    opHz = std::max(0.0, opHz + loDetHz);
                } else {
                    const double eqMidi = std::clamp(69.0 + 12.0 * std::log2(opHz / 440.0), 12.0, 108.0);
                    opHz = std::max(0.0, opHz + computeOpzDetuneHz(eqMidi, opSpec.detune));
                }
            }
        } else {
            const double ratio = computeOpzRatioMultiplier(opSpec.coarse, opSpec.fine, coarsePermOverride_);
            const double detHz = computeOpzDetuneHz(approxMidiNote, opSpec.detune);
            opHz = std::max(0.0, noteFreqHz * ratio + detHz);
        }
        ops_[i].setFrequencyHz(opHz);
    }
}

VoiceRenderSample OpzVoice::renderSample(const LfoOutput& lfo1,
                                         const LfoOutput& lfo2,
                                         const LfoOutput& vibLfo,
                                         const ChannelControllerState& ctrl) noexcept {
    if (!active_) {
        return {};
    }

    if ((ctrlSubCounter_++ & 0x0Fu) == 0u) {
        const LfoOutput* chosenLfo = &lfo1;
        if (cfg_.lfoSelect == 2u) {
            chosenLfo = &lfo2;
        } else if (cfg_.lfoSelect == 3u) {
            chosenLfo = &vibLfo;
        }
        updateOperatorFrequencies(*chosenLfo, ctrl);
    } else {
        (void)lfoDelayRamp_.tick();
    }

    // Smooth part volume & stereo pan per sample
    smoothGainScale_ += 0.015f * (targetGainScale_ - smoothGainScale_);
    panLeft_         += 0.015f * (targetPanLeft_ - panLeft_);
    panRight_        += 0.015f * (targetPanRight_ - panRight_);

    const int32_t aMod = currentLfoAmpAtten10_;
    const int32_t eBias = currentEgBiasAtten10_;
    const uint8_t fb = patch_.feedback;

    const auto maskOut = [this](std::size_t idx, int32_t val) noexcept -> int32_t {
        return ((opMask_ & (1u << idx)) != 0u) ? val : 0;
    };

    const uint8_t algIdx = patch_.algorithm & 0x07u;
    int32_t carrierSum = 0;
    switch (algIdx) {
        case 0u: { // ALG 1: OP4(FB) -> OP3 -> OP2 -> OP1
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t o3 = maskOut(2, ops_[2].tick(scalePhaseMod10(o4), aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(scalePhaseMod10(o3), aMod, eBias));
            carrierSum       = maskOut(0, ops_[0].tick(scalePhaseMod10(o2), aMod, eBias));
            break;
        }
        case 1u: { // ALG 2: (OP4(FB) + OP3) -> OP2 -> OP1
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t o3 = maskOut(2, ops_[2].tick(0, aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(scalePhaseMod10(o4 + o3), aMod, eBias));
            carrierSum       = maskOut(0, ops_[0].tick(scalePhaseMod10(o2), aMod, eBias));
            break;
        }
        case 2u: { // ALG 3: (OP4(FB) + (OP3 -> OP2)) -> OP1
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t o3 = maskOut(2, ops_[2].tick(0, aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(scalePhaseMod10(o3), aMod, eBias));
            carrierSum       = maskOut(0, ops_[0].tick(scalePhaseMod10(o4 + o2), aMod, eBias));
            break;
        }
        case 3u: { // ALG 4: ((OP4(FB) -> OP3) + OP2) -> OP1
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t o3 = maskOut(2, ops_[2].tick(scalePhaseMod10(o4), aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(0, aMod, eBias));
            carrierSum       = maskOut(0, ops_[0].tick(scalePhaseMod10(o3 + o2), aMod, eBias));
            break;
        }
        case 4u: { // ALG 5: (OP2 -> OP1) + (OP4(FB) -> OP3)
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t o3 = maskOut(2, ops_[2].tick(scalePhaseMod10(o4), aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(0, aMod, eBias));
            const int32_t o1 = maskOut(0, ops_[0].tick(scalePhaseMod10(o2), aMod, eBias));
            carrierSum       = o1 + o3;
            break;
        }
        case 5u: { // ALG 6: OP4(FB) -> (OP1 + OP2 + OP3)
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t m4 = scalePhaseMod10(o4);
            const int32_t o3 = maskOut(2, ops_[2].tick(m4, aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(m4, aMod, eBias));
            const int32_t o1 = maskOut(0, ops_[0].tick(m4, aMod, eBias));
            carrierSum       = o1 + o2 + o3;
            break;
        }
        case 6u: { // ALG 7: OP1 + OP2 + (OP4(FB) -> OP3)
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t o3 = maskOut(2, ops_[2].tick(scalePhaseMod10(o4), aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(0, aMod, eBias));
            const int32_t o1 = maskOut(0, ops_[0].tick(0, aMod, eBias));
            carrierSum       = o1 + o2 + o3;
            break;
        }
        case 7u: // ALG 8: OP1 + OP2 + OP3 + OP4(FB)
        default: {
            const int32_t o4 = maskOut(3, ops_[3].tickWithFeedback(fb, aMod, eBias));
            const int32_t o3 = maskOut(2, ops_[2].tick(0, aMod, eBias));
            const int32_t o2 = maskOut(1, ops_[1].tick(0, aMod, eBias));
            const int32_t o1 = maskOut(0, ops_[0].tick(0, aMod, eBias));
            carrierSum       = o1 + o2 + o3 + o4;
            break;
        }
    }

    // Check if all carrier operators have finished releasing (and any declick tail has completed)
    bool anyCarrierAlive = false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (isOperatorCarrier(patch_.algorithm, i) && !ops_[i].isSilent()) {
            anyCarrierAlive = true;
            break;
        }
    }
    if (!anyCarrierAlive && declickSamplesLeft_ == 0u && !pendingDeclickCapture_) {
        active_ = false;
        keyDown_ = false;
        sustainedByPedal_ = false;
        lastMonoOut_ = 0.0f;
        return {};
    }

    if (quickFadeStep_ > 0.0f) {
        quickFadeGain_ -= quickFadeStep_;
        if (quickFadeGain_ <= 0.0f) {
            active_ = false;
            lastMonoOut_ = 0.0f;
            declickSamplesLeft_ = 0u;
            pendingDeclickCapture_ = false;
            return {};
        }
    }

    // Foot controller volume scaling (when patch fcVolume > 0) + MIDI CC#7 / CC#11
    const float fcGain = 1.0f - (static_cast<float>(patch_.fcVolume) / 99.0f) * (1.0f - ctrl.footCtrl);
    const float exprGain = ctrl.channelVolume * ctrl.expression * fcGain;

    // 14-bit operator peak is +-4094; normalize with per-algorithm multi-carrier headroom across 16 voices
    constexpr float kNorm14Bit = 1.0f / 14336.0f;
    const float algScale = kAlgorithmCarrierHeadroom[algIdx];
    const float rawMonoSample = static_cast<float>(carrierSum) * (kNorm14Bit * algScale) * smoothGainScale_ * exprGain * quickFadeGain_;

    // Apply C0 boundary de-click / EG Forced Damp ramp if algorithm, waveform, or voice-steal occurred
    if (pendingDeclickCapture_) {
        static constexpr std::array<uint32_t, 4> kStealDampSamples = { 1024u, 256u, 128u, 32u };
        const uint32_t rampLen = kStealDampSamples[static_cast<std::size_t>(std::min<uint8_t>(egDampSpeed_, 3u))];
        declickOffset_ += (lastMonoOut_ - rawMonoSample);
        declickStep_ = declickOffset_ / static_cast<float>(rampLen);
        declickSamplesLeft_ = rampLen;
        pendingDeclickCapture_ = false;
    }

    float monoSample = rawMonoSample;
    if (declickSamplesLeft_ > 0u) {
        monoSample += declickOffset_;
        declickOffset_ -= declickStep_;
        --declickSamplesLeft_;
        if (declickSamplesLeft_ == 0u) {
            declickOffset_ = 0.0f;
        }
    }
    lastMonoOut_ = monoSample;

    float pL = panLeft_;
    float pR = panRight_;
    if (dynamicPanOffset_ != 0.0f || ctrl.panNorm != 0.5f) {
        const float baseAngle = std::atan2(panRight_, panLeft_); // [0..pi/2]
        const float dynScale = (cfg_.outAssign == 1u || cfg_.outAssign == 2u) ? 1.0f : 0.5f;
        const float modOffset = (ctrl.panNorm - 0.5f) + dynamicPanOffset_ * dynScale;
        const float angle = std::clamp(baseAngle + modOffset * static_cast<float>(kPi * 0.5),
                                       0.0f, static_cast<float>(kPi * 0.5));
        pL = std::cos(angle);
        pR = std::sin(angle);
    }

    return VoiceRenderSample{
        .left         = monoSample * pL,
        .right        = monoSample * pR,
        .effectEnable = cfg_.effectEnable
    };
}

} // namespace vf50::dsp::fm
