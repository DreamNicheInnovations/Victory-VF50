#include "DSP/FM/VF50FmEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vf50::dsp::fm {

VF50FmEngine::VF50FmEngine() noexcept {
    setRuntimeRomBank(&vf50::rom::getDefaultRomImage(), true);
    partPans_.fill(0);
    reset();
}

void VF50FmEngine::setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadFactoryDefaults) noexcept {
    romBank_ = (romBank != nullptr) ? romBank : &vf50::rom::getDefaultRomImage();
    const auto& rom = *romBank_;
    for (auto& v : voices_) {
        v.setRomLutOverrides(rom.coarsePermTable.data(), rom.lowOutLevelToTl.data());
    }
    if (reloadFactoryDefaults) {
        internalVoices_       = rom.factoryVoices;
        internalPerformances_ = rom.factoryPerformances;
        userOctaveExpanded_   = rom.factoryMicrotunings[0].keys;
        userFullMicrotuning_  = rom.factoryMicrotunings[0].keys;
        activeVoice_          = rom.factoryVoices[std::min(currentVoiceIndex_, rom.factoryVoices.size() - 1u)];
        activePerformance_    = rom.factoryPerformances[std::min(currentPerfIndex_, rom.factoryPerformances.size() - 1u)];
    }
}

void VF50FmEngine::reset() noexcept {
    for (auto& v : voices_) {
        v.reset();
        v.setOperatorMask(opEnableMask_);
    }
    for (auto& c : channelCtrl_) {
        c = ChannelControllerState{};
    }
    lastPartPitchCents_.fill(-1.0);
    for (auto& e : echoQueue_) {
        e = ScheduledMidiEcho{};
    }

    lfo1_.reset(0x594D3234u);
    lfo2_.reset(0x4F505A32u);
    vibLfo_.reset(0x56494230u);
    vibLfo_.configure(35u, 2u, 99u, 0u, 0u); // ~6.2 Hz smooth triangle vibrato

    allocationCounter_ = 1u;
    alternateRoundRobin_ = 0u;
    perfPartMuteMask_.store(0u, std::memory_order_release);
    activePartHitMask_.store(0u, std::memory_order_release);
    mutedPartHitMask_.store(0u, std::memory_order_release);
    prevPerfPartMuteMask_ = 0u;
    refreshLfoConfigs();
}

void VF50FmEngine::setPlayMode(SynthPlayMode mode) noexcept {
    if (playMode_ != mode) {
        releaseAllVoicesSmooth();
        playMode_ = mode;
        lastPartPitchCents_.fill(-1.0);
        alternateRoundRobin_ = 0u;
        refreshLfoConfigs();
    }
}

void VF50FmEngine::selectSingleVoice(std::size_t presetIndex, bool fromInternalBank) noexcept {
    const std::size_t idx = std::min(presetIndex, vf50::generated::kFactoryVoiceCount - 1u);
    currentVoiceIndex_ = idx;
    currentVoiceUserBank_ = fromInternalBank;
    activeVoice_ = fromInternalBank ? internalVoices_[idx] : runtimeRomBank().factoryVoices[idx];
    lastPartPitchCents_.fill(-1.0);
    if (playMode_ == SynthPlayMode::Single) {
        notifyLiveVoiceParametersChanged();
    } else {
        refreshLfoConfigs();
    }
}

void VF50FmEngine::purgeVoicesAndLfoForPatchSwitch() noexcept {
    // 1. Terminate lingering release tails & clear sustain/echo queues
    allNotesOff();

    // 2. Flush all 16 FM voices (history[0]=0, history[1]=0, phase=0, egStage=Off)
    for (auto& v : voices_) {
        v.reset();
        v.setOperatorMask((playMode_ == SynthPlayMode::Single) ? opEnableMask_ : 0x0Fu);
    }
    for (auto& e : echoQueue_) {
        e = ScheduledMidiEcho{};
    }

    // 3. Reset portamento history & voice allocator counters
    lastPartPitchCents_.fill(-1.0);
    allocationCounter_ = 1u;
    alternateRoundRobin_ = 0u;

    // 4. Reset LFO wave generator phases & RNG state to prevent pitch-warped attack transients
    lfo1_.reset(0x594D3234u);
    lfo2_.reset(0x4F505A32u);
    vibLfo_.reset(0x56494230u);
    vibLfo_.configure(35u, 2u, 99u, 0u, 0u);
    refreshLfoConfigs();
}

void VF50FmEngine::applyAtomicSingleVoicePatch(const vf50::generated::FactoryVoice& voice,
                                              std::size_t presetIndex,
                                              bool fromInternalBank,
                                              const QuickEditOffsets& qe,
                                              uint8_t opEnableMask,
                                              bool singleEffectSend,
                                              bool dampExistingVoices) noexcept {
    currentVoiceIndex_ = std::min(presetIndex, vf50::generated::kFactoryVoiceCount - 1u);
    currentVoiceUserBank_ = fromInternalBank;
    activeVoice_ = voice;
    quickEdit_ = qe;
    opEnableMask_ = static_cast<uint8_t>(opEnableMask & 0x0Fu);
    singleEffectSend_ = singleEffectSend;
    lfo2Override_.enabled = false;
    if (dampExistingVoices || voiceDampOn_) {
        purgeVoicesAndLfoForPatchSwitch();
    } else {
        // Manual p. 116: Voice Damp = off keeps sustaining notes sounding, while LFO updates immediately
        refreshLfoConfigs();
    }
}

void VF50FmEngine::setActiveSingleVoiceDirect(const vf50::generated::FactoryVoice& voice) noexcept {
    activeVoice_ = voice;
    if (playMode_ == SynthPlayMode::Single) {
        notifyLiveVoiceParametersChanged();
    } else {
        refreshLfoConfigs();
    }
}

void VF50FmEngine::selectPerformance(std::size_t perfIndex, bool fromInternalBank) noexcept {
    const std::size_t idx = std::min(perfIndex, vf50::generated::kFactoryPerformanceCount - 1u);
    currentPerfIndex_ = idx;
    currentPerfUserBank_ = fromInternalBank;
    activePerformance_ = fromInternalBank ? internalPerformances_[idx] : runtimeRomBank().factoryPerformances[idx];
    lastPartPitchCents_.fill(-1.0);
    alternateRoundRobin_ = 0u;
    if (playMode_ == SynthPlayMode::Performance) {
        notifyLivePerformanceParametersChanged();
    } else {
        refreshLfoConfigs();
    }
}

void VF50FmEngine::applyAtomicPerformancePatch(const vf50::generated::FactoryPerformance& perf,
                                              std::size_t perfIndex,
                                              bool fromInternalBank,
                                              const std::array<int8_t, kMaxPerformanceParts>& pans) noexcept {
    currentPerfIndex_ = std::min(perfIndex, vf50::generated::kFactoryPerformanceCount - 1u);
    currentPerfUserBank_ = fromInternalBank;
    activePerformance_ = perf;
    partPans_ = pans;
    quickEdit_ = { 0, 0, 0, 0 };
    lfo2Override_.enabled = false;
    purgeVoicesAndLfoForPatchSwitch();
}

