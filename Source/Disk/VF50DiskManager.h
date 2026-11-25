#pragma once

#include "DSP/VF50DemoSongPlayer.h"
#include "Plugin/PluginProcessor.h"
#include "State/VF50MemoryManager.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vf50::disk {

enum class DiskSourceKind : uint8_t {
    Unmounted      = 0,
    Fat12Image     = 1,
    LocalDirectory = 2
};

enum class VF50FileKind : uint8_t {
    Unknown    = 0,
    AllData    = 1, // .V01-.V99 (Hardware ALL) / .ALL (Modern)
    VoiceBank  = 2, // .I01-.I99 (Hardware SYN) / .SYN / .VOI / .VCE (Modern)
    PerfBank   = 3, // .PFM / .PRF (Modern Performance Bank)
    RhythmData = 4, // .R01-.R99 (Hardware R.SEQ) / .RHY / .DRM (Modern)
    Sequence   = 5, // .A01-.A99 (Hardware SEQ / QX5FD) / .SEQ / .SNG (Modern)
    SysEx      = 6, // .B01-.B99 (Hardware MDR) / .MDR / .SYX / .MID / .MDI (Modern)
    DiskImage  = 7, // .IMG / .DSK / .FLP / .IMA (FAT12 Floppy Image)
    CardData   = 8  // .C01-.C99 (Hardware CARD) / .CRD (Modern)
};

enum class VF50LoadFilter : uint8_t {
    AutoByExtension = 0,
    AllData         = 1,
    VoiceOnly       = 2,
    PerfOnly        = 3,
    RhythmOnly      = 4,
    SequenceOnly    = 5,
    SynthOnly       = 6, // SYN: Internal voices (I00-I99), performances (PFI00-PFI99) & synth setup ONLY
    CardOnly        = 7  // CARD: Active virtual card bank ONLY
};

enum class VF50ImportOutcome : uint8_t {
    None             = 0,
    MountedDiskImage = 1,
    MountedDirectory = 2,
    SingleVoiceEdit  = 3,
    VoiceBank        = 4,
    PerfEdit         = 5,
    PerfBank         = 6,
    RhythmData       = 7,
    Sequence         = 8,
    AllData          = 9,
    CardData         = 10
};

struct DiskFileEntry {
    std::string name{};
    std::string fullOrRelPath{};
    VF50FileKind kind            = VF50FileKind::Unknown;
    uint32_t    sizeBytes       = 0u;
    uint16_t    firstCluster    = 0u;
    uint32_t    fatDirOffset    = 0u;
    bool        hasFatDirOffset = false;
};

struct Fat12Geometry {
    uint16_t bytesPerSector    = 512u;
    uint8_t  sectorsPerCluster = 2u;
    uint16_t reservedSectors   = 1u;
    uint8_t  numFats           = 2u;
    uint16_t rootEntries       = 112u;
    uint32_t totalSectors      = 1440u;
    uint16_t sectorsPerFat     = 3u;
    uint8_t  mediaDescriptor   = 0xF9u;
    uint32_t rootDirByteOffset = 0u;
    uint32_t rootDirByteSize   = 0u;
    uint32_t dataAreaSector    = 0u;
    uint32_t totalClusters     = 0u;
    std::string volumeLabel    = "NO NAME";
};

class VF50DiskManager {
public:
    static constexpr const char* kUniversalFileFilter =
        "*.v*;*.V*;*.i*;*.I*;*.a*;*.A*;*.r*;*.R*;*.c*;*.C*;*.b*;*.B*;"
        "*.all;*.ALL;*.syn;*.SYN;*.voi;*.VOI;*.pfm;*.PFM;*.rhy;*.RHY;"
        "*.seq;*.SEQ;*.crd;*.CRD;*.mdr;*.MDR;*.syx;*.SYX;*.img;*.IMG;*.dsk;*.DSK;*.mid;*.MID;"
        "*.vce;*.VCE;*.prf;*.PRF;*.drm;*.DRM;*.sng;*.SNG;"
        "*.flp;*.FLP;*.ima;*.IMA;*.mdi;*.MDI";

    VF50DiskManager();

    // =========================================================================
    // Writable Common User Disk Folder (Persistent Default: <Docs>/Victory VF50/User Disk)
    // =========================================================================
    [[nodiscard]] static juce::File defaultUserDiskDirectory();
    [[nodiscard]] static juce::File userDiskConfigFile();
    [[nodiscard]] const juce::File& userDiskDirectory() const noexcept { return userDiskDir_; }
    bool setUserDiskDirectory(const juce::File& dir, bool persistToConfig = true);
    bool mountUserDisk(juce::String& outStatusMessage);
    [[nodiscard]] bool isUserDiskMounted() const noexcept;
    [[nodiscard]] bool isMountedSourceReadOnly() const noexcept { return sourceKind_ == DiskSourceKind::Fat12Image; }
    [[nodiscard]] bool hasExistingFileInSaveTarget(std::string_view baseName, VF50FileKind kind) const;

