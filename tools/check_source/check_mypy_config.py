# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

__all__ = (
    "PATHS",
    "PATHS_EXCLUDE",
    "SOURCE_DIR",
)

import os
from typing import (
    Any,
)

# Notes:
# - Most tests in `tests/python` use `bpy` enough that it's simpler to list the scripts that *are* type checked.
# - References individual files which are also included in a directory are supported
#   without checking those files twice. This is needed to allow those files to use their own settings.
PATHS: tuple[tuple[str, tuple[Any, ...], dict[str, str]], ...] = (
    ("build_files/cmake/", (), {'MYPYPATH': "modules"}),
    ("build_files/utils/", (), {'MYPYPATH': "modules"}),
    ("doc/manpage/blender.1.py", (), {}),
    ("release/datafiles/", (), {}),
    ("release/release_notes/", (), {}),
    ("scripts/modules/_bpy_internal/extensions/junction_module.py", (), {}),
    ("scripts/modules/_bpy_internal/extensions/wheel_manager.py", (), {}),
    ("scripts/modules/_bpy_internal/freedesktop.py", (), {}),
    ("source/blender/nodes/intern/discover_nodes.py", (), {}),
    ("tests/python/bl_keymap_validate.py", (), {}),
    ("tests/python/bl_pyapi_bpy_app_tempdir.py", (), {}),
    ("tests/utils/blender_headless.py", (), {}),
    ("tools/check_blender_release/", (), {}),
    ("tools/check_docs/", (), {}),
    ("tools/check_source/", (), {'MYPYPATH': "modules"}),
    ("tools/check_source/check_unused_defines.py", (), {'MYPYPATH': "../utils_maintenance/modules"}),
    ("tools/check_source/static_check_size_comments.py", (), {'MYPYPATH': "../utils_maintenance/modules"}),
    ("tools/config/", (), {}),
    ("tools/triage/", (), {}),
    ("tools/utils/", (), {}),
    ("tools/utils_api/", (), {}),
    ("tools/utils_build/", (), {}),
    ("tools/utils_doc/", (), {}),
    ("tools/utils_ide/", (), {}),
    ("tools/utils_maintenance/", (), {'MYPYPATH': "modules"}),
)

SOURCE_DIR = os.path.normpath(os.path.abspath(os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", ".."))))

PATHS_EXCLUDE = set(
    os.path.join(SOURCE_DIR, p.replace("/", os.sep))
    for p in
    (
        "release/datafiles/blender_icons_geom.py",  # Uses `bpy` too much.
        "tests/utils/bl_run_operators.py",  # Uses `bpy` too much.
        "tests/utils/bl_run_operators_event_simulate.py",  # Uses `bpy` too much.
        "tools/check_blender_release/check_module_enabled.py",
        "tools/check_blender_release/check_module_numpy.py",
        "tools/check_blender_release/check_module_requests.py",
        "tools/check_blender_release/check_release.py",
        "tools/check_blender_release/check_static_binaries.py",
        "tools/check_blender_release/check_utils.py",
        "tools/check_blender_release/scripts/modules_enabled.py",
        "tools/check_blender_release/scripts/requests_basic_access.py",
        "tools/check_blender_release/scripts/requests_import.py",
        "tools/check_source/check_descriptions.py",
        "tools/check_source/clang_array_check.py",
        "tools/utils/blend2json.py",
        "tools/utils/blender_keyconfig_export_permutations.py",
        "tools/utils/blender_merge_format_changes.py",
        "tools/utils/blender_theme_as_c.py",
        "tools/utils/cycles_timeit.py",
        "tools/utils/gdb_struct_repr_c99.py",
        "tools/utils/git_log_review_commits.py",
        "tools/utils/git_log_review_commits_advanced.py",
        "tools/utils/make_cursor_gui.py",
        "tools/utils/make_gl_stipple_from_xpm.py",
        "tools/utils/make_shape_2d_from_blend.py",
        "tools/utils_api/bpy_introspect_ui.py",  # Uses `bpy`.
        "tools/utils_doc/code_layout_diagram.py",  # Uses `bpy`.
        "tools/utils_doc/rna_manual_reference_updater.py",
        "tools/utils_ide/qtcreator/externaltools/qtc_assembler_preview.py",
        "tools/utils_ide/qtcreator/externaltools/qtc_blender_diffusion.py",
        "tools/utils_ide/qtcreator/externaltools/qtc_cpp_to_c_comments.py",
        "tools/utils_ide/qtcreator/externaltools/qtc_doxy_file.py",
        "tools/utils_ide/qtcreator/externaltools/qtc_project_update.py",
        "tools/utils_ide/qtcreator/externaltools/qtc_sort_paths.py",
        "tools/utils_maintenance/blender_menu_search_coverage.py",  # Uses `bpy`.
        "tools/utils_maintenance/blender_update_themes.py",  # Uses `bpy`.
    )
)

PATHS = tuple(
    (os.path.join(SOURCE_DIR, p_items[0].replace("/", os.sep)), *p_items[1:])
    for p_items in PATHS
)

# Validate:
for p_items in PATHS:
    if not os.path.exists(os.path.join(SOURCE_DIR, p_items[0])):
        print("PATH:", p_items[0], "doesn't exist")

for p in PATHS_EXCLUDE:
    if not os.path.exists(os.path.join(SOURCE_DIR, p)):
        print("PATHS_EXCLUDE:", p, "doesn't exist")
