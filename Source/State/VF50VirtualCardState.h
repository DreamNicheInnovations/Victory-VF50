#pragma once

#include "DSP/VF50DemoSongPlayer.h"
#include "State/VF50ClassicSystemTypes.h"
#include "State/VF50MemoryManager.h"

#include <array>
#include <cstdint>

namespace vf50::state {

enum class CardMediaType : uint8_t {
    Mcd32Ram = 0, // 1 bank
    Mcd64Ram = 1, // 2 banks
    RomCard  = 2  // 1 bank, read-only
};

enum class CardBankFormat : uint8_t {
    Unformatted = 0, // unfmtd
    VF50Syn      = 1, // VF50SYN
    VF50Rhy      = 2  // VF50RHY
};

struct VirtualCardRhythmSet {
    bool                                                    hasAll         = false;
    bool                                                    hasRSeq        = false;
    bool                                                    hasSetup       = false;
    std::array<UserRhythmPattern, 100>                      patterns{};
    std::array<vf50::generated::RhythmSongData, 8>           songs{};
    RhythmKitBank                                           kit{};
    std::array<std::array<int8_t, 61>, 2>                   userKeyMaps{};
    uint8_t                                                 rhythmRch      = 9u;
    uint8_t                                                 rhythmTch      = 10u;
    bool                                                    rhythmVelOn    = true;
    bool                                                    rhythmSyncMidi = false;
    uint16_t                                                rhythmTempo    = 120u;
};

struct VirtualCardBank {
    CardBankFormat format = CardBankFormat::Unformatted;

    // Synthesizer bank data (VF50SYN)
    bool                                       hasVoicesPerfs = false;
    std::array<StoredVoiceSlot, 100>           voices{};
    std::array<StoredPerformanceSlot, 100>     performances{};

    bool                                       hasPct         = false;
    ProgramChangeTable                         pct{};

    bool                                       hasPefct       = false;
    PerfEffectBank                             pefct{};

    bool                                       hasMct         = false;
    std::array<vf50::generated::MicrotuningEntry, 12>  microOct{};
    std::array<vf50::generated::MicrotuningEntry, 128> microFull{};

    bool                                       hasSys         = false;
    VF50SystemSettings                          sys{};
    bool                                       sysIntProtect  = false;
    bool                                       sysCardProtect = false;

    // Rhythm bank data (VF50RHY: two independent sets A and B)
    std::array<VirtualCardRhythmSet, 2>        rhythmSets{};

    // Optional YS200/B200 sequence data for SEQ(YS) load (p. 98)
    bool                                       hasYsSeq       = false;
    vf50::dsp::CustomSeqSongBuffer              ysSeq{};
};

struct VF50VirtualCardState {
    CardMediaType mediaType            = CardMediaType::Mcd64Ram;
    bool          hardwareWriteProtect = false;
    bool          batteryLowWarning    = false;
    uint8_t       activeBankIndex      = 0u; // 0 = Bank 1, 1 = Bank 2 (MCD64)
    std::array<VirtualCardBank, 2> banks{};

    [[nodiscard]] int bankCount() const noexcept {
        return (mediaType == CardMediaType::Mcd64Ram) ? 2 : 1;
    }

    [[nodiscard]] bool isReadOnlyMedia() const noexcept {
        return mediaType == CardMediaType::RomCard;
    }

    [[nodiscard]] VirtualCardBank& activeBank() noexcept {
        const int idx = (mediaType == CardMediaType::Mcd64Ram) ? static_cast<int>(activeBankIndex & 1u) : 0;
        return banks[static_cast<std::size_t>(idx)];
    }

    [[nodiscard]] const VirtualCardBank& activeBank() const noexcept {
        const int idx = (mediaType == CardMediaType::Mcd64Ram) ? static_cast<int>(activeBankIndex & 1u) : 0;
        return banks[static_cast<std::size_t>(idx)];
    }
};

} // namespace vf50::state
