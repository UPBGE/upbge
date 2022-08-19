/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup collada
 */

#include <vector>

#include "BlenderContext.h"
#include "ExportSettings.h"

#include "BKE_scene.h"

bool bc_is_base_node(LinkNode *export_set, Object *ob, ViewLayer *view_layer)
{
  Object *root = bc_get_highest_exported_ancestor_or_self(export_set, ob, view_layer);
  return (root == ob);
}

Object *bc_get_highest_exported_ancestor_or_self(LinkNode *export_set,
                                                 Object *ob,
                                                 ViewLayer *view_layer)
{
  Object *ancestor = ob;
  while (ob->parent) {
    if (bc_is_in_Export_set(export_set, ob->parent, view_layer)) {
      ancestor = ob->parent;
    }
    ob = ob->parent;
  }
  return ancestor;
}

void bc_get_children(std::vector<Object *> &child_set, Object *ob, ViewLayer *view_layer)
{
  Base *base;
  for (base = (Base *)view_layer->object_bases.first; base; base = base->next) {
    Object *cob = base->object;
    if (cob->parent == ob) {
      switch (ob->type) {
        case OB_MESH:
        case OB_CAMERA:
        case OB_LAMP:
        case OB_EMPTY:
        case OB_ARMATURE:
          child_set.push_back(cob);
        default:
          break;
      }
    }
  }
}

bool bc_is_in_Export_set(LinkNode *export_set, Object *ob, ViewLayer *view_layer)
{
  bool to_export = (BLI_linklist_index(export_set, ob) != -1);

  if (!to_export) {
    /* Mark this object as to_export even if it is not in the
     * export list, but it contains children to export. */

    std::vector<Object *> children;
    bc_get_children(children, ob, view_layer);
    for (Object *child : children) {
      if (bc_is_in_Export_set(export_set, child, view_layer)) {
        to_export = true;
        break;
      }
    }
  }
  return to_export;
}

int bc_is_marked(Object *ob)
{
  return ob && (ob->id.tag & LIB_TAG_DOIT);
}

void bc_remove_mark(Object *ob)
{
  ob->id.tag &= ~LIB_TAG_DOIT;
}

void bc_set_mark(Object *ob)
{
  ob->id.tag |= LIB_TAG_DOIT;
}

BlenderContext::BlenderContext(bContext *C)
{
  context = C;
  main = CTX_data_main(C);
  scene = CTX_data_scene(C);
  view_layer = CTX_data_view_layer(C);
  depsgraph = nullptr; /* create only when needed */
}

bContext *BlenderContext::get_context()
{
  return context;
}

Depsgraph *BlenderContext::get_depsgraph()
{
  if (!depsgraph) {
    depsgraph = BKE_scene_ensure_depsgraph(main, scene, view_layer);
  }
  return depsgraph;
}

Scene *BlenderContext::get_scene()
{
  return scene;
}

Scene *BlenderContext::get_evaluated_scene()
{
  Scene *scene_eval = DEG_get_evaluated_scene(get_depsgraph());
  return scene_eval;
}

Object *BlenderContext::get_evaluated_object(Object *ob)
{
  Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob);
  return ob_eval;
}

ViewLayer *BlenderContext::get_view_layer()
{
  return view_layer;
}

Main *BlenderContext::get_main()
{
  return main;
}
