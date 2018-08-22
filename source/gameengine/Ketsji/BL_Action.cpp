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
 * Contributor(s): Mitchell Stokes.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BL_Action.cpp
 *  \ingroup ketsji
 */

#include "CM_Message.h"

#include "BL_Action.h"
#include "BL_ArmatureObject.h"
#include "BL_ShapeDeformer.h"
#include "BL_IpoConvert.h"
#include "BL_ActionData.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_Mesh.h"
#include "BL_Material.h"

#include "RAS_MaterialBucket.h"

#include "SG_Controller.h"

// These three are for getting the action from the logic manager
#include "KX_Scene.h"
#include "BL_Converter.h"
#include "SCA_LogicManager.h"

extern "C" {
#include "BKE_animsys.h"
#include "BKE_action.h"
#include "RNA_access.h"
#include "RNA_define.h"

// Needed for material IPOs
#include "BKE_material.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
}

#include "MEM_guardedalloc.h"
#include "BKE_library.h"
#include "BKE_global.h"

BL_Action::BL_Action(KX_GameObject *gameobj)
	:m_actionData(nullptr),
	m_tmpaction(nullptr),
	m_blendpose(nullptr),
	m_blendinpose(nullptr),
	m_obj(gameobj),
	m_startframe(0.f),
	m_endframe(0.f),
	m_localframe(0.f),
	m_blendin(0.f),
	m_blendframe(0.f),
	m_blendstart(0.f),
	m_speed(0.f),
	m_priority(0),
	m_playmode(ACT_MODE_PLAY),
	m_blendmode(ACT_BLEND_BLEND),
	m_ipo_flags(0),
	m_done(true),
	m_appliedToObject(true),
	m_requestIpo(false),
	m_calc_localtime(true),
	m_prevUpdate(-1.0f)
{
}

BL_Action::~BL_Action()
{
	if (m_blendpose) {
		BKE_pose_free(m_blendpose);
	}
	if (m_blendinpose) {
		BKE_pose_free(m_blendinpose);
	}
	ClearControllerList();

	if (m_tmpaction) {
		BKE_libblock_free(G.main, m_tmpaction);
		m_tmpaction = nullptr;
	}
}

void BL_Action::AddController(SG_Controller *cont)
{
	if (!cont || cont->Empty()) {
		return;
	}

	m_controllers.push_back(cont);
}

void BL_Action::ClearControllerList()
{
	// Clear out the controller list
	for (SG_Controller *cont : m_controllers) {
		delete cont;
	}

	m_controllers.clear();
}

bool BL_Action::Play(const std::string& name,
                     float start,
                     float end,
                     short priority,
                     float blendin,
                     short play_mode,
                     float layer_weight,
                     short ipo_flags,
                     float playback_speed,
                     short blend_mode)
{

	// Only start playing a new action if we're done, or if
	// the new action has a higher priority
	if (!IsDone() && priority > m_priority) {
		return false;
	}
	m_priority = priority;
	BL_ActionData *prev_action = m_actionData;

	KX_Scene *kxscene = m_obj->GetScene();

	// First try to load the action
	m_actionData = static_cast<BL_ActionData *>(kxscene->GetLogicManager()->GetActionByName(name));
	if (!m_actionData) {
		CM_Error("failed to load action: " << name);
		m_done = true;
		return false;
	}

	// If we have the same settings, don't play again
	// This is to resolve potential issues with pulses on sensors such as the ones
	// reported in bug #29412. The fix is here so it works for both logic bricks and Python.
	// However, this may eventually lead to issues where a user wants to override an already
	// playing action with the same action and settings. If this becomes an issue,
	// then this fix may have to be re-evaluated.
	if (!IsDone() && m_actionData == prev_action && m_startframe == start && m_endframe == end
	    && m_priority == priority && m_speed == playback_speed) {
		return false;
	}

	// Keep a copy of the action for threading purposes
	if (m_tmpaction) {
		BKE_libblock_free(G.main, m_tmpaction);
		m_tmpaction = nullptr;
	}

	m_tmpaction = BKE_action_copy(G.main, m_actionData->GetAction());

	// First get rid of any old controllers
	ClearControllerList();

	// Create an SG_Controller
	AddController(BL_CreateIPO(m_actionData, m_obj, kxscene));
	// World
	AddController(BL_CreateWorldIPO(m_actionData, kxscene->GetBlenderScene()->world, kxscene));
	// Try obcolor
	AddController(BL_CreateObColorIPO(m_actionData, m_obj, kxscene));

	// Now try materials
	for (KX_Mesh *mesh : m_obj->GetMeshList()) {
		for (RAS_MeshMaterial *meshmat : mesh->GetMeshMaterialList()) {
			BL_Material *mat = static_cast<BL_Material *>(meshmat->GetBucket()->GetMaterial());
			AddController(BL_CreateMaterialIpo(m_actionData, mat, m_obj, kxscene));
		}
	}

	// Extra controllers
	if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_LIGHT) {
		AddController(BL_CreateLampIPO(m_actionData, m_obj, kxscene));
	}
	else if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_CAMERA) {
		AddController(BL_CreateCameraIPO(m_actionData, m_obj, kxscene));
	}

	m_ipo_flags = ipo_flags;
	InitIPO();

	// Setup blendin shapes/poses
	if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
		BL_ArmatureObject *obj = (BL_ArmatureObject *)m_obj;
		obj->GetPose(&m_blendinpose);
	}
	else {
		BL_ShapeDeformer *shape_deformer = dynamic_cast<BL_ShapeDeformer *>(m_obj->GetDeformer());

		if (shape_deformer && shape_deformer->GetKey()) {
			shape_deformer->GetShape(m_blendinshape);

			// Now that we have the previous blend shape saved, we can clear out the key to avoid any
			// further interference.
			KeyBlock *kb;
			for (kb = (KeyBlock *)shape_deformer->GetKey()->block.first; kb; kb = (KeyBlock *)kb->next) {
				kb->curval = 0.f;
			}
		}
	}

	// Now that we have an action, we have something we can play
	m_starttime = KX_GetActiveEngine()->GetFrameTime() - kxscene->GetSuspendedDelta();
	m_startframe = m_localframe = start;
	m_endframe = end;
	m_blendin = blendin;
	m_playmode = play_mode;
	m_blendmode = blend_mode;
	m_blendframe = 0.f;
	m_blendstart = 0.f;
	m_speed = playback_speed;
	m_layer_weight = layer_weight;

	m_done = false;
	m_appliedToObject = false;
	m_requestIpo = false;

	m_prevUpdate = -1.0f;

	return true;
}

