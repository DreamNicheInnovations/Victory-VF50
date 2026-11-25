#include "GUI/VF50MixerDrawer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vf50::gui {

namespace {

constexpr uint32_t kColDrawerTop      = 0xFF1A1E24u;
constexpr uint32_t kColDrawerBot      = 0xFF111419u;
constexpr uint32_t kColStripFill      = 0xFF161A20u;
constexpr uint32_t kColStripBorder    = 0xFF2B313Bu;
constexpr uint32_t kColWellBg         = 0xFF0A0C0Fu;
constexpr uint32_t kColMuteRed        = 0xFFFF5238u;
constexpr uint32_t kColSoloTeal       = tokens::kMixerTealAccent;
constexpr uint32_t kColSoloAmber      = 0xFFF2B632u;
constexpr uint32_t kColSegGreenOn     = 0xFF2EE85Cu;
constexpr uint32_t kColSegAmberOn     = 0xFFF5B025u;
constexpr uint32_t kColSegRedOn       = 0xFFFF362Cu;
constexpr uint32_t kColSegOff         = 0xFF1B2027u;

[[nodiscard]] float meterNormFromLinear(float peak) noexcept {
    if (peak <= 1e-5f) {
        return 0.0f;
    }
    const float db = 20.0f * std::log10(std::clamp(peak, 1e-5f, 1.4f));
    return std::clamp((db + 42.0f) / 42.0f, 0.0f, 1.0f);
}

[[nodiscard]] uint8_t nextOutAssignValue(uint8_t cur) noexcept {
    switch (cur & 0x03u) {
        case 3u: return 1u; // L+R -> L
        case 1u: return 2u; // L   -> R
        case 2u: return 0u; // R   -> OFF
        default: return 3u; // OFF -> L+R
    }
}

[[nodiscard]] vf50::dsp::drums::RhythmEffectMode nextRhythmEfctValue(vf50::dsp::drums::RhythmEffectMode cur) noexcept {
    switch (cur) {
        case vf50::dsp::drums::RhythmEffectMode::Off: return vf50::dsp::drums::RhythmEffectMode::On;
        case vf50::dsp::drums::RhythmEffectMode::On:  return vf50::dsp::drums::RhythmEffectMode::Mix;
        default:                                     return vf50::dsp::drums::RhythmEffectMode::Off;
    }
}

[[nodiscard]] juce::String formatPanStepLabel(int panNeg3ToPos3) noexcept {
    const int p = std::clamp(panNeg3ToPos3, -3, 3);
    if (p == 0) {
        return "C";
    }
    if (p < 0) {
        return "L" + juce::String(-p);
    }
    return "R" + juce::String(p);
}

[[nodiscard]] bool isKickSnareHatName(const juce::String& name) noexcept {
    const auto upper = name.toUpperCase();
    return upper.contains("BD") || upper.contains("B.D")
        || upper.contains("SD") || upper.contains("S.D")
        || upper.contains("HH") || upper.contains("HAT")
        || upper.contains("RIM") || upper.contains("CLAP")
        || upper.contains("KICK") || upper.contains("SNAR");
}

} // namespace

// =============================================================================
// MixerFaderLookAndFeel
// =============================================================================
void VF50MixerDrawer::MixerFaderLookAndFeel::drawLinearSlider(juce::Graphics& g,
                                                             int x, int y, int width, int height,
                                                             float sliderPos,
                                                             float /*minSliderPos*/,
                                                             float /*maxSliderPos*/,
                                                             juce::Slider::SliderStyle style,
                                                             juce::Slider& slider) {
    const auto bounds = juce::Rectangle<float>(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height));

    const uint32_t accentArgb = static_cast<uint32_t>(
        static_cast<int64_t>(slider.getProperties().getWithDefault("mixAccent", static_cast<int64_t>(kColSoloTeal))));
    const juce::Colour accent(accentArgb);

    if (style == juce::Slider::LinearHorizontal) {
        const float cy = bounds.getCentreY();
        const float leftX = bounds.getX() + 14.0f;
        const float rightX = bounds.getRight() - 14.0f;
        const float trackW = std::max(16.0f, rightX - leftX);
        const float clampedX = std::clamp(sliderPos, leftX, rightX);

        const juce::Rectangle<float> slotRect(leftX - 4.0f, cy - 5.0f, trackW + 8.0f, 10.0f);
        g.setColour(juce::Colour(0xFF06080Au));
        g.fillRoundedRectangle(slotRect, 3.0f);
        g.setColour(juce::Colour(0xFF313842u));
        g.drawRoundedRectangle(slotRect, 3.0f, 1.1f);

        const float fillW = clampedX - leftX;
        if (fillW > 1.0f) {
            g.setColour(accent.withAlpha(0.55f));
            g.fillRoundedRectangle(leftX, cy - 2.2f, fillW, 4.4f, 1.5f);
        }

        const float capW = 18.0f;
        const float capH = std::min(26.0f, bounds.getHeight() - 2.0f);
        const juce::Rectangle<float> capRect(clampedX - capW * 0.5f, cy - capH * 0.5f, capW, capH);
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(capRect.translated(0.0f, 2.0f), 3.0f);

        juce::ColourGradient capGrad(juce::Colour(0xFF424953u), capRect.getX(), capRect.getY(),
                                     juce::Colour(0xFF1D2229u), capRect.getX(), capRect.getBottom(), false);
        g.setGradientFill(capGrad);
        g.fillRoundedRectangle(capRect, 3.0f);
        g.setColour(juce::Colour(0xFF56606Du));
        g.drawRoundedRectangle(capRect, 3.0f, 1.1f);

        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.fillRect(clampedX - 1.1f, capRect.getY() + 3.0f, 2.2f, capRect.getHeight() - 6.0f);
        return;
    }

    if (style != juce::Slider::LinearVertical) {
        return;
    }

    const float cx = bounds.getCentreX();
    const float capHalfH = 14.0f;
    const float trackTop = bounds.getY() + capHalfH;
    const float trackBot = bounds.getBottom() - capHalfH;
    const float trackH = std::max(24.0f, trackBot - trackTop);

    // 1. Calibrated silkscreen ticks on both sides of slot (0..10)
    for (int t = 0; t <= 10; ++t) {
        const float ty = trackBot - (static_cast<float>(t) / 10.0f) * trackH;
        const bool isMajor = (t == 0 || t == 5 || t == 10);
        const float tickInner = 9.0f;
        const float tickOuter = isMajor ? 22.0f : 16.0f;
        g.setColour(juce::Colour(isMajor ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver)
                        .withAlpha(isMajor ? 0.78f : 0.45f));
        g.drawLine(cx - tickOuter, ty, cx - tickInner, ty, isMajor ? 1.8f : 1.2f);
        g.drawLine(cx + tickInner, ty, cx + tickOuter, ty, isMajor ? 1.8f : 1.2f);
    }

    // 2. Brushed recessed slot well & dust channel
    const juce::Rectangle<float> slotWell(cx - 6.0f, trackTop - 6.0f, 12.0f, trackH + 12.0f);
    g.setColour(juce::Colour(0xFF06080Au));
    g.fillRoundedRectangle(slotWell, 3.5f);
    g.setColour(juce::Colour(0xFF313842u));
    g.drawRoundedRectangle(slotWell.expanded(0.8f), 4.0f, 1.2f);

    const float clampedY = std::clamp(sliderPos, trackTop, trackBot);
    const float fillH = trackBot - clampedY;
    if (fillH > 1.0f) {
        g.setColour(accent.withAlpha(0.45f));
        g.fillRoundedRectangle(cx - 2.0f, clampedY, 4.0f, fillH, 1.5f);
    }

    // 3. Sculpted 3D VF50 hardware fader cap with crisp warm-white hairline
    const float capW = std::clamp(bounds.getWidth() - 10.0f, 36.0f, 56.0f);
    const float capH = 28.0f;
    const juce::Rectangle<float> capRect(cx - capW * 0.5f, clampedY - capH * 0.5f, capW, capH);

    g.setColour(juce::Colours::black.withAlpha(0.60f));
    g.fillRoundedRectangle(capRect.translated(0.0f, 3.5f).expanded(1.2f, 0.8f), 3.5f);

    g.setColour(juce::Colour(0xFF111418u));
    g.fillRoundedRectangle(capRect, 3.2f);

    const juce::Rectangle<float> upperSlope(capRect.getX() + 2.0f, capRect.getY() + 2.0f,
                                            capRect.getWidth() - 4.0f, capH * 0.36f);
    juce::ColourGradient topGrad(juce::Colour(0xFF434A53u), upperSlope.getX(), upperSlope.getY(),
                                 juce::Colour(0xFF282D34u), upperSlope.getX(), upperSlope.getBottom(), false);
    g.setGradientFill(topGrad);
    g.fillRoundedRectangle(upperSlope, 2.0f);

    const juce::Rectangle<float> lowerSlope(capRect.getX() + 2.0f, capRect.getBottom() - 2.0f - capH * 0.36f,
                                            capRect.getWidth() - 4.0f, capH * 0.36f);
    juce::ColourGradient botGrad(juce::Colour(0xFF23282Eu), lowerSlope.getX(), lowerSlope.getY(),
                                 juce::Colour(0xFF14171Bu), lowerSlope.getX(), lowerSlope.getBottom(), false);
    g.setGradientFill(botGrad);
    g.fillRoundedRectangle(lowerSlope, 2.0f);

    g.setColour(juce::Colour(0xFF1B2026u));
    g.fillRect(capRect.getX() + 2.0f, clampedY - 4.5f, capRect.getWidth() - 4.0f, 9.0f);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.fillRect(capRect.getX() + 4.0f, clampedY - 1.3f, capRect.getWidth() - 8.0f, 2.6f);

    g.setColour(juce::Colour(0xFF535C68u).withAlpha(0.75f));
    g.drawLine(capRect.getX() + 3.0f, capRect.getY() + 1.4f,
               capRect.getRight() - 3.0f, capRect.getY() + 1.4f, 1.2f);
}

