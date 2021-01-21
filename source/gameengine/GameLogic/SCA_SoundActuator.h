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

/** \file SCA_SoundActuator.h
 *  \ingroup ketsji
 */

#pragma once

#include "BKE_sound.h"

#include "SCA_IActuator.h"

#ifdef WITH_AUDASPACE
#  include <AUD_Handle.h>
#  include <AUD_Sound.h>
#endif

typedef struct KX_3DSoundSettings {
  float min_gain;
  float max_gain;
  float reference_distance;
  float max_distance;
  float rolloff_factor;
  float cone_inner_angle;
  float cone_outer_angle;
  float cone_outer_gain;
} KX_3DSoundSettings;

class SCA_SoundActuator : public SCA_IActuator {
  Py_Header bool m_isplaying;
#ifdef WITH_AUDASPACE
  AUD_Sound *m_sound;
  AUD_Handle *m_handle;
#endif  // WITH_AUDASPACE
  float m_volume;
  float m_pitch;
  bool m_is3d;
  KX_3DSoundSettings m_3d;

  void play();

 public:
  enum KX_SOUNDACT_TYPE {
    KX_SOUNDACT_NODEF = 0,
    KX_SOUNDACT_PLAYSTOP,
    KX_SOUNDACT_PLAYEND,
    KX_SOUNDACT_LOOPSTOP,
    KX_SOUNDACT_LOOPEND,
    KX_SOUNDACT_LOOPBIDIRECTIONAL,
    KX_SOUNDACT_LOOPBIDIRECTIONAL_STOP,
    KX_SOUNDACT_MAX
  };

  KX_SOUNDACT_TYPE m_type;

  SCA_SoundActuator(SCA_IObject *gameobj,
#ifdef WITH_AUDASPACE
                    AUD_Sound *sound,
#endif  // WITH_AUDASPACE
                    float volume,
                    float pitch,
                    bool is3d,
                    KX_3DSoundSettings settings,
                    KX_SOUNDACT_TYPE type);

  ~SCA_SoundActuator();

  virtual bool Update(double curtime);

  EXP_Value *GetReplica();
  void ProcessReplica();

#ifdef WITH_PYTHON

  /* -------------------------------------------------------------------- */
  /* Python interface --------------------------------------------------- */
  /* -------------------------------------------------------------------- */

  EXP_PYMETHOD_DOC_NOARGS(SCA_SoundActuator, startSound);
  EXP_PYMETHOD_DOC_NOARGS(SCA_SoundActuator, pauseSound);
  EXP_PYMETHOD_DOC_NOARGS(SCA_SoundActuator, stopSound);

  static int pyattr_set_3d_property(EXP_PyObjectPlus *self,
                                    const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                    PyObject *value);
  static int pyattr_set_audposition(EXP_PyObjectPlus *self,
                                    const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                    PyObject *value);
  static int pyattr_set_gain(EXP_PyObjectPlus *self,
                             const struct EXP_PYATTRIBUTE_DEF *attrdef,
                             PyObject *value);
  static int pyattr_set_pitch(EXP_PyObjectPlus *self,
                              const struct EXP_PYATTRIBUTE_DEF *attrdef,
                              PyObject *value);
  static int pyattr_set_type(EXP_PyObjectPlus *self,
                             const struct EXP_PYATTRIBUTE_DEF *attrdef,
                             PyObject *value);
  static int pyattr_set_sound(EXP_PyObjectPlus *self,
                              const struct EXP_PYATTRIBUTE_DEF *attrdef,
                              PyObject *value);

  static PyObject *pyattr_get_3d_property(EXP_PyObjectPlus *self,
                                          const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_audposition(EXP_PyObjectPlus *self,
                                          const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_gain(EXP_PyObjectPlus *self,
                                   const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_pitch(EXP_PyObjectPlus *self,
                                    const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_type(EXP_PyObjectPlus *self,
                                   const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_sound(EXP_PyObjectPlus *self,
                                    const struct EXP_PYATTRIBUTE_DEF *attrdef);

#endif /* WITH_PYTHON */
};
