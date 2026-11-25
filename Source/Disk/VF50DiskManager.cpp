#include "Disk/VF50DiskManager.h"
#include "MIDI/VF50SysExHandler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>

namespace vf50::disk {

namespace {

constexpr char kMagicAll[8]  = { 'V', 'F', '5', '0', 'A', 'L', 'V', '1' };
constexpr char kMagicSeq8[8] = { 'V', 'F', '5', '0', 'S', 'Q', '8', '\0' };
constexpr char kMagicVoi[8]  = { 'V', 'F', '5', '0', 'V', 'O', 'V', '1' };
constexpr char kMagicPfm[8]  = { 'V', 'F', '5', '0', 'P', 'F', 'V', '1' };
constexpr char kMagicRhy[8]  = { 'V', 'F', '5', '0', 'R', 'H', 'V', '1' };

[[nodiscard]] inline bool isHwSeqHeaderMagic(const uint8_t* p) noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < 8u; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
    return h == 0xd485378e87ae2802ULL;
}

[[nodiscard]] inline uint16_t readU16LE(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

[[nodiscard]] inline uint32_t readU32LE(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8u)
         | (static_cast<uint32_t>(p[2]) << 16u)
         | (static_cast<uint32_t>(p[3]) << 24u);
}

inline void writeU16LE(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

inline void writeU32LE(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
}

inline void appendU16LE(std::vector<uint8_t>& dst, uint16_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
}

inline void appendU32LE(std::vector<uint8_t>& dst, uint32_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
}

[[nodiscard]] std::string toUpperAscii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] std::string trimRightSpaces(std::string_view sv) {
    std::size_t len = sv.size();
    while (len > 0u && (sv[len - 1u] == ' ' || sv[len - 1u] == '\0')) {
        --len;
    }
    return std::string(sv.substr(0u, len));
}

void formatDos83(std::string_view input, std::array<uint8_t, 11>& out83) noexcept {
    out83.fill(static_cast<uint8_t>(' '));
    const auto dotPos = input.find_last_of('.');
    const std::string_view stem = (dotPos == std::string_view::npos) ? input : input.substr(0u, dotPos);
    const std::string_view ext  = (dotPos == std::string_view::npos) ? std::string_view("ALL") : input.substr(dotPos + 1u);

    std::size_t w = 0u;
    for (std::size_t i = 0u; i < stem.size() && w < 8u; ++i) {
        const unsigned char c = static_cast<unsigned char>(stem[i]);
        if (std::isalnum(c) || c == '_' || c == '-' || c == '!' || c == '\'' || (c == ' ' && w > 0u)) {
            out83[w++] = static_cast<uint8_t>(std::toupper(c));
        }
    }
    while (w > 0u && out83[w - 1u] == static_cast<uint8_t>(' ')) {
        --w;
    }
    if (w == 0u) {
        const char* defStem = "VF50USER";
        for (std::size_t i = 0u; i < 8u; ++i) out83[i] = static_cast<uint8_t>(defStem[i]);
    }

    std::size_t ew = 0u;
    for (std::size_t i = 0u; i < ext.size() && ew < 3u; ++i) {
        const unsigned char c = static_cast<unsigned char>(ext[i]);
        if (std::isalnum(c)) {
            out83[8u + ew] = static_cast<uint8_t>(std::toupper(c));
            ++ew;
        }
    }
}

void decodeTicksPerBarFromVF50Ts(uint8_t tsCode, uint8_t& outNum, uint8_t& outDen, uint16_t& outTpb) noexcept {
    if (tsCode <= 3u) {
        outNum = static_cast<uint8_t>(tsCode + 1u);
        outDen = 4u;
        outTpb = static_cast<uint16_t>(outNum * 48u);
    } else if (tsCode <= 11u) {
        outNum = static_cast<uint8_t>(tsCode - 3u);
        outDen = 8u;
        outTpb = static_cast<uint16_t>(outNum * 24u);
    } else if (tsCode <= 27u) {
        outNum = static_cast<uint8_t>(tsCode - 11u);
        outDen = 16u;
        outTpb = static_cast<uint16_t>(outNum * 12u);
    } else {
        outNum = 4u;
        outDen = 4u;
        outTpb = 192u;
    }
}

[[nodiscard]] uint8_t encodeVF50TsFromTimeSig(uint8_t num, uint8_t den) noexcept {
    if (den == 4u && num >= 1u && num <= 4u) return static_cast<uint8_t>(num - 1u);
    if (den == 8u && num >= 1u && num <= 8u) return static_cast<uint8_t>(num + 3u);
    if (den == 16u && num >= 1u && num <= 16u) return static_cast<uint8_t>(num + 11u);
    return 3u; // 4/4 fallback
}

[[nodiscard]] std::string sanitizeDos83Stem(std::string_view input, std::string_view fallback = "NEWFILE") {
    std::string out;
    out.reserve(8u);
    for (std::size_t i = 0u; i < input.size() && out.size() < 8u; ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            continue;
        }
        if (std::isalnum(c) || c == '_' || c == '-' || c == '!' || c == '$' || c == '#'
            || c == '\'' || (c == ' ' && !out.empty())) {
            out.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.empty()) {
        out.assign(fallback.substr(0u, 8u));
    }
    return out;
}

[[nodiscard]] char hardwareExtPrefixForKind(VF50FileKind kind) noexcept {
    if (kind == VF50FileKind::VoiceBank || kind == VF50FileKind::PerfBank) return 'I';
    if (kind == VF50FileKind::Sequence) return 'A';
    if (kind == VF50FileKind::RhythmData) return 'R';
    if (kind == VF50FileKind::CardData) return 'C';
    if (kind == VF50FileKind::SysEx) return 'B';
    return 'V';
}

[[nodiscard]] bool hasKnownVF50BinaryMagic(const uint8_t* data, std::size_t sizeBytes) noexcept {
    if (data == nullptr || sizeBytes < 8u) {
        return false;
    }
    return std::memcmp(data, kMagicAll, 8u) == 0
        || std::memcmp(data, kMagicVoi, 8u) == 0
        || std::memcmp(data, kMagicPfm, 8u) == 0
        || std::memcmp(data, kMagicRhy, 8u) == 0
        || isHwSeqHeaderMagic(data)
        || std::memcmp(data, "VF5M", 4u) == 0;
}

[[nodiscard]] bool hasSysExStartByte(const uint8_t* data, std::size_t sizeBytes, bool scanEntireBuffer = true) noexcept {
    if (data == nullptr || sizeBytes < 6u || hasKnownVF50BinaryMagic(data, sizeBytes)) {
        return false;
    }
    const std::size_t limit = scanEntireBuffer ? (sizeBytes - 5u) : std::min<std::size_t>(sizeBytes - 5u, 512u);
    for (std::size_t i = 0u; i < limit; ++i) {
        if (data[i] == 0xF0u && data[i + 1u] == 0x43u) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] uint32_t readU32BE(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24u)
         | (static_cast<uint32_t>(p[1]) << 16u)
         | (static_cast<uint32_t>(p[2]) << 8u)
         | static_cast<uint32_t>(p[3]);
}

[[nodiscard]] bool readSmfVlq(const uint8_t* buf, std::size_t endPos, std::size_t& ioPos, uint32_t& outVal) noexcept {
    outVal = 0u;
    for (int i = 0; i < 4; ++i) {
        if (ioPos >= endPos) {
            return false;
        }
        const uint8_t b = buf[ioPos++];
        outVal = (outVal << 7u) | static_cast<uint32_t>(b & 0x7Fu);
        if ((b & 0x80u) == 0u) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool extractSysExFromSmf(const uint8_t* data,
                                       std::size_t sizeBytes,
                                       std::vector<uint8_t>& outSysEx) {
    outSysEx.clear();
    if (data == nullptr || sizeBytes < 14u || std::memcmp(data, "MThd", 4u) != 0) {
        return false;
    }

    const uint32_t hdrLen = readU32BE(data + 4u);
    std::size_t pos = 8u + static_cast<std::size_t>(hdrLen);
    if (hdrLen < 6u || pos > sizeBytes) {
        return false;
    }

    while (pos + 8u <= sizeBytes) {
        const bool isMTrk     = (std::memcmp(data + pos, "MTrk", 4u) == 0);
        const uint32_t chkLen = readU32BE(data + pos + 4u);
        pos += 8u;
        const std::size_t trkEnd = std::min<std::size_t>(sizeBytes, pos + static_cast<std::size_t>(chkLen));
        if (isMTrk) {
            std::size_t tp = pos;
            uint8_t runningStatus = 0u;
            while (tp < trkEnd) {
                uint32_t delta = 0u;
                if (!readSmfVlq(data, trkEnd, tp, delta) || tp >= trkEnd) {
                    break;
                }
                const uint8_t statusByte = data[tp];
                if (statusByte == 0xF0u || statusByte == 0xF7u) {
                    ++tp;
                    runningStatus = 0u;
                    uint32_t sxLen = 0u;
                    if (!readSmfVlq(data, trkEnd, tp, sxLen) || tp + sxLen > trkEnd) {
                        break;
                    }
                    if (statusByte == 0xF0u) {
                        outSysEx.push_back(0xF0u);
                    }
                    if (sxLen > 0u) {
                        outSysEx.insert(outSysEx.end(), data + tp, data + tp + sxLen);
                    }
                    tp += sxLen;
                } else if (statusByte == 0xFFu) {
                    ++tp;
                    runningStatus = 0u;
                    if (tp >= trkEnd) {
                        break;
                    }
                    ++tp; // Meta event type
                    uint32_t metaLen = 0u;
                    if (!readSmfVlq(data, trkEnd, tp, metaLen) || tp + metaLen > trkEnd) {
                        break;
                    }
                    tp += metaLen;
                } else if ((statusByte & 0x80u) != 0u) {
                    ++tp;
                    if (statusByte < 0xF0u) {
                        runningStatus = statusByte;
                        const uint8_t hi = statusByte & 0xF0u;
                        const std::size_t dBytes = (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
                        if (tp + dBytes > trkEnd) break;
                        tp += dBytes;
                    } else if (statusByte == 0xF2u) {
                        if (tp + 2u > trkEnd) break;
                        tp += 2u;
                    } else if (statusByte == 0xF1u || statusByte == 0xF3u) {
                        if (tp + 1u > trkEnd) break;
                        tp += 1u;
                    }
                } else {
                    if (runningStatus == 0u) {
                        break;
                    }
                    const uint8_t hi = runningStatus & 0xF0u;
                    const std::size_t dBytes = (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
                    if (tp + dBytes > trkEnd) break;
                    tp += dBytes;
                }
            }
        }
        pos = trkEnd;
    }

    return hasSysExStartByte(outSysEx.data(), outSysEx.size(), true);
}

} // namespace

// =============================================================================
// File Kind Classification
// =============================================================================

// =============================================================================
// Safe 8-Bit ASCII String Sanitizer & File Extension Classification
// =============================================================================

juce::String VF50DiskManager::sanitizeAsciiString(const uint8_t* data, int length) {
    if (data == nullptr || length <= 0) {
        return {};
    }
    juce::MemoryOutputStream mos;
    for (int i = 0; i < length; ++i) {
        const uint8_t raw = data[i];
        if (raw == 0u) {
            break;
        }
        if (raw >= 32u && raw <= 126u) {
            mos.writeByte(static_cast<char>(raw));
        } else {
            mos.writeByte(' ');
        }
    }
    return mos.toString().trim();
}

VF50FileKind VF50DiskManager::classifyFileKind(std::string_view fileName,
                                             bool classicHardwareOnly) noexcept {
    const auto dotPos = fileName.find_last_of('.');
    if (dotPos == std::string_view::npos) {
        return VF50FileKind::Unknown;
    }
    const std::string ext = toUpperAscii(fileName.substr(dotPos + 1u));
    const bool isHwNum01To99 = (ext.size() == 3u
        && std::isdigit(static_cast<unsigned char>(ext[1]))
        && std::isdigit(static_cast<unsigned char>(ext[2]))
        && !(ext[1] == '0' && ext[2] == '0'));

    // Official VF50 hardware extension table (IC712 Bank 0 $BBAF = "VIARCB"):
    //   ALL=.V01-.V99, SYN=.I01-.I99, SEQ=.A01-.A99, R.SEQ=.R01-.R99, CARD=.C01-.C99, MDR=.B01-.B99
    if (isHwNum01To99) {
        switch (ext[0]) {
            case 'V': return VF50FileKind::AllData;
            case 'I': return VF50FileKind::VoiceBank;
            case 'A': return VF50FileKind::Sequence;
            case 'R': return VF50FileKind::RhythmData;
            case 'C': return VF50FileKind::CardData;
            case 'B': return VF50FileKind::SysEx;
            default:  return VF50FileKind::Unknown;
        }
    }

    if (classicHardwareOnly) {
        return VF50FileKind::Unknown;
    }

    // Modern Mode fallback extensions (unrecognized extensions such as MS-DOS ._DD are strictly rejected)
    if (ext == "ALL") {
        return VF50FileKind::AllData;
    }
    if (ext == "SYN" || ext == "VOI" || ext == "VCE") {
        return VF50FileKind::VoiceBank;
    }
    if (ext == "PFM" || ext == "PRF") {
        return VF50FileKind::PerfBank;
    }
    if (ext == "RHY" || ext == "DRM") {
        return VF50FileKind::RhythmData;
    }
    if (ext == "SEQ" || ext == "SNG") {
        return VF50FileKind::Sequence;
    }
    if (ext == "CRD") {
        return VF50FileKind::CardData;
    }
    if (ext == "MDR" || ext == "SYX" || ext == "MID" || ext == "MDI") {
        return VF50FileKind::SysEx;
    }
    if (ext == "IMG" || ext == "DSK" || ext == "FLP" || ext == "IMA") {
        return VF50FileKind::DiskImage;
    }
    return VF50FileKind::Unknown;
}

bool VF50DiskManager::isHardwareExtensionForKind(std::string_view fileName, VF50FileKind kind) noexcept {
    return kind != VF50FileKind::Unknown && classifyFileKind(fileName, true) == kind;
}

VF50LoadFilter VF50DiskManager::defaultLoadFilterForKind(VF50FileKind kind) noexcept {
    switch (kind) {
        case VF50FileKind::AllData:    return VF50LoadFilter::AllData;
        case VF50FileKind::VoiceBank:  return VF50LoadFilter::SynthOnly;
        case VF50FileKind::PerfBank:   return VF50LoadFilter::PerfOnly;
        case VF50FileKind::RhythmData: return VF50LoadFilter::RhythmOnly;
        case VF50FileKind::Sequence:   return VF50LoadFilter::SequenceOnly;
        case VF50FileKind::CardData:   return VF50LoadFilter::CardOnly;
        case VF50FileKind::SysEx:      return VF50LoadFilter::AutoByExtension;
        case VF50FileKind::DiskImage:  return VF50LoadFilter::AutoByExtension;
        case VF50FileKind::Unknown:    return VF50LoadFilter::AutoByExtension;
    }
    return VF50LoadFilter::AutoByExtension;
}

std::string_view VF50DiskManager::fileKindShortLabel(VF50FileKind kind) noexcept {
    switch (kind) {
        case VF50FileKind::AllData:    return "ALL";
        case VF50FileKind::VoiceBank:  return "SYN";
        case VF50FileKind::PerfBank:   return "PFM";
        case VF50FileKind::RhythmData: return "RHY";
        case VF50FileKind::Sequence:   return "SEQ";
        case VF50FileKind::CardData:   return "CRD";
        case VF50FileKind::SysEx:      return "SYX";
        case VF50FileKind::DiskImage:  return "IMG";
        case VF50FileKind::Unknown:    return "BIN";
    }
    return "BIN";
}

// =============================================================================
// Blank FAT12 Floppy Formatter (720KB 2DD / 1.44MB 2HD)
// =============================================================================

std::vector<uint8_t> VF50DiskManager::createFormattedFat12Image(bool highDensity144MB,
                                                               std::string_view volumeLabel) {
    const uint16_t bytesPerSector    = 512u;
    const uint8_t  sectorsPerCluster = highDensity144MB ? 1u : 2u;
    const uint16_t reservedSectors   = 1u;
    const uint8_t  numFats           = 2u;
    const uint16_t rootEntries       = highDensity144MB ? 224u : 112u;
    const uint16_t totalSectors16    = highDensity144MB ? 2880u : 1440u;
    const uint8_t  mediaDesc         = highDensity144MB ? 0xF0u : 0xF9u;
    const uint16_t sectorsPerFat     = highDensity144MB ? 9u : 3u;
    const uint16_t sectorsPerTrack   = highDensity144MB ? 18u : 9u;
    const uint16_t numHeads          = 2u;

    std::vector<uint8_t> img(static_cast<std::size_t>(totalSectors16) * bytesPerSector, 0u);

    // Boot sector + BIOS Parameter Block (BPB)
    img[0] = 0xEBu;
    img[1] = 0x3Cu;
    img[2] = 0x90u;
    std::memcpy(&img[3], "VF50_DSK", 8u);
    writeU16LE(&img[11], bytesPerSector);
    img[13] = sectorsPerCluster;
    writeU16LE(&img[14], reservedSectors);
    img[16] = numFats;
    writeU16LE(&img[17], rootEntries);
    writeU16LE(&img[19], totalSectors16);
    img[21] = mediaDesc;
    writeU16LE(&img[22], sectorsPerFat);
    writeU16LE(&img[24], sectorsPerTrack);
    writeU16LE(&img[26], numHeads);
    writeU32LE(&img[28], 0u);
    writeU32LE(&img[32], 0u);
    img[36] = 0x00u;
    img[38] = 0x29u; // Extended boot signature
    writeU32LE(&img[39], 0x19890550u); // Volume serial

    std::array<uint8_t, 11> labelBuf{};
    labelBuf.fill(static_cast<uint8_t>(' '));
    const std::size_t copyLen = std::min<std::size_t>(volumeLabel.size(), 11u);
    for (std::size_t i = 0u; i < copyLen; ++i) {
        labelBuf[i] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(volumeLabel[i])));
    }
    std::memcpy(&img[43], labelBuf.data(), 11u);
    std::memcpy(&img[54], "FAT12   ", 8u);
    img[510] = 0x55u;
    img[511] = 0xAAu;

    // Initialize FAT1 & FAT2 media descriptor headers
    for (uint8_t f = 0u; f < numFats; ++f) {
        const std::size_t fatOff = static_cast<std::size_t>(reservedSectors + f * sectorsPerFat) * bytesPerSector;
        img[fatOff + 0u] = mediaDesc;
        img[fatOff + 1u] = 0xFFu;
        img[fatOff + 2u] = 0xFFu;
    }

    return img;
}

// =============================================================================
// Persistent Common User Disk Folder (<UserDocuments>/Victory VF50/User Disk)
// =============================================================================

juce::File VF50DiskManager::defaultUserDiskDirectory() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Victory VF50")
        .getChildFile("User Disk");
}

juce::File VF50DiskManager::userDiskConfigFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Victory Audio")
        .getChildFile("VF50")
        .getChildFile("vf50_user_disk.cfg");
}

VF50DiskManager::VF50DiskManager() {
    userDiskDir_ = defaultUserDiskDirectory();
    const juce::File cfg = userDiskConfigFile();
    if (cfg.existsAsFile()) {
        juce::StringArray lines;
        cfg.readLines(lines);
        for (const auto& line : lines) {
            if (line.startsWith("USER_DISK_DIR=")) {
                const juce::String p = line.fromFirstOccurrenceOf("USER_DISK_DIR=", false, false).trim();
                if (p.isNotEmpty()) {
                    const juce::File cand(p);
                    if (cand.getFullPathName().isNotEmpty()) {
                        userDiskDir_ = cand;
                    }
                }
                break;
            }
        }
    }
    if (!userDiskDir_.isDirectory()) {
        (void)userDiskDir_.createDirectory();
    }
}

bool VF50DiskManager::setUserDiskDirectory(const juce::File& dir, bool persistToConfig) {
    if (dir == juce::File{}) {
        return false;
    }
    if (!dir.isDirectory() && !dir.createDirectory()) {
        return false;
    }
    const bool wasUserDiskMounted = isUserDiskMounted();
    userDiskDir_ = dir;
    if (persistToConfig) {
        const juce::File cfg = userDiskConfigFile();
        (void)cfg.getParentDirectory().createDirectory();
        (void)cfg.replaceWithText("USER_DISK_DIR=" + userDiskDir_.getFullPathName() + "\n");
    }
    if (wasUserDiskMounted) {
        juce::String dummy;
        (void)mountUserDisk(dummy);
    }
    return true;
}

bool VF50DiskManager::mountUserDisk(juce::String& outStatusMessage) {
    if (userDiskDir_ == juce::File{}) {
        userDiskDir_ = defaultUserDiskDirectory();
    }
    if (!userDiskDir_.isDirectory() && !userDiskDir_.createDirectory()) {
        outStatusMessage = "Cannot create User Disk";
        return false;
    }
    if (!mountDirectory(userDiskDir_, outStatusMessage)) {
        return false;
    }
    mountedName_ = "USER DISK";
    outStatusMessage = "User Disk (" + juce::String(entries_.size()) + " files)";
    return true;
}

bool VF50DiskManager::isUserDiskMounted() const noexcept {
    return sourceKind_ == DiskSourceKind::LocalDirectory
        && userDiskDir_ != juce::File{}
        && mountedFile_ == userDiskDir_;
}

bool VF50DiskManager::hasExistingFileInSaveTarget(std::string_view baseName, VF50FileKind kind) const {
    const juce::File targetDir = (sourceKind_ == DiskSourceKind::LocalDirectory && mountedFile_.isDirectory())
                                     ? mountedFile_
                                     : userDiskDir_;
    if (!targetDir.isDirectory()) {
        return false;
    }
    std::string raw(baseName);
    const auto dotPos = raw.find_last_of('.');
    const std::string stemPart = (dotPos == std::string::npos) ? raw : raw.substr(0u, dotPos);
    const std::string cleanStem = sanitizeDos83Stem(stemPart, "NEWFILE");
    if (dotPos != std::string::npos) {
        const std::string ext = toUpperAscii(raw.substr(dotPos + 1u));
        if (isHardwareExtensionForKind("X." + ext, kind)) {
            return targetDir.getChildFile(juce::String(cleanStem + "." + ext)).existsAsFile();
        }
    }
    // Without explicit .XX suffix, auto-allocation assigns next free .01-.99 unless user matches an existing slot.
    return false;
}

