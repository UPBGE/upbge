/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32
#  include <unistd.h>
#else
#  include <io.h>
#endif

#include <time.h>

#include "MEM_guardedalloc.h"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_defaults.h"

#include "DNA_constraint_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_utildefines.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_math.h"
#include "BLI_threads.h"

#include "BLT_translation.h"

#include "BKE_anim_data.h"
#include "BKE_bpath.h"
#include "BKE_colortools.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_image.h" /* openanim */
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_movieclip.h"
#include "BKE_node.h"
#include "BKE_node_tree_update.h"
#include "BKE_tracking.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"
#include "IMB_moviecache.h"
#include "IMB_openexr.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "GPU_texture.h"

#include "BLO_read_write.h"

static void free_buffers(MovieClip *clip);

static void movie_clip_init_data(ID *id)
{
  MovieClip *movie_clip = (MovieClip *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(movie_clip, id));

  MEMCPY_STRUCT_AFTER(movie_clip, DNA_struct_default_get(MovieClip), id);

  BKE_tracking_settings_init(&movie_clip->tracking);
  BKE_color_managed_colorspace_settings_init(&movie_clip->colorspace_settings);
}

static void movie_clip_copy_data(Main *UNUSED(bmain), ID *id_dst, const ID *id_src, const int flag)
{
  MovieClip *movie_clip_dst = (MovieClip *)id_dst;
  const MovieClip *movie_clip_src = (const MovieClip *)id_src;

  /* We never handle user-count here for own data. */
  const int flag_subdata = flag | LIB_ID_CREATE_NO_USER_REFCOUNT;

  movie_clip_dst->anim = NULL;
  movie_clip_dst->cache = NULL;

  BKE_tracking_copy(&movie_clip_dst->tracking, &movie_clip_src->tracking, flag_subdata);
  movie_clip_dst->tracking_context = NULL;

  BKE_color_managed_colorspace_settings_copy(&movie_clip_dst->colorspace_settings,
                                             &movie_clip_src->colorspace_settings);
}

static void movie_clip_free_data(ID *id)
{
  MovieClip *movie_clip = (MovieClip *)id;

  /* Also frees animdata. */
  free_buffers(movie_clip);

  BKE_tracking_free(&movie_clip->tracking);
}

static void movie_clip_foreach_id(ID *id, LibraryForeachIDData *data)
{
  MovieClip *movie_clip = (MovieClip *)id;
  MovieTracking *tracking = &movie_clip->tracking;

  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, movie_clip->gpd, IDWALK_CB_USER);

  LISTBASE_FOREACH (MovieTrackingTrack *, track, &tracking->tracks) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, track->gpd, IDWALK_CB_USER);
  }
  LISTBASE_FOREACH (MovieTrackingObject *, object, &tracking->objects) {
    LISTBASE_FOREACH (MovieTrackingTrack *, track, &object->tracks) {
      BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, track->gpd, IDWALK_CB_USER);
    }
  }

  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, &tracking->plane_tracks) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, plane_track->image, IDWALK_CB_USER);
  }
}

static void movie_clip_foreach_cache(ID *id,
                                     IDTypeForeachCacheFunctionCallback function_callback,
                                     void *user_data)
{
  MovieClip *movie_clip = (MovieClip *)id;
  IDCacheKey key = {
      .id_session_uuid = id->session_uuid,
      .offset_in_ID = offsetof(MovieClip, cache),
  };
  function_callback(id, &key, (void **)&movie_clip->cache, 0, user_data);

  key.offset_in_ID = offsetof(MovieClip, tracking.camera.intrinsics);
  function_callback(id, &key, (void **)&movie_clip->tracking.camera.intrinsics, 0, user_data);
}

static void movie_clip_foreach_path(ID *id, BPathForeachPathData *bpath_data)
{
  MovieClip *movie_clip = (MovieClip *)id;
  BKE_bpath_foreach_path_fixed_process(bpath_data, movie_clip->filepath);
}

static void write_movieTracks(BlendWriter *writer, ListBase *tracks)
{
  MovieTrackingTrack *track;

  track = tracks->first;
  while (track) {
    BLO_write_struct(writer, MovieTrackingTrack, track);

    if (track->markers) {
      BLO_write_struct_array(writer, MovieTrackingMarker, track->markersnr, track->markers);
    }

    track = track->next;
  }
}

static void write_moviePlaneTracks(BlendWriter *writer, ListBase *plane_tracks_base)
{
  MovieTrackingPlaneTrack *plane_track;

  for (plane_track = plane_tracks_base->first; plane_track; plane_track = plane_track->next) {
    BLO_write_struct(writer, MovieTrackingPlaneTrack, plane_track);

    BLO_write_pointer_array(writer, plane_track->point_tracksnr, plane_track->point_tracks);
    BLO_write_struct_array(
        writer, MovieTrackingPlaneMarker, plane_track->markersnr, plane_track->markers);
  }
}

static void write_movieReconstruction(BlendWriter *writer,
                                      MovieTrackingReconstruction *reconstruction)
{
  if (reconstruction->camnr) {
    BLO_write_struct_array(
        writer, MovieReconstructedCamera, reconstruction->camnr, reconstruction->cameras);
  }
}

static void movieclip_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  MovieClip *clip = (MovieClip *)id;

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  clip->anim = NULL;
  clip->tracking_context = NULL;
  clip->tracking.stats = NULL;

  MovieTracking *tracking = &clip->tracking;
  MovieTrackingObject *object;

  BLO_write_id_struct(writer, MovieClip, id_address, &clip->id);
  BKE_id_blend_write(writer, &clip->id);

  if (clip->adt) {
    BKE_animdata_blend_write(writer, clip->adt);
  }

  write_movieTracks(writer, &tracking->tracks);
  write_moviePlaneTracks(writer, &tracking->plane_tracks);
  write_movieReconstruction(writer, &tracking->reconstruction);

  object = tracking->objects.first;
  while (object) {
    BLO_write_struct(writer, MovieTrackingObject, object);

    write_movieTracks(writer, &object->tracks);
    write_moviePlaneTracks(writer, &object->plane_tracks);
    write_movieReconstruction(writer, &object->reconstruction);

    object = object->next;
  }
}

static void direct_link_movieReconstruction(BlendDataReader *reader,
                                            MovieTrackingReconstruction *reconstruction)
{
  BLO_read_data_address(reader, &reconstruction->cameras);
}

static void direct_link_movieTracks(BlendDataReader *reader, ListBase *tracksbase)
{
  BLO_read_list(reader, tracksbase);

  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracksbase) {
    BLO_read_data_address(reader, &track->markers);
  }
}

static void direct_link_moviePlaneTracks(BlendDataReader *reader, ListBase *plane_tracks_base)
{
  BLO_read_list(reader, plane_tracks_base);

  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, plane_tracks_base) {
    BLO_read_pointer_array(reader, (void **)&plane_track->point_tracks);
    for (int i = 0; i < plane_track->point_tracksnr; i++) {
      BLO_read_data_address(reader, &plane_track->point_tracks[i]);
    }

    BLO_read_data_address(reader, &plane_track->markers);
  }
}

static void movieclip_blend_read_data(BlendDataReader *reader, ID *id)
{
  MovieClip *clip = (MovieClip *)id;
  MovieTracking *tracking = &clip->tracking;

  BLO_read_data_address(reader, &clip->adt);
  BKE_animdata_blend_read_data(reader, clip->adt);

  direct_link_movieTracks(reader, &tracking->tracks);
  direct_link_moviePlaneTracks(reader, &tracking->plane_tracks);
  direct_link_movieReconstruction(reader, &tracking->reconstruction);

  BLO_read_data_address(reader, &clip->tracking.act_track);
  BLO_read_data_address(reader, &clip->tracking.act_plane_track);

  clip->anim = NULL;
  clip->tracking_context = NULL;
  clip->tracking.stats = NULL;

  /* TODO: we could store those in undo cache storage as well, and preserve them instead of
   * re-creating them... */
  BLI_listbase_clear(&clip->runtime.gputextures);

  /* Needed for proper versioning, will be NULL for all newer files anyway. */
  BLO_read_data_address(reader, &clip->tracking.stabilization.rot_track);

  clip->tracking.dopesheet.ok = 0;
  BLI_listbase_clear(&clip->tracking.dopesheet.channels);
  BLI_listbase_clear(&clip->tracking.dopesheet.coverage_segments);

  BLO_read_list(reader, &tracking->objects);

  LISTBASE_FOREACH (MovieTrackingObject *, object, &tracking->objects) {
    direct_link_movieTracks(reader, &object->tracks);
    direct_link_moviePlaneTracks(reader, &object->plane_tracks);
    direct_link_movieReconstruction(reader, &object->reconstruction);
  }
}

