#pragma once

#include "DSP/VF50SynthEngine.h"
#include "State/VF50ClassicSystemTypes.h"
#include "State/VF50MemoryManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vf50::midi {

// Pre-allocated, zero-allocation SysEx packet queue for real-time audio thread replies
struct SysExReplyBuffer {
    static constexpr std::size_t kMaxTotalBytes = 20480u;
    static constexpr std::size_t kMaxPackets    = 16u;

    struct PacketSpan {
        uint32_t offset = 0u;
        uint32_t length = 0u;
    };

    std::array<uint8_t, kMaxTotalBytes> storage{};
    std::array<PacketSpan, kMaxPackets> packets{};
    std::size_t bytesUsed   = 0u;
    std::size_t packetCount = 0u;

    void clear() noexcept {
        bytesUsed   = 0u;
        packetCount = 0u;
    }

    [[nodiscard]] bool appendPacket(const uint8_t* bytes, std::size_t len) noexcept {
        if (bytes == nullptr || len == 0u) return false;
        if (packetCount >= kMaxPackets || (bytesUsed + len) > kMaxTotalBytes) return false;
        for (std::size_t i = 0; i < len; ++i) {
            storage[bytesUsed + i] = bytes[i];
        }
        packets[packetCount].offset = static_cast<uint32_t>(bytesUsed);
        packets[packetCount].length = static_cast<uint32_t>(len);
        bytesUsed += len;
        ++packetCount;
        return true;
    }

    [[nodiscard]] const uint8_t* packetData(std::size_t index) const noexcept {
        return (index < packetCount) ? &storage[packets[index].offset] : nullptr;
    }

    [[nodiscard]] std::size_t packetSize(std::size_t index) const noexcept {
        return (index < packetCount) ? static_cast<std::size_t>(packets[index].length) : 0u;
    }

    void copyConcatenatedTo(std::vector<uint8_t>& out) const {
        out.insert(out.end(), storage.begin(), storage.begin() + static_cast<std::ptrdiff_t>(bytesUsed));
    }
};

enum class SysExEventKind : uint8_t {
    None                 = 0,
    InvalidOrIgnored     = 1,
    ParameterChange      = 2,
    SingleVoiceEditDump  = 3,
    SingleVoiceBulkDump  = 4,
    PerformanceEditDump  = 5,
    PerformanceBulkDump  = 6,
    MicrotuningOctDump   = 7,
    MicrotuningFullDump  = 8,
    RhythmSetupDump      = 9,
    DumpRequestServiced  = 10
};

struct SysExProcessResult {
    SysExEventKind kind         = SysExEventKind::None;
    bool accepted               = false;
    bool checksumValid          = true;
    bool modifiedSingleEdit     = false;
    bool modifiedUserVoices     = false;
    bool modifiedPerfEdit       = false;
    bool modifiedUserPerfs      = false;
    bool modifiedMicrotuning    = false;
    bool modifiedRhythm         = false;
    bool generatedReply         = false;
    std::size_t voicesLoadedCount = 0u;
};

class VF50SysExHandler {
public:
    static constexpr uint8_t kSysExStart      = 0xF0u;
    static constexpr uint8_t kSysExEnd        = 0xF7u;
    static constexpr uint8_t kSysExManufacturerId        = 0x43u;
    static constexpr uint8_t kDeviceOmni      = 16u;

    VF50SysExHandler() noexcept = default;

    void reset() noexcept;

    // 0..15 = MIDI Device Number 1..16, 16 = Omni (accept all device numbers, emit on Ch 1 / 0)
    void setDeviceChannel(uint8_t deviceCh0To16) noexcept;
    [[nodiscard]] uint8_t deviceChannel() const noexcept { return deviceChannel_; }
    void setDeviceEnabled(bool enabled) noexcept { deviceEnabled_ = enabled; }
    [[nodiscard]] bool isDeviceEnabled() const noexcept { return deviceEnabled_; }
    [[nodiscard]] uint8_t transmitChannelNibble() const noexcept {
        return (deviceChannel_ < 16u) ? deviceChannel_ : 0u;
    }

