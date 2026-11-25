#pragma once

#include "Classic/VF50ClassicTypes.h"
#include "Disk/VF50DiskManager.h"
#include "Plugin/PluginProcessor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vf50::classic {

// Faithful 1989 VF50 Unified Classic Workstation Controller (Chapters 1-6).
// Strict Hardware Contract:
//   - Sole permanent operating engine for the unified workstation UI.
//   - Never opens OS popup menus or native FileChoosers inside faceplate workflows.
//   - Audio and sequencer playback remain decoupled from UI screen transitions.
class VF50ClassicController final {
public:
    VF50ClassicController(vf50::plugin::VF50AudioProcessor& processor,
                         vf50::disk::VF50DiskManager& diskManager) noexcept;

    void resetToBootOrPlay(bool showBootSplash = false) noexcept;
    void syncAudioHitTelemetry() noexcept;
    void tick30Hz() noexcept;

    // Direct state query & synchronization with VF50MainCanvas delegated edit pages
    [[nodiscard]] ClassicScreen screen() const noexcept { return screen_; }
    [[nodiscard]] bool usesOwnLcd() const noexcept { return screen_ != ClassicScreen::DelegatedEdit; }
    void enterScreen(ClassicScreen newScreen) noexcept;
    void syncFromWorkstationMode(vf50::state::WorkstationMode wsMode, bool isEditPage) noexcept;

    void setDelegatedVoiceEditInfo(bool isVoiceEdit, int selectedOperator0To3) noexcept {
        delegatedVoiceEdit_ = isVoiceEdit;
        selectedOperator_   = std::clamp(selectedOperator0To3, 0, 3);
    }

    // Play Mode / System / Utility / Action / Transport Hardware Button Handlers
    bool onPressPerformance() noexcept;
    bool onPressSingle() noexcept;
    bool onPressSeq() noexcept;
    bool onPressRhythm() noexcept;
    bool onPressCard() noexcept;
    bool onPressMidi() noexcept;
    bool onPressSetup() noexcept;
    bool onPressDemo() noexcept;
    bool onPressDemoAndStore() noexcept;
    bool onHoldPlayModeKey(bool isPerformance, bool heldDown) noexcept;
    [[nodiscard]] bool isDirectPcActive() const noexcept { return directPcMode_ != 0; }
    bool onPressJob() noexcept;
    bool onPressDisk() noexcept;
    bool onPressMemoryProtect() noexcept;
    bool onPressUtilityOthers() noexcept;
    bool onPressStore() noexcept;
    bool onPressCompare(bool toggledOn) noexcept;
    bool onPressCompare() noexcept;
    bool onPressInstMinus() noexcept; // [-] / INST ON/OFF / LETTER modifier
    void setInstMinusArmed(bool armed) noexcept { instMinusArmed_ = armed; }
    void cancelDirectPcQuietly() noexcept {
        directPcMode_ = 0;
        directPcDigits_.clear();
        midiPgmEditPerfHeld_ = false;
    }
    bool onPressMemBank(int bank0Int1Card2Preset) noexcept;
    bool onPressEditTrackKey(int track0To7) noexcept;
    bool onPressRhyTrackKey() noexcept;
    bool onPressEditMatrixKey(int index0To11) noexcept;
    void onChordKbdNoteOn(uint8_t midiNote) noexcept;
    void onMicroKbdNoteOn(uint8_t midiNote) noexcept;

    [[nodiscard]] static bool isCardScreen(ClassicScreen s) noexcept;
    [[nodiscard]] static bool isMidiScreen(ClassicScreen s) noexcept;
    [[nodiscard]] static bool isSetupScreen(ClassicScreen s) noexcept;
    [[nodiscard]] static bool isDemoOrMsgScreen(ClassicScreen s) noexcept;
    [[nodiscard]] bool demoLedState() const noexcept { return isDemoOrMsgScreen(screen_); }
    [[nodiscard]] static bool isPhase3UtilScreen(ClassicScreen s) noexcept;

    // Transport buttons (p. 7: RECORD / BWD / STOP / PLAY / FWD)
    bool onPressSeqOp(int btn0Rec1Bwd2Stop3Play4Fwd) noexcept;
    bool onPressStopAndBwdTogether() noexcept; // [STOP] + [BWD] = Top (measure 001, p. 87)

