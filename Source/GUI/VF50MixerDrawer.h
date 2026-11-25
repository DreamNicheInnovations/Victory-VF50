#pragma once

#include "Classic/VF50ClassicController.h"
#include "GUI/VF50DesignTokens.h"
#include "GUI/VF50LookAndFeel.h"
#include "Plugin/PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace vf50::gui {

// =============================================================================
// Drop-Down Visual Mixer Console (8 Perf Instruments + Rhythm + Master / FX +
// Embedded 61-Drum INST SET Sub-Mix View)
// =============================================================================
class VF50MixerDrawer final : public juce::Component {
public:
    enum class DrumFilterMode : uint8_t {
        UsedInSong   = 0,
        All61        = 1,
        KickSnareHat = 2
    };

        VF50MixerDrawer(vf50::plugin::VF50AudioProcessor& processor,
                   vf50::classic::VF50ClassicController& classicController);
    ~VF50MixerDrawer() override;

    std::function<void()> onMixerStateChanged;

    // Pulls active PMEM, Rhythm Kit, Effect, Mute/Solo, Console Extensions & Stereo Meters
    void syncFromProcessor();

    // Programmatic interaction & inspection helpers for automated headless tests
    bool setInstVolumeFromMixer(std::size_t inst0To7, int vol0To99);
    bool cycleInstOutAssignFromMixer(std::size_t inst0To7);
    bool setInstOutAssignFromMixer(std::size_t inst0To7, uint8_t outAssign0To3);
    bool setInstNotesFromMixer(std::size_t inst0To7, int notes0To16);
    bool setInstNoteShiftFromMixer(std::size_t inst0To7, int shiftNeg24ToPos24);
    bool setInstDetuneFromMixer(std::size_t inst0To7, int detuneNeg7ToPos7);
    void toggleInstMuteFromMixer(std::size_t inst0To7);
    void toggleInstSoloFromMixer(std::size_t inst0To7);

    void setRhythmVolumeFromMixer(int vol0To99);
    void cycleRhythmEffectModeFromMixer();
    void toggleRhythmMuteFromMixer();
    void toggleRhythmSoloFromMixer();
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    void toggleChannelBusRouteFromMixer(std::size_t ch0To8);
    [[nodiscard]] bool isChannelAuxRouteSelected(std::size_t ch0To8) const noexcept;
#endif

    void setChannelBalanceFromMixer(std::size_t ch0To8, float balNeg1ToPos1);
    void setChannelFxSendFromMixer(std::size_t ch0To8, int send0To99);
    void setChannelEqFromMixer(std::size_t ch0To8, bool enabled, float lowDbNeg12ToPos12, float highDbNeg12ToPos12);

    void clearAllSoloFromMixer();
    void resetMixFromMixer();
    void toggleFxBypassFromMixer();
    bool setFxTypeFromMixer(int fxType0To32);
    bool setFxBalanceFromMixer(int bal0To99);
    void toggleSafetyLimiterFromMixer();

    void setDrumSubMixVisible(bool visible);
    void setDrumFilterMode(DrumFilterMode mode);
    void setDrumInstVolumeFromMixer(std::size_t drum0To60, int vol0To15);
    void setDrumInstPanFromMixer(std::size_t drum0To60, int panNeg3ToPos3);

