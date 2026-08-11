/* UTF-8 decoding, display width, and character-boundary slicing. */

#include "dispatch_utf8.h"

#include <stdint.h>

typedef struct {
    uint32_t low;
    uint32_t high;
} CodepointRange;

/* Zero-width characters: combining marks and format controls that attach to
 * the preceding character instead of advancing the cursor. */
static const CodepointRange zero_width_ranges[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x0903},
    {0x093A, 0x093C}, {0x093E, 0x094F}, {0x0951, 0x0957}, {0x0962, 0x0963},
    {0x0981, 0x0983}, {0x09BC, 0x09BC}, {0x09BE, 0x09CD}, {0x09D7, 0x09D7},
    {0x09E2, 0x09E3}, {0x0A01, 0x0A03}, {0x0A3C, 0x0A51}, {0x0A70, 0x0A71},
    {0x0A75, 0x0A75}, {0x0A81, 0x0A83}, {0x0ABC, 0x0ACD}, {0x0AE2, 0x0AE3},
    {0x0B01, 0x0B03}, {0x0B3C, 0x0B57}, {0x0B62, 0x0B63}, {0x0B82, 0x0B82},
    {0x0BBE, 0x0BCD}, {0x0BD7, 0x0BD7}, {0x0C00, 0x0C03}, {0x0C3E, 0x0C56},
    {0x0C62, 0x0C63}, {0x0C81, 0x0C83}, {0x0CBC, 0x0CD6}, {0x0CE2, 0x0CE3},
    {0x0D01, 0x0D03}, {0x0D3E, 0x0D4D}, {0x0D57, 0x0D57}, {0x0D62, 0x0D63},
    {0x0D82, 0x0D83}, {0x0DCA, 0x0DDF}, {0x0DF2, 0x0DF3}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC},
    {0x0EC8, 0x0ECD}, {0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37},
    {0x0F39, 0x0F39}, {0x0F3E, 0x0F3F}, {0x0F71, 0x0F84}, {0x0F86, 0x0F87},
    {0x0F8D, 0x0FBC}, {0x0FC6, 0x0FC6}, {0x102B, 0x103E}, {0x1056, 0x1059},
    {0x105E, 0x1060}, {0x1062, 0x1064}, {0x1067, 0x106D}, {0x1071, 0x1074},
    {0x1082, 0x108D}, {0x108F, 0x108F}, {0x109A, 0x109D}, {0x135D, 0x135F},
    {0x1712, 0x1714}, {0x1732, 0x1734}, {0x1752, 0x1753}, {0x1772, 0x1773},
    {0x17B4, 0x17D3}, {0x17DD, 0x17DD}, {0x180B, 0x180E}, {0x18A9, 0x18A9},
    {0x1920, 0x193B}, {0x1A17, 0x1A1B}, {0x1A55, 0x1A7F}, {0x1AB0, 0x1AFF},
    {0x1B00, 0x1B04}, {0x1B34, 0x1B44}, {0x1B6B, 0x1B73}, {0x1B80, 0x1B82},
    {0x1BA1, 0x1BAD}, {0x1BE6, 0x1BF3}, {0x1C24, 0x1C37}, {0x1CD0, 0x1CD2},
    {0x1CD4, 0x1CE8}, {0x1CED, 0x1CED}, {0x1CF4, 0x1CF4}, {0x1CF8, 0x1CF9},
    {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
    {0x2066, 0x206F}, {0x20D0, 0x20F0}, {0x2CEF, 0x2CF1}, {0x2D7F, 0x2D7F},
    {0x2DE0, 0x2DFF}, {0x302A, 0x302F}, {0x3099, 0x309A}, {0xA66F, 0xA672},
    {0xA674, 0xA67D}, {0xA69E, 0xA69F}, {0xA6F0, 0xA6F1}, {0xA802, 0xA802},
    {0xA806, 0xA806}, {0xA80B, 0xA80B}, {0xA823, 0xA827}, {0xA880, 0xA881},
    {0xA8B4, 0xA8C5}, {0xA8E0, 0xA8F1}, {0xA926, 0xA92D}, {0xA947, 0xA953},
    {0xA980, 0xA983}, {0xA9B3, 0xA9C0}, {0xA9E5, 0xA9E5}, {0xAA29, 0xAA36},
    {0xAA43, 0xAA43}, {0xAA4C, 0xAA4D}, {0xAA7B, 0xAA7D}, {0xAAB0, 0xAAB0},
    {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8}, {0xAABE, 0xAABF}, {0xAAC1, 0xAAC1},
    {0xAAEB, 0xAAEF}, {0xAAF5, 0xAAF6}, {0xABE3, 0xABEA}, {0xABEC, 0xABED},
    {0xFB1E, 0xFB1E}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
    {0xFFF9, 0xFFFB}, {0x101FD, 0x101FD}, {0x102E0, 0x102E0},
    {0x10376, 0x1037A}, {0x10A01, 0x10A0F}, {0x10A38, 0x10A3F},
    {0x11000, 0x11002}, {0x11038, 0x11046}, {0x1107F, 0x11082},
    {0x110B0, 0x110BA}, {0x11100, 0x11102}, {0x11127, 0x11134},
    {0x11173, 0x11173}, {0x11180, 0x11182}, {0x111B3, 0x111C0},
    {0x1122C, 0x11237}, {0x112DF, 0x112EA}, {0x11301, 0x11303},
    {0x1133C, 0x1134D}, {0x11362, 0x11374}, {0x114B0, 0x114C3},
    {0x115AF, 0x115C0}, {0x11630, 0x11640}, {0x116AB, 0x116B7},
    {0x1171D, 0x1172B}, {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36},
    {0x1BC9D, 0x1BC9E}, {0x1D165, 0x1D169}, {0x1D16D, 0x1D182},
    {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1D242, 0x1D244},
    {0x1DA00, 0x1DA36}, {0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75},
    {0x1DA84, 0x1DA84}, {0x1DA9B, 0x1DAAF}, {0x1E8D0, 0x1E8D6},
    {0xE0100, 0xE01EF},
};