    // 7-bit two's complement 7-bit SysEx bulk checksum: (-sum(payload)) & 0x7F
    [[nodiscard]] static uint8_t computeSysExChecksum(const uint8_t* payload, std::size_t length) noexcept;

    // =========================================================================
    // Real-Time Lock-Free / Zero-Allocation SysEx Receiver & Request Dispatcher
    // =========================================================================
    SysExProcessResult processSysEx(const uint8_t* data,
                                    std::size_t length,
                                    vf50::state::VF50MemoryManager& memory,
                                    vf50::dsp::VF50SynthEngine& synth,
                                    SysExReplyBuffer* optionalReplyOut = nullptr) noexcept;

    // Convenience helper to parse a multi-message concatenated SysEx stream (e.g. .syx file)
    SysExProcessResult processSysExStream(const uint8_t* streamData,
                                          std::size_t streamLength,
                                          vf50::state::VF50MemoryManager& memory,
                                          vf50::dsp::VF50SynthEngine& synth,
                                          SysExReplyBuffer* optionalReplyOut = nullptr) noexcept;

    // =========================================================================
    // Zero-Allocation Hardware-Authentic VF50 / TX81Z / DX11 Emitters
    // =========================================================================

    // VF50 Single Voice Edit Dump: ACED3 + ACED2 + ACED + VCED (or subsets for DX11/TX81Z)
    bool emitSingleVoiceEditDump(const vf50::generated::FactoryVoice& voice,
                                 SysExReplyBuffer& out,
                                 bool includeAced = true,
                                 bool includeAced2 = true,
                                 bool includeAced3 = true) const noexcept;

    // Standard 32-Voice VMEM Bulk Dump (F0 43 0n 04 20 00 <4096B> <sum> F7, TX81Z/DX11/VF50 block)
    bool emitSingleVoiceBulkDump32(const vf50::state::VF50MemoryManager& memory,
                                   vf50::state::MemoryBank bank,
                                   std::size_t startIndex,
                                   SysExReplyBuffer& out) const noexcept;

    // Full VF50 100-Voice Internal Bank Dump (4 blocks: F0 43 1n 44 07 <1..4> F7 + VMEM)
    bool emitSingleVoiceBankDump100(const vf50::state::VF50MemoryManager& memory,
                                    vf50::state::MemoryBank bank,
                                    SysExReplyBuffer& out) const noexcept;

    // VF50 Performance Edit Dump: PCED2 + PCED (or standalone PCED for TX81Z/DX11)
    bool emitPerformanceEditDump(const vf50::generated::FactoryPerformance& perf,
                                 SysExReplyBuffer& out,
                                 bool includePced2 = true) const noexcept;

    // Single 32-Performance PMEM (+ optional PMEM2) Bulk Block
    bool emitPerformanceBulkBlock32(const vf50::state::VF50MemoryManager& memory,
                                    vf50::state::MemoryBank bank,
                                    std::size_t blockIndex0To3,
                                    SysExReplyBuffer& out,
                                    bool includeBlockHeader = true,
                                    bool includePmem2 = true) const noexcept;

    // Full VF50 100-Performance Bank Dump (4 blocks 0..3: block header + PMEM + PMEM2)
    bool emitPerformanceBankDump100(const vf50::state::VF50MemoryManager& memory,
                                    vf50::state::MemoryBank bank,
                                    SysExReplyBuffer& out) const noexcept;

    // Microtuning Bulk Dumps: MCRTE0 (12-Note Octave) & MCRTE1 (128-Key Full Keyboard)
    bool emitMicrotuningOctaveDump(const std::array<vf50::generated::MicrotuningEntry, 12>& table,
                                   SysExReplyBuffer& out) const noexcept;

    bool emitMicrotuningFullDump(const std::array<vf50::generated::MicrotuningEntry, 128>& table,
                                 SysExReplyBuffer& out) const noexcept;

    // Rhythm Dumps: SYSR (16B), RINST (183B), RKAT1/RKAT2 (61B)
    bool emitRhythmSystemDump(const vf50::state::VF50MemoryManager& memory,
                              const vf50::dsp::VF50SynthEngine& synth,
                              SysExReplyBuffer& out) const noexcept;

