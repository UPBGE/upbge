/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include "DNA_layer_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BLT_translation.h"

#include "ED_object.h"
#include "ED_render.h"

#include "RE_engine.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_define.h"

#include "rna_internal.h"

#ifdef RNA_RUNTIME

#  ifdef WITH_PYTHON
#    include "BPY_extern.h"
#  endif

#  include "DNA_collection_types.h"
#  include "DNA_object_types.h"

#  include "RNA_access.h"

#  include "BKE_idprop.h"
#  include "BKE_layer.h"
#  include "BKE_mesh.h"
#  include "BKE_node.h"
#  include "BKE_scene.h"

#  include "NOD_composite.h"

#  include "BLI_listbase.h"

#  include "DEG_depsgraph_build.h"
#  include "DEG_depsgraph_query.h"

/***********************************/

static PointerRNA rna_ViewLayer_active_layer_collection_get(PointerRNA *ptr)
{
  ViewLayer *view_layer = (ViewLayer *)ptr->data;
  LayerCollection *lc = view_layer->active_collection;
  return rna_pointer_inherit_refine(ptr, &RNA_LayerCollection, lc);
}

static void rna_ViewLayer_active_layer_collection_set(PointerRNA *ptr,
                                                      PointerRNA value,
                                                      struct ReportList *UNUSED(reports))
{
  ViewLayer *view_layer = (ViewLayer *)ptr->data;
  LayerCollection *lc = (LayerCollection *)value.data;
  const int index = BKE_layer_collection_findindex(view_layer, lc);
  if (index != -1) {
    BKE_layer_collection_activate(view_layer, lc);
  }
}

static PointerRNA rna_LayerObjects_active_object_get(PointerRNA *ptr)
{
  ViewLayer *view_layer = (ViewLayer *)ptr->data;
  return rna_pointer_inherit_refine(
      ptr, &RNA_Object, view_layer->basact ? view_layer->basact->object : NULL);
}

static void rna_LayerObjects_active_object_set(PointerRNA *ptr,
                                               PointerRNA value,
                                               struct ReportList *reports)
{
  ViewLayer *view_layer = (ViewLayer *)ptr->data;
  if (value.data) {
    Object *ob = value.data;
    Base *basact_test = BKE_view_layer_base_find(view_layer, ob);
    if (basact_test != NULL) {
      view_layer->basact = basact_test;
    }
    else {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "ViewLayer '%s' does not contain object '%s'",
                  view_layer->name,
                  ob->id.name + 2);
    }
  }
  else {
    view_layer->basact = NULL;
  }
}

size_t rna_ViewLayer_path_buffer_get(const ViewLayer *view_layer,
                                     char *r_rna_path,
                                     const size_t rna_path_buffer_size)
{
  char name_esc[sizeof(view_layer->name) * 2];
  BLI_str_escape(name_esc, view_layer->name, sizeof(name_esc));

  return BLI_snprintf_rlen(r_rna_path, rna_path_buffer_size, "view_layers[\"%s\"]", name_esc);
}

static char *rna_ViewLayer_path(const PointerRNA *ptr)
{
  const ViewLayer *view_layer = (ViewLayer *)ptr->data;
  char rna_path[sizeof(view_layer->name) * 3];

  rna_ViewLayer_path_buffer_get(view_layer, rna_path, sizeof(rna_path));

  return BLI_strdup(rna_path);
}

static IDProperty **rna_ViewLayer_idprops(PointerRNA *ptr)
{
  ViewLayer *view_layer = (ViewLayer *)ptr->data;
  return &view_layer->id_properties;
}

static bool rna_LayerCollection_visible_get(LayerCollection *layer_collection, bContext *C)
{
  View3D *v3d = CTX_wm_view3d(C);

  if ((v3d == NULL) || ((v3d->flag & V3D_LOCAL_COLLECTIONS) == 0)) {
    return (layer_collection->runtime_flag & LAYER_COLLECTION_VISIBLE_VIEW_LAYER) != 0;
  }

  if (v3d->local_collections_uuid & layer_collection->local_collections_bits) {
    return (layer_collection->runtime_flag & LAYER_COLLECTION_HIDE_VIEWPORT) == 0;
  }

  return false;
}

