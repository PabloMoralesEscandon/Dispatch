# Pin a UTF-8 locale for tests that render or capture terminal text.
#
# The headless renderer decodes text through the locale, exactly as a real
# terminal does. Under the C locale ncurses splits each accented character
# into separate cells, so captured frames would depend on the environment the
# suite happens to run in. Source this file to make that choice explicit and
# to fail loudly when no UTF-8 locale is installed.

for candidate in C.UTF-8 C.utf8 en_US.UTF-8 en_US.utf8; do
    if [ "$(LC_ALL="$candidate" locale charmap 2>/dev/null)" = "UTF-8" ]; then
        export LC_ALL="$candidate"
        export LANG="$candidate"
        break
    fi
done

if [ "$(locale charmap 2>/dev/null)" != "UTF-8" ]; then
    printf 'These tests need a UTF-8 locale; none of C.UTF-8, en_US.UTF-8 is available\n' >&2
    exit 1
fi
