# SPDX-FileCopyrightText: 2012-2023 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "Manage UI translations",
    # This is now displayed as the maintainer, so show the foundation.
    # "author": "Bastien Montagne", # Original Author
    "author": "Blender Foundation",
    "version": (2, 1, 0),
    "blender": (4, 2, 0),
    "location": "Render properties, I18n Update Translation panel",
    "description": "Allows managing UI translations directly from Blender "
    "(update main .po files, update scripts' translations, etc.)",
    "doc_url": "https://developer.blender.org/docs/handbook/translating/translator_guide/",
    "support": 'OFFICIAL',
    "category": "System",
}


from . import (
    settings,
    update_repo,
    update_addon,
    update_ui,
)
if "bpy" in locals():
    import importlib
    importlib.reload(settings)
    importlib.reload(update_repo)
    importlib.reload(update_addon)
    importlib.reload(update_ui)

import bpy


classes = settings.classes + update_repo.classes + update_addon.classes + update_ui.classes


def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.WindowManager.i18n_update_settings = \
        bpy.props.PointerProperty(type=update_ui.I18nUpdateTranslationSettings)

    # Init addon's preferences (unfortunately, as we are using an external storage for the properties,
    # the load/save user preferences process has no effect on them :( ).
    if __name__ in bpy.context.preferences.addons:
        pref = bpy.context.preferences.addons[__name__].preferences
        import os
        if os.path.isfile(pref.persistent_data_path):
            pref._settings.load(pref.persistent_data_path, reset=True)


def unregister():
    for cls in classes:
        bpy.utils.unregister_class(cls)

    del bpy.types.WindowManager.i18n_update_settings
