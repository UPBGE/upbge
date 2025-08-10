/* SPDX-FileCopyrightText: 2012 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_threads.h"

#include "BKE_attribute.hh"
#include "BKE_ccg.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_derived_mesh.hh"
#include "BKE_mesh_tangent.hh"
#include "BKE_multires.hh"
#include "BKE_subsurf.hh"

#include "DEG_depsgraph.hh"

#include "RE_multires_bake.h"
#include "RE_pipeline.h"
#include "RE_texture_margin.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::render {
namespace {

using MPassKnownData = void (*)(Span<float3> vert_positions,
                                Span<float3> vert_normals,
                                OffsetIndices<int> faces,
                                Span<int> corner_verts,
                                Span<int3> corner_tris,
                                Span<int> tri_faces,
                                Span<float2> uv_map,
                                DerivedMesh *hires_dm,
                                void *thread_data,
                                void *bake_data,
                                ImBuf *ibuf,
                                int face_index,
                                int lvl,
                                const float st[2],
                                float tangmat[3][3],
                                int x,
                                int y);

using MInitBakeData = void *(*)(MultiresBakeRender &bake, ImBuf *ibuf);
using MFreeBakeData = void (*)(void *bake_data);

struct MultiresBakeResult {
  float height_min, height_max;
};

struct MResolvePixelData {
  /* Data from low-resolution mesh. */
  Span<float3> vert_positions;
  OffsetIndices<int> faces;
  Span<int> corner_verts;
  Span<int3> corner_tris;
  Span<int> tri_faces;
  Span<float3> vert_normals;
  Span<float3> face_normals;

  Span<float2> uv_map;

  /* May be null. */
  const int *material_indices;
  const bool *sharp_faces;

  float uv_offset[2];
  Span<float4> pvtangent;
  int w, h;
  int tri_index;

  DerivedMesh *hires_dm;

  int lvl;
  void *thread_data;
  void *bake_data;
  ImBuf *ibuf;
  MPassKnownData pass_data;
  /* material aligned UV array */
  Image **image_array;
};

using MFlushPixel = void (*)(const MResolvePixelData *data, int x, int y);

struct MBakeRast {
  int w, h;
  char *texels;
  const MResolvePixelData *data;
  MFlushPixel flush_pixel;
  bool *do_update;
};

struct MHeightBakeData {
  float *heights;
  DerivedMesh *ssdm;
  const int *orig_index_mp_to_orig;
};

struct MNormalBakeData {
  const int *orig_index_mp_to_orig;
};

struct BakeImBufuserData {
  float *displacement_buffer;
  char *mask_buffer;
};

static void multiresbake_get_normal(const MResolvePixelData *data,
                                    const int tri_num,
                                    const int vert_index,
                                    float r_normal[3])
{
  const int face_index = data->tri_faces[tri_num];
  const bool smoothnormal = !(data->sharp_faces && data->sharp_faces[face_index]);

  if (smoothnormal) {
    const int vi = data->corner_verts[data->corner_tris[tri_num][vert_index]];
    copy_v3_v3(r_normal, data->vert_normals[vi]);
  }
  else {
    copy_v3_v3(r_normal, data->face_normals[face_index]);
  }
}

static void init_bake_rast(MBakeRast *bake_rast,
                           const ImBuf *ibuf,
                           const MResolvePixelData *data,
                           MFlushPixel flush_pixel,
                           bool *do_update)
{
  BakeImBufuserData *userdata = (BakeImBufuserData *)ibuf->userdata;

  memset(bake_rast, 0, sizeof(MBakeRast));

  bake_rast->texels = userdata->mask_buffer;
  bake_rast->w = ibuf->x;
  bake_rast->h = ibuf->y;
  bake_rast->data = data;
  bake_rast->flush_pixel = flush_pixel;
  bake_rast->do_update = do_update;
}

static void flush_pixel(const MResolvePixelData *data, const int x, const int y)
{
  const float st[2] = {(x + 0.5f) / data->w + data->uv_offset[0],
                       (y + 0.5f) / data->h + data->uv_offset[1]};
  const float *st0, *st1, *st2;
  float no0[3], no1[3], no2[3];
  float fUV[2], from_tang[3][3], to_tang[3][3];
  float u, v, w, sign;
  int r;

  st0 = data->uv_map[data->corner_tris[data->tri_index][0]];
  st1 = data->uv_map[data->corner_tris[data->tri_index][1]];
  st2 = data->uv_map[data->corner_tris[data->tri_index][2]];

  multiresbake_get_normal(data, data->tri_index, 0, no0); /* can optimize these 3 into one call */
  multiresbake_get_normal(data, data->tri_index, 1, no1);
  multiresbake_get_normal(data, data->tri_index, 2, no2);

  resolve_tri_uv_v2(fUV, st, st0, st1, st2);

  u = fUV[0];
  v = fUV[1];
  w = 1 - u - v;

  if (!data->pvtangent.is_empty()) {
    const float4 &tang0 = data->pvtangent[data->corner_tris[data->tri_index][0]];
    const float4 &tang1 = data->pvtangent[data->corner_tris[data->tri_index][1]];
    const float4 &tang2 = data->pvtangent[data->corner_tris[data->tri_index][2]];

    /* the sign is the same at all face vertices for any non-degenerate face.
     * Just in case we clamp the interpolated value though. */
    sign = (tang0[3] * u + tang1[3] * v + tang2[3] * w) < 0 ? (-1.0f) : 1.0f;

    /* this sequence of math is designed specifically as is with great care
     * to be compatible with our shader. Please don't change without good reason. */
    for (r = 0; r < 3; r++) {
      from_tang[0][r] = tang0[r] * u + tang1[r] * v + tang2[r] * w;
      from_tang[2][r] = no0[r] * u + no1[r] * v + no2[r] * w;
    }

    cross_v3_v3v3(from_tang[1], from_tang[2], from_tang[0]); /* `B = sign * cross(N, T)` */
    mul_v3_fl(from_tang[1], sign);
    invert_m3_m3(to_tang, from_tang);
  }
  else {
    zero_m3(to_tang);
  }

  data->pass_data(data->vert_positions,
                  data->vert_normals,
                  data->faces,
                  data->corner_verts,
                  data->corner_tris,
                  data->tri_faces,
                  data->uv_map,
                  data->hires_dm,
                  data->thread_data,
                  data->bake_data,
                  data->ibuf,
                  data->tri_index,
                  data->lvl,
                  st,
                  to_tang,
                  x,
                  y);
}

