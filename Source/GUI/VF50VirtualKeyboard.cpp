#include "GUI/VF50VirtualKeyboard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vf50::gui {

namespace {

constexpr float kLeftCheekWidth2x  = 340.0f;
constexpr float kRightCheekWidth2x = 48.0f;
constexpr float kKeybedTopY2x      = 22.0f;
constexpr float kWhiteKeyHeight2x  = 302.0f;
constexpr float kBlackKeyHeight2x  = 192.0f;

[[nodiscard]] constexpr bool isBlackSemitone(int semitoneInOctave) noexcept {
    return semitoneInOctave == 1 || semitoneInOctave == 3 ||
           semitoneInOctave == 6 || semitoneInOctave == 8 || semitoneInOctave == 10;
}

// Returns white-key index (0..6) to the left of a black semitone, and key offset ratio
[[nodiscard]] float blackKeyCenterWhiteOffset(int semitoneInOctave) noexcept {
    switch (semitoneInOctave) {
        case 1:  return 0.94f; // C# sits slightly left of C|D seam
        case 3:  return 2.06f; // D# sits slightly right of D|E seam
        case 6:  return 3.92f; // F# sits slightly left of F|G seam
        case 8:  return 5.00f; // G# centered on G|A seam
        case 10: return 6.08f; // A# sits slightly right of A|B seam
        default: return 0.0f;
    }
}

} // namespace

VF50VirtualKeyboard::VF50VirtualKeyboard(vf50::plugin::VF50AudioProcessor& processor)
    : processor_(processor) {
    setOpaque(true);

    btnOctDown_ = std::make_unique<VF50PanelButton>("Keybd_OctDown");
    btnOctDown_->setCapLegends("OCT -", tokens::kSilkWarmWhite);
    btnOctDown_->setFontScale(0.86f);
    btnOctDown_->onClick = [this]() { setOctaveShift(octaveShift_ - 1); };
    addAndMakeVisible(*btnOctDown_);

    btnOctUp_ = std::make_unique<VF50PanelButton>("Keybd_OctUp");
    btnOctUp_->setCapLegends("OCT +", tokens::kSilkAccentTeal);
    btnOctUp_->setFontScale(0.86f);
    btnOctUp_->onClick = [this]() { setOctaveShift(octaveShift_ + 1); };
    addAndMakeVisible(*btnOctUp_);

    rebuildKeyGeometries();
}

void VF50VirtualKeyboard::setOctaveShift(int shiftNeg1ToPos1) noexcept {
    const int clamped = std::clamp(shiftNeg1ToPos1, -1, 1);
    if (clamped != octaveShift_) {
        releaseMouseNote();
        octaveShift_ = clamped;
        repaint();
    }
}

bool VF50VirtualKeyboard::isKeyVisuallyLit(int midiNote) const noexcept {
    if (midiNote < 0 || midiNote > 127) {
        return false;
    }
    if (processor_.keyboardState().isNoteOnForChannels(0xFFFF, midiNote)) {
        return true;
    }
    if (midiNote >= 36 && midiNote <= 96) {
        const uint64_t demoMask = processor_.demoVisualKeyMask61();
        if ((demoMask & (1ULL << static_cast<uint32_t>(midiNote - 36))) != 0ULL) {
            return true;
        }
    }
    return false;
}

int VF50VirtualKeyboard::resolveActiveMidiChannel1To16() const noexcept {
    const auto ws = processor_.activeWorkstationMode();
    const auto& pBank = processor_.parameterBank();

    if (ws == vf50::state::WorkstationMode::RhythmPlay) {
        const int rCh = static_cast<int>(std::lround(pBank.rhythmReceiveCh->load(std::memory_order_relaxed)));
        if (rCh >= 0 && rCh < 16) {
            return rCh + 1;
        }
        return 10;
    }

    if (processor_.synthEngine().playMode() == vf50::dsp::fm::SynthPlayMode::Performance) {
        for (std::size_t p = 0; p < 8u; ++p) {
            if (pBank.parts[p].maxNotes->load(std::memory_order_relaxed) >= 0.5f) {
                const int rc = static_cast<int>(std::lround(pBank.parts[p].receiveCh->load(std::memory_order_relaxed)));
                if (rc >= 0 && rc < 16) {
                    return rc + 1;
                }
                if (rc == 16) {
                    return 1; // OMNI
                }
            }
        }
        return 1;
    }

    const int sCh = static_cast<int>(std::lround(pBank.singleReceiveCh->load(std::memory_order_relaxed)));
    if (sCh >= 0 && sCh < 16) {
        return sCh + 1;
    }
    return 1;
}