static void rna_ViewLayer_update_render_passes(ID *id)
{
  Scene *scene = (Scene *)id;
  if (scene->nodetree) {
    ntreeCompositUpdateRLayers(scene->nodetree);
  }

  RenderEngineType *engine_type = RE_engines_find(scene->r.engine);
  if (engine_type->update_render_passes) {
    RenderEngine *engine = RE_engine_create(engine_type);
    if (engine) {
      LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
        BKE_view_layer_verify_aov(engine, scene, view_layer);
      }
    }
    RE_engine_free(engine);
    engine = NULL;
  }
}

static PointerRNA rna_ViewLayer_objects_get(CollectionPropertyIterator *iter)
{
  ListBaseIterator *internal = &iter->internal.listbase;

  /* we are actually iterating a ObjectBase list */
  Base *base = (Base *)internal->link;
  return rna_pointer_inherit_refine(&iter->parent, &RNA_Object, base->object);
}

static int rna_ViewLayer_objects_selected_skip(CollectionPropertyIterator *iter,
                                               void *UNUSED(data))
{
  ListBaseIterator *internal = &iter->internal.listbase;
  Base *base = (Base *)internal->link;

  if ((base->flag & BASE_SELECTED) != 0) {
    return 0;
  }

  return 1;
};

static PointerRNA rna_ViewLayer_depsgraph_get(PointerRNA *ptr)
{
  ID *id = ptr->owner_id;
  if (GS(id->name) == ID_SCE) {
    Scene *scene = (Scene *)id;
    ViewLayer *view_layer = (ViewLayer *)ptr->data;
    Depsgraph *depsgraph = BKE_scene_get_depsgraph(scene, view_layer);
    return rna_pointer_inherit_refine(ptr, &RNA_Depsgraph, depsgraph);
  }
  return PointerRNA_NULL;
}

static void rna_LayerObjects_selected_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  ViewLayer *view_layer = (ViewLayer *)ptr->data;
  rna_iterator_listbase_begin(
      iter, &view_layer->object_bases, rna_ViewLayer_objects_selected_skip);
}

static void rna_ViewLayer_update_tagged(ID *id_ptr,
                                        ViewLayer *view_layer,
                                        Main *bmain,
                                        ReportList *reports)
{
  Scene *scene = (Scene *)id_ptr;
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);

  if (DEG_is_evaluating(depsgraph)) {
    BKE_report(reports, RPT_ERROR, "Dependency graph update requested during evaluation");
    return;
  }

#  ifdef WITH_PYTHON
  /* Allow drivers to be evaluated */
  BPy_BEGIN_ALLOW_THREADS;
#  endif

  /* NOTE: This is similar to CTX_data_depsgraph_pointer(). Ideally such access would be
   * de-duplicated across all possible cases, but for now this is safest and easiest way to go.
   *
   * The reason for this is that it's possible to have Python operator which asks view layer to
   * be updated. After re-do of such operator view layer's dependency graph will not be marked
   * as active. */
  DEG_make_active(depsgraph);
  BKE_scene_graph_update_tagged(depsgraph, bmain);

#  ifdef WITH_PYTHON
  BPy_END_ALLOW_THREADS;
#  endif
}

static void rna_ObjectBase_select_update(Main *UNUSED(bmain),
                                         Scene *UNUSED(scene),
                                         PointerRNA *ptr)
{
  Base *base = (Base *)ptr->data;
  short mode = (base->flag & BASE_SELECTED) ? BA_SELECT : BA_DESELECT;
  ED_object_base_select(base, mode);
}

static void rna_ObjectBase_hide_viewport_update(bContext *C, PointerRNA *UNUSED(ptr))
{
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_layer_collection_sync(scene, view_layer);
  DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
}

static void rna_LayerCollection_name_get(struct PointerRNA *ptr, char *value)
{
  ID *id = (ID *)((LayerCollection *)ptr->data)->collection;
  BLI_strncpy(value, id->name + 2, sizeof(id->name) - 2);
}

int rna_LayerCollection_name_length(PointerRNA *ptr)
{
  ID *id = (ID *)((LayerCollection *)ptr->data)->collection;
  return strlen(id->name + 2);
}

static void rna_LayerCollection_flag_set(PointerRNA *ptr, const bool value, const int flag)
{
  LayerCollection *layer_collection = (LayerCollection *)ptr->data;
  Collection *collection = layer_collection->collection;

  if (collection->flag & COLLECTION_IS_MASTER) {
    return;
  }

  if (value) {
    layer_collection->flag |= flag;
  }
  else {
    layer_collection->flag &= ~flag;
  }
}

