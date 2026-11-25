#pragma once

#include "GUI/VF50DesignTokens.h"
#include "GUI/VF50LookAndFeel.h"
#include "Plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <memory>

namespace vf50::gui {

// =============================================================================
// 2x Vector 61-Key (C1..C6) Velocity-Sensitive VF50 Virtual Keyboard
// Styled after UI References/REAL hardware_full_keyboard.png
// =============================================================================
class VF50VirtualKeyboard final : public juce::Component {
public:
    static constexpr int kNumKeys      = 61; // C1..C6 (MIDI 36..96 at octaveShift == 0)
    static constexpr int kNumWhiteKeys = 36; // 5 octaves * 7 + top C6
    static constexpr int kBaseMidiNote = 36; // Hardware C1 = MIDI 36

    explicit VF50VirtualKeyboard(vf50::plugin::VF50AudioProcessor& processor);
    ~VF50VirtualKeyboard() override = default;

    [[nodiscard]] int octaveShift() const noexcept { return octaveShift_; }
    void setOctaveShift(int shiftNeg1ToPos1) noexcept;

    [[nodiscard]] int lowestMidiNote() const noexcept { return kBaseMidiNote + octaveShift_ * 12; }
    [[nodiscard]] int highestMidiNote() const noexcept { return lowestMidiNote() + (kNumKeys - 1); }

    [[nodiscard]] bool isKeyVisuallyLit(int midiNote) const noexcept;
    [[nodiscard]] uint8_t lastMouseVelocity() const noexcept { return lastVelocity7Bit_; }

    // Programmatic mouse-key simulation helpers for automated headless verification
    void simulateMouseKeyDown(int midiNote, float verticalFraction0To1 = 0.78f);
    void simulateMouseKeyUp();

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;

private:
    struct KeyGeometry {
        int midiOffset = 0; // 0..60
        bool isBlack   = false;
        juce::Rectangle<float> bounds{};
    };

    void rebuildKeyGeometries() noexcept;
    [[nodiscard]] int hitTestKeyAtPoint(juce::Point<float> pt, float& outVelocity0To1) const noexcept;
    [[nodiscard]] int resolveActiveMidiChannel1To16() const noexcept;
    void triggerMouseNote(int midiNote, float velocity0To1);
    void releaseMouseNote();

    vf50::plugin::VF50AudioProcessor& processor_;

    std::unique_ptr<VF50PanelButton> btnOctDown_;
    std::unique_ptr<VF50PanelButton> btnOctUp_;

    std::array<KeyGeometry, kNumKeys> keys_{};
    int octaveShift_          = 0;  // -1 (C0..C5), 0 (C1..C6 default), +1 (C2..C7)
    int activeMouseNote_      = -1; // Currently held mouse MIDI note, or -1
    int activeMouseChannel_   = 1;  // 1..16
    int hoveredMidiOffset_    = -1;
    uint8_t lastVelocity7Bit_ = 100u;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50VirtualKeyboard)
};

} // namespace vf50::gui
