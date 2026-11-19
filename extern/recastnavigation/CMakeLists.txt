# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# The Original Code is Copyright (C) 2006, Blender Foundation
# All rights reserved.
#
# The Original Code is: all of this file.
#
# Contributor(s): Daniel Genrich
#
# ***** END GPL LICENSE BLOCK *****

remove_c_and_cxx_flag_unsigned_char()

set(INC
	Recast/Include
	Detour/Include
	DetourCrowd/Include
	DetourTileCache/Include
)

set(INC_SYS

)

set(SRC
	recast-capi.cpp
	recast-capi.h

	Recast/Source/Recast.cpp
	Recast/Source/RecastAlloc.cpp
	Recast/Source/RecastArea.cpp
	Recast/Source/RecastAssert.cpp
	Recast/Source/RecastContour.cpp
	Recast/Source/RecastFilter.cpp
	Recast/Source/RecastLayers.cpp
	Recast/Source/RecastMesh.cpp
	Recast/Source/RecastMeshDetail.cpp
	Recast/Source/RecastRasterization.cpp
	Recast/Source/RecastRegion.cpp
	Recast/Include/Recast.h
	Recast/Include/RecastAlloc.h
	Recast/Include/RecastAssert.h

	Detour/Source/DetourAlloc.cpp
	Detour/Source/DetourAssert.cpp
	Detour/Source/DetourCommon.cpp
	Detour/Source/DetourNavMesh.cpp
	Detour/Source/DetourNavMeshBuilder.cpp
	Detour/Source/DetourNavMeshQuery.cpp
	Detour/Source/DetourNode.cpp
	Detour/Include/DetourAlloc.h
	Detour/Include/DetourAssert.h
	Detour/Include/DetourCommon.h
	Detour/Include/DetourMath.h
	Detour/Include/DetourNavMesh.h
	Detour/Include/DetourNavMeshBuilder.h
	Detour/Include/DetourNavMeshQuery.h
	Detour/Include/DetourNode.h
	Detour/Include/DetourStatus.h

	DetourCrowd/Source/DetourCrowd.cpp
	DetourCrowd/Source/DetourLocalBoundary.cpp
	DetourCrowd/Source/DetourObstacleAvoidance.cpp
	DetourCrowd/Source/DetourPathCorridor.cpp
	DetourCrowd/Source/DetourPathQueue.cpp
	DetourCrowd/Source/DetourProximityGrid.cpp
	DetourCrowd/Include/DetourCrowd.h
	DetourCrowd/Include/DetourLocalBoundary.h
	DetourCrowd/Include/DetourObstacleAvoidance.h
	DetourCrowd/Include/DetourPathCorridor.h
	DetourCrowd/Include/DetourPathQueue.h
	DetourCrowd/Include/DetourProximityGrid.h

	DetourTileCache/Source/DetourTileCache.cpp
	DetourTileCache/Source/DetourTileCacheBuilder.cpp
	DetourTileCache/Include/DetourTileCache.h
	DetourTileCache/Include/DetourTileCacheBuilder.h
)

set(LIB
)

# UPBGE (2026-11-18): 64-bit polygon references (room for big and endless worlds) and query
# filters with virtual costs (closed areas, crowded polygons). Every file that includes a Detour
# header must see both: the engine's navigation code checks them (KX_Detour.h).
add_definitions(-DDT_POLYREF64 -DDT_VIRTUAL_QUERYFILTER)

blender_add_lib(extern_recastnavigation "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
target_compile_definitions(extern_recastnavigation INTERFACE DT_POLYREF64 DT_VIRTUAL_QUERYFILTER)
