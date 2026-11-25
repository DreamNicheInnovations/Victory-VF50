#include "GUI/VF50ProgrammerDrawer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vf50::gui {

namespace {

constexpr uint32_t kColPgBgTop       = 0xFF1A1F26u;
constexpr uint32_t kColPgBgBot       = 0xFF11151Au;
constexpr uint32_t kColBayFill       = 0xFF151920u;
constexpr uint32_t kColBayBorder     = 0xFF2B323Eu;
constexpr uint32_t kColTealGlow      = 0xFF28D8E6u;
constexpr uint32_t kColAmberGlow     = 0xFFE8A428u;
constexpr uint32_t kColCrimsonGlow   = 0xFFE64858u;
constexpr uint32_t kColWarmWhite     = tokens::kSilkWarmWhite;
constexpr uint32_t kColMutedSilver   = tokens::kSilkMutedSilver;

[[nodiscard]] constexpr vf50::params::ParamIndex opParamIdx(int op0To3, std::size_t fieldOffset) noexcept {
    return static_cast<vf50::params::ParamIndex>(
        vf50::params::kOperatorsStart + static_cast<std::size_t>(op0To3) * vf50::params::kParamsPerOperator + fieldOffset);
}

[[nodiscard]] uint32_t accentToArgb(int accentTag) noexcept {
    switch (accentTag) {
        case 0:  return kColTealGlow;
        case 1:  return kColAmberGlow;
        case 2:  return kColCrimsonGlow;
        default: return kColWarmWhite;
    }
}

[[nodiscard]] float evalOpzWaveformPreview(int wave0To7, float phase0To1) noexcept {
    constexpr float kTwoPi = 6.28318530718f;
    const float p = phase0To1 - std::floor(phase0To1);
    const int quad = std::clamp(static_cast<int>(p * 4.0f), 0, 3);
    const float s  = std::sin(p * kTwoPi);
    const float s2 = std::sin(p * 2.0f * kTwoPi);

    switch (wave0To7 & 7) {
        case 0: // W1: Sine
            return s;
        case 1: // W2: Sin^2 (signed)
            return (s >= 0.0f) ? (s * s) : -(s * s);
        case 2: // W3: Half-Sine
            return (p < 0.5f) ? s : 0.0f;
        case 3: // W4: Half-Sin^2
            return (p < 0.5f) ? (s * s) : 0.0f;
        case 4: // W5: Alternating Sine (double freq in Q0/Q1, zero in Q2/Q3)
            return (p < 0.5f) ? s2 : 0.0f;
        case 5: // W6: Alternating Sin^2
            if (p >= 0.5f) return 0.0f;
            return (s2 >= 0.0f) ? (s2 * s2) : -(s2 * s2);
        case 6: // W7: Abs Half-Sine (Q0/Q1 positive, Q2/Q3 zero)
            return (quad < 2) ? std::abs(s2) : 0.0f;
        default: // W8: Abs Sin^2 camel-hump
            return (quad < 2) ? (s2 * s2) : 0.0f;
    }
}

} // namespace

// =============================================================================
// PgFaderLookAndFeel — Roland PG-800 / PG-1000 Calibrated Vertical Faders
// =============================================================================
void VF50ProgrammerDrawer::PgFaderLookAndFeel::drawLinearSlider(juce::Graphics& g,
                                                               int x, int y, int width, int height,
                                                               float sliderPos,
                                                               float /*minSliderPos*/,
                                                               float /*maxSliderPos*/,
                                                               juce::Slider::SliderStyle /*style*/,
                                                               juce::Slider& slider) {
    const auto bounds = juce::Rectangle<float>(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height));
    const float cx      = bounds.getCentreX();
    const float topY    = bounds.getY() + 10.0f;
    const float botY    = bounds.getBottom() - 10.0f;
    const float trackH  = std::max(16.0f, botY - topY);

    const int accentTag = static_cast<int>(slider.getProperties().getWithDefault("pgAccent", 0));
    const double cDet   = static_cast<double>(slider.getProperties().getWithDefault("pgCenter", -1000.0));
    const uint32_t glowArgb = accentToArgb(accentTag);
    const juce::Colour glowCol(glowArgb);

    const bool hasCenter = (cDet >= slider.getMinimum() && cDet <= slider.getMaximum());
    float centerY = botY;
    if (hasCenter && slider.getMaximum() > slider.getMinimum()) {
        const double frac = (cDet - slider.getMinimum()) / (slider.getMaximum() - slider.getMinimum());
        centerY = botY - static_cast<float>(frac) * trackH;
    }

    // 1. Calibrated Horizontal Tick Marks (11 graduations on both sides of slot)
    for (int t = 0; t <= 10; ++t) {
        const float ty = botY - (static_cast<float>(t) / 10.0f) * trackH;
        const bool isMajor = (t == 0 || t == 5 || t == 10);
        const bool isDetent = hasCenter && (std::abs(ty - centerY) < 2.5f);
        const float tickLen = isDetent ? 9.5f : (isMajor ? 7.5f : 4.5f);
        g.setColour(isDetent ? glowCol.withAlpha(0.85f)
                             : juce::Colour(isMajor ? 0xAA8E98A6u : 0x55687280u));
        g.drawLine(cx - 5.0f - tickLen, ty, cx - 5.0f, ty, isMajor || isDetent ? 1.6f : 1.1f);
        g.drawLine(cx + 5.0f, ty, cx + 5.0f + tickLen, ty, isMajor || isDetent ? 1.6f : 1.1f);
    }

    // 2. Recessed Dark Fader Track Slot
    const juce::Rectangle<float> slotRect(cx - 4.0f, topY - 4.0f, 8.0f, trackH + 8.0f);
    g.setColour(juce::Colour(0xFF07090Cu));
    g.fillRoundedRectangle(slotRect, 3.0f);
    g.setColour(juce::Colour(0xFF28303Au));
    g.drawRoundedRectangle(slotRect, 3.0f, 1.2f);

    // 3. Subtle Glowing Active Rail Fill
    const float clampedThumbY = std::clamp(sliderPos, topY, botY);
    if (hasCenter) {
        const float y0 = std::min(centerY, clampedThumbY);
        const float y1 = std::max(centerY, clampedThumbY);
        if (y1 - y0 > 1.0f) {
            g.setColour(glowCol.withAlpha(0.48f));
            g.fillRoundedRectangle(cx - 2.0f, y0, 4.0f, y1 - y0, 1.5f);
        }
    } else {
        const float fillH = botY - clampedThumbY;
        if (fillH > 1.0f) {
            g.setColour(glowCol.withAlpha(0.45f));
            g.fillRoundedRectangle(cx - 2.0f, clampedThumbY, 4.0f, fillH, 1.5f);
        }
    }

    // 4. Sculpted PG-Style Anodized Cap + Glowing Center Hairline
    const float capW = std::clamp(bounds.getWidth() - 8.0f, 20.0f, 34.0f);
    const float capH = 18.0f;
    const juce::Rectangle<float> capRect(cx - capW * 0.5f, clampedThumbY - capH * 0.5f, capW, capH);

    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRoundedRectangle(capRect.translated(0.0f, 2.5f), 3.0f);

    juce::ColourGradient capGrad(juce::Colour(0xFF3E4652u), capRect.getX(), capRect.getY(),
                                 juce::Colour(0xFF1F242Cu), capRect.getX(), capRect.getBottom(), false);
    capGrad.addColour(0.45, juce::Colour(0xFF2D343Eu));
    g.setGradientFill(capGrad);
    g.fillRoundedRectangle(capRect, 2.8f);

    g.setColour(juce::Colour(0xFF566070u));
    g.drawRoundedRectangle(capRect, 2.8f, 1.2f);

    // Subtle grip ridges above and below center hairline
    g.setColour(juce::Colour(0xFF14181Eu));
    g.drawLine(capRect.getX() + 3.0f, clampedThumbY - 4.5f, capRect.getRight() - 3.0f, clampedThumbY - 4.5f, 1.2f);
    g.drawLine(capRect.getX() + 3.0f, clampedThumbY + 4.5f, capRect.getRight() - 3.0f, clampedThumbY + 4.5f, 1.2f);

    // Glowing thumb hairline
    g.setColour(glowCol.withAlpha(0.32f));
    g.fillRect(capRect.getX() + 2.0f, clampedThumbY - 2.4f, capRect.getWidth() - 4.0f, 4.8f);
    g.setColour(glowCol);
    g.drawLine(capRect.getX() + 2.0f, clampedThumbY, capRect.getRight() - 2.0f, clampedThumbY, 2.4f);
}

