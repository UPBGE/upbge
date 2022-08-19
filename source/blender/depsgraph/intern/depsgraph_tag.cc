/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2013 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup depsgraph
 *
 * Core routines for how the Depsgraph works.
 */

#include "intern/depsgraph_tag.h"

#include <cstdio>
#include <cstring> /* required for memset */
#include <queue>

#include "BLI_math_bits.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "DNA_anim_types.h"
#include "DNA_curve_types.h"
#include "DNA_key_types.h"
#include "DNA_lattice_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_anim_data.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_node.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_workspace.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_debug.h"
#include "DEG_depsgraph_query.h"

#include "intern/builder/deg_builder.h"
#include "intern/depsgraph.h"
#include "intern/depsgraph_registry.h"
#include "intern/depsgraph_update.h"
#include "intern/eval/deg_eval_copy_on_write.h"
#include "intern/eval/deg_eval_flush.h"
#include "intern/node/deg_node.h"
#include "intern/node/deg_node_component.h"
#include "intern/node/deg_node_factory.h"
#include "intern/node/deg_node_id.h"
#include "intern/node/deg_node_operation.h"
#include "intern/node/deg_node_time.h"

namespace deg = blender::deg;

/* *********************** */
/* Update Tagging/Flushing */

namespace blender::deg {

namespace {

void depsgraph_geometry_tag_to_component(const ID *id, NodeType *component_type)
{
  const NodeType result = geometry_tag_to_component(id);
  if (result != NodeType::UNDEFINED) {
    *component_type = result;
  }
}

bool is_selectable_data_id_type(const ID_Type id_type)
{
  return ELEM(id_type, ID_ME, ID_CU_LEGACY, ID_MB, ID_LT, ID_GD, ID_CV, ID_PT, ID_VO);
}

void depsgraph_select_tag_to_component_opcode(const ID *id,
                                              NodeType *component_type,
                                              OperationCode *operation_code)
{
  const ID_Type id_type = GS(id->name);
  if (id_type == ID_SCE) {
    /* We need to flush base flags to all objects in a scene since we
     * don't know which ones changed. However, we don't want to update
     * the whole scene, so pick up some operation which will do as less
     * as possible.
     *
     * TODO(sergey): We can introduce explicit exit operation which
     * does nothing and which is only used to cascade flush down the
     * road. */
    *component_type = NodeType::LAYER_COLLECTIONS;
    *operation_code = OperationCode::VIEW_LAYER_EVAL;
  }
  else if (id_type == ID_OB) {
    *component_type = NodeType::OBJECT_FROM_LAYER;
    *operation_code = OperationCode::OBJECT_FROM_LAYER_ENTRY;
  }
  else if (id_type == ID_MC) {
    *component_type = NodeType::BATCH_CACHE;
    *operation_code = OperationCode::MOVIECLIP_SELECT_UPDATE;
  }
  else if (is_selectable_data_id_type(id_type)) {
    *component_type = NodeType::BATCH_CACHE;
    *operation_code = OperationCode::GEOMETRY_SELECT_UPDATE;
  }
  else {
    *component_type = NodeType::COPY_ON_WRITE;
    *operation_code = OperationCode::COPY_ON_WRITE;
  }
}

void depsgraph_base_flags_tag_to_component_opcode(const ID *id,
                                                  NodeType *component_type,
                                                  OperationCode *operation_code)
{
  const ID_Type id_type = GS(id->name);
  if (id_type == ID_SCE) {
    *component_type = NodeType::LAYER_COLLECTIONS;
    *operation_code = OperationCode::VIEW_LAYER_EVAL;
  }
  else if (id_type == ID_OB) {
    *component_type = NodeType::OBJECT_FROM_LAYER;
    *operation_code = OperationCode::OBJECT_BASE_FLAGS;
  }
}

OperationCode psysTagToOperationCode(IDRecalcFlag tag)
{
  if (tag == ID_RECALC_PSYS_RESET) {
    return OperationCode::PARTICLE_SETTINGS_RESET;
  }
  return OperationCode::OPERATION;
}

void depsgraph_tag_to_component_opcode(const ID *id,
                                       IDRecalcFlag tag,
                                       NodeType *component_type,
                                       OperationCode *operation_code)
{
  const ID_Type id_type = GS(id->name);
  *component_type = NodeType::UNDEFINED;
  *operation_code = OperationCode::OPERATION;
  /* Special case for now, in the future we should get rid of this. */
  if (tag == 0) {
    *component_type = NodeType::ID_REF;
    *operation_code = OperationCode::OPERATION;
    return;
  }
  switch (tag) {
    case ID_RECALC_TRANSFORM:
      *component_type = NodeType::TRANSFORM;
      break;
    case ID_RECALC_GEOMETRY:
      depsgraph_geometry_tag_to_component(id, component_type);
      break;
    case ID_RECALC_ANIMATION:
      *component_type = NodeType::ANIMATION;
      break;
    case ID_RECALC_PSYS_REDO:
    case ID_RECALC_PSYS_RESET:
    case ID_RECALC_PSYS_CHILD:
    case ID_RECALC_PSYS_PHYS:
      if (id_type == ID_PA) {
        /* NOTES:
         * - For particle settings node we need to use different
         *   component. Will be nice to get this unified with object,
         *   but we can survive for now with single exception here.
         *   Particles needs reconsideration anyway, */
        *component_type = NodeType::PARTICLE_SETTINGS;
        *operation_code = psysTagToOperationCode(tag);
      }
      else {
        *component_type = NodeType::PARTICLE_SYSTEM;
      }
      break;
    case ID_RECALC_COPY_ON_WRITE:
      *component_type = NodeType::COPY_ON_WRITE;
      break;
    case ID_RECALC_SHADING:
      *component_type = NodeType::SHADING;
      break;
    case ID_RECALC_SELECT:
      depsgraph_select_tag_to_component_opcode(id, component_type, operation_code);
      break;
    case ID_RECALC_BASE_FLAGS:
      depsgraph_base_flags_tag_to_component_opcode(id, component_type, operation_code);
      break;
    case ID_RECALC_POINT_CACHE:
      *component_type = NodeType::POINT_CACHE;
      break;
    case ID_RECALC_EDITORS:
      /* There is no such node in depsgraph, this tag is to be handled
       * separately. */
      break;
    case ID_RECALC_SEQUENCER_STRIPS:
      *component_type = NodeType::SEQUENCER;
      break;
    case ID_RECALC_FRAME_CHANGE:
    case ID_RECALC_AUDIO_FPS:
    case ID_RECALC_AUDIO_VOLUME:
    case ID_RECALC_AUDIO_MUTE:
    case ID_RECALC_AUDIO_LISTENER:
    case ID_RECALC_AUDIO:
      *component_type = NodeType::AUDIO;
      break;
    case ID_RECALC_PARAMETERS:
      *component_type = NodeType::PARAMETERS;
      break;
    case ID_RECALC_SOURCE:
      *component_type = NodeType::PARAMETERS;
      break;
    case ID_RECALC_GEOMETRY_ALL_MODES:
    case ID_RECALC_ALL:
    case ID_RECALC_PSYS_ALL:
      BLI_assert_msg(0, "Should not happen");
      break;
    case ID_RECALC_TAG_FOR_UNDO:
      break; /* Must be ignored by depsgraph. */
    case ID_RECALC_NTREE_OUTPUT:
      *component_type = NodeType::NTREE_OUTPUT;
      *operation_code = OperationCode::NTREE_OUTPUT;
      break;

    case ID_RECALC_PROVISION_26:
    case ID_RECALC_PROVISION_27:
    case ID_RECALC_PROVISION_28:
    case ID_RECALC_PROVISION_29:
    case ID_RECALC_PROVISION_30:
    case ID_RECALC_PROVISION_31:
      /* Silently ignore.
       * The bits might be passed here from ID_RECALC_ALL. This is not a code-mistake, but just the
       * way how the recalc flags are handled. */
      break;
  }
}

void id_tag_update_ntree_special(
    Main *bmain, Depsgraph *graph, ID *id, unsigned int flags, eUpdateSource update_source)
{
  bNodeTree *ntree = ntreeFromID(id);
  if (ntree == nullptr) {
    return;
  }
  graph_id_tag_update(bmain, graph, &ntree->id, flags, update_source);
}

void depsgraph_update_editors_tag(Main *bmain, Depsgraph *graph, ID *id)
{
  /* NOTE: We handle this immediately, without delaying anything, to be
   * sure we don't cause threading issues with OpenGL. */
  /* TODO(sergey): Make sure this works for CoW-ed data-blocks as well. */
  DEGEditorUpdateContext update_ctx = {nullptr};
  update_ctx.bmain = bmain;
  update_ctx.depsgraph = (::Depsgraph *)graph;
  update_ctx.scene = graph->scene;
  update_ctx.view_layer = graph->view_layer;
  deg_editors_id_update(&update_ctx, id);
}

void depsgraph_id_tag_copy_on_write(Depsgraph *graph, IDNode *id_node, eUpdateSource update_source)
{
  ComponentNode *cow_comp = id_node->find_component(NodeType::COPY_ON_WRITE);
  if (cow_comp == nullptr) {
    BLI_assert(!deg_copy_on_write_is_needed(GS(id_node->id_orig->name)));
    return;
  }
  cow_comp->tag_update(graph, update_source);
}

void depsgraph_tag_component(Depsgraph *graph,
                             IDNode *id_node,
                             NodeType component_type,
                             OperationCode operation_code,
                             eUpdateSource update_source)
{
  ComponentNode *component_node = id_node->find_component(component_type);
  /* NOTE: Animation component might not be existing yet (which happens when adding new driver or
   * adding a new keyframe), so the required copy-on-write tag needs to be taken care explicitly
   * here. */
  if (component_node == nullptr) {
    if (component_type == NodeType::ANIMATION) {
      id_node->is_cow_explicitly_tagged = true;
      depsgraph_id_tag_copy_on_write(graph, id_node, update_source);
    }
    return;
  }
  if (operation_code == OperationCode::OPERATION) {
    component_node->tag_update(graph, update_source);
  }
  else {
    OperationNode *operation_node = component_node->find_operation(operation_code);
    if (operation_node != nullptr) {
      operation_node->tag_update(graph, update_source);
    }
  }
  /* If component depends on copy-on-write, tag it as well. */
  if (component_node->need_tag_cow_before_update()) {
    depsgraph_id_tag_copy_on_write(graph, id_node, update_source);
  }
  if (component_type == NodeType::COPY_ON_WRITE) {
    id_node->is_cow_explicitly_tagged = true;
  }
}

/* This is a tag compatibility with legacy code.
 *
 * Mainly, old code was tagging object with ID_RECALC_GEOMETRY tag to inform
 * that object's data data-block changed. Now API expects that ID is given
 * explicitly, but not all areas are aware of this yet. */
void deg_graph_id_tag_legacy_compat(
    Main *bmain, Depsgraph *depsgraph, ID *id, IDRecalcFlag tag, eUpdateSource update_source)
{
  if (ELEM(tag, ID_RECALC_GEOMETRY, 0)) {
    switch (GS(id->name)) {
      case ID_OB: {
        Object *object = (Object *)id;
        ID *data_id = (ID *)object->data;
        if (data_id != nullptr) {
          graph_id_tag_update(bmain, depsgraph, data_id, 0, update_source);
        }
        break;
      }
      /* TODO(sergey): Shape keys are annoying, maybe we should find a
       * way to chain geometry evaluation to them, so we don't need extra
       * tagging here. */
      case ID_ME: {
        Mesh *mesh = (Mesh *)id;
        if (mesh->key != nullptr) {
          ID *key_id = &mesh->key->id;
          if (key_id != nullptr) {
            graph_id_tag_update(bmain, depsgraph, key_id, 0, update_source);
          }
        }
        break;
      }
      case ID_LT: {
        Lattice *lattice = (Lattice *)id;
        if (lattice->key != nullptr) {
          ID *key_id = &lattice->key->id;
          if (key_id != nullptr) {
            graph_id_tag_update(bmain, depsgraph, key_id, 0, update_source);
          }
        }
        break;
      }
      case ID_CU_LEGACY: {
        Curve *curve = (Curve *)id;
        if (curve->key != nullptr) {
          ID *key_id = &curve->key->id;
          if (key_id != nullptr) {
            graph_id_tag_update(bmain, depsgraph, key_id, 0, update_source);
          }
        }
        break;
      }
      default:
        break;
    }
  }
}

void graph_id_tag_update_single_flag(Main *bmain,
                                     Depsgraph *graph,
                                     ID *id,
                                     IDNode *id_node,
                                     IDRecalcFlag tag,
                                     eUpdateSource update_source)
{
  if (tag == ID_RECALC_EDITORS) {
    if (graph != nullptr && graph->is_active) {
      depsgraph_update_editors_tag(bmain, graph, id);
    }
    return;
  }
  /* Get description of what is to be tagged. */
  NodeType component_type;
  OperationCode operation_code;
  depsgraph_tag_to_component_opcode(id, tag, &component_type, &operation_code);
  /* Check whether we've got something to tag. */
  if (component_type == NodeType::UNDEFINED) {
    /* Given ID does not support tag. */
    /* TODO(sergey): Shall we raise some panic here? */
    return;
  }
  /* Some sanity checks before moving forward. */
  if (id_node == nullptr) {
    /* Happens when object is tagged for update and not yet in the
     * dependency graph (but will be after relations update). */
    return;
  }
  /* Tag ID recalc flag. */
  DepsNodeFactory *factory = type_get_factory(component_type);
  BLI_assert(factory != nullptr);
  id_node->id_cow->recalc |= factory->id_recalc_tag();
  /* Tag corresponding dependency graph operation for update. */
  if (component_type == NodeType::ID_REF) {
    id_node->tag_update(graph, update_source);
  }
  else {
    depsgraph_tag_component(graph, id_node, component_type, operation_code, update_source);
  }
  /* TODO(sergey): Get rid of this once all areas are using proper data ID
   * for tagging. */
  deg_graph_id_tag_legacy_compat(bmain, graph, id, tag, update_source);
}

string stringify_append_bit(const string &str, IDRecalcFlag tag)
{
  const char *tag_name = DEG_update_tag_as_string(tag);
  if (tag_name == nullptr) {
    return str;
  }
  string result = str;
  if (!result.empty()) {
    result += ", ";
  }
  result += tag_name;
  return result;
}

string stringify_update_bitfield(unsigned int flags)
{
  if (flags == 0) {
    return "LEGACY_0";
  }
  string result;
  unsigned int current_flag = flags;
  /* Special cases to avoid ALL flags form being split into
   * individual bits. */
  if ((current_flag & ID_RECALC_PSYS_ALL) == ID_RECALC_PSYS_ALL) {
    result = stringify_append_bit(result, ID_RECALC_PSYS_ALL);
  }
  /* Handle all the rest of the flags. */
  while (current_flag != 0) {
    IDRecalcFlag tag = (IDRecalcFlag)(1 << bitscan_forward_clear_uint(&current_flag));
    result = stringify_append_bit(result, tag);
  }
  return result;
}

const char *update_source_as_string(eUpdateSource source)
{
  switch (source) {
    case DEG_UPDATE_SOURCE_TIME:
      return "TIME";
    case DEG_UPDATE_SOURCE_USER_EDIT:
      return "USER_EDIT";
    case DEG_UPDATE_SOURCE_RELATIONS:
      return "RELATIONS";
    case DEG_UPDATE_SOURCE_VISIBILITY:
      return "VISIBILITY";
  }
  BLI_assert_msg(0, "Should never happen.");
  return "UNKNOWN";
}

int deg_recalc_flags_for_legacy_zero()
{
  return ID_RECALC_ALL & ~(ID_RECALC_PSYS_ALL | ID_RECALC_ANIMATION | ID_RECALC_FRAME_CHANGE |
                           ID_RECALC_SOURCE | ID_RECALC_EDITORS);
}

int deg_recalc_flags_effective(Depsgraph *graph, unsigned int flags)
{
  if (graph != nullptr) {
    if (!graph->is_active) {
      return 0;
    }
  }
  if (flags == 0) {
    return deg_recalc_flags_for_legacy_zero();
  }
  return flags;
}

/* Special tag function which tags all components which needs to be tagged
 * for update flag=0.
 *
 * TODO(sergey): This is something to be avoid in the future, make it more
 * explicit and granular for users to tag what they really need. */
void deg_graph_node_tag_zero(Main *bmain,
                             Depsgraph *graph,
                             IDNode *id_node,
                             eUpdateSource update_source)
{
  if (id_node == nullptr) {
    return;
  }
  ID *id = id_node->id_orig;
  /* TODO(sergey): Which recalc flags to set here? */
  id_node->id_cow->recalc |= deg_recalc_flags_for_legacy_zero();

  for (ComponentNode *comp_node : id_node->components.values()) {
    if (comp_node->type == NodeType::ANIMATION) {
      continue;
    }
    if (comp_node->type == NodeType::COPY_ON_WRITE) {
      id_node->is_cow_explicitly_tagged = true;
    }

    comp_node->tag_update(graph, update_source);
  }
  deg_graph_id_tag_legacy_compat(bmain, graph, id, (IDRecalcFlag)0, update_source);
}

void graph_tag_on_visible_update(Depsgraph *graph, const bool do_time)
{
  graph->need_tag_id_on_graph_visibility_update = true;
  graph->need_tag_id_on_graph_visibility_time_update |= do_time;
}

} /* namespace */

void graph_tag_ids_for_visible_update(Depsgraph *graph)
{
  if (!graph->need_tag_id_on_graph_visibility_update) {
    return;
  }

  const bool do_time = graph->need_tag_id_on_graph_visibility_time_update;
  Main *bmain = graph->bmain;

  /* NOTE: It is possible to have this function called with `do_time=false` first and later (prior
   * to evaluation though) with `do_time=true`. This means early output checks should be aware of
   * this. */
  for (deg::IDNode *id_node : graph->id_nodes) {
    const ID_Type id_type = GS(id_node->id_orig->name);

    if (!id_node->visible_components_mask) {
      /* ID has no components which affects anything visible.
       * No need bother with it to tag or anything. */
      continue;
    }
    unsigned int flags = 0;
    if (!deg::deg_copy_on_write_is_expanded(id_node->id_cow)) {
      flags |= ID_RECALC_COPY_ON_WRITE;
      if (do_time) {
        if (BKE_animdata_from_id(id_node->id_orig) != nullptr) {
          flags |= ID_RECALC_ANIMATION;
        }
      }
    }
    else {
      if (id_node->visible_components_mask == id_node->previously_visible_components_mask) {
        /* The ID was already visible and evaluated, all the subsequent
         * updates and tags are to be done explicitly. */
        continue;
      }
    }
    /* We only tag components which needs an update. Tagging everything is
     * not a good idea because that might reset particles cache (or any
     * other type of cache).
     *
     * TODO(sergey): Need to generalize this somehow. */
    if (id_type == ID_OB) {
      flags |= ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY;
    }
    graph_id_tag_update(bmain, graph, id_node->id_orig, flags, DEG_UPDATE_SOURCE_VISIBILITY);
    if (id_type == ID_SCE) {
      /* Make sure collection properties are up to date. */
      id_node->tag_update(graph, DEG_UPDATE_SOURCE_VISIBILITY);
    }
    /* Now when ID is updated to the new visibility state, prevent it from
     * being re-tagged again. Simplest way to do so is to pretend that it
     * was already updated by the "previous" dependency graph.
     *
     * NOTE: Even if the on_visible_update() is called from the state when
     * dependency graph is tagged for relations update, it will be fine:
     * since dependency graph builder re-schedules entry tags, all the
     * tags we request from here will be applied in the updated state of
     * dependency graph. */
    id_node->previously_visible_components_mask = id_node->visible_components_mask;
  }

  graph->need_tag_id_on_graph_visibility_update = false;
  graph->need_tag_id_on_graph_visibility_time_update = false;
}

NodeType geometry_tag_to_component(const ID *id)
{
  const ID_Type id_type = GS(id->name);
  switch (id_type) {
    case ID_OB: {
      const Object *object = (Object *)id;
      switch (object->type) {
        case OB_MESH:
        case OB_CURVES_LEGACY:
        case OB_SURF:
        case OB_FONT:
        case OB_LATTICE:
        case OB_MBALL:
        case OB_GPENCIL:
        case OB_CURVES:
        case OB_POINTCLOUD:
        case OB_VOLUME:
          return NodeType::GEOMETRY;
        case OB_ARMATURE:
          return NodeType::EVAL_POSE;
          /* TODO(sergey): More cases here? */
      }
      break;
    }
    case ID_ME:
    case ID_CU_LEGACY:
    case ID_LT:
    case ID_MB:
    case ID_CV:
    case ID_PT:
    case ID_VO:
    case ID_GR:
      return NodeType::GEOMETRY;
    case ID_PA: /* Particles */
      return NodeType::UNDEFINED;
    case ID_LP:
      return NodeType::PARAMETERS;
    case ID_GD:
      return NodeType::GEOMETRY;
    case ID_PAL: /* Palettes */
      return NodeType::PARAMETERS;
    case ID_MSK:
      return NodeType::PARAMETERS;
    default:
      break;
  }
  return NodeType::UNDEFINED;
}

void id_tag_update(Main *bmain, ID *id, unsigned int flags, eUpdateSource update_source)
{
  graph_id_tag_update(bmain, nullptr, id, flags, update_source);
  for (deg::Depsgraph *depsgraph : deg::get_all_registered_graphs(bmain)) {
    graph_id_tag_update(bmain, depsgraph, id, flags, update_source);
  }

  /* Accumulate all tags for an ID between two undo steps, so they can be
   * replayed for undo. */
  id->recalc_after_undo_push |= deg_recalc_flags_effective(nullptr, flags);
}

void graph_id_tag_update(
    Main *bmain, Depsgraph *graph, ID *id, unsigned int flags, eUpdateSource update_source)
{
  const int debug_flags = (graph != nullptr) ? DEG_debug_flags_get((::Depsgraph *)graph) : G.debug;
  if (graph != nullptr && graph->is_evaluating) {
    if (debug_flags & G_DEBUG_DEPSGRAPH_TAG) {
      printf("ID tagged for update during dependency graph evaluation.\n");
    }
    return;
  }
  if (debug_flags & G_DEBUG_DEPSGRAPH_TAG) {
    printf("%s: id=%s flags=%s source=%s\n",
           __func__,
           id->name,
           stringify_update_bitfield(flags).c_str(),
           update_source_as_string(update_source));
  }
  IDNode *id_node = (graph != nullptr) ? graph->find_id_node(id) : nullptr;
  if (graph != nullptr) {
    DEG_graph_id_type_tag(reinterpret_cast<::Depsgraph *>(graph), GS(id->name));
  }
  if (flags == 0) {
    deg_graph_node_tag_zero(bmain, graph, id_node, update_source);
  }
  /* Store original flag in the ID.
   * Allows to have more granularity than a node-factory based flags. */
  if (id_node != nullptr) {
    id_node->id_cow->recalc |= flags;
  }
  /* When ID is tagged for update based on an user edits store the recalc flags in the original ID.
   * This way IDs in the undo steps will have this flag preserved, making it possible to restore
   * all needed tags when new dependency graph is created on redo.
   * This is the only way to ensure modifications to animation data (such as keyframes i.e.)
   * properly triggers animation update for the newly constructed dependency graph on redo (while
   * usually newly created dependency graph skips animation update to avoid loss of unkeyed
   * changes). */
  if (update_source == DEG_UPDATE_SOURCE_USER_EDIT) {
    id->recalc |= deg_recalc_flags_effective(graph, flags);
  }
  unsigned int current_flag = flags;
  while (current_flag != 0) {
    IDRecalcFlag tag = (IDRecalcFlag)(1 << bitscan_forward_clear_uint(&current_flag));
    graph_id_tag_update_single_flag(bmain, graph, id, id_node, tag, update_source);
  }
  /* Special case for nested node tree data-blocks. */
  id_tag_update_ntree_special(bmain, graph, id, flags, update_source);
  /* Direct update tags means that something outside of simulated/cached
   * physics did change and that cache is to be invalidated.
   * This is only needed if data changes. If it's just a drawing, we keep the
   * point cache. */
  if (update_source == DEG_UPDATE_SOURCE_USER_EDIT && flags != ID_RECALC_SHADING) {
    graph_id_tag_update_single_flag(
        bmain, graph, id, id_node, ID_RECALC_POINT_CACHE, update_source);
  }
}

}  // namespace blender::deg

