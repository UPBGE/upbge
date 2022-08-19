/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

/* Use a define instead of `#pragma once` because of `BKE_addon.h`, `ED_object.h` & others. */
#ifndef __RNA_TYPES_H__
#define __RNA_TYPES_H__

#include "../blenlib/BLI_sys_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct BlenderRNA;
struct FunctionRNA;
struct ID;
struct Main;
struct ParameterList;
struct PropertyRNA;
struct ReportList;
struct StructRNA;
struct bContext;

/**
 * Pointer
 *
 * RNA pointers are not a single C pointer but include the type,
 * and a pointer to the ID struct that owns the struct, since
 * in some cases this information is needed to correctly get/set
 * the properties and validate them. */

typedef struct PointerRNA {
  struct ID *owner_id;
  struct StructRNA *type;
  void *data;
} PointerRNA;

typedef struct PropertyPointerRNA {
  PointerRNA ptr;
  struct PropertyRNA *prop;
} PropertyPointerRNA;

/**
 * Stored result of a RNA path lookup (as used by anim-system)
 */
typedef struct PathResolvedRNA {
  struct PointerRNA ptr;
  struct PropertyRNA *prop;
  /** -1 for non-array access. */
  int prop_index;
} PathResolvedRNA;

/* Property */

typedef enum PropertyType {
  PROP_BOOLEAN = 0,
  PROP_INT = 1,
  PROP_FLOAT = 2,
  PROP_STRING = 3,
  PROP_ENUM = 4,
  PROP_POINTER = 5,
  PROP_COLLECTION = 6,
} PropertyType;

/* also update rna_property_subtype_unit when you change this */
typedef enum PropertyUnit {
  PROP_UNIT_NONE = (0 << 16),
  PROP_UNIT_LENGTH = (1 << 16),        /* m */
  PROP_UNIT_AREA = (2 << 16),          /* m^2 */
  PROP_UNIT_VOLUME = (3 << 16),        /* m^3 */
  PROP_UNIT_MASS = (4 << 16),          /* kg */
  PROP_UNIT_ROTATION = (5 << 16),      /* radians */
  PROP_UNIT_TIME = (6 << 16),          /* frame */
  PROP_UNIT_TIME_ABSOLUTE = (7 << 16), /* time in seconds (independent of scene) */
  PROP_UNIT_VELOCITY = (8 << 16),      /* m/s */
  PROP_UNIT_ACCELERATION = (9 << 16),  /* m/(s^2) */
  PROP_UNIT_CAMERA = (10 << 16),       /* mm */
  PROP_UNIT_POWER = (11 << 16),        /* W */
  PROP_UNIT_TEMPERATURE = (12 << 16),  /* C */
} PropertyUnit;

/**
 * Use values besides #PROP_SCALE_LINEAR
 * so the movement of the mouse doesn't map linearly to the value of the slider.
 *
 * For some settings it's useful to space motion in a non-linear way, see T77868.
 *
 * NOTE: The scale types are available for all float sliders.
 * For integer sliders they are only available if they use the visible value bar.
 * Sliders with logarithmic scale and value bar must have a range > 0
 * while logarithmic sliders without the value bar can have a range of >= 0.
 */
typedef enum PropertyScaleType {
  /** Linear scale (default). */
  PROP_SCALE_LINEAR = 0,
  /**
   * Logarithmic scale
   * - Maximum range: `0 <= x < inf`
   */
  PROP_SCALE_LOG = 1,
  /**
   * Cubic scale.
   * - Maximum range: `-inf < x < inf`
   */
  PROP_SCALE_CUBIC = 2,
} PropertyScaleType;

#define RNA_SUBTYPE_UNIT(subtype) ((subtype)&0x00FF0000)
#define RNA_SUBTYPE_VALUE(subtype) ((subtype) & ~0x00FF0000)
#define RNA_SUBTYPE_UNIT_VALUE(subtype) ((subtype) >> 16)

#define RNA_ENUM_BITFLAG_SIZE 32

#define RNA_TRANSLATION_PREC_DEFAULT 5

#define RNA_STACK_ARRAY 32

