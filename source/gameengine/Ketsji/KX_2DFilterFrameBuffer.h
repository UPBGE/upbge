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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_2DFilter.h
 *  \ingroup ketsji
 */

#pragma once

#include "EXP_Value.h"
#include "RAS_2DFilterFrameBuffer.h"

class KX_2DFilterFrameBuffer : public EXP_Value, public RAS_2DFilterFrameBuffer {
  Py_Header public : KX_2DFilterFrameBuffer(unsigned short colorSlots,
                                            Flag flag,
                                            unsigned int width,
                                            unsigned int height);
  virtual ~KX_2DFilterFrameBuffer();

  virtual std::string GetName();

#ifdef WITH_PYTHON

  EXP_PYMETHOD_VARARGS(KX_2DFilterFrameBuffer, GetColorTexture);
  EXP_PYMETHOD_VARARGS(KX_2DFilterFrameBuffer, GetDepthTexture);
  static PyObject *pyattr_get_width(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_height(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_colorBindCodes(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_depthBindCode(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef);

#endif
};