const char *DEG_update_tag_as_string(IDRecalcFlag flag)
{
  switch (flag) {
    case ID_RECALC_TRANSFORM:
      return "TRANSFORM";
    case ID_RECALC_GEOMETRY:
      return "GEOMETRY";
    case ID_RECALC_GEOMETRY_ALL_MODES:
      return "GEOMETRY_ALL_MODES";
    case ID_RECALC_ANIMATION:
      return "ANIMATION";
    case ID_RECALC_PSYS_REDO:
      return "PSYS_REDO";
    case ID_RECALC_PSYS_RESET:
      return "PSYS_RESET";
    case ID_RECALC_PSYS_CHILD:
      return "PSYS_CHILD";
    case ID_RECALC_PSYS_PHYS:
      return "PSYS_PHYS";
    case ID_RECALC_PSYS_ALL:
      return "PSYS_ALL";
    case ID_RECALC_COPY_ON_WRITE:
      return "COPY_ON_WRITE";
    case ID_RECALC_SHADING:
      return "SHADING";
    case ID_RECALC_SELECT:
      return "SELECT";
    case ID_RECALC_BASE_FLAGS:
      return "BASE_FLAGS";
    case ID_RECALC_POINT_CACHE:
      return "POINT_CACHE";
    case ID_RECALC_EDITORS:
      return "EDITORS";
    case ID_RECALC_SEQUENCER_STRIPS:
      return "SEQUENCER_STRIPS";
    case ID_RECALC_FRAME_CHANGE:
      return "FRAME_CHANGE";
    case ID_RECALC_AUDIO_FPS:
      return "AUDIO_FPS";
    case ID_RECALC_AUDIO_VOLUME:
      return "AUDIO_VOLUME";
    case ID_RECALC_AUDIO_MUTE:
      return "AUDIO_MUTE";
    case ID_RECALC_AUDIO_LISTENER:
      return "AUDIO_LISTENER";
    case ID_RECALC_AUDIO:
      return "AUDIO";
    case ID_RECALC_PARAMETERS:
      return "PARAMETERS";
    case ID_RECALC_SOURCE:
      return "SOURCE";
    case ID_RECALC_ALL:
      return "ALL";
    case ID_RECALC_TAG_FOR_UNDO:
      return "TAG_FOR_UNDO";
    case ID_RECALC_NTREE_OUTPUT:
      return "ID_RECALC_NTREE_OUTPUT";

    case ID_RECALC_PROVISION_26:
    case ID_RECALC_PROVISION_27:
    case ID_RECALC_PROVISION_28:
    case ID_RECALC_PROVISION_29:
    case ID_RECALC_PROVISION_30:
    case ID_RECALC_PROVISION_31:
      /* Silently return nullptr, indicating that there is no string representation.
       *
       * This is needed due to the way how logging for ID_RECALC_ALL works: it iterates over all
       * bits and converts then to string. */
      return nullptr;
  }
  return nullptr;
}

