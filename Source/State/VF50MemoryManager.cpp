#include "State/VF50MemoryManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vf50::state {

namespace {

constexpr uint8_t kMagic[4] = { 'V', 'F', '5', 'M' };
constexpr uint32_t kBinaryFormatVersion = 1u;

void sanitizeAsciiName(std::string_view src, std::array<char, 10>& dst) noexcept {
    dst.fill(' ');
    const std::size_t n = std::min<std::size_t>(src.size(), 10u);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[i] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : ' ';
    }
}

[[nodiscard]] uint32_t computeCrc32(const uint8_t* data, std::size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

struct ByteWriter {
    std::vector<uint8_t>& buf;

    void writeU8(uint8_t v) { buf.push_back(v); }
    void writeI8(int8_t v) { buf.push_back(static_cast<uint8_t>(v)); }
    void writeU16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFFu));
        buf.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    }
    void writeU32(uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFFu));
        buf.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
        buf.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
        buf.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
    }
    void writeF32(float v) {
        uint32_t bits = 0u;
        static_assert(sizeof(float) == sizeof(uint32_t));
        std::memcpy(&bits, &v, sizeof(uint32_t));
        writeU32(bits);
    }
    void writeBytes(const void* src, std::size_t len) {
        const auto* p = static_cast<const uint8_t*>(src);
        buf.insert(buf.end(), p, p + len);
    }
};

struct ByteReader {
    const uint8_t* data = nullptr;
    std::size_t size = 0u;
    std::size_t pos = 0u;

    [[nodiscard]] bool hasRemaining(std::size_t n) const noexcept {
        return pos <= size && (size - pos) >= n;
    }
    bool readU8(uint8_t& out) noexcept {
        if (!hasRemaining(1u)) return false;
        out = data[pos++];
        return true;
    }
    bool readI8(int8_t& out) noexcept {
        uint8_t u = 0u;
        if (!readU8(u)) return false;
        out = static_cast<int8_t>(u);
        return true;
    }
    bool readU16(uint16_t& out) noexcept {
        if (!hasRemaining(2u)) return false;
        out = static_cast<uint16_t>(data[pos]) | static_cast<uint16_t>(static_cast<uint16_t>(data[pos + 1u]) << 8u);
        pos += 2u;
        return true;
    }
    bool readU32(uint32_t& out) noexcept {
        if (!hasRemaining(4u)) return false;
        out = static_cast<uint32_t>(data[pos])
            | (static_cast<uint32_t>(data[pos + 1u]) << 8u)
            | (static_cast<uint32_t>(data[pos + 2u]) << 16u)
            | (static_cast<uint32_t>(data[pos + 3u]) << 24u);
        pos += 4u;
        return true;
    }
    bool readF32(float& out) noexcept {
        uint32_t bits = 0u;
        if (!readU32(bits)) return false;
        std::memcpy(&out, &bits, sizeof(float));
        if (!std::isfinite(out)) out = 0.0f;
        return true;
    }
    bool readBytes(void* dst, std::size_t len) noexcept {
        if (!hasRemaining(len)) return false;
        std::memcpy(dst, data + pos, len);
        pos += len;
        return true;
    }
};

void unpackVoiceFromRom95(const std::array<uint8_t, 95>& raw,
                          vf50::generated::FactoryVoice& v,
                          std::array<char, 10>& nameChars) noexcept {
    static constexpr std::array<std::size_t, 4> kVmemOpSlots = { 3u, 1u, 2u, 0u };
    for (std::size_t logOp = 0; logOp < 4u; ++logOp) {
        const std::size_t slot = kVmemOpSlots[logOp];
        const uint8_t* opB = &raw[slot * 10u];
        auto& o = v.operators[logOp];

        o.ar = opB[0] & 0x1Fu;
        o.d1r = opB[1] & 0x1Fu;
        o.d2r = opB[2] & 0x1Fu;
        o.rr = std::max<uint8_t>(1u, static_cast<uint8_t>(opB[3] & 0x0Fu));
        o.d1l = opB[4] & 0x0Fu;

        const int lsMag = std::min<int>(opB[5] & 0x7Fu, 99);
        o.ampModEnable = (opB[6] >> 6u) & 0x01u;
        o.egBiasSens = (opB[6] >> 3u) & 0x07u;
        const int kvsMag = opB[6] & 0x07u;
        o.outputLevel = std::min<uint8_t>(opB[7] & 0x7Fu, 99u);
        o.coarse = opB[8] & 0x3Fu;

        const bool lsNeg = ((opB[9] >> 6u) & 0x01u) != 0u;
        const bool kvsNeg = ((opB[9] >> 5u) & 0x01u) != 0u;
        o.rateScaling = (opB[9] >> 3u) & 0x03u;
        const int detRaw = std::clamp<int>(opB[9] & 0x07u, 0, 6);

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

    for (std::size_t i = 0; i < 10u; ++i) {
        const uint8_t c = raw[57u + i];
        nameChars[i] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : ' ';
    }
    v.name = std::string_view(nameChars.data(), nameChars.size());

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
    v.rawRomBytes       = raw;
}

void unpackPerfFromRom97(const std::array<uint8_t, 97>& raw,
                         vf50::generated::FactoryPerformance& pf,
                         std::array<char, 10>& nameChars) noexcept {
    for (std::size_t i = 0; i < 10u; ++i) {
        const uint8_t c = raw[66u + i];
        nameChars[i] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : ' ';
    }
    pf.name = std::string_view(nameChars.data(), nameChars.size());

    pf.microTuneTable   = std::min<uint8_t>(raw[64] & 0x0Fu, 12u);
    const uint8_t efsel2 = (raw[64] >> 4u) & 0x03u;
    const uint8_t efsel1 = (raw[65] >> 1u) & 0x03u;
    pf.perfEffectSelect = (efsel1 != 0u) ? static_cast<uint8_t>(std::min<int>(efsel2 * 3u + efsel1, 12)) : 0u;
    pf.microTuneRootKey = std::min<uint8_t>((raw[65] >> 3u) & 0x0Fu, 11u);
    const uint8_t asmod1 = raw[65] & 0x01u;
    const uint8_t asmod2 = raw[92] & 0x01u;
    pf.assignMode = std::min<uint8_t>(static_cast<uint8_t>((asmod2 << 1u) | asmod1), 2u);

    for (std::size_t i = 0; i < 8u; ++i) {
        const uint8_t* ib = &raw[i * 8u];
        auto& inst = pf.instruments[i];
        const uint8_t max1 = ib[0] & 0x0Fu;
        const uint8_t pairByte = raw[93u + (i / 2u)];
        const uint8_t max2 = ((i % 2u) == 0u) ? ((pairByte >> 2u) & 0x01u) : (pairByte & 0x01u);
        inst.maxNotes        = std::min<uint8_t>(static_cast<uint8_t>((max2 << 4u) | max1), 16u);
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

        const uint8_t pm2 = raw[76u + i];
        inst.reserveNotes = std::min<uint8_t>(pm2 & 0x1Fu, 17u);
        inst.effectEnable = (pm2 >> 6u) & 0x01u;
    }

    pf.effect.effectType = std::min<uint8_t>(raw[84] & 0x3Fu, 32u);
    pf.effect.balance    = std::min<uint8_t>(raw[85] & 0x7Fu, 100u);
    pf.effect.outLevel   = std::min<uint8_t>(raw[86] & 0x7Fu, 100u);
    pf.effect.stereoMix  = raw[87] & 0x01u;
    pf.effect.param1     = std::min<uint8_t>(raw[88] & 0x7Fu, 75u);
    pf.effect.param2     = std::min<uint8_t>(raw[89] & 0x7Fu, 99u);
    pf.effect.param3     = std::min<uint8_t>(raw[90] & 0x7Fu, 99u);
    pf.rawRomBytes       = raw;
}

} // namespace

// =============================================================================
// StoredVoiceSlot
// =============================================================================

StoredVoiceSlot::StoredVoiceSlot() noexcept {
    setFromVoice(VF50MemoryManager::makeInitVoice());
}

StoredVoiceSlot::StoredVoiceSlot(const vf50::generated::FactoryVoice& src) noexcept {
    setFromVoice(src);
}

StoredVoiceSlot::StoredVoiceSlot(const StoredVoiceSlot& other) noexcept
    : voice(other.voice), nameChars(other.nameChars) {
    voice.name = std::string_view(nameChars.data(), nameChars.size());
}

StoredVoiceSlot& StoredVoiceSlot::operator=(const StoredVoiceSlot& other) noexcept {
    if (this != &other) {
        voice = other.voice;
        nameChars = other.nameChars;
        voice.name = std::string_view(nameChars.data(), nameChars.size());
    }
    return *this;
}

StoredVoiceSlot::StoredVoiceSlot(StoredVoiceSlot&& other) noexcept
    : voice(other.voice), nameChars(other.nameChars) {
    voice.name = std::string_view(nameChars.data(), nameChars.size());
}

StoredVoiceSlot& StoredVoiceSlot::operator=(StoredVoiceSlot&& other) noexcept {
    if (this != &other) {
        voice = other.voice;
        nameChars = other.nameChars;
        voice.name = std::string_view(nameChars.data(), nameChars.size());
    }
    return *this;
}