static void rna_LayerCollection_exclude_set(PointerRNA *ptr, bool value)
{
  rna_LayerCollection_flag_set(ptr, value, LAYER_COLLECTION_EXCLUDE);
}

static void rna_LayerCollection_holdout_set(PointerRNA *ptr, bool value)
{
  rna_LayerCollection_flag_set(ptr, value, LAYER_COLLECTION_HOLDOUT);
}

static void rna_LayerCollection_indirect_only_set(PointerRNA *ptr, bool value)
{
  rna_LayerCollection_flag_set(ptr, value, LAYER_COLLECTION_INDIRECT_ONLY);
}

static void rna_LayerCollection_hide_viewport_set(PointerRNA *ptr, bool value)
{
  rna_LayerCollection_flag_set(ptr, value, LAYER_COLLECTION_HIDE);
}

static void rna_LayerCollection_exclude_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Scene *scene = (Scene *)ptr->owner_id;
  LayerCollection *lc = (LayerCollection *)ptr->data;
  ViewLayer *view_layer = BKE_view_layer_find_from_collection(scene, lc);

  /* Set/Unset it recursively to match the behavior of excluding via the menu or shortcuts. */
  const bool exclude = (lc->flag & LAYER_COLLECTION_EXCLUDE) != 0;
  BKE_layer_collection_set_flag(lc, LAYER_COLLECTION_EXCLUDE, exclude);

  BKE_layer_collection_sync(scene, view_layer);

  DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
  if (!exclude) {
    /* We need to update animation of objects added back to the scene through enabling this view
     * layer. */
    FOREACH_OBJECT_BEGIN (view_layer, ob) {
      DEG_id_tag_update(&ob->id, ID_RECALC_ANIMATION);
    }
    FOREACH_OBJECT_END;
  }

  DEG_relations_tag_update(bmain);
  WM_main_add_notifier(NC_SCENE | ND_LAYER_CONTENT, NULL);
  if (exclude) {
    ED_object_base_active_refresh(bmain, scene, view_layer);
  }
}

static void rna_LayerCollection_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  Scene *scene = (Scene *)ptr->owner_id;
  LayerCollection *lc = (LayerCollection *)ptr->data;
  ViewLayer *view_layer = BKE_view_layer_find_from_collection(scene, lc);

  BKE_layer_collection_sync(scene, view_layer);

  DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);

  WM_main_add_notifier(NC_SCENE | ND_LAYER_CONTENT, NULL);
  WM_main_add_notifier(NC_IMAGE | ND_LAYER_CONTENT, NULL);
}

static bool rna_LayerCollection_has_objects(LayerCollection *lc)
{
  return (lc->runtime_flag & LAYER_COLLECTION_HAS_OBJECTS) != 0;
}

static bool rna_LayerCollection_has_selected_objects(LayerCollection *lc, ViewLayer *view_layer)
{
  return BKE_layer_collection_has_selected_objects(view_layer, lc);
}

#else

