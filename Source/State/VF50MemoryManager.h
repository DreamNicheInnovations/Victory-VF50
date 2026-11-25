#pragma once

#include "DSP/VF50SynthEngine.h"
#include "ROM/VF50RomBank.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace vf50::state {

using VF50VoiceData = vf50::generated::FactoryVoice;

// Complete VF50 front-panel workstation operating modes
enum class WorkstationMode : uint8_t {
    SinglePlay      = 0,
    SingleEdit      = 1,
    PerformancePlay = 2,
    PerformanceEdit = 3,
    RhythmPlay      = 4,
    EffectEdit      = 5
};

enum class MemoryBank : uint8_t {
    FactoryRom = 0, // Preset ROM (P00..P99 / PF P00..P99, read-only)
    UserRam    = 1  // Internal RAM (I00..I99 / PF I00..I99, editable & persistent)
};

// Authentic VF50 Performance Initialize templates (JOB -> INIT PERFORMANCE)
enum class PerfInitTemplate : uint8_t {
    All     = 0, // Full 8-part DVA initialized performance
    Single  = 1, // 16-voice single timbre on Ch 1 / Omni (SNGL)
    Dual    = 2, // Stereo detuned 2-part layer across keyboard (DUAL)
    Split   = 3, // Lower/Upper split at B2/C3 (SPLT)
    MCh8x2  = 4, // 8 Multitimbral parts on MIDI Ch 1..8 (SEQ8)
    MCh4x4  = 5, // 4 Multitimbral parts on MIDI Ch 1..4 (SEQ4)
    Poly4   = 6, // 4-part Alternate round-robin ensemble
    Layer4  = 7, // 4 layered instruments across full keyboard (4LYR)
    Layer8  = 8  // 8 layered instruments across full keyboard (8LYR)
};

// Self-owning Voice slot that guarantees `voice.name` always points to `nameChars`
struct StoredVoiceSlot {
    vf50::generated::FactoryVoice voice{};
    std::array<char, 10> nameChars{ {'I','N','I','T',' ','V','O','I','C','E'} };

    StoredVoiceSlot() noexcept;
    explicit StoredVoiceSlot(const vf50::generated::FactoryVoice& src) noexcept;
    StoredVoiceSlot(const StoredVoiceSlot& other) noexcept;
    StoredVoiceSlot& operator=(const StoredVoiceSlot& other) noexcept;
    StoredVoiceSlot(StoredVoiceSlot&& other) noexcept;
    StoredVoiceSlot& operator=(StoredVoiceSlot&& other) noexcept;

    void setFromVoice(const vf50::generated::FactoryVoice& src) noexcept;
    void overwriteCleanFromPreset(const VF50VoiceData& pristine) noexcept;
    void unpackFromRom95(const std::array<uint8_t, 95>& raw) noexcept;
    void setName(std::string_view newName) noexcept;
    void repackRomBytes() noexcept;
};

// Self-owning Performance slot with persistent `perf.name` and per-part fine panning
struct StoredPerformanceSlot {
    vf50::generated::FactoryPerformance perf{};
    std::array<char, 10> nameChars{ {'I','N','I','T',' ','P','E','R','F',' '} };
    std::array<int8_t, 8> partPans{}; // -64..+63 fine stereo pan extension per part

    StoredPerformanceSlot() noexcept;
    explicit StoredPerformanceSlot(const vf50::generated::FactoryPerformance& src) noexcept;
    StoredPerformanceSlot(const StoredPerformanceSlot& other) noexcept;
    StoredPerformanceSlot& operator=(const StoredPerformanceSlot& other) noexcept;
    StoredPerformanceSlot(StoredPerformanceSlot&& other) noexcept;
    StoredPerformanceSlot& operator=(StoredPerformanceSlot&& other) noexcept;

    void setFromPerformance(const vf50::generated::FactoryPerformance& src,
                            const std::array<int8_t, 8>& pans = {}) noexcept;
    void unpackFromRom97(const std::array<uint8_t, 97>& raw,
                         const std::array<int8_t, 8>& pans = {}) noexcept;
    void setName(std::string_view newName) noexcept;
    void repackRomBytes() noexcept;
};

