/*
 * SCA_SoundActuator.cpp
 *
 *
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
 *
 */

/** \file gameengine/Ketsji/SCA_SoundActuator.cpp
 *  \ingroup ketsji
 */

#include "SCA_SoundActuator.h"

#ifdef WITH_AUDASPACE
typedef float sample_t;
#  include <Exception.h>
#  include <devices/IHandle.h>
#  include <devices/I3DHandle.h>
#  include <devices/DeviceManager.h>
#  include <sequence/PingPong.h>
#  include <util/StreamBuffer.h>
#  include <file/File.h>
#  include <python/PyAPI.h>
#  include <devices/IDevice.h>
#endif

#include "KX_Camera.h"
#include "KX_Globals.h"

using namespace blender;

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */
SCA_SoundActuator::SCA_SoundActuator(SCA_IObject *gameobj,
#ifdef WITH_AUDASPACE
                                     AUD_Sound sound,
#endif  // WITH_AUDASPACE
                                     float volume,
                                     float pitch,
                                     bool is3d,
                                     bool preload,
                                     KX_3DSoundSettings settings,
                                     KX_SOUNDACT_TYPE type)  //,
    : SCA_IActuator(gameobj, KX_ACT_SOUND)
{
#ifdef WITH_AUDASPACE
  m_sound = sound; /* shared_ptr copy */
  m_handle.reset();
  // No prepared buffer yet; will be built if m_preload is true.
  m_prepared.reset();
#endif  // WITH_AUDASPACE

  m_volume = volume;
  m_pitch = pitch;
  m_is3d = is3d;
  m_3d = settings;
  m_type = type;
  m_isplaying = false;

  // Default: enable RAM buffering for snappy repeated triggers.
  m_preload = preload;

#ifdef WITH_AUDASPACE
  if (m_preload && m_sound) {
    // Buffer the whole sound in memory for fast replay.
    try {
      m_prepared = std::make_shared<aud::StreamBuffer>(m_sound);
    }
    catch (aud::Exception &) {
      m_prepared.reset();
    }
  }
#endif
}

SCA_SoundActuator::~SCA_SoundActuator()
{
#ifdef WITH_AUDASPACE
  if (m_handle) {
    m_handle->stop();
    m_handle.reset();
  }

  m_prepared.reset();
  m_sound.reset();
#endif  // WITH_AUDASPACE
}

void SCA_SoundActuator::play()
{
#ifdef WITH_AUDASPACE
  if (m_handle) {
    m_handle->stop();
    m_handle.reset();
  }

  // If nothing to play, bail early.
  if (!m_sound && !m_prepared)
    return;

  // Base sound used for playback. Prefer the pre-buffered version.
  AUD_Sound base = m_prepared ? m_prepared : m_sound;
  // This is the sound actually passed to the device (may be wrapped).
  AUD_Sound sound = base;

  bool loop = false;

  switch (m_type) {
    case KX_SOUNDACT_LOOPBIDIRECTIONAL:
    case KX_SOUNDACT_LOOPBIDIRECTIONAL_STOP: {
      // Wrap the base into a pingpong sound; keep it in a shared_ptr.
      sound = std::make_shared<aud::PingPong>(base);
      ATTR_FALLTHROUGH;
    }
    case KX_SOUNDACT_LOOPEND:
    case KX_SOUNDACT_LOOPSTOP:
      loop = true;
      break;
    case KX_SOUNDACT_PLAYSTOP:
    case KX_SOUNDACT_PLAYEND:
    default:
      break;
  }

  try {
    auto device = aud::DeviceManager::getDevice();
    if (device) {
      m_handle = device->play(sound, false);
    }
  }
  catch (aud::Exception &) {
    m_handle.reset();
  }

  if (m_handle) {
    if (m_is3d) {
      auto h3d = std::dynamic_pointer_cast<aud::I3DHandle>(m_handle);
      if (h3d) {
        h3d->setRelative(true);
        h3d->setVolumeMaximum(m_3d.max_gain);
        h3d->setVolumeMinimum(m_3d.min_gain);
        h3d->setDistanceReference(m_3d.reference_distance);
        h3d->setDistanceMaximum(m_3d.max_distance);
        h3d->setAttenuation(m_3d.rolloff_factor);
        h3d->setConeAngleInner(m_3d.cone_inner_angle);
        h3d->setConeAngleOuter(m_3d.cone_outer_angle);
        h3d->setConeVolumeOuter(m_3d.cone_outer_gain);
      }
    }

    if (loop) {
      m_handle->setLoopCount(-1);
    }
    m_handle->setPitch(m_pitch);
    m_handle->setVolume(m_volume);
  }

  m_isplaying = true;
#endif  // WITH_AUDASPACE
}