// =============================================================================
// Mounting & FAT12 Core Engine
// =============================================================================

void VF50DiskManager::unmount() noexcept {
    sourceKind_ = DiskSourceKind::Unmounted;
    mountedFile_ = juce::File{};
    mountedName_ = "NO DISK";
    imageBytes_.clear();
    entries_.clear();
    geometry_ = {};
}

bool VF50DiskManager::mountFat12ImageFile(const juce::File& imageFile, juce::String& outStatusMessage) {
    if (!imageFile.existsAsFile()) {
        outStatusMessage = "Disk image not found";
        return false;
    }

    juce::MemoryBlock mb;
    if (!imageFile.loadFileAsData(mb) || mb.getSize() < 4096u) {
        outStatusMessage = "Cannot read disk image";
        return false;
    }

    std::vector<uint8_t> raw(mb.getSize());
    std::memcpy(raw.data(), mb.getData(), mb.getSize());

    if (!mountFat12ImageMemory(std::move(raw), imageFile.getFileName().toStdString(), outStatusMessage)) {
        return false;
    }

    mountedFile_ = imageFile;
    return true;
}

bool VF50DiskManager::mountFat12ImageMemory(std::vector<uint8_t> rawImageBytes,
                                           std::string_view displayName,
                                           juce::String& outStatusMessage) {
    imageBytes_ = std::move(rawImageBytes);
    entries_.clear();

    juce::String err;
    if (!parseFat12Geometry(err)) {
        unmount();
        outStatusMessage = err;
        return false;
    }

    sourceKind_  = DiskSourceKind::Fat12Image;
    lastOutcome_ = VF50ImportOutcome::MountedDiskImage;
    mountedFile_ = juce::File{};
    mountedName_ = sanitizeAsciiString(reinterpret_cast<const uint8_t*>(displayName.data()),
                                       static_cast<int>(displayName.size()));
    if (mountedName_.isEmpty()) {
        mountedName_ = "FLOPPY.IMG";
    }

    enumerateFat12Directory(geometry_.rootDirByteOffset, geometry_.rootDirByteSize, "", 0);

    const bool is2HD = (imageBytes_.size() >= 1200000u);
    outStatusMessage = juce::String("Mounted ") + (is2HD ? "2HD " : "2DD ")
                     + mountedName_ + " (" + juce::String(entries_.size()) + " files)";
    return true;
}

bool VF50DiskManager::mountDirectory(const juce::File& directory, juce::String& outStatusMessage) {
    if (!directory.isDirectory()) {
        outStatusMessage = "Invalid directory";
        return false;
    }

    imageBytes_.clear();
    geometry_    = {};
    sourceKind_  = DiskSourceKind::LocalDirectory;
    lastOutcome_ = VF50ImportOutcome::MountedDirectory;
    mountedFile_ = directory;
    mountedName_ = (userDiskDir_ != juce::File{} && directory == userDiskDir_)
                       ? juce::String("USER DISK")
                       : directory.getFileName();
    rescanMountedSource();

    outStatusMessage = "Folder: " + mountedName_ + " (" + juce::String(entries_.size()) + " files)";
    return true;
}

void VF50DiskManager::rescanMountedSource() {
    entries_.clear();
    if (sourceKind_ == DiskSourceKind::Fat12Image) {
        enumerateFat12Directory(geometry_.rootDirByteOffset, geometry_.rootDirByteSize, "", 0);
    } else if (sourceKind_ == DiskSourceKind::LocalDirectory && mountedFile_.isDirectory()) {
        auto children = mountedFile_.findChildFiles(juce::File::findFiles, false, kUniversalFileFilter);
        children.sort();
        for (const auto& f : children) {
            const std::string fname = f.getFileName().toStdString();
            const VF50FileKind kind  = classifyFileKind(fname);
            if (kind != VF50FileKind::Unknown) {
                DiskFileEntry e{};
                e.name          = fname;
                e.fullOrRelPath = f.getFullPathName().toStdString();
                e.kind          = kind;
                e.sizeBytes     = static_cast<uint32_t>(std::max<juce::int64>(0, f.getSize()));
                entries_.push_back(std::move(e));
            }
        }
    }
}

bool VF50DiskManager::parseFat12Geometry(juce::String& outError) {
    const std::size_t imgSize = imageBytes_.size();
    if (imgSize < 4096u) {
        outError = "Image too small for FAT12";
        return false;
    }

    Fat12Geometry g{};
    bool validBpb = false;

    const uint16_t bpbSectorSize = readU16LE(&imageBytes_[11]);
    const uint8_t  bpbSecPerClus = imageBytes_[13];
    const uint16_t bpbReserved   = readU16LE(&imageBytes_[14]);
    const uint8_t  bpbNumFats    = imageBytes_[16];
    const uint16_t bpbRootEnts   = readU16LE(&imageBytes_[17]);
    const uint16_t bpbTotSec16   = readU16LE(&imageBytes_[19]);
    const uint32_t bpbTotSec32   = readU32LE(&imageBytes_[32]);
    const uint16_t bpbSecPerFat  = readU16LE(&imageBytes_[22]);

    const bool powerOfTwoClus = (bpbSecPerClus != 0u) && ((bpbSecPerClus & (bpbSecPerClus - 1u)) == 0u);
    const uint32_t totSec     = (bpbTotSec16 != 0u) ? static_cast<uint32_t>(bpbTotSec16) : bpbTotSec32;

    if ((bpbSectorSize == 512u || bpbSectorSize == 1024u)
        && powerOfTwoClus
        && bpbReserved >= 1u && bpbReserved <= 32u
        && bpbNumFats >= 1u && bpbNumFats <= 4u
        && bpbRootEnts >= 16u && bpbRootEnts <= 1024u
        && bpbSecPerFat >= 1u && bpbSecPerFat <= 64u
        && totSec >= 64u
        && static_cast<std::size_t>(totSec) * bpbSectorSize <= imgSize + bpbSectorSize) {
        g.bytesPerSector    = bpbSectorSize;
        g.sectorsPerCluster = bpbSecPerClus;
        g.reservedSectors   = bpbReserved;
        g.numFats           = bpbNumFats;
        g.rootEntries       = bpbRootEnts;
        g.totalSectors      = totSec;
        g.sectorsPerFat     = bpbSecPerFat;
        g.mediaDescriptor   = imageBytes_[21];
        if (imageBytes_[38] == 0x29u) {
            const juce::String cleanLabel = sanitizeAsciiString(&imageBytes_[43], 11);
            g.volumeLabel = cleanLabel.isNotEmpty() ? cleanLabel.toStdString() : std::string("NO NAME");
        }
        validBpb = true;
    }

    // Fallback for raw headerless Vintage / Atari / PC98 / DOS 2DD & 2HD floppy dumps
    if (!validBpb) {
        if (imgSize == 737280u) { // Standard 3.5" 720KB 2DD (VF50 native format)
            g.bytesPerSector    = 512u;
            g.sectorsPerCluster = 2u;
            g.reservedSectors   = 1u;
            g.numFats           = 2u;
            g.rootEntries       = 112u;
            g.totalSectors      = 1440u;
            g.sectorsPerFat     = 3u;
            g.mediaDescriptor   = 0xF9u;
            validBpb            = true;
        } else if (imgSize == 1474560u) { // Standard 3.5" 1.44MB 2HD
            g.bytesPerSector    = 512u;
            g.sectorsPerCluster = 1u;
            g.reservedSectors   = 1u;
            g.numFats           = 2u;
            g.rootEntries       = 224u;
            g.totalSectors      = 2880u;
            g.sectorsPerFat     = 9u;
            g.mediaDescriptor   = 0xF0u;
            validBpb            = true;
        } else if (imgSize == 368640u) { // 360KB 2D
            g.bytesPerSector    = 512u;
            g.sectorsPerCluster = 2u;
            g.reservedSectors   = 1u;
            g.numFats           = 2u;
            g.rootEntries       = 112u;
            g.totalSectors      = 720u;
            g.sectorsPerFat     = 2u;
            g.mediaDescriptor   = 0xFDu;
            validBpb            = true;
        } else if (imgSize == 1228800u) { // 1.2MB 2HD (PC-98 / AT)
            g.bytesPerSector    = 512u;
            g.sectorsPerCluster = 1u;
            g.reservedSectors   = 1u;
            g.numFats           = 2u;
            g.rootEntries       = 224u;
            g.totalSectors      = 2400u;
            g.sectorsPerFat     = 7u;
            g.mediaDescriptor   = 0xF9u;
            validBpb            = true;
        } else if (imgSize >= 0x2000u
                   && imageBytes_[0x0200] == 0xF9u && imageBytes_[0x0201] == 0xFFu && imageBytes_[0x0202] == 0xFFu
                   && imageBytes_[0x0800] == 0xF9u && imageBytes_[0x0801] == 0xFFu && imageBytes_[0x0802] == 0xFFu) {
            // Authentic headerless VF50 3.5" 2DD floppy format (Sector 0 filled with 0xE5, FAT1 @ 0x0200, FAT2 @ 0x0800)
            g.bytesPerSector    = 512u;
            g.sectorsPerCluster = 2u;
            g.reservedSectors   = 1u;
            g.numFats           = 2u;
            g.rootEntries       = 112u;
            g.totalSectors      = 1440u;
            g.sectorsPerFat     = 3u;
            g.mediaDescriptor   = 0xF9u;
            g.volumeLabel       = "VF50_DISK";
            validBpb            = true;
        }
    }

    if (!validBpb) {
        outError = "Unrecognized FAT12 geometry";
        return false;
    }

    const uint32_t rootDirSectors = ((static_cast<uint32_t>(g.rootEntries) * 32u) + (g.bytesPerSector - 1u))
                                  / g.bytesPerSector;
    const uint32_t firstRootSector = static_cast<uint32_t>(g.reservedSectors)
                                   + static_cast<uint32_t>(g.numFats) * static_cast<uint32_t>(g.sectorsPerFat);
    g.rootDirByteOffset = firstRootSector * g.bytesPerSector;
    g.rootDirByteSize   = static_cast<uint32_t>(g.rootEntries) * 32u;
    g.dataAreaSector    = firstRootSector + rootDirSectors;

    if (g.dataAreaSector >= g.totalSectors
        || static_cast<std::size_t>(g.rootDirByteOffset + g.rootDirByteSize) > imgSize) {
        outError = "Corrupt FAT12 directory bounds";
        return false;
    }

    const uint32_t dataSectors = g.totalSectors - g.dataAreaSector;
    g.totalClusters = dataSectors / g.sectorsPerCluster;
    if (g.totalClusters == 0u || g.totalClusters > 4085u) {
        outError = "Not a FAT12 volume";
        return false;
    }

    geometry_ = g;
    return true;
}

uint16_t VF50DiskManager::readFat12Entry(uint16_t cluster) const noexcept {
    const std::size_t fatBase = static_cast<std::size_t>(geometry_.reservedSectors) * geometry_.bytesPerSector;
    const std::size_t off     = fatBase + static_cast<std::size_t>(cluster) + (static_cast<std::size_t>(cluster) / 2u);
    if (off + 1u >= imageBytes_.size()) {
        return 0xFFFu;
    }
    const uint16_t pair = readU16LE(&imageBytes_[off]);
    return ((cluster & 1u) != 0u) ? static_cast<uint16_t>((pair >> 4u) & 0x0FFFu)
                                  : static_cast<uint16_t>(pair & 0x0FFFu);
}

void VF50DiskManager::writeFat12Entry(uint16_t cluster, uint16_t value) noexcept {
    const uint16_t cleanVal = static_cast<uint16_t>(value & 0x0FFFu);
    for (uint8_t f = 0u; f < geometry_.numFats; ++f) {
        const std::size_t fatBase = static_cast<std::size_t>(
            geometry_.reservedSectors + f * geometry_.sectorsPerFat) * geometry_.bytesPerSector;
        const std::size_t off = fatBase + static_cast<std::size_t>(cluster) + (static_cast<std::size_t>(cluster) / 2u);
        if (off + 1u >= imageBytes_.size()) {
            continue;
        }
        uint16_t pair = readU16LE(&imageBytes_[off]);
        if ((cluster & 1u) != 0u) {
            pair = static_cast<uint16_t>((pair & 0x000Fu) | (cleanVal << 4u));
        } else {
            pair = static_cast<uint16_t>((pair & 0xF000u) | cleanVal);
        }
        writeU16LE(&imageBytes_[off], pair);
    }
}

bool VF50DiskManager::extractFat12Chain(uint16_t firstCluster,
                                       uint32_t fileSize,
                                       std::vector<uint8_t>& outBytes) const {
    outBytes.clear();
    if (fileSize == 0u) {
        return true;
    }
    if (firstCluster < 2u) {
        return false;
    }

    outBytes.reserve(fileSize);
    const uint32_t clusterBytes = static_cast<uint32_t>(geometry_.bytesPerSector) * geometry_.sectorsPerCluster;
    uint16_t curCluster         = firstCluster;
    uint32_t safetySteps        = 0u;

    while (outBytes.size() < fileSize && curCluster >= 2u && curCluster < 0xFF7u) {
        if (++safetySteps > (geometry_.totalClusters + 2u)) {
            return false;
        }
        const uint32_t sectorIdx = geometry_.dataAreaSector
                                 + static_cast<uint32_t>(curCluster - 2u) * geometry_.sectorsPerCluster;
        const std::size_t byteOff = static_cast<std::size_t>(sectorIdx) * geometry_.bytesPerSector;
        if (byteOff >= imageBytes_.size()) {
            const std::size_t fullDiskBytes = static_cast<std::size_t>(geometry_.totalSectors) * geometry_.bytesPerSector;
            if (imageBytes_.size() < fullDiskBytes && outBytes.size() >= 1024u) {
                break; // Truncated archival image dump: return recovered prefix clusters
            }
            return false;
        }

        const std::size_t availInImg = imageBytes_.size() - byteOff;
        const std::size_t remaining  = static_cast<std::size_t>(fileSize) - outBytes.size();
        const std::size_t toCopy     = std::min<std::size_t>({ clusterBytes, availInImg, remaining });
        outBytes.insert(outBytes.end(), imageBytes_.begin() + static_cast<std::ptrdiff_t>(byteOff),
                        imageBytes_.begin() + static_cast<std::ptrdiff_t>(byteOff + toCopy));

        curCluster = readFat12Entry(curCluster);
    }

    const std::size_t fullDiskBytes = static_cast<std::size_t>(geometry_.totalSectors) * geometry_.bytesPerSector;
    if (imageBytes_.size() < fullDiskBytes && outBytes.size() >= 1024u) {
        return true;
    }
    return outBytes.size() == fileSize;
}

void VF50DiskManager::enumerateFat12Directory(uint32_t dirOffsetBytes,
                                             uint32_t dirSizeBytes,
                                             const std::string& prefix,
                                             int depth) {
    if (depth > 4 || static_cast<std::size_t>(dirOffsetBytes + dirSizeBytes) > imageBytes_.size()) {
        return;
    }

    for (uint32_t off = 0u; off + 32u <= dirSizeBytes; off += 32u) {
        const uint8_t* ent = &imageBytes_[dirOffsetBytes + off];
        const uint8_t first = ent[0];
        if (first == 0x00u) {
            break; // End of directory
        }
        if (first == 0xE5u) {
            continue; // Deleted entry
        }
        const uint8_t attr = ent[11];
        if (attr == 0x0Fu) {
            continue; // VFAT long filename entry
        }
        if ((attr & 0x08u) != 0u) {
            // Volume label entry
            if (depth == 0) {
                const juce::String cleanVol = sanitizeAsciiString(ent, 11);
                geometry_.volumeLabel = cleanVol.isNotEmpty() ? cleanVol.toStdString() : std::string("NO NAME");
            }
            continue;
        }

        std::array<uint8_t, 8> rawStem{};
        std::memcpy(rawStem.data(), ent, 8u);
        if (rawStem[0] == 0x05u) {
            rawStem[0] = static_cast<uint8_t>('_');
        }
        const std::string stem = sanitizeAsciiString(rawStem.data(), 8).toStdString();
        const std::string ext  = sanitizeAsciiString(ent + 8u, 3).toStdString();
        if (stem == "." || stem == ".." || stem.empty()) {
            continue;
        }

        const std::string baseName = ext.empty() ? stem : (stem + "." + ext);
        const std::string relName  = prefix.empty() ? baseName : (prefix + "/" + baseName);
        const uint16_t startClus   = readU16LE(&ent[26]);
        const uint32_t fileSize    = readU32LE(&ent[28]);

        if ((attr & 0x10u) != 0u) {
            // Subdirectory: follow cluster chain and enumerate
            const uint32_t clusBytes = static_cast<uint32_t>(geometry_.bytesPerSector) * geometry_.sectorsPerCluster;
            uint16_t c               = startClus;
            uint32_t guard           = 0u;
            while (c >= 2u && c < 0xFF7u && ++guard <= geometry_.totalClusters) {
                const uint32_t sec = geometry_.dataAreaSector + static_cast<uint32_t>(c - 2u) * geometry_.sectorsPerCluster;
                enumerateFat12Directory(sec * geometry_.bytesPerSector, clusBytes, relName, depth + 1);
                c = readFat12Entry(c);
            }
            continue;
        }

        const VF50FileKind kind = classifyFileKind(baseName);
        if (kind == VF50FileKind::Unknown) {
            continue; // Strictly ignore unrecognized/non-VF50 extensions (e.g. MS-DOS ._DD recovery files)
        }

        DiskFileEntry item{};
        item.name            = baseName;
        item.fullOrRelPath   = relName;
        item.kind            = kind;
        item.sizeBytes       = fileSize;
        item.firstCluster    = startClus;
        item.fatDirOffset    = dirOffsetBytes + off;
        item.hasFatDirOffset = true;
        entries_.push_back(std::move(item));
    }
}

bool VF50DiskManager::readEntryBytes(std::size_t entryIndex,
                                    std::vector<uint8_t>& outBytes,
                                    juce::String& outStatusMessage) const {
    if (entryIndex >= entries_.size()) {
        outStatusMessage = "No file selected";
        return false;
    }

    const auto& e = entries_[entryIndex];
    if (sourceKind_ == DiskSourceKind::Fat12Image) {
        if (!extractFat12Chain(e.firstCluster, e.sizeBytes, outBytes)) {
            const juce::String cleanEntry = sanitizeAsciiString(
                reinterpret_cast<const uint8_t*>(e.name.data()),
                static_cast<int>(e.name.size()));
            outStatusMessage = "FAT12 read error: " + (cleanEntry.isNotEmpty() ? cleanEntry : juce::String("ENTRY"));
            return false;
        }
        return true;
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory) {
        const juce::File f(juce::String::fromUTF8(e.fullOrRelPath.c_str()));
        juce::MemoryBlock mb;
        if (!f.loadFileAsData(mb)) {
            outStatusMessage = "File read error: " + f.getFileName();
            return false;
        }
        outBytes.resize(mb.getSize());
        if (mb.getSize() > 0u) {
            std::memcpy(outBytes.data(), mb.getData(), mb.getSize());
        }
        return true;
    }

    outStatusMessage = "No disk mounted";
    return false;
}

bool VF50DiskManager::writeFileToMountedFat12(std::string_view fileName83,
                                             const uint8_t* data,
                                             std::size_t sizeBytes,
                                             juce::String& outStatusMessage) {
    if (writeProtected_) {
        outStatusMessage = "Disk Write Protected";
        return false;
    }
    if (sourceKind_ != DiskSourceKind::Fat12Image) {
        outStatusMessage = "No FAT12 image mounted";
        return false;
    }

    std::array<uint8_t, 11> target83{};
    formatDos83(fileName83, target83);

    // 1. Locate existing matching root entry OR first free slot
    int existingSlot = -1;
    int freeSlot     = -1;
    const int maxRootSlots = static_cast<int>(geometry_.rootEntries);
    for (int i = 0; i < maxRootSlots; ++i) {
        uint8_t* ent = &imageBytes_[geometry_.rootDirByteOffset + static_cast<uint32_t>(i) * 32u];
        if (ent[0] == 0x00u || ent[0] == 0xE5u) {
            if (freeSlot < 0) freeSlot = i;
            if (ent[0] == 0x00u) break;
            continue;
        }
        if ((ent[11] & 0x18u) == 0u && std::memcmp(ent, target83.data(), 11u) == 0) {
            existingSlot = i;
            break;
        }
    }

    const int useSlot = (existingSlot >= 0) ? existingSlot : freeSlot;
    if (useSlot < 0) {
        outStatusMessage = "Disk root directory full";
        return false;
    }

    // 2. Temporarily collect old clusters if overwriting so we know total available space
    std::vector<uint16_t> oldClusters;
    if (existingSlot >= 0) {
        const uint8_t* ent = &imageBytes_[geometry_.rootDirByteOffset + static_cast<uint32_t>(existingSlot) * 32u];
        uint16_t c         = readU16LE(&ent[26]);
        uint32_t guard     = 0u;
        while (c >= 2u && c < 0xFF7u && ++guard <= geometry_.totalClusters) {
            oldClusters.push_back(c);
            c = readFat12Entry(c);
        }
    }

    const uint32_t clusterBytes = static_cast<uint32_t>(geometry_.bytesPerSector) * geometry_.sectorsPerCluster;
    const std::size_t neededClusters = (sizeBytes == 0u) ? 0u : ((sizeBytes + clusterBytes - 1u) / clusterBytes);

    // Free old clusters first
    for (const uint16_t c : oldClusters) {
        writeFat12Entry(c, 0x000u);
    }

    std::vector<uint16_t> allocClusters;
    allocClusters.reserve(neededClusters);
    const uint16_t maxClusterId = static_cast<uint16_t>(geometry_.totalClusters + 1u);
    for (uint16_t c = 2u; c <= maxClusterId && allocClusters.size() < neededClusters; ++c) {
        if (readFat12Entry(c) == 0x000u) {
            allocClusters.push_back(c);
        }
    }

    if (allocClusters.size() < neededClusters) {
        outStatusMessage = "Floppy image full";
        return false;
    }

    // 3. Link FAT12 chain & write cluster payload bytes
    std::size_t written = 0u;
    for (std::size_t idx = 0u; idx < allocClusters.size(); ++idx) {
        const uint16_t c    = allocClusters[idx];
        const uint16_t next = (idx + 1u < allocClusters.size()) ? allocClusters[idx + 1u] : 0xFFFu;
        writeFat12Entry(c, next);

        const uint32_t sec     = geometry_.dataAreaSector + static_cast<uint32_t>(c - 2u) * geometry_.sectorsPerCluster;
        const std::size_t bOff = static_cast<std::size_t>(sec) * geometry_.bytesPerSector;
        std::memset(&imageBytes_[bOff], 0, clusterBytes);
        const std::size_t chunk = std::min<std::size_t>(clusterBytes, sizeBytes - written);
        if (chunk > 0u && data != nullptr) {
            std::memcpy(&imageBytes_[bOff], data + written, chunk);
            written += chunk;
        }
    }

    // 4. Write 32-byte DOS directory entry
    uint8_t* ent = &imageBytes_[geometry_.rootDirByteOffset + static_cast<uint32_t>(useSlot) * 32u];
    std::memset(ent, 0, 32u);
    std::memcpy(ent, target83.data(), 11u);
    ent[11] = 0x20u; // Archive attribute
    writeU16LE(&ent[22], 0x6000u); // 12:00:00
    writeU16LE(&ent[24], 0x1221u); // 1989-01-01 (VF50 release era!)
    writeU16LE(&ent[26], allocClusters.empty() ? 0u : allocClusters.front());
    writeU32LE(&ent[28], static_cast<uint32_t>(sizeBytes));

    // Flush updated image to host file if mounted from disk
    if (mountedFile_ != juce::File{}) {
        (void)mountedFile_.replaceWithData(imageBytes_.data(), imageBytes_.size());
    }

    rescanMountedSource();

    const std::string cleanStem = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(target83.data()), 8u));
    const std::string cleanExt  = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(target83.data() + 8u), 3u));
    outStatusMessage = "Saved " + juce::String(cleanStem + "." + cleanExt) + " to disk image";
    return true;
}