void VF50MixerDrawer::MixerFaderLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                                             int x, int y, int width, int height,
                                                             float sliderPosProportional,
                                                             float rotaryStartAngle,
                                                             float rotaryEndAngle,
                                                             juce::Slider& slider) {
    const auto bounds = juce::Rectangle<float>(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height)).reduced(2.0f);

    const float radius = std::min(bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;
    if (radius <= 4.0f) {
        return;
    }
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();

    const uint32_t accentArgb = static_cast<uint32_t>(
        static_cast<int64_t>(slider.getProperties().getWithDefault("mixAccent", static_cast<int64_t>(kColSoloTeal))));
    const bool isBipolar = static_cast<bool>(slider.getProperties().getWithDefault("mixBipolar", false));
    const bool dimmed    = static_cast<bool>(slider.getProperties().getWithDefault("mixDimmed", false));
    const juce::Colour accent = juce::Colour(accentArgb).withMultipliedAlpha(dimmed ? 0.35f : 1.0f);

    // Outer arc track
    const float arcR = radius - 1.5f;
    juce::Path bgArc;
    bgArc.addCentredArc(cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(juce::Colour(0xFF0A0D12u));
    g.strokePath(bgArc, juce::PathStrokeType(3.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0xFF28303Au));
    g.strokePath(bgArc, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const float midAngle = 0.5f * (rotaryStartAngle + rotaryEndAngle);
    const float startFillAngle = isBipolar ? midAngle : rotaryStartAngle;

    if (std::abs(angle - startFillAngle) > 0.01f) {
        juce::Path valArc;
        valArc.addCentredArc(cx, cy, arcR, arcR, 0.0f,
                             std::min(startFillAngle, angle),
                             std::max(startFillAngle, angle), true);
        g.setColour(accent);
        g.strokePath(valArc, juce::PathStrokeType(2.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (isBipolar) {
        const float tx = cx + (arcR + 2.5f) * std::sin(midAngle);
        const float ty = cy - (arcR + 2.5f) * std::cos(midAngle);
        g.setColour(juce::Colour(tokens::kSilkMutedSilver).withAlpha(0.7f));
        g.fillEllipse(tx - 1.2f, ty - 1.2f, 2.4f, 2.4f);
    }

    // Recessed knob skirt & hardware anodized cap
    const float knobR = radius * 0.72f;
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillEllipse(cx - knobR, cy - knobR + 1.5f, knobR * 2.0f, knobR * 2.0f);

    juce::ColourGradient capGrad(juce::Colour(0xFF3A414Bu), cx - knobR, cy - knobR,
                                 juce::Colour(0xFF171B21u), cx + knobR, cy + knobR, false);
    g.setGradientFill(capGrad);
    g.fillEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f);

    g.setColour(juce::Colour(0xFF4D5663u));
    g.drawEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f, 1.1f);

    // Crisp pointer notch
    const float pInner = knobR * 0.25f;
    const float pOuter = knobR * 0.88f;
    const float sinA = std::sin(angle);
    const float cosA = std::cos(angle);
    g.setColour(dimmed ? juce::Colour(tokens::kSilkDimLabel) : juce::Colour(tokens::kSilkWarmWhite));
    g.drawLine(cx + pInner * sinA, cy - pInner * cosA,
               cx + pOuter * sinA, cy - pOuter * cosA, 2.2f);
}

// =============================================================================
// MixerTactileButton
// =============================================================================
VF50MixerDrawer::MixerTactileButton::MixerTactileButton(const juce::String& name)
    : juce::Button(name) {
    setWantsKeyboardFocus(false);
}

void VF50MixerDrawer::MixerTactileButton::setVisualState(const juce::String& label,
                                                        uint32_t textArgb,
                                                        bool lit,
                                                        uint32_t litBorderArgb,
                                                        uint32_t litFillArgb,
                                                        bool showLedDot,
                                                        bool blinkDim) noexcept {
    label_         = label;
    textArgb_      = textArgb;
    lit_           = lit;
    litBorderArgb_ = litBorderArgb;
    litFillArgb_   = litFillArgb;
    showLedDot_    = showLedDot;
    blinkDim_      = blinkDim;
}

void VF50MixerDrawer::MixerTactileButton::paintButton(juce::Graphics& g, bool over, bool down) {
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    const auto cap = down ? b.translated(0.0f, 1.2f) : b;

    g.setColour(juce::Colour(tokens::kButtonBevelShadow));
    g.fillRoundedRectangle(b.translated(0.0f, 1.5f), 4.0f);

    juce::ColourGradient capGrad(juce::Colour(tokens::kSquareCapTop), cap.getX(), cap.getY(),
                                 juce::Colour(tokens::kSquareCapBottom), cap.getX(), cap.getBottom(), false);
    g.setGradientFill(capGrad);
    g.fillRoundedRectangle(cap, 4.0f);

    if (lit_) {
        const float alphaMul = blinkDim_ ? 0.55f : 1.0f;
        g.setColour(juce::Colour(litFillArgb_).withMultipliedAlpha(alphaMul));
        g.fillRoundedRectangle(cap, 4.0f);
        g.setColour(juce::Colour(litBorderArgb_).withMultipliedAlpha(alphaMul));
        g.drawRoundedRectangle(cap.reduced(0.6f), 4.0f, 1.8f);
    } else {
        if (over) {
            g.setColour(juce::Colours::white.withAlpha(0.05f));
            g.fillRoundedRectangle(cap, 4.0f);
        }
        g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
        g.drawRoundedRectangle(cap.reduced(0.5f), 4.0f, 1.2f);
    }

    auto textArea = cap;
    if (showLedDot_) {
        const float dotR = 3.6f;
        const float dotX = cap.getX() + 10.0f;
        const float dotY = cap.getCentreY();
        const juce::Colour dotCol = lit_
            ? juce::Colour(litBorderArgb_).withMultipliedAlpha(blinkDim_ ? 0.45f : 1.0f)
            : juce::Colour(0xFF283038u);
        g.setColour(dotCol);
        g.fillEllipse(dotX - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);
        textArea.removeFromLeft(12.0f);
    }

    const juce::Colour txtCol = lit_
        ? juce::Colour(textArgb_).withMultipliedAlpha(blinkDim_ ? 0.68f : 1.0f)
        : juce::Colour(textArgb_);
    g.setColour(txtCol);
    const float fontH = std::clamp(cap.getHeight() * 0.48f, 11.5f, 16.5f);
    g.setFont(VF50LookAndFeel::makeFont(fontH, true, 0.96f));
    g.drawFittedText(label_, textArea.toNearestInt(), juce::Justification::centred, 1);
}

// =============================================================================
// MixerDragPill
// =============================================================================
VF50MixerDrawer::MixerDragPill::MixerDragPill(int minVal, int maxVal, int defaultVal, uint32_t accentArgb)
    : minVal_(minVal),
      maxVal_(maxVal),
      defaultVal_(defaultVal),
      value_(defaultVal),
      accentArgb_(accentArgb) {
    setWantsKeyboardFocus(false);
    setRepaintsOnMouseActivity(true);
}

void VF50MixerDrawer::MixerDragPill::setRange(int minVal, int maxVal, int defaultVal) noexcept {
    minVal_ = minVal;
    maxVal_ = std::max(minVal, maxVal);
    defaultVal_ = std::clamp(defaultVal, minVal_, maxVal_);
    value_ = std::clamp(value_, minVal_, maxVal_);
}

void VF50MixerDrawer::MixerDragPill::setValue(int val, bool sendNotification) {
    const int clamped = std::clamp(val, minVal_, maxVal_);
    const bool changed = (clamped != value_);
    value_ = clamped;
    repaint();
    if (sendNotification && changed && onValueChanged) {
        onValueChanged(value_);
    }
}

void VF50MixerDrawer::MixerDragPill::setDisplayText(const juce::String& text, bool active) noexcept {
    displayText_ = text;
    active_ = active;
    repaint();
}

void VF50MixerDrawer::MixerDragPill::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat().reduced(0.8f);
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(r, 4.0f);

    const bool over = isMouseOver();
    const juce::Colour borderCol = active_
        ? juce::Colour(accentArgb_).withAlpha(over ? 0.90f : 0.45f)
        : juce::Colour(kColStripBorder);
    g.setColour(borderCol);
    g.drawRoundedRectangle(r, 4.0f, over ? 1.4f : 1.1f);

    g.setColour(juce::Colour(active_ ? tokens::kSilkWarmWhite : tokens::kSilkDimLabel));
    const float fontH = std::clamp(r.getHeight() * 0.48f, 11.0f, 15.0f);
    g.setFont(VF50LookAndFeel::makeFont(fontH, true, 0.95f));
    g.drawFittedText(displayText_, getLocalBounds().reduced(5, 1), juce::Justification::centred, 1);
}

void VF50MixerDrawer::MixerDragPill::mouseDown(const juce::MouseEvent& e) {
    dragStartVal_ = value_;
    dragged_ = false;
    if (e.mods.isRightButtonDown()) {
        const int next = (value_ > minVal_) ? (value_ - 1) : (cycleOnClick_ ? maxVal_ : minVal_);
        setValue(next, true);
    }
}

void VF50MixerDrawer::MixerDragPill::mouseDrag(const juce::MouseEvent& e) {
    if (e.mods.isRightButtonDown()) {
        return;
    }
    const int dy = e.getDistanceFromDragStartY();
    if (std::abs(dy) >= 3) {
        dragged_ = true;
        const int span = std::max(1, maxVal_ - minVal_);
        const int pxPerStep = (span > 24) ? 5 : 10;
        const int delta = -dy / pxPerStep;
        setValue(dragStartVal_ + delta, true);
    }
}

void VF50MixerDrawer::MixerDragPill::mouseUp(const juce::MouseEvent& e) {
    if (!dragged_ && !e.mods.isRightButtonDown() && e.mouseWasClicked()) {
        if (cycleOnClick_) {
            const int next = (value_ >= maxVal_) ? minVal_ : (value_ + 1);
            setValue(next, true);
        } else {
            const int midX = getWidth() / 2;
            const int delta = (e.x >= midX) ? 1 : -1;
            setValue(value_ + delta, true);
        }
    }
}

void VF50MixerDrawer::MixerDragPill::mouseDoubleClick(const juce::MouseEvent&) {
    setValue(defaultVal_, true);
}

void VF50MixerDrawer::MixerDragPill::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
    if (std::abs(wheel.deltaY) < 0.001f) {
        return;
    }
    const int step = (wheel.deltaY > 0.0f) ? 1 : -1;
    setValue(value_ + step, true);
}

// =============================================================================
// DrumSubMixCell
// =============================================================================
VF50MixerDrawer::DrumSubMixCell::DrumSubMixCell(VF50MixerDrawer& owner, std::size_t drumIndex)
    : owner_(owner),
      drumIndex_(drumIndex) {
    setWantsKeyboardFocus(false);
}

void VF50MixerDrawer::DrumSubMixCell::setDrumData(std::size_t drumIndex,
                                                 const juce::String& name,
                                                 int vol0To15,
                                                 int panNeg3ToPos3) {
    drumIndex_ = drumIndex;
    drumName_  = name;
    volume_    = std::clamp(vol0To15, 0, 15);
    pan_       = std::clamp(panNeg3ToPos3, -3, 3);
    repaint();
}

juce::Rectangle<float> VF50MixerDrawer::DrumSubMixCell::volBarBounds() const noexcept {
    const auto b = getLocalBounds().toFloat();
    return { 164.0f, 7.0f, std::max(60.0f, b.getWidth() * 0.33f), b.getHeight() - 14.0f };
}

juce::Rectangle<float> VF50MixerDrawer::DrumSubMixCell::panStripBounds() const noexcept {
    const auto vb = volBarBounds();
    const float x = vb.getRight() + 10.0f;
    return { x, 7.0f, std::max(70.0f, static_cast<float>(getWidth()) - x - 8.0f),
             static_cast<float>(getHeight()) - 14.0f };
}

void VF50MixerDrawer::DrumSubMixCell::applyVolFromX(float x) {
    const auto vb = volBarBounds();
    const float norm = std::clamp((x - vb.getX()) / std::max(1.0f, vb.getWidth()), 0.0f, 1.0f);
    const int newVol = std::clamp(static_cast<int>(std::lround(norm * 15.0f)), 0, 15);
    if (newVol != volume_) {
        volume_ = newVol;
        owner_.setDrumInstVolumeFromMixer(drumIndex_, volume_);
    }
}

void VF50MixerDrawer::DrumSubMixCell::applyPanFromX(float x) {
    const auto pb = panStripBounds();
    const float norm = std::clamp((x - pb.getX()) / std::max(1.0f, pb.getWidth()), 0.0f, 0.9999f);
    const int step = std::clamp(static_cast<int>(std::floor(norm * 7.0f)) - 3, -3, 3);
    if (step != pan_) {
        pan_ = step;
        owner_.setDrumInstPanFromMixer(drumIndex_, pan_);
    }
}

void VF50MixerDrawer::DrumSubMixCell::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat().reduced(0.8f);
    g.setColour(juce::Colour(kColStripFill));
    g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(kColStripBorder));
    g.drawRoundedRectangle(b, 4.0f, 1.0f);

    // Drum index + 6-char VF50 Name
    const juce::String label = juce::String::formatted("%02d:", static_cast<int>(drumIndex_) + 1) + drumName_;
    g.setColour(juce::Colour(volume_ > 0 ? tokens::kSilkWarmWhite : tokens::kSilkDimLabel));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true, 0.96f));
    g.drawFittedText(label, 8, 4, 152, getHeight() - 8, juce::Justification::centredLeft, 1);

    // Volume bar (0..15)
    const auto vb = volBarBounds();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(vb, 3.0f);
    g.setColour(juce::Colour(0xFF28303Au));
    g.drawRoundedRectangle(vb, 3.0f, 1.0f);

    const float fillW = (static_cast<float>(volume_) / 15.0f) * (vb.getWidth() - 4.0f);
    if (fillW > 1.0f) {
        g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.75f));
        g.fillRoundedRectangle(vb.getX() + 2.0f, vb.getY() + 2.0f, fillW, vb.getHeight() - 4.0f, 2.0f);
    }
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(12.5f, true, 0.98f));
    g.drawText(juce::String::formatted("V:%02d", volume_), vb.toNearestInt(), juce::Justification::centred, false);

    // 7-step Pan strip (L3..C..R3)
    const auto pb = panStripBounds();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(pb, 3.0f);
    g.setColour(juce::Colour(0xFF28303Au));
    g.drawRoundedRectangle(pb, 3.0f, 1.0f);

    const float segW = pb.getWidth() / 7.0f;
    for (int s = 0; s < 7; ++s) {
        const int panStep = s - 3;
        const bool isSel = (panStep == pan_);
        const juce::Rectangle<float> sr(pb.getX() + static_cast<float>(s) * segW + 1.2f,
                                        pb.getY() + 2.0f, segW - 2.4f, pb.getHeight() - 4.0f);
        if (isSel) {
            g.setColour(juce::Colour(panStep == 0 ? kColSoloTeal : kColSoloAmber));
            g.fillRoundedRectangle(sr, 2.0f);
        }
        g.setColour(juce::Colour(isSel ? 0xFF0A0C0Fu : tokens::kSilkMutedSilver));
        g.setFont(VF50LookAndFeel::makeFont(10.5f, true, 0.92f));
        g.drawText(formatPanStepLabel(panStep), sr.toNearestInt(), juce::Justification::centred, false);
    }
}

void VF50MixerDrawer::DrumSubMixCell::mouseDown(const juce::MouseEvent& e) {
    const auto fx = static_cast<float>(e.x);
    if (volBarBounds().expanded(2.0f, 4.0f).contains(fx, static_cast<float>(e.y))) {
        dragTarget_ = 1;
        applyVolFromX(fx);
        return;
    }
    if (panStripBounds().expanded(2.0f, 4.0f).contains(fx, static_cast<float>(e.y))) {
        dragTarget_ = 2;
        applyPanFromX(fx);
        return;
    }
    dragTarget_ = 0;
    // Audition click on drum label when rhythm channel is valid
    const auto& proc = owner_.processor_;
    const uint8_t rch = proc.synthEngine().drums().rhythmReceiveChannel();
    if (rch < 16u) {
        const uint8_t note = proc.hostMemory().activeRhythmKit().instruments[drumIndex_].midiNote;
        owner_.processor_.enqueueUiMidi3(static_cast<uint8_t>(0x90u | (rch & 0x0Fu)), note, 110u);
    }
}

void VF50MixerDrawer::DrumSubMixCell::mouseDrag(const juce::MouseEvent& e) {
    if (dragTarget_ == 1) {
        applyVolFromX(static_cast<float>(e.x));
    } else if (dragTarget_ == 2) {
        applyPanFromX(static_cast<float>(e.x));
    }
}

void VF50MixerDrawer::DrumSubMixCell::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    if (std::abs(wheel.deltaY) < 0.001f) {
        return;
    }
    const int step = (wheel.deltaY > 0.0f) ? 1 : -1;
    if (panStripBounds().expanded(2.0f, 4.0f).contains(static_cast<float>(e.x), static_cast<float>(e.y))) {
        owner_.setDrumInstPanFromMixer(drumIndex_, pan_ + step);
    } else {
        owner_.setDrumInstVolumeFromMixer(drumIndex_, volume_ + step);
    }
}

// =============================================================================
// VF50MixerDrawer
// =============================================================================
VF50MixerDrawer::VF50MixerDrawer(vf50::plugin::VF50AudioProcessor& processor,
                               vf50::classic::VF50ClassicController& classicController)
    : processor_(processor),
      classicController_(classicController) {
    setOpaque(true);
    setWantsKeyboardFocus(false);
    buildControls();
    syncFromProcessor();
}

VF50MixerDrawer::~VF50MixerDrawer() {
    masterStrip_.masterAttach.reset();
    for (auto& st : instStrips_) {
        if (st.balKnob) st.balKnob->setLookAndFeel(nullptr);
        if (st.fxSendKnob) st.fxSendKnob->setLookAndFeel(nullptr);
        if (st.eqLoKnob) st.eqLoKnob->setLookAndFeel(nullptr);
        if (st.eqHiKnob) st.eqHiKnob->setLookAndFeel(nullptr);
        if (st.fader) st.fader->setLookAndFeel(nullptr);
    }
    if (rhyStrip_.balKnob) rhyStrip_.balKnob->setLookAndFeel(nullptr);
    if (rhyStrip_.fxSendKnob) rhyStrip_.fxSendKnob->setLookAndFeel(nullptr);
    if (rhyStrip_.eqLoKnob) rhyStrip_.eqLoKnob->setLookAndFeel(nullptr);
    if (rhyStrip_.eqHiKnob) rhyStrip_.eqHiKnob->setLookAndFeel(nullptr);
    if (rhyStrip_.fader) rhyStrip_.fader->setLookAndFeel(nullptr);
    if (masterStrip_.fxBalKnob) masterStrip_.fxBalKnob->setLookAndFeel(nullptr);
    if (masterStrip_.masterFader) masterStrip_.masterFader->setLookAndFeel(nullptr);
}

std::unique_ptr<juce::Slider> VF50MixerDrawer::makeRotaryKnob(const juce::String& name,
                                                             double minV,
                                                             double maxV,
                                                             double stepV,
                                                             double defaultV,
                                                             uint32_t accentArgb,
                                                             bool bipolar) {
    auto k = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
    k->setName(name);
    k->setLookAndFeel(&mixerLookAndFeel_);
    k->setRange(minV, maxV, stepV);
    k->setValue(defaultV, juce::dontSendNotification);
    k->setDoubleClickReturnValue(true, defaultV);
    k->setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                           juce::MathConstants<float>::pi * 2.75f, true);
    k->getProperties().set("mixAccent", static_cast<int64_t>(accentArgb));
    k->getProperties().set("mixBipolar", bipolar);
    k->getProperties().set("mixDimmed", false);
    addAndMakeVisible(*k);
    return k;
}

