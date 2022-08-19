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
 * Contributor(s): Mitchell Stokes
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_LibLoadStatus.h
 *  \ingroup bgeconv
 */

#pragma once

#include "EXP_PyObjectPlus.h"

class KX_LibLoadStatus : public EXP_PyObjectPlus {
  Py_Header private : class BL_Converter *m_converter;
  class KX_KetsjiEngine *m_engine;
  class KX_Scene *m_mergescene;
  void *m_data;
  std::string m_libname;

  float m_progress;
  double m_starttime;
  double m_endtime;

  // The current status of this libload, used by the scene converter.
  bool m_finished;

#ifdef WITH_PYTHON
  PyObject *m_finish_cb;
  PyObject *m_progress_cb;
#endif

 public:
  KX_LibLoadStatus(class BL_Converter *kx_converter,
                   class KX_KetsjiEngine *kx_engine,
                   class KX_Scene *merge_scene,
                   const std::string &path);

  void Finish();  // Called when the libload is done
  void RunFinishCallback();
  void RunProgressCallback();

  class BL_Converter *GetConverter();
  class KX_KetsjiEngine *GetEngine();
  class KX_Scene *GetMergeScene();

  void SetData(void *data);
  void *GetData();

  inline bool IsFinished() const
  {
    return m_finished;
  }

  void SetProgress(float progress);
  float GetProgress();
  void AddProgress(float progress);

#ifdef WITH_PYTHON
  static PyObject *pyattr_get_onfinish(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_onfinish(EXP_PyObjectPlus *self_v,
                                 const EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value);
  static PyObject *pyattr_get_onprogress(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_onprogress(EXP_PyObjectPlus *self_v,
                                   const EXP_PYATTRIBUTE_DEF *attrdef,
                                   PyObject *value);

  static PyObject *pyattr_get_timetaken(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
#endif
};
