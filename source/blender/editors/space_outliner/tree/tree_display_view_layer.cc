/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <iostream>

#include "DNA_collection_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_layer.h"

#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "BLT_translation.h"

#include "../outliner_intern.hh"
#include "common.hh"
#include "tree_display.hh"
#include "tree_element.hh"

namespace blender::ed::outliner {

template<typename T> using List = ListBaseWrapper<T>;

class ObjectsChildrenBuilder {
  using TreeChildren = Vector<TreeElement *>;
  using ObjectTreeElementsMap = Map<Object *, TreeChildren>;

  SpaceOutliner &outliner_;
  ObjectTreeElementsMap object_tree_elements_map_;

 public:
  ObjectsChildrenBuilder(SpaceOutliner &soutliner);
  ~ObjectsChildrenBuilder() = default;

  void operator()(TreeElement &collection_tree_elem);

 private:
  void object_tree_elements_lookup_create_recursive(TreeElement *te_parent);
  void make_object_parent_hierarchy_collections();
};

/* -------------------------------------------------------------------- */
/** \name Tree-Display for a View Layer.
 * \{ */

TreeDisplayViewLayer::TreeDisplayViewLayer(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

bool TreeDisplayViewLayer::supportsModeColumn() const
{
  return true;
}

ListBase TreeDisplayViewLayer::buildTree(const TreeSourceData &source_data)
{
  ListBase tree = {nullptr};
  Scene *scene = source_data.scene;
  show_objects_ = !(space_outliner_.filter & SO_FILTER_NO_OBJECT);

  for (auto *view_layer : ListBaseWrapper<ViewLayer>(scene->view_layers)) {
    view_layer_ = view_layer;

    if (space_outliner_.filter & SO_FILTER_NO_VIEW_LAYERS) {
      if (view_layer != source_data.view_layer) {
        continue;
      }

      add_view_layer(*scene, tree, (TreeElement *)nullptr);
    }
    else {
      TreeElement &te_view_layer = *outliner_add_element(
          &space_outliner_, &tree, scene, nullptr, TSE_R_LAYER, 0);
      TREESTORE(&te_view_layer)->flag &= ~TSE_CLOSED;
      te_view_layer.name = view_layer->name;
      te_view_layer.directdata = view_layer;

      add_view_layer(*scene, te_view_layer.subtree, &te_view_layer);
    }
  }

  return tree;
}

void TreeDisplayViewLayer::add_view_layer(Scene &scene, ListBase &tree, TreeElement *parent)
{
  const bool show_children = (space_outliner_.filter & SO_FILTER_NO_CHILDREN) == 0;

  if (space_outliner_.filter & SO_FILTER_NO_COLLECTION) {
    /* Show objects in the view layer. */
    for (Base *base : List<Base>(view_layer_->object_bases)) {
      TreeElement *te_object = outliner_add_element(
          &space_outliner_, &tree, base->object, parent, TSE_SOME_ID, 0);
      te_object->directdata = base;
    }

    if (show_children) {
      outliner_make_object_parent_hierarchy(&tree);
    }
  }
  else {
    /* Show collections in the view layer. */
    TreeElement &ten = *outliner_add_element(
        &space_outliner_, &tree, &scene, parent, TSE_VIEW_COLLECTION_BASE, 0);
    ten.name = IFACE_("Scene Collection");
    TREESTORE(&ten)->flag &= ~TSE_CLOSED;

    /* First layer collection is for master collection, don't show it. */
    LayerCollection *lc = static_cast<LayerCollection *>(view_layer_->layer_collections.first);
    if (lc == nullptr) {
      return;
    }

    add_layer_collections_recursive(ten.subtree, lc->layer_collections, ten);
    if (show_objects_) {
      add_layer_collection_objects(ten.subtree, *lc, ten);
    }
    if (show_children) {
      add_layer_collection_objects_children(ten);
    }
  }
}

void TreeDisplayViewLayer::add_layer_collections_recursive(ListBase &tree,
                                                           ListBase &layer_collections,
                                                           TreeElement &parent_ten)
{
  for (LayerCollection *lc : List<LayerCollection>(layer_collections)) {
    const bool exclude = (lc->flag & LAYER_COLLECTION_EXCLUDE) != 0;
    TreeElement *ten;

    if (exclude && ((space_outliner_.show_restrict_flags & SO_RESTRICT_ENABLE) == 0)) {
      ten = &parent_ten;
    }
    else {
      ID *id = &lc->collection->id;
      ten = outliner_add_element(
          &space_outliner_, &tree, id, &parent_ten, TSE_LAYER_COLLECTION, 0);

      ten->name = id->name + 2;
      ten->directdata = lc;

      /* Open by default, except linked collections, which may contain many elements. */
      TreeStoreElem *tselem = TREESTORE(ten);
      if (!(tselem->used || ID_IS_LINKED(id) || ID_IS_OVERRIDE_LIBRARY(id))) {
        tselem->flag &= ~TSE_CLOSED;
      }
    }

    add_layer_collections_recursive(ten->subtree, lc->layer_collections, *ten);
    if (!exclude && show_objects_) {
      add_layer_collection_objects(ten->subtree, *lc, *ten);
    }
  }
}

void TreeDisplayViewLayer::add_layer_collection_objects(ListBase &tree,
                                                        LayerCollection &lc,
                                                        TreeElement &ten)
{
  for (CollectionObject *cob : List<CollectionObject>(lc.collection->gobject)) {
    Base *base = BKE_view_layer_base_find(view_layer_, cob->ob);
    TreeElement *te_object = outliner_add_element(
        &space_outliner_, &tree, base->object, &ten, TSE_SOME_ID, 0);
    te_object->directdata = base;
  }
}

void TreeDisplayViewLayer::add_layer_collection_objects_children(TreeElement &collection_tree_elem)
{
  /* Call helper to add children. */
  ObjectsChildrenBuilder child_builder{space_outliner_};
  child_builder(collection_tree_elem);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Children helper.
 *
 * Helper to add child objects to the sub-tree of their parent, recursively covering all nested
 * collections.
 *
 * \{ */

ObjectsChildrenBuilder::ObjectsChildrenBuilder(SpaceOutliner &outliner) : outliner_(outliner)
{
}

void ObjectsChildrenBuilder::operator()(TreeElement &collection_tree_elem)
{
  object_tree_elements_lookup_create_recursive(&collection_tree_elem);
  make_object_parent_hierarchy_collections();
}

/**
 * Build a map from Object* to a list of TreeElement* matching the object.
 */
void ObjectsChildrenBuilder::object_tree_elements_lookup_create_recursive(TreeElement *te_parent)
{
  for (TreeElement *te : List<TreeElement>(te_parent->subtree)) {
    TreeStoreElem *tselem = TREESTORE(te);

    if (tselem->type == TSE_LAYER_COLLECTION) {
      object_tree_elements_lookup_create_recursive(te);
      continue;
    }

    if ((tselem->type == TSE_SOME_ID) && (te->idcode == ID_OB)) {
      Object *ob = (Object *)tselem->id;
      /* Lookup children or add new, empty children vector. */
      Vector<TreeElement *> &tree_elements = object_tree_elements_map_.lookup_or_add(ob, {});

      tree_elements.append(te);
      object_tree_elements_lookup_create_recursive(te);
    }
  }
}

/**
 * For all objects in the tree, lookup the parent in this map,
 * and move or add tree elements as needed.
 */
void ObjectsChildrenBuilder::make_object_parent_hierarchy_collections()
{
  for (ObjectTreeElementsMap::MutableItem item : object_tree_elements_map_.items()) {
    Object *child = item.key;

    if (child->parent == nullptr) {
      continue;
    }

    Vector<TreeElement *> &child_ob_tree_elements = item.value;
    Vector<TreeElement *> *parent_ob_tree_elements = object_tree_elements_map_.lookup_ptr(
        child->parent);
    if (parent_ob_tree_elements == nullptr) {
      continue;
    }

    for (TreeElement *parent_ob_tree_element : *parent_ob_tree_elements) {
      TreeElement *parent_ob_collection_tree_element = nullptr;
      bool found = false;

      /* We always want to remove the child from the direct collection its parent is nested under.
       * This is particularly important when dealing with multi-level nesting (grandchildren). */
      parent_ob_collection_tree_element = parent_ob_tree_element->parent;
      while (!ELEM(TREESTORE(parent_ob_collection_tree_element)->type,
                   TSE_VIEW_COLLECTION_BASE,
                   TSE_LAYER_COLLECTION)) {
        parent_ob_collection_tree_element = parent_ob_collection_tree_element->parent;
      }

      for (TreeElement *child_ob_tree_element : child_ob_tree_elements) {
        if (child_ob_tree_element->parent == parent_ob_collection_tree_element) {
          /* Move from the collection subtree into the parent object subtree. */
          BLI_remlink(&parent_ob_collection_tree_element->subtree, child_ob_tree_element);
          BLI_addtail(&parent_ob_tree_element->subtree, child_ob_tree_element);
          child_ob_tree_element->parent = parent_ob_tree_element;
          found = true;
          break;
        }
      }

      if (!found) {
        /* We add the child in the tree even if it is not in the collection.
         * We don't expand its sub-tree though, to make it less prominent. */
        TreeElement *child_ob_tree_element = outliner_add_element(&outliner_,
                                                                  &parent_ob_tree_element->subtree,
                                                                  child,
                                                                  parent_ob_tree_element,
                                                                  TSE_SOME_ID,
                                                                  0,
                                                                  false);
        child_ob_tree_element->flag |= TE_CHILD_NOT_IN_COLLECTION;
        child_ob_tree_elements.append(child_ob_tree_element);
      }
    }
  }
}

/** \} */

}  // namespace blender::ed::outliner
