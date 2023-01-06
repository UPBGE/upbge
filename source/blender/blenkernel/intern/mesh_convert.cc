/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "DNA_curve_types.h"
#include "DNA_key_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_meta_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"

#include "BLI_edgehash.h"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_DerivedMesh.h"
#include "BKE_curves.hh"
#include "BKE_deform.h"
#include "BKE_displist.h"
#include "BKE_editmesh.h"
#include "BKE_geometry_set.hh"
#include "BKE_key.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mball.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
/* these 2 are only used by conversion functions */
#include "BKE_curve.h"
/* -- */
#include "BKE_object.h"
/* -- */
#include "BKE_pointcloud.h"

#include "BKE_curve_to_mesh.hh"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

using blender::float3;
using blender::IndexRange;
using blender::MutableSpan;
using blender::Span;
using blender::StringRefNull;

/* Define for cases when you want extra validation of mesh
 * after certain modifications.
 */
// #undef VALIDATE_MESH

#ifdef VALIDATE_MESH
#  define ASSERT_IS_VALID_MESH(mesh) \
    (BLI_assert((mesh == nullptr) || (BKE_mesh_is_valid(mesh) == true)))
#else
#  define ASSERT_IS_VALID_MESH(mesh)
#endif

static CLG_LogRef LOG = {"bke.mesh_convert"};

/**
 * Specialized function to use when we _know_ existing edges don't overlap with poly edges.
 */
static void make_edges_mdata_extend(Mesh &mesh)
{
  int totedge = mesh.totedge;
  const MPoly *mp;
  int i;

  const Span<MPoly> polys = mesh.polys();
  MutableSpan<MLoop> loops = mesh.loops_for_write();

  const int eh_reserve = max_ii(totedge, BLI_EDGEHASH_SIZE_GUESS_FROM_POLYS(mesh.totpoly));
  EdgeHash *eh = BLI_edgehash_new_ex(__func__, eh_reserve);

  for (const MPoly &poly : polys) {
    BKE_mesh_poly_edgehash_insert(eh, &poly, &loops[poly.loopstart]);
  }

  const int totedge_new = BLI_edgehash_len(eh);

#ifdef DEBUG
  /* ensure that there's no overlap! */
  if (totedge_new) {
    for (const MEdge &edge : mesh.edges()) {
      BLI_assert(BLI_edgehash_haskey(eh, edge.v1, edge.v2) == false);
    }
  }
#endif

  if (totedge_new) {
    /* The only layer should be edges, so no other layers need to be initialized. */
    BLI_assert(mesh.edata.totlayer == 1);
    CustomData_realloc(&mesh.edata, totedge, totedge + totedge_new);
    mesh.totedge += totedge_new;
    MutableSpan<MEdge> edges = mesh.edges_for_write();
    MEdge *medge = &edges[totedge];

    EdgeHashIterator *ehi;
    uint e_index = totedge;
    for (ehi = BLI_edgehashIterator_new(eh); BLI_edgehashIterator_isDone(ehi) == false;
         BLI_edgehashIterator_step(ehi), ++medge, e_index++) {
      BLI_edgehashIterator_getKey(ehi, &medge->v1, &medge->v2);
      BLI_edgehashIterator_setValue(ehi, POINTER_FROM_UINT(e_index));

      medge->flag = ME_EDGEDRAW;
    }
    BLI_edgehashIterator_free(ehi);

    for (i = 0, mp = polys.data(); i < mesh.totpoly; i++, mp++) {
      MLoop *l = &loops[mp->loopstart];
      MLoop *l_prev = (l + (mp->totloop - 1));
      int j;
      for (j = 0; j < mp->totloop; j++, l++) {
        /* lookup hashed edge index */
        l_prev->e = POINTER_AS_UINT(BLI_edgehash_lookup(eh, l_prev->v, l->v));
        l_prev = l;
      }
    }
  }

  BLI_edgehash_free(eh, nullptr);
}

