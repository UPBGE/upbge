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

/** \file BL_ArmatureObject.h
 *  \ingroup bgeconv
 */

#pragma once

#include "BL_ArmatureChannel.h"
#include "BL_ArmatureConstraint.h"
#include "KX_GameObject.h"

struct AnimationEvalContext;
struct Bone;
struct bPose;
struct Object;
class MT_Matrix4x4;
class BL_SceneConverter;
class RAS_DebugDraw;

class BL_ArmatureObject : public KX_GameObject {
  Py_Header

      protected :
      /// List element: BL_ArmatureConstraint.
      EXP_ListValue<BL_ArmatureConstraint> *m_controlledConstraints;
  /// List element: BL_ArmatureChannel.
  EXP_ListValue<BL_ArmatureChannel> *m_poseChannels;
  Object *m_objArma;
  Object *m_origObjArma;

  double m_lastframe;
  size_t m_constraintNumber;
  size_t m_channelNumber;
  /// Store the original armature object matrix.
  float m_object_to_world[4][4];
  /// Set to true to allow draw debug info for one frame, reset in DrawDebugArmature.
  bool m_drawDebug;

  double m_lastapplyframe;

 public:
  BL_ArmatureObject();
  virtual ~BL_ArmatureObject();

  virtual KX_PythonProxy *NewInstance();
  virtual void ProcessReplica();
  virtual int GetGameObjectType() const;
  virtual void ReParentLogic();
  virtual void Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map);
  virtual bool UnlinkObject(SCA_IObject *clientobj);

  double GetLastFrame();

  void GetPose(bPose **pose) const;
  /// Never edit this, only for accessing names.
  bPose *GetPose() const;
  void ApplyPose();
  void SetPoseByAction(bAction *action, AnimationEvalContext *evalCtx);
  void BlendInPose(bPose *blend_pose, float weight, short mode);

  bool UpdateTimestep(double curtime);

  Object *GetArmatureObject();
  Object *GetOrigArmatureObject();
  bool GetDrawDebug() const;
  void DrawDebug(RAS_DebugDraw &debugDraw);

  // for constraint python API
  void LoadConstraints(BL_SceneConverter *converter);
  size_t GetConstraintNumber() const;
  BL_ArmatureConstraint *GetConstraint(const std::string &posechannel,
                                       const std::string &constraint);
  BL_ArmatureConstraint *GetConstraint(const std::string &posechannelconstraint);
  BL_ArmatureConstraint *GetConstraint(int index);

  // for pose channel python API
  void LoadChannels();
  size_t GetChannelNumber() const;
  BL_ArmatureChannel *GetChannel(bPoseChannel *channel);
  BL_ArmatureChannel *GetChannel(const std::string &channel);
  BL_ArmatureChannel *GetChannel(int index);

  /// Retrieve the pose matrix for the specified bone.
  /// Returns true on success.
  bool GetBoneMatrix(Bone *bone, MT_Matrix4x4 &matrix);

  /// Returns the bone length.  The end of the bone is in the local y direction.
  float GetBoneLength(Bone *bone) const;

  virtual void SetBlenderObject(Object *obj);

#ifdef WITH_PYTHON

  // PYTHON
  static PyObject *game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

  static PyObject *pyattr_get_constraints(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_channels(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  EXP_PYMETHOD_DOC_NOARGS(BL_ArmatureObject, update);
  EXP_PYMETHOD_DOC_NOARGS(BL_ArmatureObject, draw);

#endif /* WITH_PYTHON */
};
