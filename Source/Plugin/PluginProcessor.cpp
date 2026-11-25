#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"
#include "ROM/VF50RomLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace vf50::plugin {

namespace {

constexpr uint8_t kStateContainerMagic[4] = { 'V', 'F', '5', 'S' };
constexpr uint8_t kUiPrefsTrailerMagic[4] = { 'V', 'F', '5', 'U' };
constexpr uint8_t kWorkflowTrailerMagic[4] = { 'V', 'F', '5', 'W' };
constexpr uint8_t kClassicPhase3TrailerMagic[4] = { 'V', 'F', '5', 'C' };
constexpr uint8_t kMixerTrailerMagic[4] = { 'V', 'F', '5', 'M' };
constexpr uint32_t kStateContainerVersion = 1u;
constexpr uint32_t kClassicTrailerVersion = 1u;
constexpr uint32_t kMixerTrailerVersion = 3u;
constexpr int kMaxSubBlockSamples         = 64;

[[nodiscard]] inline int loadInt(const std::atomic<float>* ptr, int minV, int maxV) noexcept {
    const int raw = static_cast<int>(std::lround(ptr->load(std::memory_order_relaxed)));
    return std::clamp(raw, minV, maxV);
}

[[nodiscard]] inline bool loadBool(const std::atomic<float>* ptr) noexcept {
    return ptr->load(std::memory_order_relaxed) >= 0.5f;
}

void writeU32LE(std::vector<uint8_t>& dst, uint32_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
}

[[nodiscard]] bool readU32LE(const uint8_t* src, std::size_t totalSize, std::size_t& pos, uint32_t& out) noexcept {
    if (pos > totalSize || (totalSize - pos) < 4u) return false;
    out = static_cast<uint32_t>(src[pos])
        | (static_cast<uint32_t>(src[pos + 1u]) << 8u)
        | (static_cast<uint32_t>(src[pos + 2u]) << 16u)
        | (static_cast<uint32_t>(src[pos + 3u]) << 24u);
    pos += 4u;
    return true;
}

void writeF32LE(std::vector<uint8_t>& dst, float v) {
    uint32_t bits = 0u;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32LE(dst, bits);
}

[[nodiscard]] bool readF32LE(const uint8_t* src, std::size_t totalSize, std::size_t& pos, float& out) noexcept {
    uint32_t bits = 0u;
    if (!readU32LE(src, totalSize, pos, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

struct ScopedDirtySuppress {
    std::atomic<uint32_t>& counter;
    explicit ScopedDirtySuppress(std::atomic<uint32_t>& c) noexcept : counter(c) {
        counter.fetch_add(1u, std::memory_order_acq_rel);
    }
    ~ScopedDirtySuppress() noexcept {
        counter.fetch_sub(1u, std::memory_order_acq_rel);
    }
};

} // namespace

void VF50AudioProcessor::IndexedParamListener::parameterChanged(const juce::String&, float) {
    if (owner->suppressParamDirtyCount_.load(std::memory_order_acquire) > 0u) {
        return;
    }
    owner->handleExternalParameterChange(index);
}

juce::AudioProcessor::BusesProperties VF50AudioProcessor::makeVF50BusesProperties() {
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    return BusesProperties()
        .withOutput("Main / FX Out", juce::AudioChannelSet::stereo(), true)
        .withOutput("Inst 1 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Inst 2 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Inst 3 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Inst 4 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Inst 5 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Inst 6 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Inst 7 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Inst 8 Out", juce::AudioChannelSet::stereo(), false)
        .withOutput("Rhythm Out", juce::AudioChannelSet::stereo(), false);
#else
    return BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true);
#endif
}

VF50AudioProcessor::VF50AudioProcessor()
    : AudioProcessor(makeVF50BusesProperties()),
      apvts_(*this, nullptr, "VF50State", createParameterLayout()) {
    paramBank_.bind(apvts_);

    for (auto& w : dirtyWords_) {
        w.store(0ULL, std::memory_order_relaxed);
    }

    for (std::size_t i = 0; i < vf50::params::kTotalParameterCount; ++i) {
        paramListeners_[i].owner = this;
        paramListeners_[i].index = i;
        const auto idSv = vf50::params::kParameterIds[i];
        apvts_.addParameterListener(juce::String(idSv.data(), idSv.size()), &paramListeners_[i]);
    }

    for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
        hostSeqSongs_[s] = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        hostSeqSongs_[s]->clear();
        stagingSeqBanks_[0][s] = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        stagingSeqBanks_[0][s]->clear();
        stagingSeqBanks_[1][s] = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        stagingSeqBanks_[1][s]->clear();
        hostSeqTitles_[s] = { ' ','N','e','w','S','o','n','g',' ',' ',' ',' ',' ',' ',' ',' ' };
    }
    classicSongSlots_[0] = std::make_unique<vf50::generated::RhythmSongData>();
    classicSongSlots_[1] = std::make_unique<vf50::generated::RhythmSongData>();
    hostRhythmSongs_     = std::make_unique<std::array<vf50::generated::RhythmSongData, 8>>();
    audioRhythmSongs_    = std::make_unique<std::array<vf50::generated::RhythmSongData, 8>>();
    for (auto& rs : *hostRhythmSongs_) rs.clear();
    for (auto& rs : *audioRhythmSongs_) rs.clear();
    rhythmSongsSlots_[0] = std::make_unique<std::array<vf50::generated::RhythmSongData, 8>>();
    rhythmSongsSlots_[1] = std::make_unique<std::array<vf50::generated::RhythmSongData, 8>>();
    classicRhyKitSlots_[0] = std::make_unique<vf50::state::RhythmKitBank>();
    classicRhyKitSlots_[1] = std::make_unique<vf50::state::RhythmKitBank>();

    pgmChangeTable_.resetToFactory();
    virtualCard_ = std::make_unique<vf50::state::VF50VirtualCardState>();
    {
        const auto& rom = activeRomBank();
        for (std::size_t i = 0u; i < 100u; ++i) {
            virtualCard_->banks[0].voices[i].setFromVoice(rom.factoryVoices[i]);
            virtualCard_->banks[0].performances[i].setFromPerformance(rom.factoryPerformances[i]);
            virtualCard_->banks[1].voices[i].setFromVoice(rom.factoryVoices[i]);
            virtualCard_->banks[1].performances[i].setFromPerformance(rom.factoryPerformances[i]);
        }
        virtualCard_->banks[0].pct = pgmChangeTable_;
        virtualCard_->banks[0].pefct = perfEffectBank_;
        virtualCard_->banks[0].microOct = hostMemory_.userOctaveMicrotuning();
        virtualCard_->banks[0].microFull = hostMemory_.userFullMicrotuning();
        virtualCard_->banks[1].pct = pgmChangeTable_;
        virtualCard_->banks[1].pefct = perfEffectBank_;
        virtualCard_->banks[1].microOct = hostMemory_.userOctaveMicrotuning();
        virtualCard_->banks[1].microFull = hostMemory_.userFullMicrotuning();
    }
    classicAudioSysSlots_[0] = std::make_unique<ClassicAudioSystemSnapshot>();
    classicAudioSysSlots_[1] = std::make_unique<ClassicAudioSystemSnapshot>();
    classicAudioSys_         = std::make_unique<ClassicAudioSystemSnapshot>();
    classicAudioSys_->sys   = classicSysSettings_;
    classicAudioSys_->pefct = perfEffectBank_;
    classicAudioSys_->pct   = pgmChangeTable_;
    for (std::size_t i = 0u; i < 100u; ++i) {
        classicAudioSys_->cardVoices[i] = virtualCard_->banks[0].voices[i].voice;
        classicAudioSys_->cardPerfs[i]  = virtualCard_->banks[0].performances[i].perf;
    }
    bulkSysExOutSlots_[0] = std::make_unique<vf50::midi::SysExReplyBuffer>();
    bulkSysExOutSlots_[1] = std::make_unique<vf50::midi::SysExReplyBuffer>();
    localKbdMidiBuf_.ensureSize(4096);
    mergedMidiBuf_.ensureSize(8192);

    {
        juce::String romStatus;
        if (romSetBuilder_.loadFromConfigOrUserFolder(romStatus) > 0) {
            auto loadedRom = romSetBuilder_.buildRomImage(romStatus);
            if (loadedRom != nullptr) {
                hostRomImage_ = loadedRom.get();
                romHistory_.push_back(std::move(loadedRom));
                synth_.setRuntimeRomBank(hostRomImage_, true);
                hostMemory_.setRuntimeRomBank(hostRomImage_, true);
            }
        }
    }

    syncApvtsToActiveSingleVoice(true);
    audioMemory_ = hostMemory_;
    audioMemory_.applyToSynthEngine(synth_, true);
    resetMixerConsoleToNominal(true);
    applyGlobalRuntimeParamsFromAtomics();
    clearAllDirtyFlags();
    updateHostSongMetadata(0u);
}

VF50AudioProcessor::~VF50AudioProcessor() {
    for (std::size_t i = 0; i < vf50::params::kTotalParameterCount; ++i) {
        const auto idSv = vf50::params::kParameterIds[i];
        apvts_.removeParameterListener(juce::String(idSv.data(), idSv.size()), &paramListeners_[i]);
    }
    synth_.setRuntimeRomBank(nullptr, false);
    hostMemory_.setRuntimeRomBank(nullptr, false);
    audioMemory_.setRuntimeRomBank(nullptr, false);
}

juce::AudioProcessorValueTreeState::ParameterLayout VF50AudioProcessor::createParameterLayout() {
    return vf50::params::createVF50ParameterLayout();
}

void VF50AudioProcessor::handleExternalParameterChange(std::size_t paramIndex) {
    using vf50::params::ParamIndex;

    if (paramIndex == static_cast<std::size_t>(ParamIndex::VoicePreset) ||
        paramIndex == static_cast<std::size_t>(ParamIndex::VoiceBank)) {
        const auto bank = (loadInt(paramBank_.voiceBank, 0, 1) == 0)
                              ? vf50::state::MemoryBank::FactoryRom
                              : vf50::state::MemoryBank::UserRam;
        const auto preset = static_cast<std::size_t>(loadInt(paramBank_.voicePreset, 0, 99));

        const auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm == nullptr || mm->isThisTheMessageThread()) {
            selectSinglePresetAtomic(bank, preset, false);
        } else {
            clearSingleVoiceDirtyFlags();
            const uint32_t writeSlot = (activeStagedPatchSlot_.load(std::memory_order_relaxed) + 1u) & 1u;
            auto& staged = stagedPatchSlots_[writeSlot];
            staged.bank = bank;
            staged.presetIndex = preset;
            staged.quickEdit = { 0, 0, 0, 0 };
            staged.opEnableMask = 0x0Fu;
            staged.singleEffectSend = true;
            if (bank == vf50::state::MemoryBank::FactoryRom) {
                staged.voiceSlot.overwriteCleanFromPreset(activeRomBank().factoryVoices[preset]);
                staged.useAudioUserRamLookup = false;
            } else {
                staged.useAudioUserRamLookup = true;
            }
            activeStagedPatchSlot_.store(writeSlot, std::memory_order_release);
            newPatchPending.store(true, std::memory_order_release);
        }
        return;
    }

    if (paramIndex == static_cast<std::size_t>(ParamIndex::PerfPreset) ||
        paramIndex == static_cast<std::size_t>(ParamIndex::PerfBank)) {
        const auto bank = (loadInt(paramBank_.perfBank, 0, 1) == 0)
                              ? vf50::state::MemoryBank::FactoryRom
                              : vf50::state::MemoryBank::UserRam;
        const auto preset = static_cast<std::size_t>(loadInt(paramBank_.perfPreset, 0, 99));

        const auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm == nullptr || mm->isThisTheMessageThread()) {
            selectPerformancePresetAtomic(bank, preset, false);
        } else {
            clearPerformanceDirtyFlags();
            const uint32_t writeSlot = (activeStagedPerfSlot_.load(std::memory_order_relaxed) + 1u) & 1u;
            auto& staged = stagedPerfSlots_[writeSlot];
            staged.bank = bank;
            staged.presetIndex = preset;
            if (bank == vf50::state::MemoryBank::FactoryRom) {
                staged.perfSlot.setFromPerformance(activeRomBank().factoryPerformances[preset]);
                staged.useAudioUserRamLookup = false;
            } else {
                staged.useAudioUserRamLookup = true;
            }
            activeStagedPerfSlot_.store(writeSlot, std::memory_order_release);
            newPerfPatchPending_.store(true, std::memory_order_release);
        }
        return;
    }

    markParameterDirty(paramIndex);
}

void VF50AudioProcessor::markParameterDirty(std::size_t paramIndex) noexcept {
    if (paramIndex >= vf50::params::kTotalParameterCount) return;
    const std::size_t wordIdx = paramIndex >> 6u;
    const uint64_t bitMask    = uint64_t{ 1 } << (paramIndex & 63u);
    dirtyWords_[wordIdx].fetch_or(bitMask, std::memory_order_release);
    anyParamDirty_.store(true, std::memory_order_release);
}

void VF50AudioProcessor::clearAllDirtyFlags() noexcept {
    for (auto& w : dirtyWords_) {
        w.store(0ULL, std::memory_order_relaxed);
    }
    anyParamDirty_.store(false, std::memory_order_release);
}

void VF50AudioProcessor::clearSingleVoiceDirtyFlags() noexcept {
    using vf50::params::ParamIndex;
    std::array<uint64_t, vf50::params::kDirtyBitsetWords> clearMasks{};

    auto addClearBit = [&clearMasks](std::size_t idx) noexcept {
        clearMasks[idx >> 6u] |= (uint64_t{ 1 } << (idx & 63u));
    };

    addClearBit(static_cast<std::size_t>(ParamIndex::PlayMode));
    addClearBit(static_cast<std::size_t>(ParamIndex::WorkstationMode));
    addClearBit(static_cast<std::size_t>(ParamIndex::VoiceBank));
    addClearBit(static_cast<std::size_t>(ParamIndex::VoicePreset));
    addClearBit(static_cast<std::size_t>(ParamIndex::QuickAttack));
    addClearBit(static_cast<std::size_t>(ParamIndex::QuickRelease));
    addClearBit(static_cast<std::size_t>(ParamIndex::QuickBrilliance));
    addClearBit(static_cast<std::size_t>(ParamIndex::QuickTotalLevel));

    for (std::size_t idx = vf50::params::kVoiceStart; idx < vf50::params::kVoiceEnd; ++idx) {
        addClearBit(idx);
    }
    for (std::size_t idx = vf50::params::kEffectsStart; idx < vf50::params::kEffectsEnd; ++idx) {
        if (idx != static_cast<std::size_t>(ParamIndex::EffectBypass) &&
            idx != static_cast<std::size_t>(ParamIndex::FxRhythmSendMode)) {
            addClearBit(idx);
        }
    }

    for (std::size_t w = 0; w < vf50::params::kDirtyBitsetWords; ++w) {
        if (clearMasks[w] != 0ULL) {
            dirtyWords_[w].fetch_and(~clearMasks[w], std::memory_order_acq_rel);
        }
    }
}

void VF50AudioProcessor::clearPerformanceDirtyFlags() noexcept {
    using vf50::params::ParamIndex;
    std::array<uint64_t, vf50::params::kDirtyBitsetWords> clearMasks{};

    auto addClearBit = [&clearMasks](std::size_t idx) noexcept {
        clearMasks[idx >> 6u] |= (uint64_t{ 1 } << (idx & 63u));
    };

    addClearBit(static_cast<std::size_t>(ParamIndex::PerfBank));
    addClearBit(static_cast<std::size_t>(ParamIndex::PerfPreset));
    for (std::size_t idx = vf50::params::kPerfStart; idx < vf50::params::kPerfEnd; ++idx) {
        addClearBit(idx);
    }
    for (std::size_t idx = vf50::params::kEffectsStart; idx < vf50::params::kEffectsEnd; ++idx) {
        if (idx != static_cast<std::size_t>(ParamIndex::EffectBypass) &&
            idx != static_cast<std::size_t>(ParamIndex::FxRhythmSendMode)) {
            addClearBit(idx);
        }
    }

    for (std::size_t w = 0; w < vf50::params::kDirtyBitsetWords; ++w) {
        if (clearMasks[w] != 0ULL) {
            dirtyWords_[w].fetch_and(~clearMasks[w], std::memory_order_acq_rel);
        }
    }
}

void VF50AudioProcessor::pushHostMemoryToAudio() noexcept {
    // Discard any stale pre-push audio->host snapshot or superseded single-patch stage
    pendingAudioSlot_.store(-1, std::memory_order_release);
    pendingRhythmKeyLayout_.store(-1, std::memory_order_release);
    newPatchPending.store(false, std::memory_order_release);
    newPerfPatchPending_.store(false, std::memory_order_release);

    const int slot = nextHostWriteSlot_ & 1;
    hostToAudioSlots_[static_cast<std::size_t>(slot)] = hostMemory_;
    pendingHostSlot_.store(slot, std::memory_order_release);
    nextHostWriteSlot_ ^= 1;

    activeWsMode_.store(static_cast<uint8_t>(hostMemory_.workstationMode()), std::memory_order_relaxed);
    activeVoiceBank_.store(static_cast<uint8_t>(hostMemory_.selectedVoiceBank()), std::memory_order_relaxed);
    activePerfBank_.store(static_cast<uint8_t>(hostMemory_.selectedPerfBank()), std::memory_order_relaxed);
    activeRhythmKeyLayout_.store(
        static_cast<uint8_t>(std::min<std::size_t>(hostMemory_.selectedRhythmKeyMap(), 3u)),
        std::memory_order_relaxed);
}

void VF50AudioProcessor::pullLatestAudioMemoryToHost() noexcept {
    if (newPatchPending.load(std::memory_order_acquire) ||
        newPerfPatchPending_.load(std::memory_order_acquire) ||
        pendingHostSlot_.load(std::memory_order_acquire) >= 0) {
        pendingAudioSlot_.store(-1, std::memory_order_release);
        return;
    }
    const int rxSlot = pendingAudioSlot_.exchange(-1, std::memory_order_acq_rel);
    if (rxSlot >= 0 && rxSlot < 2) {
        hostMemory_ = audioToHostSlots_[static_cast<std::size_t>(rxSlot)];
    }
}

void VF50AudioProcessor::enqueueUiMidi3(uint8_t status, uint8_t data1, uint8_t data2) noexcept {
    const uint32_t w = uiMidiWriteIdx_.load(std::memory_order_relaxed);
    const uint32_t r = uiMidiReadIdx_.load(std::memory_order_acquire);
    if ((w - r) >= static_cast<uint32_t>(kUiMidiQueueSize)) {
        return;
    }
    auto& slot = uiMidiQueue_[w & static_cast<uint32_t>(kUiMidiQueueSize - 1u)];
    slot.bytes = { status, static_cast<uint8_t>(data1 & 0x7Fu), static_cast<uint8_t>(data2 & 0x7Fu) };
    slot.len   = 3u;
    uiMidiWriteIdx_.store(w + 1u, std::memory_order_release);
}

void VF50AudioProcessor::enqueueUiPitchBendNorm(float bipolarNeg1ToPos1, uint8_t channel) noexcept {
    const float clamped = std::clamp(bipolarNeg1ToPos1, -1.0f, 1.0f);
    livePitchBendNorm_.store(clamped, std::memory_order_relaxed);
    const int bend14 = std::clamp(static_cast<int>(std::lround((clamped + 1.0f) * 8192.0f)), 0, 16383);
    const uint8_t lsb = static_cast<uint8_t>(bend14 & 0x7F);
    const uint8_t msb = static_cast<uint8_t>((bend14 >> 7) & 0x7F);
    enqueueUiMidi3(static_cast<uint8_t>(0xE0u | (channel & 0x0Fu)), lsb, msb);
}

void VF50AudioProcessor::enqueueUiModWheelNorm(float unipolar0To1, uint8_t channel) noexcept {
    const float clamped = std::clamp(unipolar0To1, 0.0f, 1.0f);
    liveModWheelNorm_.store(clamped, std::memory_order_relaxed);
    const uint8_t ccVal = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(clamped * 127.0f)), 0, 127));
    enqueueUiMidi3(static_cast<uint8_t>(0xB0u | (channel & 0x0Fu)), 1u, ccVal);
}

void VF50AudioProcessor::updateHostSongMetadata(std::size_t songIndex) noexcept {
    if (vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(songIndex)) {
        const auto* u = hostSeqSongs_[songIndex].get();
        if (u != nullptr && u->valid && u->eventCount > 0u) {
            demoTempoBpm_.store(static_cast<int>(u->initialTempoBpm), std::memory_order_relaxed);
            demoTotalMeasures_.store(std::max<int>(1, static_cast<int>(u->totalBars)), std::memory_order_relaxed);
            demoTimeSigNum_.store(std::max<int>(1, static_cast<int>(u->timeSigNum)), std::memory_order_relaxed);
            demoTimeSigDen_.store(std::max<int>(1, static_cast<int>(u->timeSigDen)), std::memory_order_relaxed);
            seqTrackDataMask_.store(
                vf50::dsp::VF50DemoSongPlayer::computeTrackDataMaskForEvents(
                    std::span<const vf50::generated::DemoSongEvent>(u->events.data(), u->eventCount),
                    u->trackChannels,
                    true),
                std::memory_order_relaxed);
        } else {
            demoTempoBpm_.store(120, std::memory_order_relaxed);
            demoTotalMeasures_.store(1, std::memory_order_relaxed);
            demoTimeSigNum_.store(4, std::memory_order_relaxed);
            demoTimeSigDen_.store(4, std::memory_order_relaxed);
            seqTrackDataMask_.store(0u, std::memory_order_relaxed);
        }
        return;
    }

    if (vf50::dsp::VF50DemoSongPlayer::isFactoryDemoSongIndex(songIndex)) {
        const auto& rom = activeRomBank();
        const std::size_t romIdx = songIndex - vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase;
        const auto& s = rom.factoryDemoSongs[romIdx];
        demoTempoBpm_.store(static_cast<int>(s.initialTempoBpm), std::memory_order_relaxed);
        demoTotalMeasures_.store(std::max<int>(1, static_cast<int>(s.totalBars)), std::memory_order_relaxed);
        demoTimeSigNum_.store(std::max<int>(1, static_cast<int>(s.timeSigNum)), std::memory_order_relaxed);
        demoTimeSigDen_.store(std::max<int>(1, static_cast<int>(s.timeSigDen)), std::memory_order_relaxed);
        seqTrackDataMask_.store(
            vf50::dsp::VF50DemoSongPlayer::computeTrackDataMaskForEvents(
                rom.getDemoSongEvents(romIdx),
                s.trackChannels,
                false),
            std::memory_order_relaxed);
    }
}

void VF50AudioProcessor::requestDemoStart(std::size_t songIndex) noexcept {
    const std::size_t clamped = std::min(songIndex, vf50::dsp::VF50DemoSongPlayer::kTotalSongSlots - 1u);
    if (vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(clamped)) {
        lastUserSeqSlot_.store(static_cast<int>(clamped), std::memory_order_relaxed);
        if (!hasUserSequenceInSlot(clamped)) {
            demoSongIdx_.store(static_cast<int>(clamped), std::memory_order_relaxed);
            updateHostSongMetadata(clamped);
            demoPlaying_.store(false, std::memory_order_relaxed);
            return;
        }
    }
    const int prevSong = demoSongIdx_.load(std::memory_order_relaxed);
    requestedDemoSongIdx_.store(clamped, std::memory_order_relaxed);
    demoSongIdx_.store(static_cast<int>(clamped), std::memory_order_relaxed);
    updateHostSongMetadata(clamped);
    if (static_cast<int>(clamped) != prevSong) {
        demoMeasure_.store(1, std::memory_order_relaxed);
    }
    demoPlaying_.store(true, std::memory_order_relaxed);
    pendingDemoCmd_.store(1u, std::memory_order_release);
}

void VF50AudioProcessor::requestDemoStop() noexcept {
    demoPlaying_.store(false, std::memory_order_relaxed);
    pendingDemoCmd_.store(2u, std::memory_order_release);
}

void VF50AudioProcessor::requestDemoToggle() noexcept {
    const bool cur = demoPlaying_.load(std::memory_order_relaxed);
    if (!cur) {
        const auto curIdx = static_cast<std::size_t>(std::max(0, demoSongIdx_.load(std::memory_order_relaxed)));
        if (vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(curIdx) && !hasUserSequenceInSlot(curIdx)) {
            return;
        }
    }
    demoPlaying_.store(!cur, std::memory_order_relaxed);
    pendingDemoCmd_.store(3u, std::memory_order_release);
}

void VF50AudioProcessor::requestDemoRewindToTop() noexcept {
    demoPlaying_.store(false, std::memory_order_relaxed);
    demoMeasure_.store(1, std::memory_order_relaxed);
    pendingDemoSeekBars_.store(0, std::memory_order_release);
    pendingDemoTargetMeasure_.store(0, std::memory_order_release);
    pendingDemoCmd_.store(5u, std::memory_order_release);
}

void VF50AudioProcessor::requestDemoSeekMeasures(int deltaMeasures) noexcept {
    if (deltaMeasures != 0) {
        pendingDemoSeekBars_.fetch_add(deltaMeasures, std::memory_order_acq_rel);
    }
}

void VF50AudioProcessor::requestDemoSeekToMeasure(int measureOneBased) noexcept {
    const int maxMeas = std::max(1, demoTotalMeasures_.load(std::memory_order_relaxed));
    const int clamped = std::clamp(measureOneBased, 1, maxMeas);
    demoMeasure_.store(clamped, std::memory_order_relaxed);
    pendingDemoSeekBars_.store(0, std::memory_order_release);
    pendingDemoTargetMeasure_.store(clamped, std::memory_order_release);
}