void StoredVoiceSlot::setFromVoice(const vf50::generated::FactoryVoice& src) noexcept {
    const std::string_view srcName = src.name;
    sanitizeAsciiName(srcName, nameChars);
    voice = src;
    voice.name = std::string_view(nameChars.data(), nameChars.size());
    repackRomBytes();
}

void StoredVoiceSlot::overwriteCleanFromPreset(const VF50VoiceData& pristine) noexcept {
    const std::string_view srcName = pristine.name;
    voice = pristine;
    std::memcpy(voice.rawRomBytes.data(), pristine.rawRomBytes.data(), pristine.rawRomBytes.size());
    sanitizeAsciiName(srcName, nameChars);
    voice.name = std::string_view(nameChars.data(), nameChars.size());
}

void StoredVoiceSlot::setName(std::string_view newName) noexcept {
    sanitizeAsciiName(newName, nameChars);
    voice.name = std::string_view(nameChars.data(), nameChars.size());
    for (std::size_t i = 0; i < 10u; ++i) {
        voice.rawRomBytes[57u + i] = static_cast<uint8_t>(nameChars[i]);
    }
}

void StoredVoiceSlot::repackRomBytes() noexcept {
    auto& raw = voice.rawRomBytes;
    raw.fill(0u);

    static constexpr std::array<std::size_t, 4> kVmemOpSlots = { 3u, 1u, 2u, 0u };
    for (std::size_t logOp = 0; logOp < 4u; ++logOp) {
        const std::size_t slot = kVmemOpSlots[logOp];
        const auto& o = voice.operators[logOp];
        uint8_t* opB = &raw[slot * 10u];

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
        opB[3] = o.rr & 0x0Fu;
        opB[4] = o.d1l & 0x0Fu;
        opB[5] = lsMag & 0x7Fu;
        opB[6] = static_cast<uint8_t>(((o.ampModEnable & 0x01u) << 6u)
                                    | ((o.egBiasSens & 0x07u) << 3u)
                                    | (kvsMag & 0x07u));
        opB[7] = std::min<uint8_t>(o.outputLevel, 99u);
        opB[8] = o.coarse & 0x3Fu;
        opB[9] = static_cast<uint8_t>((lsNeg << 6u) | (kvsNeg << 5u)
                                    | ((o.rateScaling & 0x03u) << 3u) | (detRaw & 0x07u));

        raw[73u + slot * 2u] = static_cast<uint8_t>(((o.fixedRangeLow & 0x01u) << 6u)
                                                  | ((o.egShift & 0x03u) << 4u)
                                                  | ((o.fixedFreqMode & 0x01u) << 3u)
                                                  | (o.fixedRange & 0x07u));
        raw[74u + slot * 2u] = static_cast<uint8_t>(((o.waveform & 0x07u) << 4u) | (o.fine & 0x0Fu));
    }

    raw[40] = static_cast<uint8_t>(((voice.lfoSync & 0x01u) << 6u)
                                 | ((voice.feedback & 0x07u) << 3u)
                                 | (voice.algorithm & 0x07u));
    raw[41] = std::min<uint8_t>(voice.lfoSpeed, 99u);
    raw[42] = std::min<uint8_t>(voice.lfoDelay, 99u);
    raw[43] = std::min<uint8_t>(voice.pmd, 99u);
    raw[44] = std::min<uint8_t>(voice.amd, 99u);
    raw[45] = static_cast<uint8_t>(((voice.pms & 0x07u) << 4u)
                                 | ((voice.ams & 0x03u) << 2u)
                                 | (voice.lfoWave & 0x03u));
    raw[46] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(voice.transpose) + 24, 0, 48));
    raw[47] = std::min<uint8_t>(voice.pitchBendRange, 12u);
    raw[48] = static_cast<uint8_t>(((voice.chorusSwitch & 0x01u) << 4u)
                                 | ((voice.monoMode & 0x01u) << 3u)
                                 | ((voice.sustainSwitch & 0x01u) << 2u)
                                 | ((voice.portaSwitch & 0x01u) << 1u)
                                 | (voice.portaMode & 0x01u));
    raw[49] = std::min<uint8_t>(voice.portaTime, 99u);
    raw[50] = std::min<uint8_t>(voice.fcVolume, 99u);
    raw[51] = std::min<uint8_t>(voice.mwPitch, 99u);
    raw[52] = std::min<uint8_t>(voice.mwAmp, 99u);
    raw[53] = std::min<uint8_t>(voice.bcPitch, 99u);
    raw[54] = std::min<uint8_t>(voice.bcAmp, 99u);
    raw[55] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(voice.bcPitchBias) + 50, 0, 100));
    raw[56] = std::min<uint8_t>(voice.bcEgBias, 99u);

    for (std::size_t i = 0; i < 10u; ++i) {
        raw[57u + i] = static_cast<uint8_t>(nameChars[i]);
    }

    raw[67] = std::min<uint8_t>(voice.peg.pr1, 99u);
    raw[68] = std::min<uint8_t>(voice.peg.pr2, 99u);
    raw[69] = std::min<uint8_t>(voice.peg.pr3, 99u);
    raw[70] = std::min<uint8_t>(voice.peg.pl1, 99u);
    raw[71] = std::min<uint8_t>(voice.peg.pl2, 99u);
    raw[72] = std::min<uint8_t>(voice.peg.pl3, 99u);

    raw[81] = voice.reverbRate & 0x07u;
    raw[82] = std::min<uint8_t>(voice.fcPitch, 99u);
    raw[83] = std::min<uint8_t>(voice.fcAmp, 99u);
    raw[84] = std::min<uint8_t>(voice.atPitch, 99u);
    raw[85] = std::min<uint8_t>(voice.atAmp, 99u);
    raw[86] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(voice.atPitchBias) + 50, 0, 100));
    raw[87] = std::min<uint8_t>(voice.atEgBias, 99u);

    raw[88] = std::min<uint8_t>(voice.effect.effectType, 32u);
    raw[89] = std::min<uint8_t>(voice.effect.balance, 100u);
    raw[90] = std::min<uint8_t>(voice.effect.outLevel, 100u);
    raw[91] = voice.effect.stereoMix & 0x01u;
    raw[92] = std::min<uint8_t>(voice.effect.param1, 75u);
    raw[93] = std::min<uint8_t>(voice.effect.param2, 99u);
    raw[94] = std::min<uint8_t>(voice.effect.param3, 99u);

    voice.name = std::string_view(nameChars.data(), nameChars.size());
}

// =============================================================================
// StoredPerformanceSlot
// =============================================================================

StoredPerformanceSlot::StoredPerformanceSlot() noexcept {
    setFromPerformance(VF50MemoryManager::makeInitPerformance(PerfInitTemplate::All));
}

StoredPerformanceSlot::StoredPerformanceSlot(const vf50::generated::FactoryPerformance& src) noexcept {
    setFromPerformance(src);
}

StoredPerformanceSlot::StoredPerformanceSlot(const StoredPerformanceSlot& other) noexcept
    : perf(other.perf), nameChars(other.nameChars), partPans(other.partPans) {
    perf.name = std::string_view(nameChars.data(), nameChars.size());
}

StoredPerformanceSlot& StoredPerformanceSlot::operator=(const StoredPerformanceSlot& other) noexcept {
    if (this != &other) {
        perf = other.perf;
        nameChars = other.nameChars;
        partPans = other.partPans;
        perf.name = std::string_view(nameChars.data(), nameChars.size());
    }
    return *this;
}

StoredPerformanceSlot::StoredPerformanceSlot(StoredPerformanceSlot&& other) noexcept
    : perf(other.perf), nameChars(other.nameChars), partPans(other.partPans) {
    perf.name = std::string_view(nameChars.data(), nameChars.size());
}

StoredPerformanceSlot& StoredPerformanceSlot::operator=(StoredPerformanceSlot&& other) noexcept {
    if (this != &other) {
        perf = other.perf;
        nameChars = other.nameChars;
        partPans = other.partPans;
        perf.name = std::string_view(nameChars.data(), nameChars.size());
    }
    return *this;
}

void StoredPerformanceSlot::setFromPerformance(const vf50::generated::FactoryPerformance& src,
                                               const std::array<int8_t, 8>& pans) noexcept {
    const std::string_view srcName = src.name;
    sanitizeAsciiName(srcName, nameChars);
    perf = src;
    partPans = pans;
    perf.name = std::string_view(nameChars.data(), nameChars.size());
    repackRomBytes();
}

void StoredVoiceSlot::unpackFromRom95(const std::array<uint8_t, 95>& raw) noexcept {
    unpackVoiceFromRom95(raw, voice, nameChars);
}

void StoredPerformanceSlot::unpackFromRom97(const std::array<uint8_t, 97>& raw,
                                            const std::array<int8_t, 8>& pans) noexcept {
    unpackPerfFromRom97(raw, perf, nameChars);
    partPans = pans;
}

void StoredPerformanceSlot::setName(std::string_view newName) noexcept {
    sanitizeAsciiName(newName, nameChars);
    perf.name = std::string_view(nameChars.data(), nameChars.size());
    for (std::size_t i = 0; i < 10u; ++i) {
        perf.rawRomBytes[66u + i] = static_cast<uint8_t>(nameChars[i]);
    }
}