    // Softkeys F1..F8 (cols 0..39 in 5-col spans) & Data Entry controls
    bool onPressSoftKey(int f0To7) noexcept;
    bool onKeypadDigit(int digit0To9) noexcept;
    bool onKeypadYes() noexcept;
    bool onKeypadNo() noexcept;
    bool onStepValue(int deltaSteps) noexcept;
    [[nodiscard]] bool isTextEntryScreenActive() const noexcept;
    bool onTextKeyPressed(char asciiChar) noexcept;
    bool onTextBackspace() noexcept;
    bool onTextDelete() noexcept;
    bool onTextMoveCursor(int delta) noexcept;
    bool onTextConfirmEnter() noexcept;
    bool onTextCancelEscape() noexcept;
    void onDataEntryNormalized(float norm0To1) noexcept;

    // Live PG Programmer Slider <-> Classic VMEM & 40x2 LCD Synchronization
    bool onProgrammerVoiceParamEdited(vf50::params::ParamIndex paramIndex) noexcept;

    // Virtual Card hardware write-protect & Card RAM access
    void setCardHardwareWriteProtected(bool prot) noexcept { processor_.mutableVirtualCard().hardwareWriteProtect = prot; }
    [[nodiscard]] bool isCardHardwareWriteProtected() const noexcept { return processor_.virtualCard().hardwareWriteProtect; }
    [[nodiscard]] bool isMultiVoiceEditActive() const noexcept { return multiVoiceEditActive_; }
    [[nodiscard]] int selectedOperator0To3() const noexcept { return selectedOperator_; }
    [[nodiscard]] int selectedPerfEditInstrument() const noexcept { return perfEditInst_; }
    [[nodiscard]] int perfOthersSubPage() const noexcept { return perfOthersSubPage_; }
    [[nodiscard]] int perfEffectSubPage() const noexcept { return perfEffectSubPage_; }
    [[nodiscard]] int voiceQuickSubPage() const noexcept { return voiceQuickSubPage_; }
    [[nodiscard]] int voiceScalingSubPage() const noexcept { return voiceScalingSubPage_; }
    [[nodiscard]] int voiceEffectSubPage() const noexcept { return voiceEffectSubPage_; }
    [[nodiscard]] const vf50::generated::FactoryVoice& getCardVoice(std::size_t idx) const noexcept;
    [[nodiscard]] const vf50::generated::FactoryPerformance& getCardPerformance(std::size_t idx) const noexcept;

    // Rhythm Machine helpers & accessors
    [[nodiscard]] static bool isRhythmScreen(ClassicScreen s) noexcept;
    bool onRhythmKeyNoteOn(uint8_t midiNote, uint8_t velocity, bool eraseHeld = false, bool instSelectHeld = false) noexcept;
    void setHeldRhythmModifiers(bool eraseHeld, bool instSoftKeyHeld) noexcept {
        heldEraseMod_ = eraseHeld;
        heldInstSoftkey_ = instSoftKeyHeld;
    }
    void setRhythmTempoNormalized(float norm0To1) noexcept;
    [[nodiscard]] const vf50::generated::RhythmSongData& rhythmSong(std::size_t idx0To7) const noexcept;
    [[nodiscard]] int rhythmTempoBpm() const noexcept { return rhyTempoBpm_; }
    [[nodiscard]] bool isRhythmKbdModeRhy() const noexcept { return rhyKbdIsRhy_; }
    [[nodiscard]] bool isRhythmPatternQueued() const noexcept { return rhyQueuedPtnActive_; }
    [[nodiscard]] int rhythmKeyAssignMapIndex() const noexcept { return rhyAsgnMapIdx_; }
    [[nodiscard]] int stepSelectedDrumIndex() const noexcept { return stepSelectedDrum_; }
    [[nodiscard]] uint16_t stepRecordingAbsTick() const noexcept { return stepAbsTick_; }

    // Note hit & performance mute telemetry (Ch. 1.2 & Ch. 1.4)
    void notifySingleKeyActivity() noexcept;
    [[nodiscard]] uint8_t perfInstrumentMuteMask() const noexcept { return perfInstMuteMask_; }
    [[nodiscard]] bool isPerfInstrumentMuted(int part0To7) const noexcept {
        return (part0To7 >= 0 && part0To7 < 8) && (((perfInstMuteMask_ >> part0To7) & 1u) != 0u);
    }
    void setPerfInstrumentMuted(int part0To7, bool muted) noexcept;
    void togglePerfInstrumentMute(int part0To7) noexcept;
    [[nodiscard]] bool isSequencerMuteContext() const noexcept;
    [[nodiscard]] bool isInstMinusArmed() const noexcept { return instMinusArmed_; }
    [[nodiscard]] bool isBankHoldEnabled() const noexcept { return bankHoldEnabled_; }
    [[nodiscard]] bool isCompareModeLocked() const noexcept;
    [[nodiscard]] int selectedSeqSongSlot() const noexcept { return activeSongSlot_; }
    [[nodiscard]] int selectedRhythmSongSlot() const noexcept { return activeRhythmSong_; }
    [[nodiscard]] const std::string& lastNotice() const noexcept { return noticeText_; }
    [[nodiscard]] bool hasActiveNotice() const noexcept { return noticeTicks_ > 0; }
    [[nodiscard]] SeqEditSubKind activeSeqEditSubKind() const noexcept { return seqEditSubKind_; }
    [[nodiscard]] SeqRecMode activeSeqRecMode() const noexcept { return seqRecMode_; }
    [[nodiscard]] DiskOperation activeDiskOperation() const noexcept { return diskOp_; }
    [[nodiscard]] DiskDataType activeDiskDataType() const noexcept { return diskDataType_; }