    // =========================================================================
    // Source Mounting & Inspection
    // =========================================================================
    [[nodiscard]] DiskSourceKind sourceKind() const noexcept { return sourceKind_; }
    [[nodiscard]] VF50ImportOutcome lastImportOutcome() const noexcept { return lastOutcome_; }
    [[nodiscard]] bool isMounted() const noexcept { return sourceKind_ != DiskSourceKind::Unmounted; }
    [[nodiscard]] const juce::String& mountedSourceName() const noexcept { return mountedName_; }
    [[nodiscard]] const juce::File& mountedFileOrFolder() const noexcept { return mountedFile_; }
    [[nodiscard]] const std::vector<DiskFileEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const Fat12Geometry& fat12Geometry() const noexcept { return geometry_; }

    void unmount() noexcept;
    void rescanMountedSource();

    // Mounts a raw 3.5" FAT12 floppy image (.img, .dsk, .flp, .ima - 720KB 2DD / 1.44MB 2HD / custom BPB)
    bool mountFat12ImageFile(const juce::File& imageFile, juce::String& outStatusMessage);
    bool mountFat12ImageMemory(std::vector<uint8_t> rawImageBytes,
                               std::string_view displayName,
                               juce::String& outStatusMessage);

    // Mounts a local directory containing extracted VF50 / SysEx files
    bool mountDirectory(const juce::File& directory, juce::String& outStatusMessage);

    // Reads raw bytes of an enumerated entry from the mounted FAT12 image or local folder
    bool readEntryBytes(std::size_t entryIndex,
                        std::vector<uint8_t>& outBytes,
                        juce::String& outStatusMessage) const;

    // Writes or overwrites an 8.3 file inside the currently mounted FAT12 image (and flushes to disk if file-backed)
    bool writeFileToMountedFat12(std::string_view fileName83,
                                 const uint8_t* data,
                                 std::size_t sizeBytes,
                                 juce::String& outStatusMessage);

    [[nodiscard]] const std::vector<uint8_t>& rawFat12ImageBytes() const noexcept { return imageBytes_; }

    // =========================================================================
    // High-Level Workstation Load / Import / Save Operations
    // =========================================================================
    // Universal file ingestion loader (auto-mounts .img/.dsk/.flp/.ima or imports soundbank/SysEx data)
    bool loadFile(const juce::File& file,
                  vf50::plugin::VF50AudioProcessor& processor,
                  juce::String& outStatusMessage);

    bool loadMountedEntry(std::size_t entryIndex,
                          vf50::plugin::VF50AudioProcessor& processor,
                          VF50LoadFilter filter,
                          juce::String& outStatusMessage);

    bool importExternalFile(const juce::File& file,
                            vf50::plugin::VF50AudioProcessor& processor,
                            VF50LoadFilter filter,
                            juce::String& outStatusMessage);

    bool importBytes(std::string_view fileName,
                     const uint8_t* data,
                     std::size_t sizeBytes,
                     vf50::plugin::VF50AudioProcessor& processor,
                     VF50LoadFilter filter,
                     juce::String& outStatusMessage);

    // Saves complete workstation state (.ALL) into the mounted FAT12 image or mounted folder
    bool saveAllToMountedSource(std::string_view fileName83,
                                vf50::plugin::VF50AudioProcessor& processor,
                                juce::String& outStatusMessage);

    // Classic Mode Disk LCD Workflow Helpers (Zero OS dialogs: FAT12 image & mounted folder)
    void setWriteProtected(bool wp) noexcept { writeProtected_ = wp; }
    [[nodiscard]] bool isWriteProtected() const noexcept { return writeProtected_; }
    bool deleteMountedEntry(std::size_t entryIndex, juce::String& outStatusMessage);
    bool renameMountedEntry(std::size_t entryIndex, std::string_view newNameOrStem, juce::String& outStatusMessage);
    bool saveTypedToMountedSource(std::string_view baseName,
                                  VF50FileKind kind,
                                  vf50::plugin::VF50AudioProcessor& processor,
                                  juce::String& outStatusMessage,
                                  bool* outRoutedToUserDisk = nullptr);
    bool formatMountedDisk(juce::String& outStatusMessage);
    void computeDiskUsageKb(int& outUsedKb, int& outFreeKb) const noexcept;
    void computeSaveTargetUsageKb(int& outUsedKb, int& outFreeKb) const noexcept;

    // Saves complete workstation state (.ALL / .VOI / .PFM / .RHY / .SYX by extension) to an explicit host path
    bool saveToExternalFile(const juce::File& destFile,
                            vf50::plugin::VF50AudioProcessor& processor,
                            juce::String& outStatusMessage);

    // =========================================================================
    // Static Format Builders, Parsers, ASCII Sanitizer & Blank Floppy Formatter
    // =========================================================================
    [[nodiscard]] static juce::String sanitizeAsciiString(const uint8_t* data, int length);
    [[nodiscard]] static VF50FileKind classifyFileKind(std::string_view fileName,
                                                      bool classicHardwareOnly = false) noexcept;
    [[nodiscard]] static bool isHardwareExtensionForKind(std::string_view fileName, VF50FileKind kind) noexcept;
    [[nodiscard]] static std::string_view fileKindShortLabel(VF50FileKind kind) noexcept;
    [[nodiscard]] static uint32_t bytesToKbCeil(uint32_t sizeBytes) noexcept {
        return (sizeBytes == 0u) ? 0u : static_cast<uint32_t>((static_cast<uint64_t>(sizeBytes) + 1023ULL) / 1024ULL);
    }

