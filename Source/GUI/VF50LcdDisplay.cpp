#include "GUI/VF50LcdDisplay.h"

#include <algorithm>
#include <cmath>

namespace vf50::gui {

namespace {

using Glyph5x8 = std::array<uint8_t, VF50LcdDisplay::kDotRows>;

// Build complete 256-character Hitachi HD44780 5x8 Dot-Matrix ROM + VF50 CGRAM Custom Icons
consteval std::array<Glyph5x8, 256> buildHd44780CharacterRom() noexcept {
    std::array<Glyph5x8, 256> rom{};

    // 0x00: Blank space
    rom[0x00] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    // 0x01 .. 0x08: Custom VF50 CGRAM Vertical Bar-Graph Level Meters (1/8 to 8/8 height)
    for (int level = 1; level <= 8; ++level) {
        Glyph5x8 bar{};
        for (int r = 0; r < 8; ++r) {
            bar[static_cast<std::size_t>(r)] = (r >= (8 - level)) ? 0x1Fu : 0x00u;
        }
        rom[static_cast<std::size_t>(level)] = bar;
    }

    // 0x09..0x13: VF50 Custom CGRAM Workstation & Sequencer Icons
    rom[0x09] = { 0x04, 0x0E, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x00 }; // Up arrow
    rom[0x0A] = { 0x04, 0x04, 0x04, 0x04, 0x1F, 0x0E, 0x04, 0x00 }; // Down arrow
    rom[0x0B] = { 0x02, 0x03, 0x02, 0x02, 0x0E, 0x1E, 0x0C, 0x00 }; // Quarter note
    rom[0x0C] = { 0x0E, 0x11, 0x11, 0x1F, 0x1B, 0x1B, 0x1F, 0x00 }; // Memory Protect lock
    rom[0x0D] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00 }; // Active operator block
    rom[0x0E] = { 0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F, 0x00 }; // Muted operator hollow box
    rom[0x0F] = { 0x00, 0x04, 0x0E, 0x1F, 0x0E, 0x04, 0x00, 0x00 }; // Center diamond
    rom[0x10] = { 0x08, 0x0C, 0x0E, 0x0F, 0x0E, 0x0C, 0x08, 0x00 }; // Play triangle right
    rom[0x11] = { 0x02, 0x06, 0x0E, 0x1E, 0x0E, 0x06, 0x02, 0x00 }; // Triangle left
    rom[0x12] = { 0x00, 0x0E, 0x1F, 0x1F, 0x1F, 0x0E, 0x00, 0x00 }; // Record circle
    rom[0x13] = { 0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x00 }; // Stop square
    rom[0x14] = { 0x04, 0x1F, 0x04, 0x04, 0x1F, 0x04, 0x00, 0x00 }; // Multi-press page indicator (‡)