void VF50FmEngine::setActivePerformanceDirect(const vf50::generated::FactoryPerformance& perf) noexcept {
    activePerformance_ = perf;
    if (playMode_ == SynthPlayMode::Performance) {
        notifyLivePerformanceParametersChanged();
    } else {
        refreshLfoConfigs();
    }
}

void VF50FmEngine::setPerformanceChannelVoice(uint8_t midiChannel, bool presetBank, uint8_t voiceIndex) noexcept {
    const uint8_t ch = static_cast<uint8_t>(midiChannel & 0x0Fu);
    const uint8_t bankCode = presetBank ? 1u : 0u; // 0 = Internal RAM, 1 = Factory Preset ROM
    const uint8_t clampedVoice = static_cast<uint8_t>(
        std::min<std::size_t>(voiceIndex, vf50::generated::kFactoryVoiceCount - 1u));
    bool updated = false;
    for (auto& inst : activePerformance_.instruments) {
        if (inst.receiveChannel == ch || inst.receiveChannel == 16u) {
            inst.voiceBank   = bankCode;
            inst.voiceNumber = clampedVoice;
            updated          = true;
        }
    }
    if (updated && playMode_ == SynthPlayMode::Performance) {
        notifyLivePerformanceParametersChanged();
    }
}

void VF50FmEngine::storeInternalVoice(std::size_t slotIndex, const vf50::generated::FactoryVoice& voice) noexcept {
    if (slotIndex < internalVoices_.size()) {
        internalVoices_[slotIndex] = voice;
        if (playMode_ == SynthPlayMode::Single && currentVoiceUserBank_ && currentVoiceIndex_ == slotIndex) {
            activeVoice_ = voice;
            notifyLiveVoiceParametersChanged();
        } else if (playMode_ == SynthPlayMode::Performance) {
            notifyLivePerformanceParametersChanged();
        }
    }
}

void VF50FmEngine::storeInternalPerformance(std::size_t slotIndex, const vf50::generated::FactoryPerformance& perf) noexcept {
    if (slotIndex < internalPerformances_.size()) {
        internalPerformances_[slotIndex] = perf;
        if (playMode_ == SynthPlayMode::Performance && currentPerfUserBank_ && currentPerfIndex_ == slotIndex) {
            activePerformance_ = perf;
            notifyLivePerformanceParametersChanged();
        }
    }
}

const vf50::generated::FactoryVoice& VF50FmEngine::getVoicePatch(uint8_t bank, uint8_t number) const noexcept {
    const std::size_t idx = std::min<std::size_t>(number, vf50::generated::kFactoryVoiceCount - 1u);
    if (bank == 0u) {
        return internalVoices_[idx];
    }
    return runtimeRomBank().factoryVoices[idx];
}

const vf50::generated::FactoryPerformance& VF50FmEngine::getPerformancePatch(uint8_t bank, uint8_t number) const noexcept {
    const std::size_t idx = std::min<std::size_t>(number, vf50::generated::kFactoryPerformanceCount - 1u);
    if (bank == 0u) {
        return internalPerformances_[idx];
    }
    return runtimeRomBank().factoryPerformances[idx];
}

void VF50FmEngine::setUserOctaveMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 12>& entries) noexcept {
    for (int note = 0; note < 128; ++note) {
        const int oct = (note / 12) - 5; // C3 (60) is octave 5
        const auto& base = entries[static_cast<std::size_t>(note % 12)];
        const int shiftedCoarse = std::clamp(static_cast<int>(base.coarseNote) + oct * 12, 0, 127);
        userOctaveExpanded_[static_cast<std::size_t>(note)] = {
            static_cast<uint8_t>(shiftedCoarse),
            static_cast<uint8_t>(base.fineStep64 & 0x3Fu)
        };
    }
    if (playMode_ == SynthPlayMode::Performance) {
        notifyLivePerformanceParametersChanged();
    }
}

void VF50FmEngine::setUserFullMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 128>& entries) noexcept {
    userFullMicrotuning_ = entries;
    if (playMode_ == SynthPlayMode::Performance) {
        notifyLivePerformanceParametersChanged();
    }
}

void VF50FmEngine::setMasterTuneCents(double cents) noexcept {
    const double clamped = std::clamp(cents, -100.0, 100.0);
    if (std::abs(clamped - masterTuneCents_) > 1e-6) {
        masterTuneCents_ = clamped;
        notifyLiveVoiceParametersChanged();
    }
}

void VF50FmEngine::setGlobalTransposeSemitones(int semitones) noexcept {
    const int clamped = std::clamp(semitones, -24, 24);
    if (clamped != globalTransposeSemitones_) {
        globalTransposeSemitones_ = clamped;
        notifyLiveVoiceParametersChanged();
    }
}

void VF50FmEngine::setQuickEditOffsets(const QuickEditOffsets& offsets) noexcept {
    if (quickEdit_.attackOffset != offsets.attackOffset
        || quickEdit_.releaseOffset != offsets.releaseOffset
        || quickEdit_.brillianceOffset != offsets.brillianceOffset
        || quickEdit_.volumeOffset != offsets.volumeOffset) {
        quickEdit_ = offsets;
        notifyLiveVoiceParametersChanged();
    }
}

void VF50FmEngine::setOperatorEnableMask(uint8_t mask0To15) noexcept {
    const uint8_t clean = mask0To15 & 0x0Fu;
    if (opEnableMask_ != clean) {
        opEnableMask_ = clean;
        notifyLiveVoiceParametersChanged();
    }
}

void VF50FmEngine::setSingleEffectSend(bool enabled) noexcept {
    if (singleEffectSend_ != enabled) {
        singleEffectSend_ = enabled;
        if (playMode_ == SynthPlayMode::Single) {
            notifyLiveVoiceParametersChanged();
        }
    }
}

void VF50FmEngine::setLfo2Override(const Lfo2OverrideConfig& cfg) noexcept {
    lfo2Override_ = cfg;
    refreshLfoConfigs();
}

void VF50FmEngine::setPartPan(std::size_t partIndex, int8_t panNeg64ToPos63) noexcept {
    if (partIndex < kMaxPerformanceParts) {
        const int8_t clamped = static_cast<int8_t>(std::clamp<int>(panNeg64ToPos63, -64, 63));
        if (partPans_[partIndex] != clamped) {
            partPans_[partIndex] = clamped;
            if (playMode_ == SynthPlayMode::Performance) {
                notifyLivePerformanceParametersChanged();
            }
        }
    }
}

int8_t VF50FmEngine::partPan(std::size_t partIndex) const noexcept {
    if (partIndex < kMaxPerformanceParts) {
        return partPans_[partIndex];
    }
    return 0;
}

