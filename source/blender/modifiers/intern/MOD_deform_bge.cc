/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "BLI_math_vector.h"

#include "BLT_translation.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_object_types.h"

#include "BKE_mesh.hh"

#include "UI_interface.hh"

#include "MOD_util.hh"

namespace blender {

static void deform_verts(ModifierData *md,
                         const ModifierEvalContext * /*ctx*/,
                         Mesh * /*mesh*/,
                         blender::MutableSpan<blender::float3> positions)
{
  SimpleDeformModifierDataBGE *smd = (SimpleDeformModifierDataBGE *)md;
  if (!smd->vertcoos) {
    printf("SimpleDeformBGE: vertcoos is nullptr !\n");
    return;
  }
  if (positions.size() > 0 && !smd->vertcoos) {
    printf("SimpleDeformBGE: vertcoos is nullptr whereas positions.size()=%d\n",
           int(positions.size()));
    return;
  }
  blender::threading::parallel_for(
      blender::IndexRange(positions.size()), 4096, [&](const blender::IndexRange range) {
        for (int i : range) {
          copy_v3_v3(positions[i], smd->vertcoos[i]);
        }
      });
}

/* SimpleDeform */
static void init_data(ModifierData *md)
{
  SimpleDeformModifierDataBGE *smd = (SimpleDeformModifierDataBGE *)md;
  INIT_DEFAULT_STRUCT_AFTER(smd, modifier);
}

ModifierTypeInfo modifierType_SimpleDeformBGE = {
    /*idname*/ "SimpleDeformBGE",
    /*name*/ N_("SimpleDeformBGE"),
    /*struct_name*/ "SimpleDeformModifierDataBGE",
    /*struct_size*/ sizeof(SimpleDeformModifierDataBGE),
    /*srna*/ nullptr,
    /*type*/ ModifierTypeType::OnlyDeform,

    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_Single,
    /*icon*/ ICON_MOD_SIMPLEDEFORM,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ deform_verts,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
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

}  // namespace blender