void VF50MixerDrawer::buildControls() {
    btnClearSolo_ = std::make_unique<MixerTactileButton>("MIXER_CLEAR_SOLO");
    btnClearSolo_->setVisualState("CLEAR SOLO", tokens::kSilkMutedSilver, false, kColSoloAmber, 0x38F2B632u, true);
    btnClearSolo_->onClick = [this]() { clearAllSoloFromMixer(); };
    addAndMakeVisible(*btnClearSolo_);

    btnResetMix_ = std::make_unique<MixerTactileButton>("MIXER_RESET_MIX");
    btnResetMix_->setVisualState("RESET MIX", tokens::kSilkWarmWhite, false, kColSoloTeal, 0x3300B8A9u, false);
    btnResetMix_->onClick = [this]() { resetMixFromMixer(); };
    addAndMakeVisible(*btnResetMix_);

    // Strips 1..8: Performance Instruments
    for (std::size_t i = 0u; i < 8u; ++i) {
        auto& st = instStrips_[i];

        st.pillNotes = std::make_unique<MixerDragPill>(0, 16, 16, kColSoloTeal);
        st.pillNotes->onValueChanged = [this, i](int v) {
            if (!suppressCallbacks_) (void)setInstNotesFromMixer(i, v);
        };
        addAndMakeVisible(*st.pillNotes);

        st.pillShift = std::make_unique<MixerDragPill>(-24, 24, 0, kColSoloTeal);
        st.pillShift->onValueChanged = [this, i](int v) {
            if (!suppressCallbacks_) (void)setInstNoteShiftFromMixer(i, v);
        };
        addAndMakeVisible(*st.pillShift);

        st.pillDetune = std::make_unique<MixerDragPill>(-7, 7, 0, kColSoloTeal);
        st.pillDetune->onValueChanged = [this, i](int v) {
            if (!suppressCallbacks_) (void)setInstDetuneFromMixer(i, v);
        };
        addAndMakeVisible(*st.pillDetune);

        st.btnOutAssign = std::make_unique<MixerTactileButton>("MIX_OUT_" + juce::String(i + 1u));
        st.btnOutAssign->onClick = [this, i]() { (void)cycleInstOutAssignFromMixer(i); };
        addAndMakeVisible(*st.btnOutAssign);

        st.balKnob = makeRotaryKnob("MIX_BAL_" + juce::String(i + 1u), -64.0, 63.0, 1.0, 0.0, kColSoloTeal, true);
        st.balKnob->onValueChange = [this, i]() {
            if (!suppressCallbacks_) {
                const float raw = static_cast<float>(instStrips_[i].balKnob->getValue());
                const float norm = (raw >= 0.0f) ? (raw / 63.0f) : (raw / 64.0f);
                setChannelBalanceFromMixer(i, norm);
            }
        };

        st.fxSendKnob = makeRotaryKnob("MIX_FXSEND_" + juce::String(i + 1u), 0.0, 99.0, 1.0, 99.0, kColSoloTeal, false);
        st.fxSendKnob->onValueChange = [this, i]() {
            if (!suppressCallbacks_) {
                setChannelFxSendFromMixer(i, static_cast<int>(std::lround(instStrips_[i].fxSendKnob->getValue())));
            }
        };

        st.btnEqToggle = std::make_unique<MixerTactileButton>("MIX_EQ_" + juce::String(i + 1u));
        st.btnEqToggle->setVisualState("EQ", tokens::kSilkMutedSilver, false, kColSegGreenOn, 0x382EE85Cu, false);
        st.btnEqToggle->onClick = [this, i]() {
            const bool next = !processor_.mixerChannelEqEnabled(i);
            setChannelEqFromMixer(i, next, processor_.mixerChannelEqLowDb(i), processor_.mixerChannelEqHighDb(i));
        };
        addAndMakeVisible(*st.btnEqToggle);

        st.eqLoKnob = makeRotaryKnob("MIX_EQLO_" + juce::String(i + 1u), -12.0, 12.0, 0.5, 0.0, kColSoloAmber, true);
        st.eqLoKnob->onValueChange = [this, i]() {
            if (!suppressCallbacks_) {
                const float lo = static_cast<float>(instStrips_[i].eqLoKnob->getValue());
                const bool en = processor_.mixerChannelEqEnabled(i) || std::abs(lo) > 0.05f;
                setChannelEqFromMixer(i, en, lo, processor_.mixerChannelEqHighDb(i));
            }
        };

        st.eqHiKnob = makeRotaryKnob("MIX_EQHI_" + juce::String(i + 1u), -12.0, 12.0, 0.5, 0.0, kColSoloAmber, true);
        st.eqHiKnob->onValueChange = [this, i]() {
            if (!suppressCallbacks_) {
                const float hi = static_cast<float>(instStrips_[i].eqHiKnob->getValue());
                const bool en = processor_.mixerChannelEqEnabled(i) || std::abs(hi) > 0.05f;
                setChannelEqFromMixer(i, en, processor_.mixerChannelEqLowDb(i), hi);
            }
        };

        st.btnMute = std::make_unique<MixerTactileButton>("MIX_MUTE_" + juce::String(i + 1u));
        st.btnMute->setVisualState("M", tokens::kSilkWarmWhite, false, kColMuteRed, 0x44FF5238u, false);
        st.btnMute->onClick = [this, i]() { toggleInstMuteFromMixer(i); };
        addAndMakeVisible(*st.btnMute);

        st.btnSolo = std::make_unique<MixerTactileButton>("MIX_SOLO_" + juce::String(i + 1u));
        st.btnSolo->setVisualState("S", tokens::kSilkWarmWhite, false, kColSoloTeal, 0x4400B8A9u, false);
        st.btnSolo->onClick = [this, i]() { toggleInstSoloFromMixer(i); };
        addAndMakeVisible(*st.btnSolo);

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        st.btnBusRoute = std::make_unique<MixerTactileButton>("MIX_BUS_" + juce::String(i + 1u));
        st.btnBusRoute->setTooltip("AUX = isolate to Inst " + juce::String(i + 1u)
                                   + " Out when enabled (wet FX stays on Main); MAIN = sum to Main + Aux");
        st.btnBusRoute->onClick = [this, i]() { toggleChannelBusRouteFromMixer(i); };
        addAndMakeVisible(*st.btnBusRoute);
#endif

        st.fader = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
        st.fader->setLookAndFeel(&mixerLookAndFeel_);
        st.fader->setRange(0.0, 99.0, 1.0);
        st.fader->setValue(99.0, juce::dontSendNotification);
        st.fader->setDoubleClickReturnValue(true, 99.0);
        st.fader->getProperties().set("mixAccent", static_cast<int64_t>(kColSoloTeal));
        st.fader->onValueChange = [this, i]() {
            if (!suppressCallbacks_) {
                (void)setInstVolumeFromMixer(i, static_cast<int>(std::lround(instStrips_[i].fader->getValue())));
            }
        };
        addAndMakeVisible(*st.fader);
    }

    // Strip 9: PCM Rhythm Bus
    rhyStrip_.btnDrumSubMix = std::make_unique<MixerTactileButton>("MIX_RHY_SUBMIX");
    rhyStrip_.btnDrumSubMix->setVisualState("DRUM KIT MIX", tokens::kSilkWarmWhite, false, kColSoloTeal, 0x3800B8A9u, false);
    rhyStrip_.btnDrumSubMix->onClick = [this]() { setDrumSubMixVisible(!drumSubMixVisible_); };
    addAndMakeVisible(*rhyStrip_.btnDrumSubMix);

    rhyStrip_.btnEffectMode = std::make_unique<MixerTactileButton>("MIX_RHY_EFCT");
    rhyStrip_.btnEffectMode->onClick = [this]() { cycleRhythmEffectModeFromMixer(); };
    addAndMakeVisible(*rhyStrip_.btnEffectMode);

    rhyStrip_.balKnob = makeRotaryKnob("MIX_RHY_BAL", -64.0, 63.0, 1.0, 0.0, kColSoloTeal, true);
    rhyStrip_.balKnob->onValueChange = [this]() {
        if (!suppressCallbacks_) {
            const float raw = static_cast<float>(rhyStrip_.balKnob->getValue());
            const float norm = (raw >= 0.0f) ? (raw / 63.0f) : (raw / 64.0f);
            setChannelBalanceFromMixer(8u, norm);
        }
    };

    rhyStrip_.fxSendKnob = makeRotaryKnob("MIX_RHY_FXSEND", 0.0, 99.0, 1.0, 50.0, kColSoloTeal, false);
    rhyStrip_.fxSendKnob->onValueChange = [this]() {
        if (!suppressCallbacks_) {
            setChannelFxSendFromMixer(8u, static_cast<int>(std::lround(rhyStrip_.fxSendKnob->getValue())));
        }
    };

    rhyStrip_.btnEqToggle = std::make_unique<MixerTactileButton>("MIX_RHY_EQ");
    rhyStrip_.btnEqToggle->setVisualState("EQ", tokens::kSilkMutedSilver, false, kColSegGreenOn, 0x382EE85Cu, false);
    rhyStrip_.btnEqToggle->onClick = [this]() {
        const bool next = !processor_.mixerChannelEqEnabled(8u);
        setChannelEqFromMixer(8u, next, processor_.mixerChannelEqLowDb(8u), processor_.mixerChannelEqHighDb(8u));
    };
    addAndMakeVisible(*rhyStrip_.btnEqToggle);

    rhyStrip_.eqLoKnob = makeRotaryKnob("MIX_RHY_EQLO", -12.0, 12.0, 0.5, 0.0, kColSoloAmber, true);
    rhyStrip_.eqLoKnob->onValueChange = [this]() {
        if (!suppressCallbacks_) {
            const float lo = static_cast<float>(rhyStrip_.eqLoKnob->getValue());
            const bool en = processor_.mixerChannelEqEnabled(8u) || std::abs(lo) > 0.05f;
            setChannelEqFromMixer(8u, en, lo, processor_.mixerChannelEqHighDb(8u));
        }
    };

    rhyStrip_.eqHiKnob = makeRotaryKnob("MIX_RHY_EQHI", -12.0, 12.0, 0.5, 0.0, kColSoloAmber, true);
    rhyStrip_.eqHiKnob->onValueChange = [this]() {
        if (!suppressCallbacks_) {
            const float hi = static_cast<float>(rhyStrip_.eqHiKnob->getValue());
            const bool en = processor_.mixerChannelEqEnabled(8u) || std::abs(hi) > 0.05f;
            setChannelEqFromMixer(8u, en, processor_.mixerChannelEqLowDb(8u), hi);
        }
    };

    rhyStrip_.btnMute = std::make_unique<MixerTactileButton>("MIX_RHY_MUTE");
    rhyStrip_.btnMute->setVisualState("M", tokens::kSilkWarmWhite, false, kColMuteRed, 0x44FF5238u, false);
    rhyStrip_.btnMute->onClick = [this]() { toggleRhythmMuteFromMixer(); };
    addAndMakeVisible(*rhyStrip_.btnMute);

    rhyStrip_.btnSolo = std::make_unique<MixerTactileButton>("MIX_RHY_SOLO");
    rhyStrip_.btnSolo->setVisualState("S", tokens::kSilkWarmWhite, false, kColSoloTeal, 0x4400B8A9u, false);
    rhyStrip_.btnSolo->onClick = [this]() { toggleRhythmSoloFromMixer(); };
    addAndMakeVisible(*rhyStrip_.btnSolo);

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    rhyStrip_.btnBusRoute = std::make_unique<MixerTactileButton>("MIX_RHY_BUS");
    rhyStrip_.btnBusRoute->setTooltip("AUX = isolate to Rhythm Out when enabled (wet FX stays on Main); MAIN = sum to Main + Aux");
    rhyStrip_.btnBusRoute->onClick = [this]() { toggleChannelBusRouteFromMixer(8u); };
    addAndMakeVisible(*rhyStrip_.btnBusRoute);
#endif

    rhyStrip_.fader = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    rhyStrip_.fader->setLookAndFeel(&mixerLookAndFeel_);
    rhyStrip_.fader->setRange(0.0, 99.0, 1.0);
    rhyStrip_.fader->setValue(85.0, juce::dontSendNotification);
    rhyStrip_.fader->setDoubleClickReturnValue(true, 85.0);
    rhyStrip_.fader->getProperties().set("mixAccent", static_cast<int64_t>(kColSoloTeal));
    rhyStrip_.fader->onValueChange = [this]() {
        if (!suppressCallbacks_) {
            setRhythmVolumeFromMixer(static_cast<int>(std::lround(rhyStrip_.fader->getValue())));
        }
    };
    addAndMakeVisible(*rhyStrip_.fader);

    // Strip 10: Master / FX
    masterStrip_.pillFxType = std::make_unique<MixerDragPill>(0, 32, 0, kColSoloTeal);
    masterStrip_.pillFxType->setCycleOnClick(true);
    masterStrip_.pillFxType->onValueChanged = [this](int fxIdx) {
        if (!suppressCallbacks_) (void)setFxTypeFromMixer(fxIdx);
    };
    addAndMakeVisible(*masterStrip_.pillFxType);

    masterStrip_.fxBalKnob = makeRotaryKnob("MIX_FX_BAL", 0.0, 99.0, 1.0, 50.0, kColSoloTeal, false);
    masterStrip_.fxBalKnob->onValueChange = [this]() {
        if (!suppressCallbacks_) {
            (void)setFxBalanceFromMixer(static_cast<int>(std::lround(masterStrip_.fxBalKnob->getValue())));
        }
    };

    masterStrip_.btnFxBypass = std::make_unique<MixerTactileButton>("MIX_FX_BYPASS");
    masterStrip_.btnFxBypass->setVisualState("FX BYP", tokens::kSilkWarmWhite, false, kColMuteRed, 0x44FF5238u, true);
    masterStrip_.btnFxBypass->onClick = [this]() { toggleFxBypassFromMixer(); };
    addAndMakeVisible(*masterStrip_.btnFxBypass);

    masterStrip_.btnLimiter = std::make_unique<MixerTactileButton>("MIX_LIMITER");
    masterStrip_.btnLimiter->setVisualState("LIMIT -0.3dB", tokens::kSilkWarmWhite, true, kColSoloAmber, 0x38F2B632u, true);
    masterStrip_.btnLimiter->onClick = [this]() { toggleSafetyLimiterFromMixer(); };
    addAndMakeVisible(*masterStrip_.btnLimiter);

    masterStrip_.masterFader = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    masterStrip_.masterFader->setLookAndFeel(&mixerLookAndFeel_);
    masterStrip_.masterFader->setDoubleClickReturnValue(true, 0.80);
    masterStrip_.masterFader->getProperties().set("mixAccent", static_cast<int64_t>(tokens::kSilkWarmWhite));
    addAndMakeVisible(*masterStrip_.masterFader);
    masterStrip_.masterAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor_.apvts(), "masterVolume", *masterStrip_.masterFader);

    // Embedded 61-Drum Sub-Mix View
    btnDrumFilterUsed_ = std::make_unique<MixerTactileButton>("MIX_DRUM_USED");
    btnDrumFilterUsed_->onClick = [this]() { setDrumFilterMode(DrumFilterMode::UsedInSong); };
    addChildComponent(*btnDrumFilterUsed_);

    btnDrumFilterAll_ = std::make_unique<MixerTactileButton>("MIX_DRUM_ALL");
    btnDrumFilterAll_->onClick = [this]() { setDrumFilterMode(DrumFilterMode::All61); };
    addChildComponent(*btnDrumFilterAll_);

    btnDrumFilterKsh_ = std::make_unique<MixerTactileButton>("MIX_DRUM_KSH");
    btnDrumFilterKsh_->onClick = [this]() { setDrumFilterMode(DrumFilterMode::KickSnareHat); };
    addChildComponent(*btnDrumFilterKsh_);

    btnDrumBackToMixer_ = std::make_unique<MixerTactileButton>("MIX_DRUM_BACK");
    btnDrumBackToMixer_->setVisualState("BACK TO MIXER", tokens::kSilkWarmWhite, true, kColSoloTeal, 0x3800B8A9u, false);
    btnDrumBackToMixer_->onClick = [this]() { setDrumSubMixVisible(false); };
    addChildComponent(*btnDrumBackToMixer_);

    for (std::size_t d = 0u; d < drumCells_.size(); ++d) {
        drumCells_[d] = std::make_unique<DrumSubMixCell>(*this, d);
        addChildComponent(*drumCells_[d]);
    }

    updateChannelStripVisibility();
}