bool BL_Action::IsDone()
{
	return m_done;
}

void BL_Action::InitIPO()
{
	// Initialize the IPOs
	for (SG_Controller *cont : m_controllers) {
		cont->SetOption(SG_Controller::SG_CONTR_IPO_RESET, true);
		cont->SetOption(SG_Controller::SG_CONTR_IPO_IPO_AS_FORCE, m_ipo_flags & ACT_IPOFLAG_FORCE);
		cont->SetOption(SG_Controller::SG_CONTR_IPO_IPO_ADD, m_ipo_flags & ACT_IPOFLAG_ADD);
		cont->SetOption(SG_Controller::SG_CONTR_IPO_LOCAL, m_ipo_flags & ACT_IPOFLAG_LOCAL);
	}
}

BL_ActionData *BL_Action::GetActionData()
{
	return (IsDone()) ? nullptr : m_actionData;
}

float BL_Action::GetFrame()
{
	return m_localframe;
}

const std::string BL_Action::GetName()
{
	return (m_actionData) ? m_actionData->GetName() : "";
}

void BL_Action::SetFrame(float frame)
{
	// Clamp the frame to the start and end frame
	if (frame < std::min(m_startframe, m_endframe)) {
		frame = std::min(m_startframe, m_endframe);
	}
	else if (frame > std::max(m_startframe, m_endframe)) {
		frame = std::max(m_startframe, m_endframe);
	}

	m_localframe = frame;
	m_calc_localtime = false;
}

void BL_Action::SetPlayMode(short play_mode)
{
	m_playmode = play_mode;
}

void BL_Action::SetLocalTime(float curtime)
{
	float dt = (curtime - m_starttime) * (float)KX_GetActiveEngine()->GetAnimFrameRate() * m_speed;

	if (m_endframe < m_startframe) {
		dt = -dt;
	}

	m_localframe = m_startframe + dt;
}

void BL_Action::ResetStartTime(float curtime)
{
	float dt = (m_localframe > m_startframe) ? m_localframe - m_startframe : m_startframe - m_localframe;

	m_starttime = curtime - dt / ((float)KX_GetActiveEngine()->GetAnimFrameRate() * m_speed);
	SetLocalTime(curtime);
}

void BL_Action::IncrementBlending(float curtime)
{
	// Setup m_blendstart if we need to
	if (m_blendstart == 0.f) {
		m_blendstart = curtime;
	}

	// Bump the blend frame
	m_blendframe = (curtime - m_blendstart) * (float)KX_GetActiveEngine()->GetAnimFrameRate();

	// Clamp
	if (m_blendframe > m_blendin) {
		m_blendframe = m_blendin;
	}
}


