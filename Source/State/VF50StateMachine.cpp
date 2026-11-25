#include "State/VF50StateMachine.h"

#include <algorithm>

namespace vf50::state {

VF50StateMachine::VF50StateMachine() noexcept {
    reset();
}

void VF50StateMachine::reset() noexcept {
    topMode_              = ClassicTopMode::MODE_VOICE;
    seqSubPage_           = SequencerSubPage::SongPlayRec;
    rhythmJobOneBased_    = 13;
    selectedSeqTrack_     = 0;
    utilSetupPage_        = 0;
    awaitingStoreConfirm_ = false;
    pendingStoreSlot_     = 0;

    static constexpr std::array<uint8_t, kNumSequencerTracks> kDefaultVoices = {
        56u, 73u, 88u, 1u, 39u, 33u, 4u, 86u
    };
    for (std::size_t t = 0; t < kNumSequencerTracks; ++t) {
        seqTracks_[t].enabled         = true;
        seqTracks_[t].transmitCh0To15 = static_cast<uint8_t>(t);
        seqTracks_[t].voiceBank       = 1u; // PRESET
        seqTracks_[t].voiceNumber     = kDefaultVoices[t];
        seqTracks_[t].volume          = 90u;
    }
}

void VF50StateMachine::setClassicTopMode(ClassicTopMode mode) noexcept {
    topMode_ = mode;
    awaitingStoreConfirm_ = false;
}

std::string_view VF50StateMachine::classicTopModeLabel(ClassicTopMode mode) noexcept {
    switch (mode) {
        case ClassicTopMode::MODE_VOICE:       return "VOICE";
        case ClassicTopMode::MODE_PERFORMANCE: return "PERFORMANCE";
        case ClassicTopMode::MODE_RHYTHM:      return "RHYTHM";
        case ClassicTopMode::MODE_SEQUENCER:   return "SEQUENCER";
        case ClassicTopMode::MODE_UTILITY:     return "UTILITY";
    }
    return "VOICE";
}

void VF50StateMachine::setRhythmJobNumber(int jobOneBased1To14) noexcept {
    rhythmJobOneBased_ = std::clamp(jobOneBased1To14, 1, kNumRhythmJobs);
}

std::string_view VF50StateMachine::rhythmJobTitle(int jobOneBased1To14) noexcept {
    static constexpr std::array<std::string_view, kNumRhythmJobs> kJobTitles = {
        "01:Tempo",
        "02:Clear Pattern",
        "03:Copy Pattern",
        "04:Quantize",
        "05:Swing",
        "06:Beat / Time Sig",
        "07:Inst Level",
        "08:Inst Pan",
        "09:Inst Fine Tune",
        "10:Inst Accent",
        "11:Inst Out Assign",
        "12:Effect Mode",
        "13:Key Assign",
        "14:Midi Receive Ch"
    };
    const int idx = std::clamp(jobOneBased1To14, 1, kNumRhythmJobs) - 1;
    return kJobTitles[static_cast<std::size_t>(idx)];
}

std::string_view VF50StateMachine::rhythmJobActionHint(int jobOneBased1To14) noexcept {
    static constexpr std::array<std::string_view, kNumRhythmJobs> kJobHints = {
        "Pattern Tempo (30-250 BPM)   [+1/-1:JOB]",
        "Clear Active User Pattern    [+1/-1:JOB]",
        "Copy Pattern Src -> Dest     [+1/-1:JOB]",
        "Quantize Resolution (1/4-48) [+1/-1:JOB]",
        "Swing Timing (50%-75%)       [+1/-1:JOB]",
        "Pattern Time Sig (1/4-8/4)   [+1/-1:JOB]",
        "Drum Inst Level (0-31)       [+1/-1:JOB]",
        "Drum Inst Pan (L63..C..R63)  [+1/-1:JOB]",
        "Drum Inst Fine Tune          [+1/-1:JOB]",
        "Drum Accent Velocity Curve   [+1/-1:JOB]",
        "Drum Stereo Output Assign    [+1/-1:JOB]",
        "Rhythm DSP Effect Send       [+1/-1:JOB]",
        "C1-C6 Drum Key Map Layout    [+1/-1:MAP]",
        "Rhythm MIDI Receive Ch       [+1/-1:JOB]"
    };
    const int idx = std::clamp(jobOneBased1To14, 1, kNumRhythmJobs) - 1;
    return kJobHints[static_cast<std::size_t>(idx)];
}

std::string_view VF50StateMachine::keyAssignLayoutLabel(uint8_t layout0To3) noexcept {
    static constexpr std::array<std::string_view, 4> kLabels = {
        "PRESET 1 (RX Layout)",
        "PRESET 2 (C3 Layout)",
        "PRESET 3 (Clavinova)",
        "USER / SEQ (1-61)"
    };
    return kLabels[std::min<std::size_t>(layout0To3, 3u)];
}

void VF50StateMachine::setSelectedSequencerTrack(int trackZeroBased0To7) noexcept {
    selectedSeqTrack_ = std::clamp(trackZeroBased0To7, 0, kNumSequencerTracks - 1);
}

SequencerTrackSetupState& VF50StateMachine::trackSetup(std::size_t track0To7) noexcept {
    return seqTracks_[std::min<std::size_t>(track0To7, kNumSequencerTracks - 1u)];
}

const SequencerTrackSetupState& VF50StateMachine::trackSetup(std::size_t track0To7) const noexcept {
    return seqTracks_[std::min<std::size_t>(track0To7, kNumSequencerTracks - 1u)];
}

void VF50StateMachine::setUtilitySetupPage(int pageZeroBased0To2) noexcept {
    utilSetupPage_ = std::clamp(pageZeroBased0To2, 0, kNumSetupPages - 1);
}

void VF50StateMachine::setPendingStoreSlot(int slot0To99) noexcept {
    pendingStoreSlot_ = std::clamp(slot0To99, 0, 99);
}

StoreActionOutcome VF50StateMachine::initiateOrConfirmStore(VF50MemoryManager& hostMemory,
                                                           bool isPerformanceMode,
                                                           int defaultTargetSlot0To99,
                                                           bool intMemProtected,
                                                           bool cardMemProtected,
                                                           bool targetingCardBank) noexcept {
    // Enforce 1989 VF50 Memory Protect lock & Store Confirmation prompt
    const bool protectLocked = targetingCardBank ? cardMemProtected : intMemProtected;
    if (protectLocked) {
        awaitingStoreConfirm_ = false;
        return StoreActionOutcome::BlockedByMemoryProtect;
    }

    if (!awaitingStoreConfirm_) {
        pendingStoreSlot_     = std::clamp(defaultTargetSlot0To99, 0, 99);
        awaitingStoreConfirm_ = true;
        return StoreActionOutcome::EnteredConfirmPrompt;
    }

    return confirmPendingStore(hostMemory, isPerformanceMode);
}

StoreActionOutcome VF50StateMachine::confirmPendingStore(VF50MemoryManager& hostMemory,
                                                        bool isPerformanceMode) noexcept {
    awaitingStoreConfirm_ = false;
    const auto slot = static_cast<std::size_t>(std::clamp(pendingStoreSlot_, 0, 99));
    if (isPerformanceMode) {
        hostMemory.storeEditBufferToUserPerformance(slot);
    } else {
        hostMemory.storeEditBufferToUserVoice(slot);
    }
    return StoreActionOutcome::CommittedToInternalRam;
}

StoreActionOutcome VF50StateMachine::cancelPendingStore() noexcept {
    awaitingStoreConfirm_ = false;
    return StoreActionOutcome::Cancelled;
}

} // namespace vf50::state
