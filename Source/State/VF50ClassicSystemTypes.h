#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace vf50::state {

// Spec §3.4 (pp. 111-114): Editable Performance Effect Memory (delay1-4, pan1-4, chord1-4)
struct PerfDelayMemory {
    uint8_t delayTimeCs = 15u; // 1..128 (0.01 .. 1.28 sec)
    int8_t  pitchShift  = 0;   // -24..+24 semitones
    uint8_t feedback    = 4u;  // 0..7
    uint8_t effectLevel = 90u; // 0..99
};

struct PerfPanMemory {
    uint8_t select    = 0u;  // 0=LFO, 1=VEL, 2=NOTE
    uint8_t direction = 0u;  // 0=L->R, 1=L<-R
    uint8_t panRange  = 99u; // 0..99
};

struct PerfChordMemory {
    // 12 trigger pitch classes C3..B3 (index = note % 12), up to 4 MIDI notes (0..127, or -1 for '*')
    std::array<std::array<int8_t, 4>, 12> chords{};
};

struct PerfEffectBank {
    std::array<PerfDelayMemory, 4> delays{};
    std::array<PerfPanMemory, 4>   pans{};
    std::array<PerfChordMemory, 4> chords{};
};

[[nodiscard]] inline PerfEffectBank makeFactoryPerfEffectBank() noexcept {
    PerfEffectBank bank{};
    bank.delays[0] = { 15u,  0, 4u, 90u };
    bank.delays[1] = { 25u,  7, 4u, 85u };
    bank.delays[2] = { 40u, 12, 5u, 80u };
    bank.delays[3] = { 128u, 0, 6u, 90u };

    bank.pans[0] = { 0u, 0u, 99u };
    bank.pans[1] = { 1u, 0u, 99u };
    bank.pans[2] = { 2u, 0u, 99u };
    bank.pans[3] = { 0u, 1u, 80u };

    for (int pc = 0; pc < 12; ++pc) {
        const int8_t root = static_cast<int8_t>(60 + pc); // C3..B3
        bank.chords[0].chords[static_cast<std::size_t>(pc)] = { root, static_cast<int8_t>(root + 4), static_cast<int8_t>(root + 7), -1 };
        bank.chords[1].chords[static_cast<std::size_t>(pc)] = { root, static_cast<int8_t>(root + 3), static_cast<int8_t>(root + 7), -1 };
        bank.chords[2].chords[static_cast<std::size_t>(pc)] = { root, static_cast<int8_t>(root + 4), static_cast<int8_t>(root + 7), static_cast<int8_t>(root + 11) };
        bank.chords[3].chords[static_cast<std::size_t>(pc)] = { root, static_cast<int8_t>(root + 5), static_cast<int8_t>(root + 7), -1 };
    }
    return bank;
}

// Spec §2.4 (pp. 101-103): 128-entry Program Change Mapping Table (PC 001..128 -> Voice/Perf I/C/P 00..99)
struct ProgramChangeEntry {
    bool    isPerformance = false;
    uint8_t bank          = 0u; // 0=INT, 1=CARD, 2=PRESET
    uint8_t number        = 0u; // 0..99
};

struct ProgramChangeTable {
    std::array<ProgramChangeEntry, 128> entries{};

    void resetToFactory() noexcept {
        for (std::size_t i = 0u; i < 100u; ++i) {
            entries[i] = { false, 0u, static_cast<uint8_t>(i) }; // I00..I99
        }
        for (std::size_t i = 100u; i < 128u; ++i) {
            entries[i] = { false, 2u, static_cast<uint8_t>(i - 100u) }; // P00..P27
        }
    }
};

// Spec §1.3 (p. 96 20-item SYS table), §2 (pp. 99-104), §3 (pp. 110-116), §4.2 (pp. 11, 22)
struct VF50SystemSettings {
    int8_t  masterTuningSteps   = 0;    // -64..+64 (1.5625 cents/step; 0 = 440.0Hz)
    uint8_t synthVolume         = 99u;  // 0..99 (default 99 = unity)
    bool    combineWithFunction = true; // off / on
    bool    controllerReset     = false;// false=hold, true=reset
    uint8_t fixedVelocity       = 0u;   // 0=off, 1..127
    uint8_t velocityCurve       = 0u;   // 0=norm, 1..5=curve1..5, 6=flat (<=80), 7=revers
    uint8_t egDampSpeed         = 3u;   // 0=veryslow, 1=slow, 2=medium, 3=fast
    bool    voiceDampOn         = true; // off / on

    bool    midiOn              = true; // off / on
    uint8_t basicReceiveCh      = 16u;  // 0..15=Ch1..16, 16=omni
    uint8_t transmitCh          = 0u;   // 0..15=Ch1..16
    bool    localOn             = true; // Always resets true at power-on (p. 99)
    uint8_t controlChangeMode   = 1u;   // 0=off, 1=norm, 2..17=G1..G16
    uint8_t aftertouchMode      = 1u;   // 0=off, 1=norm, 2..17=G1..G16
    uint8_t pitchBendMode       = 1u;   // 0=off, 1=norm, 2..17=G1..G16
    uint8_t noteOnOffFilter     = 0u;   // 0=all, 1=odd, 2=even
    uint8_t dataEntryAssignCc   = 0u;   // 0..31
    uint8_t pgmChangeMode       = 2u;   // 0=off, 1=common, 2=individual, 3=direct, 4=TransFilter
    uint8_t deviceNumber        = 17u;  // 0=off, 1..16=Dev1..16, 17=all

    std::array<char, 40> powerOnMessage{ {
        '>','>','>','>',' ',' ',' ',' ',' ',' ',' ',
        'N','i','c','e',' ','t','o',' ','m','e','e','t',' ','y','o','u',' ','!','!',
        ' ',' ',' ',' ',' ',' ','<','<','<','<'
    } };
};

} // namespace vf50::state