    // Standard HD44780A00 ASCII Character ROM (0x20 .. 0x7F)
    rom[0x20] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // ' '
    rom[0x21] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00 }; // '!'
    rom[0x22] = { 0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 }; // '"'
    rom[0x23] = { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A, 0x00 }; // '#'
    rom[0x24] = { 0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04, 0x00 }; // '$'
    rom[0x25] = { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03, 0x00 }; // '%'
    rom[0x26] = { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D, 0x00 }; // '&'
    rom[0x27] = { 0x06, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 }; // '''
    rom[0x28] = { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02, 0x00 }; // '('
    rom[0x29] = { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08, 0x00 }; // ')'
    rom[0x2A] = { 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00, 0x00 }; // '*'
    rom[0x2B] = { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00, 0x00 }; // '+'
    rom[0x2C] = { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08, 0x00 }; // ','
    rom[0x2D] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00 }; // '-'
    rom[0x2E] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00 }; // '.'
    rom[0x2F] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00 }; // '/'

    rom[0x30] = { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E, 0x00 }; // '0'
    rom[0x31] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00 }; // '1'
    rom[0x32] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F, 0x00 }; // '2'
    rom[0x33] = { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E, 0x00 }; // '3'
    rom[0x34] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02, 0x00 }; // '4'
    rom[0x35] = { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E, 0x00 }; // '5'
    rom[0x36] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E, 0x00 }; // '6'
    rom[0x37] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08, 0x00 }; // '7'
    rom[0x38] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E, 0x00 }; // '8'
    rom[0x39] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C, 0x00 }; // '9'

    rom[0x3A] = { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00, 0x00 }; // ':'
    rom[0x3B] = { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08, 0x00 }; // ';'
    rom[0x3C] = { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02, 0x00 }; // '<'
    rom[0x3D] = { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00 }; // '='
    rom[0x3E] = { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08, 0x00 }; // '>'
    rom[0x3F] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 }; // '?'
    rom[0x40] = { 0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E, 0x00 }; // '@'

    rom[0x41] = { 0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x00 }; // 'A'
    rom[0x42] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E, 0x00 }; // 'B'
    rom[0x43] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E, 0x00 }; // 'C'
    rom[0x44] = { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C, 0x00 }; // 'D'
    rom[0x45] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F, 0x00 }; // 'E'
    rom[0x46] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x00 }; // 'F'
    rom[0x47] = { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F, 0x00 }; // 'G'
    rom[0x48] = { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00 }; // 'H'
    rom[0x49] = { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00 }; // 'I'
    rom[0x4A] = { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C, 0x00 }; // 'J'
    rom[0x4B] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11, 0x00 }; // 'K'
    rom[0x4C] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F, 0x00 }; // 'L'
    rom[0x4D] = { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11, 0x00 }; // 'M'
    rom[0x4E] = { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x00 }; // 'N'
    rom[0x4F] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00 }; // 'O'
    rom[0x50] = { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10, 0x00 }; // 'P'
    rom[0x51] = { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D, 0x00 }; // 'Q'
    rom[0x52] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11, 0x00 }; // 'R'
    rom[0x53] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E, 0x00 }; // 'S'
    rom[0x54] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00 }; // 'T'
    rom[0x55] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00 }; // 'U'
    rom[0x56] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04, 0x00 }; // 'V'
    rom[0x57] = { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A, 0x00 }; // 'W'
    rom[0x58] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11, 0x00 }; // 'X'
    rom[0x59] = { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x00 }; // 'Y'
    rom[0x5A] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F, 0x00 }; // 'Z'

    rom[0x5B] = { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E, 0x00 }; // '['
    rom[0x5C] = { 0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00 }; // '\' (Yen/Backslash)
    rom[0x5D] = { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E, 0x00 }; // ']'
    rom[0x5E] = { 0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00 }; // '^'
    rom[0x5F] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00 }; // '_'
    rom[0x60] = { 0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 }; // '`'

    rom[0x61] = { 0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F, 0x00 }; // 'a'
    rom[0x62] = { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E, 0x00 }; // 'b'
    rom[0x63] = { 0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E, 0x00 }; // 'c'
    rom[0x64] = { 0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F, 0x00 }; // 'd'
    rom[0x65] = { 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E, 0x00 }; // 'e'
    rom[0x66] = { 0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08, 0x00 }; // 'f'
    rom[0x67] = { 0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E, 0x00 }; // 'g'
    rom[0x68] = { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11, 0x00 }; // 'h'
    rom[0x69] = { 0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E, 0x00 }; // 'i'
    rom[0x6A] = { 0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C, 0x00 }; // 'j'
    rom[0x6B] = { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12, 0x00 }; // 'k'
    rom[0x6C] = { 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00 }; // 'l'
    rom[0x6D] = { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11, 0x00 }; // 'm'
    rom[0x6E] = { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11, 0x00 }; // 'n'
    rom[0x6F] = { 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00 }; // 'o'
    rom[0x70] = { 0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10, 0x00 }; // 'p'
    rom[0x71] = { 0x00, 0x00, 0x0D, 0x13, 0x0F, 0x01, 0x01, 0x00 }; // 'q'
    rom[0x72] = { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x00 }; // 'r'
    rom[0x73] = { 0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E, 0x00 }; // 's'
    rom[0x74] = { 0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06, 0x00 }; // 't'
    rom[0x75] = { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D, 0x00 }; // 'u'
    rom[0x76] = { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04, 0x00 }; // 'v'
    rom[0x77] = { 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A, 0x00 }; // 'w'
    rom[0x78] = { 0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00 }; // 'x'
    rom[0x79] = { 0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E, 0x00 }; // 'y'
    rom[0x7A] = { 0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F, 0x00 }; // 'z'

    rom[0x7B] = { 0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02, 0x00 }; // '{'
    rom[0x7C] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00 }; // '|'
    rom[0x7D] = { 0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08, 0x00 }; // '}'
    rom[0x7E] = { 0x00, 0x04, 0x02, 0x1F, 0x02, 0x04, 0x00, 0x00 }; // HD44780 Right Arrow '->'
    rom[0x7F] = { 0x00, 0x04, 0x08, 0x1F, 0x08, 0x04, 0x00, 0x00 }; // HD44780 Left Arrow '<-'

    // 0xFF: Full 5x8 Solid Block
    rom[0xFF] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F };

    return rom;
}

