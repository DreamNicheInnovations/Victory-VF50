#include "Disk/VF50MidiExporter.h"
#include "ROM/VF50RomBank.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace vf50::disk {

namespace {

[[nodiscard]] int bpmToMicrosecondsPerQuarter(double bpm) noexcept {
    const double clamped = std::clamp(bpm, 20.0, 300.0);
    return static_cast<int>(std::llround(60000000.0 / clamped));
}

[[nodiscard]] juce::String sanitizeFileStem(const juce::String& title) {
    juce::String clean;
    for (int i = 0; i < title.length(); ++i) {
        const juce::juce_wchar ch = title[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            clean += ch;
        } else if (ch == ' ' || ch == '_' || ch == '-') {
            clean += '_';
        }
    }
    clean = clean.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    return clean.isEmpty() ? juce::String("VF50_Sequence") : clean;
}

[[nodiscard]] juce::String lookupVoiceNameTrimmed(const vf50::state::VF50MemoryManager& mem,
                                                  bool isFactoryPreset,
                                                  uint8_t voiceIndex0To99) {
    const auto bank = isFactoryPreset ? vf50::state::MemoryBank::FactoryRom
                                      : vf50::state::MemoryBank::UserRam;
    const auto& voice = mem.getVoice(bank, std::min<std::size_t>(voiceIndex0To99, 99u));
    return juce::String(voice.name.data(), voice.name.size()).trim();
}

bool writeMidiFileTruncatedAndFlushed(const juce::MidiFile& midiFile,
                                      const juce::File& targetFile,
                                      juce::String& outError) {
    if (const auto parent = targetFile.getParentDirectory(); !parent.isDirectory()) {
        (void)parent.createDirectory();
    }

    juce::FileOutputStream stream(targetFile);
    if (!stream.openedOk()) {
        outError = "Cannot open MIDI output: " + targetFile.getFileName();
        return false;
    }

    stream.setPosition(0);
    if (!stream.truncate().wasOk()) {
        outError = "Cannot truncate MIDI file: " + targetFile.getFileName();
        return false;
    }

    // Explicitly write SMF Format 1 (multi-track synchronous)
    if (!midiFile.writeTo(stream, 1)) {
        outError = "Failed writing Format 1 MIDI data";
        return false;
    }

    stream.flush();
    return true;
}

} // namespace

uint8_t VF50MidiExporter::mapDrumIndexToC1C6Note(const vf50::dsp::drums::Ym3602DrumEngine& drumEngine,
                                                uint8_t drumIndex0To60) noexcept {
    const std::size_t idx = std::min<std::size_t>(drumIndex0To60, vf50::dsp::drums::Ym3602DrumEngine::kNumDrums - 1u);
    const uint8_t assigned = drumEngine.instrumentState(idx).midiNote;
    // Verify note falls in 61-key C1..C6 range (MIDI 36..96) and actually triggers this drum or is valid
    if (assigned >= 36u && assigned <= 96u && drumEngine.drumForMidiNote(assigned) == static_cast<int>(idx)) {
        return assigned;
    }
    // Deterministic C1..C6 fallback (36..96 covers all 61 VF50 PCM drums 1-to-1)
    return static_cast<uint8_t>(std::clamp<int>(36 + static_cast<int>(idx), 36, 96));
}

uint8_t VF50MidiExporter::accentLevelToMidiVelocity(uint8_t accent1To9) noexcept {
    static constexpr std::array<uint8_t, 10> kAccentToVel = {
        0u, 28u, 44u, 60u, 76u, 92u, 104u, 114u, 121u, 127u
    };
    return kAccentToVel[std::clamp<std::size_t>(accent1To9, 1u, 9u)];
}

bool VF50MidiExporter::buildMidiFileFromActiveSong(vf50::plugin::VF50AudioProcessor& processor,
                                                  juce::MidiFile& outMidiFile,
                                                  MidiExportSummary* outSummary) {
    int slot = processor.activeDemoSongIndex();
    if (slot < 0) {
        slot = 0;
    }
    return buildMidiFileForSongSlot(processor, static_cast<std::size_t>(slot), outMidiFile, outSummary);
}