// =============================================================================
// PgMiniToggleButton
// =============================================================================
VF50ProgrammerDrawer::PgMiniToggleButton::PgMiniToggleButton(const juce::String& name,
                                                            const juce::String& offLabel,
                                                            const juce::String& onLabel,
                                                            uint32_t accentArgb,
                                                            bool showLedDot)
    : juce::Button(name),
      offText_(offLabel),
      onText_(onLabel),
      accentArgb_(accentArgb),
      showLedDot_(showLedDot) {
    setClickingTogglesState(true);
}

void VF50ProgrammerDrawer::PgMiniToggleButton::paintButton(juce::Graphics& g, bool over, bool down) {
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    const bool state = getToggleState();
    const juce::Colour accent(accentArgb_);

    g.setColour(juce::Colour(down ? 0xFF12161Cu : (state ? 0xFF202832u : 0xFF171C22u)));
    g.fillRoundedRectangle(b, 4.0f);

    g.setColour(state ? accent.withAlpha(0.85f)
                      : juce::Colour(over ? 0xFF566070u : 0xFF343C48u));
    g.drawRoundedRectangle(b, 4.0f, state ? 1.8f : 1.3f);

    juce::Rectangle<float> textArea = b;
    if (showLedDot_) {
        const float ledR = 4.5f;
        const juce::Point<float> ledC(b.getX() + 11.0f, b.getCentreY());
        if (state) {
            g.setColour(accent.withAlpha(0.35f));
            g.fillEllipse(ledC.x - ledR * 1.7f, ledC.y - ledR * 1.7f, ledR * 3.4f, ledR * 3.4f);
            g.setColour(accent);
        } else {
            g.setColour(juce::Colour(0xFF2A3038u));
        }
        g.fillEllipse(ledC.x - ledR, ledC.y - ledR, ledR * 2.0f, ledR * 2.0f);
        textArea.removeFromLeft(18.0f);
    }

    g.setColour(state ? accent.brighter(0.25f) : juce::Colour(kColMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true));
    g.drawText(state ? onText_ : offText_, textArea.toNearestInt(), juce::Justification::centred, false);
}

// =============================================================================
// VF50ProgrammerDrawer Construction & Control Wiring
// =============================================================================
VF50ProgrammerDrawer::VF50ProgrammerDrawer(vf50::plugin::VF50AudioProcessor& processor)
    : processor_(processor) {
    setName("VF50ProgrammerDrawer");
    setOpaque(true);
    buildAllControls();
    syncFromProcessor();
}

VF50ProgrammerDrawer::~VF50ProgrammerDrawer() {
    for (auto& slot : faders_) {
        if (slot.slider != nullptr) {
            slot.slider->setLookAndFeel(nullptr);
        }
    }
}

void VF50ProgrammerDrawer::configureFader(std::size_t slotIdx,
                                         vf50::params::ParamIndex paramIdx,
                                         int opIdx,
                                         const char* shortLbl,
                                         const char* fullLbl,
                                         double minV,
                                         double maxV,
                                         double defV,
                                         double centerDetent,
                                         AccentTheme accent,
                                         bool wave1To8,
                                         bool voiceLvl) {
    auto& s = faders_[slotIdx];
    s.paramIndex = paramIdx;
    s.opIndex    = opIdx;
    s.shortLabel = shortLbl;
    s.fullName   = fullLbl;
    s.minVal     = minV;
    s.maxVal     = maxV;
    s.defaultVal = defV;
    s.centerVal  = centerDetent;
    s.accent     = accent;
    s.isWave1To8 = wave1To8;
    s.isVoiceLvl = voiceLvl;

    auto sl = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    sl->setName(juce::String("PG_Fader_") + juce::String(static_cast<int>(slotIdx)));
    sl->setLookAndFeel(&pgLookAndFeel_);
    sl->setRange(minV, maxV, 1.0);
    sl->setValue(defV, juce::dontSendNotification);
    sl->setDoubleClickReturnValue(true, defV);
    sl->getProperties().set("pgAccent", static_cast<int>(accent));
    sl->getProperties().set("pgCenter", centerDetent);
    sl->onValueChange = [this, slotIdx]() {
        handleFaderChanged(slotIdx);
    };
    addAndMakeVisible(*sl);
    s.slider = std::move(sl);
}

