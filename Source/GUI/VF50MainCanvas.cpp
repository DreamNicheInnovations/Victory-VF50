#include "GUI/VF50MainCanvas.h"
#include "DSP/FM/OpzTables.h"
#include "Generated/VF50ParameterLayout.h"
#include "ROM/VF50RomLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vf50::gui {

namespace {

using vf50::params::ParamIndex;

[[nodiscard]] juce::Rectangle<int> toRect(const tokens::Rect2x& r) noexcept {
    return { r.x, r.y, r.w, r.h };
}

[[nodiscard]] ParamIndex opParam(int opIdx0To3, std::size_t fieldOffset) noexcept {
    const auto op = static_cast<std::size_t>(std::clamp(opIdx0To3, 0, 3));
    return static_cast<ParamIndex>(vf50::params::kOperatorsStart + op * vf50::params::kParamsPerOperator + fieldOffset);
}

[[nodiscard]] ParamIndex partParam(int partIdx0To7, std::size_t fieldOffset) noexcept {
    const auto p = static_cast<std::size_t>(std::clamp(partIdx0To7, 0, 7));
    return static_cast<ParamIndex>(vf50::params::kPerfPartsStart + p * vf50::params::kParamsPerPerfPart + fieldOffset);
}

[[nodiscard]] ParamIndex drumParam(int drumIdx0To60, std::size_t fieldOffset) noexcept {
    const auto d = static_cast<std::size_t>(std::clamp(drumIdx0To60, 0, 60));
    return static_cast<ParamIndex>(vf50::params::kDrumsStart + d * vf50::params::kParamsPerDrumInst + fieldOffset);
}

void writeFixedCol(std::array<char, 41>& buf, int col, const char* src, int maxChars = 40) noexcept {
    if (src == nullptr || col >= 40) {
        return;
    }
    int pos = std::max(0, col);
    for (int i = 0; src[i] != '\0' && i < maxChars && pos < 40; ++i, ++pos) {
        buf[static_cast<std::size_t>(pos)] = src[i];
    }
}

void formatMidiNoteName(int midiNote, char outBuf[5]) noexcept {
    static constexpr std::array<const char*, 12> kNames = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int n = std::clamp(midiNote, 0, 127);
    const int oct = (n / 12) - 2; // Hardware C3 = MIDI 60
    std::snprintf(outBuf, 5, "%s%d", kNames[static_cast<std::size_t>(n % 12)], oct);
}

void drawClassicLedState(juce::Graphics& g,
                         juce::Point<float> center,
                         float radius,
                         vf50::classic::LedState state,
                         bool blinkOn) {
    using vf50::classic::LedState;
    const bool isRed = (state == LedState::BlinkRed || state == LedState::SolidRed);
    const bool isLit = (state == LedState::SolidGreen || state == LedState::SolidRed)
                    || ((state == LedState::BlinkGreen || state == LedState::BlinkRed) && blinkOn);
    VF50LookAndFeel::drawHardwareLed(g, center, radius, isRed, isLit);
}

} // namespace

// =============================================================================
// Draggable 8-Track SMF Export Badge: [DRAG 8 TRACKS TO DAW]
// =============================================================================
VF50DragTracksBadge::VF50DragTracksBadge() {
    setName("Drag8TracksToDawBadge");
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void VF50DragTracksBadge::paint(juce::Graphics& g) {
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    const juce::Colour borderCol = dragging_
                                       ? juce::Colour(0xFF00E5FFu)
                                       : (hovered_ ? juce::Colour(tokens::kSilkOchreGold)
                                                   : juce::Colour(0xFF3E4652u));

    juce::ColourGradient bgGrad(juce::Colour(hovered_ ? 0xFF2C323Cu : 0xFF22272Fu), b.getX(), b.getY(),
                                juce::Colour(0xFF15191Eu), b.getX(), b.getBottom(), false);
    g.setGradientFill(bgGrad);
    g.fillRoundedRectangle(b, 5.5f);
    g.setColour(borderCol);
    g.drawRoundedRectangle(b, 5.5f, dragging_ ? 2.2f : 1.6f);

    // 2x3 tactile drag-grip knurl dots on left
    g.setColour(juce::Colour(hovered_ ? tokens::kSilkOchreGold : tokens::kSilkMutedSilver));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 2; ++c) {
            g.fillEllipse(b.getX() + 14.0f + static_cast<float>(c) * 8.0f,
                          b.getCentreY() - 10.0f + static_cast<float>(r) * 8.0f,
                          4.0f, 4.0f);
        }
    }

    // Mini 8-track lane indicator bars on right
    for (int t = 0; t < 4; ++t) {
        const float ty = b.getY() + 12.0f + static_cast<float>(t) * 8.5f;
        g.setColour(juce::Colour((t == 3) ? tokens::kSilkOchreGold : tokens::kSilkAccentTeal).withAlpha(0.75f));
        g.fillRoundedRectangle(b.getRight() - 42.0f, ty, 26.0f - static_cast<float>((t & 1) * 6), 4.5f, 1.5f);
    }

    g.setColour(juce::Colour(hovered_ || dragging_ ? tokens::kSilkWarmWhite : tokens::kSilkOchreGold));
    g.setFont(VF50LookAndFeel::makeFont(15.5f, true, 1.03f));
    g.drawText("DRAG 8 TRACKS TO DAW",
               juce::Rectangle<int>(static_cast<int>(b.getX() + 34.0f),
                                    static_cast<int>(b.getY()),
                                    static_cast<int>(b.getWidth() - 82.0f),
                                    static_cast<int>(b.getHeight())),
               juce::Justification::centred,
               false);
}

void VF50DragTracksBadge::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        return;
    }
    dragging_      = true;
    dragInitiated_ = false;
    repaint();
}

void VF50DragTracksBadge::mouseDrag(const juce::MouseEvent& e) {
    if (!dragInitiated_ && e.getDistanceFromDragStart() >= 4) {
        dragInitiated_ = true;
        if (onStartExternalMidiDrag) {
            onStartExternalMidiDrag();
        }
        dragging_ = false;
        repaint();
    }
}

VF50MainCanvas::VF50MainCanvas(vf50::plugin::VF50AudioProcessor& processor)
    : processor_(processor),
      currentScaleFactor_(processor.uiScaleFactor()),
      programmerVisible_(processor.isProgrammerVisible()),
      mixerVisible_(processor.isMixerVisible() && !processor.isProgrammerVisible()),
      virtualKeyboardVisible_(processor.isVirtualKeyboardVisible()),
      lastSeenPrefsEpoch_(processor.uiPreferencesEpoch()),
      lastSeenPatchEpoch_(processor.patchSyncEpoch()),
      lastSeenRomEpoch_(processor.romEpoch()),
      classicController_(processor_, diskManager_),
      programmerDrawer_(processor),
      mixerDrawer_(processor, classicController_),
      virtualKeyboard_(processor),
      romGuideOverlay_(processor),
      lastSeenDrumTrigSeq_(processor.lastDrumTriggerSeq()) {
    setLookAndFeel(&lookAndFeel_);
    setOpaque(true);
    setWantsKeyboardFocus(true);

    // Zero-chrome DAW-safe keyboard focus proxy for PC keyboard typing on LCD text-entry screens
    textFocusProxy_.setAlpha(0.0f);
    textFocusProxy_.setMultiLine(false, false);
    textFocusProxy_.setReturnKeyStartsNewLine(false);
    textFocusProxy_.setTabKeyUsedAsCharacter(false);
    textFocusProxy_.setCaretVisible(false);
    textFocusProxy_.setScrollbarsShown(false);
    textFocusProxy_.setBorder(juce::BorderSize<int>(0));
    textFocusProxy_.setInterceptsMouseClicks(false, false);
    textFocusProxy_.setWantsKeyboardFocus(false);
    textFocusProxy_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    textFocusProxy_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    textFocusProxy_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    textFocusProxy_.onKey = [this](const juce::KeyPress& k) {
        return handlePcTextEntryKeyPress(k);
    };
    textFocusProxy_.onTextInsert = [this](const juce::String& txt) {
        if (!classicController_.isTextEntryScreenActive()) {
            return;
        }
        bool anyHandled = false;
        for (int i = 0; i < txt.length(); ++i) {
            const auto ch = txt[i];
            if (ch >= 0x20 && ch <= 0x7E) {
                anyHandled |= classicController_.onTextKeyPressed(static_cast<char>(ch));
            }
        }
        if (anyHandled) {
            syncMenuPageFromClassicController();
            programmerDrawer_.syncFromProcessor();
            refreshDisplayAndLeds();
        }
    };
    addAndMakeVisible(textFocusProxy_);

    // Recessed 40x2 Dot-Matrix LCD (BL corner badge & right-click cycle LCD themes)
    lcd_.setThemeIndex(processor_.lcdThemeIndex());
    lcd_.onThemeChanged = [this](int newThemeIdx) {
        processor_.setLcdThemeIndex(newThemeIdx);
    };
    addAndMakeVisible(lcd_);
    lcd_.onCellClicked = [this](int row, int col) {
        if (classicController_.isTextEntryScreenActive()) {
            textFocusProxy_.setWantsKeyboardFocus(true);
            if (isShowing() && getPeer() != nullptr) {
                textFocusProxy_.grabKeyboardFocus();
            }
            if (row == 0) {
                return;
            }
        }
        const int slot = std::clamp(col / 5, 0, 7);
        pressSoftKey(slot);
    };
    lcd_.onCellValueScrub = [this](int col, int deltaSteps) {
        const int slot = std::clamp(col / 5, 0, 7);
        if (slot < activeFieldCount_ && pageFields_[static_cast<std::size_t>(slot)].valid) {
            focusedFieldIdx_ = slot;
        }
        stepFocusedField(deltaSteps);
    };

    setupHeaderScalePills();
    setupLeftSectionControls();
    setupCenterSectionControls();
    setupRightSectionControls();

    // Pop-Down PG-Style 63-Slider Hardware Programmer Drawer
    programmerDrawer_.onParameterAdjusted = [this](vf50::params::ParamIndex pIdx,
                                                   int /*opIdx*/,
                                                   const juce::String& /*lcdLine0*/,
                                                   const juce::String& /*lcdLine1*/) {
        handleProgrammerParamAdjusted(pIdx, 0.0f);
    };
    addChildComponent(programmerDrawer_);
    programmerDrawer_.setVisible(programmerVisible_);

    // Pop-Down Visual Mixer Drawer (mutually exclusive with Programmer Drawer)
    mixerDrawer_.onMixerStateChanged = [this]() {
        refreshDisplayAndLeds();
    };
    addChildComponent(mixerDrawer_);
    mixerDrawer_.setVisible(mixerVisible_);

    // Switchable bottom 61-key VF50 Virtual Keyboard
    addChildComponent(virtualKeyboard_);
    virtualKeyboard_.setVisible(virtualKeyboardVisible_);

    // Set 2x virtual canvas bounds AFTER all child hardware controls are constructed
    setSize(tokens::kVirtualWidth,
            tokens::computeVirtualHeight(programmerVisible_, virtualKeyboardVisible_, mixerVisible_));

    // Sync initial APVTS mirror and build Classic 40x2 LCD state
    const auto ws = processor_.activeWorkstationMode();
    if (processor_.isDemoPlaying()) {
        page_ = MenuPage::SequencerDemo;
    } else if (ws == vf50::state::WorkstationMode::RhythmPlay) {
        processor_.syncApvtsToActiveSingleVoice(false);
        page_ = MenuPage::RhythmPlayEdit;
    } else if (ws == vf50::state::WorkstationMode::PerformancePlay || ws == vf50::state::WorkstationMode::PerformanceEdit) {
        processor_.syncApvtsToActivePerformance(false);
        page_ = MenuPage::PerfPlay;
    } else {
        processor_.syncApvtsToActiveSingleVoice(false);
        page_ = MenuPage::SinglePlay;
    }

    syncStateMachineFromMenuPage();
    programmerDrawer_.syncFromProcessor();
    mixerDrawer_.syncFromProcessor();
    if (page_ == MenuPage::SequencerDemo) {
        classicController_.enterScreen(vf50::classic::ClassicScreen::SeqPlaySong);
    } else if (page_ == MenuPage::RhythmPlayEdit) {
        (void)classicController_.onPressRhythm();
    } else {
        const bool isEditPg = (page_ != MenuPage::SinglePlay && page_ != MenuPage::PerfPlay);
        classicController_.syncFromWorkstationMode(ws, isEditPg);
    }

    romGuideOverlay_.onDismissed = [this]() { refreshDisplayAndLeds(); };
    romGuideOverlay_.onRomStateChanged = [this](const juce::String& st) {
        diskStatusMsg_   = st.substring(0, 40);
        diskStatusTicks_ = 120u;
        refreshDisplayAndLeds();
    };
    addChildComponent(romGuideOverlay_);
    if (!processor_.activeRomBank().isAllRomsLoaded()) {
        romGuideOverlay_.setVisible(true);
        romGuideOverlay_.toFront(false);
    }

    resized();
    refreshDisplayAndLeds();
    startTimerHz(30);
}

VF50MainCanvas::~VF50MainCanvas() {
    stopTimer();
    activeFileChooser_.reset();
    volumeAttach_.reset();
    if (c3AuditionHeld_) {
        processor_.enqueueUiMidi3(0x80u, 60u, 0u);
        c3AuditionHeld_ = false;
    }
    setLookAndFeel(nullptr);
}

void VF50MainCanvas::setCurrentScaleFactor(float scaleFactor) noexcept {
    currentScaleFactor_ = std::clamp(scaleFactor, tokens::kMinScaleFactor, tokens::kMaxScaleFactor);
    lookAndFeel_.setScaleFactor(currentScaleFactor_);
    for (std::size_t i = 0; i < scalePills_.size(); ++i) {
        if (scalePills_[i]) {
            const bool match = std::abs(currentScaleFactor_ - tokens::kDiscreteUiScales[i]) < 0.03f;
            scalePills_[i]->setActivePillHighlight(match);
        }
    }
    repaint(0, 0, tokens::kVirtualWidth, tokens::kHeaderContourGrooveY2x);
}

void VF50MainCanvas::setProgrammerVisible(bool visible) {
    const bool closedMixer = visible && mixerVisible_;
    if (closedMixer) {
        mixerVisible_ = false;
        mixerDrawer_.setVisible(false);
    }
    if (programmerVisible_ != visible || programmerDrawer_.isVisible() != visible || closedMixer) {
        programmerVisible_ = visible;
        programmerDrawer_.setVisible(visible);
        if (visible) {
            programmerDrawer_.syncFromProcessor();
        }
        setSize(tokens::kVirtualWidth,
                tokens::computeVirtualHeight(programmerVisible_, virtualKeyboardVisible_, mixerVisible_));
        resized();
        repaint();
    }
}

void VF50MainCanvas::setMixerVisible(bool visible) {
    const bool closedPg = visible && programmerVisible_;
    if (closedPg) {
        programmerVisible_ = false;
        programmerDrawer_.setVisible(false);
    }
    if (mixerVisible_ != visible || mixerDrawer_.isVisible() != visible || closedPg) {
        mixerVisible_ = visible;
        mixerDrawer_.setVisible(visible);
        if (visible) {
            mixerDrawer_.syncFromProcessor();
        }
        if (btnMixer_) {
            btnMixer_->setActivePillHighlight(mixerVisible_);
        }
        setSize(tokens::kVirtualWidth,
                tokens::computeVirtualHeight(programmerVisible_, virtualKeyboardVisible_, mixerVisible_));
        resized();
        repaint();
    }
}

void VF50MainCanvas::setVirtualKeyboardVisible(bool visible) {
    if (virtualKeyboardVisible_ != visible || virtualKeyboard_.isVisible() != visible) {
        virtualKeyboardVisible_ = visible;
        virtualKeyboard_.setVisible(visible);
        setSize(tokens::kVirtualWidth,
                tokens::computeVirtualHeight(programmerVisible_, virtualKeyboardVisible_, mixerVisible_));
        resized();
        repaint();
    }
}

bool VF50MainCanvas::isFloppyHostSlotVisible() const noexcept {
    return (btnFloppyInsert_ != nullptr && btnFloppyInsert_->isVisible())
        && (btnFloppyFolder_ != nullptr && btnFloppyFolder_->isVisible())
        && (btnFloppyUserDisk_ != nullptr && btnFloppyUserDisk_->isVisible())
        && (btnFloppySetUserDir_ != nullptr && btnFloppySetUserDir_->isVisible())
        && (btnFloppyEject_ != nullptr && btnFloppyEject_->isVisible());
}

bool VF50MainCanvas::isPreservedHostUtilitiesVisible() const noexcept {
    const bool expVis    = (btnExportMid_ != nullptr) && btnExportMid_->isVisible();
    const bool dragVis   = dragToDawBadge_.isVisible();
    const bool romBtnVis = (btnRomLoader_ != nullptr) && btnRomLoader_->isVisible();
    const bool pgBtnVis  = (btnProgrammer_ != nullptr) && btnProgrammer_->isVisible();
    return isFloppyHostSlotVisible() && expVis && dragVis && romBtnVis && pgBtnVis;
}

void VF50MainCanvas::setupHeaderScalePills() {
    btnFloppyInsert_ = std::make_unique<VF50PanelButton>("FloppyInsert",
                                                        VF50PanelButton::CapStyle::UtilityPill);
    btnFloppyInsert_->setCapLegends("DISK IN", tokens::kSilkAccentTeal);
    btnFloppyInsert_->setFontScale(0.84f);
    btnFloppyInsert_->onClick = [this]() { launchMountImageDialog(); };
    addAndMakeVisible(*btnFloppyInsert_);

    btnFloppyFolder_ = std::make_unique<VF50PanelButton>("FloppyFolder",
                                                        VF50PanelButton::CapStyle::UtilityPill);
    btnFloppyFolder_->setCapLegends("FOLDER", tokens::kSilkWarmWhite);
    btnFloppyFolder_->setFontScale(0.82f);
    btnFloppyFolder_->setTooltip("Mount any local folder containing VF50 files");
    btnFloppyFolder_->onClick = [this]() { launchMountFolderDialog(); };
    addAndMakeVisible(*btnFloppyFolder_);

    btnFloppyUserDisk_ = std::make_unique<VF50PanelButton>("FloppyUserDisk",
                                                          VF50PanelButton::CapStyle::UtilityPill);
    btnFloppyUserDisk_->setCapLegends("USER DISK", tokens::kSilkAccentTeal);
    btnFloppyUserDisk_->setFontScale(0.76f);
    btnFloppyUserDisk_->setTooltip("Mount Common User Disk folder: " + diskManager_.userDiskDirectory().getFullPathName());
    btnFloppyUserDisk_->onClick = [this]() { (void)mountUserDiskButton(); };
    addAndMakeVisible(*btnFloppyUserDisk_);

    btnFloppySetUserDir_ = std::make_unique<VF50PanelButton>("FloppySetUserDir",
                                                            VF50PanelButton::CapStyle::UtilityPill);
    btnFloppySetUserDir_->setCapLegends("SET DIR", tokens::kSilkWarmWhite);
    btnFloppySetUserDir_->setFontScale(0.76f);
    btnFloppySetUserDir_->setTooltip("Change User Disk Folder...");
    btnFloppySetUserDir_->onClick = [this]() { launchChangeUserDiskFolderDialog(); };
    addAndMakeVisible(*btnFloppySetUserDir_);

    btnFloppyEject_ = std::make_unique<VF50PanelButton>("FloppyEject",
                                                       VF50PanelButton::CapStyle::UtilityPill);
    btnFloppyEject_->setCapLegends("EJECT", tokens::kSilkOchreGold);
    btnFloppyEject_->setFontScale(0.82f);
    btnFloppyEject_->onClick = [this]() {
        if (diskManager_.isMounted()) {
            diskManager_.unmount();
            diskStatusMsg_   = "FLOPPY EJECTED";
            diskStatusTicks_ = 60u;
            refreshDisplayAndLeds();
        }
    };
    addAndMakeVisible(*btnFloppyEject_);

    // Top Header Firmware ROM Setup & Status Pill (alongside Virtual Floppy Loader & SCALE)
    btnRomLoader_ = std::make_unique<VF50PanelButton>("ROM_LOADER", VF50PanelButton::CapStyle::UtilityPill);
    btnRomLoader_->setCapLegends("LOAD ROMS  [0/4]", tokens::kSilkOchreGold);
    btnRomLoader_->setFontScale(0.84f);
    btnRomLoader_->onClick = [this]() { setRomGuideVisible(!romGuideOverlay_.isVisible()); };
    addAndMakeVisible(*btnRomLoader_);

    static constexpr std::array<const char*, 5> kLabels = {
        "75%", "100%", "125%", "150%", "200%"
    };
    for (std::size_t i = 0; i < scalePills_.size(); ++i) {
        auto btn = std::make_unique<VF50PanelButton>(juce::String("Scale_") + kLabels[i],
                                                    VF50PanelButton::CapStyle::UtilityPill);
        btn->setCapLegends(kLabels[i], tokens::kSilkWarmWhite);
        btn->onClick = [this, i]() {
            if (onRequestScaleChange) {
                onRequestScaleChange(tokens::kDiscreteUiScales[i]);
            }
        };
        addAndMakeVisible(*btn);
        scalePills_[i] = std::move(btn);
    }
    setCurrentScaleFactor(currentScaleFactor_);
}

void VF50MainCanvas::setupLeftSectionControls() {
    // Master Volume Fader (APVTS attached)
    volumeFader_.setSliderStyle(juce::Slider::LinearVertical);
    volumeFader_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    addAndMakeVisible(volumeFader_);
    volumeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor_.apvts(), "masterVolume", volumeFader_);

    // Data Entry / Tempo Fader (edits currently focused LCD menu field)
    dataEntryFader_.setSliderStyle(juce::Slider::LinearVertical);
    dataEntryFader_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    dataEntryFader_.setRange(0.0, 1.0, 0.001);
    dataEntryFader_.setValue(0.5, juce::dontSendNotification);
    dataEntryFader_.onValueChange = [this]() {
        if (!suppressDataSliderCallback_) {
            setDataEntryNormalized(static_cast<float>(dataEntryFader_.getValue()));
        }
    };
    addAndMakeVisible(dataEntryFader_);

    // Spring-Loaded Pitch Wheel & Continuous Modulation Wheel
    pitchWheel_.onValueChanged = [this](float bipolar) {
        processor_.enqueueUiPitchBendNorm(bipolar, 0u);
    };
    addAndMakeVisible(pitchWheel_);

    modWheel_.onValueChanged = [this](float unipolar) {
        processor_.enqueueUiModWheelNorm(unipolar, 0u);
    };
    addAndMakeVisible(modWheel_);
}