static void set_rast_triangle(const MBakeRast *bake_rast, const int x, const int y)
{
  const int w = bake_rast->w;
  const int h = bake_rast->h;

  if (x >= 0 && x < w && y >= 0 && y < h) {
    if ((bake_rast->texels[y * w + x]) == 0) {
      bake_rast->texels[y * w + x] = FILTER_MASK_USED;
      flush_pixel(bake_rast->data, x, y);
      if (bake_rast->do_update) {
        *bake_rast->do_update = true;
      }
    }
  }
}

static void rasterize_half(const MBakeRast *bake_rast,
                           const float s0_s,
                           const float t0_s,
                           const float s1_s,
                           const float t1_s,
                           const float s0_l,
                           const float t0_l,
                           const float s1_l,
                           const float t1_l,
                           const int y0_in,
                           const int y1_in,
                           const int is_mid_right)
{
  const int s_stable = fabsf(t1_s - t0_s) > FLT_EPSILON ? 1 : 0;
  const int l_stable = fabsf(t1_l - t0_l) > FLT_EPSILON ? 1 : 0;
  const int w = bake_rast->w;
  const int h = bake_rast->h;
  int y, y0, y1;

  if (y1_in <= 0 || y0_in >= h) {
    return;
  }

  y0 = y0_in < 0 ? 0 : y0_in;
  y1 = y1_in >= h ? h : y1_in;

  for (y = y0; y < y1; y++) {
    /*-b(x-x0) + a(y-y0) = 0 */
    int iXl, iXr, x;
    float x_l = s_stable != 0 ? (s0_s + (((s1_s - s0_s) * (y - t0_s)) / (t1_s - t0_s))) : s0_s;
    float x_r = l_stable != 0 ? (s0_l + (((s1_l - s0_l) * (y - t0_l)) / (t1_l - t0_l))) : s0_l;

    if (is_mid_right != 0) {
      std::swap(x_l, x_r);
    }

    iXl = int(ceilf(x_l));
    iXr = int(ceilf(x_r));

    if (iXr > 0 && iXl < w) {
      iXl = iXl < 0 ? 0 : iXl;
      iXr = iXr >= w ? w : iXr;

      for (x = iXl; x < iXr; x++) {
        set_rast_triangle(bake_rast, x, y);
      }
    }
  }
}

static void bake_rasterize(const MBakeRast *bake_rast,
                           const float st0_in[2],
                           const float st1_in[2],
                           const float st2_in[2])
{
  const int w = bake_rast->w;
  const int h = bake_rast->h;
  float slo = st0_in[0] * w - 0.5f;
  float tlo = st0_in[1] * h - 0.5f;
  float smi = st1_in[0] * w - 0.5f;
  float tmi = st1_in[1] * h - 0.5f;
  float shi = st2_in[0] * w - 0.5f;
  float thi = st2_in[1] * h - 0.5f;
  int is_mid_right = 0, ylo, yhi, yhi_beg;

  /* skip degenerates */
  if ((slo == smi && tlo == tmi) || (slo == shi && tlo == thi) || (smi == shi && tmi == thi)) {
    return;
  }

  /* sort by T */
  if (tlo > tmi && tlo > thi) {
    std::swap(shi, slo);
    std::swap(thi, tlo);
  }
  else if (tmi > thi) {
    std::swap(shi, smi);
    std::swap(thi, tmi);
  }

  if (tlo > tmi) {
    std::swap(slo, smi);
    std::swap(tlo, tmi);
  }

  /* check if mid point is to the left or to the right of the lo-hi edge */
  is_mid_right = (-(shi - slo) * (tmi - thi) + (thi - tlo) * (smi - shi)) > 0 ? 1 : 0;
  ylo = int(ceilf(tlo));
  yhi_beg = int(ceilf(tmi));
  yhi = int(ceilf(thi));

  // if (fTmi>ceilf(fTlo))
  rasterize_half(bake_rast, slo, tlo, smi, tmi, slo, tlo, shi, thi, ylo, yhi_beg, is_mid_right);
  rasterize_half(bake_rast, smi, tmi, shi, thi, slo, tlo, shi, thi, yhi_beg, yhi, is_mid_right);
}

static bool multiresbake_test_break(const MultiresBakeRender &bake)
{
  if (!bake.stop) {
    /* This means baker is executed outside from job system. */
    return false;
  }
  return *bake.stop || G.is_break;
}