// Editable User Rhythm Kit (all 61 YM3602 drum setups + global rhythm mix)
struct RhythmKitBank {
    std::array<char, 10> nameChars{ {'U','S','E','R',' ','K','I','T',' ','1'} };
    std::array<vf50::dsp::drums::DrumInstrumentState, vf50::generated::kDrumInstrumentCount> instruments{};
    uint8_t rhythmVolume = 85u;
    vf50::dsp::drums::RhythmEffectMode effectMode = vf50::dsp::drums::RhythmEffectMode::Mix;
    uint8_t keyMapSelect = 0u; // 0..2 = Preset Map 1..3, 3..4 = User Map 1..2

    [[nodiscard]] std::string_view name() const noexcept {
        return std::string_view(nameChars.data(), nameChars.size());
    }
    void setName(std::string_view newName) noexcept;
};

// Pre-allocated fixed-capacity User Rhythm Pattern (100% lock-free / zero heap allocations on audio thread)
struct UserRhythmPattern {
    static constexpr std::size_t kMaxEvents = 256u;

    uint8_t  bars       = 1u;
    uint8_t  timeSigNum = 4u;
    uint8_t  timeSigDen = 4u;
    uint16_t totalTicks = 192u; // 48 PPQN * 4 beats = 192 ticks per 4/4 bar
    uint16_t eventCount = 0u;
    std::array<vf50::generated::RhythmNoteEvent, kMaxEvents> events{};

    void clear() noexcept;
    bool addEventSorted(uint16_t tick, uint8_t drumIndex, uint8_t velocityLevel1To9) noexcept;
    bool eraseEventsAtTick(uint16_t tick, uint8_t drumIndex, uint16_t toleranceTicks = 0u) noexcept;
    bool eraseInstrument(uint8_t drumIndex) noexcept;
    [[nodiscard]] const vf50::generated::RhythmNoteEvent* findEventAtTick(uint16_t tick, uint8_t drumIndex) const noexcept;
    [[nodiscard]] vf50::generated::RhythmNoteEvent* findMutableEventAtTick(uint16_t tick, uint8_t drumIndex) noexcept;
};

class VF50MemoryManager {
public:
    static constexpr std::size_t kVoiceBankSize      = 100u;
    static constexpr std::size_t kPerfBankSize       = 100u;
    static constexpr std::size_t kUserRhythmKitCount = 4u;
    static constexpr std::size_t kUserKeyMapCount    = 2u;
    static constexpr std::size_t kRhythmPatternCount = 100u;

    VF50MemoryManager() noexcept;