static void rna_def_layer_collection(BlenderRNA *brna)
{
  StructRNA *srna;
  FunctionRNA *func;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "LayerCollection", NULL);
  RNA_def_struct_ui_text(srna, "Layer Collection", "Layer collection");
  RNA_def_struct_ui_icon(srna, ICON_OUTLINER_COLLECTION);

  prop = RNA_def_property(srna, "collection", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE | PROP_ANIMATABLE);
  RNA_def_property_struct_type(prop, "Collection");
  RNA_def_property_ui_text(prop, "Collection", "Collection this layer collection is wrapping");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, NULL, "collection->id.name");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE | PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Name", "Name of this view layer (same as its collection one)");
  RNA_def_property_string_funcs(
      prop, "rna_LayerCollection_name_get", "rna_LayerCollection_name_length", NULL);
  RNA_def_struct_name_property(srna, prop);

  prop = RNA_def_property(srna, "children", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "layer_collections", NULL);
  RNA_def_property_struct_type(prop, "LayerCollection");
  RNA_def_property_ui_text(prop, "Children", "Child layer collections");

  /* Restriction flags. */
  prop = RNA_def_property(srna, "exclude", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", LAYER_COLLECTION_EXCLUDE);
  RNA_def_property_boolean_funcs(prop, NULL, "rna_LayerCollection_exclude_set");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Exclude from View Layer", "Exclude from view layer");
  RNA_def_property_ui_icon(prop, ICON_CHECKBOX_HLT, -1);
  RNA_def_property_update(prop, NC_SCENE | ND_LAYER, "rna_LayerCollection_exclude_update");

  prop = RNA_def_property(srna, "holdout", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", LAYER_COLLECTION_HOLDOUT);
  RNA_def_property_boolean_funcs(prop, NULL, "rna_LayerCollection_holdout_set");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_icon(prop, ICON_HOLDOUT_OFF, 1);
  RNA_def_property_ui_text(prop, "Holdout", "Mask out objects in collection from view layer");
  RNA_def_property_update(prop, NC_SCENE | ND_LAYER, "rna_LayerCollection_update");

  prop = RNA_def_property(srna, "indirect_only", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", LAYER_COLLECTION_INDIRECT_ONLY);
  RNA_def_property_boolean_funcs(prop, NULL, "rna_LayerCollection_indirect_only_set");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_icon(prop, ICON_INDIRECT_ONLY_OFF, 1);
  RNA_def_property_ui_text(
      prop,
      "Indirect Only",
      "Objects in collection only contribute indirectly (through shadows and reflections) "
      "in the view layer");
  RNA_def_property_update(prop, NC_SCENE | ND_LAYER, "rna_LayerCollection_update");

  prop = RNA_def_property(srna, "hide_viewport", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", LAYER_COLLECTION_HIDE);
  RNA_def_property_boolean_funcs(prop, NULL, "rna_LayerCollection_hide_viewport_set");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_icon(prop, ICON_HIDE_OFF, -1);
  RNA_def_property_ui_text(prop, "Hide in Viewport", "Temporarily hide in viewport");
  RNA_def_property_update(prop, NC_SCENE | ND_LAYER_CONTENT, "rna_LayerCollection_update");

  func = RNA_def_function(srna, "visible_get", "rna_LayerCollection_visible_get");
  RNA_def_function_ui_description(func,
                                  "Whether this collection is visible, take into account the "
                                  "collection parent and the viewport");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  RNA_def_function_return(func, RNA_def_boolean(func, "result", 0, "", ""));

  /* Run-time flags. */
  prop = RNA_def_property(srna, "is_visible", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "runtime_flag", LAYER_COLLECTION_VISIBLE_VIEW_LAYER);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Visible",
                           "Whether this collection is visible for the view layer, take into "
                           "account the collection parent");

  func = RNA_def_function(srna, "has_objects", "rna_LayerCollection_has_objects");
  RNA_def_function_ui_description(func, "");
  RNA_def_function_return(func, RNA_def_boolean(func, "result", 0, "", ""));

  func = RNA_def_function(
      srna, "has_selected_objects", "rna_LayerCollection_has_selected_objects");
  RNA_def_function_ui_description(func, "");
  prop = RNA_def_pointer(
      func, "view_layer", "ViewLayer", "", "View layer the layer collection belongs to");
  RNA_def_parameter_flags(prop, 0, PARM_REQUIRED);
  RNA_def_function_return(func, RNA_def_boolean(func, "result", 0, "", ""));
}

static void rna_def_layer_objects(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  PropertyRNA *prop;

  RNA_def_property_srna(cprop, "LayerObjects");
  srna = RNA_def_struct(brna, "LayerObjects", NULL);
  RNA_def_struct_sdna(srna, "ViewLayer");
  RNA_def_struct_ui_text(srna, "Layer Objects", "Collections of objects");

  prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_LayerObjects_active_object_get",
                                 "rna_LayerObjects_active_object_set",
                                 NULL,
                                 NULL);
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_NEVER_UNLINK);
  RNA_def_property_ui_text(prop, "Active Object", "Active object for this layer");
  /* Could call: `ED_object_base_activate(C, view_layer->basact);`
   * but would be a bad level call and it seems the notifier is enough */
  RNA_def_property_update(prop, NC_SCENE | ND_OB_ACTIVE, NULL);

  prop = RNA_def_property(srna, "selected", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "object_bases", NULL);
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_collection_funcs(prop,
                                    "rna_LayerObjects_selected_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_ViewLayer_objects_get",
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL);
  RNA_def_property_ui_text(prop, "Selected Objects", "All the selected objects of this layer");
}

