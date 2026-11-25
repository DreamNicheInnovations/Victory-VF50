#pragma once

#include "DSP/Drums/Ym3602DrumEngine.h"
#include "DSP/Effects/Ym3413DspProcessor.h"
#include "DSP/FM/VF50FmEngine.h"
#include "DSP/VF50DemoSongPlayer.h"
#include "ROM/VF50RomBank.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vf50::dsp {

struct TriggeredDrumTelemetry {
    uint32_t sequence  = 0u;
    uint8_t  midiNote  = 0u;
    int8_t   drumIndex = -1;
    uint8_t  velocity  = 0u;
};

struct MultiBusOutputSpec {
    float* mainL = nullptr;
    float* mainR = nullptr;
    std::array<float*, 9> auxL{}; // 0..7 = INST 1..8, 8 = RHY
    std::array<float*, 9> auxR{};
    uint16_t hostEnabledAuxMask = 0u; // bit ch (0..8) set when aux bus (ch + 1) is stereo & enabled
};

class VF50SynthEngine {
public:
    static constexpr double kNativeBusSampleRate = 50000.0;

    VF50SynthEngine();

    void setRuntimeRomBank(const vf50::rom::VF50RomImage* romBank, bool reloadDefaults = false) noexcept;
    [[nodiscard]] const vf50::rom::VF50RomImage& runtimeRomBank() const noexcept {
        return fmEngine_.runtimeRomBank();
    }

    void prepare(double hostSampleRate, std::size_t maxBlockSize = 2048u) noexcept;
    void reset() noexcept;

    [[nodiscard]] double hostSampleRate() const noexcept { return hostSampleRate_; }

    // High-level Preset & Mode Selection (automatically loads preset's YM3413 DSP settings)
    void selectSingleVoice(std::size_t voiceIndex, bool fromInternalBank = false, bool syncEffects = true) noexcept;
    void selectPerformance(std::size_t perfIndex, bool fromInternalBank = false, bool syncEffects = true) noexcept;
    void applyAtomicSingleVoicePatch(const vf50::generated::FactoryVoice& voice,
                                     std::size_t presetIndex,
                                     bool fromInternalBank,
                                     const fm::QuickEditOffsets& qe,
                                     uint8_t opEnableMask = 0x0Fu,
                                     bool singleEffectSend = true,
                                     bool syncEffects = true,
                                     bool dampExistingVoices = true) noexcept;
    void applyAtomicPerformancePatch(const vf50::generated::FactoryPerformance& perf,
                                     std::size_t perfIndex,
                                     bool fromInternalBank,
                                     const std::array<int8_t, fm::VF50FmEngine::kMaxPerformanceParts>& pans,
                                     bool syncEffects = true) noexcept;
    void setPlayMode(fm::SynthPlayMode mode, bool syncEffects = true) noexcept;
    [[nodiscard]] fm::SynthPlayMode playMode() const noexcept { return fmEngine_.playMode(); }

    // Direct Subsystem Access
    [[nodiscard]] fm::VF50FmEngine& fm() noexcept { return fmEngine_; }
    [[nodiscard]] const fm::VF50FmEngine& fm() const noexcept { return fmEngine_; }

    [[nodiscard]] drums::Ym3602DrumEngine& drums() noexcept { return drumEngine_; }
    [[nodiscard]] const drums::Ym3602DrumEngine& drums() const noexcept { return drumEngine_; }

    [[nodiscard]] effects::Ym3413DspProcessor& effects() noexcept { return dspProcessor_; }
    [[nodiscard]] const effects::Ym3413DspProcessor& effects() const noexcept { return dspProcessor_; }

    [[nodiscard]] VF50DemoSongPlayer& demoPlayer() noexcept { return demoPlayer_; }
    [[nodiscard]] const VF50DemoSongPlayer& demoPlayer() const noexcept { return demoPlayer_; }

    // Master Controls & Rhythm Keyboard Routing
    void setMasterVolume(float linearGain0To1) noexcept;
    [[nodiscard]] float masterVolume() const noexcept { return masterVolume_; }

    void setSynthVolume(uint8_t synthVol0To99) noexcept;
    [[nodiscard]] uint8_t synthVolume() const noexcept { return synthVolume_; }

    [[nodiscard]] static uint8_t transformNoteVelocity(uint8_t rawVelocity,
                                                       uint8_t fixedVelocity0Off,
                                                       uint8_t velCurve0To7) noexcept;

