#include "ROM/VF50RomLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace vf50::rom {

namespace {

inline constexpr std::array<RomChipSpec, 4> kRomChipSpecs = {{
    {
        RomChipId::MainCpuA_IC712,
        "IC712 XF805 C0.BIN",
        131072u,
        "Main-CPU ROM A (100 Factory Voices, 100 Performances, 11 Microtunings, FX Names)"
    },
    {
        RomChipId::MainCpuB_IC723,
        "IC723 XF810 C0.BIN",
        131072u,
        "Main-CPU ROM B (61 Drum Names, 100 Rhythm Patterns, 3 Built-in Demo Songs)"
    },
    {
        RomChipId::SubCpu_IC731,
        "IC731 XF804 B0.BIN",
        32768u,
        "Sub-CPU ROM (OPRW Drum Map Tables, 32 YM3413 DSP Presets, TL Curve)"
    },
    {
        RomChipId::DrumWave_IC738,
        "XF653A0-101 IC738.BIN",
        524288u,
        "YM3602 OPRW Drum Waveform ROM (61 PCM/DPCM Percussion Samples)"
    }
}};

[[nodiscard]] bool matchesFnv1a64(const uint8_t* data, std::size_t sizeBytes,
                                  std::size_t offset, std::size_t len, uint64_t expectedHash) noexcept {
    if (data == nullptr || offset + len > sizeBytes) {
        return false;
    }
    uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[offset + i]);
        h *= 0x100000001b3ULL;
    }
    return h == expectedHash;
}

[[nodiscard]] uint16_t readBE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | static_cast<uint16_t>(p[1]));
}

[[nodiscard]] uint16_t readLE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u));
}

template <std::size_t N>
void storeFixedName(std::array<char, N>& dst, const uint8_t* src, std::size_t srcLen, bool trimTrailing) noexcept {
    dst.fill(' ');
    std::size_t len = std::min(srcLen, N);
    if (trimTrailing) {
        while (len > 0u && (src[len - 1u] == ' ' || src[len - 1u] == '\0')) {
            --len;
        }
    }
    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t b = src[i];
        dst[i] = (b >= 0x20u && b <= 0x7Eu) ? static_cast<char>(b) : ' ';
    }
}

