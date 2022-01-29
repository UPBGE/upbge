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

/** \file KX_LodManager.h
 *  \ingroup ketsji
 */

#pragma once

#include <vector>

#include "EXP_Value.h"

class KX_Scene;
class RAS_Rasterizer;
class BL_SceneConverter;
class KX_LodLevel;
struct Object;

class KX_LodManager : public EXP_Value {
  Py_Header

      private :
      /** This class helps to compare the object distance to camera with the list of lod levels.
       * It represent the gap between two levels, when you compare it with a distance it compare
       * with the a N level distance and a N+1 level distance including hysteresis.
       */
      class LodLevelIterator {
   private:
    const std::vector<KX_LodLevel *> &m_levels;
    short m_index;
    KX_Scene *m_scene;
    float GetHysteresis(unsigned short level) const;

   public:
    LodLevelIterator(const std::vector<KX_LodLevel *> &levels,
                     unsigned short index,
                     KX_Scene *scene);

    int operator++();
    int operator--();
    short operator*() const;
    /// Compare next level distance more hysteresis with current distance.
    bool operator<=(float distance2) const;
    /// Compare the current lod level distance less hysteresis with current distance.
    bool operator>(float distance2) const;
  };

  std::vector<KX_LodLevel *> m_levels;

  /** Get the hysteresis from the level or the scene.
   * \param scene Scene used to get default hysteresis.
   * \param level Level index used to get hysteresis.
   */
  float GetHysteresis(KX_Scene *scene, unsigned short level);

  int m_refcount;

  /// Factor applied to the distance from the camera to the object.
  float m_distanceFactor;

 public:
  KX_LodManager(Object *ob,
                KX_Scene *scene,
                RAS_Rasterizer *rasty,
                BL_SceneConverter *converter,
                bool libloading,
                bool converting_during_runtime);
  KX_LodManager(class RAS_MeshObject *meshObj, struct Object *ob);  // just to use for ReplaceMesh
  virtual ~KX_LodManager();

  virtual std::string GetName();

  /// Return number of lod levels.
  unsigned int GetLevelCount() const;

  /** Get lod level by index.
   * \param index The lod level index.
   */
  KX_LodLevel *GetLevel(unsigned int index) const;

  /** Get lod level cooresponding to distance and previous level.
   * \param scene Scene used to get default hysteresis.
   * \param previouslod Previous lod computed by this function before.
   *   Use -1 to disable the hysteresis when the lod manager has changed.
   * \param distance2 Squared distance object to the camera.
   */
  KX_LodLevel *GetLevel(KX_Scene *scene, short previouslod, float distance);

#ifdef WITH_PYTHON

  static PyObject *pyattr_get_levels(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

#endif  // WITH_PYTHON
};

#ifdef WITH_PYTHON

/// Utility python conversion function.
bool ConvertPythonToLodManager(PyObject *value,
                               KX_LodManager **object,
                               bool py_none_ok,
                               const char *error_prefix);

#endif  // WITH_PYTHON