inline constexpr std::array<Glyph5x8, 256> kHd44780CharacterRom = buildHd44780CharacterRom();

} // namespace

VF50LcdDisplay::VF50LcdDisplay() {
    for (auto& row : vram_) {
        row.fill(static_cast<uint8_t>(' '));
    }
    setOpaque(false);
}

const std::array<uint8_t, VF50LcdDisplay::kDotRows>& VF50LcdDisplay::getGlyphBitmap(uint8_t charCode) noexcept {
    return kHd44780CharacterRom[charCode];
}

void VF50LcdDisplay::setLine(int row, std::string_view text) noexcept {
    if (row < 0 || row >= kRows) {
        return;
    }
    auto& dst = vram_[static_cast<std::size_t>(row)];
    bool changed = false;
    for (int c = 0; c < kCols; ++c) {
        const uint8_t ch = (static_cast<std::size_t>(c) < text.size())
                               ? static_cast<uint8_t>(text[static_cast<std::size_t>(c)])
                               : static_cast<uint8_t>(' ');
        if (dst[static_cast<std::size_t>(c)] != ch) {
            dst[static_cast<std::size_t>(c)] = ch;
            changed = true;
        }
    }
    if (changed) {
        lcdBufferDirty_ = true;
        repaint();
    }
}

void VF50LcdDisplay::setLines(std::string_view row0, std::string_view row1) noexcept {
    setLine(0, row0);
    setLine(1, row1);
}

void VF50LcdDisplay::setChar(int row, int col, uint8_t hd44780Char) noexcept {
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) {
        return;
    }
    auto& cell = vram_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
    if (cell != hd44780Char) {
        cell = hd44780Char;
        lcdBufferDirty_ = true;
        repaint();
    }
}

uint8_t VF50LcdDisplay::getChar(int row, int col) const noexcept {
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) {
        return static_cast<uint8_t>(' ');
    }
    return vram_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
}

juce::String VF50LcdDisplay::getLineAsString(int row) const {
    if (row < 0 || row >= kRows) {
        return {};
    }
    char buf[kCols + 1];
    for (int c = 0; c < kCols; ++c) {
        const uint8_t raw = vram_[static_cast<std::size_t>(row)][static_cast<std::size_t>(c)];
        if (raw >= 0x01u && raw <= 0x08u) {
            buf[c] = '|';
        } else if (raw == 0x09u) {
            buf[c] = '^';
        } else if (raw == 0x0Au) {
            buf[c] = 'v';
        } else if (raw == 0x0Bu) {
            buf[c] = 'q';
        } else if (raw == 0x0Du) {
            buf[c] = '#';
        } else if (raw == 0x0Fu) {
            buf[c] = '*';
        } else if (raw == 0x10u || raw == 0x7Eu) {
            buf[c] = '>';
        } else if (raw == 0x14u) {
            buf[c] = '+';
        } else if (raw == 0x7Fu) {
            buf[c] = '<';
        } else if (raw >= 0x20u && raw <= 0x7Du) {
            buf[c] = static_cast<char>(raw);
        } else {
            buf[c] = ' ';
        }
    }
    buf[kCols] = '\0';
    return juce::String(buf, kCols);
}

