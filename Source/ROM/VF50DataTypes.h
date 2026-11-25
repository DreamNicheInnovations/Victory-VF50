#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#ifndef VF50_GENERATED_TYPES_DEFINED
#define VF50_GENERATED_TYPES_DEFINED

namespace vf50::generated {

inline constexpr std::size_t kFactoryVoiceCount       = 100u;
inline constexpr std::size_t kFactoryPerformanceCount = 100u;
inline constexpr std::size_t kFactoryMicrotuningCount = 11u;
inline constexpr std::size_t kFactoryRhythmPtnCount   = 100u;
inline constexpr std::size_t kDrumInstrumentCount     = 61u;
inline constexpr std::size_t kNumDrumWaveforms        = 61u;
inline constexpr std::size_t kDspEffectPresetCount    = 32u;
inline constexpr uint16_t    kRhythmTicksPerQuarter   = 48u;
inline constexpr uint32_t    kOprwMasterClockHz       = 3200000u;
inline constexpr uint32_t    kDrumNativeSampleRateHz  = 25000u;
inline constexpr uint32_t    kDrumUpsampledBusRateHz  = 50000u;
inline constexpr std::size_t kDemoSongCount           = 3u;
inline constexpr uint16_t    kDemoSongPpqn            = 48u;

struct DspEffectSettings {
    uint8_t effectType = 0u;  // 0 = off, 1..32 = YM3413 DSP Effect preset
    uint8_t balance    = 50u; // 0..100 (%)
    uint8_t outLevel   = 100u;// 0..100
    uint8_t stereoMix  = 1u;  // 0 = off, 1 = stereo mix enabled
    uint8_t param1     = 10u; // 0..75
    uint8_t param2     = 10u; // 0..99
    uint8_t param3     = 10u; // 0..99
};

struct VoiceOperator {
    uint8_t ar            = 31u; // 0..31
    uint8_t d1r           = 0u;  // 0..31
    uint8_t d2r           = 0u;  // 0..31
    uint8_t rr            = 7u;  // 1..15 (0..15 raw)
    uint8_t d1l           = 15u; // 0..15
    int8_t  levelScaling  = 0;   // -99..+99
    uint8_t rateScaling   = 0u;  // 0..3
    uint8_t egBiasSens    = 0u;  // 0..7
    uint8_t ampModEnable  = 0u;  // 0..1
    int8_t  keyVelSens    = 0;   // -7..+7
    uint8_t outputLevel   = 0u;  // 0..99
    uint8_t coarse        = 4u;  // 0..63
    uint8_t fine          = 0u;  // 0..15
    int8_t  detune        = 0;   // -3..+3
    uint8_t waveform      = 0u;  // 0..7 (W1..W8)
    uint8_t egShift       = 0u;  // 0=off, 1=48dB, 2=24dB, 3=12dB
    uint8_t fixedFreqMode = 0u;  // 0=Ratio, 1=Fixed Frequency
    uint8_t fixedRange    = 0u;  // 0..7
    uint8_t fixedRangeLow = 0u;  // 0=HI (255Hz..32kHz), 1=LO (1Hz..100Hz down to 0.00Hz)
};

struct PitchEnvelope {
    uint8_t pr1 = 99u, pr2 = 99u, pr3 = 99u; // 0..99
    uint8_t pl1 = 50u, pl2 = 50u, pl3 = 50u; // 0..99 (50 = 0 semitones)
};

struct FactoryVoice {
    uint8_t          index          = 0u;
    std::string_view name           = {};
    uint8_t          algorithm      = 0u;   // 0..7 (ALG 1..8)
    uint8_t          feedback       = 0u;   // 0..7
    uint8_t          lfoSpeed       = 35u;  // 0..99
    uint8_t          lfoDelay       = 0u;   // 0..99
    uint8_t          pmd            = 0u;   // 0..99
    uint8_t          amd            = 0u;   // 0..99
    uint8_t          lfoSync        = 0u;   // 0..1
    uint8_t          lfoWave        = 2u;   // 0=SawUp, 1=Square, 2=Triangle, 3=Sample&Hold
    uint8_t          pms            = 0u;   // 0..7
    uint8_t          ams            = 0u;   // 0..3
    int8_t           transpose      = 0;    // -24..+24 semitones
    uint8_t          monoMode       = 0u;   // 0=Poly, 1=Mono
    uint8_t          pitchBendRange = 2u;   // 0..12 semitones
    uint8_t          portaMode      = 0u;   // 0=FullTime, 1=Fingered
    uint8_t          portaSwitch    = 0u;   // 0..1
    uint8_t          portaTime      = 0u;   // 0..99
    uint8_t          sustainSwitch  = 1u;   // 0..1
    uint8_t          chorusSwitch   = 0u;   // 0..1
    uint8_t          reverbRate     = 0u;   // 0..7
    uint8_t          fcVolume       = 0u;   // 0..99
    uint8_t          fcPitch        = 0u;   // 0..99
    uint8_t          fcAmp          = 0u;   // 0..99
    uint8_t          mwPitch        = 50u;  // 0..99
    uint8_t          mwAmp          = 0u;   // 0..99
    uint8_t          bcPitch        = 0u;   // 0..99
    uint8_t          bcAmp          = 0u;   // 0..99
    int8_t           bcPitchBias    = 0;    // -50..+50
    uint8_t          bcEgBias       = 0u;   // 0..99
    uint8_t          atPitch        = 0u;   // 0..99
    uint8_t          atAmp          = 0u;   // 0..99
    int8_t           atPitchBias    = 0;    // -50..+50
    uint8_t          atEgBias       = 0u;   // 0..99
    PitchEnvelope    peg{};
    std::array<VoiceOperator, 4> operators{}; // Logical order: OP1, OP2, OP3, OP4
    DspEffectSettings effect{};
    std::array<uint8_t, 95> rawRomBytes{};    // Packed 95-byte VMEM+ACED3 slice
};

struct PerformanceInstrument {
    uint8_t maxNotes        = 16u;  // 0..16
    uint8_t reserveNotes    = 0u;   // 0=off, 1..17 -> 0..16 reserved voices in DVA mode
    uint8_t voiceBank       = 1u;   // 0=Internal/Card, 1=Preset (P00..P99)
    uint8_t voiceNumber     = 0u;   // 0..99
    uint8_t receiveChannel  = 16u;  // 0..15=Ch1..16, 16=Omni
    uint8_t limitLow        = 0u;   // 0..127 MIDI note
    uint8_t limitHigh       = 127u; // 0..127 MIDI note
    int8_t  detune          = 0;    // -7..+7
    int8_t  noteShift       = 0;    // -24..+24 semitones
    uint8_t volume          = 99u;  // 0..99
    uint8_t outAssign       = 3u;   // 0=off, 1=Out I(L), 2=Out II(R), 3=Out I+II(L+R)
    uint8_t lfoSelect       = 1u;   // 0=off, 1=1st Inst, 2=2nd Inst, 3=Vibrato
    uint8_t microTuneEnable = 0u;   // 0..1
    uint8_t effectEnable    = 1u;   // 0..1
};

struct FactoryPerformance {
    uint8_t          index            = 0u;
    std::string_view name             = {};
    uint8_t          assignMode       = 0u; // 0=Normal, 1=Alternate, 2=DVA
    uint8_t          microTuneTable   = 0u; // 0..10=Preset 1..11, 11=Int Oct, 12=Int Full
    uint8_t          microTuneRootKey = 0u; // 0..11 (C..B)
    uint8_t          perfEffectSelect = 0u; // 0=off, 1..12=Delay/Pan/Chord
    std::array<PerformanceInstrument, 8> instruments{};
    DspEffectSettings effect{};
    std::array<uint8_t, 97> rawRomBytes{}; // Packed 97-byte PMEM+PMEM2 slice
};

struct MicrotuningEntry {
    uint8_t coarseNote = 60u; // 13..107 MIDI semitone index
    uint8_t fineStep64 = 0u;  // 0..63 (1 step = 100/64 = 1.5625 cents)
    constexpr double totalCents() const noexcept {
        return static_cast<double>(coarseNote) * 100.0 + (static_cast<double>(fineStep64) * 100.0 / 64.0);
    }
};

struct MicrotuningTable {
    uint8_t          index = 0u;
    std::string_view name  = {};
    std::array<MicrotuningEntry, 12>  baseOctave{};
    std::array<MicrotuningEntry, 128> keys{};
};

struct RhythmNoteEvent {
    uint16_t tick          = 0u; // Tick offset from pattern start (48 PPQN)
    uint8_t  drumIndex     = 0u; // 0..60 YM3602 drum instrument index
    uint8_t  velocityLevel = 5u; // 1..9 VF50 rhythm accent/velocity level
    uint8_t  rawParam      = 0u; // Raw 2nd event byte
};

struct FactoryRhythmPattern {
    uint8_t  index       = 0u;   // 0..99 (PTN 001..100)
    uint8_t  timeSigCode = 3u;   // 0..27 VF50 time signature code
    uint8_t  timeSigNum  = 4u;   // Numerator (1..16)
    uint8_t  timeSigDen  = 4u;   // Denominator (4, 8, or 16)
    uint8_t  bars        = 1u;   // 1..4 measures
    uint16_t ticksPerBar = 192u; // Ticks per measure at 48 PPQN
    uint16_t totalTicks  = 192u; // Total pattern length in ticks
    uint16_t rawOffset   = 0u;   // Byte offset into raw rhythm stream
    uint16_t rawLength   = 0u;   // Byte length in raw rhythm stream
    uint16_t eventOffset = 0u;   // Index into decoded rhythm events array
    uint16_t eventCount  = 0u;   // Number of drum note events
};

struct DrumInstrumentSetup {
    uint8_t          index           = 0u;  // 0..60
    std::string_view name            = {};  // 6-char VF50 LCD name
    uint8_t          chokeGroup      = 7u;  // 0 = Hi-Hat exclusive choke group, 1..7 = voice group
    uint8_t          defaultVolume   = 15u; // 0..15
    int8_t           defaultPan      = 0;   // -3..+3
    uint8_t          defaultMidiNote = 31u; // 0..127 MIDI note assignment
};

struct DspEffectPreset {
    uint8_t          effectNumber = 1u;
    std::string_view name         = {};
    std::array<uint8_t, 20> subCpuParams{};
};

struct DrumWaveformDescriptor {
    uint8_t          index            = 0u;
    std::string_view name             = {};
    std::string_view lcdName          = {};
    uint8_t          oprwWaveId       = 0u;
    uint16_t         romPage          = 0u;
    uint32_t         romDataOffset    = 0u;
    uint32_t         totalSamples     = 0u;
    uint32_t         pcmAttackSamples = 0u;
    uint32_t         loopSampleIndex  = 0u;
    uint8_t          dpcmScaleCode    = 0u;
    uint8_t          dpcmShiftLeft    = 0u;
    uint8_t          oprwReg2         = 0u;
    uint8_t          oprwReg3         = 0u;
    uint8_t          defaultGateTicks = 0u;
    uint8_t          levelAttenCode   = 0u;
    uint8_t          attackRate       = 0x1Fu;
    uint8_t          decay1Rate       = 0u;
    uint8_t          decay1Level      = 0x0Fu;
    uint8_t          decay2Rate       = 0u;
    uint8_t          releaseRate      = 0x0Fu;
    std::array<uint8_t, 26> rawHeader{};
    const int16_t*   samples          = nullptr;

