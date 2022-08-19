/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BlenderStrokeRenderer.h"

#include "../application/AppConfig.h"
#include "../stroke/Canvas.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"
#include "RNA_types.h"

#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_linestyle_types.h"
#include "DNA_listBase.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_collection.h"
#include "BKE_customdata.h"
#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h" /* free_libblock */
#include "BKE_material.h"
#include "BKE_mesh.h"
#include "BKE_node.h"
#include "BKE_node_tree_update.h"
#include "BKE_object.h"
#include "BKE_scene.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "RE_pipeline.h"

#include "render_types.h"

#include <climits>

namespace Freestyle {

const char *BlenderStrokeRenderer::uvNames[] = {"along_stroke", "along_stroke_tips"};

BlenderStrokeRenderer::BlenderStrokeRenderer(Render *re, int render_count)
{
  freestyle_bmain = BKE_main_new();

  /* We use the same window manager for freestyle bmain as
   * real bmain uses. This is needed because freestyle's
   * bmain could be used to tag scenes for update, which
   * implies call of ED_render_scene_update in some cases
   * and that function requires proper window manager
   * to present (sergey)
   */
  freestyle_bmain->wm = re->main->wm;

  // for stroke mesh generation
  _width = re->winx;
  _height = re->winy;

  old_scene = re->scene;

  char name[MAX_ID_NAME - 2];
  BLI_snprintf(name, sizeof(name), "FRS%d_%s", render_count, re->scene->id.name + 2);
  freestyle_scene = BKE_scene_add(freestyle_bmain, name);
  freestyle_scene->r.cfra = old_scene->r.cfra;
  freestyle_scene->r.mode = old_scene->r.mode & ~(R_EDGE_FRS | R_BORDER);
  freestyle_scene->r.xsch = re->rectx;    // old_scene->r.xsch
  freestyle_scene->r.ysch = re->recty;    // old_scene->r.ysch
  freestyle_scene->r.xasp = 1.0f;         // old_scene->r.xasp;
  freestyle_scene->r.yasp = 1.0f;         // old_scene->r.yasp;
  freestyle_scene->r.size = 100;          // old_scene->r.size
  freestyle_scene->r.color_mgt_flag = 0;  // old_scene->r.color_mgt_flag;
  freestyle_scene->r.scemode = (old_scene->r.scemode &
                                ~(R_SINGLE_LAYER | R_NO_FRAME_UPDATE | R_MULTIVIEW)) &
                               (re->r.scemode);
  freestyle_scene->r.flag = old_scene->r.flag;
  freestyle_scene->r.threads = old_scene->r.threads;
  freestyle_scene->r.border.xmin = old_scene->r.border.xmin;
  freestyle_scene->r.border.ymin = old_scene->r.border.ymin;
  freestyle_scene->r.border.xmax = old_scene->r.border.xmax;
  freestyle_scene->r.border.ymax = old_scene->r.border.ymax;
  strcpy(freestyle_scene->r.pic, old_scene->r.pic);
  freestyle_scene->r.dither_intensity = old_scene->r.dither_intensity;
  STRNCPY(freestyle_scene->r.engine, old_scene->r.engine);
  if (G.debug & G_DEBUG_FREESTYLE) {
    cout << "Stroke rendering engine : " << freestyle_scene->r.engine << endl;
  }
  freestyle_scene->r.im_format.planes = R_IMF_PLANES_RGBA;
  freestyle_scene->r.im_format.imtype = R_IMF_IMTYPE_PNG;

  // Copy ID properties, including Cycles render properties
  if (old_scene->id.properties) {
    freestyle_scene->id.properties = IDP_CopyProperty_ex(old_scene->id.properties, 0);
  }
  // Copy eevee render settings.
  BKE_scene_copy_data_eevee(freestyle_scene, old_scene);

  /* Render with transparent background. */
  freestyle_scene->r.alphamode = R_ALPHAPREMUL;

  if (G.debug & G_DEBUG_FREESTYLE) {
    printf("%s: %d thread(s)\n", __func__, BKE_render_num_threads(&freestyle_scene->r));
  }

  BKE_scene_set_background(freestyle_bmain, freestyle_scene);

  // Scene layer.
  ViewLayer *view_layer = (ViewLayer *)freestyle_scene->view_layers.first;
  view_layer->layflag = SCE_LAY_SOLID;

  // Camera
  Object *object_camera = BKE_object_add(freestyle_bmain, view_layer, OB_CAMERA, nullptr);

  Camera *camera = (Camera *)object_camera->data;
  camera->type = CAM_ORTHO;
  camera->ortho_scale = max(re->rectx, re->recty);
  camera->clip_start = 0.1f;
  camera->clip_end = 100.0f;

  _z_delta = 0.00001f;
  _z = camera->clip_start + _z_delta;

  object_camera->loc[0] = re->disprect.xmin + 0.5f * re->rectx;
  object_camera->loc[1] = re->disprect.ymin + 0.5f * re->recty;
  object_camera->loc[2] = 1.0f;

  freestyle_scene->camera = object_camera;

  // Reset serial mesh ID (used for BlenderStrokeRenderer::NewMesh())
  _mesh_id = 0xffffffff;

  // Create a bNodeTree-to-Material hash table
  _nodetree_hash = BLI_ghash_ptr_new("BlenderStrokeRenderer::_nodetree_hash");

  // Depsgraph
  freestyle_depsgraph = DEG_graph_new(
      freestyle_bmain, freestyle_scene, view_layer, DAG_EVAL_RENDER);
  DEG_graph_id_tag_update(freestyle_bmain, freestyle_depsgraph, &freestyle_scene->id, 0);
  DEG_graph_id_tag_update(freestyle_bmain, freestyle_depsgraph, &object_camera->id, 0);
  DEG_graph_tag_relations_update(freestyle_depsgraph);
}

BlenderStrokeRenderer::~BlenderStrokeRenderer()
{
  BLI_ghash_free(_nodetree_hash, nullptr, nullptr);

  DEG_graph_free(freestyle_depsgraph);

  FreeStrokeGroups();

  /* detach the window manager from freestyle bmain (see comments
   * in add_freestyle() for more detail)
   */
  BLI_listbase_clear(&freestyle_bmain->wm);

  BKE_main_free(freestyle_bmain);
}

float BlenderStrokeRenderer::get_stroke_vertex_z() const
{
  float z = _z;
  BlenderStrokeRenderer *self = const_cast<BlenderStrokeRenderer *>(this);
  if (!(_z < _z_delta * 100000.0f)) {
    self->_z_delta *= 10.0f;
  }
  self->_z += _z_delta;
  return -z;
}

unsigned int BlenderStrokeRenderer::get_stroke_mesh_id() const
{
  unsigned mesh_id = _mesh_id;
  BlenderStrokeRenderer *self = const_cast<BlenderStrokeRenderer *>(this);
  self->_mesh_id--;
  return mesh_id;
}

Material *BlenderStrokeRenderer::GetStrokeShader(Main *bmain,
                                                 bNodeTree *iNodeTree,
                                                 bool do_id_user)
{
  Material *ma = BKE_material_add(bmain, "stroke_shader");
  bNodeTree *ntree;
  bNode *output_linestyle = nullptr;
  bNodeSocket *fromsock, *tosock;
  PointerRNA fromptr, toptr;
  NodeShaderAttribute *storage;

  id_us_min(&ma->id);

  if (iNodeTree) {
    // make a copy of linestyle->nodetree
    ntree = ntreeCopyTree_ex(iNodeTree, bmain, do_id_user);

    // find the active Output Line Style node
    for (bNode *node = (bNode *)ntree->nodes.first; node; node = node->next) {
      if (node->type == SH_NODE_OUTPUT_LINESTYLE && (node->flag & NODE_DO_OUTPUT)) {
        output_linestyle = node;
        break;
      }
    }
  }
  else {
    ntree = ntreeAddTree(nullptr, "stroke_shader", "ShaderNodeTree");
  }
  ma->nodetree = ntree;
  ma->use_nodes = 1;
  ma->blend_method = MA_BM_HASHED;

  bNode *input_attr_color = nodeAddStaticNode(nullptr, ntree, SH_NODE_ATTRIBUTE);
  input_attr_color->locx = 0.0f;
  input_attr_color->locy = -200.0f;
  storage = (NodeShaderAttribute *)input_attr_color->storage;
  BLI_strncpy(storage->name, "Color", sizeof(storage->name));

  bNode *mix_rgb_color = nodeAddStaticNode(nullptr, ntree, SH_NODE_MIX_RGB);
  mix_rgb_color->custom1 = MA_RAMP_BLEND;  // Mix
  mix_rgb_color->locx = 200.0f;
  mix_rgb_color->locy = -200.0f;
  tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_color->inputs, 0);  // Fac
  RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, tosock, &toptr);
  RNA_float_set(&toptr, "default_value", 0.0f);

  bNode *input_attr_alpha = nodeAddStaticNode(nullptr, ntree, SH_NODE_ATTRIBUTE);
  input_attr_alpha->locx = 400.0f;
  input_attr_alpha->locy = 300.0f;
  storage = (NodeShaderAttribute *)input_attr_alpha->storage;
  BLI_strncpy(storage->name, "Alpha", sizeof(storage->name));

  bNode *mix_rgb_alpha = nodeAddStaticNode(nullptr, ntree, SH_NODE_MIX_RGB);
  mix_rgb_alpha->custom1 = MA_RAMP_BLEND;  // Mix
  mix_rgb_alpha->locx = 600.0f;
  mix_rgb_alpha->locy = 300.0f;
  tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_alpha->inputs, 0);  // Fac
  RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, tosock, &toptr);
  RNA_float_set(&toptr, "default_value", 0.0f);

  bNode *shader_emission = nodeAddStaticNode(nullptr, ntree, SH_NODE_EMISSION);
  shader_emission->locx = 400.0f;
  shader_emission->locy = -200.0f;

  bNode *input_light_path = nodeAddStaticNode(nullptr, ntree, SH_NODE_LIGHT_PATH);
  input_light_path->locx = 400.0f;
  input_light_path->locy = 100.0f;

  bNode *mix_shader_color = nodeAddStaticNode(nullptr, ntree, SH_NODE_MIX_SHADER);
  mix_shader_color->locx = 600.0f;
  mix_shader_color->locy = -100.0f;

  bNode *shader_transparent = nodeAddStaticNode(nullptr, ntree, SH_NODE_BSDF_TRANSPARENT);
  shader_transparent->locx = 600.0f;
  shader_transparent->locy = 100.0f;

  bNode *mix_shader_alpha = nodeAddStaticNode(nullptr, ntree, SH_NODE_MIX_SHADER);
  mix_shader_alpha->locx = 800.0f;
  mix_shader_alpha->locy = 100.0f;

  bNode *output_material = nodeAddStaticNode(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  output_material->locx = 1000.0f;
  output_material->locy = 100.0f;

  fromsock = (bNodeSocket *)BLI_findlink(&input_attr_color->outputs, 0);  // Color
  tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_color->inputs, 1);        // Color1
  nodeAddLink(ntree, input_attr_color, fromsock, mix_rgb_color, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&mix_rgb_color->outputs, 0);  // Color
  tosock = (bNodeSocket *)BLI_findlink(&shader_emission->inputs, 0);   // Color
  nodeAddLink(ntree, mix_rgb_color, fromsock, shader_emission, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&shader_emission->outputs, 0);  // Emission
  tosock = (bNodeSocket *)BLI_findlink(&mix_shader_color->inputs, 2);    // Shader (second)
  nodeAddLink(ntree, shader_emission, fromsock, mix_shader_color, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&input_light_path->outputs, 0);  // In Camera Ray
  tosock = (bNodeSocket *)BLI_findlink(&mix_shader_color->inputs, 0);     // Fac
  nodeAddLink(ntree, input_light_path, fromsock, mix_shader_color, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&mix_rgb_alpha->outputs, 0);  // Color
  tosock = (bNodeSocket *)BLI_findlink(&mix_shader_alpha->inputs, 0);  // Fac
  nodeAddLink(ntree, mix_rgb_alpha, fromsock, mix_shader_alpha, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&input_attr_alpha->outputs, 0);  // Color
  tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_alpha->inputs, 1);        // Color1
  nodeAddLink(ntree, input_attr_alpha, fromsock, mix_rgb_alpha, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&shader_transparent->outputs, 0);  // BSDF
  tosock = (bNodeSocket *)BLI_findlink(&mix_shader_alpha->inputs, 1);       // Shader (first)
  nodeAddLink(ntree, shader_transparent, fromsock, mix_shader_alpha, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&mix_shader_color->outputs, 0);  // Shader
  tosock = (bNodeSocket *)BLI_findlink(&mix_shader_alpha->inputs, 2);     // Shader (second)
  nodeAddLink(ntree, mix_shader_color, fromsock, mix_shader_alpha, tosock);

  fromsock = (bNodeSocket *)BLI_findlink(&mix_shader_alpha->outputs, 0);  // Shader
  tosock = (bNodeSocket *)BLI_findlink(&output_material->inputs, 0);      // Surface
  nodeAddLink(ntree, mix_shader_alpha, fromsock, output_material, tosock);

  if (output_linestyle) {
    bNodeSocket *outsock;
    bNodeLink *link;

    mix_rgb_color->custom1 = output_linestyle->custom1;  // blend_type
    mix_rgb_color->custom2 = output_linestyle->custom2;  // use_clamp

    outsock = (bNodeSocket *)BLI_findlink(&output_linestyle->inputs, 0);  // Color
    tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_color->inputs, 2);      // Color2
    link = (bNodeLink *)BLI_findptr(&ntree->links, outsock, offsetof(bNodeLink, tosock));
    if (link) {
      nodeAddLink(ntree, link->fromnode, link->fromsock, mix_rgb_color, tosock);
    }
    else {
      float color[4];
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, outsock, &fromptr);
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, tosock, &toptr);
      RNA_float_get_array(&fromptr, "default_value", color);
      RNA_float_set_array(&toptr, "default_value", color);
    }

    outsock = (bNodeSocket *)BLI_findlink(&output_linestyle->inputs, 1);  // Color Fac
    tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_color->inputs, 0);      // Fac
    link = (bNodeLink *)BLI_findptr(&ntree->links, outsock, offsetof(bNodeLink, tosock));
    if (link) {
      nodeAddLink(ntree, link->fromnode, link->fromsock, mix_rgb_color, tosock);
    }
    else {
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, outsock, &fromptr);
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, tosock, &toptr);
      RNA_float_set(&toptr, "default_value", RNA_float_get(&fromptr, "default_value"));
    }

    outsock = (bNodeSocket *)BLI_findlink(&output_linestyle->inputs, 2);  // Alpha
    tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_alpha->inputs, 2);      // Color2
    link = (bNodeLink *)BLI_findptr(&ntree->links, outsock, offsetof(bNodeLink, tosock));
    if (link) {
      nodeAddLink(ntree, link->fromnode, link->fromsock, mix_rgb_alpha, tosock);
    }
    else {
      float color[4];
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, outsock, &fromptr);
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, tosock, &toptr);
      color[0] = color[1] = color[2] = RNA_float_get(&fromptr, "default_value");
      color[3] = 1.0f;
      RNA_float_set_array(&toptr, "default_value", color);
    }

    outsock = (bNodeSocket *)BLI_findlink(&output_linestyle->inputs, 3);  // Alpha Fac
    tosock = (bNodeSocket *)BLI_findlink(&mix_rgb_alpha->inputs, 0);      // Fac
    link = (bNodeLink *)BLI_findptr(&ntree->links, outsock, offsetof(bNodeLink, tosock));
    if (link) {
      nodeAddLink(ntree, link->fromnode, link->fromsock, mix_rgb_alpha, tosock);
    }
    else {
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, outsock, &fromptr);
      RNA_pointer_create((ID *)ntree, &RNA_NodeSocket, tosock, &toptr);
      RNA_float_set(&toptr, "default_value", RNA_float_get(&fromptr, "default_value"));
    }

    for (bNode *node = (bNode *)ntree->nodes.first; node; node = node->next) {
      if (node->type == SH_NODE_UVALONGSTROKE) {
        // UV output of the UV Along Stroke node
        bNodeSocket *sock = (bNodeSocket *)BLI_findlink(&node->outputs, 0);

        // add new UV Map node
        bNode *input_uvmap = nodeAddStaticNode(nullptr, ntree, SH_NODE_UVMAP);
        input_uvmap->locx = node->locx - 200.0f;
        input_uvmap->locy = node->locy;
        NodeShaderUVMap *storage = (NodeShaderUVMap *)input_uvmap->storage;
        if (node->custom1 & 1) {  // use_tips
          BLI_strncpy(storage->uv_map, uvNames[1], sizeof(storage->uv_map));
        }
        else {
          BLI_strncpy(storage->uv_map, uvNames[0], sizeof(storage->uv_map));
        }
        fromsock = (bNodeSocket *)BLI_findlink(&input_uvmap->outputs, 0);  // UV

        // replace links from the UV Along Stroke node by links from the UV Map node
        for (bNodeLink *link = (bNodeLink *)ntree->links.first; link; link = link->next) {
          if (link->fromnode == node && link->fromsock == sock) {
            nodeAddLink(ntree, input_uvmap, fromsock, link->tonode, link->tosock);
          }
        }
        nodeRemSocketLinks(ntree, sock);
      }
    }
  }

  nodeSetActive(ntree, output_material);
  BKE_ntree_update_main_tree(bmain, ntree, nullptr);

  return ma;
}