EXP_Value *SCA_SoundActuator::GetReplica()
{
  SCA_SoundActuator *replica = new SCA_SoundActuator(*this);
  replica->ProcessReplica();
  return replica;
}

void SCA_SoundActuator::ProcessReplica()
{
  SCA_IActuator::ProcessReplica();
#ifdef WITH_AUDASPACE
  m_handle.reset();
  /* copy shared_ptr */
  // m_sound already copied via default copy-constructor; ensure it's a distinct shared_ptr copy
  // Rebuild buffered copy for the replica if needed.
  if (m_prepared) {
    m_prepared.reset();
  }
  if (m_preload && m_sound) {
    try {
      m_prepared = std::make_shared<aud::StreamBuffer>(m_sound);
    }
    catch (aud::Exception &) {
      m_prepared.reset();
    }
  }
#endif  // WITH_AUDASPACE
}

bool SCA_SoundActuator::Update(double curtime)
{
  bool result = false;

#ifdef WITH_AUDASPACE
  // do nothing on negative events, otherwise sounds are played twice!
  bool bNegativeEvent = IsNegativeEvent();
  bool bPositiveEvent = m_posevent;
#endif  // WITH_AUDASPACE

  RemoveAllEvents();

#ifdef WITH_AUDASPACE
  // Guard: if we have neither original sound nor prepared buffer, nothing to do.
  if (!m_sound && !m_prepared)
    return false;

  // actual audio device playing state
  bool isplaying = m_handle ? (m_handle->getStatus() == aud::STATUS_PLAYING) : false;

  if (bNegativeEvent) {
    // here must be a check if it is still playing
    if (m_isplaying && isplaying) {
      switch (m_type) {
        case KX_SOUNDACT_PLAYSTOP:
        case KX_SOUNDACT_LOOPSTOP:
        case KX_SOUNDACT_LOOPBIDIRECTIONAL_STOP: {
          // stop immediately
          if (m_handle) {
            m_handle->stop();
            m_handle.reset();
          }
          break;
        }
        case KX_SOUNDACT_PLAYEND: {
          // do nothing, sound will stop anyway when it's finished
          break;
        }
        case KX_SOUNDACT_LOOPEND:
        case KX_SOUNDACT_LOOPBIDIRECTIONAL: {
          // stop the looping so that the sound stops when it finished
          if (m_handle)
            m_handle->setLoopCount(0);
          break;
        }
        default:
          // implement me !!
          break;
      }
    }
    // remember that we tried to stop the actuator
    m_isplaying = false;
  }

  // Only trigger playback on an explicit positive event. This preserves the original
  // behavior where sounds are not (re)started unless a positive pulse was received.
  else if (bPositiveEvent) {
    if (!m_isplaying)
      play();
  }
  // verify that the sound is still playing
  isplaying = m_handle ? (m_handle->getStatus() == aud::STATUS_PLAYING) : false;

  if (isplaying) {
    if (m_is3d) {
      KX_Camera *cam = KX_GetActiveScene()->GetActiveCamera();
      if (cam) {
        KX_GameObject *obj = (KX_GameObject *)this->GetParent();
        MT_Vector3 p;
        MT_Matrix3x3 Mo;
        float data[4];

        Mo = cam->NodeGetWorldOrientation().inverse();
        p = (obj->NodeGetWorldPosition() - cam->NodeGetWorldPosition());
        p = Mo * p;
        p.getValue(data);
        auto h3d = std::dynamic_pointer_cast<aud::I3DHandle>(m_handle);
        if (h3d) {
          aud::Vector3 loc(data[0], data[1], data[2]);
          h3d->setLocation(loc);
        }
        p = (obj->GetLinearVelocity() - cam->GetLinearVelocity());
        p = Mo * p;
        p.getValue(data);
        if (h3d) {
          aud::Vector3 vel(data[0], data[1], data[2]);
          h3d->setVelocity(vel);
        }
        (Mo * obj->NodeGetWorldOrientation()).getRotation().getValue(data);
        if (h3d) {
          aud::Quaternion orient(data[0], data[1], data[2], data[3]);
          h3d->setOrientation(orient);
        }
      }
    }
    result = true;
  }
  else {
    m_isplaying = false;
    result = false;
  }
#endif  // WITH_AUDASPACE

  return result;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_SoundActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_SoundActuator",
                                        sizeof(EXP_PyObjectPlus_Proxy),
                                        0,
                                        py_base_dealloc,
                                        0,
                                        0,
                                        0,
                                        0,
                                        py_base_repr,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        Methods,
                                        0,
                                        0,
                                        &SCA_IActuator::Type,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        py_base_new};

