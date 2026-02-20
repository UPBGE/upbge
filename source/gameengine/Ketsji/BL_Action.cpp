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

#include "BL_Action.h"

#include "ANIM_action.hh"
#include "BKE_context.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "RNA_access.hh"

#include "BL_ArmatureObject.h"
#include "BL_IpoConvert.h"
#include "CM_Message.h"

using namespace blender;

using namespace blender::animrig;

BL_Action::BL_Action(class KX_GameObject *gameobj)
    : m_action(nullptr),
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
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
  /* See: 686ab4c9401a90b22fb17e46c992eb513fe4f693
   * This AnimtionEvalContext will not be used directly but
   * will be used to create other blender::AnimationEvalContext with
   * localActionTime (m_localFrame) each frame. We need to
   * construct a new blender::AnimationEvalContext each frame because
   * blender::AnimationEvalContext->eval_time is const .
   */
  m_animEvalCtx = BKE_animsys_eval_context_construct(depsgraph, 0.0);
}

BL_Action::~BL_Action()
{
  if (m_blendpose)
    BKE_pose_free(m_blendpose);
  if (m_blendinpose)
    BKE_pose_free(m_blendinpose);
  ClearControllerList();

  // free shape key snapshots
  m_blendshape.clear();
  m_blendinshape.clear();

  blender::Object *ob = m_obj->GetBlenderObject();
  if (ob && ob->adt && m_action) {
    ob->adt->action = m_action;
  }
}

void BL_Action::ClearControllerList()
{
  // Clear out the controller list
  std::vector<SG_Controller *>::iterator it;
  for (it = m_sg_contr_list.begin(); it != m_sg_contr_list.end(); it++) {
    m_obj->GetSGNode()->RemoveSGController((*it));
    delete *it;
  }

  m_sg_contr_list.clear();
}

bool BL_Action::Play(const std::string &name,
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
  if (!IsDone() && priority > m_priority)
    return false;
  m_priority = priority;
  blender::bAction *prev_action = m_action;

  KX_Scene *kxscene = m_obj->GetScene();

  // First try to load the action
  m_action = (blender::bAction *)kxscene->GetLogicManager()->GetActionByName(name);
  if (!m_action) {
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
  if (!IsDone() && m_action == prev_action && m_startframe == start && m_endframe == end &&
      m_priority == priority && m_speed == playback_speed)
    return false;

  // First get rid of any old controllers
  ClearControllerList();

  // Create an SG_Controller
  SG_Controller *sg_contr = BL_CreateIPO(m_action, m_obj, kxscene);
  m_sg_contr_list.push_back(sg_contr);
  m_obj->GetSGNode()->AddSGController(sg_contr);
  sg_contr->SetNode(m_obj->GetSGNode());

  // Try obcolor
  sg_contr = BL_CreateObColorIPO(m_action, m_obj, kxscene);
  if (sg_contr) {
    m_sg_contr_list.push_back(sg_contr);
    m_obj->GetSGNode()->AddSGController(sg_contr);
    sg_contr->SetNode(m_obj->GetSGNode());
  }

  // Extra controllers
  if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_LIGHT) {
    sg_contr = BL_CreateLampIPO(m_action, m_obj, kxscene);
    m_sg_contr_list.push_back(sg_contr);
    m_obj->GetSGNode()->AddSGController(sg_contr);
    sg_contr->SetNode(m_obj->GetSGNode());
  }
  else if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_CAMERA) {
    sg_contr = BL_CreateCameraIPO(m_action, m_obj, kxscene);
    m_sg_contr_list.push_back(sg_contr);
    m_obj->GetSGNode()->AddSGController(sg_contr);
    sg_contr->SetNode(m_obj->GetSGNode());
  }

  m_ipo_flags = ipo_flags;
  InitIPO();

  // Setup blendin shapes/poses
  if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
    BL_ArmatureObject *obj = (BL_ArmatureObject *)m_obj;
    obj->GetPose(&m_blendinpose);
  }
  else {
    /* Capture a snapshot of current shape key curvals to blend from.
     * Only do this when we actually will blend (blendin > 0 or layer active). */
    if (blendin > 0.0f || layer_weight >= 0.0f) {
      blender::Object *ob = m_obj->GetBlenderObject();
      m_blendinshape.clear();
      m_blendshape.clear();
      if (ob && ob->type == OB_MESH) {
        blender::Mesh *me = id_cast<blender::Mesh *>(ob->data);
        if (me && me->key) {
          /* Reserve exact size (totkey minus possibly the refkey). */
          int tot = me->key->totkey;
          int has_ref = (me->key->refkey != nullptr) ? 1 : 0;
          int expected = std::max(0, tot - has_ref);
          m_blendinshape.resize(expected);
          int i = 0;
          for (KeyBlock *kb = static_cast<KeyBlock *>(me->key->block.first); kb; kb = kb->next) {
            if (kb == me->key->refkey) {
              continue;
            }
            m_blendinshape[i++] = kb->curval;
          }
          /* keep a working buffer if needed later */
          m_blendshape = m_blendinshape;
        }
      }
    }
  }

  // Now that we have an action, we have something we can play
  m_starttime = KX_GetActiveEngine()->GetFrameTime();
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
  std::vector<SG_Controller *>::iterator it;
  for (it = m_sg_contr_list.begin(); it != m_sg_contr_list.end(); it++) {
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_RESET, true);
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_IPO_AS_FORCE, m_ipo_flags & ACT_IPOFLAG_FORCE);
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_IPO_ADD, m_ipo_flags & ACT_IPOFLAG_ADD);
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_LOCAL, m_ipo_flags & ACT_IPOFLAG_LOCAL);
  }
}

