/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file FilterBlueScreen.h
 *  \ingroup bgevideotex
 */

#pragma once

#include "Common.h"
#include "FilterBase.h"

/// pixel filter for blue screen
class FilterBlueScreen : public FilterBase {
 public:
  /// constructor
  FilterBlueScreen(void);
  /// destructor
  virtual ~FilterBlueScreen(void)
  {
  }

  /// get color
  unsigned char *getColor(void)
  {
    return m_color;
  }
  /// set color
  void setColor(unsigned char red, unsigned char green, unsigned char blue);

  /// get limits for color variation
  unsigned short *getLimits(void)
  {
    return m_limits;
  }
  /// set limits for color variation
  void setLimits(unsigned short minLimit, unsigned short maxLimit);

 protected:
  ///  blue screen color (red component first)
  unsigned char m_color[3];
  /// limits for color variation - first defines, where ends fully transparent
  /// color, second defines, where begins fully opaque color
  unsigned short m_limits[2];
  /// squared limits for color variation
  unsigned int m_squareLimits[2];
  /// distance between squared limits
  unsigned int m_limitDist;

  /// filter pixel template, source int buffer
  template<class SRC>
  unsigned int tFilter(
      SRC src, short x, short y, short *size, unsigned int pixSize, unsigned int val)
  {
    // calculate differences
    int difRed = int(VT_R(val)) - int(m_color[0]);
    int difGreen = int(VT_G(val)) - int(m_color[1]);
    int difBlue = int(VT_B(val)) - int(m_color[2]);
    // calc distance from "blue screen" color
    unsigned int dist = (unsigned int)(difRed * difRed + difGreen * difGreen + difBlue * difBlue);
    // condition for fully transparent color
    if (m_squareLimits[0] >= dist)
      // return color with zero alpha
      VT_A(val) = 0;
    // condition for fully opaque color
    else if (m_squareLimits[1] <= dist)
      // return normal color
      VT_A(val) = 0xFF;
    // otherwise calc alpha
    else
      VT_A(val) = (((dist - m_squareLimits[0]) << 8) / m_limitDist);
    return val;
  }

  /// virtual filtering function for byte source
  virtual unsigned int filter(unsigned char *src,
                              short x,
                              short y,
                              short *size,
                              unsigned int pixSize,
                              unsigned int val = 0)
  {
    return tFilter(src, x, y, size, pixSize, val);
  }
  /// virtual filtering function for unsigned int source
  virtual unsigned int filter(
      unsigned int *src, short x, short y, short *size, unsigned int pixSize, unsigned int val = 0)
  {
    return tFilter(src, x, y, size, pixSize, val);
  }
};
