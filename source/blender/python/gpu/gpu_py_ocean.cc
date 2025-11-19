// SPDX-License-Identifier: GPL-2.0-or-later
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "BLI_utildefines.h"

#include "DNA_defaults.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "BLI_task.h"

#include "../depsgraph/DEG_depsgraph_query.hh"
#include "../draw/intern/draw_cache_extract.hh"
#include "../draw/intern/draw_cache.hh"
#include "../python/intern/bpy_rna.hh"
#include "../gpu/intern/gpu_shader_create_info.hh"

#include "../blenkernel/intern/mesh_gpu_cache.hh"
#include "GPU_batch.hh"
#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"

#include "../blenkernel/intern/ocean_intern.h"

#include "gpu_py_storagebuffer.hh"

#include "../windowmanager/WM_api.hh"

using blender::gpu::StorageBuf;
using namespace blender::bke;

/* Cache SSBO per Ocean* to avoid create/free every frame.
 * Stores both the native StorageBuf* and a persistent Python wrapper so returning
 * a cached buffer to Python does not create ownership/ double-free issues.
 */
struct SSBOCacheEntry {
  PyObject *py_ssbo; /* owns a reference to GPUStorageBuf Python wrapper */
  size_t capacity;
};

/* Internal SSBO cache (raw StorageBuf*). Used to avoid repeated GPU allocations
 * for transient pipeline buffers (pong, temp, transposed, rotated, etc.). */
enum InternalSSBORole {
  SSBO_ROLE_PONG = 1,
  SSBO_ROLE_PONG2,
  SSBO_ROLE_TRANSPOSED,
  SSBO_ROLE_HTILDA_EXPANDED,
  SSBO_ROLE_ROTATED,
  SSBO_ROLE_FFT_IN_X,          /* dedicated role for fft_in_x */
  SSBO_ROLE_FFT_IN_Z,         /* dedicated role for fft_in_z */
  SSBO_ROLE_SPATIAL_COMPLEX_X, /* distinct roles to avoid aliasing */
  SSBO_ROLE_SPATIAL_COMPLEX_Z,
  SSBO_ROLE_SPATIAL_COMPLEX,
  SSBO_ROLE_DST,   /* disp vec4 output */
  SSBO_ROLE_TEMP,  /* general purpose temp */
  SSBO_ROLE_OMEGA, /* optional: cache omega per ocean */
  SSBO_ROLE_H0_COMPACT,
  SSBO_ROLE_H0M_COMPACT,
  /* add more roles as needed */
};

static std::unordered_map<Ocean *, SSBOCacheEntry> g_ocean_ssbo_cache;
static std::unordered_map<Ocean *, SSBOCacheEntry> g_ocean_base_ssbo_cache;
static std::unordered_map<Ocean *, SSBOCacheEntry> g_ocean_out_ssbo_cache;
static std::unordered_map<Ocean *, std::pair<float *, size_t>> g_ocean_padded_cpu_cache;
static std::unordered_map<Ocean *, PyObject *> g_ocean_object_cache;
static std::unordered_map<Ocean *, std::tuple<float, float, int>> g_ocean_base_state;
static std::unordered_map<Ocean *, const void *> g_ocean_h0_last_ptr;
static std::unordered_map<Ocean *, const void *> g_ocean_h0m_last_ptr;

/* -------------------------------------------------------------------- */
/* Geometry generation helpers (MOD_ocean copy temp)                    */
/* -------------------------------------------------------------------- */

struct GenerateOceanGeometryData {
  blender::MutableSpan<blender::float3> vert_positions;
  blender::MutableSpan<int> face_offsets;
  blender::MutableSpan<int> corner_verts;
  float (*uv_map)[2];

  int res_x, res_y;
  int rx, ry;
  float ox, oy;
  float sx, sy;
  float ix, iy;
};

static void gpu_generate_ocean_geometry_verts(void *__restrict userdata,
                                              const int y,
                                              const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanGeometryData *gogd = static_cast<GenerateOceanGeometryData *>(userdata);
  for (int x = 0; x <= gogd->res_x; x++) {
    const int i = y * (gogd->res_x + 1) + x;
    float *co = gogd->vert_positions[i];
    co[0] = gogd->ox + (x * gogd->sx);
    co[1] = gogd->oy + (y * gogd->sy);
    co[2] = 0.0f;
  }
}

static void gpu_generate_ocean_geometry_faces(void *__restrict userdata,
                                              const int y,
                                              const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanGeometryData *gogd = static_cast<GenerateOceanGeometryData *>(userdata);
  for (int x = 0; x < gogd->res_x; x++) {
    const int fi = y * gogd->res_x + x;
    const int vi = y * (gogd->res_x + 1) + x;

    gogd->corner_verts[fi * 4 + 0] = vi;
    gogd->corner_verts[fi * 4 + 1] = vi + 1;
    gogd->corner_verts[fi * 4 + 2] = vi + 1 + gogd->res_x + 1;
    gogd->corner_verts[fi * 4 + 3] = vi + gogd->res_x + 1;

    gogd->face_offsets[fi] = fi * 4;
  }
}

static void gpu_generate_ocean_geometry_uvs(void *__restrict userdata,
                                        const int y,
                                        const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanGeometryData *gogd = static_cast<GenerateOceanGeometryData *>(userdata);
  int x;

  for (x = 0; x < gogd->res_x; x++) {
    const int i = y * gogd->res_x + x;
    float(*luv)[2] = &gogd->uv_map[i * 4];

    (*luv)[0] = x * gogd->ix;
    (*luv)[1] = y * gogd->iy;
    luv++;

    (*luv)[0] = (x + 1) * gogd->ix;
    (*luv)[1] = y * gogd->iy;
    luv++;

    (*luv)[0] = (x + 1) * gogd->ix;
    (*luv)[1] = (y + 1) * gogd->iy;
    luv++;

    (*luv)[0] = x * gogd->ix;
    (*luv)[1] = (y + 1) * gogd->iy;
    luv++;
  }
}

static const int64_t GPU_OCEAN_MAX_VERTS = 50000000; /* sécurité: 50M vertices max */

static Mesh *gpu_generate_ocean_geometry_nomain(OceanModifierData *omd, const int resolution)
{
  if (resolution <= 0) {
    fprintf(stderr, "gpu_generate_ocean_geometry_nomain: invalid resolution %d\n", resolution);
    return nullptr;
  }

  GenerateOceanGeometryData gogd;

  const bool use_threading = resolution > 4;

  const int64_t rx64 = int64_t(resolution) * int64_t(resolution);
  const int64_t ry64 = rx64;
  const int64_t res_x64 = rx64 * int64_t(omd->repeat_x);
  const int64_t res_y64 = ry64 * int64_t(omd->repeat_y);

  if (res_x64 <= 0 || res_y64 <= 0) {
    fprintf(stderr,
            "gpu_generate_ocean_geometry_nomain: bad grid size res_x=%" PRId64 " res_y=%" PRId64
            "\n",
            res_x64,
            res_y64);
    return nullptr;
  }

  const int64_t verts_num64 = (res_x64 + 1) * (res_y64 + 1);
  const int64_t faces_num64 = res_x64 * res_y64;

  if (verts_num64 > GPU_OCEAN_MAX_VERTS) {
    fprintf(stderr,
            "gpu_generate_ocean_geometry_nomain: verts_num %" PRId64 " exceeds limit %" PRId64
            "\n",
            verts_num64,
            (int64_t)GPU_OCEAN_MAX_VERTS);
    return nullptr;
  }

  if (verts_num64 > INT_MAX || faces_num64 > INT_MAX) {
    fprintf(stderr, "gpu_generate_ocean_geometry_nomain: required counts overflow int\n");
    return nullptr;
  }

  const int res_x = int(res_x64);
  const int res_y = int(res_y64);
  const int verts_num = int(verts_num64);
  const int faces_num = int(faces_num64);

  gogd.rx = resolution * resolution;
  gogd.ry = resolution * resolution;
  gogd.res_x = res_x;
  gogd.res_y = res_y;

  gogd.sx = omd->size * omd->spatial_size;
  gogd.sy = omd->size * omd->spatial_size;
  gogd.ox = -gogd.sx / 2.0f;
  gogd.oy = -gogd.sy / 2.0f;

  if (gogd.rx == 0 || gogd.ry == 0) {
    fprintf(
        stderr, "gpu_generate_ocean_geometry_nomain: rx/ry zero (resolution=%d)\n", resolution);
    return nullptr;
  }
  gogd.sx /= gogd.rx;
  gogd.sy /= gogd.ry;

  Mesh *result = BKE_mesh_new_nomain(verts_num, 0, faces_num, faces_num * 4);
  if (!result) {
    fprintf(stderr,
            "gpu_generate_ocean_geometry_nomain: BKE_mesh_new_nomain failed (verts=%d faces=%d)\n",
            verts_num,
            faces_num);
    return nullptr;
  }

  gogd.vert_positions = result->vert_positions_for_write();
  gogd.face_offsets = result->face_offsets_for_write();
  gogd.corner_verts = result->corner_verts_for_write();

  if (gogd.vert_positions.data() == nullptr || gogd.face_offsets.data() == nullptr ||
      gogd.corner_verts.data() == nullptr)
  {
    fprintf(stderr,
            "gpu_generate_ocean_geometry_nomain: runtime buffers not allocated (vert=%p "
            "offsets=%p corner=%p)\n",
            (void *)gogd.vert_positions.data(),
            (void *)gogd.face_offsets.data(),
            (void *)gogd.corner_verts.data());
    BKE_id_free(nullptr, &result->id);
    return nullptr;
  }

  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.use_threading = use_threading;

  BLI_task_parallel_range(0, gogd.res_y + 1, &gogd, gpu_generate_ocean_geometry_verts, &settings);
  BLI_task_parallel_range(0, gogd.res_y, &gogd, gpu_generate_ocean_geometry_faces, &settings);

  blender::bke::mesh_calc_edges(*result, false, false);

  if (CustomData_number_of_layers(&result->corner_data, CD_PROP_FLOAT2) < MAX_MTFACE) {
    gogd.uv_map = static_cast<float(*)[2]>(CustomData_add_layer_named(
        &result->corner_data, CD_PROP_FLOAT2, CD_SET_DEFAULT, faces_num * 4, "UVMap"));

    if (gogd.uv_map) {
      gogd.ix = 1.0f / gogd.rx;
      gogd.iy = 1.0f / gogd.ry;
      BLI_task_parallel_range(0, gogd.res_y, &gogd, gpu_generate_ocean_geometry_uvs, &settings);
    }
  }

  return result;
}

/* -------------------------------------------------------------------- */
/* Geometry generation helpers end                                      */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Cache resources helpers                                              */
/* -------------------------------------------------------------------- */

/* Capsule name for Ocean* */
static const char *PY_OCEAN_PTR_CAPSULE = "bpygpu.ocean_ptr";

/* Get or create a cached SSBO entry sized at least \a bytes for ocean \a o.
 * name is used for GPU_storagebuf_create_ex naming.
 * Returns pointer to map entry on success, nullptr on allocation error.
 * On success the entry->ssbo and entry->py_ssbo are valid.
 */
/* Helper: get native StorageBuf* from cache entry (or nullptr). */
static StorageBuf *pygpu_ocean_entry_get_ssbo(const SSBOCacheEntry *entry)
{
  if (entry == nullptr || entry->py_ssbo == nullptr) {
    return nullptr;
  }
  BPyGPUStorageBuf *bpy = reinterpret_cast<BPyGPUStorageBuf *>(entry->py_ssbo);
  return bpy ? bpy->ssbo : nullptr;
}

/* Generic helper to evict existing cache entry safely (DECREF wrapper only). */
static void pygpu_ocean_evict_cache_entry(std::unordered_map<Ocean *, SSBOCacheEntry> &cache,
                                          std::unordered_map<Ocean *, SSBOCacheEntry>::iterator it)
{
  if (it == cache.end()) {
    return;
  }
  SSBOCacheEntry &entry = it->second;
  if (entry.py_ssbo) {
    /* wrapper will free the native buffer in its dealloc */
    Py_DECREF(entry.py_ssbo);
    entry.py_ssbo = nullptr;
  }
  /* we intentionally do not free any raw StorageBuf* here (wrapper owns it) */
  cache.erase(it);
}

/* Get or create cached SSBO entry (disp cache). */
static SSBOCacheEntry *pygpu_ocean_get_or_create_cached_ssbo_entry(Ocean *o,
                                                                   size_t bytes,
                                                                   const char *name)
{
  auto it = g_ocean_ssbo_cache.find(o);
  if (it != g_ocean_ssbo_cache.end()) {
    SSBOCacheEntry &entry = it->second;
    StorageBuf *ssbo = pygpu_ocean_entry_get_ssbo(&entry);
    if (entry.capacity >= bytes && ssbo) {
      return &entry;
    }
    /* evict old entry safely */
    pygpu_ocean_evict_cache_entry(g_ocean_ssbo_cache, it);
    it = g_ocean_ssbo_cache.end();
  }

  GPUUsageType usage = GPU_USAGE_STATIC;
  if (name && (strstr(name, "disp") != nullptr || strstr(name, "out") != nullptr ||
               strstr(name, "basepos") != nullptr))
  {
    usage = GPU_USAGE_DYNAMIC;
  }

  StorageBuf *ssbo = GPU_storagebuf_create_ex(bytes, nullptr, usage, name);
  if (!ssbo) {
    return nullptr;
  }

  PyObject *py_ssbo = BPyGPUStorageBuf_CreatePyObject(reinterpret_cast<StorageBuf *>(ssbo));
  if (!py_ssbo) {
    GPU_storagebuf_free(ssbo);
    return nullptr;
  }

  SSBOCacheEntry new_entry;
  new_entry.py_ssbo = py_ssbo; /* owns new ref */
  new_entry.capacity = bytes;

  auto insert_res = g_ocean_ssbo_cache.emplace(o, std::move(new_entry));
  if (!insert_res.second) {
    Py_DECREF(py_ssbo);
    GPU_storagebuf_free(ssbo);
    return nullptr;
  }

  return &insert_res.first->second;
}

/* Get or create cached basepos SSBO entry. */
static SSBOCacheEntry *pygpu_ocean_get_or_create_base_ssbo_entry(Ocean *o,
                                                                 size_t bytes,
                                                                 const char *name)
{
  auto it = g_ocean_base_ssbo_cache.find(o);
  if (it != g_ocean_base_ssbo_cache.end()) {
    SSBOCacheEntry &entry = it->second;
    StorageBuf *ssbo = pygpu_ocean_entry_get_ssbo(&entry);
    if (entry.capacity >= bytes && ssbo) {
      return &entry;
    }
    pygpu_ocean_evict_cache_entry(g_ocean_base_ssbo_cache, it);
  }

  StorageBuf *ssbo = GPU_storagebuf_create_ex(bytes, nullptr, GPU_USAGE_STATIC, name);
  if (!ssbo) {
    return nullptr;
  }

  PyObject *py_ssbo = BPyGPUStorageBuf_CreatePyObject(reinterpret_cast<StorageBuf *>(ssbo));
  if (!py_ssbo) {
    GPU_storagebuf_free(ssbo);
    return nullptr;
  }

  SSBOCacheEntry entry;
  entry.py_ssbo = py_ssbo;
  entry.capacity = bytes;

  auto insert_res = g_ocean_base_ssbo_cache.emplace(o, std::move(entry));
  if (!insert_res.second) {
    Py_DECREF(py_ssbo);
    GPU_storagebuf_free(ssbo);
    return nullptr;
  }
  return &insert_res.first->second;
}

/* Get or create cached out SSBO entry. */
static SSBOCacheEntry *pygpu_ocean_get_or_create_out_ssbo_entry(Ocean *o,
                                                                size_t bytes,
                                                                const char *name)
{
  auto it = g_ocean_out_ssbo_cache.find(o);
  if (it != g_ocean_out_ssbo_cache.end()) {
    SSBOCacheEntry &entry = it->second;
    StorageBuf *ssbo = pygpu_ocean_entry_get_ssbo(&entry);
    if (entry.capacity >= bytes && ssbo) {
      return &entry;
    }
    pygpu_ocean_evict_cache_entry(g_ocean_out_ssbo_cache, it);
  }

  StorageBuf *ssbo = GPU_storagebuf_create_ex(bytes, nullptr, GPU_USAGE_STATIC, name);
  if (!ssbo) {
    return nullptr;
  }

  PyObject *py_ssbo = BPyGPUStorageBuf_CreatePyObject(reinterpret_cast<StorageBuf *>(ssbo));
  if (!py_ssbo) {
    GPU_storagebuf_free(ssbo);
    return nullptr;
  }

  SSBOCacheEntry entry;
  entry.py_ssbo = py_ssbo;
  entry.capacity = bytes;

  auto insert_res = g_ocean_out_ssbo_cache.emplace(o, std::move(entry));
  if (!insert_res.second) {
    Py_DECREF(py_ssbo);
    GPU_storagebuf_free(ssbo);
    return nullptr;
  }
  return &insert_res.first->second;
}

/* Helper: obtain Ocean* from Python arg (accepts int or capsule). */
static bool get_ocean_ptr_from_pyobj(PyObject *obj, Ocean **r_ocean)
{
  if (!r_ocean) {
    return false;
  }
  *r_ocean = nullptr;

  if (PyCapsule_CheckExact(obj)) {
    void *p = PyCapsule_GetPointer(obj, PY_OCEAN_PTR_CAPSULE);
    if (p) {
      *r_ocean = reinterpret_cast<Ocean *>(p);
      return true;
    }
    if (PyErr_Occurred()) {
      PyErr_Clear();
    }
    p = PyCapsule_GetPointer(obj, nullptr);
    if (p) {
      *r_ocean = reinterpret_cast<Ocean *>(p);
      return true;
    }
    PyErr_SetString(PyExc_TypeError, "Capsule does not contain an Ocean pointer");
    return false;
  }

  if (PyLong_Check(obj)) {
    void *p = PyLong_AsVoidPtr(obj);
    if (p == nullptr && PyErr_Occurred()) {
      return false;
    }
    *r_ocean = reinterpret_cast<Ocean *>(p);
    return true;
  }

  PyErr_SetString(PyExc_TypeError, "ocean pointer must be an integer or capsule");
  return false;
}

/* Free generated cached object for an Ocean (Python wrapper) */
static PyObject *pygpu_ocean_free_generated_mesh(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_ocean_obj)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  if (!o) {
    Py_RETURN_NONE;
  }

  auto it = g_ocean_object_cache.find(o);
  if (it != g_ocean_object_cache.end()) {
    PyObject *weak = it->second;
    if (weak) {
      /* stored object is a weakref: DECREF the weakref object itself (does not DECREF the target)
       */
      Py_DECREF(weak);
    }
    g_ocean_object_cache.erase(it);
  }

  Py_RETURN_NONE;
}

static StorageBuf *pygpu_ocean_get_or_create_internal_ssbo(Ocean *o,
                                                           int role,
                                                           size_t bytes,
                                                           const char *name)
{
  if (!o) {
    return nullptr;
  }
  // Clé lisible: on mappe le role -> string stable
  std::string key;
  switch (role) {
    case SSBO_ROLE_PONG:
      key = "pong";
      break;
    case SSBO_ROLE_PONG2:
      key = "pong2";
      break;
    case SSBO_ROLE_TRANSPOSED:
      key = "transposed";
      break;
    case SSBO_ROLE_HTILDA_EXPANDED:
      key = "htilda_expanded";
      break;
    case SSBO_ROLE_ROTATED:
      key = "rotated";
      break;
    case SSBO_ROLE_FFT_IN_X:
      key = "fft_in_x";
      break;
    case SSBO_ROLE_FFT_IN_Z:
      key = "fft_in_z";
      break;
    case SSBO_ROLE_SPATIAL_COMPLEX_X:
      key = "spatial_complex_x";
      break;
    case SSBO_ROLE_SPATIAL_COMPLEX_Z:
      key = "spatial_complex_z";
      break;
    case SSBO_ROLE_SPATIAL_COMPLEX:
      key = "spatial_complex";
      break;
    case SSBO_ROLE_DST:
      key = "dst";
      break;
    case SSBO_ROLE_TEMP:
      key = "temp";
      break;
    case SSBO_ROLE_OMEGA:
      key = "omega";
      break;
    case SSBO_ROLE_H0_COMPACT:
      key = "h0_compact";
      break;
    case SSBO_ROLE_H0M_COMPACT:
      key = "h0m_compact";
      break;
    default:
      key = name ? name : "unknown";
      break;
  }

  auto &mgr = blender::bke::MeshGPUCacheManager::get();
  return mgr.ocean_internal_ssbo_ensure(o, key, bytes);
}

/* Free all internal SSBOs for an Ocean (call on ocean free or module free) */
static void pygpu_ocean_free_internal_ssbos_for_ocean(Ocean *o)
{
  if (o) {
    MeshGPUCacheManager::get().free_ocean_cache(o);
  }
}

/* Free everything on module unload */
static void pygpu_ocean_free_all_internal_ssbos()
{
  MeshGPUCacheManager::get().free_all_ocean_caches();
}

/* -------------------------------------------------------------------- */
/* Cache resources helpers end                                          */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Python API helpers                                                  */
/* -------------------------------------------------------------------- */

/* Helper: get (or allocate) padded CPU buffer of at least bytes_needed. */
static float *pygpu_ocean_get_or_alloc_padded_cpu(Ocean *o, size_t bytes_needed)
{
  auto it = g_ocean_padded_cpu_cache.find(o);
  if (it != g_ocean_padded_cpu_cache.end()) {
    float *buf = it->second.first;
    size_t cap = it->second.second;
    if (cap >= bytes_needed) {
      return buf;
    }
    /* resize existing allocation */
    MEM_freeN(buf);
    g_ocean_padded_cpu_cache.erase(it);
  }
  float *buf = static_cast<float *>(MEM_mallocN(bytes_needed, "ocean_disp_xyz_padded_cached"));
  if (!buf) {
    return nullptr;
  }
  g_ocean_padded_cpu_cache.emplace(o, std::make_pair(buf, bytes_needed));
  return buf;
}

/* Context stored on PyCapsule to keep Python-visible defaults for simulate calls. */
struct OceanCapsuleContext {
  double time;
  double scale;
  double chop;
  double size;
  int spatial_size;
  double wave_scale;
  double smallest_wave;
  double wind_velocity;
  int resolution;
  char spectrum[64]; /* optional user-visible spectrum name (null-terminated) */
};

static void pygpu_ocean_capsule_destructor(PyObject *capsule)
{
  void *ctx = PyCapsule_GetContext(capsule);
  if (ctx) {
    MEM_freeN(ctx);
    PyCapsule_SetContext(capsule, nullptr);
  }
}

/* Create default Ocean (based on OceanModifierData defaults).
 * Accepts optional keyword arguments to override some modifier defaults:
 *   resolution:int=omd.resolution
 *   size:float=omd.size
 *   spatial_size:int=omd.spatial_size
 *   wave_scale:float=omd.wave_scale
 *   smallest_wave:float=omd.smallest_wave
 *   chop_amount:float=omd.chop_amount
 *   wind_velocity:float=omd.wind_velocity
 *   spectrum:str=omd.spectrum (one of "Phillips", "JONSWAP", "Texel-Marsen-Arsloe",
 * "Pierson-Moskowitz")
 */
static PyObject *pygpu_ocean_create_default_ocean(PyObject * /*self*/,
                                                  PyObject *args,
                                                  PyObject *kwds)
{
  static char *kwlist[] = {(char *)"resolution",
                           (char *)"size",
                           (char *)"spatial_size",
                           (char *)"wave_scale",
                           (char *)"smallest_wave",
                           (char *)"chop_amount",
                           (char *)"wind_velocity",
                           (char *)"spectrum",
                           nullptr};

  OceanModifierData omd;
  MEMCPY_STRUCT_AFTER(&omd, DNA_struct_default_get(OceanModifierData), modifier);

  /* initialize defaults from DNA defaults */
  int resolution = omd.resolution;
  double size_d = omd.size;
  int spatial_size = omd.spatial_size;
  double wave_scale = omd.wave_scale;
  double smallest_wave = omd.smallest_wave;
  double chop_amount = omd.chop_amount;
  double wind_velocity = omd.wind_velocity;
  const char *spectrum_c = nullptr;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "|ididddds",
                                   kwlist,
                                   &resolution,
                                   &size_d,
                                   &spatial_size,
                                   &wave_scale,
                                   &smallest_wave,
                                   &chop_amount,
                                   &wind_velocity,
                                   &spectrum_c))
  {
    return nullptr;
  }

  /* apply parsed/validated values into modifier data */
  omd.resolution = resolution;
  omd.size = float(size_d);
  omd.spatial_size = spatial_size;
  omd.wave_scale = float(wave_scale);
  omd.smallest_wave = float(smallest_wave);
  omd.chop_amount = float(chop_amount);
  omd.wind_velocity = float(wind_velocity);

  /* If user provided a spectrum name, map it to MOD_OCEAN_SPECTRUM_* and set on omd BEFORE init.
   * This avoids reinitializing after creation and preserves seed/flags. */
  if (spectrum_c) {
    std::string s = spectrum_c;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });

    int mapped = omd.spectrum; /* default from DNA */

    if (s.find("jonswap") != std::string::npos) {
      mapped = MOD_OCEAN_SPECTRUM_JONSWAP;
    }
    else if (s.find("texel") != std::string::npos || s.find("marsen") != std::string::npos ||
             s.find("arsloe") != std::string::npos)
    {
      mapped = MOD_OCEAN_SPECTRUM_TEXEL_MARSEN_ARSLOE;
    }
    else if (s.find("pierson") != std::string::npos || s.find("mosk") != std::string::npos) {
      mapped = MOD_OCEAN_SPECTRUM_PIERSON_MOSKOWITZ;
    }
    /* else: unknown string -> keep DNA default (Phillips-style handled by default case) */

    omd.spectrum = mapped;
  }

  omd.ocean = BKE_ocean_add();
  if (!omd.ocean) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_add failed");
    return nullptr;
  }

  if (!BKE_ocean_init_from_modifier(omd.ocean, &omd, resolution)) {
    BKE_ocean_free(omd.ocean);
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_init_from_modifier failed");
    return nullptr;
  }

  PyObject *capsule = PyCapsule_New(
      omd.ocean, PY_OCEAN_PTR_CAPSULE, pygpu_ocean_capsule_destructor);
  if (!capsule) {
    BKE_ocean_free(omd.ocean);
    PyErr_SetString(PyExc_RuntimeError, "Failed to create capsule for Ocean");
    return nullptr;
  }

  /* attach defaults context so simulate functions can use them when keywords omitted */
  OceanCapsuleContext *ctx = (OceanCapsuleContext *)MEM_mallocN(sizeof(OceanCapsuleContext),
                                                                "ocean_capsule_ctx");
  if (!ctx) {
    /* best-effort cleanup */
    Py_DECREF(capsule);
    BKE_ocean_free(omd.ocean);
    PyErr_NoMemory();
    return nullptr;
  }
  ctx->time = omd.time;
  ctx->scale = 1.0; /* default runtime scale */
  ctx->chop = omd.chop_amount;
  ctx->size = omd.size;
  ctx->spatial_size = omd.spatial_size;
  ctx->wave_scale = omd.wave_scale;
  ctx->smallest_wave = omd.smallest_wave;
  ctx->wind_velocity = omd.wind_velocity;
  ctx->resolution = omd.resolution;
  /* store optional spectrum name (safely) */
  ctx->spectrum[0] = '\0';
  if (spectrum_c) {
    strncpy(ctx->spectrum, spectrum_c, sizeof(ctx->spectrum) - 1);
    ctx->spectrum[sizeof(ctx->spectrum) - 1] = '\0';
  }
  PyCapsule_SetContext(capsule, ctx);

  return capsule;
}