void VF50MixerDrawer::updateChannelStripVisibility() {
    const bool showStrips = !drumSubMixVisible_;

    for (auto& st : instStrips_) {
        if (st.pillNotes) st.pillNotes->setVisible(showStrips);
        if (st.pillShift) st.pillShift->setVisible(showStrips);
        if (st.pillDetune) st.pillDetune->setVisible(showStrips);
        if (st.btnOutAssign) st.btnOutAssign->setVisible(showStrips);
        if (st.balKnob) st.balKnob->setVisible(showStrips);
        if (st.fxSendKnob) st.fxSendKnob->setVisible(showStrips);
        if (st.btnEqToggle) st.btnEqToggle->setVisible(showStrips);
        if (st.eqLoKnob) st.eqLoKnob->setVisible(showStrips);
        if (st.eqHiKnob) st.eqHiKnob->setVisible(showStrips);
        if (st.btnMute) st.btnMute->setVisible(showStrips);
        if (st.btnSolo) st.btnSolo->setVisible(showStrips);
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        if (st.btnBusRoute) st.btnBusRoute->setVisible(showStrips);
#endif
        if (st.fader) st.fader->setVisible(showStrips);
    }

    if (rhyStrip_.btnDrumSubMix) rhyStrip_.btnDrumSubMix->setVisible(showStrips);
    if (rhyStrip_.btnEffectMode) rhyStrip_.btnEffectMode->setVisible(showStrips);
    if (rhyStrip_.balKnob) rhyStrip_.balKnob->setVisible(showStrips);
    if (rhyStrip_.fxSendKnob) rhyStrip_.fxSendKnob->setVisible(showStrips);
    if (rhyStrip_.btnEqToggle) rhyStrip_.btnEqToggle->setVisible(showStrips);
    if (rhyStrip_.eqLoKnob) rhyStrip_.eqLoKnob->setVisible(showStrips);
    if (rhyStrip_.eqHiKnob) rhyStrip_.eqHiKnob->setVisible(showStrips);
    if (rhyStrip_.btnMute) rhyStrip_.btnMute->setVisible(showStrips);
    if (rhyStrip_.btnSolo) rhyStrip_.btnSolo->setVisible(showStrips);
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    if (rhyStrip_.btnBusRoute) rhyStrip_.btnBusRoute->setVisible(showStrips);
#endif
    if (rhyStrip_.fader) rhyStrip_.fader->setVisible(showStrips);

    if (masterStrip_.pillFxType) masterStrip_.pillFxType->setVisible(showStrips);
    if (masterStrip_.fxBalKnob) masterStrip_.fxBalKnob->setVisible(showStrips);
    if (masterStrip_.btnFxBypass) masterStrip_.btnFxBypass->setVisible(showStrips);
    if (masterStrip_.btnLimiter) masterStrip_.btnLimiter->setVisible(showStrips);
    if (masterStrip_.masterFader) masterStrip_.masterFader->setVisible(showStrips);

    if (btnDrumFilterUsed_) btnDrumFilterUsed_->setVisible(drumSubMixVisible_);
    if (btnDrumFilterAll_) btnDrumFilterAll_->setVisible(drumSubMixVisible_);
    if (btnDrumFilterKsh_) btnDrumFilterKsh_->setVisible(drumSubMixVisible_);
    if (btnDrumBackToMixer_) btnDrumBackToMixer_->setVisible(drumSubMixVisible_);

    if (drumSubMixVisible_) {
        rebuildDrumSubMixGrid();
    } else {
        for (auto& cell : drumCells_) {
            if (cell) cell->setVisible(false);
        }
    }
}

void VF50MixerDrawer::rebuildDrumSubMixGrid() {
    visibleDrumIndices_.clear();
    const auto& rom = processor_.activeRomBank();
    const auto& mem = processor_.hostMemory();
    const auto& kit = mem.activeRhythmKit();

    if (drumFilterMode_ == DrumFilterMode::All61) {
        for (std::size_t d = 0u; d < vf50::generated::kDrumInstrumentCount; ++d) {
            visibleDrumIndices_.push_back(d);
        }
    } else if (drumFilterMode_ == DrumFilterMode::KickSnareHat) {
        for (std::size_t d = 0u; d < vf50::generated::kDrumInstrumentCount; ++d) {
            const auto sv = rom.defaultDrumSetup[d].name;
            const juce::String nm(sv.data(), sv.size());
            if (isKickSnareHatName(nm)) {
                visibleDrumIndices_.push_back(d);
            }
        }
        if (visibleDrumIndices_.empty()) {
            for (std::size_t d = 0u; d < 16u; ++d) {
                visibleDrumIndices_.push_back(d);
            }
        }
    } else {
        // UsedInSong: collect drums referenced by active rhythm pattern, active rhythm song, and active sequencer song
        std::array<bool, vf50::generated::kDrumInstrumentCount> used{};
        auto markPtn = [&](bool isUser, std::size_t ptnIdx) {
            if (isUser) {
                const auto& uPtn = mem.userRhythmPattern(ptnIdx % vf50::state::VF50MemoryManager::kRhythmPatternCount);
                for (uint16_t e = 0u; e < uPtn.eventCount; ++e) {
                    const std::size_t di = uPtn.events[e].drumIndex;
                    if (di < used.size()) used[di] = true;
                }
            } else {
                const auto span = rom.getRhythmPatternEvents(ptnIdx % vf50::generated::kFactoryRhythmPtnCount);
                for (const auto& ev : span) {
                    if (ev.drumIndex < used.size()) used[ev.drumIndex] = true;
                }
            }
        };

        markPtn(mem.selectedRhythmPatternBank() == vf50::state::MemoryBank::UserRam,
                mem.selectedRhythmPatternIndex());

        const int rhySongSlot = std::clamp(classicController_.selectedRhythmSongSlot(), 0, 7);
        const auto& rSong = processor_.hostRhythmSongs()[static_cast<std::size_t>(rhySongSlot)];
        for (uint16_t p = 0u; p < rSong.partCount; ++p) {
            const auto& part = rSong.parts[p];
            if (part.type == vf50::generated::RhythmSongPartType::Pattern) {
                markPtn(part.ptnIsUser, part.ptnIndex);
            }
        }

        const int activeDemoIdx = processor_.activeDemoSongIndex();
        if (activeDemoIdx >= 0 && static_cast<std::size_t>(activeDemoIdx) < vf50::dsp::VF50DemoSongPlayer::kUserSongCount) {
            if (const auto* seq = processor_.userSequenceInSlot(static_cast<std::size_t>(activeDemoIdx))) {
                for (uint32_t e = 0u; e < seq->eventCount; ++e) {
                    const auto& ev = seq->events[e];
                    if (ev.type == vf50::generated::DemoEventType::DrumHit && ev.data1 < used.size()) {
                        used[ev.data1] = true;
                    }
                }
            }
        } else if (activeDemoIdx >= static_cast<int>(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase)) {
            const std::size_t fIdx = static_cast<std::size_t>(activeDemoIdx) - vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase;
            if (fIdx < vf50::generated::kDemoSongCount) {
                const auto span = rom.getDemoSongEvents(fIdx);
                for (const auto& ev : span) {
                    if (ev.type == vf50::generated::DemoEventType::DrumHit && ev.data1 < used.size()) {
                        used[ev.data1] = true;
                    }
                }
            }
        }

        for (std::size_t d = 0u; d < used.size(); ++d) {
            if (used[d]) {
                visibleDrumIndices_.push_back(d);
            }
        }
        if (visibleDrumIndices_.empty()) {
            for (std::size_t d = 0u; d < 15u; ++d) {
                visibleDrumIndices_.push_back(d);
            }
        }
    }

    if (btnDrumFilterUsed_) {
        const bool sel = (drumFilterMode_ == DrumFilterMode::UsedInSong);
        btnDrumFilterUsed_->setVisualState("USED IN SONG",
                                           sel ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                           sel, kColSoloTeal, 0x4000B8A9u, false);
        btnDrumFilterUsed_->repaint();
    }
    if (btnDrumFilterAll_) {
        const bool sel = (drumFilterMode_ == DrumFilterMode::All61);
        btnDrumFilterAll_->setVisualState("ALL 61",
                                          sel ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                          sel, kColSoloTeal, 0x4000B8A9u, false);
        btnDrumFilterAll_->repaint();
    }
    if (btnDrumFilterKsh_) {
        const bool sel = (drumFilterMode_ == DrumFilterMode::KickSnareHat);
        btnDrumFilterKsh_->setVisualState("KICK-SNARE-HAT",
                                          sel ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                          sel, kColSoloTeal, 0x4000B8A9u, false);
        btnDrumFilterKsh_->repaint();
    }

    const int gridX = drumSubMixRect_.getX();
    const int gridY = drumSubMixHeaderRect_.getBottom() + 10;
    const int gridW = std::max(500, drumSubMixRect_.getWidth());
    const int gridH = std::max(200, drumSubMixRect_.getBottom() - gridY);

    constexpr int kCols = 5;
    constexpr int kRows = 13;
    constexpr int kGapX = 10;
    constexpr int kGapY = 4;
    const int cellW = (gridW - (kCols - 1) * kGapX) / kCols;
    const int cellH = std::min(46, (gridH - (kRows - 1) * kGapY) / kRows);

    for (std::size_t idx = 0u; idx < drumCells_.size(); ++idx) {
        auto& cell = drumCells_[idx];
        if (!cell) continue;
        if (!drumSubMixVisible_ || idx >= visibleDrumIndices_.size()) {
            cell->setVisible(false);
            continue;
        }
        const std::size_t drumIdx = visibleDrumIndices_[idx];
        const auto sv = rom.defaultDrumSetup[drumIdx].name;
        const juce::String dName = juce::String(sv.data(), sv.size()).trimEnd();
        const auto& st = kit.instruments[drumIdx];
        cell->setDrumData(drumIdx, dName.isEmpty() ? juce::String::formatted("DRM%02d", static_cast<int>(drumIdx) + 1) : dName,
                          static_cast<int>(st.volume), static_cast<int>(st.pan));
        const int col = static_cast<int>(idx) / kRows;
        const int row = static_cast<int>(idx) % kRows;
        cell->setBounds(gridX + col * (cellW + kGapX), gridY + row * (cellH + kGapY), cellW, cellH);
        cell->setVisible(true);
    }
}

void VF50MixerDrawer::notifyParentStateChanged() {
    if (onMixerStateChanged) {
        onMixerStateChanged();
    }
}

void VF50MixerDrawer::updateMeterBallistics() noexcept {
    auto updateCh = [](float rawLinear, MeterChannelState& st) noexcept {
        if (rawLinear >= 0.999f) {
            st.clipped = true;
            st.clipTick = 28;
        } else if (st.clipTick > 0) {
            --st.clipTick;
            if (st.clipTick == 0) {
                st.clipped = false;
            }
        }
        const float target = meterNormFromLinear(rawLinear);
        st.val = (target >= st.val) ? target : (st.val * 0.82f);
        if (st.val < 0.004f) {
            st.val = 0.0f;
        }
        if (st.val >= st.hold) {
            st.hold = st.val;
            st.holdTick = 12;
        } else if (st.holdTick > 0) {
            --st.holdTick;
        } else {
            st.hold *= 0.93f;
            if (st.hold < 0.004f) {
                st.hold = 0.0f;
            }
        }
    };

    for (std::size_t i = 0u; i < 8u; ++i) {
        updateCh(processor_.instPeakL(i), instStrips_[i].meterL);
        updateCh(processor_.instPeakR(i), instStrips_[i].meterR);
    }
    updateCh(processor_.rhyPeakL(), rhyStrip_.meterL);
    updateCh(processor_.rhyPeakR(), rhyStrip_.meterR);
    updateCh(processor_.masterPeakL(), masterStrip_.meterL);
    updateCh(processor_.masterPeakR(), masterStrip_.meterR);
}