void BlenderStrokeRenderer::RenderStrokeRep(StrokeRep *iStrokeRep) const
{
  RenderStrokeRepBasic(iStrokeRep);
}

void BlenderStrokeRenderer::RenderStrokeRepBasic(StrokeRep *iStrokeRep) const
{
  bNodeTree *nt = iStrokeRep->getNodeTree();
  Material *ma = (Material *)BLI_ghash_lookup(_nodetree_hash, nt);
  if (!ma) {
    ma = BlenderStrokeRenderer::GetStrokeShader(freestyle_bmain, nt, false);
    BLI_ghash_insert(_nodetree_hash, nt, ma);
  }
  iStrokeRep->setMaterial(ma);

  const vector<Strip *> &strips = iStrokeRep->getStrips();
  const bool hasTex = iStrokeRep->hasTex();
  int totvert = 0, totedge = 0, totpoly = 0, totloop = 0;
  int visible_faces, visible_segments;
  for (vector<Strip *>::const_iterator s = strips.begin(), send = strips.end(); s != send; ++s) {
    Strip::vertex_container &strip_vertices = (*s)->vertices();

    // count visible faces and strip segments
    test_strip_visibility(strip_vertices, &visible_faces, &visible_segments);
    if (visible_faces == 0) {
      continue;
    }

    totvert += visible_faces + visible_segments * 2;
    totedge += visible_faces * 2 + visible_segments;
    totpoly += visible_faces;
    totloop += visible_faces * 3;
  }

  BlenderStrokeRenderer *self = const_cast<BlenderStrokeRenderer *>(this);  // FIXME
  vector<StrokeGroup *> *groups = hasTex ? &self->texturedStrokeGroups : &self->strokeGroups;
  StrokeGroup *group;
  if (groups->empty() || !(groups->back()->totvert + totvert < MESH_MAX_VERTS &&
                           groups->back()->materials.size() + 1 < MAXMAT)) {
    group = new StrokeGroup;
    groups->push_back(group);
  }
  else {
    group = groups->back();
  }
  group->strokes.push_back(iStrokeRep);
  group->totvert += totvert;
  group->totedge += totedge;
  group->totpoly += totpoly;
  group->totloop += totloop;

  if (!group->materials.contains(ma)) {
    group->materials.add_new(ma, group->materials.size());
  }
}

