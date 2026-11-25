#include "ROM/VF50RomBank.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace vf50::rom {

namespace {

template <std::size_t N>
void copyFixedName(std::array<char, N>& dst, std::string_view src, char pad = ' ') noexcept {
    dst.fill(pad);
    const std::size_t n = std::min(src.size(), N);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[i] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : ' ';
    }
}

void copyCStringTruncated(char* dst, std::size_t dstCap, std::string_view src) noexcept {
    if (dst == nullptr || dstCap == 0u) {
        return;
    }
    const std::size_t n = std::min(src.size(), dstCap - 1u);
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

[[nodiscard]] vf50::generated::FactoryVoice makeCleanInitVoice(uint8_t idx) noexcept {
    vf50::generated::FactoryVoice v{};
    v.index          = idx;
    v.name           = "INIT VOICE";
    v.algorithm      = 0u; // ALG 1: OP4 -> OP3 -> OP2 -> OP1 (OP1 carrier)
    v.feedback       = 0u;
    v.lfoSpeed       = 35u;
    v.lfoDelay       = 0u;
    v.pmd            = 0u;
    v.amd            = 0u;
    v.lfoSync        = 0u;
    v.lfoWave        = 2u; // Triangle
    v.pms            = 0u;
    v.ams            = 0u;
    v.transpose      = 0;
    v.monoMode       = 0u;
    v.pitchBendRange = 2u;
    v.sustainSwitch  = 1u;
    v.mwPitch        = 50u;
    v.peg            = { 99u, 99u, 99u, 50u, 50u, 50u };

    for (auto& op : v.operators) {
        op.ar            = 31u;
        op.d1r           = 0u;
        op.d2r           = 0u;
        op.rr            = 7u;
        op.d1l           = 15u;
        op.levelScaling  = 0;
        op.rateScaling   = 0u;
        op.egBiasSens    = 0u;
        op.ampModEnable  = 0u;
        op.keyVelSens    = 0;
        op.outputLevel   = 0u;
        op.coarse        = 4u; // 1.00 harmonic ratio
        op.fine          = 0u;
        op.detune        = 0;
        op.waveform      = 0u; // W1 pure sine
        op.egShift       = 0u;
        op.fixedFreqMode = 0u;
        op.fixedRange    = 0u;
        op.fixedRangeLow = 0u;
    }
    // Enable OP1 carrier with clean 1.00 sine output
    v.operators[0].outputLevel = 90u;
    v.effect = { 0u, 50u, 100u, 1u, 10u, 10u, 10u };
    return v;
}

[[nodiscard]] vf50::generated::FactoryPerformance makeCleanInitPerf(uint8_t idx) noexcept {
    vf50::generated::FactoryPerformance pf{};
    pf.index            = idx;
    pf.name             = "INIT PERF ";
    pf.assignMode       = 2u; // DVA
    pf.microTuneTable   = 0u;
    pf.microTuneRootKey = 0u;
    pf.perfEffectSelect = 0u;
    pf.instruments[0].maxNotes        = 16u;
    pf.instruments[0].reserveNotes    = 0u;
    pf.instruments[0].voiceBank       = 1u;
    pf.instruments[0].voiceNumber     = 0u;
    pf.instruments[0].receiveChannel  = 16u; // Omni
    pf.instruments[0].limitLow        = 0u;
    pf.instruments[0].limitHigh       = 127u;
    pf.instruments[0].volume          = 99u;
    pf.instruments[0].outAssign       = 3u;
    pf.instruments[0].lfoSelect       = 1u;
    pf.instruments[0].effectEnable    = 1u;
    for (std::size_t i = 1; i < 8u; ++i) {
        pf.instruments[i].maxNotes        = 0u;
        pf.instruments[i].outAssign       = 0u;
        pf.instruments[i].limitLow        = 0u;
        pf.instruments[i].limitHigh       = 0u;
        pf.instruments[i].receiveChannel  = static_cast<uint8_t>(i);
    }
    pf.effect = { 0u, 50u, 100u, 1u, 10u, 10u, 10u };
    return pf;
}

} // namespace