/* **** Threading routines **** */

struct MultiresBakeQueue {
  int cur_tri;
  int tot_tri;
  SpinLock spin;
};

struct MultiresBakeThread {
  /* this data is actually shared between all the threads */
  MultiresBakeQueue *queue;
  MultiresBakeRender *bake;
  Image *image;
  void *bake_data;
  int num_total_faces;

  /* thread-specific data */
  MBakeRast bake_rast;
  MResolvePixelData data;

  /* displacement-specific data */
  float height_min, height_max;
};

static int multires_bake_queue_next_tri(MultiresBakeQueue *queue)
{
  int face = -1;

  /* TODO: it could worth making it so thread will handle neighbor faces
   *       for better memory cache utilization
   */

  BLI_spin_lock(&queue->spin);
  if (queue->cur_tri < queue->tot_tri) {
    face = queue->cur_tri;
    queue->cur_tri++;
  }
  BLI_spin_unlock(&queue->spin);

  return face;
}

static void *do_multires_bake_thread(void *data_v)
{
  MultiresBakeThread *handle = (MultiresBakeThread *)data_v;
  MResolvePixelData *data = &handle->data;
  MBakeRast *bake_rast = &handle->bake_rast;
  MultiresBakeRender &bake = *handle->bake;
  int tri_index;

  while ((tri_index = multires_bake_queue_next_tri(handle->queue)) >= 0) {
    const int3 &tri = data->corner_tris[tri_index];
    const int face_i = data->tri_faces[tri_index];
    const short mat_nr = data->material_indices == nullptr ? 0 : data->material_indices[face_i];

    if (multiresbake_test_break(bake)) {
      break;
    }

    Image *tri_image = mat_nr < bake.ob_image.size() ? bake.ob_image[mat_nr] : nullptr;
    if (tri_image != handle->image) {
      continue;
    }

    data->tri_index = tri_index;

    float uv[3][2];
    sub_v2_v2v2(uv[0], data->uv_map[tri[0]], data->uv_offset);
    sub_v2_v2v2(uv[1], data->uv_map[tri[1]], data->uv_offset);
    sub_v2_v2v2(uv[2], data->uv_map[tri[2]], data->uv_offset);

    bake_rasterize(bake_rast, uv[0], uv[1], uv[2]);

    /* tag image buffer for refresh */
    if (data->ibuf->float_buffer.data) {
      data->ibuf->userflags |= IB_RECT_INVALID;
    }

    data->ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

    /* update progress */
    BLI_spin_lock(&handle->queue->spin);
    bake.baked_faces++;

    if (bake.do_update) {
      *bake.do_update = true;
    }

    if (bake.progress) {
      *bake.progress = (float(bake.baked_objects) +
                        float(bake.baked_faces) / handle->num_total_faces) /
                       bake.tot_obj;
    }
    BLI_spin_unlock(&handle->queue->spin);
  }

  return nullptr;
}

/* some of arrays inside ccgdm are lazy-initialized, which will generally
 * require lock around accessing such data
 * this function will ensure all arrays are allocated before threading started
 */
static void init_ccgdm_arrays(DerivedMesh *dm)
{
  CCGElem **grid_data;
  CCGKey key;
  int grid_size;
  const int *grid_offset;

  grid_size = dm->getGridSize(dm);
  grid_data = dm->getGridData(dm);
  grid_offset = dm->getGridOffset(dm);
  dm->getGridKey(dm, &key);

  (void)grid_size;
  (void)grid_data;
  (void)grid_offset;
}

