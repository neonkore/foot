#include "my-wcwidth.h"

#if FOOT_SYSTEM_WCWIDTH == 0

#include <stdlib.h>

#define LOG_MODULE "wcwidth"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "util.h"

#include "my-wcwidth-tables.h"

UNITTEST
{
    uint32_t last_stop;

    last_stop = ucs_invalid[0].stop;
    xassert(last_stop >= ucs_invalid[0].start);

    for (size_t i = 1; i < ALEN(ucs_invalid); i++) {
        uint32_t start = ucs_invalid[i].start;
        uint32_t stop = ucs_invalid[i].stop;

        xassert(stop >= start);
        xassert(start > last_stop);

        last_stop = stop;
    }

    last_stop = ucs_zero_width[0].stop;
    xassert(last_stop >= ucs_zero_width[0].start);

    for (size_t i = 1; i < ALEN(ucs_zero_width); i++) {
        uint32_t start = ucs_zero_width[i].start;
        uint32_t stop = ucs_zero_width[i].stop;

        xassert(stop >= start);
        xassert(start > last_stop);

        last_stop = stop;
    }

    last_stop = ucs_double_width[0].stop;
    xassert(last_stop >= ucs_double_width[0].start);

    for (size_t i = 1; i < ALEN(ucs_double_width); i++) {
        uint32_t start = ucs_double_width[i].start;
        uint32_t stop = ucs_double_width[i].stop;

        xassert(stop >= start);
        xassert(start > last_stop);

        last_stop = stop;
    }
}

static int
ucs_compar(const void *_key, const void *_range)
{
    uint32_t key = (uintptr_t)_key;
    const struct ucs_range *range = _range;

    if (key < range->start)
        return -1;
    else if (key > range->stop)
        return 1;
    else
        return 0;
}

IGNORE_WARNING("-Wpedantic")

int
my_wcwidth(wchar_t wc)
{
#define lookup(table)                                           \
    wc >= table[0].start &&                                     \
    wc <= table[ALEN(table) - 1].stop &&                        \
    bsearch(key, table, ALEN(table), sizeof(table[0]), &ucs_compar) != NULL

    if (unlikely(wc == 0))
        return 0;

    else if (unlikely(wc < 32 || (wc >= 0x7f && wc < 0xa0)))  /* C0/C1/DEL */
        return -1;

    else if (unlikely(wc == 0xad)) { /* SOFT HYPHEN */
        /* TODO: return 0 instead? */
        return 1;
    }

    else {
        const void *key = (const void *)(uintptr_t)wc;

        if (unlikely(lookup(ucs_double_width)))
            return 2;

        if (unlikely(lookup(ucs_zero_width)))
            return 0;

        if (unlikely(lookup(ucs_invalid)))
            return -1;

#undef lookup

        return 1;
    }
}

UNITTEST
{
    xassert(my_wcwidth(L'a') == 1);
    xassert(my_wcwidth(L'🥲') == 2);
    xassert(my_wcwidth(L'­') == 1);  /* SOFT HYPHEN */
}

UNIGNORE_WARNINGS

int
my_wcswidth(const wchar_t *s, size_t n)
{
    int width = 0;

    for (; *s != L'\0' && n-- > 0; s++) {
        int w = my_wcwidth(*s);
        if (w < 0)
            return -1;
        width += w;
    }

    return width;
}

#endif /* FOOT_SYSTEM_WCWIDTH == 0 */
