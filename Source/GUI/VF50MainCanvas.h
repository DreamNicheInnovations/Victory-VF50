#pragma once

#include "Classic/VF50ClassicController.h"
#include "Disk/VF50DiskManager.h"
#include "Disk/VF50MidiExporter.h"
#include "GUI/VF50DesignTokens.h"
#include "GUI/VF50LcdDisplay.h"
#include "GUI/VF50LookAndFeel.h"
#include "GUI/VF50MixerDrawer.h"
#include "GUI/VF50ProgrammerDrawer.h"
#include "GUI/VF50RomGuideOverlay.h"
#include "GUI/VF50VirtualKeyboard.h"
#include "Plugin/PluginProcessor.h"
#include "State/VF50StateMachine.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace vf50::gui {

class VF50DragTracksBadge final : public juce::Component {
public:
    VF50DragTracksBadge();

    std::function<void()> onStartExternalMidiDrag;

    void paint(juce::Graphics& g) override;
    void mouseEnter(const juce::MouseEvent&) override { hovered_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered_ = false; dragging_ = false; repaint(); }
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override { dragging_ = false; repaint(); }

private:
    bool hovered_       = false;
    bool dragging_      = false;
    bool dragInitiated_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50DragTracksBadge)
};

class VF50MainCanvas final : public juce::Component,
                            private juce::Timer {
public:
    enum class MenuPage : uint8_t {
        SinglePlay = 0,
        SingleQuickEdit,
        SingleAlgorithm,
        SingleLfo,
        SingleSensitivity,
        SingleOscillator,
        SingleEg,
        SinglePeg,
        SingleOutLevel,
        SingleScaling,
        SingleTranspose,
        SingleFunction,

        PerfPlay,
        PerfAssignMode,
        PerfNotes,
        PerfVoiceNumber,
        PerfRecvCh,
        PerfLimitLow,
        PerfLimitHigh,
        PerfDetune,
        PerfNoteShift,
        PerfVolumePan,
        PerfOutAssign,
        PerfOthers,

        EffectEdit,
        RhythmPlayEdit,
        RhythmJob,
        SequencerDemo,
        SequencerTrackSetup,
        JobMenu,
        UtilCard,
        UtilMidi,
        UtilDisk,
        UtilMemProtect,
        UtilSetup,
        UtilOthers,
        StoreConfirm
    };

    explicit VF50MainCanvas(vf50::plugin::VF50AudioProcessor& processor);
    ~VF50MainCanvas() override;

    // UI Scale, PG-Style Programmer Drawer & Virtual Keyboard synchronisation with parent PluginEditor
    void setCurrentScaleFactor(float scaleFactor) noexcept;
    [[nodiscard]] float currentScaleFactor() const noexcept { return currentScaleFactor_; }
    std::function<void(float)> onRequestScaleChange;

    void setProgrammerVisible(bool visible);
    [[nodiscard]] bool isProgrammerVisible() const noexcept { return programmerVisible_; }
    std::function<void(bool)> onRequestProgrammerVisibilityChange;

    void setMixerVisible(bool visible);
    [[nodiscard]] bool isMixerVisible() const noexcept { return mixerVisible_; }
    std::function<void(bool)> onRequestMixerVisibilityChange;

    void setVirtualKeyboardVisible(bool visible);
    [[nodiscard]] bool isVirtualKeyboardVisible() const noexcept { return virtualKeyboardVisible_; }
    std::function<void(bool)> onRequestKeyboardVisibilityChange;

    // Unified Classic Workstation Mode (permanent) + Preserved Host Utility Visibility Queries
    [[nodiscard]] vf50::state::WorkflowMode workflowMode() const noexcept { return processor_.workflowMode(); }
    [[nodiscard]] bool isFloppyHostSlotVisible() const noexcept;
    [[nodiscard]] bool isPreservedHostUtilitiesVisible() const noexcept;
    [[nodiscard]] bool isProgrammerDrawerEffectivelyVisible() const noexcept { return programmerVisible_; }
    [[nodiscard]] uint32_t faceplatePopupSpawnCount() const noexcept { return faceplatePopupSpawnCount_; }
    void resetFaceplatePopupSpawnCount() noexcept { faceplatePopupSpawnCount_ = 0u; }

    // Direct access to the emulated 40x2 HD44780 LCD, LookAndFeel, Disk Manager, PG Programmer & Virtual Keyboard
    [[nodiscard]] VF50LookAndFeel& lookAndFeel() noexcept { return lookAndFeel_; }
    [[nodiscard]] const VF50LookAndFeel& lookAndFeel() const noexcept { return lookAndFeel_; }
    [[nodiscard]] VF50LcdDisplay& lcdDisplay() noexcept { return lcd_; }
    [[nodiscard]] const VF50LcdDisplay& lcdDisplay() const noexcept { return lcd_; }
    [[nodiscard]] vf50::disk::VF50DiskManager& diskManager() noexcept { return diskManager_; }
    [[nodiscard]] const vf50::disk::VF50DiskManager& diskManager() const noexcept { return diskManager_; }
    [[nodiscard]] vf50::classic::VF50ClassicController& classicController() noexcept { return classicController_; }
    [[nodiscard]] const vf50::classic::VF50ClassicController& classicController() const noexcept { return classicController_; }
    [[nodiscard]] vf50::classic::ClassicLedSnapshot classicLedSnapshot() const noexcept {
        return classicController_.computeLedSnapshot();
    }
    [[nodiscard]] VF50ProgrammerDrawer& programmerDrawer() noexcept { return programmerDrawer_; }
    [[nodiscard]] const VF50ProgrammerDrawer& programmerDrawer() const noexcept { return programmerDrawer_; }
    [[nodiscard]] VF50MixerDrawer& mixerDrawer() noexcept { return mixerDrawer_; }
    [[nodiscard]] const VF50MixerDrawer& mixerDrawer() const noexcept { return mixerDrawer_; }
    [[nodiscard]] VF50VirtualKeyboard& virtualKeyboard() noexcept { return virtualKeyboard_; }
    [[nodiscard]] const VF50VirtualKeyboard& virtualKeyboard() const noexcept { return virtualKeyboard_; }
    [[nodiscard]] MenuPage activeMenuPage() const noexcept { return page_; }

    // Programmatic interaction helpers for automated verification & front-panel control
    void selectMenuPage(MenuPage newPage);
    void pressSoftKey(int index0To7);
    void pressDemoButton() { handleDemoToggle(); }
    void pressCardButton() { handleUtilityPress(0); }
    void pressMidiButton() { handleUtilityPress(1); }
    void pressDiskButton() { handleUtilityPress(2); }
    void pressSetupButton() { handleUtilityPress(4); }
    void pressUtilityOthersButton() { handleUtilityPress(5); }
    void pressJobButton() { handleJobPress(); }
    void pressPerformanceButton() { handlePlayModeSelect(true); }
    void pressSingleButton() { handlePlayModeSelect(false); }
    void holdPerformanceButton(bool heldDown, bool fromLatch = false);
    void holdSingleButton(bool heldDown, bool fromLatch = false);
    void holdDemoButton(bool heldDown) noexcept { demoBtnHeld_ = heldDown; }
    void pressDemoAndStoreTogether();
    void pressSeqButton();
    void pressInstMinusButton();
    void pressStopAndBwdTogether();
    bool mountUserDiskButton();
    bool sendPcKeyPress(const juce::KeyPress& key);
    [[nodiscard]] bool isTextEntryProxyActive() const noexcept { return textProxyFocusActive_; }
    void rightClickStopButton() { if (seqOpBtns_[2]) seqOpBtns_[2]->toggleHoldLatchFromInput(); }
    void shiftLeftClickBwdButton() { if (seqOpBtns_[1]) seqOpBtns_[1]->triggerShiftLeftClick(); }
    void rightClickInstMinusButton() { if (btnInstMinus_) btnInstMinus_->toggleHoldLatchFromInput(); }
    void rightClickDemoButton() { if (btnDemo_) btnDemo_->toggleHoldLatchFromInput(); }
    void rightClickSingleButton() { if (btnSingle_) btnSingle_->toggleHoldLatchFromInput(); }
    void rightClickPerformanceButton() { if (btnPerformance_) btnPerformance_->toggleHoldLatchFromInput(); }
    void rightClickRhythmButton() { if (btnRhythm_) btnRhythm_->toggleHoldLatchFromInput(); }
    void rightClickSoftKey(int idx0To7) {
        if (idx0To7 >= 0 && idx0To7 < 8 && softKeys_[static_cast<std::size_t>(idx0To7)]) {
            softKeys_[static_cast<std::size_t>(idx0To7)]->toggleHoldLatchFromInput();
        }
    }
    [[nodiscard]] bool isStopLatched() const noexcept { return seqOpBtns_[2] && seqOpBtns_[2]->isHoldLatched(); }
    [[nodiscard]] bool isInstMinusLatched() const noexcept { return btnInstMinus_ && btnInstMinus_->isHoldLatched(); }
    [[nodiscard]] bool isDemoLatched() const noexcept { return btnDemo_ && btnDemo_->isHoldLatched(); }
    [[nodiscard]] bool isSingleLatched() const noexcept { return btnSingle_ && btnSingle_->isHoldLatched(); }
    [[nodiscard]] bool isPerformanceLatched() const noexcept { return btnPerformance_ && btnPerformance_->isHoldLatched(); }
    [[nodiscard]] bool isRhythmLatched() const noexcept { return btnRhythm_ && btnRhythm_->isHoldLatched(); }
    [[nodiscard]] bool isSoftKeyLatched(int idx0To7) const noexcept {
        return idx0To7 >= 0 && idx0To7 < 8
            && softKeys_[static_cast<std::size_t>(idx0To7)]
            && softKeys_[static_cast<std::size_t>(idx0To7)]->isHoldLatched();
    }
    void clearAllHoldLatches(bool preserveDirectPc = false);
    void pressMemoryProtectButton() { handleUtilityPress(3); }
    void pressMemBankButton(int idx0To2) { handleMemoryBankSelect(idx0To2); }
    void pressKeypadDigitButton(int digit0To9) { handleKeypadDigit(digit0To9); }
    void pressRhythmButton();
    void pressRhyEditButton() { handleEditMatrixPress(4); }
    void pressLimitHighEditButton() { handleEditMatrixPress(5); }
    void pressOthersFuncEditButton() { handleEditMatrixPress(10); }
    void pressEffectEditButton() { handleEditMatrixPress(11); }
    void pressEditMatrixButton(int index0To11) { handleEditMatrixPress(index0To11); }
    void pressSeqOpButton(int index0To4) { handleSeqOpRowPress(index0To4); }
    void pressStoreButton() { handleStorePress(); }
    void pressCompareButton() { handleComparePress(); }
    void pressEffectBypassButton() { handleEffectBypassToggle(); }
    void pressKeypadYesButton();
    void pressKeypadNoButton();
    void pressProgrammerToggleButton() { handleProgrammerVisibilityToggle(); }
    void pressMixerToggleButton() { handleMixerVisibilityToggle(); }
    void pressKeyboardToggleButton() { handleKeyboardVisibilityToggle(); }
    [[nodiscard]] int utilSetupSubPage() const noexcept { return utilSetupSubPage_; }
    bool loadDirectoryEntry(std::size_t entryIndex, juce::String& outStatus);
    void handleFileDropped(const juce::File& file);
    void handleFilesDropped(const juce::StringArray& files);
    void setRomGuideVisible(bool visible);
    [[nodiscard]] bool isRomGuideVisible() const noexcept { return romGuideOverlay_.isVisible(); }
    bool exportMidiToFile(const juce::File& destFile, juce::String& outStatus);
    bool prepareDragToDawTempMidi(juce::File& outTempFile, juce::String& outStatus);
    void stepFocusedField(int deltaSteps);
    void setDataEntryNormalized(float norm0To1);
    void sendRhythmKeyNoteOn(uint8_t midiNote, uint8_t velocity, bool eraseHeld = false, bool instSelectHeld = false);
    void refreshDisplayAndLeds();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusLost(FocusChangeType cause) override;

private:
    struct EditableField {
        bool valid        = false;
        int cursorRow     = 1;
        int cursorCol     = 0;
        float minVal      = 0.0f;
        float maxVal      = 99.0f;
        float stepVal     = 1.0f;
        std::function<float()> readValue;
        std::function<void(float)> writeValue;
    };

    void timerCallback() override;

    void setupLeftSectionControls();
    void setupCenterSectionControls();
    void setupRightSectionControls();
    void setupHeaderScalePills();

    void handlePlayModeSelect(bool performanceMode);
    void handleMemoryBankSelect(int bankButtonIdx); // 0=INT, 1=CARD, 2=PRESET
    void handleEditMatrixPress(int index0To11);
    void handleUtilityPress(int index0To5);
    void handleJobPress();
    void handleKeypadDigit(int digit0To9);
    void handleSeqOpRowPress(int index0To4);
    void handleDemoToggle();
    void handleProgrammerVisibilityToggle();
    void handleMixerVisibilityToggle();
    void handleKeyboardVisibilityToggle();
    void handleProgrammerParamAdjusted(vf50::params::ParamIndex pIdx, float newVal);
    void handleStorePress();
    void handleComparePress();
    void handleEffectBypassToggle();

    void launchMountImageDialog();
    void launchMountFolderDialog();
    void launchChangeUserDiskFolderDialog();
    void launchExportMidiDialog();
    void startExternalMidiDragToDaw();

    bool handlePcTextEntryKeyPress(const juce::KeyPress& key);
    void syncTextEntryKeyboardFocus();

    void syncStateMachineFromMenuPage() noexcept;
    void syncMenuPageFromClassicController() noexcept;
    [[nodiscard]] bool isPerformanceContextForPage() const noexcept;
    void syncDrumTriggerTelemetry() noexcept;
    void rebuildPageModelAndLcd();
    void syncDataEntrySliderFromFocusedField();

    [[nodiscard]] bool isEraseModifierActive() const noexcept;
    [[nodiscard]] bool isStepInstModifierActive() const noexcept;
    void releaseStopLatchIfSet();

    // Procedural 2x Vector Surface & Silkscreen Drawing Helpers
    void paintChassisAndGroove(juce::Graphics& g) const;
    void paintHeaderBranding(juce::Graphics& g) const;
    void paintLeftColumnPanelsAndLegends(juce::Graphics& g) const;
    void paintBelowDisplaySilkscreenAndLeds(juce::Graphics& g) const;
    void paintRightOfDisplaySilkscreenAndLeds(juce::Graphics& g) const;

    vf50::plugin::VF50AudioProcessor& processor_;
    VF50LookAndFeel lookAndFeel_{};

    float currentScaleFactor_    = tokens::kDefaultScaleFactor; // 1.00f (100% = 1x desktop 1482x440)
    bool programmerVisible_      = false;
    bool mixerVisible_           = false;
    bool virtualKeyboardVisible_ = false;
    uint32_t lastSeenPrefsEpoch_ = 0u;
    uint32_t lastSeenPatchEpoch_ = 0u;
    uint32_t lastSeenRomEpoch_   = 0u;
    uint32_t compareBlinkTicks_  = 0u;
    bool compareBlinkVisible_    = true;
    uint32_t ledBlinkTicks_      = 0u;
    bool ledBlinkOn_             = true;

    // Emulated 40x2 HD44780 LCD
    VF50LcdDisplay lcd_;

    // Built-in FAT12 Floppy / Directory Disk Manager (mounted via top header bar, browsed via 40x2 LCD DISK)
    vf50::disk::VF50DiskManager diskManager_{};
    std::unique_ptr<juce::FileChooser> activeFileChooser_{};
    juce::String diskStatusMsg_{};
    uint32_t diskStatusTicks_       = 0u;
    uint32_t faceplatePopupSpawnCount_ = 0u;

    // Faithful 1989 Classic Mode Firmware Controller (Ch. 1, 5, 6)
    vf50::classic::VF50ClassicController classicController_;

    // Left Section Controls
    juce::Slider volumeFader_;
    juce::Slider dataEntryFader_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttach_;
    bool suppressDataSliderCallback_ = false;

    VF50WheelControl pitchWheel_{ true };
    VF50WheelControl modWheel_{ false };

    // Header Virtual Floppy Slot (DISK IN / FOLDER / USER DISK / SET DIR / EJECT), ROM Setup & UI Scale Pills
    std::unique_ptr<VF50PanelButton> btnFloppyInsert_;
    std::unique_ptr<VF50PanelButton> btnFloppyFolder_;
    std::unique_ptr<VF50PanelButton> btnFloppyUserDisk_;
    std::unique_ptr<VF50PanelButton> btnFloppySetUserDir_;
    std::unique_ptr<VF50PanelButton> btnFloppyEject_;
    std::array<std::unique_ptr<VF50PanelButton>, 5> scalePills_{};

    // DAW-safe zero-chrome PC keyboard capture proxy active ONLY during LCD text-entry screens
    class TextFocusProxy final : public juce::TextEditor {
    public:
        TextFocusProxy() = default;

        std::function<bool(const juce::KeyPress&)> onKey;
        std::function<void(const juce::String&)> onTextInsert;

        bool keyPressed(const juce::KeyPress& key) override {
            if (onKey && onKey(key)) {
                setText(juce::String(), juce::dontSendNotification);
                return true;
            }
            return juce::TextEditor::keyPressed(key);
        }

        void insertTextAtCaret(const juce::String& textToInsert) override {
            if (onTextInsert && textToInsert.isNotEmpty()) {
                onTextInsert(textToInsert);
            }
            setText(juce::String(), juce::dontSendNotification);
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TextFocusProxy)
    };
    TextFocusProxy textFocusProxy_{};
    bool textProxyFocusActive_ = false;

    // Center Below-Display Controls + Sequencer MIDI Export Strip
    std::array<std::unique_ptr<VF50PanelButton>, 8> softKeys_{};
    std::unique_ptr<VF50PanelButton> btnSeq_;
    std::unique_ptr<VF50PanelButton> btnRhythm_;
    std::unique_ptr<VF50PanelButton> btnJob_;
    std::unique_ptr<VF50PanelButton> btnInstMinus_;
    std::array<std::unique_ptr<VF50PanelButton>, 5> seqOpBtns_{};
    std::unique_ptr<VF50PanelButton> btnExportMid_;
    VF50DragTracksBadge dragToDawBadge_;
    std::unique_ptr<VF50PanelButton> btnRomLoader_;
    std::array<std::unique_ptr<VF50PanelButton>, 12> keypadBtns_{}; // 0..9, 10=[-1/NO], 11=[+1/YES]

    // Right-of-Display Controls
    std::unique_ptr<VF50PanelButton> btnPerformance_;
    std::unique_ptr<VF50PanelButton> btnSingle_;
    std::unique_ptr<VF50PanelButton> btnMemInt_;
    std::unique_ptr<VF50PanelButton> btnMemCard_;
    std::unique_ptr<VF50PanelButton> btnMemPreset_;
    std::array<std::unique_ptr<VF50PanelButton>, 12> editBtns_{};   // 0..5 Left Col, 6..11 Right Col
    std::array<std::unique_ptr<VF50PanelButton>, 6>  utilBtns_{};   // CARD, MIDI, DISK, MEM PROT, SETUP, OTHERS
    std::unique_ptr<VF50PanelButton> btnDemo_;
    std::unique_ptr<VF50PanelButton> btnStore_;
    std::unique_ptr<VF50PanelButton> btnCompare_;
    std::unique_ptr<VF50PanelButton> btnEffectBypass_;
    std::unique_ptr<VF50PanelButton> btnMixer_;
    std::unique_ptr<VF50PanelButton> btnProgrammer_;
    std::unique_ptr<VF50PanelButton> btnKeyboard_;

    // Pop-Down PG-Style 63-Slider FM Programmer Drawer, Visual Mixer Drawer, Bottom Switchable 61-Key Virtual Keyboard & ROM Guide Overlay
    VF50ProgrammerDrawer programmerDrawer_;
    VF50MixerDrawer mixerDrawer_;
    VF50VirtualKeyboard virtualKeyboard_;
    VF50RomGuideOverlay romGuideOverlay_;

    // Workstation Front-Panel Menu State
    MenuPage page_             = MenuPage::SinglePlay;
    int focusedFieldIdx_       = 0;
    int selectedOperator_      = 0; // 0..3 (OP1..OP4)
    int selectedPerfPart_      = 0; // 0..7 (Part 1..8)
    int selectedSeqTrack_      = 0; // 0..7 (Sequencer Track 1..8)
    int selectedDrumInst_      = 0; // 0..60
    int rhythmJobNumber_       = 13; // 1..14 (Defaults to Job 13: Key Assign)
    int storeTargetSlot_       = 0; // 0..99
    int utilSetupSubPage_      = 0; // 0..2 (Page 1: Tune/Transpose, Page 2: Mem Protect, Page 3: Controllers)
    bool cardBankVisualActive_ = false;
    bool instOnOffSoftMode_    = false;
    int pendingTensDigit_      = -1;
    uint32_t pendingDigitTicks_ = 0u;

    // Rhythm Keyboard / MIDI Drum Trigger Live LCD Readout State
    uint32_t lastSeenDrumTrigSeq_ = 0u;
    int lastStruckMidiNote_       = -1;
    int lastStruckDrumIdx_        = -1;
    int lastStruckDrumVel_        = 0;

    std::array<EditableField, 8> pageFields_{};
    int activeFieldCount_ = 0;

    // C3 Reference Note Audition State
    bool c3AuditionHeld_ = false;
    bool demoBtnHeld_ = false;
    bool suppressNextPlayModeClick_ = false;
    vf50::classic::ClassicScreen lastSyncedClassicScreen_ = vf50::classic::ClassicScreen::PlaySingle;

    // Clickable interactive hotspots on procedural chassis elements
    juce::Rectangle<int> cardSlotClickRect_{};
    juce::Rectangle<int> c3AuditionClickRect_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50MainCanvas)
};

} // namespace vf50::gui
