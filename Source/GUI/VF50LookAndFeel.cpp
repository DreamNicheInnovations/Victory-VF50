#include "GUI/VF50LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace vf50::gui {

// =============================================================================
// VF50LookAndFeel
// =============================================================================
VF50LookAndFeel::VF50LookAndFeel() {
    setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xFF1A1E24u));
    setColour(juce::PopupMenu::textColourId, juce::Colour(tokens::kSilkWarmWhite));
    setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.26f));
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(tokens::kSilkAccentTeal));
}

juce::Font VF50LookAndFeel::makeFont(float heightPx2x, bool bold, float horizontalScale) {
    juce::Font font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(),
                                      heightPx2x,
                                      bold ? juce::Font::bold : juce::Font::plain));
    font.setHorizontalScale(horizontalScale);
    return font;
}

void VF50LookAndFeel::drawHardwareLed(juce::Graphics& g,
                                     juce::Point<float> center,
                                     float lensRadius2x,
                                     bool isRed,
                                     bool isLit) {
    // 1. Recessed circular bezel socket
    const float socketR = lensRadius2x + 2.5f;
    g.setColour(juce::Colour(0xFF080A0Cu));
    g.fillEllipse(center.x - socketR, center.y - socketR, socketR * 2.0f, socketR * 2.0f);
    g.setColour(juce::Colour(0xFF333942u));
    g.drawEllipse(center.x - socketR, center.y - socketR, socketR * 2.0f, socketR * 2.0f, 1.1f);

    if (isLit) {
        const uint32_t coreCol = isRed ? tokens::kLedRedCore : tokens::kLedGreenCore;
        const uint32_t midCol  = isRed ? tokens::kLedRedMid  : tokens::kLedGreenMid;
        const uint32_t haloCol = isRed ? tokens::kLedRedHalo : tokens::kLedGreenHalo;

        // 2. Multi-stop radial bloom halo onto surrounding anodized panel
        const float bloomR = lensRadius2x * 2.8f;
        juce::ColourGradient bloom(juce::Colour(haloCol),
                                   center.x, center.y,
                                   juce::Colours::transparentBlack,
                                   center.x + bloomR, center.y,
                                   true);
        bloom.addColour(0.38, juce::Colour(haloCol).withMultipliedAlpha(0.65f));
        g.setGradientFill(bloom);
        g.fillEllipse(center.x - bloomR, center.y - bloomR, bloomR * 2.0f, bloomR * 2.0f);

        // 3. Illuminated epoxy LED lens with hot core
        juce::ColourGradient lensGrad(juce::Colour(coreCol),
                                      center.x - lensRadius2x * 0.22f,
                                      center.y - lensRadius2x * 0.22f,
                                      juce::Colour(midCol).darker(0.35f),
                                      center.x + lensRadius2x,
                                      center.y + lensRadius2x,
                                      true);
        lensGrad.addColour(0.45, juce::Colour(midCol));
        g.setGradientFill(lensGrad);
        g.fillEllipse(center.x - lensRadius2x, center.y - lensRadius2x,
                      lensRadius2x * 2.0f, lensRadius2x * 2.0f);
    } else {
        // Authentic unlit dark-red / dark-green translucent epoxy lens
        const uint32_t unlitBase = isRed ? tokens::kLedRedUnlit : tokens::kLedGreenUnlit;
        juce::ColourGradient unlitGrad(juce::Colour(unlitBase).brighter(0.18f),
                                       center.x - lensRadius2x * 0.3f,
                                       center.y - lensRadius2x * 0.3f,
                                       juce::Colour(unlitBase).darker(0.45f),
                                       center.x + lensRadius2x,
                                       center.y + lensRadius2x,
                                       true);
        g.setGradientFill(unlitGrad);
        g.fillEllipse(center.x - lensRadius2x, center.y - lensRadius2x,
                      lensRadius2x * 2.0f, lensRadius2x * 2.0f);

        // Subtle glass specular dot
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillEllipse(center.x - lensRadius2x * 0.45f,
                      center.y - lensRadius2x * 0.50f,
                      lensRadius2x * 0.52f,
                      lensRadius2x * 0.42f);
    }
}

