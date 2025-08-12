/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "WM_types.hh"

#ifdef RNA_RUNTIME

#  include "DNA_object_types.h"
#  include "WM_api.hh"

#  include "usd.hh"

using namespace blender::io::usd;

static StructRNA *rna_USDHook_refine(PointerRNA *ptr)
{
  USDHook *hook = (USDHook *)ptr->data;
  return (hook->rna_ext.srna) ? hook->rna_ext.srna : &RNA_USDHook;
}

static bool rna_USDHook_unregister(Main * /*bmain*/, StructRNA *type)
{
  USDHook *hook = static_cast<USDHook *>(RNA_struct_blender_type_get(type));

  if (hook == nullptr) {
    return false;
  }

  /* free RNA data referencing this */
  RNA_struct_free_extension(type, &hook->rna_ext);
  RNA_struct_free(&BLENDER_RNA, type);

  WM_main_add_notifier(NC_WINDOW, nullptr);

  /* unlink Blender-side data */
  USD_unregister_hook(hook);

  return true;
}

static StructRNA *rna_USDHook_register(Main *bmain,
                                       ReportList *reports,
                                       void *data,
                                       const char *identifier,
                                       StructValidateFunc validate,
                                       StructCallbackFunc call,
                                       StructFreeFunc free)
{
  const char *error_prefix = "Registering USD hook class:";
  USDHook dummy_hook{};

  /* setup dummy type info to store static properties in */
  PointerRNA dummy_hook_ptr = RNA_pointer_create_discrete(nullptr, &RNA_USDHook, &dummy_hook);

  /* validate the python class */
  if (validate(&dummy_hook_ptr, data, nullptr) != 0) {
    return nullptr;
  }

  if (strlen(identifier) >= sizeof(dummy_hook.idname)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "%s '%s' is too long, maximum length is %d",
                error_prefix,
                identifier,
                int(sizeof(dummy_hook.idname)));
    return nullptr;
  }

  /* check if we have registered this hook before, and remove it */
  if (USDHook *hook = USD_find_hook_name(dummy_hook.idname)) {
    BKE_reportf(reports,
                RPT_INFO,
                "%s '%s', bl_idname '%s' has been registered before, unregistering previous",
                error_prefix,
                identifier,
                dummy_hook.idname);

    StructRNA *srna = hook->rna_ext.srna;
    if (!rna_USDHook_unregister(bmain, srna)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "%s '%s', bl_idname '%s' %s",
                  error_prefix,
                  identifier,
                  dummy_hook.idname,
                  "could not be unregistered");
      return nullptr;
    }
  }

  /* create a new KeyingSetInfo type */
  auto hook = std::make_unique<USDHook>();
  *hook = dummy_hook;

  /* set RNA-extensions info */
  hook->rna_ext.srna = RNA_def_struct_ptr(&BLENDER_RNA, hook->idname, &RNA_USDHook);
  hook->rna_ext.data = data;
  hook->rna_ext.call = call;
  hook->rna_ext.free = free;
  RNA_struct_blender_type_set(hook->rna_ext.srna, hook.get());

  /* add and register with other info as needed */
  StructRNA *srna = hook->rna_ext.srna;
  USD_register_hook(std::move(hook));

  WM_main_add_notifier(NC_WINDOW, nullptr);

  /* return the struct-rna added */
  return srna;
}

#else

static void rna_def_usd_hook(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "USDHook", nullptr);
  RNA_def_struct_ui_text(srna, "USD Hook", "Defines callback functions to extend USD IO");
  RNA_def_struct_sdna(srna, "USDHook");
  RNA_def_struct_refine_func(srna, "rna_USDHook_refine");
  RNA_def_struct_register_funcs(srna, "rna_USDHook_register", "rna_USDHook_unregister", nullptr);

  ///* Properties --------------------- */

  RNA_define_verify_sdna(false); /* not in sdna */

  prop = RNA_def_property(srna, "bl_idname", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "idname");
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(prop, "ID Name", "");

  prop = RNA_def_property(srna, "bl_label", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_ui_text(prop, "UI Name", "");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_flag(prop, PROP_REGISTER);

  prop = RNA_def_property(srna, "bl_description", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "description");
  RNA_def_property_string_maxlength(prop, RNA_DYN_DESCR_MAX); /* else it uses the pointer size! */
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop, "Description", "A short description of the USD hook");
}

/* --- */

void RNA_def_usd(BlenderRNA *brna)
{
  rna_def_usd_hook(brna);
}

#endif
