#include "MIDI/VF50SysExHandler.h"
#include "DSP/FM/OpzTables.h"

#include <iostream>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace vf50::midi {

namespace {

// Hardware SysEx operator slot order: [OP4, OP2, OP3, OP1] -> logical index [0=OP1, 1=OP2, 2=OP3, 3=OP4]
constexpr std::array<std::size_t, 4> kSysExSlotForLogicalOp = { 3u, 1u, 2u, 0u };
constexpr std::array<std::size_t, 4> kLogicalOpForSysExSlot = { 3u, 1u, 2u, 0u };

[[nodiscard]] constexpr uint8_t encodeMidiAtPitchBias(int8_t signedBias) noexcept {
    // Hardware MIDI encoding (um_page_148 / um_page_151):
    // LCD   0 -> MIDI 0
    // LCD  +1..+50 -> MIDI 1..50
    // LCD -50..-1  -> MIDI 51..100
    const int clamped = std::clamp<int>(signedBias, -50, 50);
    if (clamped == 0) return 0u;
    if (clamped > 0) return static_cast<uint8_t>(clamped);
    return static_cast<uint8_t>(101 + clamped);
}

[[nodiscard]] constexpr int8_t decodeMidiAtPitchBias(uint8_t midiVal) noexcept {
    const int v = static_cast<int>(midiVal & 0x7Fu);
    if (v == 0) return 0;
    if (v <= 50) return static_cast<int8_t>(v);
    if (v <= 100) return static_cast<int8_t>(v - 101);
    return 0;
}

[[nodiscard]] constexpr uint8_t encodeVcedKvs(int8_t signedKvs) noexcept {
    // VCED KVS (um_page_148 / um_page_151):
    // 0..+7 -> 0..7, -7..-1 -> 8..14
    const int clamped = std::clamp<int>(signedKvs, -7, 7);
    return static_cast<uint8_t>(clamped < 0 ? (15 + clamped) : clamped);
}

[[nodiscard]] constexpr int8_t decodeVcedKvs(uint8_t vcedVal) noexcept {
    const int v = static_cast<int>(vcedVal & 0x0Fu);
    if (v <= 7) return static_cast<int8_t>(v);
    if (v <= 14) return static_cast<int8_t>(v - 15);
    return 0;
}

[[nodiscard]] bool matchAsciiTag10(const uint8_t* p, const char (&tag)[11]) noexcept {
    return std::memcmp(p, tag, 10u) == 0;
}

bool appendFramedBulkPacket(uint8_t txChannelNibble,
                            uint8_t formatCode,
                            const uint8_t* payload,
                            std::size_t payloadLen,
                            SysExReplyBuffer& out) noexcept {
    const std::size_t totalBytes = payloadLen + 8u;
    if (out.packetCount >= SysExReplyBuffer::kMaxPackets ||
        (out.bytesUsed + totalBytes) > SysExReplyBuffer::kMaxTotalBytes) {
        return false;
    }

    uint8_t* dst = &out.storage[out.bytesUsed];
    dst[0] = VF50SysExHandler::kSysExStart;
    dst[1] = VF50SysExHandler::kSysExManufacturerId;
    dst[2] = static_cast<uint8_t>(txChannelNibble & 0x0Fu); // 0n = Bulk Dump
    dst[3] = static_cast<uint8_t>(formatCode & 0x7Fu);
    dst[4] = static_cast<uint8_t>((payloadLen >> 7u) & 0x7Fu);
    dst[5] = static_cast<uint8_t>(payloadLen & 0x7Fu);
    if (payloadLen > 0u) {
        std::memcpy(dst + 6u, payload, payloadLen);
    }
    dst[6u + payloadLen] = VF50SysExHandler::computeSysExChecksum(payload, payloadLen);
    dst[7u + payloadLen] = VF50SysExHandler::kSysExEnd;

    out.packets[out.packetCount].offset = static_cast<uint32_t>(out.bytesUsed);
    out.packets[out.packetCount].length = static_cast<uint32_t>(totalBytes);
    out.bytesUsed += totalBytes;
    ++out.packetCount;
    return true;
}

bool appendLmBulkPacket(uint8_t txChannelNibble,
                        const char (&lmHeader)[11],
                        const uint8_t* body,
                        std::size_t bodyLen,
                        SysExReplyBuffer& out) noexcept {
    const std::size_t payloadLen = 10u + bodyLen;
    const std::size_t totalBytes = payloadLen + 8u;
    if (out.packetCount >= SysExReplyBuffer::kMaxPackets ||
        (out.bytesUsed + totalBytes) > SysExReplyBuffer::kMaxTotalBytes) {
        return false;
    }

    uint8_t* dst = &out.storage[out.bytesUsed];
    dst[0] = VF50SysExHandler::kSysExStart;
    dst[1] = VF50SysExHandler::kSysExManufacturerId;
    dst[2] = static_cast<uint8_t>(txChannelNibble & 0x0Fu);
    dst[3] = 0x7Eu;
    dst[4] = static_cast<uint8_t>((payloadLen >> 7u) & 0x7Fu);
    dst[5] = static_cast<uint8_t>(payloadLen & 0x7Fu);
    std::memcpy(dst + 6u, lmHeader, 10u);
    if (bodyLen > 0u) {
        std::memcpy(dst + 16u, body, bodyLen);
    }
    dst[6u + payloadLen] = VF50SysExHandler::computeSysExChecksum(dst + 6u, payloadLen);
    dst[7u + payloadLen] = VF50SysExHandler::kSysExEnd;

    out.packets[out.packetCount].offset = static_cast<uint32_t>(out.bytesUsed);
    out.packets[out.packetCount].length = static_cast<uint32_t>(totalBytes);
    out.bytesUsed += totalBytes;
    ++out.packetCount;
    return true;
}

void packVoiceToVmem128(const vf50::generated::FactoryVoice& v, uint8_t* out128) noexcept {
    std::memset(out128, 0, 128u);

    for (std::size_t logOp = 0; logOp < 4u; ++logOp) {
        const std::size_t slot = kSysExSlotForLogicalOp[logOp];
        const auto& o = v.operators[logOp];
        uint8_t* opB = &out128[slot * 10u];

        const int lsClamped = std::clamp<int>(o.levelScaling, -99, 99);
        const uint8_t lsMag = static_cast<uint8_t>(std::abs(lsClamped));
        const uint8_t lsNeg = (lsClamped < 0) ? 1u : 0u;

        const int kvsClamped = std::clamp<int>(o.keyVelSens, -7, 7);
        const uint8_t kvsMag = static_cast<uint8_t>(std::abs(kvsClamped));
        const uint8_t kvsNeg = (kvsClamped < 0) ? 1u : 0u;
        const uint8_t detRaw = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(o.detune) + 3, 0, 6));

        opB[0] = o.ar & 0x1Fu;
        opB[1] = o.d1r & 0x1Fu;
        opB[2] = o.d2r & 0x1Fu;
        opB[3] = std::clamp<uint8_t>(o.rr, 1u, 15u) & 0x0Fu;
        opB[4] = o.d1l & 0x0Fu;
        opB[5] = lsMag & 0x7Fu;
        opB[6] = static_cast<uint8_t>(((o.ampModEnable & 0x01u) << 6u)
                                    | ((o.egBiasSens & 0x07u) << 3u)
                                    | (kvsMag & 0x07u));
        opB[7] = std::min<uint8_t>(o.outputLevel, 99u);
        opB[8] = o.coarse & 0x3Fu;
        opB[9] = static_cast<uint8_t>((lsNeg << 6u) | (kvsNeg << 5u)
                                    | ((o.rateScaling & 0x03u) << 3u) | (detRaw & 0x07u));

        out128[73u + slot * 2u] = static_cast<uint8_t>(((o.fixedRangeLow & 0x01u) << 6u)
                                                     | ((o.egShift & 0x03u) << 4u)
                                                     | ((o.fixedFreqMode & 0x01u) << 3u)
                                                     | (o.fixedRange & 0x07u));
        out128[74u + slot * 2u] = static_cast<uint8_t>(((o.waveform & 0x07u) << 4u)
                                                     | (o.fine & 0x0Fu));
    }

    out128[40] = static_cast<uint8_t>(((v.lfoSync & 0x01u) << 6u)
                                    | ((v.feedback & 0x07u) << 3u)
                                    | (v.algorithm & 0x07u));
    out128[41] = std::min<uint8_t>(v.lfoSpeed, 99u);
    out128[42] = std::min<uint8_t>(v.lfoDelay, 99u);
    out128[43] = std::min<uint8_t>(v.pmd, 99u);
    out128[44] = std::min<uint8_t>(v.amd, 99u);
    out128[45] = static_cast<uint8_t>(((v.pms & 0x07u) << 4u)
                                    | ((v.ams & 0x03u) << 2u)
                                    | (v.lfoWave & 0x03u));
    out128[46] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(v.transpose) + 24, 0, 48));
    out128[47] = std::min<uint8_t>(v.pitchBendRange, 12u);
    out128[48] = static_cast<uint8_t>(((v.chorusSwitch & 0x01u) << 4u)
                                    | ((v.monoMode & 0x01u) << 3u)
                                    | ((v.sustainSwitch & 0x01u) << 2u)
                                    | ((v.portaSwitch & 0x01u) << 1u)
                                    | (v.portaMode & 0x01u));
    out128[49] = std::min<uint8_t>(v.portaTime, 99u);
    out128[50] = std::min<uint8_t>(v.fcVolume, 99u);
    out128[51] = std::min<uint8_t>(v.mwPitch, 99u);
    out128[52] = std::min<uint8_t>(v.mwAmp, 99u);
    out128[53] = std::min<uint8_t>(v.bcPitch, 99u);
    out128[54] = std::min<uint8_t>(v.bcAmp, 99u);
    out128[55] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(v.bcPitchBias) + 50, 0, 100));
    out128[56] = std::min<uint8_t>(v.bcEgBias, 99u);

    for (std::size_t i = 0; i < 10u; ++i) {
        char c = (i < v.name.size()) ? v.name[i] : ' ';
        if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) > 0x7Eu) {
            c = ' ';
        }
        out128[57u + i] = static_cast<uint8_t>(c);
    }

    out128[67] = std::min<uint8_t>(v.peg.pr1, 99u);
    out128[68] = std::min<uint8_t>(v.peg.pr2, 99u);
    out128[69] = std::min<uint8_t>(v.peg.pr3, 99u);
    out128[70] = std::min<uint8_t>(v.peg.pl1, 99u);
    out128[71] = std::min<uint8_t>(v.peg.pl2, 99u);
    out128[72] = std::min<uint8_t>(v.peg.pl3, 99u);

    out128[81] = v.reverbRate & 0x07u;
    out128[82] = std::min<uint8_t>(v.fcPitch, 99u);
    out128[83] = std::min<uint8_t>(v.fcAmp, 99u);
    out128[84] = std::min<uint8_t>(v.atPitch, 99u);
    out128[85] = std::min<uint8_t>(v.atAmp, 99u);
    out128[86] = encodeMidiAtPitchBias(v.atPitchBias);
    out128[87] = std::min<uint8_t>(v.atEgBias, 99u);

    // Bytes 88..93 are DX11/YS reserved (0); VF50 DSP Effect sits at 94..100 (um_page_151)
    out128[94] = std::min<uint8_t>(v.effect.effectType, 32u);
    out128[95] = std::min<uint8_t>(v.effect.balance, 100u);
    out128[96] = std::min<uint8_t>(v.effect.outLevel, 100u);
    out128[97] = v.effect.stereoMix & 0x01u;
    out128[98] = std::min<uint8_t>(v.effect.param1, 75u);
    out128[99] = std::min<uint8_t>(v.effect.param2, 99u);
    out128[100] = std::min<uint8_t>(v.effect.param3, 99u);
}

