#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vf50::dsp::fm {

inline constexpr double kOpzNativeSampleRate = 50000.0;
inline constexpr double kOpzEgClockRate      = kOpzNativeSampleRate / 3.0; // Hardware EG ticks every 3 FM frames
inline constexpr double kPi                  = 3.14159265358979323846;

// Exact YM2151/YM2414/YM2424 DT1 linear phase-increment delta table (32 key-codes x 4 detune magnitudes)
inline constexpr std::array<std::array<uint8_t, 4>, 32> kOpzDt1Table = {{
    {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 0, 1, 2},
    {0, 1, 2, 2}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3},
    {0, 1, 2, 4}, {0, 1, 3, 4}, {0, 1, 3, 4}, {0, 1, 3, 5},
    {0, 2, 4, 5}, {0, 2, 4, 6}, {0, 2, 4, 6}, {0, 2, 5, 7},
    {0, 2, 5, 8}, {0, 3, 6, 8}, {0, 3, 6, 9}, {0, 3, 7, 10},
    {0, 4, 8, 11}, {0, 4, 8, 12}, {0, 4, 9, 13}, {0, 5, 10, 14},
    {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20},
    {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}
}};

// Clean-room mathematical OPZ II Coarse Frequency ordering & low-level TL curve (overridden at runtime when IC712/IC731 load)
consteval std::array<uint8_t, 64> computeCleanOpzCoarsePermTable() noexcept {
    constexpr std::array<uint32_t, 4> kDt2Scaled = { 1000u, 1414u, 1570u, 1731u };
    std::array<uint8_t, 64> perm{};
    for (uint8_t i = 0; i < 64u; ++i) {
        perm[i] = i;
    }
    auto score = [&](uint8_t c) noexcept -> uint32_t {
        const uint32_t mult = (c >> 2u);
        const uint32_t base = (mult == 0u) ? 5u : (mult * 10u);
        return base * kDt2Scaled[c & 0x03u];
    };
    for (std::size_t i = 0; i < 64u; ++i) {
        for (std::size_t j = i + 1u; j < 64u; ++j) {
            const uint32_t si = score(perm[i]);
            const uint32_t sj = score(perm[j]);
            if (sj < si || (sj == si && perm[j] < perm[i])) {
                const uint8_t tmp = perm[i];
                perm[i] = perm[j];
                perm[j] = tmp;
            }
        }
    }
    return perm;
}

inline constexpr std::array<uint8_t, 64> kOpzCoarsePermTable = computeCleanOpzCoarsePermTable();

inline constexpr std::array<uint8_t, 20> kLowOutLevelToTl = {
    127, 121, 116, 112, 108, 105, 102, 100, 98, 96,
    94,  92,  90,  89,  87,  86,  85,  83,  82, 81
};

// Hardware OPZ II Envelope Rate (0..31) -> base 6-bit internal rate index (before Key Scale Rate addition)
inline constexpr std::array<uint8_t, 32> kOpzRateBaseTable = {
     0,  2,  4,  6,  8, 10, 12, 14,
    16, 18, 20, 22, 24, 26, 28, 30,
    32, 34, 36, 38, 40, 42, 44, 46,
    48, 50, 52, 54, 56, 58, 60, 62
};

// Hardware OPZ II Release Rate (0..15) -> base 6-bit internal rate index (4 * RR + 2)
inline constexpr std::array<uint8_t, 16> kOpzReleaseRateBaseTable = {
     2,  6, 10, 14, 18, 22, 26, 30,
    34, 38, 42, 46, 50, 54, 58, 62
};

// YM2424 8-step EG sub-cycle increment table indexed by effective rate mantissa (0..3) and high-rate clamp
inline constexpr std::array<uint8_t, 4> kOpzEgMantissaQuarter = { 4u, 5u, 6u, 7u };

struct OpzHardwareTables {
    std::array<uint16_t, 256> logSin{};         // 4.8 fixed-point attenuation in 1/256 octave
    std::array<uint16_t, 256> exp2{};           // 10-bit mantissa (0..1023) for 2^-x reconstruction
    std::array<int32_t, 64>   egDecayStep24{};  // Logarithmic attenuation step per 50 kHz sample (10.14 fixed-point)
    std::array<int32_t, 64>   egAttackStep24{}; // Concave exponential attack rate step (10.14 fixed-point)
    std::array<int32_t, 32>   rate0To31DecayStep24{}; // Direct lookup for unscaled rates 0..31 (KSR = 0)
    std::array<double, 100>   lfoHz{};          // LFO Speed 0..99 -> Hz
    std::array<double, 100>   pegSemi{};        // Pitch EG Level 0..99 -> semitones (50 = 0.0)

