/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <stdlib.h>

#include "DNA_camera_types.h"

#include "BLI_math.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "rna_internal.h"

#include "WM_api.h"
#include "WM_types.h"

#ifdef RNA_RUNTIME

#  include "BKE_camera.h"
#  include "BKE_object.h"

#  include "DEG_depsgraph.h"
#  include "DEG_depsgraph_build.h"

#  include "SEQ_relations.h"

static float rna_Camera_angle_get(PointerRNA *ptr)
{
  Camera *cam = (Camera *)ptr->owner_id;
  float sensor = BKE_camera_sensor_size(cam->sensor_fit, cam->sensor_x, cam->sensor_y);
  return focallength_to_fov(cam->lens, sensor);
}

static void rna_Camera_angle_set(PointerRNA *ptr, float value)
{
  Camera *cam = (Camera *)ptr->owner_id;
  float sensor = BKE_camera_sensor_size(cam->sensor_fit, cam->sensor_x, cam->sensor_y);
  cam->lens = fov_to_focallength(value, sensor);
}

static float rna_Camera_angle_x_get(PointerRNA *ptr)
{
  Camera *cam = (Camera *)ptr->owner_id;
  return focallength_to_fov(cam->lens, cam->sensor_x);
}

static void rna_Camera_angle_x_set(PointerRNA *ptr, float value)
{
  Camera *cam = (Camera *)ptr->owner_id;
  cam->lens = fov_to_focallength(value, cam->sensor_x);
}

static float rna_Camera_angle_y_get(PointerRNA *ptr)
{
  Camera *cam = (Camera *)ptr->owner_id;
  return focallength_to_fov(cam->lens, cam->sensor_y);
}

static void rna_Camera_angle_y_set(PointerRNA *ptr, float value)
{
  Camera *cam = (Camera *)ptr->owner_id;
  cam->lens = fov_to_focallength(value, cam->sensor_y);
}

static void rna_Camera_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  Camera *camera = (Camera *)ptr->owner_id;

  DEG_id_tag_update(&camera->id, 0);
}

static void rna_Camera_dependency_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Camera *camera = (Camera *)ptr->owner_id;
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&camera->id, 0);
}

static CameraBGImage *rna_Camera_background_images_new(Camera *cam)
{
  CameraBGImage *bgpic = BKE_camera_background_image_new(cam);

  WM_main_add_notifier(NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, cam);

  return bgpic;
}

static void rna_Camera_background_images_remove(Camera *cam,
                                                ReportList *reports,
                                                PointerRNA *bgpic_ptr)
{
  CameraBGImage *bgpic = bgpic_ptr->data;
  if (BLI_findindex(&cam->bg_images, bgpic) == -1) {
    BKE_report(reports, RPT_ERROR, "Background image cannot be removed");
  }

  BKE_camera_background_image_remove(cam, bgpic);
  RNA_POINTER_INVALIDATE(bgpic_ptr);

  WM_main_add_notifier(NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, cam);
}

static void rna_Camera_background_images_clear(Camera *cam)
{
  BKE_camera_background_image_clear(cam);

  WM_main_add_notifier(NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, cam);
}

static char *rna_Camera_background_image_path(const PointerRNA *ptr)
{
  const CameraBGImage *bgpic = ptr->data;
  Camera *camera = (Camera *)ptr->owner_id;

  const int bgpic_index = BLI_findindex(&camera->bg_images, bgpic);

  if (bgpic_index >= 0) {
    return BLI_sprintfN("background_images[%d]", bgpic_index);
  }

  return NULL;
}

char *rna_CameraBackgroundImage_image_or_movieclip_user_path(const PointerRNA *ptr)
{
  const char *user = ptr->data;
  Camera *camera = (Camera *)ptr->owner_id;

  int bgpic_index = BLI_findindex(&camera->bg_images, user - offsetof(CameraBGImage, iuser));
  if (bgpic_index >= 0) {
    return BLI_sprintfN("background_images[%d].image_user", bgpic_index);
  }

  bgpic_index = BLI_findindex(&camera->bg_images, user - offsetof(CameraBGImage, cuser));
  if (bgpic_index >= 0) {
    return BLI_sprintfN("background_images[%d].clip_user", bgpic_index);
  }

  return NULL;
}