void copyCString(char* dst, std::size_t dstCap, std::string_view src) noexcept {
    if (dst == nullptr || dstCap == 0u) {
        return;
    }
    const std::size_t n = std::min(src.size(), dstCap - 1u);
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

[[nodiscard]] bool ticksPerBarFromTs(uint8_t ts, uint8_t& outNum, uint8_t& outDen, uint16_t& outTpb) noexcept {
    if (ts <= 3u) {
        outNum = static_cast<uint8_t>(ts + 1u);
        outDen = 4u;
        outTpb = static_cast<uint16_t>(outNum * 48u);
        return true;
    }
    if (ts <= 11u) {
        outNum = static_cast<uint8_t>(ts - 3u);
        outDen = 8u;
        outTpb = static_cast<uint16_t>(outNum * 24u);
        return true;
    }
    if (ts <= 27u) {
        outNum = static_cast<uint8_t>(ts - 11u);
        outDen = 16u;
        outTpb = static_cast<uint16_t>(outNum * 12u);
        return true;
    }
    return false;
}

void unpackSingleVoice95(uint8_t index, const uint8_t* raw,
                         vf50::generated::FactoryVoice& v,
                         std::array<char, 16>& nameDst) noexcept {
    static constexpr std::array<std::size_t, 4> kVmemOpSlots = { 3u, 1u, 2u, 0u };
    v.index = index;
    std::memcpy(v.rawRomBytes.data(), raw, 95u);

    for (std::size_t logOp = 0; logOp < 4u; ++logOp) {
        const std::size_t slot = kVmemOpSlots[logOp];
        const uint8_t* opB = raw + slot * 10u;
        auto& o = v.operators[logOp];

        o.ar  = opB[0] & 0x1Fu;
        o.d1r = opB[1] & 0x1Fu;
        o.d2r = opB[2] & 0x1Fu;
        o.rr  = std::max<uint8_t>(1u, static_cast<uint8_t>(opB[3] & 0x0Fu));
        o.d1l = opB[4] & 0x0Fu;

        const int lsMag     = std::min<int>(opB[5] & 0x7Fu, 99);
        o.ampModEnable      = (opB[6] >> 6u) & 0x01u;
        o.egBiasSens        = (opB[6] >> 3u) & 0x07u;
        const int kvsMag    = opB[6] & 0x07u;
        o.outputLevel       = std::min<uint8_t>(opB[7] & 0x7Fu, 99u);
        o.coarse            = opB[8] & 0x3Fu;

        const bool lsNeg    = ((opB[9] >> 6u) & 0x01u) != 0u;
        const bool kvsNeg   = ((opB[9] >> 5u) & 0x01u) != 0u;
        o.rateScaling       = (opB[9] >> 3u) & 0x03u;
        const int detRaw    = std::clamp<int>(opB[9] & 0x07u, 0, 6);

        const uint8_t acedEven = raw[73u + slot * 2u];
        const uint8_t acedOdd  = raw[74u + slot * 2u];
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

    v.lfoSync        = (raw[40] >> 6u) & 0x01u;
    v.feedback       = (raw[40] >> 3u) & 0x07u;
    v.algorithm      = raw[40] & 0x07u;
    v.lfoSpeed       = std::min<uint8_t>(raw[41] & 0x7Fu, 99u);
    v.lfoDelay       = std::min<uint8_t>(raw[42] & 0x7Fu, 99u);
    v.pmd            = std::min<uint8_t>(raw[43] & 0x7Fu, 99u);
    v.amd            = std::min<uint8_t>(raw[44] & 0x7Fu, 99u);
    v.pms            = (raw[45] >> 4u) & 0x07u;
    v.ams            = (raw[45] >> 2u) & 0x03u;
    v.lfoWave        = raw[45] & 0x03u;
    v.transpose      = static_cast<int8_t>(std::clamp<int>(static_cast<int>(raw[46] & 0x3Fu) - 24, -24, 24));
    v.pitchBendRange = std::min<uint8_t>(raw[47] & 0x0Fu, 12u);
    v.chorusSwitch   = (raw[48] >> 4u) & 0x01u;
    v.monoMode       = (raw[48] >> 3u) & 0x01u;
    v.sustainSwitch  = (raw[48] >> 2u) & 0x01u;
    v.portaSwitch    = (raw[48] >> 1u) & 0x01u;
    v.portaMode      = raw[48] & 0x01u;
    v.portaTime      = std::min<uint8_t>(raw[49] & 0x7Fu, 99u);
    v.fcVolume       = std::min<uint8_t>(raw[50] & 0x7Fu, 99u);
    v.mwPitch        = std::min<uint8_t>(raw[51] & 0x7Fu, 99u);
    v.mwAmp          = std::min<uint8_t>(raw[52] & 0x7Fu, 99u);
    v.bcPitch        = std::min<uint8_t>(raw[53] & 0x7Fu, 99u);
    v.bcAmp          = std::min<uint8_t>(raw[54] & 0x7Fu, 99u);
    v.bcPitchBias    = static_cast<int8_t>(std::clamp<int>(static_cast<int>(raw[55] & 0x7Fu) - 50, -50, 50));
    v.bcEgBias       = std::min<uint8_t>(raw[56] & 0x7Fu, 99u);

    storeFixedName(nameDst, raw + 57u, 10u, false);

    v.peg.pr1 = std::min<uint8_t>(raw[67] & 0x7Fu, 99u);
    v.peg.pr2 = std::min<uint8_t>(raw[68] & 0x7Fu, 99u);
    v.peg.pr3 = std::min<uint8_t>(raw[69] & 0x7Fu, 99u);
    v.peg.pl1 = std::min<uint8_t>(raw[70] & 0x7Fu, 99u);
    v.peg.pl2 = std::min<uint8_t>(raw[71] & 0x7Fu, 99u);
    v.peg.pl3 = std::min<uint8_t>(raw[72] & 0x7Fu, 99u);

    v.reverbRate  = raw[81] & 0x07u;
    v.fcPitch     = std::min<uint8_t>(raw[82] & 0x7Fu, 99u);
    v.fcAmp       = std::min<uint8_t>(raw[83] & 0x7Fu, 99u);
    v.atPitch     = std::min<uint8_t>(raw[84] & 0x7Fu, 99u);
    v.atAmp       = std::min<uint8_t>(raw[85] & 0x7Fu, 99u);
    v.atPitchBias = static_cast<int8_t>(std::clamp<int>(static_cast<int>(raw[86] & 0x7Fu) - 50, -50, 50));
    v.atEgBias    = std::min<uint8_t>(raw[87] & 0x7Fu, 99u);

    v.effect.effectType = std::min<uint8_t>(raw[88] & 0x3Fu, 32u);
    v.effect.balance    = std::min<uint8_t>(raw[89] & 0x7Fu, 100u);
    v.effect.outLevel   = std::min<uint8_t>(raw[90] & 0x7Fu, 100u);
    v.effect.stereoMix  = raw[91] & 0x01u;
    v.effect.param1     = std::min<uint8_t>(raw[92] & 0x7Fu, 75u);
    v.effect.param2     = std::min<uint8_t>(raw[93] & 0x7Fu, 99u);
    v.effect.param3     = std::min<uint8_t>(raw[94] & 0x7Fu, 99u);
}

void unpackPerformance97(uint8_t index, const uint8_t* raw,
                         vf50::generated::FactoryPerformance& p,
                         std::array<char, 16>& nameDst) noexcept {
    p.index = index;
    std::memcpy(p.rawRomBytes.data(), raw, 97u);
    storeFixedName(nameDst, raw + 66u, 10u, false);

    p.microTuneTable   = std::min<uint8_t>(raw[64] & 0x0Fu, 12u);
    const uint8_t efs2 = (raw[64] >> 4u) & 0x03u;
    const uint8_t efs1 = (raw[65] >> 1u) & 0x03u;
    p.perfEffectSelect = (efs1 != 0u) ? static_cast<uint8_t>(std::min<int>(efs2 * 3 + efs1, 12)) : 0u;
    p.microTuneRootKey = std::min<uint8_t>((raw[65] >> 3u) & 0x0Fu, 11u);
    const uint8_t asm1 = raw[65] & 0x01u;
    const uint8_t asm2 = raw[92] & 0x01u;
    p.assignMode       = std::min<uint8_t>(static_cast<uint8_t>((asm2 << 1u) | asm1), 2u);

    for (std::size_t i = 0; i < 8u; ++i) {
        const uint8_t* ib     = raw + i * 8u;
        const uint8_t max1    = ib[0] & 0x0Fu;
        const uint8_t pairB   = raw[93u + (i / 2u)];
        const uint8_t max2    = ((i % 2u) == 0u) ? ((pairB >> 2u) & 0x01u) : (pairB & 0x01u);

        auto& inst = p.instruments[i];
        inst.maxNotes        = std::min<uint8_t>(static_cast<uint8_t>((max2 << 4u) | max1), 16u);
        inst.voiceBank       = (ib[0] >> 4u) & 0x01u;
        inst.outAssign       = (ib[0] >> 5u) & 0x03u;
        inst.voiceNumber     = std::min<uint8_t>(ib[1] & 0x7Fu, 99u);
        inst.receiveChannel  = std::min<uint8_t>(ib[2] & 0x1Fu, 16u);
        inst.lfoSelect       = (ib[2] >> 5u) & 0x03u;
        inst.limitLow        = ib[3] & 0x7Fu;
        inst.limitHigh       = ib[4] & 0x7Fu;
        inst.detune          = static_cast<int8_t>(std::clamp<int>(static_cast<int>(ib[5] & 0x0Fu) - 7, -7, 7));
        inst.noteShift       = static_cast<int8_t>(std::clamp<int>(static_cast<int>(ib[6] & 0x3Fu) - 24, -24, 24));
        inst.microTuneEnable = (ib[6] >> 6u) & 0x01u;
        inst.volume          = std::min<uint8_t>(ib[7] & 0x7Fu, 99u);

        const uint8_t pm2 = raw[76u + i];
        inst.reserveNotes = std::min<uint8_t>(pm2 & 0x1Fu, 17u);
        inst.effectEnable = (pm2 >> 6u) & 0x01u;
    }

    p.effect.effectType = std::min<uint8_t>(raw[84] & 0x3Fu, 32u);
    p.effect.balance    = std::min<uint8_t>(raw[85] & 0x7Fu, 100u);
    p.effect.outLevel   = std::min<uint8_t>(raw[86] & 0x7Fu, 100u);
    p.effect.stereoMix  = raw[87] & 0x01u;
    p.effect.param1     = std::min<uint8_t>(raw[88] & 0x7Fu, 75u);
    p.effect.param2     = std::min<uint8_t>(raw[89] & 0x7Fu, 99u);
    p.effect.param3     = std::min<uint8_t>(raw[90] & 0x7Fu, 99u);
}

void expandMicrotuning128(std::size_t scaleIdx,
                          const uint8_t* base24,
                          vf50::generated::MicrotuningTable& dst) noexcept {
    for (std::size_t i = 0; i < 12u; ++i) {
        dst.baseOctave[i] = { base24[i * 2u], base24[i * 2u + 1u] };
        dst.keys[60u + i] = dst.baseOctave[i];
    }

    if (scaleIdx == 9u || scaleIdx == 10u) {
        const uint8_t step = (scaleIdx == 9u) ? 6u : 3u; // Quarter Tone / Eighth Tone
        for (std::size_t i = 0; i < 12u; ++i) {
            dst.keys[48u + i] = dst.keys[60u + i];
        }
        for (int b = 0x30; b < 0x74; ++b) {
            uint8_t c = dst.keys[static_cast<std::size_t>(b)].coarseNote;
            const uint8_t f = dst.keys[static_cast<std::size_t>(b)].fineStep64;
            c = static_cast<uint8_t>(c + step);
            if (c > 0x6Bu) {
                c = static_cast<uint8_t>(c - 12u);
            }
            dst.keys[static_cast<std::size_t>(b + 12)] = { c, f };
        }
        for (int b = 0x2F; b >= 0; --b) {
            uint8_t c = dst.keys[static_cast<std::size_t>(b + 12)].coarseNote;
            const uint8_t f = dst.keys[static_cast<std::size_t>(b + 12)].fineStep64;
            c = (c >= step) ? static_cast<uint8_t>(c - step) : 0u;
            if (c < 0x0Du) {
                c = static_cast<uint8_t>(c + 12u);
            }
            dst.keys[static_cast<std::size_t>(b)] = { c, f };
        }
    } else {
        for (int b = 0x3C; b < 0x74; ++b) {
            uint8_t c = dst.keys[static_cast<std::size_t>(b)].coarseNote;
            const uint8_t f = dst.keys[static_cast<std::size_t>(b)].fineStep64;
            c = static_cast<uint8_t>(c + 12u);
            if (c > 0x6Bu) {
                c = static_cast<uint8_t>(c - 12u);
            }
            dst.keys[static_cast<std::size_t>(b + 12)] = { c, f };
        }
        for (int b = 0x3B; b >= 0; --b) {
            uint8_t c = dst.keys[static_cast<std::size_t>(b + 12)].coarseNote;
            const uint8_t f = dst.keys[static_cast<std::size_t>(b + 12)].fineStep64;
            c = (c >= 12u) ? static_cast<uint8_t>(c - 12u) : 0u;
            if (c < 0x0Du) {
                c = static_cast<uint8_t>(c + 12u);
            }
            dst.keys[static_cast<std::size_t>(b)] = { c, f };
        }
    }
}

struct DecodedPatternResult {
    uint8_t  timeSigCode = 3u;
    uint8_t  timeSigNum  = 4u;
    uint8_t  timeSigDen  = 4u;
    uint8_t  bars        = 1u;
    uint16_t ticksPerBar = 192u;
    uint16_t totalTicks  = 192u;
    std::vector<vf50::generated::RhythmNoteEvent> events;
};

[[nodiscard]] DecodedPatternResult decodeRhythmPatternStream(const uint8_t* raw,
                                                             std::size_t rawLen,
                                                             uint8_t tsCode,
                                                             uint8_t barCode) {
    DecodedPatternResult out{};
    out.timeSigCode = tsCode;
    if (!ticksPerBarFromTs(tsCode, out.timeSigNum, out.timeSigDen, out.ticksPerBar)) {
        out.timeSigNum  = 4u;
        out.timeSigDen  = 4u;
        out.ticksPerBar = 192u;
    }
    out.bars       = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(barCode) + 1, 1, 16));
    out.totalTicks = static_cast<uint16_t>(out.ticksPerBar * out.bars);

    uint32_t tick = 0u;
    std::size_t pos = 0u;
    while (pos < rawLen && tick < out.totalTicks) {
        const uint8_t b = raw[pos++];
        if (b >= 0x80u) {
            tick += (b & 0x7Fu);
        } else if (b <= 0x3Fu) {
            if (b < 61u) {
                out.events.push_back({ static_cast<uint16_t>(tick), b, 5u, 0u });
            }
        } else {
            if (pos >= rawLen) {
                break;
            }
            const uint8_t drum  = b & 0x3Fu;
            const uint8_t param = raw[pos++];
            // Verified HD6303 IC723 @0x0F0AA accent decoding: bit 5 negative sign-extension, else low 3 bits
            const int offset = ((param & 0x20u) != 0u)
                                   ? (static_cast<int>(param | 0x80u) - 256)
                                   : static_cast<int>(param & 0x07u);
            const int hwAccent = std::clamp(5 + offset, 0, 7);
            const auto velLevel = static_cast<uint8_t>(std::clamp(hwAccent, 1, 8));
            if (drum < 61u) {
                out.events.push_back({ static_cast<uint16_t>(tick), drum, velLevel, param });
            }
        }
    }
    return out;
}