    OpzHardwareTables() noexcept {
        for (int i = 0; i < 256; ++i) {
            const double angle = (static_cast<double>(i) + 0.5) * kPi / 512.0;
            const double ls = -std::log2(std::sin(angle)) * 256.0;
            logSin[static_cast<std::size_t>(i)] = static_cast<uint16_t>(std::clamp(std::lround(ls), 0L, 4095L));

            const double ex = (std::exp2(static_cast<double>(255 - i) / 256.0) - 1.0) * 1024.0;
            exp2[static_cast<std::size_t>(i)] = static_cast<uint16_t>(std::clamp(std::lround(ex), 0L, 1023L));
        }

        // Exact YM2151/YM2424 OPZ II logarithmic attenuation rate table:
        // Hardware EG advances its 10-bit log-attenuation counter (0..1023, 0.09375 dB/LSB)
        // at fs / 3 = 16,666.67 Hz using mantissa {4,5,6,7}/4 * 2^((R_eff >> 2) - 11) LSBs per EG tick.
        // In 10.14 fixed-point (1 LSB = 16,384) at 50,000 Hz, 1 EG tick = 3 samples:
        //   step24 = round((mantissa * 2^(R_eff >> 2) * 8) / 3), clamped at R_eff >= 60 (8 LSBs / EG tick).
        egDecayStep24[0]  = 0;
        egDecayStep24[1]  = 0;
        egAttackStep24[0] = 0;
        egAttackStep24[1] = 0;
        for (int r = 2; r < 64; ++r) {
            const int effR = std::min(r, 60);
            const uint32_t shift = static_cast<uint32_t>(effR >> 2);
            const uint32_t mant  = (r >= 60) ? 4u : kOpzEgMantissaQuarter[static_cast<std::size_t>(r & 0x03)];
            // Exact hardware rate scaled by 1/3 for 50 kHz per-sample continuous updates
            const double rawStep24 = (static_cast<double>(mant) * std::ldexp(1.0, static_cast<int>(shift) + 1)) / 3.0;
            egDecayStep24[static_cast<std::size_t>(r)] = std::max<int32_t>(1, static_cast<int32_t>(std::lround(rawStep24)));

            // Hardware attack accelerates ~6.5x relative to linear-dB decay via (~env * inc) >> 4
            const double atkStep24 = rawStep24 * 1.15;
            egAttackStep24[static_cast<std::size_t>(r)] = std::max<int32_t>(2, static_cast<int32_t>(std::lround(atkStep24)));
        }

        for (std::size_t rate = 0; rate < 32u; ++rate) {
            rate0To31DecayStep24[rate] = egDecayStep24[kOpzRateBaseTable[rate]];
        }

        for (int s = 0; s < 100; ++s) {
            // YM2424 LFO speed curve: 0 -> 0.0625 Hz, 33 -> ~5.5 Hz, 99 -> 50.0 Hz
            if (s == 0) {
                lfoHz[0] = 0.0625;
            } else {
                const double norm = static_cast<double>(s) / 99.0;
                lfoHz[static_cast<std::size_t>(s)] = 0.0625 * std::pow(800.0, norm);
            }
        }

        // TX81Z/DX11/VF50 Pitch EG level curve: 50 = 0 semitones, +-48 semitones at extremes (1 step = 1 semitone)
        for (int i = 0; i < 100; ++i) {
            pegSemi[static_cast<std::size_t>(i)] = std::clamp(static_cast<double>(i - 50), -48.0, 48.0);
        }
    }