static bool rna_Camera_background_images_override_apply(Main *bmain,
                                                        PointerRNA *ptr_dst,
                                                        PointerRNA *ptr_src,
                                                        PointerRNA *UNUSED(ptr_storage),
                                                        PropertyRNA *prop_dst,
                                                        PropertyRNA *UNUSED(prop_src),
                                                        PropertyRNA *UNUSED(prop_storage),
                                                        const int UNUSED(len_dst),
                                                        const int UNUSED(len_src),
                                                        const int UNUSED(len_storage),
                                                        PointerRNA *UNUSED(ptr_item_dst),
                                                        PointerRNA *UNUSED(ptr_item_src),
                                                        PointerRNA *UNUSED(ptr_item_storage),
                                                        IDOverrideLibraryPropertyOperation *opop)
{
  BLI_assert_msg(opop->operation == IDOVERRIDE_LIBRARY_OP_INSERT_AFTER,
                 "Unsupported RNA override operation on background images collection");

  Camera *cam_dst = (Camera *)ptr_dst->owner_id;
  Camera *cam_src = (Camera *)ptr_src->owner_id;

  /* Remember that insertion operations are defined and stored in correct order, which means that
   * even if we insert several items in a row, we always insert first one, then second one, etc.
   * So we should always find 'anchor' constraint in both _src *and* _dst. */
  CameraBGImage *bgpic_anchor = BLI_findlink(&cam_dst->bg_images, opop->subitem_reference_index);

  /* If `bgpic_anchor` is NULL, `bgpic_src` will be inserted in first position. */
  CameraBGImage *bgpic_src = BLI_findlink(&cam_src->bg_images, opop->subitem_local_index);

  if (bgpic_src == NULL) {
    BLI_assert(bgpic_src != NULL);
    return false;
  }

  CameraBGImage *bgpic_dst = BKE_camera_background_image_copy(bgpic_src, 0);

  /* This handles NULL anchor as expected by adding at head of list. */
  BLI_insertlinkafter(&cam_dst->bg_images, bgpic_anchor, bgpic_dst);

  RNA_property_update_main(bmain, NULL, ptr_dst, prop_dst);
  return true;
}

static void rna_Camera_dof_update(Main *bmain, Scene *scene, PointerRNA *UNUSED(ptr))
{
  SEQ_relations_invalidate_scene_strips(bmain, scene);
  WM_main_add_notifier(NC_SCENE | ND_SEQUENCER, scene);
}

char *rna_CameraDOFSettings_path(const PointerRNA *ptr)
{
  /* if there is ID-data, resolve the path using the index instead of by name,
   * since the name used is the name of the texture assigned, but the texture
   * may be used multiple times in the same stack
   */
  if (ptr->owner_id) {
    if (GS(ptr->owner_id->name) == ID_CA) {
      return BLI_strdup("dof");
    }
  }

  return BLI_strdup("");
}

static void rna_CameraDOFSettings_aperture_blades_set(PointerRNA *ptr, const int value)
{
  CameraDOFSettings *dofsettings = (CameraDOFSettings *)ptr->data;

  if (ELEM(value, 1, 2)) {
    if (dofsettings->aperture_blades == 0) {
      dofsettings->aperture_blades = 3;
    }
    else {
      dofsettings->aperture_blades = 0;
    }
  }
  else {
    dofsettings->aperture_blades = value;
  }
}

#else