// =============================================================================
// Native VF50 Format Builders & Parsers (.VOI, .PFM, .RHY, .ALL, .SEQ)
// =============================================================================

void VF50DiskManager::buildVoiceBankBytes(const vf50::state::VF50MemoryManager& mem, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    outBytes.reserve(8u + 100u * 95u);
    outBytes.insert(outBytes.end(), std::begin(kMagicVoi), std::end(kMagicVoi));

    vf50::state::StoredVoiceSlot slot{};
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kVoiceBankSize; ++i) {
        slot.setFromVoice(mem.getUserVoice(i));
        outBytes.insert(outBytes.end(), slot.voice.rawRomBytes.begin(), slot.voice.rawRomBytes.end());
    }
}

void VF50DiskManager::buildPerfBankBytes(const vf50::state::VF50MemoryManager& mem, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    outBytes.reserve(8u + 100u * 97u + 800u);
    outBytes.insert(outBytes.end(), std::begin(kMagicPfm), std::end(kMagicPfm));

    vf50::state::StoredPerformanceSlot slot{};
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kPerfBankSize; ++i) {
        slot.setFromPerformance(mem.getUserPerformance(i), mem.getUserPerformancePartPans(i));
        outBytes.insert(outBytes.end(), slot.perf.rawRomBytes.begin(), slot.perf.rawRomBytes.end());
    }
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kPerfBankSize; ++i) {
        const auto& pans = mem.getUserPerformancePartPans(i);
        for (int8_t p : pans) {
            outBytes.push_back(static_cast<uint8_t>(p));
        }
    }
}

void VF50DiskManager::buildHardwareSynBytes(const vf50::plugin::VF50AudioProcessor& processor,
                                           std::vector<uint8_t>& outBytes) {
    // Authentic VF50 hardware 26,624-byte (0x6800) .I## (SYN) layout:
    //   0x0000..0x03FF (1,024 B): Synth Setup header (40-char greeting split @ 0x2B..0x3A & 0x4B..0x62)
    //   0x0400..0x35FF (12,800 B): 100 x 128 B Performances (97B ROM + 31B pad)
    //   0x3600..0x67FF (12,800 B): 100 x 128 B Voices (88B base + 6B pad + 7B DSP + 27B pad)
    outBytes.assign(0x6800u, 0x00u);
    const auto& msg = processor.classicSystemSettings().powerOnMessage;
    for (std::size_t i = 0u; i < 16u; ++i) {
        outBytes[0x2Bu + i] = static_cast<uint8_t>(msg[i]);
    }
    for (std::size_t i = 0u; i < 24u; ++i) {
        outBytes[0x4Bu + i] = static_cast<uint8_t>(msg[16u + i]);
    }

    const auto& mem = processor.hostMemory();
    vf50::state::StoredPerformanceSlot perfSlot{};
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kPerfBankSize; ++i) {
        perfSlot.setFromPerformance(mem.getUserPerformance(i), mem.getUserPerformancePartPans(i));
        std::memcpy(&outBytes[0x0400u + i * 128u], perfSlot.perf.rawRomBytes.data(), 97u);
    }

    vf50::state::StoredVoiceSlot voiceSlot{};
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kVoiceBankSize; ++i) {
        voiceSlot.setFromVoice(mem.getUserVoice(i));
        uint8_t* dst = &outBytes[0x3600u + i * 128u];
        std::memcpy(dst, voiceSlot.voice.rawRomBytes.data(), 88u);
        std::memcpy(dst + 94u, voiceSlot.voice.rawRomBytes.data() + 88u, 7u);
    }
}

void VF50DiskManager::buildCardBankBytes(const vf50::state::VirtualCardBank& cardBank,
                                        std::vector<uint8_t>& outBytes) {
    // Authentic VF50 hardware 26,624-byte (0x6800) .C## (CARD) layout (VF50SYN bank)
    outBytes.assign(0x6800u, 0x00u);
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kPerfBankSize; ++i) {
        vf50::state::StoredPerformanceSlot perfSlot = cardBank.performances[i];
        perfSlot.repackRomBytes();
        std::memcpy(&outBytes[0x0400u + i * 128u], perfSlot.perf.rawRomBytes.data(), 97u);
    }
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kVoiceBankSize; ++i) {
        vf50::state::StoredVoiceSlot voiceSlot = cardBank.voices[i];
        voiceSlot.repackRomBytes();
        uint8_t* dst = &outBytes[0x3600u + i * 128u];
        std::memcpy(dst, voiceSlot.voice.rawRomBytes.data(), 88u);
        std::memcpy(dst + 94u, voiceSlot.voice.rawRomBytes.data() + 88u, 7u);
    }
}

void VF50DiskManager::buildRhythmDataBytes(const vf50::state::VF50MemoryManager& mem,
                                          std::vector<uint8_t>& outBytes,
                                          const std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs) {
    outBytes.clear();
    outBytes.insert(outBytes.end(), std::begin(kMagicRhy), std::end(kMagicRhy));

    auto writeKit = [&outBytes](const vf50::state::RhythmKitBank& kit) {
        outBytes.insert(outBytes.end(), kit.nameChars.begin(), kit.nameChars.end());
        outBytes.push_back(kit.rhythmVolume);
        outBytes.push_back(static_cast<uint8_t>(kit.effectMode));
        outBytes.push_back(kit.keyMapSelect);
        for (const auto& d : kit.instruments) {
            outBytes.push_back(d.volume);
            outBytes.push_back(static_cast<uint8_t>(d.pan));
            const int16_t pitchCents = static_cast<int16_t>(std::clamp(d.pitchSemitones * 100.0f, -2400.0f, 2400.0f));
            appendU16LE(outBytes, static_cast<uint16_t>(pitchCents));
            outBytes.push_back(d.accent);
            outBytes.push_back(d.outAssign);
            outBytes.push_back(d.midiNote);
        }
    };

    outBytes.push_back(static_cast<uint8_t>(mem.selectedRhythmKit()));
    writeKit(mem.activeRhythmKit());
    for (std::size_t k = 0u; k < vf50::state::VF50MemoryManager::kUserRhythmKitCount; ++k) {
        writeKit(mem.userRhythmKit(k));
    }

    for (std::size_t m = 0u; m < vf50::state::VF50MemoryManager::kUserKeyMapCount; ++m) {
        const auto& km = mem.userKeyMap(m);
        for (int8_t v : km) {
            outBytes.push_back(static_cast<uint8_t>(v));
        }
    }

    for (std::size_t p = 0u; p < vf50::state::VF50MemoryManager::kRhythmPatternCount; ++p) {
        const auto& ptn = mem.userRhythmPattern(p);
        outBytes.push_back(ptn.bars);
        outBytes.push_back(ptn.timeSigNum);
        outBytes.push_back(ptn.timeSigDen);
        appendU16LE(outBytes, ptn.totalTicks);
        const uint16_t cnt = std::min<uint16_t>(ptn.eventCount, static_cast<uint16_t>(vf50::state::UserRhythmPattern::kMaxEvents));
        appendU16LE(outBytes, cnt);
        for (uint16_t e = 0u; e < cnt; ++e) {
            appendU16LE(outBytes, ptn.events[e].tick);
            outBytes.push_back(ptn.events[e].drumIndex);
            outBytes.push_back(ptn.events[e].velocityLevel);
        }
    }

    if (optRhythmSongs != nullptr) {
        outBytes.push_back('R');
        outBytes.push_back('Y');
        outBytes.push_back('S');
        outBytes.push_back('G');
        for (std::size_t s = 0u; s < 8u; ++s) {
            const auto& song = (*optRhythmSongs)[s];
            outBytes.insert(outBytes.end(), song.name.begin(), song.name.end());
            const uint16_t pc = std::min<uint16_t>(song.partCount, static_cast<uint16_t>(vf50::generated::RhythmSongData::kMaxParts));
            appendU16LE(outBytes, pc);
            for (uint16_t i = 0u; i < pc; ++i) {
                const auto& pt = song.parts[i];
                outBytes.push_back(static_cast<uint8_t>(pt.type));
                outBytes.push_back(pt.ptnIsUser ? 1u : 0u);
                outBytes.push_back(pt.ptnIndex);
                outBytes.push_back(pt.repeatCount);
                outBytes.push_back(static_cast<uint8_t>(pt.volDelta));
                appendU16LE(outBytes, static_cast<uint16_t>(pt.tempoDelta));
                outBytes.push_back(pt.tempoBeats);
                outBytes.push_back(static_cast<uint8_t>(pt.markChar));
            }
        }
    }
}

void VF50DiskManager::buildVF50SeqV1Bytes(const vf50::dsp::CustomSeqSongBuffer* optSeq,
                                           std::string_view fallbackTitle,
                                           int fallbackTempoBpm,
                                           std::vector<uint8_t>& outBytes) {
    outBytes.assign(34u, 0x00u);
    static constexpr uint8_t kHwSeqMagic[8] = { 0x56u, 0x35u, 0x30u, 0x53u, 0x45u, 0x51u, 0x56u, 0x31u };
    std::memcpy(outBytes.data(), kHwSeqMagic, 8u);

    const uint8_t tsNum = (optSeq != nullptr && optSeq->timeSigNum >= 1u) ? optSeq->timeSigNum : 4u;
    const uint8_t tsDen = (optSeq != nullptr && (optSeq->timeSigDen == 4u || optSeq->timeSigDen == 8u || optSeq->timeSigDen == 16u))
                              ? optSeq->timeSigDen
                              : 4u;
    outBytes[9] = encodeVF50TsFromTimeSig(tsNum, tsDen);

    const int bpm = std::clamp(
        (optSeq != nullptr && optSeq->initialTempoBpm >= 30u && optSeq->initialTempoBpm <= 300u)
            ? static_cast<int>(optSeq->initialTempoBpm)
            : fallbackTempoBpm,
        30, 300);
    outBytes[10] = static_cast<uint8_t>((bpm >> 8) & 0xFF);
    outBytes[11] = static_cast<uint8_t>(bpm & 0xFF);

    std::array<char, 8> title8{};
    title8.fill(' ');
    std::string_view srcTitle = fallbackTitle;
    if (optSeq != nullptr) {
        const auto st = optSeq->title();
        bool anyNonSpace = false;
        for (char c : st) {
            if (c != ' ' && c != '\0') {
                anyNonSpace = true;
                break;
            }
        }
        if (anyNonSpace) {
            srcTitle = st;
        }
    }
    for (std::size_t i = 0u; i < std::min<std::size_t>(srcTitle.size(), 8u); ++i) {
        const unsigned char c = static_cast<unsigned char>(srcTitle[i]);
        title8[i] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : ' ';
    }
    std::memcpy(outBytes.data() + 12u, title8.data(), 8u);

    std::array<uint8_t, 8> trackCh{ { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u } };
    if (optSeq != nullptr) {
        trackCh = optSeq->trackChannels;
        outBytes[28] = optSeq->playMode;
        outBytes[29] = (optSeq->perfPresetBank >= 1u) ? 2u : 0u;
        outBytes[30] = optSeq->perfNumber;
        outBytes[31] = optSeq->rhythmSyncIsSong ? 1u : 0u;
        outBytes[32] = optSeq->rhythmTargetIdx;
    } else {
        outBytes[28] = 1u;
        outBytes[29] = 2u;
        outBytes[30] = 0u;
        outBytes[31] = 0u;
        outBytes[32] = 0u;
    }
    for (std::size_t t = 0u; t < 8u; ++t) {
        outBytes[20u + t] = trackCh[t] & 0x0Fu;
    }

    auto appendDelta = [&outBytes](uint32_t dt) {
        while (dt > 0u) {
            const uint32_t chunk = std::min<uint32_t>(dt, 8191u);
            const uint32_t enc   = chunk << 1u;
            outBytes.push_back(static_cast<uint8_t>((enc >> 7u) & 0x7Fu));
            outBytes.push_back(static_cast<uint8_t>(enc & 0x7Fu));
            dt -= chunk;
            if (dt > 0u) {
                outBytes.push_back(0xF8u);
            }
        }
    };

    if (optSeq == nullptr || !optSeq->valid || optSeq->eventCount == 0u) {
        outBytes.push_back(0xF0u);
        outBytes.push_back(0x00u);
        outBytes.push_back(0xF5u);
        outBytes.push_back(0x80u | 24u);
        outBytes.push_back(60u);
        outBytes.push_back(64u);
        outBytes.push_back(0xF2u);
        outBytes.push_back(0xF9u);
        return;
    }

    const uint32_t tpb = std::max<uint32_t>(12u, optSeq->ticksPerBar);
    const std::size_t evCount = std::min<std::size_t>(optSeq->eventCount, optSeq->events.size());

    auto trackForChannel = [&trackCh](uint8_t ch) noexcept -> std::size_t {
        for (std::size_t t = 0u; t < 8u; ++t) {
            if ((trackCh[t] & 0x0Fu) == (ch & 0x0Fu)) {
                return t;
            }
        }
        return static_cast<std::size_t>(ch & 0x07u);
    };

    std::vector<bool> consumed(evCount, false);
    std::array<std::vector<std::pair<std::size_t, uint32_t>>, 8> trkEvents;

    for (std::size_t i = 0u; i < evCount; ++i) {
        if (consumed[i]) continue;
        const auto& ev = optSeq->events[i];
        const std::size_t trk = trackForChannel(ev.channel);
        if (ev.type == vf50::generated::DemoEventType::FmNoteOn && ev.data2 > 0u) {
            uint32_t gate = 48u;
            for (std::size_t j = i + 1u; j < evCount; ++j) {
                if (consumed[j]) continue;
                const auto& offEv = optSeq->events[j];
                const bool isMatchingOff = (offEv.channel == ev.channel && offEv.data1 == ev.data1)
                    && ((offEv.type == vf50::generated::DemoEventType::FmNoteOff)
                        || (offEv.type == vf50::generated::DemoEventType::FmNoteOn && offEv.data2 == 0u));
                if (isMatchingOff && offEv.tick >= ev.tick) {
                    gate = std::clamp<uint32_t>(offEv.tick - ev.tick, 1u, 8191u);
                    consumed[j] = true;
                    break;
                }
            }
            trkEvents[trk].push_back({ i, gate });
        } else if (ev.type == vf50::generated::DemoEventType::FmControl
                || ev.type == vf50::generated::DemoEventType::FmProgram
                || ev.type == vf50::generated::DemoEventType::FmAftertouch
                || ev.type == vf50::generated::DemoEventType::FmPitchBend
                || ev.type == vf50::generated::DemoEventType::TempoChange) {
            trkEvents[trk].push_back({ i, 0u });
        }
    }

    bool wroteAnyTrack = false;
    for (std::size_t t = 0u; t < 8u; ++t) {
        if (trkEvents[t].empty()) {
            continue;
        }
        wroteAnyTrack = true;
        outBytes.push_back(0xF0u);
        outBytes.push_back(static_cast<uint8_t>(t));

        uint32_t curTick = 0u;
        uint32_t nextBarTick = 0u;
        for (const auto& [idx, gate] : trkEvents[t]) {
            const auto& ev = optSeq->events[idx];
            while (nextBarTick <= ev.tick) {
                if (nextBarTick > curTick) {
                    appendDelta(nextBarTick - curTick);
                    curTick = nextBarTick;
                }
                outBytes.push_back(0xF5u);
                nextBarTick += tpb;
            }
            if (ev.tick > curTick) {
                appendDelta(ev.tick - curTick);
                curTick = ev.tick;
            }
            switch (ev.type) {
                case vf50::generated::DemoEventType::FmNoteOn: {
                    const uint32_t g = std::clamp<uint32_t>(gate, 1u, 8191u);
                    if (g <= 63u) {
                        outBytes.push_back(static_cast<uint8_t>(0x80u | g));
                    } else {
                        outBytes.push_back(static_cast<uint8_t>(0xC0u | ((g >> 7u) & 0x3Fu)));
                        outBytes.push_back(static_cast<uint8_t>(g & 0x7Fu));
                    }
                    outBytes.push_back(ev.data1 & 0x7Fu);
                    outBytes.push_back(std::max<uint8_t>(1u, ev.data2 & 0x7Fu));
                    break;
                }
                case vf50::generated::DemoEventType::FmControl:
                    outBytes.push_back(0xFBu);
                    outBytes.push_back(ev.data1 & 0x7Fu);
                    outBytes.push_back(ev.data2 & 0x7Fu);
                    break;
                case vf50::generated::DemoEventType::FmProgram:
                    outBytes.push_back(0xFCu);
                    outBytes.push_back((ev.data1 == 0u) ? 120u : 121u);
                    outBytes.push_back(0xFCu);
                    outBytes.push_back(std::min<uint8_t>(ev.data2 & 0x7Fu, 99u));
                    break;
                case vf50::generated::DemoEventType::FmAftertouch:
                    outBytes.push_back(0xFDu);
                    outBytes.push_back(ev.data1 & 0x7Fu);
                    break;
                case vf50::generated::DemoEventType::FmPitchBend:
                    outBytes.push_back(0xFEu);
                    outBytes.push_back(ev.data1 & 0x7Fu);
                    outBytes.push_back(ev.data2 & 0x7Fu);
                    break;
                case vf50::generated::DemoEventType::TempoChange:
                    outBytes.push_back(0xF6u);
                    outBytes.push_back(0x00u);
                    outBytes.push_back(static_cast<uint8_t>(std::clamp<int>(ev.data2, 30, 240)));
                    break;
                default:
                    break;
            }
        }
        const uint32_t targetEndTick = optSeq->totalBars * tpb;
        while (nextBarTick < targetEndTick) {
            if (nextBarTick > curTick) {
                appendDelta(nextBarTick - curTick);
                curTick = nextBarTick;
            }
            outBytes.push_back(0xF5u);
            nextBarTick += tpb;
        }
        outBytes.push_back(0xF2u);
    }

    if (!wroteAnyTrack) {
        outBytes.push_back(0xF0u);
        outBytes.push_back(0x00u);
        outBytes.push_back(0xF5u);
        outBytes.push_back(0x80u | 24u);
        outBytes.push_back(60u);
        outBytes.push_back(64u);
        outBytes.push_back(0xF2u);
    }
    outBytes.push_back(0xF9u);
}

void VF50DiskManager::buildAllDataBytes(const vf50::state::VF50MemoryManager& mem,
                                       std::vector<uint8_t>& outBytes,
                                       const std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs,
                                       const vf50::plugin::VF50AudioProcessor* optProcessor) {
    std::vector<uint8_t> memChunk;
    std::vector<uint8_t> voiChunk;
    std::vector<uint8_t> pfmChunk;
    std::vector<uint8_t> rhyChunk;

    mem.serializeToBinary(memChunk);
    buildVoiceBankBytes(mem, voiChunk);
    buildPerfBankBytes(mem, pfmChunk);
    buildRhythmDataBytes(mem, rhyChunk, optRhythmSongs);

    outBytes.clear();
    outBytes.reserve(24u + memChunk.size() + voiChunk.size() + pfmChunk.size() + rhyChunk.size());
    outBytes.insert(outBytes.end(), std::begin(kMagicAll), std::end(kMagicAll));
    appendU32LE(outBytes, static_cast<uint32_t>(memChunk.size()));
    appendU32LE(outBytes, static_cast<uint32_t>(voiChunk.size()));
    appendU32LE(outBytes, static_cast<uint32_t>(pfmChunk.size()));
    appendU32LE(outBytes, static_cast<uint32_t>(rhyChunk.size()));

    outBytes.insert(outBytes.end(), memChunk.begin(), memChunk.end());
    outBytes.insert(outBytes.end(), voiChunk.begin(), voiChunk.end());
    outBytes.insert(outBytes.end(), pfmChunk.begin(), pfmChunk.end());
    outBytes.insert(outBytes.end(), rhyChunk.begin(), rhyChunk.end());

    if (optProcessor != nullptr) {
        uint8_t slotMask = 0u;
        for (std::size_t s = 0u; s < 8u; ++s) {
            if (optProcessor->hasUserSequenceInSlot(s)) {
                slotMask |= static_cast<uint8_t>(1u << s);
            }
        }
        if (slotMask != 0u) {
            outBytes.insert(outBytes.end(), std::begin(kMagicSeq8), std::end(kMagicSeq8));
            outBytes.push_back(slotMask);
            std::vector<uint8_t> seqChunk;
            for (std::size_t s = 0u; s < 8u; ++s) {
                if (((slotMask >> s) & 1u) != 0u) {
                    const auto* cs = optProcessor->userSequenceInSlot(s);
                    buildVF50SeqV1Bytes(cs, "SONG", optProcessor->activeDemoTempoBpm(), seqChunk);
                    appendU32LE(outBytes, static_cast<uint32_t>(seqChunk.size()));
                    outBytes.insert(outBytes.end(), seqChunk.begin(), seqChunk.end());
                }
            }
        }
    }
}