blender::bAction *BL_Action::GetAction()
{
  return (IsDone()) ? nullptr : m_action;
}

float BL_Action::GetFrame()
{
  return m_localframe;
}

const std::string BL_Action::GetName()
{
  if (m_action != nullptr) {
    return m_action->id.name + 2;
  }
  else {
    return "";
  }
}

void BL_Action::SetFrame(float frame)
{
  // Clamp the frame to the start and end frame
  if (frame < std::min(m_startframe, m_endframe))
    frame = std::min(m_startframe, m_endframe);
  else if (frame > std::max(m_startframe, m_endframe))
    frame = std::max(m_startframe, m_endframe);

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

  if (m_endframe < m_startframe)
    dt = -dt;

  m_localframe = m_startframe + dt;
}

void BL_Action::ResetStartTime(float curtime)
{
  float dt = (m_localframe > m_startframe) ? m_localframe - m_startframe :
                                             m_startframe - m_localframe;

  m_starttime = curtime - dt / ((float)KX_GetActiveEngine()->GetAnimFrameRate() * m_speed);
  SetLocalTime(curtime);
}

void BL_Action::IncrementBlending(float curtime)
{
  // Setup m_blendstart if we need to
  if (m_blendstart == 0.f)
    m_blendstart = curtime;

  // Bump the blend frame
  m_blendframe = (curtime - m_blendstart) * (float)KX_GetActiveEngine()->GetAnimFrameRate();

  // Clamp
  if (m_blendframe > m_blendin)
    m_blendframe = m_blendin;
}

void BL_Action::BlendShape(blender::Key *key, float srcweight, std::vector<float> &blendshape)
{
  if (!key) {
    return;
  }

  const float dstweight = 1.0f - srcweight;
  size_t i = 0;
  KeyBlock *kb = static_cast<KeyBlock *>(key->block.first);

  /* Advance through blocks, but only consume `blendshape` entries for non-refkey
   * entries so indexes stay aligned with the snapshot created in Play(). */
  for (; kb && i < blendshape.size(); kb = kb->next) {
    if (kb == key->refkey) {
      continue;
    }
    kb->curval = kb->curval * dstweight + blendshape[i] * srcweight;
    ++i;
  }
}

enum eActionType {
  ACT_TYPE_MODIFIER = 0,
  ACT_TYPE_CONSTRAINT,
  ACT_TYPE_IDPROP,
};