static void lib_link_movieTracks(BlendLibReader *reader, MovieClip *clip, ListBase *tracksbase)
{
  LISTBASE_FOREACH (MovieTrackingTrack *, track, tracksbase) {
    BLO_read_id_address(reader, clip->id.lib, &track->gpd);
  }
}

static void lib_link_moviePlaneTracks(BlendLibReader *reader,
                                      MovieClip *clip,
                                      ListBase *tracksbase)
{
  LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, tracksbase) {
    BLO_read_id_address(reader, clip->id.lib, &plane_track->image);
  }
}

static void movieclip_blend_read_lib(BlendLibReader *reader, ID *id)
{
  MovieClip *clip = (MovieClip *)id;
  MovieTracking *tracking = &clip->tracking;

  BLO_read_id_address(reader, clip->id.lib, &clip->gpd);

  lib_link_movieTracks(reader, clip, &tracking->tracks);
  lib_link_moviePlaneTracks(reader, clip, &tracking->plane_tracks);

  LISTBASE_FOREACH (MovieTrackingObject *, object, &tracking->objects) {
    lib_link_movieTracks(reader, clip, &object->tracks);
    lib_link_moviePlaneTracks(reader, clip, &object->plane_tracks);
  }
}

IDTypeInfo IDType_ID_MC = {
    .id_code = ID_MC,
    .id_filter = FILTER_ID_MC,
    .main_listbase_index = INDEX_ID_MC,
    .struct_size = sizeof(MovieClip),
    .name = "MovieClip",
    .name_plural = "movieclips",
    .translation_context = BLT_I18NCONTEXT_ID_MOVIECLIP,
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = NULL,

    .init_data = movie_clip_init_data,
    .copy_data = movie_clip_copy_data,
    .free_data = movie_clip_free_data,
    .make_local = NULL,
    .foreach_id = movie_clip_foreach_id,
    .foreach_cache = movie_clip_foreach_cache,
    .foreach_path = movie_clip_foreach_path,
    .owner_get = NULL,

    .blend_write = movieclip_blend_write,
    .blend_read_data = movieclip_blend_read_data,
    .blend_read_lib = movieclip_blend_read_lib,
    .blend_read_expand = NULL,

    .blend_read_undo_preserve = NULL,

    .lib_override_apply_post = NULL,
};

/*********************** movieclip buffer loaders *************************/

static int sequence_guess_offset(const char *full_name, int head_len, unsigned short numlen)
{
  char num[FILE_MAX] = {0};

  BLI_strncpy(num, full_name + head_len, numlen + 1);

  return atoi(num);
}

static int rendersize_to_proxy(const MovieClipUser *user, int flag)
{
  if ((flag & MCLIP_USE_PROXY) == 0) {
    return IMB_PROXY_NONE;
  }

  switch (user->render_size) {
    case MCLIP_PROXY_RENDER_SIZE_25:
      return IMB_PROXY_25;

    case MCLIP_PROXY_RENDER_SIZE_50:
      return IMB_PROXY_50;

    case MCLIP_PROXY_RENDER_SIZE_75:
      return IMB_PROXY_75;

    case MCLIP_PROXY_RENDER_SIZE_100:
      return IMB_PROXY_100;

    case MCLIP_PROXY_RENDER_SIZE_FULL:
      return IMB_PROXY_NONE;
  }

  return IMB_PROXY_NONE;
}

static int rendersize_to_number(int render_size)
{
  switch (render_size) {
    case MCLIP_PROXY_RENDER_SIZE_25:
      return 25;

    case MCLIP_PROXY_RENDER_SIZE_50:
      return 50;

    case MCLIP_PROXY_RENDER_SIZE_75:
      return 75;

    case MCLIP_PROXY_RENDER_SIZE_100:
      return 100;

    case MCLIP_PROXY_RENDER_SIZE_FULL:
      return 100;
  }

  return 100;
}

static int get_timecode(MovieClip *clip, int flag)
{
  if ((flag & MCLIP_USE_PROXY) == 0) {
    return IMB_TC_NONE;
  }

  return clip->proxy.tc;
}

static void get_sequence_fname(const MovieClip *clip, const int framenr, char *name)
{
  unsigned short numlen;
  char head[FILE_MAX], tail[FILE_MAX];
  int offset;

  BLI_strncpy(name, clip->filepath, sizeof(clip->filepath));
  BLI_path_sequence_decode(name, head, tail, &numlen);

  /* Movie-clips always points to first image from sequence, auto-guess offset for now.
   * Could be something smarter in the future. */
  offset = sequence_guess_offset(clip->filepath, strlen(head), numlen);

  if (numlen) {
    BLI_path_sequence_encode(
        name, head, tail, numlen, offset + framenr - clip->start_frame + clip->frame_offset);
  }
  else {
    BLI_strncpy(name, clip->filepath, sizeof(clip->filepath));
  }

  BLI_path_abs(name, ID_BLEND_PATH_FROM_GLOBAL(&clip->id));
}

/* supposed to work with sequences only */
static void get_proxy_fname(
    const MovieClip *clip, int proxy_render_size, bool undistorted, int framenr, char *name)
{
  int size = rendersize_to_number(proxy_render_size);
  char dir[FILE_MAX], clipdir[FILE_MAX], clipfile[FILE_MAX];
  int proxynr = framenr - clip->start_frame + 1 + clip->frame_offset;

  BLI_split_dirfile(clip->filepath, clipdir, clipfile, FILE_MAX, FILE_MAX);

  if (clip->flag & MCLIP_USE_PROXY_CUSTOM_DIR) {
    BLI_strncpy(dir, clip->proxy.dir, sizeof(dir));
  }
  else {
    BLI_snprintf(dir, FILE_MAX, "%s/BL_proxy", clipdir);
  }

  if (undistorted) {
    BLI_snprintf(name, FILE_MAX, "%s/%s/proxy_%d_undistorted/%08d", dir, clipfile, size, proxynr);
  }
  else {
    BLI_snprintf(name, FILE_MAX, "%s/%s/proxy_%d/%08d", dir, clipfile, size, proxynr);
  }

  BLI_path_abs(name, BKE_main_blendfile_path_from_global());
  BLI_path_frame(name, 1, 0);

  strcat(name, ".jpg");
}

#ifdef WITH_OPENEXR

typedef struct MultilayerConvertContext {
  float *combined_pass;
  int num_combined_channels;
} MultilayerConvertContext;

static void *movieclip_convert_multilayer_add_view(void *UNUSED(ctx_v),
                                                   const char *UNUSED(view_name))
{
  return NULL;
}

static void *movieclip_convert_multilayer_add_layer(void *ctx_v, const char *UNUSED(layer_name))
{
  /* Return dummy non-NULL value, we don't use layer handle but need to return
   * something, so render API invokes the add_pass() callbacks. */
  return ctx_v;
}

static void movieclip_convert_multilayer_add_pass(void *UNUSED(layer),
                                                  void *ctx_v,
                                                  const char *pass_name,
                                                  float *rect,
                                                  int num_channels,
                                                  const char *chan_id,
                                                  const char *UNUSED(view_name))
{
  /* NOTE: This function must free pass pixels data if it is not used, this
   * is how IMB_exr_multilayer_convert() is working. */
  MultilayerConvertContext *ctx = ctx_v;
  /* If we've found a first combined pass, skip all the rest ones. */
  if (ctx->combined_pass != NULL) {
    MEM_freeN(rect);
    return;
  }
  if (STREQ(pass_name, RE_PASSNAME_COMBINED) || STR_ELEM(chan_id, "RGBA", "RGB")) {
    ctx->combined_pass = rect;
    ctx->num_combined_channels = num_channels;
  }
  else {
    MEM_freeN(rect);
  }
}

#endif /* WITH_OPENEXR */

void BKE_movieclip_convert_multilayer_ibuf(struct ImBuf *ibuf)
{
  if (ibuf == NULL) {
    return;
  }
#ifdef WITH_OPENEXR
  if (ibuf->ftype != IMB_FTYPE_OPENEXR || ibuf->userdata == NULL) {
    return;
  }
  MultilayerConvertContext ctx;
  ctx.combined_pass = NULL;
  ctx.num_combined_channels = 0;
  IMB_exr_multilayer_convert(ibuf->userdata,
                             &ctx,
                             movieclip_convert_multilayer_add_view,
                             movieclip_convert_multilayer_add_layer,
                             movieclip_convert_multilayer_add_pass);
  if (ctx.combined_pass != NULL) {
    BLI_assert(ibuf->rect_float == NULL);
    ibuf->rect_float = ctx.combined_pass;
    ibuf->channels = ctx.num_combined_channels;
    ibuf->flags |= IB_rectfloat;
    ibuf->mall |= IB_rectfloat;
  }
  IMB_exr_close(ibuf->userdata);
  ibuf->userdata = NULL;
#endif
}

