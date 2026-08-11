/* Unit tests for the UTF-8 display-width and slicing helpers. */

#include "dispatch_utf8.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *label, size_t actual, size_t expected) {
    fprintf(stderr, "FAIL: %s: got %zu, expected %zu\n", label, actual,
            expected);
    failures++;
}

static void expect(const char *label, size_t actual, size_t expected) {
    if (actual != expected)
        fail(label, actual, expected);
}

static void test_width_ascii(void) {
    expect("empty width", dispatch_utf8_width(""), 0);
    expect("ascii width", dispatch_utf8_width("hello"), 5);
    expect("null width", dispatch_utf8_width(NULL), 0);
}

static void test_width_accented(void) {
    /* "revisión" is 8 characters in 9 bytes; "Ándale" is 6 in 7. */
    expect("accented width", dispatch_utf8_width("revisi\xc3\xb3n"), 8);
    expect("accented bytes", strlen("revisi\xc3\xb3n"), 9);
    expect("leading accent width", dispatch_utf8_width("\xc3\x81ndale"), 6);
    /* Every Latin-1 supplement letter is one column. */
    expect("umlaut width", dispatch_utf8_width("\xc3\xbc\xc3\xb1"), 2);
}

static void test_width_wide_and_combining(void) {
    /* U+4E16 U+754C: two CJK ideographs, two columns each. */
    expect("cjk width", dispatch_utf8_width("\xe4\xb8\x96\xe7\x95\x8c"), 4);
    /* "e" followed by U+0301 COMBINING ACUTE ACCENT is one column. */
    expect("combining width", dispatch_utf8_width("e\xcc\x81"), 1);
    expect("combining bytes", strlen("e\xcc\x81"), 3);
}

static void test_width_invalid_bytes(void) {
    /* A lone continuation byte and a truncated sequence each measure as one
     * column rather than derailing the whole measurement. */
    expect("stray continuation", dispatch_utf8_width("a\x80" "b"), 3);
    expect("truncated sequence", dispatch_utf8_width("a\xc3"), 2);
    /* Overlong encoding of '/' must not decode as U+002F. */
    expect("overlong", dispatch_utf8_width("\xc0\xaf"), 2);
}

static void test_width_n(void) {
    const char *text = "revisi\xc3\xb3n";
    expect("width_n whole", dispatch_utf8_width_n(text, strlen(text)), 8);
    expect("width_n prefix", dispatch_utf8_width_n(text, 6), 6);
    /* Stopping between the two bytes of "ó" must not count half a character
     * as a full column. */
    expect("width_n split", dispatch_utf8_width_n(text, 7), 7);
    expect("width_n past end", dispatch_utf8_width_n(text, 100), 8);
}

static void test_char_bytes(void) {
    expect("ascii char bytes", dispatch_utf8_char_bytes("abc"), 1);
    expect("two byte char", dispatch_utf8_char_bytes("\xc3\xb3n"), 2);
    expect("three byte char", dispatch_utf8_char_bytes("\xe4\xb8\x96"), 3);
    expect("four byte char", dispatch_utf8_char_bytes("\xf0\x9f\x9a\x80"), 4);
    expect("empty char bytes", dispatch_utf8_char_bytes(""), 0);
}

static void test_prev_char_bytes(void) {
    const char *text = "revisi\xc3\xb3n";
    size_t len = strlen(text);
    /* Trailing "n". */
    expect("prev ascii", dispatch_utf8_prev_char_bytes(text, len), 1);
    /* Then the two-byte "ó". */
    expect("prev multibyte", dispatch_utf8_prev_char_bytes(text, len - 1), 2);
    expect("prev at start", dispatch_utf8_prev_char_bytes(text, 0), 0);

    const char *emoji = "a\xf0\x9f\x9a\x80";
    expect("prev four byte", dispatch_utf8_prev_char_bytes(emoji, 5), 4);

    /* A stray continuation byte is its own character, so backspace removes
     * exactly one byte and the buffer cannot get stuck. */
    const char *broken = "a\x80";
    expect("prev stray byte", dispatch_utf8_prev_char_bytes(broken, 2), 1);

    /* Deleting one character at a time drains the buffer without ever
     * landing mid-sequence. */
    size_t offset = strlen(text);
    size_t deletions = 0;
    while (offset > 0) {
        size_t back = dispatch_utf8_prev_char_bytes(text, offset);
        if (back == 0 || back > offset) {
            fail("prev drain step", back, 1);
            break;
        }
        offset -= back;
        deletions++;
    }
    expect("prev drain count", deletions, 8);
}