void VF50LookAndFeel::drawLinearSlider(juce::Graphics& g,
                                      int x, int y, int width, int height,
                                      float sliderPos,
                                      float /*minSliderPos*/,
                                      float /*maxSliderPos*/,
                                      juce::Slider::SliderStyle style,
                                      juce::Slider& /*slider*/) {
    if (style != juce::Slider::LinearVertical) {
        return;
    }

    const auto area = juce::Rectangle<float>(static_cast<float>(x),
                                             static_cast<float>(y),
                                             static_cast<float>(width),
                                             static_cast<float>(height));
    const float cx = area.getCentreX();
    const float capHalfH = 16.0f;
    const float trackTop = area.getY() + capHalfH;
    const float trackBottom = area.getBottom() - capHalfH;
    const float trackH = std::max(20.0f, trackBottom - trackTop);

    // 1. Calibrated Silkscreen Tick Marks on both sides of the recessed fader slot (11 divisions: 0..10)
    for (int i = 0; i <= 10; ++i) {
        const float norm = static_cast<float>(i) / 10.0f;
        const float ty = trackTop + norm * trackH;
        const bool isMajor = (i == 0 || i == 5 || i == 10);
        const float tickInner = 11.0f;
        const float tickOuter = isMajor ? 28.0f : 20.0f;
        const float strokeW   = isMajor ? 2.2f : 1.5f;

        g.setColour(juce::Colour(isMajor ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver)
                        .withAlpha(isMajor ? 0.90f : 0.62f));
        g.drawLine(cx - tickOuter, ty, cx - tickInner, ty, strokeW);
        g.drawLine(cx + tickInner, ty, cx + tickOuter, ty, strokeW);
    }

    // 2. Deep Recessed Vertical Fader Slot + Dust Felt Channel
    const juce::Rectangle<float> slotWell(cx - 7.5f, trackTop - 8.0f, 15.0f, trackH + 16.0f);
    g.setColour(juce::Colour(0xFF060809u));
    g.fillRoundedRectangle(slotWell, 4.0f);

    // Beveled slot rim
    g.setColour(juce::Colour(0xFF333942u));
    g.drawRoundedRectangle(slotWell.expanded(1.0f), 4.5f, 1.3f);

    // Inner metallic guide rail
    g.setColour(juce::Colour(0xFF1A1F25u));
    g.fillRect(cx - 2.0f, trackTop - 2.0f, 4.0f, trackH + 4.0f);

    // 3. Sculpted 3D VF50 Fader Cap with White Center Hairline
    const float clampedY = std::clamp(sliderPos, trackTop, trackBottom);
    const float capW = std::min(62.0f, area.getWidth() - 10.0f);
    const float capH = 32.0f;
    const juce::Rectangle<float> capRect(cx - capW * 0.5f, clampedY - capH * 0.5f, capW, capH);

    // Cast shadow beneath fader cap
    g.setColour(juce::Colours::black.withAlpha(0.62f));
    g.fillRoundedRectangle(capRect.translated(0.0f, 4.5f).expanded(1.5f, 1.0f), 4.0f);

    // Outer dark cap body
    g.setColour(juce::Colour(0xFF111418u));
    g.fillRoundedRectangle(capRect, 3.5f);

    // Upper angled bevel slope (catches overhead studio light)
    const juce::Rectangle<float> upperSlope(capRect.getX() + 2.0f, capRect.getY() + 2.0f,
                                            capRect.getWidth() - 4.0f, capH * 0.36f);
    juce::ColourGradient topSlopeGrad(juce::Colour(0xFF434A53u), upperSlope.getX(), upperSlope.getY(),
                                      juce::Colour(0xFF282D34u), upperSlope.getX(), upperSlope.getBottom(), false);
    g.setGradientFill(topSlopeGrad);
    g.fillRoundedRectangle(upperSlope, 2.0f);

    // Lower angled bevel slope
    const juce::Rectangle<float> lowerSlope(capRect.getX() + 2.0f, capRect.getBottom() - 2.0f - capH * 0.36f,
                                            capRect.getWidth() - 4.0f, capH * 0.36f);
    juce::ColourGradient botSlopeGrad(juce::Colour(0xFF23282Eu), lowerSlope.getX(), lowerSlope.getY(),
                                      juce::Colour(0xFF14171Bu), lowerSlope.getX(), lowerSlope.getBottom(), false);
    g.setGradientFill(botSlopeGrad);
    g.fillRoundedRectangle(lowerSlope, 2.0f);

    // Center concave saddle
    const juce::Rectangle<float> centerDeck(capRect.getX() + 2.0f, clampedY - 5.0f,
                                            capRect.getWidth() - 4.0f, 10.0f);
    g.setColour(juce::Colour(0xFF1B2026u));
    g.fillRect(centerDeck);

    // Crisp Warm-White Center Hairline Indicator
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.fillRect(capRect.getX() + 4.0f, clampedY - 1.4f, capRect.getWidth() - 8.0f, 2.8f);

    // Subtle 3D cap rim highlight
    g.setColour(juce::Colour(0xFF4D5560u).withAlpha(0.65f));
    g.drawLine(capRect.getX() + 3.0f, capRect.getY() + 1.5f,
               capRect.getRight() - 3.0f, capRect.getY() + 1.5f, 1.3f);
}

void VF50LookAndFeel::setScaleFactor(float scaleFactor) noexcept {
    currentScaleFactor_ = std::clamp(scaleFactor, tokens::kMinScaleFactor, tokens::kMaxScaleFactor);
}

void VF50LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    const float s = currentScaleFactor_;
    const float corner = 4.0f * s;
    const float stroke = std::max(1.0f, 1.5f * s);
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(juce::Colour(0xFF181C22u));
    g.fillRoundedRectangle(r, corner);
    g.setColour(juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.60f));
    g.drawRoundedRectangle(r.reduced(stroke * 0.5f), corner, stroke);
}

void VF50LookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g,
                                                const juce::Rectangle<int>& area,
                                                const juce::String& sectionName) {
    const float s = currentScaleFactor_;
    const int padX = std::max(6, static_cast<int>(std::lround(10.0f * s)));
    const int padY = std::max(2, static_cast<int>(std::lround(2.0f * s)));
    auto r = area.reduced(padX, padY);

    auto font = makeFont(13.5f * s, true);
    const float maxFontH = static_cast<float>(std::max(1, r.getHeight())) * 0.78f;
    if (font.getHeight() > maxFontH) {
        font.setHeight(maxFontH);
    }

    g.setFont(font);
    g.setColour(juce::Colour(tokens::kSilkAccentTeal));
    g.drawFittedText(sectionName, r, juce::Justification::centredLeft, 1);

    const float lineH = std::max(1.0f, 1.0f * s);
    g.setColour(juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.32f));
    g.fillRect(static_cast<float>(area.getX() + padX),
               static_cast<float>(area.getBottom()) - lineH,
               static_cast<float>(std::max(0, area.getWidth() - padX * 2)),
               lineH);
}