/* East Asian wide and fullwidth characters, which occupy two columns. */
static const CodepointRange double_width_ranges[] = {
    {0x1100, 0x115F},   {0x2329, 0x232A},   {0x2E80, 0x303E},
    {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},
    {0xA000, 0xA4CF},   {0xA960, 0xA97F},   {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
    {0x17000, 0x18AFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F200, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C},
    {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A},
    {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F},
    {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6F9}, {0x1F910, 0x1F9FF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static int range_contains(const CodepointRange *ranges, size_t count,
                          uint32_t cp) {
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (cp < ranges[mid].low)
            high = mid;
        else if (cp > ranges[mid].high)
            low = mid + 1;
        else
            return 1;
    }
    return 0;
}

/* Decode the sequence starting at `text` (which holds at least `bytes`
 * readable bytes). Stores the code point and returns the bytes consumed.
 * Malformed input consumes exactly one byte and reports a single-column
 * replacement, so decoding always advances. */
static size_t decode_char(const char *text, size_t bytes, uint32_t *out) {
    const unsigned char *p = (const unsigned char *)text;
    unsigned char lead = p[0];
    size_t length;
    uint32_t cp;

    if (lead < 0x80) {
        *out = lead;
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        cp = lead & 0x1FU;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        cp = lead & 0x0FU;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        cp = lead & 0x07U;
    } else {
        *out = 0xFFFD;
        return 1;
    }

    if (length > bytes) {
        *out = 0xFFFD;
        return 1;
    }
    for (size_t i = 1; i < length; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *out = 0xFFFD;
            return 1;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3FU);
    }

    /* Reject overlong encodings, surrogates, and out-of-range code points so
     * they cannot masquerade as zero-width or wide characters. */
    static const uint32_t minimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < minimum[length] || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *out = 0xFFFD;
        return 1;
    }

    *out = cp;
    return length;
}

static size_t codepoint_width(uint32_t cp) {
    if (cp == 0)
        return 0;
    /* C0 and C1 controls never advance the cursor on their own. */
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;
    if (cp < 0x0300)
        return 1;
    if (range_contains(zero_width_ranges,
                       sizeof(zero_width_ranges) / sizeof(zero_width_ranges[0]),
                       cp))
        return 0;
    if (range_contains(double_width_ranges,
                       sizeof(double_width_ranges) /
                           sizeof(double_width_ranges[0]),
                       cp))
        return 2;
    return 1;
}

size_t dispatch_utf8_width_n(const char *text, size_t bytes) {
    if (!text)
        return 0;

    size_t width = 0;
    size_t offset = 0;
    while (offset < bytes && text[offset]) {
        uint32_t cp;
        size_t length = decode_char(text + offset, bytes - offset, &cp);
        width += codepoint_width(cp);
        offset += length;
    }
    return width;
}

size_t dispatch_utf8_width(const char *text) {
    if (!text)
        return 0;

    size_t width = 0;
    for (size_t offset = 0; text[offset];) {
        uint32_t cp;
        size_t length = decode_char(text + offset, 4, &cp);
        width += codepoint_width(cp);
        offset += length;
    }
    return width;
}

size_t dispatch_utf8_char_bytes(const char *text) {
    uint32_t cp;
    if (!text || !text[0])
        return 0;
    return decode_char(text, 4, &cp);
}

size_t dispatch_utf8_prev_char_bytes(const char *text, size_t offset) {
    if (!text || offset == 0)
        return 0;

    /* Walk back over continuation bytes to the lead byte, then confirm the
     * sequence really ends where we started; a stray continuation byte is a
     * character of its own so backspace still removes exactly one byte. */
    size_t start = offset - 1;
    size_t steps = 0;
    while (start > 0 && steps < 3 &&
           ((unsigned char)text[start] & 0xC0) == 0x80) {
        start--;
        steps++;
    }

    uint32_t cp;
    size_t length = decode_char(text + start, offset - start, &cp);
    if (start + length == offset)
        return offset - start;
    return 1;
}

size_t dispatch_utf8_encode(uint32_t codepoint, char *out) {
    unsigned char *p = (unsigned char *)out;

    if (!out)
        return 0;
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        return 0;

    if (codepoint < 0x80) {
        p[0] = (unsigned char)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        p[0] = (unsigned char)(0xC0 | (codepoint >> 6));
        p[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        p[0] = (unsigned char)(0xE0 | (codepoint >> 12));
        p[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        p[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    p[0] = (unsigned char)(0xF0 | (codepoint >> 18));
    p[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
    p[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
    p[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
    return 4;
}

size_t dispatch_utf8_take_columns_n(const char *text, size_t bytes,
                                    size_t columns, size_t *used_columns) {
    size_t used = 0;
    size_t offset = 0;

    if (!text) {
        if (used_columns)
            *used_columns = 0;
        return 0;
    }

    while (offset < bytes && text[offset]) {
        uint32_t cp;
        size_t length = decode_char(text + offset, bytes - offset, &cp);
        size_t width = codepoint_width(cp);
        if (used + width > columns)
            break;
        used += width;
        offset += length;
    }

    if (used_columns)
        *used_columns = used;
    return offset;
}

size_t dispatch_utf8_take_columns(const char *text, size_t columns,
                                  size_t *used_columns) {
    size_t bytes = 0;
    if (text)
        while (text[bytes])
            bytes++;
    return dispatch_utf8_take_columns_n(text, bytes, columns, used_columns);
}