void VF50ProgrammerDrawer::buildAllControls() {
    using vf50::params::ParamIndex;

    // 1. Bay 1: Interactive Algorithm Selector Buttons 1..8
    for (int a = 0; a < 8; ++a) {
        auto btn = std::make_unique<juce::TextButton>(juce::String(a + 1));
        btn->setName("PG_Alg_" + juce::String(a + 1));
        btn->setClickingTogglesState(false);
        btn->onClick = [this, a]() {
            selectAlgorithmOneBased(a + 1, true);
        };
        addAndMakeVisible(*btn);
        algButtons_[static_cast<std::size_t>(a)] = std::move(btn);
    }

    // Bay 1 Faders (indices 0..2): FEEDBACK, VOICE LEVEL, TRANSPOSE
    configureFader(0u, ParamIndex::VoiceFeedback,   -1, "FB",    "FEEDBACK",    0.0,   7.0,  0.0, -1000.0, AccentTheme::Crimson);
    configureFader(1u, ParamIndex::QuickTotalLevel, -1, "LEVEL", "VOICE LEVEL", 0.0,  99.0, 80.0,    80.0, AccentTheme::Teal, false, true);
    configureFader(2u, ParamIndex::VoiceTranspose,  -1, "TRNS",  "TRANSPOSE", -24.0,  24.0,  0.0,     0.0, AccentTheme::Amber);

    // 2. Bays 2..5: Operators 1, 2, 3, 4 (4 x 12 = 48 vertical faders + 3 switches each)
    for (int op = 0; op < static_cast<int>(kOpCount); ++op) {
        auto& oc = opControls_[static_cast<std::size_t>(op)];
        const juce::String opTag = "OP" + juce::String(op + 1);

        oc.btnEnable = std::make_unique<PgMiniToggleButton>(
            opTag + "_ON", "MUTE", "ON", tokens::kLedGreenMid, true);
        oc.btnEnable->setToggleState(true, juce::dontSendNotification);
        oc.btnEnable->onClick = [this, op]() {
            if (suppressCallbacks_) return;
            const bool en = opControls_[static_cast<std::size_t>(op)].btnEnable->getToggleState();
            const auto pIdx = opParamIdx(op, 0u);
            vf50::params::setApvtsRawValue(processor_.apvts(), pIdx, en ? 1.0f : 0.0f);
            emitLcdJumpForParam(pIdx, op, "OP" + juce::String(op + 1) + " ENABLE", en ? "ON" : "MUTE");
            repaint();
        };
        addAndMakeVisible(*oc.btnEnable);

        oc.btnFreqMode = std::make_unique<PgMiniToggleButton>(
            opTag + "_FMODE", "RATIO", "FIXED", kColTealGlow, false);
        oc.btnFreqMode->onClick = [this, op]() {
            if (suppressCallbacks_) return;
            const bool fixed = opControls_[static_cast<std::size_t>(op)].btnFreqMode->getToggleState();
            const auto pIdx = opParamIdx(op, 2u);
            vf50::params::setApvtsRawValue(processor_.apvts(), pIdx, fixed ? 1.0f : 0.0f);
            emitLcdJumpForParam(pIdx, op, "OP" + juce::String(op + 1) + " FREQ MODE", fixed ? "FIXED HZ" : "RATIO");
            repaint();
        };
        addAndMakeVisible(*oc.btnFreqMode);

        oc.btnAme = std::make_unique<PgMiniToggleButton>(
            opTag + "_AME", "AME", "AME", kColAmberGlow, true);
        oc.btnAme->onClick = [this, op]() {
            if (suppressCallbacks_) return;
            const bool ame = opControls_[static_cast<std::size_t>(op)].btnAme->getToggleState();
            const auto pIdx = opParamIdx(op, 19u);
            vf50::params::setApvtsRawValue(processor_.apvts(), pIdx, ame ? 1.0f : 0.0f);
            emitLcdJumpForParam(pIdx, op, "OP" + juce::String(op + 1) + " AMP MOD", ame ? "ON" : "OFF");
            repaint();
        };
        addAndMakeVisible(*oc.btnAme);

        const std::size_t base = kGlobalFaderCount + static_cast<std::size_t>(op) * kFadersPerOp;
        configureFader(base +  0u, opParamIdx(op,  1u), op, "WAVE", "WAVEFORM",     1.0,  8.0,  1.0, -1000.0, AccentTheme::Teal, true);
        configureFader(base +  1u, opParamIdx(op,  5u), op, "CRS",  "COARSE FREQ",  0.0, 63.0,  1.0, -1000.0, AccentTheme::Teal);
        configureFader(base +  2u, opParamIdx(op,  6u), op, "FINE", "FINE FREQ",    0.0, 15.0,  0.0, -1000.0, AccentTheme::Teal);
        configureFader(base +  3u, opParamIdx(op,  7u), op, "DET",  "DETUNE",      -3.0,  3.0,  0.0,     0.0, AccentTheme::Teal);
        configureFader(base +  4u, opParamIdx(op,  8u), op, "AR",   "ATTACK RATE",  0.0, 31.0, 31.0, -1000.0, AccentTheme::Amber);
        configureFader(base +  5u, opParamIdx(op,  9u), op, "D1R",  "DECAY 1 RATE", 0.0, 31.0,  0.0, -1000.0, AccentTheme::Amber);
        configureFader(base +  6u, opParamIdx(op, 10u), op, "D1L",  "DECAY 1 LVL",  0.0, 15.0, 15.0, -1000.0, AccentTheme::Amber);
        configureFader(base +  7u, opParamIdx(op, 11u), op, "D2R",  "DECAY 2 RATE", 0.0, 31.0,  0.0, -1000.0, AccentTheme::Amber);
        configureFader(base +  8u, opParamIdx(op, 12u), op, "RR",   "RELEASE RATE", 1.0, 15.0,  7.0, -1000.0, AccentTheme::Amber);
        configureFader(base +  9u, opParamIdx(op, 14u), op, "OUT",  "OUTPUT LEVEL", 0.0, 99.0, 90.0, -1000.0, AccentTheme::Crimson);
        configureFader(base + 10u, opParamIdx(op, 15u), op, "RS",   "RATE SCALING", 0.0,  3.0,  0.0, -1000.0, AccentTheme::Crimson);
        configureFader(base + 11u, opParamIdx(op, 17u), op, "KVS",  "KEY VEL SENS", 0.0,  7.0,  0.0, -1000.0, AccentTheme::Crimson);
    }

    // 3. Bay 6: Pitch Envelope Generator (PEG) & LFO
    static constexpr std::array<const char*, 4> kLfoWaveShort = { "SAW", "SQR", "TRI", "S&H" };
    static constexpr std::array<const char*, 4> kLfoWaveFull  = { "SAW UP", "SQUARE", "TRIANGLE", "S&HOLD" };
    for (int w = 0; w < 4; ++w) {
        auto lb = std::make_unique<juce::TextButton>(kLfoWaveShort[static_cast<std::size_t>(w)]);
        lb->setName("PG_LfoWave_" + juce::String(w));
        lb->onClick = [this, w]() {
            if (suppressCallbacks_) return;
            currentLfoWave_ = w;
            vf50::params::setApvtsRawValue(processor_.apvts(), ParamIndex::Lfo1Wave, static_cast<float>(w));
            emitLcdJumpForParam(ParamIndex::Lfo1Wave, -1, "LFO WAVE", kLfoWaveFull[static_cast<std::size_t>(w)]);
            syncFromProcessor();
            repaint();
        };
        addAndMakeVisible(*lb);
        lfoWaveButtons_[static_cast<std::size_t>(w)] = std::move(lb);
    }

    btnLfoSync_ = std::make_unique<PgMiniToggleButton>("PG_LfoSync", "SYNC", "SYNC", kColCrimsonGlow, true);
    btnLfoSync_->onClick = [this]() {
        if (suppressCallbacks_) return;
        const bool syncOn = btnLfoSync_->getToggleState();
        vf50::params::setApvtsRawValue(processor_.apvts(), ParamIndex::Lfo1Sync, syncOn ? 1.0f : 0.0f);
        emitLcdJumpForParam(ParamIndex::Lfo1Sync, -1, "LFO KEY SYNC", syncOn ? "ON" : "OFF");
        repaint();
    };
    addAndMakeVisible(*btnLfoSync_);

    const std::size_t pegBase = kGlobalFaderCount + kOpCount * kFadersPerOp; // 51
    configureFader(pegBase +  0u, ParamIndex::PegPr1,    -1, "PR1", "PEG RATE 1",   0.0, 99.0, 99.0, -1000.0, AccentTheme::Amber);
    configureFader(pegBase +  1u, ParamIndex::PegPr2,    -1, "PR2", "PEG RATE 2",   0.0, 99.0, 99.0, -1000.0, AccentTheme::Amber);
    configureFader(pegBase +  2u, ParamIndex::PegPr3,    -1, "PR3", "PEG RATE 3",   0.0, 99.0, 99.0, -1000.0, AccentTheme::Amber);
    configureFader(pegBase +  3u, ParamIndex::PegPl1,    -1, "PL1", "PEG LEVEL 1",  0.0, 99.0, 50.0,    50.0, AccentTheme::Amber);
    configureFader(pegBase +  4u, ParamIndex::PegPl2,    -1, "PL2", "PEG LEVEL 2",  0.0, 99.0, 50.0,    50.0, AccentTheme::Amber);
    configureFader(pegBase +  5u, ParamIndex::PegPl3,    -1, "PL3", "PEG LEVEL 3",  0.0, 99.0, 50.0,    50.0, AccentTheme::Amber);
    configureFader(pegBase +  6u, ParamIndex::Lfo1Speed, -1, "SPD", "LFO SPEED",    0.0, 99.0, 35.0, -1000.0, AccentTheme::Crimson);
    configureFader(pegBase +  7u, ParamIndex::Lfo1Delay, -1, "DLY", "LFO DELAY",    0.0, 99.0,  0.0, -1000.0, AccentTheme::Crimson);
    configureFader(pegBase +  8u, ParamIndex::Lfo1Pmd,   -1, "PMD", "PITCH MOD DP", 0.0, 99.0,  0.0, -1000.0, AccentTheme::Crimson);
    configureFader(pegBase +  9u, ParamIndex::Lfo1Amd,   -1, "AMD", "AMP MOD DP",   0.0, 99.0,  0.0, -1000.0, AccentTheme::Crimson);
    configureFader(pegBase + 10u, ParamIndex::Lfo1Pms,   -1, "PMS", "PITCH SENS",   0.0,  7.0,  2.0, -1000.0, AccentTheme::Teal);
    configureFader(pegBase + 11u, ParamIndex::Lfo1Ams,   -1, "AMS", "AMP SENS",     0.0,  3.0,  0.0, -1000.0, AccentTheme::Teal);
}