/* Generate a Mesh from OceanModifierData using generate_ocean_geometry. */
static PyObject *pygpu_ocean_generate_object(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  PyObject *py_target_obj = nullptr;
  int resolution = 7;
  const char *name = "OceanMesh";

  if (!PyArg_ParseTuple(args, "O|iO", &py_ocean_obj, &resolution, &py_target_obj)) {
    return nullptr;
  }

  Ocean *ocean = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &ocean)) {
    return nullptr;
  }

  /* Create Mesh in Main (persistent). */
  Mesh *mesh = BKE_mesh_add(G_MAIN, name);
  if (!mesh) {
    PyErr_SetString(PyExc_RuntimeError, "generate_mesh_from_modifier failed: BKE_mesh_add");
    return nullptr;
  }

  OceanModifierData omd;
  MEMCPY_STRUCT_AFTER(&omd, DNA_struct_default_get(OceanModifierData), modifier);
  omd.ocean = ocean;

  /* Generate temporary non-main mesh, then copy into persistent mesh. */
  Mesh *tmp = gpu_generate_ocean_geometry_nomain(&omd, resolution);
  if (!tmp) {
    BKE_id_free(nullptr, &mesh->id);
    PyErr_SetString(PyExc_RuntimeError,
                    "generate_mesh_from_modifier failed: failed to create temporary mesh or "
                    "allocate buffers (check stderr)");
    return nullptr;
  }

  BKE_mesh_nomain_to_mesh(tmp, mesh, nullptr);

  if (mesh->verts_num == 0 || mesh->corners_num == 0) {
    BKE_id_free(nullptr, &mesh->id);
    PyErr_SetString(PyExc_RuntimeError,
                    "generate_mesh_from_modifier failed: copied mesh is empty");
    return nullptr;
  }

  /* NOTE: Ne pas incrémenter ici le compteur d'utilisateurs du mesh.
   * L'incrémentation sera faite une seule fois ci-dessous lors de l'affectation au Object. */

  /* Create an Object ID in Main and assign the mesh as its data. */
  Object *ob = static_cast<Object *>(BKE_id_new(G_MAIN, ID_OB, name));
  if (!ob) {
    /* cleanup mesh we created */
    BKE_id_free(nullptr, &mesh->id);
    PyErr_SetString(PyExc_RuntimeError, "generate_mesh_from_modifier failed: BKE_id_new(Object)");
    return nullptr;
  }

  ob->type = OB_MESH;
  ob->data = mesh;
  /* Increment mesh user for the object once (object now owns the mesh). */
  id_us_plus(&mesh->id);

  /* Wrap Object into PyObject and return it. */
  PyObject *py_obj = pyrna_id_CreatePyObject(&ob->id);
  if (!py_obj) {
    /* rollback: free created object and mesh safely */
    /* detach mesh from object before freeing object */
    ob->data = nullptr;
    BKE_id_free(nullptr, &ob->id);

    id_us_min(&mesh->id);
    BKE_id_free(nullptr, &mesh->id);

    PyErr_SetString(PyExc_RuntimeError,
                    "generate_mesh_from_modifier failed: pyrna_id_CreatePyObject");
    return nullptr;
  }

  /* If caller passed a target object (existing bpy.types.Object), assign its .data to our mesh.
     This preserves previous behaviour where a Python object was updated in-place. */
  if (py_target_obj != nullptr && py_target_obj != Py_None) {
    if (PyObject_HasAttrString(py_target_obj, "data")) {
      if (PyObject_SetAttrString(py_target_obj, "data", py_obj) == -1) {
        Py_DECREF(py_obj);
        return nullptr;
      }
      /* Target now holds a new reference to the wrapper -> release our local ref to avoid leak. */
      Py_DECREF(py_obj);
      /* Return the updated target object for clarity. */
      Py_RETURN_NONE;
    }
    else {
      /* Not an object with .data, ignore (caller may want the returned object). */
    }
  }

  return py_obj;
}

/* Return a new-ref bpy.types.Object for Ocean *o, creating and caching it when needed.
 * The cache stores a Python weakref (PyWeakref_NewRef) to avoid keeping the object alive.
 */
static PyObject *pygpu_ocean_get_or_create_object(PyObject *py_ocean_obj, int resolution, Ocean *o)
{
  if (!o) {
    PyErr_SetString(PyExc_ValueError, "Invalid Ocean pointer");
    return nullptr;
  }

  /* Return cached wrapper if present and still alive. The map stores a weakref object. */
  auto it = g_ocean_object_cache.find(o);
  if (it != g_ocean_object_cache.end() && it->second) {
    PyObject *weak = it->second; /* owns ref */
    /* Get the referenced object (borrowed reference). */
    PyObject *target = PyWeakref_GetObject(weak);
    if (target == nullptr) {
      /* Unexpected: weakref API failure — remove entry and continue to create a new one. */
      PyErr_Clear();
      Py_DECREF(weak);
      g_ocean_object_cache.erase(it);
    }
    else {
      /* If the referent is dead, PyWeakref_GetObject returns Py_None (borrowed). */
      if (target != Py_None) {
        /* Return a new reference to the live target. */
        Py_INCREF(target);
        return target;
      }
      else {
        /* expired: remove weakref entry and continue to create new object */
        Py_DECREF(weak);
        g_ocean_object_cache.erase(it);
      }
    }
  }

  /* Build args for pygpu_ocean_generate_object(ocean, resolution, None). */
  PyObject *py_res = PyLong_FromLong(resolution);
  if (!py_res) {
    return nullptr;
  }
  PyObject *gen_args = PyTuple_New(3);
  if (!gen_args) {
    Py_DECREF(py_res);
    return nullptr;
  }

  /* Steal/increment refs into tuple: keep py_ocean_obj as borrowed => INCREF then set. */
  Py_INCREF(py_ocean_obj);
  PyTuple_SET_ITEM(gen_args, 0, py_ocean_obj); /* steals ref */
  PyTuple_SET_ITEM(gen_args, 1, py_res);       /* steals ref */
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(gen_args, 2, Py_None); /* steals ref */

  /* Call generator (returns new-ref on success). */
  PyObject *py_created = pygpu_ocean_generate_object(nullptr, gen_args);
  Py_DECREF(gen_args);

  if (!py_created) {
    /* Error already set by generator. */
    return nullptr;
  }

  /* Create a weakref to the created wrapper and store it in cache.
   * Note: we DO NOT INCREF py_created for the cache — we create a weakref object which
   * owns its own reference. The caller receives the new-ref py_created.
   */
  PyObject *weakref = PyWeakref_NewRef(py_created, nullptr);
  if (!weakref) {
    /* unable to create weakref: clean up and error out */
    PyErr_SetString(PyExc_RuntimeError, "Failed to create weakref for cached object");
    Py_DECREF(py_created);
    return nullptr;
  }

  /* Insert weakref into cache (cache owns the weakref reference). */
  auto insert_res = g_ocean_object_cache.emplace(o, weakref);
  if (!insert_res.second) {
    /* insertion failed: release weakref and fall back to returning created object. */
    Py_DECREF(weakref);
    /* returned object py_created is still owned by caller -> return it */
    return py_created;
  }

  /* Return the created object (caller owns the returned ref). */
  return py_created;
}

/* -------------------------------------------------------------------- */
/* Export helpers (create SSBOs by copying CPU export into GPU)          */
/* -------------------------------------------------------------------- */

static PyObject *pygpu_ocean_export_htilda_ssbo(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_ocean_obj)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  float *data = nullptr;
  int len = 0;
  if (!BKE_ocean_export_htilda_float2(o, &data, &len)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_htilda_float2 failed");
    return nullptr;
  }

  const size_t byte_len = size_t(len) * 2u * sizeof(float);

  /* Try to reuse/create an 'out' cached SSBO entry for this ocean and update it in-place.
   * This avoids creating/freeing GPU buffers and Python wrappers each call. */
  SSBOCacheEntry *entry = pygpu_ocean_get_or_create_out_ssbo_entry(o, byte_len, "ocean_htilda");
  if (!entry) {
    /* Fallback: create GPU SSBO + wrapper and cache it for reuse */
    StorageBuf *ssbo = GPU_storagebuf_create_ex(byte_len, data, GPU_USAGE_STATIC, "ocean_htilda");
    BKE_ocean_free_export(data);
    if (!ssbo) {
      PyErr_SetString(PyExc_RuntimeError, "GPU_storagebuf_create_ex failed");
      return nullptr;
    }
    PyObject *py_ssbo = BPyGPUStorageBuf_CreatePyObject(reinterpret_cast<StorageBuf *>(ssbo));
    if (!py_ssbo) {
      GPU_storagebuf_free(ssbo);
      PyErr_SetString(PyExc_RuntimeError, "Failed to wrap GPU storage buffer");
      return nullptr;
    }

    /* Insert into out cache so subsequent calls reuse the wrapper/buffer */
    SSBOCacheEntry cache_entry;
    cache_entry.py_ssbo = py_ssbo; /* cache owns this ref */
    cache_entry.capacity = byte_len;
    auto insert_res = g_ocean_out_ssbo_cache.emplace(o, std::move(cache_entry));
    if (!insert_res.second) {
      /* insertion failed: release wrapper (will free native buffer) and return wrapper as
       * transient */
      Py_DECREF(py_ssbo);
      PyErr_SetString(PyExc_RuntimeError, "Failed to insert HTILDA SSBO into ocean out cache");
      return nullptr;
    }

    /* Return new-ref to cached wrapper */
    Py_INCREF(insert_res.first->second.py_ssbo);
    return insert_res.first->second.py_ssbo;
  }

  /* Update cached SSBO in-place */
  StorageBuf *ssbo = pygpu_ocean_entry_get_ssbo(entry);
  if (!ssbo) {
    BKE_ocean_free_export(data);
    PyErr_SetString(PyExc_RuntimeError, "Cached out SSBO invalid");
    return nullptr;
  }

  GPU_storagebuf_update(ssbo, data);
  BKE_ocean_free_export(data);

  /* Return cached Python wrapper (new-ref). */
  Py_INCREF(entry->py_ssbo);
  return entry->py_ssbo;
}

static PyObject *pygpu_ocean_export_disp_xyz_ssbo(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_ocean_obj)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  float *buf = nullptr;
  int texels = 0;
  if (!BKE_ocean_export_disp_xyz(o, &buf, &texels)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_disp_xyz failed");
    return nullptr;
  }

  const size_t padded_count = size_t(texels);
  const size_t padded_bytes = padded_count * 4u * sizeof(float);

  /* Use or allocate cached padded CPU buffer to avoid repeated MEM_mallocN/freeN. */
  float *padded = pygpu_ocean_get_or_alloc_padded_cpu(o, padded_bytes);
  if (!padded) {
    BKE_ocean_free_export(buf);
    PyErr_NoMemory();
    return nullptr;
  }

  /* Pack vec3 -> vec4 (parallel if available) */
  blender::threading::parallel_for(
      blender::IndexRange(texels), 2048, [&](const blender::IndexRange range) {
        for (int i : range) {
          const size_t src = size_t(i) * 3u;
          const size_t dst = size_t(i) * 4u;
          padded[dst + 0] = buf[src + 0];
          padded[dst + 1] = buf[src + 1];
          padded[dst + 2] = buf[src + 2];
          padded[dst + 3] = 0.0f;
        }
      });

  /* Try to obtain (or create) a cached GPU SSBO + Python wrapper and update it in-place. */
  SSBOCacheEntry *entry = pygpu_ocean_get_or_create_cached_ssbo_entry(
      o, padded_bytes, "ocean_disp_xyz");
  if (!entry) {
    /* Fallback: create GPU SSBO + wrapper and cache it for reuse */
    StorageBuf *ssbo = GPU_storagebuf_create_ex(
        padded_bytes, padded, GPU_USAGE_STATIC, "ocean_disp_xyz");
    BKE_ocean_free_export(buf);
    if (!ssbo) {
      PyErr_SetString(PyExc_RuntimeError, "GPU_storagebuf_create_ex failed");
      return nullptr;
    }
    PyObject *py_ssbo = BPyGPUStorageBuf_CreatePyObject(reinterpret_cast<StorageBuf *>(ssbo));
    if (!py_ssbo) {
      GPU_storagebuf_free(ssbo);
      PyErr_SetString(PyExc_RuntimeError, "Failed to wrap GPU storage buffer");
      return nullptr;
    }

    /* Insert into disp cache so subsequent calls reuse it */
    SSBOCacheEntry cache_entry;
    cache_entry.py_ssbo = py_ssbo;
    cache_entry.capacity = padded_bytes;
    auto insert_res = g_ocean_ssbo_cache.emplace(o, std::move(cache_entry));
    if (!insert_res.second) {
      Py_DECREF(py_ssbo);
      PyErr_SetString(PyExc_RuntimeError, "Failed to insert disp SSBO into ocean cache");
      return nullptr;
    }

    /* Return new-ref to cached wrapper */
    Py_INCREF(insert_res.first->second.py_ssbo);
    return insert_res.first->second.py_ssbo;
  }

  StorageBuf *ssbo = pygpu_ocean_entry_get_ssbo(entry);
  if (!ssbo) {
    BKE_ocean_free_export(buf);
    PyErr_SetString(PyExc_RuntimeError, "Cached SSBO invalid");
    return nullptr;
  }

  GPU_storagebuf_update(ssbo, padded);
  BKE_ocean_free_export(buf);

  /* Return cached Python wrapper (new-ref). */
  Py_INCREF(entry->py_ssbo);
  return entry->py_ssbo;
}

/* -------------------------------------------------------------------- */
/* Simulation helpers                                                   */
/* -------------------------------------------------------------------- */

static bool g_ocean_debug_dump = false;
static bool g_ocean_debug_dump_full = false;

/* Control flags for fast iterative testing / profiling */
static bool g_ocean_enable_cpu_simulate = false; /* set false to skip the CPU simulate call */
static bool g_ocean_debug_prints = false;       /* use to enable the debug prints */

/* Profiling / framerate simple for gpu.ocean */
static bool g_ocean_show_fps = false;
/* Throttle prints to ~1s windows to avoid flooding the console */
static std::chrono::steady_clock::time_point g_ocean_prof_last_print_time =
    std::chrono::steady_clock::now();
static std::chrono::steady_clock::time_point g_ocean_prof_last_frame_time =
    std::chrono::steady_clock::now();

/* Conditional debug print macro (replace plain printf(...) with OCEAN_DBG_PRINT(...)) */
#define OCEAN_DBG_PRINT(...) \
  do { \
    if (g_ocean_debug_prints) { \
      printf(__VA_ARGS__); \
      fflush(stdout); \
    } \
  } while (0)

static inline bool pygpu_is_power_of_two(int v)
{
  return (v > 0) && ((v & (v - 1)) == 0);
}

static blender::gpu::Shader *g_ocean_eval_shader = nullptr;

// GLSL compute body (sans les `layout` et `uniform`, à injecter dans un R"GLSL(...)GLSL" string)
static const char *OCEAN_EVAL_COMP_BODY_GLSL = R"GLSL(
/* positive modulo helper */
int mod_pos(int a, int b) {
  int r = a % b;
  return (r < 0) ? r + b : r;
}

/* BILERP using SSBO 'disp' laid out as (i * N + j) */
vec3 bilerp_disp(int i0, int j0, float fx, float fz) {
  int i1 = i0 + 1;
  int j1 = j0 + 1;

  /* wrap indices like CPU: ensure 0 <= index < M/N */
  int ii0 = mod_pos(i0, M);
  int ii1 = mod_pos(i1, M);
  int jj0 = mod_pos(j0, N);
  int jj1 = mod_pos(j1, N);

  int idx00 = ii0 * N + jj0;
  int idx10 = ii1 * N + jj0;
  int idx01 = ii0 * N + jj1;
  int idx11 = ii1 * N + jj1;

  vec3 c00 = disp[idx00].xyz;
  vec3 c10 = disp[idx10].xyz;
  vec3 c01 = disp[idx01].xyz;
  vec3 c11 = disp[idx11].xyz;

  vec3 lx0 = mix(c00, c10, fx);
  vec3 lx1 = mix(c01, c11, fx);
  return mix(lx0, lx1, fz);
}

void main() {
  uint idx = gl_GlobalInvocationID.x;
  if (idx >= outbuf.length()) {
    return;
  }

  vec2 bp = basepos[idx].xy;
  float vx = bp.x;
  float vy = bp.y;

  float inv_size = (size_param != 0.0) ? (1.0 / size_param) : 1.0;
  float u = vx * inv_size + 0.5;
  float v = vy * inv_size + 0.5;

  float uu = u * float(M);
  float vv = v * float(N);
  int i0 = int(floor(uu));
  int j0 = int(floor(vv));
  float fx = uu - float(i0);
  float fz = vv - float(j0);

  vec3 d = bilerp_disp(i0, j0, fx, fz);

  /* Remapping corrected:
   * - vx is base X
   * - vy is base Y slot in buffer but represents the horizontal second axis (intended Z)
   * - d.x/d.z are horizontal displacements, d.y is height
   *
   * Final mesh vertex (Blender convention Z up) = (X, Y, Z) =
   *   X = vx + d.x
   *   Y = vy + d.z
   *   Z = d.y
   */
  vec3 pos_local = vec3(vx + d.x, vy + d.z, d.y);
  outbuf[idx] = vec4(pos_local, 1.0);
}
)GLSL";

/* Create & cache compute shader. This fills ShaderCreateInfo with push-constants we need:
 *   - int M
 *   - int N
 *   - float size_param
 *
 * Note: the runtime helper `info.push_constant(...)` will declare push-constants / uniforms
 * in the final GLSL when the shader is built by the Blender shader creator.
 */
static blender::gpu::Shader *pygpu_ocean_ensure_eval_shader()
{
  if (g_ocean_eval_shader) {
    return g_ocean_eval_shader;
  }

  using namespace blender::gpu::shader;

  ShaderCreateInfo info("ocean_eval_comp");

  /* Use only the GLSL compute body: resource declarations (bindings) are provided
   * by the ShaderCreateInfo machinery / runtime when the shader is created. */
  info.compute_source_generated = OCEAN_EVAL_COMP_BODY_GLSL;
  info.compute_source("draw_colormanagement_lib.glsl");

  info.local_group_size(256, 1, 1);

  info.storage_buf(0, Qualifier::read, "vec4", "basepos[]");
  info.storage_buf(1, Qualifier::read, "vec4", "disp[]");
  info.storage_buf(2, Qualifier::write, "vec4", "outbuf[]");

  /* Push-constants used by the GLSL body (names must match exactly). */
  info.push_constant(Type::int_t, "M", 0);
  info.push_constant(Type::int_t, "N", 0);
  info.push_constant(Type::float_t, "size_param");

  /* Optionally declare a specialization constant for workgroup size:
   * info.specialization_constant(Type::int_t, "LOCAL_SIZE_X", 256);
   * (then replace any hard-coded layout(local_size_x = ...) in the GLSL body). */

  /* Create and cache the shader */
  g_ocean_eval_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  if (!g_ocean_eval_shader) {
    return nullptr;
  }

  return g_ocean_eval_shader;
}

static bool pygpu_ocean_dispatch_eval_shader(StorageBuf *base_sb,
                                             StorageBuf *disp_sb,
                                             StorageBuf *out_sb,
                                             Ocean *ocean,
                                             float size_param,
                                             size_t verts)
{
  if (!base_sb || !disp_sb || !out_sb || !ocean) {
    PyErr_SetString(PyExc_ValueError, "Invalid arguments to dispatch_eval_shader");
    return false;
  }

  blender::gpu::Shader *sh = pygpu_ocean_ensure_eval_shader();
  if (!sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create/obtain ocean eval shader");
    return false;
  }

  /* Query grid shape via public API for logging (best-effort). */
  int M_val = 0, N_val = 0;
  if (!BKE_ocean_export_shape(ocean, &M_val, &N_val)) {
    /* Not fatal for logging, keep going but mark unknown */
    M_val = 0;
    N_val = 0;
  }

  GPU_shader_bind(sh);

  /* Bind SSBOs to expected bindings (0=basepos, 1=disp, 2=out) */
  GPU_storagebuf_bind(base_sb, 0);
  GPU_storagebuf_bind(disp_sb, 1);
  GPU_storagebuf_bind(out_sb, 2);

  int loc = GPU_shader_get_uniform(sh, "M");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &M_val);
  }
  loc = GPU_shader_get_uniform(sh, "N");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &N_val);
  }
  loc = GPU_shader_get_uniform(sh, "size_param");
  float spf = size_param;
  if (loc != -1) {
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &spf);
  }

  /* Dispatch compute */
  const uint32_t local_size_x = 256u;
  const uint32_t groups_x = uint32_t((verts + local_size_x - 1) / local_size_x);

  /* Diagnostic print */
  OCEAN_DBG_PRINT("[ocean_dispatch] shader=eval verts=%llu M=%d N=%d groups_x=%u\n",
         (unsigned long long)verts,
         M_val,
         N_val,
         groups_x);

  GPU_compute_dispatch(sh, groups_x, 1u, 1u);

  /* Ensure SSBO writes are visible */
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup */
  GPU_shader_unbind();

  return true;
}

/* Python wrapper: evaluate_disp_with_ssbos(ocean, basepos_ssbo, disp_ssbo, out_ssbo,
 * size_param:float=1.0) */