void VF50MixerDrawer::syncFromProcessor() {
    processor_.pullLatestAudioMemoryToHost();

    if (++blinkCounter_ >= 8u) {
        blinkCounter_ = 0u;
        blinkPhase_   = !blinkPhase_;
    }

    suppressCallbacks_ = true;

    const auto& mem = processor_.hostMemory();
    const auto& perf = mem.activePerformance();
    const uint16_t soloMask = processor_.mixerSoloMask();
    const bool seqMuteCtx = classicController_.isSequencerMuteContext();

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    auto syncBusButton = [this](MixerTactileButton* btn, std::size_t ch) {
        if (btn == nullptr) {
            return;
        }
        const bool auxExcl = processor_.mixerChannelAuxExclusive(ch);
        const bool hostOn = processor_.isHostAuxBusEnabled(ch);
        btn->setVisualState(auxExcl ? "BUS:AUX" : "BUS:MAIN",
                            auxExcl ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                            auxExcl,
                            hostOn ? kColSoloTeal : kColSoloAmber,
                            hostOn ? 0x4200B8A9u : 0x2EE8A020u,
                            hostOn);
        btn->repaint();
    };
#endif

    totalFmVoices_ = processor_.totalActiveFmVoices();
    totalRhyVoices_ = processor_.totalActiveRhyVoices();

    if (btnClearSolo_) {
        const bool anySolo = (soloMask != 0u);
        btnClearSolo_->setVisualState("CLEAR SOLO",
                                      anySolo ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                      anySolo,
                                      kColSoloAmber,
                                      0x40F2B632u,
                                      true);
        btnClearSolo_->repaint();
    }

    for (std::size_t i = 0u; i < 8u; ++i) {
        auto& st = instStrips_[i];
        const auto& inst = perf.instruments[i];
        const auto vBank = (inst.voiceBank == 0u)
                               ? vf50::state::MemoryBank::UserRam
                               : vf50::state::MemoryBank::FactoryRom;
        const auto& vPatch = mem.getVoice(vBank, inst.voiceNumber);

        const char bankChar = (inst.voiceBank == 0u) ? 'I' : 'P';
        st.slotChBadge = juce::String::formatted("%c%02d | ", bankChar, static_cast<int>(inst.voiceNumber))
                       + formatRecvChannelBadge(inst.receiveChannel);

        juce::String vName(vPatch.name.data(), vPatch.name.size());
        vName = vName.trimEnd();
        st.voiceName = vName.isEmpty() ? juce::String("INIT VOICE") : vName;

        st.outAssign = inst.outAssign & 0x03u;
        st.enabled = (perf.assignMode == 2u)
                         ? (inst.reserveNotes > 0u || inst.maxNotes > 0u)
                         : (inst.maxNotes > 0u);
        st.activeVoices = processor_.instActiveVoices(i);

        const int notesDisp = (perf.assignMode == 2u)
                                  ? ((inst.reserveNotes == 0u) ? 0 : static_cast<int>(inst.reserveNotes) - 1)
                                  : static_cast<int>(inst.maxNotes);
        if (st.pillNotes) {
            st.pillNotes->setValue(notesDisp, false);
            const juce::String nText = (perf.assignMode == 2u && inst.reserveNotes == 0u)
                                           ? "N:OFF"
                                           : juce::String::formatted("N:%02d", std::clamp(notesDisp, 0, 16));
            st.pillNotes->setDisplayText(nText, st.enabled);
        }

        if (st.pillShift) {
            const int sh = std::clamp<int>(inst.noteShift, -24, 24);
            st.pillShift->setValue(sh, false);
            st.pillShift->setDisplayText(formatSignedInt("SHF:", sh), st.enabled);
        }

        if (st.pillDetune) {
            const int det = std::clamp<int>(inst.detune, -7, 7);
            st.pillDetune->setValue(det, false);
            st.pillDetune->setDisplayText(formatSignedInt("DET:", det), st.enabled);
        }

        if (st.btnOutAssign) {
            const bool isOff = (st.outAssign == 0u);
            st.btnOutAssign->setVisualState(formatOutAssignLabel(st.outAssign),
                                            isOff ? tokens::kSilkDimLabel : tokens::kSilkWarmWhite,
                                            !isOff,
                                            kColSoloTeal,
                                            0x2400B8A9u,
                                            false);
            st.btnOutAssign->repaint();
        }

        const float balNorm = processor_.mixerChannelBalance(i);
        if (st.balKnob && !st.balKnob->isMouseButtonDown()) {
            const double balSteps = std::lround((balNorm >= 0.0f) ? (balNorm * 63.0f) : (balNorm * 64.0f));
            if (std::abs(st.balKnob->getValue() - balSteps) > 0.1) {
                st.balKnob->setValue(balSteps, juce::dontSendNotification);
            }
        }

        const int fxSendVal = static_cast<int>(processor_.mixerChannelFxSend(i));
        if (st.fxSendKnob && !st.fxSendKnob->isMouseButtonDown()) {
            if (std::abs(st.fxSendKnob->getValue() - static_cast<double>(fxSendVal)) > 0.1) {
                st.fxSendKnob->setValue(static_cast<double>(fxSendVal), juce::dontSendNotification);
            }
        }

        const bool eqOn = processor_.mixerChannelEqEnabled(i);
        if (st.btnEqToggle) {
            st.btnEqToggle->setVisualState("EQ",
                                           eqOn ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                           eqOn,
                                           kColSegGreenOn,
                                           0x3C2EE85Cu,
                                           false);
            st.btnEqToggle->repaint();
        }
        if (st.eqLoKnob) {
            st.eqLoKnob->getProperties().set("mixDimmed", !eqOn);
            if (!st.eqLoKnob->isMouseButtonDown()) {
                const double loDb = static_cast<double>(processor_.mixerChannelEqLowDb(i));
                if (std::abs(st.eqLoKnob->getValue() - loDb) > 0.05) {
                    st.eqLoKnob->setValue(loDb, juce::dontSendNotification);
                }
            }
            st.eqLoKnob->repaint();
        }
        if (st.eqHiKnob) {
            st.eqHiKnob->getProperties().set("mixDimmed", !eqOn);
            if (!st.eqHiKnob->isMouseButtonDown()) {
                const double hiDb = static_cast<double>(processor_.mixerChannelEqHighDb(i));
                if (std::abs(st.eqHiKnob->getValue() - hiDb) > 0.05) {
                    st.eqHiKnob->setValue(hiDb, juce::dontSendNotification);
                }
            }
            st.eqHiKnob->repaint();
        }

        const bool isMuted = seqMuteCtx
                                 ? processor_.isSequencerTrackMuted(i)
                                 : classicController_.isPerfInstrumentMuted(static_cast<int>(i));
        if (st.btnMute) {
            st.btnMute->setVisualState("M",
                                       isMuted ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                       isMuted,
                                       kColMuteRed,
                                       0x52FF5238u,
                                       false,
                                       isMuted && !blinkPhase_);
            st.btnMute->repaint();
        }

        const bool isSoloed = ((soloMask >> i) & 1u) != 0u;
        if (st.btnSolo) {
            st.btnSolo->setVisualState("S",
                                       isSoloed ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                       isSoloed,
                                       kColSoloTeal,
                                       0x4800B8A9u,
                                       false);
            st.btnSolo->repaint();
        }

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        syncBusButton(st.btnBusRoute.get(), i);
#endif

        if (st.fader && !st.fader->isMouseButtonDown()) {
            const double targetVal = static_cast<double>(std::min<uint8_t>(inst.volume, 99u));
            if (std::abs(st.fader->getValue() - targetVal) > 0.1) {
                st.fader->setValue(targetVal, juce::dontSendNotification);
            }
        }
    }

    // Strip 9: Rhythm
    {
        const auto& kit = mem.activeRhythmKit();
        const std::size_t kitSel = mem.selectedRhythmKit();
        const juce::String kitTag = (kitSel == 0u)
                                        ? "KIT:ROM"
                                        : juce::String::formatted("KIT:U%d", static_cast<int>(kitSel));
        const juce::String modeTag = processor_.isRhythmSongPlaying()
                                         ? "SONG"
                                         : (processor_.isRhythmEnginePlaying() ? "PTN" : "STBY");
        rhyStrip_.kitModeBadge = kitTag + " | " + modeTag;
        rhyStrip_.effectMode = kit.effectMode;
        rhyStrip_.activeVoices = totalRhyVoices_;

        if (rhyStrip_.btnEffectMode) {
            const bool efctActive = (kit.effectMode != vf50::dsp::drums::RhythmEffectMode::Off);
            rhyStrip_.btnEffectMode->setVisualState("EFCT: " + formatRhythmEfctLabel(kit.effectMode),
                                                    efctActive ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                                    efctActive,
                                                    kColSoloTeal,
                                                    0x2E00B8A9u,
                                                    false);
            rhyStrip_.btnEffectMode->repaint();
        }

        const float balNorm = processor_.mixerChannelBalance(8u);
        if (rhyStrip_.balKnob && !rhyStrip_.balKnob->isMouseButtonDown()) {
            const double balSteps = std::lround((balNorm >= 0.0f) ? (balNorm * 63.0f) : (balNorm * 64.0f));
            if (std::abs(rhyStrip_.balKnob->getValue() - balSteps) > 0.1) {
                rhyStrip_.balKnob->setValue(balSteps, juce::dontSendNotification);
            }
        }

        const int fxSendVal = static_cast<int>(processor_.mixerChannelFxSend(8u));
        if (rhyStrip_.fxSendKnob && !rhyStrip_.fxSendKnob->isMouseButtonDown()) {
            if (std::abs(rhyStrip_.fxSendKnob->getValue() - static_cast<double>(fxSendVal)) > 0.1) {
                rhyStrip_.fxSendKnob->setValue(static_cast<double>(fxSendVal), juce::dontSendNotification);
            }
        }

        const bool eqOn = processor_.mixerChannelEqEnabled(8u);
        if (rhyStrip_.btnEqToggle) {
            rhyStrip_.btnEqToggle->setVisualState("EQ",
                                                  eqOn ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                                  eqOn,
                                                  kColSegGreenOn,
                                                  0x3C2EE85Cu,
                                                  false);
            rhyStrip_.btnEqToggle->repaint();
        }
        if (rhyStrip_.eqLoKnob) {
            rhyStrip_.eqLoKnob->getProperties().set("mixDimmed", !eqOn);
            if (!rhyStrip_.eqLoKnob->isMouseButtonDown()) {
                const double loDb = static_cast<double>(processor_.mixerChannelEqLowDb(8u));
                if (std::abs(rhyStrip_.eqLoKnob->getValue() - loDb) > 0.05) {
                    rhyStrip_.eqLoKnob->setValue(loDb, juce::dontSendNotification);
                }
            }
            rhyStrip_.eqLoKnob->repaint();
        }
        if (rhyStrip_.eqHiKnob) {
            rhyStrip_.eqHiKnob->getProperties().set("mixDimmed", !eqOn);
            if (!rhyStrip_.eqHiKnob->isMouseButtonDown()) {
                const double hiDb = static_cast<double>(processor_.mixerChannelEqHighDb(8u));
                if (std::abs(rhyStrip_.eqHiKnob->getValue() - hiDb) > 0.05) {
                    rhyStrip_.eqHiKnob->setValue(hiDb, juce::dontSendNotification);
                }
            }
            rhyStrip_.eqHiKnob->repaint();
        }

        const bool rhyMuted = processor_.isSequencerTrackMuted(8u);
        if (rhyStrip_.btnMute) {
            rhyStrip_.btnMute->setVisualState("M",
                                              rhyMuted ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                              rhyMuted,
                                              kColMuteRed,
                                              0x52FF5238u,
                                              false,
                                              rhyMuted && !blinkPhase_);
            rhyStrip_.btnMute->repaint();
        }

        const bool rhySoloed = ((soloMask >> 8u) & 1u) != 0u;
        if (rhyStrip_.btnSolo) {
            rhyStrip_.btnSolo->setVisualState("S",
                                              rhySoloed ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                              rhySoloed,
                                              kColSoloTeal,
                                              0x4800B8A9u,
                                              false);
            rhyStrip_.btnSolo->repaint();
        }

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        syncBusButton(rhyStrip_.btnBusRoute.get(), 8u);
#endif

        if (rhyStrip_.fader && !rhyStrip_.fader->isMouseButtonDown()) {
            const double targetVol = static_cast<double>(std::min<uint8_t>(kit.rhythmVolume, 99u));
            if (std::abs(rhyStrip_.fader->getValue() - targetVol) > 0.1) {
                rhyStrip_.fader->setValue(targetVol, juce::dontSendNotification);
            }
        }
    }

    // Strip 10: Master / FX
    {
        const auto fx = mem.activeEffectSettings();
        const bool fxBypassed = (processor_.parameterBank().effectBypass->load(std::memory_order_relaxed) >= 0.5f);
        juce::String fxLabel;
        if (fx.effectType == 0u) {
            fxLabel = "00: OFF";
        } else {
            const auto& names = processor_.activeRomBank().dspEffectNames;
            if (static_cast<std::size_t>(fx.effectType) < names.size()) {
                const auto sv = names[static_cast<std::size_t>(fx.effectType)];
                juce::String nm(sv.data(), sv.size());
                nm = nm.trim();
                fxLabel = juce::String::formatted("%02d: ", static_cast<int>(fx.effectType)) + nm;
            } else {
                fxLabel = juce::String::formatted("FX #%02d", static_cast<int>(fx.effectType));
            }
        }
        if (fxBypassed) {
            fxLabel += " [BYP]";
        }
        if (masterStrip_.pillFxType) {
            masterStrip_.pillFxType->setValue(static_cast<int>(fx.effectType), false);
            masterStrip_.pillFxType->setDisplayText(fxLabel, !fxBypassed && fx.effectType != 0u);
        }

        if (masterStrip_.btnFxBypass) {
            masterStrip_.btnFxBypass->setVisualState("FX BYP",
                                                     fxBypassed ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                                     fxBypassed,
                                                     kColMuteRed,
                                                     0x48FF5238u,
                                                     true);
            masterStrip_.btnFxBypass->repaint();
        }

        if (masterStrip_.fxBalKnob && !masterStrip_.fxBalKnob->isMouseButtonDown()) {
            const double balVal = static_cast<double>(std::min<uint8_t>(fx.balance, 99u));
            if (std::abs(masterStrip_.fxBalKnob->getValue() - balVal) > 0.1) {
                masterStrip_.fxBalKnob->setValue(balVal, juce::dontSendNotification);
            }
        }

        const bool limOn = processor_.isMasterSafetyLimiterEnabled();
        masterStrip_.limiterGr = processor_.limiterGainReduction();
        if (masterStrip_.btnLimiter) {
            masterStrip_.btnLimiter->setVisualState(limOn ? "LIMIT -0.3dB" : "LIMIT OFF",
                                                    limOn ? tokens::kSilkWarmWhite : tokens::kSilkMutedSilver,
                                                    limOn,
                                                    kColSoloAmber,
                                                    0x3AF2B632u,
                                                    true);
            masterStrip_.btnLimiter->repaint();
        }
    }

    if (drumSubMixVisible_) {
        rebuildDrumSubMixGrid();
    }

    suppressCallbacks_ = false;
    updateMeterBallistics();

    if (isVisible()) {
        repaint();
    }
}

bool VF50MixerDrawer::setInstVolumeFromMixer(std::size_t inst0To7, int vol0To99) {
    if (inst0To7 >= 8u) {
        return false;
    }
    const int clamped = std::clamp(vol0To99, 0, 99);
    const bool ok = processor_.applyHostMixerPerfPartVolume(inst0To7, static_cast<uint8_t>(clamped));
    syncFromProcessor();
    notifyParentStateChanged();
    return ok;
}

bool VF50MixerDrawer::cycleInstOutAssignFromMixer(std::size_t inst0To7) {
    if (inst0To7 >= 8u) {
        return false;
    }
    processor_.pullLatestAudioMemoryToHost();
    const uint8_t cur = processor_.hostMemory().activePerformance().instruments[inst0To7].outAssign;
    return setInstOutAssignFromMixer(inst0To7, nextOutAssignValue(cur));
}

