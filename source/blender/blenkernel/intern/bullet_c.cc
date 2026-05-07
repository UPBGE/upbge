/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"
#include "BKE_bullet.h"
#include "DNA_object_force_types.h"

namespace blender {

extern "C" {

BulletSoftBody *bsbNew()
{
  return MEM_new<BulletSoftBody>("bulletsoftbody");
}

void bsbFree(BulletSoftBody *bsb)
{
  MEM_delete(bsb);
}

}  // extern "C"

}  // namespace blender