    void setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadDefaults = true) noexcept;
    [[nodiscard]] const vf50::rom::VF50RomImage& runtimeRomBank() const noexcept {
        return (romBank_ != nullptr) ? *romBank_ : vf50::rom::getDefaultRomImage();
    }

    void resetAllUserRamToFactory() noexcept;

    // =========================================================================
    // Workstation Mode State Machine
    // =========================================================================
    void setWorkstationMode(WorkstationMode newMode) noexcept;
    [[nodiscard]] WorkstationMode workstationMode() const noexcept { return mode_; }
    [[nodiscard]] vf50::dsp::fm::SynthPlayMode effectiveFmPlayMode() const noexcept;
    [[nodiscard]] static std::string_view workstationModeName(WorkstationMode m) noexcept;

    // =========================================================================
    // Single Voice ROM (100) & User RAM (100) + Edit / Compare / Init
    // =========================================================================
    [[nodiscard]] const vf50::generated::FactoryVoice& getFactoryVoice(std::size_t index) const noexcept;
    [[nodiscard]] const vf50::generated::FactoryVoice& getUserVoice(std::size_t index) const noexcept;
    [[nodiscard]] const vf50::generated::FactoryVoice& getVoice(MemoryBank bank, std::size_t index) const noexcept;

    void selectSingleVoice(MemoryBank bank, std::size_t index) noexcept;
    [[nodiscard]] MemoryBank selectedVoiceBank() const noexcept { return selectedVoiceBank_; }
    [[nodiscard]] std::size_t selectedVoiceIndex() const noexcept { return selectedVoiceIndex_; }

    void storeUserVoice(std::size_t index, const vf50::generated::FactoryVoice& voice) noexcept;
    void storeEditBufferToUserVoice(std::size_t destUserIndex, std::string_view optionalNewName = {}) noexcept;

    [[nodiscard]] const vf50::generated::FactoryVoice& activeSingleVoice() const noexcept;
    [[nodiscard]] StoredVoiceSlot& singleEditSlot() noexcept { return singleEditBuffer_; }
    [[nodiscard]] const StoredVoiceSlot& singleEditSlot() const noexcept { return singleEditBuffer_; }
    void initSingleVoiceEditBuffer() noexcept;
    bool recallSingleVoiceEditBuffer() noexcept;
    void markSingleEditDirty() noexcept { singleEditDirty_ = true; }
    [[nodiscard]] bool isSingleEditDirty() const noexcept { return singleEditDirty_; }
    [[nodiscard]] bool hasSingleRecallBuffer() const noexcept { return hasSingleRecall_; }
    bool toggleSingleCompare() noexcept;
    void cancelSingleCompare() noexcept { singleCompareActive_ = false; }
    [[nodiscard]] bool isSingleCompareActive() const noexcept { return singleCompareActive_; }

    // Front-panel Quick Edit offsets & Single play live switches (reset to 0 on preset load)
    [[nodiscard]] const vf50::dsp::fm::QuickEditOffsets& quickEditOffsets() const noexcept { return quickEditOffsets_; }
    void setQuickEditOffsets(const vf50::dsp::fm::QuickEditOffsets& offsets) noexcept { quickEditOffsets_ = offsets; }
    void resetQuickEditOffsets() noexcept { quickEditOffsets_ = { 0, 0, 0, 0 }; }

    [[nodiscard]] uint8_t operatorEnableMask() const noexcept { return operatorEnableMask_; }
    void setOperatorEnableMask(uint8_t mask0To15) noexcept { operatorEnableMask_ = static_cast<uint8_t>(mask0To15 & 0x0Fu); }

    [[nodiscard]] bool singleEffectSend() const noexcept { return singleEffectSend_; }
    void setSingleEffectSend(bool enabled) noexcept { singleEffectSend_ = enabled; }

    // =========================================================================
    // Performance ROM (100) & User RAM (100) + Edit / Compare / Init
    // =========================================================================
    [[nodiscard]] const vf50::generated::FactoryPerformance& getFactoryPerformance(std::size_t index) const noexcept;
    [[nodiscard]] const vf50::generated::FactoryPerformance& getUserPerformance(std::size_t index) const noexcept;
    [[nodiscard]] const std::array<int8_t, 8>& getUserPerformancePartPans(std::size_t index) const noexcept;
    [[nodiscard]] const vf50::generated::FactoryPerformance& getPerformance(MemoryBank bank, std::size_t index) const noexcept;

    void selectPerformance(MemoryBank bank, std::size_t index) noexcept;
    [[nodiscard]] MemoryBank selectedPerfBank() const noexcept { return selectedPerfBank_; }
    [[nodiscard]] std::size_t selectedPerfIndex() const noexcept { return selectedPerfIndex_; }

    void storeUserPerformance(std::size_t index,
                              const vf50::generated::FactoryPerformance& perf,
                              const std::array<int8_t, 8>& partPans = {}) noexcept;
    void storeEditBufferToUserPerformance(std::size_t destUserIndex, std::string_view optionalNewName = {}) noexcept;

    [[nodiscard]] const vf50::generated::FactoryPerformance& activePerformance() const noexcept;
    [[nodiscard]] const std::array<int8_t, 8>& activePerformancePartPans() const noexcept;
    [[nodiscard]] StoredPerformanceSlot& perfEditSlot() noexcept { return perfEditBuffer_; }
    [[nodiscard]] const StoredPerformanceSlot& perfEditSlot() const noexcept { return perfEditBuffer_; }
    void initPerformanceEditBuffer(PerfInitTemplate tmpl = PerfInitTemplate::All) noexcept;
    bool recallPerformanceEditBuffer() noexcept;
    void markPerfEditDirty() noexcept { perfEditDirty_ = true; }
    [[nodiscard]] bool isPerfEditDirty() const noexcept { return perfEditDirty_; }
    [[nodiscard]] bool hasPerfRecallBuffer() const noexcept { return hasPerfRecall_; }
    bool togglePerformanceCompare() noexcept;
    void cancelPerformanceCompare() noexcept { perfCompareActive_ = false; }
    [[nodiscard]] bool isPerformanceCompareActive() const noexcept { return perfCompareActive_; }

    // =========================================================================
    // Rhythm Kits, Key Maps & 100 Factory / 100 User Rhythm Patterns
    // =========================================================================
    [[nodiscard]] const RhythmKitBank& factoryRhythmKit() const noexcept { return factoryRhythmKit_; }
    [[nodiscard]] const RhythmKitBank& activeRhythmKit() const noexcept;
    [[nodiscard]] RhythmKitBank& mutableActiveRhythmKit() noexcept { return rhythmEditKit_; }

    // kitSelect: 0 = Factory ROM Kit, 1..4 = User Kit 1..4
    void selectRhythmKit(std::size_t kitSelect0To4) noexcept;
    [[nodiscard]] std::size_t selectedRhythmKit() const noexcept { return selectedRhythmKit_; }
    void storeActiveKitToUserSlot(std::size_t userKitIdx0To3, std::string_view optionalName = {}) noexcept;
    void setUserRhythmKit(std::size_t userKitIdx0To3, const RhythmKitBank& kit) noexcept;
    [[nodiscard]] const RhythmKitBank& userRhythmKit(std::size_t userKitIdx0To3) const noexcept;

    // Key Maps: 0..2 = Preset Map 1..3, 3..4 = User Map 1..2 (RKAT1 / RKAT2)
    void selectRhythmKeyMap(std::size_t keyMapIdx0To4) noexcept;
    [[nodiscard]] std::size_t selectedRhythmKeyMap() const noexcept { return rhythmEditKit_.keyMapSelect; }
    void setUserKeyMap(std::size_t userMapIdx0To1, const std::array<int8_t, 61>& map) noexcept;
    [[nodiscard]] const std::array<int8_t, 61>& userKeyMap(std::size_t userMapIdx0To1) const noexcept;

    // Rhythm Patterns: Factory (0..99) & User RAM (0..99)
    void selectRhythmPattern(MemoryBank bank, std::size_t patternIdx0To99) noexcept;
    [[nodiscard]] MemoryBank selectedRhythmPatternBank() const noexcept { return selectedPatternBank_; }
    [[nodiscard]] std::size_t selectedRhythmPatternIndex() const noexcept { return selectedPatternIndex_; }

    [[nodiscard]] const UserRhythmPattern& userRhythmPattern(std::size_t index0To99) const noexcept;
    [[nodiscard]] UserRhythmPattern& mutableUserRhythmPattern(std::size_t index0To99) noexcept;
    void copyFactoryPatternToUserSlot(std::size_t factoryIdx, std::size_t userIdx) noexcept;

    // =========================================================================
    // User Microtuning Tables (Int Oct 12-Note & Int Full 128-Key)
    // =========================================================================
    void setUserOctaveMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 12>& tbl) noexcept;
    [[nodiscard]] const std::array<vf50::generated::MicrotuningEntry, 12>& userOctaveMicrotuning() const noexcept {
        return userOctaveMicrotuning_;
    }

    void setUserFullMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 128>& tbl) noexcept;
    [[nodiscard]] const std::array<vf50::generated::MicrotuningEntry, 128>& userFullMicrotuning() const noexcept {
        return userFullMicrotuning_;
    }
    void resetUserMicrotuningsToFactory() noexcept;

    // =========================================================================
    // Active Effect Edit Helper & Real-Time Engine Synchronization
    // =========================================================================
    [[nodiscard]] vf50::generated::DspEffectSettings activeEffectSettings() const noexcept;
    void updateActiveEffectSettings(const vf50::generated::DspEffectSettings& fx) noexcept;

    // Applies full RAM banks, Microtuning, Rhythm setup, Mode, and Edit buffers to VF50SynthEngine (lock-free, 0 allocs)
    void applyToSynthEngine(vf50::dsp::VF50SynthEngine& engine, bool syncEffects = true) const noexcept;
    void applyActiveSelectionToSynthEngine(vf50::dsp::VF50SynthEngine& engine, bool syncEffects = true) const noexcept;

    // =========================================================================
    // Binary DAW State Serialization / Deserialization (CRC32 Protected)
    // =========================================================================
    void serializeToBinary(std::vector<uint8_t>& outBytes) const;
    [[nodiscard]] bool deserializeFromBinary(const uint8_t* data, std::size_t numBytes) noexcept;

    // Canonical builders for INIT VOICE & INIT PERFORMANCE
    [[nodiscard]] static vf50::generated::FactoryVoice makeInitVoice() noexcept;
    [[nodiscard]] static vf50::generated::FactoryPerformance makeInitPerformance(PerfInitTemplate tmpl) noexcept;