/* Ensure name of data (ModifierData, blender::bConstraint...) matches m_action's FCurve rna path */
static bool ActionMatchesName(blender::bAction *action, char *name, eActionType type)
{
  // std::cout << "curves listbase len: " << BLI_listbase_count(&action->curves) << std::endl;
  Action &new_action = action->wrap();

  for (Layer *layer : new_action.layers()) {
    for (Strip *strip : layer->strips()) {
      if (strip->type() != Strip::Type::Keyframe) {
        continue;
      }
      for (Channelbag *bag : strip->data<StripKeyframeData>(new_action).channelbags()) {
        for (FCurve *fcu : bag->fcurves()) {
          if (fcu->rna_path) {
            char pattern[256];
            char md_name_esc[sizeof(name) * 2];
            switch (type) {
              case ACT_TYPE_MODIFIER:
                BLI_str_escape(md_name_esc, name, sizeof(md_name_esc));
                BLI_snprintf(pattern, sizeof(pattern), "modifiers[\"%s\"]", md_name_esc);
                break;
              case ACT_TYPE_CONSTRAINT:
                BLI_str_escape(md_name_esc, name, sizeof(md_name_esc));
                BLI_snprintf(pattern, sizeof(pattern), "constraints[\"%s\"]", md_name_esc);
                break;
              case ACT_TYPE_IDPROP:
                BLI_str_escape(md_name_esc, name, sizeof(md_name_esc));
                BLI_snprintf(pattern, sizeof(pattern), "[\"%s\"]", md_name_esc);
                break;
              default:
                BLI_str_escape(pattern, "", sizeof(pattern));
                break;
            }
            // std::cout << "fcu name: " << fcu->rna_path << std::endl;
            // std::cout << "data name: " << pattern << std::endl;
            /* Find a correspondance between ob->modifier/ob->constraint... and actuator action
             * (m_action) */
            if (strstr(fcu->rna_path, pattern)) {
              // std::cout << "fcu and name match" << std::endl;
              return true;
            }
          }
        }
      }
    }
  }
  // std::cout << "fcu and name DON'T match" << std::endl;
  return false;
}

void BL_Action::Update(float curtime, bool applyToObject)
{
  /* Don't bother if we're done with the animation and if the animation was already applied to the
   * object. of if the animation made a double update for the same time and that it was applied to
   * the object.
   */
  if (ShouldSkipUpdate(curtime, applyToObject)) {
    return;
  }

  m_prevUpdate = curtime;

  // Update timing
  UpdateActionTiming(curtime);

  // Handle frame wrapping based on play mode
  HandleFrameWrapping(curtime);

  m_appliedToObject = applyToObject;

  // In case of culled armatures (doesn't requesting to transform the object) we only manages time.
  if (!applyToObject) {
    return;
  }

  // Update controllers and apply animation
  UpdateControllersAndAnimation(curtime);

  // If the action is done we can remove its scene graph IPO controller.
  if (m_done) {
    ClearControllerList();
  }
}

bool BL_Action::ShouldSkipUpdate(float curtime, bool applyToObject)
{
  return ((m_done || m_prevUpdate == curtime) && m_appliedToObject);
}

void BL_Action::UpdateActionTiming(float curtime)
{
  if (m_calc_localtime) {
    SetLocalTime(curtime);
  }
  else {
    ResetStartTime(curtime);
    m_calc_localtime = true;
  }
}

void BL_Action::HandleFrameWrapping(float curtime)
{
  // Compute minimum and maximum action frame.
  const float minFrame = std::min(m_startframe, m_endframe);
  const float maxFrame = std::max(m_startframe, m_endframe);

  // Handle wrap around
  if (m_localframe < minFrame || m_localframe > maxFrame) {
    switch (m_playmode) {
      case ACT_MODE_PLAY:
        // Clamp
        m_localframe = m_endframe;
        m_done = true;
        break;
      case ACT_MODE_LOOP:
        // Put the time back to the beginning
        m_localframe = m_startframe;
        m_starttime = curtime;
        break;
      case ACT_MODE_PING_PONG:
        m_localframe = m_endframe;
        m_starttime = curtime;
        // Swap the start and end frames
        std::swap(m_startframe, m_endframe);
        break;
    }
  }

  BLI_assert(m_localframe >= minFrame && m_localframe <= maxFrame);
}

