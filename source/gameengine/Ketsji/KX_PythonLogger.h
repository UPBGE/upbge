/**
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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#pragma once

#include "EXP_Value.h"

class KX_PythonLogger : public EXP_Value {

 private:
  PyObject *m_logger;

 public:
  KX_PythonLogger();

  virtual ~KX_PythonLogger();

  PyObject *GetLogger();

  void LogError();

  void ProcessReplica();

  static PyObject *pyattr_get_logger(EXP_PyObjectPlus *self_v,
                                     const EXP_PYATTRIBUTE_DEF *attrdef);

  static PyObject *pyattr_get_logger_name(EXP_PyObjectPlus *self_v, 
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
};