static Mesh *mesh_nurbs_displist_to_mesh(const Curve *cu, const ListBase *dispbase)
{
  using namespace blender::bke;
  int a, b, ofs;
  const bool conv_polys = (
      /* 2D polys are filled with #DispList.type == #DL_INDEX3. */
      (CU_DO_2DFILL(cu) == false) ||
      /* surf polys are never filled */
      BKE_curve_type_get(cu) == OB_SURF);

  /* count */
  int totvert = 0;
  int totedge = 0;
  int totpoly = 0;
  int totloop = 0;
  LISTBASE_FOREACH (const DispList *, dl, dispbase) {
    if (dl->type == DL_SEGM) {
      totvert += dl->parts * dl->nr;
      totedge += dl->parts * (dl->nr - 1);
    }
    else if (dl->type == DL_POLY) {
      if (conv_polys) {
        totvert += dl->parts * dl->nr;
        totedge += dl->parts * dl->nr;
      }
    }
    else if (dl->type == DL_SURF) {
      if (dl->parts != 0) {
        int tot;
        totvert += dl->parts * dl->nr;
        tot = (((dl->flag & DL_CYCL_U) ? 1 : 0) + (dl->nr - 1)) *
              (((dl->flag & DL_CYCL_V) ? 1 : 0) + (dl->parts - 1));
        totpoly += tot;
        totloop += tot * 4;
      }
    }
    else if (dl->type == DL_INDEX3) {
      int tot;
      totvert += dl->nr;
      tot = dl->parts;
      totpoly += tot;
      totloop += tot * 3;
    }
  }

  if (totvert == 0) {
    return BKE_mesh_new_nomain(0, 0, 0, 0, 0);
  }

  Mesh *mesh = BKE_mesh_new_nomain(totvert, totedge, 0, totloop, totpoly);
  MutableSpan<MVert> verts = mesh->verts_for_write();
  MutableSpan<MEdge> edges = mesh->edges_for_write();
  MutableSpan<MPoly> polys = mesh->polys_for_write();
  MutableSpan<MLoop> loops = mesh->loops_for_write();

  MutableAttributeAccessor attributes = mesh->attributes_for_write();
  SpanAttributeWriter<int> material_indices = attributes.lookup_or_add_for_write_only_span<int>(
      "material_index", ATTR_DOMAIN_FACE);
  MLoopUV *mloopuv = static_cast<MLoopUV *>(CustomData_add_layer_named(
      &mesh->ldata, CD_MLOOPUV, CD_SET_DEFAULT, nullptr, mesh->totloop, DATA_("UVMap")));

  int dst_vert = 0;
  int dst_edge = 0;
  int dst_poly = 0;
  int dst_loop = 0;
  LISTBASE_FOREACH (const DispList *, dl, dispbase) {
    const bool is_smooth = (dl->rt & CU_SMOOTH) != 0;

    if (dl->type == DL_SEGM) {
      const int startvert = dst_vert;
      a = dl->parts * dl->nr;
      const float *data = dl->verts;
      while (a--) {
        copy_v3_v3(verts[dst_vert].co, data);
        data += 3;
        dst_vert++;
      }

      for (a = 0; a < dl->parts; a++) {
        ofs = a * dl->nr;
        for (b = 1; b < dl->nr; b++) {
          edges[dst_edge].v1 = startvert + ofs + b - 1;
          edges[dst_edge].v2 = startvert + ofs + b;
          edges[dst_edge].flag = ME_EDGEDRAW;

          dst_edge++;
        }
      }
    }
    else if (dl->type == DL_POLY) {
      if (conv_polys) {
        const int startvert = dst_vert;
        a = dl->parts * dl->nr;
        const float *data = dl->verts;
        while (a--) {
          copy_v3_v3(verts[dst_vert].co, data);
          data += 3;
          dst_vert++;
        }

        for (a = 0; a < dl->parts; a++) {
          ofs = a * dl->nr;
          for (b = 0; b < dl->nr; b++) {
            edges[dst_edge].v1 = startvert + ofs + b;
            if (b == dl->nr - 1) {
              edges[dst_edge].v2 = startvert + ofs;
            }
            else {
              edges[dst_edge].v2 = startvert + ofs + b + 1;
            }
            edges[dst_edge].flag = ME_EDGEDRAW;
            dst_edge++;
          }
        }
      }
    }
    else if (dl->type == DL_INDEX3) {
      const int startvert = dst_vert;
      a = dl->nr;
      const float *data = dl->verts;
      while (a--) {
        copy_v3_v3(verts[dst_vert].co, data);
        data += 3;
        dst_vert++;
      }

      a = dl->parts;
      const int *index = dl->index;
      while (a--) {
        loops[dst_loop + 0].v = startvert + index[0];
        loops[dst_loop + 1].v = startvert + index[2];
        loops[dst_loop + 2].v = startvert + index[1];
        polys[dst_poly].loopstart = dst_loop;
        polys[dst_poly].totloop = 3;
        material_indices.span[dst_poly] = dl->col;

        if (mloopuv) {
          for (int i = 0; i < 3; i++, mloopuv++) {
            mloopuv->uv[0] = (loops[dst_loop + i].v - startvert) / float(dl->nr - 1);
            mloopuv->uv[1] = 0.0f;
          }
        }

        if (is_smooth) {
          polys[dst_poly].flag |= ME_SMOOTH;
        }
        dst_poly++;
        dst_loop += 3;
        index += 3;
      }
    }
    else if (dl->type == DL_SURF) {
      const int startvert = dst_vert;
      a = dl->parts * dl->nr;
      const float *data = dl->verts;
      while (a--) {
        copy_v3_v3(verts[dst_vert].co, data);
        data += 3;
        dst_vert++;
      }

      for (a = 0; a < dl->parts; a++) {

        if ((dl->flag & DL_CYCL_V) == 0 && a == dl->parts - 1) {
          break;
        }

        int p1, p2, p3, p4;
        if (dl->flag & DL_CYCL_U) {    /* p2 -> p1 -> */
          p1 = startvert + dl->nr * a; /* p4 -> p3 -> */
          p2 = p1 + dl->nr - 1;        /* -----> next row */
          p3 = p1 + dl->nr;
          p4 = p2 + dl->nr;
          b = 0;
        }
        else {
          p2 = startvert + dl->nr * a;
          p1 = p2 + 1;
          p4 = p2 + dl->nr;
          p3 = p1 + dl->nr;
          b = 1;
        }
        if ((dl->flag & DL_CYCL_V) && a == dl->parts - 1) {
          p3 -= dl->parts * dl->nr;
          p4 -= dl->parts * dl->nr;
        }

        for (; b < dl->nr; b++) {
          loops[dst_loop + 0].v = p1;
          loops[dst_loop + 1].v = p3;
          loops[dst_loop + 2].v = p4;
          loops[dst_loop + 3].v = p2;
          polys[dst_poly].loopstart = dst_loop;
          polys[dst_poly].totloop = 4;
          material_indices.span[dst_poly] = dl->col;

          if (mloopuv) {
            int orco_sizeu = dl->nr - 1;
            int orco_sizev = dl->parts - 1;

            /* exception as handled in convertblender.c too */
            if (dl->flag & DL_CYCL_U) {
              orco_sizeu++;
              if (dl->flag & DL_CYCL_V) {
                orco_sizev++;
              }
            }
            else if (dl->flag & DL_CYCL_V) {
              orco_sizev++;
            }

            for (int i = 0; i < 4; i++, mloopuv++) {
              /* find uv based on vertex index into grid array */
              int v = loops[dst_loop + i].v - startvert;

              mloopuv->uv[0] = (v / dl->nr) / float(orco_sizev);
              mloopuv->uv[1] = (v % dl->nr) / float(orco_sizeu);

              /* cyclic correction */
              if (ELEM(i, 1, 2) && mloopuv->uv[0] == 0.0f) {
                mloopuv->uv[0] = 1.0f;
              }
              if (ELEM(i, 0, 1) && mloopuv->uv[1] == 0.0f) {
                mloopuv->uv[1] = 1.0f;
              }
            }
          }

          if (is_smooth) {
            polys[dst_poly].flag |= ME_SMOOTH;
          }
          dst_poly++;
          dst_loop += 4;

          p4 = p3;
          p3++;
          p2 = p1;
          p1++;
        }
      }
    }
  }

  if (totpoly) {
    make_edges_mdata_extend(*mesh);
  }

  material_indices.finish();

  return mesh;
}

