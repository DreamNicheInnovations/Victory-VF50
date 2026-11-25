#include "DSP/Drums/Ym3602DrumEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vf50::dsp::drums {

namespace {

constexpr float kHalfPi = 1.5707963267948966f;

[[nodiscard]] float sampleHermite4Point(const int16_t* data, uint32_t length, double pos) noexcept {
    const int idx1 = static_cast<int>(pos);
    if (idx1 < 0 || static_cast<uint32_t>(idx1) >= length) {
        return 0.0f;
    }
    const float frac = static_cast<float>(pos - static_cast<double>(idx1));
    const auto fetch = [data, length](int i) noexcept -> float {
        const int clamped = std::clamp(i, 0, static_cast<int>(length) - 1);
        return static_cast<float>(data[clamped]) * (1.0f / 32768.0f);
    };

    const float ym1 = fetch(idx1 - 1);
    const float y0  = fetch(idx1);
    const float y1  = fetch(idx1 + 1);
    const float y2  = fetch(idx1 + 2);

    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

[[nodiscard]] inline float evalHermite4Point(float ym1, float y0, float y1, float y2, float frac) noexcept {
    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

} // namespace

Ym3602DrumEngine::Ym3602DrumEngine() noexcept
    : activeSong_(std::make_unique<vf50::generated::RhythmSongData>()) {
    resetInstrumentDefaults();
    setRhythmMasterVolume(85u);
    smoothMasterGain_ = targetMasterGain_;
    reset();
}

void Ym3602DrumEngine::setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadDefaults) noexcept {
    romBank_ = (romBank != nullptr) ? romBank : &vf50::rom::getDefaultRomImage();
    for (auto& v : voices_) {
        v = OprwVoice{};
    }
    if (reloadDefaults) {
        resetInstrumentDefaults();
    }
}

void Ym3602DrumEngine::reset() noexcept {
    for (auto& v : voices_) {
        v = OprwVoice{};
    }
    triggerCounter_ = 1u;
    patternPlaying_ = false;
    songPlaying_ = false;
    hasQueuedPattern_ = false;
    usingCustomPattern_ = false;
    outputTriggerMuted_ = false;
    slavedToSequencer_ = false;
    currentTickPos_ = 0.0;
    totalElapsedTicks_ = 0.0;
    nextEventCursor_ = 0u;
    nextClickTick_ = 0.0;
    clickEnv_ = 0.0f;
    clickPhase_ = 0.0;
    recentVoiceHitMask_.store(0u, std::memory_order_relaxed);
    hostFracPos_ = 0.0;
    smoothMasterGain_ = targetMasterGain_;
    hist50k_.fill({});
}

void Ym3602DrumEngine::resetInstrumentDefaults() noexcept {
    const auto& rom = runtimeRomBank();
    for (std::size_t i = 0; i < kNumDrums; ++i) {
        const auto& def = rom.defaultDrumSetup[i];
        instruments_[i].volume         = def.defaultVolume;
        instruments_[i].pan            = def.defaultPan;
        instruments_[i].pitchSemitones = 0.0f;
        instruments_[i].accent         = 5u;
        instruments_[i].outAssign      = 3u;
        instruments_[i].midiNote       = def.defaultMidiNote;
        instruments_[i].chokeGroup     = def.chokeGroup;
    }
    activeKeyAssignLayout_ = 0u;
    rebuildMidiNoteMap();
}

void Ym3602DrumEngine::rebuildMidiNoteMap() noexcept {
    midiNoteToDrum_.fill(-1);
    for (std::size_t i = 0; i < kNumDrums; ++i) {
        const uint8_t note = instruments_[i].midiNote & 0x7Fu;
        midiNoteToDrum_[note] = static_cast<int8_t>(i);
    }
}

void Ym3602DrumEngine::setInstrumentVolume(std::size_t drumIndex, uint8_t volume0To15) noexcept {
    if (drumIndex < kNumDrums) {
        instruments_[drumIndex].volume = std::min<uint8_t>(volume0To15, 15u);
    }
}

void Ym3602DrumEngine::setInstrumentPan(std::size_t drumIndex, int8_t panNeg3ToPos3) noexcept {
    if (drumIndex < kNumDrums) {
        instruments_[drumIndex].pan = static_cast<int8_t>(std::clamp<int>(panNeg3ToPos3, -3, 3));
    }
}

void Ym3602DrumEngine::setInstrumentPitchSemitones(std::size_t drumIndex, float semitones) noexcept {
    if (drumIndex < kNumDrums) {
        instruments_[drumIndex].pitchSemitones = std::clamp(semitones, -24.0f, 24.0f);
    }
}

void Ym3602DrumEngine::setInstrumentAccent(std::size_t drumIndex, uint8_t accent1To9) noexcept {
    if (drumIndex < kNumDrums) {
        instruments_[drumIndex].accent = std::clamp<uint8_t>(accent1To9, 1u, 9u);
    }
}

void Ym3602DrumEngine::setInstrumentOutAssign(std::size_t drumIndex, uint8_t outAssign0To3) noexcept {
    if (drumIndex < kNumDrums) {
        instruments_[drumIndex].outAssign = std::min<uint8_t>(outAssign0To3, 3u);
    }
}

void Ym3602DrumEngine::setInstrumentMidiNote(std::size_t drumIndex, uint8_t midiNote) noexcept {
    if (drumIndex < kNumDrums) {
        const uint8_t clean = midiNote & 0x7Fu;
        if (instruments_[drumIndex].midiNote != clean) {
            instruments_[drumIndex].midiNote = clean;
            rebuildMidiNoteMap();
        }
    }
}

void Ym3602DrumEngine::setInstrumentState(std::size_t drumIndex, const DrumInstrumentState& state) noexcept {
    if (drumIndex < kNumDrums) {
        DrumInstrumentState s = state;
        s.volume         = std::min<uint8_t>(s.volume, 15u);
        s.pan            = static_cast<int8_t>(std::clamp<int>(s.pan, -3, 3));
        s.pitchSemitones = std::clamp(s.pitchSemitones, -24.0f, 24.0f);
        s.accent         = std::clamp<uint8_t>(s.accent, 1u, 9u);
        s.outAssign      = std::min<uint8_t>(s.outAssign, 3u);
        s.midiNote       = s.midiNote & 0x7Fu;
        s.chokeGroup     = runtimeRomBank().defaultDrumSetup[drumIndex].chokeGroup;

        const bool noteChanged = (instruments_[drumIndex].midiNote != s.midiNote);
        instruments_[drumIndex] = s;
        if (noteChanged) {
            rebuildMidiNoteMap();
        }
    }
}

void Ym3602DrumEngine::loadPresetKeyMap(std::size_t mapIndex0To2) noexcept {
    const std::size_t mapIdx = std::min<std::size_t>(mapIndex0To2, 2u);
    activeKeyAssignLayout_ = static_cast<uint8_t>(mapIdx);
    loadCustomKeyMap(runtimeRomBank().presetDrumKeyMaps[mapIdx]);
}

void Ym3602DrumEngine::applyKeyAssignLayout(uint8_t layout0To3) noexcept {
    const uint8_t clean = std::min<uint8_t>(layout0To3, 3u);
    if (clean < 3u) {
        loadPresetKeyMap(clean);
        return;
    }
    // Layout 3: USER / SEQ (Sequential drums 1..61 mapped 1-to-1 across C1..C6 = MIDI 36..96)
    activeKeyAssignLayout_ = 3u;
    midiNoteToDrum_.fill(-1);
    for (std::size_t d = 0; d < kNumDrums; ++d) {
        const uint8_t note = static_cast<uint8_t>(36u + d);
        instruments_[d].midiNote = note;
        midiNoteToDrum_[note] = static_cast<int8_t>(d);
    }
}

void Ym3602DrumEngine::loadCustomKeyMap(const std::array<int8_t, 61>& keyToDrumMap) noexcept {
    loadCustomKeyMapAtBaseNote(keyToDrumMap, 31u);
}

void Ym3602DrumEngine::loadCustomKeyMapAtBaseNote(const std::array<int8_t, 61>& keyToDrumMap,
                                                  uint8_t baseMidiNote) noexcept {
    midiNoteToDrum_.fill(-1);
    for (std::size_t keyOffset = 0; keyOffset < keyToDrumMap.size(); ++keyOffset) {
        const int8_t drumIdx = keyToDrumMap[keyOffset];
        if (drumIdx >= 0 && static_cast<std::size_t>(drumIdx) < kNumDrums) {
            const uint8_t note = static_cast<uint8_t>((static_cast<unsigned>(baseMidiNote) + keyOffset) & 0x7Fu);
            instruments_[static_cast<std::size_t>(drumIdx)].midiNote = note;
            midiNoteToDrum_[note] = drumIdx;
        }
    }
}

const DrumInstrumentState& Ym3602DrumEngine::instrumentState(std::size_t drumIndex) const noexcept {
    return instruments_[std::min(drumIndex, kNumDrums - 1u)];
}

std::string_view Ym3602DrumEngine::instrumentName(std::size_t drumIndex) const noexcept {
    return runtimeRomBank().drumWaveforms[std::min(drumIndex, kNumDrums - 1u)].name;
}

int Ym3602DrumEngine::drumForMidiNote(uint8_t midiNote) const noexcept {
    return static_cast<int>(midiNoteToDrum_[midiNote & 0x7Fu]);
}

void Ym3602DrumEngine::setRhythmMasterVolume(uint8_t vol0To99) noexcept {
    masterVolume0To99_ = std::min<uint8_t>(vol0To99, 99u);
    const float norm = static_cast<float>(masterVolume0To99_) / 99.0f;
    targetMasterGain_ = norm * norm;
}

void Ym3602DrumEngine::adjustRhythmMasterVolumeDelta(int delta) noexcept {
    const int nextVol = std::clamp(static_cast<int>(masterVolume0To99_) + delta, 0, 99);
    setRhythmMasterVolume(static_cast<uint8_t>(nextVol));
}

void Ym3602DrumEngine::noteOn(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept {
    (void)noteOnWithResult(midiChannel, midiNote, velocity);
}

int Ym3602DrumEngine::noteOnWithResult(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept {
    if (velocity == 0u || receiveChannel_ > 16u) {
        return -1;
    }
    if (receiveChannel_ < 16u && (midiChannel & 0x0Fu) != receiveChannel_) {
        return -1;
    }
    return triggerKeyDirect(midiNote, velocity);
}

int Ym3602DrumEngine::triggerKeyDirect(uint8_t midiNote, uint8_t velocity) noexcept {
    if (velocity == 0u) {
        return -1;
    }
    const int8_t drumIdx = midiNoteToDrum_[midiNote & 0x7Fu];
    if (drumIdx >= 0) {
        triggerDrum(static_cast<std::size_t>(drumIdx), velocity);
        return static_cast<int>(drumIdx);
    }
    return -1;
}

void Ym3602DrumEngine::triggerDrumAccentLevel(std::size_t drumIndex, uint8_t vf50VelLevel1To9) noexcept {
    static constexpr std::array<uint8_t, 10> kAccentToMidiVel = {
        0, 28, 44, 60, 76, 92, 104, 114, 121, 127
    };
    const uint8_t clamped = std::clamp<uint8_t>(vf50VelLevel1To9, 1u, 9u);
    triggerDrum(drumIndex, kAccentToMidiVel[clamped]);
}

void Ym3602DrumEngine::triggerDrum(std::size_t drumIndex, uint8_t midiVelocity) noexcept {
    if (drumIndex >= kNumDrums || midiVelocity == 0u) {
        return;
    }

    const auto& inst = instruments_[drumIndex];
    if (inst.volume == 0u || inst.outAssign == 0u) {
        return;
    }

    const auto& wave = runtimeRomBank().drumWaveforms[drumIndex];
    if (wave.samples == nullptr || wave.totalSamples == 0u) {
        return;
    }

    // 1. Exclusive Group 0 Choke (Closed/Pedal/1-4 Open/Open Hi-Hats immediately choke one another)
    if (inst.chokeGroup == 0u) {
        for (auto& v : voices_) {
            if (v.active && v.chokeGroup == 0u) {
                v.chokeStep = 1.0f / 75.0f; // 1.5 ms click-free hardware hi-hat choke
            }
        }
    }

    const std::size_t slot = allocateVoice(drumIndex, inst.chokeGroup);

    auto& v = voices_[slot];
    v.active       = true;
    v.drumIndex    = drumIndex;
    v.chokeGroup   = inst.chokeGroup;
    v.triggerOrder = triggerCounter_++;
    v.phase25k     = 0.0;

    if (std::abs(inst.pitchSemitones) < 1e-4f) {
        v.stepPer50k = 0.5;
    } else {
        v.stepPer50k = 0.5 * std::exp2(static_cast<double>(inst.pitchSemitones) / 12.0);
    }

    // ROM header levelAttenCode + per-drum volume + per-drum accent (1..9, 5=0 dB) + MIDI velocity
    const float romTrim     = std::pow(10.0f, -static_cast<float>(wave.levelAttenCode & 0x07u) * (1.5f / 20.0f));
    const float volDb       = -static_cast<float>(15 - static_cast<int>(inst.volume)) * 2.0f;
    const float instVol     = std::pow(10.0f, volDb / 20.0f);
    const float accentDb    = static_cast<float>(static_cast<int>(std::clamp<uint8_t>(inst.accent, 1u, 9u)) - 5) * 1.25f;
    const float accentScale = std::pow(10.0f, accentDb / 20.0f);
    const float velNorm     = static_cast<float>(std::clamp<int>(midiVelocity, 1, 127)) / 127.0f;
    const float velGain     = 0.20f + 0.80f * std::pow(velNorm, 1.25f);

    v.gain = romTrim * instVol * accentScale * velGain * 0.65f;

    // Output Assign & Constant-power stereo pan for L3..C..R3 (-3..+3)
    if (inst.outAssign == 1u) {
        v.panL = 1.0f;
        v.panR = 0.0f;
    } else if (inst.outAssign == 2u) {
        v.panL = 0.0f;
        v.panR = 1.0f;
    } else {
        const float panNorm01 = (static_cast<float>(inst.pan) + 3.0f) / 6.0f;
        const float angle = panNorm01 * kHalfPi;
        v.panL = std::cos(angle);
        v.panR = std::sin(angle);
    }

    v.envGain = 1.0f;
    v.envDecayMul = (wave.decay2Rate >= 0x1Au) ? 0.99993f : 0.999985f;
    v.chokeGain  = 1.0f;
    v.chokeStep  = 0.0f;
    v.aaFilterZ1 = 0.0f;

    recentVoiceHitMask_.fetch_or(static_cast<uint8_t>(1u << (slot & 7u)), std::memory_order_acq_rel);
    totalTriggerCount_.fetch_add(1u, std::memory_order_relaxed);
}

std::size_t Ym3602DrumEngine::allocateVoice(std::size_t drumIndex, uint8_t chokeGroup) noexcept {
    if (chokeGroup != 255u) {
        for (std::size_t i = 0; i < kMaxVoices; ++i) {
            if (voices_[i].active && voices_[i].drumIndex == drumIndex) {
                return i;
            }
        }
    }

    for (std::size_t i = 0; i < kMaxVoices; ++i) {
        if (!voices_[i].active) {
            return i;
        }
    }

    if (chokeGroup <= 7u) {
        std::size_t groupSteal = kMaxVoices;
        uint64_t oldestOrder = std::numeric_limits<uint64_t>::max();
        for (std::size_t i = 0; i < kMaxVoices; ++i) {
            if (voices_[i].chokeGroup == chokeGroup && voices_[i].triggerOrder < oldestOrder) {
                oldestOrder = voices_[i].triggerOrder;
                groupSteal = i;
            }
        }
        if (groupSteal < kMaxVoices) {
            return groupSteal;
        }
    }

    std::size_t oldestIdx = 0u;
    uint64_t oldestOrder = std::numeric_limits<uint64_t>::max();
    for (std::size_t i = 0; i < kMaxVoices; ++i) {
        if (voices_[i].triggerOrder < oldestOrder) {
            oldestOrder = voices_[i].triggerOrder;
            oldestIdx = i;
        }
    }
    return oldestIdx;
}

void Ym3602DrumEngine::silenceActiveDrumVoices() noexcept {
    for (auto& v : voices_) {
        v.active = false;
    }
}

void Ym3602DrumEngine::allDrumsOff() noexcept {
    silenceActiveDrumVoices();
    patternPlaying_ = false;
    songPlaying_ = false;
    hasQueuedPattern_ = false;
    slavedToSequencer_ = false;
    outputTriggerMuted_ = false;
    clickEnv_ = 0.0f;
}

void Ym3602DrumEngine::startPattern(std::size_t patternIndex0To99, double tempoBpm, bool loop) noexcept {
    currentPatternIndex_ = std::min(patternIndex0To99, vf50::generated::kFactoryRhythmPtnCount - 1u);
    usingCustomPattern_ = false;
    songPlaying_ = false;
    hasQueuedPattern_ = false;
    slavedToSequencer_ = false;
    patternLoop_ = loop;
    currentTickPos_ = 0.0;
    totalElapsedTicks_ = 0.0;
    nextEventCursor_ = 0u;
    nextClickTick_ = 0.0;
    setTempoBpm(tempoBpm);
    patternPlaying_ = true;
}

void Ym3602DrumEngine::startCustomPattern(const vf50::generated::RhythmNoteEvent* events,
                                          std::size_t eventCount,
                                          uint16_t totalTicks,
                                          double tempoBpm,
                                          bool loop) noexcept {
    customEvents_ = events;
    customEventCount_ = (events != nullptr) ? eventCount : 0u;
    customTotalTicks_ = std::max<uint16_t>(totalTicks, 48u);
    customTicksPerBar_ = 192u;
    usingCustomPattern_ = true;
    songPlaying_ = false;
    hasQueuedPattern_ = false;
    slavedToSequencer_ = false;
    patternLoop_ = loop;
    currentTickPos_ = 0.0;
    totalElapsedTicks_ = 0.0;
    nextEventCursor_ = 0u;
    nextClickTick_ = 0.0;
    setTempoBpm(tempoBpm);
    patternPlaying_ = true;
}

void Ym3602DrumEngine::startOwnedCustomPattern(const vf50::generated::RhythmNoteEvent* events,
                                               std::size_t eventCount,
                                               uint16_t totalTicks,
                                               uint16_t ticksPerBar,
                                               double tempoBpm,
                                               bool loop) noexcept {
    const std::size_t safeCnt = (events != nullptr) ? std::min(eventCount, ownedCustomEvents_.size()) : 0u;
    for (std::size_t i = 0u; i < safeCnt; ++i) {
        ownedCustomEvents_[i] = events[i];
    }
    customEvents_ = ownedCustomEvents_.data();
    customEventCount_ = safeCnt;
    customTotalTicks_ = std::max<uint16_t>(totalTicks, 12u);
    customTicksPerBar_ = std::max<uint16_t>(ticksPerBar, 12u);
    usingCustomPattern_ = true;
    songPlaying_ = false;
    hasQueuedPattern_ = false;
    slavedToSequencer_ = false;
    patternLoop_ = loop;
    currentTickPos_ = 0.0;
    totalElapsedTicks_ = 0.0;
    nextEventCursor_ = 0u;
    nextClickTick_ = 0.0;
    setTempoBpm(tempoBpm);
    patternPlaying_ = true;
}

void Ym3602DrumEngine::updateActiveCustomPatternInPlace(const vf50::generated::RhythmNoteEvent* events,
                                                        std::size_t eventCount,
                                                        uint16_t totalTicks,
                                                        uint16_t ticksPerBar) noexcept {
    const std::size_t safeCnt = (events != nullptr) ? std::min(eventCount, ownedCustomEvents_.size()) : 0u;
    for (std::size_t i = 0u; i < safeCnt; ++i) {
        ownedCustomEvents_[i] = events[i];
    }
    customEvents_ = ownedCustomEvents_.data();
    customEventCount_ = safeCnt;
    customTotalTicks_ = std::max<uint16_t>(totalTicks, 12u);
    customTicksPerBar_ = std::max<uint16_t>(ticksPerBar, 12u);
    usingCustomPattern_ = true;

    nextEventCursor_ = 0u;
    while (nextEventCursor_ < customEventCount_
           && static_cast<double>(ownedCustomEvents_[nextEventCursor_].tick) <= currentTickPos_) {
        ++nextEventCursor_;
    }
}

void Ym3602DrumEngine::queueNextPattern(std::size_t factoryIdx0To99) noexcept {
    hasQueuedPattern_ = true;
    queuedIsCustom_ = false;
    queuedFactoryIdx_ = std::min(factoryIdx0To99, vf50::generated::kFactoryRhythmPtnCount - 1u);
}

void Ym3602DrumEngine::queueNextOwnedCustomPattern(const vf50::generated::RhythmNoteEvent* events,
                                                    std::size_t eventCount,
                                                    uint16_t totalTicks,
                                                    uint16_t ticksPerBar) noexcept {
    hasQueuedPattern_ = true;
    queuedIsCustom_ = true;
    queuedCustomCount_ = (events != nullptr) ? std::min(eventCount, queuedCustomEvents_.size()) : 0u;
    for (std::size_t i = 0u; i < queuedCustomCount_; ++i) {
        queuedCustomEvents_[i] = events[i];
    }
    queuedTotalTicks_ = std::max<uint16_t>(totalTicks, 12u);
    queuedTicksPerBar_ = std::max<uint16_t>(ticksPerBar, 12u);
}

void Ym3602DrumEngine::rewindPattern() noexcept {
    currentTickPos_ = 0.0;
    totalElapsedTicks_ = 0.0;
    nextEventCursor_ = 0u;
    nextClickTick_ = 0.0;
    songPartIdx_ = 0u;
    songMeasureAtPartStart_ = 1u;
    songCurrentMeasure_ = 1u;
    repeatDepth_ = 0u;
    songEffectiveVolOffset_ = 0;
    songTempoRampRemainingTicks_ = 0.0;
    songTempoRampStepPerTick_ = 0.0;
    songBarsPlayedCount_.store(0u, std::memory_order_relaxed);
    if (songPlaying_ && activeSong_ != nullptr && activeSong_->partCount > 0u) {
        setRhythmMasterVolume(songBaseVolume_);
        (void)advanceToNextPlayableSongPart();
    }
}

void Ym3602DrumEngine::stopPattern() noexcept {
    patternPlaying_ = false;
    if (songPlaying_) {
        songPlaying_ = false;
        setRhythmMasterVolume(songBaseVolume_);
    }
    hasQueuedPattern_ = false;
    slavedToSequencer_ = false;
    outputTriggerMuted_ = false;
    clickEnv_ = 0.0f;
}

void Ym3602DrumEngine::setMetronomeClick(bool enabled,
                                         uint16_t intervalTicks48Ppqn,
                                         uint16_t ticksPerBar48Ppqn) noexcept {
    clickEnabled_ = enabled;
    clickIntervalTicks_ = std::max<uint16_t>(intervalTicks48Ppqn, 6u);
    clickTicksPerBar_ = std::max<uint16_t>(ticksPerBar48Ppqn, 12u);
    if (!enabled) {
        clickEnv_ = 0.0f;
    }
}

void Ym3602DrumEngine::triggerClickSound(bool isDownbeat) noexcept {
    const double freqHz = isDownbeat ? 1680.0 : 1040.0;
    clickStep_ = (2.0 * 3.14159265358979323846 * freqHz) / kNativeBusRate;
    clickPhase_ = 0.0;
    clickEnv_ = isDownbeat ? 0.36f : 0.24f;
}

void Ym3602DrumEngine::loadSongPatternForCurrentPart() noexcept {
    if (activeSong_ == nullptr || songPartIdx_ >= activeSong_->partCount) {
        return;
    }
    const auto& part = activeSong_->parts[songPartIdx_];
    songCurrentPtnIsUser_ = part.ptnIsUser;
    songCurrentPtnIdx_ = part.ptnIndex;
    currentTickPos_ = 0.0;
    nextEventCursor_ = 0u;
    nextClickTick_ = 0.0;
    patternLoop_ = false;

    if (part.ptnIsUser) {
        const auto& uRef = songUserPatternRefs_[std::min<std::size_t>(part.ptnIndex, 99u)];
        const std::size_t safeCnt = (uRef.events != nullptr)
                                      ? std::min<std::size_t>(uRef.eventCount, ownedCustomEvents_.size())
                                      : 0u;
        for (std::size_t i = 0u; i < safeCnt; ++i) {
            ownedCustomEvents_[i] = uRef.events[i];
        }
        customEvents_ = ownedCustomEvents_.data();
        customEventCount_ = safeCnt;
        customTotalTicks_ = std::max<uint16_t>(uRef.totalTicks, 12u);
        customTicksPerBar_ = std::max<uint16_t>(uRef.ticksPerBar, 12u);
        songCurrentTicksPerBar_ = customTicksPerBar_;
        songCurrentPtnBars_ = std::max<uint8_t>(uRef.bars, 1u);
        usingCustomPattern_ = true;
    } else {
        currentPatternIndex_ = std::min<std::size_t>(part.ptnIndex, vf50::generated::kFactoryRhythmPtnCount - 1u);
        const auto& fPtn = runtimeRomBank().factoryRhythmPatterns[currentPatternIndex_];
        songCurrentTicksPerBar_ = std::max<uint16_t>(fPtn.ticksPerBar, 12u);
        songCurrentPtnBars_ = std::max<uint8_t>(fPtn.bars, 1u);
        usingCustomPattern_ = false;
    }
}

bool Ym3602DrumEngine::advanceToNextPlayableSongPart() noexcept {
    if (activeSong_ == nullptr) {
        return false;
    }
    uint16_t completedUnpairedEnd = 0xFFFFu;
    for (int guard = 0; guard < 1024; ++guard) {
        if (songPartIdx_ >= activeSong_->partCount) {
            return false;
        }
        const auto& part = activeSong_->parts[songPartIdx_];
        switch (part.type) {
            case vf50::generated::RhythmSongPartType::Pattern:
                loadSongPatternForCurrentPart();
                return true;

            case vf50::generated::RhythmSongPartType::RepeatBegin:
                if (repeatDepth_ < repeatStack_.size()) {
                    repeatStack_[repeatDepth_++] = RepeatFrame{ songPartIdx_, 0xFFFFu, 0u };
                }
                ++songPartIdx_;
                break;

            case vf50::generated::RhythmSongPartType::RepeatEnd: {
                const uint8_t targetCount = std::max<uint8_t>(part.repeatCount, 1u);
                if (repeatDepth_ > 0u) {
                    auto& top = repeatStack_[repeatDepth_ - 1u];
                    if (top.endPartIdx != songPartIdx_) {
                        top.endPartIdx = songPartIdx_;
                        top.remaining = (targetCount > 1u) ? static_cast<uint8_t>(targetCount - 1u) : 0u;
                    }
                    if (top.remaining > 0u) {
                        --top.remaining;
                        songPartIdx_ = static_cast<uint16_t>(top.startPartIdx + 1u);
                    } else {
                        --repeatDepth_;
                        ++songPartIdx_;
                    }
                } else if (targetCount > 1u && completedUnpairedEnd != songPartIdx_) {
                    if (repeatDepth_ < repeatStack_.size()) {
                        repeatStack_[repeatDepth_++] = RepeatFrame{ 0u, songPartIdx_, static_cast<uint8_t>(targetCount - 2u) };
                    }
                    songPartIdx_ = 0u;
                } else {
                    completedUnpairedEnd = songPartIdx_;
                    ++songPartIdx_;
                }
                break;
            }

            case vf50::generated::RhythmSongPartType::Volume: {
                songEffectiveVolOffset_ += static_cast<int>(part.volDelta);
                const int eff = std::clamp(static_cast<int>(songBaseVolume_) + songEffectiveVolOffset_,
                                           0, static_cast<int>(songBaseVolume_));
                setRhythmMasterVolume(static_cast<uint8_t>(eff));
                ++songPartIdx_;
                break;
            }

            case vf50::generated::RhythmSongPartType::Tempo: {
                const double delta = static_cast<double>(part.tempoDelta);
                if (part.tempoBeats == 0u) {
                    songTempoRampRemainingTicks_ = 0.0;
                    songTempoRampStepPerTick_ = 0.0;
                    setTempoBpm(tempoBpm_ + delta);
                } else {
                    songTempoRampRemainingTicks_ = static_cast<double>(part.tempoBeats) * 48.0;
                    songTempoRampStepPerTick_ = delta / songTempoRampRemainingTicks_;
                }
                ++songPartIdx_;
                break;
            }

            case vf50::generated::RhythmSongPartType::Mark:
                ++songPartIdx_;
                break;
        }
    }
    return false;
}

void Ym3602DrumEngine::startRhythmSong(const vf50::generated::RhythmSongData& song,
                                       const std::array<RhythmSongPatternRef, 100>& userPatterns,
                                       uint16_t startPartIdx,
                                       uint16_t startMeasure,
                                       double initialTempoBpm) noexcept {
    if (activeSong_ == nullptr || song.partCount == 0u) {
        return;
    }
    *activeSong_ = song;
    songUserPatternRefs_ = userPatterns;
    songBaseVolume_ = masterVolume0To99_;
    songEffectiveVolOffset_ = 0;
    songTempoRampRemainingTicks_ = 0.0;
    songTempoRampStepPerTick_ = 0.0;
    repeatDepth_ = 0u;
    songBarsPlayedCount_.store(0u, std::memory_order_relaxed);
    hasQueuedPattern_ = false;
    slavedToSequencer_ = false;
    totalElapsedTicks_ = 0.0;
    setTempoBpm(initialTempoBpm);

    songPartIdx_ = std::min<uint16_t>(startPartIdx, activeSong_->partCount);
    songMeasureAtPartStart_ = std::max<uint16_t>(startMeasure, 1u);
    songCurrentMeasure_ = songMeasureAtPartStart_;

    if (!advanceToNextPlayableSongPart()) {
        songPlaying_ = false;
        patternPlaying_ = false;
        setRhythmMasterVolume(songBaseVolume_);
        return;
    }
    songPlaying_ = true;
    patternPlaying_ = true;
}

void Ym3602DrumEngine::stopRhythmSong() noexcept {
    if (songPlaying_) {
        songPlaying_ = false;
        patternPlaying_ = false;
        setRhythmMasterVolume(songBaseVolume_);
    } else {
        patternPlaying_ = false;
    }
    hasQueuedPattern_ = false;
    slavedToSequencer_ = false;
    outputTriggerMuted_ = false;
    clickEnv_ = 0.0f;
}

void Ym3602DrumEngine::setTempoBpm(double tempoBpm) noexcept {
    tempoBpm_ = std::clamp(tempoBpm, 30.0, 300.0);
    ticksPer50kSample_ = (tempoBpm_ * static_cast<double>(vf50::generated::kRhythmTicksPerQuarter))
                         / (60.0 * kNativeBusRate);
}

void Ym3602DrumEngine::syncPatternToPpq(double ppqPosition, double tempoBpm) noexcept {
    setTempoBpm(tempoBpm);
    if (!patternPlaying_ || !std::isfinite(ppqPosition)) {
        return;
    }

    const vf50::generated::RhythmNoteEvent* evPtr = nullptr;
    std::size_t evCount = 0u;
    uint16_t totalTicks = 192u;
    if (usingCustomPattern_) {
        evPtr = customEvents_;
        evCount = customEventCount_;
        totalTicks = std::max<uint16_t>(customTotalTicks_, 48u);
    } else {
        const auto& rom = runtimeRomBank();
        const auto& ptn = rom.factoryRhythmPatterns[currentPatternIndex_];
        const auto span = rom.getRhythmPatternEvents(currentPatternIndex_);
        evPtr = span.data();
        evCount = span.size();
        totalTicks = std::max<uint16_t>(ptn.totalTicks, 48u);
    }

    const double totalD = static_cast<double>(totalTicks);
    double targetTick = std::fmod(std::max(0.0, ppqPosition * 48.0), totalD);
    if (targetTick < 0.0) {
        targetTick += totalD;
    }

    double diff = std::abs(targetTick - currentTickPos_);
    diff = std::min(diff, totalD - diff);
    if (diff > 3.0) {
        currentTickPos_ = targetTick;
        nextEventCursor_ = 0u;
        while (evPtr != nullptr && nextEventCursor_ < evCount
               && static_cast<double>(evPtr[nextEventCursor_].tick) < currentTickPos_) {
            ++nextEventCursor_;
        }
    }
}

void Ym3602DrumEngine::seekSlavedToMasterTick(double masterTick48) noexcept {
    if (!patternPlaying_) {
        return;
    }
    silenceActiveDrumVoices();
    const bool prevMuted = outputTriggerMuted_;
    const double target = std::max(0.0, masterTick48);

    auto resolveActiveMetrics = [this](const vf50::generated::RhythmNoteEvent*& evPtr,
                                       std::size_t& evCount,
                                       uint16_t& totalTicks,
                                       uint16_t& ticksPerBar) noexcept {
        if (usingCustomPattern_) {
            evPtr = customEvents_;
            evCount = customEventCount_;
            totalTicks = std::max<uint16_t>(customTotalTicks_, 12u);
            ticksPerBar = std::max<uint16_t>(customTicksPerBar_, 12u);
        } else {
            const auto& rom = runtimeRomBank();
            const auto& ptn = rom.factoryRhythmPatterns[currentPatternIndex_];
            const auto span = rom.getRhythmPatternEvents(currentPatternIndex_);
            evPtr = span.data();
            evCount = span.size();
            totalTicks = std::max<uint16_t>(ptn.totalTicks, 12u);
            ticksPerBar = std::max<uint16_t>(ptn.ticksPerBar, 12u);
        }
    };

    if (songPlaying_ && activeSong_ != nullptr && activeSong_->partCount > 0u) {
        repeatDepth_ = 0u;
        songPartIdx_ = 0u;
        songMeasureAtPartStart_ = 1u;
        songCurrentMeasure_ = 1u;
        songEffectiveVolOffset_ = 0;
        songTempoRampRemainingTicks_ = 0.0;
        songTempoRampStepPerTick_ = 0.0;
        songBarsPlayedCount_.store(0u, std::memory_order_relaxed);
        setRhythmMasterVolume(songBaseVolume_);

        if (!advanceToNextPlayableSongPart()) {
            stopRhythmSong();
            outputTriggerMuted_ = prevMuted;
            return;
        }

        double rem = target;
        for (int guard = 0; guard < 2048; ++guard) {
            const vf50::generated::RhythmNoteEvent* evPtr = nullptr;
            std::size_t evCount = 0u;
            uint16_t totalTicks = 192u;
            uint16_t ticksPerBar = 192u;
            resolveActiveMetrics(evPtr, evCount, totalTicks, ticksPerBar);

            if (rem < static_cast<double>(totalTicks)) {
                currentTickPos_ = rem;
                totalElapsedTicks_ = target;
                const uint16_t barInPtn = static_cast<uint16_t>(std::max(0.0, currentTickPos_) / static_cast<double>(ticksPerBar));
                songCurrentMeasure_ = static_cast<uint16_t>(songMeasureAtPartStart_ + barInPtn);
                nextEventCursor_ = 0u;
                while (evPtr != nullptr && nextEventCursor_ < evCount
                       && static_cast<double>(evPtr[nextEventCursor_].tick) < currentTickPos_) {
                    ++nextEventCursor_;
                }
                break;
            }

            rem -= static_cast<double>(totalTicks);
            songBarsPlayedCount_.fetch_add(static_cast<uint32_t>(std::max<uint8_t>(songCurrentPtnBars_, 1u)),
                                           std::memory_order_relaxed);
            songMeasureAtPartStart_ = static_cast<uint16_t>(songMeasureAtPartStart_ + std::max<uint8_t>(songCurrentPtnBars_, 1u));
            songCurrentMeasure_ = songMeasureAtPartStart_;
            ++songPartIdx_;
            if (!advanceToNextPlayableSongPart()) {
                stopRhythmSong();
                break;
            }
        }
    } else {
        const vf50::generated::RhythmNoteEvent* evPtr = nullptr;
        std::size_t evCount = 0u;
        uint16_t totalTicks = 192u;
        uint16_t ticksPerBar = 192u;
        resolveActiveMetrics(evPtr, evCount, totalTicks, ticksPerBar);

        const double totalD = static_cast<double>(totalTicks);
        currentTickPos_ = std::fmod(target, totalD);
        if (currentTickPos_ < 0.0) {
            currentTickPos_ += totalD;
        }
        totalElapsedTicks_ = target;
        nextEventCursor_ = 0u;
        while (evPtr != nullptr && nextEventCursor_ < evCount
               && static_cast<double>(evPtr[nextEventCursor_].tick) < currentTickPos_) {
            ++nextEventCursor_;
        }
    }

    outputTriggerMuted_ = prevMuted;
}

void Ym3602DrumEngine::advancePatternClockByDeltaTicks(double deltaTicks) noexcept {
    if (!patternPlaying_ || deltaTicks < 0.0) {
        return;
    }

    auto resolveActiveMetrics = [this](const vf50::generated::RhythmNoteEvent*& evPtr,
                                       std::size_t& evCount,
                                       uint16_t& totalTicks,
                                       uint16_t& ticksPerBar) noexcept {
        if (usingCustomPattern_) {
            evPtr = customEvents_;
            evCount = customEventCount_;
            totalTicks = std::max<uint16_t>(customTotalTicks_, 12u);
            ticksPerBar = std::max<uint16_t>(customTicksPerBar_, 12u);
        } else {
            const auto& rom = runtimeRomBank();
            const auto& ptn = rom.factoryRhythmPatterns[currentPatternIndex_];
            const auto span = rom.getRhythmPatternEvents(currentPatternIndex_);
            evPtr = span.data();
            evCount = span.size();
            totalTicks = std::max<uint16_t>(ptn.totalTicks, 12u);
            ticksPerBar = std::max<uint16_t>(ptn.ticksPerBar, 12u);
        }
    };

    const vf50::generated::RhythmNoteEvent* evPtr = nullptr;
    std::size_t evCount = 0u;
    uint16_t totalTicks = 192u;
    uint16_t ticksPerBar = 192u;
    resolveActiveMetrics(evPtr, evCount, totalTicks, ticksPerBar);

    if (clickEnabled_) {
        while (nextClickTick_ <= currentTickPos_ && nextClickTick_ < static_cast<double>(totalTicks)) {
            const uint32_t clickInt = static_cast<uint32_t>(std::lround(nextClickTick_));
            const bool isDownbeat = (ticksPerBar > 0u) ? ((clickInt % ticksPerBar) == 0u) : (clickInt == 0u);
            triggerClickSound(isDownbeat);
            nextClickTick_ += static_cast<double>(std::max<uint16_t>(clickIntervalTicks_, 6u));
        }
    }

    // Strict output-gate mute: when outputTriggerMuted_ is true, advance cursor & transport
    // silently without firing PCM drum triggers or incrementing totalTriggerCount_.
    while (evPtr != nullptr && nextEventCursor_ < evCount
           && static_cast<double>(evPtr[nextEventCursor_].tick) <= currentTickPos_) {
        const auto& ev = evPtr[nextEventCursor_];
        if (!outputTriggerMuted_) {
            triggerDrumAccentLevel(ev.drumIndex, ev.velocityLevel);
        }
        ++nextEventCursor_;
    }

    if (songPlaying_ && songTempoRampRemainingTicks_ > 0.0) {
        const double step = std::min(deltaTicks, songTempoRampRemainingTicks_);
        if (!slavedToSequencer_) {
            setTempoBpm(tempoBpm_ + songTempoRampStepPerTick_ * step);
        }
        songTempoRampRemainingTicks_ -= step;
    }

    currentTickPos_ += deltaTicks;
    totalElapsedTicks_ += deltaTicks;
    if (songPlaying_) {
        const uint16_t barInPtn = static_cast<uint16_t>(std::max(0.0, currentTickPos_ + 1e-6) / static_cast<double>(ticksPerBar));
        songCurrentMeasure_ = static_cast<uint16_t>(songMeasureAtPartStart_ + barInPtn);
    }

    int wrapGuard = 0;
    while (patternPlaying_ && (currentTickPos_ + 1e-6) >= static_cast<double>(totalTicks) && wrapGuard++ < 16) {
        const double overshoot = std::max(0.0, currentTickPos_ - static_cast<double>(totalTicks));
        if (songPlaying_) {
            songBarsPlayedCount_.fetch_add(static_cast<uint32_t>(std::max<uint8_t>(songCurrentPtnBars_, 1u)),
                                           std::memory_order_relaxed);
            songMeasureAtPartStart_ = static_cast<uint16_t>(songMeasureAtPartStart_ + std::max<uint8_t>(songCurrentPtnBars_, 1u));
            songCurrentMeasure_ = songMeasureAtPartStart_;
            ++songPartIdx_;
            if (!advanceToNextPlayableSongPart()) {
                stopRhythmSong();
                break;
            }
            // Carry fractional sub-tick overshoot across part boundaries for zero phase drift
            currentTickPos_ = overshoot;
            resolveActiveMetrics(evPtr, evCount, totalTicks, ticksPerBar);
            const uint16_t barInPtn = static_cast<uint16_t>(std::max(0.0, currentTickPos_) / static_cast<double>(ticksPerBar));
            songCurrentMeasure_ = static_cast<uint16_t>(songMeasureAtPartStart_ + barInPtn);
            while (evPtr != nullptr && nextEventCursor_ < evCount
                   && static_cast<double>(evPtr[nextEventCursor_].tick) <= currentTickPos_) {
                const auto& ev = evPtr[nextEventCursor_];
                if (!outputTriggerMuted_) {
                    triggerDrumAccentLevel(ev.drumIndex, ev.velocityLevel);
                }
                ++nextEventCursor_;
            }
        } else if (hasQueuedPattern_) {
            hasQueuedPattern_ = false;
            if (queuedIsCustom_) {
                for (std::size_t i = 0u; i < queuedCustomCount_; ++i) {
                    ownedCustomEvents_[i] = queuedCustomEvents_[i];
                }
                customEvents_ = ownedCustomEvents_.data();
                customEventCount_ = queuedCustomCount_;
                customTotalTicks_ = queuedTotalTicks_;
                customTicksPerBar_ = queuedTicksPerBar_;
                usingCustomPattern_ = true;
            } else {
                currentPatternIndex_ = queuedFactoryIdx_;
                usingCustomPattern_ = false;
            }
            currentTickPos_ = std::max(0.0, overshoot);
            nextEventCursor_ = 0u;
            nextClickTick_ = 0.0;
            patternWrapCount_.fetch_add(1u, std::memory_order_relaxed);
            resolveActiveMetrics(evPtr, evCount, totalTicks, ticksPerBar);
        } else if (patternLoop_) {
            currentTickPos_ = std::max(0.0, overshoot);
            nextEventCursor_ = 0u;
            nextClickTick_ = 0.0;
            patternWrapCount_.fetch_add(1u, std::memory_order_relaxed);
            while (evPtr != nullptr && nextEventCursor_ < evCount
                   && static_cast<double>(evPtr[nextEventCursor_].tick) <= currentTickPos_) {
                const auto& ev = evPtr[nextEventCursor_];
                if (!outputTriggerMuted_) {
                    triggerDrumAccentLevel(ev.drumIndex, ev.velocityLevel);
                }
                ++nextEventCursor_;
            }
        } else {
            patternPlaying_ = false;
        }
    }
}

void Ym3602DrumEngine::advancePatternClock50kHz() noexcept {
    if (!patternPlaying_ || slavedToSequencer_) {
        return;
    }
    advancePatternClockByDeltaTicks(ticksPer50kSample_);
}

DrumBusFrame Ym3602DrumEngine::renderFrame50kHz() noexcept {
    advancePatternClock50kHz();

    smoothMasterGain_ += 0.005f * (targetMasterGain_ - smoothMasterGain_);

    float sumL = 0.0f;
    float sumR = 0.0f;
    uint8_t activeDrums = 0u;

    for (auto& v : voices_) {
        if (!v.active) {
            continue;
        }

        const auto& wave = runtimeRomBank().drumWaveforms[v.drumIndex];
        const uint32_t intPos = static_cast<uint32_t>(v.phase25k);
        if (wave.samples == nullptr || intPos >= wave.totalSamples) {
            v.active = false;
            continue;
        }

        // Resample 25 kHz PCM rhythm ROM to 50 kHz internal bus using 4-point cubic Hermite interpolation
        float rawSample = sampleHermite4Point(wave.samples, wave.totalSamples, v.phase25k);
        const float aaAlpha = (v.stepPer50k > 0.5)
                                  ? static_cast<float>(std::clamp(0.5 / v.stepPer50k, 0.15, 0.85))
                                  : 0.82f; // Gentle ultrasonic anti-imaging above 12.5 kHz
        v.aaFilterZ1 += aaAlpha * (rawSample - v.aaFilterZ1);
        rawSample = v.aaFilterZ1;

        v.phase25k += v.stepPer50k;
        v.envGain *= v.envDecayMul;

        if (v.chokeStep > 0.0f) {
            v.chokeGain -= v.chokeStep;
            if (v.chokeGain <= 0.0f) {
                v.active = false;
                continue;
            }
        }

        float tailGuard = 1.0f;
        const uint32_t remain = wave.totalSamples - intPos;
        if (remain < 32u) {
            tailGuard = static_cast<float>(remain) * (1.0f / 32.0f);
        }

        const float mono = rawSample * v.gain * v.envGain * v.chokeGain * tailGuard * smoothMasterGain_;
        sumL += mono * v.panL;
        sumR += mono * v.panR;
        ++activeDrums;
    }

    float clickSample = 0.0f;
    if (clickEnv_ > 1e-4f) {
        clickSample = static_cast<float>(std::sin(clickPhase_)) * clickEnv_;
        clickPhase_ += clickStep_;
        clickEnv_ *= 0.992f;
    } else {
        clickEnv_ = 0.0f;
    }

    DrumBusFrame out{};
    out.rawDrumL = sumL;
    out.rawDrumR = sumR;
    out.clickSample = clickSample;
    out.activeVoices = activeDrums;
    switch (effectMode_) {
        case RhythmEffectMode::Off:
            out.dryOnlyL = sumL;
            out.dryOnlyR = sumR;
            break;
        case RhythmEffectMode::On:
            out.efctDryL = sumL;
            out.efctDryR = sumR;
            out.wetSendL = sumL;
            out.wetSendR = sumR;
            break;
        case RhythmEffectMode::Mix:
            out.dryOnlyL = sumL;
            out.dryOnlyR = sumR;
            out.wetSendL = sumL;
            out.wetSendR = sumR;
            break;
    }
    out.dryOnlyL += clickSample;
    out.dryOnlyR += clickSample;
    return out;
}

void Ym3602DrumEngine::prepareHostResampler(double hostSampleRate) noexcept {
    hostSampleRate_ = std::max(8000.0, hostSampleRate);
    hostPhaseStep_ = kNativeBusRate / hostSampleRate_;
    hostFracPos_ = 0.0;
    for (auto& f : hist50k_) {
        f = renderFrame50kHz();
    }
}

DrumBusFrame Ym3602DrumEngine::renderFrameAtHostRate() noexcept {
    while (hostFracPos_ >= 1.0) {
        hostFracPos_ -= 1.0;
        hist50k_[0] = hist50k_[1];
        hist50k_[1] = hist50k_[2];
        hist50k_[2] = hist50k_[3];
        hist50k_[3] = renderFrame50kHz();
    }
    const float t = static_cast<float>(hostFracPos_);
    hostFracPos_ += hostPhaseStep_;

    return DrumBusFrame{
        .dryOnlyL     = evalHermite4Point(hist50k_[0].dryOnlyL, hist50k_[1].dryOnlyL, hist50k_[2].dryOnlyL, hist50k_[3].dryOnlyL, t),
        .dryOnlyR     = evalHermite4Point(hist50k_[0].dryOnlyR, hist50k_[1].dryOnlyR, hist50k_[2].dryOnlyR, hist50k_[3].dryOnlyR, t),
        .efctDryL     = evalHermite4Point(hist50k_[0].efctDryL, hist50k_[1].efctDryL, hist50k_[2].efctDryL, hist50k_[3].efctDryL, t),
        .efctDryR     = evalHermite4Point(hist50k_[0].efctDryR, hist50k_[1].efctDryR, hist50k_[2].efctDryR, hist50k_[3].efctDryR, t),
        .wetSendL     = evalHermite4Point(hist50k_[0].wetSendL, hist50k_[1].wetSendL, hist50k_[2].wetSendL, hist50k_[3].wetSendL, t),
        .wetSendR     = evalHermite4Point(hist50k_[0].wetSendR, hist50k_[1].wetSendR, hist50k_[2].wetSendR, hist50k_[3].wetSendR, t),
        .rawDrumL     = evalHermite4Point(hist50k_[0].rawDrumL, hist50k_[1].rawDrumL, hist50k_[2].rawDrumL, hist50k_[3].rawDrumL, t),
        .rawDrumR     = evalHermite4Point(hist50k_[0].rawDrumR, hist50k_[1].rawDrumR, hist50k_[2].rawDrumR, hist50k_[3].rawDrumR, t),
        .clickSample  = evalHermite4Point(hist50k_[0].clickSample, hist50k_[1].clickSample, hist50k_[2].clickSample, hist50k_[3].clickSample, t),
        .activeVoices = hist50k_[3].activeVoices
    };
}

std::size_t Ym3602DrumEngine::activeVoiceCount() const noexcept {
    std::size_t n = 0u;
    for (const auto& v : voices_) {
        if (v.active) {
            ++n;
        }
    }
    return n;
}

uint8_t Ym3602DrumEngine::activeVoiceSlotMask() const noexcept {
    uint8_t mask = 0u;
    for (std::size_t i = 0u; i < kMaxVoices; ++i) {
        if (voices_[i].active) {
            mask |= static_cast<uint8_t>(1u << i);
        }
    }
    return mask;
}

} // namespace vf50::dsp::drums
