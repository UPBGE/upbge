/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include <optional>
#include <utility>

#include "AS_asset_library.hh"

#include "BLI_function_ref.hh"
#include "BLI_map.hh"

#include <memory>

struct AssetLibraryReference;
struct bUserAssetLibrary;

namespace blender::asset_system {

class AllAssetLibrary;
class OnDiskAssetLibrary;
class RuntimeAssetLibrary;

/**
 * Global singleton-ish that provides access to individual #AssetLibrary instances.
 *
 * Whenever a blend file is loaded, the existing instance of AssetLibraryService is destructed, and
 * a new one is created -- hence the "singleton-ish". This ensures only information about relevant
 * asset libraries is loaded.
 *
 * \note How Asset libraries are identified may change in the future.
 *  For now they are assumed to be:
 * - on disk (identified by the absolute directory), or
 * - the "current file" library (which is in memory but could have catalogs
 *   loaded from a file on disk).
 */
class AssetLibraryService {
  static std::unique_ptr<AssetLibraryService> instance_;

  /**
   * Identify libraries with the library type, and the absolute path of the library's root path
   * (normalize with #normalize_directory_path()!). The type is relevant since the current file
   * library may point to the same path as a custom library.
   */
  using OnDiskLibraryIdentifier = std::pair<eAssetLibraryType, std::string>;
  /** Mapping of a (type, root path) pair to the AssetLibrary instance. */
  Map<OnDiskLibraryIdentifier, std::unique_ptr<OnDiskAssetLibrary>> on_disk_libraries_;
  /**
   * Library without a known path, i.e. the "Current File" library if the file isn't saved yet. If
   * the file was saved, a valid path for the library can be determined and #on_disk_libraries_
   * above should be used.
   */
  std::unique_ptr<RuntimeAssetLibrary> current_file_library_;
  /** The "all" asset library, merging all other libraries into one. */
  std::unique_ptr<AllAssetLibrary> all_library_;

  /** Handlers for managing the life cycle of the AssetLibraryService instance. */
  bCallbackFuncStore on_load_callback_store_;
  static bool atexit_handler_registered_;

 public:
  AssetLibraryService() = default;
  ~AssetLibraryService() = default;

  /** Return the AssetLibraryService singleton, allocating it if necessary. */
  static AssetLibraryService *get();

  /** Destroy the AssetLibraryService singleton. It will be reallocated by #get() if necessary. */
  static void destroy();

  static std::string root_path_from_library_ref(const AssetLibraryReference &library_reference);
  static bUserAssetLibrary *find_custom_asset_library_from_library_ref(
      const AssetLibraryReference &library_reference);
  static bUserAssetLibrary *find_custom_preferences_asset_library_from_asset_weak_ref(
      const AssetWeakReference &asset_reference);
  /**
   * Turn the runtime current file library into an on-disk current file library, preserving
   * catalog data like undo/redo history, deleted catalog info, catalog saving state, etc.
   * Note that this creates a new on-disk asset library and destroys the runtime one.
   *
   * Call when the `.blend` file is saved to disk.
   *
   * \return the new on-disk current file asset library (null in case of failure to find a path to
   * store the library in, based on the #Main.filepath from \a main).
   */
  static AssetLibrary *move_runtime_current_file_into_on_disk_library(const Main &bmain);

  AssetLibrary *get_asset_library(const Main *bmain,
                                  const AssetLibraryReference &library_reference);

  /**
   * Get an asset library of type #ASSET_LIBRARY_CUSTOM from a directory path. Use
   * #get_asset_library_on_disk_custom_preferences() for asset libraries registered in the
   * Preferences.
   */
  AssetLibrary *get_asset_library_on_disk_custom(StringRef name, StringRefNull root_path);
  /**
   * Get an asset library of type #ASSET_LIBRARY_CUSTOM from an asset library definition in the
   * Preferences.
   */
  AssetLibrary *get_asset_library_on_disk_custom_preferences(bUserAssetLibrary *custom_library);
  /** Get a builtin (not user defined) asset library. I.e. a library that is **not** of type
   * #ASSET_LIBRARY_CUSTOM. */
  AssetLibrary *get_asset_library_on_disk_builtin(eAssetLibraryType type, StringRefNull root_path);
  /** Get the "Current File" asset library. */
  AssetLibrary *get_asset_library_current_file();
  /** Get the "All" asset library, which loads all others and merges them into one. */
  AssetLibrary *get_asset_library_all(const Main *bmain);
  /**
   * Tag the "All" asset library as needing to reload catalogs. This should be called when catalog
   * data of other asset libraries changes. Note that changes to the catalog definition file on
   * disk don't ever affect this "dirty" flag. It only reflects changes from this Blender session.
   */
  void tag_all_library_catalogs_dirty();
  void reload_all_library_catalogs_if_dirty();

