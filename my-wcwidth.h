#pragma once
#include <wchar.h>

#if FOOT_SYSTEM_WCWIDTH == 0

int my_wcwidth(wchar_t wc);
int my_wcswidth(const wchar_t *s, size_t n);

#else

static inline int my_wcwidth(wchar_t wc) { return wcwidth(wc); }
static inline int my_wcswidth(const wchar_t *s, size_t n) { return wcswidth(s, n); }

#endif