/**
 * Copy evaluated texture space from curve to mesh.
 *
 * \note We disable auto texture space feature since that will cause texture space to evaluate
 * differently for curve and mesh, since curves use control points and handles to calculate the
 * bounding box, and mesh uses the tessellated curve.
 */
static void mesh_copy_texture_space_from_curve_type(const Curve *cu, Mesh *me)
{
  me->texflag = cu->texflag & ~CU_AUTOSPACE;
  copy_v3_v3(me->loc, cu->loc);
  copy_v3_v3(me->size, cu->size);
  BKE_mesh_texspace_calc(me);
}

Mesh *BKE_mesh_new_nomain_from_curve_displist(const Object *ob, const ListBase *dispbase)
{
  const Curve *cu = (const Curve *)ob->data;

  Mesh *mesh = mesh_nurbs_displist_to_mesh(cu, dispbase);
  mesh_copy_texture_space_from_curve_type(cu, mesh);
  mesh->mat = (Material **)MEM_dupallocN(cu->mat);
  mesh->totcol = cu->totcol;

  return mesh;
}

Mesh *BKE_mesh_new_nomain_from_curve(const Object *ob)
{
  ListBase disp = {nullptr, nullptr};

  if (ob->runtime.curve_cache) {
    disp = ob->runtime.curve_cache->disp;
  }

  return BKE_mesh_new_nomain_from_curve_displist(ob, &disp);
}

struct EdgeLink {
  struct EdgeLink *next, *prev;
  const void *edge;
};

struct VertLink {
  Link *next, *prev;
  uint index;
};

static void prependPolyLineVert(ListBase *lb, uint index)
{
  VertLink *vl = MEM_cnew<VertLink>("VertLink");
  vl->index = index;
  BLI_addhead(lb, vl);
}

static void appendPolyLineVert(ListBase *lb, uint index)
{
  VertLink *vl = MEM_cnew<VertLink>("VertLink");
  vl->index = index;
  BLI_addtail(lb, vl);
}

void BKE_mesh_to_curve_nurblist(const Mesh *me, ListBase *nurblist, const int edge_users_test)
{
  const Span<MVert> verts = me->verts();
  const Span<MEdge> mesh_edges = me->edges();
  const Span<MPoly> polys = me->polys();
  const Span<MLoop> loops = me->loops();

  const MEdge *med;
  const MPoly *mp;

  int medge_len = me->totedge;
  int mpoly_len = me->totpoly;
  int totedges = 0;
  int i;

  /* only to detect edge polylines */
  int *edge_users;

  ListBase edges = {nullptr, nullptr};

  /* get boundary edges */
  edge_users = (int *)MEM_calloc_arrayN(medge_len, sizeof(int), __func__);
  for (i = 0, mp = polys.data(); i < mpoly_len; i++, mp++) {
    const MLoop *ml = &loops[mp->loopstart];
    int j;
    for (j = 0; j < mp->totloop; j++, ml++) {
      edge_users[ml->e]++;
    }
  }

  /* create edges from all faces (so as to find edges not in any faces) */
  med = mesh_edges.data();
  for (i = 0; i < medge_len; i++, med++) {
    if (edge_users[i] == edge_users_test) {
      EdgeLink *edl = MEM_cnew<EdgeLink>("EdgeLink");
      edl->edge = med;

      BLI_addtail(&edges, edl);
      totedges++;
    }
  }
  MEM_freeN(edge_users);

  if (edges.first) {
    while (edges.first) {
      /* each iteration find a polyline and add this as a nurbs poly spline */

      ListBase polyline = {nullptr, nullptr}; /* store a list of VertLink's */
      bool closed = false;
      int totpoly = 0;
      MEdge *med_current = (MEdge *)((EdgeLink *)edges.last)->edge;
      uint startVert = med_current->v1;
      uint endVert = med_current->v2;
      bool ok = true;

      appendPolyLineVert(&polyline, startVert);
      totpoly++;
      appendPolyLineVert(&polyline, endVert);
      totpoly++;
      BLI_freelinkN(&edges, edges.last);
      totedges--;

      while (ok) { /* while connected edges are found... */
        EdgeLink *edl = (EdgeLink *)edges.last;
        ok = false;
        while (edl) {
          EdgeLink *edl_prev = edl->prev;

          med = (MEdge *)edl->edge;

          if (med->v1 == endVert) {
            endVert = med->v2;
            appendPolyLineVert(&polyline, med->v2);
            totpoly++;
            BLI_freelinkN(&edges, edl);
            totedges--;
            ok = true;
          }
          else if (med->v2 == endVert) {
            endVert = med->v1;
            appendPolyLineVert(&polyline, endVert);
            totpoly++;
            BLI_freelinkN(&edges, edl);
            totedges--;
            ok = true;
          }
          else if (med->v1 == startVert) {
            startVert = med->v2;
            prependPolyLineVert(&polyline, startVert);
            totpoly++;
            BLI_freelinkN(&edges, edl);
            totedges--;
            ok = true;
          }
          else if (med->v2 == startVert) {
            startVert = med->v1;
            prependPolyLineVert(&polyline, startVert);
            totpoly++;
            BLI_freelinkN(&edges, edl);
            totedges--;
            ok = true;
          }

          edl = edl_prev;
        }
      }

      /* Now we have a polyline, make into a curve */
      if (startVert == endVert) {
        BLI_freelinkN(&polyline, polyline.last);
        totpoly--;
        closed = true;
      }

      /* --- nurbs --- */
      {
        Nurb *nu;
        BPoint *bp;
        VertLink *vl;

        /* create new 'nurb' within the curve */
        nu = MEM_new<Nurb>("MeshNurb", blender::dna::shallow_zero_initialize());

        nu->pntsu = totpoly;
        nu->pntsv = 1;
        nu->orderu = 4;
        nu->flagu = CU_NURB_ENDPOINT | (closed ? CU_NURB_CYCLIC : 0); /* endpoint */
        nu->resolu = 12;

        nu->bp = (BPoint *)MEM_calloc_arrayN(totpoly, sizeof(BPoint), "bpoints");

        /* add points */
        vl = (VertLink *)polyline.first;
        for (i = 0, bp = nu->bp; i < totpoly; i++, bp++, vl = (VertLink *)vl->next) {
          copy_v3_v3(bp->vec, verts[vl->index].co);
          bp->f1 = SELECT;
          bp->radius = bp->weight = 1.0;
        }
        BLI_freelistN(&polyline);

        /* add nurb to curve */
        BLI_addtail(nurblist, nu);
      }
      /* --- done with nurbs --- */
    }
  }
}