void unpackVoiceFromVmem128(const uint8_t* b, vf50::state::StoredVoiceSlot& outSlot) noexcept {
    vf50::generated::FactoryVoice v = vf50::state::VF50MemoryManager::makeInitVoice();

    for (std::size_t logOp = 0; logOp < 4u; ++logOp) {
        const std::size_t slot = kSysExSlotForLogicalOp[logOp];
        const uint8_t* opB = &b[slot * 10u];
        auto& o = v.operators[logOp];

        o.ar  = opB[0] & 0x1Fu;
        o.d1r = opB[1] & 0x1Fu;
        o.d2r = opB[2] & 0x1Fu;
        o.rr  = std::max<uint8_t>(1u, static_cast<uint8_t>(opB[3] & 0x0Fu));
        o.d1l = opB[4] & 0x0Fu;

        const int lsMag  = std::min<int>(opB[5] & 0x7Fu, 99);
        o.ampModEnable   = (opB[6] >> 6u) & 0x01u;
        o.egBiasSens     = (opB[6] >> 3u) & 0x07u;
        const int kvsMag = opB[6] & 0x07u;
        o.outputLevel    = std::min<uint8_t>(opB[7] & 0x7Fu, 99u);
        o.coarse         = opB[8] & 0x3Fu;

        const bool lsNeg  = ((opB[9] >> 6u) & 0x01u) != 0u;
        const bool kvsNeg = ((opB[9] >> 5u) & 0x01u) != 0u;
        o.rateScaling     = (opB[9] >> 3u) & 0x03u;
        const int detRaw  = std::clamp<int>(opB[9] & 0x07u, 0, 6);

        const uint8_t acedEven = b[73u + slot * 2u];
        const uint8_t acedOdd  = b[74u + slot * 2u];
        o.fixedRangeLow = (acedEven >> 6u) & 0x01u;
        o.egShift       = (acedEven >> 4u) & 0x03u;
        o.fixedFreqMode = (acedEven >> 3u) & 0x01u;
        o.fixedRange    = acedEven & 0x07u;
        o.waveform      = (acedOdd >> 4u) & 0x07u;
        o.fine          = acedOdd & 0x0Fu;

        o.levelScaling = static_cast<int8_t>((lsNeg && lsMag > 0) ? -lsMag : lsMag);
        o.keyVelSens   = static_cast<int8_t>((kvsNeg && kvsMag > 0) ? -kvsMag : kvsMag);
        o.detune       = static_cast<int8_t>(detRaw - 3);
    }

    v.lfoSync        = (b[40] >> 6u) & 0x01u;
    v.feedback       = (b[40] >> 3u) & 0x07u;
    v.algorithm      = b[40] & 0x07u;
    v.lfoSpeed       = std::min<uint8_t>(b[41] & 0x7Fu, 99u);
    v.lfoDelay       = std::min<uint8_t>(b[42] & 0x7Fu, 99u);
    v.pmd            = std::min<uint8_t>(b[43] & 0x7Fu, 99u);
    v.amd            = std::min<uint8_t>(b[44] & 0x7Fu, 99u);
    v.pms            = (b[45] >> 4u) & 0x07u;
    v.ams            = (b[45] >> 2u) & 0x03u;
    v.lfoWave        = b[45] & 0x03u;
    v.transpose      = static_cast<int8_t>(std::clamp<int>(static_cast<int>(b[46] & 0x3Fu) - 24, -24, 24));
    v.pitchBendRange = std::min<uint8_t>(b[47] & 0x0Fu, 12u);
    v.chorusSwitch   = (b[48] >> 4u) & 0x01u;
    v.monoMode       = (b[48] >> 3u) & 0x01u;
    v.sustainSwitch  = (b[48] >> 2u) & 0x01u;
    v.portaSwitch    = (b[48] >> 1u) & 0x01u;
    v.portaMode      = b[48] & 0x01u;
    v.portaTime      = std::min<uint8_t>(b[49] & 0x7Fu, 99u);
    v.fcVolume       = std::min<uint8_t>(b[50] & 0x7Fu, 99u);
    v.mwPitch        = std::min<uint8_t>(b[51] & 0x7Fu, 99u);
    v.mwAmp          = std::min<uint8_t>(b[52] & 0x7Fu, 99u);
    v.bcPitch        = std::min<uint8_t>(b[53] & 0x7Fu, 99u);
    v.bcAmp          = std::min<uint8_t>(b[54] & 0x7Fu, 99u);
    v.bcPitchBias    = static_cast<int8_t>(std::clamp<int>(static_cast<int>(b[55] & 0x7Fu) - 50, -50, 50));
    v.bcEgBias       = std::min<uint8_t>(b[56] & 0x7Fu, 99u);

    std::array<char, 10> nameBuf{};
    for (std::size_t i = 0; i < 10u; ++i) {
        const uint8_t ch = b[57u + i] & 0x7Fu;
        nameBuf[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
    }
    v.name = std::string_view(nameBuf.data(), nameBuf.size());

    // TX81Z compatibility: bytes 67..72 were unused 0x00 on TX81Z; normalize all-zero to neutral PEG
    const bool allZeroPeg = (b[67] == 0u && b[68] == 0u && b[69] == 0u &&
                             b[70] == 0u && b[71] == 0u && b[72] == 0u);
    if (allZeroPeg) {
        v.peg = { 99u, 99u, 99u, 50u, 50u, 50u };
    } else {
        v.peg.pr1 = std::min<uint8_t>(b[67] & 0x7Fu, 99u);
        v.peg.pr2 = std::min<uint8_t>(b[68] & 0x7Fu, 99u);
        v.peg.pr3 = std::min<uint8_t>(b[69] & 0x7Fu, 99u);
        v.peg.pl1 = std::min<uint8_t>(b[70] & 0x7Fu, 99u);
        v.peg.pl2 = std::min<uint8_t>(b[71] & 0x7Fu, 99u);
        v.peg.pl3 = std::min<uint8_t>(b[72] & 0x7Fu, 99u);
    }

    v.reverbRate  = b[81] & 0x07u;
    v.fcPitch     = std::min<uint8_t>(b[82] & 0x7Fu, 99u);
    v.fcAmp       = std::min<uint8_t>(b[83] & 0x7Fu, 99u);
    v.atPitch     = std::min<uint8_t>(b[84] & 0x7Fu, 99u);
    v.atAmp       = std::min<uint8_t>(b[85] & 0x7Fu, 99u);
    v.atPitchBias = decodeMidiAtPitchBias(b[86]);
    v.atEgBias    = std::min<uint8_t>(b[87] & 0x7Fu, 99u);

    v.effect.effectType = std::min<uint8_t>(b[94] & 0x3Fu, 32u);
    v.effect.balance    = std::min<uint8_t>(b[95] & 0x7Fu, 100u);
    v.effect.outLevel   = std::min<uint8_t>(b[96] & 0x7Fu, 100u);
    v.effect.stereoMix  = b[97] & 0x01u;
    v.effect.param1     = std::min<uint8_t>(b[98] & 0x7Fu, 75u);
    v.effect.param2     = std::min<uint8_t>(b[99] & 0x7Fu, 99u);
    v.effect.param3     = std::min<uint8_t>(b[100] & 0x7Fu, 99u);

    outSlot.setFromVoice(v);
}

void packPerfToPmem76AndPmem2_25(const vf50::generated::FactoryPerformance& pf,
                                 uint8_t* pmem76,
                                 uint8_t* pmem2_25) noexcept {
    std::memset(pmem76, 0, 76u);
    std::memset(pmem2_25, 0, 25u);

    std::array<uint8_t, 8> max2Bits{};
    for (std::size_t i = 0; i < 8u; ++i) {
        const auto& inst = pf.instruments[i];
        uint8_t* ib = &pmem76[i * 8u];

        // Hardware SysEx compatibility encoding (um_page_152):
        // MAX NOTES = 8 * MAX2 + MAX1 (<= 16); for <= 8 keep MAX2=0, MAX1=maxNotes (0..8) for DX11/TX81Z
        const uint8_t clampedMax = std::min<uint8_t>(inst.maxNotes, 16u);
        uint8_t max1 = 0u;
        uint8_t max2 = 0u;
        if (clampedMax <= 8u) {
            max1 = clampedMax;
            max2 = 0u;
        } else {
            max1 = static_cast<uint8_t>(clampedMax - 8u); // 1..8
            max2 = 1u;
        }
        max2Bits[i] = max2;

        ib[0] = static_cast<uint8_t>(((inst.outAssign & 0x03u) << 5u)
                                   | ((inst.voiceBank & 0x01u) << 4u)
                                   | (max1 & 0x0Fu));
        ib[1] = std::min<uint8_t>(inst.voiceNumber, 99u);
        ib[2] = static_cast<uint8_t>(((inst.lfoSelect & 0x03u) << 5u)
                                   | (std::min<uint8_t>(inst.receiveChannel, 17u) & 0x1Fu));
        ib[3] = std::min<uint8_t>(inst.limitLow, 127u);
        ib[4] = std::min<uint8_t>(inst.limitHigh, 127u);
        ib[5] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(inst.detune) + 7, 0, 14) & 0x0Fu);
        ib[6] = static_cast<uint8_t>(((inst.microTuneEnable & 0x01u) << 6u)
                                   | (std::clamp<int>(static_cast<int>(inst.noteShift) + 24, 0, 48) & 0x3Fu));
        ib[7] = std::min<uint8_t>(inst.volume, 99u);

        pmem2_25[i] = static_cast<uint8_t>(((inst.effectEnable & 0x01u) << 6u)
                                         | (std::min<uint8_t>(inst.reserveNotes, 17u) & 0x1Fu));
    }

    const uint8_t efsel = std::min<uint8_t>(pf.perfEffectSelect, 12u);
    uint8_t efsel2 = 0u;
    uint8_t efsel1 = 0u;
    if (efsel > 0u) {
        efsel2 = static_cast<uint8_t>((efsel - 1u) / 3u) & 0x03u;
        efsel1 = static_cast<uint8_t>(((efsel - 1u) % 3u) + 1u) & 0x03u;
    }

    const uint8_t asmod  = std::min<uint8_t>(pf.assignMode, 2u);
    const uint8_t asmod1 = asmod & 0x01u;
    const uint8_t asmod2 = (asmod >> 1u) & 0x01u;

    pmem76[64] = static_cast<uint8_t>((efsel2 << 4u) | (std::min<uint8_t>(pf.microTuneTable, 12u) & 0x0Fu));
    pmem76[65] = static_cast<uint8_t>(((std::min<uint8_t>(pf.microTuneRootKey, 11u) & 0x0Fu) << 3u)
                                    | (efsel1 << 1u) | asmod1);

    for (std::size_t i = 0; i < 10u; ++i) {
        char c = (i < pf.name.size()) ? pf.name[i] : ' ';
        if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) > 0x7Eu) {
            c = ' ';
        }
        pmem76[66u + i] = static_cast<uint8_t>(c);
    }

    pmem2_25[8]  = std::min<uint8_t>(pf.effect.effectType, 32u);
    pmem2_25[9]  = std::min<uint8_t>(pf.effect.balance, 100u);
    pmem2_25[10] = std::min<uint8_t>(pf.effect.outLevel, 100u);
    pmem2_25[11] = pf.effect.stereoMix & 0x01u;
    pmem2_25[12] = std::min<uint8_t>(pf.effect.param1, 75u);
    pmem2_25[13] = std::min<uint8_t>(pf.effect.param2, 99u);
    pmem2_25[14] = std::min<uint8_t>(pf.effect.param3, 99u);
    pmem2_25[15] = 0u;
    pmem2_25[16] = asmod2;
    for (std::size_t pair = 0; pair < 4u; ++pair) {
        pmem2_25[17u + pair] = static_cast<uint8_t>(((max2Bits[pair * 2u] & 0x03u) << 2u)
                                                  | (max2Bits[pair * 2u + 1u] & 0x03u));
    }
}

void unpackPerfFromPmem76(const uint8_t* pmem76,
                          vf50::generated::FactoryPerformance& pf,
                          std::array<uint8_t, 8>& outMax1,
                          uint8_t& outAsmod1) noexcept {
    pf = vf50::state::VF50MemoryManager::makeInitPerformance(vf50::state::PerfInitTemplate::All);

    for (std::size_t i = 0; i < 8u; ++i) {
        const uint8_t* ib = &pmem76[i * 8u];
        auto& inst = pf.instruments[i];

        const uint8_t max1 = std::min<uint8_t>(ib[0] & 0x0Fu, 16u);
        outMax1[i]           = max1;
        inst.maxNotes        = max1;
        inst.voiceBank       = (ib[0] >> 4u) & 0x01u;
        inst.outAssign       = (ib[0] >> 5u) & 0x03u;
        inst.voiceNumber     = std::min<uint8_t>(ib[1] & 0x7Fu, 99u);
        inst.receiveChannel  = std::min<uint8_t>(ib[2] & 0x1Fu, 17u);
        inst.lfoSelect       = (ib[2] >> 5u) & 0x03u;
        inst.limitLow        = ib[3] & 0x7Fu;
        inst.limitHigh       = ib[4] & 0x7Fu;
        inst.detune          = static_cast<int8_t>(std::clamp<int>(static_cast<int>(ib[5] & 0x0Fu) - 7, -7, 7));
        inst.noteShift       = static_cast<int8_t>(std::clamp<int>(static_cast<int>(ib[6] & 0x3Fu) - 24, -24, 24));
        inst.microTuneEnable = (ib[6] >> 6u) & 0x01u;
        inst.volume          = std::min<uint8_t>(ib[7] & 0x7Fu, 99u);
        inst.reserveNotes    = 0u;
        inst.effectEnable    = 1u;
    }

    pf.microTuneTable   = std::min<uint8_t>(pmem76[64] & 0x0Fu, 12u);
    const uint8_t efs2  = (pmem76[64] >> 4u) & 0x03u;
    const uint8_t efs1  = (pmem76[65] >> 1u) & 0x03u;
    pf.perfEffectSelect = (efs1 != 0u) ? std::min<uint8_t>(static_cast<uint8_t>(efs2 * 3u + efs1), 12u) : 0u;
    pf.microTuneRootKey = std::min<uint8_t>((pmem76[65] >> 3u) & 0x0Fu, 11u);
    outAsmod1           = pmem76[65] & 0x01u;
    pf.assignMode       = outAsmod1;
}

void overlayPmem2_25(const uint8_t* pmem2_25,
                     const std::array<uint8_t, 8>& max1,
                     uint8_t asmod1,
                     vf50::generated::FactoryPerformance& pf) noexcept {
    for (std::size_t i = 0; i < 8u; ++i) {
        auto& inst = pf.instruments[i];
        inst.reserveNotes = std::min<uint8_t>(pmem2_25[i] & 0x1Fu, 17u);
        inst.effectEnable = (pmem2_25[i] >> 6u) & 0x01u;

        const uint8_t pairByte = pmem2_25[17u + (i / 2u)];
        const uint8_t max2 = ((i % 2u) == 0u) ? ((pairByte >> 2u) & 0x03u) : (pairByte & 0x03u);
        inst.maxNotes = std::min<uint8_t>(static_cast<uint8_t>(8u * max2 + max1[i]), 16u);
    }

    pf.effect.effectType = std::min<uint8_t>(pmem2_25[8] & 0x3Fu, 32u);
    pf.effect.balance    = std::min<uint8_t>(pmem2_25[9] & 0x7Fu, 100u);
    pf.effect.outLevel   = std::min<uint8_t>(pmem2_25[10] & 0x7Fu, 100u);
    pf.effect.stereoMix  = pmem2_25[11] & 0x01u;
    pf.effect.param1     = std::min<uint8_t>(pmem2_25[12] & 0x7Fu, 75u);
    pf.effect.param2     = std::min<uint8_t>(pmem2_25[13] & 0x7Fu, 99u);
    pf.effect.param3     = std::min<uint8_t>(pmem2_25[14] & 0x7Fu, 99u);

    const uint8_t asmod2 = pmem2_25[16] & 0x01u;
    pf.assignMode = std::min<uint8_t>(static_cast<uint8_t>((asmod2 << 1u) | (asmod1 & 0x01u)), 2u);
}

void findBestOpzRatio(double targetRatio, uint8_t& outCoarse, uint8_t& outFine) noexcept {
    double bestDiff  = 1.0e9;
    uint8_t bestCrs  = 4u;
    uint8_t bestFine = 0u;
    const double zeroFineTol = (targetRatio >= 1.0) ? 0.005 : 0.0025;
    for (uint8_t c = 0u; c < 64u; ++c) {
        for (uint8_t f = 0u; f < 16u; ++f) {
            const double r    = vf50::dsp::fm::computeOpzRatioMultiplier(c, f);
            const double diff = std::abs(r - targetRatio);
            const bool canonicalZeroFineMatch = (f == 0u && diff <= zeroFineTol && (bestFine != 0u || diff < bestDiff - 1.0e-9));
            const bool betterFit = (bestFine == 0u && bestDiff <= zeroFineTol)
                ? (f == 0u && diff < bestDiff - 1.0e-9)
                : (diff < bestDiff - 1.0e-9 || (std::abs(diff - bestDiff) <= 1.0e-9 && f < bestFine));
            if (canonicalZeroFineMatch || betterFit) {
                bestDiff = diff;
                bestCrs  = c;
                bestFine = f;
            }
        }
    }
    outCoarse = bestCrs;
    outFine   = bestFine;
}

void findBestOpzFixedHz(double targetHz,
                        uint8_t& outCoarse,
                        uint8_t& outFine,
                        uint8_t& outRange,
                        uint8_t& outLow) noexcept {
    if (targetHz <= 0.02) {
        outCoarse = 0u;
        outFine   = 0u;
        outRange  = 0u;
        outLow    = 1u;
        return;
    }
    double bestDiff  = 1.0e9;
    uint8_t bestCrs  = 4u;
    uint8_t bestFine = 0u;
    uint8_t bestRng  = 0u;
    uint8_t bestLow  = 0u;
    for (uint8_t low = 0u; low <= 1u; ++low) {
        for (uint8_t rng = 0u; rng < 8u; ++rng) {
            for (uint16_t m = 0u; m < 256u; ++m) {
                const uint8_t c  = static_cast<uint8_t>((m >> 4u) * 4u);
                const uint8_t f  = static_cast<uint8_t>(m & 0x0Fu);
                const double hz  = vf50::dsp::fm::computeOpzFixedFreqHz(c, f, rng, low);
                const double err = std::abs(hz - targetHz);
                if (err < bestDiff - 1.0e-9) {
                    bestDiff = err;
                    bestCrs  = c;
                    bestFine = f;
                    bestRng  = rng;
                    bestLow  = low;
                }
            }
        }
    }
    outCoarse = bestCrs;
    outFine   = bestFine;
    outRange  = bestRng;
    outLow    = bestLow;
}

void mapRefaceFxToVF50Effect(uint8_t fx1Type, uint8_t fx1P1, uint8_t fx1P2,
                            uint8_t fx2Type, uint8_t fx2P1, uint8_t fx2P2,
                            vf50::generated::DspEffectSettings& outFx) noexcept {
    auto applySlot = [&outFx](uint8_t type, uint8_t p1, uint8_t p2) noexcept -> bool {
        switch (type & 0x0Fu) {
            case 7u: { // Reverb -> VF50 Reverb Hall / Room
                outFx.effectType = (p2 >= 48u) ? 1u : 2u;
                outFx.balance    = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p1 * 100.0 / 127.0)), 10, 100));
                outFx.outLevel   = 85u;
                outFx.stereoMix  = 1u;
                outFx.param1     = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p2 * 38.0 / 127.0)), 3, 75));
                outFx.param2     = 10u;
                outFx.param3     = 16u;
                return true;
            }
            case 6u: { // Delay -> VF50 Delay L/R
                outFx.effectType = 5u;
                outFx.balance    = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p1 * 100.0 / 127.0)), 10, 100));
                outFx.outLevel   = 85u;
                outFx.stereoMix  = 1u;
                outFx.param1     = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p2 * 60.0 / 127.0)), 4, 75));
                outFx.param2     = 35u;
                outFx.param3     = 20u;
                return true;
            }
            case 2u:
            case 3u:
            case 4u: { // Touch Wah / Chorus / Flanger / Phaser -> Doubler 1
                outFx.effectType = 22u;
                outFx.balance    = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p1 * 100.0 / 127.0)), 15, 100));
                outFx.outLevel   = 85u;
                outFx.stereoMix  = 1u;
                outFx.param1     = 18u;
                outFx.param2     = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p2 * 80.0 / 127.0)), 5, 99));
                outFx.param3     = 20u;
                return true;
            }
            case 1u: { // Distortion -> Distortion Rev.
                outFx.effectType = 7u;
                outFx.balance    = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p1 * 100.0 / 127.0)), 20, 100));
                outFx.outLevel   = 80u;
                outFx.stereoMix  = 1u;
                outFx.param1     = 20u;
                outFx.param2     = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(p2 * 90.0 / 127.0)), 10, 99));
                outFx.param3     = 15u;
                return true;
            }
            default:
                return false;
        }
    };

    if (fx2Type != 0u && applySlot(fx2Type, fx2P1, fx2P2)) {
        return;
    }
    if (fx1Type != 0u && applySlot(fx1Type, fx1P1, fx1P2)) {
        return;
    }
}

void commitDecodedSingleVoice(const vf50::generated::FactoryVoice& v,
                              int& bankCursor,
                              vf50::state::VF50MemoryManager& memory,
                              vf50::dsp::VF50SynthEngine& synth,
                              SysExProcessResult& res) noexcept {
    if (bankCursor < 0) {
        memory.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
        memory.singleEditSlot().setFromVoice(v);
        memory.applyActiveSelectionToSynthEngine(synth, true);
        res.kind               = SysExEventKind::SingleVoiceEditDump;
        res.accepted           = true;
        res.modifiedSingleEdit = true;
        res.voicesLoadedCount  = 1u;
    } else {
        const std::size_t idx = static_cast<std::size_t>(bankCursor);
        if (idx < vf50::state::VF50MemoryManager::kVoiceBankSize) {
            memory.storeUserVoice(idx, v);
            synth.fm().storeInternalVoice(idx, memory.getUserVoice(idx));
            res.voicesLoadedCount = 1u;
        }
        if (bankCursor < static_cast<int>(vf50::state::VF50MemoryManager::kVoiceBankSize)) {
            ++bankCursor;
        }
        if (memory.selectedVoiceBank() == vf50::state::MemoryBank::UserRam) {
            memory.selectSingleVoice(vf50::state::MemoryBank::UserRam, memory.selectedVoiceIndex());
        }
        memory.applyActiveSelectionToSynthEngine(synth, true);
        res.kind               = SysExEventKind::SingleVoiceBulkDump;
        res.accepted           = true;
        res.modifiedUserVoices = true;
    }
}

} // namespace

// =============================================================================
// VF50SysExHandler Core Methods
// =============================================================================

void VF50SysExHandler::reset() noexcept {
    hasPendingAced_             = false;
    hasPendingAced2_            = false;
    hasPendingAced3_            = false;
    hasPendingPced2_            = false;
    pendingVmemBlock_           = -1;
    autoVmemBlockCursor_        = 0;
    useAutoVmemCursor_          = false;
    pendingPmemBlock_           = -1;
    lastPmemBaseSlot_           = 0u;
    lastPmemSlotCount_          = 0u;
    stagedRefaceVoice_.clear();
    stagedSingleVoiceBankIndex_ = -1;
}