void VF50FmEngine::setPerfEffectBank(const vf50::state::PerfEffectBank& bank) noexcept {
    perfEffects_ = bank;
    if (playMode_ == SynthPlayMode::Performance) {
        notifyLivePerformanceParametersChanged();
    }
}

void VF50FmEngine::resetAllChannelControllers() noexcept {
    for (auto& c : channelCtrl_) {
        c = ChannelControllerState{};
    }
}

void VF50FmEngine::refreshLfoConfigs() noexcept {
    if (playMode_ == SynthPlayMode::Single) {
        lfo1_.configure(activeVoice_.lfoSpeed, activeVoice_.lfoWave,
                        activeVoice_.pmd, activeVoice_.amd, activeVoice_.lfoSync);
        if (lfo2Override_.enabled) {
            lfo2_.configure(lfo2Override_.speed, lfo2Override_.wave,
                            lfo2Override_.pmd, lfo2Override_.amd, lfo2Override_.sync);
        } else {
            lfo2_.configure(activeVoice_.lfoSpeed, activeVoice_.lfoWave,
                            activeVoice_.pmd, activeVoice_.amd, activeVoice_.lfoSync);
        }
    } else {
        const auto& inst1 = activePerformance_.instruments[0];
        const auto& v1 = getVoicePatch(inst1.voiceBank, inst1.voiceNumber);
        lfo1_.configure(v1.lfoSpeed, v1.lfoWave, v1.pmd, v1.amd, v1.lfoSync);

        if (lfo2Override_.enabled) {
            lfo2_.configure(lfo2Override_.speed, lfo2Override_.wave,
                            lfo2Override_.pmd, lfo2Override_.amd, lfo2Override_.sync);
        } else {
            const auto& inst2 = activePerformance_.instruments[1];
            const auto& v2 = getVoicePatch(inst2.voiceBank, inst2.voiceNumber);
            lfo2_.configure(v2.lfoSpeed, v2.lfoWave, v2.pmd, v2.amd, v2.lfoSync);
        }
    }
}

void VF50FmEngine::notifyLiveVoiceParametersChanged() noexcept {
    refreshLfoConfigs();
    const double globalCents = effectiveGlobalTuneCents();
    for (auto& v : voices_) {
        if (!v.isActive() || v.isQuickReleasing()) {
            v.setOperatorMask(opEnableMask_);
            continue;
        }
        if (playMode_ == SynthPlayMode::Single) {
            v.updateLivePatchParameters(activeVoice_, quickEdit_, globalCents, opEnableMask_, singleEffectSend_);
        } else {
            const int pIdx = v.partIndex();
            if (pIdx >= 0 && static_cast<std::size_t>(pIdx) < kMaxPerformanceParts) {
                const auto& inst = activePerformance_.instruments[static_cast<std::size_t>(pIdx)];
                const auto& patch = getVoicePatch(inst.voiceBank, inst.voiceNumber);
                v.updateLivePatchParameters(patch, quickEdit_, globalCents, 0x0Fu, inst.effectEnable != 0u);
            }
        }
    }
}

void VF50FmEngine::notifyLivePerformanceParametersChanged() noexcept {
    refreshLfoConfigs();
    if (playMode_ != SynthPlayMode::Performance) {
        return;
    }

    const uint8_t mtTable = activePerformance_.microTuneTable;
    const uint8_t mtRoot  = activePerformance_.microTuneRootKey;
    const uint8_t pef     = activePerformance_.perfEffectSelect;
    const bool dynPan     = (pef == 2u || pef == 5u || pef == 8u || pef == 11u);
    const double globalCents = effectiveGlobalTuneCents();

    for (auto& v : voices_) {
        if (!v.isActive() || v.isQuickReleasing()) {
            continue;
        }
        const int pIdx = v.partIndex();
        if (pIdx >= 0 && static_cast<std::size_t>(pIdx) < kMaxPerformanceParts) {
            const auto uPart = static_cast<std::size_t>(pIdx);
            const auto& inst = activePerformance_.instruments[uPart];
            const auto& patch = getVoicePatch(inst.voiceBank, inst.voiceNumber);
            const double basePitchCents = computeNotePitchCents(
                v.midiNote(), inst.microTuneEnable != 0u, mtTable, mtRoot);

            v.updateLivePartParameters(inst, partPans_[uPart], basePitchCents, globalCents);
            v.updateLivePatchParameters(patch, quickEdit_, globalCents, 0x0Fu, inst.effectEnable != 0u);

            const bool panTargetsInst = (inst.outAssign == 1u || inst.outAssign == 2u);
            if (dynPan && panTargetsInst) {
                const auto& pm = perfEffects_.pans[static_cast<std::size_t>((pef - 1u) / 3u)];
                const float rangeFrac = static_cast<float>(std::min<uint8_t>(pm.panRange, 99u)) / 99.0f;
                const float dirSign = (pm.direction == 0u) ? 1.0f : -1.0f;
                if (pm.select == 1u) {
                    const float vNorm = (static_cast<float>(v.lastVelocity()) / 127.0f) * 2.0f - 1.0f;
                    v.setDynamicPanOffset(std::clamp(vNorm * dirSign * rangeFrac, -1.0f, 1.0f));
                } else if (pm.select == 2u) {
                    const float nNorm = std::clamp((static_cast<float>(v.midiNote()) - 60.0f) / 36.0f, -1.0f, 1.0f);
                    v.setDynamicPanOffset(std::clamp(nNorm * dirSign * rangeFrac, -1.0f, 1.0f));
                }
            } else {
                v.setDynamicPanOffset(0.0f);
            }
        }
    }
}

bool VF50FmEngine::isInstrumentEnabled(const vf50::generated::PerformanceInstrument& inst,
                                      uint8_t assignMode) const noexcept {
    if (inst.outAssign == 0u || inst.volume == 0u || inst.receiveChannel > 16u) {
        return false;
    }
    if (inst.limitLow > inst.limitHigh) {
        return false;
    }
    if (assignMode == 2u) {
        if (inst.limitLow == 0u && inst.limitHigh == 0u && inst.maxNotes == 0u && inst.reserveNotes == 0u) {
            return false;
        }
        return true;
    }
    return inst.maxNotes > 0u;
}

uint8_t VF50FmEngine::reservedCountForInstrument(const vf50::generated::PerformanceInstrument& inst) noexcept {
    if (inst.reserveNotes <= 1u) {
        return 0u;
    }
    return static_cast<uint8_t>(std::min<int>(16, static_cast<int>(inst.reserveNotes) - 1));
}

std::size_t VF50FmEngine::countActiveVoicesForPart(int partIndex) const noexcept {
    std::size_t count = 0u;
    for (const auto& v : voices_) {
        if (v.isActive() && !v.isQuickReleasing() && v.partIndex() == partIndex) {
            ++count;
        }
    }
    return count;
}

