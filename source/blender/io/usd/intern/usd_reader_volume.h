/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Tangent Animation. All rights reserved. */
#pragma once

#include "usd.h"
#include "usd_reader_xform.h"

#include "pxr/usd/usdVol/volume.h"

namespace blender::io::usd {

class USDVolumeReader : public USDXformReader {
 private:
  pxr::UsdVolVolume volume_;

 public:
  USDVolumeReader(const pxr::UsdPrim &prim,
                  const USDImportParams &import_params,
                  const ImportSettings &settings)
      : USDXformReader(prim, import_params, settings), volume_(prim)
  {
  }

  bool valid() const override
  {
    return static_cast<bool>(volume_);
  }

  void create_object(Main *bmain, double motionSampleTime) override;
  void read_object_data(Main *bmain, double motionSampleTime) override;
};

}  // namespace blender::io::usd