/**
 * \note Also update enums in bpy_props.c and rna_rna.c when adding items here.
 * Watch it: these values are written to files as part of node socket button subtypes!
 */
typedef enum PropertySubType {
  PROP_NONE = 0,

  /* strings */
  PROP_FILEPATH = 1,
  PROP_DIRPATH = 2,
  PROP_FILENAME = 3,
  /** A string which should be represented as bytes in python, NULL terminated though. */
  PROP_BYTESTRING = 4,
  /* 5 was used by "PROP_TRANSLATE" sub-type, which is now a flag. */
  /** A string which should not be displayed in UI. */
  PROP_PASSWORD = 6,

  /* numbers */
  /** A dimension in pixel units, possibly before DPI scaling (so value may not be the final pixel
   * value but the one to apply DPI scale to). */
  PROP_PIXEL = 12,
  PROP_UNSIGNED = 13,
  PROP_PERCENTAGE = 14,
  PROP_FACTOR = 15,
  PROP_ANGLE = 16 | PROP_UNIT_ROTATION,
  PROP_TIME = 17 | PROP_UNIT_TIME,
  PROP_TIME_ABSOLUTE = 17 | PROP_UNIT_TIME_ABSOLUTE,
  /** Distance in 3d space, don't use for pixel distance for eg. */
  PROP_DISTANCE = 18 | PROP_UNIT_LENGTH,
  PROP_DISTANCE_CAMERA = 19 | PROP_UNIT_CAMERA,

  /* number arrays */
  PROP_COLOR = 20,
  PROP_TRANSLATION = 21 | PROP_UNIT_LENGTH,
  PROP_DIRECTION = 22,
  PROP_VELOCITY = 23 | PROP_UNIT_VELOCITY,
  PROP_ACCELERATION = 24 | PROP_UNIT_ACCELERATION,
  PROP_MATRIX = 25,
  PROP_EULER = 26 | PROP_UNIT_ROTATION,
  PROP_QUATERNION = 27,
  PROP_AXISANGLE = 28,
  PROP_XYZ = 29,
  PROP_XYZ_LENGTH = 29 | PROP_UNIT_LENGTH,
  /** Used for colors which would be color managed before display. */
  PROP_COLOR_GAMMA = 30,
  /** Generic array, no units applied, only that x/y/z/w are used (Python vector). */
  PROP_COORDS = 31,

  /* booleans */
  PROP_LAYER = 40,
  PROP_LAYER_MEMBER = 41,

  /** Light */
  PROP_POWER = 42 | PROP_UNIT_POWER,

  /* temperature */
  PROP_TEMPERATURE = 43 | PROP_UNIT_TEMPERATURE,
} PropertySubType;

/* Make sure enums are updated with these */
/* HIGHEST FLAG IN USE: 1 << 31
 * FREE FLAGS: 2, 9, 11, 13, 14, 15. */