double VF50FmEngine::computeNotePitchCents(uint8_t midiNote, bool microTuneEnabled,
                                          uint8_t tableId, uint8_t rootKey) const noexcept {
    const uint8_t clampedNote = std::min<uint8_t>(midiNote, 127u);
    if (!microTuneEnabled || tableId == 0u) {
        return static_cast<double>(clampedNote) * 100.0;
    }

    if (tableId <= 10u) {
        const auto& tbl = runtimeRomBank().factoryMicrotunings[tableId];
        if (tableId >= 9u || rootKey == 0u) {
            return tbl.keys[clampedNote].totalCents();
        }
        const int r = static_cast<int>(rootKey % 12u);
        const int relNote = std::clamp(static_cast<int>(clampedNote) - r, 0, 127);
        return tbl.keys[static_cast<std::size_t>(relNote)].totalCents() + static_cast<double>(r) * 100.0;
    }

    if (tableId == 11u) {
        return userOctaveExpanded_[clampedNote].totalCents();
    }
    if (tableId == 12u) {
        return userFullMicrotuning_[clampedNote].totalCents();
    }
    return static_cast<double>(clampedNote) * 100.0;
}

void VF50FmEngine::noteOn(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept {
    const uint8_t ch = midiChannel & 0x0Fu;
    const uint8_t note = midiNote & 0x7Fu;
    if (velocity == 0u) {
        noteOff(ch, note);
        return;
    }
    const uint8_t cleanVel = velocity & 0x7Fu;
    lastTriggeredVelocity_ = cleanVel;
    triggerNoteInternal(ch, note, cleanVel, false, 255u, -1);
}

void VF50FmEngine::triggerNoteInternal(uint8_t midiChannel,
                                      uint8_t midiNote,
                                      uint8_t velocity,
                                      bool fromEcho,
                                      uint8_t triggerMidiNoteOverride,
                                      int8_t forcedPartIndex) noexcept {
    const uint8_t effPef = (playMode_ == SynthPlayMode::Performance)
                               ? activePerformance_.perfEffectSelect
                               : auditionPerfEffect_;
    const bool isDelayPef = (effPef == 1u || effPef == 4u || effPef == 7u || effPef == 10u);
    const bool isChordPef = (effPef == 3u || effPef == 6u || effPef == 9u || effPef == 12u);
    const std::size_t pefSlot = (effPef > 0u) ? static_cast<std::size_t>((effPef - 1u) / 3u) : 0u;

    auto scheduleDelayEcho = [&](int8_t targetPartIdx) noexcept {
        const auto& dm = perfEffects_.delays[pefSlot];
        if (dm.feedback == 0u || dm.effectLevel == 0u) {
            return;
        }
        // Manual p. 111 Note: Max 4 delayed notes concurrently; 5th cancels 1st (oldest)
        std::size_t chosenSlot = 0u;
        int activeCount = 0;
        uint64_t oldestOrder = std::numeric_limits<uint64_t>::max();
        std::size_t oldestSlot = 0u;
        std::size_t freeSlot = echoQueue_.size();
        for (std::size_t i = 0u; i < echoQueue_.size(); ++i) {
            if (echoQueue_[i].active) {
                ++activeCount;
                if (echoQueue_[i].order < oldestOrder) {
                    oldestOrder = echoQueue_[i].order;
                    oldestSlot = i;
                }
            } else if (freeSlot >= echoQueue_.size()) {
                freeSlot = i;
            }
        }
        if (activeCount >= 4 || freeSlot >= echoQueue_.size()) {
            chosenSlot = oldestSlot;
        } else {
            chosenSlot = freeSlot;
        }

        const uint32_t interval = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(std::llround(static_cast<double>(std::clamp<int>(dm.delayTimeCs, 1, 128))
                                               * 0.01 * kOpzNativeSampleRate)));
        const int scaledVel = std::clamp((static_cast<int>(velocity) * static_cast<int>(dm.effectLevel)) / 99, 1, 127);
        const int shiftedNote = static_cast<int>(midiNote) + static_cast<int>(dm.pitchShift);
        if (shiftedNote < 0 || shiftedNote > 127) {
            return;
        }
        auto& slot = echoQueue_[chosenSlot];
        slot.active          = true;
        slot.intervalSamples = interval;
        slot.samplesRemain   = interval;
        slot.midiChannel     = midiChannel;
        slot.midiNote        = static_cast<uint8_t>(shiftedNote);
        slot.triggerNote     = (triggerMidiNoteOverride <= 127u) ? triggerMidiNoteOverride : midiNote;
        slot.velocity        = static_cast<uint8_t>(scaledVel);
        slot.repeatsLeft     = dm.feedback;
        slot.effectLevel     = dm.effectLevel;
        slot.pitchShift      = dm.pitchShift;
        slot.targetPart      = targetPartIdx;
        slot.order           = echoOrderCounter_++;
    };

    if (playMode_ == SynthPlayMode::Single) {
        if (singleReceiveChannel_ < 16u && singleReceiveChannel_ != midiChannel) {
            return;
        }

        vf50::generated::PerformanceInstrument singleInst{};
        singleInst.maxNotes = (activeVoice_.monoMode != 0u) ? 1u : 16u;
        singleInst.reserveNotes = 0u;
        singleInst.voiceBank = currentVoiceUserBank_ ? 0u : 1u;
        singleInst.voiceNumber = static_cast<uint8_t>(currentVoiceIndex_);
        singleInst.receiveChannel = singleReceiveChannel_;
        singleInst.limitLow = 0u;
        singleInst.limitHigh = 127u;
        singleInst.detune = 0;
        singleInst.noteShift = 0;
        singleInst.volume = 99u;
        singleInst.outAssign = 3u; // Stereo L+R
        singleInst.lfoSelect = 1u;
        singleInst.microTuneEnable = 0u;
        singleInst.effectEnable = singleEffectSend_ ? 1u : 0u;

        lfo1_.triggerNoteOn();
        if (lfo2Override_.enabled) {
            lfo2_.triggerNoteOn();
        }

        if (!fromEcho && isChordPef) {
            const auto& chordSlots = perfEffects_.chords[pefSlot].chords[static_cast<std::size_t>(midiNote % 12u)];
            const int octShift = (static_cast<int>(midiNote) / 12 - 5) * 12;
            bool triggeredAny = false;
            for (int8_t rawN : chordSlots) {
                if (rawN < 0) continue;
                const int cn = static_cast<int>(rawN) + octShift;
                if (cn >= 0 && cn <= 127) {
                    triggeredAny = true;
                    triggerForPart(0, activeVoice_, singleInst, 0, midiChannel,
                                   static_cast<uint8_t>(cn), velocity, 0u, midiNote);
                }
            }
            if (triggeredAny) return;
        }

        if (!fromEcho && isDelayPef) {
            scheduleDelayEcho(-1);
        }

        if (activeVoice_.chorusSwitch != 0u && activeVoice_.monoMode == 0u) {
            auto leftInst = singleInst;
            leftInst.maxNotes = 8u;
            leftInst.detune = -2;
            leftInst.outAssign = 1u;
            triggerForPart(0, activeVoice_, leftInst, 0, midiChannel, midiNote, velocity, 0u, triggerMidiNoteOverride);

            auto rightInst = singleInst;
            rightInst.maxNotes = 8u;
            rightInst.detune = +2;
            rightInst.outAssign = 2u;
            triggerForPart(1, activeVoice_, rightInst, 0, midiChannel, midiNote, velocity, 0u, triggerMidiNoteOverride);
        } else {
            triggerForPart(0, activeVoice_, singleInst, 0, midiChannel, midiNote, velocity, 0u, triggerMidiNoteOverride);
        }
        return;
    }

    // Performance Mode (8 Multitimbral Parts)
    const uint8_t assignMode = activePerformance_.assignMode;

    std::array<int, kMaxPerformanceParts> matchingParts{};
    std::size_t matchCount = 0u;
    const uint8_t muteMask = perfPartMuteMask_.load(std::memory_order_acquire);

    for (std::size_t p = 0; p < kMaxPerformanceParts; ++p) {
        if (forcedPartIndex >= 0 && static_cast<int>(p) != forcedPartIndex) {
            continue;
        }
        const auto& inst = activePerformance_.instruments[p];
        if (!isInstrumentEnabled(inst, assignMode)) {
            continue;
        }
        const bool chMatch = (inst.receiveChannel == 16u) || (inst.receiveChannel == midiChannel);
        const bool rangeMatch = (midiNote >= inst.limitLow) && (midiNote <= inst.limitHigh);
        if (chMatch && rangeMatch) {
            if (((muteMask >> p) & 1u) != 0u) {
                mutedPartHitMask_.fetch_or(static_cast<uint8_t>(1u << p), std::memory_order_relaxed);
            } else {
                matchingParts[matchCount++] = static_cast<int>(p);
            }
        }
    }

    if (matchCount == 0u) {
        return;
    }

    lfo1_.triggerNoteOn();
    lfo2_.triggerNoteOn();

    // Manual p. 111 Note: In Performance Play, Delay applies only to Instrument 1
    if (!fromEcho && isDelayPef && matchingParts[0] == 0) {
        scheduleDelayEcho(0);
    }

    if (assignMode == 1u && matchCount > 1u) {
        const int chosenPart = matchingParts[alternateRoundRobin_ % matchCount];
        ++alternateRoundRobin_;
        const auto uPart = static_cast<std::size_t>(chosenPart);
        const auto& inst = activePerformance_.instruments[uPart];
        const auto& patch = getVoicePatch(inst.voiceBank, inst.voiceNumber);
        triggerForPart(chosenPart, patch, inst, partPans_[uPart], midiChannel, midiNote, velocity, assignMode, triggerMidiNoteOverride);
        return;
    }

    for (std::size_t m = 0; m < matchCount; ++m) {
        const int partIdx = matchingParts[m];
        const auto uPart = static_cast<std::size_t>(partIdx);
        const auto& inst = activePerformance_.instruments[uPart];
        const auto& patch = getVoicePatch(inst.voiceBank, inst.voiceNumber);

        // Manual p. 113 Note: In Performance Play, Chord applies only to the lowest-numbered sounding instrument
        if (!fromEcho && isChordPef && m == 0u) {
            const auto& chordSlots = perfEffects_.chords[pefSlot].chords[static_cast<std::size_t>(midiNote % 12u)];
            const int octShift = (static_cast<int>(midiNote) / 12 - 5) * 12;
            bool triggeredAny = false;
            for (int8_t rawN : chordSlots) {
                if (rawN < 0) continue;
                const int cn = static_cast<int>(rawN) + octShift;
                if (cn >= 0 && cn <= 127) {
                    triggeredAny = true;
                    triggerForPart(partIdx, patch, inst, partPans_[uPart], midiChannel,
                                   static_cast<uint8_t>(cn), velocity, assignMode, midiNote);
                }
            }
            if (triggeredAny) continue;
        }

        triggerForPart(partIdx, patch, inst, partPans_[uPart], midiChannel, midiNote, velocity, assignMode, triggerMidiNoteOverride);
    }
}