    [[nodiscard]] constexpr std::span<const int16_t> sampleSpan() const noexcept {
        return (samples != nullptr && totalSamples > 0u)
                   ? std::span<const int16_t>{ samples, totalSamples }
                   : std::span<const int16_t>{};
    }

    [[nodiscard]] constexpr int16_t sampleAt50kHzTruncated(std::size_t busSampleIndex) const noexcept {
        const std::size_t nativeIndex = busSampleIndex >> 1u;
        return (samples != nullptr && nativeIndex < totalSamples) ? samples[nativeIndex] : int16_t{ 0 };
    }
};

enum class DemoEventType : uint8_t {
    FmNoteOn       = 0,
    FmNoteOff      = 1,
    FmControl      = 2,
    FmProgram      = 3,
    FmPitchBend    = 4,
    FmAftertouch   = 5,
    DrumHit        = 6,
    TempoChange    = 7,
    RhythmVolDelta = 8
};

struct DemoSongEvent {
    uint32_t      tick    = 0u;
    DemoEventType type    = DemoEventType::FmNoteOn;
    uint8_t       channel = 0u;
    uint8_t       data1   = 0u;
    uint8_t       data2   = 0u;
};

struct FactoryDemoSong {
    uint8_t                 index            = 0u;
    std::string_view        title            = {};
    uint8_t                 initialTempoBpm  = 120u;
    uint8_t                 timeSigNum       = 4u;
    uint8_t                 timeSigDen       = 4u;
    uint16_t                ticksPerBar      = 192u;
    uint16_t                totalBars        = 1u;
    uint32_t                totalTicks       = 192u;
    uint8_t                 perfPresetBank   = 1u;
    uint8_t                 perfNumber       = 0u;
    uint8_t                 rhythmSongIndex  = 0u;
    std::array<uint8_t, 8>  trackChannels    = {};
    uint32_t                eventOffset      = 0u;
    uint32_t                eventCount       = 0u;
};

enum class RhythmSongPartType : uint8_t {
    Pattern     = 0,
    RepeatBegin = 1,
    RepeatEnd   = 2,
    Volume      = 3,
    Tempo       = 4,
    Mark        = 5
};

struct RhythmSongPart {
    RhythmSongPartType type        = RhythmSongPartType::Pattern;
    bool               ptnIsUser   = false;
    uint8_t            ptnIndex    = 0u;
    uint8_t            repeatCount = 2u;
    int8_t             volDelta    = 0;
    int16_t            tempoDelta  = 0;
    uint8_t            tempoBeats  = 4u;
    char               markChar    = 'A';
};

struct RhythmSongData {
    static constexpr std::size_t kMaxParts = 999u;

    std::array<char, 8>                  name{ { 'N','e','w','S','o','n','g',' ' } };
    uint16_t                             partCount = 0u;
    std::array<RhythmSongPart, kMaxParts> parts{};

    void clear() noexcept {
        name = { 'N','e','w','S','o','n','g',' ' };
        partCount = 0u;
        parts.fill(RhythmSongPart{});
    }

    [[nodiscard]] bool hasData() const noexcept {
        return partCount > 0u;
    }
};

} // namespace vf50::generated

#endif // VF50_GENERATED_TYPES_DEFINED