static ImBuf *movieclip_load_sequence_file(MovieClip *clip,
                                           const MovieClipUser *user,
                                           int framenr,
                                           int flag)
{
  struct ImBuf *ibuf;
  char name[FILE_MAX];
  int loadflag;
  bool use_proxy = false;
  char *colorspace;

  use_proxy = (flag & MCLIP_USE_PROXY) && user->render_size != MCLIP_PROXY_RENDER_SIZE_FULL;
  if (use_proxy) {
    int undistort = user->render_flag & MCLIP_PROXY_RENDER_UNDISTORT;
    get_proxy_fname(clip, user->render_size, undistort, framenr, name);

    /* Well, this is a bit weird, but proxies for movie sources
     * are built in the same exact color space as the input,
     *
     * But image sequences are built in the display space.
     */
    if (clip->source == MCLIP_SRC_MOVIE) {
      colorspace = clip->colorspace_settings.name;
    }
    else {
      colorspace = NULL;
    }
  }
  else {
    get_sequence_fname(clip, framenr, name);
    colorspace = clip->colorspace_settings.name;
  }

  loadflag = IB_rect | IB_multilayer | IB_alphamode_detect | IB_metadata;

  /* read ibuf */
  ibuf = IMB_loadiffname(name, loadflag, colorspace);
  BKE_movieclip_convert_multilayer_ibuf(ibuf);

  return ibuf;
}

static void movieclip_open_anim_file(MovieClip *clip)
{
  char str[FILE_MAX];

  if (!clip->anim) {
    BLI_strncpy(str, clip->filepath, FILE_MAX);
    BLI_path_abs(str, ID_BLEND_PATH_FROM_GLOBAL(&clip->id));

    /* FIXME: make several stream accessible in image editor, too */
    clip->anim = openanim(str, IB_rect, 0, clip->colorspace_settings.name);

    if (clip->anim) {
      if (clip->flag & MCLIP_USE_PROXY_CUSTOM_DIR) {
        char dir[FILE_MAX];
        BLI_strncpy(dir, clip->proxy.dir, sizeof(dir));
        BLI_path_abs(dir, BKE_main_blendfile_path_from_global());
        IMB_anim_set_index_dir(clip->anim, dir);
      }
    }
  }
}

static ImBuf *movieclip_load_movie_file(MovieClip *clip,
                                        const MovieClipUser *user,
                                        int framenr,
                                        int flag)
{
  ImBuf *ibuf = NULL;
  int tc = get_timecode(clip, flag);
  int proxy = rendersize_to_proxy(user, flag);

  movieclip_open_anim_file(clip);

  if (clip->anim) {
    int fra = framenr - clip->start_frame + clip->frame_offset;

    ibuf = IMB_anim_absolute(clip->anim, fra, tc, proxy);
  }

  return ibuf;
}

static void movieclip_calc_length(MovieClip *clip)
{
  if (clip->source == MCLIP_SRC_MOVIE) {
    movieclip_open_anim_file(clip);

    if (clip->anim) {
      clip->len = IMB_anim_get_duration(clip->anim, clip->proxy.tc);
    }
  }
  else if (clip->source == MCLIP_SRC_SEQUENCE) {
    unsigned short numlen;
    char name[FILE_MAX], head[FILE_MAX], tail[FILE_MAX];

    BLI_path_sequence_decode(clip->filepath, head, tail, &numlen);

    if (numlen == 0) {
      /* there's no number group in file name, assume it's single framed sequence */
      clip->len = 1;
    }
    else {
      clip->len = 0;
      for (;;) {
        get_sequence_fname(clip, clip->len + clip->start_frame, name);

        if (BLI_exists(name)) {
          clip->len++;
        }
        else {
          break;
        }
      }
    }
  }
}

/*********************** image buffer cache *************************/

typedef struct MovieClipCache {
  /* regular movie cache */
  struct MovieCache *moviecache;

  /* cached postprocessed shot */
  struct {
    ImBuf *ibuf;
    int framenr;
    int flag;

    /* cache for undistorted shot */
    float focal_length;
    float principal[2];
    float polynomial_k[3];
    float division_k[2];
    float nuke_k[2];
    float brown_k[4];
    float brown_p[2];
    short distortion_model;
    bool undistortion_used;

    int proxy;
    short render_flag;
  } postprocessed;

  /* cache for stable shot */
  struct {
    ImBuf *reference_ibuf;

    ImBuf *ibuf;
    int framenr;
    int postprocess_flag;

    float loc[2], scale, angle, aspect;
    int proxy, filter;
    short render_flag;
  } stabilized;

  int sequence_offset;

  bool is_still_sequence;
} MovieClipCache;

typedef struct MovieClipImBufCacheKey {
  int framenr;
  int proxy;
  short render_flag;
} MovieClipImBufCacheKey;

typedef struct MovieClipCachePriorityData {
  int framenr;
} MovieClipCachePriorityData;

static int user_frame_to_cache_frame(MovieClip *clip, int framenr)
{
  int index;

  index = framenr - clip->start_frame + clip->frame_offset;

  if (clip->source == MCLIP_SRC_SEQUENCE) {
    if (clip->cache->sequence_offset == -1) {
      unsigned short numlen;
      char head[FILE_MAX], tail[FILE_MAX];

      BLI_path_sequence_decode(clip->filepath, head, tail, &numlen);

      /* see comment in get_sequence_fname */
      clip->cache->sequence_offset = sequence_guess_offset(clip->filepath, strlen(head), numlen);
    }

    index += clip->cache->sequence_offset;
  }

  if (index < 0) {
    return framenr - index;
  }

  return framenr;
}

static void moviecache_keydata(void *userkey, int *framenr, int *proxy, int *render_flags)
{
  const MovieClipImBufCacheKey *key = userkey;

  *framenr = key->framenr;
  *proxy = key->proxy;
  *render_flags = key->render_flag;
}

static unsigned int moviecache_hashhash(const void *keyv)
{
  const MovieClipImBufCacheKey *key = keyv;
  int rval = key->framenr;

  return rval;
}

static bool moviecache_hashcmp(const void *av, const void *bv)
{
  const MovieClipImBufCacheKey *a = av;
  const MovieClipImBufCacheKey *b = bv;

  return ((a->framenr != b->framenr) || (a->proxy != b->proxy) ||
          (a->render_flag != b->render_flag));
}

static void *moviecache_getprioritydata(void *key_v)
{
  MovieClipImBufCacheKey *key = (MovieClipImBufCacheKey *)key_v;
  MovieClipCachePriorityData *priority_data;

  priority_data = MEM_callocN(sizeof(*priority_data), "movie cache clip priority data");
  priority_data->framenr = key->framenr;

  return priority_data;
}

static int moviecache_getitempriority(void *last_userkey_v, void *priority_data_v)
{
  MovieClipImBufCacheKey *last_userkey = (MovieClipImBufCacheKey *)last_userkey_v;
  MovieClipCachePriorityData *priority_data = (MovieClipCachePriorityData *)priority_data_v;

  return -abs(last_userkey->framenr - priority_data->framenr);
}

static void moviecache_prioritydeleter(void *priority_data_v)
{
  MovieClipCachePriorityData *priority_data = (MovieClipCachePriorityData *)priority_data_v;

  MEM_freeN(priority_data);
}

static ImBuf *get_imbuf_cache(MovieClip *clip, const MovieClipUser *user, int flag)
{
  if (clip->cache) {
    MovieClipImBufCacheKey key;

    if (!clip->cache->is_still_sequence) {
      key.framenr = user_frame_to_cache_frame(clip, user->framenr);
    }
    else {
      key.framenr = 1;
    }

    if (flag & MCLIP_USE_PROXY) {
      key.proxy = rendersize_to_proxy(user, flag);
      key.render_flag = user->render_flag;
    }
    else {
      key.proxy = IMB_PROXY_NONE;
      key.render_flag = 0;
    }

    return IMB_moviecache_get(clip->cache->moviecache, &key, NULL);
  }

  return NULL;
}

