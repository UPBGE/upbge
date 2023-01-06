/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2018 Blender Foundation. */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "MOD_gpencil_modifiertypes.h"
#include "MOD_gpencil_ui_common.h"
#include "MOD_gpencil_util.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_api.h"

#include "DEG_depsgraph.h"

static void initData(GpencilModifierData *md)
{
  TimeGpencilModifierData *gpmd = (TimeGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(TimeGpencilModifierData), modifier);
  TimeGpencilModifierSegment *ds = DNA_struct_default_alloc(TimeGpencilModifierSegment);
  ds->gpmd = gpmd;
  BLI_strncpy(ds->name, DATA_("Segment"), sizeof(ds->name));

  gpmd->segments = ds;
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  TimeGpencilModifierData *gpmd = (TimeGpencilModifierData *)target;
  const TimeGpencilModifierData *gpmd_src = (const TimeGpencilModifierData *)md;
  BKE_gpencil_modifier_copydata_generic(md, target);
  gpmd->segments = MEM_dupallocN(gpmd_src->segments);
}

static void freeData(GpencilModifierData *md)
{
  TimeGpencilModifierData *gpmd = (TimeGpencilModifierData *)md;

  MEM_SAFE_FREE(gpmd->segments);
}

static int remapTime(struct GpencilModifierData *md,
                     struct Depsgraph *UNUSED(depsgraph),
                     struct Scene *scene,
                     struct Object *UNUSED(ob),
                     struct bGPDlayer *gpl,
                     int cfra)
{
  TimeGpencilModifierData *mmd = (TimeGpencilModifierData *)md;
  const bool custom = mmd->flag & GP_TIME_CUSTOM_RANGE;
  const bool invgpl = mmd->flag & GP_TIME_INVERT_LAYER;
  const bool invpass = mmd->flag & GP_TIME_INVERT_LAYERPASS;
  int sfra = custom ? mmd->sfra : scene->r.sfra;
  int efra = custom ? mmd->efra : scene->r.efra;
  int offset = mmd->offset;
  int nfra = 0;

  CLAMP_MIN(sfra, 0);
  CLAMP_MIN(efra, 0);

  if (offset < 0) {
    offset = abs(efra - sfra + offset + 1);
  }
  /* Avoid inverse ranges. */
  if (efra <= sfra) {
    return cfra;
  }

  /* omit if filter by layer */
  if (mmd->layername[0] != '\0') {
    if (invgpl == false) {
      if (!STREQ(mmd->layername, gpl->info)) {
        return cfra;
      }
    }
    else {
      if (STREQ(mmd->layername, gpl->info)) {
        return cfra;
      }
    }
  }
  /* verify pass */
  if (mmd->layer_pass > 0) {
    if (invpass == false) {
      if (gpl->pass_index != mmd->layer_pass) {
        return cfra;
      }
    }
    else {
      if (gpl->pass_index == mmd->layer_pass) {
        return cfra;
      }
    }
  }

  /* apply frame scale */
  cfra *= mmd->frame_scale;
  CLAMP_MIN(cfra, 1);

  /* if fix mode, return predefined frame number */
  if (mmd->mode == GP_TIME_MODE_FIX) {
    return offset;
  }

  if (mmd->mode == GP_TIME_MODE_NORMAL) {
    if ((mmd->flag & GP_TIME_KEEP_LOOP) == 0) {
      nfra = cfra + sfra + offset - 1 < efra ? cfra + sfra + offset - 1 : efra;
    }
    else {
      nfra = (offset + cfra - 1) % (efra - sfra + 1) + sfra;
    }
  }
  if (mmd->mode == GP_TIME_MODE_REVERSE) {
    if ((mmd->flag & GP_TIME_KEEP_LOOP) == 0) {
      nfra = efra - cfra - offset > sfra ? efra - cfra - offset + 1 : sfra;
    }
    else {
      nfra = (efra + 1 - (cfra + offset - 1) % (efra - sfra + 1)) - 1;
    }
  }

  if (mmd->mode == GP_TIME_MODE_PINGPONG) {
    if ((mmd->flag & GP_TIME_KEEP_LOOP) == 0) {
      if (((int)(cfra + offset - 1) / (efra - sfra)) % (2)) {
        nfra = efra - (cfra + offset - 1) % (efra - sfra);
      }
      else {
        nfra = sfra + (cfra + offset - 1) % (efra - sfra);
      }
      if (cfra > (efra - sfra) * 2) {
        nfra = sfra + offset;
      }
    }
    else {

      if (((int)(cfra + offset - 1) / (efra - sfra)) % (2)) {
        nfra = efra - (cfra + offset - 1) % (efra - sfra);
      }
      else {
        nfra = sfra + (cfra + offset - 1) % (efra - sfra);
      }
    }
  }

  if (mmd->mode == GP_TIME_MODE_CHAIN) {
    int sequence_length = 0;
    int frame_key = 0;
    int *segment_arr;
    int start, end;
    if (mmd->segments_len > 0) {
      for (int i = 0; i < mmd->segments_len; i++) {
        start = mmd->segments[i].seg_start;
        end = mmd->segments[i].seg_end;
        if (mmd->segments[i].seg_end < mmd->segments[i].seg_start) {
          start = mmd->segments[i].seg_end;
          end = mmd->segments[i].seg_start;
        }

        if (ELEM(mmd->segments[i].seg_mode, GP_TIME_SEG_MODE_PINGPONG)) {
          sequence_length += ((end - start) * mmd->segments[i].seg_repeat) * 2 + 1;
        }
        else {
          sequence_length += ((end - start + 1) * mmd->segments[i].seg_repeat);
        }
      }
      segment_arr = MEM_malloc_arrayN(sequence_length, sizeof(int *), __func__);

      for (int i = 0; i < mmd->segments_len; i++) {

        if (mmd->segments[i].seg_end < mmd->segments[i].seg_start) {
          start = mmd->segments[i].seg_end;
          end = mmd->segments[i].seg_start;
        }
        else {
          start = mmd->segments[i].seg_start;
          end = mmd->segments[i].seg_end;
        }
        for (int a = 0; a < mmd->segments[i].seg_repeat; a++) {
          switch (mmd->segments[i].seg_mode) {
            case GP_TIME_SEG_MODE_NORMAL:
              for (int b = 0; b < end - start + 1; b++) {
                segment_arr[frame_key] = start + b;
                frame_key++;
              }
              break;
            case GP_TIME_SEG_MODE_REVERSE:
              for (int b = 0; b < end - start + 1; b++) {
                segment_arr[frame_key] = end - b;
                frame_key++;
              }
              break;
            case GP_TIME_SEG_MODE_PINGPONG:
              for (int b = 0; b < end - start; b++) {
                segment_arr[frame_key] = start + b;
                frame_key++;
              }
              for (int b = 0; b < end - start; b++) {
                segment_arr[frame_key] = end - b;
                frame_key++;
                if (a == mmd->segments[i].seg_repeat - 1 && b == end - start - 1) {
                  segment_arr[frame_key] = start;
                  frame_key++;
                }
              }
              break;
          }
        }
      }

      if ((mmd->flag & GP_TIME_KEEP_LOOP) == 0) {
        if ((cfra + offset - 1) < sequence_length) {
          nfra = segment_arr[(cfra - 1 + offset)];
        }
        else {
          nfra = segment_arr[frame_key - 1];
        }
      }
      else {
        nfra = segment_arr[(cfra - 1 + offset) % sequence_length];
      }

      MEM_freeN(segment_arr);
    }
  }

  return nfra;
}

