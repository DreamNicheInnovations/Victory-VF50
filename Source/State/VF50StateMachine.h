#pragma once

#include "State/VF50MemoryManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vf50::state {

// Unified Classic VF50 operating engine:
// Faithful 1989 VF50 Operating Manual 5-mode hierarchy, VMEM isolation,
// [COMPARE] flashing edit-buffer toggle, and multi-step [STORE] confirmation.
enum class WorkflowMode : uint8_t {
    Classic = 1
};
using OperatingMode = WorkflowMode;

// Authentic 5-mode 1989 VF50 Operating Manual state hierarchy
enum class ClassicTopMode : uint8_t {
    MODE_VOICE       = 0, // Play Mode, Quick Edit, Full Edit via F1-F8 softkeys & Edit matrix
    MODE_PERFORMANCE = 1, // Play Mode, Part Edit 1-8 (Voice Assign, Recv CH, Volume, Pan, Detune, Note Limits)
    MODE_RHYTHM      = 2, // Pattern Play, Pattern Edit, Job Menu 01-14 (incl. Job 13 Key Assign)
    MODE_SEQUENCER   = 3, // Song Play/Rec, Track Setup 1-8, DAW Sync
    MODE_UTILITY     = 4  // Setup (Pages 1-3), Controller Assign, MIDI/Bulk, Disk, Card
};

enum class SequencerSubPage : uint8_t {
    SongPlayRec = 0, // Song 1-4 selection, transport, tempo, DAW sync
    TrackSetup  = 1  // Track 1-8 MIDI ch, voice assign, mute/play status, volume
};

enum class StoreActionOutcome : uint8_t {
    BlockedByMemoryProtect = 0, // Classic mode when INT or CARD Memory Protect is ON
    EnteredConfirmPrompt   = 1, // Classic mode step 1: "Store to INT [00-99]? -> Press [+1/YES] to Confirm"
    CommittedToInternalRam = 2, // Modern immediate store OR Classic confirmed [+1/YES] store
    Cancelled              = 3  // Classic [-1/NO] cancel
};

struct SequencerTrackSetupState {
    bool    enabled        = true;
    uint8_t transmitCh0To15 = 0u;  // 0..15 (MIDI Ch 1..16)
    uint8_t voiceBank      = 1u;   // 1 = PRESET, 0 = INTERNAL
    uint8_t voiceNumber    = 0u;   // 0..99
    uint8_t volume         = 90u;  // 0..99
};

class VF50StateMachine {
public:
    static constexpr int kNumRhythmJobs      = 14;
    static constexpr int kNumSequencerTracks = 8;
    static constexpr int kNumSetupPages      = 3;

    VF50StateMachine() noexcept;

    void reset() noexcept;

    // Unified Classic operating mode (permanent)
    [[nodiscard]] WorkflowMode workflowMode() const noexcept { return WorkflowMode::Classic; }
    [[nodiscard]] bool isClassicMode() const noexcept { return true; }

    // Classic 5-Mode Hardware State Hierarchy
    void setClassicTopMode(ClassicTopMode mode) noexcept;
    [[nodiscard]] ClassicTopMode classicTopMode() const noexcept { return topMode_; }
    [[nodiscard]] static std::string_view classicTopModeLabel(ClassicTopMode mode) noexcept;

    // Rhythm Job Menu 01-14 (defaulting to Job 13: Key Assign on initial open for instant drum map access)
    void setRhythmJobNumber(int jobOneBased1To14) noexcept;
    [[nodiscard]] int rhythmJobNumber() const noexcept { return rhythmJobOneBased_; }
    [[nodiscard]] static std::string_view rhythmJobTitle(int jobOneBased1To14) noexcept;
    [[nodiscard]] static std::string_view rhythmJobActionHint(int jobOneBased1To14) noexcept;
    [[nodiscard]] static std::string_view keyAssignLayoutLabel(uint8_t layout0To3) noexcept;

    // Sequencer Sub-Pages (Song Play/Rec vs Track Setup 1-8)
    void setSequencerSubPage(SequencerSubPage sub) noexcept { seqSubPage_ = sub; }
    [[nodiscard]] SequencerSubPage sequencerSubPage() const noexcept { return seqSubPage_; }

    void setSelectedSequencerTrack(int trackZeroBased0To7) noexcept;
    [[nodiscard]] int selectedSequencerTrack() const noexcept { return selectedSeqTrack_; }
    [[nodiscard]] SequencerTrackSetupState& trackSetup(std::size_t track0To7) noexcept;
    [[nodiscard]] const SequencerTrackSetupState& trackSetup(std::size_t track0To7) const noexcept;

    // Utility Setup Pages (1: Master Tune/Transpose, 2: Memory Protect, 3: Controllers PB/MW/AT/Foot/Breath)
    void setUtilitySetupPage(int pageZeroBased0To2) noexcept;
    [[nodiscard]] int utilitySetupPage() const noexcept { return utilSetupPage_; }

    // Store Confirmation State Machine (Classic vs Modern)
    [[nodiscard]] bool isAwaitingStoreConfirm() const noexcept { return awaitingStoreConfirm_; }
    [[nodiscard]] int pendingStoreSlot() const noexcept { return pendingStoreSlot_; }
    void setPendingStoreSlot(int slot0To99) noexcept;

    StoreActionOutcome initiateOrConfirmStore(VF50MemoryManager& hostMemory,
                                              bool isPerformanceMode,
                                              int defaultTargetSlot0To99,
                                              bool intMemProtected,
                                              bool cardMemProtected,
                                              bool targetingCardBank) noexcept;

    StoreActionOutcome confirmPendingStore(VF50MemoryManager& hostMemory,
                                           bool isPerformanceMode) noexcept;

    StoreActionOutcome cancelPendingStore() noexcept;

private:
    ClassicTopMode topMode_              = ClassicTopMode::MODE_VOICE;
    SequencerSubPage seqSubPage_         = SequencerSubPage::SongPlayRec;
    int            rhythmJobOneBased_    = 13; // Job 13: Key Assign
    int            selectedSeqTrack_     = 0;  // 0..7 -> Track 1..8
    int            utilSetupPage_        = 0;  // 0..2 -> Setup Pages 1..3
    bool           awaitingStoreConfirm_ = false;
    int            pendingStoreSlot_     = 0;

    std::array<SequencerTrackSetupState, kNumSequencerTracks> seqTracks_{};
};

} // namespace vf50::state
