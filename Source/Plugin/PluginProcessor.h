#pragma once

#ifndef JucePlugin_Name
#define JucePlugin_Name "Victory VF50"
#endif

#include "DSP/VF50SynthEngine.h"
#include "Generated/VF50ParameterLayout.h"
#include "MIDI/VF50SysExHandler.h"
#include "ROM/VF50RomLoader.h"
#include "State/VF50ClassicSystemTypes.h"
#include "State/VF50MemoryManager.h"
#include "State/VF50StateMachine.h"
#include "State/VF50VirtualCardState.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace vf50::plugin {

class VF50AudioProcessor final : public juce::AudioProcessor {
public:
    VF50AudioProcessor();
    ~VF50AudioProcessor() override;

    [[nodiscard]] static constexpr bool isMultiOutBuild() noexcept {
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        return true;
#else
        return false;
#endif
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    [[nodiscard]] juce::AudioProcessorValueTreeState& apvts() noexcept { return apvts_; }
    [[nodiscard]] const juce::AudioProcessorValueTreeState& apvts() const noexcept { return apvts_; }
    [[nodiscard]] const vf50::params::VF50AtomicParameterBank& parameterBank() const noexcept { return paramBank_; }
    [[nodiscard]] juce::MidiKeyboardState& keyboardState() noexcept { return keyboardState_; }

    [[nodiscard]] vf50::dsp::VF50SynthEngine& synthEngine() noexcept { return synth_; }
    [[nodiscard]] const vf50::dsp::VF50SynthEngine& synthEngine() const noexcept { return synth_; }

    // Host/UI-side Workstation Memory Manager & Manual State Machine + Wait-Free Publish to Audio Thread
    [[nodiscard]] vf50::state::VF50MemoryManager& hostMemory() noexcept { return hostMemory_; }
    [[nodiscard]] const vf50::state::VF50MemoryManager& hostMemory() const noexcept { return hostMemory_; }
    [[nodiscard]] vf50::state::VF50StateMachine& stateMachine() noexcept { return stateMachine_; }
    [[nodiscard]] const vf50::state::VF50StateMachine& stateMachine() const noexcept { return stateMachine_; }
    void pushHostMemoryToAudio() noexcept;
    void pullLatestAudioMemoryToHost() noexcept;

    // Runtime Firmware ROM Bank Management (Supports Embedded or Firmware-Free External Loading)
    [[nodiscard]] const vf50::rom::VF50RomImage& activeRomBank() const noexcept {
        return (hostRomImage_ != nullptr) ? *hostRomImage_ : vf50::rom::getDefaultRomImage();
    }
    [[nodiscard]] const vf50::rom::VF50RomSetBuilder& romSetBuilder() const noexcept { return romSetBuilder_; }
    [[nodiscard]] uint32_t romEpoch() const noexcept { return romEpoch_.load(std::memory_order_acquire); }
    int loadRomFiles(const juce::StringArray& paths, juce::String& outStatus, bool persistConfig = true);
    int loadRomFolder(const juce::File& folder, juce::String& outStatus, bool persistConfig = true);
    void clearExternalRoms();
    void installRuntimeRomBank(std::unique_ptr<vf50::rom::VF50RomImage> newRom, bool reloadFactoryDefaults = true);

    // Unified Classic Workstation Operating Mode (permanent)
    [[nodiscard]] vf50::state::WorkflowMode workflowMode() const noexcept {
        return vf50::state::WorkflowMode::Classic;
    }

    // Non-audio-thread helpers to mirror active preset parameters into APVTS controls
    void selectSinglePresetAtomic(vf50::state::MemoryBank bank, std::size_t presetIndex, bool updateSelectorParams = true);
    void selectPerformancePresetAtomic(vf50::state::MemoryBank bank, std::size_t presetIndex, bool updateSelectorParams = true);
    void stageActiveSingleEditVoiceAtomic();
    void stageActivePerfEditAtomic();
    [[nodiscard]] bool hasPendingPatchSwap() const noexcept {
        return newPatchPending.load(std::memory_order_acquire)
            || newPerfPatchPending_.load(std::memory_order_acquire);
    }
    void syncApvtsToActiveSingleVoice(bool includeEffects = true);
    void syncApvtsToActivePerformance(bool includeEffects = true);
    void commitHostVoiceEditToAudio(bool syncEffects = true);
    void commitHostPerfEditToAudio(bool syncEffects = true);
    bool applyHostProgrammerVoiceParam(vf50::params::ParamIndex paramIndex);

    // Live host-side Visual Mixer write helpers (respects [COMPARE] lock, marks dirty)
    bool applyHostMixerPerfPartVolume(std::size_t part0To7, uint8_t vol0To99);
    bool applyHostMixerPerfPartOutAssign(std::size_t part0To7, uint8_t outAssign0To3);
    bool applyHostMixerPerfPartNotes(std::size_t part0To7, uint8_t notes0To16);
    bool applyHostMixerPerfPartNoteShift(std::size_t part0To7, int8_t shiftNeg24ToPos24);
    bool applyHostMixerPerfPartDetune(std::size_t part0To7, int8_t detuneNeg7ToPos7);
    void applyHostMixerRhythmVolume(uint8_t vol0To99);
    void applyHostMixerRhythmEffectMode(vf50::dsp::drums::RhythmEffectMode mode);
    void applyHostMixerDrumInstVolume(std::size_t drum0To60, uint8_t vol0To15);
    void applyHostMixerDrumInstPan(std::size_t drum0To60, int8_t panNeg3ToPos3);
    bool applyHostMixerEffectType(uint8_t fxType0To32);
    bool applyHostMixerEffectBalance(uint8_t bal0To99);

    // Console-only Mixer Extensions (Balance, FX Send, 2-Band EQ, Safety Limiter)
    void setMixerChannelBalance(std::size_t ch0To8, float balNeg1ToPos1) noexcept {
        synth_.setChannelBalance(ch0To8, balNeg1ToPos1);
    }
    [[nodiscard]] float mixerChannelBalance(std::size_t ch0To8) const noexcept {
        return synth_.channelBalance(ch0To8);
    }
    void setMixerChannelFxSend(std::size_t ch0To8, uint8_t send0To99);
    [[nodiscard]] uint8_t mixerChannelFxSend(std::size_t ch0To8) const noexcept {
        return synth_.channelFxSend(ch0To8);
    }
    void setMixerChannelEqEnabled(std::size_t ch0To8, bool enabled) noexcept {
        synth_.setChannelEqEnabled(ch0To8, enabled);
    }
    [[nodiscard]] bool mixerChannelEqEnabled(std::size_t ch0To8) const noexcept {
        return synth_.channelEqEnabled(ch0To8);
    }
    void setMixerChannelEqLowDb(std::size_t ch0To8, float dbNeg12ToPos12) noexcept {
        synth_.setChannelEqLowDb(ch0To8, dbNeg12ToPos12);
    }
    [[nodiscard]] float mixerChannelEqLowDb(std::size_t ch0To8) const noexcept {
        return synth_.channelEqLowDb(ch0To8);
    }
    void setMixerChannelEqHighDb(std::size_t ch0To8, float dbNeg12ToPos12) noexcept {
        synth_.setChannelEqHighDb(ch0To8, dbNeg12ToPos12);
    }
    [[nodiscard]] float mixerChannelEqHighDb(std::size_t ch0To8) const noexcept {
        return synth_.channelEqHighDb(ch0To8);
    }
    void setMasterSafetyLimiterEnabled(bool enabled) noexcept {
        synth_.setSafetyLimiterEnabled(enabled);
    }
    [[nodiscard]] bool isMasterSafetyLimiterEnabled() const noexcept {
        return synth_.isSafetyLimiterEnabled();
    }
    void resetMixerConsoleToNominal(bool clearSoloToo = true) noexcept;

    // Real-time UI telemetry (lock-free relaxed atomics)
    [[nodiscard]] float meterLeft() const noexcept { return meterPeakL_.load(std::memory_order_relaxed); }
    [[nodiscard]] float meterRight() const noexcept { return meterPeakR_.load(std::memory_order_relaxed); }
    [[nodiscard]] float instPeakL(std::size_t inst0To7) const noexcept { return synth_.instPeakL(inst0To7); }
    [[nodiscard]] float instPeakR(std::size_t inst0To7) const noexcept { return synth_.instPeakR(inst0To7); }
    [[nodiscard]] float instPeak(std::size_t inst0To7) const noexcept { return synth_.instPeak(inst0To7); }
    [[nodiscard]] float rhyPeakL() const noexcept { return synth_.rhyPeakL(); }
    [[nodiscard]] float rhyPeakR() const noexcept { return synth_.rhyPeakR(); }
    [[nodiscard]] float rhyPeak() const noexcept { return synth_.rhyPeak(); }
    [[nodiscard]] float masterPeakL() const noexcept { return synth_.masterPeakL(); }
    [[nodiscard]] float masterPeakR() const noexcept { return synth_.masterPeakR(); }
    [[nodiscard]] float limiterGainReduction() const noexcept { return synth_.limiterGainReduction(); }
    [[nodiscard]] uint8_t instActiveVoices(std::size_t inst0To7) const noexcept { return synth_.instActiveVoices(inst0To7); }
    [[nodiscard]] uint8_t totalActiveFmVoices() const noexcept { return synth_.totalActiveFmVoices(); }
    [[nodiscard]] uint8_t totalActiveRhyVoices() const noexcept { return synth_.totalActiveRhyVoices(); }

    void setMixerSoloMask(uint16_t mask) noexcept { synth_.setSoloMask(mask); }
    [[nodiscard]] uint16_t mixerSoloMask() const noexcept { return synth_.soloMask(); }
    void setMixerChannelSoloed(std::size_t ch0To8, bool soloed) noexcept { synth_.setChannelSoloed(ch0To8, soloed); }
    void toggleMixerSoloChannel(std::size_t ch0To8) noexcept { synth_.toggleChannelSolo(ch0To8); }
    void clearMixerSolo() noexcept { synth_.clearSolo(); }

    void setMixerChannelAuxExclusive(std::size_t ch0To8, bool auxExclusive) noexcept {
        synth_.setChannelAuxExclusive(ch0To8, auxExclusive);
    }
    [[nodiscard]] bool mixerChannelAuxExclusive(std::size_t ch0To8) const noexcept {
        return synth_.isChannelAuxExclusive(ch0To8);
    }
    void toggleMixerChannelAuxExclusive(std::size_t ch0To8) noexcept {
        synth_.toggleChannelAuxExclusive(ch0To8);
    }
    [[nodiscard]] bool isHostAuxBusEnabled(std::size_t ch0To8) const noexcept;
    [[nodiscard]] int activeFmVoices() const noexcept { return activeFmVoices_.load(std::memory_order_relaxed); }
    [[nodiscard]] int activeDrumVoices() const noexcept { return activeDrumVoices_.load(std::memory_order_relaxed); }
    [[nodiscard]] float livePitchBendNorm() const noexcept { return livePitchBendNorm_.load(std::memory_order_relaxed); }
    [[nodiscard]] float liveModWheelNorm() const noexcept { return liveModWheelNorm_.load(std::memory_order_relaxed); }
    [[nodiscard]] vf50::state::WorkstationMode activeWorkstationMode() const noexcept {
        return static_cast<vf50::state::WorkstationMode>(activeWsMode_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] vf50::state::MemoryBank activeVoiceBank() const noexcept {
        return static_cast<vf50::state::MemoryBank>(activeVoiceBank_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] vf50::state::MemoryBank activePerfBank() const noexcept {
        return static_cast<vf50::state::MemoryBank>(activePerfBank_.load(std::memory_order_relaxed));
    }

    // Lock-free UI -> Audio MIDI event helpers (Pitch/Mod wheels, C3 audition)
    void enqueueUiMidi3(uint8_t status, uint8_t data1, uint8_t data2) noexcept;
    void enqueueUiPitchBendNorm(float bipolarNeg1ToPos1, uint8_t channel = 0u) noexcept;
    void enqueueUiModWheelNorm(float unipolar0To1, uint8_t channel = 0u) noexcept;

    // Rhythm Keyboard / MIDI Drum Trigger Telemetry (Task 2)
    [[nodiscard]] uint32_t lastDrumTriggerSeq() const noexcept { return drumTrigSeq_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint8_t lastTriggeredDrumNote() const noexcept { return drumTrigNote_.load(std::memory_order_relaxed); }
    [[nodiscard]] int lastTriggeredDrumIndex() const noexcept { return drumTrigIndex_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint8_t lastTriggeredDrumVelocity() const noexcept { return drumTrigVel_.load(std::memory_order_relaxed); }

    // Authentic ROM Demo Song (isolated slots 8..10) & 8-Slot User Sequencer (slots 0..7)
    void requestDemoStart(std::size_t songIndex) noexcept;
    void requestDemoStop() noexcept;
    void requestDemoToggle() noexcept;
    void requestDemoRewindToTop() noexcept;
    void requestDemoSeekMeasures(int deltaMeasures) noexcept;
    void requestDemoSeekToMeasure(int measureOneBased) noexcept;
    void requestDemoSetTempoBpm(int bpm) noexcept;
    void selectDemoSong(std::size_t songIndex) noexcept;
    void setDemoChainAll(bool chainAll) noexcept { demoChainAll_.store(chainAll, std::memory_order_relaxed); }
    [[nodiscard]] bool demoChainAll() const noexcept { return demoChainAll_.load(std::memory_order_relaxed); }

    void publishUserSequenceToSlot(std::size_t slot0To7, const vf50::dsp::CustomSeqSongBuffer& seq) noexcept;
    void clearUserSequenceSlot(std::size_t slot0To7) noexcept;
    void clearAllUserSequences() noexcept;
    void publishAllUserSequencesToAudio() noexcept;
    [[nodiscard]] bool hasUserSequenceInSlot(std::size_t slot0To7) const noexcept {
        return slot0To7 < vf50::dsp::VF50DemoSongPlayer::kUserSongCount
            && ((userSeqPopulatedMask_.load(std::memory_order_relaxed) >> slot0To7) & 1u) != 0u;
    }
    [[nodiscard]] const vf50::dsp::CustomSeqSongBuffer* userSequenceInSlot(std::size_t slot0To7) const noexcept {
        return (slot0To7 < vf50::dsp::VF50DemoSongPlayer::kUserSongCount) ? hostSeqSongs_[slot0To7].get() : nullptr;
    }
    [[nodiscard]] int firstPopulatedUserSeqSlot() const noexcept;
    [[nodiscard]] std::size_t activeUserSeqSlot() const noexcept {
        return static_cast<std::size_t>(std::clamp(lastUserSeqSlot_.load(std::memory_order_relaxed), 0, 7));
    }
    void publishCustomSequenceToAudio(const vf50::dsp::CustomSeqSongBuffer& seq) noexcept {
        publishUserSequenceToSlot(0u, seq);
        selectDemoSong(0u);
    }
    [[nodiscard]] const vf50::dsp::CustomSeqSongBuffer* hostCustomSequence() const noexcept {
        return userSequenceInSlot(activeUserSeqSlot());
    }

    // Sequencer & Rhythm DAW AudioPlayHead Sync
    [[nodiscard]] bool isDawSyncEnabled() const noexcept { return dawSyncEnabled_.load(std::memory_order_relaxed); }
    void setDawSyncEnabled(bool enabled) noexcept { dawSyncEnabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isDawSequencerArmed() const noexcept { return dawArmSequencer_.load(std::memory_order_relaxed); }
    void setDawSequencerArmed(bool armed) noexcept { dawArmSequencer_.store(armed, std::memory_order_relaxed); }

    [[nodiscard]] bool isDemoPlaying() const noexcept { return demoPlaying_.load(std::memory_order_relaxed); }
    [[nodiscard]] int activeDemoSongIndex() const noexcept { return demoSongIdx_.load(std::memory_order_relaxed); }
    [[nodiscard]] int activeDemoTempoBpm() const noexcept { return demoTempoBpm_.load(std::memory_order_relaxed); }
    [[nodiscard]] int activeDemoMeasure() const noexcept { return demoMeasure_.load(std::memory_order_relaxed); }
    [[nodiscard]] int activeDemoTotalMeasures() const noexcept { return demoTotalMeasures_.load(std::memory_order_relaxed); }
    [[nodiscard]] int activeDemoTimeSigNum() const noexcept { return demoTimeSigNum_.load(std::memory_order_relaxed); }
    [[nodiscard]] int activeDemoTimeSigDen() const noexcept { return demoTimeSigDen_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t sequencerTrackDataMask() const noexcept { return seqTrackDataMask_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool hasCustomDemoSequence() const noexcept {
        return userSeqPopulatedMask_.load(std::memory_order_relaxed) != 0u;
    }
    [[nodiscard]] std::string_view activeDemoSongTitle() const noexcept;
    [[nodiscard]] uint64_t demoVisualKeyMask61() const noexcept { return demoKeyMask61_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t sequencerDispatchedNoteCount() const noexcept { return seqDispatchedNotes_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint8_t sequencerTrackMeterLevel(std::size_t track0To8) const noexcept {
        return (track0To8 < 9u) ? seqTrackMeterLevels_[track0To8].load(std::memory_order_relaxed) : 0u;
    }

    // Classic Performance Instrument Mute & Hit Telemetry Pass-Throughs (Thread-Safe)
    void setClassicPerfPartMuteMask(uint8_t mask) noexcept {
        synth_.fm().setPerfPartMuteMask(mask);
    }
    [[nodiscard]] uint8_t classicPerfPartMuteMask() const noexcept {
        return synth_.fm().perfPartMuteMask();
    }
    [[nodiscard]] uint8_t consumeActivePartHitMask() noexcept {
        return synth_.fm().consumeActivePartHitMask();
    }
    [[nodiscard]] uint8_t consumeMutedPartHitMask() noexcept {
        return synth_.fm().consumeMutedPartHitMask();
    }

    // Real-time Track 1..8 (0..7) & Rhythm Track (8) Mute Controls (Thread-Safe)
    void setSequencerTrackMuted(std::size_t track0To8, bool muted) noexcept {
        synth_.demoPlayer().setTrackMuted(track0To8, muted);
    }
    void toggleSequencerTrackMute(std::size_t track0To8) noexcept {
        synth_.demoPlayer().toggleTrackMute(track0To8);
    }
    void toggleSequencerRhythmMute() noexcept {
        synth_.demoPlayer().toggleRhythmTrackMute();
    }
    [[nodiscard]] bool isSequencerTrackMuted(std::size_t track0To8) const noexcept {
        return synth_.demoPlayer().isTrackMuted(track0To8);
    }
    void resetSequencerTrackMutes() noexcept {
        synth_.demoPlayer().resetTrackMutes();
    }

    // Hardware Utility Setup & Rhythm Key Assign State (Authentic VF50 Menus)
    [[nodiscard]] int globalTransposeSemitones() const noexcept { return globalTransposeSemitones_.load(std::memory_order_relaxed); }
    void setGlobalTransposeSemitones(int semitones) noexcept;
    [[nodiscard]] bool isIntMemoryProtected() const noexcept { return memProtectInt_.load(std::memory_order_relaxed); }
    void setIntMemoryProtected(bool protect) noexcept { memProtectInt_.store(protect, std::memory_order_relaxed); }
    [[nodiscard]] bool isCardMemoryProtected() const noexcept { return memProtectCard_.load(std::memory_order_relaxed); }
    void setCardMemoryProtected(bool protect) noexcept { memProtectCard_.store(protect, std::memory_order_relaxed); }
    [[nodiscard]] int rhythmKeyAssignLayout() const noexcept { return static_cast<int>(activeRhythmKeyLayout_.load(std::memory_order_relaxed)); }
    void setRhythmKeyAssignLayout(int layout0To3) noexcept;

    // Classic System Settings, Performance Effect Bank, Program Change Table & Virtual Card
    [[nodiscard]] const vf50::state::VF50SystemSettings& classicSystemSettings() const noexcept { return classicSysSettings_; }
    [[nodiscard]] vf50::state::VF50SystemSettings& mutableClassicSystemSettings() noexcept { return classicSysSettings_; }
    [[nodiscard]] const vf50::state::PerfEffectBank& perfEffectBank() const noexcept { return perfEffectBank_; }
    [[nodiscard]] vf50::state::PerfEffectBank& mutablePerfEffectBank() noexcept { return perfEffectBank_; }
    [[nodiscard]] const vf50::state::ProgramChangeTable& pgmChangeTable() const noexcept { return pgmChangeTable_; }
    [[nodiscard]] vf50::state::ProgramChangeTable& mutablePgmChangeTable() noexcept { return pgmChangeTable_; }
    [[nodiscard]] const vf50::state::VF50VirtualCardState& virtualCard() const noexcept { return *virtualCard_; }
    [[nodiscard]] vf50::state::VF50VirtualCardState& mutableVirtualCard() noexcept { return *virtualCard_; }
    void publishClassicSystemStateToAudio() noexcept;
    void enqueueMidiOutShort(uint8_t b0, uint8_t b1, uint8_t b2 = 0u, uint8_t len = 3u) noexcept;
    bool enqueueBulkSysEx(const vf50::midi::SysExReplyBuffer& buf) noexcept;
    void setClassicAuditionPerfEffect(uint8_t pef0To12) noexcept {
        classicAuditionPef_.store(pef0To12 <= 12u ? pef0To12 : 0u, std::memory_order_relaxed);
    }
    void setClassicAuditionMicroTuneMode(uint8_t mode0Off1Oct2Full) noexcept {
        classicAuditionMicro_.store(mode0Off1Oct2Full <= 2u ? mode0Off1Oct2Full : 0u, std::memory_order_relaxed);
    }
    void setClassicCardBankActive(bool active) noexcept {
        classicCardBankActive_.store(active, std::memory_order_relaxed);
    }

    // Classic Rhythm Machine Wait-Free Bridges & Telemetry
    struct RhythmUiNoteEvent {
        uint8_t  midiNote    = 0u;
        uint8_t  velocity    = 0u;
        int8_t   drumIndex   = -1;
        uint16_t patternTick = 0u;
    };

    void setClassicRhythmKbdOverride(int modeNeg1Default0Syn1Rhy) noexcept {
        classicRhythmKbdOverride_.store(modeNeg1Default0Syn1Rhy, std::memory_order_release);
    }
    void setClassicRhythmSyncMidi(bool midiSync) noexcept {
        classicRhythmSyncMidi_.store(midiSync, std::memory_order_relaxed);
    }
    void setClassicRhythmCaptureNotes(bool capture) noexcept {
        classicCaptureRhythmNotes_.store(capture, std::memory_order_relaxed);
    }
    void setClassicDemoSuppressDrums(bool suppress) noexcept {
        synth_.demoPlayer().setSuppressInternalDrums(suppress);
    }

    void requestClassicRhythmStartFactory(std::size_t ptnIdx0To99, double tempoBpm, bool loop = true) noexcept;
    void requestClassicRhythmStartUser(std::size_t ptnIdx0To99, const vf50::state::UserRhythmPattern& ptn, double tempoBpm, bool loop = true) noexcept;
    void requestClassicRhythmQueueFactory(std::size_t ptnIdx0To99) noexcept;
    void requestClassicRhythmQueueUser(std::size_t ptnIdx0To99, const vf50::state::UserRhythmPattern& ptn) noexcept;
    void requestClassicRhythmUpdateUserInPlace(std::size_t ptnIdx0To99, const vf50::state::UserRhythmPattern& ptn) noexcept;
    void requestClassicRhythmCommitUserPattern(std::size_t ptnIdx0To99, const vf50::state::UserRhythmPattern& ptn) noexcept;
    void requestClassicRhythmCommitKit(const vf50::state::RhythmKitBank& kit) noexcept;
    void requestClassicRhythmStop() noexcept;
    void requestClassicRhythmRewind() noexcept;
    void requestClassicRhythmSetTempo(double tempoBpm) noexcept;
    void requestClassicRhythmSetMetronome(bool enabled, uint16_t intervalTicks, uint16_t ticksPerBar) noexcept;
    void requestClassicRhythmApplyKeyMap(const std::array<int8_t, 61>& map, uint8_t baseMidiNote) noexcept;
    void requestClassicRhythmStartSong(const vf50::generated::RhythmSongData& song,
                                       uint16_t startPartIdx,
                                       uint16_t startMeasure,
                                       double tempoBpm) noexcept;
    void requestClassicRhythmStopSong() noexcept;

    // Canonical 8 Rhythm Songs (S1..S8, shared across DiskManager, Classic Controller & Audio Thread)
    [[nodiscard]] const std::array<vf50::generated::RhythmSongData, 8>& hostRhythmSongs() const noexcept {
        return *hostRhythmSongs_;
    }
    [[nodiscard]] std::array<vf50::generated::RhythmSongData, 8>& mutableHostRhythmSongs() noexcept {
        return *hostRhythmSongs_;
    }
    void publishRhythmSongsToAudio() noexcept;

    std::size_t consumeRhythmUiNoteEvents(RhythmUiNoteEvent* out, std::size_t maxCount) noexcept;
    [[nodiscard]] uint8_t consumeRhythmVoiceHitMask() noexcept {
        return rhyVoiceHitMask_.exchange(0u, std::memory_order_acq_rel);
    }
    [[nodiscard]] uint8_t rhythmActiveVoiceMask() const noexcept { return rhyActiveVoiceMask_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isRhythmEnginePlaying() const noexcept { return rhyEnginePlaying_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isRhythmSongPlaying() const noexcept { return rhySongPlaying_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isRhythmNextPatternQueued() const noexcept { return rhyNextPatternQueued_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t rhythmCurrentTick() const noexcept { return rhyCurrentTick_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t rhythmPatternWrapCount() const noexcept { return rhyPatternWrapCount_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t rhythmSongCurrentMeasure() const noexcept { return rhySongMeas_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t rhythmSongCurrentPart() const noexcept { return rhySongPart_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool rhythmSongCurrentPtnIsUser() const noexcept { return rhySongPtnIsUser_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint8_t rhythmSongCurrentPtnIndex() const noexcept { return rhySongPtnIdx_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t rhythmSongBarsPlayed() const noexcept { return rhySongBarsPlayed_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t rhythmTotalTriggerCount() const noexcept { return rhyTotalTriggers_.load(std::memory_order_relaxed); }
    [[nodiscard]] int rhythmLiveTempoBpm() const noexcept { return rhyLiveTempoBpm_.load(std::memory_order_relaxed); }

    // Persisted UI Scale, Virtual Keyboard, Programmer Drawer & LCD Theme State
    [[nodiscard]] float uiScaleFactor() const noexcept { return uiScaleFactor_.load(std::memory_order_relaxed); }
    void setUiScaleFactor(float scale) noexcept;
    [[nodiscard]] bool isVirtualKeyboardVisible() const noexcept { return virtualKeyboardVisible_.load(std::memory_order_relaxed); }
    void setVirtualKeyboardVisible(bool visible) noexcept { virtualKeyboardVisible_.store(visible, std::memory_order_relaxed); }
    [[nodiscard]] bool isProgrammerVisible() const noexcept { return programmerVisible_.load(std::memory_order_relaxed); }
    void setProgrammerVisible(bool visible) noexcept {
        programmerVisible_.store(visible, std::memory_order_relaxed);
        if (visible) {
            mixerVisible_.store(false, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] bool isMixerVisible() const noexcept { return mixerVisible_.load(std::memory_order_relaxed); }
    void setMixerVisible(bool visible) noexcept {
        mixerVisible_.store(visible, std::memory_order_relaxed);
        if (visible) {
            programmerVisible_.store(false, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] int lcdThemeIndex() const noexcept { return lcdThemeIndex_.load(std::memory_order_relaxed); }
    void setLcdThemeIndex(int themeIdx) noexcept;
    [[nodiscard]] uint32_t uiPreferencesEpoch() const noexcept { return uiPreferencesEpoch_.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t patchSyncEpoch() const noexcept { return patchSyncEpoch_.load(std::memory_order_acquire); }
    void bumpPatchSyncEpoch() noexcept { patchSyncEpoch_.fetch_add(1u, std::memory_order_release); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    struct IndexedParamListener final : juce::AudioProcessorValueTreeState::Listener {
        VF50AudioProcessor* owner = nullptr;
        std::size_t index = 0u;

        void parameterChanged(const juce::String& parameterID, float newValue) override;
        JUCE_LEAK_DETECTOR(IndexedParamListener)
    };

    struct StagedVoicePatch {
        vf50::state::StoredVoiceSlot voiceSlot{};
        vf50::state::MemoryBank bank = vf50::state::MemoryBank::FactoryRom;
        std::size_t presetIndex = 0u;
        vf50::dsp::fm::QuickEditOffsets quickEdit{ 0, 0, 0, 0 };
        uint8_t opEnableMask = 0x0Fu;
        bool singleEffectSend = true;
        bool useAudioUserRamLookup = false;
        bool forceSingleEditMode = false;
    };

    struct StagedPerfPatch {
        vf50::state::StoredPerformanceSlot perfSlot{};
        vf50::state::MemoryBank bank = vf50::state::MemoryBank::FactoryRom;
        std::size_t presetIndex = 0u;
        bool useAudioUserRamLookup = false;
        bool forcePerfEditMode = false;
    };

    void handleExternalParameterChange(std::size_t paramIndex);
    void markParameterDirty(std::size_t paramIndex) noexcept;
    void clearAllDirtyFlags() noexcept;
    void clearSingleVoiceDirtyFlags() noexcept;
    void clearPerformanceDirtyFlags() noexcept;
    void applyStagedVoicePatchRealTime(const StagedVoicePatch& staged) noexcept;
    void applyStagedPerfPatchRealTime(const StagedPerfPatch& staged) noexcept;
    void applyGlobalRuntimeParamsFromAtomics() noexcept;
    void processDirtyParametersRealTime() noexcept;
    void applySingleVoiceParamEdit(std::size_t idx, vf50::generated::FactoryVoice& v,
                                   bool& maskOrLfo2Only) noexcept;
    void applyPerfParamEdit(std::size_t idx, vf50::state::StoredPerformanceSlot& pfSlot) noexcept;
    void applyRhythmParamEdit(std::size_t idx, bool& restartPatternNeeded) noexcept;
    void applyEffectParamEdit(std::size_t idx, vf50::generated::DspEffectSettings& fx,
                              bool& fxSettingsModified) noexcept;
    void updateRhythmTransportRealTime(bool forceRestart) noexcept;
    void startAccompanyingRhythmPatternForSong(double songTempoBpm) noexcept;

    vf50::dsp::VF50SynthEngine synth_{};
    juce::AudioProcessorValueTreeState apvts_;
    vf50::params::VF50AtomicParameterBank paramBank_{};
    std::array<IndexedParamListener, vf50::params::kTotalParameterCount> paramListeners_{};

    std::array<std::atomic<uint64_t>, vf50::params::kDirtyBitsetWords> dirtyWords_{};
    std::atomic<bool> anyParamDirty_{ false };
    std::atomic<uint32_t> suppressParamDirtyCount_{ 0u };

    // Lock-free double-buffered atomic preset staging (eliminates torn reads during processBlock)
    std::array<StagedVoicePatch, 2> stagedPatchSlots_{};
    std::atomic<uint32_t> activeStagedPatchSlot_{ 0u };
    std::atomic<bool> newPatchPending{ false };

    std::array<StagedPerfPatch, 2> stagedPerfSlots_{};
    std::atomic<uint32_t> activeStagedPerfSlot_{ 0u };
    std::atomic<bool> newPerfPatchPending_{ false };

    // Wait-free Memory Manager instances (zero mutexes, zero heap allocations on audio thread)
    vf50::state::VF50MemoryManager hostMemory_{};
    vf50::state::VF50MemoryManager audioMemory_{};
    vf50::state::VF50StateMachine  stateMachine_{};
    std::array<vf50::state::VF50MemoryManager, 2> hostToAudioSlots_{};
    std::atomic<int> pendingHostSlot_{ -1 };
    int nextHostWriteSlot_ = 0;

    std::array<vf50::state::VF50MemoryManager, 2> audioToHostSlots_{};
    std::atomic<int> pendingAudioSlot_{ -1 };
    int nextAudioWriteSlot_ = 0;

    // Pre-allocated real-time SysEx handler & reply packet buffer
    vf50::midi::VF50SysExHandler sysExHandler_{};
    vf50::midi::SysExReplyBuffer sysExReplyBuffer_{};

    juce::MidiKeyboardState keyboardState_{};

    bool lastRhythmPlay_ = false;

    // Lock-free SPSC UI MIDI queue (256 slots, zero allocations)
    struct UiMidiEvent {
        std::array<uint8_t, 3> bytes{};
        uint8_t len = 0u;
    };
    static constexpr std::size_t kUiMidiQueueSize = 256u;
    std::array<UiMidiEvent, kUiMidiQueueSize> uiMidiQueue_{};
    std::atomic<uint32_t> uiMidiWriteIdx_{ 0u };
    std::atomic<uint32_t> uiMidiReadIdx_{ 0u };

    // UI Telemetry Atomics
    std::atomic<float> meterPeakL_{ 0.0f };
    std::atomic<float> meterPeakR_{ 0.0f };
    std::atomic<int> activeFmVoices_{ 0 };
    std::atomic<int> activeDrumVoices_{ 0 };
    std::atomic<float> livePitchBendNorm_{ 0.0f };
    std::atomic<float> liveModWheelNorm_{ 0.0f };
    std::atomic<uint8_t> activeWsMode_{ static_cast<uint8_t>(vf50::state::WorkstationMode::SinglePlay) };
    std::atomic<uint8_t> activeVoiceBank_{ static_cast<uint8_t>(vf50::state::MemoryBank::FactoryRom) };
    std::atomic<uint8_t> activePerfBank_{ static_cast<uint8_t>(vf50::state::MemoryBank::FactoryRom) };

    // Rhythm Drum Trigger Telemetry Atomics
    std::atomic<uint32_t> drumTrigSeq_{ 0u };
    std::atomic<uint8_t>  drumTrigNote_{ 0u };
    std::atomic<int>      drumTrigIndex_{ -1 };
    std::atomic<uint8_t>  drumTrigVel_{ 0u };

    // Lock-Free 8-Slot User Sequencer & Isolated Demo Song Command/Telemetry State
    std::array<std::unique_ptr<vf50::dsp::CustomSeqSongBuffer>, 8> hostSeqSongs_{};
    std::array<std::array<std::unique_ptr<vf50::dsp::CustomSeqSongBuffer>, 8>, 2> stagingSeqBanks_{};
    std::atomic<int> pendingSeqBank_{ -1 };
    int nextSeqBankWrite_ = 0;
    std::atomic<uint8_t> userSeqPopulatedMask_{ 0u };
    std::atomic<int> lastUserSeqSlot_{ 0 };
    std::array<std::array<char, 16>, 8> hostSeqTitles_{};

    void updateHostSongMetadata(std::size_t songIndex) noexcept;

    std::atomic<uint8_t>     pendingDemoCmd_{ 0u }; // 0=None, 1=Start, 2=Stop, 3=Toggle, 4=Select, 5=RewindTop
    std::atomic<int>         pendingDemoSeekBars_{ 0 };
    std::atomic<int>         pendingDemoTargetMeasure_{ 0 };
    std::atomic<int>         pendingDemoTempoBpm_{ 0 };
    std::atomic<std::size_t> requestedDemoSongIdx_{ 0u };
    std::atomic<bool>        demoChainAll_{ false };

    // Sequencer & Rhythm DAW Sync State
    std::atomic<bool> dawSyncEnabled_{ true };
    std::atomic<bool> dawArmSequencer_{ false };
    bool prevHostPlaying_  = false;
    bool dawTriggeredDemo_ = false;

    std::atomic<bool>     demoPlaying_{ false };
    std::atomic<int>      demoSongIdx_{ 0 };
    std::atomic<int>      demoTempoBpm_{ 120 };
    std::atomic<int>      demoMeasure_{ 1 };
    std::atomic<int>      demoTotalMeasures_{ 1 };
    std::atomic<int>      demoTimeSigNum_{ 4 };
    std::atomic<int>      demoTimeSigDen_{ 4 };
    std::atomic<uint16_t> seqTrackDataMask_{ 0u };
    std::atomic<uint64_t> demoKeyMask61_{ 0ULL };
    std::atomic<uint64_t> seqDispatchedNotes_{ 0ULL };
    std::array<std::atomic<uint8_t>, 9> seqTrackMeterLevels_{};

    // Hardware Utility Setup & Rhythm Key Assign Atomics
    std::atomic<int>     globalTransposeSemitones_{ 0 };
    std::atomic<bool>    memProtectInt_{ false };
    std::atomic<bool>    memProtectCard_{ false };
    std::atomic<uint8_t> activeRhythmKeyLayout_{ 0u };
    std::atomic<int>     pendingRhythmKeyLayout_{ -1 };

    // Classic Rhythm Machine Lock-Free Command & Telemetry Queues
    struct ClassicRhythmCmd {
        enum class Type : uint8_t {
            None = 0,
            StartFactoryPattern,
            StartUserPattern,
            QueueFactoryPattern,
            QueueUserPattern,
            UpdateUserPatternInPlace,
            CommitUserPatternOnly,
            StopAll,
            RewindPattern,
            StartSong,
            StopSong,
            SetTempo,
            SetMetronome,
            ApplyKeyMap
        };
        Type                             type = Type::None;
        uint8_t                          index = 0u;
        bool                             flag = false;
        uint16_t                         u16A = 0u;
        uint16_t                         u16B = 0u;
        double                           tempoBpm = 120.0;
        vf50::state::UserRhythmPattern    pattern{};
        std::array<int8_t, 61>           keyMap{};
    };
    static constexpr std::size_t kClassicRhyCmdQueueSize = 16u;
    std::array<ClassicRhythmCmd, kClassicRhyCmdQueueSize> classicRhyCmdQueue_{};
    std::atomic<uint32_t> classicRhyCmdWrite_{ 0u };
    std::atomic<uint32_t> classicRhyCmdRead_{ 0u };
    bool pushClassicRhythmCmd(const ClassicRhythmCmd& cmd) noexcept;
    void processClassicRhythmCmdsRealTime() noexcept;

    std::array<std::unique_ptr<vf50::generated::RhythmSongData>, 2> classicSongSlots_{};
    std::atomic<int> pendingClassicSongSlot_{ -1 };
    int nextClassicSongWriteSlot_ = 0;

    std::unique_ptr<std::array<vf50::generated::RhythmSongData, 8>> hostRhythmSongs_{};
    std::unique_ptr<std::array<vf50::generated::RhythmSongData, 8>> audioRhythmSongs_{};
    std::array<std::unique_ptr<std::array<vf50::generated::RhythmSongData, 8>>, 2> rhythmSongsSlots_{};
    std::atomic<int> pendingRhythmSongsSlot_{ -1 };
    int nextRhythmSongsWriteSlot_ = 0;

    std::array<std::unique_ptr<vf50::state::RhythmKitBank>, 2> classicRhyKitSlots_{};
    std::atomic<int> pendingClassicRhyKitSlot_{ -1 };
    int nextClassicRhyKitWriteSlot_ = 0;

    static constexpr std::size_t kRhythmUiNoteQueueSize = 32u;
    std::array<RhythmUiNoteEvent, kRhythmUiNoteQueueSize> rhyUiNoteQueue_{};
    std::atomic<uint32_t> rhyUiNoteWrite_{ 0u };
    std::atomic<uint32_t> rhyUiNoteRead_{ 0u };

    std::atomic<int>      classicRhythmKbdOverride_{ -1 };
    std::atomic<bool>     classicRhythmSyncMidi_{ false };
    std::atomic<bool>     classicCaptureRhythmNotes_{ false };
    std::atomic<uint8_t>  rhyVoiceHitMask_{ 0u };
    std::atomic<uint8_t>  rhyActiveVoiceMask_{ 0u };
    std::atomic<bool>     rhyEnginePlaying_{ false };
    std::atomic<bool>     rhySongPlaying_{ false };
    std::atomic<bool>     rhyNextPatternQueued_{ false };
    std::atomic<uint16_t> rhyCurrentTick_{ 0u };
    std::atomic<uint32_t> rhyPatternWrapCount_{ 0u };
    std::atomic<uint16_t> rhySongMeas_{ 1u };
    std::atomic<uint16_t> rhySongPart_{ 1u };
    std::atomic<bool>     rhySongPtnIsUser_{ false };
    std::atomic<uint8_t>  rhySongPtnIdx_{ 0u };
    std::atomic<uint32_t> rhySongBarsPlayed_{ 0u };
    std::atomic<uint64_t> rhyTotalTriggers_{ 0u };
    std::atomic<int>      rhyLiveTempoBpm_{ 120 };

    // Persisted UI Preferences
    std::atomic<float>    uiScaleFactor_{ 1.00f };
    std::atomic<bool>     virtualKeyboardVisible_{ false };
    std::atomic<bool>     programmerVisible_{ false };
    std::atomic<bool>     mixerVisible_{ false };
    std::atomic<int>      lcdThemeIndex_{ 0 };
    std::atomic<uint32_t> uiPreferencesEpoch_{ 0u };
    std::atomic<uint32_t> patchSyncEpoch_{ 0u };

    // Runtime Firmware ROM Bank & Wait-Free Audio Thread Publish
    vf50::rom::VF50RomSetBuilder romSetBuilder_{};
    std::vector<std::unique_ptr<vf50::rom::VF50RomImage>> romHistory_{};
    const vf50::rom::VF50RomImage* hostRomImage_ = &vf50::rom::getDefaultRomImage();
    std::atomic<const vf50::rom::VF50RomImage*> pendingRomImagePtr_{ nullptr };
    std::atomic<bool> pendingRomReloadDefaults_{ true };
    std::atomic<uint32_t> romEpoch_{ 0u };

    // Host/Audio Classic System, Perf Effects, PC Table, Virtual Card & MIDI Out Queues
    struct ClassicAudioSystemSnapshot {
        vf50::state::VF50SystemSettings                          sys{};
        vf50::state::PerfEffectBank                             pefct{};
        vf50::state::ProgramChangeTable                         pct{};
        std::array<vf50::generated::FactoryVoice, 100>          cardVoices{};
        std::array<vf50::generated::FactoryPerformance, 100>    cardPerfs{};
    };
    vf50::state::VF50SystemSettings classicSysSettings_{};
    vf50::state::PerfEffectBank perfEffectBank_{ vf50::state::makeFactoryPerfEffectBank() };
    vf50::state::ProgramChangeTable pgmChangeTable_{};
    std::unique_ptr<vf50::state::VF50VirtualCardState> virtualCard_{};

    std::array<std::unique_ptr<ClassicAudioSystemSnapshot>, 2> classicAudioSysSlots_{};
    std::unique_ptr<ClassicAudioSystemSnapshot> classicAudioSys_{};
    std::atomic<int> pendingClassicSysSlot_{ -1 };
    int nextClassicSysWriteSlot_ = 0;

    static constexpr std::size_t kMidiOutQueueSize = 128u;
    std::array<UiMidiEvent, kMidiOutQueueSize> midiOutQueue_{};
    std::atomic<uint32_t> midiOutWriteIdx_{ 0u };
    std::atomic<uint32_t> midiOutReadIdx_{ 0u };

    std::array<std::unique_ptr<vf50::midi::SysExReplyBuffer>, 2> bulkSysExOutSlots_{};
    std::atomic<int> pendingBulkSysExSlot_{ -1 };
    int nextBulkSysExWriteSlot_ = 0;

    juce::MidiBuffer localKbdMidiBuf_{};
    juce::MidiBuffer mergedMidiBuf_{};

    std::atomic<uint8_t> classicAuditionPef_{ 0u };
    std::atomic<uint8_t> classicAuditionMicro_{ 0u };
    std::atomic<bool>    classicCardBankActive_{ false };

    [[nodiscard]] static BusesProperties makeVF50BusesProperties();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50AudioProcessor)
};

} // namespace vf50::plugin