void StoredPerformanceSlot::repackRomBytes() noexcept {
    auto& raw = perf.rawRomBytes;
    raw.fill(0u);

    for (std::size_t i = 0; i < 8u; ++i) {
        const auto& inst = perf.instruments[i];
        uint8_t* ib = &raw[i * 8u];
        const uint8_t clampedMax = std::min<uint8_t>(inst.maxNotes, 16u);
        const uint8_t max1 = clampedMax & 0x0Fu;
        const uint8_t max2 = (clampedMax >> 4u) & 0x01u;

        ib[0] = static_cast<uint8_t>(((inst.outAssign & 0x03u) << 5u)
                                   | ((inst.voiceBank & 0x01u) << 4u)
                                   | max1);
        ib[1] = std::min<uint8_t>(inst.voiceNumber, 99u);
        ib[2] = static_cast<uint8_t>(((inst.lfoSelect & 0x03u) << 5u)
                                   | (std::min<uint8_t>(inst.receiveChannel, 17u) & 0x1Fu));
        ib[3] = inst.limitLow & 0x7Fu;
        ib[4] = inst.limitHigh & 0x7Fu;
        ib[5] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(inst.detune) + 7, 0, 14));
        ib[6] = static_cast<uint8_t>(((inst.microTuneEnable & 0x01u) << 6u)
                                   | (std::clamp<int>(static_cast<int>(inst.noteShift) + 24, 0, 48) & 0x3Fu));
        ib[7] = std::min<uint8_t>(inst.volume, 99u);

        raw[76u + i] = static_cast<uint8_t>(((inst.effectEnable & 0x01u) << 6u)
                                          | (std::min<uint8_t>(inst.reserveNotes, 17u) & 0x1Fu));

        if ((i % 2u) == 0u) {
            raw[93u + (i / 2u)] |= static_cast<uint8_t>(max2 << 2u);
        } else {
            raw[93u + (i / 2u)] |= max2;
        }
    }

    const uint8_t pef = std::min<uint8_t>(perf.perfEffectSelect, 12u);
    const uint8_t efsel1 = (pef == 0u) ? 0u : static_cast<uint8_t>(((pef - 1u) % 3u) + 1u);
    const uint8_t efsel2 = (pef == 0u) ? 0u : static_cast<uint8_t>((pef - 1u) / 3u);
    const uint8_t asmod  = std::min<uint8_t>(perf.assignMode, 2u);

    raw[64] = static_cast<uint8_t>(((efsel2 & 0x03u) << 4u) | (std::min<uint8_t>(perf.microTuneTable, 12u) & 0x0Fu));
    raw[65] = static_cast<uint8_t>(((std::min<uint8_t>(perf.microTuneRootKey, 11u) & 0x0Fu) << 3u)
                                 | ((efsel1 & 0x03u) << 1u)
                                 | (asmod & 0x01u));

    for (std::size_t i = 0; i < 10u; ++i) {
        raw[66u + i] = static_cast<uint8_t>(nameChars[i]);
    }

    raw[84] = std::min<uint8_t>(perf.effect.effectType, 32u);
    raw[85] = std::min<uint8_t>(perf.effect.balance, 100u);
    raw[86] = std::min<uint8_t>(perf.effect.outLevel, 100u);
    raw[87] = perf.effect.stereoMix & 0x01u;
    raw[88] = std::min<uint8_t>(perf.effect.param1, 75u);
    raw[89] = std::min<uint8_t>(perf.effect.param2, 99u);
    raw[90] = std::min<uint8_t>(perf.effect.param3, 99u);
    raw[92] = (asmod >> 1u) & 0x01u;

    perf.name = std::string_view(nameChars.data(), nameChars.size());
}

// =============================================================================
// RhythmKitBank & UserRhythmPattern
// =============================================================================

void RhythmKitBank::setName(std::string_view newName) noexcept {
    sanitizeAsciiName(newName, nameChars);
}

void UserRhythmPattern::clear() noexcept {
    bars = 1u;
    timeSigNum = 4u;
    timeSigDen = 4u;
    totalTicks = 192u;
    eventCount = 0u;
    events.fill(vf50::generated::RhythmNoteEvent{ 0u, 0u, 5u, 0u });
}

bool UserRhythmPattern::addEventSorted(uint16_t tick, uint8_t drumIndex, uint8_t velocityLevel1To9) noexcept {
    if (eventCount >= kMaxEvents || drumIndex >= vf50::generated::kDrumInstrumentCount) {
        return false;
    }
    const vf50::generated::RhythmNoteEvent ev{
        std::min<uint16_t>(tick, static_cast<uint16_t>(totalTicks > 0u ? totalTicks - 1u : 0u)),
        drumIndex,
        std::clamp<uint8_t>(velocityLevel1To9, 1u, 9u),
        0u
    };
    std::size_t insertPos = eventCount;
    while (insertPos > 0u && events[insertPos - 1u].tick > ev.tick) {
        events[insertPos] = events[insertPos - 1u];
        --insertPos;
    }
    events[insertPos] = ev;
    ++eventCount;
    return true;
}

bool UserRhythmPattern::eraseEventsAtTick(uint16_t tick, uint8_t drumIndex, uint16_t toleranceTicks) noexcept {
    bool removed = false;
    std::size_t writeIdx = 0u;
    for (std::size_t i = 0u; i < eventCount; ++i) {
        const auto& ev = events[i];
        const uint16_t diff = (ev.tick >= tick) ? static_cast<uint16_t>(ev.tick - tick)
                                                : static_cast<uint16_t>(tick - ev.tick);
        if (ev.drumIndex == drumIndex && diff <= toleranceTicks) {
            removed = true;
            continue;
        }
        events[writeIdx++] = ev;
    }
    eventCount = static_cast<uint16_t>(writeIdx);
    return removed;
}

bool UserRhythmPattern::eraseInstrument(uint8_t drumIndex) noexcept {
    bool removed = false;
    std::size_t writeIdx = 0u;
    for (std::size_t i = 0u; i < eventCount; ++i) {
        const auto& ev = events[i];
        if (ev.drumIndex == drumIndex) {
            removed = true;
            continue;
        }
        events[writeIdx++] = ev;
    }
    eventCount = static_cast<uint16_t>(writeIdx);
    return removed;
}

const vf50::generated::RhythmNoteEvent* UserRhythmPattern::findEventAtTick(uint16_t tick, uint8_t drumIndex) const noexcept {
    for (std::size_t i = 0u; i < eventCount; ++i) {
        if (events[i].tick == tick && events[i].drumIndex == drumIndex) {
            return &events[i];
        }
    }
    return nullptr;
}

vf50::generated::RhythmNoteEvent* UserRhythmPattern::findMutableEventAtTick(uint16_t tick, uint8_t drumIndex) noexcept {
    for (std::size_t i = 0u; i < eventCount; ++i) {
        if (events[i].tick == tick && events[i].drumIndex == drumIndex) {
            return &events[i];
        }
    }
    return nullptr;
}

// =============================================================================
// VF50MemoryManager
// =============================================================================

VF50MemoryManager::VF50MemoryManager() noexcept {
    setRuntimeRomBank(&vf50::rom::getDefaultRomImage(), true);
}

void VF50MemoryManager::setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadDefaults) noexcept {
    romBank_ = (romBank != nullptr) ? romBank : &vf50::rom::getDefaultRomImage();
    const auto& rom = *romBank_;

    // Update Factory Rhythm Kit & Choke Groups from active ROM image
    factoryRhythmKit_.setName("FACTORY KT");
    factoryRhythmKit_.rhythmVolume = 85u;
    factoryRhythmKit_.effectMode = vf50::dsp::drums::RhythmEffectMode::Mix;
    factoryRhythmKit_.keyMapSelect = 0u;
    for (std::size_t i = 0; i < vf50::generated::kDrumInstrumentCount; ++i) {
        const auto& def = rom.defaultDrumSetup[i];
        auto& d = factoryRhythmKit_.instruments[i];
        d.volume = def.defaultVolume;
        d.pan = def.defaultPan;
        d.pitchSemitones = 0.0f;
        d.accent = 5u;
        d.outAssign = 3u;
        d.midiNote = def.defaultMidiNote;
        d.chokeGroup = def.chokeGroup;
    }

    if (reloadDefaults) {
        resetAllUserRamToFactory();
    }
}