/* Data-Based Tagging. */

void DEG_id_tag_update(ID *id, unsigned int flags)
{
  DEG_id_tag_update_ex(G.main, id, flags);
}

void DEG_id_tag_update_ex(Main *bmain, ID *id, unsigned int flags)
{
  if (id == nullptr) {
    /* Ideally should not happen, but old depsgraph allowed this. */
    return;
  }
  deg::id_tag_update(bmain, id, flags, deg::DEG_UPDATE_SOURCE_USER_EDIT);
}

void DEG_graph_id_tag_update(struct Main *bmain,
                             struct Depsgraph *depsgraph,
                             struct ID *id,
                             unsigned int flags)
{
  deg::Depsgraph *graph = (deg::Depsgraph *)depsgraph;
  deg::graph_id_tag_update(bmain, graph, id, flags, deg::DEG_UPDATE_SOURCE_USER_EDIT);
}

void DEG_time_tag_update(struct Main *bmain)
{
  for (deg::Depsgraph *depsgraph : deg::get_all_registered_graphs(bmain)) {
    DEG_graph_time_tag_update(reinterpret_cast<::Depsgraph *>(depsgraph));
  }
}

void DEG_graph_time_tag_update(struct Depsgraph *depsgraph)
{
  deg::Depsgraph *deg_graph = reinterpret_cast<deg::Depsgraph *>(depsgraph);
  deg_graph->tag_time_source();
}