// Check if the triangle is visible (i.e., within the render image boundary)
bool BlenderStrokeRenderer::test_triangle_visibility(StrokeVertexRep *svRep[3]) const
{
  int xl, xu, yl, yu;
  Vec2r p;

  xl = xu = yl = yu = 0;
  for (int i = 0; i < 3; i++) {
    p = svRep[i]->point2d();
    if (p[0] < 0.0) {
      xl++;
    }
    else if (p[0] > _width) {
      xu++;
    }
    if (p[1] < 0.0) {
      yl++;
    }
    else if (p[1] > _height) {
      yu++;
    }
  }
  return !(xl == 3 || xu == 3 || yl == 3 || yu == 3);
}

// Check the visibility of faces and strip segments.
void BlenderStrokeRenderer::test_strip_visibility(Strip::vertex_container &strip_vertices,
                                                  int *visible_faces,
                                                  int *visible_segments) const
{
  const int strip_vertex_count = strip_vertices.size();
  Strip::vertex_container::iterator v[3];
  StrokeVertexRep *svRep[3];
  bool visible;

  /* Iterate over all vertices and count visible faces and strip segments
   * (NOTE: a strip segment is a series of visible faces, while two strip
   * segments are separated by one or more invisible faces). */
  v[0] = strip_vertices.begin();
  v[1] = v[0] + 1;
  v[2] = v[0] + 2;
  *visible_faces = *visible_segments = 0;
  visible = false;
  for (int n = 2; n < strip_vertex_count; n++, v[0]++, v[1]++, v[2]++) {
    svRep[0] = *(v[0]);
    svRep[1] = *(v[1]);
    svRep[2] = *(v[2]);
    if (test_triangle_visibility(svRep)) {
      (*visible_faces)++;
      if (!visible) {
        (*visible_segments)++;
      }
      visible = true;
    }
    else {
      visible = false;
    }
  }
}

