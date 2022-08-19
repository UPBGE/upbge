/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_context.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "WM_api.h"

#include "UI_interface.h"
#include "interface_intern.h"

#include "UI_abstract_view.hh"

namespace blender::ui {

/* ---------------------------------------------------------------------- */
/** \name View Reconstruction
 * \{ */

void AbstractViewItem::update_from_old(const AbstractViewItem &old)
{
  is_active_ = old.is_active_;
  is_renaming_ = old.is_renaming_;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Renaming
 * \{ */

bool AbstractViewItem::supports_renaming() const
{
  /* No renaming by default. */
  return false;
}
bool AbstractViewItem::rename(StringRefNull /*new_name*/)
{
  /* No renaming by default. */
  return false;
}

StringRef AbstractViewItem::get_rename_string() const
{
  /* No rename string by default. */
  return {};
}

bool AbstractViewItem::is_renaming() const
{
  return is_renaming_;
}

void AbstractViewItem::begin_renaming()
{
  AbstractView &view = get_view();
  if (view.is_renaming() || !supports_renaming()) {
    return;
  }

  if (view.begin_renaming()) {
    is_renaming_ = true;
  }

  StringRef initial_str = get_rename_string();
  std::copy(std::begin(initial_str), std::end(initial_str), std::begin(view.get_rename_buffer()));
}

void AbstractViewItem::rename_apply()
{
  const AbstractView &view = get_view();
  rename(view.get_rename_buffer().data());
  end_renaming();
}

void AbstractViewItem::end_renaming()
{
  if (!is_renaming()) {
    return;
  }

  is_renaming_ = false;

  AbstractView &view = get_view();
  view.end_renaming();
}

static AbstractViewItem *find_item_from_rename_button(const uiBut &rename_but)
{
  /* A minimal sanity check, can't do much more here. */
  BLI_assert(rename_but.type == UI_BTYPE_TEXT && rename_but.poin);

  LISTBASE_FOREACH (uiBut *, but, &rename_but.block->buttons) {
    if (but->type != UI_BTYPE_VIEW_ITEM) {
      continue;
    }

    uiButViewItem *view_item_but = (uiButViewItem *)but;
    AbstractViewItem *item = reinterpret_cast<AbstractViewItem *>(view_item_but->view_item);
    const AbstractView &view = item->get_view();

    if (item->is_renaming() && (view.get_rename_buffer().data() == rename_but.poin)) {
      return item;
    }
  }

  return nullptr;
}

static void rename_button_fn(bContext *UNUSED(C), void *arg, char *UNUSED(origstr))
{
  const uiBut *rename_but = static_cast<uiBut *>(arg);
  AbstractViewItem *item = find_item_from_rename_button(*rename_but);
  BLI_assert(item);
  item->rename_apply();
}

void AbstractViewItem::add_rename_button(uiBlock &block)
{
  AbstractView &view = get_view();
  uiBut *rename_but = uiDefBut(&block,
                               UI_BTYPE_TEXT,
                               1,
                               "",
                               0,
                               0,
                               UI_UNIT_X * 10,
                               UI_UNIT_Y,
                               view.get_rename_buffer().data(),
                               1.0f,
                               view.get_rename_buffer().size(),
                               0,
                               0,
                               "");

  /* Gotta be careful with what's passed to the `arg1` here. Any view data will be freed once the
   * callback is executed. */
  UI_but_func_rename_set(rename_but, rename_button_fn, rename_but);
  UI_but_flag_disable(rename_but, UI_BUT_UNDO);

  const bContext *evil_C = reinterpret_cast<bContext *>(block.evil_C);
  ARegion *region = CTX_wm_region(evil_C);
  /* Returns false if the button was removed. */
  if (UI_but_active_only(evil_C, region, &block, rename_but) == false) {
    end_renaming();
  }
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Context Menu
 * \{ */

void AbstractViewItem::build_context_menu(bContext & /*C*/, uiLayout & /*column*/) const
{
  /* No context menu by default. */
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Drag 'n Drop
 * \{ */

std::unique_ptr<AbstractViewItemDragController> AbstractViewItem::create_drag_controller() const
{
  /* There's no drag controller (and hence no drag support) by default. */
  return nullptr;
}

std::unique_ptr<AbstractViewItemDropController> AbstractViewItem::create_drop_controller() const
{
  /* There's no drop controller (and hence no drop support) by default. */
  return nullptr;
}

AbstractViewItemDragController::AbstractViewItemDragController(AbstractView &view) : view_(view)
{
}

void AbstractViewItemDragController::on_drag_start()
{
  /* Do nothing by default. */
}

AbstractViewItemDropController::AbstractViewItemDropController(AbstractView &view) : view_(view)
{
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name General Getters & Setters
 * \{ */

AbstractView &AbstractViewItem::get_view() const
{
  if (UNLIKELY(!view_)) {
    throw std::runtime_error(
        "Invalid state, item must be registered through AbstractView::register_item()");
  }
  return *view_;
}

bool AbstractViewItem::is_active() const
{
  BLI_assert_msg(get_view().is_reconstructed(),
                 "State can't be queried until reconstruction is completed");
  return is_active_;
}

/** \} */

}  // namespace blender::ui

/* ---------------------------------------------------------------------- */
/** \name C-API
 * \{ */

namespace blender::ui {

/**
 * Helper class to provide a higher level public (C-)API. Has access to private/protected view item
 * members and ensures some invariants that way.
 */
class ViewItemAPIWrapper {
 public:
  static bool matches(const AbstractViewItem &a, const AbstractViewItem &b)
  {
    if (typeid(a) != typeid(b)) {
      return false;
    }
    /* TODO should match the view as well. */
    return a.matches(b);
  }

  static bool can_rename(const AbstractViewItem &item)
  {
    const AbstractView &view = item.get_view();
    return !view.is_renaming() && item.supports_renaming();
  }

  static bool drag_start(bContext &C, const AbstractViewItem &item)
  {
    const std::unique_ptr<AbstractViewItemDragController> drag_controller =
        item.create_drag_controller();
    if (!drag_controller) {
      return false;
    }

    WM_event_start_drag(&C,
                        ICON_NONE,
                        drag_controller->get_drag_type(),
                        drag_controller->create_drag_data(),
                        0,
                        WM_DRAG_FREE_DATA);
    drag_controller->on_drag_start();

    return true;
  }

  static bool can_drop(const AbstractViewItem &item,
                       const wmDrag &drag,
                       const char **r_disabled_hint)
  {
    const std::unique_ptr<AbstractViewItemDropController> drop_controller =
        item.create_drop_controller();
    if (!drop_controller) {
      return false;
    }

    return drop_controller->can_drop(drag, r_disabled_hint);
  }

  static std::string drop_tooltip(const AbstractViewItem &item, const wmDrag &drag)
  {
    const std::unique_ptr<AbstractViewItemDropController> drop_controller =
        item.create_drop_controller();
    if (!drop_controller) {
      return {};
    }

    return drop_controller->drop_tooltip(drag);
  }

  static bool drop_handle(bContext &C, const AbstractViewItem &item, const ListBase &drags)
  {
    std::unique_ptr<AbstractViewItemDropController> drop_controller =
        item.create_drop_controller();

    const char *disabled_hint_dummy = nullptr;
    LISTBASE_FOREACH (const wmDrag *, drag, &drags) {
      if (drop_controller->can_drop(*drag, &disabled_hint_dummy)) {
        return drop_controller->on_drop(&C, *drag);
      }
    }

    return false;
  }
};

}  // namespace blender::ui

using namespace blender::ui;

bool UI_view_item_is_active(const uiViewItemHandle *item_handle)
{
  const AbstractViewItem &item = reinterpret_cast<const AbstractViewItem &>(*item_handle);
  return item.is_active();
}

bool UI_view_item_matches(const uiViewItemHandle *a_handle, const uiViewItemHandle *b_handle)
{
  const AbstractViewItem &a = reinterpret_cast<const AbstractViewItem &>(*a_handle);
  const AbstractViewItem &b = reinterpret_cast<const AbstractViewItem &>(*b_handle);
  return ViewItemAPIWrapper::matches(a, b);
}

bool UI_view_item_can_rename(const uiViewItemHandle *item_handle)
{
  const AbstractViewItem &item = reinterpret_cast<const AbstractViewItem &>(*item_handle);
  return ViewItemAPIWrapper::can_rename(item);
}

void UI_view_item_begin_rename(uiViewItemHandle *item_handle)
{
  AbstractViewItem &item = reinterpret_cast<AbstractViewItem &>(*item_handle);
  item.begin_renaming();
}

void UI_view_item_context_menu_build(bContext *C,
                                     const uiViewItemHandle *item_handle,
                                     uiLayout *column)
{
  const AbstractViewItem &item = reinterpret_cast<const AbstractViewItem &>(*item_handle);
  item.build_context_menu(*C, *column);
}

bool UI_view_item_drag_start(bContext *C, const uiViewItemHandle *item_)
{
  const AbstractViewItem &item = reinterpret_cast<const AbstractViewItem &>(*item_);
  return ViewItemAPIWrapper::drag_start(*C, item);
}

bool UI_view_item_can_drop(const uiViewItemHandle *item_,
                           const wmDrag *drag,
                           const char **r_disabled_hint)
{
  const AbstractViewItem &item = reinterpret_cast<const AbstractViewItem &>(*item_);
  return ViewItemAPIWrapper::can_drop(item, *drag, r_disabled_hint);
}

char *UI_view_item_drop_tooltip(const uiViewItemHandle *item_, const wmDrag *drag)
{
  const AbstractViewItem &item = reinterpret_cast<const AbstractViewItem &>(*item_);

  const std::string tooltip = ViewItemAPIWrapper::drop_tooltip(item, *drag);
  return tooltip.empty() ? nullptr : BLI_strdup(tooltip.c_str());
}

bool UI_view_item_drop_handle(bContext *C, const uiViewItemHandle *item_, const ListBase *drags)
{
  const AbstractViewItem &item = reinterpret_cast<const AbstractViewItem &>(*item_);
  return ViewItemAPIWrapper::drop_handle(*C, item, *drags);
}

/** \} */
