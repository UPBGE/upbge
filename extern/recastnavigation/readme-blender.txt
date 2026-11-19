Recast & Detour (Recast, Detour, DetourCrowd, DetourTileCache), from:
https://github.com/recastnavigation/recastnavigation

Version: the main branch at commit 9f4ce64458dfae86e1239c525ddc219c4e9e06f1 (2026-02-27),
after release 1.6.0 (2023-05-21) plus its bug fixes (UPBGE, 2026-11-18; before: Recast 1.5.0 and
Detour 1.4 from 2010).

The library's files are copied unchanged (Include and Source of the four modules, License.txt,
README.md). Never edit them: update by copying a newer version over them.

Build options (CMakeLists.txt, also given to every target that links the library):
  * DT_POLYREF64: 64-bit polygon references.
  * DT_VIRTUAL_QUERYFILTER: query filters with virtual passFilter / getCost.

Our own files:
  * recast-capi.cpp, recast-capi.h: a C interface to Recast for the editor's Build Navigation
    Mesh (source/blender/editors/mesh/mesh_navmesh.cc) and the game engine. It holds a copy of
    Recast's buildMeshAdjacency(), which the library keeps static.
  * CMakeLists.txt.