void VF50LcdDisplay::setCursor(int row, int col, CursorMode mode) noexcept {
    const int clampedRow = std::clamp(row, 0, kRows - 1);
    const int clampedCol = std::clamp(col, 0, kCols - 1);
    if (cursorRow_ != clampedRow || cursorCol_ != clampedCol || cursorMode_ != mode) {
        cursorRow_ = clampedRow;
        cursorCol_ = clampedCol;
        cursorMode_ = mode;
        lcdBufferDirty_ = true;
        repaint();
    }
}

void VF50LcdDisplay::tickCursorBlink() noexcept {
    ++blinkTick_;
    // At 30 Hz UI timer, toggle every 7 ticks (~2.14 Hz authentic HD44780 blink rate)
    const bool newPhase = ((blinkTick_ / 7u) & 1u) == 0u;
    if (newPhase != blinkPhaseOn_) {
        blinkPhaseOn_ = newPhase;
        if (cursorMode_ == CursorMode::BlinkingUnderline || cursorMode_ == CursorMode::BlinkingBlock) {
            lcdBufferDirty_ = true;
            repaint();
        }
    }
}

void VF50LcdDisplay::setThemeIndex(int index) noexcept {
    const int clamped = ((index % 3) + 3) % 3;
    if (themeIndex_ != clamped) {
        themeIndex_ = clamped;
        lcdBufferDirty_ = true;
        repaint();
        if (onThemeChanged) {
            onThemeChanged(themeIndex_);
        }
    }
}

void VF50LcdDisplay::cycleTheme() noexcept {
    setThemeIndex((themeIndex_ + 1) % 3);
}

const char* VF50LcdDisplay::currentThemeName() const noexcept {
    return tokens::kLcdThemes[static_cast<std::size_t>(themeIndex_)].name;
}

void VF50LcdDisplay::resized() {
    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());

    const float scaleX = w / static_cast<float>(kNativeWidth2x);
    const float scaleY = h / static_cast<float>(kNativeHeight2x);

    glassBounds_ = juce::Rectangle<float>(static_cast<float>(kGlassX2x) * scaleX,
                                          static_cast<float>(kGlassY2x) * scaleY,
                                          std::max(80.0f, static_cast<float>(kGlassW2x) * scaleX),
                                          std::max(32.0f, static_cast<float>(kGlassH2x) * scaleY));

    gridOriginX_ = static_cast<float>(kGridOriginX2x) * scaleX;
    charStepX_   = static_cast<float>(kCharStepX2x) * scaleX;
}