juce::String VF50ProgrammerDrawer::formatFaderReadout(const FaderSlot& slot, double val) {
    const int iv = static_cast<int>(std::lround(val));
    if (slot.isWave1To8) {
        return "W" + juce::String(std::clamp(iv, 1, 8));
    }
    if (slot.paramIndex == vf50::params::ParamIndex::VoiceTranspose ||
        (slot.centerVal == 0.0 && slot.minVal < 0.0)) {
        if (iv > 0) return "+" + juce::String(iv);
        return juce::String(iv);
    }
    return juce::String(iv);
}

void VF50ProgrammerDrawer::handleFaderChanged(std::size_t slotIdx) {
    if (suppressCallbacks_ || slotIdx >= faders_.size()) {
        return;
    }

    const auto& slot = faders_[slotIdx];
    if (slot.slider == nullptr) {
        return;
    }

    const double uiVal = slot.slider->getValue();
    float rawApvtsVal  = static_cast<float>(uiVal);

    if (slot.isWave1To8) {
        rawApvtsVal = static_cast<float>(std::clamp(static_cast<int>(std::lround(uiVal)) - 1, 0, 7));
    } else if (slot.isVoiceLvl) {
        const int lvl0To99 = std::clamp(static_cast<int>(std::lround(uiVal)), 0, 99);
        rawApvtsVal = (lvl0To99 == 0) ? -99.0f : static_cast<float>(lvl0To99 - 80);
    }

    vf50::params::setApvtsRawValue(processor_.apvts(), slot.paramIndex, rawApvtsVal);

    const juce::String prefix = (slot.opIndex >= 0)
                                    ? ("OP" + juce::String(slot.opIndex + 1) + " " + slot.fullName)
                                    : juce::String(slot.fullName);
    const juce::String valStr = formatFaderReadout(slot, uiVal);
    emitLcdJumpForParam(slot.paramIndex, slot.opIndex, prefix, valStr);

    repaint(slot.readoutRect.expanded(4));
    if (slot.isWave1To8 && slot.opIndex >= 0 && slot.opIndex < 4) {
        repaint(opControls_[static_cast<std::size_t>(slot.opIndex)].waveBoxRect.expanded(4));
    }
    if (slotIdx >= (kGlobalFaderCount + kOpCount * kFadersPerOp) &&
        slotIdx < (kGlobalFaderCount + kOpCount * kFadersPerOp + 6u)) {
        repaint(pegPreviewRect_.expanded(4));
    }
}

void VF50ProgrammerDrawer::selectAlgorithmOneBased(int alg1To8, bool notify) {
    const int alg0 = std::clamp(alg1To8 - 1, 0, 7);
    currentAlgZeroBased_ = alg0;

    for (int a = 0; a < 8; ++a) {
        if (auto* btn = algButtons_[static_cast<std::size_t>(a)].get()) {
            const bool sel = (a == alg0);
            btn->setColour(juce::TextButton::buttonColourId,
                           juce::Colour(sel ? 0xFF1F4B54u : 0xFF1A1F26u));
            btn->setColour(juce::TextButton::textColourOffId,
                           juce::Colour(sel ? kColTealGlow : kColMutedSilver));
        }
    }

    if (notify && !suppressCallbacks_) {
        vf50::params::setApvtsRawValue(
            processor_.apvts(), vf50::params::ParamIndex::VoiceAlgorithm, static_cast<float>(alg0));
        emitLcdJumpForParam(
            vf50::params::ParamIndex::VoiceAlgorithm, -1, "ALGORITHM", juce::String(alg0 + 1));
    }
    repaint();
}

void VF50ProgrammerDrawer::emitLcdJumpForParam(vf50::params::ParamIndex paramIdx,
                                              int opIdx,
                                              const juce::String& fullParamName,
                                              const juce::String& formattedVal) {
    if (!onParameterAdjusted) {
        return;
    }
    const auto& vc = processor_.synthEngine().fm().activeSingleVoice();
    const juce::String patchName = juce::String(vc.name.data(), vc.name.size()).trimEnd();
    const juce::String line0 = "PG-EDIT [" + patchName + "] -> " + fullParamName;
    const juce::String line1 = fullParamName + " = " + formattedVal;
    onParameterAdjusted(paramIdx, opIdx, line0, line1);
}