void VF50VirtualKeyboard::rebuildKeyGeometries() noexcept {
    const float totalW = (getWidth() > 0) ? static_cast<float>(getWidth()) : static_cast<float>(tokens::kVirtualWidth);
    const float keybedX = kLeftCheekWidth2x;
    const float keybedW = std::max(360.0f, totalW - kLeftCheekWidth2x - kRightCheekWidth2x);
    const float whiteW  = keybedW / static_cast<float>(kNumWhiteKeys);
    const float blackW  = whiteW * 0.58f;

    int whiteCounter = 0;
    for (int offset = 0; offset < kNumKeys; ++offset) {
        const int oct = offset / 12;
        const int semi = offset % 12;
        auto& kg = keys_[static_cast<std::size_t>(offset)];
        kg.midiOffset = offset;
        kg.isBlack    = isBlackSemitone(semi);

        if (!kg.isBlack) {
            const float x0 = keybedX + static_cast<float>(whiteCounter) * whiteW;
            const float x1 = keybedX + static_cast<float>(whiteCounter + 1) * whiteW;
            kg.bounds = juce::Rectangle<float>(x0, kKeybedTopY2x, x1 - x0, kWhiteKeyHeight2x);
            ++whiteCounter;
        } else {
            const float octaveWhiteBase = static_cast<float>(oct * 7);
            const float centerWhite = octaveWhiteBase + blackKeyCenterWhiteOffset(semi);
            const float cx = keybedX + centerWhite * whiteW;
            kg.bounds = juce::Rectangle<float>(cx - blackW * 0.5f, kKeybedTopY2x, blackW, kBlackKeyHeight2x);
        }
    }
}

int VF50VirtualKeyboard::hitTestKeyAtPoint(juce::Point<float> pt, float& outVelocity0To1) const noexcept {
    // 1. Test sculpted black keys first (they sit physically above white keys)
    for (const auto& kg : keys_) {
        if (kg.isBlack && kg.bounds.contains(pt)) {
            const float relY = (pt.y - kg.bounds.getY()) / std::max(1.0f, kg.bounds.getHeight());
            outVelocity0To1 = std::clamp(0.18f + 0.82f * relY, 0.12f, 1.0f);
            return kg.midiOffset;
        }
    }

    // 2. Test white keys
    for (const auto& kg : keys_) {
        if (!kg.isBlack && kg.bounds.contains(pt)) {
            const float relY = (pt.y - kg.bounds.getY()) / std::max(1.0f, kg.bounds.getHeight());
            outVelocity0To1 = std::clamp(0.16f + 0.84f * relY, 0.12f, 1.0f);
            return kg.midiOffset;
        }
    }

    return -1;
}

void VF50VirtualKeyboard::triggerMouseNote(int midiNote, float velocity0To1) {
    const int clampedNote = std::clamp(midiNote, 0, 127);
    const float clampedVel = std::clamp(velocity0To1, 0.05f, 1.0f);
    if (activeMouseNote_ == clampedNote) {
        return;
    }
    releaseMouseNote();

    activeMouseChannel_ = resolveActiveMidiChannel1To16();
    activeMouseNote_    = clampedNote;
    lastVelocity7Bit_   = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(clampedVel * 127.0f)), 1, 127));
    processor_.keyboardState().noteOn(activeMouseChannel_, activeMouseNote_, clampedVel);
    repaint();
}