void VF50LcdDisplay::rebuildLcdBuffer() const {
    if (!lcdBuffer_.isValid()
        || lcdBuffer_.getWidth() != kNativeWidth2x
        || lcdBuffer_.getHeight() != kNativeHeight2x) {
        lcdBuffer_ = juce::Image(juce::Image::ARGB,
                                 kNativeWidth2x,
                                 kNativeHeight2x,
                                 true,
                                 juce::SoftwareImageType{});
    } else {
        lcdBuffer_.clear(lcdBuffer_.getBounds());
    }

    constexpr int kMip1Width  = kNativeWidth2x / 2;
    constexpr int kMip1Height = kNativeHeight2x / 2;
    if (!lcdBuffer1x_.isValid()
        || lcdBuffer1x_.getWidth() != kMip1Width
        || lcdBuffer1x_.getHeight() != kMip1Height) {
        lcdBuffer1x_ = juce::Image(juce::Image::ARGB,
                                   kMip1Width,
                                   kMip1Height,
                                   true,
                                   juce::SoftwareImageType{});
    } else {
        lcdBuffer1x_.clear(lcdBuffer1x_.getBounds());
    }

    const auto& theme = tokens::kLcdThemes[static_cast<std::size_t>(themeIndex_)];
    const juce::PixelARGB ghostPx  = juce::Colour(theme.ghostPixel).getPixelARGB();
    const juce::PixelARGB activePx = juce::Colour(theme.activePixel).getPixelARGB();

    // 1. Render crisp integer 2x native dot-matrix (3x3 dots + 1px integer micro-gaps on even 4x4 grid)
    {
        juce::Image::BitmapData dst2x(lcdBuffer_, juce::Image::BitmapData::writeOnly);

        for (int r = 0; r < kRows; ++r) {
            const int baseY = (r == 0) ? kRowOriginY0_2x : kRowOriginY1_2x;

            for (int c = 0; c < kCols; ++c) {
                const int baseX = kGridOriginX2x + c * kCharStepX2x;
                const uint8_t chCode = vram_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
                const auto& glyph = kHd44780CharacterRom[chCode];

                const bool isCursorCell = (cursorMode_ != CursorMode::None && r == cursorRow_ && c == cursorCol_);
                const bool invertBlock  = isCursorCell && (cursorMode_ == CursorMode::BlinkingBlock) && blinkPhaseOn_;
                const bool drawUnder    = isCursorCell && ((cursorMode_ == CursorMode::SolidUnderline)
                                                           || (cursorMode_ == CursorMode::BlinkingUnderline && blinkPhaseOn_));

                for (int dy = 0; dy < kDotRows; ++dy) {
                    uint8_t rowMask = glyph[static_cast<std::size_t>(dy)] & 0x1Fu;
                    if (invertBlock) {
                        rowMask = static_cast<uint8_t>((~rowMask) & 0x1Fu);
                    } else if (drawUnder && dy == (kDotRows - 1)) {
                        rowMask = 0x1Fu;
                    }

                    const int py = baseY + dy * kDotPitchY2x;
                    for (int dx = 0; dx < kDotCols; ++dx) {
                        const bool isLit = (rowMask & (1u << (4 - dx))) != 0u;
                        const juce::PixelARGB dotPx = isLit ? activePx : ghostPx;
                        const int px = baseX + dx * kDotPitchX2x;

                        for (int sy = 0; sy < kDotHeight2x; ++sy) {
                            auto* line = reinterpret_cast<juce::PixelARGB*>(dst2x.getLinePointer(py + sy));
                            for (int sx = 0; sx < kDotWidth2x; ++sx) {
                                line[px + sx] = dotPx;
                            }
                        }
                    }
                }
            }
        }
    }

    // 2. Build exact 2x2 box-area integrated 1x mip (zero cubic ringing at 100%, Nyquist-safe for 75%)
    {
        const juce::Image::BitmapData src2x(lcdBuffer_, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst1x(lcdBuffer1x_, juce::Image::BitmapData::writeOnly);

        constexpr int kYStart1x = kGlassY2x / 2;
        constexpr int kYEnd1x   = (kGlassY2x + kGlassH2x) / 2;
        constexpr int kXStart1x = kGlassX2x / 2;
        constexpr int kXEnd1x   = (kGlassX2x + kGlassW2x) / 2;

        for (int y = kYStart1x; y < kYEnd1x; ++y) {
            const auto* row0 = reinterpret_cast<const juce::PixelARGB*>(src2x.getLinePointer(y * 2));
            const auto* row1 = reinterpret_cast<const juce::PixelARGB*>(src2x.getLinePointer(y * 2 + 1));
            auto* outRow     = reinterpret_cast<juce::PixelARGB*>(dst1x.getLinePointer(y));

            for (int x = kXStart1x; x < kXEnd1x; ++x) {
                const auto& p00 = row0[x * 2];
                const auto& p01 = row0[x * 2 + 1];
                const auto& p10 = row1[x * 2];
                const auto& p11 = row1[x * 2 + 1];

                const auto a = static_cast<juce::uint8>((static_cast<uint32_t>(p00.getAlpha()) + p01.getAlpha() + p10.getAlpha() + p11.getAlpha() + 2u) >> 2u);
                const auto r = static_cast<juce::uint8>((static_cast<uint32_t>(p00.getRed())   + p01.getRed()   + p10.getRed()   + p11.getRed()   + 2u) >> 2u);
                const auto g = static_cast<juce::uint8>((static_cast<uint32_t>(p00.getGreen()) + p01.getGreen() + p10.getGreen() + p11.getGreen() + 2u) >> 2u);
                const auto b = static_cast<juce::uint8>((static_cast<uint32_t>(p00.getBlue())  + p01.getBlue()  + p10.getBlue()  + p11.getBlue()  + 2u) >> 2u);
                outRow[x] = juce::PixelARGB(a, r, g, b);
            }
        }
    }

    lcdBufferDirty_ = false;
}

void VF50LcdDisplay::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();
    const auto& theme = tokens::kLcdThemes[static_cast<std::size_t>(themeIndex_)];

    // 1. Outer Recessed Anodized Bezel Frame
    g.setColour(juce::Colour(0xFF090B0Du));
    g.fillRoundedRectangle(bounds, 6.0f);

    // Angled 3D recessed bezel slopes (top dark shadow slope, bottom subtle anodized catchlight, angled side walls)
    {
        juce::Path topSlope;
        topSlope.startNewSubPath(bounds.getX() + 2.0f, bounds.getY() + 2.0f);
        topSlope.lineTo(bounds.getRight() - 2.0f, bounds.getY() + 2.0f);
        topSlope.lineTo(glassBounds_.getRight() + 4.0f, glassBounds_.getY() - 3.0f);
        topSlope.lineTo(glassBounds_.getX() - 4.0f, glassBounds_.getY() - 3.0f);
        topSlope.closeSubPath();
        g.setColour(juce::Colour(0xFF0D1014u));
        g.fillPath(topSlope);

        juce::Path bottomSlope;
        bottomSlope.startNewSubPath(glassBounds_.getX() - 4.0f, glassBounds_.getBottom() + 3.0f);
        bottomSlope.lineTo(glassBounds_.getRight() + 4.0f, glassBounds_.getBottom() + 3.0f);
        bottomSlope.lineTo(bounds.getRight() - 2.0f, bounds.getBottom() - 2.0f);
        bottomSlope.lineTo(bounds.getX() + 2.0f, bounds.getBottom() - 2.0f);
        bottomSlope.closeSubPath();
        g.setColour(juce::Colour(0xFF232830u));
        g.fillPath(bottomSlope);

        juce::Path leftSlope;
        leftSlope.startNewSubPath(bounds.getX() + 2.0f, bounds.getY() + 2.0f);
        leftSlope.lineTo(glassBounds_.getX() - 4.0f, glassBounds_.getY() - 3.0f);
        leftSlope.lineTo(glassBounds_.getX() - 4.0f, glassBounds_.getBottom() + 3.0f);
        leftSlope.lineTo(bounds.getX() + 2.0f, bounds.getBottom() - 2.0f);
        leftSlope.closeSubPath();
        g.setColour(juce::Colour(0xFF14181Du));
        g.fillPath(leftSlope);

        juce::Path rightSlope;
        rightSlope.startNewSubPath(glassBounds_.getRight() + 4.0f, glassBounds_.getY() - 3.0f);
        rightSlope.lineTo(bounds.getRight() - 2.0f, bounds.getY() + 2.0f);
        rightSlope.lineTo(bounds.getRight() - 2.0f, bounds.getBottom() - 2.0f);
        rightSlope.lineTo(glassBounds_.getRight() + 4.0f, glassBounds_.getBottom() + 3.0f);
        rightSlope.closeSubPath();
        g.setColour(juce::Colour(0xFF171C22u));
        g.fillPath(rightSlope);
    }

    // Outer chassis catchlight rim
    g.setColour(juce::Colour(0xFF343B44u));
    g.drawRoundedRectangle(bounds.reduced(1.0f), 5.5f, 1.5f);

    // Subtle etched theme-toggle indicator dot on right bezel corner ("BL" backlight lens)
    {
        const float dotCx = bounds.getRight() - 26.0f;
        const float dotCy = bounds.getY() + 22.0f;
        g.setColour(juce::Colour(theme.substrateTop).withAlpha(0.55f));
        g.fillEllipse(dotCx - 4.5f, dotCy - 4.5f, 9.0f, 9.0f);
        g.setColour(juce::Colour(tokens::kSilkDimLabel).withAlpha(0.65f));
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText("BL", juce::Rectangle<float>(dotCx - 16.0f, dotCy + 6.0f, 32.0f, 14.0f),
                   juce::Justification::centred, false);
    }

    // 2. Inner LCD Metal Mounting Frame
    const auto innerFrame = glassBounds_.expanded(4.0f);
    g.setColour(juce::Colour(theme.bezelOuter));
    g.fillRoundedRectangle(innerFrame, 3.5f);

    // 3. Even Backlit Liquid-Crystal Substrate
    {
        juce::ColourGradient bgGrad(juce::Colour(theme.substrateTop),
                                    glassBounds_.getX(), glassBounds_.getY(),
                                    juce::Colour(theme.substrateBottom),
                                    glassBounds_.getX(), glassBounds_.getBottom(),
                                    false);
        bgGrad.addColour(0.50, juce::Colour(theme.substrateTop).interpolatedWith(juce::Colour(theme.substrateBottom), 0.35f));
        g.setGradientFill(bgGrad);
        g.fillRoundedRectangle(glassBounds_, 2.5f);
    }

    // 4. Cached Offscreen 40x2 x 5x8 Dot-Matrix Buffer Blit (Area-Resampled)
    if (lcdBufferDirty_) {
        rebuildLcdBuffer();
    }
    const float physScale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const juce::Image& srcBuffer = (physScale <= 0.505f) ? lcdBuffer1x_ : lcdBuffer_;

    g.setOpacity(1.0f);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImage(srcBuffer, bounds);

    // 6. Angled Inner Bezel Drop Shadows onto LCD Glass (floating-point Path clip - zero integer rounding)
    {
        juce::Graphics::ScopedSaveState clipSave(g);
        juce::Path glassClipPath;
        glassClipPath.addRoundedRectangle(glassBounds_, 2.5f);
        g.reduceClipRegion(glassClipPath);

        // Top inner drop shadow
        juce::ColourGradient topShad(juce::Colour(theme.bezelInnerShadow),
                                     glassBounds_.getX(), glassBounds_.getY(),
                                     juce::Colours::transparentBlack,
                                     glassBounds_.getX(), glassBounds_.getY() + 11.0f,
                                     false);
        g.setGradientFill(topShad);
        g.fillRect(glassBounds_.getX(), glassBounds_.getY(), glassBounds_.getWidth(), 11.0f);

        // Left inner drop shadow
        juce::ColourGradient leftShad(juce::Colour(theme.bezelInnerShadow).withMultipliedAlpha(0.75f),
                                      glassBounds_.getX(), glassBounds_.getY(),
                                      juce::Colours::transparentBlack,
                                      glassBounds_.getX() + 12.0f, glassBounds_.getY(),
                                      false);
        g.setGradientFill(leftShad);
        g.fillRect(glassBounds_.getX(), glassBounds_.getY(), 12.0f, glassBounds_.getHeight());

        // Subtle diagonal acrylic glass specular reflection
        juce::Path glare;
        glare.startNewSubPath(glassBounds_.getX() + glassBounds_.getWidth() * 0.08f, glassBounds_.getY());
        glare.lineTo(glassBounds_.getX() + glassBounds_.getWidth() * 0.24f, glassBounds_.getY());
        glare.lineTo(glassBounds_.getX() + glassBounds_.getWidth() * 0.17f, glassBounds_.getBottom());
        glare.lineTo(glassBounds_.getX() + glassBounds_.getWidth() * 0.02f, glassBounds_.getBottom());
        glare.closeSubPath();

        juce::ColourGradient glareGrad(juce::Colours::white.withAlpha(0.065f),
                                       glassBounds_.getX(), glassBounds_.getY(),
                                       juce::Colours::white.withAlpha(0.010f),
                                       glassBounds_.getX(), glassBounds_.getBottom(),
                                       false);
        g.setGradientFill(glareGrad);
        g.fillPath(glare);
    }

    // Crisp inner glass frame hairline
    g.setColour(juce::Colour(0x99050708u));
    g.drawRoundedRectangle(glassBounds_, 2.5f, 1.5f);
}

