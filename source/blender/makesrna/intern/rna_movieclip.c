/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <limits.h>
#include <stdlib.h>

#include "MEM_guardedalloc.h"

#include "DNA_movieclip_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "rna_internal.h"

#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "WM_types.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"
#include "IMB_metadata.h"

#ifdef RNA_RUNTIME

#  include "DEG_depsgraph.h"

#  include "ED_clip.h"

#  include "DNA_screen_types.h"
#  include "DNA_space_types.h"

#  include "SEQ_relations.h"

static void rna_MovieClip_reload_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  MovieClip *clip = (MovieClip *)ptr->owner_id;

  DEG_id_tag_update(&clip->id, ID_RECALC_SOURCE);
}

static void rna_MovieClip_size_get(PointerRNA *ptr, int *values)
{
  MovieClip *clip = (MovieClip *)ptr->owner_id;

  values[0] = clip->lastsize[0];
  values[1] = clip->lastsize[1];
}

static float rna_MovieClip_fps_get(PointerRNA *ptr)
{
  MovieClip *clip = (MovieClip *)ptr->owner_id;
  return BKE_movieclip_get_fps(clip);
}

static void rna_MovieClip_use_proxy_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  MovieClip *clip = (MovieClip *)ptr->owner_id;
  BKE_movieclip_clear_cache(clip);
  SEQ_relations_invalidate_movieclip_strips(bmain, clip);
}

static void rna_MovieClipUser_proxy_render_settings_update(Main *bmain,
                                                           Scene *UNUSED(scene),
                                                           PointerRNA *ptr)
{
  ID *id = ptr->owner_id;
  MovieClipUser *user = (MovieClipUser *)ptr->data;

  /* when changing render settings of space clip user
   * clear cache for clip, so all the memory is available
   * for new render settings
   */
  if (GS(id->name) == ID_SCR) {
    bScreen *screen = (bScreen *)id;
    ScrArea *area;
    SpaceLink *sl;

    for (area = screen->areabase.first; area; area = area->next) {
      for (sl = area->spacedata.first; sl; sl = sl->next) {
        if (sl->spacetype == SPACE_CLIP) {
          SpaceClip *sc = (SpaceClip *)sl;

          if (&sc->user == user) {
            MovieClip *clip = ED_space_clip_get_clip(sc);

            if (clip && (clip->flag & MCLIP_USE_PROXY)) {
              BKE_movieclip_clear_cache(clip);
              SEQ_relations_invalidate_movieclip_strips(bmain, clip);
            }

            break;
          }
        }
      }
    }
  }
}

static PointerRNA rna_MovieClip_metadata_get(MovieClip *clip)
{
  if (clip == NULL || clip->anim == NULL) {
    return PointerRNA_NULL;
  }

  IDProperty *metadata = IMB_anim_load_metadata(clip->anim);
  if (metadata == NULL) {
    return PointerRNA_NULL;
  }

  PointerRNA ptr;
  RNA_pointer_create(NULL, &RNA_IDPropertyWrapPtr, metadata, &ptr);
  return ptr;
}

static char *rna_MovieClipUser_path(const PointerRNA *ptr)
{
  if (ptr->owner_id) {
    /* MovieClipUser *mc_user = ptr->data; */

    switch (GS(ptr->owner_id->name)) {
      case ID_CA:
        return rna_CameraBackgroundImage_image_or_movieclip_user_path(ptr);
      default:
        break;
    }
  }

  return BLI_strdup("");
}

#else