void BL_Action::UpdateControllersAndAnimation(float curtime)
{
  // Update spatial controllers
  UpdateSpatialControllers();

  blender::Object *ob = m_obj->GetBlenderObject();  // eevee

  /* Create an blender::AnimationEvalContext based on the current local frame time (See comment in
   * constructor) */
  blender::AnimationEvalContext animEvalContext = BKE_animsys_eval_context_construct_at(&m_animEvalCtx,
                                                                               m_localframe);

  if (m_obj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
    UpdateArmatureAnimation(curtime, ob, animEvalContext);
  }
  else {
    UpdateObjectAnimation(ob, animEvalContext);
  }
}

void BL_Action::UpdateSpatialControllers()
{
  // Update controllers time. The controllers list is cleared when action is done
  for (SG_Controller *cont : m_sg_contr_list) {
    cont->SetSimulatedTime(m_localframe);  // update spatial controllers
    cont->Update(m_localframe);
    m_requestIpo = true;
  }
}

void BL_Action::UpdateArmatureAnimation(float curtime,
                                        blender::Object *ob,
                                        const blender::AnimationEvalContext &animEvalContext)
{
  BL_ArmatureObject *obj = (BL_ArmatureObject *)m_obj;
  KX_Scene *scene = m_obj->GetScene();

  obj->RemapParentChildren();

  if (m_layer_weight >= 0) {
    obj->GetPose(&m_blendpose);
  }

  obj->ApplyAction(m_action, animEvalContext);

  ProcessArmatureBlending(obj, curtime);

  obj->ApplyPose();

  /* GPU skinning handled on blender side now */
  ProcessPipeline(obj, ob, scene, animEvalContext);

  obj->UpdateTimestep(curtime);
}

void BL_Action::ProcessPipeline(BL_ArmatureObject *obj,
                                   blender::Object *ob,
                                   KX_Scene *scene,
                                   const blender::AnimationEvalContext &animEvalContext)
{
  // === CPU/GPU PIPELINES ===
  bool is_running_gpu_skinning = false;
  for (KX_GameObject *child : m_obj->GetChildren()) {
    blender::Object *child_ob = child->GetBlenderObject();
    if (!child_ob) {
      continue;
    }
    if (child_ob->type != OB_MESH) {
      continue;
    }
    blender::Mesh *me = id_cast<blender::Mesh *>(child_ob->data);
    if (me->is_running_gpu_animation_playback) {
      is_running_gpu_skinning = true;
      break;
    }
  }
  if (!is_running_gpu_skinning) {
    scene->AppendToIdsToUpdate(&ob->id, ID_RECALC_TRANSFORM, ob->gameflag & OB_OVERLAY_COLLECTION);
  }
  m_obj->ForceIgnoreParentTx();
}

void BL_Action::ProcessArmatureBlending(BL_ArmatureObject *obj, float curtime)
{
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
}

void BL_Action::UpdateObjectAnimation(blender::Object *ob, const blender::AnimationEvalContext &animEvalContext)
{
  KX_Scene *scene = m_obj->GetScene();
  /* To skip some code if not needed */
  bool actionIsUpdated = false;

  /* WARNING: The check to be sure the right action is played (to know if the action
   * which is in the actuator will be the one which will be played)
   * might be wrong (if (ob->adt && ob->adt->action == m_action) playaction;)
   * because WE MIGHT NEED TO CHANGE OB->ADT->ACTION DURING RUNTIME
   * then another check should be found to ensure to play the right action.
   */
  // TEST KEYFRAMED MODIFIERS (WRONG CODE BUT JUST FOR TESTING PURPOSE)
  if (!actionIsUpdated) {
    actionIsUpdated = TryUpdateModifierActions(ob, scene, animEvalContext);
  }

  if (!actionIsUpdated) {
    // TEST FollowPath action
    actionIsUpdated = TryUpdateConstraintActions(ob, scene, animEvalContext);
  }

  // TEST IDPROP ACTIONS
  if (!actionIsUpdated) {
    actionIsUpdated = TryUpdateIDPropertyActions(ob, scene, animEvalContext);
  }

  if (!actionIsUpdated) {
    // Node Trees actions (Geometry one and Shader ones (material, world))
    actionIsUpdated = TryUpdateNodeTreeActions(scene, animEvalContext);
  }

  if (!actionIsUpdated) {
    // TEST Shapekeys action
    TryUpdateShapeKeyActions(ob, scene, animEvalContext);
  }
}

