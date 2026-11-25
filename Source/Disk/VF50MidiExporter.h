#pragma once

#include "DSP/VF50DemoSongPlayer.h"
#include "Plugin/PluginProcessor.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace vf50::disk {

struct MidiExportSummary {
    bool   succeeded         = false;
    int    numTracks         = 0;   // Expected 10 (Track 0 Conductor + Tracks 1-8 FM + Track 9 Rhythm)
    short  ticksPerQuarter   = 480; // 480 PPQN Format 1 SMF
    int    totalNoteOnEvents = 0;
    std::array<int, 8> trackNoteCounts{};
    int    rhythmNoteCount   = 0;
    juce::String songTitle{};
    juce::String statusMessage{};
};

class VF50MidiExporter {
public:
    static constexpr short  kSmfPpqn             = 480; // 10x native VF50 48 PPQN
    static constexpr double kTickScaleFromVF50    = 10.0;
    static constexpr int    kMidiDrumChannel1B   = 10;  // Standard MIDI Channel 10 (1-based)
    static constexpr double kDrumGateTicks480    = 60.0; // 1/32-note drum gate at 480 PPQN

    // Build a complete Format 1 Standard MIDI File (480 PPQN, 10 tracks: Conductor + 8 Seq + Ch10 Rhythm)
    // from whichever song is currently active on processor (ROM Demo 0..2 or imported .SEQ/.IMG/.ALL slot 3).
    static bool buildMidiFileFromActiveSong(vf50::plugin::VF50AudioProcessor& processor,
                                            juce::MidiFile& outMidiFile,
                                            MidiExportSummary* outSummary = nullptr);

    // Build Format 1 SMF from an explicit song slot (0..2 = Factory ROM Demo, 3 = Imported CustomSeqSongBuffer)
    static bool buildMidiFileForSongSlot(vf50::plugin::VF50AudioProcessor& processor,
                                         std::size_t songSlot,
                                         juce::MidiFile& outMidiFile,
                                         MidiExportSummary* outSummary = nullptr);

    // Save Format 1 .mid file cleanly using FileOutputStream::truncate() + flush()
    static bool exportActiveSongToFile(vf50::plugin::VF50AudioProcessor& processor,
                                       const juce::File& targetFile,
                                       MidiExportSummary* outSummary = nullptr);

    // Write clean Format 1 .mid file into juce::File::tempDirectory for DAW external drag-and-drop
    static bool writeTempDragMidiFile(vf50::plugin::VF50AudioProcessor& processor,
                                      juce::File& outTempMidiFile,
                                      MidiExportSummary* outSummary = nullptr);

    // Map a VF50 drum instrument (0..60) to C1..C6 (MIDI 36..96) under the active YM3602 drum layout
    [[nodiscard]] static uint8_t mapDrumIndexToC1C6Note(const vf50::dsp::drums::Ym3602DrumEngine& drumEngine,
                                                        uint8_t drumIndex0To60) noexcept;

    [[nodiscard]] static uint8_t accentLevelToMidiVelocity(uint8_t accent1To9) noexcept;
};

} // namespace vf50::disk
