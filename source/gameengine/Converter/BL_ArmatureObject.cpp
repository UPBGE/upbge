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

/** \file gameengine/Converter/BL_ArmatureObject.cpp
 *  \ingroup bgeconv
 */

#include "MEM_guardedalloc.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_ghash.h"
#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_object.h"
#include "BKE_library.h"
#include "BKE_global.h"
#include "BKE_constraint.h"
#include "DNA_armature_types.h"
#include "RNA_access.h"

extern "C" {
#  include "BKE_animsys.h"
#  include "BKE_main.h"
#  include "BKE_layer.h"
#  include "BKE_scene.h"
}

#include "BL_ArmatureObject.h"
#include "BL_ActionActuator.h"
#include "BL_Action.h"
#include "KX_BlenderSceneConverter.h"
#include "KX_BlenderConverter.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"

#include "RAS_DebugDraw.h"

#include "EXP_ListWrapper.h"

#include "MT_Matrix4x4.h"

#include "CM_Message.h"

/**
 * Move here pose function for game engine so that we can mix with GE objects
 * Principle is as follow:
 * Use Blender structures so that BKE_pose_where_is can be used unchanged
 * Copy the constraint so that they can be enabled/disabled/added/removed at runtime
 * Don't copy the constraints for the pose used by the Action actuator, it does not need them.
 * Scan the constraint structures so that the KX equivalent of target objects are identified and
 * stored in separate list.
 * When it is about to evaluate the pose, set the KX object position in the obmat of the corresponding
 * Blender objects and restore after the evaluation.
 */
static void game_copy_pose(bPose **dst, bPose *src, int copy_constraint)
{
	/* The game engine copies the current armature pose and then swaps
	 * the object pose pointer. this makes it possible to change poses
	 * without affecting the original blender data. */

	if (!src) {
		*dst = nullptr;
		return;
	}
	else if (*dst == src) {
		CM_Warning("game_copy_pose source and target are the same");
		*dst = nullptr;
		return;
	}

	bPose *out = (bPose *)MEM_dupallocN(src);
	out->chanhash = nullptr;
	out->agroups.first = out->agroups.last = nullptr;
	out->ikdata = nullptr;
	out->ikparam = MEM_dupallocN(src->ikparam);
	out->flag |= POSE_GAME_ENGINE;
	BLI_duplicatelist(&out->chanbase, &src->chanbase);

	/* remap pointers */
	GHash *ghash = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "game_copy_pose gh");

	bPoseChannel *pchan = (bPoseChannel *)src->chanbase.first;
	bPoseChannel *outpchan = (bPoseChannel *)out->chanbase.first;
	for (; pchan; pchan = pchan->next, outpchan = outpchan->next) {
		BLI_ghash_insert(ghash, pchan, outpchan);
	}

	for (pchan = (bPoseChannel *)out->chanbase.first; pchan; pchan = pchan->next) {
		pchan->parent = (bPoseChannel *)BLI_ghash_lookup(ghash, pchan->parent);
		pchan->child = (bPoseChannel *)BLI_ghash_lookup(ghash, pchan->child);

		if (copy_constraint) {
			ListBase listb;
			// copy all constraint for backward compatibility
			// BKE_constraints_copy nullptrs listb, no need to make extern for this operation.
			BKE_constraints_copy(&listb, &pchan->constraints, false);
			pchan->constraints = listb;
		}
		else {
			BLI_listbase_clear(&pchan->constraints);
		}

		if (pchan->custom) {
			id_us_plus(&pchan->custom->id);
		}

		// fails to link, props are not used in the BGE yet.
#if 0
		if (pchan->prop) {
			pchan->prop = IDP_CopyProperty(pchan->prop);
		}
#endif
		pchan->prop = nullptr;
	}

	BLI_ghash_free(ghash, nullptr, nullptr);
	// set acceleration structure for channel lookup
	BKE_pose_channels_hash_make(out);
	*dst = out;
}