static void test_take_columns(void) {
    const char *text = "revisi\xc3\xb3n";
    size_t used = 0;

    /* Six columns is a clean ASCII cut. */
    expect("take six", dispatch_utf8_take_columns(text, 6, &used), 6);
    expect("take six used", used, 6);

    /* Seven columns must include the whole two-byte "ó": 7 bytes. */
    expect("take seven", dispatch_utf8_take_columns(text, 7, &used), 8);
    expect("take seven used", used, 7);

    expect("take all", dispatch_utf8_take_columns(text, 99, &used), 9);
    expect("take all used", used, 8);
    expect("take none", dispatch_utf8_take_columns(text, 0, &used), 0);
    expect("take none used", used, 0);

    /* A wide character that would straddle the limit is left out entirely,
     * so the caller learns it has a spare column to pad. */
    const char *cjk = "\xe4\xb8\x96\xe7\x95\x8c";
    expect("take odd wide", dispatch_utf8_take_columns(cjk, 3, &used), 3);
    expect("take odd wide used", used, 2);
    expect("take one wide", dispatch_utf8_take_columns(cjk, 1, &used), 0);
    expect("take one wide used", used, 0);

    /* Combining marks ride along with the character they follow. */
    const char *combining = "e\xcc\x81x";
    expect("take combining", dispatch_utf8_take_columns(combining, 1, &used),
           3);
    expect("take combining used", used, 1);

    expect("take null", dispatch_utf8_take_columns(NULL, 5, &used), 0);
}

/* Slicing a string at every column budget must never produce a byte offset
 * that lands inside a multibyte sequence. */
static void test_take_columns_never_splits(void) {
    const char *text = "caf\xc3\xa9 \xe4\xb8\x96 na\xc3\xafve";
    size_t width = dispatch_utf8_width(text);

    for (size_t columns = 0; columns <= width + 2; columns++) {
        size_t used = 0;
        size_t offset = dispatch_utf8_take_columns(text, columns, &used);
        if (offset > strlen(text)) {
            fail("slice past end", offset, strlen(text));
            return;
        }
        if (((unsigned char)text[offset] & 0xC0) == 0x80) {
            fail("slice split a character", columns, offset);
            return;
        }
        if (used > columns) {
            fail("slice overflowed budget", used, columns);
            return;
        }
        if (dispatch_utf8_width_n(text, offset) != used) {
            fail("slice width disagrees",
                 dispatch_utf8_width_n(text, offset), used);
            return;
        }
    }
}

static void test_take_columns_n(void) {
    /* Wrapping stops at the segment end even though the buffer continues,
     * so a newline is never swallowed into the preceding row. */
    const char *text = "caf\xc3\xa9\nnext";
    size_t used = 0;
    size_t seg = 5; /* "café" in bytes, up to but not including the newline */

    expect("take_n whole segment",
           dispatch_utf8_take_columns_n(text, seg, 10, &used), 5);
    expect("take_n whole segment used", used, 4);
    expect("take_n exact fit",
           dispatch_utf8_take_columns_n(text, seg, 4, &used), 5);
    expect("take_n short",
           dispatch_utf8_take_columns_n(text, seg, 3, &used), 3);
    expect("take_n short used", used, 3);

    /* A byte limit landing inside a character leaves an incomplete sequence,
     * which decodes as one replacement column per byte rather than being
     * dropped. Callers slice on segment boundaries, so this only guarantees
     * the function stays total on a mid-character limit. */
    expect("take_n split byte limit",
           dispatch_utf8_take_columns_n(text, 4, 10, &used), 4);
    expect("take_n split byte limit used", used, 4);
}

int main(void) {
    test_width_ascii();
    test_width_accented();
    test_width_wide_and_combining();
    test_width_invalid_bytes();
    test_width_n();
    test_char_bytes();
    test_prev_char_bytes();
    test_take_columns();
    test_take_columns_n();
    test_take_columns_never_splits();

    if (failures) {
        fprintf(stderr, "utf8_width_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("utf8_width_test: ok\n");
    return 0;
}