PyMethodDef SCA_SoundActuator::Methods[] = {
    EXP_PYMETHODTABLE_NOARGS(SCA_SoundActuator, startSound),
    EXP_PYMETHODTABLE_NOARGS(SCA_SoundActuator, pauseSound),
    EXP_PYMETHODTABLE_NOARGS(SCA_SoundActuator, stopSound),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_SoundActuator::Attributes[] = {
    EXP_PYATTRIBUTE_BOOL_RO("is3D", SCA_SoundActuator, m_is3d),

    // 3D properties
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "volume_maximum", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "volume_minimum", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "distance_reference", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "distance_maximum", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "attenuation", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "cone_angle_inner", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "cone_angle_outer", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "cone_volume_outer", SCA_SoundActuator, pyattr_get_3d_property, pyattr_set_3d_property),

    // Sound handle / playback properties
    EXP_PYATTRIBUTE_RW_FUNCTION("sound", SCA_SoundActuator, pyattr_get_sound, pyattr_set_sound),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "time", SCA_SoundActuator, pyattr_get_audposition, pyattr_set_audposition),
    EXP_PYATTRIBUTE_RW_FUNCTION("volume", SCA_SoundActuator, pyattr_get_gain, pyattr_set_gain),
    EXP_PYATTRIBUTE_RW_FUNCTION("pitch", SCA_SoundActuator, pyattr_get_pitch, pyattr_set_pitch),

    // New toggle to control RAM buffering (default False).
    EXP_PYATTRIBUTE_BOOL_RW("preload", SCA_SoundActuator, m_preload),

    EXP_PYATTRIBUTE_ENUM_RW("mode",
                            SCA_SoundActuator::KX_SOUNDACT_NODEF + 1,
                            SCA_SoundActuator::KX_SOUNDACT_MAX - 1,
                            false,
                            SCA_SoundActuator,
                            m_type),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