static void segment_list_item(struct uiList *UNUSED(ui_list),
                              const struct bContext *UNUSED(C),
                              struct uiLayout *layout,
                              struct PointerRNA *UNUSED(idataptr),
                              struct PointerRNA *itemptr,
                              int UNUSED(icon),
                              struct PointerRNA *UNUSED(active_dataptr),
                              const char *UNUSED(active_propname),
                              int UNUSED(index),
                              int UNUSED(flt_flag))
{
  uiLayout *row = uiLayoutRow(layout, true);
  uiItemR(row, itemptr, "name", UI_ITEM_R_NO_BG, "", ICON_NONE);
}
static void foreachIDLink(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  TimeGpencilModifierData *mmd = (TimeGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
}
static void panel_draw(const bContext *C, Panel *panel)
{
  uiLayout *row, *col;
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  int mode = RNA_enum_get(ptr, "mode");

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "mode", 0, NULL, ICON_NONE);

  col = uiLayoutColumn(layout, false);

  const char *text = (mode == GP_TIME_MODE_FIX) ? IFACE_("Frame") : IFACE_("Frame Offset");
  uiItemR(col, ptr, "offset", 0, text, ICON_NONE);

  row = uiLayoutRow(col, false);
  uiLayoutSetActive(row, mode != GP_TIME_MODE_FIX);
  uiItemR(row, ptr, "frame_scale", 0, IFACE_("Scale"), ICON_NONE);

  row = uiLayoutRow(layout, false);
  uiLayoutSetActive(row, mode != GP_TIME_MODE_FIX);
  uiItemR(row, ptr, "use_keep_loop", 0, NULL, ICON_NONE);

  if (mode == GP_TIME_MODE_CHAIN) {

    row = uiLayoutRow(layout, false);
    uiLayoutSetPropSep(row, false);

    uiTemplateList(row,
                   (bContext *)C,
                   "MOD_UL_time_segment",
                   "",
                   ptr,
                   "segments",
                   ptr,
                   "segment_active_index",
                   NULL,
                   3,
                   10,
                   0,
                   1,
                   UI_TEMPLATE_LIST_FLAG_NONE);

    col = uiLayoutColumn(row, false);
    uiLayoutSetContextPointer(col, "modifier", ptr);

    uiLayout *sub = uiLayoutColumn(col, true);
    uiItemO(sub, "", ICON_ADD, "GPENCIL_OT_time_segment_add");
    uiItemO(sub, "", ICON_REMOVE, "GPENCIL_OT_time_segment_remove");
    uiItemS(col);
    sub = uiLayoutColumn(col, true);
    uiItemEnumO_string(sub, "", ICON_TRIA_UP, "GPENCIL_OT_time_segment_move", "type", "UP");
    uiItemEnumO_string(sub, "", ICON_TRIA_DOWN, "GPENCIL_OT_time_segment_move", "type", "DOWN");

    TimeGpencilModifierData *gpmd = ptr->data;
    if (gpmd->segment_active_index >= 0 && gpmd->segment_active_index < gpmd->segments_len) {
      PointerRNA ds_ptr;
      RNA_pointer_create(ptr->owner_id,
                         &RNA_TimeGpencilModifierSegment,
                         &gpmd->segments[gpmd->segment_active_index],
                         &ds_ptr);

      sub = uiLayoutColumn(layout, true);
      uiItemR(sub, &ds_ptr, "seg_mode", 0, NULL, ICON_NONE);
      sub = uiLayoutColumn(layout, true);
      uiItemR(sub, &ds_ptr, "seg_start", 0, NULL, ICON_NONE);
      uiItemR(sub, &ds_ptr, "seg_end", 0, NULL, ICON_NONE);
      uiItemR(sub, &ds_ptr, "seg_repeat", 0, NULL, ICON_NONE);
    }

    gpencil_modifier_panel_end(layout, ptr);
  }

  gpencil_modifier_panel_end(layout, ptr);
}