static void rna_def_camera_background_image(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem bgpic_source_items[] = {
      {CAM_BGIMG_SOURCE_IMAGE, "IMAGE", 0, "Image", ""},
      {CAM_BGIMG_SOURCE_MOVIE, "MOVIE_CLIP", 0, "Movie Clip", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem bgpic_camera_frame_items[] = {
      {0, "STRETCH", 0, "Stretch", ""},
      {CAM_BGIMG_FLAG_CAMERA_ASPECT, "FIT", 0, "Fit", ""},
      {CAM_BGIMG_FLAG_CAMERA_ASPECT | CAM_BGIMG_FLAG_CAMERA_CROP, "CROP", 0, "Crop", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem bgpic_display_depth_items[] = {
      {0, "BACK", 0, "Back", ""},
      {CAM_BGIMG_FLAG_FOREGROUND, "FRONT", 0, "Front", ""},
      {0, NULL, 0, NULL, NULL},
  };

  srna = RNA_def_struct(brna, "CameraBackgroundImage", NULL);
  RNA_def_struct_sdna(srna, "CameraBGImage");
  RNA_def_struct_ui_text(
      srna, "Background Image", "Image and settings for display in the 3D View background");
  RNA_def_struct_path_func(srna, "rna_Camera_background_image_path");

  prop = RNA_def_boolean(srna,
                         "is_override_data",
                         false,
                         "Override Background Image",
                         "In a local override camera, whether this background image comes from "
                         "the linked reference camera, or is local to the override");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_boolean_negative_sdna(
      prop, NULL, "flag", CAM_BGIMG_FLAG_OVERRIDE_LIBRARY_LOCAL);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "source", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "source");
  RNA_def_property_enum_items(prop, bgpic_source_items);
  RNA_def_property_ui_text(prop, "Background Source", "Data source used for background");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "image", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "ima");
  RNA_def_property_ui_text(prop, "Image", "Image displayed and edited in this space");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "clip", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "clip");
  RNA_def_property_ui_text(prop, "MovieClip", "Movie clip displayed and edited in this space");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "image_user", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "ImageUser");
  RNA_def_property_pointer_sdna(prop, NULL, "iuser");
  RNA_def_property_ui_text(
      prop,
      "Image User",
      "Parameters defining which layer, pass and frame of the image is displayed");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "clip_user", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "MovieClipUser");
  RNA_def_property_pointer_sdna(prop, NULL, "cuser");
  RNA_def_property_ui_text(
      prop, "Clip User", "Parameters defining which frame of the movie clip is displayed");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "offset", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "offset");
  RNA_def_property_ui_text(prop, "Offset", "");
  RNA_def_property_ui_range(prop, -FLT_MAX, FLT_MAX, 0.1, RNA_TRANSLATION_PREC_DEFAULT);
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "scale");
  RNA_def_property_ui_text(prop, "Scale", "Scale the background image");
  RNA_def_property_range(prop, 0.0, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0, 10.0, 0.100, RNA_TRANSLATION_PREC_DEFAULT);
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "rotation", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "rotation");
  RNA_def_property_ui_text(
      prop, "Rotation", "Rotation for the background image (ortho view only)");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "use_flip_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_BGIMG_FLAG_FLIP_X);
  RNA_def_property_ui_text(prop, "Flip Horizontally", "Flip the background image horizontally");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "use_flip_y", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_BGIMG_FLAG_FLIP_Y);
  RNA_def_property_ui_text(prop, "Flip Vertically", "Flip the background image vertically");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "alpha", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "alpha");
  RNA_def_property_ui_text(
      prop, "Opacity", "Image opacity to blend the image against the background color");
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_expanded", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NO_DEG_UPDATE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_BGIMG_FLAG_EXPANDED);
  RNA_def_property_ui_text(prop, "Show Expanded", "Show the expanded in the user interface");
  RNA_def_property_ui_icon(prop, ICON_DISCLOSURE_TRI_RIGHT, 1);

  prop = RNA_def_property(srna, "use_camera_clip", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_BGIMG_FLAG_CAMERACLIP);
  RNA_def_property_ui_text(prop, "Camera Clip", "Use movie clip from active scene camera");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_background_image", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, NULL, "flag", CAM_BGIMG_FLAG_DISABLED);
  RNA_def_property_ui_text(prop, "Show Background Image", "Show this image as background");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_on_foreground", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_BGIMG_FLAG_FOREGROUND);
  RNA_def_property_ui_text(
      prop, "Show On Foreground", "Show this image in front of objects in viewport");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  /* expose 1 flag as a enum of 2 items */
  prop = RNA_def_property(srna, "display_depth", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, NULL, "flag");
  RNA_def_property_enum_items(prop, bgpic_display_depth_items);
  RNA_def_property_ui_text(prop, "Depth", "Display under or over everything");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  /* expose 2 flags as a enum of 3 items */
  prop = RNA_def_property(srna, "frame_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, NULL, "flag");
  RNA_def_property_enum_items(prop, bgpic_camera_frame_items);
  RNA_def_property_ui_text(prop, "Frame Method", "How the image fits in the camera frame");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  RNA_define_lib_overridable(false);
}