void VF50FmEngine::triggerForPart(int partIndex,
                                 const vf50::generated::FactoryVoice& patch,
                                 const vf50::generated::PerformanceInstrument& inst,
                                 int8_t finePan,
                                 uint8_t midiChannel,
                                 uint8_t midiNote,
                                 uint8_t velocity,
                                 uint8_t assignMode,
                                 uint8_t triggerMidiNoteOverride) noexcept {
    bool retriggerSameSlot = false;
    const std::size_t slot = allocateVoiceForPart(
        partIndex, midiChannel, midiNote, patch, inst, assignMode, retriggerSameSlot);
    if (slot >= kTotalVoices) {
        return;
    }

    const bool microEn = (playMode_ == SynthPlayMode::Performance)
                             ? (inst.microTuneEnable != 0u)
                             : (auditionMicroTuneMode_ != 0u);
    const uint8_t mtTable = (playMode_ == SynthPlayMode::Performance)
                                ? activePerformance_.microTuneTable
                                : ((auditionMicroTuneMode_ == 1u) ? 11u : ((auditionMicroTuneMode_ == 2u) ? 12u : 0u));
    const uint8_t mtRoot  = (playMode_ == SynthPlayMode::Performance) ? activePerformance_.microTuneRootKey : 0u;
    const double basePitchCents = computeNotePitchCents(midiNote, microEn, mtTable, mtRoot);

    double prevGlide = -1.0;
    const auto partSlot = static_cast<std::size_t>(std::clamp<int>(partIndex, 0, static_cast<int>(kMaxPerformanceParts - 1)));
    if (patch.portaSwitch != 0u) {
        const bool anyHeldOnPart = [this, partIndex]() noexcept {
            for (const auto& v : voices_) {
                if (v.isActive() && v.isKeyDown() && v.partIndex() == partIndex) {
                    return true;
                }
            }
            return false;
        }();
        if (patch.portaMode == 0u || anyHeldOnPart) {
            prevGlide = lastPartPitchCents_[partSlot];
        }
    }
    lastPartPitchCents_[partSlot] = basePitchCents;

    VoiceNoteConfig cfg{};
    cfg.partIndex       = partIndex;
    cfg.midiChannel     = midiChannel;
    cfg.midiNote        = midiNote;
    cfg.triggerMidiNote = triggerMidiNoteOverride;
    cfg.velocity        = velocity;
    cfg.partNoteShift   = inst.noteShift;
    cfg.partDetune      = inst.detune;
    cfg.partPan         = finePan;
    cfg.partVolume      = inst.volume;
    cfg.outAssign       = inst.outAssign;
    cfg.lfoSelect       = inst.lfoSelect;
    cfg.effectEnable    = inst.effectEnable != 0u;
    cfg.basePitchCents  = basePitchCents;
    cfg.prevGlideCents  = prevGlide;
    cfg.masterTuneCents = effectiveGlobalTuneCents();

    voices_[slot].setOperatorMask((playMode_ == SynthPlayMode::Single) ? opEnableMask_ : 0x0Fu);
    voices_[slot].startNote(patch, cfg, quickEdit_, allocationCounter_++, retriggerSameSlot);

    const uint8_t effPef = (playMode_ == SynthPlayMode::Performance)
                               ? activePerformance_.perfEffectSelect
                               : auditionPerfEffect_;
    const bool isPanPef = (effPef == 2u || effPef == 5u || effPef == 8u || effPef == 11u);
    const bool panTargetsInst = (playMode_ == SynthPlayMode::Single)
                             || (inst.outAssign == 1u || inst.outAssign == 2u); // Manual p. 112 Note

    if (playMode_ == SynthPlayMode::Performance) {
        if (partIndex >= 0 && partIndex < static_cast<int>(kMaxPerformanceParts)) {
            activePartHitMask_.fetch_or(static_cast<uint8_t>(1u << partIndex), std::memory_order_relaxed);
        }
    } else {
        activePartHitMask_.fetch_or(1u, std::memory_order_relaxed);
    }

    if (isPanPef && panTargetsInst) {
        const auto& pm = perfEffects_.pans[static_cast<std::size_t>((effPef - 1u) / 3u)];
        const float rangeFrac = static_cast<float>(std::min<uint8_t>(pm.panRange, 99u)) / 99.0f;
        const float dirSign = (pm.direction == 0u) ? 1.0f : -1.0f;
        if (pm.select == 1u) { // VEL
            const float vNorm = (static_cast<float>(velocity) / 127.0f) * 2.0f - 1.0f;
            voices_[slot].setDynamicPanOffset(std::clamp(vNorm * dirSign * rangeFrac, -1.0f, 1.0f));
        } else if (pm.select == 2u) { // NOTE
            const float nNorm = std::clamp((static_cast<float>(midiNote) - 60.0f) / 36.0f, -1.0f, 1.0f);
            voices_[slot].setDynamicPanOffset(std::clamp(nNorm * dirSign * rangeFrac, -1.0f, 1.0f));
        } else {
            voices_[slot].setDynamicPanOffset(0.0f);
        }
    } else {
        voices_[slot].setDynamicPanOffset(0.0f);
    }
}