bool VF50MidiExporter::buildMidiFileForSongSlot(vf50::plugin::VF50AudioProcessor& processor,
                                               std::size_t songSlot,
                                               juce::MidiFile& outMidiFile,
                                               MidiExportSummary* outSummary) {
    using vf50::generated::DemoEventType;
    using vf50::generated::DemoSongEvent;

    processor.pullLatestAudioMemoryToHost();
    const auto& hostMem    = processor.hostMemory();
    const auto& rom        = hostMem.runtimeRomBank();
    const auto& drumEngine = processor.synthEngine().drums();
    const auto& pBank      = processor.parameterBank();

    outMidiFile.clear();
    outMidiFile.setTicksPerQuarterNote(kSmfPpqn);

    // 1. Resolve Song Metadata & Event Span (48 PPQN native VF50 timeline)
    juce::String songTitle;
    uint16_t initialTempoBpm = 120u;
    uint8_t timeSigNum      = 4u;
    uint8_t timeSigDen      = 4u;
    uint32_t totalTicks48   = 192u;
    uint8_t perfPresetBank  = 1u;
    uint8_t perfNumber      = 0u;
    std::array<uint8_t, 8> trackChannels = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
    std::span<const DemoSongEvent> events{};

    if (vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(songSlot)) {
        const auto* customSeq = processor.userSequenceInSlot(songSlot);
        if (customSeq != nullptr && customSeq->valid && customSeq->eventCount > 0u) {
            const auto sv   = customSeq->title();
            songTitle       = juce::String(sv.data(), sv.size()).trim();
            initialTempoBpm = std::clamp<uint16_t>(customSeq->initialTempoBpm, 30u, 300u);
            timeSigNum      = std::max<uint8_t>(1u, customSeq->timeSigNum);
            timeSigDen      = std::max<uint8_t>(1u, customSeq->timeSigDen);
            totalTicks48    = std::max<uint32_t>(192u, customSeq->totalTicks);
            perfPresetBank  = customSeq->perfPresetBank;
            perfNumber      = customSeq->perfNumber;
            trackChannels   = customSeq->trackChannels;
            events          = std::span<const DemoSongEvent>(customSeq->events.data(), customSeq->eventCount);
        } else {
            if (outSummary != nullptr) {
                outSummary->succeeded = false;
                outSummary->statusMessage = "No Song Data in Slot " + juce::String(static_cast<int>(songSlot + 1u));
            }
            return false;
        }
    } else if (vf50::dsp::VF50DemoSongPlayer::isFactoryDemoSongIndex(songSlot)) {
        const std::size_t romIdx = songSlot - vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase;
        const auto& romSong      = rom.factoryDemoSongs[romIdx];
        songTitle                = juce::String(romSong.title.data(), romSong.title.size()).trim();
        initialTempoBpm          = std::clamp<uint8_t>(romSong.initialTempoBpm, 30u, 240u);
        timeSigNum               = std::max<uint8_t>(1u, romSong.timeSigNum);
        timeSigDen               = std::max<uint8_t>(1u, romSong.timeSigDen);
        totalTicks48             = std::max<uint32_t>(192u, romSong.totalTicks);
        perfPresetBank           = romSong.perfPresetBank;
        perfNumber               = romSong.perfNumber;
        trackChannels            = romSong.trackChannels;
        events                   = rom.getDemoSongEvents(romIdx);
    } else {
        if (outSummary != nullptr) {
            outSummary->succeeded = false;
            outSummary->statusMessage = "Invalid Song Slot";
        }
        return false;
    }

    if (songTitle.isEmpty()) {
        songTitle = "Victory VF50 Song";
    }

    const double endTick480 = static_cast<double>(totalTicks48) * kTickScaleFromVF50 + 96.0;

    // 2. Determine Initial Program / Bank for Each of the 8 Sequencer Tracks
    const auto& initPerf = hostMem.getPerformance(
        (perfPresetBank != 0u) ? vf50::state::MemoryBank::FactoryRom : vf50::state::MemoryBank::UserRam,
        std::min<std::size_t>(perfNumber, 99u));

    std::array<bool, 8>    trackIsPreset{};
    std::array<uint8_t, 8> trackVoiceNum{};
    std::array<bool, 8>    trackProgramFound{};

    for (std::size_t t = 0; t < 8u; ++t) {
        const auto& inst  = initPerf.instruments[t];
        trackIsPreset[t]  = (inst.voiceBank != 0u);
        trackVoiceNum[t]  = std::min<uint8_t>(inst.voiceNumber, 99u);
    }

    auto resolveTrackForChannel = [&trackChannels](uint8_t ch0To15) noexcept -> std::size_t {
        const uint8_t c = static_cast<uint8_t>(ch0To15 & 0x0Fu);
        for (std::size_t t = 0; t < 8u; ++t) {
            if ((trackChannels[t] & 0x0Fu) == c) {
                return t;
            }
        }
        return static_cast<std::size_t>(c & 0x07u);
    };

    for (const auto& ev : events) {
        if (ev.type == DemoEventType::FmProgram) {
            const std::size_t t = resolveTrackForChannel(ev.channel);
            if (!trackProgramFound[t]) {
                trackIsPreset[t]     = (ev.data1 != 0u);
                trackVoiceNum[t]     = std::min<uint8_t>(ev.data2, 99u);
                trackProgramFound[t] = true;
            }
        }
    }

    // 3. Track 0: Conductor Track (Song Title, Time Signature, Full Tempo Map)
    juce::MidiMessageSequence conductorSeq;
    conductorSeq.addEvent(juce::MidiMessage::textMetaEvent(0x03, songTitle), 0.0);
    conductorSeq.addEvent(juce::MidiMessage::timeSignatureMetaEvent(
                              static_cast<int>(timeSigNum),
                              static_cast<int>(timeSigDen)),
                          0.0);
    conductorSeq.addEvent(juce::MidiMessage::tempoMetaEvent(
                              bpmToMicrosecondsPerQuarter(static_cast<double>(initialTempoBpm))),
                          0.0);

    double runningBpm = static_cast<double>(initialTempoBpm);
    for (const auto& ev : events) {
        if (ev.type != DemoEventType::TempoChange) {
            continue;
        }
        const double baseTick480 = static_cast<double>(ev.tick) * kTickScaleFromVF50;
        if (ev.data1 == 0u) {
            runningBpm = std::clamp(static_cast<double>(ev.data2), 30.0, 240.0);
            conductorSeq.addEvent(
                juce::MidiMessage::tempoMetaEvent(bpmToMicrosecondsPerQuarter(runningBpm)),
                baseTick480);
        } else {
            const double deltaBpm = static_cast<double>(static_cast<int8_t>(ev.data1));
            if (ev.data2 == 0u) {
                runningBpm = std::clamp(runningBpm + deltaBpm, 30.0, 240.0);
                conductorSeq.addEvent(
                    juce::MidiMessage::tempoMetaEvent(bpmToMicrosecondsPerQuarter(runningBpm)),
                    baseTick480);
            } else {
                const int steps = std::clamp<int>(static_cast<int>(ev.data2) * 2, 1, 32);
                const double totalRamp480 = static_cast<double>(ev.data2) * static_cast<double>(kSmfPpqn);
                const double startBpm     = runningBpm;
                for (int s = 1; s <= steps; ++s) {
                    const double frac = static_cast<double>(s) / static_cast<double>(steps);
                    const double stepBpm = std::clamp(startBpm + deltaBpm * frac, 30.0, 240.0);
                    conductorSeq.addEvent(
                        juce::MidiMessage::tempoMetaEvent(bpmToMicrosecondsPerQuarter(stepBpm)),
                        baseTick480 + totalRamp480 * frac);
                }
                runningBpm = std::clamp(startBpm + deltaBpm, 30.0, 240.0);
            }
        }
    }
    conductorSeq.sort();
    conductorSeq.addEvent(juce::MidiMessage::endOfTrack(), endTick480);
    outMidiFile.addTrack(conductorSeq);

    // 4. Tracks 1..8: Sequencer Tracks 1-8 on MIDI Channels 1-8 + Track 9: Rhythm on MIDI Channel 10
    std::array<juce::MidiMessageSequence, 8> fmTracks{};
    std::array<std::array<int, 128>, 8> activeNoteCounts{};
    std::array<int, 8> trackNoteTotals{};

    for (std::size_t t = 0; t < 8u; ++t) {
        const int midiCh1Based = static_cast<int>(t + 1u);
        juce::String voiceName = lookupVoiceNameTrimmed(hostMem, trackIsPreset[t], trackVoiceNum[t]);
        if (voiceName.isEmpty()) {
            voiceName = "FM Voice " + juce::String(static_cast<int>(trackVoiceNum[t]));
        }
        const juce::String trackTitle = "VF50 Track " + juce::String(midiCh1Based) + " - " + voiceName;

        auto& seq = fmTracks[t];
        seq.addEvent(juce::MidiMessage::textMetaEvent(0x03, trackTitle), 0.0);
        // Initial Bank Select (CC0 MSB = 0, CC32 LSB = 1 Preset / 0 Internal) + Program Change
        seq.addEvent(juce::MidiMessage::controllerEvent(midiCh1Based, 0, 0), 0.0);
        seq.addEvent(juce::MidiMessage::controllerEvent(midiCh1Based, 32, trackIsPreset[t] ? 1 : 0), 0.0);
        seq.addEvent(juce::MidiMessage::programChange(midiCh1Based, static_cast<int>(trackVoiceNum[t])), 0.0);
    }

    juce::MidiMessageSequence rhythmSeq;
    rhythmSeq.addEvent(juce::MidiMessage::textMetaEvent(0x03, "VF50 Track 9 - Rhythm"), 0.0);

    struct RawDrumNote {
        double  tick480 = 0.0;
        uint8_t note    = 36u;
        uint8_t vel     = 100u;
    };
    std::vector<RawDrumNote> rawDrumNotes;
    rawDrumNotes.reserve(512u);

    for (const auto& ev : events) {
        const double tick480 = static_cast<double>(ev.tick) * kTickScaleFromVF50;

        switch (ev.type) {
            case DemoEventType::FmNoteOn: {
                const std::size_t t    = resolveTrackForChannel(ev.channel);
                const int midiCh1Based = static_cast<int>(t + 1u);
                const uint8_t note     = static_cast<uint8_t>(ev.data1 & 0x7Fu);
                const uint8_t vel      = static_cast<uint8_t>(ev.data2 & 0x7Fu);
                if (vel > 0u) {
                    while (activeNoteCounts[t][note] > 0) {
                        fmTracks[t].addEvent(juce::MidiMessage::noteOff(midiCh1Based, note), tick480);
                        --activeNoteCounts[t][note];
                    }
                    fmTracks[t].addEvent(juce::MidiMessage::noteOn(midiCh1Based, note, vel), tick480);
                    ++activeNoteCounts[t][note];
                    ++trackNoteTotals[t];
                } else if (activeNoteCounts[t][note] > 0) {
                    fmTracks[t].addEvent(juce::MidiMessage::noteOff(midiCh1Based, note), tick480);
                    --activeNoteCounts[t][note];
                }
                break;
            }
            case DemoEventType::FmNoteOff: {
                const std::size_t t    = resolveTrackForChannel(ev.channel);
                const int midiCh1Based = static_cast<int>(t + 1u);
                const uint8_t note     = static_cast<uint8_t>(ev.data1 & 0x7Fu);
                if (activeNoteCounts[t][note] > 0) {
                    fmTracks[t].addEvent(juce::MidiMessage::noteOff(midiCh1Based, note), tick480);
                    --activeNoteCounts[t][note];
                }
                break;
            }
            case DemoEventType::FmControl: {
                const std::size_t t    = resolveTrackForChannel(ev.channel);
                const int midiCh1Based = static_cast<int>(t + 1u);
                fmTracks[t].addEvent(
                    juce::MidiMessage::controllerEvent(midiCh1Based, ev.data1 & 0x7Fu, ev.data2 & 0x7Fu),
                    tick480);
                break;
            }
            case DemoEventType::FmProgram: {
                const std::size_t t    = resolveTrackForChannel(ev.channel);
                const int midiCh1Based = static_cast<int>(t + 1u);
                const bool isPre       = (ev.data1 != 0u);
                const int prog         = std::min<int>(ev.data2, 99);
                fmTracks[t].addEvent(juce::MidiMessage::controllerEvent(midiCh1Based, 0, 0), tick480);
                fmTracks[t].addEvent(juce::MidiMessage::controllerEvent(midiCh1Based, 32, isPre ? 1 : 0), tick480);
                fmTracks[t].addEvent(juce::MidiMessage::programChange(midiCh1Based, prog), tick480);
                break;
            }
            case DemoEventType::FmPitchBend: {
                const std::size_t t    = resolveTrackForChannel(ev.channel);
                const int midiCh1Based = static_cast<int>(t + 1u);
                const int bend14       = static_cast<int>(ev.data1 & 0x7Fu)
                                       | (static_cast<int>(ev.data2 & 0x7Fu) << 7);
                fmTracks[t].addEvent(
                    juce::MidiMessage::pitchWheel(midiCh1Based, std::clamp(bend14, 0, 16383)),
                    tick480);
                break;
            }
            case DemoEventType::FmAftertouch: {
                const std::size_t t    = resolveTrackForChannel(ev.channel);
                const int midiCh1Based = static_cast<int>(t + 1u);
                fmTracks[t].addEvent(
                    juce::MidiMessage::channelPressureChange(midiCh1Based, ev.data1 & 0x7Fu),
                    tick480);
                break;
            }
            case DemoEventType::DrumHit: {
                const uint8_t drumNote = mapDrumIndexToC1C6Note(drumEngine, ev.data1);
                const uint8_t vel      = accentLevelToMidiVelocity(ev.data2);
                rawDrumNotes.push_back({ tick480, drumNote, vel });
                break;
            }
            case DemoEventType::TempoChange:
            case DemoEventType::RhythmVolDelta:
                break;
        }
    }

    // If the song had no embedded DrumHit events (e.g. an 8-track .SEQ song paired with a Rhythm Pattern),
    // tile the currently selected Rhythm Pattern across the song duration on Track 9 (MIDI Channel 10).
    if (rawDrumNotes.empty()) {
        const bool userRhyBank = (pBank.rhythmBank->load() >= 0.5f);
        const std::size_t ptnIdx = static_cast<std::size_t>(
            std::clamp(static_cast<int>(std::lround(pBank.rhythmPattern->load())), 0, 99));

        const vf50::generated::RhythmNoteEvent* ptnEvents = nullptr;
        std::size_t ptnCount = 0u;
        uint32_t ptnLen48    = 192u;

        if (userRhyBank) {
            const auto& uPtn = hostMem.userRhythmPattern(ptnIdx);
            if (uPtn.eventCount > 0u) {
                ptnEvents = uPtn.events.data();
                ptnCount  = uPtn.eventCount;
                ptnLen48  = std::max<uint32_t>(48u, uPtn.totalTicks);
            }
        }
        if (ptnEvents == nullptr || ptnCount == 0u) {
            const std::size_t fIdx = std::min<std::size_t>(ptnIdx, vf50::generated::kFactoryRhythmPtnCount - 1u);
            const auto span = rom.getRhythmPatternEvents(fIdx);
            ptnEvents = span.data();
            ptnCount  = span.size();
            ptnLen48  = std::max<uint32_t>(48u, rom.factoryRhythmPatterns[fIdx].totalTicks);
        }

        if (ptnEvents != nullptr && ptnCount > 0u) {
            for (uint32_t base48 = 0u; base48 < totalTicks48; base48 += ptnLen48) {
                for (std::size_t e = 0; e < ptnCount; ++e) {
                    const uint32_t abs48 = base48 + ptnEvents[e].tick;
                    if (abs48 >= totalTicks48) {
                        break;
                    }
                    const double t480      = static_cast<double>(abs48) * kTickScaleFromVF50;
                    const uint8_t drumNote = mapDrumIndexToC1C6Note(drumEngine, ptnEvents[e].drumIndex);
                    const uint8_t vel      = accentLevelToMidiVelocity(ptnEvents[e].velocityLevel);
                    rawDrumNotes.push_back({ t480, drumNote, vel });
                }
            }
        }
    }

    // Sort and emit strictly non-overlapping drum NoteOn/NoteOff pairs on MIDI Channel 10
    std::sort(rawDrumNotes.begin(), rawDrumNotes.end(), [](const RawDrumNote& a, const RawDrumNote& b) {
        if (a.tick480 != b.tick480) return a.tick480 < b.tick480;
        return a.note < b.note;
    });

    int rhythmNoteCount = 0;
    for (std::size_t i = 0; i < rawDrumNotes.size(); ++i) {
        const auto cur = rawDrumNotes[i];
        // Coalesce duplicate hits of the exact same drum note on the exact same tick
        if (i > 0u && rawDrumNotes[i - 1u].note == cur.note &&
            std::abs(rawDrumNotes[i - 1u].tick480 - cur.tick480) < 0.5) {
            continue;
        }
        double offTick = cur.tick480 + kDrumGateTicks480;
        for (std::size_t j = i + 1u; j < rawDrumNotes.size(); ++j) {
            if (rawDrumNotes[j].tick480 - cur.tick480 > kDrumGateTicks480) {
                break;
            }
            if (rawDrumNotes[j].note == cur.note && rawDrumNotes[j].tick480 > cur.tick480 + 0.5) {
                offTick = std::max(cur.tick480 + 1.0, rawDrumNotes[j].tick480 - 1.0);
                break;
            }
        }
        offTick = std::min(offTick, std::max(cur.tick480 + 1.0, endTick480 - 2.0));
        rhythmSeq.addEvent(juce::MidiMessage::noteOn(kMidiDrumChannel1B, cur.note, cur.vel), cur.tick480);
        rhythmSeq.addEvent(juce::MidiMessage::noteOff(kMidiDrumChannel1B, cur.note), offTick);
        ++rhythmNoteCount;
    }

    // Close any unclosed NoteOns, sort, pair NoteOn/NoteOff, and append Tracks 1..8 + Track 9
    int totalNotes = rhythmNoteCount;
    for (std::size_t t = 0; t < 8u; ++t) {
        const int midiCh1Based = static_cast<int>(t + 1u);
        for (int n = 0; n < 128; ++n) {
            while (activeNoteCounts[t][static_cast<std::size_t>(n)] > 0) {
                fmTracks[t].addEvent(
                    juce::MidiMessage::noteOff(midiCh1Based, static_cast<uint8_t>(n)),
                    endTick480 - 10.0);
                --activeNoteCounts[t][static_cast<std::size_t>(n)];
            }
        }
        fmTracks[t].sort();
        fmTracks[t].updateMatchedPairs();
        fmTracks[t].addEvent(juce::MidiMessage::endOfTrack(), endTick480);
        outMidiFile.addTrack(fmTracks[t]);
        totalNotes += trackNoteTotals[t];
    }

    rhythmSeq.sort();
    rhythmSeq.updateMatchedPairs();
    rhythmSeq.addEvent(juce::MidiMessage::endOfTrack(), endTick480);
    outMidiFile.addTrack(rhythmSeq);

    if (outSummary != nullptr) {
        outSummary->succeeded         = true;
        outSummary->numTracks         = outMidiFile.getNumTracks();
        outSummary->ticksPerQuarter   = outMidiFile.getTimeFormat();
        outSummary->totalNoteOnEvents = totalNotes;
        outSummary->trackNoteCounts   = trackNoteTotals;
        outSummary->rhythmNoteCount   = rhythmNoteCount;
        outSummary->songTitle         = songTitle;
        outSummary->statusMessage     = "EXPORTED MIDI: " + songTitle;
    }
    return true;
}