    void setDrumsEnabled(bool enabled) noexcept { drumsEnabled_ = enabled; }
    [[nodiscard]] bool drumsEnabled() const noexcept { return drumsEnabled_; }

    void setRhythmKeyboardMode(bool enabled) noexcept { rhythmKeyboardMode_ = enabled; }
    [[nodiscard]] bool isRhythmKeyboardMode() const noexcept { return rhythmKeyboardMode_; }

    // Lock-Free Solo Bus (bits 0..7 = INST 1..8, bit 8 = RHY) & Per-Channel Stereo Peak Meters
    void setSoloMask(uint16_t mask) noexcept {
        soloMask_.store(static_cast<uint16_t>(mask & 0x01FFu), std::memory_order_release);
    }
    [[nodiscard]] uint16_t soloMask() const noexcept {
        return static_cast<uint16_t>(soloMask_.load(std::memory_order_acquire) & 0x01FFu);
    }
    void setChannelSoloed(std::size_t ch0To8, bool soloed) noexcept;
    void toggleChannelSolo(std::size_t ch0To8) noexcept;
    void clearSolo() noexcept { soloMask_.store(0u, std::memory_order_release); }

    // Per-channel Multi-Out Bus Mode (bit ch 0..8: 1 = AUX isolated when host bus enabled, 0 = parallel MAIN + AUX)
    void setAuxExclusiveMask(uint16_t mask) noexcept {
        auxExclusiveMask_.store(static_cast<uint16_t>(mask & 0x01FFu), std::memory_order_release);
    }
    [[nodiscard]] uint16_t auxExclusiveMask() const noexcept {
        return static_cast<uint16_t>(auxExclusiveMask_.load(std::memory_order_acquire) & 0x01FFu);
    }
    void setChannelAuxExclusive(std::size_t ch0To8, bool auxExclusive) noexcept;
    [[nodiscard]] bool isChannelAuxExclusive(std::size_t ch0To8) const noexcept {
        return (ch0To8 < 9u) && (((auxExclusiveMask() >> ch0To8) & 1u) != 0u);
    }
    void toggleChannelAuxExclusive(std::size_t ch0To8) noexcept;

    // Console Mix Extensions (Channels 0..7 = INST 1..8, Channel 8 = RHY)
    void setChannelBalance(std::size_t ch0To8, float balanceNeg1ToPos1) noexcept;
    [[nodiscard]] float channelBalance(std::size_t ch0To8) const noexcept;
    void setChannelFxSend(std::size_t ch0To8, uint8_t send0To99) noexcept;
    [[nodiscard]] uint8_t channelFxSend(std::size_t ch0To8) const noexcept;
    void setChannelEqEnabled(std::size_t ch0To8, bool enabled) noexcept;
    [[nodiscard]] bool channelEqEnabled(std::size_t ch0To8) const noexcept;
    void setChannelEqLowDb(std::size_t ch0To8, float dbNeg12ToPos12) noexcept;
    [[nodiscard]] float channelEqLowDb(std::size_t ch0To8) const noexcept;
    void setChannelEqHighDb(std::size_t ch0To8, float dbNeg12ToPos12) noexcept;
    [[nodiscard]] float channelEqHighDb(std::size_t ch0To8) const noexcept;
    void setSafetyLimiterEnabled(bool enabled) noexcept {
        safetyLimiterEnabled_.store(enabled, std::memory_order_relaxed);
    }
    [[nodiscard]] bool isSafetyLimiterEnabled() const noexcept {
        return safetyLimiterEnabled_.load(std::memory_order_relaxed);
    }
    void resetConsoleMixExtensions(const vf50::generated::FactoryPerformance& perf,
                                   drums::RhythmEffectMode rhyEfctMode,
                                   bool clearSoloToo = true) noexcept;

    [[nodiscard]] float instPeakL(std::size_t inst0To7) const noexcept {
        return (inst0To7 < 8u) ? instPeakL_[inst0To7].load(std::memory_order_relaxed) : 0.0f;
    }
    [[nodiscard]] float instPeakR(std::size_t inst0To7) const noexcept {
        return (inst0To7 < 8u) ? instPeakR_[inst0To7].load(std::memory_order_relaxed) : 0.0f;
    }
    [[nodiscard]] float instPeak(std::size_t inst0To7) const noexcept {
        return std::max(instPeakL(inst0To7), instPeakR(inst0To7));
    }
    [[nodiscard]] float rhyPeakL() const noexcept { return rhyPeakL_.load(std::memory_order_relaxed); }
    [[nodiscard]] float rhyPeakR() const noexcept { return rhyPeakR_.load(std::memory_order_relaxed); }
    [[nodiscard]] float rhyPeak() const noexcept { return std::max(rhyPeakL(), rhyPeakR()); }
    [[nodiscard]] float masterPeakL() const noexcept { return masterPeakL_.load(std::memory_order_relaxed); }
    [[nodiscard]] float masterPeakR() const noexcept { return masterPeakR_.load(std::memory_order_relaxed); }
    [[nodiscard]] float limiterGainReduction() const noexcept { return limiterGr_.load(std::memory_order_relaxed); }