void VF50MemoryManager::resetAllUserRamToFactory() noexcept {
    const auto& rom = runtimeRomBank();
    for (std::size_t i = 0; i < kVoiceBankSize; ++i) {
        userVoices_[i].setFromVoice(rom.factoryVoices[i]);
    }
    for (std::size_t i = 0; i < kPerfBankSize; ++i) {
        userPerformances_[i].setFromPerformance(rom.factoryPerformances[i]);
    }

    for (std::size_t k = 0; k < kUserRhythmKitCount; ++k) {
        userRhythmKits_[k] = factoryRhythmKit_;
        std::array<char, 10> kitName = { 'U','S','E','R',' ','K','I','T',' ', static_cast<char>('1' + k) };
        userRhythmKits_[k].setName(std::string_view(kitName.data(), kitName.size()));
    }
    rhythmEditKit_ = factoryRhythmKit_;
    selectedRhythmKit_ = 0u;

    userKeyMaps_[0] = rom.presetDrumKeyMaps[0];
    userKeyMaps_[1] = rom.presetDrumKeyMaps[1];

    for (std::size_t p = 0; p < kRhythmPatternCount; ++p) {
        userRhythmPatterns_[p].clear();
    }

    for (std::size_t n = 0; n < 12u; ++n) {
        userOctaveMicrotuning_[n] = { static_cast<uint8_t>(60u + n), 0u };
    }
    userFullMicrotuning_ = rom.factoryMicrotunings[0].keys;

    selectedVoiceBank_ = MemoryBank::FactoryRom;
    selectedVoiceIndex_ = 0u;
    singleEditBuffer_.setFromVoice(rom.factoryVoices[0]);
    singleCompareBackup_ = singleEditBuffer_;
    singleRecallBuffer_ = singleEditBuffer_;
    singleEditDirty_ = false;
    hasSingleRecall_ = false;
    singleCompareActive_ = false;

    selectedPerfBank_ = MemoryBank::FactoryRom;
    selectedPerfIndex_ = 0u;
    perfEditBuffer_.setFromPerformance(rom.factoryPerformances[0]);
    perfCompareBackup_ = perfEditBuffer_;
    perfRecallBuffer_ = perfEditBuffer_;
    perfEditDirty_ = false;
    hasPerfRecall_ = false;
    perfCompareActive_ = false;

    selectedPatternBank_ = MemoryBank::FactoryRom;
    selectedPatternIndex_ = 0u;

    mode_ = WorkstationMode::SinglePlay;
    underlyingFmMode_ = vf50::dsp::fm::SynthPlayMode::Single;
}

void VF50MemoryManager::setWorkstationMode(WorkstationMode newMode) noexcept {
    if (mode_ == newMode) {
        return;
    }

    switch (newMode) {
        case WorkstationMode::SinglePlay:
            if (singleCompareActive_) {
                singleCompareActive_ = false;
            }
            underlyingFmMode_ = vf50::dsp::fm::SynthPlayMode::Single;
            break;

        case WorkstationMode::SingleEdit:
            singleCompareBackup_.setFromVoice(getVoice(selectedVoiceBank_, selectedVoiceIndex_));
            singleCompareActive_ = false;
            underlyingFmMode_ = vf50::dsp::fm::SynthPlayMode::Single;
            break;

        case WorkstationMode::PerformancePlay:
            if (perfCompareActive_) {
                perfCompareActive_ = false;
            }
            underlyingFmMode_ = vf50::dsp::fm::SynthPlayMode::Performance;
            break;

        case WorkstationMode::PerformanceEdit:
            perfCompareBackup_.setFromPerformance(
                getPerformance(selectedPerfBank_, selectedPerfIndex_),
                (selectedPerfBank_ == MemoryBank::UserRam) ? getUserPerformancePartPans(selectedPerfIndex_) : std::array<int8_t, 8>{});
            perfCompareActive_ = false;
            underlyingFmMode_ = vf50::dsp::fm::SynthPlayMode::Performance;
            break;

        case WorkstationMode::RhythmPlay:
        case WorkstationMode::EffectEdit:
            // Preserve current underlying FM Single vs Performance play context
            break;
    }

    mode_ = newMode;
}

vf50::dsp::fm::SynthPlayMode VF50MemoryManager::effectiveFmPlayMode() const noexcept {
    if (mode_ == WorkstationMode::SinglePlay || mode_ == WorkstationMode::SingleEdit) {
        return vf50::dsp::fm::SynthPlayMode::Single;
    }
    if (mode_ == WorkstationMode::PerformancePlay || mode_ == WorkstationMode::PerformanceEdit) {
        return vf50::dsp::fm::SynthPlayMode::Performance;
    }
    return underlyingFmMode_;
}

std::string_view VF50MemoryManager::workstationModeName(WorkstationMode m) noexcept {
    switch (m) {
        case WorkstationMode::SinglePlay:      return "SINGLE PLAY";
        case WorkstationMode::SingleEdit:      return "SINGLE EDIT";
        case WorkstationMode::PerformancePlay: return "PERF PLAY";
        case WorkstationMode::PerformanceEdit: return "PERF EDIT";
        case WorkstationMode::RhythmPlay:      return "RHYTHM PLAY";
        case WorkstationMode::EffectEdit:      return "EFFECT EDIT";
    }
    return "SINGLE PLAY";
}

const vf50::generated::FactoryVoice& VF50MemoryManager::getFactoryVoice(std::size_t index) const noexcept {
    return runtimeRomBank().factoryVoices[std::min(index, kVoiceBankSize - 1u)];
}

const vf50::generated::FactoryVoice& VF50MemoryManager::getUserVoice(std::size_t index) const noexcept {
    return userVoices_[std::min(index, kVoiceBankSize - 1u)].voice;
}

const vf50::generated::FactoryVoice& VF50MemoryManager::getVoice(MemoryBank bank, std::size_t index) const noexcept {
    return (bank == MemoryBank::UserRam) ? getUserVoice(index) : getFactoryVoice(index);
}

void VF50MemoryManager::selectSingleVoice(MemoryBank bank, std::size_t index) noexcept {
    if (singleEditDirty_) {
        singleRecallBuffer_ = singleEditBuffer_;
        hasSingleRecall_ = true;
    }

    selectedVoiceBank_ = bank;
    selectedVoiceIndex_ = std::min(index, kVoiceBankSize - 1u);

    // Reset all front-panel Quick Edit offsets and Single Voice performance switches
    resetQuickEditOffsets();
    operatorEnableMask_ = 0x0Fu;
    singleEffectSend_ = true;

    // Retrieve pristine, read-only VF50VoiceData and overwrite VMEM in a single clean copy
    const VF50VoiceData& pristine = (selectedVoiceBank_ == MemoryBank::FactoryRom)
                                       ? runtimeRomBank().factoryVoices[selectedVoiceIndex_]
                                       : userVoices_[selectedVoiceIndex_].voice;

    StoredVoiceSlot cleanEditBuffer{};
    cleanEditBuffer.overwriteCleanFromPreset(pristine);
    singleEditBuffer_ = cleanEditBuffer;
    singleCompareBackup_ = cleanEditBuffer;
    singleEditDirty_ = false;
    singleCompareActive_ = false;
}

void VF50MemoryManager::storeUserVoice(std::size_t index, const vf50::generated::FactoryVoice& voice) noexcept {
    if (index < kVoiceBankSize) {
        userVoices_[index].setFromVoice(voice);
        userVoices_[index].voice.index = static_cast<uint8_t>(index);
        if (selectedVoiceBank_ == MemoryBank::UserRam && selectedVoiceIndex_ == index && mode_ == WorkstationMode::SinglePlay) {
            singleEditBuffer_ = userVoices_[index];
        }
    }
}

void VF50MemoryManager::storeEditBufferToUserVoice(std::size_t destUserIndex, std::string_view optionalNewName) noexcept {
    const std::size_t idx = std::min(destUserIndex, kVoiceBankSize - 1u);
    if (!optionalNewName.empty()) {
        singleEditBuffer_.setName(optionalNewName);
    }
    // Manual p. 38: Cannot store a voice with operators turned off; STORE automatically restores 1111
    operatorEnableMask_ = 0x0Fu;
    singleEditBuffer_.repackRomBytes();
    userVoices_[idx] = singleEditBuffer_;
    userVoices_[idx].voice.index = static_cast<uint8_t>(idx);
    selectedVoiceBank_ = MemoryBank::UserRam;
    selectedVoiceIndex_ = idx;
    singleCompareBackup_ = singleEditBuffer_;
    singleEditDirty_ = false;
    singleCompareActive_ = false;
}

const vf50::generated::FactoryVoice& VF50MemoryManager::activeSingleVoice() const noexcept {
    return singleCompareActive_ ? singleCompareBackup_.voice : singleEditBuffer_.voice;
}

void VF50MemoryManager::initSingleVoiceEditBuffer() noexcept {
    singleRecallBuffer_ = singleEditBuffer_;
    hasSingleRecall_ = true;
    resetQuickEditOffsets();
    operatorEnableMask_ = 0x0Fu;
    singleEffectSend_ = true;
    singleCompareBackup_.setFromVoice(getVoice(selectedVoiceBank_, selectedVoiceIndex_));
    singleEditBuffer_.setFromVoice(makeInitVoice());
    singleEditDirty_ = true;
    singleCompareActive_ = false;
}

bool VF50MemoryManager::recallSingleVoiceEditBuffer() noexcept {
    if (!hasSingleRecall_) {
        return false;
    }
    singleEditBuffer_ = singleRecallBuffer_;
    singleEditBuffer_.repackRomBytes();
    singleEditDirty_ = true;
    singleCompareActive_ = false;
    return true;
}

bool VF50MemoryManager::toggleSingleCompare() noexcept {
    singleCompareActive_ = !singleCompareActive_;
    return singleCompareActive_;
}

const vf50::generated::FactoryPerformance& VF50MemoryManager::getFactoryPerformance(std::size_t index) const noexcept {
    return runtimeRomBank().factoryPerformances[std::min(index, kPerfBankSize - 1u)];
}