    // Pure 40x2 LCD & 4-state Hardware LED Computation (Verbatim VF50_CLASSIC_SPEC_PHASE1.md)
    [[nodiscard]] ClassicLcdFrame renderLcdFrame() const noexcept;
    [[nodiscard]] ClassicLedSnapshot computeLedSnapshot() const noexcept;

private:
    struct ClassicSongSlotMeta {
        std::array<char, 8> name{ { ' ','N','e','w','S','o','n','g' } };
        bool    populatedFromDisk  = false;
        bool    hasSetupData       = false;
        uint8_t setupPlayMode      = 1u; // 0=Single, 1=Perf
        uint8_t setupPerfBank      = 1u; // 0=Int, 1=Preset
        uint8_t setupPerfNum       = 0u;
        uint8_t setupVoiceBank     = 1u;
        uint8_t setupVoiceNum      = 0u;
        std::array<uint8_t, 8> trackTxCh{ { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u } }; // 0=off, 1..16
    };

    void clearPendingBankDigits() noexcept;
    void setNotice(std::string_view text, int ticks30Hz = 60) noexcept;
    void clearNotice() noexcept;

    void selectSeqSongSlot(int slot0To7, bool applyStoredSetup) noexcept;
    void syncSongSlotTitlesFromProcessor() noexcept;
    void syncActiveRhythmSongFromSeqSlot(int seqSlot0To7) noexcept;
    void applyAudioPlaySongIndexForSlot(int slot0To7, bool startPlaying) noexcept;
    [[nodiscard]] std::size_t mappedDemoSongIndex(int slot0To7) const noexcept;

    [[nodiscard]] std::vector<std::size_t> filteredDiskEntryIndices(DiskDataType type) const;
    [[nodiscard]] std::vector<std::size_t> classicDirectoryEntryIndices() const;
    [[nodiscard]] bool matchesDiskType(const vf50::disk::DiskFileEntry& entry, DiskDataType type) const noexcept;
    [[nodiscard]] static std::string_view diskTypeTag(DiskDataType type) noexcept;
    [[nodiscard]] static std::string_view diskEntryShortType(const vf50::disk::DiskFileEntry& entry) noexcept;

    void adjustFilenameChar(int delta) noexcept;
    void adjustSongNameChar(int delta) noexcept;
    void syncSeqSlotNameToAudio() noexcept;
    void resetPerfInstrumentMutes() noexcept;
    [[nodiscard]] bool executeConfirmedDiskOperation() noexcept;
    [[nodiscard]] bool executeConfirmedSeqEdit() noexcept;

    // Performance & Voice Edit screen helpers (implemented in VF50ClassicEditScreens.cpp)
    [[nodiscard]] static bool isPerfEditScreen(ClassicScreen s) noexcept;
    [[nodiscard]] static bool isVoiceEditScreen(ClassicScreen s) noexcept;
    [[nodiscard]] static bool isUtilOthersScreen(ClassicScreen s) noexcept;
    [[nodiscard]] static bool isStoreOrCopyScreen(ClassicScreen s) noexcept;
    [[nodiscard]] bool isPerformanceContextScreen() const noexcept;
    [[nodiscard]] bool isPerfInstActive(const vf50::generated::FactoryPerformance& pf, int instIdx) const noexcept;
    void ensurePerfEditBackupInitialized() noexcept;
    void ensureVoiceEditBackupInitialized() noexcept;
    void commitCurrentVoiceEdit(bool syncEffects = true) noexcept;
    void commitCurrentPerfEdit(bool syncEffects = true) noexcept;
    void syncQuickEditBase() noexcept;
    void applyQuickEditOffset(int newRelOffset) noexcept;
    void enterMultiVoiceSlot(int instIdx) noexcept;
    void saveActiveMultiVoiceBackToSlot() noexcept;
    void resetPerfEffectMemoriesToFactory() noexcept;

