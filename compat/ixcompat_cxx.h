/* Spike shim: declarations libstdc++ 16 expects but ixemul's headers lack.
   Declared only; never defined, so any real use is a link error. */
#ifndef IXCOMPAT_CXX_H
#define IXCOMPAT_CXX_H
#ifdef __cplusplus
#include <stddef.h>
#include <stdarg.h>
extern "C" {
struct __sFILE;
int vfscanf(struct __sFILE *, const char *, __builtin_va_list);
}
extern "C" {
int at_quick_exit(void (*)(void));
void quick_exit(int) __attribute__((noreturn));
void _Exit(int) __attribute__((noreturn));
float strtof(const char *, char **);
long double strtold(const char *, char **);
long wcstol(const wchar_t *, wchar_t **, int);
unsigned long wcstoul(const wchar_t *, wchar_t **, int);
long long wcstoll(const wchar_t *, wchar_t **, int);
unsigned long long wcstoull(const wchar_t *, wchar_t **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);

float wcstof(const wchar_t *, wchar_t **);
double wcstod(const wchar_t *, wchar_t **);
long double wcstold(const wchar_t *, wchar_t **);
}
#include "/Users/spot/opt/amiga/m68k-amigaos/ixemul/include/wchar.h"
/* skip libstdc++'s <cwchar>: ixemul's wchar.h is the minimal C95 subset */
#define _GLIBCXX_CWCHAR 1
extern "C" {
size_t wcslen(const wchar_t *);
wchar_t *wmemcpy(wchar_t *, const wchar_t *, size_t);
wchar_t *wmemmove(wchar_t *, const wchar_t *, size_t);
wchar_t *wmemset(wchar_t *, wchar_t, size_t);
wchar_t *wmemchr(const wchar_t *, wchar_t, size_t);
int wmemcmp(const wchar_t *, const wchar_t *, size_t);
}
namespace std {
using ::wcstol; using ::wcstoul; using ::wcstoll; using ::wcstoull; using ::wcstof; using ::wcstod; using ::wcstold;
using ::mbstate_t; using ::wint_t; using ::wcslen; using ::wmemcpy; using ::wmemmove;
using ::wmemset; using ::wmemchr; using ::wmemcmp; using ::btowc; using ::wctob;
using ::mbrtowc; using ::wcrtomb; using ::mbrlen; using ::mbsinit;
}
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
/* errno values libstdc++ <system_error> names but ixemul's errno.h lacks (values arbitrary, never produced) */
#ifndef ENOMSG
#define ENOMSG 200
#endif
#ifndef EIDRM
#define EIDRM 201
#endif
/* C99 rounding the std:: namespace lacks; defined in compat/c99math.c */
extern "C" { double round(double); long lround(double); }
namespace std { using ::round; using ::lround; }
#endif
#endif
