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
 * Contributor(s): Ulysse Martin, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_LodManager.cpp
 *  \ingroup ketsji
 */

#include "KX_LodManager.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "DNA_object_types.h"

#include "BL_DataConversion.h"
#include "EXP_ListWrapper.h"
#include "KX_LodLevel.h"
#include "KX_Scene.h"

KX_LodManager::LodLevelIterator::LodLevelIterator(const std::vector<KX_LodLevel *> &levels,
                                                  unsigned short index,
                                                  KX_Scene *scene)
    : m_levels(levels), m_index(index), m_scene(scene)
{
}

inline float KX_LodManager::LodLevelIterator::GetHysteresis(unsigned short level) const
{
  if (level < 1 || !m_scene->IsActivedLodHysteresis()) {
    return 0.0f;
  }

  KX_LodLevel *lod = m_levels[level];
  KX_LodLevel *prelod = m_levels[level - 1];

  float hysteresis = 0.0f;
  // if exists, LoD level hysteresis will override scene hysteresis
  if (lod->GetFlag() & KX_LodLevel::USE_HYSTERESIS) {
    hysteresis = lod->GetHysteresis() / 100.0f;
  }
  else {
    hysteresis = m_scene->GetLodHysteresisValue() / 100.0f;
  }

  return MT_abs(prelod->GetDistance() - lod->GetDistance()) * hysteresis;
}

inline int KX_LodManager::LodLevelIterator::operator++()
{
  return m_index++;
}

inline int KX_LodManager::LodLevelIterator::operator--()
{
  return m_index--;
}

inline short KX_LodManager::LodLevelIterator::operator*() const
{
  return m_index;
}

inline bool KX_LodManager::LodLevelIterator::operator<=(float distance2) const
{
  // The last level doesn't have a next level, then the maximum distance is infinite and should
  // always return false.
  if (m_index == (m_levels.size() - 1)) {
    return false;
  }

  return square_f(m_levels[m_index + 1]->GetDistance() + GetHysteresis(m_index + 1)) <= distance2;
}

inline bool KX_LodManager::LodLevelIterator::operator>(float distance2) const
{
  return square_f(m_levels[m_index]->GetDistance() - GetHysteresis(m_index)) > distance2;
}

KX_LodManager::KX_LodManager(Object *ob,
                             KX_Scene *scene,
                             RAS_Rasterizer *rasty,
                             BL_SceneConverter *converter,
                             bool libloading,
                             bool converting_during_runtime)
    : m_refcount(1), m_distanceFactor(ob->lodfactor)
{
  if (BLI_listbase_count_at_most(&ob->lodlevels, 2) > 1) {
    Mesh *lodmesh = (Mesh *)ob->data;
    Object *lodmatob = ob;
    unsigned short level = 0;

    for (LodLevel *lod = (LodLevel *)ob->lodlevels.first; lod; lod = lod->next) {
      if (!lod->source || lod->source->type != OB_MESH) {
        continue;
      }
      unsigned short flag = 0;
      if (lod->flags & OB_LOD_USE_HYST) {
        flag |= KX_LodLevel::USE_HYSTERESIS;
      }

      if (lod->flags & OB_LOD_USE_MESH) {
        lodmesh = (Mesh *)lod->source->data;
        flag |= KX_LodLevel::USE_MESH;
      }

      if (lod->flags & OB_LOD_USE_MAT) {
        lodmatob = lod->source;
        flag |= KX_LodLevel::USE_MATERIAL;
      }
      KX_LodLevel *lodLevel = new KX_LodLevel(
          lod->distance,
          lod->obhysteresis,
          level++,
          BL_ConvertMesh(
              lodmesh, lodmatob, scene, rasty, converter, libloading, converting_during_runtime),
          lod->source,
          flag);

      m_levels.push_back(lodLevel);
    }
  }
}

KX_LodManager::KX_LodManager(RAS_MeshObject *meshObj, Object *lodsource)
    : m_refcount(1), m_distanceFactor(1.0f)
{
  KX_LodLevel *lodLevel = new KX_LodLevel(
      0.0f, 0.0f, 0, meshObj, lodsource, OB_LOD_USE_MESH | OB_LOD_USE_MAT);
  m_levels.push_back(lodLevel);
}

KX_LodManager::~KX_LodManager()
{
  for (KX_LodLevel *lodLevel : m_levels) {
    delete lodLevel;
  }
}

std::string KX_LodManager::GetName()
{
  return "KX_LodManager";
}

unsigned int KX_LodManager::GetLevelCount() const
{
  return m_levels.size();
}

KX_LodLevel *KX_LodManager::GetLevel(unsigned int index) const
{
  return m_levels[index];
}

KX_LodLevel *KX_LodManager::GetLevel(KX_Scene *scene, short previouslod, float distance2)
{
  if (m_levels.size() == 1) {
    return m_levels[0];  // in the case of ReplaceMesh we have only 1 level with 1 RAS_MeshObject
  }
  distance2 *= (m_distanceFactor * m_distanceFactor);

  LodLevelIterator it(m_levels, previouslod, scene);

  while (true) {
    if (it <= distance2) {
      ++it;
    }
    else if (it > distance2) {
      --it;
    }
    else {
      break;
    }
  }

  const unsigned short level = *it;
  return (level == previouslod) ? nullptr : m_levels[level];
}

#ifdef WITH_PYTHON

PyTypeObject KX_LodManager::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_LodManager",
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
                                    &EXP_Value::Type,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    py_base_new};

PyMethodDef KX_LodManager::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_LodManager::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("levels", KX_LodManager, pyattr_get_levels),
    EXP_PYATTRIBUTE_FLOAT_RW("distanceFactor", 0.0f, FLT_MAX, KX_LodManager, m_distanceFactor),
    EXP_PYATTRIBUTE_NULL};

static int kx_lod_manager_get_levels_size_cb(void *self_v)
{
  return ((KX_LodManager *)self_v)->GetLevelCount();
}

static PyObject *kx_lod_manager_get_levels_item_cb(void *self_v, int index)
{
  return ((KX_LodManager *)self_v)->GetLevel(index)->GetProxy();
}

PyObject *KX_LodManager::pyattr_get_levels(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_LodManager *)self_v)->GetProxy(),
                              nullptr,
                              kx_lod_manager_get_levels_size_cb,
                              kx_lod_manager_get_levels_item_cb,
                              nullptr,
                              nullptr))
      ->NewProxy(true);
}

bool ConvertPythonToLodManager(PyObject *value,
                               KX_LodManager **object,
                               bool py_none_ok,
                               const char *error_prefix)
{
  if (value == nullptr) {
    PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
    *object = nullptr;
    return false;
  }

  if (value == Py_None) {
    *object = nullptr;

    if (py_none_ok) {
      return true;
    }
    else {
      PyErr_Format(PyExc_TypeError, "%s, expected KX_LodManager, None is invalid", error_prefix);
      return false;
    }
  }

  if (PyObject_TypeCheck(value, &KX_LodManager::Type)) {
    *object = static_cast<KX_LodManager *> EXP_PROXY_REF(value);

    /* sets the error */
    if (*object == nullptr) {
      PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
      return false;
    }

    return true;
  }

  *object = nullptr;

  if (py_none_ok) {
    PyErr_Format(PyExc_TypeError, "%s, expect a KX_LodManager or None", error_prefix);
  }
  else {
    PyErr_Format(PyExc_TypeError, "%s, expect a KX_LodManager", error_prefix);
  }

  return false;
}

#endif  // WITH_PYTHON