std::size_t VF50FmEngine::allocateVoiceForPart(int partIndex,
                                              uint8_t midiChannel,
                                              uint8_t midiNote,
                                              const vf50::generated::FactoryVoice& patch,
                                              const vf50::generated::PerformanceInstrument& inst,
                                              uint8_t assignMode,
                                              bool& outRetriggerSameSlot) noexcept {
    outRetriggerSameSlot = false;

    // 1. STRICT MIDI NOTE-ON DEDUPLICATION
    for (std::size_t i = 0; i < kTotalVoices; ++i) {
        const auto& v = voices_[i];
        if (v.isActive() && !v.isQuickReleasing() && v.partIndex() == partIndex
            && v.midiChannel() == midiChannel && v.midiNote() == midiNote
            && (v.isKeyDown() || v.isSustained())) {
            outRetriggerSameSlot = true;
            return i;
        }
    }

    // 2. Mono Voice Mode
    if (patch.monoMode != 0u) {
        for (std::size_t i = 0; i < kTotalVoices; ++i) {
            if (voices_[i].isActive() && !voices_[i].isQuickReleasing() && voices_[i].partIndex() == partIndex) {
                outRetriggerSameSlot = true;
                return i;
            }
        }
    }

    // 3. Normal / Alternate Fixed-Cap Modes (assignMode == 0 or 1)
    if (assignMode != 2u) {
        const std::size_t maxPartVoices = std::max<std::size_t>(1u, inst.maxNotes);
        const std::size_t currentPartVoices = countActiveVoicesForPart(partIndex);

        if (currentPartVoices < maxPartVoices) {
            for (std::size_t i = 0; i < kTotalVoices; ++i) {
                if (!voices_[i].isActive()) {
                    return i;
                }
            }
        }

        std::size_t bestReleaseIdx = kTotalVoices;
        uint64_t oldestReleaseOrder = std::numeric_limits<uint64_t>::max();
        std::size_t bestHeldIdx = kTotalVoices;
        uint64_t oldestHeldOrder = std::numeric_limits<uint64_t>::max();

        for (std::size_t i = 0; i < kTotalVoices; ++i) {
            const auto& v = voices_[i];
            if (!v.isActive() || v.partIndex() != partIndex) {
                continue;
            }
            if (!v.isKeyDown()) {
                if (v.allocationOrder() < oldestReleaseOrder) {
                    oldestReleaseOrder = v.allocationOrder();
                    bestReleaseIdx = i;
                }
            } else {
                if (v.allocationOrder() < oldestHeldOrder) {
                    oldestHeldOrder = v.allocationOrder();
                    bestHeldIdx = i;
                }
            }
        }

        if (bestReleaseIdx < kTotalVoices) {
            return bestReleaseIdx;
        }
        if (bestHeldIdx < kTotalVoices) {
            return bestHeldIdx;
        }
    }

    // 4. DVA (Dynamic Voice Allocation, assignMode == 2) & global fallback
    std::array<std::size_t, kMaxPerformanceParts> partCounts{};
    std::size_t totalActive = 0u;
    for (const auto& v : voices_) {
        if (v.isActive()) {
            ++totalActive;
            if (!v.isQuickReleasing() && v.partIndex() >= 0
                && static_cast<std::size_t>(v.partIndex()) < kMaxPerformanceParts) {
                ++partCounts[static_cast<std::size_t>(v.partIndex())];
            }
        }
    }

    std::size_t unfulfilledOtherReserves = 0u;
    if (playMode_ == SynthPlayMode::Performance && assignMode == 2u) {
        for (std::size_t p = 0; p < kMaxPerformanceParts; ++p) {
            if (static_cast<int>(p) == partIndex) {
                continue;
            }
            const auto& otherInst = activePerformance_.instruments[p];
            if (!isInstrumentEnabled(otherInst, assignMode)) {
                continue;
            }
            const std::size_t req = reservedCountForInstrument(otherInst);
            if (partCounts[p] < req) {
                unfulfilledOtherReserves += (req - partCounts[p]);
            }
        }
    }

    const std::size_t freeVoices = kTotalVoices - totalActive;
    if (freeVoices > unfulfilledOtherReserves) {
        for (std::size_t i = 0; i < kTotalVoices; ++i) {
            if (!voices_[i].isActive()) {
                return i;
            }
        }
    }

    const auto canStealFromPart = [&](int victimPart) noexcept -> bool {
        if (victimPart < 0 || victimPart == partIndex || playMode_ != SynthPlayMode::Performance || assignMode != 2u) {
            return true;
        }
        if (static_cast<std::size_t>(victimPart) >= kMaxPerformanceParts) {
            return true;
        }
        const std::size_t minReserve = reservedCountForInstrument(
            activePerformance_.instruments[static_cast<std::size_t>(victimPart)]);
        return partCounts[static_cast<std::size_t>(victimPart)] > minReserve;
    };

    std::size_t bestSteal = kTotalVoices;
    int32_t bestScore = -1;
    for (std::size_t i = 0; i < kTotalVoices; ++i) {
        const auto& v = voices_[i];
        if (!v.isActive() || v.isKeyDown() || !canStealFromPart(v.partIndex())) {
            continue;
        }
        const int32_t quickBonus = v.isQuickReleasing() ? 8192 : 0;
        const int32_t score = quickBonus + static_cast<int32_t>(v.carrierEnvelopeAtten10()) * 16
                              + static_cast<int32_t>(std::min<uint64_t>(allocationCounter_ - v.allocationOrder(), 4096u));
        if (score > bestScore) {
            bestScore = score;
            bestSteal = i;
        }
    }
    if (bestSteal < kTotalVoices) {
        return bestSteal;
    }

    uint64_t oldestOrder = std::numeric_limits<uint64_t>::max();
    for (std::size_t i = 0; i < kTotalVoices; ++i) {
        const auto& v = voices_[i];
        if (!v.isActive() || !canStealFromPart(v.partIndex())) {
            continue;
        }
        if (v.allocationOrder() < oldestOrder) {
            oldestOrder = v.allocationOrder();
            bestSteal = i;
        }
    }
    if (bestSteal < kTotalVoices) {
        return bestSteal;
    }

    for (std::size_t i = 0; i < kTotalVoices; ++i) {
        if (!voices_[i].isActive()) {
            return i;
        }
    }
    return 0u;
}