juce::Rectangle<float> VF50LcdDisplay::blBadgeBounds() const noexcept {
    const auto bounds = getLocalBounds().toFloat();
    return { bounds.getRight() - 50.0f, bounds.getY() + 6.0f, 48.0f, 48.0f };
}

bool VF50LcdDisplay::isBlBadgeHit(juce::Point<float> pt) const noexcept {
    return blBadgeBounds().contains(pt);
}

float VF50LcdDisplay::paintedCharGridLeftXInParent() const noexcept {
    const float scaleX = static_cast<float>(getWidth()) / static_cast<float>(kNativeWidth2x);
    return static_cast<float>(getX()) + static_cast<float>(kGridOriginX2x) * scaleX;
}

float VF50LcdDisplay::paintedCharGridRightXInParent() const noexcept {
    constexpr int kPaintedBlockW2x  = (kDotCols - 1) * kDotPitchX2x + kDotWidth2x; // 4*4 + 3 = 19
    constexpr int kGridRightLocal2x = kGridOriginX2x + (kCols - 1) * kCharStepX2x + kPaintedBlockW2x; // 104 + 39*26 + 19 = 1137
    const float scaleX = static_cast<float>(getWidth()) / static_cast<float>(kNativeWidth2x);
    return static_cast<float>(getX()) + static_cast<float>(kGridRightLocal2x) * scaleX;
}