void VF50SysExHandler::setDeviceChannel(uint8_t deviceCh0To16) noexcept {
    deviceChannel_ = std::min<uint8_t>(deviceCh0To16, kDeviceOmni);
}

uint8_t VF50SysExHandler::computeSysExChecksum(const uint8_t* payload, std::size_t length) noexcept {
    uint32_t sum = 0u;
    for (std::size_t i = 0; i < length; ++i) {
        sum += static_cast<uint32_t>(payload[i] & 0x7Fu);
    }
    return static_cast<uint8_t>((0u - sum) & 0x7Fu);
}

bool VF50SysExHandler::matchesDeviceChannel(uint8_t statusSubByte) const noexcept {
    if (!deviceEnabled_) return false;
    if (deviceChannel_ >= kDeviceOmni) return true;
    return (statusSubByte & 0x0Fu) == deviceChannel_;
}

void VF50SysExHandler::applyStagedAcedToVoice(const StagedAced& aced, vf50::generated::FactoryVoice& v) noexcept {
    for (std::size_t op = 0; op < 4u; ++op) {
        auto& dst = v.operators[op];
        const auto& src = aced.ops[op];
        dst.fixedFreqMode = src.fixedMode & 0x01u;
        dst.fixedRange    = src.fixedRange & 0x07u;
        dst.fine          = src.fine & 0x0Fu;
        dst.waveform      = src.waveform & 0x07u;
        dst.egShift       = src.egShift & 0x03u;
    }
    v.reverbRate = aced.reverbRate & 0x07u;
    v.fcPitch    = std::min<uint8_t>(aced.fcPitch, 99u);
    v.fcAmp      = std::min<uint8_t>(aced.fcAmp, 99u);
}

void VF50SysExHandler::applyStagedAced2ToVoice(const StagedAced2& aced2, vf50::generated::FactoryVoice& v) noexcept {
    v.atPitch     = std::min<uint8_t>(aced2.atPitch, 99u);
    v.atAmp       = std::min<uint8_t>(aced2.atAmp, 99u);
    v.atPitchBias = std::clamp<int8_t>(aced2.atPitchBias, -50, 50);
    v.atEgBias    = std::min<uint8_t>(aced2.atEgBias, 99u);
    for (std::size_t op = 0; op < 4u; ++op) {
        v.operators[op].fixedRangeLow = aced2.fixedRangeLow[op] & 0x01u;
        // LS SIGN bit mapping (um_page_148): b3=OP1, b2=OP2, b1=OP3, b0=OP4
        const uint8_t signBit = (aced2.lsSignMask >> (3u - op)) & 0x01u;
        const int mag = std::abs(static_cast<int>(v.operators[op].levelScaling));
        v.operators[op].levelScaling = static_cast<int8_t>((signBit != 0u && mag > 0) ? -mag : mag);
    }
}

void VF50SysExHandler::applyStagedPced2ToPerf(const StagedPced2& pced2, vf50::generated::FactoryPerformance& pf) noexcept {
    for (std::size_t i = 0; i < 8u; ++i) {
        pf.instruments[i].reserveNotes = std::min<uint8_t>(pced2.reserveNotes[i], 17u);
        pf.instruments[i].effectEnable = pced2.effectEnable[i] & 0x01u;
    }
    pf.effect = pced2.effect;
}

SysExProcessResult VF50SysExHandler::processSysEx(const uint8_t* data,
                                                 std::size_t length,
                                                 vf50::state::VF50MemoryManager& memory,
                                                 vf50::dsp::VF50SynthEngine& synth,
                                                 SysExReplyBuffer* optionalReplyOut) noexcept {
    SysExProcessResult res{};
    if (data == nullptr || length < 5u) {
        res.kind = SysExEventKind::InvalidOrIgnored;
        return res;
    }

    // Strip optional F0/F7 framing or require valid Manufacturer ID 0x43
    std::size_t start = 0u;
    std::size_t end   = length;
    if (data[0] == kSysExStart) {
        start = 1u;
    }
    if (end > start && data[end - 1u] == kSysExEnd) {
        --end;
    }
    const std::size_t innerLen = end - start;
    if (innerLen < 3u) {
        res.kind = SysExEventKind::InvalidOrIgnored;
        return res;
    }

    const uint8_t* body = data + start;
    if (body[0] != kSysExManufacturerId || !matchesDeviceChannel(body[1])) {
        res.kind = SysExEventKind::InvalidOrIgnored;
        return res;
    }

    const int midiChannel = static_cast<int>(body[1] & 0x0Fu);
    const int formatType  = static_cast<int>((body[1] >> 4u) & 0x07u);
    (void)midiChannel;

    switch (formatType) {
        case 0: // 0n = Bulk Dump
            return handleBulkDump(body, innerLen, memory, synth);
        case 1: // 1n = Parameter Change / Block Header
            return handleParameterChange(body, innerLen, memory, synth);
        case 2: // 2n = Dump Request
            return handleDumpRequest(body, innerLen, memory, synth, optionalReplyOut);
        default:
            res.kind = SysExEventKind::InvalidOrIgnored;
            return res;
    }
}

SysExProcessResult VF50SysExHandler::processSysExStream(const uint8_t* streamData,
                                                       std::size_t streamLength,
                                                       vf50::state::VF50MemoryManager& memory,
                                                       vf50::dsp::VF50SynthEngine& synth,
                                                       SysExReplyBuffer* optionalReplyOut) noexcept {
    SysExProcessResult aggregate{};
    if (streamData == nullptr || streamLength == 0u) return aggregate;

    // Zero-allocation pre-scan to detect concatenated multi-voice streams & VMEM block headers
    std::size_t singleVoiceCount   = 0u;
    std::size_t vmemBlockHdrCount  = 0u;
    {
        std::size_t scanPos = 0u;
        while (scanPos < streamLength) {
            while (scanPos < streamLength && streamData[scanPos] != kSysExStart) {
                ++scanPos;
            }
            if (scanPos >= streamLength) break;
            std::size_t scanEnd = scanPos + 1u;
            while (scanEnd < streamLength && streamData[scanEnd] != kSysExEnd) {
                ++scanEnd;
            }
            const std::size_t pktEnd = (scanEnd < streamLength) ? scanEnd : streamLength;
            const std::size_t innerLen = (pktEnd > scanPos + 1u) ? (pktEnd - (scanPos + 1u)) : 0u;
            const uint8_t* b = streamData + scanPos + 1u;

            if (innerLen >= 5u && b[0] == kSysExManufacturerId) {
                const uint8_t subType = (b[1] >> 4u) & 0x07u;
                if (subType == 0u) {
                    const uint8_t fmt = b[2] & 0x7Fu;
                    if (fmt == 0x03u && innerLen >= 98u) {
                        ++singleVoiceCount;
                    } else if (fmt == 0x24u && innerLen >= 100u && innerLen <= 140u) {
                        ++singleVoiceCount;
                    } else if (fmt == 0x7Fu && innerLen >= 10u && b[3] == 0x1Cu && b[6] == 0x05u && b[7] == 0x30u) {
                        ++singleVoiceCount;
                    }
                } else if (subType == 1u && innerLen == 5u) {
                    const uint8_t gh = b[2] & 0x7Fu;
                    if ((gh == 0x44u || gh == 0x24u) && b[3] == 0x07u) {
                        ++vmemBlockHdrCount;
                    }
                }
            }
            scanPos = (scanEnd < streamLength) ? (scanEnd + 1u) : streamLength;
        }
    }

    hasPendingAced_             = false;
    hasPendingAced2_            = false;
    hasPendingAced3_            = false;
    hasPendingPced2_            = false;
    stagedRefaceVoice_.clear();
    pendingVmemBlock_           = -1;
    autoVmemBlockCursor_        = 0;
    useAutoVmemCursor_          = (vmemBlockHdrCount == 0u);
    stagedSingleVoiceBankIndex_ = (singleVoiceCount > 1u) ? 0 : -1;

    std::size_t pos = 0u;
    while (pos < streamLength) {
        while (pos < streamLength && streamData[pos] != kSysExStart) {
            ++pos;
        }
        if (pos >= streamLength) break;
        std::size_t endPos = pos + 1u;
        while (endPos < streamLength && streamData[endPos] != kSysExEnd) {
            ++endPos;
        }
        const std::size_t msgLen = (endPos < streamLength) ? ((endPos - pos) + 1u) : (streamLength - pos);

        const SysExProcessResult one = processSysEx(
            streamData + pos, msgLen, memory, synth, optionalReplyOut);

        if (one.accepted) {
            aggregate.accepted = true;
            if (one.kind != SysExEventKind::ParameterChange || aggregate.kind == SysExEventKind::None) {
                aggregate.kind = one.kind;
            }
            aggregate.modifiedSingleEdit  |= one.modifiedSingleEdit;
            aggregate.modifiedUserVoices  |= one.modifiedUserVoices;
            aggregate.modifiedPerfEdit    |= one.modifiedPerfEdit;
            aggregate.modifiedUserPerfs   |= one.modifiedUserPerfs;
            aggregate.modifiedMicrotuning |= one.modifiedMicrotuning;
            aggregate.modifiedRhythm      |= one.modifiedRhythm;
            aggregate.generatedReply      |= one.generatedReply;
            aggregate.voicesLoadedCount   += one.voicesLoadedCount;
        }
        if (!one.checksumValid) {
            aggregate.checksumValid = false;
        }
        pos = (endPos < streamLength) ? (endPos + 1u) : streamLength;
    }

    if (aggregate.modifiedUserVoices) {
        memory.setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
        memory.selectSingleVoice(vf50::state::MemoryBank::UserRam, 0u);
        memory.applyActiveSelectionToSynthEngine(synth, true);
    }

    stagedSingleVoiceBankIndex_ = -1;
    useAutoVmemCursor_          = false;
    autoVmemBlockCursor_        = 0;
    return aggregate;
}

// =============================================================================
// Bulk Dump Decoder (0n)
// =============================================================================