void VF50AudioProcessor::requestDemoSetTempoBpm(int bpm) noexcept {
    const int clamped = std::clamp(bpm, 30, 300);
    demoTempoBpm_.store(clamped, std::memory_order_relaxed);
    pendingDemoTempoBpm_.store(clamped, std::memory_order_release);
}

void VF50AudioProcessor::selectDemoSong(std::size_t songIndex) noexcept {
    const std::size_t clamped = std::min(songIndex, vf50::dsp::VF50DemoSongPlayer::kTotalSongSlots - 1u);
    if (vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(clamped)) {
        lastUserSeqSlot_.store(static_cast<int>(clamped), std::memory_order_relaxed);
    }
    const int prevSong = demoSongIdx_.load(std::memory_order_relaxed);
    requestedDemoSongIdx_.store(clamped, std::memory_order_relaxed);
    demoSongIdx_.store(static_cast<int>(clamped), std::memory_order_relaxed);
    if (static_cast<int>(clamped) != prevSong) {
        resetSequencerTrackMutes();
    }
    if (!demoPlaying_.load(std::memory_order_relaxed)) {
        updateHostSongMetadata(clamped);
        demoMeasure_.store(1, std::memory_order_relaxed);
    } else {
        updateHostSongMetadata(clamped);
    }
    pendingDemoCmd_.store(4u, std::memory_order_release);
}

void VF50AudioProcessor::publishAllUserSequencesToAudio() noexcept {
    uint8_t mask = 0u;
    const int bank = nextSeqBankWrite_ & 1;
    for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
        const auto* src = hostSeqSongs_[s].get();
        const bool pop = (src != nullptr && src->valid && src->eventCount > 0u);
        if (pop) {
            mask |= static_cast<uint8_t>(1u << s);
            hostSeqTitles_[s] = src->titleChars;
        } else {
            hostSeqTitles_[s] = { ' ','N','e','w','S','o','n','g',' ',' ',' ',' ',' ',' ',' ',' ' };
        }
        if (stagingSeqBanks_[static_cast<std::size_t>(bank)][s] != nullptr) {
            if (pop) {
                stagingSeqBanks_[static_cast<std::size_t>(bank)][s]->copyFromFast(*src);
            } else {
                stagingSeqBanks_[static_cast<std::size_t>(bank)][s]->clear();
            }
        }
    }
    userSeqPopulatedMask_.store(mask, std::memory_order_relaxed);
    pendingSeqBank_.store(bank, std::memory_order_release);
    nextSeqBankWrite_ ^= 1;

    const int activeIdx = demoSongIdx_.load(std::memory_order_relaxed);
    if (activeIdx >= 0 && vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(static_cast<std::size_t>(activeIdx))) {
        updateHostSongMetadata(static_cast<std::size_t>(activeIdx));
    }
}

void VF50AudioProcessor::publishUserSequenceToSlot(std::size_t slot0To7,
                                                  const vf50::dsp::CustomSeqSongBuffer& seq) noexcept {
    if (slot0To7 >= vf50::dsp::VF50DemoSongPlayer::kUserSongCount || hostSeqSongs_[slot0To7] == nullptr) {
        return;
    }
    hostSeqSongs_[slot0To7]->copyFromFast(seq);
    publishAllUserSequencesToAudio();
}

void VF50AudioProcessor::clearUserSequenceSlot(std::size_t slot0To7) noexcept {
    if (slot0To7 >= vf50::dsp::VF50DemoSongPlayer::kUserSongCount || hostSeqSongs_[slot0To7] == nullptr) {
        return;
    }
    hostSeqSongs_[slot0To7]->clear();
    publishAllUserSequencesToAudio();
}

void VF50AudioProcessor::clearAllUserSequences() noexcept {
    for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
        if (hostSeqSongs_[s] != nullptr) {
            hostSeqSongs_[s]->clear();
        }
    }
    publishAllUserSequencesToAudio();
}

int VF50AudioProcessor::firstPopulatedUserSeqSlot() const noexcept {
    const uint8_t mask = userSeqPopulatedMask_.load(std::memory_order_relaxed);
    for (int s = 0; s < static_cast<int>(vf50::dsp::VF50DemoSongPlayer::kUserSongCount); ++s) {
        if (((mask >> s) & 1u) != 0u) {
            return s;
        }
    }
    return -1;
}

std::string_view VF50AudioProcessor::activeDemoSongTitle() const noexcept {
    const int idx = demoSongIdx_.load(std::memory_order_relaxed);
    if (idx >= 0 && vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(static_cast<std::size_t>(idx))) {
        const auto& arr = hostSeqTitles_[static_cast<std::size_t>(idx)];
        std::size_t len = arr.size();
        while (len > 0u && (arr[len - 1u] == ' ' || arr[len - 1u] == '\0')) {
            --len;
        }
        return (len > 0u) ? std::string_view(arr.data(), len) : std::string_view("NewSong");
    }
    if (idx >= 0 && vf50::dsp::VF50DemoSongPlayer::isFactoryDemoSongIndex(static_cast<std::size_t>(idx))) {
        const std::size_t romIdx = static_cast<std::size_t>(idx) - vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase;
        return activeRomBank().factoryDemoSongs[romIdx].title;
    }
    return "NewSong";
}

int VF50AudioProcessor::loadRomFiles(const juce::StringArray& paths, juce::String& outStatus, bool persistConfig) {
    int loaded = 0;
    juce::String firstMsg;
    for (const auto& path : paths) {
        const juce::File f(path);
        juce::String msg;
        if (f.isDirectory()) {
            loaded += romSetBuilder_.scanDirectory(f, msg);
        } else if (romSetBuilder_.addRomFile(f, msg)) {
            ++loaded;
        }
        if (firstMsg.isEmpty() && msg.isNotEmpty()) {
            firstMsg = msg;
        }
    }

    if (loaded > 0) {
        juce::String summary;
        auto newRom = romSetBuilder_.buildRomImage(summary);
        if (newRom != nullptr) {
            installRuntimeRomBank(std::move(newRom), true);
        }
        if (persistConfig) {
            romSetBuilder_.saveConfiguredPaths();
        }
        outStatus = summary;
    } else {
        outStatus = firstMsg.isNotEmpty() ? firstMsg : juce::String("No valid VF50 ROM images matched");
    }
    return loaded;
}

int VF50AudioProcessor::loadRomFolder(const juce::File& folder, juce::String& outStatus, bool persistConfig) {
    const int loaded = romSetBuilder_.scanDirectory(folder, outStatus);
    if (loaded > 0) {
        juce::String summary;
        auto newRom = romSetBuilder_.buildRomImage(summary);
        if (newRom != nullptr) {
            installRuntimeRomBank(std::move(newRom), true);
        }
        if (persistConfig) {
            romSetBuilder_.saveConfiguredPaths();
        }
        outStatus = summary;
    }
    return loaded;
}

void VF50AudioProcessor::clearExternalRoms() {
    romSetBuilder_.clear();
    vf50::rom::VF50RomSetBuilder::clearConfiguredPaths();
    const vf50::rom::VF50RomImage* defPtr = &vf50::rom::getDefaultRomImage();
    hostRomImage_ = defPtr;
    hostMemory_.setRuntimeRomBank(defPtr, true);
    syncApvtsToActiveSingleVoice(true);
    pendingRomReloadDefaults_.store(true, std::memory_order_release);
    pendingRomImagePtr_.store(defPtr, std::memory_order_release);
    pushHostMemoryToAudio();
    romEpoch_.fetch_add(1u, std::memory_order_release);
    patchSyncEpoch_.fetch_add(1u, std::memory_order_release);
}

void VF50AudioProcessor::installRuntimeRomBank(std::unique_ptr<vf50::rom::VF50RomImage> newRom,
                                              bool reloadFactoryDefaults) {
    if (newRom == nullptr) {
        return;
    }
    const vf50::rom::VF50RomImage* ptr = newRom.get();
    romHistory_.push_back(std::move(newRom));
    hostRomImage_ = ptr;
    hostMemory_.setRuntimeRomBank(ptr, reloadFactoryDefaults);
    if (reloadFactoryDefaults) {
        syncApvtsToActiveSingleVoice(true);
        resetMixerConsoleToNominal(true);
    }
    pendingRomReloadDefaults_.store(reloadFactoryDefaults, std::memory_order_release);
    pendingRomImagePtr_.store(ptr, std::memory_order_release);
    pushHostMemoryToAudio();
    romEpoch_.fetch_add(1u, std::memory_order_release);
    patchSyncEpoch_.fetch_add(1u, std::memory_order_release);
}

void VF50AudioProcessor::setGlobalTransposeSemitones(int semitones) noexcept {
    globalTransposeSemitones_.store(std::clamp(semitones, -24, 24), std::memory_order_relaxed);
}

void VF50AudioProcessor::setRhythmKeyAssignLayout(int layout0To3) noexcept {
    const int clamped = std::clamp(layout0To3, 0, 3);
    pullLatestAudioMemoryToHost();
    hostMemory_.selectRhythmKeyMap(static_cast<std::size_t>(clamped));
    activeRhythmKeyLayout_.store(static_cast<uint8_t>(clamped), std::memory_order_relaxed);
    pendingRhythmKeyLayout_.store(clamped, std::memory_order_release);
}

void VF50AudioProcessor::publishClassicSystemStateToAudio() noexcept {
    const int slot = nextClassicSysWriteSlot_ & 1;
    if (classicAudioSysSlots_[static_cast<std::size_t>(slot)] != nullptr) {
        auto& snap = *classicAudioSysSlots_[static_cast<std::size_t>(slot)];
        snap.sys   = classicSysSettings_;
        snap.pefct = perfEffectBank_;
        snap.pct   = pgmChangeTable_;
        if (virtualCard_ != nullptr) {
            const auto& cb = virtualCard_->activeBank();
            for (std::size_t i = 0u; i < 100u; ++i) {
                snap.cardVoices[i] = cb.voices[i].voice;
                snap.cardPerfs[i]  = cb.performances[i].perf;
            }
        }
        pendingClassicSysSlot_.store(slot, std::memory_order_release);
        nextClassicSysWriteSlot_ ^= 1;
    }
}

void VF50AudioProcessor::enqueueMidiOutShort(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t len) noexcept {
    const uint32_t w = midiOutWriteIdx_.load(std::memory_order_relaxed);
    const uint32_t r = midiOutReadIdx_.load(std::memory_order_acquire);
    if ((w - r) >= static_cast<uint32_t>(kMidiOutQueueSize)) {
        return;
    }
    auto& slot = midiOutQueue_[w & static_cast<uint32_t>(kMidiOutQueueSize - 1u)];
    slot.bytes = { b0, static_cast<uint8_t>(b1 & 0x7Fu), static_cast<uint8_t>(b2 & 0x7Fu) };
    slot.len   = std::clamp<uint8_t>(len, 1u, 3u);
    midiOutWriteIdx_.store(w + 1u, std::memory_order_release);
}

bool VF50AudioProcessor::enqueueBulkSysEx(const vf50::midi::SysExReplyBuffer& buf) noexcept {
    if (buf.packetCount == 0u) return false;
    const int slot = nextBulkSysExWriteSlot_ & 1;
    if (bulkSysExOutSlots_[static_cast<std::size_t>(slot)] == nullptr) return false;
    *bulkSysExOutSlots_[static_cast<std::size_t>(slot)] = buf;
    pendingBulkSysExSlot_.store(slot, std::memory_order_release);
    nextBulkSysExWriteSlot_ ^= 1;
    return true;
}

bool VF50AudioProcessor::pushClassicRhythmCmd(const ClassicRhythmCmd& cmd) noexcept {
    const uint32_t w = classicRhyCmdWrite_.load(std::memory_order_relaxed);
    const uint32_t r = classicRhyCmdRead_.load(std::memory_order_acquire);
    if ((w - r) >= static_cast<uint32_t>(kClassicRhyCmdQueueSize)) {
        return false;
    }
    classicRhyCmdQueue_[w & static_cast<uint32_t>(kClassicRhyCmdQueueSize - 1u)] = cmd;
    classicRhyCmdWrite_.store(w + 1u, std::memory_order_release);
    return true;
}

void VF50AudioProcessor::requestClassicRhythmStartFactory(std::size_t ptnIdx0To99, double tempoBpm, bool loop) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::StartFactoryPattern;
    cmd.index = static_cast<uint8_t>(std::min<std::size_t>(ptnIdx0To99, 99u));
    cmd.flag = loop;
    cmd.tempoBpm = std::clamp(tempoBpm, 30.0, 240.0);
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmStartUser(std::size_t ptnIdx0To99,
                                                      const vf50::state::UserRhythmPattern& ptn,
                                                      double tempoBpm,
                                                      bool loop) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::StartUserPattern;
    cmd.index = static_cast<uint8_t>(std::min<std::size_t>(ptnIdx0To99, 99u));
    cmd.flag = loop;
    cmd.tempoBpm = std::clamp(tempoBpm, 30.0, 240.0);
    cmd.pattern = ptn;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmQueueFactory(std::size_t ptnIdx0To99) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::QueueFactoryPattern;
    cmd.index = static_cast<uint8_t>(std::min<std::size_t>(ptnIdx0To99, 99u));
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmQueueUser(std::size_t ptnIdx0To99,
                                                      const vf50::state::UserRhythmPattern& ptn) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::QueueUserPattern;
    cmd.index = static_cast<uint8_t>(std::min<std::size_t>(ptnIdx0To99, 99u));
    cmd.pattern = ptn;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmUpdateUserInPlace(std::size_t ptnIdx0To99,
                                                              const vf50::state::UserRhythmPattern& ptn) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::UpdateUserPatternInPlace;
    cmd.index = static_cast<uint8_t>(std::min<std::size_t>(ptnIdx0To99, 99u));
    cmd.pattern = ptn;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmCommitUserPattern(std::size_t ptnIdx0To99,
                                                              const vf50::state::UserRhythmPattern& ptn) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::CommitUserPatternOnly;
    cmd.index = static_cast<uint8_t>(std::min<std::size_t>(ptnIdx0To99, 99u));
    cmd.pattern = ptn;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmCommitKit(const vf50::state::RhythmKitBank& kit) noexcept {
    const int slot = nextClassicRhyKitWriteSlot_ & 1;
    if (classicRhyKitSlots_[static_cast<std::size_t>(slot)] != nullptr) {
        *classicRhyKitSlots_[static_cast<std::size_t>(slot)] = kit;
        pendingClassicRhyKitSlot_.store(slot, std::memory_order_release);
        nextClassicRhyKitWriteSlot_ ^= 1;
    }
}

void VF50AudioProcessor::requestClassicRhythmStop() noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::StopAll;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmRewind() noexcept {
    rhyCurrentTick_.store(0, std::memory_order_relaxed);
    rhySongMeas_.store(1u, std::memory_order_relaxed);
    rhySongPart_.store(1u, std::memory_order_relaxed);
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::RewindPattern;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmSetTempo(double tempoBpm) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::SetTempo;
    cmd.tempoBpm = std::clamp(tempoBpm, 30.0, 240.0);
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmSetMetronome(bool enabled,
                                                        uint16_t intervalTicks,
                                                        uint16_t ticksPerBar) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::SetMetronome;
    cmd.flag = enabled;
    cmd.u16A = intervalTicks;
    cmd.u16B = ticksPerBar;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmApplyKeyMap(const std::array<int8_t, 61>& map,
                                                        uint8_t baseMidiNote) noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::ApplyKeyMap;
    cmd.index = baseMidiNote;
    cmd.keyMap = map;
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::requestClassicRhythmStartSong(const vf50::generated::RhythmSongData& song,
                                                      uint16_t startPartIdx,
                                                      uint16_t startMeasure,
                                                      double tempoBpm) noexcept {
    const int slot = nextClassicSongWriteSlot_ & 1;
    if (classicSongSlots_[static_cast<std::size_t>(slot)] != nullptr) {
        *classicSongSlots_[static_cast<std::size_t>(slot)] = song;
        pendingClassicSongSlot_.store(slot, std::memory_order_release);
        nextClassicSongWriteSlot_ ^= 1;
    }
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::StartSong;
    cmd.u16A = startPartIdx;
    cmd.u16B = startMeasure;
    cmd.tempoBpm = std::clamp(tempoBpm, 30.0, 300.0);
    (void)pushClassicRhythmCmd(cmd);
}

void VF50AudioProcessor::publishRhythmSongsToAudio() noexcept {
    const int slot = nextRhythmSongsWriteSlot_ & 1;
    if (hostRhythmSongs_ != nullptr && rhythmSongsSlots_[static_cast<std::size_t>(slot)] != nullptr) {
        *rhythmSongsSlots_[static_cast<std::size_t>(slot)] = *hostRhythmSongs_;
        pendingRhythmSongsSlot_.store(slot, std::memory_order_release);
        nextRhythmSongsWriteSlot_ ^= 1;
    }
}

void VF50AudioProcessor::requestClassicRhythmStopSong() noexcept {
    ClassicRhythmCmd cmd{};
    cmd.type = ClassicRhythmCmd::Type::StopSong;
    (void)pushClassicRhythmCmd(cmd);
}

std::size_t VF50AudioProcessor::consumeRhythmUiNoteEvents(RhythmUiNoteEvent* out,
                                                         std::size_t maxCount) noexcept {
    if (out == nullptr || maxCount == 0u) {
        return 0u;
    }
    uint32_t r = rhyUiNoteRead_.load(std::memory_order_relaxed);
    const uint32_t w = rhyUiNoteWrite_.load(std::memory_order_acquire);
    std::size_t count = 0u;
    while (r != w && count < maxCount) {
        out[count++] = rhyUiNoteQueue_[r & static_cast<uint32_t>(kRhythmUiNoteQueueSize - 1u)];
        ++r;
    }
    rhyUiNoteRead_.store(r, std::memory_order_release);
    return count;
}

void VF50AudioProcessor::processClassicRhythmCmdsRealTime() noexcept {
    const int rhySongsSlot = pendingRhythmSongsSlot_.exchange(-1, std::memory_order_acq_rel);
    if (rhySongsSlot >= 0 && rhySongsSlot < 2
        && rhythmSongsSlots_[static_cast<std::size_t>(rhySongsSlot)] != nullptr
        && audioRhythmSongs_ != nullptr) {
        *audioRhythmSongs_ = *rhythmSongsSlots_[static_cast<std::size_t>(rhySongsSlot)];
    }

    const int kitSlot = pendingClassicRhyKitSlot_.exchange(-1, std::memory_order_acq_rel);
    if (kitSlot >= 0 && kitSlot < 2 && classicRhyKitSlots_[static_cast<std::size_t>(kitSlot)] != nullptr) {
        const auto& kit = *classicRhyKitSlots_[static_cast<std::size_t>(kitSlot)];
        audioMemory_.mutableActiveRhythmKit() = kit;
        for (std::size_t d = 0u; d < vf50::generated::kDrumInstrumentCount; ++d) {
            synth_.drums().setInstrumentState(d, kit.instruments[d]);
        }
        synth_.drums().setRhythmMasterVolume(kit.rhythmVolume);
        synth_.drums().setEffectMode(kit.effectMode);
    }

    uint32_t r = classicRhyCmdRead_.load(std::memory_order_relaxed);
    const uint32_t w = classicRhyCmdWrite_.load(std::memory_order_acquire);
    while (r != w) {
        const auto& cmd = classicRhyCmdQueue_[r & static_cast<uint32_t>(kClassicRhyCmdQueueSize - 1u)];
        ++r;
        switch (cmd.type) {
            case ClassicRhythmCmd::Type::None:
                break;
            case ClassicRhythmCmd::Type::StartFactoryPattern:
                synth_.setDrumsEnabled(true);
                audioMemory_.selectRhythmPattern(vf50::state::MemoryBank::FactoryRom, cmd.index);
                synth_.drums().startPattern(cmd.index, cmd.tempoBpm, cmd.flag);
                break;
            case ClassicRhythmCmd::Type::StartUserPattern: {
                synth_.setDrumsEnabled(true);
                audioMemory_.mutableUserRhythmPattern(cmd.index) = cmd.pattern;
                audioMemory_.selectRhythmPattern(vf50::state::MemoryBank::UserRam, cmd.index);
                const auto& uPtn = audioMemory_.userRhythmPattern(cmd.index);
                const uint16_t tpb = static_cast<uint16_t>(std::max(12, static_cast<int>(uPtn.totalTicks) / std::max<int>(1, uPtn.bars)));
                synth_.drums().startOwnedCustomPattern(uPtn.events.data(), uPtn.eventCount, uPtn.totalTicks, tpb, cmd.tempoBpm, cmd.flag);
                break;
            }
            case ClassicRhythmCmd::Type::QueueFactoryPattern:
                synth_.drums().queueNextPattern(cmd.index);
                break;
            case ClassicRhythmCmd::Type::QueueUserPattern: {
                audioMemory_.mutableUserRhythmPattern(cmd.index) = cmd.pattern;
                const auto& uPtn = audioMemory_.userRhythmPattern(cmd.index);
                const uint16_t tpb = static_cast<uint16_t>(std::max(12, static_cast<int>(uPtn.totalTicks) / std::max<int>(1, uPtn.bars)));
                synth_.drums().queueNextOwnedCustomPattern(uPtn.events.data(), uPtn.eventCount, uPtn.totalTicks, tpb);
                break;
            }
            case ClassicRhythmCmd::Type::UpdateUserPatternInPlace: {
                audioMemory_.mutableUserRhythmPattern(cmd.index) = cmd.pattern;
                const auto& uPtn = audioMemory_.userRhythmPattern(cmd.index);
                const uint16_t tpb = static_cast<uint16_t>(std::max(12, static_cast<int>(uPtn.totalTicks) / std::max<int>(1, uPtn.bars)));
                synth_.drums().updateActiveCustomPatternInPlace(uPtn.events.data(), uPtn.eventCount, uPtn.totalTicks, tpb);
                break;
            }
            case ClassicRhythmCmd::Type::CommitUserPatternOnly:
                audioMemory_.mutableUserRhythmPattern(cmd.index) = cmd.pattern;
                break;
            case ClassicRhythmCmd::Type::StopAll:
                synth_.drums().stopRhythmSong();
                synth_.drums().stopPattern();
                break;
            case ClassicRhythmCmd::Type::RewindPattern:
                synth_.drums().rewindPattern();
                break;
            case ClassicRhythmCmd::Type::StartSong: {
                const int songSlot = pendingClassicSongSlot_.exchange(-1, std::memory_order_acq_rel);
                if (songSlot >= 0 && songSlot < 2 && classicSongSlots_[static_cast<std::size_t>(songSlot)] != nullptr) {
                    synth_.setDrumsEnabled(true);
                    std::array<vf50::dsp::drums::Ym3602DrumEngine::RhythmSongPatternRef, 100> refs{};
                    for (std::size_t p = 0u; p < 100u; ++p) {
                        const auto& uPtn = audioMemory_.userRhythmPattern(p);
                        refs[p].events = uPtn.events.data();
                        refs[p].eventCount = uPtn.eventCount;
                        refs[p].totalTicks = uPtn.totalTicks;
                        refs[p].bars = std::max<uint8_t>(uPtn.bars, 1u);
                        refs[p].ticksPerBar = static_cast<uint16_t>(std::max(12, static_cast<int>(uPtn.totalTicks) / std::max<int>(1, uPtn.bars)));
                    }
                    synth_.drums().startRhythmSong(*classicSongSlots_[static_cast<std::size_t>(songSlot)],
                                                   refs, cmd.u16A, cmd.u16B, cmd.tempoBpm);
                }
                break;
            }
            case ClassicRhythmCmd::Type::StopSong:
                synth_.drums().stopRhythmSong();
                break;
            case ClassicRhythmCmd::Type::SetTempo:
                synth_.drums().setTempoBpm(cmd.tempoBpm);
                break;
            case ClassicRhythmCmd::Type::SetMetronome:
                synth_.drums().setMetronomeClick(cmd.flag, cmd.u16A, cmd.u16B);
                break;
            case ClassicRhythmCmd::Type::ApplyKeyMap:
                synth_.drums().loadCustomKeyMapAtBaseNote(cmd.keyMap, cmd.index);
                break;
        }
    }
    classicRhyCmdRead_.store(r, std::memory_order_release);
}

void VF50AudioProcessor::setUiScaleFactor(float scale) noexcept {
    uiScaleFactor_.store(std::clamp(scale, 0.75f, 2.00f), std::memory_order_relaxed);
}

void VF50AudioProcessor::setLcdThemeIndex(int themeIdx) noexcept {
    lcdThemeIndex_.store(std::clamp(themeIdx, 0, 2), std::memory_order_relaxed);
}

void VF50AudioProcessor::selectSinglePresetAtomic(vf50::state::MemoryBank bank,
                                                 std::size_t presetIndex,
                                                 bool updateSelectorParams) {
    using vf50::params::ParamIndex;

    pullLatestAudioMemoryToHost();
    const std::size_t clampedIdx = std::min(presetIndex, vf50::state::VF50MemoryManager::kVoiceBankSize - 1u);

    // 1. Cleanly overwrite host Voice Edit Buffer (VMEM) and reset Quick Edit offsets to zero
    hostMemory_.selectSingleVoice(bank, clampedIdx);

    // 2. Mirror pristine preset & zeroed Quick Edit controls to APVTS without firing dirty listeners
    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        if (updateSelectorParams) {
            vf50::params::setApvtsRawValue(apvts_, ParamIndex::VoiceBank,
                                          (bank == vf50::state::MemoryBank::UserRam) ? 1.0f : 0.0f);
            vf50::params::setApvtsRawValue(apvts_, ParamIndex::VoicePreset, static_cast<float>(clampedIdx));
        }
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickAttack, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickRelease, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickBrilliance, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickTotalLevel, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::FxSingleSend, 1.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::Lfo2Override, 0.0f);

        const bool syncFx = (hostMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Single);
        vf50::params::syncVoiceToApvts(apvts_,
                                      hostMemory_.activeSingleVoice(),
                                      hostMemory_.operatorEnableMask(),
                                      syncFx);
    }

    // 3. Purge any stale single-voice/Quick-Edit/effect dirty bits from previous preset
    clearSingleVoiceDirtyFlags();
    pendingAudioSlot_.store(-1, std::memory_order_release);

    // 4. Stage complete 60+ parameter voice block in lock-free double buffer and flag audio thread
    const uint32_t writeSlot = (activeStagedPatchSlot_.load(std::memory_order_relaxed) + 1u) & 1u;
    auto& staged = stagedPatchSlots_[writeSlot];
    staged.voiceSlot             = hostMemory_.singleEditSlot();
    staged.bank                  = bank;
    staged.presetIndex           = clampedIdx;
    staged.quickEdit             = { 0, 0, 0, 0 };
    staged.opEnableMask          = 0x0Fu;
    staged.singleEffectSend      = true;
    staged.useAudioUserRamLookup = false;
    staged.forceSingleEditMode   = false;

    activeStagedPatchSlot_.store(writeSlot, std::memory_order_release);
    activeVoiceBank_.store(static_cast<uint8_t>(bank), std::memory_order_relaxed);
    newPatchPending.store(true, std::memory_order_release);
    resetMixerConsoleToNominal(true);
    bumpPatchSyncEpoch();
}

