/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Code to manage views as part of the regular screen hierarchy. E.g. managing ownership of views
 * inside blocks (#uiBlock.views), looking up items in the region, passing WM notifiers to views,
 * etc.
 *
 * Blocks and their contained views are reconstructed on every redraw. This file also contains
 * functions related to this recreation of views inside blocks. For example to query state
 * information before the view is done reconstructing (#AbstractView.is_reconstructed() returns
 * false), it may be enough to query the previous version of the block/view/view-item. Since such
 * queries rely on the details of the UI reconstruction process, they should remain internal to
 * `interface/` code.
 */

#include <memory>
#include <type_traits>
#include <variant>

#include "DNA_screen_types.h"

#include "BKE_screen.h"

#include "BLI_listbase.h"

#include "ED_screen.h"

#include "interface_intern.h"

#include "UI_interface.hh"

#include "UI_abstract_view.hh"
#include "UI_grid_view.hh"
#include "UI_tree_view.hh"

using namespace blender;
using namespace blender::ui;

/**
 * Wrapper to store views in a #ListBase, addressable via an identifier.
 */
struct ViewLink : public Link {
  std::string idname;
  std::unique_ptr<AbstractView> view;
};

template<class T>
static T *ui_block_add_view_impl(uiBlock &block,
                                 StringRef idname,
                                 std::unique_ptr<AbstractView> view)
{
  ViewLink *view_link = MEM_new<ViewLink>(__func__);
  BLI_addtail(&block.views, view_link);

  view_link->view = std::move(view);
  view_link->idname = idname;

  return dynamic_cast<T *>(view_link->view.get());
}

AbstractGridView *UI_block_add_view(uiBlock &block,
                                    StringRef idname,
                                    std::unique_ptr<AbstractGridView> grid_view)
{
  return ui_block_add_view_impl<AbstractGridView>(block, idname, std::move(grid_view));
}

AbstractTreeView *UI_block_add_view(uiBlock &block,
                                    StringRef idname,
                                    std::unique_ptr<AbstractTreeView> tree_view)
{
  return ui_block_add_view_impl<AbstractTreeView>(block, idname, std::move(tree_view));
}

void ui_block_free_views(uiBlock *block)
{
  LISTBASE_FOREACH_MUTABLE (ViewLink *, link, &block->views) {
    MEM_delete(link);
  }
}

void UI_block_views_listen(const uiBlock *block, const wmRegionListenerParams *listener_params)
{
  ARegion *region = listener_params->region;

  LISTBASE_FOREACH (ViewLink *, view_link, &block->views) {
    if (view_link->view->listen(*listener_params->notifier)) {
      ED_region_tag_redraw(region);
    }
  }
}

uiViewItemHandle *UI_region_views_find_item_at(const ARegion *region, const int xy[2])
{
  uiButViewItem *item_but = (uiButViewItem *)ui_view_item_find_mouse_over(region, xy);
  if (!item_but) {
    return nullptr;
  }

  return item_but->view_item;
}

uiViewItemHandle *UI_region_views_find_active_item(const ARegion *region)
{
  uiButViewItem *item_but = (uiButViewItem *)ui_view_item_find_active(region);
  if (!item_but) {
    return nullptr;
  }

  return item_but->view_item;
}

static StringRef ui_block_view_find_idname(const uiBlock &block, const AbstractView &view)
{
  /* First get the idname the of the view we're looking for. */
  LISTBASE_FOREACH (ViewLink *, view_link, &block.views) {
    if (view_link->view.get() == &view) {
      return view_link->idname;
    }
  }

  return {};
}

template<class T>
static T *ui_block_view_find_matching_in_old_block_impl(const uiBlock &new_block,
                                                        const T &new_view)
{
  uiBlock *old_block = new_block.oldblock;
  if (!old_block) {
    return nullptr;
  }

  StringRef idname = ui_block_view_find_idname(new_block, new_view);
  if (idname.is_empty()) {
    return nullptr;
  }

  LISTBASE_FOREACH (ViewLink *, old_view_link, &old_block->views) {
    if (old_view_link->idname == idname) {
      return dynamic_cast<T *>(old_view_link->view.get());
    }
  }

  return nullptr;
}

uiViewHandle *ui_block_view_find_matching_in_old_block(const uiBlock *new_block,
                                                       const uiViewHandle *new_view_handle)
{
  BLI_assert(new_block && new_view_handle);
  const AbstractView &new_view = reinterpret_cast<const AbstractView &>(*new_view_handle);

  AbstractView *old_view = ui_block_view_find_matching_in_old_block_impl(*new_block, new_view);
  return reinterpret_cast<uiViewHandle *>(old_view);
}

uiButViewItem *ui_block_view_find_matching_view_item_but_in_old_block(
    const uiBlock *new_block, const uiViewItemHandle *new_item_handle)
{
  uiBlock *old_block = new_block->oldblock;
  if (!old_block) {
    return nullptr;
  }

  const AbstractViewItem &new_item = *reinterpret_cast<const AbstractViewItem *>(new_item_handle);
  const AbstractView *old_view = ui_block_view_find_matching_in_old_block_impl(
      *new_block, new_item.get_view());
  if (!old_view) {
    return nullptr;
  }

  LISTBASE_FOREACH (uiBut *, old_but, &old_block->buttons) {
    if (old_but->type != UI_BTYPE_VIEW_ITEM) {
      continue;
    }
    uiButViewItem *old_item_but = (uiButViewItem *)old_but;
    if (!old_item_but->view_item) {
      continue;
    }
    AbstractViewItem &old_item = *reinterpret_cast<AbstractViewItem *>(old_item_but->view_item);
    /* Check if the item is from the expected view. */
    if (&old_item.get_view() != old_view) {
      continue;
    }

    if (UI_view_item_matches(reinterpret_cast<const uiViewItemHandle *>(&new_item),
                             reinterpret_cast<const uiViewItemHandle *>(&old_item))) {
      return old_item_but;
    }
  }

  return nullptr;
}