static bool has_imbuf_cache(MovieClip *clip, MovieClipUser *user, int flag)
{
  if (clip->cache) {
    MovieClipImBufCacheKey key;

    key.framenr = user_frame_to_cache_frame(clip, user->framenr);

    if (flag & MCLIP_USE_PROXY) {
      key.proxy = rendersize_to_proxy(user, flag);
      key.render_flag = user->render_flag;
    }
    else {
      key.proxy = IMB_PROXY_NONE;
      key.render_flag = 0;
    }

    return IMB_moviecache_has_frame(clip->cache->moviecache, &key);
  }

  return false;
}

static bool put_imbuf_cache(
    MovieClip *clip, const MovieClipUser *user, ImBuf *ibuf, int flag, bool destructive)
{
  MovieClipImBufCacheKey key;

  if (clip->cache == NULL) {
    struct MovieCache *moviecache;

    // char cache_name[64];
    // BLI_snprintf(cache_name, sizeof(cache_name), "movie %s", clip->id.name);

    clip->cache = MEM_callocN(sizeof(MovieClipCache), "movieClipCache");

    moviecache = IMB_moviecache_create(
        "movieclip", sizeof(MovieClipImBufCacheKey), moviecache_hashhash, moviecache_hashcmp);

    IMB_moviecache_set_getdata_callback(moviecache, moviecache_keydata);
    IMB_moviecache_set_priority_callback(moviecache,
                                         moviecache_getprioritydata,
                                         moviecache_getitempriority,
                                         moviecache_prioritydeleter);

    clip->cache->moviecache = moviecache;
    clip->cache->sequence_offset = -1;
    if (clip->source == MCLIP_SRC_SEQUENCE) {
      unsigned short numlen;
      BLI_path_sequence_decode(clip->filepath, NULL, NULL, &numlen);
      clip->cache->is_still_sequence = (numlen == 0);
    }
  }

  if (!clip->cache->is_still_sequence) {
    key.framenr = user_frame_to_cache_frame(clip, user->framenr);
  }
  else {
    key.framenr = 1;
  }

  if (flag & MCLIP_USE_PROXY) {
    key.proxy = rendersize_to_proxy(user, flag);
    key.render_flag = user->render_flag;
  }
  else {
    key.proxy = IMB_PROXY_NONE;
    key.render_flag = 0;
  }

  if (destructive) {
    IMB_moviecache_put(clip->cache->moviecache, &key, ibuf);
    return true;
  }

  return IMB_moviecache_put_if_possible(clip->cache->moviecache, &key, ibuf);
}

static bool moviecache_check_free_proxy(ImBuf *UNUSED(ibuf), void *userkey, void *UNUSED(userdata))
{
  MovieClipImBufCacheKey *key = (MovieClipImBufCacheKey *)userkey;

  return !(key->proxy == IMB_PROXY_NONE && key->render_flag == 0);
}

/*********************** common functions *************************/

/* only image block itself */
static MovieClip *movieclip_alloc(Main *bmain, const char *name)
{
  MovieClip *clip;

  clip = BKE_id_new(bmain, ID_MC, name);

  return clip;
}

static void movieclip_load_get_size(MovieClip *clip)
{
  int width, height;
  MovieClipUser user = *DNA_struct_default_get(MovieClipUser);

  user.framenr = BKE_movieclip_remap_clip_to_scene_frame(clip, 1);
  BKE_movieclip_get_size(clip, &user, &width, &height);

  if (width && height) {
    clip->tracking.camera.principal[0] = ((float)width) / 2.0f;
    clip->tracking.camera.principal[1] = ((float)height) / 2.0f;
  }
  else {
    clip->lastsize[0] = clip->lastsize[1] = IMG_SIZE_FALLBACK;
  }
}

static void detect_clip_source(Main *bmain, MovieClip *clip)
{
  ImBuf *ibuf;
  char name[FILE_MAX];

  BLI_strncpy(name, clip->filepath, sizeof(name));
  BLI_path_abs(name, BKE_main_blendfile_path(bmain));

  ibuf = IMB_testiffname(name, IB_rect | IB_multilayer);
  if (ibuf) {
    clip->source = MCLIP_SRC_SEQUENCE;
    IMB_freeImBuf(ibuf);
  }
  else {
    clip->source = MCLIP_SRC_MOVIE;
  }
}

MovieClip *BKE_movieclip_file_add(Main *bmain, const char *name)
{
  MovieClip *clip;
  int file;
  char str[FILE_MAX];

  BLI_strncpy(str, name, sizeof(str));
  BLI_path_abs(str, BKE_main_blendfile_path(bmain));

  /* exists? */
  file = BLI_open(str, O_BINARY | O_RDONLY, 0);
  if (file == -1) {
    return NULL;
  }
  close(file);

  /* ** add new movieclip ** */

  /* create a short library name */
  clip = movieclip_alloc(bmain, BLI_path_basename(name));
  BLI_strncpy(clip->filepath, name, sizeof(clip->filepath));

  detect_clip_source(bmain, clip);

  movieclip_load_get_size(clip);
  if (clip->lastsize[0]) {
    int width = clip->lastsize[0];

    clip->tracking.camera.focal = 24.0f * width / clip->tracking.camera.sensor_width;
  }

  movieclip_calc_length(clip);

  return clip;
}

MovieClip *BKE_movieclip_file_add_exists_ex(Main *bmain, const char *filepath, bool *r_exists)
{
  MovieClip *clip;
  char str[FILE_MAX], strtest[FILE_MAX];

  BLI_strncpy(str, filepath, sizeof(str));
  BLI_path_abs(str, BKE_main_blendfile_path(bmain));

  /* first search an identical filepath */
  for (clip = bmain->movieclips.first; clip; clip = clip->id.next) {
    BLI_strncpy(strtest, clip->filepath, sizeof(clip->filepath));
    BLI_path_abs(strtest, ID_BLEND_PATH(bmain, &clip->id));

    if (BLI_path_cmp(strtest, str) == 0) {
      id_us_plus(&clip->id); /* officially should not, it doesn't link here! */
      if (r_exists) {
        *r_exists = true;
      }
      return clip;
    }
  }

  if (r_exists) {
    *r_exists = false;
  }
  return BKE_movieclip_file_add(bmain, filepath);
}

MovieClip *BKE_movieclip_file_add_exists(Main *bmain, const char *filepath)
{
  return BKE_movieclip_file_add_exists_ex(bmain, filepath, NULL);
}

static void real_ibuf_size(
    const MovieClip *clip, const MovieClipUser *user, const ImBuf *ibuf, int *width, int *height)
{
  *width = ibuf->x;
  *height = ibuf->y;

  if (clip->flag & MCLIP_USE_PROXY) {
    switch (user->render_size) {
      case MCLIP_PROXY_RENDER_SIZE_25:
        (*width) *= 4;
        (*height) *= 4;
        break;

      case MCLIP_PROXY_RENDER_SIZE_50:
        (*width) *= 2.0f;
        (*height) *= 2.0f;
        break;

      case MCLIP_PROXY_RENDER_SIZE_75:
        *width = ((float)*width) * 4.0f / 3.0f;
        *height = ((float)*height) * 4.0f / 3.0f;
        break;
    }
  }
}

static ImBuf *get_undistorted_ibuf(MovieClip *clip,
                                   struct MovieDistortion *distortion,
                                   ImBuf *ibuf)
{
  ImBuf *undistibuf;

  if (distortion) {
    undistibuf = BKE_tracking_distortion_exec(
        distortion, &clip->tracking, ibuf, ibuf->x, ibuf->y, 0.0f, 1);
  }
  else {
    undistibuf = BKE_tracking_undistort_frame(&clip->tracking, ibuf, ibuf->x, ibuf->y, 0.0f);
  }

  IMB_scaleImBuf(undistibuf, ibuf->x, ibuf->y);

  return undistibuf;
}

static bool need_undistortion_postprocess(const MovieClipUser *user, int clip_flag)
{
  bool result = 0;
  const bool uses_full_frame = ((clip_flag & MCLIP_USE_PROXY) == 0) ||
                               (user->render_size == MCLIP_PROXY_RENDER_SIZE_FULL);
  /* Only full undistorted render can be used as on-fly undistorting image. */
  result |= uses_full_frame && (user->render_flag & MCLIP_PROXY_RENDER_UNDISTORT) != 0;
  return result;
}

static bool need_postprocessed_frame(const MovieClipUser *user,
                                     int clip_flag,
                                     int postprocess_flag)
{
  bool result = (postprocess_flag != 0);
  result |= need_undistortion_postprocess(user, clip_flag);
  return result;
}

