/*
  Hatari - netusbee/core/Pacing.hpp

  This file is distributed under the GNU General Public License, version 3
  or at your option any later version. Read the file gpl-3.0.txt for details.

  NeoST's CPU clock, for the NeoST devices in Hatari: the rate of the clock
  the glue gives the ISP1160 (Hatari's CPU cycles, netusbee.cpp).
*/
#pragma once
#include <cstdint>

namespace neost {
namespace pacing {
int64_t cpuHzInt();	// netusbee.cpp
}  // namespace pacing
}  // namespace neost