void VF50ProgrammerDrawer::syncFromProcessor() {
    const auto& pBank = processor_.parameterBank();
    suppressCallbacks_ = true;

    const int alg0 = std::clamp(static_cast<int>(std::lround(pBank.voiceAlgorithm->load())), 0, 7);
    if (alg0 != currentAlgZeroBased_) {
        currentAlgZeroBased_ = alg0;
    }
    for (int a = 0; a < 8; ++a) {
        if (auto* btn = algButtons_[static_cast<std::size_t>(a)].get()) {
            const bool sel = (a == alg0);
            btn->setColour(juce::TextButton::buttonColourId,
                           juce::Colour(sel ? 0xFF1F4B54u : 0xFF1A1F26u));
            btn->setColour(juce::TextButton::textColourOffId,
                           juce::Colour(sel ? kColTealGlow : kColMutedSilver));
        }
    }

    for (std::size_t op = 0u; op < kOpCount; ++op) {
        auto& oc = opControls_[op];
        const auto& opAtomics = pBank.ops[op];
        if (oc.btnEnable != nullptr) {
            oc.btnEnable->setToggleState(opAtomics.enable->load() >= 0.5f, juce::dontSendNotification);
        }
        if (oc.btnFreqMode != nullptr) {
            oc.btnFreqMode->setToggleState(opAtomics.fixedMode->load() >= 0.5f, juce::dontSendNotification);
        }
        if (oc.btnAme != nullptr) {
            oc.btnAme->setToggleState(opAtomics.ampModEnable->load() >= 0.5f, juce::dontSendNotification);
        }
    }

    const int lfoW = std::clamp(static_cast<int>(std::lround(pBank.lfo1Wave->load())), 0, 3);
    currentLfoWave_ = lfoW;
    for (int w = 0; w < 4; ++w) {
        if (auto* lb = lfoWaveButtons_[static_cast<std::size_t>(w)].get()) {
            const bool sel = (w == lfoW);
            lb->setColour(juce::TextButton::buttonColourId,
                          juce::Colour(sel ? 0xFF4C1F28u : 0xFF1A1F26u));
            lb->setColour(juce::TextButton::textColourOffId,
                          juce::Colour(sel ? kColCrimsonGlow : kColMutedSilver));
        }
    }
    if (btnLfoSync_ != nullptr) {
        btnLfoSync_->setToggleState(pBank.lfo1Sync->load() >= 0.5f, juce::dontSendNotification);
    }

    for (auto& slot : faders_) {
        if (slot.slider == nullptr) {
            continue;
        }
        const auto* atomicPtr = pBank.byIndex[static_cast<std::size_t>(slot.paramIndex)];
        if (atomicPtr == nullptr) {
            continue;
        }
        const float raw = atomicPtr->load(std::memory_order_relaxed);
        double uiVal    = static_cast<double>(raw);
        if (slot.isWave1To8) {
            uiVal = static_cast<double>(std::clamp(static_cast<int>(std::lround(raw)) + 1, 1, 8));
        } else if (slot.isVoiceLvl) {
            const int qeVol = static_cast<int>(std::lround(raw));
            uiVal = (qeVol <= -99) ? 0.0 : static_cast<double>(std::clamp(80 + qeVol, 0, 99));
        } else {
            uiVal = std::clamp(std::round(uiVal), slot.minVal, slot.maxVal);
        }
        if (std::abs(slot.slider->getValue() - uiVal) > 0.01) {
            slot.slider->setValue(uiVal, juce::dontSendNotification);
        }
    }

    suppressCallbacks_ = false;
    repaint();
}

bool VF50ProgrammerDrawer::setFaderValueForParam(vf50::params::ParamIndex paramIndex,
                                                double displayValue,
                                                bool notify) {
    for (std::size_t i = 0u; i < faders_.size(); ++i) {
        if (faders_[i].paramIndex == paramIndex && faders_[i].slider != nullptr) {
            faders_[i].slider->setValue(
                std::clamp(displayValue, faders_[i].minVal, faders_[i].maxVal),
                juce::dontSendNotification);
            if (notify) {
                handleFaderChanged(i);
            } else {
                repaint();
            }
            return true;
        }
    }
    return false;
}

double VF50ProgrammerDrawer::getFaderValueForParam(vf50::params::ParamIndex paramIndex) const noexcept {
    for (const auto& slot : faders_) {
        if (slot.paramIndex == paramIndex && slot.slider != nullptr) {
            return slot.slider->getValue();
        }
    }
    return 0.0;
}

void VF50ProgrammerDrawer::resized() {
    // Layout across 2964 x 680 @ 2x virtual coordinates:
    // Bay 1 (Global & Alg):  x=16,   w=346
    // Bays 2..5 (OP 1..4):   x=370 + op*500, w=492 (370..862, 870..1362, 1370..1862, 1870..2362)
    // Bay 6 (PEG & LFO):     x=2370, w=578
    const int bayY = 14;
    const int bayH = getHeight() - 24; // 656 @ 2x

    bay1Rect_ = juce::Rectangle<int>(16, bayY, 346, bayH);
    bay6Rect_ = juce::Rectangle<int>(2370, bayY, 578, bayH);

    // 1. Bay 1: 8 Algorithm buttons (2 rows of 4 or 1 row of 8) + Live Vector Diagram + 3 Global Sliders
    for (int a = 0; a < 8; ++a) {
        algButtons_[static_cast<std::size_t>(a)]->setBounds(bay1Rect_.getX() + 13 + a * 40, bayY + 42, 37, 32);
    }
    algDiagramRect_ = juce::Rectangle<int>(bay1Rect_.getX() + 14, bayY + 82, bay1Rect_.getWidth() - 28, 198);

    const int gFaderTop = bayY + 318;
    const int gFaderH   = bayH - 364;
    const int gStepX    = (bay1Rect_.getWidth() - 24) / 3;
    for (std::size_t i = 0u; i < kGlobalFaderCount; ++i) {
        const int fx = bay1Rect_.getX() + 12 + static_cast<int>(i) * gStepX;
        auto& slot   = faders_[i];
        slot.labelRect   = juce::Rectangle<int>(fx, gFaderTop - 28, gStepX, 24);
        slot.slider->setBounds(fx + (gStepX - 44) / 2, gFaderTop, 44, gFaderH);
        slot.readoutRect = juce::Rectangle<int>(fx, gFaderTop + gFaderH + 2, gStepX, 26);
    }

    // 2. Bays 2..5: Operators 1, 2, 3, 4 (12 vertical sliders each)
    const int opFaderTop = bayY + 138;
    const int opFaderH   = bayH - 184;
    for (int op = 0; op < static_cast<int>(kOpCount); ++op) {
        auto& oc = opControls_[static_cast<std::size_t>(op)];
        oc.bayBounds = juce::Rectangle<int>(370 + op * 500, bayY, 492, bayH);
        const int bx = oc.bayBounds.getX();

        oc.btnEnable->setBounds(bx + 12, bayY + 42, 92, 36);
        oc.btnFreqMode->setBounds(bx + 112, bayY + 42, 96, 36);
        oc.btnAme->setBounds(bx + 216, bayY + 42, 86, 36);
        oc.waveBoxRect = juce::Rectangle<int>(bx + 312, bayY + 38, 166, 44);

        const int stepX = 40; // 12 * 40 = 480 px inside 492 px bay
        const std::size_t base = kGlobalFaderCount + static_cast<std::size_t>(op) * kFadersPerOp;
        for (std::size_t f = 0u; f < kFadersPerOp; ++f) {
            const int fx = bx + 6 + static_cast<int>(f) * stepX;
            auto& slot   = faders_[base + f];
            slot.labelRect   = juce::Rectangle<int>(fx - 2, opFaderTop - 28, stepX + 4, 24);
            slot.slider->setBounds(fx + 2, opFaderTop, stepX - 4, opFaderH);
            slot.readoutRect = juce::Rectangle<int>(fx - 2, opFaderTop + opFaderH + 2, stepX + 4, 26);
        }
    }

    // 3. Bay 6: PEG & LFO (12 vertical sliders: 6 PEG + 6 LFO)
    const int b6x = bay6Rect_.getX();
    pegPreviewRect_ = juce::Rectangle<int>(b6x + 14, bayY + 38, 256, 44);

    for (int w = 0; w < 4; ++w) {
        lfoWaveButtons_[static_cast<std::size_t>(w)]->setBounds(b6x + 286 + w * 50, bayY + 42, 46, 36);
    }
    btnLfoSync_->setBounds(b6x + 490, bayY + 42, 76, 36);

    const std::size_t pegBase = kGlobalFaderCount + kOpCount * kFadersPerOp;
    const int b6StepX = 47; // 12 * 47 = 564 px inside 578 px bay
    for (std::size_t f = 0u; f < kPegLfoFaderCount; ++f) {
        const int fx = b6x + 7 + static_cast<int>(f) * b6StepX;
        auto& slot   = faders_[pegBase + f];
        slot.labelRect   = juce::Rectangle<int>(fx - 2, opFaderTop - 28, b6StepX + 4, 24);
        slot.slider->setBounds(fx + 3, opFaderTop, b6StepX - 6, opFaderH);
        slot.readoutRect = juce::Rectangle<int>(fx - 2, opFaderTop + opFaderH + 2, b6StepX + 4, 26);
    }
}

