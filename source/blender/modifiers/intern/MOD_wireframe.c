/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <string.h>

#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_mesh.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "MOD_modifiertypes.h"
#include "MOD_ui_common.h"

#include "bmesh.h"
#include "tools/bmesh_wireframe.h"

static void initData(ModifierData *md)
{
  WireframeModifierData *wmd = (WireframeModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(wmd, modifier));

  MEMCPY_STRUCT_AFTER(wmd, DNA_struct_default_get(WireframeModifierData), modifier);
}

static void requiredDataMask(Object *UNUSED(ob),
                             ModifierData *md,
                             CustomData_MeshMasks *r_cddata_masks)
{
  WireframeModifierData *wmd = (WireframeModifierData *)md;

  /* ask for vertexgroups if we need them */
  if (wmd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static bool dependsOnNormals(ModifierData *UNUSED(md))
{
  return true;
}

static Mesh *WireframeModifier_do(WireframeModifierData *wmd, Object *ob, Mesh *mesh)
{
  Mesh *result;
  BMesh *bm;

  const int defgrp_index = BKE_id_defgroup_name_index(&mesh->id, wmd->defgrp_name);

  bm = BKE_mesh_to_bmesh_ex(mesh,
                            &(struct BMeshCreateParams){0},
                            &(struct BMeshFromMeshParams){
                                .calc_face_normal = true,
                                .calc_vert_normal = true,
                                .add_key_index = false,
                                .use_shapekey = false,
                                .active_shapekey = 0,
                                .cd_mask_extra = {.vmask = CD_MASK_ORIGINDEX,
                                                  .emask = CD_MASK_ORIGINDEX,
                                                  .pmask = CD_MASK_ORIGINDEX},
                            });

  BM_mesh_wireframe(bm,
                    wmd->offset,
                    wmd->offset_fac,
                    wmd->offset_fac_vg,
                    (wmd->flag & MOD_WIREFRAME_REPLACE) != 0,
                    (wmd->flag & MOD_WIREFRAME_BOUNDARY) != 0,
                    (wmd->flag & MOD_WIREFRAME_OFS_EVEN) != 0,
                    (wmd->flag & MOD_WIREFRAME_OFS_RELATIVE) != 0,
                    (wmd->flag & MOD_WIREFRAME_CREASE) != 0,
                    wmd->crease_weight,
                    defgrp_index,
                    (wmd->flag & MOD_WIREFRAME_INVERT_VGROUP) != 0,
                    wmd->mat_ofs,
                    MAX2(ob->totcol - 1, 0),
                    false);

  result = BKE_mesh_from_bmesh_for_eval_nomain(bm, NULL, mesh);
  BM_mesh_free(bm);

  return result;
}

static Mesh *modifyMesh(ModifierData *md, const struct ModifierEvalContext *ctx, struct Mesh *mesh)
{
  return WireframeModifier_do((WireframeModifierData *)md, ctx->object, mesh);
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *col, *row, *sub;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "thickness", 0, IFACE_("Thickness"), ICON_NONE);
  uiItemR(layout, ptr, "offset", 0, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "use_boundary", 0, IFACE_("Boundary"), ICON_NONE);
  uiItemR(col, ptr, "use_replace", 0, IFACE_("Replace Original"), ICON_NONE);

  col = uiLayoutColumnWithHeading(layout, true, IFACE_("Thickness"));
  uiItemR(col, ptr, "use_even_offset", 0, IFACE_("Even"), ICON_NONE);
  uiItemR(col, ptr, "use_relative_offset", 0, IFACE_("Relative"), ICON_NONE);

  row = uiLayoutRowWithHeading(layout, true, IFACE_("Crease Edges"));
  uiItemR(row, ptr, "use_crease", 0, "", ICON_NONE);
  sub = uiLayoutRow(row, true);
  uiLayoutSetActive(sub, RNA_boolean_get(ptr, "use_crease"));
  uiItemR(sub, ptr, "crease_weight", UI_ITEM_R_SLIDER, "", ICON_NONE);

  uiItemR(layout, ptr, "material_offset", 0, IFACE_("Material Offset"), ICON_NONE);

  modifier_panel_end(layout, ptr);
}

static void vertex_group_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *row;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  bool has_vertex_group = RNA_string_length(ptr, "vertex_group") != 0;

  uiLayoutSetPropSep(layout, true);

  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", NULL);

  row = uiLayoutRow(layout, true);
  uiLayoutSetActive(row, has_vertex_group);
  uiItemR(row, ptr, "thickness_vertex_group", 0, IFACE_("Factor"), ICON_NONE);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(
      region_type, eModifierType_Wireframe, panel_draw);
  modifier_subpanel_register(
      region_type, "vertex_group", "Vertex Group", NULL, vertex_group_panel_draw, panel_type);
}

ModifierTypeInfo modifierType_Wireframe = {
    /* name */ N_("Wireframe"),
    /* structName */ "WireframeModifierData",
    /* structSize */ sizeof(WireframeModifierData),
    /* srna */ &RNA_WireframeModifier,
    /* type */ eModifierTypeType_Constructive,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /* icon */ ICON_MOD_WIREFRAME,

    /* copyData */ BKE_modifier_copydata_generic,

    /* deformVerts */ NULL,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ modifyMesh,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ requiredDataMask,
    /* freeData */ NULL,
    /* isDisabled */ NULL,
    /* updateDepgraph */ NULL,
    /* dependsOnTime */ NULL,
    /* dependsOnNormals */ dependsOnNormals,
    /* foreachIDLink */ NULL,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ NULL,
    /* blendRead */ NULL,
};