static void rna_def_movieclip_proxy(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem clip_tc_items[] = {
      {IMB_TC_NONE, "NONE", 0, "None", ""},
      {IMB_TC_RECORD_RUN,
       "RECORD_RUN",
       0,
       "Record Run",
       "Use images in the order they are recorded"},
      {IMB_TC_FREE_RUN,
       "FREE_RUN",
       0,
       "Free Run",
       "Use global timestamp written by recording device"},
      {IMB_TC_INTERPOLATED_REC_DATE_FREE_RUN,
       "FREE_RUN_REC_DATE",
       0,
       "Free Run (rec date)",
       "Interpolate a global timestamp using the record date and time "
       "written by recording device"},
      {IMB_TC_RECORD_RUN_NO_GAPS,
       "FREE_RUN_NO_GAPS",
       0,
       "Free Run No Gaps",
       "Record run, but ignore timecode, changes in framerate or dropouts"},
      {0, NULL, 0, NULL, NULL},
  };

  srna = RNA_def_struct(brna, "MovieClipProxy", NULL);
  RNA_def_struct_ui_text(srna, "Movie Clip Proxy", "Proxy parameters for a movie clip");
  RNA_def_struct_sdna(srna, "MovieClipProxy");

  /* build proxy sized */
  prop = RNA_def_property(srna, "build_25", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_SIZE_25);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "25%", "Build proxy resolution 25% of the original footage dimension");

  prop = RNA_def_property(srna, "build_50", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_SIZE_50);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "50%", "Build proxy resolution 50% of the original footage dimension");

  prop = RNA_def_property(srna, "build_75", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_SIZE_75);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "75%", "Build proxy resolution 75% of the original footage dimension");

  prop = RNA_def_property(srna, "build_100", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_SIZE_100);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "100%", "Build proxy resolution 100% of the original footage dimension");

  prop = RNA_def_property(srna, "build_undistorted_25", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_UNDISTORTED_SIZE_25);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "25%", "Build proxy resolution 25% of the original undistorted footage dimension");

  prop = RNA_def_property(srna, "build_undistorted_50", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_UNDISTORTED_SIZE_50);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "50%", "Build proxy resolution 50% of the original undistorted footage dimension");

  prop = RNA_def_property(srna, "build_undistorted_75", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_UNDISTORTED_SIZE_75);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "75%", "Build proxy resolution 75% of the original undistorted footage dimension");

  prop = RNA_def_property(srna, "build_undistorted_100", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_size_flag", MCLIP_PROXY_UNDISTORTED_SIZE_100);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "100%", "Build proxy resolution 100% of the original undistorted footage dimension");

  /* Build time-codes. */
  prop = RNA_def_property(srna, "build_record_run", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_tc_flag", IMB_TC_RECORD_RUN);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Rec Run", "Build record run time code index");

  prop = RNA_def_property(srna, "build_free_run", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "build_tc_flag", IMB_TC_FREE_RUN);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Free Run", "Build free run time code index");

  prop = RNA_def_property(srna, "build_free_run_rec_date", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, NULL, "build_tc_flag", IMB_TC_INTERPOLATED_REC_DATE_FREE_RUN);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "Free Run (Rec Date)", "Build free run time code index using Record Date/Time");

  /* quality of proxied image */
  prop = RNA_def_property(srna, "quality", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, NULL, "quality");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Quality", "JPEG quality of proxy images");
  RNA_def_property_ui_range(prop, 1, 100, 1, -1);

  prop = RNA_def_property(srna, "timecode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "tc");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_enum_items(prop, clip_tc_items);
  RNA_def_property_ui_text(prop, "Timecode", "");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, "rna_MovieClip_reload_update");

  /* directory */
  prop = RNA_def_property(srna, "directory", PROP_STRING, PROP_DIRPATH);
  RNA_def_property_string_sdna(prop, NULL, "dir");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Directory", "Location to store the proxy files");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, "rna_MovieClip_reload_update");
}

static void rna_def_movieclipUser(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem clip_render_size_items[] = {
      {MCLIP_PROXY_RENDER_SIZE_25, "PROXY_25", 0, "25%", ""},
      {MCLIP_PROXY_RENDER_SIZE_50, "PROXY_50", 0, "50%", ""},
      {MCLIP_PROXY_RENDER_SIZE_75, "PROXY_75", 0, "75%", ""},
      {MCLIP_PROXY_RENDER_SIZE_100, "PROXY_100", 0, "100%", ""},
      {MCLIP_PROXY_RENDER_SIZE_FULL, "FULL", 0, "None, full render", ""},
      {0, NULL, 0, NULL, NULL},
  };

  srna = RNA_def_struct(brna, "MovieClipUser", NULL);
  RNA_def_struct_ui_text(
      srna,
      "Movie Clip User",
      "Parameters defining how a MovieClip data-block is used by another data-block");
  RNA_def_struct_path_func(srna, "rna_MovieClipUser_path");

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "frame_current", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "framenr");
  RNA_def_property_range(prop, MINAFRAME, MAXFRAME);
  RNA_def_property_ui_text(
      prop, "Current Frame", "Current frame number in movie or image sequence");

  /* render size */
  prop = RNA_def_property(srna, "proxy_render_size", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "render_size");
  RNA_def_property_enum_items(prop, clip_render_size_items);
  RNA_def_property_ui_text(prop,
                           "Proxy Render Size",
                           "Display preview using full resolution or different proxy resolutions");
  RNA_def_property_update(
      prop, NC_MOVIECLIP | ND_DISPLAY, "rna_MovieClipUser_proxy_render_settings_update");

  /* render undistorted */
  prop = RNA_def_property(srna, "use_render_undistorted", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "render_flag", MCLIP_PROXY_RENDER_UNDISTORT);
  RNA_def_property_ui_text(prop, "Render Undistorted", "Render preview using undistorted proxy");
  RNA_def_property_update(
      prop, NC_MOVIECLIP | ND_DISPLAY, "rna_MovieClipUser_proxy_render_settings_update");

  RNA_define_lib_overridable(false);
}

static void rna_def_movieClipScopes(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "MovieClipScopes", NULL);
  RNA_def_struct_ui_text(srna, "MovieClipScopes", "Scopes for statistical view of a movie clip");
}