void VF50AudioProcessor::stageActiveSingleEditVoiceAtomic() {
    using vf50::params::ParamIndex;

    hostMemory_.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
    hostMemory_.resetQuickEditOffsets();
    hostMemory_.setOperatorEnableMask(0x0Fu);
    hostMemory_.setSingleEffectSend(true);

    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::PlayMode, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::WorkstationMode,
                                      static_cast<float>(vf50::state::WorkstationMode::SingleEdit));
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickAttack, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickRelease, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickBrilliance, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickTotalLevel, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::FxSingleSend, 1.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::Lfo2Override, 0.0f);
        vf50::params::syncVoiceToApvts(apvts_,
                                      hostMemory_.activeSingleVoice(),
                                      hostMemory_.operatorEnableMask(),
                                      true);
    }

    clearSingleVoiceDirtyFlags();
    pendingAudioSlot_.store(-1, std::memory_order_release);

    const uint32_t writeSlot = (activeStagedPatchSlot_.load(std::memory_order_relaxed) + 1u) & 1u;
    auto& staged = stagedPatchSlots_[writeSlot];
    staged.voiceSlot             = hostMemory_.singleEditSlot();
    staged.bank                  = hostMemory_.selectedVoiceBank();
    staged.presetIndex           = hostMemory_.selectedVoiceIndex();
    staged.quickEdit             = { 0, 0, 0, 0 };
    staged.opEnableMask          = 0x0Fu;
    staged.singleEffectSend      = true;
    staged.useAudioUserRamLookup = false;
    staged.forceSingleEditMode   = true;

    activeStagedPatchSlot_.store(writeSlot, std::memory_order_release);
    activeWsMode_.store(static_cast<uint8_t>(vf50::state::WorkstationMode::SingleEdit), std::memory_order_relaxed);
    newPatchPending.store(true, std::memory_order_release);
    bumpPatchSyncEpoch();
}

void VF50AudioProcessor::stageActivePerfEditAtomic() {
    using vf50::params::ParamIndex;

    hostMemory_.setWorkstationMode(vf50::state::WorkstationMode::PerformanceEdit);

    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::PlayMode, 1.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::WorkstationMode,
                                      static_cast<float>(vf50::state::WorkstationMode::PerformanceEdit));
        vf50::params::syncPerformanceToApvts(apvts_,
                                            hostMemory_.activePerformance(),
                                            hostMemory_.activePerformancePartPans(),
                                            true);
    }

    clearPerformanceDirtyFlags();
    pendingAudioSlot_.store(-1, std::memory_order_release);

    const uint32_t writeSlot = (activeStagedPerfSlot_.load(std::memory_order_relaxed) + 1u) & 1u;
    auto& staged = stagedPerfSlots_[writeSlot];
    staged.perfSlot              = hostMemory_.perfEditSlot();
    staged.bank                  = hostMemory_.selectedPerfBank();
    staged.presetIndex           = hostMemory_.selectedPerfIndex();
    staged.useAudioUserRamLookup = false;
    staged.forcePerfEditMode     = true;

    activeStagedPerfSlot_.store(writeSlot, std::memory_order_release);
    activeWsMode_.store(static_cast<uint8_t>(vf50::state::WorkstationMode::PerformanceEdit), std::memory_order_relaxed);
    newPerfPatchPending_.store(true, std::memory_order_release);
}

void VF50AudioProcessor::selectPerformancePresetAtomic(vf50::state::MemoryBank bank,
                                                      std::size_t presetIndex,
                                                      bool updateSelectorParams) {
    using vf50::params::ParamIndex;

    pullLatestAudioMemoryToHost();
    const std::size_t clampedIdx = std::min(presetIndex, vf50::state::VF50MemoryManager::kPerfBankSize - 1u);

    hostMemory_.selectPerformance(bank, clampedIdx);

    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        if (updateSelectorParams) {
            vf50::params::setApvtsRawValue(apvts_, ParamIndex::PerfBank,
                                          (bank == vf50::state::MemoryBank::UserRam) ? 1.0f : 0.0f);
            vf50::params::setApvtsRawValue(apvts_, ParamIndex::PerfPreset, static_cast<float>(clampedIdx));
        }
        const bool syncFx = (hostMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Performance);
        vf50::params::syncPerformanceToApvts(apvts_,
                                            hostMemory_.activePerformance(),
                                            hostMemory_.activePerformancePartPans(),
                                            syncFx);
    }

    clearPerformanceDirtyFlags();
    pendingAudioSlot_.store(-1, std::memory_order_release);

    const uint32_t writeSlot = (activeStagedPerfSlot_.load(std::memory_order_relaxed) + 1u) & 1u;
    auto& staged = stagedPerfSlots_[writeSlot];
    staged.perfSlot              = hostMemory_.perfEditSlot();
    staged.bank                  = bank;
    staged.presetIndex           = clampedIdx;
    staged.useAudioUserRamLookup = false;
    staged.forcePerfEditMode     = false;

    activeStagedPerfSlot_.store(writeSlot, std::memory_order_release);
    activePerfBank_.store(static_cast<uint8_t>(bank), std::memory_order_relaxed);
    newPerfPatchPending_.store(true, std::memory_order_release);
    resetMixerConsoleToNominal(true);
    bumpPatchSyncEpoch();
}

void VF50AudioProcessor::syncApvtsToActiveSingleVoice(bool includeEffects) {
    pullLatestAudioMemoryToHost();
    const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
    const auto& qe = hostMemory_.quickEditOffsets();
    vf50::params::setApvtsRawValue(apvts_, vf50::params::ParamIndex::QuickAttack, static_cast<float>(qe.attackOffset));
    vf50::params::setApvtsRawValue(apvts_, vf50::params::ParamIndex::QuickRelease, static_cast<float>(qe.releaseOffset));
    vf50::params::setApvtsRawValue(apvts_, vf50::params::ParamIndex::QuickBrilliance, static_cast<float>(qe.brillianceOffset));
    vf50::params::setApvtsRawValue(apvts_, vf50::params::ParamIndex::QuickTotalLevel, static_cast<float>(qe.volumeOffset));
    vf50::params::setApvtsRawValue(apvts_, vf50::params::ParamIndex::FxSingleSend,
                                  hostMemory_.singleEffectSend() ? 1.0f : 0.0f);
    vf50::params::syncVoiceToApvts(apvts_,
                                  hostMemory_.activeSingleVoice(),
                                  hostMemory_.operatorEnableMask(),
                                  includeEffects);
    bumpPatchSyncEpoch();
}

void VF50AudioProcessor::syncApvtsToActivePerformance(bool includeEffects) {
    pullLatestAudioMemoryToHost();
    const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
    vf50::params::syncPerformanceToApvts(apvts_,
                                        hostMemory_.activePerformance(),
                                        hostMemory_.activePerformancePartPans(),
                                        includeEffects);
}

void VF50AudioProcessor::commitHostVoiceEditToAudio(bool syncEffects) {
    using vf50::params::ParamIndex;

    hostMemory_.singleEditSlot().repackRomBytes();
    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::PlayMode, 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::WorkstationMode,
                                      static_cast<float>(hostMemory_.workstationMode()));
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::VoiceBank,
                                      (hostMemory_.selectedVoiceBank() == vf50::state::MemoryBank::UserRam) ? 1.0f : 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::VoicePreset,
                                      static_cast<float>(hostMemory_.selectedVoiceIndex()));
        const auto& qe = hostMemory_.quickEditOffsets();
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickAttack, static_cast<float>(qe.attackOffset));
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickRelease, static_cast<float>(qe.releaseOffset));
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickBrilliance, static_cast<float>(qe.brillianceOffset));
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::QuickTotalLevel, static_cast<float>(qe.volumeOffset));
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::FxSingleSend,
                                      hostMemory_.singleEffectSend() ? 1.0f : 0.0f);
        vf50::params::syncVoiceToApvts(apvts_,
                                      hostMemory_.activeSingleVoice(),
                                      hostMemory_.operatorEnableMask(),
                                      syncEffects);
    }
    clearSingleVoiceDirtyFlags();
    pushHostMemoryToAudio();
    bumpPatchSyncEpoch();
}

bool VF50AudioProcessor::applyHostProgrammerVoiceParam(vf50::params::ParamIndex paramIndex) {
    using vf50::params::ParamIndex;

    pullLatestAudioMemoryToHost();

    // Respect Classic [COMPARE] lock: do not mutate VMEM while comparing against original stored voice
    if (hostMemory_.isSingleCompareActive()) {
        syncApvtsToActiveSingleVoice(false);
        return false;
    }

    const auto idx = static_cast<std::size_t>(paramIndex);
    bool applied = false;

    if (paramIndex == ParamIndex::QuickAttack || paramIndex == ParamIndex::QuickRelease
        || paramIndex == ParamIndex::QuickBrilliance || paramIndex == ParamIndex::QuickTotalLevel) {
        auto qe = hostMemory_.quickEditOffsets();
        qe.attackOffset     = loadInt(paramBank_.quickAttack, -99, 99);
        qe.releaseOffset    = loadInt(paramBank_.quickRelease, -99, 99);
        qe.brillianceOffset = loadInt(paramBank_.quickBrilliance, -99, 99);
        qe.volumeOffset     = loadInt(paramBank_.quickTotalLevel, -99, 99);
        hostMemory_.setQuickEditOffsets(qe);
        applied = true;
    } else if (idx >= vf50::params::kOperatorsStart && idx < vf50::params::kVoiceEnd) {
        const std::size_t rel   = idx - vf50::params::kOperatorsStart;
        const std::size_t opIdx = rel / vf50::params::kParamsPerOperator;
        const std::size_t field = rel % vf50::params::kParamsPerOperator;
        if (field == 0u) {
            uint8_t mask = hostMemory_.operatorEnableMask();
            if (loadBool(paramBank_.ops[opIdx].enable)) {
                mask |= static_cast<uint8_t>(1u << opIdx);
            } else {
                mask = static_cast<uint8_t>(mask & ~(1u << opIdx));
            }
            hostMemory_.setOperatorEnableMask(mask);
            applied = true;
        } else {
            bool maskOrLfo2Only = true;
            applySingleVoiceParamEdit(idx, hostMemory_.singleEditSlot().voice, maskOrLfo2Only);
            applied = true;
        }
    } else if (idx >= vf50::params::kVoiceStart && idx < vf50::params::kVoiceEnd) {
        if (paramIndex < ParamIndex::Lfo2Override || paramIndex > ParamIndex::Lfo2Sync) {
            bool maskOrLfo2Only = true;
            applySingleVoiceParamEdit(idx, hostMemory_.singleEditSlot().voice, maskOrLfo2Only);
            applied = true;
        }
    }

    if (!applied) {
        return false;
    }

    hostMemory_.markSingleEditDirty();
    if (hostMemory_.workstationMode() != vf50::state::WorkstationMode::SingleEdit) {
        hostMemory_.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
    }
    commitHostVoiceEditToAudio(false);
    return true;
}

bool VF50AudioProcessor::applyHostMixerPerfPartVolume(std::size_t part0To7, uint8_t vol0To99) {
    using vf50::params::ParamIndex;

    if (part0To7 >= 8u) {
        return false;
    }
    pullLatestAudioMemoryToHost();
    if (hostMemory_.isPerformanceCompareActive()) {
        syncApvtsToActivePerformance(false);
        return false;
    }

    const uint8_t clamped = std::min<uint8_t>(vol0To99, 99u);
    hostMemory_.perfEditSlot().perf.instruments[part0To7].volume = clamped;
    hostMemory_.perfEditSlot().repackRomBytes();
    hostMemory_.markPerfEditDirty();

    const auto pIdx = static_cast<ParamIndex>(
        vf50::params::kPerfPartsStart + part0To7 * vf50::params::kParamsPerPerfPart + 4u);
    vf50::params::setApvtsRawValue(apvts_, pIdx, static_cast<float>(clamped));
    bumpPatchSyncEpoch();
    return true;
}

bool VF50AudioProcessor::applyHostMixerPerfPartOutAssign(std::size_t part0To7, uint8_t outAssign0To3) {
    using vf50::params::ParamIndex;

    if (part0To7 >= 8u) {
        return false;
    }
    pullLatestAudioMemoryToHost();
    if (hostMemory_.isPerformanceCompareActive()) {
        syncApvtsToActivePerformance(false);
        return false;
    }

    const uint8_t clamped = std::min<uint8_t>(outAssign0To3, 3u);
    hostMemory_.perfEditSlot().perf.instruments[part0To7].outAssign = clamped;
    hostMemory_.perfEditSlot().repackRomBytes();
    hostMemory_.markPerfEditDirty();

    const auto pIdx = static_cast<ParamIndex>(
        vf50::params::kPerfPartsStart + part0To7 * vf50::params::kParamsPerPerfPart + 10u);
    vf50::params::setApvtsRawValue(apvts_, pIdx, static_cast<float>(clamped));
    bumpPatchSyncEpoch();
    return true;
}

void VF50AudioProcessor::applyHostMixerRhythmVolume(uint8_t vol0To99) {
    using vf50::params::ParamIndex;

    pullLatestAudioMemoryToHost();
    const uint8_t clamped = std::min<uint8_t>(vol0To99, 99u);
    hostMemory_.mutableActiveRhythmKit().rhythmVolume = clamped;
    requestClassicRhythmCommitKit(hostMemory_.activeRhythmKit());
    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::RhythmVolume, static_cast<float>(clamped));
    }
    bumpPatchSyncEpoch();
}

void VF50AudioProcessor::applyHostMixerRhythmEffectMode(vf50::dsp::drums::RhythmEffectMode mode) {
    using vf50::params::ParamIndex;

    pullLatestAudioMemoryToHost();
    hostMemory_.mutableActiveRhythmKit().effectMode = mode;
    requestClassicRhythmCommitKit(hostMemory_.activeRhythmKit());
    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::FxRhythmSendMode, static_cast<float>(mode));
    }
    uint8_t sendVal = 50u;
    if (mode == vf50::dsp::drums::RhythmEffectMode::Off) {
        sendVal = 0u;
    } else if (mode == vf50::dsp::drums::RhythmEffectMode::On) {
        sendVal = 99u;
    }
    synth_.setChannelFxSend(8u, sendVal);
    bumpPatchSyncEpoch();
}

bool VF50AudioProcessor::applyHostMixerPerfPartNotes(std::size_t part0To7, uint8_t notes0To16) {
    using vf50::params::ParamIndex;

    if (part0To7 >= 8u) {
        return false;
    }
    pullLatestAudioMemoryToHost();
    if (hostMemory_.isPerformanceCompareActive()) {
        syncApvtsToActivePerformance(false);
        return false;
    }

    auto& pf = hostMemory_.perfEditSlot().perf;
    auto& inst = pf.instruments[part0To7];
    const int reqNotes = std::clamp<int>(notes0To16, 0, 16);

    if (pf.assignMode == 2u) {
        int usedOther = 0;
        for (std::size_t i = 0u; i < 8u; ++i) {
            if (i != part0To7 && pf.instruments[i].reserveNotes > 1u) {
                usedOther += static_cast<int>(pf.instruments[i].reserveNotes) - 1;
            }
        }
        const int maxAllowed = std::max(0, 16 - usedOther);
        const int clamped = std::clamp(reqNotes, 0, maxAllowed);
        if (clamped == 0) {
            inst.reserveNotes = 0u;
            inst.maxNotes = 0u;
        } else {
            inst.reserveNotes = static_cast<uint8_t>(1 + clamped);
            if (inst.maxNotes == 0u) {
                inst.maxNotes = 1u;
            }
            if (inst.limitLow == 0u && inst.limitHigh == 0u && inst.outAssign == 0u) {
                inst.limitHigh = 127u;
                inst.outAssign = 3u;
                if (inst.volume == 0u) inst.volume = 90u;
            }
        }
    } else {
        int usedOther = 0;
        for (std::size_t i = 0u; i < 8u; ++i) {
            if (i != part0To7) {
                usedOther += static_cast<int>(pf.instruments[i].maxNotes);
            }
        }
        const int maxAllowed = std::max(0, 16 - usedOther);
        const int clamped = std::clamp(reqNotes, 0, maxAllowed);
        inst.maxNotes = static_cast<uint8_t>(clamped);
        if (clamped > 0 && inst.limitLow == 0u && inst.limitHigh == 0u && inst.outAssign == 0u) {
            inst.limitHigh = 127u;
            inst.outAssign = 3u;
            if (inst.volume == 0u) inst.volume = 90u;
        }
    }

    hostMemory_.perfEditSlot().repackRomBytes();
    hostMemory_.markPerfEditDirty();

    const auto pMaxIdx = static_cast<ParamIndex>(
        vf50::params::kPerfPartsStart + part0To7 * vf50::params::kParamsPerPerfPart + 2u);
    const auto pResIdx = static_cast<ParamIndex>(
        vf50::params::kPerfPartsStart + part0To7 * vf50::params::kParamsPerPerfPart + 3u);
    vf50::params::setApvtsRawValue(apvts_, pMaxIdx, static_cast<float>(inst.maxNotes));
    vf50::params::setApvtsRawValue(apvts_, pResIdx, static_cast<float>(inst.reserveNotes));
    bumpPatchSyncEpoch();
    return true;
}

bool VF50AudioProcessor::applyHostMixerPerfPartNoteShift(std::size_t part0To7, int8_t shiftNeg24ToPos24) {
    using vf50::params::ParamIndex;

    if (part0To7 >= 8u) {
        return false;
    }
    pullLatestAudioMemoryToHost();
    if (hostMemory_.isPerformanceCompareActive()) {
        syncApvtsToActivePerformance(false);
        return false;
    }

    const int8_t clamped = static_cast<int8_t>(std::clamp<int>(shiftNeg24ToPos24, -24, 24));
    hostMemory_.perfEditSlot().perf.instruments[part0To7].noteShift = clamped;
    hostMemory_.perfEditSlot().repackRomBytes();
    hostMemory_.markPerfEditDirty();

    const auto pIdx = static_cast<ParamIndex>(
        vf50::params::kPerfPartsStart + part0To7 * vf50::params::kParamsPerPerfPart + 7u);
    vf50::params::setApvtsRawValue(apvts_, pIdx, static_cast<float>(clamped));
    bumpPatchSyncEpoch();
    return true;
}

bool VF50AudioProcessor::applyHostMixerPerfPartDetune(std::size_t part0To7, int8_t detuneNeg7ToPos7) {
    using vf50::params::ParamIndex;

    if (part0To7 >= 8u) {
        return false;
    }
    pullLatestAudioMemoryToHost();
    if (hostMemory_.isPerformanceCompareActive()) {
        syncApvtsToActivePerformance(false);
        return false;
    }

    const int8_t clamped = static_cast<int8_t>(std::clamp<int>(detuneNeg7ToPos7, -7, 7));
    hostMemory_.perfEditSlot().perf.instruments[part0To7].detune = clamped;
    hostMemory_.perfEditSlot().repackRomBytes();
    hostMemory_.markPerfEditDirty();

    const auto pIdx = static_cast<ParamIndex>(
        vf50::params::kPerfPartsStart + part0To7 * vf50::params::kParamsPerPerfPart + 6u);
    vf50::params::setApvtsRawValue(apvts_, pIdx, static_cast<float>(clamped));
    bumpPatchSyncEpoch();
    return true;
}

void VF50AudioProcessor::applyHostMixerDrumInstVolume(std::size_t drum0To60, uint8_t vol0To15) {
    using vf50::params::ParamIndex;

    if (drum0To60 >= vf50::generated::kDrumInstrumentCount) {
        return;
    }
    pullLatestAudioMemoryToHost();
    const uint8_t clamped = std::min<uint8_t>(vol0To15, 15u);
    hostMemory_.mutableActiveRhythmKit().instruments[drum0To60].volume = clamped;
    requestClassicRhythmCommitKit(hostMemory_.activeRhythmKit());
    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        const auto pIdx = static_cast<ParamIndex>(
            vf50::params::kDrumsStart + drum0To60 * vf50::params::kParamsPerDrumInst + 0u);
        vf50::params::setApvtsRawValue(apvts_, pIdx, static_cast<float>(clamped));
    }
    bumpPatchSyncEpoch();
}

void VF50AudioProcessor::applyHostMixerDrumInstPan(std::size_t drum0To60, int8_t panNeg3ToPos3) {
    using vf50::params::ParamIndex;

    if (drum0To60 >= vf50::generated::kDrumInstrumentCount) {
        return;
    }
    pullLatestAudioMemoryToHost();
    const int8_t clamped = static_cast<int8_t>(std::clamp<int>(panNeg3ToPos3, -3, 3));
    hostMemory_.mutableActiveRhythmKit().instruments[drum0To60].pan = clamped;
    requestClassicRhythmCommitKit(hostMemory_.activeRhythmKit());
    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        const auto pIdx = static_cast<ParamIndex>(
            vf50::params::kDrumsStart + drum0To60 * vf50::params::kParamsPerDrumInst + 1u);
        vf50::params::setApvtsRawValue(apvts_, pIdx, static_cast<float>(clamped));
    }
    bumpPatchSyncEpoch();
}

bool VF50AudioProcessor::applyHostMixerEffectType(uint8_t fxType0To32) {
    using vf50::params::ParamIndex;

    pullLatestAudioMemoryToHost();
    const uint8_t clamped = std::min<uint8_t>(fxType0To32, 32u);
    if (hostMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Performance) {
        if (hostMemory_.isPerformanceCompareActive()) {
            syncApvtsToActivePerformance(true);
            return false;
        }
        hostMemory_.perfEditSlot().perf.effect.effectType = clamped;
        hostMemory_.perfEditSlot().repackRomBytes();
        hostMemory_.markPerfEditDirty();
    } else {
        if (hostMemory_.isSingleCompareActive()) {
            syncApvtsToActiveSingleVoice(true);
            return false;
        }
        hostMemory_.singleEditSlot().voice.effect.effectType = clamped;
        hostMemory_.singleEditSlot().repackRomBytes();
        hostMemory_.markSingleEditDirty();
    }

    vf50::params::setApvtsRawValue(apvts_, ParamIndex::FxAlgorithm, static_cast<float>(clamped));
    bumpPatchSyncEpoch();
    return true;
}

void VF50AudioProcessor::setMixerChannelFxSend(std::size_t ch0To8, uint8_t send0To99) {
    if (ch0To8 >= 9u) {
        return;
    }
    const uint8_t clamped = std::min<uint8_t>(send0To99, 99u);
    synth_.setChannelFxSend(ch0To8, clamped);
    if (ch0To8 == 8u) {
        using vf50::dsp::drums::RhythmEffectMode;
        const RhythmEffectMode mode = (clamped == 0u)
                                          ? RhythmEffectMode::Off
                                          : ((clamped >= 75u) ? RhythmEffectMode::On : RhythmEffectMode::Mix);
        pullLatestAudioMemoryToHost();
        if (hostMemory_.activeRhythmKit().effectMode != mode) {
            hostMemory_.mutableActiveRhythmKit().effectMode = mode;
            requestClassicRhythmCommitKit(hostMemory_.activeRhythmKit());
            {
                const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
                vf50::params::setApvtsRawValue(apvts_, vf50::params::ParamIndex::FxRhythmSendMode,
                                              static_cast<float>(mode));
            }
            bumpPatchSyncEpoch();
        }
    }
}

void VF50AudioProcessor::resetMixerConsoleToNominal(bool clearSoloToo) noexcept {
    synth_.resetConsoleMixExtensions(hostMemory_.activePerformance(),
                                     hostMemory_.activeRhythmKit().effectMode,
                                     clearSoloToo);
}

bool VF50AudioProcessor::applyHostMixerEffectBalance(uint8_t bal0To99) {
    using vf50::params::ParamIndex;

    pullLatestAudioMemoryToHost();
    const uint8_t clamped = std::min<uint8_t>(bal0To99, 99u);
    if (hostMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Performance) {
        if (hostMemory_.isPerformanceCompareActive()) {
            syncApvtsToActivePerformance(true);
            return false;
        }
        hostMemory_.perfEditSlot().perf.effect.balance = clamped;
        hostMemory_.perfEditSlot().repackRomBytes();
        hostMemory_.markPerfEditDirty();
    } else {
        if (hostMemory_.isSingleCompareActive()) {
            syncApvtsToActiveSingleVoice(true);
            return false;
        }
        hostMemory_.singleEditSlot().voice.effect.balance = clamped;
        hostMemory_.singleEditSlot().repackRomBytes();
        hostMemory_.markSingleEditDirty();
    }

    vf50::params::setApvtsRawValue(apvts_, ParamIndex::FxWetDryMix, static_cast<float>(clamped));
    bumpPatchSyncEpoch();
    return true;
}