void BKE_mesh_to_curve(Main *bmain, Depsgraph *depsgraph, Scene * /*scene*/, Object *ob)
{
  /* make new mesh data from the original copy */
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
  Mesh *me_eval = mesh_get_eval_final(depsgraph, scene_eval, ob_eval, &CD_MASK_MESH);
  ListBase nurblist = {nullptr, nullptr};

  BKE_mesh_to_curve_nurblist(me_eval, &nurblist, 0);
  BKE_mesh_to_curve_nurblist(me_eval, &nurblist, 1);

  if (nurblist.first) {
    Curve *cu = BKE_curve_add(bmain, ob->id.name + 2, OB_CURVES_LEGACY);
    cu->flag |= CU_3D;

    cu->nurb = nurblist;

    id_us_min(&((Mesh *)ob->data)->id);
    ob->data = cu;
    ob->type = OB_CURVES_LEGACY;

    BKE_object_free_derived_caches(ob);
  }
}

void BKE_pointcloud_from_mesh(Mesh *me, PointCloud *pointcloud)
{
  using namespace blender;

  BLI_assert(me != nullptr);
  /* The pointcloud should only contain the position attribute, otherwise more attributes would
   * need to be initialized below. */
  BLI_assert(pointcloud->attributes().all_ids().size() == 1);
  CustomData_realloc(&pointcloud->pdata, pointcloud->totpoint, me->totvert);
  pointcloud->totpoint = me->totvert;

  /* Copy over all attributes. */
  CustomData_merge(&me->vdata, &pointcloud->pdata, CD_MASK_PROP_ALL, CD_DUPLICATE, me->totvert);

  bke::AttributeAccessor mesh_attributes = me->attributes();
  bke::MutableAttributeAccessor point_attributes = pointcloud->attributes_for_write();

  const VArray<float3> mesh_positions = mesh_attributes.lookup_or_default<float3>(
      "position", ATTR_DOMAIN_POINT, float3(0));
  bke::SpanAttributeWriter<float3> point_positions =
      point_attributes.lookup_or_add_for_write_only_span<float3>("position", ATTR_DOMAIN_POINT);
  mesh_positions.materialize(point_positions.span);
  point_positions.finish();
}

void BKE_mesh_to_pointcloud(Main *bmain, Depsgraph *depsgraph, Scene * /*scene*/, Object *ob)
{
  BLI_assert(ob->type == OB_MESH);

  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
  Mesh *me_eval = mesh_get_eval_final(depsgraph, scene_eval, ob_eval, &CD_MASK_MESH);

  PointCloud *pointcloud = (PointCloud *)BKE_pointcloud_add(bmain, ob->id.name + 2);

  BKE_pointcloud_from_mesh(me_eval, pointcloud);

  BKE_id_materials_copy(bmain, (ID *)ob->data, (ID *)pointcloud);

  id_us_min(&((Mesh *)ob->data)->id);
  ob->data = pointcloud;
  ob->type = OB_POINTCLOUD;

  BKE_object_free_derived_caches(ob);
}

void BKE_mesh_from_pointcloud(const PointCloud *pointcloud, Mesh *me)
{
  BLI_assert(pointcloud != nullptr);

  me->totvert = pointcloud->totpoint;

  /* Merge over all attributes. */
  CustomData_merge(
      &pointcloud->pdata, &me->vdata, CD_MASK_PROP_ALL, CD_DUPLICATE, pointcloud->totpoint);

  /* Convert the Position attribute to a mesh vertex. */
  CustomData_add_layer(&me->vdata, CD_MVERT, CD_SET_DEFAULT, nullptr, me->totvert);

  const int layer_idx = CustomData_get_named_layer_index(
      &me->vdata, CD_PROP_FLOAT3, POINTCLOUD_ATTR_POSITION);
  CustomDataLayer *pos_layer = &me->vdata.layers[layer_idx];
  float(*positions)[3] = (float(*)[3])pos_layer->data;

  MutableSpan<MVert> verts = me->verts_for_write();
  for (int i = 0; i < me->totvert; i++) {
    copy_v3_v3(verts[i].co, positions[i]);
  }

  /* Delete Position attribute since it is now in vertex coordinates. */
  CustomData_free_layer(&me->vdata, CD_PROP_FLOAT3, me->totvert, layer_idx);
}

void BKE_mesh_edges_set_draw_render(Mesh *mesh)
{
  MutableSpan<MEdge> edges = mesh->edges_for_write();
  for (int i = 0; i < mesh->totedge; i++) {
    edges[i].flag |= ME_EDGEDRAW;
  }
}

