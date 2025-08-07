/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The \link edmesh EDBM module \endlink is for editmode bmesh stuff.
 * In contrast, this module is for code shared with blenkernel that's
 * only concerned with low level operations on the #BMEditMesh structure.
 */

#include <array>

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"

#include "bmesh.hh"

struct BMLoop;
struct BMPartialUpdate;
struct BMesh;
struct BMeshCalcTessellation_Params;
struct Depsgraph;
struct Mesh;
struct Object;
struct Scene;

/**
 * This structure is used for mesh edit-mode.
 *
 * Through this, you get access to both the edit #BMesh, its tessellation,
 * and various data that doesn't belong in the #BMesh struct itself
 * (mostly related to mesh evaluation).
 *
 * #Mesh.runtime.edit_mesh stores a pointer to this structure.
 */
struct BMEditMesh {
  /* Always owned by an original mesh in edit mode. */
  BMesh *bm;

  /**
   * Face triangulation (tessellation) is stored as triplets of three loops,
   * which each define a triangle.
   *
   * \see #Mesh::corner_tris() as the documentation gives useful hints that apply to this data too.
   */
  blender::Array<std::array<BMLoop *, 3>> looptris;

  /** Selection mode (#SCE_SELECT_VERTEX, #SCE_SELECT_EDGE & #SCE_SELECT_FACE). */
  short selectmode;
  /** The active material (assigned to newly created faces). */
  short mat_nr;

  /** Temp variables for x-mirror editing (-1 when the layer does not exist). */
  int mirror_cdlayer;

  /**
   * ID data is older than edit-mode data.
   * Set #Main.is_memfile_undo_flush_needed when enabling.
   */
  char needs_flush_to_id;
};

/* editmesh.cc */

void BKE_editmesh_looptris_calc_ex(BMEditMesh *em, const BMeshCalcTessellation_Params *params);
void BKE_editmesh_looptris_calc(BMEditMesh *em);
void BKE_editmesh_looptris_calc_with_partial_ex(BMEditMesh *em,
                                                BMPartialUpdate *bmpinfo,
                                                const BMeshCalcTessellation_Params *params);
void BKE_editmesh_looptris_calc_with_partial(BMEditMesh *em, BMPartialUpdate *bmpinfo);
void BKE_editmesh_looptris_and_normals_calc_with_partial(BMEditMesh *em, BMPartialUpdate *bmpinfo);

/**
 * Performing the face normal calculation at the same time as tessellation
 * gives a reasonable performance boost (approx ~20% faster).
 */
void BKE_editmesh_looptris_and_normals_calc(BMEditMesh *em);

/**
 * \note The caller is responsible for ensuring triangulation data,
 * typically by calling #BKE_editmesh_looptris_calc.
 */
BMEditMesh *BKE_editmesh_create(BMesh *bm);
BMEditMesh *BKE_editmesh_copy(BMEditMesh *em);
/**
 * \brief Return the #BMEditMesh for a given object
 *
 * \note this function assumes this is a mesh object,
 * don't add NULL data check here. caller must do that
 */
BMEditMesh *BKE_editmesh_from_object(Object *ob);

/**
 * Return whether the evaluated mesh is a "descendant" of the original mesh: whether it is a
 * version of the original mesh propagated during evaluation. This will be false if the mesh was
 * taken from an different object during evaluation, with the object info node for example.
 */
bool BKE_editmesh_eval_orig_map_available(const Mesh &mesh_eval, const Mesh *mesh_orig);

/**
 * \note Does not free the #BMEditMesh itself.
 */
void BKE_editmesh_free_data(BMEditMesh *em);

blender::Array<blender::float3> BKE_editmesh_vert_coords_alloc(Depsgraph *depsgraph,
                                                               BMEditMesh *em,
                                                               Scene *scene,
                                                               Object *ob);
blender::Array<blender::float3> BKE_editmesh_vert_coords_alloc_orco(BMEditMesh *em);
blender::Span<blender::float3> BKE_editmesh_vert_coords_when_deformed(
    Depsgraph *depsgraph,
    BMEditMesh *em,
    Scene *scene,
    Object *obedit,
    blender::Array<blender::float3> &r_alloc);

void BKE_editmesh_lnorspace_update(BMEditMesh *em);