const vf50::generated::FactoryPerformance& VF50MemoryManager::getUserPerformance(std::size_t index) const noexcept {
    return userPerformances_[std::min(index, kPerfBankSize - 1u)].perf;
}

const std::array<int8_t, 8>& VF50MemoryManager::getUserPerformancePartPans(std::size_t index) const noexcept {
    return userPerformances_[std::min(index, kPerfBankSize - 1u)].partPans;
}

const vf50::generated::FactoryPerformance& VF50MemoryManager::getPerformance(MemoryBank bank, std::size_t index) const noexcept {
    return (bank == MemoryBank::UserRam) ? getUserPerformance(index) : getFactoryPerformance(index);
}

void VF50MemoryManager::selectPerformance(MemoryBank bank, std::size_t index) noexcept {
    if (perfEditDirty_) {
        perfRecallBuffer_ = perfEditBuffer_;
        hasPerfRecall_ = true;
    }
    selectedPerfBank_ = bank;
    selectedPerfIndex_ = std::min(index, kPerfBankSize - 1u);
    const std::array<int8_t, 8> pans = (selectedPerfBank_ == MemoryBank::UserRam)
                                           ? userPerformances_[selectedPerfIndex_].partPans
                                           : std::array<int8_t, 8>{};
    perfEditBuffer_.setFromPerformance(getPerformance(selectedPerfBank_, selectedPerfIndex_), pans);
    perfCompareBackup_ = perfEditBuffer_;
    perfEditDirty_ = false;
    perfCompareActive_ = false;
}

void VF50MemoryManager::storeUserPerformance(std::size_t index,
                                            const vf50::generated::FactoryPerformance& perf,
                                            const std::array<int8_t, 8>& partPans) noexcept {
    if (index < kPerfBankSize) {
        userPerformances_[index].setFromPerformance(perf, partPans);
        userPerformances_[index].perf.index = static_cast<uint8_t>(index);
        if (selectedPerfBank_ == MemoryBank::UserRam && selectedPerfIndex_ == index && mode_ == WorkstationMode::PerformancePlay) {
            perfEditBuffer_ = userPerformances_[index];
        }
    }
}

void VF50MemoryManager::storeEditBufferToUserPerformance(std::size_t destUserIndex, std::string_view optionalNewName) noexcept {
    const std::size_t idx = std::min(destUserIndex, kPerfBankSize - 1u);
    if (!optionalNewName.empty()) {
        perfEditBuffer_.setName(optionalNewName);
    }
    perfEditBuffer_.repackRomBytes();
    userPerformances_[idx] = perfEditBuffer_;
    userPerformances_[idx].perf.index = static_cast<uint8_t>(idx);
    selectedPerfBank_ = MemoryBank::UserRam;
    selectedPerfIndex_ = idx;
    perfCompareBackup_ = perfEditBuffer_;
    perfEditDirty_ = false;
    perfCompareActive_ = false;
}

const vf50::generated::FactoryPerformance& VF50MemoryManager::activePerformance() const noexcept {
    return perfCompareActive_ ? perfCompareBackup_.perf : perfEditBuffer_.perf;
}

const std::array<int8_t, 8>& VF50MemoryManager::activePerformancePartPans() const noexcept {
    return perfCompareActive_ ? perfCompareBackup_.partPans : perfEditBuffer_.partPans;
}

void VF50MemoryManager::initPerformanceEditBuffer(PerfInitTemplate tmpl) noexcept {
    perfRecallBuffer_ = perfEditBuffer_;
    hasPerfRecall_ = true;
    const std::array<int8_t, 8> origPans = (selectedPerfBank_ == MemoryBank::UserRam)
                                               ? getUserPerformancePartPans(selectedPerfIndex_)
                                               : std::array<int8_t, 8>{};
    perfCompareBackup_.setFromPerformance(getPerformance(selectedPerfBank_, selectedPerfIndex_), origPans);
    perfEditBuffer_.setFromPerformance(makeInitPerformance(tmpl));
    perfEditDirty_ = true;
    perfCompareActive_ = false;
}

bool VF50MemoryManager::recallPerformanceEditBuffer() noexcept {
    if (!hasPerfRecall_) {
        return false;
    }
    perfEditBuffer_ = perfRecallBuffer_;
    perfEditBuffer_.repackRomBytes();
    perfEditDirty_ = true;
    perfCompareActive_ = false;
    return true;
}

bool VF50MemoryManager::togglePerformanceCompare() noexcept {
    perfCompareActive_ = !perfCompareActive_;
    return perfCompareActive_;
}

const RhythmKitBank& VF50MemoryManager::activeRhythmKit() const noexcept {
    return rhythmEditKit_;
}

void VF50MemoryManager::selectRhythmKit(std::size_t kitSelect0To4) noexcept {
    selectedRhythmKit_ = std::min<std::size_t>(kitSelect0To4, kUserRhythmKitCount);
    if (selectedRhythmKit_ == 0u) {
        rhythmEditKit_ = factoryRhythmKit_;
    } else {
        rhythmEditKit_ = userRhythmKits_[selectedRhythmKit_ - 1u];
    }
}

void VF50MemoryManager::storeActiveKitToUserSlot(std::size_t userKitIdx0To3, std::string_view optionalName) noexcept {
    const std::size_t idx = std::min<std::size_t>(userKitIdx0To3, kUserRhythmKitCount - 1u);
    if (!optionalName.empty()) {
        rhythmEditKit_.setName(optionalName);
    }
    userRhythmKits_[idx] = rhythmEditKit_;
    selectedRhythmKit_ = idx + 1u;
}

void VF50MemoryManager::setUserRhythmKit(std::size_t userKitIdx0To3, const RhythmKitBank& kit) noexcept {
    const std::size_t idx = std::min<std::size_t>(userKitIdx0To3, kUserRhythmKitCount - 1u);
    userRhythmKits_[idx] = kit;
    if (selectedRhythmKit_ == (idx + 1u)) {
        rhythmEditKit_ = kit;
    }
}

const RhythmKitBank& VF50MemoryManager::userRhythmKit(std::size_t userKitIdx0To3) const noexcept {
    return userRhythmKits_[std::min<std::size_t>(userKitIdx0To3, kUserRhythmKitCount - 1u)];
}

void VF50MemoryManager::selectRhythmKeyMap(std::size_t keyMapIdx0To4) noexcept {
    const uint8_t idx = static_cast<uint8_t>(std::min<std::size_t>(keyMapIdx0To4, 4u));
    rhythmEditKit_.keyMapSelect = idx;
    if (idx == 3u) {
        // USER / SEQ: Sequential 1..61 across C1..C6 (MIDI 36..96)
        for (std::size_t d = 0; d < vf50::generated::kDrumInstrumentCount; ++d) {
            rhythmEditKit_.instruments[d].midiNote = static_cast<uint8_t>(36u + d);
        }
        return;
    }
    const auto& srcMap = (idx < 3u)
                             ? runtimeRomBank().presetDrumKeyMaps[idx]
                             : userKeyMaps_[static_cast<std::size_t>(idx - 3u)];
    for (std::size_t k = 0; k < srcMap.size(); ++k) {
        const int8_t drumIdx = srcMap[k];
        if (drumIdx >= 0 && static_cast<std::size_t>(drumIdx) < vf50::generated::kDrumInstrumentCount) {
            rhythmEditKit_.instruments[static_cast<std::size_t>(drumIdx)].midiNote = static_cast<uint8_t>(31u + k);
        }
    }
}

void VF50MemoryManager::setUserKeyMap(std::size_t userMapIdx0To1, const std::array<int8_t, 61>& map) noexcept {
    const std::size_t idx = std::min<std::size_t>(userMapIdx0To1, kUserKeyMapCount - 1u);
    userKeyMaps_[idx] = map;
    if (rhythmEditKit_.keyMapSelect == static_cast<uint8_t>(3u + idx)) {
        for (std::size_t k = 0; k < map.size(); ++k) {
            const int8_t drumIdx = map[k];
            if (drumIdx >= 0 && static_cast<std::size_t>(drumIdx) < vf50::generated::kDrumInstrumentCount) {
                rhythmEditKit_.instruments[static_cast<std::size_t>(drumIdx)].midiNote = static_cast<uint8_t>(31u + k);
            }
        }
    }
}

const std::array<int8_t, 61>& VF50MemoryManager::userKeyMap(std::size_t userMapIdx0To1) const noexcept {
    return userKeyMaps_[std::min<std::size_t>(userMapIdx0To1, kUserKeyMapCount - 1u)];
}

void VF50MemoryManager::selectRhythmPattern(MemoryBank bank, std::size_t patternIdx0To99) noexcept {
    selectedPatternBank_ = bank;
    selectedPatternIndex_ = std::min(patternIdx0To99, kRhythmPatternCount - 1u);
}

const UserRhythmPattern& VF50MemoryManager::userRhythmPattern(std::size_t index0To99) const noexcept {
    return userRhythmPatterns_[std::min(index0To99, kRhythmPatternCount - 1u)];
}

UserRhythmPattern& VF50MemoryManager::mutableUserRhythmPattern(std::size_t index0To99) noexcept {
    return userRhythmPatterns_[std::min(index0To99, kRhythmPatternCount - 1u)];
}

