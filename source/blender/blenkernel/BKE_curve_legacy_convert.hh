/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BKE_curves.hh"

struct Curve;
struct Curves;

namespace blender::bke {

Curves *curve_legacy_to_curves(const Curve &curve_legacy);
Curves *curve_legacy_to_curves(const Curve &curve_legacy, const ListBase &nurbs_list);

}  // namespace blender::bke
