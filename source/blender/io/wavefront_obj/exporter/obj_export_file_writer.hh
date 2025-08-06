/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup obj
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "IO_wavefront_obj.hh"
#include "obj_export_io.hh"
#include "obj_export_mesh.hh"
#include "obj_export_mtl.hh"

namespace blender::io::obj {

class IOBJCurve;
class OBJMesh;
/**
 * Total vertices/ UV vertices/ normals of previous Objects
 * should be added to the current Object's indices.
 */
struct IndexOffsets {
  int vertex_offset;
  int uv_vertex_offset;
  int normal_offset;
};

/**
 * Responsible for writing a `.OBJ` file.
 */
class OBJWriter : NonMovable, NonCopyable {
 private:
  const OBJExportParams &export_params_;
  std::string outfile_path_;
  FILE *outfile_;

 public:
  OBJWriter(const char *filepath, const OBJExportParams &export_params) noexcept(false);
  ~OBJWriter();

  FILE *get_outfile() const
  {
    return outfile_;
  }

  void write_header() const;

  /**
   * Write object's name or group.
   */
  void write_object_name(FormatHandler &fh, const OBJMesh &obj_mesh_data) const;
  /**
   * Write file name of Material Library in `.OBJ` file.
   */
  void write_mtllib_name(StringRefNull mtl_filepath) const;
  /**
   * Write vertex coordinates for all vertices as "v x y z" or "v x y z r g b".
   */
  void write_vertex_coords(FormatHandler &fh,
                           const OBJMesh &obj_mesh_data,
                           bool write_colors) const;
  /**
   * Write UV vertex coordinates for all vertices as `vt u v`.
   * \note UV indices are stored here, but written with faces later.
   */
  void write_uv_coords(FormatHandler &fh, OBJMesh &obj_mesh_data) const;
  /**
   * Write corner normals for smooth-shaded faces, and face normals otherwise, as "vn x y z".
   * \note Normal indices ares stored here, but written with faces later.
   */
  void write_normals(FormatHandler &fh, OBJMesh &obj_mesh_data);
  /**
   * Write face elements with at least vertex indices, and conditionally with UV vertex
   * indices and face normal indices. Also write groups: smooth, vertex, material.
   * The matname_fn turns a 0-indexed material slot number in an Object into the
   * name used in the `.obj` file.
   * \note UV indices were stored while writing UV vertices.
   */
  void write_face_elements(FormatHandler &fh,
                           const IndexOffsets &offsets,
                           const OBJMesh &obj_mesh_data,
                           FunctionRef<const char *(int)> matname_fn);
  /**
   * Write loose edges of a mesh as "l v1 v2".
   */
  void write_edges_indices(FormatHandler &fh,
                           const IndexOffsets &offsets,
                           const OBJMesh &obj_mesh_data) const;
  /**
   * Write a NURBS curve to the `.OBJ` file in parameter form.
   */
  void write_nurbs_curve(FormatHandler &fh, const IOBJCurve &obj_nurbs_data) const;

 private:
  using func_vert_uv_normal_indices = void (OBJWriter::*)(FormatHandler &fh,
                                                          const IndexOffsets &offsets,
                                                          Span<int> vert_indices,
                                                          Span<int> uv_indices,
                                                          Span<int> normal_indices,
                                                          bool flip) const;
  /**
   * \return Writer function with appropriate face-element syntax.
   */
  func_vert_uv_normal_indices get_face_element_writer(int total_uv_vertices) const;

  /**
   * Write one line of face indices as "f v1/vt1/vn1 v2/vt2/vn2 ...".
   */
  void write_vert_uv_normal_indices(FormatHandler &fh,
                                    const IndexOffsets &offsets,
                                    Span<int> vert_indices,
                                    Span<int> uv_indices,
                                    Span<int> normal_indices,
                                    bool flip) const;
  /**
   * Write one line of face indices as "f v1//vn1 v2//vn2 ...".
   */
  void write_vert_normal_indices(FormatHandler &fh,
                                 const IndexOffsets &offsets,
                                 Span<int> vert_indices,
                                 Span<int> /*uv_indices*/,
                                 Span<int> normal_indices,
                                 bool flip) const;
  /**
   * Write one line of face indices as "f v1/vt1 v2/vt2 ...".
   */
  void write_vert_uv_indices(FormatHandler &fh,
                             const IndexOffsets &offsets,
                             Span<int> vert_indices,
                             Span<int> uv_indices,
                             Span<int> /*normal_indices*/,
                             bool flip) const;
  /**
   * Write one line of face indices as "f v1 v2 ...".
   */
  void write_vert_indices(FormatHandler &fh,
                          const IndexOffsets &offsets,
                          Span<int> vert_indices,
                          Span<int> /*uv_indices*/,
                          Span<int> /*normal_indices*/,
                          bool flip) const;
};

/**
 * Responsible for writing a `.MTL` file.
 */
class MTLWriter : NonMovable, NonCopyable {
 private:
  FormatHandler fmt_handler_;
  FILE *outfile_ = nullptr;
  std::string mtl_filepath_;
  Vector<MTLMaterial> mtlmaterials_;
  /* Map from a Material* to an index into mtlmaterials_. */
  Map<const Material *, int> material_map_;

 public:
  /*
   * Create the `.MTL` file.
   */
  MTLWriter(const char *obj_filepath, bool write_file) noexcept(false);
  ~MTLWriter();

  void write_header(const char *blen_filepath);
  /**
   * Write all of the material specifications to the MTL file.
   * For consistency of output from run to run (useful for testing),
   * the materials are sorted by name before writing.
   */
  void write_materials(const char *blen_filepath,
                       ePathReferenceMode path_mode,
                       const char *dest_dir,
                       bool write_pbr);
  StringRefNull mtl_file_path() const;
  /**
   * Add the materials of the given object to #MTLWriter, de-duplicating
   * against ones that are already there.
   * Return a Vector of indices into mtlmaterials_ that hold the #MTLMaterial
   * that corresponds to each material slot, in order, of the given Object.
   * Indexes are returned rather than pointers to the MTLMaterials themselves
   * because the mtlmaterials_ Vector may move around when resized.
   */
  Vector<int> add_materials(const OBJMesh &mesh_to_export);
  const char *mtlmaterial_name(int index);

 private:
  /**
   * Write properties sourced from p-BSDF node or #Object.Material.
   */
  void write_bsdf_properties(const MTLMaterial &mtl_material, bool write_pbr);
  /**
   * Write a texture map in the form "map_XX -s 1. 1. 1. -o 0. 0. 0. [-bm 1.] path/to/image".
   */
  void write_texture_map(const MTLMaterial &mtl_material,
                         MTLTexMapType texture_key,
                         const MTLTexMap &texture_map,
                         const char *blen_filedir,
                         const char *dest_dir,
                         ePathReferenceMode mode,
                         Set<std::pair<std::string, std::string>> &copy_set);
};
}  // namespace blender::io::obj