    [[nodiscard]] int displayedInstVolume(std::size_t inst0To7) const noexcept;
    [[nodiscard]] uint8_t displayedInstOutAssign(std::size_t inst0To7) const noexcept;
    [[nodiscard]] int displayedInstNotes(std::size_t inst0To7) const noexcept;
    [[nodiscard]] int displayedInstNoteShift(std::size_t inst0To7) const noexcept;
    [[nodiscard]] int displayedInstDetune(std::size_t inst0To7) const noexcept;
    [[nodiscard]] int displayedRhythmVolume() const noexcept;
    [[nodiscard]] vf50::dsp::drums::RhythmEffectMode displayedRhythmEffectMode() const noexcept;
    [[nodiscard]] float displayedChannelBalance(std::size_t ch0To8) const noexcept;
    [[nodiscard]] int displayedChannelFxSend(std::size_t ch0To8) const noexcept;
    [[nodiscard]] bool isChannelEqEnabled(std::size_t ch0To8) const noexcept;
    [[nodiscard]] float displayedChannelEqLowDb(std::size_t ch0To8) const noexcept;
    [[nodiscard]] float displayedChannelEqHighDb(std::size_t ch0To8) const noexcept;
    [[nodiscard]] bool isInstMuteButtonLit(std::size_t inst0To7) const noexcept;
    [[nodiscard]] bool isRhythmMuteButtonLit() const noexcept;
    [[nodiscard]] bool isSoloButtonLit(std::size_t ch0To8) const noexcept;
    [[nodiscard]] bool isClearSoloButtonLit() const noexcept;
    [[nodiscard]] bool isFxBypassButtonLit() const noexcept;
    [[nodiscard]] int displayedFxType() const noexcept;
    [[nodiscard]] int displayedFxBalance() const noexcept;
    [[nodiscard]] bool isSafetyLimiterButtonLit() const noexcept;
    [[nodiscard]] bool isDrumSubMixVisible() const noexcept { return drumSubMixVisible_; }
    [[nodiscard]] DrumFilterMode drumFilterMode() const noexcept { return drumFilterMode_; }
    [[nodiscard]] std::size_t visibleDrumSubMixCount() const noexcept { return visibleDrumIndices_.size(); }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class MixerFaderLookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        MixerFaderLookAndFeel() = default;

        void drawLinearSlider(juce::Graphics& g,
                              int x, int y, int width, int height,
                              float sliderPos,
                              float minSliderPos,
                              float maxSliderPos,
                              juce::Slider::SliderStyle style,
                              juce::Slider& slider) override;