void VF50MemoryManager::copyFactoryPatternToUserSlot(std::size_t factoryIdx, std::size_t userIdx) noexcept {
    const auto& rom = runtimeRomBank();
    const std::size_t fIdx = std::min(factoryIdx, vf50::generated::kFactoryRhythmPtnCount - 1u);
    const std::size_t uIdx = std::min(userIdx, kRhythmPatternCount - 1u);
    const auto& srcPtn = rom.factoryRhythmPatterns[fIdx];
    const auto srcEvents = rom.getRhythmPatternEvents(fIdx);

    auto& dst = userRhythmPatterns_[uIdx];
    dst.clear();
    dst.bars       = srcPtn.bars;
    dst.timeSigNum = srcPtn.timeSigNum;
    dst.timeSigDen = srcPtn.timeSigDen;
    dst.totalTicks = srcPtn.totalTicks;

    const std::size_t copyCount = std::min<std::size_t>(srcEvents.size(), UserRhythmPattern::kMaxEvents);
    for (std::size_t e = 0; e < copyCount; ++e) {
        dst.events[e] = srcEvents[e];
    }
    dst.eventCount = static_cast<uint16_t>(copyCount);
}

void VF50MemoryManager::setUserOctaveMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 12>& tbl) noexcept {
    for (std::size_t i = 0; i < 12u; ++i) {
        userOctaveMicrotuning_[i] = {
            static_cast<uint8_t>(tbl[i].coarseNote & 0x7Fu),
            static_cast<uint8_t>(tbl[i].fineStep64 & 0x3Fu)
        };
    }
}

void VF50MemoryManager::setUserFullMicrotuning(const std::array<vf50::generated::MicrotuningEntry, 128>& tbl) noexcept {
    for (std::size_t i = 0; i < 128u; ++i) {
        userFullMicrotuning_[i] = {
            static_cast<uint8_t>(tbl[i].coarseNote & 0x7Fu),
            static_cast<uint8_t>(tbl[i].fineStep64 & 0x3Fu)
        };
    }
}

void VF50MemoryManager::resetUserMicrotuningsToFactory() noexcept {
    for (std::size_t n = 0; n < 12u; ++n) {
        userOctaveMicrotuning_[n] = { static_cast<uint8_t>(60u + n), 0u };
    }
    userFullMicrotuning_ = runtimeRomBank().factoryMicrotunings[0].keys;
}

vf50::generated::DspEffectSettings VF50MemoryManager::activeEffectSettings() const noexcept {
    return (effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Single)
               ? activeSingleVoice().effect
               : activePerformance().effect;
}

void VF50MemoryManager::updateActiveEffectSettings(const vf50::generated::DspEffectSettings& fx) noexcept {
    if (effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Single) {
        singleEditBuffer_.voice.effect = fx;
    } else {
        perfEditBuffer_.perf.effect = fx;
    }
}

void VF50MemoryManager::applyToSynthEngine(vf50::dsp::VF50SynthEngine& engine, bool syncEffects) const noexcept {
    for (std::size_t i = 0; i < kVoiceBankSize; ++i) {
        engine.fm().storeInternalVoice(i, userVoices_[i].voice);
    }
    for (std::size_t i = 0; i < kPerfBankSize; ++i) {
        engine.fm().storeInternalPerformance(i, userPerformances_[i].perf);
    }
    engine.fm().setUserOctaveMicrotuning(userOctaveMicrotuning_);
    engine.fm().setUserFullMicrotuning(userFullMicrotuning_);

    for (std::size_t d = 0; d < vf50::generated::kDrumInstrumentCount; ++d) {
        engine.drums().setInstrumentState(d, rhythmEditKit_.instruments[d]);
    }
    engine.drums().setActiveKeyAssignLayoutTag(std::min<uint8_t>(rhythmEditKit_.keyMapSelect, 3u));
    engine.drums().setRhythmMasterVolume(rhythmEditKit_.rhythmVolume);
    engine.drums().setEffectMode(rhythmEditKit_.effectMode);

    applyActiveSelectionToSynthEngine(engine, syncEffects);
}

void VF50MemoryManager::applyActiveSelectionToSynthEngine(vf50::dsp::VF50SynthEngine& engine, bool syncEffects) const noexcept {
    engine.setRhythmKeyboardMode(mode_ == WorkstationMode::RhythmPlay);

    const auto fmMode = effectiveFmPlayMode();
    engine.setPlayMode(fmMode, false);

    engine.fm().selectSingleVoice(selectedVoiceIndex_, selectedVoiceBank_ == MemoryBank::UserRam);
    engine.fm().setActiveSingleVoiceDirect(activeSingleVoice());
    const vf50::dsp::fm::QuickEditOffsets effectiveQe = singleCompareActive_
                                                           ? vf50::dsp::fm::QuickEditOffsets{ 0, 0, 0, 0 }
                                                           : quickEditOffsets_;
    const uint8_t effectiveMask = singleCompareActive_ ? 0x0Fu : operatorEnableMask_;
    engine.fm().setQuickEditOffsets(effectiveQe);
    engine.fm().setOperatorEnableMask(effectiveMask);
    engine.fm().setSingleEffectSend(singleCompareActive_ ? true : singleEffectSend_);

    const auto& pans = activePerformancePartPans();
    for (std::size_t p = 0; p < 8u; ++p) {
        engine.fm().setPartPan(p, pans[p]);
    }
    engine.fm().selectPerformance(selectedPerfIndex_, selectedPerfBank_ == MemoryBank::UserRam);
    engine.fm().setActivePerformanceDirect(activePerformance());

    if (syncEffects) {
        engine.effects().setSettings(activeEffectSettings());
    }
}

vf50::generated::FactoryVoice VF50MemoryManager::makeInitVoice() noexcept {
    vf50::generated::FactoryVoice v{};
    v.index = 0u;
    v.name = "INIT VOICE";
    v.algorithm = 0u; // ALG 1
    v.feedback = 0u;
    v.lfoSpeed = 35u;
    v.lfoDelay = 0u;
    v.pmd = 0u;
    v.amd = 0u;
    v.lfoSync = 0u;
    v.lfoWave = 2u; // Triangle
    v.pms = 3u;
    v.ams = 0u;
    v.transpose = 0; // C3
    v.monoMode = 0u;
    v.pitchBendRange = 4u;
    v.portaMode = 0u;
    v.portaSwitch = 0u;
    v.portaTime = 0u;
    v.sustainSwitch = 1u;
    v.chorusSwitch = 0u;
    v.reverbRate = 0u;

    v.fcVolume = 99u;
    v.fcPitch = 0u;
    v.fcAmp = 0u;
    v.mwPitch = 50u;
    v.mwAmp = 0u;
    v.bcPitch = 0u;
    v.bcAmp = 0u;
    v.bcPitchBias = 0;
    v.bcEgBias = 0u;
    v.atPitch = 0u;
    v.atAmp = 0u;
    v.atPitchBias = 0;
    v.atEgBias = 0u;

    v.peg = { 99u, 99u, 99u, 50u, 50u, 50u };

    for (std::size_t op = 0; op < 4u; ++op) {
        auto& o = v.operators[op];
        o.ar = 31u;
        o.d1r = 0u;
        o.d2r = 0u;
        o.rr = 15u;
        o.d1l = 15u;
        o.levelScaling = 0;
        o.rateScaling = 0u;
        o.egBiasSens = 0u;
        o.ampModEnable = 0u;
        o.keyVelSens = 0;
        o.outputLevel = (op == 0u) ? 99u : 0u; // Pure sine carrier OP1 at 99, modulators at 0
        o.coarse = 2u; // Ratio 1.00
        o.fine = 0u;
        o.detune = 0;
        o.waveform = 0u; // W1 Sine
        o.egShift = 0u;
        o.fixedFreqMode = 0u;
        o.fixedRange = 0u;
        o.fixedRangeLow = 0u;
    }

    v.effect = { 0u, 50u, 100u, 1u, 20u, 10u, 15u };
    const std::string_view literalName = v.name;
    StoredVoiceSlot tmp(v);
    tmp.voice.name = literalName;
    return tmp.voice;
}