static bool check_undistortion_cache_flags(const MovieClip *clip)
{
  const MovieClipCache *cache = clip->cache;
  const MovieTrackingCamera *camera = &clip->tracking.camera;

  if (camera->focal != cache->postprocessed.focal_length) {
    return false;
  }

  /* check for distortion model changes */
  if (!equals_v2v2(camera->principal, cache->postprocessed.principal)) {
    return false;
  }

  if (camera->distortion_model != cache->postprocessed.distortion_model) {
    return false;
  }

  if (!equals_v3v3(&camera->k1, cache->postprocessed.polynomial_k)) {
    return false;
  }

  if (!equals_v2v2(&camera->division_k1, cache->postprocessed.division_k)) {
    return false;
  }

  if (!equals_v2v2(&camera->nuke_k1, cache->postprocessed.nuke_k)) {
    return false;
  }

  if (!equals_v4v4(&camera->brown_k1, cache->postprocessed.brown_k)) {
    return false;
  }
  if (!equals_v2v2(&camera->brown_p1, cache->postprocessed.brown_p)) {
    return false;
  }

  return true;
}

static ImBuf *get_postprocessed_cached_frame(const MovieClip *clip,
                                             const MovieClipUser *user,
                                             int flag,
                                             int postprocess_flag)
{
  const MovieClipCache *cache = clip->cache;
  int framenr = user->framenr;
  short proxy = IMB_PROXY_NONE;
  int render_flag = 0;

  if (flag & MCLIP_USE_PROXY) {
    proxy = rendersize_to_proxy(user, flag);
    render_flag = user->render_flag;
  }

  /* no cache or no cached postprocessed image */
  if (!clip->cache || !clip->cache->postprocessed.ibuf) {
    return NULL;
  }

  /* Postprocessing happened for other frame. */
  if (cache->postprocessed.framenr != framenr) {
    return NULL;
  }

  /* cached ibuf used different proxy settings */
  if (cache->postprocessed.render_flag != render_flag || cache->postprocessed.proxy != proxy) {
    return NULL;
  }

  if (cache->postprocessed.flag != postprocess_flag) {
    return NULL;
  }

  if (need_undistortion_postprocess(user, flag)) {
    if (!check_undistortion_cache_flags(clip)) {
      return NULL;
    }
  }
  else if (cache->postprocessed.undistortion_used) {
    return NULL;
  }

  IMB_refImBuf(cache->postprocessed.ibuf);

  return cache->postprocessed.ibuf;
}

static ImBuf *postprocess_frame(
    MovieClip *clip, const MovieClipUser *user, ImBuf *ibuf, int flag, int postprocess_flag)
{
  ImBuf *postproc_ibuf = NULL;

  if (need_undistortion_postprocess(user, flag)) {
    postproc_ibuf = get_undistorted_ibuf(clip, NULL, ibuf);
  }
  else {
    postproc_ibuf = IMB_dupImBuf(ibuf);
  }

  if (postprocess_flag) {
    bool disable_red = (postprocess_flag & MOVIECLIP_DISABLE_RED) != 0;
    bool disable_green = (postprocess_flag & MOVIECLIP_DISABLE_GREEN) != 0;
    bool disable_blue = (postprocess_flag & MOVIECLIP_DISABLE_BLUE) != 0;
    bool grayscale = (postprocess_flag & MOVIECLIP_PREVIEW_GRAYSCALE) != 0;

    if (disable_red || disable_green || disable_blue || grayscale) {
      BKE_tracking_disable_channels(postproc_ibuf, disable_red, disable_green, disable_blue, 1);
    }
  }

  return postproc_ibuf;
}

static void put_postprocessed_frame_to_cache(
    MovieClip *clip, const MovieClipUser *user, ImBuf *ibuf, int flag, int postprocess_flag)
{
  MovieClipCache *cache = clip->cache;
  MovieTrackingCamera *camera = &clip->tracking.camera;

  cache->postprocessed.framenr = user->framenr;
  cache->postprocessed.flag = postprocess_flag;

  if (flag & MCLIP_USE_PROXY) {
    cache->postprocessed.proxy = rendersize_to_proxy(user, flag);
    cache->postprocessed.render_flag = user->render_flag;
  }
  else {
    cache->postprocessed.proxy = IMB_PROXY_NONE;
    cache->postprocessed.render_flag = 0;
  }

  if (need_undistortion_postprocess(user, flag)) {
    cache->postprocessed.distortion_model = camera->distortion_model;
    cache->postprocessed.focal_length = camera->focal;
    copy_v2_v2(cache->postprocessed.principal, camera->principal);
    copy_v3_v3(cache->postprocessed.polynomial_k, &camera->k1);
    copy_v2_v2(cache->postprocessed.division_k, &camera->division_k1);
    copy_v2_v2(cache->postprocessed.nuke_k, &camera->nuke_k1);
    copy_v4_v4(cache->postprocessed.brown_k, &camera->brown_k1);
    copy_v2_v2(cache->postprocessed.brown_p, &camera->brown_p1);
    cache->postprocessed.undistortion_used = true;
  }
  else {
    cache->postprocessed.undistortion_used = false;
  }

  IMB_refImBuf(ibuf);

  if (cache->postprocessed.ibuf) {
    IMB_freeImBuf(cache->postprocessed.ibuf);
  }

  cache->postprocessed.ibuf = ibuf;
}

static ImBuf *movieclip_get_postprocessed_ibuf(
    MovieClip *clip, const MovieClipUser *user, int flag, int postprocess_flag, int cache_flag)
{
  ImBuf *ibuf = NULL;
  int framenr = user->framenr;
  bool need_postprocess = false;

  /* cache isn't threadsafe itself and also loading of movies
   * can't happen from concurrent threads that's why we use lock here */
  BLI_thread_lock(LOCK_MOVIECLIP);

  /* try to obtain cached postprocessed frame first */
  if (need_postprocessed_frame(user, flag, postprocess_flag)) {
    ibuf = get_postprocessed_cached_frame(clip, user, flag, postprocess_flag);

    if (!ibuf) {
      need_postprocess = true;
    }
  }

  if (!ibuf) {
    ibuf = get_imbuf_cache(clip, user, flag);
  }

  if (!ibuf) {
    bool use_sequence = false;

    /* undistorted proxies for movies should be read as image sequence */
    use_sequence = (user->render_flag & MCLIP_PROXY_RENDER_UNDISTORT) &&
                   (user->render_size != MCLIP_PROXY_RENDER_SIZE_FULL);

    if (clip->source == MCLIP_SRC_SEQUENCE || use_sequence) {
      ibuf = movieclip_load_sequence_file(clip, user, framenr, flag);
    }
    else {
      ibuf = movieclip_load_movie_file(clip, user, framenr, flag);
    }

    if (ibuf && (cache_flag & MOVIECLIP_CACHE_SKIP) == 0) {
      put_imbuf_cache(clip, user, ibuf, flag, true);
    }
  }

  if (ibuf) {
    clip->lastframe = framenr;
    real_ibuf_size(clip, user, ibuf, &clip->lastsize[0], &clip->lastsize[1]);

    /* Post-process frame and put to cache if needed. */
    if (need_postprocess) {
      ImBuf *tmpibuf = ibuf;
      ibuf = postprocess_frame(clip, user, tmpibuf, flag, postprocess_flag);
      IMB_freeImBuf(tmpibuf);
      if (ibuf && (cache_flag & MOVIECLIP_CACHE_SKIP) == 0) {
        put_postprocessed_frame_to_cache(clip, user, ibuf, flag, postprocess_flag);
      }
    }
  }

  BLI_thread_unlock(LOCK_MOVIECLIP);

  /* Fallback render in case proxies are not enabled or built */
  if (!ibuf && user->render_flag & MCLIP_PROXY_RENDER_USE_FALLBACK_RENDER &&
      user->render_size != MCLIP_PROXY_RENDER_SIZE_FULL) {
    MovieClipUser user_fallback = *user;
    user_fallback.render_size = MCLIP_PROXY_RENDER_SIZE_FULL;

    ibuf = movieclip_get_postprocessed_ibuf(
        clip, &user_fallback, flag, postprocess_flag, cache_flag);
  }

  return ibuf;
}

ImBuf *BKE_movieclip_get_ibuf(MovieClip *clip, MovieClipUser *user)
{
  return BKE_movieclip_get_ibuf_flag(clip, user, clip->flag, 0);
}

ImBuf *BKE_movieclip_get_ibuf_flag(MovieClip *clip, MovieClipUser *user, int flag, int cache_flag)
{
  return movieclip_get_postprocessed_ibuf(clip, user, flag, 0, cache_flag);
}

