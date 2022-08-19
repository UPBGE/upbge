/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */
#pragma once

/** \file
 * \ingroup bgpencil
 */
#include "gpencil_io_base.hh"

namespace blender::io::gpencil {

class GpencilExporter : public GpencilIO {

 public:
  GpencilExporter(const struct GpencilIOParams *iparams) : GpencilIO(iparams){};
  virtual bool write() = 0;

 protected:
 private:
};

}  // namespace blender::io::gpencil