SysExProcessResult VF50SysExHandler::handleBulkDump(const uint8_t* body,
                                                   std::size_t innerLen,
                                                   vf50::state::VF50MemoryManager& memory,
                                                   vf50::dsp::VF50SynthEngine& synth) noexcept {
    // body[0]=0x43, body[1]=0n, body[2]=fmt, body[3]=msb, body[4]=lsb, body[5..5+N-1]=payload, body[5+N]=checksum
    SysExProcessResult res{};
    if (innerLen < 6u) {
        res.kind = SysExEventKind::InvalidOrIgnored;
        return res;
    }

    const uint8_t fmt = body[2] & 0x7Fu;
    if (fmt == 0x7Fu && innerLen >= 8u && body[3] == 0x1Cu) {
        return handleRefaceSysExPacket(body, innerLen, memory, synth);
    }

    const std::size_t declaredLen = (static_cast<std::size_t>(body[3] & 0x7Fu) << 7u)
                                  | static_cast<std::size_t>(body[4] & 0x7Fu);
    const std::size_t availAfterHdr = innerLen - 5u;
    std::size_t payloadLen = declaredLen;

    if (availAfterHdr < declaredLen) {
        if (fmt == 0x03u && availAfterHdr >= 93u) {
            payloadLen = (availAfterHdr >= 94u) ? (availAfterHdr - 1u) : 93u;
        } else if (fmt == 0x04u && availAfterHdr >= 4096u) {
            payloadLen = 4096u;
        } else if (fmt == 0x24u && availAfterHdr >= 95u) {
            payloadLen = (availAfterHdr >= 96u) ? (availAfterHdr - 1u) : 95u;
        } else {
            res.kind = SysExEventKind::InvalidOrIgnored;
            return res;
        }
    } else if (fmt == 0x24u && availAfterHdr >= 9500u && declaredLen < 9500u) {
        // Handle >14-bit wrapped native 100-voice payload lengths if encountered
        payloadLen = availAfterHdr - 1u;
    }

    const uint8_t* payload = body + 5u;
    if (availAfterHdr >= payloadLen + 1u) {
        const uint8_t rxSum   = payload[payloadLen] & 0x7Fu;
        const uint8_t calcSum = computeSysExChecksum(payload, payloadLen);
        if (rxSum != calcSum) {
            std::cerr << "VF50SysEx: SysEx checksum mismatch (rx="
                      << static_cast<int>(rxSum) << ", calc=" << static_cast<int>(calcSum)
                      << ") - proceeding with parameter loading.\n";
            res.checksumValid = false;
        }
    } else {
        std::cerr << "VF50SysEx: SysEx packet missing trailing checksum byte - proceeding with parameter loading.\n";
        res.checksumValid = false;
    }

    // -------------------------------------------------------------------------
    // Format 0x03: VCED (93..128-byte 1-Voice Dump, VF50 / DX11 / TX81Z / DX21)
    // -------------------------------------------------------------------------
    if (fmt == 0x03u && payloadLen >= 93u) {
        vf50::generated::FactoryVoice v = vf50::state::VF50MemoryManager::makeInitVoice();
        if (payloadLen == 128u) {
            vf50::state::StoredVoiceSlot tempSlot{};
            unpackVoiceFromVmem128(payload, tempSlot);
            v = tempSlot.voice;
        } else {
            for (std::size_t slot = 0; slot < 4u; ++slot) {
                const std::size_t logOp = kLogicalOpForSysExSlot[slot];
                const uint8_t* opB = &payload[slot * 13u];
                auto& o = v.operators[logOp];

                o.ar           = opB[0] & 0x1Fu;
                o.d1r          = opB[1] & 0x1Fu;
                o.d2r          = opB[2] & 0x1Fu;
                o.rr           = std::max<uint8_t>(1u, static_cast<uint8_t>(opB[3] & 0x0Fu));
                o.d1l          = opB[4] & 0x0Fu;
                o.levelScaling = static_cast<int8_t>(std::min<int>(opB[5] & 0x7Fu, 99));
                o.rateScaling  = opB[6] & 0x03u;
                o.egBiasSens   = opB[7] & 0x07u;
                o.ampModEnable = opB[8] & 0x01u;
                o.keyVelSens   = decodeVcedKvs(opB[9]);
                o.outputLevel  = std::min<uint8_t>(opB[10] & 0x7Fu, 99u);
                o.coarse       = opB[11] & 0x3Fu;
                o.detune       = static_cast<int8_t>(std::clamp<int>(static_cast<int>(opB[12] & 0x07u) - 3, -3, 3));
            }

            v.algorithm      = payload[52] & 0x07u;
            v.feedback       = payload[53] & 0x07u;
            v.lfoSpeed       = std::min<uint8_t>(payload[54] & 0x7Fu, 99u);
            v.lfoDelay       = std::min<uint8_t>(payload[55] & 0x7Fu, 99u);
            v.pmd            = std::min<uint8_t>(payload[56] & 0x7Fu, 99u);
            v.amd            = std::min<uint8_t>(payload[57] & 0x7Fu, 99u);
            v.lfoSync        = payload[58] & 0x01u;
            v.lfoWave        = payload[59] & 0x03u;
            v.pms            = payload[60] & 0x07u;
            v.ams            = payload[61] & 0x03u;
            v.transpose      = static_cast<int8_t>(std::clamp<int>(static_cast<int>(payload[62] & 0x3Fu) - 24, -24, 24));
            v.monoMode       = payload[63] & 0x01u;
            v.pitchBendRange = std::min<uint8_t>(payload[64] & 0x0Fu, 12u);
            v.portaMode      = payload[65] & 0x01u;
            v.portaTime      = std::min<uint8_t>(payload[66] & 0x7Fu, 99u);
            v.fcVolume       = std::min<uint8_t>(payload[67] & 0x7Fu, 99u);
            v.sustainSwitch  = payload[68] & 0x01u;
            v.portaSwitch    = payload[69] & 0x01u;
            v.chorusSwitch   = payload[70] & 0x01u;
            v.mwPitch        = std::min<uint8_t>(payload[71] & 0x7Fu, 99u);
            v.mwAmp          = std::min<uint8_t>(payload[72] & 0x7Fu, 99u);
            v.bcPitch        = std::min<uint8_t>(payload[73] & 0x7Fu, 99u);
            v.bcAmp          = std::min<uint8_t>(payload[74] & 0x7Fu, 99u);
            v.bcPitchBias    = static_cast<int8_t>(std::clamp<int>(static_cast<int>(payload[75] & 0x7Fu) - 50, -50, 50));
            v.bcEgBias       = std::min<uint8_t>(payload[76] & 0x7Fu, 99u);

            std::array<char, 10> nameBuf{};
            for (std::size_t i = 0; i < 10u; ++i) {
                const uint8_t ch = payload[77u + i] & 0x7Fu;
                nameBuf[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
            }
            v.name = std::string_view(nameBuf.data(), nameBuf.size());

            const bool allZeroPeg = (payload[87] == 0u && payload[88] == 0u && payload[89] == 0u &&
                                     payload[90] == 0u && payload[91] == 0u && payload[92] == 0u);
            if (allZeroPeg) {
                v.peg = { 99u, 99u, 99u, 50u, 50u, 50u };
            } else {
                v.peg.pr1 = std::min<uint8_t>(payload[87] & 0x7Fu, 99u);
                v.peg.pr2 = std::min<uint8_t>(payload[88] & 0x7Fu, 99u);
                v.peg.pr3 = std::min<uint8_t>(payload[89] & 0x7Fu, 99u);
                v.peg.pl1 = std::min<uint8_t>(payload[90] & 0x7Fu, 99u);
                v.peg.pl2 = std::min<uint8_t>(payload[91] & 0x7Fu, 99u);
                v.peg.pl3 = std::min<uint8_t>(payload[92] & 0x7Fu, 99u);
            }

            if (hasPendingAced_) {
                applyStagedAcedToVoice(pendingAced_, v);
            }
            if (hasPendingAced2_) {
                applyStagedAced2ToVoice(pendingAced2_, v);
            }
            if (hasPendingAced3_) {
                v.effect = pendingAced3_;
            }
        }
        hasPendingAced_  = false;
        hasPendingAced2_ = false;
        hasPendingAced3_ = false;

        commitDecodedSingleVoice(v, stagedSingleVoiceBankIndex_, memory, synth, res);
        return res;
    }

    // -------------------------------------------------------------------------
    // Format 0x04: VMEM (4096-byte 32-Voice Bulk Dump, VF50 4-block or TX81Z/DX11)
    // -------------------------------------------------------------------------
    if (fmt == 0x04u && payloadLen >= 4096u) {
        std::size_t baseSlot = 0u;
        if (pendingVmemBlock_ >= 1 && pendingVmemBlock_ <= 4) {
            baseSlot = static_cast<std::size_t>(pendingVmemBlock_ - 1) * 32u;
        } else if (useAutoVmemCursor_) {
            baseSlot = static_cast<std::size_t>(autoVmemBlockCursor_) * 32u;
            if (autoVmemBlockCursor_ < 3) {
                ++autoVmemBlockCursor_;
            }
        }
        pendingVmemBlock_ = -1;

        vf50::state::StoredVoiceSlot tempSlot{};
        std::size_t loaded = 0u;
        for (std::size_t i = 0; i < 32u; ++i) {
            const std::size_t dstIdx = baseSlot + i;
            if (dstIdx >= vf50::state::VF50MemoryManager::kVoiceBankSize) break;
            unpackVoiceFromVmem128(payload + i * 128u, tempSlot);
            memory.storeUserVoice(dstIdx, tempSlot.voice);
            synth.fm().storeInternalVoice(dstIdx, memory.getUserVoice(dstIdx));
            ++loaded;
        }

        if (memory.selectedVoiceBank() == vf50::state::MemoryBank::UserRam) {
            memory.selectSingleVoice(vf50::state::MemoryBank::UserRam, memory.selectedVoiceIndex());
        }
        memory.applyActiveSelectionToSynthEngine(synth, true);

        res.kind               = SysExEventKind::SingleVoiceBulkDump;
        res.accepted           = true;
        res.modifiedUserVoices = true;
        res.voicesLoadedCount  = loaded;
        return res;
    }

    // -------------------------------------------------------------------------
    // Format 0x24: Native VF50 Model ID 0x24 Bulk Dump (100-Voice / 32-Voice / 1-Voice)
    // -------------------------------------------------------------------------
    if (fmt == 0x24u && payloadLen >= 95u) {
        vf50::state::StoredVoiceSlot tempSlot{};
        // 100-voice VMEM128 (12800B / 16384B) or 32-voice VMEM128 (4096B)
        if (payloadLen == 12800u || payloadLen == 16384u || payloadLen == 4096u) {
            std::size_t baseSlot = 0u;
            if (payloadLen == 4096u) {
                if (pendingVmemBlock_ >= 1 && pendingVmemBlock_ <= 4) {
                    baseSlot = static_cast<std::size_t>(pendingVmemBlock_ - 1) * 32u;
                } else if (useAutoVmemCursor_) {
                    baseSlot = static_cast<std::size_t>(autoVmemBlockCursor_) * 32u;
                    if (autoVmemBlockCursor_ < 3) ++autoVmemBlockCursor_;
                }
            }
            pendingVmemBlock_ = -1;

            const std::size_t recCount = payloadLen / 128u;
            std::size_t loaded = 0u;
            for (std::size_t i = 0; i < recCount; ++i) {
                const std::size_t dstIdx = baseSlot + i;
                if (dstIdx >= vf50::state::VF50MemoryManager::kVoiceBankSize) break;
                unpackVoiceFromVmem128(payload + i * 128u, tempSlot);
                memory.storeUserVoice(dstIdx, tempSlot.voice);
                synth.fm().storeInternalVoice(dstIdx, memory.getUserVoice(dstIdx));
                ++loaded;
            }
            memory.setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
            memory.selectSingleVoice(vf50::state::MemoryBank::UserRam, 0u);
            memory.applyActiveSelectionToSynthEngine(synth, true);

            res.kind               = SysExEventKind::SingleVoiceBulkDump;
            res.accepted           = true;
            res.modifiedUserVoices = true;
            res.voicesLoadedCount  = loaded;
            return res;
        }

        // 100-voice or N-voice packed 95-byte native VF50 ROM/RAM voice records (9500B = 100 voices)
        if ((payloadLen % 95u) == 0u && (payloadLen / 95u) >= 2u && (payloadLen / 95u) <= 100u) {
            const std::size_t recCount = payloadLen / 95u;
            for (std::size_t i = 0; i < recCount; ++i) {
                std::array<uint8_t, 95> raw{};
                std::memcpy(raw.data(), payload + i * 95u, 95u);
                tempSlot.unpackFromRom95(raw);
                memory.storeUserVoice(i, tempSlot.voice);
                synth.fm().storeInternalVoice(i, memory.getUserVoice(i));
            }
            memory.setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
            memory.selectSingleVoice(vf50::state::MemoryBank::UserRam, 0u);
            memory.applyActiveSelectionToSynthEngine(synth, true);

            res.kind               = SysExEventKind::SingleVoiceBulkDump;
            res.accepted           = true;
            res.modifiedUserVoices = true;
            res.voicesLoadedCount  = recCount;
            return res;
        }

        // Single 128-byte VMEM or 95-byte ROM voice in Model 0x24 wrapper
        if (payloadLen == 128u || payloadLen == 95u) {
            if (payloadLen == 128u) {
                unpackVoiceFromVmem128(payload, tempSlot);
            } else {
                std::array<uint8_t, 95> raw{};
                std::memcpy(raw.data(), payload, 95u);
                tempSlot.unpackFromRom95(raw);
            }
            commitDecodedSingleVoice(tempSlot.voice, stagedSingleVoiceBankIndex_, memory, synth, res);
            return res;
        }
    }

    // -------------------------------------------------------------------------
    // Format 0x7E: Universal Hardware "LM  ..." Classified Bulk Dumps
    // -------------------------------------------------------------------------
    if (fmt == 0x7Eu && payloadLen >= 10u) {
        const uint8_t* lmData = payload + 10u;
        const std::size_t lmDataLen = payloadLen - 10u;

        // ACED ("LM  8976AE", 23 bytes, TX81Z / DX11 / VF50)
        if (matchAsciiTag10(payload, "LM  8976AE") && lmDataLen == 23u) {
            for (std::size_t slot = 0; slot < 4u; ++slot) {
                const std::size_t logOp = kLogicalOpForSysExSlot[slot];
                const uint8_t* ob = &lmData[slot * 5u];
                pendingAced_.ops[logOp].fixedMode  = ob[0] & 0x01u;
                pendingAced_.ops[logOp].fixedRange = ob[1] & 0x07u;
                pendingAced_.ops[logOp].fine       = ob[2] & 0x0Fu;
                pendingAced_.ops[logOp].waveform   = ob[3] & 0x07u;
                pendingAced_.ops[logOp].egShift    = ob[4] & 0x03u;
            }
            pendingAced_.reverbRate = lmData[20] & 0x07u;
            pendingAced_.fcPitch    = std::min<uint8_t>(lmData[21] & 0x7Fu, 99u);
            pendingAced_.fcAmp      = std::min<uint8_t>(lmData[22] & 0x7Fu, 99u);
            hasPendingAced_         = true;

            if (stagedSingleVoiceBankIndex_ < 0) {
                auto& slot = memory.singleEditSlot();
                applyStagedAcedToVoice(pendingAced_, slot.voice);
                slot.repackRomBytes();
                if (memory.workstationMode() == vf50::state::WorkstationMode::SingleEdit) {
                    memory.applyActiveSelectionToSynthEngine(synth, false);
                }
                res.kind               = SysExEventKind::SingleVoiceEditDump;
                res.accepted           = true;
                res.modifiedSingleEdit = true;
            } else {
                res.kind     = SysExEventKind::ParameterChange;
                res.accepted = true;
            }
            return res;
        }

        // ACED2 ("LM  8023AE", 10 bytes, DX11 / VF50)
        if (matchAsciiTag10(payload, "LM  8023AE") && lmDataLen == 10u) {
            pendingAced2_.atPitch     = std::min<uint8_t>(lmData[0] & 0x7Fu, 99u);
            pendingAced2_.atAmp       = std::min<uint8_t>(lmData[1] & 0x7Fu, 99u);
            pendingAced2_.atPitchBias = decodeMidiAtPitchBias(lmData[2]);
            pendingAced2_.atEgBias    = std::min<uint8_t>(lmData[3] & 0x7Fu, 99u);
            for (std::size_t slot = 0; slot < 4u; ++slot) {
                const std::size_t logOp = kLogicalOpForSysExSlot[slot];
                pendingAced2_.fixedRangeLow[logOp] = lmData[4u + slot] & 0x01u;
            }
            pendingAced2_.lsSignMask = lmData[8] & 0x0Fu;
            hasPendingAced2_         = true;

            if (stagedSingleVoiceBankIndex_ < 0) {
                auto& slot = memory.singleEditSlot();
                applyStagedAced2ToVoice(pendingAced2_, slot.voice);
                slot.repackRomBytes();
                if (memory.workstationMode() == vf50::state::WorkstationMode::SingleEdit) {
                    memory.applyActiveSelectionToSynthEngine(synth, false);
                }
                res.kind               = SysExEventKind::SingleVoiceEditDump;
                res.accepted           = true;
                res.modifiedSingleEdit = true;
            } else {
                res.kind     = SysExEventKind::ParameterChange;
                res.accepted = true;
            }
            return res;
        }

        // ACED3 ("LM  8073AE", 20 bytes, VF50 DSP Effect)
        if (matchAsciiTag10(payload, "LM  8073AE") && lmDataLen == 20u) {
            pendingAced3_.effectType = std::min<uint8_t>(lmData[0] & 0x3Fu, 32u);
            pendingAced3_.balance    = std::min<uint8_t>(lmData[1] & 0x7Fu, 100u);
            pendingAced3_.outLevel   = std::min<uint8_t>(lmData[2] & 0x7Fu, 100u);
            pendingAced3_.stereoMix  = lmData[3] & 0x01u;
            pendingAced3_.param1     = std::min<uint8_t>(lmData[4] & 0x7Fu, 75u);
            pendingAced3_.param2     = std::min<uint8_t>(lmData[5] & 0x7Fu, 99u);
            pendingAced3_.param3     = std::min<uint8_t>(lmData[6] & 0x7Fu, 99u);
            hasPendingAced3_         = true;

            if (stagedSingleVoiceBankIndex_ < 0) {
                auto& slot = memory.singleEditSlot();
                slot.voice.effect = pendingAced3_;
                slot.repackRomBytes();
                if (memory.workstationMode() == vf50::state::WorkstationMode::SingleEdit) {
                    memory.applyActiveSelectionToSynthEngine(synth, true);
                }
                res.kind               = SysExEventKind::SingleVoiceEditDump;
                res.accepted           = true;
                res.modifiedSingleEdit = true;
            } else {
                res.kind     = SysExEventKind::ParameterChange;
                res.accepted = true;
            }
            return res;
        }

        // PCED2 ("LM  8073PE", 33 bytes, VF50 Performance Extension)
        if (matchAsciiTag10(payload, "LM  8073PE") && lmDataLen == 33u) {
            for (std::size_t i = 0; i < 8u; ++i) {
                pendingPced2_.reserveNotes[i] = std::min<uint8_t>(lmData[i * 2u] & 0x1Fu, 17u);
                pendingPced2_.effectEnable[i] = lmData[i * 2u + 1u] & 0x01u;
            }
            pendingPced2_.effect.effectType = std::min<uint8_t>(lmData[16] & 0x3Fu, 32u);
            pendingPced2_.effect.balance    = std::min<uint8_t>(lmData[17] & 0x7Fu, 100u);
            pendingPced2_.effect.outLevel   = std::min<uint8_t>(lmData[18] & 0x7Fu, 100u);
            pendingPced2_.effect.stereoMix  = lmData[19] & 0x01u;
            pendingPced2_.effect.param1     = std::min<uint8_t>(lmData[20] & 0x7Fu, 75u);
            pendingPced2_.effect.param2     = std::min<uint8_t>(lmData[21] & 0x7Fu, 99u);
            pendingPced2_.effect.param3     = std::min<uint8_t>(lmData[22] & 0x7Fu, 99u);
            hasPendingPced2_                = true;

            auto& pfSlot = memory.perfEditSlot();
            applyStagedPced2ToPerf(pendingPced2_, pfSlot.perf);
            pfSlot.repackRomBytes();
            if (memory.workstationMode() == vf50::state::WorkstationMode::PerformanceEdit) {
                memory.applyActiveSelectionToSynthEngine(synth, true);
            }

            res.kind             = SysExEventKind::PerformanceEditDump;
            res.accepted         = true;
            res.modifiedPerfEdit = true;
            return res;
        }

        // PCED ("LM  8976PE", 110 bytes, TX81Z / DX11 / VF50 Performance Edit)
        if (matchAsciiTag10(payload, "LM  8976PE") && lmDataLen == 110u) {
            vf50::generated::FactoryPerformance pf =
                vf50::state::VF50MemoryManager::makeInitPerformance(vf50::state::PerfInitTemplate::All);

            for (std::size_t i = 0; i < 8u; ++i) {
                const uint8_t* ib = &lmData[i * 12u];
                auto& inst = pf.instruments[i];

                inst.maxNotes        = std::min<uint8_t>(ib[0] & 0x1Fu, 16u);
                inst.voiceBank       = ib[1] & 0x01u; // 0=int, 1=preset
                inst.voiceNumber     = std::min<uint8_t>(ib[2] & 0x7Fu, 99u);
                inst.receiveChannel  = std::min<uint8_t>(ib[3] & 0x1Fu, 17u);
                inst.limitLow        = ib[4] & 0x7Fu;
                inst.limitHigh       = ib[5] & 0x7Fu;
                inst.detune          = static_cast<int8_t>(std::clamp<int>(static_cast<int>(ib[6] & 0x0Fu) - 7, -7, 7));
                inst.noteShift       = static_cast<int8_t>(std::clamp<int>(static_cast<int>(ib[7] & 0x3Fu) - 24, -24, 24));
                inst.volume          = std::min<uint8_t>(ib[8] & 0x7Fu, 99u);
                inst.outAssign       = ib[9] & 0x03u;
                inst.lfoSelect       = ib[10] & 0x03u;
                inst.microTuneEnable = ib[11] & 0x01u;
                inst.reserveNotes    = 0u;
                inst.effectEnable    = 1u;
            }

            pf.microTuneTable   = std::min<uint8_t>(lmData[96] & 0x0Fu, 12u);
            pf.assignMode       = std::min<uint8_t>(lmData[97] & 0x03u, 2u);
            pf.perfEffectSelect = std::min<uint8_t>(lmData[98] & 0x0Fu, 12u);
            pf.microTuneRootKey = std::min<uint8_t>(lmData[99] & 0x0Fu, 11u);

            std::array<char, 10> nameBuf{};
            for (std::size_t i = 0; i < 10u; ++i) {
                const uint8_t ch = lmData[100u + i] & 0x7Fu;
                nameBuf[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
            }
            pf.name = std::string_view(nameBuf.data(), nameBuf.size());

            if (hasPendingPced2_) {
                applyStagedPced2ToPerf(pendingPced2_, pf);
                hasPendingPced2_ = false;
            }

            memory.setWorkstationMode(vf50::state::WorkstationMode::PerformanceEdit);
            memory.perfEditSlot().setFromPerformance(pf);
            memory.applyActiveSelectionToSynthEngine(synth, true);

            res.kind             = SysExEventKind::PerformanceEditDump;
            res.accepted         = true;
            res.modifiedPerfEdit = true;
            return res;
        }

        // PMEM ("LM  8976PM", 32*76=2432 bytes on DX11/VF50, or 24*76=1824 bytes on TX81Z)
        if (matchAsciiTag10(payload, "LM  8976PM") &&
            (lmDataLen == 32u * 76u || lmDataLen == 24u * 76u)) {
            const std::size_t numRecords = lmDataLen / 76u;
            const std::size_t baseSlot = (pendingPmemBlock_ >= 0 && pendingPmemBlock_ <= 3)
                                             ? (static_cast<std::size_t>(pendingPmemBlock_) * 32u)
                                             : 0u;
            pendingPmemBlock_  = -1;
            lastPmemBaseSlot_  = baseSlot;
            lastPmemSlotCount_ = 0u;

            for (std::size_t i = 0; i < numRecords; ++i) {
                const std::size_t dstIdx = baseSlot + i;
                if (dstIdx >= vf50::state::VF50MemoryManager::kPerfBankSize) break;

                vf50::generated::FactoryPerformance pf{};
                unpackPerfFromPmem76(lmData + i * 76u, pf, lastPmemMax1_[i], lastPmemAsmod1_[i]);

                std::array<char, 10> nameBuf{};
                for (std::size_t c = 0; c < 10u; ++c) {
                    const uint8_t ch = lmData[i * 76u + 66u + c] & 0x7Fu;
                    nameBuf[c] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
                }
                pf.name = std::string_view(nameBuf.data(), nameBuf.size());

                memory.storeUserPerformance(dstIdx, pf);
                synth.fm().storeInternalPerformance(dstIdx, memory.getUserPerformance(dstIdx));
                ++lastPmemSlotCount_;
            }

            if (memory.selectedPerfBank() == vf50::state::MemoryBank::UserRam) {
                memory.selectPerformance(vf50::state::MemoryBank::UserRam, memory.selectedPerfIndex());
            }
            memory.applyActiveSelectionToSynthEngine(synth, true);

            res.kind              = SysExEventKind::PerformanceBulkDump;
            res.accepted          = true;
            res.modifiedUserPerfs = true;
            return res;
        }

        // PMEM2 ("LM  8073PM", 32*25=800 bytes, VF50 Performance Bulk Extension)
        if (matchAsciiTag10(payload, "LM  8073PM") && lmDataLen == 32u * 25u) {
            const std::size_t count = (lastPmemSlotCount_ > 0u) ? lastPmemSlotCount_ : 32u;
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t dstIdx = lastPmemBaseSlot_ + i;
                if (dstIdx >= vf50::state::VF50MemoryManager::kPerfBankSize) break;

                vf50::state::StoredPerformanceSlot slot(memory.getUserPerformance(dstIdx));
                overlayPmem2_25(lmData + i * 25u, lastPmemMax1_[i], lastPmemAsmod1_[i], slot.perf);
                slot.repackRomBytes();
                memory.storeUserPerformance(dstIdx, slot.perf, memory.getUserPerformancePartPans(dstIdx));
                synth.fm().storeInternalPerformance(dstIdx, memory.getUserPerformance(dstIdx));
            }

            if (memory.selectedPerfBank() == vf50::state::MemoryBank::UserRam) {
                memory.selectPerformance(vf50::state::MemoryBank::UserRam, memory.selectedPerfIndex());
            }
            memory.applyActiveSelectionToSynthEngine(synth, true);

            res.kind              = SysExEventKind::PerformanceBulkDump;
            res.accepted          = true;
            res.modifiedUserPerfs = true;
            return res;
        }

        // Microtuning Octave ("LM  MCRTE0", 24 bytes = 12 * 2)
        if (matchAsciiTag10(payload, "LM  MCRTE0") && lmDataLen == 24u) {
            std::array<vf50::generated::MicrotuningEntry, 12> tbl{};
            for (std::size_t i = 0; i < 12u; ++i) {
                tbl[i].coarseNote = lmData[i * 2u] & 0x7Fu;
                tbl[i].fineStep64 = lmData[i * 2u + 1u] & 0x3Fu;
            }
            memory.setUserOctaveMicrotuning(tbl);
            synth.fm().setUserOctaveMicrotuning(tbl);

            res.kind                = SysExEventKind::MicrotuningOctDump;
            res.accepted            = true;
            res.modifiedMicrotuning = true;
            return res;
        }

        // Microtuning Full Keyboard ("LM  MCRTE1", 256 bytes = 128 * 2)
        if (matchAsciiTag10(payload, "LM  MCRTE1") && lmDataLen == 256u) {
            std::array<vf50::generated::MicrotuningEntry, 128> tbl{};
            for (std::size_t i = 0; i < 128u; ++i) {
                tbl[i].coarseNote = lmData[i * 2u] & 0x7Fu;
                tbl[i].fineStep64 = lmData[i * 2u + 1u] & 0x3Fu;
            }
            memory.setUserFullMicrotuning(tbl);
            synth.fm().setUserFullMicrotuning(tbl);

            res.kind                = SysExEventKind::MicrotuningFullDump;
            res.accepted            = true;
            res.modifiedMicrotuning = true;
            return res;
        }

        // Rhythm System Setup ("LM  8073RS", 16 bytes)
        if (matchAsciiTag10(payload, "LM  8073RS") && lmDataLen == 16u) {
            auto& kit = memory.mutableActiveRhythmKit();
            kit.rhythmVolume = std::min<uint8_t>(lmData[0] & 0x7Fu, 99u);
            kit.effectMode = static_cast<vf50::dsp::drums::RhythmEffectMode>(std::min<uint8_t>(lmData[1] & 0x03u, 2u));
            synth.drums().setRhythmReceiveChannel(std::min<uint8_t>(lmData[4] & 0x1Fu, 17u));
            memory.selectRhythmKeyMap(std::min<std::size_t>(lmData[9] & 0x07u, 4u));
            const auto ptnBank = ((lmData[10] & 0x01u) == 0u) ? vf50::state::MemoryBank::UserRam
                                                              : vf50::state::MemoryBank::FactoryRom;
            memory.selectRhythmPattern(ptnBank, std::min<std::size_t>(lmData[11] & 0x7Fu, 99u));
            memory.applyActiveSelectionToSynthEngine(synth, false);

            res.kind           = SysExEventKind::RhythmSetupDump;
            res.accepted       = true;
            res.modifiedRhythm = true;
            return res;
        }

        // Rhythm Instrument Setup ("LM  8073RI", 183 bytes = 61 Vol + 61 Pan + 61 Note)
        if (matchAsciiTag10(payload, "LM  8073RI") && lmDataLen == 183u) {
            auto& kit = memory.mutableActiveRhythmKit();
            for (std::size_t d = 0; d < vf50::generated::kDrumInstrumentCount; ++d) {
                kit.instruments[d].volume   = std::min<uint8_t>(lmData[d] & 0x0Fu, 15u);
                kit.instruments[d].pan      = static_cast<int8_t>(std::clamp<int>(static_cast<int>(lmData[61u + d] & 0x07u) - 3, -3, 3));
                kit.instruments[d].midiNote = lmData[122u + d] & 0x7Fu;
                synth.drums().setInstrumentState(d, kit.instruments[d]);
            }

            res.kind           = SysExEventKind::RhythmSetupDump;
            res.accepted       = true;
            res.modifiedRhythm = true;
            return res;
        }

        // Rhythm User Keyboard Assign Table 1 & 2 ("LM  8073K0" / "LM  8073K1", 61 bytes)
        if ((matchAsciiTag10(payload, "LM  8073K0") || matchAsciiTag10(payload, "LM  8073K1")) && lmDataLen == 61u) {
            const std::size_t userIdx = (payload[9] == static_cast<uint8_t>('1')) ? 1u : 0u;
            std::array<int8_t, 61> km{};
            for (std::size_t k = 0; k < 61u; ++k) {
                const uint8_t v = lmData[k] & 0x7Fu;
                km[k] = (v < 61u) ? static_cast<int8_t>(v) : static_cast<int8_t>(-1);
            }
            memory.setUserKeyMap(userIdx, km);
            if (memory.selectedRhythmKeyMap() == (3u + userIdx)) {
                synth.drums().loadCustomKeyMap(km);
            }

            res.kind           = SysExEventKind::RhythmSetupDump;
            res.accepted       = true;
            res.modifiedRhythm = true;
            return res;
        }
    }

    res.kind = SysExEventKind::InvalidOrIgnored;
    return res;
}

// =============================================================================
// Parameter Change & Block Header Handler (1n)
// =============================================================================

SysExProcessResult VF50SysExHandler::handleParameterChange(const uint8_t* body,
                                                          std::size_t innerLen,
                                                          vf50::state::VF50MemoryManager& memory,
                                                          vf50::dsp::VF50SynthEngine& synth) noexcept {
    SysExProcessResult res{};
    if (innerLen < 5u) {
        res.kind = SysExEventKind::InvalidOrIgnored;
        return res;
    }

    const uint8_t gh = body[2] & 0x7Fu;

    // VMEM Block Header: F0 43 1n 44 07 <block 0..4> F7 (or 24 07 <block 0..4>)
    if ((gh == 0x44u || gh == 0x24u) && body[3] == 0x07u && innerLen == 5u) {
        pendingVmemBlock_ = static_cast<int>(body[4] & 0x07u);
        res.kind          = SysExEventKind::ParameterChange;
        res.accepted      = true;
        return res;
    }

    // PMEM Block Header: F0 43 1n 10 75 01 <block 0..3> F7 (or 6-byte 10 75 <block>)
    if (gh == 0x10u && body[3] == 0x75u) {
        if (innerLen == 6u && body[4] == 0x01u) {
            pendingPmemBlock_ = static_cast<int>(body[5] & 0x03u);
            res.kind          = SysExEventKind::ParameterChange;
            res.accepted      = true;
            return res;
        }
        if (innerLen == 5u) {
            pendingPmemBlock_ = static_cast<int>(body[4] & 0x03u);
            res.kind          = SysExEventKind::ParameterChange;
            res.accepted      = true;
            return res;
        }
    }

    // VCED Parameter Change: g=4, h=2 -> gh = 0x12, param=0..93, val=body[4]
    if (gh == 0x12u && innerLen == 5u) {
        const uint8_t p   = body[3] & 0x7Fu;
        const uint8_t val = body[4] & 0x7Fu;

        if (p == 93u) {
            // Operator Enable bitmask: b3=OP1, b2=OP2, b1=OP3, b0=OP4 -> engine mask bit0=OP1..bit3=OP4
            uint8_t mask = 0u;
            if ((val & 0x08u) != 0u) mask |= 0x01u;
            if ((val & 0x04u) != 0u) mask |= 0x02u;
            if ((val & 0x02u) != 0u) mask |= 0x04u;
            if ((val & 0x01u) != 0u) mask |= 0x08u;
            synth.fm().setOperatorEnableMask(mask);
            res.kind               = SysExEventKind::ParameterChange;
            res.accepted           = true;
            res.modifiedSingleEdit = true;
            return res;
        }

        memory.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
        auto& v = memory.singleEditSlot().voice;

        if (p < 52u) {
            const std::size_t slot  = p / 13u;
            const std::size_t field = p % 13u;
            auto& o = v.operators[kLogicalOpForSysExSlot[slot]];
            switch (field) {
                case 0u:  o.ar = val & 0x1Fu; break;
                case 1u:  o.d1r = val & 0x1Fu; break;
                case 2u:  o.d2r = val & 0x1Fu; break;
                case 3u:  o.rr = std::max<uint8_t>(1u, static_cast<uint8_t>(val & 0x0Fu)); break;
                case 4u:  o.d1l = val & 0x0Fu; break;
                case 5u: {
                    const int sign = (o.levelScaling < 0) ? -1 : 1;
                    o.levelScaling = static_cast<int8_t>(sign * std::min<int>(val, 99));
                    break;
                }
                case 6u:  o.rateScaling = val & 0x03u; break;
                case 7u:  o.egBiasSens = val & 0x07u; break;
                case 8u:  o.ampModEnable = val & 0x01u; break;
                case 9u:  o.keyVelSens = decodeVcedKvs(val); break;
                case 10u: o.outputLevel = std::min<uint8_t>(val, 99u); break;
                case 11u: o.coarse = val & 0x3Fu; break;
                case 12u: o.detune = static_cast<int8_t>(std::clamp<int>(static_cast<int>(val & 0x07u) - 3, -3, 3)); break;
                default: break;
            }
        } else {
            switch (p) {
                case 52u: v.algorithm = val & 0x07u; break;
                case 53u: v.feedback = val & 0x07u; break;
                case 54u: v.lfoSpeed = std::min<uint8_t>(val, 99u); break;
                case 55u: v.lfoDelay = std::min<uint8_t>(val, 99u); break;
                case 56u: v.pmd = std::min<uint8_t>(val, 99u); break;
                case 57u: v.amd = std::min<uint8_t>(val, 99u); break;
                case 58u: v.lfoSync = val & 0x01u; break;
                case 59u: v.lfoWave = val & 0x03u; break;
                case 60u: v.pms = val & 0x07u; break;
                case 61u: v.ams = val & 0x03u; break;
                case 62u: v.transpose = static_cast<int8_t>(std::clamp<int>(static_cast<int>(val) - 24, -24, 24)); break;
                case 63u: v.monoMode = val & 0x01u; break;
                case 64u: v.pitchBendRange = std::min<uint8_t>(val, 12u); break;
                case 65u: v.portaMode = val & 0x01u; break;
                case 66u: v.portaTime = std::min<uint8_t>(val, 99u); break;
                case 67u: v.fcVolume = std::min<uint8_t>(val, 99u); break;
                case 68u: v.sustainSwitch = val & 0x01u; break;
                case 69u: v.portaSwitch = val & 0x01u; break;
                case 70u: v.chorusSwitch = val & 0x01u; break;
                case 71u: v.mwPitch = std::min<uint8_t>(val, 99u); break;
                case 72u: v.mwAmp = std::min<uint8_t>(val, 99u); break;
                case 73u: v.bcPitch = std::min<uint8_t>(val, 99u); break;
                case 74u: v.bcAmp = std::min<uint8_t>(val, 99u); break;
                case 75u: v.bcPitchBias = static_cast<int8_t>(std::clamp<int>(static_cast<int>(val) - 50, -50, 50)); break;
                case 76u: v.bcEgBias = std::min<uint8_t>(val, 99u); break;
                case 87u: v.peg.pr1 = std::min<uint8_t>(val, 99u); break;
                case 88u: v.peg.pr2 = std::min<uint8_t>(val, 99u); break;
                case 89u: v.peg.pr3 = std::min<uint8_t>(val, 99u); break;
                case 90u: v.peg.pl1 = std::min<uint8_t>(val, 99u); break;
                case 91u: v.peg.pl2 = std::min<uint8_t>(val, 99u); break;
                case 92u: v.peg.pl3 = std::min<uint8_t>(val, 99u); break;
                default:
                    if (p >= 77u && p <= 86u) {
                        memory.singleEditSlot().nameChars[p - 77u] =
                            (val >= 0x20u && val <= 0x7Eu) ? static_cast<char>(val) : ' ';
                    }
                    break;
            }
        }
        memory.singleEditSlot().repackRomBytes();
        synth.fm().setActiveSingleVoiceDirect(memory.singleEditSlot().voice);
        res.kind               = SysExEventKind::ParameterChange;
        res.accepted           = true;
        res.modifiedSingleEdit = true;
        return res;
    }

    // ACED / ACED2 / ACED3 Parameter Change: g=4, h=3 -> gh = 0x13, param=0..52
    if (gh == 0x13u && innerLen == 5u) {
        const uint8_t p   = body[3] & 0x7Fu;
        const uint8_t val = body[4] & 0x7Fu;

        memory.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
        auto& v = memory.singleEditSlot().voice;
        bool fxChanged = false;

        if (p < 20u) {
            const std::size_t slot  = p / 5u;
            const std::size_t field = p % 5u;
            auto& o = v.operators[kLogicalOpForSysExSlot[slot]];
            switch (field) {
                case 0u: o.fixedFreqMode = val & 0x01u; break;
                case 1u: o.fixedRange = val & 0x07u; break;
                case 2u: o.fine = val & 0x0Fu; break;
                case 3u: o.waveform = val & 0x07u; break;
                case 4u: o.egShift = val & 0x03u; break;
                default: break;
            }
        } else {
            switch (p) {
                case 20u: v.reverbRate = val & 0x07u; break;
                case 21u: v.fcPitch = std::min<uint8_t>(val, 99u); break;
                case 22u: v.fcAmp = std::min<uint8_t>(val, 99u); break;
                case 23u: v.atPitch = std::min<uint8_t>(val, 99u); break;
                case 24u: v.atAmp = std::min<uint8_t>(val, 99u); break;
                case 25u: v.atPitchBias = decodeMidiAtPitchBias(val); break;
                case 26u: v.atEgBias = std::min<uint8_t>(val, 99u); break;
                case 27u: case 28u: case 29u: case 30u: {
                    const std::size_t slot = static_cast<std::size_t>(p - 27u);
                    v.operators[kLogicalOpForSysExSlot[slot]].fixedRangeLow = val & 0x01u;
                    break;
                }
                case 31u: {
                    for (std::size_t op = 0; op < 4u; ++op) {
                        const uint8_t neg = (val >> (3u - op)) & 0x01u;
                        const int mag = std::abs(static_cast<int>(v.operators[op].levelScaling));
                        v.operators[op].levelScaling = static_cast<int8_t>((neg != 0u && mag > 0) ? -mag : mag);
                    }
                    break;
                }
                case 33u: v.effect.effectType = std::min<uint8_t>(val, 32u); fxChanged = true; break;
                case 34u: v.effect.balance = std::min<uint8_t>(val, 100u); fxChanged = true; break;
                case 35u: v.effect.outLevel = std::min<uint8_t>(val, 100u); fxChanged = true; break;
                case 36u: v.effect.stereoMix = val & 0x01u; fxChanged = true; break;
                case 37u: v.effect.param1 = std::min<uint8_t>(val, 75u); fxChanged = true; break;
                case 38u: v.effect.param2 = std::min<uint8_t>(val, 99u); fxChanged = true; break;
                case 39u: v.effect.param3 = std::min<uint8_t>(val, 99u); fxChanged = true; break;
                default: break;
            }
        }
        memory.singleEditSlot().repackRomBytes();
        synth.fm().setActiveSingleVoiceDirect(memory.singleEditSlot().voice);
        if (fxChanged) {
            synth.effects().setSettings(v.effect);
        }
        res.kind               = SysExEventKind::ParameterChange;
        res.accepted           = true;
        res.modifiedSingleEdit = true;
        return res;
    }

    // System / Quick Edit / Effect Bypass Parameter Change: g=4, h=0 -> gh = 0x10
    if (gh == 0x10u && innerLen == 5u) {
        const uint8_t p   = body[3] & 0x7Fu;
        const uint8_t val = body[4] & 0x7Fu;
        if (p >= 43u && p <= 46u) {
            auto qe = synth.fm().quickEditOffsets();
            const int centered = std::clamp<int>(static_cast<int>(val) - 50, -50, 50);
            if (p == 43u) qe.attackOffset = centered;
            else if (p == 44u) qe.releaseOffset = centered;
            else if (p == 45u) qe.volumeOffset = centered;
            else if (p == 46u) qe.brillianceOffset = centered;
            synth.fm().setQuickEditOffsets(qe);
            res.kind     = SysExEventKind::ParameterChange;
            res.accepted = true;
            return res;
        }
        if (p == 79u) {
            synth.effects().setBypass((val & 0x01u) != 0u);
            res.kind     = SysExEventKind::ParameterChange;
            res.accepted = true;
            return res;
        }
    }

    res.kind = SysExEventKind::InvalidOrIgnored;
    return res;
}

// =============================================================================
// Dump Request Handler (2n) - Zero Heap Allocations
// =============================================================================

SysExProcessResult VF50SysExHandler::handleDumpRequest(const uint8_t* body,
                                                      std::size_t innerLen,
                                                      const vf50::state::VF50MemoryManager& memory,
                                                      const vf50::dsp::VF50SynthEngine& synth,
                                                      SysExReplyBuffer* replyOut) const noexcept {
    SysExProcessResult res{};
    if (replyOut == nullptr || innerLen < 3u) {
        res.kind = SysExEventKind::InvalidOrIgnored;
        return res;
    }

    const uint8_t fmt = body[2] & 0x7Fu;
    bool ok = false;

    if (fmt == 0x03u && innerLen == 3u) {
        ok = emitSingleVoiceEditDump(memory.activeSingleVoice(), *replyOut, false, false, false);
    } else if (fmt == 0x04u && innerLen == 3u) {
        ok = emitSingleVoiceBankDump100(memory, vf50::state::MemoryBank::UserRam, *replyOut);
    } else if (fmt == 0x7Eu && innerLen == 13u) {
        const uint8_t* tag = body + 3u;
        if (matchAsciiTag10(tag, "LM  8976AE")) {
            ok = emitSingleVoiceEditDump(memory.activeSingleVoice(), *replyOut, true, false, false);
        } else if (matchAsciiTag10(tag, "LM  8023AE")) {
            ok = emitSingleVoiceEditDump(memory.activeSingleVoice(), *replyOut, true, true, false);
        } else if (matchAsciiTag10(tag, "LM  8073AE")) {
            ok = emitSingleVoiceEditDump(memory.activeSingleVoice(), *replyOut, true, true, true);
        } else if (matchAsciiTag10(tag, "LM  8976PE")) {
            ok = emitPerformanceEditDump(memory.activePerformance(), *replyOut, false);
        } else if (matchAsciiTag10(tag, "LM  8073PE")) {
            ok = emitPerformanceEditDump(memory.activePerformance(), *replyOut, true);
        } else if (matchAsciiTag10(tag, "LM  8976PM")) {
            ok = emitPerformanceBulkBlock32(memory, vf50::state::MemoryBank::UserRam, 0u, *replyOut, false, false);
        } else if (matchAsciiTag10(tag, "LM  8073PM")) {
            ok = emitPerformanceBankDump100(memory, vf50::state::MemoryBank::UserRam, *replyOut);
        } else if (matchAsciiTag10(tag, "LM  MCRTE0")) {
            ok = emitMicrotuningOctaveDump(memory.userOctaveMicrotuning(), *replyOut);
        } else if (matchAsciiTag10(tag, "LM  MCRTE1")) {
            ok = emitMicrotuningFullDump(memory.userFullMicrotuning(), *replyOut);
        } else if (matchAsciiTag10(tag, "LM  8073RS")) {
            ok = emitRhythmSystemDump(memory, synth, *replyOut);
        } else if (matchAsciiTag10(tag, "LM  8073RI")) {
            ok = emitRhythmInstrumentsDump(memory.activeRhythmKit(), *replyOut);
        } else if (matchAsciiTag10(tag, "LM  8073K0")) {
            ok = emitRhythmKeyMapDump(memory.userKeyMap(0u), 0u, *replyOut);
        } else if (matchAsciiTag10(tag, "LM  8073K1")) {
            ok = emitRhythmKeyMapDump(memory.userKeyMap(1u), 1u, *replyOut);
        }
    }

    if (ok) {
        res.kind           = SysExEventKind::DumpRequestServiced;
        res.accepted       = true;
        res.generatedReply = true;
    } else {
        res.kind = SysExEventKind::InvalidOrIgnored;
    }
    return res;
}

SysExProcessResult VF50SysExHandler::handleRefaceSysExPacket(const uint8_t* body,
                                                           std::size_t innerLen,
                                                           vf50::state::VF50MemoryManager& memory,
                                                           vf50::dsp::VF50SynthEngine& synth) noexcept {
    SysExProcessResult res{};
    if (body == nullptr || innerLen < 8u || body[6] != 0x05u) {
        res.kind = SysExEventKind::InvalidOrIgnored;
        return res;
    }

    // Lenient Reface DX 7-bit checksum verification over [ModelID, AddrHi, AddrMid, AddrLo, Data...]
    const std::size_t refaceBlockLen = (static_cast<std::size_t>(body[4] & 0x7Fu) << 7u)
                                     | static_cast<std::size_t>(body[5] & 0x7Fu);
    if (refaceBlockLen > 0u && innerLen >= (6u + refaceBlockLen + 1u)) {
        const uint8_t rxSum   = body[6u + refaceBlockLen] & 0x7Fu;
        const uint8_t calcSum = computeSysExChecksum(body + 6u, refaceBlockLen);
        if (rxSum != calcSum) {
            std::cerr << "VF50SysEx: Reface SysEx checksum mismatch (rx="
                      << static_cast<int>(rxSum) << ", calc=" << static_cast<int>(calcSum)
                      << ") - proceeding with parameter loading.\n";
            res.checksumValid = false;
        }
    }

    const uint8_t subcmd = body[7] & 0x7Fu;

    auto commitStagedReface = [&]() noexcept {
        vf50::generated::FactoryVoice v = vf50::state::VF50MemoryManager::makeInitVoice();
        v.name           = std::string_view(stagedRefaceVoice_.nameChars.data(), 10u);
        v.transpose      = std::clamp<int8_t>(stagedRefaceVoice_.transpose, -24, 24);
        v.monoMode       = (stagedRefaceVoice_.polyMono != 0u) ? 1u : 0u;
        v.portaTime      = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(stagedRefaceVoice_.portaTime * 99.0 / 127.0)), 0, 99));
        v.portaSwitch    = (v.portaTime > 0u) ? 1u : 0u;
        v.pitchBendRange = std::min<uint8_t>(stagedRefaceVoice_.pbRange, 12u);

        // Map Reface 12-Algorithm index (0..11 = ALG 1..12) -> VF50 8-Algorithm index (0..7 = ALG 1..8)
        static constexpr std::array<uint8_t, 12> kRefaceAlgToVF50 = {
            0u, 1u, 2u, 0u, 1u, 2u, 4u, 4u, 5u, 6u, 6u, 7u
        };
        const uint8_t refAlg = std::min<uint8_t>(stagedRefaceVoice_.algorithm, 11u);
        uint8_t vf50Alg       = kRefaceAlgToVF50[refAlg];
        std::array<std::size_t, 4> opMap = { 0u, 1u, 2u, 3u };
        if (refAlg == 2u && stagedRefaceVoice_.operators[2].feedback > stagedRefaceVoice_.operators[3].feedback
            && stagedRefaceVoice_.operators[3].feedback == 0u) {
            // VF50 ALG 4 ((OP4->OP3 + OP2) -> OP1) is encoded in Reface DX as ALG 3 with op order [0, 3, 1, 2]
            vf50Alg = 3u;
            opMap  = { 0u, 3u, 1u, 2u };
        }
        v.algorithm = vf50Alg;

        // Reface LFO Wave: 0/2=SawUp->0, 3/4=Square->1, 1=Triangle->2, 5/6=S&H->3
        switch (stagedRefaceVoice_.lfoWave & 0x07u) {
            case 0u:
            case 2u: v.lfoWave = 0u; break;
            case 3u:
            case 4u: v.lfoWave = 1u; break;
            case 1u: v.lfoWave = 2u; break;
            default: v.lfoWave = 3u; break;
        }
        v.lfoSpeed = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(stagedRefaceVoice_.lfoSpeed * 99.0 / 127.0)), 0, 99));
        v.lfoDelay = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(stagedRefaceVoice_.lfoDelay * 99.0 / 127.0)), 0, 99));
        v.pmd      = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(stagedRefaceVoice_.lfoPmd * 99.0 / 127.0)), 0, 99));
        v.pms      = (v.pmd > 0u) ? 4u : 3u;
        v.amd      = 0u;
        v.ams      = 0u;
        v.mwPitch  = 50u;

        // PEG conversion (neutral 64 -> 50)
        const bool neutralPeg = (stagedRefaceVoice_.pegLevels[0] == 64u &&
                                 stagedRefaceVoice_.pegLevels[1] == 64u &&
                                 stagedRefaceVoice_.pegLevels[2] == 64u &&
                                 stagedRefaceVoice_.pegLevels[3] == 64u);
        if (neutralPeg) {
            v.peg = { 99u, 99u, 99u, 50u, 50u, 50u };
        } else {
            v.peg.pr1 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(stagedRefaceVoice_.pegRates[0] * 99.0 / 127.0)), 0, 99));
            v.peg.pr2 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(stagedRefaceVoice_.pegRates[1] * 99.0 / 127.0)), 0, 99));
            v.peg.pr3 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(stagedRefaceVoice_.pegRates[2] * 99.0 / 127.0)), 0, 99));
            v.peg.pl1 = static_cast<uint8_t>(std::clamp<int>(50 + (static_cast<int>(stagedRefaceVoice_.pegLevels[0]) - 64), 0, 99));
            v.peg.pl2 = static_cast<uint8_t>(std::clamp<int>(50 + (static_cast<int>(stagedRefaceVoice_.pegLevels[1]) - 64), 0, 99));
            v.peg.pl3 = static_cast<uint8_t>(std::clamp<int>(50 + (static_cast<int>(stagedRefaceVoice_.pegLevels[3]) - 64), 0, 99));
        }

        mapRefaceFxToVF50Effect(stagedRefaceVoice_.fx1Type, stagedRefaceVoice_.fx1P1, stagedRefaceVoice_.fx1P2,
                               stagedRefaceVoice_.fx2Type, stagedRefaceVoice_.fx2P1, stagedRefaceVoice_.fx2P2,
                               v.effect);

        static constexpr std::array<int, 4> kRefaceRsLevels = { 0, 43, 85, 127 };
        static constexpr std::array<int, 8> kRefaceKvsLevels = { 0, 37, 45, 53, 61, 69, 77, 86 };

        v.feedback = 0u;
        for (std::size_t logOp = 0u; logOp < 4u; ++logOp) {
            const auto& srcOp = stagedRefaceVoice_.operators[opMap[logOp]];
            auto& dstOp       = v.operators[logOp];

            dstOp.ar  = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.r1 * 31.0 / 127.0)), 0, 31));
            dstOp.d1r = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.r2 * 31.0 / 127.0)), 0, 31));
            dstOp.rr  = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.r4 * 15.0 / 127.0)), 1, 15));

            // Reface DX L3 is the sustained plateau level; when L3==0 and L2>0, decay L2->0 via D2R
            if (srcOp.l3 == 0u && srcOp.l2 > 0u) {
                dstOp.d1l = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.l2 * 15.0 / 127.0)), 0, 15));
                dstOp.d2r = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.r3 * 31.0 / 127.0)), 1, 31));
            } else {
                dstOp.d1l = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.l3 * 15.0 / 127.0)), 0, 15));
                if (srcOp.l3 >= srcOp.l2 || srcOp.r3 <= 12u) {
                    dstOp.d2r = 0u;
                } else {
                    dstOp.d2r = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.r3 * 31.0 / 127.0)), 0, 31));
                }
            }

            // Rate Scaling (nearest of 0, 43, 85, 127 -> 0..3)
            uint8_t bestRs = 0u;
            int bestRsErr  = 999;
            for (uint8_t rIdx = 0u; rIdx < 4u; ++rIdx) {
                const int err = std::abs(static_cast<int>(srcOp.rateScaling) - kRefaceRsLevels[rIdx]);
                if (err < bestRsErr) {
                    bestRsErr = err;
                    bestRs    = rIdx;
                }
            }
            dstOp.rateScaling = bestRs;

            // Level Scaling (-99..+99)
            if (srcOp.rd == 0u) {
                dstOp.levelScaling = 0;
            } else {
                const int mag = std::clamp<int>(static_cast<int>(std::lround(srcOp.rd * 99.0 / 85.0)), 1, 99);
                dstOp.levelScaling = static_cast<int8_t>((srcOp.rc >= 2u) ? -mag : mag);
            }

            // Velocity Sensitivity (0..7)
            int8_t bestKvs = 0;
            int bestKvsErr = 999;
            for (int kIdx = 0; kIdx < 8; ++kIdx) {
                const int err = std::abs(static_cast<int>(srcOp.veloSens) - kRefaceKvsLevels[static_cast<std::size_t>(kIdx)]);
                if (err < bestKvsErr) {
                    bestKvsErr = err;
                    bestKvs    = static_cast<int8_t>(kIdx);
                }
            }
            dstOp.keyVelSens = bestKvs;

            if (srcOp.lfoAmd > 0u) {
                dstOp.ampModEnable = 1u;
                v.ams = std::max<uint8_t>(v.ams, 2u);
                v.amd = std::max<uint8_t>(v.amd, static_cast<uint8_t>(std::min<int>(srcOp.lfoAmd * 14, 99)));
            } else {
                dstOp.ampModEnable = 0u;
            }

            dstOp.outputLevel = (srcOp.on != 0u)
                ? static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(srcOp.outputLevel * 99.0 / 127.0)), 0, 99))
                : 0u;

            // Per-operator Feedback/Saw/Square -> OPZ II Waveform W1..W8 + OP4 Voice Feedback
            const uint8_t fb     = srcOp.feedback;
            const uint8_t fbType = srcOp.fbType & 0x01u;
            if (logOp == 3u) {
                if (fb == 0u) {
                    dstOp.waveform = 0u;
                    v.feedback     = 0u;
                } else if (fbType == 0u) {
                    if (fb <= 63u) {
                        dstOp.waveform = 0u;
                        v.feedback     = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(fb * 7.0 / 50.0)), 1, 7));
                    } else {
                        dstOp.waveform = 3u;
                        v.feedback     = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround((fb - 64) * 7.0 / 16.0)), 3, 7));
                    }
                } else {
                    dstOp.waveform = (fb >= 80u) ? 6u : 1u;
                    v.feedback     = (fb > 16u && fb < 80u)
                        ? static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround((fb - 16) * 7.0 / 45.0)), 1, 7))
                        : ((fb > 80u) ? static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround((fb - 80) * 7.0 / 35.0)), 1, 7)) : 0u);
                }
            } else {
                if (fbType == 0u) {
                    if (fb < 16u)      dstOp.waveform = 0u;
                    else if (fb < 48u) dstOp.waveform = 3u;
                    else if (fb < 80u) dstOp.waveform = 4u;
                    else               dstOp.waveform = 5u;
                } else {
                    if (fb < 16u)      dstOp.waveform = 0u;
                    else if (fb < 48u) dstOp.waveform = 1u;
                    else if (fb < 92u) dstOp.waveform = 6u;
                    else               dstOp.waveform = 7u;
                }
            }

            // Oscillator Ratio / Fixed Hz -> exact OPZ II Coarse (0..63), Fine (0..15), Range, Shift
            dstOp.fixedFreqMode = srcOp.freqMode & 0x01u;
            if (dstOp.fixedFreqMode == 0u) {
                const uint8_t crs = std::min<uint8_t>(srcOp.coarse, 31u);
                const uint8_t fin = std::min<uint8_t>(srcOp.fine, 99u);
                const double targetRatio = (crs == 0u)
                    ? (0.5 + static_cast<double>(fin) * 0.005)
                    : (static_cast<double>(crs) + static_cast<double>(fin) * 0.01);
                findBestOpzRatio(targetRatio, dstOp.coarse, dstOp.fine);
                dstOp.fixedRange    = 0u;
                dstOp.fixedRangeLow = 0u;
            } else {
                const uint8_t crs = std::min<uint8_t>(srcOp.coarse, 31u);
                const uint8_t fin = std::min<uint8_t>(srcOp.fine, 99u);
                const double targetHz = std::pow(10.0, static_cast<double>(crs) / 8.0 + static_cast<double>(fin) / 100.0);
                findBestOpzFixedHz(targetHz, dstOp.coarse, dstOp.fine, dstOp.fixedRange, dstOp.fixedRangeLow);
            }

            // Detune (-4..+4 -> VF50 -3..+3)
            const int d = std::clamp<int>(srcOp.detune, -4, 4);
            static constexpr std::array<int8_t, 9> kRefaceDetMap = { -3, -2, -2, -1, 0, 1, 2, 2, 3 };
            dstOp.detune = kRefaceDetMap[static_cast<std::size_t>(d + 4)];
        }

        commitDecodedSingleVoice(v, stagedSingleVoiceBankIndex_, memory, synth, res);
        stagedRefaceVoice_.clear();
    };

    if (subcmd == 0x0Eu) {
        if (stagedRefaceVoice_.hasCommon && stagedRefaceVoice_.opsReceivedMask != 0u) {
            commitStagedReface();
            return res;
        }
        stagedRefaceVoice_.clear();
        res.kind     = SysExEventKind::ParameterChange;
        res.accepted = true;
        return res;
    }

    if (subcmd == 0x30u) {
        for (std::size_t i = 0; i < 10u && (10u + i) < innerLen; ++i) {
            const uint8_t ch = body[10u + i] & 0x7Fu;
            stagedRefaceVoice_.nameChars[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
        }
        if (innerLen > 22u) stagedRefaceVoice_.transpose = static_cast<int8_t>(std::clamp<int>(static_cast<int>(body[22] & 0x7Fu) - 64, -24, 24));
        if (innerLen > 23u) stagedRefaceVoice_.polyMono  = body[23] & 0x7Fu;
        if (innerLen > 24u) stagedRefaceVoice_.portaTime = body[24] & 0x7Fu;
        if (innerLen > 25u) stagedRefaceVoice_.pbRange   = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(body[25] & 0x7Fu) - 64, 0, 12));
        if (innerLen > 26u) stagedRefaceVoice_.algorithm = body[26] & 0x0Fu;
        if (innerLen > 27u) stagedRefaceVoice_.lfoWave   = body[27] & 0x07u;
        if (innerLen > 28u) stagedRefaceVoice_.lfoSpeed  = body[28] & 0x7Fu;
        if (innerLen > 29u) stagedRefaceVoice_.lfoDelay  = body[29] & 0x7Fu;
        if (innerLen > 30u) stagedRefaceVoice_.lfoPmd    = body[30] & 0x7Fu;
        for (std::size_t i = 0; i < 4u; ++i) {
            if (innerLen > 31u + i) stagedRefaceVoice_.pegRates[i]  = body[31u + i] & 0x7Fu;
            if (innerLen > 35u + i) stagedRefaceVoice_.pegLevels[i] = body[35u + i] & 0x7Fu;
        }
        if (innerLen > 41u) {
            stagedRefaceVoice_.fx1Type = body[39] & 0x0Fu;
            stagedRefaceVoice_.fx1P1   = body[40] & 0x7Fu;
            stagedRefaceVoice_.fx1P2   = body[41] & 0x7Fu;
        }
        if (innerLen > 44u) {
            stagedRefaceVoice_.fx2Type = body[42] & 0x0Fu;
            stagedRefaceVoice_.fx2P1   = body[43] & 0x7Fu;
            stagedRefaceVoice_.fx2P2   = body[44] & 0x7Fu;
        }
        stagedRefaceVoice_.hasCommon = true;

        if (stagedRefaceVoice_.opsReceivedMask == 0x0Fu) {
            commitStagedReface();
            return res;
        }

        res.kind     = SysExEventKind::ParameterChange;
        res.accepted = true;
        return res;
    }

    if (subcmd == 0x31u) {
        const std::size_t opIdx = std::min<std::size_t>(body[8] & 0x03u, 3u);
        auto& op = stagedRefaceVoice_.operators[opIdx];

        if (innerLen > 10u) op.on          = body[10] & 0x01u;
        if (innerLen > 11u) op.r1          = body[11] & 0x7Fu;
        if (innerLen > 12u) op.r2          = body[12] & 0x7Fu;
        if (innerLen > 13u) op.r3          = body[13] & 0x7Fu;
        if (innerLen > 14u) op.r4          = body[14] & 0x7Fu;
        if (innerLen > 15u) op.l1          = body[15] & 0x7Fu;
        if (innerLen > 16u) op.l2          = body[16] & 0x7Fu;
        if (innerLen > 17u) op.l3          = body[17] & 0x7Fu;
        if (innerLen > 18u) op.l4          = body[18] & 0x7Fu;
        if (innerLen > 19u) op.rateScaling = body[19] & 0x7Fu;
        if (innerLen > 20u) op.ld          = body[20] & 0x7Fu;
        if (innerLen > 21u) op.rd          = body[21] & 0x7Fu;
        if (innerLen > 22u) op.lc          = body[22] & 0x03u;
        if (innerLen > 23u) op.rc          = body[23] & 0x03u;
        if (innerLen > 24u) op.lfoAmd      = body[24] & 0x0Fu;
        if (innerLen > 27u) op.veloSens    = body[27] & 0x7Fu;
        if (innerLen > 28u) op.outputLevel = body[28] & 0x7Fu;
        if (innerLen > 29u) op.feedback    = body[29] & 0x7Fu;
        if (innerLen > 30u) op.fbType      = body[30] & 0x01u;
        if (innerLen > 31u) op.freqMode    = body[31] & 0x01u;
        if (innerLen > 32u) op.coarse      = body[32] & 0x3Fu;
        if (innerLen > 33u) op.fine        = body[33] & 0x7Fu;
        if (innerLen > 34u) op.detune      = static_cast<int8_t>(std::clamp<int>(static_cast<int>(body[34] & 0x7Fu) - 64, -7, 7));

        stagedRefaceVoice_.opsReceivedMask |= static_cast<uint8_t>(1u << opIdx);

        if (stagedRefaceVoice_.hasCommon && stagedRefaceVoice_.opsReceivedMask == 0x0Fu) {
            commitStagedReface();
            return res;
        }

        res.kind     = SysExEventKind::ParameterChange;
        res.accepted = true;
        return res;
    }

    if (subcmd == 0x0Fu) {
        if (stagedRefaceVoice_.hasCommon) {
            commitStagedReface();
            return res;
        }
        stagedRefaceVoice_.clear();
        res.kind     = SysExEventKind::ParameterChange;
        res.accepted = true;
        return res;
    }

    // Accept other Reface DX header/system packets (e.g. 0x00/0x10) harmlessly
    res.kind     = SysExEventKind::ParameterChange;
    res.accepted = true;
    return res;
}

