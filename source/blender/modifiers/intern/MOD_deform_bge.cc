/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <algorithm>

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"
#include "BLT_translation.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_defaults.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_modifier.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_ui_common.hh"
#include "MOD_util.hh"

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh * /*mesh*/)
{
  SimpleDeformModifierDataBGE *smd = (SimpleDeformModifierDataBGE *)md;
  Object *object_eval = DEG_get_evaluated(ctx->depsgraph, ctx->object);
  Mesh *source = (Mesh *)object_eval->data;
  Mesh *result = BKE_mesh_copy_for_eval(*source);

  float(*positions)[3] = reinterpret_cast<float(*)[3]>(
      result->vert_positions_for_write().data());

  for (int i = 0; i < result->vert_positions().size(); i++) {
    copy_v3_v3(positions[i], smd->vertcoos[i]);
  }
  result->tag_positions_changed();

  return result;
}

/* SimpleDeform */
static void init_data(ModifierData *md)
{
  SimpleDeformModifierDataBGE *smd = (SimpleDeformModifierDataBGE *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(smd, modifier));

  MEMCPY_STRUCT_AFTER(smd, DNA_struct_default_get(SimpleDeformModifierDataBGE), modifier);
}

ModifierTypeInfo modifierType_SimpleDeformBGE = {
    /*idname*/ "SimpleDeformBGE",
    /*name*/ N_("SimpleDeformBGE"),
    /*struct_name*/ "SimpleDeformModifierDataBGE",
    /*struct_size*/ sizeof(SimpleDeformModifierDataBGE),
    /*srna*/ nullptr,
    /*type*/ ModifierTypeType::Constructive,

    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping | eModifierTypeFlag_Single,
    /*icon*/ ICON_MOD_SIMPLEDEFORM,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ nullptr,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
};