    bool emitRhythmInstrumentsDump(const vf50::state::RhythmKitBank& kit,
                                   SysExReplyBuffer& out) const noexcept;

    bool emitRhythmKeyMapDump(const std::array<int8_t, 61>& keyMap,
                              std::size_t userTable0Or1,
                              SysExReplyBuffer& out) const noexcept;

    // Range & Utility Bulk Dump Emitters (UT DUMP, Spec §2.5, pp. 103-104)
    bool emitVoiceRangeBulkDump(const std::array<vf50::state::StoredVoiceSlot, 100>& voices,
                                std::size_t startIdx,
                                std::size_t count,
                                SysExReplyBuffer& out) const noexcept;

    bool emitPerfRangeBulkDump(const std::array<vf50::state::StoredPerformanceSlot, 100>& perfs,
                               std::size_t startIdx,
                               std::size_t count,
                               SysExReplyBuffer& out) const noexcept;

    bool emitProgramChangeTableDump(const vf50::state::ProgramChangeTable& pct,
                                    SysExReplyBuffer& out) const noexcept;

    bool emitPerfEffectBankDump(const vf50::state::PerfEffectBank& pefct,
                                SysExReplyBuffer& out) const noexcept;

    bool emitSystemDataDump(const vf50::state::VF50SystemSettings& sys,
                            bool intProtect,
                            bool cardProtect,
                            SysExReplyBuffer& out) const noexcept;

    bool emitSequenceBulkDump(const vf50::dsp::CustomSeqSongBuffer* seq,
                              SysExReplyBuffer& out) const noexcept;

    bool emitRhythmSeqBulkDump(const vf50::state::VF50MemoryManager& memory,
                               SysExReplyBuffer& out) const noexcept;

private:
    struct StagedRefaceVoice {
        std::array<char, 10> nameChars = { 'V', 'O', 'I', 'C', 'E', ' ', ' ', ' ', ' ', ' ' };
        int8_t transpose = 0;
        uint8_t polyMono = 0;
        uint8_t pbRange = 2;
        uint8_t portaTime = 0;
        uint8_t algorithm = 0;
        uint8_t lfoWave = 1;
        uint8_t lfoSpeed = 35;
        uint8_t lfoDelay = 0;
        uint8_t lfoPmd = 0;
        std::array<uint8_t, 4> pegRates = { 39u, 39u, 127u, 39u };
        std::array<uint8_t, 4> pegLevels = { 64u, 64u, 64u, 64u };
        uint8_t fx1Type = 0u, fx1P1 = 0u, fx1P2 = 0u;
        uint8_t fx2Type = 0u, fx2P1 = 0u, fx2P2 = 0u;

        struct Op {
            uint8_t on = 1;
            uint8_t r1 = 127, r2 = 127, r3 = 11, r4 = 63;
            uint8_t l1 = 127, l2 = 127, l3 = 127, l4 = 0;
            uint8_t rateScaling = 0;
            uint8_t ld = 0, rd = 0, lc = 0, rc = 0;
            uint8_t lfoAmd = 0;
            uint8_t veloSens = 0;
            uint8_t outputLevel = 127;
            uint8_t feedback = 0;
            uint8_t fbType = 0;
            uint8_t freqMode = 0;
            uint8_t coarse = 1;
            uint8_t fine = 0;
            int8_t detune = 0;
        };
        std::array<Op, 4> operators{};
        uint8_t opsReceivedMask = 0;
        bool hasCommon = false;

        void clear() noexcept {
            nameChars = { 'V', 'O', 'I', 'C', 'E', ' ', ' ', ' ', ' ', ' ' };
            transpose = 0;
            polyMono = 0;
            pbRange = 2;
            portaTime = 0;
            algorithm = 0;
            lfoWave = 1;
            lfoSpeed = 35;
            lfoDelay = 0;
            lfoPmd = 0;
            pegRates = { 39u, 39u, 127u, 39u };
            pegLevels = { 64u, 64u, 64u, 64u };
            fx1Type = 0u; fx1P1 = 0u; fx1P2 = 0u;
            fx2Type = 0u; fx2P1 = 0u; fx2P2 = 0u;
            operators = {};
            opsReceivedMask = 0;
            hasCommon = false;
        }
    };

