#pragma once

#include "GUI/VF50DesignTokens.h"
#include "GUI/VF50LookAndFeel.h"
#include "Generated/VF50ParameterLayout.h"
#include "Plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace vf50::gui {

class VF50ProgrammerDrawer final : public juce::Component {
public:
    static constexpr std::size_t kGlobalFaderCount = 3u;
    static constexpr std::size_t kOpCount          = 4u;
    static constexpr std::size_t kFadersPerOp      = 12u;
    static constexpr std::size_t kPegLfoFaderCount = 12u;
    static constexpr std::size_t kTotalFaderCount  = kGlobalFaderCount + (kOpCount * kFadersPerOp) + kPegLfoFaderCount; // 63

    // Callback fired whenever any programmer control is adjusted by the user:
    // (paramIndex, operatorZeroBasedOrNeg1, lcdSummaryLine0, lcdSummaryLine1)
    std::function<void(vf50::params::ParamIndex paramIndex,
                       int operatorIndexOrNeg1,
                       const juce::String& lcdLine0,
                       const juce::String& lcdLine1)> onParameterAdjusted;

    explicit VF50ProgrammerDrawer(vf50::plugin::VF50AudioProcessor& processor);
    ~VF50ProgrammerDrawer() override;

    // Pulls all 63 fader values + switches/selectors from processor atomic APVTS bank without feedback loops
    void syncFromProcessor();

    // Programmatic accessors for verification & automation tests
    [[nodiscard]] std::size_t totalFaderCount() const noexcept { return kTotalFaderCount; }
    bool setFaderValueForParam(vf50::params::ParamIndex paramIndex, double displayValue, bool notify = true);
    [[nodiscard]] double getFaderValueForParam(vf50::params::ParamIndex paramIndex) const noexcept;
    void selectAlgorithmOneBased(int alg1To8, bool notify = true);
    [[nodiscard]] int selectedAlgorithmOneBased() const noexcept { return currentAlgZeroBased_ + 1; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    enum class AccentTheme : uint8_t {
        Teal    = 0,
        Amber   = 1,
        Crimson = 2,
        White   = 3
    };

    class PgFaderLookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        PgFaderLookAndFeel() = default;

        void drawLinearSlider(juce::Graphics& g,
                              int x, int y, int width, int height,
                              float sliderPos, float minSliderPos, float maxSliderPos,
                              juce::Slider::SliderStyle style,
                              juce::Slider& slider) override;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PgFaderLookAndFeel)
    };

    class PgMiniToggleButton final : public juce::Button {
    public:
        PgMiniToggleButton(const juce::String& name,
                           const juce::String& offLabel,
                           const juce::String& onLabel,
                           uint32_t accentArgb,
                           bool showLedDot = true);

        void paintButton(juce::Graphics& g, bool over, bool down) override;

    private:
        juce::String offText_;
        juce::String onText_;
        uint32_t accentArgb_ = 0xFF28D8E6u;
        bool showLedDot_ = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PgMiniToggleButton)
    };

    struct FaderSlot {
        std::unique_ptr<juce::Slider> slider{};
        vf50::params::ParamIndex       paramIndex = vf50::params::ParamIndex::VoiceAlgorithm;
        int                           opIndex    = -1; // -1 = Global/PEG/LFO, 0..3 = OP 1..4
        const char*                   shortLabel = "";
        const char*                   fullName   = "";
        double                        minVal     = 0.0;
        double                        maxVal     = 99.0;
        double                        defaultVal = 0.0;
        double                        centerVal  = -1000.0; // >= minVal enables center detent tick & bipolar fill
        AccentTheme                   accent     = AccentTheme::Teal;
        bool                          isWave1To8 = false;
        bool                          isVoiceLvl = false;
        juce::Rectangle<int>          labelRect{};
        juce::Rectangle<int>          readoutRect{};
    };

    struct OperatorControls {
        std::unique_ptr<PgMiniToggleButton> btnEnable{};
        std::unique_ptr<PgMiniToggleButton> btnFreqMode{};
        std::unique_ptr<PgMiniToggleButton> btnAme{};
        juce::Rectangle<int>                waveBoxRect{};
        juce::Rectangle<int>                bayBounds{};
    };

    void buildAllControls();
    void configureFader(std::size_t slotIdx,
                        vf50::params::ParamIndex paramIdx,
                        int opIdx,
                        const char* shortLbl,
                        const char* fullLbl,
                        double minV,
                        double maxV,
                        double defV,
                        double centerDetent,
                        AccentTheme accent,
                        bool wave1To8 = false,
                        bool voiceLvl = false);

    void handleFaderChanged(std::size_t slotIdx);
    void emitLcdJumpForParam(vf50::params::ParamIndex paramIdx,
                             int opIdx,
                             const juce::String& fullParamName,
                             const juce::String& formattedVal);

    [[nodiscard]] static juce::String formatFaderReadout(const FaderSlot& slot, double val);
    [[nodiscard]] static bool isOperatorCarrier(int algZeroBased, int opZeroBased) noexcept;

    void paintDrawerChassis(juce::Graphics& g) const;
    void paintBayFrame(juce::Graphics& g,
                       const juce::Rectangle<int>& bounds,
                       const juce::String& title,
                       uint32_t bracketArgb,
                       const juce::String& rightBadge = {},
                       uint32_t badgeArgb = 0u) const;
    void paintAlgorithmDiagram(juce::Graphics& g, const juce::Rectangle<float>& area, int algZeroBased) const;
    void paintOpzWaveformIcon(juce::Graphics& g, const juce::Rectangle<float>& area, int waveZeroBased, bool enabled) const;
    void paintPegPreview(juce::Graphics& g, const juce::Rectangle<float>& area) const;

    vf50::plugin::VF50AudioProcessor& processor_;
    PgFaderLookAndFeel pgLookAndFeel_{};
    bool suppressCallbacks_ = false;

    int currentAlgZeroBased_ = 0;
    int currentLfoWave_      = 0;

    juce::Rectangle<int> bay1Rect_{};
    juce::Rectangle<int> bay6Rect_{};
    juce::Rectangle<int> algDiagramRect_{};
    juce::Rectangle<int> pegPreviewRect_{};

    std::array<std::unique_ptr<juce::TextButton>, 8> algButtons_{};
    std::array<OperatorControls, kOpCount> opControls_{};
    std::array<std::unique_ptr<juce::TextButton>, 4> lfoWaveButtons_{};
    std::unique_ptr<PgMiniToggleButton> btnLfoSync_{};

    std::array<FaderSlot, kTotalFaderCount> faders_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50ProgrammerDrawer)
};

} // namespace vf50::gui
