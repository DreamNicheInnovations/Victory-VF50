#include "Plugin/PluginEditor.h"
#include "Disk/VF50DiskManager.h"

#include <algorithm>
#include <cmath>

namespace vf50::plugin {

VF50AudioProcessorEditor::VF50AudioProcessorEditor(VF50AudioProcessor& processor)
    : AudioProcessorEditor(&processor),
      processorRef_(processor),
      mainCanvas_(processor) {
    setLookAndFeel(&mainCanvas_.lookAndFeel());
    addAndMakeVisible(mainCanvas_);
    tooltipWindow_.setMillisecondsBeforeTipAppears(550);

    mainCanvas_.onRequestScaleChange = [this](float requestedScale) {
        applyScaleFactor(requestedScale);
    };

    mainCanvas_.onRequestProgrammerVisibilityChange = [this](bool visible) {
        setProgrammerVisible(visible);
    };

    mainCanvas_.onRequestMixerVisibilityChange = [this](bool visible) {
        setMixerVisible(visible);
    };

    mainCanvas_.onRequestKeyboardVisibilityChange = [this](bool visible) {
        setVirtualKeyboardVisible(visible);
    };

    setResizable(true, true);

    // Restore persisted user UI scale (default 100% = 1.00f -> 1482x440 1x desktop), PG/Mixer Drawer & Virtual Keyboard state
    const bool initPg = processorRef_.isProgrammerVisible();
    const bool initMx = processorRef_.isMixerVisible() && !initPg;
    applyLayoutMetricsAndSize(processorRef_.uiScaleFactor(),
                              initPg,
                              initMx,
                              processorRef_.isVirtualKeyboardVisible());
}

VF50AudioProcessorEditor::~VF50AudioProcessorEditor() {
    setLookAndFeel(nullptr);
}

void VF50AudioProcessorEditor::applyScaleFactor(float scaleFactor) {
    applyLayoutMetricsAndSize(scaleFactor,
                              mainCanvas_.isProgrammerVisible(),
                              mainCanvas_.isMixerVisible(),
                              mainCanvas_.isVirtualKeyboardVisible());
}

void VF50AudioProcessorEditor::setProgrammerVisible(bool visible) {
    applyLayoutMetricsAndSize(mainCanvas_.currentScaleFactor(),
                              visible,
                              visible ? false : mainCanvas_.isMixerVisible(),
                              mainCanvas_.isVirtualKeyboardVisible());
}

void VF50AudioProcessorEditor::setMixerVisible(bool visible) {
    applyLayoutMetricsAndSize(mainCanvas_.currentScaleFactor(),
                              visible ? false : mainCanvas_.isProgrammerVisible(),
                              visible,
                              mainCanvas_.isVirtualKeyboardVisible());
}

void VF50AudioProcessorEditor::setVirtualKeyboardVisible(bool visible) {
    applyLayoutMetricsAndSize(mainCanvas_.currentScaleFactor(),
                              mainCanvas_.isProgrammerVisible(),
                              mainCanvas_.isMixerVisible(),
                              visible);
}

void VF50AudioProcessorEditor::applyLayoutMetricsAndSize(float userScale,
                                                        bool programmerVisible,
                                                        bool mixerVisible,
                                                        bool keyboardVisible) {
    if (mixerVisible) {
        programmerVisible = false;
    }
    const float clampedScale = std::clamp(userScale,
                                          vf50::gui::tokens::kMinScaleFactor,
                                          vf50::gui::tokens::kMaxScaleFactor);
    processorRef_.setUiScaleFactor(clampedScale);
    if (mixerVisible) {
        processorRef_.setProgrammerVisible(false);
        processorRef_.setMixerVisible(true);
    } else {
        processorRef_.setMixerVisible(false);
        processorRef_.setProgrammerVisible(programmerVisible);
    }
    processorRef_.setVirtualKeyboardVisible(keyboardVisible);
    mainCanvas_.setProgrammerVisible(programmerVisible);
    mainCanvas_.setMixerVisible(mixerVisible);
    mainCanvas_.setVirtualKeyboardVisible(keyboardVisible);

    const bool effectivePgVisible = programmerVisible;
    const bool effectiveMxVisible = mixerVisible && !effectivePgVisible;

    const int virtualW = vf50::gui::tokens::kVirtualWidth;
    const int virtualH = vf50::gui::tokens::computeVirtualHeight(effectivePgVisible, keyboardVisible, effectiveMxVisible);
    const double aspect = static_cast<double>(virtualW) / static_cast<double>(virtualH);

    if (auto* boundsConstrainer = getConstrainer()) {
        boundsConstrainer->setFixedAspectRatio(aspect);
    }

    const float minTransform = vf50::gui::tokens::canvasTransformForUserScale(vf50::gui::tokens::kMinScaleFactor);
    const float maxTransform = vf50::gui::tokens::canvasTransformForUserScale(vf50::gui::tokens::kMaxScaleFactor);
    const int minW = static_cast<int>(std::lround(static_cast<float>(virtualW) * minTransform));
    const int minH = static_cast<int>(std::lround(static_cast<float>(virtualH) * minTransform));
    const int maxW = static_cast<int>(std::lround(static_cast<float>(virtualW) * maxTransform));
    const int maxH = static_cast<int>(std::lround(static_cast<float>(virtualH) * maxTransform));
    setResizeLimits(minW, minH, maxW, maxH);

    const float transformScale = vf50::gui::tokens::canvasTransformForUserScale(clampedScale);
    const int targetW = static_cast<int>(std::lround(static_cast<float>(virtualW) * transformScale));
    const int targetH = static_cast<int>(std::lround(static_cast<float>(virtualH) * transformScale));

    mainCanvas_.setBounds(0, 0, virtualW, virtualH);
    mainCanvas_.setTransform(juce::AffineTransform::scale(transformScale));
    mainCanvas_.setCurrentScaleFactor(clampedScale);

    if (getWidth() != targetW || getHeight() != targetH) {
        setSize(targetW, targetH);
    }
}

bool VF50AudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& path : files) {
        const juce::File f(path);
        if (f.isDirectory()) {
            return true;
        }
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext == ".bin" || ext == ".rom") {
            return true;
        }
        if (vf50::disk::VF50DiskManager::classifyFileKind(f.getFileName().toStdString())
            != vf50::disk::VF50FileKind::Unknown) {
            return true;
        }
    }
    return false;
}

