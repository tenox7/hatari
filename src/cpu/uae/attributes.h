/*
  Hatari - uae/attributes.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Function attributes of the UAE CPU core (written anew for Hatari).
*/

#ifndef UAE_ATTRIBUTES_H
#define UAE_ATTRIBUTES_H

/* The C calling convention, where the host has several */
#if defined(_WIN32)
# define uae_cdecl __cdecl
#elif defined(__GNUC__) && defined(__i386__)
# define uae_cdecl __attribute__((cdecl))
#else
# define uae_cdecl
#endif

/* printf-like functions: the compiler checks their arguments */
#ifdef __GNUC__
# define UAE_PRINTF_FORMAT(f, a) __attribute__((format(printf, f, a)))
#else
# define UAE_PRINTF_FORMAT(f, a)
#endif
#define UAE_WPRINTF_FORMAT(f, a)

#endif /* UAE_ATTRIBUTES_H */