void BKE_pointcloud_to_mesh(Main *bmain, Depsgraph *depsgraph, Scene * /*scene*/, Object *ob)
{
  BLI_assert(ob->type == OB_POINTCLOUD);

  Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
  PointCloud *pointcloud_eval = (PointCloud *)ob_eval->runtime.data_eval;

  Mesh *me = BKE_mesh_add(bmain, ob->id.name + 2);

  BKE_mesh_from_pointcloud(pointcloud_eval, me);

  BKE_id_materials_copy(bmain, (ID *)ob->data, (ID *)me);

  id_us_min(&((PointCloud *)ob->data)->id);
  ob->data = me;
  ob->type = OB_MESH;

  BKE_object_free_derived_caches(ob);
}

/* Create a temporary object to be used for nurbs-to-mesh conversion. */
static Object *object_for_curve_to_mesh_create(const Object *object)
{
  const Curve *curve = (const Curve *)object->data;

  /* Create a temporary object which can be evaluated and modified by generic
   * curve evaluation (hence the #LIB_ID_COPY_SET_COPIED_ON_WRITE flag). */
  Object *temp_object = (Object *)BKE_id_copy_ex(
      nullptr, &object->id, nullptr, LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_SET_COPIED_ON_WRITE);

  /* Remove all modifiers, since we don't want them to be applied. */
  BKE_object_free_modifiers(temp_object, LIB_ID_CREATE_NO_USER_REFCOUNT);

  /* Need to create copy of curve itself as well, since it will be changed by the curve evaluation
   * process. NOTE: Copies the data, but not the shape-keys. */
  temp_object->data = BKE_id_copy_ex(nullptr,
                                     (const ID *)object->data,
                                     nullptr,
                                     LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_SET_COPIED_ON_WRITE);
  Curve *temp_curve = (Curve *)temp_object->data;

  /* Make sure texture space is calculated for a copy of curve, it will be used for the final
   * result. */
  BKE_curve_texspace_calc(temp_curve);

  /* Temporarily set edit so we get updates from edit mode, but also because for text data-blocks
   * copying it while in edit mode gives invalid data structures. */
  temp_curve->editfont = curve->editfont;
  temp_curve->editnurb = curve->editnurb;

  return temp_object;
}

static void object_for_curve_to_mesh_free(Object *temp_object)
{
  /* Clear edit mode pointers that were explicitly copied to the temporary curve. */
  ID *final_object_data = static_cast<ID *>(temp_object->data);
  if (GS(final_object_data->name) == ID_CU_LEGACY) {
    Curve &curve = *reinterpret_cast<Curve *>(final_object_data);
    curve.editfont = nullptr;
    curve.editnurb = nullptr;
  }

  /* Only free the final object data if it is *not* stored in the #data_eval field. This is still
   * necessary because #temp_object's data could be replaced by a #Curve data-block that isn't also
   * assigned to #data_eval. */
  const bool object_data_stored_in_data_eval = final_object_data == temp_object->runtime.data_eval;

  BKE_id_free(nullptr, temp_object);
  if (!object_data_stored_in_data_eval) {
    BKE_id_free(nullptr, final_object_data);
  }
}

/**
 * Populate `object->runtime.curve_cache` which is then used to create the mesh.
 */
static void curve_to_mesh_eval_ensure(Object &object)
{
  BLI_assert(GS(static_cast<ID *>(object.data)->name) == ID_CU_LEGACY);
  Curve &curve = *static_cast<Curve *>(object.data);
  /* Clear all modifiers for the bevel object.
   *
   * This is because they can not be reliably evaluated for an original object (at least because
   * the state of dependencies is not know).
   *
   * So we create temporary copy of the object which will use same data as the original bevel, but
   * will have no modifiers. */
  Object bevel_object = blender::dna::shallow_zero_initialize();
  if (curve.bevobj != nullptr) {
    bevel_object = blender::dna::shallow_copy(*curve.bevobj);
    BLI_listbase_clear(&bevel_object.modifiers);
    BKE_object_runtime_reset(&bevel_object);
    curve.bevobj = &bevel_object;
  }

  /* Same thing for taper. */
  Object taper_object = blender::dna::shallow_zero_initialize();
  if (curve.taperobj != nullptr) {
    taper_object = blender::dna::shallow_copy(*curve.taperobj);
    BLI_listbase_clear(&taper_object.modifiers);
    BKE_object_runtime_reset(&taper_object);
    curve.taperobj = &taper_object;
  }

  /* NOTE: We don't have dependency graph or scene here, so we pass nullptr. This is all fine since
   * they are only used for modifier stack, which we have explicitly disabled for all objects.
   *
   * TODO(sergey): This is a very fragile logic, but proper solution requires re-writing quite a
   * bit of internal functions (#BKE_mesh_nomain_to_mesh) and also Mesh From Curve operator.
   * Brecht says hold off with that. */
  BKE_displist_make_curveTypes(nullptr, nullptr, &object, true);

  BKE_object_runtime_free_data(&bevel_object);
  BKE_object_runtime_free_data(&taper_object);
}

static const Curves *get_evaluated_curves_from_object(const Object *object)
{
  if (GeometrySet *geometry_set_eval = object->runtime.geometry_set_eval) {
    return geometry_set_eval->get_curves_for_read();
  }
  return nullptr;
}

static Mesh *mesh_new_from_evaluated_curve_type_object(const Object *evaluated_object)
{
  if (const Mesh *mesh = BKE_object_get_evaluated_mesh(evaluated_object)) {
    return BKE_mesh_copy_for_eval(mesh, false);
  }
  if (const Curves *curves = get_evaluated_curves_from_object(evaluated_object)) {
    const blender::bke::AnonymousAttributePropagationInfo propagation_info;
    return blender::bke::curve_to_wire_mesh(blender::bke::CurvesGeometry::wrap(curves->geometry),
                                            propagation_info);
  }
  return nullptr;
}