struct PrioritizedDemoEvent {
    uint32_t tick     = 0u;
    uint8_t  prio     = 2u;
    vf50::generated::DemoEventType type = vf50::generated::DemoEventType::FmNoteOn;
    uint8_t  channel  = 0u;
    uint8_t  data1    = 0u;
    uint8_t  data2    = 0u;
};

struct DecodedRhySong {
    std::array<char, 8> name{};
    uint32_t totalTicks = 0u;
    std::vector<PrioritizedDemoEvent> events;
};

void decodeIc712(const std::vector<uint8_t>& romA, VF50RomImage& img) {
    img.hasMainRomA = true;
    const uint8_t* data = romA.data();
    const std::size_t sz = romA.size();

    // Firmware Version Stamp around "Ver."
    const std::string_view needle = "Ver.";
    const auto* found = std::search(data, data + sz, needle.begin(), needle.end());
    if (found != data + sz) {
        const std::size_t off = static_cast<std::size_t>(found - data);
        const std::size_t start = (off >= 14u) ? (off - 14u) : 0u;
        const std::size_t len   = std::min<std::size_t>(26u, sz - start);
        copyCString(img.firmwareVersion.data(), img.firmwareVersion.size(),
                    std::string_view(reinterpret_cast<const char*>(data + start), len));
    }

    // Coarse Frequency Permutation Table (64 bytes @ 0x0ECEC)
    if (0x0ECECu + 64u <= sz) {
        std::memcpy(img.coarsePermTable.data(), data + 0x0ECECu, 64u);
    }

    // 33 YM3413 DSP Effect LCD Names (33 x 16 bytes @ 0x0CD61)
    if (0x0CD61u + 33u * 16u <= sz) {
        for (std::size_t i = 0; i < 33u; ++i) {
            storeFixedName(img.dspEffectNameStorage[i], data + 0x0CD61u + i * 16u, 16u, true);
        }
    }

    // 100 Factory Single Voices (100 x 95 bytes @ 0x15A00)
    if (0x15A00u + 100u * 95u <= sz) {
        for (std::size_t i = 0; i < 100u; ++i) {
            unpackSingleVoice95(static_cast<uint8_t>(i),
                                data + 0x15A00u + i * 95u,
                                img.factoryVoices[i],
                                img.voiceNames[i]);
        }
    }

    // 100 Factory Performances (100 x 97 bytes @ 0x19A00)
    if (0x19A00u + 100u * 97u <= sz) {
        for (std::size_t i = 0; i < 100u; ++i) {
            unpackPerformance97(static_cast<uint8_t>(i),
                                data + 0x19A00u + i * 97u,
                                img.factoryPerformances[i],
                                img.perfNames[i]);
        }
    }

    // 11 Factory Microtuning Tables (Names @ 0x0575F, Base24 @ 0x144FD)
    if (0x0575Fu + 11u * 14u <= sz && 0x144FDu + 11u * 24u <= sz) {
        for (std::size_t i = 0; i < 11u; ++i) {
            img.factoryMicrotunings[i].index = static_cast<uint8_t>(i);
            storeFixedName(img.microNames[i], data + 0x0575Fu + i * 14u, 14u, true);
            expandMicrotuning128(i, data + 0x144FDu + i * 24u, img.factoryMicrotunings[i]);
        }
    }
}