static PyObject *pygpu_ocean_evaluate_disp_with_ssbos(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  PyObject *py_base_sb_obj = nullptr;
  PyObject *py_disp_sb_obj = nullptr;
  PyObject *py_out_sb_obj = nullptr;
  double size_param = 1.0;

  if (!PyArg_ParseTuple(args,
                        "OOOO|d",
                        &py_ocean_obj,
                        &py_base_sb_obj,
                        &py_disp_sb_obj,
                        &py_out_sb_obj,
                        &size_param))
  {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  if (!PyObject_TypeCheck(py_base_sb_obj, &BPyGPUStorageBuf_Type) ||
      !PyObject_TypeCheck(py_disp_sb_obj, &BPyGPUStorageBuf_Type) ||
      !PyObject_TypeCheck(py_out_sb_obj, &BPyGPUStorageBuf_Type))
  {
    PyErr_SetString(PyExc_TypeError, "basepos, disp and out must be GPUStorageBuf Python objects");
    return nullptr;
  }

  BPyGPUStorageBuf *b_base = reinterpret_cast<BPyGPUStorageBuf *>(py_base_sb_obj);
  BPyGPUStorageBuf *b_disp = reinterpret_cast<BPyGPUStorageBuf *>(py_disp_sb_obj);
  BPyGPUStorageBuf *b_out = reinterpret_cast<BPyGPUStorageBuf *>(py_out_sb_obj);

  if (!b_base->ssbo || !b_disp->ssbo || !b_out->ssbo) {
    PyErr_SetString(PyExc_ReferenceError, "One of provided GPUStorageBuf has been freed");
    return nullptr;
  }

  /* Determine number of verts to dispatch:
   * Prefer querying the out buffer length (if backend supports it) because caller
   * is expected to size the out SSBO correctly. Fall back to base size or ocean shape. */
  size_t verts = 0;

  if (verts == 0) {
    /* fallback to ocean shape (verts = (M+1)*(N+1)) */
    int Mv = 0, Nv = 0;
    if (BKE_ocean_export_shape(o, &Mv, &Nv) && Mv > 0 && Nv > 0) {
      verts = size_t(Mv + 1) * size_t(Nv + 1);
    }
  }

  if (verts == 0) {
    PyErr_SetString(PyExc_RuntimeError,
                    "Unable to determine vertex count for eval shader dispatch (provide an out "
                    "SSBO sized to verts*vec4)");
    return nullptr;
  }

  if (!pygpu_ocean_dispatch_eval_shader(
          b_base->ssbo, b_disp->ssbo, b_out->ssbo, o, float(size_param), verts))
  {
    return nullptr;
  }

  Py_RETURN_NONE;
}

/* Python helper: full end-to-end test: build basepos grid, export disp, dispatch shader.
 * Usage from Python:
 *    gpu.ocean.test_eval_shader(ocean_capsule, size_param:float=1.0)
 */
static PyObject *pygpu_ocean_test_eval_shader(PyObject * /*self*/, PyObject *args)
{
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError,
                    "pygpu_ocean_test_eval_shader: GPU context is not active (cannot run GPU test "
                    "in this process)");
    return nullptr;
  }

  PyObject *py_ocean_obj = nullptr;
  double size_param = 1.0;

  if (!PyArg_ParseTuple(args, "O|d", &py_ocean_obj, &size_param)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return nullptr;
  }

  const size_t verts = size_t(M + 1) * size_t(N + 1);

  BKE_ocean_simulate(o, 0.0f, 1.0f, 0.0f);

  /* Export disp and pad into cached CPU buffer */
  float *disp_buf = nullptr;
  int texels = 0;
  if (!BKE_ocean_export_disp_xyz(o, &disp_buf, &texels)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_disp_xyz_threaded failed");
    return nullptr;
  }

  const size_t disp_padded_bytes = size_t(texels) * 4u * sizeof(float);
  float *disp_padded = pygpu_ocean_get_or_alloc_padded_cpu(o, disp_padded_bytes);
  if (!disp_padded) {
    BKE_ocean_free_export(disp_buf);
    PyErr_NoMemory();
    return nullptr;
  }
  memset(disp_padded, 0, disp_padded_bytes);
  for (int i = 0; i < texels; ++i) {
    const size_t src = size_t(i) * 3u;
    const size_t dst = size_t(i) * 4u;
    disp_padded[dst + 0] = disp_buf[src + 0];
    disp_padded[dst + 1] = disp_buf[src + 1];
    disp_padded[dst + 2] = disp_buf[src + 2];
    disp_padded[dst + 3] = 0.0f;
  }

  /* Reuse/create cached disp SSBO and update */
  SSBOCacheEntry *disp_entry = pygpu_ocean_get_or_create_cached_ssbo_entry(
      o, disp_padded_bytes, "ocean_test_disp");
  StorageBuf *disp_ssbo = nullptr;
  bool created_disp_transient = false;
  if (disp_entry) {
    disp_ssbo = pygpu_ocean_entry_get_ssbo(disp_entry);
  }
  if (disp_ssbo) {
    GPU_storagebuf_update(disp_ssbo, disp_padded);
  }
  else {
    /* Create GPU SSBO + Python wrapper and cache it so future calls reuse it */
    StorageBuf *new_disp = GPU_storagebuf_create_ex(
        disp_padded_bytes, disp_padded, GPU_USAGE_STATIC, "ocean_test_disp");
    if (!new_disp) {
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "GPU alloc failed (disp)");
      return nullptr;
    }
    PyObject *py_new_disp = BPyGPUStorageBuf_CreatePyObject(
        reinterpret_cast<StorageBuf *>(new_disp));
    if (!py_new_disp) {
      GPU_storagebuf_free(new_disp);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Failed to wrap GPU storage buffer (disp)");
      return nullptr;
    }

    /* Insert into disp cache */
    SSBOCacheEntry entry_disp;
    entry_disp.py_ssbo = py_new_disp;
    entry_disp.capacity = disp_padded_bytes;
    auto ins_disp = g_ocean_ssbo_cache.emplace(o, std::move(entry_disp));
    if (!ins_disp.second) {
      Py_DECREF(py_new_disp);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Failed to cache disp SSBO");
      return nullptr;
    }
    /* Cache now owns the wrapper -> mark as not transient */
    created_disp_transient = false;
    disp_ssbo = pygpu_ocean_entry_get_ssbo(&ins_disp.first->second);
    if (!disp_ssbo) {
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Cached disp SSBO invalid after insert");
      return nullptr;
    }
  }

  /* Build basepos CPU buffer and update cached base SSBO */
  const size_t base_bytes = verts * 4u * sizeof(float);
  float *base_cpu = static_cast<float *>(MEM_mallocN(base_bytes, "ocean_test_basepos"));
  if (!base_cpu) {
    if (created_disp_transient && disp_ssbo) {
      GPU_storagebuf_free(disp_ssbo);
    }
    BKE_ocean_free_export(disp_buf);
    PyErr_NoMemory();
    return nullptr;
  }
  const float half = float(size_param) * 0.5f;
  for (int i = 0; i <= M; ++i) {
    for (int j = 0; j <= N; ++j) {
      const size_t idx = size_t(i) * size_t(N + 1) + size_t(j);
      const size_t off = idx * 4u;
      float vx = (float(j) / float(N)) * float(size_param) - half;
      float vy = (float(i) / float(M)) * float(size_param) - half;
      base_cpu[off + 0] = vx;
      base_cpu[off + 1] = vy;
      base_cpu[off + 2] = 0.0f;
      base_cpu[off + 3] = 0.0f;
    }
  }

  SSBOCacheEntry *base_entry = pygpu_ocean_get_or_create_base_ssbo_entry(
      o, base_bytes, "ocean_test_basepos");
  StorageBuf *base_ssbo = nullptr;
  bool created_base_transient = false;
  if (base_entry) {
    base_ssbo = pygpu_ocean_entry_get_ssbo(base_entry);
  }
  if (base_ssbo) {
    GPU_storagebuf_update(base_ssbo, base_cpu);
  }
  else {
    /* Create GPU SSBO + Python wrapper and cache it */
    StorageBuf *new_base = GPU_storagebuf_create_ex(
        base_bytes, base_cpu, GPU_USAGE_STATIC, "ocean_test_basepos");
    if (!new_base) {
      if (created_disp_transient && disp_ssbo) {
        GPU_storagebuf_free(disp_ssbo);
      }
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "GPU alloc failed (base)");
      return nullptr;
    }
    PyObject *py_new_base = BPyGPUStorageBuf_CreatePyObject(
        reinterpret_cast<StorageBuf *>(new_base));
    if (!py_new_base) {
      GPU_storagebuf_free(new_base);
      if (created_disp_transient && disp_ssbo) {
        GPU_storagebuf_free(disp_ssbo);
      }
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Failed to wrap GPU storage buffer (base)");
      return nullptr;
    }

    /* Insert into base cache */
    SSBOCacheEntry entry_base;
    entry_base.py_ssbo = py_new_base;
    entry_base.capacity = base_bytes;
    auto ins_base = g_ocean_base_ssbo_cache.emplace(o, std::move(entry_base));
    if (!ins_base.second) {
      Py_DECREF(py_new_base);
      if (created_disp_transient && disp_ssbo) {
        GPU_storagebuf_free(disp_ssbo);
      }
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Failed to cache base SSBO");
      return nullptr;
    }
    /* Cache now owns wrapper -> mark as not transient */
    created_base_transient = false;
    base_ssbo = pygpu_ocean_entry_get_ssbo(&ins_base.first->second);
    if (!base_ssbo) {
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Cached base SSBO invalid after insert");
      return nullptr;
    }
  }

  /* Create/zero an out SSBO (reuse cached out if present) */
  const size_t out_bytes = verts * 4u * sizeof(float);
  SSBOCacheEntry *out_entry = pygpu_ocean_get_or_create_out_ssbo_entry(
      o, out_bytes, "ocean_test_out");
  StorageBuf *out_ssbo = nullptr;
  bool created_out_transient = false;
  if (out_entry) {
    out_ssbo = pygpu_ocean_entry_get_ssbo(out_entry);
  }
  if (out_ssbo) {
    const size_t floats_count = out_bytes / sizeof(float);
    static thread_local std::vector<float> zero_buf;
    if (zero_buf.size() < floats_count) {
      zero_buf.assign(floats_count, 0.0f);
    }
    else {
      std::fill(zero_buf.begin(), zero_buf.begin() + floats_count, 0.0f);
    }
    GPU_storagebuf_update(out_ssbo, zero_buf.data());
  }
  else {
    /* Create GPU SSBO + Python wrapper and cache it */
    float *out_cpu = static_cast<float *>(MEM_callocN(out_bytes, "ocean_test_outcpu"));
    if (!out_cpu) {
      if (created_disp_transient && disp_ssbo) {
        GPU_storagebuf_free(disp_ssbo);
      }
      if (created_base_transient && base_ssbo) {
        GPU_storagebuf_free(base_ssbo);
      }
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_NoMemory();
      return nullptr;
    }
    StorageBuf *new_out = GPU_storagebuf_create_ex(
        out_bytes, out_cpu, GPU_USAGE_STATIC, "ocean_test_out");
    MEM_freeN(out_cpu);
    if (!new_out) {
      if (created_disp_transient && disp_ssbo) {
        GPU_storagebuf_free(disp_ssbo);
      }
      if (created_base_transient && base_ssbo) {
        GPU_storagebuf_free(base_ssbo);
      }
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "GPU_storagebuf_create_ex failed (out)");
      return nullptr;
    }
    PyObject *py_new_out = BPyGPUStorageBuf_CreatePyObject(
        reinterpret_cast<StorageBuf *>(new_out));
    if (!py_new_out) {
      GPU_storagebuf_free(new_out);
      if (created_disp_transient && disp_ssbo) {
        GPU_storagebuf_free(disp_ssbo);
      }
      if (created_base_transient && base_ssbo) {
        GPU_storagebuf_free(base_ssbo);
      }
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Failed to wrap GPU storage buffer (out)");
      return nullptr;
    }

    /* Insert into out cache */
    SSBOCacheEntry entry_out;
    entry_out.py_ssbo = py_new_out;
    entry_out.capacity = out_bytes;
    auto ins_out = g_ocean_out_ssbo_cache.emplace(o, std::move(entry_out));
    if (!ins_out.second) {
      Py_DECREF(py_new_out);
      if (created_disp_transient && disp_ssbo) {
        GPU_storagebuf_free(disp_ssbo);
      }
      if (created_base_transient && base_ssbo) {
        GPU_storagebuf_free(base_ssbo);
      }
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Failed to cache out SSBO");
      return nullptr;
    }
    /* Cache now owns wrapper -> mark as not transient */
    created_out_transient = false;
    out_ssbo = pygpu_ocean_entry_get_ssbo(&ins_out.first->second);
    if (!out_ssbo) {
      MEM_freeN(base_cpu);
      BKE_ocean_free_export(disp_buf);
      PyErr_SetString(PyExc_RuntimeError, "Cached out SSBO invalid after insert");
      return nullptr;
    }
  }

  /* Dispatch eval shader */
  const bool ok = pygpu_ocean_dispatch_eval_shader(
      base_ssbo, disp_ssbo, out_ssbo, o, float(size_param), verts);

  /* cleanup CPU temporaries */
  MEM_freeN(base_cpu);
  BKE_ocean_free_export(disp_buf);

  if (!ok) {
    if (created_out_transient && out_ssbo)
      GPU_storagebuf_free(out_ssbo);
    if (created_base_transient && base_ssbo)
      GPU_storagebuf_free(base_ssbo);
    if (created_disp_transient && disp_ssbo)
      GPU_storagebuf_free(disp_ssbo);
    return nullptr;
  }

  /* Free only the transient SSBOs (cached ones remain owned by cache/wrappers). */
  if (created_out_transient && out_ssbo)
    GPU_storagebuf_free(out_ssbo);
  if (created_base_transient && base_ssbo)
    GPU_storagebuf_free(base_ssbo);
  if (created_disp_transient && disp_ssbo)
    GPU_storagebuf_free(disp_ssbo);

  Py_RETURN_NONE;
}

/* -------------------------------------------------------------------- */
/* Simulation helpers End                                               */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Simulation Begin                                                     */
/* -------------------------------------------------------------------- */

/* ----------------- BEGIN: Prototype GPU iFFT rows (naive DFT) -----------------
 * Prototype to perform row-wise spectral transform on GPU.
 * - Naive DFT per row
 * - Transpose kernel to prepare for column passes later
 * - Simple copy of exported htilda into GPU buffer
 * - More than enough for small resolutions (we are anyway limited (buffer sizes... on GPU)
 * - Note: Stockam (pow2 only) works fine but not flexible enough for arbitrary sizes, and bluestein
 *   did not bring performances improvements over naive dft for arbitrary sizes in my tests.
 */

/* shader: naive 1D DFT per row
 * inbuf/outbuf laid out row-major: index = row * N + k
 * Push-constants: int M, int N
 */
static const char *OCEAN_FFT_ROW_DFT_COMP_BODY_GLSL = R"GLSL(
#define TWO_PI 6.28318530717958647692

vec2 c_mul(vec2 a, vec2 b) { return vec2(a.x*b.x - a.y*b.y, a.x*b.y + a.y*b.x); }
vec2 c_add(vec2 a, vec2 b) { return a + b; }

void main() {
  uint gid = gl_GlobalInvocationID.x;
  uint total = uint(M) * uint(N);
  if (gid >= total) {
    return;
  }
  int idx = int(gid);
  int Nloc = N;
  int row = idx / Nloc;
  int k = idx % Nloc;

  vec2 sum = vec2(0.0, 0.0);
  for (int n = 0; n < Nloc; ++n) {
    int src_idx = row * Nloc + n;
    vec2 c = rows_in[src_idx].xy;
    float angle = -TWO_PI * float(k * n) / float(Nloc);
    vec2 tw = vec2(cos(angle), sin(angle));
    sum = c_add(sum, c_mul(c, tw));
  }

  /* Keep per-pass scaling = 1.0 here; normalisation must be handled consistently
   * at a single place (CPU or a single GPU stage). Removing 1/N avoids total
   * 1/(M*N) double-normalisation. */
  float out_scale = 1.0;
  rows_out[idx] = sum * out_scale * SCALE_FAC;
}
)GLSL";

/* shader: transpose complex buffer MxN -> NxM
 * src index = i * N + j  (i in 0..M-1, j in 0..N-1)
 * dst index = j * M + i
 * push constants: int M, int N
 */
static const char *OCEAN_TRANSPOSE_COMP_BODY_GLSL = R"GLSL(
void main() {
  uint gid = gl_GlobalInvocationID.x;
  int Mloc = M;
  int Nloc = N;

  /* Bounds check: avoid out-of-range threads writing/reading past SSBO length.
   * Without this some extra work-items (due to group rounding) can cause
   * undefined behaviour or silent zero-writes on some drivers. */
  uint total = uint(Mloc) * uint(Nloc);
  if (gid >= total) {
    return;
  }

  int idx = int(gid);
  int i = idx / Nloc;
  int j = idx % Nloc;
  int src = i * Nloc + j;
  int dst = j * Mloc + i;

  /* copy components explicitly to avoid potential driver/packing issues */
  vec2 v = srcbuf[src];
  transposed[dst].x = v.x;
  transposed[dst].y = v.y;
}
)GLSL";

static const char *OCEAN_HTILDA_EXPAND_COMP_BODY_GLSL = R"GLSL(
void main() {
  uint gid = gl_GlobalInvocationID.x;
  uint total = dst_full.length();
  if (gid >= total) {
    return;
  }
  int idx = int(gid);
  int Nloc = N;
  int half_count = halfN;
  int row = idx / Nloc;
  int n = idx % Nloc;
  int src_idx;
  vec2 v;
  if (n <= Nloc / 2) {
    src_idx = row * half_count + n;
    v = src_half[src_idx].xy;
  }
  else {
    int mirror = Nloc - n;
    src_idx = row * half_count + mirror;
    vec2 a = src_half[src_idx].xy;
    v = vec2(a.x, -a.y);
  }
  dst_full[idx] = v;
}
)GLSL";

static const char *OCEAN_PREP_FFTIN_CHOP_COMP_BODY_GLSL = R"GLSL(
// src: vec2 htilda[] (complex re,im) layout row-major M*N
// dst_x: vec2 out_fft_in_x[]  (complex)
// dst_z: vec2 out_fft_in_z[]  (complex)
// push-constants: int M, int N, float CHOP, float SIZE_PARAM, float SCALE_FAC
#define TWO_PI 6.28318530717958647692

vec2 c_mul(vec2 a, vec2 b) { return vec2(a.x*b.x - a.y*b.y, a.x*b.y + a.y*b.x); }

void main() {
  uint gid = gl_GlobalInvocationID.x;
  uint total = uint(M) * uint(N);
  if (gid >= total) return;

  int idx = int(gid);
  int Nloc = N;
  int i = idx / Nloc; // row index -> corresponds to kx index
  int j = idx % Nloc; // column index -> kz index

  // compute kx and kz consistent with CPU: k = 2*pi * (idx <= M/2 ? idx : idx - M) / L
  float inv_size = (SIZE_PARAM != 0.0) ? (1.0 / SIZE_PARAM) : 1.0;
  int kx_idx = (i <= M/2) ? i : i - M;
  int kz_idx = (j <= N/2) ? j : j - N;
  float kx = TWO_PI * float(kx_idx) * inv_size;
  float kz = TWO_PI * float(kz_idx) * inv_size;
  float k = sqrt(kx*kx + kz*kz);

  vec2 h = src[idx]; // htilda (re,im)

  // factor = (k==0) ? 0 : (chop * (component / k))
  float fx = (k == 0.0) ? 0.0 : (CHOP * (kx / k));
  float fz = (k == 0.0) ? 0.0 : (CHOP * (kz / k));

  // Multiply by -i * factor : complex mul by (0, -factor)
  // (0,-f) * (a + i b) = ( f * b, -f * a )
  vec2 mul_x = vec2( fx * h.y, -fx * h.x );
  vec2 mul_z = vec2( fz * h.y, -fz * h.x );

  // apply global scale fac if needed (matches CPU scaling before iFFT)
  mul_x *= SCALE_FAC;
  mul_z *= SCALE_FAC;

  dst_x[idx] = mul_x;
  dst_z[idx] = mul_z;
}
)GLSL";

/* New: complex3 -> disp (reads three vec2 complex arrays and writes vec4(dx,dy,dz,0)) */
static const char *OCEAN_COMPLEX3_TO_DISP_COMP_BODY_GLSL = R"GLSL(
void main() {
  uint idx = gl_GlobalInvocationID.x;
  uint total = dst.length();
  if (idx >= total) return;

  // src_complex: vec2 (spatial Y), src_x: vec2 (spatial X), src_z: vec2 (spatial Z)
  vec2 c = src_complex[idx];
  vec2 cx = src_x[idx];
  vec2 cz = src_z[idx];

  // use real part from each complex buffer
  float dx = cx.x;
  float dy = c.x;
  float dz = cz.x;

  dst[idx] = vec4(dx, dy, dz, 0.0);
}
)GLSL";

static const char *OCEAN_VEC2_COPY_COMP_BODY_GLSL = R"GLSL(
// Simple vec2 copy: src_vec2[] -> dst_vec2[]
void main() {
  uint idx = gl_GlobalInvocationID.x;
  dst_vec2[idx] = src_vec2[idx];
}
)GLSL";

/* ---- normals compute shader (sample disp tex + central-difference normals) ---- */
static const char *OCEAN_FINAL_COMP_BODY_GLSL = R"GLSL(
int pack_i10_trunc(float x) { return clamp(int(x * 511.0), -512, 511) & 0x3FF; }
uint pack_norm(vec3 n) {
  return uint(pack_i10_trunc(n.x)) | (uint(pack_i10_trunc(n.y)) << 10) | (uint(pack_i10_trunc(n.z)) << 20);
}

void main() {
  uint gid = gl_GlobalInvocationID.x;
  if (gid >= normals_out.length()) {
    return;
  }

  /* read basepos (vec4) and compute uv */
  positions[gid] = basepos[gid];
  vec2 uv = (bp.xy / size_param) + vec2(0.5);

  /* center sample */
  vec3 c = sample_disp_tex_bilerp(tex_side, inv_tex_side, uv);
  vec3 p_center = vec3(bp.x + c.x, bp.y + c.z, bp.z + c.y * HEIGHT_SCALE);

  /* small offsets in uv to sample neighbors (one texel) */
  float du = inv_tex_side;
  float dv = inv_tex_side;
  vec3 cr = sample_disp_tex_bilerp(tex_side, inv_tex_side, uv + vec2(du, 0.0));
  vec3 cu = sample_disp_tex_bilerp(tex_side, inv_tex_side, uv + vec2(0.0, dv));
  vec3 p_right = vec3(bp.x + cr.x, bp.y + cr.z, bp.z + cr.y * HEIGHT_SCALE);
  vec3 p_up = vec3(bp.x + cu.x, bp.y + cu.z, bp.z + cu.y * HEIGHT_SCALE);

  vec3 n = normalize(cross(p_right - p_center, p_up - p_center));
  uint packed = pack_norm(n);
  normals_out[uint(gid)] = packed;
}
)GLSL";

/* GLSL: compute htilda per compact element (row-major, count = M * halfN)
 * bindings:
 *  0 = vec2 h0_compact[]         (re,im)
 *  1 = vec2 h0_minus_compact[]   (re,im)
 *  2 = vec2 dst_htilda[]         (re,im)
 *
 * push-constants / uniforms:
 *   int M, int N, int halfN
 *   float TIME_PARAM, float SCALE_FAC, float SIZE_PARAM
 */
static const char *OCEAN_HTILDA_SIMULATE_COMP_BODY_GLSL = R"GLSL(
#define TWO_PI 6.28318530717958647692
const float G_CONST = 9.81;

void main() {
  uint gid = gl_GlobalInvocationID.x;
  uint total = uint(M) * uint(halfN);
  if (gid >= total) return;

  // compact indices
  int i = int(gid) / halfN;
  int j = int(gid) % halfN;

  vec2 h0 = h0_compact[gid];
  vec2 h0m = h0_minus_compact[gid];

  // compute omega from indices (matches CPU convention used elsewhere)
  int kx_idx = (i <= M/2) ? i : i - M;
  int kz_idx = j; /* compact j always <= N/2 */
  float inv_size = (SIZE_PARAM != 0.0) ? (1.0 / SIZE_PARAM) : 1.0;
  float kx = TWO_PI * float(kx_idx) * inv_size;
  float kz = TWO_PI * float(kz_idx) * inv_size;
  float k = sqrt(kx * kx + kz * kz);
  float w = (k > 0.0) ? sqrt(G_CONST * k) : 0.0;

  float ph = w * TIME_PARAM;
  float co = cos(ph);
  float si = sin(ph);

  vec2 exp_p = vec2(co, si);
  vec2 exp_m = vec2(co, -si);

  vec2 a;
  a.x = h0.x * exp_p.x - h0.y * exp_p.y;
  a.y = h0.x * exp_p.y + h0.y * exp_p.x;

  vec2 conj_h0m = vec2(h0m.x, -h0m.y);

  vec2 b;
  b.x = conj_h0m.x * exp_m.x - conj_h0m.y * exp_m.y;
  b.y = conj_h0m.x * exp_m.y + conj_h0m.y * exp_m.x;

  vec2 ht = vec2(a.x + b.x, a.y + b.y);

  // final scale (user-provided)
  dst_htilda[gid] = ht * SCALE_FAC;
}
)GLSL";

/* shader objects */
static blender::gpu::Shader *g_ocean_fft_row_dft_shader = nullptr;

static blender::gpu::Shader *g_ocean_htilda_expand_shader = nullptr;
static blender::gpu::Shader *g_ocean_htilda_simulate_shader = nullptr;
static blender::gpu::Shader *g_ocean_vec2_copy_shader = nullptr;
static blender::gpu::Shader *g_ocean_transpose_shader = nullptr;
static blender::gpu::Shader *g_ocean_prep_fftin_chop_shader = nullptr;
static blender::gpu::Shader *g_ocean_complex3_to_disp_shader = nullptr;
static blender::gpu::Shader *g_ocean_final_shader = nullptr;

/* -------------------------------------------------------------------- */
/* Main debug helpers                                                   */
/* -------------------------------------------------------------------- */

/* Python wrapper: set_show_fps(on: bool) */
static PyObject *pygpu_ocean_set_show_fps(PyObject * /*self*/, PyObject *args)
{
  int on = 0;
  if (!PyArg_ParseTuple(args, "p", &on)) {
    return nullptr;
  }
  g_ocean_show_fps = (on != 0);
  Py_RETURN_NONE;
}

/* Python wrapper to enable/disable dumps at runtime:
 *   gpu.ocean.set_debug_dumps(on: bool)
 */
static PyObject *pygpu_ocean_set_debug_dumps(PyObject * /*self*/, PyObject *args)
{
  int on = 0;
  if (!PyArg_ParseTuple(args, "p", &on)) {
    return nullptr;
  }
  g_ocean_debug_dump = (on != 0);
  Py_RETURN_NONE;
}

/* Python wrapper: set_debug_dumps_full(on: bool) */
static PyObject *pygpu_ocean_set_debug_dumps_full(PyObject * /*self*/, PyObject *args)
{
  int on = 0;
  if (!PyArg_ParseTuple(args, "p", &on)) {
    return nullptr;
  }
  g_ocean_debug_dump_full = (on != 0);
  Py_RETURN_NONE;
}

static PyObject *pygpu_ocean_dump_ssbo_indices(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  PyObject *py_sb_obj = nullptr;
  PyObject *py_arg2 = nullptr;
  PyObject *py_arg3 = nullptr;
  const char *label = nullptr;
  static char *kwlist[] = {(char *)"ssbo",
                           (char *)"element_count_or_indices",
                           (char *)"indices_or_element_count",
                           (char *)"label",
                           nullptr};

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "OO|Os", kwlist, &py_sb_obj, &py_arg2, &py_arg3, &label))
  {
    return nullptr;
  }

  /* Validate SSBO object */
  if (!PyObject_TypeCheck(py_sb_obj, &BPyGPUStorageBuf_Type)) {
    PyErr_SetString(PyExc_TypeError, "first argument must be a GPUStorageBuf Python object");
    return nullptr;
  }

  /* Decide which argument is element_count and which is indices. */
  int element_count = 0;
  PyObject *py_indices = nullptr;

  if (PyLong_Check(py_arg2)) {
    /* form: (ssbo, element_count, indices?, label?) */
    element_count = (int)PyLong_AsLong(py_arg2);
    if (PyErr_Occurred()) {
      return nullptr;
    }
    py_indices = py_arg3; /* may be NULL -> will be checked below */
  }
  else if (py_arg2 && PySequence_Check(py_arg2)) {
    /* form: (ssbo, indices, element_count?, label?) */
    py_indices = py_arg2;
    if (py_arg3 && PyLong_Check(py_arg3)) {
      element_count = (int)PyLong_AsLong(py_arg3);
      if (PyErr_Occurred()) {
        return nullptr;
      }
    }
    else {
      /* Try to infer element_count from indices (max index + 1). */
      Py_ssize_t seq_len = PySequence_Size(py_indices);
      if (seq_len < 0) {
        return nullptr;
      }
      int maxidx = -1;
      for (Py_ssize_t i = 0; i < seq_len; ++i) {
        PyObject *it = PySequence_GetItem(py_indices, i); /* new ref */
        if (!it) {
          PyErr_Clear();
          continue;
        }
        long idx = PyLong_AsLong(it);
        Py_DECREF(it);
        if (PyErr_Occurred()) {
          PyErr_Clear();
          continue;
        }
        if (idx > maxidx) {
          maxidx = (int)idx;
        }
      }
      if (maxidx >= 0) {
        element_count = maxidx + 1;
      }
      else {
        PyErr_SetString(PyExc_ValueError,
                        "Cannot infer element_count from indices; please provide element_count");
        return nullptr;
      }
    }
  }
  else {
    PyErr_SetString(PyExc_TypeError,
                    "second argument must be an integer (element_count) or a sequence of indices");
    return nullptr;
  }

  if (!py_indices || !PySequence_Check(py_indices)) {
    PyErr_SetString(PyExc_TypeError, "indices argument must be a sequence of integers");
    return nullptr;
  }

  BPyGPUStorageBuf *bpy_sb = reinterpret_cast<BPyGPUStorageBuf *>(py_sb_obj);
  StorageBuf *sb = bpy_sb->ssbo;
  if (!sb) {
    PyErr_SetString(PyExc_ReferenceError, "Provided GPUStorageBuf has been freed");
    return nullptr;
  }

  if (element_count <= 0) {
    PyErr_SetString(PyExc_ValueError, "element_count must be > 0");
    return nullptr;
  }

  const size_t floats = size_t(element_count) * 2u;
  std::vector<float> data(floats);
  GPU_storagebuf_read(sb, data.data());

  Py_ssize_t seq_len = PySequence_Size(py_indices);
  if (seq_len < 0) {
    return nullptr; /* error */
  }

  std::ostringstream out;
  out << std::fixed;
  if (label) {
    out << "[ocean_dump_indices] " << label << ": element_count=" << element_count
        << " indices=" << static_cast<long long>(seq_len) << "\n";
  }
  else {
    out << "[ocean_dump_indices] element_count=" << element_count
        << " indices=" << static_cast<long long>(seq_len) << "\n";
  }

  for (Py_ssize_t i = 0; i < seq_len; ++i) {
    PyObject *py_idx = PySequence_GetItem(py_indices, i); /* new ref */
    if (!py_idx) {
      PyErr_Clear();
      continue;
    }
    long idx = PyLong_AsLong(py_idx);
    Py_DECREF(py_idx);
    if (PyErr_Occurred()) {
      PyErr_Clear();
      out << "  [" << static_cast<long long>(i) << "] <invalid index>\n";
      continue;
    }
    if (idx < 0 || size_t(idx) >= size_t(element_count)) {
      out << "  [" << idx << "] out-of-range (0.." << (element_count - 1) << ")\n";
      continue;
    }
    float re = data[size_t(idx) * 2u + 0];
    float im = data[size_t(idx) * 2u + 1];

    std::ostringstream val;
    val << std::setprecision(6) << std::defaultfloat << std::showpos << re << " " << std::showpos
        << im;
    out << "  [" << std::setw(4) << idx << "] (" << val.str() << " j)\n";
  }

  std::string s = out.str();
  fwrite(s.c_str(), 1, s.size(), stdout);
  fflush(stdout);

  Py_RETURN_NONE;
}

