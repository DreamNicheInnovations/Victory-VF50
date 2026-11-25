#include "GUI/VF50RomGuideOverlay.h"
#include "Plugin/PluginProcessor.h"
#include "ROM/VF50RomLoader.h"

namespace vf50::gui {

VF50RomGuideOverlay::VF50RomGuideOverlay(vf50::plugin::VF50AudioProcessor& processor)
    : processor_(processor) {
    btnLoadFolder_ = std::make_unique<VF50PanelButton>("romLoadFolder", VF50PanelButton::CapStyle::UtilityPill);
    btnLoadFolder_->setCapLegends("LOAD ROM FOLDER...", tokens::kSilkAccentTeal);
    btnLoadFolder_->setFontScale(1.02f);
    btnLoadFolder_->onClick = [this]() { triggerLoadFolderChooser(); };
    addAndMakeVisible(*btnLoadFolder_);

    btnSelectFiles_ = std::make_unique<VF50PanelButton>("romSelectFiles", VF50PanelButton::CapStyle::UtilityPill);
    btnSelectFiles_->setCapLegends("SELECT .BIN FILES...", tokens::kSilkWarmWhite);
    btnSelectFiles_->setFontScale(1.02f);
    btnSelectFiles_->onClick = [this]() { triggerSelectFilesChooser(); };
    addAndMakeVisible(*btnSelectFiles_);

    btnOpenAutoFolder_ = std::make_unique<VF50PanelButton>("romOpenFolder", VF50PanelButton::CapStyle::UtilityPill);
    btnOpenAutoFolder_->setCapLegends("OPEN AUTO-LOAD FOLDER", tokens::kSilkWarmWhite);
    btnOpenAutoFolder_->setFontScale(0.95f);
    btnOpenAutoFolder_->onClick = [this]() { openAutoLoadFolder(); };
    addAndMakeVisible(*btnOpenAutoFolder_);

    btnClearRoms_ = std::make_unique<VF50PanelButton>("romClear", VF50PanelButton::CapStyle::UtilityPill);
    btnClearRoms_->setCapLegends("CLEAR EXTERNAL ROMS", tokens::kSilkOchreGold);
    btnClearRoms_->setFontScale(0.95f);
    btnClearRoms_->onClick = [this]() { triggerClearRoms(); };
    addAndMakeVisible(*btnClearRoms_);

    btnClose_ = std::make_unique<VF50PanelButton>("romClose", VF50PanelButton::CapStyle::UtilityPill);
    btnClose_->setCapLegends("CLOSE / USE SYNTH", tokens::kSilkWarmWhite);
    btnClose_->setFontScale(1.02f);
    btnClose_->onClick = [this]() {
        setVisible(false);
        if (onDismissed) {
            onDismissed();
        }
    };
    addAndMakeVisible(*btnClose_);

    refreshState();
}

VF50RomGuideOverlay::~VF50RomGuideOverlay() {
    activeChooser_.reset();
}

void VF50RomGuideOverlay::setStatusBanner(const juce::String& message) {
    statusBanner_ = message;
    repaint();
}

void VF50RomGuideOverlay::refreshState() {
    const auto& rom = processor_.activeRomBank();
    if (rom.isAllRomsLoaded()) {
        btnClose_->setCapLegends("DONE - PLAY SYNTH", tokens::kSilkAccentTeal);
        btnClose_->setActivePillHighlight(true);
    } else {
        btnClose_->setCapLegends("CLOSE (CLEAN MODE)", tokens::kSilkWarmWhite);
        btnClose_->setActivePillHighlight(false);
    }
    repaint();
}

void VF50RomGuideOverlay::resized() {
    const int w = getWidth();
    const int h = getHeight();
    const int btnY = h - 92;
    const int btnH = 60;

    btnLoadFolder_->setBounds(52, btnY, 440, btnH);
    btnSelectFiles_->setBounds(514, btnY, 440, btnH);
    btnOpenAutoFolder_->setBounds(976, btnY, 470, btnH);
    btnClearRoms_->setBounds(1468, btnY, 440, btnH);
    btnClose_->setBounds(w - 492, btnY, 440, btnH);
}

void VF50RomGuideOverlay::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();