bool BL_Action::TryUpdateModifierActions(blender::Object *ob,
                                         KX_Scene *scene,
                                         const blender::AnimationEvalContext &animEvalContext)
{
  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
    bool isRightAction = ActionMatchesName(m_action, md->name, ACT_TYPE_MODIFIER);
    // TODO: We need to find the good notifier per action
    if (isRightAction) {
      IDRecalcFlag flag = BKE_modifier_is_non_geometrical(md) ? ID_RECALC_TRANSFORM :
                                                                ID_RECALC_GEOMETRY;
      bool skip_depsgraph = false;
      if (ob->type == OB_MESH) {
        blender::Mesh *me = id_cast<blender::Mesh *>(ob->data);
        skip_depsgraph = (me && me->is_running_gpu_animation_playback);
      }
      if (!skip_depsgraph) {
        scene->AppendToIdsToUpdate(&ob->id, flag, ob->gameflag & OB_OVERLAY_COLLECTION);
      }
      else {
        bContext *C = KX_GetActiveEngine()->GetContext();
        Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
        Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
        DEG_bump_update_count(depsgraph);
        ob_eval->runtime->last_update_geometry = DEG_get_update_count(depsgraph);
      }

      blender::PointerRNA ptrrna = RNA_id_pointer_create(&ob->id);
      const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(
          *m_action);
      animsys_evaluate_action(&ptrrna, m_action, slot_handle, &animEvalContext, false);
      return true;
    }
  }
  return false;
}

bool BL_Action::TryUpdateConstraintActions(blender::Object *ob,
                                           KX_Scene *scene,
                                           const blender::AnimationEvalContext &animEvalContext)
{
  for (blender::bConstraint *con = (blender::bConstraint *)ob->constraints.first; con; con = con->next) {
    if (ActionMatchesName(m_action, con->name, ACT_TYPE_CONSTRAINT)) {
      if (!scene->OrigObCanBeTransformedInRealtime(ob)) {
        return false;
      }

      scene->AppendToIdsToUpdate(
          &ob->id, ID_RECALC_TRANSFORM, ob->gameflag & OB_OVERLAY_COLLECTION);
      blender::PointerRNA ptrrna = RNA_id_pointer_create(&ob->id);
      const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(
          *m_action);
      animsys_evaluate_action(&ptrrna, m_action, slot_handle, &animEvalContext, false);

      m_obj->ForceIgnoreParentTx();
      return true;
      /* HERE we can add other constraint action types,
       * if some actions require another notifier than ID_RECALC_TRANSFORM */
    }
  }
  return false;
}

bool BL_Action::TryUpdateIDPropertyActions(blender::Object *ob,
                                           KX_Scene *scene,
                                           const blender::AnimationEvalContext &animEvalContext)
{
  if (!ob->id.properties) {
    return false;
  }

  for (blender::IDProperty *prop = (blender::IDProperty *)ob->id.properties->data.group.first; prop; prop = prop->next) {
    if (prop->type == IDP_GROUP) {
      continue;
    }

    if (ActionMatchesName(m_action, prop->name, ACT_TYPE_IDPROP)) {
      scene->AppendToIdsToUpdate(
          &ob->id, ID_RECALC_TRANSFORM, ob->gameflag & OB_OVERLAY_COLLECTION);
      blender::PointerRNA ptrrna = RNA_id_pointer_create(&ob->id);
      const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(
          *m_action);
      animsys_evaluate_action(&ptrrna, m_action, slot_handle, &animEvalContext, false);
      return true;
    }
  }
  return false;
}

bool BL_Action::TryUpdateNodeTreeActions(KX_Scene *scene,
                                         const blender::AnimationEvalContext &animEvalContext)
{
  blender::Main *bmain = KX_GetActiveEngine()->GetConverter()->GetMain();

  FOREACH_NODETREE_BEGIN (bmain, nodetree, id) {
    if (IsNodeTreeActionMatch(nodetree)) {
      scene->AppendToIdsToUpdate(&nodetree->id, static_cast<IDRecalcFlag>(0), false);
      blender::PointerRNA ptrrna = RNA_id_pointer_create(&nodetree->id);
      const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(
          *m_action);
      animsys_evaluate_action(&ptrrna, m_action, slot_handle, &animEvalContext, false);
      return true;
    }
  }
  FOREACH_NODETREE_END;

  return false;
}