typedef enum PropertyFlag {
  /**
   * Editable means the property is editable in the user
   * interface, properties are editable by default except
   * for pointers and collections.
   */
  PROP_EDITABLE = (1 << 0),
  /**
   * This property is editable even if it is lib linked,
   * meaning it will get lost on reload, but it's useful
   * for editing.
   */
  PROP_LIB_EXCEPTION = (1 << 16),
  /**
   * Animatable means the property can be driven by some
   * other input, be it animation curves, expressions, ..
   * properties are animatable by default except for pointers
   * and collections.
   */
  PROP_ANIMATABLE = (1 << 1),
  /**
   * This flag means when the property's widget is in 'text-edit' mode, it will be updated
   * after every typed char, instead of waiting final validation. Used e.g. for text search-box.
   * It will also cause UI_BUT_VALUE_CLEAR to be set for text buttons. We could add an own flag
   * for search/filter properties, but this works just fine for now.
   */
  PROP_TEXTEDIT_UPDATE = (1u << 31),

  /* icon */
  PROP_ICONS_CONSECUTIVE = (1 << 12),
  PROP_ICONS_REVERSE = (1 << 8),

  /** Hidden in the user interface. */
  PROP_HIDDEN = (1 << 19),
  /** Do not write in presets. */
  PROP_SKIP_SAVE = (1 << 28),

  /* numbers */

  /** Each value is related proportionally (object scale, image size). */
  PROP_PROPORTIONAL = (1 << 26),

  /* pointers */
  PROP_ID_REFCOUNT = (1 << 6),

  /**
   * Disallow assigning a variable to itself, eg an object tracking itself
   * only apply this to types that are derived from an ID ().
   */
  PROP_ID_SELF_CHECK = (1 << 20),
  /**
   * Use for...
   * - pointers: in the UI and python so unsetting or setting to None won't work.
   * - strings: so our internal generated get/length/set
   *   functions know to do NULL checks before access T30865.
   */
  PROP_NEVER_NULL = (1 << 18),
  /**
   * Currently only used for UI, this is similar to PROP_NEVER_NULL
   * except that the value may be NULL at times, used for ObData, where an Empty's will be NULL
   * but setting NULL on a mesh object is not possible.
   * So if it's not NULL, setting NULL can't be done!
   */
  PROP_NEVER_UNLINK = (1 << 25),

  /**
   * Pointers to data that is not owned by the struct.
   * Typical example: Bone.parent, Bone.child, etc., and nearly all ID pointers.
   * This is crucial information for processes that walk the whole data of an ID e.g.
   * (like library override).
   * Note that all ID pointers are enforced to this by default,
   * this probably will need to be rechecked
   * (see ugly infamous node-trees of material/texture/scene/etc.).
   */
  PROP_PTR_NO_OWNERSHIP = (1 << 7),

  /**
   * flag contains multiple enums.
   * NOTE: not to be confused with `prop->enumbitflags`
   * this exposes the flag as multiple options in python and the UI.
   *
   * \note These can't be animated so use with care.
   */
  PROP_ENUM_FLAG = (1 << 21),

  /* need context for update function */
  PROP_CONTEXT_UPDATE = (1 << 22),
  PROP_CONTEXT_PROPERTY_UPDATE = PROP_CONTEXT_UPDATE | (1 << 27),

  /* registering */
  PROP_REGISTER = (1 << 4),
  PROP_REGISTER_OPTIONAL = PROP_REGISTER | (1 << 5),

  /**
   * Use for allocated function return values of arrays or strings
   * for any data that should not have a reference kept.
   *
   * It can be used for properties which are dynamically allocated too.
   *
   * \note Currently dynamic sized thick wrapped data isn't supported.
   * This would be a useful addition and avoid a fixed maximum sized as in done at the moment.
   */
  PROP_THICK_WRAP = (1 << 23),

  /** This is an IDProperty, not a DNA one. */
  PROP_IDPROPERTY = (1 << 10),
  /** For dynamic arrays, and retvals of type string. */
  PROP_DYNAMIC = (1 << 17),
  /** For enum that shouldn't be contextual */
  PROP_ENUM_NO_CONTEXT = (1 << 24),
  /** For enums not to be translated (e.g. viewlayers' names in nodes). */
  PROP_ENUM_NO_TRANSLATE = (1 << 29),

  /**
   * Don't do dependency graph tag from a property update callback.
   * Use this for properties which defines interface state, for example,
   * properties which denotes whether modifier panel is collapsed or not.
   */
  PROP_NO_DEG_UPDATE = (1 << 30),
} PropertyFlag;

/**
 * Flags related to comparing and overriding RNA properties.
 * Make sure enums are updated with these.
 *
 * FREE FLAGS: 2, 3, 4, 5, 6, 7, 8, 9, 12 and above.
 */
