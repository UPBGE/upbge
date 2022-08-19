/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edobj
 */

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "MEM_guardedalloc.h"

#include "DNA_actuator_types.h"
#include "DNA_anim_types.h"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_curve_types.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_key_types.h"
#include "DNA_light_types.h"
#include "DNA_lightprobe_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meta_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_fluidsim_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"
#include "DNA_vfont_types.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.h"

#include "BKE_action.h"
#include "BKE_anim_data.h"
#include "BKE_armature.h"
#include "BKE_camera.h"
#include "BKE_collection.h"
#include "BKE_constraint.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_curves.h"
#include "BKE_displist.h"
#include "BKE_duplilist.h"
#include "BKE_effect.h"
#include "BKE_geometry_set.h"
#include "BKE_geometry_set.hh"
#include "BKE_gpencil_curve.h"
#include "BKE_gpencil_geom.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_key.h"
#include "BKE_lattice.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_lib_override.h"
#include "BKE_lib_query.h"
#include "BKE_lib_remap.h"
#include "BKE_light.h"
#include "BKE_lightprobe.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mball.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_nla.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_particle.h"
#include "BKE_pointcloud.h"
#include "BKE_report.h"
#include "BKE_sca.h"
#include "BKE_scene.h"
#include "BKE_speaker.h"
#include "BKE_vfont.h"
#include "BKE_volume.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "UI_interface.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_armature.h"
#include "ED_curve.h"
#include "ED_curves.h"
#include "ED_gpencil.h"
#include "ED_mball.h"
#include "ED_mesh.h"
#include "ED_node.h"
#include "ED_object.h"
#include "ED_outliner.h"
#include "ED_physics.h"
#include "ED_render.h"
#include "ED_screen.h"
#include "ED_select_utils.h"
#include "ED_transform.h"
#include "ED_view3d.h"

#include "UI_resources.h"

#include "object_intern.h"

using blender::float3;
using blender::float4x4;
using blender::Vector;

/* -------------------------------------------------------------------- */
/** \name Local Enum Declarations
 * \{ */

/* This is an exact copy of the define in `rna_light.c`
 * kept here because of linking order.
 * Icons are only defined here */
const EnumPropertyItem rna_enum_light_type_items[] = {
    {LA_LOCAL, "POINT", ICON_LIGHT_POINT, "Point", "Omnidirectional point light source"},
    {LA_SUN, "SUN", ICON_LIGHT_SUN, "Sun", "Constant direction parallel ray light source"},
    {LA_SPOT, "SPOT", ICON_LIGHT_SPOT, "Spot", "Directional cone light source"},
    {LA_AREA, "AREA", ICON_LIGHT_AREA, "Area", "Directional area light source"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* copy from rna_object_force.c */
static const EnumPropertyItem field_type_items[] = {
    {PFIELD_FORCE, "FORCE", ICON_FORCE_FORCE, "Force", ""},
    {PFIELD_WIND, "WIND", ICON_FORCE_WIND, "Wind", ""},
    {PFIELD_VORTEX, "VORTEX", ICON_FORCE_VORTEX, "Vortex", ""},
    {PFIELD_MAGNET, "MAGNET", ICON_FORCE_MAGNETIC, "Magnetic", ""},
    {PFIELD_HARMONIC, "HARMONIC", ICON_FORCE_HARMONIC, "Harmonic", ""},
    {PFIELD_CHARGE, "CHARGE", ICON_FORCE_CHARGE, "Charge", ""},
    {PFIELD_LENNARDJ, "LENNARDJ", ICON_FORCE_LENNARDJONES, "Lennard-Jones", ""},
    {PFIELD_TEXTURE, "TEXTURE", ICON_FORCE_TEXTURE, "Texture", ""},
    {PFIELD_GUIDE, "GUIDE", ICON_FORCE_CURVE, "Curve Guide", ""},
    {PFIELD_BOID, "BOID", ICON_FORCE_BOID, "Boid", ""},
    {PFIELD_TURBULENCE, "TURBULENCE", ICON_FORCE_TURBULENCE, "Turbulence", ""},
    {PFIELD_DRAG, "DRAG", ICON_FORCE_DRAG, "Drag", ""},
    {PFIELD_FLUIDFLOW, "FLUID", ICON_FORCE_FLUIDFLOW, "Fluid Flow", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static EnumPropertyItem lightprobe_type_items[] = {
    {LIGHTPROBE_TYPE_CUBE,
     "CUBEMAP",
     ICON_LIGHTPROBE_CUBEMAP,
     "Reflection Cubemap",
     "Reflection probe with spherical or cubic attenuation"},
    {LIGHTPROBE_TYPE_PLANAR,
     "PLANAR",
     ICON_LIGHTPROBE_PLANAR,
     "Reflection Plane",
     "Planar reflection probe"},
    {LIGHTPROBE_TYPE_GRID,
     "GRID",
     ICON_LIGHTPROBE_GRID,
     "Irradiance Volume",
     "Irradiance probe to capture diffuse indirect lighting"},
    {0, nullptr, 0, nullptr, nullptr},
};

enum {
  ALIGN_WORLD = 0,
  ALIGN_VIEW,
  ALIGN_CURSOR,
};

static const EnumPropertyItem align_options[] = {
    {ALIGN_WORLD, "WORLD", 0, "World", "Align the new object to the world"},
    {ALIGN_VIEW, "VIEW", 0, "View", "Align the new object to the view"},
    {ALIGN_CURSOR, "CURSOR", 0, "3D Cursor", "Use the 3D cursor orientation for the new object"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Helpers
 * \{ */

/**
 * Operator properties for creating an object under a screen space (2D) coordinate.
 * Used for object dropping like behavior (drag object and drop into 3D View).
 */
static void object_add_drop_xy_props(wmOperatorType *ot)
{
  PropertyRNA *prop;

  prop = RNA_def_int(ot->srna,
                     "drop_x",
                     0,
                     INT_MIN,
                     INT_MAX,
                     "Drop X",
                     "X-coordinate (screen space) to place the new object under",
                     INT_MIN,
                     INT_MAX);
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));
  prop = RNA_def_int(ot->srna,
                     "drop_y",
                     0,
                     INT_MIN,
                     INT_MAX,
                     "Drop Y",
                     "Y-coordinate (screen space) to place the new object under",
                     INT_MIN,
                     INT_MAX);
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));
}

static bool object_add_drop_xy_is_set(const wmOperator *op)
{
  return RNA_struct_property_is_set(op->ptr, "drop_x") &&
         RNA_struct_property_is_set(op->ptr, "drop_y");
}

/**
 * Query the currently set X- and Y-coordinate to position the new object under.
 * \param r_mval: Returned pointer to the coordinate in region-space.
 */
static bool object_add_drop_xy_get(bContext *C, wmOperator *op, int (*r_mval)[2])
{
  if (!object_add_drop_xy_is_set(op)) {
    (*r_mval)[0] = 0.0f;
    (*r_mval)[1] = 0.0f;
    return false;
  }

  const ARegion *region = CTX_wm_region(C);
  (*r_mval)[0] = RNA_int_get(op->ptr, "drop_x") - region->winrct.xmin;
  (*r_mval)[1] = RNA_int_get(op->ptr, "drop_y") - region->winrct.ymin;

  return true;
}

/**
 * Set the drop coordinate to the mouse position (if not already set) and call the operator's
 * `exec()` callback.
 */
static int object_add_drop_xy_generic_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!object_add_drop_xy_is_set(op)) {
    RNA_int_set(op->ptr, "drop_x", event->xy[0]);
    RNA_int_set(op->ptr, "drop_y", event->xy[1]);
  }
  return op->type->exec(C, op);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Add Object API
 * \{ */

void ED_object_location_from_view(bContext *C, float loc[3])
{
  const Scene *scene = CTX_data_scene(C);
  copy_v3_v3(loc, scene->cursor.location);
}

void ED_object_rotation_from_quat(float rot[3], const float viewquat[4], const char align_axis)
{
  BLI_assert(align_axis >= 'X' && align_axis <= 'Z');

  switch (align_axis) {
    case 'X': {
      /* Same as 'rv3d->viewinv[1]' */
      const float axis_y[4] = {0.0f, 1.0f, 0.0f};
      float quat_y[4], quat[4];
      axis_angle_to_quat(quat_y, axis_y, M_PI_2);
      mul_qt_qtqt(quat, viewquat, quat_y);
      quat_to_eul(rot, quat);
      break;
    }
    case 'Y': {
      quat_to_eul(rot, viewquat);
      rot[0] -= (float)M_PI_2;
      break;
    }
    case 'Z': {
      quat_to_eul(rot, viewquat);
      break;
    }
  }
}

void ED_object_rotation_from_view(bContext *C, float rot[3], const char align_axis)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  BLI_assert(align_axis >= 'X' && align_axis <= 'Z');
  if (rv3d) {
    float viewquat[4];
    copy_qt_qt(viewquat, rv3d->viewquat);
    viewquat[0] *= -1.0f;
    ED_object_rotation_from_quat(rot, viewquat, align_axis);
  }
  else {
    zero_v3(rot);
  }
}

void ED_object_base_init_transform_on_add(Object *object, const float loc[3], const float rot[3])
{
  if (loc) {
    copy_v3_v3(object->loc, loc);
  }

  if (rot) {
    copy_v3_v3(object->rot, rot);
  }

  BKE_object_to_mat4(object, object->obmat);
}

float ED_object_new_primitive_matrix(bContext *C,
                                     Object *obedit,
                                     const float loc[3],
                                     const float rot[3],
                                     const float scale[3],
                                     float r_primmat[4][4])
{
  Scene *scene = CTX_data_scene(C);
  View3D *v3d = CTX_wm_view3d(C);
  float mat[3][3], rmat[3][3], cmat[3][3], imat[3][3];

  unit_m4(r_primmat);

  eul_to_mat3(rmat, rot);
  invert_m3(rmat);

  /* inverse transform for initial rotation and object */
  copy_m3_m4(mat, obedit->obmat);
  mul_m3_m3m3(cmat, rmat, mat);
  invert_m3_m3(imat, cmat);
  copy_m4_m3(r_primmat, imat);

  /* center */
  copy_v3_v3(r_primmat[3], loc);
  sub_v3_v3v3(r_primmat[3], r_primmat[3], obedit->obmat[3]);
  invert_m3_m3(imat, mat);
  mul_m3_v3(imat, r_primmat[3]);

  if (scale != nullptr) {
    rescale_m4(r_primmat, scale);
  }

  {
    const float dia = v3d ? ED_view3d_grid_scale(scene, v3d, nullptr) :
                            ED_scene_grid_scale(scene, nullptr);
    return dia;
  }

  /* return 1.0f; */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Object Operator
 * \{ */

static void view_align_update(struct Main *UNUSED(main),
                              struct Scene *UNUSED(scene),
                              struct PointerRNA *ptr)
{
  RNA_struct_idprops_unset(ptr, "rotation");
}

void ED_object_add_unit_props_size(wmOperatorType *ot)
{
  RNA_def_float_distance(
      ot->srna, "size", 2.0f, 0.0, OBJECT_ADD_SIZE_MAXF, "Size", "", 0.001, 100.00);
}

void ED_object_add_unit_props_radius_ex(wmOperatorType *ot, float default_value)
{
  RNA_def_float_distance(
      ot->srna, "radius", default_value, 0.0, OBJECT_ADD_SIZE_MAXF, "Radius", "", 0.001, 100.00);
}

void ED_object_add_unit_props_radius(wmOperatorType *ot)
{
  ED_object_add_unit_props_radius_ex(ot, 1.0f);
}

void ED_object_add_generic_props(wmOperatorType *ot, bool do_editmode)
{
  PropertyRNA *prop;

  if (do_editmode) {
    prop = RNA_def_boolean(ot->srna,
                           "enter_editmode",
                           false,
                           "Enter Edit Mode",
                           "Enter edit mode when adding this object");
    RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));
  }
  /* NOTE: this property gets hidden for add-camera operator. */
  prop = RNA_def_enum(
      ot->srna, "align", align_options, ALIGN_WORLD, "Align", "The alignment of the new object");
  RNA_def_property_update_runtime(prop, (void *)view_align_update);

  prop = RNA_def_float_vector_xyz(ot->srna,
                                  "location",
                                  3,
                                  nullptr,
                                  -OBJECT_ADD_SIZE_MAXF,
                                  OBJECT_ADD_SIZE_MAXF,
                                  "Location",
                                  "Location for the newly added object",
                                  -1000.0f,
                                  1000.0f);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_float_rotation(ot->srna,
                                "rotation",
                                3,
                                nullptr,
                                -OBJECT_ADD_SIZE_MAXF,
                                OBJECT_ADD_SIZE_MAXF,
                                "Rotation",
                                "Rotation for the newly added object",
                                DEG2RADF(-360.0f),
                                DEG2RADF(360.0f));
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_float_vector_xyz(ot->srna,
                                  "scale",
                                  3,
                                  nullptr,
                                  -OBJECT_ADD_SIZE_MAXF,
                                  OBJECT_ADD_SIZE_MAXF,
                                  "Scale",
                                  "Scale for the newly added object",
                                  -1000.0f,
                                  1000.0f);
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));
}

void ED_object_add_mesh_props(wmOperatorType *ot)
{
  RNA_def_boolean(ot->srna, "calc_uvs", true, "Generate UVs", "Generate a default UV map");
}