vf50::generated::FactoryPerformance VF50MemoryManager::makeInitPerformance(PerfInitTemplate tmpl) noexcept {
    vf50::generated::FactoryPerformance pf{};
    pf.index = 0u;
    pf.name = "INIT PERF ";
    pf.assignMode = 2u; // DVA default
    pf.microTuneTable = 0u;
    pf.microTuneRootKey = 0u;
    pf.perfEffectSelect = 0u;
    pf.effect = { 1u, 40u, 85u, 1u, 20u, 10u, 15u };

    const vf50::generated::PerformanceInstrument disabledInst{
        0u, 0u, 1u, 0u, 17u, 0u, 127u, 0, 0, 0u, 0u, 0u, 0u, 0u
    };
    pf.instruments.fill(disabledInst);

    switch (tmpl) {
        case PerfInitTemplate::All:
        case PerfInitTemplate::Single:
            pf.assignMode = 2u;
            pf.instruments[0] = { 16u, 0u, 1u, 0u, 16u, 0u, 127u, 0, 0, 99u, 3u, 1u, 0u, 1u };
            break;

        case PerfInitTemplate::Dual:
            pf.name = "INIT DUAL ";
            pf.assignMode = 0u;
            pf.instruments[0] = { 8u, 0u, 1u, 0u, 16u, 0u, 127u, -2, 0, 95u, 1u, 1u, 0u, 1u };
            pf.instruments[1] = { 8u, 0u, 1u, 0u, 16u, 0u, 127u,  2, 0, 95u, 2u, 2u, 0u, 1u };
            break;

        case PerfInitTemplate::Split:
            pf.name = "INIT SPLIT";
            pf.assignMode = 2u;
            pf.instruments[0] = { 8u, 5u, 1u, 20u, 16u,  0u,  59u, 0, 12, 95u, 3u, 1u, 0u, 0u };
            pf.instruments[1] = { 8u, 9u, 1u,  0u, 16u, 60u, 127u, 0,  0, 95u, 3u, 2u, 0u, 1u };
            break;

        case PerfInitTemplate::MCh8x2:
            pf.name = "INIT MC8-2";
            pf.assignMode = 0u;
            for (std::size_t i = 0; i < 8u; ++i) {
                pf.instruments[i] = {
                    2u, 0u, 1u, static_cast<uint8_t>(i), static_cast<uint8_t>(i),
                    0u, 127u, 0, 0, 95u, 3u, static_cast<uint8_t>((i == 1u) ? 2u : 1u), 0u, 1u
                };
            }
            break;

        case PerfInitTemplate::MCh4x4:
            pf.name = "INIT MC4-4";
            pf.assignMode = 0u;
            for (std::size_t i = 0; i < 4u; ++i) {
                pf.instruments[i] = {
                    4u, 0u, 1u, static_cast<uint8_t>(i), static_cast<uint8_t>(i),
                    0u, 127u, 0, 0, 95u, 3u, static_cast<uint8_t>((i == 1u) ? 2u : 1u), 0u, 1u
                };
            }
            break;

        case PerfInitTemplate::Poly4:
            pf.name = "INIT POLY4";
            pf.assignMode = 1u; // Alternate round-robin
            for (std::size_t i = 0; i < 4u; ++i) {
                pf.instruments[i] = {
                    4u, 0u, 1u, static_cast<uint8_t>(i), 16u,
                    0u, 127u, 0, 0, 95u, 3u, 1u, 0u, 1u
                };
            }
            break;

        case PerfInitTemplate::Layer4:
            pf.name = "INIT 4LYR ";
            pf.assignMode = 0u;
            for (std::size_t i = 0; i < 4u; ++i) {
                pf.instruments[i] = {
                    4u, 0u, 1u, static_cast<uint8_t>(i), 16u,
                    0u, 127u, 0, 0, 95u, 3u, static_cast<uint8_t>((i == 1u) ? 2u : 1u), 0u, 1u
                };
            }
            break;

        case PerfInitTemplate::Layer8:
            pf.name = "INIT 8LYR ";
            pf.assignMode = 0u;
            for (std::size_t i = 0; i < 8u; ++i) {
                pf.instruments[i] = {
                    2u, 0u, 1u, static_cast<uint8_t>(i), 16u,
                    0u, 127u, 0, 0, 95u, 3u, static_cast<uint8_t>((i == 1u) ? 2u : 1u), 0u, 1u
                };
            }
            break;
    }

    const std::string_view literalName = pf.name;
    StoredPerformanceSlot tmp(pf);
    tmp.perf.name = literalName;
    return tmp.perf;
}

// =============================================================================
// Binary DAW State Serialization / Deserialization
// =============================================================================

void VF50MemoryManager::serializeToBinary(std::vector<uint8_t>& outBytes) const {
    outBytes.clear();
    outBytes.reserve(96u * 1024u);

    std::vector<uint8_t> payload;
    payload.reserve(90u * 1024u);
    ByteWriter w{ payload };

    // 1. Workstation Mode & Active Selections
    w.writeU8(static_cast<uint8_t>(mode_));
    w.writeU8(static_cast<uint8_t>(underlyingFmMode_));
    w.writeU8(static_cast<uint8_t>(selectedVoiceBank_));
    w.writeU16(static_cast<uint16_t>(selectedVoiceIndex_));
    w.writeU8(singleCompareActive_ ? 1u : 0u);

    w.writeU8(static_cast<uint8_t>(selectedPerfBank_));
    w.writeU16(static_cast<uint16_t>(selectedPerfIndex_));
    w.writeU8(perfCompareActive_ ? 1u : 0u);

    w.writeU8(static_cast<uint8_t>(selectedRhythmKit_));
    w.writeU8(static_cast<uint8_t>(selectedPatternBank_));
    w.writeU16(static_cast<uint16_t>(selectedPatternIndex_));

    auto writeVoiceSlot = [&](const StoredVoiceSlot& slot) {
        StoredVoiceSlot copy = slot;
        copy.repackRomBytes();
        w.writeBytes(copy.voice.rawRomBytes.data(), copy.voice.rawRomBytes.size());
    };
    auto writePerfSlot = [&](const StoredPerformanceSlot& slot) {
        StoredPerformanceSlot copy = slot;
        copy.repackRomBytes();
        w.writeBytes(copy.perf.rawRomBytes.data(), copy.perf.rawRomBytes.size());
        for (int8_t pan : copy.partPans) {
            w.writeI8(pan);
        }
    };

    // 2. Edit & Compare Buffers
    writeVoiceSlot(singleEditBuffer_);
    writeVoiceSlot(singleCompareBackup_);
    writePerfSlot(perfEditBuffer_);
    writePerfSlot(perfCompareBackup_);

    // 3. 100 User Single Voices + 100 User Performances
    for (std::size_t i = 0; i < kVoiceBankSize; ++i) {
        writeVoiceSlot(userVoices_[i]);
    }
    for (std::size_t i = 0; i < kPerfBankSize; ++i) {
        writePerfSlot(userPerformances_[i]);
    }

    // 4. Rhythm Edit Kit + 4 User Rhythm Kits + 2 User Key Maps
    auto writeKit = [&](const RhythmKitBank& kit) {
        w.writeBytes(kit.nameChars.data(), kit.nameChars.size());
        w.writeU8(kit.rhythmVolume);
        w.writeU8(static_cast<uint8_t>(kit.effectMode));
        w.writeU8(kit.keyMapSelect);
        for (const auto& d : kit.instruments) {
            w.writeU8(d.volume);
            w.writeI8(d.pan);
            w.writeF32(d.pitchSemitones);
            w.writeU8(d.accent);
            w.writeU8(d.outAssign);
            w.writeU8(d.midiNote);
        }
    };
    writeKit(rhythmEditKit_);
    for (std::size_t k = 0; k < kUserRhythmKitCount; ++k) {
        writeKit(userRhythmKits_[k]);
    }
    for (std::size_t m = 0; m < kUserKeyMapCount; ++m) {
        w.writeBytes(userKeyMaps_[m].data(), userKeyMaps_[m].size());
    }

    // 5. 100 User Rhythm Patterns (compact variable-length event encoding)
    for (std::size_t p = 0; p < kRhythmPatternCount; ++p) {
        const auto& ptn = userRhythmPatterns_[p];
        w.writeU8(ptn.bars);
        w.writeU8(ptn.timeSigNum);
        w.writeU8(ptn.timeSigDen);
        w.writeU16(ptn.totalTicks);
        const uint16_t count = std::min<uint16_t>(ptn.eventCount, static_cast<uint16_t>(UserRhythmPattern::kMaxEvents));
        w.writeU16(count);
        for (uint16_t e = 0u; e < count; ++e) {
            w.writeU16(ptn.events[e].tick);
            w.writeU8(ptn.events[e].drumIndex);
            w.writeU8(ptn.events[e].velocityLevel);
        }
    }

    // 6. User Microtuning Tables (12-note Octave + 128-key Full)
    for (const auto& entry : userOctaveMicrotuning_) {
        w.writeU8(entry.coarseNote);
        w.writeU8(entry.fineStep64);
    }
    for (const auto& entry : userFullMicrotuning_) {
        w.writeU8(entry.coarseNote);
        w.writeU8(entry.fineStep64);
    }

    // Frame with Magic + Version + Payload Size + CRC32
    ByteWriter header{ outBytes };
    header.writeBytes(kMagic, 4u);
    header.writeU32(kBinaryFormatVersion);
    header.writeU32(static_cast<uint32_t>(payload.size()));
    header.writeU32(computeCrc32(payload.data(), payload.size()));
    header.writeBytes(payload.data(), payload.size());
}