void decodeIc723(const std::vector<uint8_t>& romB, VF50RomImage& img) {
    img.hasMainRomB = true;
    const uint8_t* data = romB.data();
    const std::size_t sz = romB.size();

    // 61 Drum Names (6 bytes each @ 0x11AB5) + Choke Groups (@ 0x0FDAC) + 3 Preset Key Maps (@ 0x131E2)
    if (0x11AB5u + 61u * 6u <= sz) {
        for (std::size_t i = 0; i < 61u; ++i) {
            const uint8_t* nm = data + 0x11AB5u + i * 6u;
            storeFixedName(img.drumSetupNameStorage[i], nm, 6u, true);
            storeFixedName(img.drumWaveNameStorage[i],  nm, 6u, true);
            storeFixedName(img.drumLcdNameStorage[i],   nm, 6u, false);
        }
    }
    if (0x0FDACu + 61u <= sz) {
        for (std::size_t i = 0; i < 61u; ++i) {
            img.defaultDrumSetup[i].chokeGroup = data[0x0FDACu + i];
        }
    }
    if (0x131E2u + 3u * 61u <= sz) {
        for (std::size_t m = 0; m < 3u; ++m) {
            for (std::size_t k = 0; k < 61u; ++k) {
                const uint8_t b = data[0x131E2u + m * 61u + k];
                img.presetDrumKeyMaps[m][k] = (b == 0xFFu) ? int8_t{ -1 } : static_cast<int8_t>(b);
            }
        }
    }

    // Default Drum Volumes / Pans / MIDI Note Assignments (0x13680..0x137DF)
    if (0x137DFu <= sz) {
        const uint8_t* rinit = data + 0x13680u;
        const std::size_t rlen = 0x137DFu - 0x13680u;
        std::array<uint8_t, 61> fifteenRun{};
        fifteenRun.fill(0x0Fu);
        const auto* hit = std::search(rinit, rinit + rlen, fifteenRun.begin(), fifteenRun.end());
        if (hit != rinit + rlen) {
            const std::size_t volOff = static_cast<std::size_t>(hit - rinit);
            if (volOff + 183u <= rlen) {
                for (std::size_t i = 0; i < 61u; ++i) {
                    img.defaultDrumSetup[i].defaultVolume = std::min<uint8_t>(rinit[volOff + i], 15u);
                    const int panRaw = std::clamp<int>(static_cast<int>(rinit[volOff + 61u + i]), 0, 6);
                    img.defaultDrumSetup[i].defaultPan = static_cast<int8_t>(panRaw - 3);
                    img.defaultDrumSetup[i].defaultMidiNote = rinit[volOff + 122u + i] & 0x7Fu;
                }
                // Apply Preset Key Map 0 (RKAT 1: G0..G5) cleanly onto defaults
                for (std::size_t keyOff = 0; keyOff < 61u; ++keyOff) {
                    const int8_t dId = img.presetDrumKeyMaps[0][keyOff];
                    if (dId >= 0 && dId < 61) {
                        img.defaultDrumSetup[static_cast<std::size_t>(dId)].defaultMidiNote =
                            static_cast<uint8_t>(31u + keyOff);
                    }
                }
            }
        }
    }

    // 100 Factory Rhythm Patterns (IC723 + 0x11F88..0x13105)
    std::array<DecodedPatternResult, 100> presetPatterns;
    if (0x1211Au <= sz) {
        const uint16_t streamLen = readBE16(data + 0x12050u);
        if (streamLen > 0u && 0x1211Au + streamLen <= sz) {
            img.rhythmEventStream.assign(data + 0x1211Au, data + 0x1211Au + streamLen);
            img.decodedRhythmEvents.clear();

            std::array<uint16_t, 100> ptrs{};
            for (std::size_t i = 0; i < 100u; ++i) {
                ptrs[i] = static_cast<uint16_t>(readBE16(data + 0x11F88u + i * 2u) & 0x7FFFu);
            }

            for (std::size_t i = 0; i < 100u; ++i) {
                const uint16_t startOff = ptrs[i];
                const uint16_t endOff   = (i < 99u) ? ptrs[i + 1u] : streamLen;
                if (startOff < endOff && endOff <= streamLen) {
                    const uint8_t tsCode  = data[0x12052u + i];
                    const uint8_t barCode = data[0x120B6u + i];
                    auto dec = decodeRhythmPatternStream(img.rhythmEventStream.data() + startOff,
                                                         static_cast<std::size_t>(endOff - startOff),
                                                         tsCode, barCode);
                    auto& dst = img.factoryRhythmPatterns[i];
                    dst.index       = static_cast<uint8_t>(i);
                    dst.timeSigCode = dec.timeSigCode;
                    dst.timeSigNum  = dec.timeSigNum;
                    dst.timeSigDen  = dec.timeSigDen;
                    dst.bars        = dec.bars;
                    dst.ticksPerBar = dec.ticksPerBar;
                    dst.totalTicks  = dec.totalTicks;
                    dst.rawOffset   = startOff;
                    dst.rawLength   = static_cast<uint16_t>(endOff - startOff);
                    dst.eventOffset = static_cast<uint16_t>(img.decodedRhythmEvents.size());
                    dst.eventCount  = static_cast<uint16_t>(dec.events.size());
                    img.decodedRhythmEvents.insert(img.decodedRhythmEvents.end(),
                                                   dec.events.begin(), dec.events.end());
                    presetPatterns[i] = std::move(dec);
                }
            }
        }
    }

    // Demo Rhythm Songs & 3 Built-In HardwareSeqV1 Factory Demo Songs (Victory!, Route 50, MoonLite)
    std::map<std::size_t, DecodedRhySong> rhySongsByIdx;
    if (0x13F88u <= sz) {
        const uint8_t* ublk = data + 0x137C0u;
        const std::size_t ublkSize = 0x13F88u - 0x137C0u;
        std::array<uint16_t, 100> uPtrs{};
        for (std::size_t i = 0; i < 100u; ++i) {
            uPtrs[i] = readBE16(ublk + i * 2u);
        }
        std::array<uint16_t, 8> songPtrs{};
        for (std::size_t i = 0; i < 8u; ++i) {
            songPtrs[i] = readBE16(ublk + 0xC8u + i * 2u);
        }
        const uint16_t rhyStreamEnd = readBE16(ublk + 0xD8u);
        const std::size_t uStreamMax = ublkSize - 0x1E2u;
        const uint8_t* uStream = ublk + 0x1E2u;

        std::vector<std::size_t> populatedPtns;
        for (std::size_t i = 0; i < 100u; ++i) {
            if ((uPtrs[i] & 0x8000u) != 0u) {
                populatedPtns.push_back(i);
            }
        }
        const uint16_t firstSongOff = static_cast<uint16_t>(songPtrs[0] & 0x7FFFu);

        std::map<std::size_t, DecodedPatternResult> userPtns;
        for (std::size_t pos = 0; pos < populatedPtns.size(); ++pos) {
            const std::size_t pIdx = populatedPtns[pos];
            const uint16_t sOff = static_cast<uint16_t>(uPtrs[pIdx] & 0x7FFFu);
            const uint16_t eOff = (pos + 1u < populatedPtns.size())
                                      ? static_cast<uint16_t>(uPtrs[populatedPtns[pos + 1u]] & 0x7FFFu)
                                      : firstSongOff;
            if (sOff < eOff && eOff <= uStreamMax) {
                userPtns[pIdx] = decodeRhythmPatternStream(uStream + sOff, eOff - sOff,
                                                           ublk[0xDAu + pIdx], ublk[0x13Eu + pIdx]);
            }
        }

        std::vector<std::size_t> populatedSongs;
        for (std::size_t i = 0; i < 8u; ++i) {
            if ((songPtrs[i] & 0x8000u) != 0u) {
                populatedSongs.push_back(i);
            }
        }
        for (std::size_t sPos = 0; sPos < populatedSongs.size(); ++sPos) {
            const std::size_t sIdx = populatedSongs[sPos];
            const uint16_t sOff = static_cast<uint16_t>(songPtrs[sIdx] & 0x7FFFu);
            const uint16_t eOff = (sPos + 1u < populatedSongs.size())
                                      ? static_cast<uint16_t>(songPtrs[populatedSongs[sPos + 1u]] & 0x7FFFu)
                                      : rhyStreamEnd;
            if (sOff >= eOff || eOff > uStreamMax) {
                continue;
            }
            DecodedRhySong rSong{};
            std::memcpy(rSong.name.data(), ublk + 0x1A2u + sIdx * 8u, 8u);

            std::size_t pos = sOff;
            struct RepeatFrame {
                std::size_t startPos = 0u;
                std::size_t endPos   = SIZE_MAX;
                int         remaining = 0;
            };
            std::vector<RepeatFrame> repeatStack;
            uint32_t curTick = 0u;
            std::size_t safetySteps = 0u;

            // Exact HD6303 IC723 $A824 length table = [1, 1, 3, 3, 4, 3]:
            //   0x00..0x63 (1B) = I00..I99, 0x64..0xC7 (1B) = P00..P99,
            //   0xC8 (1B) = ||:, 0xC9 <N> 0xC9 (3B) = :|| x (N+1),
            //   0xCA <d> 0xCA (3B) = Vol, 0xCB <d> <beats> 0xCB (4B) = Tempo,
            //   0xCC <m> 0xCC (3B) = Mark
            while (pos < eOff && safetySteps++ < 4096u) {
                const uint8_t cmd = uStream[pos];
                if (cmd <= 0x63u) {
                    const auto it = userPtns.find(cmd);
                    if (it != userPtns.end()) {
                        for (const auto& ev : it->second.events) {
                            rSong.events.push_back({ curTick + ev.tick, 3u,
                                                     vf50::generated::DemoEventType::DrumHit,
                                                     9u, ev.drumIndex, ev.velocityLevel });
                        }
                        curTick += it->second.totalTicks;
                    }
                    ++pos;
                } else if (cmd <= 0xC7u) {
                    const auto& ptn = presetPatterns[cmd - 0x64u];
                    for (const auto& ev : ptn.events) {
                        rSong.events.push_back({ curTick + ev.tick, 3u,
                                                 vf50::generated::DemoEventType::DrumHit,
                                                 9u, ev.drumIndex, ev.velocityLevel });
                    }
                    curTick += ptn.totalTicks;
                    ++pos;
                } else if (cmd == 0xC8u) {
                    if (repeatStack.size() < 16u) {
                        repeatStack.push_back({ pos + 1u, SIZE_MAX, 0 });
                    }
                    ++pos;
                } else if (cmd == 0xC9u) {
                    const int extraRepeats = (pos + 1u < eOff) ? static_cast<int>(uStream[pos + 1u] & 0x7Fu) : 0;
                    const std::size_t cmdLen = (pos + 2u < eOff && uStream[pos + 2u] == 0xC9u) ? 3u : 2u;
                    if (!repeatStack.empty()) {
                        auto& top = repeatStack.back();
                        if (top.endPos != pos) {
                            top.endPos    = pos;
                            top.remaining = extraRepeats;
                        }
                        if (top.remaining > 0) {
                            --top.remaining;
                            pos = top.startPos;
                        } else {
                            repeatStack.pop_back();
                            pos += cmdLen;
                        }
                    } else {
                        pos += cmdLen;
                    }
                } else if (cmd == 0xCAu) {
                    if (pos + 1u < eOff) {
                        rSong.events.push_back({ curTick, 2u,
                                                 vf50::generated::DemoEventType::RhythmVolDelta,
                                                 9u, uStream[pos + 1u], 0u });
                    }
                    pos += (pos + 2u < eOff) ? 3u : 2u;
                } else if (cmd == 0xCBu) {
                    if (pos + 2u < eOff) {
                        rSong.events.push_back({ curTick, 2u,
                                                 vf50::generated::DemoEventType::TempoChange,
                                                 0u, uStream[pos + 1u], uStream[pos + 2u] });
                    }
                    pos += (pos + 3u < eOff && uStream[pos + 3u] == 0xCBu) ? 4u : 3u;
                } else if (cmd == 0xCCu) {
                    pos += (pos + 2u < eOff) ? 3u : 2u;
                } else {
                    ++pos;
                }
            }
            rSong.totalTicks = curTick;
            rhySongsByIdx[sIdx] = std::move(rSong);
        }
    }

    if (0x1C000u <= sz) {
        std::vector<std::vector<uint8_t>> rawSongSlices(3u);
        rawSongSlices[0].assign(data + 0x18000u, data + 0x190DCu);
        rawSongSlices[1].assign(data + 0x190DCu, data + 0x1AAACu);
        rawSongSlices[2].assign(data + 0x1AAACu, data + 0x1C000u);
        rawSongSlices[2].insert(rawSongSlices[2].end(), data + 0x16000u, data + 0x18000u);

        img.demoSongEvents.clear();
        for (std::size_t sIdx = 0; sIdx < 3u; ++sIdx) {
            const auto& buf = rawSongSlices[sIdx];
            if (buf.size() < 35u || !matchesFnv1a64(buf.data(), buf.size(), 0u, 8u, 0xd485378e87ae2802ULL)) {
                continue;
            }

            auto& songMeta = img.factoryDemoSongs[sIdx];
            songMeta.index = static_cast<uint8_t>(sIdx);
            (void)ticksPerBarFromTs(buf[9], songMeta.timeSigNum, songMeta.timeSigDen, songMeta.ticksPerBar);
            songMeta.initialTempoBpm = std::clamp<uint8_t>(buf[11], 30u, 240u);
            storeFixedName(img.demoTitleStorage[sIdx], buf.data() + 12u, 8u, true);
            for (std::size_t t = 0; t < 8u; ++t) {
                songMeta.trackChannels[t] = buf[20u + t] & 0x0Fu;
            }
            songMeta.perfPresetBank  = (buf[29] >= 2u) ? 1u : 0u;
            songMeta.perfNumber      = std::min<uint8_t>(buf[30] & 0x7Fu, 99u);
            const bool rhythmSync    = (buf[31] & 0x01u) != 0u;
            songMeta.rhythmSongIndex = buf[32] & 0x07u;

            std::size_t pos   = 34u;
            uint8_t curTrk    = 0u;
            uint8_t curCh     = 0u;
            uint32_t curTick  = 0u;
            uint32_t maxTick  = 0u;
            uint16_t maxBars  = 0u;
            uint16_t curBars  = 0u;
            uint8_t pendBank  = 1u;

            std::vector<PrioritizedDemoEvent> merged;
            merged.reserve(4096u);

            while (pos < buf.size()) {
                const uint8_t b = buf[pos++];
                if (b == 0xF9u) {
                    break;
                }
                if (b == 0x00u) {
                    continue;
                }
                if (b >= 0x01u && b <= 0x7Fu) {
                    uint32_t dt = 0u;
                    if (pos < buf.size() && buf[pos] <= 0x7Fu) {
                        const uint8_t b2 = buf[pos++];
                        dt = ((static_cast<uint32_t>(b) << 7u) | static_cast<uint32_t>(b2)) >> 1u;
                    } else {
                        dt = (b == 0x01u) ? 0u : static_cast<uint32_t>(b >> 1u);
                    }
                    curTick += dt;
                    maxTick = std::max(maxTick, curTick);
                } else if (b == 0xF0u) {
                    if (pos < buf.size()) {
                        curTrk   = buf[pos++] & 0x07u;
                        curCh    = songMeta.trackChannels[curTrk];
                        curTick  = 0u;
                        curBars  = 0u;
                        pendBank = 1u;
                    }
                } else if (b == 0xF2u) {
                    maxTick = std::max(maxTick, curTick);
                    maxBars = std::max(maxBars, curBars);
                } else if (b == 0xF5u) {
                    ++curBars;
                } else if (b >= 0x80u && b <= 0xDFu) {
                    uint32_t gate = 0u;
                    if (b <= 0xBFu) {
                        gate = b & 0x3Fu;
                    } else if (pos < buf.size()) {
                        const uint8_t b2 = buf[pos++];
                        gate = (static_cast<uint32_t>(b & 0x3Fu) << 7u) | static_cast<uint32_t>(b2 & 0x7Fu);
                    }
                    if (pos >= buf.size()) break;
                    const uint8_t nb   = buf[pos++];
                    const uint8_t note = nb & 0x7Fu;
                    uint8_t vel        = 64u;
                    if ((nb & 0x80u) == 0u) {
                        if (pos >= buf.size()) break;
                        vel = buf[pos++] & 0x7Fu;
                    }
                    const uint32_t effGate = std::max<uint32_t>(1u, gate);
                    if (vel > 0u) {
                        merged.push_back({ curTick, 3u, vf50::generated::DemoEventType::FmNoteOn, curCh, note, vel });
                        merged.push_back({ curTick + effGate, 0u, vf50::generated::DemoEventType::FmNoteOff, curCh, note, 0u });
                        maxTick = std::max(maxTick, curTick + effGate);
                    }
                } else if (b >= 0xE0u && b <= 0xEFu) {
                    continue;
                } else if (b == 0xFAu) {
                    if (pos + 1u < buf.size()) {
                        const uint8_t val = buf[pos + 1u] & 0x7Fu;
                        merged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmAftertouch, curCh, val, 0u });
                        pos += 2u;
                    } else {
                        break;
                    }
                } else if (b == 0xFBu) {
                    if (pos + 1u < buf.size()) {
                        const uint8_t cc  = buf[pos] & 0x7Fu;
                        const uint8_t val = buf[pos + 1u] & 0x7Fu;
                        merged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmControl, curCh, cc, val });
                        pos += 2u;
                    } else {
                        break;
                    }
                } else if (b == 0xFCu) {
                    if (pos < buf.size()) {
                        const uint8_t prog = buf[pos++] & 0x7Fu;
                        if (prog == 120u) {
                            pendBank = 0u;
                        } else if (prog == 121u) {
                            pendBank = 1u;
                        } else if (prog < 100u) {
                            merged.push_back({ curTick, 1u, vf50::generated::DemoEventType::FmProgram, curCh, pendBank, prog });
                        }
                    }
                } else if (b == 0xFDu) {
                    if (pos < buf.size()) {
                        const uint8_t val = buf[pos++] & 0x7Fu;
                        merged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmAftertouch, curCh, val, 0u });
                    }
                } else if (b == 0xFEu) {
                    if (pos + 1u < buf.size()) {
                        const uint8_t lsb = buf[pos] & 0x7Fu;
                        const uint8_t msb = buf[pos + 1u] & 0x7Fu;
                        merged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmPitchBend, curCh, lsb, msb });
                        pos += 2u;
                    } else {
                        break;
                    }
                } else if (b == 0xF6u) {
                    if (pos + 1u < buf.size()) {
                        const uint8_t b2 = buf[pos + 1u];
                        if (b2 >= 30u && b2 <= 240u) {
                            merged.push_back({ curTick, 2u, vf50::generated::DemoEventType::TempoChange, 0u, 0u, b2 });
                        }
                        pos += 2u;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }

            if (rhythmSync) {
                const auto itRhy = rhySongsByIdx.find(songMeta.rhythmSongIndex);
                if (itRhy != rhySongsByIdx.end()) {
                    for (const auto& rev : itRhy->second.events) {
                        merged.push_back(rev);
                    }
                    maxTick = std::max(maxTick, itRhy->second.totalTicks);
                }
            }

            std::stable_sort(merged.begin(), merged.end(), [](const PrioritizedDemoEvent& a, const PrioritizedDemoEvent& b) {
                if (a.tick != b.tick) return a.tick < b.tick;
                return a.prio < b.prio;
            });

            songMeta.totalBars   = std::max<uint16_t>(1u, maxBars);
            songMeta.totalTicks  = std::max<uint32_t>(1u, maxTick);
            songMeta.eventOffset = static_cast<uint32_t>(img.demoSongEvents.size());
            songMeta.eventCount  = static_cast<uint32_t>(merged.size());
            for (const auto& me : merged) {
                img.demoSongEvents.push_back({ me.tick, me.type, me.channel, me.data1, me.data2 });
            }
        }
    }
}

