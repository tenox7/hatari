/*
  Hatari - uae/types.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The basic types of the UAE CPU core (written anew for Hatari).
*/

#ifndef UAE_TYPES_H
#define UAE_TYPES_H

#include <stdint.h>

/* Fixed size integers */
typedef int8_t   uae_s8;
typedef uint8_t  uae_u8;
typedef int16_t  uae_s16;
typedef uint16_t uae_u16;
typedef int32_t  uae_s32;
typedef uint32_t uae_u32;
/* long long, so that "%lld" fits on all hosts */
#ifndef uae_s64
typedef long long int uae_s64;
#endif
#ifndef uae_u64
typedef unsigned long long int uae_u64;
#endif
#ifdef HAVE___UINT128_T
#define HAVE_UAE_U128
typedef __uint128_t uae_u128;
#endif

/* The C++ code of the core uses bool */
#ifndef __cplusplus
#include <stdbool.h>
#endif

/* A guest address (32 or 24 bits), whatever the host is */
typedef uae_u32 uaecptr;

/* Strings: char, UTF-8 (TCHAR is wchar_t only with UNICODE on Windows) */
typedef char uae_char;
#ifdef _WIN32
# include <tchar.h>
# ifdef UNICODE
#  define SIZEOF_TCHAR 2
# else
#  define SIZEOF_TCHAR 1
# endif
#else
typedef char TCHAR;
# define SIZEOF_TCHAR 1
#endif
#ifndef _T
# if SIZEOF_TCHAR == 1
#  define _T(x) x
# else
#  define _T(x) L##x
# endif
#endif

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE (!FALSE)
#endif

/* Event time */
typedef signed long long evt_t;

#endif /* UAE_TYPES_H */