    bool handleEditSoftKey(int k) noexcept;
    bool handleEditStepValue(int deltaSteps) noexcept;
    bool handleEditDigit(int digit0To9) noexcept;
    bool handleEditYes() noexcept;
    bool handleEditNo() noexcept;
    void handleEditDataEntry(float norm0To1) noexcept;
    bool renderEditLcdFrame(ClassicLcdFrame& frame) const noexcept;
    void applyEditLedSnapshot(ClassicLedSnapshot& leds) const noexcept;

    // Rhythm Machine screen helpers (implemented in VF50ClassicRhythmScreens.cpp)
    void initRhythmDefaults() noexcept;
    void syncRhythmTelemetry30Hz() noexcept;
    void commitRhythmKitAndSync() noexcept;
    void pushActiveRhythmKeyMapToAudio() noexcept;
    void updateRhythmMetronomeForMode() noexcept;
    void startRhythmPlayCurrent() noexcept;
    void stopRhythmPlayCurrent() noexcept;
    void startRhythmSongCurrent(uint16_t startPart, uint16_t startMeas) noexcept;
    [[nodiscard]] bool patternHasData(bool isUser, int ptnIdx) const noexcept;
    [[nodiscard]] uint8_t patternBars(bool isUser, int ptnIdx) const noexcept;
    [[nodiscard]] uint8_t patternTimeSigNum(bool isUser, int ptnIdx) const noexcept;
    [[nodiscard]] uint8_t patternTimeSigDen(bool isUser, int ptnIdx) const noexcept;
    [[nodiscard]] uint16_t computeTicksPerBar(uint8_t num, uint8_t den) const noexcept;
    [[nodiscard]] uint16_t activeRecQuantizeStepTicks() const noexcept;
    void loadWorkPatternFromSelection() noexcept;
    void commitWorkPatternToSlot(int userSlot0To99) noexcept;

    bool handleRhythmJobPress() noexcept;
    bool handleRhythmMemBank(int bIdx) noexcept;
    bool handleRhythmSeqOp(int btn) noexcept;
    bool handleRhythmStopAndBwd() noexcept;
    bool handleRhythmSoftKey(int k) noexcept;
    bool handleRhythmStepValue(int deltaSteps) noexcept;
    bool handleRhythmDigit(int digit0To9) noexcept;
    bool handleRhythmYes() noexcept;
    bool handleRhythmNo() noexcept;
    bool handleRhythmTextChar(char asciiChar) noexcept;
    void handleRhythmDataEntry(float norm0To1) noexcept;
    bool renderRhythmLcdFrame(ClassicLcdFrame& frame) const noexcept;
    void applyRhythmLedSnapshot(ClassicLedSnapshot& leds) const noexcept;

    // Utility, Card, MIDI, Setup & Demo helpers (implemented in VF50ClassicUtilScreens.cpp)
    bool handleUtilSoftKey(int k) noexcept;
    bool handleUtilStepValue(int deltaSteps) noexcept;
    bool handleUtilDigit(int digit0To9) noexcept;
    bool handleUtilYes() noexcept;
    bool handleUtilNo() noexcept;
    bool handleUtilTextChar(char asciiChar) noexcept;
    void handleUtilDataEntry(float norm0To1) noexcept;
    bool handleUtilMemBank(int bIdx) noexcept;
    bool handleUtilSeqOp(int btn) noexcept;
    bool handleUtilStopAndBwd() noexcept;
    bool renderUtilLcdFrame(ClassicLcdFrame& frame) const noexcept;
    void applyUtilLedSnapshot(ClassicLedSnapshot& leds) const noexcept;
    void tickPhase3Demo30Hz() noexcept;
    void clearAuditionOverridesIfNeeded(ClassicScreen nextScreen) noexcept;

    vf50::plugin::VF50AudioProcessor& processor_;
    vf50::disk::VF50DiskManager& diskManager_;

    ClassicScreen screen_            = ClassicScreen::PlaySingle;
    ClassicScreen preUtilityScreen_  = ClassicScreen::PlaySingle;
    ClassicScreen preCompareLock_    = ClassicScreen::PlaySingle;

    // Transient greeting / status message banner
    int         bootSplashTicks_ = 0;
    std::string noticeText_{};
    int         noticeTicks_     = 0;

    // Ch. 1 Play Single / Performance state
    bool    bankHoldEnabled_    = false;
    bool    cardBankActive_     = false;
    bool    instMinusArmed_     = false;
    int     pendingBankSelect_  = -1; // 0=INT, 1=CARD, 2=PRESET when awaiting 2 digits
    int     pendingTensDigit_   = -1;
    uint8_t perfInstMuteMask_   = 0u;
    std::array<uint8_t, 8> perfHitTicks_{};
    std::array<uint8_t, 8> perfMuteHitTicks_{};
    uint8_t singleHitTicks_     = 0u;
    bool    delegatedVoiceEdit_ = false;
    int     selectedOperator_   = 0;