void decodeIc731(const std::vector<uint8_t>& romSub, VF50RomImage& img) {
    img.hasSubCpuRom = true;
    const uint8_t* data = romSub.data();
    const std::size_t sz = romSub.size();

    if (0x2A87u + 20u <= sz) {
        std::memcpy(img.lowOutLevelToTl.data(), data + 0x2A87u, 20u);
    }

    if (0x04AA0u + 32u * 20u <= sz) {
        for (std::size_t i = 0; i < 32u; ++i) {
            img.factoryDspPresets[i].effectNumber = static_cast<uint8_t>(i + 1u);
            std::memcpy(img.factoryDspPresets[i].subCpuParams.data(), data + 0x04AA0u + i * 20u, 20u);
        }
    }
}

bool decodeDrumWaveformsIc738(const std::vector<uint8_t>& romSub,
                             const std::vector<uint8_t>& romDrum,
                             VF50RomImage& img) {
    if (romSub.size() < 0x3346u + 64u || romDrum.size() < 524288u) {
        return false;
    }

    const uint8_t* gateTbl = romSub.data() + 0x32C6u;
    const uint8_t* reg3Tbl = romSub.data() + 0x3306u;
    const uint8_t* reg2Tbl = romSub.data() + 0x3346u;

    img.drumPcmPool.clear();
    img.drumPcmPool.reserve(510000u);

    for (std::size_t idx = 0; idx < vf50::generated::kNumDrumWaveforms; ++idx) {
        const uint8_t reg2   = reg2Tbl[idx];
        const uint8_t reg3   = reg3Tbl[idx];
        const uint8_t waveId = static_cast<uint8_t>((reg3 << 2u) | ((reg2 >> 3u) & 0x03u));
        const std::size_t dirOff = static_cast<std::size_t>(waveId) * 2u;
        if (dirOff + 2u > romDrum.size()) {
            return false;
        }
        const uint16_t romPage = readLE16(romDrum.data() + dirOff);
        const std::size_t base = static_cast<std::size_t>(romPage) << 8u;
        if (base + 0x40u > romDrum.size()) {
            return false;
        }
        const uint8_t* hdr = romDrum.data() + base;

        const uint32_t endSampleIdx  = (static_cast<uint32_t>(hdr[4]) << 8u) | static_cast<uint32_t>(hdr[2]);
        const uint32_t totalSamples  = endSampleIdx + 1u;
        const uint32_t loopSample    = (static_cast<uint32_t>(hdr[8]) << 8u) | static_cast<uint32_t>(hdr[6]);
        const uint32_t pcmSamples    = static_cast<uint32_t>(hdr[10]) * 256u;
        const uint8_t dpcmScaleCode  = hdr[12];
        const uint8_t dpcmShiftLeft  = static_cast<uint8_t>(3u - std::min<uint8_t>(3u, dpcmScaleCode >> 2u));

        const std::size_t dataOffset = base + 0x40u;
        if (dataOffset + totalSamples > romDrum.size()) {
            return false;
        }

        auto& desc = img.drumWaveforms[idx];
        desc.index            = static_cast<uint8_t>(idx);
        desc.oprwWaveId       = waveId;
        desc.romPage          = romPage;
        desc.romDataOffset    = static_cast<uint32_t>(dataOffset);
        desc.totalSamples     = totalSamples;
        desc.pcmAttackSamples = pcmSamples;
        desc.loopSampleIndex  = loopSample;
        desc.dpcmScaleCode    = dpcmScaleCode;
        desc.dpcmShiftLeft    = dpcmShiftLeft;
        desc.oprwReg2         = reg2;
        desc.oprwReg3         = reg3;
        desc.defaultGateTicks = gateTbl[idx];
        desc.levelAttenCode   = hdr[16];
        desc.attackRate       = hdr[17];
        desc.decay1Rate       = hdr[18];
        desc.decay1Level      = hdr[19];
        desc.decay2Rate       = hdr[20];
        desc.releaseRate      = hdr[22];
        std::memcpy(desc.rawHeader.data(), hdr, 26u);

        img.drumPcmOffsets[idx] = static_cast<uint32_t>(img.drumPcmPool.size());
        int32_t acc = 0;
        for (uint32_t n = 0; n < totalSamples; ++n) {
            const int8_t val = static_cast<int8_t>(romDrum[dataOffset + n]);
            if (n < pcmSamples) {
                acc = static_cast<int32_t>(val) << 4;
            } else {
                acc += static_cast<int32_t>(val) << dpcmShiftLeft;
            }
            acc = std::clamp(acc, -2048, 2047);
            img.drumPcmPool.push_back(static_cast<int16_t>(acc << 4));
        }
    }

    img.hasDrumWaveRom = true;
    return true;
}

} // namespace