// =============================================================================
// SysEx Bulk Emitters
// =============================================================================

bool VF50SysExHandler::emitSingleVoiceEditDump(const vf50::generated::FactoryVoice& v,
                                              SysExReplyBuffer& out,
                                              bool includeAced,
                                              bool includeAced2,
                                              bool includeAced3) const noexcept {
    const uint8_t txCh = transmitChannelNibble();

    // 1. ACED3 ("LM  8073AE", 20 data bytes)
    if (includeAced3) {
        std::array<uint8_t, 20> aced3{};
        aced3[0] = std::min<uint8_t>(v.effect.effectType, 32u);
        aced3[1] = std::min<uint8_t>(v.effect.balance, 100u);
        aced3[2] = std::min<uint8_t>(v.effect.outLevel, 100u);
        aced3[3] = v.effect.stereoMix & 0x01u;
        aced3[4] = std::min<uint8_t>(v.effect.param1, 75u);
        aced3[5] = std::min<uint8_t>(v.effect.param2, 99u);
        aced3[6] = std::min<uint8_t>(v.effect.param3, 99u);
        if (!appendLmBulkPacket(txCh, "LM  8073AE", aced3.data(), aced3.size(), out)) return false;
    }

    // 2. ACED2 ("LM  8023AE", 10 data bytes)
    if (includeAced2) {
        std::array<uint8_t, 10> aced2{};
        aced2[0] = std::min<uint8_t>(v.atPitch, 99u);
        aced2[1] = std::min<uint8_t>(v.atAmp, 99u);
        aced2[2] = encodeMidiAtPitchBias(v.atPitchBias);
        aced2[3] = std::min<uint8_t>(v.atEgBias, 99u);
        for (std::size_t slot = 0; slot < 4u; ++slot) {
            const std::size_t logOp = kLogicalOpForSysExSlot[slot];
            aced2[4u + slot] = v.operators[logOp].fixedRangeLow & 0x01u;
        }
        uint8_t lsSign = 0u;
        for (std::size_t op = 0; op < 4u; ++op) {
            if (v.operators[op].levelScaling < 0) {
                lsSign |= static_cast<uint8_t>(1u << (3u - op));
            }
        }
        aced2[8] = lsSign;
        aced2[9] = 0u;
        if (!appendLmBulkPacket(txCh, "LM  8023AE", aced2.data(), aced2.size(), out)) return false;
    }

    // 3. ACED ("LM  8976AE", 23 data bytes)
    if (includeAced) {
        std::array<uint8_t, 23> aced{};
        for (std::size_t slot = 0; slot < 4u; ++slot) {
            const std::size_t logOp = kLogicalOpForSysExSlot[slot];
            const auto& o = v.operators[logOp];
            uint8_t* ob = &aced[slot * 5u];
            ob[0] = o.fixedFreqMode & 0x01u;
            ob[1] = o.fixedRange & 0x07u;
            ob[2] = o.fine & 0x0Fu;
            ob[3] = o.waveform & 0x07u;
            ob[4] = o.egShift & 0x03u;
        }
        aced[20] = v.reverbRate & 0x07u;
        aced[21] = std::min<uint8_t>(v.fcPitch, 99u);
        aced[22] = std::min<uint8_t>(v.fcAmp, 99u);
        if (!appendLmBulkPacket(txCh, "LM  8976AE", aced.data(), aced.size(), out)) return false;
    }

    // 4. VCED (fmt 0x03, 93 bytes)
    std::array<uint8_t, 93> vced{};
    for (std::size_t slot = 0; slot < 4u; ++slot) {
        const std::size_t logOp = kLogicalOpForSysExSlot[slot];
        const auto& o = v.operators[logOp];
        uint8_t* ob = &vced[slot * 13u];

        ob[0]  = o.ar & 0x1Fu;
        ob[1]  = o.d1r & 0x1Fu;
        ob[2]  = o.d2r & 0x1Fu;
        ob[3]  = std::clamp<uint8_t>(o.rr, 1u, 15u) & 0x0Fu;
        ob[4]  = o.d1l & 0x0Fu;
        ob[5]  = static_cast<uint8_t>(std::min<int>(std::abs(static_cast<int>(o.levelScaling)), 99));
        ob[6]  = o.rateScaling & 0x03u;
        ob[7]  = o.egBiasSens & 0x07u;
        ob[8]  = o.ampModEnable & 0x01u;
        ob[9]  = encodeVcedKvs(o.keyVelSens);
        ob[10] = std::min<uint8_t>(o.outputLevel, 99u);
        ob[11] = o.coarse & 0x3Fu;
        ob[12] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(o.detune) + 3, 0, 6));
    }

    vced[52] = v.algorithm & 0x07u;
    vced[53] = v.feedback & 0x07u;
    vced[54] = std::min<uint8_t>(v.lfoSpeed, 99u);
    vced[55] = std::min<uint8_t>(v.lfoDelay, 99u);
    vced[56] = std::min<uint8_t>(v.pmd, 99u);
    vced[57] = std::min<uint8_t>(v.amd, 99u);
    vced[58] = v.lfoSync & 0x01u;
    vced[59] = v.lfoWave & 0x03u;
    vced[60] = v.pms & 0x07u;
    vced[61] = v.ams & 0x03u;
    vced[62] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(v.transpose) + 24, 0, 48));
    vced[63] = v.monoMode & 0x01u;
    vced[64] = std::min<uint8_t>(v.pitchBendRange, 12u);
    vced[65] = v.portaMode & 0x01u;
    vced[66] = std::min<uint8_t>(v.portaTime, 99u);
    vced[67] = std::min<uint8_t>(v.fcVolume, 99u);
    vced[68] = v.sustainSwitch & 0x01u;
    vced[69] = v.portaSwitch & 0x01u;
    vced[70] = v.chorusSwitch & 0x01u;
    vced[71] = std::min<uint8_t>(v.mwPitch, 99u);
    vced[72] = std::min<uint8_t>(v.mwAmp, 99u);
    vced[73] = std::min<uint8_t>(v.bcPitch, 99u);
    vced[74] = std::min<uint8_t>(v.bcAmp, 99u);
    vced[75] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(v.bcPitchBias) + 50, 0, 100));
    vced[76] = std::min<uint8_t>(v.bcEgBias, 99u);

    for (std::size_t i = 0; i < 10u; ++i) {
        char c = (i < v.name.size()) ? v.name[i] : ' ';
        if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) > 0x7Eu) {
            c = ' ';
        }
        vced[77u + i] = static_cast<uint8_t>(c);
    }

    vced[87] = std::min<uint8_t>(v.peg.pr1, 99u);
    vced[88] = std::min<uint8_t>(v.peg.pr2, 99u);
    vced[89] = std::min<uint8_t>(v.peg.pr3, 99u);
    vced[90] = std::min<uint8_t>(v.peg.pl1, 99u);
    vced[91] = std::min<uint8_t>(v.peg.pl2, 99u);
    vced[92] = std::min<uint8_t>(v.peg.pl3, 99u);

    return appendFramedBulkPacket(txCh, 0x03u, vced.data(), vced.size(), out);
}