typedef enum PropertyOverrideFlag {
  /** Means that the property can be overridden by a local override of some linked datablock. */
  PROPOVERRIDE_OVERRIDABLE_LIBRARY = (1 << 0),

  /**
   * Forbid usage of this property in comparison (& hence override) code.
   * Useful e.g. for collections of data like mesh's geometry, particles, etc.
   * Also for runtime data that should never be considered as part of actual Blend data (e.g.
   * depsgraph from ViewLayers...).
   */
  PROPOVERRIDE_NO_COMPARISON = (1 << 1),

  /**
   * Means the property can be fully ignored by override process.
   * Unlike NO_COMPARISON, it can still be used by diffing code, but no override operation will be
   * created for it, and no attempt to restore the data from linked reference either.
   *
   * WARNING: This flag should be used with a lot of caution, as it completely by-passes override
   * system. It is currently only used for ID's names, since we cannot prevent local override to
   * get a different name from the linked reference, and ID names are 'rna name property' (i.e. are
   * used in overrides of collections of IDs). See also `BKE_lib_override_library_update()` where
   * we deal manually with the value of that property at DNA level. */
  PROPOVERRIDE_IGNORE = (1 << 2),

  /*** Collections-related ***/

  /** The property supports insertion (collections only). */
  PROPOVERRIDE_LIBRARY_INSERTION = (1 << 10),

  /** Only use indices to compare items in the property, never names (collections only).
   *
   * Useful when nameprop of the items is generated from other data
   * (e.g. name of material slots is actually name of assigned material).
   */
  PROPOVERRIDE_NO_PROP_NAME = (1 << 11),
} PropertyOverrideFlag;

/**
 * Function parameters flags.
 * \warning 16bits only.
 */
typedef enum ParameterFlag {
  PARM_REQUIRED = (1 << 0),
  PARM_OUTPUT = (1 << 1),
  PARM_RNAPTR = (1 << 2),
  /**
   * This allows for non-breaking API updates,
   * when adding non-critical new parameter to a callback function.
   * This way, old py code defining funcs without that parameter would still work.
   * WARNING: any parameter after the first PYFUNC_OPTIONAL one will be considered as optional!
   * \note only for input parameters!
   */
  PARM_PYFUNC_OPTIONAL = (1 << 3),
} ParameterFlag;

struct CollectionPropertyIterator;
struct Link;
typedef int (*IteratorSkipFunc)(struct CollectionPropertyIterator *iter, void *data);

typedef struct ListBaseIterator {
  struct Link *link;
  int flag;
  IteratorSkipFunc skip;
} ListBaseIterator;

typedef struct ArrayIterator {
  char *ptr;
  /** Past the last valid pointer, only for comparisons, ignores skipped values. */
  char *endptr;
  /** Will be freed if set. */
  void *free_ptr;
  int itemsize;

  /**
   * Array length with no skip functions applied,
   * take care not to compare against index from animsys or Python indices.
   */
  int length;

  /**
   * Optional skip function,
   * when set the array as viewed by rna can contain only a subset of the members.
   * this changes indices so quick array index lookups are not possible when skip function is used.
   */
  IteratorSkipFunc skip;
} ArrayIterator;

typedef struct CountIterator {
  void *ptr;
  int item;
} CountIterator;

typedef struct CollectionPropertyIterator {
  /* internal */
  PointerRNA parent;
  PointerRNA builtin_parent;
  struct PropertyRNA *prop;
  union {
    ArrayIterator array;
    ListBaseIterator listbase;
    CountIterator count;
    void *custom;
  } internal;
  int idprop;
  int level;

  /* external */
  PointerRNA ptr;
  int valid;
} CollectionPropertyIterator;

typedef struct CollectionPointerLink {
  struct CollectionPointerLink *next, *prev;
  PointerRNA ptr;
} CollectionPointerLink;

/** Copy of ListBase for RNA. */
typedef struct CollectionListBase {
  struct CollectionPointerLink *first, *last;
} CollectionListBase;

typedef enum RawPropertyType {
  PROP_RAW_UNSET = -1,
  PROP_RAW_INT, /* XXX: abused for types that are not set, eg. MFace.verts, needs fixing. */
  PROP_RAW_SHORT,
  PROP_RAW_CHAR,
  PROP_RAW_BOOLEAN,
  PROP_RAW_DOUBLE,
  PROP_RAW_FLOAT,
} RawPropertyType;

typedef struct RawArray {
  void *array;
  RawPropertyType type;
  int len;
  int stride;
} RawArray;

/**
 * This struct is are typically defined in arrays which define an *enum* for RNA,
 * which is used by the RNA API both for user-interface and the Python API.
 */
