/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_Detour.h
 *  \ingroup ketsji
 *  The engine's only way to include Recast / Detour. The library is built with 64-bit polygon
 *  references and virtual query filters (extern/recastnavigation/CMakeLists.txt): a file that saw
 *  the headers without them would lay their classes out differently.
 */

#pragma once

#if !defined(DT_POLYREF64) || !defined(DT_VIRTUAL_QUERYFILTER)
#  error "Detour needs DT_POLYREF64 and DT_VIRTUAL_QUERYFILTER (link extern_recastnavigation)"
#endif

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourPathCorridor.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"
#include "Recast.h"