static PyObject *pygpu_ocean_debug_compare_expansion(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  PyObject *py_ssbo_obj = nullptr;
  int is_compact =
      1; /* 1 = SSBO holds compact M*(1+N/2) vec2 elements, 0 = SSBO holds full M*N vec2 */

  if (!PyArg_ParseTuple(args, "OO|p", &py_ocean_obj, &py_ssbo_obj, &is_compact)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  if (!PyObject_TypeCheck(py_ssbo_obj, &BPyGPUStorageBuf_Type)) {
    PyErr_SetString(PyExc_TypeError, "second argument must be a GPUStorageBuf Python object");
    return nullptr;
  }

  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return nullptr;
  }
  if (M <= 0 || N <= 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid M/N from BKE_ocean_export_shape");
    return nullptr;
  }

  /* CPU export (compact or full) */
  float *cpu_data = nullptr;
  int cpu_len = 0;
  if (!BKE_ocean_export_htilda_float2(o, &cpu_data, &cpu_len)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_htilda_float2 failed");
    return nullptr;
  }
  const int halfN = 1 + N / 2;
  const size_t expected_compact = size_t(M) * size_t(halfN);
  const size_t expected_full = size_t(M) * size_t(N);

  /* Build CPU-expanded full array: cpu_full[ (row*N + n) * 2 + (0=re,1=im) ] */
  std::vector<double> cpu_full;
  cpu_full.resize(expected_full * 2u, 0.0);
  if (size_t(cpu_len) == expected_full) {
    /* exporter gave full complex values already (len = M*N) */
    for (size_t i = 0; i < expected_full; ++i) {
      cpu_full[i * 2 + 0] = double(cpu_data[i * 2 + 0]);
      cpu_full[i * 2 + 1] = double(cpu_data[i * 2 + 1]);
    }
  }
  else if (size_t(cpu_len) == expected_compact) {
    /* exporter gave compact M * halfN */
    for (int row = 0; row < M; ++row) {
      for (int n = 0; n < N; ++n) {
        size_t dst_idx = size_t(row) * size_t(N) + size_t(n);
        double re = 0.0, im = 0.0;
        if (n <= N / 2) {
          size_t src_idx = size_t(row) * size_t(halfN) + size_t(n);
          re = double(cpu_data[src_idx * 2 + 0]);
          im = double(cpu_data[src_idx * 2 + 1]);
        }
        else {
          int mirror = N - n;
          size_t src_idx = size_t(row) * size_t(halfN) + size_t(mirror);
          re = double(cpu_data[src_idx * 2 + 0]);
          im = -double(cpu_data[src_idx * 2 + 1]); /* conjugate */
        }
        cpu_full[dst_idx * 2 + 0] = re;
        cpu_full[dst_idx * 2 + 1] = im;
      }
    }
  }
  else {
    BKE_ocean_free_export(cpu_data);
    PyErr_Format(PyExc_RuntimeError,
                 "Unexpected htilda export length: %d (expected compact=%zu or full=%zu)",
                 cpu_len,
                 expected_compact,
                 expected_full);
    return nullptr;
  }

  /* Read GPU SSBO content */
  BPyGPUStorageBuf *bpy_sb = reinterpret_cast<BPyGPUStorageBuf *>(py_ssbo_obj);
  StorageBuf *sb = bpy_sb->ssbo;
  if (!sb) {
    BKE_ocean_free_export(cpu_data);
    PyErr_SetString(PyExc_ReferenceError, "Provided GPUStorageBuf has been freed");
    return nullptr;
  }

  /* Decide how many complex elements to read from SSBO based on is_compact */
  size_t gpu_complex_count = is_compact ? expected_compact : expected_full;
  std::vector<float> gpu_raw;
  gpu_raw.resize(gpu_complex_count * 2u);
  GPU_storagebuf_read(sb, gpu_raw.data());

  /* Expand GPU-side if it was compact to full (apply same mirror/conjugate logic) */
  std::vector<double> gpu_full;
  gpu_full.resize(expected_full * 2u, 0.0);
  if (is_compact) {
    for (int row = 0; row < M; ++row) {
      for (int n = 0; n < N; ++n) {
        size_t dst_idx = size_t(row) * size_t(N) + size_t(n);
        double re = 0.0, im = 0.0;
        if (n <= N / 2) {
          size_t src_idx = size_t(row) * size_t(halfN) + size_t(n);
          re = double(gpu_raw[src_idx * 2 + 0]);
          im = double(gpu_raw[src_idx * 2 + 1]);
        }
        else {
          int mirror = N - n;
          size_t src_idx = size_t(row) * size_t(halfN) + size_t(mirror);
          re = double(gpu_raw[src_idx * 2 + 0]);
          im = -double(gpu_raw[src_idx * 2 + 1]);
        }
        gpu_full[dst_idx * 2 + 0] = re;
        gpu_full[dst_idx * 2 + 1] = im;
      }
    }
  }
  else {
    for (size_t i = 0; i < expected_full; ++i) {
      gpu_full[i * 2 + 0] = double(gpu_raw[i * 2 + 0]);
      gpu_full[i * 2 + 1] = double(gpu_raw[i * 2 + 1]);
    }
  }

  /* Compare CPU-expanded vs GPU-expanded */
  struct Mismatch {
    size_t idx;
    double cpu_re, cpu_im, gpu_re, gpu_im, abs_err;
  };
  std::vector<Mismatch> mismatches;
  mismatches.reserve(64);

  for (size_t i = 0; i < expected_full; ++i) {
    double c_re = cpu_full[i * 2 + 0];
    double c_im = cpu_full[i * 2 + 1];
    double g_re = gpu_full[i * 2 + 0];
    double g_im = gpu_full[i * 2 + 1];
    double abs_err = std::hypot(c_re - g_re, c_im - g_im);
    if (abs_err > 1e-6) {
      mismatches.push_back({i, c_re, c_im, g_re, g_im, abs_err});
    }
  }

  /* sort descending by error */
  std::sort(mismatches.begin(), mismatches.end(), [](const Mismatch &a, const Mismatch &b) {
    return a.abs_err > b.abs_err;
  });

  OCEAN_DBG_PRINT("[pygpu_ocean_debug_compare_expansion] M=%d N=%d (full=%zu) mismatches=%zu\n",
         M,
         N,
         expected_full,
         mismatches.size());
  size_t show = std::min<size_t>(mismatches.size(), 20);
  for (size_t i = 0; i < show; ++i) {
    size_t idx = mismatches[i].idx;
    int row = int(idx / size_t(N));
    int col = int(idx % size_t(N));
    OCEAN_DBG_PRINT(
        " [%4zu] row=%2d col=%2d abs_err=%g\n"
        "        cpu: (%+.6g, %+.6g)\n"
        "        gpu: (%+.6g, %+.6g)\n",
        idx,
        row,
        col,
        mismatches[i].abs_err,
        mismatches[i].cpu_re,
        mismatches[i].cpu_im,
        mismatches[i].gpu_re,
        mismatches[i].gpu_im);
  }
  if (mismatches.empty()) {
    OCEAN_DBG_PRINT(" All elements match (within tolerance).\n");
  }

  BKE_ocean_free_export(cpu_data);
  Py_RETURN_NONE;
}

static PyObject *pygpu_ocean_debug_compare_spatial(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  int try_factors = 1;
  if (!PyArg_ParseTuple(args, "O|p", &py_ocean_obj, &try_factors)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "GPU context is not active");
    return nullptr;
  }

  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return nullptr;
  }
  if (M <= 0 || N <= 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid M/N from BKE_ocean_export_shape");
    return nullptr;
  }

  const size_t full_count = size_t(M) * size_t(N);

  /* Try to find spatial_complex SSBO in internal cache. If missing, fall back to cached disp vec4.
   */
  StorageBuf *spatial_sb = MeshGPUCacheManager::get().ocean_internal_ssbo_get(o,
                                                                              "spatial_complex");

  bool used_disp_fallback = false;
  std::vector<float> disp_vec4;       /* vec4 per texel if fallback */
  std::vector<float> gpu_complex_raw; /* vec2 per element when reading spatial_complex */

  if (!spatial_sb) {
    /* try cached disp SSBO (vec4) */
    auto it_disp = g_ocean_ssbo_cache.find(o);
    if (it_disp != g_ocean_ssbo_cache.end()) {
      StorageBuf *disp_sb = pygpu_ocean_entry_get_ssbo(&it_disp->second);
      if (disp_sb) {
        disp_vec4.resize(full_count * 4u);
        GPU_storagebuf_read(disp_sb, disp_vec4.data());
        used_disp_fallback = true;
      }
    }
    if (!used_disp_fallback) {
      PyErr_SetString(PyExc_RuntimeError,
                      "No spatial_complex SSBO and no cached disp SSBO fallback");
      return nullptr;
    }
  }
  else {
    /* try to query length if backend supports it, else assume full_count */
    size_t complex_count = full_count;
    if (complex_count < full_count) {
      PyErr_Format(PyExc_RuntimeError,
                   "spatial_complex SSBO too small: complex_count=%zu expected>=%zu",
                   complex_count,
                   full_count);
      return nullptr;
    }
    gpu_complex_raw.resize(complex_count * 2u);
    GPU_storagebuf_read(spatial_sb, gpu_complex_raw.data());
  }

  /* Read CPU disp Y */
  float *cpu_disp = nullptr;
  int cpu_texels = 0;
  if (!BKE_ocean_export_disp_xyz(o, &cpu_disp, &cpu_texels)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_disp_xyz_threaded failed");
    return nullptr;
  }
  if (size_t(cpu_texels) != full_count) {
    BKE_ocean_free_export(cpu_disp);
    PyErr_Format(PyExc_RuntimeError,
                 "disp texel count mismatch: got %d expected %zu",
                 cpu_texels,
                 full_count);
    return nullptr;
  }

  /* CPU Y array */
  std::vector<double> cpu_y(full_count);
  for (size_t i = 0; i < full_count; ++i) {
    cpu_y[i] = double(cpu_disp[i * 3u + 1]); /* Y component */
  }

  /* Build GPU real arrays for identity and transposed layouts */
  std::vector<double> gpu_real(full_count, 0.0);
  std::vector<double> gpu_real_tr(full_count, 0.0);

  if (used_disp_fallback) {
    for (size_t i = 0; i < full_count; ++i) {
      gpu_real[i] = double(disp_vec4[i * 4u + 1]); /* Y */
    }
    for (int i = 0; i < M; ++i) {
      for (int j = 0; j < N; ++j) {
        size_t dst = size_t(i) * size_t(N) + size_t(j);
        size_t src = size_t(j) * size_t(M) + size_t(i);
        if (src < full_count) {
          gpu_real_tr[dst] = double(disp_vec4[src * 4u + 1]);
        }
      }
    }
  }
  else {
    /* gpu_complex_raw: vec2 per element [re, im] */
    for (size_t i = 0; i < full_count; ++i) {
      gpu_real[i] = double(gpu_complex_raw[i * 2u + 0]); /* real part */
    }
    for (int i = 0; i < M; ++i) {
      for (int j = 0; j < N; ++j) {
        size_t dst = size_t(i) * size_t(N) + size_t(j);
        size_t src = size_t(j) * size_t(M) + size_t(i);
        if (src < gpu_complex_raw.size() / 2u) {
          gpu_real_tr[dst] = double(gpu_complex_raw[src * 2u + 0]);
        }
      }
    }
  }

  auto compute_stats =
      [&](const std::vector<double> &g, double &out_rms, double &out_max, double &out_fopt) {
        double dot_cg = 0.0, dot_gg = 0.0, dot_cc = 0.0;
        for (size_t i = 0; i < full_count; ++i) {
          dot_cg += cpu_y[i] * g[i];
          dot_gg += g[i] * g[i];
          dot_cc += cpu_y[i] * cpu_y[i];
        }
        double f_opt = 1.0;
        if (dot_gg > 1e-18)
          f_opt = dot_cg / dot_gg;
        double sse = 0.0, maxe = 0.0;
        for (size_t i = 0; i < full_count; ++i) {
          double d = cpu_y[i] - f_opt * g[i];
          double e = fabs(d);
          if (e > maxe)
            maxe = e;
          sse += d * d;
        }
        out_rms = (full_count > 0) ? sqrt(sse / double(full_count)) : 0.0;
        out_max = maxe;
        out_fopt = f_opt;
      };

  double rms_id = 0.0, max_id = 0.0, fopt_id = 1.0;
  double rms_tr = 0.0, max_tr = 0.0, fopt_tr = 1.0;
  compute_stats(gpu_real, rms_id, max_id, fopt_id);
  compute_stats(gpu_real_tr, rms_tr, max_tr, fopt_tr);

  printf("[pygpu_ocean_debug_compare_spatial] M=%d N=%d full_count=%zu\n", M, N, full_count);
  printf(" identity: f_opt=%g rms=%g max=%g\n", fopt_id, rms_id, max_id);
  printf(" transpose: f_opt=%g rms=%g max=%g\n", fopt_tr, rms_tr, max_tr);

  /* Show top mismatches (pick best layout) */
  struct Pair {
    size_t idx;
    double cpu;
    double g;
    double err;
  };
  std::vector<Pair> diffs;
  diffs.reserve(full_count);
  bool use_transpose = (rms_tr < rms_id);
  double used_f = use_transpose ? fopt_tr : fopt_id;
  for (size_t i = 0; i < full_count; ++i) {
    double gval = use_transpose ? gpu_real_tr[i] : gpu_real[i];
    double err = fabs(cpu_y[i] - used_f * gval);
    diffs.push_back({i, cpu_y[i], gval, err});
  }
  std::sort(
      diffs.begin(), diffs.end(), [](const Pair &a, const Pair &b) { return a.err > b.err; });

  size_t show = std::min<size_t>(diffs.size(), 20);
  printf(" Top %zu mismatches (using %s layout, f_opt=%g):\n",
         show,
         use_transpose ? "transposed" : "identity",
         used_f);
  for (size_t i = 0; i < show; ++i) {
    size_t idx = diffs[i].idx;
    int row = int(idx / size_t(N));
    int col = int(idx % size_t(N));
    printf(" [%4zu] row=%2d col=%2d err=%g cpu=%g gpu=%g\n",
           idx,
           row,
           col,
           diffs[i].err,
           diffs[i].cpu,
           diffs[i].g);
  }

  BKE_ocean_free_export(cpu_disp);
  Py_RETURN_NONE;
}

static void gpu_generate_ocean_geometry_uvs_debug(void *__restrict userdata,
                                                  const int y,
                                                  const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanGeometryData *gogd = static_cast<GenerateOceanGeometryData *>(userdata);
  int x;

  for (x = 0; x < gogd->res_x; x++) {
    const int i = y * gogd->res_x + x;
    float(*luv)[2] = &gogd->uv_map[i * 4];

    // Génération des UVs
    (*luv)[0] = x * gogd->ix;
    (*luv)[1] = y * gogd->iy;
    luv++;

    (*luv)[0] = (x + 1) * gogd->ix;
    (*luv)[1] = y * gogd->iy;
    luv++;

    (*luv)[0] = (x + 1) * gogd->ix;
    (*luv)[1] = (y + 1) * gogd->iy;
    luv++;

    (*luv)[0] = x * gogd->ix;
    (*luv)[1] = (y + 1) * gogd->iy;
    luv++;

    // Ajout de logs pour déboguer les UVs
    fprintf(stderr,
            "UV[%d]: (%f, %f), (%f, %f), (%f, %f), (%f, %f)\n",
            i,
            gogd->uv_map[i * 4][0],
            gogd->uv_map[i * 4][1],
            gogd->uv_map[i * 4 + 1][0],
            gogd->uv_map[i * 4 + 1][1],
            gogd->uv_map[i * 4 + 2][0],
            gogd->uv_map[i * 4 + 2][1],
            gogd->uv_map[i * 4 + 3][0],
            gogd->uv_map[i * 4 + 3][1]);
  }
}

static PyObject *pygpu_ocean_debug_dump_ocean(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_ocean_obj)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }
  if (!o) {
    Py_RETURN_NONE;
  }

  /* lock for safe access */
  BLI_rw_mutex_lock(&o->oceanmutex, THREAD_LOCK_READ);

  printf("[pygpu_ocean_debug_dump_ocean] Ocean @%p\n", (void *)o);
  printf(" _M=%d _N=%d _Lx=%g _Lz=%g _V=%g _l=%g _A=%g _w=%g\n",
         o->_M,
         o->_N,
         o->_Lx,
         o->_Lz,
         o->_V,
         o->_l,
         o->_A,
         o->_w);
  printf(" _damp_reflections=%g _wind_alignment=%g depth=%g time=%g\n",
         o->_damp_reflections,
         o->_wind_alignment,
         o->_depth,
         o->time);
  printf(" flags: do_disp_y=%d do_chop=%d do_normals=%d do_jacobian=%d do_spray=%d\n",
         int(o->_do_disp_y),
         int(o->_do_chop),
         int(o->_do_normals),
         int(o->_do_jacobian),
         int(o->_do_spray));
  printf(" normalize_factor=%g\n", o->normalize_factor);

  /* show presence of arrays and a few sample values when available */
  auto print_array_sample = [&](const char *name, const double *arr, int count) {
    if (!arr) {
      printf("  %s: <null>\n", name);
      return;
    }
    printf("  %s: ptr=%p first3=", name, (const void *)arr);
    for (int i = 0; i < std::min(3, count); ++i) {
      printf("%g ", arr[i]);
    }
    printf(" ... last=%g\n", arr[count - 1]);
  };

  int M = o->_M;
  int N = o->_N;
  const size_t texels = size_t(M) * size_t(N);

  print_array_sample("_disp_y", o->_disp_y, (int)texels);
  print_array_sample("_disp_x", o->_disp_x, (int)texels);
  print_array_sample("_disp_z", o->_disp_z, (int)texels);
  print_array_sample("_kx", (const double *)o->_kx, o->_M ? o->_M : 0);
  print_array_sample("_kz", (const double *)o->_kz, o->_N ? o->_N : 0);

  /* htilda presence: show few complex entries */
  if (o->_htilda) {
    printf(" _htilda first few (re,im):");
    int lim = std::min<int>(5, int(size_t(o->_M) * (1 + o->_N / 2)));
    if (lim > 0) {
      for (int i = 0; i < lim; ++i) {
        printf(" (%g,%g)", (double)o->_htilda[i][0], (double)o->_htilda[i][1]);
      }
    }
    printf(" ...\n");
  }
  else {
    printf(" _htilda: <null>\n");
  }

  BLI_rw_mutex_unlock(&o->oceanmutex);

  Py_RETURN_NONE;
}

static PyObject *pygpu_ocean_debug_compare_spatial_extended(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  int try_factors = 1;
  if (!PyArg_ParseTuple(args, "O|p", &py_ocean_obj, &try_factors)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }
  if (!o) {
    Py_RETURN_NONE;
  }

  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return nullptr;
  }
  if (M <= 0 || N <= 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid M/N from BKE_ocean_export_shape");
    return nullptr;
  }

  const size_t full_count = size_t(M) * size_t(N);

  /* Get CPU disp Y */
  float *cpu_disp = nullptr;
  int cpu_texels = 0;
  if (!BKE_ocean_export_disp_xyz(o, &cpu_disp, &cpu_texels)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_disp_xyz_threaded failed");
    return nullptr;
  }
  if (size_t(cpu_texels) != full_count) {
    BKE_ocean_free_export(cpu_disp);
    PyErr_Format(PyExc_RuntimeError,
                 "disp texel count mismatch: got %d expected %zu",
                 cpu_texels,
                 full_count);
    return nullptr;
  }

  std::vector<double> cpu_y(full_count);
  for (size_t i = 0; i < full_count; ++i) {
    cpu_y[i] = double(cpu_disp[i * 3u + 1]); /* Y component */
  }

  /* Try to locate internal spatial_complex SSBO (raw) */
  StorageBuf *spatial_sb = MeshGPUCacheManager::get().ocean_internal_ssbo_get(o,
                                                                              "spatial_complex");

  std::vector<float> gpu_complex_raw; /* if we read complex vec2 raw */
  std::vector<float> disp_vec4;       /* if we fallback to cached disp vec4 */

  if (spatial_sb) {
    /* unknown length: conservatively assume full M*N complex */
    gpu_complex_raw.resize(full_count * 2u);
    GPU_storagebuf_read(spatial_sb, gpu_complex_raw.data());
  }
  else {
    /* fallback: try cached disp vec4 SSBO */
    auto it_disp = g_ocean_ssbo_cache.find(o);
    if (it_disp != g_ocean_ssbo_cache.end()) {
      StorageBuf *disp_sb = pygpu_ocean_entry_get_ssbo(&it_disp->second);
      if (disp_sb) {
        /* assume full_count */
        disp_vec4.resize(full_count * 4u);
        GPU_storagebuf_read(disp_sb, disp_vec4.data());
      }
    }
    if (disp_vec4.empty()) {
      BKE_ocean_free_export(cpu_disp);
      PyErr_SetString(PyExc_RuntimeError,
                      "No spatial_complex SSBO found and no cached disp fallback");
      return nullptr;
    }
  }

  /* Prepare gpu_real (identity) and gpu_real_tr (transposed) */
  std::vector<double> gpu_real(full_count, 0.0);
  std::vector<double> gpu_real_tr(full_count, 0.0);

  if (!disp_vec4.empty()) {
    size_t tex = disp_vec4.size() / 4u;
    size_t use_count = std::min(tex, full_count);
    for (size_t i = 0; i < use_count; ++i) {
      gpu_real[i] = double(disp_vec4[i * 4u + 1]); /* Y */
    }
    /* build transposed */
    for (int i = 0; i < M; ++i) {
      for (int j = 0; j < N; ++j) {
        size_t dst = size_t(i) * size_t(N) + size_t(j);
        size_t src = size_t(j) * size_t(M) + size_t(i);
        if (src < use_count) {
          gpu_real_tr[dst] = double(disp_vec4[src * 4u + 1]);
        }
      }
    }
  }
  else if (!gpu_complex_raw.empty()) {
    size_t complex_count = gpu_complex_raw.size() / 2u;
    /* if buffer is compact (M*(1+N/2)), expand heuristically */
    const size_t compact_expected = size_t(M) * size_t(1 + N / 2);
    if (complex_count == compact_expected) {
      /* expand compact -> full */
      for (int i = 0; i < M; ++i) {
        for (int n = 0; n < N; ++n) {
          size_t dst = size_t(i) * size_t(N) + size_t(n);
          double re = 0.0;
          if (n <= N / 2) {
            size_t src = size_t(i) * size_t(1 + N / 2) + size_t(n);
            re = double(gpu_complex_raw[src * 2u + 0]);
          }
          else {
            int mirror = N - n;
            size_t src = size_t(i) * size_t(1 + N / 2) + size_t(mirror);
            re = double(gpu_complex_raw[src * 2u + 0]);
          }
          gpu_real[dst] = re;
        }
      }
      /* build transposed */
      for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
          size_t dst = size_t(i) * size_t(N) + size_t(j);
          size_t src = size_t(j) * size_t(M) + size_t(i);
          gpu_real_tr[dst] = gpu_real[src];
        }
      }
    }
    else {
      /* assume full complex M*N */
      size_t use = std::min<size_t>(complex_count, full_count);
      for (size_t i = 0; i < use; ++i) {
        gpu_real[i] = double(gpu_complex_raw[i * 2u + 0]); /* real part */
      }
      for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
          size_t dst = size_t(i) * size_t(N) + size_t(j);
          size_t src = size_t(j) * size_t(M) + size_t(i);
          if (src < use) {
            gpu_real_tr[dst] = double(gpu_real[src]);
          }
        }
      }
    }
  }

  /* Candidate scale factors to try (including normalize_factor and M*N combos). */
  std::vector<double> candidates;
  candidates.push_back(1.0);
  if (try_factors) {
    double nf = double(o->normalize_factor);
    candidates.push_back(nf);
    candidates.push_back(1.0 / (nf == 0.0 ? 1.0 : nf));
    double mn = double(M) * double(N);
    candidates.push_back(mn);
    candidates.push_back(1.0 / (mn == 0.0 ? 1.0 : mn));
    candidates.push_back(nf * mn);
    if (mn != 0.0 && nf != 0.0) {
      candidates.push_back(1.0 / (nf * mn));
    }
  }

  auto compute_stats =
      [&](const std::vector<double> &g, double factor, double &out_rms, double &out_max) {
        double sse = 0.0, maxe = 0.0;
        for (size_t i = 0; i < full_count; ++i) {
          double d = cpu_y[i] - factor * g[i];
          double e = fabs(d);
          sse += d * d;
          if (e > maxe)
            maxe = e;
        }
        out_rms = sqrt(sse / double(full_count));
        out_max = maxe;
      };

  OCEAN_DBG_PRINT(
      "[pygpu_ocean_debug_compare_spatial_extended] M=%d N=%d count=%zu\n", M, N, full_count);
  OCEAN_DBG_PRINT("Ocean normalize_factor=%g\n", o->normalize_factor);

  /* Evaluate candidates for identity and transpose and print a matrix of results. */
  struct Result {
    double factor, rms, max;
    bool transpose;
  };
  std::vector<Result> results;
  for (double f : candidates) {
    double rms_id = 0.0, max_id = 0.0;
    double rms_tr = 0.0, max_tr = 0.0;
    compute_stats(gpu_real, f, rms_id, max_id);
    compute_stats(gpu_real_tr, f, rms_tr, max_tr);
    results.push_back({f, rms_id, max_id, false});
    results.push_back({f, rms_tr, max_tr, true});
  }

  /* Print table */
  OCEAN_DBG_PRINT(" factor\ttranspose\trms\t\tmax\n");
  for (auto &r : results) {
    OCEAN_DBG_PRINT(" %g\t%s\t%g\t%g\n", r.factor, (r.transpose ? "T" : "I"), r.rms, r.max);
  }

  /* pick best (lowest rms) */
  auto best = std::min_element(results.begin(),
                               results.end(),
                               [](const Result &a, const Result &b) { return a.rms < b.rms; });

  if (best != results.end()) {
    OCEAN_DBG_PRINT(" Best: factor=%g layout=%s rms=%g max=%g\n",
                    best->factor,
                    best->transpose ? "transposed" : "identity",
                    best->rms,
                    best->max);

    bool use_tr = best->transpose;
    double used_f = best->factor;

    /* build diffs and show top mismatches */
    struct Pair {
      size_t idx;
      double cpu;
      double g;
      double err;
    };
    std::vector<Pair> diffs;
    diffs.reserve(full_count);
    for (size_t i = 0; i < full_count; ++i) {
      double gval = use_tr ? gpu_real_tr[i] : gpu_real[i];
      double err = fabs(cpu_y[i] - used_f * gval);
      diffs.push_back({i, cpu_y[i], gval, err});
    }
    std::sort(
        diffs.begin(), diffs.end(), [](const Pair &a, const Pair &b) { return a.err > b.err; });
    size_t show = std::min<size_t>(diffs.size(), 20);
    OCEAN_DBG_PRINT(" Top %zu mismatches (using %s layout, factor=%g):\n",
                    show,
                    use_tr ? "transposed" : "identity",
                    used_f);
    for (size_t i = 0; i < show; ++i) {
      size_t idx = diffs[i].idx;
      int row = int(idx / size_t(N));
      int col = int(idx % size_t(N));
      OCEAN_DBG_PRINT(" [%4zu] row=%2d col=%2d err=%g cpu=%g gpu=%g\n",
                      idx,
                      row,
                      col,
                      diffs[i].err,
                      diffs[i].cpu,
                      diffs[i].g);
    }
  }

  BKE_ocean_free_export(cpu_disp);
  Py_RETURN_NONE;
}

