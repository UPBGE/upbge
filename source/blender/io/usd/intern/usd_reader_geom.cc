/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Tangent Animation. All rights reserved. */

#include "usd_reader_geom.h"

#include "BKE_lib_id.h"
#include "BKE_modifier.h"
#include "BKE_object.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_geom.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_cachefile_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h" /* for FILE_MAX */

namespace blender::io::usd {

void USDGeomReader::add_cache_modifier()
{
  ModifierData *md = BKE_modifier_new(eModifierType_MeshSequenceCache);
  BLI_addtail(&object_->modifiers, md);

  MeshSeqCacheModifierData *mcmd = reinterpret_cast<MeshSeqCacheModifierData *>(md);

  mcmd->cache_file = settings_->cache_file;
  id_us_plus(&mcmd->cache_file->id);
  mcmd->read_flag = import_params_.mesh_read_flag;

  BLI_strncpy(mcmd->object_path, prim_.GetPath().GetString().c_str(), FILE_MAX);
}

void USDGeomReader::add_subdiv_modifier()
{
  ModifierData *md = BKE_modifier_new(eModifierType_Subsurf);
  BLI_addtail(&object_->modifiers, md);
}

}  // namespace blender::io::usd
