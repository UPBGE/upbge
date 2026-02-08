/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Minimal skeleton for a GPU brush-like modifier (OnlyDeform) to be
 * extended later. This file follows the pattern used by other only-deform
 * modifiers (e.g. MOD_cast.cc) and keeps C++ logic minimal.
 */

#include "BLT_translation.hh"

#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_modifier.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "MOD_ui_common.hh"

/* DNA header for GPU dynamic paint types. Use the include name generated in makesdna. */
#include "DNA_dynamicpaint2_gpu_types.h"

namespace blender {

static void init_data(ModifierData *md)
{
  /* Minimal init: zero the structure and set defaults via RNA if needed. */
  DynamicPaint2GpuModifierData *pmd = reinterpret_cast<DynamicPaint2GpuModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(pmd, modifier);
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  /* Disable by default if strength/falloff are zero. Keep simple for now. */
  /* For now always enabled; logic will be added later when properties exist. */
  return false;
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *r_cddata_masks)
{
  /* This modifier will likely use vertex groups; leave hook for later. */
  (void)r_cddata_masks;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  /* No ID links yet. Keep API compatible. */
  (void)md;
  (void)ob;
  (void)walk;
  (void)user_data;
}

static void update_depsgraph(ModifierData * /*md*/, const ModifierUpdateDepsgraphContext * /*ctx*/)
{
  /* Nothing yet */
}

static void deform_verts(ModifierData *md,
                         const ModifierEvalContext * /*ctx*/,
                         Mesh * /*mesh*/,
                         MutableSpan<float3> positions)
{
  /* Minimal behavior: no deformation. Implement GPU dispatch later. */
  (void)md;
  (void)positions;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout *layout = panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout->use_property_split_set(true);

  /* Placeholder UI. Real UI will expose brushes collection etc. */
  layout->label("GPU Brush modifier (skeleton)", ICON_NONE);
  modifier_error_message_draw(*layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_DynamicPaint2Gpu, panel_draw);
}

ModifierTypeInfo modifierType_DynamicPaint2Gpu = {
    /*idname*/ "DynamicPaint2Gpu",
    /*name*/ N_("Dynamic Paint GPU"),
    /*struct_name*/ "DynamicPaint2GpuModifierData",
    /*struct_size*/ sizeof(DynamicPaint2GpuModifierData),
    /*srna*/ nullptr,
    /*type*/ ModifierTypeType::OnlyDeform,
    /*flags*/ static_cast<ModifierTypeFlag>(0),
    /*icon*/ ICON_MOD_CAST,

    /*copy_data*/ nullptr,

    /*deform_verts*/ deform_verts,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ nullptr,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

} // namespace blender