bool VF50DiskManager::applyVoiceBytes(const uint8_t* data,
                                     std::size_t sizeBytes,
                                     vf50::state::VF50MemoryManager& mem) noexcept {
    if (data == nullptr || sizeBytes < 95u) return false;

    std::size_t offset = 0u;
    if (sizeBytes >= 8u + 95u && std::memcmp(data, kMagicVoi, 8u) == 0) {
        offset = 8u;
    }

    const std::size_t rem = sizeBytes - offset;
    if (rem >= 95u && (rem % 95u) == 0u && (rem / 95u) <= 100u) {
        const std::size_t count = rem / 95u;
        vf50::state::StoredVoiceSlot slot{};
        for (std::size_t i = 0u; i < count; ++i) {
            std::array<uint8_t, 95> raw{};
            std::memcpy(raw.data(), data + offset + i * 95u, 95u);
            slot.unpackFromRom95(raw);
            mem.storeUserVoice(i, slot.voice);
        }
        mem.selectSingleVoice(vf50::state::MemoryBank::UserRam, mem.selectedVoiceIndex());
        return true;
    }

    // Support raw headerless 128-byte VMEM records (32-voice = 4096B or 100-voice = 12800B)
    if ((rem == 4096u || rem == 12800u || rem == 16384u) && !hasSysExStartByte(data, sizeBytes)) {
        auto scratchSynth = std::make_unique<vf50::dsp::VF50SynthEngine>();
        vf50::midi::VF50SysExHandler sx{};
        const std::size_t blocks = (rem + 4095u) / 4096u;
        for (std::size_t b = 0u; b < std::min<std::size_t>(blocks, 4u); ++b) {
            const std::array<uint8_t, 7> blkHdr = {
                0xF0u, 0x43u, 0x10u, 0x44u, 0x07u, static_cast<uint8_t>(b + 1u), 0xF7u
            };
            (void)sx.processSysEx(blkHdr.data(), blkHdr.size(), mem, *scratchSynth, nullptr);

            std::vector<uint8_t> pkt(4104u, 0u);
            pkt[0] = 0xF0u;
            pkt[1] = 0x43u;
            pkt[2] = 0x00u;
            pkt[3] = 0x04u;
            pkt[4] = 0x20u;
            pkt[5] = 0x00u;
            const std::size_t avail = std::min<std::size_t>(4096u, rem - b * 4096u);
            std::memcpy(&pkt[6], data + offset + b * 4096u, avail);
            pkt[4102] = vf50::midi::VF50SysExHandler::computeSysExChecksum(&pkt[6], 4096u);
            pkt[4103] = 0xF7u;
            (void)sx.processSysEx(pkt.data(), pkt.size(), mem, *scratchSynth, nullptr);
        }
        mem.selectSingleVoice(vf50::state::MemoryBank::UserRam, mem.selectedVoiceIndex());
        return true;
    }

    return false;
}

bool VF50DiskManager::applyPerfBytes(const uint8_t* data,
                                    std::size_t sizeBytes,
                                    vf50::state::VF50MemoryManager& mem) noexcept {
    if (data == nullptr || sizeBytes < 96u) return false;

    if (sizeBytes >= 8u + 100u * 97u && std::memcmp(data, kMagicPfm, 8u) == 0) {
        const uint8_t* pfBase   = data + 8u;
        const bool hasPans      = (sizeBytes >= 8u + 100u * 97u + 800u);
        const uint8_t* pansBase = hasPans ? (pfBase + 100u * 97u) : nullptr;

        vf50::state::StoredPerformanceSlot slot{};
        for (std::size_t i = 0u; i < 100u; ++i) {
            std::array<uint8_t, 97> raw{};
            std::memcpy(raw.data(), pfBase + i * 97u, 97u);
            std::array<int8_t, 8> pans{};
            if (pansBase != nullptr) {
                for (std::size_t p = 0u; p < 8u; ++p) {
                    pans[p] = static_cast<int8_t>(std::clamp<int>(static_cast<int8_t>(pansBase[i * 8u + p]), -64, 63));
                }
            }
            slot.unpackFromRom97(raw, pans);
            mem.storeUserPerformance(i, slot.perf, slot.partPans);
        }
        mem.selectPerformance(vf50::state::MemoryBank::UserRam, mem.selectedPerfIndex());
        return true;
    }

    // Raw 96-byte or 97-byte VF50 Performance records
    if ((sizeBytes % 97u) == 0u && (sizeBytes / 97u) <= 100u) {
        const std::size_t count = sizeBytes / 97u;
        vf50::state::StoredPerformanceSlot slot{};
        for (std::size_t i = 0u; i < count; ++i) {
            std::array<uint8_t, 97> raw{};
            std::memcpy(raw.data(), data + i * 97u, 97u);
            slot.unpackFromRom97(raw);
            mem.storeUserPerformance(i, slot.perf);
        }
        mem.selectPerformance(vf50::state::MemoryBank::UserRam, mem.selectedPerfIndex());
        return true;
    }

    if ((sizeBytes % 96u) == 0u && (sizeBytes / 96u) <= 100u) {
        const std::size_t count = sizeBytes / 96u;
        vf50::state::StoredPerformanceSlot slot{};
        for (std::size_t i = 0u; i < count; ++i) {
            std::array<uint8_t, 97> raw{};
            std::memcpy(raw.data(), data + i * 96u, 96u);
            slot.unpackFromRom97(raw);
            mem.storeUserPerformance(i, slot.perf);
        }
        mem.selectPerformance(vf50::state::MemoryBank::UserRam, mem.selectedPerfIndex());
        return true;
    }

    return false;
}

namespace {

[[nodiscard]] bool validateHardwareRhythmRam(const uint8_t* ram,
                                             std::size_t ramSize,
                                             bool allowEmptyZeroStream) noexcept {
    if (ram == nullptr || ramSize < 0x1E2u) {
        return false;
    }
    if (hasKnownVF50BinaryMagic(ram, ramSize)
        || (ramSize >= 6u && std::memcmp(ram, "QX5FD ", 6u) == 0)) {
        return false;
    }

    const uint16_t streamEnd = static_cast<uint16_t>((static_cast<uint16_t>(ram[0xD8]) << 8u) | ram[0xD9]);
    if (0x1E2u + static_cast<std::size_t>(streamEnd) > ramSize) {
        return false;
    }

    std::size_t popPatterns = 0u;
    std::size_t popSongs    = 0u;
    uint16_t lastOff        = 0u;
    for (std::size_t i = 0u; i < 100u; ++i) {
        const uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(ram[i * 2u]) << 8u) | ram[i * 2u + 1u]);
        if ((raw & 0x8000u) != 0u) {
            const uint16_t off = raw & 0x7FFFu;
            if (off > streamEnd || off < lastOff) {
                return false;
            }
            if (ram[0xDAu + i] > 31u) {
                return false;
            }
            lastOff = off;
            ++popPatterns;
        }
    }
    for (std::size_t s = 0u; s < 8u; ++s) {
        const uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(ram[0xC8u + s * 2u]) << 8u) | ram[0xC8u + s * 2u + 1u]);
        if ((raw & 0x8000u) != 0u) {
            const uint16_t off = raw & 0x7FFFu;
            if (off > streamEnd || off < lastOff) {
                return false;
            }
            lastOff = off;
            ++popSongs;
        }
    }

    if (streamEnd == 0u) {
        return allowEmptyZeroStream && popPatterns == 0u && popSongs == 0u;
    }
    return (popPatterns > 0u || popSongs > 0u);
}

void applyHardwareRhythmSetupHeader(const uint8_t* hdr1024, vf50::state::VF50MemoryManager& mem) noexcept {
    if (hdr1024 == nullptr) {
        return;
    }
    // Real VF50 1,024-byte Rhythm Setup header (verified across all real .R## and .V## files):
    //   0x20        (1B) : Rhythm Master Volume (0..99)
    //   0x100..0x13C (61B): drum volumes (0..15)
    //   0x13D..0x179 (61B): drum pans (0..6, center=3 -> -3..+3)
    //   0x17A..0x1B6 (61B): drum MIDI notes (0..127)
    auto& kit = mem.mutableActiveRhythmKit();
    if (hdr1024[0x20u] >= 1u && hdr1024[0x20u] <= 99u) {
        kit.rhythmVolume = hdr1024[0x20u];
    }

    bool anyVol = false;
    for (std::size_t d = 0u; d < vf50::generated::kDrumInstrumentCount; ++d) {
        const uint8_t vol  = hdr1024[0x100u + d];
        const uint8_t pan  = hdr1024[0x13Du + d];
        const uint8_t note = hdr1024[0x17Au + d];
        if (vol > 15u || pan > 6u || note > 127u) {
            return;
        }
        if (vol > 0u) {
            anyVol = true;
        }
    }
    if (!anyVol) {
        return;
    }
    for (std::size_t d = 0u; d < vf50::generated::kDrumInstrumentCount; ++d) {
        kit.instruments[d].volume   = hdr1024[0x100u + d];
        kit.instruments[d].pan      = static_cast<int8_t>(static_cast<int>(hdr1024[0x13Du + d]) - 3);
        kit.instruments[d].midiNote = hdr1024[0x17Au + d];
    }
}

[[nodiscard]] bool applyHardwareRhythmRamBlock(const uint8_t* ram,
                                               std::size_t ramSize,
                                               const uint8_t* optSetupHdr1024,
                                               vf50::state::VF50MemoryManager& mem,
                                               std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs) noexcept {
    if (!validateHardwareRhythmRam(ram, ramSize, optSetupHdr1024 != nullptr)) {
        return false;
    }

    if (optSetupHdr1024 != nullptr) {
        applyHardwareRhythmSetupHeader(optSetupHdr1024, mem);
    }

    // Clear all 100 user rhythm patterns and all 8 rhythm songs before unpacking so stale data never lingers
    for (std::size_t i = 0u; i < vf50::state::VF50MemoryManager::kRhythmPatternCount; ++i) {
        mem.mutableUserRhythmPattern(i).clear();
    }
    if (optRhythmSongs != nullptr) {
        for (auto& song : *optRhythmSongs) {
            song.clear();
        }
    }

    const uint16_t streamEnd = static_cast<uint16_t>((static_cast<uint16_t>(ram[0xD8]) << 8u) | ram[0xD9]);
    if (streamEnd == 0u) {
        if (optSetupHdr1024 != nullptr) {
            const uint8_t ptnSel = optSetupHdr1024[0x2Au];
            if (ptnSel >= 100u && ptnSel < 200u) {
                mem.selectRhythmPattern(vf50::state::MemoryBank::FactoryRom, static_cast<std::size_t>(ptnSel - 100u));
            }
        }
        return true;
    }

    std::vector<std::size_t> populated;
    std::array<uint16_t, 100> ptrs{};
    for (std::size_t i = 0u; i < 100u; ++i) {
        ptrs[i] = static_cast<uint16_t>((static_cast<uint16_t>(ram[i * 2u]) << 8u) | ram[i * 2u + 1u]);
        if ((ptrs[i] & 0x8000u) != 0u) {
            populated.push_back(i);
        }
    }

    std::vector<std::size_t> populatedSongs;
    std::array<uint16_t, 8> songPtrs{};
    uint16_t ptnStreamEnd = streamEnd;
    for (std::size_t s = 0u; s < 8u; ++s) {
        songPtrs[s] = static_cast<uint16_t>((static_cast<uint16_t>(ram[0xC8u + s * 2u]) << 8u) | ram[0xC8u + s * 2u + 1u]);
        if ((songPtrs[s] & 0x8000u) != 0u) {
            if (populatedSongs.empty()) {
                ptnStreamEnd = std::min<uint16_t>(ptnStreamEnd, static_cast<uint16_t>(songPtrs[s] & 0x7FFFu));
            }
            populatedSongs.push_back(s);
        }
    }

    const uint8_t* uStream = ram + 0x1E2u;
    for (std::size_t pPos = 0u; pPos < populated.size(); ++pPos) {
        const std::size_t idx = populated[pPos];
        const uint16_t sOff   = ptrs[idx] & 0x7FFFu;
        const uint16_t eOff   = (pPos + 1u < populated.size())
                                  ? static_cast<uint16_t>(ptrs[populated[pPos + 1u]] & 0x7FFFu)
                                  : ptnStreamEnd;
        if (sOff >= eOff || 0x1E2u + eOff > ramSize) {
            continue;
        }

        uint8_t num = 4u, den = 4u;
        uint16_t tpb = 192u;
        decodeTicksPerBarFromVF50Ts(ram[0xDAu + idx], num, den, tpb);
        const uint8_t bars = static_cast<uint8_t>((ram[0x13Eu + idx] & 0x0Fu) + 1u);

        auto& dst = mem.mutableUserRhythmPattern(idx);
        dst.clear();
        dst.bars       = bars;
        dst.timeSigNum = num;
        dst.timeSigDen = den;
        dst.totalTicks = static_cast<uint16_t>(tpb * bars);

        uint16_t tick = 0u;
        uint16_t p    = sOff;
        while (p < eOff && tick < dst.totalTicks) {
            const uint8_t b = uStream[p++];
            if (b >= 0x80u) {
                tick = static_cast<uint16_t>(tick + (b & 0x7Fu));
            } else if (b <= 0x3Fu) {
                if (b < 61u) { // 0x3F is hardware erased-slot marker -> ignore
                    (void)dst.addEventSorted(tick, b, 5u);
                }
            } else if (p < eOff) {
                const uint8_t drum  = b & 0x3Fu;
                const uint8_t param = uStream[p++];
                // Verified HD6303 IC723 @0x0F0AA accent decoding: bit 5 sign-extend, else low 3 bits
                const int offset    = ((param & 0x20u) != 0u)
                                        ? (static_cast<int>(param | 0x80u) - 256)
                                        : static_cast<int>(param & 0x07u);
                const int hwAccent  = std::clamp(5 + offset, 0, 7);
                const auto vel      = static_cast<uint8_t>(std::clamp(hwAccent, 1, 8));
                if (drum < 61u) {
                    (void)dst.addEventSorted(tick, drum, vel);
                }
            }
        }
    }

    // Decode all populated Rhythm Songs (S1..S8) using exact HD6303 IC723 $A824 table = [1, 1, 3, 3, 4, 3]:
    //   0x00..0x63 (1B) = I00..I99, 0x64..0xC7 (1B) = P00..P99,
    //   0xC8 (1B) = ||:, 0xC9 <N> 0xC9 (3B) = :|| x (N+1),
    //   0xCA <d> 0xCA (3B) = Vol, 0xCB <d> <beats> 0xCB (4B) = Tempo,
    //   0xCC <m> 0xCC (3B) = Mark
    if (optRhythmSongs != nullptr) {
        for (std::size_t sPos = 0u; sPos < populatedSongs.size(); ++sPos) {
            const std::size_t sIdx = populatedSongs[sPos];
            const uint16_t sOff    = songPtrs[sIdx] & 0x7FFFu;
            const uint16_t eOff    = (sPos + 1u < populatedSongs.size())
                                       ? static_cast<uint16_t>(songPtrs[populatedSongs[sPos + 1u]] & 0x7FFFu)
                                       : streamEnd;
            if (sOff >= eOff || 0x1E2u + eOff > ramSize) {
                continue;
            }

            auto& dstSong = (*optRhythmSongs)[sIdx];
            dstSong.clear();
            for (std::size_t c = 0u; c < 8u; ++c) {
                const uint8_t ch = ram[0x1A2u + sIdx * 8u + c];
                dstSong.name[c] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
            }

            uint16_t p = sOff;
            while (p < eOff && dstSong.partCount < vf50::generated::RhythmSongData::kMaxParts) {
                const uint8_t cmd = uStream[p];
                vf50::generated::RhythmSongPart part{};
                if (cmd <= 0x63u) {
                    part.type      = vf50::generated::RhythmSongPartType::Pattern;
                    part.ptnIsUser = true;
                    part.ptnIndex  = cmd;
                    dstSong.parts[dstSong.partCount++] = part;
                    ++p;
                } else if (cmd <= 0xC7u) {
                    part.type      = vf50::generated::RhythmSongPartType::Pattern;
                    part.ptnIsUser = false;
                    part.ptnIndex  = static_cast<uint8_t>(cmd - 0x64u);
                    dstSong.parts[dstSong.partCount++] = part;
                    ++p;
                } else if (cmd == 0xC8u) {
                    part.type = vf50::generated::RhythmSongPartType::RepeatBegin;
                    dstSong.parts[dstSong.partCount++] = part;
                    ++p;
                } else if (cmd == 0xC9u) {
                    const uint8_t extraRepeats = (p + 1u < eOff) ? static_cast<uint8_t>(uStream[p + 1u] & 0x7Fu) : 1u;
                    part.type        = vf50::generated::RhythmSongPartType::RepeatEnd;
                    part.repeatCount = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(extraRepeats) + 1, 1, 99));
                    dstSong.parts[dstSong.partCount++] = part;
                    p = static_cast<uint16_t>(p + ((p + 2u < eOff && uStream[p + 2u] == 0xC9u) ? 3u : 2u));
                } else if (cmd == 0xCAu) {
                    part.type     = vf50::generated::RhythmSongPartType::Volume;
                    part.volDelta = (p + 1u < eOff) ? static_cast<int8_t>(uStream[p + 1u]) : int8_t{ 0 };
                    dstSong.parts[dstSong.partCount++] = part;
                    p = static_cast<uint16_t>(p + ((p + 2u < eOff) ? 3u : 2u));
                } else if (cmd == 0xCBu) {
                    part.type       = vf50::generated::RhythmSongPartType::Tempo;
                    part.tempoDelta = (p + 1u < eOff) ? static_cast<int16_t>(static_cast<int8_t>(uStream[p + 1u])) : int16_t{ 0 };
                    part.tempoBeats = (p + 2u < eOff) ? uStream[p + 2u] : 0u;
                    dstSong.parts[dstSong.partCount++] = part;
                    p = static_cast<uint16_t>(p + ((p + 3u < eOff && uStream[p + 3u] == 0xCBu) ? 4u : 3u));
                } else if (cmd == 0xCCu) {
                    const uint8_t rawMark = (p + 1u < eOff) ? uStream[p + 1u] : 0u;
                    part.type     = vf50::generated::RhythmSongPartType::Mark;
                    part.markChar = (rawMark >= 'A' && rawMark <= 'G')
                                        ? static_cast<char>(rawMark)
                                        : static_cast<char>('A' + (rawMark % 7u));
                    dstSong.parts[dstSong.partCount++] = part;
                    p = static_cast<uint16_t>(p + ((p + 2u < eOff) ? 3u : 2u));
                } else {
                    ++p;
                }
            }
        }
    }

    if (optSetupHdr1024 != nullptr) {
        const uint8_t ptnSel = optSetupHdr1024[0x2Au];
        if (ptnSel >= 100u && ptnSel < 200u) {
            mem.selectRhythmPattern(vf50::state::MemoryBank::FactoryRom, static_cast<std::size_t>(ptnSel - 100u));
        } else if (ptnSel < 100u && mem.userRhythmPattern(ptnSel).eventCount > 0u) {
            mem.selectRhythmPattern(vf50::state::MemoryBank::UserRam, static_cast<std::size_t>(ptnSel));
        }
    }
    return true;
}

} // namespace