void VF50AudioProcessor::commitHostPerfEditToAudio(bool syncEffects) {
    using vf50::params::ParamIndex;

    hostMemory_.perfEditSlot().repackRomBytes();
    {
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::PlayMode, 1.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::WorkstationMode,
                                      static_cast<float>(hostMemory_.workstationMode()));
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::PerfBank,
                                      (hostMemory_.selectedPerfBank() == vf50::state::MemoryBank::UserRam) ? 1.0f : 0.0f);
        vf50::params::setApvtsRawValue(apvts_, ParamIndex::PerfPreset,
                                      static_cast<float>(hostMemory_.selectedPerfIndex()));
        vf50::params::syncPerformanceToApvts(apvts_,
                                            hostMemory_.activePerformance(),
                                            hostMemory_.activePerformancePartPans(),
                                            syncEffects);
    }
    clearPerformanceDirtyFlags();
    pushHostMemoryToAudio();
    bumpPatchSyncEpoch();
}

void VF50AudioProcessor::applyStagedVoicePatchRealTime(const StagedVoicePatch& staged) noexcept {
    const bool seqRunning = synth_.demoPlayer().isPlaying();
    if (seqRunning) {
        // Keep sequencer running uninterrupted while cleanly releasing active notes so none hang!
        synth_.demoPlayer().notifyExternalPatchOrModeSwitch(synth_.fm());
    }

    if (staged.forceSingleEditMode) {
        audioMemory_.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
    }
    audioMemory_.selectSingleVoice(staged.bank, staged.presetIndex);
    if (!staged.useAudioUserRamLookup) {
        audioMemory_.singleEditSlot() = staged.voiceSlot;
    }
    audioMemory_.setQuickEditOffsets(staged.quickEdit);
    audioMemory_.setOperatorEnableMask(staged.opEnableMask);
    audioMemory_.setSingleEffectSend(staged.singleEffectSend);

    if (classicAudioSys_ != nullptr && classicAudioSys_->sys.controllerReset) {
        synth_.fm().resetAllChannelControllers();
    }

    if (audioMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Single) {
        const bool dampVoices = staged.forceSingleEditMode
                             || (classicAudioSys_ == nullptr)
                             || classicAudioSys_->sys.voiceDampOn;
        synth_.applyAtomicSingleVoicePatch(audioMemory_.activeSingleVoice(),
                                           staged.presetIndex,
                                           staged.bank == vf50::state::MemoryBank::UserRam,
                                           staged.quickEdit,
                                           staged.opEnableMask,
                                           staged.singleEffectSend,
                                           true,
                                           dampVoices);
    } else {
        synth_.fm().selectSingleVoice(staged.presetIndex, staged.bank == vf50::state::MemoryBank::UserRam);
        synth_.fm().setActiveSingleVoiceDirect(audioMemory_.activeSingleVoice());
    }

    activeWsMode_.store(static_cast<uint8_t>(audioMemory_.workstationMode()), std::memory_order_relaxed);
    activeVoiceBank_.store(static_cast<uint8_t>(audioMemory_.selectedVoiceBank()), std::memory_order_relaxed);

    if (!newPatchPending.load(std::memory_order_acquire) &&
        pendingHostSlot_.load(std::memory_order_acquire) < 0) {
        const int slot = nextAudioWriteSlot_ & 1;
        audioToHostSlots_[static_cast<std::size_t>(slot)] = audioMemory_;
        pendingAudioSlot_.store(slot, std::memory_order_release);
        nextAudioWriteSlot_ ^= 1;
    }
}

void VF50AudioProcessor::applyStagedPerfPatchRealTime(const StagedPerfPatch& staged) noexcept {
    const bool seqRunning = synth_.demoPlayer().isPlaying();
    if (seqRunning) {
        // Keep sequencer running uninterrupted while updating 8-part multitimbral routing & releasing old notes!
        synth_.demoPlayer().notifyExternalPatchOrModeSwitch(synth_.fm());
    }

    if (staged.forcePerfEditMode) {
        audioMemory_.setWorkstationMode(vf50::state::WorkstationMode::PerformanceEdit);
    }
    audioMemory_.selectPerformance(staged.bank, staged.presetIndex);
    if (!staged.useAudioUserRamLookup) {
        audioMemory_.perfEditSlot() = staged.perfSlot;
    }

    if (classicAudioSys_ != nullptr && classicAudioSys_->sys.controllerReset) {
        synth_.fm().resetAllChannelControllers();
    }

    if (audioMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Performance || seqRunning) {
        synth_.applyAtomicPerformancePatch(audioMemory_.activePerformance(),
                                           staged.presetIndex,
                                           staged.bank == vf50::state::MemoryBank::UserRam,
                                           audioMemory_.activePerformancePartPans(),
                                           true);
    } else {
        synth_.fm().selectPerformance(staged.presetIndex, staged.bank == vf50::state::MemoryBank::UserRam);
        synth_.fm().setActivePerformanceDirect(audioMemory_.activePerformance());
    }

    activeWsMode_.store(static_cast<uint8_t>(audioMemory_.workstationMode()), std::memory_order_relaxed);
    activePerfBank_.store(static_cast<uint8_t>(audioMemory_.selectedPerfBank()), std::memory_order_relaxed);

    if (!newPerfPatchPending_.load(std::memory_order_acquire) &&
        pendingHostSlot_.load(std::memory_order_acquire) < 0) {
        const int slot = nextAudioWriteSlot_ & 1;
        audioToHostSlots_[static_cast<std::size_t>(slot)] = audioMemory_;
        pendingAudioSlot_.store(slot, std::memory_order_release);
        nextAudioWriteSlot_ ^= 1;
    }
}

void VF50AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    const double safeSampleRate = (sampleRate > 1000.0 && std::isfinite(sampleRate)) ? sampleRate : 44100.0;
    synth_.prepare(safeSampleRate, static_cast<std::size_t>(std::max(samplesPerBlock, 64)));
    keyboardState_.reset();
    sysExHandler_.reset();
    sysExReplyBuffer_.clear();
    localKbdMidiBuf_.ensureSize(4096);
    mergedMidiBuf_.ensureSize(8192);
    lastRhythmPlay_ = false;

    const int staged = pendingHostSlot_.exchange(-1, std::memory_order_acq_rel);
    if (staged >= 0 && staged < 2) {
        audioMemory_ = hostToAudioSlots_[static_cast<std::size_t>(staged)];
    }
    const int reqKeyLayout = pendingRhythmKeyLayout_.exchange(-1, std::memory_order_acq_rel);
    if (reqKeyLayout >= 0 && reqKeyLayout <= 3) {
        audioMemory_.selectRhythmKeyMap(static_cast<std::size_t>(reqKeyLayout));
    }
    audioMemory_.applyToSynthEngine(synth_, true);
    activeRhythmKeyLayout_.store(synth_.drums().activeKeyAssignLayout(), std::memory_order_relaxed);
    applyGlobalRuntimeParamsFromAtomics();

    if (newPatchPending.load(std::memory_order_acquire)) {
        if (newPatchPending.exchange(false, std::memory_order_acq_rel)) {
            const uint32_t slotIdx = activeStagedPatchSlot_.load(std::memory_order_acquire) & 1u;
            applyStagedVoicePatchRealTime(stagedPatchSlots_[slotIdx]);
        }
    }
    if (newPerfPatchPending_.load(std::memory_order_acquire)) {
        if (newPerfPatchPending_.exchange(false, std::memory_order_acq_rel)) {
            const uint32_t slotIdx = activeStagedPerfSlot_.load(std::memory_order_acquire) & 1u;
            applyStagedPerfPatchRealTime(stagedPerfSlots_[slotIdx]);
        }
    }

    if (anyParamDirty_.exchange(false, std::memory_order_acquire)) {
        processDirtyParametersRealTime();
    }
}

void VF50AudioProcessor::releaseResources() {
    keyboardState_.reset();
    synth_.allSoundOff();
}

bool VF50AudioProcessor::isHostAuxBusEnabled(std::size_t ch0To8) const noexcept {
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    if (ch0To8 < 9u) {
        if (const auto* bus = getBus(false, static_cast<int>(ch0To8) + 1)) {
            return bus->isEnabled();
        }
    }
#else
    (void)ch0To8;
#endif
    return false;
}

bool VF50AudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    for (const auto& inSet : layouts.inputBuses) {
        if (!inSet.isDisabled()) {
            return false;
        }
    }
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    if (layouts.outputBuses.isEmpty() || layouts.outputBuses.size() > 10) {
        return false;
    }
    if (layouts.outputBuses.getReference(0) != juce::AudioChannelSet::stereo()) {
        return false;
    }
    for (int b = 1; b < layouts.outputBuses.size(); ++b) {
        const auto& outSet = layouts.outputBuses.getReference(b);
        if (!outSet.isDisabled() && outSet != juce::AudioChannelSet::stereo()) {
            return false;
        }
    }
    return true;
#else
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
#endif
}

void VF50AudioProcessor::applyGlobalRuntimeParamsFromAtomics() noexcept {
    synth_.setMasterVolume(paramBank_.masterVolume->load(std::memory_order_relaxed));
    synth_.fm().setMasterTuneCents(static_cast<double>(paramBank_.masterTune->load(std::memory_order_relaxed)));
    synth_.fm().setGlobalTransposeSemitones(globalTransposeSemitones_.load(std::memory_order_relaxed));
    synth_.fm().setSingleReceiveChannel(static_cast<uint8_t>(loadInt(paramBank_.singleReceiveCh, 0, 16)));

    vf50::dsp::fm::QuickEditOffsets qe{};
    if (!audioMemory_.isSingleCompareActive()) {
        qe.attackOffset     = loadInt(paramBank_.quickAttack, -99, 99);
        qe.releaseOffset    = loadInt(paramBank_.quickRelease, -99, 99);
        qe.brillianceOffset = loadInt(paramBank_.quickBrilliance, -99, 99);
        qe.volumeOffset     = loadInt(paramBank_.quickTotalLevel, -99, 99);
    }
    synth_.fm().setQuickEditOffsets(qe);

    uint8_t opMask = 0u;
    if (audioMemory_.isSingleCompareActive()) {
        opMask = 0x0Fu;
    } else {
        for (std::size_t op = 0; op < 4u; ++op) {
            if (loadBool(paramBank_.ops[op].enable)) {
                opMask |= static_cast<uint8_t>(1u << op);
            }
        }
    }
    synth_.fm().setOperatorEnableMask(opMask);

    vf50::dsp::fm::Lfo2OverrideConfig lfo2{};
    lfo2.enabled = loadBool(paramBank_.lfo2Override);
    lfo2.wave    = static_cast<uint8_t>(loadInt(paramBank_.lfo2Wave, 0, 3));
    lfo2.speed   = static_cast<uint8_t>(loadInt(paramBank_.lfo2Speed, 0, 99));
    lfo2.delay   = static_cast<uint8_t>(loadInt(paramBank_.lfo2Delay, 0, 99));
    lfo2.pmd     = static_cast<uint8_t>(loadInt(paramBank_.lfo2Pmd, 0, 99));
    lfo2.amd     = static_cast<uint8_t>(loadInt(paramBank_.lfo2Amd, 0, 99));
    lfo2.sync    = loadBool(paramBank_.lfo2Sync);
    synth_.fm().setLfo2Override(lfo2);

    synth_.effects().setBypass(loadBool(paramBank_.effectBypass));
    synth_.fm().setSingleEffectSend(loadBool(paramBank_.fxSingleSend));
    synth_.setDrumsEnabled(loadBool(paramBank_.rhythmEnable));
    synth_.drums().setRhythmReceiveChannel(static_cast<uint8_t>(loadInt(paramBank_.rhythmReceiveCh, 0, 17)));
    synth_.drums().setTempoBpm(static_cast<double>(paramBank_.rhythmTempo->load(std::memory_order_relaxed)));
    updateRhythmTransportRealTime(false);

    activeWsMode_.store(static_cast<uint8_t>(audioMemory_.workstationMode()), std::memory_order_relaxed);
    activeVoiceBank_.store(static_cast<uint8_t>(audioMemory_.selectedVoiceBank()), std::memory_order_relaxed);
    activePerfBank_.store(static_cast<uint8_t>(audioMemory_.selectedPerfBank()), std::memory_order_relaxed);
}

void VF50AudioProcessor::startAccompanyingRhythmPatternForSong(double songTempoBpm) noexcept {
    synth_.setDrumsEnabled(true);
    const auto& dp = synth_.demoPlayer();
    const bool rhyMuted = dp.isTrackMuted(8u);

    auto startLinkedRhythmSong = [this, songTempoBpm, rhyMuted](std::size_t rSongIdx) noexcept -> bool {
        if (audioRhythmSongs_ == nullptr || rSongIdx >= 8u) {
            return false;
        }
        const auto& rSong = (*audioRhythmSongs_)[rSongIdx];
        if (!rSong.hasData()) {
            return false;
        }
        std::array<vf50::dsp::drums::Ym3602DrumEngine::RhythmSongPatternRef, 100> refs{};
        for (std::size_t p = 0u; p < 100u; ++p) {
            const auto& uPtn = audioMemory_.userRhythmPattern(p);
            refs[p].events      = uPtn.events.data();
            refs[p].eventCount  = uPtn.eventCount;
            refs[p].totalTicks  = uPtn.totalTicks;
            refs[p].bars        = std::max<uint8_t>(uPtn.bars, 1u);
            refs[p].ticksPerBar = static_cast<uint16_t>(
                std::max(12, static_cast<int>(uPtn.totalTicks) / std::max<int>(1, uPtn.bars)));
        }
        synth_.drums().startRhythmSong(rSong, refs, 0u, 1u, songTempoBpm);
        synth_.drums().setSlavedToSequencer(true);
        synth_.drums().setOutputTriggerMuted(rhyMuted);
        return true;
    };

    if (vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(dp.selectedSongIndex())
        && dp.hasUserSequence(dp.selectedSongIndex())) {
        const auto& cs = dp.userSequence(dp.selectedSongIndex());
        if (cs.hasRhythmLink) {
            if (cs.rhythmSyncIsSong) {
                const std::size_t rSongIdx = std::min<std::size_t>(cs.rhythmTargetIdx & 0x07u, 7u);
                if (startLinkedRhythmSong(rSongIdx)) {
                    return;
                }
            } else {
                if (cs.rhythmTargetIdx >= 100u && cs.rhythmTargetIdx < 200u) {
                    const std::size_t pIdx = static_cast<std::size_t>(cs.rhythmTargetIdx - 100u);
                    audioMemory_.selectRhythmPattern(vf50::state::MemoryBank::FactoryRom, pIdx);
                    synth_.drums().startPattern(pIdx, songTempoBpm, true);
                    synth_.drums().setSlavedToSequencer(true);
                    synth_.drums().setOutputTriggerMuted(rhyMuted);
                    return;
                }
                if (cs.rhythmTargetIdx < 100u) {
                    const auto& uPtn = audioMemory_.userRhythmPattern(cs.rhythmTargetIdx);
                    if (uPtn.eventCount > 0u) {
                        audioMemory_.selectRhythmPattern(vf50::state::MemoryBank::UserRam, cs.rhythmTargetIdx);
                        const uint16_t tpb = static_cast<uint16_t>(
                            std::max(12, static_cast<int>(uPtn.totalTicks) / std::max<int>(1, uPtn.bars)));
                        synth_.drums().startOwnedCustomPattern(uPtn.events.data(),
                                                               uPtn.eventCount,
                                                               uPtn.totalTicks,
                                                               tpb,
                                                               songTempoBpm,
                                                               true);
                        synth_.drums().setSlavedToSequencer(true);
                        synth_.drums().setOutputTriggerMuted(rhyMuted);
                        return;
                    }
                }
            }
        }
        // Default 1:1 pairing with Rhythm Song slot if populated
        if (startLinkedRhythmSong(dp.selectedSongIndex())) {
            return;
        }
    }

    const auto ptnBank = (loadInt(paramBank_.rhythmBank, 0, 1) == 0)
                             ? vf50::state::MemoryBank::FactoryRom
                             : vf50::state::MemoryBank::UserRam;
    const auto ptnIdx  = static_cast<std::size_t>(loadInt(paramBank_.rhythmPattern, 0, 99));
    audioMemory_.selectRhythmPattern(ptnBank, ptnIdx);
    if (ptnBank == vf50::state::MemoryBank::UserRam) {
        const auto& uPtn = audioMemory_.userRhythmPattern(ptnIdx);
        if (uPtn.eventCount > 0u) {
            const uint16_t tpb = static_cast<uint16_t>(
                std::max(12, static_cast<int>(uPtn.totalTicks) / std::max<int>(1, uPtn.bars)));
            synth_.drums().startOwnedCustomPattern(uPtn.events.data(),
                                                   uPtn.eventCount,
                                                   uPtn.totalTicks,
                                                   tpb,
                                                   songTempoBpm,
                                                   true);
            synth_.drums().setSlavedToSequencer(true);
            synth_.drums().setOutputTriggerMuted(rhyMuted);
            return;
        }
    }
    // Factory ROM pattern (or fallback when User RAM pattern is empty, matching VF50MidiExporter Track 9)
    synth_.drums().startPattern(ptnIdx, songTempoBpm, true);
    synth_.drums().setSlavedToSequencer(true);
    synth_.drums().setOutputTriggerMuted(rhyMuted);
}

void VF50AudioProcessor::updateRhythmTransportRealTime(bool /*forceRestart*/) noexcept {
    // Unified Classic workstation drives rhythm transport strictly via wait-free ClassicRhythmCmd queue
    // and Sequencer accompanying rhythm lockstep; APVTS rhythmPlay is not used to override Classic playback.
}