static Mesh *mesh_new_from_curve_type_object(const Object *object)
{
  /* If the object is evaluated, it should either have an evaluated mesh or curve data already.
   * The mesh can be duplicated, or the curve converted to wire mesh edges. */
  if (DEG_is_evaluated_object(object)) {
    return mesh_new_from_evaluated_curve_type_object(object);
  }

  /* Otherwise, create a temporary "fake" evaluated object and try again. This might have
   * different results, since in order to avoid having adverse affects to other original objects,
   * modifiers are cleared. An alternative would be to create a temporary depsgraph only for this
   * object and its dependencies. */
  Object *temp_object = object_for_curve_to_mesh_create(object);
  ID *temp_data = static_cast<ID *>(temp_object->data);
  curve_to_mesh_eval_ensure(*temp_object);

  /* If evaluating the curve replaced object data with different data, free the original data. */
  if (temp_data != temp_object->data) {
    if (GS(temp_data->name) == ID_CU_LEGACY) {
      /* Clear edit mode pointers that were explicitly copied to the temporary curve. */
      Curve *curve = reinterpret_cast<Curve *>(temp_data);
      curve->editfont = nullptr;
      curve->editnurb = nullptr;
    }
    BKE_id_free(nullptr, temp_data);
  }

  Mesh *mesh = mesh_new_from_evaluated_curve_type_object(temp_object);

  object_for_curve_to_mesh_free(temp_object);

  return mesh;
}

static Mesh *mesh_new_from_mball_object(Object *object)
{
  /* NOTE: We can only create mesh for a polygonized meta ball. This figures out all original meta
   * balls and all evaluated child meta balls (since polygonization is only stored in the mother
   * ball).
   *
   * Create empty mesh so script-authors don't run into None objects. */
  if (!DEG_is_evaluated_object(object)) {
    return (Mesh *)BKE_id_new_nomain(ID_ME, ((ID *)object->data)->name + 2);
  }

  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(object);
  if (mesh_eval == nullptr) {
    return (Mesh *)BKE_id_new_nomain(ID_ME, ((ID *)object->data)->name + 2);
  }

  return BKE_mesh_copy_for_eval(mesh_eval, false);
}

static Mesh *mesh_new_from_mesh(Object *object, Mesh *mesh)
{
  /* While we could copy this into the new mesh,
   * add the data to 'mesh' so future calls to this function don't need to re-convert the data. */
  if (mesh->runtime->wrapper_type == ME_WRAPPER_TYPE_BMESH) {
    BKE_mesh_wrapper_ensure_mdata(mesh);
  }
  else {
    mesh = BKE_mesh_wrapper_ensure_subdivision(mesh);
  }

  Mesh *mesh_result = (Mesh *)BKE_id_copy_ex(
      nullptr, &mesh->id, nullptr, LIB_ID_CREATE_NO_MAIN | LIB_ID_CREATE_NO_USER_REFCOUNT);
  /* NOTE: Materials should already be copied. */
  /* Copy original mesh name. This is because edit meshes might not have one properly set name. */
  BLI_strncpy(mesh_result->id.name, ((ID *)object->data)->name, sizeof(mesh_result->id.name));
  return mesh_result;
}

static Mesh *mesh_new_from_mesh_object_with_layers(Depsgraph *depsgraph,
                                                   Object *object,
                                                   const bool preserve_origindex)
{
  if (DEG_is_original_id(&object->id)) {
    return mesh_new_from_mesh(object, (Mesh *)object->data);
  }

  if (depsgraph == nullptr) {
    return nullptr;
  }

  Object object_for_eval = blender::dna::shallow_copy(*object);
  if (object_for_eval.runtime.data_orig != nullptr) {
    object_for_eval.data = object_for_eval.runtime.data_orig;
  }

  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  CustomData_MeshMasks mask = CD_MASK_MESH;
  if (preserve_origindex) {
    mask.vmask |= CD_MASK_ORIGINDEX;
    mask.emask |= CD_MASK_ORIGINDEX;
    mask.lmask |= CD_MASK_ORIGINDEX;
    mask.pmask |= CD_MASK_ORIGINDEX;
  }
  Mesh *result = mesh_create_eval_final(depsgraph, scene, &object_for_eval, &mask);
  return BKE_mesh_wrapper_ensure_subdivision(result);
}

static Mesh *mesh_new_from_mesh_object(Depsgraph *depsgraph,
                                       Object *object,
                                       const bool preserve_all_data_layers,
                                       const bool preserve_origindex)
{
  if (preserve_all_data_layers || preserve_origindex) {
    return mesh_new_from_mesh_object_with_layers(depsgraph, object, preserve_origindex);
  }
  Mesh *mesh_input = (Mesh *)object->data;
  /* If we are in edit mode, use evaluated mesh from edit structure, matching to what
   * viewport is using for visualization. */
  if (mesh_input->edit_mesh != nullptr) {
    Mesh *editmesh_eval_final = BKE_object_get_editmesh_eval_final(object);
    if (editmesh_eval_final != nullptr) {
      mesh_input = editmesh_eval_final;
    }
  }
  return mesh_new_from_mesh(object, mesh_input);
}