    // Ch. 5 Sequencer state
    std::array<ClassicSongSlotMeta, 8> songSlots_{};
    int             activeSongSlot_       = 0; // 0..7 -> Song 1..8
    int             pendingSongSlot_      = 0;
    int             playSongCursorField_  = 0; // 0=Song, 1=Meas, 2=Tempo
    int             seqSetupCursorField_  = 0; // 0=Rch, 1=A.T, 2=Vel, 3=Click, 4=Sync
    int             seqRch_               = 0; // 0=Kbd, 1..16=Ch, 17=omn
    bool            seqAftertouch_        = false;
    bool            seqVelocity_          = true;
    int             seqClickMode_         = 1; // 0=off, 1=Rec, 2=Play/Rec, 3=Always
    bool            seqSyncMidi_          = false; // false=int, true=MIDI
    int             tchCursorTrack_       = 0; // 0..7
    int             songNameCursorCol_    = 0; // 0..7
    bool            songClearConfirm_     = false;
    SeqEditSubKind  seqEditSubKind_       = SeqEditSubKind::Mix;
    int             seqEditCursorField_   = 0;
    bool            seqEditConfirm_       = false;
    int             mixSrcA_              = 1;
    int             mixSrcB_              = 1;
    int             mixDst_               = 1;
    int             quantizeTrk_          = 1;
    int             quantizeSizeIdx_      = 2; // 0..6: 1/48, 1/32, 1/16, 1/12, 1/8, 1/6, 1/4
    int             delFromMeas_          = 1;
    int             delToMeas_            = 1;
    int             insFromMeas_          = 1;
    int             insToMeas_            = 1;
    int             insCount_             = 1;
    int             copySrcTrk_           = 1;
    int             copyFromMeas_         = 1;
    int             copyToMeas_           = 1;
    int             copyDstTrk_           = 1;
    int             copyDstMeas_          = 1;
    int             eraseTrk_             = 1; // 1..8, 9=all
    int             eraseFromMeas_        = 1;
    int             eraseToMeas_          = 1;
    int             remvEventType_        = 0; // 0=Cnt, 1=P.B., 2=A.T.
    int             remvRangeLo_          = 0;
    int             remvRangeHi_          = 0;
    int             remvTrk_              = 1; // 1..8, 9=all
    int             remvFromMeas_         = 1;
    int             remvToMeas_           = 1;

    SeqRecMode      seqRecMode_           = SeqRecMode::Realtime;
    SeqRecCondition seqRecCondition_      = SeqRecCondition::Replace;
    int             recModeCursor_        = 0; // 0=Mode, 1=Condition
    int             recStandbyCursor_     = 0; // 0=Mode, 1=Voice, 2=Time/In, 3=Meas/Out, 4=Tempo
    int             recVoiceAssign_       = 2; // 0=SGL, 1=PFM, 2=IND
    int             recMeas_              = 1;
    int             recPunchIn_           = 1;
    int             recPunchOut_          = 1;
    int             recSelectedTrack_     = 0; // 0..7
    bool            stepGateSubView_      = false;
    int             stepGatePreset_       = 0; // 0=Norm, 1=Stac, 2=Slur
    int             stepBarTick_          = 0;

    // Ch. 6 Disk LCD & Memory Protect state
    DiskOperation   diskOp_               = DiskOperation::Load;
    DiskDataType    diskDataType_         = DiskDataType::All;
    int             diskFileIndex_        = 0; // 0..N-1 in filtered list
    std::array<char, 8> diskEditName_{ { 'N','e','w','F','i','l','e',' ' } };
    int             diskNameCursorCol_    = 0; // 0..8
    bool            diskEditingName_      = false; // Save/Rename: false=File slot focus, true=Name focus
    bool            diskNameFreshDefault_ = false;
    bool            diskActionConfirm_    = false;
    bool            diskActionOverwrite_  = false;
    bool            diskFormatConfirm_    = false;
    int             diskBackupStep_       = 0; // 0=Sure?, 1=Original, 2=Duplicate, 3=Completed!
    int             diskMdrIntTime_       = 1; // 1..9 (*100 ms)
    int             memProtectCursor_     = 0; // 0=INT, 1=CARD

    // Virtual Card RAM & Perf Effects are unified on VF50AudioProcessor
    // (processor_.mutableVirtualCard() / processor_.mutablePerfEffectBank())