    // Evaluate any of the 8 OPZ II waveforms (W1..W8, index 0..7) at 10-bit phase [0..1023]
    // and 10-bit total envelope attenuation [0..1023] (where 1023 = -96 dB).
    // Returns signed 13-bit linear operator output in [-4094, +4094].
    [[nodiscard]] int32_t lookupWaveform(uint8_t waveform, uint16_t phase10, uint16_t egAtten10) const noexcept {
        const uint16_t p = phase10 & 0x3FFu;
        const uint8_t wf = waveform & 0x07u;

        // Waveforms W3..W8 (indices 2..7) mute completely on the second half-period [pi, 2*pi)
        if (wf >= 2u && (p & 0x200u) != 0u) {
            return 0;
        }

        uint16_t lookupPhase = p;
        bool negative = false;

        switch (wf) {
            case 0u: // W1: Full Sine
            case 1u: // W2: Bipolar Squared Sine
                lookupPhase = p;
                negative = (p & 0x200u) != 0u;
                break;

            case 2u: // W3: Half-Wave Sine
            case 3u: // W4: Half-Wave Squared Sine
                lookupPhase = p;
                negative = false;
                break;

            case 4u: // W5: Alternating Double-Freq Sine
            case 5u: // W6: Alternating Double-Freq Squared Sine
                lookupPhase = static_cast<uint16_t>((p << 1u) & 0x3FFu);
                negative = (lookupPhase & 0x200u) != 0u;
                break;

            case 6u: // W7: Rectified Double-Freq Sine
            case 7u: // W8: Rectified Double-Freq Squared Sine
                lookupPhase = static_cast<uint16_t>((p << 1u) & 0x3FFu);
                negative = false;
                break;
        }

        const uint8_t quarterIdx = (lookupPhase & 0x100u)
                                       ? static_cast<uint8_t>((~lookupPhase) & 0xFFu)
                                       : static_cast<uint8_t>(lookupPhase & 0xFFu);

        // Odd waveform indices (W2=1, W4=3, W6=5, W8=7) use sin^2(theta) -> 2x log-sine attenuation
        uint32_t waveAtten12 = logSin[quarterIdx];
        if ((wf & 0x01u) != 0u) {
            waveAtten12 <<= 1u;
        }

        const uint32_t totalAtten12 = waveAtten12 + (static_cast<uint32_t>(std::min<uint16_t>(egAtten10, 1023u)) << 2u);
        if (totalAtten12 >= (13u << 8u)) {
            return 0;
        }

        const uint32_t mantissa = (static_cast<uint32_t>(exp2[totalAtten12 & 0xFFu]) | 0x400u) << 1u;
        const int32_t linearMag = static_cast<int32_t>(mantissa >> (totalAtten12 >> 8u));
        return negative ? -linearMag : linearMag;
    }
};

[[nodiscard]] inline const OpzHardwareTables& getOpzTables() noexcept {
    static const OpzHardwareTables kInstance{};
    return kInstance;
}

// Exact YM2424 (OPZ II) Ratio Multiplier from Coarse (0..63) and Fine (0..15)
[[nodiscard]] inline double computeOpzRatioMultiplier(uint8_t coarse, uint8_t fine,
                                                      const uint8_t* coarsePermOverride = nullptr) noexcept {
    // Silicon DT2 family ratios in 1/768-octave units: {0, 384 (sqrt2), 500 (~pi/2), 608 (~sqrt3)}
    static constexpr std::array<double, 4> kDt2FamilyMultiplier = {
        1.0,
        1.4142135623730951, // 2^(384/768)
        1.5704582406636498, // 2^(500/768)
        1.7310731496594726  // 2^(608/768)
    };

    const uint8_t* permTbl = (coarsePermOverride != nullptr) ? coarsePermOverride : kOpzCoarsePermTable.data();
    const uint8_t code = permTbl[coarse & 0x3Fu];
    const uint8_t fam = code & 0x03u;
    const uint8_t mult = code >> 2u;
    const uint8_t f = fine & 0x0Fu;

    const double harmonicFactor = (mult == 0u)
                                      ? (static_cast<double>(8u + f) / 16.0)
                                      : (static_cast<double>(static_cast<uint32_t>(mult) * 16u + f) / 16.0);
    return harmonicFactor * kDt2FamilyMultiplier[fam];
}

// Exact VF50 Firmware Fixed Frequency computation (IC712 Bank 3 $AD2C..$ADDF):
//   - Coarse group = (coarse & 0x3F) >> 2 (0..15), Fine = fine & 0x0F (0..15).
//   - When coarseGroup == 0 (CRS < 4), firmware ($AD3A: LDAB #$08) uses base 8 + fine (8..23);
//     otherwise base is (coarseGroup << 4) + fine (16..255).
//   - Shift = HI (fixedRangeLow == 0): Hz = mantissa * 2^range (8 Hz .. 32,640 Hz).
//   - Shift = LO (fixedRangeLow != 0, OPZ II 0 Hz / Range Low mode, $AD6A..$AD8C):
//     At range == 0 with mantissa <= 15 (CRS < 4, FINE <= 7), frequency is exact 0.00 Hz stationary;
//     otherwise firmware scales by 171 / 51200 (~1/300) -> 0.05 Hz .. 109.01 Hz.
[[nodiscard]] inline double computeOpzFixedFreqHz(uint8_t coarse, uint8_t fine,
                                                  uint8_t fixedRange, uint8_t fixedRangeLow) noexcept {
    const uint32_t coarseGroup = static_cast<uint32_t>(coarse & 0x3Fu) >> 2u;
    const uint32_t f = static_cast<uint32_t>(fine & 0x0Fu);
    const uint32_t mantissa = (coarseGroup == 0u) ? (8u + f) : ((coarseGroup << 4u) + f);
    const uint32_t range = fixedRange & 0x07u;

    if (fixedRangeLow != 0u) {
        if (range == 0u && mantissa <= 15u) {
            return 0.0;
        }
        return static_cast<double>(mantissa << range) * (171.0 / 51200.0);
    }

    return static_cast<double>(mantissa << range);
}

