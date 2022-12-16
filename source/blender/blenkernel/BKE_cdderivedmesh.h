/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 * \section aboutcdderivedmesh CDDerivedMesh interface
 *   CDDerivedMesh (CD = Custom Data) is a DerivedMesh backend which stores
 *   mesh elements (vertices, edges and faces) as layers of custom element data.
 *
 * \note This is deprecated & should eventually be removed.
 */

#pragma once

#include "BKE_DerivedMesh.h"
#include "BKE_customdata.h"

#ifdef __cplusplus
extern "C" {
#endif

struct BMEditMesh;
struct CustomData_MeshMasks;
struct DerivedMesh;
struct MLoopNorSpaceArray;
struct Mesh;
struct Object;

/* creates a new CDDerivedMesh */
struct DerivedMesh *CDDM_new(int numVerts, int numEdges, int numFaces, int numLoops, int numPolys);

/* creates a CDDerivedMesh from the given Mesh, this will reference the
 * original data in Mesh, but it is safe to apply vertex coordinates or
 * calculate normals as those functions will automatically create new
 * data to not overwrite the original. */
struct DerivedMesh *CDDM_from_mesh(struct Mesh *mesh);

/* creates a CDDerivedMesh from the given Mesh with custom allocation type. */
struct DerivedMesh *cdDM_from_mesh_ex(struct Mesh *mesh,
                                      eCDAllocType alloctype,
                                      const struct CustomData_MeshMasks *mask);

/* merge verts  */
/* Enum for merge_mode of CDDM_merge_verts.
 * Refer to cdderivedmesh.c for details. */
enum {
  CDDM_MERGE_VERTS_DUMP_IF_MAPPED,
  CDDM_MERGE_VERTS_DUMP_IF_EQUAL,
};
#if 0
DerivedMesh *CDDM_merge_verts(DerivedMesh *dm,
                              const int *vtargetmap,
                              const int tot_vtargetmap,
                              const int merge_mode);
#endif

/* Copies the given DerivedMesh with verts, faces & edges stored as
 * custom element data. */
struct DerivedMesh *CDDM_copy(struct DerivedMesh *source);

/* creates a CDDerivedMesh with the same layer stack configuration as the
 * given DerivedMesh and containing the requested numbers of elements.
 * elements are initialized to all zeros
 */
struct DerivedMesh *CDDM_from_template(struct DerivedMesh *source,
                                       int numVerts,
                                       int numEdges,
                                       int numFaces,
                                       int numLoops,
                                       int numPolys);

/* recalculates vertex and face normals for a CDDerivedMesh
 */
void CDDM_calc_loop_normals(struct DerivedMesh *dm,
                            const bool use_split_normals,
                            const float split_angle);
void CDDM_calc_loop_normals_spacearr(struct DerivedMesh *dm,
                                     const bool use_split_normals,
                                     const float split_angle,
                                     struct MLoopNorSpaceArray *r_lnors_spacearr);

/* calculates edges for a CDDerivedMesh (from face data)
 * this completely replaces the current edge data in the DerivedMesh
 * builds edges from the tessellated face data.
 */
void CDDM_calc_edges_tessface(struct DerivedMesh *dm);

/* same as CDDM_calc_edges_tessface only makes edges from ngon faces instead of tessellation
 * faces*/
void CDDM_calc_edges(struct DerivedMesh *dm);

/* reconstitute face triangulation */
void CDDM_recalc_tessellation(struct DerivedMesh *dm, struct Mesh *me);
void CDDM_recalc_tessellation_ex(struct DerivedMesh *dm, struct Mesh *me, const bool do_face_nor_cpy);

/* UPBGE - Not static */
int mesh_tessface_calc(CustomData *fdata,
                       CustomData *ldata,
                       CustomData *pdata,
                       MVert *mvert,
                       int totface,
                       int totloop,
                       int totpoly);

/* lowers the number of vertices/edges/faces in a CDDerivedMesh
 * the layer data stays the same size
 */
void CDDM_lower_num_verts(struct DerivedMesh *dm, int numVerts);
void CDDM_lower_num_edges(struct DerivedMesh *dm, int numEdges);
void CDDM_lower_num_loops(struct DerivedMesh *dm, int numLoops);
void CDDM_lower_num_polys(struct DerivedMesh *dm, int numPolys);
void CDDM_lower_num_tessfaces(DerivedMesh *dm, int numTessFaces);

/* vertex/edge/face access functions
 * should always succeed if index is within bounds
 * note these return pointers - any change modifies the internals of the mesh
 */
struct MVert *CDDM_get_vert(struct DerivedMesh *dm, int index);
struct MEdge *CDDM_get_edge(struct DerivedMesh *dm, int index);
struct MFace *CDDM_get_tessface(struct DerivedMesh *dm, int index);
struct MLoop *CDDM_get_loop(struct DerivedMesh *dm, int index);
struct MPoly *CDDM_get_poly(struct DerivedMesh *dm, int index);

/* vertex/edge/face array access functions - return the array holding the
 * desired data
 * should always succeed
 * note these return pointers - any change modifies the internals of the mesh
 */
struct MVert *CDDM_get_verts(struct DerivedMesh *dm);
struct MEdge *CDDM_get_edges(struct DerivedMesh *dm);
struct MFace *CDDM_get_tessfaces(struct DerivedMesh *dm);
struct MLoop *CDDM_get_loops(struct DerivedMesh *dm);
struct MPoly *CDDM_get_polys(struct DerivedMesh *dm);

#ifdef __cplusplus
}
#endif
