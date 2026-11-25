#pragma once

#include "ROM/VF50RomBank.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace vf50::rom {

enum class RomChipId : uint8_t {
    MainCpuA_IC712 = 0, // 131,072 bytes: IC712 XF805 C0.BIN
    MainCpuB_IC723 = 1, // 131,072 bytes: IC723 XF810 C0.BIN
    SubCpu_IC731   = 2, //  32,768 bytes: IC731 XF804 B0.BIN
    DrumWave_IC738 = 3, // 524,288 bytes: XF653A0-101 IC738.BIN
    Count          = 4
};

struct RomChipSpec {
    RomChipId    chipId = RomChipId::MainCpuA_IC712;
    const char*  canonicalFilename = "";
    std::size_t  expectedBytes     = 0u;
    const char*  roleSummary       = "";
};

[[nodiscard]] const RomChipSpec& getRomChipSpec(RomChipId chip) noexcept;

class VF50RomSetBuilder {
public:
    VF50RomSetBuilder() = default;

    void clear() noexcept;

    // Identifies a .BIN/.ROM file by content signature + exact byte size (independent of filename)
    [[nodiscard]] static std::optional<RomChipId> identifyRomBytes(const uint8_t* data, std::size_t sizeBytes) noexcept;

    bool addRomFile(const juce::File& file, juce::String& outStatus, RomChipId* outChip = nullptr);
    int  scanDirectory(const juce::File& directory, juce::String& outStatus);
    void clearChip(RomChipId chip) noexcept;

    [[nodiscard]] bool hasChip(RomChipId chip) const noexcept;
    [[nodiscard]] const juce::File& chipFile(RomChipId chip) const noexcept;
    [[nodiscard]] int loadedRawChipCount() const noexcept;
    [[nodiscard]] bool hasAllFourChips() const noexcept;

    // Decodes whichever loaded ROM chips are present into a complete, thread-safe VF50RomImage
    [[nodiscard]] std::unique_ptr<VF50RomImage> buildRomImage(juce::String& outSummary) const;

    // Dedicated User Auto-Load Folder (Documents/Victory Audio/VF50/ROMs) & Config Persistence
    [[nodiscard]] static juce::File getUserDefaultRomFolder();
    [[nodiscard]] static juce::File getRomPathsConfigFile();
    bool saveConfiguredPaths() const;
    int  loadFromConfigOrUserFolder(juce::String& outStatus);
    static void clearConfiguredPaths();

private:
    std::array<std::vector<uint8_t>, static_cast<std::size_t>(RomChipId::Count)> rawBytes_{};
    std::array<juce::File, static_cast<std::size_t>(RomChipId::Count)>           sourceFiles_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50RomSetBuilder)
};

} // namespace vf50::rom