void DEG_graph_id_type_tag(Depsgraph *depsgraph, short id_type)
{
  if (id_type == ID_NT) {
    /* Stupid workaround so parent data-blocks of nested node-tree get looped
     * over when we loop over tagged data-block types. */
    DEG_graph_id_type_tag(depsgraph, ID_MA);
    DEG_graph_id_type_tag(depsgraph, ID_TE);
    DEG_graph_id_type_tag(depsgraph, ID_LA);
    DEG_graph_id_type_tag(depsgraph, ID_WO);
    DEG_graph_id_type_tag(depsgraph, ID_SCE);
    DEG_graph_id_type_tag(depsgraph, ID_SIM);
  }
  const int id_type_index = BKE_idtype_idcode_to_index(id_type);
  deg::Depsgraph *deg_graph = reinterpret_cast<deg::Depsgraph *>(depsgraph);
  deg_graph->id_type_updated[id_type_index] = 1;
}

void DEG_id_type_tag(Main *bmain, short id_type)
{
  for (deg::Depsgraph *depsgraph : deg::get_all_registered_graphs(bmain)) {
    DEG_graph_id_type_tag(reinterpret_cast<::Depsgraph *>(depsgraph), id_type);
  }
}

void DEG_graph_tag_on_visible_update(Depsgraph *depsgraph, const bool do_time)
{
  deg::Depsgraph *graph = (deg::Depsgraph *)depsgraph;
  deg::graph_tag_on_visible_update(graph, do_time);
}