void VF50LcdDisplay::handleThemeClickInteraction(juce::Point<float> pt, bool isRightClick) noexcept {
    if (isRightClick || isBlBadgeHit(pt)) {
        cycleTheme();
        dragStartCol_ = -1;
    }
}

int VF50LcdDisplay::columnFromLocalX(float x) const noexcept {
    const float rel = (x - gridOriginX_) / std::max(1.0f, charStepX_);
    return std::clamp(static_cast<int>(std::floor(rel)), 0, kCols - 1);
}

int VF50LcdDisplay::rowFromLocalY(float y) const noexcept {
    const float midY = glassBounds_.getCentreY();
    return (y < midY) ? 0 : 1;
}

void VF50LcdDisplay::mouseDown(const juce::MouseEvent& event) {
    const auto pt = event.position;
    if (event.mods.isPopupMenu() || isBlBadgeHit(pt)) {
        handleThemeClickInteraction(pt, event.mods.isPopupMenu());
        return;
    }
    if (!glassBounds_.contains(pt)) {
        dragStartCol_ = -1;
        return;
    }

    const int r = rowFromLocalY(pt.y);
    const int c = columnFromLocalX(pt.x);
    dragStartCol_ = c;
    dragAccumPx_  = 0.0f;

    if (onCellClicked) {
        onCellClicked(r, c);
    }
}

void VF50LcdDisplay::mouseDrag(const juce::MouseEvent& event) {
    if (dragStartCol_ < 0 || !onCellValueScrub) {
        return;
    }
    const float totalDy = -static_cast<float>(event.getDistanceFromDragStartY());
    const float delta = totalDy - dragAccumPx_;
    constexpr float kPixelsPerStep = 9.0f;
    if (std::abs(delta) >= kPixelsPerStep) {
        const int steps = static_cast<int>(delta / kPixelsPerStep);
        dragAccumPx_ += static_cast<float>(steps) * kPixelsPerStep;
        if (steps != 0) {
            onCellValueScrub(dragStartCol_, steps);
        }
    }
}

void VF50LcdDisplay::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
    if (!glassBounds_.contains(event.position) || !onCellValueScrub) {
        return;
    }
    const int col = columnFromLocalX(event.position.x);
    const int steps = (wheel.deltaY > 0.0f) ? 1 : ((wheel.deltaY < 0.0f) ? -1 : 0);
    if (steps != 0) {
        onCellValueScrub(col, steps);
    }
}

} // namespace vf50::gui