        void drawRotarySlider(juce::Graphics& g,
                              int x, int y, int width, int height,
                              float sliderPosProportional,
                              float rotaryStartAngle,
                              float rotaryEndAngle,
                              juce::Slider& slider) override;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerFaderLookAndFeel)
    };

    class MixerTactileButton final : public juce::Button {
    public:
        explicit MixerTactileButton(const juce::String& name);

        void setVisualState(const juce::String& label,
                            uint32_t textArgb,
                            bool lit,
                            uint32_t litBorderArgb,
                            uint32_t litFillArgb,
                            bool showLedDot = false,
                            bool blinkDim = false) noexcept;

        [[nodiscard]] bool isLit() const noexcept { return lit_; }
        [[nodiscard]] const juce::String& currentLabel() const noexcept { return label_; }

        void paintButton(juce::Graphics& g, bool over, bool down) override;

    private:
        juce::String label_;
        uint32_t textArgb_      = tokens::kSilkWarmWhite;
        uint32_t litBorderArgb_ = tokens::kMixerTealAccent;
        uint32_t litFillArgb_   = 0x3300B8A9u;
        bool lit_               = false;
        bool showLedDot_        = false;
        bool blinkDim_          = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerTactileButton)
    };

    class MixerDragPill final : public juce::Component {
    public:
        MixerDragPill(int minVal, int maxVal, int defaultVal, uint32_t accentArgb);

        void setRange(int minVal, int maxVal, int defaultVal) noexcept;
        void setValue(int val, bool sendNotification = false);
        [[nodiscard]] int value() const noexcept { return value_; }
        void setDisplayText(const juce::String& text, bool active = true) noexcept;
        void setAccentArgb(uint32_t argb) noexcept { accentArgb_ = argb; }
        void setCycleOnClick(bool cycle) noexcept { cycleOnClick_ = cycle; }

        std::function<void(int)> onValueChanged;

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseDoubleClick(const juce::MouseEvent& e) override;
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    private:
        int minVal_          = 0;
        int maxVal_          = 16;
        int defaultVal_      = 0;
        int value_           = 0;
        int dragStartVal_    = 0;
        bool dragged_        = false;
        bool active_         = true;
        bool cycleOnClick_   = false;
        uint32_t accentArgb_ = tokens::kMixerTealAccent;
        juce::String displayText_{};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerDragPill)
    };

    class DrumSubMixCell final : public juce::Component {
    public:
        DrumSubMixCell(VF50MixerDrawer& owner, std::size_t drumIndex);

        void setDrumData(std::size_t drumIndex, const juce::String& name, int vol0To15, int panNeg3ToPos3);

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    private:
        [[nodiscard]] juce::Rectangle<float> volBarBounds() const noexcept;
        [[nodiscard]] juce::Rectangle<float> panStripBounds() const noexcept;
        void applyVolFromX(float x);
        void applyPanFromX(float x);

        VF50MixerDrawer& owner_;
        std::size_t drumIndex_ = 0u;
        juce::String drumName_{};
        int volume_            = 15;
        int pan_               = 0;
        int dragTarget_        = 0; // 0=none, 1=vol, 2=pan

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumSubMixCell)
    };

    struct MeterChannelState {
        float val      = 0.0f;
        float hold     = 0.0f;
        int   holdTick = 0;
        bool  clipped  = false;
        int   clipTick = 0;
    };

    struct InstStrip {
        juce::Rectangle<int> stripRect{};
        juce::Rectangle<int> headerRect{};
        juce::Rectangle<int> pipsRect{};
        juce::Rectangle<int> badgeRect{};
        juce::Rectangle<int> voiceNameRect{};
        juce::Rectangle<int> balLabelRect{};
        juce::Rectangle<int> balValueRect{};
        juce::Rectangle<int> fxLabelRect{};
        juce::Rectangle<int> fxValueRect{};
        juce::Rectangle<int> loLabelRect{};
        juce::Rectangle<int> loValueRect{};
        juce::Rectangle<int> hiLabelRect{};
        juce::Rectangle<int> hiValueRect{};
        juce::Rectangle<int> meterLRect{};
        juce::Rectangle<int> meterRRect{};
        juce::Rectangle<int> readoutRect{};

        std::unique_ptr<MixerDragPill>      pillNotes{};
        std::unique_ptr<MixerDragPill>      pillShift{};
        std::unique_ptr<MixerDragPill>      pillDetune{};
        std::unique_ptr<MixerTactileButton> btnOutAssign{};
        std::unique_ptr<juce::Slider>       balKnob{};
        std::unique_ptr<juce::Slider>       fxSendKnob{};
        std::unique_ptr<MixerTactileButton> btnEqToggle{};
        std::unique_ptr<juce::Slider>       eqLoKnob{};
        std::unique_ptr<juce::Slider>       eqHiKnob{};
        std::unique_ptr<MixerTactileButton> btnMute{};
        std::unique_ptr<MixerTactileButton> btnSolo{};
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        std::unique_ptr<MixerTactileButton> btnBusRoute{};
#endif
        std::unique_ptr<juce::Slider>       fader{};

        juce::String voiceName{ "----------" };
        juce::String slotChBadge{ "P00 | OMNI" };
        uint8_t      outAssign    = 3u;
        uint8_t      activeVoices = 0u;
        bool         enabled      = true;
        MeterChannelState meterL{};
        MeterChannelState meterR{};
    };

    struct RhythmStrip {
        juce::Rectangle<int> stripRect{};
        juce::Rectangle<int> headerRect{};
        juce::Rectangle<int> pipsRect{};
        juce::Rectangle<int> badgeRect{};
        juce::Rectangle<int> balLabelRect{};
        juce::Rectangle<int> balValueRect{};
        juce::Rectangle<int> fxLabelRect{};
        juce::Rectangle<int> fxValueRect{};
        juce::Rectangle<int> loLabelRect{};
        juce::Rectangle<int> loValueRect{};
        juce::Rectangle<int> hiLabelRect{};
        juce::Rectangle<int> hiValueRect{};
        juce::Rectangle<int> meterLRect{};
        juce::Rectangle<int> meterRRect{};
        juce::Rectangle<int> readoutRect{};

        std::unique_ptr<MixerTactileButton> btnDrumSubMix{};
        std::unique_ptr<MixerTactileButton> btnEffectMode{};
        std::unique_ptr<juce::Slider>       balKnob{};
        std::unique_ptr<juce::Slider>       fxSendKnob{};
        std::unique_ptr<MixerTactileButton> btnEqToggle{};
        std::unique_ptr<juce::Slider>       eqLoKnob{};
        std::unique_ptr<juce::Slider>       eqHiKnob{};
        std::unique_ptr<MixerTactileButton> btnMute{};
        std::unique_ptr<MixerTactileButton> btnSolo{};
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        std::unique_ptr<MixerTactileButton> btnBusRoute{};
#endif
        std::unique_ptr<juce::Slider>       fader{};

        juce::String kitModeBadge{ "KIT:ROM | PTN" };
        vf50::dsp::drums::RhythmEffectMode effectMode = vf50::dsp::drums::RhythmEffectMode::Mix;
        uint8_t activeVoices = 0u;
        MeterChannelState meterL{};
        MeterChannelState meterR{};
    };

    struct MasterStrip {
        juce::Rectangle<int> stripRect{};
        juce::Rectangle<int> headerRect{};
        juce::Rectangle<int> fxBalLabelRect{};
        juce::Rectangle<int> fxBalReadoutRect{};
        juce::Rectangle<int> limitGrRect{};
        juce::Rectangle<int> meterLRect{};
        juce::Rectangle<int> meterRRect{};
        juce::Rectangle<int> readoutRect{};

        std::unique_ptr<MixerDragPill>      pillFxType{};
        std::unique_ptr<juce::Slider>       fxBalKnob{};
        std::unique_ptr<MixerTactileButton> btnFxBypass{};
        std::unique_ptr<MixerTactileButton> btnLimiter{};
        std::unique_ptr<juce::Slider>       masterFader{};
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAttach{};

        float limiterGr = 0.0f;
        MeterChannelState meterL{};
        MeterChannelState meterR{};
    };

    void buildControls();
    void updateChannelStripVisibility();
    void rebuildDrumSubMixGrid();
    void updateMeterBallistics() noexcept;
    void notifyParentStateChanged();

    void paintHeaderStrip(juce::Graphics& g) const;
    void paintInstStrip(juce::Graphics& g, std::size_t idx) const;
    void paintRhythmStrip(juce::Graphics& g) const;
    void paintMasterStrip(juce::Graphics& g) const;
    void paintDrumSubMixBackground(juce::Graphics& g) const;
    void paintLedPeakMeter(juce::Graphics& g,
                           const juce::Rectangle<int>& rect,
                           const MeterChannelState& state) const;
    void paintVoicePips(juce::Graphics& g,
                        const juce::Rectangle<int>& rect,
                        int maxPips,
                        int activeCount,
                        uint32_t onArgb) const;

    [[nodiscard]] std::unique_ptr<juce::Slider> makeRotaryKnob(const juce::String& name,
                                                               double minV,
                                                               double maxV,
                                                               double stepV,
                                                               double defaultV,
                                                               uint32_t accentArgb,
                                                               bool bipolar);

    [[nodiscard]] static juce::String formatOutAssignLabel(uint8_t outAssign) noexcept;
    [[nodiscard]] static juce::String formatRhythmEfctLabel(vf50::dsp::drums::RhythmEffectMode mode) noexcept;
    [[nodiscard]] static juce::String formatRecvChannelBadge(uint8_t ch) noexcept;
    [[nodiscard]] static juce::String formatBalanceText(float balNeg1ToPos1) noexcept;
    [[nodiscard]] static juce::String formatEqDbText(float db) noexcept;
    [[nodiscard]] static juce::String formatSignedInt(const juce::String& prefix, int val) noexcept;

    vf50::plugin::VF50AudioProcessor& processor_;
    vf50::classic::VF50ClassicController& classicController_;
    MixerFaderLookAndFeel mixerLookAndFeel_{};

    bool suppressCallbacks_ = false;
    bool drumSubMixVisible_ = false;
    DrumFilterMode drumFilterMode_ = DrumFilterMode::UsedInSong;
    uint32_t blinkCounter_  = 0u;
    bool blinkPhase_        = true;
    uint8_t totalFmVoices_  = 0u;
    uint8_t totalRhyVoices_ = 0u;

    juce::Rectangle<int> headerRect_{};
    juce::Rectangle<int> patchBadgeRect_{};
    juce::Rectangle<int> polyBadgeRect_{};
    std::unique_ptr<MixerTactileButton> btnClearSolo_{};
    std::unique_ptr<MixerTactileButton> btnResetMix_{};

    std::array<InstStrip, 8> instStrips_{};
    RhythmStrip rhyStrip_{};
    MasterStrip masterStrip_{};

    // Embedded 61-Drum Sub-Mix View controls
    juce::Rectangle<int> drumSubMixRect_{};
    juce::Rectangle<int> drumSubMixHeaderRect_{};
    std::unique_ptr<MixerTactileButton> btnDrumFilterUsed_{};
    std::unique_ptr<MixerTactileButton> btnDrumFilterAll_{};
    std::unique_ptr<MixerTactileButton> btnDrumFilterKsh_{};
    std::unique_ptr<MixerTactileButton> btnDrumBackToMixer_{};
    std::array<std::unique_ptr<DrumSubMixCell>, vf50::generated::kDrumInstrumentCount> drumCells_{};
    std::vector<std::size_t> visibleDrumIndices_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50MixerDrawer)
};

} // namespace vf50::gui
