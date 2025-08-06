/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Catalog tree-view to enable/disable catalogs in the asset shelf settings.
 */

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"

#include "BLI_string_utf8.h"

#include "DNA_screen_types.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "ED_asset_filter.hh"
#include "ED_asset_list.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"

#include "asset_shelf.hh"

namespace blender::ed::asset::shelf {

class AssetCatalogSelectorTree : public ui::AbstractTreeView {
  AssetShelf &shelf_;
  AssetShelfSettings &shelf_settings_;
  asset_system::AssetCatalogTree catalog_tree_;

 public:
  class Item;

  AssetCatalogSelectorTree(asset_system::AssetLibrary &library, AssetShelf &shelf)
      : shelf_(shelf), shelf_settings_(shelf_.settings)
  {
    catalog_tree_ = build_filtered_catalog_tree(
        library,
        shelf_settings_.asset_library_reference,
        [this](const asset_system::AssetRepresentation &asset) {
          return (!shelf_.type->asset_poll || shelf_.type->asset_poll(shelf_.type, &asset));
        });
  }

  void build_tree() override
  {
    if (catalog_tree_.is_empty()) {
      auto &item = add_tree_item<ui::BasicTreeViewItem>(RPT_("No applicable assets found"),
                                                        ICON_INFO);
      item.disable_interaction();
      return;
    }

    catalog_tree_.foreach_root_item(
        [this](const asset_system::AssetCatalogTreeItem &catalog_item) {
          Item &item = build_catalog_items_recursive(*this, catalog_item);
          item.uncollapse_by_default();
        });
  }

  Item &build_catalog_items_recursive(ui::TreeViewOrItem &parent_view_item,
                                      const asset_system::AssetCatalogTreeItem &catalog_item) const
  {
    Item &view_item = parent_view_item.add_tree_item<Item>(catalog_item, shelf_);

    const int parent_count = view_item.count_parents() + 1;
    catalog_item.foreach_child([&, this](const asset_system::AssetCatalogTreeItem &child) {
      Item &child_item = build_catalog_items_recursive(view_item, child);

      /* Uncollapse to some level (gives quick access, but don't let the tree get too big). */
      if (parent_count < 2) {
        child_item.uncollapse_by_default();
      }
    });

    return view_item;
  }

  void update_shelf_settings_from_enabled_catalogs();

  class Item : public ui::BasicTreeViewItem {
    const asset_system::AssetCatalogTreeItem &catalog_item_;
    /* Is the catalog path enabled in this redraw? Set on construction, updated by the UI (which
     * gets a pointer to it). The UI needs it as char. */
    char catalog_path_enabled_ = false;

   public:
    Item(const asset_system::AssetCatalogTreeItem &catalog_item, AssetShelf &shelf)
        : ui::BasicTreeViewItem(catalog_item.get_name()),
          catalog_item_(catalog_item),
          catalog_path_enabled_(
              settings_is_catalog_path_enabled(shelf, catalog_item.catalog_path()))
    {
      disable_activatable();
    }

    bool is_catalog_path_enabled() const
    {
      return catalog_path_enabled_ != 0;
    }

    bool has_enabled_in_subtree()
    {
      bool has_enabled = false;

      foreach_item_recursive(
          [&has_enabled](const ui::AbstractTreeViewItem &abstract_item) {
            const Item &item = dynamic_cast<const Item &>(abstract_item);
            if (item.is_catalog_path_enabled()) {
              has_enabled = true;
            }
          },
          IterOptions::SkipFiltered);

      return has_enabled;
    }

    asset_system::AssetCatalogPath catalog_path() const
    {
      return catalog_item_.catalog_path();
    }

    void build_row(uiLayout &row) override
    {
      AssetCatalogSelectorTree &tree = dynamic_cast<AssetCatalogSelectorTree &>(get_tree_view());
      uiBlock *block = row.block();

      row.emboss_set(ui::EmbossType::Emboss);

      uiLayout *subrow = &row.row(false);
      subrow->active_set(catalog_path_enabled_);
      subrow->label(catalog_item_.get_name(), ICON_NONE);
      ui::block_layout_set_current(block, &row);

      uiBut *toggle_but = uiDefButC(block,
                                    ButType::Checkbox,
                                    0,
                                    "",
                                    0,
                                    0,
                                    UI_UNIT_X,
                                    UI_UNIT_Y,
                                    &catalog_path_enabled_,
                                    0,
                                    0,
                                    TIP_("Toggle catalog visibility in the asset shelf"));
      UI_but_func_set(toggle_but, [&tree](bContext &C) {
        tree.update_shelf_settings_from_enabled_catalogs();
        send_redraw_notifier(C);
      });
      if (!is_catalog_path_enabled() && has_enabled_in_subtree()) {
        UI_but_drawflag_enable(toggle_but, UI_BUT_INDETERMINATE);
      }
      UI_but_flag_disable(toggle_but, UI_BUT_UNDO);
    }
  };
};

void AssetCatalogSelectorTree::update_shelf_settings_from_enabled_catalogs()
{
  settings_clear_enabled_catalogs(shelf_);
  foreach_item([this](ui::AbstractTreeViewItem &view_item) {
    const auto &selector_tree_item = dynamic_cast<AssetCatalogSelectorTree::Item &>(view_item);
    if (selector_tree_item.is_catalog_path_enabled()) {
      settings_set_catalog_path_enabled(shelf_, selector_tree_item.catalog_path());
    }
  });
}

void library_selector_draw(const bContext *C, uiLayout *layout, AssetShelf &shelf)
{
  layout->operator_context_set(wm::OpCallContext::InvokeDefault);

  PointerRNA shelf_ptr = RNA_pointer_create_discrete(
      &CTX_wm_screen(C)->id, &RNA_AssetShelf, &shelf);

  uiLayout *row = &layout->row(true);
  row->prop(&shelf_ptr, "asset_library_reference", UI_ITEM_NONE, "", ICON_NONE);
  if (shelf.settings.asset_library_reference.type != ASSET_LIBRARY_LOCAL) {
    row->op("ASSET_OT_library_refresh", "", ICON_FILE_REFRESH);
  }
}

static void catalog_selector_panel_draw(const bContext *C, Panel *panel)
{
  AssetShelf *shelf = active_shelf_from_context(C);
  if (!shelf) {
    return;
  }

  uiLayout *layout = panel->layout;

  library_selector_draw(C, layout, *shelf);

  asset_system::AssetLibrary *library = list::library_get_once_available(
      shelf->settings.asset_library_reference);
  if (!library) {
    return;
  }

  uiBlock *block = layout->block();
  ui::AbstractTreeView *tree_view = UI_block_add_view(
      *block,
      "asset catalog tree view",
      std::make_unique<AssetCatalogSelectorTree>(*library, *shelf));
  tree_view->set_context_menu_title("Catalog");
  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}

void catalog_selector_panel_register(ARegionType *region_type)
{
  /* Uses global paneltype registry to allow usage as popover. So only register this once (may be
   * called from multiple spaces). */
  if (WM_paneltype_find("ASSETSHELF_PT_catalog_selector", true)) {
    return;
  }

  PanelType *pt = MEM_callocN<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "ASSETSHELF_PT_catalog_selector");
  STRNCPY_UTF8(pt->label, N_("Catalog Selector"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_(
      "Select the asset library and the contained catalogs to display in the asset shelf");
  pt->draw = catalog_selector_panel_draw;
  pt->listener = asset::list::asset_reading_region_listen_fn;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

}  // namespace blender::ed::asset::shelf