bool BL_Action::IsNodeTreeActionMatch(blender::bNodeTree *nodetree)
{
  bool isRightAction = false;
  isRightAction = (nodetree->adt && nodetree->adt->action == m_action);
  if (!isRightAction && nodetree->adt && nodetree->adt->nla_tracks.first) {
    for (NlaTrack &track : nodetree->adt->nla_tracks) {
      for (NlaStrip &strip : track.strips) {
        if (strip.act == m_action) {
          isRightAction = true;
          break;
        }
      }
    }
  }
  return isRightAction;
}

bool BL_Action::TryUpdateShapeKeyActions(blender::Object *ob,
                                         KX_Scene *scene,
                                         const blender::AnimationEvalContext &animEvalContext)
{
  blender::Mesh *me = (blender::Mesh *)ob->data;
  if (ob->type == OB_MESH && me) {
    const bool bHasShapeKey = me->key && me->key->type == KEY_RELATIVE;
    bool has_animdata = bHasShapeKey && me->key->adt;
    bool play_normal_key_action = has_animdata && me->key->adt->action == m_action;
    bool play_nla_key_action = false;
    if (!play_normal_key_action && has_animdata) {
      for (NlaTrack &track : me->key->adt->nla_tracks) {
        for (NlaStrip &strip : track.strips) {
          if (strip.act == m_action) {
            play_nla_key_action = true;
            break;
          }
        }
      }
    }

    if (play_normal_key_action || play_nla_key_action) {
      bool gpu_deformation = me->is_running_gpu_animation_playback;
      if (!gpu_deformation) {
        scene->AppendToIdsToUpdate(&me->id, ID_RECALC_GEOMETRY, false);
      }
      blender::Key *key = me->key;

      blender::PointerRNA ptrrna = RNA_id_pointer_create(&key->id);
      const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(
          *m_action);
      animsys_evaluate_action(&ptrrna, m_action, slot_handle, &animEvalContext, false);

      ProcessShapeKeyBlending(key);
      return true;
    }
  }
  return false;
}

bool BL_Action::IsNLAShapeKeyActionMatch(blender::Key *key)
{
  for (NlaTrack &track : key->adt->nla_tracks) {
    for (NlaStrip &strip : track.strips) {
      if (strip.act == m_action) {
        return true;
      }
    }
  }
  return false;
}

void BL_Action::ProcessShapeKeyBlending(blender::Key *key)
{
  if (!key) {
    return;
  }

  /* Blend-in: interpolate from captured snapshot to evaluated action values.
   * We clear curvals first to avoid interference then apply the snapshot blend.
   * Skip the refkey so we keep alignment with m_blendinshape which stores only
   * non-ref keyblocks. */
  if (m_blendin && m_blendframe < m_blendin && !m_blendinshape.empty()) {
    IncrementBlending(KX_GetActiveEngine()->GetFrameTime());
    float weight = 1.0f - (m_blendframe / m_blendin);

    for (KeyBlock *kb = static_cast<KeyBlock *>(key->block.first); kb; kb = kb->next) {
      if (kb == key->refkey) {
        continue;
      }
      kb->curval = 0.0f;
    }

    BlendShape(key, weight, m_blendinshape);
  }

  /* Layer blending: apply stored layer snapshot on top if requested.
   * m_blendshape was captured similarly to m_blendinshape. */
  if (m_layer_weight >= 0.0f && !m_blendshape.empty()) {
    BlendShape(key, m_layer_weight, m_blendshape);
  }
}

/* To sync m_obj and children in SceneGraph after potential m_obj transform update in SG_Controller actions */
/* (In KX_IpoController.cpp, NodeSetLocalPosition can be called for example, but NodeUpdateGS
 * causes an issue, then update is done here) */
void BL_Action::UpdateIPOs()
{
  if (m_requestIpo) {
    m_obj->GetSGNode()->UpdateWorldData(0.0);
    m_requestIpo = false;
  }
}
