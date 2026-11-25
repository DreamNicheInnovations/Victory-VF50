#pragma once

#include "GUI/VF50DesignTokens.h"
#include "GUI/VF50LookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace vf50::plugin {
class VF50AudioProcessor;
}

namespace vf50::gui {

class VF50RomGuideOverlay final : public juce::Component {
public:
    explicit VF50RomGuideOverlay(vf50::plugin::VF50AudioProcessor& processor);
    ~VF50RomGuideOverlay() override;

    void setStatusBanner(const juce::String& message);
    void refreshState();

    void paint(juce::Graphics& g) override;
    void resized() override;

    std::function<void()> onDismissed;
    std::function<void(const juce::String&)> onRomStateChanged;

private:
    void triggerLoadFolderChooser();
    void triggerSelectFilesChooser();
    void openAutoLoadFolder();
    void triggerClearRoms();

    vf50::plugin::VF50AudioProcessor& processor_;
    std::unique_ptr<juce::FileChooser> activeChooser_{};

    std::unique_ptr<VF50PanelButton> btnLoadFolder_{};
    std::unique_ptr<VF50PanelButton> btnSelectFiles_{};
    std::unique_ptr<VF50PanelButton> btnOpenAutoFolder_{};
    std::unique_ptr<VF50PanelButton> btnClearRoms_{};
    std::unique_ptr<VF50PanelButton> btnClose_{};

    juce::String statusBanner_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50RomGuideOverlay)
};

} // namespace vf50::gui