ImBuf *BKE_movieclip_get_postprocessed_ibuf(MovieClip *clip,
                                            MovieClipUser *user,
                                            int postprocess_flag)
{
  return movieclip_get_postprocessed_ibuf(clip, user, clip->flag, postprocess_flag, 0);
}

static ImBuf *get_stable_cached_frame(
    MovieClip *clip, MovieClipUser *user, ImBuf *reference_ibuf, int framenr, int postprocess_flag)
{
  MovieClipCache *cache = clip->cache;
  MovieTracking *tracking = &clip->tracking;
  ImBuf *stableibuf;
  float tloc[2], tscale, tangle;
  short proxy = IMB_PROXY_NONE;
  int render_flag = 0;
  int clip_framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, framenr);

  if (clip->flag & MCLIP_USE_PROXY) {
    proxy = rendersize_to_proxy(user, clip->flag);
    render_flag = user->render_flag;
  }

  /* there's no cached frame or it was calculated for another frame */
  if (!cache->stabilized.ibuf || cache->stabilized.framenr != framenr) {
    return NULL;
  }

  if (cache->stabilized.reference_ibuf != reference_ibuf) {
    return NULL;
  }

  /* cached ibuf used different proxy settings */
  if (cache->stabilized.render_flag != render_flag || cache->stabilized.proxy != proxy) {
    return NULL;
  }

  if (cache->stabilized.postprocess_flag != postprocess_flag) {
    return NULL;
  }

  /* stabilization also depends on pixel aspect ratio */
  if (cache->stabilized.aspect != tracking->camera.pixel_aspect) {
    return NULL;
  }

  if (cache->stabilized.filter != tracking->stabilization.filter) {
    return NULL;
  }

  stableibuf = cache->stabilized.ibuf;

  BKE_tracking_stabilization_data_get(
      clip, clip_framenr, stableibuf->x, stableibuf->y, tloc, &tscale, &tangle);

  /* check for stabilization parameters */
  if (tscale != cache->stabilized.scale || tangle != cache->stabilized.angle ||
      !equals_v2v2(tloc, cache->stabilized.loc)) {
    return NULL;
  }

  IMB_refImBuf(stableibuf);

  return stableibuf;
}

static ImBuf *put_stabilized_frame_to_cache(
    MovieClip *clip, MovieClipUser *user, ImBuf *ibuf, int framenr, int postprocess_flag)
{
  MovieClipCache *cache = clip->cache;
  MovieTracking *tracking = &clip->tracking;
  ImBuf *stableibuf;
  float tloc[2], tscale, tangle;
  int clip_framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, framenr);

  stableibuf = BKE_tracking_stabilize_frame(clip, clip_framenr, ibuf, tloc, &tscale, &tangle);

  copy_v2_v2(cache->stabilized.loc, tloc);

  cache->stabilized.reference_ibuf = ibuf;
  cache->stabilized.scale = tscale;
  cache->stabilized.angle = tangle;
  cache->stabilized.framenr = framenr;
  cache->stabilized.aspect = tracking->camera.pixel_aspect;
  cache->stabilized.filter = tracking->stabilization.filter;

  if (clip->flag & MCLIP_USE_PROXY) {
    cache->stabilized.proxy = rendersize_to_proxy(user, clip->flag);
    cache->stabilized.render_flag = user->render_flag;
  }
  else {
    cache->stabilized.proxy = IMB_PROXY_NONE;
    cache->stabilized.render_flag = 0;
  }

  cache->stabilized.postprocess_flag = postprocess_flag;

  if (cache->stabilized.ibuf) {
    IMB_freeImBuf(cache->stabilized.ibuf);
  }

  cache->stabilized.ibuf = stableibuf;

  IMB_refImBuf(stableibuf);

  return stableibuf;
}

ImBuf *BKE_movieclip_get_stable_ibuf(MovieClip *clip,
                                     MovieClipUser *user,
                                     float loc[2],
                                     float *scale,
                                     float *angle,
                                     int postprocess_flag)
{
  ImBuf *ibuf, *stableibuf = NULL;
  int framenr = user->framenr;

  ibuf = BKE_movieclip_get_postprocessed_ibuf(clip, user, postprocess_flag);

  if (!ibuf) {
    return NULL;
  }

  if (clip->tracking.stabilization.flag & TRACKING_2D_STABILIZATION) {
    MovieClipCache *cache = clip->cache;

    stableibuf = get_stable_cached_frame(clip, user, ibuf, framenr, postprocess_flag);

    if (!stableibuf) {
      stableibuf = put_stabilized_frame_to_cache(clip, user, ibuf, framenr, postprocess_flag);
    }

    if (loc) {
      copy_v2_v2(loc, cache->stabilized.loc);
    }

    if (scale) {
      *scale = cache->stabilized.scale;
    }

    if (angle) {
      *angle = cache->stabilized.angle;
    }
  }
  else {
    if (loc) {
      zero_v2(loc);
    }

    if (scale) {
      *scale = 1.0f;
    }

    if (angle) {
      *angle = 0.0f;
    }

    stableibuf = ibuf;
  }

  if (stableibuf != ibuf) {
    IMB_freeImBuf(ibuf);
    ibuf = stableibuf;
  }

  return ibuf;
}

bool BKE_movieclip_has_frame(MovieClip *clip, MovieClipUser *user)
{
  ImBuf *ibuf = BKE_movieclip_get_ibuf(clip, user);

  if (ibuf) {
    IMB_freeImBuf(ibuf);
    return true;
  }

  return false;
}

