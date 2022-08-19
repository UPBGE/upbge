/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_Rect.h
 *  \ingroup bgerast
 */

#pragma once

#include <iostream>

/**
 * \section interface class.
 * RAS_Rect just encodes a simple rectangle.
 * \note Should be part of a generic library
 */
class RAS_Rect {
 private:
  int m_x1;
  int m_y1;
  int m_x2;
  int m_y2;

 public:
  explicit RAS_Rect(int x1, int y1, int x2, int y2) : m_x1(x1), m_y1(y1), m_x2(x2), m_y2(y2)
  {
  }

  explicit RAS_Rect(int w, int h) : RAS_Rect(0, 0, w, h)
  {
  }

  explicit RAS_Rect() : RAS_Rect(0, 0, 0, 0)
  {
  }

  int GetWidth() const
  {
    return m_x2 - m_x1;
  }
  int GetHeight() const
  {
    return m_y2 - m_y1;
  }
  int GetLeft() const
  {
    return m_x1;
  }
  int GetRight() const
  {
    return m_x2;
  }
  int GetBottom() const
  {
    return m_y1;
  }
  int GetTop() const
  {
    return m_y2;
  }

  void SetLeft(int x1)
  {
    m_x1 = x1;
  }
  void SetBottom(int y1)
  {
    m_y1 = y1;
  }
  void SetRight(int x2)
  {
    m_x2 = x2;
  }
  void SetTop(int y2)
  {
    m_y2 = y2;
  }

  /// Pack to a OpenGL like viewport with width and height for the two last components.
  void Pack(int array[4]) const
  {
    array[0] = GetLeft();
    array[1] = GetBottom();
    array[2] = GetWidth();
    array[3] = GetHeight();
  }
};

inline std::ostream &operator<<(std::ostream &os, const RAS_Rect &rect)
{
  os << "(" << rect.GetLeft() << ", " << rect.GetBottom() << ", " << rect.GetRight() << ", "
     << rect.GetTop() << ")";
  return os;
}