void VF50ProgrammerDrawer::mouseDown(const juce::MouseEvent& event) {
    if (algDiagramRect_.contains(event.getPosition())) {
        const int delta = event.mods.isRightButtonDown() ? -1 : 1;
        const int nextAlg1 = ((currentAlgZeroBased_ + 8 + delta) % 8) + 1;
        selectAlgorithmOneBased(nextAlg1, true);
    }
}

bool VF50ProgrammerDrawer::isOperatorCarrier(int algZeroBased, int opZeroBased) noexcept {
    // 4-Op (VF50 / TX81Z / DX11) Carrier Mask per Algorithm 1..8 (OP1=bit0 .. OP4=bit3):
    // Alg 1..4: OP1 (0x01)
    // Alg 5:    OP1, OP3 (0x05)
    // Alg 6..7: OP1, OP2, OP3 (0x07)
    // Alg 8:    OP1, OP2, OP3, OP4 (0x0F)
    static constexpr std::array<uint8_t, 8> kCarrierMasks = {
        0x01u, 0x01u, 0x01u, 0x01u, 0x05u, 0x07u, 0x07u, 0x0Fu
    };
    const uint8_t mask = kCarrierMasks[static_cast<std::size_t>(std::clamp(algZeroBased, 0, 7))];
    return ((mask >> std::clamp(opZeroBased, 0, 3)) & 1u) != 0u;
}

// =============================================================================
// Procedural 2x Vector Rendering (PG-Style Anodized Chassis & Live Diagrams)
// =============================================================================
void VF50ProgrammerDrawer::paint(juce::Graphics& g) {
    paintDrawerChassis(g);

    // Bay 1: Global & Algorithm
    paintBayFrame(g, bay1Rect_, "GLOBAL & ALGORITHM", kColTealGlow,
                  "ALG " + juce::String(currentAlgZeroBased_ + 1), kColTealGlow);
    paintAlgorithmDiagram(g, algDiagramRect_.toFloat(), currentAlgZeroBased_);

    // Bays 2..5: Operators 1..4
    for (int op = 0; op < static_cast<int>(kOpCount); ++op) {
        const auto& oc = opControls_[static_cast<std::size_t>(op)];
        const bool isCar = isOperatorCarrier(currentAlgZeroBased_, op);
        paintBayFrame(g,
                      oc.bayBounds,
                      "OPERATOR " + juce::String(op + 1),
                      isCar ? kColTealGlow : kColAmberGlow,
                      isCar ? "CARRIER" : "MODULATOR",
                      isCar ? kColTealGlow : kColAmberGlow);

        // Sub-section PG color brackets above faders (OSC = Teal, EG = Amber, OUT/SCALE = Crimson)
        const int bx = oc.bayBounds.getX() + 6;
        const int brY = oc.bayBounds.getY() + 96;
        auto drawSubBracket = [&](int x0, int w, const char* txt, uint32_t col) {
            g.setColour(juce::Colour(col));
            g.drawLine(static_cast<float>(x0 + 2), static_cast<float>(brY + 10),
                       static_cast<float>(x0 + w - 2), static_cast<float>(brY + 10), 1.8f);
            g.setFont(VF50LookAndFeel::makeFont(12.0f, true));
            g.drawText(txt, x0, brY - 8, w, 16, juce::Justification::centred, false);
        };
        drawSubBracket(bx +   0, 160, "OSCILLATOR",       kColTealGlow);
        drawSubBracket(bx + 160, 200, "ENVELOPE (OPZ II)", kColAmberGlow);
        drawSubBracket(bx + 360, 120, "LEVEL / SENS",     kColCrimsonGlow);

        const std::size_t waveSlotIdx = kGlobalFaderCount + static_cast<std::size_t>(op) * kFadersPerOp;
        const int waveZero = std::clamp(static_cast<int>(std::lround(faders_[waveSlotIdx].slider->getValue())) - 1, 0, 7);
        const bool opEn    = oc.btnEnable != nullptr && oc.btnEnable->getToggleState();
        paintOpzWaveformIcon(g, oc.waveBoxRect.toFloat(), waveZero, opEn);
    }

    // Bay 6: PEG & LFO
    paintBayFrame(g, bay6Rect_, "PITCH ENVELOPE (PEG)  &  MULTI-WAVE LFO", kColAmberGlow, "PG-VF50", kColCrimsonGlow);
    {
        const int b6x = bay6Rect_.getX() + 7;
        const int brY = bay6Rect_.getY() + 96;
        g.setColour(juce::Colour(kColAmberGlow));
        g.drawLine(static_cast<float>(b6x + 2), static_cast<float>(brY + 10),
                   static_cast<float>(b6x + 278), static_cast<float>(brY + 10), 1.8f);
        g.setFont(VF50LookAndFeel::makeFont(12.0f, true));
        g.drawText("PITCH ENVELOPE (RATES / LEVELS)", b6x, brY - 8, 280, 16, juce::Justification::centred, false);

        g.setColour(juce::Colour(kColCrimsonGlow));
        g.drawLine(static_cast<float>(b6x + 284), static_cast<float>(brY + 10),
                   static_cast<float>(b6x + 560), static_cast<float>(brY + 10), 1.8f);
        g.drawText("LOW FREQUENCY OSCILLATOR", b6x + 282, brY - 8, 280, 16, juce::Justification::centred, false);
    }
    paintPegPreview(g, pegPreviewRect_.toFloat());

    // Labels & Live Numeric Readout Pills for all 63 Vertical Sliders
    for (const auto& slot : faders_) {
        if (slot.slider == nullptr) {
            continue;
        }
        g.setColour(juce::Colour(kColWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(13.0f, true));
        g.drawText(slot.shortLabel, slot.labelRect, juce::Justification::centred, false);

        const auto pill = slot.readoutRect.toFloat().reduced(3.0f, 2.0f);
        g.setColour(juce::Colour(0xFF0A0D11u));
        g.fillRoundedRectangle(pill, 3.5f);
        g.setColour(juce::Colour(0xFF272E38u));
        g.drawRoundedRectangle(pill, 3.5f, 1.0f);

        const uint32_t valCol = accentToArgb(static_cast<int>(slot.accent));
        g.setColour(juce::Colour(valCol).brighter(0.18f));
        g.setFont(VF50LookAndFeel::makeFont(13.5f, true));
        g.drawText(formatFaderReadout(slot, slot.slider->getValue()),
                   slot.readoutRect, juce::Justification::centred, false);
    }
}

void VF50ProgrammerDrawer::paintDrawerChassis(juce::Graphics& g) const {
    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());

    juce::ColourGradient bgGrad(juce::Colour(kColPgBgTop), 0.0f, 0.0f,
                                juce::Colour(kColPgBgBot), 0.0f, h, false);
    g.setGradientFill(bgGrad);
    g.fillRect(0.0f, 0.0f, w, h);

    // Subtle horizontal brushed-metal lines
    g.setColour(juce::Colours::white.withAlpha(0.014f));
    for (int y = 6; y < getHeight(); y += 6) {
        g.fillRect(0.0f, static_cast<float>(y), w, 1.0f);
    }

    // Recessed hinge groove separating main VF50 deck from PG Programmer Drawer
    g.setColour(juce::Colour(0xFF06080Au));
    g.fillRect(0.0f, 0.0f, w, 5.0f);
    g.setColour(juce::Colour(0xFF39414Du));
    g.fillRect(0.0f, 5.0f, w, 2.0f);
    g.setColour(juce::Colour(0xFF080A0Du));
    g.fillRect(0.0f, h - 3.0f, w, 3.0f);
}