const RomChipSpec& getRomChipSpec(RomChipId chip) noexcept {
    const auto idx = std::min<std::size_t>(static_cast<std::size_t>(chip), kRomChipSpecs.size() - 1u);
    return kRomChipSpecs[idx];
}

void VF50RomSetBuilder::clear() noexcept {
    for (auto& buf : rawBytes_) {
        buf.clear();
    }
    for (auto& f : sourceFiles_) {
        f = juce::File{};
    }
}

void VF50RomSetBuilder::clearChip(RomChipId chip) noexcept {
    const auto idx = static_cast<std::size_t>(chip);
    if (idx < rawBytes_.size()) {
        rawBytes_[idx].clear();
        sourceFiles_[idx] = juce::File{};
    }
}

std::optional<RomChipId> VF50RomSetBuilder::identifyRomBytes(const uint8_t* data, std::size_t sizeBytes) noexcept {
    if (data == nullptr || sizeBytes == 0u) {
        return std::nullopt;
    }

    // 1. IC712 XF805 C0.BIN (131,072 bytes): reset vector 0xC000 + voice table signature hash @ 0x15A39
    if (sizeBytes == 131072u &&
        data[131070] == 0xC0u && data[131071] == 0x00u &&
        matchesFnv1a64(data, sizeBytes, 0x15A39u, 10u, 0x0c456d9963d80bc2ULL)) {
        return RomChipId::MainCpuA_IC712;
    }

    // 2. IC723 XF810 C0.BIN (131,072 bytes): drum setup + sequencer header hashes @ 0x11AB5 / 0x18000
    if (sizeBytes == 131072u &&
        matchesFnv1a64(data, sizeBytes, 0x11AB5u, 6u, 0x4703840700137ba8ULL) &&
        matchesFnv1a64(data, sizeBytes, 0x18000u, 8u, 0xd485378e87ae2802ULL)) {
        return RomChipId::MainCpuB_IC723;
    }

    // 3. IC731 XF804 B0.BIN (32,768 bytes): linear reset vector 0x8000 + low-end TL curve @ 0x2A87
    if (sizeBytes == 32768u &&
        data[32766] == 0x80u && data[32767] == 0x00u &&
        data[0x2A87] == 127u && data[0x2A88] == 122u && data[0x2A89] == 118u) {
        return RomChipId::SubCpu_IC731;
    }

    // 4. XF653A0-101 IC738.BIN (524,288 bytes): OPRW wave directory + 64-byte header markers
    if (sizeBytes == 524288u) {
        const uint16_t page0 = readLE16(data);
        const std::size_t off0 = static_cast<std::size_t>(page0) << 8u;
        if (off0 + 0x40u <= sizeBytes &&
            data[off0] == 0x00u && data[off0 + 1u] == 0x00u &&
            data[off0 + 3u] == 0x02u && data[off0 + 7u] == 0x01u) {
            const uint8_t scaleCode = data[off0 + 12u];
            if (scaleCode == 0x00u || scaleCode == 0x04u || scaleCode == 0x08u || scaleCode == 0x0Cu) {
                return RomChipId::DrumWave_IC738;
            }
        }
    }

    return std::nullopt;
}