// Only allowed for Poses with identical channels.
static void game_blend_poses(bPose *dst, bPose *src, float srcweight, short mode)
{
	float dstweight;

	if (mode == BL_Action::ACT_BLEND_BLEND) {
		dstweight = 1.0f - srcweight;
	}
	else if (mode == BL_Action::ACT_BLEND_ADD) {
		dstweight = 1.0f;
	}
	else {
		dstweight = 1.0f;
	}

	bPoseChannel *schan = (bPoseChannel *)src->chanbase.first;
	for (bPoseChannel *dchan = (bPoseChannel *)dst->chanbase.first; dchan; dchan = (bPoseChannel *)dchan->next, schan = (bPoseChannel *)schan->next) {
		// always blend on all channels since we don't know which one has been set
		/* quat interpolation done separate */
		if (schan->rotmode == ROT_MODE_QUAT) {
			float dquat[4], squat[4];

			copy_qt_qt(dquat, dchan->quat);
			copy_qt_qt(squat, schan->quat);
			// Normalize quaternions so that interpolation/multiplication result is correct.
			normalize_qt(dquat);
			normalize_qt(squat);

			if (mode == BL_Action::ACT_BLEND_BLEND) {
				interp_qt_qtqt(dchan->quat, dquat, squat, srcweight);
			}
			else {
				mul_fac_qt_fl(squat, srcweight);
				mul_qt_qtqt(dchan->quat, dquat, squat);
			}

			normalize_qt(dchan->quat);
		}

		for (unsigned short i = 0; i < 3; i++) {
			/* blending for loc and scale are pretty self-explanatory... */
			dchan->loc[i] = (dchan->loc[i] * dstweight) + (schan->loc[i] * srcweight);
			dchan->size[i] = 1.0f + ((dchan->size[i] - 1.0f) * dstweight) + ((schan->size[i] - 1.0f) * srcweight);

			/* euler-rotation interpolation done here instead... */
			// FIXME: are these results decent?
			if (schan->rotmode) {
				dchan->eul[i] = (dchan->eul[i] * dstweight) + (schan->eul[i] * srcweight);
			}
		}
		for (bConstraint *dcon = (bConstraint *)dchan->constraints.first, *scon = (bConstraint *)schan->constraints.first;
		     dcon && scon;
		     dcon = dcon->next, scon = scon->next)
		{
			/* no 'add' option for constraint blending */
			dcon->enforce = dcon->enforce * (1.0f - srcweight) + scon->enforce * srcweight;
		}
	}

	/* this pose is now in src time */
	dst->ctime = src->ctime;
}

BL_ArmatureObject::BL_ArmatureObject(void *sgReplicationInfo,
                                     SG_Callbacks callbacks,
                                     Object *armature,
                                     Scene *scene,
                                     int vert_deform_type)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_scene(scene),
	m_lastframe(0.0),
	m_timestep(0.040),
	m_vert_deform_type(vert_deform_type),
	m_drawDebug(false),
	m_lastapplyframe(0.0)
{
	m_controlledConstraints = new CListValue<BL_ArmatureConstraint>();
	m_poseChannels = new CListValue<BL_ArmatureChannel>();

	// Keep a copy of the original armature so we can fix drivers later
	m_origObjArma = armature;
	m_objArma = BKE_object_copy(G.main, armature);
	m_objArma->data = BKE_armature_copy(G.main, (bArmature *)armature->data);
	// During object replication ob->data is increase, we decrease it now because we get a copy.
	id_us_min(&((bArmature *)m_origObjArma->data)->id);
	m_pose = m_objArma->pose;
	// need this to get iTaSC working ok in the BGE
	m_pose->flag |= POSE_GAME_ENGINE;
	memcpy(m_obmat, m_objArma->obmat, sizeof(m_obmat));
}

BL_ArmatureObject::~BL_ArmatureObject()
{
	m_poseChannels->Release();
	m_controlledConstraints->Release();

	if (m_objArma) {
		BKE_libblock_free(G.main, m_objArma->data);
		/* avoid BKE_libblock_free(G.main, m_objArma)
		   try to access m_objArma->data */
		m_objArma->data = nullptr;
		BKE_libblock_free(G.main, m_objArma);
	}
}