bool VF50MidiExporter::exportActiveSongToFile(vf50::plugin::VF50AudioProcessor& processor,
                                             const juce::File& targetFile,
                                             MidiExportSummary* outSummary) {
    juce::MidiFile midiFile;
    MidiExportSummary summary{};
    if (!buildMidiFileFromActiveSong(processor, midiFile, &summary)) {
        if (outSummary != nullptr) {
            *outSummary = summary;
        }
        return false;
    }

    juce::String writeErr;
    if (!writeMidiFileTruncatedAndFlushed(midiFile, targetFile, writeErr)) {
        summary.succeeded     = false;
        summary.statusMessage = writeErr;
        if (outSummary != nullptr) {
            *outSummary = summary;
        }
        return false;
    }

    summary.statusMessage = "SAVED MIDI: " + targetFile.getFileName();
    if (outSummary != nullptr) {
        *outSummary = summary;
    }
    return true;
}

bool VF50MidiExporter::writeTempDragMidiFile(vf50::plugin::VF50AudioProcessor& processor,
                                            juce::File& outTempMidiFile,
                                            MidiExportSummary* outSummary) {
    juce::MidiFile midiFile;
    MidiExportSummary summary{};
    if (!buildMidiFileFromActiveSong(processor, midiFile, &summary)) {
        if (outSummary != nullptr) {
            *outSummary = summary;
        }
        return false;
    }

    const juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("VictoryAudio_VF50_Export");
    (void)tempDir.createDirectory();

    const juce::String stem = sanitizeFileStem(summary.songTitle);
    outTempMidiFile = tempDir.getChildFile("VF50_" + stem + "_8Tracks.mid");

    juce::String writeErr;
    if (!writeMidiFileTruncatedAndFlushed(midiFile, outTempMidiFile, writeErr)) {
        summary.succeeded     = false;
        summary.statusMessage = writeErr;
        if (outSummary != nullptr) {
            *outSummary = summary;
        }
        return false;
    }

    summary.statusMessage = "DRAG 8 TRKS: " + summary.songTitle;
    if (outSummary != nullptr) {
        *outSummary = summary;
    }
    return true;
}

} // namespace vf50::disk