    struct StagedAced {
        struct OpExt {
            uint8_t fixedMode  = 0u;
            uint8_t fixedRange = 0u;
            uint8_t fine       = 0u;
            uint8_t waveform   = 0u;
            uint8_t egShift    = 0u;
        };
        std::array<OpExt, 4> ops{}; // Logical OP1..OP4
        uint8_t reverbRate = 0u;
        uint8_t fcPitch    = 0u;
        uint8_t fcAmp      = 0u;
    };

    struct StagedAced2 {
        uint8_t atPitch     = 0u;
        uint8_t atAmp       = 0u;
        int8_t  atPitchBias = 0;
        uint8_t atEgBias    = 0u;
        std::array<uint8_t, 4> fixedRangeLow{}; // Logical OP1..OP4
        uint8_t lsSignMask  = 0u;               // bit3=OP1, bit2=OP2, bit1=OP3, bit0=OP4
    };

    struct StagedPced2 {
        std::array<uint8_t, 8> reserveNotes{};
        std::array<uint8_t, 8> effectEnable{};
        vf50::generated::DspEffectSettings effect{};
    };

    [[nodiscard]] bool matchesDeviceChannel(uint8_t statusSubByte) const noexcept;

    SysExProcessResult handleBulkDump(const uint8_t* data,
                                      std::size_t length,
                                      vf50::state::VF50MemoryManager& memory,
                                      vf50::dsp::VF50SynthEngine& synth) noexcept;

    SysExProcessResult handleParameterChange(const uint8_t* data,
                                             std::size_t length,
                                             vf50::state::VF50MemoryManager& memory,
                                             vf50::dsp::VF50SynthEngine& synth) noexcept;

    SysExProcessResult handleDumpRequest(const uint8_t* data,
                                         std::size_t length,
                                         const vf50::state::VF50MemoryManager& memory,
                                         const vf50::dsp::VF50SynthEngine& synth,
                                         SysExReplyBuffer* replyOut) const noexcept;

    SysExProcessResult handleRefaceSysExPacket(const uint8_t* body,
                                               std::size_t innerLen,
                                               vf50::state::VF50MemoryManager& memory,
                                               vf50::dsp::VF50SynthEngine& synth) noexcept;

    static void applyStagedAcedToVoice(const StagedAced& aced, vf50::generated::FactoryVoice& v) noexcept;
    static void applyStagedAced2ToVoice(const StagedAced2& aced2, vf50::generated::FactoryVoice& v) noexcept;
    static void applyStagedPced2ToPerf(const StagedPced2& pced2, vf50::generated::FactoryPerformance& pf) noexcept;

    uint8_t deviceChannel_ = kDeviceOmni;
    bool deviceEnabled_ = true;

    // Multi-packet staging state (allocation-free)
    bool hasPendingAced_  = false;
    bool hasPendingAced2_ = false;
    bool hasPendingAced3_ = false;
    StagedAced pendingAced_{};
    StagedAced2 pendingAced2_{};
    vf50::generated::DspEffectSettings pendingAced3_{};

    bool hasPendingPced2_ = false;
    StagedPced2 pendingPced2_{};

    StagedRefaceVoice stagedRefaceVoice_{};
    int stagedSingleVoiceBankIndex_ = -1; // -1 = active Voice Edit Buffer (VMEM), 0..99 = User RAM bank slot

    int pendingVmemBlock_ = -1; // -1 = none (legacy 32-voice), 0 = 32-voice, 1..4 = VF50 blocks
    int autoVmemBlockCursor_ = 0;
    bool useAutoVmemCursor_ = false;
    int pendingPmemBlock_ = -1; // -1 = none (legacy block 0), 0..3 = VF50 blocks
    std::size_t lastPmemBaseSlot_ = 0u;
    std::size_t lastPmemSlotCount_ = 0u;
    std::array<std::array<uint8_t, 8>, 32> lastPmemMax1_{};
    std::array<uint8_t, 32> lastPmemAsmod1_{};
};

} // namespace vf50::midi