    // Backdrop dim & anodized modal chassis card
    g.setColour(juce::Colour(0xE80A0D10u));
    g.fillRoundedRectangle(bounds, 18.0f);

    g.setColour(juce::Colour(tokens::kChassisBase));
    g.fillRoundedRectangle(bounds.reduced(8.0f), 14.0f);

    g.setColour(juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.70f));
    g.drawRoundedRectangle(bounds.reduced(8.0f), 14.0f, 3.0f);

    const auto& rom = processor_.activeRomBank();
    const auto& builder = processor_.romSetBuilder();
    const int loadedCount = rom.loadedChipCount();

    // Header band
    g.setColour(juce::Colour(tokens::kHeaderBarBg));
    g.fillRoundedRectangle(24.0f, 22.0f, bounds.getWidth() - 48.0f, 76.0f, 10.0f);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(31.0f, true));
    g.drawText("VICTORY VF50  -  BRING-YOUR-OWN-ROM (BYOR) SETUP GUIDE",
               48, 24, 1850, 72, juce::Justification::centredLeft);

    // Status pill on header right
    const bool allReady = rom.isAllRomsLoaded();
    const juce::Colour badgeCol = allReady ? juce::Colour(tokens::kSilkAccentTeal)
                                           : (loadedCount > 0 ? juce::Colour(tokens::kSilkOchreGold)
                                                              : juce::Colour(tokens::kSilkCrimsonBadge));
    const juce::Rectangle<float> pill(bounds.getWidth() - 560.0f, 34.0f, 512.0f, 52.0f);
    g.setColour(badgeCol.withAlpha(0.18f));
    g.fillRoundedRectangle(pill, 8.0f);
    g.setColour(badgeCol);
    g.drawRoundedRectangle(pill, 8.0f, 2.0f);
    g.setFont(VF50LookAndFeel::makeFont(24.0f, true));
    g.drawText(juce::String(loadedCount) + " / 4 ROM CHIPS ACTIVE" + (allReady ? "  (FULL VF50)" : ""),
               pill.toNearestInt(), juce::Justification::centred);

    // Subtitle / Live Status Banner
    const juce::String defaultStatus = allReady
        ? juce::String("All 4 Victory VF50 firmware & drum wave EPROMs are active. Full factory presets, rhythm patterns, drums & demo songs are enabled.")
        : juce::String("No proprietary firmware ROMs are bundled in this build. Load the 4 external VF50 EPROM dumps below to unlock factory presets, rhythm kits, patterns & demo songs.");
    const juce::String shownStatus = statusBanner_.isNotEmpty() ? statusBanner_ : defaultStatus;

    g.setColour(juce::Colour(tokens::kPanelRecessWell));
    g.fillRoundedRectangle(36.0f, 112.0f, bounds.getWidth() - 72.0f, 56.0f, 8.0f);
    g.setColour(allReady ? juce::Colour(tokens::kSilkAccentTeal) : juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(22.0f, false));
    g.drawText(shownStatus, 56, 112, static_cast<int>(bounds.getWidth()) - 112, 56,
               juce::Justification::centredLeft, true);

    // 4 Required EPROM Chip Rows
    const std::array<bool, 4> chipActiveFlags = {
        rom.hasMainRomA,
        rom.hasMainRomB,
        rom.hasSubCpuRom,
        rom.hasDrumWaveRom
    };

    float rowY = 186.0f;
    constexpr float rowH = 82.0f;
    constexpr float rowGap = 14.0f;
    for (int i = 0; i < 4; ++i) {
        const auto chipId = static_cast<vf50::rom::RomChipId>(i);
        const auto& spec = vf50::rom::getRomChipSpec(chipId);
        const bool active = chipActiveFlags[static_cast<std::size_t>(i)];

        const juce::Rectangle<float> card(36.0f, rowY, bounds.getWidth() - 72.0f, rowH);
        g.setColour(juce::Colour(tokens::kPanelSubFrameBg));
        g.fillRoundedRectangle(card, 10.0f);
        g.setColour(active ? juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.65f)
                           : juce::Colour(tokens::kPanelSubFrameBorder));
        g.drawRoundedRectangle(card, 10.0f, 2.0f);

        // Status chip badge
        const bool stagedOnly = (!active) && builder.hasChip(chipId);
        const juce::Rectangle<float> stRect(54.0f, rowY + 19.0f, 184.0f, 44.0f);
        const juce::Colour stCol = active ? juce::Colour(tokens::kSilkAccentTeal)
                                          : (stagedOnly ? juce::Colour(tokens::kSilkOchreGold)
                                                        : juce::Colour(tokens::kSilkCrimsonBadge));
        g.setColour(stCol.withAlpha(0.16f));
        g.fillRoundedRectangle(stRect, 7.0f);
        g.setColour(stCol);
        g.drawRoundedRectangle(stRect, 7.0f, 1.8f);
        g.setFont(VF50LookAndFeel::makeFont(stagedOnly ? 17.5f : 21.0f, true));
        g.drawText(active ? "LOADED" : (stagedOnly ? "NEED IC731" : "MISSING"),
                   stRect.toNearestInt(), juce::Justification::centred);

        // Canonical filename & byte size
        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(24.0f, true));
        g.drawText(spec.canonicalFilename, 260, static_cast<int>(rowY) + 10, 420, 34,
                   juce::Justification::centredLeft);

        g.setColour(juce::Colour(tokens::kSilkMutedSilver));
        g.setFont(VF50LookAndFeel::makeFont(19.0f, false));
        g.drawText(juce::String::formatted("Exact size: %zu bytes (%zu KB)",
                                           spec.expectedBytes, spec.expectedBytes / 1024u),
                   260, static_cast<int>(rowY) + 44, 420, 28,
                   juce::Justification::centredLeft);

        // Role & source path
        g.setColour(juce::Colour(tokens::kSilkWarmWhite).withAlpha(0.92f));
        g.setFont(VF50LookAndFeel::makeFont(21.0f, false));
        g.drawText(spec.roleSummary, 690, static_cast<int>(rowY) + 10,
                   static_cast<int>(bounds.getWidth()) - 750, 32,
                   juce::Justification::centredLeft, true);

        juce::String sourceLabel;
        if (builder.hasChip(chipId)) {
            sourceLabel = "External File: " + builder.chipFile(chipId).getFullPathName();
        } else if (active) {
            sourceLabel = "Source: " + juce::String(rom.chipSourcePaths[static_cast<std::size_t>(i)].data());
        } else {
            sourceLabel = "Drop this .BIN/.ROM anywhere onto the VF50 window or click [LOAD ROM FOLDER...]";
        }
        g.setColour((active || stagedOnly) ? juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.88f)
                                          : juce::Colour(tokens::kSilkDimLabel));
        g.setFont(VF50LookAndFeel::makeFont(18.0f, false));
        g.drawText(sourceLabel, 690, static_cast<int>(rowY) + 44,
                   static_cast<int>(bounds.getWidth()) - 750, 28,
                   juce::Justification::centredLeft, true);

        rowY += rowH + rowGap;
    }

    // Bottom Quick Instructions Box
    const float guideY = rowY + 4.0f;
    const float guideH = bounds.getHeight() - guideY - 110.0f;
    const juce::Rectangle<float> guideRect(36.0f, guideY, bounds.getWidth() - 72.0f, guideH);
    g.setColour(juce::Colour(tokens::kPanelRecessWell));
    g.fillRoundedRectangle(guideRect, 10.0f);
    g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
    g.drawRoundedRectangle(guideRect, 10.0f, 1.5f);

    const auto autoDir = vf50::rom::VF50RomSetBuilder::getUserDefaultRomFolder().getFullPathName();
    g.setColour(juce::Colour(tokens::kSilkAccentTeal));
    g.setFont(VF50LookAndFeel::makeFont(22.0f, true));
    g.drawText("HOW TO GET VICTORY VF50 RUNNING IN 3 QUICK STEPS:",
               58, static_cast<int>(guideY) + 12, 1600, 30, juce::Justification::centredLeft);

    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(19.5f, false));
    g.drawText("1. Obtain the 4 external VF50 EPROM dumps (.BIN/.ROM) listed above (content signatures & sizes are auto-detected even if renamed).",
               58, static_cast<int>(guideY) + 44, static_cast<int>(bounds.getWidth()) - 116, 26,
               juce::Justification::centredLeft);
    g.drawText("2. Either Drag & Drop the 4 files / folder directly onto the plugin, click [LOAD ROM FOLDER...], OR place them in:  " + autoDir,
               58, static_cast<int>(guideY) + 72, static_cast<int>(bounds.getWidth()) - 116, 26,
               juce::Justification::centredLeft, true);
    g.drawText("3. Paths are saved automatically across DAW sessions. Without ROMs, Init Voice, PG-Programmer sliders, .SYX SysEx patches & .IMG/.DSK floppy disk images remain usable.",
               58, static_cast<int>(guideY) + 100, static_cast<int>(bounds.getWidth()) - 116, 26,
               juce::Justification::centredLeft, true);
}

