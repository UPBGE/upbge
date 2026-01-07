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
  BulletSoftBody *bsb = MEM_new<BulletSoftBody>("bulletsoftbody");

  bsb->flag = OB_BSB_BENDING_CONSTRAINTS | OB_BSB_SHAPE_MATCHING | OB_BSB_AERO_VPOINT;
  bsb->linStiff = 0.5f;
  bsb->angStiff = 1.0f;
  bsb->volume = 1.0f;

  bsb->viterations = 0;
  bsb->piterations = 2;
  bsb->diterations = 0;
  bsb->citerations = 4;

  bsb->kSRHR_CL = 0.1f;
  bsb->kSKHR_CL = 1.f;
  bsb->kSSHR_CL = 0.5f;
  bsb->kSR_SPLT_CL = 0.5f;

  bsb->kSK_SPLT_CL = 0.5f;
  bsb->kSS_SPLT_CL = 0.5f;
  bsb->kVCF = 1;
  bsb->kDP = 0;

  bsb->kDG = 0;
  bsb->kLF = 0;
  bsb->kPR = 0;
  bsb->kVC = 0;

  bsb->kDF = 0.2f;
  bsb->kMT = 0.05;
  bsb->kCHR = 1.0f;
  bsb->kKHR = 0.1f;

  bsb->kSHR = 1.0f;
  bsb->kAHR = 0.7f;

  bsb->collisionflags = 0;
  bsb->numclusteriterations = 64;
  bsb->bending_dist = 2;
  bsb->welding = 0.f;

  return bsb;
}

void bsbFree(BulletSoftBody *bsb)
{
  MEM_delete(bsb);
}

}  // extern "C"

}  // namespace blender
