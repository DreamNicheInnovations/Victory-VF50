#pragma once

#include "GUI/VF50DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>

namespace vf50::gui {

// =============================================================================
// Custom 2x Vector LookAndFeel_V4 for VF50 Hardware Faders, Menus & Fonts
// =============================================================================
class VF50LookAndFeel final : public juce::LookAndFeel_V4 {
public:
    VF50LookAndFeel();
    ~VF50LookAndFeel() override = default;

    [[nodiscard]] static juce::Font makeFont(float heightPx2x,
                                             bool bold = true,
                                             float horizontalScale = 1.0f);

    void setScaleFactor(float scaleFactor) noexcept;
    [[nodiscard]] float scaleFactor() const noexcept { return currentScaleFactor_; }

    static void drawHardwareLed(juce::Graphics& g,
                                juce::Point<float> center,
                                float lensRadius2x,
                                bool isRed,
                                bool isLit);

    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override;

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuSectionHeader(juce::Graphics& g,
                                    const juce::Rectangle<int>& area,
                                    const juce::String& sectionName) override;
    void drawPopupMenuItem(juce::Graphics& g,
                           const juce::Rectangle<int>& area,
                           bool isSeparator,
                           bool isActive,
                           bool isHighlighted,
                           bool isTicked,
                           bool hasSubMenu,
                           const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon,
                           const juce::Colour* textColourToUse) override;
    void getIdealPopupMenuItemSize(const juce::String& text,
                                   bool isSeparator,
                                   int standardMenuItemHeight,
                                   int& idealWidth,
                                   int& idealHeight) override;
    int getPopupMenuBorderSize() override;
    bool shouldPopupMenuScaleWithTargetComponent(const juce::PopupMenu::Options& options) override;
    juce::Font getPopupMenuFont() override;

private:
    float currentScaleFactor_ = tokens::kDefaultScaleFactor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50LookAndFeel)
};

// =============================================================================
// Sculpted 3D Hardware Tactile Button (Square, Soft-Key, Numeric Keypad, Pill)
// =============================================================================
class VF50PanelButton final : public juce::Button {
public:
    enum class CapStyle : uint8_t {
        SquareTactile = 0,
        SoftKeySlim,
        KeypadRect,
        UtilityPill
    };

    enum class TransportIcon : uint8_t {
        None = 0,
        RecordDot,
        RewindDouble,
        StopSquare,
        PlaySingle,
        ForwardDouble
    };

    VF50PanelButton(const juce::String& componentName, CapStyle style = CapStyle::SquareTactile);
    ~VF50PanelButton() override = default;

    void setCapLegends(juce::String primary,
                       uint32_t primaryArgb = tokens::kSilkWarmWhite,
                       juce::String secondary = {},
                       uint32_t secondaryArgb = tokens::kSilkWarmWhite,
                       juce::String tertiaryTeal = {},
                       uint32_t tertiaryArgb = tokens::kSilkAccentTeal);

    void setTransportIcon(TransportIcon icon, uint32_t iconArgb) noexcept;
    void setFontScale(float scale) noexcept { fontScale_ = scale; }
    void setActivePillHighlight(bool active) noexcept;

    void setRightClickLatchEnabled(bool enabled) noexcept;
    void setHoldLatched(bool latched, bool notifyCallback = false);
    [[nodiscard]] bool isHoldLatched() const noexcept { return holdLatched_; }
    void toggleHoldLatchFromInput();
    void triggerShiftLeftClick();

    std::function<void()> onPressDown;
    std::function<void()> onReleaseUp;
    std::function<bool()> canRightClickLatch;
    std::function<void(bool)> onHoldLatchChanged;
    std::function<void(const juce::MouseEvent&)> onShiftLeftClick;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    void paintButton(juce::Graphics& g,
                     bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override;

private:
    void drawTransportVectorIcon(juce::Graphics& g, juce::Rectangle<float> area, float alphaMul) const;

    CapStyle style_ = CapStyle::SquareTactile;
    juce::String primaryText_;
    juce::String secondaryText_;
    juce::String tertiaryText_;
    uint32_t primaryCol_   = tokens::kSilkWarmWhite;
    uint32_t secondaryCol_ = tokens::kSilkWarmWhite;
    uint32_t tertiaryCol_  = tokens::kSilkAccentTeal;
    TransportIcon icon_    = TransportIcon::None;
    uint32_t iconCol_      = tokens::kSilkWarmWhite;
    float fontScale_       = 1.0f;
    bool pillActive_       = false;
    bool rightClickLatchEnabled_ = false;
    bool holdLatched_            = false;
    bool suppressMouseUpAfterSpecialClick_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50PanelButton)
};

// =============================================================================
// Procedural 3D Ribbed Rubber Pitch (Spring-Loaded) & Modulation Wheel
// =============================================================================
class VF50WheelControl final : public juce::Component {
public:
    explicit VF50WheelControl(bool springReturnToZero);
    ~VF50WheelControl() override = default;

    // Bipolar [-1..+1] when springReturnToZero == true; Unipolar [0..+1] otherwise
    void setValue(float newVal, bool sendCallback) noexcept;
    [[nodiscard]] float value() const noexcept { return value_; }
    [[nodiscard]] bool isDraggingWheel() const noexcept { return dragging_; }

    std::function<void(float)> onValueChanged;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    bool springLoaded_ = false;
    bool dragging_     = false;
    float value_       = 0.0f;
    float dragStartVal_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50WheelControl)
};

} // namespace vf50::gui