void VF50MainCanvas::setupCenterSectionControls() {
    // 8 Soft-Function Keys directly beneath the 40x2 LCD
    for (int i = 0; i < 8; ++i) {
        auto sk = std::make_unique<VF50PanelButton>("SoftKey_" + juce::String(i + 1),
                                                   VF50PanelButton::CapStyle::SoftKeySlim);
        sk->setRightClickLatchEnabled(true);
        sk->canRightClickLatch = [this]() {
            return classicController_.screen() == vf50::classic::ClassicScreen::RhythmPtnRecStep;
        };
        sk->onHoldLatchChanged = [this](bool /*latched*/) {
            classicController_.setHeldRhythmModifiers(isEraseModifierActive(), isStepInstModifierActive());
            refreshDisplayAndLeds();
        };
        if (i == 2 || i == 3) {
            sk->setTooltip("Softkey (>Inst) — Right-Click during Rhythm Step Record to hold + press drum key");
        }
        sk->onClick = [this, i]() { pressSoftKey(i); };
        addAndMakeVisible(*sk);
        softKeys_[static_cast<std::size_t>(i)] = std::move(sk);
    }

    // SYSTEM Row: SEQ, RHYTHM, JOB
    btnSeq_ = std::make_unique<VF50PanelButton>("SEQ");
    btnSeq_->setCapLegends("SEQ", tokens::kSilkWarmWhite);
    btnSeq_->onClick = [this]() { pressSeqButton(); };
    addAndMakeVisible(*btnSeq_);

    btnRhythm_ = std::make_unique<VF50PanelButton>("RHYTHM");
    btnRhythm_->setCapLegends("RHYTHM", tokens::kSilkWarmWhite);
    btnRhythm_->setRightClickLatchEnabled(true);
    btnRhythm_->onHoldLatchChanged = [this](bool /*latched*/) {
        refreshDisplayAndLeds();
    };
    btnRhythm_->setTooltip("RHYTHM — Right-Click to hold + drag Data Entry slider to adjust Rhythm Tempo");
    btnRhythm_->onClick = [this]() { pressRhythmButton(); };
    addAndMakeVisible(*btnRhythm_);

    btnJob_ = std::make_unique<VF50PanelButton>("JOB");
    btnJob_->setCapLegends("JOB", tokens::kSilkWarmWhite);
    btnJob_->onClick = [this]() { handleJobPress(); };
    addAndMakeVisible(*btnJob_);

    // [-] INST ON/OFF / LETTER / ERASE Button
    btnInstMinus_ = std::make_unique<VF50PanelButton>("INST_ON_OFF");
    btnInstMinus_->setCapLegends("[-]", tokens::kSilkWarmWhite);
    btnInstMinus_->setRightClickLatchEnabled(true);
    btnInstMinus_->onHoldLatchChanged = [this](bool latched) {
        classicController_.setInstMinusArmed(latched);
        classicController_.setHeldRhythmModifiers(isEraseModifierActive(), isStepInstModifierActive());
        refreshDisplayAndLeds();
    };
    btnInstMinus_->setTooltip("[-] / ERASE — Right-Click to hold: +7/8 Bank Hold, +TR1-8/RHY KbdTch, or +Drum Keys to Erase");
    btnInstMinus_->onClick = [this]() { pressInstMinusButton(); };
    addAndMakeVisible(*btnInstMinus_);

    // SEQUENCER / OPERATOR Row (5 Dual-Function Tactile Buttons)
    static constexpr std::array<VF50PanelButton::TransportIcon, 5> kSeqIcons = {
        VF50PanelButton::TransportIcon::RecordDot,
        VF50PanelButton::TransportIcon::RewindDouble,
        VF50PanelButton::TransportIcon::StopSquare,
        VF50PanelButton::TransportIcon::PlaySingle,
        VF50PanelButton::TransportIcon::ForwardDouble
    };
    static constexpr std::array<uint32_t, 5> kSeqIconCols = {
        tokens::kSilkCrimsonBadge,
        tokens::kSilkWarmWhite,
        tokens::kSilkWarmWhite,
        tokens::kSilkPlayGreen,
        tokens::kSilkWarmWhite
    };
    for (int i = 0; i < 5; ++i) {
        auto b = std::make_unique<VF50PanelButton>("SeqOp_" + juce::String(i));
        if (i == 2) {
            b->setCapLegends("TOP", tokens::kSilkMutedSilver);
            b->setRightClickLatchEnabled(true);
            b->setTooltip("STOP — Click again while stopped to return to Meas 001 | Right-Click to hold for STOP + BWD");
        } else if (i == 1) {
            b->onShiftLeftClick = [this](const juce::MouseEvent&) {
                releaseStopLatchIfSet();
                pressStopAndBwdTogether();
            };
            b->setTooltip("BWD (-1 Meas) — Shift+Click (or Right-Click STOP then BWD) to return to Meas 001");
        }
        b->setTransportIcon(kSeqIcons[static_cast<std::size_t>(i)], kSeqIconCols[static_cast<std::size_t>(i)]);
        b->onClick = [this, i]() { handleSeqOpRowPress(i); };
        addAndMakeVisible(*b);
        seqOpBtns_[static_cast<std::size_t>(i)] = std::move(b);
    }

    // Sequencer / Outer Utility Strip: [EXPORT .MID] & [DRAG 8 TRACKS TO DAW]
    btnExportMid_ = std::make_unique<VF50PanelButton>("EXPORT_MID", VF50PanelButton::CapStyle::UtilityPill);
    btnExportMid_->setCapLegends("EXPORT .MID", tokens::kSilkAccentTeal);
    btnExportMid_->onClick = [this]() { launchExportMidiDialog(); };
    addAndMakeVisible(*btnExportMid_);

    dragToDawBadge_.onStartExternalMidiDrag = [this]() { startExternalMidiDragToDaw(); };
    addAndMakeVisible(dragToDawBadge_);

    // 4x3 Numeric Keypad (7,8,9 / 4,5,6 / 1,2,3 / 0,-1,+1)
    struct KeySpec {
        int logicalId; // 0..9 = digit, 10 = -1/NO, 11 = +1/YES
        const char* pri;
        uint32_t priCol;
        const char* sec;
        uint32_t secCol;
        const char* ter;
    };
    static constexpr std::array<KeySpec, 12> kKeyLayout = {{
        { 7,  "7",  tokens::kSilkWarmWhite,  "VWX", tokens::kSilkWarmWhite,  "TIE"     },
        { 8,  "8",  tokens::kSilkWarmWhite,  "YZ!", tokens::kSilkWarmWhite,  "."       },
        { 9,  "9",  tokens::kSilkWarmWhite,  "SPC", tokens::kSilkWarmWhite,  "-3-"     },
        { 4,  "4",  tokens::kSilkWarmWhite,  "MNO", tokens::kSilkWarmWhite,  "NOTE_16" },
        { 5,  "5",  tokens::kSilkWarmWhite,  "PQR", tokens::kSilkWarmWhite,  "NOTE_8"  },
        { 6,  "6",  tokens::kSilkWarmWhite,  "STU", tokens::kSilkWarmWhite,  "NOTE_4"  },
        { 1,  "1",  tokens::kSilkWarmWhite,  "DEF", tokens::kSilkWarmWhite,  ""        },
        { 2,  "2",  tokens::kSilkWarmWhite,  "GHI", tokens::kSilkWarmWhite,  ""        },
        { 3,  "3",  tokens::kSilkWarmWhite,  "JKL", tokens::kSilkWarmWhite,  ""        },
        { 0,  "0",  tokens::kSilkWarmWhite,  "ABC", tokens::kSilkWarmWhite,  "REST"    },
        { 10, "-1", tokens::kSilkOchreGold,  "NO",  tokens::kSilkOchreGold,  "DEC_KEY" },
        { 11, "+1", tokens::kSilkAccentTeal, "YES", tokens::kSilkAccentTeal, "INC_KEY" }
    }};

    for (std::size_t i = 0; i < 12u; ++i) {
        const auto& ks = kKeyLayout[i];
        auto kb = std::make_unique<VF50PanelButton>("Keypad_" + juce::String(ks.pri),
                                                   VF50PanelButton::CapStyle::KeypadRect);
        kb->setCapLegends(ks.pri, ks.priCol, ks.sec, ks.secCol, ks.ter, tokens::kSilkAccentTeal);
        kb->onClick = [this, id = ks.logicalId]() {
            if (id < 10) {
                handleKeypadDigit(id);
            } else if (id == 10) {
                pressKeypadNoButton();
            } else {
                pressKeypadYesButton();
            }
        };
        addAndMakeVisible(*kb);
        keypadBtns_[i] = std::move(kb);
    }
}

void VF50MainCanvas::pressKeypadNoButton() {
    if (classicController_.usesOwnLcd()) {
        if (classicController_.onKeypadNo()) {
            syncMenuPageFromClassicController();
        }
        refreshDisplayAndLeds();
        return;
    }
    stepFocusedField(-1);
}

void VF50MainCanvas::pressKeypadYesButton() {
    if (classicController_.usesOwnLcd()) {
        if (classicController_.onKeypadYes()) {
            syncMenuPageFromClassicController();
        }
        refreshDisplayAndLeds();
        return;
    }
    stepFocusedField(+1);
}

void VF50MainCanvas::setupRightSectionControls() {
    // PLAY Group: PERFORMANCE & SINGLE
    btnPerformance_ = std::make_unique<VF50PanelButton>("PERFORMANCE");
    btnPerformance_->setCapLegends("PERFOR-", tokens::kSilkWarmWhite, "MANCE", tokens::kSilkWarmWhite);
    btnPerformance_->setFontScale(0.88f);
    btnPerformance_->setRightClickLatchEnabled(true);
    btnPerformance_->onPressDown = [this]() { holdPerformanceButton(true, false); };
    btnPerformance_->onReleaseUp = [this]() { holdPerformanceButton(false, false); };
    btnPerformance_->onHoldLatchChanged = [this](bool latched) { holdPerformanceButton(latched, true); };
    btnPerformance_->setTooltip("PERFORMANCE — Right-Click to hold + 3 digits (click again to send MIDI Program Change)");
    btnPerformance_->onClick = [this]() { handlePlayModeSelect(true); };
    addAndMakeVisible(*btnPerformance_);

    btnSingle_ = std::make_unique<VF50PanelButton>("SINGLE");
    btnSingle_->setCapLegends("SINGLE", tokens::kSilkAccentTeal);
    btnSingle_->setRightClickLatchEnabled(true);
    btnSingle_->onPressDown = [this]() { holdSingleButton(true, false); };
    btnSingle_->onReleaseUp = [this]() { holdSingleButton(false, false); };
    btnSingle_->onHoldLatchChanged = [this](bool latched) { holdSingleButton(latched, true); };
    btnSingle_->setTooltip("SINGLE — Right-Click to hold + 3 digits (click again to send MIDI Program Change)");
    btnSingle_->onClick = [this]() { handlePlayModeSelect(false); };
    addAndMakeVisible(*btnSingle_);

    // MEMORY Group: INT, CARD, PRESET
    btnMemInt_ = std::make_unique<VF50PanelButton>("MEM_INT");
    btnMemInt_->setCapLegends("INT", tokens::kSilkWarmWhite);
    btnMemInt_->onClick = [this]() { handleMemoryBankSelect(0); };
    addAndMakeVisible(*btnMemInt_);

    btnMemCard_ = std::make_unique<VF50PanelButton>("MEM_CARD");
    btnMemCard_->setCapLegends("CARD", tokens::kSilkWarmWhite);
    btnMemCard_->onClick = [this]() { handleMemoryBankSelect(1); };
    addAndMakeVisible(*btnMemCard_);

    btnMemPreset_ = std::make_unique<VF50PanelButton>("MEM_PRESET");
    btnMemPreset_->setCapLegends("PRESET", tokens::kSilkAccentTeal);
    btnMemPreset_->onClick = [this]() { handleMemoryBankSelect(2); };
    addAndMakeVisible(*btnMemPreset_);

    // EDIT 2x6 Matrix (Left Col 0..5: TR1..TR4, RHY, [blank]; Right Col 6..11: TR5..TR8, [blank], [blank])
    static constexpr std::array<const char*, 12> kEditCapLegends = {
        "TR 1", "TR 2", "TR 3", "TR 4", "RHY", "",
        "TR 5", "TR 6", "TR 7", "TR 8", "",    ""
    };
    for (int i = 0; i < 12; ++i) {
        auto eb = std::make_unique<VF50PanelButton>("Edit_" + juce::String(i));
        if (kEditCapLegends[static_cast<std::size_t>(i)][0] != '\0') {
            eb->setCapLegends(kEditCapLegends[static_cast<std::size_t>(i)], tokens::kSilkWarmWhite);
        }
        eb->onClick = [this, i]() { handleEditMatrixPress(i); };
        addAndMakeVisible(*eb);
        editBtns_[static_cast<std::size_t>(i)] = std::move(eb);
    }

    // UTILITY Column (CARD, MIDI, DISK, MEMORY PROTECT, SETUP, OTHERS)
    struct UtilSpec {
        const char* line1;
        uint32_t col1;
        const char* line2;
        uint32_t col2;
    };
    static constexpr std::array<UtilSpec, 6> kUtilSpecs = {{
        { "CARD",    tokens::kSilkWarmWhite,  "",        tokens::kSilkWarmWhite },
        { "MIDI",    tokens::kSilkWarmWhite,  "",        tokens::kSilkWarmWhite },
        { "DISK",    tokens::kSilkAccentTeal, "",        tokens::kSilkAccentTeal },
        { "MEMORY",  tokens::kSilkWarmWhite,  "PROTECT", tokens::kSilkWarmWhite },
        { "SETUP",   tokens::kSilkWarmWhite,  "",        tokens::kSilkWarmWhite },
        { "OTHERS",  tokens::kSilkWarmWhite,  "",        tokens::kSilkWarmWhite }
    }};
    for (int i = 0; i < 6; ++i) {
        const auto& us = kUtilSpecs[static_cast<std::size_t>(i)];
        auto ub = std::make_unique<VF50PanelButton>("Util_" + juce::String(i));
        ub->setCapLegends(us.line1, us.col1, us.line2, us.col2);
        if (i == 3) {
            ub->setFontScale(0.84f);
        }
        ub->onClick = [this, i]() { handleUtilityPress(i); };
        addAndMakeVisible(*ub);
        utilBtns_[static_cast<std::size_t>(i)] = std::move(ub);
    }

    // Far-Right Action Column: DEMO, STORE/COPY, COMPARE, EFFECT BYPASS
    btnDemo_ = std::make_unique<VF50PanelButton>("DEMO");
    btnDemo_->setCapLegends("DEMO", tokens::kSilkPlayGreen);
    btnDemo_->setRightClickLatchEnabled(true);
    btnDemo_->onPressDown = [this]() { demoBtnHeld_ = true; };
    btnDemo_->onReleaseUp = [this]() {
        if (!btnDemo_->isHoldLatched()) {
            demoBtnHeld_ = false;
        }
    };
    btnDemo_->onHoldLatchChanged = [this](bool latched) {
        demoBtnHeld_ = latched;
        refreshDisplayAndLeds();
    };
    btnDemo_->setTooltip("DEMO — Right-Click to hold + STORE/COPY to open UT EDIT MESSAGE");
    btnDemo_->onClick = [this]() { handleDemoToggle(); };
    addAndMakeVisible(*btnDemo_);

    btnStore_ = std::make_unique<VF50PanelButton>("STORE_COPY");
    btnStore_->setCapLegends("STORE", tokens::kSilkWarmWhite, "COPY", tokens::kSilkCrimsonBadge);
    btnStore_->setFontScale(0.92f);
    btnStore_->onClick = [this]() { handleStorePress(); };
    addAndMakeVisible(*btnStore_);

    btnCompare_ = std::make_unique<VF50PanelButton>("COMPARE");
    btnCompare_->setCapLegends("COMPARE", tokens::kSilkOchreGold);
    btnCompare_->setFontScale(0.86f);
    btnCompare_->onClick = [this]() { handleComparePress(); };
    addAndMakeVisible(*btnCompare_);

    btnEffectBypass_ = std::make_unique<VF50PanelButton>("EFFECT_BYPASS");
    btnEffectBypass_->setCapLegends("EFFECT", tokens::kSilkWarmWhite, "BYPASS", tokens::kSilkWarmWhite);
    btnEffectBypass_->setFontScale(0.88f);
    btnEffectBypass_->onClick = [this]() { handleEffectBypassToggle(); };
    addAndMakeVisible(*btnEffectBypass_);

    btnProgrammer_ = std::make_unique<VF50PanelButton>("PROGRAMMER_TOGGLE");
    btnProgrammer_->setCapLegends("PROGRAMMER", tokens::kSilkOchreGold);
    btnProgrammer_->setFontScale(0.64f);
    btnProgrammer_->onClick = [this]() { handleProgrammerVisibilityToggle(); };
    addAndMakeVisible(*btnProgrammer_);

    btnMixer_ = std::make_unique<VF50PanelButton>("MIXER_TOGGLE");
    btnMixer_->setCapLegends("MIXER", tokens::kMixerTealAccent);
    btnMixer_->setFontScale(0.82f);
    btnMixer_->setActivePillHighlight(mixerVisible_);
    btnMixer_->setTooltip("MIXER — Toggle Performance & Rhythm Visual Mixer drawer");
    btnMixer_->onClick = [this]() { handleMixerVisibilityToggle(); };
    addAndMakeVisible(*btnMixer_);

    btnKeyboard_ = std::make_unique<VF50PanelButton>("KEYBOARD_TOGGLE");
    btnKeyboard_->setCapLegends("KEYBOARD", tokens::kSilkAccentTeal);
    btnKeyboard_->setFontScale(0.75f);
    btnKeyboard_->onClick = [this]() { handleKeyboardVisibilityToggle(); };
    addAndMakeVisible(*btnKeyboard_);
}

void VF50MainCanvas::resized() {
    // 1. Header Virtual Floppy Slot (DISK IN / FOLDER / USER DISK / SET DIR / EJECT) + ROM Setup + UI Scale Pills
    if (btnFloppyInsert_ && btnFloppyFolder_ && btnFloppyUserDisk_ && btnFloppySetUserDir_ && btnFloppyEject_) {
        btnFloppyInsert_->setFontScale(0.80f);
        btnFloppyFolder_->setFontScale(0.80f);
        btnFloppyUserDisk_->setFontScale(0.74f);
        btnFloppySetUserDir_->setFontScale(0.74f);
        btnFloppyEject_->setFontScale(0.80f);
        btnFloppyInsert_->setBounds(1538, 30, 96, 44);
        btnFloppyFolder_->setBounds(1640, 30, 92, 44);
        btnFloppyUserDisk_->setBounds(1738, 30, 120, 44);
        btnFloppySetUserDir_->setBounds(1864, 30, 90, 44);
        btnFloppyEject_->setBounds(1960, 30, 86, 44);
    }
    if (btnRomLoader_) {
        btnRomLoader_->setFontScale(0.82f);
        btnRomLoader_->setBounds(2060, 30, 356, 44);
    }
    for (int i = 0; i < static_cast<int>(scalePills_.size()); ++i) {
        scalePills_[static_cast<std::size_t>(i)]->setBounds(2512 + i * 84, 32, 78, 40);
    }

    // 2. Left Column: Card Hotspot, Recessed Faders, Pitch & Mod Wheels
    cardSlotClickRect_ = toRect(tokens::kCardHousingRect2x);
    volumeFader_.setBounds(toRect(tokens::kVolumeFaderRect2x));
    dataEntryFader_.setBounds(toRect(tokens::kDataEntryFaderRect2x));
    pitchWheel_.setBounds(toRect(tokens::kPitchWheelRect2x));
    modWheel_.setBounds(toRect(tokens::kModWheelRect2x));

    // 3. Center Column: 40x2 Dot-Matrix LCD + 8 Soft Keys + SYSTEM + SEQUENCER + Export Strip + 4x3 Keypad
    lcd_.setBounds(toRect(tokens::kLcdBezelRect2x));
    textFocusProxy_.setBounds(262, 73, 2, 2);

    {
        const float lcdCharLeftX  = lcd_.paintedCharGridLeftXInParent();
        const float lcdCharRightX = lcd_.paintedCharGridRightXInParent();
        const float charCellWidth = (lcdCharRightX - lcdCharLeftX) / static_cast<float>(VF50LcdDisplay::kCols);
        const float softKeyW      = 4.0f * charCellWidth;
        const float softKeyGap    = (8.0f / 7.0f) * charCellWidth;
        const float softKeyStep   = softKeyW + softKeyGap;
        const int   softKeyWInt   = static_cast<int>(std::lround(softKeyW));
        const int   softKeyY2x    = tokens::kSoftKeyY1x * 2;
        const int   softKeyH2x    = tokens::kSoftKeyH1x * 2;
        const int   lcdRightInt   = static_cast<int>(std::lround(lcdCharRightX));

        for (int i = 0; i < 8; ++i) {
            const int bx = (i == 7)
                               ? (lcdRightInt - softKeyWInt)
                               : static_cast<int>(std::lround(lcdCharLeftX + static_cast<float>(i) * softKeyStep));
            softKeys_[static_cast<std::size_t>(i)]->setBounds(bx, softKeyY2x, softKeyWInt, softKeyH2x);
        }
    }

    btnSeq_->setBounds(toRect(tokens::kBtnSeqRect2x));
    btnRhythm_->setBounds(toRect(tokens::kBtnRhythmRect2x));
    btnJob_->setBounds(toRect(tokens::kBtnJobRect2x));
    btnInstMinus_->setBounds(toRect(tokens::kBtnMinusInstRect2x));

    for (int i = 0; i < 5; ++i) {
        const auto r = tokens::fromRef1x(tokens::kSeqRowStartX1x + i * tokens::kSeqRowStepX1x,
                                         tokens::kSeqRowY1x,
                                         tokens::kSeqBtnSize1x,
                                         42);
        seqOpBtns_[static_cast<std::size_t>(i)]->setBounds(toRect(r));
    }

    btnExportMid_->setBounds(toRect(tokens::kBtnExportMidRect2x));
    dragToDawBadge_.setBounds(toRect(tokens::kDragToDawBadgeRect2x));
    romGuideOverlay_.setBounds(80, 36, 2804, 808);

    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int idx = row * 3 + col;
            const auto r = tokens::fromRef1x(tokens::kKeypadStartX1x + col * tokens::kKeypadStepX1x,
                                             tokens::kKeypadStartY1x + row * tokens::kKeypadStepY1x,
                                             tokens::kKeypadBtnW1x,
                                             tokens::kKeypadBtnH1x);
            keypadBtns_[static_cast<std::size_t>(idx)]->setBounds(toRect(r));
        }
    }
    c3AuditionClickRect_ = toRect(tokens::kC3AuditionRect2x);

    // 4. Right Column: PLAY, MEMORY, EDIT 2x6 Matrix, UTILITY & DEMO/STORE/COMPARE/BYPASS/PROGRAMMER/KEYBOARD
    btnPerformance_->setBounds(toRect(tokens::kBtnPerformanceRect2x));
    btnSingle_->setBounds(toRect(tokens::kBtnSingleRect2x));
    btnMemInt_->setBounds(toRect(tokens::kBtnMemIntRect2x));
    btnMemCard_->setBounds(toRect(tokens::kBtnMemCardRect2x));
    btnMemPreset_->setBounds(toRect(tokens::kBtnMemPresetRect2x));

    for (int row = 0; row < 6; ++row) {
        const auto leftR = tokens::fromRef1x(tokens::kEditColLeftX1x,
                                             tokens::kEditStartY1x + row * tokens::kEditStepY1x,
                                             tokens::kEditBtnSize1x,
                                             tokens::kEditBtnSize1x);
        const auto rightR = tokens::fromRef1x(tokens::kEditColRightX1x,
                                              tokens::kEditStartY1x + row * tokens::kEditStepY1x,
                                              tokens::kEditBtnSize1x,
                                              tokens::kEditBtnSize1x);
        editBtns_[static_cast<std::size_t>(row)]->setBounds(toRect(leftR));
        editBtns_[static_cast<std::size_t>(6 + row)]->setBounds(toRect(rightR));
    }

    for (int row = 0; row < 6; ++row) {
        const auto ur = tokens::fromRef1x(tokens::kUtilColLeftX1x,
                                          tokens::kUtilStartY1x + row * tokens::kUtilStepY1x,
                                          tokens::kUtilBtnW1x,
                                          tokens::kUtilBtnH1x);
        utilBtns_[static_cast<std::size_t>(row)]->setBounds(toRect(ur));
    }

    btnDemo_->setBounds(toRect(tokens::fromRef1x(tokens::kUtilColRightX1x, 66, tokens::kUtilBtnW1x, tokens::kUtilBtnH1x)));
    btnStore_->setBounds(toRect(tokens::fromRef1x(tokens::kUtilColRightX1x, 210, tokens::kUtilBtnW1x, tokens::kUtilBtnH1x)));
    btnCompare_->setBounds(toRect(tokens::fromRef1x(tokens::kUtilColRightX1x, 258, tokens::kUtilBtnW1x, tokens::kUtilBtnH1x)));
    btnEffectBypass_->setBounds(toRect(tokens::fromRef1x(tokens::kUtilColRightX1x, 305, tokens::kUtilBtnW1x, tokens::kUtilBtnH1x)));
    if (btnMixer_) {
        btnMixer_->setBounds(toRect(tokens::kBtnMixerRect2x));
    }
    btnProgrammer_->setBounds(toRect(tokens::kBtnProgrammerRect2x));
    btnKeyboard_->setBounds(toRect(tokens::kBtnKeyboardRect2x));

    // 5. Pop-Down Drawers (Programmer / Visual Mixer) & Switchable 61-Key Virtual Keyboard
    int nextDrawerY = tokens::kVirtualHeight;
    programmerDrawer_.setBounds(0, nextDrawerY, tokens::kVirtualWidth, tokens::kVirtualProgrammerHeight);
    mixerDrawer_.setBounds(0, nextDrawerY, tokens::kVirtualWidth, tokens::kVirtualMixerHeight);
    if (programmerVisible_) {
        nextDrawerY += tokens::kVirtualProgrammerHeight;
    } else if (mixerVisible_) {
        nextDrawerY += tokens::kVirtualMixerHeight;
    }
    virtualKeyboard_.setBounds(0, nextDrawerY, tokens::kVirtualWidth, tokens::kVirtualKeyboardHeight);
}

void VF50MainCanvas::mouseDown(const juce::MouseEvent& event) {
    if (classicController_.isTextEntryScreenActive()) {
        textFocusProxy_.setWantsKeyboardFocus(true);
        if (isShowing() && getPeer() != nullptr) {
            textFocusProxy_.grabKeyboardFocus();
        }
    } else if (isShowing() && getPeer() != nullptr) {
        grabKeyboardFocus();
    }
    if (event.mods.isPopupMenu()) {
        return;
    }

    const auto pt = event.getPosition();
    if (cardSlotClickRect_.contains(pt)) {
        // Clicking the MCD64 RAM Card slot selects the CARD / User bank
        handleMemoryBankSelect(1);
        return;
    }

    if (c3AuditionClickRect_.contains(pt)) {
        // Toggle C3 reference audition note (Hardware C3 = MIDI 60)
        c3AuditionHeld_ = !c3AuditionHeld_;
        if (c3AuditionHeld_) {
            processor_.enqueueUiMidi3(0x90u, 60u, 108u);
            classicController_.notifySingleKeyActivity();
        } else {
            processor_.enqueueUiMidi3(0x80u, 60u, 0u);
        }
        repaint(c3AuditionClickRect_.expanded(8));
    }
}

bool VF50MainCanvas::handlePcTextEntryKeyPress(const juce::KeyPress& key) {
    if (!classicController_.isTextEntryScreenActive()) {
        syncTextEntryKeyboardFocus();
        return false;
    }

    bool changed = false;
    if (key == juce::KeyPress::backspaceKey) {
        changed = classicController_.onTextBackspace();
    } else if (key == juce::KeyPress::deleteKey) {
        changed = classicController_.onTextDelete();
    } else if (key == juce::KeyPress::leftKey) {
        changed = classicController_.onTextMoveCursor(-1);
    } else if (key == juce::KeyPress::rightKey) {
        changed = classicController_.onTextMoveCursor(+1);
    } else if (key == juce::KeyPress::returnKey) {
        changed = classicController_.onTextConfirmEnter();
    } else if (key == juce::KeyPress::escapeKey) {
        changed = classicController_.onTextCancelEscape();
    } else if (!key.getModifiers().isCommandDown() && !key.getModifiers().isAltDown()) {
        const auto ch = key.getTextCharacter();
        if (ch >= 0x20 && ch <= 0x7E) {
            changed = classicController_.onTextKeyPressed(static_cast<char>(ch));
        }
    }

    if (changed) {
        syncMenuPageFromClassicController();
        programmerDrawer_.syncFromProcessor();
    }
    refreshDisplayAndLeds();
    return true;
}

void VF50MainCanvas::syncTextEntryKeyboardFocus() {
    const bool active = classicController_.isTextEntryScreenActive();
    const bool changed = (active != textProxyFocusActive_);
    textProxyFocusActive_ = active;
    textFocusProxy_.setWantsKeyboardFocus(active);

    if (active) {
        if (isShowing() && getPeer() != nullptr
            && (changed || (hasKeyboardFocus(true) && !textFocusProxy_.hasKeyboardFocus(false)))) {
            textFocusProxy_.grabKeyboardFocus();
        }
    } else if (changed) {
        if (textFocusProxy_.hasKeyboardFocus(false)) {
            textFocusProxy_.giveAwayKeyboardFocus();
        }
        if (hasKeyboardFocus(true)) {
            juce::Component::unfocusAllComponents();
        }
    }
}

