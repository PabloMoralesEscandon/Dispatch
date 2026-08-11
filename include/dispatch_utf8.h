#ifndef DISPATCH_UTF8_H
#define DISPATCH_UTF8_H

#include <stddef.h>

/* Display-width and slicing helpers for UTF-8 text.
 *
 * Decoding and width are computed from the byte string itself rather than
 * through mbrtowc/wcwidth, so results do not depend on the locale the process
 * happens to run under. Golden TUI frames and CLI column alignment therefore
 * reproduce identically on a developer machine and on a CI runner with no
 * LANG set.
 *
 * Every function is total: bytes that are not a valid UTF-8 sequence are
 * treated as one single-column character each, so callers always make
 * progress and never slice a well-formed sequence in half. */

/* Display columns occupied by the first `bytes` bytes of `text`.
 * Combining marks count zero; East Asian wide characters count two. */
size_t dispatch_utf8_width_n(const char *text, size_t bytes);

/* Display columns occupied by the whole NUL-terminated string. */
size_t dispatch_utf8_width(const char *text);

/* Byte length of the character starting at `text`. Returns 0 at the string
 * terminator. */
size_t dispatch_utf8_char_bytes(const char *text);

/* Byte length of the character that ends at byte offset `offset` — what a
 * backspace must remove to delete one whole character. Returns 0 when
 * `offset` is 0. */
size_t dispatch_utf8_prev_char_bytes(const char *text, size_t offset);

/* Byte offset of the longest prefix of `text` that fits in `columns` display
 * columns without splitting a character. A wide character that would straddle
 * the limit is left out entirely. When `used_columns` is non-NULL it receives
 * the columns that prefix actually occupies, which can be less than `columns`
 * for exactly that reason. */
size_t dispatch_utf8_take_columns(const char *text, size_t columns,
                                  size_t *used_columns);

/* As dispatch_utf8_take_columns, but never looks past the first `bytes` bytes.
 * Use this to slice one segment of a larger buffer — wrapping a line that ends
 * at a newline, say — where the terminator is not where the segment ends. */
size_t dispatch_utf8_take_columns_n(const char *text, size_t bytes,
                                    size_t columns, size_t *used_columns);

#endif