static void do_multires_bake(MultiresBakeRender &bake,
                             Image *image,
                             ImageTile *tile,
                             ImBuf *ibuf,
                             const bool require_tangent,
                             const MPassKnownData passKnownData,
                             const MInitBakeData initBakeData,
                             const MFreeBakeData freeBakeData,
                             MultiresBakeResult &result)
{
  DerivedMesh *dm = bake.lores_dm;
  const int lvl = bake.lvl;
  if (dm->getNumPolys(dm) == 0) {
    return;
  }

  const Span<float2> uv_map(
      reinterpret_cast<const float2 *>(dm->getLoopDataArray(dm, CD_PROP_FLOAT2)),
      dm->getNumLoops(dm));

  Array<float4> pvtangent;

  Mesh *temp_mesh = BKE_mesh_new_nomain(
      dm->getNumVerts(dm), dm->getNumEdges(dm), dm->getNumPolys(dm), dm->getNumLoops(dm));
  temp_mesh->vert_positions_for_write().copy_from(
      {reinterpret_cast<const float3 *>(dm->getVertArray(dm)), temp_mesh->verts_num});
  temp_mesh->edges_for_write().copy_from(
      {reinterpret_cast<const int2 *>(dm->getEdgeArray(dm)), temp_mesh->edges_num});
  temp_mesh->face_offsets_for_write().copy_from({dm->getPolyArray(dm), temp_mesh->faces_num + 1});
  temp_mesh->corner_verts_for_write().copy_from(
      {dm->getCornerVertArray(dm), temp_mesh->corners_num});
  temp_mesh->corner_edges_for_write().copy_from(
      {dm->getCornerEdgeArray(dm), temp_mesh->corners_num});

  const Span<float3> positions = temp_mesh->vert_positions();
  const OffsetIndices faces = temp_mesh->faces();
  const Span<int> corner_verts = temp_mesh->corner_verts();
  const Span<float3> vert_normals = temp_mesh->vert_normals();
  const Span<float3> face_normals = temp_mesh->face_normals();
  const Span<int3> corner_tris = temp_mesh->corner_tris();
  const Span<int> tri_faces = temp_mesh->corner_tri_faces();

  if (require_tangent) {
    const bool *sharp_edges = static_cast<const bool *>(
        CustomData_get_layer_named(&dm->edgeData, CD_PROP_BOOL, "sharp_edge"));
    const bool *sharp_faces = static_cast<const bool *>(
        CustomData_get_layer_named(&dm->polyData, CD_PROP_BOOL, "sharp_face"));

    /* Copy sharp faces and edges, for corner normals domain and tangents
     * to be computed correctly. */
    if (sharp_edges != nullptr) {
      bke::MutableAttributeAccessor attributes = temp_mesh->attributes_for_write();
      attributes.add<bool>("sharp_edge",
                           bke::AttrDomain::Edge,
                           bke::AttributeInitVArray(VArray<bool>::from_span(
                               Span<bool>(sharp_edges, temp_mesh->edges_num))));
    }
    if (sharp_faces != nullptr) {
      bke::MutableAttributeAccessor attributes = temp_mesh->attributes_for_write();
      attributes.add<bool>("sharp_face",
                           bke::AttrDomain::Face,
                           bke::AttributeInitVArray(VArray<bool>::from_span(
                               Span<bool>(sharp_faces, temp_mesh->faces_num))));
    }

    const Span<float3> corner_normals = temp_mesh->corner_normals();
    Array<Array<float4>> tangent_data = bke::mesh::calc_uv_tangents(
        positions,
        faces,
        corner_verts,
        corner_tris,
        tri_faces,
        sharp_faces ? Span(sharp_faces, faces.size()) : Span<bool>(),
        vert_normals,
        face_normals,
        corner_normals,
        {uv_map});

    pvtangent = std::move(tangent_data[0]);
  }

  /* All threads share the same custom bake data. */
  void *bake_data = nullptr;
  if (initBakeData) {
    bake_data = initBakeData(bake, ibuf);
  }

  ListBase threads;
  const int tot_thread = bake.threads > 0 ? bake.threads : BLI_system_thread_count();
  if (tot_thread > 1) {
    BLI_threadpool_init(&threads, do_multires_bake_thread, tot_thread);
  }

  Array<MultiresBakeThread> handles(tot_thread);

  init_ccgdm_arrays(bake.hires_dm);

  /* Faces queue. */
  MultiresBakeQueue queue;
  queue.cur_tri = 0;
  queue.tot_tri = corner_tris.size();
  BLI_spin_init(&queue.spin);

  /* Fill in threads handles. */
  for (int i = 0; i < tot_thread; i++) {
    MultiresBakeThread *handle = &handles[i];

    handle->bake = &bake;
    handle->image = image;
    handle->num_total_faces = queue.tot_tri * BLI_listbase_count(&image->tiles);
    handle->queue = &queue;

    handle->data.vert_positions = positions;
    handle->data.faces = faces;
    handle->data.corner_verts = corner_verts;
    handle->data.corner_tris = corner_tris;
    handle->data.tri_faces = tri_faces;
    handle->data.vert_normals = vert_normals;
    handle->data.face_normals = face_normals;
    handle->data.material_indices = static_cast<const int *>(
        CustomData_get_layer_named(&dm->polyData, CD_PROP_INT32, "material_index"));
    handle->data.sharp_faces = static_cast<const bool *>(
        CustomData_get_layer_named(&dm->polyData, CD_PROP_BOOL, "sharp_face"));
    handle->data.uv_map = uv_map;
    BKE_image_get_tile_uv(image, tile->tile_number, handle->data.uv_offset);
    handle->data.pvtangent = pvtangent;
    handle->data.w = ibuf->x;
    handle->data.h = ibuf->y;
    handle->data.hires_dm = bake.hires_dm;
    handle->data.lvl = lvl;
    handle->data.pass_data = passKnownData;
    handle->data.thread_data = handle;
    handle->data.bake_data = bake_data;
    handle->data.ibuf = ibuf;

    handle->height_min = FLT_MAX;
    handle->height_max = -FLT_MAX;

    init_bake_rast(&handle->bake_rast, ibuf, &handle->data, flush_pixel, bake.do_update);

    if (tot_thread > 1) {
      BLI_threadpool_insert(&threads, handle);
    }
  }

  /* Run threads. */
  if (tot_thread > 1) {
    BLI_threadpool_end(&threads);
  }
  else {
    do_multires_bake_thread(handles.data());
  }

  for (int i = 0; i < tot_thread; i++) {
    result.height_min = min_ff(result.height_min, handles[i].height_min);
    result.height_max = max_ff(result.height_max, handles[i].height_max);
  }

  BLI_spin_end(&queue.spin);

  /* Finalize baking. */
  if (freeBakeData) {
    freeBakeData(bake_data);
  }

  BKE_id_free(nullptr, temp_mesh);
}

/* mode = 0: interpolate normals,
 * mode = 1: interpolate coord */