bool VF50MainCanvas::sendPcKeyPress(const juce::KeyPress& key) {
    return textFocusProxy_.keyPressed(key);
}

bool VF50MainCanvas::keyPressed(const juce::KeyPress& key) {
    if (classicController_.isTextEntryScreenActive()) {
        return handlePcTextEntryKeyPress(key);
    }

    if (classicController_.usesOwnLcd()) {
        if (key == juce::KeyPress::leftKey) {
            (void)classicController_.onStepValue(-1);
            syncMenuPageFromClassicController();
            refreshDisplayAndLeds();
            return true;
        }
        if (key == juce::KeyPress::rightKey) {
            (void)classicController_.onStepValue(+1);
            syncMenuPageFromClassicController();
            refreshDisplayAndLeds();
            return true;
        }
        if (key == juce::KeyPress::upKey || key.getTextCharacter() == '+' || key.getTextCharacter() == '=') {
            pressKeypadYesButton();
            return true;
        }
        if (key == juce::KeyPress::downKey || key.getTextCharacter() == '-') {
            pressKeypadNoButton();
            return true;
        }
        if (key == juce::KeyPress::returnKey) {
            pressKeypadYesButton();
            return true;
        }
        const auto ch = key.getTextCharacter();
        if (ch >= 0x20 && ch <= 0x7E && classicController_.onTextKeyPressed(static_cast<char>(ch))) {
            refreshDisplayAndLeds();
            return true;
        }
        if (ch >= '0' && ch <= '9') {
            handleKeypadDigit(static_cast<int>(ch - '0'));
            return true;
        }
    }

    if (key == juce::KeyPress::leftKey) {
        if (activeFieldCount_ > 0) {
            for (int step = 1; step <= activeFieldCount_; ++step) {
                const int cand = (focusedFieldIdx_ + activeFieldCount_ - step) % activeFieldCount_;
                if (pageFields_[static_cast<std::size_t>(cand)].valid) {
                    focusedFieldIdx_ = cand;
                    refreshDisplayAndLeds();
                    break;
                }
            }
            return true;
        }
    } else if (key == juce::KeyPress::rightKey) {
        if (activeFieldCount_ > 0) {
            for (int step = 1; step <= activeFieldCount_; ++step) {
                const int cand = (focusedFieldIdx_ + step) % activeFieldCount_;
                if (pageFields_[static_cast<std::size_t>(cand)].valid) {
                    focusedFieldIdx_ = cand;
                    refreshDisplayAndLeds();
                    break;
                }
            }
            return true;
        }
    } else if (key == juce::KeyPress::upKey || key.getTextCharacter() == '+' || key.getTextCharacter() == '=') {
        stepFocusedField(+1);
        return true;
    } else if (key == juce::KeyPress::downKey || key.getTextCharacter() == '-') {
        stepFocusedField(-1);
        return true;
    } else if (key == juce::KeyPress::returnKey) {
        if (page_ == MenuPage::StoreConfirm) {
            handleStorePress();
            return true;
        }
    }
    return false;
}

void VF50MainCanvas::focusLost(FocusChangeType /*cause*/) {
    if (!hasKeyboardFocus(true)) {
        clearAllHoldLatches(false);
    }
}

void VF50MainCanvas::clearAllHoldLatches(bool preserveDirectPc) {
    if (seqOpBtns_[2]) {
        seqOpBtns_[2]->setHoldLatched(false, false);
    }
    if (btnInstMinus_) {
        btnInstMinus_->setHoldLatched(false, false);
    }
    classicController_.setInstMinusArmed(false);
    if (btnDemo_) {
        btnDemo_->setHoldLatched(false, false);
    }
    demoBtnHeld_ = false;
    if (btnRhythm_) {
        btnRhythm_->setHoldLatched(false, false);
    }
    if (!preserveDirectPc) {
        if (btnPerformance_) {
            btnPerformance_->setHoldLatched(false, false);
        }
        if (btnSingle_) {
            btnSingle_->setHoldLatched(false, false);
        }
        classicController_.cancelDirectPcQuietly();
    }
    for (auto& sk : softKeys_) {
        if (sk) {
            sk->setHoldLatched(false, false);
        }
    }
    classicController_.setHeldRhythmModifiers(isEraseModifierActive(), isStepInstModifierActive());
}

bool VF50MainCanvas::isEraseModifierActive() const noexcept {
    return btnInstMinus_ != nullptr
        && (btnInstMinus_->isMouseButtonDown() || btnInstMinus_->isHoldLatched());
}

bool VF50MainCanvas::isStepInstModifierActive() const noexcept {
    const bool f3 = (softKeys_[2] != nullptr)
                 && (softKeys_[2]->isMouseButtonDown() || softKeys_[2]->isHoldLatched());
    const bool f4 = (softKeys_[3] != nullptr)
                 && (softKeys_[3]->isMouseButtonDown() || softKeys_[3]->isHoldLatched());
    return f3 || f4;
}

void VF50MainCanvas::releaseStopLatchIfSet() {
    if (seqOpBtns_[2] != nullptr && seqOpBtns_[2]->isHoldLatched()) {
        seqOpBtns_[2]->setHoldLatched(false, false);
    }
}