Mesh *BKE_mesh_new_from_object(Depsgraph *depsgraph,
                               Object *object,
                               const bool preserve_all_data_layers,
                               const bool preserve_origindex)
{
  Mesh *new_mesh = nullptr;
  switch (object->type) {
    case OB_FONT:
    case OB_CURVES_LEGACY:
    case OB_SURF:
      new_mesh = mesh_new_from_curve_type_object(object);
      break;
    case OB_MBALL:
      new_mesh = mesh_new_from_mball_object(object);
      break;
    case OB_MESH:
      new_mesh = mesh_new_from_mesh_object(
          depsgraph, object, preserve_all_data_layers, preserve_origindex);
      break;
    default:
      /* Object does not have geometry data. */
      return nullptr;
  }
  if (new_mesh == nullptr) {
    /* Happens in special cases like request of mesh for non-mother meta ball. */
    return nullptr;
  }

  /* The result must have 0 users, since it's just a mesh which is free-dangling data-block.
   * All the conversion functions are supposed to ensure mesh is not counted. */
  BLI_assert(new_mesh->id.us == 0);

  /* It is possible that mesh came from modifier stack evaluation, which preserves edit_mesh
   * pointer (which allows draw manager to access edit mesh when drawing). Normally this does
   * not cause ownership problems because evaluated object runtime is keeping track of the real
   * ownership.
   *
   * Here we are constructing a mesh which is supposed to be independent, which means no shared
   * ownership is allowed, so we make sure edit mesh is reset to nullptr (which is similar to as if
   * one duplicates the objects and applies all the modifiers). */
  new_mesh->edit_mesh = nullptr;

  return new_mesh;
}

static int foreach_libblock_make_original_callback(LibraryIDLinkCallbackData *cb_data)
{
  ID **id_p = cb_data->id_pointer;
  if (*id_p == nullptr) {
    return IDWALK_RET_NOP;
  }
  *id_p = DEG_get_original_id(*id_p);

  return IDWALK_RET_NOP;
}

static int foreach_libblock_make_usercounts_callback(LibraryIDLinkCallbackData *cb_data)
{
  ID **id_p = cb_data->id_pointer;
  if (*id_p == nullptr) {
    return IDWALK_RET_NOP;
  }

  const int cb_flag = cb_data->cb_flag;
  if (cb_flag & IDWALK_CB_USER) {
    id_us_plus(*id_p);
  }
  else if (cb_flag & IDWALK_CB_USER_ONE) {
    /* NOTE: in that context, that one should not be needed (since there should be at least already
     * one USER_ONE user of that ID), but better be consistent. */
    id_us_ensure_real(*id_p);
  }
  return IDWALK_RET_NOP;
}

Mesh *BKE_mesh_new_from_object_to_bmain(Main *bmain,
                                        Depsgraph *depsgraph,
                                        Object *object,
                                        bool preserve_all_data_layers)
{
  BLI_assert(ELEM(object->type, OB_FONT, OB_CURVES_LEGACY, OB_SURF, OB_MBALL, OB_MESH));

  Mesh *mesh = BKE_mesh_new_from_object(depsgraph, object, preserve_all_data_layers, false);
  if (mesh == nullptr) {
    /* Unable to convert the object to a mesh, return an empty one. */
    Mesh *mesh_in_bmain = BKE_mesh_add(bmain, ((ID *)object->data)->name + 2);
    id_us_min(&mesh_in_bmain->id);
    return mesh_in_bmain;
  }

  /* Make sure mesh only points original data-blocks, also increase users of materials and other
   * possibly referenced data-blocks.
   *
   * Going to original data-blocks is required to have bmain in a consistent state, where
   * everything is only allowed to reference original data-blocks.
   *
   * Note that user-count updates has to be done *after* mesh has been transferred to Main database
   * (since doing reference-counting on non-Main IDs is forbidden). */
  BKE_library_foreach_ID_link(
      nullptr, &mesh->id, foreach_libblock_make_original_callback, nullptr, IDWALK_NOP);

  /* Append the mesh to 'bmain'.
   * We do it a bit longer way since there is no simple and clear way of adding existing data-block
   * to the 'bmain'. So we allocate new empty mesh in the 'bmain' (which guarantees all the naming
   * and orders and flags) and move the temporary mesh in place there. */
  Mesh *mesh_in_bmain = BKE_mesh_add(bmain, mesh->id.name + 2);

  /* NOTE: BKE_mesh_nomain_to_mesh() does not copy materials and instead it preserves them in the
   * destination mesh. So we "steal" all related fields before calling it.
   *
   * TODO(sergey): We really better have a function which gets and ID and accepts it for the bmain.
   */
  mesh_in_bmain->mat = mesh->mat;
  mesh_in_bmain->totcol = mesh->totcol;
  mesh_in_bmain->flag = mesh->flag;
  mesh_in_bmain->smoothresh = mesh->smoothresh;
  mesh->mat = nullptr;

  BKE_mesh_nomain_to_mesh(mesh, mesh_in_bmain, nullptr);

  /* Anonymous attributes shouldn't exist on original data. */
  mesh_in_bmain->attributes_for_write().remove_anonymous();

  /* User-count is required because so far mesh was in a limbo, where library management does
   * not perform any user management (i.e. copy of a mesh will not increase users of materials). */
  BKE_library_foreach_ID_link(
      nullptr, &mesh_in_bmain->id, foreach_libblock_make_usercounts_callback, nullptr, IDWALK_NOP);

  /* Make sure user count from BKE_mesh_add() is the one we expect here and bring it down to 0. */
  BLI_assert(mesh_in_bmain->id.us == 1);
  id_us_min(&mesh_in_bmain->id);

  return mesh_in_bmain;
}

static KeyBlock *keyblock_ensure_from_uid(Key &key, const int uid, const StringRefNull name)
{
  if (KeyBlock *kb = BKE_keyblock_find_uid(&key, uid)) {
    return kb;
  }
  KeyBlock *kb = BKE_keyblock_add(&key, name.c_str());
  kb->uid = uid;
  return kb;
}

static int find_object_active_key_uid(const Key &key, const Object &object)
{
  const int active_kb_index = object.shapenr - 1;
  const KeyBlock *kb = (const KeyBlock *)BLI_findlink(&key.block, active_kb_index);
  if (!kb) {
    CLOG_ERROR(&LOG, "Could not find object's active shapekey %d", active_kb_index);
    return -1;
  }
  return kb->uid;
}