void BKE_movieclip_get_size(MovieClip *clip, MovieClipUser *user, int *width, int *height)
{
#if 0
  /* originally was needed to support image sequences with different image dimensions,
   * which might be useful for such things as reconstruction of unordered image sequence,
   * or painting/rotoscoping of non-equal-sized images, but this ended up in unneeded
   * cache lookups and even unwanted non-proxied files loading when doing mask parenting,
   * so let's disable this for now and assume image sequence consists of images with
   * equal sizes (sergey)
   * TODO(sergey): Support reading sequences of different resolution.
   */
  if (user->framenr == clip->lastframe) {
#endif
  if (clip->lastsize[0] != 0 && clip->lastsize[1] != 0) {
    *width = clip->lastsize[0];
    *height = clip->lastsize[1];
  }
  else {
    ImBuf *ibuf = BKE_movieclip_get_ibuf(clip, user);

    if (ibuf && ibuf->x && ibuf->y) {
      real_ibuf_size(clip, user, ibuf, width, height);
    }
    else {
      *width = clip->lastsize[0];
      *height = clip->lastsize[1];
    }

    if (ibuf) {
      IMB_freeImBuf(ibuf);
    }
  }
}
void BKE_movieclip_get_size_fl(MovieClip *clip, MovieClipUser *user, float size[2])
{
  int width, height;
  BKE_movieclip_get_size(clip, user, &width, &height);

  size[0] = (float)width;
  size[1] = (float)height;
}

int BKE_movieclip_get_duration(MovieClip *clip)
{
  if (!clip->len) {
    movieclip_calc_length(clip);
  }

  return clip->len;
}

float BKE_movieclip_get_fps(MovieClip *clip)
{
  if (clip->source != MCLIP_SRC_MOVIE) {
    return 0.0f;
  }
  movieclip_open_anim_file(clip);
  if (clip->anim == NULL) {
    return 0.0f;
  }
  short frs_sec;
  float frs_sec_base;
  if (IMB_anim_get_fps(clip->anim, &frs_sec, &frs_sec_base, true)) {
    return (float)frs_sec / frs_sec_base;
  }
  return 0.0f;
}

void BKE_movieclip_get_aspect(MovieClip *clip, float *aspx, float *aspy)
{
  *aspx = 1.0;

  /* x is always 1 */
  *aspy = clip->aspy / clip->aspx / clip->tracking.camera.pixel_aspect;
}

void BKE_movieclip_get_cache_segments(MovieClip *clip,
                                      MovieClipUser *user,
                                      int *r_totseg,
                                      int **r_points)
{
  *r_totseg = 0;
  *r_points = NULL;

  if (clip->cache) {
    int proxy = rendersize_to_proxy(user, clip->flag);

    BLI_thread_lock(LOCK_MOVIECLIP);
    IMB_moviecache_get_cache_segments(
        clip->cache->moviecache, proxy, user->render_flag, r_totseg, r_points);
    BLI_thread_unlock(LOCK_MOVIECLIP);
  }
}

void BKE_movieclip_user_set_frame(MovieClipUser *iuser, int framenr)
{
  /* TODO: clamp framenr here? */

  iuser->framenr = framenr;
}

static void free_buffers(MovieClip *clip)
{
  if (clip->cache) {
    IMB_moviecache_free(clip->cache->moviecache);

    if (clip->cache->postprocessed.ibuf) {
      IMB_freeImBuf(clip->cache->postprocessed.ibuf);
    }

    if (clip->cache->stabilized.ibuf) {
      IMB_freeImBuf(clip->cache->stabilized.ibuf);
    }

    MEM_freeN(clip->cache);
    clip->cache = NULL;
  }

  if (clip->anim) {
    IMB_free_anim(clip->anim);
    clip->anim = NULL;
  }

  MovieClip_RuntimeGPUTexture *tex;
  for (tex = clip->runtime.gputextures.first; tex; tex = tex->next) {
    for (int i = 0; i < TEXTARGET_COUNT; i++) {
      if (tex->gputexture[i] != NULL) {
        GPU_texture_free(tex->gputexture[i]);
        tex->gputexture[i] = NULL;
      }
    }
  }
  BLI_freelistN(&clip->runtime.gputextures);
}

void BKE_movieclip_clear_cache(MovieClip *clip)
{
  free_buffers(clip);
}

void BKE_movieclip_clear_proxy_cache(MovieClip *clip)
{
  if (clip->cache && clip->cache->moviecache) {
    IMB_moviecache_cleanup(clip->cache->moviecache, moviecache_check_free_proxy, NULL);
  }
}

void BKE_movieclip_reload(Main *bmain, MovieClip *clip)
{
  /* clear cache */
  free_buffers(clip);

  /* update clip source */
  detect_clip_source(bmain, clip);

  clip->lastsize[0] = clip->lastsize[1] = 0;
  movieclip_load_get_size(clip);

  movieclip_calc_length(clip);

  BKE_ntree_update_tag_id_changed(bmain, &clip->id);
}

void BKE_movieclip_update_scopes(MovieClip *clip, MovieClipUser *user, MovieClipScopes *scopes)
{
  if (scopes->ok) {
    return;
  }

  if (scopes->track_preview) {
    IMB_freeImBuf(scopes->track_preview);
    scopes->track_preview = NULL;
  }

  if (scopes->track_search) {
    IMB_freeImBuf(scopes->track_search);
    scopes->track_search = NULL;
  }

  scopes->marker = NULL;
  scopes->track = NULL;
  scopes->track_locked = true;

  scopes->scene_framenr = user->framenr;
  scopes->ok = true;

  if (clip == NULL) {
    return;
  }

  MovieTrackingTrack *track = BKE_tracking_track_get_active(&clip->tracking);
  if (track == NULL) {
    return;
  }

  const int framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, user->framenr);
  MovieTrackingMarker *marker = BKE_tracking_marker_get(track, framenr);

  scopes->marker = marker;
  scopes->track = track;

  if (marker->flag & MARKER_DISABLED) {
    scopes->track_disabled = true;
  }
  else {
    ImBuf *ibuf = BKE_movieclip_get_ibuf(clip, user);

    scopes->track_disabled = false;

    if (ibuf && (ibuf->rect || ibuf->rect_float)) {
      MovieTrackingMarker undist_marker = *marker;

      if (user->render_flag & MCLIP_PROXY_RENDER_UNDISTORT) {
        int width, height;
        float aspy = 1.0f / clip->tracking.camera.pixel_aspect;

        BKE_movieclip_get_size(clip, user, &width, &height);

        undist_marker.pos[0] *= width;
        undist_marker.pos[1] *= height * aspy;

        BKE_tracking_undistort_v2(
            &clip->tracking, width, height, undist_marker.pos, undist_marker.pos);

        undist_marker.pos[0] /= width;
        undist_marker.pos[1] /= height * aspy;
      }

      scopes->track_search = BKE_tracking_get_search_imbuf(
          ibuf, track, &undist_marker, true, true);

      scopes->undist_marker = undist_marker;

      scopes->frame_width = ibuf->x;
      scopes->frame_height = ibuf->y;

      scopes->use_track_mask = (track->flag & TRACK_PREVIEW_ALPHA) != 0;
    }

    IMB_freeImBuf(ibuf);
  }

  if ((track->flag & TRACK_LOCKED) == 0) {
    float pat_min[2], pat_max[2];

    scopes->track_locked = false;

    /* XXX: would work fine with non-transformed patterns, but would likely fail
     *      with transformed patterns, but that would be easier to debug when
     *      we'll have real pattern sampling (at least to test) */
    BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

    scopes->slide_scale[0] = pat_max[0] - pat_min[0];
    scopes->slide_scale[1] = pat_max[1] - pat_min[1];
  }
}

static void movieclip_build_proxy_ibuf(
    MovieClip *clip, ImBuf *ibuf, int cfra, int proxy_render_size, bool undistorted, bool threaded)
{
  char name[FILE_MAX];
  int quality, rectx, recty;
  int size = rendersize_to_number(proxy_render_size);
  ImBuf *scaleibuf;

  get_proxy_fname(clip, proxy_render_size, undistorted, cfra, name);

  rectx = ibuf->x * size / 100.0f;
  recty = ibuf->y * size / 100.0f;

  scaleibuf = IMB_dupImBuf(ibuf);

  if (threaded) {
    IMB_scaleImBuf_threaded(scaleibuf, (short)rectx, (short)recty);
  }
  else {
    IMB_scaleImBuf(scaleibuf, (short)rectx, (short)recty);
  }

  quality = clip->proxy.quality;
  scaleibuf->ftype = IMB_FTYPE_JPG;
  scaleibuf->foptions.quality = quality;
  /* unsupported feature only confuses other s/w */
  if (scaleibuf->planes == 32) {
    scaleibuf->planes = 24;
  }

  /* TODO: currently the most weak part of multi-threaded proxies,
   *       could be solved in a way that thread only prepares memory
   *       buffer and write to disk happens separately
   */
  BLI_thread_lock(LOCK_MOVIECLIP);

  BLI_make_existing_file(name);
  if (IMB_saveiff(scaleibuf, name, IB_rect) == 0) {
    perror(name);
  }

  BLI_thread_unlock(LOCK_MOVIECLIP);

  IMB_freeImBuf(scaleibuf);
}

void BKE_movieclip_build_proxy_frame(MovieClip *clip,
                                     int clip_flag,
                                     struct MovieDistortion *distortion,
                                     int cfra,
                                     int *build_sizes,
                                     int build_count,
                                     bool undistorted)
{
  ImBuf *ibuf;
  MovieClipUser user;

  if (!build_count) {
    return;
  }

  user.framenr = cfra;
  user.render_flag = 0;
  user.render_size = MCLIP_PROXY_RENDER_SIZE_FULL;

  ibuf = BKE_movieclip_get_ibuf_flag(clip, &user, clip_flag, MOVIECLIP_CACHE_SKIP);

  if (ibuf) {
    ImBuf *tmpibuf = ibuf;
    int i;

    if (undistorted) {
      tmpibuf = get_undistorted_ibuf(clip, distortion, ibuf);
    }

    for (i = 0; i < build_count; i++) {
      movieclip_build_proxy_ibuf(clip, tmpibuf, cfra, build_sizes[i], undistorted, true);
    }

    IMB_freeImBuf(ibuf);

    if (tmpibuf != ibuf) {
      IMB_freeImBuf(tmpibuf);
    }
  }
}

void BKE_movieclip_build_proxy_frame_for_ibuf(MovieClip *clip,
                                              ImBuf *ibuf,
                                              struct MovieDistortion *distortion,
                                              int cfra,
                                              int *build_sizes,
                                              int build_count,
                                              bool undistorted)
{
  if (!build_count) {
    return;
  }

  if (ibuf) {
    ImBuf *tmpibuf = ibuf;
    int i;

    if (undistorted) {
      tmpibuf = get_undistorted_ibuf(clip, distortion, ibuf);
    }

    for (i = 0; i < build_count; i++) {
      movieclip_build_proxy_ibuf(clip, tmpibuf, cfra, build_sizes[i], undistorted, false);
    }

    if (tmpibuf != ibuf) {
      IMB_freeImBuf(tmpibuf);
    }
  }
}

bool BKE_movieclip_proxy_enabled(MovieClip *clip)
{
  return clip->flag & MCLIP_USE_PROXY;
}

float BKE_movieclip_remap_scene_to_clip_frame(const MovieClip *clip, float framenr)
{
  return framenr - (float)clip->start_frame + 1.0f;
}

