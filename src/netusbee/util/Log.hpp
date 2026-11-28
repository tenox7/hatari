/*
  Hatari - netusbee/util/Log.hpp

  This file is distributed under the GNU General Public License, version 3
  or at your option any later version. Read the file gpl-3.0.txt for details.

  NeoST's log, for the NeoST devices in Hatari: their traces go to stderr.
*/
#pragma once
#include <cstdarg>
#include <cstdio>

namespace neost {
namespace log {
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}
}  // namespace log
}  // namespace neost
