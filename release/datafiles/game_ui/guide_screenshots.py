# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Screenshots of the Complete UI Example for the UI guide (make_game_ui_guide.py).

Run with Blender's interface (the game is played embedded, in the camera view):

  blender --factory-startup --enable-event-simulate --window-geometry 0 0 1600 900 \\
      game_ui_example.blend --python guide_screenshots.py -- SHOTS_DIR

It plays the example once per picture: the world (with the HUD), then each menu, opened at the
start by a test tree that is not saved. Writes SHOTS_DIR/<name>.png and quits.
"""

import os
import sys

import bpy

OUT = sys.argv[sys.argv.index("--") + 1]
SHOTS = (
    ("world", None),
    ("inventory", "InventoryMenu"),
    ("chest", "ChestMenu"),
    ("settings", "SettingsMenu"),
    ("pause", "Pause Menu"),
)
# Frames to wait before the picture: the menus open, their grids fill and fade in.
WAIT = 90


def view3d():
    window = bpy.context.window_manager.windows[0]
    area = max((area for area in window.screen.areas if area.type == 'VIEW_3D'),
               key=lambda area: area.width * area.height)
    region = next(region for region in area.regions if region.type == 'WINDOW')
    return window, area, region


def tester():
    """An empty with a tree that opens one menu at the start, and a Python controller that takes
    the picture and ends the game."""
    ob = bpy.data.objects.new("GuideShots", None)
    bpy.context.scene.collection.objects.link(ob)
    tree = bpy.data.node_groups.new("GuideShotsOpen", "LogicNodeTree")
    start = tree.nodes.new("LogicNativeOnInit")
    opener = tree.nodes.new("LogicNativeOpenObjectMenu")
    opener.inputs["Camera"].default_value = bpy.data.objects["InvCamera"]
    tree.links.new(start.outputs["Out"], opener.inputs["Flow"])
    binding = ob.game.logic_node_binding_new()
    binding.tree = tree
    text = bpy.data.texts.new("guide_shot.py")
    window, area, region = view3d()
    with bpy.context.temp_override(window=window, area=area, region=region, object=ob,
                                   active_object=ob, selected_objects=[ob]):
        bpy.ops.logic.sensor_add(type="ALWAYS", name="Always", object=ob.name)
        ob.game.sensors[-1].use_pulse_true_level = True
        bpy.ops.logic.controller_add(type="PYTHON", name="Shot", object=ob.name)
        ob.game.controllers[-1].mode = "SCRIPT"
        ob.game.controllers[-1].text = text
        ob.game.controllers[-1].link(sensor=ob.game.sensors[-1])
    return binding, opener, text


def run():
    import traceback
    try:
        os.makedirs(OUT, exist_ok=True)
        window, area, region = view3d()
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
        tester()
        for name, collection in SHOTS:
            # The game puts the data back as it was when it ends: find it again each time.
            binding = bpy.data.objects["GuideShots"].game.logic_node_bindings[0]
            opener = bpy.data.node_groups["GuideShotsOpen"].nodes["Open Object Menu"]
            text = bpy.data.texts["guide_shot.py"]
            binding.enabled = collection is not None
            opener.inputs["Collection"].default_value = bpy.data.collections.get(collection or "")
            path = os.path.join(OUT, name + ".png")
            text.clear()
            text.write(
                "import bge\n"
                "gd = bge.logic.globalDict\n"
                "gd['n'] = gd.get('n', 0) + 1\n"
                f"if gd['n'] == {WAIT}:\n"
                f"    bge.render.makeScreenshot({path!r})\n"
                f"if gd['n'] == {WAIT + 10}:\n"
                "    bge.logic.endGame()\n"
                "    gd['n'] = 0\n")
            # The view as the scene camera sees it, filling the window (a game changes the
            # screen when it ends: find the view again each time).
            window, area, region = view3d()
            area.spaces[0].region_3d.view_perspective = 'CAMERA'
            area.spaces[0].overlay.show_overlays = False
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.view3d.view_center_camera()
                bpy.ops.view3d.game_start()
            print("SHOT", name, os.path.exists(path), flush=True)
    except Exception:
        traceback.print_exc()
    sys.stdout.flush()
    os._exit(0)


bpy.app.timers.register(run, first_interval=1.5)