// Exact YM2424 DT1 linear detune offset in Hz for detune in [-3, +3]
[[nodiscard]] inline double computeOpzDetuneHz(double midiNote, int8_t detune) noexcept {
    if (detune == 0) {
        return 0.0;
    }
    const int kc5 = std::clamp(static_cast<int>((midiNote - 12.0) * (32.0 / 96.0)), 0, 31);
    const int magIdx = std::clamp(static_cast<int>(std::abs(detune)), 0, 3);
    // In OPM/OPZ at 50 kHz, 1 DT1 LSB corresponds to 50000 / 2^20 = 0.0476837 Hz (VF50 manual: +-2.6c at C3 for DTN +-3)
    const double deltaHz = static_cast<double>(kOpzDt1Table[static_cast<std::size_t>(kc5)][static_cast<std::size_t>(magIdx)])
                           * (kOpzNativeSampleRate / 1048576.0);
    return (detune > 0) ? deltaHz : -deltaHz;
}

// Output Level (0..99) -> 7-bit hardware Total Level register TL (0..127, exact 0.75 dB/step)
[[nodiscard]] inline uint8_t outputLevelToTl7(int outLevel,
                                              const uint8_t* lowOutTlOverride = nullptr) noexcept {
    const int clamped = std::clamp(outLevel, 0, 99);
    const uint8_t* tlTbl = (lowOutTlOverride != nullptr) ? lowOutTlOverride : kLowOutLevelToTl.data();
    return (clamped < 20)
               ? tlTbl[static_cast<std::size_t>(clamped)]
               : static_cast<uint8_t>(99 - clamped);
}

// 7-bit hardware Total Level TL (0..127) -> 10-bit EG attenuation (0..1023).
// Each TL step adds exactly 8 units in 10-bit binary log2 attenuation (32/256 octave = 0.75 dB/step).
[[nodiscard]] inline constexpr uint16_t tl7ToAtten10(uint8_t tl7) noexcept {
    const uint16_t clampedTl = (tl7 > 127u) ? 127u : static_cast<uint16_t>(tl7);
    const uint16_t atten10 = static_cast<uint16_t>(clampedTl * 8u);
    return (atten10 > 1023u) ? 1023u : atten10;
}

// Output Level (0..99) -> 10-bit EG attenuation (0..1023) following exact 0.75 dB/step TL curve
[[nodiscard]] inline uint16_t outputLevelToAtten10(int outLevel,
                                                   const uint8_t* lowOutTlOverride = nullptr) noexcept {
    return tl7ToAtten10(outputLevelToTl7(outLevel, lowOutTlOverride));
}

// D1L (0..15) -> 10-bit EG target attenuation (D1L=15 -> 0 dB, D1L=1..14 -> -3..-42 dB, D1L=0 -> -96 dB)
[[nodiscard]] inline uint16_t d1lToAtten10(uint8_t d1l) noexcept {
    const uint8_t clamped = d1l & 0x0Fu;
    if (clamped == 0u) {
        return 1023u;
    }
    return static_cast<uint16_t>((15u - clamped) * 32u);
}

// PMS (0..7) -> total pitch modulation range in cents at PMD = 99 (hardware spec: 0,5,10,20,50,100,400,700)
[[nodiscard]] inline double pmsToMaxCents(uint8_t pms) noexcept {
    static constexpr std::array<double, 8> kPmsCents = {
        0.0, 5.0, 10.0, 20.0, 50.0, 100.0, 400.0, 700.0
    };
    return kPmsCents[pms & 0x07u];
}

// AMS (0..3) -> peak amplitude modulation in 10-bit EG attenuation units at AMD = 99
[[nodiscard]] inline double amsToMaxAtten10(uint8_t ams) noexcept {
    static constexpr std::array<double, 4> kAmsAtten10 = {
        0.0, 64.0, 148.0, 312.0 // 0 dB, ~6 dB, ~14 dB, ~29 dB
    };
    return kAmsAtten10[ams & 0x03u];
}

} // namespace vf50::dsp::fm