bool VF50DiskManager::applyRhythmBytes(const uint8_t* data,
                                      std::size_t sizeBytes,
                                      vf50::state::VF50MemoryManager& mem,
                                      std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs) noexcept {
    if (data == nullptr || sizeBytes < 16u) return false;

    if (sizeBytes >= 8u && std::memcmp(data, kMagicRhy, 8u) == 0) {
        std::size_t pos = 8u;
        if (pos >= sizeBytes) return false;
        const uint8_t selKit = data[pos++];

        auto readKit = [&](vf50::state::RhythmKitBank& kit) noexcept -> bool {
            if (pos + 13u + 61u * 7u > sizeBytes) return false;
            for (std::size_t i = 0u; i < 10u; ++i) {
                const uint8_t c = data[pos++];
                kit.nameChars[i] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : ' ';
            }
            kit.rhythmVolume = std::min<uint8_t>(data[pos++], 99u);
            kit.effectMode   = static_cast<vf50::dsp::drums::RhythmEffectMode>(std::min<uint8_t>(data[pos++], 2u));
            kit.keyMapSelect = std::min<uint8_t>(data[pos++], 4u);
            for (std::size_t d = 0u; d < vf50::generated::kDrumInstrumentCount; ++d) {
                auto& inst          = kit.instruments[d];
                inst.volume         = std::min<uint8_t>(data[pos++], 15u);
                inst.pan            = static_cast<int8_t>(std::clamp<int>(static_cast<int8_t>(data[pos++]), -3, 3));
                const int16_t cents = static_cast<int16_t>(readU16LE(&data[pos]));
                pos += 2u;
                inst.pitchSemitones = std::clamp(static_cast<float>(cents) * 0.01f, -24.0f, 24.0f);
                inst.accent         = std::clamp<uint8_t>(data[pos++], 1u, 9u);
                inst.outAssign      = std::min<uint8_t>(data[pos++], 3u);
                inst.midiNote       = data[pos++] & 0x7Fu;
                inst.chokeGroup     = mem.runtimeRomBank().defaultDrumSetup[d].chokeGroup;
            }
            return true;
        };

        vf50::state::RhythmKitBank activeKit{};
        if (!readKit(activeKit)) return false;

        for (std::size_t k = 0u; k < vf50::state::VF50MemoryManager::kUserRhythmKitCount; ++k) {
            vf50::state::RhythmKitBank uKit{};
            if (!readKit(uKit)) return false;
            mem.setUserRhythmKit(k, uKit);
        }

        for (std::size_t m = 0u; m < vf50::state::VF50MemoryManager::kUserKeyMapCount; ++m) {
            if (pos + 61u > sizeBytes) return false;
            std::array<int8_t, 61> km{};
            for (std::size_t k = 0u; k < 61u; ++k) {
                const int8_t v = static_cast<int8_t>(data[pos++]);
                km[k] = (v >= 0 && v < 61) ? v : static_cast<int8_t>(-1);
            }
            mem.setUserKeyMap(m, km);
        }

        for (std::size_t p = 0u; p < vf50::state::VF50MemoryManager::kRhythmPatternCount; ++p) {
            if (pos + 7u > sizeBytes) return false;
            auto& ptn = mem.mutableUserRhythmPattern(p);
            ptn.clear();
            ptn.bars       = std::max<uint8_t>(1u, data[pos++]);
            ptn.timeSigNum = std::max<uint8_t>(1u, data[pos++]);
            ptn.timeSigDen = std::max<uint8_t>(4u, data[pos++]);
            ptn.totalTicks = std::max<uint16_t>(48u, readU16LE(&data[pos]));
            pos += 2u;
            const uint16_t cnt = readU16LE(&data[pos]);
            pos += 2u;
            if (pos + static_cast<std::size_t>(cnt) * 4u > sizeBytes) return false;
            for (uint16_t e = 0u; e < cnt; ++e) {
                const uint16_t tick = readU16LE(&data[pos]);
                const uint8_t drum  = data[pos + 2u];
                const uint8_t vel   = data[pos + 3u];
                pos += 4u;
                (void)ptn.addEventSorted(tick, drum, vel);
            }
        }

        if (optRhythmSongs != nullptr) {
            for (auto& s : *optRhythmSongs) {
                s.clear();
            }
            if (pos + 4u <= sizeBytes && std::memcmp(data + pos, "RYSG", 4u) == 0) {
                pos += 4u;
                for (std::size_t s = 0u; s < 8u && pos + 10u <= sizeBytes; ++s) {
                    auto& song = (*optRhythmSongs)[s];
                    for (std::size_t c = 0u; c < 8u; ++c) {
                        const uint8_t ch = data[pos++];
                        song.name[c] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : ' ';
                    }
                    const uint16_t pc = readU16LE(&data[pos]);
                    pos += 2u;
                    const std::size_t bytesNeeded = static_cast<std::size_t>(pc) * 9u;
                    if (pos + bytesNeeded > sizeBytes) {
                        break;
                    }
                    for (uint16_t i = 0u; i < pc; ++i) {
                        vf50::generated::RhythmSongPart pt{};
                        pt.type        = static_cast<vf50::generated::RhythmSongPartType>(std::min<uint8_t>(data[pos++], 5u));
                        pt.ptnIsUser   = (data[pos++] != 0u);
                        pt.ptnIndex    = std::min<uint8_t>(data[pos++], 99u);
                        pt.repeatCount = std::clamp<uint8_t>(data[pos++], 1u, 99u);
                        pt.volDelta    = static_cast<int8_t>(data[pos++]);
                        pt.tempoDelta  = static_cast<int16_t>(readU16LE(&data[pos]));
                        pos += 2u;
                        pt.tempoBeats  = data[pos++];
                        const uint8_t mc = data[pos++];
                        pt.markChar    = (mc >= 'A' && mc <= 'G') ? static_cast<char>(mc) : 'A';
                        if (song.partCount < vf50::generated::RhythmSongData::kMaxParts) {
                            song.parts[song.partCount++] = pt;
                        }
                    }
                }
            }
        }

        mem.selectRhythmKit(selKit);
        mem.mutableActiveRhythmKit() = activeKit;
        return true;
    }

    if (hasKnownVF50BinaryMagic(data, sizeBytes)
        || (sizeBytes >= 6u && std::memcmp(data, "QX5FD ", 6u) == 0)) {
        return false;
    }

    // Hardware .R## (R.SEQ): 1,024 B Rhythm Setup header @ 0x0000 + $3800 Rhythm RAM @ 0x0400
    if (sizeBytes >= 1024u + 0x1E2u
        && applyHardwareRhythmRamBlock(data + 1024u, sizeBytes - 1024u, data, mem, optRhythmSongs)) {
        return true;
    }

    // Raw headerless $3800 Rhythm RAM image @ 0x0000
    if (sizeBytes >= 0x1E2u
        && applyHardwareRhythmRamBlock(data, sizeBytes, nullptr, mem, optRhythmSongs)) {
        return true;
    }

    return false;
}

namespace {

[[nodiscard]] std::string fileStemNoExt(std::string_view fileName) {
    const auto slashPos = fileName.find_last_of("/\\");
    const std::string_view base = (slashPos == std::string_view::npos) ? fileName : fileName.substr(slashPos + 1u);
    const auto dotPos = base.find_last_of('.');
    const std::string_view stem = (dotPos == std::string_view::npos) ? base : base.substr(0u, dotPos);
    return trimRightSpaces(stem);
}

[[nodiscard]] bool isPrintableNonBlank10(const uint8_t* p) noexcept {
    bool hasNonSpace = false;
    for (std::size_t c = 0u; c < 10u; ++c) {
        if (p[c] < 0x20u || p[c] > 0x7Eu) {
            return false;
        }
        if (p[c] != 0x20u) {
            hasNonSpace = true;
        }
    }
    return hasNonSpace;
}

[[nodiscard]] bool hasAnyNonZeroBytes(const uint8_t* p, std::size_t count) noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
        if (p[i] != 0u) {
            return true;
        }
    }
    return false;
}

// Unpacks a 128-byte hardware VF50 floppy Voice record into the canonical 95-byte ROM format:
// Exact IC712 Bank 0 $BAF0..$BB05 layout: 88B base (+0..+87), 6B pad (+88..+93), 7B DSP (+94..+100), 27B pad
inline void extractHwFloppyVoice128ToRom95(const uint8_t* rec128, std::array<uint8_t, 95>& outRom95) noexcept {
    std::memcpy(outRom95.data(), rec128, 88u);
    std::memcpy(outRom95.data() + 88u, rec128 + 94u, 7u);
}

[[nodiscard]] bool parseSingleVF50SeqV1Chunk(const uint8_t* buf,
                                            std::size_t len,
                                            vf50::dsp::CustomSeqSongBuffer& outSeq,
                                            std::size_t& outConsumedBytes) noexcept {
    outConsumedBytes = 0u;
    if (buf == nullptr || len < 35u || !isHwSeqHeaderMagic(buf)) {
        return false;
    }

    outSeq.clear();
    decodeTicksPerBarFromVF50Ts(buf[9], outSeq.timeSigNum, outSeq.timeSigDen, outSeq.ticksPerBar);
    const uint16_t rawBpm16 = static_cast<uint16_t>((static_cast<uint16_t>(buf[10]) << 8u) | buf[11]);
    outSeq.initialTempoBpm  = static_cast<uint16_t>(std::clamp<int>(static_cast<int>(rawBpm16), 30, 300));
    const juce::String cleanTitle = VF50DiskManager::sanitizeAsciiString(buf + 12u, 8);
    outSeq.setTitle(cleanTitle.toStdString());

    std::array<uint8_t, 8> trackChannels{};
    for (std::size_t t = 0u; t < 8u; ++t) {
        trackChannels[t] = buf[20u + t] & 0x0Fu;
    }
    outSeq.trackChannels    = trackChannels;
    outSeq.playMode         = (buf[28] == 0u) ? 0u : 1u;
    outSeq.perfPresetBank   = (buf[29] >= 2u) ? 1u : 0u;
    outSeq.perfNumber       = std::min<uint8_t>(buf[30] & 0x7Fu, 99u);
    outSeq.rhythmSyncIsSong = (buf[31] & 0x01u) != 0u;
    outSeq.rhythmTargetIdx  = buf[32];
    outSeq.hasRhythmLink    = true;

    struct StagedEv {
        uint32_t tick;
        uint8_t  prio;
        vf50::generated::DemoEventType type;
        uint8_t  ch;
        uint8_t  d1;
        uint8_t  d2;
    };

    std::vector<StagedEv> staged;
    staged.reserve(4096u);

    std::size_t pos    = 34u;
    uint8_t curTrk     = 0u;
    uint8_t curCh      = trackChannels[0];
    uint32_t curTick   = 0u;
    uint32_t maxTick   = 0u;
    uint16_t curBars   = 0u;
    uint16_t maxBars   = 1u;
    uint8_t pendBank   = 1u;

    while (pos < len) {
        const uint8_t b = buf[pos++];
        if (b == 0xF9u) {
            break;
        } else if (b == 0x00u || b == 0xF8u) {
            continue;
        } else if (b <= 0x7Fu) {
            uint32_t dt = 0u;
            if (pos < len && buf[pos] <= 0x7Fu) {
                const uint8_t b2 = buf[pos++];
                dt = ((static_cast<uint32_t>(b) << 7u) | static_cast<uint32_t>(b2)) >> 1u;
            } else {
                dt = (b == 0x01u) ? 0u : static_cast<uint32_t>(b >> 1u);
            }
            curTick += dt;
            maxTick = std::max(maxTick, curTick);
        } else if (b == 0xF0u) {
            if (pos >= len) break;
            curTrk   = buf[pos++] & 0x07u;
            curCh    = trackChannels[curTrk];
            curTick  = 0u;
            curBars  = 0u;
            pendBank = 1u;
        } else if (b == 0xF2u) {
            maxTick = std::max(maxTick, curTick);
            maxBars = std::max(maxBars, curBars);
        } else if (b == 0xF5u) {
            ++curBars;
        } else if (b >= 0x80u && b <= 0xDFu) {
            uint32_t gate = 0u;
            if (b <= 0xBFu) {
                gate = static_cast<uint32_t>(b & 0x3Fu);
            } else {
                if (pos >= len) break;
                const uint8_t b2 = buf[pos++];
                gate = ((static_cast<uint32_t>(b & 0x3Fu) << 7u) | static_cast<uint32_t>(b2 & 0x7Fu));
            }
            if (pos >= len) break;
            const uint8_t nb = buf[pos++];
            uint8_t note = nb & 0x7Fu;
            uint8_t vel  = 64u;
            if ((nb & 0x80u) == 0u) {
                if (pos >= len) break;
                vel = buf[pos++] & 0x7Fu;
            }
            const uint32_t effGate = std::max<uint32_t>(1u, gate);
            if (vel > 0u && staged.size() + 2u <= vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 3u, vf50::generated::DemoEventType::FmNoteOn, curCh, note, vel });
                staged.push_back({ curTick + effGate, 0u, vf50::generated::DemoEventType::FmNoteOff, curCh, note, 0u });
                maxTick = std::max(maxTick, curTick + effGate);
            }
        } else if (b >= 0xE0u && b <= 0xEFu) {
            continue;
        } else if (b == 0xFAu) {
            if (pos + 1u >= len) break;
            const uint8_t val = buf[pos + 1u] & 0x7Fu;
            pos += 2u;
            if (staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmAftertouch, curCh, val, 0u });
            }
        } else if (b == 0xFBu) {
            if (pos + 1u >= len) break;
            const uint8_t cc  = buf[pos] & 0x7Fu;
            const uint8_t val = buf[pos + 1u] & 0x7Fu;
            pos += 2u;
            if (staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmControl, curCh, cc, val });
            }
        } else if (b == 0xFCu) {
            if (pos >= len) break;
            const uint8_t prog = buf[pos++] & 0x7Fu;
            if (prog == 120u) pendBank = 0u;
            else if (prog == 121u) pendBank = 1u;
            else if (prog < 100u && staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 1u, vf50::generated::DemoEventType::FmProgram, curCh, pendBank, prog });
            }
        } else if (b == 0xFDu) {
            if (pos >= len) break;
            const uint8_t val = buf[pos++] & 0x7Fu;
            if (staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmAftertouch, curCh, val, 0u });
            }
        } else if (b == 0xFEu) {
            if (pos + 1u >= len) break;
            const uint8_t lsb = buf[pos] & 0x7Fu;
            const uint8_t msb = buf[pos + 1u] & 0x7Fu;
            pos += 2u;
            if (staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmPitchBend, curCh, lsb, msb });
            }
        } else if (b == 0xF6u) {
            if (pos + 1u >= len) break;
            const uint8_t bpm = buf[pos + 1u];
            pos += 2u;
            if (bpm >= 30u && bpm <= 240u && staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::TempoChange, 0u, 0u, bpm });
            }
        } else if (b == 0xF1u || b == 0xF3u || b == 0xF7u) {
            if (pos >= len) break;
            ++pos;
        } else if (b == 0xF4u) {
            if (pos + 1u >= len) break;
            pos += 2u;
        } else if (b == 0xFFu) {
            while (pos < len && buf[pos] != 0xFFu && buf[pos] != 0xF7u) {
                ++pos;
            }
            if (pos < len) {
                ++pos;
            }
        } else {
            break;
        }
    }

    outConsumedBytes = pos;
    if (staged.empty()) {
        return false;
    }

    std::stable_sort(staged.begin(), staged.end(), [](const StagedEv& a, const StagedEv& b) noexcept {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.prio < b.prio;
    });

    const std::size_t copyCnt = std::min<std::size_t>(staged.size(), vf50::dsp::CustomSeqSongBuffer::kMaxEvents);
    for (std::size_t i = 0u; i < copyCnt; ++i) {
        outSeq.events[i] = { staged[i].tick, staged[i].type, staged[i].ch, staged[i].d1, staged[i].d2 };
    }
    outSeq.eventCount = static_cast<uint32_t>(copyCnt);
    outSeq.totalTicks = std::max<uint32_t>(outSeq.ticksPerBar, maxTick);
    outSeq.totalBars  = std::max<uint16_t>(maxBars, static_cast<uint16_t>((outSeq.totalTicks + outSeq.ticksPerBar - 1u) / outSeq.ticksPerBar));
    outSeq.valid      = true;
    return true;
}

void ensureSongTitleFromFileName(vf50::dsp::CustomSeqSongBuffer& seq, std::string_view fileName) noexcept {
    const std::string_view cur = seq.title();
    if (cur.empty() || cur == "NewSong" || cur == "NEWSONG" || cur == "DISK SEQ") {
        const std::string stem = fileStemNoExt(fileName);
        if (!stem.empty()) {
            seq.setTitle(stem);
        }
    }
}

} // namespace

bool VF50DiskManager::parseVF50SeqV1(const uint8_t* data,
                                   std::size_t sizeBytes,
                                   vf50::dsp::CustomSeqSongBuffer& outSeq) noexcept {
    if (data == nullptr || sizeBytes < 35u) {
        return false;
    }

    // Hardware VF50 ALL (.V##) files save Songs 7..0 in reverse order on 1KB boundaries,
    // so Songs 7..1 are often empty 35-byte HardwareSeqV1 stubs while Song 0 holds the active sequence!
    // Scan every HardwareSeqV1 chunk and return the richest non-empty song.
    bool foundAny = false;
    std::unique_ptr<vf50::dsp::CustomSeqSongBuffer> cand = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();

    std::size_t pos = 0u;
    while (pos + 35u <= sizeBytes) {
        if (isHwSeqHeaderMagic(data + pos)) {
            std::size_t consumed = 0u;
            if (parseSingleVF50SeqV1Chunk(data + pos, sizeBytes - pos, *cand, consumed)) {
                if (!foundAny || cand->eventCount > outSeq.eventCount) {
                    outSeq   = *cand;
                    foundAny = true;
                }
            }
            pos += std::max<std::size_t>(consumed, 8u);
        } else {
            ++pos;
        }
    }
    return foundAny;
}

int VF50DiskManager::parseAllVF50SeqV1Songs(
    const uint8_t* data,
    std::size_t sizeBytes,
    std::array<std::unique_ptr<vf50::dsp::CustomSeqSongBuffer>, 8>& outSlots,
    std::string_view fallbackFileName) noexcept {
    for (auto& s : outSlots) {
        if (s != nullptr) {
            s->clear();
        }
    }
    if (data == nullptr || sizeBytes < 35u) {
        return 0;
    }

    struct RawChunk {
        bool populated = false;
        std::unique_ptr<vf50::dsp::CustomSeqSongBuffer> buf;
    };
    std::vector<RawChunk> chunks;
    chunks.reserve(8u);

    std::size_t pos = 0u;
    while (pos + 35u <= sizeBytes && chunks.size() < 16u) {
        if (isHwSeqHeaderMagic(data + pos)) {
            auto cand = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
            std::size_t consumed = 0u;
            const bool ok = parseSingleVF50SeqV1Chunk(data + pos, sizeBytes - pos, *cand, consumed);
            chunks.push_back(RawChunk{ ok, ok ? std::move(cand) : nullptr });
            pos += std::max<std::size_t>(consumed, 8u);
        } else {
            ++pos;
        }
    }

    int populatedCount = 0;
    if (chunks.size() == 8u) {
        // Authentic hardware VF50 ALL (.V##) dump: 8 consecutive HardwareSeqV1 chunks in reverse order (Song #8..#1 -> slots 7..0)
        for (std::size_t ci = 0u; ci < 8u; ++ci) {
            const std::size_t slotIdx = 7u - ci;
            if (chunks[ci].populated && chunks[ci].buf != nullptr) {
                outSlots[slotIdx] = std::move(chunks[ci].buf);
                ++populatedCount;
            }
        }
    } else {
        std::size_t nextSlot = 0u;
        for (auto& ch : chunks) {
            if (ch.populated && ch.buf != nullptr && nextSlot < 8u) {
                outSlots[nextSlot++] = std::move(ch.buf);
                ++populatedCount;
            }
        }
    }

    if (populatedCount > 0 && !fallbackFileName.empty()) {
        for (std::size_t s = 0u; s < 8u; ++s) {
            if (outSlots[s] != nullptr && outSlots[s]->valid && outSlots[s]->eventCount > 0u) {
                if (populatedCount == 1 || s == 0u) {
                    ensureSongTitleFromFileName(*outSlots[s], fallbackFileName);
                }
            }
        }
    }
    return populatedCount;
}

bool VF50DiskManager::parseQx5fdSequence(const uint8_t* data,
                                        std::size_t sizeBytes,
                                        vf50::dsp::CustomSeqSongBuffer& outSeq) noexcept {
    // QX5FD compatible .A## disk format (verified from IC723 Bank 9 $8285..$851A)
    if (data == nullptr || sizeBytes < 0x0220u || std::memcmp(data, "QX5FD ", 6u) != 0) {
        return false;
    }

    outSeq.clear();
    outSeq.timeSigNum      = 4u;
    outSeq.timeSigDen      = 4u;
    outSeq.ticksPerBar     = 192u;
    outSeq.initialTempoBpm = static_cast<uint8_t>(std::clamp<int>(data[0x20], 30, 240));
    outSeq.playMode        = 1u;
    outSeq.perfPresetBank  = 1u;
    outSeq.perfNumber      = 0u;
    outSeq.trackChannels   = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };

    if (isPrintableNonBlank10(data + 0x08u)) {
        const juce::String cleanTitle = VF50DiskManager::sanitizeAsciiString(data + 0x08u, 8);
        outSeq.setTitle(cleanTitle.toStdString());
    }

    struct StagedEv {
        uint32_t tick;
        uint8_t  prio;
        vf50::generated::DemoEventType type;
        uint8_t  ch;
        uint8_t  d1;
        uint8_t  d2;
    };
    std::vector<StagedEv> staged;
    staged.reserve(4096u);

    std::size_t pos  = 0x0214u;
    uint8_t curTrk   = 0u;
    uint32_t curTick = 0u;
    uint32_t maxTick = 0u;
    uint16_t curBars = 0u;
    uint16_t maxBars = 1u;

    while (pos < sizeBytes) {
        const uint8_t b = data[pos++];
        if (b == 0xF0u) {
            if (pos >= sizeBytes) break;
            const uint8_t rawTrk = data[pos++] & 0x0Fu;
            if (rawTrk >= 1u && rawTrk <= 8u) {
                curTrk = static_cast<uint8_t>(rawTrk - 1u);
            } else {
                curTrk = rawTrk & 0x07u;
            }
            curTick = 0u;
            curBars = 0u;
        } else if (b == 0xF2u) {
            maxTick = std::max(maxTick, curTick);
            maxBars = std::max(maxBars, curBars);
            if (pos >= sizeBytes || data[pos] != 0xF0u) {
                break;
            }
        } else if (b == 0x00u || b == 0xE0u || b == 0xF8u) {
            continue;
        } else if (b <= 0x7Fu) {
            uint32_t dt = 0u;
            if (pos < sizeBytes && data[pos] <= 0x7Fu) {
                const uint8_t b2 = data[pos++];
                dt = ((static_cast<uint32_t>(b) << 7u) | static_cast<uint32_t>(b2)) >> 1u;
            } else {
                dt = (b == 0x01u) ? 0u : static_cast<uint32_t>(b >> 1u);
            }
            curTick += dt;
            maxTick = std::max(maxTick, curTick);
        } else if (b == 0xF5u) {
            ++curBars;
        } else if (b >= 0x80u && b <= 0xDFu) {
            uint32_t gate = 0u;
            if (b <= 0xBFu) {
                gate = static_cast<uint32_t>(b & 0x3Fu);
            } else {
                if (pos >= sizeBytes) break;
                const uint8_t b2 = data[pos++];
                gate = ((static_cast<uint32_t>(b & 0x3Fu) << 7u) | static_cast<uint32_t>(b2 & 0x7Fu));
            }
            if (pos >= sizeBytes) break;
            const uint8_t nb = data[pos++];
            const uint8_t note = nb & 0x7Fu;
            uint8_t vel = 64u;
            if ((nb & 0x80u) == 0u) {
                if (pos >= sizeBytes) break;
                vel = data[pos++] & 0x7Fu;
            }
            const uint32_t effGate = std::max<uint32_t>(1u, gate);
            if (vel > 0u && staged.size() + 2u <= vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 3u, vf50::generated::DemoEventType::FmNoteOn, curTrk, note, vel });
                staged.push_back({ curTick + effGate, 0u, vf50::generated::DemoEventType::FmNoteOff, curTrk, note, 0u });
                maxTick = std::max(maxTick, curTick + effGate);
            }
        } else if (b == 0xFBu) {
            if (pos + 1u >= sizeBytes) break;
            const uint8_t cc  = data[pos] & 0x7Fu;
            const uint8_t val = data[pos + 1u] & 0x7Fu;
            pos += 2u;
            if (staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmControl, curTrk, cc, val });
            }
        } else if (b == 0xFCu) {
            if (pos >= sizeBytes) break;
            const uint8_t prog = data[pos++] & 0x7Fu;
            if (prog < 100u && staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 1u, vf50::generated::DemoEventType::FmProgram, curTrk, 1u, prog });
            }
        } else if (b == 0xFDu) {
            if (pos >= sizeBytes) break;
            const uint8_t val = data[pos++] & 0x7Fu;
            if (staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmAftertouch, curTrk, val, 0u });
            }
        } else if (b == 0xFEu) {
            if (pos + 1u >= sizeBytes) break;
            const uint8_t lsb = data[pos] & 0x7Fu;
            const uint8_t msb = data[pos + 1u] & 0x7Fu;
            pos += 2u;
            if (staged.size() < vf50::dsp::CustomSeqSongBuffer::kMaxEvents) {
                staged.push_back({ curTick, 2u, vf50::generated::DemoEventType::FmPitchBend, curTrk, lsb, msb });
            }
        } else if (b == 0xF1u || b == 0xF3u || b == 0xF7u || b == 0xF9u) {
            if (pos >= sizeBytes) break;
            ++pos;
        } else if (b == 0xF4u || b == 0xF6u || b == 0xFAu) {
            if (pos + 1u >= sizeBytes) break;
            pos += 2u;
        } else if (b == 0xFFu) {
            while (pos < sizeBytes && data[pos] != 0xFFu) {
                ++pos;
            }
            if (pos < sizeBytes) {
                ++pos;
            }
        } else {
            break;
        }
    }

    if (staged.empty()) {
        return false;
    }

    std::stable_sort(staged.begin(), staged.end(), [](const StagedEv& a, const StagedEv& b) noexcept {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.prio < b.prio;
    });

    const std::size_t copyCnt = std::min<std::size_t>(staged.size(), vf50::dsp::CustomSeqSongBuffer::kMaxEvents);
    for (std::size_t i = 0u; i < copyCnt; ++i) {
        outSeq.events[i] = { staged[i].tick, staged[i].type, staged[i].ch, staged[i].d1, staged[i].d2 };
    }
    outSeq.eventCount = static_cast<uint32_t>(copyCnt);
    outSeq.totalTicks = std::max<uint32_t>(outSeq.ticksPerBar, maxTick);
    outSeq.totalBars  = std::max<uint16_t>(maxBars, static_cast<uint16_t>((outSeq.totalTicks + outSeq.ticksPerBar - 1u) / outSeq.ticksPerBar));
    outSeq.valid      = true;
    return true;
}

bool VF50DiskManager::applyCardBankBytes(const uint8_t* data,
                                        std::size_t sizeBytes,
                                        vf50::state::VirtualCardBank& dstBank) noexcept {
    if (data == nullptr || sizeBytes < 95u) {
        return false;
    }

    auto unpackCardVoices95 = [&dstBank](const uint8_t* src, std::size_t len) noexcept -> bool {
        if (src == nullptr || len < 95u) return false;
        std::size_t off = 0u;
        if (len >= 8u + 95u && std::memcmp(src, kMagicVoi, 8u) == 0) {
            off = 8u;
        }
        const std::size_t rem = len - off;
        if (rem < 95u || (rem % 95u) != 0u || (rem / 95u) > 100u) {
            return false;
        }
        const std::size_t count = rem / 95u;
        for (std::size_t i = 0u; i < count; ++i) {
            std::array<uint8_t, 95> raw{};
            std::memcpy(raw.data(), src + off + i * 95u, 95u);
            dstBank.voices[i].unpackFromRom95(raw);
        }
        dstBank.format         = vf50::state::CardBankFormat::VF50Syn;
        dstBank.hasVoicesPerfs = true;
        return true;
    };

    auto unpackCardPerfs97 = [&dstBank](const uint8_t* src, std::size_t len) noexcept -> bool {
        if (src == nullptr || len < 97u) return false;
        if (len >= 8u + 100u * 97u && std::memcmp(src, kMagicPfm, 8u) == 0) {
            const uint8_t* pfBase   = src + 8u;
            const bool hasPans      = (len >= 8u + 100u * 97u + 800u);
            const uint8_t* pansBase = hasPans ? (pfBase + 100u * 97u) : nullptr;
            for (std::size_t i = 0u; i < 100u; ++i) {
                std::array<uint8_t, 97> raw{};
                std::memcpy(raw.data(), pfBase + i * 97u, 97u);
                std::array<int8_t, 8> pans{};
                if (pansBase != nullptr) {
                    for (std::size_t p = 0u; p < 8u; ++p) {
                        pans[p] = static_cast<int8_t>(std::clamp<int>(static_cast<int8_t>(pansBase[i * 8u + p]), -64, 63));
                    }
                }
                dstBank.performances[i].unpackFromRom97(raw, pans);
            }
            dstBank.format         = vf50::state::CardBankFormat::VF50Syn;
            dstBank.hasVoicesPerfs = true;
            return true;
        }
        return false;
    };

    // 1. VF50ALV1 archive -> extract voices & performances directly into target card bank
    if (sizeBytes >= 24u && std::memcmp(data, kMagicAll, 8u) == 0) {
        const uint32_t memLen = readU32LE(data + 8u);
        const uint32_t voiLen = readU32LE(data + 12u);
        const uint32_t pfmLen = readU32LE(data + 16u);
        const uint32_t rhyLen = readU32LE(data + 20u);
        const std::size_t archiveEnd = 24u + static_cast<std::size_t>(memLen) + voiLen + pfmLen + rhyLen;
        if (archiveEnd <= sizeBytes) {
            const uint8_t* voiPtr = data + 24u + memLen;
            const uint8_t* pfmPtr = voiPtr + voiLen;
            const bool okV = unpackCardVoices95(voiPtr, voiLen);
            const bool okP = unpackCardPerfs97(pfmPtr, pfmLen);
            return okV || okP;
        }
        return false;
    }

    // 2. Native VF50VOV1 / VF50PFV1
    if (unpackCardVoices95(data, sizeBytes)) return true;
    if (unpackCardPerfs97(data, sizeBytes))  return true;

    // 3. Hardware 26,624-byte (0x6800) .C## / .I## layout: 1KB hdr + 100x128B Perfs @ 0x0400 + 100x128B Voices @ 0x3600
    if (!hasKnownVF50BinaryMagic(data, sizeBytes) && sizeBytes >= 0x0400u + 128u) {
        bool loadedAny = false;
        const std::size_t availPerfs = std::min<std::size_t>(100u, (sizeBytes - 0x0400u) / 128u);
        std::size_t validPerfs = 0u;
        for (std::size_t p = 0u; p < std::min<std::size_t>(availPerfs, 4u); ++p) {
            if (isPrintableNonBlank10(data + 0x0400u + p * 128u + 66u)) {
                ++validPerfs;
            }
        }
        if (validPerfs > 0u) {
            for (std::size_t i = 0u; i < availPerfs; ++i) {
                const uint8_t* rec = data + 0x0400u + i * 128u;
                if (!isPrintableNonBlank10(rec + 66u)) continue;
                std::array<uint8_t, 97> rawPerf{};
                std::memcpy(rawPerf.data(), rec, 97u);
                dstBank.performances[i].unpackFromRom97(rawPerf);
                loadedAny = true;
            }
            constexpr std::size_t kVoiceOff = 0x3600u;
            if (sizeBytes >= kVoiceOff + 128u) {
                const std::size_t availVoices = std::min<std::size_t>(100u, (sizeBytes - kVoiceOff) / 128u);
                for (std::size_t i = 0u; i < availVoices; ++i) {
                    const uint8_t* rec = data + kVoiceOff + i * 128u;
                    if (!isPrintableNonBlank10(rec + 57u) || !hasAnyNonZeroBytes(rec, 88u)) continue;
                    std::array<uint8_t, 95> rawVoice{};
                    extractHwFloppyVoice128ToRom95(rec, rawVoice);
                    dstBank.voices[i].unpackFromRom95(rawVoice);
                    loadedAny = true;
                }
            }
        }
        if (loadedAny) {
            dstBank.format         = vf50::state::CardBankFormat::VF50Syn;
            dstBank.hasVoicesPerfs = true;
            return true;
        }
    }

    return false;
}

bool VF50DiskManager::applyHardwareVF50FloppyAllBytes(const uint8_t* data,
                                                    std::size_t sizeBytes,
                                                    VF50LoadFilter filter,
                                                    vf50::state::VF50MemoryManager& mem,
                                                    vf50::state::VF50SystemSettings* optSysSettings,
                                                    std::array<vf50::generated::RhythmSongData, 8>* optRhythmSongs,
                                                    bool& outLoadedVoices,
                                                    bool& outLoadedPerfs,
                                                    bool& outLoadedRhythm) noexcept {
    outLoadedVoices = false;
    outLoadedPerfs  = false;
    outLoadedRhythm = false;
    if (data == nullptr || sizeBytes < 128u || hasKnownVF50BinaryMagic(data, sizeBytes)
        || (sizeBytes >= 6u && std::memcmp(data, "QX5FD ", 6u) == 0)) {
        return false;
    }

    const bool allowVoices = (filter == VF50LoadFilter::AllData
                           || filter == VF50LoadFilter::AutoByExtension
                           || filter == VF50LoadFilter::SynthOnly
                           || filter == VF50LoadFilter::VoiceOnly);
    const bool allowPerfs  = (filter == VF50LoadFilter::AllData
                           || filter == VF50LoadFilter::AutoByExtension
                           || filter == VF50LoadFilter::SynthOnly
                           || filter == VF50LoadFilter::PerfOnly);
    const bool allowRhythm = (filter == VF50LoadFilter::AllData
                           || filter == VF50LoadFilter::AutoByExtension
                           || filter == VF50LoadFilter::RhythmOnly);

    auto unpackVoiceSection128 = [&mem](const uint8_t* basePtr, std::size_t maxSlots) noexcept -> bool {
        if (maxSlots == 0u) return false;
        std::size_t validRom95Count = 0u;
        std::size_t validVmemCount  = 0u;
        const std::size_t probe = std::min<std::size_t>(maxSlots, 8u);
        for (std::size_t i = 0u; i < probe; ++i) {
            const uint8_t* rec = basePtr + i * 128u;
            if (isPrintableNonBlank10(rec + 57u)) ++validRom95Count;
            if (isPrintableNonBlank10(rec + 118u)) ++validVmemCount;
        }

        if (validRom95Count > 0u) {
            vf50::state::StoredVoiceSlot vSlot{};
            bool anyLoaded = false;
            for (std::size_t i = 0u; i < maxSlots; ++i) {
                const uint8_t* rec = basePtr + i * 128u;
                // Only unpack populated voice records so unrelated INTERNAL RAM slots stay intact
                if (!isPrintableNonBlank10(rec + 57u) || !hasAnyNonZeroBytes(rec, 88u)) {
                    continue;
                }
                std::array<uint8_t, 95> rawVoice{};
                extractHwFloppyVoice128ToRom95(rec, rawVoice);
                vSlot.unpackFromRom95(rawVoice);
                mem.storeUserVoice(i, vSlot.voice);
                anyLoaded = true;
            }
            return anyLoaded;
        }

        if (validVmemCount > 0u) {
            auto scratchSynth = std::make_unique<vf50::dsp::VF50SynthEngine>();
            vf50::midi::VF50SysExHandler sx{};
            std::array<uint8_t, 136> vmem1Packet{};
            vmem1Packet[0] = 0xF0u;
            vmem1Packet[1] = 0x43u;
            vmem1Packet[2] = 0x00u;
            vmem1Packet[3] = 0x24u;
            vmem1Packet[4] = 0x01u;
            vmem1Packet[5] = 0x00u;
            vmem1Packet[135] = 0xF7u;
            bool anyLoaded = false;
            for (std::size_t i = 0u; i < maxSlots; ++i) {
                const uint8_t* rec = basePtr + i * 128u;
                if (!isPrintableNonBlank10(rec + 118u)) {
                    continue;
                }
                std::memcpy(&vmem1Packet[6], rec, 128u);
                vmem1Packet[134] = vf50::midi::VF50SysExHandler::computeSysExChecksum(&vmem1Packet[6], 128u);
                (void)sx.processSysEx(vmem1Packet.data(), vmem1Packet.size(), mem, *scratchSynth, nullptr);
                mem.storeUserVoice(i, mem.singleEditSlot().voice);
                anyLoaded = true;
            }
            return anyLoaded;
        }
        return false;
    };

    // Case A: Standard VF50 hardware SYN (.I##) / ALL (.V##) file layout (IC712 Bank 0 $B751/$B7E7):
    //   0x0000..0x03FF (1,024 B): Synth Setup / Greeting / Microtuning / Perf-FX / PCT header
    //   0x0400..0x35FF (12,800 B): 100 x 128 B Performances (97B ROM + 31B pad; name @ +66..+75)
    //   0x3600..0x67FF (12,800 B): 100 x 128 B Voices (88B + 6B pad + 7B DSP + 27B pad; name @ +57..+66)
    //   0x6800..       (R.SEQ)   : 1,024 B Rhythm Setup + $3800 Rhythm Pattern/Song RAM
    bool hasPerfHeaderBlock = false;
    if (sizeBytes >= 1024u + 128u) {
        const std::size_t availPerfSlots = std::min<std::size_t>(100u, (sizeBytes - 1024u) / 128u);
        const std::size_t probeCount     = std::min<std::size_t>(availPerfSlots, 4u);
        std::size_t validPerfs           = 0u;
        for (std::size_t p = 0u; p < probeCount; ++p) {
            if (isPrintableNonBlank10(data + 1024u + p * 128u + 66u)) {
                ++validPerfs;
            }
        }
        hasPerfHeaderBlock = (validPerfs >= std::min<std::size_t>(probeCount, 2u)) && (validPerfs > 0u);

        if (hasPerfHeaderBlock) {
            // Apply Synth Setup 40-char power-on greeting (0x2B..0x3A + 0x4B..0x62) when loading SYN or ALL
            if ((allowVoices || allowPerfs) && optSysSettings != nullptr) {
                std::array<char, 40> greeting{};
                bool allPrintable = true;
                bool anyNonSpace  = false;
                for (std::size_t i = 0u; i < 16u; ++i) {
                    const uint8_t b = data[0x2Bu + i];
                    if (b < 0x20u || b > 0x7Eu) { allPrintable = false; break; }
                    if (b != 0x20u) anyNonSpace = true;
                    greeting[i] = static_cast<char>(b);
                }
                if (allPrintable) {
                    for (std::size_t i = 0u; i < 24u; ++i) {
                        const uint8_t b = data[0x4Bu + i];
                        if (b < 0x20u || b > 0x7Eu) { allPrintable = false; break; }
                        if (b != 0x20u) anyNonSpace = true;
                        greeting[16u + i] = static_cast<char>(b);
                    }
                }
                if (allPrintable && anyNonSpace) {
                    optSysSettings->powerOnMessage = greeting;
                }
            }

            if (allowPerfs) {
                vf50::state::StoredPerformanceSlot perfSlot{};
                for (std::size_t i = 0u; i < availPerfSlots; ++i) {
                    const uint8_t* rec = data + 1024u + i * 128u;
                    if (!isPrintableNonBlank10(rec + 66u)) {
                        continue;
                    }
                    std::array<uint8_t, 97> rawPerf{};
                    std::memcpy(rawPerf.data(), rec, 97u);
                    perfSlot.unpackFromRom97(rawPerf);
                    mem.storeUserPerformance(i, perfSlot.perf);
                    outLoadedPerfs = true;
                }
            }

            constexpr std::size_t kVoiceSectionOffset = 1024u + 100u * 128u; // 0x3600 = 13824
            if (allowVoices && sizeBytes >= kVoiceSectionOffset + 128u) {
                const std::size_t availVoiceSlots = std::min<std::size_t>(100u, (sizeBytes - kVoiceSectionOffset) / 128u);
                outLoadedVoices = unpackVoiceSection128(data + kVoiceSectionOffset, availVoiceSlots);
            }

            constexpr std::size_t kRhythmChunkOffset = 1024u + 200u * 128u; // 0x6800 = 26624
            if (allowRhythm && sizeBytes >= kRhythmChunkOffset + 1024u + 0x1E2u) {
                outLoadedRhythm = applyHardwareRhythmRamBlock(
                    data + kRhythmChunkOffset + 1024u,
                    sizeBytes - (kRhythmChunkOffset + 1024u),
                    data + kRhythmChunkOffset,
                    mem,
                    optRhythmSongs);
            }
        }
    }

    // Detect standalone hardware .R## (R.SEQ) BEFORE probing Case B so Rhythm Song names ("NewSong ")
    // at 0x0400 + 0x1A2 (slot 3 +57) never false-match as a Voice bank!
    const bool isStandaloneHwRhythm = !hasPerfHeaderBlock
        && sizeBytes >= 1024u + 0x1E2u
        && validateHardwareRhythmRam(data + 1024u, sizeBytes - 1024u, false);

    // Case B: Standalone Voice-only 128-byte padded hardware dump (at +0x0400 after 1KB header OR at +0x0000)
    if (!hasPerfHeaderBlock && !isStandaloneHwRhythm && allowVoices) {
        if (sizeBytes >= 1024u + 128u) {
            const std::size_t availSlots = std::min<std::size_t>(100u, (sizeBytes - 1024u) / 128u);
            if (unpackVoiceSection128(data + 1024u, availSlots)) {
                outLoadedVoices = true;
            }
        }
        if (!outLoadedVoices && (sizeBytes % 128u) == 0u && (sizeBytes / 128u) <= 100u) {
            if (unpackVoiceSection128(data, sizeBytes / 128u)) {
                outLoadedVoices = true;
            }
        }
    }

    // Case C: Standalone hardware .R## (R.SEQ: 1,024 B Rhythm Setup header + $3800 Rhythm RAM @ +0x0400)
    if (!outLoadedRhythm && allowRhythm && sizeBytes >= 1024u + 0x1E2u) {
        if (applyHardwareRhythmRamBlock(data + 1024u, sizeBytes - 1024u, data, mem, optRhythmSongs)) {
            outLoadedRhythm = true;
        }
    }

    return outLoadedVoices || outLoadedPerfs || outLoadedRhythm;
}

// =============================================================================
// High-Level Load / Import / Save Operations
// =============================================================================

bool VF50DiskManager::loadFile(const juce::File& file,
                             vf50::plugin::VF50AudioProcessor& processor,
                             juce::String& outStatusMessage) {
    lastOutcome_ = VF50ImportOutcome::None;
    if (file.isDirectory()) {
        const bool ok = mountDirectory(file, outStatusMessage);
        if (ok) {
            outStatusMessage = "LOADED: " + file.getFileName();
        }
        return ok;
    }
    if (!file.existsAsFile()) {
        outStatusMessage = "File does not exist";
        return false;
    }
    const auto ext = file.getFileExtension().toLowerCase();
    if (ext == ".img" || ext == ".dsk" || ext == ".flp" || ext == ".ima") {
        const bool ok = mountFat12ImageFile(file, outStatusMessage);
        if (ok) {
            outStatusMessage = "LOADED: " + file.getFileName();
        }
        return ok;
    }
    const bool ok = importExternalFile(file, processor, VF50LoadFilter::AutoByExtension, outStatusMessage);
    if (ok) {
        outStatusMessage = "LOADED: " + file.getFileName();
    }
    return ok;
}

bool VF50DiskManager::loadMountedEntry(std::size_t entryIndex,
                                      vf50::plugin::VF50AudioProcessor& processor,
                                      VF50LoadFilter filter,
                                      juce::String& outStatusMessage) {
    if (entryIndex >= entries_.size()) {
        outStatusMessage = "No file selected";
        return false;
    }

    // If browsing a local folder and the selected entry is a floppy image (.IMG/.DSK/.FLP/.IMA), mount it directly!
    if (sourceKind_ == DiskSourceKind::LocalDirectory && entries_[entryIndex].kind == VF50FileKind::DiskImage) {
        const juce::File imgFile(juce::String::fromUTF8(entries_[entryIndex].fullOrRelPath.c_str()));
        return mountFat12ImageFile(imgFile, outStatusMessage);
    }

    const std::string entryName = entries_[entryIndex].name;
    std::vector<uint8_t> raw;
    if (!readEntryBytes(entryIndex, raw, outStatusMessage)) {
        return false;
    }
    return importBytes(entryName, raw.data(), raw.size(), processor, filter, outStatusMessage);
}

bool VF50DiskManager::importExternalFile(const juce::File& file,
                                        vf50::plugin::VF50AudioProcessor& processor,
                                        VF50LoadFilter filter,
                                        juce::String& outStatusMessage) {
    if (!file.existsAsFile()) {
        outStatusMessage = "File does not exist";
        return false;
    }
    juce::MemoryBlock mb;
    if (!file.loadFileAsData(mb) || mb.getSize() == 0u) {
        outStatusMessage = "Cannot read file: " + file.getFileName();
        return false;
    }
    return importBytes(file.getFileName().toStdString(),
                       static_cast<const uint8_t*>(mb.getData()),
                       mb.getSize(),
                       processor,
                       filter,
                       outStatusMessage);
}