    [[nodiscard]] uint8_t instActiveVoices(std::size_t inst0To7) const noexcept {
        return (inst0To7 < 8u) ? instActiveVoices_[inst0To7].load(std::memory_order_relaxed) : 0u;
    }
    [[nodiscard]] uint8_t totalActiveFmVoices() const noexcept {
        return totalActiveFmVoices_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint8_t totalActiveRhyVoices() const noexcept {
        return totalActiveRhyVoices_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] TriggeredDrumTelemetry lastTriggeredDrum() const noexcept { return lastDrumTelemetry_; }

    // Unified MIDI Dispatch (routes to both Dual YM2424 FM and YM3602 Rhythm sections)
    void noteOn(uint8_t midiChannel, uint8_t midiNote, uint8_t velocity) noexcept;
    void noteOff(uint8_t midiChannel, uint8_t midiNote) noexcept;
    void controlChange(uint8_t midiChannel, uint8_t ccNumber, uint8_t ccValue) noexcept;
    void pitchBend(uint8_t midiChannel, int bendValue14Bit) noexcept;
    void channelPressure(uint8_t midiChannel, uint8_t pressure) noexcept;
    void programChange(uint8_t midiChannel, uint8_t programNumber) noexcept;
    void handleRawMidiMessage(const uint8_t* data, std::size_t numBytes) noexcept;
    void allNotesOff() noexcept;
    void allSoundOff() noexcept;

    // Render stereo / multi-bus audio block at prepared hostSampleRate (44.1 / 48 / 88.2 / 96 kHz, etc.)
    void renderStereoBlock(float* outLeft, float* outRight, std::size_t numSamples) noexcept;
    void renderMultiBusBlock(const MultiBusOutputSpec& outputs, std::size_t numSamples) noexcept;

private:
    static constexpr std::size_t kSincTaps   = 16u;
    static constexpr std::size_t kSincPhases = 256u;
    static constexpr std::size_t kRingSize   = 32u;
    static constexpr std::size_t kRingMask   = kRingSize - 1u;

    struct DcBlocker {
        float prevX = 0.0f;
        float prevY = 0.0f;
        float coeffR = 0.998995f;

        void clear() noexcept {
            prevX = 0.0f;
            prevY = 0.0f;
        }

        [[nodiscard]] float process(float x) noexcept {
            if (!std::isfinite(x)) {
                clear();
                return 0.0f;
            }
            const float y = (x - prevX) + coeffR * prevY;
            prevX = x;
            prevY = (!std::isfinite(y) || (y > -1e-18f && y < 1e-18f)) ? 0.0f : y;
            return prevY;
        }
    };

    struct StereoBiquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1L = 0.0f, z2L = 0.0f;
        float z1R = 0.0f, z2R = 0.0f;

        void clear() noexcept {
            z1L = 0.0f; z2L = 0.0f;
            z1R = 0.0f; z2R = 0.0f;
        }
        void setIdentity() noexcept {
            b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
            a1 = 0.0f; a2 = 0.0f;
        }
        void process(float& l, float& r) noexcept {
            const float yL = b0 * l + z1L;
            if (!std::isfinite(yL)) {
                z1L = 0.0f;
                z2L = 0.0f;
                l = 0.0f;
            } else {
                z1L = b1 * l - a1 * yL + z2L;
                z2L = b2 * l - a2 * yL;
                if (!std::isfinite(z1L) || (z1L > -1e-18f && z1L < 1e-18f)) z1L = 0.0f;
                if (!std::isfinite(z2L) || (z2L > -1e-18f && z2L < 1e-18f)) z2L = 0.0f;
                l = yL;
            }

            const float yR = b0 * r + z1R;
            if (!std::isfinite(yR)) {
                z1R = 0.0f;
                z2R = 0.0f;
                r = 0.0f;
            } else {
                z1R = b1 * r - a1 * yR + z2R;
                z2R = b2 * r - a2 * yR;
                if (!std::isfinite(z1R) || (z1R > -1e-18f && z1R < 1e-18f)) z1R = 0.0f;
                if (!std::isfinite(z2R) || (z2R > -1e-18f && z2R < 1e-18f)) z2R = 0.0f;
                r = yR;
            }
        }
    };