private:
    const vf50::rom::VF50RomImage* romBank_ = &vf50::rom::getDefaultRomImage();
    WorkstationMode mode_ = WorkstationMode::SinglePlay;
    vf50::dsp::fm::SynthPlayMode underlyingFmMode_ = vf50::dsp::fm::SynthPlayMode::Single;

    MemoryBank selectedVoiceBank_ = MemoryBank::FactoryRom;
    std::size_t selectedVoiceIndex_ = 0u;
    bool singleCompareActive_ = false;
    vf50::dsp::fm::QuickEditOffsets quickEditOffsets_{ 0, 0, 0, 0 };
    uint8_t operatorEnableMask_ = 0x0Fu;
    bool singleEffectSend_ = true;

    MemoryBank selectedPerfBank_ = MemoryBank::FactoryRom;
    std::size_t selectedPerfIndex_ = 0u;
    bool perfCompareActive_ = false;

    std::size_t selectedRhythmKit_ = 0u; // 0 = Factory, 1..4 = User 1..4
    MemoryBank selectedPatternBank_ = MemoryBank::FactoryRom;
    std::size_t selectedPatternIndex_ = 0u;

    // Edit, Compare & Volatile Recall Buffers
    StoredVoiceSlot singleEditBuffer_{};
    StoredVoiceSlot singleCompareBackup_{};
    StoredVoiceSlot singleRecallBuffer_{};
    bool singleEditDirty_ = false;
    bool hasSingleRecall_ = false;
    StoredPerformanceSlot perfEditBuffer_{};
    StoredPerformanceSlot perfCompareBackup_{};
    StoredPerformanceSlot perfRecallBuffer_{};
    bool perfEditDirty_ = false;
    bool hasPerfRecall_ = false;

    // User RAM Banks (100 Single Voices + 100 Performances)
    std::array<StoredVoiceSlot, kVoiceBankSize> userVoices_{};
    std::array<StoredPerformanceSlot, kPerfBankSize> userPerformances_{};

    // Rhythm Kits, Key Maps & 100 User Rhythm Patterns
    RhythmKitBank factoryRhythmKit_{};
    RhythmKitBank rhythmEditKit_{};
    std::array<RhythmKitBank, kUserRhythmKitCount> userRhythmKits_{};
    std::array<std::array<int8_t, 61>, kUserKeyMapCount> userKeyMaps_{};
    std::array<UserRhythmPattern, kRhythmPatternCount> userRhythmPatterns_{};

    // User Microtuning RAM
    std::array<vf50::generated::MicrotuningEntry, 12> userOctaveMicrotuning_{};
    std::array<vf50::generated::MicrotuningEntry, 128> userFullMicrotuning_{};
};

} // namespace vf50::state