bool VF50DiskManager::importBytes(std::string_view fileName,
                                 const uint8_t* data,
                                 std::size_t sizeBytes,
                                 vf50::plugin::VF50AudioProcessor& processor,
                                 VF50LoadFilter filter,
                                 juce::String& outStatusMessage) {
    lastOutcome_ = VF50ImportOutcome::None;
    if (data == nullptr || sizeBytes == 0u) {
        outStatusMessage = "Empty file";
        return false;
    }

    const juce::String juceFileName = sanitizeAsciiString(reinterpret_cast<const uint8_t*>(fileName.data()),
                                                          static_cast<int>(fileName.size()));
    const VF50FileKind extKind = classifyFileKind(fileName);

    // Resolve effective subsystem filter so extension-based imports also honor strict subsystem isolation
    VF50LoadFilter effectiveFilter = filter;
    if (effectiveFilter == VF50LoadFilter::AutoByExtension) {
        switch (extKind) {
            case VF50FileKind::AllData:    effectiveFilter = VF50LoadFilter::AllData;      break;
            case VF50FileKind::VoiceBank:  effectiveFilter = VF50LoadFilter::SynthOnly;    break;
            case VF50FileKind::PerfBank:   effectiveFilter = VF50LoadFilter::PerfOnly;     break;
            case VF50FileKind::RhythmData: effectiveFilter = VF50LoadFilter::RhythmOnly;   break;
            case VF50FileKind::Sequence:   effectiveFilter = VF50LoadFilter::SequenceOnly; break;
            case VF50FileKind::CardData:   effectiveFilter = VF50LoadFilter::CardOnly;     break;
            default:                      effectiveFilter = VF50LoadFilter::AutoByExtension; break;
        }
    }

    const bool startsWithSeqMagic = (sizeBytes >= 35u && isHwSeqHeaderMagic(data))
                                 || (sizeBytes >= 0x0220u && std::memcmp(data, "QX5FD ", 6u) == 0);

    auto publishSingleSeqToSlot = [&processor, fileName](std::size_t targetSlot0To7, vf50::dsp::CustomSeqSongBuffer& seq) {
        const std::size_t safeSlot = std::min<std::size_t>(targetSlot0To7, 7u);
        ensureSongTitleFromFileName(seq, fileName);
        processor.publishUserSequenceToSlot(safeSlot, seq);
        processor.selectDemoSong(safeSlot);
    };

    // =========================================================================
    // STRICT ISOLATION 1: CARD (.C01-.C99 / .CRD) -> Active Virtual Card Bank ONLY
    //   Never modifies Internal Voices, Perfs, Rhythm, or Sequencer!
    // =========================================================================
    if (effectiveFilter == VF50LoadFilter::CardOnly) {
        if (processor.virtualCard().isReadOnlyMedia() || processor.virtualCard().hardwareWriteProtect) {
            outStatusMessage = "Card Write Protected";
            return false;
        }
        if (startsWithSeqMagic) {
            outStatusMessage = "Unsupported CARD format";
            return false;
        }
        if (applyCardBankBytes(data, sizeBytes, processor.mutableVirtualCard().activeBank())) {
            processor.publishClassicSystemStateToAudio();
            lastOutcome_     = VF50ImportOutcome::CardData;
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }
        outStatusMessage = "Unsupported CARD format";
        return false;
    }

    // =========================================================================
    // STRICT ISOLATION 2: SEQ (.A01-.A99 / .SEQ) -> Active Sequencer Slot ONLY
    //   Never modifies Internal Synth, Rhythm, or Virtual Card!
    // =========================================================================
    if (effectiveFilter == VF50LoadFilter::SequenceOnly
        || (effectiveFilter == VF50LoadFilter::AutoByExtension && startsWithSeqMagic)) {
        auto seq = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
        bool parsedSeq = false;
        if (sizeBytes >= 24u && std::memcmp(data, kMagicAll, 8u) == 0) {
            const uint32_t memLen = readU32LE(data + 8u);
            const uint32_t voiLen = readU32LE(data + 12u);
            const uint32_t pfmLen = readU32LE(data + 16u);
            const uint32_t rhyLen = readU32LE(data + 20u);
            const std::size_t archiveEnd = 24u + static_cast<std::size_t>(memLen) + voiLen + pfmLen + rhyLen;
            if (archiveEnd + 35u <= sizeBytes) {
                parsedSeq = parseVF50SeqV1(data + archiveEnd, sizeBytes - archiveEnd, *seq);
            }
        } else {
            parsedSeq = parseVF50SeqV1(data, sizeBytes, *seq) || parseQx5fdSequence(data, sizeBytes, *seq);
        }
        if (parsedSeq) {
            // Load strictly into the currently selected sequencer song slot (1..8), leaving all other slots untouched.
            publishSingleSeqToSlot(processor.activeUserSeqSlot(), *seq);
            processor.resetMixerConsoleToNominal(true);
            lastOutcome_     = VF50ImportOutcome::Sequence;
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }
        outStatusMessage = "Unsupported .SEQ format";
        return false;
    }

    processor.pullLatestAudioMemoryToHost();
    auto& mem = processor.hostMemory();

    auto finishHostSync = [&processor]() {
        processor.pushHostMemoryToAudio();
        processor.syncApvtsToActiveSingleVoice(true);
        processor.syncApvtsToActivePerformance(true);
        processor.resetMixerConsoleToNominal(true);
        processor.updateHostDisplay();
    };

    auto selectFirstPopulatedUserRhythm = [&processor, &mem]() {
        // Respect a populated User pattern or explicit Factory Preset already selected by the hardware setup header
        if (mem.selectedRhythmPatternBank() == vf50::state::MemoryBank::UserRam
            && mem.userRhythmPattern(mem.selectedRhythmPatternIndex()).eventCount > 0u) {
            vf50::params::setApvtsRawValue(processor.apvts(), vf50::params::ParamIndex::RhythmBank, 1.0f);
            vf50::params::setApvtsRawValue(processor.apvts(), vf50::params::ParamIndex::RhythmPattern,
                                          static_cast<float>(mem.selectedRhythmPatternIndex()));
            return;
        }
        for (std::size_t p = 0u; p < vf50::state::VF50MemoryManager::kRhythmPatternCount; ++p) {
            if (mem.userRhythmPattern(p).eventCount > 0u) {
                mem.selectRhythmPattern(vf50::state::MemoryBank::UserRam, p);
                vf50::params::setApvtsRawValue(processor.apvts(), vf50::params::ParamIndex::RhythmBank, 1.0f);
                vf50::params::setApvtsRawValue(processor.apvts(), vf50::params::ParamIndex::RhythmPattern, static_cast<float>(p));
                return;
            }
        }
        if (mem.selectedRhythmPatternBank() == vf50::state::MemoryBank::FactoryRom) {
            vf50::params::setApvtsRawValue(processor.apvts(), vf50::params::ParamIndex::RhythmBank, 0.0f);
            vf50::params::setApvtsRawValue(processor.apvts(), vf50::params::ParamIndex::RhythmPattern,
                                          static_cast<float>(mem.selectedRhythmPatternIndex()));
        }
    };

    auto activateLoadedVoiceBank = [&processor, &mem, this]() {
        mem.setWorkstationMode(vf50::state::WorkstationMode::SinglePlay);
        mem.selectSingleVoice(vf50::state::MemoryBank::UserRam, 0u);
        processor.pushHostMemoryToAudio();
        processor.selectSinglePresetAtomic(vf50::state::MemoryBank::UserRam, 0u, true);
        processor.updateHostDisplay();
        lastOutcome_ = VF50ImportOutcome::VoiceBank;
    };

    auto activateLoadedPerfBank = [&processor, &mem, this]() {
        mem.setWorkstationMode(vf50::state::WorkstationMode::PerformancePlay);
        mem.selectPerformance(vf50::state::MemoryBank::UserRam, 0u);
        processor.pushHostMemoryToAudio();
        processor.selectPerformancePresetAtomic(vf50::state::MemoryBank::UserRam, 0u, true);
        processor.updateHostDisplay();
        lastOutcome_ = VF50ImportOutcome::PerfBank;
    };

    // =========================================================================
    // STRICT ISOLATION 3: R.SEQ (.R01-.R99 / .RHY) -> Rhythm Patterns/Setup ONLY
    //   Never modifies Synth Voices/Perfs, Sequencer, or Virtual Card!
    // =========================================================================
    if (effectiveFilter == VF50LoadFilter::RhythmOnly) {
        if (startsWithSeqMagic) {
            outStatusMessage = "Unsupported Rhythm format";
            return false;
        }
        if (sizeBytes >= 24u && std::memcmp(data, kMagicAll, 8u) == 0) {
            const uint32_t memLen = readU32LE(data + 8u);
            const uint32_t voiLen = readU32LE(data + 12u);
            const uint32_t pfmLen = readU32LE(data + 16u);
            const uint32_t rhyLen = readU32LE(data + 20u);
            const std::size_t archiveEnd = 24u + static_cast<std::size_t>(memLen) + voiLen + pfmLen + rhyLen;
            if (archiveEnd <= sizeBytes
                && applyRhythmBytes(data + 24u + memLen + voiLen + pfmLen, rhyLen, mem, &processor.mutableHostRhythmSongs())) {
                selectFirstPopulatedUserRhythm();
                processor.publishRhythmSongsToAudio();
                finishHostSync();
                lastOutcome_     = VF50ImportOutcome::RhythmData;
                outStatusMessage = "LOADED: " + juceFileName;
                return true;
            }
            outStatusMessage = "Invalid Rhythm block";
            return false;
        }

        if (applyRhythmBytes(data, sizeBytes, mem, &processor.mutableHostRhythmSongs())) {
            selectFirstPopulatedUserRhythm();
            processor.publishRhythmSongsToAudio();
            finishHostSync();
            lastOutcome_     = VF50ImportOutcome::RhythmData;
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }

        bool hwV = false, hwP = false, hwR = false;
        if (applyHardwareVF50FloppyAllBytes(data, sizeBytes, VF50LoadFilter::RhythmOnly, mem, nullptr,
                                           &processor.mutableHostRhythmSongs(), hwV, hwP, hwR) && hwR) {
            selectFirstPopulatedUserRhythm();
            processor.publishRhythmSongsToAudio();
            finishHostSync();
            lastOutcome_     = VF50ImportOutcome::RhythmData;
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }

        outStatusMessage = "Unsupported Rhythm format";
        return false;
    }

    // =========================================================================
    // STRICT ISOLATION 4: SYN / VoiceOnly / PerfOnly (.I01-.I99 / .SYN / .VOI / .PFM)
    //   Overwrites ONLY Internal Voices (I00-I99), Perfs (PFI00-PFI99) & Synth Setup.
    //   NEVER touches Sequencer songs, Rhythm patterns/kit, or Virtual Card!
    // =========================================================================
    if (effectiveFilter == VF50LoadFilter::SynthOnly
        || effectiveFilter == VF50LoadFilter::VoiceOnly
        || effectiveFilter == VF50LoadFilter::PerfOnly) {
        if (startsWithSeqMagic) {
            outStatusMessage = "Unsupported Synth format";
            return false;
        }

        // 4a. VF50ALV1 archive -> extract ONLY Voice and/or Perf blocks
        if (sizeBytes >= 24u && std::memcmp(data, kMagicAll, 8u) == 0) {
            const uint32_t memLen = readU32LE(data + 8u);
            const uint32_t voiLen = readU32LE(data + 12u);
            const uint32_t pfmLen = readU32LE(data + 16u);
            const uint32_t rhyLen = readU32LE(data + 20u);
            const std::size_t archiveEnd = 24u + static_cast<std::size_t>(memLen) + voiLen + pfmLen + rhyLen;
            if (archiveEnd <= sizeBytes) {
                const uint8_t* voiPtr = data + 24u + memLen;
                const uint8_t* pfmPtr = voiPtr + voiLen;
                if (effectiveFilter == VF50LoadFilter::VoiceOnly) {
                    if (applyVoiceBytes(voiPtr, voiLen, mem)) {
                        activateLoadedVoiceBank();
                        outStatusMessage = "LOADED: " + juceFileName;
                        return true;
                    }
                    outStatusMessage = "Invalid Voice block";
                    return false;
                }
                if (effectiveFilter == VF50LoadFilter::PerfOnly) {
                    if (applyPerfBytes(pfmPtr, pfmLen, mem)) {
                        activateLoadedPerfBank();
                        outStatusMessage = "LOADED: " + juceFileName;
                        return true;
                    }
                    outStatusMessage = "Invalid Perf block";
                    return false;
                }
                const bool okV = applyVoiceBytes(voiPtr, voiLen, mem);
                const bool okP = applyPerfBytes(pfmPtr, pfmLen, mem);
                if (okV || okP) {
                    if (okV) activateLoadedVoiceBank();
                    else     activateLoadedPerfBank();
                    outStatusMessage = "LOADED: " + juceFileName;
                    return true;
                }
            }
            outStatusMessage = "Invalid Synth block";
            return false;
        }

        // 4b. Native Voice / Perf formats
        if (effectiveFilter != VF50LoadFilter::PerfOnly && applyVoiceBytes(data, sizeBytes, mem)) {
            activateLoadedVoiceBank();
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }
        if (effectiveFilter != VF50LoadFilter::VoiceOnly && applyPerfBytes(data, sizeBytes, mem)) {
            activateLoadedPerfBank();
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }

        // 4c. Authentic Hardware VF50 .I## / .SYN (1KB Synth Setup + 100 Perfs + 100 Voices ONLY)
        bool hwV = false, hwP = false, hwR = false;
        auto* sysPtr = (effectiveFilter == VF50LoadFilter::SynthOnly) ? &processor.mutableClassicSystemSettings() : nullptr;
        if (applyHardwareVF50FloppyAllBytes(data, sizeBytes, effectiveFilter, mem, sysPtr, nullptr, hwV, hwP, hwR)
            && (hwV || hwP)) {
            if (sysPtr != nullptr) {
                processor.publishClassicSystemStateToAudio();
            }
            if (effectiveFilter == VF50LoadFilter::PerfOnly || (!hwV && hwP)) {
                activateLoadedPerfBank();
            } else {
                activateLoadedVoiceBank();
            }
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }

        // 4d. Modern Mode SysEx synth fallback if a .SYX file was loaded with SynthOnly/VoiceOnly/PerfOnly
        if (hasSysExStartByte(data, sizeBytes, false)) {
            auto scratchSynth = std::make_unique<vf50::dsp::VF50SynthEngine>();
            vf50::midi::VF50SysExHandler sx{};
            sx.setDeviceChannel(vf50::midi::VF50SysExHandler::kDeviceOmni);
            const auto res = sx.processSysExStream(data, sizeBytes, mem, *scratchSynth, nullptr);
            if (res.accepted && !res.modifiedRhythm) {
                if (res.modifiedUserVoices) {
                    activateLoadedVoiceBank();
                } else if (res.modifiedSingleEdit) {
                    processor.pushHostMemoryToAudio();
                    processor.stageActiveSingleEditVoiceAtomic();
                    processor.updateHostDisplay();
                    lastOutcome_ = VF50ImportOutcome::SingleVoiceEdit;
                } else if (res.modifiedUserPerfs) {
                    activateLoadedPerfBank();
                } else if (res.modifiedPerfEdit) {
                    processor.pushHostMemoryToAudio();
                    processor.stageActivePerfEditAtomic();
                    processor.updateHostDisplay();
                    lastOutcome_ = VF50ImportOutcome::PerfEdit;
                } else {
                    finishHostSync();
                    lastOutcome_ = VF50ImportOutcome::VoiceBank;
                }
                outStatusMessage = "LOADED: " + juceFileName;
                return true;
            }
        }

        outStatusMessage = "Unsupported Synth format";
        return false;
    }

    // =========================================================================
    // 5. ALL (.V01-.V99 / .ALL) & MDR / SysEx / AutoByExtension
    // =========================================================================

    // 5a. VF50ALV1 multi-section archive
    if (sizeBytes >= 24u && std::memcmp(data, kMagicAll, 8u) == 0) {
        const uint32_t memLen = readU32LE(data + 8u);
        const uint32_t voiLen = readU32LE(data + 12u);
        const uint32_t pfmLen = readU32LE(data + 16u);
        const uint32_t rhyLen = readU32LE(data + 20u);
        const std::size_t archiveEnd = 24u + static_cast<std::size_t>(memLen) + voiLen + pfmLen + rhyLen;
        if (archiveEnd <= sizeBytes) {
            const uint8_t* memPtr = data + 24u;
            if (mem.deserializeFromBinary(memPtr, memLen)) {
                // Clear all 8 sequencer song slots and all 8 rhythm song slots before loading ALL
                processor.clearAllUserSequences();
                for (auto& rs : processor.mutableHostRhythmSongs()) {
                    rs.clear();
                }
                if (rhyLen > 0u) {
                    (void)applyRhythmBytes(data + 24u + memLen + voiLen + pfmLen, rhyLen, mem,
                                           &processor.mutableHostRhythmSongs());
                }
                processor.publishRhythmSongsToAudio();
                if (archiveEnd + 9u <= sizeBytes && std::memcmp(data + archiveEnd, kMagicSeq8, 8u) == 0) {
                    const uint8_t slotMask = data[archiveEnd + 8u];
                    std::size_t seqPos = archiveEnd + 9u;
                    int firstPopSlot = -1;
                    for (std::size_t s = 0u; s < 8u; ++s) {
                        if (((slotMask >> s) & 1u) != 0u) {
                            if (seqPos + 4u > sizeBytes) break;
                            const uint32_t seqLen = readU32LE(data + seqPos);
                            seqPos += 4u;
                            if (seqLen > sizeBytes - seqPos) break;
                            auto seq = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
                            if (parseVF50SeqV1(data + seqPos, seqLen, *seq)) {
                                publishSingleSeqToSlot(s, *seq);
                                if (firstPopSlot < 0) firstPopSlot = static_cast<int>(s);
                            }
                            seqPos += seqLen;
                        }
                    }
                    processor.selectDemoSong(firstPopSlot >= 0 ? static_cast<std::size_t>(firstPopSlot) : 0u);
                } else if (archiveEnd + 35u <= sizeBytes) {
                    auto seq = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
                    if (parseVF50SeqV1(data + archiveEnd, sizeBytes - archiveEnd, *seq)) {
                        publishSingleSeqToSlot(0u, *seq);
                    } else {
                        processor.selectDemoSong(0u);
                    }
                } else {
                    processor.selectDemoSong(0u);
                }
                finishHostSync();
                lastOutcome_     = VF50ImportOutcome::AllData;
                outStatusMessage = "LOADED: " + juceFileName;
                return true;
            }
            outStatusMessage = "Corrupt ALL archive";
            return false;
        }
    }

    // 5b. Universal 4-Op MIDI SysEx & SMF (.syx / .SYX / .mid / .MID / .mdr / .B##)
    std::vector<uint8_t> smfSysExBytes;
    const uint8_t* syxPtr = nullptr;
    std::size_t syxLen    = 0u;
    if (extractSysExFromSmf(data, sizeBytes, smfSysExBytes)) {
        syxPtr = smfSysExBytes.data();
        syxLen = smfSysExBytes.size();
    } else if (extKind == VF50FileKind::SysEx || hasSysExStartByte(data, sizeBytes, false)) {
        syxPtr = data;
        syxLen = sizeBytes;
    }

    if (syxPtr != nullptr && syxLen > 0u) {
        auto scratchSynth = std::make_unique<vf50::dsp::VF50SynthEngine>();
        vf50::midi::VF50SysExHandler sx{};
        sx.setDeviceChannel(vf50::midi::VF50SysExHandler::kDeviceOmni);
        const auto res = sx.processSysExStream(syxPtr, syxLen, mem, *scratchSynth, nullptr);
        if (res.accepted) {
            if (res.modifiedUserVoices) {
                activateLoadedVoiceBank();
            } else if (res.modifiedSingleEdit) {
                processor.pushHostMemoryToAudio();
                processor.stageActiveSingleEditVoiceAtomic();
                processor.resetMixerConsoleToNominal(true);
                processor.updateHostDisplay();
                lastOutcome_ = VF50ImportOutcome::SingleVoiceEdit;
            } else if (res.modifiedUserPerfs) {
                activateLoadedPerfBank();
            } else if (res.modifiedPerfEdit) {
                processor.pushHostMemoryToAudio();
                processor.stageActivePerfEditAtomic();
                processor.resetMixerConsoleToNominal(true);
                processor.updateHostDisplay();
                lastOutcome_ = VF50ImportOutcome::PerfEdit;
            } else if (res.modifiedRhythm) {
                finishHostSync();
                lastOutcome_ = VF50ImportOutcome::RhythmData;
            } else {
                finishHostSync();
                lastOutcome_ = VF50ImportOutcome::AllData;
            }
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }
        if (extKind == VF50FileKind::SysEx) {
            outStatusMessage = "No 4-Op FM SysEx found";
            return false;
        }
    }

    // 5c. Raw VF50MemoryManager binary fallback for .ALL
    if (mem.deserializeFromBinary(data, sizeBytes)) {
        finishHostSync();
        lastOutcome_     = VF50ImportOutcome::AllData;
        outStatusMessage = "LOADED: " + juceFileName;
        return true;
    }

    // 5d. Authentic Multi-Chunk Hardware VF50 Floppy .V## (ALL) Ingestion:
    //     Clears all 8 sequencer slots & all 8 rhythm song slots, then unpacks Synth Setup + 100 Perfs + 100 Voices
    //     + Rhythm Setup/RAM (Patterns + Songs 1-8) + all HardwareSeqV1 songs into their exact native slots starting at Song #1!
    std::array<vf50::generated::RhythmSongData, 8> parsedRhythmSongs{};
    for (auto& rs : parsedRhythmSongs) {
        rs.clear();
    }
    bool hwLoadedVoices = false;
    bool hwLoadedPerfs  = false;
    bool hwLoadedRhythm = false;
    const bool hwSynthOrRhyLoaded = applyHardwareVF50FloppyAllBytes(
        data, sizeBytes, VF50LoadFilter::AllData, mem, &processor.mutableClassicSystemSettings(),
        &parsedRhythmSongs, hwLoadedVoices, hwLoadedPerfs, hwLoadedRhythm);

    std::array<std::unique_ptr<vf50::dsp::CustomSeqSongBuffer>, 8> parsedSeqSlots{};
    int hwSeqCount = 0;
    if (!startsWithSeqMagic || effectiveFilter == VF50LoadFilter::AutoByExtension) {
        hwSeqCount = parseAllVF50SeqV1Songs(data, sizeBytes, parsedSeqSlots, fileName);
        if (hwSeqCount == 0) {
            auto qx = std::make_unique<vf50::dsp::CustomSeqSongBuffer>();
            if (parseQx5fdSequence(data, sizeBytes, *qx)) {
                ensureSongTitleFromFileName(*qx, fileName);
                parsedSeqSlots[0] = std::move(qx);
                hwSeqCount = 1;
            }
        }
    }

    if (hwSynthOrRhyLoaded || hwSeqCount > 0) {
        // Clear all 8 sequencer song slots & all 8 rhythm song slots before populating the ALL file
        if (effectiveFilter == VF50LoadFilter::AllData || hwSynthOrRhyLoaded) {
            processor.clearAllUserSequences();
            processor.mutableHostRhythmSongs() = parsedRhythmSongs;
            processor.publishRhythmSongsToAudio();
        }
        int firstPopSlot = -1;
        for (std::size_t s = 0u; s < 8u; ++s) {
            if (parsedSeqSlots[s] != nullptr && parsedSeqSlots[s]->valid && parsedSeqSlots[s]->eventCount > 0u) {
                processor.publishUserSequenceToSlot(s, *parsedSeqSlots[s]);
                if (firstPopSlot < 0) {
                    firstPopSlot = static_cast<int>(s);
                }
            }
        }
        if (effectiveFilter == VF50LoadFilter::AllData || hwSeqCount > 0) {
            processor.selectDemoSong(firstPopSlot >= 0 ? static_cast<std::size_t>(firstPopSlot) : 0u);
        }
        if (hwSynthOrRhyLoaded) {
            if (hwLoadedVoices || hwLoadedPerfs) {
                processor.publishClassicSystemStateToAudio();
            }
            if (hwLoadedRhythm) {
                selectFirstPopulatedUserRhythm();
            }
            if (hwLoadedRhythm && !hwLoadedVoices && !hwLoadedPerfs && hwSeqCount == 0) {
                finishHostSync();
                lastOutcome_ = VF50ImportOutcome::RhythmData;
            } else {
                finishHostSync();
                lastOutcome_ = VF50ImportOutcome::AllData;
            }
        } else {
            processor.resetMixerConsoleToNominal(true);
            lastOutcome_ = VF50ImportOutcome::Sequence;
        }
        outStatusMessage = "LOADED: " + juceFileName;
        return true;
    }

    // 5e. Auto-detect by content magic if extension was non-standard
    if (effectiveFilter == VF50LoadFilter::AutoByExtension) {
        if (applyVoiceBytes(data, sizeBytes, mem)) {
            activateLoadedVoiceBank();
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }
        if (applyPerfBytes(data, sizeBytes, mem)) {
            activateLoadedPerfBank();
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }
        if (applyRhythmBytes(data, sizeBytes, mem, &processor.mutableHostRhythmSongs())) {
            selectFirstPopulatedUserRhythm();
            processor.publishRhythmSongsToAudio();
            finishHostSync();
            lastOutcome_     = VF50ImportOutcome::RhythmData;
            outStatusMessage = "LOADED: " + juceFileName;
            return true;
        }
    }

    outStatusMessage = "Unrecognized VF50 file format";
    return false;
}