static void interp_bilinear_grid(
    const CCGKey &key, CCGElem *grid, float crn_x, float crn_y, int mode, float res[3])
{
  int x0, x1, y0, y1;
  float u, v;
  float data[4][3];

  x0 = int(crn_x);
  x1 = x0 >= (key.grid_size - 1) ? (key.grid_size - 1) : (x0 + 1);

  y0 = int(crn_y);
  y1 = y0 >= (key.grid_size - 1) ? (key.grid_size - 1) : (y0 + 1);

  u = crn_x - x0;
  v = crn_y - y0;

  if (mode == 0) {
    copy_v3_v3(data[0], CCG_grid_elem_no(key, grid, x0, y0));
    copy_v3_v3(data[1], CCG_grid_elem_no(key, grid, x1, y0));
    copy_v3_v3(data[2], CCG_grid_elem_no(key, grid, x1, y1));
    copy_v3_v3(data[3], CCG_grid_elem_no(key, grid, x0, y1));
  }
  else {
    copy_v3_v3(data[0], CCG_grid_elem_co(key, grid, x0, y0));
    copy_v3_v3(data[1], CCG_grid_elem_co(key, grid, x1, y0));
    copy_v3_v3(data[2], CCG_grid_elem_co(key, grid, x1, y1));
    copy_v3_v3(data[3], CCG_grid_elem_co(key, grid, x0, y1));
  }

  interp_bilinear_quad_v3(data, u, v, res);
}

static void get_ccgdm_data(const OffsetIndices<int> lores_polys,
                           DerivedMesh *hidm,
                           const int *index_mp_to_orig,
                           const int lvl,
                           const int face_index,
                           const float u,
                           const float v,
                           float co[3],
                           float n[3])
{
  CCGElem **grid_data;
  CCGKey key;
  float crn_x, crn_y;
  int grid_size, S, face_side;
  int *grid_offset, g_index;

  grid_size = hidm->getGridSize(hidm);
  grid_data = hidm->getGridData(hidm);
  grid_offset = hidm->getGridOffset(hidm);
  hidm->getGridKey(hidm, &key);

  if (lvl == 0) {
    face_side = (grid_size << 1) - 1;

    g_index = grid_offset[face_index];
    S = mdisp_rot_face_to_crn(lores_polys[face_index].size(),
                              face_side,
                              u * (face_side - 1),
                              v * (face_side - 1),
                              &crn_x,
                              &crn_y);
  }
  else {
    /* number of faces per grid side */
    int polys_per_grid_side = (1 << (lvl - 1));
    /* get the original cage face index */
    int cage_face_index = index_mp_to_orig ? index_mp_to_orig[face_index] : face_index;
    /* local offset in total cage face grids
     * `(1 << (2 * lvl))` is number of all faces for one cage face */
    int loc_cage_poly_ofs = face_index % (1 << (2 * lvl));
    /* local offset in the vertex grid itself */
    int cell_index = loc_cage_poly_ofs % (polys_per_grid_side * polys_per_grid_side);
    int cell_side = (grid_size - 1) / polys_per_grid_side;
    /* row and column based on grid side */
    int row = cell_index / polys_per_grid_side;
    int col = cell_index % polys_per_grid_side;

    /* S is the vertex whose grid we are examining */
    S = face_index / (1 << (2 * (lvl - 1))) - grid_offset[cage_face_index];
    /* get offset of grid data for original cage face */
    g_index = grid_offset[cage_face_index];

    crn_y = (row * cell_side) + u * cell_side;
    crn_x = (col * cell_side) + v * cell_side;
  }

  CLAMP(crn_x, 0.0f, grid_size);
  CLAMP(crn_y, 0.0f, grid_size);

  if (n != nullptr) {
    interp_bilinear_grid(key, grid_data[g_index + S], crn_x, crn_y, 0, n);
  }

  if (co != nullptr) {
    interp_bilinear_grid(key, grid_data[g_index + S], crn_x, crn_y, 1, co);
  }
}

/* mode = 0: interpolate normals,
 * mode = 1: interpolate coord */

static void interp_bilinear_mpoly(const Span<float3> vert_positions,
                                  const Span<float3> vert_normals,
                                  const Span<int> corner_verts,
                                  const IndexRange face,
                                  const float u,
                                  const float v,
                                  const int mode,
                                  float res[3])
{
  float data[4][3];

  if (mode == 0) {
    copy_v3_v3(data[0], vert_normals[corner_verts[face[0]]]);
    copy_v3_v3(data[1], vert_normals[corner_verts[face[1]]]);
    copy_v3_v3(data[2], vert_normals[corner_verts[face[2]]]);
    copy_v3_v3(data[3], vert_normals[corner_verts[face[3]]]);
  }
  else {
    copy_v3_v3(data[0], vert_positions[corner_verts[face[0]]]);
    copy_v3_v3(data[1], vert_positions[corner_verts[face[1]]]);
    copy_v3_v3(data[2], vert_positions[corner_verts[face[2]]]);
    copy_v3_v3(data[3], vert_positions[corner_verts[face[3]]]);
  }

  interp_bilinear_quad_v3(data, u, v, res);
}

