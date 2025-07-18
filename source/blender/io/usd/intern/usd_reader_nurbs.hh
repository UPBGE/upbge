/* SPDX-FileCopyrightText: 2021 Tangent Animation. All rights reserved.
 * SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from the Blender Alembic importer implementation. */

#pragma once

#include "usd.hh"
#include "usd_reader_geom.hh"

#include <pxr/usd/usdGeom/nurbsCurves.h>

struct Curve;

namespace blender::io::usd {

class USDNurbsReader : public USDGeomReader {
 protected:
  pxr::UsdGeomNurbsCurves curve_prim_;

 public:
  USDNurbsReader(const pxr::UsdPrim &prim,
                 const USDImportParams &import_params,
                 const ImportSettings &settings)
      : USDGeomReader(prim, import_params, settings), curve_prim_(prim)
  {
  }

  bool valid() const override
  {
    return bool(curve_prim_);
  }

  void create_object(Main *bmain) override;
  void read_object_data(Main *bmain, pxr::UsdTimeCode time) override;

  void read_curve_sample(Curve *cu, pxr::UsdTimeCode time);

  void read_geometry(bke::GeometrySet &geometry_set,
                     USDMeshReadParams params,
                     const char **r_err_str) override;

 private:
  Mesh *read_mesh(struct Mesh *existing_mesh, USDMeshReadParams params, const char **r_err_str);
};

}  // namespace blender::io::usd