bool ED_object_add_generic_get_opts(bContext *C,
                                    wmOperator *op,
                                    const char view_align_axis,
                                    float r_loc[3],
                                    float r_rot[3],
                                    float r_scale[3],
                                    bool *r_enter_editmode,
                                    ushort *r_local_view_bits,
                                    bool *r_is_view_aligned)
{
  /* Edit Mode! (optional) */
  {
    bool _enter_editmode;
    if (!r_enter_editmode) {
      r_enter_editmode = &_enter_editmode;
    }
    /* Only to ensure the value is _always_ set.
     * Typically the property will exist when the argument is non-nullptr. */
    *r_enter_editmode = false;

    PropertyRNA *prop = RNA_struct_find_property(op->ptr, "enter_editmode");
    if (prop != nullptr) {
      if (RNA_property_is_set(op->ptr, prop) && r_enter_editmode) {
        *r_enter_editmode = RNA_property_boolean_get(op->ptr, prop);
      }
      else {
        *r_enter_editmode = (U.flag & USER_ADD_EDITMODE) != 0;
        RNA_property_boolean_set(op->ptr, prop, *r_enter_editmode);
      }
    }
  }

  if (r_local_view_bits) {
    View3D *v3d = CTX_wm_view3d(C);
    *r_local_view_bits = (v3d && v3d->localvd) ? v3d->local_view_uuid : 0;
  }

  /* Location! */
  {
    float _loc[3];
    if (!r_loc) {
      r_loc = _loc;
    }

    if (RNA_struct_property_is_set(op->ptr, "location")) {
      RNA_float_get_array(op->ptr, "location", r_loc);
    }
    else {
      ED_object_location_from_view(C, r_loc);
      RNA_float_set_array(op->ptr, "location", r_loc);
    }
  }

  /* Rotation! */
  {
    bool _is_view_aligned;
    float _rot[3];
    if (!r_is_view_aligned) {
      r_is_view_aligned = &_is_view_aligned;
    }
    if (!r_rot) {
      r_rot = _rot;
    }

    if (RNA_struct_property_is_set(op->ptr, "rotation")) {
      /* If rotation is set, always use it. Alignment (and corresponding user preference)
       * can be ignored since this is in world space anyways.
       * To not confuse (e.g. on redo), don't set it to #ALIGN_WORLD in the op UI though. */
      *r_is_view_aligned = false;
      RNA_float_get_array(op->ptr, "rotation", r_rot);
    }
    else {
      int alignment = ALIGN_WORLD;
      PropertyRNA *prop = RNA_struct_find_property(op->ptr, "align");

      if (RNA_property_is_set(op->ptr, prop)) {
        /* If alignment is set, always use it. */
        *r_is_view_aligned = alignment == ALIGN_VIEW;
        alignment = RNA_property_enum_get(op->ptr, prop);
      }
      else {
        /* If alignment is not set, use User Preferences. */
        *r_is_view_aligned = (U.flag & USER_ADD_VIEWALIGNED) != 0;
        if (*r_is_view_aligned) {
          RNA_property_enum_set(op->ptr, prop, ALIGN_VIEW);
          alignment = ALIGN_VIEW;
        }
        else if ((U.flag & USER_ADD_CURSORALIGNED) != 0) {
          RNA_property_enum_set(op->ptr, prop, ALIGN_CURSOR);
          alignment = ALIGN_CURSOR;
        }
        else {
          RNA_property_enum_set(op->ptr, prop, ALIGN_WORLD);
          alignment = ALIGN_WORLD;
        }
      }
      switch (alignment) {
        case ALIGN_WORLD:
          RNA_float_get_array(op->ptr, "rotation", r_rot);
          break;
        case ALIGN_VIEW:
          ED_object_rotation_from_view(C, r_rot, view_align_axis);
          RNA_float_set_array(op->ptr, "rotation", r_rot);
          break;
        case ALIGN_CURSOR: {
          const Scene *scene = CTX_data_scene(C);
          float tmat[3][3];
          BKE_scene_cursor_rot_to_mat3(&scene->cursor, tmat);
          mat3_normalized_to_eul(r_rot, tmat);
          RNA_float_set_array(op->ptr, "rotation", r_rot);
          break;
        }
      }
    }
  }

  /* Scale! */
  {
    float _scale[3];
    if (!r_scale) {
      r_scale = _scale;
    }

    /* For now this is optional, we can make it always use. */
    copy_v3_fl(r_scale, 1.0f);

    PropertyRNA *prop = RNA_struct_find_property(op->ptr, "scale");
    if (prop != nullptr) {
      if (RNA_property_is_set(op->ptr, prop)) {
        RNA_property_float_get_array(op->ptr, prop, r_scale);
      }
      else {
        copy_v3_fl(r_scale, 1.0f);
        RNA_property_float_set_array(op->ptr, prop, r_scale);
      }
    }
  }

  return true;
}

Object *ED_object_add_type_with_obdata(bContext *C,
                                       const int type,
                                       const char *name,
                                       const float loc[3],
                                       const float rot[3],
                                       const bool enter_editmode,
                                       const ushort local_view_bits,
                                       ID *obdata)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  {
    Object *obedit = OBEDIT_FROM_VIEW_LAYER(view_layer);
    if (obedit != nullptr) {
      ED_object_editmode_exit_ex(bmain, scene, obedit, EM_FREEDATA);
    }
  }

  /* deselects all, sets active object */
  Object *ob;
  if (obdata != nullptr) {
    BLI_assert(type == BKE_object_obdata_to_type(obdata));
    ob = BKE_object_add_for_data(bmain, view_layer, type, name, obdata, true);
    const short *materials_len_p = BKE_id_material_len_p(obdata);
    if (materials_len_p && *materials_len_p > 0) {
      BKE_object_materials_test(bmain, ob, static_cast<ID *>(ob->data));
    }
  }
  else {
    ob = BKE_object_add(bmain, view_layer, type, name);
  }

  Base *ob_base_act = BASACT(view_layer);
  /* While not getting a valid base is not a good thing, it can happen in convoluted corner cases,
   * better not crash on it in releases. */
  BLI_assert(ob_base_act != nullptr);
  if (ob_base_act != nullptr) {
    ob_base_act->local_view_bits = local_view_bits;
    /* editor level activate, notifiers */
    ED_object_base_activate(C, ob_base_act);
  }

  /* more editor stuff */
  ED_object_base_init_transform_on_add(ob, loc, rot);

  /* Ignore collisions by default for non-mesh objects */
  if (type != OB_MESH) {
    ob->body_type = OB_BODY_TYPE_NO_COLLISION;
    ob->gameflag &= ~(OB_SENSOR | OB_RIGID_BODY | OB_SOFT_BODY | OB_COLLISION | OB_CHARACTER |
                      OB_OCCLUDER | OB_DYNAMIC | OB_NAVMESH); /* copied from rna_object.c */
  }

  /* TODO(sergey): This is weird to manually tag objects for update, better to
   * use DEG_id_tag_update here perhaps.
   */
  DEG_id_type_tag(bmain, ID_OB);
  DEG_relations_tag_update(bmain);
  if (ob->data != nullptr) {
    DEG_id_tag_update_ex(bmain, (ID *)ob->data, ID_RECALC_EDITORS);
  }

  if (enter_editmode) {
    ED_object_editmode_enter_ex(bmain, scene, ob, 0);
  }

  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  /* TODO(sergey): Use proper flag for tagging here. */
  DEG_id_tag_update(&scene->id, 0);

  ED_outliner_select_sync_from_object_tag(C);

  return ob;
}

Object *ED_object_add_type(bContext *C,
                           const int type,
                           const char *name,
                           const float loc[3],
                           const float rot[3],
                           const bool enter_editmode,
                           const ushort local_view_bits)
{
  return ED_object_add_type_with_obdata(
      C, type, name, loc, rot, enter_editmode, local_view_bits, nullptr);
}

/* for object add operator */
static int object_add_exec(bContext *C, wmOperator *op)
{
  ushort local_view_bits;
  bool enter_editmode;
  float loc[3], rot[3], radius;
  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  radius = RNA_float_get(op->ptr, "radius");
  Object *ob = ED_object_add_type(
      C, RNA_enum_get(op->ptr, "type"), nullptr, loc, rot, enter_editmode, local_view_bits);

  if (ob->type == OB_LATTICE) {
    /* lattice is a special case!
     * we never want to scale the obdata since that is the rest-state */
    copy_v3_fl(ob->scale, radius);
  }
  else {
    BKE_object_obdata_size_init(ob, radius);
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Object";
  ot->description = "Add an object to the scene";
  ot->idname = "OBJECT_OT_add";

  /* api callbacks */
  ot->exec = object_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ED_object_add_unit_props_radius(ot);
  PropertyRNA *prop = RNA_def_enum(ot->srna, "type", rna_enum_object_type_items, 0, "Type", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_ID);

  ED_object_add_generic_props(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Probe Operator
 * \{ */

/* for object add operator */
static const char *get_lightprobe_defname(int type)
{
  switch (type) {
    case LIGHTPROBE_TYPE_GRID:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "IrradianceVolume");
    case LIGHTPROBE_TYPE_PLANAR:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "ReflectionPlane");
    case LIGHTPROBE_TYPE_CUBE:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "ReflectionCubemap");
    default:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "LightProbe");
  }
}

static int lightprobe_add_exec(bContext *C, wmOperator *op)
{
  bool enter_editmode;
  ushort local_view_bits;
  float loc[3], rot[3];
  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  int type = RNA_enum_get(op->ptr, "type");
  float radius = RNA_float_get(op->ptr, "radius");

  Object *ob = ED_object_add_type(
      C, OB_LIGHTPROBE, get_lightprobe_defname(type), loc, rot, false, local_view_bits);
  copy_v3_fl(ob->scale, radius);

  LightProbe *probe = (LightProbe *)ob->data;

  BKE_lightprobe_type_set(probe, type);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_lightprobe_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Light Probe";
  ot->description = "Add a light probe object";
  ot->idname = "OBJECT_OT_lightprobe_add";

  /* api callbacks */
  ot->exec = lightprobe_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", lightprobe_type_items, 0, "Type", "");

  ED_object_add_unit_props_radius(ot);
  ED_object_add_generic_props(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Effector Operator
 * \{ */

/* for object add operator */

static const char *get_effector_defname(ePFieldType type)
{
  switch (type) {
    case PFIELD_FORCE:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Force");
    case PFIELD_VORTEX:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Vortex");
    case PFIELD_MAGNET:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Magnet");
    case PFIELD_WIND:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Wind");
    case PFIELD_GUIDE:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "CurveGuide");
    case PFIELD_TEXTURE:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "TextureField");
    case PFIELD_HARMONIC:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Harmonic");
    case PFIELD_CHARGE:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Charge");
    case PFIELD_LENNARDJ:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Lennard-Jones");
    case PFIELD_BOID:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Boid");
    case PFIELD_TURBULENCE:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Turbulence");
    case PFIELD_DRAG:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Drag");
    case PFIELD_FLUIDFLOW:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "FluidField");
    case PFIELD_NULL:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Field");
    case NUM_PFIELD_TYPES:
      break;
  }

  BLI_assert(false);
  return CTX_DATA_(BLT_I18NCONTEXT_ID_OBJECT, "Field");
}