typedef struct EnumPropertyItem {
  /** The internal value of the enum, not exposed to users. */
  int value;
  /**
   * Note that identifiers must be unique within the array,
   * by convention they're upper case with underscores for separators.
   * - An empty string is used to define menu separators.
   * - NULL denotes the end of the array of items.
   */
  const char *identifier;
  /** Optional icon, typically 'ICON_NONE' */
  int icon;
  /** Name displayed in the interface. */
  const char *name;
  /** Longer description used in the interface. */
  const char *description;
} EnumPropertyItem;

/**
 * Heading for RNA enum items (shown in the UI).
 *
 * The description is currently only shown in the Python documentation.
 * By convention the value should be a non-empty string or NULL when there is no description
 * (never an empty string).
 */
#define RNA_ENUM_ITEM_HEADING(name, description) \
  { \
    0, "", 0, name, description \
  }

/** Separator for RNA enum items (shown in the UI). */
#define RNA_ENUM_ITEM_SEPR \
  { \
    0, "", 0, NULL, NULL \
  }

/** Separator for RNA enum that begins a new column in menus (shown in the UI). */
#define RNA_ENUM_ITEM_SEPR_COLUMN RNA_ENUM_ITEM_HEADING("", NULL)

/* extended versions with PropertyRNA argument */
typedef bool (*BooleanPropertyGetFunc)(struct PointerRNA *ptr, struct PropertyRNA *prop);
typedef void (*BooleanPropertySetFunc)(struct PointerRNA *ptr,
                                       struct PropertyRNA *prop,
                                       bool value);
typedef void (*BooleanArrayPropertyGetFunc)(struct PointerRNA *ptr,
                                            struct PropertyRNA *prop,
                                            bool *values);
typedef void (*BooleanArrayPropertySetFunc)(struct PointerRNA *ptr,
                                            struct PropertyRNA *prop,
                                            const bool *values);
typedef int (*IntPropertyGetFunc)(struct PointerRNA *ptr, struct PropertyRNA *prop);
typedef void (*IntPropertySetFunc)(struct PointerRNA *ptr, struct PropertyRNA *prop, int value);
typedef void (*IntArrayPropertyGetFunc)(struct PointerRNA *ptr,
                                        struct PropertyRNA *prop,
                                        int *values);
typedef void (*IntArrayPropertySetFunc)(struct PointerRNA *ptr,
                                        struct PropertyRNA *prop,
                                        const int *values);
typedef void (*IntPropertyRangeFunc)(struct PointerRNA *ptr,
                                     struct PropertyRNA *prop,
                                     int *min,
                                     int *max,
                                     int *softmin,
                                     int *softmax);
typedef float (*FloatPropertyGetFunc)(struct PointerRNA *ptr, struct PropertyRNA *prop);
typedef void (*FloatPropertySetFunc)(struct PointerRNA *ptr,
                                     struct PropertyRNA *prop,
                                     float value);
typedef void (*FloatArrayPropertyGetFunc)(struct PointerRNA *ptr,
                                          struct PropertyRNA *prop,
                                          float *values);
typedef void (*FloatArrayPropertySetFunc)(struct PointerRNA *ptr,
                                          struct PropertyRNA *prop,
                                          const float *values);
typedef void (*FloatPropertyRangeFunc)(struct PointerRNA *ptr,
                                       struct PropertyRNA *prop,
                                       float *min,
                                       float *max,
                                       float *softmin,
                                       float *softmax);
typedef void (*StringPropertyGetFunc)(struct PointerRNA *ptr,
                                      struct PropertyRNA *prop,
                                      char *value);
typedef int (*StringPropertyLengthFunc)(struct PointerRNA *ptr, struct PropertyRNA *prop);
typedef void (*StringPropertySetFunc)(struct PointerRNA *ptr,
                                      struct PropertyRNA *prop,
                                      const char *value);

typedef struct StringPropertySearchVisitParams {
  /** Text being searched for (never NULL). */
  const char *text;
  /** Additional information to display (optional, may be NULL). */
  const char *info;
} StringPropertySearchVisitParams;