void VF50AudioProcessorEditor::fileDragEnter(const juce::StringArray& /*files*/, int /*x*/, int /*y*/) {
    isDragActive_ = true;
    repaint();
}

void VF50AudioProcessorEditor::fileDragMove(const juce::StringArray& /*files*/, int /*x*/, int /*y*/) {
    if (!isDragActive_) {
        isDragActive_ = true;
        repaint();
    }
}

void VF50AudioProcessorEditor::fileDragExit(const juce::StringArray& /*files*/) {
    isDragActive_ = false;
    repaint();
}

void VF50AudioProcessorEditor::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) {
    isDragActive_ = false;
    repaint();

    if (files.size() > 0) {
        mainCanvas_.handleFilesDropped(files);
    }
}

void VF50AudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(vf50::gui::tokens::kChassisBottom));
}

void VF50AudioProcessorEditor::paintOverChildren(juce::Graphics& g) {
    if (!isDragActive_) {
        return;
    }
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0x1400E5FF));
    g.fillRect(bounds);

    g.setColour(juce::Colour(0x4800E5FF));
    g.drawRect(bounds.reduced(4.0f), 2.0f);

    g.setColour(juce::Colour(0xD800E5FF));
    g.drawRect(bounds.reduced(1.5f), 3.0f);
}

void VF50AudioProcessorEditor::resized() {
    const bool pgVisible = mainCanvas_.isProgrammerDrawerEffectivelyVisible();
    const bool mxVisible = mainCanvas_.isMixerVisible() && !pgVisible;
    const bool kbVisible = mainCanvas_.isVirtualKeyboardVisible();
    const int virtualW   = vf50::gui::tokens::kVirtualWidth;
    const int virtualH   = vf50::gui::tokens::computeVirtualHeight(pgVisible, kbVisible, mxVisible);

    mainCanvas_.setBounds(0, 0, virtualW, virtualH);

    const float sx = static_cast<float>(getWidth()) / static_cast<float>(virtualW);
    const float sy = static_cast<float>(getHeight()) / static_cast<float>(virtualH);
    const float rawTransform = std::min(sx, sy);
    const float userScale = std::clamp(rawTransform * 2.0f,
                                       vf50::gui::tokens::kMinScaleFactor,
                                       vf50::gui::tokens::kMaxScaleFactor);
    const float clampedTransform = vf50::gui::tokens::canvasTransformForUserScale(userScale);

    mainCanvas_.setTransform(juce::AffineTransform::scale(clampedTransform));
    mainCanvas_.setCurrentScaleFactor(userScale);
    processorRef_.setUiScaleFactor(userScale);
}

} // namespace vf50::plugin