static void rna_def_object_base(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "ObjectBase", NULL);
  RNA_def_struct_sdna(srna, "Base");
  RNA_def_struct_ui_text(srna, "Object Base", "An object instance in a render layer");
  RNA_def_struct_ui_icon(srna, ICON_OBJECT_DATA);

  prop = RNA_def_property(srna, "object", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "object");
  RNA_def_property_ui_text(prop, "Object", "Object this base links to");

  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", BASE_SELECTED);
  RNA_def_property_ui_text(prop, "Select", "Object base selection state");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ObjectBase_select_update");

  prop = RNA_def_property(srna, "hide_viewport", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", BASE_HIDDEN);
  RNA_def_property_flag(prop, PROP_LIB_EXCEPTION);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_icon(prop, ICON_HIDE_OFF, -1);
  RNA_def_property_ui_text(prop, "Hide in Viewport", "Temporarily hide in viewport");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ObjectBase_hide_viewport_update");
}

void RNA_def_view_layer(BlenderRNA *brna)
{
  FunctionRNA *func;
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "ViewLayer", NULL);
  RNA_def_struct_ui_text(srna, "View Layer", "View layer");
  RNA_def_struct_ui_icon(srna, ICON_RENDER_RESULT);
  RNA_def_struct_path_func(srna, "rna_ViewLayer_path");
  RNA_def_struct_idprops_func(srna, "rna_ViewLayer_idprops");

  rna_def_view_layer_common(brna, srna, true);

  func = RNA_def_function(srna, "update_render_passes", "rna_ViewLayer_update_render_passes");
  RNA_def_function_ui_description(func,
                                  "Requery the enabled render passes from the render engine");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_NO_SELF);

  prop = RNA_def_property(srna, "layer_collection", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "LayerCollection");
  RNA_def_property_pointer_sdna(prop, NULL, "layer_collections.first");
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_ui_text(
      prop,
      "Layer Collection",
      "Root of collections hierarchy of this view layer,"
      "its 'collection' pointer property is the same as the scene's master collection");

  prop = RNA_def_property(srna, "active_layer_collection", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "LayerCollection");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_ViewLayer_active_layer_collection_get",
                                 "rna_ViewLayer_active_layer_collection_set",
                                 NULL,
                                 NULL);
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_NEVER_NULL);
  RNA_def_property_ui_text(
      prop, "Active Layer Collection", "Active layer collection in this view layer's hierarchy");
  RNA_def_property_update(prop, NC_SCENE | ND_LAYER, NULL);

  prop = RNA_def_property(srna, "objects", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "object_bases", NULL);
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_collection_funcs(
      prop, NULL, NULL, NULL, "rna_ViewLayer_objects_get", NULL, NULL, NULL, NULL);
  RNA_def_property_ui_text(prop, "Objects", "All the objects in this layer");
  rna_def_layer_objects(brna, prop);

  /* layer options */
  prop = RNA_def_property(srna, "use", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", VIEW_LAYER_RENDER);
  RNA_def_property_ui_text(prop, "Enabled", "Enable or disable rendering of this View Layer");
  RNA_def_property_update(prop, NC_SCENE | ND_LAYER, NULL);

  prop = RNA_def_property(srna, "use_freestyle", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", VIEW_LAYER_FREESTYLE);
  RNA_def_property_ui_text(prop, "Freestyle", "Render stylized strokes in this Layer");
  RNA_def_property_update(prop, NC_SCENE | ND_LAYER, NULL);

  /* Freestyle */
  rna_def_freestyle_settings(brna);

  prop = RNA_def_property(srna, "freestyle_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_pointer_sdna(prop, NULL, "freestyle_config");
  RNA_def_property_struct_type(prop, "FreestyleSettings");
  RNA_def_property_ui_text(prop, "Freestyle Settings", "");

  /* debug update routine */
  func = RNA_def_function(srna, "update", "rna_ViewLayer_update_tagged");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_MAIN | FUNC_USE_REPORTS);
  RNA_def_function_ui_description(
      func, "Update data tagged to be updated from previous access to data or operators");

  /* Dependency Graph */
  prop = RNA_def_property(srna, "depsgraph", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Depsgraph");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(prop, "Dependency Graph", "Dependencies in the scene data");
  RNA_def_property_pointer_funcs(prop, "rna_ViewLayer_depsgraph_get", NULL, NULL, NULL);

  /* Nested Data. */
  /* *** Non-Animated *** */
  RNA_define_animate_sdna(false);
  rna_def_layer_collection(brna);
  rna_def_object_base(brna);
  RNA_define_animate_sdna(true);
  /* *** Animated *** */
}

#endif