void DEG_tag_on_visible_update(Main *bmain, const bool do_time)
{
  for (deg::Depsgraph *depsgraph : deg::get_all_registered_graphs(bmain)) {
    deg::graph_tag_on_visible_update(depsgraph, do_time);
  }
}

void DEG_enable_editors_update(Depsgraph *depsgraph)
{
  deg::Depsgraph *graph = (deg::Depsgraph *)depsgraph;
  graph->use_editors_update = true;
}

void DEG_editors_update(Depsgraph *depsgraph, bool time)
{
  deg::Depsgraph *graph = (deg::Depsgraph *)depsgraph;
  if (!graph->use_editors_update) {
    return;
  }

  Scene *scene = DEG_get_input_scene(depsgraph);
  ViewLayer *view_layer = DEG_get_input_view_layer(depsgraph);
  Main *bmain = DEG_get_bmain(depsgraph);
  bool updated = time || DEG_id_type_any_updated(depsgraph);

  DEGEditorUpdateContext update_ctx = {nullptr};
  update_ctx.bmain = bmain;
  update_ctx.depsgraph = depsgraph;
  update_ctx.scene = scene;
  update_ctx.view_layer = view_layer;
  deg::deg_editors_scene_update(&update_ctx, updated);
}

static void deg_graph_clear_id_recalc_flags(ID *id)
{
  id->recalc &= ~ID_RECALC_ALL;
  bNodeTree *ntree = ntreeFromID(id);
  /* Clear embedded node trees too. */
  if (ntree) {
    ntree->id.recalc &= ~ID_RECALC_ALL;
  }
  /* XXX And what about scene's master collection here? */
}