/* Methods ----------------------------------------------------------------- */
EXP_PYMETHODDEF_DOC_NOARGS(SCA_SoundActuator,
                           startSound,
                           "startSound()\n"
                           "\tStarts the sound.\n")
{
#  ifdef WITH_AUDASPACE
  if (m_handle) {
    auto status = m_handle->getStatus();
    if (status == aud::STATUS_PLAYING) {
      // already playing
    }
    else if (status == aud::STATUS_PAUSED) {
      m_handle->resume();
    }
    else {
      play();
    }
  }
  else {
    play();
  }
#  endif  // WITH_AUDASPACE

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(SCA_SoundActuator,
                           pauseSound,
                           "pauseSound()\n"
                           "\tPauses the sound.\n")
{
#  ifdef WITH_AUDASPACE
  if (m_handle)
    m_handle->pause();
#  endif  // WITH_AUDASPACE

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(SCA_SoundActuator,
                           stopSound,
                           "stopSound()\n"
                           "\tStops the sound.\n")
{
#  ifdef WITH_AUDASPACE
  if (m_handle) {
    m_handle->stop();
    m_handle.reset();
  }
#  endif  // WITH_AUDASPACE

  Py_RETURN_NONE;
}

/* Atribute setting and getting -------------------------------------------- */
PyObject *SCA_SoundActuator::pyattr_get_3d_property(EXP_PyObjectPlus *self,
                                                    const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);
  const std::string &prop = attrdef->m_name;
  float result_value = 0.0f;

  if (prop == "volume_maximum") {
    result_value = actuator->m_3d.max_gain;
  }
  else if (prop == "volume_minimum") {
    result_value = actuator->m_3d.min_gain;
  }
  else if (prop == "distance_reference") {
    result_value = actuator->m_3d.reference_distance;
  }
  else if (prop == "distance_maximum") {
    result_value = actuator->m_3d.max_distance;
  }
  else if (prop == "attenuation") {
    result_value = actuator->m_3d.rolloff_factor;
  }
  else if (prop == "cone_angle_inner") {
    result_value = actuator->m_3d.cone_inner_angle;
  }
  else if (prop == "cone_angle_outer") {
    result_value = actuator->m_3d.cone_outer_angle;
  }
  else if (prop == "cone_volume_outer") {
    result_value = actuator->m_3d.cone_outer_gain;
  }
  else {
    Py_RETURN_NONE;
  }

  PyObject *result = PyFloat_FromDouble(result_value);
  return result;
}

PyObject *SCA_SoundActuator::pyattr_get_audposition(EXP_PyObjectPlus *self,
                                                    const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  float position = 0.0f;

#  ifdef WITH_AUDASPACE
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);

  if (actuator->m_handle)
    position = static_cast<float>(actuator->m_handle->getPosition());
#  endif  // WITH_AUDASPACE

  PyObject *result = PyFloat_FromDouble(position);

  return result;
}

PyObject *SCA_SoundActuator::pyattr_get_gain(EXP_PyObjectPlus *self,
                                             const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);
  float gain = actuator->m_volume;

  PyObject *result = PyFloat_FromDouble(gain);

  return result;
}

PyObject *SCA_SoundActuator::pyattr_get_pitch(EXP_PyObjectPlus *self,
                                              const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);
  float pitch = actuator->m_pitch;

  PyObject *result = PyFloat_FromDouble(pitch);

  return result;
}

PyObject *SCA_SoundActuator::pyattr_get_sound(EXP_PyObjectPlus *self,
                                              const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef WITH_AUDASPACE
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);

  if (actuator->m_sound) {
    // AUD_getPythonSound expects a pointer to a std::shared_ptr<ISound>.
    std::shared_ptr<aud::ISound> tmp = actuator->m_sound;
    return AUD_getPythonSound(&tmp);
  }
#  endif  // WITH_AUDASPACE

  Py_RETURN_NONE;
}

int SCA_SoundActuator::pyattr_set_3d_property(EXP_PyObjectPlus *self,
                                              const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                              PyObject *value)
{
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);
  const std::string &prop = attrdef->m_name;
  float prop_value = 0.0f;

  if (!PyArg_Parse(value, "f", &prop_value))
    return PY_SET_ATTR_FAIL;

  // If sound is working and 3D, set the new setting.
  if (!actuator->m_is3d)
    return PY_SET_ATTR_FAIL;

#  ifdef WITH_AUDASPACE
  std::shared_ptr<aud::I3DHandle> h3d;
  if (actuator->m_handle) {
    h3d = std::dynamic_pointer_cast<aud::I3DHandle>(actuator->m_handle);
  }