bool VF50MixerDrawer::setInstOutAssignFromMixer(std::size_t inst0To7, uint8_t outAssign0To3) {
    if (inst0To7 >= 8u) {
        return false;
    }
    const bool ok = processor_.applyHostMixerPerfPartOutAssign(inst0To7, static_cast<uint8_t>(outAssign0To3 & 0x03u));
    syncFromProcessor();
    notifyParentStateChanged();
    return ok;
}

bool VF50MixerDrawer::setInstNotesFromMixer(std::size_t inst0To7, int notes0To16) {
    if (inst0To7 >= 8u) {
        return false;
    }
    const int clamped = std::clamp(notes0To16, 0, 16);
    const bool ok = processor_.applyHostMixerPerfPartNotes(inst0To7, static_cast<uint8_t>(clamped));
    syncFromProcessor();
    notifyParentStateChanged();
    return ok;
}

bool VF50MixerDrawer::setInstNoteShiftFromMixer(std::size_t inst0To7, int shiftNeg24ToPos24) {
    if (inst0To7 >= 8u) {
        return false;
    }
    const int clamped = std::clamp(shiftNeg24ToPos24, -24, 24);
    const bool ok = processor_.applyHostMixerPerfPartNoteShift(inst0To7, static_cast<int8_t>(clamped));
    syncFromProcessor();
    notifyParentStateChanged();
    return ok;
}

bool VF50MixerDrawer::setInstDetuneFromMixer(std::size_t inst0To7, int detuneNeg7ToPos7) {
    if (inst0To7 >= 8u) {
        return false;
    }
    const int clamped = std::clamp(detuneNeg7ToPos7, -7, 7);
    const bool ok = processor_.applyHostMixerPerfPartDetune(inst0To7, static_cast<int8_t>(clamped));
    syncFromProcessor();
    notifyParentStateChanged();
    return ok;
}