void VF50VirtualKeyboard::releaseMouseNote() {
    if (activeMouseNote_ >= 0) {
        processor_.keyboardState().noteOff(activeMouseChannel_, activeMouseNote_, 0.0f);
        activeMouseNote_ = -1;
        repaint();
    }
}

void VF50VirtualKeyboard::simulateMouseKeyDown(int midiNote, float verticalFraction0To1) {
    const float vel = std::clamp(0.16f + 0.84f * std::clamp(verticalFraction0To1, 0.0f, 1.0f), 0.12f, 1.0f);
    triggerMouseNote(midiNote, vel);
}

void VF50VirtualKeyboard::simulateMouseKeyUp() {
    releaseMouseNote();
}

void VF50VirtualKeyboard::resized() {
    rebuildKeyGeometries();
    if (btnOctDown_ != nullptr) {
        btnOctDown_->setBounds(66, 98, 100, 74);
    }
    if (btnOctUp_ != nullptr) {
        btnOctUp_->setBounds(182, 98, 100, 74);
    }
}

void VF50VirtualKeyboard::mouseDown(const juce::MouseEvent& event) {
    float vel = 0.80f;
    const int offset = hitTestKeyAtPoint(event.position, vel);
    hoveredMidiOffset_ = offset;
    if (offset >= 0) {
        triggerMouseNote(lowestMidiNote() + offset, vel);
    }
}

void VF50VirtualKeyboard::mouseDrag(const juce::MouseEvent& event) {
    float vel = 0.80f;
    const int offset = hitTestKeyAtPoint(event.position, vel);
    hoveredMidiOffset_ = offset;
    if (offset >= 0) {
        const int targetNote = lowestMidiNote() + offset;
        if (targetNote != activeMouseNote_) {
            triggerMouseNote(targetNote, vel);
        }
    } else {
        releaseMouseNote();
    }
}

void VF50VirtualKeyboard::mouseUp(const juce::MouseEvent& /*event*/) {
    hoveredMidiOffset_ = -1;
    releaseMouseNote();
}

void VF50VirtualKeyboard::mouseExit(const juce::MouseEvent& /*event*/) {
    if (hoveredMidiOffset_ >= 0) {
        hoveredMidiOffset_ = -1;
        repaint();
    }
}