#  endif

  if (prop == "volume_maximum") {
    actuator->m_3d.max_gain = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setVolumeMaximum(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else if (prop == "volume_minimum") {
    actuator->m_3d.min_gain = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setVolumeMinimum(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else if (prop == "distance_reference") {
    actuator->m_3d.reference_distance = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setDistanceReference(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else if (prop == "distance_maximum") {
    actuator->m_3d.max_distance = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setDistanceMaximum(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else if (prop == "attenuation") {
    actuator->m_3d.rolloff_factor = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setAttenuation(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else if (prop == "cone_angle_inner") {
    actuator->m_3d.cone_inner_angle = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setConeAngleInner(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else if (prop == "cone_angle_outer") {
    actuator->m_3d.cone_outer_angle = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setConeAngleOuter(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else if (prop == "cone_volume_outer") {
    actuator->m_3d.cone_outer_gain = prop_value;
#  ifdef WITH_AUDASPACE
    if (h3d)
      h3d->setConeVolumeOuter(prop_value);
#  endif  // WITH_AUDASPACE
  }
  else {
    return PY_SET_ATTR_FAIL;
  }

  return PY_SET_ATTR_SUCCESS;
}

int SCA_SoundActuator::pyattr_set_audposition(EXP_PyObjectPlus *self,
                                              const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                              PyObject *value)
{
  float position = 1.0f;
  if (!PyArg_Parse(value, "f", &position))
    return PY_SET_ATTR_FAIL;

#  ifdef WITH_AUDASPACE
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);

  if (actuator->m_handle)
    actuator->m_handle->seek(position);
#  endif  // WITH_AUDASPACE

  return PY_SET_ATTR_SUCCESS;
}

int SCA_SoundActuator::pyattr_set_gain(EXP_PyObjectPlus *self,
                                       const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                       PyObject *value)
{
  float gain = 1.0f;
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);
  if (!PyArg_Parse(value, "f", &gain))
    return PY_SET_ATTR_FAIL;

  actuator->m_volume = gain;

#  ifdef WITH_AUDASPACE
  if (actuator->m_handle)
    actuator->m_handle->setVolume(gain);
#  endif  // WITH_AUDASPACE

  return PY_SET_ATTR_SUCCESS;
}

int SCA_SoundActuator::pyattr_set_pitch(EXP_PyObjectPlus *self,
                                        const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value)
{
  float pitch = 1.0f;
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);
  if (!PyArg_Parse(value, "f", &pitch))
    return PY_SET_ATTR_FAIL;

  actuator->m_pitch = pitch;

#  ifdef WITH_AUDASPACE
  if (actuator->m_handle)
    actuator->m_handle->setPitch(pitch);
#  endif  // WITH_AUDASPACE

  return PY_SET_ATTR_SUCCESS;
}

int SCA_SoundActuator::pyattr_set_sound(EXP_PyObjectPlus *self,
                                        const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value)
{
  // Accept Py_None to clear sound, otherwise attempt to convert Python aud.Sound.
  if (value == Py_None) {
#  ifdef WITH_AUDASPACE
    SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);
    actuator->m_sound.reset();
    if (actuator->m_prepared) {
      actuator->m_prepared.reset();
    }
#  endif
    return PY_SET_ATTR_SUCCESS;
  }

#  ifdef WITH_AUDASPACE
  // Try to get audaspace sound from Python object.
  void *sndptr = AUD_getSoundFromPython(value);
  if (!sndptr) {
    return PY_SET_ATTR_FAIL;
  }

  std::shared_ptr<aud::ISound> *ps = reinterpret_cast<std::shared_ptr<aud::ISound> *>(sndptr);
  SCA_SoundActuator *actuator = static_cast<SCA_SoundActuator *>(self);

  // Replace original sound (shared_ptr assignment handles release).
  actuator->m_sound = *ps;

  // free the temporary pointer allocated by AUD_getSoundFromPython
  delete ps;

  // Rebuild pre-buffer if enabled.
  if (actuator->m_prepared) {
    actuator->m_prepared.reset();
  }
  if (actuator->m_preload && actuator->m_sound) {
    try {
      actuator->m_prepared = std::make_shared<aud::StreamBuffer>(actuator->m_sound);
    }
    catch (aud::Exception &) {
      actuator->m_prepared.reset();
    }
  }
  return PY_SET_ATTR_SUCCESS;
#  else
  return PY_SET_ATTR_FAIL;
#  endif
}

#endif  // WITH_PYTHON