void VF50AudioProcessor::applySingleVoiceParamEdit(std::size_t idx,
                                                  vf50::generated::FactoryVoice& v,
                                                  bool& maskOrLfo2Only) noexcept {
    using vf50::params::ParamIndex;

    if (idx >= vf50::params::kOperatorsStart && idx < vf50::params::kVoiceEnd) {
        const std::size_t rel   = idx - vf50::params::kOperatorsStart;
        const std::size_t opIdx = rel / vf50::params::kParamsPerOperator;
        const std::size_t field = rel % vf50::params::kParamsPerOperator;
        const auto& opP = paramBank_.ops[opIdx];
        auto& o = v.operators[opIdx];

        switch (field) {
            case 0u: {
                uint8_t mask = synth_.fm().operatorEnableMask();
                if (loadBool(opP.enable)) mask |= static_cast<uint8_t>(1u << opIdx);
                else mask = static_cast<uint8_t>(mask & ~(1u << opIdx));
                synth_.fm().setOperatorEnableMask(mask);
                return;
            }
            case 1u:  o.waveform       = static_cast<uint8_t>(loadInt(opP.waveform, 0, 7)); break;
            case 2u:  o.fixedFreqMode  = loadBool(opP.fixedMode) ? 1u : 0u; break;
            case 3u:  o.fixedRangeLow  = loadBool(opP.fixedShift) ? 1u : 0u; break;
            case 4u:  o.fixedRange     = static_cast<uint8_t>(loadInt(opP.fixedRange, 0, 7)); break;
            case 5u:  o.coarse         = static_cast<uint8_t>(loadInt(opP.coarse, 0, 63)); break;
            case 6u:  o.fine           = static_cast<uint8_t>(loadInt(opP.fine, 0, 15)); break;
            case 7u:  o.detune         = static_cast<int8_t>(loadInt(opP.detune, -3, 3)); break;
            case 8u:  o.ar             = static_cast<uint8_t>(loadInt(opP.ar, 0, 31)); break;
            case 9u:  o.d1r            = static_cast<uint8_t>(loadInt(opP.d1r, 0, 31)); break;
            case 10u: o.d1l            = static_cast<uint8_t>(loadInt(opP.d1l, 0, 15)); break;
            case 11u: o.d2r            = static_cast<uint8_t>(loadInt(opP.d2r, 0, 31)); break;
            case 12u: o.rr             = static_cast<uint8_t>(loadInt(opP.rr, 1, 15)); break;
            case 13u: o.egShift        = static_cast<uint8_t>(loadInt(opP.egShift, 0, 3)); break;
            case 14u: o.outputLevel    = static_cast<uint8_t>(loadInt(opP.outputLevel, 0, 99)); break;
            case 15u: o.rateScaling    = static_cast<uint8_t>(loadInt(opP.rateScaling, 0, 3)); break;
            case 16u: o.levelScaling   = static_cast<int8_t>(loadInt(opP.levelScaling, -99, 99)); break;
            case 17u: o.keyVelSens     = static_cast<int8_t>(loadInt(opP.keyVelSens, -7, 7)); break;
            case 18u: o.egBiasSens     = static_cast<uint8_t>(loadInt(opP.egBiasSens, 0, 7)); break;
            case 19u: o.ampModEnable   = loadBool(opP.ampModEnable) ? 1u : 0u; break;
            default: break;
        }
        maskOrLfo2Only = false;
        return;
    }

    switch (static_cast<ParamIndex>(idx)) {
        case ParamIndex::VoiceAlgorithm:      v.algorithm      = static_cast<uint8_t>(loadInt(paramBank_.voiceAlgorithm, 0, 7)); maskOrLfo2Only = false; break;
        case ParamIndex::VoiceFeedback:       v.feedback       = static_cast<uint8_t>(loadInt(paramBank_.voiceFeedback, 0, 7)); maskOrLfo2Only = false; break;
        case ParamIndex::VoiceTranspose:      v.transpose      = static_cast<int8_t>(loadInt(paramBank_.voiceTranspose, -24, 24)); maskOrLfo2Only = false; break;
        case ParamIndex::VoiceMonoMode:       v.monoMode       = static_cast<uint8_t>(loadInt(paramBank_.voiceMonoMode, 0, 1)); maskOrLfo2Only = false; break;
        case ParamIndex::VoicePitchBendRange: v.pitchBendRange = static_cast<uint8_t>(loadInt(paramBank_.voicePitchBendRange, 0, 12)); maskOrLfo2Only = false; break;
        case ParamIndex::VoicePortaSwitch:    v.portaSwitch    = loadBool(paramBank_.voicePortaSwitch) ? 1u : 0u; maskOrLfo2Only = false; break;
        case ParamIndex::VoicePortaMode:      v.portaMode      = static_cast<uint8_t>(loadInt(paramBank_.voicePortaMode, 0, 1)); maskOrLfo2Only = false; break;
        case ParamIndex::VoicePortaTime:      v.portaTime      = static_cast<uint8_t>(loadInt(paramBank_.voicePortaTime, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::VoiceSustainSwitch:  v.sustainSwitch  = loadBool(paramBank_.voiceSustainSwitch) ? 1u : 0u; maskOrLfo2Only = false; break;
        case ParamIndex::VoiceChorusSwitch:   v.chorusSwitch   = loadBool(paramBank_.voiceChorusSwitch) ? 1u : 0u; maskOrLfo2Only = false; break;
        case ParamIndex::VoiceReverbRate:     v.reverbRate     = static_cast<uint8_t>(loadInt(paramBank_.voiceReverbRate, 0, 7)); maskOrLfo2Only = false; break;

        case ParamIndex::Lfo1Wave:            v.lfoWave        = static_cast<uint8_t>(loadInt(paramBank_.lfo1Wave, 0, 3)); maskOrLfo2Only = false; break;
        case ParamIndex::Lfo1Speed:           v.lfoSpeed       = static_cast<uint8_t>(loadInt(paramBank_.lfo1Speed, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::Lfo1Delay:           v.lfoDelay       = static_cast<uint8_t>(loadInt(paramBank_.lfo1Delay, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::Lfo1Pmd:             v.pmd            = static_cast<uint8_t>(loadInt(paramBank_.lfo1Pmd, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::Lfo1Amd:             v.amd            = static_cast<uint8_t>(loadInt(paramBank_.lfo1Amd, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::Lfo1Sync:            v.lfoSync        = loadBool(paramBank_.lfo1Sync) ? 1u : 0u; maskOrLfo2Only = false; break;
        case ParamIndex::Lfo1Pms:             v.pms            = static_cast<uint8_t>(loadInt(paramBank_.lfo1Pms, 0, 7)); maskOrLfo2Only = false; break;
        case ParamIndex::Lfo1Ams:             v.ams            = static_cast<uint8_t>(loadInt(paramBank_.lfo1Ams, 0, 3)); maskOrLfo2Only = false; break;

        case ParamIndex::Lfo2Override:
        case ParamIndex::Lfo2Wave:
        case ParamIndex::Lfo2Speed:
        case ParamIndex::Lfo2Delay:
        case ParamIndex::Lfo2Pmd:
        case ParamIndex::Lfo2Amd:
        case ParamIndex::Lfo2Sync: {
            vf50::dsp::fm::Lfo2OverrideConfig lfo2{};
            lfo2.enabled = loadBool(paramBank_.lfo2Override);
            lfo2.wave    = static_cast<uint8_t>(loadInt(paramBank_.lfo2Wave, 0, 3));
            lfo2.speed   = static_cast<uint8_t>(loadInt(paramBank_.lfo2Speed, 0, 99));
            lfo2.delay   = static_cast<uint8_t>(loadInt(paramBank_.lfo2Delay, 0, 99));
            lfo2.pmd     = static_cast<uint8_t>(loadInt(paramBank_.lfo2Pmd, 0, 99));
            lfo2.amd     = static_cast<uint8_t>(loadInt(paramBank_.lfo2Amd, 0, 99));
            lfo2.sync    = loadBool(paramBank_.lfo2Sync);
            synth_.fm().setLfo2Override(lfo2);
            break;
        }

        case ParamIndex::PegPr1:              v.peg.pr1        = static_cast<uint8_t>(loadInt(paramBank_.pegPr1, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::PegPr2:              v.peg.pr2        = static_cast<uint8_t>(loadInt(paramBank_.pegPr2, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::PegPr3:              v.peg.pr3        = static_cast<uint8_t>(loadInt(paramBank_.pegPr3, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::PegPl1:              v.peg.pl1        = static_cast<uint8_t>(loadInt(paramBank_.pegPl1, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::PegPl2:              v.peg.pl2        = static_cast<uint8_t>(loadInt(paramBank_.pegPl2, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::PegPl3:              v.peg.pl3        = static_cast<uint8_t>(loadInt(paramBank_.pegPl3, 0, 99)); maskOrLfo2Only = false; break;

        case ParamIndex::CtrlMwPitch:         v.mwPitch        = static_cast<uint8_t>(loadInt(paramBank_.ctrlMwPitch, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlMwAmp:           v.mwAmp          = static_cast<uint8_t>(loadInt(paramBank_.ctrlMwAmp, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlFcVol:           v.fcVolume       = static_cast<uint8_t>(loadInt(paramBank_.ctrlFcVol, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlFcPitch:         v.fcPitch        = static_cast<uint8_t>(loadInt(paramBank_.ctrlFcPitch, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlFcAmp:           v.fcAmp          = static_cast<uint8_t>(loadInt(paramBank_.ctrlFcAmp, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlBcPitch:         v.bcPitch        = static_cast<uint8_t>(loadInt(paramBank_.ctrlBcPitch, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlBcAmp:           v.bcAmp          = static_cast<uint8_t>(loadInt(paramBank_.ctrlBcAmp, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlBcPitchBias:     v.bcPitchBias    = static_cast<int8_t>(loadInt(paramBank_.ctrlBcPitchBias, -50, 50)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlBcEgBias:        v.bcEgBias       = static_cast<uint8_t>(loadInt(paramBank_.ctrlBcEgBias, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlAtPitch:         v.atPitch        = static_cast<uint8_t>(loadInt(paramBank_.ctrlAtPitch, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlAtAmp:           v.atAmp          = static_cast<uint8_t>(loadInt(paramBank_.ctrlAtAmp, 0, 99)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlAtPitchBias:     v.atPitchBias    = static_cast<int8_t>(loadInt(paramBank_.ctrlAtPitchBias, -50, 50)); maskOrLfo2Only = false; break;
        case ParamIndex::CtrlAtEgBias:        v.atEgBias       = static_cast<uint8_t>(loadInt(paramBank_.ctrlAtEgBias, 0, 99)); maskOrLfo2Only = false; break;
        default: break;
    }
}

void VF50AudioProcessor::applyPerfParamEdit(std::size_t idx,
                                           vf50::state::StoredPerformanceSlot& pfSlot) noexcept {
    using vf50::params::ParamIndex;

    if (idx >= vf50::params::kPerfPartsStart && idx < vf50::params::kPerfEnd) {
        const std::size_t rel     = idx - vf50::params::kPerfPartsStart;
        const std::size_t partIdx = rel / vf50::params::kParamsPerPerfPart;
        const std::size_t field   = rel % vf50::params::kParamsPerPerfPart;
        const auto& pPtrs = paramBank_.parts[partIdx];
        auto& inst = pfSlot.perf.instruments[partIdx];

        switch (field) {
            case 0u:  inst.voiceBank       = static_cast<uint8_t>(loadInt(pPtrs.voiceBank, 0, 1)); break;
            case 1u:  inst.voiceNumber     = static_cast<uint8_t>(loadInt(pPtrs.voiceNumber, 0, 99)); break;
            case 2u:  inst.maxNotes        = static_cast<uint8_t>(loadInt(pPtrs.maxNotes, 0, 16)); break;
            case 3u:  inst.reserveNotes    = static_cast<uint8_t>(loadInt(pPtrs.reserveNotes, 0, 17)); break;
            case 4u:  inst.volume          = static_cast<uint8_t>(loadInt(pPtrs.volume, 0, 99)); break;
            case 5u: {
                const auto pan = static_cast<int8_t>(loadInt(pPtrs.pan, -64, 63));
                pfSlot.partPans[partIdx] = pan;
                synth_.fm().setPartPan(partIdx, pan);
                break;
            }
            case 6u:  inst.detune          = static_cast<int8_t>(loadInt(pPtrs.detune, -7, 7)); break;
            case 7u:  inst.noteShift       = static_cast<int8_t>(loadInt(pPtrs.noteShift, -24, 24)); break;
            case 8u:  inst.limitLow        = static_cast<uint8_t>(loadInt(pPtrs.limitLow, 0, 127)); break;
            case 9u:  inst.limitHigh       = static_cast<uint8_t>(loadInt(pPtrs.limitHigh, 0, 127)); break;
            case 10u: inst.outAssign       = static_cast<uint8_t>(loadInt(pPtrs.outAssign, 0, 3)); break;
            case 11u: inst.receiveChannel  = static_cast<uint8_t>(loadInt(pPtrs.receiveCh, 0, 17)); break;
            case 12u: inst.lfoSelect       = static_cast<uint8_t>(loadInt(pPtrs.lfoSelect, 0, 3)); break;
            case 13u: inst.microTuneEnable = loadBool(pPtrs.microTuneEnable) ? 1u : 0u; break;
            case 14u:
                inst.effectEnable = loadBool(pPtrs.effectSend) ? 1u : 0u;
                synth_.setChannelFxSend(partIdx, inst.effectEnable ? 99u : 0u);
                break;
            default: break;
        }
        return;
    }

    switch (static_cast<ParamIndex>(idx)) {
        case ParamIndex::PerfAssignMode:     pfSlot.perf.assignMode       = static_cast<uint8_t>(loadInt(paramBank_.perfAssignMode, 0, 2)); break;
        case ParamIndex::PerfMicroTuneTable: pfSlot.perf.microTuneTable   = static_cast<uint8_t>(loadInt(paramBank_.perfMicroTuneTable, 0, 12)); break;
        case ParamIndex::PerfMicroTuneRoot:  pfSlot.perf.microTuneRootKey = static_cast<uint8_t>(loadInt(paramBank_.perfMicroTuneRoot, 0, 11)); break;
        case ParamIndex::PerfEffectSelect:   pfSlot.perf.perfEffectSelect = static_cast<uint8_t>(loadInt(paramBank_.perfEffectSelect, 0, 12)); break;
        default: break;
    }
}

void VF50AudioProcessor::applyRhythmParamEdit(std::size_t idx, bool& restartPatternNeeded) noexcept {
    using vf50::params::ParamIndex;

    if (idx >= vf50::params::kDrumsStart && idx < vf50::params::kRhythmEnd) {
        const std::size_t rel     = idx - vf50::params::kDrumsStart;
        const std::size_t drumIdx = rel / vf50::params::kParamsPerDrumInst;
        const std::size_t field   = rel % vf50::params::kParamsPerDrumInst;
        const auto& dPtrs = paramBank_.drums[drumIdx];
        auto& st = audioMemory_.mutableActiveRhythmKit().instruments[drumIdx];

        switch (field) {
            case 0u:
                st.volume = static_cast<uint8_t>(loadInt(dPtrs.level, 0, 15));
                synth_.drums().setInstrumentVolume(drumIdx, st.volume);
                break;
            case 1u:
                st.pan = static_cast<int8_t>(loadInt(dPtrs.pan, -3, 3));
                synth_.drums().setInstrumentPan(drumIdx, st.pan);
                break;
            case 2u:
                st.pitchSemitones = std::clamp(dPtrs.pitch->load(std::memory_order_relaxed), -24.0f, 24.0f);
                synth_.drums().setInstrumentPitchSemitones(drumIdx, st.pitchSemitones);
                break;
            case 3u:
                st.accent = static_cast<uint8_t>(loadInt(dPtrs.accent, 1, 9));
                synth_.drums().setInstrumentAccent(drumIdx, st.accent);
                break;
            case 4u:
                st.outAssign = static_cast<uint8_t>(loadInt(dPtrs.outAssign, 0, 3));
                synth_.drums().setInstrumentOutAssign(drumIdx, st.outAssign);
                break;
            case 5u:
                st.midiNote = static_cast<uint8_t>(loadInt(dPtrs.midiNote, 0, 127));
                synth_.drums().setInstrumentMidiNote(drumIdx, st.midiNote);
                break;
            default: break;
        }
        return;
    }

    switch (static_cast<ParamIndex>(idx)) {
        case ParamIndex::RhythmEnable:
            synth_.setDrumsEnabled(loadBool(paramBank_.rhythmEnable));
            break;
        case ParamIndex::RhythmPlay:
        case ParamIndex::RhythmBank:
        case ParamIndex::RhythmPattern:
            restartPatternNeeded = true;
            break;
        case ParamIndex::RhythmTempo:
            synth_.drums().setTempoBpm(static_cast<double>(paramBank_.rhythmTempo->load(std::memory_order_relaxed)));
            break;
        case ParamIndex::RhythmVolume: {
            const auto vol = static_cast<uint8_t>(loadInt(paramBank_.rhythmVolume, 0, 99));
            audioMemory_.mutableActiveRhythmKit().rhythmVolume = vol;
            synth_.drums().setRhythmMasterVolume(vol);
            break;
        }
        case ParamIndex::RhythmReceiveCh:
            synth_.drums().setRhythmReceiveChannel(static_cast<uint8_t>(loadInt(paramBank_.rhythmReceiveCh, 0, 17)));
            break;
        case ParamIndex::RhythmKeyMap: {
            const auto km = static_cast<std::size_t>(loadInt(paramBank_.rhythmKeyMap, 0, 4));
            audioMemory_.selectRhythmKeyMap(km);
            if (km < 3u) synth_.drums().loadPresetKeyMap(km);
            else synth_.drums().loadCustomKeyMap(audioMemory_.userKeyMap(km - 3u));
            break;
        }
        case ParamIndex::RhythmKitSelect: {
            const auto kitIdx = static_cast<std::size_t>(loadInt(paramBank_.rhythmKitSelect, 0, 4));
            audioMemory_.selectRhythmKit(kitIdx);
            const auto& kit = audioMemory_.activeRhythmKit();
            for (std::size_t d = 0; d < vf50::generated::kDrumInstrumentCount; ++d) {
                synth_.drums().setInstrumentState(d, kit.instruments[d]);
            }
            synth_.drums().setRhythmMasterVolume(kit.rhythmVolume);
            synth_.drums().setEffectMode(kit.effectMode);
            break;
        }
        default: break;
    }
}

void VF50AudioProcessor::applyEffectParamEdit(std::size_t idx,
                                             vf50::generated::DspEffectSettings& fx,
                                             bool& fxSettingsModified) noexcept {
    using vf50::params::ParamIndex;

    switch (static_cast<ParamIndex>(idx)) {
        case ParamIndex::EffectBypass:
            synth_.effects().setBypass(loadBool(paramBank_.effectBypass));
            break;
        case ParamIndex::FxSingleSend:
            synth_.fm().setSingleEffectSend(loadBool(paramBank_.fxSingleSend));
            break;
        case ParamIndex::FxRhythmSendMode: {
            const auto mode = static_cast<vf50::dsp::drums::RhythmEffectMode>(loadInt(paramBank_.fxRhythmSendMode, 0, 2));
            audioMemory_.mutableActiveRhythmKit().effectMode = mode;
            synth_.drums().setEffectMode(mode);
            uint8_t sendVal = 50u;
            if (mode == vf50::dsp::drums::RhythmEffectMode::Off) sendVal = 0u;
            else if (mode == vf50::dsp::drums::RhythmEffectMode::On) sendVal = 99u;
            synth_.setChannelFxSend(8u, sendVal);
            break;
        }
        case ParamIndex::FxAlgorithm:
            fx.effectType = static_cast<uint8_t>(loadInt(paramBank_.fxAlgorithm, 0, 32));
            fxSettingsModified = true;
            break;
        case ParamIndex::FxWetDryMix:
            fx.balance = static_cast<uint8_t>(loadInt(paramBank_.fxWetDryMix, 0, 100));
            fxSettingsModified = true;
            break;
        case ParamIndex::FxOutLevel:
            fx.outLevel = static_cast<uint8_t>(loadInt(paramBank_.fxOutLevel, 0, 100));
            fxSettingsModified = true;
            break;
        case ParamIndex::FxStereoMix:
            fx.stereoMix = loadBool(paramBank_.fxStereoMix) ? 1u : 0u;
            fxSettingsModified = true;
            break;
        case ParamIndex::FxParam1:
            fx.param1 = static_cast<uint8_t>(loadInt(paramBank_.fxParam1, 0, 75));
            fxSettingsModified = true;
            break;
        case ParamIndex::FxParam2:
            fx.param2 = static_cast<uint8_t>(loadInt(paramBank_.fxParam2, 0, 99));
            fxSettingsModified = true;
            break;
        case ParamIndex::FxParam3:
            fx.param3 = static_cast<uint8_t>(loadInt(paramBank_.fxParam3, 0, 99));
            fxSettingsModified = true;
            break;
        default: break;
    }
}

void VF50AudioProcessor::processDirtyParametersRealTime() noexcept {
    using vf50::params::ParamIndex;

    std::array<uint64_t, vf50::params::kDirtyBitsetWords> words{};
    for (std::size_t w = 0; w < vf50::params::kDirtyBitsetWords; ++w) {
        words[w] = dirtyWords_[w].exchange(0ULL, std::memory_order_acq_rel);
    }

    auto isDirty = [&words](ParamIndex p) noexcept -> bool {
        const auto i = static_cast<std::size_t>(p);
        return ((words[i >> 6u] >> (i & 63u)) & 1ULL) != 0ULL;
    };

    bool memoryMutated     = false;
    bool modeOrPresetDirty = false;

    // 1. Bank, Preset & Workstation Mode Transitions
    if (isDirty(ParamIndex::VoiceBank) || isDirty(ParamIndex::VoicePreset)) {
        const auto bank = (loadInt(paramBank_.voiceBank, 0, 1) == 0)
                              ? vf50::state::MemoryBank::FactoryRom
                              : vf50::state::MemoryBank::UserRam;
        const auto preset = static_cast<std::size_t>(loadInt(paramBank_.voicePreset, 0, 99));
        audioMemory_.selectSingleVoice(bank, preset);
        modeOrPresetDirty = true;
        memoryMutated     = true;
    }

    if (isDirty(ParamIndex::PerfBank) || isDirty(ParamIndex::PerfPreset)) {
        const auto bank = (loadInt(paramBank_.perfBank, 0, 1) == 0)
                              ? vf50::state::MemoryBank::FactoryRom
                              : vf50::state::MemoryBank::UserRam;
        const auto preset = static_cast<std::size_t>(loadInt(paramBank_.perfPreset, 0, 99));
        audioMemory_.selectPerformance(bank, preset);
        modeOrPresetDirty = true;
        memoryMutated     = true;
    }

    if (isDirty(ParamIndex::WorkstationMode)) {
        const auto ws = static_cast<vf50::state::WorkstationMode>(loadInt(paramBank_.workstationMode, 0, 5));
        audioMemory_.setWorkstationMode(ws);
        modeOrPresetDirty = true;
        memoryMutated     = true;
    } else if (isDirty(ParamIndex::PlayMode)) {
        const int pm = loadInt(paramBank_.playMode, 0, 1);
        audioMemory_.setWorkstationMode(pm == 0 ? vf50::state::WorkstationMode::SinglePlay
                                                : vf50::state::WorkstationMode::PerformancePlay);
        modeOrPresetDirty = true;
        memoryMutated     = true;
    }

    if (modeOrPresetDirty) {
        if (synth_.demoPlayer().isPlaying()) {
            synth_.demoPlayer().notifyExternalPatchOrModeSwitch(synth_.fm());
        }
        audioMemory_.applyActiveSelectionToSynthEngine(synth_, true);
    }

    // 2. Global Master & Quick Edit Parameters
    if (isDirty(ParamIndex::MasterVolume)) {
        synth_.setMasterVolume(paramBank_.masterVolume->load(std::memory_order_relaxed));
    }
    if (isDirty(ParamIndex::MasterTune)) {
        synth_.fm().setMasterTuneCents(static_cast<double>(paramBank_.masterTune->load(std::memory_order_relaxed)));
    }
    if (isDirty(ParamIndex::SingleReceiveCh)) {
        synth_.fm().setSingleReceiveChannel(static_cast<uint8_t>(loadInt(paramBank_.singleReceiveCh, 0, 16)));
    }
    if (isDirty(ParamIndex::QuickAttack) || isDirty(ParamIndex::QuickRelease) ||
        isDirty(ParamIndex::QuickBrilliance) || isDirty(ParamIndex::QuickTotalLevel)) {
        vf50::dsp::fm::QuickEditOffsets qe{};
        qe.attackOffset     = loadInt(paramBank_.quickAttack, -99, 99);
        qe.releaseOffset    = loadInt(paramBank_.quickRelease, -99, 99);
        qe.brillianceOffset = loadInt(paramBank_.quickBrilliance, -99, 99);
        qe.volumeOffset     = loadInt(paramBank_.quickTotalLevel, -99, 99);
        audioMemory_.setQuickEditOffsets(qe);
        synth_.fm().setQuickEditOffsets(qe);
        memoryMutated = true;
    }

    // 3. Fine-Grained Voice, Performance, Rhythm & Effects Edits
    bool voiceStructEdited    = false;
    bool perfStructEdited     = false;
    bool restartRhythmNeeded  = isDirty(ParamIndex::RhythmPlay);
    bool fxStructEdited       = false;
    vf50::generated::DspEffectSettings activeFx = audioMemory_.activeEffectSettings();

    for (std::size_t w = 0; w < vf50::params::kDirtyBitsetWords; ++w) {
        uint64_t mask = words[w];
        while (mask != 0ULL) {
            unsigned long bitPos = 0u;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
            _BitScanForward64(&bitPos, mask);
#else
            while (((mask >> bitPos) & 1ULL) == 0ULL) { ++bitPos; }
#endif
            const std::size_t idx = (w << 6u) + static_cast<std::size_t>(bitPos);
            mask &= (mask - 1ULL);

            if (idx >= vf50::params::kVoiceStart && idx < vf50::params::kVoiceEnd) {
                bool maskOrLfo2Only = true;
                applySingleVoiceParamEdit(idx, audioMemory_.singleEditSlot().voice, maskOrLfo2Only);
                if (!maskOrLfo2Only) {
                    voiceStructEdited = true;
                }
            } else if (idx >= vf50::params::kPerfStart && idx < vf50::params::kPerfEnd) {
                applyPerfParamEdit(idx, audioMemory_.perfEditSlot());
                perfStructEdited = true;
            } else if (idx >= vf50::params::kRhythmStart && idx < vf50::params::kRhythmEnd) {
                applyRhythmParamEdit(idx, restartRhythmNeeded);
                memoryMutated = true;
            } else if (idx >= vf50::params::kEffectsStart && idx < vf50::params::kEffectsEnd) {
                applyEffectParamEdit(idx, activeFx, fxStructEdited);
            }
        }
    }

    if (voiceStructEdited) {
        audioMemory_.cancelSingleCompare();
        audioMemory_.singleEditSlot().repackRomBytes();
        audioMemory_.markSingleEditDirty();
        if (audioMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Single) {
            synth_.fm().setActiveSingleVoiceDirect(audioMemory_.activeSingleVoice());
        }
        memoryMutated = true;
    }

    if (perfStructEdited) {
        audioMemory_.cancelPerformanceCompare();
        audioMemory_.perfEditSlot().repackRomBytes();
        audioMemory_.markPerfEditDirty();
        if (audioMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Performance) {
            synth_.fm().setActivePerformanceDirect(audioMemory_.activePerformance());
        } else if (synth_.demoPlayer().isPlaying()
                   && synth_.fm().playMode() == vf50::dsp::fm::SynthPlayMode::Performance) {
            auto& livePerf = synth_.fm().mutableActivePerformance();
            const auto& editedPerf = audioMemory_.activePerformance();
            for (std::size_t p = 0u; p < 8u; ++p) {
                livePerf.instruments[p].volume       = editedPerf.instruments[p].volume;
                livePerf.instruments[p].outAssign    = editedPerf.instruments[p].outAssign;
                livePerf.instruments[p].detune       = editedPerf.instruments[p].detune;
                livePerf.instruments[p].noteShift    = editedPerf.instruments[p].noteShift;
                livePerf.instruments[p].maxNotes     = editedPerf.instruments[p].maxNotes;
                livePerf.instruments[p].reserveNotes = editedPerf.instruments[p].reserveNotes;
            }
            synth_.fm().notifyLivePerformanceParametersChanged();
        }
        memoryMutated = true;
    }

    if (fxStructEdited) {
        audioMemory_.updateActiveEffectSettings(activeFx);
        synth_.effects().setSettings(activeFx);
        memoryMutated = true;
    }

    if (restartRhythmNeeded) {
        updateRhythmTransportRealTime(true);
    }

    activeWsMode_.store(static_cast<uint8_t>(audioMemory_.workstationMode()), std::memory_order_relaxed);
    activeVoiceBank_.store(static_cast<uint8_t>(audioMemory_.selectedVoiceBank()), std::memory_order_relaxed);
    activePerfBank_.store(static_cast<uint8_t>(audioMemory_.selectedPerfBank()), std::memory_order_relaxed);

    if (memoryMutated &&
        !newPatchPending.load(std::memory_order_acquire) &&
        !newPerfPatchPending_.load(std::memory_order_acquire) &&
        pendingHostSlot_.load(std::memory_order_acquire) < 0) {
        const int slot = nextAudioWriteSlot_ & 1;
        audioToHostSlots_[static_cast<std::size_t>(slot)] = audioMemory_;
        pendingAudioSlot_.store(slot, std::memory_order_release);
        nextAudioWriteSlot_ ^= 1;
    }
}

void VF50AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    auto mainBus = getBusBuffer(buffer, false, 0);
    if (numSamples <= 0 || mainBus.getNumChannels() < 2 || mainBus.getNumSamples() < numSamples) {
        buffer.clear();
        return;
    }
    for (int ch = 2; ch < mainBus.getNumChannels(); ++ch) {
        mainBus.clear(ch, 0, numSamples);
    }

    vf50::dsp::MultiBusOutputSpec busSpec{};
    busSpec.mainL = mainBus.getWritePointer(0);
    busSpec.mainR = mainBus.getWritePointer(1);
    const int totalOutBuses = std::min(10, getBusCount(false));
    for (int b = 1; b < totalOutBuses; ++b) {
        if (const auto* auxInfo = getBus(false, b); auxInfo != nullptr && auxInfo->isEnabled()) {
            auto auxBuf = getBusBuffer(buffer, false, b);
            if (auxBuf.getNumChannels() >= 2 && auxBuf.getNumSamples() >= numSamples) {
                const auto chIdx = static_cast<std::size_t>(b - 1);
                busSpec.auxL[chIdx] = auxBuf.getWritePointer(0);
                busSpec.auxR[chIdx] = auxBuf.getWritePointer(1);
                busSpec.hostEnabledAuxMask |= static_cast<uint16_t>(uint16_t{ 1u } << chIdx);
            }
        }
    }
#else
    const int numOutCh   = buffer.getNumChannels();
    if (numOutCh < 2 || numSamples <= 0) {
        buffer.clear();
        return;
    }

    for (int ch = 2; ch < numOutCh; ++ch) {
        buffer.clear(ch, 0, numSamples);
    }
#endif

    // 0. Wait-free check for newly imported/updated 8-slot User Sequencer bank
    const int seqBank = pendingSeqBank_.exchange(-1, std::memory_order_acq_rel);
    if (seqBank >= 0 && seqBank < 2) {
        for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
            const auto* src = stagingSeqBanks_[static_cast<std::size_t>(seqBank)][s].get();
            if (src != nullptr && src->valid && src->eventCount > 0u) {
                synth_.demoPlayer().loadUserSequenceSlot(s, *src);
            } else {
                synth_.demoPlayer().clearUserSequenceSlot(s);
            }
        }
    }

    // 0b. Wait-free runtime ROM bank swap (before staged Host Memory apply)
    if (const auto* newRom = pendingRomImagePtr_.exchange(nullptr, std::memory_order_acq_rel)) {
        const bool reloadDefaults = pendingRomReloadDefaults_.load(std::memory_order_acquire);
        synth_.setRuntimeRomBank(newRom, reloadDefaults);
        audioMemory_.setRuntimeRomBank(newRom, false);
        activeRhythmKeyLayout_.store(synth_.drums().activeKeyAssignLayout(), std::memory_order_relaxed);
    }

    // 1. Wait-free check for staged DAW / Host Memory Bank load
    const int stagedSlot = pendingHostSlot_.exchange(-1, std::memory_order_acq_rel);
    if (stagedSlot >= 0 && stagedSlot < 2) {
        if (synth_.demoPlayer().isPlaying()) {
            synth_.demoPlayer().notifyExternalPatchOrModeSwitch(synth_.fm());
        }
        audioMemory_ = hostToAudioSlots_[static_cast<std::size_t>(stagedSlot)];
        audioMemory_.applyToSynthEngine(synth_, true);
        activeRhythmKeyLayout_.store(synth_.drums().activeKeyAssignLayout(), std::memory_order_relaxed);
        applyGlobalRuntimeParamsFromAtomics();
    }

    // 1a. Atomic lock-free patch swap before any voice rendering occurs
    if (newPatchPending.load(std::memory_order_acquire)) {
        if (newPatchPending.exchange(false, std::memory_order_acq_rel)) {
            const uint32_t slotIdx = activeStagedPatchSlot_.load(std::memory_order_acquire) & 1u;
            applyStagedVoicePatchRealTime(stagedPatchSlots_[slotIdx]);
        }
    }
    if (newPerfPatchPending_.load(std::memory_order_acquire)) {
        if (newPerfPatchPending_.exchange(false, std::memory_order_acq_rel)) {
            const uint32_t slotIdx = activeStagedPerfSlot_.load(std::memory_order_acquire) & 1u;
            applyStagedPerfPatchRealTime(stagedPerfSlots_[slotIdx]);
        }
    }

    // 1a. Wait-free Global Utility Transpose & Rhythm Key Assign Layout updates
    synth_.fm().setGlobalTransposeSemitones(globalTransposeSemitones_.load(std::memory_order_relaxed));
    const int reqKeyLayout = pendingRhythmKeyLayout_.exchange(-1, std::memory_order_acq_rel);
    if (reqKeyLayout >= 0 && reqKeyLayout <= 3) {
        audioMemory_.selectRhythmKeyMap(static_cast<std::size_t>(reqKeyLayout));
        synth_.drums().applyKeyAssignLayout(static_cast<uint8_t>(reqKeyLayout));
        activeRhythmKeyLayout_.store(static_cast<uint8_t>(reqKeyLayout), std::memory_order_relaxed);

        const int slot = nextAudioWriteSlot_ & 1;
        audioToHostSlots_[static_cast<std::size_t>(slot)] = audioMemory_;
        pendingAudioSlot_.store(slot, std::memory_order_release);
        nextAudioWriteSlot_ ^= 1;
    }

    // 1a-rhy. Wait-free Classic Rhythm Machine commands & kit updates
    processClassicRhythmCmdsRealTime();

    // 1b. Wait-free Demo Song transport command dispatch
    synth_.demoPlayer().setChainAllFactorySongs(demoChainAll_.load(std::memory_order_relaxed));
    const uint8_t demoCmd = pendingDemoCmd_.exchange(0u, std::memory_order_acq_rel);
    if (demoCmd != 0u) {
        auto& dp = synth_.demoPlayer();
        const std::size_t reqIdx = requestedDemoSongIdx_.load(std::memory_order_relaxed);
        auto syncRhythmAfterStart = [this, &dp]() noexcept {
            if (dp.isPlaying() && dp.needsAccompanyingRhythmPattern()) {
                // Always start slaved to master sequencer clock (output-gated if RHY is already muted!)
                startAccompanyingRhythmPatternForSong(static_cast<double>(dp.currentTempoBpmInt()));
                if (dp.currentTickPosition() > 0.5) {
                    synth_.drums().seekSlavedToMasterTick(dp.currentTickPosition());
                }
            } else if (loadBool(paramBank_.rhythmPlay)) {
                updateRhythmTransportRealTime(true);
            }
        };
        auto syncRhythmAfterStop = [this]() noexcept {
            if (loadBool(paramBank_.rhythmPlay)) {
                updateRhythmTransportRealTime(true);
            }
        };

        if (demoCmd == 1u) {
            dp.startSong(reqIdx, synth_.fm(), synth_.drums(), synth_.effects());
            syncRhythmAfterStart();
        } else if (demoCmd == 2u) {
            const bool wasPlaying = dp.isPlaying();
            dp.stopSong(synth_.fm(), synth_.drums(), synth_.effects(), true, !wasPlaying);
            if (wasPlaying) {
                audioMemory_.applyActiveSelectionToSynthEngine(synth_, true);
            }
            syncRhythmAfterStop();
        } else if (demoCmd == 3u) {
            if (dp.isPlaying()) {
                dp.stopSong(synth_.fm(), synth_.drums(), synth_.effects(), true, false);
                audioMemory_.applyActiveSelectionToSynthEngine(synth_, true);
                syncRhythmAfterStop();
            } else {
                dp.startSong(dp.selectedSongIndex(), synth_.fm(), synth_.drums(), synth_.effects());
                syncRhythmAfterStart();
            }
        } else if (demoCmd == 4u) {
            if (dp.isPlaying()) {
                dp.startSong(reqIdx, synth_.fm(), synth_.drums(), synth_.effects());
                syncRhythmAfterStart();
            } else {
                dp.selectSongIndex(reqIdx, false);
            }
        } else if (demoCmd == 5u) {
            const bool wasPlaying = dp.isPlaying();
            const bool standaloneRhythmRunning = !synth_.drums().isSlavedToSequencer()
                                              && (synth_.drums().isSongPlaying() || synth_.drums().isPatternPlaying());
            if (standaloneRhythmRunning) {
                const bool prevSuppress = dp.isSuppressingInternalDrums();
                dp.setSuppressInternalDrums(true);
                dp.rewindToTop(synth_.fm(), synth_.drums(), synth_.effects());
                dp.setSuppressInternalDrums(prevSuppress);
            } else {
                dp.rewindToTop(synth_.fm(), synth_.drums(), synth_.effects());
                synth_.drums().rewindPattern();
            }
            if (wasPlaying) {
                audioMemory_.applyActiveSelectionToSynthEngine(synth_, true);
            }
            syncRhythmAfterStop();
        }
    }

    const int seekBars = pendingDemoSeekBars_.exchange(0, std::memory_order_acq_rel);
    if (seekBars != 0) {
        synth_.demoPlayer().seekByMeasures(seekBars, synth_.fm(), &synth_.drums());
    }

    const int reqMeas = pendingDemoTargetMeasure_.exchange(0, std::memory_order_acq_rel);
    if (reqMeas > 0) {
        synth_.demoPlayer().seekToMeasureOneBased(reqMeas, synth_.fm(), &synth_.drums());
    }

    const int reqTempo = pendingDemoTempoBpm_.exchange(0, std::memory_order_acq_rel);
    if (reqTempo > 0) {
        synth_.demoPlayer().setTempoOverrideBpm(static_cast<double>(reqTempo));
        if (synth_.demoPlayer().needsAccompanyingRhythmPattern()) {
            synth_.drums().setTempoBpm(static_cast<double>(reqTempo));
        }
    }

    synth_.demoPlayer().syncTrackMuteTransitions(synth_.fm(), synth_.drums());

    // 1c. Wait-free Classic System Settings, Performance Effects & Program Change Table sync
    const int classicSysSlot = pendingClassicSysSlot_.exchange(-1, std::memory_order_acq_rel);
    if (classicSysSlot >= 0 && classicSysSlot < 2 && classicAudioSysSlots_[static_cast<std::size_t>(classicSysSlot)] != nullptr) {
        *classicAudioSys_ = *classicAudioSysSlots_[static_cast<std::size_t>(classicSysSlot)];
    }
    if (classicAudioSys_ != nullptr) {
        const auto& csys = classicAudioSys_->sys;
        sysExHandler_.setDeviceEnabled(csys.deviceNumber != 0u);
        sysExHandler_.setDeviceChannel((csys.deviceNumber >= 17u) ? 16u : static_cast<uint8_t>(csys.deviceNumber - 1u));
        synth_.setSynthVolume(csys.synthVolume);
        synth_.fm().setMidiSwitchModes(csys.controlChangeMode, csys.aftertouchMode, csys.pitchBendMode);
        synth_.fm().setEgDampSpeed(csys.egDampSpeed);
        synth_.fm().setVoiceDampOn(csys.voiceDampOn);
        synth_.fm().setPerfEffectBank(classicAudioSys_->pefct);
        synth_.fm().setSingleReceiveChannel(csys.basicReceiveCh);
        synth_.fm().setMasterTuneCents(static_cast<double>(csys.masterTuningSteps) * (100.0 / 64.0));
    }
    synth_.fm().setAuditionPerfEffect(classicAuditionPef_.load(std::memory_order_relaxed));
    synth_.fm().setAuditionMicroTuneMode(classicAuditionMicro_.load(std::memory_order_relaxed));

    // 1d. Sequencer & Rhythm DAW Sync via juce::AudioPlayHead (lock-free, zero allocations)
    const bool allowRhythmDawSync = classicRhythmSyncMidi_.load(std::memory_order_relaxed);
    if (dawSyncEnabled_.load(std::memory_order_relaxed)) {
        if (auto* hostPlayHead = getPlayHead()) {
            if (const auto pos = hostPlayHead->getPosition()) {
                double hostBpm = static_cast<double>(synth_.demoPlayer().currentTempoBpmInt());
                if (const auto bpmOpt = pos->getBpm(); bpmOpt.hasValue() && *bpmOpt >= 20.0 && *bpmOpt <= 300.0) {
                    hostBpm = *bpmOpt;
                    if (allowRhythmDawSync) {
                        synth_.drums().setTempoBpm(hostBpm);
                    }
                }

                const bool hostPlaying = pos->getIsPlaying();
                const double hostPpq   = pos->getPpqPosition().orFallback(0.0);

                if (hostPlaying && !prevHostPlaying_) {
                    if (dawArmSequencer_.load(std::memory_order_relaxed) && !synth_.demoPlayer().isPlaying()) {
                        synth_.demoPlayer().startSong(synth_.demoPlayer().selectedSongIndex(),
                                                      synth_.fm(), synth_.drums(), synth_.effects());
                        if (synth_.demoPlayer().isPlaying() && synth_.demoPlayer().needsAccompanyingRhythmPattern()) {
                            startAccompanyingRhythmPatternForSong(
                                static_cast<double>(synth_.demoPlayer().currentTempoBpmInt()));
                        }
                        dawTriggeredDemo_ = true;
                    }
                } else if (!hostPlaying && prevHostPlaying_) {
                    if ((dawTriggeredDemo_ || dawArmSequencer_.load(std::memory_order_relaxed))
                        && synth_.demoPlayer().isPlaying()) {
                        synth_.demoPlayer().stopSong(synth_.fm(), synth_.drums(), synth_.effects(), true);
                        audioMemory_.applyActiveSelectionToSynthEngine(synth_, true);
                        if (loadBool(paramBank_.rhythmPlay)) {
                            updateRhythmTransportRealTime(true);
                        }
                        dawTriggeredDemo_ = false;
                    }
                }

                if (hostPlaying) {
                    if (synth_.demoPlayer().isPlaying()) {
                        synth_.demoPlayer().syncToHostPlayHead(hostPpq, hostBpm, synth_.fm(), synth_.drums());
                    }
                    if (allowRhythmDawSync && synth_.drums().isPatternPlaying() && !synth_.drums().isSlavedToSequencer()) {
                        synth_.drums().syncPatternToPpq(hostPpq, hostBpm);
                    }
                }
                prevHostPlaying_ = hostPlaying;
            }
        }
    }

    // 2. Wait-free atomic drain of dirty APVTS parameters
    if (anyParamDirty_.exchange(false, std::memory_order_acquire)) {
        processDirtyParametersRealTime();
    }

    {
        const int kbdOv = classicRhythmKbdOverride_.load(std::memory_order_acquire);
        if (kbdOv >= 0) {
            synth_.setRhythmKeyboardMode(kbdOv == 1);
        }
    }

    // Separate local virtual keyboard events from external MIDI IN (for Local on/off & odd/even Note filter)
    localKbdMidiBuf_.clear();
    keyboardState_.processNextMidiBuffer(localKbdMidiBuf_, 0, numSamples, true);
    keyboardState_.processNextMidiBuffer(midiMessages, 0, numSamples, false);

    // Drain lock-free UI MIDI events (Pitch Wheel, Mod Wheel, Demo/Audition triggers)
    {
        uint32_t r = uiMidiReadIdx_.load(std::memory_order_relaxed);
        const uint32_t w = uiMidiWriteIdx_.load(std::memory_order_acquire);
        while (r != w) {
            const auto& ev = uiMidiQueue_[r & static_cast<uint32_t>(kUiMidiQueueSize - 1u)];
            if (ev.len > 0u) {
                localKbdMidiBuf_.addEvent(ev.bytes.data(), static_cast<int>(ev.len), 0);
            }
            ++r;
        }
        uiMidiReadIdx_.store(r, std::memory_order_release);
    }

    sysExReplyBuffer_.clear();
    bool sysExModifiedMemory = false;

    const auto& csysRef = classicAudioSys_->sys;
    const bool classicMidiOn   = csysRef.midiOn;
    const bool classicLocalOn  = csysRef.localOn;

    // Build merged MIDI buffer for audio rendering while echoing local keyboard MIDI out when Midi=on
    mergedMidiBuf_.clear();
    for (const auto metadata : midiMessages) {
        if (metadata.numBytes <= 0 || metadata.data == nullptr) continue;
        const uint8_t st0 = metadata.data[0];
        if (!classicMidiOn && st0 < 0xF1u) {
            continue; // Spec §2.1 (p. 99): MIDI off blocks channel/SysEx messages while leaving Realtime/Common
        }
        if ((st0 & 0xF0u) != 0xF0u) {
            const uint8_t msgType = static_cast<uint8_t>(st0 & 0xF0u);
            if ((msgType == 0x80u || msgType == 0x90u) && metadata.numBytes >= 3) {
                const uint8_t note = static_cast<uint8_t>(metadata.data[1] & 0x7Fu);
                if (csysRef.noteOnOffFilter == 1u && ((note & 1u) == 0u)) continue; // odd only
                if (csysRef.noteOnOffFilter == 2u && ((note & 1u) != 0u)) continue; // even only
                if (msgType == 0x90u && (metadata.data[2] & 0x7Fu) > 0u) {
                    const uint8_t vel = vf50::dsp::VF50SynthEngine::transformNoteVelocity(
                        metadata.data[2] & 0x7Fu, csysRef.fixedVelocity, csysRef.velocityCurve);
                    const uint8_t remapped[3] = { st0, note, vel };
                    mergedMidiBuf_.addEvent(remapped, 3, metadata.samplePosition);
                    continue;
                }
            }
        }
        mergedMidiBuf_.addEvent(metadata.data, metadata.numBytes, metadata.samplePosition);
    }

    // Track local keyboard events to echo out if Midi=on (for Local off external routing)
    bool hasLocalOutEcho = false;
    if (csysRef.midiOn && !localKbdMidiBuf_.isEmpty()) {
        hasLocalOutEcho = true;
    }

    if (classicLocalOn) {
        for (const auto metadata : localKbdMidiBuf_) {
            if (metadata.numBytes <= 0 || metadata.data == nullptr) continue;
            const uint8_t st0 = metadata.data[0];
            const uint8_t msgType = static_cast<uint8_t>(st0 & 0xF0u);
            if (msgType == 0x90u && metadata.numBytes >= 3 && (metadata.data[2] & 0x7Fu) > 0u) {
                const uint8_t vel = vf50::dsp::VF50SynthEngine::transformNoteVelocity(
                    metadata.data[2] & 0x7Fu, csysRef.fixedVelocity, csysRef.velocityCurve);
                const uint8_t remapped[3] = { st0, static_cast<uint8_t>(metadata.data[1] & 0x7Fu), vel };
                mergedMidiBuf_.addEvent(remapped, 3, metadata.samplePosition);
            } else {
                mergedMidiBuf_.addEvent(metadata.data, metadata.numBytes, metadata.samplePosition);
            }
        }
    }

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    float* outL = busSpec.mainL;
    float* outR = busSpec.mainR;

    auto renderSubSliced = [this, &busSpec](int startSample, int endSample) noexcept {
        int pos = startSample;
        while (pos < endSample) {
            const int chunk = std::min(endSample - pos, kMaxSubBlockSamples);
            vf50::dsp::MultiBusOutputSpec subSpec{};
            subSpec.mainL = busSpec.mainL + pos;
            subSpec.mainR = busSpec.mainR + pos;
            subSpec.hostEnabledAuxMask = busSpec.hostEnabledAuxMask;
            if (subSpec.hostEnabledAuxMask != 0u) {
                for (std::size_t ch = 0u; ch < 9u; ++ch) {
                    if (((subSpec.hostEnabledAuxMask >> ch) & 1u) != 0u) {
                        subSpec.auxL[ch] = busSpec.auxL[ch] + pos;
                        subSpec.auxR[ch] = busSpec.auxR[ch] + pos;
                    }
                }
            }
            synth_.renderMultiBusBlock(subSpec, static_cast<std::size_t>(chunk));
            pos += chunk;
        }
    };
#else
    float* outL = buffer.getWritePointer(0);
    float* outR = buffer.getWritePointer(1);

    auto renderSubSliced = [this, outL, outR](int startSample, int endSample) noexcept {
        int pos = startSample;
        while (pos < endSample) {
            const int chunk = std::min(endSample - pos, kMaxSubBlockSamples);
            synth_.renderStereoBlock(outL + pos, outR + pos, static_cast<std::size_t>(chunk));
            pos += chunk;
        }
    };
#endif

    int renderedOffset = 0;
    for (const auto metadata : mergedMidiBuf_) {
        const int eventSample = std::clamp(metadata.samplePosition, 0, numSamples);
        if (eventSample > renderedOffset) {
            renderSubSliced(renderedOffset, eventSample);
            renderedOffset = eventSample;
        }

        if (metadata.numBytes > 0 && metadata.data != nullptr) {
            if (metadata.data[0] == vf50::midi::VF50SysExHandler::kSysExStart) {
                // Spec §2.5 (p. 133 §4.5-4.6): block incoming SEQ/RHYTHM bulk dumps during active playback
                bool blockedBySeqRhyPlayback = false;
                if (synth_.demoPlayer().isPlaying() || synth_.drums().isPatternPlaying() || synth_.drums().isSongPlaying()) {
                    if (metadata.numBytes >= 16 && metadata.data[3] == 0x7Eu) {
                        const uint8_t* lm = metadata.data + 6;
                        if (std::memcmp(lm, "LM  8952SQ", 10u) == 0 || std::memcmp(lm, "LM  8952RS", 10u) == 0
                            || std::memcmp(lm, "LM  8073RS", 10u) == 0 || std::memcmp(lm, "LM  8073RI", 10u) == 0
                            || std::memcmp(lm, "LM  8073K0", 10u) == 0 || std::memcmp(lm, "LM  8073K1", 10u) == 0) {
                            blockedBySeqRhyPlayback = true;
                        }
                    }
                }
                if (!blockedBySeqRhyPlayback) {
                    const auto res = sysExHandler_.processSysEx(
                        metadata.data,
                        static_cast<std::size_t>(metadata.numBytes),
                        audioMemory_,
                        synth_,
                        &sysExReplyBuffer_);
                    if (res.modifiedSingleEdit || res.modifiedUserVoices ||
                        res.modifiedPerfEdit || res.modifiedUserPerfs ||
                        res.modifiedMicrotuning || res.modifiedRhythm) {
                        sysExModifiedMemory = true;
                    }
                }
            } else {
                const uint8_t st = static_cast<uint8_t>(metadata.data[0] & 0xF0u);
                const uint8_t ch = static_cast<uint8_t>(metadata.data[0] & 0x0Fu);
                if (st == 0xE0u && metadata.numBytes >= 3) {
                    const int bend14 = static_cast<int>(metadata.data[1] & 0x7Fu)
                                     | (static_cast<int>(metadata.data[2] & 0x7Fu) << 7);
                    livePitchBendNorm_.store(std::clamp((static_cast<float>(bend14) - 8192.0f) / 8192.0f, -1.0f, 1.0f),
                                             std::memory_order_relaxed);
                } else if (st == 0xB0u && metadata.numBytes >= 3 && (metadata.data[1] & 0x7Fu) == 1u) {
                    liveModWheelNorm_.store(static_cast<float>(metadata.data[2] & 0x7Fu) / 127.0f,
                                            std::memory_order_relaxed);
                }
                if (st == 0x90u && metadata.numBytes >= 3 && (metadata.data[2] & 0x7Fu) > 0u) {
                    if (synth_.isRhythmKeyboardMode() || classicCaptureRhythmNotes_.load(std::memory_order_relaxed)) {
                        const uint8_t note = static_cast<uint8_t>(metadata.data[1] & 0x7Fu);
                        const uint8_t vel  = static_cast<uint8_t>(metadata.data[2] & 0x7Fu);
                        const uint32_t w = rhyUiNoteWrite_.load(std::memory_order_relaxed);
                        const uint32_t r = rhyUiNoteRead_.load(std::memory_order_acquire);
                        if ((w - r) < static_cast<uint32_t>(kRhythmUiNoteQueueSize)) {
                            auto& slot = rhyUiNoteQueue_[w & static_cast<uint32_t>(kRhythmUiNoteQueueSize - 1u)];
                            slot.midiNote    = note;
                            slot.velocity    = vel;
                            slot.drumIndex   = static_cast<int8_t>(synth_.drums().drumForMidiNote(note));
                            slot.patternTick = synth_.drums().currentPatternTickInt();
                            rhyUiNoteWrite_.store(w + 1u, std::memory_order_release);
                        }
                    }
                }
                if (st == 0xC0u && metadata.numBytes >= 2) {
                    const uint8_t pg = static_cast<uint8_t>(metadata.data[1] & 0x7Fu);
                    const uint8_t pcMode = csysRef.pgmChangeMode;
                    if (pcMode == 0u) {
                        continue; // Pgm Change = off
                    }
                    if (pcMode == 1u) { // common: obeys BasicRch + Program Change Table
                        if (csysRef.basicReceiveCh >= 16u || csysRef.basicReceiveCh == ch) {
                            const auto& entry = classicAudioSys_->pct.entries[pg];
                            if (entry.isPerformance) {
                                const auto mb = (entry.bank == 2u) ? vf50::state::MemoryBank::FactoryRom : vf50::state::MemoryBank::UserRam;
                                audioMemory_.setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
                                audioMemory_.selectPerformance(mb, entry.number);
                                if (entry.bank == 1u) {
                                    audioMemory_.perfEditSlot().setFromPerformance(classicAudioSys_->cardPerfs[entry.number]);
                                }
                                synth_.applyAtomicPerformancePatch(audioMemory_.activePerformance(), entry.number,
                                                                   mb == vf50::state::MemoryBank::UserRam,
                                                                   audioMemory_.activePerformancePartPans(), true);
                                classicCardBankActive_.store(entry.bank == 1u, std::memory_order_relaxed);
                            } else {
                                const auto mb = (entry.bank == 2u) ? vf50::state::MemoryBank::FactoryRom : vf50::state::MemoryBank::UserRam;
                                audioMemory_.setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
                                audioMemory_.selectSingleVoice(mb, entry.number);
                                if (entry.bank == 1u) {
                                    audioMemory_.singleEditSlot().setFromVoice(classicAudioSys_->cardVoices[entry.number]);
                                }
                                synth_.applyAtomicSingleVoicePatch(audioMemory_.activeSingleVoice(), entry.number,
                                                                   mb == vf50::state::MemoryBank::UserRam,
                                                                   { 0, 0, 0, 0 }, 0x0Fu, true, true, csysRef.voiceDampOn);
                                classicCardBankActive_.store(entry.bank == 1u, std::memory_order_relaxed);
                            }
                            sysExModifiedMemory = true;
                        }
                        continue;
                    }
                    if (pcMode == 2u || pcMode == 4u) { // individual / TransFilter
                        if (audioMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Performance) {
                            const auto& entry = classicAudioSys_->pct.entries[pg];
                            if (!entry.isPerformance) { // Spec p. 102 Note: perf entries ignored in individual mode
                                const bool isPreset = (entry.bank == 2u);
                                for (auto& inst : audioMemory_.perfEditSlot().perf.instruments) {
                                    if (inst.receiveChannel == ch || inst.receiveChannel == 16u) {
                                        inst.voiceBank   = isPreset ? 1u : 0u;
                                        inst.voiceNumber = entry.number;
                                    }
                                }
                                if (entry.bank == 1u) {
                                    audioMemory_.storeUserVoice(entry.number, classicAudioSys_->cardVoices[entry.number]);
                                    synth_.fm().storeInternalVoice(entry.number, classicAudioSys_->cardVoices[entry.number]);
                                }
                                synth_.fm().setPerformanceChannelVoice(ch, isPreset, entry.number);
                                sysExModifiedMemory = true;
                            }
                        } else if (csysRef.basicReceiveCh >= 16u || csysRef.basicReceiveCh == ch) {
                            const auto& entry = classicAudioSys_->pct.entries[pg];
                            if (!entry.isPerformance) {
                                const auto mb = (entry.bank == 2u) ? vf50::state::MemoryBank::FactoryRom : vf50::state::MemoryBank::UserRam;
                                audioMemory_.selectSingleVoice(mb, entry.number);
                                if (entry.bank == 1u) {
                                    audioMemory_.singleEditSlot().setFromVoice(classicAudioSys_->cardVoices[entry.number]);
                                }
                                synth_.applyAtomicSingleVoicePatch(audioMemory_.activeSingleVoice(), entry.number,
                                                                   mb == vf50::state::MemoryBank::UserRam,
                                                                   { 0, 0, 0, 0 }, 0x0Fu, true, true, csysRef.voiceDampOn);
                                sysExModifiedMemory = true;
                            }
                        }
                        continue;
                    }
                    if (pcMode == 3u) { // direct
                        if (audioMemory_.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Performance) {
                            const uint8_t vn = static_cast<uint8_t>(pg % 100u);
                            for (auto& inst : audioMemory_.perfEditSlot().perf.instruments) {
                                if (inst.receiveChannel == ch || inst.receiveChannel == 16u) {
                                    inst.voiceNumber = vn;
                                }
                            }
                            synth_.fm().setPerformanceChannelVoice(ch, !classicCardBankActive_.load(std::memory_order_relaxed), vn);
                            sysExModifiedMemory = true;
                            continue;
                        }
                    }
                }
                synth_.handleRawMidiMessage(metadata.data, static_cast<std::size_t>(metadata.numBytes));
            }
        }
    }

    if (renderedOffset < numSamples) {
        renderSubSliced(renderedOffset, numSamples);
    }

    // Emit any requested SysEx dumps generated into pre-allocated sysExReplyBuffer_
    midiMessages.clear();
    if (classicMidiOn && sysExReplyBuffer_.packetCount > 0u) {
        for (std::size_t i = 0; i < sysExReplyBuffer_.packetCount; ++i) {
            midiMessages.addEvent(sysExReplyBuffer_.packetData(i),
                                  static_cast<int>(sysExReplyBuffer_.packetSize(i)),
                                  0);
        }
    }

    // Emit any queued Classic UT DUMP) bulk SysEx packets
    const int bulkSlot = pendingBulkSysExSlot_.exchange(-1, std::memory_order_acq_rel);
    if (bulkSlot >= 0 && bulkSlot < 2 && bulkSysExOutSlots_[static_cast<std::size_t>(bulkSlot)] != nullptr) {
        const auto& bbuf = *bulkSysExOutSlots_[static_cast<std::size_t>(bulkSlot)];
        if (classicMidiOn) {
            for (std::size_t i = 0u; i < bbuf.packetCount; ++i) {
                midiMessages.addEvent(bbuf.packetData(i), static_cast<int>(bbuf.packetSize(i)), 0);
            }
        }
    }

    // Emit any queued short MIDI OUT events (Direct PC Transmit, Data Entry CC, etc.)
    {
        uint32_t r = midiOutReadIdx_.load(std::memory_order_relaxed);
        const uint32_t w = midiOutWriteIdx_.load(std::memory_order_acquire);
        while (r != w) {
            const auto& ev = midiOutQueue_[r & static_cast<uint32_t>(kMidiOutQueueSize - 1u)];
            if (classicMidiOn && ev.len > 0u) {
                midiMessages.addEvent(ev.bytes.data(), static_cast<int>(ev.len), 0);
            }
            ++r;
        }
        midiOutReadIdx_.store(r, std::memory_order_release);
    }

    // Echo local virtual keyboard notes to MIDI OUT when Classic Midi=on (including Local=off)
    if (hasLocalOutEcho) {
        const uint8_t txCh = static_cast<uint8_t>(csysRef.transmitCh & 0x0Fu);
        for (const auto metadata : localKbdMidiBuf_) {
            if (metadata.numBytes > 0 && metadata.data != nullptr) {
                const uint8_t st = static_cast<uint8_t>(metadata.data[0] & 0xF0u);
                if (st == 0x80u || st == 0x90u) {
                    uint8_t msg[3]{
                        static_cast<uint8_t>(st | txCh),
                        static_cast<uint8_t>(metadata.data[1] & 0x7Fu),
                        (metadata.numBytes >= 3) ? static_cast<uint8_t>(metadata.data[2] & 0x7Fu) : uint8_t{ 0u }
                    };
                    if (st == 0x90u && msg[2] > 0u) {
                        msg[2] = vf50::dsp::VF50SynthEngine::transformNoteVelocity(
                            msg[2], csysRef.fixedVelocity, csysRef.velocityCurve);
                    }
                    midiMessages.addEvent(msg, 3, metadata.samplePosition);
                }
            }
        }
    }

    {
        std::array<std::pair<uint8_t, uint8_t>, 64> muteNoteOffs{};
        const std::size_t cnt = synth_.demoPlayer().consumePendingMuteNoteOffs(
            muteNoteOffs.data(), muteNoteOffs.size());
        for (std::size_t i = 0u; i < cnt; ++i) {
            const uint8_t ch0 = static_cast<uint8_t>(muteNoteOffs[i].first & 0x0Fu);
            const uint8_t noteByte = muteNoteOffs[i].second;
            if (noteByte == 0xFFu) {
                const uint8_t msg[3] = { static_cast<uint8_t>(0xB0u | ch0), 123u, 0u };
                midiMessages.addEvent(msg, 3, 0);
            } else {
                const uint8_t msg[3] = { static_cast<uint8_t>(0x80u | ch0), static_cast<uint8_t>(noteByte & 0x7Fu), 0u };
                midiMessages.addEvent(msg, 3, 0);
            }
        }
    }

    if (sysExModifiedMemory) {
        const int slot = nextAudioWriteSlot_ & 1;
        audioToHostSlots_[static_cast<std::size_t>(slot)] = audioMemory_;
        pendingAudioSlot_.store(slot, std::memory_order_release);
        nextAudioWriteSlot_ ^= 1;

        activeWsMode_.store(static_cast<uint8_t>(audioMemory_.workstationMode()), std::memory_order_relaxed);
        activeVoiceBank_.store(static_cast<uint8_t>(audioMemory_.selectedVoiceBank()), std::memory_order_relaxed);
        activePerfBank_.store(static_cast<uint8_t>(audioMemory_.selectedPerfBank()), std::memory_order_relaxed);
        activeRhythmKeyLayout_.store(
            static_cast<uint8_t>(std::min<std::size_t>(audioMemory_.selectedRhythmKeyMap(), 3u)),
            std::memory_order_relaxed);
    }

    // Update lock-free UI telemetry meters
    float peakL = 0.0f;
    float peakR = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        peakL = std::max(peakL, std::abs(outL[i]));
        peakR = std::max(peakR, std::abs(outR[i]));
    }
    const float prevL = meterPeakL_.load(std::memory_order_relaxed) * 0.85f;
    const float prevR = meterPeakR_.load(std::memory_order_relaxed) * 0.85f;
    meterPeakL_.store(std::max(peakL, prevL), std::memory_order_relaxed);
    meterPeakR_.store(std::max(peakR, prevR), std::memory_order_relaxed);
    activeFmVoices_.store(static_cast<int>(synth_.fm().activeVoiceCount()), std::memory_order_relaxed);
    activeDrumVoices_.store(static_cast<int>(synth_.drums().activeVoiceCount()), std::memory_order_relaxed);

    // Update Rhythm Trigger & Demo Song Telemetry Atomics
    const auto drumInfo = synth_.lastTriggeredDrum();
    drumTrigNote_.store(drumInfo.midiNote, std::memory_order_relaxed);
    drumTrigIndex_.store(static_cast<int>(drumInfo.drumIndex), std::memory_order_relaxed);
    drumTrigVel_.store(drumInfo.velocity, std::memory_order_relaxed);
    drumTrigSeq_.store(drumInfo.sequence, std::memory_order_release);

    const auto& drums = synth_.drums();
    rhyVoiceHitMask_.fetch_or(synth_.drums().consumeRecentVoiceHitMask(), std::memory_order_acq_rel);
    rhyActiveVoiceMask_.store(drums.activeVoiceSlotMask(), std::memory_order_relaxed);
    rhyEnginePlaying_.store(drums.isPatternPlaying(), std::memory_order_relaxed);
    rhySongPlaying_.store(drums.isSongPlaying(), std::memory_order_relaxed);
    rhyNextPatternQueued_.store(drums.isNextPatternQueued(), std::memory_order_relaxed);
    rhyCurrentTick_.store(drums.currentPatternTickInt(), std::memory_order_relaxed);
    rhyPatternWrapCount_.store(drums.patternWrapCount(), std::memory_order_relaxed);
    rhySongMeas_.store(drums.currentSongMeasureOneBased(), std::memory_order_relaxed);
    rhySongPart_.store(drums.currentSongPartOneBased(), std::memory_order_relaxed);
    rhySongPtnIsUser_.store(drums.currentSongPtnIsUser(), std::memory_order_relaxed);
    rhySongPtnIdx_.store(drums.currentSongPtnIndex(), std::memory_order_relaxed);
    rhySongBarsPlayed_.store(drums.songBarsPlayedCount(), std::memory_order_relaxed);
    rhyTotalTriggers_.store(drums.totalTriggerCount(), std::memory_order_relaxed);
    rhyLiveTempoBpm_.store(drums.currentTempoBpmInt(), std::memory_order_relaxed);

    const auto& dp = synth_.demoPlayer();
    demoPlaying_.store(dp.isPlaying(), std::memory_order_relaxed);
    demoSongIdx_.store(static_cast<int>(dp.selectedSongIndex()), std::memory_order_relaxed);
    demoTempoBpm_.store(dp.currentTempoBpmInt(), std::memory_order_relaxed);
    demoMeasure_.store(dp.currentMeasureOneBased(), std::memory_order_relaxed);
    demoTotalMeasures_.store(dp.totalMeasures(), std::memory_order_relaxed);
    demoTimeSigNum_.store(static_cast<int>(dp.timeSigNum()), std::memory_order_relaxed);
    demoTimeSigDen_.store(static_cast<int>(dp.timeSigDen()), std::memory_order_relaxed);
    seqTrackDataMask_.store(dp.computeTrackDataMaskForSong(), std::memory_order_relaxed);
    demoKeyMask61_.store(dp.visualKeyMask61(), std::memory_order_relaxed);
    seqDispatchedNotes_.store(dp.dispatchedNoteOnCount(), std::memory_order_relaxed);
    for (std::size_t t = 0u; t < 9u; ++t) {
        seqTrackMeterLevels_[t].store(dp.trackMeterLevel(t), std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* VF50AudioProcessor::createEditor() {
    return new VF50AudioProcessorEditor(*this);
}

int VF50AudioProcessor::getNumPrograms() {
    return static_cast<int>(vf50::state::VF50MemoryManager::kVoiceBankSize);
}

int VF50AudioProcessor::getCurrentProgram() {
    return loadInt(paramBank_.voicePreset, 0, 99);
}

void VF50AudioProcessor::setCurrentProgram(int index) {
    const auto clamped = static_cast<std::size_t>(std::clamp(index, 0, 99));
    const auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread()) {
        const auto bank = (loadInt(paramBank_.voiceBank, 0, 1) == 0)
                              ? vf50::state::MemoryBank::FactoryRom
                              : vf50::state::MemoryBank::UserRam;
        selectSinglePresetAtomic(bank, clamped, true);
    } else {
        vf50::params::setApvtsRawValue(apvts_, vf50::params::ParamIndex::VoicePreset, static_cast<float>(clamped));
    }
}

const juce::String VF50AudioProcessor::getProgramName(int index) {
    pullLatestAudioMemoryToHost();
    const auto clamped = static_cast<std::size_t>(std::clamp(index, 0, 99));
    const auto bank    = hostMemory_.selectedVoiceBank();
    const auto sv      = hostMemory_.getVoice(bank, clamped).name;
    return juce::String(sv.data(), sv.size()).trimEnd();
}

void VF50AudioProcessor::changeProgramName(int index, const juce::String& newName) {
    pullLatestAudioMemoryToHost();
    if (hostMemory_.selectedVoiceBank() == vf50::state::MemoryBank::UserRam) {
        const auto clamped = static_cast<std::size_t>(std::clamp(index, 0, 99));
        vf50::state::StoredVoiceSlot slot(hostMemory_.getUserVoice(clamped));
        slot.setName(newName.toStdString());
        hostMemory_.storeUserVoice(clamped, slot.voice);
        pushHostMemoryToAudio();
    }
}

void VF50AudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    pullLatestAudioMemoryToHost();

    juce::MemoryBlock apvtsXmlBlock;
    const auto treeCopy = apvts_.copyState();
    if (std::unique_ptr<juce::XmlElement> xml = treeCopy.createXml()) {
        copyXmlToBinary(*xml, apvtsXmlBlock);
    }

    std::vector<uint8_t> memBytes;
    hostMemory_.serializeToBinary(memBytes);

    std::vector<uint8_t> container;
    container.reserve(28u + apvtsXmlBlock.getSize() + memBytes.size());
    container.insert(container.end(), std::begin(kStateContainerMagic), std::end(kStateContainerMagic));
    writeU32LE(container, kStateContainerVersion);
    writeU32LE(container, static_cast<uint32_t>(apvtsXmlBlock.getSize()));
    if (apvtsXmlBlock.getSize() > 0u) {
        const auto* xmlPtr = static_cast<const uint8_t*>(apvtsXmlBlock.getData());
        container.insert(container.end(), xmlPtr, xmlPtr + apvtsXmlBlock.getSize());
    }
    writeU32LE(container, static_cast<uint32_t>(memBytes.size()));
    container.insert(container.end(), memBytes.begin(), memBytes.end());

    // 12-byte UI preferences trailer ('VF5U')
    container.insert(container.end(), std::begin(kUiPrefsTrailerMagic), std::end(kUiPrefsTrailerMagic));
    const auto scaleMilli = static_cast<uint32_t>(
        std::clamp(static_cast<int>(std::lround(uiScaleFactor() * 1000.0f)), 750, 2000));
    writeU32LE(container, scaleMilli);
    container.push_back(isVirtualKeyboardVisible() ? 1u : 0u);
    container.push_back(static_cast<uint8_t>(std::clamp(lcdThemeIndex(), 0, 2)));
    container.push_back(static_cast<uint8_t>(static_cast<int8_t>(std::clamp(globalTransposeSemitones(), -24, 24))));
    container.push_back(static_cast<uint8_t>((isIntMemoryProtected() ? 1u : 0u)
                                           | (isCardMemoryProtected() ? 2u : 0u)));

    // 8-byte Workflow & Programmer trailer ('VF5W')
    container.insert(container.end(), std::begin(kWorkflowTrailerMagic), std::end(kWorkflowTrailerMagic));
    container.push_back(static_cast<uint8_t>(workflowMode()));
    container.push_back(isProgrammerVisible() ? 1u : 0u);
    container.push_back(isDawSyncEnabled() ? 1u : 0u);
    container.push_back(isDawSequencerArmed() ? 1u : 0u);

    // Classic Workstation Trailer ('VF5C': SystemSettings, PerfEffectBank, PCT, VirtualCard)
    {
        std::vector<uint8_t> cbuf;
        cbuf.reserve(4096u);
        auto writeSys = [&cbuf](const vf50::state::VF50SystemSettings& s) {
            cbuf.push_back(static_cast<uint8_t>(s.masterTuningSteps));
            cbuf.push_back(s.synthVolume);
            cbuf.push_back(s.combineWithFunction ? 1u : 0u);
            cbuf.push_back(s.controllerReset ? 1u : 0u);
            cbuf.push_back(s.fixedVelocity);
            cbuf.push_back(s.velocityCurve);
            cbuf.push_back(s.egDampSpeed);
            cbuf.push_back(s.voiceDampOn ? 1u : 0u);
            cbuf.push_back(s.midiOn ? 1u : 0u);
            cbuf.push_back(s.basicReceiveCh);
            cbuf.push_back(s.transmitCh);
            cbuf.push_back(s.localOn ? 1u : 0u);
            cbuf.push_back(s.controlChangeMode);
            cbuf.push_back(s.aftertouchMode);
            cbuf.push_back(s.pitchBendMode);
            cbuf.push_back(s.noteOnOffFilter);
            cbuf.push_back(s.dataEntryAssignCc);
            cbuf.push_back(s.pgmChangeMode);
            cbuf.push_back(s.deviceNumber);
            cbuf.insert(cbuf.end(), s.powerOnMessage.begin(), s.powerOnMessage.end());
        };
        auto writePef = [&cbuf](const vf50::state::PerfEffectBank& pb) {
            for (const auto& d : pb.delays) {
                cbuf.push_back(d.delayTimeCs);
                cbuf.push_back(static_cast<uint8_t>(d.pitchShift));
                cbuf.push_back(d.feedback);
                cbuf.push_back(d.effectLevel);
            }
            for (const auto& pn : pb.pans) {
                cbuf.push_back(pn.select);
                cbuf.push_back(pn.direction);
                cbuf.push_back(pn.panRange);
            }
            for (const auto& ch : pb.chords) {
                for (const auto& row : ch.chords) {
                    for (int8_t n : row) {
                        cbuf.push_back(static_cast<uint8_t>(n));
                    }
                }
            }
        };
        auto writePct = [&cbuf](const vf50::state::ProgramChangeTable& pct) {
            for (const auto& e : pct.entries) {
                cbuf.push_back(e.isPerformance ? 1u : 0u);
                cbuf.push_back(e.bank);
                cbuf.push_back(e.number);
            }
        };

        writeSys(classicSysSettings_);
        writePef(perfEffectBank_);
        writePct(pgmChangeTable_);

        if (virtualCard_ != nullptr) {
            cbuf.push_back(static_cast<uint8_t>(virtualCard_->mediaType));
            cbuf.push_back(virtualCard_->hardwareWriteProtect ? 1u : 0u);
            cbuf.push_back(virtualCard_->batteryLowWarning ? 1u : 0u);
            cbuf.push_back(virtualCard_->activeBankIndex);
            for (std::size_t bi = 0u; bi < 2u; ++bi) {
                const auto& bk = virtualCard_->banks[bi];
                cbuf.push_back(static_cast<uint8_t>(bk.format));
                const uint8_t flags = static_cast<uint8_t>(
                    (bk.hasVoicesPerfs ? 1u : 0u)
                    | (bk.hasPct ? 2u : 0u)
                    | (bk.hasPefct ? 4u : 0u)
                    | (bk.hasMct ? 8u : 0u)
                    | (bk.hasSys ? 16u : 0u)
                    | (bk.hasYsSeq ? 32u : 0u));
                cbuf.push_back(flags);
                if (bk.hasVoicesPerfs) {
                    for (const auto& v : bk.voices) {
                        vf50::state::StoredVoiceSlot vc = v;
                        vc.repackRomBytes();
                        cbuf.insert(cbuf.end(), vc.voice.rawRomBytes.begin(), vc.voice.rawRomBytes.end());
                    }
                    for (const auto& pf : bk.performances) {
                        vf50::state::StoredPerformanceSlot pc = pf;
                        pc.repackRomBytes();
                        cbuf.insert(cbuf.end(), pc.perf.rawRomBytes.begin(), pc.perf.rawRomBytes.end());
                    }
                }
                if (bk.hasPct) writePct(bk.pct);
                if (bk.hasPefct) writePef(bk.pefct);
                if (bk.hasMct) {
                    for (const auto& m : bk.microOct) { cbuf.push_back(m.coarseNote); cbuf.push_back(m.fineStep64); }
                    for (const auto& m : bk.microFull) { cbuf.push_back(m.coarseNote); cbuf.push_back(m.fineStep64); }
                }
                if (bk.hasSys) {
                    writeSys(bk.sys);
                    cbuf.push_back(bk.sysIntProtect ? 1u : 0u);
                    cbuf.push_back(bk.sysCardProtect ? 1u : 0u);
                }
                for (std::size_t rs = 0u; rs < 2u; ++rs) {
                    const auto& rset = bk.rhythmSets[rs];
                    const uint8_t rflags = static_cast<uint8_t>(
                        (rset.hasAll ? 1u : 0u) | (rset.hasRSeq ? 2u : 0u) | (rset.hasSetup ? 4u : 0u));
                    cbuf.push_back(rflags);
                    if (rflags != 0u) {
                        cbuf.push_back(rset.rhythmRch);
                        cbuf.push_back(rset.rhythmTch);
                        cbuf.push_back(rset.rhythmVelOn ? 1u : 0u);
                        cbuf.push_back(rset.rhythmSyncMidi ? 1u : 0u);
                        cbuf.push_back(static_cast<uint8_t>(rset.rhythmTempo & 0xFFu));
                        cbuf.push_back(static_cast<uint8_t>((rset.rhythmTempo >> 8u) & 0xFFu));
                        cbuf.push_back(rset.kit.rhythmVolume);
                        cbuf.push_back(static_cast<uint8_t>(rset.kit.effectMode));
                        cbuf.push_back(rset.kit.keyMapSelect);
                        for (std::size_t p = 0u; p < 100u; ++p) {
                            const auto& pt = rset.patterns[p];
                            cbuf.push_back(pt.bars);
                            cbuf.push_back(pt.timeSigNum);
                            cbuf.push_back(pt.timeSigDen);
                            cbuf.push_back(static_cast<uint8_t>(pt.eventCount & 0xFFu));
                            cbuf.push_back(static_cast<uint8_t>((pt.eventCount >> 8u) & 0xFFu));
                            for (std::size_t e = 0u; e < pt.eventCount; ++e) {
                                cbuf.push_back(static_cast<uint8_t>(pt.events[e].tick & 0xFFu));
                                cbuf.push_back(static_cast<uint8_t>((pt.events[e].tick >> 8u) & 0xFFu));
                                cbuf.push_back(pt.events[e].drumIndex);
                                cbuf.push_back(pt.events[e].velocityLevel);
                            }
                        }
                    }
                }
            }

            // Unconditional MCD64 Banks 0..1 Voices/Perfs + Full Sequencer State
            cbuf.push_back(0xA7u); // Extended state block marker
            for (std::size_t bi = 0u; bi < 2u; ++bi) {
                const auto& bk = virtualCard_->banks[bi];
                for (const auto& v : bk.voices) {
                    vf50::state::StoredVoiceSlot vc = v;
                    vc.repackRomBytes();
                    cbuf.insert(cbuf.end(), vc.voice.rawRomBytes.begin(), vc.voice.rawRomBytes.end());
                }
                for (const auto& pf : bk.performances) {
                    vf50::state::StoredPerformanceSlot pc = pf;
                    pc.repackRomBytes();
                    cbuf.insert(cbuf.end(), pc.perf.rawRomBytes.begin(), pc.perf.rawRomBytes.end());
                    for (int8_t pan : pc.partPans) {
                        cbuf.push_back(static_cast<uint8_t>(pan));
                    }
                }
            }

            const uint8_t seqSongIdx = static_cast<uint8_t>(std::clamp(activeDemoSongIndex(), 0, 10));
            const uint16_t seqTempo  = static_cast<uint16_t>(std::clamp(activeDemoTempoBpm(), 30, 300));
            const uint16_t seqMeas   = static_cast<uint16_t>(std::max(1, activeDemoMeasure()));
            uint16_t muteMask = 0u;
            for (std::size_t t = 0u; t < 9u; ++t) {
                if (isSequencerTrackMuted(t)) {
                    muteMask |= static_cast<uint16_t>(1u << t);
                }
            }
            const auto* slot0 = hostSeqSongs_[0].get();
            const bool customValid = (slot0 != nullptr && slot0->valid && slot0->eventCount > 0u);
            cbuf.push_back(seqSongIdx);
            cbuf.push_back(static_cast<uint8_t>(seqTempo & 0xFFu));
            cbuf.push_back(static_cast<uint8_t>((seqTempo >> 8u) & 0xFFu));
            cbuf.push_back(static_cast<uint8_t>(seqMeas & 0xFFu));
            cbuf.push_back(static_cast<uint8_t>((seqMeas >> 8u) & 0xFFu));
            cbuf.push_back(demoChainAll() ? 1u : 0u);
            cbuf.push_back(static_cast<uint8_t>(muteMask & 0xFFu));
            cbuf.push_back(static_cast<uint8_t>((muteMask >> 8u) & 0xFFu));
            cbuf.push_back(customValid ? 1u : 0u);
            auto writeSeqBuffer = [&cbuf](const vf50::dsp::CustomSeqSongBuffer& cs) {
                cbuf.insert(cbuf.end(), cs.titleChars.begin(), cs.titleChars.end());
                cbuf.push_back(static_cast<uint8_t>(cs.initialTempoBpm & 0xFFu));
                cbuf.push_back(static_cast<uint8_t>((cs.initialTempoBpm >> 8u) & 0xFFu));
                cbuf.push_back(cs.timeSigNum);
                cbuf.push_back(cs.timeSigDen);
                cbuf.push_back(static_cast<uint8_t>(cs.ticksPerBar & 0xFFu));
                cbuf.push_back(static_cast<uint8_t>((cs.ticksPerBar >> 8u) & 0xFFu));
                cbuf.push_back(static_cast<uint8_t>(cs.totalBars & 0xFFu));
                cbuf.push_back(static_cast<uint8_t>((cs.totalBars >> 8u) & 0xFFu));
                writeU32LE(cbuf, cs.totalTicks);
                cbuf.push_back(cs.playMode);
                cbuf.push_back(cs.perfPresetBank);
                cbuf.push_back(cs.perfNumber);
                cbuf.insert(cbuf.end(), cs.trackChannels.begin(), cs.trackChannels.end());
                const uint32_t evCnt = std::min<uint32_t>(cs.eventCount, static_cast<uint32_t>(vf50::dsp::CustomSeqSongBuffer::kMaxEvents));
                writeU32LE(cbuf, evCnt);
                for (uint32_t e = 0u; e < evCnt; ++e) {
                    const auto& ev = cs.events[e];
                    writeU32LE(cbuf, ev.tick);
                    cbuf.push_back(static_cast<uint8_t>(ev.type));
                    cbuf.push_back(ev.channel);
                    cbuf.push_back(ev.data1);
                    cbuf.push_back(ev.data2);
                }
            };
            if (customValid) {
                writeSeqBuffer(*slot0);
            }

            // Extension 0xA8: Full 8-slot User Sequencer Bank (slots 0..7 with rhythm sync link)
            cbuf.push_back(0xA8u);
            uint8_t slotMask = 0u;
            for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
                if (hasUserSequenceInSlot(s)) {
                    slotMask |= static_cast<uint8_t>(1u << s);
                }
            }
            cbuf.push_back(slotMask);
            for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
                if (((slotMask >> s) & 1u) != 0u && hostSeqSongs_[s] != nullptr) {
                    const auto& cs = *hostSeqSongs_[s];
                    cbuf.push_back(static_cast<uint8_t>((cs.hasRhythmLink ? 1u : 0u) | (cs.rhythmSyncIsSong ? 2u : 0u)));
                    cbuf.push_back(cs.rhythmTargetIdx);
                    writeSeqBuffer(cs);
                }
            }
        }

        container.insert(container.end(), std::begin(kClassicPhase3TrailerMagic), std::end(kClassicPhase3TrailerMagic));
        writeU32LE(container, kClassicTrailerVersion);
        writeU32LE(container, static_cast<uint32_t>(cbuf.size()));
        container.insert(container.end(), cbuf.begin(), cbuf.end());
    }

    // Backward-compatible Visual Mixer Console trailer ('VF5M' v3)
    container.insert(container.end(), std::begin(kMixerTrailerMagic), std::end(kMixerTrailerMagic));
    container.push_back(static_cast<uint8_t>(kMixerTrailerVersion));
    container.push_back(isMixerVisible() ? 1u : 0u);
    container.push_back(isMasterSafetyLimiterEnabled() ? 1u : 0u);
    container.push_back(0u);
    writeU32LE(container, 9u * 14u + 2u);
    for (std::size_t ch = 0u; ch < 9u; ++ch) {
        writeF32LE(container, mixerChannelBalance(ch));
        container.push_back(mixerChannelFxSend(ch));
        container.push_back(mixerChannelEqEnabled(ch) ? 1u : 0u);
        writeF32LE(container, mixerChannelEqLowDb(ch));
        writeF32LE(container, mixerChannelEqHighDb(ch));
    }
    const uint16_t auxExclMask = synth_.auxExclusiveMask();
    container.push_back(static_cast<uint8_t>(auxExclMask & 0xFFu));
    container.push_back(static_cast<uint8_t>((auxExclMask >> 8u) & 0xFFu));

    destData.replaceAll(container.data(), container.size());
}

void VF50AudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes <= 0) return;

    auto restoreApvtsXml = [this](const juce::XmlElement& xmlState) {
        if (!xmlState.hasTagName(apvts_.state.getType())) {
            return;
        }
        const ScopedDirtySuppress suppress(suppressParamDirtyCount_);
        apvts_.replaceState(juce::ValueTree::fromXml(xmlState));
        // Snap host-facing normalized values on discrete params even when unnormalisedValue already matched.
        for (auto* p : getParameters()) {
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*>(p)) {
                if (const auto* rawVal = apvts_.getRawParameterValue(rp->paramID)) {
                    const float targetNorm = rp->convertTo0to1(rawVal->load(std::memory_order_relaxed));
                    if (!juce::approximatelyEqual(rp->getValue(), targetNorm)) {
                        rp->setValueNotifyingHost(targetNorm);
                    }
                }
            }
        }
        clearAllDirtyFlags();
    };

    const auto* bytes     = static_cast<const uint8_t*>(data);
    const auto totalBytes = static_cast<std::size_t>(sizeInBytes);

    if (totalBytes >= 16u && std::memcmp(bytes, kStateContainerMagic, 4u) == 0) {
        std::size_t pos  = 4u;
        uint32_t version = 0u;
        uint32_t xmlLen  = 0u;
        if (!readU32LE(bytes, totalBytes, pos, version) || version != kStateContainerVersion) {
            return;
        }
        if (!readU32LE(bytes, totalBytes, pos, xmlLen) || (totalBytes - pos) < xmlLen) {
            return;
        }

        if (xmlLen > 0u) {
            if (std::unique_ptr<juce::XmlElement> xmlState =
                    getXmlFromBinary(bytes + pos, static_cast<int>(xmlLen))) {
                restoreApvtsXml(*xmlState);
            }
            pos += xmlLen;
        }

        // Clear dirty flags triggered by replaceState so restored Edit/User buffers are not clobbered
        clearAllDirtyFlags();

        uint32_t memLen = 0u;
        if (readU32LE(bytes, totalBytes, pos, memLen) && (totalBytes - pos) >= memLen && memLen > 0u) {
            if (hostMemory_.deserializeFromBinary(bytes + pos, memLen)) {
                pendingAudioSlot_.store(-1, std::memory_order_release);
                pushHostMemoryToAudio();
            }
            pos += memLen;
        }

        if ((totalBytes - pos) >= 12u && std::memcmp(bytes + pos, kUiPrefsTrailerMagic, 4u) == 0) {
            pos += 4u;
            uint32_t scaleMilli = 1000u;
            if (readU32LE(bytes, totalBytes, pos, scaleMilli) && (totalBytes - pos) >= 4u) {
                setUiScaleFactor(static_cast<float>(scaleMilli) / 1000.0f);
                setVirtualKeyboardVisible(bytes[pos] != 0u);
                setLcdThemeIndex(static_cast<int>(bytes[pos + 1u]));
                setGlobalTransposeSemitones(static_cast<int>(static_cast<int8_t>(bytes[pos + 2u])));
                setIntMemoryProtected((bytes[pos + 3u] & 1u) != 0u);
                setCardMemoryProtected((bytes[pos + 3u] & 2u) != 0u);
                pos += 4u;

                if ((totalBytes - pos) >= 8u && std::memcmp(bytes + pos, kWorkflowTrailerMagic, 4u) == 0) {
                    pos += 4u;
                    // Legacy OperatingMode / WorkflowMode byte (0=Modern, 1=Classic) is intentionally
                    // ignored: unified workstation always runs on VF50ClassicController.
                    (void)bytes[pos];
                    setProgrammerVisible(bytes[pos + 1u] != 0u);
                    setDawSyncEnabled(bytes[pos + 2u] != 0u);
                    setDawSequencerArmed(bytes[pos + 3u] != 0u);
                    pos += 4u;
                }

                if ((totalBytes - pos) >= 12u && std::memcmp(bytes + pos, kClassicPhase3TrailerMagic, 4u) == 0) {
                    pos += 4u;
                    uint32_t cVer = 0u, cLen = 0u;
                    if (readU32LE(bytes, totalBytes, pos, cVer) && cVer == kClassicTrailerVersion
                        && readU32LE(bytes, totalBytes, pos, cLen) && (totalBytes - pos) >= cLen) {
                        std::size_t cp = pos;
                        const std::size_t cEnd = pos + cLen;
                        auto hasRem = [&](std::size_t n) noexcept -> bool { return cp <= cEnd && (cEnd - cp) >= n; };
                        auto readSys = [&](vf50::state::VF50SystemSettings& s) noexcept -> bool {
                            if (!hasRem(19u + 40u)) return false;
                            s.masterTuningSteps   = static_cast<int8_t>(bytes[cp++]);
                            s.synthVolume         = std::min<uint8_t>(bytes[cp++], 99u);
                            s.combineWithFunction = (bytes[cp++] != 0u);
                            s.controllerReset     = (bytes[cp++] != 0u);
                            s.fixedVelocity       = std::min<uint8_t>(bytes[cp++], 127u);
                            s.velocityCurve       = std::min<uint8_t>(bytes[cp++], 7u);
                            s.egDampSpeed         = std::min<uint8_t>(bytes[cp++], 3u);
                            s.voiceDampOn         = (bytes[cp++] != 0u);
                            s.midiOn              = (bytes[cp++] != 0u);
                            s.basicReceiveCh      = std::min<uint8_t>(bytes[cp++], 16u);
                            s.transmitCh          = std::min<uint8_t>(bytes[cp++], 15u);
                            (void)bytes[cp++]; // localOn always defaults true at boot (Manual p. 99)
                            s.localOn             = true;
                            s.controlChangeMode   = std::min<uint8_t>(bytes[cp++], 17u);
                            s.aftertouchMode      = std::min<uint8_t>(bytes[cp++], 17u);
                            s.pitchBendMode       = std::min<uint8_t>(bytes[cp++], 17u);
                            s.noteOnOffFilter     = std::min<uint8_t>(bytes[cp++], 2u);
                            s.dataEntryAssignCc   = std::min<uint8_t>(bytes[cp++], 31u);
                            s.pgmChangeMode       = std::min<uint8_t>(bytes[cp++], 4u);
                            s.deviceNumber        = std::min<uint8_t>(bytes[cp++], 17u);
                            for (std::size_t i = 0u; i < 40u; ++i) {
                                const uint8_t ch = bytes[cp++];
                                s.powerOnMessage[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
                            }
                            return true;
                        };
                        auto readPef = [&](vf50::state::PerfEffectBank& pb) noexcept -> bool {
                            if (!hasRem(16u + 12u + 192u)) return false;
                            for (auto& d : pb.delays) {
                                d.delayTimeCs = std::clamp<uint8_t>(bytes[cp++], 1u, 128u);
                                d.pitchShift  = std::clamp<int8_t>(static_cast<int8_t>(bytes[cp++]), -24, 24);
                                d.feedback    = std::min<uint8_t>(bytes[cp++], 7u);
                                d.effectLevel = std::min<uint8_t>(bytes[cp++], 99u);
                            }
                            for (auto& pn : pb.pans) {
                                pn.select    = std::min<uint8_t>(bytes[cp++], 2u);
                                pn.direction = std::min<uint8_t>(bytes[cp++], 1u);
                                pn.panRange  = std::min<uint8_t>(bytes[cp++], 99u);
                            }
                            for (auto& ch : pb.chords) {
                                for (auto& row : ch.chords) {
                                    for (int8_t& n : row) {
                                        n = static_cast<int8_t>(bytes[cp++]);
                                    }
                                }
                            }
                            return true;
                        };
                        auto readPct = [&](vf50::state::ProgramChangeTable& pct) noexcept -> bool {
                            if (!hasRem(128u * 3u)) return false;
                            for (auto& e : pct.entries) {
                                e.isPerformance = (bytes[cp++] != 0u);
                                e.bank          = std::min<uint8_t>(bytes[cp++], 2u);
                                e.number        = std::min<uint8_t>(bytes[cp++], 99u);
                            }
                            return true;
                        };

                        if (readSys(classicSysSettings_) && readPef(perfEffectBank_) && readPct(pgmChangeTable_)
                            && virtualCard_ != nullptr && hasRem(4u)) {
                            virtualCard_->mediaType            = static_cast<vf50::state::CardMediaType>(std::min<uint8_t>(bytes[cp++], 2u));
                            virtualCard_->hardwareWriteProtect = (bytes[cp++] != 0u);
                            virtualCard_->batteryLowWarning    = (bytes[cp++] != 0u);
                            virtualCard_->activeBankIndex      = bytes[cp++] & 1u;
                            for (std::size_t bi = 0u; bi < 2u && hasRem(2u); ++bi) {
                                auto& bk = virtualCard_->banks[bi];
                                bk.format = static_cast<vf50::state::CardBankFormat>(std::min<uint8_t>(bytes[cp++], 2u));
                                const uint8_t flags = bytes[cp++];
                                bk.hasVoicesPerfs = (flags & 1u) != 0u;
                                bk.hasPct         = (flags & 2u) != 0u;
                                bk.hasPefct       = (flags & 4u) != 0u;
                                bk.hasMct         = (flags & 8u) != 0u;
                                bk.hasSys         = (flags & 16u) != 0u;
                                bk.hasYsSeq       = (flags & 32u) != 0u;
                                if (bk.hasVoicesPerfs && hasRem(100u * 95u + 100u * 97u)) {
                                    std::array<uint8_t, 95> rv{};
                                    std::array<uint8_t, 97> rp{};
                                    for (auto& v : bk.voices) {
                                        std::memcpy(rv.data(), bytes + cp, 95u); cp += 95u;
                                        v.unpackFromRom95(rv);
                                    }
                                    for (auto& pf : bk.performances) {
                                        std::memcpy(rp.data(), bytes + cp, 97u); cp += 97u;
                                        pf.unpackFromRom97(rp);
                                    }
                                }
                                if (bk.hasPct) (void)readPct(bk.pct);
                                if (bk.hasPefct) (void)readPef(bk.pefct);
                                if (bk.hasMct && hasRem(24u + 256u)) {
                                    for (auto& m : bk.microOct) { m.coarseNote = bytes[cp++]; m.fineStep64 = bytes[cp++]; }
                                    for (auto& m : bk.microFull) { m.coarseNote = bytes[cp++]; m.fineStep64 = bytes[cp++]; }
                                }
                                if (bk.hasSys && readSys(bk.sys) && hasRem(2u)) {
                                    bk.sysIntProtect  = (bytes[cp++] != 0u);
                                    bk.sysCardProtect = (bytes[cp++] != 0u);
                                }
                                for (std::size_t rs = 0u; rs < 2u && hasRem(1u); ++rs) {
                                    auto& rset = bk.rhythmSets[rs];
                                    const uint8_t rflags = bytes[cp++];
                                    rset.hasAll   = (rflags & 1u) != 0u;
                                    rset.hasRSeq  = (rflags & 2u) != 0u;
                                    rset.hasSetup = (rflags & 4u) != 0u;
                                    if (rflags != 0u && hasRem(9u)) {
                                        rset.rhythmRch      = bytes[cp++];
                                        rset.rhythmTch      = bytes[cp++];
                                        rset.rhythmVelOn    = (bytes[cp++] != 0u);
                                        rset.rhythmSyncMidi = (bytes[cp++] != 0u);
                                        rset.rhythmTempo    = static_cast<uint16_t>(bytes[cp]) | static_cast<uint16_t>(bytes[cp + 1u] << 8u);
                                        cp += 2u;
                                        rset.kit.rhythmVolume = std::min<uint8_t>(bytes[cp++], 99u);
                                        rset.kit.effectMode   = static_cast<vf50::dsp::drums::RhythmEffectMode>(std::min<uint8_t>(bytes[cp++], 2u));
                                        rset.kit.keyMapSelect = std::min<uint8_t>(bytes[cp++], 4u);
                                        for (std::size_t p = 0u; p < 100u && hasRem(5u); ++p) {
                                            auto& pt = rset.patterns[p];
                                            pt.clear();
                                            pt.bars       = std::clamp<uint8_t>(bytes[cp++], 1u, 4u);
                                            pt.timeSigNum = std::max<uint8_t>(bytes[cp++], 1u);
                                            pt.timeSigDen = std::max<uint8_t>(bytes[cp++], 1u);
                                            pt.totalTicks = static_cast<uint16_t>((192u * pt.timeSigNum) / pt.timeSigDen) * pt.bars;
                                            const uint16_t ec = static_cast<uint16_t>(bytes[cp]) | static_cast<uint16_t>(bytes[cp + 1u] << 8u);
                                            cp += 2u;
                                            const uint16_t clampedEc = std::min<uint16_t>(ec, static_cast<uint16_t>(vf50::state::UserRhythmPattern::kMaxEvents));
                                            for (uint16_t e = 0u; e < ec; ++e) {
                                                if (!hasRem(4u)) break;
                                                const uint16_t tk = static_cast<uint16_t>(bytes[cp]) | static_cast<uint16_t>(bytes[cp + 1u] << 8u);
                                                const uint8_t d   = bytes[cp + 2u];
                                                const uint8_t vl  = bytes[cp + 3u];
                                                cp += 4u;
                                                if (e < clampedEc) {
                                                    (void)pt.addEventSorted(tk, d, vl);
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Unconditional MCD64 Banks 0..1 Voices/Perfs + Full Sequencer State
                            if (hasRem(1u) && bytes[cp] == 0xA7u) {
                                ++cp;
                                const std::size_t cardFullBytes = 2u * (100u * 95u + 100u * (97u + 8u));
                                if (hasRem(cardFullBytes)) {
                                    std::array<uint8_t, 95> rv{};
                                    std::array<uint8_t, 97> rp{};
                                    std::array<int8_t, 8> pans{};
                                    for (std::size_t bi = 0u; bi < 2u; ++bi) {
                                        auto& bk = virtualCard_->banks[bi];
                                        for (auto& v : bk.voices) {
                                            std::memcpy(rv.data(), bytes + cp, 95u);
                                            cp += 95u;
                                            v.unpackFromRom95(rv);
                                        }
                                        for (auto& pf : bk.performances) {
                                            std::memcpy(rp.data(), bytes + cp, 97u);
                                            cp += 97u;
                                            for (std::size_t pi = 0u; pi < 8u; ++pi) {
                                                pans[pi] = static_cast<int8_t>(std::clamp<int>(static_cast<int8_t>(bytes[cp++]), -64, 63));
                                            }
                                            pf.unpackFromRom97(rp, pans);
                                        }
                                    }
                                }
                                if (hasRem(9u)) {
                                    const uint8_t seqSongIdx = bytes[cp++];
                                    const uint16_t seqTempo  = static_cast<uint16_t>(bytes[cp])
                                                             | static_cast<uint16_t>(static_cast<uint16_t>(bytes[cp + 1u]) << 8u);
                                    cp += 2u;
                                    const uint16_t seqMeas   = static_cast<uint16_t>(bytes[cp])
                                                             | static_cast<uint16_t>(static_cast<uint16_t>(bytes[cp + 1u]) << 8u);
                                    cp += 2u;
                                    const bool chainAll      = (bytes[cp++] != 0u);
                                    const uint16_t muteMask  = static_cast<uint16_t>(bytes[cp])
                                                             | static_cast<uint16_t>(static_cast<uint16_t>(bytes[cp + 1u]) << 8u);
                                    cp += 2u;
                                    const bool hasCustom     = (bytes[cp++] != 0u);

                                    auto readSeqBuffer = [&](vf50::dsp::CustomSeqSongBuffer& cs) -> bool {
                                        if (!hasRem(16u + 2u + 2u + 4u + 4u + 3u + 8u + 4u)) {
                                            return false;
                                        }
                                        cs.clear();
                                        std::memcpy(cs.titleChars.data(), bytes + cp, 16u);
                                        cp += 16u;
                                        cs.initialTempoBpm = static_cast<uint16_t>(bytes[cp])
                                                           | static_cast<uint16_t>(static_cast<uint16_t>(bytes[cp + 1u]) << 8u);
                                        cp += 2u;
                                        cs.timeSigNum  = std::max<uint8_t>(1u, bytes[cp++]);
                                        cs.timeSigDen  = std::max<uint8_t>(1u, bytes[cp++]);
                                        cs.ticksPerBar = static_cast<uint16_t>(bytes[cp])
                                                       | static_cast<uint16_t>(static_cast<uint16_t>(bytes[cp + 1u]) << 8u);
                                        cp += 2u;
                                        cs.totalBars   = static_cast<uint16_t>(bytes[cp])
                                                       | static_cast<uint16_t>(static_cast<uint16_t>(bytes[cp + 1u]) << 8u);
                                        cp += 2u;
                                        uint32_t totTicks = 192u, evCnt = 0u;
                                        (void)readU32LE(bytes, cEnd, cp, totTicks);
                                        cs.totalTicks = std::max<uint32_t>(1u, totTicks);
                                        cs.playMode       = bytes[cp++];
                                        cs.perfPresetBank = bytes[cp++];
                                        cs.perfNumber     = std::min<uint8_t>(bytes[cp++], 99u);
                                        for (std::size_t t = 0u; t < 8u; ++t) {
                                            cs.trackChannels[t] = bytes[cp++] & 0x0Fu;
                                        }
                                        (void)readU32LE(bytes, cEnd, cp, evCnt);
                                        const uint32_t safeCnt = std::min<uint32_t>(evCnt, static_cast<uint32_t>(vf50::dsp::CustomSeqSongBuffer::kMaxEvents));
                                        uint32_t loadedEv = 0u;
                                        for (uint32_t e = 0u; e < evCnt && hasRem(8u); ++e) {
                                            uint32_t tk = 0u;
                                            (void)readU32LE(bytes, cEnd, cp, tk);
                                            const auto tp = static_cast<vf50::generated::DemoEventType>(std::min<uint8_t>(bytes[cp++], 8u));
                                            const uint8_t ch = bytes[cp++];
                                            const uint8_t d1 = bytes[cp++];
                                            const uint8_t d2 = bytes[cp++];
                                            if (e < safeCnt) {
                                                cs.events[loadedEv++] = { tk, tp, ch, d1, d2 };
                                            }
                                        }
                                        cs.eventCount = loadedEv;
                                        cs.valid      = (loadedEv > 0u);
                                        return true;
                                    };

                                    for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
                                        if (hostSeqSongs_[s] != nullptr) {
                                            hostSeqSongs_[s]->clear();
                                        }
                                        synth_.demoPlayer().clearUserSequenceSlot(s);
                                    }

                                    if (hasCustom && hostSeqSongs_[0] != nullptr) {
                                        if (readSeqBuffer(*hostSeqSongs_[0])) {
                                            synth_.demoPlayer().loadUserSequenceSlot(0u, *hostSeqSongs_[0]);
                                        }
                                    }

                                    bool hasA8Bank = false;
                                    if (hasRem(2u) && bytes[cp] == 0xA8u) {
                                        ++cp;
                                        hasA8Bank = true;
                                        const uint8_t slotMask = bytes[cp++];
                                        for (std::size_t s = 0u; s < vf50::dsp::VF50DemoSongPlayer::kUserSongCount; ++s) {
                                            if (((slotMask >> s) & 1u) != 0u && hasRem(2u) && hostSeqSongs_[s] != nullptr) {
                                                const uint8_t linkFlags = bytes[cp++];
                                                const uint8_t linkTgt   = bytes[cp++];
                                                if (readSeqBuffer(*hostSeqSongs_[s])) {
                                                    hostSeqSongs_[s]->hasRhythmLink    = (linkFlags & 1u) != 0u;
                                                    hostSeqSongs_[s]->rhythmSyncIsSong = (linkFlags & 2u) != 0u;
                                                    hostSeqSongs_[s]->rhythmTargetIdx  = linkTgt;
                                                    synth_.demoPlayer().loadUserSequenceSlot(s, *hostSeqSongs_[s]);
                                                }
                                            }
                                        }
                                    }
                                    publishAllUserSequencesToAudio();

                                    // Legacy 0xA7 without 0xA8 stored custom song at index 3; map cleanly to Slot 0 (Song #1)
                                    const std::size_t restoredSongIdx = (!hasA8Bank && hasCustom && seqSongIdx == 3u)
                                                                            ? 0u
                                                                            : static_cast<std::size_t>(std::min<uint8_t>(seqSongIdx, 10u));

                                    setDemoChainAll(chainAll);
                                    selectDemoSong(restoredSongIdx);
                                    synth_.demoPlayer().selectSongIndex(restoredSongIdx);
                                    for (std::size_t t = 0u; t < 9u; ++t) {
                                        setSequencerTrackMuted(t, ((muteMask >> t) & 1u) != 0u);
                                    }
                                    requestDemoSetTempoBpm(static_cast<int>(seqTempo));
                                    requestDemoSeekToMeasure(static_cast<int>(seqMeas));
                                }
                            }
                        }
                        publishClassicSystemStateToAudio();
                        pos += cLen;
                    }
                }
                if ((totalBytes - pos) >= 8u && std::memcmp(bytes + pos, kMixerTrailerMagic, 4u) == 0) {
                    pos += 4u;
                    const uint8_t ver = bytes[pos++];
                    const bool wantMixer = (bytes[pos++] != 0u);
                    const uint8_t flags = bytes[pos++];
                    (void)bytes[pos++];
                    if (ver >= 1u) {
                        if (wantMixer) {
                            setMixerVisible(true);
                        } else {
                            mixerVisible_.store(false, std::memory_order_relaxed);
                        }
                    }
                    if (ver >= 2u) {
                        setMasterSafetyLimiterEnabled((flags & 1u) != 0u);
                        uint32_t mLen = 0u;
                        if (readU32LE(bytes, totalBytes, pos, mLen) && (totalBytes - pos) >= mLen && mLen >= 9u * 14u) {
                            const std::size_t mEnd = pos + mLen;
                            for (std::size_t ch = 0u; ch < 9u; ++ch) {
                                float bal = 0.0f, lo = 0.0f, hi = 0.0f;
                                (void)readF32LE(bytes, mEnd, pos, bal);
                                const uint8_t fxSend = bytes[pos++];
                                const bool eqOn = (bytes[pos++] != 0u);
                                (void)readF32LE(bytes, mEnd, pos, lo);
                                (void)readF32LE(bytes, mEnd, pos, hi);
                                synth_.setChannelBalance(ch, bal);
                                synth_.setChannelFxSend(ch, fxSend);
                                synth_.setChannelEqEnabled(ch, eqOn);
                                synth_.setChannelEqLowDb(ch, lo);
                                synth_.setChannelEqHighDb(ch, hi);
                            }
                            if (ver >= 3u && pos + 2u <= mEnd) {
                                const uint16_t auxMask = static_cast<uint16_t>(bytes[pos])
                                                       | static_cast<uint16_t>(static_cast<uint16_t>(bytes[pos + 1u]) << 8u);
                                synth_.setAuxExclusiveMask(auxMask);
                            } else {
                                synth_.setAuxExclusiveMask(0x01FFu);
                            }
                            pos = mEnd;
                        } else {
                            resetMixerConsoleToNominal(true);
                            synth_.setAuxExclusiveMask(0x01FFu);
                        }
                        clearMixerSolo();
                    } else {
                        setMasterSafetyLimiterEnabled(true);
                        resetMixerConsoleToNominal(true);
                        synth_.setAuxExclusiveMask(0x01FFu);
                    }
                } else {
                    mixerVisible_.store(false, std::memory_order_relaxed);
                    setMasterSafetyLimiterEnabled(true);
                    resetMixerConsoleToNominal(true);
                    synth_.setAuxExclusiveMask(0x01FFu);
                }
                uiPreferencesEpoch_.fetch_add(1u, std::memory_order_release);
            }
        }
        return;
    }

    // Legacy XML fallback
    if (std::unique_ptr<juce::XmlElement> xmlState = getXmlFromBinary(data, sizeInBytes)) {
        restoreApvtsXml(*xmlState);
    }
}

} // namespace vf50::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new vf50::plugin::VF50AudioProcessor();
}