static PyObject *pygpu_ocean_set_debug_prints(PyObject * /*self*/, PyObject *args)
{
  int on = 0;
  if (!PyArg_ParseTuple(args, "p", &on)) {
    return nullptr;
  }
  g_ocean_debug_prints = (on != 0);
  Py_RETURN_NONE;
}

/* -------------------------------------------------------------------- */

/* Forward declarations for debug tools */
static bool pygpu_ocean_dispatch_htilda_expand(StorageBuf *src_half,
                                               StorageBuf *dst_full,
                                               int M,
                                               int N);
static bool pygpu_ocean_dispatch_fft_rows_dft(
    StorageBuf *in_sb, StorageBuf *out_sb, Ocean *ocean, int M_val, int N_val);


static PyObject *pygpu_ocean_export_shape(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_ocean_obj)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return nullptr;
  }

  return Py_BuildValue("ii", M, N);
}

static PyObject *pygpu_ocean_ssbo_info(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_sb_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_sb_obj)) {
    return nullptr;
  }

  if (!PyObject_TypeCheck(py_sb_obj, &BPyGPUStorageBuf_Type)) {
    PyErr_SetString(PyExc_TypeError, "argument must be a GPUStorageBuf Python object");
    return nullptr;
  }

  BPyGPUStorageBuf *bpy_sb = reinterpret_cast<BPyGPUStorageBuf *>(py_sb_obj);
  StorageBuf *sb = bpy_sb->ssbo;
  if (!sb) {
    PyErr_SetString(PyExc_ReferenceError, "Provided GPUStorageBuf has been freed");
    return nullptr;
  }

  size_t byte_len = 0;
  /* Fallback: call the Python wrapper read() method which returns bytes when available.
   * This avoids depending on GPU_STORAGEBUF_HAS_LENGTH on some backends. */
  PyObject *py_bytes = PyObject_CallMethod(py_sb_obj, (char *)"read", nullptr);
  if (py_bytes && PyBytes_Check(py_bytes)) {
    Py_ssize_t size = PyBytes_Size(py_bytes);
    if (size > 0) {
      byte_len = (size_t)size;
    }
    Py_DECREF(py_bytes);
  }
  else {
    if (PyErr_Occurred()) {
      /* clear Python error, we return 0 as unknown length */
      PyErr_Clear();
    }
    /* leave byte_len == 0 */
    if (py_bytes) {
      Py_DECREF(py_bytes);
    }
  }

  /* complex_count = number of vec2 complex elements (each element = 2 floats) */
  uint64_t complex_count = 0;
  if (byte_len != 0) {
    complex_count = (uint64_t)(byte_len / (2u * sizeof(float)));
  }

  return Py_BuildValue("KK", (unsigned long long)byte_len, (unsigned long long)complex_count);
}

static PyObject *pygpu_ocean_read_ssbo_bytes(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_sb_obj = nullptr;
  PyObject *py_complex_count = nullptr;

  if (!PyArg_ParseTuple(args, "O|O", &py_sb_obj, &py_complex_count)) {
    return nullptr;
  }

  if (!PyObject_TypeCheck(py_sb_obj, &BPyGPUStorageBuf_Type)) {
    PyErr_SetString(PyExc_TypeError, "first argument must be a GPUStorageBuf Python object");
    return nullptr;
  }

  BPyGPUStorageBuf *bpy_sb = reinterpret_cast<BPyGPUStorageBuf *>(py_sb_obj);
  StorageBuf *sb = bpy_sb->ssbo;
  if (!sb) {
    PyErr_SetString(PyExc_ReferenceError, "Provided GPUStorageBuf has been freed");
    return nullptr;
  }

  uint64_t complex_count = 0;
  if (py_complex_count && PyLong_Check(py_complex_count)) {
    complex_count = (uint64_t)PyLong_AsUnsignedLongLong(py_complex_count);
    if (PyErr_Occurred()) {
      return nullptr;
    }
  }
  else {
    /* Fallback: call py_ssbo.read() and return the bytes directly. */
    PyObject *py_bytes = PyObject_CallMethod(py_sb_obj, (char *)"read", nullptr);
    if (!py_bytes) {
      if (PyErr_Occurred()) {
        /* propagate the error from the wrapper read() if any */
        return nullptr;
      }
      PyErr_SetString(PyExc_RuntimeError, "Failed to call GPUStorageBuf.read()");
      return nullptr;
    }
    if (!PyBytes_Check(py_bytes)) {
      Py_DECREF(py_bytes);
      PyErr_SetString(PyExc_RuntimeError, "GPUStorageBuf.read() did not return bytes");
      return nullptr;
    }
    /* Return the bytes object returned by the wrapper directly (no copy). */
    return py_bytes; /* owns ref */
  }

  const size_t floats = size_t(complex_count) * 2u;
  if (floats == 0) {
    return PyBytes_FromStringAndSize("", 0);
  }

  std::vector<float> data(floats);
  GPU_storagebuf_read(sb, data.data());

  /* Build Python bytes from raw float buffer (little-endian float32) */
  PyObject *py_bytes = PyBytes_FromStringAndSize(reinterpret_cast<const char *>(data.data()),
                                                 size_t(floats * sizeof(float)));
  if (!py_bytes) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create bytes object from SSBO data");
    return nullptr;
  }
  return py_bytes;
}

/* Forward declaration for debug tool */
static bool pygpu_ocean_simulate_and_export_disp_xyz_ssbo_cpp(Ocean *o,
                                                              StorageBuf *existing_ssbo,
                                                              StorageBuf **r_disp_ssbo,
                                                              int *r_tex_side,
                                                              float time,
                                                              float scale,
                                                              float chop,
                                                              float size_param);

static PyObject *pygpu_ocean_validate_cpu_vs_gpu(PyObject * /*self*/,
                                                 PyObject *args,
                                                 PyObject *kwds)
{
  PyObject *py_ocean_obj = nullptr;
  double time = 0.0;
  double scale = 1.0;
  double chop = 0.0;
  double size_param = 0.0;
  double tol = 1e-6;

  static char *kwlist[] = {
      (char *)"ocean",
      (char *)"time",
      (char *)"scale",
      (char *)"chop",
      (char *)"size",
      (char *)"tolerance",
      nullptr,
  };

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O|ddddd", kwlist, &py_ocean_obj, &time, &scale, &chop, &size_param, &tol))
  {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  OceanCapsuleContext *ctx = nullptr;
  if (PyCapsule_CheckExact(py_ocean_obj)) {
    ctx = reinterpret_cast<OceanCapsuleContext *>(PyCapsule_GetContext(py_ocean_obj));
  }
  if (time == 0.0 && ctx)
    time = ctx->time;
  if (scale == 1.0 && ctx)
    scale = ctx->scale;
  if (chop == 0.0 && ctx)
    chop = ctx->chop;
  if (size_param <= 0.0 && ctx)
    size_param = double(ctx->size) * double(ctx->spatial_size);

  /* --- CPU: simulate (skip normals) and export disp xyz --- */
  if (!o) {
    PyErr_SetString(PyExc_ValueError, "Invalid Ocean pointer");
    return nullptr;
  }

  /* run CPU simulate (explicit for clarity; simulate_and_export may also call it) */
  BKE_ocean_simulate(o, float(time), float(scale), float(chop));

  float *cpu_disp = nullptr;
  int cpu_texels = 0;
  if (!BKE_ocean_export_disp_xyz(o, &cpu_disp, &cpu_texels)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_disp_xyz_threaded failed");
    return nullptr;
  }

  if (cpu_texels <= 0) {
    BKE_ocean_free_export(cpu_disp);
    PyErr_SetString(PyExc_RuntimeError, "CPU export returned no texels");
    return nullptr;
  }

  const size_t full_count = size_t(cpu_texels);
  /* pack CPU vec3 -> vec4 (vec4 layout used by SSBOs) */
  std::vector<float> cpu_padded;
  cpu_padded.resize(full_count * 4u, 0.0f);
  for (size_t i = 0; i < full_count; ++i) {
    size_t s = i * 3u;
    size_t d = i * 4u;
    cpu_padded[d + 0] = cpu_disp[s + 0];
    cpu_padded[d + 1] = cpu_disp[s + 1];
    cpu_padded[d + 2] = cpu_disp[s + 2];
    cpu_padded[d + 3] = 0.0f;
  }

  /* --- GPU: run GPU pipeline to produce disp SSBO (vec4 per texel) --- */
  StorageBuf *out_ssbo = nullptr;
  int tex_side = 0;
  bool ok = pygpu_ocean_simulate_and_export_disp_xyz_ssbo_cpp(o,
                                                              /* existing_ssbo */ nullptr,
                                                              &out_ssbo,
                                                              &tex_side,
                                                              float(time),
                                                              float(scale),
                                                              float(chop),
                                                              float(size_param));

  if (!ok || !out_ssbo) {
    BKE_ocean_free_export(cpu_disp);
    PyErr_SetString(PyExc_RuntimeError, "GPU pipeline simulate/export failed");
    return nullptr;
  }

  /* --- Read GPU SSBO contents into cpu array --- */
  std::vector<float> gpu_padded;
  gpu_padded.resize(full_count * 4u);
  /* read SSBO raw floats */
  GPU_storagebuf_read(out_ssbo, gpu_padded.data());

  /* --- Compute metrics --- */
  double sse = 0.0;
  double sse_comp[3] = {0.0, 0.0, 0.0};
  double max_err = 0.0;
  double max_err_comp[3] = {0.0, 0.0, 0.0};
  double mean_abs = 0.0;
  size_t count_vals = full_count * 3u;

  double dot_cg = 0.0;
  double dot_gg = 0.0;

  struct Mismatch {
    size_t idx;
    float cpu[3];
    float gpu[3];
    double err;
  };
  int max_mismatches = 64;
  std::vector<Mismatch> mismatches;
  mismatches.reserve(max_mismatches);

  for (size_t i = 0; i < max_mismatches; ++i) {
    size_t s3 = i * 4u;
    float cx = cpu_padded[s3 + 0];
    float cy = cpu_padded[s3 + 1];
    float cz = cpu_padded[s3 + 2];

    float gx = gpu_padded[s3 + 0];
    float gy = gpu_padded[s3 + 1];
    float gz = gpu_padded[s3 + 2];

    double ex = double(cx - gx);
    double ey = double(cy - gy);
    double ez = double(cz - gz);

    sse_comp[0] += ex * ex;
    sse_comp[1] += ey * ey;
    sse_comp[2] += ez * ez;

    double local_err = sqrt(ex * ex + ey * ey + ez * ez);
    sse += local_err * local_err;
    if (local_err > max_err)
      max_err = local_err;

    double abs_sum = fabs(ex) + fabs(ey) + fabs(ez);
    mean_abs += abs_sum;

    /* per-component max */
    if (fabs(ex) > max_err_comp[0])
      max_err_comp[0] = fabs(ex);
    if (fabs(ey) > max_err_comp[1])
      max_err_comp[1] = fabs(ey);
    if (fabs(ez) > max_err_comp[2])
      max_err_comp[2] = fabs(ez);

    /* collect for top mismatches if above tolerance */
    if (local_err > tol) {
      Mismatch m;
      m.idx = i;
      m.cpu[0] = cx;
      m.cpu[1] = cy;
      m.cpu[2] = cz;
      m.gpu[0] = gx;
      m.gpu[1] = gy;
      m.gpu[2] = gz;
      m.err = local_err;
      mismatches.push_back(m);
    }

    /* accumulate for best-fit scale (component-wise flattened) */
    /* dot_cg = sum(cpu * gpu) over all components */
    dot_cg += double(cx) * double(gx) + double(cy) * double(gy) + double(cz) * double(gz);
    dot_gg += double(gx) * double(gx) + double(gy) * double(gy) + double(gz) * double(gz);
  }

  mean_abs = mean_abs / double(count_vals);

  double rms = (full_count > 0) ? sqrt(sse / double(full_count)) : 0.0;
  double rms_comp[3];
  for (int c = 0; c < 3; ++c) {
    rms_comp[c] = (full_count > 0) ? sqrt(sse_comp[c] / double(full_count)) : 0.0;
  }

  double f_opt = 1.0;
  if (dot_gg > 1e-18) {
    f_opt = dot_cg / dot_gg;
  }

  /* compute RMS after applying f_opt */
  double sse_scaled = 0.0;
  for (size_t i = 0; i < full_count; ++i) {
    size_t s3 = i * 4u;
    double cx = double(cpu_padded[s3 + 0]);
    double cy = double(cpu_padded[s3 + 1]);
    double cz = double(cpu_padded[s3 + 2]);
    double gx = double(gpu_padded[s3 + 0]) * f_opt;
    double gy = double(gpu_padded[s3 + 1]) * f_opt;
    double gz = double(gpu_padded[s3 + 2]) * f_opt;
    double ex = cx - gx;
    double ey = cy - gy;
    double ez = cz - gz;
    double local = ex * ex + ey * ey + ez * ez;
    sse_scaled += local;
  }
  double rms_scaled = (full_count > 0) ? sqrt(sse_scaled / double(full_count)) : 0.0;

  /* sort mismatches descending by error */
  std::sort(mismatches.begin(), mismatches.end(), [](const Mismatch &a, const Mismatch &b) {
    return a.err > b.err;
  });

  /* Print summary */
  OCEAN_DBG_PRINT(
      "[gpu.ocean.validate_cpu_vs_gpu] texels=%zu rms=%g rms_scaled=%g max_err=%g mean_abs=%g "
      "f_opt=%g\n",
      full_count,
      rms,
      rms_scaled,
      max_err,
      mean_abs,
      f_opt);
  OCEAN_DBG_PRINT(" per-component rms: x=%g y=%g z=%g  per-component max: x=%g y=%g z=%g\n",
                  rms_comp[0],
                  rms_comp[1],
                  rms_comp[2],
                  max_err_comp[0],
                  max_err_comp[1],
                  max_err_comp[2]);

  size_t show = std::min<size_t>(mismatches.size(), 20);
  if (show > 0) {
    printf(" Top %zu mismatches (idx row/col, err, cpu=(x,y,z), gpu=(x,y,z)):\n", show);
    /* Compute M/N for nicer row/col printing if shape available */
    int M = 0, N = 0;
    if (!BKE_ocean_export_shape(o, &M, &N)) {
      M = 0;
      N = 0;
    }
    for (size_t i = 0; i < show; ++i) {
      size_t idx = mismatches[i].idx;
      int row = -1, col = -1;
      if (M > 0 && N > 0) {
        row = int(idx / size_t(N));
        col = int(idx % size_t(N));
      }
      const auto &m = mismatches[i];
      if (row >= 0 && col >= 0) {
        printf("  [%4zu] row=%d col=%d err=%g cpu=(%+.6g,%+.6g,%+.6g) gpu=(%+.6g,%+.6g,%+.6g)\n",
               idx,
               row,
               col,
               m.err,
               double(m.cpu[0]),
               double(m.cpu[1]),
               double(m.cpu[2]),
               double(m.gpu[0]),
               double(m.gpu[1]),
               double(m.gpu[2]));
      }
      else {
        printf("  [%4zu] err=%g cpu=(%+.6g,%+.6g,%+.6g) gpu=(%+.6g,%+.6g,%+.6g)\n",
               idx,
               m.err,
               double(m.cpu[0]),
               double(m.cpu[1]),
               double(m.cpu[2]),
               double(m.gpu[0]),
               double(m.gpu[1]),
               double(m.gpu[2]));
      }
    }
  }
  else {
    printf(" All elements match within tolerance %g.\n", tol);
  }

  BKE_ocean_free_export(cpu_disp);
  Py_RETURN_NONE;
}

/* -------------------------------------------------------------------- */
/* Main debug helpers end                                               */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* GPU iFFT                                                             */
/* -------------------------------------------------------------------- */

/* ----------------- Begin: Prototype GPU iFFT ----------------- */

static blender::gpu::Shader *pygpu_ocean_ensure_htilda_simulate_shader()
{
  if (g_ocean_htilda_simulate_shader) {
    return g_ocean_htilda_simulate_shader;
  }
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("ocean_htilda_simulate");
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = OCEAN_HTILDA_SIMULATE_COMP_BODY_GLSL;
  info.local_group_size(256, 1, 1);

  /* bindings: h0_compact, h0_minus_compact, dst_htilda */
  info.storage_buf(0, Qualifier::read, "vec2", "h0_compact[]");
  info.storage_buf(1, Qualifier::read, "vec2", "h0_minus_compact[]");
  /* no omega[] binding any more */
  info.storage_buf(2, Qualifier::write, "vec2", "dst_htilda[]");

  /* push-constants used by GLSL body */
  info.push_constant(Type::int_t, "M");
  info.push_constant(Type::int_t, "N");
  info.push_constant(Type::int_t, "halfN");
  info.push_constant(Type::float_t, "TIME_PARAM");
  info.push_constant(Type::float_t, "SCALE_FAC");
  info.push_constant(Type::float_t, "SIZE_PARAM");

  g_ocean_htilda_simulate_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  return g_ocean_htilda_simulate_shader;
}

/* dispatch helper: assumes compact layout (M * halfN elements)
 * NOTE: omega_sb removed (omega computed in-shader); add size_param argument.
 */
static bool pygpu_ocean_dispatch_compute_htilda(
    StorageBuf *h0_sb,
    StorageBuf *h0_minus_sb,
    StorageBuf *dst_sb,
    int M_val,
    int N_val,
    int halfN_val,
    Ocean *ocean, /* NEW: pass Ocean* so we can set PH params */
    float time,
    float scale_fac,
    float size_param)
{
  if (!h0_sb || !h0_minus_sb || !dst_sb || !ocean) {
    PyErr_SetString(PyExc_ValueError, "Invalid SSBO args to dispatch_compute_htilda");
    return false;
  }
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "GPU context not active");
    return false;
  }

  blender::gpu::Shader *sh = pygpu_ocean_ensure_htilda_simulate_shader();
  if (!sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create htilda simulate shader");
    return false;
  }

  GPU_shader_bind(sh);
  GPU_storagebuf_bind(h0_sb, 0);
  GPU_storagebuf_bind(h0_minus_sb, 1);
  GPU_storagebuf_bind(dst_sb, 2);

  int loc = GPU_shader_get_uniform(sh, "M");
  if (loc != -1)
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &M_val);
  loc = GPU_shader_get_uniform(sh, "N");
  if (loc != -1)
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &N_val);
  loc = GPU_shader_get_uniform(sh, "halfN");
  if (loc != -1)
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &halfN_val);

  loc = GPU_shader_get_uniform(sh, "TIME_PARAM");
  if (loc != -1) {
    float t = time;
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &t);
  }
  loc = GPU_shader_get_uniform(sh, "SCALE_FAC");
  if (loc != -1) {
    float s = scale_fac;
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &s);
  }
  loc = GPU_shader_get_uniform(sh, "SIZE_PARAM");
  if (loc != -1) {
    float sp = size_param;
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &sp);
  }

  const uint32_t total = uint32_t(size_t(M_val) * size_t(halfN_val));
  const uint32_t groups = uint32_t((total + 256u - 1u) / 256u);
  GPU_compute_dispatch(sh, groups, 1u, 1u);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return true;
}

/* Ensure / create prep_fftin_chop shader */
static blender::gpu::Shader *pygpu_ocean_ensure_prep_fftin_chop_shader()
{
  if (g_ocean_prep_fftin_chop_shader) {
    return g_ocean_prep_fftin_chop_shader;
  }
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("ocean_prep_fftin_chop");
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = OCEAN_PREP_FFTIN_CHOP_COMP_BODY_GLSL;
  info.local_group_size(256, 1, 1);

  /* bindings: 0=htilda (vec2 read), 1=dst_x (vec2 write), 2=dst_z (vec2 write) */
  info.storage_buf(0, Qualifier::read, "vec2", "src[]");
  info.storage_buf(1, Qualifier::write, "vec2", "dst_x[]");
  info.storage_buf(2, Qualifier::write, "vec2", "dst_z[]");

  /* push-constants / uniforms used by GLSL body */
  info.push_constant(Type::int_t, "M");
  info.push_constant(Type::int_t, "N");
  info.push_constant(Type::float_t, "CHOP");
  info.push_constant(Type::float_t, "SIZE_PARAM");
  info.push_constant(Type::float_t, "SCALE_FAC");

  g_ocean_prep_fftin_chop_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  return g_ocean_prep_fftin_chop_shader;
}

/* Dispatch prep_fftin_chop:
 * src_htilda: vec2 htilda expanded
 * dst_x, dst_z: vec2 outputs (must have capacity >= elements)
 */
static bool pygpu_ocean_dispatch_prep_fftin_chop(StorageBuf *src_htilda,
                                                 StorageBuf *dst_x,
                                                 StorageBuf *dst_z,
                                                 int M_val,
                                                 int N_val,
                                                 float chop,
                                                 float size_param,
                                                 float scale_fac,
                                                 size_t elements)
{
  if (!src_htilda || !dst_x || !dst_z) {
    PyErr_SetString(PyExc_ValueError, "Invalid args to dispatch_prep_fftin_chop");
    return false;
  }
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "GPU context not active");
    return false;
  }
  if (elements == 0) {
    return true;
  }

  blender::gpu::Shader *sh = pygpu_ocean_ensure_prep_fftin_chop_shader();
  if (!sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create prep_fftin_chop shader");
    return false;
  }

  GPU_shader_bind(sh);
  GPU_storagebuf_bind(src_htilda, 0);
  GPU_storagebuf_bind(dst_x, 1);
  GPU_storagebuf_bind(dst_z, 2);

  int loc = GPU_shader_get_uniform(sh, "M");
  if (loc != -1)
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &M_val);
  loc = GPU_shader_get_uniform(sh, "N");
  if (loc != -1)
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &N_val);

  loc = GPU_shader_get_uniform(sh, "CHOP");
  if (loc != -1)
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &chop);
  loc = GPU_shader_get_uniform(sh, "SIZE_PARAM");
  if (loc != -1)
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &size_param);
  loc = GPU_shader_get_uniform(sh, "SCALE_FAC");
  if (loc != -1)
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &scale_fac);

  const uint32_t local = 256u;
  const uint32_t groups = uint32_t((elements + local - 1u) / local);
  GPU_compute_dispatch(sh, groups, 1u, 1u);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return true;
}

/* Ensure / create complex3_to_disp shader */
static blender::gpu::Shader *pygpu_ocean_ensure_complex3_to_disp_shader()
{
  if (g_ocean_complex3_to_disp_shader) {
    return g_ocean_complex3_to_disp_shader;
  }
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("ocean_complex3_to_disp");
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = OCEAN_COMPLEX3_TO_DISP_COMP_BODY_GLSL;
  info.local_group_size(256, 1, 1);

  /* bindings: 0=src_complex (vec2), 1=src_x (vec2), 2=src_z (vec2), 3=dst (vec4) */
  info.storage_buf(0, Qualifier::read, "vec2", "src_complex[]");
  info.storage_buf(1, Qualifier::read, "vec2", "src_x[]");
  info.storage_buf(2, Qualifier::read, "vec2", "src_z[]");
  info.storage_buf(3, Qualifier::write, "vec4", "dst[]");

  g_ocean_complex3_to_disp_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  return g_ocean_complex3_to_disp_shader;
}

/* Dispatch complex3 -> disp:
 * src_complex, src_x, src_z: vec2 arrays (spatial complex after iFFT)
 * dst_pos: vec4 output per texel
 */
static bool pygpu_ocean_dispatch_complexs3_to_disp(StorageBuf *src_complex,
                                                  StorageBuf *src_x,
                                                  StorageBuf *src_z,
                                                  StorageBuf *dst_pos,
                                                  size_t elements)
{
  if (!src_complex || !src_x || !src_z || !dst_pos) {
    PyErr_SetString(PyExc_ValueError, "Invalid args to dispatch_complexs_to_disp");
    return false;
  }
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "GPU context not active");
    return false;
  }
  if (elements == 0) {
    return true;
  }
  if (src_complex == dst_pos || src_x == dst_pos || src_z == dst_pos) {
    PyErr_SetString(PyExc_ValueError,
                    "src and dst SSBO cannot alias for complex->disp conversion");
    return false;
  }

  blender::gpu::Shader *sh = pygpu_ocean_ensure_complex3_to_disp_shader();
  if (!sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create complex3_to_disp shader");
    return false;
  }

  GPU_shader_bind(sh);
  GPU_storagebuf_bind(src_complex, 0);
  GPU_storagebuf_bind(src_x, 1);
  GPU_storagebuf_bind(src_z, 2);
  GPU_storagebuf_bind(dst_pos, 3);

  const uint32_t local = 256u;
  const uint32_t groups = uint32_t((elements + local - 1u) / local);
  GPU_compute_dispatch(sh, groups, 1u, 1u);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return true;
}

static blender::gpu::Shader *pygpu_ocean_ensure_fft_row_dft_shader()
{
  if (g_ocean_fft_row_dft_shader) {
    return g_ocean_fft_row_dft_shader;
  }
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("ocean_fft_row_dft");
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = OCEAN_FFT_ROW_DFT_COMP_BODY_GLSL;
  info.local_group_size(256, 1, 1);
  /* rows_in and rows_out both vec2[] */
  info.storage_buf(0, Qualifier::read, "vec2", "rows_in[]");
  info.storage_buf(1, Qualifier::write, "vec2", "rows_out[]");
  info.push_constant(Type::int_t, "M", 0);
  info.push_constant(Type::int_t, "N", 0);
  info.push_constant(Type::float_t, "SCALE_FAC");
  g_ocean_fft_row_dft_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  return g_ocean_fft_row_dft_shader;
}