static void custom_range_header_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  int mode = RNA_enum_get(ptr, "mode");

  uiLayoutSetActive(layout, !ELEM(mode, GP_TIME_MODE_FIX, GP_TIME_MODE_CHAIN));

  uiItemR(layout, ptr, "use_custom_frame_range", 0, NULL, ICON_NONE);
}

static void custom_range_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  int mode = RNA_enum_get(ptr, "mode");

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetActive(layout,
                    (!ELEM(mode, GP_TIME_MODE_FIX, GP_TIME_MODE_CHAIN)) &&
                        RNA_boolean_get(ptr, "use_custom_frame_range"));

  col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "frame_start", 0, IFACE_("Frame Start"), ICON_NONE);
  uiItemR(col, ptr, "frame_end", 0, IFACE_("End"), ICON_NONE);
}

static void mask_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, false, false);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_Time, panel_draw);
  gpencil_modifier_subpanel_register(region_type,
                                     "custom_range",
                                     "",
                                     custom_range_header_draw,
                                     custom_range_panel_draw,
                                     panel_type);
  gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", NULL, mask_panel_draw, panel_type);

  uiListType *list_type = MEM_callocN(sizeof(uiListType), "time modifier segment uilist");
  strcpy(list_type->idname, "MOD_UL_time_segment");
  list_type->draw_item = segment_list_item;
  WM_uilisttype_add(list_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_Time = {
    /* name */ N_("TimeOffset"),
    /* structName */ "TimeGpencilModifierData",
    /* structSize */ sizeof(TimeGpencilModifierData),
    /* type */ eGpencilModifierTypeType_Gpencil,
    /* flags */ eGpencilModifierTypeFlag_NoApply,

    /* copyData */ copyData,

    /* deformStroke */ NULL,
    /* generateStrokes */ NULL,
    /* bakeModifier */ NULL,
    /* remapTime */ remapTime,

    /* initData */ initData,
    /* freeData */ freeData,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* panelRegister */ panelRegister,
};