bool VF50RomSetBuilder::addRomFile(const juce::File& file, juce::String& outStatus, RomChipId* outChip) {
    if (!file.existsAsFile()) {
        outStatus = "ROM FILE NOT FOUND: " + file.getFileName();
        return false;
    }

    const int64_t fSize = file.getSize();
    if (fSize != 32768 && fSize != 131072 && fSize != 524288) {
        outStatus = "INVALID ROM SIZE: " + file.getFileName();
        return false;
    }

    juce::MemoryBlock mb;
    if (!file.loadFileAsData(mb)) {
        outStatus = "CANNOT READ ROM: " + file.getFileName();
        return false;
    }

    const auto chipOpt = identifyRomBytes(static_cast<const uint8_t*>(mb.getData()), mb.getSize());
    if (!chipOpt.has_value()) {
        outStatus = "UNRECOGNIZED VF50 ROM: " + file.getFileName();
        return false;
    }

    const RomChipId chip = *chipOpt;
    const auto idx = static_cast<std::size_t>(chip);
    const auto* ptr = static_cast<const uint8_t*>(mb.getData());
    rawBytes_[idx].assign(ptr, ptr + mb.getSize());
    sourceFiles_[idx] = file;
    if (outChip != nullptr) {
        *outChip = chip;
    }

    outStatus = "LOADED ROM [" + juce::String(loadedRawChipCount()) + "/4]: "
              + juce::String(getRomChipSpec(chip).canonicalFilename);
    return true;
}