    // Performance Edit state
    int  perfEditInst_          = 0; // 0..7
    int  perfAssignCursor_      = 0; // 0=AssignMode, 1=Name
    int  perfNameCursorCol_     = 0; // 0..9
    int  perfOthersSubPage_     = 0; // 0..3
    int  perfMicroTableCursor_  = 0; // 0=Table, 1=Key
    int  perfEffectSubPage_     = 0; // 0..3
    int  perfEffectCursor_      = 0; // 0..2
    int  perfVoiceEditPendingTens_ = -1;

    // Voice Edit state
    int  voiceQuickSubPage_     = 0; // 0=ATTACK, 1=RELEAS, 2=VOLUME, 3=BRIL
    int  quickEditRelOffset_    = 0; // -50..+50
    std::array<vf50::generated::VoiceOperator, 4> quickEditBaseOps_{};
    int  voiceAlgCursor_        = 0; // 0=ALG, 1=Feedback
    int  voiceLfoCursor_        = 0; // 0..5
    int  voiceSensCursor_       = 0; // 0..4
    int  voiceOscCursor_        = 0; // 0..6
    int  voiceEgCursor_         = 0; // 0..5
    int  voicePegCursor_        = 0; // 0..5
    int  voiceOutCursor_        = 0; // 0..3
    int  voiceScalingSubPage_   = 0; // 0=RATE, 1=LEVEL
    int  voiceScalingCursor_    = 0; // 0..3
    int  voiceTransposeCursor_  = 0; // 0=Middle C, 1=KBD
    bool voiceTransposeKbdIn_   = true;
    int  voiceFuncCursor_       = 0; // 0..3
    int  voiceNameCursorCol_    = 0; // 0..10
    std::array<char, 10> voiceNameEditBackup_{ { 'I','N','I','T',' ','V','O','I','C','E' } };
    std::array<char, 10> perfNameEditBackup_{ { 'I','N','I','T',' ','P','E','R','F',' ' } };
    int  voiceEffectSubPage_    = 0; // 0..2
    int  voiceEffectCursor_     = 0; // 0..2

    // Multi-Voice Edit (M1111, p. 118)
    bool multiVoiceEditActive_       = false;
    int  multiVoiceEditInst_         = 0;
    std::array<vf50::state::StoredVoiceSlot, 8> multiVoiceBuffers_{};
    std::array<bool, 8> multiVoiceDirty_{};
    bool multiVoiceExitSglConfirm_   = false;

    // Utility OTHERS & Confirm State (pp. 117-118)
    bool utilConfirmPending_    = false;
    int  utilPendingAction_     = 0; // 1=VoiceInit, 2=VoiceRecall, 3=PerfInit, 4=PerfRecall, 5=PresetLoad
    int  utilPendingSubArg_     = 0; // PerfInit template 0..6, PresetLoad 0..2

    // Store & Copy State (pp. 119-120)
    ClassicScreen storeReturnScreen_   = ClassicScreen::PlaySingle;
    ClassicScreen preEditReturnScreen_ = ClassicScreen::PlaySingle;
    bool storeTargetCard_       = false;
    int  storeTargetSlot_       = 0;  // 0..99
    int  storePendingTens_      = -1;
    bool storeConfirmReady_     = false;
    bool storeIntProtBypassed_  = false;
    bool copyConfirmReady_      = false;
    bool copyFromPerfEffect_    = false;
    int  copyPerfFxSrcIdx_      = 0;  // 0..11
    int  copyPerfFxDstSlot_     = 0;  // 0..3 within family
    int  copyEgSrcOp_           = -1; // -1=unset, 0..3
    int  copyEgDstOp_           = -1; // -1=unset, 0..3
    int  copyEgCursor_          = 0;  // 0=Src, 1=Dst

    // Chapter 4 Rhythm Machine State (pp. 57-77, points to processor_.mutableHostRhythmSongs())
    std::array<vf50::generated::RhythmSongData, 8>* rhythmSongs_ = nullptr;
    bool     lastRhythmSubmodeIsSong_   = false;
    bool     rhyPtnIsUser_              = false;
    int      rhyPtnIdx_                 = 0;
    int      rhyPtnCursor_              = 0; // 0=Pattern, 1=Tempo, 2=KBD
    int      rhyPtnPendingBank_         = -1; // 0=User, 1=Preset
    int      rhyPtnPendingTens_         = -1;
    int      rhyTempoBpm_               = 120;
    bool     rhyKbdIsRhy_               = true;
    bool     rhyQueuedPtnActive_        = false;
    bool     rhyQueuedIsUser_           = false;
    int      rhyQueuedIdx_              = 0;
    uint32_t rhyQueuedStartWrap_        = 0u;
    uint32_t rhyBlinkCounter_           = 0u;
    std::array<uint8_t, 8> rhyVoiceHitTicks_{};

