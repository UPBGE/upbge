/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2019 Blender Foundation. All rights reserved. */
#pragma once

#include "usd_writer_abstract.h"

#include <pxr/usd/usdGeom/mesh.h>

namespace blender::io::usd {

struct USDMeshData;

/* Writer for USD geometry. Does not assume the object is a mesh object. */
class USDGenericMeshWriter : public USDAbstractWriter {
 public:
  USDGenericMeshWriter(const USDExporterContext &ctx);

 protected:
  virtual bool is_supported(const HierarchyContext *context) const override;
  virtual void do_write(HierarchyContext &context) override;

  virtual Mesh *get_export_mesh(Object *object_eval, bool &r_needsfree) = 0;
  virtual void free_export_mesh(Mesh *mesh);

 private:
  /* Mapping from material slot number to array of face indices with that material. */
  typedef std::map<short, pxr::VtIntArray> MaterialFaceGroups;

  void write_mesh(HierarchyContext &context, Mesh *mesh);
  void get_geometry_data(const Mesh *mesh, struct USDMeshData &usd_mesh_data);
  void assign_materials(const HierarchyContext &context,
                        pxr::UsdGeomMesh usd_mesh,
                        const MaterialFaceGroups &usd_face_groups);
  void write_uv_maps(const Mesh *mesh, pxr::UsdGeomMesh usd_mesh);
  void write_normals(const Mesh *mesh, pxr::UsdGeomMesh usd_mesh);
  void write_surface_velocity(const Mesh *mesh, pxr::UsdGeomMesh usd_mesh);
};

class USDMeshWriter : public USDGenericMeshWriter {
 public:
  USDMeshWriter(const USDExporterContext &ctx);

 protected:
  virtual Mesh *get_export_mesh(Object *object_eval, bool &r_needsfree) override;
};

}  // namespace blender::io::usd
