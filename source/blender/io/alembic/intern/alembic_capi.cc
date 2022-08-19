/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup balembic
 */

#include "../ABC_alembic.h"
#include "IO_types.h"

#include <Alembic/AbcMaterial/IMaterial.h>

#include "abc_axis_conversion.h"
#include "abc_reader_archive.h"
#include "abc_reader_camera.h"
#include "abc_reader_curves.h"
#include "abc_reader_mesh.h"
#include "abc_reader_nurbs.h"
#include "abc_reader_points.h"
#include "abc_reader_transform.h"
#include "abc_util.h"

#include "MEM_guardedalloc.h"

#include "DNA_cachefile_types.h"
#include "DNA_collection_types.h"
#include "DNA_curve_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_cachefile.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_global.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_object.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "ED_undo.h"

#include "BLI_compiler_compat.h"
#include "BLI_fileops.h"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_timeit.hh"

#include "WM_api.h"
#include "WM_types.h"

using Alembic::Abc::IV3fArrayProperty;
using Alembic::Abc::ObjectHeader;
using Alembic::Abc::PropertyHeader;
using Alembic::Abc::V3fArraySamplePtr;
using Alembic::AbcGeom::ICamera;
using Alembic::AbcGeom::ICurves;
using Alembic::AbcGeom::IFaceSet;
using Alembic::AbcGeom::ILight;
using Alembic::AbcGeom::INuPatch;
using Alembic::AbcGeom::IObject;
using Alembic::AbcGeom::IPoints;
using Alembic::AbcGeom::IPolyMesh;
using Alembic::AbcGeom::IPolyMeshSchema;
using Alembic::AbcGeom::ISampleSelector;
using Alembic::AbcGeom::ISubD;
using Alembic::AbcGeom::IXform;
using Alembic::AbcGeom::kWrapExisting;
using Alembic::AbcGeom::MetaData;
using Alembic::AbcMaterial::IMaterial;

using namespace blender::io::alembic;

BLI_INLINE ArchiveReader *archive_from_handle(CacheArchiveHandle *handle)
{
  return reinterpret_cast<ArchiveReader *>(handle);
}

BLI_INLINE CacheArchiveHandle *handle_from_archive(ArchiveReader *archive)
{
  return reinterpret_cast<CacheArchiveHandle *>(archive);
}

//#define USE_NURBS

/* NOTE: this function is similar to visit_objects below, need to keep them in
 * sync. */
static bool gather_objects_paths(const IObject &object, ListBase *object_paths)
{
  if (!object.valid()) {
    return false;
  }

  size_t children_claiming_this_object = 0;
  size_t num_children = object.getNumChildren();

  for (size_t i = 0; i < num_children; i++) {
    bool child_claims_this_object = gather_objects_paths(object.getChild(i), object_paths);
    children_claiming_this_object += child_claims_this_object ? 1 : 0;
  }

  const MetaData &md = object.getMetaData();
  bool get_path = false;
  bool parent_is_part_of_this_object = false;

  if (!object.getParent()) {
    /* The root itself is not an object we should import. */
  }
  else if (IXform::matches(md)) {
    if (has_property(object.getProperties(), "locator")) {
      get_path = true;
    }
    else {
      get_path = children_claiming_this_object == 0;
    }

    /* Transforms are never "data" for their parent. */
    parent_is_part_of_this_object = false;
  }
  else {
    /* These types are "data" for their parent. */
    get_path = IPolyMesh::matches(md) || ISubD::matches(md) ||
#ifdef USE_NURBS
               INuPatch::matches(md) ||
#endif
               ICamera::matches(md) || IPoints::matches(md) || ICurves::matches(md);
    parent_is_part_of_this_object = get_path;
  }

  if (get_path) {
    void *abc_path_void = MEM_callocN(sizeof(CacheObjectPath), "CacheObjectPath");
    CacheObjectPath *abc_path = static_cast<CacheObjectPath *>(abc_path_void);

    BLI_strncpy(abc_path->path, object.getFullName().c_str(), sizeof(abc_path->path));
    BLI_addtail(object_paths, abc_path);
  }

  return parent_is_part_of_this_object;
}