void BL_ArmatureObject::LoadConstraints(KX_BlenderSceneConverter& converter)
{
	// first delete any existing constraint (should not have any)
	m_controlledConstraints->ReleaseAndRemoveAll();

	// list all the constraint and convert them to BL_ArmatureConstraint
	// get the persistent pose structure

	// and locate the constraint
	for (bPoseChannel *pchan = (bPoseChannel *)m_pose->chanbase.first; pchan; pchan = pchan->next) {
		for (bConstraint *pcon = (bConstraint *)pchan->constraints.first; pcon; pcon = pcon->next) {
			if (pcon->flag & CONSTRAINT_DISABLE) {
				continue;
			}
			// which constraint should we support?
			switch (pcon->type)
			{
				case CONSTRAINT_TYPE_TRACKTO:
				case CONSTRAINT_TYPE_DAMPTRACK:
				case CONSTRAINT_TYPE_KINEMATIC:
				case CONSTRAINT_TYPE_ROTLIKE:
				case CONSTRAINT_TYPE_LOCLIKE:
				case CONSTRAINT_TYPE_MINMAX:
				case CONSTRAINT_TYPE_SIZELIKE:
				case CONSTRAINT_TYPE_LOCKTRACK:
				case CONSTRAINT_TYPE_STRETCHTO:
				case CONSTRAINT_TYPE_CLAMPTO:
				case CONSTRAINT_TYPE_TRANSFORM:
				case CONSTRAINT_TYPE_DISTLIMIT:
				case CONSTRAINT_TYPE_TRANSLIKE:
				{
					const bConstraintTypeInfo *cti = BKE_constraint_typeinfo_get(pcon);
					KX_GameObject *gametarget = nullptr;
					KX_GameObject *gamesubtarget = nullptr;
					if (cti && cti->get_constraint_targets) {
						ListBase listb = { nullptr, nullptr };
						cti->get_constraint_targets(pcon, &listb);
						if (listb.first) {
							bConstraintTarget *target = (bConstraintTarget *)listb.first;
							if (target->tar && target->tar != m_objArma) {
								// only remember external objects, self target is handled automatically
								gametarget = converter.FindGameObject(target->tar);
							}
							if (target->next != nullptr) {
								// secondary target
								target = target->next;
								if (target->tar && target->tar != m_objArma) {
									// only track external object
									gamesubtarget = converter.FindGameObject(target->tar);
								}
							}
						}
						if (cti->flush_constraint_targets) {
							cti->flush_constraint_targets(pcon, &listb, 1);
						}
					}
					BL_ArmatureConstraint* constraint = new BL_ArmatureConstraint(this, pchan, pcon, gametarget, gamesubtarget);
					m_controlledConstraints->Add(constraint);
				}
			}
		}
	}

	// If we have constraints, make sure we get treated as an "animated" object
	if (m_controlledConstraints->GetCount() > 0) {
		GetActionManager();
	}
}

size_t BL_ArmatureObject::GetConstraintNumber() const
{
	return m_controlledConstraints->GetCount();
}

BL_ArmatureConstraint *BL_ArmatureObject::GetConstraint(const std::string& posechannel, const std::string& constraintname)
{
	return m_controlledConstraints->FindIf(
		[&posechannel, &constraintname](BL_ArmatureConstraint *constraint) { return constraint->Match(posechannel, constraintname); });
}

BL_ArmatureConstraint *BL_ArmatureObject::GetConstraint(const std::string& posechannelconstraint)
{
	return static_cast<BL_ArmatureConstraint *>(m_controlledConstraints->FindValue(posechannelconstraint));
}

BL_ArmatureConstraint *BL_ArmatureObject::GetConstraint(int index)
{
	return static_cast<BL_ArmatureConstraint *>(m_controlledConstraints->GetValue(index));
}

/* this function is called to populate the m_poseChannels list */
void BL_ArmatureObject::LoadChannels()
{
	if (m_poseChannels->GetCount() == 0) {
		for (bPoseChannel *pchan = (bPoseChannel *)m_pose->chanbase.first; pchan; pchan = (bPoseChannel *)pchan->next) {
			BL_ArmatureChannel *proxy = new BL_ArmatureChannel(this, pchan);
			m_poseChannels->Add(proxy);
		}
	}
}

size_t BL_ArmatureObject::GetChannelNumber() const
{
	return m_poseChannels->GetCount();
}

BL_ArmatureChannel *BL_ArmatureObject::GetChannel(bPoseChannel *pchan)
{
	LoadChannels();
	return m_poseChannels->FindIf([&pchan](BL_ArmatureChannel *channel) { return channel->m_posechannel == pchan; });
}

BL_ArmatureChannel *BL_ArmatureObject::GetChannel(const std::string& str)
{
	LoadChannels();
	return static_cast<BL_ArmatureChannel *>(m_poseChannels->FindValue(str));
}

BL_ArmatureChannel *BL_ArmatureObject::GetChannel(int index)
{
	LoadChannels();
	if (index < 0 || index >= m_poseChannels->GetCount()) {
		return nullptr;
	}
	return static_cast<BL_ArmatureChannel *>(m_poseChannels->GetValue(index));
}