    // Song Play state
    int      activeRhythmSong_          = 0; // 0..7
    int      rhySongCursor_             = 0; // 0=SONG, 1=Meas, 2=Part, 3=Tempo, 4=KBD
    int      rhySongStartMeas_          = 1;
    int      rhySongStartPart_          = 0;
    bool     rhySongSyncedSeq_          = false;

    // Pattern Recording & Step Bar Graph state
    bool     recIsStep_                 = false;
    int      recSetupCursor_            = 0; // 0=Mode, 1=PTN, 2=Time, 3=Bar, 4=Tempo, 5=Qntz
    bool     recPtnIsUser_              = true;
    int      recPtnIdx_                 = 0;
    int      recTimeSigIdx_             = 3; // Index into VF50 time signature table (default 4/4)
    int      recBars_                   = 1;
    int      recQntzIdx_                = 4; // 0=1/4, 1=1/6, 2=1/8, 3=1/12, 4=1/16, 5=1/24, 6=1/32, 7=off
    int      recPendingQntzIdx_         = 4;
    bool     recQntzPendingBlink_       = false;
    uint32_t recQntzWrapAnchor_         = 0u;
    vf50::state::UserRhythmPattern recWorkPattern_{};
    bool     recSourceWasPreset_        = false;
    bool     recModified_               = false;
    int      recStoreSlot_              = 0;
    int      recStorePendingTens_       = -1;
    bool     recStoreConfirm_           = false;
    uint16_t stepAbsTick_               = 0u;
    int      stepSelectedDrum_          = 0;
    int      stepCursorField_           = 0; // 0=Qntz, 1=Inst, 2=Acc
    bool     stepQntzBlink_             = false;
    int      stepLastTripletIdx_        = 1; // 1=1/6, 3=1/12, 5=1/24
    bool     heldEraseMod_              = false;
    bool     heldInstSoftkey_           = false;

    // Shared Rhythm Setup, Inst Set, Key Assign (pp. 67-70)
    int      rhySetupCursor_            = 0; // 0=Rch, 1=Tch, 2=Vel, 3=Click, 4=Beat, 5=Sync
    int      rhyRch_                    = 9; // 0..15=Ch1..16, 16=omn
    int      rhyTch_                    = 10; // 0=off, 1..16=Ch1..16
    bool     rhyVelOn_                  = true;
    bool     rhyClickPlay_              = false; // false=rec, true=play
    int      rhyBeatIdx_                = 0; // 0=1/4, 1=1/6, 2=1/8, 3=1/12, 4=1/24, 5=1/32
    int      rhyInstCursor_             = 0; // 0=Inst, 1=Vol, 2=Pan, 3=Note, 4=R.Vol, 5=Efct
    int      rhyInstSelected_           = 0; // 0..60
    int      rhyAsgnCursor_             = 0; // 0=Map, 1=Key, 2=Inst
    int      rhyAsgnMapIdx_             = 0; // 0=PRESET1, 1=PRESET2, 2=PRESET3, 3=USER 1, 4=USER 2
    int      rhyAsgnKeyIdx_             = 0; // 0..60

    // Pattern Jobs (Copy / Clear)
    bool     rhySharedJobReturnToSong_  = false;
    int      rhyCopyCursor_             = 0; // 0=Src1, 1=Src2, 2=Dst
    bool     rhyCopySrc1IsUser_         = true;
    int      rhyCopySrc1Idx_            = 0;
    bool     rhyCopyHasSrc2_            = false;
    bool     rhyCopySrc2IsUser_         = true;
    int      rhyCopySrc2Idx_            = 0;
    int      rhyCopyDstIdx_             = 0;
    bool     rhyCopyConfirm_            = false;
    int      rhyClearSubMode_           = 0; // 0=menu, 1=Pattern, 2=Inst, 3=All
    int      rhyClearTargetIdx_         = 0;
    int      rhyClearInstIdx_           = 0;
    int      rhyClearCursor_            = 0;
    bool     rhyClearConfirm_           = false;