static blender::gpu::Shader *pygpu_ocean_ensure_transpose_shader()
{
  if (g_ocean_transpose_shader) {
    return g_ocean_transpose_shader;
  }
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("ocean_transpose");
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = OCEAN_TRANSPOSE_COMP_BODY_GLSL;
  info.local_group_size(256, 1, 1);
  info.storage_buf(0, Qualifier::read, "vec2", "srcbuf[]");
  info.storage_buf(1, Qualifier::write, "vec2", "transposed[]");
  info.push_constant(Type::int_t, "M", 0);
  info.push_constant(Type::int_t, "N", 0);
  g_ocean_transpose_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  return g_ocean_transpose_shader;
}

static bool pygpu_ocean_dispatch_transpose(
    StorageBuf *src_sb, StorageBuf *dst_sb, Ocean *ocean, int M_val, int N_val)
{
  /* ocean param unused for now (keeps future-proof API). Silence compiler warning. */
  (void)ocean;

  blender::gpu::Shader *sh = pygpu_ocean_ensure_transpose_shader();
  if (!sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create transpose shader");
    return false;
  }
  GPU_shader_bind(sh);
  GPU_storagebuf_bind(src_sb, 0);
  GPU_storagebuf_bind(dst_sb, 1);

  int loc = GPU_shader_get_uniform(sh, "M");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &M_val);
  }
  loc = GPU_shader_get_uniform(sh, "N");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &N_val);
  }

  const uint32_t total = uint32_t(size_t(M_val) * size_t(N_val));
  const uint32_t local_size_x = 256u;
  const uint32_t groups_x = uint32_t((total + local_size_x - 1) / local_size_x);

  GPU_compute_dispatch(sh, groups_x, 1u, 1u);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return true;
}

static blender::gpu::Shader *pygpu_ocean_ensure_htilda_expand_shader()
{
  if (g_ocean_htilda_expand_shader) {
    return g_ocean_htilda_expand_shader;
  }
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("ocean_htilda_expand");
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = OCEAN_HTILDA_EXPAND_COMP_BODY_GLSL;
  info.local_group_size(256, 1, 1);
  info.storage_buf(0, Qualifier::read, "vec2", "src_half[]");
  info.storage_buf(1, Qualifier::write, "vec2", "dst_full[]");
  info.push_constant(Type::int_t, "M", 0);
  info.push_constant(Type::int_t, "N", 0);
  info.push_constant(Type::int_t, "halfN", 0);
  g_ocean_htilda_expand_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  return g_ocean_htilda_expand_shader;
}

/* Replacement: pygpu_ocean_dispatch_htilda_expand */
static bool pygpu_ocean_dispatch_htilda_expand(StorageBuf *src_half,
                                               StorageBuf *dst_full,
                                               int M_val,
                                               int N_val)
{
  blender::gpu::Shader *sh = pygpu_ocean_ensure_htilda_expand_shader();
  if (!sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create htilda expand shader");
    return false;
  }
  GPU_shader_bind(sh);
  GPU_storagebuf_bind(src_half, 0);
  GPU_storagebuf_bind(dst_full, 1);
  int loc = GPU_shader_get_uniform(sh, "M");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &M_val);
  }
  loc = GPU_shader_get_uniform(sh, "N");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &N_val);
  }
  int halfN = 1 + N_val / 2;
  loc = GPU_shader_get_uniform(sh, "halfN");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &halfN);
  }

  const uint32_t total = uint32_t(size_t(M_val) * size_t(N_val));
  const uint32_t local_size_x = 256u;
  const uint32_t groups_x = uint32_t((total + local_size_x - 1) / local_size_x);

  GPU_compute_dispatch(sh, groups_x, 1u, 1u);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return true;
}

static blender::gpu::Shader *pygpu_ocean_ensure_vec2_copy_shader()
{
  if (g_ocean_vec2_copy_shader) {
    return g_ocean_vec2_copy_shader;
  }
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("ocean_vec2_copy");
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = OCEAN_VEC2_COPY_COMP_BODY_GLSL;
  info.local_group_size(256, 1, 1);
  info.storage_buf(0, Qualifier::read, "vec2", "src_vec2[]");
  info.storage_buf(1, Qualifier::write, "vec2", "dst_vec2[]");
  g_ocean_vec2_copy_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  return g_ocean_vec2_copy_shader;
}

/* Replacement: pygpu_ocean_dispatch_fft_rows_dft */
static bool pygpu_ocean_dispatch_fft_rows_dft(
    StorageBuf *in_sb, StorageBuf *out_sb, Ocean *ocean, int M_val, int N_val)
{
  /* ocean param is currently unused here (keeps API consistent). Silence compiler warning. */
  (void)ocean;

  blender::gpu::Shader *sh = pygpu_ocean_ensure_fft_row_dft_shader();
  if (!sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create fft row shader");
    return false;
  }
  GPU_shader_bind(sh);
  GPU_storagebuf_bind(in_sb, 0);
  GPU_storagebuf_bind(out_sb, 1);

  int loc = GPU_shader_get_uniform(sh, "M");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &M_val);
  }
  loc = GPU_shader_get_uniform(sh, "N");
  if (loc != -1) {
    GPU_shader_uniform_int_ex(sh, loc, 1, 1, &N_val);
  }

  /* Always use scale factor 1.0 here. The GLSL implementations already perform per-row
   * normalization where required. Applying ocean->normalize_factor here produces double /
   * incorrect normalization (observed factor ~1/(M*N)). */
  float scale_fac = 1.0f;
  loc = GPU_shader_get_uniform(sh, "SCALE_FAC");
  if (loc != -1) {
    GPU_shader_uniform_float_ex(sh, loc, 1, 1, &scale_fac);
  }

  /* dispatch M * N work items */
  const uint32_t total = uint32_t(size_t(M_val) * size_t(N_val));
  const uint32_t local_size_x = 256u;
  const uint32_t groups_x = uint32_t((total + local_size_x - 1) / local_size_x);

  GPU_compute_dispatch(sh, groups_x, 1u, 1u);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Optional: dump DFT result when debug_dump_full or debug_dump enabled.
   * - g_ocean_debug_dump_full => write .bin file (existing behavior)
   * - g_ocean_debug_dump => print small console summary/sample (no files)
   */
  if (g_ocean_debug_dump_full || g_ocean_debug_dump) {
    try {
      const size_t total = size_t(M_val) * size_t(N_val);
      std::vector<float> dump;
      dump.resize(total * 2u);
      GPU_storagebuf_read(out_sb, dump.data());

      if (g_ocean_debug_dump_full) {
        char fname[512];
        snprintf(fname, sizeof(fname), "dft_dump_o%p_M%d_N%d.bin", (void *)ocean, M_val, N_val);
        FILE *f = fopen(fname, "wb");
        if (f) {
          fwrite(dump.data(), sizeof(float), dump.size(), f);
          fclose(f);
          OCEAN_DBG_PRINT("[dft_dump] wrote %s (%zu floats)\n", fname, dump.size());
        }
        else {
          OCEAN_DBG_PRINT("[dft_dump] failed to open %s for writing\n", fname);
        }
      }

      if (g_ocean_debug_dump) {
        const size_t show = std::min<size_t>(4, total);
        printf("[dft_dump] sample first %zu complex values (re,im):\n", show);
        for (size_t i = 0; i < show; ++i) {
          printf("  [%4zu] (%+.6g, %+.6g)\n", i, dump[i * 2u + 0], dump[i * 2u + 1]);
        }
        fflush(stdout);
      }
    }
    catch (...) {
      OCEAN_DBG_PRINT("[dft_dump] exception while dumping DFT result\n");
    }
  }

  return true;
}

/* Python wrapper: gpu.ocean.fft_rows_from_htilda(ocean_capsule) -> GPUStorageBuf (transposed
 * complex) */
static PyObject *pygpu_ocean_gpu_fft_rows(PyObject * /*self*/, PyObject *args)
{
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError,
                    "pygpu_ocean_gpu_fft_rows: GPU context is not active (cannot run GPU FFT)");
    return nullptr;
  }

  PyObject *py_ocean_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_ocean_obj)) {
    return nullptr;
  }
  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return nullptr;
  }

  float *data = nullptr;
  int len = 0;
  if (!BKE_ocean_export_htilda_float2(o, &data, &len)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_htilda_float2 failed");
    return nullptr;
  }

  const size_t complex_count = size_t(len);
  const size_t expected_full = size_t(M) * size_t(N);
  const int halfN = 1 + N / 2;
  const size_t expected_compact = size_t(M) * size_t(halfN);

  StorageBuf *rows_src = nullptr;

  // Upload/expand htilda into cached full-sized vec2 SSBO (rows_src).
  if (complex_count == expected_full) {
    const size_t complex_bytes = expected_full * 2u * sizeof(float);

    /* Directly update cached expanded SSBO instead of creating a transient raw_ssbo. */
    StorageBuf *expanded = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_HTILDA_EXPANDED, complex_bytes, "ocean_htilda_expanded");
    if (!expanded) {
      BKE_ocean_free_export(data);
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse expanded HTILDA SSBO");
      return nullptr;
    }

    GPU_storagebuf_update(expanded, data);
    BKE_ocean_free_export(data);
    data = nullptr;
    rows_src = expanded;
  }
  else if (complex_count == expected_compact) {
    const size_t compact_bytes = expected_compact * 2u * sizeof(float);

    /* Update half (compact) SSBO in-place, then expand on-GPU. */
    StorageBuf *half_vec2 = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_TEMP, compact_bytes, "ocean_htilda_half_vec2");
    if (!half_vec2) {
      BKE_ocean_free_export(data);
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse half HTILDA SSBO");
      return nullptr;
    }

    GPU_storagebuf_update(half_vec2, data);
    BKE_ocean_free_export(data);
    data = nullptr;

    const size_t full_bytes = expected_full * 2u * sizeof(float);
    StorageBuf *expanded = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_HTILDA_EXPANDED, full_bytes, "ocean_htilda_expanded");
    if (!expanded) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse expanded HTILDA SSBO");
      return nullptr;
    }

    if (!pygpu_ocean_dispatch_htilda_expand(half_vec2, expanded, M, N)) {
      PyErr_SetString(PyExc_RuntimeError, "htilda expand pass failed");
      return nullptr;
    }

    rows_src = expanded;
  }
  else {
    BKE_ocean_free_export(data);
    PyErr_Format(
        PyExc_RuntimeError,
        "htilda export size mismatch: exported len=%llu expected full=%zu or compact=%zu.",
        (unsigned long long)complex_count,
        expected_full,
        expected_compact);
    return nullptr;
  }

  const size_t full_count = expected_full;
  const size_t full_bytes2 = full_count * 2u * sizeof(float);

  /* Use cached pong buffer */
  StorageBuf *pong = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_PONG, full_bytes2, "ocean_htilda_pong");
  if (!pong) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse pong SSBO");
    return nullptr;
  }

  /* Run row-wise FFT - force naive DFT for correctness and predictability. */
  bool ok_fft = pygpu_ocean_dispatch_fft_rows_dft(rows_src, pong, o, M, N);
  if (!ok_fft) {
    PyErr_SetString(PyExc_RuntimeError, "FFT rows (DFT) pass failed");
    return nullptr;
  }

  /* Transpose -> use cached transposed buffer */
  StorageBuf *transposed_cached = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_TRANSPOSED, full_bytes2, "ocean_htilda_rows_transposed");
  if (!transposed_cached) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse transposed SSBO");
    return nullptr;
  }

  if (!pygpu_ocean_dispatch_transpose(pong, transposed_cached, o, M, N)) {
    PyErr_SetString(PyExc_RuntimeError, "Transpose pass failed");
    return nullptr;
  }

  /* Instead of allocating a transient ret buffer every time, reuse/create an internal DST buffer,
   * copy transposed_cached -> dst_internal, then return a Python wrapper for dst_internal.
   * When exposing to Python we move ownership: remove the internal cache entry for DST and
   * insert a Python wrapper into g_ocean_ssbo_cache to avoid double-free. */
  StorageBuf *dst_internal = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_DST, full_bytes2, "ocean_htilda_rows_transposed_dst");
  if (!dst_internal) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse dst SSBO");
    return nullptr;
  }

  /* Copy cached -> dst_internal with vec2 copy shader */
  blender::gpu::Shader *copy_sh = pygpu_ocean_ensure_vec2_copy_shader();
  if (!copy_sh) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to create vec2 copy shader");
    return nullptr;
  }

  GPU_shader_bind(copy_sh);
  GPU_storagebuf_bind(transposed_cached, 0);
  GPU_storagebuf_bind(dst_internal, 1);
  const uint32_t local = 256u;
  const uint32_t groups = uint32_t((full_count + local - 1u) / local);
  GPU_compute_dispatch(copy_sh, groups, 1u, 1u);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Prepare Python wrapper for dst_internal and move it from internal cache -> python cache */
  PyObject *py_sb = nullptr;

  /* If a cached python wrapper already exists and points to dst_internal, return it. */
  auto it_cache = g_ocean_ssbo_cache.find(o);
  if (it_cache != g_ocean_ssbo_cache.end()) {
    StorageBuf *cached = pygpu_ocean_entry_get_ssbo(&it_cache->second);
    if (cached == dst_internal && it_cache->second.capacity >= full_bytes2) {
      Py_INCREF(it_cache->second.py_ssbo);
      py_sb = it_cache->second.py_ssbo;
    }
  }

  if (!py_sb) {
    PyObject *created = BPyGPUStorageBuf_CreatePyObject(
        reinterpret_cast<StorageBuf *>(dst_internal));
    if (!created) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to wrap dst SSBO");
      return nullptr;
    }
    MeshGPUCacheManager::get().ocean_internal_ssbo_detach(o, std::string("dst"));

    auto it_existing = g_ocean_ssbo_cache.find(o);
    if (it_existing != g_ocean_ssbo_cache.end()) {
      pygpu_ocean_evict_cache_entry(g_ocean_ssbo_cache, it_existing);
    }

    SSBOCacheEntry new_entry;
    new_entry.py_ssbo = created; /* owns ref */
    new_entry.capacity = full_bytes2;
    auto insert_res = g_ocean_ssbo_cache.emplace(o, std::move(new_entry));
    if (!insert_res.second) {
      /* insertion fail: DECREF wrapper et remonter erreur */
      Py_DECREF(created);
      PyErr_SetString(PyExc_RuntimeError, "Failed to insert SSBO into ocean python cache");
      return nullptr;
    }

    Py_INCREF(insert_res.first->second.py_ssbo);
    py_sb = insert_res.first->second.py_ssbo;
  }

  return py_sb;
}

/* ----------------- END: Prototype GPU iFFT ----------------- */

/* -------------------------------------------------------------------- */
/* GPU iFFT End                                                         */
/* -------------------------------------------------------------------- */


/* -------------------------------------------------------------------- */
/* Simulation To Mesh                                                   */
/* -------------------------------------------------------------------- */

/* Dispatch final ocean displacement shader:
 * - disp_sb: input MxN vec3 SSBO (displacement map)
 * - base_sb: input Vx2 vec2 SSBO (base mesh vertex positions)
 * - size_param: ocean size parameter
 * - height_scale: vertical displacement scale
 * - modifies evaluated mesh vertex positions and normals in-place
 */
static GpuComputeStatus pygpu_ocean_dispatch_final_shader(Ocean *ocean,
                                                          Depsgraph *depsgraph,
                                                          Object *ob_eval,
                                                          StorageBuf *disp_sb,
                                                          StorageBuf *base_sb,
                                                          float size_param,
                                                          float height_scale)
{
  using namespace blender::bke;
  using namespace blender::gpu::shader;

  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "GPU context not active");
    return GpuComputeStatus::Error;
  }
  if (!depsgraph || !ob_eval) {
    PyErr_SetString(PyExc_ValueError, "Invalid depsgraph or object");
    return GpuComputeStatus::Error;
  }

  /* Build the GLSL compute body (will be concatenated with topology accessors by
   * BKE_mesh_gpu_run_compute). This is similar to your Python COMPUTE_SRC but simplified
   * for mesh compute: it expects the topology accessors to exist. */
  const std::string main_glsl = R"GLSL(
/* helpers for normals packing (keep same packing used before) */
int pack_i10_trunc(float x) { return clamp(int(x * 511.0), -512, 511) & 0x3FF; }
uint pack_norm(vec3 n) { return uint(pack_i10_trunc(n.x)) | (uint(pack_i10_trunc(n.y)) << 10) | (uint(pack_i10_trunc(n.z)) << 20); }

vec3 newell_face_normal_object(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  vec3 n = vec3(0.0);
  vec3 v_prev = positions_out[end - 1].xyz;
  for (int i = beg; i < end; ++i) {
    vec3 v_curr = positions_out[i].xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }
  return normalize(n);
}

vec3 transform_normal(vec3 n, mat4 m) { return transpose(inverse(mat3(m))) * n; }

/* positive modulo helper */
int mod_pos(int a, int b) {
  int r = a % b;
  return (r < 0) ? r + b : r;
}

/* BILERP using SSBO 'disp' laid out as (i * N + j) */
vec3 bilerp_disp(int i0, int j0, float fx, float fz) {
  int i1 = i0 + 1;
  int j1 = j0 + 1;

  /* wrap indices like CPU: ensure 0 <= index < M/N */
  int ii0 = mod_pos(i0, M);
  int ii1 = mod_pos(i1, M);
  int jj0 = mod_pos(j0, N);
  int jj1 = mod_pos(j1, N);

  int idx00 = ii0 * N + jj0;
  int idx10 = ii1 * N + jj0;
  int idx01 = ii0 * N + jj1;
  int idx11 = ii1 * N + jj1;

  vec3 c00 = disp_pos_in[idx00].xyz;
  vec3 c10 = disp_pos_in[idx10].xyz;
  vec3 c01 = disp_pos_in[idx01].xyz;
  vec3 c11 = disp_pos_in[idx11].xyz;

  vec3 lx0 = mix(c00, c10, fx);
  vec3 lx1 = mix(c01, c11, fx);
  return mix(lx0, lx1, fz);
}

void main() {
  uint c = gl_GlobalInvocationID.x;
  if (c >= positions_out.length()) return;

  int vert_idx = corner_verts(int(c));
  vec2 bp = base_pos_in[vert_idx].xy;
  float vx = bp.x;
  float vy = bp.y;

  float inv_size = (size_param != 0.0) ? (1.0 / size_param) : 1.0;
  float u = vx * inv_size + 0.5;
  float v = vy * inv_size + 0.5;

  float uu = u * float(M);
  float vv = v * float(N);
  int i0 = int(floor(uu));
  int j0 = int(floor(vv));
  float fx = uu - float(i0);
  float fz = vv - float(j0);

  vec3 d = bilerp_disp(i0, j0, fx, fz);
  vec3 pos_local = vec3(vx + d.x, vy + d.z, d.y);
  positions_out[c] = vec4(pos_local, 1.0);

  vec3 n_obj;
  if (normals_domain == 1) {
    int f = corner_to_face(int(c));
    n_obj = newell_face_normal_object(f);
  }
  else {
    int beg = vert_to_face_offsets(vert_idx);
    int end = vert_to_face_offsets(vert_idx + 1);
    vec3 acc = vec3(0.0);
    for (int i = beg; i < end; ++i) {
      acc += newell_face_normal_object(vert_to_face(i));
    }
    n_obj = (end > beg) ? normalize(acc / float(end - beg)) : vec3(0.0, 0.0, 1.0);
  }
  mat4 transform_mat = mat4(1.0);
  vec3 n_world = transform_normal(n_obj, transform_mat);
  normals_out[int(c)] = pack_norm(n_world);
}
)GLSL";

  Mesh *me = static_cast<Mesh *>(ob_eval->data);
  if (!me) {
    PyErr_SetString(PyExc_RuntimeError, "Object has no mesh data");
    return GpuComputeStatus::Error;
  }
  using namespace blender::draw;
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(me->runtime->batch_cache);
  if (!cache) {
    return GpuComputeStatus::NotReady;
  }
  blender::gpu::VertBufPtr *vbo_pos_ptr = cache->final.buff.vbos.lookup_ptr(VBOType::Position);
  if (!vbo_pos_ptr) {
    return GpuComputeStatus::NotReady;
  }
  blender::gpu::VertBuf *vbo_pos = cache->final.buff.vbos.lookup(VBOType::Position).get(); /* positions */
  blender::gpu::VertBuf *vbo_nor = cache->final.buff.vbos.lookup(VBOType::CornerNormal).get(); /* normals */
  if (!vbo_pos || !vbo_nor) {
    return GpuComputeStatus::NotReady;
  }

  const GPUVertFormat *fmt = GPU_vertbuf_get_format(vbo_pos);
  if (!fmt || fmt->stride != 16) {
    if (Mesh *orig_me = BKE_object_get_original_mesh(ob_eval)) {
      orig_me->is_running_gpu_animation_playback = 1;
      me->is_running_gpu_animation_playback = 1;
      DEG_id_tag_update(&DEG_get_original(ob_eval)->id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_WINDOW, nullptr);
      return GpuComputeStatus::NotReady;
    }
  }

  int M_val = ocean->_M;
  int N_val = ocean->_N;
  /* Build caller bindings vector.
   * Type GpuMeshComputeBinding is provided by BKE_mesh_gpu.hh and contains:
   *   int binding;
   *   blender::gpu::shader::Qualifier qualifiers;
   *   const char *type_name;
   *   const char *bind_name;
   *   std::variant<...> buffer; (StorageBuf* accepted)
   *
   * We create bindings for:
   *   0: positions_out (write vec4)
   *   1: normals_out   (write uint)
   *   2: disp_pos_in   (read vec4)
   *   3: base_pos_in   (read vec4)
   */
  std::vector<blender::bke::GpuMeshComputeBinding> caller_bindings;
  caller_bindings.reserve(4);

  {
    blender::bke::GpuMeshComputeBinding b = {};
    b.binding = 0;
    b.qualifiers = blender::gpu::shader::Qualifier::read_write;
    b.type_name = "vec4";
    b.bind_name = "positions_out[]";
    b.buffer = vbo_pos;
    caller_bindings.push_back(b);
  }
  {
    blender::bke::GpuMeshComputeBinding b = {};
    b.binding = 1;
    b.qualifiers = blender::gpu::shader::Qualifier::write;
    b.type_name = "uint";
    b.bind_name = "normals_out[]";
    b.buffer = vbo_nor;
    caller_bindings.push_back(b);
  }
  {
    blender::bke::GpuMeshComputeBinding b = {};
    b.binding = 2;
    b.qualifiers = blender::gpu::shader::Qualifier::read;
    b.type_name = "vec4";
    b.bind_name = "disp_pos_in[]";
    b.buffer = disp_sb;
    caller_bindings.push_back(b);
  }
  {
    blender::bke::GpuMeshComputeBinding b = {};
    b.binding = 3;
    b.qualifiers = blender::gpu::shader::Qualifier::read;
    b.type_name = "vec4";
    b.bind_name = "base_pos_in[]";
    b.buffer = base_sb;
    caller_bindings.push_back(b);
  }

  auto post_bind_fn = [&](blender::gpu::Shader *sh) {
    int loc = GPU_shader_get_uniform(sh, "M");
    if (loc != -1) {
      GPU_shader_uniform_int_ex(sh, loc, 1, 1, &M_val);
    }
    loc = GPU_shader_get_uniform(sh, "N");
    if (loc != -1) {
      GPU_shader_uniform_int_ex(sh, loc, 1, 1, &N_val);
    }
    loc = GPU_shader_get_uniform(sh, "size_param");
    float sp = size_param;
    if (loc != -1) {
      GPU_shader_uniform_float_ex(sh, loc, 1, 1, &sp);
    }
  };

  /* Build config function to add push-constants (size_param, tex_side, inv_tex_side, HEIGHT_SCALE)
   */
  auto config_fn = [height_scale, size_param](blender::gpu::shader::ShaderCreateInfo &info) {
    info.push_constant(Type::int_t, "M", 0);
    info.push_constant(Type::int_t, "N", 0);
    info.specialization_constant(Type::float_t, "HEIGHT_SCALE", height_scale);
    info.specialization_constant(Type::float_t, "size_param", size_param);
  };

  /* Call BKE_mesh_gpu_run_compute which will take care of topology binding and shader caching */
  blender::bke::GpuComputeStatus status = BKE_mesh_gpu_run_compute(
      depsgraph,
      ob_eval,
      main_glsl.c_str(),
      blender::Span<blender::bke::GpuMeshComputeBinding>(caller_bindings),
      config_fn,
      post_bind_fn,
      me->corners_num);

  if (status == blender::bke::GpuComputeStatus::Error) {
    PyErr_SetString(PyExc_RuntimeError,
                    "BKE_mesh_gpu_run_compute failed to dispatch final compute");
    return GpuComputeStatus::Error;
  }

  return GpuComputeStatus::Success;
}

/* -------------------------------------------------------------------- */
/* Simulation To Mesh End                                               */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Main Simulation function                                             */
/* -------------------------------------------------------------------- */