static void move_shapekey_layers_to_keyblocks(const Mesh &mesh,
                                              CustomData &custom_data,
                                              Key &key_dst,
                                              const int actshape_uid)
{
  using namespace blender::bke;
  for (const int i : IndexRange(CustomData_number_of_layers(&custom_data, CD_SHAPEKEY))) {
    const int layer_index = CustomData_get_layer_index_n(&custom_data, CD_SHAPEKEY, i);
    CustomDataLayer &layer = custom_data.layers[layer_index];

    KeyBlock *kb = keyblock_ensure_from_uid(key_dst, layer.uid, layer.name);
    MEM_SAFE_FREE(kb->data);

    kb->totelem = mesh.totvert;

    if (kb->uid == actshape_uid) {
      kb->data = MEM_malloc_arrayN(kb->totelem, sizeof(float3), __func__);
      MutableSpan<float3> kb_coords(static_cast<float3 *>(kb->data), kb->totelem);
      mesh.attributes().lookup<float3>("position").materialize(kb_coords);
    }
    else {
      kb->data = layer.data;
      layer.data = nullptr;
    }
  }

  LISTBASE_FOREACH (KeyBlock *, kb, &key_dst.block) {
    if (kb->totelem != mesh.totvert) {
      MEM_SAFE_FREE(kb->data);
      kb->totelem = mesh.totvert;
      kb->data = MEM_cnew_array<float3>(kb->totelem, __func__);
      CLOG_ERROR(&LOG, "Data for shape key '%s' on mesh missing from evaluated mesh ", kb->name);
    }
  }
}

void BKE_mesh_nomain_to_mesh(Mesh *mesh_src, Mesh *mesh_dst, Object *ob)
{
  using namespace blender::bke;
  BLI_assert(mesh_src->id.tag & LIB_TAG_NO_MAIN);
  if (ob) {
    BLI_assert(mesh_dst == ob->data);
  }

  BKE_mesh_clear_geometry(mesh_dst);

  /* Make sure referenced layers have a single user so assigning them to the mesh in main doesn't
   * share them. "Referenced" layers are not expected to be shared between original meshes. */
  CustomData_duplicate_referenced_layers(&mesh_src->vdata, mesh_src->totvert);
  CustomData_duplicate_referenced_layers(&mesh_src->edata, mesh_src->totedge);
  CustomData_duplicate_referenced_layers(&mesh_src->pdata, mesh_src->totpoly);
  CustomData_duplicate_referenced_layers(&mesh_src->ldata, mesh_src->totloop);

  const bool verts_num_changed = mesh_dst->totvert != mesh_src->totvert;
  mesh_dst->totvert = mesh_src->totvert;
  mesh_dst->totedge = mesh_src->totedge;
  mesh_dst->totpoly = mesh_src->totpoly;
  mesh_dst->totloop = mesh_src->totloop;

  /* Using #CD_MASK_MESH ensures that only data that should exist in Main meshes is moved. */
  const CustomData_MeshMasks mask = CD_MASK_MESH;
  CustomData_copy(&mesh_src->vdata, &mesh_dst->vdata, mask.vmask, CD_ASSIGN, mesh_src->totvert);
  CustomData_copy(&mesh_src->edata, &mesh_dst->edata, mask.emask, CD_ASSIGN, mesh_src->totedge);
  CustomData_copy(&mesh_src->pdata, &mesh_dst->pdata, mask.pmask, CD_ASSIGN, mesh_src->totpoly);
  CustomData_copy(&mesh_src->ldata, &mesh_dst->ldata, mask.lmask, CD_ASSIGN, mesh_src->totloop);

  /* Make sure active/default color attribute (names) are brought over. */
  if (mesh_src->active_color_attribute) {
    MEM_SAFE_FREE(mesh_dst->active_color_attribute);
    mesh_dst->active_color_attribute = BLI_strdup(mesh_src->active_color_attribute);
  }
  if (mesh_src->default_color_attribute) {
    MEM_SAFE_FREE(mesh_dst->default_color_attribute);
    mesh_dst->default_color_attribute = BLI_strdup(mesh_src->default_color_attribute);
  }

  BLI_freelistN(&mesh_dst->vertex_group_names);
  mesh_dst->vertex_group_names = mesh_src->vertex_group_names;
  BLI_listbase_clear(&mesh_src->vertex_group_names);

  BKE_mesh_copy_parameters(mesh_dst, mesh_src);

  /* For original meshes, shape key data is stored in the #Key data-block, so it
   * must be moved from the storage in #CustomData layers used for evaluation. */
  if (Key *key_dst = mesh_dst->key) {
    if (CustomData_has_layer(&mesh_src->vdata, CD_SHAPEKEY)) {
      /* If no object, set to -1 so we don't mess up any shapekey layers. */
      const int uid_active = ob ? find_object_active_key_uid(*key_dst, *ob) : -1;
      move_shapekey_layers_to_keyblocks(*mesh_dst, mesh_src->vdata, *key_dst, uid_active);
    }
    else if (verts_num_changed) {
      CLOG_WARN(&LOG, "Shape key data lost when replacing mesh '%s' in Main", mesh_src->id.name);
      id_us_min(&mesh_dst->key->id);
      mesh_dst->key = nullptr;
    }
  }

  BKE_id_free(nullptr, mesh_src);
}

void BKE_mesh_nomain_to_meshkey(Mesh *mesh_src, Mesh *mesh_dst, KeyBlock *kb)
{
  BLI_assert(mesh_src->id.tag & LIB_TAG_NO_MAIN);

  int a, totvert = mesh_src->totvert;
  float *fp;

  if (totvert == 0 || mesh_dst->totvert == 0 || mesh_dst->totvert != totvert) {
    return;
  }

  if (kb->data) {
    MEM_freeN(kb->data);
  }
  kb->data = MEM_malloc_arrayN(mesh_dst->key->elemsize, mesh_dst->totvert, "kb->data");
  kb->totelem = totvert;

  fp = (float *)kb->data;
  const Span<MVert> verts = mesh_src->verts();
  for (a = 0; a < kb->totelem; a++, fp += 3) {
    copy_v3_v3(fp, verts[a].co);
  }
}