static void rna_def_movieclip(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  FunctionRNA *func;
  PropertyRNA *parm;

  static const EnumPropertyItem clip_source_items[] = {
      {MCLIP_SRC_SEQUENCE, "SEQUENCE", 0, "Image Sequence", "Multiple image files, as a sequence"},
      {MCLIP_SRC_MOVIE, "MOVIE", 0, "Movie File", "Movie file"},
      {0, NULL, 0, NULL, NULL},
  };

  srna = RNA_def_struct(brna, "MovieClip", "ID");
  RNA_def_struct_ui_text(
      srna, "MovieClip", "MovieClip data-block referencing an external movie file");
  RNA_def_struct_ui_icon(srna, ICON_SEQUENCE);

  prop = RNA_def_property(srna, "filepath", PROP_STRING, PROP_FILEPATH);
  RNA_def_property_string_sdna(prop, NULL, "filepath");
  RNA_def_property_ui_text(prop, "File Path", "Filename of the movie or sequence file");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, "rna_MovieClip_reload_update");

  prop = RNA_def_property(srna, "tracking", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MovieTracking");

  prop = RNA_def_property(srna, "proxy", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "MovieClipProxy");

  /* use proxy */
  prop = RNA_def_property(srna, "use_proxy", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", MCLIP_USE_PROXY);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop, "Use Proxy / Timecode", "Use a preview proxy and/or timecode index for this clip");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, "rna_MovieClip_use_proxy_update");

  prop = RNA_def_int_vector(srna,
                            "size",
                            2,
                            NULL,
                            0,
                            0,
                            "Size",
                            "Width and height in pixels, zero when image data can't be loaded",
                            0,
                            0);
  RNA_def_property_int_funcs(prop, "rna_MovieClip_size_get", NULL, NULL);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "display_aspect", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "aspx");
  RNA_def_property_array(prop, 2);
  RNA_def_property_range(prop, 0.1f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.1f, 5000.0f, 1, 2);
  RNA_def_property_ui_text(
      prop, "Display Aspect", "Display Aspect for this clip, does not affect rendering");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, NULL);

  /* source */
  prop = RNA_def_property(srna, "source", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, clip_source_items);
  RNA_def_property_ui_text(prop, "Source", "Where the clip comes from");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  /* custom proxy directory */
  prop = RNA_def_property(srna, "use_proxy_custom_directory", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", MCLIP_USE_PROXY_CUSTOM_DIR);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(
      prop,
      "Proxy Custom Directory",
      "Create proxy images in a custom directory (default is movie location)");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, "rna_MovieClip_reload_update");

  /* grease pencil */
  prop = RNA_def_property(srna, "grease_pencil", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "gpd");
  RNA_def_property_struct_type(prop, "GreasePencil");
  RNA_def_property_pointer_funcs(
      prop, NULL, NULL, NULL, "rna_GPencil_datablocks_annotations_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_REFCOUNT);
  RNA_def_property_ui_text(prop, "Grease Pencil", "Grease pencil data for this movie clip");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, NULL);

  /* start_frame */
  prop = RNA_def_property(srna, "frame_start", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "start_frame");
  RNA_def_property_ui_text(prop,
                           "Start Frame",
                           "Global scene frame number at which this movie starts playing "
                           "(affects all data associated with a clip)");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, NULL);

  /* frame_offset */
  prop = RNA_def_property(srna, "frame_offset", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "frame_offset");
  RNA_def_property_ui_text(
      prop,
      "Frame Offset",
      "Offset of footage first frame relative to its file name "
      "(affects only how footage is loading, does not change data associated with a clip)");
  RNA_def_property_update(prop, NC_MOVIECLIP | ND_DISPLAY, NULL);

  /* length */
  prop = RNA_def_property(srna, "frame_duration", PROP_INT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_int_sdna(prop, NULL, "len");
  RNA_def_property_ui_text(prop, "Duration", "Detected duration of movie clip in frames");

  /* FPS */
  prop = RNA_def_property(srna, "fps", PROP_FLOAT, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_float_funcs(prop, "rna_MovieClip_fps_get", NULL, NULL);
  RNA_def_property_ui_text(
      prop, "Frame Rate", "Detected frame rate of the movie clip in frames per second");

  /* color management */
  prop = RNA_def_property(srna, "colorspace_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "colorspace_settings");
  RNA_def_property_struct_type(prop, "ColorManagedInputColorspaceSettings");
  RNA_def_property_ui_text(prop, "Color Space Settings", "Input color space settings");

  /* metadata */
  func = RNA_def_function(srna, "metadata", "rna_MovieClip_metadata_get");
  RNA_def_function_ui_description(func, "Retrieve metadata of the movie file");
  /* return type */
  parm = RNA_def_pointer(
      func, "metadata", "IDPropertyWrapPtr", "", "Dict-like object containing the metadata");
  RNA_def_parameter_flags(parm, 0, PARM_RNAPTR);
  RNA_def_function_return(func, parm);

  rna_def_animdata_common(srna);
}

void RNA_def_movieclip(BlenderRNA *brna)
{
  rna_def_movieclip(brna);
  rna_def_movieclip_proxy(brna);
  rna_def_movieclipUser(brna);
  rna_def_movieClipScopes(brna);
}

#endif