CacheArchiveHandle *ABC_create_handle(struct Main *bmain,
                                      const char *filename,
                                      const CacheFileLayer *layers,
                                      ListBase *object_paths)
{
  std::vector<const char *> filenames;
  filenames.push_back(filename);

  while (layers) {
    if ((layers->flag & CACHEFILE_LAYER_HIDDEN) == 0) {
      filenames.push_back(layers->filepath);
    }
    layers = layers->next;
  }

  /* We need to reverse the order as overriding archives should come first. */
  std::reverse(filenames.begin(), filenames.end());

  ArchiveReader *archive = ArchiveReader::get(bmain, filenames);

  if (!archive || !archive->valid()) {
    delete archive;
    return nullptr;
  }

  if (object_paths) {
    gather_objects_paths(archive->getTop(), object_paths);
  }

  return handle_from_archive(archive);
}

void ABC_free_handle(CacheArchiveHandle *handle)
{
  delete archive_from_handle(handle);
}

int ABC_get_version()
{
  return ALEMBIC_LIBRARY_VERSION;
}

static void find_iobject(const IObject &object, IObject &ret, const std::string &path)
{
  if (!object.valid()) {
    return;
  }

  std::vector<std::string> tokens;
  split(path, '/', tokens);

  IObject tmp = object;

  std::vector<std::string>::iterator iter;
  for (iter = tokens.begin(); iter != tokens.end(); ++iter) {
    IObject child = tmp.getChild(*iter);
    tmp = child;
  }

  ret = tmp;
}

/* ********************** Import file ********************** */

/**
 * Generates an AbcObjectReader for this Alembic object and its children.
 *
 * \param object: The Alembic IObject to visit.
 * \param readers: The created AbcObjectReader * will be appended to this vector.
 * \param settings: Import settings, not used directly but passed to the
 *                 AbcObjectReader subclass constructors.
 * \param r_assign_as_parent: Return parameter, contains a list of reader
 *                 pointers, whose parent pointer should still be set.
 *                 This is filled when this call to visit_object() didn't create
 *                 a reader that should be the parent.
 * \return A pair of boolean and reader pointer. The boolean indicates whether
 *         this IObject claims its parent as part of the same object
 *         (for example an IPolyMesh object would claim its parent, as the mesh
 *         is interpreted as the object's data, and the parent IXform as its
 *         Blender object). The pointer is the AbcObjectReader that represents
 *         the IObject parameter.
 *
 * NOTE: this function is similar to gather_object_paths above, need to keep
 * them in sync. */
static std::pair<bool, AbcObjectReader *> visit_object(
    const IObject &object,
    AbcObjectReader::ptr_vector &readers,
    ImportSettings &settings,
    AbcObjectReader::ptr_vector &r_assign_as_parent)
{
  const std::string &full_name = object.getFullName();

  if (!object.valid()) {
    std::cerr << "  - " << full_name << ": object is invalid, skipping it and all its children.\n";
    return std::make_pair(false, static_cast<AbcObjectReader *>(nullptr));
  }

  /* The interpretation of data by the children determine the role of this
   * object. This is especially important for Xform objects, as they can be
   * either part of a Blender object or a Blender object (Empty) themselves.
   */
  size_t children_claiming_this_object = 0;
  size_t num_children = object.getNumChildren();
  AbcObjectReader::ptr_vector claiming_child_readers;
  AbcObjectReader::ptr_vector nonclaiming_child_readers;
  AbcObjectReader::ptr_vector assign_as_parent;
  for (size_t i = 0; i < num_children; i++) {
    const IObject ichild = object.getChild(i);

    /* TODO: When we only support C++11, use std::tie() instead. */
    std::pair<bool, AbcObjectReader *> child_result;
    child_result = visit_object(ichild, readers, settings, assign_as_parent);

    bool child_claims_this_object = child_result.first;
    AbcObjectReader *child_reader = child_result.second;

    if (child_reader == nullptr) {
      BLI_assert(!child_claims_this_object);
    }
    else {
      if (child_claims_this_object) {
        claiming_child_readers.push_back(child_reader);
      }
      else {
        nonclaiming_child_readers.push_back(child_reader);
      }
    }

    children_claiming_this_object += child_claims_this_object ? 1 : 0;
  }
  BLI_assert(children_claiming_this_object == claiming_child_readers.size());
  UNUSED_VARS_NDEBUG(children_claiming_this_object);

  AbcObjectReader *reader = nullptr;
  const MetaData &md = object.getMetaData();
  bool parent_is_part_of_this_object = false;

  if (!object.getParent()) {
    /* The root itself is not an object we should import. */
  }
  else if (IXform::matches(md)) {
    bool create_empty;

    /* An xform can either be a Blender Object (if it contains a mesh, for
     * example), but it can also be an Empty. Its correct translation to
     * Blender's data model depends on its children. */

    /* Check whether or not this object is a Maya locator, which is
     * similar to empties used as parent object in Blender. */
    if (has_property(object.getProperties(), "locator")) {
      create_empty = true;
    }
    else {
      create_empty = claiming_child_readers.empty();
    }

    if (create_empty) {
      reader = new AbcEmptyReader(object, settings);
    }
  }
  else if (IPolyMesh::matches(md)) {
    reader = new AbcMeshReader(object, settings);
    parent_is_part_of_this_object = true;
  }
  else if (ISubD::matches(md)) {
    reader = new AbcSubDReader(object, settings);
    parent_is_part_of_this_object = true;
  }
  else if (INuPatch::matches(md)) {
#ifdef USE_NURBS
    /* TODO(kevin): importing cyclic NURBS from other software crashes
     * at the moment. This is due to the fact that NURBS in other
     * software have duplicated points which causes buffer overflows in
     * Blender. Need to figure out exactly how these points are
     * duplicated, in all cases (cyclic U, cyclic V, and cyclic UV).
     * Until this is fixed, disabling NURBS reading. */
    reader = new AbcNurbsReader(object, settings);
    parent_is_part_of_this_object = true;
#endif
  }
  else if (ICamera::matches(md)) {
    reader = new AbcCameraReader(object, settings);
    parent_is_part_of_this_object = true;
  }
  else if (IPoints::matches(md)) {
    reader = new AbcPointsReader(object, settings);
    parent_is_part_of_this_object = true;
  }
  else if (IMaterial::matches(md)) {
    /* Pass for now. */
  }
  else if (ILight::matches(md)) {
    /* Pass for now. */
  }
  else if (IFaceSet::matches(md)) {
    /* Pass, those are handled in the mesh reader. */
  }
  else if (ICurves::matches(md)) {
    reader = new AbcCurveReader(object, settings);
    parent_is_part_of_this_object = true;
  }
  else {
    std::cerr << "Alembic object " << full_name << " is of unsupported schema type '"
              << object.getMetaData().get("schemaObjTitle") << "'" << std::endl;
  }

  if (reader) {
    /* We have created a reader, which should imply that this object is
     * not claimed as part of any child Alembic object. */
    BLI_assert(claiming_child_readers.empty());

    readers.push_back(reader);
    reader->incref();

    CacheObjectPath *abc_path = static_cast<CacheObjectPath *>(
        MEM_callocN(sizeof(CacheObjectPath), "CacheObjectPath"));
    BLI_strncpy(abc_path->path, full_name.c_str(), sizeof(abc_path->path));
    BLI_addtail(&settings.cache_file->object_paths, abc_path);

    /* We can now assign this reader as parent for our children. */
    if (nonclaiming_child_readers.size() + assign_as_parent.size() > 0) {
      for (AbcObjectReader *child_reader : nonclaiming_child_readers) {
        child_reader->parent_reader = reader;
      }
      for (AbcObjectReader *child_reader : assign_as_parent) {
        child_reader->parent_reader = reader;
      }
    }
  }
  else if (object.getParent()) {
    if (!claiming_child_readers.empty()) {
      /* The first claiming child will serve just fine as parent to
       * our non-claiming children. Since all claiming children share
       * the same XForm, it doesn't really matter which one we pick. */
      AbcObjectReader *claiming_child = claiming_child_readers[0];
      for (AbcObjectReader *child_reader : nonclaiming_child_readers) {
        child_reader->parent_reader = claiming_child;
      }
      for (AbcObjectReader *child_reader : assign_as_parent) {
        child_reader->parent_reader = claiming_child;
      }
      /* Claiming children should have our parent set as their parent. */
      for (AbcObjectReader *child_reader : claiming_child_readers) {
        r_assign_as_parent.push_back(child_reader);
      }
    }
    else {
      /* This object isn't claimed by any child, and didn't produce
       * a reader. Odd situation, could be the top Alembic object, or
       * an unsupported Alembic schema. Delegate to our parent. */
      for (AbcObjectReader *child_reader : claiming_child_readers) {
        r_assign_as_parent.push_back(child_reader);
      }
      for (AbcObjectReader *child_reader : nonclaiming_child_readers) {
        r_assign_as_parent.push_back(child_reader);
      }
      for (AbcObjectReader *child_reader : assign_as_parent) {
        r_assign_as_parent.push_back(child_reader);
      }
    }
  }

  return std::make_pair(parent_is_part_of_this_object, reader);
}

enum {
  ABC_NO_ERROR = 0,
  ABC_ARCHIVE_FAIL,
};

struct ImportJobData {
  bContext *C;
  Main *bmain;
  Scene *scene;
  ViewLayer *view_layer;
  wmWindowManager *wm;

  char filename[1024];
  ImportSettings settings;

  ArchiveReader *archive;
  std::vector<AbcObjectReader *> readers;

  short *stop;
  short *do_update;
  float *progress;

  char error_code;
  bool was_cancelled;
  bool import_ok;
  bool is_background_job;
  blender::timeit::TimePoint start_time;
};

static void report_job_duration(const ImportJobData *data)
{
  blender::timeit::Nanoseconds duration = blender::timeit::Clock::now() - data->start_time;
  std::cout << "Alembic import of '" << data->filename << "' took ";
  blender::timeit::print_duration(duration);
  std::cout << '\n';
}

static void import_startjob(void *user_data, short *stop, short *do_update, float *progress)
{
  SCOPE_TIMER("Alembic import, objects reading and creation");

  ImportJobData *data = static_cast<ImportJobData *>(user_data);

  data->stop = stop;
  data->do_update = do_update;
  data->progress = progress;
  data->start_time = blender::timeit::Clock::now();

  WM_set_locked_interface(data->wm, true);

  ArchiveReader *archive = ArchiveReader::get(data->bmain, {data->filename});

  if (!archive || !archive->valid()) {
    data->error_code = ABC_ARCHIVE_FAIL;
    delete archive;
    return;
  }

  CacheFile *cache_file = static_cast<CacheFile *>(
      BKE_cachefile_add(data->bmain, BLI_path_basename(data->filename)));

  /* Decrement the ID ref-count because it is going to be incremented for each
   * modifier and constraint that it will be attached to, so since currently
   * it is not used by anyone, its use count will be off by one. */
  id_us_min(&cache_file->id);

  cache_file->is_sequence = data->settings.is_sequence;
  cache_file->scale = data->settings.scale;
  STRNCPY(cache_file->filepath, data->filename);

  data->archive = archive;
  data->settings.cache_file = cache_file;

  *data->do_update = true;
  *data->progress = 0.05f;

  /* Parse Alembic Archive. */
  AbcObjectReader::ptr_vector assign_as_parent;
  visit_object(archive->getTop(), data->readers, data->settings, assign_as_parent);

  /* There shouldn't be any orphans. */
  BLI_assert(assign_as_parent.empty());

  if (G.is_break) {
    data->was_cancelled = true;
    return;
  }

  *data->do_update = true;
  *data->progress = 0.1f;

  /* Create objects and set scene frame range. */

  const float size = static_cast<float>(data->readers.size());
  size_t i = 0;

  chrono_t min_time = std::numeric_limits<chrono_t>::max();
  chrono_t max_time = std::numeric_limits<chrono_t>::min();

  ISampleSelector sample_sel(0.0);
  std::vector<AbcObjectReader *>::iterator iter;
  for (iter = data->readers.begin(); iter != data->readers.end(); ++iter) {
    AbcObjectReader *reader = *iter;

    if (reader->valid()) {
      reader->readObjectData(data->bmain, sample_sel);

      min_time = std::min(min_time, reader->minTime());
      max_time = std::max(max_time, reader->maxTime());
    }
    else {
      std::cerr << "Object " << reader->name() << " in Alembic file " << data->filename
                << " is invalid.\n";
    }

    *data->progress = 0.1f + 0.3f * (++i / size);
    *data->do_update = true;

    if (G.is_break) {
      data->was_cancelled = true;
      return;
    }
  }

  if (data->settings.set_frame_range) {
    Scene *scene = data->scene;

    if (data->settings.is_sequence) {
      scene->r.sfra = data->settings.sequence_offset;
      scene->r.efra = scene->r.sfra + (data->settings.sequence_len - 1);
      scene->r.cfra = scene->r.sfra;
    }
    else if (min_time < max_time) {
      scene->r.sfra = static_cast<int>(round(min_time * FPS));
      scene->r.efra = static_cast<int>(round(max_time * FPS));
      scene->r.cfra = scene->r.sfra;
    }
  }

  /* Setup parenthood. */
  for (iter = data->readers.begin(); iter != data->readers.end(); ++iter) {
    const AbcObjectReader *reader = *iter;
    const AbcObjectReader *parent_reader = reader->parent_reader;
    Object *ob = reader->object();

    if (parent_reader == nullptr || !reader->inherits_xform()) {
      ob->parent = nullptr;
    }
    else {
      ob->parent = parent_reader->object();
    }
  }

  /* Setup transformations and constraints. */
  i = 0;
  for (iter = data->readers.begin(); iter != data->readers.end(); ++iter) {
    AbcObjectReader *reader = *iter;
    reader->setupObjectTransform(0.0);

    *data->progress = 0.7f + 0.3f * (++i / size);
    *data->do_update = true;

    if (G.is_break) {
      data->was_cancelled = true;
      return;
    }
  }
}

static void import_endjob(void *user_data)
{
  SCOPE_TIMER("Alembic import, cleanup");

  ImportJobData *data = static_cast<ImportJobData *>(user_data);

  /* Delete objects on cancellation. */
  if (data->was_cancelled) {
    for (AbcObjectReader *reader : data->readers) {
      Object *ob = reader->object();

      /* It's possible that cancellation occurred between the creation of
       * the reader and the creation of the Blender object. */
      if (ob == nullptr) {
        continue;
      }

      BKE_id_free_us(data->bmain, ob);
    }
  }
  else {
    Base *base;
    LayerCollection *lc;
    ViewLayer *view_layer = data->view_layer;

    BKE_view_layer_base_deselect_all(view_layer);

    lc = BKE_layer_collection_get_active(view_layer);

    /* Add all objects to the collection (don't do sync for each object). */
    BKE_layer_collection_resync_forbid();
    for (AbcObjectReader *reader : data->readers) {
      Object *ob = reader->object();
      BKE_collection_object_add(data->bmain, lc->collection, ob);
    }
    /* Sync the collection, and do view layer operations. */
    BKE_layer_collection_resync_allow();
    BKE_main_collection_sync(data->bmain);
    for (AbcObjectReader *reader : data->readers) {
      Object *ob = reader->object();
      base = BKE_view_layer_base_find(view_layer, ob);
      /* TODO: is setting active needed? */
      BKE_view_layer_base_select_and_set_active(view_layer, base);

      DEG_id_tag_update(&lc->collection->id, ID_RECALC_COPY_ON_WRITE);
      DEG_id_tag_update_ex(data->bmain,
                           &ob->id,
                           ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION |
                               ID_RECALC_BASE_FLAGS);
    }

    DEG_id_tag_update(&data->scene->id, ID_RECALC_BASE_FLAGS);
    DEG_relations_tag_update(data->bmain);

    if (data->is_background_job) {
      /* Blender already returned from the import operator, so we need to store our own extra undo
       * step. */
      ED_undo_push(data->C, "Alembic Import Finished");
    }
  }

  for (AbcObjectReader *reader : data->readers) {
    reader->decref();

    if (reader->refcount() == 0) {
      delete reader;
    }
  }

  WM_set_locked_interface(data->wm, false);

  switch (data->error_code) {
    default:
    case ABC_NO_ERROR:
      data->import_ok = !data->was_cancelled;
      break;
    case ABC_ARCHIVE_FAIL:
      WM_report(RPT_ERROR, "Could not open Alembic archive for reading! See console for detail.");
      break;
  }

  WM_main_add_notifier(NC_SCENE | ND_FRAME, data->scene);
  report_job_duration(data);
}

static void import_freejob(void *user_data)
{
  ImportJobData *data = static_cast<ImportJobData *>(user_data);
  delete data->archive;
  delete data;
}

bool ABC_import(bContext *C,
                const char *filepath,
                const AlembicImportParams *params,
                bool as_background_job)
{
  /* Using new here since MEM_* functions do not call constructor to properly initialize data. */
  ImportJobData *job = new ImportJobData();
  job->C = C;
  job->bmain = CTX_data_main(C);
  job->scene = CTX_data_scene(C);
  job->view_layer = CTX_data_view_layer(C);
  job->wm = CTX_wm_manager(C);
  job->import_ok = false;
  BLI_strncpy(job->filename, filepath, 1024);

  job->settings.scale = params->global_scale;
  job->settings.is_sequence = params->is_sequence;
  job->settings.set_frame_range = params->set_frame_range;
  job->settings.sequence_len = params->sequence_len;
  job->settings.sequence_offset = params->sequence_offset;
  job->settings.validate_meshes = params->validate_meshes;
  job->settings.always_add_cache_reader = params->always_add_cache_reader;
  job->error_code = ABC_NO_ERROR;
  job->was_cancelled = false;
  job->archive = nullptr;
  job->is_background_job = as_background_job;

  G.is_break = false;

  bool import_ok = false;
  if (as_background_job) {
    wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                                CTX_wm_window(C),
                                job->scene,
                                "Alembic Import",
                                WM_JOB_PROGRESS,
                                WM_JOB_TYPE_ALEMBIC);

    /* setup job */
    WM_jobs_customdata_set(wm_job, job, import_freejob);
    WM_jobs_timer(wm_job, 0.1, NC_SCENE | ND_FRAME, NC_SCENE | ND_FRAME);
    WM_jobs_callbacks(wm_job, import_startjob, nullptr, nullptr, import_endjob);

    WM_jobs_start(CTX_wm_manager(C), wm_job);
  }
  else {
    /* Fake a job context, so that we don't need NULL pointer checks while importing. */
    short stop = 0, do_update = 0;
    float progress = 0.0f;

    import_startjob(job, &stop, &do_update, &progress);
    import_endjob(job);
    import_ok = job->import_ok;

    import_freejob(job);
  }

  return import_ok;
}

/* ************************************************************************** */

void ABC_get_transform(CacheReader *reader, float r_mat_world[4][4], double time, float scale)
{
  if (!reader) {
    return;
  }

  AbcObjectReader *abc_reader = reinterpret_cast<AbcObjectReader *>(reader);

  bool is_constant = false;

  /* Convert from the local matrix we obtain from Alembic to world coordinates
   * for Blender. This conversion is done here rather than by Blender due to
   * work around the non-standard interpretation of CONSTRAINT_SPACE_LOCAL in
   * BKE_constraint_mat_convertspace(). */
  Object *object = abc_reader->object();
  if (object->parent == nullptr) {
    /* No parent, so local space is the same as world space. */
    abc_reader->read_matrix(r_mat_world, time, scale, is_constant);
    return;
  }

  float mat_parent[4][4];
  BKE_object_get_parent_matrix(object, object->parent, mat_parent);

  float mat_local[4][4];
  abc_reader->read_matrix(mat_local, time, scale, is_constant);
  mul_m4_m4m4(r_mat_world, mat_parent, object->parentinv);
  mul_m4_m4m4(r_mat_world, r_mat_world, mat_local);
}

/* ************************************************************************** */

static AbcObjectReader *get_abc_reader(CacheReader *reader, Object *ob, const char **err_str)
{
  AbcObjectReader *abc_reader = reinterpret_cast<AbcObjectReader *>(reader);
  IObject iobject = abc_reader->iobject();

  if (!iobject.valid()) {
    *err_str = "Invalid object: verify object path";
    return nullptr;
  }

  const ObjectHeader &header = iobject.getHeader();
  if (!abc_reader->accepts_object_type(header, ob, err_str)) {
    /* err_str is set by acceptsObjectType() */
    return nullptr;
  }

  return abc_reader;
}

static ISampleSelector sample_selector_for_time(chrono_t time)
{
  /* kFloorIndex is used to be compatible with non-interpolating
   * properties; they use the floor. */
  return ISampleSelector(time, ISampleSelector::kFloorIndex);
}

Mesh *ABC_read_mesh(CacheReader *reader,
                    Object *ob,
                    Mesh *existing_mesh,
                    const ABCReadParams *params,
                    const char **err_str)
{
  AbcObjectReader *abc_reader = get_abc_reader(reader, ob, err_str);
  if (abc_reader == nullptr) {
    return nullptr;
  }

  ISampleSelector sample_sel = sample_selector_for_time(params->time);
  return abc_reader->read_mesh(existing_mesh,
                               sample_sel,
                               params->read_flags,
                               params->velocity_name,
                               params->velocity_scale,
                               err_str);
}

bool ABC_mesh_topology_changed(CacheReader *reader,
                               Object *ob,
                               const Mesh *existing_mesh,
                               const double time,
                               const char **err_str)
{
  AbcObjectReader *abc_reader = get_abc_reader(reader, ob, err_str);
  if (abc_reader == nullptr) {
    return false;
  }

  ISampleSelector sample_sel = sample_selector_for_time(time);
  return abc_reader->topology_changed(existing_mesh, sample_sel);
}

/* ************************************************************************** */

void ABC_CacheReader_free(CacheReader *reader)
{
  AbcObjectReader *abc_reader = reinterpret_cast<AbcObjectReader *>(reader);
  abc_reader->decref();

  if (abc_reader->refcount() == 0) {
    delete abc_reader;
  }
}

void ABC_CacheReader_incref(CacheReader *reader)
{
  AbcObjectReader *abc_reader = reinterpret_cast<AbcObjectReader *>(reader);
  abc_reader->incref();
}

CacheReader *CacheReader_open_alembic_object(CacheArchiveHandle *handle,
                                             CacheReader *reader,
                                             Object *object,
                                             const char *object_path)
{
  if (object_path[0] == '\0') {
    return reader;
  }

  ArchiveReader *archive = archive_from_handle(handle);

  if (!archive || !archive->valid()) {
    return reader;
  }

  IObject iobject;
  find_iobject(archive->getTop(), iobject, object_path);

  if (reader) {
    ABC_CacheReader_free(reader);
  }

  ImportSettings settings;
  AbcObjectReader *abc_reader = create_reader(iobject, settings);
  if (abc_reader == nullptr) {
    /* This object is not supported */
    return nullptr;
  }
  abc_reader->object(object);
  abc_reader->incref();

  return reinterpret_cast<CacheReader *>(abc_reader);
}