static bool pygpu_ocean_simulate_and_export_disp_xyz_ssbo_cpp(Ocean *o,
                                                              StorageBuf *existing_ssbo,
                                                              StorageBuf **r_disp_ssbo,
                                                              int *r_tex_side,
                                                              float time,
                                                              float scale,
                                                              float chop,
                                                              float size_param)
{
  if (!o || !r_disp_ssbo || !r_tex_side) {
    PyErr_SetString(PyExc_ValueError, "Invalid arguments");
    return false;
  }
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "GPU context not active");
    return false;
  }

  /* Determine spectral shape early */
  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return false;
  }
  if (M <= 0 || N <= 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid spectral shape (M/N)");
    return false;
  }

  const int halfN = 1 + N / 2;
  const size_t expected_full = size_t(M) * size_t(N);
  const size_t expected_compact = size_t(M) * size_t(halfN);
  const size_t full_bytes2 = expected_full * 2u * sizeof(float);

  StorageBuf *rows_src = nullptr;

  if (g_ocean_enable_cpu_simulate) {
    /* Original CPU simulate path */
    BKE_ocean_simulate(o, time, scale, chop);

    float *htilda_data = nullptr;
    int htilda_len = 0;
    if (!BKE_ocean_export_htilda_float2(o, &htilda_data, &htilda_len)) {
      PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_htilda_float2 failed");
      return false;
    }

    /* --- Scale exported htilda to match CPU 'fft_in' (CPU multiplies by normalize_factor). */
    {
      const float htilda_scale = float(scale * float(o->normalize_factor));
      if (htilda_scale != 1.0f) {
        const size_t count = size_t(htilda_len); /* complex element count */
        for (size_t ii = 0; ii < count; ++ii) {
          htilda_data[ii * 2u + 0] *= htilda_scale;
          htilda_data[ii * 2u + 1] *= htilda_scale;
        }
      }
    }

    /* Upload/expand htilda into cached full-sized vec2 SSBO (rows_src). */
    if (size_t(htilda_len) == expected_full) {
      StorageBuf *expanded = pygpu_ocean_get_or_create_internal_ssbo(
          o, SSBO_ROLE_HTILDA_EXPANDED, full_bytes2, "ocean_htilda_expanded");
      if (!expanded) {
        BKE_ocean_free_export(htilda_data);
        PyErr_SetString(PyExc_RuntimeError, "Failed to allocate internal expanded HTILDA SSBO");
        return false;
      }

      GPU_storagebuf_update(expanded, htilda_data);
      BKE_ocean_free_export(htilda_data);
      htilda_data = nullptr;
      rows_src = expanded;
    }
    else if (size_t(htilda_len) == expected_compact) {
      const size_t compact_bytes = expected_compact * 2u * sizeof(float);
      StorageBuf *half_vec2 = pygpu_ocean_get_or_create_internal_ssbo(
          o, SSBO_ROLE_TEMP, compact_bytes, "ocean_htilda_half_vec2");
      if (!half_vec2) {
        BKE_ocean_free_export(htilda_data);
        PyErr_SetString(PyExc_RuntimeError, "Failed to allocate internal half vec2 SSBO");
        return false;
      }

      GPU_storagebuf_update(half_vec2, htilda_data);
      BKE_ocean_free_export(htilda_data);
      htilda_data = nullptr;

      StorageBuf *expanded = pygpu_ocean_get_or_create_internal_ssbo(
          o, SSBO_ROLE_HTILDA_EXPANDED, full_bytes2, "ocean_htilda_expanded");
      if (!expanded) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse expanded HTILDA SSBO");
        return false;
      }

      if (!pygpu_ocean_dispatch_htilda_expand(half_vec2, expanded, M, N)) {
        PyErr_SetString(PyExc_RuntimeError, "htilda expand failed");
        return false;
      }

      rows_src = expanded;
    }
    else {
      BKE_ocean_free_export(htilda_data);
      PyErr_Format(PyExc_RuntimeError,
                   "htilda export size mismatch: len=%d expected compact=%zu or full=%zu",
                   htilda_len,
                   expected_compact,
                   expected_full);
      return false;
    }
  }
  else {
    /* GPU simulate path: build (or reuse) compact h0 / h0_minus SSBOs.
     * Convert ONCE from double->float and cache by pointer address to avoid repeated CPU work. */
    if (!o->_h0 || !o->_h0_minus) {
      PyErr_SetString(PyExc_RuntimeError, "Ocean missing _h0 / _h0_minus data for GPU simulate");
      return false;
    }

    const size_t compact_elems = expected_compact;
    const size_t compact_bytes = compact_elems * 2u * sizeof(float);

    /* Ensure destination SSBOs exist (cached internal SSBOs). */
    StorageBuf *h0_sb = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_H0_COMPACT, compact_bytes, "ocean_h0_compact");
    if (!h0_sb) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse internal h0_compact SSBO");
      return false;
    }
    StorageBuf *h0m_sb = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_H0M_COMPACT, compact_bytes, "ocean_h0m_compact");
    if (!h0m_sb) {
      PyErr_SetString(PyExc_RuntimeError,
                      "Failed to allocate/reuse internal h0_minus_compact SSBO");
      return false;
    }

    /* Check if we already converted & uploaded for this Ocean instance (pointer-stable). */
    const void *cur_h0_ptr = reinterpret_cast<const void *>(o->_h0);
    const void *cur_h0m_ptr = reinterpret_cast<const void *>(o->_h0_minus);
    bool need_upload = true;
    auto it0 = g_ocean_h0_last_ptr.find(o);
    auto it1 = g_ocean_h0m_last_ptr.find(o);
    if (it0 != g_ocean_h0_last_ptr.end() && it1 != g_ocean_h0m_last_ptr.end()) {
      if (it0->second == cur_h0_ptr && it1->second == cur_h0m_ptr) {
        /* pointers unchanged -> assume SSBOs already contain converted floats */
        need_upload = false;
      }
    }

    if (need_upload) {
      /* allocate temp float vectors once for conversion */
      std::vector<float> h0_compact;
      std::vector<float> h0m_compact;
      h0_compact.resize(compact_elems * 2u);
      h0m_compact.resize(compact_elems * 2u);

      /* Convert double -> float in parallel (same logic as before) */
      BLI_rw_mutex_lock(&o->oceanmutex, THREAD_LOCK_READ);
      {
        const double(*h0d)[2] = reinterpret_cast<const double(*)[2]>(o->_h0);
        const double(*h0md)[2] = reinterpret_cast<const double(*)[2]>(o->_h0_minus);

        blender::threading::parallel_for(
            blender::IndexRange(M), 32, [&](const blender::IndexRange range) {
              for (int i : range) {
                const size_t row_src = size_t(i) * size_t(o->_N);
                const size_t row_dst = size_t(i) * size_t(halfN);
                for (int j = 0; j < halfN; ++j) {
                  size_t dst = row_dst + size_t(j);
                  size_t src_idx = row_src + size_t(j);
                  double re0 = h0d[src_idx][0];
                  double im0 = h0d[src_idx][1];
                  double re1 = h0md[src_idx][0];
                  double im1 = h0md[src_idx][1];
                  h0_compact[dst * 2u + 0] = float(re0);
                  h0_compact[dst * 2u + 1] = float(im0);
                  h0m_compact[dst * 2u + 0] = float(re1);
                  h0m_compact[dst * 2u + 1] = float(im1);
                }
              }
            });
      }
      BLI_rw_mutex_unlock(&o->oceanmutex);

      /* Upload once to internal SSBOs */
      GPU_storagebuf_update(h0_sb, h0_compact.data());
      GPU_storagebuf_update(h0m_sb, h0m_compact.data());

      /* update cache pointers */
      g_ocean_h0_last_ptr[o] = cur_h0_ptr;
      g_ocean_h0m_last_ptr[o] = cur_h0m_ptr;
    }
    /* else: reuse existing h0_sb / h0m_sb without reupload */

    /* Destination compact htilda (use an internal cached SSBO to avoid leak) */
    StorageBuf *dst_compact = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_TEMP, compact_bytes, "ocean_htilda_simulated_compact");
    if (!dst_compact) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate internal dst_compact SSBO");
      return false;
    }

    /* scale on GPU: match CPU behavior where scale multiplied by o->normalize_factor earlier */
    const float htilda_scale = float(scale * float(o->normalize_factor));

    /* Dispatch compute shader that computes htilda and omega internally,
     * using the (cached) h0_sb and h0m_sb prepared above. */
    if (!pygpu_ocean_dispatch_compute_htilda(
            h0_sb, h0m_sb, dst_compact, M, N, halfN, o, time, htilda_scale, size_param))
    {
      PyErr_SetString(PyExc_RuntimeError, "GPU htilda simulate dispatch failed");
      return false;
    }

    /* Expand compact -> full on GPU (use cached expanded buffer) */
    StorageBuf *expanded = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_HTILDA_EXPANDED, full_bytes2, "ocean_htilda_expanded");
    if (!expanded) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse expanded HTILDA SSBO");
      return false;
    }

    if (!pygpu_ocean_dispatch_htilda_expand(dst_compact, expanded, M, N)) {
      PyErr_SetString(PyExc_RuntimeError, "htilda expand (after simulate) failed");
      return false;
    }

    rows_src = expanded;
  }

  if (!rows_src) {
    PyErr_SetString(PyExc_RuntimeError, "Internal error: rows_src not prepared");
    return false;
  }

  /* Row-wise FFT -> use cached pong (rows_src must be valid by here)
   * Always use naive DFT: simpler, robust and sufficient for our target resolutions. */
  StorageBuf *pong = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_PONG, full_bytes2, "ocean_htilda_pong");
  if (!pong) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse pong SSBO");
    return false;
  }

  /* Force naive DFT for row-wise pass for stability/perf at our resolutions. */
  bool ok_fft = pygpu_ocean_dispatch_fft_rows_dft(rows_src, pong, o, M, N);
  if (!ok_fft) {
    PyErr_SetString(PyExc_RuntimeError, "FFT rows (DFT) pass failed");
    return false;
  }

  /* Transpose -> cached transposed */
  StorageBuf *transposed = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_TRANSPOSED, full_bytes2, "ocean_htilda_rows_transposed");
  if (!transposed) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse transposed SSBO");
    return false;
  }

  if (!pygpu_ocean_dispatch_transpose(pong, transposed, o, M, N)) {
    PyErr_SetString(PyExc_RuntimeError, "Transpose pass failed");
    return false;
  }

  /* Column-wise FFT */
  StorageBuf *pong2 = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_PONG2, full_bytes2, "ocean_htilda_pong2");
  if (!pong2) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse pong2 SSBO");
    return false;
  }

  bool ok_second_fft = false;
  ok_second_fft = pygpu_ocean_dispatch_fft_rows_dft(transposed, pong2, o, N, M);

  if (!ok_second_fft) {
    PyErr_SetString(PyExc_RuntimeError, "Second FFT rows pass failed");
    return false;
  }

  /* Transpose back -> spatial_complex (Y component) */
  StorageBuf *spatial_complex = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_SPATIAL_COMPLEX, full_bytes2, "ocean_spatial_complex");
  if (!spatial_complex) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse spatial_complex SSBO");
    return false;
  }

  if (!pygpu_ocean_dispatch_transpose(pong2, spatial_complex, o, N, M)) {
    PyErr_SetString(PyExc_RuntimeError, "Transpose back failed");
    return false;
  }

  /* Use dedicated roles for fft_in_x / fft_in_z to avoid reuse of general TEMP/ROTATED buffers. */
  StorageBuf *fft_in_x = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_FFT_IN_X, full_bytes2, "ocean_fft_in_x");
  if (!fft_in_x) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse fft_in_x SSBO");
    return false;
  }
  StorageBuf *fft_in_z = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_FFT_IN_Z, full_bytes2, "ocean_fft_in_z");
  if (!fft_in_z) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse fft_in_z SSBO");
    return false;
  }

  /* When we simulated on GPU above we already applied htilda_scale; therefore pass SCALE_FAC=1.0
   * into the prep shader to avoid double-scaling. */
  const float prep_scale_fac = 1.0f;
  if (!pygpu_ocean_dispatch_prep_fftin_chop(rows_src,
                                            fft_in_x,
                                            fft_in_z,
                                            M,
                                            N,
                                            float(chop),
                                            float(size_param),
                                            prep_scale_fac,
                                            size_t(expected_full)))
  {
    PyErr_SetString(PyExc_RuntimeError, "prep_fftin_chop dispatch failed");
    return false;
  }

  /* Continue pipeline: run full FFT on fft_in_x -> spatial_complex_x and fft_in_z ->
   * spatial_complex_z. Implementation below unchanged from previous flow. */
  auto run_full_fft_pipeline =
      [&](StorageBuf *in_rows, StorageBuf *spatial_out, const char *name) {
        bool ok1 = false;
        ok1 = pygpu_ocean_dispatch_fft_rows_dft(in_rows, pong, o, M, N);
        if (!ok1) {
          std::string err = std::string("Row FFT pass failed (") + name + ")";
          PyErr_SetString(PyExc_RuntimeError, err.c_str());
          return false;
        }

        if (!pygpu_ocean_dispatch_transpose(pong, transposed, o, M, N)) {
          std::string err = std::string("Transpose pass failed (") + name + ")";
          PyErr_SetString(PyExc_RuntimeError, err.c_str());
          return false;
        }

        bool ok2 = false;
        ok2 = pygpu_ocean_dispatch_fft_rows_dft(transposed, pong2, o, N, M);
        if (!ok2) {
          std::string err = std::string("Second FFT pass failed (") + name + ")";
          PyErr_SetString(PyExc_RuntimeError, err.c_str());
          return false;
        }

        if (!pygpu_ocean_dispatch_transpose(pong2, spatial_out, o, N, M)) {
          std::string err = std::string("Transpose back failed (") + name + ")";
          PyErr_SetString(PyExc_RuntimeError, err.c_str());
          return false;
        }

        return true;
      };

  /* Use dedicated roles for spatial_complex_x / spatial_complex_z to avoid
   * overwriting other temp buffers used earlier in the pipeline. */
  StorageBuf *spatial_complex_x = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_SPATIAL_COMPLEX_X, full_bytes2, "ocean_spatial_complex_x");
  if (!spatial_complex_x) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate spatial_complex_x SSBO");
    return false;
  }

  StorageBuf *spatial_complex_z = pygpu_ocean_get_or_create_internal_ssbo(
      o, SSBO_ROLE_SPATIAL_COMPLEX_Z, full_bytes2, "ocean_spatial_complex_z");
  if (!spatial_complex_z) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate spatial_complex_z SSBO");
    return false;
  }

  if (!run_full_fft_pipeline(fft_in_x, spatial_complex_x, "fft_in_x")) {
    return false;
  }
  if (!run_full_fft_pipeline(fft_in_z, spatial_complex_z, "fft_in_z")) {
    return false;
  }

  /* Destination (vec4 positions) */
  const size_t padded_bytes = expected_full * 4u * sizeof(float); /* vec4 per element */
  StorageBuf *dst_ssbo = existing_ssbo;
  SSBOCacheEntry *disp_entry = pygpu_ocean_get_or_create_cached_ssbo_entry(
      o, padded_bytes, "ocean_disp_xyz");
  if (disp_entry && disp_entry->py_ssbo) {
    StorageBuf *cached_dst = pygpu_ocean_entry_get_ssbo(disp_entry);
    if (cached_dst) {
      dst_ssbo = cached_dst;
    }
  }
  bool created_transient_dst = false;
  if (!dst_ssbo) {
    /* Use internal cached DST SSBO (will be wrapped for Python on return if needed). */
    dst_ssbo = pygpu_ocean_get_or_create_internal_ssbo(
        o, SSBO_ROLE_DST, padded_bytes, "ocean_disp_xyz_internal");
    if (!dst_ssbo) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate/reuse internal dst SSBO");
      return false;
    }
    created_transient_dst = false; /* internal cached ownership retained by internal cache */
  }

  /* Convert complex -> vec4 positions (X,Y,Z) on GPU */
  {
    StorageBuf *src_complex_y = spatial_complex;
    StorageBuf *src_complex_x = spatial_complex_x;
    StorageBuf *src_complex_z = spatial_complex_z;

    /* Guard against aliasing with dst; copy to temp if needed */
    if (src_complex_y == dst_ssbo || src_complex_x == dst_ssbo || src_complex_z == dst_ssbo) {
      StorageBuf *temp_copy = pygpu_ocean_get_or_create_internal_ssbo(
          o, SSBO_ROLE_TEMP, full_bytes2, "ocean_complex_to_disp_temp");
      if (!temp_copy) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to allocate temp SSBO for complex->disp");
        if (created_transient_dst && dst_ssbo) {
          GPU_storagebuf_free(dst_ssbo);
        }
        return false;
      }
      blender::gpu::Shader *copy_sh = pygpu_ocean_ensure_vec2_copy_shader();
      if (!copy_sh) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create vec2 copy shader");
        if (created_transient_dst && dst_ssbo) {
          GPU_storagebuf_free(dst_ssbo);
        }
        return false;
      }
      if (src_complex_y == dst_ssbo) {
        GPU_shader_bind(copy_sh);
        GPU_storagebuf_bind(src_complex_y, 0);
        GPU_storagebuf_bind(temp_copy, 1);
        const uint32_t groups_copy = uint32_t((expected_full + 256u - 1u) / 256u);
        GPU_compute_dispatch(copy_sh, groups_copy, 1u, 1u);
        GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
        GPU_shader_unbind();
        src_complex_y = temp_copy;
      }
      if (src_complex_x == dst_ssbo) {
        GPU_shader_bind(copy_sh);
        GPU_storagebuf_bind(src_complex_x, 0);
        GPU_storagebuf_bind(temp_copy, 1);
        const uint32_t groups_copy = uint32_t((expected_full + 256u - 1u) / 256u);
        GPU_compute_dispatch(copy_sh, groups_copy, 1u, 1u);
        GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
        GPU_shader_unbind();
        src_complex_x = temp_copy;
      }
      if (src_complex_z == dst_ssbo) {
        GPU_shader_bind(copy_sh);
        GPU_storagebuf_bind(src_complex_z, 0);
        GPU_storagebuf_bind(temp_copy, 1);
        const uint32_t groups_copy = uint32_t((expected_full + 256u - 1u) / 256u);
        GPU_compute_dispatch(copy_sh, groups_copy, 1u, 1u);
        GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
        GPU_shader_unbind();
        src_complex_z = temp_copy;
      }
    }



    if (!pygpu_ocean_dispatch_complexs3_to_disp(
            src_complex_y, src_complex_x, src_complex_z, dst_ssbo, expected_full))
    {
      PyErr_SetString(PyExc_RuntimeError, "complex3->disp conversion failed");
      if (created_transient_dst && dst_ssbo) {
        GPU_storagebuf_free(dst_ssbo);
      }
      return false;
    }
  }

  /* Diagnostic: read small stats from spatial_complex, spatial_complex_x, spatial_complex_z */
  if (g_ocean_debug_prints) {
    try {
      std::vector<float> tmp;
      const size_t sample_count = expected_full;
      tmp.resize(sample_count * 2u);

      auto print_buf_stats = [&](StorageBuf *sb, const char *label) {
        if (!sb) {
          printf("[dbg] %s: <null>\n", label);
          return;
        }
        size_t use = sample_count;
        GPU_storagebuf_read(sb, tmp.data());
        double sum_sq = 0.0;
        double max_mag = 0.0;
        for (size_t i = 0; i < use; ++i) {
          double re = double(tmp[i * 2u + 0]);
          double im = double(tmp[i * 2u + 1]);
          double mag2 = re * re + im * im;
          sum_sq += mag2;
          if (mag2 > max_mag)
            max_mag = mag2;
        }
        double rms = (use > 0) ? sqrt(sum_sq / double(use)) : 0.0;
        max_mag = sqrt(max_mag);
        printf("[dbg] %s: elements=%zu rms=%g max_mag=%g sample0=(%+.6g,%+.6g)\n",
               label,
               use,
               rms,
               max_mag,
               double(tmp[0]),
               double(tmp[1]));
        fflush(stdout);
      };

      print_buf_stats(spatial_complex, "spatial_complex (Y?)");
      print_buf_stats(spatial_complex_x, "spatial_complex_x (X?)");
      print_buf_stats(spatial_complex_z, "spatial_complex_z (Z?)");
    }
    catch (...) {
      OCEAN_DBG_PRINT("[dbg] exception while reading spatial_complex stats\n");
    }
  }

  int tex_side = 0;
  if (expected_full > 0) {
    tex_side = int(std::sqrt(double(expected_full)));
    while (size_t(tex_side) * size_t(tex_side) < expected_full) {
      ++tex_side;
    }
  }

  *r_disp_ssbo = dst_ssbo;
  *r_tex_side = tex_side;
  return true;
}

/* -------------------------------------------------------------------- */
/* Main Simulation function End                                         */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* High Level Python API                                                */
/* -------------------------------------------------------------------- */



/* Python wrapper: simulate_and_export_disp_xyz_ssbo(ocean, existing_ssbo:GPUStorageBuf|None=None,
 * time:float=0.0, scale:float=1.0, chop:float=0.0, size:float=0.0) -> (GPUStorageBuf, int)
 * (Ajout de mesures temps / FPS)
 */
static PyObject *pygpu_ocean_simulate_and_export_disp_ssbo(PyObject * /*self*/,
                                                           PyObject *args,
                                                           PyObject *kwds)
{
  PyObject *py_ocean_obj = nullptr;
  PyObject *py_existing_ssbo_obj = nullptr;
  double time = 0.0;
  double scale = 1.0;
  double chop = 0.0;
  double size_param = 0.0;

  static char *kwlist[] = {
      (char *)"ocean",
      (char *)"existing_ssbo",
      (char *)"time",
      (char *)"scale",
      (char *)"chop",
      (char *)"size",
      nullptr,
  };

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "O|Odddd",
                                   kwlist,
                                   &py_ocean_obj,
                                   &py_existing_ssbo_obj,
                                   &time,
                                   &scale,
                                   &chop,
                                   &size_param))
  {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  OceanCapsuleContext *ctx = nullptr;
  if (PyCapsule_CheckExact(py_ocean_obj)) {
    ctx = reinterpret_cast<OceanCapsuleContext *>(PyCapsule_GetContext(py_ocean_obj));
  }

  /* defaults from capsule context if caller omitted values */
  if (time == 0.0 && ctx) {
    time = ctx->time;
  }
  if (scale == 1.0 && ctx) {
    scale = ctx->scale;
  }
  if (chop == 0.0 && ctx) {
    chop = ctx->chop;
  }
  /* size: if not provided or <= 0, derive from capsule context */
  if (size_param <= 0.0f && ctx) {
    size_param = double(ctx->size) * double(ctx->spatial_size);
  }

  StorageBuf *existing_ssbo = nullptr;
  if (py_existing_ssbo_obj && py_existing_ssbo_obj != Py_None) {
    if (!PyObject_TypeCheck(py_existing_ssbo_obj, &BPyGPUStorageBuf_Type)) {
      PyErr_SetString(PyExc_TypeError, "existing_ssbo must be a GPUStorageBuf or None");
      return nullptr;
    }
    BPyGPUStorageBuf *b_exist = reinterpret_cast<BPyGPUStorageBuf *>(py_existing_ssbo_obj);
    if (!b_exist->ssbo) {
      PyErr_SetString(PyExc_ReferenceError, "Provided existing_ssbo has been freed");
      return nullptr;
    }
    existing_ssbo = b_exist->ssbo;
  }

  /* Profiling: mesure globale autour de l'appel complet (simulate + pipeline GPU) */
  auto prof_t0 = std::chrono::steady_clock::now();

  StorageBuf *out_ssbo = nullptr;
  int tex_side = 0;
  bool ok = pygpu_ocean_simulate_and_export_disp_xyz_ssbo_cpp(o,
                                                              existing_ssbo,
                                                              &out_ssbo,
                                                              &tex_side,
                                                              float(time),
                                                              float(scale),
                                                              float(chop),
                                                              float(size_param));

  auto prof_t1 = std::chrono::steady_clock::now();
  const double frame_delta_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                                    prof_t0 - g_ocean_prof_last_frame_time)
                                    .count() /
                                1000.0;

  /* Affichage périodique (1s) : print instantané (plus intuitif que l'EMA/Welford). */
  double elapsed_since_print = std::chrono::duration_cast<std::chrono::duration<double>>(
                                   prof_t1 - g_ocean_prof_last_print_time)
                                   .count();
  if (elapsed_since_print >= 1.0) {
    if (g_ocean_show_fps) {
      printf("[gpu.ocean.prof] simulate_and_export: since_last_frame=%.3f ms tex_side=%d\n",
             frame_delta_ms,
             tex_side);
      fflush(stdout);
    }

    /* reset throttle timestamp */
    g_ocean_prof_last_print_time = prof_t1;
  }

  if (!ok) {
    return nullptr; /* error already set */
  }

  /* Met à jour l'instant de la dernière frame pour la prochaine invocation. */
  g_ocean_prof_last_frame_time = prof_t0;

  PyObject *py_return_ssbo = nullptr;

  /* If caller passed an existing Python wrapper, return it (updated in-place). */
  if (py_existing_ssbo_obj && py_existing_ssbo_obj != Py_None) {
    Py_INCREF(py_existing_ssbo_obj);
    py_return_ssbo = py_existing_ssbo_obj;
  }
  else {
    /* Try to return cached wrapper if any (cache keyed by Ocean*). */
    auto it = g_ocean_ssbo_cache.find(o);
    if (it != g_ocean_ssbo_cache.end() && it->second.py_ssbo) {
      StorageBuf *cached = pygpu_ocean_entry_get_ssbo(&it->second);
      if (cached == out_ssbo) {
        Py_INCREF(it->second.py_ssbo);
        py_return_ssbo = it->second.py_ssbo;
      }
    }

    if (!py_return_ssbo) {
      MeshGPUCacheManager::get().ocean_internal_ssbo_detach(o, std::string("dst"));
      py_return_ssbo = BPyGPUStorageBuf_CreatePyObject(reinterpret_cast<StorageBuf *>(out_ssbo));
      if (!py_return_ssbo) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create GPUStorageBuf Python wrapper");
        return nullptr;
      }
    }
  }

  /* Profile summary si demandé : afficher aussi durée depuis la dernière frame (simple). */
  if (g_ocean_show_fps) {
    printf("[gpu.ocean.prof] simulate_and_export: since_last_frame=%.3f ms tex_side=%d\n",
           frame_delta_ms,
           tex_side);
    fflush(stdout);
  }

  /* Return (ssbo_python_obj, tex_side) */
  PyObject *ret = Py_BuildValue("Oi", py_return_ssbo, tex_side);
  Py_DECREF(py_return_ssbo); /* Py_BuildValue increments ref as needed; DECREF our local ref */
  return ret;
}

/* Updated scatter_to_mesh implementing cache-on-create behaviour.
 * Returns: None on successful scatter, or a bpy.types.Object (new reference)
 * when creating/returning the cached generated object.
 *
 * Ajout de mesures temps pour base SSBO update et dispatch final + FPS global.
 */