std::array<uint8_t, 64> makeCleanRoomCoarsePermTable() noexcept {
    // Pure mathematical sorting by OPZ II harmonic ratio * DT2 family multiplier (4-family matrix)
    static constexpr std::array<double, 4> kDt2 = {
        1.0, 1.4142135623730951, 1.5704582406636498, 1.7310731496594726
    };
    std::array<uint8_t, 64> perm{};
    std::iota(perm.begin(), perm.end(), uint8_t{ 0 });
    std::stable_sort(perm.begin(), perm.end(), [](uint8_t a, uint8_t b) noexcept {
        const uint8_t multA = a >> 2u;
        const uint8_t multB = b >> 2u;
        const double ratioA = (multA == 0u ? 0.5 : static_cast<double>(multA)) * kDt2[a & 0x03u];
        const double ratioB = (multB == 0u ? 0.5 : static_cast<double>(multB)) * kDt2[b & 0x03u];
        return ratioA < ratioB;
    });
    return perm;
}

std::array<uint8_t, 20> makeCleanRoomLowOutTlTable() noexcept {
    // Clean-room monotonic exponential output-level taper for levels 0..19
    std::array<uint8_t, 20> tbl{};
    for (std::size_t i = 0; i < 20u; ++i) {
        const double norm = static_cast<double>(i) / 19.0;
        const long val = std::lround(127.0 - 46.0 * std::pow(norm, 0.85));
        tbl[i] = static_cast<uint8_t>(std::clamp(val, 80L, 127L));
    }
    return tbl;
}

void VF50RomImage::initCleanRoomBlank() noexcept {
    hasMainRomA    = false;
    hasMainRomB    = false;
    hasSubCpuRom   = false;
    hasDrumWaveRom = false;
    copyCStringTruncated(firmwareVersion.data(), firmwareVersion.size(), "ROM-FREE INIT (LOAD ROMS)");
    for (auto& cp : chipSourcePaths) {
        cp.fill('\0');
    }

    coarsePermTable = makeCleanRoomCoarsePermTable();
    lowOutLevelToTl = makeCleanRoomLowOutTlTable();

    for (std::size_t i = 0; i < factoryVoices.size(); ++i) {
        factoryVoices[i] = makeCleanInitVoice(static_cast<uint8_t>(i));
        copyFixedName(voiceNames[i], "INIT VOICE");
    }

    for (std::size_t i = 0; i < factoryPerformances.size(); ++i) {
        factoryPerformances[i] = makeCleanInitPerf(static_cast<uint8_t>(i));
        copyFixedName(perfNames[i], "INIT PERF ");
    }

    for (std::size_t i = 0; i < factoryMicrotunings.size(); ++i) {
        auto& mt = factoryMicrotunings[i];
        mt.index = static_cast<uint8_t>(i);
        copyFixedName(microNames[i], (i == 0u) ? "Equal (Init)    " : "Equal Init      ");
        for (std::size_t n = 0; n < 12u; ++n) {
            mt.baseOctave[n] = { static_cast<uint8_t>(60u + n), 0u };
        }
        for (std::size_t k = 0; k < 128u; ++k) {
            mt.keys[k] = { static_cast<uint8_t>(k), 0u };
        }
    }

    copyFixedName(dspEffectNameStorage[0], "off                 ");
    for (std::size_t i = 1; i < dspEffectNameStorage.size(); ++i) {
        char buf[24]{};
        std::snprintf(buf, sizeof(buf), "Effect %-13u", static_cast<unsigned>(i));
        copyFixedName(dspEffectNameStorage[i], std::string_view(buf, 16));
    }
    for (std::size_t i = 0; i < factoryDspPresets.size(); ++i) {
        factoryDspPresets[i].effectNumber = static_cast<uint8_t>(i + 1u);
        factoryDspPresets[i].subCpuParams.fill(0u);
    }

    for (std::size_t d = 0; d < defaultDrumSetup.size(); ++d) {
        char nm[16]{};
        std::snprintf(nm, sizeof(nm), "DRUM%02u", static_cast<unsigned>(d + 1u));
        copyFixedName(drumSetupNameStorage[d], std::string_view(nm, 6));
        copyFixedName(drumWaveNameStorage[d], std::string_view(nm, 6));
        copyFixedName(drumLcdNameStorage[d], std::string_view(nm, 6));

        defaultDrumSetup[d] = {
            static_cast<uint8_t>(d),
            {},
            static_cast<uint8_t>((d >= 27u && d <= 30u) ? 0u : 7u),
            15u,
            0,
            static_cast<uint8_t>(31u + d)
        };
        drumWaveforms[d] = vf50::generated::DrumWaveformDescriptor{};
        drumWaveforms[d].index = static_cast<uint8_t>(d);
        drumPcmOffsets[d] = 0u;
    }

    for (std::size_t m = 0; m < presetDrumKeyMaps.size(); ++m) {
        for (std::size_t k = 0; k < 61u; ++k) {
            presetDrumKeyMaps[m][k] = static_cast<int8_t>(k);
        }
    }

    decodedRhythmEvents.clear();
    rhythmEventStream.clear();
    for (std::size_t p = 0; p < factoryRhythmPatterns.size(); ++p) {
        factoryRhythmPatterns[p] = {
            static_cast<uint8_t>(p), 3u, 4u, 4u, 1u, 192u, 192u, 0u, 0u, 0u, 0u
        };
    }

    drumPcmPool.clear();

    demoSongEvents.clear();
    for (std::size_t s = 0; s < factoryDemoSongs.size(); ++s) {
        copyFixedName(demoTitleStorage[s], "NO ROM   ");
        factoryDemoSongs[s] = {
            static_cast<uint8_t>(s), {}, 120u, 4u, 4u, 192u, 1u, 192u, 1u, 0u,
            static_cast<uint8_t>(s), { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u }, 0u, 0u
        };
    }

    rebindPointers();
}