static void interp_barycentric_corner_tri(const Span<float3> vert_positions,
                                          const Span<float3> vert_normals,
                                          const Span<int> corner_verts,
                                          const int3 &corner_tri,
                                          const float u,
                                          const float v,
                                          const int mode,
                                          float res[3])
{
  float data[3][3];

  if (mode == 0) {
    copy_v3_v3(data[0], vert_normals[corner_verts[corner_tri[0]]]);
    copy_v3_v3(data[1], vert_normals[corner_verts[corner_tri[1]]]);
    copy_v3_v3(data[2], vert_normals[corner_verts[corner_tri[2]]]);
  }
  else {
    copy_v3_v3(data[0], vert_positions[corner_verts[corner_tri[0]]]);
    copy_v3_v3(data[1], vert_positions[corner_verts[corner_tri[1]]]);
    copy_v3_v3(data[2], vert_positions[corner_verts[corner_tri[2]]]);
  }

  interp_barycentric_tri_v3(data, u, v, res);
}

/* **************** Displacement Baker **************** */

static void *init_heights_data(MultiresBakeRender &bake, ImBuf *ibuf)
{
  MHeightBakeData *height_data;
  DerivedMesh *lodm = bake.lores_dm;
  BakeImBufuserData *userdata = static_cast<BakeImBufuserData *>(ibuf->userdata);

  if (userdata->displacement_buffer == nullptr) {
    userdata->displacement_buffer = MEM_calloc_arrayN<float>(IMB_get_pixel_count(ibuf),
                                                             "MultiresBake heights");
  }

  height_data = MEM_callocN<MHeightBakeData>("MultiresBake heightData");

  height_data->heights = userdata->displacement_buffer;

  if (!bake.use_lores_mesh) {
    SubsurfModifierData smd = {{nullptr}};
    int ss_lvl = bake.tot_lvl - bake.lvl;

    CLAMP(ss_lvl, 0, 6);

    if (ss_lvl > 0) {
      smd.levels = smd.renderLevels = ss_lvl;
      smd.uv_smooth = SUBSURF_UV_SMOOTH_PRESERVE_BOUNDARIES;
      smd.quality = 3;

      height_data->ssdm = subsurf_make_derived_from_derived(
          bake.lores_dm, &smd, bake.scene, nullptr, SubsurfFlags(0));
      init_ccgdm_arrays(height_data->ssdm);
    }
  }

  height_data->orig_index_mp_to_orig = static_cast<const int *>(
      lodm->getPolyDataArray(lodm, CD_ORIGINDEX));

  return (void *)height_data;
}

static void free_heights_data(void *bake_data)
{
  MHeightBakeData *height_data = (MHeightBakeData *)bake_data;

  if (height_data->ssdm) {
    height_data->ssdm->release(height_data->ssdm);
  }

  MEM_freeN(height_data);
}

/* MultiresBake callback for heights baking
 * general idea:
 *   - find coord of point with specified UV in hi-res mesh (let's call it p1)
 *   - find coord of point and normal with specified UV in lo-res mesh (or subdivided lo-res
 *     mesh to make texture smoother) let's call this point p0 and n.
 *   - height wound be dot(n, p1-p0) */
static void apply_heights_callback(const Span<float3> vert_positions,
                                   const Span<float3> vert_normals,
                                   const OffsetIndices<int> faces,
                                   const Span<int> corner_verts,
                                   const Span<int3> corner_tris,
                                   const Span<int> tri_faces,
                                   const Span<float2> uv_map,
                                   DerivedMesh *hires_dm,
                                   void *thread_data_v,
                                   void *bake_data,
                                   ImBuf *ibuf,
                                   const int tri_index,
                                   const int lvl,
                                   const float st[2],
                                   float /*tangmat*/[3][3],
                                   const int x,
                                   const int y)
{
  const int3 &tri = corner_tris[tri_index];
  const int face_i = tri_faces[tri_index];
  const IndexRange face = faces[face_i];
  MHeightBakeData *height_data = (MHeightBakeData *)bake_data;
  MultiresBakeThread *thread_data = (MultiresBakeThread *)thread_data_v;
  float uv[2];
  const float *st0, *st1, *st2, *st3;
  int pixel = ibuf->x * y + x;
  float vec[3], p0[3], p1[3], n[3], len;

  /* ideally we would work on triangles only, however, we rely on quads to get orthogonal
   * coordinates for use in grid space (triangle barycentric is not orthogonal) */
  if (face.size() == 4) {
    st0 = uv_map[face[0]];
    st1 = uv_map[face[1]];
    st2 = uv_map[face[2]];
    st3 = uv_map[face[3]];
    resolve_quad_uv_v2(uv, st, st0, st1, st2, st3);
  }
  else {
    st0 = uv_map[tri[0]];
    st1 = uv_map[tri[1]];
    st2 = uv_map[tri[2]];
    resolve_tri_uv_v2(uv, st, st0, st1, st2);
  }

  clamp_v2(uv, 0.0f, 1.0f);

  get_ccgdm_data(
      faces, hires_dm, height_data->orig_index_mp_to_orig, lvl, face_i, uv[0], uv[1], p1, nullptr);

  if (height_data->ssdm) {
    get_ccgdm_data(faces,
                   height_data->ssdm,
                   height_data->orig_index_mp_to_orig,
                   0,
                   face_i,
                   uv[0],
                   uv[1],
                   p0,
                   n);
  }
  else {
    if (face.size() == 4) {
      interp_bilinear_mpoly(vert_positions, vert_normals, corner_verts, face, uv[0], uv[1], 1, p0);
      interp_bilinear_mpoly(vert_positions, vert_normals, corner_verts, face, uv[0], uv[1], 0, n);
    }
    else {
      interp_barycentric_corner_tri(
          vert_positions, vert_normals, corner_verts, tri, uv[0], uv[1], 1, p0);
      interp_barycentric_corner_tri(
          vert_positions, vert_normals, corner_verts, tri, uv[0], uv[1], 0, n);
    }
  }

  sub_v3_v3v3(vec, p1, p0);
  len = dot_v3v3(n, vec);

  height_data->heights[pixel] = len;

  thread_data->height_min = min_ff(thread_data->height_min, len);
  thread_data->height_max = max_ff(thread_data->height_max, len);

  if (ibuf->float_buffer.data) {
    float *rrgbf = ibuf->float_buffer.data + pixel * 4;
    rrgbf[0] = rrgbf[1] = rrgbf[2] = len;
    rrgbf[3] = 1.0f;
  }
  else {
    uchar *rrgb = ibuf->byte_buffer.data + pixel * 4;
    rrgb[0] = rrgb[1] = rrgb[2] = unit_float_to_uchar_clamp(len);
    rrgb[3] = 255;
  }
}