static int effector_add_exec(bContext *C, wmOperator *op)
{
  bool enter_editmode;
  ushort local_view_bits;
  float loc[3], rot[3];
  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  const ePFieldType type = static_cast<ePFieldType>(RNA_enum_get(op->ptr, "type"));
  float dia = RNA_float_get(op->ptr, "radius");

  Object *ob;
  if (type == PFIELD_GUIDE) {
    Main *bmain = CTX_data_main(C);
    Scene *scene = CTX_data_scene(C);
    ob = ED_object_add_type(
        C, OB_CURVES_LEGACY, get_effector_defname(type), loc, rot, false, local_view_bits);

    Curve *cu = static_cast<Curve *>(ob->data);
    cu->flag |= CU_PATH | CU_3D;
    ED_object_editmode_enter_ex(bmain, scene, ob, 0);

    float mat[4][4];
    ED_object_new_primitive_matrix(C, ob, loc, rot, nullptr, mat);
    mul_mat3_m4_fl(mat, dia);
    BLI_addtail(&cu->editnurb->nurbs,
                ED_curve_add_nurbs_primitive(C, ob, mat, CU_NURBS | CU_PRIM_PATH, 1));
    if (!enter_editmode) {
      ED_object_editmode_exit_ex(bmain, scene, ob, EM_FREEDATA);
    }
  }
  else {
    ob = ED_object_add_type(
        C, OB_EMPTY, get_effector_defname(type), loc, rot, false, local_view_bits);
    BKE_object_obdata_size_init(ob, dia);
    if (ELEM(type, PFIELD_WIND, PFIELD_VORTEX)) {
      ob->empty_drawtype = OB_SINGLE_ARROW;
    }
  }

  ob->pd = BKE_partdeflect_new(type);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_effector_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Effector";
  ot->description = "Add an empty object with a physics effector to the scene";
  ot->idname = "OBJECT_OT_effector_add";

  /* api callbacks */
  ot->exec = effector_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", field_type_items, 0, "Type", "");

  ED_object_add_unit_props_radius(ot);
  ED_object_add_generic_props(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Camera Operator
 * \{ */

static int object_camera_add_exec(bContext *C, wmOperator *op)
{
  View3D *v3d = CTX_wm_view3d(C);
  Scene *scene = CTX_data_scene(C);

  /* force view align for cameras */
  RNA_enum_set(op->ptr, "align", ALIGN_VIEW);

  ushort local_view_bits;
  bool enter_editmode;
  float loc[3], rot[3];
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  Object *ob = ED_object_add_type(C, OB_CAMERA, nullptr, loc, rot, false, local_view_bits);

  if (v3d) {
    if (v3d->camera == nullptr) {
      v3d->camera = ob;
    }
    if (v3d->scenelock && scene->camera == nullptr) {
      scene->camera = ob;
    }
  }

  Camera *cam = static_cast<Camera *>(ob->data);
  cam->drawsize = v3d ? ED_view3d_grid_scale(scene, v3d, nullptr) :
                        ED_scene_grid_scale(scene, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_camera_add(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Add Camera";
  ot->description = "Add a camera object to the scene";
  ot->idname = "OBJECT_OT_camera_add";

  /* api callbacks */
  ot->exec = object_camera_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ED_object_add_generic_props(ot, true);

  /* hide this for cameras, default */
  prop = RNA_struct_type_find_property(ot->srna, "align");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Metaball Operator
 * \{ */

static int object_metaball_add_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  ushort local_view_bits;
  bool enter_editmode;
  float loc[3], rot[3];
  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }

  bool newob = false;
  Object *obedit = OBEDIT_FROM_VIEW_LAYER(view_layer);
  if (obedit == nullptr || obedit->type != OB_MBALL) {
    obedit = ED_object_add_type(C, OB_MBALL, nullptr, loc, rot, true, local_view_bits);
    newob = true;
  }
  else {
    DEG_id_tag_update(&obedit->id, ID_RECALC_GEOMETRY);
  }

  float mat[4][4];
  ED_object_new_primitive_matrix(C, obedit, loc, rot, nullptr, mat);
  /* Halving here is done to account for constant values from #BKE_mball_element_add.
   * While the default radius of the resulting meta element is 2,
   * we want to pass in 1 so other values such as resolution are scaled by 1.0. */
  float dia = RNA_float_get(op->ptr, "radius") / 2;

  ED_mball_add_primitive(C, obedit, newob, mat, dia, RNA_enum_get(op->ptr, "type"));

  /* userdef */
  if (newob && !enter_editmode) {
    ED_object_editmode_exit_ex(bmain, scene, obedit, EM_FREEDATA);
  }
  else {
    /* Only needed in edit-mode (#ED_object_add_type normally handles this). */
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, obedit);
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_metaball_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Metaball";
  ot->description = "Add an metaball object to the scene";
  ot->idname = "OBJECT_OT_metaball_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_metaball_add_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "type", rna_enum_metaelem_type_items, 0, "Primitive", "");

  ED_object_add_unit_props_radius_ex(ot, 2.0f);
  ED_object_add_generic_props(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Text Operator
 * \{ */

static int object_add_text_exec(bContext *C, wmOperator *op)
{
  Object *obedit = CTX_data_edit_object(C);
  bool enter_editmode;
  ushort local_view_bits;
  float loc[3], rot[3];

  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  if (obedit && obedit->type == OB_FONT) {
    return OPERATOR_CANCELLED;
  }

  obedit = ED_object_add_type(C, OB_FONT, nullptr, loc, rot, enter_editmode, local_view_bits);
  BKE_object_obdata_size_init(obedit, RNA_float_get(op->ptr, "radius"));

  return OPERATOR_FINISHED;
}

void OBJECT_OT_text_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Text";
  ot->description = "Add a text object to the scene";
  ot->idname = "OBJECT_OT_text_add";

  /* api callbacks */
  ot->exec = object_add_text_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ED_object_add_unit_props_radius(ot);
  ED_object_add_generic_props(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Armature Operator
 * \{ */

static int object_armature_add_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obedit = OBEDIT_FROM_VIEW_LAYER(view_layer);

  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  bool newob = false;
  bool enter_editmode;
  ushort local_view_bits;
  float loc[3], rot[3], dia;
  bool view_aligned = rv3d && (U.flag & USER_ADD_VIEWALIGNED);

  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, &enter_editmode, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  if ((obedit == nullptr) || (obedit->type != OB_ARMATURE)) {
    obedit = ED_object_add_type(C, OB_ARMATURE, nullptr, loc, rot, true, local_view_bits);
    ED_object_editmode_enter_ex(bmain, scene, obedit, 0);
    newob = true;
  }
  else {
    DEG_id_tag_update(&obedit->id, ID_RECALC_GEOMETRY);
  }

  if (obedit == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Cannot create editmode armature");
    return OPERATOR_CANCELLED;
  }

  dia = RNA_float_get(op->ptr, "radius");
  ED_armature_ebone_add_primitive(obedit, dia, view_aligned);

  /* userdef */
  if (newob && !enter_editmode) {
    ED_object_editmode_exit_ex(bmain, scene, obedit, EM_FREEDATA);
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_armature_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Armature";
  ot->description = "Add an armature object to the scene";
  ot->idname = "OBJECT_OT_armature_add";

  /* api callbacks */
  ot->exec = object_armature_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ED_object_add_unit_props_radius(ot);
  ED_object_add_generic_props(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Empty Operator
 * \{ */

static int object_empty_add_exec(bContext *C, wmOperator *op)
{
  Object *ob;
  int type = RNA_enum_get(op->ptr, "type");
  ushort local_view_bits;
  float loc[3], rot[3];

  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  ob = ED_object_add_type(C, OB_EMPTY, nullptr, loc, rot, false, local_view_bits);

  BKE_object_empty_draw_type_set(ob, type);
  BKE_object_obdata_size_init(ob, RNA_float_get(op->ptr, "radius"));

  return OPERATOR_FINISHED;
}

void OBJECT_OT_empty_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Empty";
  ot->description = "Add an empty object to the scene";
  ot->idname = "OBJECT_OT_empty_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_empty_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", rna_enum_object_empty_drawtype_items, 0, "Type", "");

  ED_object_add_unit_props_radius(ot);
  ED_object_add_generic_props(ot, false);
}

static int empty_drop_named_image_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Scene *scene = CTX_data_scene(C);

  Image *ima = nullptr;

  ima = (Image *)WM_operator_drop_load_path(C, op, ID_IM);
  if (!ima) {
    return OPERATOR_CANCELLED;
  }
  /* handled below */
  id_us_min(&ima->id);

  Object *ob = nullptr;
  Object *ob_cursor = ED_view3d_give_object_under_cursor(C, event->mval);

  /* either change empty under cursor or create a new empty */
  if (ob_cursor && ob_cursor->type == OB_EMPTY) {
    WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
    DEG_id_tag_update((ID *)ob_cursor, ID_RECALC_TRANSFORM);
    ob = ob_cursor;
  }
  else {
    /* add new empty */
    ushort local_view_bits;
    float rot[3];

    if (!ED_object_add_generic_get_opts(
            C, op, 'Z', nullptr, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
      return OPERATOR_CANCELLED;
    }
    ob = ED_object_add_type(C, OB_EMPTY, nullptr, nullptr, rot, false, local_view_bits);

    ED_object_location_from_view(C, ob->loc);
    ED_view3d_cursor3d_position(C, event->mval, false, ob->loc);
    ED_object_rotation_from_view(C, ob->rot, 'Z');
    ob->empty_drawsize = 5.0f;
  }

  BKE_object_empty_draw_type_set(ob, OB_EMPTY_IMAGE);

  id_us_min(static_cast<ID *>(ob->data));
  ob->data = ima;
  id_us_plus(static_cast<ID *>(ob->data));

  return OPERATOR_FINISHED;
}

void OBJECT_OT_drop_named_image(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Add Empty Image/Drop Image to Empty";
  ot->description = "Add an empty image type to scene with data";
  ot->idname = "OBJECT_OT_drop_named_image";

  /* api callbacks */
  ot->invoke = empty_drop_named_image_invoke;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_string(ot->srna, "filepath", nullptr, FILE_MAX, "Filepath", "Path to image file");
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));
  RNA_def_boolean(ot->srna,
                  "relative_path",
                  true,
                  "Relative Path",
                  "Select the file relative to the blend file");
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));

  WM_operator_properties_id_lookup(ot, true);

  ED_object_add_generic_props(ot, false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Gpencil Operator
 * \{ */

static bool object_gpencil_add_poll(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  Object *obact = CTX_data_active_object(C);

  if ((scene == nullptr) || ID_IS_LINKED(scene) || ID_IS_OVERRIDE_LIBRARY(scene)) {
    return false;
  }

  if (obact && obact->type == OB_GPENCIL) {
    if (obact->mode != OB_MODE_OBJECT) {
      return false;
    }
  }

  return true;
}

static int object_gpencil_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C), *ob_orig = ob;
  bGPdata *gpd = (ob && (ob->type == OB_GPENCIL)) ? static_cast<bGPdata *>(ob->data) : nullptr;

  const int type = RNA_enum_get(op->ptr, "type");
  const bool use_in_front = RNA_boolean_get(op->ptr, "use_in_front");
  const bool use_lights = RNA_boolean_get(op->ptr, "use_lights");
  const int stroke_depth_order = RNA_enum_get(op->ptr, "stroke_depth_order");
  const float stroke_depth_offset = RNA_float_get(op->ptr, "stroke_depth_offset");

  ushort local_view_bits;
  float loc[3], rot[3];
  bool newob = false;

  /* NOTE: We use 'Y' here (not 'Z'), as. */
  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Y', loc, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  /* Add new object if not currently editing a GP object. */
  if ((gpd == nullptr) || (GPENCIL_ANY_MODE(gpd) == false)) {
    const char *ob_name = nullptr;
    switch (type) {
      case GP_EMPTY: {
        ob_name = CTX_DATA_(BLT_I18NCONTEXT_ID_GPENCIL, "GPencil");
        break;
      }
      case GP_MONKEY: {
        ob_name = CTX_DATA_(BLT_I18NCONTEXT_ID_GPENCIL, "Suzanne");
        break;
      }
      case GP_STROKE: {
        ob_name = CTX_DATA_(BLT_I18NCONTEXT_ID_GPENCIL, "Stroke");
        break;
      }
      case GP_LRT_OBJECT:
      case GP_LRT_SCENE:
      case GP_LRT_COLLECTION: {
        ob_name = CTX_DATA_(BLT_I18NCONTEXT_ID_GPENCIL, "LineArt");
        break;
      }
      default: {
        break;
      }
    }

    ob = ED_object_add_type(C, OB_GPENCIL, ob_name, loc, rot, true, local_view_bits);
    gpd = static_cast<bGPdata *>(ob->data);
    newob = true;
  }
  else {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_ADDED, nullptr);
  }

  /* create relevant geometry */
  switch (type) {
    case GP_EMPTY: {
      float mat[4][4];

      ED_object_new_primitive_matrix(C, ob, loc, rot, nullptr, mat);
      ED_gpencil_create_blank(C, ob, mat);
      break;
    }
    case GP_STROKE: {
      float radius = RNA_float_get(op->ptr, "radius");
      float scale[3];
      copy_v3_fl(scale, radius);
      float mat[4][4];

      ED_object_new_primitive_matrix(C, ob, loc, rot, scale, mat);

      ED_gpencil_create_stroke(C, ob, mat);
      break;
    }
    case GP_MONKEY: {
      float radius = RNA_float_get(op->ptr, "radius");
      float scale[3];
      copy_v3_fl(scale, radius);
      float mat[4][4];

      ED_object_new_primitive_matrix(C, ob, loc, rot, scale, mat);

      ED_gpencil_create_monkey(C, ob, mat);
      break;
    }
    case GP_LRT_SCENE:
    case GP_LRT_COLLECTION:
    case GP_LRT_OBJECT: {
      float radius = RNA_float_get(op->ptr, "radius");
      float scale[3];
      copy_v3_fl(scale, radius);
      float mat[4][4];

      ED_object_new_primitive_matrix(C, ob, loc, rot, scale, mat);

      ED_gpencil_create_lineart(C, ob);

      gpd = static_cast<bGPdata *>(ob->data);

      /* Add Line Art modifier */
      LineartGpencilModifierData *md = (LineartGpencilModifierData *)BKE_gpencil_modifier_new(
          eGpencilModifierType_Lineart);
      BLI_addtail(&ob->greasepencil_modifiers, md);
      BKE_gpencil_modifier_unique_name(&ob->greasepencil_modifiers, (GpencilModifierData *)md);

      if (type == GP_LRT_COLLECTION) {
        md->source_type = LRT_SOURCE_COLLECTION;
        md->source_collection = CTX_data_collection(C);
      }
      else if (type == GP_LRT_OBJECT) {
        md->source_type = LRT_SOURCE_OBJECT;
        md->source_object = ob_orig;
      }
      else {
        /* Whole scene. */
        md->source_type = LRT_SOURCE_SCENE;
      }
      /* Only created one layer and one material. */
      strcpy(md->target_layer, ((bGPDlayer *)gpd->layers.first)->info);
      md->target_material = BKE_gpencil_material(ob, 1);
      if (md->target_material) {
        id_us_plus(&md->target_material->id);
      }

      if (use_lights) {
        ob->dtx |= OB_USE_GPENCIL_LIGHTS;
      }
      else {
        ob->dtx &= ~OB_USE_GPENCIL_LIGHTS;
      }

      /* Stroke object is drawn in front of meshes by default. */
      if (use_in_front) {
        ob->dtx |= OB_DRAW_IN_FRONT;
      }
      else {
        if (stroke_depth_order == GP_DRAWMODE_3D) {
          gpd->draw_mode = GP_DRAWMODE_3D;
        }
        md->stroke_depth_offset = stroke_depth_offset;
      }

      break;
    }
    default:
      BKE_report(op->reports, RPT_WARNING, "Not implemented");
      break;
  }

  /* If this is a new object, initialize default stuff (colors, etc.) */
  if (newob) {
    /* set default viewport color to black */
    copy_v3_fl(ob->color, 0.0f);

    ED_gpencil_add_defaults(C, ob);
  }

  return OPERATOR_FINISHED;
}

static void object_add_ui(bContext *UNUSED(C), wmOperator *op)
{
  uiLayout *layout = op->layout;

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, op->ptr, "radius", 0, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "align", 0, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "location", 0, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "rotation", 0, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "type", 0, nullptr, ICON_NONE);

  int type = RNA_enum_get(op->ptr, "type");
  if (ELEM(type, GP_LRT_COLLECTION, GP_LRT_OBJECT, GP_LRT_SCENE)) {
    uiItemR(layout, op->ptr, "use_lights", 0, nullptr, ICON_NONE);
    uiItemR(layout, op->ptr, "use_in_front", 0, nullptr, ICON_NONE);
    bool in_front = RNA_boolean_get(op->ptr, "use_in_front");
    uiLayout *col = uiLayoutColumn(layout, false);
    uiLayoutSetActive(col, !in_front);
    uiItemR(col, op->ptr, "stroke_depth_offset", 0, nullptr, ICON_NONE);
    uiItemR(col, op->ptr, "stroke_depth_order", 0, nullptr, ICON_NONE);
  }
}

static EnumPropertyItem rna_enum_gpencil_add_stroke_depth_order_items[] = {
    {GP_DRAWMODE_2D,
     "2D",
     0,
     "2D Layers",
     "Display strokes using grease pencil layers to define order"},
    {GP_DRAWMODE_3D, "3D", 0, "3D Location", "Display strokes using real 3D position in 3D space"},
    {0, nullptr, 0, nullptr, nullptr},
};

void OBJECT_OT_gpencil_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Grease Pencil";
  ot->description = "Add a Grease Pencil object to the scene";
  ot->idname = "OBJECT_OT_gpencil_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_gpencil_add_exec;
  ot->poll = object_gpencil_add_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* ui */
  ot->ui = object_add_ui;

  /* properties */
  ED_object_add_unit_props_radius(ot);
  ED_object_add_generic_props(ot, false);

  ot->prop = RNA_def_enum(ot->srna, "type", rna_enum_object_gpencil_type_items, 0, "Type", "");
  RNA_def_boolean(ot->srna,
                  "use_in_front",
                  true,
                  "Show In Front",
                  "Show line art grease pencil in front of everything");
  RNA_def_float(ot->srna,
                "stroke_depth_offset",
                0.05f,
                0.0f,
                FLT_MAX,
                "Stroke Offset",
                "Stroke offset for the line art modifier",
                0.0f,
                0.5f);
  RNA_def_boolean(
      ot->srna, "use_lights", false, "Use Lights", "Use lights for this grease pencil object");
  RNA_def_enum(
      ot->srna,
      "stroke_depth_order",
      rna_enum_gpencil_add_stroke_depth_order_items,
      GP_DRAWMODE_3D,
      "Stroke Depth Order",
      "Defines how the strokes are ordered in 3D space for objects not displayed 'In Front')");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Light Operator
 * \{ */

static const char *get_light_defname(int type)
{
  switch (type) {
    case LA_LOCAL:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "Point");
    case LA_SUN:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "Sun");
    case LA_SPOT:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "Spot");
    case LA_AREA:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "Area");
    default:
      return CTX_DATA_(BLT_I18NCONTEXT_ID_LIGHT, "Light");
  }
}

static int object_light_add_exec(bContext *C, wmOperator *op)
{
  Object *ob;
  Light *la;
  int type = RNA_enum_get(op->ptr, "type");
  ushort local_view_bits;
  float loc[3], rot[3];

  WM_operator_view3d_unit_defaults(C, op);
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  ob = ED_object_add_type(C, OB_LAMP, get_light_defname(type), loc, rot, false, local_view_bits);

  float size = RNA_float_get(op->ptr, "radius");
  /* Better defaults for light size. */
  switch (type) {
    case LA_LOCAL:
    case LA_SPOT:
      break;
    case LA_AREA:
      size *= 4.0f;
      break;
    default:
      size *= 0.5f;
      break;
  }
  BKE_object_obdata_size_init(ob, size);

  la = (Light *)ob->data;
  la->type = type;

  if (type == LA_SUN) {
    la->energy = 1.0f;
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_light_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Light";
  ot->description = "Add a light object to the scene";
  ot->idname = "OBJECT_OT_light_add";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_light_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(ot->srna, "type", rna_enum_light_type_items, 0, "Type", "");
  RNA_def_property_translation_context(ot->prop, BLT_I18NCONTEXT_ID_LIGHT);

  ED_object_add_unit_props_radius(ot);
  ED_object_add_generic_props(ot, false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Collection Instance Operator
 * \{ */

struct CollectionAddInfo {
  /* The collection that is supposed to be added, determined through operator properties. */
  Collection *collection;
  /* The local-view bits (if any) the object should have set to become visible in current context.
   */
  ushort local_view_bits;
  /* The transform that should be applied to the collection, determined through operator properties
   * if set (e.g. to place the collection under the cursor), otherwise through context (e.g. 3D
   * cursor location). */
  float loc[3], rot[3];
};

static std::optional<CollectionAddInfo> collection_add_info_get_from_op(bContext *C,
                                                                        wmOperator *op)
{
  CollectionAddInfo add_info{};

  Main *bmain = CTX_data_main(C);

  PropertyRNA *prop_location = RNA_struct_find_property(op->ptr, "location");

  add_info.collection = reinterpret_cast<Collection *>(
      WM_operator_properties_id_lookup_from_name_or_session_uuid(bmain, op->ptr, ID_GR));

  bool update_location_if_necessary = false;
  if (add_info.collection) {
    update_location_if_necessary = true;
  }
  else {
    add_info.collection = static_cast<Collection *>(
        BLI_findlink(&bmain->collections, RNA_enum_get(op->ptr, "collection")));
  }

  if (update_location_if_necessary) {
    int mval[2];
    if (!RNA_property_is_set(op->ptr, prop_location) && object_add_drop_xy_get(C, op, &mval)) {
      ED_object_location_from_view(C, add_info.loc);
      ED_view3d_cursor3d_position(C, mval, false, add_info.loc);
      RNA_property_float_set_array(op->ptr, prop_location, add_info.loc);
    }
  }

  if (add_info.collection == nullptr) {
    return std::nullopt;
  }

  if (!ED_object_add_generic_get_opts(C,
                                      op,
                                      'Z',
                                      add_info.loc,
                                      add_info.rot,
                                      nullptr,
                                      nullptr,
                                      &add_info.local_view_bits,
                                      nullptr)) {
    return std::nullopt;
  }

  ViewLayer *view_layer = CTX_data_view_layer(C);

  /* Avoid dependency cycles. */
  LayerCollection *active_lc = BKE_layer_collection_get_active(view_layer);
  while (BKE_collection_cycle_find(active_lc->collection, add_info.collection)) {
    active_lc = BKE_layer_collection_activate_parent(view_layer, active_lc);
  }

  return add_info;
}

static int collection_instance_add_exec(bContext *C, wmOperator *op)
{
  std::optional<CollectionAddInfo> add_info = collection_add_info_get_from_op(C, op);
  if (!add_info) {
    return OPERATOR_CANCELLED;
  }

  Object *ob = ED_object_add_type(C,
                                  OB_EMPTY,
                                  add_info->collection->id.name + 2,
                                  add_info->loc,
                                  add_info->rot,
                                  false,
                                  add_info->local_view_bits);
  ob->instance_collection = add_info->collection;
  ob->empty_drawsize = U.collection_instance_empty_size;
  ob->transflag |= OB_DUPLICOLLECTION;
  id_us_plus(&add_info->collection->id);

  return OPERATOR_FINISHED;
}

static int object_instance_add_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!object_add_drop_xy_is_set(op)) {
    RNA_int_set(op->ptr, "drop_x", event->xy[0]);
    RNA_int_set(op->ptr, "drop_y", event->xy[1]);
  }

  if (!WM_operator_properties_id_lookup_is_set(op->ptr)) {
    return WM_enum_search_invoke(C, op, event);
  }
  return op->type->exec(C, op);
}

void OBJECT_OT_collection_instance_add(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Add Collection Instance";
  ot->description = "Add a collection instance";
  ot->idname = "OBJECT_OT_collection_instance_add";

  /* api callbacks */
  ot->invoke = object_instance_add_invoke;
  ot->exec = collection_instance_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_string(
      ot->srna, "name", "Collection", MAX_ID_NAME - 2, "Name", "Collection name to add");
  prop = RNA_def_enum(ot->srna, "collection", DummyRNA_NULL_items, 0, "Collection", "");
  RNA_def_enum_funcs(prop, RNA_collection_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;
  ED_object_add_generic_props(ot, false);

  WM_operator_properties_id_lookup(ot, false);

  object_add_drop_xy_props(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Collection Drop Operator
 *
 * Internal operator for collection dropping.
 *
 * \warning This is tied closely together to the drop-box callbacks, so it shouldn't be used on its
 *          own.
 *
 * The drop-box callback imports the collection, links it into the view-layer, selects all imported
 * objects (which may include peripheral objects like parents or boolean-objects of an object in
 * the collection) and activates one. Only the callback has enough info to do this reliably. Based
 * on the instancing operator option, this operator then does one of two things:
 * - Instancing enabled: Unlink the collection again, and instead add a collection instance empty
 *   at the drop position.
 * - Instancing disabled: Transform the objects to the drop position, keeping all relative
 *   transforms of the objects to each other as is.
 *
 * \{ */

static int collection_drop_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  LayerCollection *active_collection = CTX_data_layer_collection(C);
  std::optional<CollectionAddInfo> add_info = collection_add_info_get_from_op(C, op);
  if (!add_info) {
    return OPERATOR_CANCELLED;
  }

  if (RNA_boolean_get(op->ptr, "use_instance")) {
    BKE_collection_child_remove(bmain, active_collection->collection, add_info->collection);
    DEG_id_tag_update(&active_collection->collection->id, ID_RECALC_COPY_ON_WRITE);
    DEG_relations_tag_update(bmain);

    Object *ob = ED_object_add_type(C,
                                    OB_EMPTY,
                                    add_info->collection->id.name + 2,
                                    add_info->loc,
                                    add_info->rot,
                                    false,
                                    add_info->local_view_bits);
    ob->instance_collection = add_info->collection;
    ob->empty_drawsize = U.collection_instance_empty_size;
    ob->transflag |= OB_DUPLICOLLECTION;
    id_us_plus(&add_info->collection->id);
  }
  else {
    ViewLayer *view_layer = CTX_data_view_layer(C);
    float delta_mat[4][4];
    unit_m4(delta_mat);

    const float scale[3] = {1.0f, 1.0f, 1.0f};
    loc_eul_size_to_mat4(delta_mat, add_info->loc, add_info->rot, scale);

    float offset[3];
    /* Reverse apply the instance offset, so toggling the Instance option doesn't cause the
     * collection to jump. */
    negate_v3_v3(offset, add_info->collection->instance_offset);
    translate_m4(delta_mat, UNPACK3(offset));

    ObjectsInViewLayerParams params = {0};
    uint objects_len;
    Object **objects = BKE_view_layer_array_selected_objects_params(
        view_layer, nullptr, &objects_len, &params);
    ED_object_xform_array_m4(objects, objects_len, delta_mat);

    MEM_freeN(objects);
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_collection_external_asset_drop(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  /* Name should only be displayed in the drag tooltip. */
  ot->name = "Add Collection";
  ot->description = "Add the dragged collection to the scene";
  ot->idname = "OBJECT_OT_collection_external_asset_drop";

  /* api callbacks */
  ot->invoke = object_instance_add_invoke;
  ot->exec = collection_drop_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  /* properties */
  WM_operator_properties_id_lookup(ot, false);

  ED_object_add_generic_props(ot, false);

  /* IMPORTANT: Instancing option. Intentionally remembered across executions (no #PROP_SKIP_SAVE).
   */
  RNA_def_boolean(ot->srna,
                  "use_instance",
                  true,
                  "Instance",
                  "Add the dropped collection as collection instance");

  object_add_drop_xy_props(ot);

  prop = RNA_def_enum(ot->srna, "collection", DummyRNA_NULL_items, 0, "Collection", "");
  RNA_def_enum_funcs(prop, RNA_collection_itemf);
  RNA_def_property_flag(prop,
                        (PropertyFlag)(PROP_SKIP_SAVE | PROP_HIDDEN | PROP_ENUM_NO_TRANSLATE));
  ot->prop = prop;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Data Instance Operator
 *
 * Use for dropping ID's from the outliner.
 * \{ */

static int object_data_instance_add_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  ID *id = nullptr;
  ushort local_view_bits;
  float loc[3], rot[3];

  PropertyRNA *prop_type = RNA_struct_find_property(op->ptr, "type");
  PropertyRNA *prop_location = RNA_struct_find_property(op->ptr, "location");

  const short id_type = RNA_property_enum_get(op->ptr, prop_type);
  id = WM_operator_properties_id_lookup_from_name_or_session_uuid(
      bmain, op->ptr, (ID_Type)id_type);
  if (id == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int object_type = BKE_object_obdata_to_type(id);
  if (object_type == -1) {
    return OPERATOR_CANCELLED;
  }

  int mval[2];
  if (!RNA_property_is_set(op->ptr, prop_location) && object_add_drop_xy_get(C, op, &mval)) {
    ED_object_location_from_view(C, loc);
    ED_view3d_cursor3d_position(C, mval, false, loc);
    RNA_property_float_set_array(op->ptr, prop_location, loc);
  }

  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }

  ED_object_add_type_with_obdata(
      C, object_type, id->name + 2, loc, rot, false, local_view_bits, id);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_data_instance_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Object Data Instance";
  ot->description = "Add an object data instance";
  ot->idname = "OBJECT_OT_data_instance_add";

  /* api callbacks */
  ot->invoke = object_add_drop_xy_generic_invoke;
  ot->exec = object_data_instance_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_id_lookup(ot, true);
  PropertyRNA *prop = RNA_def_enum(ot->srna, "type", rna_enum_id_type_items, 0, "Type", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_ID);
  ED_object_add_generic_props(ot, false);

  object_add_drop_xy_props(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Speaker Operator
 * \{ */

static int object_speaker_add_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  ushort local_view_bits;
  float loc[3], rot[3];
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }
  Object *ob = ED_object_add_type(C, OB_SPEAKER, nullptr, loc, rot, false, local_view_bits);
  const bool is_liboverride = ID_IS_OVERRIDE_LIBRARY(ob);

  /* To make it easier to start using this immediately in NLA, a default sound clip is created
   * ready to be moved around to re-time the sound and/or make new sound clips. */
  {
    /* create new data for NLA hierarchy */
    AnimData *adt = BKE_animdata_ensure_id(&ob->id);
    NlaTrack *nlt = BKE_nlatrack_add(adt, nullptr, is_liboverride);
    NlaStrip *strip = BKE_nla_add_soundstrip(bmain, scene, static_cast<Speaker *>(ob->data));
    strip->start = scene->r.cfra;
    strip->end += strip->start;

    /* hook them up */
    BKE_nlatrack_add_strip(nlt, strip, is_liboverride);

    /* Auto-name the strip, and give the track an interesting name. */
    BLI_strncpy(nlt->name, DATA_("SoundTrack"), sizeof(nlt->name));
    BKE_nlastrip_validate_name(adt, strip);

    WM_event_add_notifier(C, NC_ANIMATION | ND_NLA | NA_ADDED, nullptr);
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_speaker_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Speaker";
  ot->description = "Add a speaker object to the scene";
  ot->idname = "OBJECT_OT_speaker_add";

  /* api callbacks */
  ot->exec = object_speaker_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ED_object_add_generic_props(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Curves Operator
 * \{ */

static int object_curves_random_add_exec(bContext *C, wmOperator *op)
{
  using namespace blender;

  ushort local_view_bits;
  float loc[3], rot[3];
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }

  Object *object = ED_object_add_type(C, OB_CURVES, nullptr, loc, rot, false, local_view_bits);

  Curves *curves_id = static_cast<Curves *>(object->data);
  bke::CurvesGeometry::wrap(curves_id->geometry) = ed::curves::primitive_random_sphere(500, 8);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_curves_random_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Random Curves";
  ot->description = "Add a curves object with random curves to the scene";
  ot->idname = "OBJECT_OT_curves_random_add";

  /* api callbacks */
  ot->exec = object_curves_random_add_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ED_object_add_generic_props(ot, false);
}

static int object_curves_empty_hair_add_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);

  ushort local_view_bits;
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', nullptr, nullptr, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }

  Object *surface_ob = CTX_data_active_object(C);
  BLI_assert(surface_ob != nullptr);

  Object *curves_ob = ED_object_add_type(
      C, OB_CURVES, nullptr, nullptr, nullptr, false, local_view_bits);
  BKE_object_apply_mat4(curves_ob, surface_ob->obmat, false, false);

  /* Set surface object. */
  Curves *curves_id = static_cast<Curves *>(curves_ob->data);
  curves_id->surface = surface_ob;

  /* Parent to surface object. */
  ED_object_parent_set(
      op->reports, C, scene, curves_ob, surface_ob, PAR_OBJECT, false, true, nullptr);

  /* Decide which UV map to use for attachment. */
  Mesh *surface_mesh = static_cast<Mesh *>(surface_ob->data);
  const char *uv_name = CustomData_get_active_layer_name(&surface_mesh->ldata, CD_MLOOPUV);
  if (uv_name != nullptr) {
    curves_id->surface_uv_map = BLI_strdup(uv_name);
  }

  /* Add deformation modifier. */
  blender::ed::curves::ensure_surface_deformation_node_exists(*C, *curves_ob);

  /* Make sure the surface object has a rest position attribute which is necessary for
   * deformations. */
  surface_ob->modifier_flag |= OB_MODIFIER_FLAG_ADD_REST_POSITION;

  return OPERATOR_FINISHED;
}

static bool object_curves_empty_hair_add_poll(bContext *C)
{
  if (!ED_operator_objectmode(C)) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH) {
    CTX_wm_operator_poll_msg_set(C, "No active mesh object");
    return false;
  }
  return true;
}

void OBJECT_OT_curves_empty_hair_add(wmOperatorType *ot)
{
  ot->name = "Add Empty Curves";
  ot->description = "Add an empty curve object to the scene with the selected mesh as surface";
  ot->idname = "OBJECT_OT_curves_empty_hair_add";

  ot->exec = object_curves_empty_hair_add_exec;
  ot->poll = object_curves_empty_hair_add_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ED_object_add_generic_props(ot, false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Point Cloud Operator
 * \{ */

static bool object_pointcloud_add_poll(bContext *C)
{
  if (!U.experimental.use_new_point_cloud_type) {
    return false;
  }
  return ED_operator_objectmode(C);
}

static int object_pointcloud_add_exec(bContext *C, wmOperator *op)
{
  ushort local_view_bits;
  float loc[3], rot[3];
  if (!ED_object_add_generic_get_opts(
          C, op, 'Z', loc, rot, nullptr, nullptr, &local_view_bits, nullptr)) {
    return OPERATOR_CANCELLED;
  }

  Object *object = ED_object_add_type(C, OB_POINTCLOUD, nullptr, loc, rot, false, local_view_bits);
  object->dtx |= OB_DRAWBOUNDOX; /* TODO: remove once there is actual drawing. */

  return OPERATOR_FINISHED;
}

void OBJECT_OT_pointcloud_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Point Cloud";
  ot->description = "Add a point cloud object to the scene";
  ot->idname = "OBJECT_OT_pointcloud_add";

  /* api callbacks */
  ot->exec = object_pointcloud_add_exec;
  ot->poll = object_pointcloud_add_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ED_object_add_generic_props(ot, false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Object Operator
 * \{ */

void ED_object_base_free_and_unlink(Main *bmain, Scene *scene, Object *ob)
{
  if (ID_REAL_USERS(ob) <= 1 && ID_EXTRA_USERS(ob) == 0 &&
      BKE_library_ID_is_indirectly_used(bmain, ob)) {
    /* We cannot delete indirectly used object... */
    printf(
        "WARNING, undeletable object '%s', should have been caught before reaching this "
        "function!",
        ob->id.name + 2);
    return;
  }
  if (!BKE_lib_override_library_id_is_user_deletable(bmain, &ob->id)) {
    /* Do not delete objects used by overrides of collections. */
    return;
  }

  DEG_id_tag_update_ex(bmain, &ob->id, ID_RECALC_BASE_FLAGS);

  BKE_scene_collections_object_remove(bmain, scene, ob, true);
}

void ED_object_base_free_and_unlink_no_indirect_check(Main *bmain, Scene *scene, Object *ob)
{
  BLI_assert(!BKE_library_ID_is_indirectly_used(bmain, ob));
  DEG_id_tag_update_ex(bmain, &ob->id, ID_RECALC_BASE_FLAGS);
  BKE_scene_collections_object_remove(bmain, scene, ob, true);
}

static int object_delete_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  const bool use_global = RNA_boolean_get(op->ptr, "use_global");
  const bool confirm = op->flag & OP_IS_INVOKE;
  uint changed_count = 0;
  uint tagged_count = 0;

  if (CTX_data_edit_object(C)) {
    return OPERATOR_CANCELLED;
  }

  BKE_main_id_tag_all(bmain, LIB_TAG_DOIT, false);

  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    if (ob->id.tag & LIB_TAG_INDIRECT) {
      /* Can this case ever happen? */
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Cannot delete indirectly linked object '%s'",
                  ob->id.name + 2);
      continue;
    }

    if (!BKE_lib_override_library_id_is_user_deletable(bmain, &ob->id)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Cannot delete object '%s' as it is used by override collections",
                  ob->id.name + 2);
      continue;
    }

    if (ID_REAL_USERS(ob) <= 1 && ID_EXTRA_USERS(ob) == 0 &&
        BKE_library_ID_is_indirectly_used(bmain, ob)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Cannot delete object '%s' from scene '%s', indirectly used objects need at "
                  "least one user",
                  ob->id.name + 2,
                  scene->id.name + 2);
      continue;
    }

    /* if grease pencil object, set cache as dirty */
    if (ob->type == OB_GPENCIL) {
      bGPdata *gpd = (bGPdata *)ob->data;
      DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    }

    /* Use multi tagged delete if `use_global=True`, or the object is used only in one scene. */
    if (use_global || ID_REAL_USERS(ob) <= 1) {
      ob->id.tag |= LIB_TAG_DOIT;
      tagged_count += 1;
    }
    else {
      /* Object is used in multiple scenes. Delete the object from the current scene only. */
      ED_object_base_free_and_unlink_no_indirect_check(bmain, scene, ob);
      changed_count += 1;

      /* FIXME: this will also remove parent from grease pencil from other scenes. */
      /* Remove from Grease Pencil parent */
      LISTBASE_FOREACH (bGPdata *, gpd, &bmain->gpencils) {
        LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
          if (gpl->parent != nullptr) {
            if (gpl->parent == ob) {
              gpl->parent = nullptr;
            }
          }
        }
      }
    }
  }
  CTX_DATA_END;

  if ((changed_count + tagged_count) == 0) {
    return OPERATOR_CANCELLED;
  }

  if (tagged_count > 0) {
    BKE_id_multi_tagged_delete(bmain);
  }

  if (confirm) {
    BKE_reportf(op->reports, RPT_INFO, "Deleted %u object(s)", (changed_count + tagged_count));
  }

  /* delete has to handle all open scenes */
  BKE_main_id_tag_listbase(&bmain->scenes, LIB_TAG_DOIT, true);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    scene = WM_window_get_active_scene(win);

    if (scene->id.tag & LIB_TAG_DOIT) {
      scene->id.tag &= ~LIB_TAG_DOIT;

      DEG_relations_tag_update(bmain);

      DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
      WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
    }
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_delete(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete";
  ot->description = "Delete selected objects";
  ot->idname = "OBJECT_OT_delete";

  /* api callbacks */
  ot->invoke = WM_operator_confirm_or_exec;
  ot->exec = object_delete_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_boolean(
      ot->srna, "use_global", false, "Delete Globally", "Remove object from all scenes");
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));
  WM_operator_properties_confirm_or_exec(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy Object Utilities
 * \{ */

/* after copying objects, copied data should get new pointers */
static void copy_object_set_idnew(bContext *C)
{
  Main *bmain = CTX_data_main(C);

  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    BKE_libblock_relink_to_newid(bmain, &ob->id, 0);
  }
  CTX_DATA_END;

#ifndef NDEBUG
  /* Call to `BKE_libblock_relink_to_newid` above is supposed to have cleared all those flags. */
  ID *id_iter;
  FOREACH_MAIN_ID_BEGIN (bmain, id_iter) {
    if (GS(id_iter->name) == ID_OB) {
      /* Not all duplicated objects would be used by other newly duplicated data, so their flag
       * will not always be cleared. */
      continue;
    }
    BLI_assert((id_iter->tag & LIB_TAG_NEW) == 0);
  }
  FOREACH_MAIN_ID_END;
#endif

  BKE_sca_set_new_points();

  BKE_main_id_newptr_and_tag_clear(bmain);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Make Instanced Objects Real Operator
 * \{ */

/* XXX TODO: That whole hierarchy handling based on persistent_id tricks is
 * very confusing and convoluted, and it will fail in many cases besides basic ones.
 * Think this should be replaced by a proper tree-like representation of the instantiations,
 * should help a lot in both readability, and precise consistent rebuilding of hierarchy.
 */

/**
 * \note regarding hashing dupli-objects which come from OB_DUPLICOLLECTION,
 * skip the first member of #DupliObject.persistent_id
 * since its a unique index and we only want to know if the group objects are from the same
 * dupli-group instance.
 *
 * \note regarding hashing dupli-objects which come from non-OB_DUPLICOLLECTION,
 * include the first member of #DupliObject.persistent_id
 * since its the index of the vertex/face the object is instantiated on and we want to identify
 * objects on the same vertex/face.
 * In other words, we consider each group of objects from a same item as being
 * the 'local group' where to check for parents.
 */
static uint dupliobject_hash(const void *ptr)
{
  const DupliObject *dob = static_cast<const DupliObject *>(ptr);
  uint hash = BLI_ghashutil_ptrhash(dob->ob);

  if (dob->type == OB_DUPLICOLLECTION) {
    for (int i = 1; (i < MAX_DUPLI_RECUR) && dob->persistent_id[i] != INT_MAX; i++) {
      hash ^= (dob->persistent_id[i] ^ i);
    }
  }
  else {
    hash ^= (dob->persistent_id[0] ^ 0);
  }
  return hash;
}

/**
 * \note regarding hashing dupli-objects when using OB_DUPLICOLLECTION,
 * skip the first member of #DupliObject.persistent_id
 * since its a unique index and we only want to know if the group objects are from the same
 * dupli-group instance.
 */
static uint dupliobject_instancer_hash(const void *ptr)
{
  const DupliObject *dob = static_cast<const DupliObject *>(ptr);
  uint hash = BLI_ghashutil_inthash(dob->persistent_id[0]);
  for (int i = 1; (i < MAX_DUPLI_RECUR) && dob->persistent_id[i] != INT_MAX; i++) {
    hash ^= (dob->persistent_id[i] ^ i);
  }
  return hash;
}

/* Compare function that matches dupliobject_hash */
static bool dupliobject_cmp(const void *a_, const void *b_)
{
  const DupliObject *a = static_cast<const DupliObject *>(a_);
  const DupliObject *b = static_cast<const DupliObject *>(b_);

  if (a->ob != b->ob) {
    return true;
  }

  if (a->type != b->type) {
    return true;
  }

  if (a->type == OB_DUPLICOLLECTION) {
    for (int i = 1; (i < MAX_DUPLI_RECUR); i++) {
      if (a->persistent_id[i] != b->persistent_id[i]) {
        return true;
      }
      if (a->persistent_id[i] == INT_MAX) {
        break;
      }
    }
  }
  else {
    if (a->persistent_id[0] != b->persistent_id[0]) {
      return true;
    }
  }

  /* matching */
  return false;
}

/* Compare function that matches dupliobject_instancer_hash. */
static bool dupliobject_instancer_cmp(const void *a_, const void *b_)
{
  const DupliObject *a = static_cast<const DupliObject *>(a_);
  const DupliObject *b = static_cast<const DupliObject *>(b_);

  for (int i = 0; (i < MAX_DUPLI_RECUR); i++) {
    if (a->persistent_id[i] != b->persistent_id[i]) {
      return true;
    }
    if (a->persistent_id[i] == INT_MAX) {
      break;
    }
  }

  /* matching */
  return false;
}

static void make_object_duplilist_real(bContext *C,
                                       Depsgraph *depsgraph,
                                       Scene *scene,
                                       Base *base,
                                       const bool use_base_parent,
                                       const bool use_hierarchy)
{
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  GHash *parent_gh = nullptr, *instancer_gh = nullptr;

  Object *object_eval = DEG_get_evaluated_object(depsgraph, base->object);

  if (!(base->object->transflag & OB_DUPLI) &&
      !BKE_object_has_geometry_set_instances(object_eval)) {
    return;
  }

  ListBase *lb_duplis = object_duplilist(depsgraph, scene, object_eval);

  if (BLI_listbase_is_empty(lb_duplis)) {
    free_object_duplilist(lb_duplis);
    return;
  }

  GHash *dupli_gh = BLI_ghash_ptr_new(__func__);
  if (use_hierarchy) {
    parent_gh = BLI_ghash_new(dupliobject_hash, dupliobject_cmp, __func__);

    if (use_base_parent) {
      instancer_gh = BLI_ghash_new(
          dupliobject_instancer_hash, dupliobject_instancer_cmp, __func__);
    }
  }

  LISTBASE_FOREACH (DupliObject *, dob, lb_duplis) {
    Object *ob_src = DEG_get_original_object(dob->ob);
    Object *ob_dst = static_cast<Object *>(ID_NEW_SET(ob_src, BKE_id_copy(bmain, &ob_src->id)));
    id_us_min(&ob_dst->id);

    /* font duplis can have a totcol without material, we get them from parent
     * should be implemented better...
     */
    if (ob_dst->mat == nullptr) {
      ob_dst->totcol = 0;
    }

    BKE_collection_object_add_from(bmain, scene, base->object, ob_dst);
    Base *base_dst = BKE_view_layer_base_find(view_layer, ob_dst);
    BLI_assert(base_dst != nullptr);

    ED_object_base_select(base_dst, BA_SELECT);
    DEG_id_tag_update(&ob_dst->id, ID_RECALC_SELECT);

    BKE_scene_object_base_flag_sync_from_base(base_dst);

    /* make sure apply works */
    BKE_animdata_free(&ob_dst->id, true);
    ob_dst->adt = nullptr;

    ob_dst->parent = nullptr;
    BKE_constraints_free(&ob_dst->constraints);
    ob_dst->runtime.curve_cache = nullptr;
    const bool is_dupli_instancer = (ob_dst->transflag & OB_DUPLI) != 0;
    ob_dst->transflag &= ~OB_DUPLI;
    /* Remove instantiated collection, it's annoying to keep it here
     * (and get potentially a lot of usages of it then...). */
    id_us_min((ID *)ob_dst->instance_collection);
    ob_dst->instance_collection = nullptr;

    copy_m4_m4(ob_dst->obmat, dob->mat);
    BKE_object_apply_mat4(ob_dst, ob_dst->obmat, false, false);

    BLI_ghash_insert(dupli_gh, dob, ob_dst);
    if (parent_gh) {
      void **val;
      /* Due to nature of hash/comparison of this ghash, a lot of duplis may be considered as
       * 'the same', this avoids trying to insert same key several time and
       * raise asserts in debug builds... */
      if (!BLI_ghash_ensure_p(parent_gh, dob, &val)) {
        *val = ob_dst;
      }

      if (is_dupli_instancer && instancer_gh) {
        /* Same as above, we may have several 'hits'. */
        if (!BLI_ghash_ensure_p(instancer_gh, dob, &val)) {
          *val = ob_dst;
        }
      }
    }
  }

  LISTBASE_FOREACH (DupliObject *, dob, lb_duplis) {
    Object *ob_src = dob->ob;
    Object *ob_dst = static_cast<Object *>(BLI_ghash_lookup(dupli_gh, dob));

    /* Remap new object to itself, and clear again newid pointer of orig object. */
    BKE_libblock_relink_to_newid(bmain, &ob_dst->id, 0);

    BKE_sca_set_new_points_ob(ob_dst);

    DEG_id_tag_update(&ob_dst->id, ID_RECALC_GEOMETRY);

    if (use_hierarchy) {
      /* original parents */
      Object *ob_src_par = ob_src->parent;
      Object *ob_dst_par = nullptr;

      /* find parent that was also made real */
      if (ob_src_par) {
        /* OK to keep most of the members uninitialized,
         * they won't be read, this is simply for a hash lookup. */
        DupliObject dob_key;
        dob_key.ob = ob_src_par;
        dob_key.type = dob->type;
        if (dob->type == OB_DUPLICOLLECTION) {
          memcpy(&dob_key.persistent_id[1],
                 &dob->persistent_id[1],
                 sizeof(dob->persistent_id[1]) * (MAX_DUPLI_RECUR - 1));
        }
        else {
          dob_key.persistent_id[0] = dob->persistent_id[0];
        }
        ob_dst_par = static_cast<Object *>(BLI_ghash_lookup(parent_gh, &dob_key));
      }

      if (ob_dst_par) {
        /* allow for all possible parent types */
        ob_dst->partype = ob_src->partype;
        BLI_strncpy(ob_dst->parsubstr, ob_src->parsubstr, sizeof(ob_dst->parsubstr));
        ob_dst->par1 = ob_src->par1;
        ob_dst->par2 = ob_src->par2;
        ob_dst->par3 = ob_src->par3;

        copy_m4_m4(ob_dst->parentinv, ob_src->parentinv);

        ob_dst->parent = ob_dst_par;
      }
    }
    if (use_base_parent && ob_dst->parent == nullptr) {
      Object *ob_dst_par = nullptr;

      if (instancer_gh != nullptr) {
        /* OK to keep most of the members uninitialized,
         * they won't be read, this is simply for a hash lookup. */
        DupliObject dob_key;
        /* We are looking one step upper in hierarchy, so we need to 'shift' the `persistent_id`,
         * ignoring the first item.
         * We only check on persistent_id here, since we have no idea what object it might be. */
        memcpy(&dob_key.persistent_id[0],
               &dob->persistent_id[1],
               sizeof(dob_key.persistent_id[0]) * (MAX_DUPLI_RECUR - 1));
        ob_dst_par = static_cast<Object *>(BLI_ghash_lookup(instancer_gh, &dob_key));
      }

      if (ob_dst_par == nullptr) {
        /* Default to parenting to root object...
         * Always the case when use_hierarchy is false. */
        ob_dst_par = base->object;
      }

      ob_dst->parent = ob_dst_par;
      ob_dst->partype = PAROBJECT;
    }

    if (ob_dst->parent) {
      /* NOTE: this may be the parent of other objects, but it should
       * still work out ok */
      BKE_object_apply_mat4(ob_dst, dob->mat, false, true);

      /* to set ob_dst->orig and in case there's any other discrepancies */
      DEG_id_tag_update(&ob_dst->id, ID_RECALC_TRANSFORM);
    }
  }

  if (base->object->transflag & OB_DUPLICOLLECTION && base->object->instance_collection) {
    base->object->instance_collection = nullptr;
  }

  ED_object_base_select(base, BA_DESELECT);
  DEG_id_tag_update(&base->object->id, ID_RECALC_SELECT);

  BLI_ghash_free(dupli_gh, nullptr, nullptr);
  if (parent_gh) {
    BLI_ghash_free(parent_gh, nullptr, nullptr);
  }
  if (instancer_gh) {
    BLI_ghash_free(instancer_gh, nullptr, nullptr);
  }

  free_object_duplilist(lb_duplis);

  BKE_main_id_newptr_and_tag_clear(bmain);

  base->object->transflag &= ~OB_DUPLI;
  DEG_id_tag_update(&base->object->id, ID_RECALC_COPY_ON_WRITE);
}

static int object_duplicates_make_real_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene = CTX_data_scene(C);

  const bool use_base_parent = RNA_boolean_get(op->ptr, "use_base_parent");
  const bool use_hierarchy = RNA_boolean_get(op->ptr, "use_hierarchy");

  BKE_main_id_newptr_and_tag_clear(bmain);

  CTX_DATA_BEGIN (C, Base *, base, selected_editable_bases) {
    make_object_duplilist_real(C, depsgraph, scene, base, use_base_parent, use_hierarchy);

    /* dependencies were changed */
    WM_event_add_notifier(C, NC_OBJECT | ND_PARENT, base->object);
  }
  CTX_DATA_END;

  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_SCENE, scene);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_duplicates_make_real(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Instances Real";
  ot->description = "Make instanced objects attached to this object real";
  ot->idname = "OBJECT_OT_duplicates_make_real";

  /* api callbacks */
  ot->exec = object_duplicates_make_real_exec;

  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "use_base_parent",
                  false,
                  "Parent",
                  "Parent newly created objects to the original instancer");
  RNA_def_boolean(
      ot->srna, "use_hierarchy", false, "Keep Hierarchy", "Maintain parent child relationships");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data Convert Operator
 * \{ */

static const EnumPropertyItem convert_target_items[] = {
    {OB_CURVES_LEGACY,
     "CURVE",
     ICON_OUTLINER_OB_CURVE,
     "Curve",
     "Curve from Mesh or Text objects"},
    {OB_MESH,
     "MESH",
     ICON_OUTLINER_OB_MESH,
     "Mesh",
#ifdef WITH_POINT_CLOUD
     "Mesh from Curve, Surface, Metaball, Text, or Point Cloud objects"},
#else
     "Mesh from Curve, Surface, Metaball, or Text objects"},
#endif
    {OB_GPENCIL,
     "GPENCIL",
     ICON_OUTLINER_OB_GREASEPENCIL,
     "Grease Pencil",
     "Grease Pencil from Curve or Mesh objects"},
#ifdef WITH_POINT_CLOUD
    {OB_POINTCLOUD,
     "POINTCLOUD",
     ICON_OUTLINER_OB_POINTCLOUD,
     "Point Cloud",
     "Point Cloud from Mesh objects"},
#endif
    {OB_CURVES, "CURVES", ICON_OUTLINER_OB_CURVES, "Curves", "Curves from evaluated curve data"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void object_data_convert_curve_to_mesh(Main *bmain, Depsgraph *depsgraph, Object *ob)
{
  Object *object_eval = DEG_get_evaluated_object(depsgraph, ob);
  Curve *curve = static_cast<Curve *>(ob->data);

  Mesh *mesh = BKE_mesh_new_from_object_to_bmain(bmain, depsgraph, object_eval, true);
  if (mesh == nullptr) {
    /* Unable to convert the curve to a mesh. */
    return;
  }

  BKE_object_free_modifiers(ob, 0);

  if (ob->type == OB_MESH) {
    /* UPBGE defaults for mesh objects */
    ob->body_type = OB_BODY_TYPE_STATIC;
    ob->gameflag = OB_PROP | OB_COLLISION;
  }

  /* Replace curve used by the object itself. */
  ob->data = mesh;
  ob->type = OB_MESH;
  id_us_min(&curve->id);
  id_us_plus(&mesh->id);
  /* Change objects which are using same curve.
   * A bit annoying, but:
   * - It's possible to have multiple curve objects selected which are sharing the same curve
   *   data-block. We don't want mesh to be created for every of those objects.
   * - This is how conversion worked for a long time. */
  LISTBASE_FOREACH (Object *, other_object, &bmain->objects) {
    if (other_object->data == curve) {
      other_object->type = OB_MESH;

      id_us_min((ID *)other_object->data);
      other_object->data = ob->data;
      id_us_plus((ID *)other_object->data);
    }
  }
}

static bool object_convert_poll(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  Base *base_act = CTX_data_active_base(C);
  Object *obact = base_act ? base_act->object : nullptr;

  if (obact == nullptr || obact->data == nullptr || ID_IS_LINKED(obact) ||
      ID_IS_OVERRIDE_LIBRARY(obact) || ID_IS_OVERRIDE_LIBRARY(obact->data)) {
    return false;
  }

  return (!ID_IS_LINKED(scene) && (BKE_object_is_in_editmode(obact) == false) &&
          (base_act->flag & BASE_SELECTED));
}

/* Helper for object_convert_exec */
static Base *duplibase_for_convert(
    Main *bmain, Depsgraph *depsgraph, Scene *scene, ViewLayer *view_layer, Base *base, Object *ob)
{
  if (ob == nullptr) {
    ob = base->object;
  }

  Object *obn = (Object *)BKE_id_copy(bmain, &ob->id);
  id_us_min(&obn->id);
  DEG_id_tag_update(&obn->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
  BKE_collection_object_add_from(bmain, scene, ob, obn);

  Base *basen = BKE_view_layer_base_find(view_layer, obn);
  ED_object_base_select(basen, BA_SELECT);
  ED_object_base_select(base, BA_DESELECT);

  /* XXX: An ugly hack needed because if we re-run depsgraph with some new meta-ball objects
   * having same 'family name' as orig ones, they will affect end result of meta-ball computation.
   * For until we get rid of that name-based thingy in meta-balls, that should do the trick
   * (this is weak, but other solution (to change name of `obn`) is even worse IMHO).
   * See T65996. */
  const bool is_meta_ball = (obn->type == OB_MBALL);
  void *obdata = obn->data;
  if (is_meta_ball) {
    obn->type = OB_EMPTY;
    obn->data = nullptr;
  }

  /* XXX Doing that here is stupid, it means we update and re-evaluate the whole depsgraph every
   * time we need to duplicate an object to convert it. Even worse, this is not 100% correct, since
   * we do not yet have duplicated obdata.
   * However, that is a safe solution for now. Proper, longer-term solution is to refactor
   * object_convert_exec to:
   *  - duplicate all data it needs to in a first loop.
   *  - do a single update.
   *  - convert data in a second loop. */
  DEG_graph_tag_relations_update(depsgraph);
  CustomData_MeshMasks customdata_mask_prev = scene->customdata_mask;
  CustomData_MeshMasks_update(&scene->customdata_mask, &CD_MASK_MESH);
  BKE_scene_graph_update_tagged(depsgraph, bmain);
  scene->customdata_mask = customdata_mask_prev;

  if (is_meta_ball) {
    obn->type = OB_MBALL;
    obn->data = obdata;
  }

  return basen;
}

static int object_convert_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Base *basen = nullptr, *basact = nullptr;
  Object *ob1, *obact = CTX_data_active_object(C);
  const short target = RNA_enum_get(op->ptr, "target");
  bool keep_original = RNA_boolean_get(op->ptr, "keep_original");
  const bool do_merge_customdata = RNA_boolean_get(op->ptr, "merge_customdata");

  const float angle = RNA_float_get(op->ptr, "angle");
  const int thickness = RNA_int_get(op->ptr, "thickness");
  const bool use_seams = RNA_boolean_get(op->ptr, "seams");
  const bool use_faces = RNA_boolean_get(op->ptr, "faces");
  const float offset = RNA_float_get(op->ptr, "offset");

  int mballConverted = 0;
  bool gpencilConverted = false;
  bool gpencilCurveConverted = false;

  /* don't forget multiple users! */

  {
    FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
      ob->flag &= ~OB_DONE;

      /* flag data that's not been edited (only needed for !keep_original) */
      if (ob->data) {
        ((ID *)ob->data)->tag |= LIB_TAG_DOIT;
      }

      /* possible metaball basis is not in this scene */
      if (ob->type == OB_MBALL && target == OB_MESH) {
        if (BKE_mball_is_basis(ob) == false) {
          Object *ob_basis;
          ob_basis = BKE_mball_basis_find(scene, ob);
          if (ob_basis) {
            ob_basis->flag &= ~OB_DONE;
          }
        }
      }
    }
    FOREACH_SCENE_OBJECT_END;
  }

  ListBase selected_editable_bases;
  CTX_data_selected_editable_bases(C, &selected_editable_bases);

  /* Ensure we get all meshes calculated with a sufficient data-mask,
   * needed since re-evaluating single modifiers causes bugs if they depend
   * on other objects data masks too, see: T50950. */
  {
    LISTBASE_FOREACH (CollectionPointerLink *, link, &selected_editable_bases) {
      Base *base = static_cast<Base *>(link->ptr.data);
      Object *ob = base->object;

      /* The way object type conversion works currently (enforcing conversion of *all* objects
       * using converted object-data, even some un-selected/hidden/another scene ones,
       * sounds totally bad to me.
       * However, changing this is more design than bug-fix, not to mention convoluted code below,
       * so that will be for later.
       * But at the very least, do not do that with linked IDs! */
      if ((!BKE_id_is_editable(bmain, &ob->id) ||
           (ob->data && !BKE_id_is_editable(bmain, static_cast<ID *>(ob->data)))) &&
          !keep_original) {
        keep_original = true;
        BKE_report(op->reports,
                   RPT_INFO,
                   "Converting some non-editable object/object data, enforcing 'Keep Original' "
                   "option to True");
      }

      DEG_id_tag_update(&base->object->id, ID_RECALC_GEOMETRY);
    }

    CustomData_MeshMasks customdata_mask_prev = scene->customdata_mask;
    CustomData_MeshMasks_update(&scene->customdata_mask, &CD_MASK_MESH);
    BKE_scene_graph_update_tagged(depsgraph, bmain);
    scene->customdata_mask = customdata_mask_prev;
  }

  LISTBASE_FOREACH (CollectionPointerLink *, link, &selected_editable_bases) {
    Object *newob = nullptr;
    Base *base = static_cast<Base *>(link->ptr.data);
    Object *ob = base->object;

    if (ob->flag & OB_DONE || !IS_TAGGED(ob->data)) {
      if (ob->type != target) {
        base->flag &= ~SELECT;
        ob->flag &= ~SELECT;
      }

      /* obdata already modified */
      if (!IS_TAGGED(ob->data)) {
        /* When 2 objects with linked data are selected, converting both
         * would keep modifiers on all but the converted object T26003. */
        if (ob->type == OB_MESH) {
          BKE_object_free_modifiers(ob, 0); /* after derivedmesh calls! */
        }
        if (ob->type == OB_GPENCIL) {
          BKE_object_free_modifiers(ob, 0); /* after derivedmesh calls! */
          BKE_object_free_shaderfx(ob, 0);
        }
      }
    }
    else if (ob->type == OB_MESH && target == OB_CURVES_LEGACY) {
      ob->flag |= OB_DONE;

      if (keep_original) {
        basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, nullptr);
        newob = basen->object;

        /* Decrement original mesh's usage count. */
        Mesh *me = static_cast<Mesh *>(newob->data);
        id_us_min(&me->id);

        /* Make a new copy of the mesh. */
        newob->data = BKE_id_copy(bmain, &me->id);
      }
      else {
        newob = ob;
      }

      BKE_mesh_to_curve(bmain, depsgraph, scene, newob);

      if (newob->type == OB_CURVES_LEGACY) {
        BKE_object_free_modifiers(newob, 0); /* after derivedmesh calls! */
        if (newob->rigidbody_object != nullptr) {
          ED_rigidbody_object_remove(bmain, scene, newob);
        }
      }
    }
    else if (ob->type == OB_MESH && target == OB_GPENCIL) {
      ob->flag |= OB_DONE;

      /* Create a new grease pencil object and copy transformations. */
      ushort local_view_bits = (v3d && v3d->localvd) ? v3d->local_view_uuid : 0;
      float loc[3], size[3], rot[3][3], eul[3];
      float matrix[4][4];
      mat4_to_loc_rot_size(loc, rot, size, ob->obmat);
      mat3_to_eul(eul, rot);

      Object *ob_gpencil = ED_gpencil_add_object(C, loc, local_view_bits);
      copy_v3_v3(ob_gpencil->loc, loc);
      copy_v3_v3(ob_gpencil->rot, eul);
      copy_v3_v3(ob_gpencil->scale, size);
      unit_m4(matrix);
      /* Set object in 3D mode. */
      bGPdata *gpd = (bGPdata *)ob_gpencil->data;
      gpd->draw_mode = GP_DRAWMODE_3D;

      gpencilConverted |= BKE_gpencil_convert_mesh(bmain,
                                                   depsgraph,
                                                   scene,
                                                   ob_gpencil,
                                                   ob,
                                                   angle,
                                                   thickness,
                                                   offset,
                                                   matrix,
                                                   0,
                                                   use_seams,
                                                   use_faces,
                                                   true);

      /* Remove unused materials. */
      int actcol = ob_gpencil->actcol;
      for (int slot = 1; slot <= ob_gpencil->totcol; slot++) {
        while (slot <= ob_gpencil->totcol && !BKE_object_material_slot_used(ob_gpencil, slot)) {
          ob_gpencil->actcol = slot;
          BKE_object_material_slot_remove(CTX_data_main(C), ob_gpencil);

          if (actcol >= slot) {
            actcol--;
          }
        }
      }
      ob_gpencil->actcol = actcol;
    }
    else if (target == OB_CURVES) {
      ob->flag |= OB_DONE;

      Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
      GeometrySet geometry;
      if (ob_eval->runtime.geometry_set_eval != nullptr) {
        geometry = *ob_eval->runtime.geometry_set_eval;
      }

      if (geometry.has_curves()) {
        if (keep_original) {
          basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, nullptr);
          newob = basen->object;

          /* Decrement original curve's usage count. */
          Curve *legacy_curve = static_cast<Curve *>(newob->data);
          id_us_min(&legacy_curve->id);

          /* Make a copy of the curve. */
          newob->data = BKE_id_copy(bmain, &legacy_curve->id);
        }
        else {
          newob = ob;
        }

        const CurveComponent &curve_component = *geometry.get_component_for_read<CurveComponent>();
        const Curves *curves_eval = curve_component.get_for_read();
        Curves *new_curves = static_cast<Curves *>(BKE_id_new(bmain, ID_CV, newob->id.name + 2));

        newob->data = new_curves;
        newob->type = OB_CURVES;

        blender::bke::CurvesGeometry::wrap(
            new_curves->geometry) = blender::bke::CurvesGeometry::wrap(curves_eval->geometry);
        BKE_object_material_from_eval_data(bmain, newob, &curves_eval->id);

        BKE_object_free_derived_caches(newob);
        BKE_object_free_modifiers(newob, 0);
      }
      else {
        BKE_reportf(
            op->reports, RPT_WARNING, "Object '%s' has no evaluated curves data", ob->id.name + 2);
      }
    }
    else if (ob->type == OB_MESH && target == OB_POINTCLOUD) {
      ob->flag |= OB_DONE;

      if (keep_original) {
        basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, nullptr);
        newob = basen->object;

        /* Decrement original mesh's usage count. */
        Mesh *me = static_cast<Mesh *>(newob->data);
        id_us_min(&me->id);

        /* Make a new copy of the mesh. */
        newob->data = BKE_id_copy(bmain, &me->id);
      }
      else {
        newob = ob;
      }

      BKE_mesh_to_pointcloud(bmain, depsgraph, scene, newob);

      if (newob->type == OB_POINTCLOUD) {
        BKE_object_free_modifiers(newob, 0); /* after derivedmesh calls! */
        ED_rigidbody_object_remove(bmain, scene, newob);
      }
    }
    else if (ob->type == OB_MESH) {
      ob->flag |= OB_DONE;

      if (keep_original) {
        basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, nullptr);
        newob = basen->object;

        /* Decrement original mesh's usage count. */
        Mesh *me = static_cast<Mesh *>(newob->data);
        id_us_min(&me->id);

        /* Make a new copy of the mesh. */
        newob->data = BKE_id_copy(bmain, &me->id);
      }
      else {
        newob = ob;
        DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
      }

      /* make new mesh data from the original copy */
      /* NOTE: get the mesh from the original, not from the copy in some
       * cases this doesn't give correct results (when MDEF is used for eg)
       */
      Scene *scene_eval = (Scene *)DEG_get_evaluated_id(depsgraph, &scene->id);
      Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
      Mesh *me_eval = mesh_get_eval_final(depsgraph, scene_eval, ob_eval, &CD_MASK_MESH);
      me_eval = BKE_mesh_copy_for_eval(me_eval, false);
      /* Full (edge-angle based) draw calculation should ideally be performed. */
      BKE_mesh_edges_set_draw_render(me_eval);
      BKE_object_material_from_eval_data(bmain, newob, &me_eval->id);
      Mesh *new_mesh = (Mesh *)newob->data;
      BKE_mesh_nomain_to_mesh(me_eval, new_mesh, newob, &CD_MASK_MESH, true);

      if (do_merge_customdata) {
        BKE_mesh_merge_customdata_for_apply_modifier(new_mesh);
      }

      /* Anonymous attributes shouldn't be available on the applied geometry. */
      blender::bke::mesh_attributes_for_write(*new_mesh).remove_anonymous();

      BKE_object_free_modifiers(newob, 0); /* after derivedmesh calls! */
    }
    else if (ob->type == OB_FONT) {
      ob->flag |= OB_DONE;

      if (keep_original) {
        basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, nullptr);
        newob = basen->object;

        /* Decrement original curve's usage count. */
        id_us_min(&((Curve *)newob->data)->id);

        /* Make a new copy of the curve. */
        newob->data = BKE_id_copy(bmain, static_cast<ID *>(ob->data));
      }
      else {
        newob = ob;
      }

      Curve *cu = static_cast<Curve *>(newob->data);

      Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
      BKE_vfont_to_curve_ex(ob_eval,
                            static_cast<Curve *>(ob_eval->data),
                            FO_EDIT,
                            &cu->nurb,
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr);

      newob->type = OB_CURVES_LEGACY;
      cu->type = OB_CURVES_LEGACY;

      if (cu->vfont) {
        id_us_min(&cu->vfont->id);
        cu->vfont = nullptr;
      }
      if (cu->vfontb) {
        id_us_min(&cu->vfontb->id);
        cu->vfontb = nullptr;
      }
      if (cu->vfonti) {
        id_us_min(&cu->vfonti->id);
        cu->vfonti = nullptr;
      }
      if (cu->vfontbi) {
        id_us_min(&cu->vfontbi->id);
        cu->vfontbi = nullptr;
      }

      if (!keep_original) {
        /* other users */
        if (ID_REAL_USERS(&cu->id) > 1) {
          for (ob1 = static_cast<Object *>(bmain->objects.first); ob1;
               ob1 = static_cast<Object *>(ob1->id.next)) {
            if (ob1->data == ob->data) {
              ob1->type = OB_CURVES_LEGACY;
              DEG_id_tag_update(&ob1->id,
                                ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);
            }
          }
        }
      }

      LISTBASE_FOREACH (Nurb *, nu, &cu->nurb) {
        nu->charidx = 0;
      }

      cu->flag &= ~CU_3D;
      BKE_curve_dimension_update(cu);

      if (target == OB_MESH) {
        /* No assumption should be made that the resulting objects is a mesh, as conversion can
         * fail. */
        object_data_convert_curve_to_mesh(bmain, depsgraph, newob);
        /* Meshes doesn't use the "curve cache". */
        BKE_object_free_curve_cache(newob);
      }
      else if (target == OB_GPENCIL) {
        ushort local_view_bits = (v3d && v3d->localvd) ? v3d->local_view_uuid : 0;
        Object *ob_gpencil = ED_gpencil_add_object(C, newob->loc, local_view_bits);
        copy_v3_v3(ob_gpencil->rot, newob->rot);
        copy_v3_v3(ob_gpencil->scale, newob->scale);
        BKE_gpencil_convert_curve(bmain, scene, ob_gpencil, newob, false, 1.0f, 0.0f);
        gpencilConverted = true;
        gpencilCurveConverted = true;
        basen = nullptr;
      }
    }
    else if (ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF)) {
      ob->flag |= OB_DONE;

      if (target == OB_MESH) {
        if (keep_original) {
          basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, nullptr);
          newob = basen->object;

          /* Decrement original curve's usage count. */
          id_us_min(&((Curve *)newob->data)->id);

          /* make a new copy of the curve */
          newob->data = BKE_id_copy(bmain, static_cast<ID *>(ob->data));
        }
        else {
          newob = ob;
        }

        /* No assumption should be made that the resulting objects is a mesh, as conversion can
         * fail. */
        object_data_convert_curve_to_mesh(bmain, depsgraph, newob);
        /* Meshes don't use the "curve cache". */
        BKE_object_free_curve_cache(newob);
      }
      else if (target == OB_GPENCIL) {
        if (ob->type != OB_CURVES_LEGACY) {
          ob->flag &= ~OB_DONE;
          BKE_report(op->reports, RPT_ERROR, "Convert Surfaces to Grease Pencil is not supported");
        }
        else {
          /* Create a new grease pencil object and copy transformations.
           * Nurbs Surface are not supported.
           */
          ushort local_view_bits = (v3d && v3d->localvd) ? v3d->local_view_uuid : 0;
          Object *ob_gpencil = ED_gpencil_add_object(C, ob->loc, local_view_bits);
          copy_v3_v3(ob_gpencil->rot, ob->rot);
          copy_v3_v3(ob_gpencil->scale, ob->scale);
          BKE_gpencil_convert_curve(bmain, scene, ob_gpencil, ob, false, 1.0f, 0.0f);
          gpencilConverted = true;
        }
      }
    }
    else if (ob->type == OB_MBALL && target == OB_MESH) {
      Object *baseob;

      base->flag &= ~BASE_SELECTED;
      ob->base_flag &= ~BASE_SELECTED;

      baseob = BKE_mball_basis_find(scene, ob);

      if (ob != baseob) {
        /* if motherball is converting it would be marked as done later */
        ob->flag |= OB_DONE;
      }

      if (!(baseob->flag & OB_DONE)) {
        basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, baseob);
        newob = basen->object;

        MetaBall *mb = static_cast<MetaBall *>(newob->data);
        id_us_min(&mb->id);

        /* Find the evaluated mesh of the basis metaball object. */
        Object *object_eval = DEG_get_evaluated_object(depsgraph, baseob);
        Mesh *mesh = BKE_mesh_new_from_object_to_bmain(bmain, depsgraph, object_eval, true);

        id_us_plus(&mesh->id);
        newob->data = mesh;
        newob->type = OB_MESH;

        if (obact->type == OB_MBALL) {
          basact = basen;
        }

        baseob->flag |= OB_DONE;
        mballConverted = 1;
      }
    }
    else if (ob->type == OB_POINTCLOUD && target == OB_MESH) {
      ob->flag |= OB_DONE;

      if (keep_original) {
        basen = duplibase_for_convert(bmain, depsgraph, scene, view_layer, base, nullptr);
        newob = basen->object;

        /* Decrement original point cloud's usage count. */
        PointCloud *pointcloud = static_cast<PointCloud *>(newob->data);
        id_us_min(&pointcloud->id);

        /* Make a new copy of the point cloud. */
        newob->data = BKE_id_copy(bmain, &pointcloud->id);
      }
      else {
        newob = ob;
      }

      BKE_pointcloud_to_mesh(bmain, depsgraph, scene, newob);

      if (newob->type == OB_MESH) {
        BKE_object_free_modifiers(newob, 0); /* after derivedmesh calls! */
        ED_rigidbody_object_remove(bmain, scene, newob);
      }
    }
    else {
      continue;
    }

    /* Ensure new object has consistent material data with its new obdata. */
    if (newob) {
      BKE_object_materials_test(bmain, newob, static_cast<ID *>(newob->data));
    }

    /* tag obdata if it was been changed */

    /* If the original object is active then make this object active */
    if (basen) {
      if (ob == obact) {
        /* store new active base to update BASACT */
        basact = basen;
      }

      basen = nullptr;
    }

    if (!keep_original && (ob->flag & OB_DONE)) {
      /* NOTE: Tag transform for update because object parenting to curve with path is handled
       * differently from all other cases. Converting curve to mesh and mesh to curve will likely
       * affect the way children are evaluated.
       * It is not enough to tag only geometry and rely on the curve parenting relations because
       * this relation is lost when curve is converted to mesh. */
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY | ID_RECALC_TRANSFORM);
      ((ID *)ob->data)->tag &= ~LIB_TAG_DOIT; /* flag not to convert this datablock again */
    }
  }
  BLI_freelistN(&selected_editable_bases);

  if (!keep_original) {
    if (mballConverted) {
      /* We need to remove non-basis MBalls first, otherwise we won't be able to detect them if
       * their basis happens to be removed first. */
      FOREACH_SCENE_OBJECT_BEGIN (scene, ob_mball) {
        if (ob_mball->type == OB_MBALL) {
          Object *ob_basis = nullptr;
          if (!BKE_mball_is_basis(ob_mball) &&
              ((ob_basis = BKE_mball_basis_find(scene, ob_mball)) && (ob_basis->flag & OB_DONE))) {
            ED_object_base_free_and_unlink(bmain, scene, ob_mball);
          }
        }
      }
      FOREACH_SCENE_OBJECT_END;
      FOREACH_SCENE_OBJECT_BEGIN (scene, ob_mball) {
        if (ob_mball->type == OB_MBALL) {
          if (ob_mball->flag & OB_DONE) {
            if (BKE_mball_is_basis(ob_mball)) {
              ED_object_base_free_and_unlink(bmain, scene, ob_mball);
            }
          }
        }
      }
      FOREACH_SCENE_OBJECT_END;
    }
    /* Remove curves and meshes converted to Grease Pencil object. */
    if (gpencilConverted) {
      FOREACH_SCENE_OBJECT_BEGIN (scene, ob_delete) {
        if (ELEM(ob_delete->type, OB_CURVES_LEGACY, OB_MESH)) {
          if (ob_delete->flag & OB_DONE) {
            ED_object_base_free_and_unlink(bmain, scene, ob_delete);
          }
        }
      }
      FOREACH_SCENE_OBJECT_END;
    }
  }
  else {
    /* Remove Text curves converted to Grease Pencil object to avoid duplicated curves. */
    if (gpencilCurveConverted) {
      FOREACH_SCENE_OBJECT_BEGIN (scene, ob_delete) {
        if (ELEM(ob_delete->type, OB_CURVES_LEGACY) && (ob_delete->flag & OB_DONE)) {
          ED_object_base_free_and_unlink(bmain, scene, ob_delete);
        }
      }
      FOREACH_SCENE_OBJECT_END;
    }
  }

  // XXX  ED_object_editmode_enter(C, 0);
  // XXX  exit_editmode(C, EM_FREEDATA|); /* free data, but no undo */

  if (basact) {
    /* active base was changed */
    ED_object_base_activate(C, basact);
    BASACT(view_layer) = basact;
  }
  else if (BASACT(view_layer)->object->flag & OB_DONE) {
    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, BASACT(view_layer)->object);
    WM_event_add_notifier(C, NC_OBJECT | ND_DATA, BASACT(view_layer)->object);
  }

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  return OPERATOR_FINISHED;
}

static void object_convert_ui(bContext *UNUSED(C), wmOperator *op)
{
  uiLayout *layout = op->layout;

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, op->ptr, "target", 0, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "keep_original", 0, nullptr, ICON_NONE);

  const int target = RNA_enum_get(op->ptr, "target");
  if (target == OB_MESH) {
    uiItemR(layout, op->ptr, "merge_customdata", 0, nullptr, ICON_NONE);
  }
  else if (target == OB_GPENCIL) {
    uiItemR(layout, op->ptr, "thickness", 0, nullptr, ICON_NONE);
    uiItemR(layout, op->ptr, "angle", 0, nullptr, ICON_NONE);
    uiItemR(layout, op->ptr, "offset", 0, nullptr, ICON_NONE);
    uiItemR(layout, op->ptr, "seams", 0, nullptr, ICON_NONE);
    uiItemR(layout, op->ptr, "faces", 0, nullptr, ICON_NONE);
  }
}

void OBJECT_OT_convert(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Convert To";
  ot->description = "Convert selected objects to another type";
  ot->idname = "OBJECT_OT_convert";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_convert_exec;
  ot->poll = object_convert_poll;
  ot->ui = object_convert_ui;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(
      ot->srna, "target", convert_target_items, OB_MESH, "Target", "Type of object to convert to");
  RNA_def_boolean(ot->srna,
                  "keep_original",
                  false,
                  "Keep Original",
                  "Keep original objects instead of replacing them");

  RNA_def_boolean(
      ot->srna,
      "merge_customdata",
      true,
      "Merge UV's",
      "Merge UV coordinates that share a vertex to account for imprecision in some modifiers");

  prop = RNA_def_float_rotation(ot->srna,
                                "angle",
                                0,
                                nullptr,
                                DEG2RADF(0.0f),
                                DEG2RADF(180.0f),
                                "Threshold Angle",
                                "Threshold to determine ends of the strokes",
                                DEG2RADF(0.0f),
                                DEG2RADF(180.0f));
  RNA_def_property_float_default(prop, DEG2RADF(70.0f));

  RNA_def_int(ot->srna, "thickness", 5, 1, 100, "Thickness", "", 1, 100);
  RNA_def_boolean(ot->srna, "seams", false, "Only Seam Edges", "Convert only seam edges");
  RNA_def_boolean(ot->srna, "faces", true, "Export Faces", "Export faces as filled strokes");
  RNA_def_float_distance(ot->srna,
                         "offset",
                         0.01f,
                         0.0,
                         OBJECT_ADD_SIZE_MAXF,
                         "Stroke Offset",
                         "Offset strokes from fill",
                         0.0,
                         100.00);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Object Operator
 * \{ */

/**
 * - Assumes `id.new` is correct.
 * - Leaves selection of base/object unaltered.
 * - Sets #ID.newid pointers.
 */
static Base *object_add_duplicate_internal(Main *bmain,
                                           Scene *scene,
                                           ViewLayer *view_layer,
                                           Object *ob,
                                           const eDupli_ID_Flags dupflag,
                                           const eLibIDDuplicateFlags duplicate_options,
                                           Object **r_ob_new)
{
  Base *base, *basen = nullptr;
  Object *obn;

  if (ob->mode & OB_MODE_POSE) {
    /* nothing? */
  }
  else {
    obn = static_cast<Object *>(
        ID_NEW_SET(ob, BKE_object_duplicate(bmain, ob, dupflag, duplicate_options)));
    if (r_ob_new) {
      *r_ob_new = obn;
    }
    DEG_id_tag_update(&obn->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);

    base = BKE_view_layer_base_find(view_layer, ob);
    if ((base != nullptr) && (base->flag & BASE_VISIBLE_DEPSGRAPH)) {
      BKE_collection_object_add_from(bmain, scene, ob, obn);
    }
    else {
      LayerCollection *layer_collection = BKE_layer_collection_get_active(view_layer);
      BKE_collection_object_add(bmain, layer_collection->collection, obn);
    }

    basen = BKE_view_layer_base_find(view_layer, obn);
    if (base != nullptr && basen != nullptr) {
      basen->local_view_bits = base->local_view_bits;
    }

    /* 1) duplis should end up in same collection as the original
     * 2) Rigid Body sim participants MUST always be part of a collection...
     */
    /* XXX: is 2) really a good measure here? */
    if (ob->rigidbody_object || ob->rigidbody_constraint) {
      LISTBASE_FOREACH (Collection *, collection, &bmain->collections) {
        if (BKE_collection_has_object(collection, ob)) {
          BKE_collection_object_add(bmain, collection, obn);
        }
      }
    }
  }
  return basen;
}

Base *ED_object_add_duplicate(
    Main *bmain, Scene *scene, ViewLayer *view_layer, Base *base, const eDupli_ID_Flags dupflag)
{
  Base *basen;
  Object *ob;

  BKE_sca_clear_new_points(); /* BGE logic */

  basen = object_add_duplicate_internal(bmain,
                                        scene,
                                        view_layer,
                                        base->object,
                                        dupflag,
                                        LIB_ID_DUPLICATE_IS_SUBPROCESS |
                                            LIB_ID_DUPLICATE_IS_ROOT_ID,
                                        nullptr);
  if (basen == nullptr) {
    return nullptr;
  }

  ob = basen->object;

  /* Link own references to the newly duplicated data T26816.
   * Note that this function can be called from edit-mode code, in which case we may have to
   * enforce remapping obdata (by default this is forbidden in edit mode). */
  const int remap_flag = BKE_object_is_in_editmode(ob) ? ID_REMAP_FORCE_OBDATA_IN_EDITMODE : 0;
  BKE_libblock_relink_to_newid(bmain, &ob->id, remap_flag);

  BKE_sca_set_new_points_ob(ob);

  /* Correct but the caller must do this. */
  // DAG_relations_tag_update(bmain);

  if (ob->data != nullptr) {
    DEG_id_tag_update_ex(bmain, (ID *)ob->data, ID_RECALC_EDITORS);
  }

  BKE_main_id_newptr_and_tag_clear(bmain);

  return basen;
}

/* contextual operator dupli */
static int duplicate_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool linked = RNA_boolean_get(op->ptr, "linked");
  const eDupli_ID_Flags dupflag = (linked) ? (eDupli_ID_Flags)0 : (eDupli_ID_Flags)U.dupflag;

  /* We need to handle that here ourselves, because we may duplicate several objects, in which case
   * we also want to remap pointers between those... */
  BKE_main_id_newptr_and_tag_clear(bmain);

  BKE_sca_clear_new_points(); /* BGE logic */

  /* Do not do collection re-syncs for each object; will do it once afterwards.
   * However this means we can't get to new duplicated Base's immediately, will
   * have to process them after the sync. */
  BKE_layer_collection_resync_forbid();

  /* Duplicate the selected objects, remember data needed to process
   * after the sync (the base of the original object, and the copy of the
   * original object). */
  blender::Vector<std::pair<Base *, Object *>> source_bases_new_objects;
  Object *ob_new_active = nullptr;

  CTX_DATA_BEGIN (C, Base *, base, selected_bases) {
    Object *ob_new = NULL;
    object_add_duplicate_internal(bmain,
                                  scene,
                                  view_layer,
                                  base->object,
                                  dupflag,
                                  LIB_ID_DUPLICATE_IS_SUBPROCESS | LIB_ID_DUPLICATE_IS_ROOT_ID,
                                  &ob_new);
    if (ob_new == nullptr) {
      continue;
    }
    source_bases_new_objects.append({base, ob_new});

    /* note that this is safe to do with this context iterator,
     * the list is made in advance */
    ED_object_base_select(base, BA_DESELECT);

    /* new object will become active */
    if (BASACT(view_layer) == base) {
      ob_new_active = ob_new;
    }
  }
  CTX_DATA_END;
  BKE_layer_collection_resync_allow();

  if (source_bases_new_objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* Sync the collection now, after everything is duplicated. */
  BKE_main_collection_sync(bmain);

  /* After sync we can get to the new Base data, process it here. */
  for (const auto &item : source_bases_new_objects) {
    Object *ob_new = item.second;
    Base *base_source = item.first;
    Base *base_new = BKE_view_layer_base_find(view_layer, ob_new);
    if (base_new == nullptr) {
      continue;
    }
    ED_object_base_select(base_new, BA_SELECT);
    if (ob_new == ob_new_active) {
      ED_object_base_activate(C, base_new);
    }
    if (base_new->object->data) {
      DEG_id_tag_update(static_cast<ID *>(base_new->object->data), 0);
    }
    /* #object_add_duplicate_internal will not have done this, since
     * before the collection sync it would not have found the new base yet. */
    base_new->local_view_bits = base_source->local_view_bits;
  }

  /* Note that this will also clear newid pointers and tags. */
  copy_object_set_idnew(C);

  ED_outliner_select_sync_from_object_tag(C);

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&scene->id, ID_RECALC_COPY_ON_WRITE | ID_RECALC_SELECT);

  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_duplicate(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Duplicate Objects";
  ot->description = "Duplicate selected objects";
  ot->idname = "OBJECT_OT_duplicate";

  /* api callbacks */
  ot->exec = duplicate_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* to give to transform */
  prop = RNA_def_boolean(ot->srna,
                         "linked",
                         false,
                         "Linked",
                         "Duplicate object but not object data, linking to the original data");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_enum(
      ot->srna, "mode", rna_enum_transform_mode_types, TFM_TRANSLATION, "Mode", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Named Object Operator
 *
 * Use for drag & drop.
 * \{ */

static int object_add_named_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool linked = RNA_boolean_get(op->ptr, "linked");
  const eDupli_ID_Flags dupflag = (linked) ? (eDupli_ID_Flags)0 : (eDupli_ID_Flags)U.dupflag;

  /* Find object, create fake base. */

  Object *ob = reinterpret_cast<Object *>(
      WM_operator_properties_id_lookup_from_name_or_session_uuid(bmain, op->ptr, ID_OB));

  if (ob == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Object not found");
    return OPERATOR_CANCELLED;
  }

  BKE_sca_clear_new_points(); /* BGE logic */

  /* prepare dupli */
  Base *basen = object_add_duplicate_internal(
      bmain,
      scene,
      view_layer,
      ob,
      dupflag,
      /* Sub-process flag because the new-ID remapping (#BKE_libblock_relink_to_newid()) in this
       * function will only work if the object is already linked in the view layer, which is not
       * the case here. So we have to do the new-ID relinking ourselves
       * (#copy_object_set_idnew()).
       */
      LIB_ID_DUPLICATE_IS_SUBPROCESS | LIB_ID_DUPLICATE_IS_ROOT_ID,
      nullptr);

  if (basen == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Object could not be duplicated");
    return OPERATOR_CANCELLED;
  }

  basen->object->visibility_flag &= ~OB_HIDE_VIEWPORT;
  /* Do immediately, as #copy_object_set_idnew() below operates on visible objects. */
  BKE_base_eval_flags(basen);

  /* object_add_duplicate_internal() doesn't deselect other objects, unlike object_add_common() or
   * BKE_view_layer_base_deselect_all(). */
  ED_object_base_deselect_all(view_layer, nullptr, SEL_DESELECT);
  ED_object_base_select(basen, BA_SELECT);
  ED_object_base_activate(C, basen);

  copy_object_set_idnew(C);

  /* TODO(sergey): Only update relations for the current scene. */
  DEG_relations_tag_update(bmain);

  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  ED_outliner_select_sync_from_object_tag(C);

  PropertyRNA *prop_matrix = RNA_struct_find_property(op->ptr, "matrix");
  if (RNA_property_is_set(op->ptr, prop_matrix)) {
    Object *ob_add = basen->object;
    RNA_property_float_get_array(op->ptr, prop_matrix, &ob_add->obmat[0][0]);
    BKE_object_apply_mat4(ob_add, ob_add->obmat, true, true);

    DEG_id_tag_update(&ob_add->id, ID_RECALC_TRANSFORM);
  }
  else {
    int mval[2];
    if (object_add_drop_xy_get(C, op, &mval)) {
      ED_object_location_from_view(C, basen->object->loc);
      ED_view3d_cursor3d_position(C, mval, false, basen->object->loc);
    }
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_add_named(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Object";
  ot->description = "Add named object";
  ot->idname = "OBJECT_OT_add_named";

  /* api callbacks */
  ot->invoke = object_add_drop_xy_generic_invoke;
  ot->exec = object_add_named_exec;
  ot->poll = ED_operator_objectmode_poll_msg;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  RNA_def_boolean(ot->srna,
                  "linked",
                  false,
                  "Linked",
                  "Duplicate object but not object data, linking to the original data");

  WM_operator_properties_id_lookup(ot, true);

  prop = RNA_def_float_matrix(
      ot->srna, "matrix", 4, 4, nullptr, 0.0f, 0.0f, "Matrix", "", 0.0f, 0.0f);
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));

  object_add_drop_xy_props(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Object to Mouse Operator
 * \{ */

/**
 * Alternate behavior for dropping an asset that positions the appended object(s).
 */
static int object_transform_to_mouse_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  Object *ob = reinterpret_cast<Object *>(
      WM_operator_properties_id_lookup_from_name_or_session_uuid(bmain, op->ptr, ID_OB));

  if (!ob) {
    ob = OBACT(view_layer);
  }

  if (ob == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Object not found");
    return OPERATOR_CANCELLED;
  }

  /* Don't transform a linked object. There's just nothing to do here in this case, so return
   * #OPERATOR_FINISHED. */
  if (!BKE_id_is_editable(bmain, &ob->id)) {
    return OPERATOR_FINISHED;
  }

  /* Ensure the locations are updated so snap reads the evaluated active location. */
  CTX_data_ensure_evaluated_depsgraph(C);

  PropertyRNA *prop_matrix = RNA_struct_find_property(op->ptr, "matrix");
  if (RNA_property_is_set(op->ptr, prop_matrix)) {
    ObjectsInViewLayerParams params = {0};
    uint objects_len;
    Object **objects = BKE_view_layer_array_selected_objects_params(
        view_layer, nullptr, &objects_len, &params);

    float matrix[4][4];
    RNA_property_float_get_array(op->ptr, prop_matrix, &matrix[0][0]);

    float mat_src_unit[4][4];
    float mat_dst_unit[4][4];
    float final_delta[4][4];

    normalize_m4_m4(mat_src_unit, ob->obmat);
    normalize_m4_m4(mat_dst_unit, matrix);
    invert_m4(mat_src_unit);
    mul_m4_m4m4(final_delta, mat_dst_unit, mat_src_unit);

    ED_object_xform_array_m4(objects, objects_len, final_delta);

    MEM_freeN(objects);
  }
  else {
    int mval[2];
    if (object_add_drop_xy_get(C, op, &mval)) {
      float cursor[3];
      ED_object_location_from_view(C, cursor);
      ED_view3d_cursor3d_position(C, mval, false, cursor);

      /* Use the active objects location since this is the ID which the user selected to drop.
       *
       * This transforms all selected objects, so that dropping a single object which links in
       * other objects will have their relative transformation preserved.
       * For example a child/parent relationship or other objects used with a boolean modifier.
       *
       * The caller is responsible for ensuring the selection state gives useful results.
       * Link/append does this using #FILE_AUTOSELECT. */
      ED_view3d_snap_selected_to_location(C, cursor, V3D_AROUND_ACTIVE);
    }
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_transform_to_mouse(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Place Object Under Mouse";
  ot->description = "Snap selected item(s) to the mouse location";
  ot->idname = "OBJECT_OT_transform_to_mouse";

  /* api callbacks */
  ot->invoke = object_add_drop_xy_generic_invoke;
  ot->exec = object_transform_to_mouse_exec;
  ot->poll = ED_operator_objectmode_poll_msg;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_string(
      ot->srna,
      "name",
      nullptr,
      MAX_ID_NAME - 2,
      "Name",
      "Object name to place (uses the active object when this and 'session_uuid' are unset)");
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_SKIP_SAVE | PROP_HIDDEN));
  prop = RNA_def_int(ot->srna,
                     "session_uuid",
                     0,
                     INT32_MIN,
                     INT32_MAX,
                     "Session UUID",
                     "Session UUID of the object to place (uses the active object when this and "
                     "'name' are unset)",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_SKIP_SAVE | PROP_HIDDEN));

  prop = RNA_def_float_matrix(
      ot->srna, "matrix", 4, 4, nullptr, 0.0f, 0.0f, "Matrix", "", 0.0f, 0.0f);
  RNA_def_property_flag(prop, (PropertyFlag)(PROP_HIDDEN | PROP_SKIP_SAVE));

  object_add_drop_xy_props(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Join Object Operator
 * \{ */

static bool object_join_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);

  if (ob == nullptr || ob->data == nullptr || ID_IS_LINKED(ob) || ID_IS_OVERRIDE_LIBRARY(ob) ||
      ID_IS_OVERRIDE_LIBRARY(ob->data)) {
    return false;
  }

  if (ELEM(ob->type, OB_MESH, OB_CURVES_LEGACY, OB_SURF, OB_ARMATURE, OB_GPENCIL)) {
    return ED_operator_screenactive(C);
  }
  return false;
}

static int object_join_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);

  if (ob->mode & OB_MODE_EDIT) {
    BKE_report(op->reports, RPT_ERROR, "This data does not support joining in edit mode");
    return OPERATOR_CANCELLED;
  }
  if (BKE_object_obdata_is_libdata(ob)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot edit external library data");
    return OPERATOR_CANCELLED;
  }
  if (!BKE_lib_override_library_id_is_user_deletable(bmain, &ob->id)) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Cannot edit object '%s' as it is used by override collections",
                ob->id.name + 2);
    return OPERATOR_CANCELLED;
  }

  if (ob->type == OB_GPENCIL) {
    bGPdata *gpd = (bGPdata *)ob->data;
    if ((!gpd) || GPENCIL_ANY_MODE(gpd)) {
      BKE_report(op->reports, RPT_ERROR, "This data does not support joining in this mode");
      return OPERATOR_CANCELLED;
    }
  }

  int ret = OPERATOR_CANCELLED;
  if (ob->type == OB_MESH) {
    ret = ED_mesh_join_objects_exec(C, op);
  }
  else if (ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF)) {
    ret = ED_curve_join_objects_exec(C, op);
  }
  else if (ob->type == OB_ARMATURE) {
    ret = ED_armature_join_objects_exec(C, op);
  }
  else if (ob->type == OB_GPENCIL) {
    ret = ED_gpencil_join_objects_exec(C, op);
  }

  if (ret & OPERATOR_FINISHED) {
    /* Even though internally failure to invert is accounted for with a fallback,
     * show a warning since the result may not be what the user expects. See T80077.
     *
     * Failure to invert the matrix is typically caused by zero scaled axes
     * (which can be caused by constraints, even if the input scale isn't zero).
     *
     * Internally the join functions use #invert_m4_m4_safe_ortho which creates
     * an inevitable matrix from one that has one or more degenerate axes.
     *
     * In most cases we don't worry about special handling for non-inevitable matrices however for
     * joining objects there may be flat 2D objects where it's not obvious the scale is zero.
     * In this case, using #invert_m4_m4_safe_ortho works as well as we can expect,
     * joining the contents, flattening on the axis that's zero scaled.
     * If the zero scale is removed, the data on this axis remains un-scaled
     * (something that wouldn't work for #invert_m4_m4_safe). */
    float imat_test[4][4];
    if (!invert_m4_m4(imat_test, ob->obmat)) {
      BKE_report(op->reports,
                 RPT_WARNING,
                 "Active object final transform has one or more zero scaled axes");
    }
  }

  return ret;
}

void OBJECT_OT_join(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Join";
  ot->description = "Join selected objects into active object";
  ot->idname = "OBJECT_OT_join";

  /* api callbacks */
  ot->exec = object_join_exec;
  ot->poll = object_join_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Join as Shape Key Operator
 * \{ */

static bool join_shapes_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);

  if (ob == nullptr || ob->data == nullptr || ID_IS_LINKED(ob) || ID_IS_OVERRIDE_LIBRARY(ob) ||
      ID_IS_OVERRIDE_LIBRARY(ob->data)) {
    return false;
  }

  /* only meshes supported at the moment */
  if (ob->type == OB_MESH) {
    return ED_operator_screenactive(C);
  }
  return false;
}

static int join_shapes_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);

  if (ob->mode & OB_MODE_EDIT) {
    BKE_report(op->reports, RPT_ERROR, "This data does not support joining in edit mode");
    return OPERATOR_CANCELLED;
  }
  if (BKE_object_obdata_is_libdata(ob)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot edit external library data");
    return OPERATOR_CANCELLED;
  }
  if (!BKE_lib_override_library_id_is_user_deletable(bmain, &ob->id)) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Cannot edit object '%s' as it is used by override collections",
                ob->id.name + 2);
    return OPERATOR_CANCELLED;
  }

  if (ob->type == OB_MESH) {
    return ED_mesh_shapes_join_objects_exec(C, op);
  }

  return OPERATOR_CANCELLED;
}

void OBJECT_OT_join_shapes(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Join as Shapes";
  ot->description = "Copy the current resulting shape of another selected object to this one";
  ot->idname = "OBJECT_OT_join_shapes";

  /* api callbacks */
  ot->exec = join_shapes_exec;
  ot->poll = join_shapes_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */
