/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup obj
 */

#include <cstdio>
#include <memory>
#include <system_error>

#include "DNA_curve_enums.h"
#include "DNA_curve_types.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_collection_types.h"
#include "DNA_scene_types.h"

#include "ED_object.hh"

#include "obj_export_mesh.hh"
#include "obj_export_nurbs.hh"
#include "obj_exporter.hh"

#include "obj_export_file_writer.hh"

#include "CLG_log.h"
static CLG_LogRef LOG = {"io.obj"};

namespace blender::io::obj {

OBJDepsgraph::OBJDepsgraph(const bContext *C,
                           const eEvaluationMode eval_mode,
                           Collection *collection)
{
  Scene *scene = CTX_data_scene(C);
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  /* If a collection was provided, use it. */
  if (collection) {
    depsgraph_ = DEG_graph_new(bmain, scene, view_layer, eval_mode);
    needs_free_ = true;
    DEG_graph_build_from_collection(depsgraph_, collection);
    BKE_scene_graph_evaluated_ensure(depsgraph_, bmain);
  }
  else if (eval_mode == DAG_EVAL_RENDER) {
    depsgraph_ = DEG_graph_new(bmain, scene, view_layer, eval_mode);
    needs_free_ = true;
    DEG_graph_build_for_all_objects(depsgraph_);
    BKE_scene_graph_evaluated_ensure(depsgraph_, bmain);
  }
  else {
    depsgraph_ = CTX_data_ensure_evaluated_depsgraph(C);
    needs_free_ = false;
  }
}

OBJDepsgraph::~OBJDepsgraph()
{
  if (needs_free_) {
    DEG_graph_free(depsgraph_);
  }
}

Depsgraph *OBJDepsgraph::get()
{
  return depsgraph_;
}

void OBJDepsgraph::update_for_newframe()
{
  BKE_scene_graph_update_for_newframe(depsgraph_);
}

static void print_exception_error(const std::system_error &ex)
{
  CLOG_ERROR(&LOG, "[%s] %s", ex.code().category().name(), ex.what());
}

static bool is_curve_nurbs_compatible(const Nurb *nurb)
{
  while (nurb) {
    if (nurb->type == CU_BEZIER || nurb->pntsv != 1) {
      return false;
    }
    nurb = nurb->next;
  }
  return true;
}

/**
 * Filter supported objects from the Scene.
 *
 * \note Curves are also stored with Meshes if export settings specify so.
 */
std::pair<Vector<std::unique_ptr<OBJMesh>>, Vector<std::unique_ptr<IOBJCurve>>>
filter_supported_objects(Depsgraph *depsgraph, const OBJExportParams &export_params)
{
  Vector<std::unique_ptr<OBJMesh>> r_exportable_meshes;
  Vector<std::unique_ptr<IOBJCurve>> r_exportable_nurbs;
  DEGObjectIterSettings deg_iter_settings{};
  deg_iter_settings.depsgraph = depsgraph;
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET | DEG_ITER_OBJECT_FLAG_VISIBLE |
                            DEG_ITER_OBJECT_FLAG_DUPLI;
  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, object) {
    if (export_params.export_selected_objects && !(object->base_flag & BASE_SELECTED)) {
      continue;
    }
    switch (object->type) {
      case OB_SURF:
        /* Evaluated surface objects appear as mesh objects from the iterator. */
        break;
      case OB_MESH:
        r_exportable_meshes.append(std::make_unique<OBJMesh>(depsgraph, export_params, object));
        break;
      case OB_CURVES_LEGACY: {
        Curve *curve = static_cast<Curve *>(object->data);
        Nurb *nurb{static_cast<Nurb *>(curve->nurb.first)};
        if (!nurb) {
          /* An empty curve. Not yet supported to export these as meshes. */
          if (export_params.export_curves_as_nurbs) {
            IOBJCurve *obj_curve = new OBJLegacyCurve(depsgraph, object);
            r_exportable_nurbs.append(std::unique_ptr<IOBJCurve>(obj_curve));
          }
          break;
        }
        if (export_params.export_curves_as_nurbs && is_curve_nurbs_compatible(nurb)) {
          /* Export in parameter form: control points. */
          IOBJCurve *obj_curve = new OBJLegacyCurve(depsgraph, object);
          r_exportable_nurbs.append(std::unique_ptr<IOBJCurve>(obj_curve));
        }
        else {
          /* Export in mesh form: edges and vertices. */
          r_exportable_meshes.append(std::make_unique<OBJMesh>(depsgraph, export_params, object));
        }
        break;
      }
      default:
        /* Other object types are not supported. */
        break;
    }
  }
  DEG_OBJECT_ITER_END;
  return {std::move(r_exportable_meshes), std::move(r_exportable_nurbs)};
}

static void write_mesh_objects(const Span<std::unique_ptr<OBJMesh>> exportable_as_mesh,
                               OBJWriter &obj_writer,
                               MTLWriter *mtl_writer,
                               const OBJExportParams &export_params)
{
  /* Parallelization is over meshes/objects, which means
   * we have to have the output text buffer for each object,
   * and write them all into the file at the end. */
  size_t count = exportable_as_mesh.size();
  Array<FormatHandler> buffers(count);

  /* Serial: gather material indices, ensure normals & edges. */
  Vector<Vector<int>> mtlindices;
  if (mtl_writer) {
    if (export_params.export_materials) {
      obj_writer.write_mtllib_name(mtl_writer->mtl_file_path());
    }
    mtlindices.reserve(count);
  }
  for (const auto &obj_mesh : exportable_as_mesh) {
    OBJMesh &obj = *obj_mesh;
    if (mtl_writer) {
      mtlindices.append(mtl_writer->add_materials(obj));
    }
  }

  /* Parallel over meshes: store normal coords & indices, uv coords and indices. */
  threading::parallel_for(IndexRange(count), 1, [&](IndexRange range) {
    for (const int i : range) {
      OBJMesh &obj = *exportable_as_mesh[i];
      if (export_params.export_normals) {
        obj.store_normal_coords_and_indices();
      }
      if (export_params.export_uv) {
        obj.store_uv_coords_and_indices();
      }
    }
  });

  /* Serial: calculate index offsets; these are sequentially added
   * over all meshes, and requite normal/uv indices to be calculated. */
  Vector<IndexOffsets> index_offsets;
  index_offsets.reserve(count);
  IndexOffsets offsets{0, 0, 0};
  for (const auto &obj_mesh : exportable_as_mesh) {
    OBJMesh &obj = *obj_mesh;
    index_offsets.append(offsets);
    offsets.vertex_offset += obj.tot_vertices();
    offsets.uv_vertex_offset += obj.tot_uv_vertices();
    offsets.normal_offset += obj.get_normal_coords().size();
  }

  /* Parallel over meshes: main result writing. */
  threading::parallel_for(IndexRange(count), 1, [&](IndexRange range) {
    for (const int i : range) {
      OBJMesh &obj = *exportable_as_mesh[i];
      auto &fh = buffers[i];

      obj_writer.write_object_name(fh, obj);
      obj_writer.write_vertex_coords(fh, obj, export_params.export_colors);

      if (obj.tot_faces() > 0) {
        if (export_params.export_smooth_groups) {
          obj.calc_smooth_groups(export_params.smooth_groups_bitflags);
        }
        if (export_params.export_materials) {
          obj.calc_face_order();
        }
        if (export_params.export_normals) {
          obj_writer.write_normals(fh, obj);
        }
        if (export_params.export_uv) {
          obj_writer.write_uv_coords(fh, obj);
        }
        /* This function takes a 0-indexed slot index for the obj_mesh object and
         * returns the material name that we are using in the `.obj` file for it. */
        const auto *obj_mtlindices = mtlindices.is_empty() ? nullptr : &mtlindices[i];
        auto matname_fn = [&](int s) -> const char * {
          if (!obj_mtlindices || s < 0 || s >= obj_mtlindices->size()) {
            return nullptr;
          }
          return mtl_writer->mtlmaterial_name((*obj_mtlindices)[s]);
        };
        obj_writer.write_face_elements(fh, index_offsets[i], obj, matname_fn);
      }
      obj_writer.write_edges_indices(fh, index_offsets[i], obj);

      /* Nothing will need this object's data after this point, release
       * various arrays here. */
      obj.clear();
    }
  });

  /* Write all the object text buffers into the output file. */
  FILE *f = obj_writer.get_outfile();
  for (auto &b : buffers) {
    b.write_to_file(f);
  }
}

/**
 * Export NURBS Curves in parameter form, not as vertices and edges.
 */
static void write_nurbs_curve_objects(const Span<std::unique_ptr<IOBJCurve>> exportable_as_nurbs,
                                      const OBJWriter &obj_writer)
{
  FormatHandler fh;
  /* #OBJCurve doesn't have any dynamically allocated memory, so it's fine
   * to wait for #blender::Vector to clean the objects up. */
  for (const std::unique_ptr<IOBJCurve> &obj_curve : exportable_as_nurbs) {
    obj_writer.write_nurbs_curve(fh, *obj_curve);
  }
  fh.write_to_file(obj_writer.get_outfile());
}

static bool open_stream_writers(const OBJExportParams &export_params,
                                const char *filepath,
                                std::unique_ptr<OBJWriter> &r_frame_writer,
                                std::unique_ptr<MTLWriter> &r_mtl_writer)
{
  try {
    r_frame_writer = std::make_unique<OBJWriter>(filepath, export_params);
  }
  catch (const std::system_error &ex) {
    print_exception_error(ex);
    BKE_reportf(export_params.reports, RPT_ERROR, "OBJ Export: Cannot open file '%s'", filepath);
    return false;
  }
  if (!r_frame_writer) {
    BLI_assert_msg(false, "File should be writable by now.");
    return false;
  }
  if (export_params.export_materials || export_params.export_material_groups) {
    try {
      r_mtl_writer = std::make_unique<MTLWriter>(filepath, export_params.export_materials);
    }
    catch (const std::system_error &ex) {
      print_exception_error(ex);
      BKE_reportf(export_params.reports,
                  RPT_WARNING,
                  "OBJ Export: Cannot create mtl file for '%s'",
                  filepath);
    }
  }
  return true;
}