    [[nodiscard]] static std::vector<uint8_t> createFormattedFat12Image(bool highDensity144MB = false,
                                                                        std::string_view volumeLabel = "VF50_DISK");

    static void buildVoiceBankBytes(const vf50::state::VF50MemoryManager& mem, std::vector<uint8_t>& outBytes);
    static void buildPerfBankBytes(const vf50::state::VF50MemoryManager& mem, std::vector<uint8_t>& outBytes);
    static void buildHardwareSynBytes(const vf50::plugin::VF50AudioProcessor& processor, std::vector<uint8_t>& outBytes);
    static void buildCardBankBytes(const vf50::state::VirtualCardBank& cardBank, std::vector<uint8_t>& outBytes);
    static void buildRhythmDataBytes(const vf50::state::VF50MemoryManager& mem,
                                     std::vector<uint8_t>& outBytes,
                                     const std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs = nullptr);
    static void buildAllDataBytes(const vf50::state::VF50MemoryManager& mem,
                                  std::vector<uint8_t>& outBytes,
                                  const std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs = nullptr,
                                  const vf50::plugin::VF50AudioProcessor* optProcessor = nullptr);
    static void buildVF50SeqV1Bytes(const vf50::dsp::CustomSeqSongBuffer* optSeq,
                                   std::string_view fallbackTitle,
                                   int fallbackTempoBpm,
                                   std::vector<uint8_t>& outBytes);

    [[nodiscard]] static bool parseVF50SeqV1(const uint8_t* data,
                                            std::size_t sizeBytes,
                                            vf50::dsp::CustomSeqSongBuffer& outSeq) noexcept;

    [[nodiscard]] static int parseAllVF50SeqV1Songs(
        const uint8_t* data,
        std::size_t sizeBytes,
        std::array<std::unique_ptr<vf50::dsp::CustomSeqSongBuffer>, 8>& outSlots,
        std::string_view fallbackFileName = {}) noexcept;

    [[nodiscard]] static bool parseQx5fdSequence(const uint8_t* data,
                                                 std::size_t sizeBytes,
                                                 vf50::dsp::CustomSeqSongBuffer& outSeq) noexcept;

    // Recommended VF50LoadFilter for dispatching a specific entry by its classified VF50FileKind
    [[nodiscard]] static VF50LoadFilter defaultLoadFilterForKind(VF50FileKind kind) noexcept;

private:
    [[nodiscard]] bool parseFat12Geometry(juce::String& outError);
    void enumerateFat12Directory(uint32_t dirOffsetBytes,
                                 uint32_t dirSizeBytes,
                                 const std::string& prefix,
                                 int depth);
    [[nodiscard]] uint16_t readFat12Entry(uint16_t cluster) const noexcept;
    void writeFat12Entry(uint16_t cluster, uint16_t value) noexcept;
    [[nodiscard]] bool extractFat12Chain(uint16_t firstCluster,
                                         uint32_t fileSize,
                                         std::vector<uint8_t>& outBytes) const;

    [[nodiscard]] static bool applyVoiceBytes(const uint8_t* data,
                                              std::size_t sizeBytes,
                                              vf50::state::VF50MemoryManager& mem) noexcept;
    [[nodiscard]] static bool applyPerfBytes(const uint8_t* data,
                                             std::size_t sizeBytes,
                                             vf50::state::VF50MemoryManager& mem) noexcept;
    [[nodiscard]] static bool applyRhythmBytes(const uint8_t* data,
                                               std::size_t sizeBytes,
                                               vf50::state::VF50MemoryManager& mem,
                                               std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs = nullptr) noexcept;
    [[nodiscard]] static bool applyCardBankBytes(const uint8_t* data,
                                                 std::size_t sizeBytes,
                                                 vf50::state::VirtualCardBank& dstBank) noexcept;
    [[nodiscard]] static bool applyHardwareVF50FloppyAllBytes(const uint8_t* data,
                                                             std::size_t sizeBytes,
                                                             VF50LoadFilter filter,
                                                             vf50::state::VF50MemoryManager& mem,
                                                             vf50::state::VF50SystemSettings* optSysSettings,
                                                             std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs,
                                                             bool& outLoadedVoices,
                                                             bool& outLoadedPerfs,
                                                             bool& outLoadedRhythm) noexcept;

    DiskSourceKind sourceKind_    = DiskSourceKind::Unmounted;
    VF50ImportOutcome lastOutcome_ = VF50ImportOutcome::None;
    bool writeProtected_          = false;
    juce::File mountedFile_{};
    juce::File userDiskDir_{};
    juce::String mountedName_{ "NO DISK" };
    std::vector<uint8_t> imageBytes_{};
    Fat12Geometry geometry_{};
    std::vector<DiskFileEntry> entries_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VF50DiskManager)
};

} // namespace vf50::disk
