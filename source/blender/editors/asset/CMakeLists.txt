# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  ../include
  ../../gpu
  ../../imbuf
  ../../makesrna
  # dna_type_offsets.h
  ${CMAKE_CURRENT_BINARY_DIR}/../../makesdna/intern
  # RNA_prototypes.hh
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
)

set(SRC
  intern/asset_catalog.cc
  intern/asset_filter.cc
  intern/asset_import.cc
  intern/asset_indexer.cc
  intern/asset_library_reference_enum.cc
  intern/asset_library_utils.cc
  intern/asset_list.cc
  intern/asset_mark_clear.cc
  intern/asset_menu_utils.cc
  intern/asset_ops.cc
  intern/asset_shelf.cc
  intern/asset_shelf_asset_view.cc
  intern/asset_shelf_catalog_selector.cc
  intern/asset_shelf_popover.cc
  intern/asset_shelf_regiondata.cc
  intern/asset_shelf_settings.cc
  intern/asset_temp_id_consumer.cc
  intern/asset_type.cc
  intern/asset_ui_utils.cc

  ED_asset_catalog.hh
  ED_asset_filter.hh
  ED_asset_import.hh
  ED_asset_indexer.hh
  ED_asset_library.hh
  ED_asset_list.hh
  ED_asset_mark_clear.hh
  ED_asset_shelf.hh
  ED_asset_temp_id_consumer.hh
  ED_asset_type.hh
  intern/asset_library_reference.hh
  intern/asset_shelf.hh
)

set(LIB
  PRIVATE bf::asset_system
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::blentranslation
  PRIVATE bf::dna
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
)

blender_add_lib(bf_editor_asset "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.hh
add_dependencies(bf_editor_asset bf_rna)
