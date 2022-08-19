# SPDX-License-Identifier: GPL-2.0-or-later

# Initialization script for 2D Animation template

import bpy
from bpy.app.handlers import persistent


def update_factory_startup_screens():
    # 2D Animation.
    screen = bpy.data.screens["2D Animation"]
    for area in screen.areas:
        if area.type == 'PROPERTIES':
            # Set Tool settings as default in properties panel.
            space = area.spaces.active
            space.context = 'TOOL'
        elif area.type == 'DOPESHEET_EDITOR':
            # Open sidebar in Dopesheet.
            space = area.spaces.active
            space.show_region_ui = True

    # 2D Full Canvas.
    screen = bpy.data.screens["2D Full Canvas"]
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            space = area.spaces.active
            space.shading.type = 'MATERIAL'
            space.shading.use_scene_world = True


def update_factory_startup_scenes():
    for scene in bpy.data.scenes:
        scene.tool_settings.use_keyframe_insert_auto = True
        scene.tool_settings.gpencil_sculpt.use_scale_thickness = True


def update_factory_startup_grease_pencils():
    for gpd in bpy.data.grease_pencils:
        gpd.onion_keyframe_type = 'ALL'


@persistent
def load_handler(_):
    update_factory_startup_screens()
    update_factory_startup_scenes()
    update_factory_startup_grease_pencils()


def register():
    bpy.app.handlers.load_factory_startup_post.append(load_handler)


def unregister():
    bpy.app.handlers.load_factory_startup_post.remove(load_handler)