/* **************** Normal Maps Baker **************** */

static void *init_normal_data(MultiresBakeRender &bake, ImBuf * /*ibuf*/)
{
  MNormalBakeData *normal_data;
  DerivedMesh *lodm = bake.lores_dm;

  normal_data = MEM_callocN<MNormalBakeData>("MultiresBake normalData");

  normal_data->orig_index_mp_to_orig = static_cast<const int *>(
      lodm->getPolyDataArray(lodm, CD_ORIGINDEX));

  return (void *)normal_data;
}

static void free_normal_data(void *bake_data)
{
  MNormalBakeData *normal_data = (MNormalBakeData *)bake_data;

  MEM_freeN(normal_data);
}

/**
 * MultiresBake callback for normals' baking.
 *
 * General idea:
 * - Find coord and normal of point with specified UV in hi-res mesh.
 * - Multiply it by tangmat.
 * - Vector in color space would be `norm(vec) / 2 + (0.5, 0.5, 0.5)`.
 */
static void apply_tangmat_callback(const Span<float3> /*vert_positions*/,
                                   const Span<float3> /*vert_normals*/,
                                   const OffsetIndices<int> faces,
                                   const Span<int> /*corner_verts*/,
                                   const Span<int3> corner_tris,
                                   const Span<int> tri_faces,
                                   const Span<float2> uv_map,
                                   DerivedMesh *hires_dm,
                                   void * /*thread_data*/,
                                   void *bake_data,
                                   ImBuf *ibuf,
                                   const int tri_index,
                                   const int lvl,
                                   const float st[2],
                                   float tangmat[3][3],
                                   const int x,
                                   const int y)
{
  const int3 &tri = corner_tris[tri_index];
  const int face_i = tri_faces[tri_index];
  const IndexRange face = faces[face_i];
  MNormalBakeData *normal_data = (MNormalBakeData *)bake_data;
  float uv[2];
  const float *st0, *st1, *st2, *st3;
  int pixel = ibuf->x * y + x;
  float n[3], vec[3], tmp[3] = {0.5, 0.5, 0.5};

  /* ideally we would work on triangles only, however, we rely on quads to get orthogonal
   * coordinates for use in grid space (triangle barycentric is not orthogonal) */
  if (face.size() == 4) {
    st0 = uv_map[face[0]];
    st1 = uv_map[face[1]];
    st2 = uv_map[face[2]];
    st3 = uv_map[face[3]];
    resolve_quad_uv_v2(uv, st, st0, st1, st2, st3);
  }
  else {
    st0 = uv_map[tri[0]];
    st1 = uv_map[tri[1]];
    st2 = uv_map[tri[2]];
    resolve_tri_uv_v2(uv, st, st0, st1, st2);
  }

  clamp_v2(uv, 0.0f, 1.0f);

  get_ccgdm_data(
      faces, hires_dm, normal_data->orig_index_mp_to_orig, lvl, face_i, uv[0], uv[1], nullptr, n);

  mul_v3_m3v3(vec, tangmat, n);
  normalize_v3_length(vec, 0.5);
  add_v3_v3(vec, tmp);

  if (ibuf->float_buffer.data) {
    float *rrgbf = ibuf->float_buffer.data + pixel * 4;
    rrgbf[0] = vec[0];
    rrgbf[1] = vec[1];
    rrgbf[2] = vec[2];
    rrgbf[3] = 1.0f;
  }
  else {
    uchar *rrgb = ibuf->byte_buffer.data + pixel * 4;
    rgb_float_to_uchar(rrgb, vec);
    rrgb[3] = 255;
  }
}

/* ******$***************** Post processing ************************* */

static void bake_ibuf_filter(ImBuf *ibuf,
                             char *mask,
                             const int margin,
                             const char margin_type,
                             DerivedMesh *dm,
                             const float uv_offset[2])
{
  /* must check before filtering */
  const bool is_new_alpha = (ibuf->planes != R_IMF_PLANES_RGBA) && BKE_imbuf_alpha_test(ibuf);

  if (margin) {
    switch (margin_type) {
      case R_BAKE_ADJACENT_FACES:
        RE_generate_texturemargin_adjacentfaces_dm(ibuf, mask, margin, dm, uv_offset);
        break;
      default:
        /* fall through */
      case R_BAKE_EXTEND:
        IMB_filter_extend(ibuf, mask, margin);
        break;
    }
  }

  /* if the bake results in new alpha then change the image setting */
  if (is_new_alpha) {
    ibuf->planes = R_IMF_PLANES_RGBA;
  }
  else {
    if (margin && ibuf->planes != R_IMF_PLANES_RGBA) {
      /* clear alpha added by filtering */
      IMB_rectfill_alpha(ibuf, 1.0f);
    }
  }
}