void DEG_ids_clear_recalc(Depsgraph *depsgraph, const bool backup)
{
  deg::Depsgraph *deg_graph = reinterpret_cast<deg::Depsgraph *>(depsgraph);
  /* TODO(sergey): Re-implement POST_UPDATE_HANDLER_WORKAROUND using entry_tags
   * and id_tags storage from the new dependency graph. */
  if (!DEG_id_type_any_updated(depsgraph)) {
    return;
  }
  /* Go over all ID nodes, clearing tags. */
  for (deg::IDNode *id_node : deg_graph->id_nodes) {
    if (backup) {
      id_node->id_cow_recalc_backup |= id_node->id_cow->recalc;
    }
    /* TODO: we clear original ID recalc flags here, but this may not work
     * correctly when there are multiple depsgraph with others still using
     * the recalc flag. */
    id_node->is_user_modified = false;
    id_node->is_cow_explicitly_tagged = false;
    deg_graph_clear_id_recalc_flags(id_node->id_cow);
    if (deg_graph->is_active) {
      deg_graph_clear_id_recalc_flags(id_node->id_orig);
    }
  }
  memset(deg_graph->id_type_updated, 0, sizeof(deg_graph->id_type_updated));
}

void DEG_ids_restore_recalc(Depsgraph *depsgraph)
{
  deg::Depsgraph *deg_graph = reinterpret_cast<deg::Depsgraph *>(depsgraph);

  for (deg::IDNode *id_node : deg_graph->id_nodes) {
    id_node->id_cow->recalc |= id_node->id_cow_recalc_backup;
    id_node->id_cow_recalc_backup = 0;
  }
}