bool VF50SysExHandler::emitSingleVoiceBulkDump32(const vf50::state::VF50MemoryManager& memory,
                                                vf50::state::MemoryBank bank,
                                                std::size_t startIndex,
                                                SysExReplyBuffer& out) const noexcept {
    std::array<uint8_t, 4096> vmem{};
    const auto initVoice = vf50::state::VF50MemoryManager::makeInitVoice();
    for (std::size_t i = 0; i < 32u; ++i) {
        const std::size_t idx = startIndex + i;
        const auto& v = (idx < vf50::state::VF50MemoryManager::kVoiceBankSize)
                            ? memory.getVoice(bank, idx)
                            : initVoice;
        packVoiceToVmem128(v, vmem.data() + i * 128u);
    }
    return appendFramedBulkPacket(transmitChannelNibble(), 0x04u, vmem.data(), vmem.size(), out);
}

bool VF50SysExHandler::emitSingleVoiceBankDump100(const vf50::state::VF50MemoryManager& memory,
                                                 vf50::state::MemoryBank bank,
                                                 SysExReplyBuffer& out) const noexcept {
    const uint8_t txCh = transmitChannelNibble();
    for (uint8_t block = 1u; block <= 4u; ++block) {
        const std::array<uint8_t, 7> hdr = {
            kSysExStart, kSysExManufacturerId, static_cast<uint8_t>(0x10u | txCh), 0x44u, 0x07u, block, kSysExEnd
        };
        if (!out.appendPacket(hdr.data(), hdr.size())) return false;
        if (!emitSingleVoiceBulkDump32(memory, bank, static_cast<std::size_t>(block - 1u) * 32u, out)) {
            return false;
        }
    }
    return true;
}

