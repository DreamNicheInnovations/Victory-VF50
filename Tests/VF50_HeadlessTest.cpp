#include "DSP/FM/OpzLfo.h"
#include "DSP/FM/OpzTables.h"
#include "DSP/VF50SynthEngine.h"
#include "Disk/VF50DiskManager.h"
#include "Disk/VF50MidiExporter.h"
#include "GUI/VF50DesignTokens.h"
#include "GUI/VF50LcdDisplay.h"
#include "GUI/VF50MainCanvas.h"
#include "GUI/VF50ProgrammerDrawer.h"
#include "GUI/VF50RomGuideOverlay.h"
#include "GUI/VF50VirtualKeyboard.h"
#include "Generated/VF50ParameterLayout.h"
#include "MIDI/VF50SysExHandler.h"
#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"
#include "ROM/VF50RomLoader.h"
#include "State/VF50MemoryManager.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

[[nodiscard]] juce::File directoryFromEnv(const char* envName) {
    const juce::String raw = juce::SystemStats::getEnvironmentVariable(envName, {}).trim();
    if (raw.isEmpty()) {
        return {};
    }
    const juce::File dir(raw);
    return dir.isDirectory() ? dir : juce::File{};
}

[[nodiscard]] juce::File findTestRomDirectory() {
    return directoryFromEnv("VF50_TEST_ROM_DIR");
}

[[nodiscard]] juce::File findTestDiskDirectory() {
    return directoryFromEnv("VF50_TEST_DISK_DIR");
}

[[nodiscard]] juce::File findTestSyxDirectory() {
    return directoryFromEnv("VF50_TEST_SYX_DIR");
}

std::unique_ptr<vf50::rom::VF50RomImage> gRuntimeTestRomImage;

bool installRuntimeTestRomsForFullSuite() {
    const juce::File romRoot = findTestRomDirectory();
    if (!romRoot.isDirectory()) {
        return false;
    }
    vf50::rom::VF50RomSetBuilder builder;
    juce::String st;
    if (builder.scanDirectory(romRoot, st) != 4 || !builder.hasAllFourChips()) {
        std::cerr << "[FAIL] Failed to scan 4 ROM chips from VF50_TEST_ROM_DIR ("
                  << romRoot.getFullPathName() << "): " << st << "\n";
        return false;
    }
    (void)builder.saveConfiguredPaths();
    gRuntimeTestRomImage = builder.buildRomImage(st);
    return gRuntimeTestRomImage != nullptr && gRuntimeTestRomImage->isAllRomsLoaded();
}

[[nodiscard]] const vf50::rom::VF50RomImage& testRom() noexcept {
    return (gRuntimeTestRomImage != nullptr) ? *gRuntimeTestRomImage : vf50::rom::getDefaultRomImage();
}

struct ScheduledEvent {
    std::size_t samplePos = 0u;
    uint8_t     type      = 0u; // 1=NoteOn, 2=NoteOff, 3=CC, 4=SwitchToSplitLayerPerf
    uint8_t     channel   = 0u;
    uint8_t     data1     = 0u;
    uint8_t     data2     = 0u;
};

[[nodiscard]] bool verifyFiniteNonZero(const std::vector<float>& left,
                                       const std::vector<float>& right,
                                       double& outRms) noexcept {
    double sumSq = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
            return false;
        }
        sumSq += static_cast<double>(left[i]) * static_cast<double>(left[i])
               + static_cast<double>(right[i]) * static_cast<double>(right[i]);
    }
    outRms = std::sqrt(sumSq / static_cast<double>(left.size() * 2u));
    return outRms > 1e-5;
}

bool writeStereoWav16(const juce::File& outFile,
                      const juce::AudioBuffer<float>& buffer,
                      double sampleRate) {
    if (outFile.existsAsFile()) {
        outFile.deleteFile();
    }
    std::unique_ptr<juce::FileOutputStream> fileStream(outFile.createOutputStream());
    if (!fileStream || !fileStream->openedOk()) {
        std::cerr << "[FAIL] Unable to open output WAV path: " << outFile.getFullPathName() << "\n";
        return false;
    }

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(fileStream.get(), sampleRate, 2u, 16, {}, 0));
    if (!writer) {
        std::cerr << "[FAIL] Unable to create WAV writer.\n";
        return false;
    }
    fileStream.release();

    if (!writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples())) {
        std::cerr << "[FAIL] Error writing audio frames to WAV.\n";
        return false;
    }
    writer->flush();
    return true;
}

bool runSubsystemSelfTests() {
    std::cout << "[SelfTest:Stage2] Verifying OPZ II waveforms, 0 Hz Fixed LO, 8 FM algorithms, DVA deduplication, "
              << "YM3602 multi-rate drums, and YM3413 DSP...\n";

    const double zeroHz = vf50::dsp::fm::computeOpzFixedFreqHz(0u, 0u, 0u, 1u);
    if (std::abs(zeroHz) > 1e-12) {
        std::cerr << "[FAIL] Fixed LO 0.00 Hz expected 0.0, got " << zeroHz << "\n";
        return false;
    }

    vf50::dsp::VF50SynthEngine engine;
    engine.setRuntimeRomBank(&testRom(), true);
    engine.prepare(48000.0, 512u);
    engine.setDrumsEnabled(false);

    for (uint8_t alg = 0u; alg < 8u; ++alg) {
        auto patch = testRom().factoryVoices[1];
        patch.algorithm = alg;
        patch.feedback  = 6u;
        for (std::size_t op = 0; op < 4u; ++op) {
            patch.operators[op].waveform    = static_cast<uint8_t>((alg + op) & 0x07u);
            patch.operators[op].outputLevel = 92u;
            patch.operators[op].ar          = 31u;
        }
        engine.fm().storeInternalVoice(0u, patch);
        engine.selectSingleVoice(0u, true, true);
        engine.noteOn(0u, 60u, 110u);

        std::vector<float> bufL(1024u, 0.0f);
        std::vector<float> bufR(1024u, 0.0f);
        engine.renderStereoBlock(bufL.data(), bufR.data(), bufL.size());
        engine.allSoundOff();

        double rms = 0.0;
        if (!verifyFiniteNonZero(bufL, bufR, rms)) {
            std::cerr << "[FAIL] FM Algorithm " << static_cast<int>(alg + 1u) << " failed sanity check\n";
            return false;
        }
    }

    engine.reset();
    engine.selectSingleVoice(1u, false, true);
    for (int repeat = 0; repeat < 5; ++repeat) {
        engine.noteOn(0u, 64u, 100u);
    }
    if (engine.fm().keyDownVoiceCount() != 1u) {
        std::cerr << "[FAIL] Single-mode MIDI Note-On deduplication expected 1 active voice\n";
        return false;
    }
    engine.allSoundOff();

    engine.setPlayMode(vf50::dsp::fm::SynthPlayMode::Performance, true);
    engine.selectPerformance(3u, false, true);
    for (int repeat = 0; repeat < 5; ++repeat) {
        engine.noteOn(0u, 64u, 100u);
    }
    if (engine.fm().keyDownVoiceCount() != 2u) {
        std::cerr << "[FAIL] Performance-mode MIDI Note-On deduplication expected 2 voices\n";
        return false;
    }
    engine.allSoundOff();

    constexpr std::array<double, 4> kRates = { 44100.0, 48000.0, 88200.0, 96000.0 };
    for (const double sr : kRates) {
        engine.prepare(sr, 512u);
        engine.setDrumsEnabled(true);
        engine.drums().triggerDrum(28u, 115u);
        engine.drums().triggerDrum(27u, 115u);
        engine.drums().triggerDrum(0u, 120u);
        engine.drums().triggerDrum(6u, 118u);

        std::vector<float> bufL(1024u, 0.0f);
        std::vector<float> bufR(1024u, 0.0f);
        engine.renderStereoBlock(bufL.data(), bufR.data(), bufL.size());

        double rms = 0.0;
        if (!verifyFiniteNonZero(bufL, bufR, rms)) {
            std::cerr << "[FAIL] Multi-rate render failed at " << sr << " Hz\n";
            return false;
        }
    }

    engine.prepare(48000.0, 256u);
    for (uint8_t fx = 1u; fx <= 32u; ++fx) {
        vf50::generated::DspEffectSettings settings{ fx, 75u, 90u, 1u, 18u, 12u, 20u };
        engine.effects().setSettings(settings);
        engine.drums().triggerDrum(6u, 115u);

        std::vector<float> bufL(512u, 0.0f);
        std::vector<float> bufR(512u, 0.0f);
        engine.renderStereoBlock(bufL.data(), bufR.data(), bufL.size());
        for (std::size_t i = 0; i < bufL.size(); ++i) {
            if (!std::isfinite(bufL[i]) || !std::isfinite(bufR[i])) {
                std::cerr << "[FAIL] YM3413 Effect #" << static_cast<int>(fx) << " produced non-finite output\n";
                return false;
            }
        }
        engine.allSoundOff();
    }

    std::cout << "[SelfTest:Stage2] All DSP subsystem unit checks PASSED.\n";
    return true;
}

bool runStage3ArchitectureSelfTests() {
    std::cout << "[SelfTest:Stage3] Verifying 647 APVTS parameters, Workstation Memory Banks, "
              << "VF50 + TX81Z SysEx, DAW State Persistence, and Lock-Free Concurrency...\n";

    // 1. Verify APVTS Parameter Layout & Cached Atomic Parameter Pointers
    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    const auto& pBank = proc->parameterBank();
    for (std::size_t i = 0; i < vf50::params::kTotalParameterCount; ++i) {
        if (pBank.byIndex[i] == nullptr || !std::isfinite(pBank.byIndex[i]->load())) {
            std::cerr << "[FAIL] Null or non-finite APVTS atomic pointer at index " << i << "\n";
            return false;
        }
    }

    // 2. Verify Workstation State Machine (6 modes), Init Templates & Compare Toggle
    vf50::state::VF50MemoryManager mem;
    mem.setRuntimeRomBank(&testRom(), true);
    constexpr std::array<vf50::state::WorkstationMode, 6> kAllModes = {
        vf50::state::WorkstationMode::SinglePlay,
        vf50::state::WorkstationMode::SingleEdit,
        vf50::state::WorkstationMode::PerformancePlay,
        vf50::state::WorkstationMode::PerformanceEdit,
        vf50::state::WorkstationMode::RhythmPlay,
        vf50::state::WorkstationMode::EffectEdit
    };
    for (const auto m : kAllModes) {
        mem.setWorkstationMode(m);
        if (mem.workstationMode() != m || vf50::state::VF50MemoryManager::workstationModeName(m).empty()) {
            std::cerr << "[FAIL] WorkstationMode transition check failed\n";
            return false;
        }
    }

    mem.selectSingleVoice(vf50::state::MemoryBank::FactoryRom, 4u); // E.Piano 1
    mem.setWorkstationMode(vf50::state::WorkstationMode::SingleEdit);
    mem.singleEditSlot().voice.algorithm = 7u;
    mem.singleEditSlot().setName("EditEPiano");
    if (!mem.toggleSingleCompare() || mem.activeSingleVoice().name != testRom().factoryVoices[4].name) {
        std::cerr << "[FAIL] Single Compare toggle to original voice failed\n";
        return false;
    }
    if (mem.toggleSingleCompare() || mem.activeSingleVoice().name != "EditEPiano") {
        std::cerr << "[FAIL] Single Compare restore to edited voice failed\n";
        return false;
    }

    for (uint8_t t = 0u; t <= 6u; ++t) {
        mem.initPerformanceEditBuffer(static_cast<vf50::state::PerfInitTemplate>(t));
        if (mem.perfEditSlot().perf.instruments[0].volume == 0u) {
            std::cerr << "[FAIL] Init Performance template " << static_cast<int>(t) << " invalid\n";
            return false;
        }
    }

    // 3. Verify VF50 SysEx Round-Trips + TX81Z 32-Voice Backward Compatibility
    vf50::midi::VF50SysExHandler txSysEx;
    vf50::midi::VF50SysExHandler rxSysEx;
    vf50::dsp::VF50SynthEngine dummySynth;
    dummySynth.setRuntimeRomBank(&testRom(), true);
    dummySynth.prepare(48000.0, 256u);

    // 3a. Single Voice Edit Dump (ACED3 + ACED2 + ACED + VCED) w/ signed fields
    vf50::state::StoredVoiceSlot customVoice(testRom().factoryVoices[1]);
    customVoice.setName("SysExTest1");
    customVoice.voice.algorithm                  = 5u;
    customVoice.voice.feedback                   = 6u;
    customVoice.voice.atPitchBias                = -33;
    customVoice.voice.bcPitchBias                = 22;
    customVoice.voice.operators[0].levelScaling  = -47;
    customVoice.voice.operators[1].levelScaling  = 64;
    customVoice.voice.operators[0].keyVelSens    = -5;
    customVoice.voice.operators[2].waveform      = 6u;
    customVoice.voice.operators[3].fixedFreqMode = 1u;
    customVoice.voice.operators[3].fixedRangeLow = 1u;
    customVoice.voice.operators[3].fixedRange    = 4u;
    customVoice.voice.effect = vf50::generated::DspEffectSettings{ 14u, 68u, 88u, 1u, 31u, 19u, 45u };
    customVoice.repackRomBytes();

    vf50::midi::SysExReplyBuffer replyBuf;
    if (!txSysEx.emitSingleVoiceEditDump(customVoice.voice, replyBuf, true, true, true) ||
        replyBuf.packetCount != 4u) {
        std::cerr << "[FAIL] Failed to emit 4-packet VF50 Single Voice Edit SysEx dump\n";
        return false;
    }

    vf50::state::VF50MemoryManager rxMem;
    rxMem.setRuntimeRomBank(&testRom(), true);
    const auto editRes = rxSysEx.processSysExStream(
        replyBuf.storage.data(), replyBuf.bytesUsed, rxMem, dummySynth, nullptr);
    const auto& decodedV = rxMem.singleEditSlot().voice;
    if (!editRes.accepted || !editRes.checksumValid ||
        decodedV.name != "SysExTest1" ||
        decodedV.algorithm != 5u ||
        decodedV.atPitchBias != -33 ||
        decodedV.bcPitchBias != 22 ||
        decodedV.operators[0].levelScaling != -47 ||
        decodedV.operators[0].keyVelSens != -5 ||
        decodedV.operators[2].waveform != 6u ||
        decodedV.operators[3].fixedRangeLow != 1u ||
        decodedV.effect.effectType != 14u ||
        decodedV.effect.balance != 68u) {
        std::cerr << "[FAIL] VF50 Single Voice Edit SysEx round-trip mismatch\n";
        return false;
    }

    // 3b. TX81Z Legacy 32-Voice VMEM Backward Compatibility (unprefixed F0 43 00 04 20 00 ... F7, 0x00 PEG/ACED2)
    std::vector<uint8_t> tx81zMsg(4104u, 0u);
    tx81zMsg[0] = 0xF0u;
    tx81zMsg[1] = 0x43u;
    tx81zMsg[2] = 0x00u;
    tx81zMsg[3] = 0x04u;
    tx81zMsg[4] = 0x20u;
    tx81zMsg[5] = 0x00u;
    for (std::size_t slot = 0; slot < 32u; ++slot) {
        uint8_t* rec = tx81zMsg.data() + 6u + slot * 128u;
        // Copy bytes 0..66 & 73..83 from Factory Voice, zero bytes 67..72 (PEG) and 84..127 like real TX81Z
        const auto& srcRom = testRom().factoryVoices[slot].rawRomBytes;
        std::memcpy(rec, srcRom.data(), 67u);
        std::memset(rec + 67u, 0, 6u);
        std::memcpy(rec + 73u, srcRom.data() + 73u, 11u);
        rec[57] = 'T'; rec[58] = 'X'; rec[59] = '8'; rec[60] = '1'; rec[61] = 'Z';
    }
    tx81zMsg[4102] = vf50::midi::VF50SysExHandler::computeSysExChecksum(tx81zMsg.data() + 6u, 4096u);
    tx81zMsg[4103] = 0xF7u;

    rxSysEx.reset();
    const auto tx81zRes = rxSysEx.processSysEx(tx81zMsg.data(), tx81zMsg.size(), rxMem, dummySynth, nullptr);
    const auto& txVoice0 = rxMem.getUserVoice(0u);
    if (!tx81zRes.accepted || !tx81zRes.modifiedUserVoices ||
        txVoice0.name.substr(0, 5) != "TX81Z" ||
        txVoice0.peg.pl1 != 50u || txVoice0.peg.pr1 != 99u ||
        txVoice0.atPitchBias != 0) {
        std::cerr << "[FAIL] TX81Z 32-voice VMEM backward-compatibility check failed\n";
        return false;
    }

    // 3c. VF50 100-Performance Bulk Dump (4 blocks of Header + PMEM + PMEM2) + Microtuning (MCRTE0/MCRTE1)
    vf50::state::StoredPerformanceSlot customPerf(testRom().factoryPerformances[2]);
    customPerf.setName("VF50Prf99 ");
    customPerf.perf.assignMode = 2u;
    customPerf.perf.perfEffectSelect = 8u;
    customPerf.perf.instruments[0].maxNotes = 16u;
    customPerf.perf.instruments[0].reserveNotes = 11u;
    customPerf.perf.instruments[0].effectEnable = 0u;
    customPerf.perf.effect = vf50::generated::DspEffectSettings{ 9u, 55u, 91u, 1u, 24u, 12u, 33u };
    customPerf.repackRomBytes();
    mem.storeUserPerformance(99u, customPerf.perf);

    replyBuf.clear();
    if (!txSysEx.emitPerformanceBankDump100(mem, vf50::state::MemoryBank::UserRam, replyBuf)) {
        std::cerr << "[FAIL] Failed to emit 100-performance PMEM+PMEM2 dump\n";
        return false;
    }
    const auto pmemRes = rxSysEx.processSysExStream(
        replyBuf.storage.data(), replyBuf.bytesUsed, rxMem, dummySynth, nullptr);
    const auto& rxPerf99 = rxMem.getUserPerformance(99u);
    if (!pmemRes.accepted || rxPerf99.name != "VF50Prf99 " ||
        rxPerf99.assignMode != 2u ||
        rxPerf99.perfEffectSelect != 8u ||
        rxPerf99.instruments[0].maxNotes != 16u ||
        rxPerf99.instruments[0].reserveNotes != 11u ||
        rxPerf99.effect.effectType != 9u) {
        std::cerr << "[FAIL] 100-Performance PMEM+PMEM2 round-trip failed\n";
        return false;
    }

    auto octTbl = mem.userOctaveMicrotuning();
    octTbl[3] = { 63u, 29u }; // Quarter-tone E-flat
    replyBuf.clear();
    if (!txSysEx.emitMicrotuningOctaveDump(octTbl, replyBuf)) {
        return false;
    }
    const auto mtRes = rxSysEx.processSysEx(replyBuf.packetData(0), replyBuf.packetSize(0), rxMem, dummySynth, nullptr);
    if (!mtRes.accepted || rxMem.userOctaveMicrotuning()[3].fineStep64 != 29u) {
        std::cerr << "[FAIL] Microtuning MCRTE0 SysEx round-trip failed\n";
        return false;
    }

    // 3d. Real-Time SysEx Dump Request inside VF50AudioProcessor::processBlock
    proc->prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> smallAudio(2, 128);
    juce::MidiBuffer reqMidi;
    const std::array<uint8_t, 15> dumpReq1Voice = {
        0xF0u, 0x43u, 0x20u, 0x7Eu, 'L', 'M', ' ', ' ', '8', '0', '7', '3', 'A', 'E', 0xF7u
    };
    reqMidi.addEvent(dumpReq1Voice.data(), static_cast<int>(dumpReq1Voice.size()), 0);
    proc->processBlock(smallAudio, reqMidi);
    int emittedSysExCount = 0;
    for (const auto meta : reqMidi) {
        if (meta.numBytes > 4 && meta.data[0] == 0xF0u && meta.data[1] == 0x43u) {
            ++emittedSysExCount;
        }
    }
    if (emittedSysExCount != 4) {
        std::cerr << "[FAIL] Expected 4 SysEx packets from VF50 1-Voice Dump Request in processBlock, got "
                  << emittedSysExCount << "\n";
        return false;
    }

    // 4. Verify DAW State Persistence (getStateInformation / setStateInformation)
    proc->hostMemory().storeUserVoice(7u, customVoice.voice);
    const std::array<int8_t, 8> testPans = { -48, 48, -20, 20, 0, 0, 0, 0 };
    proc->hostMemory().storeUserPerformance(3u, customPerf.perf, testPans);
    proc->hostMemory().setUserOctaveMicrotuning(octTbl);
    proc->hostMemory().selectPerformance(vf50::state::MemoryBank::UserRam, 3u);
    proc->hostMemory().setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
    proc->pushHostMemoryToAudio();

    vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::MasterTune, 14.5f);
    vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::QuickBrilliance, 28.0f);
    reqMidi.clear();
    proc->processBlock(smallAudio, reqMidi);

    juce::MemoryBlock savedDawState;
    proc->getStateInformation(savedDawState);
    if (savedDawState.getSize() < 1024u) {
        std::cerr << "[FAIL] Serialized DAW state unexpectedly small\n";
        return false;
    }

    auto restoredProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    restoredProc->prepareToPlay(48000.0, 128);
    restoredProc->setStateInformation(savedDawState.getData(), static_cast<int>(savedDawState.getSize()));
    reqMidi.clear();
    restoredProc->processBlock(smallAudio, reqMidi);

    if (restoredProc->hostMemory().getUserVoice(7u).name != "SysExTest1" ||
        restoredProc->hostMemory().getUserPerformance(3u).name != "VF50Prf99 " ||
        restoredProc->hostMemory().getUserPerformancePartPans(3u)[0] != -48 ||
        restoredProc->hostMemory().userOctaveMicrotuning()[3].fineStep64 != 29u ||
        restoredProc->activeWorkstationMode() != vf50::state::WorkstationMode::PerformancePlay ||
        std::abs(restoredProc->parameterBank().masterTune->load() - 14.5f) > 0.2f) {
        std::cerr << "[FAIL] DAW getStateInformation / setStateInformation round-trip verification failed\n";
        return false;
    }

    // 5. Multi-Threaded Lock-Free Concurrent Parameter Writer vs processBlock Stress Test
    std::atomic<bool> stopWriter{ false };
    std::thread writerThread([&]() {
        int iter = 0;
        while (!stopWriter.load(std::memory_order_relaxed)) {
            const float coarse = static_cast<float>((iter % 12) + 1);
            const float brill  = static_cast<float>((iter % 80) - 40);
            const float alg    = static_cast<float>(iter % 8);
            vf50::params::setApvtsRawValue(restoredProc->apvts(), vf50::params::ParamIndex::Op2Coarse, coarse);
            vf50::params::setApvtsRawValue(restoredProc->apvts(), vf50::params::ParamIndex::QuickBrilliance, brill);
            vf50::params::setApvtsRawValue(restoredProc->apvts(), vf50::params::ParamIndex::VoiceAlgorithm, alg);
            ++iter;
        }
    });

    juce::MidiBuffer noteMidi;
    noteMidi.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(108)), 0);
    restoredProc->processBlock(smallAudio, noteMidi);
    for (int block = 0; block < 200; ++block) {
        noteMidi.clear();
        restoredProc->processBlock(smallAudio, noteMidi);
        for (int s = 0; s < smallAudio.getNumSamples(); ++s) {
            if (!std::isfinite(smallAudio.getSample(0, s)) || !std::isfinite(smallAudio.getSample(1, s))) {
                stopWriter.store(true, std::memory_order_relaxed);
                writerThread.join();
                std::cerr << "[FAIL] Non-finite sample during multi-threaded APVTS stress test\n";
                return false;
            }
        }
    }
    stopWriter.store(true, std::memory_order_relaxed);
    writerThread.join();

    std::cout << "[SelfTest:Stage3] All APVTS, Memory Bank, SysEx, DAW State & Concurrency checks PASSED.\n";
    return true;
}

bool runStage4DspAndVectorGuiSelfTests(const juce::File& snapshotOutPng) {
    std::cout << "[SelfTest:Stage4] Verifying OPZ II 0..31 EG Rate Tables, 0.75 dB/step TL Curve, Feedback Calibration, "
              << "Cubic Hermite Drum Resampling, 2x Vector GUI Scaling (50%..200%), and 40x2 HD44780 LCD...\n";

    // 1. Verify OPZ II Envelope Rate 0..31 Lookup Monotonicity & 0.75 dB/step TL Attenuation Curve
    const auto& opzTbl = vf50::dsp::fm::getOpzTables();
    if (opzTbl.rate0To31DecayStep24[0] != 0) {
        std::cerr << "[FAIL] Expected OPZ II Rate 0 decay step == 0\n";
        return false;
    }
    for (std::size_t r = 2u; r < 32u; ++r) {
        if (opzTbl.rate0To31DecayStep24[r] < opzTbl.rate0To31DecayStep24[r - 1u] ||
            opzTbl.rate0To31DecayStep24[r] <= 0) {
            std::cerr << "[FAIL] Non-monotonic OPZ II logarithmic decay rate table at rate " << r << "\n";
            return false;
        }
    }

    // 8 steps of TL = 8 * 0.75 dB = 6.0206 dB (exact factor of 2.0 in linear amplitude, 64 atten10 units)
    const uint16_t attenTl0 = vf50::dsp::fm::outputLevelToAtten10(99); // TL = 0
    const uint16_t attenTl8 = vf50::dsp::fm::outputLevelToAtten10(91); // TL = 8
    if (attenTl0 != 0u || (attenTl8 - attenTl0) != 64u) {
        std::cerr << "[FAIL] Carrier TL 0.75 dB/step attenuation check failed\n";
        return false;
    }
    const int32_t peakTl0 = opzTbl.lookupWaveform(0u, 256u, attenTl0);
    const int32_t peakTl8 = opzTbl.lookupWaveform(0u, 256u, attenTl8);
    if (peakTl0 <= 0 || std::abs(static_cast<double>(peakTl0) / static_cast<double>(peakTl8) - 2.0) > 0.01) {
        std::cerr << "[FAIL] Linear carrier output ratio across 8 TL steps (6 dB) expected 2.0\n";
        return false;
    }

    // 2. Verify YM3602 4-Point Cubic Hermite Standalone Resampler across 44.1 kHz .. 96 kHz
    vf50::dsp::drums::Ym3602DrumEngine hermiteDrums;
    hermiteDrums.setRuntimeRomBank(&testRom(), true);
    for (const double sr : { 44100.0, 48000.0, 88200.0, 96000.0 }) {
        hermiteDrums.reset();
        hermiteDrums.prepareHostResampler(sr);
        hermiteDrums.setInstrumentPitchSemitones(6u, -3.5f);
        hermiteDrums.triggerDrum(0u, 120u);
        hermiteDrums.triggerDrum(6u, 118u);
        double sumSq = 0.0;
        for (int i = 0; i < 512; ++i) {
            const auto f = hermiteDrums.renderFrameAtHostRate();
            const float l = f.dryOnlyL + f.efctDryL;
            const float r = f.dryOnlyR + f.efctDryR;
            if (!std::isfinite(l) || !std::isfinite(r)) {
                std::cerr << "[FAIL] Non-finite sample in cubic Hermite drum resampler at " << sr << " Hz\n";
                return false;
            }
            sumSq += static_cast<double>(l * l + r * r);
        }
        if (sumSq < 1e-5) {
            std::cerr << "[FAIL] Silent output in cubic Hermite drum resampler at " << sr << " Hz\n";
            return false;
        }
    }

    // 3. Instantiate VST3 Processor + 2x Vector Editor & Verify Zero Layout Clipping Across All 6 Scales
    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 128);

    std::unique_ptr<juce::AudioProcessorEditor> baseEditor(proc->createEditor());
    auto* vf50Editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEditor.get());
    if (vf50Editor == nullptr) {
        std::cerr << "[FAIL] createEditor() did not return VF50AudioProcessorEditor\n";
        return false;
    }

    auto& canvas = vf50Editor->mainCanvas();
    if (canvas.getWidth() != vf50::gui::tokens::kVirtualWidth ||
        canvas.getHeight() != vf50::gui::tokens::kVirtualHeight) {
        std::cerr << "[FAIL] VF50MainCanvas virtual dimensions mismatch (expected 2964x880)\n";
        return false;
    }

    const auto canvasLocal = canvas.getLocalBounds();
    for (int i = 0; i < canvas.getNumChildComponents(); ++i) {
        const auto* child = canvas.getChildComponent(i);
        if (child != nullptr && child->isVisible() && !canvasLocal.contains(child->getBounds())) {
            std::cerr << "[FAIL] Child component '" << child->getName() << "' clips outside 2964x880 virtual canvas\n";
            return false;
        }
    }

    // 3b. Precision Faceplate Geometry, Zero-Overlap & Alignment Verification
    {
        auto findChild = [&canvas](const juce::String& name) -> const juce::Component* {
            for (int i = 0; i < canvas.getNumChildComponents(); ++i) {
                const auto* c = canvas.getChildComponent(i);
                if (c != nullptr && c->getName() == name) {
                    return c;
                }
            }
            return nullptr;
        };

        // Zero pairwise overlap among visible faceplate controls (exclude zero-chrome 2x2 text proxy & modal ROM overlay)
        for (int i = 0; i < canvas.getNumChildComponents(); ++i) {
            const auto* a = canvas.getChildComponent(i);
            if (a == nullptr || !a->isVisible() || (a->getWidth() <= 2 && a->getHeight() <= 2)
                || dynamic_cast<const vf50::gui::VF50RomGuideOverlay*>(a) != nullptr) {
                continue;
            }
            for (int j = i + 1; j < canvas.getNumChildComponents(); ++j) {
                const auto* b = canvas.getChildComponent(j);
                if (b == nullptr || !b->isVisible() || (b->getWidth() <= 2 && b->getHeight() <= 2)
                    || dynamic_cast<const vf50::gui::VF50RomGuideOverlay*>(b) != nullptr) {
                    continue;
                }
                if (a->getBounds().intersects(b->getBounds())) {
                    std::cerr << "[FAIL] Visible faceplate controls overlap: '" << a->getName()
                              << "' " << a->getBounds().toString()
                              << " vs '" << b->getName() << "' " << b->getBounds().toString() << "\n";
                    return false;
                }
            }
        }

        // Req 1: Bottom row utility buttons share exact bottom Y and safe margin above 880
        const auto* btnMixer   = findChild("MIXER_TOGGLE");
        const auto* btnPg      = findChild("PROGRAMMER_TOGGLE");
        const auto* btnKbd     = findChild("KEYBOARD_TOGGLE");
        const auto* btnExpMid  = findChild("EXPORT_MID");
        const auto* dragBadge  = findChild("Drag8TracksToDawBadge");
        if (!btnMixer || !btnPg || !btnKbd || !btnExpMid || !dragBadge) {
            std::cerr << "[FAIL] Missing one or more bottom row utility controls\n";
            return false;
        }
        const int expectedBotY = vf50::gui::tokens::kBottomUtilityBottomY1x * 2;
        if (btnMixer->getBottom() != expectedBotY || btnPg->getBottom() != expectedBotY
            || btnKbd->getBottom() != expectedBotY || btnExpMid->getBottom() != expectedBotY
            || dragBadge->getBottom() != expectedBotY) {
            std::cerr << "[FAIL] Bottom row utility buttons bottom-Y mismatch (expected " << expectedBotY << ")\n";
            return false;
        }
        if (vf50::gui::tokens::kExportWellRect2x.y + vf50::gui::tokens::kExportWellRect2x.h
            > vf50::gui::tokens::kVirtualHeight - 20) {
            std::cerr << "[FAIL] Export sub-frame too close to bottom bezel\n";
            return false;
        }

        // Req 2: SYSTEM buttons (SEQ, RHYTHM, JOB) share exact Y top & bottom with [-]
        const auto* btnSeq    = findChild("SEQ");
        const auto* btnRhy    = findChild("RHYTHM");
        const auto* btnJob    = findChild("JOB");
        const auto* btnMinus  = findChild("INST_ON_OFF");
        if (!btnSeq || !btnRhy || !btnJob || !btnMinus) {
            std::cerr << "[FAIL] Missing SYSTEM / [-] controls\n";
            return false;
        }
        if (btnSeq->getY() != btnMinus->getY() || btnSeq->getBottom() != btnMinus->getBottom()
            || btnRhy->getY() != btnMinus->getY() || btnRhy->getBottom() != btnMinus->getBottom()
            || btnJob->getY() != btnMinus->getY() || btnJob->getBottom() != btnMinus->getBottom()) {
            std::cerr << "[FAIL] SYSTEM row (SEQ/RHYTHM/JOB) not vertically aligned with [-]\n";
            return false;
        }

        // Req 3: 8 softkeys F1-F8 aligned to painted 40-char LCD grid
        const float leftX  = canvas.lcdDisplay().paintedCharGridLeftXInParent();
        const float rightX = canvas.lcdDisplay().paintedCharGridRightXInParent();
        const float cellW  = (rightX - leftX) / static_cast<float>(vf50::gui::VF50LcdDisplay::kCols);
        const int expSoftW = static_cast<int>(std::lround(4.0f * cellW));
        const float expGap = (8.0f / 7.0f) * cellW;
        const juce::Component* softBtns[8]{};
        for (int i = 0; i < 8; ++i) {
            softBtns[i] = findChild("SoftKey_" + juce::String(i + 1));
            if (softBtns[i] == nullptr || softBtns[i]->getWidth() != expSoftW) {
                std::cerr << "[FAIL] SoftKey_" << (i + 1) << " missing or width != 4*charCellWidth\n";
                return false;
            }
        }
        if (softBtns[0]->getX() != static_cast<int>(std::lround(leftX))
            || softBtns[7]->getRight() != static_cast<int>(std::lround(rightX))) {
            std::cerr << "[FAIL] F1 left or F8 right not flush with LCD 40-char grid ("
                      << softBtns[0]->getX() << ".." << softBtns[7]->getRight()
                      << " vs " << leftX << ".." << rightX << ")\n";
            return false;
        }
        for (int i = 0; i < 7; ++i) {
            const float actualGap = static_cast<float>(softBtns[i + 1]->getX() - softBtns[i]->getRight());
            if (std::abs(actualGap - expGap) > 1.0f) {
                std::cerr << "[FAIL] SoftKey gap " << i << " mismatch: " << actualGap << " vs " << expGap << "\n";
                return false;
            }
        }

        // Req 4: Numeric keypad buttons sit inside lowered keypad well aligned to [-] top
        const juce::Rectangle<int> kpWell(vf50::gui::tokens::kKeypadWellRect2x.x,
                                          vf50::gui::tokens::kKeypadWellRect2x.y,
                                          vf50::gui::tokens::kKeypadWellRect2x.w,
                                          vf50::gui::tokens::kKeypadWellRect2x.h);
        if (kpWell.getY() != btnMinus->getY()) {
            std::cerr << "[FAIL] Keypad well top Y does not align with [-] top Y\n";
            return false;
        }
        int keypadFound = 0;
        for (int i = 0; i < canvas.getNumChildComponents(); ++i) {
            const auto* c = canvas.getChildComponent(i);
            if (c != nullptr && c->getName().startsWith("Keypad_")) {
                ++keypadFound;
                if (!kpWell.contains(c->getBounds())) {
                    std::cerr << "[FAIL] Keypad button '" << c->getName() << "' outside keypad well\n";
                    return false;
                }
            }
        }
        if (keypadFound != 12) {
            std::cerr << "[FAIL] Expected 12 Keypad_* buttons, found " << keypadFound << "\n";
            return false;
        }

        // Req 5: MEMORY (INT, CARD, PRESET) aligned horizontally with EDIT rows 3, 4, 5
        const auto* memInt    = findChild("MEM_INT");
        const auto* memCard   = findChild("MEM_CARD");
        const auto* memPreset = findChild("MEM_PRESET");
        const auto* edit3     = findChild("Edit_3");
        const auto* edit4     = findChild("Edit_4");
        const auto* edit5     = findChild("Edit_5");
        if (!memInt || !memCard || !memPreset || !edit3 || !edit4 || !edit5) {
            std::cerr << "[FAIL] Missing MEMORY or EDIT controls\n";
            return false;
        }
        if (memInt->getY() != edit3->getY() || memInt->getBottom() != edit3->getBottom()
            || memCard->getY() != edit4->getY() || memCard->getBottom() != edit4->getBottom()
            || memPreset->getY() != edit5->getY() || memPreset->getBottom() != edit5->getBottom()) {
            std::cerr << "[FAIL] MEMORY buttons not aligned with EDIT rows 3..5\n";
            return false;
        }
    }

    for (const float scale : vf50::gui::tokens::kDiscreteUiScales) {
        vf50Editor->applyScaleFactor(scale);
        const float tf = vf50::gui::tokens::canvasTransformForUserScale(scale);
        const int expW = static_cast<int>(std::lround(static_cast<float>(vf50::gui::tokens::kVirtualWidth) * tf));
        const int expH = static_cast<int>(std::lround(static_cast<float>(vf50::gui::tokens::kVirtualHeight) * tf));
        if (std::abs(vf50Editor->getWidth() - expW) > 2 || std::abs(vf50Editor->getHeight() - expH) > 2) {
            std::cerr << "[FAIL] Editor scaled bounds mismatch at scale " << scale
                      << " (got " << vf50Editor->getWidth() << "x" << vf50Editor->getHeight()
                      << ", expected " << expW << "x" << expH << ")\n";
            return false;
        }

        // Render offscreen snapshot (full window at <= 1.25x, representative crop at > 1.25x to keep test fast)
        const juce::Rectangle<int> snapArea = (scale <= 1.25f)
            ? vf50Editor->getLocalBounds()
            : juce::Rectangle<int>(0, 0, std::min(vf50Editor->getWidth(), 1600), std::min(vf50Editor->getHeight(), 600));
        const juce::Image img = vf50Editor->createComponentSnapshot(snapArea, true, 1.0f);
        if (!img.isValid() || img.getWidth() <= 0 || img.getHeight() <= 0) {
            std::cerr << "[FAIL] Invalid offscreen vector snapshot at scale " << scale << "\n";
            return false;
        }

        // Verify non-blank procedural rendering (multiple distinct RGB shades present)
        const auto p0 = img.getPixelAt(img.getWidth() / 8, img.getHeight() / 2).getARGB();
        const auto p1 = img.getPixelAt(img.getWidth() / 2, img.getHeight() / 5).getARGB();
        const auto p2 = img.getPixelAt((img.getWidth() * 3) / 4, img.getHeight() / 2).getARGB();
        if (p0 == p1 && p1 == p2) {
            std::cerr << "[FAIL] Blank/uniform vector snapshot detected at scale " << scale << "\n";
            return false;
        }

        // Save default 100% 1x desktop (1482x440) vector panel PNG for visual verification
        if (std::abs(scale - 1.00f) < 0.01f && snapshotOutPng.getFullPathName().isNotEmpty()) {
            if (snapshotOutPng.existsAsFile()) {
                snapshotOutPng.deleteFile();
            }
            juce::PNGImageFormat pngFmt;
            std::unique_ptr<juce::FileOutputStream> pngStream(snapshotOutPng.createOutputStream());
            if (pngStream && pngStream->openedOk()) {
                (void)pngFmt.writeImageToStream(img, *pngStream);
            }
        }
    }

    // 4. Verify Embedded HD44780 5x8 ROM, 3 LCD Backlight Themes & Live Menu -> Data Entry -> APVTS Wiring
    auto& lcd = canvas.lcdDisplay();
    if (vf50::gui::VF50LcdDisplay::getGlyphBitmap(static_cast<uint8_t>('V'))[0] == 0u ||
        vf50::gui::VF50LcdDisplay::getGlyphBitmap(0x08u)[0] != 0x1Fu ||
        vf50::gui::VF50LcdDisplay::getGlyphBitmap(0x7Eu)[3] != 0x1Fu) {
        std::cerr << "[FAIL] Embedded HD44780 5x8 character bitmap ROM verification failed\n";
        return false;
    }

    std::array<juce::uint32, 3> themeSamples{};
    for (int t = 0; t < 3; ++t) {
        lcd.setThemeIndex(t);
        const juce::Image lcdImg = lcd.createComponentSnapshot(lcd.getLocalBounds(), true, 0.5f);
        themeSamples[static_cast<std::size_t>(t)] = lcdImg.getPixelAt(lcdImg.getWidth() / 2, 12).getARGB();
    }
    if (themeSamples[0] == themeSamples[1] || themeSamples[1] == themeSamples[2] || themeSamples[0] == themeSamples[2]) {
        std::cerr << "[FAIL] Expected 3 distinct LCD backlight theme substrates\n";
        return false;
    }
    lcd.setThemeIndex(0); // Restore Factory Yellow-Green

    // Interactive menu navigation + Data Entry fader -> live Classic 40x2 LCD & APVTS update
    canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SingleAlgorithm);
    canvas.setDataEntryNormalized(1.0f); // Push Data Entry fader to MAX -> ALG=8 (raw index 7)

    juce::AudioBuffer<float> audioBlock(2, 128);
    juce::MidiBuffer midiBlock;
    midiBlock.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(108)), 0);
    proc->processBlock(audioBlock, midiBlock);

    const int liveAlg = static_cast<int>(std::lround(proc->parameterBank().voiceAlgorithm->load()));
    const juce::String lcdRow1 = lcd.getLineAsString(1);
    if (liveAlg != 7 || !lcdRow1.contains("ALG=8")) {
        std::cerr << "[FAIL] Live Data Entry -> APVTS & 40x2 LCD sync failed (liveAlg=" << liveAlg
                  << ", lcdRow1='" << lcdRow1 << "')\n";
        return false;
    }

    std::cout << "[SelfTest:Stage4] All DSP Calibration, 2x Vector GUI Multi-Scale & 40x2 HD44780 LCD checks PASSED.\n";
    return true;
}

void configureSplitLayerPerformance(vf50::dsp::VF50SynthEngine& engine) noexcept {
    vf50::generated::FactoryPerformance perf = testRom().factoryPerformances[0];
    perf.name             = "SplitLayer";
    perf.assignMode       = 2u;
    perf.microTuneTable   = 0u;
    perf.microTuneRootKey = 0u;
    perf.perfEffectSelect = 0u;

    for (auto& inst : perf.instruments) {
        inst = vf50::generated::PerformanceInstrument{
            0u, 0u, 1u, 0u, 0u, 0u, 0u, 0, 0, 0u, 0u, 0u, 0u, 0u
        };
    }

    perf.instruments[0] = vf50::generated::PerformanceInstrument{
        4u, 3u, 1u, 20u, 0u, 0u, 59u, 0, 12, 92u, 3u, 1u, 0u, 0u
    };
    perf.instruments[1] = vf50::generated::PerformanceInstrument{
        4u, 5u, 1u, 1u, 0u, 60u, 127u, -3, 0, 90u, 1u, 1u, 0u, 1u
    };
    perf.instruments[2] = vf50::generated::PerformanceInstrument{
        4u, 5u, 1u, 4u, 0u, 60u, 127u, 3, 0, 94u, 2u, 2u, 0u, 1u
    };
    perf.instruments[3] = vf50::generated::PerformanceInstrument{
        4u, 5u, 1u, 0u, 0u, 60u, 127u, 1, 12, 78u, 3u, 1u, 0u, 1u
    };
    perf.effect = vf50::generated::DspEffectSettings{
        1u, 100u, 72u, 1u, 22u, 8u, 18u
    };

    engine.fm().storeInternalPerformance(0u, perf);
}

bool renderStage2ShowcaseWav(const juce::File& outFile) {
    constexpr double kSampleRate  = 48000.0;
    constexpr double kDurationSec = 6.0;
    constexpr std::size_t kTotalFrames = static_cast<std::size_t>(kSampleRate * kDurationSec);
    constexpr double kSecPerBeat = 60.0 / 160.0;

    auto beatToSample = [](double beat) -> std::size_t {
        const double sec = std::clamp(beat * kSecPerBeat, 0.0, kDurationSec - 0.001);
        return static_cast<std::size_t>(std::llround(sec * kSampleRate));
    };

    vf50::dsp::VF50SynthEngine engine;
    engine.setRuntimeRomBank(&testRom(), true);
    engine.prepare(kSampleRate, 512u);
    engine.setMasterVolume(0.82f);
    engine.setDrumsEnabled(true);
    engine.drums().setRhythmReceiveChannel(9u);
    engine.drums().setEffectMode(vf50::dsp::drums::RhythmEffectMode::Mix);
    engine.drums().setRhythmMasterVolume(86u);

    configureSplitLayerPerformance(engine);

    engine.setPlayMode(vf50::dsp::fm::SynthPlayMode::Single, true);
    engine.selectSingleVoice(0u, false, true);
    engine.effects().setSettings(vf50::generated::DspEffectSettings{ 1u, 100u, 75u, 1u, 22u, 8u, 18u });

    std::vector<ScheduledEvent> timeline;
    timeline.reserve(256u);

    auto addNote = [&](double startBeat, double lengthBeats, uint8_t ch, uint8_t note, uint8_t vel) {
        timeline.push_back({ beatToSample(startBeat), 1u, ch, note, vel });
        timeline.push_back({ beatToSample(startBeat + lengthBeats), 2u, ch, note, 0u });
    };
    auto addDrum = [&](double beat, uint8_t drumMidiNote, uint8_t vel) {
        timeline.push_back({ beatToSample(beat), 1u, 9u, drumMidiNote, vel });
    };
    auto addCc = [&](double beat, uint8_t ch, uint8_t cc, uint8_t val) {
        timeline.push_back({ beatToSample(beat), 3u, ch, cc, val });
    };

    struct MelodyNote { double beat; double dur; uint8_t note; uint8_t vel; uint8_t panCc; };
    constexpr std::array<MelodyNote, 10> kSoloMelody = {{
        { 0.0, 0.90, 79u, 108u, 42u },
        { 1.0, 0.90, 83u, 104u, 52u },
        { 2.0, 0.90, 86u, 112u, 68u },
        { 3.0, 0.45, 88u, 106u, 78u },
        { 3.5, 0.45, 86u, 102u, 72u },
        { 4.0, 1.40, 91u, 116u, 86u },
        { 5.5, 0.45, 88u, 104u, 64u },
        { 6.0, 0.90, 86u, 110u, 48u },
        { 7.0, 0.85, 83u, 105u, 58u },
        { 7.5, 0.40, 79u, 100u, 64u },
    }};

    addCc(0.0, 0u, 1u, 38u);
    for (const auto& m : kSoloMelody) {
        addCc(m.beat, 0u, 10u, m.panCc);
        addNote(m.beat, m.dur, 0u, m.note, m.vel);
    }

    timeline.push_back({ beatToSample(8.0), 4u, 0u, 0u, 0u });
    addCc(8.0, 0u, 10u, 64u);

    addNote( 8.0, 0.85, 0u, 40u, 114u);
    addNote( 9.0, 0.40, 0u, 40u, 102u);
    addNote( 9.5, 0.40, 0u, 47u, 108u);
    addNote(10.0, 0.85, 0u, 48u, 115u);
    addNote(11.0, 0.85, 0u, 43u, 110u);
    addNote(12.0, 0.85, 0u, 45u, 116u);
    addNote(13.0, 0.85, 0u, 40u, 108u);
    addNote(14.0, 1.75, 0u, 38u, 118u);

    addNote( 8.0, 1.80, 0u, 67u, 108u);
    addNote( 8.0, 1.80, 0u, 71u, 106u);
    addNote( 8.0, 1.80, 0u, 74u, 110u);
    addNote(10.0, 1.80, 0u, 67u, 106u);
    addNote(10.0, 1.80, 0u, 71u, 108u);
    addNote(10.0, 1.80, 0u, 76u, 112u);
    addNote(12.0, 1.80, 0u, 67u, 108u);
    addNote(12.0, 1.80, 0u, 72u, 110u);
    addNote(12.0, 1.80, 0u, 76u, 114u);
    addNote(14.0, 1.75, 0u, 67u, 110u);
    addNote(14.0, 1.75, 0u, 69u, 112u);
    addNote(14.0, 1.75, 0u, 74u, 116u);

    for (int bar = 0; bar < 4; ++bar) {
        const double b0 = static_cast<double>(bar) * 4.0;
        if (bar == 0 || bar == 2) addDrum(b0 + 0.0, 60u, 116u);
        addDrum(b0 + 0.0, 36u, 122u);
        addDrum(b0 + 1.5, 45u, 108u);
        addDrum(b0 + 2.0, 36u, 120u);
        if (bar % 2 == 1) addDrum(b0 + 2.5, 45u, 112u);
        addDrum(b0 + 1.0, 52u, 118u);
        addDrum(b0 + 3.0, 52u, 122u);
        if (bar >= 2) {
            addDrum(b0 + 1.0, 84u, 96u);
            addDrum(b0 + 3.0, 84u, 100u);
        }
        for (int eighth = 0; eighth < 8; ++eighth) {
            const double pos = b0 + static_cast<double>(eighth) * 0.5;
            if (eighth == 5) addDrum(pos, 59u, 108u);
            else addDrum(pos, 57u, (eighth % 2 == 0) ? 104u : 88u);
            if (bar >= 2 && (eighth % 2 == 0)) addDrum(pos, 62u, 92u);
        }
        if (bar == 1 || bar == 3) {
            addDrum(b0 + 3.00, 53u, 112u);
            addDrum(b0 + 3.25, 50u, 114u);
            addDrum(b0 + 3.50, 48u, 116u);
            addDrum(b0 + 3.75, 47u, 118u);
        }
    }

    std::stable_sort(timeline.begin(), timeline.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) {
        if (a.samplePos != b.samplePos) return a.samplePos < b.samplePos;
        return a.type > b.type;
    });

    juce::AudioBuffer<float> renderBuffer(2, static_cast<int>(kTotalFrames));
    renderBuffer.clear();
    float* outL = renderBuffer.getWritePointer(0);
    float* outR = renderBuffer.getWritePointer(1);

    std::size_t currentSample = 0u;
    std::size_t eventIdx = 0u;
    constexpr std::size_t kMaxChunk = 256u;

    while (currentSample < kTotalFrames) {
        while (eventIdx < timeline.size() && timeline[eventIdx].samplePos <= currentSample) {
            const auto& ev = timeline[eventIdx++];
            switch (ev.type) {
                case 1u: engine.noteOn(ev.channel, ev.data1, ev.data2); break;
                case 2u: engine.noteOff(ev.channel, ev.data1); break;
                case 3u: engine.controlChange(ev.channel, ev.data1, ev.data2); break;
                case 4u:
                    engine.allNotesOff();
                    engine.setPlayMode(vf50::dsp::fm::SynthPlayMode::Performance, true);
                    engine.selectPerformance(0u, true, true);
                    break;
                default: break;
            }
        }
        std::size_t nextStop = kTotalFrames;
        if (eventIdx < timeline.size()) nextStop = std::min(nextStop, timeline[eventIdx].samplePos);
        const std::size_t chunk = std::min(kMaxChunk, nextStop - currentSample);
        if (chunk > 0u) {
            engine.renderStereoBlock(outL + currentSample, outR + currentSample, chunk);
            currentSample += chunk;
        }
    }

    if (!writeStereoWav16(outFile, renderBuffer, kSampleRate)) return false;
    std::cout << "[HeadlessRender:Stage2] Wrote 6.0s stereo WAV (" << kTotalFrames << " frames @ 48000 Hz) to:\n  "
              << outFile.getFullPathName() << "\n";
    return true;
}

bool renderStage3AutomationSweepWav(const juce::File& outFile) {
    // =========================================================================
    // Stage 3 Automated Parameter Sweep & User Performance Switch Test (4.0s):
    //   * Full end-to-end render through VF50AudioProcessor::processBlock + APVTS
    //   * 0.00s .. 2.50s: Load Factory Voice #1 (P00 "Strings 1 "), hold warm
    //     polyphonic chord, and continuously automate:
    //       - FM Operator Frequency (OP2 & OP4 Coarse + Fine ratios)
    //       - FM Algorithm (ALG 5 -> ALG 2 -> ALG 6 -> ALG 1 mid-note!)
    //       - Filter / FM Brilliance (Quick Brilliance -45 -> +78 + OP2/OP4 TL)
    //   * 2.50s .. 4.00s: Switch via APVTS to a custom 4-Part Stereo User
    //     Performance in User RAM Slot I00 ("UserSplit1") and render full
    //     stereo split/layer chords to completion.
    // =========================================================================
    constexpr double kSampleRate       = 48000.0;
    constexpr int    kBlockSize        = 64;
    constexpr int    kTotalFrames      = 48000 * 4; // Exact 4.000 seconds (192,000 samples)
    constexpr int    kPerfSwitchSample = 48000 * 5 / 2; // 2.500 seconds (120,000 samples)

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();

    // Populate User Performance RAM Slot I00 ("UserSplit1") with wide stereo 4-part split/layer
    vf50::generated::FactoryPerformance userPerf =
        vf50::state::VF50MemoryManager::makeInitPerformance(vf50::state::PerfInitTemplate::Split);
    userPerf.assignMode = 2u; // DVA 16-voice allocation
    for (auto& inst : userPerf.instruments) {
        inst = vf50::generated::PerformanceInstrument{
            0u, 0u, 1u, 0u, 0u, 0u, 0u, 0, 0, 0u, 0u, 0u, 0u, 0u
        };
    }
    // Part 1: SynBass 1 (Lower Split 0..59, Center)
    userPerf.instruments[0] = vf50::generated::PerformanceInstrument{
        4u, 2u, 1u, 20u, 0u, 0u, 59u, 0, 12, 92u, 3u, 1u, 0u, 1u
    };
    // Part 2: PowerBrass (Upper Layer 60..127, Hard Left Pan -52)
    userPerf.instruments[1] = vf50::generated::PerformanceInstrument{
        4u, 5u, 1u, 1u, 0u, 60u, 127u, -3, 0, 88u, 1u, 1u, 0u, 1u
    };
    // Part 3: E.Piano 1 (Upper Layer 60..127, Hard Right Pan +52)
    userPerf.instruments[2] = vf50::generated::PerformanceInstrument{
        4u, 5u, 1u, 4u, 0u, 60u, 127u, 3, 0, 92u, 2u, 2u, 0u, 1u
    };
    // Part 4: Strings 1 (Upper Shimmer 60..127, Stereo +12st)
    userPerf.instruments[3] = vf50::generated::PerformanceInstrument{
        4u, 4u, 1u, 0u, 0u, 60u, 127u, 2, 12, 82u, 3u, 1u, 0u, 1u
    };
    userPerf.effect = vf50::generated::DspEffectSettings{ 1u, 100u, 74u, 1u, 22u, 8u, 20u };

    vf50::state::StoredPerformanceSlot perfSlot(userPerf);
    perfSlot.setName("UserSplit1");
    const std::array<int8_t, 8> userPans = { 0, -52, 52, 14, 0, 0, 0, 0 };
    proc->hostMemory().storeUserPerformance(0u, perfSlot.perf, userPans);
    proc->hostMemory().selectSingleVoice(vf50::state::MemoryBank::FactoryRom, 0u); // P00 "Strings 1 "
    proc->hostMemory().setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
    proc->pushHostMemoryToAudio();

    proc->prepareToPlay(kSampleRate, kBlockSize);
    proc->syncApvtsToActiveSingleVoice(true);
    vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::MasterVolume, 0.82f);

    juce::AudioBuffer<float> fullRender(2, kTotalFrames);
    fullRender.clear();
    juce::AudioBuffer<float> blockBuf(2, kBlockSize);
    juce::MidiBuffer midiBuf;

    bool switchedToUserPerf  = false;
    bool chord2Triggered     = false;
    bool finalReleaseSent    = false;
    int  lastAlgChoice       = -1;

    for (int framePos = 0; framePos < kTotalFrames; framePos += kBlockSize) {
        const int framesThisBlock = std::min(kBlockSize, kTotalFrames - framePos);
        blockBuf.setSize(2, framesThisBlock, false, false, true);
        blockBuf.clear();
        midiBuf.clear();

        const double tSec = static_cast<double>(framePos) / kSampleRate;

        if (framePos == 0) {
            // Trigger Factory Voice #1 ("Strings 1 ", transpose = -19 -> MIDI 79,83,86,91 = C4maj7)
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 79, static_cast<juce::uint8>(108)), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 83, static_cast<juce::uint8>(104)), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 86, static_cast<juce::uint8>(110)), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 91, static_cast<juce::uint8>(112)), 0);
        }

        if (framePos < kPerfSwitchSample) {
            // Smooth normalized 0..1 progress through the 2.50s Single Voice automation sweep
            const double norm = static_cast<double>(framePos) / static_cast<double>(kPerfSwitchSample);

            // 1. Automate FM Operator Frequency (OP2 & OP4 Coarse + Fine harmonic sweep)
            const double op2FreqCurve = 1.0 + 5.0 * std::sin(norm * 3.14159265358979323846);
            const int op2Coarse = std::clamp(static_cast<int>(std::floor(op2FreqCurve)), 1, 8);
            const int op2Fine   = std::clamp(static_cast<int>(std::lround((op2FreqCurve - std::floor(op2FreqCurve)) * 15.0)), 0, 15);
            const int op4Fine   = std::clamp(static_cast<int>(std::lround(norm * 15.0)), 0, 15);
            const int op4Coarse = 1 + static_cast<int>(norm * 3.9);

            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::Op2Coarse, static_cast<float>(op2Coarse));
            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::Op2Fine, static_cast<float>(op2Fine));
            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::Op4Coarse, static_cast<float>(op4Coarse));
            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::Op4Fine, static_cast<float>(op4Fine));

            // 2. Automate FM Algorithm across 4 topologies (ALG 5 -> ALG 2 -> ALG 6 -> ALG 1)
            int targetAlg = 4; // ALG 5 (0-based 4)
            if (tSec >= 1.85) targetAlg = 0;      // ALG 1
            else if (tSec >= 1.25) targetAlg = 5; // ALG 6
            else if (tSec >= 0.65) targetAlg = 1; // ALG 2

            if (targetAlg != lastAlgChoice) {
                lastAlgChoice = targetAlg;
                vf50::params::setApvtsRawValue(proc->apvts(),
                                              vf50::params::ParamIndex::VoiceAlgorithm,
                                              static_cast<float>(targetAlg));
            }

            // 3. Automate Filter / FM Brilliance (-45 warm mellow -> +78 bright harmonic -> +20)
            const double brillCurve = (norm < 0.72)
                ? (-45.0 + (norm / 0.72) * 123.0)
                : (78.0 - ((norm - 0.72) / 0.28) * 58.0);
            vf50::params::setApvtsRawValue(proc->apvts(),
                                          vf50::params::ParamIndex::QuickBrilliance,
                                          static_cast<float>(std::lround(brillCurve)));
        } else if (!switchedToUserPerf) {
            switchedToUserPerf = true;

            // Release Single Voice notes cleanly and switch via APVTS to User Performance I00
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 79), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 83), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 86), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 91), 0);

            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::QuickBrilliance, 0.0f);
            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::PerfBank, 1.0f);   // User RAM
            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::PerfPreset, 0.0f); // PF I00
            vf50::params::setApvtsRawValue(proc->apvts(),
                                          vf50::params::ParamIndex::WorkstationMode,
                                          static_cast<float>(vf50::state::WorkstationMode::PerformancePlay));

            // Trigger Chord 1 on User Performance (Bass E2 + Upper Stereo Em9)
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 40, static_cast<juce::uint8>(114)), 8);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(108)), 8);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 71, static_cast<juce::uint8>(106)), 8);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 74, static_cast<juce::uint8>(110)), 8);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 78, static_cast<juce::uint8>(108)), 8);
        } else if (!chord2Triggered && tSec >= 3.25) {
            chord2Triggered = true;
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 40), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 71), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 74), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 78), 0);

            // Trigger Chord 2 on User Performance (Bass C3 + Upper Stereo Cmaj9)
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(116)), 4);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(108)), 4);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 71, static_cast<juce::uint8>(110)), 4);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 76, static_cast<juce::uint8>(112)), 4);
            midiBuf.addEvent(juce::MidiMessage::noteOn(1, 79, static_cast<juce::uint8>(110)), 4);

            // Subtle brilliance swell on the final User Performance chord
            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::QuickBrilliance, 18.0f);
        } else if (!finalReleaseSent && tSec >= 3.86) {
            finalReleaseSent = true;
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 48), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 71), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 76), 0);
            midiBuf.addEvent(juce::MidiMessage::noteOff(1, 79), 0);
        }

        proc->processBlock(blockBuf, midiBuf);
        fullRender.copyFrom(0, framePos, blockBuf, 0, 0, framesThisBlock);
        fullRender.copyFrom(1, framePos, blockBuf, 1, 0, framesThisBlock);
    }

    if (proc->activeWorkstationMode() != vf50::state::WorkstationMode::PerformancePlay ||
        proc->activePerfBank() != vf50::state::MemoryBank::UserRam ||
        proc->synthEngine().fm().activePerformance().name != "UserSplit1") {
        std::cerr << "[FAIL] Expected active User Performance 'UserSplit1' at end of automation sweep\n";
        return false;
    }

    if (!writeStereoWav16(outFile, fullRender, kSampleRate)) return false;
    std::cout << "[HeadlessRender:Stage3] Wrote 4.0s automated sweep WAV (" << kTotalFrames
              << " frames @ 48000 Hz) to:\n  " << outFile.getFullPathName() << "\n";
    return true;
}

bool runStage5HardwareAndWorkflowSelfTests(const juce::File& buildDir) {
    std::cout << "[SelfTest:Stage5] Verifying OPZ II Fixed-Freq & Operator Order, Rhythm MIDI 31..91 (G0..G5) + LCD, "
              << "ROM Demo Songs, 100%=1x UI Scaling, FAT12 Floppy Disk Manager, and 61-Key Virtual Keyboard...\n";

    // =========================================================================
    // 1. Task 1: FM Fixed Frequency (FIX) Formula & Operator Unpacking Order
    // =========================================================================
    {
        // (coarse >> 2) == 0 uses (8 + fine) Hz base at Range 0, HI shift (0)
        const double fix12Hz = vf50::dsp::fm::computeOpzFixedFreqHz(0u, 4u, 0u, 0u);
        const double fix15Hz = vf50::dsp::fm::computeOpzFixedFreqHz(2u, 7u, 0u, 0u);
        const double fixZero = vf50::dsp::fm::computeOpzFixedFreqHz(0u, 7u, 0u, 1u);
        const double fixLoR1 = vf50::dsp::fm::computeOpzFixedFreqHz(4u, 0u, 1u, 1u); // (16 << 1) * (171/51200)
        if (std::abs(fix12Hz - 12.0) > 1e-6 ||
            std::abs(fix15Hz - 15.0) > 1e-6 ||
            std::abs(fixZero) > 1e-12 ||
            std::abs(fixLoR1 - (32.0 * (171.0 / 51200.0))) > 1e-6) {
            std::cerr << "[FAIL] OPZ II Fixed Frequency lookup verification failed (fix12=" << fix12Hz
                      << ", fix15=" << fix15Hz << ", fixZero=" << fixZero << ")\n";
            return false;
        }

        // Verify P76 "SpaceBell" (ALG 4) & P79 "Suspense" (Asymmetric ALG 6: OP4 -> OP1+2+3)
        const auto& spaceBell = testRom().factoryVoices[76];
        const auto& suspense  = testRom().factoryVoices[79];
        if (spaceBell.algorithm != 3u || spaceBell.operators[0].outputLevel != 94u ||
            spaceBell.operators[1].outputLevel != 0u || spaceBell.operators[2].outputLevel != 54u ||
            spaceBell.operators[3].outputLevel != 82u || spaceBell.operators[0].ampModEnable != 1u ||
            suspense.algorithm != 5u || suspense.operators[0].outputLevel != 99u ||
            suspense.operators[3].outputLevel != 60u) {
            std::cerr << "[FAIL] Factory ROM [OP4, OP2, OP3, OP1] -> {OP1..OP4} operator unpacking check failed\n";
            return false;
        }

        // Verify OpzVoice asymmetric ALG 6 routing: OP4 (idx 3) is purely modulator; OP1 (idx 0) is modulated carrier
        {
            vf50::dsp::fm::OpzVoice algVoice;
            algVoice.reset();
            vf50::dsp::fm::VoiceNoteConfig vcfg{};
            vcfg.velocity = 120u;
            vcfg.partVolume = 99u;
            vcfg.outAssign = 3u;
            vf50::dsp::fm::QuickEditOffsets qe{};
            vf50::dsp::fm::LfoOutput zeroLfo{};
            vf50::dsp::fm::ChannelControllerState ctrl{};

            auto testPatch = suspense;
            for (auto& op : testPatch.operators) {
                op.ar = 31u;
                op.fixedFreqMode = 0u;
                op.coarse = 4u;
            }
            testPatch.operators[3].outputLevel = 92u;
            testPatch.operators[3].coarse = 8u;

            algVoice.startNote(testPatch, vcfg, qe, 1ULL);
            algVoice.setOperatorMask(0x08u); // OP4 only (modulator) -> must output 0.0
            float op4OnlyPeak = 0.0f;
            for (int i = 0; i < 64; ++i) {
                const auto s = algVoice.renderSample(zeroLfo, zeroLfo, zeroLfo, ctrl);
                op4OnlyPeak = std::max(op4OnlyPeak, std::abs(s.left));
            }

            algVoice.reset();
            algVoice.startNote(testPatch, vcfg, qe, 2ULL);
            algVoice.setOperatorMask(0x01u); // OP1 carrier unmodulated
            double unmodDiff = 0.0;
            vf50::dsp::fm::OpzVoice modVoice;
            modVoice.reset();
            modVoice.startNote(testPatch, vcfg, qe, 3ULL);
            modVoice.setOperatorMask(0x09u); // OP1 carrier + OP4 modulator
            for (int i = 0; i < 128; ++i) {
                const auto sUnmod = algVoice.renderSample(zeroLfo, zeroLfo, zeroLfo, ctrl);
                const auto sMod   = modVoice.renderSample(zeroLfo, zeroLfo, zeroLfo, ctrl);
                unmodDiff += std::abs(static_cast<double>(sMod.left - sUnmod.left));
            }
            if (op4OnlyPeak > 1e-7f || unmodDiff < 1e-2) {
                std::cerr << "[FAIL] Asymmetric ALG 6 modulator/carrier DSP routing verification failed\n";
                return false;
            }
        }

        for (const auto& fv : testRom().factoryVoices) {
            for (const auto& op : fv.operators) {
                if (op.detune < -3 || op.detune > 3 || op.waveform > 7u) {
                    std::cerr << "[FAIL] Out-of-range operator detune or waveform in factory ROM preset\n";
                    return false;
                }
            }
        }
    }

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 256);

    std::unique_ptr<juce::AudioProcessorEditor> baseEditor(proc->createEditor());
    auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEditor.get());
    if (editor == nullptr) {
        std::cerr << "[FAIL] Expected VF50AudioProcessorEditor instance\n";
        return false;
    }
    auto& canvas = editor->mainCanvas();

    // =========================================================================
    // 2. Task 4: Default 100% Scale = 1x Desktop (1482 x 440) & 200% = 2x Native
    // =========================================================================
    if (std::abs(editor->currentScaleFactor() - 1.00f) > 0.01f ||
        editor->getWidth() != 1482 || editor->getHeight() != 440) {
        std::cerr << "[FAIL] Default 100% UI scale expected 1482x440, got "
                  << editor->getWidth() << "x" << editor->getHeight() << "\n";
        return false;
    }
    editor->applyScaleFactor(2.00f);
    if (editor->getWidth() != 2964 || editor->getHeight() != 880) {
        std::cerr << "[FAIL] 200% UI scale expected 2964x880, got "
                  << editor->getWidth() << "x" << editor->getHeight() << "\n";
        return false;
    }
    editor->applyScaleFactor(1.00f);

    // =========================================================================
    // 3. Task 2: Rhythm Mode Keyboard & MIDI Triggering (31..91 G0..G5) + LCD
    // =========================================================================
    {
        for (std::size_t keyOff = 0; keyOff < 61u; ++keyOff) {
            const int8_t drumId = testRom().presetDrumKeyMaps[0][keyOff];
            if (drumId < 0 || drumId >= 61 ||
                testRom().defaultDrumSetup[static_cast<std::size_t>(drumId)].defaultMidiNote != static_cast<uint8_t>(31u + keyOff)) {
                std::cerr << "[FAIL] Default RKAT Preset 1 drum key assignment mismatch at key offset " << keyOff << "\n";
                return false;
            }
        }

        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::WorkstationMode,
                                      static_cast<float>(vf50::state::WorkstationMode::RhythmPlay));
        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::RhythmPlayEdit);

        juce::AudioBuffer<float> audioBuf(2, 512);
        juce::MidiBuffer midiBuf;

        // Strike MIDI note 31 (G0 = Drum 3 "H.BD" in RKAT Preset 1) on Channel 1 in Rhythm Mode (soft vs loud velocity)
        midiBuf.addEvent(juce::MidiMessage::noteOn(1, 31, static_cast<juce::uint8>(36)), 0);
        audioBuf.clear();
        proc->processBlock(audioBuf, midiBuf);
        const float softRms = audioBuf.getRMSLevel(0, 0, 512) + audioBuf.getRMSLevel(1, 0, 512);

        proc->synthEngine().allSoundOff();
        midiBuf.clear();
        midiBuf.addEvent(juce::MidiMessage::noteOn(1, 31, static_cast<juce::uint8>(124)), 0);
        audioBuf.clear();
        proc->processBlock(audioBuf, midiBuf);
        const float loudRms = audioBuf.getRMSLevel(0, 0, 512) + audioBuf.getRMSLevel(1, 0, 512);

        canvas.refreshDisplayAndLeds();
        if (loudRms <= softRms || loudRms < 1e-4f || proc->activeFmVoices() != 0 ||
            proc->lastTriggeredDrumNote() != 31u || proc->lastTriggeredDrumIndex() != 3) {
            std::cerr << "[FAIL] Rhythm mode G0(31) trigger/velocity/telemetry failed (loudRms=" << loudRms
                      << ", softRms=" << softRms << ", note=" << static_cast<int>(proc->lastTriggeredDrumNote())
                      << ", idx=" << proc->lastTriggeredDrumIndex() << ")\n";
            return false;
        }

        // Strike MIDI note 91 (G5 = Drum 36 "FMprc3" in RKAT Preset 1)
        midiBuf.clear();
        midiBuf.addEvent(juce::MidiMessage::noteOn(1, 91, static_cast<juce::uint8>(115)), 0);
        audioBuf.clear();
        proc->processBlock(audioBuf, midiBuf);
        canvas.refreshDisplayAndLeds();
        if (proc->lastTriggeredDrumNote() != 91u || proc->lastTriggeredDrumIndex() != 36) {
            std::cerr << "[FAIL] Rhythm mode G5(91) telemetry update failed (note="
                      << static_cast<int>(proc->lastTriggeredDrumNote())
                      << ", idx=" << proc->lastTriggeredDrumIndex() << ")\n";
            return false;
        }

        // Return to Single Play and verify incoming MIDI on Rhythm Receive Ch (Ch 10) still triggers drums!
        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::WorkstationMode,
                                      static_cast<float>(vf50::state::WorkstationMode::SinglePlay));
        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SinglePlay);
        proc->synthEngine().allSoundOff();

        midiBuf.clear();
        midiBuf.addEvent(juce::MidiMessage::noteOn(10, 47, static_cast<juce::uint8>(120)), 0); // B1 = Drum 6 "SD 1"
        audioBuf.clear();
        proc->processBlock(audioBuf, midiBuf);
        if (proc->lastTriggeredDrumNote() != 47u ||
            (audioBuf.getRMSLevel(0, 0, 512) + audioBuf.getRMSLevel(1, 0, 512)) < 1e-4f) {
            std::cerr << "[FAIL] Rhythm Receive Channel (Ch 10) drum triggering in Single Play failed\n";
            return false;
        }
        proc->synthEngine().allSoundOff();
        proc->keyboardState().reset();
    }

    // =========================================================================
    // 4. Task 3: Authentic ROM Demo Song Extraction & Playback via DEMO Button
    // =========================================================================
    {
        const std::size_t totalRomEvents = static_cast<std::size_t>(testRom().factoryDemoSongs[0].eventCount)
                                         + static_cast<std::size_t>(testRom().factoryDemoSongs[1].eventCount)
                                         + static_cast<std::size_t>(testRom().factoryDemoSongs[2].eventCount);
        if (vf50::generated::kDemoSongCount != 3u ||
            totalRomEvents < 9000u ||
            testRom().factoryDemoSongs[0].title != "Victory!" ||
            testRom().factoryDemoSongs[1].title != "Route 50" ||
            testRom().factoryDemoSongs[2].title != "MoonLite") {
            std::cerr << "[FAIL] Extracted ROM Demo Song table verification failed\n";
            return false;
        }

        canvas.pressDemoButton();
        canvas.pressSoftKey(0);        // >ROM Play
        canvas.pressKeypadYesButton(); // Confirm "Are you sure?"
        for (int t = 0; t < 35; ++t) {
            canvas.classicController().tick30Hz();
        }
        canvas.pressSoftKey(5);        // >START -> starts Song 1 ("Victory!")
        canvas.refreshDisplayAndLeds();
        const juce::String demoRow0 = canvas.lcdDisplay().getLineAsString(0);
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::SequencerDemo ||
            !proc->isDemoPlaying()) {
            std::cerr << "[FAIL] Classic DEMO button -> [+1/YES] did not start ROM Demo Song: '"
                      << demoRow0 << "' / '" << canvas.lcdDisplay().getLineAsString(1) << "'\n";
            return false;
        }

        // Render 2.67 seconds (ticks 0..256: 4-beat Claves count-in + Bar 2 FM fanfare) of ROM Demo Song #1 ("Victory!")
        juce::AudioBuffer<float> blockBuf(2, 256);
        juce::MidiBuffer emptyMidi;
        double demoEnergy = 0.0;
        uint64_t unionKeyMask = 0ULL;
        for (int b = 0; b < 500; ++b) {
            blockBuf.clear();
            emptyMidi.clear();
            proc->processBlock(blockBuf, emptyMidi);
            unionKeyMask |= proc->demoVisualKeyMask61();
            for (int s = 0; s < 256; ++s) {
                const float l = blockBuf.getSample(0, s);
                const float r = blockBuf.getSample(1, s);
                if (!std::isfinite(l) || !std::isfinite(r)) {
                    std::cerr << "[FAIL] Non-finite sample during ROM Demo Song playback\n";
                    return false;
                }
                demoEnergy += static_cast<double>(l * l + r * r);
            }
        }
        if (demoEnergy < 0.05 || unionKeyMask == 0ULL) {
            std::cerr << "[FAIL] ROM Demo Song 'Victory!' rendered silent audio or empty key mask (energy="
                      << demoEnergy << ")\n";
            return false;
        }

        // Stop Demo Song via [STOP] and exit Demo mode via [DEMO]
        canvas.pressSeqOpButton(2); // STOP
        canvas.pressDemoButton();
        blockBuf.clear();
        emptyMidi.clear();
        proc->processBlock(blockBuf, emptyMidi);
        canvas.refreshDisplayAndLeds();
        if (proc->isDemoPlaying()) {
            std::cerr << "[FAIL] DEMO/STOP button failed to stop ROM Demo Song cleanly\n";
            return false;
        }
    }

    // =========================================================================
    // 5. Task 5: FAT12 Floppy Disk (.IMG / .DSK), Folder & Direct VF50 File Manager
    // =========================================================================
    {
        buildDir.createDirectory();
        const juce::File testImg720  = buildDir.getChildFile("vf50_test_floppy_2dd.img");
        const juce::File testDsk1440 = buildDir.getChildFile("vf50_test_floppy_2hd.dsk");
        const juce::File testFolder  = buildDir.getChildFile("vf50_test_disk_dir");
        testFolder.createDirectory();

        // Format both 720KB 2DD and 1.44MB 2HD FAT12 floppy disk images
        const auto raw720  = vf50::disk::VF50DiskManager::createFormattedFat12Image(false, "VF50_2DD");
        const auto raw1440 = vf50::disk::VF50DiskManager::createFormattedFat12Image(true, "VF50_2HD");
        if (raw720.size() != 737280u || raw1440.size() != 1474560u) {
            std::cerr << "[FAIL] FAT12 floppy formatter size mismatch\n";
            return false;
        }
        (void)testImg720.replaceWithData(raw720.data(), raw720.size());
        (void)testDsk1440.replaceWithData(raw1440.data(), raw1440.size());

        // Customize User Voice I00, User Perf I00, and User Rhythm Kit in processor
        vf50::state::StoredVoiceSlot diskVoice(testRom().factoryVoices[12]);
        diskVoice.setName("FloppyVce1");
        proc->hostMemory().storeUserVoice(0u, diskVoice.voice);

        vf50::state::StoredPerformanceSlot diskPerf(testRom().factoryPerformances[5]);
        diskPerf.setName("FloppyPrf1");
        proc->hostMemory().storeUserPerformance(0u, diskPerf.perf);

        auto diskKit = proc->hostMemory().userRhythmKit(0u);
        diskKit.instruments[0].volume = 11u;
        diskKit.instruments[0].pan    = -2;
        proc->hostMemory().setUserRhythmKit(0u, diskKit);
        proc->pushHostMemoryToAudio();

        juce::AudioBuffer<float> syncBuf(2, 64);
        juce::MidiBuffer syncMidi;
        proc->processBlock(syncBuf, syncMidi);

        auto& dm = canvas.diskManager();
        juce::String status;
        if (!dm.mountFat12ImageFile(testImg720, status)) {
            std::cerr << "[FAIL] Failed to mount 720KB FAT12 .img: " << status << "\n";
            return false;
        }

        // Write .VOI, .PFM, .RHY, and .SYX into the mounted FAT12 floppy image + Save All via DISK menu!
        std::vector<uint8_t> voiBytes, pfmBytes, rhyBytes;
        vf50::disk::VF50DiskManager::buildVoiceBankBytes(proc->hostMemory(), voiBytes);
        vf50::disk::VF50DiskManager::buildPerfBankBytes(proc->hostMemory(), pfmBytes);
        vf50::disk::VF50DiskManager::buildRhythmDataBytes(proc->hostMemory(), rhyBytes);

        vf50::midi::VF50SysExHandler syxGen;
        vf50::midi::SysExReplyBuffer syxBuf;
        (void)syxGen.emitSingleVoiceEditDump(diskVoice.voice, syxBuf, true, true, true);

        if (!dm.writeFileToMountedFat12("BANK_A.VOI", voiBytes.data(), voiBytes.size(), status) ||
            !dm.writeFileToMountedFat12("PERF_A.PFM", pfmBytes.data(), pfmBytes.size(), status) ||
            !dm.writeFileToMountedFat12("DRUM_A.RHY", rhyBytes.data(), rhyBytes.size(), status) ||
            !dm.writeFileToMountedFat12("PATCH1.SYX", syxBuf.storage.data(), syxBuf.bytesUsed, status)) {
            std::cerr << "[FAIL] Failed writing VF50 files into FAT12 floppy image: " << status << "\n";
            return false;
        }

        // Press front-panel DISK button (opens authentic 40x2 Classic LCD UT DISK) menu) and save VF50USER.ALL into FAT12
        canvas.pressDiskButton();
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::UtilDisk ||
            !canvas.lcdDisplay().getLineAsString(0).startsWith("UT DISK)")) {
            std::cerr << "[FAIL] Front-panel DISK button did not open Classic 40x2 LCD UT DISK) menu\n";
            return false;
        }
        if (!dm.saveAllToMountedSource("VF50USER.ALL", *proc, status)) {
            std::cerr << "[FAIL] Failed saving VF50USER.ALL into FAT12 floppy image: " << status << "\n";
            return false;
        }

        // Remount the 720KB .img freshly from disk into a clean processor and verify round-trip loading!
        auto cleanProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        cleanProc->prepareToPlay(48000.0, 128);
        vf50::disk::VF50DiskManager verifyDm;
        if (!verifyDm.mountFat12ImageFile(testImg720, status) || verifyDm.entries().size() != 5u) {
            std::cerr << "[FAIL] Remounted FAT12 .img expected 5 VF50 files, got "
                      << verifyDm.entries().size() << " (" << status << ")\n";
            return false;
        }

        // Find VF50USER.ALL in FAT12 root directory and load All Data
        int allEntryIdx = -1;
        for (std::size_t i = 0; i < verifyDm.entries().size(); ++i) {
            if (verifyDm.entries()[i].name == "VF50USER.ALL") {
                allEntryIdx = static_cast<int>(i);
                break;
            }
        }
        if (allEntryIdx < 0 ||
            !verifyDm.loadMountedEntry(static_cast<std::size_t>(allEntryIdx),
                                       *cleanProc,
                                       vf50::disk::VF50LoadFilter::AllData,
                                       status)) {
            std::cerr << "[FAIL] Failed loading VF50USER.ALL from FAT12 floppy image: " << status << "\n";
            return false;
        }
        if (cleanProc->hostMemory().getUserVoice(0u).name != "FloppyVce1" ||
            cleanProc->hostMemory().getUserPerformance(0u).name != "FloppyPrf1" ||
            cleanProc->hostMemory().userRhythmKit(0u).instruments[0].volume != 11u ||
            cleanProc->hostMemory().userRhythmKit(0u).instruments[0].pan != -2) {
            std::cerr << "[FAIL] VF50USER.ALL FAT12 round-trip data mismatch\n";
            return false;
        }

        // Verify Local Directory mount + .SEQ (HardwareSeqV1) import
        const juce::File extVoi = testFolder.getChildFile("FOLDER.VOI");
        (void)extVoi.replaceWithData(voiBytes.data(), voiBytes.size());

        // Build a valid 34-byte header + track stream HardwareSeqV1 sequence file and import it
        std::vector<uint8_t> seqBytes = {
            0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u,
            0x00u, 0x03u, 0x00u, 125u,
            'T','E','S','T',' ','S','E','Q',
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
            1u, 2u, 99u, 0u, 0u, 0u,
            0xF0u, 0x00u, 0x90u, 60u, 108u, 0x30u, 0x80u, 60u, 0u, 0xF2u, 0xF9u
        };
        const juce::File extSeq = testFolder.getChildFile("SONG1.SEQ");
        (void)extSeq.replaceWithData(seqBytes.data(), seqBytes.size());

        if (!verifyDm.mountDirectory(testFolder, status) || verifyDm.entries().size() < 2u ||
            !verifyDm.importExternalFile(extSeq, *cleanProc, vf50::disk::VF50LoadFilter::SequenceOnly, status) ||
            !cleanProc->hasCustomDemoSequence()) {
            std::cerr << "[FAIL] Local directory mount or HardwareSeqV1 .SEQ import failed: " << status << "\n";
            return false;
        }

        // Also verify 1.44MB 2HD .dsk mounts cleanly
        if (!verifyDm.mountFat12ImageFile(testDsk1440, status)) {
            std::cerr << "[FAIL] Failed mounting 1.44MB 2HD .dsk image: " << status << "\n";
            return false;
        }

        // Test Universal SysEx (.syx) Single Voice & Bank Loading (optional external dir via VF50_TEST_SYX_DIR)
        const juce::File syxDir = findTestSyxDirectory();
        if (syxDir.isDirectory()) {
            const auto syxFiles = syxDir.findChildFiles(juce::File::findFiles, true, "*.syx;*.SYX");
            if (!syxFiles.isEmpty()) {
                juce::String loadStatus;
                if (!verifyDm.loadFile(syxFiles[0], *cleanProc, loadStatus)) {
                    std::cerr << "[FAIL] Failed loading external SysEx " << syxFiles[0].getFullPathName() << ": " << loadStatus << "\n";
                    return false;
                }
                std::cout << "[PASS] Successfully loaded external SysEx: " << syxFiles[0].getFileName() << "\n";
            }
        } else {
            std::cout << "[SKIP] External .syx fixture loading skipped (VF50_TEST_SYX_DIR unset).\n";
        }

        (void)testImg720.deleteFile();
        (void)testDsk1440.deleteFile();
        (void)testFolder.deleteRecursively();
    }

    // =========================================================================
    // 6. Task 6: Switchable Bottom 61-Key Virtual Keyboard & State Persistence
    // =========================================================================
    {
        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::WorkstationMode,
                                      static_cast<float>(vf50::state::WorkstationMode::SinglePlay));
        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SinglePlay);
        editor->applyScaleFactor(1.00f);

        // Toggle KEYBOARD switch ON -> window expands from 1482x440 to 1482x610 at 100% scale!
        canvas.pressKeyboardToggleButton();
        if (!editor->isVirtualKeyboardVisible() ||
            editor->getWidth() != 1482 || editor->getHeight() != 610) {
            std::cerr << "[FAIL] Virtual Keyboard toggle expected 1482x610 at 100% scale, got "
                      << editor->getWidth() << "x" << editor->getHeight() << "\n";
            return false;
        }

        // Capture full workstation + 61-key virtual keyboard PNG snapshot
        const juce::Image fullImg = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
        if (!fullImg.isValid() || fullImg.getWidth() != 1482 || fullImg.getHeight() != 610) {
            std::cerr << "[FAIL] Full workstation + 61-key virtual keyboard snapshot invalid\n";
            return false;
        }
        const juce::File stage5SnapPng = buildDir.getChildFile("vf50_stage5_full_workstation_1x.png");
        if (stage5SnapPng.existsAsFile()) {
            stage5SnapPng.deleteFile();
        }
        {
            juce::PNGImageFormat pngFmt;
            std::unique_ptr<juce::FileOutputStream> pngOut(stage5SnapPng.createOutputStream());
            if (pngOut && pngOut->openedOk()) {
                (void)pngFmt.writeImageToStream(fullImg, *pngOut);
            }
        }

        // Simulate mouse key-down on C3 (MIDI 60) on the Virtual Keyboard and verify live audio + key illumination
        auto& vkb = canvas.virtualKeyboard();
        vkb.simulateMouseKeyDown(60, 0.85f);
        if (!vkb.isKeyVisuallyLit(60)) {
            std::cerr << "[FAIL] Virtual Keyboard key 60 did not illuminate on mouse press\n";
            return false;
        }

        juce::AudioBuffer<float> kbAudio(2, 512);
        juce::MidiBuffer kbMidi;
        kbAudio.clear();
        proc->processBlock(kbAudio, kbMidi);
        const float kbRms = kbAudio.getRMSLevel(0, 0, 512) + kbAudio.getRMSLevel(1, 0, 512);
        vkb.simulateMouseKeyUp();
        kbMidi.clear();
        proc->processBlock(kbAudio, kbMidi);
        if (kbRms < 1e-4f) {
            std::cerr << "[FAIL] Virtual Keyboard mouse key-down did not trigger live synth audio\n";
            return false;
        }

        // Verify incoming external MIDI illuminates virtual keys in real time
        proc->keyboardState().noteOn(1, 67, 0.90f);
        if (!vkb.isKeyVisuallyLit(67)) {
            std::cerr << "[FAIL] External MIDI Note-On did not illuminate Virtual Keyboard key 67\n";
            return false;
        }
        proc->keyboardState().noteOff(1, 67, 0.0f);

        // Verify UI Scale (150%), Virtual Keyboard visibility (true) & LCD Theme (2) persist in DAW state!
        editor->applyScaleFactor(1.50f);
        canvas.lcdDisplay().setThemeIndex(2);

        juce::MemoryBlock stateWithUiPrefs;
        proc->getStateInformation(stateWithUiPrefs);

        auto proc2 = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc2->prepareToPlay(48000.0, 128);
        proc2->setStateInformation(stateWithUiPrefs.getData(), static_cast<int>(stateWithUiPrefs.getSize()));
        if (std::abs(proc2->uiScaleFactor() - 1.50f) > 0.01f ||
            !proc2->isVirtualKeyboardVisible() ||
            proc2->lcdThemeIndex() != 2) {
            std::cerr << "[FAIL] UI Scale / Virtual Keyboard / LCD Theme persistence round-trip failed\n";
            return false;
        }
    }

    // =========================================================================
    // 7. Authentic Classic [SETUP] Menu, [MEMORY PROTECT], & BL Badge Decoupling
    // =========================================================================
    {
        auto& lcd = canvas.lcdDisplay();
        lcd.setThemeIndex(0);
        proc->setGlobalTransposeSemitones(0);
        proc->setIntMemoryProtected(false);
        proc->setCardMemoryProtected(false);

        // 7a. Press [SETUP] -> Classic UT SETUP) root (no UI SCALE / THEME!)
        canvas.pressSetupButton();
        const juce::String p1Row0 = lcd.getLineAsString(0);
        const juce::String p1Row1 = lcd.getLineAsString(1);
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::UtilSetup ||
            !p1Row0.startsWith("UT  SETUP)") ||
            p1Row0.containsIgnoreCase("SCALE") || p1Row1.containsIgnoreCase("SCALE") ||
            p1Row0.containsIgnoreCase("THEME") || p1Row1.containsIgnoreCase("THEME")) {
            std::cerr << "[FAIL] Authentic Classic UT SETUP) root check failed:\n  Row0='"
                      << p1Row0 << "'\n  Row1='" << p1Row1 << "'\n";
            return false;
        }

        // Press [MEMORY PROTECT] -> Classic UT MEMORY PROTECT)
        canvas.pressMemoryProtectButton();
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::UtilMemProtect ||
            !lcd.getLineAsString(0).contains("MEMORY PROTECT")) {
            std::cerr << "[FAIL] Classic MEMORY PROTECT LCD check failed: '" << lcd.getLineAsString(0) << "'\n";
            return false;
        }

        // 7c. BL Badge Click & LCD Right-Click Theme Cycling without disturbing active menu screen
        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SinglePlay);
        const juce::String menuBefore0 = lcd.getLineAsString(0);
        const juce::String menuBefore1 = lcd.getLineAsString(1);
        lcd.setThemeIndex(0);
        lcd.handleThemeClickInteraction(lcd.blBadgeBounds().getCentre(), false); // Click BL badge -> Theme 1
        const int afterBlClick = lcd.themeIndex();
        lcd.handleThemeClickInteraction({ 220.0f, 60.0f }, true);                // Right-click LCD -> Theme 2
        const int afterRightClick = lcd.themeIndex();
        if (afterBlClick != 1 || afterRightClick != 2 ||
            canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::SinglePlay ||
            lcd.getLineAsString(0) != menuBefore0 ||
            lcd.getLineAsString(1) != menuBefore1) {
            std::cerr << "[FAIL] BL badge / LCD right-click theme cycling check failed\n";
            return false;
        }
        lcd.setThemeIndex(0);
    }

    std::cout << "[SelfTest:Stage5] All Stage 5 Hardware, ROM Demo, UI Scale, FAT12 Disk, Virtual Keyboard & Authentic Menu checks PASSED.\n";
    return true;
}

[[nodiscard]] bool VoicesMatchPristineFactory(const vf50::generated::FactoryVoice& actual,
                                              const vf50::generated::FactoryVoice& expected) noexcept {
    if (actual.rawRomBytes != expected.rawRomBytes) return false;
    if (actual.name != expected.name) return false;
    if (actual.algorithm != expected.algorithm || actual.feedback != expected.feedback) return false;
    if (actual.lfoSpeed != expected.lfoSpeed || actual.lfoDelay != expected.lfoDelay ||
        actual.pmd != expected.pmd || actual.amd != expected.amd ||
        actual.lfoSync != expected.lfoSync || actual.lfoWave != expected.lfoWave ||
        actual.pms != expected.pms || actual.ams != expected.ams) {
        return false;
    }
    if (actual.effect.effectType != expected.effect.effectType ||
        actual.effect.balance != expected.effect.balance ||
        actual.effect.outLevel != expected.effect.outLevel ||
        actual.effect.stereoMix != expected.effect.stereoMix ||
        actual.effect.param1 != expected.effect.param1 ||
        actual.effect.param2 != expected.effect.param2 ||
        actual.effect.param3 != expected.effect.param3) {
        return false;
    }
    for (std::size_t op = 0; op < 4u; ++op) {
        const auto& a = actual.operators[op];
        const auto& e = expected.operators[op];
        if (a.ar != e.ar || a.d1r != e.d1r || a.d2r != e.d2r || a.rr != e.rr || a.d1l != e.d1l ||
            a.levelScaling != e.levelScaling || a.rateScaling != e.rateScaling ||
            a.egBiasSens != e.egBiasSens || a.ampModEnable != e.ampModEnable ||
            a.keyVelSens != e.keyVelSens || a.outputLevel != e.outputLevel ||
            a.coarse != e.coarse || a.fine != e.fine || a.detune != e.detune ||
            a.waveform != e.waveform || a.egShift != e.egShift ||
            a.fixedFreqMode != e.fixedFreqMode || a.fixedRange != e.fixedRange ||
            a.fixedRangeLow != e.fixedRangeLow) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool runPresetCyclingVerificationTest() {
    using vf50::params::ParamIndex;

    constexpr double kSampleRate  = 48000.0;
    constexpr int    kBlockSize   = 128;
    constexpr int    kNoteBlocks  = 16; // 2,048 samples (~42.7 ms) per preset note
    constexpr int    kReleaseBlk  = 10; // NoteOff at block 10 -> active release tail at patch switch!
    constexpr int    kTotalFrames = kBlockSize * kNoteBlocks;

    // -------------------------------------------------------------------------
    // 1. Fresh Cold-Boot Reference Render of Preset 05
    // -------------------------------------------------------------------------
    std::vector<float> coldL(static_cast<std::size_t>(kTotalFrames), 0.0f);
    std::vector<float> coldR(static_cast<std::size_t>(kTotalFrames), 0.0f);
    {
        auto coldProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        coldProc->selectSinglePresetAtomic(vf50::state::MemoryBank::FactoryRom, 5u);
        coldProc->prepareToPlay(kSampleRate, kBlockSize);

        juce::AudioBuffer<float> blockBuf(2, kBlockSize);
        juce::MidiBuffer midi;

        for (int b = 0; b < kNoteBlocks; ++b) {
            blockBuf.clear();
            midi.clear();
            if (b == 0) {
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(108)), 0);
            } else if (b == kReleaseBlk) {
                midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            }
            coldProc->processBlock(blockBuf, midi);

            const float* rdL = blockBuf.getReadPointer(0);
            const float* rdR = blockBuf.getReadPointer(1);
            const std::size_t base = static_cast<std::size_t>(b * kBlockSize);
            std::copy_n(rdL, kBlockSize, coldL.data() + base);
            std::copy_n(rdR, kBlockSize, coldR.data() + base);
        }
    }

    double coldEnergy = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTotalFrames); ++i) {
        if (!std::isfinite(coldL[i]) || !std::isfinite(coldR[i])) {
            std::cerr << "[FAIL] Non-finite sample in cold-boot Preset 05 reference\n";
            return false;
        }
        coldEnergy += static_cast<double>(coldL[i]) * static_cast<double>(coldL[i])
                    + static_cast<double>(coldR[i]) * static_cast<double>(coldR[i]);
    }
    const double coldRms = std::sqrt(coldEnergy / static_cast<double>(kTotalFrames * 2));
    if (coldRms < 1e-3) {
        std::cerr << "[FAIL] Cold-boot Preset 05 reference is unexpectedly silent (RMS=" << coldRms << ")\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 2. Rapid +1 Preset Cycling (Preset 00 -> Preset 01 -> ... -> Preset 09) via UI +1 Button
    // -------------------------------------------------------------------------
    std::vector<float> steppedL(static_cast<std::size_t>(kTotalFrames), 0.0f);
    std::vector<float> steppedR(static_cast<std::size_t>(kTotalFrames), 0.0f);
    {
        auto steppedProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        steppedProc->prepareToPlay(kSampleRate, kBlockSize);

        std::unique_ptr<juce::AudioProcessorEditor> baseEd(steppedProc->createEditor());
        auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
        if (editor == nullptr) {
            std::cerr << "[FAIL] Could not create VF50AudioProcessorEditor for preset cycling test\n";
            return false;
        }
        auto& canvas = editor->mainCanvas();
        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SinglePlay);

        // Deliberately dirty Quick Edit offsets and edit-buffer parameters on Preset 00 before cycling!
        vf50::params::setApvtsRawValue(steppedProc->apvts(), ParamIndex::QuickAttack, 45.0f);
        vf50::params::setApvtsRawValue(steppedProc->apvts(), ParamIndex::QuickRelease, -35.0f);
        vf50::params::setApvtsRawValue(steppedProc->apvts(), ParamIndex::QuickBrilliance, 65.0f);
        vf50::params::setApvtsRawValue(steppedProc->apvts(), ParamIndex::QuickTotalLevel, -20.0f);
        vf50::params::setApvtsRawValue(steppedProc->apvts(), ParamIndex::VoiceFeedback, 7.0f);
        vf50::params::setApvtsRawValue(steppedProc->apvts(), ParamIndex::Op2Enable, 0.0f);

        juce::AudioBuffer<float> blockBuf(2, kBlockSize);
        juce::MidiBuffer midi;

        for (std::size_t presetIdx = 0u; presetIdx < 10u; ++presetIdx) {
            if (presetIdx > 0u) {
                // Step +1 via front-panel +1/YES action while previous preset's voice/feedback/reverb tail is live!
                canvas.stepFocusedField(+1);

                const auto& pBank = steppedProc->parameterBank();
                if (static_cast<int>(std::lround(pBank.voicePreset->load())) != static_cast<int>(presetIdx) ||
                    static_cast<int>(std::lround(pBank.quickAttack->load())) != 0 ||
                    static_cast<int>(std::lround(pBank.quickRelease->load())) != 0 ||
                    static_cast<int>(std::lround(pBank.quickBrilliance->load())) != 0 ||
                    static_cast<int>(std::lround(pBank.quickTotalLevel->load())) != 0) {
                    std::cerr << "[FAIL] Quick Edit offsets or selector did not reset on +1 step to Preset "
                              << presetIdx << "\n";
                    return false;
                }

                if (!VoicesMatchPristineFactory(steppedProc->hostMemory().activeSingleVoice(),
                                                testRom().factoryVoices[presetIdx])) {
                    std::cerr << "[FAIL] Host edit buffer VMEM mismatch after +1 step to Preset "
                              << presetIdx << "\n";
                    return false;
                }
            }

            const uint8_t testNote = (presetIdx == 5u)
                                         ? static_cast<uint8_t>(60u)
                                         : static_cast<uint8_t>(52u + presetIdx * 2u);

            for (int b = 0; b < kNoteBlocks; ++b) {
                blockBuf.clear();
                midi.clear();
                if (b == 0) {
                    midi.addEvent(juce::MidiMessage::noteOn(1, testNote, static_cast<juce::uint8>(108)), 0);
                } else if (b == kReleaseBlk) {
                    midi.addEvent(juce::MidiMessage::noteOff(1, testNote), 0);
                }
                steppedProc->processBlock(blockBuf, midi);

                if (presetIdx > 0u && b == 0) {
                    const auto& dspVoice = steppedProc->synthEngine().fm().activeSingleVoice();
                    const auto& dspQe    = steppedProc->synthEngine().fm().quickEditOffsets();
                    if (!VoicesMatchPristineFactory(dspVoice, testRom().factoryVoices[presetIdx]) ||
                        dspQe.attackOffset != 0 || dspQe.releaseOffset != 0 ||
                        dspQe.brillianceOffset != 0 || dspQe.volumeOffset != 0 ||
                        steppedProc->synthEngine().fm().operatorEnableMask() != 0x0Fu) {
                        std::cerr << "[FAIL] Audio-thread DSP voice/QE bleed detected on Preset "
                                  << presetIdx << "\n";
                        return false;
                    }
                }

                const float* rdL = blockBuf.getReadPointer(0);
                const float* rdR = blockBuf.getReadPointer(1);
                for (int s = 0; s < kBlockSize; ++s) {
                    if (!std::isfinite(rdL[s]) || !std::isfinite(rdR[s])) {
                        std::cerr << "[FAIL] Non-finite sample during stepped Preset " << presetIdx << "\n";
                        return false;
                    }
                }

                if (presetIdx == 5u) {
                    const std::size_t base = static_cast<std::size_t>(b * kBlockSize);
                    std::copy_n(rdL, kBlockSize, steppedL.data() + base);
                    std::copy_n(rdR, kBlockSize, steppedR.data() + base);
                }
            }
        }
    }

    float maxDiffUiStep = 0.0f;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTotalFrames); ++i) {
        maxDiffUiStep = std::max(maxDiffUiStep, std::abs(steppedL[i] - coldL[i]));
        maxDiffUiStep = std::max(maxDiffUiStep, std::abs(steppedR[i] - coldR[i]));
    }
    if (maxDiffUiStep > 1e-6f) {
        std::cerr << "[FAIL] Stepped Preset 05 waveform diverged from cold-boot Preset 05! maxAbsDiff="
                  << maxDiffUiStep << "\n";
        return false;
    }

    // -------------------------------------------------------------------------
    // 3. Direct Headless APVTS +1 Stepping Verification (Preset 00 -> ... -> Preset 05)
    // -------------------------------------------------------------------------
    {
        auto apvtsProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        apvtsProc->prepareToPlay(kSampleRate, kBlockSize);
        vf50::params::setApvtsRawValue(apvtsProc->apvts(), ParamIndex::QuickBrilliance, 80.0f);

        juce::AudioBuffer<float> blockBuf(2, kBlockSize);
        juce::MidiBuffer midi;
        float maxDiffApvts = 0.0f;

        for (std::size_t presetIdx = 0u; presetIdx <= 5u; ++presetIdx) {
            if (presetIdx > 0u) {
                vf50::params::setApvtsRawValue(apvtsProc->apvts(), ParamIndex::VoicePreset,
                                              static_cast<float>(presetIdx));
            }
            const uint8_t note = (presetIdx == 5u) ? 60u : static_cast<uint8_t>(48u + presetIdx * 3u);
            for (int b = 0; b < kNoteBlocks; ++b) {
                blockBuf.clear();
                midi.clear();
                if (b == 0) {
                    midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(108)), 0);
                } else if (b == kReleaseBlk) {
                    midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
                }
                apvtsProc->processBlock(blockBuf, midi);

                if (presetIdx == 5u) {
                    const float* rdL = blockBuf.getReadPointer(0);
                    const float* rdR = blockBuf.getReadPointer(1);
                    const std::size_t base = static_cast<std::size_t>(b * kBlockSize);
                    for (int s = 0; s < kBlockSize; ++s) {
                        maxDiffApvts = std::max(maxDiffApvts, std::abs(rdL[s] - coldL[base + static_cast<std::size_t>(s)]));
                        maxDiffApvts = std::max(maxDiffApvts, std::abs(rdR[s] - coldR[base + static_cast<std::size_t>(s)]));
                    }
                }
            }
        }

        if (maxDiffApvts > 1e-6f) {
            std::cerr << "[FAIL] APVTS-stepped Preset 05 waveform diverged from cold-boot Preset 05! maxAbsDiff="
                      << maxDiffApvts << "\n";
            return false;
        }

        std::cout << "[SelfTest:PresetCycling] 10-Preset +1 Cycling (P00->P09) PASSED "
                  << "(Preset 05 RMS=" << coldRms
                  << ", UI +1 maxAbsDiff=" << maxDiffUiStep
                  << ", APVTS +1 maxAbsDiff=" << maxDiffApvts << ").\n";
    }

    return true;
}

bool runAuditSynthesisVerificationTest() {
    using namespace vf50::dsp::fm;

    // 1. Verify OPZ II Ratio coarse=0..3 (mult==0) table matches hardware manual ratios
    {
        const double r00 = computeOpzRatioMultiplier(0u, 0u);
        const double r04 = computeOpzRatioMultiplier(0u, 4u);
        const double r36 = computeOpzRatioMultiplier(3u, 6u);
        if (std::abs(r00 - 0.50) > 1e-4 || std::abs(r04 - 0.75) > 1e-4 || std::abs(r36 - 1.5147) > 1e-3) {
            std::cerr << "[FAIL] Ratio mult==0 calibration mismatch: r00=" << r00
                      << " r04=" << r04 << " r36=" << r36 << "\n";
            return false;
        }
    }

    // 2. Verify LFO PMD exponential taper, PMD=0 controller fallback (rawBipolar), and Delay hold+ramp
    {
        OpzLfo lfo;
        lfo.reset();
        const auto& p01 = testRom().factoryVoices[1];
        lfo.configure(p01.lfoSpeed, p01.lfoWave, p01.pmd, p01.amd, p01.lfoSync);
        float maxP01Pitch = 0.0f;
        for (int s = 0; s < 50000; ++s) {
            maxP01Pitch = std::max(maxP01Pitch, std::abs(lfo.tick().pitchBipolar));
        }
        const double maxP01Cents = static_cast<double>(maxP01Pitch) * pmsToMaxCents(p01.pms) * 0.5;
        if (maxP01Cents >= 10.0 || maxP01Cents <= 0.1) {
            std::cerr << "[FAIL] P01 LFO pitch warble out of musical bounds: " << maxP01Cents << " cents\n";
            return false;
        }

        // PMD=0 (Preset 09 SynBass 1) keeps pitchBipolar=0 while rawBipolar remains active for MW
        const auto& p09 = testRom().factoryVoices[9];
        lfo.configure(p09.lfoSpeed, p09.lfoWave, p09.pmd, p09.amd, p09.lfoSync);
        float maxRaw = 0.0f;
        for (int s = 0; s < 50000; ++s) {
            const LfoOutput out = lfo.tick();
            if (out.pitchBipolar != 0.0f) {
                std::cerr << "[FAIL] PMD=0 emitted non-zero pitchBipolar!\n";
                return false;
            }
            maxRaw = std::max(maxRaw, std::abs(out.rawBipolar));
        }
        if (maxRaw < 0.9f) {
            std::cerr << "[FAIL] LFO rawBipolar failed to oscillate when PMD=0! maxRaw=" << maxRaw << "\n";
            return false;
        }

        // LFO Delay=17 must hold near zero before fading up
        VoiceLfoDelayRamp delayRamp;
        delayRamp.trigger(17u);
        if (delayRamp.tick() != 0.0f) {
            std::cerr << "[FAIL] VoiceLfoDelayRamp did not hold at 0.0 on attack!\n";
            return false;
        }
        for (int s = 0; s < 40000; ++s) {
            (void)delayRamp.tick();
        }
        if (delayRamp.tick() < 0.99f) {
            std::cerr << "[FAIL] VoiceLfoDelayRamp failed to finish fade-in within 0.8s!\n";
            return false;
        }
    }

    // 3. Verify 1:1 PEG semitone table, symmetric +-2.5c C3 detune, Fixed Freq & live render of P45/P95/P97
    {
        const auto& tables = getOpzTables();
        if (std::abs(tables.pegSemi[47] - (-3.0)) > 1e-6 ||
            std::abs(tables.pegSemi[2] - (-48.0)) > 1e-6 ||
            std::abs(tables.pegSemi[98] - 48.0) > 1e-6) {
            std::cerr << "[FAIL] PEG semitone table mismatch: pl47=" << tables.pegSemi[47] << "\n";
            return false;
        }

        const double detPlus  = computeOpzDetuneHz(60.0, 3);
        const double detMinus = computeOpzDetuneHz(60.0, -3);
        const double detCents = 1200.0 * std::log2((261.625565 + detPlus) / 261.625565);
        if (std::abs(detPlus + detMinus) > 1e-9 || detCents < 2.2 || detCents > 2.9) {
            std::cerr << "[FAIL] C3 Detune (+-3) mismatch: cents=" << detCents << "\n";
            return false;
        }

        const double fxHi = computeOpzFixedFreqHz(60u, 6u, 3u, 0u);
        const double fxZero = computeOpzFixedFreqHz(0u, 0u, 0u, 1u);
        if (fxHi < 200.0 || fxZero != 0.0) {
            std::cerr << "[FAIL] Fixed frequency evaluation error: fxHi=" << fxHi << " fxZero=" << fxZero << "\n";
            return false;
        }

        // Render P45 (PEGvoice), P95 (Strike 1), P97 (Space), P03 (Piano) cleanly
        for (const std::size_t idx : { 3u, 45u, 95u, 97u }) {
            auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
            proc->prepareToPlay(44100.0, 256);
            vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::VoicePreset, static_cast<float>(idx));
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            double sumSq = 0.0;
            for (int b = 0; b < 40; ++b) {
                buf.clear();
                midi.clear();
                if (b == 0) {
                    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
                } else if (b == 30) {
                    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                }
                proc->processBlock(buf, midi);
                const float* ch0 = buf.getReadPointer(0);
                for (int s = 0; s < 256; ++s) {
                    if (!std::isfinite(ch0[s])) {
                        std::cerr << "[FAIL] Non-finite sample in preset P" << idx << "\n";
                        return false;
                    }
                    sumSq += static_cast<double>(ch0[s]) * static_cast<double>(ch0[s]);
                }
            }
            const double rms = std::sqrt(sumSq / static_cast<double>(40 * 256));
            if (rms < 1e-4) {
                std::cerr << "[FAIL] Preset P" << idx << " rendered nearly silent! RMS=" << rms << "\n";
                return false;
            }
        }
    }

    std::cout << "[SelfTest:AuditSynthesis] Ratio/LFO/PEG/FixedFreq/Detune & P03/P45/P95/P97 PASSED.\n";
    return true;
}

bool runUniversalSysExAndDiskIngestionSelfTests(const juce::File& buildDir) {
    std::cout << "[SelfTest:UniversalSysEx] Verifying case-insensitive filters, VCED/VMEM32/VF50-100/Concatenated SysEx, "
              << "lenient checksums, drag-and-drop cyan overlay + LCD toast, and 720KB FAT12 floppy navigation...\n";

    // 1. Universal File Filter & Case-Insensitive Local Directory Scanner
    const juce::String filterStr(vf50::disk::VF50DiskManager::kUniversalFileFilter);
    for (const char* pat : { "*.syx", "*.SYX", "*.mid", "*.MID", "*.img", "*.IMG", "*.dsk", "*.DSK",
                             "*.all", "*.ALL", "*.voi", "*.VOI", "*.pfm", "*.PFM" }) {
        if (!filterStr.contains(pat)) {
            std::cerr << "[FAIL] kUniversalFileFilter missing pattern: " << pat << "\n";
            return false;
        }
    }

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 256);
    std::unique_ptr<juce::AudioProcessorEditor> rawEd(proc->createEditor());
    auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(rawEd.get());
    if (editor == nullptr) {
        std::cerr << "[FAIL] Failed to create VF50AudioProcessorEditor for UniversalSysEx self-test\n";
        return false;
    }
    auto& canvas = editor->mainCanvas();
    auto& lcd    = canvas.lcdDisplay();
    auto& dm     = canvas.diskManager();

    const juce::File tmpDir = buildDir.getChildFile("vf50_univ_sysex_tmp");
    (void)tmpDir.deleteRecursively();
    (void)tmpDir.createDirectory();

    auto writeSyntheticVcedSyx = [](const juce::File& dst,
                                    const vf50::generated::FactoryVoice& voice) -> bool {
        vf50::midi::VF50SysExHandler tx;
        vf50::midi::SysExReplyBuffer buf;
        if (!tx.emitSingleVoiceEditDump(voice, buf, true, true, true) || buf.bytesUsed == 0u) {
            return false;
        }
        return dst.replaceWithData(buf.storage.data(), buf.bytesUsed);
    };

    vf50::state::StoredVoiceSlot vStrings(testRom().factoryVoices[0]);
    vStrings.setName("Strings 1 ");
    vStrings.voice.algorithm = 4u;
    vStrings.voice.feedback  = 7u;
    vStrings.voice.transpose = -19;
    vStrings.voice.operators[0].coarse      = 10u;
    vStrings.voice.operators[0].ar          = 23u;
    vStrings.voice.operators[0].d1r         = 31u;
    vStrings.voice.operators[0].d1l         = 15u;
    vStrings.voice.operators[0].d2r         = 0u;
    vStrings.voice.operators[0].rr          = 9u;
    vStrings.voice.operators[0].outputLevel = 95u;
    vStrings.voice.operators[3].coarse      = 10u;
    vStrings.voice.operators[3].outputLevel = 74u;
    vStrings.repackRomBytes();

    vf50::state::StoredVoiceSlot vBrass(testRom().factoryVoices[1]);
    vBrass.setName("PowerBrass");
    vBrass.voice.algorithm = 4u;
    vBrass.voice.feedback  = 5u;
    vBrass.voice.transpose = 0;
    vBrass.voice.operators[0].coarse       = 4u;
    vBrass.voice.operators[0].ar           = 24u;
    vBrass.voice.operators[1].levelScaling = -12;
    vBrass.voice.operators[2].levelScaling = 18;
    vBrass.voice.operators[2].outputLevel  = 99u;
    vBrass.voice.operators[3].outputLevel  = 92u;
    vBrass.repackRomBytes();

    vf50::state::StoredVoiceSlot vPiano(testRom().factoryVoices[2]);
    vPiano.setName("Piano     ");
    vPiano.voice.algorithm = 2u;
    vPiano.voice.operators[0].d2r         = 6u;
    vPiano.voice.operators[3].ar          = 31u;
    vPiano.voice.operators[3].coarse      = 5u;
    vPiano.voice.operators[3].outputLevel = 92u;
    vPiano.repackRomBytes();

    const juce::File syxStrings = tmpDir.getChildFile("00-Strings_1_.syx");
    const juce::File syxBrass   = tmpDir.getChildFile("01-PowerBrass.syx");
    const juce::File syxPiano   = tmpDir.getChildFile("03-Piano_____.syx");
    if (!writeSyntheticVcedSyx(syxStrings, vStrings.voice) ||
        !writeSyntheticVcedSyx(syxBrass, vBrass.voice) ||
        !writeSyntheticVcedSyx(syxPiano, vPiano.voice)) {
        std::cerr << "[FAIL] Could not write synthetic VCED .syx fixtures\n";
        return false;
    }

    // 2. Load synthetic .syx files and assert exact parameters + live audio
    {
        juce::String status;
        if (!dm.loadFile(syxStrings, *proc, status)) {
            std::cerr << "[FAIL] loadFile failed on 00-Strings_1_.syx: " << status << "\n";
            return false;
        }
        const auto& v = proc->hostMemory().activeSingleVoice();
        if (v.name != "Strings 1 " || v.algorithm != 4u || v.feedback != 7u || v.transpose != -19 ||
            v.operators[0].coarse != 10u || v.operators[0].ar != 23u || v.operators[0].d1r != 31u ||
            v.operators[0].d1l != 15u || v.operators[0].d2r != 0u || v.operators[0].rr != 9u ||
            v.operators[0].outputLevel != 95u || v.operators[3].coarse != 10u || v.operators[3].outputLevel != 74u) {
            std::cerr << "[FAIL] 00-Strings_1_.syx voice parameter mismatch\n";
            return false;
        }
    }

    // 3. Drag-and-Drop File Import (PluginEditor): cyan border overlay + atomic swap + LCD confirmation
    {
        juce::StringArray dragCheck;
        dragCheck.add(syxBrass.getFullPathName());
        if (!editor->isInterestedInFileDrag(dragCheck)) {
            std::cerr << "[FAIL] Editor rejected .syx drag!\n";
            return false;
        }
        const auto sysTmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
        juce::StringArray upperDragCheck;
        upperDragCheck.add(sysTmpDir.getChildFile("TEST_BANK.SYX").getFullPathName());
        if (!editor->isInterestedInFileDrag(upperDragCheck)) {
            std::cerr << "[FAIL] Editor rejected uppercase .SYX drag!\n";
            return false;
        }

        const juce::Image idleSnap = editor->createComponentSnapshot(juce::Rectangle<int>(0, 0, 32, 32), true, 1.0f);
        editor->fileDragEnter(dragCheck, 40, 40);
        if (!editor->isDragHighlightActive()) {
            std::cerr << "[FAIL] fileDragEnter did not activate cyan border highlight\n";
            return false;
        }
        const juce::Image dragSnap = editor->createComponentSnapshot(juce::Rectangle<int>(0, 0, 32, 32), true, 1.0f);
        if (dragSnap.getPixelAt(2, 2).getARGB() == idleSnap.getPixelAt(2, 2).getARGB()) {
            std::cerr << "[FAIL] Cyan drag-and-drop overlay did not render over child canvas\n";
            return false;
        }

        editor->filesDropped(dragCheck, 40, 40);
        if (editor->isDragHighlightActive()) {
            std::cerr << "[FAIL] filesDropped did not clear drag highlight\n";
            return false;
        }

        const auto& loadedBrass = proc->hostMemory().activeSingleVoice();
        if (loadedBrass.name != "PowerBrass" || loadedBrass.algorithm != 4u || loadedBrass.feedback == 0u ||
            loadedBrass.transpose != 0 || loadedBrass.operators[0].coarse != 4u || loadedBrass.operators[2].outputLevel != 99u ||
            loadedBrass.operators[1].levelScaling >= 0 || loadedBrass.operators[2].levelScaling <= 0 ||
            loadedBrass.operators[0].ar == 0u) {
            std::cerr << "[FAIL] Dropped 01-PowerBrass.syx voice parameter mismatch\n";
            return false;
        }

        const juce::String row0 = lcd.getLineAsString(0);
        const juce::String row1 = lcd.getLineAsString(1);
        if (!row0.startsWith("PLAY SINGLE") || !row1.contains("PowerBrass")) {
            std::cerr << "[FAIL] Classic 40x2 LCD did not show PowerBrass!\n  Row0='"
                      << row0 << "'\n  Row1='" << row1 << "'\n";
            return false;
        }

        // Render audio through processBlock to confirm atomic swap to audio thread + finite non-zero sound
        juce::AudioBuffer<float> audioBuf(2, 256);
        juce::MidiBuffer midiBuf;
        double sumSq = 0.0;
        for (int b = 0; b < 16; ++b) {
            audioBuf.clear();
            midiBuf.clear();
            if (b == 0) {
                midiBuf.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
            } else if (b == 12) {
                midiBuf.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            }
            proc->processBlock(audioBuf, midiBuf);
            for (int s = 0; s < 256; ++s) {
                const float l = audioBuf.getSample(0, s);
                const float r = audioBuf.getSample(1, s);
                if (!std::isfinite(l) || !std::isfinite(r)) {
                    std::cerr << "[FAIL] Non-finite sample after dropping 01-PowerBrass.syx\n";
                    return false;
                }
                sumSq += static_cast<double>(l * l + r * r);
            }
        }
        if (proc->synthEngine().fm().activeSingleVoice().name != "PowerBrass" || sumSq < 1e-4) {
            std::cerr << "[FAIL] Audio thread did not receive atomic swap for 01-PowerBrass.syx (sumSq="
                      << sumSq << ")\n";
            return false;
        }
    }

    // Also verify 03-Piano_____.syx
    {
        juce::String status;
        if (!dm.loadFile(syxPiano, *proc, status)) {
            std::cerr << "[FAIL] loadFile failed on 03-Piano_____.syx: " << status << "\n";
            return false;
        }
        const auto& loadedPiano = proc->hostMemory().activeSingleVoice();
        if (loadedPiano.name != "Piano     " || loadedPiano.algorithm != 2u || loadedPiano.operators[3].ar != 31u ||
            loadedPiano.operators[0].d2r == 0u || loadedPiano.operators[3].coarse != 5u) {
            std::cerr << "[FAIL] 03-Piano_____.syx voice parameter mismatch\n";
            return false;
        }
    }

    // 4. Channel Masking (MIDI Ch 12 = 0x0B) + Lenient Checksum on Single Voice VCED (101B) & 32-Voice VMEM (4104B)
    {
        vf50::state::StoredVoiceSlot customVced(testRom().factoryVoices[8]);
        customVced.setName("BadCsumVce");
        vf50::midi::VF50SysExHandler gen;
        vf50::midi::SysExReplyBuffer reply;
        (void)gen.emitSingleVoiceEditDump(customVced.voice, reply, false, false, false);
        std::vector<uint8_t> vcedBytes;
        reply.copyConcatenatedTo(vcedBytes);
        vcedBytes[2] = 0x0Bu; // MIDI Channel 12 (0-indexed 11)
        vcedBytes[vcedBytes.size() - 2u] ^= 0x55u; // Intentionally corrupt 7-bit 7-bit SysEx checksum!

        const juce::File badCsumVcedFile = tmpDir.getChildFile("SINGLE_CH12_BADCSUM.SYX");
        (void)badCsumVcedFile.replaceWithData(vcedBytes.data(), vcedBytes.size());

        juce::String status;
        if (!dm.loadFile(badCsumVcedFile, *proc, status) ||
            proc->hostMemory().activeSingleVoice().name != "BadCsumVce" ||
            proc->hostMemory().activeSingleVoice().algorithm != customVced.voice.algorithm) {
            std::cerr << "[FAIL] Single VCED on Ch 12 with bad checksum was rejected or mismatched\n";
            return false;
        }

        // 32-Voice VMEM (F0 43 05 04 20 00 ... 4096B) on MIDI Ch 6 (0x05) with bad checksum
        auto srcMem32 = std::make_unique<vf50::state::VF50MemoryManager>();
        srcMem32->setRuntimeRomBank(&testRom(), true);
        for (std::size_t i = 0; i < 32u; ++i) {
            vf50::state::StoredVoiceSlot slot32(testRom().factoryVoices[(i * 3u) % 100u]);
            char nm[11]{};
            std::snprintf(nm, sizeof(nm), "Bank32_%02u ", static_cast<unsigned>(i + 1u));
            slot32.setName(nm);
            srcMem32->storeUserVoice(i, slot32.voice);
        }
        reply.clear();
        (void)gen.emitSingleVoiceBulkDump32(*srcMem32, vf50::state::MemoryBank::UserRam, 0u, reply);
        std::vector<uint8_t> vmem32Bytes;
        reply.copyConcatenatedTo(vmem32Bytes);
        vmem32Bytes[2] = 0x05u; // MIDI Channel 6
        vmem32Bytes[vmem32Bytes.size() - 2u] ^= 0x3Fu; // Corrupt checksum!

        const juce::File badCsumBankFile = tmpDir.getChildFile("vmem32_ch6_badcsum.syx");
        (void)badCsumBankFile.replaceWithData(vmem32Bytes.data(), vmem32Bytes.size());

        if (!dm.loadFile(badCsumBankFile, *proc, status) ||
            proc->hostMemory().getUserVoice(0u).name != "Bank32_01 " ||
            proc->hostMemory().getUserVoice(31u).name != "Bank32_32 " ||
            proc->hostMemory().selectedVoiceBank() != vf50::state::MemoryBank::UserRam ||
            proc->hostMemory().selectedVoiceIndex() != 0u) {
            std::cerr << "[FAIL] 32-Voice VMEM bank dump on Ch 6 with bad checksum failed\n";
            return false;
        }
    }

    // 5. Native VF50 100-Voice Bank Dump (Model ID 0x24) -> unpacks all 100 voices into internal memory
    {
        std::vector<uint8_t> vf50Model24;
        vf50Model24.reserve(6u + 100u * 95u + 2u);
        vf50Model24.push_back(0xF0u);
        vf50Model24.push_back(0x43u);
        vf50Model24.push_back(0x09u); // MIDI Channel 10
        vf50Model24.push_back(0x24u); // Native VF50 Model ID 0x24
        const std::size_t payloadBytes = 100u * 95u;
        vf50Model24.push_back(static_cast<uint8_t>((payloadBytes >> 7u) & 0x7Fu));
        vf50Model24.push_back(static_cast<uint8_t>(payloadBytes & 0x7Fu));

        vf50::state::StoredVoiceSlot s{};
        for (std::size_t i = 0; i < 100u; ++i) {
            s.setFromVoice(testRom().factoryVoices[99u - i]);
            char nm[11]{};
            std::snprintf(nm, sizeof(nm), "VF5024_%03u", static_cast<unsigned>(i));
            s.setName(nm);
            vf50Model24.insert(vf50Model24.end(), s.voice.rawRomBytes.begin(), s.voice.rawRomBytes.end());
        }
        vf50Model24.push_back(vf50::midi::VF50SysExHandler::computeSysExChecksum(&vf50Model24[6], payloadBytes));
        vf50Model24.push_back(0xF7u);

        const juce::File model24File = tmpDir.getChildFile("NATIVE_VF50_100.SYX");
        (void)model24File.replaceWithData(vf50Model24.data(), vf50Model24.size());

        juce::String status;
        if (!dm.loadFile(model24File, *proc, status) ||
            proc->hostMemory().getUserVoice(0u).name != "VF5024_000" ||
            proc->hostMemory().getUserVoice(49u).name != "VF5024_049" ||
            proc->hostMemory().getUserVoice(99u).name != "VF5024_099") {
            std::cerr << "[FAIL] Native VF50 100-Voice Model ID 0x24 dump failed\n";
            return false;
        }
    }

    // 6. Concatenated SysEx Packets (4 consecutive F0..F7 single-voice dumps) & SMF .MID SysEx
    {
        vf50::midi::VF50SysExHandler gen;
        vf50::midi::SysExReplyBuffer reply;
        std::vector<uint8_t> concatBytes;
        for (std::size_t i = 0; i < 4u; ++i) {
            vf50::state::StoredVoiceSlot sv(testRom().factoryVoices[i + 10u]);
            char nm[11]{};
            std::snprintf(nm, sizeof(nm), "ConcatV_%02u", static_cast<unsigned>(i + 1u));
            sv.setName(nm);
            reply.clear();
            (void)gen.emitSingleVoiceEditDump(sv.voice, reply, true, false, false);
            const std::size_t startOff = concatBytes.size();
            reply.copyConcatenatedTo(concatBytes);
            for (std::size_t k = startOff; k + 3u < concatBytes.size(); ++k) {
                if (concatBytes[k] == 0xF0u && concatBytes[k + 1u] == 0x43u) {
                    concatBytes[k + 2u] = static_cast<uint8_t>((concatBytes[k + 2u] & 0xF0u) | ((i * 5u) & 0x0Fu));
                }
            }
        }
        const juce::File concatFile = tmpDir.getChildFile("concat_4voices.syx");
        (void)concatFile.replaceWithData(concatBytes.data(), concatBytes.size());

        juce::String status;
        if (!dm.loadFile(concatFile, *proc, status) ||
            proc->hostMemory().getUserVoice(0u).name != "ConcatV_01" ||
            proc->hostMemory().getUserVoice(1u).name != "ConcatV_02" ||
            proc->hostMemory().getUserVoice(2u).name != "ConcatV_03" ||
            proc->hostMemory().getUserVoice(3u).name != "ConcatV_04") {
            std::cerr << "[FAIL] Concatenated SysEx multi-packet sequential load failed\n";
            return false;
        }

        // Standard MIDI File (.MID) containing embedded 4-Op VCED SysEx event
        vf50::state::StoredVoiceSlot midVoice(testRom().factoryVoices[4]);
        midVoice.setName("SmfMidVce ");
        reply.clear();
        (void)gen.emitSingleVoiceEditDump(midVoice.voice, reply, false, false, false);
        std::vector<uint8_t> rawVced;
        reply.copyConcatenatedTo(rawVced);
        const uint8_t bodyAfterF0Len = static_cast<uint8_t>(rawVced.size() - 1u);
        const uint32_t mtrkLen = static_cast<uint32_t>(1u + 1u + 1u + bodyAfterF0Len + 4u);
        std::vector<uint8_t> smfBytes = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
            'M','T','r','k',
            static_cast<uint8_t>((mtrkLen >> 24u) & 0xFFu),
            static_cast<uint8_t>((mtrkLen >> 16u) & 0xFFu),
            static_cast<uint8_t>((mtrkLen >> 8u) & 0xFFu),
            static_cast<uint8_t>(mtrkLen & 0xFFu),
            0x00u, 0xF0u, bodyAfterF0Len
        };
        smfBytes.insert(smfBytes.end(), rawVced.begin() + 1, rawVced.end());
        smfBytes.insert(smfBytes.end(), { 0x00u, 0xFFu, 0x2Fu, 0x00u });

        const juce::File smfFile = tmpDir.getChildFile("EMBEDDED_PATCH.MID");
        (void)smfFile.replaceWithData(smfBytes.data(), smfBytes.size());
        if (!dm.loadFile(smfFile, *proc, status) ||
            proc->hostMemory().activeSingleVoice().name != "SmfMidVce ") {
            std::cerr << "[FAIL] SMF .MID embedded SysEx extraction failed: " << status << "\n";
            return false;
        }
    }

    // 7. Optional External Sound Banks (.syx / .SYX via VF50_TEST_SYX_DIR)
    {
        const juce::File syxDir = findTestSyxDirectory();
        if (syxDir.isDirectory()) {
            const auto syxFiles = syxDir.findChildFiles(juce::File::findFiles, true, "*.syx;*.SYX");
            juce::String status;
            const int limit = std::min(4, syxFiles.size());
            for (int i = 0; i < limit; ++i) {
                if (!dm.loadFile(syxFiles[i], *proc, status)) {
                    std::cerr << "[FAIL] Failed loading external .syx " << syxFiles[i].getFullPathName()
                              << ": " << status << "\n";
                    return false;
                }
            }
        }
    }

    // 8. Audit 720KB FAT12 Disk Image (.IMG = 737,280B) with .ALL, .VOI, .PFM, .SEQ + LCD Cursor & +1/-1 Navigation
    {
        auto raw720 = vf50::disk::VF50DiskManager::createFormattedFat12Image(false, "VF50_AUDIT");
        if (raw720.size() != 737280u) {
            std::cerr << "[FAIL] Expected 737,280-byte 720KB FAT12 image\n";
            return false;
        }
        const juce::File img720File = tmpDir.getChildFile("VF50_AUDIT.IMG");
        (void)img720File.replaceWithData(raw720.data(), raw720.size());

        juce::String status;
        if (!dm.mountFat12ImageFile(img720File, status)) {
            std::cerr << "[FAIL] Failed mounting 737,280B FAT12 .IMG: " << status << "\n";
            return false;
        }

        std::vector<uint8_t> allBytes, voiBytes, pfmBytes;
        vf50::disk::VF50DiskManager::buildAllDataBytes(proc->hostMemory(), allBytes);
        vf50::disk::VF50DiskManager::buildVoiceBankBytes(proc->hostMemory(), voiBytes);
        vf50::disk::VF50DiskManager::buildPerfBankBytes(proc->hostMemory(), pfmBytes);
        const std::vector<uint8_t> seqBytes = {
            0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u,
            0x00u, 0x03u, 0x00u, 120u,
            'D','I','S','K',' ','S','E','Q',
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
            1u, 2u, 99u, 0u, 0u, 0u,
            0xF0u, 0x00u, 0x90u, 60u, 100u, 0x30u, 0x80u, 60u, 0u, 0xF2u, 0xF9u
        };

        if (!dm.writeFileToMountedFat12("DEMO.ALL", allBytes.data(), allBytes.size(), status) ||
            !dm.writeFileToMountedFat12("VOICES.VOI", voiBytes.data(), voiBytes.size(), status) ||
            !dm.writeFileToMountedFat12("PERFS.PFM", pfmBytes.data(), pfmBytes.size(), status) ||
            !dm.writeFileToMountedFat12("SONG1.SEQ", seqBytes.data(), seqBytes.size(), status)) {
            std::cerr << "[FAIL] Failed writing .ALL/.VOI/.PFM/.SEQ to 720KB FAT12 image: " << status << "\n";
            return false;
        }

        juce::StringArray imgDrop;
        imgDrop.add(img720File.getFullPathName());
        editor->filesDropped(imgDrop, 50, 50);

        // Dropping a .IMG ONLY mounts the virtual floppy into the drive; browsing happens via panel [DISK]
        if (!dm.isMounted() || dm.mountedSourceName() != "VF50_AUDIT.IMG" || dm.entries().size() != 4u) {
            std::cerr << "[FAIL] Dropped 720KB .IMG did not mount VF50_AUDIT.IMG cleanly (mounted="
                      << dm.isMounted() << ", src='" << dm.mountedSourceName()
                      << "', entries=" << dm.entries().size() << ")\n";
            return false;
        }

        // Open authentic 40x2 Classic LCD Disk menu via [DISK] button -> Dir softkey
        canvas.pressDiskButton();
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::UtilDisk ||
            !lcd.getLineAsString(0).startsWith("UT DISK)")) {
            std::cerr << "[FAIL] Panel [DISK] button did not open Classic 40x2 LCD UT DISK) menu\n";
            return false;
        }
        canvas.pressSoftKey(6); // >Dir (F7 in UT DISK)
        if (!lcd.getLineAsString(0).contains("Directory") && !lcd.getLineAsString(0).contains("DIR")) {
            std::cerr << "[FAIL] Classic DISK -> Dir softkey did not open LCD directory listing: '"
                      << lcd.getLineAsString(0) << "'\n";
            return false;
        }
        canvas.stepFocusedField(+1);
        canvas.stepFocusedField(-1);

        // Optional external .img verification if VF50_TEST_DISK_DIR is set
        const juce::File extDiskDir = findTestDiskDirectory();
        if (extDiskDir.isDirectory()) {
            const auto extImgs = extDiskDir.findChildFiles(juce::File::findFiles, true, "*.img;*.IMG");
            if (!extImgs.isEmpty()) {
                if (!dm.mountFat12ImageFile(extImgs[0], status) || dm.entries().empty()) {
                    std::cerr << "[FAIL] External .img mount failed: " << status << "\n";
                    return false;
                }
            }
        }
    }

    // 9. Case-insensitive local directory scan check (.syx / .SYX / .MID)
    {
        juce::String status;
        if (!dm.mountDirectory(tmpDir, status) || dm.entries().size() < 5u) {
            std::cerr << "[FAIL] Case-insensitive local directory scanner missed uppercase/lowercase files ("
                      << dm.entries().size() << " entries)\n";
            return false;
        }
    }

    (void)tmpDir.deleteRecursively();
    std::cout << "[SelfTest:UniversalSysEx] All Universal SysEx, Drag-and-Drop, Checksum & FAT12 Floppy checks PASSED.\n";
    return true;
}

class MockDawPlayHead final : public juce::AudioPlayHead {
public:
    bool   playing = false;
    double bpm     = 132.0;
    double ppqPos  = 0.0;

    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setBpm(bpm);
        info.setPpqPosition(ppqPos);
        TimeSignature ts;
        ts.numerator   = 4;
        ts.denominator = 4;
        info.setTimeSignature(ts);
        return info;
    }
};

bool verifyExportedFormat1MidiFile(const juce::File& midiFile,
                                   bool requireAll8SequencerTracksNonEmpty,
                                   const char* contextLabel,
                                   int minPopulatedTracks = 6) {
    if (!midiFile.existsAsFile() || midiFile.getSize() < 64) {
        std::cerr << "[FAIL] " << contextLabel << ": exported .mid file missing or empty ("
                  << midiFile.getFullPathName() << ")\n";
        return false;
    }

    juce::FileInputStream inStream(midiFile);
    if (!inStream.openedOk()) {
        std::cerr << "[FAIL] " << contextLabel << ": failed to open .mid FileInputStream\n";
        return false;
    }

    int fileType = 0;
    juce::MidiFile parsed;
    if (!parsed.readFrom(inStream, true, &fileType)) {
        std::cerr << "[FAIL] " << contextLabel << ": juce::MidiFile::readFrom failed\n";
        return false;
    }

    if (fileType != 1 || parsed.getTimeFormat() != vf50::disk::VF50MidiExporter::kSmfPpqn ||
        parsed.getNumTracks() != 10) {
        std::cerr << "[FAIL] " << contextLabel << ": SMF header mismatch (type=" << fileType
                  << ", ppqn=" << parsed.getTimeFormat()
                  << ", tracks=" << parsed.getNumTracks() << ")\n";
        return false;
    }

    // Track 0: Conductor track (Tempo + Time Signature + Song Title text/track-name meta)
    const auto* trk0 = parsed.getTrack(0);
    bool hasTempo = false;
    bool hasTimeSig = false;
    bool hasTitleMeta = false;
    for (int i = 0; trk0 != nullptr && i < trk0->getNumEvents(); ++i) {
        const auto& m = trk0->getEventPointer(i)->message;
        if (m.isTempoMetaEvent()) hasTempo = true;
        if (m.isTimeSignatureMetaEvent()) hasTimeSig = true;
        if (m.isTrackNameEvent() || m.isTextMetaEvent()) hasTitleMeta = true;
    }
    if (!hasTempo || !hasTimeSig || !hasTitleMeta) {
        std::cerr << "[FAIL] " << contextLabel << ": Track 0 missing Conductor meta events (tempo="
                  << hasTempo << " ts=" << hasTimeSig << " title=" << hasTitleMeta << ")\n";
        return false;
    }

    // Tracks 1..8: Sequencer tracks on MIDI Ch 1..8
    int populatedFmTracks = 0;
    for (int t = 1; t <= 8; ++t) {
        const auto* seqTrk = parsed.getTrack(t);
        if (seqTrk == nullptr) {
            std::cerr << "[FAIL] " << contextLabel << ": null Track " << t << "\n";
            return false;
        }
        bool hasTrackName = false;
        bool hasProgChange = false;
        int noteOns = 0;
        int noteOffs = 0;
        for (int i = 0; i < seqTrk->getNumEvents(); ++i) {
            const auto& m = seqTrk->getEventPointer(i)->message;
            if (m.isTrackNameEvent() && m.getTextFromTextMetaEvent().startsWith("VF50 Track " + juce::String(t))) {
                hasTrackName = true;
            }
            if (m.isProgramChange() && m.getChannel() == t) {
                hasProgChange = true;
            }
            if (m.isNoteOn(false) && m.getChannel() == t) {
                ++noteOns;
            } else if (m.isNoteOff(true) && m.getChannel() == t) {
                ++noteOffs;
            }
        }
        if (!hasTrackName || !hasProgChange || noteOns != noteOffs) {
            std::cerr << "[FAIL] " << contextLabel << ": Track " << t
                      << " metadata/pairing mismatch (name=" << hasTrackName
                      << " pc=" << hasProgChange << " on=" << noteOns << " off=" << noteOffs << ")\n";
            return false;
        }
        if (noteOns > 0) {
            ++populatedFmTracks;
        }
    }
    if (requireAll8SequencerTracksNonEmpty && populatedFmTracks != 8) {
        std::cerr << "[FAIL] " << contextLabel << ": expected all 8 sequencer tracks non-empty, got "
                  << populatedFmTracks << "\n";
        return false;
    }
    if (!requireAll8SequencerTracksNonEmpty && populatedFmTracks < minPopulatedTracks) {
        std::cerr << "[FAIL] " << contextLabel << ": expected >= " << minPopulatedTracks
                  << " populated tracks, got " << populatedFmTracks << "\n";
        return false;
    }

    // Track 9: Rhythm track on MIDI Ch 10 mapped within C1..C6 (36..96)
    const auto* rhyTrk = parsed.getTrack(9);
    int drumNoteOns = 0;
    int drumNoteOffs = 0;
    for (int i = 0; rhyTrk != nullptr && i < rhyTrk->getNumEvents(); ++i) {
        const auto& m = rhyTrk->getEventPointer(i)->message;
        if (m.isNoteOn(false)) {
            if (m.getChannel() != 10 || m.getNoteNumber() < 36 || m.getNoteNumber() > 96) {
                std::cerr << "[FAIL] " << contextLabel << ": Track 9 drum note out of Ch10 C1..C6 bounds (ch="
                          << m.getChannel() << " note=" << m.getNoteNumber() << ")\n";
                return false;
            }
            ++drumNoteOns;
        } else if (m.isNoteOff(true) && m.getChannel() == 10) {
            ++drumNoteOffs;
        }
    }
    if (drumNoteOns == 0 || drumNoteOns != drumNoteOffs) {
        std::cerr << "[FAIL] " << contextLabel << ": Track 9 Rhythm empty or unpaired (on="
                  << drumNoteOns << " off=" << drumNoteOffs << ")\n";
        return false;
    }

    return true;
}

bool runWorkstationModesMidiExportAndProgrammerSelfTests(const juce::File& buildDir) {
    using vf50::params::ParamIndex;
    using vf50::state::MemoryBank;
    using vf50::state::WorkflowMode;

    std::cout << "[SelfTest:WorkstationV3] Verifying Unified Classic VMEM/PG Programmer, COMPARE/STORE/MemProtect, "
              << "DAW Sync, 8-Track SMF Export + Drag-to-DAW, and Legacy State Recall...\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 256);

    std::unique_ptr<juce::AudioProcessorEditor> rawEd(proc->createEditor());
    auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(rawEd.get());
    if (editor == nullptr) {
        std::cerr << "[FAIL] Failed to create VF50AudioProcessorEditor for WorkstationV3 self-test\n";
        return false;
    }

    auto& canvas = editor->mainCanvas();
    auto& lcd    = canvas.lcdDisplay();
    auto& dm     = canvas.diskManager();

    // 1. Optional external Bank SysEx check via VF50_TEST_SYX_DIR
    {
        const juce::File syxDir = findTestSyxDirectory();
        if (syxDir.isDirectory()) {
            const auto syxFiles = syxDir.findChildFiles(juce::File::findFiles, true, "*.syx;*.SYX");
            if (!syxFiles.isEmpty()) {
                juce::String bankStatus;
                if (!dm.loadFile(syxFiles[0], *proc, bankStatus)) {
                    std::cerr << "[FAIL] Failed to load external .syx: " << bankStatus << "\n";
                    return false;
                }
            }
        }
    }

    // Select clean Factory ROM P00 ("VF50 PIANO ") before testing PG <-> Classic VMEM & audio continuity
    proc->selectSinglePresetAtomic(MemoryBank::FactoryRom, 0u);
    canvas.refreshDisplayAndLeds();

    // 2. Live Polyphonic Audio Continuity while toggling PG Programmer Drawer & Virtual Keyboard
    {
        juce::AudioBuffer<float> audioBuf(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<uint8_t>(100)), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<uint8_t>(100)), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<uint8_t>(100)), 0);
        audioBuf.clear();
        proc->processBlock(audioBuf, midi);
        midi.clear();

        for (int toggleStep = 0; toggleStep < 4; ++toggleStep) {
            canvas.pressProgrammerToggleButton();
            audioBuf.clear();
            proc->processBlock(audioBuf, midi);

            double sumSq = 0.0;
            for (int ch = 0; ch < 2; ++ch) {
                const float* p = audioBuf.getReadPointer(ch);
                for (int s = 0; s < 256; ++s) {
                    if (!std::isfinite(p[s])) {
                        std::cerr << "[FAIL] NaN/Inf during live PG Programmer toggle\n";
                        return false;
                    }
                    sumSq += static_cast<double>(p[s]) * static_cast<double>(p[s]);
                }
            }
            if (std::sqrt(sumSq / 512.0) < 1e-5) {
                std::cerr << "[FAIL] Audio dropout during live PG Programmer toggle\n";
                return false;
            }
        }

        midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        proc->processBlock(audioBuf, midi);
    }

    // 3. PG Programmer -> Active Classic VMEM Dirty Tracking, [COMPARE] Lock/Swap & [STORE] + Memory Protect
    {
        proc->selectSinglePresetAtomic(MemoryBank::FactoryRom, 0u);
        canvas.pressSingleButton();
        canvas.refreshDisplayAndLeds();

        const uint8_t origP00Alg  = proc->hostMemory().getFactoryVoice(0u).algorithm;
        const uint8_t origInt05Fb = proc->hostMemory().getUserVoice(5u).feedback;
        const uint8_t origP00Ol1  = proc->hostMemory().getFactoryVoice(0u).operators[0].outputLevel;

        if (proc->hostMemory().isSingleEditDirty()) {
            std::cerr << "[FAIL] Freshly selected P00 should start with clean VMEM (isSingleEditDirty==false)\n";
            return false;
        }

        // Edit VMEM algorithm, feedback, and OP1 output level via PG Programmer drawer
        const uint8_t editedAlg = static_cast<uint8_t>((origP00Alg + 3u) % 8u);
        const uint8_t editedFb  = static_cast<uint8_t>((origInt05Fb + 3u) % 8u);
        const uint8_t editedOl1 = static_cast<uint8_t>((origP00Ol1 >= 50u) ? 42u : 88u);
        canvas.programmerDrawer().selectAlgorithmOneBased(static_cast<int>(editedAlg) + 1, true);
        canvas.programmerDrawer().setFaderValueForParam(ParamIndex::VoiceFeedback, static_cast<double>(editedFb), true);
        canvas.programmerDrawer().setFaderValueForParam(ParamIndex::Op1OutputLevel, static_cast<double>(editedOl1), true);

        // Immediately verify Classic VMEM host buffer + dirty flag updated synchronously BEFORE audio callback
        if (!proc->hostMemory().isSingleEditDirty() ||
            proc->hostMemory().singleEditSlot().voice.algorithm != editedAlg ||
            proc->hostMemory().singleEditSlot().voice.feedback != editedFb ||
            proc->hostMemory().singleEditSlot().voice.operators[0].outputLevel != editedOl1) {
            std::cerr << "[FAIL] PG Programmer slider edits did not update active Classic VMEM buffer live\n";
            return false;
        }

        juce::AudioBuffer<float> settleBuf(2, 256);
        juce::MidiBuffer emptyMidi;
        settleBuf.clear();
        proc->processBlock(settleBuf, emptyMidi);

        if (proc->synthEngine().fm().activeSingleVoice().algorithm != editedAlg ||
            proc->synthEngine().fm().activeSingleVoice().feedback != editedFb ||
            proc->synthEngine().fm().activeSingleVoice().operators[0].outputLevel != editedOl1) {
            std::cerr << "[FAIL] PG-edited Classic VMEM did not propagate to live FM engine\n";
            return false;
        }

        // Verify Classic [COMPARE] swaps live audio & VMEM readout back to unedited P00 and locks PG edits
        canvas.pressCompareButton();
        proc->processBlock(settleBuf, emptyMidi);
        if (!proc->hostMemory().isSingleCompareActive() ||
            proc->synthEngine().fm().activeSingleVoice().algorithm != origP00Alg) {
            std::cerr << "[FAIL] Classic [COMPARE] failed to swap live audio back to unedited P00\n";
            return false;
        }
        // Attempt PG slider move while Compare is active -> must be blocked and keep VMEM untouched
        canvas.programmerDrawer().setFaderValueForParam(
            ParamIndex::VoiceFeedback, static_cast<double>((editedFb + 1u) % 8u), true);
        if (proc->hostMemory().singleEditSlot().voice.feedback != editedFb) {
            std::cerr << "[FAIL] PG slider mutated VMEM while Classic [COMPARE] lock was active\n";
            return false;
        }

        // Second [COMPARE] press restores PG-edited VMEM cleanly
        canvas.pressCompareButton();
        proc->processBlock(settleBuf, emptyMidi);
        if (proc->hostMemory().isSingleCompareActive() ||
            proc->synthEngine().fm().activeSingleVoice().algorithm != editedAlg ||
            proc->synthEngine().fm().activeSingleVoice().feedback != editedFb) {
            std::cerr << "[FAIL] Second [COMPARE] press failed to restore PG-edited Classic VMEM\n";
            return false;
        }

        // Memory Protect ON: Classic [STORE] blocks commit to INT 05
        proc->setIntMemoryProtected(true);
        canvas.pressStoreButton();
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::StoreConfirm
            || !lcd.getLineAsString(1).contains("Mem Store")
            || !lcd.getLineAsString(1).contains("Prot?")) {
            std::cerr << "[FAIL] Classic [STORE] did not show Mem Store Prot? prompt\n";
            return false;
        }
        canvas.pressKeypadDigitButton(0);
        canvas.pressKeypadDigitButton(5);
        canvas.pressKeypadYesButton();
        if (!lcd.getLineAsString(1).contains("Memory Protected")
            || proc->hostMemory().getUserVoice(5u).feedback == editedFb) {
            std::cerr << "[FAIL] Classic mode failed to block [STORE] when Memory Protect is ON\n";
            return false;
        }
        canvas.pressKeypadNoButton(); // Cancel back cleanly

        // Memory Protect OFF: Classic [STORE] -> 05 -> [+1/YES] permanently commits PG-edited VMEM to INT 05!
        proc->setIntMemoryProtected(false);
        canvas.pressStoreButton();
        canvas.pressKeypadDigitButton(0);
        canvas.pressKeypadDigitButton(5);
        canvas.pressKeypadYesButton();
        proc->processBlock(settleBuf, emptyMidi);
        if (proc->hostMemory().getUserVoice(5u).algorithm != editedAlg ||
            proc->hostMemory().getUserVoice(5u).feedback != editedFb ||
            proc->hostMemory().getUserVoice(5u).operators[0].outputLevel != editedOl1) {
            std::cerr << "[FAIL] Classic [STORE] failed to commit PG-edited VMEM into INT 05\n";
            return false;
        }

        // Authentic Classic [STORE] confirmation workflow -> INT 06
        canvas.pressStoreButton();
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::StoreConfirm
            || !lcd.getLineAsString(0).startsWith("SINGLE MODE>")
            || !lcd.getLineAsString(1).contains("Mem Store")) {
            std::cerr << "[FAIL] Classic [STORE] did not open StoreConfirm prompt\n";
            return false;
        }
        // Enter destination slot 06 via 10-key pad and confirm with [+1/YES]
        canvas.pressKeypadDigitButton(0);
        canvas.pressKeypadDigitButton(6);
        canvas.pressKeypadYesButton();
        if (proc->hostMemory().getUserVoice(6u).algorithm != editedAlg) {
            std::cerr << "[FAIL] Classic [+1/YES] Store confirmation did not write VMEM to INT 06\n";
            return false;
        }

        // Verify Sequencer Track Setup screen & DAW AudioPlayHead Sync
        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SequencerTrackSetup);
        if (!lcd.getLineAsString(0).contains("TRANSMIT CHANNEL")) {
            std::cerr << "[FAIL] SequencerTrackSetup LCD header unexpected: " << lcd.getLineAsString(0) << "\n";
            return false;
        }

        MockDawPlayHead mockPlayHead;
        proc->selectDemoSong(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        proc->setPlayHead(&mockPlayHead);
        proc->setDawSyncEnabled(true);
        proc->setDawSequencerArmed(true);
        mockPlayHead.playing = true;
        mockPlayHead.bpm     = 132.0;
        for (int b = 0; b < 8; ++b) {
            mockPlayHead.ppqPos = static_cast<double>(b) * 0.25;
            settleBuf.clear();
            proc->processBlock(settleBuf, emptyMidi);
        }
        if (!proc->isDemoPlaying()) {
            std::cerr << "[FAIL] AudioPlayHead DAW Sync did not start armed VF50 sequencer\n";
            return false;
        }
        mockPlayHead.playing = false;
        proc->processBlock(settleBuf, emptyMidi);
        proc->setDawSyncEnabled(false);
        proc->setDawSequencerArmed(false);
        proc->setPlayHead(nullptr);
    }

    // 4. 8-Track Sequencer to MIDI Export (.MID) & Drag-to-DAW Temp File Verification
    const juce::File exportTmpDir = buildDir.getChildFile("midi_export_selftest_tmp");
    (void)exportTmpDir.deleteRecursively();
    (void)exportTmpDir.createDirectory();
    {
        // 4a. Export Factory ROM Demo Song 0 ("Victory!") via isolated Demo Bank
        proc->selectDemoSong(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        const juce::File romDemoMid = exportTmpDir.getChildFile("rom_demo_victory.mid");
        juce::String exportStatus;
        if (!canvas.exportMidiToFile(romDemoMid, exportStatus) ||
            !verifyExportedFormat1MidiFile(romDemoMid, false, "ROM Demo Song 0")) {
            return false;
        }

        // 4b. Populate a full 8-track + Rhythm CustomSeqSongBuffer and verify both [EXPORT .MID] and [DRAG 8 TRACKS TO DAW]
        auto full8TrackSeqPtr = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        auto& full8TrackSeq   = *full8TrackSeqPtr;
        full8TrackSeq.valid           = true;
        full8TrackSeq.setTitle("8TRK WORKSTATION");
        full8TrackSeq.initialTempoBpm = 124u;
        full8TrackSeq.timeSigNum      = 4u;
        full8TrackSeq.timeSigDen      = 4u;
        full8TrackSeq.ticksPerBar     = 192u;
        full8TrackSeq.totalBars       = 2u;
        full8TrackSeq.totalTicks      = 384u;
        full8TrackSeq.playMode        = 1u;
        full8TrackSeq.perfPresetBank  = 1u;
        full8TrackSeq.perfNumber      = 0u;

        uint32_t evIdx = 0u;
        auto pushEv = [&](uint32_t tick, vf50::generated::DemoEventType type, uint8_t ch, uint8_t d1, uint8_t d2) {
            if (evIdx < full8TrackSeq.events.size()) {
                full8TrackSeq.events[evIdx++] = { tick, type, ch, d1, d2 };
            }
        };

        // Initial Program Change + CC64 + PitchBend + NoteOn on all 8 tracks (ch 0..7) + Drum hits
        for (uint8_t ch = 0u; ch < 8u; ++ch) {
            pushEv(0u, vf50::generated::DemoEventType::FmProgram, ch, 1u, ch);
            pushEv(0u, vf50::generated::DemoEventType::FmControl, ch, 64u, 127u);
            pushEv(12u, vf50::generated::DemoEventType::FmPitchBend, ch, 0u, 72u);
            pushEv(static_cast<uint32_t>(ch) * 12u,
                   vf50::generated::DemoEventType::FmNoteOn, ch, static_cast<uint8_t>(48u + ch * 3u), 104u);
        }
        for (uint32_t step = 0u; step < 8u; ++step) {
            pushEv(step * 48u, vf50::generated::DemoEventType::DrumHit, 9u,
                   static_cast<uint8_t>((step % 2u == 0u) ? 1u : 13u), 7u);
        }
        for (uint8_t ch = 0u; ch < 8u; ++ch) {
            pushEv(240u + static_cast<uint32_t>(ch) * 12u,
                   vf50::generated::DemoEventType::FmNoteOff, ch, static_cast<uint8_t>(48u + ch * 3u), 0u);
            pushEv(360u, vf50::generated::DemoEventType::FmControl, ch, 64u, 0u);
        }
        std::sort(full8TrackSeq.events.begin(),
                  full8TrackSeq.events.begin() + static_cast<std::ptrdiff_t>(evIdx),
                  [](const auto& a, const auto& b) { return a.tick < b.tick; });
        full8TrackSeq.eventCount = evIdx;

        proc->publishCustomSequenceToAudio(full8TrackSeq);
        proc->selectDemoSong(vf50::dsp::VF50DemoSongPlayer::kCustomSongSlot);

        const juce::File seq8Mid = exportTmpDir.getChildFile("full_8track_seq.mid");
        if (!canvas.exportMidiToFile(seq8Mid, exportStatus) ||
            !verifyExportedFormat1MidiFile(seq8Mid, true, "8-Track Custom SEQ Export")) {
            return false;
        }

        juce::File dragTempMid;
        juce::String dragStatus;
        if (!canvas.prepareDragToDawTempMidi(dragTempMid, dragStatus) ||
            !verifyExportedFormat1MidiFile(dragTempMid, true, "Drag-to-DAW Temp SMF")) {
            return false;
        }
        (void)dragTempMid.deleteFile();
        proc->selectDemoSong(0u);
    }
    (void)exportTmpDir.deleteRecursively();

    // 5. Pop-Down "PG-Style" 63-Slider Vertical Programmer Drawer + Multi-Scale Layout + Legacy State Recall
    {
        if (editor->isProgrammerVisible()) {
            canvas.pressProgrammerToggleButton();
        }

        // Toggle Programmer ON
        canvas.pressProgrammerToggleButton();
        if (!editor->isProgrammerVisible() || !canvas.isProgrammerVisible() ||
            canvas.programmerDrawer().totalFaderCount() != 63u) {
            std::cerr << "[FAIL] PG Programmer Drawer toggle or 63-fader count check failed (visible="
                      << editor->isProgrammerVisible() << ", faders="
                      << canvas.programmerDrawer().totalFaderCount() << ")\n";
            return false;
        }

        // Verify crisp vector scaling across 75%, 100%, 125%, 150%, 200% with Programmer open
        for (const float scale : { 0.75f, 1.00f, 1.25f, 1.50f, 2.00f }) {
            editor->applyScaleFactor(scale);
            const int expectedW = static_cast<int>(std::lround(static_cast<float>(vf50::gui::tokens::kBaseRefWidth) * scale));
            const int expectedH = static_cast<int>(std::lround(780.0f * scale)); // (880 + 680) / 2 = 780 @ 1x
            if (editor->getWidth() != expectedW || editor->getHeight() != expectedH) {
                std::cerr << "[FAIL] Programmer Drawer window dimensions wrong at scale " << scale
                          << ": got " << editor->getWidth() << "x" << editor->getHeight()
                          << ", expected " << expectedW << "x" << expectedH << "\n";
                return false;
            }
        }

        // Also toggle 61-key Virtual Keyboard ON so Programmer stacks cleanly above Keyboard (950px @ 1x)
        editor->applyScaleFactor(1.00f);
        editor->setVirtualKeyboardVisible(true);
        if (editor->getWidth() != 1482 || editor->getHeight() != 950) {
            std::cerr << "[FAIL] Stacked Programmer + Virtual Keyboard dimensions wrong at 1x: got "
                      << editor->getWidth() << "x" << editor->getHeight() << " (expected 1482x950)\n";
            return false;
        }

        const juce::Image stackedSnap = editor->createComponentSnapshot(editor->getLocalBounds());
        if (stackedSnap.isNull()) {
            std::cerr << "[FAIL] Failed to render stacked VF50 + PG Programmer + Keyboard snapshot\n";
            return false;
        }
        {
            const juce::File snapFile = buildDir.getChildFile("vf50_programmer_drawer_1x.png");
            juce::FileOutputStream snapStream(snapFile);
            if (snapStream.openedOk()) {
                snapStream.setPosition(0);
                snapStream.truncate();
                juce::PNGImageFormat pngFmt;
                (void)pngFmt.writeImageToStream(stackedSnap, snapStream);
                snapStream.flush();
            }
        }

        // Live real-time FM parameter tweaks via PG vertical faders while a note is sounding
        proc->selectSinglePresetAtomic(MemoryBank::FactoryRom, 0u);
        canvas.refreshDisplayAndLeds();

        juce::AudioBuffer<float> liveBuf(2, 256);
        juce::MidiBuffer liveMidi;
        liveMidi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<uint8_t>(110)), 0);
        liveBuf.clear();
        proc->processBlock(liveBuf, liveMidi);
        liveMidi.clear();

        auto& drawer = canvas.programmerDrawer();
        drawer.selectAlgorithmOneBased(5, true);
        drawer.setFaderValueForParam(ParamIndex::Op1Coarse, 4.0, true);
        drawer.setFaderValueForParam(ParamIndex::Op1OutputLevel, 92.0, true);
        drawer.setFaderValueForParam(ParamIndex::PegPl1, 68.0, true);
        drawer.setFaderValueForParam(ParamIndex::Lfo1Speed, 64.0, true);

        liveBuf.clear();
        proc->processBlock(liveBuf, liveMidi);
        const auto& liveVoice = proc->synthEngine().fm().activeSingleVoice();
        if (liveVoice.algorithm != 4u ||
            liveVoice.operators[0].coarse != 4u ||
            liveVoice.operators[0].outputLevel != 92u ||
            liveVoice.peg.pl1 != 68u ||
            liveVoice.lfoSpeed != 64u) {
            std::cerr << "[FAIL] PG Programmer real-time parameter tweaks did not match live FM voice "
                      << "(alg=" << static_cast<int>(liveVoice.algorithm)
                      << " op1.coarse=" << static_cast<int>(liveVoice.operators[0].coarse)
                      << " op1.ol=" << static_cast<int>(liveVoice.operators[0].outputLevel)
                      << " peg0=" << static_cast<int>(liveVoice.peg.pl1)
                      << " lfoSpd=" << static_cast<int>(liveVoice.lfoSpeed) << ")\n";
            return false;
        }
        if (canvas.activeMenuPage() != vf50::gui::VF50MainCanvas::MenuPage::SingleLfo) {
            std::cerr << "[FAIL] Adjusting LFO1 Speed on PG Programmer did not mirror LCD to SingleLfo\n";
            return false;
        }

        liveMidi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        proc->processBlock(liveBuf, liveMidi);

        // Bi-directional preset switch -> all 63 PG faders snap cleanly to Preset 12 values
        proc->selectSinglePresetAtomic(MemoryBank::FactoryRom, 12u);
        canvas.refreshDisplayAndLeds();
        const auto& p12 = proc->hostMemory().getFactoryVoice(12u);
        if (drawer.selectedAlgorithmOneBased() != static_cast<int>(p12.algorithm) + 1 ||
            juce::roundToInt(drawer.getFaderValueForParam(ParamIndex::Op1OutputLevel)) != static_cast<int>(p12.operators[0].outputLevel) ||
            juce::roundToInt(drawer.getFaderValueForParam(ParamIndex::Op4Rr)) != static_cast<int>(p12.operators[3].rr)) {
            std::cerr << "[FAIL] PG Programmer faders did not snap to Preset 12 on bank/preset change\n";
            return false;
        }

        // State Persistence Round-Trip + Backward-Compatible Recall of Legacy OperatingMode=Modern (0) State
        juce::MemoryBlock stateBlob;
        proc->getStateInformation(stateBlob);

        // Simulate a legacy DAW session that saved OperatingMode=0 (Modern) inside the 'VF5W' trailer
        auto* rawBytes = static_cast<uint8_t*>(stateBlob.getData());
        const std::size_t blobSz = stateBlob.getSize();
        bool patchedLegacyModernByte = false;
        for (std::size_t i = 0u; i + 8u <= blobSz; ++i) {
            if (rawBytes[i] == 'V' && rawBytes[i + 1u] == 'F' && rawBytes[i + 2u] == '5' && rawBytes[i + 3u] == 'W') {
                rawBytes[i + 4u] = 0u; // Legacy OperatingMode::Modern
                patchedLegacyModernByte = true;
                break;
            }
        }
        if (!patchedLegacyModernByte) {
            std::cerr << "[FAIL] Could not locate 'VF5W' trailer in serialized state blob\n";
            return false;
        }

        auto restoredProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        restoredProc->prepareToPlay(48000.0, 256);
        restoredProc->setStateInformation(stateBlob.getData(), static_cast<int>(stateBlob.getSize()));
        if (restoredProc->workflowMode() != WorkflowMode::Classic || !restoredProc->isProgrammerVisible()) {
            std::cerr << "[FAIL] Legacy Modern VF50W state recall failed to load cleanly in Unified Classic mode\n";
            return false;
        }
    }

    std::cout << "[SelfTest:WorkstationV3] All Unified Classic, PG<->VMEM/COMPARE/STORE, DAW Sync, 8-Track SMF Export & Legacy State checks PASSED.\n";
    return true;
}

bool runVF50DiskA04AndDirDispatcherSelfTests(const juce::File& buildDir) {
    using vf50::disk::VF50DiskManager;
    using vf50::disk::VF50FileKind;
    using vf50::state::MemoryBank;
    using vf50::state::WorkflowMode;

    std::cout << "[SelfTest:DiskA04Dir] Verifying 19,456B AWESOME!.A04 + Multi-Chunk .V## Parsing, "
              << "Universal DIR/IMPRT Filters & Dispatchers, SEQ Integration, 8-Track SMF Export & Live Mode Toggle...\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 256);

    std::unique_ptr<juce::AudioProcessorEditor> baseEd(proc->createEditor());
    auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
    if (editor == nullptr) {
        std::cerr << "[FAIL] Failed to create editor for Disk A04/DIR self-test\n";
        return false;
    }
    auto& canvas = editor->mainCanvas();
    auto& lcd    = canvas.lcdDisplay();

    // 1. Verify Universal Case-Insensitive File Filter & Drag-and-Drop Acceptance (and strict rejection of ._DD!)
    const juce::String filterStr(VF50DiskManager::kUniversalFileFilter);
    for (const char* reqPat : { "*.a*", "*.A*", "*.i*", "*.I*", "*.v*", "*.V*", "*.r*", "*.R*",
                                "*.c*", "*.C*", "*.b*", "*.B*", "*.all", "*.ALL",
                                "*.voi", "*.VOI", "*.pfm", "*.PFM", "*.rhy", "*.RHY",
                                "*.seq", "*.SEQ", "*.crd", "*.CRD", "*.mdr", "*.MDR",
                                "*.syx", "*.SYX", "*.img", "*.IMG",
                                "*.dsk", "*.DSK", "*.mid", "*.MID" }) {
        if (!filterStr.contains(reqPat)) {
            std::cerr << "[FAIL] kUniversalFileFilter missing required pattern: " << reqPat << "\n";
            return false;
        }
    }
    if (filterStr.contains("_dd") || filterStr.contains("_DD")) {
        std::cerr << "[FAIL] kUniversalFileFilter must NOT include MS-DOS recovery extension ._DD!\n";
        return false;
    }
    const auto sysTmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    if (!editor->isInterestedInFileDrag(juce::StringArray{ sysTmpDir.getChildFile("AWESOME!.A04").getFullPathName() }) ||
        !editor->isInterestedInFileDrag(juce::StringArray{ sysTmpDir.getChildFile("VF50DEMO1.V01").getFullPathName() }) ||
        !editor->isInterestedInFileDrag(juce::StringArray{ sysTmpDir.getChildFile("BANK.voi").getFullPathName() }) ||
        editor->isInterestedInFileDrag(juce::StringArray{ sysTmpDir.getChildFile("FILE0000._DD").getFullPathName() }) ||
        editor->isInterestedInFileDrag(juce::StringArray{ sysTmpDir.getChildFile("README.TXT").getFullPathName() })) {
        std::cerr << "[FAIL] PluginEditor::isInterestedInFileDrag failed allowlist / non-VF50 rejection check\n";
        return false;
    }

    // 2. Build test directory with .V01 (index 0!), AWESOME!.A04 (19,456 bytes), .VOI, .PFM, .RHY, .SEQ, .SYX
    const juce::File dirRoot = buildDir.getChildFile("vf50_dir_a04_selftest_tmp");
    (void)dirRoot.deleteRecursively();
    (void)dirRoot.createDirectory();

    // 2a. Build an authentic 19,456-byte (19 x 1KB sectors) hardware VF50 AWESOME!.A04 (HardwareSeqV1, 8 tracks, 128 BPM)
    std::vector<uint8_t> a04Bytes;
    a04Bytes.reserve(19456u);
    {
        const uint8_t hdr[34] = {
            0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u,
            0x01u, 0x03u, 0x00u, 128u,             // 4/4 (ts=3), 128 BPM
            'A','W','E','S','O','M','E','!',       // 8-char Song Title
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,        // Tracks 1..8 -> MIDI Ch 1..8
            1u, 2u, 0u, 0u, 0u, 0u                 // Perf mode, Preset bank, Perf #00
        };
        a04Bytes.insert(a04Bytes.end(), std::begin(hdr), std::end(hdr));
        for (uint8_t trk = 0u; trk < 8u; ++trk) {
            a04Bytes.push_back(0xF0u);
            a04Bytes.push_back(trk);
            // Program Change (Preset bank = 121, voice = trk)
            a04Bytes.push_back(0xFCu);
            a04Bytes.push_back(121u);
            a04Bytes.push_back(0xFCu);
            a04Bytes.push_back(trk);
            // Sustain CC64 = 127, Pitch Bend center+offset
            a04Bytes.push_back(0xFBu);
            a04Bytes.push_back(64u);
            a04Bytes.push_back(127u);
            a04Bytes.push_back(0xFEu);
            a04Bytes.push_back(0u);
            a04Bytes.push_back(72u);
            // 4 quarter-note bars with notes on each track
            for (uint8_t step = 0u; step < 8u; ++step) {
                const uint8_t note = static_cast<uint8_t>(48u + trk * 3u + (step % 4u));
                a04Bytes.push_back(static_cast<uint8_t>(0x80u | 36u)); // gate = 36 ticks
                a04Bytes.push_back(note);                               // velocity byte follows
                a04Bytes.push_back(static_cast<uint8_t>(96u + trk * 3u));
                a04Bytes.push_back(static_cast<uint8_t>(48u << 1u));    // delta = 48 ticks (Wait: 0x60 <= 0x7F -> 0x60>>1 = 48)
                if ((step % 4u) == 3u) {
                    a04Bytes.push_back(0xF5u); // bar marker
                }
            }
            a04Bytes.push_back(0xFBu);
            a04Bytes.push_back(64u);
            a04Bytes.push_back(0u);
            a04Bytes.push_back(0xF2u); // End of track
        }
        a04Bytes.push_back(0xF9u); // End of song
        a04Bytes.resize(19456u, 0xF8u); // Pad to exact 19,456 bytes (19 x 1KB hardware sectors)
    }
    const juce::File a04File = dirRoot.getChildFile("AWESOME!.A04");
    (void)a04File.replaceWithData(a04Bytes.data(), a04Bytes.size());

    // 2b. Build authentic hardware multi-chunk 00_FIRST.V01 (1KB hdr + 100x128B Perfs + 100x128B Voices w/ 88+6+7 DSP!)
    //     Leave Voice Slot 50 empty (all zeros) to prove unrelated INTERNAL RAM Slot 50 is NOT corrupted!
    {
        std::vector<uint8_t> v01Bytes(1024u + 100u * 128u + 100u * 128u, 0u);
        vf50::state::StoredPerformanceSlot pSlot(testRom().factoryPerformances[2]);
        pSlot.setName("HwV01Perf0");
        pSlot.repackRomBytes();
        std::memcpy(&v01Bytes[1024u + 0u * 128u], pSlot.perf.rawRomBytes.data(), 97u);
        vf50::state::StoredPerformanceSlot pSlot1(testRom().factoryPerformances[3]);
        pSlot1.setName("HwV01Perf1");
        pSlot1.repackRomBytes();
        std::memcpy(&v01Bytes[1024u + 1u * 128u], pSlot1.perf.rawRomBytes.data(), 97u);

        vf50::state::StoredVoiceSlot vSlot(testRom().factoryVoices[8]);
        vSlot.setName("HwV01Vce00");
        vSlot.voice.effect.effectType = 6u; // Stereo Echo -> encoded at ROM95[88], floppy [+94]!
        vSlot.voice.effect.balance      = 55u;
        vSlot.repackRomBytes();
        const std::size_t vBase = 1024u + 100u * 128u;
        std::memcpy(&v01Bytes[vBase], vSlot.voice.rawRomBytes.data(), 88u);
        // Leave +88..+93 = 0 (6-byte hardware pad!) and write 7 DSP bytes at +94..+100!
        std::memcpy(&v01Bytes[vBase + 94u], vSlot.voice.rawRomBytes.data() + 88u, 7u);

        (void)dirRoot.getChildFile("00_FIRST.V01").replaceWithData(v01Bytes.data(), v01Bytes.size());
    }

    // 2c. Populate .VOI, .PFM, .RHY, .SEQ, .SYX files in the same directory
    {
        vf50::state::VF50MemoryManager tempMem{};
        tempMem.setRuntimeRomBank(&testRom(), true);
        vf50::state::StoredVoiceSlot voi0(testRom().factoryVoices[14]);
        voi0.setName("DirVoiSlot");
        tempMem.storeUserVoice(0u, voi0.voice);

        vf50::state::StoredPerformanceSlot pfm0(testRom().factoryPerformances[9]);
        pfm0.setName("DirPfmSlot");
        tempMem.storeUserPerformance(0u, pfm0.perf);

        auto rhyKit = tempMem.userRhythmKit(0u);
        rhyKit.instruments[1].volume = 13u;
        tempMem.setUserRhythmKit(0u, rhyKit);

        std::vector<uint8_t> voiBuf, pfmBuf, rhyBuf;
        VF50DiskManager::buildVoiceBankBytes(tempMem, voiBuf);
        VF50DiskManager::buildPerfBankBytes(tempMem, pfmBuf);
        VF50DiskManager::buildRhythmDataBytes(tempMem, rhyBuf);
        (void)dirRoot.getChildFile("BANK_B.VOI").replaceWithData(voiBuf.data(), voiBuf.size());
        (void)dirRoot.getChildFile("PERF_B.PFM").replaceWithData(pfmBuf.data(), pfmBuf.size());
        (void)dirRoot.getChildFile("BEAT_B.RHY").replaceWithData(rhyBuf.data(), rhyBuf.size());

        std::vector<uint8_t> seqBuf = {
            0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u,
            0x01u, 0x03u, 0x00u, 118u,
            'D','I','R','_','S','E','Q',' ',
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
            1u, 2u, 0u, 0u, 0u, 0u,
            0xF0u, 0x00u, 0x98u, 60u, 110u, 0x60u, 0xF5u, 0xF2u, 0xF9u
        };
        (void)dirRoot.getChildFile("SONG_B.SEQ").replaceWithData(seqBuf.data(), seqBuf.size());

        vf50::state::StoredVoiceSlot syxVoice(testRom().factoryVoices[22]);
        syxVoice.setName("DirSyxVce1");
        vf50::midi::VF50SysExHandler sxGen{};
        vf50::midi::SysExReplyBuffer sxReply{};
        (void)sxGen.emitSingleVoiceEditDump(syxVoice.voice, sxReply, true, true, true);
        (void)dirRoot.getChildFile("VOICE_B.SYX").replaceWithData(sxReply.storage.data(), sxReply.bytesUsed);
    }

    // 3. Mount directory via DIR and verify ALL 7 file types enumerate accurately
    juce::String status;
    if (!canvas.diskManager().mountDirectory(dirRoot, status)) {
        std::cerr << "[FAIL] mountDirectory failed: " << status << "\n";
        (void)dirRoot.deleteRecursively();
        return false;
    }

    const auto& entries = canvas.diskManager().entries();
    auto findEntryIdx = [&entries](const std::string& name, VF50FileKind expectedKind) -> int {
        for (std::size_t i = 0u; i < entries.size(); ++i) {
            if (entries[i].name == name && entries[i].kind == expectedKind) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    const int idxV01 = findEntryIdx("00_FIRST.V01", VF50FileKind::AllData);
    const int idxA04 = findEntryIdx("AWESOME!.A04", VF50FileKind::Sequence);
    const int idxVoi = findEntryIdx("BANK_B.VOI",   VF50FileKind::VoiceBank);
    const int idxPfm = findEntryIdx("PERF_B.PFM",   VF50FileKind::PerfBank);
    const int idxRhy = findEntryIdx("BEAT_B.RHY",   VF50FileKind::RhythmData);
    const int idxSeq = findEntryIdx("SONG_B.SEQ",   VF50FileKind::Sequence);
    const int idxSyx = findEntryIdx("VOICE_B.SYX",  VF50FileKind::SysEx);

    if (idxV01 != 0 || idxA04 <= 0 || idxVoi < 0 || idxPfm < 0 || idxRhy < 0 || idxSeq < 0 || idxSyx < 0) {
        std::cerr << "[FAIL] DIR enumeration missed one or more VF50 file types: "
                  << "V01=" << idxV01 << " A04=" << idxA04 << " VOI=" << idxVoi
                  << " PFM=" << idxPfm << " RHY=" << idxRhy << " SEQ=" << idxSeq
                  << " SYX=" << idxSyx << " (total=" << entries.size() << ")\n";
        (void)dirRoot.deleteRecursively();
        return false;
    }

    // 4. Plant a sentinel voice in INTERNAL Slot 50, then dispatch 00_FIRST.V01 to verify 88+6+7 DSP unpack & slot preservation
    {
        vf50::state::StoredVoiceSlot sentinel(testRom().factoryVoices[50]);
        sentinel.setName("KeepSlot50");
        proc->hostMemory().storeUserVoice(50u, sentinel.voice);
        proc->pushHostMemoryToAudio();

        if (!canvas.loadDirectoryEntry(static_cast<std::size_t>(idxV01), status) ||
            proc->hostMemory().getUserVoice(0u).name != "HwV01Vce00" ||
            proc->hostMemory().getUserVoice(0u).effect.effectType != 6u ||
            proc->hostMemory().getUserVoice(50u).name != "KeepSlot50") {
            std::cerr << "[FAIL] Hardware .V01 unpack failed DSP (+94..+100) or corrupted unrelated slot 50! "
                      << "v0='" << proc->hostMemory().getUserVoice(0u).name
                      << "' dsp=" << static_cast<int>(proc->hostMemory().getUserVoice(0u).effect.effectType)
                      << " vf50='" << proc->hostMemory().getUserVoice(50u).name << "'\n";
            (void)dirRoot.deleteRecursively();
            return false;
        }
    }

    // 5. Click/dispatch specific non-zero DIR entries (.PFM, .SYX, and AWESOME!.A04) -> must NOT default to index 0!
    if (!canvas.loadDirectoryEntry(static_cast<std::size_t>(idxPfm), status) ||
        proc->hostMemory().getUserPerformance(0u).name != "DirPfmSlot") {
        std::cerr << "[FAIL] DIR dispatch for PERF_B.PFM failed: " << status << "\n";
        (void)dirRoot.deleteRecursively();
        return false;
    }
    if (!canvas.loadDirectoryEntry(static_cast<std::size_t>(idxSyx), status) ||
        proc->hostMemory().activeSingleVoice().name != "DirSyxVce1") {
        std::cerr << "[FAIL] DIR dispatch for VOICE_B.SYX failed: " << status << "\n";
        (void)dirRoot.deleteRecursively();
        return false;
    }
    if (!canvas.loadDirectoryEntry(static_cast<std::size_t>(idxA04), status) ||
        !proc->hasCustomDemoSequence()) {
        std::cerr << "[FAIL] DIR dispatch for AWESOME!.A04 (index " << idxA04 << ") failed: " << status << "\n";
        (void)dirRoot.deleteRecursively();
        return false;
    }

    // Verify AWESOME!.A04 appears in Classic SEQ screen & song slot switching works via SEQ keypad/step
    canvas.pressSeqButton();
    proc->selectDemoSong(vf50::dsp::VF50DemoSongPlayer::kCustomSongSlot);
    if (proc->activeDemoSongIndex() != 0 || proc->activeDemoSongTitle() != "AWESOME!") {
        std::cerr << "[FAIL] Loaded AWESOME!.A04 active song metadata mismatch: '" << proc->activeDemoSongTitle() << "'\n";
        (void)dirRoot.deleteRecursively();
        return false;
    }

    // 6. Export AWESOME!.A04 via VF50MidiExporter (both Save .MID and Drag-to-DAW Temp SMF)
    const juce::File awesomeMid = dirRoot.getChildFile("AWESOME_8Tracks.mid");
    if (!canvas.exportMidiToFile(awesomeMid, status) ||
        !verifyExportedFormat1MidiFile(awesomeMid, true, "AWESOME!.A04 8-Track Export")) {
        (void)dirRoot.deleteRecursively();
        return false;
    }
    juce::File dragMid;
    if (!canvas.prepareDragToDawTempMidi(dragMid, status) ||
        !verifyExportedFormat1MidiFile(dragMid, true, "AWESOME!.A04 Drag-to-DAW Temp SMF")) {
        (void)dirRoot.deleteRecursively();
        return false;
    }
    (void)dragMid.deleteFile();

    // 7. Live audio continuity during unified Classic playback & PG Programmer toggle -> zero audio glitches/NaNs
    {
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{ 100 }), 0);
        buf.clear();
        proc->processBlock(buf, midi);
        midi.clear();

        canvas.pressProgrammerToggleButton();
        buf.clear();
        proc->processBlock(buf, midi);
        float rmsAfterToggle = buf.getRMSLevel(0, 0, 256) + buf.getRMSLevel(1, 0, 256);
        for (int s = 0; s < 256; ++s) {
            if (!std::isfinite(buf.getSample(0, s)) || !std::isfinite(buf.getSample(1, s))) {
                std::cerr << "[FAIL] Non-finite sample after live PG Programmer toggle\n";
                (void)dirRoot.deleteRecursively();
                return false;
            }
        }
        if (rmsAfterToggle < 1e-4f) {
            std::cerr << "[FAIL] Audio interrupted on live PG Programmer toggle (rms=" << rmsAfterToggle << ")\n";
            (void)dirRoot.deleteRecursively();
            return false;
        }
        canvas.pressProgrammerToggleButton();
        midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        proc->processBlock(buf, midi);
    }

    (void)dirRoot.deleteRecursively();

    // 8. Real Hardware Floppy Disk Folder Verification (via optional VF50_TEST_DISK_DIR)
    {
        const juce::File diskRoot = findTestDiskDirectory();
        juce::File realDisk1 = diskRoot.isDirectory() ? diskRoot.getChildFile("Disk A1") : juce::File{};
        if (diskRoot.isDirectory() && !realDisk1.isDirectory()) {
            realDisk1 = diskRoot.getChildFile("Disk 1");
        }
        if (realDisk1.isDirectory()) {
            juce::String realStatus;
            if (!canvas.diskManager().mountDirectory(realDisk1, realStatus)) {
                std::cerr << "[FAIL] Failed to mount real Disk A1 folder: " << realStatus << "\n";
                return false;
            }
            const auto& d1Entries = canvas.diskManager().entries();
            if (d1Entries.size() != 17u) {
                std::cerr << "[FAIL] Real Disk A1 expected exactly 17 valid VF50 disk files (excluding ._DD & .zip), got "
                          << d1Entries.size() << "\n";
                return false;
            }
            for (const auto& e : d1Entries) {
                if (e.name.find("._DD") != std::string::npos || e.name.find(".zip") != std::string::npos) {
                    std::cerr << "[FAIL] Real Disk A1 leaked non-VF50 file into directory: " << e.name << "\n";
                    return false;
                }
            }

            // Load EVERY SINGLE real hardware file in Disk 1 via loadDirectoryEntry and verify 100% success!
            int realA04Idx = -1;
            int realV11Idx = -1;
            int realV12Idx = -1;
            int realI02Idx = -1;
            int realR01Idx = -1;
            for (std::size_t i = 0u; i < d1Entries.size(); ++i) {
                if (d1Entries[i].name == "AWESOME!.A04") realA04Idx = static_cast<int>(i);
                if (d1Entries[i].name == "AWESOME!.V11") realV11Idx = static_cast<int>(i);
                if (d1Entries[i].name == "VIOLENCE.V12") realV12Idx = static_cast<int>(i);
                if (d1Entries[i].name == "AWESOME.I02")  realI02Idx = static_cast<int>(i);
                if (d1Entries[i].name == "DYNAMIC!.R01") realR01Idx = static_cast<int>(i);
            }
            if (realA04Idx < 0 || realV11Idx < 0 || realV12Idx < 0 || realI02Idx < 0 || realR01Idx < 0) {
                std::cerr << "[FAIL] Real Disk 1 missing expected files (A04/V11/V12/I02/R01)\n";
                return false;
            }

            // Verify EVERY single real hardware VF50 file loads cleanly without error
            for (std::size_t i = 0u; i < d1Entries.size(); ++i) {
                if (!canvas.loadDirectoryEntry(i, realStatus)) {
                    std::cerr << "[FAIL] Real Disk 1 file failed to load: " << d1Entries[i].name
                              << " (" << realStatus << ")\n";
                    return false;
                }
            }

            // Deep-verify REAL VIOLENCE.V12 (16-bit 264 BPM + multi-chunk SYN+RHY+SEQ at 0x0A000!)
            if (!canvas.loadDirectoryEntry(static_cast<std::size_t>(realV12Idx), realStatus) ||
                proc->hostCustomSequence() == nullptr ||
                proc->hostCustomSequence()->initialTempoBpm != 264u ||
                proc->hostCustomSequence()->eventCount < 4000u ||
                proc->hostMemory().getUserVoice(0u).name != "Universe  " ||
                proc->hostMemory().getUserVoice(1u).effect.effectType != 9u) {
                std::cerr << "[FAIL] Real VIOLENCE.V12 deep verification failed (bpm="
                          << (proc->hostCustomSequence() ? proc->hostCustomSequence()->initialTempoBpm : 0u)
                          << ", ev=" << (proc->hostCustomSequence() ? proc->hostCustomSequence()->eventCount : 0u)
                          << ", v0='" << proc->hostMemory().getUserVoice(0u).name
                          << "', v1.dsp=" << static_cast<int>(proc->hostMemory().getUserVoice(1u).effect.effectType) << ")\n";
                return false;
            }

            // Deep-verify REAL AWESOME!.V11 (100 Perfs + 100 Voices + 24 Rhythm Patterns + 1,344-note Song!)
            if (!canvas.loadDirectoryEntry(static_cast<std::size_t>(realV11Idx), realStatus) ||
                proc->hostCustomSequence() == nullptr ||
                proc->hostCustomSequence()->eventCount < 2600u ||
                proc->hostMemory().getUserPerformance(99u).name != "Kit8      ") {
                std::cerr << "[FAIL] Real AWESOME!.V11 deep verification failed (ev="
                          << (proc->hostCustomSequence() ? proc->hostCustomSequence()->eventCount : 0u)
                          << ", p99='" << proc->hostMemory().getUserPerformance(99u).name << "')\n";
                return false;
            }

            // Deep-verify REAL AWESOME!.A04 (19,456B -> 4,940 notes / 9,880 events, 120 BPM, 160 bars, Song 04: AWESOME!)
            if (!canvas.loadDirectoryEntry(static_cast<std::size_t>(realA04Idx), realStatus) ||
                proc->hostCustomSequence() == nullptr ||
                proc->hostCustomSequence()->eventCount != 9880u ||
                proc->hostCustomSequence()->initialTempoBpm != 120u ||
                proc->hostCustomSequence()->totalBars != 160u ||
                proc->activeDemoSongTitle() != "AWESOME!") {
                std::cerr << "[FAIL] Real AWESOME!.A04 deep verification failed (ev="
                          << (proc->hostCustomSequence() ? proc->hostCustomSequence()->eventCount : 0u)
                          << ", bpm=" << (proc->hostCustomSequence() ? proc->hostCustomSequence()->initialTempoBpm : 0u)
                          << ", bars=" << (proc->hostCustomSequence() ? proc->hostCustomSequence()->totalBars : 0u)
                          << ", row0='" << lcd.getLineAsString(0) << "')\n";
                return false;
            }

            // Export REAL AWESOME!.A04 to Format 1 480-PPQN 10-track .MID and verify (4 populated FM tracks, 4,940 notes + Ch10 Rhythm)!
            const juce::File realA04Mid = buildDir.getChildFile("vf50_real_awesome_a04_export.mid");
            if (!canvas.exportMidiToFile(realA04Mid, realStatus) ||
                !verifyExportedFormat1MidiFile(realA04Mid, false, "Real Hardware AWESOME!.A04 Export", 4)) {
                (void)realA04Mid.deleteFile();
                return false;
            }
            (void)realA04Mid.deleteFile();
            std::cout << "[PASS] Real Hardware Floppy Disk A1 verified: ALL " << d1Entries.size()
                      << " valid VF50 files (.A##/.V##/.I##/.R##, 0 ._DD/.zip leaks) loaded cleanly; AWESOME!.A04 (9,880 events / 4,940 notes), "
                      << "AWESOME!.V11 (100P+100V+RHY+SEQ), VIOLENCE.V12 (264 BPM) & 10-track SMF Export 100% PASSED!\n";
        }
    }

    std::cout << "[SelfTest:DiskA04Dir] AWESOME!.A04 (19,456B), Hardware .V01 DSP Unpack, DIR Dispatcher, "
              << "SEQ Song 04, 8-Track SMF Export & Live Mode Toggle PASSED.\n";
    return true;
}

bool runDecoupledSequencerModeSwitchSelfTest() {
    using vf50::state::ClassicTopMode;
    using vf50::state::MemoryBank;

    std::cout << "[SelfTest:SeqDecoupled] Verifying sequencer playback continues uninterrupted across "
              << "MODE_PERFORMANCE / VOICE / RHYTHM / UTILITY transitions and live Performance preset switching...\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 256);

    std::unique_ptr<juce::AudioProcessorEditor> baseEd(proc->createEditor());
    auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
    if (editor == nullptr) {
        std::cerr << "[FAIL] Failed to create editor for decoupled sequencer test\n";
        return false;
    }
    auto& canvas = editor->mainCanvas();

    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;

    auto pumpBlocks = [&](int count, double& outEnergy) -> bool {
        outEnergy = 0.0;
        for (int b = 0; b < count; ++b) {
            buf.clear();
            midi.clear();
            proc->processBlock(buf, midi);
            for (int s = 0; s < 256; ++s) {
                const float l = buf.getSample(0, s);
                const float r = buf.getSample(1, s);
                if (!std::isfinite(l) || !std::isfinite(r)) {
                    return false;
                }
                outEnergy += static_cast<double>(l * l + r * r);
            }
        }
        return true;
    };

    // 1. Start sequencer playback on isolated ROM Demo Song 0 ("Victory!") and advance past count-in into FM notes
    proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
    double e0 = 0.0;
    if (!pumpBlocks(420, e0) || !proc->isDemoPlaying() || proc->sequencerDispatchedNoteCount() == 0u) {
        std::cerr << "[FAIL] Sequencer failed to start or dispatch initial notes (playing="
                  << proc->isDemoPlaying() << ", notes=" << proc->sequencerDispatchedNoteCount() << ")\n";
        return false;
    }

    // 2. Switch directly to MODE_PERFORMANCE via both stateMachine and front-panel PERFORMANCE button
    const uint64_t notesBeforePerf = proc->sequencerDispatchedNoteCount();
    proc->stateMachine().setClassicTopMode(ClassicTopMode::MODE_PERFORMANCE);
    canvas.pressPerformanceButton();

    double ePerf = 0.0;
    if (!pumpBlocks(120, ePerf) ||
        !proc->isDemoPlaying() ||
        proc->stateMachine().classicTopMode() != ClassicTopMode::MODE_PERFORMANCE ||
        proc->sequencerDispatchedNoteCount() <= notesBeforePerf ||
        ePerf < 1e-4) {
        std::cerr << "[FAIL] Sequencer stopped or halted notes after switching to MODE_PERFORMANCE "
                  << "(playing=" << proc->isDemoPlaying()
                  << ", notesBefore=" << notesBeforePerf
                  << ", notesAfter=" << proc->sequencerDispatchedNoteCount()
                  << ", energy=" << ePerf << ")\n";
        return false;
    }

    // 3. Audition multiple Performance presets mid-playback (Factory P01 -> P05 -> User INT 00)
    for (const auto& target : std::array<std::pair<MemoryBank, std::size_t>, 3>{
             std::make_pair(MemoryBank::FactoryRom, std::size_t{ 1u }),
             std::make_pair(MemoryBank::FactoryRom, std::size_t{ 5u }),
             std::make_pair(MemoryBank::UserRam,    std::size_t{ 0u }) }) {
        const uint64_t notesBeforeSwap = proc->sequencerDispatchedNoteCount();
        proc->selectPerformancePresetAtomic(target.first, target.second, true);

        double eSwap = 0.0;
        if (!pumpBlocks(100, eSwap) ||
            !proc->isDemoPlaying() ||
            proc->synthEngine().fm().currentPerformanceIndex() != target.second ||
            proc->synthEngine().fm().currentPerformanceFromUserBank() != (target.first == MemoryBank::UserRam) ||
            proc->sequencerDispatchedNoteCount() <= notesBeforeSwap ||
            eSwap < 1e-4) {
            std::cerr << "[FAIL] Live Performance preset switch mid-playback failed (bank="
                      << static_cast<int>(target.first) << ", idx=" << target.second
                      << ", playing=" << proc->isDemoPlaying()
                      << ", activeIdx=" << proc->synthEngine().fm().currentPerformanceIndex()
                      << ", notes=" << proc->sequencerDispatchedNoteCount() << ")\n";
            return false;
        }
    }

    // 4. Navigate across SINGLE (VOICE), RHYTHM, and UTILITY SETUP -> sequencer MUST stay running throughout!
    const uint64_t notesBeforeNav = proc->sequencerDispatchedNoteCount();
    canvas.pressSingleButton();
    double eNav1 = 0.0;
    if (!pumpBlocks(60, eNav1) || !proc->isDemoPlaying()) {
        std::cerr << "[FAIL] Sequencer stopped when switching to SINGLE (VOICE) mode\n";
        return false;
    }
    canvas.pressRhythmButton();
    double eNav2 = 0.0;
    if (!pumpBlocks(60, eNav2) || !proc->isDemoPlaying()) {
        std::cerr << "[FAIL] Sequencer stopped when switching to RHYTHM mode\n";
        return false;
    }
    canvas.pressSetupButton();
    double eNav3 = 0.0;
    if (!pumpBlocks(60, eNav3) || !proc->isDemoPlaying() ||
        proc->sequencerDispatchedNoteCount() <= notesBeforeNav) {
        std::cerr << "[FAIL] Sequencer stopped or produced no notes across VOICE/RHYTHM/UTILITY navigation\n";
        return false;
    }

    // 5. Explicit [STOP] must cleanly stop the sequencer and release notes
    proc->requestDemoStop();
    double eStop = 0.0;
    if (!pumpBlocks(8, eStop) || proc->isDemoPlaying()) {
        std::cerr << "[FAIL] Explicit STOP failed to halt sequencer cleanly\n";
        return false;
    }

    std::cout << "[SelfTest:SeqDecoupled] Sequencer persisted seamlessly across PERFORMANCE, VOICE, RHYTHM, "
              << "UTILITY & live Performance preset swaps (" << proc->sequencerDispatchedNoteCount()
              << " notes dispatched) -> PASSED.\n";
    return true;
}

[[nodiscard]] float renderAndMeasurePeak(vf50::plugin::VF50AudioProcessor& proc,
                                         const juce::MidiBuffer& midiIn,
                                         int numBlocks = 8) {
    juce::AudioBuffer<float> buf(2, 512);
    float maxPeak = 0.0f;
    for (int b = 0; b < numBlocks; ++b) {
        buf.clear();
        juce::MidiBuffer blockMidi;
        if (b == 0) {
            blockMidi = midiIn;
        }
        proc.processBlock(buf, blockMidi);
        for (int ch = 0; ch < 2; ++ch) {
            const float* p = buf.getReadPointer(ch);
            for (int s = 0; s < buf.getNumSamples(); ++s) {
                if (!std::isfinite(p[s])) {
                    return -1.0f;
                }
                maxPeak = std::max(maxPeak, std::abs(p[s]));
            }
        }
    }
    return maxPeak;
}

bool savePngSnapshot(juce::Component& comp, const juce::File& outPng) {
    if (!outPng.getParentDirectory().exists()) {
        (void)outPng.getParentDirectory().createDirectory();
    }
    if (outPng.existsAsFile()) {
        outPng.deleteFile();
    }
    const auto img = comp.createComponentSnapshot(comp.getLocalBounds());
    if (!img.isValid()) {
        return false;
    }
    juce::PNGImageFormat pngFmt;
    std::unique_ptr<juce::FileOutputStream> stream(outPng.createOutputStream());
    if (!stream || !stream->openedOk()) {
        return false;
    }
    const bool ok = pngFmt.writeImageToStream(img, *stream);
    stream->flush();
    return ok;
}

bool runFirmwareFreeAndRuntimeRomLoadingTests(const juce::File& buildDir) {
    std::cout << "[SelfTest:NoROM] Verifying Firmware-Free Startup, Setup Guide UI, "
              << "Partial/Full Runtime EPROM Loading, Config Persistence, Audio & SMF Export...\n";

    vf50::rom::VF50RomSetBuilder::clearConfiguredPaths();

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 512);

    // 1. Assert zero ROM chips are present on clean launch
    {
        const auto& rom = proc->activeRomBank();
        if (rom.isAllRomsLoaded() || rom.loadedChipCount() != 0 ||
            rom.hasMainRomA || rom.hasMainRomB || rom.hasSubCpuRom || rom.hasDrumWaveRom) {
            std::cerr << "[FAIL] Firmware-free build unexpectedly has active ROMs on clean start!\n";
            return false;
        }
        if (std::string_view(rom.factoryVoices[0].name.data(), 10) != "INIT VOICE") {
            std::cerr << "[FAIL] Expected clean-room 'INIT VOICE' at slot 0, got '"
                      << std::string(rom.factoryVoices[0].name.data(), 10) << "'\n";
            return false;
        }
        if (rom.factoryDemoSongs[0].eventCount != 0u || rom.factoryDemoSongs[0].title != "NO ROM") {
            std::cerr << "[FAIL] Firmware-free build still contains embedded demo song events/title!\n";
            return false;
        }
    }

    // 2. Confirm Drums are safely silent (0.0f, no crash) before IC738 load, and Init Voice synthesizes clean FM audio
    {
        juce::MidiBuffer drumMidi;
        drumMidi.addEvent(juce::MidiMessage::noteOn(10, 36, uint8_t{ 120 }), 0);
        const float drumPeakNoRom = renderAndMeasurePeak(*proc, drumMidi, 4);
        if (drumPeakNoRom < 0.0f || drumPeakNoRom > 1e-6f) {
            std::cerr << "[FAIL] Expected silent drum output without IC738 ROM, got peak=" << drumPeakNoRom << "\n";
            return false;
        }

        juce::MidiBuffer fmMidi;
        fmMidi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{ 100 }), 0);
        const float fmPeak = renderAndMeasurePeak(*proc, fmMidi, 6);
        if (fmPeak < 0.05f) {
            std::cerr << "[FAIL] Clean-room Init Voice produced insufficient audio peak=" << fmPeak << "\n";
            return false;
        }

        juce::MidiBuffer offMidi;
        offMidi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        for (int ch = 1; ch <= 16; ++ch) {
            offMidi.addEvent(juce::MidiMessage::allSoundOff(ch), 1);
        }
        (void)renderAndMeasurePeak(*proc, offMidi, 4);

        proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        (void)renderAndMeasurePeak(*proc, {}, 4);
    }

    // 3. Create Editor -> Setup Guide Overlay must auto-open in NoROM mode when 0/4 chips are loaded
    std::unique_ptr<juce::AudioProcessorEditor> baseEditor(proc->createEditor());
    auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEditor.get());
    if (editor == nullptr) {
        std::cerr << "[FAIL] Failed to create VF50AudioProcessorEditor in NoROM build!\n";
        return false;
    }
    auto& canvas = editor->mainCanvas();
    if (!canvas.isRomGuideVisible()) {
        std::cerr << "[FAIL] Expected ROM Setup Guide overlay to auto-open on firmware-free startup!\n";
        return false;
    }
    const juce::File guideSnap = buildDir.getChildFile("vf50_norom_guide_1x.png");
    if (!savePngSnapshot(*editor, guideSnap)) {
        std::cerr << "[FAIL] Failed to save vf50_norom_guide_1x.png snapshot!\n";
        return false;
    }
    std::cout << "[PASS] Rendered firmware-free Setup Guide overlay -> " << guideSnap.getFullPathName() << "\n";

    // 4. Locate external EPROM images via VF50_TEST_ROM_DIR and test Partial -> Full runtime ROM loading
    const juce::File romRoot = findTestRomDirectory();
    if (!romRoot.isDirectory()) {
        std::cout << "[SKIP] Runtime external ROM loading sub-test skipped (VF50_TEST_ROM_DIR unset).\n";
        return true;
    }
    juce::File ic712, ic723, ic731, ic738;
    for (const auto& f : romRoot.findChildFiles(juce::File::findFiles, true, "*.bin;*.BIN;*.rom;*.ROM")) {
        juce::MemoryBlock mb;
        if (!f.loadFileAsData(mb)) {
            continue;
        }
        const auto chipOpt = vf50::rom::VF50RomSetBuilder::identifyRomBytes(
            static_cast<const uint8_t*>(mb.getData()), mb.getSize());
        if (!chipOpt.has_value()) {
            continue;
        }
        switch (*chipOpt) {
            case vf50::rom::RomChipId::MainCpuA_IC712: ic712 = f; break;
            case vf50::rom::RomChipId::MainCpuB_IC723: ic723 = f; break;
            case vf50::rom::RomChipId::SubCpu_IC731:   ic731 = f; break;
            case vf50::rom::RomChipId::DrumWave_IC738: ic738 = f; break;
            default: break;
        }
    }

    if (!ic712.existsAsFile() || !ic723.existsAsFile() || !ic731.existsAsFile() || !ic738.existsAsFile()) {
        std::cerr << "[FAIL] One or more canonical .BIN ROM images missing under VF50_TEST_ROM_DIR="
                  << romRoot.getFullPathName() << "\n";
        return false;
    }

    // 4a. Partial load step 1: drop IC738 alone -> staged in builder, drums wait safely for IC731 address table
    canvas.handleFilesDropped(juce::StringArray{ ic738.getFullPathName() });
    (void)renderAndMeasurePeak(*proc, {}, 1);
    if (!proc->romSetBuilder().hasChip(vf50::rom::RomChipId::DrumWave_IC738) ||
        proc->activeRomBank().hasDrumWaveRom) {
        std::cerr << "[FAIL] Expected IC738 alone to stage cleanly until IC731 sub-CPU ROM is loaded.\n";
        return false;
    }

    // 4b. Partial load step 2: add IC731 sub-CPU ROM -> 2/4 active (drums active before main CPU ROMs!)
    canvas.handleFilesDropped(juce::StringArray{ ic731.getFullPathName() });
    (void)renderAndMeasurePeak(*proc, {}, 1);

    if (!proc->activeRomBank().hasSubCpuRom || !proc->activeRomBank().hasDrumWaveRom ||
        proc->activeRomBank().loadedChipCount() != 2) {
        std::cerr << "[FAIL] Partial IC731+IC738 load failed: count=" << proc->activeRomBank().loadedChipCount() << "\n";
        return false;
    }
    {
        juce::MidiBuffer drumMidi;
        drumMidi.addEvent(juce::MidiMessage::noteOn(10, 36, uint8_t{ 120 }), 0);
        const float drumPeakLoaded = renderAndMeasurePeak(*proc, drumMidi, 6);
        if (drumPeakLoaded < 0.05f) {
            std::cerr << "[FAIL] Drum waveform did not synthesize audio after IC731+IC738 load (peak="
                      << drumPeakLoaded << ")\n";
            return false;
        }
        std::cout << "[PASS] Partial runtime IC731+IC738 load produced clean BD audio (peak=" << drumPeakLoaded << ")\n";
    }

    // 4c. Load remaining 2 Main-CPU EPROMs (IC712, IC723) via multi-file drop
    canvas.handleFilesDropped(juce::StringArray{
        ic712.getFullPathName(),
        ic723.getFullPathName()
    });
    (void)renderAndMeasurePeak(*proc, {}, 1);

    {
        const auto& rom = proc->activeRomBank();
        if (!rom.isAllRomsLoaded() || rom.loadedChipCount() != 4) {
            std::cerr << "[FAIL] Expected 4/4 active ROM chips after multi-file drop, got "
                      << rom.loadedChipCount() << "\n";
            return false;
        }
        if (std::string_view(rom.factoryVoices[0].name.data(), 10) != "Strings 1 " ||
            rom.defaultDrumSetup[0].name != "BD 1" ||
            rom.factoryDemoSongs[0].title != "Victory!" ||
            rom.factoryDemoSongs[0].eventCount == 0u ||
            rom.dspEffectNames[1] != "Reverb Hall") {
            std::cerr << "[FAIL] Decoded external ROM metadata mismatch after 4/4 load! "
                      << "v0='" << std::string(rom.factoryVoices[0].name.data(), 10)
                      << "' d0='" << rom.defaultDrumSetup[0].name
                      << "' song0='" << rom.factoryDemoSongs[0].title
                      << "' ev=" << rom.factoryDemoSongs[0].eventCount
                      << " fx1='" << rom.dspEffectNames[1] << "'\n";
            return false;
        }
    }

    canvas.setRomGuideVisible(true);
    const juce::File loadedSnap = buildDir.getChildFile("vf50_norom_loaded_1x.png");
    if (!savePngSnapshot(*editor, loadedSnap)) {
        std::cerr << "[FAIL] Failed to save vf50_norom_loaded_1x.png snapshot!\n";
        return false;
    }
    canvas.setRomGuideVisible(false);

    // 5. Verify Factory Demo Song "Victory!" audio + 10-track SMF Export after ROM load
    {
        proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        const float demoPeak = renderAndMeasurePeak(*proc, {}, 24);
        if (demoPeak < 0.01f) {
            std::cerr << "[FAIL] Demo Song 'Victory!' failed to synthesize after runtime ROM load!\n";
            return false;
        }
        proc->requestDemoStop();
        (void)renderAndMeasurePeak(*proc, {}, 2);

        const juce::File outMid = buildDir.getChildFile("vf50_norom_victory_export.mid");
        vf50::disk::MidiExportSummary expSum{};
        if (!vf50::disk::VF50MidiExporter::exportActiveSongToFile(*proc, outMid, &expSum) || !outMid.existsAsFile()) {
            std::cerr << "[FAIL] Failed to export 'Victory!' SMF after runtime ROM load: "
                      << expSum.statusMessage << "\n";
            return false;
        }
        outMid.deleteFile();
    }

    // 6. Verify Config Persistence across fresh VF50AudioProcessor construction
    {
        auto proc2 = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        if (!proc2->activeRomBank().isAllRomsLoaded() || proc2->activeRomBank().loadedChipCount() != 4 ||
            std::string_view(proc2->activeRomBank().factoryVoices[0].name.data(), 10) != "Strings 1 ") {
            std::cerr << "[FAIL] Second VF50AudioProcessor instance failed to auto-reload persisted ROM config!\n";
            return false;
        }
        std::cout << "[PASS] Persisted external ROM config auto-loaded 4/4 chips on new processor instance.\n";
    }

    // 7. Verify clearExternalRoms() returns cleanly to firmware-free INIT VOICE / silent drums
    {
        proc->clearExternalRoms();
        juce::MidiBuffer killMidi;
        for (int ch = 1; ch <= 16; ++ch) {
            killMidi.addEvent(juce::MidiMessage::allSoundOff(ch), 0);
        }
        (void)renderAndMeasurePeak(*proc, killMidi, 80); // Allow YM3413 reverb tail to decay completely
        const auto& romCleared = proc->activeRomBank();
        if (romCleared.isAllRomsLoaded() || romCleared.loadedChipCount() != 0 ||
            std::string_view(romCleared.factoryVoices[0].name.data(), 10) != "INIT VOICE") {
            std::cerr << "[FAIL] clearExternalRoms() did not restore clean firmware-free state!\n";
            return false;
        }
        juce::MidiBuffer drumMidi;
        drumMidi.addEvent(juce::MidiMessage::noteOn(10, 36, uint8_t{ 120 }), 0);
        const float silentAfterClear = renderAndMeasurePeak(*proc, drumMidi, 4);
        if (silentAfterClear < 0.0f || silentAfterClear > 1e-6f) {
            std::cerr << "[FAIL] Drums were not silenced after clearExternalRoms()! peak=" << silentAfterClear << "\n";
            return false;
        }
    }

    baseEditor.reset();
    proc.reset();
    vf50::rom::VF50RomSetBuilder::clearConfiguredPaths();

    std::cout << "[PASS] Firmware-Free & Runtime EPROM Loading Verification Complete (0 Errors).\n";
    return true;
}

[[nodiscard]] bool isPurePrintableAscii(const juce::String& s) {
    if (s.isEmpty()) {
        return false;
    }
    for (int i = 0; i < s.length(); ++i) {
        const juce::juce_wchar ch = s[i];
        if (ch < 32 || ch > 126) {
            return false;
        }
    }
    return true;
}

bool runLoadedSongAccompanyingRhythmSelfTest() {
    std::cout << "\n=== Running Loaded Song Accompanying Rhythm Playback Self-Test ===\n";

    const juce::File diskRoot = findTestDiskDirectory();
    if (!diskRoot.isDirectory()) {
        std::cout << "[SKIP] Loaded Song Accompanying Rhythm test skipped (VF50_TEST_DISK_DIR unset).\n";
        return true;
    }
    juce::File diskA1Dir = diskRoot.getChildFile("Disk A1");
    if (!diskA1Dir.isDirectory()) {
        diskA1Dir = diskRoot;
    }
    const juce::File a04File = diskA1Dir.getChildFile("AWESOME!.A04");
    const juce::File v11File = diskA1Dir.getChildFile("AWESOME!.V11");
    if (!a04File.existsAsFile() || !v11File.existsAsFile()) {
        std::cout << "[SKIP] Loaded Song Accompanying Rhythm test skipped (AWESOME!.A04 / AWESOME!.V11 not found in VF50_TEST_DISK_DIR).\n";
        return true;
    }

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(44100.0, 256);

    vf50::disk::VF50DiskManager dm;
    juce::String status;

    // 1. Load AWESOME!.A04 (8-track FM song with 0 embedded DrumHit events -> must play active Rhythm Pattern)
    if (!dm.loadFile(a04File, *proc, status)) {
        std::cerr << "[FAIL] Failed to load AWESOME!.A04: " << status << "\n";
        return false;
    }

    proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kCustomSongSlot);

    juce::AudioBuffer<float> audio(2, 256);
    juce::MidiBuffer midi;
    std::size_t maxDrumVoicesObserved = 0u;
    for (int block = 0; block < 40; ++block) {
        audio.clear();
        midi.clear();
        proc->processBlock(audio, midi);
        maxDrumVoicesObserved = std::max(maxDrumVoicesObserved, proc->synthEngine().drums().activeVoiceCount());
    }

    if (!proc->synthEngine().demoPlayer().isPlaying() ||
        !proc->synthEngine().demoPlayer().needsAccompanyingRhythmPattern() ||
        !proc->synthEngine().drums().isPatternPlaying() ||
        maxDrumVoicesObserved == 0u) {
        std::cerr << "[FAIL] AWESOME!.A04 playback did not trigger accompanying Rhythm Pattern! "
                  << "playing=" << proc->synthEngine().demoPlayer().isPlaying()
                  << " needsRhy=" << proc->synthEngine().demoPlayer().needsAccompanyingRhythmPattern()
                  << " ptnPlaying=" << proc->synthEngine().drums().isPatternPlaying()
                  << " maxDrumVoices=" << maxDrumVoicesObserved << "\n";
        return false;
    }
    std::cout << "[PASS] AWESOME!.A04 triggered accompanying Rhythm Pattern (max active drum voices="
              << maxDrumVoicesObserved << ").\n";

    // Stop must cleanly stop the accompanying Rhythm Pattern
    proc->requestDemoStop();
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    if (proc->synthEngine().demoPlayer().isPlaying() || proc->synthEngine().drums().isPatternPlaying()) {
        std::cerr << "[FAIL] requestDemoStop() did not stop accompanying Rhythm Pattern!\n";
        return false;
    }

    // 2. Load AWESOME!.V11 (has User Rhythm Patterns + HardwareSeqV1 -> must select User bank & play custom pattern)
    if (!dm.loadFile(v11File, *proc, status)) {
        std::cerr << "[FAIL] Failed to load AWESOME!.V11: " << status << "\n";
        return false;
    }
    if (proc->parameterBank().rhythmBank->load() < 0.5f) {
        std::cerr << "[FAIL] AWESOME!.V11 did not auto-select User Rhythm Bank!\n";
        return false;
    }
    proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kCustomSongSlot);
    std::size_t v11DrumVoices = 0u;
    for (int block = 0; block < 40; ++block) {
        audio.clear();
        midi.clear();
        proc->processBlock(audio, midi);
        v11DrumVoices = std::max(v11DrumVoices, proc->synthEngine().drums().activeVoiceCount());
    }
    if (!proc->synthEngine().drums().isPatternPlaying() ||
        !proc->synthEngine().drums().isUsingCustomPattern() ||
        v11DrumVoices == 0u) {
        std::cerr << "[FAIL] AWESOME!.V11 did not play loaded User Rhythm Pattern! "
                  << "ptnPlaying=" << proc->synthEngine().drums().isPatternPlaying()
                  << " customPtn=" << proc->synthEngine().drums().isUsingCustomPattern()
                  << " maxDrumVoices=" << v11DrumVoices << "\n";
        return false;
    }
    std::cout << "[PASS] AWESOME!.V11 played loaded User Rhythm Pattern alongside sequence (max active drum voices="
              << v11DrumVoices << ").\n";

    proc->requestDemoStop();
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    return true;
}

bool runSequencerManualWorkflowAndMuteSelfTest() {
    std::cout << "\n=== Running Sequencer VF50 Manual Workflow, Track Mute & Transport Self-Test ===\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(44100.0, 256);
    vf50::gui::VF50MainCanvas canvas(*proc);
    auto& lcd = canvas.lcdDisplay();

    // Load AWESOME!.A04 from VF50_TEST_DISK_DIR if set
    const juce::File diskRoot = findTestDiskDirectory();
    if (!diskRoot.isDirectory()) {
        std::cout << "[SKIP] Sequencer Manual Workflow & Mute test skipped (VF50_TEST_DISK_DIR unset).\n";
        return true;
    }
    juce::File diskA1Dir = diskRoot.getChildFile("Disk A1");
    if (!diskA1Dir.isDirectory()) {
        diskA1Dir = diskRoot;
    }
    const juce::File a04File = diskA1Dir.getChildFile("AWESOME!.A04");
    if (!a04File.existsAsFile()) {
        std::cout << "[SKIP] Sequencer Manual Workflow & Mute test skipped (AWESOME!.A04 not found in VF50_TEST_DISK_DIR).\n";
        return true;
    }

    vf50::disk::VF50DiskManager dm;
    juce::String status;
    if (!dm.loadFile(a04File, *proc, status)) {
        std::cerr << "[FAIL] Failed to load AWESOME!.A04: " << status << "\n";
        return false;
    }

    canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SequencerDemo);
    canvas.pressSeqOpButton(3); // [PLAY]

    juce::AudioBuffer<float> audio(2, 256);
    juce::MidiBuffer midi;
    for (int b = 0; b < 250; ++b) {
        audio.clear();
        midi.clear();
        proc->processBlock(audio, midi);
        if (b >= 30 && proc->synthEngine().fm().keyDownVoiceCount() > 0u) {
            break;
        }
    }

    canvas.refreshDisplayAndLeds();
    const juce::String playingRow0 = lcd.getLineAsString(0);
    const juce::String playingRow1 = lcd.getLineAsString(1);
    if (!playingRow0.startsWith("PLAY SONG)") || !playingRow0.contains("Meas") || !playingRow0.contains("Tempo") ||
        playingRow0.length() != 40 || playingRow1.length() != 40) {
        std::cerr << "[FAIL] Authentic Classic VF50 Sequencer LCD format mismatch: '"
                  << playingRow0 << "' / '" << playingRow1 << "' (len=" << playingRow1.length() << ")\n";
        return false;
    }

    // 1. Trigger toggleTrackMute(0) during playback -> must mute Track 1 & emit Note-Off / CC123 on Ch 1
    proc->toggleSequencerTrackMute(0u);
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);

    bool sawCh1NoteOff = false;
    for (const auto meta : midi) {
        const auto msg = meta.getMessage();
        if (msg.getChannel() == 1 && (msg.isNoteOff() || msg.isAllNotesOff())) {
            sawCh1NoteOff = true;
            break;
        }
    }
    if (!proc->isSequencerTrackMuted(0u) || !sawCh1NoteOff) {
        std::cerr << "[FAIL] Muting Track 1 did not emit Note-Off on Channel 1 (muted="
                  << proc->isSequencerTrackMuted(0u) << ", sawNoteOff=" << sawCh1NoteOff << ")\n";
        return false;
    }

    // 2. Verify TR 1 hardware LED & per-track meter reflect Track 1 mute cleanly
    canvas.refreshDisplayAndLeds();
    const auto mutedLeds = canvas.classicLedSnapshot();
    if (mutedLeds.tr[0] == vf50::classic::LedState::SolidGreen || proc->sequencerTrackMeterLevel(0u) != 0u) {
        std::cerr << "[FAIL] Muted Track 1 LED/meter mismatch (trk0Level="
                  << static_cast<int>(proc->sequencerTrackMeterLevel(0u)) << ")\n";
        return false;
    }
    std::cout << "[PASS] Track 1 mute emitted Channel 1 Note-Off and updated Classic TR 1 LED & meter.\n";

    // 3. Navigate from MODE_SEQUENCER to MODE_PERFORMANCE -> isPlaying() remains true, then return to SEQ for TR/RHY mutes!
    canvas.pressPerformanceButton();
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    if (!proc->isDemoPlaying()) {
        std::cerr << "[FAIL] Navigating to MODE_PERFORMANCE stopped sequencer playback!\n";
        return false;
    }
    canvas.pressSeqButton();
    canvas.pressEditMatrixButton(1); // TR 2 -> toggles Track 2 mute in SEQ mode
    canvas.pressEditMatrixButton(4); // RHY -> toggles Rhythm track mute in SEQ mode
    if (!proc->isSequencerTrackMuted(1u) || !proc->isSequencerTrackMuted(8u)) {
        std::cerr << "[FAIL] TR 2 / RHY buttons did not toggle mutes in SEQ mode while playing!\n";
        return false;
    }
    std::cout << "[PASS] MODE_PERFORMANCE preserved playback and SEQ TR/RHY mute buttons toggled cleanly.\n";

    // 4. Verify [FWD] / [BWD] / 1st [STOP] (retains measure) / [PLAY] (resumes) / 2nd [STOP] (rewinds to M001)
    canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SequencerDemo);
    canvas.pressSeqOpButton(4); // [FWD] +1 measure
    canvas.pressSeqOpButton(4); // [FWD] +1 measure
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    const int measAfterFwd = proc->activeDemoMeasure();
    if (measAfterFwd < 3) {
        std::cerr << "[FAIL] [FWD] did not advance measure (meas=" << measAfterFwd << ")\n";
        return false;
    }

    canvas.pressSeqOpButton(2); // 1st [STOP] -> halt, retain measure
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    const int measAfterStop1 = proc->activeDemoMeasure();
    if (proc->isDemoPlaying() || measAfterStop1 != measAfterFwd) {
        std::cerr << "[FAIL] 1st [STOP] did not retain measure (playing=" << proc->isDemoPlaying()
                  << ", meas=" << measAfterStop1 << " vs " << measAfterFwd << ")\n";
        return false;
    }

    canvas.pressSeqOpButton(3); // [PLAY] -> resume from retained measure (not reset to M001)
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    if (!proc->isDemoPlaying() || proc->activeDemoMeasure() < measAfterStop1) {
        std::cerr << "[FAIL] [PLAY] did not resume from retained measure (meas="
                  << proc->activeDemoMeasure() << " vs " << measAfterStop1 << ")\n";
        return false;
    }

    canvas.pressSeqOpButton(2); // Stop
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    canvas.pressStopAndBwdTogether(); // Authentic VF50 [STOP]+[BWD] -> Top (M001, Manual p. 87)
    audio.clear();
    midi.clear();
    proc->processBlock(audio, midi);
    if (proc->activeDemoMeasure() != 1) {
        std::cerr << "[FAIL] [STOP]+[BWD] did not rewind to M001 (meas=" << proc->activeDemoMeasure() << ")\n";
        return false;
    }
    std::cout << "[PASS] Transport [FWD]/[BWD]/1st [STOP] retain/[PLAY] resume/[STOP]+[BWD] Top M001 PASSED.\n";
    return true;
}

bool runGate1ModernClassicIsolationSelfTest() {
    std::cout << "\n=== Running Gate 1 Modern/Classic Isolation, Sanitizer & Sequencer Decoupling Self-Test ===\n";

    // Test 1: ASCII Sanitizer rejects non-ASCII/mojibake bytes and preserves clean .A## / .I## / .V## filenames
    {
        const uint8_t mojibakeBuf[14] = {
            'A', 'W', 'E', 'S', 'O', 'M', 'E', 0xE4u, 0xB8u, 0xADu, 0x01u, 0x7Fu, '!', 0xFFu
        };
        const juce::String cleanedMojibake = vf50::disk::VF50DiskManager::sanitizeAsciiString(mojibakeBuf, 14);
        if (!isPurePrintableAscii(cleanedMojibake)) {
            std::cerr << "[FAIL] Gate 1 Test 1: mojibake bytes leaked through sanitizeAsciiString: '"
                      << cleanedMojibake << "'\n";
            return false;
        }
        for (int i = 0; i < cleanedMojibake.length(); ++i) {
            if (cleanedMojibake[i] < 0x20 || cleanedMojibake[i] > 0x7E) {
                std::cerr << "[FAIL] Gate 1 Test 1: non-ASCII char code in cleaned output!\n";
                return false;
            }
        }

        const std::array<const char*, 3> cleanNames = { "AWESOME!.A04", "AWESOME.I02", "AWESOME!.V11" };
        for (const char* rawName : cleanNames) {
            const juce::String sanitized = vf50::disk::VF50DiskManager::sanitizeAsciiString(
                reinterpret_cast<const uint8_t*>(rawName),
                static_cast<int>(std::strlen(rawName)));
            if (sanitized != rawName || !isPurePrintableAscii(sanitized)) {
                std::cerr << "[FAIL] Gate 1 Test 1: clean hardware filename altered: '" << rawName
                          << "' -> '" << sanitized << "'\n";
                return false;
            }
            vf50::disk::DiskFileEntry entry{};
            entry.name = rawName;
            entry.kind = vf50::disk::VF50DiskManager::classifyFileKind(rawName, true);
            entry.sizeBytes = 19456u;
            if (entry.kind == vf50::disk::VF50FileKind::Unknown) {
                std::cerr << "[FAIL] Gate 1 Test 1: hardware filename failed classification: '" << rawName << "'\n";
                return false;
            }
        }
        std::cout << "[PASS] Gate 1 Test 1: ASCII sanitizer rejected mojibake & preserved .A04/.I02/.V11 filenames.\n";
    }

    auto locateA04File = []() -> juce::File {
        const juce::File diskRoot = findTestDiskDirectory();
        if (!diskRoot.isDirectory()) {
            return {};
        }
        const juce::File inSub = diskRoot.getChildFile("Disk A1").getChildFile("AWESOME!.A04");
        return inSub.existsAsFile() ? inSub : diskRoot.getChildFile("AWESOME!.A04");
    };

    // Test 2: Starting sequencer playback and switching SEQ -> PERFORMANCE -> SINGLE keeps playing & generating events
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(44100.0, 256);
        vf50::gui::VF50MainCanvas canvas(*proc);

        const juce::File a04 = locateA04File();
        if (a04.existsAsFile()) {
            vf50::disk::VF50DiskManager dm;
            juce::String status;
            if (!dm.loadFile(a04, *proc, status)) {
                std::cerr << "[FAIL] Gate 1 Test 2: failed to load AWESOME!.A04: " << status << "\n";
                return false;
            }
        } else {
            proc->selectDemoSong(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        }

        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SequencerDemo);
        if (a04.existsAsFile()) {
            canvas.pressKeypadDigitButton(1); // Select Song 1 (AWESOME!.A04 loads into active Song #1)
            canvas.pressSeqOpButton(3); // [PLAY]
        } else {
            proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        }

        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        for (int b = 0; b < 320; ++b) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if (b >= 30 && proc->sequencerDispatchedNoteCount() > 0u) {
                break;
            }
        }
        if (!proc->synthEngine().demoPlayer().isPlaying() || proc->sequencerDispatchedNoteCount() == 0u) {
            std::cerr << "[FAIL] Gate 1 Test 2: sequencer failed to start or generate initial events.\n";
            return false;
        }

        // Switch SEQ -> PERFORMANCE and verify consecutive audio blocks continue generating MIDI events
        canvas.pressPerformanceButton();
        const uint64_t notesBeforePerf = proc->sequencerDispatchedNoteCount();
        uint64_t notesMidPerf = notesBeforePerf;
        for (int b = 0; b < 120; ++b) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if (!proc->synthEngine().demoPlayer().isPlaying()) {
                std::cerr << "[FAIL] Gate 1 Test 2: sequencer stopped during PERFORMANCE block " << b << "\n";
                return false;
            }
            if (b == 59) {
                notesMidPerf = proc->sequencerDispatchedNoteCount();
            }
        }
        const uint64_t notesEndPerf = proc->sequencerDispatchedNoteCount();
        if (notesMidPerf <= notesBeforePerf || notesEndPerf <= notesMidPerf) {
            std::cerr << "[FAIL] Gate 1 Test 2: events stalled in PERFORMANCE mode ("
                      << notesBeforePerf << " -> " << notesMidPerf << " -> " << notesEndPerf << ")\n";
            return false;
        }

        // Switch PERFORMANCE -> SINGLE and verify playback continues
        canvas.pressSingleButton();
        const uint64_t notesBeforeSingle = proc->sequencerDispatchedNoteCount();
        for (int b = 0; b < 120; ++b) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if (!proc->synthEngine().demoPlayer().isPlaying()) {
                std::cerr << "[FAIL] Gate 1 Test 2: sequencer stopped during SINGLE block " << b << "\n";
                return false;
            }
        }
        if (proc->sequencerDispatchedNoteCount() <= notesBeforeSingle) {
            std::cerr << "[FAIL] Gate 1 Test 2: events stalled in SINGLE mode.\n";
            return false;
        }

        canvas.pressSeqOpButton(2); // [STOP]
        audio.clear();
        midi.clear();
        proc->processBlock(audio, midi);
        if (proc->synthEngine().demoPlayer().isPlaying()) {
            std::cerr << "[FAIL] Gate 1 Test 2: [STOP] in SINGLE Play mode did not halt sequencer.\n";
            return false;
        }
        std::cout << "[PASS] Gate 1 Test 2: Sequencer persisted across SEQ -> PERFORMANCE -> SINGLE ("
                  << notesBeforePerf << " -> " << notesEndPerf << " events) and stopped on [STOP].\n";
    }

    // Test 3: Unified Classic Workstation preserves all 5 host utilities, routes DISK to 40x2 LCD (0 popups),
    //         and keeps sequencer playback & voice state uninterrupted when toggling the PG Programmer drawer
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(44100.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> baseEditor(proc->createEditor());
        auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEditor.get());
        if (editor == nullptr) {
            std::cerr << "[FAIL] Gate 1 Test 3: failed to create VF50AudioProcessorEditor!\n";
            return false;
        }
        auto& canvas = editor->mainCanvas();

        if (proc->workflowMode() != vf50::state::OperatingMode::Classic
            || !canvas.isFloppyHostSlotVisible()
            || !canvas.isPreservedHostUtilitiesVisible()) {
            std::cerr << "[FAIL] Gate 1 Test 3: Unified Classic workstation missing preserved host utilities!\n";
            return false;
        }

        // Pressing panel DISK must ALWAYS open authentic 40x2 LCD Disk menu with zero OS popups
        canvas.resetFaceplatePopupSpawnCount();
        canvas.pressDiskButton();
        if (canvas.faceplatePopupSpawnCount() != 0u
            || !canvas.lcdDisplay().getLineAsString(0).startsWith("UT DISK)")) {
            std::cerr << "[FAIL] Gate 1 Test 3: Panel DISK button spawned popup or failed to open 40x2 LCD!\n";
            return false;
        }
        canvas.pressSingleButton();

        const int collapsedHeight = editor->getHeight();
        editor->setProgrammerVisible(true);
        if (!canvas.isProgrammerDrawerEffectivelyVisible() || editor->getHeight() <= collapsedHeight) {
            std::cerr << "[FAIL] Gate 1 Test 3: PG Programmer drawer failed to expand in Unified Classic mode!\n";
            return false;
        }
        const int expandedHeight = editor->getHeight();

        proc->selectSinglePresetAtomic(vf50::state::MemoryBank::FactoryRom, 7u);
        const auto expectedVoiceName = proc->hostMemory().activeSingleVoice().name;

        const juce::File a04 = locateA04File();
        if (a04.existsAsFile()) {
            vf50::disk::VF50DiskManager dm;
            juce::String status;
            if (!dm.loadFile(a04, *proc, status)) {
                std::cerr << "[FAIL] Gate 1 Test 3: failed to load AWESOME!.A04: " << status << "\n";
                return false;
            }
        } else {
            proc->selectDemoSong(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        }

        canvas.selectMenuPage(vf50::gui::VF50MainCanvas::MenuPage::SequencerDemo);
        if (a04.existsAsFile()) {
            canvas.pressKeypadDigitButton(1); // Select Song 1 (AWESOME!.A04)
            canvas.pressSeqOpButton(3); // [PLAY]
        } else {
            proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);
        }

        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        for (int b = 0; b < 320; ++b) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if (b >= 30 && proc->sequencerDispatchedNoteCount() > 0u) {
                break;
            }
        }
        const int activeSongBefore = proc->activeDemoSongIndex();
        const uint64_t notesBeforeToggle = proc->sequencerDispatchedNoteCount();
        if (!proc->synthEngine().demoPlayer().isPlaying() || notesBeforeToggle == 0u) {
            std::cerr << "[FAIL] Gate 1 Test 3: sequencer not playing before PG drawer toggle!\n";
            return false;
        }

        // Collapse PG Programmer drawer during playback -> preserves playback & active voice
        editor->setProgrammerVisible(false);
        if (canvas.isProgrammerDrawerEffectivelyVisible()
            || editor->getHeight() != collapsedHeight
            || proc->hostMemory().activeSingleVoice().name != expectedVoiceName) {
            std::cerr << "[FAIL] Gate 1 Test 3: collapsing PG drawer broke height or voice state!\n";
            return false;
        }

        for (int b = 0; b < 100; ++b) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if (!proc->synthEngine().demoPlayer().isPlaying()) {
                std::cerr << "[FAIL] Gate 1 Test 3: sequencer stopped after collapsing PG drawer!\n";
                return false;
            }
        }
        const uint64_t notesAfterCollapse = proc->sequencerDispatchedNoteCount();
        if (notesAfterCollapse <= notesBeforeToggle || proc->activeDemoSongIndex() != activeSongBefore) {
            std::cerr << "[FAIL] Gate 1 Test 3: sequencer events or song state broke after PG toggle!\n";
            return false;
        }

        // Expand PG Programmer drawer again -> height restores & playback continues
        editor->setProgrammerVisible(true);
        if (!canvas.isProgrammerDrawerEffectivelyVisible() || editor->getHeight() != expandedHeight) {
            std::cerr << "[FAIL] Gate 1 Test 3: re-expanding PG drawer did not restore height!\n";
            return false;
        }
        for (int b = 0; b < 100; ++b) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if (!proc->synthEngine().demoPlayer().isPlaying()) {
                std::cerr << "[FAIL] Gate 1 Test 3: sequencer stopped after re-expanding PG drawer!\n";
                return false;
            }
        }
        if (proc->sequencerDispatchedNoteCount() <= notesAfterCollapse) {
            std::cerr << "[FAIL] Gate 1 Test 3: sequencer events stalled after re-expanding PG drawer!\n";
            return false;
        }
        proc->requestDemoStop();
        audio.clear();
        midi.clear();
        proc->processBlock(audio, midi);
        std::cout << "[PASS] Gate 1 Test 3: Unified Classic host utilities, 40x2 LCD DISK (0 popups) & PG drawer continuity verified.\n";
    }

    return true;
}

bool runGate2ClassicModeWalkthroughSelfTests() {
    std::cout << "Running Gate 2 Classic Mode Ch. 1 / Ch. 5 / Ch. 6 Walkthrough Self-Tests...\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(44100.0, 256);
    vf50::gui::VF50MainCanvas canvas(*proc);
    auto& lcd = canvas.lcdDisplay();

    auto assert40x2 = [&](const char* label) -> bool {
        const juce::String r0 = lcd.getLineAsString(0);
        const juce::String r1 = lcd.getLineAsString(1);
        if (r0.length() != 40 || r1.length() != 40) {
            std::cerr << "[FAIL] " << label << ": LCD dimensions not 40x2 (r0=" << r0.length()
                      << ", r1=" << r1.length() << ")\n";
            return false;
        }
        return true;
    };

    // Unified Classic workstation (keep INT/CARD protect off at start unless testing protect)
    proc->setIntMemoryProtected(false);
    proc->setCardMemoryProtected(false);

    // =========================================================================
    // 1. CHAPTER 1: PLAY SINGLE, PF PERFORMANCE, [-] + F1 MUTE, VOICE EDIT e1111, COMPARE LOCK
    // =========================================================================
    canvas.pressSingleButton();
    if (!assert40x2("Ch1.2 PlaySingle")
        || !lcd.getLineAsString(0).startsWith("PLAY SINGLE")
        || !lcd.getLineAsString(0).contains("EFCT=")
        || !lcd.getLineAsString(0).contains("Pb=")
        || !lcd.getLineAsString(1).contains("Fs=sus")
        || canvas.classicLedSnapshot().single != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Ch1.2 PLAY SINGLE LCD/LED mismatch: r0='" << lcd.getLineAsString(0)
                  << "' r1='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Ch. 1.2 Single note hit -> TR1 BlinkGreen (~0.2 s, Manual p. 18)
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
        if (canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::BlinkGreen) {
            std::cerr << "[FAIL] Ch1.2 PLAY SINGLE note hit did not blink TR1 Green\n";
            return false;
        }
        audio.clear();
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        proc->processBlock(audio, midi);
        for (int t = 0; t < 12; ++t) {
            canvas.classicController().tick30Hz();
        }
    }

    // Bank-digit entry prompt: [PRESET] -> "P??", then 0, 5 -> "P05"
    canvas.pressMemBankButton(2);
    if (!lcd.getLineAsString(1).startsWith("P??")) {
        std::cerr << "[FAIL] Ch1.2 [PRESET] bank-digit prompt missing 'P??': '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(5);
    if (!lcd.getLineAsString(1).startsWith("P05")
        || canvas.classicLedSnapshot().memPreset != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Ch1.2 2-digit voice selection P05 failed: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Performance Play setup: ensure I00 Perf has active Part 0 (and Part 1) in both Full and NoROM
    {
        proc->pullLatestAudioMemoryToHost();
        auto perf = proc->hostMemory().getUserPerformance(0u);
        perf.instruments[0].maxNotes = 4u;
        perf.instruments[0].voiceBank = 1u;
        perf.instruments[0].voiceNumber = 5u;
        perf.instruments[0].receiveChannel = 16u;
        perf.instruments[0].limitLow = 0u;
        perf.instruments[0].limitHigh = 127u;
        perf.instruments[0].outAssign = 3u;
        perf.instruments[0].volume = 99u;
        proc->hostMemory().storeUserPerformance(0u, perf);
        proc->pushHostMemoryToAudio();
        proc->selectPerformancePresetAtomic(vf50::state::MemoryBank::UserRam, 0u, true);
    }

    canvas.pressPerformanceButton();
    if (!assert40x2("Ch1.3 PlayPerf")
        || !lcd.getLineAsString(0).startsWith("PF")
        || !lcd.getLineAsString(0).contains("EFCT=")
        || !lcd.getLineAsString(0).contains("Tch=")
        || !lcd.getLineAsString(1).contains("/")
        || canvas.classicLedSnapshot().performance != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Ch1.3 PF Performance display mismatch: r0='" << lcd.getLineAsString(0)
                  << "' r1='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Unmuted Inst 1 hit -> BlinkGreen on TR1 LED
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
        if (canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::BlinkGreen) {
            std::cerr << "[FAIL] Ch1.3 Unmuted Inst 1 hit did not light TR1 BlinkGreen\n";
            return false;
        }
        audio.clear();
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        proc->processBlock(audio, midi);
        for (int t = 0; t < 20; ++t) {
            canvas.classicController().tick30Hz();
        }
    }

    // [-] + F1 mutes Inst 1 -> "xxx" in LCD & note hit sets TR1 BlinkRed (p. 20)
    canvas.pressInstMinusButton();
    canvas.pressSoftKey(0);
    if (!lcd.getLineAsString(1).contains("xxx")
        || canvas.classicController().perfInstrumentMuteMask() != 0x01u) {
        std::cerr << "[FAIL] Ch1.4 [-] + F1 mute did not show 'xxx': '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
        if (canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::BlinkRed) {
            std::cerr << "[FAIL] Ch1.4 Muted Inst 1 hit did not trigger TR1 BlinkRed LED\n";
            return false;
        }
        audio.clear();
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        proc->processBlock(audio, midi);
    }

    // Switching Performance resets all instrument mutes (p. 20 Note)
    canvas.stepFocusedField(+1);
    if (canvas.classicController().perfInstrumentMuteMask() != 0u) {
        std::cerr << "[FAIL] Ch1.4 Selecting another Performance did not clear mute mask\n";
        return false;
    }

    // Ch. 1.5 Voice Edit "e1111" -> "e0101" + OP LEDs + Compare lock
    canvas.pressSingleButton();
    canvas.pressEditMatrixButton(0); // Quick Edit (Single Voice Edit)
    if (!lcd.getLineAsString(0).startsWith("e1111")
        || canvas.classicLedSnapshot().opSelect != vf50::classic::LedState::SolidGreen
        || canvas.classicLedSnapshot().opOnOff[0] != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Ch1.5 Voice Edit missing e1111 header or OP LEDs: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressSeqOpButton(1); // Toggle OP 1 OFF
    canvas.pressSeqOpButton(3); // Toggle OP 3 OFF
    canvas.pressSeqOpButton(0); // OP SELECT cycle
    const auto editLeds = canvas.classicLedSnapshot();
    if (!lcd.getLineAsString(0).startsWith("e0101")
        || editLeds.opOnOff[0] != vf50::classic::LedState::Off
        || editLeds.opOnOff[1] != vf50::classic::LedState::SolidGreen
        || editLeds.opOnOff[2] != vf50::classic::LedState::Off
        || editLeds.opOnOff[3] != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Ch1.5 Voice Edit operator toggle mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressSeqOpButton(1);
    canvas.pressSeqOpButton(3);

    // Compare lock blocks mode switching (p. 16)
    canvas.pressCompareButton();
    if (!canvas.classicController().isCompareModeLocked()
        || !lcd.getLineAsString(0).contains("[COMPARE]")
        || canvas.classicLedSnapshot().compare != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Ch1 Compare did not activate/lock properly\n";
        return false;
    }
    canvas.pressPerformanceButton();
    if (canvas.classicController().screen() == vf50::classic::ClassicScreen::PlayPerformance) {
        std::cerr << "[FAIL] Ch1 Compare mode lock allowed switching to Performance\n";
        return false;
    }
    canvas.pressCompareButton(); // Disengage Compare

    std::cout << "[PASS] Gate 2 Ch. 1: PLAY SINGLE, PF Perf, [-] + F1 BlinkRed mute, e1111 OP keys, Compare lock.\n";

    // =========================================================================
    // 2. CHAPTER 5: SEQUENCER PLAY SONG, TR MUTES, PERSISTENCE, TOP, SETUP PROMPT, JOB TREE
    // =========================================================================
    bool loadedRealA04 = false;
    {
        const juce::File diskRoot = findTestDiskDirectory();
        juce::File a04File = diskRoot.isDirectory() ? diskRoot.getChildFile("Disk A1").getChildFile("AWESOME!.A04") : juce::File{};
        if (diskRoot.isDirectory() && !a04File.existsAsFile()) {
            a04File = diskRoot.getChildFile("AWESOME!.A04");
        }
        if (a04File.existsAsFile()) {
            vf50::disk::VF50DiskManager a04Dm;
            juce::String a04Status;
            loadedRealA04 = a04Dm.loadFile(a04File, *proc, a04Status);
        }
        if (!loadedRealA04) {
            auto seqBuf = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
            seqBuf->clear();
            seqBuf->setTitle("GATE2SEQ");
            seqBuf->initialTempoBpm = 124u;
            seqBuf->timeSigNum = 4u;
            seqBuf->timeSigDen = 4u;
            seqBuf->ticksPerBar = 192u;
            seqBuf->totalBars = 8u;
            seqBuf->totalTicks = 1536u;
            seqBuf->trackChannels = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
            uint32_t evIdx = 0u;
            for (uint16_t b = 0u; b < 8u; ++b) {
                for (uint32_t sub = 0u; sub < 8u; ++sub) {
                    const uint32_t t0 = static_cast<uint32_t>(b) * 192u + sub * 24u;
                    seqBuf->events[evIdx++] = { t0,       vf50::generated::DemoEventType::FmNoteOn,  0u, 60u, 100u };
                    seqBuf->events[evIdx++] = { t0 + 18u, vf50::generated::DemoEventType::FmNoteOff, 0u, 60u, 0u };
                    seqBuf->events[evIdx++] = { t0,       vf50::generated::DemoEventType::FmNoteOn,  1u, 64u, 96u };
                    seqBuf->events[evIdx++] = { t0 + 18u, vf50::generated::DemoEventType::FmNoteOff, 1u, 64u, 0u };
                    if ((sub & 1u) == 0u) {
                        seqBuf->events[evIdx++] = { t0, vf50::generated::DemoEventType::DrumHit, 9u, 36u, 8u };
                    }
                }
            }
            seqBuf->eventCount = evIdx;
            seqBuf->valid = true;
            proc->publishCustomSequenceToAudio(*seqBuf);
        }
    }

    canvas.pressSeqButton();
    canvas.pressKeypadDigitButton(1); // Select Song 1 (AWESOME!.A04 / Custom .SEQ now lands in Song #1!)
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
    }

    if (!assert40x2("Ch5.1 SeqPlaySong")
        || !lcd.getLineAsString(0).startsWith("PLAY SONG)")
        || !lcd.getLineAsString(0).contains("Time")
        || !lcd.getLineAsString(0).contains(">Meas")
        || !lcd.getLineAsString(0).contains(">Tempo")
        || (loadedRealA04 && !lcd.getLineAsString(1).contains("AWESOME!"))) {
        std::cerr << "[FAIL] Ch5.1 PLAY SONG) layout mismatch: r0='" << lcd.getLineAsString(0)
                  << "' r1='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    {
        const auto seqLeds = canvas.classicLedSnapshot();
        if (seqLeds.seq != vf50::classic::LedState::SolidGreen
            || seqLeds.tr[0] != vf50::classic::LedState::SolidGreen
            || seqLeds.tr[1] != vf50::classic::LedState::SolidGreen
            || (!loadedRealA04 && seqLeds.tr[2] != vf50::classic::LedState::Off)
            || seqLeds.rhy != vf50::classic::LedState::SolidGreen) {
            std::cerr << "[FAIL] Ch5.2 Initial track data LEDs mismatch\n";
            return false;
        }
    }

    // Mute TR1 and RHY while stopped -> BlinkGreen LEDs, then unmute
    canvas.pressEditMatrixButton(0); // TR1
    canvas.pressEditMatrixButton(4); // RHY
    if (!proc->isSequencerTrackMuted(0u)
        || !proc->isSequencerTrackMuted(8u)
        || canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::BlinkGreen
        || canvas.classicLedSnapshot().rhy != vf50::classic::LedState::BlinkGreen) {
        std::cerr << "[FAIL] Ch5.2 TR1/RHY mute BlinkGreen LED failed\n";
        return false;
    }
    canvas.pressEditMatrixButton(0); // Unmute TR1
    canvas.pressEditMatrixButton(4); // Unmute RHY

    // Start sequencer playback until Track 1 is sounding, then mute TR1 live and verify Note-Off & silence
    canvas.pressSeqOpButton(3); // PLAY
    for (int i = 0; i < 200; ++i) {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
        if (i >= 15 && proc->sequencerTrackMeterLevel(0u) > 0u && proc->synthEngine().fm().keyDownVoiceCount() > 0u) {
            break;
        }
    }
    const uint64_t notesBeforeSwitch = proc->sequencerDispatchedNoteCount();
    if (!proc->isDemoPlaying() || notesBeforeSwitch == 0u || proc->sequencerTrackMeterLevel(0u) == 0u) {
        std::cerr << "[FAIL] Ch5.3 Sequencer failed to start/sound Track 1 notes\n";
        return false;
    }

    // Press TR1 while playing -> BlinkGreen LED + Channel 1 Note-Off/CC123 + subsequent Track 1 silence
    canvas.pressEditMatrixButton(0); // TR1 mute
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
        bool sawCh1NoteOff = false;
        for (const auto meta : midi) {
            const auto msg = meta.getMessage();
            if (msg.getChannel() == 1 && (msg.isNoteOff() || msg.isAllNotesOff())) {
                sawCh1NoteOff = true;
                break;
            }
        }
        if (!proc->isSequencerTrackMuted(0u)
            || canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::BlinkGreen
            || !sawCh1NoteOff) {
            std::cerr << "[FAIL] Ch5.2 Live TR1 mute failed to emit Ch 1 Note-Off / BlinkGreen\n";
            return false;
        }

        const uint64_t notesDuringMuteStart = proc->sequencerDispatchedNoteCount();
        for (int i = 0; i < 40; ++i) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if (proc->sequencerTrackMeterLevel(0u) != 0u) {
                std::cerr << "[FAIL] Ch5.2 Subsequent Track 1 notes were NOT silenced while muted\n";
                return false;
            }
        }
        if (proc->sequencerDispatchedNoteCount() <= notesDuringMuteStart) {
            std::cerr << "[FAIL] Ch5.2 Unmuted tracks stopped dispatching while Track 1 muted\n";
            return false;
        }
    }
    canvas.pressEditMatrixButton(0); // Unmute TR1

    const uint64_t notesBeforePerfSwitch = proc->sequencerDispatchedNoteCount();
    canvas.pressPerformanceButton();
    for (int i = 0; i < 40; ++i) {
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
    }
    if (!proc->isDemoPlaying()
        || proc->sequencerDispatchedNoteCount() <= notesBeforePerfSwitch
        || canvas.classicController().screen() != vf50::classic::ClassicScreen::PlayPerformance) {
        std::cerr << "[FAIL] Ch5.3 Sequencer stalled across SEQ -> PERFORMANCE\n";
        return false;
    }

    const uint64_t notesInPerf = proc->sequencerDispatchedNoteCount();
    canvas.pressSingleButton();
    for (int i = 0; i < 40; ++i) {
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
    }
    if (!proc->isDemoPlaying()
        || proc->sequencerDispatchedNoteCount() <= notesInPerf
        || canvas.classicController().screen() != vf50::classic::ClassicScreen::PlaySingle) {
        std::cerr << "[FAIL] Ch5.3 Sequencer stalled across PERFORMANCE -> SINGLE\n";
        return false;
    }

    // Return to SEQ, seek >= M002, STOP retains measure, PLAY resumes from retained measure, [STOP]+[BWD] -> Top 001
    canvas.pressSeqButton();
    canvas.pressSeqOpButton(4); // FWD measure
    {
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
    }
    canvas.pressSeqOpButton(2); // STOP
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
    }
    const int measRetainedOnStop = proc->activeDemoMeasure();
    if (proc->isDemoPlaying() || measRetainedOnStop < 2) {
        std::cerr << "[FAIL] Ch5.3 [STOP] did not halt retaining measure >= 2 (meas="
                  << measRetainedOnStop << ")\n";
        return false;
    }

    canvas.pressSeqOpButton(3); // PLAY resumes from retained measure
    if (!proc->isDemoPlaying() || proc->activeDemoMeasure() < measRetainedOnStop) {
        std::cerr << "[FAIL] Ch5.3 [PLAY] did not resume from retained measure\n";
        return false;
    }
    canvas.pressSeqOpButton(2); // STOP again
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
    }

    canvas.pressStopAndBwdTogether();
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
    }
    if (proc->activeDemoMeasure() != 1 || !lcd.getLineAsString(1).contains("001")) {
        std::cerr << "[FAIL] Ch5.3 [STOP]+[BWD] did not rewind to Top 001: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Store SETUP data to Song 1 -> switch song -> return -> "with SETUP" vs "Song only" prompt & mute reset
    canvas.pressStoreButton();
    if (!assert40x2("Ch5.4 StoreSetup")
        || !lcd.getLineAsString(0).startsWith("STORE)")
        || !lcd.getLineAsString(1).contains("Store SETUP data to song 1")) {
        std::cerr << "[FAIL] Ch5.4 STORE SETUP confirm mismatch: r0='" << lcd.getLineAsString(0)
                  << "' r1='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressKeypadYesButton(); // Stores setup for Song 1
    canvas.pressEditMatrixButton(0); // Mute TR1
    canvas.pressKeypadDigitButton(2); // Change to Song 2 -> defeats mutes (p. 87)
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
    }
    if (proc->isSequencerTrackMuted(0u)) {
        std::cerr << "[FAIL] Ch5.2 Changing song number did not clear TR1 mute\n";
        return false;
    }
    if (canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::Off) {
        std::cerr << "[FAIL] Ch5.2 Empty Song 2 TR1 LED expected Off after mute clear\n";
        return false;
    }
    canvas.pressKeypadDigitButton(1); // Selecting Song 1 with setup triggers prompt
    if (!assert40x2("Ch5.4 WithSetupPrompt")
        || !lcd.getLineAsString(1).contains("with SETUP")
        || !lcd.getLineAsString(1).contains("Song only")) {
        std::cerr << "[FAIL] Ch5.4 Missing 'with SETUP' / 'Song only' prompt: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(4); // Choose >Song only
    if (canvas.classicController().screen() != vf50::classic::ClassicScreen::SeqPlaySong) {
        std::cerr << "[FAIL] Ch5.4 Failed returning to PLAY SONG) after Song only\n";
        return false;
    }
    {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
        canvas.refreshDisplayAndLeds();
        if (canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::SolidGreen) {
            std::cerr << "[FAIL] Ch5.2 Song 1 TR1 LED did not remain unmuted SolidGreen\n";
            return false;
        }
    }

    // Walk Sequencer JOB tree: >Setup, >Tch, >Song, >Edit (all 7 sub-screens), >Rec -> STEP REC bar graph
    canvas.pressJobButton();
    if (!assert40x2("Ch5.5 JobSelect")
        || !lcd.getLineAsString(0).startsWith("SEQUENCER JOB SELECT)")
        || lcd.getLineAsString(1) != ">Setup>Tch>Song>Edit>Rec           >EXIT") {
        std::cerr << "[FAIL] Ch5.5 SEQUENCER JOB SELECT) mismatch: r1='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(0); // F1 >Setup
    if (!assert40x2("Ch5.6 Setup") || !lcd.getLineAsString(0).startsWith("SETUP)")) {
        std::cerr << "[FAIL] Ch5.6 SETUP) mismatch\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT

    canvas.pressSoftKey(1); // F2 >Tch
    if (!assert40x2("Ch5.7 Tch")) return false;
    canvas.pressJobButton(); // Back to JobSelect

    canvas.pressSoftKey(2); // F3 >Song
    if (!assert40x2("Ch5.8 SongName") || !lcd.getLineAsString(0).startsWith("SONG) NAME")) {
        std::cerr << "[FAIL] Ch5.8 SONG) NAME mismatch\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT

    canvas.pressSoftKey(3); // F4 >Edit
    if (!assert40x2("Ch5.9 EditTrack") || !lcd.getLineAsString(0).startsWith("EDIT TRACK)")) {
        std::cerr << "[FAIL] Ch5.9 EDIT TRACK) mismatch\n";
        return false;
    }
    static constexpr std::array<const char*, 7> kEditHdrs = {
        "TRACK MIX DOWN)", "QUANTIZE)", "DELETE)", "INSERT)", "COPY)", "ERASE)", "REMOVE EVENT)"
    };
    for (int sub = 0; sub < 7; ++sub) {
        canvas.pressSoftKey(sub);
        if (!assert40x2("Ch5.9 EditSub") || !lcd.getLineAsString(0).startsWith(kEditHdrs[static_cast<std::size_t>(sub)])) {
            std::cerr << "[FAIL] Ch5.9 Edit Sub[" << sub << "] header mismatch: '" << lcd.getLineAsString(0) << "'\n";
            return false;
        }
        canvas.pressSoftKey(7); // >EXIT back to EDIT TRACK)
    }
    canvas.pressSoftKey(7); // >EXIT back to SEQUENCER JOB SELECT)

    canvas.pressSoftKey(4); // F5 >Rec -> RECORD MODE)
    if (!assert40x2("Ch5.10 RecMode") || !lcd.getLineAsString(0).startsWith("RECORD MODE)")) {
        std::cerr << "[FAIL] Ch5.10 RECORD MODE) mismatch\n";
        return false;
    }
    canvas.stepFocusedField(+1); // Select Step Mode
    canvas.pressSoftKey(7); // >EXIT to JobSelect
    canvas.pressSoftKey(7); // >EXIT to PLAY SONG)

    canvas.pressSeqOpButton(0); // [RECORD] -> STEP REC)
    if (!assert40x2("Ch5.10 StepRec") || !lcd.getLineAsString(0).startsWith("STEP REC)")) {
        std::cerr << "[FAIL] Ch5.10 STEP REC) mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressSeqOpButton(3); // [PLAY] -> Bar graph
    if (!assert40x2("Ch5.10 StepBar")
        || !lcd.getLineAsString(0).startsWith("M001")
        || !lcd.getLineAsString(1).contains("GateTime")) {
        std::cerr << "[FAIL] Ch5.10 Step bar graph mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressSoftKey(3); // >GateTime
    if (!assert40x2("Ch5.10 GateSub")
        || !lcd.getLineAsString(1).contains("Norm")
        || !lcd.getLineAsString(1).contains("Stac")
        || !lcd.getLineAsString(1).contains("Slur")) {
        std::cerr << "[FAIL] Ch5.10 GateTime sub-view mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSeqOpButton(2); // [STOP] back to STEP REC)
    canvas.pressSeqOpButton(2); // [STOP] back to PLAY SONG)

    std::cout << "[PASS] Gate 2 Ch. 5: PLAY SONG), mutes, persistence, Top, Setup prompt, JOB tree & STEP REC.\n";

    // =========================================================================
    // 3. CHAPTER 6: PURE 40x2 DISK LCD WORKFLOW & MEMORY PROTECT (ZERO OS POPUPS)
    // =========================================================================
    canvas.resetFaceplatePopupSpawnCount();
    const juce::File gate2DiskDir = juce::File::getCurrentWorkingDirectory()
                                        .getChildFile("build")
                                        .getChildFile("gate2_disk_test");
    (void)gate2DiskDir.deleteRecursively();
    (void)gate2DiskDir.createDirectory();
    {
        juce::String st;
        if (!canvas.diskManager().mountDirectory(gate2DiskDir, st)) {
            std::cerr << "[FAIL] Ch6 Failed to mount writable test disk directory\n";
            return false;
        }
    }

    canvas.pressDiskButton();
    if (!assert40x2("Ch6.1 DiskRoot")
        || canvas.faceplatePopupSpawnCount() != 0u
        || !lcd.getLineAsString(0).startsWith("UT DISK)")
        || !lcd.getLineAsString(1).contains(">Save>Load>Del >Rename  >MDR >Dir  >Job")) {
        std::cerr << "[FAIL] Ch6.1 UT DISK) root mismatch or popup spawned: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Save ALL -> NEWFILE.ALL
    canvas.pressSoftKey(0); // >Save
    canvas.pressSoftKey(0); // ALL
    if (!assert40x2("Ch6.2 SaveAll") || !lcd.getLineAsString(0).startsWith("UT DISK) SAVE ALL")) {
        std::cerr << "[FAIL] Ch6.2 SAVE ALL header mismatch\n";
        return false;
    }
    canvas.pressSoftKey(7); // >GO
    canvas.pressKeypadYesButton(); // Confirm save
    if (canvas.diskManager().entries().size() != 1u) {
        std::cerr << "[FAIL] Ch6.2 SAVE ALL did not write file to FAT12 image\n";
        return false;
    }

    // Save SEQ -> NEWFILE.SEQ
    canvas.pressDiskButton(); // Exit disk
    canvas.pressDiskButton(); // Re-enter UT DISK)
    canvas.pressSoftKey(0);   // >Save
    canvas.pressSoftKey(2);   // SEQ
    canvas.pressSoftKey(7);   // >GO
    canvas.pressKeypadYesButton();
    if (canvas.diskManager().entries().size() != 2u) {
        std::cerr << "[FAIL] Ch6.2 SAVE SEQ did not write .SEQ file to FAT12 image\n";
        return false;
    }

    // Save R.SEQ -> NEWFILE.RHY (verify R.SEQ tag is not truncated in Save/Load/Dir)
    canvas.pressDiskButton(); // Exit disk
    canvas.pressDiskButton(); // Re-enter UT DISK)
    canvas.pressSoftKey(0);   // >Save
    canvas.pressSoftKey(3);   // R.SEQ
    if (!assert40x2("Ch6.2 SaveRSeq") || !lcd.getLineAsString(1).contains("(R.SEQ)")) {
        std::cerr << "[FAIL] Ch6.2 SAVE R.SEQ tag truncated: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(7);   // >GO
    canvas.pressKeypadYesButton();
    if (canvas.diskManager().entries().size() != 3u) {
        std::cerr << "[FAIL] Ch6.2 SAVE R.SEQ did not write .RHY file to FAT12 image\n";
        return false;
    }

    // >Dir (pure 40x2 LCD directory browser)
    canvas.pressDiskButton(); // Exit
    canvas.pressDiskButton(); // Re-enter UT DISK)
    canvas.pressSoftKey(6);   // F7 >Dir
    if (!assert40x2("Ch6.7 Dir")
        || !lcd.getLineAsString(0).startsWith("UT DISK) DIRECTORY")
        || !lcd.getLineAsString(0).contains("Total:  3 Files")
        || !lcd.getLineAsString(1).contains(":NEWFILE")) {
        std::cerr << "[FAIL] Ch6.7 Directory screen mismatch: r0='" << lcd.getLineAsString(0)
                  << "' r1='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.stepFocusedField(+1); // Entry 2 (.R01)
    if (!assert40x2("Ch6.7 DirRSeq") || !lcd.getLineAsString(1).contains("(R.SEQ)")) {
        std::cerr << "[FAIL] Ch6.7 Directory R.SEQ label truncated: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT to UT DISK)

    // Verify >Load -> R.SEQ displays untrimmed "(R.SEQ)" and preserves Performance LED context
    canvas.pressSoftKey(1); // >Load
    canvas.pressSoftKey(3); // R.SEQ
    if (!assert40x2("Ch6.3 LoadRSeq")
        || !lcd.getLineAsString(0).startsWith("UT DISK) LOAD R.SEQ")
        || !lcd.getLineAsString(1).contains("(R.SEQ)")) {
        std::cerr << "[FAIL] Ch6.3 Load R.SEQ tag mismatch: r0='" << lcd.getLineAsString(0)
                  << "' r1='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT to TypeSelect
    canvas.pressSoftKey(7); // >EXIT to UT DISK)

    // >Job -> >Status & >BackUp
    canvas.pressSoftKey(7); // F8 >Job
    canvas.pressSoftKey(4); // F5 >Status
    if (!assert40x2("Ch6.8 Status")
        || !lcd.getLineAsString(0).startsWith("UT DISK) STATUS")
        || !lcd.getLineAsString(1).contains("Total= 3files")
        || !lcd.getLineAsString(1).contains("Used=")
        || !lcd.getLineAsString(1).contains("Free=")) {
        std::cerr << "[FAIL] Ch6.8 STATUS mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT to DiskJobMenu

    canvas.pressSoftKey(2); // F3 >BackUp multi-step confirmation
    if (!lcd.getLineAsString(1).contains("Sure?")) return false;
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("original")) return false;
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("duplicate")) return false;
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("Backup completed")) return false;
    canvas.pressSoftKey(7); // >EXIT to DiskJobMenu
    canvas.pressSoftKey(7); // >EXIT to DiskRoot

    // >MDR -> >IntTime
    canvas.pressSoftKey(5); // F6 >MDR
    canvas.pressSoftKey(4); // F5 >IntTime
    if (!assert40x2("Ch6.6 MdrInt") || !lcd.getLineAsString(1).contains("Interval Time = 1 x 100 ms")) {
        std::cerr << "[FAIL] Ch6.6 MDR IntTime mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.stepFocusedField(+2);
    if (!lcd.getLineAsString(1).contains("Interval Time = 3 x 100 ms")) {
        std::cerr << "[FAIL] Ch6.6 MDR IntTime increment failed\n";
        return false;
    }
    canvas.pressDiskButton(); // Exit disk

    // [MEMORY PROTECT] ON -> Disk >Load ALL blocked ("Memory Protected"), >Load SEQ succeeds (p. 105 Note)!
    canvas.pressMemoryProtectButton();
    if (!assert40x2("Ch6.9 MemProtect") || !lcd.getLineAsString(0).startsWith("UT MEMORY PROTECT)")) {
        std::cerr << "[FAIL] Ch6.9 UT MEMORY PROTECT) mismatch\n";
        return false;
    }
    canvas.stepFocusedField(+1); // Set INT: on
    if (!proc->isIntMemoryProtected() || !lcd.getLineAsString(1).contains("on")) {
        std::cerr << "[FAIL] Ch6.9 Enabling INT memory protect failed\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT

    canvas.pressDiskButton(); // UT DISK)
    canvas.pressSoftKey(1);   // >Load
    canvas.pressSoftKey(0);   // ALL
    canvas.pressSoftKey(6);   // >GO
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("Memory Protected")) {
        std::cerr << "[FAIL] Ch6.3 Load ALL was NOT blocked when INT Memory Protect was ON: '"
                  << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT back to Load Type Select
    canvas.pressSoftKey(2); // SEQ (exempt from Memory Protect per p. 105 Note!)
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("Completed")) {
        std::cerr << "[FAIL] Ch6.3 Load SEQ failed while INT Memory Protect ON: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Turn Memory Protect OFF -> Load ALL succeeds
    canvas.pressMemoryProtectButton();
    canvas.stepFocusedField(-1); // INT: off
    canvas.pressSoftKey(7);      // >EXIT
    canvas.pressDiskButton();    // UT DISK)
    canvas.pressSoftKey(1);      // >Load
    canvas.pressSoftKey(0);      // ALL
    canvas.pressSoftKey(6);      // >GO
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("Completed")) {
        std::cerr << "[FAIL] Ch6.3 Load ALL failed after Memory Protect turned OFF: '"
                  << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // >Rename & >Del & >Format
    canvas.pressDiskButton();
    canvas.pressDiskButton();
    canvas.pressSoftKey(3); // >Rename
    canvas.pressSoftKey(0); // ALL
    canvas.pressSoftKey(1); // Edit filename cursor
    canvas.stepFocusedField(+1); // N -> O
    canvas.pressSoftKey(7); // >GO
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("Completed")) {
        std::cerr << "[FAIL] Ch6.5 Rename ALL failed: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    canvas.pressDiskButton();
    canvas.pressDiskButton();
    canvas.pressSoftKey(2); // >Del
    canvas.pressSoftKey(0); // ALL
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton();
    if (canvas.diskManager().entries().size() != 2u) {
        std::cerr << "[FAIL] Ch6.4 Delete ALL did not remove file\n";
        return false;
    }

    canvas.pressSoftKey(7); // Back to type select
    canvas.pressSoftKey(7); // Back to UT DISK)
    canvas.pressSoftKey(7); // >Job
    canvas.pressSoftKey(0); // >Format
    canvas.pressKeypadYesButton();
    if (!canvas.diskManager().entries().empty()) {
        std::cerr << "[FAIL] Ch6.8 Disk Format did not clear entries\n";
        return false;
    }

    (void)gate2DiskDir.deleteRecursively();

    if (canvas.faceplatePopupSpawnCount() != 0u) {
        std::cerr << "[FAIL] Ch6 Classic Disk workflow spawned OS popup/dialog ("
                  << canvas.faceplatePopupSpawnCount() << ")\n";
        return false;
    }

    std::cout << "[PASS] Gate 2 Ch. 6: Pure 40x2 LCD Disk workflow (Save/Load/MemProt/Del/Rename/MDR/Dir/Job, 0 popups) passed.\n";
    return true;
}

bool runGate4AClassicEditAndBufferSelfTests() {
    std::cout << "Running Gate 4A Classic Mode Ch. 2 / Ch. 3 / Ch. 6 Buffer Utilities Walkthrough Tests...\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(44100.0, 256);
    vf50::gui::VF50MainCanvas canvas(*proc);
    auto& lcd = canvas.lcdDisplay();

    auto assert40x2 = [&](const char* label) -> bool {
        const juce::String r0 = lcd.getLineAsString(0);
        const juce::String r1 = lcd.getLineAsString(1);
        if (r0.length() != 40 || r1.length() != 40) {
            std::cerr << "[FAIL] " << label << ": LCD dimensions not 40x2 (r0=" << r0.length()
                      << ", r1=" << r1.length() << ")\n";
            return false;
        }
        return true;
    };

    auto pumpAudio = [&]() {
        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        proc->processBlock(audio, midi);
    };

    proc->setIntMemoryProtected(false);
    proc->setCardMemoryProtected(false);
    canvas.resetFaceplatePopupSpawnCount();

    // =========================================================================
    // 1. EDIT BUFFER ISOLATION, RECALL, INIT & PRESET READ-ONLY PROTECTION
    // =========================================================================
    canvas.pressSingleButton();
    canvas.pressMemBankButton(2); // PRESET
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(0); // P00
    const uint8_t origP00Alg = proc->hostMemory().getFactoryVoice(0u).algorithm;
    const uint8_t origI00Alg = proc->hostMemory().getUserVoice(0u).algorithm;

    canvas.pressEditMatrixButton(1); // TR2 ALG>
    if (!assert40x2("Ch3.2 ALG") || !lcd.getLineAsString(0).startsWith("e1111 ALG>")) {
        std::cerr << "[FAIL] Ch3.2 ALG> LCD mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    const uint8_t targetAlg = static_cast<uint8_t>((origP00Alg + 3u) & 0x07u);
    while (proc->hostMemory().singleEditSlot().voice.algorithm != targetAlg) {
        canvas.stepFocusedField((targetAlg > proc->hostMemory().singleEditSlot().voice.algorithm) ? +1 : -1);
    }
    pumpAudio();
    if (proc->hostMemory().getFactoryVoice(0u).algorithm != origP00Alg
        || proc->hostMemory().getUserVoice(0u).algorithm != origI00Alg
        || proc->synthEngine().fm().activeSingleVoice().algorithm != targetAlg) {
        std::cerr << "[FAIL] Gate 4A Edit Buffer Isolation failed\n";
        return false;
    }

    // Select another voice P05 without storing -> Recover edited VMEM via UT OTHERS> VOICE -> >Recall
    canvas.pressSingleButton();
    canvas.pressMemBankButton(2);
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(5); // P05
    canvas.pressUtilityOthersButton();
    if (!assert40x2("Ch6 UtOthersVoice") || !lcd.getLineAsString(0).startsWith("UT  OTHERS> VOICE")) {
        std::cerr << "[FAIL] Ch6 UT OTHERS> VOICE mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressSoftKey(1); // F2 >Recall
    canvas.pressKeypadYesButton();
    pumpAudio();
    if (proc->hostMemory().singleEditSlot().voice.algorithm != targetAlg
        || proc->synthEngine().fm().activeSingleVoice().algorithm != targetAlg) {
        std::cerr << "[FAIL] Ch6 Voice Recall did not recover edited VMEM buffer\n";
        return false;
    }

    // Voice >Init
    canvas.pressUtilityOthersButton();
    canvas.pressSoftKey(0); // F1 >Init
    canvas.pressKeypadYesButton();
    if (std::string_view(proc->hostMemory().singleEditSlot().voice.name) != "INIT VOICE") {
        std::cerr << "[FAIL] Ch6 Voice >Init did not reset to INIT VOICE\n";
        return false;
    }

    // =========================================================================
    // 2. MEMORY PROTECT, STORE/COPY, AUTO RE-ENABLE & MULTI-VOICE EDIT
    // =========================================================================
    // Disable OP1 & OP3 (e0101), enable INT protect, test Store + "Prot?" bypass -> auto-restores e1111
    canvas.pressEditMatrixButton(5); // EG
    canvas.pressSeqOpButton(1); // OP1 off
    canvas.pressSeqOpButton(3); // OP3 off
    if (!lcd.getLineAsString(0).startsWith("e0101")) {
        std::cerr << "[FAIL] Ch3 OP1/OP3 off mask mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }

    canvas.pressEditMatrixButton(1); // TR2 ALG> so [STORE/COPY] enters StoreVoice, not EG COPY>
    proc->setIntMemoryProtected(true);
    canvas.pressStoreButton();
    if (!assert40x2("Ch6 StoreVoiceProt")
        || !lcd.getLineAsString(1).contains("Mem Store")
        || !lcd.getLineAsString(1).contains("Prot?")) {
        std::cerr << "[FAIL] Ch6 StoreVoice Prot? prompt mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(5); // Target I05
    canvas.pressKeypadYesButton(); // Blocked by INT protect
    if (!lcd.getLineAsString(1).contains("Memory Protected")) {
        std::cerr << "[FAIL] Ch6 StoreVoice was not blocked by INT protect: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(7); // F8 below "Prot?" temporarily defeats INT protect
    canvas.pressKeypadYesButton();
    if (proc->hostMemory().operatorEnableMask() != 0x0Fu
        || proc->hostMemory().selectedVoiceBank() != vf50::state::MemoryBank::UserRam
        || proc->hostMemory().selectedVoiceIndex() != 5u) {
        std::cerr << "[FAIL] Ch6 StoreVoice failed to commit or re-enable OP 1..4\n";
        return false;
    }

    // Card protect + hardware card write-protect cannot be bypassed
    proc->setCardMemoryProtected(true);
    canvas.pressEditMatrixButton(1); // ALG>
    canvas.pressStoreButton();
    canvas.pressMemBankButton(1); // CARD
    if (lcd.getLineAsString(1).contains("Prot?")) {
        std::cerr << "[FAIL] Ch6 Card Store offered Prot? bypass illegally\n";
        return false;
    }
    canvas.pressKeypadYesButton();
    if (!lcd.getLineAsString(1).contains("Memory Protected")) {
        std::cerr << "[FAIL] Ch6 Card Store was not blocked by CARD protect\n";
        return false;
    }
    canvas.pressKeypadNoButton(); // Cancel back
    proc->setIntMemoryProtected(false);
    proc->setCardMemoryProtected(false);

    // EG COPY> (copies AR/D1R/D1L/D2R/RR/LS/RS, excludes SHFT)
    canvas.pressEditMatrixButton(5); // EG
    canvas.pressSeqOpButton(0); // Select OP2 (index 1)
    // Set OP2 AR=20, SHFT=48 (index 1)
    canvas.pressSoftKey(0); // AR
    while (proc->hostMemory().singleEditSlot().voice.operators[1].ar != 20u) {
        canvas.stepFocusedField((proc->hostMemory().singleEditSlot().voice.operators[1].ar < 20u) ? +1 : -1);
    }
    canvas.pressSoftKey(7); // SHFT
    canvas.stepFocusedField(+1); // 48
    // Select OP3 (index 2) and keep SHFT=off, AR=31
    canvas.pressSeqOpButton(0); // OP3
    // Verify OP1 SHFT is locked to off
    canvas.pressSeqOpButton(0); // OP4
    canvas.pressSeqOpButton(0); // OP1
    canvas.pressSoftKey(7); // SHFT on OP1
    canvas.stepFocusedField(+1);
    if (proc->hostMemory().singleEditSlot().voice.operators[0].egShift != 0u) {
        std::cerr << "[FAIL] Ch3.6 OP1 EG Shift was NOT locked to off\n";
        return false;
    }
    // Copy EG OP2 -> OP3
    canvas.pressSeqOpButton(0); // OP2
    canvas.pressStoreButton(); // EG COPY>
    if (!assert40x2("Ch6 EgCopy") || !lcd.getLineAsString(0).startsWith("EG COPY>")) {
        std::cerr << "[FAIL] Ch6 EG COPY> screen mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressKeypadDigitButton(3); // Dst OP3
    canvas.pressKeypadYesButton();
    if (proc->hostMemory().singleEditSlot().voice.operators[2].ar != 20u
        || proc->hostMemory().singleEditSlot().voice.operators[2].egShift != 0u) {
        std::cerr << "[FAIL] Ch6 EG COPY> failed to copy AR or wrongly copied SHFT\n";
        return false;
    }

    // =========================================================================
    // 3. CHAPTER 3 VOICE EDIT FULL WALKTHROUGH + COMPARE LOCKOUT
    // =========================================================================
    // Quick Edit 4-press cycle (ATTACK/RELEAS/VOLUME/BRIL)
    canvas.pressEditMatrixButton(0); // TR1 Quick
    for (int p = 0; p < 4; ++p) {
        if (!assert40x2("Ch3.1 Quick") || !lcd.getLineAsString(0).contains("QUICK>")) return false;
        if (p == 2) { // VOLUME (carriers only)
            const uint8_t beforeOp0 = proc->hostMemory().singleEditSlot().voice.operators[0].outputLevel;
            canvas.stepFocusedField(-5);
            if (proc->hostMemory().singleEditSlot().voice.operators[0].outputLevel == beforeOp0
                && beforeOp0 >= 5u) {
                std::cerr << "[FAIL] Ch3.1 Quick VOLUME did not mutate carrier level\n";
                return false;
            }
        }
        canvas.pressEditMatrixButton(0);
    }

    // Verify OP 1-4 ON/OFF does not disrupt active Sequencer playback
    {
        auto opSeq = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        opSeq->clear();
        opSeq->valid = true;
        opSeq->setTitle("OPTEST  ");
        opSeq->initialTempoBpm = 120u;
        opSeq->ticksPerBar = 192u;
        opSeq->totalBars = 4u;
        opSeq->totalTicks = 768u;
        opSeq->events[0] = { 0u, vf50::generated::DemoEventType::FmNoteOn, 0u, 60u, 100u };
        opSeq->events[1] = { 96u, vf50::generated::DemoEventType::FmNoteOff, 0u, 60u, 0u };
        opSeq->eventCount = 2u;
        proc->publishUserSequenceToSlot(0u, *opSeq);
    }
    canvas.pressSeqButton();
    canvas.pressSeqOpButton(3); // PLAY
    pumpAudio();
    canvas.pressSingleButton();
    canvas.pressEditMatrixButton(2); // TR3 LFO>
    canvas.pressSeqOpButton(1); // OP1 off during active seq playback
    if (!proc->isDemoPlaying() || (proc->hostMemory().operatorEnableMask() & 1u) != 0u) {
        std::cerr << "[FAIL] Ch3 OP ON/OFF disrupted active sequencer or failed to toggle OP1\n";
        return false;
    }
    canvas.pressSeqOpButton(1); // OP1 back on
    canvas.pressSingleButton();
    canvas.pressSeqOpButton(2); // Stop seq from Single Play

    // Walk TR3 (LFO), TR4 (SENS), RHY (OSC ratio & fix), TR5 (PEG), TR6 (OUT LEVEL), TR7 (SCALING), TR8 (TRANSPOSE), FUNCTION (all 8 submenus), EFFECT (3 presses)
    canvas.pressEditMatrixButton(2); // TR3 LFO
    if (!assert40x2("Ch3.3 LFO") || !lcd.getLineAsString(0).contains("LFO>")) return false;
    canvas.pressEditMatrixButton(3); // TR4 SENS
    if (!assert40x2("Ch3.4 SENS") || !lcd.getLineAsString(0).contains("SENS>")) return false;
    canvas.pressEditMatrixButton(4); // RHY OSC ratio
    if (!assert40x2("Ch3.5 OscRatio") || !lcd.getLineAsString(1).contains("ratio")) return false;
    canvas.pressSoftKey(0); // MODE
    canvas.stepFocusedField(+1); // fix
    if (!assert40x2("Ch3.5 OscFix") || !lcd.getLineAsString(1).contains("fix")) {
        std::cerr << "[FAIL] Ch3.5 OSC fix layout mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.stepFocusedField(-1); // ratio
    canvas.pressEditMatrixButton(6); // TR5 PEG
    if (!assert40x2("Ch3.7 PEG") || !lcd.getLineAsString(0).contains("PEG>")) return false;
    canvas.pressEditMatrixButton(7); // TR6 OUT LEVEL
    if (!assert40x2("Ch3.7 OutLvl") || !lcd.getLineAsString(0).contains("OUT LEVEL>")) return false;
    canvas.pressEditMatrixButton(8); // TR7 SCALING RATE
    if (!assert40x2("Ch3.7 ScalRate") || !lcd.getLineAsString(1).contains("RATE")) return false;
    canvas.pressEditMatrixButton(8); // TR7 SCALING LEVEL
    if (!assert40x2("Ch3.7 ScalLvl") || !lcd.getLineAsString(1).contains("LEVEL")) return false;
    canvas.pressEditMatrixButton(9); // TR8 TRANSPOSE
    if (!assert40x2("Ch3.7 Trans") || !lcd.getLineAsString(0).contains("TRANSPOSE>")) return false;

    // FUNCTION 8 submenus
    canvas.pressOthersFuncEditButton();
    if (!assert40x2("Ch3.8 FuncRoot") || !lcd.getLineAsString(1).startsWith(">PBetc>Port")) {
        std::cerr << "[FAIL] Ch3.8 FUNCTION root mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    static constexpr std::array<const char*, 8> kFuncHdrs = {
        "P.BEND>", "PORTA>", "FC>", "MW>", "BC>", "AT>", "REVERB RATE>", "VOICE NAME>"
    };
    for (int f = 0; f < 8; ++f) {
        canvas.pressSoftKey(f);
        if (!assert40x2("Ch3.8 FuncSub")
            || !lcd.getLineAsString(0).contains(kFuncHdrs[static_cast<std::size_t>(f)])
            || !lcd.getLineAsString(1).contains(">EXIT")) {
            std::cerr << "[FAIL] Ch3.8 FUNCTION sub[" << f << "] mismatch: r0='" << lcd.getLineAsString(0)
                      << "' r1='" << lcd.getLineAsString(1) << "'\n";
            return false;
        }
        canvas.pressSoftKey(7); // >EXIT
    }

    // Voice EFFECT 3-press cycle (skips pages 2-3 when Effect=off)
    canvas.pressEffectEditButton();
    if (!assert40x2("Ch3.9 Fx1") || !lcd.getLineAsString(0).contains("EFCT>")) return false;
    // Set Effect = 1 (Reverb Hall) so all 3 presses are reachable
    canvas.pressSoftKey(0);
    while (proc->hostMemory().singleEditSlot().voice.effect.effectType != 1u) {
        canvas.stepFocusedField((proc->hostMemory().singleEditSlot().voice.effect.effectType < 1u) ? +1 : -1);
    }
    canvas.pressEffectEditButton(); // Press 2: OutLevel / StereoMix
    if (!assert40x2("Ch3.9 Fx2") || !lcd.getLineAsString(0).contains("OutLevel")) return false;
    canvas.pressEffectEditButton(); // Press 3: Effect Params
    if (!assert40x2("Ch3.9 Fx3") || !lcd.getLineAsString(0).contains("Time")) return false;

    // Voice Compare on TR2 ALG> (c1111, editing locked, mode switch locked)
    canvas.pressEditMatrixButton(1); // TR2 ALG>
    const uint8_t editedAlg = proc->hostMemory().singleEditSlot().voice.algorithm;
    canvas.pressCompareButton();
    if (!lcd.getLineAsString(0).startsWith("c1111")
        || canvas.classicLedSnapshot().compare != vf50::classic::LedState::SolidGreen
        || canvas.classicLedSnapshot().single != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Ch6 Voice Compare did not switch e1111 -> c1111: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.stepFocusedField(+1); // Should be locked
    if (proc->hostMemory().singleEditSlot().voice.algorithm != editedAlg) {
        std::cerr << "[FAIL] Ch6 Voice Compare allowed parameter edit while locked\n";
        return false;
    }
    canvas.pressPerformanceButton(); // Should be locked
    if (canvas.classicController().screen() == vf50::classic::ClassicScreen::PlayPerformance) {
        std::cerr << "[FAIL] Ch6 Voice Compare allowed mode switch\n";
        return false;
    }
    canvas.pressCompareButton(); // Disengage Compare

    // =========================================================================
    // 4. CHAPTER 2 PERFORMANCE EDIT + BUFFER UTILITIES (INIT SEQ4/SEQ8, MULTI-VOICE M1111)
    // =========================================================================
    canvas.pressPerformanceButton();
    canvas.pressUtilityOthersButton();
    if (!assert40x2("Ch6 UtOthersPfm") || !lcd.getLineAsString(0).startsWith("UT  OTHERS> PFM")) return false;
    canvas.pressSoftKey(0); // >Init
    if (!assert40x2("Ch6 PfmInit") || lcd.getLineAsString(1) != "SNGL DUAL SPLT 4LYR 8LYR SEQ4 SEQ8 >EXIT") {
        std::cerr << "[FAIL] Ch6 PFM INIT template menu mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSoftKey(5); // SEQ4
    canvas.pressKeypadYesButton();
    if (proc->hostMemory().perfEditSlot().perf.instruments[3].maxNotes == 0u
        || proc->hostMemory().perfEditSlot().perf.instruments[4].maxNotes != 0u) {
        std::cerr << "[FAIL] Ch6 PFM INIT SEQ4 did not configure 4 active instruments\n";
        return false;
    }

    canvas.pressUtilityOthersButton();
    canvas.pressSoftKey(0); // >Init
    canvas.pressSoftKey(6); // SEQ8
    canvas.pressKeypadYesButton();
    if (proc->hostMemory().perfEditSlot().perf.instruments[7].maxNotes == 0u) {
        std::cerr << "[FAIL] Ch6 PFM INIT SEQ8 did not configure 8 active instruments\n";
        return false;
    }

    // Performance Edit: temporary [-] + F1 mute -> BlinkRed, cleared on Store
    canvas.pressEditMatrixButton(0); // TR1 AssignMode
    if (!assert40x2("Ch2.1 AssignMode") || !lcd.getLineAsString(0).startsWith("P.ED")) return false;
    canvas.pressInstMinusButton();
    canvas.pressSoftKey(0); // Mute Inst 1
    if (canvas.classicLedSnapshot().tr[0] != vf50::classic::LedState::BlinkRed) {
        std::cerr << "[FAIL] Ch2 [-] + F1 mute in Perf Edit did not set TR1 BlinkRed\n";
        return false;
    }
    // Performance Compare (P.ED <-> c.ED)
    canvas.pressCompareButton();
    if (!lcd.getLineAsString(0).startsWith("c.ED")) {
        std::cerr << "[FAIL] Ch6 Perf Compare did not show c.ED: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressCompareButton();

    // Store clears temporary Perf instrument mutes
    canvas.pressStoreButton();
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(2); // I02
    canvas.pressKeypadYesButton();
    if (canvas.classicController().perfInstrumentMuteMask() != 0u) {
        std::cerr << "[FAIL] Ch2/Ch6 Performance Store did not unmute instruments\n";
        return false;
    }

    // Walk all 12 Performance Edit screens + all 32 DSP effect parameter pages
    canvas.pressEditMatrixButton(1); // TR2 MAX NOTES>
    if (!assert40x2("Ch2.2 Notes") || !lcd.getLineAsString(0).contains("MAX NOTES>")) return false;
    canvas.pressEditMatrixButton(2); // TR3 VOICE NO> + Internal perf blocks Card voice
    if (!assert40x2("Ch2.3 VoiceNo") || !lcd.getLineAsString(0).contains("VOICE NO>")) return false;
    canvas.pressMemBankButton(1); // CARD should be rejected inside Internal Perf
    if (!lcd.getLineAsString(1).contains("Card Voice")) {
        std::cerr << "[FAIL] Ch2.3 Internal Perf did not reject CARD voice\n";
        return false;
    }
    canvas.pressEditMatrixButton(3); // TR4 RECV CH>
    if (!assert40x2("Ch2.4 RecvCh")) return false;
    canvas.pressRhyEditButton();     // RHY LIMIT/LOW>
    if (!assert40x2("Ch2.4 LimLow")) return false;
    canvas.pressLimitHighEditButton(); // LIMIT/HIGH>
    if (!assert40x2("Ch2.4 LimHi")) return false;
    canvas.pressEditMatrixButton(6); // TR5 DETUNE
    if (!assert40x2("Ch2.4 Detune")) return false;
    canvas.pressEditMatrixButton(7); // TR6 NOTE SHIFT
    if (!assert40x2("Ch2.4 Shift")) return false;
    canvas.pressEditMatrixButton(8); // TR7 VOLUME
    if (!assert40x2("Ch2.4 Volume")) return false;
    canvas.pressEditMatrixButton(9); // TR8 OUT ASSIGN
    if (!assert40x2("Ch2.4 OutAssign")) return false;

    // Perf OTHERS 4-press cycle + PFM EFFECT COPY>
    for (int p = 0; p < 4; ++p) {
        canvas.pressOthersFuncEditButton();
        if (!assert40x2("Ch2.5 Others")) return false;
    }
    // Press 4 (PFM EFCT SEL): select delay1, copy to delay2
    canvas.stepFocusedField(+1); // delay1
    canvas.pressStoreButton(); // PFM EFFECT COPY>
    if (!assert40x2("Ch6 PfmFxCopy") || !lcd.getLineAsString(0).startsWith("PFM EFFECT COPY>")) return false;
    canvas.pressKeypadDigitButton(2);
    canvas.pressKeypadYesButton();

    // Perf EFFECT 4-press cycle across all 32 effects + EFFECT BYPASS SolidRed LED
    canvas.pressEffectEditButton(); // Press 1 (Inst Effect On/Off)
    canvas.pressEffectEditButton(); // Press 2 (Select & Balance)
    if (proc->hostMemory().perfEditSlot().perf.effect.effectType == 0u) {
        canvas.pressSoftKey(0);
        canvas.stepFocusedField(+1); // Select Effect 1 (Reverb Hall) so pages 3-4 are active
    }
    canvas.pressEffectEditButton(); // Press 3 (OutLevel & StereoMix)
    canvas.pressEffectEditButton(); // Press 4 (Params)
    for (int fxType = 1; fxType <= 32; ++fxType) {
        proc->hostMemory().perfEditSlot().perf.effect.effectType = static_cast<uint8_t>(fxType);
        canvas.refreshDisplayAndLeds();
        if (!assert40x2("Ch2.6 All32Fx")) return false;
    }
    canvas.pressEffectBypassButton();
    if (!lcd.getLineAsString(0).contains("BYPASS")
        || canvas.classicLedSnapshot().effectBypass != vf50::classic::LedState::SolidRed) {
        std::cerr << "[FAIL] Ch2.6 EFFECT BYPASS LCD/SolidRed mismatch\n";
        return false;
    }
    canvas.pressEffectBypassButton(); // Restore bypass off

    // Multi-Voice Edit (M1111) + "** go to SGL mode Sure?" cancel warning
    canvas.pressUtilityOthersButton();
    canvas.pressSoftKey(3); // >VoiceEdit
    if (!assert40x2("Ch6 MultiVoiceSel") || !lcd.getLineAsString(0).contains("VOICE EDIT")) return false;
    canvas.pressSoftKey(0); // Edit Inst 1 voice -> M1111
    if (!assert40x2("Ch6 M1111") || !lcd.getLineAsString(0).startsWith("M1111")) {
        std::cerr << "[FAIL] Ch6 Multi-Voice Edit missing M1111 prefix: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.stepFocusedField(+1); // Dirty multi-voice edit
    canvas.pressSingleButton();  // Should trigger "** go to SGL mode Sure?"
    if (!lcd.getLineAsString(1).contains("go to SGL mode Sure?")) {
        std::cerr << "[FAIL] Ch6 Multi-Voice Edit exit warning missing: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressKeypadYesButton(); // Confirm exit to Single Play

    if (canvas.faceplatePopupSpawnCount() != 0u) {
        std::cerr << "[FAIL] Gate 4A spawned OS popup/dialog (" << canvas.faceplatePopupSpawnCount() << ")\n";
        return false;
    }

    std::cout << "[PASS] Gate 4A: Classic Mode Ch. 2 (Perf Edit), Ch. 3 (Voice Edit), & Ch. 6 Buffer Utilities passed!\n";
    return true;
}

bool runGate4BClassicRhythmMachineSelfTests() {
    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 512);

    vf50::gui::VF50MainCanvas canvas(*proc);
    canvas.setSize(vf50::gui::tokens::kVirtualWidth, vf50::gui::tokens::kVirtualHeight);
    canvas.resetFaceplatePopupSpawnCount();

    const auto& lcd = canvas.lcdDisplay();
    auto& classic = canvas.classicController();
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;

    auto pump = [&](int numBlocks) -> double {
        double sumSq = 0.0;
        for (int b = 0; b < numBlocks; ++b) {
            audio.clear();
            midi.clear();
            proc->processBlock(audio, midi);
            if ((b % 3) == 0) {
                classic.tick30Hz();
            }
            const float* L = audio.getReadPointer(0);
            const float* R = audio.getReadPointer(1);
            for (int i = 0; i < 512; ++i) {
                sumSq += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
            }
        }
        canvas.refreshDisplayAndLeds();
        return std::sqrt(sumSq / static_cast<double>(std::max(1, numBlocks) * 1024));
    };

    auto assert40x2 = [&](const char* tag) -> bool {
        const auto s0 = lcd.getLineAsString(0);
        const auto s1 = lcd.getLineAsString(1);
        if (s0.length() != 40 || s1.length() != 40) {
            std::cerr << "[FAIL] " << tag << " LCD width != 40 (" << s0.length() << ", " << s1.length() << ")\n";
            return false;
        }
        return true;
    };

    // =========================================================================
    // Test 1: Pattern Play, >KBD Routing, Queuing & Mode Continuity (pp. 57-59)
    // =========================================================================
    canvas.pressRhythmButton();
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmPtnPlay) {
        std::cerr << "[FAIL] Gate4B Test1: [RHYTHM] did not enter RhythmPtnPlay\n";
        return false;
    }
    canvas.pressMemBankButton(2); // [PRESET]
    canvas.pressKeypadDigitButton(1);
    canvas.pressKeypadDigitButton(0); // P10
    if (!assert40x2("T1 PTN PLAY>") || !lcd.getLineAsString(0).contains("PTN PLAY>")
        || !lcd.getLineAsString(1).contains(">toSONG") || !lcd.getLineAsString(1).contains("RHY")) {
        std::cerr << "[FAIL] Gate4B Test1: PTN PLAY> layout mismatch: '" << lcd.getLineAsString(0) << "' / '"
                  << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    if (!lcd.getLineAsString(1).contains("P10w")) {
        std::cerr << "[FAIL] Gate4B Test1: P10w indicator missing: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Verify >KBD RHY <-> SYN routing
    (void)pump(2);
    if (!proc->synthEngine().isRhythmKeyboardMode()) {
        std::cerr << "[FAIL] Gate4B Test1: KBD=RHY did not enable rhythmKeyboardMode\n";
        return false;
    }
    canvas.pressSoftKey(7); // F8 >KBD -> SYN
    (void)pump(2);
    if (proc->synthEngine().isRhythmKeyboardMode() || !lcd.getLineAsString(1).contains("SYN")) {
        std::cerr << "[FAIL] Gate4B Test1: KBD=SYN did not disable rhythmKeyboardMode\n";
        return false;
    }
    canvas.pressSoftKey(7); // Back to RHY
    (void)pump(2);

    // Start PLAY -> >toSONG hides, rhythm triggers fire, TR1-TR8 reflect hits, queue P11
    canvas.pressSeqOpButton(3); // [PLAY]
    const double rmsPlay = pump(20);
    if (!proc->isRhythmEnginePlaying() || lcd.getLineAsString(1).contains(">toSONG")) {
        std::cerr << "[FAIL] Gate4B Test1: PLAY did not start or hide >toSONG: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    if (rmsPlay <= 1e-4 || proc->rhythmTotalTriggerCount() == 0u) {
        std::cerr << "[FAIL] Gate4B Test1: No rhythm audio/triggers during Pattern Play\n";
        return false;
    }
    bool anyTrLit = false;
    const auto ledsPlay = canvas.classicLedSnapshot();
    if (ledsPlay.rhythm != vf50::classic::LedState::SolidGreen) {
        std::cerr << "[FAIL] Gate4B Test1: RHYTHM LED not SolidGreen during play\n";
        return false;
    }
    for (int t = 0; t < 8; ++t) {
        if (ledsPlay.tr[static_cast<std::size_t>(t)] != vf50::classic::LedState::Off) {
            anyTrLit = true;
        }
    }
    if (!anyTrLit) {
        std::cerr << "[FAIL] Gate4B Test1: No TR1-TR8 LEDs lit during rhythm playback\n";
        return false;
    }

    // Queue next pattern P11 mid-playback & pump across 2-bar wrap (P10 is 2 bars = 384 ticks)
    canvas.pressSoftKey(1); // Focus Pattern
    canvas.pressKeypadYesButton(); // Queue P11
    if (!classic.isRhythmPatternQueued()) {
        std::cerr << "[FAIL] Gate4B Test1: Pattern queuing did not arm P11\n";
        return false;
    }
    (void)pump(420); // > 4.4s ensures 2-bar P10 finishes and switches to P11
    if (classic.isRhythmPatternQueued() || !lcd.getLineAsString(1).contains("P11")) {
        std::cerr << "[FAIL] Gate4B Test1: Queued pattern P11 did not activate at loop boundary: '"
                  << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Switch to PERFORMANCE while Rhythm plays -> playback continues uninterrupted!
    canvas.pressPerformanceButton();
    (void)pump(4);
    if (classic.screen() != vf50::classic::ClassicScreen::PlayPerformance
        || !proc->isRhythmEnginePlaying()
        || proc->synthEngine().isRhythmKeyboardMode()) {
        std::cerr << "[FAIL] Gate4B Test1: Switching to PERFORMANCE broke rhythm continuity or KBD routing\n";
        return false;
    }
    canvas.pressSeqOpButton(2); // [STOP] from PERFORMANCE stops Rhythm
    (void)pump(2);
    if (proc->isRhythmEnginePlaying()) {
        std::cerr << "[FAIL] Gate4B Test1: [STOP] in PERFORMANCE did not stop Rhythm\n";
        return false;
    }

    // =========================================================================
    // Test 2: Preset Step Record Protection (P00 -> Store to I00, pp. 60-65)
    // =========================================================================
    canvas.pressRhythmButton();
    canvas.pressMemBankButton(2); // [PRESET]
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(0); // P00
    const auto p00BeforeCount = proc->activeRomBank().factoryRhythmPatterns[0].eventCount;
    const auto p00BeforeTicks = proc->activeRomBank().factoryRhythmPatterns[0].totalTicks;

    if (proc->hostMemory().userRhythmPattern(0u).eventCount != 0u) {
        std::cerr << "[FAIL] Gate4B Test2: Internal pattern I00 should start empty without 'w' flag\n";
        return false;
    }

    canvas.pressSeqOpButton(0); // [RECORD] -> P.REC> setup
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmPtnRecSetup || !assert40x2("T2 P.REC>")) {
        std::cerr << "[FAIL] Gate4B Test2: [RECORD] did not open P.REC>\n";
        return false;
    }
    canvas.pressSoftKey(0); // Toggle Real -> Step
    canvas.pressSeqOpButton(3); // [PLAY] -> Bar Graph Step Recording
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmPtnRecStep
        || !assert40x2("T2 BarGraph")
        || !lcd.getLineAsString(0).contains("Bar1:")
        || !lcd.getLineAsString(0).contains("|")
        || !lcd.getLineAsString(1).contains("ptnP00")) {
        std::cerr << "[FAIL] Gate4B Test2: Step Bar Graph layout invalid: '"
                  << lcd.getLineAsString(0) << "' / '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Add BD 1 hit at beat 1 with Acc=7
    constexpr uint8_t kTestBd1Note = 40u;
    constexpr uint8_t kTestSd1Note = 47u;
    canvas.sendRhythmKeyNoteOn(kTestBd1Note, 100, false, false);
    canvas.pressSoftKey(4); // Focus >Acc
    for (int i = 0; i < 7; ++i) {
        canvas.pressKeypadYesButton();
    }
    if (!lcd.getLineAsString(1).contains("Acc=7") || !lcd.getLineAsString(0).contains("*")) {
        std::cerr << "[FAIL] Gate4B Test2: Step Record Acc=7 or hit marker missing: '"
                  << lcd.getLineAsString(0) << "' / '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Hold F4 + snare key (SD 1) switches instrument without recording
    const auto sdBeforeCount = classic.stepSelectedDrumIndex();
    canvas.sendRhythmKeyNoteOn(kTestSd1Note, 110, false, true);
    if (classic.stepSelectedDrumIndex() != 6 || sdBeforeCount == 6) {
        std::cerr << "[FAIL] Gate4B Test2: Hold inst softkey + key did not switch to SD 1\n";
        return false;
    }

    // Step forward by 1/32 via [FWD], verify ERASE removes temporary hit, and keypad '0' -> Qntz=off (#1)
    canvas.pressSeqOpButton(4); // [FWD] -> tick 6
    canvas.sendRhythmKeyNoteOn(kTestSd1Note, 100, false, false);
    canvas.sendRhythmKeyNoteOn(kTestSd1Note, 100, true, false); // ERASE
    canvas.pressKeypadDigitButton(0); // Qntz=off -> Bar1:#1
    if (!lcd.getLineAsString(0).contains("#1")) {
        std::cerr << "[FAIL] Gate4B Test2: Qntz=off sub-step indicator missing: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }

    // Press [STOP] -> P.REC> Store Pattern prompt (Preset protected!)
    canvas.pressSeqOpButton(2); // [STOP]
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmPtnRecStore
        || !assert40x2("T2 StorePrompt")
        || !lcd.getLineAsString(0).contains("Store Pattern")
        || !lcd.getLineAsString(1).contains("ptnI")) {
        std::cerr << "[FAIL] Gate4B Test2: Store Pattern prompt missing: '"
                  << lcd.getLineAsString(0) << "' / '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    // Attempt [PRESET] should be blocked
    canvas.pressMemBankButton(2);
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmPtnRecStore) {
        std::cerr << "[FAIL] Gate4B Test2: Preset store should remain blocked\n";
        return false;
    }
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(0); // I00
    canvas.pressSoftKey(6); // >GO
    if (!lcd.getLineAsString(0).contains("Are you sure")) {
        std::cerr << "[FAIL] Gate4B Test2: Store Pattern confirmation missing\n";
        return false;
    }
    canvas.pressKeypadYesButton(); // Confirm store to I00
    for (int t = 0; t < 50; ++t) {
        classic.tick30Hz();
    }
    canvas.refreshDisplayAndLeds();
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmPtnPlay
        || !lcd.getLineAsString(1).contains("I00w")) {
        std::cerr << "[FAIL] Gate4B Test2: Stored I00w not shown after store: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    const auto& i00Ptn = proc->hostMemory().userRhythmPattern(0u);
    const auto* bdHit = i00Ptn.findEventAtTick(0u, 0u);
    if (proc->activeRomBank().factoryRhythmPatterns[0].eventCount != p00BeforeCount
        || proc->activeRomBank().factoryRhythmPatterns[0].totalTicks != p00BeforeTicks
        || i00Ptn.eventCount == 0u
        || bdHit == nullptr
        || bdHit->velocityLevel != 8u) {
        std::cerr << "[FAIL] Gate4B Test2: P00 modified or I00 missing BD1 Acc=7 hit\n";
        return false;
    }

    // =========================================================================
    // Test 3: Pattern Jobs (>COPY, >Clr, >Setup, >Inst, >Asgn, pp. 66-70)
    // =========================================================================
    // Seed two 2-bar 4/4 patterns (I10, I11) and one 2-bar 3/4 pattern (I12)
    auto& p10u = proc->hostMemory().mutableUserRhythmPattern(10u);
    p10u.clear();
    p10u.bars = 2u; p10u.timeSigNum = 4u; p10u.timeSigDen = 4u; p10u.totalTicks = 384u;
    (void)p10u.addEventSorted(0u, 0u, 7u);
    auto& p11u = proc->hostMemory().mutableUserRhythmPattern(11u);
    p11u.clear();
    p11u.bars = 2u; p11u.timeSigNum = 4u; p11u.timeSigDen = 4u; p11u.totalTicks = 384u;
    (void)p11u.addEventSorted(48u, 6u, 6u);
    auto& p12u = proc->hostMemory().mutableUserRhythmPattern(12u);
    p12u.clear();
    p12u.bars = 2u; p12u.timeSigNum = 3u; p12u.timeSigDen = 4u; p12u.totalTicks = 288u;
    (void)p12u.addEventSorted(0u, 0u, 6u);

    canvas.pressJobButton(); // PATTERN JOB SELECT)
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmPtnJobSelect
        || !assert40x2("T3 PTN JOB")
        || !lcd.getLineAsString(0).contains("PATTERN JOB SELECT)")
        || !lcd.getLineAsString(1).contains(">Copy>Clr>Setup>Inst>Asgn")) {
        std::cerr << "[FAIL] Gate4B Test3: PATTERN JOB SELECT) mismatch: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // >Copy concatenate I10 + I11 -> I01 (4 bars)
    canvas.pressSoftKey(0); // >Copy
    canvas.pressSoftKey(0); // Src1
    canvas.pressMemBankButton(0); // INT
    for (int i = 0; i < 10; ++i) canvas.pressKeypadYesButton(); // I10
    canvas.pressSoftKey(2); // Src2
    canvas.pressMemBankButton(0); // INT (enables Src2 at I00)
    for (int i = 0; i < 11; ++i) canvas.pressKeypadYesButton(); // I11
    canvas.pressSoftKey(4); // Dst
    canvas.pressKeypadYesButton(); // I01
    canvas.pressSoftKey(6); // >GO
    if (!lcd.getLineAsString(0).contains("Are you sure")) {
        std::cerr << "[FAIL] Gate4B Test3: PTN COPY> 2+2=4 bar confirmation failed: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressKeypadYesButton(); // Execute copy into I01
    const auto& i01Ptn = proc->hostMemory().userRhythmPattern(1u);
    if (i01Ptn.bars != 4u || i01Ptn.totalTicks != 768u || i01Ptn.findEventAtTick(384u + 48u, 6u) == nullptr) {
        std::cerr << "[FAIL] Gate4B Test3: Concatenated I01 invalid (bars=" << static_cast<int>(i01Ptn.bars)
                  << ", totalTicks=" << i01Ptn.totalTicks << ")\n";
        return false;
    }

    // Reject >4-bar concatenation (I01 [4 bars] + I10 [2 bars])
    canvas.pressSoftKey(0); // Src1
    for (int i = 0; i < 9; ++i) canvas.pressKeypadNoButton(); // I10 -> I01
    canvas.pressSoftKey(2); // Src2
    canvas.pressKeypadNoButton(); // I11 -> I10
    canvas.pressSoftKey(6); // >GO (must reject >4 bars)
    if (lcd.getLineAsString(0).contains("Are you sure")) {
        std::cerr << "[FAIL] Gate4B Test3: PTN COPY> allowed >4 bar concatenation!\n";
        return false;
    }

    // Reject mismatched time signature (I10 [4/4] + I12 [3/4])
    canvas.pressSoftKey(0);
    for (int i = 0; i < 9; ++i) canvas.pressKeypadYesButton(); // I10
    canvas.pressSoftKey(2);
    canvas.pressKeypadYesButton(); // I11
    canvas.pressKeypadYesButton(); // I12 (3/4)
    canvas.pressSoftKey(6); // >GO (must reject mismatch)
    if (lcd.getLineAsString(0).contains("Are you sure")) {
        std::cerr << "[FAIL] Gate4B Test3: PTN COPY> allowed mismatched time signatures!\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT back to PATTERN JOB SELECT)

    // >Setup + >Inst + >Asgn
    canvas.pressSoftKey(2); // >Setup
    if (!assert40x2("T3 SETUP>") || !lcd.getLineAsString(0).contains("SETUP>")) return false;
    canvas.pressSoftKey(6); // >Sync
    canvas.pressKeypadYesButton(); // MIDI
    if (!lcd.getLineAsString(1).contains("MIDI")) {
        std::cerr << "[FAIL] Gate4B Test3: SETUP> Sync MIDI failed\n";
        return false;
    }
    canvas.pressKeypadNoButton(); // int
    canvas.pressSoftKey(7); // >EXIT

    canvas.pressSoftKey(3); // >Inst
    if (!assert40x2("T3 INST SET>") || !lcd.getLineAsString(0).contains("INST SET>")) return false;
    canvas.pressSoftKey(3); // >Pan
    canvas.pressKeypadYesButton(); // +1 pan
    canvas.pressSoftKey(6); // >Efct
    canvas.pressKeypadNoButton(); // cycle efct
    canvas.pressSoftKey(7); // >EXIT

    canvas.pressSoftKey(4); // >Asgn
    if (!assert40x2("T3 RHYTHM ASSIGN>") || !lcd.getLineAsString(1).contains("PRESET1")) {
        std::cerr << "[FAIL] Gate4B Test3: RHYTHM ASSIGN> initial screen mismatch\n";
        return false;
    }
    canvas.pressKeypadYesButton(); // PRESET2
    if (!lcd.getLineAsString(1).contains("PRESET2")) return false;
    canvas.pressKeypadYesButton(); // PRESET3
    if (!lcd.getLineAsString(1).contains("PRESET3")) return false;
    canvas.pressKeypadYesButton(); // USER 1
    if (!lcd.getLineAsString(1).contains("USER 1")) return false;

    // Customize USER 1: C1 (keyIdx 0, MIDI 36) = SD 1 (6), C#1 (keyIdx 1, MIDI 37) = none (-1)
    auto u1Map = proc->hostMemory().userKeyMap(0u);
    u1Map[0] = 6;
    u1Map[1] = -1;
    proc->hostMemory().setUserKeyMap(0u, u1Map);
    // Step USER 1 -> PRESET3 -> USER 1 to push updated map cleanly via UI controls
    canvas.pressKeypadNoButton();
    canvas.pressKeypadYesButton();
    (void)pump(2);
    if (proc->synthEngine().drums().drumForMidiNote(36) != 6
        || proc->synthEngine().drums().drumForMidiNote(37) != -1) {
        std::cerr << "[FAIL] Gate4B Test3: USER 1 custom key assign did not apply to Ym3602DrumEngine (36="
                  << proc->synthEngine().drums().drumForMidiNote(36) << ", 37="
                  << proc->synthEngine().drums().drumForMidiNote(37) << ")\n";
        return false;
    }
    // Restore PRESET1
    for (int i = 0; i < 3; ++i) canvas.pressKeypadNoButton();
    (void)pump(2);
    canvas.pressSoftKey(7); // >EXIT to PATTERN JOB SELECT)
    canvas.pressSoftKey(7); // >EXIT to PTN PLAY>

    // =========================================================================
    // Test 4: Rhythm Song Creation, Repeats, Marks & Playback Sync (pp. 71-77)
    // =========================================================================
    canvas.pressSoftKey(0); // >toSONG -> SONG PLAY)
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmSongPlay
        || !assert40x2("T4 SONG PLAY)")
        || !lcd.getLineAsString(0).contains("SONG PLAY)")
        || !lcd.getLineAsString(1).contains(">toPTN")) {
        std::cerr << "[FAIL] Gate4B Test4: SONG PLAY) layout mismatch: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }

    canvas.pressJobButton(); // SONG JOB SELECT)
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmSongJobSelect
        || !lcd.getLineAsString(0).contains("SONG JOB SELECT)")
        || !lcd.getLineAsString(1).contains(">Edit>Copy>Clr")) {
        std::cerr << "[FAIL] Gate4B Test4: SONG JOB SELECT) mismatch\n";
        return false;
    }

    canvas.pressSoftKey(0); // >Edit -> SONG EDIT)
    // Part 1: ||: (RepeatBegin)
    canvas.pressSoftKey(5); // right -> creates Pattern part
    canvas.pressSoftKey(5); // cycles to ||:
    if (!lcd.getLineAsString(0).contains("||:")) {
        std::cerr << "[FAIL] Gate4B Test4: Part 1 ||: missing: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    // Part 2: PTN I00
    canvas.pressSoftKey(3); // next part
    canvas.pressSoftKey(5); // creates Pattern part (default P00)
    canvas.pressMemBankButton(0); // INT -> I00
    // Part 3: :|| x 2
    canvas.pressSoftKey(3);
    canvas.pressSoftKey(5); // Pattern
    canvas.pressSoftKey(5); // ||:
    canvas.pressSoftKey(5); // :|| x 2
    if (!lcd.getLineAsString(0).contains(":|| x 2")) {
        std::cerr << "[FAIL] Gate4B Test4: Part 3 :|| x 2 missing: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    // Part 4: Vol -10
    canvas.pressSoftKey(3);
    canvas.pressSoftKey(5); // Pattern
    canvas.pressSoftKey(5); // ||:
    canvas.pressSoftKey(5); // :||
    canvas.pressSoftKey(5); // Vol+0
    for (int i = 0; i < 10; ++i) canvas.pressKeypadNoButton(); // Vol-10
    // Part 5: Mark [A]
    canvas.pressSoftKey(3);
    for (int i = 0; i < 6; ++i) canvas.pressSoftKey(5); // wraps to Mark [A]
    if (!lcd.getLineAsString(0).contains("Mark [A]")) {
        std::cerr << "[FAIL] Gate4B Test4: Part 5 Mark [A] missing: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    // Part 6: PTN I00
    canvas.pressSoftKey(3);
    canvas.pressSoftKey(5);
    canvas.pressMemBankButton(0); // INT -> I00

    // Test SONG ED JOB) >Name + >Search
    canvas.pressSoftKey(6); // >JOB -> SONG ED JOB)
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmSongEditJob
        || !lcd.getLineAsString(1).contains(">Part>Ins >Del>Copy>Search>Name")) {
        std::cerr << "[FAIL] Gate4B Test4: SONG ED JOB) mismatch\n";
        return false;
    }
    canvas.pressSoftKey(4); // >Search
    if (!lcd.getLineAsString(1).contains("[A]")) {
        std::cerr << "[FAIL] Gate4B Test4: SONG ED JOB) >Search missing [A]\n";
        return false;
    }
    canvas.pressSoftKey(0); // Select [A] -> jumps to Part 5 in SONG EDIT)
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmSongEdit
        || !lcd.getLineAsString(0).contains("Part 005")) {
        std::cerr << "[FAIL] Gate4B Test4: Mark [A] jump did not land on Part 005: '" << lcd.getLineAsString(0) << "'\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT -> SONG PLAY)

    // Rewind via [STOP]+[BWD], start Song Play, verify >toPTN hides and 3 bars play (2x repeat + 1)
    canvas.pressStopAndBwdTogether();
    canvas.pressSeqOpButton(3); // [PLAY]
    (void)pump(4);
    if (!proc->isRhythmSongPlaying() || lcd.getLineAsString(1).contains(">toPTN")) {
        std::cerr << "[FAIL] Gate4B Test4: SONG PLAY) did not start or hide >toPTN: '" << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    (void)pump(650); // > 6.9s -> 3 bars at 120 BPM (6.0s) finishes cleanly
    if (proc->rhythmSongBarsPlayed() != 3u) {
        std::cerr << "[FAIL] Gate4B Test4: Expected 3 bars played with :|| x 2 repeat, got "
                  << proc->rhythmSongBarsPlayed() << "\n";
        return false;
    }
    canvas.pressSeqOpButton(2); // [STOP]
    (void)pump(2);

    // Verify SONG COPY> and SONG CLEAR>
    canvas.pressJobButton();
    canvas.pressSoftKey(1); // >Copy
    canvas.pressSoftKey(6); // >GO (Song 1 -> Song 2)
    canvas.pressKeypadYesButton();
    if (!classic.rhythmSong(1u).hasData()) {
        std::cerr << "[FAIL] Gate4B Test4: SONG COPY> failed to copy Song 1 to Song 2\n";
        return false;
    }
    canvas.pressSoftKey(7); // Back to SONG JOB SELECT)
    canvas.pressSoftKey(2); // >Clr
    canvas.pressSoftKey(0); // >Song
    canvas.pressKeypadYesButton(); // Select Song 2
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton(); // Confirm clear
    if (classic.rhythmSong(1u).hasData()) {
        std::cerr << "[FAIL] Gate4B Test4: SONG CLEAR> failed to clear Song 2\n";
        return false;
    }

    if (canvas.faceplatePopupSpawnCount() != 0u) {
        std::cerr << "[FAIL] Gate 4B spawned OS popup/dialog (" << canvas.faceplatePopupSpawnCount() << ")\n";
        return false;
    }

    std::cout << "[PASS] Gate 4B: Classic Mode Ch. 4 Rhythm Machine (Play, Step/Real Rec, Jobs, Song) passed!\n";
    return true;
}

bool runGate6ClassicPhase3AndFullAuditSelfTests() {
    std::cout << "\n--- Running Gate 6: Classic Mode Phase 3 (Ch. 6 Card/MIDI/Setup, Ch. 1 Demo/Greeting/BankHold & Full-Manual Audit) ---\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(44100.0, 256);

    vf50::gui::VF50MainCanvas canvas(*proc);
    canvas.setSize(vf50::gui::tokens::kVirtualWidth, vf50::gui::tokens::kVirtualHeight);
    canvas.resetFaceplatePopupSpawnCount();

    auto& classic = canvas.classicController();
    auto& lcd     = canvas.lcdDisplay();

    juce::AudioBuffer<float> audioBuf(2, 256);
    auto pumpBlocks = [&](int numBlocks, juce::MidiBuffer* inOutMidi = nullptr) {
        for (int b = 0; b < numBlocks; ++b) {
            audioBuf.clear();
            if (inOutMidi != nullptr && b == 0) {
                proc->processBlock(audioBuf, *inOutMidi);
            } else {
                juce::MidiBuffer empty;
                proc->processBlock(audioBuf, empty);
            }
        }
        canvas.refreshDisplayAndLeds();
    };

    // =========================================================================
    // TEST 1: Chapter 6 Card Operations (Format VF50SYN/VF50RHY, Save/Load SynALL,
    //         INT Protect Lockout with R.SEQ Exception, ROM & Write-Protect Lockouts)
    // =========================================================================
    canvas.pressCardButton();
    if (classic.screen() != vf50::classic::ClassicScreen::CardRoot ||
        !lcd.getLineAsString(0).startsWith("UT  CARD)") ||
        !lcd.getLineAsString(1).contains(">Form>Save>Load")) {
        std::cerr << "[FAIL] Gate6 Test1: [CARD] failed to enter UT CARD) ("
                  << lcd.getLineAsString(0) << " / " << lcd.getLineAsString(1) << ")\n";
        return false;
    }

    // Format MCD64 Bank 1 as VF50SYN
    canvas.pressSoftKey(5); // >Form
    if (classic.screen() != vf50::classic::ClassicScreen::CardFormat ||
        !lcd.getLineAsString(1).contains("VF50SYN   VF50RHY    >EXIT")) {
        std::cerr << "[FAIL] Gate6 Test1: >Form failed to enter FORMAT screen\n";
        return false;
    }
    canvas.pressSoftKey(3); // VF50SYN -> Are you sure?
    if (!lcd.getLineAsString(0).contains("Are you sure?")) {
        std::cerr << "[FAIL] Gate6 Test1: VF50SYN format missing 'Are you sure?' prompt\n";
        return false;
    }
    canvas.pressKeypadYesButton();
    if (proc->virtualCard().banks[0].format != vf50::state::CardBankFormat::VF50Syn ||
        !lcd.getLineAsString(1).contains("1  VF50SYN")) {
        std::cerr << "[FAIL] Gate6 Test1: Bank 1 did not format as VF50SYN\n";
        return false;
    }

    // Switch to Bank 2 and format as VF50RHY, then save R.SEQ to Rhythm(toA)
    canvas.pressKeypadDigitButton(2); // Bank 2
    if (proc->virtualCard().activeBankIndex != 1u) {
        std::cerr << "[FAIL] Gate6 Test1: Failed to select MCD64 Bank 2\n";
        return false;
    }
    canvas.pressSoftKey(5); // >Form
    canvas.pressSoftKey(5); // VF50RHY
    canvas.pressKeypadYesButton();
    if (proc->virtualCard().banks[1].format != vf50::state::CardBankFormat::VF50Rhy) {
        std::cerr << "[FAIL] Gate6 Test1: Bank 2 did not format as VF50RHY\n";
        return false;
    }
    proc->hostMemory().mutableUserRhythmPattern(0).bars = 3u;
    canvas.pressSoftKey(6); // >Save
    canvas.pressSoftKey(2); // Rhythm(toA)
    canvas.pressSoftKey(3); // R.SEQ
    canvas.pressKeypadYesButton();
    if (!proc->virtualCard().banks[1].rhythmSets[0].hasRSeq ||
        proc->virtualCard().banks[1].rhythmSets[0].patterns[0].bars != 3u) {
        std::cerr << "[FAIL] Gate6 Test1: Rhythm(toA) R.SEQ save to Bank 2 failed\n";
        return false;
    }

    // Switch back to Bank 1 (VF50SYN), customize User Voice 0 and Master Tuning, and Save SynAL
    canvas.pressKeypadDigitButton(1);
    {
        auto v0 = proc->hostMemory().getUserVoice(0);
        v0.feedback = 6u;
        proc->hostMemory().storeUserVoice(0, v0);
        proc->mutableClassicSystemSettings().masterTuningSteps = 12;
    }
    canvas.pressSoftKey(6); // >Save
    canvas.pressSoftKey(0); // Synth
    canvas.pressSoftKey(0); // SynAL
    canvas.pressKeypadYesButton();
    if (!proc->virtualCard().banks[0].hasVoicesPerfs || !proc->virtualCard().banks[0].hasSys) {
        std::cerr << "[FAIL] Gate6 Test1: SynAL save to Bank 1 failed\n";
        return false;
    }

    // Mutate internal state, then Load SynAL back from Bank 1 (INT protect OFF)
    {
        auto v0 = proc->hostMemory().getUserVoice(0);
        v0.feedback = 1u;
        proc->hostMemory().storeUserVoice(0, v0);
        proc->mutableClassicSystemSettings().masterTuningSteps = -20;
        proc->setIntMemoryProtected(false);
    }
    canvas.pressSoftKey(7); // >Load
    canvas.pressSoftKey(0); // Synth
    canvas.pressSoftKey(0); // SynAL
    canvas.pressKeypadYesButton();
    if (proc->hostMemory().getUserVoice(0).feedback != 6u ||
        proc->classicSystemSettings().masterTuningSteps != 12) {
        std::cerr << "[FAIL] Gate6 Test1: SynAL load failed to restore voice/system settings\n";
        return false;
    }

    // Enable INT Memory Protect -> assert SynAL Load is blocked with "Memory Protected",
    // while Bank 2 R.SEQ Load is permitted per manual p. 98 exception!
    proc->setIntMemoryProtected(true);
    canvas.pressSoftKey(7); // >Load
    canvas.pressSoftKey(0); // Synth
    canvas.pressSoftKey(0); // SynAL
    if (!lcd.getLineAsString(1).contains("Memory Protected")) {
        std::cerr << "[FAIL] Gate6 Test1: INT Protect failed to block SynAL Load with 'Memory Protected'\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT to LOAD category
    canvas.pressSoftKey(7); // >EXIT to CardRoot
    canvas.pressKeypadDigitButton(2); // Bank 2 (VF50RHY)
    proc->hostMemory().mutableUserRhythmPattern(0).bars = 1u;
    canvas.pressSoftKey(7); // >Load
    canvas.pressSoftKey(1); // Rhythm(A)
    canvas.pressSoftKey(3); // R.SEQ (exempt from INT Memory Protect!)
    canvas.pressKeypadYesButton();
    if (proc->hostMemory().userRhythmPattern(0).bars != 3u) {
        std::cerr << "[FAIL] Gate6 Test1: R.SEQ Load was wrongly blocked when INT Memory Protect was ON\n";
        return false;
    }
    proc->setIntMemoryProtected(false);

    // Assert Card Write-Protect and ROM Card block Save
    canvas.pressKeypadDigitButton(1); // Bank 1
    classic.setCardHardwareWriteProtected(true);
    canvas.pressSoftKey(6); // >Save
    canvas.pressSoftKey(0); // Synth
    canvas.pressSoftKey(0); // SynAL
    if (!lcd.getLineAsString(1).contains("Memory Protected")) {
        std::cerr << "[FAIL] Gate6 Test1: Card Write-Protect failed to show 'Memory Protected'\n";
        return false;
    }
    classic.setCardHardwareWriteProtected(false);
    proc->mutableVirtualCard().mediaType = vf50::state::CardMediaType::RomCard;
    canvas.pressSoftKey(0); // SynAL on ROM Card
    if (!lcd.getLineAsString(1).contains("ROM Card!")) {
        std::cerr << "[FAIL] Gate6 Test1: ROM Card failed to block Save\n";
        return false;
    }
    proc->mutableVirtualCard().mediaType = vf50::state::CardMediaType::Mcd64Ram;

    // =========================================================================
    // TEST 2: Chapter 6 MIDI Functions (Note on/off odd/even, Switch Global Ch G3,
    //         PgmCng Table Remap, Held SINGLE + 012 Direct PC Transmit, Bulk SysEx)
    // =========================================================================
    canvas.pressSingleButton();
    canvas.pressMidiButton();
    if (classic.screen() != vf50::classic::ClassicScreen::MidiRoot ||
        !lcd.getLineAsString(1).contains(">ChInfo>Switch >Cond>PgmCng   >Exclusive")) {
        std::cerr << "[FAIL] Gate6 Test2: [MIDI] failed to enter UT MIDI)\n";
        return false;
    }

    // 2a. >Cond: Note on/off odd vs even filter on incoming MIDI notes
    canvas.pressSoftKey(3); // >Cond
    canvas.pressKeypadYesButton(); // all -> odd
    if (proc->classicSystemSettings().noteOnOffFilter != 1u ||
        !lcd.getLineAsString(1).contains("odd")) {
        std::cerr << "[FAIL] Gate6 Test2: Failed to set Note on/off filter to odd\n";
        return false;
    }
    pumpBlocks(2);
    {
        juce::MidiBuffer evenNote;
        evenNote.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        pumpBlocks(1, &evenNote);
        if (proc->synthEngine().fm().activeVoiceCount() != 0u) {
            std::cerr << "[FAIL] Gate6 Test2: 'odd' filter failed to block even MIDI note 60\n";
            return false;
        }
        juce::MidiBuffer oddNote;
        oddNote.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 0);
        pumpBlocks(1, &oddNote);
        if (proc->synthEngine().fm().activeVoiceCount() == 0u) {
            std::cerr << "[FAIL] Gate6 Test2: 'odd' filter blocked odd MIDI note 61\n";
            return false;
        }
        juce::MidiBuffer allOff;
        allOff.addEvent(juce::MidiMessage::noteOff(1, 61), 0);
        pumpBlocks(1, &allOff);
    }
    canvas.pressKeypadYesButton(); // odd -> even
    canvas.pressKeypadYesButton(); // even -> all
    canvas.pressSoftKey(7); // >EXIT to UT MIDI)

    // 2b. >Switch: Global channel G3 (mode = 4) modulates all channels
    canvas.pressSoftKey(1); // >Switch
    canvas.pressSoftKey(6); // >PitchBend
    canvas.pressKeypadYesButton(); // norm -> G1
    canvas.pressKeypadYesButton(); // G1 -> G2
    canvas.pressKeypadYesButton(); // G2 -> G3
    if (proc->classicSystemSettings().pitchBendMode != 4u ||
        !lcd.getLineAsString(1).contains("G3")) {
        std::cerr << "[FAIL] Gate6 Test2: Failed to set PitchBend to G3\n";
        return false;
    }
    pumpBlocks(2);
    {
        juce::MidiBuffer pbG3;
        pbG3.addEvent(juce::MidiMessage::pitchWheel(3, 12288), 0); // Ch 3 (1-based) = G3
        pumpBlocks(1, &pbG3);
        const float ch1Bend = proc->synthEngine().fm().channelControllerState(0u).pitchBendNorm;
        const float ch8Bend = proc->synthEngine().fm().channelControllerState(7u).pitchBendNorm;
        if (ch1Bend < 0.25f || std::abs(ch1Bend - ch8Bend) > 1.0e-4f) {
            std::cerr << "[FAIL] Gate6 Test2: Global channel G3 PitchBend failed to broadcast across channels (ch1="
                      << ch1Bend << ", ch8=" << ch8Bend << ")\n";
            return false;
        }
        juce::MidiBuffer pbCenter;
        pbCenter.addEvent(juce::MidiMessage::pitchWheel(3, 8192), 0);
        pumpBlocks(1, &pbCenter);
    }
    proc->mutableClassicSystemSettings().pitchBendMode = 1u; // Restore norm
    proc->publishClassicSystemStateToAudio();
    canvas.pressSoftKey(7); // >EXIT to UT MIDI)

    // 2c. >PgmCng: set mode to 'common', edit PC 001 -> P15, verify incoming PC 0 selects Preset 15
    canvas.pressSoftKey(4); // >PgmCng
    canvas.pressKeypadNoButton(); // individual -> common
    if (proc->classicSystemSettings().pgmChangeMode != 1u) {
        std::cerr << "[FAIL] Gate6 Test2: Failed to set Pgm Change mode to common\n";
        return false;
    }
    canvas.pressSoftKey(6); // >Edit
    canvas.pressMemBankButton(2); // PRESET
    canvas.pressKeypadDigitButton(1);
    canvas.pressKeypadDigitButton(5); // P15
    if (proc->pgmChangeTable().entries[0].bank != 2u ||
        proc->pgmChangeTable().entries[0].number != 15u) {
        std::cerr << "[FAIL] Gate6 Test2: Pgm Change Table Edit failed to map PGM 1 to P15\n";
        return false;
    }
    pumpBlocks(2);
    {
        juce::MidiBuffer pcIn;
        pcIn.addEvent(juce::MidiMessage::programChange(1, 0), 0); // PC 0 -> PGM 1 -> P15
        pumpBlocks(2, &pcIn);
        if (proc->activeVoiceBank() != vf50::state::MemoryBank::FactoryRom ||
            proc->hostMemory().selectedVoiceIndex() != 15u) {
            std::cerr << "[FAIL] Gate6 Test2: Incoming PC 0 failed to remap to P15 via Program Change Table\n";
            return false;
        }
    }

    // 2d. Direct PC Transmit: hold SINGLE in PlaySingle, enter 012, release -> emits PC 11 without changing local patch
    canvas.pressSingleButton();
    const std::size_t voiceBeforeDirectPc = proc->hostMemory().selectedVoiceIndex();
    canvas.holdSingleButton(true);
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(1);
    canvas.pressKeypadDigitButton(2);
    if (!lcd.getLineAsString(0).startsWith("** Sending PC No 012")) {
        std::cerr << "[FAIL] Gate6 Test2: Direct PC overlay mismatch (" << lcd.getLineAsString(0) << ")\n";
        return false;
    }
    canvas.holdSingleButton(false);
    {
        juce::MidiBuffer outCapture;
        pumpBlocks(1, &outCapture);
        bool foundPc11 = false;
        for (const auto meta : outCapture) {
            const auto msg = meta.getMessage();
            if (msg.isProgramChange() && msg.getProgramChangeNumber() == 11) {
                foundPc11 = true;
                break;
            }
        }
        if (!foundPc11 || proc->hostMemory().selectedVoiceIndex() != voiceBeforeDirectPc) {
            std::cerr << "[FAIL] Gate6 Test2: Direct PC 012 failed to emit PC 11 or altered local voice\n";
            return false;
        }
    }

    // 2e. >Exclusive -> >Bulk emits valid SysEx packet
    canvas.pressMidiButton();
    canvas.pressSoftKey(6); // >Exclusive
    canvas.pressSoftKey(6); // >Bulk -> UT DUMP)
    if (classic.screen() != vf50::classic::ClassicScreen::MidiBulkRoot ||
        !lcd.getLineAsString(1).contains("VOICE PFM SETUP      SEQ RHY  >EXIT")) {
        std::cerr << "[FAIL] Gate6 Test2: Failed to enter UT DUMP)\n";
        return false;
    }
    canvas.pressSoftKey(1); // VOICE
    canvas.pressSoftKey(5); // >Transmit -> Transmit ready?
    if (!lcd.getLineAsString(0).contains("Transmit ready?")) {
        std::cerr << "[FAIL] Gate6 Test2: Bulk dump missing 'Transmit ready?' prompt\n";
        return false;
    }
    canvas.pressKeypadYesButton();
    {
        juce::MidiBuffer sysexOut;
        pumpBlocks(1, &sysexOut);
        bool foundHardwareSysEx = false;
        for (const auto meta : sysexOut) {
            const auto msg = meta.getMessage();
            if (msg.isSysEx() && msg.getSysExDataSize() > 6 && msg.getSysExData()[0] == 0x43u) {
                foundHardwareSysEx = true;
                break;
            }
        }
        if (!foundHardwareSysEx) {
            std::cerr << "[FAIL] Gate6 Test2: >Bulk failed to emit valid 4-Op SysEx packet\n";
            return false;
        }
    }

    // =========================================================================
    // TEST 3: Chapter 6 Utility Setup (Master Tuning, Chord 1, User Octave Microtuning,
    //         Velocity Curve / Fixed Velocity, Combine=off lowercase cue)
    // =========================================================================
    canvas.pressSingleButton();
    canvas.pressSetupButton();
    if (classic.screen() != vf50::classic::ClassicScreen::SetupRoot ||
        !lcd.getLineAsString(1).contains(">Tune>Comb>Ctrl>P.Efct  >Micro>Vel >Damp")) {
        std::cerr << "[FAIL] Gate6 Test3: [SETUP] failed to enter UT SETUP)\n";
        return false;
    }

    // 3a. Master Tuning live pitch verification
    canvas.pressSoftKey(0); // >Tune
    proc->mutableClassicSystemSettings().masterTuningSteps = 0;
    proc->publishClassicSystemStateToAudio();
    pumpBlocks(2);
    for (int i = 0; i < 32; ++i) {
        canvas.pressKeypadYesButton(); // +32 steps = +50 cents
    }
    pumpBlocks(2);
    {
        const double tunedCents = proc->synthEngine().fm().masterTuneCents();
        if (std::abs(tunedCents - 50.0) > 1.0) {
            std::cerr << "[FAIL] Gate6 Test3: Master Tuning +32 failed to shift FM master tune by +50 cents (got "
                      << tunedCents << ")\n";
            return false;
        }
    }
    proc->mutableClassicSystemSettings().masterTuningSteps = 0;
    proc->publishClassicSystemStateToAudio();
    canvas.pressSoftKey(7); // >EXIT to UT SETUP)

    // 3b. Performance Effect Chord 1 live DSP multi-note trigger during Single Play audition
    canvas.pressSoftKey(3); // >P.Efct
    canvas.pressSoftKey(4); // chord
    canvas.pressSoftKey(0); // chord1 -> UT CHORD1) (enables Single Play audition of chord1!)
    if (classic.screen() != vf50::classic::ClassicScreen::SetupChordEdit ||
        !lcd.getLineAsString(0).startsWith("UT  CHORD1)")) {
        std::cerr << "[FAIL] Gate6 Test3: Failed to enter UT CHORD1)\n";
        return false;
    }
    canvas.pressSoftKey(5); // Select 4th chord note slot ('*')
    canvas.pressKeypadYesButton(); // Enable 4th note (60)
    pumpBlocks(2);
    {
        juce::MidiBuffer trigC3;
        trigC3.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        pumpBlocks(1, &trigC3);
        const std::size_t chordVoices = proc->synthEngine().fm().activeVoiceCount();
        juce::MidiBuffer relC3;
        relC3.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        pumpBlocks(1, &relC3);
        if (chordVoices < 3u) {
            std::cerr << "[FAIL] Gate6 Test3: Chord 1 audition failed to trigger multiple chord voices (active="
                      << chordVoices << ")\n";
            return false;
        }
    }
    canvas.pressSoftKey(7); // >EXIT to slot select
    canvas.pressSoftKey(7); // >EXIT to cat select
    canvas.pressSoftKey(7); // >EXIT to UT SETUP)

    // 3c. User Octave Microtuning edit live pitch verification
    canvas.pressSoftKey(4); // >Micro -> UT MICRO TUNE)
    canvas.pressSoftKey(3); // OCT. >Edit (enables Single Play audition of User Octave Microtuning!)
    if (classic.screen() != vf50::classic::ClassicScreen::SetupMicroOctEdit ||
        !lcd.getLineAsString(0).startsWith("UT  MICRO) OCT.")) {
        std::cerr << "[FAIL] Gate6 Test3: Failed to enter UT MICRO) OCT. Edit\n";
        return false;
    }
    canvas.pressSoftKey(1); // >FINE on C3
    for (int i = 0; i < 32; ++i) {
        canvas.pressKeypadYesButton(); // +32 fine steps = +50 cents on C3
    }
    pumpBlocks(2);
    {
        juce::MidiBuffer trigC3;
        trigC3.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        pumpBlocks(1, &trigC3);
        double microCents = 0.0;
        for (std::size_t vi = 0u; vi < 16u; ++vi) {
            const auto& v = proc->synthEngine().fm().voiceAt(vi);
            if (v.isActive() && v.isKeyDown()) {
                microCents = v.currentPitchCents();
                break;
            }
        }
        juce::MidiBuffer relC3;
        relC3.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        pumpBlocks(1, &relC3);
        if (std::abs(microCents - 6050.0) > 2.5) {
            std::cerr << "[FAIL] Gate6 Test3: User Octave Microtuning C3 +32 failed to shift pitch to ~6050 cents (got "
                      << microCents << ")\n";
            return false;
        }
    }
    proc->hostMemory().resetUserMicrotuningsToFactory();
    proc->pushHostMemoryToAudio();
    canvas.pressSoftKey(7); // >EXIT to UT MICRO TUNE)
    canvas.pressSoftKey(7); // >EXIT to UT SETUP)

    // 3d. Velocity Curve & Fixed Velocity live DSP verification
    canvas.pressSoftKey(6); // >Vel
    proc->mutableClassicSystemSettings().fixedVelocity = 42u;
    proc->publishClassicSystemStateToAudio();
    pumpBlocks(2);
    {
        juce::MidiBuffer velIn;
        velIn.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        pumpBlocks(1, &velIn);
        const uint8_t trigVel = proc->synthEngine().fm().lastTriggeredVelocity();
        juce::MidiBuffer velOff;
        velOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        pumpBlocks(1, &velOff);
        if (trigVel != 42u) {
            std::cerr << "[FAIL] Gate6 Test3: FixedVelocity=42 failed to override incoming velocity (got "
                      << static_cast<int>(trigVel) << ")\n";
            return false;
        }
    }
    proc->mutableClassicSystemSettings().fixedVelocity = 0u;
    proc->mutableClassicSystemSettings().velocityCurve = 6u; // 6(flat) capped at <= 80
    proc->publishClassicSystemStateToAudio();
    pumpBlocks(2);
    {
        juce::MidiBuffer velIn;
        velIn.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(127)), 0);
        pumpBlocks(1, &velIn);
        const uint8_t flatVel = proc->synthEngine().fm().lastTriggeredVelocity();
        juce::MidiBuffer velOff;
        velOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        pumpBlocks(1, &velOff);
        if (flatVel > 80u) {
            std::cerr << "[FAIL] Gate6 Test3: VelCurve 6(flat) failed to cap velocity at <= 80 (got "
                      << static_cast<int>(flatVel) << ")\n";
            return false;
        }
    }
    proc->mutableClassicSystemSettings().velocityCurve = 0u;
    proc->publishClassicSystemStateToAudio();
    canvas.pressSoftKey(7); // >EXIT to UT SETUP)

    // 3e. Combine = off preserves Function/Effect and displays lowercase 1st character on Play LCD
    canvas.pressSoftKey(1); // >Comb
    canvas.pressSoftKey(3); // Toggle Combine -> off
    if (proc->classicSystemSettings().combineWithFunction) {
        std::cerr << "[FAIL] Gate6 Test3: Failed to toggle Combine to off\n";
        return false;
    }
    canvas.pressSingleButton();
    canvas.pressKeypadYesButton(); // Step to next voice with Combine=off
    {
        const juce::String line1 = lcd.getLineAsString(1);
        const char firstNameChar = static_cast<char>(line1[5]);
        if (firstNameChar >= 'A' && firstNameChar <= 'Z') {
            std::cerr << "[FAIL] Gate6 Test3: Combine=off did not lowercase 1st character of voice name ("
                      << line1 << ")\n";
            return false;
        }
    }
    proc->mutableClassicSystemSettings().combineWithFunction = true;
    proc->publishClassicSystemStateToAudio();

    // =========================================================================
    // TEST 4: Chapter 1 Demo, Power-On Greeting Persistence & Bank Hold
    // =========================================================================
    // 4a. Power-On Greeting edit via DEMO + STORE/COPY and state persistence
    canvas.pressDemoAndStoreTogether();
    if (classic.screen() != vf50::classic::ClassicScreen::UtilEditMessage ||
        !lcd.getLineAsString(0).startsWith("UT EDIT MESSAGE)")) {
        std::cerr << "[FAIL] Gate6 Test4: [DEMO]+[STORE/COPY] failed to enter UT EDIT MESSAGE)\n";
        return false;
    }
    classic.onTextKeyPressed('V');
    classic.onTextKeyPressed('F');
    classic.onTextKeyPressed('5');
    classic.onTextKeyPressed('0');
    classic.onTextKeyPressed('!');
    canvas.refreshDisplayAndLeds();
    if (!lcd.getLineAsString(1).startsWith("VF50!")) {
        std::cerr << "[FAIL] Gate6 Test4: Editing greeting message failed (" << lcd.getLineAsString(1) << ")\n";
        return false;
    }
    {
        proc->mutableClassicSystemSettings().localOn = false;
        juce::MemoryBlock stateBlob;
        proc->getStateInformation(stateBlob);

        auto restoredProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        restoredProc->setStateInformation(stateBlob.getData(), static_cast<int>(stateBlob.getSize()));
        if (restoredProc->classicSystemSettings().powerOnMessage[0] != 'V' ||
            restoredProc->classicSystemSettings().powerOnMessage[1] != 'F' ||
            restoredProc->classicSystemSettings().powerOnMessage[2] != '5' ||
            restoredProc->classicSystemSettings().powerOnMessage[3] != '0' ||
            restoredProc->classicSystemSettings().powerOnMessage[4] != '!' ||
            !restoredProc->classicSystemSettings().localOn) {
            std::cerr << "[FAIL] Gate6 Test4: Greeting persistence or Local=on boot reset failed\n";
            return false;
        }
    }
    classic.resetToBootOrPlay(true);
    canvas.refreshDisplayAndLeds();
    if (classic.screen() != vf50::classic::ClassicScreen::PowerOnGreeting ||
        !lcd.getLineAsString(1).startsWith("VF50!")) {
        std::cerr << "[FAIL] Gate6 Test4: Power-On Greeting splash failed to show persisted Line 2\n";
        return false;
    }

    // 4b. Bank Hold: [-] + 7 on voice 23 -> pressing 9 immediately selects 29; [-] + 8 disables
    canvas.pressSingleButton();
    canvas.pressKeypadDigitButton(2);
    canvas.pressKeypadDigitButton(3);
    if (proc->hostMemory().selectedVoiceIndex() != 23u) {
        std::cerr << "[FAIL] Gate6 Test4: Failed to select voice 23\n";
        return false;
    }
    canvas.pressInstMinusButton();
    canvas.pressKeypadDigitButton(7); // [-] + 7 -> Bank Hold ON
    if (!classic.isBankHoldEnabled()) {
        std::cerr << "[FAIL] Gate6 Test4: [-] + 7 failed to enable Bank Hold\n";
        return false;
    }
    canvas.pressKeypadDigitButton(9); // Single digit 9 -> immediately selects 29!
    if (proc->hostMemory().selectedVoiceIndex() != 29u) {
        std::cerr << "[FAIL] Gate6 Test4: Bank Hold failed to select voice 29 on single digit 9 (selected="
                  << proc->hostMemory().selectedVoiceIndex() << ")\n";
        return false;
    }
    canvas.pressInstMinusButton();
    canvas.pressKeypadDigitButton(8); // [-] + 8 -> Bank Hold OFF
    if (classic.isBankHoldEnabled()) {
        std::cerr << "[FAIL] Gate6 Test4: [-] + 8 failed to disable Bank Hold\n";
        return false;
    }
    canvas.pressKeypadDigitButton(4); // First digit of two-digit selection -> should stay on 29 waiting for units digit
    if (proc->hostMemory().selectedVoiceIndex() != 29u || !lcd.getLineAsString(1).contains("4?")) {
        std::cerr << "[FAIL] Gate6 Test4: Disabling Bank Hold did not restore 2-digit selection\n";
        return false;
    }
    canvas.pressKeypadDigitButton(1); // Complete 41

    // 4c. UT DEMO) walkthrough (>ROM Play -> Confirm -> START/STOP/EXIT & >V. Edit speed)
    canvas.pressDemoButton();
    if (classic.screen() != vf50::classic::ClassicScreen::DemoRoot ||
        !lcd.getLineAsString(1).contains(">ROM Play >Disk Play >ChainPlay >V. Edit")) {
        std::cerr << "[FAIL] Gate6 Test4: [DEMO] failed to enter UT DEMO)\n";
        return false;
    }
    canvas.pressSoftKey(0); // >ROM Play -> Confirm
    if (classic.screen() != vf50::classic::ClassicScreen::DemoConfirm ||
        !lcd.getLineAsString(1).contains("All data will be changed !   [No]  [Yes]")) {
        std::cerr << "[FAIL] Gate6 Test4: Demo confirmation prompt mismatch\n";
        return false;
    }
    canvas.pressSoftKey(7); // [Yes]
    for (int t = 0; t < 15; ++t) {
        classic.tick30Hz();
    }
    canvas.refreshDisplayAndLeds();
    if (classic.screen() != vf50::classic::ClassicScreen::DemoPlay ||
        !lcd.getLineAsString(1).contains(">START>STOP>EXIT")) {
        std::cerr << "[FAIL] Gate6 Test4: DemoPlay screen mismatch (" << lcd.getLineAsString(1) << ")\n";
        return false;
    }
    canvas.pressSoftKey(7); // >EXIT

    // =========================================================================
    // TEST 5: Full-Manual 40x2 LCD Audit across all ClassicScreens & Zero Popups
    // =========================================================================
    for (int s = static_cast<int>(vf50::classic::ClassicScreen::PowerOnGreeting);
         s <= static_cast<int>(vf50::classic::ClassicScreen::UtilEditMessage);
         ++s) {
        const auto scr = static_cast<vf50::classic::ClassicScreen>(s);
        if (scr == vf50::classic::ClassicScreen::DelegatedEdit) {
            continue;
        }
        classic.enterScreen(scr);
        const auto frame = classic.renderLcdFrame();
        if (frame.line0.size() != 40u || frame.line1.size() != 40u) {
            std::cerr << "[FAIL] Gate6 Test5: Screen enum " << s << " violated 40x2 geometry\n";
            return false;
        }
    }
    canvas.pressSingleButton();

    if (canvas.faceplatePopupSpawnCount() != 0u) {
        std::cerr << "[FAIL] Gate 6 spawned OS popup/dialog in Classic Mode ("
                  << canvas.faceplatePopupSpawnCount() << ")\n";
        return false;
    }

    std::cout << "[PASS] Gate 6: Classic Mode Phase 3 (Card, MIDI, Setup, Demo, Greeting, Bank Hold & Full-Manual 40x2 Audit) passed!\n";
    return true;
}

bool runGate7StateRoundTripAndDspStressSelfTests() {
    std::cout << "\n=== Running Gate 7: BYOR Empty-ROM Boot, Full State Round-Trip & DSP Stress Suite ===\n";

    // =========================================================================
    // TEST 1: BYOR Empty-ROM Boot, Setup Prompt Overlay & Neutral Trademark Splash
    // =========================================================================
    {
        vf50::rom::VF50RomSetBuilder::clearConfiguredPaths();
        auto emptyProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        emptyProc->prepareToPlay(48000.0, 256);

        if (emptyProc->activeRomBank().isAllRomsLoaded() || emptyProc->activeRomBank().loadedChipCount() != 0) {
            std::cerr << "[FAIL] Gate7 Test1: Empty-ROM boot unexpectedly loaded ROM chips\n";
            (void)installRuntimeTestRomsForFullSuite();
            return false;
        }
        if (emptyProc->getName() != "Victory VF50") {
            std::cerr << "[FAIL] Gate7 Test1: Plugin metadata name expected 'Victory VF50', got '"
                      << emptyProc->getName() << "'\n";
            (void)installRuntimeTestRomsForFullSuite();
            return false;
        }

        std::unique_ptr<juce::AudioProcessorEditor> baseEd(emptyProc->createEditor());
        auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
        if (editor == nullptr || !editor->mainCanvas().isRomGuideVisible()) {
            std::cerr << "[FAIL] Gate7 Test1: BYOR Setup Guide overlay did not auto-display on empty-ROM boot\n";
            (void)installRuntimeTestRomsForFullSuite();
            return false;
        }

        // Verify Classic Mode boot splash Line 1 uses neutral Victory VF50 / VF50 branding with zero Hardware strings
        auto& canvas = editor->mainCanvas();
        canvas.classicController().resetToBootOrPlay(true);
        canvas.refreshDisplayAndLeds();
        const juce::String line1 = canvas.lcdDisplay().getLineAsString(0);
        const juce::String line2 = canvas.lcdDisplay().getLineAsString(1);
        if (line1.length() != 40 || line2.length() != 40 ||
            line1 != "******     Victory VF50 Synth     ******" ||
            line1.containsIgnoreCase("HARDWARE") ||
            line2 != ">>>>       Nice to meet you !!      <<<<") {
            std::cerr << "[FAIL] Gate7 Test1: Classic boot splash branding mismatch: L1='"
                      << line1 << "' L2='" << line2 << "'\n";
            (void)installRuntimeTestRomsForFullSuite();
            return false;
        }

        // Verify empty-ROM audio rendering does not crash, silence-lock FM, or emit NaN/Inf
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{ 110 }), 0);
        midi.addEvent(juce::MidiMessage::noteOn(10, 36, uint8_t{ 120 }), 0);
        float maxFmPeak = 0.0f;
        for (int b = 0; b < 6; ++b) {
            buf.clear();
            emptyProc->processBlock(buf, midi);
            midi.clear();
            for (int ch = 0; ch < 2; ++ch) {
                const float* p = buf.getReadPointer(ch);
                for (int i = 0; i < 256; ++i) {
                    if (!std::isfinite(p[i]) || std::fpclassify(p[i]) == FP_SUBNORMAL) {
                        std::cerr << "[FAIL] Gate7 Test1: Non-finite or denormal sample during empty-ROM boot\n";
                        (void)installRuntimeTestRomsForFullSuite();
                        return false;
                    }
                    maxFmPeak = std::max(maxFmPeak, std::abs(p[i]));
                }
            }
        }
        if (maxFmPeak < 0.05f || maxFmPeak > 4.0f) {
            std::cerr << "[FAIL] Gate7 Test1: Empty-ROM Init Voice peak out of bounds: " << maxFmPeak << "\n";
            (void)installRuntimeTestRomsForFullSuite();
            return false;
        }

        baseEd.reset();
        emptyProc.reset();
        if (!installRuntimeTestRomsForFullSuite()) {
            std::cerr << "[FAIL] Gate7 Test1: Failed to restore runtime test ROMs\n";
            return false;
        }
        std::cout << "[PASS] Gate 7 Test 1: BYOR empty-ROM boot, Setup prompt & neutral 'Victory VF50 / VF50' boot splash verified.\n";
    }

    // =========================================================================
    // TEST 2: Full State Save/Reload Round-Trip (getStateInformation / setStateInformation)
    // =========================================================================
    {
        auto srcProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        srcProc->prepareToPlay(48000.0, 256);

        // 2a. UI scale factor (1.50f), PG Drawer, Virtual Keyboard & LCD Theme
        srcProc->setUiScaleFactor(1.50f);
        srcProc->setVirtualKeyboardVisible(true);
        srcProc->setProgrammerVisible(true);
        srcProc->setLcdThemeIndex(2);
        srcProc->setGlobalTransposeSemitones(-5);

        // 2b. Memory Protect flags
        srcProc->setIntMemoryProtected(true);
        srcProc->setCardMemoryProtected(true);

        // 2c. INTERNAL 00-99 Voices & Performances (customize all 100 slots)
        for (std::size_t i = 0u; i < 100u; ++i) {
            vf50::state::StoredVoiceSlot vSlot(testRom().factoryVoices[i]);
            char vName[16]{};
            std::snprintf(vName, sizeof(vName), "G7Vce_%02zu  ", i);
            vSlot.setName(std::string_view(vName, 10u));
            vSlot.voice.algorithm = static_cast<uint8_t>(i & 0x07u);
            vSlot.voice.feedback  = static_cast<uint8_t>((i + 3u) & 0x07u);
            vSlot.voice.operators[0].outputLevel = static_cast<uint8_t>(50u + (i % 49u));
            vSlot.voice.effect.effectType = static_cast<uint8_t>((i % 32u) + 1u);
            vSlot.repackRomBytes();
            srcProc->hostMemory().storeUserVoice(i, vSlot.voice);

            vf50::state::StoredPerformanceSlot pSlot(testRom().factoryPerformances[i]);
            char pName[16]{};
            std::snprintf(pName, sizeof(pName), "G7Prf_%02zu  ", i);
            pSlot.setName(std::string_view(pName, 10u));
            pSlot.perf.assignMode = static_cast<uint8_t>(i % 3u);
            pSlot.perf.perfEffectSelect = static_cast<uint8_t>(i % 13u);
            pSlot.perf.instruments[0].volume = static_cast<uint8_t>(40u + (i % 59u));
            std::array<int8_t, 8> pans{};
            for (std::size_t p = 0u; p < 8u; ++p) {
                pans[p] = static_cast<int8_t>(std::clamp<int>(static_cast<int>((i + p * 7u) % 61u) - 30, -64, 63));
            }
            pSlot.repackRomBytes();
            srcProc->hostMemory().storeUserPerformance(i, pSlot.perf, pans);
        }

        // 2d. User Microtuning (Octave & Full 128-key tables)
        auto octMt = srcProc->hostMemory().userOctaveMicrotuning();
        for (std::size_t n = 0u; n < 12u; ++n) {
            octMt[n] = { static_cast<uint8_t>(60u + n), static_cast<uint8_t>((n * 5u + 3u) & 0x3Fu) };
        }
        srcProc->hostMemory().setUserOctaveMicrotuning(octMt);

        auto fullMt = srcProc->hostMemory().userFullMicrotuning();
        for (std::size_t k = 0u; k < 128u; ++k) {
            fullMt[k] = { static_cast<uint8_t>((127u - k) & 0x7Fu), static_cast<uint8_t>((k * 3u + 1u) & 0x3Fu) };
        }
        srcProc->hostMemory().setUserFullMicrotuning(fullMt);
        srcProc->pushHostMemoryToAudio();

        // 2e. Utility Setup parameters (Master Tune, Performance Effects, Velocity Curve, Editable 40-char Greeting)
        auto& sys = srcProc->mutableClassicSystemSettings();
        sys.masterTuningSteps   = -22;
        sys.synthVolume         = 81u;
        sys.combineWithFunction = false;
        sys.controllerReset     = true;
        sys.fixedVelocity       = 104u;
        sys.velocityCurve       = 6u; // 6(flat)
        sys.egDampSpeed         = 1u; // slow
        sys.voiceDampOn         = false;
        sys.basicReceiveCh      = 4u;
        sys.transmitCh          = 2u;
        sys.controlChangeMode   = 5u; // G4
        sys.aftertouchMode      = 6u; // G5
        sys.pitchBendMode       = 7u; // G6
        sys.noteOnOffFilter     = 2u; // even
        sys.dataEntryAssignCc   = 11u;// Exp
        sys.pgmChangeMode       = 1u; // common
        sys.deviceNumber        = 3u;
        constexpr std::string_view kCustomGreeting = "****  VF50 GATE 7 STATE RECALL OK!  ****";
        static_assert(kCustomGreeting.size() == 40u);
        for (std::size_t c = 0u; c < 40u; ++c) {
            sys.powerOnMessage[c] = kCustomGreeting[c];
        }
        vf50::params::setApvtsRawValue(srcProc->apvts(), vf50::params::ParamIndex::MasterTune, -34.375f);

        auto& pef = srcProc->mutablePerfEffectBank();
        pef.delays[0] = { 47u, -12, 6u, 77u };
        pef.delays[3] = { 112u, 7,  5u, 64u };
        pef.pans[1]   = { 2u,   1u, 73u };
        pef.chords[2].chords[0] = { 60, 63, 67, 70 };

        // 2f. Virtual MCD64 Card Banks (Bank 0 = VF50SYN, Bank 1 = VF50RHY)
        auto& card = srcProc->mutableVirtualCard();
        card.mediaType            = vf50::state::CardMediaType::Mcd64Ram;
        card.hardwareWriteProtect = true;
        card.batteryLowWarning    = true;
        card.activeBankIndex      = 1u;

        card.banks[0].format         = vf50::state::CardBankFormat::VF50Syn;
        card.banks[0].hasVoicesPerfs = true;
        card.banks[0].hasPct         = true;
        card.banks[0].hasPefct       = true;
        card.banks[0].hasMct         = true;
        card.banks[0].hasSys         = true;
        card.banks[0].voices[0].setName("CardB0V00 ");
        card.banks[0].voices[99].setName("CardB0V99 ");
        card.banks[0].performances[0].setName("CardB0P00 ");
        card.banks[0].performances[99].setName("CardB0P99 ");
        card.banks[0].performances[0].partPans[0] = -41;
        card.banks[0].pefct = pef;
        card.banks[0].microOct = octMt;
        card.banks[0].microFull = fullMt;
        card.banks[0].sys = sys;

        card.banks[1].format = vf50::state::CardBankFormat::VF50Rhy;
        card.banks[1].voices[42].setName("CardB1V42 ");
        card.banks[1].performances[42].setName("CardB1P42 ");
        card.banks[1].rhythmSets[0].hasAll   = true;
        card.banks[1].rhythmSets[0].hasRSeq  = true;
        card.banks[1].rhythmSets[0].hasSetup = true;
        card.banks[1].rhythmSets[0].rhythmTempo = 144u;
        card.banks[1].rhythmSets[0].patterns[5].clear();
        card.banks[1].rhythmSets[0].patterns[5].bars = 2u;
        card.banks[1].rhythmSets[0].patterns[5].totalTicks = 384u;
        (void)card.banks[1].rhythmSets[0].patterns[5].addEventSorted(24u, 6u, 8u);
        srcProc->publishClassicSystemStateToAudio();

        // 2g. Sequencer State (Custom Sequence in Song 04, Tempo, Measure, Chain, Track Mutes)
        vf50::dsp::CustomSeqSongBuffer seq{};
        seq.clear();
        seq.setTitle("GATE7SONG");
        seq.initialTempoBpm = 136u;
        seq.timeSigNum      = 4u;
        seq.timeSigDen      = 4u;
        seq.ticksPerBar     = 192u;
        seq.totalBars       = 8u;
        seq.totalTicks      = 1536u;
        seq.playMode        = 1u;
        seq.perfPresetBank  = 0u;
        seq.perfNumber      = 12u;
        seq.trackChannels   = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
        seq.events[0]       = { 0u,   vf50::generated::DemoEventType::FmNoteOn,  0u, 60u, 112u };
        seq.events[1]       = { 96u,  vf50::generated::DemoEventType::FmNoteOff, 0u, 60u, 0u };
        seq.events[2]       = { 192u, vf50::generated::DemoEventType::DrumHit,   9u, 6u,  7u };
        seq.eventCount      = 3u;
        seq.valid           = true;
        srcProc->publishCustomSequenceToAudio(seq);
        srcProc->selectDemoSong(vf50::dsp::VF50DemoSongPlayer::kCustomSongSlot);
        srcProc->setDemoChainAll(true);
        srcProc->requestDemoSetTempoBpm(136);
        srcProc->requestDemoSeekToMeasure(4);
        srcProc->setSequencerTrackMuted(0u, true);
        srcProc->setSequencerTrackMuted(3u, true);
        srcProc->setSequencerTrackMuted(8u, true);

        juce::AudioBuffer<float> pumpBuf(2, 128);
        juce::MidiBuffer pumpMidi;
        srcProc->processBlock(pumpBuf, pumpMidi);

        // Serialize state
        juce::MemoryBlock stateBlob;
        srcProc->getStateInformation(stateBlob);
        if (stateBlob.getSize() < 16384u) {
            std::cerr << "[FAIL] Gate7 Test2: Serialized state blob unexpectedly small (" << stateBlob.getSize() << " B)\n";
            return false;
        }

        // Restore into a brand-new processor instance
        auto dstProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        dstProc->prepareToPlay(48000.0, 256);
        dstProc->setStateInformation(stateBlob.getData(), static_cast<int>(stateBlob.getSize()));
        dstProc->processBlock(pumpBuf, pumpMidi);

        if (dstProc->workflowMode() != vf50::state::OperatingMode::Classic ||
            std::abs(dstProc->uiScaleFactor() - 1.50f) > 1e-3f ||
            !dstProc->isVirtualKeyboardVisible() ||
            !dstProc->isProgrammerVisible() ||
            dstProc->lcdThemeIndex() != 2 ||
            dstProc->globalTransposeSemitones() != -5 ||
            !dstProc->isIntMemoryProtected() ||
            !dstProc->isCardMemoryProtected()) {
            std::cerr << "[FAIL] Gate7 Test2: OperatingMode, UI scale, or Memory Protect flags failed round-trip\n";
            return false;
        }

        for (std::size_t i = 0u; i < 100u; ++i) {
            const auto& sv = srcProc->hostMemory().getUserVoice(i);
            const auto& dv = dstProc->hostMemory().getUserVoice(i);
            if (sv.name != dv.name || sv.algorithm != dv.algorithm || sv.feedback != dv.feedback ||
                sv.operators[0].outputLevel != dv.operators[0].outputLevel ||
                sv.effect.effectType != dv.effect.effectType) {
                std::cerr << "[FAIL] Gate7 Test2: INTERNAL Voice " << i << " mismatch on round-trip\n";
                return false;
            }
            const auto& sp = srcProc->hostMemory().getUserPerformance(i);
            const auto& dp = dstProc->hostMemory().getUserPerformance(i);
            if (sp.name != dp.name || sp.assignMode != dp.assignMode ||
                sp.perfEffectSelect != dp.perfEffectSelect ||
                sp.instruments[0].volume != dp.instruments[0].volume ||
                srcProc->hostMemory().getUserPerformancePartPans(i) != dstProc->hostMemory().getUserPerformancePartPans(i)) {
                std::cerr << "[FAIL] Gate7 Test2: INTERNAL Performance " << i << " mismatch on round-trip\n";
                return false;
            }
        }

        for (std::size_t n = 0u; n < 12u; ++n) {
            if (dstProc->hostMemory().userOctaveMicrotuning()[n].fineStep64 != octMt[n].fineStep64) {
                std::cerr << "[FAIL] Gate7 Test2: User Octave Microtuning mismatch at note " << n << "\n";
                return false;
            }
        }
        for (std::size_t k = 0u; k < 128u; ++k) {
            if (dstProc->hostMemory().userFullMicrotuning()[k].coarseNote != fullMt[k].coarseNote ||
                dstProc->hostMemory().userFullMicrotuning()[k].fineStep64 != fullMt[k].fineStep64) {
                std::cerr << "[FAIL] Gate7 Test2: User Full Microtuning mismatch at key " << k << "\n";
                return false;
            }
        }

        const auto& dsys = dstProc->classicSystemSettings();
        const auto& dpef = dstProc->perfEffectBank();
        const auto& dcard = dstProc->virtualCard();
        if (dsys.masterTuningSteps != -22 ||
            dsys.synthVolume != 81u ||
            dsys.combineWithFunction != false ||
            dsys.controllerReset != true ||
            dsys.fixedVelocity != 104u ||
            dsys.velocityCurve != 6u ||
            dsys.egDampSpeed != 1u ||
            dsys.voiceDampOn != false ||
            std::string_view(dsys.powerOnMessage.data(), 40u) != kCustomGreeting ||
            dpef.delays[0].delayTimeCs != 47u || dpef.delays[0].pitchShift != -12 ||
            dpef.pans[1].panRange != 73u ||
            dpef.chords[2].chords[0][1] != 63) {
            std::cerr << "[FAIL] Gate7 Test2: Utility Setup / Performance Effects / 40-char Greeting mismatch\n";
            return false;
        }

        if (dcard.mediaType != vf50::state::CardMediaType::Mcd64Ram ||
            !dcard.hardwareWriteProtect ||
            dcard.activeBankIndex != 1u ||
            dcard.banks[0].format != vf50::state::CardBankFormat::VF50Syn ||
            dcard.banks[0].voices[0].voice.name != "CardB0V00 " ||
            dcard.banks[0].voices[99].voice.name != "CardB0V99 " ||
            dcard.banks[0].performances[0].perf.name != "CardB0P00 " ||
            dcard.banks[0].performances[0].partPans[0] != -41 ||
            dcard.banks[1].format != vf50::state::CardBankFormat::VF50Rhy ||
            dcard.banks[1].voices[42].voice.name != "CardB1V42 " ||
            dcard.banks[1].rhythmSets[0].rhythmTempo != 144u ||
            dcard.banks[1].rhythmSets[0].patterns[5].eventCount != 1u ||
            dcard.banks[1].rhythmSets[0].patterns[5].events[0].tick != 24u) {
            std::cerr << "[FAIL] Gate7 Test2: Virtual MCD64 Card banks mismatch on round-trip\n";
            return false;
        }

        if (!dstProc->hasCustomDemoSequence() ||
            dstProc->activeDemoSongTitle() != "GATE7SONG" ||
            dstProc->activeDemoSongIndex() != static_cast<int>(vf50::dsp::VF50DemoSongPlayer::kCustomSongSlot) ||
            dstProc->activeDemoTempoBpm() != 136 ||
            dstProc->activeDemoMeasure() != 4 ||
            !dstProc->demoChainAll() ||
            !dstProc->isSequencerTrackMuted(0u) ||
            dstProc->isSequencerTrackMuted(1u) ||
            !dstProc->isSequencerTrackMuted(3u) ||
            !dstProc->isSequencerTrackMuted(8u) ||
            dstProc->hostCustomSequence() == nullptr ||
            dstProc->hostCustomSequence()->eventCount != 3u) {
            std::cerr << "[FAIL] Gate7 Test2: Sequencer state failed round-trip\n";
            return false;
        }

        std::cout << "[PASS] Gate 7 Test 2: Full State Save/Reload Round-Trip (Modern/Classic, Scale, 100 Voices/Perfs, MCD64, Protect, Setup, Greeting, Sequencer) passed.\n";
    }

    // =========================================================================
    // TEST 3: Audio Engine Edge-Case Stress Suite
    // Sample rates: 44100, 48000, 88200, 96000 Hz
    // Buffer sizes: 32, 64, 137 (odd/non-power-of-two), 512, 2048, and 0 (empty block)
    // Polyphonic bursts (>32 notes), MODERN <-> CLASSIC toggles, live SEQ -> PERFORMANCE
    // =========================================================================
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> baseEd(proc->createEditor());
        auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
        if (editor == nullptr) {
            std::cerr << "[FAIL] Gate7 Test3: Failed to create editor for stress test\n";
            return false;
        }
        auto& canvas = editor->mainCanvas();

        constexpr std::array<double, 4> kSampleRates = { 44100.0, 48000.0, 88200.0, 96000.0 };
        constexpr std::array<int, 6>    kBufferSizes = { 32, 64, 137, 512, 2048, 0 };

        float globalMaxPeak = 0.0f;
        uint64_t totalSamplesChecked = 0ULL;

        for (const double sr : kSampleRates) {
            for (const int bufSz : kBufferSizes) {
                proc->prepareToPlay(sr, bufSz);

                // Start isolated ROM Demo Song 0 ("Victory!") so live SEQ -> PERFORMANCE transitions run under load
                proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kFactoryDemoSongBase);

                juce::AudioBuffer<float> audioBuf(2, std::max(0, bufSz));

                for (int block = 0; block < 12; ++block) {
                    audioBuf.clear();
                    juce::MidiBuffer midi;

                    // Rapid PG Programmer toggle during active audio rendering
                    canvas.pressProgrammerToggleButton();

                    // Live SEQ -> PERFORMANCE -> SINGLE switching during rendering
                    if (block % 3 == 0) {
                        canvas.pressSeqButton();
                    } else if (block % 3 == 1) {
                        canvas.pressPerformanceButton();
                    } else {
                        canvas.pressSingleButton();
                    }

                    // Trigger >32 simultaneous polyphonic notes (40 FM notes + 16 Drum notes = 56 simultaneous notes!)
                    if (bufSz > 0) {
                        if (block % 2 == 0) {
                            for (int n = 0; n < 40; ++n) {
                                const int ch = (n % 8) + 1;
                                const int note = 36 + (n % 60);
                                const uint8_t vel = static_cast<uint8_t>(60 + (n % 67));
                                const int pos = (n * 3) % bufSz;
                                midi.addEvent(juce::MidiMessage::noteOn(ch, note, vel), pos);
                            }
                            for (int d = 0; d < 16; ++d) {
                                const int drumNote = 31 + (d * 3) % 61;
                                const int pos = (d * 5) % bufSz;
                                midi.addEvent(juce::MidiMessage::noteOn(10, drumNote, uint8_t{ 122 }), pos);
                            }
                        } else {
                            // Release burst + pitch bend & mod wheel sweeps
                            for (int n = 0; n < 40; ++n) {
                                const int ch = (n % 8) + 1;
                                const int note = 36 + (n % 60);
                                const int pos = (n * 2) % bufSz;
                                midi.addEvent(juce::MidiMessage::noteOff(ch, note), pos);
                            }
                            midi.addEvent(juce::MidiMessage::pitchWheel(1, (block * 1300) & 0x3FFF), 0);
                            midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, (block * 19) & 0x7F), 0);
                        }
                    }

                    proc->processBlock(audioBuf, midi);

                    for (int ch = 0; ch < 2; ++ch) {
                        const float* samples = audioBuf.getReadPointer(ch);
                        for (int s = 0; s < bufSz; ++s) {
                            const float val = samples[s];
                            const int fpClass = std::fpclassify(val);
                            if (!std::isfinite(val) || fpClass == FP_SUBNORMAL || fpClass == FP_NAN || fpClass == FP_INFINITE) {
                                std::cerr << "[FAIL] Gate7 Test3: NaN/Inf/denormal sample detected at sr=" << sr
                                          << " bufSz=" << bufSz << " block=" << block << " ch=" << ch << " s=" << s
                                          << " val=" << val << "\n";
                                return false;
                            }
                            const float absVal = std::abs(val);
                            if (absVal > 4.0f) {
                                std::cerr << "[FAIL] Gate7 Test3: Peak output exceeded safe bound 4.0f (" << absVal
                                          << ") at sr=" << sr << " bufSz=" << bufSz << "\n";
                                return false;
                            }
                            globalMaxPeak = std::max(globalMaxPeak, absVal);
                            ++totalSamplesChecked;
                        }
                    }
                }
                proc->requestDemoStop();
            }
        }

        if (globalMaxPeak < 0.05f || globalMaxPeak > 4.0f) {
            std::cerr << "[FAIL] Gate7 Test3: Unexpected global peak across stress suite: " << globalMaxPeak << "\n";
            return false;
        }
        std::cout << "[PASS] Gate 7 Test 3: Audio Engine Edge-Case Stress Suite (" << totalSamplesChecked
                  << " samples verified across 44.1/48/88.2/96 kHz & blocks 0/32/64/137/512/2048, peak="
                  << globalMaxPeak << " <= 4.0f, 0 NaN/Inf/denormals) passed.\n";
    }

    std::cout << "[PASS] Gate 7 Pre-Release Hardening, BYOR/Trademark Audit, State Round-Trip & DSP Stress Suite 100% PASSED!\n";
    return true;
}

bool runDiskExtensionAndSubsystemIsolationSelfTests() {
    std::cout << "Running Disk Audit Synthetic Tests (Official VIARCB Extensions & Strict Subsystem Isolation)...\n";
    using vf50::disk::VF50DiskManager;
    using vf50::disk::VF50FileKind;
    using vf50::disk::VF50LoadFilter;

    // 1. Official hardware extension mapping + Modern fallbacks + strict rejection of non-VF50 extensions
    if (VF50DiskManager::classifyFileKind("SONG1.V01") != VF50FileKind::AllData
        || VF50DiskManager::classifyFileKind("SONG99.V99") != VF50FileKind::AllData
        || VF50DiskManager::classifyFileKind("MODERN.ALL") != VF50FileKind::AllData
        || VF50DiskManager::classifyFileKind("SYNTH.I01") != VF50FileKind::VoiceBank
        || VF50DiskManager::classifyFileKind("SYNTH.I99") != VF50FileKind::VoiceBank
        || VF50DiskManager::classifyFileKind("SEQ1.A01") != VF50FileKind::Sequence
        || VF50DiskManager::classifyFileKind("SEQ99.A99") != VF50FileKind::Sequence
        || VF50DiskManager::classifyFileKind("MODERN.SEQ") != VF50FileKind::Sequence
        || VF50DiskManager::classifyFileKind("DRUMS.R01") != VF50FileKind::RhythmData
        || VF50DiskManager::classifyFileKind("MODERN.RHY") != VF50FileKind::RhythmData
        || VF50DiskManager::classifyFileKind("BANK1.C01") != VF50FileKind::CardData
        || VF50DiskManager::classifyFileKind("MODERN.CRD") != VF50FileKind::CardData
        || VF50DiskManager::classifyFileKind("BULK1.B01") != VF50FileKind::SysEx
        || VF50DiskManager::classifyFileKind("MODERN.MDR") != VF50FileKind::SysEx
        || VF50DiskManager::classifyFileKind("PATCH.SYX") != VF50FileKind::SysEx
        || VF50DiskManager::classifyFileKind("FILE0000._DD") != VF50FileKind::Unknown
        || VF50DiskManager::classifyFileKind("FILE0001._dd") != VF50FileKind::Unknown
        || VF50DiskManager::classifyFileKind("BAD.V00") != VF50FileKind::Unknown
        || VF50DiskManager::classifyFileKind("BAD.S01") != VF50FileKind::Unknown
        || VF50DiskManager::classifyFileKind("README.TXT") != VF50FileKind::Unknown
        || VF50DiskManager::classifyFileKind("AWESOME!.ZIP") != VF50FileKind::Unknown
        || VF50DiskManager::classifyFileKind("RECOVERY.CHK") != VF50FileKind::Unknown
        || VF50DiskManager::bytesToKbCeil(0u) != 0u
        || VF50DiskManager::bytesToKbCeil(1u) != 1u
        || VF50DiskManager::bytesToKbCeil(1025u) != 2u
        || VF50DiskManager::bytesToKbCeil(65537u) != 65u
        || VF50DiskManager::bytesToKbCeil(74752u) != 73u) {
        std::cerr << "[FAIL] DiskAudit: classifyFileKind or bytesToKbCeil violated VIARCB / allowlist / 32-bit KB rules!\n";
        return false;
    }

    if (!VF50DiskManager::isHardwareExtensionForKind("TEST.V01", VF50FileKind::AllData)
        || VF50DiskManager::isHardwareExtensionForKind("TEST.ALL", VF50FileKind::AllData)
        || VF50DiskManager::isHardwareExtensionForKind("TEST.V00", VF50FileKind::AllData)
        || !VF50DiskManager::isHardwareExtensionForKind("TEST.I01", VF50FileKind::VoiceBank)
        || VF50DiskManager::isHardwareExtensionForKind("TEST.V01", VF50FileKind::VoiceBank)
        || VF50DiskManager::isHardwareExtensionForKind("TEST.A01", VF50FileKind::VoiceBank)
        || VF50DiskManager::isHardwareExtensionForKind("TEST.SYX", VF50FileKind::VoiceBank)
        || !VF50DiskManager::isHardwareExtensionForKind("TEST.A01", VF50FileKind::Sequence)
        || VF50DiskManager::isHardwareExtensionForKind("TEST.SEQ", VF50FileKind::Sequence)
        || !VF50DiskManager::isHardwareExtensionForKind("TEST.R01", VF50FileKind::RhythmData)
        || !VF50DiskManager::isHardwareExtensionForKind("TEST.C01", VF50FileKind::CardData)
        || !VF50DiskManager::isHardwareExtensionForKind("TEST.B01", VF50FileKind::SysEx)) {
        std::cerr << "[FAIL] DiskAudit: isHardwareExtensionForKind strict Classic filter check failed!\n";
        return false;
    }

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(44100.0, 256);
    vf50::gui::VF50MainCanvas canvas(*proc);
    auto& lcd = canvas.lcdDisplay();

    // Build synthetic hardware payloads:
    //  a) SYN (.I01): 26,624B with greeting "SYN_GREET...", Perf[0]="SynPerf00 ", Voice[0]="SynVoice00"
    std::vector<uint8_t> hwSynI01(0x6800u, 0x00u);
    {
        const char* g = ">>>>    SYNTH GREETING LOADED !!    <<<<";
        for (std::size_t i = 0u; i < 16u; ++i) hwSynI01[0x2Bu + i] = static_cast<uint8_t>(g[i]);
        for (std::size_t i = 0u; i < 24u; ++i) hwSynI01[0x4Bu + i] = static_cast<uint8_t>(g[16u + i]);
        vf50::state::StoredPerformanceSlot p0(testRom().factoryPerformances[1]);
        p0.setName("SynPerf00 ");
        vf50::state::StoredPerformanceSlot p1(testRom().factoryPerformances[2]);
        p1.setName("SynPerf01 ");
        std::memcpy(&hwSynI01[0x0400u], p0.perf.rawRomBytes.data(), 97u);
        std::memcpy(&hwSynI01[0x0400u + 128u], p1.perf.rawRomBytes.data(), 97u);

        vf50::state::StoredVoiceSlot v0(testRom().factoryVoices[3]);
        v0.setName("SynVoice00");
        std::memcpy(&hwSynI01[0x3600u], v0.voice.rawRomBytes.data(), 88u);
        std::memcpy(&hwSynI01[0x3600u + 94u], v0.voice.rawRomBytes.data() + 88u, 7u);
    }

    //  b) SEQ (.A01): HardwareSeqV1 song (132 BPM, title "SeqOnly!")
    std::vector<uint8_t> hwSeqA01 = {
        0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u, 0x00u, 0x03u, 0x00u, 132u,
        'S','e','q','O','n','l','y','!',
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
        1u, 0u, 10u, 0u, 0u, 0u,
        0xF0u, 0x00u, 0x98u, 64u, 100u, 0x30u, 0x98u, 67u, 96u, 0xF5u, 0xF2u, 0xF9u
    };

    //  c) R.SEQ (.R01): 1KB Rhythm Setup (vol=91 @ 0x20) + $3800 Rhythm RAM (pattern 0 with accented hit + Rhythm Song 1 with P00, I00, ||:, :|| x 3, Vol, Tempo, Mark!)
    std::vector<uint8_t> hwRhyR01(0x0800u, 0x00u);
    {
        hwRhyR01[0x20u] = 91u; // Rhythm master volume = 91
        for (std::size_t d = 0u; d < 61u; ++d) {
            hwRhyR01[0x100u + d] = 12u; // vol
            hwRhyR01[0x13Du + d] = 3u;  // center pan
            hwRhyR01[0x17Au + d] = static_cast<uint8_t>(31u + d);
        }
        hwRhyR01[0x100u + 0u] = 15u; // Drum 0 vol = 15
        uint8_t* ram = &hwRhyR01[0x0400u];
        ram[0] = 0x80u; ram[1] = 0x00u; // Pattern 0 populated at offset 0
        ram[0xC8u] = 0x80u; ram[0xC9u] = 0x05u; // Rhythm Song 1 (S1) populated at offset 5
        // Pattern 0 stream (5 bytes): drum 0 @ tick 0, delta +48, accented drum 5 (+2 -> acc 7), delta 0
        ram[0x1E2u + 0u] = 0u;
        ram[0x1E2u + 1u] = 0xB0u;
        ram[0x1E2u + 2u] = static_cast<uint8_t>(0x40u | 5u);
        ram[0x1E2u + 3u] = 0x02u; // +2 accent -> level 7
        ram[0x1E2u + 4u] = 0x80u;
        // Rhythm Song 1 stream @ +5 (17 bytes):
        //   0xC8 (||:), 0x64 (P00), 0x00 (I00), 0xC9 0x02 0xC9 (:|| x 3),
        //   0xCA 0xFC 0xCA (Vol -4), 0xCB 0x06 0x04 0xCB (Tempo +6 / 4 beats), 0xCC 'B' 0xCC (Mark B)
        const std::array<uint8_t, 16> song1Bytes = {
            0xC8u, 0x64u, 0x00u, 0xC9u, 0x02u, 0xC9u,
            0xCAu, 0xFCu, 0xCAu,
            0xCBu, 0x06u, 0x04u, 0xCBu,
            0xCCu, static_cast<uint8_t>('B'), 0xCCu
        };
        std::memcpy(ram + 0x1E2u + 5u, song1Bytes.data(), song1Bytes.size());
        const uint16_t r01StreamEnd = static_cast<uint16_t>(5u + song1Bytes.size());
        ram[0xD8u] = static_cast<uint8_t>((r01StreamEnd >> 8u) & 0xFFu);
        ram[0xD9u] = static_cast<uint8_t>(r01StreamEnd & 0xFFu);
        ram[0xDAu + 0u] = 3u; // 4/4
        ram[0x13Eu + 0u] = 0u; // 1 bar
        // Plant real hardware "NewSong " song names at +0x1A2 (which lands on slot 3 +57!) to verify NO Voice corruption!
        for (std::size_t s = 0u; s < 8u; ++s) {
            std::memcpy(ram + 0x1A2u + s * 8u, "NewSong ", 8u);
        }
        std::memcpy(ram + 0x1A2u, "Groove1 ", 8u);
    }

    //  d) ALL (.V01): 1KB Setup + 100 Perfs + 100 Voices + Rhythm (ptn 1 + Rhythm Song 1 linked to P00 & I01 with :|| x 3) + HardwareSeqV1 ("AllSong!", buf[31]=1, buf[32]=0)
    std::vector<uint8_t> hwAllV01(0x7800u, 0x00u);
    {
        vf50::state::StoredPerformanceSlot p0(testRom().factoryPerformances[4]);
        p0.setName("AllPerf00 ");
        vf50::state::StoredPerformanceSlot p1(testRom().factoryPerformances[5]);
        p1.setName("AllPerf01 ");
        std::memcpy(&hwAllV01[0x0400u], p0.perf.rawRomBytes.data(), 97u);
        std::memcpy(&hwAllV01[0x0400u + 128u], p1.perf.rawRomBytes.data(), 97u);

        vf50::state::StoredVoiceSlot v0(testRom().factoryVoices[6]);
        v0.setName("AllVoice00");
        std::memcpy(&hwAllV01[0x3600u], v0.voice.rawRomBytes.data(), 88u);
        std::memcpy(&hwAllV01[0x3600u + 94u], v0.voice.rawRomBytes.data() + 88u, 7u);

        // Rhythm at 0x6800 (1KB setup) + 0x6C00 ($3800 RAM, pattern 1 + Rhythm Song 1)
        hwAllV01[0x6800u + 0x20u] = 88u;
        uint8_t* ram = &hwAllV01[0x6C00u];
        ram[2] = 0x80u; ram[3] = 0x00u; // Pattern 1 populated at offset 0
        ram[0xC8u] = 0x80u; ram[0xC9u] = 0x04u; // Rhythm Song 1 populated at offset 4
        ram[0xDAu + 1u] = 3u;
        ram[0x13Eu + 1u] = 0u;
        std::memcpy(ram + 0x1A2u, "AllRhyS1", 8u);
        ram[0x1E2u + 0u] = 2u;    // Drum 2 @ tick 0
        ram[0x1E2u + 1u] = 0xB0u; // +48 ticks
        ram[0x1E2u + 2u] = 6u;    // Drum 6 @ tick 48
        ram[0x1E2u + 3u] = 0x80u;
        // Song 1: ||: P00 I01 :|| x 3 (0xC8, 0x64, 0x01, 0xC9, 0x02, 0xC9)
        const std::array<uint8_t, 6> allRhySong = { 0xC8u, 0x64u, 0x01u, 0xC9u, 0x02u, 0xC9u };
        std::memcpy(ram + 0x1E2u + 4u, allRhySong.data(), allRhySong.size());
        ram[0xD8u] = 0x00u;
        ram[0xD9u] = static_cast<uint8_t>(4u + allRhySong.size());

        // HardwareSeqV1 at 0x7400 linked to Rhythm Song 1 (buf[31]=1, buf[32]=0)
        const std::vector<uint8_t> allSeq = {
            0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u, 0x00u, 0x03u, 0x00u, 118u,
            'A','l','l','S','o','n','g','!',
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
            1u, 0u, 0u, 1u, 0u, 0u,
            0xF0u, 0x00u, 0x98u, 60u, 100u, 0xF5u, 0xF2u, 0xF9u
        };
        std::memcpy(&hwAllV01[0x7400u], allSeq.data(), allSeq.size());
    }

    //  e) CARD (.C01): 26,624B hardware card dump ("CrdPerf00 ", "CrdVoice00")
    std::vector<uint8_t> hwCardC01(0x6800u, 0x00u);
    {
        vf50::state::StoredPerformanceSlot p0(testRom().factoryPerformances[7]);
        p0.setName("CrdPerf00 ");
        vf50::state::StoredPerformanceSlot p1(testRom().factoryPerformances[8]);
        p1.setName("CrdPerf01 ");
        std::memcpy(&hwCardC01[0x0400u], p0.perf.rawRomBytes.data(), 97u);
        std::memcpy(&hwCardC01[0x0400u + 128u], p1.perf.rawRomBytes.data(), 97u);

        vf50::state::StoredVoiceSlot v0(testRom().factoryVoices[9]);
        v0.setName("CrdVoice00");
        std::memcpy(&hwCardC01[0x3600u], v0.voice.rawRomBytes.data(), 88u);
        std::memcpy(&hwCardC01[0x3600u + 94u], v0.voice.rawRomBytes.data() + 88u, 7u);
    }

    // Mount synthetic FAT12 floppy containing all 5 hardware types + >64KB file (74,752B = 73KB) + Modern .SYX + non-VF50 ._DD/.TXT!
    std::vector<uint8_t> bigAllV02(74752u, 0xA5u); // 73 KB (> 65,536 bytes: verifies 32-bit FAT12 size without 16-bit wrap!)
    bigAllV02.front() = 0x11u;
    bigAllV02[65536u] = 0x5Au;
    bigAllV02.back()  = 0xEEu;
    {
        auto img = VF50DiskManager::createFormattedFat12Image(false, "AUD_TEST");
        juce::String st;
        if (!canvas.diskManager().mountFat12ImageMemory(std::move(img), "AUDIT.IMG", st)) {
            std::cerr << "[FAIL] DiskAudit: failed to mount FAT12 image\n";
            return false;
        }
        const std::array<uint8_t, 8> dummySyx = { 0xF0u, 0x43u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0xF7u };
        const std::array<uint8_t, 16> junkBytes = { 'N','O','T',' ','A',' ','V','5','0',' ','F','I','L','E','!',' ' };
        if (!canvas.diskManager().writeFileToMountedFat12("KILL MIX.V01", hwAllV01.data(), hwAllV01.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("SYNTH_1.I01", hwSynI01.data(), hwSynI01.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("AWESOME!.A01", hwSeqA01.data(), hwSeqA01.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("DRUMZ 1.R01", hwRhyR01.data(), hwRhyR01.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("CARD_1.C01", hwCardC01.data(), hwCardC01.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("BIGSONG.V02", bigAllV02.data(), bigAllV02.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("MODERN.SYX", dummySyx.data(), dummySyx.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("FILE0000._DD", junkBytes.data(), junkBytes.size(), st)
            || !canvas.diskManager().writeFileToMountedFat12("README.TXT", junkBytes.data(), junkBytes.size(), st)) {
            std::cerr << "[FAIL] DiskAudit: failed to populate synthetic FAT12 disk: " << st << "\n";
            return false;
        }

        // Verify non-VF50 extensions (._DD, .TXT) are completely excluded from entries_ AND >64KB file reports exact 74,752B (73KB)!
        std::size_t bigIdx = SIZE_MAX;
        for (std::size_t i = 0u; i < canvas.diskManager().entries().size(); ++i) {
            const auto& e = canvas.diskManager().entries()[i];
            if (e.name.find("._DD") != std::string::npos || e.name.find(".TXT") != std::string::npos) {
                std::cerr << "[FAIL] DiskAudit: non-VF50 file leaked into FAT12 directory: " << e.name << "\n";
                return false;
            }
            if (e.name == "BIGSONG.V02") {
                bigIdx = i;
            }
        }
        std::vector<uint8_t> bigReadBack;
        if (bigIdx == SIZE_MAX
            || canvas.diskManager().entries()[bigIdx].sizeBytes != 74752u
            || VF50DiskManager::bytesToKbCeil(canvas.diskManager().entries()[bigIdx].sizeBytes) != 73u
            || !canvas.diskManager().readEntryBytes(bigIdx, bigReadBack, st)
            || bigReadBack.size() != 74752u
            || bigReadBack.front() != 0x11u
            || bigReadBack[65536u] != 0x5Au
            || bigReadBack.back() != 0xEEu) {
            std::cerr << "[FAIL] DiskAudit: >64KB FAT12 32-bit size or cluster-chain extraction failed!\n";
            return false;
        }

        // Verify Classic Mode UT DISK > Dir excludes MODERN.SYX & ._DD/.TXT (6 hardware files) and formats BIGSONG.V02 as 73K!
        canvas.pressDiskButton();
        canvas.pressSoftKey(6); // >Dir
        const juce::String dirRow0 = lcd.getLineAsString(0);
        if (!dirRow0.contains("Total:  6 Files")) {
            std::cerr << "[FAIL] DiskAudit: Classic Directory expected 6 hardware files (excluding .SYX/._DD/.TXT), got '" << dirRow0 << "'\n";
            return false;
        }
        for (int step = 0; step < 5; ++step) canvas.pressKeypadYesButton(); // advance to entry #6 (BIGSONG.V02)
        const juce::String bigDirRow1 = lcd.getLineAsString(1);
        canvas.pressDiskButton(); // exit disk
        if (!bigDirRow1.contains("BIGSONG") || !bigDirRow1.contains("(ALL)") || !bigDirRow1.contains("73K")) {
            std::cerr << "[FAIL] DiskAudit: Classic Directory >64KB formatting failed, got '" << bigDirRow1 << "'\n";
            return false;
        }
    }

    // 2. Verify Classic Mode DISK > Load strict category filtering (NO cross-category bleed!)
    auto checkClassicCategoryFile = [&](int typeKey0To4, const char* expectedStem, const char* expectedTag) -> bool {
        canvas.pressDiskButton();
        canvas.pressSoftKey(1); // >Load
        canvas.pressSoftKey(typeKey0To4);
        const juce::String row1 = lcd.getLineAsString(1);
        canvas.pressDiskButton(); // exit disk
        if (!row1.contains(expectedStem) || !row1.contains(expectedTag)) {
            std::cerr << "[FAIL] DiskAudit: Classic Load category " << typeKey0To4
                      << " expected '" << expectedStem << "' (" << expectedTag << "), got '" << row1 << "'\n";
            return false;
        }
        return true;
    };
    if (!checkClassicCategoryFile(0, "KILL MIX", "(ALL)")
        || !checkClassicCategoryFile(1, "SYNTH_1", "(SYN)")
        || !checkClassicCategoryFile(2, "AWESOME!", "(SEQ)")
        || !checkClassicCategoryFile(3, "DRUMZ 1", "(R.SEQ)")
        || !checkClassicCategoryFile(4, "CARD_1", "(CARD)")) {
        return false;
    }

    // 3. Plant sentinel states in ALL subsystems (Synth, Sequencer, Rhythm, Card Bank 0 & Bank 1)
    auto plantSentinels = [&]() {
        vf50::state::StoredVoiceSlot sv0(testRom().factoryVoices[0]);
        sv0.setName("SentVce00 ");
        vf50::state::StoredVoiceSlot sv3(testRom().factoryVoices[3]);
        sv3.setName("KeepVce03 ");
        vf50::state::StoredPerformanceSlot sp0(testRom().factoryPerformances[0]);
        sp0.setName("SentPrf00 ");
        proc->hostMemory().storeUserVoice(0u, sv0.voice);
        proc->hostMemory().storeUserVoice(3u, sv3.voice);
        proc->hostMemory().storeUserPerformance(0u, sp0.perf);

        auto& ptn5 = proc->hostMemory().mutableUserRhythmPattern(5u);
        ptn5.clear();
        (void)ptn5.addEventSorted(24u, 7u, 8u);
        proc->hostMemory().mutableActiveRhythmKit().instruments[0].volume = 9u;
        proc->hostMemory().mutableActiveRhythmKit().rhythmVolume = 77u;
        for (auto& s : proc->mutableHostRhythmSongs()) s.clear();
        proc->mutableHostRhythmSongs()[0].name = { 'S','e','n','t','R','S','g','1' };
        proc->mutableHostRhythmSongs()[0].partCount = 1u;
        proc->mutableHostRhythmSongs()[0].parts[0] = { vf50::generated::RhythmSongPartType::Pattern, true, 5u, 2u, 0, 0, 4u, 'A' };
        proc->publishRhythmSongsToAudio();
        proc->pushHostMemoryToAudio();

        vf50::dsp::CustomSeqSongBuffer sentSeq{};
        sentSeq.clear();
        sentSeq.setTitle("SentSeq!");
        sentSeq.initialTempoBpm = 97u;
        sentSeq.eventCount = 1u;
        sentSeq.events[0] = { 0u, vf50::generated::DemoEventType::FmNoteOn, 0u, 60u, 80u };
        sentSeq.valid = true;
        proc->publishCustomSequenceToAudio(sentSeq);

        auto& card = proc->mutableVirtualCard();
        card.activeBankIndex = 0u;
        card.banks[0].voices[0].setName("SentCrdB0 ");
        card.banks[1].voices[0].setName("SentCrdB1 ");
        proc->publishClassicSystemStateToAudio();
    };

    // 3a. Load > SYN (.I01): overwrites ONLY Voices + Perfs + Synth Setup; Seq/Rhythm/Card 100% untouched!
    plantSentinels();
    canvas.pressDiskButton();
    canvas.pressSoftKey(1); // >Load
    canvas.pressSoftKey(1); // SYN
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton();
    canvas.pressDiskButton();
    if (proc->hostMemory().getUserVoice(0u).name != "SynVoice00"
        || proc->hostMemory().getUserPerformance(0u).name != "SynPerf00 "
        || std::string_view(proc->classicSystemSettings().powerOnMessage.data(), 40u).find("SYNTH GREETING") == std::string_view::npos
        || proc->hostCustomSequence() == nullptr
        || proc->hostCustomSequence()->title() != "SentSeq!"
        || proc->hostCustomSequence()->initialTempoBpm != 97u
        || proc->hostMemory().userRhythmPattern(5u).eventCount != 1u
        || proc->hostMemory().activeRhythmKit().instruments[0].volume != 9u
        || std::string_view(proc->hostRhythmSongs()[0].name.data(), 8u) != "SentRSg1"
        || proc->virtualCard().banks[0].voices[0].voice.name != "SentCrdB0 ") {
        std::cerr << "[FAIL] DiskAudit: Load > SYN violated strict subsystem isolation!\n";
        return false;
    }

    // 3b. Load > SEQ (.A01) with INT Memory Protect ON: overwrites ONLY Sequencer; Synth/Rhythm/Card untouched!
    plantSentinels();
    proc->setIntMemoryProtected(true);
    canvas.pressDiskButton();
    canvas.pressSoftKey(1); // >Load
    canvas.pressSoftKey(2); // SEQ
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton();
    canvas.pressDiskButton();
    proc->setIntMemoryProtected(false);
    if (proc->hostCustomSequence() == nullptr
        || proc->hostCustomSequence()->title() != "SeqOnly!"
        || proc->hostCustomSequence()->initialTempoBpm != 132u
        || proc->hostMemory().getUserVoice(0u).name != "SentVce00 "
        || proc->hostMemory().getUserPerformance(0u).name != "SentPrf00 "
        || proc->hostMemory().userRhythmPattern(5u).eventCount != 1u
        || proc->virtualCard().banks[0].voices[0].voice.name != "SentCrdB0 ") {
        std::cerr << "[FAIL] DiskAudit: Load > SEQ violated strict subsystem isolation or Memory Protect exemption!\n";
        return false;
    }

    // 3c. Load > R.SEQ (.R01): overwrites ONLY Rhythm patterns, songs & setup; Voice #3 ("KeepVce03 "), Perf, Seq, Card untouched!
    plantSentinels();
    canvas.pressDiskButton();
    canvas.pressSoftKey(1); // >Load
    canvas.pressSoftKey(3); // R.SEQ
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton();
    canvas.pressDiskButton();
    const auto& decR01Song = proc->hostRhythmSongs()[0];
    if (proc->hostMemory().userRhythmPattern(0u).eventCount != 2u
        || proc->hostMemory().userRhythmPattern(0u).events[1].drumIndex != 5u
        || proc->hostMemory().userRhythmPattern(0u).events[1].velocityLevel != 7u
        || proc->hostMemory().userRhythmPattern(5u).eventCount != 0u
        || proc->hostMemory().activeRhythmKit().instruments[0].volume != 15u
        || proc->hostMemory().activeRhythmKit().rhythmVolume != 91u
        || std::string_view(decR01Song.name.data(), 8u) != "Groove1 "
        || decR01Song.partCount != 7u
        || decR01Song.parts[0].type != vf50::generated::RhythmSongPartType::RepeatBegin
        || decR01Song.parts[1].type != vf50::generated::RhythmSongPartType::Pattern
        || decR01Song.parts[1].ptnIsUser != false || decR01Song.parts[1].ptnIndex != 0u
        || decR01Song.parts[2].type != vf50::generated::RhythmSongPartType::Pattern
        || decR01Song.parts[2].ptnIsUser != true || decR01Song.parts[2].ptnIndex != 0u
        || decR01Song.parts[3].type != vf50::generated::RhythmSongPartType::RepeatEnd || decR01Song.parts[3].repeatCount != 3u
        || decR01Song.parts[4].type != vf50::generated::RhythmSongPartType::Volume || decR01Song.parts[4].volDelta != -4
        || decR01Song.parts[5].type != vf50::generated::RhythmSongPartType::Tempo || decR01Song.parts[5].tempoDelta != 6 || decR01Song.parts[5].tempoBeats != 4u
        || decR01Song.parts[6].type != vf50::generated::RhythmSongPartType::Mark || decR01Song.parts[6].markChar != 'B'
        || proc->hostMemory().getUserVoice(0u).name != "SentVce00 "
        || proc->hostMemory().getUserVoice(3u).name != "KeepVce03 "
        || proc->hostMemory().getUserPerformance(0u).name != "SentPrf00 "
        || proc->hostCustomSequence()->title() != "SentSeq!"
        || proc->virtualCard().banks[0].voices[0].voice.name != "SentCrdB0 ") {
        std::cerr << "[FAIL] DiskAudit: Load > R.SEQ violated Rhythm Song/Pattern decoding or strict subsystem isolation!\n";
        return false;
    }

    // 3d. Load > CARD (.C01): overwrites ONLY active Card Bank 0; Bank 1 + Internal Synth/Rhythm/Seq untouched!
    plantSentinels();
    canvas.pressDiskButton();
    canvas.pressSoftKey(1); // >Load
    canvas.pressSoftKey(4); // CARD
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton();
    canvas.pressDiskButton();
    if (proc->virtualCard().banks[0].voices[0].voice.name != "CrdVoice00"
        || proc->virtualCard().banks[0].performances[0].perf.name != "CrdPerf00 "
        || proc->virtualCard().banks[1].voices[0].voice.name != "SentCrdB1 "
        || proc->hostMemory().getUserVoice(0u).name != "SentVce00 "
        || proc->hostMemory().getUserPerformance(0u).name != "SentPrf00 "
        || proc->hostMemory().userRhythmPattern(5u).eventCount != 1u
        || proc->hostCustomSequence()->title() != "SentSeq!") {
        std::cerr << "[FAIL] DiskAudit: Load > CARD violated strict subsystem isolation!\n";
        return false;
    }

    // 3e. Load > ALL (.V01): overwrites Synth, Sequencer, AND Rhythm (Patterns + Song 1) together; Card Bank 0 untouched!
    plantSentinels();
    canvas.pressDiskButton();
    canvas.pressSoftKey(1); // >Load
    canvas.pressSoftKey(0); // ALL
    canvas.pressSoftKey(6); // >GO
    canvas.pressKeypadYesButton();
    canvas.pressDiskButton();
    const auto& decAllRhySong = proc->hostRhythmSongs()[0];
    if (proc->hostMemory().getUserVoice(0u).name != "AllVoice00"
        || proc->hostMemory().getUserPerformance(0u).name != "AllPerf00 "
        || proc->hostMemory().userRhythmPattern(1u).eventCount != 2u
        || proc->hostMemory().userRhythmPattern(5u).eventCount != 0u
        || proc->hostMemory().activeRhythmKit().rhythmVolume != 88u
        || std::string_view(decAllRhySong.name.data(), 8u) != "AllRhyS1"
        || decAllRhySong.partCount != 4u
        || decAllRhySong.parts[1].ptnIsUser != false || decAllRhySong.parts[1].ptnIndex != 0u
        || decAllRhySong.parts[2].ptnIsUser != true  || decAllRhySong.parts[2].ptnIndex != 1u
        || decAllRhySong.parts[3].repeatCount != 3u
        || proc->hostCustomSequence()->title() != "AllSong!"
        || proc->hostCustomSequence()->initialTempoBpm != 118u
        || !proc->hostCustomSequence()->rhythmSyncIsSong
        || proc->hostCustomSequence()->rhythmTargetIdx != 0u
        || proc->virtualCard().banks[0].voices[0].voice.name != "SentCrdB0 ") {
        std::cerr << "[FAIL] DiskAudit: Load > ALL failed to populate Synth+Rhythm(Patterns+Song)+Seq or touched Card!\n";
        return false;
    }

    // 3f. Verify Sequencer playback starts linked Rhythm Song 1 (P00 + I01 x 3) in lockstep and triggers drums!
    {
        const uint64_t trigBefore = proc->rhythmTotalTriggerCount();
        proc->requestDemoStart(vf50::dsp::VF50DemoSongPlayer::kCustomSongSlot);
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        for (int b = 0; b < 40; ++b) {
            buf.clear();
            midi.clear();
            proc->processBlock(buf, midi);
        }
        const bool songLockstepActive = proc->isRhythmSongPlaying();
        const uint64_t trigAfter = proc->rhythmTotalTriggerCount();
        proc->requestDemoStop();
        buf.clear();
        midi.clear();
        proc->processBlock(buf, midi);
        if (!songLockstepActive || trigAfter <= trigBefore) {
            std::cerr << "[FAIL] DiskAudit: Sequencer playback failed to start linked Rhythm Song in lockstep (songPlaying="
                      << songLockstepActive << ", triggers=" << (trigAfter - trigBefore) << ")!\n";
            return false;
        }
    }

    // 4. Verify FAT12 Rename & Delete work on filenames with spaces & exclamation marks (KILL MIX.V01)
    {
        juce::String st;
        if (!canvas.diskManager().renameMountedEntry(0u, "RENAMED", st)
            || canvas.diskManager().entries()[0].name != "RENAMED.V01"
            || !canvas.diskManager().deleteMountedEntry(0u, st)) {
            std::cerr << "[FAIL] DiskAudit: FAT12 Rename/Delete failed on space-containing VF50 filename: " << st << "\n";
            return false;
        }
    }

    std::cout << "[PASS] DiskAudit Synthetic Suite: VIARCB mapping, Classic filtering & 5-way strict subsystem isolation 100% PASSED!\n";
    return true;
}

bool runRealFloppyImgAuditSelfTests(const juce::File& /*buildDir*/) {
    using vf50::disk::VF50DiskManager;
    using vf50::disk::VF50FileKind;
    using vf50::disk::VF50LoadFilter;

    const juce::File floppyDir = findTestDiskDirectory();
    if (!floppyDir.isDirectory()) {
        std::cout << "[SKIP] Real Floppy .IMG Validation skipped (VF50_TEST_DISK_DIR unset).\n";
        return true;
    }

    auto imgFiles = floppyDir.findChildFiles(juce::File::findFiles, true, "*.img;*.IMG;*.dsk;*.DSK");
    imgFiles.sort();
    if (imgFiles.isEmpty()) {
        std::cout << "[SKIP] Real Floppy .IMG Validation skipped (no .IMG/.DSK images found).\n";
        return true;
    }

    std::cout << "Running Real Hardware VF50 .IMG Floppy Validation (" << imgFiles.size() << " images, read-only)...\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(44100.0, 256);

    auto isCleanPrintableAscii = [](const std::string& s) noexcept -> bool {
        if (s.empty()) return false;
        for (char ch : s) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c < 0x20u || c > 0x7Eu) return false;
        }
        return true;
    };

    std::size_t totalEntriesVerified = 0u;
    std::size_t verifiedSynCount = 0u;
    std::size_t verifiedSeqCount = 0u;
    std::size_t verifiedRhyCount = 0u;
    std::size_t verifiedAllCount = 0u;
    std::size_t verifiedOver64KbCount = 0u;
    std::size_t verifiedRhySongsCount = 0u;

    for (const auto& imgFile : imgFiles) {
        // Strictly read-only: load bytes into RAM and mount via mountFat12ImageMemory (never flush back to fixture!)
        juce::MemoryBlock mb;
        if (!imgFile.loadFileAsData(mb) || mb.getSize() < 4096u) {
            std::cerr << "[FAIL] RealFloppy: failed to read " << imgFile.getFileName() << "\n";
            return false;
        }
        std::vector<uint8_t> rawCopy(mb.getSize());
        std::memcpy(rawCopy.data(), mb.getData(), mb.getSize());

        VF50DiskManager dm;
        juce::String st;
        if (!dm.mountFat12ImageMemory(std::move(rawCopy), imgFile.getFileName().toStdString(), st)) {
            std::cerr << "[FAIL] RealFloppy: failed to mount " << imgFile.getFileName() << ": " << st << "\n";
            return false;
        }

        const auto& entries = dm.entries();
        if (entries.empty()) {
            std::cerr << "[FAIL] RealFloppy: 0 entries enumerated in " << imgFile.getFileName() << "\n";
            return false;
        }

        for (std::size_t i = 0u; i < entries.size(); ++i) {
            const auto& e = entries[i];
            if (!isCleanPrintableAscii(e.name)) {
                std::cerr << "[FAIL] RealFloppy: non-ASCII filename in " << imgFile.getFileName() << " slot " << i << "\n";
                return false;
            }
            std::vector<uint8_t> bytes;
            if (!dm.readEntryBytes(i, bytes, st) || bytes.size() != e.sizeBytes) {
                std::cerr << "[FAIL] RealFloppy: exact byte count mismatch for " << imgFile.getFileName()
                          << "/" << e.name << " (expected " << e.sizeBytes << ", got " << bytes.size() << ")\n";
                return false;
            }
            if (e.sizeBytes > 65536u) {
                if (VF50DiskManager::bytesToKbCeil(e.sizeBytes) <= 64u) {
                    std::cerr << "[FAIL] RealFloppy: >64KB file wrapped KB calculation for " << e.name << "\n";
                    return false;
                }
                ++verifiedOver64KbCount;
            }
            if (e.kind == VF50FileKind::Unknown) {
                std::cerr << "[FAIL] RealFloppy: Unknown/non-VF50 file appeared in FAT12 directory: " << e.name << "\n";
                return false;
            }
            ++totalEntriesVerified;

            // Plant sentinels before testing each file type for strict subsystem isolation!
            vf50::state::StoredVoiceSlot sv3(testRom().factoryVoices[3]);
            sv3.setName("KeepVce03 ");
            proc->hostMemory().storeUserVoice(3u, sv3.voice);
            vf50::state::StoredPerformanceSlot sp0(testRom().factoryPerformances[0]);
            sp0.setName("KeepPrf00 ");
            proc->hostMemory().storeUserPerformance(0u, sp0.perf);
            auto& ptn99 = proc->hostMemory().mutableUserRhythmPattern(99u);
            ptn99.clear();
            (void)ptn99.addEventSorted(177u, 60u, 9u);
            proc->pushHostMemoryToAudio();

            auto isSentinelPtn99 = [&]() noexcept -> bool {
                const auto& p = proc->hostMemory().userRhythmPattern(99u);
                return p.eventCount == 1u
                    && p.events[0].tick == 177u
                    && p.events[0].drumIndex == 60u
                    && p.events[0].velocityLevel == 9u;
            };

            vf50::dsp::CustomSeqSongBuffer sentSeq{};
            sentSeq.clear();
            sentSeq.setTitle("SentSeq!");
            sentSeq.initialTempoBpm = 88u;
            sentSeq.eventCount = 1u;
            sentSeq.events[0] = { 0u, vf50::generated::DemoEventType::FmNoteOn, 0u, 60u, 80u };
            sentSeq.valid = true;
            proc->publishCustomSequenceToAudio(sentSeq);

            if (e.kind == VF50FileKind::VoiceBank && VF50DiskManager::isHardwareExtensionForKind(e.name, VF50FileKind::VoiceBank)) {
                // Real .I## (SYN): must populate voices & perfs, and leave Sequencer + Rhythm 100% UNTOUCHED!
                if (!dm.loadMountedEntry(i, *proc, VF50LoadFilter::SynthOnly, st)
                    || proc->hostMemory().getUserPerformance(0u).name == "KeepPrf00 "
                    || proc->hostCustomSequence() == nullptr
                    || proc->hostCustomSequence()->title() != "SentSeq!"
                    || proc->hostCustomSequence()->initialTempoBpm != 88u
                    || !isSentinelPtn99()) {
                    std::cerr << "[FAIL] RealFloppy: SYN load failed or contaminated SEQ/RHY for "
                              << imgFile.getFileName() << "/" << e.name << " (" << st << ")\n";
                    return false;
                }
                ++verifiedSynCount;
            } else if (e.kind == VF50FileKind::Sequence && VF50DiskManager::isHardwareExtensionForKind(e.name, VF50FileKind::Sequence)) {
                // Real .A## (SEQ): must populate sequence events & leave Synth + Rhythm 100% UNTOUCHED!
                if (!dm.loadMountedEntry(i, *proc, VF50LoadFilter::SequenceOnly, st)
                    || proc->hostCustomSequence() == nullptr
                    || proc->hostCustomSequence()->eventCount < 100u
                    || proc->hostMemory().getUserVoice(3u).name != "KeepVce03 "
                    || proc->hostMemory().getUserPerformance(0u).name != "KeepPrf00 "
                    || !isSentinelPtn99()) {
                    std::cerr << "[FAIL] RealFloppy: SEQ load failed or contaminated SYN/RHY for "
                              << imgFile.getFileName() << "/" << e.name << " (" << st << ")\n";
                    return false;
                }
                ++verifiedSeqCount;
            } else if (e.kind == VF50FileKind::RhythmData && VF50DiskManager::isHardwareExtensionForKind(e.name, VF50FileKind::RhythmData)) {
                // Real .R## (R.SEQ): must populate rhythm patterns & leave Voice #3 ("KeepVce03 "), Perf, Seq 100% UNTOUCHED!
                if (!dm.loadMountedEntry(i, *proc, VF50LoadFilter::RhythmOnly, st)
                    || proc->hostMemory().getUserVoice(3u).name != "KeepVce03 "
                    || proc->hostMemory().getUserPerformance(0u).name != "KeepPrf00 "
                    || proc->hostCustomSequence() == nullptr
                    || proc->hostCustomSequence()->title() != "SentSeq!"
                    || isSentinelPtn99()) {
                    std::cerr << "[FAIL] RealFloppy: R.SEQ load failed or contaminated SYN/SEQ for "
                              << imgFile.getFileName() << "/" << e.name << " (" << st << ")\n";
                    return false;
                }
                std::size_t popPtns = 0u;
                for (std::size_t p = 0u; p < 100u; ++p) {
                    if (proc->hostMemory().userRhythmPattern(p).eventCount > 0u) ++popPtns;
                }
                if (popPtns == 0u) {
                    std::cerr << "[FAIL] RealFloppy: R.SEQ loaded 0 rhythm patterns for "
                              << imgFile.getFileName() << "/" << e.name << "\n";
                    return false;
                }
                ++verifiedRhyCount;
            } else if (e.kind == VF50FileKind::AllData && VF50DiskManager::isHardwareExtensionForKind(e.name, VF50FileKind::AllData)) {
                // Real .V## (ALL): must populate voices, perfs, rhythm (clearing stale sentinel ptn 99), and sequence!
                if (!dm.loadMountedEntry(i, *proc, VF50LoadFilter::AllData, st)
                    || proc->hostMemory().getUserPerformance(0u).name == "KeepPrf00 "
                    || proc->hostCustomSequence() == nullptr
                    || proc->hostCustomSequence()->eventCount < 100u
                    || isSentinelPtn99()) {
                    std::cerr << "[FAIL] RealFloppy: ALL load failed for "
                              << imgFile.getFileName() << "/" << e.name << " (" << st << ")\n";
                    return false;
                }
                // Verify authentic 0..60 drum IDs on every decoded user pattern & verify real hardware Rhythm Songs!
                for (std::size_t p = 0u; p < 100u; ++p) {
                    const auto& up = proc->hostMemory().userRhythmPattern(p);
                    for (uint16_t ev = 0u; ev < up.eventCount; ++ev) {
                        if (up.events[ev].drumIndex >= 61u) {
                            std::cerr << "[FAIL] RealFloppy: out-of-range drum ID in " << e.name << "\n";
                            return false;
                        }
                    }
                }
                if (e.name == "LONELY.V09") {
                    const auto& s1 = proc->hostRhythmSongs()[0];
                    const auto& s2 = proc->hostRhythmSongs()[1];
                    if (s1.partCount != 9u || s1.parts[3].repeatCount != 4u || s1.parts[8].repeatCount != 21u
                        || s2.partCount < 30u || !proc->hostCustomSequence()->rhythmSyncIsSong) {
                        std::cerr << "[FAIL] RealFloppy: LONELY.V09 Rhythm Song decoding mismatch (s1.parts="
                                  << s1.partCount << ", s2.parts=" << s2.partCount << ")\n";
                        return false;
                    }
                    ++verifiedRhySongsCount;
                } else if (e.name == "CLUBSONG.V08") {
                    const auto& s1 = proc->hostRhythmSongs()[0];
                    if (s1.partCount != 4u || s1.parts[0].type != vf50::generated::RhythmSongPartType::RepeatBegin
                        || s1.parts[2].repeatCount != 60u || !proc->hostCustomSequence()->rhythmSyncIsSong) {
                        std::cerr << "[FAIL] RealFloppy: CLUBSONG.V08 Rhythm Song decoding mismatch!\n";
                        return false;
                    }
                    ++verifiedRhySongsCount;
                } else if (e.name == "FAR OUT!.V08") {
                    const auto& s2 = proc->hostRhythmSongs()[1];
                    if (s2.partCount != 50u || !proc->hostCustomSequence()->rhythmSyncIsSong
                        || proc->hostCustomSequence()->rhythmTargetIdx != 1u) {
                        std::cerr << "[FAIL] RealFloppy: FAR OUT!.V08 Rhythm Song S2 decoding mismatch!\n";
                        return false;
                    }
                    ++verifiedRhySongsCount;
                }
                ++verifiedAllCount;
            }
        }
    }

    std::cout << "[PASS] Real Hardware Floppy .IMG Audit 100% PASSED: " << imgFiles.size() << " images, "
              << totalEntriesVerified << " entries verified (ALL=" << verifiedAllCount
              << ", SYN=" << verifiedSynCount << ", SEQ=" << verifiedSeqCount
              << ", R.SEQ=" << verifiedRhyCount << ", >64KB=" << verifiedOver64KbCount
              << ", RhythmSongs=" << verifiedRhySongsCount << ", 0 contamination)!\n";
    return true;
}

bool runSequencerRhythmClockAndSlotSyncTests(const juce::File& buildDir) {
    std::cout << "\n=== Running Sequencer-Rhythm Master Clock, Clean Boot, Slot Loading & LCD Playback Rules Tests ===\n";

    // -------------------------------------------------------------------------
    // TEST 1: RHY Mute/Unmute Zero-Desync Shared Master Clock Test
    //   Start song -> render 2 full measures -> mute RHY for 2.5 measures (assert 0 drum triggers)
    //   -> unmute mid-measure at Measure 5 Beat 3 -> assert rhythm measure/part/tick equals
    //   exact master sequencer tick (zero phase drift, zero reset to tick 0) and audio resumes!
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        vf50::gui::VF50MainCanvas canvas(*proc);

        // User Pattern I00: 1 bar 4/4 (192 ticks), 4 quarter-note BD1 hits (ticks 0, 48, 96, 144)
        vf50::state::UserRhythmPattern ptn0{};
        ptn0.clear();
        ptn0.bars = 1u;
        ptn0.timeSigNum = 4u;
        ptn0.timeSigDen = 4u;
        ptn0.totalTicks = 192u;
        (void)ptn0.addEventSorted(0u, 0u, 8u);
        (void)ptn0.addEventSorted(48u, 0u, 7u);
        (void)ptn0.addEventSorted(96u, 0u, 8u);
        (void)ptn0.addEventSorted(144u, 0u, 7u);

        // User Pattern I01: 1 bar 4/4 (192 ticks), 4 quarter-note SD1 hits (ticks 0, 48, 96, 144)
        vf50::state::UserRhythmPattern ptn1{};
        ptn1.clear();
        ptn1.bars = 1u;
        ptn1.timeSigNum = 4u;
        ptn1.timeSigDen = 4u;
        ptn1.totalTicks = 192u;
        (void)ptn1.addEventSorted(0u, 6u, 8u);
        (void)ptn1.addEventSorted(48u, 6u, 7u);
        (void)ptn1.addEventSorted(96u, 6u, 8u);
        (void)ptn1.addEventSorted(144u, 6u, 7u);

        proc->hostMemory().mutableUserRhythmPattern(0u) = ptn0;
        proc->hostMemory().mutableUserRhythmPattern(1u) = ptn1;
        proc->pushHostMemoryToAudio();

        // Rhythm Song #1 (slot 0): ||: I00 :|| x4 (Measures 1..4) -> ||: I01 :|| x4 (Measures 5..8)
        auto& rs1 = proc->mutableHostRhythmSongs()[0];
        rs1.clear();
        rs1.name = { 'S','Y','N','C','R','H','Y','1' };
        rs1.parts[0] = { vf50::generated::RhythmSongPartType::RepeatBegin, true, 0u, 2u, 0, 0, 0u, 'A' };
        rs1.parts[1] = { vf50::generated::RhythmSongPartType::Pattern,     true, 0u, 2u, 0, 0, 0u, 'A' };
        rs1.parts[2] = { vf50::generated::RhythmSongPartType::RepeatEnd,   true, 0u, 4u, 0, 0, 0u, 'A' };
        rs1.parts[3] = { vf50::generated::RhythmSongPartType::RepeatBegin, true, 0u, 2u, 0, 0, 0u, 'A' };
        rs1.parts[4] = { vf50::generated::RhythmSongPartType::Pattern,     true, 1u, 2u, 0, 0, 0u, 'A' };
        rs1.parts[5] = { vf50::generated::RhythmSongPartType::RepeatEnd,   true, 0u, 4u, 0, 0, 0u, 'A' };
        rs1.partCount = 6u;
        proc->publishRhythmSongsToAudio();

        // Sequencer Song #1 (slot 0): 8 bars @ 120 BPM (192 ticks/bar, 48000 Hz => 96,000 samples/bar), linked to Rhythm Song #1
        auto seqPtr = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        seqPtr->clear();
        seqPtr->valid = true;
        seqPtr->setTitle("SYNCSONG");
        seqPtr->initialTempoBpm = 120u;
        seqPtr->timeSigNum = 4u;
        seqPtr->timeSigDen = 4u;
        seqPtr->ticksPerBar = 192u;
        seqPtr->totalBars = 8u;
        seqPtr->totalTicks = 8u * 192u;
        seqPtr->playMode = 1u;
        seqPtr->perfPresetBank = 1u;
        seqPtr->perfNumber = 0u;
        seqPtr->hasRhythmLink = true;
        seqPtr->rhythmSyncIsSong = true;
        seqPtr->rhythmTargetIdx = 0u;
        uint32_t evi = 0u;
        for (uint32_t bar = 0u; bar < 8u; ++bar) {
            const uint32_t base = bar * 192u;
            seqPtr->events[evi++] = { base,        vf50::generated::DemoEventType::FmNoteOn,  0u, 60u, 100u };
            seqPtr->events[evi++] = { base + 96u,  vf50::generated::DemoEventType::FmNoteOff, 0u, 60u, 0u   };
            seqPtr->events[evi++] = { base + 96u,  vf50::generated::DemoEventType::FmNoteOn,  1u, 64u, 96u  };
            seqPtr->events[evi++] = { base + 180u, vf50::generated::DemoEventType::FmNoteOff, 1u, 64u, 0u   };
        }
        seqPtr->eventCount = evi;
        proc->publishUserSequenceToSlot(0u, *seqPtr);
        proc->selectDemoSong(0u);

        auto renderExactSamples = [&proc](int totalSamples) {
            juce::MidiBuffer midi;
            int rem = totalSamples;
            while (rem > 0) {
                const int chunk = std::min(rem, 512);
                juce::AudioBuffer<float> buf(2, chunk);
                buf.clear();
                midi.clear();
                proc->processBlock(buf, midi);
                rem -= chunk;
            }
        };

        canvas.pressSeqButton();
        canvas.pressSeqOpButton(3); // [PLAY]

        // Render 2 full measures = 192,000 samples @ 48 kHz, 120 BPM (384 ticks)
        renderExactSamples(192000);
        const auto& dp = proc->synthEngine().demoPlayer();
        const auto& dr = proc->synthEngine().drums();
        const uint64_t triggersAfter2Bars = dr.totalTriggerCount();
        if (!dp.isPlaying() || !dr.isSongPlaying() || !dr.isSlavedToSequencer()
            || triggersAfter2Bars < 8u
            || dp.currentMeasureOneBased() != 3
            || dr.currentSongMeasureOneBased() != 3
            || std::abs(dr.totalElapsedTicks() - dp.currentTickPosition()) > 0.5) {
            std::cerr << "[FAIL] RhyMuteSync: initial 2-bar lockstep failed (seqMeas="
                      << dp.currentMeasureOneBased() << ", rhyMeas=" << dr.currentSongMeasureOneBased()
                      << ", seqTick=" << dp.currentTickPosition() << ", rhyElapsed=" << dr.totalElapsedTicks()
                      << ", trigs=" << triggersAfter2Bars << ")\n";
            return false;
        }

        // Mute [RHY] track -> BlinkGreen LED, immediate voice silence, ZERO transport stop!
        canvas.pressEditMatrixButton(4); // [RHY] mute toggle
        canvas.refreshDisplayAndLeds();
        if (!proc->isSequencerTrackMuted(8u)
            || canvas.classicLedSnapshot().rhy != vf50::classic::LedState::BlinkGreen) {
            std::cerr << "[FAIL] RhyMuteSync: [RHY] mute did not set BlinkGreen LED\n";
            return false;
        }

        // Settle 1 sample-block to apply mute edge and capture baseline trigger count
        renderExactSamples(256);
        const uint64_t triggersAtMuteStart = dr.totalTriggerCount();
        if (dr.activeVoiceCount() != 0u || !dr.isSongPlaying() || !dr.isOutputTriggerMuted()) {
            std::cerr << "[FAIL] RhyMuteSync: [RHY] mute stopped song or failed to silence/gate drum voices!\n";
            return false;
        }

        // Render remaining 2.5 measures (240,000 - 256 samples) -> lands at Measure 5, Beat 3 (tick 864 = 4.5 bars)!
        renderExactSamples(240000 - 256);
        if (dr.totalTriggerCount() != triggersAtMuteStart || dr.activeVoiceCount() != 0u) {
            std::cerr << "[FAIL] RhyMuteSync: drum triggers leaked while RHY was muted (before="
                      << triggersAtMuteStart << ", after=" << dr.totalTriggerCount()
                      << ", activeVoices=" << dr.activeVoiceCount() << ")\n";
            return false;
        }

        // Unmute [RHY] mid-measure at Measure 5 Beat 3 -> SolidGreen LED, ZERO desync, ZERO reset to tick 0!
        canvas.pressEditMatrixButton(4); // [RHY] unmute
        renderExactSamples(64);
        canvas.refreshDisplayAndLeds();
        const int seqMeasAtUnmute  = dp.currentMeasureOneBased();
        const int rhyMeasAtUnmute  = static_cast<int>(dr.currentSongMeasureOneBased());
        const int rhyPartAtUnmute  = static_cast<int>(dr.currentSongPartOneBased());
        const uint16_t rhyPtnTick  = dr.currentPatternTickInt();
        const double tickDrift     = std::abs(dr.totalElapsedTicks() - dp.currentTickPosition());

        if (proc->isSequencerTrackMuted(8u)
            || canvas.classicLedSnapshot().rhy != vf50::classic::LedState::SolidGreen
            || seqMeasAtUnmute != 5
            || rhyMeasAtUnmute != 5
            || rhyPartAtUnmute != 5 // Part index 4 (1-based 5) = Pattern I01 after 4x repeat of I00!
            || dr.currentSongPtnIndex() != 1u
            || rhyPtnTick < 90u || rhyPtnTick > 106u // Beat 3 = tick 96 (NEVER reset to tick 0!)
            || tickDrift > 0.5) {
            std::cerr << "[FAIL] RhyMuteSync: mid-measure unmute desynced! (seqMeas=" << seqMeasAtUnmute
                      << ", rhyMeas=" << rhyMeasAtUnmute << ", rhyPart=" << rhyPartAtUnmute
                      << ", rhyPtnIdx=" << static_cast<int>(dr.currentSongPtnIndex())
                      << ", rhyPtnTick=" << rhyPtnTick << ", drift=" << tickDrift << ")\n";
            return false;
        }

        // Render 0.5 measure (48,000 samples) -> Beat 4 snare trigger MUST fire immediately!
        renderExactSamples(48000);
        if (dr.totalTriggerCount() <= triggersAtMuteStart) {
            std::cerr << "[FAIL] RhyMuteSync: drums did not resume triggering after RHY unmute!\n";
            return false;
        }

        // Verify LCD Playback Rule Test while playing at Measure 5+!
        const int liveSeqMeas = proc->activeDemoMeasure();
        canvas.pressRhythmButton(); // Should enter SONG PLAY) while Rhythm Song is playing
        canvas.refreshDisplayAndLeds();
        const auto rhyLine0 = canvas.lcdDisplay().getLineAsString(0);
        const auto rhyLine1 = canvas.lcdDisplay().getLineAsString(1);
        const juce::String expectedMeasStr = juce::String::formatted("%03d", liveSeqMeas);
        if (canvas.classicController().screen() != vf50::classic::ClassicScreen::RhythmSongPlay
            || !rhyLine0.startsWith("SONG PLAY)")
            || rhyLine1.contains(">toPTN")
            || !rhyLine1.contains("1w:SYNCRHY1")
            || !rhyLine1.contains(expectedMeasStr)) {
            std::cerr << "[FAIL] LcdPlaybackRule: SONG PLAY) failed live Meas / >toPTN hide during SEQ playback: r0='"
                      << rhyLine0 << "' r1='" << rhyLine1 << "' expectedMeas=" << expectedMeasStr << "\n";
            return false;
        }
        // Softkey F1 (0) MUST NOT switch to PTN PLAY> while playing!
        canvas.pressSoftKey(0);
        if (canvas.classicController().screen() != vf50::classic::ClassicScreen::RhythmSongPlay) {
            std::cerr << "[FAIL] LcdPlaybackRule: F1 (>toPTN) switched away from SONG PLAY) during active playback!\n";
            return false;
        }

        // Stop playback -> >toPTN must reappear; then switch to PTN PLAY> and verify >toSONG hides during playback!
        canvas.pressSeqOpButton(2); // [STOP]
        renderExactSamples(512);
        canvas.refreshDisplayAndLeds();
        if (!canvas.lcdDisplay().getLineAsString(1).contains(">toPTN")) {
            std::cerr << "[FAIL] LcdPlaybackRule: >toPTN did not reappear after STOP: '"
                      << canvas.lcdDisplay().getLineAsString(1) << "'\n";
            return false;
        }
        canvas.pressSoftKey(0); // >toPTN -> PTN PLAY>
        canvas.refreshDisplayAndLeds();
        if (canvas.classicController().screen() != vf50::classic::ClassicScreen::RhythmPtnPlay
            || !canvas.lcdDisplay().getLineAsString(1).contains(">toSONG")) {
            std::cerr << "[FAIL] LcdPlaybackRule: PTN PLAY> did not show >toSONG when stopped!\n";
            return false;
        }
        canvas.pressSeqOpButton(3); // [PLAY] in PTN PLAY>
        renderExactSamples(512);
        canvas.refreshDisplayAndLeds();
        if (canvas.lcdDisplay().getLineAsString(1).contains(">toSONG")) {
            std::cerr << "[FAIL] LcdPlaybackRule: PTN PLAY> failed to hide >toSONG during playback!\n";
            return false;
        }
        canvas.pressSoftKey(0); // Blocked during playback!
        if (canvas.classicController().screen() != vf50::classic::ClassicScreen::RhythmPtnPlay) {
            std::cerr << "[FAIL] LcdPlaybackRule: F1 (>toSONG) switched away from PTN PLAY> during playback!\n";
            return false;
        }
        canvas.pressSeqOpButton(2); // [STOP]
        renderExactSamples(512);
        std::cout << "[PASS] Test 1 & Test 3: RHY Mute Zero-Desync Shared Master Clock (M5 Beat 3 tick="
                  << rhyPtnTick << ", drift=" << tickDrift << ") & LCD >toPTN/>toSONG Playback Rules PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 2: Clean Boot (Zero Demo Songs in Slots 1..8), Isolated DEMO Workflow,
    //         SEQ (.A##) Selected-Slot Isolation & ALL (.V##) Song #1 Slot Loading
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        vf50::gui::VF50MainCanvas canvas(*proc);
        auto& classic = canvas.classicController();
        auto& lcd = canvas.lcdDisplay();

        // 2a. Clean boot: all 8 sequencer song slots & all 8 rhythm song slots MUST be empty, Song #1 selected!
        for (std::size_t s = 0u; s < 8u; ++s) {
            if (proc->hasUserSequenceInSlot(s) || proc->hostRhythmSongs()[s].hasData()) {
                std::cerr << "[FAIL] CleanBoot: slot " << (s + 1u) << " is NOT empty on clean plugin boot!\n";
                return false;
            }
        }
        if (classic.selectedSeqSongSlot() != 0 || classic.selectedRhythmSongSlot() != 0) {
            std::cerr << "[FAIL] CleanBoot: Song #1 not selected at boot!\n";
            return false;
        }
        canvas.pressSeqButton();
        canvas.refreshDisplayAndLeds();
        const auto bootSeqRow1 = lcd.getLineAsString(1);
        if (!bootSeqRow1.contains("1: NewSong")
            || bootSeqRow1.contains("Victory")
            || bootSeqRow1.contains("Route")
            || bootSeqRow1.contains("Moon")) {
            std::cerr << "[FAIL] CleanBoot: PLAY SONG) unexpectedly pre-populated with demo song: '"
                      << bootSeqRow1 << "'\n";
            return false;
        }

        // 2b. Verify 3 Demo Songs remain strictly isolated inside UT DEMO> -> >ROM Play -> Confirm -> DemoPlay!
        canvas.pressDemoButton();
        canvas.pressSoftKey(0); // >ROM Play
        canvas.refreshDisplayAndLeds();
        if (classic.screen() != vf50::classic::ClassicScreen::DemoConfirm
            || !lcd.getLineAsString(1).contains("All data will be changed")) {
            std::cerr << "[FAIL] CleanBoot: UT DEMO> confirm prompt mismatch!\n";
            return false;
        }
        canvas.pressKeypadYesButton();
        canvas.pressSoftKey(4); // >START
        {
            juce::AudioBuffer<float> buf(2, 512);
            juce::MidiBuffer midi;
            for (int i = 0; i < 16; ++i) {
                buf.clear(); midi.clear();
                proc->processBlock(buf, midi);
            }
        }
        if (!proc->isDemoPlaying()) {
            std::cerr << "[FAIL] CleanBoot: UT DEMO> >ROM Play failed to play ROM Demo Song!\n";
            return false;
        }
        canvas.pressSoftKey(7); // >EXIT DemoPlay
        {
            juce::AudioBuffer<float> buf(2, 512);
            juce::MidiBuffer midi;
            proc->processBlock(buf, midi);
        }
        for (std::size_t s = 0u; s < 8u; ++s) {
            if (proc->hasUserSequenceInSlot(s)) {
                std::cerr << "[FAIL] CleanBoot: UT DEMO> leaked ROM demo into user slot " << (s + 1u) << "!\n";
                return false;
            }
        }

        // 2c. DISK > Load > SEQ (.A01) loads strictly into currently selected Song #3 (slot 2)!
        const juce::File tmpDiskDir = buildDir.getChildFile("slot_sync_test_disk");
        (void)tmpDiskDir.deleteRecursively();
        (void)tmpDiskDir.createDirectory();

        // Build minimal valid HardwareSeqV1 file ("SLOT3SEQ")
        auto makeVF50SeqBytes = [](const char name8[8], bool linkSong, uint8_t linkIdx) -> std::vector<uint8_t> {
            std::vector<uint8_t> b(34u, 0u);
            static constexpr uint8_t kHwSeqMagic[8] = { 0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u };
            std::memcpy(b.data(), kHwSeqMagic, 8u);
            b[9] = 3u;   // 4/4
            b[10] = 0u; b[11] = 120u; // 120 BPM
            std::memcpy(b.data() + 12u, name8, 8u);
            for (uint8_t t = 0u; t < 8u; ++t) b[20u + t] = t;
            b[28] = 1u; b[29] = 2u; b[30] = 0u;
            b[31] = linkSong ? 1u : 0u;
            b[32] = linkIdx;
            b.push_back(0xF0u); b.push_back(0x00u); // Track 0
            b.push_back(0xF5u); // Bar 1
            b.push_back(0x80u | 24u); b.push_back(60u); b.push_back(100u);
            b.push_back(0xF2u); // End track
            b.push_back(0xF9u); // End song
            return b;
        };

        const auto a01Bytes = makeVF50SeqBytes("SLOT3SEQ", false, 0u);
        (void)tmpDiskDir.getChildFile("SLOT3.A01").replaceWithData(a01Bytes.data(), a01Bytes.size());

        // Build synthetic ALL (.V01) with VF50ALV1 + Rhythm Song #1 + appended HardwareSeqV1 ("ALLSONG1")
        {
            auto tmpProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
            tmpProc->mutableHostRhythmSongs()[0].clear();
            tmpProc->mutableHostRhythmSongs()[0].name = { 'A','L','L','R','H','Y','0','1' };
            tmpProc->mutableHostRhythmSongs()[0].parts[0] = { vf50::generated::RhythmSongPartType::Pattern, false, 0u, 2u, 0, 0, 0u, 'A' };
            tmpProc->mutableHostRhythmSongs()[0].partCount = 1u;
            std::vector<uint8_t> allBytes;
            vf50::disk::VF50DiskManager::buildAllDataBytes(tmpProc->hostMemory(), allBytes, &tmpProc->hostRhythmSongs());
            const auto allSeq = makeVF50SeqBytes("ALLSONG1", true, 0u);
            allBytes.insert(allBytes.end(), allSeq.begin(), allSeq.end());
            (void)tmpDiskDir.getChildFile("ALLTEST.V01").replaceWithData(allBytes.data(), allBytes.size());
        }

        juce::String mountSt;
        if (!canvas.diskManager().mountDirectory(tmpDiskDir, mountSt)) {
            std::cerr << "[FAIL] SlotSync: failed to mount temp disk dir: " << mountSt << "\n";
            return false;
        }

        // Plant Synth & Rhythm sentinels, select Song #3, load SLOT3.A01 via Classic DISK > Load > SEQ
        auto v0 = proc->hostMemory().getUserVoice(0u);
        v0.name = "SentVoi00 ";
        proc->hostMemory().storeUserVoice(0u, v0);
        proc->mutableHostRhythmSongs()[5].clear();
        proc->mutableHostRhythmSongs()[5].name = { 'S','E','N','T','R','H','Y','6' };
        proc->mutableHostRhythmSongs()[5].parts[0] = { vf50::generated::RhythmSongPartType::Pattern, false, 1u, 2u, 0, 0, 0u, 'A' };
        proc->mutableHostRhythmSongs()[5].partCount = 1u;
        proc->publishRhythmSongsToAudio();

        canvas.pressSeqButton();
        canvas.pressKeypadDigitButton(3); // Select Song #3 (slot 2)
        canvas.pressDiskButton();
        canvas.pressSoftKey(1); // F2 >Load
        canvas.pressSoftKey(2); // F3 >SEQ
        canvas.pressSoftKey(6); // F7 >GO
        canvas.pressKeypadYesButton(); // Confirm load SLOT3.A01 into Song #3

        if (classic.selectedSeqSongSlot() != 2
            || !proc->hasUserSequenceInSlot(2u)
            || proc->hasUserSequenceInSlot(0u)
            || proc->hasUserSequenceInSlot(3u)
            || proc->hostMemory().getUserVoice(0u).name != v0.name
            || !proc->hostRhythmSongs()[5].hasData()) {
            std::cerr << "[FAIL] SlotSync: DISK > Load > SEQ did not isolate to selected Song #3!\n";
            return false;
        }

        // Now load ALLTEST.V01 via Classic DISK > Load > ALL:
        // Must clear Song #3 and Rhythm Song #6, land in Song #1 (NOT #4!) with populated Rhythm Song #1!
        canvas.pressSeqButton();
        canvas.pressDiskButton();
        canvas.pressSoftKey(1); // F2 >Load
        canvas.pressSoftKey(0); // F1 >ALL
        canvas.pressSoftKey(6); // F7 >GO
        canvas.pressKeypadYesButton(); // Confirm load ALLTEST.V01

        if (classic.selectedSeqSongSlot() != 0
            || !proc->hasUserSequenceInSlot(0u)
            || proc->hasUserSequenceInSlot(2u) // Old Song #3 MUST be cleared!
            || proc->hasUserSequenceInSlot(3u) // Must NOT shift into Song #4!
            || proc->hostRhythmSongs()[5].hasData() // Old Rhythm Song #6 MUST be cleared!
            || !proc->hostRhythmSongs()[0].hasData()
            || classic.selectedRhythmSongSlot() != 0) {
            std::cerr << "[FAIL] SlotSync: DISK > Load > ALL did not clear stale slots & land in Song #1 + RhySong #1!\n";
            return false;
        }
        canvas.pressRhythmButton();
        if (classic.screen() != vf50::classic::ClassicScreen::RhythmSongPlay) {
            canvas.pressSoftKey(0); // >toSONG
        }
        canvas.refreshDisplayAndLeds();
        if (!lcd.getLineAsString(1).contains("1w:ALLRHY01")) {
            std::cerr << "[FAIL] SlotSync: RHYTHM SONG PLAY) did not display paired 1w:ALLRHY01: '"
                      << lcd.getLineAsString(1) << "'\n";
            return false;
        }
        (void)tmpDiskDir.deleteRecursively();

        // 2d. Guarded Real Floppy Verification (when VF50_TEST_DISK_DIR is set, strictly read-only)
        const juce::File floppyRoot = findTestDiskDirectory();
        const juce::File diskA3 = floppyRoot.isDirectory() ? floppyRoot.getChildFile("Disk-A3.IMG") : juce::File{};
        if (diskA3.existsAsFile()) {
            juce::String mSt;
            if (!canvas.diskManager().mountFat12ImageFile(diskA3, mSt)) {
                std::cerr << "[FAIL] SlotSync(RealFloppy): failed to mount Disk-A3.IMG: " << mSt << "\n";
                return false;
            }
            // Locate LONELY.V09 and PARADISE.V02 in filtered ALL list via Classic DISK > Load > ALL
            int lonelyAllIdx = -1;
            int paradiseAllIdx = -1;
            int allOrd = 0;
            for (const auto& e : canvas.diskManager().entries()) {
                if (e.kind == vf50::disk::VF50FileKind::AllData
                    && vf50::disk::VF50DiskManager::isHardwareExtensionForKind(e.name, vf50::disk::VF50FileKind::AllData)) {
                    if (e.name == "LONELY.V09")   lonelyAllIdx = allOrd;
                    if (e.name == "PARADISE.V02") paradiseAllIdx = allOrd;
                    ++allOrd;
                }
            }
            if (lonelyAllIdx >= 0) {
                canvas.pressSeqButton();
                canvas.pressDiskButton();
                canvas.pressSoftKey(1); // F2 >Load
                canvas.pressSoftKey(0); // F1 >ALL
                for (int step = 0; step < lonelyAllIdx; ++step) {
                    canvas.pressKeypadYesButton();
                }
                canvas.pressSoftKey(6); // F7 >GO
                canvas.pressKeypadYesButton(); // Confirm
                if (classic.selectedSeqSongSlot() != 0
                    || !proc->hasUserSequenceInSlot(0u)
                    || proc->hasUserSequenceInSlot(3u)
                    || classic.selectedRhythmSongSlot() != 0
                    || !proc->hostRhythmSongs()[0].hasData()) {
                    std::cerr << "[FAIL] SlotSync(RealFloppy): LONELY.V09 did not land in Song #1 + Rhythm Song #1!\n";
                    return false;
                }
            }
            if (paradiseAllIdx >= 0) {
                canvas.pressSeqButton();
                canvas.pressDiskButton();
                canvas.pressSoftKey(1); // F2 >Load
                canvas.pressSoftKey(0); // F1 >ALL
                for (int step = 0; step < paradiseAllIdx; ++step) {
                    canvas.pressKeypadYesButton();
                }
                canvas.pressSoftKey(6); // F7 >GO
                canvas.pressKeypadYesButton(); // Confirm
                if (classic.selectedSeqSongSlot() != 0
                    || !proc->hasUserSequenceInSlot(0u)
                    || !proc->hasUserSequenceInSlot(1u)
                    || !proc->hasUserSequenceInSlot(2u)
                    || !proc->hasUserSequenceInSlot(3u)
                    || proc->hasUserSequenceInSlot(4u)) {
                    std::cerr << "[FAIL] SlotSync(RealFloppy): PARADISE.V02 did not populate exact native Slots #1..#4!\n";
                    return false;
                }
            }
            std::cout << "[PASS] Guarded Real Floppy Verification (LONELY.V09 -> Song #1 + RhySong #1; PARADISE.V02 -> native Slots #1..#4) PASSED.\n";
        }
        std::cout << "[PASS] Test 2: Clean Boot (8 empty slots, isolated UT DEMO>), SEQ Slot Isolation & ALL Song #1 Loading PASSED.\n";
    }

    return true;
}

bool runRightClickLatchAndStopBwdTests() {
    std::cout << "\n=== Running Right-Click Modifier Latch & Ergonomic STOP+BWD Tests ===\n";

    auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
    proc->prepareToPlay(48000.0, 512);
    vf50::gui::VF50MainCanvas canvas(*proc);
    auto& classic = canvas.classicController();
    auto& lcd = canvas.lcdDisplay();

    // Populate User Rhythm Pattern I00 (1 bar = 192 ticks, 4 quarter kicks)
    vf50::state::UserRhythmPattern p0{};
    p0.clear();
    p0.bars = 1u;
    p0.timeSigNum = 4u;
    p0.timeSigDen = 4u;
    p0.totalTicks = 192u;
    p0.eventCount = 4u;
    p0.events[0] = { 0u,   0u, 5u };
    p0.events[1] = { 48u,  0u, 3u };
    p0.events[2] = { 96u,  0u, 4u };
    p0.events[3] = { 144u, 0u, 3u };
    proc->hostMemory().mutableUserRhythmPattern(0u) = p0;
    proc->pushHostMemoryToAudio();

    // Populate Rhythm Song #1: ||: I00 :|| x60 (60 measures)
    auto& rs1 = proc->mutableHostRhythmSongs()[0];
    rs1.clear();
    rs1.name = { 'L','O','N','G','S','O','N','G' };
    rs1.parts[0] = { vf50::generated::RhythmSongPartType::RepeatBegin, true, 0u, 2u, 0, 0, 0u, 'A' };
    rs1.parts[1] = { vf50::generated::RhythmSongPartType::Pattern,     true, 0u, 2u, 0, 0, 0u, 'A' };
    rs1.parts[2] = { vf50::generated::RhythmSongPartType::RepeatEnd,   true, 0u, 60u, 0, 0, 0u, 'A' };
    rs1.partCount = 3u;
    proc->publishRhythmSongsToAudio();

    // Populate 60-bar Sequencer Song #1 linked to Rhythm Song #1
    auto seqPtr = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
    seqPtr->clear();
    seqPtr->valid = true;
    seqPtr->setTitle("LONG60  ");
    seqPtr->initialTempoBpm = 120u;
    seqPtr->timeSigNum = 4u;
    seqPtr->timeSigDen = 4u;
    seqPtr->ticksPerBar = 192u;
    seqPtr->totalBars = 60u;
    seqPtr->totalTicks = 60u * 192u;
    seqPtr->playMode = 1u;
    seqPtr->perfPresetBank = 1u;
    seqPtr->perfNumber = 0u;
    seqPtr->hasRhythmLink = true;
    seqPtr->rhythmSyncIsSong = true;
    seqPtr->rhythmTargetIdx = 0u;
    uint32_t evi = 0u;
    for (uint32_t bar = 0u; bar < 60u; ++bar) {
        const uint32_t base = bar * 192u;
        seqPtr->events[evi++] = { base,        vf50::generated::DemoEventType::FmNoteOn,  0u, 60u, 100u };
        seqPtr->events[evi++] = { base + 96u,  vf50::generated::DemoEventType::FmNoteOff, 0u, 60u, 0u   };
    }
    seqPtr->eventCount = evi;
    proc->publishUserSequenceToSlot(0u, *seqPtr);
    proc->selectDemoSong(0u);

    auto pumpBlocks = [&proc](int blocks, juce::MidiBuffer* outMidi = nullptr) {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b) {
            buf.clear();
            midi.clear();
            proc->processBlock(buf, midi);
            if (outMidi != nullptr && !midi.isEmpty()) {
                for (const auto meta : midi) {
                    outMidi->addEvent(meta.getMessage(), meta.samplePosition);
                }
            }
        }
    };

    const auto& dp = proc->synthEngine().demoPlayer();
    const auto& dr = proc->synthEngine().drums();

    // -------------------------------------------------------------------------
    // TEST 1: Latched STOP (Right-Click) + BWD from Measure 042 -> M001 & P001
    //         (tick 0) across both Sequencer & Rhythm Song + auto-release latch
    // -------------------------------------------------------------------------
    canvas.pressSeqButton();
    canvas.pressSeqOpButton(3); // [PLAY]
    pumpBlocks(2);
    proc->requestDemoSeekToMeasure(42);
    pumpBlocks(4);
    canvas.refreshDisplayAndLeds();

    if (!proc->isDemoPlaying() || proc->activeDemoMeasure() != 42 || dr.currentSongMeasureOneBased() != 42u) {
        std::cerr << "[FAIL] LatchTest1: Failed to position Seq & RhySong at Measure 042 (seq="
                  << proc->activeDemoMeasure() << ", rhy=" << dr.currentSongMeasureOneBased() << ")\n";
        return false;
    }

    canvas.rightClickStopButton();
    if (!canvas.isStopLatched()) {
        std::cerr << "[FAIL] LatchTest1: Right-click STOP did not latch STOP button!\n";
        return false;
    }

    canvas.pressSeqOpButton(1); // [BWD] while STOP latched -> STOP + BWD
    pumpBlocks(4);
    canvas.refreshDisplayAndLeds();

    if (canvas.isStopLatched()
        || proc->isDemoPlaying()
        || proc->activeDemoMeasure() != 1
        || dp.currentTickPosition() >= 1.0
        || dr.currentSongMeasureOneBased() != 1u
        || dr.currentPatternTickInt() != 0u
        || !lcd.getLineAsString(1).contains("001")) {
        std::cerr << "[FAIL] LatchTest1: Latched STOP+BWD failed to snap M042->M001 & auto-release! "
                  << "latched=" << canvas.isStopLatched()
                  << " playing=" << proc->isDemoPlaying()
                  << " seqMeas=" << proc->activeDemoMeasure()
                  << " rhyMeas=" << dr.currentSongMeasureOneBased()
                  << " lcd='" << lcd.getLineAsString(1) << "'\n";
        return false;
    }

    // Also verify switching to RHYTHM SONG PLAY) shows Meas 001 / Part 001
    canvas.pressRhythmButton();
    canvas.pressSoftKey(0); // >toSONG (if starting on PTN PLAY>)
    canvas.refreshDisplayAndLeds();
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmSongPlay
        || !lcd.getLineAsString(1).contains("001")) {
        std::cerr << "[FAIL] LatchTest1: RHYTHM SONG PLAY) did not reflect Measure 001 after rewind: '"
                  << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    std::cout << "[PASS] Test 1: Latched STOP + BWD snapped M042 -> M001 / P001 (tick 0) & auto-released latch.\n";

    // -------------------------------------------------------------------------
    // TEST 2: 1st STOP stops in place at M042; 2nd STOP while stopped snaps M001;
    //         Shift+Left-Click on BWD stops & snaps M042 -> M001
    // -------------------------------------------------------------------------
    canvas.pressSeqButton();
    canvas.pressSeqOpButton(3); // [PLAY]
    pumpBlocks(2);
    proc->requestDemoSeekToMeasure(42);
    pumpBlocks(4);

    canvas.pressSeqOpButton(2); // 1st [STOP] -> stops at M042
    pumpBlocks(4);
    canvas.refreshDisplayAndLeds();
    if (proc->isDemoPlaying() || proc->activeDemoMeasure() < 42) {
        std::cerr << "[FAIL] LatchTest2: 1st STOP failed to hold position at M042 (playing="
                  << proc->isDemoPlaying() << ", meas=" << proc->activeDemoMeasure() << ")\n";
        return false;
    }

    canvas.pressSeqOpButton(2); // 2nd [STOP] while stopped -> snaps to M001
    pumpBlocks(4);
    canvas.refreshDisplayAndLeds();
    if (proc->activeDemoMeasure() != 1
        || dp.currentTickPosition() >= 1.0
        || dr.currentSongMeasureOneBased() != 1u
        || !lcd.getLineAsString(1).contains("001")) {
        std::cerr << "[FAIL] LatchTest2: 2nd STOP click while stopped failed to snap to M001 (meas="
                  << proc->activeDemoMeasure() << ", rhy=" << dr.currentSongMeasureOneBased() << ")\n";
        return false;
    }

    // Shift + Left-Click on BWD while playing at M042
    canvas.pressSeqOpButton(3); // [PLAY]
    pumpBlocks(2);
    proc->requestDemoSeekToMeasure(42);
    pumpBlocks(4);
    canvas.shiftLeftClickBwdButton();
    pumpBlocks(4);
    canvas.refreshDisplayAndLeds();
    if (proc->isDemoPlaying()
        || proc->activeDemoMeasure() != 1
        || dp.currentTickPosition() >= 1.0
        || dr.currentSongMeasureOneBased() != 1u) {
        std::cerr << "[FAIL] LatchTest2: Shift+Left-Click BWD failed to rewind M042 -> M001!\n";
        return false;
    }

    // Also verify 2nd STOP click in RhythmSongPlay snaps start measure back to 001
    canvas.pressRhythmButton();
    if (classic.screen() != vf50::classic::ClassicScreen::RhythmSongPlay) {
        canvas.pressSoftKey(0);
    }
    canvas.pressSoftKey(4); // Move cursor to >Meas (F5)
    for (int step = 0; step < 9; ++step) {
        canvas.pressKeypadYesButton();
    }
    canvas.refreshDisplayAndLeds();
    if (!lcd.getLineAsString(1).contains("010")) {
        std::cerr << "[FAIL] LatchTest2: RHYTHM SONG PLAY) could not increment >Meas to 010: '"
                  << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    canvas.pressSeqOpButton(2); // STOP while already stopped on RhythmSongPlay -> snaps M001
    pumpBlocks(2);
    canvas.refreshDisplayAndLeds();
    if (!lcd.getLineAsString(1).contains("001")) {
        std::cerr << "[FAIL] LatchTest2: 2nd STOP in RHYTHM SONG PLAY) failed to snap M010 -> M001: '"
                  << lcd.getLineAsString(1) << "'\n";
        return false;
    }
    std::cout << "[PASS] Test 2: 1st STOP pause / 2nd STOP snap M001 / Shift+Click BWD PASSED.\n";

    // -------------------------------------------------------------------------
    // TEST 3: Latched [-] + 7/8 Bank Hold, Latched [-] + TR2 KbdTch,
    //         Latched DEMO + STORE/COPY -> UT EDIT MESSAGE),
    //         Latched SINGLE + 012 + Release -> MIDI Program Change 12,
    //         and Latched RHYTHM + Data Entry slider -> Rhythm Tempo change
    // -------------------------------------------------------------------------
    canvas.pressSingleButton();
    canvas.rightClickInstMinusButton();
    if (!canvas.isInstMinusLatched()) {
        std::cerr << "[FAIL] LatchTest3: Right-click [-] did not latch!\n";
        return false;
    }
    canvas.pressKeypadDigitButton(7);
    if (!classic.isBankHoldEnabled() || canvas.isInstMinusLatched()) {
        std::cerr << "[FAIL] LatchTest3: Latched [-] + 7 failed Bank Hold ON or did not auto-release!\n";
        return false;
    }
    canvas.rightClickInstMinusButton();
    canvas.pressKeypadDigitButton(8);
    if (classic.isBankHoldEnabled() || canvas.isInstMinusLatched()) {
        std::cerr << "[FAIL] LatchTest3: Latched [-] + 8 failed Bank Hold OFF or did not auto-release!\n";
        return false;
    }

    // [-] + TR2 on Performance sets transmitCh to Instrument 2 receiveChannel
    canvas.pressPerformanceButton();
    proc->hostMemory().perfEditSlot().perf.instruments[1].receiveChannel = 4u; // Ch 5 (0-based 4)
    proc->commitHostPerfEditToAudio(true);
    canvas.rightClickInstMinusButton();
    canvas.pressEditMatrixButton(1); // TR2
    if (proc->classicSystemSettings().transmitCh != 4u || canvas.isInstMinusLatched()) {
        std::cerr << "[FAIL] LatchTest3: Latched [-] + TR2 failed KbdTch update or did not auto-release!\n";
        return false;
    }

    // DEMO (latched) + STORE/COPY -> UT EDIT MESSAGE) + auto-release
    canvas.rightClickDemoButton();
    if (!canvas.isDemoLatched()) {
        std::cerr << "[FAIL] LatchTest3: Right-click DEMO did not latch!\n";
        return false;
    }
    canvas.pressStoreButton();
    if (classic.screen() != vf50::classic::ClassicScreen::UtilEditMessage || canvas.isDemoLatched()) {
        std::cerr << "[FAIL] LatchTest3: Latched DEMO + STORE/COPY did not open UT EDIT MESSAGE) or failed auto-release!\n";
        return false;
    }

    // SINGLE (latched) + 0 1 2 + Release click -> emits MIDI Program Change 12 (data1 == 11)
    canvas.pressSingleButton();
    proc->mutableClassicSystemSettings().midiOn = true;
    proc->mutableClassicSystemSettings().pgmChangeMode = 1u;
    proc->mutableClassicSystemSettings().transmitCh = 0u;
    proc->publishClassicSystemStateToAudio();
    pumpBlocks(2); // Drain any setup sys update

    canvas.rightClickSingleButton();
    if (!canvas.isSingleLatched() || !classic.isDirectPcActive()) {
        std::cerr << "[FAIL] LatchTest3: Right-click SINGLE did not latch / enter Direct PC mode!\n";
        return false;
    }
    canvas.pressKeypadDigitButton(0);
    canvas.pressKeypadDigitButton(1);
    canvas.pressKeypadDigitButton(2);
    if (!canvas.isSingleLatched()) {
        std::cerr << "[FAIL] LatchTest3: SINGLE latch unexpectedly released during 3-digit entry!\n";
        return false;
    }
    canvas.rightClickSingleButton(); // Release click
    juce::MidiBuffer pcOutMidi;
    pumpBlocks(2, &pcOutMidi);

    bool foundPc12 = false;
    for (const auto meta : pcOutMidi) {
        const auto& m = meta.getMessage();
        if (m.isProgramChange() && m.getProgramChangeNumber() == 11) {
            foundPc12 = true;
            break;
        }
    }
    if (canvas.isSingleLatched() || !foundPc12) {
        std::cerr << "[FAIL] LatchTest3: Releasing latched SINGLE after 012 did not emit MIDI PC 12 (0-based 11)!\n";
        return false;
    }

    // RHYTHM (latched) + Data Entry slider -> adjusts Rhythm Tempo & stays latched until clicked again
    canvas.pressRhythmButton();
    const int tempoBefore = classic.rhythmTempoBpm();
    canvas.rightClickRhythmButton();
    canvas.setDataEntryNormalized(1.0f); // Max 240 BPM
    if (!canvas.isRhythmLatched() || classic.rhythmTempoBpm() != 240 || classic.rhythmTempoBpm() == tempoBefore) {
        std::cerr << "[FAIL] LatchTest3: Latched RHYTHM + Data Entry did not adjust Rhythm Tempo to 240!\n";
        return false;
    }
    canvas.rightClickRhythmButton();
    if (canvas.isRhythmLatched()) {
        std::cerr << "[FAIL] LatchTest3: Second click did not unlatch RHYTHM button!\n";
        return false;
    }

    std::cout << "[PASS] Test 3: Latched [-] + 7/8, [-] + TR2, DEMO + STORE, SINGLE + 012 PC out, and RHYTHM + Data Entry PASSED.\n";
    return true;
}

bool runUserDiskAndPcKeyboardTextEntryTests(const juce::File& buildDir) {
    std::cout << "\n=== Running Writable Common User Disk & DAW-Safe PC Keyboard Text-Entry Tests ===\n";

    auto typePcString = [](vf50::gui::VF50MainCanvas& canvas, std::string_view text) {
        for (char c : text) {
            (void)canvas.sendPcKeyPress(juce::KeyPress(static_cast<int>(static_cast<unsigned char>(c)),
                                                       juce::ModifierKeys(),
                                                       static_cast<juce::juce_wchar>(c)));
        }
    };
    auto sendSpecialKey = [](vf50::gui::VF50MainCanvas& canvas, int keyCode) {
        (void)canvas.sendPcKeyPress(juce::KeyPress(keyCode));
    };

    const juce::File tmpUserDisk = buildDir.getChildFile("test_user_disk_rt");
    const juce::File tmpImgFile  = buildDir.getChildFile("test_readonly_floppy.img");
    (void)tmpUserDisk.deleteRecursively();
    (void)tmpUserDisk.createDirectory();
    (void)tmpImgFile.deleteFile();

    // -------------------------------------------------------------------------
    // TEST 1: Read-Only .IMG -> Common User Disk Save & Full Reload Round-Trip
    // -------------------------------------------------------------------------
    {
        auto rawImg = vf50::disk::VF50DiskManager::createFormattedFat12Image(false, "READONLY");
        if (!tmpImgFile.replaceWithData(rawImg.data(), rawImg.size())) {
            std::cerr << "[FAIL] UserDiskTest1: Could not create temp .IMG fixture!\n";
            return false;
        }
        juce::MemoryBlock preImgBytes;
        (void)tmpImgFile.loadFileAsData(preImgBytes);

        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        vf50::gui::VF50MainCanvas canvas(*proc);
        auto& dm = canvas.diskManager();
        auto& lcd = canvas.lcdDisplay();

        if (!dm.setUserDiskDirectory(tmpUserDisk, false)) {
            std::cerr << "[FAIL] UserDiskTest1: setUserDiskDirectory failed!\n";
            return false;
        }

        // Populate distinctive state across FM Voices, Perfs, Sequencer Song #1, and Rhythm Pattern + Song #1
        {
            vf50::state::StoredVoiceSlot v0(proc->hostMemory().getUserVoice(0u));
            v0.setName("USRVOICET1");
            v0.voice.algorithm = 5u;
            v0.voice.operators[0].outputLevel = 77u;
            v0.repackRomBytes();
            proc->hostMemory().storeUserVoice(0u, v0.voice);

            vf50::state::StoredPerformanceSlot p0{};
            p0.setFromPerformance(proc->hostMemory().getUserPerformance(0u),
                                  proc->hostMemory().getUserPerformancePartPans(0u));
            p0.setName("USRPERFT1 ");
            p0.perf.instruments[0].voiceNumber = 12u;
            p0.repackRomBytes();
            proc->hostMemory().storeUserPerformance(0u, p0.perf, p0.partPans);

            vf50::state::UserRhythmPattern rp0{};
            rp0.clear();
            rp0.bars = 1u;
            rp0.timeSigNum = 4u;
            rp0.timeSigDen = 4u;
            rp0.totalTicks = 192u;
            rp0.eventCount = 4u;
            rp0.events[0] = { 0u,   0u, 7u };
            rp0.events[1] = { 48u,  6u, 5u };
            rp0.events[2] = { 96u,  0u, 6u };
            rp0.events[3] = { 144u, 6u, 4u };
            proc->hostMemory().mutableUserRhythmPattern(0u) = rp0;

            auto& rs0 = proc->mutableHostRhythmSongs()[0];
            rs0.clear();
            rs0.name = { 'U','S','R','R','H','Y','S','1' };
            rs0.parts[0] = { vf50::generated::RhythmSongPartType::Pattern, true, 0u, 2u, 0, 0, 0u, 'A' };
            rs0.partCount = 1u;
            proc->publishRhythmSongsToAudio();

            auto seq = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
            seq->clear();
            seq->valid = true;
            seq->setTitle("TESTSEQ1");
            seq->initialTempoBpm = 134u;
            seq->timeSigNum = 4u;
            seq->timeSigDen = 4u;
            seq->ticksPerBar = 192u;
            seq->totalBars = 2u;
            seq->totalTicks = 384u;
            seq->playMode = 1u;
            seq->perfPresetBank = 0u;
            seq->perfNumber = 0u;
            seq->events[0] = { 0u,   vf50::generated::DemoEventType::FmNoteOn,  0u, 60u, 100u };
            seq->events[1] = { 48u,  vf50::generated::DemoEventType::FmNoteOff, 0u, 60u, 0u   };
            seq->events[2] = { 96u,  vf50::generated::DemoEventType::FmNoteOn,  0u, 64u, 95u  };
            seq->events[3] = { 144u, vf50::generated::DemoEventType::FmNoteOff, 0u, 64u, 0u   };
            seq->events[4] = { 192u, vf50::generated::DemoEventType::FmNoteOn,  0u, 67u, 90u  };
            seq->events[5] = { 240u, vf50::generated::DemoEventType::FmNoteOff, 0u, 67u, 0u   };
            seq->events[6] = { 288u, vf50::generated::DemoEventType::FmNoteOn,  0u, 72u, 110u };
            seq->events[7] = { 336u, vf50::generated::DemoEventType::FmNoteOff, 0u, 72u, 0u   };
            seq->eventCount = 8u;
            proc->publishUserSequenceToSlot(0u, *seq);
            proc->selectDemoSong(0u);
            proc->pushHostMemoryToAudio();
        }

        // Mount read-only .IMG file
        juce::String mSt;
        if (!dm.mountFat12ImageFile(tmpImgFile, mSt) || !dm.isMountedSourceReadOnly()) {
            std::cerr << "[FAIL] UserDiskTest1: Failed to mount read-only .IMG: " << mSt << "\n";
            return false;
        }

        struct SaveCase {
            int typeSoftkey;
            const char* stem;
            const char* expectedFile;
        };
        const std::array<SaveCase, 4> kSaveCases = {{
            { 0, "MYALL", "MYALL.V01" }, // ALL
            { 1, "MYSYN", "MYSYN.I01" }, // SYN
            { 2, "MYSEQ", "MYSEQ.A01" }, // SEQ
            { 3, "MYRHY", "MYRHY.R01" }  // R.SEQ
        }};

        for (const auto& sc : kSaveCases) {
            canvas.pressSingleButton();
            canvas.pressDiskButton();
            canvas.pressSoftKey(0); // >Save
            canvas.pressSoftKey(sc.typeSoftkey);
            typePcString(canvas, sc.stem);
            sendSpecialKey(canvas, juce::KeyPress::returnKey); // >GO
            sendSpecialKey(canvas, juce::KeyPress::returnKey); // Confirm YES
            const juce::String row0 = lcd.getLineAsString(0);
            const juce::String row1 = lcd.getLineAsString(1);
            if (row1.contains("Disk Full") || (!row1.contains("Saved -> User Disk") && !row0.contains("Saved -> User Disk"))) {
                std::cerr << "[FAIL] UserDiskTest1: Save " << sc.expectedFile << " failed or missing notice! "
                          << "r0='" << row0 << "' r1='" << row1 << "'\n";
                return false;
            }
            const juce::File savedFile = tmpUserDisk.getChildFile(sc.expectedFile);
            if (!savedFile.existsAsFile() || savedFile.getSize() <= 0) {
                std::cerr << "[FAIL] UserDiskTest1: Saved file missing or empty: " << savedFile.getFullPathName() << "\n";
                return false;
            }
        }

        // Confirm read-only .IMG file on disk is byte-for-byte untouched!
        juce::MemoryBlock postImgBytes;
        (void)tmpImgFile.loadFileAsData(postImgBytes);
        if (postImgBytes != preImgBytes) {
            std::cerr << "[FAIL] UserDiskTest1: Mounted .IMG file on disk was unexpectedly modified!\n";
            return false;
        }

        // Switch active drive to Common User Disk via one-click top-bar button
        if (!canvas.mountUserDiskButton() || !dm.isUserDiskMounted() || dm.entries().size() != 4u) {
            std::cerr << "[FAIL] UserDiskTest1: mountUserDiskButton failed! entries=" << dm.entries().size() << "\n";
            return false;
        }

        // Check UT DISK) STATUS reports non-zero writable free space
        canvas.pressSingleButton();
        canvas.pressDiskButton();
        canvas.pressSoftKey(7); // >Job
        canvas.pressSoftKey(4); // >Status
        if (!lcd.getLineAsString(1).contains("Free=") || lcd.getLineAsString(1).contains("Free=  0K")) {
            std::cerr << "[FAIL] UserDiskTest1: User Disk STATUS unexpected Free KB: '" << lcd.getLineAsString(1) << "'\n";
            return false;
        }

        // Scramble RAM in a fresh processor and verify reload round-trip of MYALL.V01, MYSYN.I01, MYSEQ.A01, MYRHY.R01
        auto reloadAndVerifyAll = [&tmpUserDisk](const char* fileName, vf50::disk::VF50LoadFilter filt,
                                                 std::function<bool(vf50::plugin::VF50AudioProcessor&)> verifier) -> bool {
            auto cleanProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
            cleanProc->prepareToPlay(48000.0, 512);
            vf50::gui::VF50MainCanvas c(*cleanProc);
            if (!c.diskManager().setUserDiskDirectory(tmpUserDisk, false) || !c.mountUserDiskButton()) {
                return false;
            }
            const juce::File f = tmpUserDisk.getChildFile(fileName);
            juce::String st;
            if (!c.diskManager().importExternalFile(f, *cleanProc, filt, st)) {
                std::cerr << "[FAIL] UserDiskTest1: Failed to reload " << fileName << ": " << st << "\n";
                return false;
            }
            return verifier(*cleanProc);
        };

        if (!reloadAndVerifyAll("MYALL.V01", vf50::disk::VF50LoadFilter::AllData, [](vf50::plugin::VF50AudioProcessor& p) {
            const auto& v0 = p.hostMemory().getUserVoice(0u);
            const auto& p0 = p.hostMemory().getUserPerformance(0u);
            const auto* seq0 = p.userSequenceInSlot(0u);
            const auto& rs0 = p.hostRhythmSongs()[0];
            return std::string_view(v0.name.data(), 10) == "USRVOICET1"
                && v0.algorithm == 5u
                && v0.operators[0].outputLevel == 77u
                && std::string_view(p0.name.data(), 10) == "USRPERFT1 "
                && p0.instruments[0].voiceNumber == 12u
                && seq0 != nullptr && seq0->valid && seq0->initialTempoBpm == 134u && seq0->eventCount >= 8u
                && rs0.hasData() && std::string_view(rs0.name.data(), 8) == "USRRHYS1";
        })) {
            std::cerr << "[FAIL] UserDiskTest1: MYALL.V01 reload round-trip verification failed!\n";
            return false;
        }

        if (!reloadAndVerifyAll("MYSYN.I01", vf50::disk::VF50LoadFilter::SynthOnly, [](vf50::plugin::VF50AudioProcessor& p) {
            const auto& v0 = p.hostMemory().getUserVoice(0u);
            const auto& p0 = p.hostMemory().getUserPerformance(0u);
            return std::string_view(v0.name.data(), 10) == "USRVOICET1"
                && v0.algorithm == 5u
                && std::string_view(p0.name.data(), 10) == "USRPERFT1 "
                && !p.hasUserSequenceInSlot(0u); // SYN must NOT load sequencer data
        })) {
            std::cerr << "[FAIL] UserDiskTest1: MYSYN.I01 reload round-trip verification failed!\n";
            return false;
        }

        if (!reloadAndVerifyAll("MYSEQ.A01", vf50::disk::VF50LoadFilter::SequenceOnly, [](vf50::plugin::VF50AudioProcessor& p) {
            const auto* seq0 = p.userSequenceInSlot(0u);
            return seq0 != nullptr && seq0->valid
                && seq0->title() == "TESTSEQ1"
                && seq0->initialTempoBpm == 134u
                && seq0->eventCount == 8u;
        })) {
            std::cerr << "[FAIL] UserDiskTest1: MYSEQ.A01 reload round-trip verification failed!\n";
            return false;
        }

        if (!reloadAndVerifyAll("MYRHY.R01", vf50::disk::VF50LoadFilter::RhythmOnly, [](vf50::plugin::VF50AudioProcessor& p) {
            const auto& rp0 = p.hostMemory().userRhythmPattern(0u);
            const auto& rs0 = p.hostRhythmSongs()[0];
            return rp0.eventCount == 4u && rs0.hasData() && std::string_view(rs0.name.data(), 8) == "USRRHYS1";
        })) {
            std::cerr << "[FAIL] UserDiskTest1: MYRHY.R01 reload round-trip verification failed!\n";
            return false;
        }

        // Rename & Delete inside User Disk (locate MYSEQ.A01 then REN_A01.A01 across sorted entries)
        auto findEntryIdx = [&dm](const std::string_view targetName) -> std::optional<std::size_t> {
            const auto& ents = dm.entries();
            for (std::size_t i = 0u; i < ents.size(); ++i) {
                if (ents[i].name == targetName) return i;
            }
            return std::nullopt;
        };
        const auto seqIdx = findEntryIdx("MYSEQ.A01");
        juce::String opsSt;
        if (!seqIdx.has_value()
            || !dm.renameMountedEntry(*seqIdx, "REN_A01", opsSt)
            || !tmpUserDisk.getChildFile("REN_A01.A01").existsAsFile()) {
            std::cerr << "[FAIL] UserDiskTest1: Rename in User Disk failed: " << opsSt << "\n";
            return false;
        }
        const auto renIdx = findEntryIdx("REN_A01.A01");
        if (!renIdx.has_value()
            || !dm.deleteMountedEntry(*renIdx, opsSt)
            || tmpUserDisk.getChildFile("REN_A01.A01").existsAsFile()) {
            std::cerr << "[FAIL] UserDiskTest1: Delete in User Disk failed: " << opsSt << "\n";
            return false;
        }
        std::cout << "[PASS] Test 1: Read-Only .IMG -> Common User Disk Save (.V01/.I01/.A01/.R01) & Reload Round-Trip PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 2: PC Keyboard Text-Entry, Sanitization, Backspace/Left/Enter/Escape
    //         & DAW-Safe Focus Proxy Lifecycle
    // -------------------------------------------------------------------------
    {
        const juce::File tmpKeyDisk = buildDir.getChildFile("test_user_disk_keys");
        (void)tmpKeyDisk.deleteRecursively();
        (void)tmpKeyDisk.createDirectory();

        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        vf50::gui::VF50MainCanvas canvas(*proc);
        auto& classic = canvas.classicController();
        auto& lcd = canvas.lcdDisplay();

        (void)canvas.diskManager().setUserDiskDirectory(tmpKeyDisk, false);
        (void)canvas.mountUserDiskButton();

        if (classic.isTextEntryScreenActive() || canvas.isTextEntryProxyActive()) {
            std::cerr << "[FAIL] UserDiskTest2: TextEntry proxy active outside text-entry screen!\n";
            return false;
        }

        // Enter DISK > Save > ALL
        canvas.pressDiskButton();
        canvas.pressSoftKey(0); // >Save
        canvas.pressSoftKey(0); // ALL
        if (!classic.isTextEntryScreenActive() || !canvas.isTextEntryProxyActive()) {
            std::cerr << "[FAIL] UserDiskTest2: TextEntry proxy did not activate on DISK > Save!\n";
            return false;
        }

        // Feed illegal Windows / FAT12 filename chars -> all rejected cleanly!
        for (char bad : std::string_view("/\\:*?\"<>|")) {
            (void)canvas.sendPcKeyPress(juce::KeyPress(static_cast<int>(bad), juce::ModifierKeys(), static_cast<juce::juce_wchar>(bad)));
        }
        if (!lcd.getLineAsString(1).contains("NewFile")) {
            std::cerr << "[FAIL] UserDiskTest2: Illegal filename characters were not rejected! line1='"
                      << lcd.getLineAsString(1) << "'\n";
            return false;
        }

        // Type exact sequence: "my_song!", Backspace, Left, '2', Enter, Enter
        typePcString(canvas, "my_song!");
        sendSpecialKey(canvas, juce::KeyPress::backspaceKey);
        sendSpecialKey(canvas, juce::KeyPress::leftKey);
        typePcString(canvas, "2");
        sendSpecialKey(canvas, juce::KeyPress::returnKey); // >GO
        sendSpecialKey(canvas, juce::KeyPress::returnKey); // Confirm YES

        const juce::File expectedSaved = tmpKeyDisk.getChildFile("MY_SON2.V01");
        if (!expectedSaved.existsAsFile() || expectedSaved.getSize() <= 0) {
            std::cerr << "[FAIL] UserDiskTest2: Expected MY_SON2.V01 on disk after PC typing sequence, lcd='"
                      << lcd.getLineAsString(1) << "'\n";
            return false;
        }

        // Exit Disk mode -> focus proxy MUST deactivate cleanly!
        canvas.pressSingleButton();
        if (classic.isTextEntryScreenActive() || canvas.isTextEntryProxyActive()) {
            std::cerr << "[FAIL] UserDiskTest2: TextEntry proxy stayed active after exiting Disk Save!\n";
            return false;
        }

        // Enter 10-char Voice Name screen (Single -> Utility/Others? No: Edit F8 Function -> F8 >Name)
        canvas.pressEditMatrixButton(10); // Function root
        canvas.pressSoftKey(7);           // F8 >Name
        canvas.refreshDisplayAndLeds();
        if (classic.screen() != vf50::classic::ClassicScreen::VoiceEditFuncName
            || !classic.isTextEntryScreenActive()
            || !canvas.isTextEntryProxyActive()) {
            std::cerr << "[FAIL] UserDiskTest2: VoiceEditFuncName failed to activate text-entry proxy!\n";
            return false;
        }
        typePcString(canvas, "DX_LEAD_99");
        if (!lcd.getLineAsString(1).contains("DX_LEAD_99")) {
            std::cerr << "[FAIL] UserDiskTest2: Voice Name LCD did not display 'DX_LEAD_99': '"
                      << lcd.getLineAsString(1) << "'\n";
            return false;
        }
        sendSpecialKey(canvas, juce::KeyPress::returnKey); // Confirm/Exit keeps "DX_LEAD_99"
        if (classic.isTextEntryScreenActive()
            || canvas.isTextEntryProxyActive()
            || std::string_view(proc->hostMemory().singleEditSlot().voice.name.data(), 10) != "DX_LEAD_99") {
            std::cerr << "[FAIL] UserDiskTest2: Return on Voice Name did not exit cleanly or retain DX_LEAD_99!\n";
            return false;
        }

        // Re-enter Voice Name, type "BAD", press Escape -> cancels & restores "DX_LEAD_99", releasing proxy!
        canvas.pressSoftKey(7); // F8 >Name
        typePcString(canvas, "BAD");
        sendSpecialKey(canvas, juce::KeyPress::escapeKey);
        if (classic.isTextEntryScreenActive()
            || canvas.isTextEntryProxyActive()
            || std::string_view(proc->hostMemory().singleEditSlot().voice.name.data(), 10) != "DX_LEAD_99") {
            std::cerr << "[FAIL] UserDiskTest2: Escape on Voice Name did not restore DX_LEAD_99 or release proxy!\n";
            return false;
        }

        // Verify Power-On Greeting (DEMO + STORE -> UT EDIT MESSAGE) accepts typing & releases on Escape
        canvas.rightClickDemoButton();
        canvas.pressStoreButton();
        if (classic.screen() != vf50::classic::ClassicScreen::UtilEditMessage
            || !canvas.isTextEntryProxyActive()) {
            std::cerr << "[FAIL] UserDiskTest2: UT EDIT MESSAGE failed to activate text-entry proxy!\n";
            return false;
        }
        typePcString(canvas, "HELLO STUDIO");
        if (!lcd.getLineAsString(1).startsWith("HELLO STUDIO")) {
            std::cerr << "[FAIL] UserDiskTest2: Greeting typing mismatch: '" << lcd.getLineAsString(1) << "'\n";
            return false;
        }
        sendSpecialKey(canvas, juce::KeyPress::escapeKey);
        if (classic.isTextEntryScreenActive() || canvas.isTextEntryProxyActive()) {
            std::cerr << "[FAIL] UserDiskTest2: Escape on UT EDIT MESSAGE failed to release text-entry proxy!\n";
            return false;
        }

        (void)tmpKeyDisk.deleteRecursively();
        std::cout << "[PASS] Test 2: PC Keyboard Naming ('my_song!' + BS + Left + '2' -> MY_SON2.V01), Illegal Char Reject, Voice/Greeting Name & Focus Release PASSED.\n";
    }

    (void)tmpUserDisk.deleteRecursively();
    (void)tmpImgFile.deleteFile();
    return true;
}

bool runVisualMixerDrawerSelfTests() {
    std::cout << "\n=== Running Drop-Down Visual Mixer Drawer & Metering/Solo Tests ===\n";

    auto renderBlock = [](vf50::plugin::VF50AudioProcessor& p, int totalSamples, juce::MidiBuffer* firstBlockMidi = nullptr) {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer emptyMidi;
        int done = 0;
        while (done < totalSamples) {
            const int chunk = std::min(512, totalSamples - done);
            buf.setSize(2, chunk, false, false, true);
            buf.clear();
            auto& midi = (done == 0 && firstBlockMidi != nullptr) ? *firstBlockMidi : emptyMidi;
            p.processBlock(buf, midi);
            done += chunk;
        }
    };

    // -------------------------------------------------------------------------
    // TEST 0: Mutually Exclusive Drawer Switching, 75%..200% Scaling & State Persistence
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);

        std::unique_ptr<juce::AudioProcessorEditor> baseEd(proc->createEditor());
        auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
        if (editor == nullptr) {
            std::cerr << "[FAIL] MixerTest0: Failed to create VF50AudioProcessorEditor\n";
            return false;
        }
        auto& canvas = editor->mainCanvas();
        canvas.resetFaceplatePopupSpawnCount();

        // Open PROGRAMMER, then click MIXER -> cleanly swaps (only 1 drawer open at a time)
        canvas.pressProgrammerToggleButton();
        if (!canvas.isProgrammerVisible() || canvas.isMixerVisible()) {
            std::cerr << "[FAIL] MixerTest0: Programmer failed to open exclusively\n";
            return false;
        }
        canvas.pressMixerToggleButton();
        if (!canvas.isMixerVisible() || canvas.isProgrammerVisible() || !proc->isMixerVisible() || proc->isProgrammerVisible()) {
            std::cerr << "[FAIL] MixerTest0: MIXER toggle did not close PROGRAMMER mutually exclusively\n";
            return false;
        }

        // Verify all 5 discrete scale factors (75%, 100%, 125%, 150%, 200%) & seamless drawer anchoring
        for (float sc : vf50::gui::tokens::kDiscreteUiScales) {
            editor->applyScaleFactor(sc);
            const float xf = vf50::gui::tokens::canvasTransformForUserScale(sc);
            const int expW = static_cast<int>(std::lround(static_cast<float>(vf50::gui::tokens::kVirtualWidth) * xf));
            const int expH = static_cast<int>(std::lround(static_cast<float>(vf50::gui::tokens::computeVirtualHeight(false, false, true)) * xf));
            if (editor->getWidth() != expW || editor->getHeight() != expH) {
                std::cerr << "[FAIL] MixerTest0: Scale " << sc << " mismatch: got "
                          << editor->getWidth() << "x" << editor->getHeight()
                          << ", expected " << expW << "x" << expH << "\n";
                return false;
            }
            if (canvas.mixerDrawer().getY() != vf50::gui::tokens::kVirtualHeight
                || canvas.programmerDrawer().getY() != vf50::gui::tokens::kVirtualHeight) {
                std::cerr << "[FAIL] MixerTest0: Drawer top Y not anchored flush at "
                          << vf50::gui::tokens::kVirtualHeight << " at scale " << sc << "\n";
                return false;
            }
        }
        editor->applyScaleFactor(1.0f);

        // Save state with MIXER open, restore into fresh processor & verify persisted state
        juce::MemoryBlock stateBlock;
        proc->getStateInformation(stateBlock);

        auto proc2 = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc2->prepareToPlay(48000.0, 512);
        proc2->setStateInformation(stateBlock.getData(), static_cast<int>(stateBlock.getSize()));
        if (!proc2->isMixerVisible() || proc2->isProgrammerVisible()) {
            std::cerr << "[FAIL] MixerTest0: Mixer drawer open state did not survive get/setStateInformation!\n";
            return false;
        }

        // Clicking PROGRAMMER while MIXER is open cleanly swaps back
        canvas.pressProgrammerToggleButton();
        if (!canvas.isProgrammerVisible() || canvas.isMixerVisible()) {
            std::cerr << "[FAIL] MixerTest0: PROGRAMMER toggle did not close MIXER mutually exclusively\n";
            return false;
        }
        if (canvas.faceplatePopupSpawnCount() != 0u) {
            std::cerr << "[FAIL] MixerTest0: Unexpected OS popup spawned!\n";
            return false;
        }
        std::cout << "[PASS] Test 0: Mutually Exclusive Drawer Swapping, 75%..200% Scaling & State Persistence PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 1: INST 1 Volume Fader (0..99), Output Assign (L/R/L+R/OFF), PMEM Dirty & COMPARE
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        proc->selectPerformancePresetAtomic(vf50::state::MemoryBank::FactoryRom, 0u, true);
        renderBlock(*proc, 512);

        vf50::gui::VF50MainCanvas canvas(*proc);
        canvas.pressPerformanceButton();
        canvas.pressMixerToggleButton();
        auto& mixer = canvas.mixerDrawer();

        const uint8_t origVol = proc->hostMemory().activePerformance().instruments[0].volume;
        const uint8_t origOut = proc->hostMemory().activePerformance().instruments[0].outAssign;

        // Adjust INST 1 fader to 42 and cycle Output Assign through L, R, OFF, L+R
        if (!mixer.setInstVolumeFromMixer(0u, 42)) {
            std::cerr << "[FAIL] MixerTest1: setInstVolumeFromMixer(0, 42) returned false\n";
            return false;
        }
        if (!mixer.setInstOutAssignFromMixer(0u, 1u) // L
            || !mixer.setInstOutAssignFromMixer(0u, 2u) // R
            || !mixer.setInstOutAssignFromMixer(0u, 0u) // OFF
            || !mixer.setInstOutAssignFromMixer(0u, 3u) // L+R
            || !mixer.setInstOutAssignFromMixer(0u, 1u)) { // leave at L (1)
            std::cerr << "[FAIL] MixerTest1: setInstOutAssignFromMixer failed\n";
            return false;
        }
        renderBlock(*proc, 1024);
        proc->pullLatestAudioMemoryToHost();

        const auto& editedPerf = proc->hostMemory().activePerformance();
        if (editedPerf.instruments[0].volume != 42u
            || editedPerf.instruments[0].outAssign != 1u
            || !proc->hostMemory().isPerfEditDirty()
            || proc->synthEngine().fm().activePerformance().instruments[0].volume != 42u
            || proc->synthEngine().fm().activePerformance().instruments[0].outAssign != 1u) {
            std::cerr << "[FAIL] MixerTest1: PMEM or live FM engine did not reflect Mixer edits (vol="
                      << static_cast<int>(editedPerf.instruments[0].volume)
                      << ", out=" << static_cast<int>(editedPerf.instruments[0].outAssign)
                      << ", dirty=" << proc->hostMemory().isPerfEditDirty() << ")\n";
            return false;
        }

        // Toggle COMPARE ON -> reverts active PMEM & Mixer UI to original stored values, preserving edit buffer
        canvas.pressCompareButton();
        renderBlock(*proc, 512);
        mixer.syncFromProcessor();
        if (!proc->hostMemory().isPerformanceCompareActive()
            || proc->hostMemory().activePerformance().instruments[0].volume != origVol
            || proc->hostMemory().activePerformance().instruments[0].outAssign != origOut
            || mixer.displayedInstVolume(0u) != static_cast<int>(origVol)
            || mixer.displayedInstOutAssign(0u) != origOut) {
            std::cerr << "[FAIL] MixerTest1: COMPARE ON did not revert active PMEM & Mixer UI to stored values\n";
            return false;
        }

        // Editing INST 1 while COMPARE is active must be locked (non-destructive)
        if (mixer.setInstVolumeFromMixer(0u, 15)) {
            std::cerr << "[FAIL] MixerTest1: Mixer edit succeeded while COMPARE lock was active!\n";
            return false;
        }

        // Toggle COMPARE OFF -> restores edited values (42, L)
        canvas.pressCompareButton();
        renderBlock(*proc, 512);
        mixer.syncFromProcessor();
        if (proc->hostMemory().isPerformanceCompareActive()
            || proc->hostMemory().activePerformance().instruments[0].volume != 42u
            || proc->hostMemory().activePerformance().instruments[0].outAssign != 1u
            || mixer.displayedInstVolume(0u) != 42
            || mixer.displayedInstOutAssign(0u) != 1u) {
            std::cerr << "[FAIL] MixerTest1: COMPARE OFF did not restore edited values\n";
            return false;
        }

        std::cout << "[PASS] Test 1: INST 1 Fader & Output Assign -> Live PMEM, Dirty Flag & COMPARE PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 2: RHY Fader (R.Vol) Live Update, Efct & Clock-Locked RHY Mute Zero Drift
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        vf50::gui::VF50MainCanvas canvas(*proc);
        canvas.pressRhythmButton();
        canvas.pressMixerToggleButton();
        auto& mixer = canvas.mixerDrawer();

        // Update R.Vol to 61 and cycle Efct mode -> verify live drum engine updates
        mixer.setRhythmVolumeFromMixer(61);
        mixer.cycleRhythmEffectModeFromMixer(); // Mix -> Off
        renderBlock(*proc, 512);
        proc->pullLatestAudioMemoryToHost();
        if (proc->hostMemory().activeRhythmKit().rhythmVolume != 61u
            || proc->synthEngine().drums().rhythmMasterVolume() != 61u
            || proc->hostMemory().activeRhythmKit().effectMode != vf50::dsp::drums::RhythmEffectMode::Off
            || proc->synthEngine().drums().effectMode() != vf50::dsp::drums::RhythmEffectMode::Off) {
            std::cerr << "[FAIL] MixerTest2: R.Vol or Efct did not propagate live to drum engine!\n";
            return false;
        }

        // Start factory rhythm pattern at 120 BPM (96000 samples = 1 bar = 192 ticks @ 48 kHz)
        canvas.pressSeqOpButton(3); // PLAY
        renderBlock(*proc, 48000); // 0.5 bar = 96 ticks

        const auto& dr = proc->synthEngine().drums();
        const double ticksBeforeMute = dr.totalElapsedTicks();
        if (!dr.isPatternPlaying() || dr.totalTriggerCount() == 0u) {
            std::cerr << "[FAIL] MixerTest2: Rhythm pattern failed to start or trigger\n";
            return false;
        }

        // Mute RHY from Mixer drawer -> faceplate RHY LED becomes BlinkGreen, clock stays locked!
        mixer.toggleRhythmMuteFromMixer();
        canvas.refreshDisplayAndLeds();
        renderBlock(*proc, 512);
        const uint64_t triggersAtMute = dr.totalTriggerCount();
        if (!proc->isSequencerTrackMuted(8u)
            || !mixer.isRhythmMuteButtonLit()
            || canvas.classicLedSnapshot().rhy != vf50::classic::LedState::BlinkGreen
            || !dr.isOutputTriggerMuted()
            || dr.activeVoiceCount() != 0u) {
            std::cerr << "[FAIL] MixerTest2: Mixer RHY Mute did not sync with faceplate RHY LED or silence drums\n";
            return false;
        }

        // Advance exactly 1.5 bars (144000 samples @ 120 BPM = 288 ticks) while muted
        renderBlock(*proc, 144000 - 512);
        const double ticksAfterMuteWindow = dr.totalElapsedTicks();
        const double expectedTotalDelta = (144000.0 / 48000.0) * (120.0 * 48.0 / 60.0); // 288.0 ticks
        if (dr.totalTriggerCount() != triggersAtMute
            || dr.activeVoiceCount() != 0u
            || !dr.isPatternPlaying()
            || std::abs((ticksAfterMuteWindow - ticksBeforeMute) - expectedTotalDelta) > 1.5) {
            std::cerr << "[FAIL] MixerTest2: Clock drifted or triggers leaked while RHY muted (delta="
                      << (ticksAfterMuteWindow - ticksBeforeMute) << ", expected=" << expectedTotalDelta << ")\n";
            return false;
        }

        // Unmute RHY -> continuous phase, triggers resume immediately
        mixer.toggleRhythmMuteFromMixer();
        renderBlock(*proc, 48000);
        if (proc->isSequencerTrackMuted(8u) || dr.isOutputTriggerMuted() || dr.totalTriggerCount() <= triggersAtMute) {
            std::cerr << "[FAIL] MixerTest2: Drums did not resume cleanly after unmuting RHY\n";
            return false;
        }

        std::cout << "[PASS] Test 2: RHY Fader (R.Vol), Efct & Clock-Locked RHY Mute Zero Drift PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 3: Non-Destructive Solo on INST 2 (Preserves Saved PMEM/R.Vol & Clock)
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        proc->selectPerformancePresetAtomic(vf50::state::MemoryBank::FactoryRom, 0u, true);
        renderBlock(*proc, 512);

        vf50::gui::VF50MainCanvas canvas(*proc);
        canvas.pressPerformanceButton();
        canvas.pressMixerToggleButton();
        auto& mixer = canvas.mixerDrawer();

        // Configure INST 1 (Ch 1) and INST 2 (Ch 2) active at full range, vol 90 & 80
        auto pf = proc->hostMemory().activePerformance();
        pf.assignMode = 0u;
        pf.instruments[0].receiveChannel = 0u;
        pf.instruments[0].maxNotes = 8u;
        pf.instruments[0].limitLow = 0u;
        pf.instruments[0].limitHigh = 127u;
        pf.instruments[0].outAssign = 3u;
        pf.instruments[0].volume = 90u;

        pf.instruments[1].receiveChannel = 1u;
        pf.instruments[1].maxNotes = 8u;
        pf.instruments[1].limitLow = 0u;
        pf.instruments[1].limitHigh = 127u;
        pf.instruments[1].outAssign = 3u;
        pf.instruments[1].volume = 80u;

        for (std::size_t p = 2u; p < 8u; ++p) {
            pf.instruments[p].maxNotes = 0u;
            pf.instruments[p].outAssign = 0u;
        }
        proc->hostMemory().setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
        proc->hostMemory().perfEditSlot().setFromPerformance(pf);
        proc->commitHostPerfEditToAudio(true);
        renderBlock(*proc, 512);

        // Start Rhythm pattern so RHY is active as well
        proc->requestClassicRhythmStartFactory(0u, 120.0, true);
        renderBlock(*proc, 512);

        // Trigger simultaneous NoteOn on Ch 1 (INST 1) and Ch 2 (INST 2)
        juce::MidiBuffer notesOn;
        notesOn.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
        notesOn.addEvent(juce::MidiMessage::noteOn(2, 64, static_cast<juce::uint8>(110)), 0);
        renderBlock(*proc, 2048, &notesOn);

        // Solo INST 2
        mixer.toggleInstSoloFromMixer(1u);
        if (proc->mixerSoloMask() != 0x0002u || !mixer.isSoloButtonLit(1u) || !mixer.isClearSoloButtonLit()) {
            std::cerr << "[FAIL] MixerTest3: Solo INST 2 mask/LED state incorrect!\n";
            return false;
        }

        // Wait a short window for meter decay of non-soloed channels while INST 2 continues sounding
        const double ticksPreSolo = proc->synthEngine().drums().totalElapsedTicks();
        renderBlock(*proc, 24000); // 0.5 s @ 48 kHz (> 8x tau=0.06s)
        const double ticksPostSolo = proc->synthEngine().drums().totalElapsedTicks();

        if (proc->instPeak(1u) < 0.01f
            || proc->instPeak(0u) > 1e-4f
            || proc->rhyPeak() > 1e-4f
            || proc->masterPeakL() < 0.005f) {
            std::cerr << "[FAIL] MixerTest3: Solo isolation failed (inst1=" << proc->instPeak(0u)
                      << ", inst2=" << proc->instPeak(1u) << ", rhy=" << proc->rhyPeak()
                      << ", masterL=" << proc->masterPeakL() << ")\n";
            return false;
        }

        // Rhythm clock must have kept advancing while soloed away
        if ((ticksPostSolo - ticksPreSolo) < 40.0) {
            std::cerr << "[FAIL] MixerTest3: Rhythm clock stalled during INST 2 solo!\n";
            return false;
        }

        // Release INST 2 (Ch 2) while INST 1 (Ch 1) and RHY remain running -> master output drops to ~0
        juce::MidiBuffer releaseInst2;
        releaseInst2.addEvent(juce::MidiMessage::noteOff(2, 64), 0);
        renderBlock(*proc, 64000, &releaseInst2); // ~1.33 s so INST 2 voice envelope finishes
        if (proc->masterPeakL() > 1e-3f || proc->masterPeakR() > 1e-3f) {
            std::cerr << "[FAIL] MixerTest3: Non-soloed INST 1 or RHY leaked into master while INST 2 silent! (L="
                      << proc->masterPeakL() << ", R=" << proc->masterPeakR() << ")\n";
            return false;
        }

        // Saved PMEM volumes & R.Vol must be completely untouched
        proc->pullLatestAudioMemoryToHost();
        if (proc->hostMemory().activePerformance().instruments[0].volume != 90u
            || proc->hostMemory().activePerformance().instruments[1].volume != 80u
            || proc->hostMemory().activeRhythmKit().rhythmVolume != 85u) {
            std::cerr << "[FAIL] MixerTest3: Solo modified saved PMEM or R.Vol!\n";
            return false;
        }

        // Clear Solo -> INST 1 & RHY immediately audible again
        mixer.clearAllSoloFromMixer();
        renderBlock(*proc, 8192);
        if (proc->mixerSoloMask() != 0u || mixer.isClearSoloButtonLit()
            || proc->instPeak(0u) < 0.005f || proc->rhyPeak() < 0.005f) {
            std::cerr << "[FAIL] MixerTest3: CLEAR SOLO failed to restore INST 1 & RHY audio!\n";
            return false;
        }

        std::cout << "[PASS] Test 3: Non-Destructive Solo on INST 2 (Clock & Saved PMEM/R.Vol Untouched) PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 4: Per-Channel Atomic Peak Meters (instPeak[0..7], rhyPeak, masterL/R) & Decay
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        proc->selectPerformancePresetAtomic(vf50::state::MemoryBank::FactoryRom, 0u, true);
        renderBlock(*proc, 512);

        // Map INST 1..8 onto MIDI Channels 1..8 with loud voice P00 (or P02) and trigger all 8 + RHY
        auto pf = proc->hostMemory().activePerformance();
        pf.assignMode = 0u;
        for (std::size_t p = 0u; p < 8u; ++p) {
            pf.instruments[p].voiceBank = 1u;
            pf.instruments[p].voiceNumber = 0u;
            pf.instruments[p].receiveChannel = static_cast<uint8_t>(p);
            pf.instruments[p].maxNotes = 2u;
            pf.instruments[p].limitLow = 0u;
            pf.instruments[p].limitHigh = 127u;
            pf.instruments[p].outAssign = 3u;
            pf.instruments[p].volume = 99u;
        }
        proc->hostMemory().setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
        proc->hostMemory().perfEditSlot().setFromPerformance(pf);
        proc->commitHostPerfEditToAudio(true);
        renderBlock(*proc, 512);

        proc->requestClassicRhythmStartFactory(0u, 120.0, true);
        juce::MidiBuffer allChNotes;
        for (int ch = 1; ch <= 8; ++ch) {
            allChNotes.addEvent(juce::MidiMessage::noteOn(ch, 56 + ch, static_cast<juce::uint8>(115)), 0);
        }
        renderBlock(*proc, 4096, &allChNotes);

        for (std::size_t p = 0u; p < 8u; ++p) {
            if (proc->instPeak(p) <= 0.005f) {
                std::cerr << "[FAIL] MixerTest4: instPeak(" << p << ") remained ~0 during playback ("
                          << proc->instPeak(p) << ")\n";
                return false;
            }
        }
        if (proc->rhyPeak() <= 0.005f || proc->masterPeakL() <= 0.005f || proc->masterPeakR() <= 0.005f) {
            std::cerr << "[FAIL] MixerTest4: rhyPeak or masterPeakL/R remained ~0 during playback (rhy="
                      << proc->rhyPeak() << ", L=" << proc->masterPeakL() << ", R=" << proc->masterPeakR() << ")\n";
            return false;
        }

        // Silence all voices & render 1.5s of silent audio -> all 11 meters must decay cleanly to < 1e-4f
        proc->requestClassicRhythmStop();
        proc->synthEngine().allSoundOff();
        renderBlock(*proc, 72000);

        for (std::size_t p = 0u; p < 8u; ++p) {
            if (proc->instPeak(p) >= 1e-4f) {
                std::cerr << "[FAIL] MixerTest4: instPeak(" << p << ") did not decay when silent ("
                          << proc->instPeak(p) << ")\n";
                return false;
            }
        }
        if (proc->rhyPeak() >= 1e-4f || proc->masterPeakL() >= 1e-4f || proc->masterPeakR() >= 1e-4f) {
            std::cerr << "[FAIL] MixerTest4: rhyPeak or masterPeak did not decay when silent (rhy="
                      << proc->rhyPeak() << ", L=" << proc->masterPeakL() << ", R=" << proc->masterPeakR() << ")\n";
            return false;
        }

        std::cout << "[PASS] Test 4: All 11 Atomic Peak Meters Active During Playback & Decay Cleanly PASSED.\n";
    }

    return true;
}

bool runComprehensiveConsoleMixerAndDemoLedTests() {
    std::cout << "\n=== Running Comprehensive Console Mixer & DEMO LED Verification Tests ===\n";

    auto renderBlock = [](vf50::plugin::VF50AudioProcessor& p, int totalSamples, juce::MidiBuffer* firstBlockMidi = nullptr, float* outMaxAbs = nullptr, double* outRms = nullptr) {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer emptyMidi;
        juce::MidiBuffer firstCopy;
        if (firstBlockMidi != nullptr) {
            firstCopy = *firstBlockMidi;
        }
        double sumSq = 0.0;
        int totalCount = 0;
        int done = 0;
        while (done < totalSamples) {
            const int chunk = std::min(512, totalSamples - done);
            buf.setSize(2, chunk, false, false, true);
            buf.clear();
            auto& midi = (done == 0 && firstBlockMidi != nullptr) ? firstCopy : emptyMidi;
            p.processBlock(buf, midi);
            for (int c = 0; c < 2; ++c) {
                const float* ptr = buf.getReadPointer(c);
                for (int i = 0; i < chunk; ++i) {
                    const float s = ptr[i];
                    if (outMaxAbs != nullptr) {
                        *outMaxAbs = std::max(*outMaxAbs, std::abs(s));
                    }
                    sumSq += static_cast<double>(s) * static_cast<double>(s);
                }
            }
            totalCount += chunk * 2;
            done += chunk;
        }
        if (outRms != nullptr) {
            *outRms = (totalCount > 0) ? std::sqrt(sumSq / static_cast<double>(totalCount)) : 0.0;
        }
    };

    // -------------------------------------------------------------------------
    // TEST 1: DEMO Button Status LED Across All DEMO Screens, Playback & Mode Exits
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> baseEd(proc->createEditor());
        auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
        if (editor == nullptr) {
            std::cerr << "[FAIL] DemoLedTest: Failed to create editor\n";
            return false;
        }
        auto& canvas = editor->mainCanvas();
        auto& classic = canvas.classicController();

        if (editor->demoLedState() || canvas.classicLedSnapshot().demo != vf50::classic::LedState::Off) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED initially lit on boot\n";
            return false;
        }

        // Press DEMO -> UT DEMO> (DemoRoot) -> LED ON
        canvas.pressDemoButton();
        if (!editor->demoLedState() || classic.screen() != vf50::classic::ClassicScreen::DemoRoot
            || canvas.classicLedSnapshot().demo != vf50::classic::LedState::SolidGreen) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED not SolidGreen on DemoRoot\n";
            return false;
        }

        // Select >ROM Play -> DemoConfirm ("Are you sure? All data will be changed !") -> LED ON
        canvas.pressSoftKey(0);
        if (!editor->demoLedState() || classic.screen() != vf50::classic::ClassicScreen::DemoConfirm
            || canvas.classicLedSnapshot().demo != vf50::classic::LedState::SolidGreen) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED not SolidGreen on DemoConfirm\n";
            return false;
        }

        // Confirm Yes -> DemoPlay -> Start playback -> LED stays ON
        canvas.pressKeypadYesButton();
        renderBlock(*proc, 512);
        canvas.pressSoftKey(4); // >START
        renderBlock(*proc, 1024);
        if (!proc->isDemoPlaying() || !editor->demoLedState()
            || canvas.classicLedSnapshot().demo != vf50::classic::LedState::SolidGreen) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED not SolidGreen during DemoPlay playback\n";
            return false;
        }

        // >EXIT -> LED OFF immediately
        canvas.pressSoftKey(7);
        renderBlock(*proc, 512);
        if (editor->demoLedState() || canvas.classicLedSnapshot().demo != vf50::classic::LedState::Off) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED stayed lit after >EXIT\n";
            return false;
        }

        // V.Edit -> LED ON, then >EXIT -> LED OFF
        canvas.pressDemoButton();
        canvas.pressSoftKey(6); // >V. Edit
        canvas.pressKeypadYesButton();
        if (!editor->demoLedState() || classic.screen() != vf50::classic::ClassicScreen::DemoVoiceEdit
            || canvas.classicLedSnapshot().demo != vf50::classic::LedState::SolidGreen) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED not SolidGreen on DemoVoiceEdit\n";
            return false;
        }
        canvas.pressSoftKey(7);
        if (editor->demoLedState() || canvas.classicLedSnapshot().demo != vf50::classic::LedState::Off) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED stayed lit after DemoVoiceEdit >EXIT\n";
            return false;
        }

        // UT EDIT MESSAGE -> LED ON, then switching to every other mode turns LED OFF
        canvas.rightClickDemoButton();
        canvas.pressStoreButton();
        if (!editor->demoLedState() || classic.screen() != vf50::classic::ClassicScreen::UtilEditMessage
            || canvas.classicLedSnapshot().demo != vf50::classic::LedState::SolidGreen) {
            std::cerr << "[FAIL] DemoLedTest: DEMO LED not SolidGreen on UtilEditMessage\n";
            return false;
        }

        auto checkModeTurnsDemoLedOff = [&](auto&& fn, const char* name) -> bool {
            canvas.pressDemoButton();
            if (!editor->demoLedState()) {
                std::cerr << "[FAIL] DemoLedTest: Failed to re-enter DEMO before " << name << "\n";
                return false;
            }
            fn();
            if (editor->demoLedState() || canvas.classicLedSnapshot().demo != vf50::classic::LedState::Off) {
                std::cerr << "[FAIL] DemoLedTest: DEMO LED did not turn OFF when switching to " << name << "\n";
                return false;
            }
            return true;
        };

        if (!checkModeTurnsDemoLedOff([&]() { canvas.pressPerformanceButton(); }, "PERFORMANCE")
            || !checkModeTurnsDemoLedOff([&]() { canvas.pressSingleButton(); }, "SINGLE")
            || !checkModeTurnsDemoLedOff([&]() { canvas.pressSeqButton(); }, "SEQ")
            || !checkModeTurnsDemoLedOff([&]() { canvas.pressRhythmButton(); }, "RHYTHM")
            || !checkModeTurnsDemoLedOff([&]() { canvas.pressDiskButton(); }, "DISK")
            || !checkModeTurnsDemoLedOff([&]() { canvas.pressCardButton(); }, "CARD")
            || !checkModeTurnsDemoLedOff([&]() { canvas.pressMidiButton(); }, "MIDI")
            || !checkModeTurnsDemoLedOff([&]() { canvas.pressSetupButton(); }, "SETUP")) {
            return false;
        }

        std::cout << "[PASS] ConsoleTest 1: DEMO Status LED across all DEMO screens, playback, >EXIT & mode switches PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 2: Stereo Metering, Active Voice Counts, Balance/Pan & FX Send Bus
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        proc->selectPerformancePresetAtomic(vf50::state::MemoryBank::FactoryRom, 0u, true);
        renderBlock(*proc, 512);

        vf50::gui::VF50MainCanvas canvas(*proc);
        canvas.pressPerformanceButton();
        canvas.pressMixerToggleButton();
        auto& mixer = canvas.mixerDrawer();

        auto pf = proc->hostMemory().activePerformance();
        pf.assignMode = 0u;
        for (std::size_t p = 0u; p < 8u; ++p) {
            pf.instruments[p].voiceBank = 1u;
            pf.instruments[p].voiceNumber = 0u;
            pf.instruments[p].receiveChannel = static_cast<uint8_t>(p);
            pf.instruments[p].maxNotes = (p < 2u) ? 4u : 0u;
            pf.instruments[p].limitLow = 0u;
            pf.instruments[p].limitHigh = 127u;
            pf.instruments[p].outAssign = 3u;
            pf.instruments[p].volume = 95u;
            pf.instruments[p].effectEnable = 1u;
        }
        pf.effect.effectType = 1u; // Reverb Hall
        pf.effect.balance = 99u;   // 100% wet mix emphasis
        pf.effect.outLevel = 100u;
        pf.effect.stereoMix = 0u;
        proc->hostMemory().setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
        proc->hostMemory().perfEditSlot().setFromPerformance(pf);
        proc->commitHostPerfEditToAudio(true);
        renderBlock(*proc, 512);

        // Pan INST 1 hard left (-1.0f), bypass FX for clean L/R isolation check
        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::EffectBypass, 1.0f);
        mixer.setChannelBalanceFromMixer(0u, -1.0f);
        renderBlock(*proc, 512);

        juce::MidiBuffer noteCh1;
        noteCh1.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(115)), 0);
        renderBlock(*proc, 4096, &noteCh1);

        if (proc->instPeakL(0u) <= 0.01f || proc->instPeakR(0u) != 0.0f
            || proc->instActiveVoices(0u) < 1u || proc->totalActiveFmVoices() < 1u) {
            std::cerr << "[FAIL] ConsoleTest2: Hard-left stereo meter / active voice count failed (L="
                      << proc->instPeakL(0u) << ", R=" << proc->instPeakR(0u)
                      << ", v0=" << static_cast<int>(proc->instActiveVoices(0u))
                      << ", tot=" << static_cast<int>(proc->totalActiveFmVoices()) << ")\n";
            return false;
        }

        // Test FX Send = 0 vs FX Send = 99 (re-enable FX, center balance)
        mixer.setChannelBalanceFromMixer(0u, 0.0f);
        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::EffectBypass, 0.0f);
        mixer.setChannelFxSendFromMixer(0u, 0);
        proc->synthEngine().allSoundOff();
        proc->synthEngine().effects().reset();
        renderBlock(*proc, 512);

        // Render INST 1 with FX Send = 0 and check reverb tail after noteOff -> tail must be 0
        juce::MidiBuffer note1On, note1Off;
        note1On.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(115)), 0);
        renderBlock(*proc, 2048, &note1On);
        note1Off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        renderBlock(*proc, 48000, &note1Off);
        // After voice release completes, render 4096 more samples and measure peak: with FX Send=0, zero reverb tail
        float tailSend0 = 0.0f;
        renderBlock(*proc, 4096, nullptr, &tailSend0);

        // Now trigger INST 2 with FX Send = 99 and same note/release -> long reverb wet tail must be audible!
        mixer.setChannelFxSendFromMixer(1u, 99);
        juce::MidiBuffer note2On, note2Off;
        note2On.addEvent(juce::MidiMessage::noteOn(2, 60, static_cast<juce::uint8>(115)), 0);
        renderBlock(*proc, 2048, &note2On);
        note2Off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
        renderBlock(*proc, 48000, &note2Off);
        float tailSend99 = 0.0f;
        renderBlock(*proc, 4096, nullptr, &tailSend99);

        if (tailSend0 > 1e-4f || tailSend99 < 0.002f) {
            std::cerr << "[FAIL] ConsoleTest2: FX Send bus routing failed (tailSend0="
                      << tailSend0 << ", tailSend99=" << tailSend99 << ")\n";
            return false;
        }

        std::cout << "[PASS] ConsoleTest 2: Stereo Metering, Active Voice Counts, Balance/Pan & FX Send Bus PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 3: 2-Band EQ, Safety Limiter & RESET MIX
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        proc->selectPerformancePresetAtomic(vf50::state::MemoryBank::FactoryRom, 0u, true);
        renderBlock(*proc, 512);

        vf50::gui::VF50MainCanvas canvas(*proc);
        canvas.pressPerformanceButton();
        canvas.pressMixerToggleButton();
        auto& mixer = canvas.mixerDrawer();

        auto pf = proc->hostMemory().activePerformance();
        pf.assignMode = 0u;
        for (std::size_t p = 0u; p < 8u; ++p) {
            pf.instruments[p].voiceBank = 1u;
            pf.instruments[p].voiceNumber = 0u;
            pf.instruments[p].receiveChannel = 0u; // All 8 layered on Ch 1 for dense peak stress!
            pf.instruments[p].maxNotes = 2u;
            pf.instruments[p].limitLow = 0u;
            pf.instruments[p].limitHigh = 127u;
            pf.instruments[p].outAssign = 3u;
            pf.instruments[p].volume = 99u;
        }
        proc->hostMemory().setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
        proc->hostMemory().perfEditSlot().setFromPerformance(pf);
        proc->commitHostPerfEditToAudio(true);
        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::EffectBypass, 1.0f);
        renderBlock(*proc, 512);

        // Measure flat EQ vs +12 dB Low/High Shelf EQ on INST 1 (soloed, linear headroom below limiter knee)
        mixer.toggleInstSoloFromMixer(0u);
        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::MasterVolume, 0.25f);
        juce::MidiBuffer eqNote;
        eqNote.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(100)), 0);
        double rmsFlat = 0.0;
        renderBlock(*proc, 4096, &eqNote, nullptr, &rmsFlat);

        mixer.setChannelEqFromMixer(0u, true, 12.0f, 12.0f);
        proc->synthEngine().allSoundOff();
        proc->synthEngine().reset();
        renderBlock(*proc, 512);
        double rmsEqBoost = 0.0;
        renderBlock(*proc, 4096, &eqNote, nullptr, &rmsEqBoost);
        mixer.clearAllSoloFromMixer();

        if (!std::isfinite(rmsEqBoost) || rmsFlat <= 0.005 || rmsEqBoost <= rmsFlat * 1.25) {
            std::cerr << "[FAIL] ConsoleTest3: 2-Band EQ boost did not increase energy cleanly (flatRms="
                      << rmsFlat << ", boostedRms=" << rmsEqBoost << ")\n";
            return false;
        }

        // Dense multi-voice hot test for Safety Limiter ON vs OFF
        proc->synthEngine().allSoundOff();
        proc->synthEngine().reset();
        vf50::params::setApvtsRawValue(proc->apvts(), vf50::params::ParamIndex::MasterVolume, 1.25f);
        renderBlock(*proc, 256);
        juce::MidiBuffer chord;
        for (int n : { 48, 52, 55, 60, 64, 67 }) {
            chord.addEvent(juce::MidiMessage::noteOn(1, n, static_cast<juce::uint8>(127)), 0);
        }
        float limitedMax = 0.0f;
        renderBlock(*proc, 4096, &chord, &limitedMax);
        const float grVal = proc->limiterGainReduction();
        if (limitedMax >= 1.0f || grVal <= 0.0f) {
            std::cerr << "[FAIL] ConsoleTest3: Safety Limiter failed to clamp hot peaks below 1.0f (max="
                      << limitedMax << ", gr=" << grVal << ")\n";
            return false;
        }

        // Toggle Safety Limiter OFF -> peaks exceed 0.97f
        mixer.toggleSafetyLimiterFromMixer();
        proc->synthEngine().allSoundOff();
        renderBlock(*proc, 512);
        float unlimitedMax = 0.0f;
        renderBlock(*proc, 4096, &chord, &unlimitedMax);
        if (unlimitedMax <= 0.97f || mixer.isSafetyLimiterButtonLit()) {
            std::cerr << "[FAIL] ConsoleTest3: Disabling Safety Limiter did not allow raw hot peaks (max="
                      << unlimitedMax << ")\n";
            return false;
        }

        // RESET MIX -> restores Balance C, nominal FX Sends, flat/bypassed EQ, clears Solo, preserves VF50 volumes
        mixer.setChannelBalanceFromMixer(0u, -0.75f);
        mixer.setChannelFxSendFromMixer(0u, 15);
        mixer.toggleInstSoloFromMixer(0u);
        mixer.resetMixFromMixer();

        if (std::abs(mixer.displayedChannelBalance(0u)) > 1e-4f
            || mixer.displayedChannelFxSend(0u) != 99
            || mixer.isChannelEqEnabled(0u)
            || std::abs(mixer.displayedChannelEqLowDb(0u)) > 1e-4f
            || std::abs(mixer.displayedChannelEqHighDb(0u)) > 1e-4f
            || proc->mixerSoloMask() != 0u
            || proc->hostMemory().activePerformance().instruments[0].volume != 99u) {
            std::cerr << "[FAIL] ConsoleTest3: RESET MIX did not restore neutral console state!\n";
            return false;
        }

        std::cout << "[PASS] ConsoleTest 3: 2-Band EQ, Toggleable -0.3dBFS Safety Limiter & RESET MIX PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 4: Native VF50 Mixer Sync (Notes, Note Shift, Detune, FX Type, Drum INST SET & COMPARE)
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        proc->selectPerformancePresetAtomic(vf50::state::MemoryBank::FactoryRom, 0u, true);
        renderBlock(*proc, 512);

        vf50::gui::VF50MainCanvas canvas(*proc);
        canvas.pressPerformanceButton();
        canvas.pressMixerToggleButton();
        auto& mixer = canvas.mixerDrawer();

        const auto origPerf = proc->hostMemory().activePerformance();
        const int origNotes = (origPerf.assignMode == 2u)
                                  ? ((origPerf.instruments[0].reserveNotes == 0u) ? 0 : static_cast<int>(origPerf.instruments[0].reserveNotes) - 1)
                                  : static_cast<int>(origPerf.instruments[0].maxNotes);
        const int origShift = static_cast<int>(origPerf.instruments[0].noteShift);
        const int origDet   = static_cast<int>(origPerf.instruments[0].detune);
        const int origFx    = static_cast<int>(origPerf.effect.effectType);

        if (!mixer.setInstNotesFromMixer(0u, 6)
            || !mixer.setInstNoteShiftFromMixer(0u, 12)
            || !mixer.setInstDetuneFromMixer(0u, -4)
            || !mixer.setFxTypeFromMixer(7)) {
            std::cerr << "[FAIL] ConsoleTest4: Native VF50 Mixer setter returned false\n";
            return false;
        }
        renderBlock(*proc, 1024);
        proc->pullLatestAudioMemoryToHost();

        const auto& edited = proc->hostMemory().activePerformance();
        const int editedNotes = (edited.assignMode == 2u)
                                    ? ((edited.instruments[0].reserveNotes == 0u) ? 0 : static_cast<int>(edited.instruments[0].reserveNotes) - 1)
                                    : static_cast<int>(edited.instruments[0].maxNotes);
        if (editedNotes != 6
            || edited.instruments[0].noteShift != 12
            || edited.instruments[0].detune != -4
            || edited.effect.effectType != 7u
            || !proc->hostMemory().isPerfEditDirty()) {
            std::cerr << "[FAIL] ConsoleTest4: Native VF50 PMEM did not update from Mixer!\n";
            return false;
        }

        // COMPARE ON -> reverts to stored preset; COMPARE OFF -> restores edits
        canvas.pressCompareButton();
        renderBlock(*proc, 512);
        mixer.syncFromProcessor();
        if (!proc->hostMemory().isPerformanceCompareActive()
            || mixer.displayedInstNotes(0u) != origNotes
            || mixer.displayedInstNoteShift(0u) != origShift
            || mixer.displayedInstDetune(0u) != origDet
            || mixer.displayedFxType() != origFx) {
            std::cerr << "[FAIL] ConsoleTest4: COMPARE ON did not revert Mixer native values!\n";
            return false;
        }
        canvas.pressCompareButton();
        renderBlock(*proc, 512);
        mixer.syncFromProcessor();
        if (proc->hostMemory().isPerformanceCompareActive()
            || mixer.displayedInstNotes(0u) != 6
            || mixer.displayedInstNoteShift(0u) != 12
            || mixer.displayedInstDetune(0u) != -4
            || mixer.displayedFxType() != 7) {
            std::cerr << "[FAIL] ConsoleTest4: COMPARE OFF did not restore Mixer native values!\n";
            return false;
        }

        // Embedded 61-Drum Sub-Mix View: edit drum 0 Vol=9, Pan=-2 -> verify INST SET & live drum engine
        mixer.setDrumSubMixVisible(true);
        mixer.setDrumInstVolumeFromMixer(0u, 9);
        mixer.setDrumInstPanFromMixer(0u, -2);
        renderBlock(*proc, 512);
        proc->pullLatestAudioMemoryToHost();
        if (!mixer.isDrumSubMixVisible()
            || proc->hostMemory().activeRhythmKit().instruments[0].volume != 9u
            || proc->hostMemory().activeRhythmKit().instruments[0].pan != -2
            || proc->synthEngine().drums().instrumentState(0u).volume != 9u
            || proc->synthEngine().drums().instrumentState(0u).pan != -2) {
            std::cerr << "[FAIL] ConsoleTest4: 61-Drum Sub-Mix INST SET sync failed!\n";
            return false;
        }
        mixer.setDrumSubMixVisible(false);

        std::cout << "[PASS] ConsoleTest 4: Native VF50 Sync (Notes/Shift/Detune/FX Type/Drum INST SET) & COMPARE PASSED.\n";
    }

    // -------------------------------------------------------------------------
    // TEST 5: Full Console State Round-Trip & Legacy v1 Backward Compatibility
    // -------------------------------------------------------------------------
    {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 512);
        proc->setMixerVisible(true);
        proc->setMixerChannelBalance(0u, -0.55f);
        proc->setMixerChannelBalance(8u, 0.42f);
        proc->setMixerChannelFxSend(1u, 33u);
        proc->setMixerChannelFxSend(8u, 80u);
        proc->setMixerChannelEqEnabled(2u, true);
        proc->setMixerChannelEqLowDb(2u, -6.5f);
        proc->setMixerChannelEqHighDb(2u, 4.5f);
        proc->setMasterSafetyLimiterEnabled(false);

        juce::MemoryBlock stateV2;
        proc->getStateInformation(stateV2);

        auto procRestored = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        procRestored->prepareToPlay(48000.0, 512);
        procRestored->setStateInformation(stateV2.getData(), static_cast<int>(stateV2.getSize()));

        if (!procRestored->isMixerVisible()
            || std::abs(procRestored->mixerChannelBalance(0u) - (-0.55f)) > 1e-4f
            || std::abs(procRestored->mixerChannelBalance(8u) - 0.42f) > 1e-4f
            || procRestored->mixerChannelFxSend(1u) != 33u
            || procRestored->mixerChannelFxSend(8u) != 80u
            || !procRestored->mixerChannelEqEnabled(2u)
            || std::abs(procRestored->mixerChannelEqLowDb(2u) - (-6.5f)) > 1e-4f
            || std::abs(procRestored->mixerChannelEqHighDb(2u) - 4.5f) > 1e-4f
            || procRestored->isMasterSafetyLimiterEnabled()) {
            std::cerr << "[FAIL] ConsoleTest5: v2 Console state round-trip mismatch!\n";
            return false;
        }

        // Backward compatibility with legacy v1 8-byte 'VF5M' trailer
        const auto* raw = static_cast<const uint8_t*>(stateV2.getData());
        const std::size_t sz = stateV2.getSize();
        std::size_t vf50mOffset = sz;
        for (std::size_t i = 0u; i + 8u <= sz; ++i) {
            if (raw[i] == 'V' && raw[i + 1u] == 'F' && raw[i + 2u] == '5' && raw[i + 3u] == 'M') {
                vf50mOffset = i;
            }
        }
        if (vf50mOffset >= sz) {
            std::cerr << "[FAIL] ConsoleTest5: Could not locate VF50M trailer in serialized state\n";
            return false;
        }

        juce::MemoryBlock legacyV1(raw, vf50mOffset);
        const uint8_t v1Trailer[8] = { 'V', 'F', '5', 'M', 1u, 1u, 0u, 0u };
        legacyV1.append(v1Trailer, 8u);

        auto procLegacy = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        procLegacy->prepareToPlay(48000.0, 512);
        procLegacy->setStateInformation(legacyV1.getData(), static_cast<int>(legacyV1.getSize()));
        if (!procLegacy->isMixerVisible()
            || !procLegacy->isMasterSafetyLimiterEnabled()
            || std::abs(procLegacy->mixerChannelBalance(0u)) > 1e-4f
            || procLegacy->mixerChannelEqEnabled(2u)) {
            std::cerr << "[FAIL] ConsoleTest5: Legacy v1 VF50M compatibility failed!\n";
            return false;
        }

        std::cout << "[PASS] ConsoleTest 5: Full Console State Round-Trip & Legacy v1 Compatibility PASSED.\n";
    }

    return true;
}

bool runLifecycleAndMemoryLeakStressTest(const juce::File& buildDir) {
    std::cout << "\n=== Running 20-Iteration Lifecycle & Memory-Leak Stress Test ===\n";
    constexpr int kIterations = 20;

    const juce::File tmpDir = buildDir.getChildFile("vf50_leak_stress_tmp");
    (void)tmpDir.deleteRecursively();
    (void)tmpDir.createDirectory();
    const juce::File tmpImg = tmpDir.getChildFile("stress_disk.img");

    // Pre-build one formatted FAT12 image with an .ALL file so disk mount/load is exercised on every pass
    {
        auto rawImg = vf50::disk::VF50DiskManager::createFormattedFat12Image(false, "STRESS720");
        if (!tmpImg.replaceWithData(rawImg.data(), rawImg.size())) {
            std::cerr << "[FAIL] LeakStress: Failed writing FAT12 stress image.\n";
            (void)tmpDir.deleteRecursively();
            return false;
        }
        auto tmpProc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        vf50::disk::VF50DiskManager prepDm;
        juce::String prepStatus;
        if (!prepDm.mountFat12ImageFile(tmpImg, prepStatus)
            || !prepDm.saveAllToMountedSource("VF50STRS.ALL", *tmpProc, prepStatus)) {
            std::cerr << "[FAIL] LeakStress: Failed populating FAT12 stress image: " << prepStatus << "\n";
            (void)tmpDir.deleteRecursively();
            return false;
        }
    }

    for (int iter = 0; iter < kIterations; ++iter) {
        auto proc = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        proc->prepareToPlay(48000.0, 256);

        // Load a synthetic User Sequencer song + User Rhythm pattern and configure Mixer FX/EQ/Solo
        auto seqBuf = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        seqBuf->clear();
        seqBuf->setTitle("STRESS01");
        seqBuf->initialTempoBpm = 128u;
        seqBuf->timeSigNum = 4u;
        seqBuf->timeSigDen = 4u;
        seqBuf->ticksPerBar = 192u;
        seqBuf->totalBars = 2u;
        seqBuf->totalTicks = 384u;
        seqBuf->playMode = 1u;
        seqBuf->trackChannels = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
        seqBuf->events[0] = { 0u, vf50::generated::DemoEventType::FmNoteOn, 0u, 60u, 110u };
        seqBuf->events[1] = { 24u, vf50::generated::DemoEventType::FmNoteOn, 1u, 64u, 100u };
        seqBuf->events[2] = { 96u, vf50::generated::DemoEventType::FmNoteOff, 0u, 60u, 0u };
        seqBuf->events[3] = { 120u, vf50::generated::DemoEventType::FmNoteOff, 1u, 64u, 0u };
        seqBuf->eventCount = 4u;
        seqBuf->valid = true;
        proc->publishUserSequenceToSlot(0u, *seqBuf);

        vf50::state::UserRhythmPattern rp{};
        rp.clear();
        rp.bars = 1u;
        rp.timeSigNum = 4u;
        rp.timeSigDen = 4u;
        rp.totalTicks = 192u;
        rp.eventCount = 2u;
        rp.events[0] = { 0u, 0u, 7u };
        rp.events[1] = { 48u, 6u, 6u };
        proc->hostMemory().mutableUserRhythmPattern(0u) = rp;

        proc->requestDemoStart(0u);
        proc->requestClassicRhythmStartUser(0u, rp, 128.0, true);
        proc->setMixerChannelBalance(0u, -0.35f);
        proc->setMixerChannelFxSend(0u, 72u);
        proc->setMixerChannelEqEnabled(0u, true);
        proc->setMixerChannelEqLowDb(0u, 3.0f);
        proc->setMixerChannelEqHighDb(0u, -2.5f);
        proc->setMixerChannelBalance(8u, 0.25f);

        juce::AudioBuffer<float> audio(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{ 108 }), 0);
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 64), 32);
        midi.addEvent(juce::MidiMessage::pitchWheel(1, 9000), 64);

        for (int b = 0; b < 6; ++b) {
            audio.clear();
            proc->processBlock(audio, midi);
            midi.clear();
            if (b == 3) {
                midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            }
            for (int ch = 0; ch < 2; ++ch) {
                const float* p = audio.getReadPointer(ch);
                for (int s = 0; s < 256; ++s) {
                    if (!std::isfinite(p[s]) || std::fpclassify(p[s]) == FP_SUBNORMAL) {
                        std::cerr << "[FAIL] LeakStress iter " << iter << ": non-finite/subnormal sample\n";
                        (void)tmpDir.deleteRecursively();
                        return false;
                    }
                }
            }
        }

        // State round-trip
        juce::MemoryBlock stateBlock;
        proc->getStateInformation(stateBlock);
        if (stateBlock.getSize() < 64u) {
            std::cerr << "[FAIL] LeakStress iter " << iter << ": empty state block\n";
            (void)tmpDir.deleteRecursively();
            return false;
        }
        proc->setStateInformation(stateBlock.getData(), static_cast<int>(stateBlock.getSize()));

        // Disk mount & load
        {
            vf50::disk::VF50DiskManager dm;
            juce::String status;
            if (!dm.mountFat12ImageFile(tmpImg, status) || dm.entries().empty()
                || !dm.loadMountedEntry(0u, *proc, vf50::disk::VF50LoadFilter::AutoByExtension, status)) {
                std::cerr << "[FAIL] LeakStress iter " << iter << ": disk mount/load failed: " << status << "\n";
                (void)tmpDir.deleteRecursively();
                return false;
            }
            dm.unmount();
        }

        // Full editor lifecycle: Programmer, Visual Mixer + 61-Drum Sub-Mix, Virtual Keyboard, ROM Guide, Resize
        {
            std::unique_ptr<juce::AudioProcessorEditor> baseEd(proc->createEditor());
            auto* editor = dynamic_cast<vf50::plugin::VF50AudioProcessorEditor*>(baseEd.get());
            if (editor == nullptr) {
                std::cerr << "[FAIL] LeakStress iter " << iter << ": createEditor failed\n";
                (void)tmpDir.deleteRecursively();
                return false;
            }
            auto& canvas = editor->mainCanvas();
            canvas.setRomGuideVisible(true);
            canvas.setRomGuideVisible(false);
            editor->setProgrammerVisible(true);
            editor->setProgrammerVisible(false);
            editor->setMixerVisible(true);
            canvas.mixerDrawer().setDrumSubMixVisible(true);
            canvas.mixerDrawer().setDrumSubMixVisible(false);
            editor->setMixerVisible(false);
            editor->setVirtualKeyboardVisible(true);
            editor->setVirtualKeyboardVisible(false);
            editor->applyScaleFactor(0.75f);
            editor->applyScaleFactor(1.00f);
            canvas.refreshDisplayAndLeds();
            baseEd.reset();
        }

        proc->requestDemoStop();
        proc->requestClassicRhythmStop();
        proc->releaseResources();
        proc.reset();
    }

    (void)tmpDir.deleteRecursively();
    std::cout << "[PASS] Lifecycle & Memory-Leak Stress Test (" << kIterations
              << " full Processor + DSP + State + Disk + Editor iterations) PASSED.\n";
    return true;
}

bool runMultiBusRoutingSelfTests() {
    std::cout << "\n=== Running Multi-Bus Routing & Variable Bus Mask Tests ===\n";

    // 1. Engine-level renderMultiBusBlock verification (isolated AUX, parallel MAIN, FX wet on Main, disabled fallback, sentinel bounds)
    {
        auto engPtr = std::make_unique<vf50::dsp::VF50SynthEngine>();
        auto& eng = *engPtr;
        eng.prepare(48000.0, 512u);
        eng.setMasterVolume(1.0f);
        eng.setSynthVolume(99u);
        eng.setChannelFxSend(0u, 99u);
        eng.effects().setBypass(true);

        constexpr std::size_t kGuard = 16u;
        constexpr std::size_t kMaxBlk = 300u;
        constexpr std::size_t kStride = kMaxBlk + 2u * kGuard;
        std::array<float, kStride> mainL{}, mainR{};
        std::array<std::array<float, kStride>, 9> auxL{}, auxR{};

        auto fillSentinels = [&]() {
            mainL.fill(-999.0f);
            mainR.fill(-999.0f);
            for (std::size_t c = 0u; c < 9u; ++c) {
                auxL[c].fill(-999.0f);
                auxR[c].fill(-999.0f);
            }
        };
        auto peakRange = [](const std::array<float, kStride>& buf, std::size_t len) noexcept -> float {
            float pk = 0.0f;
            for (std::size_t i = 0u; i < len; ++i) {
                pk = std::max(pk, std::abs(buf[kGuard + i]));
            }
            return pk;
        };
        auto makeSpec = [&](uint16_t mask) {
            vf50::dsp::MultiBusOutputSpec spec{};
            spec.mainL = mainL.data() + kGuard;
            spec.mainR = mainR.data() + kGuard;
            spec.hostEnabledAuxMask = mask;
            for (std::size_t c = 0u; c < 9u; ++c) {
                if (((mask >> c) & 1u) != 0u) {
                    spec.auxL[c] = auxL[c].data() + kGuard;
                    spec.auxR[c] = auxR[c].data() + kGuard;
                }
            }
            return spec;
        };

        // 1a. Isolated AUX (FX bypassed -> Aux 0 loud, Main silent)
        eng.setAuxExclusiveMask(0x01FFu);
        eng.noteOn(0u, 60u, 112u);
        fillSentinels();
        const auto specAux0 = makeSpec(0x0001u);
        eng.renderMultiBusBlock(specAux0, 256u);
        eng.renderMultiBusBlock(specAux0, 256u);
        const float isoMain = std::max(peakRange(mainL, 256u), peakRange(mainR, 256u));
        const float isoAux0 = std::max(peakRange(auxL[0], 256u), peakRange(auxR[0], 256u));
        if (isoAux0 <= 0.01f || isoMain >= 1e-5f) {
            std::cerr << "[FAIL] MultiBus Engine isolated AUX check failed (aux0=" << isoAux0
                      << ", main=" << isoMain << ")\n";
            return false;
        }

        // 1b. Wet FX return remains on Main / FX Out even when channel is AUX-exclusive
        vf50::generated::DspEffectSettings fx{};
        fx.effectType = 1u;
        fx.param1 = 45u;
        fx.param2 = 12u;
        fx.param3 = 0u;
        fx.balance = 50u;
        fx.outLevel = 99u;
        fx.stereoMix = 1u;
        eng.effects().loadPatchSettingsImmediate(fx);
        eng.effects().setBypass(false);
        float fxMain = 0.0f;
        float fxAux0 = 0.0f;
        for (int b = 0; b < 20; ++b) {
            fillSentinels();
            eng.renderMultiBusBlock(specAux0, 256u);
            fxMain = std::max(fxMain, std::max(peakRange(mainL, 256u), peakRange(mainR, 256u)));
            fxAux0 = std::max(fxAux0, std::max(peakRange(auxL[0], 256u), peakRange(auxR[0], 256u)));
        }
        if (fxMain <= 1e-4f || fxAux0 <= 0.01f) {
            std::cerr << "[FAIL] MultiBus Engine wet FX return on Main failed (fxMain=" << fxMain
                      << ", fxAux0=" << fxAux0 << ")\n";
            return false;
        }

        // 1c. Parallel MAIN mode (FX bypassed -> BOTH Main and Aux 0 active)
        eng.effects().setBypass(true);
        eng.setChannelAuxExclusive(0u, false);
        fillSentinels();
        eng.renderMultiBusBlock(specAux0, 256u);
        const float parMain = std::max(peakRange(mainL, 256u), peakRange(mainR, 256u));
        const float parAux0 = std::max(peakRange(auxL[0], 256u), peakRange(auxR[0], 256u));
        if (parMain <= 0.01f || parAux0 <= 0.01f) {
            std::cerr << "[FAIL] MultiBus Engine parallel MAIN+AUX failed (main=" << parMain
                      << ", aux0=" << parAux0 << ")\n";
            return false;
        }

        // 1d. Disabled Aux Bus Fallback (ch 0 set to AUX, host mask = 0 -> falls back to Main)
        eng.setChannelAuxExclusive(0u, true);
        fillSentinels();
        const auto specNone = makeSpec(0x0000u);
        eng.renderMultiBusBlock(specNone, 256u);
        const float fbMain = std::max(peakRange(mainL, 256u), peakRange(mainR, 256u));
        if (fbMain <= 0.01f) {
            std::cerr << "[FAIL] MultiBus Engine disabled-aux fallback failed (main=" << fbMain << ")\n";
            return false;
        }

        // 1e. Arbitrary bus masks & varied block sizes with strict sentinel bounds verification
        constexpr std::array<uint16_t, 8> kMasks = {
            0x0000u, 0x0001u, 0x0002u, 0x0080u, 0x0100u, 0x0105u, 0x00AAu, 0x01FFu
        };
        constexpr std::array<std::size_t, 4> kSizes = { 1u, 37u, 64u, 257u };
        for (const uint16_t mask : kMasks) {
            for (const std::size_t blk : kSizes) {
                fillSentinels();
                const auto sp = makeSpec(mask);
                eng.renderMultiBusBlock(sp, blk);

                auto checkBuf = [&](const std::array<float, kStride>& buf, bool active, const char* tag) -> bool {
                    for (std::size_t g = 0u; g < kGuard; ++g) {
                        if (buf[g] != -999.0f || buf[kGuard + blk + g] != -999.0f) {
                            std::cerr << "[FAIL] Sentinel overrun (" << tag << ", mask=" << mask
                                      << ", blk=" << blk << ")\n";
                            return false;
                        }
                    }
                    for (std::size_t i = 0u; i < blk; ++i) {
                        const float v = buf[kGuard + i];
                        if (active) {
                            if (!std::isfinite(v) || std::fpclassify(v) == FP_SUBNORMAL) {
                                std::cerr << "[FAIL] Non-finite/subnormal output (" << tag << ")\n";
                                return false;
                            }
                        } else if (v != -999.0f) {
                            std::cerr << "[FAIL] Wrote to disabled aux bus (" << tag << ")\n";
                            return false;
                        }
                    }
                    return true;
                };

                if (!checkBuf(mainL, true, "mainL") || !checkBuf(mainR, true, "mainR")) {
                    return false;
                }
                for (std::size_t c = 0u; c < 9u; ++c) {
                    const bool en = ((mask >> c) & 1u) != 0u;
                    if (!checkBuf(auxL[c], en, "auxL") || !checkBuf(auxR[c], en, "auxR")) {
                        return false;
                    }
                }
            }
        }
    }

#if defined(VF50_MULTI_OUT) && VF50_MULTI_OUT
    // 2. Full 10-Bus VF50AudioProcessor & Visual Mixer BUS pill verification
    {
        auto procPtr = std::make_unique<vf50::plugin::VF50AudioProcessor>();
        auto& proc = *procPtr;
        if (proc.getBusCount(false) != 10) {
            std::cerr << "[FAIL] Multi-Out processor expected 10 output buses, got " << proc.getBusCount(false) << "\n";
            return false;
        }
        if (proc.getBus(false, 0)->getName() != "Main / FX Out" || !proc.getBus(false, 0)->isEnabled()) {
            std::cerr << "[FAIL] Multi-Out Bus 0 must be enabled 'Main / FX Out'\n";
            return false;
        }
        for (int b = 1; b < 10; ++b) {
            if (proc.getBus(false, b)->isEnabled()) {
                std::cerr << "[FAIL] Multi-Out Aux Bus " << b << " should be disabled by default\n";
                return false;
            }
        }

        auto makeLayout = [](std::initializer_list<int> enabledBuses) {
            juce::AudioProcessor::BusesLayout lay;
            for (int b = 0; b < 10; ++b) {
                const bool en = std::find(enabledBuses.begin(), enabledBuses.end(), b) != enabledBuses.end();
                lay.outputBuses.add(en ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
            }
            return lay;
        };

        if (!proc.checkBusesLayoutSupported(makeLayout({ 0 })) ||
            !proc.checkBusesLayoutSupported(makeLayout({ 0, 1, 9 })) ||
            !proc.checkBusesLayoutSupported(makeLayout({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }))) {
            std::cerr << "[FAIL] Multi-Out rejected valid stereo bus subset layout\n";
            return false;
        }
        {
            auto badMono = makeLayout({ 0, 1 });
            badMono.outputBuses.getReference(1) = juce::AudioChannelSet::mono();
            if (proc.checkBusesLayoutSupported(badMono)) {
                std::cerr << "[FAIL] Multi-Out accepted invalid mono aux bus\n";
                return false;
            }
            auto badMain = makeLayout({ 1 });
            if (proc.checkBusesLayoutSupported(badMain)) {
                std::cerr << "[FAIL] Multi-Out accepted disabled Bus 0\n";
                return false;
            }
            auto badIn = makeLayout({ 0 });
            badIn.inputBuses.add(juce::AudioChannelSet::stereo());
            if (proc.checkBusesLayoutSupported(badIn)) {
                std::cerr << "[FAIL] Multi-Out accepted enabled audio input bus\n";
                return false;
            }
        }

        // Enable subset {0, 1, 9} -> 6 total output channels
        if (!proc.setBusesLayout(makeLayout({ 0, 1, 9 })) || proc.getTotalNumOutputChannels() != 6) {
            std::cerr << "[FAIL] Failed applying subset layout {0, 1, 9}\n";
            return false;
        }
        proc.prepareToPlay(48000.0, 256);
        vf50::params::setApvtsRawValue(proc.apvts(), vf50::params::ParamIndex::EffectBypass, 1.0f);

        juce::AudioBuffer<float> buf6(6, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{ 110 }), 0);
        for (int b = 0; b < 3; ++b) {
            buf6.clear();
            proc.processBlock(buf6, midi);
            midi.clear();
        }
        const float bus0Pk = proc.getBusBuffer(buf6, false, 0).getMagnitude(0, 256);
        const float bus1Pk = proc.getBusBuffer(buf6, false, 1).getMagnitude(0, 256);
        const float bus9Pk = proc.getBusBuffer(buf6, false, 9).getMagnitude(0, 256);
        if (bus1Pk <= 0.01f || bus0Pk >= 1e-5f || bus9Pk >= 1e-5f) {
            std::cerr << "[FAIL] 10-bus isolated processBlock failed (bus0=" << bus0Pk
                      << ", bus1=" << bus1Pk << ", bus9=" << bus9Pk << ")\n";
            return false;
        }

        // Toggle Ch 0 to parallel MAIN via Mixer Drawer UI & persist across state save/restore
        {
            auto dmPtr = std::make_unique<vf50::disk::VF50DiskManager>();
            auto ccPtr = std::make_unique<vf50::classic::VF50ClassicController>(proc, *dmPtr);
            auto drawerPtr = std::make_unique<vf50::gui::VF50MixerDrawer>(proc, *ccPtr);
            auto& drawer = *drawerPtr;
            drawer.syncFromProcessor();
            if (!drawer.isChannelAuxRouteSelected(0u)) {
                std::cerr << "[FAIL] Mixer BUS pill should default to AUX\n";
                return false;
            }
            drawer.toggleChannelBusRouteFromMixer(0u);
            if (drawer.isChannelAuxRouteSelected(0u)) {
                std::cerr << "[FAIL] Mixer BUS pill toggle to MAIN failed\n";
                return false;
            }

            buf6.clear();
            proc.processBlock(buf6, midi);
            const float par0 = proc.getBusBuffer(buf6, false, 0).getMagnitude(0, 256);
            const float par1 = proc.getBusBuffer(buf6, false, 1).getMagnitude(0, 256);
            if (par0 <= 0.01f || par1 <= 0.01f) {
                std::cerr << "[FAIL] Parallel MAIN+AUX processBlock failed (bus0=" << par0
                          << ", bus1=" << par1 << ")\n";
                return false;
            }

            juce::MemoryBlock stateBlk;
            proc.getStateInformation(stateBlk);
            proc.setMixerChannelAuxExclusive(0u, true);
            proc.setStateInformation(stateBlk.getData(), static_cast<int>(stateBlk.getSize()));
            if (proc.mixerChannelAuxExclusive(0u)) {
                std::cerr << "[FAIL] Aux routing mask did not restore from state\n";
                return false;
            }
        }

        // Disable Bus 1 -> verify fallback to Bus 0 even with AUX exclusive selected
        proc.setMixerChannelAuxExclusive(0u, true);
        if (!proc.setBusesLayout(makeLayout({ 0, 9 })) || proc.getTotalNumOutputChannels() != 4) {
            std::cerr << "[FAIL] Failed applying subset layout {0, 9}\n";
            return false;
        }
        proc.prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buf4(4, 256);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{ 110 }), 0);
        for (int b = 0; b < 3; ++b) {
            buf4.clear();
            proc.processBlock(buf4, midi);
            midi.clear();
        }
        const float fbBus0 = proc.getBusBuffer(buf4, false, 0).getMagnitude(0, 256);
        if (fbBus0 <= 0.01f) {
            std::cerr << "[FAIL] Multi-Out disabled bus fallback to Bus 0 failed (bus0=" << fbBus0 << ")\n";
            return false;
        }
        proc.releaseResources();
    }
#endif

    std::cout << "[PASS] Multi-Bus Routing & Variable Bus Mask Tests PASSED.\n";
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File cwd = juce::File::getCurrentWorkingDirectory();
    const juce::File buildDir = cwd.getChildFile("build");
    (void)buildDir.createDirectory();

    struct ScopedRomConfigCleanup {
        ~ScopedRomConfigCleanup() noexcept {
            vf50::rom::VF50RomSetBuilder::clearConfiguredPaths();
            gRuntimeTestRomImage.reset();
        }
    } romConfigCleanup;

    if (!runFirmwareFreeAndRuntimeRomLoadingTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runLifecycleAndMemoryLeakStressTest(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runMultiBusRoutingSelfTests()) {
        return EXIT_FAILURE;
    }

    const bool hasExternalRoms = installRuntimeTestRomsForFullSuite();
    if (!hasExternalRoms) {
        std::cout << "[SKIP] External ROM factory-bank test suite skipped (VF50_TEST_ROM_DIR unset).\n";
        if (!runRealFloppyImgAuditSelfTests(buildDir)) {
            return EXIT_FAILURE;
        }
        if (!runUserDiskAndPcKeyboardTextEntryTests(buildDir)) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    if (!runSubsystemSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runStage3ArchitectureSelfTests()) {
        return EXIT_FAILURE;
    }

    const juce::File stage4SnapPng = buildDir.getChildFile("vf50_stage4_vector_panel_2x.png");
    if (!runStage4DspAndVectorGuiSelfTests(stage4SnapPng)) {
        return EXIT_FAILURE;
    }
    if (!runStage5HardwareAndWorkflowSelfTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runPresetCyclingVerificationTest()) {
        return EXIT_FAILURE;
    }
    if (!runAuditSynthesisVerificationTest()) {
        return EXIT_FAILURE;
    }
    if (!runUniversalSysExAndDiskIngestionSelfTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runWorkstationModesMidiExportAndProgrammerSelfTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runVF50DiskA04AndDirDispatcherSelfTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runDecoupledSequencerModeSwitchSelfTest()) {
        return EXIT_FAILURE;
    }
    if (!runLoadedSongAccompanyingRhythmSelfTest()) {
        return EXIT_FAILURE;
    }
    if (!runSequencerManualWorkflowAndMuteSelfTest()) {
        return EXIT_FAILURE;
    }
    if (!runGate1ModernClassicIsolationSelfTest()) {
        return EXIT_FAILURE;
    }
    if (!runGate2ClassicModeWalkthroughSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runGate4AClassicEditAndBufferSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runGate4BClassicRhythmMachineSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runGate6ClassicPhase3AndFullAuditSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runGate7StateRoundTripAndDspStressSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runDiskExtensionAndSubsystemIsolationSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runRealFloppyImgAuditSelfTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runSequencerRhythmClockAndSlotSyncTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runRightClickLatchAndStopBwdTests()) {
        return EXIT_FAILURE;
    }
    if (!runUserDiskAndPcKeyboardTextEntryTests(buildDir)) {
        return EXIT_FAILURE;
    }
    if (!runVisualMixerDrawerSelfTests()) {
        return EXIT_FAILURE;
    }
    if (!runComprehensiveConsoleMixerAndDemoLedTests()) {
        return EXIT_FAILURE;
    }

    const juce::File stage2Wav = (argc > 1)
        ? cwd.getChildFile(juce::String::fromUTF8(argv[1]))
        : cwd.getChildFile("test_render.wav");
    const juce::File stage3AutoWav = (argc > 2)
        ? cwd.getChildFile(juce::String::fromUTF8(argv[2]))
        : stage2Wav.getSiblingFile("test_automation_render.wav");

    if (!renderStage2ShowcaseWav(stage2Wav)) {
        return EXIT_FAILURE;
    }
    if (!renderStage3AutomationSweepWav(stage3AutoWav)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
