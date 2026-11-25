#pragma once

#include "GUI/VF50DesignTokens.h"
#include "GUI/VF50MainCanvas.h"
#include "Plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vf50::plugin {

class VF50AudioProcessorEditor final : public juce::AudioProcessorEditor,
                                      public juce::FileDragAndDropTarget,
                                      public juce::DragAndDropContainer {
public:
    explicit VF50AudioProcessorEditor(VF50AudioProcessor& processor);
    ~VF50AudioProcessorEditor() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // User-facing UI scaling: 75% (0.75f), 100% Default 1x (1.00f = 1482x440), 125%, 150%, 200% Native 2x (2964x880)
    void applyScaleFactor(float scaleFactor);
    [[nodiscard]] float currentScaleFactor() const noexcept { return mainCanvas_.currentScaleFactor(); }

    void setProgrammerVisible(bool visible);
    [[nodiscard]] bool isProgrammerVisible() const noexcept { return mainCanvas_.isProgrammerVisible(); }

    void setMixerVisible(bool visible);
    [[nodiscard]] bool isMixerVisible() const noexcept { return mainCanvas_.isMixerVisible(); }

    void setVirtualKeyboardVisible(bool visible);
    [[nodiscard]] bool isVirtualKeyboardVisible() const noexcept { return mainCanvas_.isVirtualKeyboardVisible(); }

    [[nodiscard]] vf50::gui::VF50MainCanvas& mainCanvas() noexcept { return mainCanvas_; }
    [[nodiscard]] const vf50::gui::VF50MainCanvas& mainCanvas() const noexcept { return mainCanvas_; }
    [[nodiscard]] bool isDragHighlightActive() const noexcept { return isDragActive_; }
    [[nodiscard]] bool demoLedState() const noexcept { return mainCanvas_.classicController().demoLedState(); }

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

private:
    void applyLayoutMetricsAndSize(float userScale, bool programmerVisible, bool mixerVisible, bool keyboardVisible);

    VF50AudioProcessor& processorRef_;
    vf50::gui::VF50MainCanvas mainCanvas_;
    juce::TooltipWindow tooltipWindow_{ this, 550 };
    bool isDragActive_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50AudioProcessorEditor)
};

} // namespace vf50::plugin