void VF50ProgrammerDrawer::paintBayFrame(juce::Graphics& g,
                                        const juce::Rectangle<int>& bounds,
                                        const juce::String& title,
                                        uint32_t bracketArgb,
                                        const juce::String& rightBadge,
                                        uint32_t badgeArgb) const {
    const auto bf = bounds.toFloat();
    g.setColour(juce::Colour(kColBayFill));
    g.fillRoundedRectangle(bf, 7.0f);

    g.setColour(juce::Colour(kColBayBorder));
    g.drawRoundedRectangle(bf, 7.0f, 1.6f);

    // PG-800 style top color-coded header bar
    const juce::Rectangle<float> hdr(bf.getX() + 2.0f, bf.getY() + 2.0f, bf.getWidth() - 4.0f, 30.0f);
    g.setColour(juce::Colour(bracketArgb).withAlpha(0.16f));
    g.fillRoundedRectangle(hdr, 5.0f);
    g.setColour(juce::Colour(bracketArgb));
    g.drawLine(bf.getX() + 8.0f, bf.getY() + 32.0f, bf.getRight() - 8.0f, bf.getY() + 32.0f, 2.2f);

    g.setColour(juce::Colour(kColWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(15.0f, true, 1.04f));
    g.drawText(title, bounds.getX() + 12, bounds.getY() + 4, bounds.getWidth() - 24, 26,
               juce::Justification::centredLeft, false);

    if (rightBadge.isNotEmpty()) {
        const juce::Rectangle<float> bRect(bf.getRight() - 104.0f, bf.getY() + 6.0f, 92.0f, 20.0f);
        g.setColour(juce::Colour(badgeArgb).withAlpha(0.22f));
        g.fillRoundedRectangle(bRect, 3.5f);
        g.setColour(juce::Colour(badgeArgb));
        g.drawRoundedRectangle(bRect, 3.5f, 1.2f);
        g.setFont(VF50LookAndFeel::makeFont(12.5f, true));
        g.drawText(rightBadge, bRect.toNearestInt(), juce::Justification::centred, false);
    }
}

void VF50ProgrammerDrawer::paintAlgorithmDiagram(juce::Graphics& g,
                                                const juce::Rectangle<float>& area,
                                                int algZeroBased) const {
    g.setColour(juce::Colour(0xFF0B0E12u));
    g.fillRoundedRectangle(area, 5.0f);
    g.setColour(juce::Colour(0xFF28313Du));
    g.drawRoundedRectangle(area, 5.0f, 1.4f);

    // Normalized node coordinates (x, y in 0..1) for OP1, OP2, OP3, OP4 across Algorithms 1..8
    struct Pt { float x; float y; };
    static constexpr std::array<std::array<Pt, 4>, 8> kNodePos = {{
        // ALG 1: 4 -> 3 -> 2 -> 1 (vertical / stepped stack)
        {{ { 0.78f, 0.76f }, { 0.56f, 0.58f }, { 0.36f, 0.40f }, { 0.18f, 0.22f } }},
        // ALG 2: (4 + 3) -> 2 -> 1
        {{ { 0.78f, 0.62f }, { 0.50f, 0.62f }, { 0.22f, 0.76f }, { 0.22f, 0.28f } }},
        // ALG 3: 3 -> 2 -> 1, plus 4 -> 1
        {{ { 0.78f, 0.62f }, { 0.50f, 0.76f }, { 0.22f, 0.76f }, { 0.44f, 0.25f } }},
        // ALG 4: 4 -> 3 -> 1, plus 2 -> 1
        {{ { 0.78f, 0.62f }, { 0.48f, 0.78f }, { 0.50f, 0.30f }, { 0.20f, 0.30f } }},
        // ALG 5: (4 -> 3*) + (2 -> 1*)
        {{ { 0.68f, 0.74f }, { 0.68f, 0.28f }, { 0.28f, 0.74f }, { 0.28f, 0.28f } }},
        // ALG 6: 4 -> (3* + 2* + 1*)
        {{ { 0.78f, 0.76f }, { 0.50f, 0.76f }, { 0.22f, 0.76f }, { 0.50f, 0.24f } }},
        // ALG 7: (4 -> 3*) + 2* + 1*
        {{ { 0.78f, 0.76f }, { 0.50f, 0.76f }, { 0.22f, 0.76f }, { 0.22f, 0.24f } }},
        // ALG 8: 4* + 3* + 2* + 1* (4 parallel carriers)
        {{ { 0.82f, 0.62f }, { 0.61f, 0.62f }, { 0.39f, 0.62f }, { 0.18f, 0.62f } }}
    }};

    struct Edge { int fromOp; int toOp; }; // 1-based OP numbers
    static constexpr std::array<std::array<Edge, 4>, 8> kEdges = {{
        {{ {4,3}, {3,2}, {2,1}, {0,0} }},
        {{ {4,2}, {3,2}, {2,1}, {0,0} }},
        {{ {3,2}, {2,1}, {4,1}, {0,0} }},
        {{ {4,3}, {3,1}, {2,1}, {0,0} }},
        {{ {4,3}, {2,1}, {0,0}, {0,0} }},
        {{ {4,3}, {4,2}, {4,1}, {0,0} }},
        {{ {4,3}, {0,0}, {0,0}, {0,0} }},
        {{ {0,0}, {0,0}, {0,0}, {0,0} }}
    }};

    const int alg = std::clamp(algZeroBased, 0, 7);
    auto toScreen = [&](Pt p) -> juce::Point<float> {
        return { area.getX() + 16.0f + p.x * (area.getWidth() - 32.0f),
                 area.getY() + 14.0f + p.y * (area.getHeight() - 34.0f) };
    };

    // 1. Draw modulation edges
    g.setColour(juce::Colour(kColAmberGlow).withAlpha(0.88f));
    for (const auto& e : kEdges[static_cast<std::size_t>(alg)]) {
        if (e.fromOp >= 1 && e.toOp >= 1) {
            const auto p0 = toScreen(kNodePos[static_cast<std::size_t>(alg)][static_cast<std::size_t>(e.fromOp - 1)]);
            const auto p1 = toScreen(kNodePos[static_cast<std::size_t>(alg)][static_cast<std::size_t>(e.toOp - 1)]);
            g.drawArrow(juce::Line<float>(p0, p1), 2.4f, 8.0f, 8.0f);
        }
    }

    // 2. Draw Crimson OP4 Self-Feedback Loop
    {
        const auto p4 = toScreen(kNodePos[static_cast<std::size_t>(alg)][3]);
        const juce::Rectangle<float> fbLoop(p4.x - 22.0f, p4.y - 30.0f, 44.0f, 22.0f);
        g.setColour(juce::Colour(kColCrimsonGlow));
        g.drawRoundedRectangle(fbLoop, 6.0f, 2.0f);
    }

    // 3. Carrier Output Bus Line along bottom
    const float outBusY = area.getBottom() - 12.0f;
    g.setColour(juce::Colour(kColTealGlow).withAlpha(0.65f));
    g.drawLine(area.getX() + 22.0f, outBusY, area.getRight() - 22.0f, outBusY, 2.0f);

    // 4. Draw Operator 1..4 Nodes
    const float boxW = 46.0f;
    const float boxH = 30.0f;
    for (int op = 0; op < 4; ++op) {
        const auto c    = toScreen(kNodePos[static_cast<std::size_t>(alg)][static_cast<std::size_t>(op)]);
        const bool isC  = isOperatorCarrier(alg, op);
        const bool opEn = opControls_[static_cast<std::size_t>(op)].btnEnable == nullptr
                       || opControls_[static_cast<std::size_t>(op)].btnEnable->getToggleState();

        if (isC) {
            g.setColour(juce::Colour(kColTealGlow).withAlpha(0.75f));
            g.drawLine(c.x, c.y + boxH * 0.5f, c.x, outBusY, 2.0f);
        }

        const juce::Rectangle<float> r(c.x - boxW * 0.5f, c.y - boxH * 0.5f, boxW, boxH);
        const juce::Colour nodeCol(isC ? kColTealGlow : kColAmberGlow);
        g.setColour(opEn ? nodeCol.withAlpha(0.24f) : juce::Colour(0xFF14181Eu));
        g.fillRoundedRectangle(r, 4.5f);
        g.setColour(opEn ? nodeCol : juce::Colour(0xFF3E4652u));
        g.drawRoundedRectangle(r, 4.5f, 2.0f);

        g.setColour(opEn ? juce::Colour(kColWarmWhite) : juce::Colour(0xFF66707Cu));
        g.setFont(VF50LookAndFeel::makeFont(14.5f, true));
        g.drawText("OP" + juce::String(op + 1), r.toNearestInt(), juce::Justification::centred, false);
    }

    g.setColour(juce::Colour(kColMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(11.5f, true));
    g.drawText("CLICK DIAGRAM OR 1-8 TO SELECT",
               static_cast<int>(area.getX() + 8.0f), static_cast<int>(area.getY() + 3.0f),
               static_cast<int>(area.getWidth() - 16.0f), 16,
               juce::Justification::topRight, false);
}

void VF50ProgrammerDrawer::paintOpzWaveformIcon(juce::Graphics& g,
                                               const juce::Rectangle<float>& area,
                                               int waveZeroBased,
                                               bool enabled) const {
    g.setColour(juce::Colour(0xFF0A0D11u));
    g.fillRoundedRectangle(area, 4.5f);
    g.setColour(juce::Colour(0xFF2A3340u));
    g.drawRoundedRectangle(area, 4.5f, 1.3f);

    // Zero-line axis
    const float midY = area.getCentreY() + 2.0f;
    g.setColour(juce::Colour(0x44586676u));
    g.drawLine(area.getX() + 6.0f, midY, area.getRight() - 42.0f, midY, 1.0f);

    juce::Path wavePath;
    const float x0 = area.getX() + 8.0f;
    const float x1 = area.getRight() - 46.0f;
    const float amp = (area.getHeight() * 0.5f) - 7.0f;
    constexpr int kSamples = 48;
    for (int i = 0; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const float yVal = evalOpzWaveformPreview(waveZeroBased, t);
        const float px = x0 + t * (x1 - x0);
        const float py = midY - yVal * amp;
        if (i == 0) wavePath.startNewSubPath(px, py);
        else wavePath.lineTo(px, py);
    }

    const juce::Colour traceCol = enabled ? juce::Colour(kColTealGlow) : juce::Colour(0xFF55606Eu);
    g.setColour(traceCol);
    g.strokePath(wavePath, juce::PathStrokeType(2.2f));

    g.setColour(traceCol.brighter(0.2f));
    g.setFont(VF50LookAndFeel::makeFont(14.5f, true));
    g.drawText("W" + juce::String(std::clamp(waveZeroBased + 1, 1, 8)),
               static_cast<int>(area.getRight() - 42.0f), static_cast<int>(area.getY()),
               38, static_cast<int>(area.getHeight()),
               juce::Justification::centred, false);
}

void VF50ProgrammerDrawer::paintPegPreview(juce::Graphics& g, const juce::Rectangle<float>& area) const {
    g.setColour(juce::Colour(0xFF0A0D11u));
    g.fillRoundedRectangle(area, 4.5f);
    g.setColour(juce::Colour(0xFF2A3340u));
    g.drawRoundedRectangle(area, 4.5f, 1.3f);

    const float midY = area.getCentreY();
    g.setColour(juce::Colour(0x44786848u));
    g.drawLine(area.getX() + 6.0f, midY, area.getRight() - 6.0f, midY, 1.0f);

    const std::size_t pegBase = kGlobalFaderCount + kOpCount * kFadersPerOp;
    const float pl1 = static_cast<float>(faders_[pegBase + 3u].slider->getValue() - 50.0) / 50.0f;
    const float pl2 = static_cast<float>(faders_[pegBase + 4u].slider->getValue() - 50.0) / 50.0f;
    const float pl3 = static_cast<float>(faders_[pegBase + 5u].slider->getValue() - 50.0) / 50.0f;

    const float x0  = area.getX() + 10.0f;
    const float w   = area.getWidth() - 20.0f;
    const float amp = (area.getHeight() * 0.5f) - 6.0f;

    juce::Path p;
    p.startNewSubPath(x0, midY - pl1 * amp);
    p.lineTo(x0 + w * 0.33f, midY - pl2 * amp);
    p.lineTo(x0 + w * 0.68f, midY - pl3 * amp);
    p.lineTo(x0 + w, midY);

    g.setColour(juce::Colour(kColAmberGlow));
    g.strokePath(p, juce::PathStrokeType(2.2f));
}

} // namespace vf50::gui