    // Song Edit & Song Jobs (pp. 72-77)
    int      songEditPartIdx_           = 0; // 0..998
    int      songEditTempoSub_          = 0; // 0=delta, 1=beats
    int      songEditPendingTens_       = -1;
    int      songEdJobSub_              = 0; // 0=menu, 1=Part, 2=Ins, 3=Del, 4=Copy, 5=Search, 6=Name
    int      songEdJumpTarget_          = 1;
    int      songEdCopyFrom_            = 1;
    int      songEdCopyTo_              = 1;
    int      songEdCopyDst_             = 1;
    int      songEdCopyCursor_          = 0;
    bool     songEdCopyConfirm_         = false;
    int      songEdNameCol_             = 0;
    int      songCopySrc_               = 0;
    int      songCopyDst_               = 1;
    int      songCopyCursor_            = 0;
    bool     songCopyConfirm_           = false;
    int      songClearSub_              = 0; // 0=menu, 1=Song, 2=All
    int      songClearTarget_           = 0;
    bool     rhySongClearConfirm_       = false;

    // Chapter 6 Card Operations (pp. 95-98)
    int      cardSubCategory_           = 0; // Save: 0=Synth,1=RhyA,2=RhyB; Load: 0=Synth,1=RhyA,2=RhyB,3=SeqYs
    int      cardPendingAction_         = 0; // 0=none, 1=Format, 2=Save, 3=Load
    int      cardPendingSubType_        = 0; // Format: 1=SYN,2=RHY; Save/Load: F-type / YsSeq
    bool     cardConfirmPending_        = false;

    // Chapter 6 MIDI Functions (pp. 99-104)
    int      midiChInfoCursor_          = 0; // 0=Midi, 1=BasicRch, 2=KbdTch, 3=LocaL
    int      midiSwitchCursor_          = 0; // 0=CC, 1=AT, 2=PB
    int      midiCondCursor_            = 0; // 0=NoteOnOff, 1=DataEntryAssign
    bool     midiPgmInitConfirm_        = false;
    int      midiPgmEditSlot_           = 0; // 0..127 (displayed 1..128)
    int      midiPgmEditPendingTens_    = -1;
    bool     midiPgmEditPerfHeld_       = false;
    int      midiBulkCategory_          = 0; // 0=VOICE, 1=PFM, 2=SETUP, 3=SEQ, 4=RHY
    int      midiBulkBank_              = 0; // 0=INT, 1=PRESET, 2=CARD
    int      midiBulkSubIdx_            = 0; // VOICE/PFM: 0..4 (00-24..ALL); SETUP:0..4; SEQ/RHY:0..2
    int      midiBulkCursor_            = 0; // 0=Bank, 1=Range (VOICE/PFM)
    bool     midiBulkConfirm_           = false;
    int      directPcMode_              = 0; // 0=inactive, 1=Single, 2=Perf
    std::string directPcDigits_{};

    // Chapter 6 Utility Setup Functions (pp. 110-116)
    int      setupTuneCursor_           = 0; // 0=MasterTuning, 1=SynthVol
    int      setupPerfFxCat_            = 0; // 0=delay, 1=pan, 2=chord
    int      setupPerfFxSlot_           = 0; // 0..3
    int      setupDelayPage_            = 0; // 0=DelayTime/PitchShift, 1=Feedback/EffectLevel
    int      setupDelayCursor_          = 0; // 0 or 1 within page
    int      setupPanCursor_            = 0; // 0=Select, 1=Direction, 2=PanRange
    int      setupChordKey_             = 0; // 0..11 (C3..B3)
    int      setupChordSlot_            = 0; // 0..3
    bool     setupChordKbdIn_           = false;
    int      setupChordKbdWriteIdx_     = 0;
    int      setupMicroCursor_          = 0; // 0=CRS, 1=FINE
    int      setupMicroKey_             = 0; // 0..11 (OCT) or 0..127 (FULL)
    int      setupMicroInitTable_       = 0; // 0..10 (1..11)
    int      setupMicroInitKey_         = 0; // 0..11 (C..B)
    int      setupMicroInitCursor_      = 0; // 0=Table, 1=key
    bool     setupMicroInitConfirm_     = false;
    int      setupVelCursor_            = 0; // 0=FixedVel, 1=VelCurve
    int      setupDampCursor_           = 0; // 0=EG Damp, 1=Voice Damp
    bool     combineBorrowedFunction_   = false;

    // Chapter 1 Demo & Power-On Greeting (pp. 11, 13-15, 22)
    int      demoSubMode_               = 0; // 0=ROM, 1=Disk, 2=Chain, 3=VEdit
    int      demoBusyTicks_             = 0;
    int      demoSelectedSong_          = 0;
    int      demoVEditStep_             = 0;
    float    demoVEditSpeed_            = 0.5f;
    int      demoVEditAccum_            = 0;
    int      msgEditCursorCol_          = 0; // 0..39
    int      msgMultiTapDigit_          = -1;
    int      msgMultiTapCycle_          = 0;
    bool     msgLowerCase_              = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50ClassicController)
};

} // namespace vf50::classic