void VF50LookAndFeel::drawPopupMenuItem(juce::Graphics& g,
                                       const juce::Rectangle<int>& area,
                                       bool isSeparator,
                                       bool isActive,
                                       bool isHighlighted,
                                       bool isTicked,
                                       bool hasSubMenu,
                                       const juce::String& text,
                                       const juce::String& shortcutKeyText,
                                       const juce::Drawable* icon,
                                       const juce::Colour* textColourToUse) {
    const float s = currentScaleFactor_;

    if (isSeparator) {
        const int sepPadX = std::max(6, static_cast<int>(std::lround(8.0f * s)));
        const float lineH = std::max(1.0f, 1.0f * s);
        const float cy = static_cast<float>(area.getCentreY());
        g.setColour(findColour(juce::PopupMenu::textColourId).withAlpha(0.28f));
        g.fillRect(static_cast<float>(area.getX() + sepPadX),
                   cy - lineH * 0.5f,
                   static_cast<float>(std::max(0, area.getWidth() - sepPadX * 2)),
                   lineH);
        return;
    }

    const auto textColour = (textColourToUse == nullptr)
                                ? findColour(juce::PopupMenu::textColourId)
                                : *textColourToUse;

    const int inset = std::max(1, static_cast<int>(std::lround(2.0f * s)));
    auto r = area.reduced(inset, std::max(1, inset / 2));

    if (isHighlighted && isActive) {
        g.setColour(findColour(juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRoundedRectangle(r.toFloat(), 3.0f * s);
        g.setColour(findColour(juce::PopupMenu::highlightedTextColourId));
    } else {
        g.setColour(textColour.withMultipliedAlpha(isActive ? 1.0f : 0.48f));
    }

    const int sidePad = std::max(6, static_cast<int>(std::lround(8.0f * s)));
    r.reduce(sidePad, 0);

    auto font = getPopupMenuFont();
    const float maxFontHeight = static_cast<float>(std::max(1, r.getHeight())) / 1.25f;
    if (font.getHeight() > maxFontHeight) {
        font.setHeight(maxFontHeight);
    }
    g.setFont(font);

    const int iconWidth = std::max(10, static_cast<int>(std::lround(16.0f * s)));
    const int iconGap   = std::max(4, static_cast<int>(std::lround(6.0f * s)));
    const auto iconArea = r.removeFromLeft(iconWidth).toFloat();
    r.removeFromLeft(iconGap);

    if (icon != nullptr) {
        icon->drawWithin(g,
                         iconArea,
                         juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                         1.0f);
    } else if (isTicked) {
        const auto tick = getTickShape(1.0f);
        const auto tickBounds = iconArea.reduced(iconArea.getWidth() * 0.14f, iconArea.getHeight() * 0.22f);
        g.fillPath(tick, tick.getTransformToScaleToFit(tickBounds, true));
    }

    if (hasSubMenu) {
        const float arrowH = std::max(6.0f, 0.55f * font.getAscent());
        const float arrowX = static_cast<float>(r.removeFromRight(static_cast<int>(std::ceil(arrowH * 1.1f))).getX());
        const float halfH  = static_cast<float>(r.getCentreY());

        juce::Path path;
        path.startNewSubPath(arrowX, halfH - arrowH * 0.5f);
        path.lineTo(arrowX + arrowH * 0.6f, halfH);
        path.lineTo(arrowX, halfH + arrowH * 0.5f);
        g.strokePath(path, juce::PathStrokeType(std::max(1.2f, 1.8f * s)));
    }

    r.removeFromRight(std::max(3, static_cast<int>(std::lround(4.0f * s))));
    g.drawFittedText(text, r, juce::Justification::centredLeft, 1);

    if (shortcutKeyText.isNotEmpty()) {
        auto f2 = font;
        f2.setHeight(f2.getHeight() * 0.78f);
        f2.setHorizontalScale(0.95f);
        g.setFont(f2);
        g.drawText(shortcutKeyText, r, juce::Justification::centredRight, true);
    }
}

void VF50LookAndFeel::getIdealPopupMenuItemSize(const juce::String& text,
                                               bool isSeparator,
                                               int standardMenuItemHeight,
                                               int& idealWidth,
                                               int& idealHeight) {
    const float s = currentScaleFactor_;
    if (isSeparator) {
        idealWidth  = std::max(40, static_cast<int>(std::lround(60.0f * s)));
        idealHeight = std::max(6, static_cast<int>(std::lround(10.0f * s)));
        return;
    }

    auto font = getPopupMenuFont();
    const int targetH = (standardMenuItemHeight > 0)
                            ? standardMenuItemHeight
                            : std::max(18, static_cast<int>(std::lround(26.0f * s)));

    if (font.getHeight() > static_cast<float>(targetH) / 1.25f) {
        font.setHeight(static_cast<float>(targetH) / 1.25f);
    }

    idealHeight = targetH;
    const int iconAndPad = static_cast<int>(std::lround(48.0f * s));
    idealWidth = juce::GlyphArrangement::getStringWidthInt(font, text) + iconAndPad;
}

int VF50LookAndFeel::getPopupMenuBorderSize() {
    return std::max(2, static_cast<int>(std::lround(4.0f * currentScaleFactor_)));
}

bool VF50LookAndFeel::shouldPopupMenuScaleWithTargetComponent(const juce::PopupMenu::Options&) {
    // PopupMenu typography and item metrics are explicitly scaled by currentScaleFactor_;
    // disable JUCE's automatic target-component scaling so VF50MainCanvas's 2x->1x (0.5f)
    // virtual canvas transform never shrinks the popup menu by half.
    return false;
}

juce::Font VF50LookAndFeel::getPopupMenuFont() {
    constexpr float kBaseHeight = 15.0f;
    return makeFont(kBaseHeight * currentScaleFactor_, true);
}

// =============================================================================
// VF50PanelButton
// =============================================================================
VF50PanelButton::VF50PanelButton(const juce::String& componentName, CapStyle style)
    : juce::Button(componentName), style_(style) {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void VF50PanelButton::setCapLegends(juce::String primary,
                                   uint32_t primaryArgb,
                                   juce::String secondary,
                                   uint32_t secondaryArgb,
                                   juce::String tertiaryTeal,
                                   uint32_t tertiaryArgb) {
    primaryText_   = std::move(primary);
    primaryCol_    = primaryArgb;
    secondaryText_ = std::move(secondary);
    secondaryCol_  = secondaryArgb;
    tertiaryText_  = std::move(tertiaryTeal);
    tertiaryCol_   = tertiaryArgb;
    repaint();
}

void VF50PanelButton::setTransportIcon(TransportIcon icon, uint32_t iconArgb) noexcept {
    icon_    = icon;
    iconCol_ = iconArgb;
    repaint();
}

void VF50PanelButton::setActivePillHighlight(bool active) noexcept {
    if (pillActive_ != active) {
        pillActive_ = active;
        repaint();
    }
}

void VF50PanelButton::setRightClickLatchEnabled(bool enabled) noexcept {
    rightClickLatchEnabled_ = enabled;
    if (!enabled && holdLatched_) {
        holdLatched_ = false;
        repaint();
    }
}

void VF50PanelButton::setHoldLatched(bool latched, bool notifyCallback) {
    if (holdLatched_ == latched) {
        return;
    }
    holdLatched_ = latched;
    repaint();
    if (notifyCallback && onHoldLatchChanged) {
        onHoldLatchChanged(holdLatched_);
    }
}

void VF50PanelButton::toggleHoldLatchFromInput() {
    if (!rightClickLatchEnabled_) {
        return;
    }
    if (!holdLatched_ && canRightClickLatch && !canRightClickLatch()) {
        return;
    }
    setHoldLatched(!holdLatched_, true);
}

void VF50PanelButton::triggerShiftLeftClick() {
    if (!onShiftLeftClick) {
        return;
    }
    const auto center = getLocalBounds().toFloat().getCentre();
    const juce::MouseEvent dummy(
        juce::Desktop::getInstance().getMainMouseSource(),
        center,
        juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier),
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        this, this, juce::Time::getCurrentTime(),
        center, juce::Time::getCurrentTime(), 1, false);
    onShiftLeftClick(dummy);
}

void VF50PanelButton::mouseDown(const juce::MouseEvent& e) {
    suppressMouseUpAfterSpecialClick_ = false;

    if (e.mods.isPopupMenu()) {
        suppressMouseUpAfterSpecialClick_ = true;
        if (rightClickLatchEnabled_) {
            toggleHoldLatchFromInput();
        } else if (holdLatched_) {
            setHoldLatched(false, true);
        }
        return;
    }

    if (holdLatched_) {
        suppressMouseUpAfterSpecialClick_ = true;
        setHoldLatched(false, true);
        return;
    }

    if (e.mods.isShiftDown() && onShiftLeftClick) {
        suppressMouseUpAfterSpecialClick_ = true;
        onShiftLeftClick(e);
        return;
    }

    juce::Button::mouseDown(e);
    if (onPressDown) {
        onPressDown();
    }
}

void VF50PanelButton::mouseUp(const juce::MouseEvent& e) {
    if (suppressMouseUpAfterSpecialClick_ || e.mods.isPopupMenu()) {
        suppressMouseUpAfterSpecialClick_ = false;
        return;
    }
    if (onReleaseUp) {
        onReleaseUp();
    }
    juce::Button::mouseUp(e);
}

void VF50PanelButton::drawTransportVectorIcon(juce::Graphics& g,
                                             juce::Rectangle<float> area,
                                             float alphaMul) const {
    g.setColour(juce::Colour(iconCol_).withMultipliedAlpha(alphaMul));
    const float cx = area.getCentreX();
    const float cy = area.getCentreY();

    switch (icon_) {
        case TransportIcon::RecordDot: {
            const float r = 8.0f;
            g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
            break;
        }
        case TransportIcon::StopSquare: {
            const float s = 14.0f;
            g.fillRoundedRectangle(cx - s * 0.5f, cy - s * 0.5f, s, s, 1.5f);
            break;
        }
        case TransportIcon::PlaySingle: {
            juce::Path p;
            p.addTriangle(cx - 7.0f, cy - 8.5f, cx - 7.0f, cy + 8.5f, cx + 9.0f, cy);
            g.fillPath(p);
            break;
        }
        case TransportIcon::RewindDouble: {
            juce::Path p;
            p.addTriangle(cx + 1.0f, cy - 7.5f, cx + 1.0f, cy + 7.5f, cx - 11.0f, cy);
            p.addTriangle(cx + 12.0f, cy - 7.5f, cx + 12.0f, cy + 7.5f, cx, cy);
            g.fillPath(p);
            break;
        }
        case TransportIcon::ForwardDouble: {
            juce::Path p;
            p.addTriangle(cx - 12.0f, cy - 7.5f, cx - 12.0f, cy + 7.5f, cx, cy);
            p.addTriangle(cx - 1.0f, cy - 7.5f, cx - 1.0f, cy + 7.5f, cx + 11.0f, cy);
            g.fillPath(p);
            break;
        }
        case TransportIcon::None:
        default:
            break;
    }
}

void VF50PanelButton::paintButton(juce::Graphics& g,
                                 bool shouldDrawButtonAsHighlighted,
                                 bool shouldDrawButtonAsDown) {
    const auto fullBounds = getLocalBounds().toFloat();

    // Utility header scale pill button
    if (style_ == CapStyle::UtilityPill) {
        const auto pill = fullBounds.reduced(1.5f);
        const uint32_t bg = pillActive_
                                ? 0xFF0E3D3Au
                                : (shouldDrawButtonAsHighlighted ? 0xFF2B3139u : 0xFF1E2228u);
        g.setColour(juce::Colour(bg));
        g.fillRoundedRectangle(pill, 5.0f);

        g.setColour(juce::Colour(pillActive_ ? tokens::kSilkAccentTeal : 0xFF424A55u));
        g.drawRoundedRectangle(pill, 5.0f, pillActive_ ? 2.0f : 1.3f);

        g.setColour(juce::Colour(pillActive_ ? tokens::kSilkAccentTeal : tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(17.5f * fontScale_, true));
        g.drawText(primaryText_, pill.translated(0.0f, shouldDrawButtonAsDown ? 1.5f : 0.0f),
                   juce::Justification::centred, false);
        return;
    }

    const bool isVisuallyDown = shouldDrawButtonAsDown || holdLatched_;

    // Physical 1-pixel downward press shift (@1x = 2.0f virtual pixels @2x)
    const float pressShiftY = (isVisuallyDown || getToggleState()) ? 2.0f : 0.0f;

    // 1. Recessed Dark Mounting Well
    const float wellCorner = (style_ == CapStyle::SoftKeySlim) ? 4.0f : 5.5f;
    g.setColour(juce::Colour(tokens::kButtonMountingWell));
    g.fillRoundedRectangle(fullBounds, wellCorner);

    // Subtle outer chassis bevel around mounting well
    g.setColour(juce::Colour(0xFF343A43u).withAlpha(0.55f));
    g.drawLine(fullBounds.getX() + 3.0f, fullBounds.getBottom() - 0.8f,
               fullBounds.getRight() - 3.0f, fullBounds.getBottom() - 0.8f, 1.3f);

    // 2. Sculpted 3D Matte Cap Body inside Well
    const float wellInset = (style_ == CapStyle::SoftKeySlim) ? 2.5f : 3.5f;
    const auto capRect = fullBounds.reduced(wellInset).translated(0.0f, pressShiftY);

    if (!isVisuallyDown) {
        g.setColour(juce::Colour(tokens::kButtonBevelShadow));
        g.fillRoundedRectangle(capRect.translated(0.0f, 2.2f), wellCorner - 1.5f);
    }

    const uint32_t topCol = (style_ == CapStyle::SquareTactile) ? tokens::kSquareCapTop : tokens::kKeypadCapTop;
    const uint32_t midCol = (style_ == CapStyle::SquareTactile) ? tokens::kSquareCapMid : tokens::kKeypadCapMid;
    const uint32_t botCol = (style_ == CapStyle::SquareTactile) ? tokens::kSquareCapBottom : tokens::kKeypadCapBottom;

    juce::Colour topShade(topCol);
    juce::Colour midShade(midCol);
    juce::Colour botShade(botCol);
    if (shouldDrawButtonAsHighlighted && !isVisuallyDown) {
        topShade = topShade.brighter(0.11f);
        midShade = midShade.brighter(0.08f);
    } else if (isVisuallyDown) {
        topShade = topShade.darker(0.14f);
        midShade = midShade.darker(0.10f);
    }

    juce::ColourGradient capGrad(topShade, capRect.getX(), capRect.getY(),
                                 botShade, capRect.getX(), capRect.getBottom(), false);
    capGrad.addColour(0.48, midShade);
    g.setGradientFill(capGrad);
    g.fillRoundedRectangle(capRect, wellCorner - 1.5f);

    // 3D top/left chamfer highlight & bottom/right chamfer shadow
    const auto bevelRect = capRect.reduced(1.2f);
    g.setColour(juce::Colour(tokens::kButtonBevelHighlight).withAlpha(isVisuallyDown ? 0.35f : 0.85f));
    g.drawLine(bevelRect.getX() + 2.0f, bevelRect.getY(),
               bevelRect.getRight() - 2.0f, bevelRect.getY(), 1.8f);
    g.drawLine(bevelRect.getX(), bevelRect.getY() + 2.0f,
               bevelRect.getX(), bevelRect.getBottom() - 2.0f, 1.4f);

    g.setColour(juce::Colour(0xFF0D0F12u).withAlpha(0.85f));
    g.drawLine(bevelRect.getX() + 2.0f, bevelRect.getBottom(),
               bevelRect.getRight() - 2.0f, bevelRect.getBottom(), 1.8f);

    if (shouldDrawButtonAsHighlighted && !holdLatched_) {
        g.setColour(juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.28f));
        g.drawRoundedRectangle(capRect.reduced(0.8f), wellCorner - 1.5f, 1.5f);
    }

    if (holdLatched_) {
        const float ringRadius = std::max(1.5f, wellCorner - 1.5f);
        g.setColour(juce::Colour(0x55F2B632u));
        g.drawRoundedRectangle(capRect.expanded(0.8f), ringRadius + 0.8f, 3.2f);
        g.setColour(juce::Colour(0xFFF2B632u));
        g.drawRoundedRectangle(capRect.reduced(0.4f), ringRadius, 1.8f);
    } else if (pillActive_) {
        const float ringRadius = std::max(1.5f, wellCorner - 1.5f);
        g.setColour(juce::Colour(tokens::kMixerTealAccent).withAlpha(0.42f));
        g.drawRoundedRectangle(capRect.expanded(0.8f), ringRadius + 0.8f, 3.0f);
        g.setColour(juce::Colour(tokens::kMixerTealAccent));
        g.drawRoundedRectangle(capRect.reduced(0.4f), ringRadius, 1.6f);
    }

    // 3. Crisp 2x Vector Cap Typography & Hardware Symbols
    const float alphaMul = isVisuallyDown ? 0.92f : 1.0f;
    const auto textPad = capRect.reduced(5.0f, 3.0f);

    if (style_ == CapStyle::KeypadRect) {
        if (tertiaryText_ == "DEC_KEY" || tertiaryText_ == "INC_KEY") {
            // [-1 / NO <] (Ochre Gold) and [+1 / YES >] (Accent Teal) keys
            g.setColour(juce::Colour(primaryCol_).withMultipliedAlpha(alphaMul));
            g.setFont(VF50LookAndFeel::makeFont(22.0f * fontScale_, true));
            g.drawText(primaryText_,
                       juce::Rectangle<float>(textPad.getX() + 2.0f, textPad.getY() + 2.0f,
                                              textPad.getWidth() * 0.48f, textPad.getHeight() * 0.55f),
                       juce::Justification::centredLeft, false);

            g.setColour(juce::Colour(secondaryCol_).withMultipliedAlpha(alphaMul));
            g.setFont(VF50LookAndFeel::makeFont(16.5f * fontScale_, true, 0.94f));
            g.drawText(secondaryText_,
                       juce::Rectangle<float>(textPad.getCentreX() - 4.0f, textPad.getY() + 4.0f,
                                              textPad.getWidth() * 0.52f, textPad.getHeight() * 0.48f),
                       juce::Justification::centredRight, false);

            // Vector directional triangle at bottom right
            juce::Path arrow;
            const float ay = textPad.getBottom() - 10.0f;
            const float ax = textPad.getRight() - 12.0f;
            if (tertiaryText_ == "DEC_KEY") {
                arrow.addTriangle(ax + 5.0f, ay - 6.0f, ax + 5.0f, ay + 6.0f, ax - 6.0f, ay);
            } else {
                arrow.addTriangle(ax - 5.0f, ay - 6.0f, ax - 5.0f, ay + 6.0f, ax + 6.0f, ay);
            }
            g.setColour(juce::Colour(primaryCol_).withMultipliedAlpha(alphaMul));
            g.fillPath(arrow);
            return;
        }

        // Standard numeric keypad key (0..9 + alphanumeric + teal musical symbol)
        g.setColour(juce::Colour(primaryCol_).withMultipliedAlpha(alphaMul));
        g.setFont(VF50LookAndFeel::makeFont(27.0f * fontScale_, true));
        g.drawText(primaryText_,
                   juce::Rectangle<float>(textPad.getX() + 3.0f, textPad.getY(),
                                          textPad.getWidth() * 0.42f, textPad.getHeight() * 0.72f),
                   juce::Justification::centredLeft, false);

        if (secondaryText_.isNotEmpty()) {
            g.setColour(juce::Colour(secondaryCol_).withMultipliedAlpha(alphaMul * 0.92f));
            g.setFont(VF50LookAndFeel::makeFont(15.0f * fontScale_, true, 0.92f));
            g.drawText(secondaryText_,
                       juce::Rectangle<float>(textPad.getX() + textPad.getWidth() * 0.36f, textPad.getY() + 2.0f,
                                              textPad.getWidth() * 0.64f, textPad.getHeight() * 0.46f),
                       juce::Justification::centredRight, false);
        }

        if (tertiaryText_.isNotEmpty()) {
            g.setColour(juce::Colour(tertiaryCol_).withMultipliedAlpha(alphaMul));
            const auto botRight = juce::Rectangle<float>(textPad.getX() + textPad.getWidth() * 0.32f,
                                                         textPad.getY() + textPad.getHeight() * 0.50f,
                                                         textPad.getWidth() * 0.68f,
                                                         textPad.getHeight() * 0.50f);
            if (tertiaryText_.startsWith("NOTE_")) {
                // Procedural vector musical note duration symbol (16th, 8th, quarter)
                const float nx = botRight.getRight() - 12.0f;
                const float ny = botRight.getBottom() - 6.0f;
                g.fillEllipse(nx - 6.0f, ny - 4.5f, 8.0f, 6.0f);
                g.drawLine(nx + 1.2f, ny - 1.5f, nx + 1.2f, ny - 16.0f, 2.0f);
                if (tertiaryText_ == "NOTE_8" || tertiaryText_ == "NOTE_16") {
                    g.drawLine(nx + 1.2f, ny - 16.0f, nx + 7.5f, ny - 12.0f, 2.0f);
                }
                if (tertiaryText_ == "NOTE_16") {
                    g.drawLine(nx + 1.2f, ny - 11.5f, nx + 7.5f, ny - 7.5f, 2.0f);
                }
            } else {
                g.setFont(VF50LookAndFeel::makeFont(14.5f * fontScale_, true, 0.92f));
                g.drawText(tertiaryText_, botRight, juce::Justification::centredRight, false);
            }
        }
        return;
    }

    if (icon_ != TransportIcon::None) {
        if (primaryText_.isNotEmpty()) {
            const auto topHalf = juce::Rectangle<float>(textPad.getX(), textPad.getY() + 2.0f,
                                                        textPad.getWidth(), textPad.getHeight() * 0.44f);
            const auto botHalf = juce::Rectangle<float>(textPad.getX(), textPad.getY() + textPad.getHeight() * 0.44f,
                                                        textPad.getWidth(), textPad.getHeight() * 0.54f);
            g.setColour(juce::Colour(primaryCol_).withMultipliedAlpha(alphaMul));
            g.setFont(VF50LookAndFeel::makeFont(14.5f * fontScale_, true, 0.92f));
            g.drawText(primaryText_, topHalf, juce::Justification::centred, false);
            drawTransportVectorIcon(g, botHalf, alphaMul);
        } else {
            drawTransportVectorIcon(g, textPad, alphaMul);
        }
        return;
    }

    if (style_ == CapStyle::SoftKeySlim) {
        // Subtle tactile horizontal ridge in the middle of the soft key cap
        g.setColour(juce::Colour(0xFF39404Au).withAlpha(0.45f));
        g.drawLine(capRect.getX() + 18.0f, capRect.getCentreY(),
                   capRect.getRight() - 18.0f, capRect.getCentreY(), 1.6f);
        return;
    }

    // Square Tactile Cap with 1 or 2 silkscreen text lines
    if (secondaryText_.isNotEmpty()) {
        const auto topLine = juce::Rectangle<float>(textPad.getX(), textPad.getY() + 5.0f,
                                                    textPad.getWidth(), textPad.getHeight() * 0.46f);
        const auto botLine = juce::Rectangle<float>(textPad.getX(), textPad.getCentreY() - 1.0f,
                                                    textPad.getWidth(), textPad.getHeight() * 0.46f);
        g.setColour(juce::Colour(primaryCol_).withMultipliedAlpha(alphaMul));
        g.setFont(VF50LookAndFeel::makeFont(15.5f * fontScale_, true, 0.90f));
        g.drawText(primaryText_, topLine, juce::Justification::centred, false);

        g.setColour(juce::Colour(secondaryCol_).withMultipliedAlpha(alphaMul));
        g.setFont(VF50LookAndFeel::makeFont(15.5f * fontScale_, true, 0.90f));
        g.drawText(secondaryText_, botLine, juce::Justification::centred, false);
    } else if (primaryText_.isNotEmpty()) {
        g.setColour(juce::Colour(primaryCol_).withMultipliedAlpha(alphaMul));
        g.setFont(VF50LookAndFeel::makeFont(16.5f * fontScale_, true, 0.92f));
        g.drawFittedText(primaryText_, textPad.toNearestInt(), juce::Justification::centred, 2, 0.85f);
    }
}

// =============================================================================
// VF50WheelControl (Ribbed Rubber Pitch & Modulation Wheels w/ Dynamic 3D Shading)
// =============================================================================
VF50WheelControl::VF50WheelControl(bool springReturnToZero)
    : springLoaded_(springReturnToZero),
      value_(0.0f) {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

void VF50WheelControl::setValue(float newVal, bool sendCallback) noexcept {
    const float minV = springLoaded_ ? -1.0f : 0.0f;
    const float clamped = std::clamp(newVal, minV, 1.0f);
    if (std::abs(clamped - value_) > 1e-4f) {
        value_ = clamped;
        repaint();
        if (sendCallback && onValueChanged) {
            onValueChanged(value_);
        }
    }
}

void VF50WheelControl::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat();

    // 1. Deep Recessed Wheel Well Cavity
    g.setColour(juce::Colour(0xFF060709u));
    g.fillRoundedRectangle(b, 6.0f);

    // Beveled housing rim
    g.setColour(juce::Colour(0xFF343B44u));
    g.drawRoundedRectangle(b.reduced(1.0f), 5.5f, 1.6f);

    // 2. Cylindrical 3D Rubber Wheel Barrel with Dynamic Shading that Shifts with Rotation
    const auto barrel = b.reduced(8.0f, 8.0f);
    const float norm01 = springLoaded_
                             ? ((1.0f - value_) * 0.5f)  // +1.0 = top, 0.0 = center, -1.0 = bottom
                             : (1.0f - value_);          // +1.0 = top, 0.0 = bottom

    // Specular zenith shifts subtly as wheel rotates toward top or bottom
    const float specPeak = std::clamp(0.28f + (norm01 - 0.5f) * 0.24f, 0.14f, 0.78f);

    juce::ColourGradient barrelGrad(juce::Colour(0xFF0D0F12u), barrel.getX(), barrel.getY(),
                                    juce::Colour(0xFF090B0Du), barrel.getX(), barrel.getBottom(), false);
    barrelGrad.addColour(std::clamp(specPeak - 0.14, 0.04, 0.92), juce::Colour(0xFF282D34u));
    barrelGrad.addColour(specPeak, juce::Colour(0xFF454C56u));
    barrelGrad.addColour(std::clamp(specPeak + 0.18, 0.08, 0.96), juce::Colour(0xFF242930u));
    g.setGradientFill(barrelGrad);
    g.fillRoundedRectangle(barrel, 5.0f);

    // 3. Procedural Ribbed Horizontal Rubber Ridges Projected Around Cylinder Circumference
    {
        juce::Graphics::ScopedSaveState clipGuard(g);
        g.reduceClipRegion(barrel.toNearestInt());

        constexpr float kHalfPi = 1.5707963268f;
        // Rotation angle offset in radians driven by current wheel position
        const float rotOffsetRad = (0.5f - norm01) * 1.18f;
        constexpr int kNumRibs = 25;

        for (int i = -kNumRibs; i <= kNumRibs; ++i) {
            const float theta = static_cast<float>(i) * 0.135f + rotOffsetRad;
            if (theta <= -1.36f || theta >= 1.36f) {
                continue;
            }
            const float sinT = std::sin(theta);
            const float cosT = std::cos(theta); // foreshortening & illumination intensity
            const float ry = barrel.getCentreY() + sinT * (barrel.getHeight() * 0.50f);
            const float ribH = std::max(1.2f, 2.8f * cosT);

            // Rib shadow groove
            g.setColour(juce::Colours::black.withAlpha(0.55f * cosT));
            g.fillRect(barrel.getX() + 2.0f, ry - ribH * 0.5f, barrel.getWidth() - 4.0f, ribH);

            // Rib catchlight ridge
            g.setColour(juce::Colour(0xFF5D6673u).withAlpha(0.42f * cosT * cosT));
            g.fillRect(barrel.getX() + 2.0f, ry + ribH * 0.5f, barrel.getWidth() - 4.0f, 1.3f * cosT);
        }

        // 4. Engraved White/Silver Position Indicator Notch Rotating on Cylinder
        const float notchTheta = std::clamp((norm01 - 0.5f) * (kHalfPi * 1.32f), -1.28f, 1.28f);
        const float notchY = barrel.getCentreY() + std::sin(notchTheta) * (barrel.getHeight() * 0.47f);
        const float notchVis = std::max(0.25f, std::cos(notchTheta));
        const float notchH = std::max(3.0f, 6.2f * notchVis);

        g.setColour(juce::Colour(0xFF0A0C0Eu));
        g.fillRect(barrel.getX() + 1.0f, notchY - notchH * 0.7f, barrel.getWidth() - 2.0f, notchH * 1.4f);

        g.setColour(juce::Colour(tokens::kSilkWarmWhite).withAlpha(0.92f * notchVis));
        g.fillRoundedRectangle(barrel.getX() + 4.0f, notchY - notchH * 0.35f,
                               barrel.getWidth() - 8.0f, std::max(2.2f, notchH * 0.70f), 1.2f);

        // Top & bottom curved cylinder end-shadow vignettes inside well
        juce::ColourGradient topShad(juce::Colours::black.withAlpha(0.78f),
                                     barrel.getX(), barrel.getY(),
                                     juce::Colours::transparentBlack,
                                     barrel.getX(), barrel.getY() + 18.0f, false);
        g.setGradientFill(topShad);
        g.fillRect(barrel.getX(), barrel.getY(), barrel.getWidth(), 18.0f);

        juce::ColourGradient botShad(juce::Colours::black.withAlpha(0.82f),
                                     barrel.getX(), barrel.getBottom(),
                                     juce::Colours::transparentBlack,
                                     barrel.getX(), barrel.getBottom() - 18.0f, false);
        g.setGradientFill(botShad);
        g.fillRect(barrel.getX(), barrel.getBottom() - 18.0f, barrel.getWidth(), 18.0f);
    }

    // Center zero calibration tick mark on left bezel of spring-loaded Pitch Wheel
    if (springLoaded_) {
        const float midY = b.getCentreY();
        g.setColour(juce::Colour(tokens::kSilkWarmWhite).withAlpha(0.75f));
        g.fillRect(b.getX() + 1.5f, midY - 1.2f, 5.0f, 2.4f);
        g.fillRect(b.getRight() - 6.5f, midY - 1.2f, 5.0f, 2.4f);
    }
}

void VF50WheelControl::mouseDown(const juce::MouseEvent&) {
    dragging_     = true;
    dragStartVal_ = value_;
}

void VF50WheelControl::mouseDrag(const juce::MouseEvent& event) {
    const float spanPx = std::max(40.0f, static_cast<float>(getHeight()) - 24.0f);
    const float range  = springLoaded_ ? 2.0f : 1.0f;
    const float delta  = (-static_cast<float>(event.getDistanceFromDragStartY()) / spanPx) * range;
    setValue(dragStartVal_ + delta, true);
}

void VF50WheelControl::mouseUp(const juce::MouseEvent&) {
    dragging_ = false;
    if (springLoaded_) {
        // Spring-loaded return to zero center upon mouse release!
        setValue(0.0f, true);
    }
}

void VF50WheelControl::mouseDoubleClick(const juce::MouseEvent&) {
    setValue(0.0f, true);
}

void VF50WheelControl::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
    if (springLoaded_) {
        return; // Keep spring-loaded pitch wheel centered unless actively held
    }
    setValue(value_ + wheel.deltaY * 0.12f, true);
}

} // namespace vf50::gui