bool VF50SysExHandler::emitPerformanceEditDump(const vf50::generated::FactoryPerformance& pf,
                                              SysExReplyBuffer& out,
                                              bool includePced2) const noexcept {
    const uint8_t txCh = transmitChannelNibble();

    if (includePced2) {
        std::array<uint8_t, 33> pced2{};
        for (std::size_t i = 0; i < 8u; ++i) {
            pced2[i * 2u]      = std::min<uint8_t>(pf.instruments[i].reserveNotes, 17u);
            pced2[i * 2u + 1u] = pf.instruments[i].effectEnable & 0x01u;
        }
        pced2[16] = std::min<uint8_t>(pf.effect.effectType, 32u);
        pced2[17] = std::min<uint8_t>(pf.effect.balance, 100u);
        pced2[18] = std::min<uint8_t>(pf.effect.outLevel, 100u);
        pced2[19] = pf.effect.stereoMix & 0x01u;
        pced2[20] = std::min<uint8_t>(pf.effect.param1, 75u);
        pced2[21] = std::min<uint8_t>(pf.effect.param2, 99u);
        pced2[22] = std::min<uint8_t>(pf.effect.param3, 99u);
        if (!appendLmBulkPacket(txCh, "LM  8073PE", pced2.data(), pced2.size(), out)) return false;
    }

    std::array<uint8_t, 110> pced{};
    for (std::size_t i = 0; i < 8u; ++i) {
        const auto& inst = pf.instruments[i];
        uint8_t* ib = &pced[i * 12u];
        ib[0]  = std::min<uint8_t>(inst.maxNotes, 16u);
        ib[1]  = inst.voiceBank & 0x01u;
        ib[2]  = std::min<uint8_t>(inst.voiceNumber, 99u);
        ib[3]  = std::min<uint8_t>(inst.receiveChannel, 17u);
        ib[4]  = std::min<uint8_t>(inst.limitLow, 127u);
        ib[5]  = std::min<uint8_t>(inst.limitHigh, 127u);
        ib[6]  = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(inst.detune) + 7, 0, 14));
        ib[7]  = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(inst.noteShift) + 24, 0, 48));
        ib[8]  = std::min<uint8_t>(inst.volume, 99u);
        ib[9]  = inst.outAssign & 0x03u;
        ib[10] = inst.lfoSelect & 0x03u;
        ib[11] = inst.microTuneEnable & 0x01u;
    }
    pced[96] = std::min<uint8_t>(pf.microTuneTable, 12u);
    pced[97] = std::min<uint8_t>(pf.assignMode, 2u);
    pced[98] = std::min<uint8_t>(pf.perfEffectSelect, 12u);
    pced[99] = std::min<uint8_t>(pf.microTuneRootKey, 11u);

    for (std::size_t i = 0; i < 10u; ++i) {
        char c = (i < pf.name.size()) ? pf.name[i] : ' ';
        if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) > 0x7Eu) {
            c = ' ';
        }
        pced[100u + i] = static_cast<uint8_t>(c);
    }

    return appendLmBulkPacket(txCh, "LM  8976PE", pced.data(), pced.size(), out);
}