    struct ChannelEqState {
        StereoBiquad lowShelf{};
        StereoBiquad highShelf{};
        float smoothLowDb    = 0.0f;
        float smoothHighDb   = 0.0f;
        float smoothWet      = 0.0f;
        float lastCalcLowDb  = 999.0f;
        float lastCalcHighDb = 999.0f;
        uint32_t subCounter  = 0u;

        void resetToTargets(bool enabled, float lowDb, float highDb) noexcept;
        void process(float& l, float& r, bool enabled, float targetLowDb, float targetHighDb, float sampleRateHz) noexcept;
    };

    void rebuildResamplerTable() noexcept;
    [[nodiscard]] effects::StereoFrame renderOneNative50kHzFrame(
        uint16_t hostEnabledAuxMask = 0u,
        std::array<effects::StereoFrame, 9>* outAuxFrames = nullptr) noexcept;
    [[nodiscard]] static float softCeiling(float x) noexcept;

    fm::VF50FmEngine fmEngine_{};
    drums::Ym3602DrumEngine drumEngine_{};
    effects::Ym3413DspProcessor dspProcessor_{};
    VF50DemoSongPlayer demoPlayer_{};

    double hostSampleRate_ = 48000.0;
    double stepRatio50kPerHost_ = kNativeBusSampleRate / 48000.0;
    double resamplerFrac_ = 0.0;

    std::array<effects::StereoFrame, kRingSize> ring50k_{};
    std::size_t ringWrite_ = 0u;
    std::array<std::array<float, kSincTaps>, kSincPhases + 1u> sincTable_{};

    DcBlocker dcBlock50kL_{};
    DcBlocker dcBlock50kR_{};
    DcBlocker dcBlockHostL_{};
    DcBlocker dcBlockHostR_{};
    std::array<std::array<effects::StereoFrame, kRingSize>, 9> auxRing50k_{};
    std::array<DcBlocker, 9> auxDcBlockHostL_{};
    std::array<DcBlocker, 9> auxDcBlockHostR_{};
    std::atomic<uint16_t> auxExclusiveMask_{ 0x01FFu };

    float masterVolume_ = 0.80f;
    float smoothMasterVolume_ = 0.80f;
    uint8_t synthVolume_ = 99u;
    float synthVolumeGain_ = 1.0f;
    bool drumsEnabled_ = true;
    bool rhythmKeyboardMode_ = false;
    TriggeredDrumTelemetry lastDrumTelemetry_{};

    std::atomic<uint16_t> soloMask_{ 0u };
    std::array<std::atomic<float>, 9> targetBalance_{};
    std::array<std::atomic<uint8_t>, 9> targetFxSend_{};
    std::array<std::atomic<bool>, 9> targetEqEnabled_{};
    std::array<std::atomic<float>, 9> targetEqLowDb_{};
    std::array<std::atomic<float>, 9> targetEqHighDb_{};
    std::atomic<bool> safetyLimiterEnabled_{ true };

    std::array<float, 9> smoothBal_{};
    std::array<float, 9> smoothFxSend_{};
    std::array<ChannelEqState, 9> channelEq_{};

    std::array<std::atomic<float>, 8> instPeakL_{};
    std::array<std::atomic<float>, 8> instPeakR_{};
    std::atomic<float> rhyPeakL_{ 0.0f };
    std::atomic<float> rhyPeakR_{ 0.0f };
    std::atomic<float> masterPeakL_{ 0.0f };
    std::atomic<float> masterPeakR_{ 0.0f };
    std::atomic<float> limiterGr_{ 0.0f };
    std::array<std::atomic<uint8_t>, 8> instActiveVoices_{};
    std::atomic<uint8_t> totalActiveFmVoices_{ 0u };
    std::atomic<uint8_t> totalActiveRhyVoices_{ 0u };

    std::array<float, 8> blockInstPeakL_{};
    std::array<float, 8> blockInstPeakR_{};
    float blockRhyPeakL_ = 0.0f;
    float blockRhyPeakR_ = 0.0f;
    std::array<uint8_t, 8> lastFrameFmVoices_{};
    uint8_t lastFrameTotalFmVoices_ = 0u;
    uint8_t lastFrameTotalRhyVoices_ = 0u;
};

} // namespace vf50::dsp