void VF50RomImage::rebindPointers() noexcept {
    for (std::size_t i = 0; i < factoryVoices.size(); ++i) {
        factoryVoices[i].name = std::string_view(voiceNames[i].data(), 10u);
    }

    for (std::size_t i = 0; i < factoryPerformances.size(); ++i) {
        factoryPerformances[i].name = std::string_view(perfNames[i].data(), 10u);
    }

    for (std::size_t i = 0; i < factoryMicrotunings.size(); ++i) {
        std::size_t len = microNames[i].size();
        while (len > 0u && microNames[i][len - 1u] == ' ') {
            --len;
        }
        factoryMicrotunings[i].name = std::string_view(microNames[i].data(), len);
    }

    for (std::size_t i = 0; i < dspEffectNameStorage.size(); ++i) {
        std::size_t len = dspEffectNameStorage[i].size();
        while (len > 0u && dspEffectNameStorage[i][len - 1u] == ' ') {
            --len;
        }
        dspEffectNames[i] = std::string_view(dspEffectNameStorage[i].data(), len);
    }
    for (std::size_t i = 0; i < factoryDspPresets.size(); ++i) {
        factoryDspPresets[i].name = dspEffectNames[i + 1u];
    }

    for (std::size_t d = 0; d < defaultDrumSetup.size(); ++d) {
        std::size_t setupLen = 6u;
        while (setupLen > 0u && drumSetupNameStorage[d][setupLen - 1u] == ' ') {
            --setupLen;
        }
        defaultDrumSetup[d].name = std::string_view(drumSetupNameStorage[d].data(), setupLen);

        std::size_t waveLen = 6u;
        while (waveLen > 0u && drumWaveNameStorage[d][waveLen - 1u] == ' ') {
            --waveLen;
        }
        drumWaveforms[d].name = std::string_view(drumWaveNameStorage[d].data(), waveLen);
        drumWaveforms[d].lcdName = std::string_view(drumLcdNameStorage[d].data(), 6u);

        const std::size_t poolOff = drumPcmOffsets[d];
        if (drumWaveforms[d].totalSamples > 0u &&
            poolOff + drumWaveforms[d].totalSamples <= drumPcmPool.size()) {
            drumWaveforms[d].samples = drumPcmPool.data() + poolOff;
        } else {
            drumWaveforms[d].samples = nullptr;
            drumWaveforms[d].totalSamples = 0u;
        }
    }

    for (std::size_t s = 0; s < factoryDemoSongs.size(); ++s) {
        std::size_t len = demoTitleStorage[s].size();
        while (len > 0u && demoTitleStorage[s][len - 1u] == ' ') {
            --len;
        }
        factoryDemoSongs[s].title = std::string_view(demoTitleStorage[s].data(), len);
    }
}

const VF50RomImage& getDefaultRomImage() noexcept {
    static const std::unique_ptr<VF50RomImage> kDefaultImage = []() {
        auto img = std::make_unique<VF50RomImage>();
        img->initCleanRoomBlank();
        return img;
    }();
    return *kDefaultImage;
}

std::unique_ptr<VF50RomImage> createCleanRoomBlankRomImage() {
    auto img = std::make_unique<VF50RomImage>();
    img->initCleanRoomBlank();
    return img;
}

} // namespace vf50::rom
