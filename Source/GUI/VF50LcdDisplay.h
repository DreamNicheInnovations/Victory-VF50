#pragma once

#include "GUI/VF50DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace vf50::gui {

class VF50LcdDisplay final : public juce::Component {
public:
    static constexpr int kCols    = 40;
    static constexpr int kRows    = 2;
    static constexpr int kDotCols = 5;
    static constexpr int kDotRows = 8;

    enum class CursorMode : uint8_t {
        None = 0,
        SolidUnderline,
        BlinkingUnderline,
        BlinkingBlock
    };

    VF50LcdDisplay();
    ~VF50LcdDisplay() override = default;

    // Update 40-column character rows (padded or truncated cleanly to 40 chars)
    void setLine(int row, std::string_view text) noexcept;
    void setLines(std::string_view row0, std::string_view row1) noexcept;
    void setChar(int row, int col, uint8_t hd44780Char) noexcept;
    [[nodiscard]] uint8_t getChar(int row, int col) const noexcept;
    [[nodiscard]] juce::String getLineAsString(int row) const;

    // Hardware HD44780 cursor control
    void setCursor(int row, int col, CursorMode mode) noexcept;
    void tickCursorBlink() noexcept;

    // Backlight theme control (0 = Factory Yellow-Green, 1 = Amber PMVA, 2 = Cool White/Blue)
    void setThemeIndex(int index) noexcept;
    [[nodiscard]] int themeIndex() const noexcept { return themeIndex_; }
    void cycleTheme() noexcept;
    [[nodiscard]] const char* currentThemeName() const noexcept;
    [[nodiscard]] juce::Rectangle<float> blBadgeBounds() const noexcept;
    [[nodiscard]] bool isBlBadgeHit(juce::Point<float> pt) const noexcept;
    [[nodiscard]] float paintedCharGridLeftXInParent() const noexcept;
    [[nodiscard]] float paintedCharGridRightXInParent() const noexcept;
    void handleThemeClickInteraction(juce::Point<float> pt, bool isRightClick) noexcept;

    // Interactive callbacks wired to VF50 menu state machine
    std::function<void(int row, int col)> onCellClicked;
    std::function<void(int col, int deltaSteps)> onCellValueScrub;
    std::function<void(int newThemeIdx)> onThemeChanged;

    // Lookup raw 5-bit row mask (bits 4..0 = left..right) from embedded HD44780 5x8 ROM
    [[nodiscard]] static const std::array<uint8_t, kDotRows>& getGlyphBitmap(uint8_t charCode) noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    static constexpr int kNativeWidth2x  = tokens::kLcdBezelRect2x.w; // 1236
    static constexpr int kNativeHeight2x = tokens::kLcdBezelRect2x.h; // 132
    static constexpr int kGlassX2x       = 60;
    static constexpr int kGlassY2x       = 16;
    static constexpr int kGlassW2x       = 1120;
    static constexpr int kGlassH2x       = 104;

    // Clean even-integer 2x dot-matrix metrics (zero fractional phase drift at 100% 0.5x & 200% 1.0x)
    static constexpr int kCharStepX2x    = 26;  // 13px @ 1x; 5 cols = 130px @ 2x (exact soft-key pitch)
    static constexpr int kDotPitchX2x    = 4;   // 2px @ 1x
    static constexpr int kDotWidth2x     = 3;   // 3px dot + 1px integer micro-gap @ 2x
    static constexpr int kDotPitchY2x    = 4;   // 2px @ 1x
    static constexpr int kDotHeight2x    = 3;   // 3px dot + 1px integer micro-gap @ 2x
    static constexpr int kGridOriginX2x  = 104; // 52px @ 1x (centers 40-col array inside 1120px glass)
    static constexpr int kRowOriginY0_2x = 32;  // 16px @ 1x
    static constexpr int kRowOriginY1_2x = 76;  // 38px @ 1x

    void rebuildLcdBuffer() const;
    [[nodiscard]] int columnFromLocalX(float x) const noexcept;
    [[nodiscard]] int rowFromLocalY(float y) const noexcept;

    std::array<std::array<uint8_t, kCols>, kRows> vram_{};

    int cursorRow_         = 0;
    int cursorCol_         = 0;
    CursorMode cursorMode_ = CursorMode::None;
    uint32_t blinkTick_    = 0u;
    bool blinkPhaseOn_     = true;

    int themeIndex_ = 0;

    juce::Rectangle<float> glassBounds_{};
    float gridOriginX_ = static_cast<float>(kGridOriginX2x);
    float charStepX_   = static_cast<float>(kCharStepX2x);

    mutable juce::Image lcdBuffer_{};   // 2x native integer dot-matrix buffer (1236 x 132)
    mutable juce::Image lcdBuffer1x_{}; // Exact 2x2 area-averaged 1x mip buffer (618 x 66)
    mutable bool lcdBufferDirty_ = true;

    int dragStartCol_   = -1;
    float dragAccumPx_  = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50LcdDisplay)
};

} // namespace vf50::gui