static void write_materials(MTLWriter *mtl_writer, const OBJExportParams &export_params)
{
  BLI_assert(mtl_writer);
  mtl_writer->write_header(export_params.blen_filepath);
  char dest_dir[FILE_MAX];
  if (export_params.file_base_for_tests[0] == '\0') {
    BLI_path_split_dir_part(export_params.filepath, dest_dir, sizeof(dest_dir));
  }
  else {
    STRNCPY(dest_dir, export_params.file_base_for_tests);
  }
  BLI_path_slash_native(dest_dir);
  BLI_path_normalize(dest_dir);
  mtl_writer->write_materials(export_params.blen_filepath,
                              export_params.path_mode,
                              dest_dir,
                              export_params.export_pbr_extensions);
}

void export_objects(const OBJExportParams &export_params,
                    const Span<std::unique_ptr<OBJMesh>> meshes,
                    const Span<std::unique_ptr<IOBJCurve>> curves,
                    const char *filepath)
{
  /* Open */
  std::unique_ptr<OBJWriter> obj_writer;
  std::unique_ptr<MTLWriter> mtl_writer;
  if (!open_stream_writers(export_params, filepath, obj_writer, mtl_writer)) {
    return;
  }

  /* Write */
  obj_writer->write_header();
  write_mesh_objects(meshes, *obj_writer, mtl_writer.get(), export_params);
  write_nurbs_curve_objects(curves, *obj_writer);
  if (mtl_writer && export_params.export_materials) {
    write_materials(mtl_writer.get(), export_params);
  }
}

void export_frame(Depsgraph *depsgraph, const OBJExportParams &export_params, const char *filepath)
{
  auto [exportable_as_mesh, exportable_as_nurbs] = filter_supported_objects(depsgraph,
                                                                            export_params);

  if (exportable_as_mesh.size() == 0 && exportable_as_nurbs.size() == 0) {
    BKE_reportf(export_params.reports, RPT_WARNING, "OBJ Export: No information to write");
    return;
  }

  export_objects(export_params, exportable_as_mesh, exportable_as_nurbs, filepath);
}

bool append_frame_to_filename(const char *filepath,
                              const int frame,
                              char r_filepath_with_frames[1024])
{
  BLI_strncpy(r_filepath_with_frames, filepath, FILE_MAX);
  BLI_path_extension_strip(r_filepath_with_frames);
  BLI_path_frame(r_filepath_with_frames, FILE_MAX, frame, 4);
  return BLI_path_extension_replace(r_filepath_with_frames, FILE_MAX, ".obj");
}

void exporter_main(bContext *C, const OBJExportParams &export_params)
{
  ed::object::mode_set(C, OB_MODE_OBJECT);

  Collection *collection = nullptr;
  if (export_params.collection[0]) {
    Main *bmain = CTX_data_main(C);
    collection = reinterpret_cast<Collection *>(
        BKE_libblock_find_name(bmain, ID_GR, export_params.collection));
    if (!collection) {
      BKE_reportf(export_params.reports,
                  RPT_ERROR,
                  "OBJ Export: Unable to find collection '%s'",
                  export_params.collection);
      return;
    }
  }

  OBJDepsgraph obj_depsgraph(C, export_params.export_eval_mode, collection);
  Scene *scene = DEG_get_input_scene(obj_depsgraph.get());
  const char *filepath = export_params.filepath;

  /* Single frame export, i.e. no animation. */
  if (!export_params.export_animation) {
    fmt::println("Writing to {}", filepath);
    export_frame(obj_depsgraph.get(), export_params, filepath);
    return;
  }

  char filepath_with_frames[FILE_MAX];
  /* Used to reset the Scene to its original state. */
  const int original_frame = scene->r.cfra;

  for (int frame = export_params.start_frame; frame <= export_params.end_frame; frame++) {
    const bool filepath_ok = append_frame_to_filename(filepath, frame, filepath_with_frames);
    if (!filepath_ok) {
      CLOG_ERROR(&LOG, "File Path too long: %s", filepath_with_frames);
      return;
    }

    scene->r.cfra = frame;
    obj_depsgraph.update_for_newframe();
    fmt::println("Writing to {}", filepath_with_frames);
    export_frame(obj_depsgraph.get(), export_params, filepath_with_frames);
  }
  scene->r.cfra = original_frame;
}
}  // namespace blender::io::obj