  /**
   * Return the start position of the last blend-file extension in given path,
   * or #std::string::npos if not found. Works with both kind of path separators.
   */
  int64_t rfind_blendfile_extension(StringRef path);
  /**
   * Return a normalized version of #AssetWeakReference.relative_asset_identifier.
   * Special care is required here because slashes or backslashes should not be converted in the ID
   * name itself.
   */
  std::string normalize_asset_weak_reference_relative_asset_identifier(
      const AssetWeakReference &asset_reference);
  /** Get a valid library path from the weak reference. Empty if e.g. the reference is to a local
   * asset. */
  std::string resolve_asset_weak_reference_to_library_path(
      const AssetWeakReference &asset_reference);
  /**
   * Attempt to build a full path to an asset based on the currently available (not necessary
   * loaded) asset libraries. The path is not guaranteed to exist. The returned path will be
   * normalized and using native slashes.
   *
   * \note Only works for asset libraries on disk (others can't be resolved).
   */
  std::string resolve_asset_weak_reference_to_full_path(const AssetWeakReference &asset_reference);
  /** Struct to hold results from path explosion functions
   * (#resolve_asset_weak_reference_to_exploded_path()). */
  struct ExplodedPath {
    /** The string buffer containing the fully resolved path, if resolving was successful. Pointer
     * so that the contained string address doesn't change when moving this object. */
    std::unique_ptr<std::string> full_path;
    /** Reference into the part of #full_path that is the library directory path. That is, it ends
     * with the library .blend file ("directory" is misleading). */
    StringRef dir_component = "";
    /** Reference into the part of #full_path that is the ID group name ("Object", "Material",
     * "Brush", ...). */
    StringRef group_component = "";
    /** Reference into the part of #full_path that is the ID name. */
    StringRef name_component = "";
  };
  /** Similar to #BKE_blendfile_library_path_explode, returns the full path as
   * #resolve_asset_weak_reference_to_library_path, with StringRefs to the `dir` (i.e. blendfile
   * path), `group` (i.e. ID type) and `name` (i.e. ID name) parts. */
  std::optional<ExplodedPath> resolve_asset_weak_reference_to_exploded_path(
      const AssetWeakReference &asset_reference);

  /** Returns whether there are any known asset libraries with unsaved catalog edits. */
  bool has_any_unsaved_catalogs() const;

  /** See AssetLibrary::foreach_loaded(). */
  void foreach_loaded_asset_library(FunctionRef<void(AssetLibrary &)> fn,
                                    bool include_all_library) const;

 protected:
  /** Allocate a new instance of the service and assign it to `instance_`. */
  static void allocate_service_instance();

  OnDiskAssetLibrary *lookup_on_disk_library(eAssetLibraryType type, StringRefNull root_path);

  AssetLibrary *find_loaded_on_disk_asset_library_from_name(StringRef name) const;

  /**
   * Get the given asset library. Opens it (i.e. creates a new AssetLibrary instance) if necessary.
   *
   * \param root_path: The top level directory.
   * \param preferences_library: The definition of the library from the Preferences. Set this to
   * null if the library is not registered in the Preferences (but non-null if it is!).
   */
  AssetLibrary *get_asset_library_on_disk(eAssetLibraryType library_type,
                                          StringRef name,
                                          StringRefNull root_path,
                                          bool load_catalogs = true,
                                          bUserAssetLibrary *preferences_library = nullptr);
  /**
   * Ensure the AssetLibraryService instance is destroyed before a new blend file is loaded.
   * This makes memory management simple, and ensures a fresh start for every blend file. */
  void app_handler_register();
  void app_handler_unregister();
};

}  // namespace blender::asset_system