bool VF50DiskManager::saveAllToMountedSource(std::string_view fileName83,
                                            vf50::plugin::VF50AudioProcessor& processor,
                                            juce::String& outStatusMessage) {
    if (writeProtected_) {
        outStatusMessage = "Disk Write Protected";
        return false;
    }
    processor.pullLatestAudioMemoryToHost();
    std::vector<uint8_t> allBytes;
    buildAllDataBytes(processor.hostMemory(), allBytes, &processor.hostRhythmSongs(), &processor);

    if (sourceKind_ == DiskSourceKind::Fat12Image) {
        return writeFileToMountedFat12(fileName83, allBytes.data(), allBytes.size(), outStatusMessage);
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory && mountedFile_.isDirectory()) {
        std::array<uint8_t, 11> dos83{};
        formatDos83(fileName83, dos83);
        const std::string stem = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(dos83.data()), 8u));
        const std::string ext  = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(dos83.data() + 8u), 3u));
        const juce::File dst   = mountedFile_.getChildFile(juce::String(stem + "." + ext));
        if (!dst.replaceWithData(allBytes.data(), allBytes.size())) {
            outStatusMessage = "Write failed: " + dst.getFileName();
            return false;
        }
        rescanMountedSource();
        outStatusMessage = "Saved " + dst.getFileName() + " to folder";
        return true;
    }

    outStatusMessage = "Mount disk/folder or choose Save File";
    return false;
}

bool VF50DiskManager::saveToExternalFile(const juce::File& destFile,
                                        vf50::plugin::VF50AudioProcessor& processor,
                                        juce::String& outStatusMessage) {
    processor.pullLatestAudioMemoryToHost();
    const auto& mem     = processor.hostMemory();
    const std::string fname = destFile.getFileName().toStdString();
    const auto fileKind = classifyFileKind(fname);
    const std::string upperExt = toUpperAscii(destFile.getFileExtension().toStdString());

    std::vector<uint8_t> outBytes;
    if (fileKind == VF50FileKind::CardData) {
        buildCardBankBytes(processor.virtualCard().activeBank(), outBytes);
    } else if (fileKind == VF50FileKind::VoiceBank) {
        if (upperExt == ".SYN" || isHardwareExtensionForKind(fname, VF50FileKind::VoiceBank)) {
            buildHardwareSynBytes(processor, outBytes);
        } else {
            buildVoiceBankBytes(mem, outBytes);
        }
    } else if (fileKind == VF50FileKind::PerfBank) {
        buildPerfBankBytes(mem, outBytes);
    } else if (fileKind == VF50FileKind::RhythmData) {
        buildRhythmDataBytes(mem, outBytes, &processor.hostRhythmSongs());
    } else if (fileKind == VF50FileKind::SysEx) {
        vf50::midi::VF50SysExHandler sx{};
        vf50::midi::SysExReplyBuffer reply{};
        (void)sx.emitSingleVoiceBankDump100(mem, vf50::state::MemoryBank::UserRam, reply);
        reply.copyConcatenatedTo(outBytes);
        reply.clear();
        (void)sx.emitPerformanceBankDump100(mem, vf50::state::MemoryBank::UserRam, reply);
        reply.copyConcatenatedTo(outBytes);
    } else {
        buildAllDataBytes(mem, outBytes, &processor.hostRhythmSongs(), &processor);
    }

    if (!destFile.replaceWithData(outBytes.data(), outBytes.size())) {
        outStatusMessage = "Failed to save " + destFile.getFileName();
        return false;
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory && destFile.getParentDirectory() == mountedFile_) {
        rescanMountedSource();
    }

    outStatusMessage = "Saved " + destFile.getFileName();
    return true;
}

bool VF50DiskManager::deleteMountedEntry(std::size_t entryIndex, juce::String& outStatusMessage) {
    if (!isMounted()) {
        outStatusMessage = "No disk mounted";
        return false;
    }
    if (writeProtected_) {
        outStatusMessage = "Disk Write Protected";
        return false;
    }
    if (entryIndex >= entries_.size()) {
        outStatusMessage = "No file selected";
        return false;
    }

    const DiskFileEntry entry = entries_[entryIndex];
    if (sourceKind_ == DiskSourceKind::Fat12Image) {
        if (mountedFile_ != juce::File{}) {
            outStatusMessage = "Read-Only Disk Image";
            return false;
        }
        uint8_t* matchedEnt = nullptr;
        if (entry.hasFatDirOffset && static_cast<std::size_t>(entry.fatDirOffset) + 32u <= imageBytes_.size()) {
            uint8_t* direct = &imageBytes_[entry.fatDirOffset];
            if (direct[0] != 0x00u && direct[0] != 0xE5u && (direct[11] & 0x18u) == 0u) {
                matchedEnt = direct;
            }
        }
        if (matchedEnt == nullptr) {
            std::array<uint8_t, 11> target83{};
            formatDos83(entry.name, target83);
            const int maxRootSlots = static_cast<int>(geometry_.rootEntries);
            for (int i = 0; i < maxRootSlots; ++i) {
                const uint32_t off = geometry_.rootDirByteOffset + static_cast<uint32_t>(i) * 32u;
                if (static_cast<std::size_t>(off) + 32u > imageBytes_.size()) {
                    break;
                }
                uint8_t* ent = &imageBytes_[off];
                if (ent[0] == 0x00u) break;
                if (ent[0] == 0xE5u || (ent[11] & 0x18u) != 0u) continue;
                if (std::memcmp(ent, target83.data(), 11u) == 0) {
                    matchedEnt = ent;
                    break;
                }
            }
        }
        if (matchedEnt != nullptr) {
            const uint16_t startClus = readU16LE(&matchedEnt[26]);
            bool sharedWithAnotherEntry = false;
            if (startClus >= 2u) {
                for (std::size_t i = 0u; i < entries_.size(); ++i) {
                    if (i != entryIndex && entries_[i].firstCluster == startClus) {
                        sharedWithAnotherEntry = true;
                        break;
                    }
                }
            }
            if (!sharedWithAnotherEntry) {
                uint16_t c = startClus;
                uint32_t guard = 0u;
                while (c >= 2u && c < 0xFF7u && ++guard <= geometry_.totalClusters) {
                    const uint16_t next = readFat12Entry(c);
                    writeFat12Entry(c, 0x000u);
                    c = next;
                }
            }
            matchedEnt[0] = 0xE5u;
            if (mountedFile_ != juce::File{}) {
                (void)mountedFile_.replaceWithData(imageBytes_.data(), imageBytes_.size());
            }
            rescanMountedSource();
            outStatusMessage = "Deleted " + juce::String(entry.name);
            return true;
        }
        outStatusMessage = "Entry not found";
        return false;
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory && mountedFile_.isDirectory()) {
        const juce::File f(juce::String::fromUTF8(entry.fullOrRelPath.c_str()));
        if (!f.existsAsFile() || !f.deleteFile()) {
            outStatusMessage = "Delete failed";
            return false;
        }
        rescanMountedSource();
        outStatusMessage = "Deleted " + f.getFileName();
        return true;
    }

    outStatusMessage = "Unsupported source";
    return false;
}

bool VF50DiskManager::renameMountedEntry(std::size_t entryIndex,
                                        std::string_view newNameOrStem,
                                        juce::String& outStatusMessage) {
    if (!isMounted()) {
        outStatusMessage = "No disk mounted";
        return false;
    }
    if (writeProtected_) {
        outStatusMessage = "Disk Write Protected";
        return false;
    }
    if (entryIndex >= entries_.size()) {
        outStatusMessage = "No file selected";
        return false;
    }

    const DiskFileEntry entry = entries_[entryIndex];
    std::string desiredName(newNameOrStem);
    if (desiredName.find('.') == std::string::npos) {
        const auto dotPos = entry.name.find_last_of('.');
        if (dotPos != std::string::npos) {
            desiredName += entry.name.substr(dotPos);
        }
    }

    if (sourceKind_ == DiskSourceKind::Fat12Image) {
        if (mountedFile_ != juce::File{}) {
            outStatusMessage = "Read-Only Disk Image";
            return false;
        }
        std::array<uint8_t, 11> new83{};
        formatDos83(desiredName, new83);
        uint8_t* matchedEnt = nullptr;
        if (entry.hasFatDirOffset && static_cast<std::size_t>(entry.fatDirOffset) + 32u <= imageBytes_.size()) {
            uint8_t* direct = &imageBytes_[entry.fatDirOffset];
            if (direct[0] != 0x00u && direct[0] != 0xE5u && (direct[11] & 0x18u) == 0u) {
                matchedEnt = direct;
            }
        }
        if (matchedEnt == nullptr) {
            std::array<uint8_t, 11> old83{};
            formatDos83(entry.name, old83);
            const int maxRootSlots = static_cast<int>(geometry_.rootEntries);
            for (int i = 0; i < maxRootSlots; ++i) {
                const uint32_t off = geometry_.rootDirByteOffset + static_cast<uint32_t>(i) * 32u;
                if (static_cast<std::size_t>(off) + 32u > imageBytes_.size()) {
                    break;
                }
                uint8_t* ent = &imageBytes_[off];
                if (ent[0] == 0x00u) break;
                if (ent[0] == 0xE5u || (ent[11] & 0x18u) != 0u) continue;
                if (std::memcmp(ent, old83.data(), 11u) == 0) {
                    matchedEnt = ent;
                    break;
                }
            }
        }
        if (matchedEnt != nullptr) {
            std::memcpy(matchedEnt, new83.data(), 11u);
            if (mountedFile_ != juce::File{}) {
                (void)mountedFile_.replaceWithData(imageBytes_.data(), imageBytes_.size());
            }
            rescanMountedSource();
            const std::string cleanStem = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(new83.data()), 8u));
            const std::string cleanExt  = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(new83.data() + 8u), 3u));
            outStatusMessage = "Renamed to " + juce::String(cleanExt.empty() ? cleanStem : (cleanStem + "." + cleanExt));
            return true;
        }
        outStatusMessage = "Entry not found";
        return false;
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory && mountedFile_.isDirectory()) {
        std::array<uint8_t, 11> new83{};
        formatDos83(desiredName, new83);
        const std::string cleanStem = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(new83.data()), 8u));
        const std::string cleanExt  = trimRightSpaces(std::string_view(reinterpret_cast<const char*>(new83.data() + 8u), 3u));
        const std::string finalName = cleanExt.empty() ? cleanStem : (cleanStem + "." + cleanExt);
        const juce::File src(juce::String::fromUTF8(entry.fullOrRelPath.c_str()));
        const juce::File dst = src.getParentDirectory().getChildFile(juce::String(finalName));
        if (!src.existsAsFile() || !src.moveFileTo(dst)) {
            outStatusMessage = "Rename failed";
            return false;
        }
        rescanMountedSource();
        outStatusMessage = "Renamed to " + dst.getFileName();
        return true;
    }

    outStatusMessage = "Unsupported source";
    return false;
}

bool VF50DiskManager::saveTypedToMountedSource(std::string_view baseName,
                                              VF50FileKind kind,
                                              vf50::plugin::VF50AudioProcessor& processor,
                                              juce::String& outStatusMessage,
                                              bool* outRoutedToUserDisk) {
    if (outRoutedToUserDisk != nullptr) {
        *outRoutedToUserDisk = false;
    }
    if (!isMounted()) {
        outStatusMessage = "No disk mounted";
        return false;
    }

    const bool routedToUserDisk = (sourceKind_ == DiskSourceKind::Fat12Image);
    if (!routedToUserDisk && writeProtected_) {
        outStatusMessage = "Disk Write Protected";
        return false;
    }

    juce::File targetDir;
    if (routedToUserDisk) {
        if (userDiskDir_ == juce::File{}) {
            userDiskDir_ = defaultUserDiskDirectory();
        }
        if (!userDiskDir_.isDirectory() && !userDiskDir_.createDirectory()) {
            outStatusMessage = "Cannot open User Disk";
            return false;
        }
        targetDir = userDiskDir_;
    } else if (sourceKind_ == DiskSourceKind::LocalDirectory && mountedFile_.isDirectory()) {
        targetDir = mountedFile_;
    } else {
        outStatusMessage = "Mount disk first";
        return false;
    }

    std::string rawInput(baseName);
    const auto dotPos = rawInput.find_last_of('.');
    const std::string stemPart = (dotPos == std::string::npos) ? rawInput : rawInput.substr(0u, dotPos);
    const std::string cleanStem = sanitizeDos83Stem(stemPart, "NEWFILE");
    const char hwPrefix = hardwareExtPrefixForKind(kind);

    std::string finalExt;
    if (dotPos != std::string::npos) {
        const std::string candidateExt = toUpperAscii(rawInput.substr(dotPos + 1u));
        if (isHardwareExtensionForKind("X." + candidateExt, kind)) {
            finalExt = candidateExt;
        }
    }

    if (finalExt.empty()) {
        // Scan target directory (including common User Disk) for next available .X01-.X99 slot
        std::array<bool, 100> usedNums{};
        auto existingFiles = targetDir.findChildFiles(juce::File::findFiles, false, "*");
        for (const auto& f : existingFiles) {
            const std::string fn = f.getFileName().toStdString();
            const auto dp = fn.find_last_of('.');
            if (dp == std::string::npos) continue;
            const std::string ex = toUpperAscii(fn.substr(dp + 1u));
            if (ex.size() == 3u && ex[0] == hwPrefix
                && std::isdigit(static_cast<unsigned char>(ex[1]))
                && std::isdigit(static_cast<unsigned char>(ex[2]))) {
                const int n = (ex[1] - '0') * 10 + (ex[2] - '0');
                if (n >= 1 && n <= 99) {
                    usedNums[static_cast<std::size_t>(n)] = true;
                }
            }
        }
        int nextNum = 1;
        while (nextNum <= 99 && usedNums[static_cast<std::size_t>(nextNum)]) {
            ++nextNum;
        }
        if (nextNum > 99) {
            nextNum = 99;
        }
        char extBuf[4]{ hwPrefix,
                        static_cast<char>('0' + (nextNum / 10)),
                        static_cast<char>('0' + (nextNum % 10)),
                        '\0' };
        finalExt.assign(extBuf);
    }

    const std::string fileName83 = cleanStem + "." + finalExt;

    processor.pullLatestAudioMemoryToHost();
    const auto& mem = processor.hostMemory();
    std::vector<uint8_t> outBytes;

    if (kind == VF50FileKind::AllData) {
        buildAllDataBytes(mem, outBytes, &processor.hostRhythmSongs(), &processor);
    } else if (kind == VF50FileKind::VoiceBank || kind == VF50FileKind::PerfBank) {
        buildHardwareSynBytes(processor, outBytes);
    } else if (kind == VF50FileKind::CardData) {
        buildCardBankBytes(processor.virtualCard().activeBank(), outBytes);
    } else if (kind == VF50FileKind::RhythmData) {
        buildRhythmDataBytes(mem, outBytes, &processor.hostRhythmSongs());
    } else if (kind == VF50FileKind::SysEx) {
        vf50::midi::VF50SysExHandler sx{};
        vf50::midi::SysExReplyBuffer reply{};
        (void)sx.emitSingleVoiceBankDump100(mem, vf50::state::MemoryBank::UserRam, reply);
        reply.copyConcatenatedTo(outBytes);
    } else if (kind == VF50FileKind::Sequence) {
        const vf50::dsp::CustomSeqSongBuffer* activeSeqPtr = nullptr;
        const int activeIdx = processor.activeDemoSongIndex();
        if (activeIdx >= 0 && vf50::dsp::VF50DemoSongPlayer::isUserSongIndex(static_cast<std::size_t>(activeIdx))) {
            activeSeqPtr = processor.userSequenceInSlot(static_cast<std::size_t>(activeIdx));
        }
        buildVF50SeqV1Bytes(activeSeqPtr, cleanStem, processor.activeDemoTempoBpm(), outBytes);
    } else {
        buildAllDataBytes(mem, outBytes, &processor.hostRhythmSongs(), &processor);
    }

    const juce::int64 volFreeBytes = targetDir.getBytesFreeOnVolume();
    if (volFreeBytes >= 0 && static_cast<uint64_t>(volFreeBytes) < static_cast<uint64_t>(outBytes.size()) + 4096ULL) {
        outStatusMessage = "Disk Full";
        return false;
    }

    const juce::File dst = targetDir.getChildFile(juce::String(fileName83));
    if (!dst.replaceWithData(outBytes.data(), outBytes.size())) {
        outStatusMessage = "Write failed: " + dst.getFileName();
        return false;
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory && targetDir == mountedFile_) {
        rescanMountedSource();
    }
    if (outRoutedToUserDisk != nullptr) {
        *outRoutedToUserDisk = routedToUserDisk;
    }
    outStatusMessage = routedToUserDisk
                           ? ("Saved -> User Disk (" + dst.getFileName() + ")")
                           : ("Saved " + dst.getFileName());
    return true;
}

bool VF50DiskManager::formatMountedDisk(juce::String& outStatusMessage) {
    if (!isMounted()) {
        outStatusMessage = "No disk mounted";
        return false;
    }
    if (writeProtected_) {
        outStatusMessage = "Disk Write Protected";
        return false;
    }

    if (sourceKind_ == DiskSourceKind::Fat12Image) {
        if (mountedFile_ != juce::File{}) {
            outStatusMessage = "Read-Only Disk Image";
            return false;
        }
        const bool is2HD = (imageBytes_.size() >= 1200000u);
        imageBytes_ = createFormattedFat12Image(is2HD, "VF50_DISK");
        juce::String err;
        if (!parseFat12Geometry(err)) {
            outStatusMessage = err;
            return false;
        }
        if (mountedFile_ != juce::File{}) {
            (void)mountedFile_.replaceWithData(imageBytes_.data(), imageBytes_.size());
        }
        rescanMountedSource();
        outStatusMessage = "Formatted disk";
        return true;
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory && mountedFile_.isDirectory()) {
        for (const auto& entry : entries_) {
            const juce::File f(juce::String::fromUTF8(entry.fullOrRelPath.c_str()));
            if (f.existsAsFile()) {
                (void)f.deleteFile();
            }
        }
        rescanMountedSource();
        outStatusMessage = "Formatted folder";
        return true;
    }

    outStatusMessage = "Unsupported source";
    return false;
}

void VF50DiskManager::computeDiskUsageKb(int& outUsedKb, int& outFreeKb) const noexcept {
    outUsedKb = 0;
    outFreeKb = 0;
    if (sourceKind_ == DiskSourceKind::Fat12Image && geometry_.totalClusters > 0u) {
        const uint32_t clusBytes = static_cast<uint32_t>(geometry_.bytesPerSector) * geometry_.sectorsPerCluster;
        uint32_t usedClus = 0u;
        uint32_t freeClus = 0u;
        const uint16_t maxClusterId = static_cast<uint16_t>(geometry_.totalClusters + 1u);
        for (uint16_t c = 2u; c <= maxClusterId; ++c) {
            const uint16_t val = readFat12Entry(c);
            if (val == 0x000u) {
                ++freeClus;
            } else if (val != 0xFF7u) {
                ++usedClus;
            }
        }
        outUsedKb = static_cast<int>((static_cast<uint64_t>(usedClus) * clusBytes + 1023ULL) / 1024ULL);
        outFreeKb = static_cast<int>((static_cast<uint64_t>(freeClus) * clusBytes) / 1024ULL);
        return;
    }

    if (sourceKind_ == DiskSourceKind::LocalDirectory) {
        uint64_t totalBytes = 0ULL;
        for (const auto& e : entries_) {
            totalBytes += e.sizeBytes;
        }
        outUsedKb = static_cast<int>((totalBytes + 1023ULL) / 1024ULL);
        const juce::int64 volFreeBytes = mountedFile_.isDirectory() ? mountedFile_.getBytesFreeOnVolume() : -1;
        outFreeKb = (volFreeBytes >= 0)
                        ? static_cast<int>(std::min<juce::int64>(volFreeBytes / 1024LL, 999LL))
                        : 713;
    }
}

void VF50DiskManager::computeSaveTargetUsageKb(int& outUsedKb, int& outFreeKb) const noexcept {
    if (sourceKind_ == DiskSourceKind::LocalDirectory) {
        computeDiskUsageKb(outUsedKb, outFreeKb);
        return;
    }
    outUsedKb = 0;
    outFreeKb = 713;
    if (userDiskDir_.isDirectory()) {
        uint64_t totalBytes = 0ULL;
        auto children = userDiskDir_.findChildFiles(juce::File::findFiles, false, kUniversalFileFilter);
        for (const auto& f : children) {
            if (classifyFileKind(f.getFileName().toStdString()) != VF50FileKind::Unknown) {
                totalBytes += static_cast<uint64_t>(std::max<juce::int64>(0, f.getSize()));
            }
        }
        outUsedKb = static_cast<int>((totalBytes + 1023ULL) / 1024ULL);
        const juce::int64 volFreeBytes = userDiskDir_.getBytesFreeOnVolume();
        outFreeKb = (volFreeBytes >= 0)
                        ? static_cast<int>(std::min<juce::int64>(volFreeBytes / 1024LL, 999LL))
                        : 713;
    }
}

} // namespace vf50::disk