void VF50FmEngine::noteOff(uint8_t midiChannel, uint8_t midiNote) noexcept {
    const uint8_t ch = midiChannel & 0x0Fu;
    const uint8_t note = midiNote & 0x7Fu;
    const bool sustain = channelCtrl_[ch].sustainPedal;

    for (auto& v : voices_) {
        if (v.isActive() && v.isKeyDown() && v.midiChannel() == ch
            && (v.midiNote() == note || v.triggerMidiNote() == note)) {
            v.releaseKey(sustain);
        }
    }
}

void VF50FmEngine::controlChange(uint8_t midiChannel, uint8_t ccNumber, uint8_t ccValue) noexcept {
    const uint8_t ch = midiChannel & 0x0Fu;
    const uint8_t cleanCc = ccNumber & 0x7Fu;
    // Allow Channel Mode / Panic (120, 121, 123) regardless of Switch; gate normal CCs by ccMode_
    const bool isChannelModeMsg = (cleanCc == 120u || cleanCc == 121u || cleanCc == 123u);
    if (!isChannelModeMsg && ccMode_ == 0u) {
        return;
    }

    const float norm = static_cast<float>(ccValue & 0x7Fu) / 127.0f;
    const bool globalMatch = (!isChannelModeMsg && ccMode_ >= 2u && static_cast<uint8_t>(ccMode_ - 2u) == ch);
    const std::size_t chStart = globalMatch ? 0u : static_cast<std::size_t>(ch);
    const std::size_t chEnd   = globalMatch ? 16u : static_cast<std::size_t>(ch + 1u);

    for (std::size_t targetCh = chStart; targetCh < chEnd; ++targetCh) {
        auto& ctrl = channelCtrl_[targetCh];
        switch (cleanCc) {
            case 1u:
                ctrl.modWheel = norm;
                break;
            case 2u:
                ctrl.breathCtrl = norm;
                break;
            case 4u:
                ctrl.footCtrl = norm;
                break;
            case 7u:
                ctrl.channelVolume = norm;
                break;
            case 10u:
                ctrl.panNorm = norm;
                break;
            case 11u:
                ctrl.expression = norm;
                break;
            case 64u: {
                const bool down = (ccValue & 0x7Fu) >= 64u;
                ctrl.sustainPedal = down;
                for (auto& v : voices_) {
                    if (v.isActive() && v.midiChannel() == static_cast<uint8_t>(targetCh)) {
                        v.setSustainPedal(down);
                    }
                }
                break;
            }
            case 120u:
                allSoundOff();
                return;
            case 121u:
                ctrl = ChannelControllerState{};
                break;
            case 123u:
                allNotesOff();
                return;
            default:
                break;
        }
    }
}

void VF50FmEngine::pitchBend(uint8_t midiChannel, int bendValue14Bit) noexcept {
    if (pbMode_ == 0u) {
        return;
    }
    const uint8_t ch = midiChannel & 0x0Fu;
    const int centered = std::clamp(bendValue14Bit, 0, 16383) - 8192;
    const float norm = (centered >= 0)
                           ? (static_cast<float>(centered) / 8191.0f)
                           : (static_cast<float>(centered) / 8192.0f);
    if (pbMode_ >= 2u && static_cast<uint8_t>(pbMode_ - 2u) == ch) {
        for (auto& ctrl : channelCtrl_) {
            ctrl.pitchBendNorm = norm;
        }
    } else {
        channelCtrl_[ch].pitchBendNorm = norm;
    }
}

void VF50FmEngine::channelPressure(uint8_t midiChannel, uint8_t pressure) noexcept {
    if (atMode_ == 0u) {
        return;
    }
    const uint8_t ch = midiChannel & 0x0Fu;
    const float norm = static_cast<float>(pressure & 0x7Fu) / 127.0f;
    if (atMode_ >= 2u && static_cast<uint8_t>(atMode_ - 2u) == ch) {
        for (auto& ctrl : channelCtrl_) {
            ctrl.aftertouch = norm;
        }
    } else {
        channelCtrl_[ch].aftertouch = norm;
    }
}

void VF50FmEngine::channelNotesOff(uint8_t midiChannel) noexcept {
    const uint8_t ch = midiChannel & 0x0Fu;
    channelCtrl_[ch].sustainPedal = false;
    for (auto& v : voices_) {
        if (v.isActive() && v.midiChannel() == ch) {
            v.setSustainPedal(false);
            v.releaseKey(false);
        }
    }
}

void VF50FmEngine::allNotesOff() noexcept {
    for (auto& c : channelCtrl_) {
        c.sustainPedal = false;
    }
    for (auto& v : voices_) {
        if (v.isActive()) {
            v.setSustainPedal(false);
            v.releaseKey(false);
        }
    }
    for (auto& e : echoQueue_) {
        e.active = false;
    }
}

void VF50FmEngine::releaseAllVoicesSmooth() noexcept {
    for (auto& v : voices_) {
        if (v.isActive()) {
            v.forceReleaseQuick();
        }
    }
    for (auto& e : echoQueue_) {
        e.active = false;
    }
}