int VF50RomSetBuilder::scanDirectory(const juce::File& directory, juce::String& outStatus) {
    if (!directory.isDirectory()) {
        outStatus = "ROM FOLDER NOT FOUND";
        return 0;
    }

    juce::Array<juce::File> candidates;
    directory.findChildFiles(candidates, juce::File::findFiles, true, "*.bin;*.BIN;*.rom;*.ROM");

    int addedCount = 0;
    for (const auto& cand : candidates) {
        const int64_t sz = cand.getSize();
        if (sz != 32768 && sz != 131072 && sz != 524288) {
            continue;
        }
        juce::String dummy;
        if (addRomFile(cand, dummy)) {
            ++addedCount;
        }
    }

    if (addedCount > 0) {
        outStatus = "LOADED " + juce::String(loadedRawChipCount()) + "/4 VF50 ROM CHIPS";
    } else {
        outStatus = "NO VF50 .BIN ROMS FOUND IN FOLDER";
    }
    return addedCount;
}

bool VF50RomSetBuilder::hasChip(RomChipId chip) const noexcept {
    const auto idx = static_cast<std::size_t>(chip);
    return idx < rawBytes_.size() && !rawBytes_[idx].empty();
}

const juce::File& VF50RomSetBuilder::chipFile(RomChipId chip) const noexcept {
    const auto idx = std::min<std::size_t>(static_cast<std::size_t>(chip), sourceFiles_.size() - 1u);
    return sourceFiles_[idx];
}

int VF50RomSetBuilder::loadedRawChipCount() const noexcept {
    int count = 0;
    for (const auto& buf : rawBytes_) {
        if (!buf.empty()) {
            ++count;
        }
    }
    return count;
}

bool VF50RomSetBuilder::hasAllFourChips() const noexcept {
    return loadedRawChipCount() == 4;
}

std::unique_ptr<VF50RomImage> VF50RomSetBuilder::buildRomImage(juce::String& outSummary) const {
    auto img = createCleanRoomBlankRomImage();

    if (hasChip(RomChipId::MainCpuA_IC712)) {
        decodeIc712(rawBytes_[0], *img);
        copyCString(img->chipSourcePaths[0].data(), img->chipSourcePaths[0].size(),
                    sourceFiles_[0].getFullPathName().toStdString());
    }
    if (hasChip(RomChipId::MainCpuB_IC723)) {
        decodeIc723(rawBytes_[1], *img);
        copyCString(img->chipSourcePaths[1].data(), img->chipSourcePaths[1].size(),
                    sourceFiles_[1].getFullPathName().toStdString());
    }
    if (hasChip(RomChipId::SubCpu_IC731)) {
        decodeIc731(rawBytes_[2], *img);
        copyCString(img->chipSourcePaths[2].data(), img->chipSourcePaths[2].size(),
                    sourceFiles_[2].getFullPathName().toStdString());
    }

    bool drumNeedsSubCpu = false;
    if (hasChip(RomChipId::DrumWave_IC738)) {
        copyCString(img->chipSourcePaths[3].data(), img->chipSourcePaths[3].size(),
                    sourceFiles_[3].getFullPathName().toStdString());
        if (hasChip(RomChipId::SubCpu_IC731)) {
            (void)decodeDrumWaveformsIc738(rawBytes_[2], rawBytes_[3], *img);
        } else {
            drumNeedsSubCpu = true;
        }
    }

    img->rebindPointers();

    if (img->isAllRomsLoaded()) {
        outSummary = "ROMS READY [4/4]: 100V/100P/61DRUMS/3DEMO";
    } else if (drumNeedsSubCpu) {
        outSummary = "ROMS [" + juce::String(loadedRawChipCount()) + "/4]: NEED IC731 FOR DRUMS";
    } else if (loadedRawChipCount() > 0) {
        outSummary = "ROMS PARTIAL [" + juce::String(loadedRawChipCount()) + "/4] LOADED";
    } else {
        outSummary = "ROM-FREE INIT MODE (0/4 ROMS)";
    }
    return img;
}

juce::File VF50RomSetBuilder::getUserDefaultRomFolder() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Victory Audio")
        .getChildFile("VF50")
        .getChildFile("ROMs");
}

juce::File VF50RomSetBuilder::getRomPathsConfigFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Victory Audio")
        .getChildFile("VF50")
        .getChildFile("vf50_rom_paths.cfg");
}

bool VF50RomSetBuilder::saveConfiguredPaths() const {
    const juce::File cfgFile = getRomPathsConfigFile();
    (void)cfgFile.getParentDirectory().createDirectory();

    juce::String text;
    static constexpr std::array<const char*, 4> kKeys = { "IC712=", "IC723=", "IC731=", "IC738=" };
    for (std::size_t i = 0; i < 4u; ++i) {
        if (!rawBytes_[i].empty() && sourceFiles_[i].existsAsFile()) {
            text << kKeys[i] << sourceFiles_[i].getFullPathName() << "\n";
        }
    }
    return cfgFile.replaceWithText(text);
}

int VF50RomSetBuilder::loadFromConfigOrUserFolder(juce::String& outStatus) {
    clear();

    const juce::File cfgFile = getRomPathsConfigFile();
    if (cfgFile.existsAsFile()) {
        juce::StringArray lines;
        cfgFile.readLines(lines);
        for (const auto& line : lines) {
            const int eqPos = line.indexOfChar('=');
            if (eqPos > 0) {
                const juce::String pathStr = line.substring(eqPos + 1).trim();
                const juce::File f(pathStr);
                if (f.existsAsFile()) {
                    juce::String dummy;
                    (void)addRomFile(f, dummy);
                }
            }
        }
    }

    const juce::File defaultDir = getUserDefaultRomFolder();
    if (!hasAllFourChips() && defaultDir.isDirectory()) {
        juce::String dummy;
        (void)scanDirectory(defaultDir, dummy);
    }

    const int n = loadedRawChipCount();
    if (n == 4) {
        outStatus = "AUTO-LOADED 4/4 VF50 ROM CHIPS";
    } else if (n > 0) {
        outStatus = "AUTO-LOADED " + juce::String(n) + "/4 VF50 ROM CHIPS";
    } else {
        outStatus = "NO EXTERNAL VF50 ROMS FOUND";
    }
    return n;
}

void VF50RomSetBuilder::clearConfiguredPaths() {
    const juce::File cfgFile = getRomPathsConfigFile();
    if (cfgFile.existsAsFile()) {
        (void)cfgFile.deleteFile();
    }
}

} // namespace vf50::rom