void VF50MixerDrawer::toggleInstMuteFromMixer(std::size_t inst0To7) {
    if (inst0To7 >= 8u) {
        return;
    }
    if (classicController_.isSequencerMuteContext()) {
        processor_.toggleSequencerTrackMute(inst0To7);
    } else {
        classicController_.togglePerfInstrumentMute(static_cast<int>(inst0To7));
    }
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::toggleInstSoloFromMixer(std::size_t inst0To7) {
    if (inst0To7 >= 8u) {
        return;
    }
    processor_.toggleMixerSoloChannel(inst0To7);
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::setRhythmVolumeFromMixer(int vol0To99) {
    const int clamped = std::clamp(vol0To99, 0, 99);
    processor_.applyHostMixerRhythmVolume(static_cast<uint8_t>(clamped));
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::cycleRhythmEffectModeFromMixer() {
    processor_.pullLatestAudioMemoryToHost();
    const auto nextMode = nextRhythmEfctValue(processor_.hostMemory().activeRhythmKit().effectMode);
    processor_.applyHostMixerRhythmEffectMode(nextMode);
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::toggleRhythmMuteFromMixer() {
    processor_.toggleSequencerRhythmMute();
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::toggleRhythmSoloFromMixer() {
    processor_.toggleMixerSoloChannel(8u);
    syncFromProcessor();
    notifyParentStateChanged();
}

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
void VF50MixerDrawer::toggleChannelBusRouteFromMixer(std::size_t ch0To8) {
    if (ch0To8 >= 9u) {
        return;
    }
    processor_.toggleMixerChannelAuxExclusive(ch0To8);
    syncFromProcessor();
    notifyParentStateChanged();
}

bool VF50MixerDrawer::isChannelAuxRouteSelected(std::size_t ch0To8) const noexcept {
    return processor_.mixerChannelAuxExclusive(ch0To8);
}
#endif

void VF50MixerDrawer::setChannelBalanceFromMixer(std::size_t ch0To8, float balNeg1ToPos1) {
    if (ch0To8 >= 9u) {
        return;
    }
    processor_.setMixerChannelBalance(ch0To8, balNeg1ToPos1);
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::setChannelFxSendFromMixer(std::size_t ch0To8, int send0To99) {
    if (ch0To8 >= 9u) {
        return;
    }
    processor_.setMixerChannelFxSend(ch0To8, static_cast<uint8_t>(std::clamp(send0To99, 0, 99)));
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::setChannelEqFromMixer(std::size_t ch0To8,
                                           bool enabled,
                                           float lowDbNeg12ToPos12,
                                           float highDbNeg12ToPos12) {
    if (ch0To8 >= 9u) {
        return;
    }
    processor_.setMixerChannelEqEnabled(ch0To8, enabled);
    processor_.setMixerChannelEqLowDb(ch0To8, lowDbNeg12ToPos12);
    processor_.setMixerChannelEqHighDb(ch0To8, highDbNeg12ToPos12);
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::clearAllSoloFromMixer() {
    processor_.clearMixerSolo();
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::resetMixFromMixer() {
    processor_.pullLatestAudioMemoryToHost();
    processor_.resetMixerConsoleToNominal(true);
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::toggleFxBypassFromMixer() {
    const bool cur = processor_.parameterBank().effectBypass->load(std::memory_order_relaxed) >= 0.5f;
    vf50::params::setApvtsRawValue(processor_.apvts(), vf50::params::ParamIndex::EffectBypass, cur ? 0.0f : 1.0f);
    syncFromProcessor();
    notifyParentStateChanged();
}

bool VF50MixerDrawer::setFxTypeFromMixer(int fxType0To32) {
    const int clamped = std::clamp(fxType0To32, 0, 32);
    const bool ok = processor_.applyHostMixerEffectType(static_cast<uint8_t>(clamped));
    syncFromProcessor();
    notifyParentStateChanged();
    return ok;
}

bool VF50MixerDrawer::setFxBalanceFromMixer(int bal0To99) {
    const int clamped = std::clamp(bal0To99, 0, 99);
    const bool ok = processor_.applyHostMixerEffectBalance(static_cast<uint8_t>(clamped));
    syncFromProcessor();
    notifyParentStateChanged();
    return ok;
}

void VF50MixerDrawer::toggleSafetyLimiterFromMixer() {
    processor_.setMasterSafetyLimiterEnabled(!processor_.isMasterSafetyLimiterEnabled());
    syncFromProcessor();
    notifyParentStateChanged();
}

void VF50MixerDrawer::setDrumSubMixVisible(bool visible) {
    if (drumSubMixVisible_ == visible) {
        return;
    }
    drumSubMixVisible_ = visible;
    updateChannelStripVisibility();
    repaint();
}

void VF50MixerDrawer::setDrumFilterMode(DrumFilterMode mode) {
    drumFilterMode_ = mode;
    if (drumSubMixVisible_) {
        rebuildDrumSubMixGrid();
        repaint();
    }
}

void VF50MixerDrawer::setDrumInstVolumeFromMixer(std::size_t drum0To60, int vol0To15) {
    if (drum0To60 >= vf50::generated::kDrumInstrumentCount) {
        return;
    }
    processor_.applyHostMixerDrumInstVolume(drum0To60, static_cast<uint8_t>(std::clamp(vol0To15, 0, 15)));
    if (drumSubMixVisible_) {
        rebuildDrumSubMixGrid();
    }
    notifyParentStateChanged();
}

void VF50MixerDrawer::setDrumInstPanFromMixer(std::size_t drum0To60, int panNeg3ToPos3) {
    if (drum0To60 >= vf50::generated::kDrumInstrumentCount) {
        return;
    }
    processor_.applyHostMixerDrumInstPan(drum0To60, static_cast<int8_t>(std::clamp(panNeg3ToPos3, -3, 3)));
    if (drumSubMixVisible_) {
        rebuildDrumSubMixGrid();
    }
    notifyParentStateChanged();
}

int VF50MixerDrawer::displayedInstVolume(std::size_t inst0To7) const noexcept {
    if (inst0To7 >= 8u || !instStrips_[inst0To7].fader) {
        return 0;
    }
    return static_cast<int>(std::lround(instStrips_[inst0To7].fader->getValue()));
}

uint8_t VF50MixerDrawer::displayedInstOutAssign(std::size_t inst0To7) const noexcept {
    return (inst0To7 < 8u) ? instStrips_[inst0To7].outAssign : 0u;
}

int VF50MixerDrawer::displayedInstNotes(std::size_t inst0To7) const noexcept {
    return (inst0To7 < 8u && instStrips_[inst0To7].pillNotes) ? instStrips_[inst0To7].pillNotes->value() : 0;
}

int VF50MixerDrawer::displayedInstNoteShift(std::size_t inst0To7) const noexcept {
    return (inst0To7 < 8u && instStrips_[inst0To7].pillShift) ? instStrips_[inst0To7].pillShift->value() : 0;
}

int VF50MixerDrawer::displayedInstDetune(std::size_t inst0To7) const noexcept {
    return (inst0To7 < 8u && instStrips_[inst0To7].pillDetune) ? instStrips_[inst0To7].pillDetune->value() : 0;
}

int VF50MixerDrawer::displayedRhythmVolume() const noexcept {
    return rhyStrip_.fader ? static_cast<int>(std::lround(rhyStrip_.fader->getValue())) : 0;
}

vf50::dsp::drums::RhythmEffectMode VF50MixerDrawer::displayedRhythmEffectMode() const noexcept {
    return rhyStrip_.effectMode;
}

float VF50MixerDrawer::displayedChannelBalance(std::size_t ch0To8) const noexcept {
    return processor_.mixerChannelBalance(ch0To8);
}

int VF50MixerDrawer::displayedChannelFxSend(std::size_t ch0To8) const noexcept {
    return static_cast<int>(processor_.mixerChannelFxSend(ch0To8));
}

bool VF50MixerDrawer::isChannelEqEnabled(std::size_t ch0To8) const noexcept {
    return processor_.mixerChannelEqEnabled(ch0To8);
}

float VF50MixerDrawer::displayedChannelEqLowDb(std::size_t ch0To8) const noexcept {
    return processor_.mixerChannelEqLowDb(ch0To8);
}

float VF50MixerDrawer::displayedChannelEqHighDb(std::size_t ch0To8) const noexcept {
    return processor_.mixerChannelEqHighDb(ch0To8);
}

bool VF50MixerDrawer::isInstMuteButtonLit(std::size_t inst0To7) const noexcept {
    return (inst0To7 < 8u) && instStrips_[inst0To7].btnMute && instStrips_[inst0To7].btnMute->isLit();
}

bool VF50MixerDrawer::isRhythmMuteButtonLit() const noexcept {
    return rhyStrip_.btnMute && rhyStrip_.btnMute->isLit();
}

bool VF50MixerDrawer::isSoloButtonLit(std::size_t ch0To8) const noexcept {
    if (ch0To8 < 8u) {
        return instStrips_[ch0To8].btnSolo && instStrips_[ch0To8].btnSolo->isLit();
    }
    if (ch0To8 == 8u) {
        return rhyStrip_.btnSolo && rhyStrip_.btnSolo->isLit();
    }
    return false;
}

bool VF50MixerDrawer::isClearSoloButtonLit() const noexcept {
    return btnClearSolo_ && btnClearSolo_->isLit();
}

bool VF50MixerDrawer::isFxBypassButtonLit() const noexcept {
    return masterStrip_.btnFxBypass && masterStrip_.btnFxBypass->isLit();
}

int VF50MixerDrawer::displayedFxType() const noexcept {
    return masterStrip_.pillFxType ? masterStrip_.pillFxType->value() : 0;
}

int VF50MixerDrawer::displayedFxBalance() const noexcept {
    return masterStrip_.fxBalKnob
               ? static_cast<int>(std::lround(masterStrip_.fxBalKnob->getValue()))
               : 50;
}

bool VF50MixerDrawer::isSafetyLimiterButtonLit() const noexcept {
    return masterStrip_.btnLimiter && masterStrip_.btnLimiter->isLit();
}

juce::String VF50MixerDrawer::formatOutAssignLabel(uint8_t outAssign) noexcept {
    switch (outAssign & 0x03u) {
        case 1u: return "OUT: L";
        case 2u: return "OUT: R";
        case 3u: return "OUT: L+R";
        default: return "OUT: OFF";
    }
}

juce::String VF50MixerDrawer::formatRhythmEfctLabel(vf50::dsp::drums::RhythmEffectMode mode) noexcept {
    switch (mode) {
        case vf50::dsp::drums::RhythmEffectMode::Off: return "OFF";
        case vf50::dsp::drums::RhythmEffectMode::On:  return "ON";
        default:                                     return "MIX";
    }
}

juce::String VF50MixerDrawer::formatRecvChannelBadge(uint8_t ch) noexcept {
    if (ch == 16u) {
        return "OMNI";
    }
    if (ch < 16u) {
        return juce::String::formatted("CH%d", static_cast<int>(ch) + 1);
    }
    return "OFF";
}

juce::String VF50MixerDrawer::formatBalanceText(float balNeg1ToPos1) noexcept {
    const int steps = static_cast<int>(std::lround(
        (balNeg1ToPos1 >= 0.0f) ? (balNeg1ToPos1 * 63.0f) : (balNeg1ToPos1 * 64.0f)));
    if (steps == 0) {
        return "C";
    }
    if (steps < 0) {
        return "L" + juce::String(-steps);
    }
    return "R" + juce::String(steps);
}

juce::String VF50MixerDrawer::formatEqDbText(float db) noexcept {
    if (std::abs(db) < 0.05f) {
        return "0.0";
    }
    return juce::String::formatted("%+.1f", static_cast<double>(db));
}

juce::String VF50MixerDrawer::formatSignedInt(const juce::String& prefix, int val) noexcept {
    if (val == 0) {
        return prefix + "0";
    }
    return prefix + juce::String::formatted("%+d", val);
}

void VF50MixerDrawer::resized() {
    const int W = getWidth();
    headerRect_ = { 0, 0, W, 60 };
    patchBadgeRect_ = { 530, 11, 1140, 38 };
    polyBadgeRect_  = { 1686, 11, 440, 38 };
    if (btnClearSolo_) {
        btnClearSolo_->setBounds(W - 508, 11, 232, 38);
    }
    if (btnResetMix_) {
        btnResetMix_->setBounds(W - 260, 11, 238, 38);
    }

    constexpr int kMarginX = 20;
    constexpr int kTopY = 68;
    constexpr int kStripH = 720;
    constexpr int kInstW = 274;
    constexpr int kInstGap = 8;
    constexpr int kRhyW = 304;
    constexpr int kMasterW = 340;

    int x = kMarginX;
    for (std::size_t i = 0u; i < 8u; ++i) {
        auto& st = instStrips_[i];
        st.stripRect = { x, kTopY, kInstW, kStripH };
        st.headerRect = { x + 10, kTopY + 6, 56, 24 };
        st.pipsRect = { x + 70, kTopY + 11, 108, 14 };
        if (st.pillNotes) {
            st.pillNotes->setBounds(x + kInstW - 88, kTopY + 6, 78, 24);
        }
        st.badgeRect = { x + 10, kTopY + 32, kInstW - 20, 20 };
        st.voiceNameRect = { x + 10, kTopY + 54, kInstW - 20, 24 };

        if (st.pillShift) {
            st.pillShift->setBounds(x + 10, kTopY + 84, 78, 32);
        }
        if (st.pillDetune) {
            st.pillDetune->setBounds(x + 93, kTopY + 84, 74, 32);
        }
        if (st.btnOutAssign) {
            st.btnOutAssign->setBounds(x + 172, kTopY + 84, 92, 32);
        }

        // Row A Knobs: BAL + FX
        st.balLabelRect = { x + 12, kTopY + 122, 120, 14 };
        if (st.balKnob) {
            st.balKnob->setBounds(x + 44, kTopY + 136, 56, 56);
        }
        st.balValueRect = { x + 12, kTopY + 192, 120, 16 };

        st.fxLabelRect = { x + 142, kTopY + 122, 120, 14 };
        if (st.fxSendKnob) {
            st.fxSendKnob->setBounds(x + 174, kTopY + 136, 56, 56);
        }
        st.fxValueRect = { x + 142, kTopY + 192, 120, 16 };

        // Row B Knobs: EQ + LO + HI
        if (st.btnEqToggle) {
            st.btnEqToggle->setBounds(x + 10, kTopY + 236, 52, 34);
        }
        st.loLabelRect = { x + 68, kTopY + 212, 92, 14 };
        if (st.eqLoKnob) {
            st.eqLoKnob->setBounds(x + 88, kTopY + 226, 52, 52);
        }
        st.loValueRect = { x + 68, kTopY + 278, 92, 16 };

        st.hiLabelRect = { x + 168, kTopY + 212, 92, 14 };
        if (st.eqHiKnob) {
            st.eqHiKnob->setBounds(x + 188, kTopY + 226, 52, 52);
        }
        st.hiValueRect = { x + 168, kTopY + 278, 92, 16 };

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        if (st.btnMute) {
            st.btnMute->setBounds(x + 14, kTopY + 304, 62, 36);
        }
        if (st.btnSolo) {
            st.btnSolo->setBounds(x + 82, kTopY + 304, 62, 36);
        }
        if (st.btnBusRoute) {
            st.btnBusRoute->setBounds(x + 150, kTopY + 304, 110, 36);
        }
#else
        const int halfW = (kInstW - 36) / 2;
        if (st.btnMute) {
            st.btnMute->setBounds(x + 14, kTopY + 304, halfW, 36);
        }
        if (st.btnSolo) {
            st.btnSolo->setBounds(x + 22 + halfW, kTopY + 304, halfW, 36);
        }
#endif

        const int faderY = kTopY + 348;
        const int faderH = 324;
        if (st.fader) {
            st.fader->setBounds(x + 14, faderY, 128, faderH);
        }
        st.meterLRect = { x + 156, faderY + 8, 44, faderH - 30 };
        st.meterRRect = { x + 208, faderY + 8, 44, faderH - 30 };
        st.readoutRect = { x + 36, kTopY + 678, kInstW - 72, 32 };

        x += kInstW + kInstGap;
    }

    // Strip 9: RHYTHM (after 16px divider gap; x already has +8)
    x += 8;
    {
        rhyStrip_.stripRect = { x, kTopY, kRhyW, kStripH };
        rhyStrip_.headerRect = { x + 12, kTopY + 6, 130, 24 };
        rhyStrip_.pipsRect = { x + 148, kTopY + 11, 142, 14 };
        rhyStrip_.badgeRect = { x + 12, kTopY + 32, kRhyW - 24, 20 };

        if (rhyStrip_.btnDrumSubMix) {
            rhyStrip_.btnDrumSubMix->setBounds(x + 14, kTopY + 54, kRhyW - 28, 26);
        }
        if (rhyStrip_.btnEffectMode) {
            rhyStrip_.btnEffectMode->setBounds(x + 14, kTopY + 84, kRhyW - 28, 32);
        }

        rhyStrip_.balLabelRect = { x + 16, kTopY + 122, 132, 14 };
        if (rhyStrip_.balKnob) {
            rhyStrip_.balKnob->setBounds(x + 54, kTopY + 136, 56, 56);
        }
        rhyStrip_.balValueRect = { x + 16, kTopY + 192, 132, 16 };

        rhyStrip_.fxLabelRect = { x + 156, kTopY + 122, 132, 14 };
        if (rhyStrip_.fxSendKnob) {
            rhyStrip_.fxSendKnob->setBounds(x + 194, kTopY + 136, 56, 56);
        }
        rhyStrip_.fxValueRect = { x + 156, kTopY + 192, 132, 16 };

        if (rhyStrip_.btnEqToggle) {
            rhyStrip_.btnEqToggle->setBounds(x + 14, kTopY + 236, 56, 34);
        }
        rhyStrip_.loLabelRect = { x + 82, kTopY + 212, 98, 14 };
        if (rhyStrip_.eqLoKnob) {
            rhyStrip_.eqLoKnob->setBounds(x + 105, kTopY + 226, 52, 52);
        }
        rhyStrip_.loValueRect = { x + 82, kTopY + 278, 98, 16 };

        rhyStrip_.hiLabelRect = { x + 188, kTopY + 212, 98, 14 };
        if (rhyStrip_.eqHiKnob) {
            rhyStrip_.eqHiKnob->setBounds(x + 211, kTopY + 226, 52, 52);
        }
        rhyStrip_.hiValueRect = { x + 188, kTopY + 278, 98, 16 };

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
        if (rhyStrip_.btnMute) {
            rhyStrip_.btnMute->setBounds(x + 16, kTopY + 304, 68, 36);
        }
        if (rhyStrip_.btnSolo) {
            rhyStrip_.btnSolo->setBounds(x + 90, kTopY + 304, 68, 36);
        }
        if (rhyStrip_.btnBusRoute) {
            rhyStrip_.btnBusRoute->setBounds(x + 164, kTopY + 304, 124, 36);
        }
#else
        const int halfW = (kRhyW - 40) / 2;
        if (rhyStrip_.btnMute) {
            rhyStrip_.btnMute->setBounds(x + 16, kTopY + 304, halfW, 36);
        }
        if (rhyStrip_.btnSolo) {
            rhyStrip_.btnSolo->setBounds(x + 24 + halfW, kTopY + 304, halfW, 36);
        }
#endif

        const int faderY = kTopY + 348;
        const int faderH = 324;
        if (rhyStrip_.fader) {
            rhyStrip_.fader->setBounds(x + 18, faderY, 136, faderH);
        }
        rhyStrip_.meterLRect = { x + 174, faderY + 8, 48, faderH - 30 };
        rhyStrip_.meterRRect = { x + 232, faderY + 8, 48, faderH - 30 };
        rhyStrip_.readoutRect = { x + 44, kTopY + 678, kRhyW - 88, 32 };

        x += kRhyW + 16;
    }

    // Strip 10: MASTER / FX
    {
        masterStrip_.stripRect = { x, kTopY, kMasterW, kStripH };
        masterStrip_.headerRect = { x + 14, kTopY + 6, kMasterW - 28, 24 };

        if (masterStrip_.pillFxType) {
            masterStrip_.pillFxType->setBounds(x + 14, kTopY + 36, kMasterW - 28, 40);
        }

        masterStrip_.fxBalLabelRect = { x + 14, kTopY + 84, 130, 14 };
        if (masterStrip_.fxBalKnob) {
            masterStrip_.fxBalKnob->setBounds(x + 45, kTopY + 98, 68, 68);
        }
        masterStrip_.fxBalReadoutRect = { x + 14, kTopY + 166, 130, 18 };

        if (masterStrip_.btnFxBypass) {
            masterStrip_.btnFxBypass->setBounds(x + 160, kTopY + 104, kMasterW - 174, 46);
        }

        if (masterStrip_.btnLimiter) {
            masterStrip_.btnLimiter->setBounds(x + 16, kTopY + 196, kMasterW - 32, 38);
        }
        masterStrip_.limitGrRect = { x + 18, kTopY + 242, kMasterW - 36, 16 };

        const int faderY = kTopY + 348;
        const int faderH = 324;
        if (masterStrip_.masterFader) {
            masterStrip_.masterFader->setBounds(x + 22, faderY, 140, faderH);
        }
        masterStrip_.meterLRect = { x + 184, faderY + 8, 56, faderH - 30 };
        masterStrip_.meterRRect = { x + 252, faderY + 8, 56, faderH - 30 };
        masterStrip_.readoutRect = { x + 36, kTopY + 678, kMasterW - 72, 32 };
    }

    // Embedded 61-Drum Sub-Mix View layout
    drumSubMixRect_ = { kMarginX, kTopY, W - kMarginX * 2, kStripH };
    drumSubMixHeaderRect_ = { kMarginX, kTopY, W - kMarginX * 2, 46 };
    if (btnDrumFilterUsed_) {
        btnDrumFilterUsed_->setBounds(kMarginX + 640, kTopY + 6, 210, 34);
    }
    if (btnDrumFilterAll_) {
        btnDrumFilterAll_->setBounds(kMarginX + 862, kTopY + 6, 140, 34);
    }
    if (btnDrumFilterKsh_) {
        btnDrumFilterKsh_->setBounds(kMarginX + 1014, kTopY + 6, 220, 34);
    }
    if (btnDrumBackToMixer_) {
        btnDrumBackToMixer_->setBounds(drumSubMixRect_.getRight() - 250, kTopY + 6, 240, 34);
    }
    if (drumSubMixVisible_) {
        rebuildDrumSubMixGrid();
    }
}

void VF50MixerDrawer::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();

    // Matte dark-graphite chassis gradient
    juce::ColourGradient bgGrad(juce::Colour(kColDrawerTop), 0.0f, 0.0f,
                                juce::Colour(kColDrawerBot), 0.0f, bounds.getBottom(), false);
    g.setGradientFill(bgGrad);
    g.fillRect(bounds);

    // Top contour groove separating faceplate and drawer
    g.setColour(juce::Colour(tokens::kGrooveShadow));
    g.fillRect(0.0f, 0.0f, bounds.getWidth(), 3.0f);
    g.setColour(juce::Colour(tokens::kGrooveHighlight));
    g.fillRect(0.0f, 3.0f, bounds.getWidth(), 1.5f);

    paintHeaderStrip(g);

    if (drumSubMixVisible_) {
        paintDrumSubMixBackground(g);
        return;
    }

    for (std::size_t i = 0u; i < 8u; ++i) {
        paintInstStrip(g, i);
    }

    // Subtle divider line before RHYTHM strip
    {
        const float divX = static_cast<float>(rhyStrip_.stripRect.getX() - 8);
        g.setColour(juce::Colour(tokens::kGrooveShadow));
        g.fillRect(divX - 1.0f, 74.0f, 2.0f, 708.0f);
        g.setColour(juce::Colour(tokens::kGrooveHighlight));
        g.fillRect(divX + 1.0f, 74.0f, 1.0f, 708.0f);
    }
    paintRhythmStrip(g);

    // Crisp divider line before MASTER / FX strip
    {
        const float divX = static_cast<float>(masterStrip_.stripRect.getX() - 8);
        g.setColour(juce::Colour(tokens::kGrooveShadow));
        g.fillRect(divX - 1.5f, 74.0f, 3.0f, 708.0f);
        g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.35f));
        g.fillRect(divX + 1.5f, 74.0f, 1.5f, 708.0f);
    }
    paintMasterStrip(g);
}

void VF50MixerDrawer::paintHeaderStrip(juce::Graphics& g) const {
    g.setColour(juce::Colour(0xFF14181Eu));
    g.fillRect(headerRect_);
    g.setColour(juce::Colour(kColStripBorder));
    g.fillRect(0, headerRect_.getBottom() - 2, getWidth(), 2);

    // Teal accent bar on left
    g.setColour(juce::Colour(kColSoloTeal));
    g.fillRoundedRectangle(22.0f, 14.0f, 8.0f, 32.0f, 3.0f);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(21.0f, true, 1.04f));
    g.drawText("PERFORMANCE & RHYTHM MIXER", 40, 10, 480, 40, juce::Justification::centredLeft, false);

    // Active patch / compare / edit readout badge
    const auto& mem = processor_.hostMemory();
    const auto& perf = mem.activePerformance();
    juce::String perfName(perf.name.data(), perf.name.size());
    perfName = perfName.trimEnd();
    if (perfName.isEmpty()) {
        perfName = "INIT PERF";
    }
    const bool isUser = (mem.selectedPerfBank() == vf50::state::MemoryBank::UserRam);
    const bool isSingle = (mem.effectiveFmPlayMode() == vf50::dsp::fm::SynthPlayMode::Single);
    const bool isCompare = mem.isPerformanceCompareActive();
    const bool isDirty = mem.isPerfEditDirty();

    juce::String badgeText = juce::String::formatted("PMEM %c%02d: ", isUser ? 'I' : 'P',
                                                     static_cast<int>(mem.selectedPerfIndex()))
                           + perfName;
    if (isSingle) {
        badgeText += "   [SINGLE MODE ACTIVE]";
    }
    if (isCompare) {
        badgeText += "   [COMPARE]";
    } else if (isDirty) {
        badgeText += "   [EDITED*]";
    }

    const auto br = patchBadgeRect_.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(br, 5.0f);
    g.setColour(juce::Colour(isCompare ? kColSoloAmber : kColStripBorder));
    g.drawRoundedRectangle(br, 5.0f, 1.4f);

    g.setColour(juce::Colour(isCompare ? kColSoloAmber : tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(16.0f, true, 0.98f));
    g.drawText(badgeText, patchBadgeRect_.reduced(14, 0), juce::Justification::centred, true);

    // Master Polyphony Readout: FM 00/16 | RHY 0/8
    const auto pr = polyBadgeRect_.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(pr, 5.0f);
    g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.45f));
    g.drawRoundedRectangle(pr, 5.0f, 1.3f);

    const juce::String polyTxt = juce::String::formatted(
        "FM %02d/16  |  RHY %d/8",
        static_cast<int>(std::min<uint8_t>(totalFmVoices_, 16u)),
        static_cast<int>(std::min<uint8_t>(totalRhyVoices_, 8u)));
    g.setColour(juce::Colour((totalFmVoices_ > 0u || totalRhyVoices_ > 0u) ? kColSoloTeal : tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(16.0f, true, 1.02f));
    g.drawText(polyTxt, polyBadgeRect_.reduced(10, 0), juce::Justification::centred, false);
}

void VF50MixerDrawer::paintVoicePips(juce::Graphics& g,
                                    const juce::Rectangle<int>& rect,
                                    int maxPips,
                                    int activeCount,
                                    uint32_t onArgb) const {
    if (maxPips <= 0) {
        return;
    }
    const float gap = 2.0f;
    const float totalW = static_cast<float>(rect.getWidth());
    const float pipW = std::max(2.5f, (totalW - gap * static_cast<float>(maxPips - 1)) / static_cast<float>(maxPips));
    const float pipH = static_cast<float>(rect.getHeight());

    for (int p = 0; p < maxPips; ++p) {
        const float px = static_cast<float>(rect.getX()) + static_cast<float>(p) * (pipW + gap);
        const bool lit = (p < activeCount);
        g.setColour(juce::Colour(lit ? onArgb : 0xFF1F252Eu));
        g.fillRoundedRectangle(px, static_cast<float>(rect.getY()), pipW, pipH, 1.2f);
    }
}

void VF50MixerDrawer::paintInstStrip(juce::Graphics& g, std::size_t idx) const {
    const auto& st = instStrips_[idx];
    const auto sr = st.stripRect.toFloat();

    g.setColour(juce::Colour(kColStripFill));
    g.fillRoundedRectangle(sr, 7.0f);
    g.setColour(juce::Colour(kColStripBorder));
    g.drawRoundedRectangle(sr, 7.0f, 1.5f);

    // Header: TR1..TR8 + 16 mini active-voice pips
    g.setColour(juce::Colour(st.enabled ? tokens::kSilkWarmWhite : tokens::kSilkDimLabel));
    g.setFont(VF50LookAndFeel::makeFont(18.5f, true, 1.02f));
    g.drawText(juce::String::formatted("TR%d", static_cast<int>(idx) + 1),
               st.headerRect, juce::Justification::centredLeft, false);

    paintVoicePips(g, st.pipsRect, 16, static_cast<int>(st.activeVoices), kColSegGreenOn);

    // Voice Slot & MIDI Recv Ch badge
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true, 0.96f));
    g.drawText(st.slotChBadge, st.badgeRect, juce::Justification::centred, false);

    // Voice Name in dark recessed pill
    const auto vr = st.voiceNameRect.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(vr, 4.0f);
    g.setColour(juce::Colour(0xFF242A33u));
    g.drawRoundedRectangle(vr, 4.0f, 1.1f);
    g.setColour(juce::Colour(st.enabled ? kColSoloTeal : tokens::kSilkDimLabel));
    g.setFont(VF50LookAndFeel::makeFont(14.5f, true, 0.96f));
    g.drawFittedText(st.voiceName, st.voiceNameRect.reduced(8, 0), juce::Justification::centred, 1);

    // Knob labels & readouts
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(11.5f, true, 1.0f));
    g.drawText("BAL", st.balLabelRect, juce::Justification::centred, false);
    g.drawText("FX", st.fxLabelRect, juce::Justification::centred, false);
    g.drawText("LO", st.loLabelRect, juce::Justification::centred, false);
    g.drawText("HI", st.hiLabelRect, juce::Justification::centred, false);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(12.5f, true, 0.98f));
    g.drawText(formatBalanceText(processor_.mixerChannelBalance(idx)),
               st.balValueRect, juce::Justification::centred, false);
    g.drawText(juce::String::formatted("%02d", static_cast<int>(processor_.mixerChannelFxSend(idx))),
               st.fxValueRect, juce::Justification::centred, false);

    const bool eqOn = processor_.mixerChannelEqEnabled(idx);
    g.setColour(juce::Colour(eqOn ? tokens::kSilkWarmWhite : tokens::kSilkDimLabel));
    g.drawText(formatEqDbText(processor_.mixerChannelEqLowDb(idx)),
               st.loValueRect, juce::Justification::centred, false);
    g.drawText(formatEqDbText(processor_.mixerChannelEqHighDb(idx)),
               st.hiValueRect, juce::Justification::centred, false);

    // Dual L / R Segmented LED Peak Meters beside fader
    paintLedPeakMeter(g, st.meterLRect, st.meterL);
    paintLedPeakMeter(g, st.meterRRect, st.meterR);

    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(11.5f, true, 1.0f));
    g.drawText("L", st.meterLRect.getX(), st.meterLRect.getBottom() + 2,
               st.meterLRect.getWidth(), 16, juce::Justification::centred, false);
    g.drawText("R", st.meterRRect.getX(), st.meterRRect.getBottom() + 2,
               st.meterRRect.getWidth(), 16, juce::Justification::centred, false);

    // Recessed numeric readout (00..99)
    const int volVal = st.fader ? static_cast<int>(std::lround(st.fader->getValue())) : 0;
    const auto rr = st.readoutRect.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(rr, 4.0f);
    g.setColour(juce::Colour(kColStripBorder));
    g.drawRoundedRectangle(rr, 4.0f, 1.2f);
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(18.0f, true, 1.04f));
    g.drawText(juce::String::formatted("%02d", std::clamp(volVal, 0, 99)),
               st.readoutRect, juce::Justification::centred, false);
}