float BKE_movieclip_remap_clip_to_scene_frame(const MovieClip *clip, float framenr)
{
  return framenr + (float)clip->start_frame - 1.0f;
}

void BKE_movieclip_filename_for_frame(MovieClip *clip, MovieClipUser *user, char *name)
{
  if (clip->source == MCLIP_SRC_SEQUENCE) {
    int use_proxy;

    use_proxy = (clip->flag & MCLIP_USE_PROXY) &&
                user->render_size != MCLIP_PROXY_RENDER_SIZE_FULL;

    if (use_proxy) {
      int undistort = user->render_flag & MCLIP_PROXY_RENDER_UNDISTORT;
      get_proxy_fname(clip, user->render_size, undistort, user->framenr, name);
    }
    else {
      get_sequence_fname(clip, user->framenr, name);
    }
  }
  else {
    BLI_strncpy(name, clip->filepath, FILE_MAX);
    BLI_path_abs(name, ID_BLEND_PATH_FROM_GLOBAL(&clip->id));
  }
}

ImBuf *BKE_movieclip_anim_ibuf_for_frame_no_lock(MovieClip *clip, MovieClipUser *user)
{
  ImBuf *ibuf = NULL;

  if (clip->source == MCLIP_SRC_MOVIE) {
    ibuf = movieclip_load_movie_file(clip, user, user->framenr, clip->flag);
  }

  return ibuf;
}

bool BKE_movieclip_has_cached_frame(MovieClip *clip, MovieClipUser *user)
{
  bool has_frame = false;

  BLI_thread_lock(LOCK_MOVIECLIP);
  has_frame = has_imbuf_cache(clip, user, clip->flag);
  BLI_thread_unlock(LOCK_MOVIECLIP);

  return has_frame;
}

bool BKE_movieclip_put_frame_if_possible(MovieClip *clip, MovieClipUser *user, ImBuf *ibuf)
{
  bool result;

  BLI_thread_lock(LOCK_MOVIECLIP);
  result = put_imbuf_cache(clip, user, ibuf, clip->flag, false);
  BLI_thread_unlock(LOCK_MOVIECLIP);

  return result;
}

static void movieclip_selection_sync(MovieClip *clip_dst, const MovieClip *clip_src)
{
  BLI_assert(clip_dst != clip_src);
  MovieTracking *tracking_dst = &clip_dst->tracking, tracking_src = clip_src->tracking;
  /* Syncs the active object, track and plane track. */
  tracking_dst->objectnr = tracking_src.objectnr;
  const int active_track_index = BLI_findindex(&tracking_src.tracks, tracking_src.act_track);
  const int active_plane_track_index = BLI_findindex(&tracking_src.plane_tracks,
                                                     tracking_src.act_plane_track);
  tracking_dst->act_track = BLI_findlink(&tracking_dst->tracks, active_track_index);
  tracking_dst->act_plane_track = BLI_findlink(&tracking_dst->plane_tracks,
                                               active_plane_track_index);

  /* Syncs the tracking selection flag. */
  MovieTrackingObject *tracking_object_dst, *tracking_object_src;
  tracking_object_src = tracking_src.objects.first;

  for (tracking_object_dst = tracking_dst->objects.first; tracking_object_dst != NULL;
       tracking_object_dst = tracking_object_dst->next,
      tracking_object_src = tracking_object_src->next) {
    ListBase *tracksbase_dst, *tracksbase_src;
    tracksbase_dst = BKE_tracking_object_get_tracks(tracking_dst, tracking_object_dst);
    tracksbase_src = BKE_tracking_object_get_tracks(&tracking_src, tracking_object_src);

    MovieTrackingTrack *track_dst, *track_src;
    track_src = tracksbase_src->first;
    for (track_dst = tracksbase_dst->first; track_dst != NULL;
         track_dst = track_dst->next, track_src = track_src->next) {
      track_dst->flag = track_src->flag;
      track_dst->pat_flag = track_src->pat_flag;
      track_dst->search_flag = track_src->search_flag;
    }
  }
}

static void movieclip_eval_update_reload(struct Depsgraph *depsgraph, Main *bmain, MovieClip *clip)
{
  BKE_movieclip_reload(bmain, clip);
  if (DEG_is_active(depsgraph)) {
    MovieClip *clip_orig = (MovieClip *)DEG_get_original_id(&clip->id);
    BKE_movieclip_reload(bmain, clip_orig);
  }
}

static void movieclip_eval_update_generic(struct Depsgraph *depsgraph, MovieClip *clip)
{
  BKE_tracking_dopesheet_tag_update(&clip->tracking);
  if (DEG_is_active(depsgraph)) {
    MovieClip *clip_orig = (MovieClip *)DEG_get_original_id(&clip->id);
    BKE_tracking_dopesheet_tag_update(&clip_orig->tracking);
  }
}

void BKE_movieclip_eval_update(struct Depsgraph *depsgraph, Main *bmain, MovieClip *clip)
{
  DEG_debug_print_eval(depsgraph, __func__, clip->id.name, clip);
  if (clip->id.recalc & ID_RECALC_SOURCE) {
    movieclip_eval_update_reload(depsgraph, bmain, clip);
  }
  else {
    movieclip_eval_update_generic(depsgraph, clip);
  }
}

void BKE_movieclip_eval_selection_update(struct Depsgraph *depsgraph, MovieClip *clip)
{
  DEG_debug_print_eval(depsgraph, __func__, clip->id.name, clip);
  movieclip_selection_sync(clip, (MovieClip *)clip->id.orig_id);
}

/* -------------------------------------------------------------------- */
/** \name GPU textures
 * \{ */

static GPUTexture **movieclip_get_gputexture_ptr(MovieClip *clip,
                                                 MovieClipUser *cuser,
                                                 eGPUTextureTarget textarget)
{
  /* Check if we have an existing entry for that clip user. */
  MovieClip_RuntimeGPUTexture *tex;
  for (tex = clip->runtime.gputextures.first; tex; tex = tex->next) {
    if (memcmp(&tex->user, cuser, sizeof(MovieClipUser)) == 0) {
      break;
    }
  }

  /* If not, allocate a new one. */
  if (tex == NULL) {
    tex = (MovieClip_RuntimeGPUTexture *)MEM_mallocN(sizeof(MovieClip_RuntimeGPUTexture),
                                                     __func__);

    for (int i = 0; i < TEXTARGET_COUNT; i++) {
      tex->gputexture[i] = NULL;
    }

    memcpy(&tex->user, cuser, sizeof(MovieClipUser));
    BLI_addtail(&clip->runtime.gputextures, tex);
  }

  return &tex->gputexture[textarget];
}

GPUTexture *BKE_movieclip_get_gpu_texture(MovieClip *clip, MovieClipUser *cuser)
{
  if (clip == NULL) {
    return NULL;
  }

  GPUTexture **tex = movieclip_get_gputexture_ptr(clip, cuser, TEXTARGET_2D);
  if (*tex) {
    return *tex;
  }

  /* check if we have a valid image buffer */
  ImBuf *ibuf = BKE_movieclip_get_ibuf(clip, cuser);
  if (ibuf == NULL) {
    fprintf(stderr, "GPUTexture: Blender Texture Not Loaded!\n");
    *tex = GPU_texture_create_error(2, false);
    return *tex;
  }

  /* This only means RGBA16F instead of RGBA32F. */
  const bool high_bitdepth = false;
  const bool store_premultiplied = ibuf->rect_float ? false : true;
  *tex = IMB_create_gpu_texture(clip->id.name + 2, ibuf, high_bitdepth, store_premultiplied);

  /* Do not generate mips for movieclips... too slow. */
  GPU_texture_mipmap_mode(*tex, false, true);

  IMB_freeImBuf(ibuf);

  return *tex;
}

void BKE_movieclip_free_gputexture(struct MovieClip *clip)
{
  /* Number of gpu textures to keep around as cache.
   * We don't want to keep too many GPU textures for
   * movie clips around, as they can be large. */
  const int MOVIECLIP_NUM_GPUTEXTURES = 1;

  while (BLI_listbase_count(&clip->runtime.gputextures) > MOVIECLIP_NUM_GPUTEXTURES) {
    MovieClip_RuntimeGPUTexture *tex = (MovieClip_RuntimeGPUTexture *)BLI_pophead(
        &clip->runtime.gputextures);
    for (int i = 0; i < TEXTARGET_COUNT; i++) {
      /* free glsl image binding */
      if (tex->gputexture[i]) {
        GPU_texture_free(tex->gputexture[i]);
        tex->gputexture[i] = NULL;
      }
    }
    MEM_freeN(tex);
  }
}

/** \} */