void VF50RomGuideOverlay::triggerLoadFolderChooser() {
    auto startDir = vf50::rom::VF50RomSetBuilder::getUserDefaultRomFolder();
    if (!startDir.exists()) {
        startDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }
    activeChooser_ = std::make_unique<juce::FileChooser>(
        "Select Folder Containing VF50 Firmware & Drum ROM (.BIN/.ROM) Files",
        startDir,
        "*");
    juce::Component::SafePointer<VF50RomGuideOverlay> safeThis(this);
    activeChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) {
                return;
            }
            const auto dir = chooser.getResult();
            if (dir != juce::File{} && dir.isDirectory()) {
                juce::String status;
                safeThis->processor_.loadRomFolder(dir, status, true);
                safeThis->setStatusBanner(status);
                safeThis->refreshState();
                if (safeThis->onRomStateChanged) {
                    safeThis->onRomStateChanged(status);
                }
            }
        });
}

void VF50RomGuideOverlay::triggerSelectFilesChooser() {
    auto startDir = vf50::rom::VF50RomSetBuilder::getUserDefaultRomFolder();
    if (!startDir.exists()) {
        startDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }
    activeChooser_ = std::make_unique<juce::FileChooser>(
        "Select VF50 Firmware & Drum ROM (.BIN/.ROM) Files",
        startDir,
        "*.bin;*.BIN;*.rom;*.ROM");
    juce::Component::SafePointer<VF50RomGuideOverlay> safeThis(this);
    activeChooser_->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectMultipleItems,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) {
                return;
            }
            const auto results = chooser.getResults();
            if (!results.isEmpty()) {
                juce::StringArray paths;
                for (const auto& f : results) {
                    paths.add(f.getFullPathName());
                }
                juce::String status;
                safeThis->processor_.loadRomFiles(paths, status, true);
                safeThis->setStatusBanner(status);
                safeThis->refreshState();
                if (safeThis->onRomStateChanged) {
                    safeThis->onRomStateChanged(status);
                }
            }
        });
}

void VF50RomGuideOverlay::openAutoLoadFolder() {
    const auto dir = vf50::rom::VF50RomSetBuilder::getUserDefaultRomFolder();
    (void) dir.createDirectory();
    dir.revealToUser();
    setStatusBanner("Auto-load folder opened: " + dir.getFullPathName());
}

void VF50RomGuideOverlay::triggerClearRoms() {
    processor_.clearExternalRoms();
    setStatusBanner("Cleared saved external ROM paths.");
    refreshState();
    if (onRomStateChanged) {
        onRomStateChanged("External ROMs cleared");
    }
}

} // namespace vf50::gui