CValue *BL_ArmatureObject::GetReplica()
{
	BL_ArmatureObject *replica = new BL_ArmatureObject(*this);
	replica->ProcessReplica();
	return replica;
}

void BL_ArmatureObject::ProcessReplica()
{
	KX_GameObject::ProcessReplica();

	// Replicate each constraints.
	m_controlledConstraints = static_cast<CListValue<BL_ArmatureConstraint> *>(m_controlledConstraints->GetReplica());
	// Share pose channels.
	m_poseChannels->AddRef();

	bArmature *tmp = (bArmature *)m_objArma->data;
	m_objArma = BKE_object_copy(G.main, m_objArma);
	m_objArma->data = BKE_armature_copy(G.main, tmp);
	m_pose = m_objArma->pose;
}

int BL_ArmatureObject::GetGameObjectType() const
{
	return OBJ_ARMATURE;
}

void BL_ArmatureObject::ReParentLogic()
{
	for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
		constraint->ReParent(this);
	}
	KX_GameObject::ReParentLogic();
}

void BL_ArmatureObject::Relink(std::map<SCA_IObject *, SCA_IObject *>& obj_map)
{
	for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
		constraint->Relink(obj_map);
	}
	KX_GameObject::Relink(obj_map);
}

bool BL_ArmatureObject::UnlinkObject(SCA_IObject *clientobj)
{
	// clientobj is being deleted, make sure we don't hold any reference to it
	bool res = false;
	for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
		res |= constraint->UnlinkObject(clientobj);
	}
	return res;
}

void BL_ArmatureObject::ApplyPose()
{
	m_armpose = m_objArma->pose;
	m_objArma->pose = m_pose;
	// in the GE, we use ctime to store the timestep
	m_pose->ctime = (float)m_timestep;
	//m_scene->r.cfra++;
	if (m_lastapplyframe != m_lastframe) {
		// update the constraint if any, first put them all off so that only the active ones will be updated
		for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
			constraint->UpdateTarget();
		}
		// update ourself
		UpdateBlenderObjectMatrix(m_objArma);
		ViewLayer *view_layer = BKE_view_layer_default_view(m_scene);
		Depsgraph *depsgraph = BKE_scene_get_depsgraph(m_scene, view_layer, false);
		BKE_pose_where_is(depsgraph, m_scene, m_objArma);
		// restore ourself
		memcpy(m_objArma->obmat, m_obmat, sizeof(m_obmat));
		m_lastapplyframe = m_lastframe;
	}
}

void BL_ArmatureObject::RestorePose()
{
	m_objArma->pose = m_armpose;
	m_armpose = nullptr;
}

void BL_ArmatureObject::SetPose(bPose *pose)
{
	extract_pose_from_pose(m_pose, pose);
	m_lastapplyframe = -1.0;
}

void BL_ArmatureObject::SetPoseByAction(bAction *action, float localtime)
{
	Object *arm = GetArmatureObject();

	PointerRNA ptrrna;
	RNA_id_pointer_create(&arm->id, &ptrrna);

	Scene *scene = KX_GetActiveScene()->GetBlenderScene();
	ViewLayer *view_layer = BKE_view_layer_default_view(scene);
	Depsgraph *depsgraph = BKE_scene_get_depsgraph(scene, view_layer, false);

	animsys_evaluate_action(depsgraph, &ptrrna, action, localtime);
}

void BL_ArmatureObject::BlendInPose(bPose *blend_pose, float weight, short mode)
{
	game_blend_poses(m_pose, blend_pose, weight, mode);
}

bool BL_ArmatureObject::UpdateTimestep(double curtime)
{
	if (curtime != m_lastframe) {
		// compute the timestep for the underlying IK algorithm
		m_timestep = curtime - m_lastframe;
		m_lastframe = curtime;
	}

	return false;
}

bArmature *BL_ArmatureObject::GetArmature()
{
	return (bArmature *)m_objArma->data;
}
const bArmature *BL_ArmatureObject::GetArmature() const
{
	return (bArmature *)m_objArma->data;
}
const Scene *BL_ArmatureObject::GetScene() const
{
	return m_scene;
}

Object *BL_ArmatureObject::GetArmatureObject()
{
	return m_objArma;
}
Object *BL_ArmatureObject::GetOrigArmatureObject()
{
	return m_origObjArma;
}

int BL_ArmatureObject::GetVertDeformType()
{
	return m_vert_deform_type;
}