typedef enum eStringPropertySearchFlag {
  /**
   * Used so the result of #RNA_property_string_search_flag can be used to check
   * if search is supported.
   */
  PROP_STRING_SEARCH_SUPPORTED = (1 << 0),
  /** Items resulting from  the search must be sorted. */
  PROP_STRING_SEARCH_SORT = (1 << 1),
  /**
   * Allow members besides the ones listed to be entered.
   *
   * \warning disabling this options causes the search callback to run on redraw and should
   * only be enabled this doesn't cause performance issues.
   */
  PROP_STRING_SEARCH_SUGGESTION = (1 << 2),
} eStringPropertySearchFlag;

/**
 * Visit string search candidates, `text` may be freed once this callback has finished,
 * so references to it should not be held.
 */
typedef void (*StringPropertySearchVisitFunc)(void *visit_user_data,
                                              const StringPropertySearchVisitParams *params);
/**
 * \param C: context, may be NULL (in this case all available items should be shown).
 * \param ptr: RNA pointer.
 * \param prop: RNA property. This must have it's #StringPropertyRNA.search callback set,
 * to check this use `RNA_property_string_search_flag(prop) & PROP_STRING_SEARCH_SUPPORTED`.
 * \param edit_text: Optionally use the string being edited by the user as a basis
 * for the search results (auto-complete Python attributes for e.g.).
 * \param visit_fn: This function is called with every search candidate and is typically
 * responsible for storing the search results.
 * \param visit_user_data: Caller defined data, passed to `visit_fn`.
 */
typedef void (*StringPropertySearchFunc)(const struct bContext *C,
                                         struct PointerRNA *ptr,
                                         struct PropertyRNA *prop,
                                         const char *edit_text,
                                         StringPropertySearchVisitFunc visit_fn,
                                         void *visit_user_data);

typedef int (*EnumPropertyGetFunc)(struct PointerRNA *ptr, struct PropertyRNA *prop);
typedef void (*EnumPropertySetFunc)(struct PointerRNA *ptr, struct PropertyRNA *prop, int value);
/* same as PropEnumItemFunc */
typedef const EnumPropertyItem *(*EnumPropertyItemFunc)(struct bContext *C,
                                                        PointerRNA *ptr,
                                                        struct PropertyRNA *prop,
                                                        bool *r_free);

typedef struct PropertyRNA PropertyRNA;

/* Parameter List */

typedef struct ParameterList {
  /** Storage for parameters*. */
  void *data;

  /** Function passed at creation time. */
  struct FunctionRNA *func;

  /** Store the parameter size. */
  int alloc_size;

  int arg_count, ret_count;
} ParameterList;

typedef struct ParameterIterator {
  struct ParameterList *parms;
  // PointerRNA funcptr; /* UNUSED */
  void *data;
  int size, offset;

  PropertyRNA *parm;
  int valid;
} ParameterIterator;

/** Mainly to avoid confusing casts. */
typedef struct ParameterDynAlloc {
  /** Important, this breaks when set to an int. */
  intptr_t array_tot;
  void *array;
} ParameterDynAlloc;

/* Function */

/**
 * Options affecting callback signature.
 *
 * Those add additional parameters at the beginning of the C callback, like that:
 * <pre>
 * rna_my_func([ID *_selfid],
 *             [<DNA_STRUCT> *self|StructRNA *type],
 *             [Main *bmain],
 *             [bContext *C],
 *             [ReportList *reports],
 *             <other RNA-defined parameters>);
 * </pre>
 */
