#pragma once

#include "ROM/VF50DataTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#ifndef VF50_EMBEDDED_ROMS
    #define VF50_EMBEDDED_ROMS 0
#endif

namespace vf50::rom {

struct VF50RomImage {
    // 4 External Hardware EPROM chips (Bring-Your-Own-ROM):
    //   0: IC712 XF805 C0.BIN     (131,072 B) Main CPU ROM A
    //   1: IC723 XF810 C0.BIN     (131,072 B) Main CPU ROM B
    //   2: IC731 XF804 B0.BIN     ( 32,768 B) Sub-CPU ROM
    //   3: XF653A0-101 IC738.BIN  (524,288 B) YM3602 OPRW Drum Waveform ROM
    bool hasMainRomA    = false;
    bool hasMainRomB    = false;
    bool hasSubCpuRom   = false;
    bool hasDrumWaveRom = false;

    std::array<char, 40> firmwareVersion{ { 'R','O','M','-','F','R','E','E',' ','I','N','I','T','\0' } };
    std::array<std::array<char, 260>, 4> chipSourcePaths{};

    // String storage backing all std::string_view fields cleanly within this object
    std::array<std::array<char, 16>, vf50::generated::kFactoryVoiceCount>       voiceNames{};
    std::array<std::array<char, 16>, vf50::generated::kFactoryPerformanceCount> perfNames{};
    std::array<std::array<char, 16>, vf50::generated::kFactoryMicrotuningCount> microNames{};
    std::array<std::array<char, 20>, 33>                                      dspEffectNameStorage{};
    std::array<std::array<char, 8>,  vf50::generated::kDrumInstrumentCount>    drumSetupNameStorage{};
    std::array<std::array<char, 12>, vf50::generated::kNumDrumWaveforms>       drumWaveNameStorage{};
    std::array<std::array<char, 8>,  vf50::generated::kNumDrumWaveforms>       drumLcdNameStorage{};
    std::array<std::array<char, 16>, vf50::generated::kDemoSongCount>          demoTitleStorage{};

    // Decoded Hardware Banks & Tables
    std::array<vf50::generated::FactoryVoice, vf50::generated::kFactoryVoiceCount>             factoryVoices{};
    std::array<vf50::generated::FactoryPerformance, vf50::generated::kFactoryPerformanceCount> factoryPerformances{};
    std::array<vf50::generated::MicrotuningTable, vf50::generated::kFactoryMicrotuningCount>   factoryMicrotunings{};
    std::array<std::string_view, 33>                                                         dspEffectNames{};
    std::array<vf50::generated::DspEffectPreset, vf50::generated::kDspEffectPresetCount>       factoryDspPresets{};

    std::array<vf50::generated::DrumInstrumentSetup, vf50::generated::kDrumInstrumentCount>    defaultDrumSetup{};
    std::array<std::array<int8_t, 61>, 3>                                                    presetDrumKeyMaps{};
    std::array<vf50::generated::FactoryRhythmPattern, vf50::generated::kFactoryRhythmPtnCount> factoryRhythmPatterns{};
    std::vector<vf50::generated::RhythmNoteEvent>                                             decodedRhythmEvents{};
    std::vector<uint8_t>                                                                     rhythmEventStream{};

    std::array<vf50::generated::DrumWaveformDescriptor, vf50::generated::kNumDrumWaveforms>   drumWaveforms{};
    std::array<uint32_t, vf50::generated::kNumDrumWaveforms>                                  drumPcmOffsets{};
    std::vector<int16_t>                                                                     drumPcmPool{};

    std::array<vf50::generated::FactoryDemoSong, vf50::generated::kDemoSongCount>              factoryDemoSongs{};
    std::vector<vf50::generated::DemoSongEvent>                                               demoSongEvents{};

    // OPZ II Hardware LUTs (algorithmic defaults when ROM-free; exact IC712/IC731 tables when loaded)
    std::array<uint8_t, 64> coarsePermTable{};
    std::array<uint8_t, 20> lowOutLevelToTl{};

    void initCleanRoomBlank() noexcept;

    // Rebinds all internal string_view and sample buffer pointers after population/copy
    void rebindPointers() noexcept;

    [[nodiscard]] bool isAllRomsLoaded() const noexcept {
        return hasMainRomA && hasMainRomB && hasSubCpuRom && hasDrumWaveRom;
    }

    [[nodiscard]] int loadedChipCount() const noexcept {
        return static_cast<int>(hasMainRomA)
             + static_cast<int>(hasMainRomB)
             + static_cast<int>(hasSubCpuRom)
             + static_cast<int>(hasDrumWaveRom);
    }

    [[nodiscard]] std::span<const vf50::generated::RhythmNoteEvent> getRhythmPatternEvents(std::size_t patternIndex) const noexcept {
        const std::size_t idx = (patternIndex < factoryRhythmPatterns.size()) ? patternIndex : 0u;
        const auto& p = factoryRhythmPatterns[idx];
        const std::size_t endOff = static_cast<std::size_t>(p.eventOffset) + static_cast<std::size_t>(p.eventCount);
        if (p.eventCount == 0u || endOff > decodedRhythmEvents.size()) {
            return {};
        }
        return std::span<const vf50::generated::RhythmNoteEvent>(decodedRhythmEvents.data() + p.eventOffset, p.eventCount);
    }

    [[nodiscard]] std::span<const vf50::generated::DemoSongEvent> getDemoSongEvents(std::size_t songIndex) const noexcept {
        const std::size_t idx = (songIndex < factoryDemoSongs.size()) ? songIndex : 0u;
        const auto& s = factoryDemoSongs[idx];
        const std::size_t endOff = static_cast<std::size_t>(s.eventOffset) + static_cast<std::size_t>(s.eventCount);
        if (s.eventCount == 0u || endOff > demoSongEvents.size()) {
            return {};
        }
        return std::span<const vf50::generated::DemoSongEvent>(demoSongEvents.data() + s.eventOffset, s.eventCount);
    }
};

// Returns clean-room baseline image (always zero embedded ROM bytes)
[[nodiscard]] const VF50RomImage& getDefaultRomImage() noexcept;

// Allocates a fresh clean-room firmware-free image (always zero ROM bytes)
[[nodiscard]] std::unique_ptr<VF50RomImage> createCleanRoomBlankRomImage();

// Clean-room mathematical OPZ II default tables (zero copyright ROM contents)
[[nodiscard]] std::array<uint8_t, 64> makeCleanRoomCoarsePermTable() noexcept;
[[nodiscard]] std::array<uint8_t, 20> makeCleanRoomLowOutTlTable() noexcept;

} // namespace vf50::rom