void BL_ArmatureObject::GetPose(bPose **pose)
{
	/* If the caller supplies a null pose, create a new one. */
	/* Otherwise, copy the armature's pose channels into the caller-supplied pose */

	if (!*pose) {
		/* probably not to good of an idea to
		 * duplicate everything, but it clears up
		 * a crash and memory leakage when
		 * &BL_ActionActuator::m_pose is freed
		 */
		game_copy_pose(pose, m_pose, 0);
	}
	else {
		if (*pose == m_pose) {
			// no need to copy if the pointers are the same
			return;
		}

		extract_pose_from_pose(*pose, m_pose);
	}
}

bPose *BL_ArmatureObject::GetOrigPose()
{
	return m_pose;
}

double BL_ArmatureObject::GetLastFrame()
{
	return m_lastframe;
}

bool BL_ArmatureObject::GetBoneMatrix(Bone *bone, MT_Matrix4x4& matrix)
{
	ApplyPose();
	bPoseChannel *pchan = BKE_pose_channel_find_name(m_objArma->pose, bone->name);
	if (pchan) {
		matrix.setValue(&pchan->pose_mat[0][0]);
	}
	RestorePose();

	return (pchan != nullptr);
}

bool BL_ArmatureObject::GetDrawDebug() const
{
	return m_drawDebug;
}

void BL_ArmatureObject::DrawDebug(RAS_DebugDraw& debugDraw)
{
	const MT_Vector3& scale = NodeGetWorldScaling();
	const MT_Matrix3x3& rot = NodeGetWorldOrientation();
	const MT_Vector3& pos = NodeGetWorldPosition();

	for (bPoseChannel *pchan = (bPoseChannel *)m_pose->chanbase.first; pchan; pchan = pchan->next) {
		MT_Vector3 head = rot * (MT_Vector3(pchan->pose_head) * scale) + pos;
		MT_Vector3 tail = rot * (MT_Vector3(pchan->pose_tail) * scale) + pos;
		debugDraw.DrawLine(tail, head, MT_Vector4(1.0f, 0.0f, 0.0f, 1.0f));
	}
	m_drawDebug = false;
}

float BL_ArmatureObject::GetBoneLength(Bone *bone) const
{
	return (float)(MT_Vector3(bone->head) - MT_Vector3(bone->tail)).length();
}

#ifdef WITH_PYTHON

// PYTHON

PyTypeObject BL_ArmatureObject::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"BL_ArmatureObject",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&KX_GameObject::Sequence,
	&KX_GameObject::Mapping,
	0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&KX_GameObject::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef BL_ArmatureObject::Methods[] = {
	KX_PYMETHODTABLE_NOARGS(BL_ArmatureObject, update),
	KX_PYMETHODTABLE_NOARGS(BL_ArmatureObject, draw),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef BL_ArmatureObject::Attributes[] = {

	KX_PYATTRIBUTE_RO_FUNCTION("constraints",       BL_ArmatureObject, pyattr_get_constraints),
	KX_PYATTRIBUTE_RO_FUNCTION("channels",      BL_ArmatureObject, pyattr_get_channels),
	KX_PYATTRIBUTE_NULL //Sentinel
};

PyObject *BL_ArmatureObject::pyattr_get_constraints(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_ArmatureObject *self = static_cast<BL_ArmatureObject *>(self_v);
	return self->m_controlledConstraints->GetProxy();
}

PyObject *BL_ArmatureObject::pyattr_get_channels(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	BL_ArmatureObject *self = static_cast<BL_ArmatureObject *>(self_v);
	self->LoadChannels(); // make sure we have the channels
	return self->m_poseChannels->GetProxy();
}

KX_PYMETHODDEF_DOC_NOARGS(BL_ArmatureObject, update,
                          "update()\n"
                          "Make sure that the armature will be updated on next graphic frame.\n"
                          "This is automatically done if a KX_ArmatureActuator with mode run is active\n"
                          "or if an action is playing. This function is useful in other cases.\n")
{
	UpdateTimestep(KX_GetActiveEngine()->GetFrameTime());
	Py_RETURN_NONE;
}

KX_PYMETHODDEF_DOC_NOARGS(BL_ArmatureObject, draw,
                          "Draw Debug Armature")
{
	/* Armature bones are updated later, so we only set to true a flag
	 * to request a debug draw later in ApplyPose after updating bones. */
	m_drawDebug = true;
	Py_RETURN_NONE;
}

#endif // WITH_PYTHON