void BL_Action::BlendShape(Key *key, float srcweight, std::vector<float>& blendshape)
{
	std::vector<float>::const_iterator it;
	float dstweight;
	KeyBlock *kb;

	dstweight = 1.0F - srcweight;
	for (it = blendshape.begin(), kb = (KeyBlock *)key->block.first;
	     kb && it != blendshape.end();
	     kb = (KeyBlock *)kb->next, it++)
	{
		kb->curval = kb->curval * dstweight + (*it) * srcweight;
	}
}

void BL_Action::Update(float curtime, bool applyToObject)
{
	/* Don't bother if we're done with the animation and if the animation was already applied to the object.
	 * of if the animation made a double update for the same time and that it was applied to the object.
	 */
	if ((m_done || m_prevUpdate == curtime) && m_appliedToObject) {
		return;
	}
	m_prevUpdate = curtime;

	KX_Scene *scene = m_obj->GetScene();
	curtime -= (float)scene->GetSuspendedDelta();

	if (m_calc_localtime) {
		SetLocalTime(curtime);
	}
	else {
		ResetStartTime(curtime);
		m_calc_localtime = true;
	}

	// Compute minimum and maximum action frame.
	const float minFrame = std::min(m_startframe, m_endframe);
	const float maxFrame = std::max(m_startframe, m_endframe);

	// Handle wrap around
	if (m_localframe < minFrame || m_localframe > maxFrame) {
		switch (m_playmode) {
			case ACT_MODE_PLAY:
			{
				// Clamp
				m_localframe = m_endframe;
				m_done = true;
				break;
			}
			case ACT_MODE_LOOP:
			{
				// Put the time back to the beginning
				m_localframe = m_startframe;
				m_starttime = curtime;
				break;
			}
			case ACT_MODE_PING_PONG:
			{
				m_localframe = m_endframe;
				m_starttime = curtime;

				// Swap the start and end frames
				float temp = m_startframe;
				m_startframe = m_endframe;
				m_endframe = temp;
				break;
			}
		}
	}

	BLI_assert(m_localframe >= minFrame && m_localframe <= maxFrame);

	m_appliedToObject = applyToObject;
	// In case of culled armatures (doesn't requesting to transform the object) we only manages time.
	if (!applyToObject) {
		return;
	}

	m_requestIpo = true;

	SG_Node *node = m_obj->GetNode();
	// Update controllers time.
	for (SG_Controller *cont : m_controllers) {
        cont->SetSimulatedTime(m_localframe);        // update spatial controllers
        cont->Update(node);
    }

	if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
		BL_ArmatureObject *obj = (BL_ArmatureObject *)m_obj;

		if (m_layer_weight >= 0) {
			obj->GetPose(&m_blendpose);
		}

		// Extract the pose from the action
		obj->SetPoseByAction(m_tmpaction, m_localframe);

		// Handle blending between armature actions
		if (m_blendin && m_blendframe < m_blendin) {
			IncrementBlending(curtime);

			// Calculate weight
			float weight = 1.f - (m_blendframe / m_blendin);

			// Blend the poses
			obj->BlendInPose(m_blendinpose, weight, ACT_BLEND_BLEND);
		}


		// Handle layer blending
		if (m_layer_weight >= 0) {
			obj->BlendInPose(m_blendpose, m_layer_weight, m_blendmode);
		}

		obj->UpdateTimestep(curtime);
	}
	else {
		BL_ShapeDeformer *shape_deformer = dynamic_cast<BL_ShapeDeformer *>(m_obj->GetDeformer());

		// Handle shape actions if we have any
		if (shape_deformer && shape_deformer->GetKey()) {
			Key *key = shape_deformer->GetKey();

			PointerRNA ptrrna;
			RNA_id_pointer_create(&key->id, &ptrrna);

			animsys_evaluate_action(&ptrrna, m_tmpaction, nullptr, m_localframe);

			// Handle blending between shape actions
			if (m_blendin && m_blendframe < m_blendin) {
				IncrementBlending(curtime);

				float weight = 1.f - (m_blendframe / m_blendin);

				// We go through and clear out the keyblocks so there isn't any interference
				// from other shape actions
				KeyBlock *kb;
				for (kb = (KeyBlock *)key->block.first; kb; kb = (KeyBlock *)kb->next) {
					kb->curval = 0.f;
				}

				// Now blend the shape
				BlendShape(key, weight, m_blendinshape);
			}

			// Handle layer blending
			if (m_layer_weight >= 0) {
				shape_deformer->GetShape(m_blendshape);
				BlendShape(key, m_layer_weight, m_blendshape);
			}

			shape_deformer->SetLastFrame(curtime);
		}
	}

	// If the action is done we can remove its scene graph IPO controller.
	if (m_done) {
		ClearControllerList();
	}
}

void BL_Action::UpdateIPOs()
{
	if (m_requestIpo) {
        m_obj->GetNode()->UpdateWorldDataThread();
		m_requestIpo = false;
	}
}