// Release allocated memory for stroke groups
void BlenderStrokeRenderer::FreeStrokeGroups()
{
  vector<StrokeGroup *>::const_iterator it, itend;

  for (it = strokeGroups.begin(), itend = strokeGroups.end(); it != itend; ++it) {
    delete (*it);
  }
  for (it = texturedStrokeGroups.begin(), itend = texturedStrokeGroups.end(); it != itend; ++it) {
    delete (*it);
  }
}

// Build a scene populated by mesh objects representing stylized strokes
int BlenderStrokeRenderer::GenerateScene()
{
  vector<StrokeGroup *>::const_iterator it, itend;

  for (it = strokeGroups.begin(), itend = strokeGroups.end(); it != itend; ++it) {
    GenerateStrokeMesh(*it, false);
  }
  for (it = texturedStrokeGroups.begin(), itend = texturedStrokeGroups.end(); it != itend; ++it) {
    GenerateStrokeMesh(*it, true);
  }
  return get_stroke_count();
}

// Return the number of strokes
int BlenderStrokeRenderer::get_stroke_count() const
{
  return strokeGroups.size() + texturedStrokeGroups.size();
}

// Build a mesh object representing a group of stylized strokes
void BlenderStrokeRenderer::GenerateStrokeMesh(StrokeGroup *group, bool hasTex)
{
#if 0
  Object *object_mesh = BKE_object_add(
      freestyle_bmain, (ViewLayer *)freestyle_scene->view_layers.first, OB_MESH);
  DEG_relations_tag_update(freestyle_bmain);
#else
  Object *object_mesh = NewMesh();
#endif
  Mesh *mesh = (Mesh *)object_mesh->data;

  mesh->totvert = group->totvert;
  mesh->totedge = group->totedge;
  mesh->totpoly = group->totpoly;
  mesh->totloop = group->totloop;
  mesh->totcol = group->materials.size();

  mesh->mvert = (MVert *)CustomData_add_layer(
      &mesh->vdata, CD_MVERT, CD_CALLOC, nullptr, mesh->totvert);
  mesh->medge = (MEdge *)CustomData_add_layer(
      &mesh->edata, CD_MEDGE, CD_CALLOC, nullptr, mesh->totedge);
  mesh->mpoly = (MPoly *)CustomData_add_layer(
      &mesh->pdata, CD_MPOLY, CD_CALLOC, nullptr, mesh->totpoly);
  mesh->mloop = (MLoop *)CustomData_add_layer(
      &mesh->ldata, CD_MLOOP, CD_CALLOC, nullptr, mesh->totloop);

  MVert *vertices = mesh->mvert;
  MEdge *edges = mesh->medge;
  MPoly *polys = mesh->mpoly;
  MLoop *loops = mesh->mloop;
  MLoopUV *loopsuv[2] = {nullptr};

  if (hasTex) {
    // First UV layer
    CustomData_add_layer_named(
        &mesh->ldata, CD_MLOOPUV, CD_CALLOC, nullptr, mesh->totloop, uvNames[0]);
    CustomData_set_layer_active(&mesh->ldata, CD_MLOOPUV, 0);
    BKE_mesh_update_customdata_pointers(mesh, true);
    loopsuv[0] = mesh->mloopuv;

    // Second UV layer
    CustomData_add_layer_named(
        &mesh->ldata, CD_MLOOPUV, CD_CALLOC, nullptr, mesh->totloop, uvNames[1]);
    CustomData_set_layer_active(&mesh->ldata, CD_MLOOPUV, 1);
    BKE_mesh_update_customdata_pointers(mesh, true);
    loopsuv[1] = mesh->mloopuv;
  }

  // colors and transparency (the latter represented by grayscale colors)
  MLoopCol *colors = (MLoopCol *)CustomData_add_layer_named(
      &mesh->ldata, CD_PROP_BYTE_COLOR, CD_CALLOC, nullptr, mesh->totloop, "Color");
  MLoopCol *transp = (MLoopCol *)CustomData_add_layer_named(
      &mesh->ldata, CD_PROP_BYTE_COLOR, CD_CALLOC, nullptr, mesh->totloop, "Alpha");
  mesh->mloopcol = colors;

  mesh->mat = (Material **)MEM_mallocN(sizeof(Material *) * mesh->totcol, "MaterialList");
  for (const auto item : group->materials.items()) {
    Material *material = item.key;
    const int matnr = item.value;
    mesh->mat[matnr] = material;
    if (material) {
      id_us_plus(&material->id);
    }
  }

  ////////////////////
  //  Data copy
  ////////////////////

  int vertex_index = 0, edge_index = 0, loop_index = 0;
  int visible_faces, visible_segments;
  bool visible;
  Strip::vertex_container::iterator v[3];
  StrokeVertexRep *svRep[3];
  Vec2r p;

  for (vector<StrokeRep *>::const_iterator it = group->strokes.begin(),
                                           itend = group->strokes.end();
       it != itend;
       ++it) {
    const int matnr = group->materials.lookup_default((*it)->getMaterial(), 0);

    vector<Strip *> &strips = (*it)->getStrips();
    for (vector<Strip *>::const_iterator s = strips.begin(), send = strips.end(); s != send; ++s) {
      Strip::vertex_container &strip_vertices = (*s)->vertices();
      int strip_vertex_count = strip_vertices.size();

      // count visible faces and strip segments
      test_strip_visibility(strip_vertices, &visible_faces, &visible_segments);
      if (visible_faces == 0) {
        continue;
      }

      v[0] = strip_vertices.begin();
      v[1] = v[0] + 1;
      v[2] = v[0] + 2;

      visible = false;

      // NOTE: Mesh generation in the following loop assumes stroke strips
      // to be triangle strips.
      for (int n = 2; n < strip_vertex_count; n++, v[0]++, v[1]++, v[2]++) {
        svRep[0] = *(v[0]);
        svRep[1] = *(v[1]);
        svRep[2] = *(v[2]);
        if (!test_triangle_visibility(svRep)) {
          visible = false;
        }
        else {
          if (!visible) {
            // first vertex
            vertices->co[0] = svRep[0]->point2d()[0];
            vertices->co[1] = svRep[0]->point2d()[1];
            vertices->co[2] = get_stroke_vertex_z();

            ++vertices;
            ++vertex_index;

            // second vertex
            vertices->co[0] = svRep[1]->point2d()[0];
            vertices->co[1] = svRep[1]->point2d()[1];
            vertices->co[2] = get_stroke_vertex_z();

            ++vertices;
            ++vertex_index;

            // first edge
            edges->v1 = vertex_index - 2;
            edges->v2 = vertex_index - 1;
            ++edges;
            ++edge_index;
          }
          visible = true;

          // vertex
          vertices->co[0] = svRep[2]->point2d()[0];
          vertices->co[1] = svRep[2]->point2d()[1];
          vertices->co[2] = get_stroke_vertex_z();
          ++vertices;
          ++vertex_index;

          // edges
          edges->v1 = vertex_index - 1;
          edges->v2 = vertex_index - 3;
          ++edges;
          ++edge_index;

          edges->v1 = vertex_index - 1;
          edges->v2 = vertex_index - 2;
          ++edges;
          ++edge_index;

          // poly
          polys->loopstart = loop_index;
          polys->totloop = 3;
          polys->mat_nr = matnr;
          ++polys;

          // Even and odd loops connect triangles vertices differently
          bool is_odd = n % 2;
          // loops
          if (is_odd) {
            loops[0].v = vertex_index - 1;
            loops[0].e = edge_index - 2;

            loops[1].v = vertex_index - 3;
            loops[1].e = edge_index - 3;

            loops[2].v = vertex_index - 2;
            loops[2].e = edge_index - 1;
          }
          else {
            loops[0].v = vertex_index - 1;
            loops[0].e = edge_index - 1;

            loops[1].v = vertex_index - 2;
            loops[1].e = edge_index - 3;

            loops[2].v = vertex_index - 3;
            loops[2].e = edge_index - 2;
          }
          loops += 3;
          loop_index += 3;

          // UV
          if (hasTex) {
            // First UV layer (loopsuv[0]) has no tips (texCoord(0)).
            // Second UV layer (loopsuv[1]) has tips:  (texCoord(1)).
            for (int L = 0; L < 2; L++) {
              if (is_odd) {
                loopsuv[L][0].uv[0] = svRep[2]->texCoord(L).x();
                loopsuv[L][0].uv[1] = svRep[2]->texCoord(L).y();

                loopsuv[L][1].uv[0] = svRep[0]->texCoord(L).x();
                loopsuv[L][1].uv[1] = svRep[0]->texCoord(L).y();

                loopsuv[L][2].uv[0] = svRep[1]->texCoord(L).x();
                loopsuv[L][2].uv[1] = svRep[1]->texCoord(L).y();
              }
              else {
                loopsuv[L][0].uv[0] = svRep[2]->texCoord(L).x();
                loopsuv[L][0].uv[1] = svRep[2]->texCoord(L).y();

                loopsuv[L][1].uv[0] = svRep[1]->texCoord(L).x();
                loopsuv[L][1].uv[1] = svRep[1]->texCoord(L).y();

                loopsuv[L][2].uv[0] = svRep[0]->texCoord(L).x();
                loopsuv[L][2].uv[1] = svRep[0]->texCoord(L).y();
              }
              loopsuv[L] += 3;
            }
          }

          // colors and alpha transparency. vertex colors are in sRGB
          // space by convention, so convert from linear
          float rgba[3][4];

          for (int i = 0; i < 3; i++) {
            copy_v3fl_v3db(rgba[i], &svRep[i]->color()[0]);
            rgba[i][3] = svRep[i]->alpha();
          }

          if (is_odd) {
            linearrgb_to_srgb_uchar4(&colors[0].r, rgba[2]);
            linearrgb_to_srgb_uchar4(&colors[1].r, rgba[0]);
            linearrgb_to_srgb_uchar4(&colors[2].r, rgba[1]);
          }
          else {
            linearrgb_to_srgb_uchar4(&colors[0].r, rgba[2]);
            linearrgb_to_srgb_uchar4(&colors[1].r, rgba[1]);
            linearrgb_to_srgb_uchar4(&colors[2].r, rgba[0]);
          }
          transp[0].r = transp[0].g = transp[0].b = colors[0].a;
          transp[1].r = transp[1].g = transp[1].b = colors[1].a;
          transp[2].r = transp[2].g = transp[2].b = colors[2].a;
          colors += 3;
          transp += 3;
        }
      }  // loop over strip vertices
    }    // loop over strips
  }      // loop over strokes

  BKE_object_materials_test(freestyle_bmain, object_mesh, (ID *)mesh);
  BKE_mesh_normals_tag_dirty(mesh);

#if 0  // XXX
  BLI_assert(mesh->totvert == vertex_index);
  BLI_assert(mesh->totedge == edge_index);
  BLI_assert(mesh->totloop == loop_index);
  BKE_mesh_validate(mesh, true, true);
#endif
}