void VF50MixerDrawer::paintRhythmStrip(juce::Graphics& g) const {
    const auto sr = rhyStrip_.stripRect.toFloat();

    // Distinct subtle teal accent border
    g.setColour(juce::Colour(0xFF161D22u));
    g.fillRoundedRectangle(sr, 7.0f);
    g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.55f));
    g.drawRoundedRectangle(sr, 7.0f, 1.8f);

    g.setColour(juce::Colour(kColSoloTeal));
    g.setFont(VF50LookAndFeel::makeFont(19.0f, true, 1.04f));
    g.drawText("RHYTHM", rhyStrip_.headerRect, juce::Justification::centredLeft, false);

    paintVoicePips(g, rhyStrip_.pipsRect, 8, static_cast<int>(rhyStrip_.activeVoices), kColSoloTeal);

    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true, 0.96f));
    g.drawText(rhyStrip_.kitModeBadge, rhyStrip_.badgeRect, juce::Justification::centred, false);

    // Knob labels & readouts
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(11.5f, true, 1.0f));
    g.drawText("BAL", rhyStrip_.balLabelRect, juce::Justification::centred, false);
    g.drawText("FX", rhyStrip_.fxLabelRect, juce::Justification::centred, false);
    g.drawText("LO", rhyStrip_.loLabelRect, juce::Justification::centred, false);
    g.drawText("HI", rhyStrip_.hiLabelRect, juce::Justification::centred, false);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(12.5f, true, 0.98f));
    g.drawText(formatBalanceText(processor_.mixerChannelBalance(8u)),
               rhyStrip_.balValueRect, juce::Justification::centred, false);
    g.drawText(juce::String::formatted("%02d", static_cast<int>(processor_.mixerChannelFxSend(8u))),
               rhyStrip_.fxValueRect, juce::Justification::centred, false);

    const bool eqOn = processor_.mixerChannelEqEnabled(8u);
    g.setColour(juce::Colour(eqOn ? tokens::kSilkWarmWhite : tokens::kSilkDimLabel));
    g.drawText(formatEqDbText(processor_.mixerChannelEqLowDb(8u)),
               rhyStrip_.loValueRect, juce::Justification::centred, false);
    g.drawText(formatEqDbText(processor_.mixerChannelEqHighDb(8u)),
               rhyStrip_.hiValueRect, juce::Justification::centred, false);

    // Dual L / R Peak LED Meters
    paintLedPeakMeter(g, rhyStrip_.meterLRect, rhyStrip_.meterL);
    paintLedPeakMeter(g, rhyStrip_.meterRRect, rhyStrip_.meterR);

    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(11.5f, true, 1.0f));
    g.drawText("L", rhyStrip_.meterLRect.getX(), rhyStrip_.meterLRect.getBottom() + 2,
               rhyStrip_.meterLRect.getWidth(), 16, juce::Justification::centred, false);
    g.drawText("R", rhyStrip_.meterRRect.getX(), rhyStrip_.meterRRect.getBottom() + 2,
               rhyStrip_.meterRRect.getWidth(), 16, juce::Justification::centred, false);

    const int volVal = rhyStrip_.fader ? static_cast<int>(std::lround(rhyStrip_.fader->getValue())) : 0;
    const auto rr = rhyStrip_.readoutRect.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(rr, 4.0f);
    g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.50f));
    g.drawRoundedRectangle(rr, 4.0f, 1.2f);
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(18.0f, true, 1.04f));
    g.drawText(juce::String::formatted("%02d", std::clamp(volVal, 0, 99)),
               rhyStrip_.readoutRect, juce::Justification::centred, false);
}

void VF50MixerDrawer::paintMasterStrip(juce::Graphics& g) const {
    const auto sr = masterStrip_.stripRect.toFloat();

    g.setColour(juce::Colour(0xFF181C23u));
    g.fillRoundedRectangle(sr, 7.0f);
    g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.65f));
    g.drawRoundedRectangle(sr, 7.0f, 1.8f);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(19.0f, true, 1.04f));
    g.drawText("MASTER", masterStrip_.headerRect, juce::Justification::centredLeft, false);

    g.setColour(juce::Colour(kColSoloTeal));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true, 0.96f));
#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    g.drawText("MAIN / FX BUS", masterStrip_.headerRect, juce::Justification::centredRight, false);
#else
    g.drawText("STEREO BUS", masterStrip_.headerRect, juce::Justification::centredRight, false);
#endif

    // FX BAL label + readout
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(12.0f, true, 0.96f));
    g.drawText("FX BAL", masterStrip_.fxBalLabelRect, juce::Justification::centred, false);

    const int balVal = masterStrip_.fxBalKnob
                           ? static_cast<int>(std::lround(masterStrip_.fxBalKnob->getValue()))
                           : 50;
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true, 0.98f));
    g.drawText(juce::String::formatted("%02d%%", std::clamp(balVal, 0, 99)),
               masterStrip_.fxBalReadoutRect, juce::Justification::centred, false);

    // Gain-reduction indicator bar under LIMIT toggle
    const auto grRect = masterStrip_.limitGrRect.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(grRect, 3.0f);
    g.setColour(juce::Colour(0xFF28303Au));
    g.drawRoundedRectangle(grRect, 3.0f, 1.0f);

    const float grNorm = std::clamp(masterStrip_.limiterGr * 4.0f, 0.0f, 1.0f);
    if (grNorm > 0.01f) {
        const float barW = grNorm * (grRect.getWidth() - 4.0f);
        g.setColour(juce::Colour(kColSoloAmber));
        g.fillRoundedRectangle(grRect.getRight() - 2.0f - barW, grRect.getY() + 2.0f,
                               barW, grRect.getHeight() - 4.0f, 2.0f);
    }
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(10.5f, true, 0.96f));
    g.drawText(grNorm > 0.01f ? "GR ACTIVE" : "SAFETY CEILING",
               masterStrip_.limitGrRect, juce::Justification::centred, false);

    // Wide Stereo L / R Peak LED Meters
    paintLedPeakMeter(g, masterStrip_.meterLRect, masterStrip_.meterL);
    paintLedPeakMeter(g, masterStrip_.meterRRect, masterStrip_.meterR);

    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(12.5f, true, 1.0f));
    g.drawText("L", masterStrip_.meterLRect.getX(), masterStrip_.meterLRect.getBottom() + 2,
               masterStrip_.meterLRect.getWidth(), 16, juce::Justification::centred, false);
    g.drawText("R", masterStrip_.meterRRect.getX(), masterStrip_.meterRRect.getBottom() + 2,
               masterStrip_.meterRRect.getWidth(), 16, juce::Justification::centred, false);

    // Master Volume numeric readout (00..99)
    const double rawMaster = masterStrip_.masterFader ? masterStrip_.masterFader->getValue() : 0.80;
    const int masterDisp = std::clamp(static_cast<int>(std::lround((rawMaster / 1.25) * 99.0)), 0, 99);
    const auto rr = masterStrip_.readoutRect.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(rr, 4.0f);
    g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.55f));
    g.drawRoundedRectangle(rr, 4.0f, 1.2f);
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(18.0f, true, 1.04f));
    g.drawText(juce::String::formatted("%02d", masterDisp),
               masterStrip_.readoutRect, juce::Justification::centred, false);
}

void VF50MixerDrawer::paintDrumSubMixBackground(juce::Graphics& g) const {
    const auto hr = drumSubMixHeaderRect_.toFloat();
    g.setColour(juce::Colour(0xFF14181Eu));
    g.fillRoundedRectangle(hr, 6.0f);
    g.setColour(juce::Colour(kColSoloTeal).withAlpha(0.45f));
    g.drawRoundedRectangle(hr, 6.0f, 1.3f);

    g.setColour(juce::Colour(kColSoloTeal));
    g.setFont(VF50LookAndFeel::makeFont(18.5f, true, 1.03f));
    g.drawText("RHYTHM INST SET — 61-DRUM SUB-MIX (VOL 0-15 / PAN L3-R3)",
               drumSubMixHeaderRect_.withWidth(620).reduced(14, 0),
               juce::Justification::centredLeft, false);
}

void VF50MixerDrawer::paintLedPeakMeter(juce::Graphics& g,
                                       const juce::Rectangle<int>& rect,
                                       const MeterChannelState& state) const {
    const auto well = rect.toFloat();
    g.setColour(juce::Colour(kColWellBg));
    g.fillRoundedRectangle(well, 4.0f);
    g.setColour(juce::Colour(0xFF282E38u));
    g.drawRoundedRectangle(well, 4.0f, 1.1f);

    constexpr int kSegments = 18;
    const auto inner = well.reduced(4.0f, 5.0f);
    const float gap = 2.2f;
    const float segH = (inner.getHeight() - gap * static_cast<float>(kSegments - 1)) / static_cast<float>(kSegments);
    const int litCount = static_cast<int>(std::lround(std::clamp(state.val, 0.0f, 1.0f) * static_cast<float>(kSegments)));
    const int holdIdx = static_cast<int>(std::floor(std::clamp(state.hold, 0.0f, 0.999f) * static_cast<float>(kSegments)));

    for (int s = 0; s < kSegments; ++s) {
        const float segY = inner.getBottom() - static_cast<float>(s + 1) * segH - static_cast<float>(s) * gap;
        const juce::Rectangle<float> segRect(inner.getX(), segY, inner.getWidth(), segH);

        uint32_t onCol = kColSegGreenOn;
        if (s >= 16) {
            onCol = kColSegRedOn;
        } else if (s >= 12) {
            onCol = kColSegAmberOn;
        }

        const bool isClipTop = (s == kSegments - 1) && state.clipped;
        const bool isLit = (s < litCount) || isClipTop;
        const bool isHold = (state.hold > 0.02f && s == holdIdx);
        if (isLit) {
            g.setColour(juce::Colour(onCol));
            g.fillRoundedRectangle(segRect, 1.3f);
        } else if (isHold) {
            g.setColour(juce::Colour(onCol).withAlpha(0.75f));
            g.fillRoundedRectangle(segRect, 1.3f);
        } else {
            g.setColour(juce::Colour(kColSegOff));
            g.fillRoundedRectangle(segRect, 1.3f);
        }
    }
}

} // namespace vf50::gui