bool VF50SysExHandler::emitPerformanceBulkBlock32(const vf50::state::VF50MemoryManager& memory,
                                                 vf50::state::MemoryBank bank,
                                                 std::size_t blockIndex0To3,
                                                 SysExReplyBuffer& out,
                                                 bool includeBlockHeader,
                                                 bool includePmem2) const noexcept {
    const uint8_t txCh = transmitChannelNibble();
    const uint8_t blk  = static_cast<uint8_t>(std::min<std::size_t>(blockIndex0To3, 3u));

    if (includeBlockHeader) {
        const std::array<uint8_t, 8> hdr = {
            kSysExStart, kSysExManufacturerId, static_cast<uint8_t>(0x10u | txCh), 0x10u, 0x75u, 0x01u, blk, kSysExEnd
        };
        if (!out.appendPacket(hdr.data(), hdr.size())) return false;
    }

    std::array<uint8_t, 32u * 76u> pmem{};
    std::array<uint8_t, 32u * 25u> pmem2{};
    const auto initPerf = vf50::state::VF50MemoryManager::makeInitPerformance(vf50::state::PerfInitTemplate::All);
    const std::size_t baseSlot = static_cast<std::size_t>(blk) * 32u;

    for (std::size_t i = 0; i < 32u; ++i) {
        const std::size_t idx = baseSlot + i;
        const auto& pf = (idx < vf50::state::VF50MemoryManager::kPerfBankSize)
                             ? memory.getPerformance(bank, idx)
                             : initPerf;
        packPerfToPmem76AndPmem2_25(pf, pmem.data() + i * 76u, pmem2.data() + i * 25u);
    }

    if (!appendLmBulkPacket(txCh, "LM  8976PM", pmem.data(), pmem.size(), out)) return false;
    if (includePmem2) {
        if (!appendLmBulkPacket(txCh, "LM  8073PM", pmem2.data(), pmem2.size(), out)) return false;
    }
    return true;
}

bool VF50SysExHandler::emitPerformanceBankDump100(const vf50::state::VF50MemoryManager& memory,
                                                 vf50::state::MemoryBank bank,
                                                 SysExReplyBuffer& out) const noexcept {
    for (std::size_t blk = 0; blk < 4u; ++blk) {
        if (!emitPerformanceBulkBlock32(memory, bank, blk, out, true, true)) return false;
    }
    return true;
}

bool VF50SysExHandler::emitMicrotuningOctaveDump(
    const std::array<vf50::generated::MicrotuningEntry, 12>& table,
    SysExReplyBuffer& out) const noexcept {
    std::array<uint8_t, 24> body{};
    for (std::size_t i = 0; i < 12u; ++i) {
        body[i * 2u]      = table[i].coarseNote & 0x7Fu;
        body[i * 2u + 1u] = table[i].fineStep64 & 0x3Fu;
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  MCRTE0", body.data(), body.size(), out);
}

bool VF50SysExHandler::emitMicrotuningFullDump(
    const std::array<vf50::generated::MicrotuningEntry, 128>& table,
    SysExReplyBuffer& out) const noexcept {
    std::array<uint8_t, 256> body{};
    for (std::size_t i = 0; i < 128u; ++i) {
        body[i * 2u]      = table[i].coarseNote & 0x7Fu;
        body[i * 2u + 1u] = table[i].fineStep64 & 0x3Fu;
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  MCRTE1", body.data(), body.size(), out);
}

bool VF50SysExHandler::emitRhythmSystemDump(const vf50::state::VF50MemoryManager& memory,
                                           const vf50::dsp::VF50SynthEngine& synth,
                                           SysExReplyBuffer& out) const noexcept {
    const auto& kit = memory.activeRhythmKit();
    std::array<uint8_t, 16> sysr{};
    sysr[0]  = std::min<uint8_t>(kit.rhythmVolume, 99u);
    sysr[1]  = static_cast<uint8_t>(kit.effectMode) & 0x03u;
    sysr[2]  = 0u; // kbd assign = rhy
    sysr[3]  = 0u;
    sysr[4]  = std::min<uint8_t>(synth.drums().rhythmReceiveChannel(), 17u);
    sysr[5]  = 16u;
    sysr[6]  = 1u; // velocity sw on
    sysr[7]  = 0u;
    sysr[8]  = 0u;
    sysr[9]  = std::min<uint8_t>(kit.keyMapSelect, 4u);
    sysr[10] = (memory.selectedRhythmPatternBank() == vf50::state::MemoryBank::FactoryRom) ? 1u : 0u;
    sysr[11] = static_cast<uint8_t>(std::min<std::size_t>(memory.selectedRhythmPatternIndex(), 99u));
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8073RS", sysr.data(), sysr.size(), out);
}

bool VF50SysExHandler::emitRhythmInstrumentsDump(const vf50::state::RhythmKitBank& kit,
                                                SysExReplyBuffer& out) const noexcept {
    std::array<uint8_t, 183> rinst{};
    for (std::size_t d = 0; d < vf50::generated::kDrumInstrumentCount; ++d) {
        rinst[d]        = std::min<uint8_t>(kit.instruments[d].volume, 15u);
        rinst[61u + d]  = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(kit.instruments[d].pan) + 3, 0, 6));
        rinst[122u + d] = kit.instruments[d].midiNote & 0x7Fu;
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8073RI", rinst.data(), rinst.size(), out);
}

bool VF50SysExHandler::emitRhythmKeyMapDump(const std::array<int8_t, 61>& keyMap,
                                           std::size_t userTable0Or1,
                                           SysExReplyBuffer& out) const noexcept {
    std::array<uint8_t, 61> rkat{};
    for (std::size_t k = 0; k < 61u; ++k) {
        const int idx = static_cast<int>(keyMap[k]);
        rkat[k] = (idx >= 0 && idx < 61) ? static_cast<uint8_t>(idx) : 61u;
    }
    if (userTable0Or1 == 0u) {
        return appendLmBulkPacket(transmitChannelNibble(), "LM  8073K0", rkat.data(), rkat.size(), out);
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8073K1", rkat.data(), rkat.size(), out);
}

bool VF50SysExHandler::emitVoiceRangeBulkDump(const std::array<vf50::state::StoredVoiceSlot, 100>& voices,
                                             std::size_t startIdx,
                                             std::size_t count,
                                             SysExReplyBuffer& out) const noexcept {
    if (!deviceEnabled_ || startIdx >= 100u || count == 0u) return false;
    const uint8_t txCh = transmitChannelNibble();
    const std::size_t clampedEnd = std::min<std::size_t>(100u, startIdx + count);
    if (count <= 25u) {
        std::array<uint8_t, 4096> vmem{};
        const auto initVoice = vf50::state::VF50MemoryManager::makeInitVoice();
        for (std::size_t i = 0u; i < 32u; ++i) {
            const std::size_t idx = startIdx + i;
            const auto& v = (idx < clampedEnd) ? voices[idx].voice : initVoice;
            packVoiceToVmem128(v, vmem.data() + i * 128u);
        }
        return appendFramedBulkPacket(txCh, 0x04u, vmem.data(), vmem.size(), out);
    }
    const auto initVoice = vf50::state::VF50MemoryManager::makeInitVoice();
    for (uint8_t block = 1u; block <= 4u; ++block) {
        const std::array<uint8_t, 7> hdr = {
            kSysExStart, kSysExManufacturerId, static_cast<uint8_t>(0x10u | txCh), 0x44u, 0x07u, block, kSysExEnd
        };
        if (!out.appendPacket(hdr.data(), hdr.size())) return false;
        std::array<uint8_t, 4096> vmem{};
        const std::size_t base = static_cast<std::size_t>(block - 1u) * 32u;
        for (std::size_t i = 0u; i < 32u; ++i) {
            const std::size_t idx = base + i;
            const auto& v = (idx < 100u) ? voices[idx].voice : initVoice;
            packVoiceToVmem128(v, vmem.data() + i * 128u);
        }
        if (!appendFramedBulkPacket(txCh, 0x04u, vmem.data(), vmem.size(), out)) return false;
    }
    return true;
}

bool VF50SysExHandler::emitPerfRangeBulkDump(const std::array<vf50::state::StoredPerformanceSlot, 100>& perfs,
                                            std::size_t startIdx,
                                            std::size_t count,
                                            SysExReplyBuffer& out) const noexcept {
    if (!deviceEnabled_ || startIdx >= 100u || count == 0u) return false;
    const uint8_t txCh = transmitChannelNibble();
    const auto initPerf = vf50::state::VF50MemoryManager::makeInitPerformance(vf50::state::PerfInitTemplate::All);
    if (count <= 25u) {
        const uint8_t blk = static_cast<uint8_t>(std::min<std::size_t>(startIdx / 25u, 3u));
        const std::array<uint8_t, 8> hdr = {
            kSysExStart, kSysExManufacturerId, static_cast<uint8_t>(0x10u | txCh), 0x10u, 0x75u, 0x01u, blk, kSysExEnd
        };
        if (!out.appendPacket(hdr.data(), hdr.size())) return false;
        std::array<uint8_t, 32u * 76u> pmem{};
        std::array<uint8_t, 32u * 25u> pmem2{};
        const std::size_t clampedEnd = std::min<std::size_t>(100u, startIdx + count);
        for (std::size_t i = 0u; i < 32u; ++i) {
            const std::size_t idx = startIdx + i;
            const auto& pf = (idx < clampedEnd) ? perfs[idx].perf : initPerf;
            packPerfToPmem76AndPmem2_25(pf, pmem.data() + i * 76u, pmem2.data() + i * 25u);
        }
        if (!appendLmBulkPacket(txCh, "LM  8976PM", pmem.data(), pmem.size(), out)) return false;
        return appendLmBulkPacket(txCh, "LM  8073PM", pmem2.data(), pmem2.size(), out);
    }
    for (uint8_t blk = 0u; blk < 4u; ++blk) {
        const std::array<uint8_t, 8> hdr = {
            kSysExStart, kSysExManufacturerId, static_cast<uint8_t>(0x10u | txCh), 0x10u, 0x75u, 0x01u, blk, kSysExEnd
        };
        if (!out.appendPacket(hdr.data(), hdr.size())) return false;
        std::array<uint8_t, 32u * 76u> pmem{};
        std::array<uint8_t, 32u * 25u> pmem2{};
        const std::size_t base = static_cast<std::size_t>(blk) * 32u;
        for (std::size_t i = 0u; i < 32u; ++i) {
            const std::size_t idx = base + i;
            const auto& pf = (idx < 100u) ? perfs[idx].perf : initPerf;
            packPerfToPmem76AndPmem2_25(pf, pmem.data() + i * 76u, pmem2.data() + i * 25u);
        }
        if (!appendLmBulkPacket(txCh, "LM  8976PM", pmem.data(), pmem.size(), out)) return false;
        if (!appendLmBulkPacket(txCh, "LM  8073PM", pmem2.data(), pmem2.size(), out)) return false;
    }
    return true;
}

bool VF50SysExHandler::emitProgramChangeTableDump(const vf50::state::ProgramChangeTable& pct,
                                                 SysExReplyBuffer& out) const noexcept {
    if (!deviceEnabled_) return false;
    std::array<uint8_t, 256> body{};
    for (std::size_t i = 0u; i < 128u; ++i) {
        const auto& e = pct.entries[i];
        body[i * 2u]      = static_cast<uint8_t>((e.isPerformance ? 0x40u : 0x00u) | (e.bank & 0x03u));
        body[i * 2u + 1u] = std::min<uint8_t>(e.number, 99u) & 0x7Fu;
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8952PC", body.data(), body.size(), out);
}

bool VF50SysExHandler::emitPerfEffectBankDump(const vf50::state::PerfEffectBank& pefct,
                                             SysExReplyBuffer& out) const noexcept {
    if (!deviceEnabled_) return false;
    std::array<uint8_t, 224> body{};
    std::size_t p = 0u;
    for (std::size_t i = 0u; i < 4u; ++i) {
        const auto& d = pefct.delays[i];
        body[p++] = static_cast<uint8_t>(std::clamp<int>(d.delayTimeCs, 1, 127) & 0x7Fu);
        body[p++] = static_cast<uint8_t>(std::clamp<int>(d.pitchShift + 24, 0, 48) & 0x7Fu);
        body[p++] = d.feedback & 0x07u;
        body[p++] = std::min<uint8_t>(d.effectLevel, 99u) & 0x7Fu;
    }
    for (std::size_t i = 0u; i < 4u; ++i) {
        const auto& pn = pefct.pans[i];
        body[p++] = pn.select & 0x03u;
        body[p++] = pn.direction & 0x01u;
        body[p++] = std::min<uint8_t>(pn.panRange, 99u) & 0x7Fu;
    }
    for (std::size_t c = 0u; c < 4u; ++c) {
        for (std::size_t pc = 0u; pc < 12u; ++pc) {
            for (std::size_t s = 0u; s < 4u; ++s) {
                const int8_t v = pefct.chords[c].chords[pc][s];
                body[p++] = (v < 0) ? 0x7Fu : (static_cast<uint8_t>(v) & 0x7Fu);
            }
        }
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8952EF", body.data(), p, out);
}

bool VF50SysExHandler::emitSystemDataDump(const vf50::state::VF50SystemSettings& sys,
                                         bool intProtect,
                                         bool cardProtect,
                                         SysExReplyBuffer& out) const noexcept {
    if (!deviceEnabled_) return false;
    std::array<uint8_t, 64> body{};
    body[0]  = static_cast<uint8_t>(std::clamp<int>(sys.masterTuningSteps + 64, 0, 127) & 0x7Fu);
    body[1]  = std::min<uint8_t>(sys.synthVolume, 99u) & 0x7Fu;
    body[2]  = sys.combineWithFunction ? 1u : 0u;
    body[3]  = sys.controllerReset ? 1u : 0u;
    body[4]  = sys.fixedVelocity & 0x7Fu;
    body[5]  = sys.velocityCurve & 0x07u;
    body[6]  = sys.egDampSpeed & 0x03u;
    body[7]  = sys.voiceDampOn ? 1u : 0u;
    body[8]  = sys.midiOn ? 1u : 0u;
    body[9]  = sys.basicReceiveCh & 0x1Fu;
    body[10] = sys.transmitCh & 0x0Fu;
    body[11] = sys.localOn ? 1u : 0u;
    body[12] = sys.controlChangeMode & 0x1Fu;
    body[13] = sys.aftertouchMode & 0x1Fu;
    body[14] = sys.pitchBendMode & 0x1Fu;
    body[15] = sys.noteOnOffFilter & 0x03u;
    body[16] = sys.dataEntryAssignCc & 0x1Fu;
    body[17] = sys.pgmChangeMode & 0x07u;
    body[18] = sys.deviceNumber & 0x1Fu;
    body[19] = static_cast<uint8_t>((intProtect ? 1u : 0u) | (cardProtect ? 2u : 0u));
    for (std::size_t i = 0u; i < 40u; ++i) {
        body[20u + i] = static_cast<uint8_t>(sys.powerOnMessage[i]) & 0x7Fu;
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8952SY", body.data(), 60u, out);
}

bool VF50SysExHandler::emitSequenceBulkDump(const vf50::dsp::CustomSeqSongBuffer* seq,
                                           SysExReplyBuffer& out) const noexcept {
    if (!deviceEnabled_) return false;
    std::array<uint8_t, 64> body{};
    if (seq != nullptr && seq->valid) {
        for (std::size_t i = 0u; i < 16u; ++i) {
            body[i] = static_cast<uint8_t>(seq->titleChars[i]) & 0x7Fu;
        }
        body[16] = static_cast<uint8_t>(seq->initialTempoBpm & 0x7Fu);
        body[17] = static_cast<uint8_t>((seq->initialTempoBpm >> 7u) & 0x7Fu);
        body[18] = seq->timeSigNum & 0x7Fu;
        body[19] = seq->timeSigDen & 0x7Fu;
        body[20] = static_cast<uint8_t>(seq->totalBars & 0x7Fu);
        body[21] = static_cast<uint8_t>((seq->totalBars >> 7u) & 0x7Fu);
        const std::size_t evSampleCount = std::min<std::size_t>(seq->eventCount, 8u);
        for (std::size_t e = 0u; e < evSampleCount; ++e) {
            const auto& ev = seq->events[e];
            body[24u + e * 4u]      = static_cast<uint8_t>(ev.type) & 0x7Fu;
            body[24u + e * 4u + 1u] = ev.channel & 0x0Fu;
            body[24u + e * 4u + 2u] = ev.data1 & 0x7Fu;
            body[24u + e * 4u + 3u] = ev.data2 & 0x7Fu;
        }
    } else {
        const char* defTitle = "EMPTY SEQ       ";
        for (std::size_t i = 0u; i < 16u; ++i) {
            body[i] = static_cast<uint8_t>(defTitle[i]);
        }
        body[16] = 120u;
        body[18] = 4u;
        body[19] = 4u;
        body[20] = 1u;
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8952SQ", body.data(), body.size(), out);
}

bool VF50SysExHandler::emitRhythmSeqBulkDump(const vf50::state::VF50MemoryManager& memory,
                                            SysExReplyBuffer& out) const noexcept {
    if (!deviceEnabled_) return false;
    const auto& u0 = memory.userRhythmPattern(0u);
    std::array<uint8_t, 64> body{};
    body[0] = u0.bars & 0x7Fu;
    body[1] = u0.timeSigNum & 0x7Fu;
    body[2] = u0.timeSigDen & 0x7Fu;
    body[3] = static_cast<uint8_t>(u0.eventCount & 0x7Fu);
    body[4] = static_cast<uint8_t>((u0.eventCount >> 7u) & 0x7Fu);
    const std::size_t evCount = std::min<std::size_t>(u0.eventCount, 12u);
    for (std::size_t e = 0u; e < evCount; ++e) {
        const auto& ev = u0.events[e];
        body[8u + e * 4u]      = static_cast<uint8_t>(ev.tick & 0x7Fu);
        body[8u + e * 4u + 1u] = static_cast<uint8_t>((ev.tick >> 7u) & 0x7Fu);
        body[8u + e * 4u + 2u] = ev.drumIndex & 0x7Fu;
        body[8u + e * 4u + 3u] = ev.velocityLevel & 0x0Fu;
    }
    return appendLmBulkPacket(transmitChannelNibble(), "LM  8952RS", body.data(), body.size(), out);
}

} // namespace vf50::midi