static void rna_def_camera_background_images(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "CameraBackgroundImages");
  srna = RNA_def_struct(brna, "CameraBackgroundImages", NULL);
  RNA_def_struct_sdna(srna, "Camera");
  RNA_def_struct_ui_text(srna, "Background Images", "Collection of background images");

  func = RNA_def_function(srna, "new", "rna_Camera_background_images_new");
  RNA_def_function_ui_description(func, "Add new background image");
  parm = RNA_def_pointer(
      func, "image", "CameraBackgroundImage", "", "Image displayed as viewport background");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_Camera_background_images_remove");
  RNA_def_function_ui_description(func, "Remove background image");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "image", "CameraBackgroundImage", "", "Image displayed as viewport background");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);

  func = RNA_def_function(srna, "clear", "rna_Camera_background_images_clear");
  RNA_def_function_ui_description(func, "Remove all background images");
}

static void rna_def_camera_stereo_data(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem convergence_mode_items[] = {
      {CAM_S3D_OFFAXIS, "OFFAXIS", 0, "Off-Axis", "Off-axis frustums converging in a plane"},
      {CAM_S3D_PARALLEL, "PARALLEL", 0, "Parallel", "Parallel cameras with no convergence"},
      {CAM_S3D_TOE, "TOE", 0, "Toe-in", "Rotated cameras, looking at the convergence distance"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem pivot_items[] = {
      {CAM_S3D_PIVOT_LEFT, "LEFT", 0, "Left", ""},
      {CAM_S3D_PIVOT_RIGHT, "RIGHT", 0, "Right", ""},
      {CAM_S3D_PIVOT_CENTER, "CENTER", 0, "Center", ""},
      {0, NULL, 0, NULL, NULL},
  };

  srna = RNA_def_struct(brna, "CameraStereoData", NULL);
  RNA_def_struct_sdna(srna, "CameraStereoSettings");
  RNA_def_struct_nested(brna, srna, "Camera");
  RNA_def_struct_ui_text(srna, "Stereo", "Stereoscopy settings for a Camera data-block");

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "convergence_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, convergence_mode_items);
  RNA_def_property_ui_text(prop, "Mode", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "pivot", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, pivot_items);
  RNA_def_property_ui_text(prop, "Pivot", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "interocular_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 1e4f, 1, 3);
  RNA_def_property_ui_text(
      prop,
      "Interocular Distance",
      "Set the distance between the eyes - the stereo plane distance / 30 should be fine");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "convergence_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.00001f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.00001f, 15.0f, 1, 3);
  RNA_def_property_ui_text(prop,
                           "Convergence Plane Distance",
                           "The converge point for the stereo cameras "
                           "(often the distance between a projector and the projection screen)");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "use_spherical_stereo", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_S3D_SPHERICAL);
  RNA_def_property_ui_text(prop,
                           "Spherical Stereo",
                           "Render every pixel rotating the camera around the "
                           "middle of the interocular distance");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "use_pole_merge", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_S3D_POLE_MERGE);
  RNA_def_property_ui_text(
      prop, "Use Pole Merge", "Fade interocular distance to 0 after the given cutoff angle");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "pole_merge_angle_from", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_range(prop, 0.0f, M_PI_2);
  RNA_def_property_ui_text(
      prop, "Pole Merge Start Angle", "Angle at which interocular distance starts to fade to 0");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "pole_merge_angle_to", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_range(prop, 0.0f, M_PI_2);
  RNA_def_property_ui_text(
      prop, "Pole Merge End Angle", "Angle at which interocular distance is 0");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  RNA_define_lib_overridable(false);
}

static void rna_def_camera_dof_settings_data(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "CameraDOFSettings", NULL);
  RNA_def_struct_sdna(srna, "CameraDOFSettings");
  RNA_def_struct_path_func(srna, "rna_CameraDOFSettings_path");
  RNA_def_struct_ui_text(srna, "Depth of Field", "Depth of Field settings");

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "use_dof", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_DOF_ENABLED);
  RNA_def_property_ui_text(prop, "Depth of Field", "Use Depth of Field");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dof_update");

  prop = RNA_def_property(srna, "focus_object", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_pointer_sdna(prop, NULL, "focus_object");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(
      prop, "Focus Object", "Use this object to define the depth of field focal point");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dependency_update");

  prop = RNA_def_property(srna, "focus_subtarget", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, NULL, "focus_subtarget");
  RNA_def_property_ui_text(
      prop, "Focus Bone", "Use this armature bone to define the depth of field focal point");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dependency_update");

  prop = RNA_def_property(srna, "focus_distance", PROP_FLOAT, PROP_DISTANCE);
  // RNA_def_property_pointer_sdna(prop, NULL, "focus_distance");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, 5000.0f, 1, 2);
  RNA_def_property_ui_text(
      prop, "Focus Distance", "Distance to the focus point for depth of field");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dof_update");

  prop = RNA_def_property(srna, "aperture_fstop", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(
      prop,
      "F-Stop",
      "F-Stop ratio (lower numbers give more defocus, higher numbers give a sharper image)");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.1f, 128.0f, 10, 1);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dof_update");

  prop = RNA_def_property(srna, "aperture_blades", PROP_INT, PROP_NONE);
  RNA_def_property_ui_text(
      prop, "Blades", "Number of blades in aperture for polygonal bokeh (at least 3)");
  RNA_def_property_range(prop, 0, 16);
  RNA_def_property_int_funcs(prop, NULL, "rna_CameraDOFSettings_aperture_blades_set", NULL);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dof_update");

  prop = RNA_def_property(srna, "aperture_rotation", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_ui_text(prop, "Rotation", "Rotation of blades in aperture");
  RNA_def_property_range(prop, -M_PI, M_PI);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dof_update");

  prop = RNA_def_property(srna, "aperture_ratio", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Ratio", "Distortion to simulate anamorphic lens bokeh");
  RNA_def_property_range(prop, 0.01f, FLT_MAX);
  RNA_def_property_ui_range(prop, 1.0f, 2.0f, 0.1, 3);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_dof_update");

  RNA_define_lib_overridable(false);
}

void RNA_def_camera(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  static const EnumPropertyItem prop_type_items[] = {
      {CAM_PERSP, "PERSP", 0, "Perspective", ""},
      {CAM_ORTHO, "ORTHO", 0, "Orthographic", ""},
      {CAM_PANO, "PANO", 0, "Panoramic", ""},
      {0, NULL, 0, NULL, NULL},
  };
  static const EnumPropertyItem prop_lens_unit_items[] = {
      {0, "MILLIMETERS", 0, "Millimeters", "Specify focal length of the lens in millimeters"},
      {CAM_ANGLETOGGLE,
       "FOV",
       0,
       "Field of View",
       "Specify the lens as the field of view's angle"},
      {0, NULL, 0, NULL, NULL},
  };
  static const EnumPropertyItem sensor_fit_items[] = {
      {CAMERA_SENSOR_FIT_AUTO,
       "AUTO",
       0,
       "Auto",
       "Fit to the sensor width or height depending on image resolution"},
      {CAMERA_SENSOR_FIT_HOR, "HORIZONTAL", 0, "Horizontal", "Fit to the sensor width"},
      {CAMERA_SENSOR_FIT_VERT, "VERTICAL", 0, "Vertical", "Fit to the sensor height"},
      {0, NULL, 0, NULL, NULL},
  };

  srna = RNA_def_struct(brna, "Camera", "ID");
  RNA_def_struct_ui_text(srna, "Camera", "Camera data-block for storing camera settings");
  RNA_def_struct_ui_icon(srna, ICON_CAMERA_DATA);

  RNA_define_lib_overridable(true);

  /* Enums */
  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, prop_type_items);
  RNA_def_property_ui_text(prop, "Type", "Camera types");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "sensor_fit", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "sensor_fit");
  RNA_def_property_enum_items(prop, sensor_fit_items);
  RNA_def_property_ui_text(
      prop, "Sensor Fit", "Method to fit image and field of view angle inside the sensor");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  /* Number values */

  prop = RNA_def_property(srna, "passepartout_alpha", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "passepartalpha");
  RNA_def_property_ui_text(
      prop, "Passepartout Alpha", "Opacity (alpha) of the darkened overlay in Camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "angle_x", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_range(prop, DEG2RAD(0.367), DEG2RAD(172.847));
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Horizontal FOV", "Camera lens horizontal field of view");
  RNA_def_property_float_funcs(prop, "rna_Camera_angle_x_get", "rna_Camera_angle_x_set", NULL);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "angle_y", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_range(prop, DEG2RAD(0.367), DEG2RAD(172.847));
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Vertical FOV", "Camera lens vertical field of view");
  RNA_def_property_float_funcs(prop, "rna_Camera_angle_y_get", "rna_Camera_angle_y_set", NULL);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_range(prop, DEG2RAD(0.367), DEG2RAD(172.847));
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Field of View", "Camera lens field of view");
  RNA_def_property_float_funcs(prop, "rna_Camera_angle_get", "rna_Camera_angle_set", NULL);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "clip_start", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 1e-6f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001f, FLT_MAX, 10, 3);
  RNA_def_property_ui_text(prop, "Clip Start", "Camera near clipping distance");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "clip_end", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 1e-6f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001f, FLT_MAX, 10, 3);
  RNA_def_property_ui_text(prop, "Clip End", "Camera far clipping distance");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "lens", PROP_FLOAT, PROP_DISTANCE_CAMERA);
  RNA_def_property_float_sdna(prop, NULL, "lens");
  RNA_def_property_range(prop, 1.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 1.0f, 5000.0f, 100, 4);
  RNA_def_property_ui_text(
      prop, "Focal Length", "Perspective Camera focal length value in millimeters");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "sensor_width", PROP_FLOAT, PROP_DISTANCE_CAMERA);
  RNA_def_property_float_sdna(prop, NULL, "sensor_x");
  RNA_def_property_range(prop, 1.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 1.0f, 100.0f, 100, 4);
  RNA_def_property_ui_text(
      prop, "Sensor Width", "Horizontal size of the image sensor area in millimeters");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "sensor_height", PROP_FLOAT, PROP_DISTANCE_CAMERA);
  RNA_def_property_float_sdna(prop, NULL, "sensor_y");
  RNA_def_property_range(prop, 1.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 1.0f, 100.0f, 100, 4);
  RNA_def_property_ui_text(
      prop, "Sensor Height", "Vertical size of the image sensor area in millimeters");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "ortho_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "ortho_scale");
  RNA_def_property_range(prop, FLT_MIN, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001f, 10000.0f, 10, 3);
  RNA_def_property_ui_text(
      prop, "Orthographic Scale", "Orthographic Camera scale (similar to zoom)");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "display_size", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, NULL, "drawsize");
  RNA_def_property_range(prop, 0.01f, 1000.0f);
  RNA_def_property_ui_range(prop, 0.01, 100, 1, 2);
  RNA_def_property_ui_text(
      prop, "Display Size", "Apparent size of the Camera object in the 3D View");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "shift_x", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "shiftx");
  RNA_def_property_ui_range(prop, -2.0, 2.0, 1, 3);
  RNA_def_property_ui_text(prop, "Shift X", "Camera horizontal shift");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  prop = RNA_def_property(srna, "shift_y", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "shifty");
  RNA_def_property_ui_range(prop, -2.0, 2.0, 1, 3);
  RNA_def_property_ui_text(prop, "Shift Y", "Camera vertical shift");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Camera_update");

  /* UPBGE */
  prop = RNA_def_property(srna, "use_object_activity_culling", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "gameflag", GAME_CAM_OBJECT_ACTIVITY_CULLING);
  RNA_def_property_ui_text(
      prop, "Activity Culling", "Enable object activity culling with this camera");

  prop = RNA_def_property(srna, "lod_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "lodfactor");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(
      prop, "Level of Detail Distance Factor", "The factor applied to distance computed in Lod");
  RNA_def_property_update(prop, NC_OBJECT | ND_LOD, NULL);

  prop = RNA_def_property(srna, "overlay_disable_bloom", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "gameflag", GAME_CAM_OVERLAY_DISABLE_BLOOM);
  RNA_def_property_ui_text(prop, "Disable Bloom", "Disable Bloom in Overlay Pass");

  prop = RNA_def_property(srna, "overlay_disable_ao", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "gameflag", GAME_CAM_OVERLAY_DISABLE_AO);
  RNA_def_property_ui_text(prop, "Disable AO", "Disable AO in Overlay Pass");

  prop = RNA_def_property(srna, "overlay_disable_ssr", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "gameflag", GAME_CAM_OVERLAY_DISABLE_SSR);
  RNA_def_property_ui_text(prop, "Disable SSR", "Disable SSR in Overlay Pass");

  prop = RNA_def_property(srna, "overlay_disable_world_volumes", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "gameflag", GAME_CAM_OVERLAY_DISABLE_WORLD_VOLUMES);
  RNA_def_property_ui_text(
      prop, "Disable World Volumetrics", "Disable World Volumes in Overlay Pass");
  /*********/

  /* Stereo Settings */
  prop = RNA_def_property(srna, "stereo", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_pointer_sdna(prop, NULL, "stereo");
  RNA_def_property_struct_type(prop, "CameraStereoData");
  RNA_def_property_ui_text(prop, "Stereo", "");

  /* flag */
  prop = RNA_def_property(srna, "show_limits", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOWLIMITS);
  RNA_def_property_ui_text(
      prop, "Show Limits", "Display the clipping range and focus point on the camera");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "show_mist", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOWMIST);
  RNA_def_property_ui_text(
      prop, "Show Mist", "Display a line from the Camera to indicate the mist area");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "show_passepartout", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOWPASSEPARTOUT);
  RNA_def_property_ui_text(
      prop, "Show Passepartout", "Show a darkened overlay outside the image area in Camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_safe_areas", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOW_SAFE_MARGINS);
  RNA_def_property_ui_text(
      prop, "Show Safe Areas", "Show TV title safe and action safe areas in Camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_safe_center", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOW_SAFE_CENTER);
  RNA_def_property_ui_text(prop,
                           "Show Center-Cut Safe Areas",
                           "Show safe areas to fit content in a different aspect ratio");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_name", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOWNAME);
  RNA_def_property_ui_text(prop, "Show Name", "Show the active Camera's name in Camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_sensor", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOWSENSOR);
  RNA_def_property_ui_text(
      prop, "Show Sensor Size", "Show sensor size (film gate) in Camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_background_images", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", CAM_SHOW_BG_IMAGE);
  RNA_def_property_ui_text(
      prop, "Display Background Images", "Display reference images behind objects in the 3D View");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  /* UPBGE */
  prop = RNA_def_property(srna, "use_overlay_mouse_control", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "gameflag", GAME_CAM_OVERLAY_MOUSE_CONTROL);
  RNA_def_property_ui_text(prop,
                           "Game Overlay Mouse Control",
                           "If enabled and if the cam is an overlay cam,"
                           " mouse events will be handled in this camera space");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "lens_unit", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, NULL, "flag");
  RNA_def_property_enum_items(prop, prop_lens_unit_items);
  RNA_def_property_ui_text(prop, "Lens Unit", "Unit to edit lens in for the user interface");

  /* dtx */
  prop = RNA_def_property(srna, "show_composition_center", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_CENTER);
  RNA_def_property_ui_text(
      prop, "Center", "Display center composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_composition_center_diagonal", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_CENTER_DIAG);
  RNA_def_property_ui_text(
      prop, "Center Diagonal", "Display diagonal center composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_composition_thirds", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_THIRDS);
  RNA_def_property_ui_text(
      prop, "Thirds", "Display rule of thirds composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_composition_golden", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_GOLDEN);
  RNA_def_property_ui_text(
      prop, "Golden Ratio", "Display golden ratio composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_composition_golden_tria_a", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_GOLDEN_TRI_A);
  RNA_def_property_ui_text(prop,
                           "Golden Triangle A",
                           "Display golden triangle A composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_composition_golden_tria_b", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_GOLDEN_TRI_B);
  RNA_def_property_ui_text(prop,
                           "Golden Triangle B",
                           "Display golden triangle B composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_composition_harmony_tri_a", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_HARMONY_TRI_A);
  RNA_def_property_ui_text(
      prop, "Harmonious Triangle A", "Display harmony A composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  prop = RNA_def_property(srna, "show_composition_harmony_tri_b", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "dtx", CAM_DTX_HARMONY_TRI_B);
  RNA_def_property_ui_text(
      prop, "Harmonious Triangle B", "Display harmony B composition guide inside the camera view");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  /* pointers */
  prop = RNA_def_property(srna, "dof", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "CameraDOFSettings");
  RNA_def_property_ui_text(prop, "Depth Of Field", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "background_images", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "bg_images", NULL);
  RNA_def_property_struct_type(prop, "CameraBackgroundImage");
  RNA_def_property_ui_text(prop, "Background Images", "List of background images");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_LIBRARY_INSERTION | PROPOVERRIDE_NO_PROP_NAME);
  RNA_def_property_override_funcs(prop, NULL, NULL, "rna_Camera_background_images_override_apply");
  RNA_def_property_update(prop, NC_CAMERA | ND_DRAW_RENDER_VIEWPORT, NULL);

  RNA_define_lib_overridable(false);

  rna_def_animdata_common(srna);

  rna_def_camera_background_image(brna);
  rna_def_camera_background_images(brna, prop);

  /* Nested Data. */
  RNA_define_animate_sdna(true);

  /* *** Animated *** */
  rna_def_camera_stereo_data(brna);
  rna_def_camera_dof_settings_data(brna);

  /* Camera API */
  RNA_api_camera(srna);
}

#endif