void VF50MainCanvas::pressSeqButton() {
    clearAllHoldLatches(false);
    if (classicController_.onPressSeq()) {
        page_ = MenuPage::SequencerDemo;
        syncStateMachineFromMenuPage();
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::pressInstMinusButton() {
    (void)classicController_.onPressInstMinus();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::pressStopAndBwdTogether() {
    releaseStopLatchIfSet();
    (void)classicController_.onPressStopAndBwdTogether();
    syncMenuPageFromClassicController();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::pressRhythmButton() {
    clearAllHoldLatches(false);
    if (classicController_.onPressRhythm()) {
        syncMenuPageFromClassicController();
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleJobPress() {
    instOnOffSoftMode_ = false;
    if (classicController_.onPressJob()) {
        syncMenuPageFromClassicController();
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::syncStateMachineFromMenuPage() noexcept {
    auto& sm = processor_.stateMachine();
    using vf50::state::ClassicTopMode;
    using vf50::state::SequencerSubPage;

    if (page_ <= MenuPage::SingleFunction || page_ == MenuPage::EffectEdit) {
        sm.setClassicTopMode(ClassicTopMode::MODE_VOICE);
    } else if (page_ >= MenuPage::PerfPlay && page_ <= MenuPage::PerfOthers) {
        sm.setClassicTopMode(ClassicTopMode::MODE_PERFORMANCE);
    } else if (page_ == MenuPage::RhythmPlayEdit || page_ == MenuPage::RhythmJob) {
        sm.setClassicTopMode(ClassicTopMode::MODE_RHYTHM);
        sm.setRhythmJobNumber(rhythmJobNumber_);
    } else if (page_ == MenuPage::SequencerDemo) {
        sm.setClassicTopMode(ClassicTopMode::MODE_SEQUENCER);
        sm.setSequencerSubPage(SequencerSubPage::SongPlayRec);
    } else if (page_ == MenuPage::SequencerTrackSetup) {
        sm.setClassicTopMode(ClassicTopMode::MODE_SEQUENCER);
        sm.setSequencerSubPage(SequencerSubPage::TrackSetup);
        sm.setSelectedSequencerTrack(selectedSeqTrack_);
    } else if (page_ != MenuPage::StoreConfirm) {
        sm.setClassicTopMode(ClassicTopMode::MODE_UTILITY);
        sm.setUtilitySetupPage(utilSetupSubPage_);
    }
}

void VF50MainCanvas::syncMenuPageFromClassicController() noexcept {
    using vf50::classic::ClassicScreen;
    switch (classicController_.screen()) {
        case ClassicScreen::PowerOnGreeting:
        case ClassicScreen::PlaySingle:
            page_ = MenuPage::SinglePlay;
            break;
        case ClassicScreen::PlayPerformance:
            page_ = MenuPage::PerfPlay;
            break;
        case ClassicScreen::SeqPlaySong:
        case ClassicScreen::SeqSongSetupPrompt:
        case ClassicScreen::SeqStoreSetupConfirm:
        case ClassicScreen::SeqJobSelect:
        case ClassicScreen::SeqJobSetup:
        case ClassicScreen::SeqJobTransmitCh:
        case ClassicScreen::SeqJobSong:
        case ClassicScreen::SeqJobEditMenu:
        case ClassicScreen::SeqJobEditSub:
        case ClassicScreen::SeqJobRecMode:
        case ClassicScreen::SeqRecStandby:
        case ClassicScreen::SeqStepBarGraph:
            page_ = MenuPage::SequencerDemo;
            break;
        case ClassicScreen::DiskRoot:
        case ClassicScreen::DiskTypeSelect:
        case ClassicScreen::DiskFileAction:
        case ClassicScreen::DiskMdrMenu:
        case ClassicScreen::DiskMdrIntTime:
        case ClassicScreen::DiskDirectory:
        case ClassicScreen::DiskJobMenu:
        case ClassicScreen::DiskStatus:
        case ClassicScreen::DiskBackupPrompt:
            page_ = MenuPage::UtilDisk;
            break;
        case ClassicScreen::UtilMemoryProtect:
            page_ = MenuPage::UtilMemProtect;
            break;
        case ClassicScreen::PerfEditAssignMode:  page_ = MenuPage::PerfAssignMode; break;
        case ClassicScreen::PerfEditNotes:       page_ = MenuPage::PerfNotes; break;
        case ClassicScreen::PerfEditVoiceNo:     page_ = MenuPage::PerfVoiceNumber; break;
        case ClassicScreen::PerfEditRecvCh:      page_ = MenuPage::PerfRecvCh; break;
        case ClassicScreen::PerfEditLimitLow:    page_ = MenuPage::PerfLimitLow; break;
        case ClassicScreen::PerfEditLimitHigh:   page_ = MenuPage::PerfLimitHigh; break;
        case ClassicScreen::PerfEditDetune:      page_ = MenuPage::PerfDetune; break;
        case ClassicScreen::PerfEditNoteShift:   page_ = MenuPage::PerfNoteShift; break;
        case ClassicScreen::PerfEditVolume:      page_ = MenuPage::PerfVolumePan; break;
        case ClassicScreen::PerfEditOutAssign:   page_ = MenuPage::PerfOutAssign; break;
        case ClassicScreen::PerfEditOthers:      page_ = MenuPage::PerfOthers; break;
        case ClassicScreen::PerfEditEffect:
        case ClassicScreen::VoiceEditEffect:     page_ = MenuPage::EffectEdit; break;
        case ClassicScreen::VoiceEditQuick:      page_ = MenuPage::SingleQuickEdit; break;
        case ClassicScreen::VoiceEditAlgorithm:  page_ = MenuPage::SingleAlgorithm; break;
        case ClassicScreen::VoiceEditLfo:        page_ = MenuPage::SingleLfo; break;
        case ClassicScreen::VoiceEditSens:       page_ = MenuPage::SingleSensitivity; break;
        case ClassicScreen::VoiceEditOsc:        page_ = MenuPage::SingleOscillator; break;
        case ClassicScreen::VoiceEditEg:         page_ = MenuPage::SingleEg; break;
        case ClassicScreen::VoiceEditPeg:        page_ = MenuPage::SinglePeg; break;
        case ClassicScreen::VoiceEditOutLevel:   page_ = MenuPage::SingleOutLevel; break;
        case ClassicScreen::VoiceEditScaling:    page_ = MenuPage::SingleScaling; break;
        case ClassicScreen::VoiceEditTranspose:  page_ = MenuPage::SingleTranspose; break;
        case ClassicScreen::VoiceEditFuncRoot:
        case ClassicScreen::VoiceEditFuncPbEtc:
        case ClassicScreen::VoiceEditFuncPort:
        case ClassicScreen::VoiceEditFuncFc:
        case ClassicScreen::VoiceEditFuncMw:
        case ClassicScreen::VoiceEditFuncBc:
        case ClassicScreen::VoiceEditFuncAt:
        case ClassicScreen::VoiceEditFuncRevr:
        case ClassicScreen::VoiceEditFuncName:   page_ = MenuPage::SingleFunction; break;
        case ClassicScreen::UtilOthersVoice:
        case ClassicScreen::UtilOthersPerf:
        case ClassicScreen::UtilOthersPerfInit:
        case ClassicScreen::UtilOthersPresetLoad:
        case ClassicScreen::UtilOthersMultiVoiceSelect:
            page_ = MenuPage::UtilOthers;
            break;
        case ClassicScreen::StoreVoice:
        case ClassicScreen::StorePerf:
        case ClassicScreen::StoreMultiVoice:
        case ClassicScreen::CopyEffect:
        case ClassicScreen::CopyPerfEffect:
        case ClassicScreen::CopyEg:
            page_ = MenuPage::StoreConfirm;
            break;
        case ClassicScreen::RhythmPtnPlay:
        case ClassicScreen::RhythmSongPlay:
        case ClassicScreen::RhythmPtnRecSetup:
        case ClassicScreen::RhythmPtnRecReal:
        case ClassicScreen::RhythmPtnRecStep:
        case ClassicScreen::RhythmPtnRecStore:
            page_ = MenuPage::RhythmPlayEdit;
            break;
        case ClassicScreen::RhythmPtnJobSelect:
        case ClassicScreen::RhythmPtnJobCopy:
        case ClassicScreen::RhythmPtnJobClear:
        case ClassicScreen::RhythmSetup:
        case ClassicScreen::RhythmInstSet:
        case ClassicScreen::RhythmKeyAssign:
        case ClassicScreen::RhythmSongJobSelect:
        case ClassicScreen::RhythmSongEdit:
        case ClassicScreen::RhythmSongEditJob:
        case ClassicScreen::RhythmSongJobCopy:
        case ClassicScreen::RhythmSongJobClear:
        case ClassicScreen::RhythmSongJobSearch:
            page_ = MenuPage::RhythmJob;
            break;
        case ClassicScreen::CardRoot:
        case ClassicScreen::CardFormat:
        case ClassicScreen::CardSaveCategory:
        case ClassicScreen::CardSaveSub:
        case ClassicScreen::CardLoadCategory:
        case ClassicScreen::CardLoadSub:
            page_ = MenuPage::UtilCard;
            break;
        case ClassicScreen::MidiRoot:
        case ClassicScreen::MidiChInfo:
        case ClassicScreen::MidiSwitch:
        case ClassicScreen::MidiCond:
        case ClassicScreen::MidiPgmCng:
        case ClassicScreen::MidiPgmInit:
        case ClassicScreen::MidiPgmEdit:
        case ClassicScreen::MidiExclusive:
        case ClassicScreen::MidiBulkRoot:
        case ClassicScreen::MidiBulkSub:
            page_ = MenuPage::UtilMidi;
            break;
        case ClassicScreen::SetupRoot:
        case ClassicScreen::SetupTune:
        case ClassicScreen::SetupCombine:
        case ClassicScreen::SetupCtrlReset:
        case ClassicScreen::SetupPerfEfctCat:
        case ClassicScreen::SetupPerfEfctSlot:
        case ClassicScreen::SetupDelayEdit:
        case ClassicScreen::SetupPanEdit:
        case ClassicScreen::SetupChordEdit:
        case ClassicScreen::SetupMicroRoot:
        case ClassicScreen::SetupMicroOctEdit:
        case ClassicScreen::SetupMicroOctInit:
        case ClassicScreen::SetupMicroFullEdit:
        case ClassicScreen::SetupMicroFullInit:
        case ClassicScreen::SetupVel:
        case ClassicScreen::SetupDamp:
        case ClassicScreen::UtilEditMessage:
            page_ = MenuPage::UtilSetup;
            break;
        case ClassicScreen::DemoRoot:
        case ClassicScreen::DemoConfirm:
        case ClassicScreen::DemoPlay:
        case ClassicScreen::DemoVoiceEdit:
            page_ = MenuPage::SequencerDemo;
            break;
        case ClassicScreen::DelegatedEdit:
            break;
    }
    const auto curScreen = classicController_.screen();
    if (curScreen != lastSyncedClassicScreen_) {
        const auto prevScreen = lastSyncedClassicScreen_;
        lastSyncedClassicScreen_ = curScreen;

        if (curScreen != ClassicScreen::RhythmPtnRecStep) {
            for (auto& sk : softKeys_) {
                if (sk) {
                    sk->setHoldLatched(false, false);
                }
            }
        }
        if (!vf50::classic::VF50ClassicController::isRhythmScreen(curScreen)) {
            if (btnRhythm_) {
                btnRhythm_->setHoldLatched(false, false);
            }
        }
        if (curScreen != ClassicScreen::RhythmPtnRecReal
            && curScreen != ClassicScreen::RhythmPtnRecStep
            && (prevScreen == ClassicScreen::RhythmPtnRecReal
                || prevScreen == ClassicScreen::RhythmPtnRecStep)) {
            if (btnInstMinus_) {
                btnInstMinus_->setHoldLatched(false, false);
            }
            classicController_.setInstMinusArmed(false);
        }
        if (curScreen != ClassicScreen::PlaySingle
            && curScreen != ClassicScreen::PlayPerformance
            && curScreen != ClassicScreen::MidiPgmEdit) {
            if (btnPerformance_) {
                btnPerformance_->setHoldLatched(false, false);
            }
            if (btnSingle_) {
                btnSingle_->setHoldLatched(false, false);
            }
            classicController_.cancelDirectPcQuietly();
        }
        classicController_.setHeldRhythmModifiers(isEraseModifierActive(), isStepInstModifierActive());
    }
    syncStateMachineFromMenuPage();
    syncTextEntryKeyboardFocus();
}

bool VF50MainCanvas::isPerformanceContextForPage() const noexcept {
    if (page_ >= MenuPage::SinglePlay && page_ <= MenuPage::SingleFunction) {
        return false;
    }
    if (page_ >= MenuPage::PerfPlay && page_ <= MenuPage::PerfOthers) {
        return true;
    }
    return processor_.synthEngine().playMode() == vf50::dsp::fm::SynthPlayMode::Performance;
}

void VF50MainCanvas::selectMenuPage(MenuPage newPage) {
    diskStatusTicks_ = 0u;
    if (page_ != newPage) {
        if (page_ == MenuPage::StoreConfirm && newPage != MenuPage::StoreConfirm) {
            processor_.stateMachine().cancelPendingStore();
        }
        page_ = newPage;
        focusedFieldIdx_ = 0;
        pendingTensDigit_ = -1;
    }
    {
        using vf50::classic::ClassicScreen;
        if (newPage == MenuPage::SinglePlay) {
            classicController_.enterScreen(ClassicScreen::PlaySingle);
        } else if (newPage == MenuPage::SingleAlgorithm) {
            classicController_.enterScreen(ClassicScreen::VoiceEditAlgorithm);
        } else if (newPage == MenuPage::PerfPlay) {
            classicController_.enterScreen(ClassicScreen::PlayPerformance);
        } else if (newPage == MenuPage::SequencerDemo) {
            classicController_.enterScreen(ClassicScreen::SeqPlaySong);
        } else if (newPage == MenuPage::SequencerTrackSetup) {
            classicController_.enterScreen(ClassicScreen::SeqJobTransmitCh);
        } else if (newPage == MenuPage::RhythmPlayEdit) {
            (void)classicController_.onPressRhythm();
        } else if (newPage == MenuPage::UtilDisk) {
            classicController_.enterScreen(ClassicScreen::DiskRoot);
        } else if (newPage == MenuPage::UtilMemProtect) {
            classicController_.enterScreen(ClassicScreen::UtilMemoryProtect);
        } else {
            classicController_.enterScreen(ClassicScreen::DelegatedEdit);
        }
    }
    syncStateMachineFromMenuPage();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::holdPerformanceButton(bool heldDown, bool fromLatch) {
    if (heldDown) {
        if (btnSingle_ && btnSingle_->isHoldLatched()) {
            btnSingle_->setHoldLatched(false, false);
        }
        (void)classicController_.onHoldPlayModeKey(true, true);
        refreshDisplayAndLeds();
        return;
    }
    if (classicController_.isDirectPcActive()) {
        if (!fromLatch) {
            suppressNextPlayModeClick_ = true;
        }
        (void)classicController_.onHoldPlayModeKey(true, false);
        syncMenuPageFromClassicController();
        refreshDisplayAndLeds();
    } else if (classicController_.screen() == vf50::classic::ClassicScreen::MidiPgmEdit) {
        (void)classicController_.onHoldPlayModeKey(true, false);
        refreshDisplayAndLeds();
    }
}

void VF50MainCanvas::holdSingleButton(bool heldDown, bool fromLatch) {
    if (heldDown) {
        if (btnPerformance_ && btnPerformance_->isHoldLatched()) {
            btnPerformance_->setHoldLatched(false, false);
        }
        (void)classicController_.onHoldPlayModeKey(false, true);
        refreshDisplayAndLeds();
        return;
    }
    if (classicController_.isDirectPcActive()) {
        if (!fromLatch) {
            suppressNextPlayModeClick_ = true;
        }
        (void)classicController_.onHoldPlayModeKey(false, false);
        syncMenuPageFromClassicController();
        refreshDisplayAndLeds();
    }
}

void VF50MainCanvas::pressDemoAndStoreTogether() {
    demoBtnHeld_ = true;
    handleStorePress();
    demoBtnHeld_ = false;
}

void VF50MainCanvas::handlePlayModeSelect(bool performanceMode) {
    if (suppressNextPlayModeClick_) {
        suppressNextPlayModeClick_ = false;
        return;
    }
    clearAllHoldLatches(false);
    auto& apvts = processor_.apvts();
    instOnOffSoftMode_ = false;
    pendingTensDigit_  = -1;

    const bool allowed = performanceMode ? classicController_.onPressPerformance()
                                         : classicController_.onPressSingle();
    if (!allowed) {
        refreshDisplayAndLeds();
        return;
    }

    if (performanceMode) {
        vf50::params::setApvtsRawValue(apvts, ParamIndex::PlayMode, 1.0f);
        vf50::params::setApvtsRawValue(apvts, ParamIndex::WorkstationMode,
                                      static_cast<float>(vf50::state::WorkstationMode::PerformancePlay));
        processor_.syncApvtsToActivePerformance(true);
        selectMenuPage(MenuPage::PerfPlay);
    } else {
        vf50::params::setApvtsRawValue(apvts, ParamIndex::PlayMode, 0.0f);
        vf50::params::setApvtsRawValue(apvts, ParamIndex::WorkstationMode,
                                      static_cast<float>(vf50::state::WorkstationMode::SinglePlay));
        processor_.syncApvtsToActiveSingleVoice(true);
        selectMenuPage(MenuPage::SinglePlay);
    }
}

void VF50MainCanvas::handleMemoryBankSelect(int bankButtonIdx) {
    if (classicController_.usesOwnLcd()) {
        if (classicController_.onPressMemBank(bankButtonIdx)) {
            if (classicController_.screen() == vf50::classic::ClassicScreen::PlaySingle
                || classicController_.screen() == vf50::classic::ClassicScreen::PlayPerformance) {
                cardBankVisualActive_ = (bankButtonIdx == 1);
            }
        }
        programmerDrawer_.syncFromProcessor();
        refreshDisplayAndLeds();
        return;
    }
    programmerDrawer_.syncFromProcessor();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleEditMatrixPress(int index0To11) {
    const int idx = std::clamp(index0To11, 0, 11);
    instOnOffSoftMode_ = false;

    const bool minusWasLatched = (btnInstMinus_ != nullptr && btnInstMinus_->isHoldLatched());
    const bool isTrackOrRhyKey = (idx >= 0 && idx <= 4) || (idx >= 6 && idx <= 9);
    if (minusWasLatched && isTrackOrRhyKey) {
        classicController_.setInstMinusArmed(true);
    }

    if (classicController_.usesOwnLcd()) {
        if (classicController_.onPressEditMatrixKey(idx)) {
            if (idx <= 3) selectedSeqTrack_ = idx;
            else if (idx >= 6 && idx <= 9) selectedSeqTrack_ = (idx - 6) + 4;
            selectedPerfPart_ = classicController_.selectedPerfEditInstrument();
            selectedOperator_ = classicController_.selectedOperator0To3();
            syncMenuPageFromClassicController();
        }
        if (minusWasLatched && isTrackOrRhyKey && btnInstMinus_ != nullptr) {
            btnInstMinus_->setHoldLatched(false, false);
            classicController_.setInstMinusArmed(false);
        }
        refreshDisplayAndLeds();
        return;
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleUtilityPress(int index0To5) {
    clearAllHoldLatches(false);
    const int idx = std::clamp(index0To5, 0, 5);
    switch (idx) {
        case 0: // [CARD] -> UT CARD)
            (void)classicController_.onPressCard();
            break;
        case 1: // [MIDI] -> UT MIDI)
            (void)classicController_.onPressMidi();
            break;
        case 2: // [DISK] -> pure 40x2 LCD Disk softkey workflow (never OS popup)
            (void)classicController_.onPressDisk();
            break;
        case 3: // [MEMORY PROTECT] -> UT MEMORY PROTECT)
            (void)classicController_.onPressMemoryProtect();
            break;
        case 4: // [SETUP] -> UT SETUP)
            (void)classicController_.onPressSetup();
            break;
        case 5: // [OTHERS] -> UT OTHERS> VOICE / PFM
            (void)classicController_.onPressUtilityOthers();
            break;
        default:
            break;
    }
    syncMenuPageFromClassicController();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleSeqOpRowPress(int index0To4) {
    const bool bwdHeld  = (seqOpBtns_[1] != nullptr && seqOpBtns_[1]->isMouseButtonDown());
    const bool stopHeld = (seqOpBtns_[2] != nullptr && (seqOpBtns_[2]->isMouseButtonDown() || seqOpBtns_[2]->isHoldLatched()));
    const bool stopBwdChord = (index0To4 == 2 && bwdHeld) || (index0To4 == 1 && stopHeld);
    if (stopBwdChord) {
        releaseStopLatchIfSet();
        pressStopAndBwdTogether();
        return;
    }
    if (classicController_.onPressSeqOp(index0To4)) {
        selectedOperator_ = classicController_.selectedOperator0To3();
        syncMenuPageFromClassicController();
        programmerDrawer_.syncFromProcessor();
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleKeypadDigit(int digit0To9) {
    const bool minusWasLatched = (btnInstMinus_ != nullptr && btnInstMinus_->isHoldLatched());
    if (minusWasLatched && !classicController_.isDirectPcActive() && (digit0To9 == 7 || digit0To9 == 8)) {
        classicController_.setInstMinusArmed(true);
    }

    if (classicController_.usesOwnLcd()) {
        if (classicController_.onKeypadDigit(digit0To9)) {
            syncMenuPageFromClassicController();
            programmerDrawer_.syncFromProcessor();
        }
        if (minusWasLatched && !classicController_.isDirectPcActive()
            && (digit0To9 == 7 || digit0To9 == 8) && btnInstMinus_ != nullptr) {
            btnInstMinus_->setHoldLatched(false, false);
            classicController_.setInstMinusArmed(false);
        }
        refreshDisplayAndLeds();
        return;
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::pressSoftKey(int index0To7) {
    const int slot = std::clamp(index0To7, 0, 7);

    if (classicController_.usesOwnLcd()) {
        (void)classicController_.onPressSoftKey(slot);
        syncMenuPageFromClassicController();
        programmerDrawer_.syncFromProcessor();
        refreshDisplayAndLeds();
        return;
    }

    if (slot < activeFieldCount_ && pageFields_[static_cast<std::size_t>(slot)].valid) {
        focusedFieldIdx_ = slot;
    } else if (activeFieldCount_ > 0) {
        focusedFieldIdx_ = std::min(slot, activeFieldCount_ - 1);
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::stepFocusedField(int deltaSteps) {
    if (classicController_.usesOwnLcd()) {
        if (classicController_.onStepValue(deltaSteps)) {
            syncMenuPageFromClassicController();
            programmerDrawer_.syncFromProcessor();
            refreshDisplayAndLeds();
        }
        return;
    }
    if (deltaSteps == 0 || activeFieldCount_ <= 0) {
        return;
    }
    diskStatusTicks_ = 0u;
    const int idx = std::clamp(focusedFieldIdx_, 0, activeFieldCount_ - 1);
    const auto& fld = pageFields_[static_cast<std::size_t>(idx)];
    if (!fld.valid || !fld.readValue || !fld.writeValue) {
        return;
    }
    const float cur = fld.readValue();
    const float next = std::clamp(cur + static_cast<float>(deltaSteps) * fld.stepVal, fld.minVal, fld.maxVal);
    fld.writeValue(next);
    programmerDrawer_.syncFromProcessor();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::setDataEntryNormalized(float norm0To1) {
    if (classicController_.usesOwnLcd()) {
        if (btnRhythm_ != nullptr && (btnRhythm_->isMouseButtonDown() || btnRhythm_->isHoldLatched())
            && vf50::classic::VF50ClassicController::isRhythmScreen(classicController_.screen())) {
            classicController_.setRhythmTempoNormalized(norm0To1);
        } else {
            classicController_.onDataEntryNormalized(norm0To1);
        }
        syncMenuPageFromClassicController();
        programmerDrawer_.syncFromProcessor();
        refreshDisplayAndLeds();
        return;
    }
    if (activeFieldCount_ <= 0) {
        return;
    }
    diskStatusTicks_ = 0u;
    const int idx = std::clamp(focusedFieldIdx_, 0, activeFieldCount_ - 1);
    const auto& fld = pageFields_[static_cast<std::size_t>(idx)];
    if (!fld.valid || !fld.writeValue) {
        return;
    }
    const float clampedNorm = std::clamp(norm0To1, 0.0f, 1.0f);
    const float raw = fld.minVal + clampedNorm * (fld.maxVal - fld.minVal);
    const float snapped = (fld.stepVal >= 1.0f)
                              ? (std::round((raw - fld.minVal) / fld.stepVal) * fld.stepVal + fld.minVal)
                              : raw;
    fld.writeValue(std::clamp(snapped, fld.minVal, fld.maxVal));
    programmerDrawer_.syncFromProcessor();
    rebuildPageModelAndLcd();
    repaint(0, 0, tokens::kVirtualWidth, tokens::kHeaderContourGrooveY2x);
}

void VF50MainCanvas::syncDataEntrySliderFromFocusedField() {
    if (activeFieldCount_ <= 0) {
        return;
    }
    const int idx = std::clamp(focusedFieldIdx_, 0, activeFieldCount_ - 1);
    const auto& fld = pageFields_[static_cast<std::size_t>(idx)];
    if (!fld.valid || !fld.readValue || fld.maxVal <= fld.minVal) {
        return;
    }
    const float norm = std::clamp((fld.readValue() - fld.minVal) / (fld.maxVal - fld.minVal), 0.0f, 1.0f);
    suppressDataSliderCallback_ = true;
    dataEntryFader_.setValue(static_cast<double>(norm), juce::dontSendNotification);
    suppressDataSliderCallback_ = false;
}

void VF50MainCanvas::handleDemoToggle() {
    clearAllHoldLatches(false);
    instOnOffSoftMode_ = false;
    pendingTensDigit_  = -1;
    diskStatusTicks_   = 0u;

    (void)classicController_.onPressDemo();
    syncMenuPageFromClassicController();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleProgrammerVisibilityToggle() {
    const bool nextVisible = !programmerVisible_;
    setProgrammerVisible(nextVisible);
    processor_.setProgrammerVisible(nextVisible);
    if (onRequestProgrammerVisibilityChange) {
        onRequestProgrammerVisibilityChange(nextVisible);
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleMixerVisibilityToggle() {
    const bool nextVisible = !mixerVisible_;
    setMixerVisible(nextVisible);
    processor_.setMixerVisible(nextVisible);
    if (onRequestMixerVisibilityChange) {
        onRequestMixerVisibilityChange(nextVisible);
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleKeyboardVisibilityToggle() {
    const bool nextVisible = !virtualKeyboardVisible_;
    setVirtualKeyboardVisible(nextVisible);
    processor_.setVirtualKeyboardVisible(nextVisible);
    if (onRequestKeyboardVisibilityChange) {
        onRequestKeyboardVisibilityChange(nextVisible);
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleProgrammerParamAdjusted(vf50::params::ParamIndex pIdx, float /*newVal*/) {
    diskStatusTicks_ = 0u;

    // Respect Classic [COMPARE] lock & wire PG slider directly into Classic VMEM + 40x2 LCD
    if (!classicController_.onProgrammerVoiceParamEdited(pIdx)) {
        programmerDrawer_.syncFromProcessor();
        refreshDisplayAndLeds();
        return;
    }

    (void)processor_.applyHostProgrammerVoiceParam(pIdx);
    selectedOperator_ = classicController_.selectedOperator0To3();
    syncMenuPageFromClassicController();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::setRomGuideVisible(bool visible) {
    romGuideOverlay_.refreshState();
    romGuideOverlay_.setVisible(visible);
    if (visible) {
        romGuideOverlay_.toFront(false);
    }
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleFileDropped(const juce::File& file) {
    if (!file.exists()) {
        return;
    }
    handleFilesDropped(juce::StringArray{ file.getFullPathName() });
}

void VF50MainCanvas::handleFilesDropped(const juce::StringArray& files) {
    if (files.isEmpty()) {
        return;
    }

    bool hasRomCandidate = false;
    bool hasExplicitRomExt = false;
    for (const auto& path : files) {
        const juce::File f(path);
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext == ".bin" || ext == ".rom") {
            hasRomCandidate = true;
            hasExplicitRomExt = true;
            break;
        }
        if (f.isDirectory() &&
            !f.findChildFiles(juce::File::findFiles, false, "*.bin;*.BIN;*.rom;*.ROM").isEmpty()) {
            hasRomCandidate = true;
        }
    }

    if (hasRomCandidate) {
        juce::String status;
        const int loadedCount = processor_.loadRomFiles(files, status, true);
        if (loadedCount > 0 || hasExplicitRomExt) {
            romGuideOverlay_.setStatusBanner(status);
            romGuideOverlay_.refreshState();
            programmerDrawer_.syncFromProcessor();
            syncStateMachineFromMenuPage();
            diskStatusMsg_   = status.substring(0, 40);
            diskStatusTicks_ = 120u;
            refreshDisplayAndLeds();
            return;
        }
    }

    // Drag-and-drop .IMG/.DSK/.FLP/.IMA floppy images or disk folders ONLY inserts them into
    // the virtual floppy drive; all browsing/loading happens via the authentic 40x2 LCD DISK menu.
    const juce::File file(files[0]);
    if (!file.exists()) {
        return;
    }
    juce::String status;
    if (file.isDirectory()) {
        (void)diskManager_.mountDirectory(file, status);
    } else {
        const auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".img" || ext == ".dsk" || ext == ".flp" || ext == ".ima") {
            (void)diskManager_.mountFat12ImageFile(file, status);
        } else if (ext == ".syx") {
            (void)diskManager_.loadFile(file, processor_, status);
            programmerDrawer_.syncFromProcessor();
        } else {
            status = "DROP .IMG/.DSK OR FOLDER TO MOUNT";
        }
    }
    diskStatusMsg_   = status;
    diskStatusTicks_ = 90u;
    refreshDisplayAndLeds();
}

bool VF50MainCanvas::exportMidiToFile(const juce::File& destFile, juce::String& outStatus) {
    vf50::disk::MidiExportSummary summary{};
    const bool ok = vf50::disk::VF50MidiExporter::exportActiveSongToFile(processor_, destFile, &summary);
    outStatus        = summary.statusMessage;
    diskStatusMsg_   = outStatus;
    diskStatusTicks_ = 90u;
    refreshDisplayAndLeds();
    return ok;
}

bool VF50MainCanvas::prepareDragToDawTempMidi(juce::File& outTempFile, juce::String& outStatus) {
    vf50::disk::MidiExportSummary summary{};
    const bool ok = vf50::disk::VF50MidiExporter::writeTempDragMidiFile(processor_, outTempFile, &summary);
    outStatus        = summary.statusMessage;
    diskStatusMsg_   = ok ? ("DRAG 8 TRKS: " + outTempFile.getFileName()) : outStatus;
    diskStatusTicks_ = 90u;
    refreshDisplayAndLeds();
    return ok;
}

void VF50MainCanvas::launchExportMidiDialog() {
    const auto rawTitle = processor_.activeDemoSongTitle();
    juce::String defaultStem(rawTitle.data(), rawTitle.size());
    defaultStem = defaultStem.trim();
    if (defaultStem.isEmpty()) {
        defaultStem = "VF50_Song";
    }
    for (const juce::juce_wchar badCh : { '/', '\\', ':', '*', '?', '"', '<', '>', '|' }) {
        defaultStem = defaultStem.replaceCharacter(badCh, '_');
    }

    const juce::File baseDir = diskManager_.mountedFileOrFolder().isDirectory()
                                   ? diskManager_.mountedFileOrFolder()
                                   : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    activeFileChooser_ = std::make_unique<juce::FileChooser>(
        "Export Victory VF50 8-Track + Rhythm Sequence as Standard MIDI File (.MID)",
        baseDir.getChildFile(defaultStem + ".mid"),
        "*.mid;*.MID");

    const int chooserFlags = juce::FileBrowserComponent::saveMode |
                             juce::FileBrowserComponent::canSelectFiles |
                             juce::FileBrowserComponent::warnAboutOverwriting;
    ++faceplatePopupSpawnCount_;
    juce::Component::SafePointer<VF50MainCanvas> safeThis(this);
    activeFileChooser_->launchAsync(chooserFlags, [safeThis](const juce::FileChooser& fc) {
        if (safeThis == nullptr) {
            return;
        }
        auto file = fc.getResult();
        if (file == juce::File{}) {
            return;
        }
        if (file.getFileExtension().isEmpty()) {
            file = file.withFileExtension(".mid");
        }
        juce::String status;
        (void)safeThis->exportMidiToFile(file, status);
    });
}

void VF50MainCanvas::startExternalMidiDragToDaw() {
    juce::File tempMidiFile;
    juce::String status;
    if (!prepareDragToDawTempMidi(tempMidiFile, status)) {
        return;
    }

    juce::StringArray filesToDrag;
    filesToDrag.add(tempMidiFile.getFullPathName());
    (void)juce::DragAndDropContainer::performExternalDragDropOfFiles(filesToDrag, false, &dragToDawBadge_);
}

void VF50MainCanvas::launchMountImageDialog() {
    activeFileChooser_ = std::make_unique<juce::FileChooser>(
        "Insert Virtual 3.5\" Floppy Disk Image (.IMG / .DSK)",
        diskManager_.mountedFileOrFolder(),
        "*.img;*.IMG;*.dsk;*.DSK;*.flp;*.FLP;*.ima;*.IMA");

    const int chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<VF50MainCanvas> safeThis(this);
    activeFileChooser_->launchAsync(chooserFlags, [safeThis](const juce::FileChooser& fc) {
        if (safeThis == nullptr) {
            return;
        }
        const auto file = fc.getResult();
        if (!file.existsAsFile()) {
            return;
        }
        juce::String status;
        (void)safeThis->diskManager_.mountFat12ImageFile(file, status);
        safeThis->diskStatusMsg_   = status;
        safeThis->diskStatusTicks_ = 90u;
        safeThis->refreshDisplayAndLeds();
    });
}

void VF50MainCanvas::launchMountFolderDialog() {
    activeFileChooser_ = std::make_unique<juce::FileChooser>(
        "Mount Local Folder Containing VF50 Floppy Files",
        diskManager_.mountedFileOrFolder());

    const int chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    juce::Component::SafePointer<VF50MainCanvas> safeThis(this);
    activeFileChooser_->launchAsync(chooserFlags, [safeThis](const juce::FileChooser& fc) {
        if (safeThis == nullptr) {
            return;
        }
        const auto dir = fc.getResult();
        if (!dir.isDirectory()) {
            return;
        }
        juce::String status;
        (void)safeThis->diskManager_.mountDirectory(dir, status);
        safeThis->diskStatusMsg_   = status;
        safeThis->diskStatusTicks_ = 90u;
        safeThis->refreshDisplayAndLeds();
    });
}

bool VF50MainCanvas::mountUserDiskButton() {
    juce::String status;
    const bool ok = diskManager_.mountUserDisk(status);
    diskStatusMsg_   = status;
    diskStatusTicks_ = 90u;
    refreshDisplayAndLeds();
    return ok;
}

void VF50MainCanvas::launchChangeUserDiskFolderDialog() {
    activeFileChooser_ = std::make_unique<juce::FileChooser>(
        "Choose Common User Disk Folder for Saved VF50 Files",
        diskManager_.userDiskDirectory());

    const int chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    juce::Component::SafePointer<VF50MainCanvas> safeThis(this);
    activeFileChooser_->launchAsync(chooserFlags, [safeThis](const juce::FileChooser& fc) {
        if (safeThis == nullptr) {
            return;
        }
        const auto dir = fc.getResult();
        if (!dir.isDirectory()) {
            return;
        }
        if (safeThis->diskManager_.setUserDiskDirectory(dir, true)) {
            juce::String status;
            (void)safeThis->diskManager_.mountUserDisk(status);
            safeThis->diskStatusMsg_   = "User Disk -> " + dir.getFileName();
            safeThis->diskStatusTicks_ = 90u;
            safeThis->refreshDisplayAndLeds();
        }
    });
}

bool VF50MainCanvas::loadDirectoryEntry(std::size_t entryIndex, juce::String& outStatus) {
    if (!diskManager_.isMounted() || diskManager_.entries().empty()) {
        outStatus        = "NO DISK/FILES MOUNTED";
        diskStatusMsg_   = outStatus;
        diskStatusTicks_ = 90u;
        refreshDisplayAndLeds();
        return false;
    }

    const std::size_t clampedIdx = std::min(entryIndex, diskManager_.entries().size() - 1u);
    const auto entryKind         = diskManager_.entries()[clampedIdx].kind;
    const auto dispatchFilter    = vf50::disk::VF50DiskManager::defaultLoadFilterForKind(entryKind);

    const bool ok = diskManager_.loadMountedEntry(clampedIdx, processor_, dispatchFilter, outStatus);
    if (ok) {
        programmerDrawer_.syncFromProcessor();
        syncMenuPageFromClassicController();
    }
    diskStatusMsg_   = outStatus;
    diskStatusTicks_ = 90u;
    refreshDisplayAndLeds();
    return ok;
}

void VF50MainCanvas::handleStorePress() {
    processor_.pullLatestAudioMemoryToHost();

    const bool demoHeld = demoBtnHeld_ || (btnDemo_ != nullptr && btnDemo_->isHoldLatched());
    if (demoHeld) {
        (void)classicController_.onPressDemoAndStore();
        if (btnDemo_ != nullptr && btnDemo_->isHoldLatched()) {
            btnDemo_->setHoldLatched(false, false);
        }
        demoBtnHeld_ = false;
    } else {
        (void)classicController_.onPressStore();
    }
    syncMenuPageFromClassicController();
    programmerDrawer_.syncFromProcessor();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleComparePress() {
    (void)classicController_.onPressCompare();
    syncMenuPageFromClassicController();
    compareBlinkTicks_   = 0u;
    compareBlinkVisible_ = true;
    programmerDrawer_.syncFromProcessor();
    refreshDisplayAndLeds();
}

void VF50MainCanvas::handleEffectBypassToggle() {
    const bool curBypass = processor_.parameterBank().effectBypass->load() >= 0.5f;
    vf50::params::setApvtsRawValue(processor_.apvts(), ParamIndex::EffectBypass, curBypass ? 0.0f : 1.0f);
    refreshDisplayAndLeds();
}

void VF50MainCanvas::syncDrumTriggerTelemetry() noexcept {
    const uint32_t seq = processor_.lastDrumTriggerSeq();
    if (seq != lastSeenDrumTrigSeq_) {
        lastSeenDrumTrigSeq_ = seq;
        lastStruckMidiNote_  = static_cast<int>(processor_.lastTriggeredDrumNote());
        lastStruckDrumIdx_   = processor_.lastTriggeredDrumIndex();
        lastStruckDrumVel_   = static_cast<int>(processor_.lastTriggeredDrumVelocity());
        if (lastStruckDrumIdx_ >= 0 && lastStruckDrumIdx_ < 61) {
            selectedDrumInst_ = lastStruckDrumIdx_;
        }
        if (processor_.activeWorkstationMode() == vf50::state::WorkstationMode::RhythmPlay &&
            page_ != MenuPage::RhythmPlayEdit && page_ != MenuPage::RhythmJob &&
            page_ != MenuPage::UtilSetup && page_ != MenuPage::UtilDisk) {
            page_ = MenuPage::RhythmPlayEdit;
        }
    }
}

void VF50MainCanvas::timerCallback() {
    lcd_.tickCursorBlink();

    // Sync restored UI scale, PG programmer drawer, virtual keyboard & LCD theme
    const uint32_t prefsEpoch = processor_.uiPreferencesEpoch();
    if (prefsEpoch != lastSeenPrefsEpoch_) {
        lastSeenPrefsEpoch_ = prefsEpoch;
        lcd_.setThemeIndex(processor_.lcdThemeIndex());
        const bool wantPg = processor_.isProgrammerVisible();
        const bool wantMx = processor_.isMixerVisible() && !wantPg;
        if (wantPg != programmerVisible_) {
            setProgrammerVisible(wantPg);
            if (onRequestProgrammerVisibilityChange) {
                onRequestProgrammerVisibilityChange(wantPg);
            }
        }
        if (wantMx != mixerVisible_) {
            setMixerVisible(wantMx);
            if (onRequestMixerVisibilityChange) {
                onRequestMixerVisibilityChange(wantMx);
            }
        }
        const bool wantKb = processor_.isVirtualKeyboardVisible();
        if (wantKb != virtualKeyboardVisible_) {
            setVirtualKeyboardVisible(wantKb);
            if (onRequestKeyboardVisibilityChange) {
                onRequestKeyboardVisibilityChange(wantKb);
            }
        }
        const bool isEditPg = (page_ != MenuPage::SinglePlay && page_ != MenuPage::PerfPlay && page_ != MenuPage::SequencerDemo);
        classicController_.syncFromWorkstationMode(processor_.hostMemory().workstationMode(), isEditPg);
        const float wantScale = processor_.uiScaleFactor();
        if (std::abs(wantScale - currentScaleFactor_) > 0.01f && onRequestScaleChange) {
            onRequestScaleChange(wantScale);
        }
    }

    // Bi-directional sync with Pop-Down PG Programmer Drawer & external ROM state
    const uint32_t romEpoch = processor_.romEpoch();
    if (romEpoch != lastSeenRomEpoch_) {
        lastSeenRomEpoch_ = romEpoch;
        romGuideOverlay_.refreshState();
        refreshDisplayAndLeds();
    }

    const uint32_t patchEpoch = processor_.patchSyncEpoch();
    if (patchEpoch != lastSeenPatchEpoch_) {
        lastSeenPatchEpoch_ = patchEpoch;
        programmerDrawer_.syncFromProcessor();
        mixerDrawer_.syncFromProcessor();
    } else {
        if (programmerVisible_) {
            programmerDrawer_.syncFromProcessor();
        }
        if (mixerVisible_) {
            mixerDrawer_.syncFromProcessor();
        }
    }

    // Authentic [COMPARE] LCD blink timer (~2.5 Hz half-period = 12 frames @ 30 Hz)
    const bool compareActive = isPerformanceContextForPage()
                                   ? processor_.hostMemory().isPerformanceCompareActive()
                                   : processor_.hostMemory().isSingleCompareActive();
    if (compareActive) {
        if (++compareBlinkTicks_ >= 12u) {
            compareBlinkTicks_   = 0u;
            compareBlinkVisible_ = !compareBlinkVisible_;
        }
    } else {
        compareBlinkTicks_   = 0u;
        compareBlinkVisible_ = true;
    }

    if (pendingTensDigit_ >= 0 && pendingDigitTicks_ > 0u) {
        if (--pendingDigitTicks_ == 0u) {
            pendingTensDigit_ = -1;
        }
    }

    if (diskStatusTicks_ > 0u) {
        --diskStatusTicks_;
    }

    if (++ledBlinkTicks_ >= 8u) {
        ledBlinkTicks_ = 0u;
        ledBlinkOn_    = !ledBlinkOn_;
    }

    {
        classicController_.setHeldRhythmModifiers(isEraseModifierActive(), isStepInstModifierActive());
        classicController_.tick30Hz();
        syncMenuPageFromClassicController();
    }

    // Mirror incoming MIDI Pitch Bend & Mod Wheel onto the procedural 3D rubber wheels
    if (!pitchWheel_.isDraggingWheel()) {
        pitchWheel_.setValue(processor_.livePitchBendNorm(), false);
    }
    if (!modWheel_.isDraggingWheel()) {
        modWheel_.setValue(processor_.liveModWheelNorm(), false);
    }

    rebuildPageModelAndLcd();
    if (!dataEntryFader_.isMouseButtonDown()) {
        syncDataEntrySliderFromFocusedField();
    }
    repaint();
}

void VF50MainCanvas::sendRhythmKeyNoteOn(uint8_t midiNote,
                                        uint8_t velocity,
                                        bool eraseHeld,
                                        bool instSelectHeld) {
    classicController_.onChordKbdNoteOn(midiNote);
    classicController_.onMicroKbdNoteOn(midiNote);
    const bool effErase = eraseHeld || isEraseModifierActive();
    const bool effInst  = instSelectHeld || isStepInstModifierActive();
    (void)classicController_.onRhythmKeyNoteOn(midiNote, velocity, effErase, effInst);
    refreshDisplayAndLeds();
}

void VF50MainCanvas::refreshDisplayAndLeds() {
    const uint32_t patchEpoch = processor_.patchSyncEpoch();
    if (patchEpoch != lastSeenPatchEpoch_) {
        lastSeenPatchEpoch_ = patchEpoch;
        programmerDrawer_.syncFromProcessor();
        mixerDrawer_.syncFromProcessor();
    } else if (mixerVisible_) {
        mixerDrawer_.syncFromProcessor();
    }
    if (btnMixer_ != nullptr) {
        btnMixer_->setActivePillHighlight(mixerVisible_);
    }
    if (btnRomLoader_ != nullptr) {
        const auto& rom = processor_.activeRomBank();
        const int chipCnt = rom.loadedChipCount();
        const bool allReady = rom.isAllRomsLoaded();
        btnRomLoader_->setCapLegends(
            allReady ? juce::String("ROM SETUP  [4/4]")
                     : juce::String::formatted("LOAD ROMS  [%d/4]", chipCnt),
            allReady ? tokens::kSilkAccentTeal : tokens::kSilkOchreGold);
        btnRomLoader_->setActivePillHighlight(romGuideOverlay_.isVisible());
    }
    if (btnFloppyInsert_ != nullptr) {
        btnFloppyInsert_->setActivePillHighlight(diskManager_.isMounted() && !diskManager_.isUserDiskMounted());
    }
    if (btnFloppyUserDisk_ != nullptr) {
        btnFloppyUserDisk_->setActivePillHighlight(diskManager_.isUserDiskMounted());
        btnFloppyUserDisk_->setTooltip("Mount Common User Disk folder: " + diskManager_.userDiskDirectory().getFullPathName());
    }
    if (btnFloppyEject_ != nullptr) {
        btnFloppyEject_->setEnabled(diskManager_.isMounted());
    }
    syncTextEntryKeyboardFocus();
    classicController_.syncAudioHitTelemetry();
    rebuildPageModelAndLcd();
    syncDataEntrySliderFromFocusedField();
    repaint();
}

void VF50MainCanvas::rebuildPageModelAndLcd() {
    syncDrumTriggerTelemetry();

    const auto& pBank = processor_.parameterBank();
    const auto& synth = processor_.synthEngine();

    for (auto& f : pageFields_) {
        f = EditableField{};
    }
    activeFieldCount_ = 0;

    auto bindApvtsField = [&](int slotIdx, int curRow, int curCol,
                              ParamIndex pIdx, float minV, float maxV, float stepV = 1.0f) {
        if (slotIdx < 0 || slotIdx >= 8) {
            return;
        }
        auto& fld = pageFields_[static_cast<std::size_t>(slotIdx)];
        fld.valid      = true;
        fld.cursorRow  = curRow;
        fld.cursorCol  = curCol;
        fld.minVal     = minV;
        fld.maxVal     = maxV;
        fld.stepVal    = stepV;
        const auto rawIdx = static_cast<std::size_t>(pIdx);
        fld.readValue = [this, rawIdx]() -> float {
            return processor_.parameterBank().byIndex[rawIdx]->load(std::memory_order_relaxed);
        };
        fld.writeValue = [this, pIdx](float v) {
            if (pIdx == ParamIndex::VoicePreset) {
                const auto curBank = (processor_.parameterBank().voiceBank->load() >= 0.5f)
                                         ? vf50::state::MemoryBank::UserRam
                                         : vf50::state::MemoryBank::FactoryRom;
                const auto nextIdx = static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 99));
                processor_.selectSinglePresetAtomic(curBank, nextIdx);
            } else if (pIdx == ParamIndex::VoiceBank) {
                const auto targetBank = (v >= 0.5f) ? vf50::state::MemoryBank::UserRam
                                                    : vf50::state::MemoryBank::FactoryRom;
                const auto curIdx = static_cast<std::size_t>(
                    std::clamp(static_cast<int>(std::lround(processor_.parameterBank().voicePreset->load())), 0, 99));
                processor_.selectSinglePresetAtomic(targetBank, curIdx);
            } else if (pIdx == ParamIndex::PerfPreset) {
                const auto curBank = (processor_.parameterBank().perfBank->load() >= 0.5f)
                                         ? vf50::state::MemoryBank::UserRam
                                         : vf50::state::MemoryBank::FactoryRom;
                const auto nextIdx = static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 99));
                processor_.selectPerformancePresetAtomic(curBank, nextIdx);
            } else if (pIdx == ParamIndex::PerfBank) {
                const auto targetBank = (v >= 0.5f) ? vf50::state::MemoryBank::UserRam
                                                    : vf50::state::MemoryBank::FactoryRom;
                const auto curIdx = static_cast<std::size_t>(
                    std::clamp(static_cast<int>(std::lround(processor_.parameterBank().perfPreset->load())), 0, 99));
                processor_.selectPerformancePresetAtomic(targetBank, curIdx);
            } else {
                vf50::params::setApvtsRawValue(processor_.apvts(), pIdx, v);
            }
        };
        activeFieldCount_ = std::max(activeFieldCount_, slotIdx + 1);
    };

    auto bindCustomInt = [&](int slotIdx, int curRow, int curCol,
                             int minV, int maxV,
                             std::function<int()> getter,
                             std::function<void(int)> setter) {
        if (slotIdx < 0 || slotIdx >= 8) {
            return;
        }
        auto& fld = pageFields_[static_cast<std::size_t>(slotIdx)];
        fld.valid      = true;
        fld.cursorRow  = curRow;
        fld.cursorCol  = curCol;
        fld.minVal     = static_cast<float>(minV);
        fld.maxVal     = static_cast<float>(maxV);
        fld.stepVal    = 1.0f;
        fld.readValue  = [getter]() -> float { return static_cast<float>(getter()); };
        fld.writeValue = [setter](float v) { setter(static_cast<int>(std::lround(v))); };
        activeFieldCount_ = std::max(activeFieldCount_, slotIdx + 1);
    };

    std::array<char, 41> line0{};
    std::array<char, 41> line1{};
    line0.fill(' '); line0[40] = '\0';
    line1.fill(' '); line1[40] = '\0';

    processor_.pullLatestAudioMemoryToHost();

    {
        const bool isVoiceEditPg = (page_ >= MenuPage::SingleQuickEdit && page_ <= MenuPage::SingleFunction);
        classicController_.setDelegatedVoiceEditInfo(isVoiceEditPg, selectedOperator_);
        if (classicController_.usesOwnLcd()) {
            const auto frame = classicController_.renderLcdFrame();
            std::memcpy(line0.data(), frame.line0.data(), 40u);
            std::memcpy(line1.data(), frame.line1.data(), 40u);

            const bool compareActive = isPerformanceContextForPage()
                                           ? processor_.hostMemory().isPerformanceCompareActive()
                                           : processor_.hostMemory().isSingleCompareActive();
            const auto scr = classicController_.screen();
            const bool showCompareTag = (scr == vf50::classic::ClassicScreen::PlaySingle
                                      || scr == vf50::classic::ClassicScreen::PlayPerformance
                                      || scr == vf50::classic::ClassicScreen::VoiceEditQuick);
            if (compareActive && showCompareTag && !classicController_.hasActiveNotice()) {
                writeFixedCol(line0, 31, compareBlinkVisible_ ? "[COMPARE]" : "         ", 9);
            }

            lcd_.setLines(std::string_view(line0.data(), 40), std::string_view(line1.data(), 40));
            if (frame.showCursor) {
                lcd_.setCursor(frame.cursorRow, frame.cursorCol, VF50LcdDisplay::CursorMode::BlinkingUnderline);
            } else {
                lcd_.setCursor(0, 0, VF50LcdDisplay::CursorMode::None);
            }
            return;
        }
    }

    const auto& curVoice = processor_.hostMemory().activeSingleVoice();
    const auto& curPerf  = processor_.hostMemory().activePerformance();
    const int voiceIdx   = std::clamp(static_cast<int>(std::lround(pBank.voicePreset->load())), 0, 99);
    const int perfIdx    = std::clamp(static_cast<int>(std::lround(pBank.perfPreset->load())), 0, 99);
    const char vBankChar = (pBank.voiceBank->load() >= 0.5f) ? (cardBankVisualActive_ ? 'C' : 'I') : 'P';
    const char pBankChar = (pBank.perfBank->load() >= 0.5f) ? (cardBankVisualActive_ ? 'C' : 'I') : 'P';

    char tmp[64]{};

    switch (page_) {
        case MenuPage::SinglePlay: {
            const int alg = std::clamp(static_cast<int>(std::lround(pBank.voiceAlgorithm->load())), 0, 7);
            const int fb  = std::clamp(static_cast<int>(std::lround(pBank.voiceFeedback->load())), 0, 7);
            const bool isSingleEdit = (processor_.hostMemory().workstationMode() == vf50::state::WorkstationMode::SingleEdit);
            const char dispBankChar = isSingleEdit ? 'E' : vBankChar;
            std::snprintf(tmp, sizeof(tmp), "SINGLE %c%02d %-10.10s ALG%d FB%d V:%02d",
                          dispBankChar, voiceIdx, curVoice.name.data(),
                          alg + 1,
                          fb,
                          processor_.activeFmVoices());
            writeFixedCol(line0, 0, tmp, 37);

            if (diskStatusTicks_ > 0u && diskStatusMsg_.isNotEmpty()) {
                const auto statusUtf8 = diskStatusMsg_.toStdString();
                std::snprintf(tmp, sizeof(tmp), "%-40.40s", statusUtf8.c_str());
                writeFixedCol(line1, 0, tmp, 40);
            } else {
                const int atk = static_cast<int>(std::lround(pBank.quickAttack->load()));
                const int rel = static_cast<int>(std::lround(pBank.quickRelease->load()));
                const int vol = static_cast<int>(std::lround(pBank.quickTotalLevel->load()));
                const int bri = static_cast<int>(std::lround(pBank.quickBrilliance->load()));
                std::snprintf(tmp, sizeof(tmp), "%c%02d  A%+03d R%+03d V%+03d B%+03d [OSC][RHY][EFX]",
                              dispBankChar, voiceIdx, atk, rel, vol, bri);
                writeFixedCol(line1, 0, tmp, 40);
            }

            bindApvtsField(0, 1, 1,  ParamIndex::VoicePreset,     0.0f, 99.0f, 1.0f);
            bindApvtsField(1, 1, 6,  ParamIndex::QuickAttack,    -99.0f, 99.0f, 1.0f);
            bindApvtsField(2, 1, 11, ParamIndex::QuickRelease,   -99.0f, 99.0f, 1.0f);
            bindApvtsField(3, 1, 16, ParamIndex::QuickTotalLevel,-99.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 21, ParamIndex::QuickBrilliance,-99.0f, 99.0f, 1.0f);
            break;
        }

        case MenuPage::SingleQuickEdit: {
            std::snprintf(tmp, sizeof(tmp), "QUICK EDIT  %c%02d %-10.10s   OP:1234",
                          vBankChar, voiceIdx, curVoice.name.data());
            writeFixedCol(line0, 0, tmp, 40);

            const int atk = static_cast<int>(std::lround(pBank.quickAttack->load()));
            const int rel = static_cast<int>(std::lround(pBank.quickRelease->load()));
            const int vol = static_cast<int>(std::lround(pBank.quickTotalLevel->load()));
            const int bri = static_cast<int>(std::lround(pBank.quickBrilliance->load()));
            const int rev = static_cast<int>(std::lround(pBank.voiceReverbRate->load()));
            const int fxs = (pBank.fxSingleSend->load() >= 0.5f) ? 1 : 0;
            std::snprintf(tmp, sizeof(tmp), "ATK%+03d REL%+03d VOL%+03d BRI%+03d REV:%d FX:%s",
                          atk, rel, vol, bri, rev, fxs ? "ON " : "OFF");
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 3,  ParamIndex::QuickAttack,     -99.0f, 99.0f, 1.0f);
            bindApvtsField(1, 1, 10, ParamIndex::QuickRelease,    -99.0f, 99.0f, 1.0f);
            bindApvtsField(2, 1, 17, ParamIndex::QuickTotalLevel, -99.0f, 99.0f, 1.0f);
            bindApvtsField(3, 1, 24, ParamIndex::QuickBrilliance, -99.0f, 99.0f, 1.0f);
            bindApvtsField(5, 1, 32, ParamIndex::VoiceReverbRate,   0.0f,  7.0f, 1.0f);
            bindApvtsField(6, 1, 37, ParamIndex::FxSingleSend,      0.0f,  1.0f, 1.0f);
            break;
        }

        case MenuPage::SingleAlgorithm: {
            const int alg = std::clamp(static_cast<int>(std::lround(pBank.voiceAlgorithm->load())), 0, 7);
            const int fb  = std::clamp(static_cast<int>(std::lround(pBank.voiceFeedback->load())), 0, 7);
            static constexpr std::array<const char*, 8> kTopo = {
                "4>3>2>1", "(4+3)>2>1", "4+(3>2)>1", "(4>3+2)>1",
                "(2>1)+(4>3)", "4>(1+2+3)", "1+2+(4>3)", "1+2+3+4"
            };
            std::snprintf(tmp, sizeof(tmp), "VOICE ALGORITHM   %-10.10s [%s]",
                          curVoice.name.data(), kTopo[static_cast<std::size_t>(alg)]);
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "ALG:%d    FEEDBACK:%d    OP MASK: 1 2 3 4",
                          alg + 1, fb);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 4,  ParamIndex::VoiceAlgorithm, 0.0f, 7.0f, 1.0f);
            bindApvtsField(2, 1, 18, ParamIndex::VoiceFeedback,  0.0f, 7.0f, 1.0f);
            bindApvtsField(4, 1, 32, ParamIndex::Op1Enable,      0.0f, 1.0f, 1.0f);
            bindApvtsField(5, 1, 34, ParamIndex::Op2Enable,      0.0f, 1.0f, 1.0f);
            bindApvtsField(6, 1, 36, ParamIndex::Op3Enable,      0.0f, 1.0f, 1.0f);
            bindApvtsField(7, 1, 38, ParamIndex::Op4Enable,      0.0f, 1.0f, 1.0f);
            break;
        }

        case MenuPage::SingleLfo: {
            static constexpr std::array<const char*, 4> kWaves = { "SAW", "SQR", "TRI", "S&H" };
            const int wv  = std::clamp(static_cast<int>(std::lround(pBank.lfo1Wave->load())), 0, 3);
            const int spd = static_cast<int>(std::lround(pBank.lfo1Speed->load()));
            const int dly = static_cast<int>(std::lround(pBank.lfo1Delay->load()));
            const int pmd = static_cast<int>(std::lround(pBank.lfo1Pmd->load()));
            const int amd = static_cast<int>(std::lround(pBank.lfo1Amd->load()));
            const int syn = (pBank.lfo1Sync->load() >= 0.5f) ? 1 : 0;
            const int l2  = (pBank.lfo2Override->load() >= 0.5f) ? 1 : 0;
            const int l2s = static_cast<int>(std::lround(pBank.lfo2Speed->load()));

            std::snprintf(tmp, sizeof(tmp), "VOICE LFO1/2  %-10.10s  LFO2:%s(%02d)",
                          curVoice.name.data(), l2 ? "ON " : "OFF", l2s);
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "WAV:%s SPD:%02d DLY:%02d PMD:%02d AMD:%02d SY:%d",
                          kWaves[static_cast<std::size_t>(wv)], spd, dly, pmd, amd, syn);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 4,  ParamIndex::Lfo1Wave,     0.0f, 3.0f, 1.0f);
            bindApvtsField(1, 1, 12, ParamIndex::Lfo1Speed,    0.0f, 99.0f, 1.0f);
            bindApvtsField(2, 1, 19, ParamIndex::Lfo1Delay,    0.0f, 99.0f, 1.0f);
            bindApvtsField(3, 1, 26, ParamIndex::Lfo1Pmd,      0.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 33, ParamIndex::Lfo1Amd,      0.0f, 99.0f, 1.0f);
            bindApvtsField(5, 1, 39, ParamIndex::Lfo1Sync,     0.0f, 1.0f, 1.0f);
            bindApvtsField(6, 0, 31, ParamIndex::Lfo2Override, 0.0f, 1.0f, 1.0f);
            bindApvtsField(7, 0, 36, ParamIndex::Lfo2Speed,    0.0f, 99.0f, 1.0f);
            break;
        }

        case MenuPage::SingleSensitivity: {
            const auto& opB = pBank.ops[static_cast<std::size_t>(selectedOperator_)];
            const int pms = static_cast<int>(std::lround(pBank.lfo1Pms->load()));
            const int ams = static_cast<int>(std::lround(pBank.lfo1Ams->load()));
            const int kvs = static_cast<int>(std::lround(opB.keyVelSens->load()));
            const int ebs = static_cast<int>(std::lround(opB.egBiasSens->load()));
            const int ame = (opB.ampModEnable->load() >= 0.5f) ? 1 : 0;

            std::snprintf(tmp, sizeof(tmp), "SENSITIVITY [OP%d] %-10.10s  (OP SEL)",
                          selectedOperator_ + 1, curVoice.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "OP:%d PMS:%d AMS:%d KVS:%+2d EBS:%d AME:%s",
                          selectedOperator_ + 1, pms, ams, kvs, ebs, ame ? "ON " : "OFF");
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 3, 0, 3, [this]() { return selectedOperator_; },
                          [this](int v) { selectedOperator_ = std::clamp(v, 0, 3); });
            bindApvtsField(1, 1, 9,  ParamIndex::Lfo1Pms, 0.0f, 7.0f, 1.0f);
            bindApvtsField(2, 1, 15, ParamIndex::Lfo1Ams, 0.0f, 3.0f, 1.0f);
            bindApvtsField(3, 1, 21, opParam(selectedOperator_, 17u), -7.0f, 7.0f, 1.0f);
            bindApvtsField(4, 1, 28, opParam(selectedOperator_, 18u),  0.0f, 7.0f, 1.0f);
            bindApvtsField(5, 1, 34, opParam(selectedOperator_, 19u),  0.0f, 1.0f, 1.0f);
            break;
        }

        case MenuPage::SingleOscillator: {
            const auto& opB = pBank.ops[static_cast<std::size_t>(selectedOperator_)];
            const int wf  = std::clamp(static_cast<int>(std::lround(opB.waveform->load())), 0, 7);
            const bool fx = opB.fixedMode->load() >= 0.5f;
            const bool lo = opB.fixedShift->load() >= 0.5f;
            const int rng = std::clamp(static_cast<int>(std::lround(opB.fixedRange->load())), 0, 7);
            const int crs = std::clamp(static_cast<int>(std::lround(opB.coarse->load())), 0, 63);
            const int fin = std::clamp(static_cast<int>(std::lround(opB.fine->load())), 0, 15);
            const int det = std::clamp(static_cast<int>(std::lround(opB.detune->load())), -3, 3);

            double dispFreq = 0.0;
            if (fx) {
                dispFreq = vf50::dsp::fm::computeOpzFixedFreqHz(
                    static_cast<uint8_t>(crs), static_cast<uint8_t>(fin),
                    static_cast<uint8_t>(rng), lo ? 1u : 0u);
                std::snprintf(tmp, sizeof(tmp), "OSCILLATOR [OP%d]  FIX %s R%d = %7.2fHz",
                              selectedOperator_ + 1, lo ? "LO" : "HI", rng, dispFreq);
            } else {
                dispFreq = vf50::dsp::fm::computeOpzRatioMultiplier(
                    static_cast<uint8_t>(crs), static_cast<uint8_t>(fin));
                std::snprintf(tmp, sizeof(tmp), "OSCILLATOR [OP%d]  RATIO MULT = x%6.3f",
                              selectedOperator_ + 1, dispFreq);
            }
            writeFixedCol(line0, 0, tmp, 40);

            std::snprintf(tmp, sizeof(tmp), "OP%d W%d %s C:%02d F:%02d DT:%+d R:%d S:%s",
                          selectedOperator_ + 1, wf + 1, fx ? "FIX" : "RAT",
                          crs, fin, det, rng, lo ? "LO" : "HI");
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 2, 0, 3, [this]() { return selectedOperator_; },
                          [this](int v) { selectedOperator_ = std::clamp(v, 0, 3); });
            bindApvtsField(1, 1, 5,  opParam(selectedOperator_, 1u),  0.0f,  7.0f, 1.0f);
            bindApvtsField(2, 1, 7,  opParam(selectedOperator_, 2u),  0.0f,  1.0f, 1.0f);
            bindApvtsField(3, 1, 13, opParam(selectedOperator_, 5u),  0.0f, 63.0f, 1.0f);
            bindApvtsField(4, 1, 18, opParam(selectedOperator_, 6u),  0.0f, 15.0f, 1.0f);
            bindApvtsField(5, 1, 24, opParam(selectedOperator_, 7u), -3.0f,  3.0f, 1.0f);
            bindApvtsField(6, 1, 28, opParam(selectedOperator_, 4u),  0.0f,  7.0f, 1.0f);
            bindApvtsField(7, 1, 32, opParam(selectedOperator_, 3u),  0.0f,  1.0f, 1.0f);
            break;
        }

        case MenuPage::SingleEg: {
            const auto& opB = pBank.ops[static_cast<std::size_t>(selectedOperator_)];
            const int ar  = static_cast<int>(std::lround(opB.ar->load()));
            const int d1r = static_cast<int>(std::lround(opB.d1r->load()));
            const int d1l = static_cast<int>(std::lround(opB.d1l->load()));
            const int d2r = static_cast<int>(std::lround(opB.d2r->load()));
            const int rr  = static_cast<int>(std::lround(opB.rr->load()));
            const int shf = std::clamp(static_cast<int>(std::lround(opB.egShift->load())), 0, 3);
            static constexpr std::array<const char*, 4> kShfStr = { "96dB", "48dB", "24dB", "12dB" };

            std::snprintf(tmp, sizeof(tmp), "ENVELOPE GEN [OP%d] %-10.10s SHF:%s",
                          selectedOperator_ + 1, curVoice.name.data(), kShfStr[static_cast<std::size_t>(shf)]);
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "OP%d AR:%02d D1R:%02d D1L:%02d D2R:%02d RR:%02d S:%d",
                          selectedOperator_ + 1, ar, d1r, d1l, d2r, rr, shf);
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 2, 0, 3, [this]() { return selectedOperator_; },
                          [this](int v) { selectedOperator_ = std::clamp(v, 0, 3); });
            bindApvtsField(1, 1, 7,  opParam(selectedOperator_, 8u),  0.0f, 31.0f, 1.0f);
            bindApvtsField(2, 1, 14, opParam(selectedOperator_, 9u),  0.0f, 31.0f, 1.0f);
            bindApvtsField(3, 1, 21, opParam(selectedOperator_, 10u), 0.0f, 15.0f, 1.0f);
            bindApvtsField(4, 1, 28, opParam(selectedOperator_, 11u), 0.0f, 31.0f, 1.0f);
            bindApvtsField(5, 1, 34, opParam(selectedOperator_, 12u), 1.0f, 15.0f, 1.0f);
            bindApvtsField(6, 1, 39, opParam(selectedOperator_, 13u), 0.0f,  3.0f, 1.0f);
            break;
        }

        case MenuPage::SinglePeg: {
            const int pr1 = static_cast<int>(std::lround(pBank.pegPr1->load()));
            const int pr2 = static_cast<int>(std::lround(pBank.pegPr2->load()));
            const int pr3 = static_cast<int>(std::lround(pBank.pegPr3->load()));
            const int pl1 = static_cast<int>(std::lround(pBank.pegPl1->load()));
            const int pl2 = static_cast<int>(std::lround(pBank.pegPl2->load()));
            const int pl3 = static_cast<int>(std::lround(pBank.pegPl3->load()));

            std::snprintf(tmp, sizeof(tmp), "PITCH EG (50=0st)  %-10.10s", curVoice.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "R1:%02d R2:%02d R3:%02d  L1:%02d L2:%02d L3:%02d",
                          pr1, pr2, pr3, pl1, pl2, pl3);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 3,  ParamIndex::PegPr1, 0.0f, 99.0f, 1.0f);
            bindApvtsField(1, 1, 9,  ParamIndex::PegPr2, 0.0f, 99.0f, 1.0f);
            bindApvtsField(2, 1, 15, ParamIndex::PegPr3, 0.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 22, ParamIndex::PegPl1, 0.0f, 99.0f, 1.0f);
            bindApvtsField(5, 1, 28, ParamIndex::PegPl2, 0.0f, 99.0f, 1.0f);
            bindApvtsField(6, 1, 34, ParamIndex::PegPl3, 0.0f, 99.0f, 1.0f);
            break;
        }

        case MenuPage::SingleOutLevel: {
            const int ol1 = static_cast<int>(std::lround(pBank.ops[0].outputLevel->load()));
            const int ol2 = static_cast<int>(std::lround(pBank.ops[1].outputLevel->load()));
            const int ol3 = static_cast<int>(std::lround(pBank.ops[2].outputLevel->load()));
            const int ol4 = static_cast<int>(std::lround(pBank.ops[3].outputLevel->load()));

            std::snprintf(tmp, sizeof(tmp), "OPERATOR OUT LEVEL %-10.10s (0.75dB)", curVoice.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "OP1:%02d    OP2:%02d    OP3:%02d    OP4:%02d",
                          ol1, ol2, ol3, ol4);
            writeFixedCol(line1, 0, tmp, 40);

            // Inject custom CGRAM 1..8 vertical level bars next to each operator!
            const std::array<int, 4> ols = { ol1, ol2, ol3, ol4 };
            for (int op = 0; op < 4; ++op) {
                const uint8_t barCh = static_cast<uint8_t>(std::clamp((ols[static_cast<std::size_t>(op)] * 8 + 12) / 99, 1, 8));
                line1[static_cast<std::size_t>(7 + op * 10)] = static_cast<char>(barCh);
            }

            bindApvtsField(0, 1, 4,  ParamIndex::Op1OutputLevel, 0.0f, 99.0f, 1.0f);
            bindApvtsField(2, 1, 14, ParamIndex::Op2OutputLevel, 0.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 24, ParamIndex::Op3OutputLevel, 0.0f, 99.0f, 1.0f);
            bindApvtsField(6, 1, 34, ParamIndex::Op4OutputLevel, 0.0f, 99.0f, 1.0f);
            break;
        }

        case MenuPage::SingleScaling: {
            const int rs = static_cast<int>(std::lround(pBank.ops[static_cast<std::size_t>(selectedOperator_)].rateScaling->load()));
            const int l1 = static_cast<int>(std::lround(pBank.ops[0].levelScaling->load()));
            const int l2 = static_cast<int>(std::lround(pBank.ops[1].levelScaling->load()));
            const int l3 = static_cast<int>(std::lround(pBank.ops[2].levelScaling->load()));
            const int l4 = static_cast<int>(std::lround(pBank.ops[3].levelScaling->load()));

            std::snprintf(tmp, sizeof(tmp), "KEY SCALING [OP%d RS:%d] %-10.10s",
                          selectedOperator_ + 1, rs, curVoice.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "OP%d RS:%d L1%+03d L2%+03d L3%+03d L4%+03d",
                          selectedOperator_ + 1, rs, l1, l2, l3, l4);
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 2, 0, 3, [this]() { return selectedOperator_; },
                          [this](int v) { selectedOperator_ = std::clamp(v, 0, 3); });
            bindApvtsField(1, 1, 7,  opParam(selectedOperator_, 15u),   0.0f,  3.0f, 1.0f);
            bindApvtsField(2, 1, 11, ParamIndex::Op1LevelScaling, -99.0f, 99.0f, 1.0f);
            bindApvtsField(3, 1, 17, ParamIndex::Op2LevelScaling, -99.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 23, ParamIndex::Op3LevelScaling, -99.0f, 99.0f, 1.0f);
            bindApvtsField(5, 1, 29, ParamIndex::Op4LevelScaling, -99.0f, 99.0f, 1.0f);
            break;
        }

        case MenuPage::SingleTranspose: {
            const int tr = static_cast<int>(std::lround(pBank.voiceTranspose->load()));
            char keyName[5]{};
            formatMidiNoteName(60 + tr, keyName);
            std::snprintf(tmp, sizeof(tmp), "VOICE TRANSPOSE    %-10.10s", curVoice.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "TRANSPOSE: %+03d SEMI   (ROOT KEY = %-3s)", tr, keyName);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 11, ParamIndex::VoiceTranspose, -24.0f, 24.0f, 1.0f);
            break;
        }

        case MenuPage::SingleFunction: {
            const int mono = static_cast<int>(std::lround(pBank.voiceMonoMode->load()));
            const int pbr  = static_cast<int>(std::lround(pBank.voicePitchBendRange->load()));
            const int prt  = (pBank.voicePortaSwitch->load() >= 0.5f) ? 1 : 0;
            const int ptm  = static_cast<int>(std::lround(pBank.voicePortaTime->load()));
            const int sus  = (pBank.voiceSustainSwitch->load() >= 0.5f) ? 1 : 0;
            const int mwp  = static_cast<int>(std::lround(pBank.ctrlMwPitch->load()));
            const int atp  = static_cast<int>(std::lround(pBank.ctrlAtPitch->load()));

            std::snprintf(tmp, sizeof(tmp), "FUNCTION  %-10.10s  MW.P:%02d AT.P:%02d",
                          curVoice.name.data(), mwp, atp);
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "%s PB:%02d PRT:%s(%02d) SUS:%s",
                          mono ? "MONO" : "POLY", pbr, prt ? "ON" : "OF", ptm, sus ? "ON " : "OFF");
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 0,  ParamIndex::VoiceMonoMode,       0.0f,  1.0f, 1.0f);
            bindApvtsField(1, 1, 8,  ParamIndex::VoicePitchBendRange, 0.0f, 12.0f, 1.0f);
            bindApvtsField(2, 1, 15, ParamIndex::VoicePortaSwitch,    0.0f,  1.0f, 1.0f);
            bindApvtsField(3, 1, 18, ParamIndex::VoicePortaTime,      0.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 26, ParamIndex::VoiceSustainSwitch,  0.0f,  1.0f, 1.0f);
            bindApvtsField(6, 0, 27, ParamIndex::CtrlMwPitch,         0.0f, 99.0f, 1.0f);
            bindApvtsField(7, 0, 35, ParamIndex::CtrlAtPitch,         0.0f, 99.0f, 1.0f);
            break;
        }

        case MenuPage::PerfPlay: {
            const int asg = std::clamp(static_cast<int>(std::lround(pBank.perfAssignMode->load())), 0, 2);
            static constexpr std::array<const char*, 3> kAsg = { "NRM", "ALT", "DVA" };
            std::snprintf(tmp, sizeof(tmp), "PERF   %c%02d %-10.10s %s PT:%d V:%02d",
                          pBankChar, perfIdx, curPerf.name.data(),
                          kAsg[static_cast<std::size_t>(asg)],
                          selectedPerfPart_ + 1, processor_.activeFmVoices());
            writeFixedCol(line0, 0, tmp, 37);

            if (diskStatusTicks_ > 0u && diskStatusMsg_.isNotEmpty()) {
                const auto statusUtf8 = diskStatusMsg_.toStdString();
                std::snprintf(tmp, sizeof(tmp), "%-40.40s", statusUtf8.c_str());
                writeFixedCol(line1, 0, tmp, 40);
            } else {
                const auto& ptB = pBank.parts[static_cast<std::size_t>(selectedPerfPart_)];
                const int pVb  = static_cast<int>(std::lround(ptB.voiceBank->load()));
                const int pVn  = std::clamp(static_cast<int>(std::lround(ptB.voiceNumber->load())), 0, 99);
                const int pVol = static_cast<int>(std::lround(ptB.volume->load()));
                const int pPan = static_cast<int>(std::lround(ptB.pan->load()));
                std::snprintf(tmp, sizeof(tmp), "%c%02d P%d:%c%02d V:%02d PN:%+03d [MIX][RHY][EFX]",
                              pBankChar, perfIdx, selectedPerfPart_ + 1,
                              pVb ? 'I' : 'P', pVn, pVol, pPan);
                writeFixedCol(line1, 0, tmp, 40);
            }

            bindApvtsField(0, 1, 1,  ParamIndex::PerfPreset, 0.0f, 99.0f, 1.0f);
            bindCustomInt(1, 1, 5, 0, 7, [this]() { return selectedPerfPart_; },
                          [this](int v) { selectedPerfPart_ = std::clamp(v, 0, 7); });
            bindApvtsField(2, 1, 8,  partParam(selectedPerfPart_, 1u),   0.0f, 99.0f, 1.0f);
            bindApvtsField(3, 1, 13, partParam(selectedPerfPart_, 4u),   0.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 19, partParam(selectedPerfPart_, 5u), -64.0f, 63.0f, 1.0f);
            break;
        }

        case MenuPage::PerfAssignMode: {
            const int asg = std::clamp(static_cast<int>(std::lround(pBank.perfAssignMode->load())), 0, 2);
            static constexpr std::array<const char*, 3> kAsgFull = { "NORMAL", "ALTERNATE", "DYNAMIC(DVA)" };
            const int mt  = static_cast<int>(std::lround(pBank.perfMicroTuneTable->load()));
            const int fxs = static_cast<int>(std::lround(pBank.perfEffectSelect->load()));
            std::snprintf(tmp, sizeof(tmp), "PERF ASSIGN MODE   %-10.10s", curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "MODE:%-12s  M.TUNE:%02d  FXSEL:%02d",
                          kAsgFull[static_cast<std::size_t>(asg)], mt, fxs);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 5,  ParamIndex::PerfAssignMode,     0.0f,  2.0f, 1.0f);
            bindApvtsField(4, 1, 26, ParamIndex::PerfMicroTuneTable, 0.0f, 12.0f, 1.0f);
            bindApvtsField(6, 1, 36, ParamIndex::PerfEffectSelect,   0.0f, 12.0f, 1.0f);
            break;
        }

        case MenuPage::PerfNotes: {
            const auto& ptB = pBank.parts[static_cast<std::size_t>(selectedPerfPart_)];
            const int mx = static_cast<int>(std::lround(ptB.maxNotes->load()));
            const int rv = static_cast<int>(std::lround(ptB.reserveNotes->load()));
            std::snprintf(tmp, sizeof(tmp), "PERF NOTES [PART %d] %-10.10s",
                          selectedPerfPart_ + 1, curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "PART:%d   MAX NOTES:%02d   RESERVE:%02d",
                          selectedPerfPart_ + 1, mx, rv);
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 5, 0, 7, [this]() { return selectedPerfPart_; },
                          [this](int v) { selectedPerfPart_ = std::clamp(v, 0, 7); });
            bindApvtsField(2, 1, 19, partParam(selectedPerfPart_, 2u), 0.0f, 16.0f, 1.0f);
            bindApvtsField(5, 1, 32, partParam(selectedPerfPart_, 3u), 0.0f, 17.0f, 1.0f);
            break;
        }

        case MenuPage::PerfVoiceNumber: {
            const auto& ptB = pBank.parts[static_cast<std::size_t>(selectedPerfPart_)];
            const int vb = static_cast<int>(std::lround(ptB.voiceBank->load()));
            const int vn = std::clamp(static_cast<int>(std::lround(ptB.voiceNumber->load())), 0, 99);
            processor_.pullLatestAudioMemoryToHost();
            const auto vName = processor_.hostMemory().getVoice(
                vb ? vf50::state::MemoryBank::UserRam : vf50::state::MemoryBank::FactoryRom,
                static_cast<std::size_t>(vn)).name;
            std::snprintf(tmp, sizeof(tmp), "PERF VOICE [PART %d] %-10.10s",
                          selectedPerfPart_ + 1, curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "PART:%d  BANK:%s  NUM:%02d (%-10.10s)",
                          selectedPerfPart_ + 1, vb ? "INT" : "PRE", vn, vName.data());
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 5, 0, 7, [this]() { return selectedPerfPart_; },
                          [this](int v) { selectedPerfPart_ = std::clamp(v, 0, 7); });
            bindApvtsField(2, 1, 13, partParam(selectedPerfPart_, 0u), 0.0f,  1.0f, 1.0f);
            bindApvtsField(4, 1, 22, partParam(selectedPerfPart_, 1u), 0.0f, 99.0f, 1.0f);
            break;
        }

        case MenuPage::PerfRecvCh: {
            std::snprintf(tmp, sizeof(tmp), "PERF MIDI RECEIVE CH  %-10.10s", curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            int col = 0;
            for (int p = 0; p < 8; ++p) {
                const int rc = static_cast<int>(std::lround(pBank.parts[static_cast<std::size_t>(p)].receiveCh->load()));
                if (rc == 16) std::snprintf(tmp, sizeof(tmp), "%d:OM ", p + 1);
                else if (rc >= 17) std::snprintf(tmp, sizeof(tmp), "%d:-- ", p + 1);
                else std::snprintf(tmp, sizeof(tmp), "%d:%02d ", p + 1, rc + 1);
                writeFixedCol(line1, col, tmp, 5);
                bindApvtsField(p, 1, col + 2, partParam(p, 11u), 0.0f, 17.0f, 1.0f);
                col += 5;
            }
            break;
        }

        case MenuPage::PerfLimitLow:
        case MenuPage::PerfLimitHigh: {
            const bool isHigh = (page_ == MenuPage::PerfLimitHigh);
            std::snprintf(tmp, sizeof(tmp), "PERF KEY LIMIT %-4s   %-10.10s",
                          isHigh ? "HIGH" : "LOW", curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            int col = 0;
            for (int p = 0; p < 8; ++p) {
                const auto* ptr = isHigh ? pBank.parts[static_cast<std::size_t>(p)].limitHigh
                                         : pBank.parts[static_cast<std::size_t>(p)].limitLow;
                const int note = static_cast<int>(std::lround(ptr->load()));
                char kn[5]{};
                formatMidiNoteName(note, kn);
                std::snprintf(tmp, sizeof(tmp), "%-4s ", kn);
                writeFixedCol(line1, col, tmp, 5);
                bindApvtsField(p, 1, col, partParam(p, isHigh ? 9u : 8u), 0.0f, 127.0f, 1.0f);
                col += 5;
            }
            break;
        }

        case MenuPage::PerfDetune: {
            std::snprintf(tmp, sizeof(tmp), "PERF INST DETUNE      %-10.10s", curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            int col = 0;
            for (int p = 0; p < 8; ++p) {
                const int dt = static_cast<int>(std::lround(pBank.parts[static_cast<std::size_t>(p)].detune->load()));
                std::snprintf(tmp, sizeof(tmp), "%d:%+2d ", p + 1, dt);
                writeFixedCol(line1, col, tmp, 5);
                bindApvtsField(p, 1, col + 2, partParam(p, 6u), -7.0f, 7.0f, 1.0f);
                col += 5;
            }
            break;
        }

        case MenuPage::PerfNoteShift: {
            std::snprintf(tmp, sizeof(tmp), "PERF NOTE SHIFT       %-10.10s", curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            int col = 0;
            for (int p = 0; p < 8; ++p) {
                const int ns = static_cast<int>(std::lround(pBank.parts[static_cast<std::size_t>(p)].noteShift->load()));
                std::snprintf(tmp, sizeof(tmp), "%+03d  ", ns);
                writeFixedCol(line1, col, tmp, 5);
                bindApvtsField(p, 1, col, partParam(p, 7u), -24.0f, 24.0f, 1.0f);
                col += 5;
            }
            break;
        }

        case MenuPage::PerfVolumePan: {
            const auto& ptB = pBank.parts[static_cast<std::size_t>(selectedPerfPart_)];
            const int vol = static_cast<int>(std::lround(ptB.volume->load()));
            const int pan = static_cast<int>(std::lround(ptB.pan->load()));
            const int lfo = static_cast<int>(std::lround(ptB.lfoSelect->load()));
            std::snprintf(tmp, sizeof(tmp), "PERF VOLUME & PAN [P%d] %-10.10s",
                          selectedPerfPart_ + 1, curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "PART:%d  VOLUME:%02d  PAN:%+03d  LFOSEL:%d",
                          selectedPerfPart_ + 1, vol, pan, lfo);
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 5, 0, 7, [this]() { return selectedPerfPart_; },
                          [this](int v) { selectedPerfPart_ = std::clamp(v, 0, 7); });
            bindApvtsField(2, 1, 15, partParam(selectedPerfPart_, 4u),   0.0f, 99.0f, 1.0f);
            bindApvtsField(4, 1, 23, partParam(selectedPerfPart_, 5u), -64.0f, 63.0f, 1.0f);
            bindApvtsField(6, 1, 35, partParam(selectedPerfPart_, 12u),  0.0f,  3.0f, 1.0f);
            break;
        }

        case MenuPage::PerfOutAssign: {
            const auto& ptB = pBank.parts[static_cast<std::size_t>(selectedPerfPart_)];
            const int oa  = std::clamp(static_cast<int>(std::lround(ptB.outAssign->load())), 0, 3);
            const int fxs = (ptB.effectSend->load() >= 0.5f) ? 1 : 0;
            const int mte = (ptB.microTuneEnable->load() >= 0.5f) ? 1 : 0;
            static constexpr std::array<const char*, 4> kOaNames = { "OFF ", "OUT1", "OUT2", "1+2 " };
            std::snprintf(tmp, sizeof(tmp), "PERF OUTPUT ASSIGN [P%d] %-10.10s",
                          selectedPerfPart_ + 1, curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "PART:%d  OUT:%s  EFX:%s  MICROTUNE:%s",
                          selectedPerfPart_ + 1, kOaNames[static_cast<std::size_t>(oa)],
                          fxs ? "ON " : "OFF", mte ? "ON " : "OFF");
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 5, 0, 7, [this]() { return selectedPerfPart_; },
                          [this](int v) { selectedPerfPart_ = std::clamp(v, 0, 7); });
            bindApvtsField(2, 1, 12, partParam(selectedPerfPart_, 10u), 0.0f, 3.0f, 1.0f);
            bindApvtsField(4, 1, 22, partParam(selectedPerfPart_, 14u), 0.0f, 1.0f, 1.0f);
            bindApvtsField(6, 1, 37, partParam(selectedPerfPart_, 13u), 0.0f, 1.0f, 1.0f);
            break;
        }

        case MenuPage::PerfOthers: {
            const int mtt = static_cast<int>(std::lround(pBank.perfMicroTuneTable->load()));
            const int mtr = static_cast<int>(std::lround(pBank.perfMicroTuneRoot->load()));
            const int fxs = static_cast<int>(std::lround(pBank.perfEffectSelect->load()));
            std::snprintf(tmp, sizeof(tmp), "PERF OTHERS        %-10.10s", curPerf.name.data());
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "MICROTUNE TBL:%02d  ROOT:%02d  FX SELECT:%02d",
                          mtt, mtr, fxs);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(1, 1, 14, ParamIndex::PerfMicroTuneTable, 0.0f, 12.0f, 1.0f);
            bindApvtsField(4, 1, 23, ParamIndex::PerfMicroTuneRoot,  0.0f, 11.0f, 1.0f);
            bindApvtsField(6, 1, 37, ParamIndex::PerfEffectSelect,   0.0f, 12.0f, 1.0f);
            break;
        }

        case MenuPage::EffectEdit: {
            const int fxType = std::clamp(static_cast<int>(std::lround(pBank.fxAlgorithm->load())), 0, 32);
            const int bal    = static_cast<int>(std::lround(pBank.fxWetDryMix->load()));
            const int outLvl = static_cast<int>(std::lround(pBank.fxOutLevel->load()));
            const int p1     = static_cast<int>(std::lround(pBank.fxParam1->load()));
            const int p2     = static_cast<int>(std::lround(pBank.fxParam2->load()));
            const int p3     = static_cast<int>(std::lround(pBank.fxParam3->load()));
            const int stMix  = (pBank.fxStereoMix->load() >= 0.5f) ? 1 : 0;
            const auto fxNameSv = processor_.activeRomBank().dspEffectNames[static_cast<std::size_t>(fxType)];

            std::snprintf(tmp, sizeof(tmp), "EFFECT #%02d %-14.*s %s",
                          fxType, static_cast<int>(fxNameSv.size()), fxNameSv.data(),
                          synth.effects().isBypassed() ? "[BYPASS]" : "[ACTIVE]");
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "T:%02d BL:%03d LV:%03d P1:%02d P2:%02d P3:%02d S:%d",
                          fxType, bal, outLvl, p1, p2, p3, stMix);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 2,  ParamIndex::FxAlgorithm, 0.0f,  32.0f, 1.0f);
            bindApvtsField(1, 1, 8,  ParamIndex::FxWetDryMix, 0.0f, 100.0f, 1.0f);
            bindApvtsField(2, 1, 15, ParamIndex::FxOutLevel,  0.0f, 100.0f, 1.0f);
            bindApvtsField(4, 1, 22, ParamIndex::FxParam1,    0.0f,  75.0f, 1.0f);
            bindApvtsField(5, 1, 28, ParamIndex::FxParam2,    0.0f,  99.0f, 1.0f);
            bindApvtsField(6, 1, 34, ParamIndex::FxParam3,    0.0f,  99.0f, 1.0f);
            bindApvtsField(7, 1, 39, ParamIndex::FxStereoMix, 0.0f,   1.0f, 1.0f);
            break;
        }

        case MenuPage::RhythmPlayEdit: {
            const int ptnIdx = std::clamp(static_cast<int>(std::lround(pBank.rhythmPattern->load())), 0, 99);
            const char rBank = (pBank.rhythmBank->load() >= 0.5f) ? 'I' : 'P';
            const int bpm    = static_cast<int>(std::lround(pBank.rhythmTempo->load()));
            const int rVol   = static_cast<int>(std::lround(pBank.rhythmVolume->load()));
            const bool run   = pBank.rhythmPlay->load() >= 0.5f;
            const auto& ptn  = processor_.activeRomBank().factoryRhythmPatterns[static_cast<std::size_t>(ptnIdx)];
            const auto dNameSv = synth.drums().instrumentName(static_cast<std::size_t>(selectedDrumInst_));
            const int dLvl = static_cast<int>(std::lround(pBank.drums[static_cast<std::size_t>(selectedDrumInst_)].level->load()));
            const int dPan = static_cast<int>(std::lround(pBank.drums[static_cast<std::size_t>(selectedDrumInst_)].pan->load()));

            if (lastStruckMidiNote_ >= 0) {
                char noteName[5]{};
                formatMidiNoteName(lastStruckMidiNote_, noteName);
                if (lastStruckDrumIdx_ >= 0 && lastStruckDrumIdx_ < 61) {
                    const auto hitName = synth.drums().instrumentName(static_cast<std::size_t>(lastStruckDrumIdx_));
                    std::snprintf(tmp, sizeof(tmp), "RHY KEY:%-3s(%02d) %-15.*s VEL:%03d",
                                  noteName, lastStruckMidiNote_,
                                  static_cast<int>(hitName.size()), hitName.data(),
                                  lastStruckDrumVel_);
                } else {
                    std::snprintf(tmp, sizeof(tmp), "RHY KEY:%-3s(%02d) [UNASSIGNED]    VEL:%03d",
                                  noteName, lastStruckMidiNote_, lastStruckDrumVel_);
                }
            } else {
                std::snprintf(tmp, sizeof(tmp), "RHY %c%03d [%d/%d %dB]  %s T:%03d V:%02d",
                              rBank, ptnIdx + 1,
                              static_cast<int>(ptn.timeSigNum), static_cast<int>(ptn.timeSigDen),
                              static_cast<int>(ptn.bars),
                              run ? "RUN " : "STOP", bpm, rVol);
            }
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "PTN:%02d %s BPM:%03d D%02d:%-9.*s L%02d%+d",
                          ptnIdx, run ? "RUN" : "STP", bpm, selectedDrumInst_ + 1,
                          static_cast<int>(dNameSv.size()), dNameSv.data(), dLvl, dPan);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(0, 1, 4,  ParamIndex::RhythmPattern, 0.0f,  99.0f, 1.0f);
            bindApvtsField(1, 1, 7,  ParamIndex::RhythmPlay,    0.0f,   1.0f, 1.0f);
            bindApvtsField(2, 1, 15, ParamIndex::RhythmTempo,  30.0f, 240.0f, 1.0f);
            bindCustomInt(4, 1, 20, 0, 60, [this]() { return selectedDrumInst_; },
                          [this](int v) { selectedDrumInst_ = std::clamp(v, 0, 60); });
            bindApvtsField(6, 1, 34, drumParam(selectedDrumInst_, 0u),  0.0f, 15.0f, 1.0f);
            bindApvtsField(7, 1, 36, drumParam(selectedDrumInst_, 1u), -3.0f,  3.0f, 1.0f);
            break;
        }

        case MenuPage::RhythmJob: {
            static constexpr std::array<const char*, 4> kKeyAssignLines = {
                "Type: [PRESET 1] (RX Layout)",
                "Type: [PRESET 2] (C3 Perf Layout)",
                "Type: [PRESET 3] (Clavinova)",
                "Type: [USER / SEQ] (Seq 1-61 C1-C6)"
            };
            const int jobNum    = std::clamp(rhythmJobNumber_, 1, 14);
            const int layoutIdx = std::clamp(processor_.rhythmKeyAssignLayout(), 0, 3);

            if (jobNum == 13) {
                if (lastStruckMidiNote_ >= 0 && lastStruckDrumIdx_ >= 0 && lastStruckDrumIdx_ < 61) {
                    char noteName[5]{};
                    formatMidiNoteName(lastStruckMidiNote_, noteName);
                    const auto hitName = synth.drums().instrumentName(static_cast<std::size_t>(lastStruckDrumIdx_));
                    std::snprintf(tmp, sizeof(tmp), "RHYTHM JOB: KEY ASSIGN  %-3s:%-12.*s",
                                  noteName, static_cast<int>(hitName.size()), hitName.data());
                    writeFixedCol(line0, 0, tmp, 40);
                } else {
                    writeFixedCol(line0, 0, "RHYTHM JOB: KEY ASSIGN         [J13/14]", 40);
                }
                writeFixedCol(line1, 0, kKeyAssignLines[static_cast<std::size_t>(layoutIdx)], 40);

                bindCustomInt(0, 1, 7, 0, 3,
                              [this]() { return std::clamp(processor_.rhythmKeyAssignLayout(), 0, 3); },
                              [this](int v) {
                                  lastStruckMidiNote_ = -1;
                                  lastStruckDrumIdx_  = -1;
                                  processor_.setRhythmKeyAssignLayout(std::clamp(v, 0, 3));
                              });
            } else {
                const auto jTitle = vf50::state::VF50StateMachine::rhythmJobTitle(jobNum);
                const auto jHint  = vf50::state::VF50StateMachine::rhythmJobActionHint(jobNum);
                std::snprintf(tmp, sizeof(tmp), "RHYTHM %-23.*s [J%02d/14]",
                              static_cast<int>(jTitle.size()), jTitle.data(), jobNum);
                writeFixedCol(line0, 0, tmp, 40);
                std::snprintf(tmp, sizeof(tmp), "%-40.*s",
                              static_cast<int>(jHint.size()), jHint.data());
                writeFixedCol(line1, 0, tmp, 40);

                bindCustomInt(0, 0, 33, 1, 14,
                              [this]() { return std::clamp(rhythmJobNumber_, 1, 14); },
                              [this](int v) {
                                  rhythmJobNumber_ = std::clamp(v, 1, 14);
                                  processor_.stateMachine().setRhythmJobNumber(rhythmJobNumber_);
                              });
            }

            bindCustomInt(7, 0, 33, 1, 14,
                          [this]() { return std::clamp(rhythmJobNumber_, 1, 14); },
                          [this](int v) {
                              rhythmJobNumber_ = std::clamp(v, 1, 14);
                              processor_.stateMachine().setRhythmJobNumber(rhythmJobNumber_);
                          });
            break;
        }

        case MenuPage::SequencerDemo: {
            const bool playing = processor_.isDemoPlaying();
            const int maxSongs = 8;
            const int songIdx  = std::clamp(processor_.activeDemoSongIndex(), 0, maxSongs - 1);
            const auto titleSv = processor_.activeDemoSongTitle();
            const int bpm      = processor_.activeDemoTempoBpm();
            const int meas     = std::clamp(processor_.activeDemoMeasure(), 1, 999);
            const int tsNum    = std::clamp(processor_.activeDemoTimeSigNum(), 1, 16);
            const int tsDen    = std::clamp(processor_.activeDemoTimeSigDen(), 1, 16);

            std::snprintf(tmp, sizeof(tmp), "S%02d:%-12.*s M%03d %2d/%-2d T=%5.1f",
                          songIdx + 1,
                          static_cast<int>(std::min<std::size_t>(titleSv.size(), 12u)), titleSv.data(),
                          meas,
                          tsNum, tsDen,
                          static_cast<double>(bpm));
            writeFixedCol(line0, 0, tmp, 40);

            char trkChars[9]{};
            char lvlChars[9]{};
            for (std::size_t t = 0u; t < 8u; ++t) {
                const bool muted = processor_.isSequencerTrackMuted(t);
                trkChars[t] = muted ? '_' : static_cast<char>('1' + t);
                const uint8_t lvl = muted ? 0u : processor_.sequencerTrackMeterLevel(t);
                lvlChars[t] = (lvl >= 1u && lvl <= 8u) ? static_cast<char>(lvl) : ' ';
            }
            const bool rhyMuted = processor_.isSequencerTrackMuted(8u);
            trkChars[8] = rhyMuted ? '_' : 'R';
            const uint8_t rhyLvl = rhyMuted ? 0u : processor_.sequencerTrackMeterLevel(8u);
            lvlChars[8] = (rhyLvl >= 1u && rhyLvl <= 8u) ? static_cast<char>(rhyLvl) : ' ';

            const char* statusStr = playing ? "[PLAY]" : (processor_.isDawSequencerArmed() ? "[REC]" : "[STOP]");
            // 40 columns: TR: 1| 2| 3| 4| 5| 6| 7| 8| R|    [PLAY]
            std::snprintf(tmp, sizeof(tmp), "TR: %c%c %c%c %c%c %c%c %c%c %c%c %c%c %c%c %c%c    %6s",
                          trkChars[0], lvlChars[0],
                          trkChars[1], lvlChars[1],
                          trkChars[2], lvlChars[2],
                          trkChars[3], lvlChars[3],
                          trkChars[4], lvlChars[4],
                          trkChars[5], lvlChars[5],
                          trkChars[6], lvlChars[6],
                          trkChars[7], lvlChars[7],
                          trkChars[8], lvlChars[8],
                          statusStr);
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 0, 1, 0, maxSongs - 1,
                          [songIdx]() { return songIdx; },
                          [this, playing](int v) {
                              const auto idx = static_cast<std::size_t>(std::max(0, v));
                              if (playing) {
                                  processor_.requestDemoStart(idx);
                              } else {
                                  processor_.selectDemoSong(idx);
                              }
                          });
            bindCustomInt(5, 1, 35, 0, 1,
                          [playing]() { return playing ? 1 : 0; },
                          [this, playing](int v) {
                              if ((v != 0) != playing) {
                                  processor_.requestDemoToggle();
                              }
                          });
            break;
        }

        case MenuPage::SequencerTrackSetup: {
            selectedSeqTrack_ = std::clamp(selectedSeqTrack_, 0, 7);
            auto& trk = processor_.stateMachine().trackSetup(static_cast<std::size_t>(selectedSeqTrack_));
            const bool dawSync = processor_.isDawSyncEnabled();

            std::snprintf(tmp, sizeof(tmp), "SEQ TRACK %d SETUP   SYNC:%-3s    [SONG]",
                          selectedSeqTrack_ + 1, dawSync ? "DAW" : "INT");
            writeFixedCol(line0, 0, tmp, 40);

            std::snprintf(tmp, sizeof(tmp), "T%d CH:%02d V:%c%02d %-4s VOL:%03d     [BACK]",
                          selectedSeqTrack_ + 1,
                          static_cast<int>(trk.transmitCh0To15) + 1,
                          (trk.voiceBank == 0u) ? 'I' : 'P',
                          static_cast<int>(trk.voiceNumber),
                          trk.enabled ? "PLAY" : "MUTE",
                          static_cast<int>(trk.volume));
            writeFixedCol(line1, 0, tmp, 40);

            bindCustomInt(0, 1, 1, 0, 7,
                          [this]() { return selectedSeqTrack_; },
                          [this](int v) {
                              selectedSeqTrack_ = std::clamp(v, 0, 7);
                              processor_.stateMachine().setSelectedSequencerTrack(selectedSeqTrack_);
                          });
            bindCustomInt(1, 1, 6, 1, 16,
                          [this]() {
                              return static_cast<int>(processor_.stateMachine()
                                                          .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                                          .transmitCh0To15) + 1;
                          },
                          [this](int v) {
                              processor_.stateMachine()
                                  .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                  .transmitCh0To15 = static_cast<uint8_t>(std::clamp(v - 1, 0, 15));
                          });
            bindCustomInt(2, 1, 12, 0, 99,
                          [this]() {
                              return static_cast<int>(processor_.stateMachine()
                                                          .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                                          .voiceNumber);
                          },
                          [this](int v) {
                              processor_.stateMachine()
                                  .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                  .voiceNumber = static_cast<uint8_t>(std::clamp(v, 0, 99));
                          });
            bindCustomInt(3, 1, 15, 0, 1,
                          [this]() {
                              return processor_.stateMachine()
                                         .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                         .enabled ? 0 : 1;
                          },
                          [this](int v) {
                              processor_.stateMachine()
                                  .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                  .enabled = (v == 0);
                          });
            bindCustomInt(4, 1, 24, 0, 99,
                          [this]() {
                              return static_cast<int>(processor_.stateMachine()
                                                          .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                                          .volume);
                          },
                          [this](int v) {
                              processor_.stateMachine()
                                  .trackSetup(static_cast<std::size_t>(selectedSeqTrack_))
                                  .volume = static_cast<uint8_t>(std::clamp(v, 0, 99));
                          });
            bindCustomInt(6, 0, 25, 0, 1,
                          [this]() { return processor_.isDawSyncEnabled() ? 1 : 0; },
                          [this](int v) { processor_.setDawSyncEnabled(v != 0); });
            break;
        }

        case MenuPage::JobMenu: {
            writeFixedCol(line0, 0, "WORKSTATION INITIALIZE JOBS (SOFT KEYS)", 40);
            writeFixedCol(line1, 0, "[INIT VCE] [PF:SNGL] [PF:SPLT] [PF:DUAL]", 40);
            break;
        }

        case MenuPage::UtilCard: {
            std::snprintf(tmp, sizeof(tmp), "UTILITY CARD: MCD64 [VF50SN] READY");
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "ACTIVE BANK:%-6s  100 VCE / 100 PERF",
                          cardBankVisualActive_ ? "CARD" : ((pBank.voiceBank->load() >= 0.5f) ? "INT" : "PRESET"));
            writeFixedCol(line1, 0, tmp, 40);
            bindCustomInt(0, 1, 12, 0, 2,
                          [this, &pBank]() { return cardBankVisualActive_ ? 1 : ((pBank.voiceBank->load() >= 0.5f) ? 0 : 2); },
                          [this](int v) { handleMemoryBankSelect(v); });
            break;
        }

        case MenuPage::UtilMidi: {
            const int sch = static_cast<int>(std::lround(pBank.singleReceiveCh->load()));
            const int rch = static_cast<int>(std::lround(pBank.rhythmReceiveCh->load()));
            char sBuf[6]{}, rBuf[6]{};
            if (sch == 16) std::snprintf(sBuf, sizeof(sBuf), "OMNI");
            else std::snprintf(sBuf, sizeof(sBuf), "CH%02d", sch + 1);
            if (rch == 16) std::snprintf(rBuf, sizeof(rBuf), "OMNI");
            else if (rch >= 17) std::snprintf(rBuf, sizeof(rBuf), "OFF ");
            else std::snprintf(rBuf, sizeof(rBuf), "CH%02d", rch + 1);

            writeFixedCol(line0, 0, "UTILITY MIDI  SYSEX:VF50 + TX81Z    ON", 40);
            std::snprintf(tmp, sizeof(tmp), "SINGLE RECV:%s   RHYTHM RECV:%s", sBuf, rBuf);
            writeFixedCol(line1, 0, tmp, 40);

            bindApvtsField(1, 1, 12, ParamIndex::SingleReceiveCh, 0.0f, 16.0f, 1.0f);
            bindApvtsField(5, 1, 31, ParamIndex::RhythmReceiveCh, 0.0f, 17.0f, 1.0f);
            break;
        }

        case MenuPage::UtilDisk: {
            writeFixedCol(line0, 0, "UT DISK)  Use DISK key for 40x2 LCD     ", 40);
            writeFixedCol(line1, 0, " Load  Save  Dir   Job   Format         ", 40);
            break;
        }

        case MenuPage::UtilMemProtect: {
            const bool protInt  = processor_.isIntMemoryProtected();
            const bool protCard = processor_.isCardMemoryProtected();
            writeFixedCol(line0, 0, "UTILITY INTERNAL/CARD MEMORY PROTECT", 40);
            std::snprintf(tmp, sizeof(tmp), "Memory Protect = INT:%-3s  CARD:%-3s",
                          protInt ? "ON" : "OFF", protCard ? "ON" : "OFF");
            writeFixedCol(line1, 0, tmp, 40);
            if (protInt || protCard) {
                line0[38] = static_cast<char>(0x0Cu); // Custom lock icon
            }
            bindCustomInt(0, 1, 21, 0, 1,
                          [this]() { return processor_.isIntMemoryProtected() ? 1 : 0; },
                          [this](int v) { processor_.setIntMemoryProtected(v != 0); });
            bindCustomInt(6, 1, 31, 0, 1,
                          [this]() { return processor_.isCardMemoryProtected() ? 1 : 0; },
                          [this](int v) { processor_.setCardMemoryProtected(v != 0); });
            break;
        }

        case MenuPage::UtilSetup: {
            const int subPage = std::clamp(utilSetupSubPage_, 0, 2);
            if (subPage == 0) {
                // Page 1: UTILITY SETUP: Master Tune = +0.0c, Transpose = C3
                const float tune   = pBank.masterTune->load();
                const int   trSemi = std::clamp(processor_.globalTransposeSemitones(), -24, 24);
                char noteBuf[5]{};
                formatMidiNoteName(60 + trSemi, noteBuf);
                char tuneBuf[16]{};
                std::snprintf(tuneBuf, sizeof(tuneBuf), "%+.1fc", tune);

                writeFixedCol(line0, 0, "UTILITY SETUP: Master Tune / Transpose", 40);
                std::snprintf(tmp, sizeof(tmp), "Master Tune = %s, Transpose = %-3s", tuneBuf, noteBuf);
                writeFixedCol(line1, 0, tmp, 40);

                const int transposeCol = 14 + static_cast<int>(std::strlen(tuneBuf)) + 14;
                bindApvtsField(0, 1, 14, ParamIndex::MasterTune, -75.0f, 75.0f, 0.5f);
                bindCustomInt(6, 1, std::clamp(transposeCol, 0, 39), -24, 24,
                              [this]() { return processor_.globalTransposeSemitones(); },
                              [this](int v) { processor_.setGlobalTransposeSemitones(std::clamp(v, -24, 24)); });
            } else if (subPage == 1) {
                // Page 2: UTILITY SETUP: Memory Protect = INT:OFF  CARD:OFF
                const bool protInt  = processor_.isIntMemoryProtected();
                const bool protCard = processor_.isCardMemoryProtected();
                writeFixedCol(line0, 0, "UTILITY SETUP: Memory Protect", 40);
                std::snprintf(tmp, sizeof(tmp), "Memory Protect = INT:%-3s  CARD:%-3s",
                              protInt ? "ON" : "OFF", protCard ? "ON" : "OFF");
                writeFixedCol(line1, 0, tmp, 40);
                if (protInt || protCard) {
                    line0[38] = static_cast<char>(0x0Cu);
                }
                bindCustomInt(0, 1, 21, 0, 1,
                              [this]() { return processor_.isIntMemoryProtected() ? 1 : 0; },
                              [this](int v) { processor_.setIntMemoryProtected(v != 0); });
                bindCustomInt(6, 1, 31, 0, 1,
                              [this]() { return processor_.isCardMemoryProtected() ? 1 : 0; },
                              [this](int v) { processor_.setCardMemoryProtected(v != 0); });
            } else {
                // Page 3: UTILITY SETUP: Controllers (PB Range 0-12, Mod Wheel, Aftertouch, Foot, Breath)
                const int pb   = std::clamp(static_cast<int>(std::lround(pBank.voicePitchBendRange->load())), 0, 12);
                const int mw   = std::clamp(static_cast<int>(std::lround(pBank.ctrlMwPitch->load())), 0, 99);
                const int at   = std::clamp(static_cast<int>(std::lround(pBank.ctrlAtPitch->load())), 0, 99);
                const int foot = std::clamp(static_cast<int>(std::lround(pBank.ctrlFcVol->load())), 0, 99);
                const int brth = std::clamp(static_cast<int>(std::lround(pBank.ctrlBcPitch->load())), 0, 99);

                writeFixedCol(line0, 0, "UTILITY SETUP: Controllers", 40);
                std::snprintf(tmp, sizeof(tmp), "PB:%02d MW:%02d AT:%02d FOOT:%02d BREATH:%02d",
                              pb, mw, at, foot, brth);
                writeFixedCol(line1, 0, tmp, 40);

                bindApvtsField(0, 1, 3,  ParamIndex::VoicePitchBendRange, 0.0f, 12.0f, 1.0f);
                bindApvtsField(1, 1, 9,  ParamIndex::CtrlMwPitch,         0.0f, 99.0f, 1.0f);
                bindApvtsField(2, 1, 15, ParamIndex::CtrlAtPitch,         0.0f, 99.0f, 1.0f);
                bindApvtsField(4, 1, 23, ParamIndex::CtrlFcVol,           0.0f, 99.0f, 1.0f);
                bindApvtsField(6, 1, 33, ParamIndex::CtrlBcPitch,         0.0f, 99.0f, 1.0f);
            }
            break;
        }

        case MenuPage::UtilOthers: {
            std::snprintf(tmp, sizeof(tmp), "SYSTEM  DUAL YM2424 + YM3602 + YM3413");
            writeFixedCol(line0, 0, tmp, 40);
            std::snprintf(tmp, sizeof(tmp), "FM VOICES:%02d/16  DRUM VOICES:%d/8  SR:%2dk",
                          processor_.activeFmVoices(), processor_.activeDrumVoices(),
                          static_cast<int>(std::lround(synth.hostSampleRate() / 1000.0)));
            writeFixedCol(line1, 0, tmp, 40);
            break;
        }

        case MenuPage::StoreConfirm: {
            storeTargetSlot_ = processor_.stateMachine().pendingStoreSlot();
            const bool isPerf = (synth.playMode() == vf50::dsp::fm::SynthPlayMode::Performance);
            std::snprintf(tmp, sizeof(tmp), "Store to INT %02d?  [%-10.10s] %s",
                          storeTargetSlot_,
                          isPerf ? curPerf.name.data() : curVoice.name.data(),
                          isPerf ? "PFM" : "VCE");
            writeFixedCol(line0, 0, tmp, 40);
            writeFixedCol(line1, 0, "Press [+1/YES] to Confirm  [-1/NO]:Exit", 40);

            bindCustomInt(0, 0, 13, 0, 99, [this]() { return processor_.stateMachine().pendingStoreSlot(); },
                          [this](int v) {
                              storeTargetSlot_ = std::clamp(v, 0, 99);
                              processor_.stateMachine().setPendingStoreSlot(storeTargetSlot_);
                          });
            break;
        }
    }

    // Universal temporary LCD status toast (drag-and-drop LOADED:, MIDI export, store completion, etc.)
    if (diskStatusTicks_ > 0u && diskStatusMsg_.isNotEmpty() && page_ != MenuPage::StoreConfirm) {
        line1.fill(' ');
        const auto statusUtf8 = diskStatusMsg_.toStdString();
        std::snprintf(tmp, sizeof(tmp), "%-40.40s", statusUtf8.c_str());
        writeFixedCol(line1, 0, tmp, 40);
    }

    // Override Row 1 when INST ON/OFF soft-key bar mode is active!
    if (instOnOffSoftMode_) {
        line1.fill(' ');
        for (int p = 0; p < 8; ++p) {
            const bool on = pBank.parts[static_cast<std::size_t>(p)].maxNotes->load() >= 0.5f;
            std::snprintf(tmp, sizeof(tmp), "%d:%s ", p + 1, on ? "ON" : "--");
            writeFixedCol(line1, p * 5, tmp, 5);
        }
    }

    // Authentic Ch. 1.5 Voice Edit operator status prefix "e1111" (pp. 7, 38)
    if (page_ >= MenuPage::SingleQuickEdit
        && page_ <= MenuPage::SingleFunction) {
        const std::array<char, 6> opHdr = {
            'e',
            (pBank.ops[0].enable->load(std::memory_order_relaxed) >= 0.5f) ? '1' : '0',
            (pBank.ops[1].enable->load(std::memory_order_relaxed) >= 0.5f) ? '1' : '0',
            (pBank.ops[2].enable->load(std::memory_order_relaxed) >= 0.5f) ? '1' : '0',
            (pBank.ops[3].enable->load(std::memory_order_relaxed) >= 0.5f) ? '1' : '0',
            ' '
        };
        writeFixedCol(line0, 0, opHdr.data(), 6);
    }

    // Authentic flashing [COMPARE] indicator or keypad tens digit / stereo peak meters on Row 0
    const bool compareActive = isPerformanceContextForPage()
                                   ? processor_.hostMemory().isPerformanceCompareActive()
                                   : processor_.hostMemory().isSingleCompareActive();
    if (compareActive) {
        writeFixedCol(line0, 31, compareBlinkVisible_ ? "[COMPARE]" : "         ", 9);
    } else if (pendingTensDigit_ >= 0) {
        std::snprintf(tmp, sizeof(tmp), "[%d_]", pendingTensDigit_);
        writeFixedCol(line0, 36, tmp, 4);
    } else if (page_ == MenuPage::SinglePlay || page_ == MenuPage::PerfPlay) {
        // Real-time stereo peak meters using custom CGRAM vertical bar-graph glyphs 0x01..0x08!
        const int barL = std::clamp(static_cast<int>(std::ceil(processor_.meterLeft() * 8.0f)), 0, 8);
        const int barR = std::clamp(static_cast<int>(std::ceil(processor_.meterRight() * 8.0f)), 0, 8);
        line0[38] = (barL > 0) ? static_cast<char>(barL) : ' ';
        line0[39] = (barR > 0) ? static_cast<char>(barR) : ' ';
    }

    lcd_.setLines(std::string_view(line0.data(), 40), std::string_view(line1.data(), 40));

    // Position hardware HD44780 blinking cursor on the focused parameter field
    if (activeFieldCount_ > 0) {
        focusedFieldIdx_ = std::clamp(focusedFieldIdx_, 0, activeFieldCount_ - 1);
        if (!pageFields_[static_cast<std::size_t>(focusedFieldIdx_)].valid) {
            for (int i = 0; i < activeFieldCount_; ++i) {
                if (pageFields_[static_cast<std::size_t>(i)].valid) {
                    focusedFieldIdx_ = i;
                    break;
                }
            }
        }
        const auto& fld = pageFields_[static_cast<std::size_t>(focusedFieldIdx_)];
        if (fld.valid) {
            lcd_.setCursor(fld.cursorRow, fld.cursorCol,
                           (page_ == MenuPage::StoreConfirm)
                               ? VF50LcdDisplay::CursorMode::BlinkingBlock
                               : VF50LcdDisplay::CursorMode::BlinkingUnderline);
            return;
        }
    }
    lcd_.setCursor(0, 0, VF50LcdDisplay::CursorMode::None);
}

// =============================================================================
// Procedural 2x Supersampled Vector Chassis, Silkscreen Typography & LEDs
// =============================================================================
void VF50MainCanvas::paint(juce::Graphics& g) {
    paintChassisAndGroove(g);
    paintHeaderBranding(g);
    paintLeftColumnPanelsAndLegends(g);
    paintBelowDisplaySilkscreenAndLeds(g);
    paintRightOfDisplaySilkscreenAndLeds(g);
}

void VF50MainCanvas::paintChassisAndGroove(juce::Graphics& g) const {
    const float w = static_cast<float>(tokens::kVirtualWidth);
    const float h = static_cast<float>(tokens::kVirtualHeight);
    const float grooveY = static_cast<float>(tokens::kHeaderContourGrooveY2x);

    // 1. Top Anodized Header Deck (y = 0 .. 104)
    {
        juce::ColourGradient hdrGrad(juce::Colour(0xFF31363Du), 0.0f, 0.0f,
                                     juce::Colour(tokens::kHeaderBarBg), 0.0f, grooveY, false);
        g.setGradientFill(hdrGrad);
        g.fillRect(0.0f, 0.0f, w, grooveY);
    }

    // 2. Main Sculpted Charcoal-Grey Anodized Metal Control Deck (y = 104 .. 880)
    {
        juce::ColourGradient deckGrad(juce::Colour(tokens::kChassisAnodizedTop).interpolatedWith(
                                          juce::Colour(tokens::kChassisBase), 0.45f),
                                      0.0f, grooveY + 6.0f,
                                      juce::Colour(tokens::kChassisBottom),
                                      0.0f, h, false);
        deckGrad.addColour(0.28, juce::Colour(tokens::kChassisBase));
        g.setGradientFill(deckGrad);
        g.fillRect(0.0f, grooveY, w, h - grooveY);
    }

    // 3. Procedural Horizontal Brushed Metallic Micro-Texture (100% Vector Hairlines)
    {
        juce::RectangleList<float> lightGrain;
        juce::RectangleList<float> darkGrain;
        lightGrain.ensureStorageAllocated(220);
        darkGrain.ensureStorageAllocated(220);

        uint32_t lfsr = 0x56353033u; // Deterministic hardware grain seed
        for (int y = 4; y < tokens::kVirtualHeight; y += 4) {
            lfsr ^= lfsr << 13u;
            lfsr ^= lfsr >> 17u;
            lfsr ^= lfsr << 5u;
            const float yf = static_cast<float>(y);
            if ((lfsr & 3u) == 0u) {
                lightGrain.addWithoutMerging(juce::Rectangle<float>(0.0f, yf, w, 1.0f));
            } else if ((lfsr & 3u) == 1u) {
                darkGrain.addWithoutMerging(juce::Rectangle<float>(0.0f, yf, w, 1.0f));
            }
        }
        g.setColour(juce::Colours::white.withAlpha(0.016f));
        g.fillRectList(lightGrain);
        g.setColour(juce::Colours::black.withAlpha(0.028f));
        g.fillRectList(darkGrain);
    }

    // 4. Signature VF50 Horizontal Sculpted Contour Groove (y = 104 @ 2x)
    g.setColour(juce::Colour(0xFF0A0C0Fu));
    g.fillRect(0.0f, grooveY - 3.0f, w, 5.0f);
    g.setColour(juce::Colour(tokens::kGrooveShadow));
    g.fillRect(0.0f, grooveY + 2.0f, w, 3.0f);
    g.setColour(juce::Colour(tokens::kGrooveHighlight));
    g.fillRect(0.0f, grooveY + 5.0f, w, 2.2f);

    // Subtle outer chassis bevel border
    g.setColour(juce::Colour(0xFF434A54u));
    g.drawLine(0.0f, 1.0f, w, 1.0f, 2.0f);
    g.setColour(juce::Colour(0xFF0E1014u));
    g.drawLine(0.0f, h - 1.5f, w, h - 1.5f, 3.0f);
}

void VF50MainCanvas::paintHeaderBranding(juce::Graphics& g) const {
    // VICTORY (Warm White) + VF50 (Teal) + Crimson VF50 Hardware Badge
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(36.0f, true, 1.06f));
    g.drawText("VICTORY", 44, 24, 194, 52, juce::Justification::centredLeft, false);

    g.setColour(juce::Colour(tokens::kSilkAccentTeal));
    g.drawText("VF50", 236, 24, 114, 52, juce::Justification::centredLeft, false);

    // Crimson VF50 Badge
    const juce::Rectangle<float> vf50Badge(356.0f, 27.0f, 110.0f, 46.0f);
    g.setColour(juce::Colour(tokens::kSilkCrimsonBadge));
    g.fillRoundedRectangle(vf50Badge, 5.0f);
    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(31.0f, true, 1.05f));
    g.drawText("VF50", vf50Badge, juce::Justification::centred, false);

    // Subtitle
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(19.0f, true, 1.04f));
    g.drawText("DIGITAL SYNTHESIZER WORKSTATION", 482, 27, 650, 26, juce::Justification::centredLeft, false);

    g.setColour(juce::Colour(tokens::kSilkDimLabel));
    g.setFont(VF50LookAndFeel::makeFont(14.5f, true, 1.02f));
    g.drawText("16-VOICE DUAL OPZ II FM  |  8-VOICE PCM RHYTHM  |  YM3413 DIGITAL DSP",
               482, 53, 730, 22, juce::Justification::centredLeft, false);

    // Top Header Virtual 3.5" Floppy Drive Status Box (alongside DISK IN / FOLDER / USER DISK / SET DIR / EJECT, ROM SETUP & SCALE)
    {
        const juce::Rectangle<float> floppyBox(1214.0f, 24.0f, 316.0f, 54.0f);
        g.setColour(juce::Colour(0xFF171B20u));
        g.fillRoundedRectangle(floppyBox, 6.0f);
        g.setColour(juce::Colour(0xFF333942u));
        g.drawRoundedRectangle(floppyBox, 6.0f, 1.5f);

        const bool mounted = diskManager_.isMounted();
        VF50LookAndFeel::drawHardwareLed(g, { 1236.0f, 51.0f }, 5.5f, false, mounted);

        const juce::String rawName = diskManager_.mountedSourceName();
        const char* rawUtf8 = rawName.toRawUTF8();
        const juce::String cleanDiskName = vf50::disk::VF50DiskManager::sanitizeAsciiString(
            reinterpret_cast<const uint8_t*>(rawUtf8),
            static_cast<int>(std::strlen(rawUtf8)));

        g.setFont(VF50LookAndFeel::makeFont(14.5f, true));
        g.setColour(juce::Colour(tokens::kSilkDimLabel));
        g.drawText("3.5\" FD:", 1252, 30, 82, 42, juce::Justification::centredLeft, false);
        g.setColour(juce::Colour(mounted ? tokens::kSilkAccentTeal : tokens::kSilkMutedSilver));
        g.drawText(mounted ? (cleanDiskName.isNotEmpty() ? cleanDiskName : juce::String("MOUNTED"))
                           : juce::String("[NO DISK]"),
                   1336, 30, 186, 42, juce::Justification::centredLeft, true);
    }

    // UI Scale Header Caption
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(14.5f, true));
    g.drawText("SCALE", 2436, 32, 68, 40, juce::Justification::centredRight, false);
}

void VF50MainCanvas::paintLeftColumnPanelsAndLegends(juce::Graphics& g) const {
    // 1. MEMORY CARD Slot Housing (kCardHousingRect2x = 44, 132, 280, 94)
    const auto cardBox = toRect(tokens::kCardHousingRect2x).toFloat();
    g.setColour(juce::Colour(tokens::kPanelSubFrameBg));
    g.fillRoundedRectangle(cardBox, 6.0f);
    g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
    g.drawRoundedRectangle(cardBox, 6.0f, 1.8f);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(16.5f, true, 1.05f));
    g.drawText("CARD", static_cast<int>(cardBox.getX()), static_cast<int>(cardBox.getY() + 6.0f),
               static_cast<int>(cardBox.getWidth()), 24, juce::Justification::centred, false);

    // Recessed RAM Card Insertion Well + Inserted MCD64 Cartridge Edge
    const auto slotWell = toRect(tokens::kCardSlotWellRect2x).toFloat();
    g.setColour(juce::Colour(tokens::kPanelRecessWell));
    g.fillRoundedRectangle(slotWell, 4.0f);

    const auto cartEdge = slotWell.reduced(8.0f, 6.0f);
    juce::ColourGradient cartGrad(juce::Colour(0xFF2B3038u), cartEdge.getX(), cartEdge.getY(),
                                  juce::Colour(0xFF191D22u), cartEdge.getX(), cartEdge.getBottom(), false);
    g.setGradientFill(cartGrad);
    g.fillRoundedRectangle(cartEdge, 3.0f);
    g.setColour(juce::Colour(cardBankVisualActive_ ? tokens::kSilkAccentTeal : 0xFF454C56u));
    g.drawRoundedRectangle(cartEdge, 3.0f, 1.5f);

    g.setColour(juce::Colour(cardBankVisualActive_ ? tokens::kSilkAccentTeal : tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true));
    g.drawText("MCD64 MEMORY CARD", cartEdge, juce::Justification::centred, false);

    // 2. Recessed Fader Bay (VOLUME & DATA/TEMPO, kFaderPanelRect2x = 44, 250, 280, 356)
    const auto faderBay = toRect(tokens::kFaderPanelRect2x).toFloat();
    g.setColour(juce::Colour(tokens::kPanelSubFrameBg));
    g.fillRoundedRectangle(faderBay, 6.0f);
    g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
    g.drawRoundedRectangle(faderBay, 6.0f, 1.8f);

    // Vertical divider line between VOLUME and DATA/TEMPO
    g.setColour(juce::Colour(0xFF2D333Bu));
    g.drawLine(faderBay.getCentreX(), faderBay.getY() + 14.0f,
               faderBay.getCentreX(), faderBay.getBottom() - 14.0f, 1.5f);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(16.0f, true, 0.96f));
    g.drawText("VOLUME", 52, 566, 124, 28, juce::Justification::centred, false);
    g.drawText("DATA/TEMPO", 178, 566, 134, 28, juce::Justification::centred, false);

    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(13.0f, true));
    g.drawText("MAX", 84, 256, 60, 20, juce::Justification::centred, false);
    g.drawText("MAX", 212, 256, 60, 20, juce::Justification::centred, false);
    g.drawText("MIN", 84, 548, 60, 20, juce::Justification::centred, false);
    g.drawText("MIN", 212, 548, 60, 20, juce::Justification::centred, false);

    // 3. PITCH & MODULATION WheelBezels & Legends
    const auto pwRect = toRect(tokens::kPitchWheelRect2x).toFloat();
    const auto mwRect = toRect(tokens::kModWheelRect2x).toFloat();

    // Recessed wheel bay surround
    const juce::Rectangle<float> wheelBay(44.0f, 616.0f, 280.0f, 224.0f);
    g.setColour(juce::Colour(tokens::kPanelSubFrameBg));
    g.fillRoundedRectangle(wheelBay, 6.0f);
    g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
    g.drawRoundedRectangle(wheelBay, 6.0f, 1.8f);

    g.setColour(juce::Colour(tokens::kSilkWarmWhite));
    g.setFont(VF50LookAndFeel::makeFont(15.5f, true, 0.94f));
    g.drawText("PITCH", static_cast<int>(pwRect.getX() - 24.0f), 620,
               static_cast<int>(pwRect.getWidth() + 48.0f), 22, juce::Justification::centred, false);
    g.drawText("MODULATION", static_cast<int>(mwRect.getX() - 36.0f), 620,
               static_cast<int>(mwRect.getWidth() + 72.0f), 22, juce::Justification::centred, false);

    // Hardware front-edge jack silkscreen indicators (^ BREATH / v PHONES)
    g.setColour(juce::Colour(tokens::kSilkMutedSilver));
    g.setFont(VF50LookAndFeel::makeFont(13.5f, true));
    g.drawText("BREATH", static_cast<int>(pwRect.getX() - 20.0f), 808,
               static_cast<int>(pwRect.getWidth() + 40.0f), 22, juce::Justification::centred, false);
    g.drawText("PHONES", static_cast<int>(mwRect.getX() - 20.0f), 808,
               static_cast<int>(mwRect.getWidth() + 40.0f), 22, juce::Justification::centred, false);
}

void VF50MainCanvas::paintBelowDisplaySilkscreenAndLeds(juce::Graphics& g) const {
    const auto classicLeds = classicController_.computeLedSnapshot();

    // 1. INST ON/OFF Silkscreen Bracket under the 8 Soft Keys -> connecting right to [-] Key
    if (softKeys_[0] && softKeys_[3] && softKeys_[4] && softKeys_[7] && btnInstMinus_) {
        const float skLeft   = softKeys_[0]->getBounds().toFloat().getCentreX();
        const float skRight  = softKeys_[7]->getBounds().toFloat().getCentreX();
        const float tickTopY = static_cast<float>(softKeys_[0]->getBottom()) + 2.0f;
        const float brkY     = 332.0f;

        g.setColour(juce::Colour(instOnOffSoftMode_ ? tokens::kSilkAccentTeal : tokens::kBracketLineWhite));
        for (int i = 0; i < 8; ++i) {
            if (const auto& sk = softKeys_[static_cast<std::size_t>(i)]) {
                const float tickX = sk->getBounds().toFloat().getCentreX();
                g.drawLine(tickX, tickTopY, tickX, brkY, 2.0f);
            }
        }
        g.drawLine(skLeft, brkY, skRight, brkY, 2.0f);

        // Routing line from soft-key bracket down to [-] INST ON/OFF key
        const float minusCx = btnInstMinus_->getBounds().toFloat().getCentreX();
        g.drawLine(minusCx, brkY, minusCx, 354.0f, 2.0f);

        // Centered "INST ON/OFF" legend cleanly inside the gap between F4 and F5 ticks
        const float midF4F5 = 0.5f * (softKeys_[3]->getBounds().toFloat().getCentreX()
                                    + softKeys_[4]->getBounds().toFloat().getCentreX());
        const juce::Rectangle<float> lblBg(midF4F5 - 60.0f, brkY - 11.0f, 120.0f, 22.0f);
        g.setColour(juce::Colour(tokens::kChassisBase));
        g.fillRect(lblBg);
        g.setColour(juce::Colour(instOnOffSoftMode_ ? tokens::kSilkAccentTeal : tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(14.5f, true, 0.90f));
        g.drawText("INST ON/OFF", lblBg, juce::Justification::centred, false);

        // Above [-] button: "LETTER" (White) / "ERASE" (Accent Teal)
        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(14.5f, true));
        g.drawText("LETTER", 1166, 356, 110, 18, juce::Justification::centred, false);
        g.setColour(juce::Colour(tokens::kSilkAccentTeal));
        g.drawText("ERASE", 1166, 374, 110, 18, juce::Justification::centred, false);
    }

    // 2. SYSTEM Group Header Bracket + SEQ / RHYTHM Hardware LEDs
    {
        const float sysTopY = static_cast<float>(tokens::kBtnSeqRect2x.y);
        const float sysBrkY = sysTopY - 34.0f;
        const float sysLedY = sysTopY - 15.0f;

        g.setColour(juce::Colour(tokens::kBracketLineWhite));
        g.drawLine(464.0f, sysBrkY, 560.0f, sysBrkY, 2.0f);
        g.drawLine(660.0f, sysBrkY, 756.0f, sysBrkY, 2.0f);
        g.drawLine(464.0f, sysBrkY, 464.0f, sysBrkY + 12.0f, 2.0f);
        g.drawLine(756.0f, sysBrkY, 756.0f, sysBrkY + 12.0f, 2.0f);

        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(16.0f, true));
        g.drawText("SYSTEM", 562, tokens::kBtnSeqRect2x.y - 45, 96, 22, juce::Justification::centred, false);

        drawClassicLedState(g, { 505.0f, sysLedY }, 6.5f, classicLeds.seq, ledBlinkOn_);
        drawClassicLedState(g, { 607.0f, sysLedY }, 6.5f, classicLeds.rhythm, ledBlinkOn_);
    }

    // 2b. Recessed 8-Track SMF Export Sub-Frame (permanently visible host utility)
    {
        const auto expWell = toRect(tokens::kExportWellRect2x).toFloat();
        g.setColour(juce::Colour(0xFF14181Du));
        g.fillRoundedRectangle(expWell, 6.5f);
        g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
        g.drawRoundedRectangle(expWell, 6.5f, 1.6f);
    }

    // 3. SEQUENCER / OPERATOR Dual-Function Row Brackets & 5 Green Hardware LEDs
    {
        const float b0Cx = 683.0f; // Center of button 0 (642 + 41)
        const float opLeft = 738.0f;
        const float opRight = 1108.0f;

        // Teal "OP SELECT" above button 0
        g.setColour(juce::Colour(tokens::kSilkAccentTeal));
        g.setFont(VF50LookAndFeel::makeFont(15.0f, true, 0.94f));
        g.drawText("OP SELECT", 626, 534, 114, 20, juce::Justification::centred, false);

        const bool inOpPage = (page_ == MenuPage::SingleSensitivity || page_ == MenuPage::SingleOscillator ||
                               page_ == MenuPage::SingleEg || page_ == MenuPage::SingleScaling);
        drawClassicLedState(g, { b0Cx, 580.0f }, 6.5f, classicLeds.opSelect, ledBlinkOn_);

        // Teal "OPERATOR ON/OFF" bracket + 1 2 3 4 numbers + 4 Live Operator Enable LEDs!
        g.setColour(juce::Colour(tokens::kBracketLineTeal));
        g.drawLine(opLeft, 544.0f, 828.0f, 544.0f, 2.0f);
        g.drawLine(1018.0f, 544.0f, opRight, 544.0f, 2.0f);
        g.drawLine(opLeft, 544.0f, opLeft, 558.0f, 2.0f);
        g.drawLine(opRight, 544.0f, opRight, 558.0f, 2.0f);

        g.setColour(juce::Colour(tokens::kSilkAccentTeal));
        g.setFont(VF50LookAndFeel::makeFont(15.5f, true, 0.94f));
        g.drawText("OPERATOR ON/OFF", 830, 533, 186, 22, juce::Justification::centred, false);

        for (int op = 0; op < 4; ++op) {
            const float cx = static_cast<float>((tokens::kSeqRowStartX1x + (op + 1) * tokens::kSeqRowStepX1x) * 2 + 41);
            const bool isSelOp = inOpPage && (selectedOperator_ == op);

            g.setColour(juce::Colour(isSelOp ? tokens::kSilkWarmWhite : tokens::kSilkAccentTeal));
            g.setFont(VF50LookAndFeel::makeFont(17.0f, true));
            g.drawText(juce::String(op + 1), static_cast<int>(cx - 26.0f), 564, 24, 24,
                       juce::Justification::centredRight, false);

            drawClassicLedState(g, { cx + 10.0f, 577.0f }, 6.5f,
                                classicLeds.opOnOff[static_cast<std::size_t>(op)], ledBlinkOn_);
        }

        // White "SEQUENCER" bracket & transport sub-labels beneath buttons 0..4
        const float seqLeft  = 642.0f;
        const float seqRight = 1108.0f;
        const float botY     = 728.0f;
        g.setColour(juce::Colour(tokens::kBracketLineWhite));
        g.drawLine(seqLeft, 712.0f, seqLeft, botY, 2.0f);
        g.drawLine(seqRight, 712.0f, seqRight, botY, 2.0f);
        g.drawLine(seqLeft, botY, 805.0f, botY, 2.0f);
        g.drawLine(945.0f, botY, seqRight, botY, 2.0f);

        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(16.0f, true));
        g.drawText("SEQUENCER", 808, 717, 134, 22, juce::Justification::centred, false);

        static constexpr std::array<const char*, 5> kTransSub = { "REC", "BWD", "STOP", "PLAY", "FWD" };
        g.setColour(juce::Colour(tokens::kSilkMutedSilver));
        g.setFont(VF50LookAndFeel::makeFont(13.5f, true));
        for (int i = 0; i < 5; ++i) {
            const int bx = (tokens::kSeqRowStartX1x + i * tokens::kSeqRowStepX1x) * 2;
            g.drawText(kTransSub[static_cast<std::size_t>(i)], bx, 690, 82, 20, juce::Justification::centred, false);
        }
    }

    // 4. Recessed 4x3 Numeric Keypad Sub-Frame Well + "v C3" Audition Indicator
    {
        const auto kpWell = toRect(tokens::kKeypadWellRect2x).toFloat();
        g.setColour(juce::Colour(0xFF13161Au));
        g.fillRoundedRectangle(kpWell, 7.0f);
        g.setColour(juce::Colour(tokens::kPanelSubFrameBorder));
        g.drawRoundedRectangle(kpWell, 7.0f, 1.8f);

        // Interactive "v C3" reference key audition legend below keypad
        const auto c3Rect = c3AuditionClickRect_.toFloat();
        if (c3AuditionHeld_) {
            g.setColour(juce::Colour(tokens::kSilkAccentTeal).withAlpha(0.20f));
            g.fillRoundedRectangle(c3Rect, 5.0f);
        }
        g.setColour(juce::Colour(c3AuditionHeld_ ? tokens::kSilkAccentTeal : tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(17.5f, true));
        g.drawText("C3 AUDITION", c3Rect, juce::Justification::centred, false);
    }
}

void VF50MainCanvas::paintRightOfDisplaySilkscreenAndLeds(juce::Graphics& g) const {
    const auto classicLeds = classicController_.computeLedSnapshot();

    // 1. PLAY Group (PERFORMANCE, SINGLE, Reverse-Silkscreen PLAY Badge & White/Teal Mode Routing Lines)
    {
        drawClassicLedState(g, { 1854.0f, 175.0f }, 6.5f, classicLeds.performance, ledBlinkOn_);
        drawClassicLedState(g, { 1854.0f, 269.0f }, 6.5f, classicLeds.single, ledBlinkOn_);

        // White reverse-silkscreen "PLAY" badge (1862, 326, 94, 30)
        const auto playBadge = toRect(tokens::kPlayBadgeRect2x).toFloat();
        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.fillRoundedRectangle(playBadge, 3.5f);
        g.setColour(juce::Colour(0xFF181C20u));
        g.setFont(VF50LookAndFeel::makeFont(19.0f, true, 1.06f));
        g.drawText("PLAY", playBadge, juce::Justification::centred, false);

        // Authentic VF50 color-keyed routing brackets connecting PERFORMANCE (white) & SINGLE (teal) to EDIT section
        g.setColour(juce::Colour(tokens::kBracketLineWhite));
        g.drawLine(1962.0f, 175.0f, 2004.0f, 175.0f, 2.2f);
        g.drawLine(2004.0f, 118.0f, 2004.0f, 692.0f, 2.2f);
        g.drawLine(2004.0f, 118.0f, 2248.0f, 118.0f, 2.2f);

        g.setColour(juce::Colour(tokens::kBracketLineTeal));
        g.drawLine(1962.0f, 269.0f, 1994.0f, 269.0f, 2.2f);
        g.drawLine(1994.0f, 269.0f, 1994.0f, 700.0f, 2.2f);
        g.drawLine(1994.0f, 700.0f, 2110.0f, 700.0f, 2.2f);
    }

    // 2. MEMORY Group (INT, CARD, PRESET + 3 Green Bank LEDs)
    {
        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(16.0f, true));
        g.drawText("MEMORY", 1848, tokens::kBtnMemIntRect2x.y - 28, 110, 22, juce::Justification::centred, false);

        drawClassicLedState(g, { 1842.0f, static_cast<float>(tokens::kBtnMemIntRect2x.y)    + 41.0f }, 6.5f, classicLeds.memInt,    ledBlinkOn_);
        drawClassicLedState(g, { 1842.0f, static_cast<float>(tokens::kBtnMemCardRect2x.y)   + 41.0f }, 6.5f, classicLeds.memCard,   ledBlinkOn_);
        drawClassicLedState(g, { 1842.0f, static_cast<float>(tokens::kBtnMemPresetRect2x.y) + 41.0f }, 6.5f, classicLeds.memPreset, ledBlinkOn_);
    }

    // 3. EDIT 2x6 Matrix Header, Track LEDs & Dual-Color Silkscreen Legends (White = Perf, Teal = Single)
    {
        g.setColour(juce::Colour(tokens::kBracketLineWhite));
        g.drawLine(2352.0f, 118.0f, 2628.0f, 118.0f, 2.2f);
        g.drawLine(2628.0f, 118.0f, 2628.0f, 692.0f, 2.2f);

        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(18.0f, true, 1.06f));
        g.drawText("EDIT", 2252, 106, 96, 24, juce::Justification::centred, false);

        struct DualEditLabel {
            const char* perfUpperWhite;
            const char* singleLowerTeal;
            bool boxedTeal;
            MenuPage singlePage;
            MenuPage perfPage;
        };

        static constexpr std::array<DualEditLabel, 12> kEditLabels = {{
            // Left Column (0..5)
            { "ASSIGN MODE",   "QUICK EDIT",  true,  MenuPage::SingleQuickEdit,   MenuPage::PerfAssignMode  },
            { "NOTES",         "ALGORITHM",   false, MenuPage::SingleAlgorithm,   MenuPage::PerfNotes       },
            { "VOICE NUMBER",  "LFO",         false, MenuPage::SingleLfo,         MenuPage::PerfVoiceNumber },
            { "RECV CH",       "SENSITIVITY", false, MenuPage::SingleSensitivity, MenuPage::PerfRecvCh      },
            { "LIMIT / LOW",   "OSCILLATOR",  false, MenuPage::SingleOscillator,  MenuPage::PerfLimitLow    },
            { "LIMIT / HIGH",  "EG",          false, MenuPage::SingleEg,          MenuPage::PerfLimitHigh   },
            // Right Column (6..11)
            { "INST DETUNE",   "PEG",         false, MenuPage::SinglePeg,         MenuPage::PerfDetune      },
            { "NOTE SHIFT",    "OUT LEVEL",   false, MenuPage::SingleOutLevel,    MenuPage::PerfNoteShift   },
            { "VOLUME",        "SCALING",     false, MenuPage::SingleScaling,     MenuPage::PerfVolumePan   },
            { "OUTPUT ASSIGN", "TRANSPOSE",   false, MenuPage::SingleTranspose,   MenuPage::PerfOutAssign   },
            { "OTHERS",        "FUNCTION",    false, MenuPage::SingleFunction,    MenuPage::PerfOthers      },
            { "EFFECT",        "EFFECT",      false, MenuPage::EffectEdit,        MenuPage::EffectEdit      }
        }};

        for (int i = 0; i < 12; ++i) {
            const int col = i / 6;
            const int row = i % 6;
            const int btnX = ((col == 0) ? tokens::kEditColLeftX1x : tokens::kEditColRightX1x) * 2;
            const int btnY = (tokens::kEditStartY1x + row * tokens::kEditStepY1x) * 2;
            const auto& spec = kEditLabels[static_cast<std::size_t>(i)];

            // Active page LED / Track active LED to the left of TR1..TR8 and RHY
            const bool pageActive = (page_ == spec.singlePage) || (page_ == spec.perfPage)
                                 || (i == 4 && page_ == MenuPage::RhythmJob);
            if ((col == 0 && row <= 4) || (col == 1 && row <= 3) || pageActive) {
                vf50::classic::LedState ledSt = pageActive
                                                   ? vf50::classic::LedState::SolidGreen
                                                   : vf50::classic::LedState::Off;
                if (classicController_.usesOwnLcd() || processor_.isDemoPlaying()) {
                    if (col == 0 && row < 4) {
                        ledSt = classicLeds.tr[static_cast<std::size_t>(row)];
                    } else if (col == 1 && row < 4) {
                        ledSt = classicLeds.tr[static_cast<std::size_t>(row + 4)];
                    } else if (col == 0 && row == 4) {
                        ledSt = classicLeds.rhy;
                    }
                }
                drawClassicLedState(g,
                                    { static_cast<float>(btnX - 18), static_cast<float>(btnY + 41) },
                                    6.2f,
                                    ledSt,
                                    ledBlinkOn_);
            }

            const int lblX = btnX + 92;
            const int lblW = 190;

            // Upper White Legend (Performance Mode function)
            const bool hiPerf = (page_ == spec.perfPage);
            g.setColour(juce::Colour(hiPerf ? tokens::kSilkPlayGreen : tokens::kSilkWarmWhite));
            g.setFont(VF50LookAndFeel::makeFont(15.5f, true, 0.92f));
            g.drawText(spec.perfUpperWhite, lblX, btnY + 12, lblW, 24, juce::Justification::centredLeft, false);

            // Thin horizontal separator tick between Perf and Single legend
            g.setColour(juce::Colour(0x667D8692u));
            g.drawLine(static_cast<float>(lblX), static_cast<float>(btnY + 41),
                       static_cast<float>(lblX + 158), static_cast<float>(btnY + 41), 1.2f);

            // Lower Accent-Teal Legend (Single Mode function; QUICK EDIT has authentic reverse teal box!)
            const bool hiSingle = (page_ == spec.singlePage);
            if (spec.boxedTeal) {
                const juce::Rectangle<float> qeBox(static_cast<float>(lblX), static_cast<float>(btnY + 46), 148.0f, 26.0f);
                g.setColour(juce::Colour(tokens::kSilkAccentTeal));
                g.fillRoundedRectangle(qeBox, 3.5f);
                g.setColour(juce::Colour(0xFF10181Au));
                g.setFont(VF50LookAndFeel::makeFont(15.5f, true, 0.94f));
                g.drawText(spec.singleLowerTeal, qeBox, juce::Justification::centred, false);
            } else {
                g.setColour(juce::Colour(hiSingle ? tokens::kSilkWarmWhite : tokens::kSilkAccentTeal));
                g.setFont(VF50LookAndFeel::makeFont(15.5f, true, 0.92f));
                g.drawText(spec.singleLowerTeal, lblX, btnY + 45, lblW, 24, juce::Justification::centredLeft, false);
            }
        }
    }

    // 4. UTILITY Column Header & DEMO / COMPARE / EFFECT BYPASS / PROGRAMMER / KEYBOARD Hardware LEDs
    {
        g.setColour(juce::Colour(tokens::kSilkWarmWhite));
        g.setFont(VF50LookAndFeel::makeFont(17.0f, true, 1.04f));
        g.drawText("UTILITY", 2672, 104, 122, 24, juce::Justification::centred, false);

        drawClassicLedState(g, { 2822.0f, 175.0f }, 6.5f, classicLeds.demo, ledBlinkOn_);
        drawClassicLedState(g, { 2822.0f, 559.0f }, 6.5f, classicLeds.compare, ledBlinkOn_);
        drawClassicLedState(g, { 2822.0f, 653.0f }, 7.0f, classicLeds.effectBypass, ledBlinkOn_);

        // MIXER active teal/cyan hardware LED (#00B8A9)
        {
            const juce::Point<float> mxCenter{
                2512.0f,
                static_cast<float>(tokens::kBtnMixerRect2x.y) + static_cast<float>(tokens::kBtnMixerRect2x.h) * 0.5f
            };
            const float r = 6.5f;
            const float socketR = r + 2.5f;
            g.setColour(juce::Colour(0xFF080A0Cu));
            g.fillEllipse(mxCenter.x - socketR, mxCenter.y - socketR, socketR * 2.0f, socketR * 2.0f);
            g.setColour(juce::Colour(0xFF333942u));
            g.drawEllipse(mxCenter.x - socketR, mxCenter.y - socketR, socketR * 2.0f, socketR * 2.0f, 1.1f);
            if (mixerVisible_) {
                const float bloomR = r * 2.8f;
                juce::ColourGradient bloom(juce::Colour(0x6600B8A9u), mxCenter.x, mxCenter.y,
                                           juce::Colours::transparentBlack, mxCenter.x + bloomR, mxCenter.y, true);
                bloom.addColour(0.38, juce::Colour(0x4400E5D4u));
                g.setGradientFill(bloom);
                g.fillEllipse(mxCenter.x - bloomR, mxCenter.y - bloomR, bloomR * 2.0f, bloomR * 2.0f);

                juce::ColourGradient lensGrad(juce::Colour(0xFFD8FFFBu),
                                              mxCenter.x - r * 0.22f, mxCenter.y - r * 0.22f,
                                              juce::Colour(tokens::kMixerTealAccent).darker(0.35f),
                                              mxCenter.x + r, mxCenter.y + r, true);
                lensGrad.addColour(0.45, juce::Colour(tokens::kMixerTealAccent));
                g.setGradientFill(lensGrad);
                g.fillEllipse(mxCenter.x - r, mxCenter.y - r, r * 2.0f, r * 2.0f);
            } else {
                juce::ColourGradient unlitGrad(juce::Colour(0xFF132928u).brighter(0.18f),
                                               mxCenter.x - r * 0.3f, mxCenter.y - r * 0.3f,
                                               juce::Colour(0xFF132928u).darker(0.45f),
                                               mxCenter.x + r, mxCenter.y + r, true);
                g.setGradientFill(unlitGrad);
                g.fillEllipse(mxCenter.x - r, mxCenter.y - r, r * 2.0f, r * 2.0f);
                g.setColour(juce::Colours::white.withAlpha(0.16f));
                g.fillEllipse(mxCenter.x - r * 0.45f, mxCenter.y - r * 0.50f, r * 0.52f, r * 0.42f);
            }
        }

        // PROGRAMMER active green hardware LED (permanently visible host utility)
        VF50LookAndFeel::drawHardwareLed(
            g,
            { 2668.0f, static_cast<float>(tokens::kBtnProgrammerRect2x.y) + static_cast<float>(tokens::kBtnProgrammerRect2x.h) * 0.5f },
            6.5f, false, programmerVisible_);

        // KEYBOARD active green hardware LED
        VF50LookAndFeel::drawHardwareLed(
            g,
            { 2822.0f, static_cast<float>(tokens::kBtnKeyboardRect2x.y) + static_cast<float>(tokens::kBtnKeyboardRect2x.h) * 0.5f },
            6.5f, false, virtualKeyboardVisible_);
    }
}

} // namespace vf50::gui