void VF50FmEngine::allSoundOff() noexcept {
    for (auto& v : voices_) {
        v.reset();
        v.setOperatorMask(opEnableMask_);
    }
    for (auto& e : echoQueue_) {
        e.active = false;
    }
}

void VF50FmEngine::tickScheduledEchoes() noexcept {
    for (auto& slot : echoQueue_) {
        if (!slot.active) {
            continue;
        }
        if (slot.samplesRemain > 0u) {
            --slot.samplesRemain;
            continue;
        }
        triggerNoteInternal(slot.midiChannel, slot.midiNote, slot.velocity, true, slot.triggerNote, slot.targetPart);
        if (slot.repeatsLeft > 1u) {
            --slot.repeatsLeft;
            const int nextVel = std::clamp((static_cast<int>(slot.velocity) * static_cast<int>(slot.effectLevel)) / 99, 1, 127);
            const int nextNote = static_cast<int>(slot.midiNote) + static_cast<int>(slot.pitchShift);
            if (nextNote < 0 || nextNote > 127) {
                slot.active = false;
            } else {
                slot.midiNote = static_cast<uint8_t>(nextNote);
                slot.velocity = static_cast<uint8_t>(nextVel);
                slot.samplesRemain = std::max<uint32_t>(1u, slot.intervalSamples);
            }
        } else {
            slot.active = false;
        }
    }
}

FmBusFrame VF50FmEngine::renderFrame50kHz(uint16_t soloMask) noexcept {
    tickScheduledEchoes();

    if (playMode_ == SynthPlayMode::Performance) {
        const uint8_t curMute = perfPartMuteMask_.load(std::memory_order_acquire);
        const uint8_t newlyMuted = static_cast<uint8_t>(curMute & ~prevPerfPartMuteMask_);
        if (newlyMuted != 0u) {
            for (auto& v : voices_) {
                const int p = v.partIndex();
                if (v.isActive() && p >= 0 && p < static_cast<int>(kMaxPerformanceParts)
                    && (((newlyMuted >> p) & 1u) != 0u)) {
                    v.forceReleaseQuick();
                }
            }
        }
        prevPerfPartMuteMask_ = curMute;
    }

    LfoOutput outLfo1 = lfo1_.tick();
    const LfoOutput outLfo2 = lfo2_.tick();
    const LfoOutput outVib  = vibLfo_.tick();

    // When Dual LFO Override is engaged in Single Mode, sum LFO 1 + LFO 2 modulation
    if (playMode_ == SynthPlayMode::Single && lfo2Override_.enabled) {
        outLfo1.pitchBipolar = std::clamp(outLfo1.pitchBipolar + outLfo2.pitchBipolar, -1.0f, 1.0f);
        outLfo1.ampUnipolar  = std::clamp(outLfo1.ampUnipolar + outLfo2.ampUnipolar, 0.0f, 1.0f);
    }

    const uint8_t effPef = (playMode_ == SynthPlayMode::Performance)
                               ? activePerformance_.perfEffectSelect
                               : auditionPerfEffect_;
    const bool lfoPanActive = (effPef == 2u || effPef == 5u || effPef == 8u || effPef == 11u)
                           && (perfEffects_.pans[static_cast<std::size_t>((effPef - 1u) / 3u)].select == 0u);
    float lfoPanOffset = 0.0f;
    if (lfoPanActive) {
        const auto& pm = perfEffects_.pans[static_cast<std::size_t>((effPef - 1u) / 3u)];
        const float rangeFrac = static_cast<float>(std::min<uint8_t>(pm.panRange, 99u)) / 99.0f;
        const float dirSign = (pm.direction == 0u) ? 1.0f : -1.0f;
        lfoPanOffset = std::clamp(outLfo1.pitchBipolar * dirSign * rangeFrac, -1.0f, 1.0f);
    }

    FmBusFrame bus{};
    for (auto& v : voices_) {
        if (!v.isActive()) {
            continue;
        }
        v.setEgDampSpeed(egDampSpeed_);
        if (lfoPanActive) {
            bool panTargetsVoice = (playMode_ == SynthPlayMode::Single);
            if (!panTargetsVoice && v.partIndex() >= 0 && v.partIndex() < static_cast<int>(kMaxPerformanceParts)) {
                const uint8_t oa = activePerformance_.instruments[static_cast<std::size_t>(v.partIndex())].outAssign;
                panTargetsVoice = (oa == 1u || oa == 2u);
            }
            if (panTargetsVoice) {
                v.setDynamicPanOffset(lfoPanOffset);
            }
        }
        const auto& ctrl = channelCtrl_[v.midiChannel() & 0x0Fu];
        const VoiceRenderSample s = v.renderSample(outLfo1, outLfo2, outVib, ctrl);
        const int chIdx = (playMode_ == SynthPlayMode::Performance) ? v.partIndex() : 0;
        if (v.isActive()) {
            ++bus.totalActiveVoices;
            if (chIdx >= 0 && chIdx < static_cast<int>(kMaxPerformanceParts)) {
                ++bus.partActiveVoices[static_cast<std::size_t>(chIdx)];
            }
        }
        const bool soloBlocked = (soloMask != 0u)
            && (chIdx < 0 || chIdx >= static_cast<int>(kMaxPerformanceParts)
                || (((soloMask >> static_cast<unsigned>(chIdx)) & 1u) == 0u));
        if (soloBlocked) {
            continue;
        }
        if (chIdx >= 0 && chIdx < static_cast<int>(kMaxPerformanceParts)) {
            const std::size_t pIdx = static_cast<std::size_t>(chIdx);
            bus.partL[pIdx] += s.left;
            bus.partR[pIdx] += s.right;
            const float mag = std::max(std::abs(s.left), std::abs(s.right));
            auto& pk = bus.partPeak[pIdx];
            if (mag > pk) {
                pk = mag;
            }
        }
        if (s.effectEnable) {
            bus.efctDryL += s.left;
            bus.efctDryR += s.right;
            bus.wetSendL += s.left;
            bus.wetSendR += s.right;
        } else {
            bus.dryOnlyL += s.left;
            bus.dryOnlyR += s.right;
        }
    }
    return bus;
}

std::size_t VF50FmEngine::activeVoiceCount() const noexcept {
    std::size_t n = 0u;
    for (const auto& v : voices_) {
        if (v.isActive()) {
            ++n;
        }
    }
    return n;
}

std::size_t VF50FmEngine::keyDownVoiceCount() const noexcept {
    std::size_t n = 0u;
    for (const auto& v : voices_) {
        if (v.isActive() && !v.isQuickReleasing() && v.isKeyDown()) {
            ++n;
        }
    }
    return n;
}

vf50::generated::DspEffectSettings VF50FmEngine::currentPresetEffectSettings() const noexcept {
    return (playMode_ == SynthPlayMode::Single) ? activeVoice_.effect : activePerformance_.effect;
}

} // namespace vf50::dsp::fm