static void bake_ibuf_normalize_displacement(ImBuf *ibuf,
                                             const float *displacement,
                                             const char *mask,
                                             float displacement_min,
                                             float displacement_max)
{
  const float *current_displacement = displacement;
  const char *current_mask = mask;
  float max_distance;

  max_distance = max_ff(fabsf(displacement_min), fabsf(displacement_max));

  const size_t ibuf_pixel_count = IMB_get_pixel_count(ibuf);
  for (size_t i = 0; i < ibuf_pixel_count; i++) {
    if (*current_mask == FILTER_MASK_USED) {
      float normalized_displacement;

      if (max_distance > 1e-5f) {
        normalized_displacement = (*current_displacement + max_distance) / (max_distance * 2);
      }
      else {
        normalized_displacement = 0.5f;
      }

      if (ibuf->float_buffer.data) {
        /* currently baking happens to RGBA only */
        float *fp = ibuf->float_buffer.data + i * 4;
        fp[0] = fp[1] = fp[2] = normalized_displacement;
        fp[3] = 1.0f;
      }

      if (ibuf->byte_buffer.data) {
        uchar *cp = ibuf->byte_buffer.data + 4 * i;
        cp[0] = cp[1] = cp[2] = unit_float_to_uchar_clamp(normalized_displacement);
        cp[3] = 255;
      }
    }

    current_displacement++;
    current_mask++;
  }
}

/* **************** Common functions public API relates on **************** */

static void count_images(MultiresBakeRender &bake)
{
  bake.images.clear();

  for (Image *image : bake.ob_image) {
    if (image) {
      bake.images.add(image);
    }
  }
}

static void bake_images(MultiresBakeRender &bake, MultiresBakeResult &result)
{
  /* construct bake result */
  result.height_min = FLT_MAX;
  result.height_max = -FLT_MAX;

  for (Image *image : bake.images) {
    LISTBASE_FOREACH (ImageTile *, tile, &image->tiles) {
      ImageUser iuser;
      BKE_imageuser_default(&iuser);
      iuser.tile = tile->tile_number;

      ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);

      if (ibuf->x > 0 && ibuf->y > 0) {
        BakeImBufuserData *userdata = MEM_callocN<BakeImBufuserData>("MultiresBake userdata");
        userdata->mask_buffer = MEM_calloc_arrayN<char>(size_t(ibuf->y) * size_t(ibuf->x),
                                                        "MultiresBake imbuf mask");
        ibuf->userdata = userdata;

        switch (bake.mode) {
          case RE_BAKE_NORMALS:
            do_multires_bake(bake,
                             image,
                             tile,
                             ibuf,
                             true,
                             apply_tangmat_callback,
                             init_normal_data,
                             free_normal_data,
                             result);
            break;
          case RE_BAKE_DISPLACEMENT:
            do_multires_bake(bake,
                             image,
                             tile,
                             ibuf,
                             false,
                             apply_heights_callback,
                             init_heights_data,
                             free_heights_data,
                             result);
            break;
        }
      }

      BKE_image_release_ibuf(image, ibuf, nullptr);
    }

    image->id.tag |= ID_TAG_DOIT;
  }
}

static void finish_images(MultiresBakeRender &bake, MultiresBakeResult &result)
{
  const bool use_displacement_buffer = bake.mode == RE_BAKE_DISPLACEMENT;

  for (Image *image : bake.images) {
    LISTBASE_FOREACH (ImageTile *, tile, &image->tiles) {
      ImageUser iuser;
      BKE_imageuser_default(&iuser);
      iuser.tile = tile->tile_number;

      ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
      BakeImBufuserData *userdata = (BakeImBufuserData *)ibuf->userdata;

      if (ibuf->x <= 0 || ibuf->y <= 0) {
        continue;
      }

      if (use_displacement_buffer) {
        bake_ibuf_normalize_displacement(ibuf,
                                         userdata->displacement_buffer,
                                         userdata->mask_buffer,
                                         result.height_min,
                                         result.height_max);
      }

      float uv_offset[2];
      BKE_image_get_tile_uv(image, tile->tile_number, uv_offset);

      bake_ibuf_filter(ibuf,
                       userdata->mask_buffer,
                       bake.bake_margin,
                       bake.bake_margin_type,
                       bake.lores_dm,
                       uv_offset);

      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(image, ibuf);

      if (ibuf->float_buffer.data) {
        ibuf->userflags |= IB_RECT_INVALID;
      }

      if (ibuf->userdata) {
        if (userdata->displacement_buffer) {
          MEM_freeN(userdata->displacement_buffer);
        }

        MEM_freeN(userdata->mask_buffer);
        MEM_freeN(userdata);
        ibuf->userdata = nullptr;
      }

      BKE_image_release_ibuf(image, ibuf, nullptr);
      DEG_id_tag_update(&image->id, 0);
    }
  }
}

}  // namespace
}  // namespace blender::render

void RE_multires_bake_images(MultiresBakeRender *bkr)
{
  blender::render::MultiresBakeResult result;

  blender::render::count_images(*bkr);
  blender::render::bake_images(*bkr, result);
  blender::render::finish_images(*bkr, result);
}
