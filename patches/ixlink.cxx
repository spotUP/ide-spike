/* Spike link shims: symbols gcc16's libstdc++ (built for a newlib/libnix-style C
   library) references that ixemul's libc lacks. Single-threaded, never hit on the
   editor's normal paths; each one is a stub to be replaced by a real ixemul
   function if a runtime path needs it. */
#include <stdlib.h>
#include <wchar.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string.h>


extern "C" {
static char impure_dummy[512];
void *_impure_ptr = impure_dummy; /* libstdc++ stdio users (terminate message) */

unsigned int __atomic_fetch_add_4(volatile void *p, unsigned int v, int mo) {
	volatile unsigned int *q = (volatile unsigned int *)p; unsigned int o = *q; *q = o + v; return o;
}
int __xpg_strerror_r(int e, char *b, size_t n) { const char *m = strerror(e); strncpy(b, m, n); if (n) b[n-1] = 0; return 0; }
int __locale_mb_cur_max(void) { return 1; }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n) { return (wchar_t *)memmove(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n) { return (wchar_t *)memcpy(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n) { size_t i; for (i = 0; i < n; i++) d[i] = c; return d; }
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) { size_t i; for (i = 0; i < n; i++) if (s[i] == c) return (wchar_t *)s + i; return 0; }
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) { size_t i; for (i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
size_t wcslen(const wchar_t *s) { size_t n = 0; while (s[n]) n++; return n; }
int wcscoll(const wchar_t *a, const wchar_t *b) { return wmemcmp(a, b, wcslen(a) + 1); }
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n) { size_t l = wcslen(s); if (n > l) wmemcpy(d, s, l + 1); return l; }
size_t wcsftime(wchar_t *s, size_t m, const wchar_t *f, const void *t) { return 0; }
float strtof(const char *s, char **e) { return (float)strtod(s, e); }
long double strtold(const char *s, char **e) { return strtod(s, e); }
}