static PyObject *pygpu_ocean_scatter_to_mesh(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  auto prof_start = std::chrono::steady_clock::now();

  PyObject *py_ocean_obj = nullptr;
  PyObject *py_ob_eval = nullptr;
  PyObject *py_disp_sb_obj = nullptr;
  double size_param = 0.0;
  double height_scale = 1.0;

  static char *kwlist[] = {(char *)"ocean",
                           (char *)"ob_eval",
                           (char *)"disp_ssbo",
                           (char *)"size",
                           (char *)"height_scale",
                           nullptr};

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "OOOd|d",
                                   kwlist,
                                   &py_ocean_obj,
                                   &py_ob_eval,
                                   &py_disp_sb_obj,
                                   &size_param,
                                   &height_scale))
  {
    return nullptr;
  }

  /* Resolve Ocean* */
  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "GPU context is not active");
    return nullptr;
  }
  int resolution = 7;
  /* If caller asked to create/return a generated object (ob_eval == None) */
  if (py_ob_eval == Py_None) {
    /* Use helper which returns a new-ref and already caches the created object. */
    if (PyCapsule_CheckExact(py_ocean_obj)) {
      OceanCapsuleContext *ctx = reinterpret_cast<OceanCapsuleContext *>(
          PyCapsule_GetContext(py_ocean_obj));
      if (ctx) {
        resolution = ctx->resolution;
      }
    }

    PyObject *py_obj = pygpu_ocean_get_or_create_object(py_ocean_obj, resolution, o);
    /* pygpu_ocean_get_or_create_object returns new-ref or nullptr w/ exception set */
    return py_obj;
  }

  /* Validate provided evaluated object */
  ID *id_obj = nullptr;
  if (!pyrna_id_FromPyObject(py_ob_eval, &id_obj) || GS(id_obj->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError,
                 "Expected an evaluated Object (or None), not %.200s",
                 Py_TYPE(py_ob_eval)->tp_name);
    return nullptr;
  }
  Object *ob_eval = reinterpret_cast<Object *>(id_obj);
  if (!DEG_is_evaluated(ob_eval) || ob_eval->type != OB_MESH) {
    PyErr_SetString(PyExc_TypeError, "Expected an evaluated mesh object");
    return nullptr;
  }

  /* depsgraph needed by mesh GPU compute */
  Depsgraph *depsgraph = DEG_get_depsgraph_by_id(ob_eval->id);
  if (!depsgraph) {
    PyErr_SetString(PyExc_RuntimeError, "Cannot obtain Depsgraph for evaluated object");
    return nullptr;
  }

  /* Validate disp SSBO argument */
  if (!PyObject_TypeCheck(py_disp_sb_obj, &BPyGPUStorageBuf_Type)) {
    PyErr_SetString(PyExc_TypeError, "disp_ssbo must be a GPUStorageBuf");
    return nullptr;
  }
  BPyGPUStorageBuf *b_disp = reinterpret_cast<BPyGPUStorageBuf *>(py_disp_sb_obj);
  if (!b_disp->ssbo) {
    PyErr_SetString(PyExc_ReferenceError, "Provided disp_ssbo has been freed");
    return nullptr;
  }
  StorageBuf *disp_sb = b_disp->ssbo;

  /* Check ocean export shape and mesh topology compatibility */
  int M = 0, N = 0;
  if (!BKE_ocean_export_shape(o, &M, &N)) {
    PyErr_SetString(PyExc_RuntimeError, "BKE_ocean_export_shape failed");
    return nullptr;
  }
  if (M <= 0 || N <= 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid spectral shape (M/N)");
    return nullptr;
  }

  Mesh *me_eval = static_cast<Mesh *>(ob_eval->data);
  if (!me_eval) {
    PyErr_SetString(PyExc_RuntimeError, "Evaluated object has no mesh data");
    return nullptr;
  }

  const int verts = me_eval->verts_num;
  const int corners = me_eval->corners_num;
  const long expected_verts = (long)(M + 1) * (long)(N + 1);
  const long expected_corners = (long)M * (long)N * 4l;

  if (verts != expected_verts || corners != expected_corners) {
    /* mesh incompatible -> return/create cached generated object for convenience */
    PyObject *py_cached = pygpu_ocean_get_or_create_object(
        py_ocean_obj,
        (PyCapsule_CheckExact(py_ocean_obj) ?
             reinterpret_cast<OceanCapsuleContext *>(PyCapsule_GetContext(py_ocean_obj))
                 ->resolution :
             7),
        o);
    return py_cached; /* new-ref or nullptr */
  }

  /* Build basepos CPU buffer (vec4 per vertex) */
  if (verts <= 0) {
    PyErr_SetString(PyExc_ValueError, "Evaluated mesh has no vertices");
    return nullptr;
  }
  const size_t base_bytes = size_t(verts) * 4u * sizeof(float);

  float Lx = o->_Lx;
  float Lz = o->_Lz;
  const float half_x = Lx * 0.5f;
  const float half_z = Lz * 0.5f;

  /* --- Avoid allocating/filling every frame:
   *  - check cached state first (g_ocean_base_state),
   *  - allocate a padded CPU buffer (cached per Ocean) only when needed,
   *  - precompute X per column and parallelize rows fill.
   *
   * Note: do NOT free the cached buffer; module cleanup frees it. */
  bool need_update = true;
  auto it_state = g_ocean_base_state.find(o);
  const float eps = 1e-6f;
  if (it_state != g_ocean_base_state.end()) {
    float last_Lx = std::get<0>(it_state->second);
    float last_Lz = std::get<1>(it_state->second);
    int last_verts = std::get<2>(it_state->second);
    if (fabsf(last_Lx - Lx) <= eps && fabsf(last_Lz - Lz) <= eps && last_verts == verts) {
      need_update = false;
    }
  }

  float *base_cpu = nullptr;
  if (need_update) {
    /* reuse a cached padded CPU buffer to avoid MEM_mallocN/MEM_freeN each frame */
    base_cpu = pygpu_ocean_get_or_alloc_padded_cpu(o, base_bytes);
    if (!base_cpu) {
      PyErr_NoMemory();
      return nullptr;
    }

    /* Precompute inverses and column X values to avoid repeated divisions. */
    const float invN = (N != 0) ? (1.0f / float(N)) : 0.0f;
    const float invM = (M != 0) ? (1.0f / float(M)) : 0.0f;

    std::vector<float> col_x;
    col_x.resize(size_t(N + 1));
    for (int j = 0; j <= N; ++j) {
      col_x[size_t(j)] = (float(j) * invN) * Lx - half_x;
    }

    const int rows = M + 1;
    /* Parallel fill per-row. Each row writes (N+1) vec4 elements. */
    blender::threading::parallel_for(
        blender::IndexRange(rows), 64, [&](const blender::IndexRange range) {
          for (int i : range) {
            float vy = (float(i) * invM) * Lz - half_z;
            const size_t row_base = size_t(i) * size_t(N + 1) * 4u;
            float *dst = base_cpu + row_base;
            for (int j = 0; j <= N; ++j) {
              const size_t k = size_t(j) * 4u;
              dst[k + 0] = col_x[size_t(j)];
              dst[k + 1] = vy;
              dst[k + 2] = 0.0f;
              dst[k + 3] = 1.0f;
            }
          }
        });
  }
  else {
    /* nothing to do: base_cpu remains unused and base SSBO already contains correct data */
  }

  /* Reuse or create cached base SSBO for this Ocean, then update it in-place. */
  SSBOCacheEntry *base_entry = pygpu_ocean_get_or_create_base_ssbo_entry(
      o, base_bytes, "ocean_basepos");
  if (!base_entry) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate or retrieve cached base SSBO");
    return nullptr;
  }
  StorageBuf *base_sb = pygpu_ocean_entry_get_ssbo(base_entry);
  if (!base_sb) {
    PyErr_SetString(PyExc_RuntimeError, "Cached base SSBO is invalid");
    return nullptr;
  }

  if (need_update) {
    GPU_storagebuf_update(base_sb, base_cpu);
    /* update cached state */
    g_ocean_base_state[o] = std::make_tuple(Lx, Lz, verts);
  }
  else {
    /* nothing to do, base_sb already contains correct data */
  }

  /* Dispatch final mesh compute */
  GpuComputeStatus status = pygpu_ocean_dispatch_final_shader(
      o, depsgraph, ob_eval, disp_sb, base_sb, float(o->_Lx), float(height_scale));

  if (status == GpuComputeStatus::NotReady) {
    Py_RETURN_NONE;
  }
  else if (status == GpuComputeStatus::Error) {
    return nullptr; /* exception déjà définie par le helper */
  }
  Py_RETURN_NONE;
}

/* -------------------------------------------------------------------- */
/* High Level Python API End                                            */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Free Resources Helpers                                               */
/* -------------------------------------------------------------------- */

static PyObject *pygpu_ocean_free_ocean(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_ocean_obj = nullptr;
  if (!PyArg_ParseTuple(args, "O", &py_ocean_obj)) {
    return nullptr;
  }

  Ocean *o = nullptr;
  if (!get_ocean_ptr_from_pyobj(py_ocean_obj, &o)) {
    return nullptr;
  }

  /* If we have cached SSBOs for this Ocean, free them and their Python wrappers. */
  if (o) {
    auto free_one_cache_entry = [&](std::unordered_map<Ocean *, SSBOCacheEntry> &cache) {
      auto it = cache.find(o);
      if (it != cache.end()) {
        SSBOCacheEntry &entry = it->second;
        if (entry.py_ssbo) {
          /* wrapper frees the native buffer in its dealloc. */
          Py_DECREF(entry.py_ssbo);
          entry.py_ssbo = nullptr;
        }
        cache.erase(it);
      }
    };

    free_one_cache_entry(g_ocean_ssbo_cache);
    free_one_cache_entry(g_ocean_base_ssbo_cache);
    free_one_cache_entry(g_ocean_out_ssbo_cache);

    /* Free internal SSBOs owned by the internal cache for this Ocean. */
    pygpu_ocean_free_internal_ssbos_for_ocean(o);

    /* Free padded CPU buffer cache for this Ocean if present. */
    auto it_pad = g_ocean_padded_cpu_cache.find(o);
    if (it_pad != g_ocean_padded_cpu_cache.end()) {
      float *ptr = it_pad->second.first;
      if (ptr) {
        MEM_freeN(ptr);
      }
      g_ocean_padded_cpu_cache.erase(it_pad);
    }

    /* Remove base state cache entry for this Ocean. */
    auto it_state = g_ocean_base_state.find(o);
    if (it_state != g_ocean_base_state.end()) {
      g_ocean_base_state.erase(it_state);
    }

    /* Remove weakref entry for this Ocean from object cache (if present). */
    auto it_obj = g_ocean_object_cache.find(o);
    if (it_obj != g_ocean_object_cache.end()) {
      PyObject *weak = it_obj->second;
      if (weak) {
        /* stored object is a weakref: DECREF the weakref object itself (does not DECREF the
         * target) */
        Py_DECREF(weak);
      }
      g_ocean_object_cache.erase(it_obj);
    }
  }

  /* Note: do NOT clear the entire g_ocean_object_cache here — free_resources() and module
   * cleanup handle global teardown. We only remove the entry related to the Ocean being freed. */

  if (o) {
    BKE_ocean_free(o);
  }

  if (PyCapsule_CheckExact(py_ocean_obj)) {
    void *p = PyCapsule_GetPointer(py_ocean_obj, PY_OCEAN_PTR_CAPSULE);
    if (p == nullptr) {
      if (PyErr_Occurred()) {
        PyErr_Clear();
      }
      p = PyCapsule_GetPointer(py_ocean_obj, nullptr);
    }
    if (p) {
      if (PyCapsule_SetPointer(py_ocean_obj, nullptr) == -1) {
        PyErr_Clear();
      }
    }
  }

  g_ocean_h0_last_ptr.erase(o);
  g_ocean_h0m_last_ptr.erase(o);

  Py_RETURN_NONE;
}

/* Free GPU-side resources (shaders, internal SSBOs, cached SSBO wrappers, padded CPU buffers).
 * Intended to be called while GPU context is active (e.g. at end of modal operator).
 * Exposed as Python: gpu.ocean.free_resources()
 */
static PyObject *pygpu_ocean_free_resources(PyObject * /*self*/, PyObject * /*args*/)
{
  /* Require GPU context so GPU_shader_free / GPU_storagebuf_free can be called safely. */
  if (!GPU_context_active_get()) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "gpu.ocean.free_resources: GPU context is not active (cannot free GPU resources)");
    return nullptr;
  }

  /* Free cached padded CPU buffers. */
  for (auto &kv : g_ocean_padded_cpu_cache) {
    float *ptr = kv.second.first;
    if (ptr) {
      MEM_freeN(ptr);
    }
  }
  g_ocean_padded_cpu_cache.clear();

  /* Free cached GPU SSBO Python wrappers (they will free native buffers in their dealloc). */
  auto free_cache = [](std::unordered_map<Ocean *, SSBOCacheEntry> &cache) {
    for (auto &kv : cache) {
      SSBOCacheEntry &entry = kv.second;
      if (entry.py_ssbo) {
        Py_DECREF(entry.py_ssbo);
        entry.py_ssbo = nullptr;
      }
    }
    cache.clear();
  };

  free_cache(g_ocean_ssbo_cache);
  free_cache(g_ocean_base_ssbo_cache);
  free_cache(g_ocean_out_ssbo_cache);

  /* Free cached generated Python object wrappers. */
  for (auto &kv : g_ocean_object_cache) {
    PyObject *py_obj = kv.second;
    if (py_obj) {
      Py_DECREF(py_obj);
    }
  }
  g_ocean_object_cache.clear();

  /* Free all internal SSBOs (raw StorageBuf* owned by internal cache). */
  pygpu_ocean_free_all_internal_ssbos();

  /* Free all created shaders via GPU API. */
  if (g_ocean_eval_shader) {
    GPU_shader_free(g_ocean_eval_shader);
    g_ocean_eval_shader = nullptr;
  }
  if (g_ocean_fft_row_dft_shader) {
    GPU_shader_free(g_ocean_fft_row_dft_shader);
    g_ocean_fft_row_dft_shader = nullptr;
  }
  if (g_ocean_htilda_simulate_shader) {
    GPU_shader_free(g_ocean_htilda_simulate_shader);
    g_ocean_htilda_simulate_shader = nullptr;
  }
  if (g_ocean_htilda_expand_shader) {
    GPU_shader_free(g_ocean_htilda_expand_shader);
    g_ocean_htilda_expand_shader = nullptr;
  }
  if (g_ocean_transpose_shader) {
    GPU_shader_free(g_ocean_transpose_shader);
    g_ocean_transpose_shader = nullptr;
  }
  if (g_ocean_vec2_copy_shader) {
    GPU_shader_free(g_ocean_vec2_copy_shader);
    g_ocean_vec2_copy_shader = nullptr;
  }
  if (g_ocean_prep_fftin_chop_shader) {
    GPU_shader_free(g_ocean_prep_fftin_chop_shader);
    g_ocean_prep_fftin_chop_shader = nullptr;
  }
  if (g_ocean_complex3_to_disp_shader) {
    GPU_shader_free(g_ocean_complex3_to_disp_shader);
    g_ocean_complex3_to_disp_shader = nullptr;
  }
  if (g_ocean_final_shader) {
    GPU_shader_free(g_ocean_final_shader);
    g_ocean_final_shader = nullptr;
  }
  Py_RETURN_NONE;
}

/* -------------------------------------------------------------------- */
/* Free Resources Helpers End                                           */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Method table & module init                                           */
/* -------------------------------------------------------------------- */

static PyMethodDef pygpu_ocean_methods[] = {
    {"create_default_ocean",
     (PyCFunction)pygpu_ocean_create_default_ocean,
     METH_VARARGS | METH_KEYWORDS,
     "create_default_ocean(resolution:int=..., size:float=..., spatial_size:int=..., "
     "wave_scale:float=..., smallest_wave:float=..., chop_amount:float=..., "
     "wind_velocity:float=..., spectrum:str='Phillips') -> Capsule\n\n"
     "Create an Ocean using modifier defaults. Optional keyword arguments override the modifier "
     "defaults (values taken from DNA defaults when omitted). The optional `spectrum` string may "
     "be "
     "one of: \"Phillips\", \"JONSWAP\", \"Texel-Marsen-Arsloe\", \"Pierson-Moskowitz\". "
     "Returns a PyCapsule wrapping an Ocean*."},
    {"free_ocean",
     (PyCFunction)pygpu_ocean_free_ocean,
     METH_VARARGS,
     "free_ocean(ocean_capsule_or_int) -> None\n\nFree an Ocean created by create_default_ocean "
     "(accepts capsule or integer pointer)."},
    {"free_resources",
     (PyCFunction)pygpu_ocean_free_resources,
     METH_NOARGS,
     "free_resources() -> None\n\nFree GPU-side resources (shaders, internal SSBOs, cached "
     "wrappers). Call while GPU context is active."},
    {"generate_object",
     (PyCFunction)pygpu_ocean_generate_object,
     METH_VARARGS,
     "generate_object(ocean_ptr, resolution:int=7, target_object: "
     "bpy.types.Object|None=None) -> bpy.types.Mesh\n\nGenerate a Mesh from the given Ocean using "
     "generate_ocean_geometry and return a bpy.types.Mesh."},
    {"free_generated_mesh",
     (PyCFunction)pygpu_ocean_free_generated_mesh,
     METH_VARARGS,
     "free_generated_mesh(ocean_capsule_or_int) -> None\n\nFree cached generated object for this "
     "Ocean."},
    {"export_disp_xyz_ssbo",
     (PyCFunction)pygpu_ocean_export_disp_xyz_ssbo,
     METH_VARARGS,
     "export_disp_xyz_ssbo(ocean_ptr) -> GPUStorageBuf\n\nCreate and return a GPUStorageBuf with "
     "displacement XYZ floats."},
    {"export_htilda_ssbo",
     (PyCFunction)pygpu_ocean_export_htilda_ssbo,
     METH_VARARGS,
     "export_htilda_ssbo(ocean_ptr) -> GPUStorageBuf\n\nCreate and return a GPUStorageBuf "
     "populated with htilda (complex) data."},
    {"simulate_and_export_disp_xyz_ssbo",
     (PyCFunction)pygpu_ocean_simulate_and_export_disp_ssbo,
     METH_VARARGS | METH_KEYWORDS,
     "simulate_and_export_disp_xyz_ssbo(ocean, existing_ssbo:GPUStorageBuf|None=None, "
     "time:float=0.0, scale:float=1.0, chop:float=0.0, size:float=0.0) -> (GPUStorageBuf, int)"},
    {"scatter_disp_to_mesh",
     (PyCFunction)pygpu_ocean_scatter_to_mesh,
     METH_VARARGS | METH_KEYWORDS,
     "scatter_disp_to_mesh(ocean, ob_eval, disp_ssbo, size:float, height_scale:float=1.0) -> "
     "None"},
    {"evaluate_disp_with_ssbos",
     (PyCFunction)pygpu_ocean_evaluate_disp_with_ssbos,
     METH_VARARGS,
     "evaluate_disp_with_ssbos(ocean, basepos_ssbo, disp_ssbo, out_ssbo, size_param:float=1.0) -> "
     "None\n\n"
     "Bind (basepos, disp, out) SSBOs and dispatch the ocean eval compute shader."},
    {"test_eval_shader",
     (PyCFunction)pygpu_ocean_test_eval_shader,
     METH_VARARGS,
     "test_eval_shader(ocean, size_param:float=1.0) -> None\n\n"
     "End-to-end test: export disp, build basepos grid, dispatch compute shader (no readback)."},
    {"fft_rows_from_htilda",
     (PyCFunction)pygpu_ocean_gpu_fft_rows,
     METH_VARARGS,
     "fft_rows_from_htilda(ocean_capsule) -> GPUStorageBuf\n\n"
     "Prototype: export htilda, perform row-wise DFT on GPU (naive), transpose and return SSBO."},
    {"export_shape",
     (PyCFunction)pygpu_ocean_export_shape,
     METH_VARARGS,
     "export_shape(ocean_capsule) -> (M, N)\n\nReturn the spectral grid shape used by the Ocean "
     "export."},
    {"dump_ssbo_indices",
     (PyCFunction)pygpu_ocean_dump_ssbo_indices,
     METH_VARARGS | METH_KEYWORDS,
     "dump_ssbo_indices(ssbo, element_count:int, indices:Sequence[int], label:Optional[str]=None) "
     "-> None\n\n"
     "Read SSBO (vec2) and print the selected indices."},
    {"debug_compare_expansion",
     (PyCFunction)pygpu_ocean_debug_compare_expansion,
     METH_VARARGS,
     "debug_compare_expansion(ocean_capsule, ssbo:GPUStorageBuf, is_compact:int=1) -> None\n\n"
     "Compare CPU-expanded htilda vs SSBO content (SSBO can be compact or full). Prints top "
     "mismatches."},
    {"debug_compare_spatial",
     (PyCFunction)pygpu_ocean_debug_compare_spatial,
     METH_VARARGS,
     "debug_compare_spatial(ocean_capsule, try_factors:bool=True) -> None\n\n"
     "Compare GPU spatial_complex real part vs CPU exported disp Y. Tests scale factors and "
     "prints a report."},
    {"set_debug_dumps",
     (PyCFunction)pygpu_ocean_set_debug_dumps,
     METH_VARARGS,
     "set_debug_dumps(on: bool) -> None\n\nEnable/disable detailed gpu.ocean dumps (use "
     "True/False)."},
    {"ssbo_info",
     (PyCFunction)pygpu_ocean_ssbo_info,
     METH_VARARGS,
     "ssbo_info(ssbo:GPUStorageBuf) -> (byte_length:uint64, complex_count:uint64)\n\n"
     "Return raw SSBO byte length and number of complex vec2 elements (heuristic)."},
    {"read_ssbo_bytes",
     (PyCFunction)pygpu_ocean_read_ssbo_bytes,
     METH_VARARGS,
     "read_ssbo_bytes(ssbo:GPUStorageBuf, complex_count:Optional[int]=None) -> bytes\n\n"
     "Read SSBO content and return raw bytes (float32 array interleaved: re0,im0,re1,im1,...)."},
    {"set_debug_dumps_full",
     (PyCFunction)pygpu_ocean_set_debug_dumps_full,
     METH_VARARGS,
     "set_debug_dumps_full(on: bool) -> None\n\nEnable/disable full SSBO dumps (writes .bin "
     "float32 files in temp dir)."},
    {"debug_dump_ocean",
     (PyCFunction)pygpu_ocean_debug_dump_ocean,
     METH_VARARGS,
     "debug_dump_ocean(ocean_capsule) -> None\n\nDump Ocean struct fields and sample values for "
     "debugging."},
    {"debug_compare_spatial_extended",
     (PyCFunction)pygpu_ocean_debug_compare_spatial_extended,
     METH_VARARGS,
     "debug_compare_spatial_extended(ocean_capsule, try_factors:bool=True) -> None\n\nExtended "
     "compare GPU spatial vs CPU disp; prints diagnostics."},
    {"set_debug_prints",
     (PyCFunction)pygpu_ocean_set_debug_prints,
     METH_VARARGS,
     "set_debug_prints(on: bool) -> None\n\nEnable/disable debug prints used by GPU debug "
     "helpers."},
    {"set_show_fps",
     (PyCFunction)pygpu_ocean_set_show_fps,
     METH_VARARGS,
     "set_show_fps(on: bool) -> None\n\nEnable/disable periodic FPS summary printing from "
     "gpu.ocean."},
    {"validate_cpu_vs_gpu",
     (PyCFunction)pygpu_ocean_validate_cpu_vs_gpu,
     METH_VARARGS | METH_KEYWORDS,
     "validate_cpu_vs_gpu(ocean, time:float=0.0, scale:float=1.0, chop:float=0.0, size:float=0.0, "
     "tolerance:float=1e-6) -> None\n\n"
     "Run CPU simulate/export and GPU pipeline, compare disp vec3 outputs and print metrics."},
    {nullptr, nullptr, 0, nullptr},
};

/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Free Module                                                          */
/* -------------------------------------------------------------------- */

/* Module cleanup: free cached padded CPU buffers */
/* Ensure the shader is freed at module unload (augment existing cleanup). */
static void pygpu_ocean_module_free(void * /*module*/)
{
  /* free cached padded cpu buffers */
  for (auto &kv : g_ocean_padded_cpu_cache) {
    float *ptr = kv.second.first;
    if (ptr) {
      MEM_freeN(ptr);
    }
  }
  g_ocean_padded_cpu_cache.clear();

  /* free cached GPU SSBOs and Python wrappers (disp, base, out)
   * Respect ownership: if a Python wrapper exists, DECREF it and do NOT free the native buffer;
   * otherwise free the native buffer directly. */
  auto free_cache = [](std::unordered_map<Ocean *, SSBOCacheEntry> &cache) {
    for (auto &kv : cache) {
      SSBOCacheEntry &entry = kv.second;
      if (entry.py_ssbo) {
        /* Wrapper will free the StorageBuf in its dealloc. */
        Py_DECREF(entry.py_ssbo);
        entry.py_ssbo = nullptr;
      }
      /* no raw StorageBuf* stored any more in the entry */
    }
    cache.clear();
  };

  free_cache(g_ocean_ssbo_cache);
  free_cache(g_ocean_base_ssbo_cache);
  free_cache(g_ocean_out_ssbo_cache);

  /* free cached shaders
   * Only call GPU_shader_free when GPU subsystem is still initialized.
   * If GPU is already shut down, avoid calling into backend (prevents use-after-free
   * of backend global objects such as pipeline pools / mutexes).
   */
  if (GPU_context_active_get()) {
    if (g_ocean_eval_shader) {
      GPU_shader_free(g_ocean_eval_shader);
      g_ocean_eval_shader = nullptr;
    }
    if (g_ocean_fft_row_dft_shader) {
      GPU_shader_free(g_ocean_fft_row_dft_shader);
      g_ocean_fft_row_dft_shader = nullptr;
    }
    if (g_ocean_htilda_simulate_shader) {
      GPU_shader_free(g_ocean_htilda_simulate_shader);
      g_ocean_htilda_simulate_shader = nullptr;
    }
    if (g_ocean_htilda_expand_shader) {
      GPU_shader_free(g_ocean_htilda_expand_shader);
      g_ocean_htilda_expand_shader = nullptr;
    }
    if (g_ocean_transpose_shader) {
      GPU_shader_free(g_ocean_transpose_shader);
      g_ocean_transpose_shader = nullptr;
    }
    if (g_ocean_vec2_copy_shader) {
      GPU_shader_free(g_ocean_vec2_copy_shader);
      g_ocean_vec2_copy_shader = nullptr;
    }
    if (g_ocean_prep_fftin_chop_shader) {
      GPU_shader_free(g_ocean_prep_fftin_chop_shader);
      g_ocean_prep_fftin_chop_shader = nullptr;
    }
    if (g_ocean_complex3_to_disp_shader) {
      GPU_shader_free(g_ocean_complex3_to_disp_shader);
      g_ocean_complex3_to_disp_shader = nullptr;
    }
    if (g_ocean_final_shader) {
      GPU_shader_free(g_ocean_final_shader);
      g_ocean_final_shader = nullptr;
    }
    /* free all internal SSBOs */
    pygpu_ocean_free_all_internal_ssbos();
  }
  else {
    /* GPU already shut down: avoid backend calls. Clear pointers so future checks don't try again.
     */
    g_ocean_eval_shader = nullptr;
    g_ocean_fft_row_dft_shader = nullptr;
    g_ocean_htilda_expand_shader = nullptr;
    g_ocean_htilda_simulate_shader = nullptr;
    g_ocean_transpose_shader = nullptr;
    g_ocean_vec2_copy_shader = nullptr;
    g_ocean_complex3_to_disp_shader = nullptr;
    g_ocean_prep_fftin_chop_shader = nullptr;
    g_ocean_final_shader = nullptr;

    g_ocean_base_ssbo_cache.clear();
    g_ocean_out_ssbo_cache.clear();
  }
}

/* Module definition */
static PyModuleDef pygpu_ocean_module_def = {
    PyModuleDef_HEAD_INIT,
    /* m_name */ "gpu.ocean",
    /* m_doc  */ "Ocean export helpers for GPU",
    /* m_size */ -1,
    /* m_methods */ pygpu_ocean_methods,
    /* m_slots */ nullptr,
    /* m_traverse */ nullptr,
    /* m_clear */ nullptr,
    /* m_free */ pygpu_ocean_module_free,
};

/* Module initialization */
PyObject *bpygpu_ocean_init(void)
{
  return PyModule_Create(&pygpu_ocean_module_def);
}