typedef enum FunctionFlag {
  /**
   * Pass ID owning 'self' data
   * (i.e. ptr->owner_id, might be same as self in case data is an ID...).
   */
  FUNC_USE_SELF_ID = (1 << 11),

  /**
   * Do not pass the object (DNA struct pointer) from which it is called,
   * used to define static or class functions.
   */
  FUNC_NO_SELF = (1 << 0),
  /** Pass RNA type, used to define class functions, only valid when #FUNC_NO_SELF is set. */
  FUNC_USE_SELF_TYPE = (1 << 1),

  /* Pass Main, bContext and/or ReportList. */
  FUNC_USE_MAIN = (1 << 2),
  FUNC_USE_CONTEXT = (1 << 3),
  FUNC_USE_REPORTS = (1 << 4),

  /***** Registering of Python subclasses. *****/
  /**
   * This function is part of the registerable class' interface,
   * and can be implemented/redefined in Python.
   */
  FUNC_REGISTER = (1 << 5),
  /** Subclasses can choose not to implement this function. */
  FUNC_REGISTER_OPTIONAL = FUNC_REGISTER | (1 << 6),
  /**
   * If not set, the Python function implementing this call
   * is not allowed to write into data-blocks.
   * Except for WindowManager and Screen currently, see rna_id_write_error() in bpy_rna.c
   */
  FUNC_ALLOW_WRITE = (1 << 12),

  /***** Internal flags. *****/
  /** UNUSED CURRENTLY? ??? */
  FUNC_BUILTIN = (1 << 7),
  /** UNUSED CURRENTLY. ??? */
  FUNC_EXPORT = (1 << 8),
  /** Function has been defined at runtime, not statically in RNA source code. */
  FUNC_RUNTIME = (1 << 9),
  /**
   * UNUSED CURRENTLY? Function owns its identifier and description strings,
   * and has to free them when deleted.
   */
  FUNC_FREE_POINTERS = (1 << 10),
} FunctionFlag;

typedef void (*CallFunc)(struct bContext *C,
                         struct ReportList *reports,
                         PointerRNA *ptr,
                         ParameterList *parms);

typedef struct FunctionRNA FunctionRNA;

/* Struct */

typedef enum StructFlag {
  /** Indicates that this struct is an ID struct, and to use reference-counting. */
  STRUCT_ID = (1 << 0),
  STRUCT_ID_REFCOUNT = (1 << 1),
  /** defaults on, indicates when changes in members of a StructRNA should trigger undo steps. */
  STRUCT_UNDO = (1 << 2),

  /* internal flags */
  STRUCT_RUNTIME = (1 << 3),
  /* STRUCT_GENERATED = (1 << 4), */ /* UNUSED */
  STRUCT_FREE_POINTERS = (1 << 5),
  /** Menus and Panels don't need properties */
  STRUCT_NO_IDPROPERTIES = (1 << 6),
  /** e.g. for Operator */
  STRUCT_NO_DATABLOCK_IDPROPERTIES = (1 << 7),
  /** for PropertyGroup which contains pointers to datablocks */
  STRUCT_CONTAINS_DATABLOCK_IDPROPERTIES = (1 << 8),
  /** Added to type-map #BlenderRNA.structs_map */
  STRUCT_PUBLIC_NAMESPACE = (1 << 9),
  /** All subtypes are added too. */
  STRUCT_PUBLIC_NAMESPACE_INHERIT = (1 << 10),
  /**
   * When the #PointerRNA.owner_id is NULL, this signifies the property should be accessed
   * without any context (the key-map UI and import/export for example).
   * So accessing the property should not read from the current context to derive values/limits.
   */
  STRUCT_NO_CONTEXT_WITHOUT_OWNER_ID = (1 << 11),
} StructFlag;

typedef int (*StructValidateFunc)(struct PointerRNA *ptr, void *data, int *have_function);
typedef int (*StructCallbackFunc)(struct bContext *C,
                                  struct PointerRNA *ptr,
                                  struct FunctionRNA *func,
                                  ParameterList *list);
typedef void (*StructFreeFunc)(void *data);
typedef struct StructRNA *(*StructRegisterFunc)(struct Main *bmain,
                                                struct ReportList *reports,
                                                void *data,
                                                const char *identifier,
                                                StructValidateFunc validate,
                                                StructCallbackFunc call,
                                                StructFreeFunc free);

typedef void (*StructUnregisterFunc)(struct Main *bmain, struct StructRNA *type);
typedef void **(*StructInstanceFunc)(PointerRNA *ptr);

typedef struct StructRNA StructRNA;

/**
 * Blender RNA
 *
 * Root RNA data structure that lists all struct types.
 */
typedef struct BlenderRNA BlenderRNA;

/**
 * Extending
 *
 * This struct must be embedded in *Type structs in
 * order to make them definable through RNA.
 */
typedef struct ExtensionRNA {
  void *data;
  StructRNA *srna;
  StructCallbackFunc call;
  StructFreeFunc free;
} ExtensionRNA;

#ifdef __cplusplus
}
#endif

#endif /* __RNA_TYPES_H__ */