bool VF50MemoryManager::deserializeFromBinary(const uint8_t* data, std::size_t numBytes) noexcept {
    if (data == nullptr || numBytes < 16u) {
        return false;
    }

    ByteReader hdr{ data, numBytes, 0u };
    uint8_t magic[4]{};
    uint32_t version = 0u;
    uint32_t payloadLen = 0u;
    uint32_t expectedCrc = 0u;
    if (!hdr.readBytes(magic, 4u) || !hdr.readU32(version) || !hdr.readU32(payloadLen) || !hdr.readU32(expectedCrc)) {
        return false;
    }
    if (std::memcmp(magic, kMagic, 4u) != 0 || version != kBinaryFormatVersion) {
        return false;
    }
    if (!hdr.hasRemaining(payloadLen)) {
        return false;
    }
    const uint8_t* payloadPtr = data + hdr.pos;
    if (computeCrc32(payloadPtr, payloadLen) != expectedCrc) {
        return false;
    }

    ByteReader r{ payloadPtr, payloadLen, 0u };

    uint8_t rawMode = 0u, rawFmMode = 0u, rawVoiceBank = 0u, rawSingleCmp = 0u;
    uint8_t rawPerfBank = 0u, rawPerfCmp = 0u, rawKit = 0u, rawPtnBank = 0u;
    uint16_t rawVoiceIdx = 0u, rawPerfIdx = 0u, rawPtnIdx = 0u;

    if (!r.readU8(rawMode) || !r.readU8(rawFmMode)
        || !r.readU8(rawVoiceBank) || !r.readU16(rawVoiceIdx) || !r.readU8(rawSingleCmp)
        || !r.readU8(rawPerfBank) || !r.readU16(rawPerfIdx) || !r.readU8(rawPerfCmp)
        || !r.readU8(rawKit) || !r.readU8(rawPtnBank) || !r.readU16(rawPtnIdx)) {
        return false;
    }

    auto readVoiceSlot = [&](StoredVoiceSlot& slot, uint8_t idx) noexcept -> bool {
        std::array<uint8_t, 95> raw{};
        if (!r.readBytes(raw.data(), raw.size())) return false;
        unpackVoiceFromRom95(raw, slot.voice, slot.nameChars);
        slot.voice.index = idx;
        return true;
    };

    auto readPerfSlot = [&](StoredPerformanceSlot& slot, uint8_t idx) noexcept -> bool {
        std::array<uint8_t, 97> raw{};
        if (!r.readBytes(raw.data(), raw.size())) return false;
        unpackPerfFromRom97(raw, slot.perf, slot.nameChars);
        slot.perf.index = idx;
        for (std::size_t p = 0; p < 8u; ++p) {
            int8_t pan = 0;
            if (!r.readI8(pan)) return false;
            slot.partPans[p] = static_cast<int8_t>(std::clamp<int>(pan, -64, 63));
        }
        return true;
    };

    if (!readVoiceSlot(singleEditBuffer_, static_cast<uint8_t>(std::min<uint16_t>(rawVoiceIdx, 99u)))
        || !readVoiceSlot(singleCompareBackup_, static_cast<uint8_t>(std::min<uint16_t>(rawVoiceIdx, 99u)))
        || !readPerfSlot(perfEditBuffer_, static_cast<uint8_t>(std::min<uint16_t>(rawPerfIdx, 99u)))
        || !readPerfSlot(perfCompareBackup_, static_cast<uint8_t>(std::min<uint16_t>(rawPerfIdx, 99u)))) {
        return false;
    }

    for (std::size_t i = 0; i < kVoiceBankSize; ++i) {
        if (!readVoiceSlot(userVoices_[i], static_cast<uint8_t>(i))) return false;
    }
    for (std::size_t i = 0; i < kPerfBankSize; ++i) {
        if (!readPerfSlot(userPerformances_[i], static_cast<uint8_t>(i))) return false;
    }

    auto readKit = [&](RhythmKitBank& kit) noexcept -> bool {
        std::array<char, 10> nm{};
        uint8_t rVol = 85u, rFx = 2u, rMap = 0u;
        if (!r.readBytes(nm.data(), nm.size()) || !r.readU8(rVol) || !r.readU8(rFx) || !r.readU8(rMap)) {
            return false;
        }
        kit.setName(std::string_view(nm.data(), nm.size()));
        kit.rhythmVolume = std::min<uint8_t>(rVol, 99u);
        kit.effectMode = static_cast<vf50::dsp::drums::RhythmEffectMode>(std::min<uint8_t>(rFx, 2u));
        kit.keyMapSelect = std::min<uint8_t>(rMap, 4u);
        for (std::size_t d = 0; d < vf50::generated::kDrumInstrumentCount; ++d) {
            uint8_t vol = 15u, acc = 5u, oa = 3u, note = 36u;
            int8_t pan = 0;
            float pitch = 0.0f;
            if (!r.readU8(vol) || !r.readI8(pan) || !r.readF32(pitch)
                || !r.readU8(acc) || !r.readU8(oa) || !r.readU8(note)) {
                return false;
            }
            auto& inst = kit.instruments[d];
            inst.volume = std::min<uint8_t>(vol, 15u);
            inst.pan = static_cast<int8_t>(std::clamp<int>(pan, -3, 3));
            inst.pitchSemitones = std::clamp(pitch, -24.0f, 24.0f);
            inst.accent = std::clamp<uint8_t>(acc, 1u, 9u);
            inst.outAssign = std::min<uint8_t>(oa, 3u);
            inst.midiNote = note & 0x7Fu;
            inst.chokeGroup = runtimeRomBank().defaultDrumSetup[d].chokeGroup;
        }
        return true;
    };

    if (!readKit(rhythmEditKit_)) return false;
    for (std::size_t k = 0; k < kUserRhythmKitCount; ++k) {
        if (!readKit(userRhythmKits_[k])) return false;
    }
    for (std::size_t m = 0; m < kUserKeyMapCount; ++m) {
        if (!r.readBytes(userKeyMaps_[m].data(), userKeyMaps_[m].size())) return false;
        for (auto& entry : userKeyMaps_[m]) {
            entry = static_cast<int8_t>(std::clamp<int>(entry, -1, 60));
        }
    }

    for (std::size_t p = 0; p < kRhythmPatternCount; ++p) {
        auto& ptn = userRhythmPatterns_[p];
        ptn.clear();
        uint16_t count = 0u;
        if (!r.readU8(ptn.bars) || !r.readU8(ptn.timeSigNum) || !r.readU8(ptn.timeSigDen)
            || !r.readU16(ptn.totalTicks) || !r.readU16(count)) {
            return false;
        }
        ptn.bars = std::clamp<uint8_t>(ptn.bars, 1u, 8u);
        ptn.timeSigNum = std::clamp<uint8_t>(ptn.timeSigNum, 1u, 32u);
        ptn.timeSigDen = std::clamp<uint8_t>(ptn.timeSigDen, 2u, 16u);
        ptn.totalTicks = std::max<uint16_t>(ptn.totalTicks, 24u);
        const uint16_t safeCount = std::min<uint16_t>(count, static_cast<uint16_t>(UserRhythmPattern::kMaxEvents));
        for (uint16_t e = 0u; e < count; ++e) {
            uint16_t tick = 0u;
            uint8_t drumIdx = 0u, vel = 5u;
            if (!r.readU16(tick) || !r.readU8(drumIdx) || !r.readU8(vel)) {
                return false;
            }
            if (e < safeCount) {
                ptn.events[e] = {
                    std::min<uint16_t>(tick, static_cast<uint16_t>(ptn.totalTicks - 1u)),
                    std::min<uint8_t>(drumIdx, static_cast<uint8_t>(vf50::generated::kDrumInstrumentCount - 1u)),
                    std::clamp<uint8_t>(vel, 1u, 9u),
                    0u
                };
            }
        }
        ptn.eventCount = safeCount;
    }

    for (auto& entry : userOctaveMicrotuning_) {
        uint8_t c = 60u, f = 0u;
        if (!r.readU8(c) || !r.readU8(f)) return false;
        entry = { static_cast<uint8_t>(c & 0x7Fu), static_cast<uint8_t>(f & 0x3Fu) };
    }
    for (auto& entry : userFullMicrotuning_) {
        uint8_t c = 60u, f = 0u;
        if (!r.readU8(c) || !r.readU8(f)) return false;
        entry = { static_cast<uint8_t>(c & 0x7Fu), static_cast<uint8_t>(f & 0x3Fu) };
    }

    mode_ = static_cast<WorkstationMode>(std::min<uint8_t>(rawMode, 5u));
    underlyingFmMode_ = (rawFmMode == 0u) ? vf50::dsp::fm::SynthPlayMode::Single : vf50::dsp::fm::SynthPlayMode::Performance;
    selectedVoiceBank_ = (rawVoiceBank == 0u) ? MemoryBank::FactoryRom : MemoryBank::UserRam;
    selectedVoiceIndex_ = std::min<std::size_t>(rawVoiceIdx, kVoiceBankSize - 1u);
    singleCompareActive_ = (rawSingleCmp != 0u);

    selectedPerfBank_ = (rawPerfBank == 0u) ? MemoryBank::FactoryRom : MemoryBank::UserRam;
    selectedPerfIndex_ = std::min<std::size_t>(rawPerfIdx, kPerfBankSize - 1u);
    perfCompareActive_ = (rawPerfCmp != 0u);

    selectedRhythmKit_ = std::min<std::size_t>(rawKit, kUserRhythmKitCount);
    selectedPatternBank_ = (rawPtnBank == 0u) ? MemoryBank::FactoryRom : MemoryBank::UserRam;
    selectedPatternIndex_ = std::min<std::size_t>(rawPtnIdx, kRhythmPatternCount - 1u);

    return true;
}

} // namespace vf50::state