void VF50VirtualKeyboard::paint(juce::Graphics& g) {
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);

    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());
    const float keybedX = kLeftCheekWidth2x;
    const float keybedRight = w - kRightCheekWidth2x;
    const float keybedW = std::max(360.0f, keybedRight - keybedX);

    // 1. Anodized Charcoal-Grey Chassis Bed & Left/Right End Cheeks
    {
        juce::ColourGradient chassisGrad(juce::Colour(0xFF23282Eu), 0.0f, 0.0f,
                                         juce::Colour(0xFF14171Bu), 0.0f, h, false);
        g.setGradientFill(chassisGrad);
        g.fillRect(0.0f, 0.0f, w, h);

        // Top chassis step shadow separating front control panel from keybed assembly
        g.setColour(juce::Colour(0xFF07090Bu));
        g.fillRect(0.0f, 0.0f, w, 6.0f);
        g.setColour(juce::Colour(0xFF363D46u));
        g.fillRect(0.0f, 6.0f, w, 2.0f);
    }

    // 2. Left Hardware Cheek Block Control Bay (Octave Shift + Velocity Bar)
    {
        const juce::Rectangle<float> cheekPanel(44.0f, 24.0f, 280.0f, h - 40.0f);
        g.setColour(juce::Colour(tokens::kPanelSubFrameBg));
        g.fillRoundedRectangle(cheekPanel, 6.0f);
        g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
        g.drawRoundedRectangle(cheekPanel, 6.0f, 1.8f);

        const int lowNote  = lowestMidiNote();
        const int highNote = highestMidiNote();
        const int lowOct   = (lowNote / 12) - 2;  // Hardware C1 = 36 -> (3-2) = 1
        const int highOct  = (highNote / 12) - 2; // Hardware C6 = 96 -> (8-2) = 6

        const juce::String rangeStr = "61-KEY  [C" + juce::String(lowOct) + " - C" + juce::String(highOct) + "]";
        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(16.0f, true, 0.98f));
        g.drawText(rangeStr, 54, 34, 260, 24, juce::Justification::centred, false);

        const bool rhythmMode = (processor_.activeWorkstationMode() == vf50::state::WorkstationMode::RhythmPlay);
        g.setColour(juce::Colour(rhythmMode ? tokens::kSilkPlayGreen : tokens::kSilkAccentTeal));
        g.setFont(VF50LookAndFeel::makeFont(13.5f, true));
        g.drawText(rhythmMode ? "MODE: RHYTHM DRUM KIT" : "MODE: VELOCITY SYNTH",
                   54, 58, 260, 20, juce::Justification::centred, false);

        // Octave Shift Hardware LEDs
        VF50LookAndFeel::drawHardwareLed(g, { 116.0f, 86.0f }, 5.5f, false, octaveShift_ < 0);
        VF50LookAndFeel::drawHardwareLed(g, { 232.0f, 86.0f }, 5.5f, false, octaveShift_ > 0);

        // Velocity Meter Bar
        const juce::Rectangle<float> velWell(66.0f, 214.0f, 216.0f, 22.0f);
        g.setColour(juce::Colour(0xFF0A0C0Fu));
        g.fillRoundedRectangle(velWell, 4.0f);
        g.setColour(juce::Colour(0xFF2D333Bu));
        g.drawRoundedRectangle(velWell, 4.0f, 1.2f);

        const float velNorm = std::clamp(static_cast<float>(lastVelocity7Bit_) / 127.0f, 0.0f, 1.0f);
        const juce::Rectangle<float> velFill(velWell.getX() + 3.0f,
                                             velWell.getY() + 3.0f,
                                             (velWell.getWidth() - 6.0f) * velNorm,
                                             velWell.getHeight() - 6.0f);
        juce::ColourGradient velGrad(juce::Colour(tokens::kSilkAccentTeal), velFill.getX(), velFill.getY(),
                                     juce::Colour(tokens::kSilkPlayGreen), velFill.getRight(), velFill.getY(), false);
        g.setGradientFill(velGrad);
        g.fillRoundedRectangle(velFill, 2.5f);

        char velText[32]{};
        std::snprintf(velText, sizeof(velText), "KEY VELOCITY: %3u", static_cast<unsigned>(lastVelocity7Bit_));
        g.setColour(juce::Colour(tokens::kSilkMutedSilver));
        g.setFont(VF50LookAndFeel::makeFont(13.5f, true));
        g.drawText(velText, 66, 190, 216, 20, juce::Justification::centred, false);

        g.setColour(juce::Colour(tokens::kSilkDimLabel));
        g.setFont(VF50LookAndFeel::makeFont(12.5f, true));
        g.drawText("VERTICAL HIT = VELOCITY", 54, 248, 260, 18, juce::Justification::centred, false);
        if (rhythmMode && octaveShift_ == 0) {
            g.setColour(juce::Colour(tokens::kSilkOchreGold));
            g.drawText("OCT- FOR G0..B0 DRUMS", 54, 268, 260, 18, juce::Justification::centred, false);
        }
    }

    // 3. Recessed Keybed Well & Authentic Crimson Dust Felt Strip
    {
        const juce::Rectangle<float> wellRect(keybedX - 4.0f, 10.0f, keybedW + 8.0f, kWhiteKeyHeight2x + 20.0f);
        g.setColour(juce::Colour(0xFF060709u));
        g.fillRoundedRectangle(wellRect, 3.0f);

        // Deep burgundy/crimson hardware keybed felt strip above key pivots
        juce::ColourGradient feltGrad(juce::Colour(0xFF5A1116u), keybedX, 12.0f,
                                      juce::Colour(0xFF8C1B22u), keybedX, kKeybedTopY2x, false);
        g.setGradientFill(feltGrad);
        g.fillRect(keybedX, 12.0f, keybedW, kKeybedTopY2x - 12.0f);
    }

    const int baseMidi = lowestMidiNote();
    const bool rhythmMode = (processor_.activeWorkstationMode() == vf50::state::WorkstationMode::RhythmPlay);

    // 4. Draw 36 Sculpted Ivory-White Keys (Sub-pixel float coordinates, zero rounding gaps)
    for (const auto& kg : keys_) {
        if (kg.isBlack) {
            continue;
        }
        const int midiNote = baseMidi + kg.midiOffset;
        const bool lit     = isKeyVisuallyLit(midiNote);
        const auto kb      = kg.bounds.reduced(0.85f, 0.0f);

        const float pressShift = lit ? 3.0f : 0.0f;
        const juce::Rectangle<float> faceRect(kb.getX(), kb.getY() + pressShift,
                                              kb.getWidth(), kb.getHeight() - pressShift);

        if (lit) {
            juce::ColourGradient litGrad(juce::Colour(0xFFB5F2ECu), faceRect.getX(), faceRect.getY(),
                                         juce::Colour(0xFF66DCD0u), faceRect.getX(), faceRect.getBottom(), false);
            litGrad.addColour(0.45, juce::Colour(0xFF94EAE1u));
            g.setGradientFill(litGrad);
        } else {
            juce::ColourGradient ivoryGrad(juce::Colour(0xFFF6F7F4u), faceRect.getX(), faceRect.getY(),
                                           juce::Colour(0xFFE0E3DFu), faceRect.getX(), faceRect.getBottom() - 12.0f, false);
            ivoryGrad.addColour(0.72, juce::Colour(0xFFECEEEBu));
            g.setGradientFill(ivoryGrad);
        }
        g.fillRoundedRectangle(faceRect, 3.0f);

        // Front 3D hardware white-key lip overhang
        const juce::Rectangle<float> frontLip(faceRect.getX(), faceRect.getBottom() - 11.0f,
                                              faceRect.getWidth(), 11.0f);
        g.setColour(lit ? juce::Colour(0xFF28A89Au) : juce::Colour(0xFFC5CAC5u));
        g.fillRoundedRectangle(frontLip, 2.5f);

        // Subtle side key bevel highlight & seam
        g.setColour(juce::Colours::white.withAlpha(lit ? 0.55f : 0.75f));
        g.drawLine(faceRect.getX() + 1.0f, faceRect.getY() + 2.0f,
                   faceRect.getX() + 1.0f, faceRect.getBottom() - 12.0f, 1.1f);

        // Active LED bar near bottom of pressed white key
        if (lit) {
            g.setColour(juce::Colour(0xFF08665Cu));
            g.fillRoundedRectangle(faceRect.getX() + 8.0f, faceRect.getBottom() - 24.0f,
                                   faceRect.getWidth() - 16.0f, 6.0f, 2.5f);
        }

        // C-Octave root legends (C1..C6) + optional Rhythm Mode drum index hint
        if ((midiNote % 12) == 0) {
            const int octNum = (midiNote / 12) - 2;
            const juce::String cLbl = "C" + juce::String(octNum);
            g.setColour(lit ? juce::Colour(0xFF0A2826u) : juce::Colour(0xFF565E68u));
            g.setFont(VF50LookAndFeel::makeFont(14.0f, true));
            g.drawText(cLbl,
                       juce::Rectangle<float>(faceRect.getX(), faceRect.getBottom() - 38.0f, faceRect.getWidth(), 22.0f),
                       juce::Justification::centred,
                       false);
        } else if (rhythmMode && midiNote >= 31 && midiNote <= 91) {
            g.setColour(juce::Colour(0xFF7D8690u).withAlpha(0.75f));
            g.setFont(VF50LookAndFeel::makeFont(11.0f, true));
            g.drawText(juce::String(midiNote),
                       juce::Rectangle<float>(faceRect.getX(), faceRect.getBottom() - 35.0f, faceRect.getWidth(), 18.0f),
                       juce::Justification::centred,
                       false);
        }
    }

    // Cast top overhang shadow from chassis lip onto top of white keys
    {
        juce::ColourGradient lipShadow(juce::Colours::black.withAlpha(0.52f), keybedX, kKeybedTopY2x,
                                       juce::Colours::transparentBlack, keybedX, kKeybedTopY2x + 18.0f, false);
        g.setGradientFill(lipShadow);
        g.fillRect(keybedX, kKeybedTopY2x, keybedW, 18.0f);
    }

    // 5. Draw 25 Sculpted 3D Satin-Ebony Black Keys
    for (const auto& kg : keys_) {
        if (!kg.isBlack) {
            continue;
        }
        const int midiNote = baseMidi + kg.midiOffset;
        const bool lit     = isKeyVisuallyLit(midiNote);
        const auto bb      = kg.bounds;

        // Cast drop shadow onto adjacent white keys
        if (!lit) {
            g.setColour(juce::Colours::black.withAlpha(0.38f));
            g.fillRoundedRectangle(bb.translated(3.0f, 5.0f), 3.5f);
        }

        // Outer black key skirt
        g.setColour(juce::Colour(0xFF0B0D10u));
        g.fillRoundedRectangle(bb, 3.0f);

        // Raised sculpted top deck
        const float topDeckInsetX = 5.0f;
        const float noseSlopeH    = lit ? 14.0f : 22.0f;
        const juce::Rectangle<float> topDeck(bb.getX() + topDeckInsetX,
                                             bb.getY() + (lit ? 2.0f : 0.0f),
                                             bb.getWidth() - topDeckInsetX * 2.0f,
                                             bb.getHeight() - noseSlopeH);

        if (lit) {
            juce::ColourGradient litBlackGrad(juce::Colour(0xFF00D2BEu), topDeck.getX(), topDeck.getY(),
                                              juce::Colour(0xFF0D6E66u), topDeck.getX(), topDeck.getBottom(), false);
            g.setGradientFill(litBlackGrad);
            g.fillRoundedRectangle(topDeck, 2.0f);
        } else {
            juce::ColourGradient ebonyGrad(juce::Colour(0xFF323840u), topDeck.getX(), topDeck.getY(),
                                           juce::Colour(0xFF1A1E24u), topDeck.getX(), topDeck.getBottom(), false);
            ebonyGrad.addColour(0.30, juce::Colour(0xFF252A31u));
            g.setGradientFill(ebonyGrad);
            g.fillRoundedRectangle(topDeck, 2.0f);
        }

        // Front angled nose bevel of the black key
        juce::Path nosePath;
        nosePath.startNewSubPath(topDeck.getX(), topDeck.getBottom());
        nosePath.lineTo(topDeck.getRight(), topDeck.getBottom());
        nosePath.lineTo(bb.getRight() - 1.5f, bb.getBottom() - 1.5f);
        nosePath.lineTo(bb.getX() + 1.5f, bb.getBottom() - 1.5f);
        nosePath.closeSubPath();

        g.setColour(lit ? juce::Colour(0xFF42F5E3u) : juce::Colour(0xFF3E4652u));
        g.fillPath(nosePath);

        // Specular edge highlight along black-key nose crease
        g.setColour(juce::Colours::white.withAlpha(lit ? 0.65f : 0.28f));
        g.drawLine(topDeck.getX() + 1.0f, topDeck.getBottom(),
                   topDeck.getRight() - 1.0f, topDeck.getBottom(), 1.3f);
    }

    // 6. Right End-Cheek Bevel Highlight
    g.setColour(juce::Colour(0xFF363D46u));
    g.drawLine(keybedRight + 2.0f, 8.0f, keybedRight + 2.0f, h - 6.0f, 1.5f);
}

} // namespace vf50::gui