// A replacement of BKE_object_add() for better performance.
Object *BlenderStrokeRenderer::NewMesh() const
{
  Object *ob;
  char name[MAX_ID_NAME];
  unsigned int mesh_id = get_stroke_mesh_id();

  BLI_snprintf(name, MAX_ID_NAME, "0%08xOB", mesh_id);
  ob = BKE_object_add_only_object(freestyle_bmain, OB_MESH, name);
  BLI_snprintf(name, MAX_ID_NAME, "0%08xME", mesh_id);
  ob->data = BKE_mesh_add(freestyle_bmain, name);

  Collection *collection_master = freestyle_scene->master_collection;
  BKE_collection_object_add(freestyle_bmain, collection_master, ob);
  DEG_graph_tag_relations_update(freestyle_depsgraph);

  DEG_graph_id_tag_update(freestyle_bmain,
                          freestyle_depsgraph,
                          &ob->id,
                          ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION);

  return ob;
}

Render *BlenderStrokeRenderer::RenderScene(Render * /*re*/, bool render)
{
  Camera *camera = (Camera *)freestyle_scene->camera->data;
  if (camera->clip_end < _z) {
    camera->clip_end = _z + _z_delta * 100.0f;
  }
#if 0
  if (G.debug & G_DEBUG_FREESTYLE) {
    cout << "clip_start " << camera->clip_start << ", clip_end " << camera->clip_end << endl;
  }
#endif

  Render *freestyle_render = RE_NewSceneRender(freestyle_scene);
  DEG_graph_relations_update(freestyle_depsgraph);

  RE_RenderFreestyleStrokes(
      freestyle_render, freestyle_bmain, freestyle_scene, render && get_stroke_count() > 0);

  return freestyle_render;
}

} /* namespace Freestyle */
