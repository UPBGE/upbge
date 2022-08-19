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
 * Contributor(s):
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Common/CM_Utils.cpp
 *  \ingroup common
 */

#include "CM_Utils.h"

/* Remove the 3 first chars as the object
 * has a prefix now after commit d6cefef98
 */
std::string CM_RemovePrefix(const std::string &propname)
{
  std::string temporal = propname;
  const char *p = temporal.c_str();
  if (*(p + 2) == ' ') {
    if (*(p + 1) == ' ' || *(p + 1) == 'F' || *(p + 1) == '0') {
      if (*(p + 0) == ' ' || *(p + 0) == 'M' || *(p + 0) == 'L' || *(p + 0) == 'O') {
        temporal.erase(0, 3);
      }
    }
  }
  if (*(p + 1) == ' ') {
    if (*(p + 0) == ' ' || *(p + 1) == 'F' || *(p + 1) == '0') {
      temporal.erase(0, 2);
    }
  }
  return temporal;
}
