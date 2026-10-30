# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Render-sync test rig: picture checks and benchmarks of how game objects reach Blender's drawing
(moving, spawning, hiding, Dupli copies, Full Copy, shape keys and modifiers).

Each case builds its scene from nothing, plays it embedded in its own Blender (a game window takes
over the screen and the mouse while it runs) and writes a result file. A check prints PASS or FAIL
with its numbers; a benchmark prints the engine profile in ms per frame, averaged over seconds 4-8
of play (FPS Limit 60, vsync off).

Run from the source folder with the system Python:
    python3 tests/python/gameengine_render_sync_rig.py --list
    python3 tests/python/gameengine_render_sync_rig.py checks
    python3 tests/python/gameengine_render_sync_rig.py perf_movers --set COUNT=10000 --set MOVERS=1
Options: --blender PATH (default ../build_linux/bin/blender), --out DIR (default
<temp>/upbge_render_sync), --opengl (default Vulkan), --set NAME=VALUE (case settings, repeatable),
--repeat N (run each case N times: some faults show only now and then).
"""

import json
import os
import sys

RIG_FILE = os.path.abspath(__file__)
CASES = {}


class Case:
    def __init__(self, name, kind, doc, settings=None, timeout=120):
        self.name = name
        self.kind = kind  # "check" or "perf"
        self.doc = doc
        self.settings = settings or {}
        self.timeout = timeout
        self.build = None  # (settings) -> the camera; runs in Blender before the game
        self.before_game = None  # (settings) -> dict; runs in Blender right before the game
        self.logic = ""  # game-side code defining step(tick)
        self.after = None  # (settings, out_dir) -> dict; runs in Blender after the game
        self.judge = None  # (settings, values, log_text) -> (passed, summary); runs in the runner
        # Settings played one after the other, each in its own Blender and folder named by its
        # values (out/<case>/<values>); the last one is judged and may read the others' files.
        self.variants = None
        # Start Blender with a blank file: no start-up splash over the editor (cases whose
        # screenshot is the editor's own picture, Use Viewport Render).
        self.blank_file = False


def case(name, kind, doc, settings=None, timeout=120):
    def register(cls):
        entry = Case(name, kind, doc, settings, timeout)
        entry.build = getattr(cls, "build", None)
        entry.before_game = getattr(cls, "before_game", None)
        entry.logic = getattr(cls, "logic", "")
        entry.after = getattr(cls, "after", None)
        entry.judge = getattr(cls, "judge", None)
        entry.variants = getattr(cls, "variants", None)
        entry.blank_file = getattr(cls, "blank_file", False)
        CASES[name] = entry
        return cls
    return register


# --------------------------------------------------------------------------------------------
# Game side: the Python controller module each case's logic is appended to.

GAME_PRELUDE = r'''
import json
import os
import time
import bge
import bge.render

OUT = __OUT__
S = __SETTINGS__
store = bge.logic.globalDict


def scene():
    return bge.logic.getCurrentScene()


def shot(name):
    bge.render.makeScreenshot(os.path.join(OUT, name + ".png"))


def save(**values):
    store.setdefault("values", {}).update(values)


def finish():
    with open(os.path.join(OUT, "game.json"), "w") as handle:
        json.dump(store.get("values", {}), handle)
    bge.logic.endGame()


def upright(ob):
    ob.worldOrientation = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def profile_frame(start=4.0, end=8.0, quit_at=9.0):
    """Adds this frame's engine profile to the average of seconds start-end; ends the game."""
    now = time.monotonic()
    begin = store.setdefault("profile_begin", now)
    elapsed = now - begin
    if start <= elapsed < end:
        sums = store.setdefault("profile_sums", {})
        for key, value in bge.logic.getProfileInfo().items():
            sums[key.rstrip(":")] = sums.get(key.rstrip(":"), 0.0) + value[0]
        store["profile_frames"] = store.get("profile_frames", 0) + 1
    if elapsed >= quit_at:
        frames = max(1, store.get("profile_frames", 0))
        save(profile={k: v / frames for k, v in store.get("profile_sums", {}).items()},
             profile_frames=frames, objects=len(scene().objects))
        finish()


def data_counts():
    import bpy
    return {name: len(getattr(bpy.data, name)) for name in __DATA_KINDS__}


def data_names():
    import bpy
    return {name: sorted("%s (users %d)" % (item.name, item.users)
                         for item in getattr(bpy.data, name)) for name in __DATA_KINDS__}


def run(cont):
    tick = store["tick"] = store.get("tick", 0) + 1
    step(tick)
'''


# --------------------------------------------------------------------------------------------
# Blender side helpers (bpy is imported only inside Blender).

def bpy_module():
    import bpy
    return bpy


DATA_KINDS = ("objects", "meshes", "shape_keys", "actions", "materials")


def blender_data_counts():
    bpy = bpy_module()
    return {name: len(getattr(bpy.data, name)) for name in DATA_KINDS}


def clear_scene():
    bpy = bpy_module()
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    for coll in list(bpy.data.collections):
        bpy.data.collections.remove(coll)
    bpy.data.orphans_purge(do_recursive=True)


def box_mesh(name, size=(1.0, 1.0, 1.0), base_at_zero=False):
    import bmesh
    bpy = bpy_module()
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for vert in bm.verts:
        vert.co.x *= size[0]
        vert.co.y *= size[1]
        vert.co.z = vert.co.z * size[2] + (size[2] * 0.5 if base_at_zero else 0.0)
    bm.to_mesh(mesh)
    bm.free()
    return mesh


def emission_material(name, color=(1.0, 1.0, 1.0)):
    bpy = bpy_module()
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (*color, 1.0)
    emission.inputs["Strength"].default_value = 1.0
    output = nodes.new("ShaderNodeOutputMaterial")
    material.node_tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
    material.diffuse_color = (*color, 1.0)
    return material


def diffuse_material(name, color):
    bpy = bpy_module()
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Roughness"].default_value = 1.0
    material.diffuse_color = (*color, 1.0)
    return material


def new_object(name, data, location=(0.0, 0.0, 0.0), material=None, collection=None):
    bpy = bpy_module()
    ob = bpy.data.objects.new(name, data)
    (collection or bpy.context.scene.collection).objects.link(ob)
    ob.location = location
    if material is not None:
        ob.data.materials.append(material)
    if ob.type == 'MESH':
        ob.game.physics_type = 'NO_COLLISION'
    return ob


def add_camera(name, location, rotation_deg, ortho_scale=None):
    import math
    bpy = bpy_module()
    camera = new_object(name, bpy.data.cameras.new(name), location)
    camera.rotation_euler = tuple(math.radians(a) for a in rotation_deg)
    if ortho_scale is not None:
        camera.data.type = 'ORTHO'
        camera.data.ortho_scale = ortho_scale
    camera.data.clip_end = 1000.0
    return camera


def add_sun(rotation_deg=(50.0, 0.0, 30.0), energy=3.0):
    import math
    bpy = bpy_module()
    sun = new_object("Sun", bpy.data.lights.new("Sun", 'SUN'))
    sun.data.energy = energy
    sun.rotation_euler = tuple(math.radians(a) for a in rotation_deg)
    return sun


def game_settings():
    scene = bpy_module().context.scene
    gs = scene.game_settings
    gs.physics_engine = 'JOLT'
    gs.physics_timestep_method = 'FIXED'
    gs.use_fixed_physics_timestep = True
    gs.physics_tick_rate = 60
    gs.use_fixed_fps_cap = True
    gs.fixed_render_cap_rate = 60
    gs.vsync = 'OFF'


def add_game_property(ob, name, prop_type, value):
    bpy = bpy_module()
    with bpy.context.temp_override(object=ob, active_object=ob, selected_objects=[ob],
                                   selected_editable_objects=[ob]):
        bpy.ops.object.game_property_new(type=prop_type, name=name)
    prop = ob.game.properties[-1]
    prop.value = value


def add_shape_key(ob, name, offset_z, above_z):
    """A relative shape key moving the vertices above `above_z` by `offset_z`."""
    ob.shape_key_add(name="Basis")
    key = ob.shape_key_add(name=name)
    for point, vert in zip(key.data, ob.data.vertices):
        if vert.co.z > above_z:
            point.co.z = vert.co.z + offset_z
    # A new shape key starts at value 1.
    key.value = 0.0
    return key


def key_action(ob, shape_key_name, action_name, values=((1, 0.0), (2, 1.0))):
    """Keyframes a shape key value (an action whose slot is for the shape keys)."""
    block = ob.data.shape_keys.key_blocks[shape_key_name]
    for frame, value in values:
        block.value = value
        block.keyframe_insert("value", frame=frame)
    block.value = 0.0
    action = ob.data.shape_keys.animation_data.action
    action.name = action_name
    return action


def window_area_region():
    bpy = bpy_module()
    window = bpy.context.window_manager.windows[0]
    area = max((a for a in window.screen.areas if a.type == 'VIEW_3D'),
               key=lambda a: a.width * a.height)
    region = next(r for r in area.regions if r.type == 'WINDOW')
    return window, area, region


def attach_logic(driver, module_name):
    bpy = bpy_module()
    window, area, region = window_area_region()
    with bpy.context.temp_override(window=window, area=area, region=region, object=driver,
                                   active_object=driver, selected_objects=[driver]):
        bpy.ops.logic.sensor_add(type='ALWAYS', name="RigAlways", object=driver.name)
        bpy.ops.logic.controller_add(type='PYTHON', name="RigController", object=driver.name)
    sensor = driver.game.sensors["RigAlways"]
    sensor.use_pulse_true_level = True
    controller = driver.game.controllers["RigController"]
    controller.mode = 'MODULE'
    controller.module = module_name + ".run"
    controller.link(sensor=sensor)


def play_game(camera):
    bpy = bpy_module()
    bpy.context.scene.camera = camera
    window, area, region = window_area_region()
    area.spaces[0].region_3d.view_perspective = 'CAMERA'
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.view3d.game_start()


def load_picture(path):
    """The screenshot as rows of (r, g, b) lists, top row first."""
    import numpy
    bpy = bpy_module()
    image = bpy.data.images.load(path, check_existing=False)
    width, height = image.size
    pixels = numpy.empty(width * height * 4, dtype=numpy.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(height, width, 4)[::-1, :, :3]


def luminance(picture):
    return picture[:, :, 0] * 0.2126 + picture[:, :, 1] * 0.7152 + picture[:, :, 2] * 0.0722


def bright_runs(row_values, threshold):
    """[(start, end)] of the runs of a pixel row brighter than threshold (end excluded)."""
    runs = []
    start = None
    for x, value in enumerate(row_values):
        if value > threshold and start is None:
            start = x
        elif value <= threshold and start is not None:
            runs.append((start, x))
            start = None
    if start is not None:
        runs.append((start, len(row_values)))
    return runs


def column_tops(picture, probe_row_fraction, count, threshold=0.4):
    """Finds `count` bright objects crossing the row at probe_row_fraction of the height and
    gives each one's centre x and top row (the first bright row of its centre column)."""
    lum = luminance(picture)
    height = lum.shape[0]
    probe = int(height * probe_row_fraction)
    runs = [r for r in bright_runs(lum[probe], threshold) if r[1] - r[0] >= 4]
    if len(runs) != count:
        raise RuntimeError("expected %d objects on row %d, found %d: %r" % (
            count, probe, len(runs), runs))
    tops = []
    for start, end in runs:
        center = (start + end) // 2
        column = lum[:probe + 1, center]
        top = probe
        while top > 0 and column[top - 1] > threshold:
            top -= 1
        tops.append({"x": center, "top": top, "width": end - start})
    return tops


# --------------------------------------------------------------------------------------------
# Checks.

@case("gpu_shape_keys", "check",
      "Shape keys changed while nothing else moves are drawn, on the CPU and on the GPU, by an "
      "action and by Set Shape Key Value (columns: CPU action, GPU action, CPU node, GPU node).")
class GpuShapeKeys:
    def build(settings):
        bpy = bpy_module()
        material = emission_material("RigWhite")
        for index, (gpu, by_node) in enumerate(((False, False), (True, False),
                                                (False, True), (True, True))):
            name = "Col%d" % index
            ob = new_object(name, box_mesh(name + "Mesh", (1.2, 1.2, 3.0), True),
                            (-4.5 + index * 3.0, 0.0, 0.0), material)
            add_shape_key(ob, "Dent", -1.5, 1.5)
            ob.data.shape_keys.use_gpu_deform = gpu
            if by_node:
                add_game_property(ob, "dent", 'FLOAT', 0.0)
                tree = bpy.data.node_groups.new(name + "Tree", "LogicNodeTree")
                tree.use_fake_user = True
                on_update = tree.nodes.new("LogicNativeOnUpdate")
                get_value = tree.nodes.new("LogicNativeGetGamePropertyFloat")
                get_value.inputs["Property"].default_value = "dent"
                setter = tree.nodes.new("LogicNativeSetShapeKeyValue")
                setter.inputs["Object"].default_value = ob
                setter.inputs["Shape Key"].default_value = "Dent"
                tree.links.new(on_update.outputs["Out"], setter.inputs["Flow"])
                tree.links.new(get_value.outputs["Value"], setter.inputs["Value"])
                binding = ob.game.logic_node_binding_new()
                binding.tree = tree
                binding.tree_name = tree.name
                binding.enabled = True
            else:
                key_action(ob, "Dent", name + "Dent")
        game_settings()
        return add_camera("Cam", (0.0, -20.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=14.0)

    logic = r'''
def step(tick):
    objects = scene().objects
    if tick == 30:
        shot("before")
    elif tick == 40:
        for name in ("Col0", "Col1"):
            objects[name].playAction(name + "Dent", 1, 2, play_mode=bge.logic.KX_ACTION_MODE_PLAY)
        for name in ("Col2", "Col3"):
            objects[name]["dent"] = 1.0
    elif tick == 160:
        shot("after")
    elif tick == 166:
        finish()
'''

    def after(settings, out):
        before = column_tops(load_picture(os.path.join(out, "before.png")), 0.62, 4)
        after = column_tops(load_picture(os.path.join(out, "after.png")), 0.62, 4)
        return {"drops": [a["top"] - b["top"] for a, b in zip(after, before)],
                "heights": [b["width"] * 2.5 for b in before]}

    def judge(settings, values, log_text):
        drops = values["drops"]
        expected = values["heights"][0] * 0.5
        labels = ("CPU action", "GPU action", "CPU node", "GPU node")
        ok = all(drop > expected * 0.6 for drop in drops)
        ok = ok and abs(drops[1] - drops[0]) <= 3 and abs(drops[3] - drops[2]) <= 3
        summary = ", ".join("%s %d px" % (label, drop) for label, drop in zip(labels, drops))
        return ok, "top edges dropped: %s (expected about %d px)" % (summary, expected)


@case("full_copy_shape_action", "check",
      "A shape-key action played on a Full Copy changes the copy's own shape keys (they hold a "
      "copy of the action) and not its source's.")
class FullCopyShapeAction:
    def build(settings):
        material = emission_material("RigWhite")
        ob = new_object("Tall", box_mesh("TallMesh", (1.0, 1.0, 2.0), True), (0.0, 0.0, -20.0),
                        material)
        add_shape_key(ob, "Dent", -1.0, 1.0)
        key_action(ob, "Dent", "TallDent")
        ob.hide_set(True)
        game_settings()
        return add_camera("Cam", (0.0, -10.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=8.0)

    logic = r'''
def step(tick):
    if tick == 10:
        copy = scene().addObject("Tall", scene().objects["Cam"], 0, 1)
        copy.worldPosition = (0.0, 0.0, 0.0)
        upright(copy)
        store["copy"] = copy
    elif tick == 15:
        store["copy"].playAction("TallDent", 1, 2, play_mode=bge.logic.KX_ACTION_MODE_PLAY)
    elif tick == 60:
        copy_key = store["copy"].blenderObject.data.shape_keys
        source = scene().objectsInactive["Tall"].blenderObject.data.shape_keys
        save(copy_value=copy_key.key_blocks["Dent"].value,
             source_value=source.key_blocks["Dent"].value,
             copy_action=copy_key.animation_data.action.name if copy_key.animation_data else "")
    elif tick == 62:
        finish()
'''

    def judge(settings, values, log_text):
        ok = values["copy_value"] > 0.99 and values["source_value"] < 0.01
        return ok, "copy's Dent %.2f (its keys hold %r), source's Dent %.2f" % (
            values["copy_value"], values["copy_action"], values["source_value"])


@case("full_copy_cleanup", "check",
      "Full Copies take their duplicated mesh, shape keys, actions and materials with them when "
      "they go, and the game ends cleanly with some still alive (Blender's data as before).",
      {"SPAWNS": 40})
class FullCopyCleanup:
    def build(settings):
        ob = new_object("Unique", box_mesh("UniqueMesh", (0.5, 0.5, 0.5)), (0.0, 0.0, -20.0),
                        diffuse_material("UniqueMat", (0.8, 0.3, 0.1)))
        add_shape_key(ob, "Dent", -0.2, 0.0)
        key_action(ob, "Dent", "UniqueDent")
        ob.keyframe_insert("location", frame=1)
        ob.animation_data.action.name = "UniqueMove"
        ob.hide_set(True)
        add_sun()
        game_settings()
        # Blender's data after the game is what the game left, not the undo step's.
        bpy_module().context.scene.game_settings.use_undo = False
        return add_camera("Cam", (0.0, -12.0, 0.0), (90.0, 0.0, 0.0))

    logic = r'''
def step(tick):
    spawns = S["SPAWNS"]
    if tick == 5:
        save(game_start=data_counts())
        store["start_names"] = data_names()
    elif 10 <= tick < 10 + spawns:
        for index in range(2):
            copy = scene().addObject("Unique", scene().objects["Cam"], 10, 1)
            copy.worldPosition = ((tick % 10) - 5.0, 0.0, index - 0.5)
            upright(copy)
    elif tick == 10 + spawns + 40:
        names = data_names()
        save(all_gone=data_counts(), left_names={
            kind: [n for n in names[kind] if n not in store["start_names"][kind]][:3]
            for kind in names})
        for index in range(3):
            copy = scene().addObject("Unique", scene().objects["Cam"], 0, 1)
            copy.worldPosition = (index - 1.0, 0.0, 2.0)
    elif tick == 10 + spawns + 50:
        finish()
'''

    def before_game(settings):
        return {"file": blender_data_counts()}

    def after(settings, out):
        return {"after_game": blender_data_counts()}

    def judge(settings, values, log_text):
        start, gone = values["game_start"], values["all_gone"]
        file_before, file_after = values["file"], values["after_game"]
        left = {k: gone[k] - start[k] for k in start if gone[k] != start[k]}
        changed = {k: file_after[k] - file_before[k] for k in file_before
                   if file_after[k] != file_before[k]}
        ok = not left and not changed
        examples = {k: v for k, v in values.get("left_names", {}).items() if v}
        return ok, "after %d copies went: %s; after the game: %s" % (
            settings["SPAWNS"] * 2,
            "nothing left" if not left else "left %r, such as %r" % (left, examples),
            "data as before" if not changed else "changed %r" % changed)


@case("dupli_disabled_collection", "check",
      "Dupli Bases disabled in viewports (one by its own setting, one by its collection's) are "
      "kept in Blender's graph for the game, hidden as with the eye icon: their copies move on a "
      "still view (nothing else changes to make the renderer draw again; outside the graph the "
      "picture stayed as it was), nothing is reported, and after the game both bases are as "
      "before (Undo at Exit off: the game's own restore).")
class DupliDisabledCollection:
    def build(settings):
        bpy = bpy_module()
        scene = bpy.context.scene
        white = emission_material("RigWhite")
        own = new_object("OwnOff", box_mesh("OwnOffMesh", (0.4, 0.4, 0.4)), (0.0, 5.0, 0.0),
                         white)
        own.game.use_upbge_dupli_base = True
        own.hide_viewport = True
        pool = bpy.data.collections.new("Pool")
        scene.collection.children.link(pool)
        pooled = new_object("PoolBase", box_mesh("PoolBaseMesh", (0.4, 0.4, 0.4)),
                            (0.0, 5.0, 0.0), white, collection=pool)
        pooled.game.use_upbge_dupli_base = True
        pool.hide_viewport = True
        game_settings()
        scene.game_settings.use_undo = False
        return add_camera("Cam", (0.0, -10.0, 0.0), (90.0, 0.0, 0.0), ortho_scale=10.0)

    logic = r'''
def step(tick):
    sc = scene()
    if tick == 5:
        for name, z in (("OwnOff", 1.5), ("PoolBase", -1.5)):
            copy = sc.addObject(name, sc.objects["Cam"], 0)
            upright(copy)
            copy.worldPosition = (-3.0, 0.0, z)
            store.setdefault("copies", []).append(copy)
    elif tick > 5:
        for copy in store["copies"]:
            copy.applyMovement((0.05, 0.0, 0.0), False)
    if tick == 30:
        shot("early")
    elif tick == 70:
        shot("late")
    elif tick == 76:
        finish()
'''

    def after(settings, out):
        import numpy
        bpy = bpy_module()

        def centers(picture):
            """The bright copy's middle column in the upper and the lower half."""
            lum = luminance(picture)
            half = lum.shape[0] // 2
            found = []
            for rows in (lum[:half], lum[half:]):
                columns = numpy.nonzero(rows > 0.5)[1]
                found.append(float(columns.mean()) if len(columns) else None)
            return found

        own = bpy.data.objects["OwnOff"]
        pooled = bpy.data.objects["PoolBase"]
        return {"early": centers(load_picture(os.path.join(out, "early.png"))),
                "late": centers(load_picture(os.path.join(out, "late.png"))),
                "bases_after": {"own_disabled": own.hide_viewport, "own_eye": own.hide_get(),
                                "pooled_eye": pooled.hide_get(),
                                "pooled_collections": sorted(c.name for c in
                                                             pooled.users_collection),
                                "pool_disabled": bpy.data.collections["Pool"].hide_viewport}}

    def judge(settings, values, log_text):
        moved = [None if early is None or late is None else late - early
                 for early, late in zip(values["early"], values["late"])]
        expected = {"own_disabled": True, "own_eye": False, "pooled_eye": False,
                    "pooled_collections": ["Pool"], "pool_disabled": True}
        restored = values["bases_after"] == expected
        reported = "Dupli Base" in log_text and "could not be kept" in log_text
        ok = all(m is not None and m > 100 for m in moved) and restored and not reported
        return ok, ("copies moved on a still view (px): base disabled by itself %s, by its "
                    "collection %s; bases after the game %s (as before: %s); reported: %s") % (
            moved[0], moved[1], values["bases_after"], restored, reported)


MERGE_HUES = {"red": 0.0, "yellow": 60.0, "green": 120.0, "cyan": 180.0, "blue": 222.0,
              "magenta": 300.0}


def color_masks(picture):
    """{color name: mask} of the saturated pixels nearest each of MERGE_HUES."""
    import numpy
    top = picture.max(axis=2)
    low = picture.min(axis=2)
    spread = numpy.where(top - low > 1e-6, top - low, 1.0)
    r, g, b = picture[:, :, 0], picture[:, :, 1], picture[:, :, 2]
    hue = numpy.where(top == r, ((g - b) / spread) % 6.0,
                      numpy.where(top == g, (b - r) / spread + 2.0, (r - g) / spread + 4.0)) * 60.0
    saturated = (top > 0.2) & ((top - low) / numpy.maximum(top, 1e-6) > 0.3)
    distance = {name: numpy.minimum(abs(hue - h), 360.0 - abs(hue - h))
                for name, h in MERGE_HUES.items()}
    nearest = numpy.argmin(numpy.stack(list(distance.values())), axis=0)
    return {name: saturated & (nearest == index) for index, name in enumerate(distance)}


def scene_merge_state():
    """What a game must leave as it found: the editor's view, the objects, both scenes'
    collections and the merged objects' hide settings."""
    bpy = bpy_module()
    window, area, _region = window_area_region()
    space = area.spaces[0]
    game = bpy.data.scenes["Game"]
    extra = bpy.data.scenes["Extra"]
    extra_layer = extra.view_layers[0]
    return {"window_scene": window.scene.name,
            "view_layer": window.view_layer.name,
            "view_camera": space.camera.name if space.camera else None,
            "view_perspective": space.region_3d.view_perspective,
            "objects": sorted(o.name for o in bpy.data.objects),
            "game_collection": sorted(o.name for o in game.collection.objects),
            "extra_collection": sorted(o.name for o in extra.collection.objects),
            "extra_hidden": {o.name: [o.hide_viewport, o.hide_get(view_layer=extra_layer)]
                             for o in extra_layer.objects}}


@case("scene_merge", "check",
      "A scene merged by Load Scene draws as the game scene's own (both drawing paths: "
      "UPBGE_DUPLI_GROUPS 0 and 1): its placed object, copies of its eye-hidden template and of "
      "its Dupli Bases (eye-hidden and disabled in viewports) moving on a still view, an Auto "
      "group of 3 rocks, one moved by a node of the merged scene that picks it in its socket; the "
      "game scene's own Dupli copy still moves after the merge (deleting the merged scene took "
      "the drawing's Dupli hook away); Get Collection Objects of a merged collection counts 3; "
      "after the game the editor's view, the objects, both scenes' collections and the merged "
      "objects' hide settings are as before (Undo at Exit off).", timeout=150)
class SceneMerge:
    variants = [{"ENV_UPBGE_DUPLI_GROUPS": "0"}, {"ENV_UPBGE_DUPLI_GROUPS": "1"}]

    def build(settings):
        bpy = bpy_module()
        game = bpy.context.scene
        game.name = "Game"
        extra = bpy.data.scenes.new("Extra")
        cube = box_mesh("MergeCubeMesh", (0.6, 0.6, 0.6))

        def colored(name, color, location, collection, mesh=None):
            if mesh is None:
                mesh = cube.copy()
                mesh.materials.append(emission_material(name + "Mat", color))
            return new_object(name, mesh, location, collection=collection)

        colored("GameCube", (1.0, 1.0, 1.0), (3.0, 0.0, 3.0), game.collection)
        own_base = colored("GameDupli", (1.0, 0.0, 1.0), (0.0, 30.0, 0.0), game.collection)
        own_base.game.use_upbge_dupli_base = True
        own_base.hide_set(True)
        colored("ExtraShown", (1.0, 0.0, 0.0), (3.0, 0.0, 1.0), extra.collection)
        colored("ExtraTmpl", (0.0, 1.0, 0.0), (0.0, 30.0, 0.0), extra.collection)
        base = colored("ExtraDupli", (0.0, 0.3, 1.0), (0.0, 30.0, 0.0), extra.collection)
        base.game.use_upbge_dupli_base = True
        disabled = colored("ExtraDupliOff", (1.0, 1.0, 0.0), (0.0, 30.0, 0.0), extra.collection)
        disabled.game.use_upbge_dupli_base = True
        disabled.hide_viewport = True
        rocks = bpy.data.collections.new("Rocks")
        extra.collection.children.link(rocks)
        rock = colored("Rock", (0.0, 1.0, 1.0), (-4.0, 0.0, 1.0), rocks)
        colored("Rock.001", None, (-4.0, 0.0, -1.0), rocks, mesh=rock.data)
        colored("Rock.002", None, (-2.5, 0.0, 1.0), rocks, mesh=rock.data)
        extra_layer = extra.view_layers[0]
        for name in ("ExtraTmpl", "ExtraDupli"):
            bpy.data.objects[name].hide_set(True, view_layer=extra_layer)
        for scene in (game, extra):
            scene.game_settings.physics_engine = 'JOLT'
        game_settings()
        game.game_settings.use_undo = False

        def bind(ob, tree):
            tree.use_fake_user = True
            binding = ob.game.logic_node_binding_new()
            binding.tree = tree
            binding.tree_name = tree.name
            binding.enabled = True

        # Extra: Rock.001 moves, picked in the node's socket (not the tree's object).
        mover = new_object("ExtraLogic", None, collection=extra.collection)
        tree = bpy.data.node_groups.new("MoveRock", "LogicNodeTree")
        update = tree.nodes.new("LogicNativeOnUpdate")
        move = tree.nodes.new("LogicNativeApplyMovement")
        move.inputs["Object"].default_value = bpy.data.objects["Rock.001"]
        move.inputs["Vector"].default_value = (0.03, 0.0, 0.0)
        tree.links.new(update.outputs[0], move.inputs["Flow"])
        bind(mover, tree)
        # Game: Load Scene "Extra" at start; Rocks' objects counted every frame.
        driver = new_object("Driver", None)
        add_game_property(driver, "rock_count", 'INT', -1)
        tree = bpy.data.node_groups.new("MergeTree", "LogicNodeTree")
        init = tree.nodes.new("LogicNativeOnInit")
        load = tree.nodes.new("LogicNativeLoadScene")
        load.inputs["Scene"].default_value = "Extra"
        tree.links.new(init.outputs[0], load.inputs["Flow"])
        bind(driver, tree)
        tree = bpy.data.node_groups.new("CountRocks", "LogicNodeTree")
        update = tree.nodes.new("LogicNativeOnUpdate")
        members = tree.nodes.new("LogicNativeGetCollectionObjects")
        members.inputs["Collection"].default_value = rocks
        length = tree.nodes.new("LogicNativeListLength")
        store_count = tree.nodes.new("LogicNativeSetGamePropertyInt")
        store_count.inputs["Object"].default_value = driver
        store_count.inputs["Property"].default_value = "rock_count"
        tree.links.new(update.outputs[0], store_count.inputs["Flow"])
        tree.links.new(members.outputs["Objects"], length.inputs["List"])
        tree.links.new(length.outputs["Length"], store_count.inputs["Value"])
        bind(driver, tree)
        return add_camera("Cam", (0.0, -12.0, 0.0), (90.0, 0.0, 0.0), ortho_scale=12.0)

    def before_game(settings):
        bpy = bpy_module()
        bpy.context.scene.camera = bpy.data.objects["Cam"]
        _window, area, _region = window_area_region()
        area.spaces[0].region_3d.view_perspective = 'CAMERA'
        return {"before": scene_merge_state()}

    logic = r'''
def step(tick):
    sc = scene()
    if tick == 10:
        for name, z in (("ExtraTmpl", 2.0), ("ExtraDupli", 0.0), ("ExtraDupliOff", -2.0),
                        ("GameDupli", -3.0)):
            copy = sc.addObject(name, sc.objects["Cam"], 0)
            upright(copy)
            copy.worldPosition = (-4.0, 0.0, z)
            store.setdefault("copies", []).append(copy)
    elif tick > 10:
        for copy in store["copies"]:
            copy.applyMovement((0.05, 0.0, 0.0), False)
    if tick == 40:
        shot("early")
    elif tick == 80:
        save(rock_count=sc.objects["Driver"]["rock_count"])
        shot("late")
    elif tick == 86:
        finish()
'''

    def after(settings, out):
        import numpy
        found = {}
        for name in ("early", "late"):
            picture = load_picture(os.path.join(out, name + ".png"))
            masks = color_masks(picture)
            middle = picture.shape[0] // 2
            shots = {}
            for color, mask in masks.items():
                if color == "cyan":
                    # The still rocks above the middle, the moving one below it.
                    parts = {"cyan_still": mask[:middle], "cyan_moving": mask[middle:]}
                else:
                    parts = {color: mask}
                for part, part_mask in parts.items():
                    columns = numpy.nonzero(part_mask)[1]
                    shots[part] = [int(len(columns)),
                                   float(columns.mean()) if len(columns) else None]
            found[name] = shots
        values = {"shots": found, "after": scene_merge_state()}
        if settings["ENV_UPBGE_DUPLI_GROUPS"] == "1":
            other = os.path.join(os.path.dirname(out), "0", "result.json")
            with open(other) as handle:
                values["per_copy"] = json.load(handle)["values"]
        return values

    def judge(settings, values, log_text):
        def verdict(run):
            early, late = run["shots"]["early"], run["shots"]["late"]
            problems = []
            for color in ("red", "cyan_still"):
                if late[color][0] < 200:
                    problems.append("%s not drawn" % color)
            for color, least in (("green", 100), ("blue", 100), ("yellow", 100),
                                 ("magenta", 100), ("cyan_moving", 60)):
                if early[color][0] < 200 or late[color][0] < 200:
                    problems.append("%s not drawn" % color)
                elif late[color][1] - early[color][1] < least:
                    problems.append("%s moved %.0f px" % (color, late[color][1] - early[color][1]))
            if run.get("rock_count") != 3:
                problems.append("Rocks counted %s" % run.get("rock_count"))
            changed = [key for key in run["before"] if run["before"][key] != run["after"][key]]
            if changed:
                problems.append("changed after the game: %s" % ", ".join(changed))
            return problems

        grouped = verdict(values)
        per_copy = verdict(values["per_copy"]) if "per_copy" in values else ["no per-copy run"]
        reported = "could not" in log_text
        ok = not grouped and not per_copy and not reported
        return ok, "per-copy path: %s; grouped: %s; reported: %s" % (
            "; ".join(per_copy) or "all drawn and moving, data as before",
            "; ".join(grouped) or "all drawn and moving, data as before", reported)


@case("dupli_motion", "check",
      "Moving Dupli copies are drawn as sharp as moving ordinary copies with Temporal "
      "Reprojection on (each copy keeps its own motion in the renderer): widths of 3 Dupli "
      "(top row) and 3 ordinary (bottom row) copies moving 0.04 m a tick.")
class DupliMotion:
    def build(settings):
        bpy = bpy_module()
        bpy.context.scene.eevee.use_taa_reprojection = True
        material = emission_material("RigWhite")
        for name, dupli in (("BaseD", True), ("BaseO", False)):
            ob = new_object(name, box_mesh(name + "Mesh"), (0.0, 0.0, -20.0), material)
            ob.game.use_upbge_dupli_base = dupli
            ob.hide_set(True)
        game_settings()
        return add_camera("Cam", (2.0, -20.0, 0.0), (90.0, 0.0, 0.0), ortho_scale=16.0)

    logic = r'''
def step(tick):
    if tick == 1:
        movers = []
        for name, z in (("BaseD", 1.5), ("BaseO", -1.5)):
            for index in range(3):
                ob = scene().addObject(name, scene().objects["Cam"], 0)
                ob.worldPosition = (index * 2.5 - 5.0, 0.0, z)
                upright(ob)
                movers.append(ob)
        store["movers"] = movers
    for ob in store["movers"]:
        ob.applyMovement((0.04, 0.0, 0.0), False)
    if tick == 100:
        shot("moving")
    elif tick == 105:
        finish()
'''

    def after(settings, out):
        picture = load_picture(os.path.join(out, "moving.png"))
        lum = luminance(picture)
        height = lum.shape[0]
        widths = {}
        for label, fraction in (("dupli", 0.31), ("ordinary", 0.69)):
            row = lum[int(height * fraction)]
            widths[label] = [end - start for start, end in bright_runs(row, 0.5)]
        return widths

    def judge(settings, values, log_text):
        dupli, ordinary = values["dupli"], values["ordinary"]
        if len(dupli) != 3 or len(ordinary) != 3:
            return False, "expected 3 cubes per row, found %r / %r" % (dupli, ordinary)
        reference = sorted(ordinary)[1]
        ok = all(abs(width - reference) <= 2 for width in dupli + ordinary)
        return ok, "Dupli widths %r px, ordinary %r px" % (dupli, ordinary)


@case("shadow_two_cameras", "check",
      "A cube's shadow follows it in two side-by-side cameras once it stops (each camera pass "
      "sees the pose Blender drew).")
class ShadowTwoCameras:
    def build(settings):
        bpy = bpy_module()
        new_object("Ground", box_mesh("GroundMesh", (24.0, 24.0, 0.2)), (0.0, 0.0, -0.1),
                   diffuse_material("GroundMat", (0.8, 0.8, 0.8)))
        new_object("Mover", box_mesh("MoverMesh"), (-3.0, 0.0, 0.5),
                   diffuse_material("MoverMat", (0.9, 0.05, 0.05)))
        add_sun((40.0, 0.0, 50.0), 4.0)
        game_settings()
        left = add_camera("CamL", (0.0, -10.0, 10.0), (45.0, 0.0, 0.0), ortho_scale=12.0)
        add_camera("CamR", (0.0, -10.0, 10.0), (45.0, 0.0, 0.0), ortho_scale=12.0)
        return left

    logic = r'''
def step(tick):
    objects = scene().objects
    if tick == 1:
        width = bge.render.getWindowWidth()
        height = bge.render.getWindowHeight()
        objects["CamL"].setViewport(0, 0, width // 2, height)
        objects["CamR"].setViewport(width // 2, 0, width, height)
        objects["CamL"].useViewport = True
        objects["CamR"].useViewport = True
    if 20 <= tick < 80:
        objects["Mover"].applyMovement((0.03, 0.0, 0.0), False)
    if tick == 15:
        shot("start")
    elif tick == 150:
        shot("end")
    elif tick == 156:
        finish()
'''

    def after(settings, out):
        import numpy
        result = {}
        for name in ("start", "end"):
            picture = load_picture(os.path.join(out, name + ".png"))
            width = picture.shape[1]
            halves = []
            for x0, x1 in ((0, width // 2), (width // 2, width)):
                half = picture[:, x0 + 2:x1 - 2]
                r, g, b = half[:, :, 0], half[:, :, 1], half[:, :, 2]
                red = (r > 1.6 * g) & (r > 1.6 * b) & (r > 0.25)
                lum = luminance(half)
                saturation = half.max(axis=2) - half.min(axis=2)
                ground = numpy.median(lum[(saturation < 0.05) & (lum > 0.05)])
                shadow = (lum < ground * 0.6) & (saturation < 0.05) & ~red & (lum > 0.0)
                cube_y, cube_x = numpy.nonzero(red)
                shadow_y, shadow_x = numpy.nonzero(shadow)
                if len(cube_x) == 0 or len(shadow_x) == 0:
                    halves.append(None)
                    continue
                halves.append({"cube": [float(cube_x.mean()), float(cube_y.mean())],
                               "shadow": [float(shadow_x.mean()), float(shadow_y.mean())],
                               "shadow_px": int(len(shadow_x))})
            result[name] = halves
        return result

    def judge(settings, values, log_text):
        start, end = values["start"], values["end"]
        if None in start or None in end:
            return False, "cube or shadow not found: start %r, end %r" % (start, end)
        notes = []
        ok = True
        offsets = []
        for side, s, e in zip(("left", "right"), start, end):
            moved = e["cube"][0] - s["cube"][0]
            start_offset = [s["shadow"][i] - s["cube"][i] for i in range(2)]
            end_offset = [e["shadow"][i] - e["cube"][i] for i in range(2)]
            offsets.append(end_offset)
            follows = all(abs(end_offset[i] - start_offset[i]) <= 3.0 for i in range(2))
            ok = ok and moved > 10.0 and follows
            notes.append("%s: cube moved %.0f px, shadow offset %.1f,%.1f -> %.1f,%.1f" % (
                side, moved, *start_offset, *end_offset))
        same = all(abs(offsets[0][i] - offsets[1][i]) <= 2.0 for i in range(2))
        return ok and same, "; ".join(notes) + ("" if same else "; the two cameras differ")


@case("dupli_shadow", "check",
      "A moving Dupli copy's shadow leaves nothing where it was, beside a still copy of the same "
      "base (renderers keep each copy's shadow record).")
class DupliShadow:
    def build(settings):
        new_object("Ground", box_mesh("GroundMesh", (30.0, 30.0, 0.2)), (0.0, 0.0, -0.1),
                   diffuse_material("GroundMat", (0.8, 0.8, 0.8)))
        base = new_object("Block", box_mesh("BlockMesh"), (0.0, 0.0, -20.0),
                          diffuse_material("BlockMat", (0.9, 0.05, 0.05)))
        base.game.use_upbge_dupli_base = True
        base.hide_set(True)
        add_sun((35.0, 0.0, 60.0), 4.0)
        game_settings()
        return add_camera("Cam", (0.0, 0.0, 20.0), (0.0, 0.0, 0.0), ortho_scale=16.0)

    logic = r'''
def step(tick):
    if tick == 1:
        copies = []
        for x in (-5.0, 4.0):
            copy = scene().addObject("Block", scene().objects["Cam"], 0)
            copy.worldPosition = (x, 0.0, 0.5)
            upright(copy)
            copies.append(copy)
        store["copies"] = copies
    if tick == 30:
        shot("start")
    if 40 <= tick < 120:
        store["copies"][0].applyMovement((0.07, 0.0, 0.0), False)
    if tick == 220:
        shot("end")
    elif tick == 226:
        finish()
'''

    def after(settings, out):
        import numpy
        result = {}
        for name in ("start", "end"):
            picture = load_picture(os.path.join(out, name + ".png"))
            r, g, b = picture[:, :, 0], picture[:, :, 1], picture[:, :, 2]
            red = (r > 1.6 * g) & (r > 1.6 * b) & (r > 0.25)
            lum = luminance(picture)
            saturation = picture.max(axis=2) - picture.min(axis=2)
            ground = numpy.median(lum[(saturation < 0.05) & (lum > 0.05)])
            shadow = (lum < ground * 0.75) & (saturation < 0.05) & ~red
            result[name] = {"shadow_px": int(shadow.sum()), "block_px": int(red.sum())}
            # Shadow left of the picture's middle: the moving copy starts there, ends right of it.
            width = picture.shape[1]
            result[name]["shadow_left_px"] = int(shadow[:, :int(width * 0.40)].sum())
        return result

    def judge(settings, values, log_text):
        start, end = values["start"], values["end"]
        # At the start one shadow sits left; at the end both copies stand right of x=-1.6 m.
        ghost = end["shadow_left_px"]
        same_total = abs(end["shadow_px"] - start["shadow_px"]) <= start["shadow_px"] * 0.1
        ok = start["shadow_left_px"] > 100 and ghost <= 10 and same_total
        return ok, "shadow px left of the middle: start %d, end %d (ghost); total %d -> %d" % (
            start["shadow_left_px"], ghost, start["shadow_px"], end["shadow_px"])


@case("hide_show", "check",
      "Set Visible hides an object and its shadow, shows it again, keeps it hidden once it left "
      "Blender's graph (hidden over 2 s) and shows it after that (middle of three cubes).",
      timeout=180)
class HideShow:
    def build(settings):
        new_object("Ground", box_mesh("GroundMesh", (30.0, 30.0, 0.2)), (0.0, 0.0, -0.1),
                   diffuse_material("GroundMat", (0.8, 0.8, 0.8)))
        material = diffuse_material("BlockMat", (0.9, 0.05, 0.05))
        for name, x in (("Left", -4.0), ("Middle", 0.0), ("Right", 4.0)):
            new_object(name, box_mesh(name + "Mesh"), (x, 0.0, 0.5), material)
        add_sun((35.0, 0.0, 60.0), 4.0)
        game_settings()
        return add_camera("Cam", (0.0, 0.0, 20.0), (0.0, 0.0, 0.0), ortho_scale=16.0)

    logic = r'''
def step(tick):
    middle = scene().objects["Middle"]
    if tick == 30:
        shot("all")
    elif tick == 40:
        middle.visible = False
    elif tick == 100:
        shot("hidden")
    elif tick == 110:
        middle.visible = True
    elif tick == 170:
        shot("shown")
    elif tick == 180:
        middle.visible = False
    elif tick == 420:
        shot("retired")
    elif tick == 430:
        middle.visible = True
    elif tick == 490:
        shot("back")
    elif tick == 496:
        finish()
'''

    def after(settings, out):
        import numpy
        result = {}
        for name in ("all", "hidden", "shown", "retired", "back"):
            picture = load_picture(os.path.join(out, name + ".png"))
            width = picture.shape[1]
            middle = picture[:, width // 3:2 * width // 3]
            r, g, b = middle[:, :, 0], middle[:, :, 1], middle[:, :, 2]
            red = (r > 1.6 * g) & (r > 1.6 * b) & (r > 0.25)
            lum = luminance(middle)
            saturation = middle.max(axis=2) - middle.min(axis=2)
            ground = numpy.median(lum[(saturation < 0.05) & (lum > 0.05)])
            shadow = (lum < ground * 0.75) & (saturation < 0.05) & ~red
            result[name] = [int(red.sum()), int(shadow.sum())]
        return result

    def judge(settings, values, log_text):
        cube, shadow = values["all"]
        ok = cube > 1000 and shadow > 500
        for name in ("hidden", "retired"):
            ok = ok and values[name][0] <= cube * 0.02 and values[name][1] <= shadow * 0.02
        for name in ("shown", "back"):
            ok = ok and abs(values[name][0] - cube) <= cube * 0.05
            ok = ok and abs(values[name][1] - shadow) <= shadow * 0.05
        return ok, "middle cube / shadow px: " + ", ".join(
            "%s %d/%d" % (name, *values[name])
            for name in ("all", "hidden", "shown", "retired", "back"))


@case("copy_reuse", "check",
      "A removed ordinary copy's Blender object serves the next copy of its source (no graph "
      "rebuild), with the source's color, not the color the last copy was given; spawning keeps "
      "the Blender object count bounded, and parked objects are deleted after a while.",
      timeout=180)
class CopyReuse:
    def build(settings):
        bpy = bpy_module()
        material = bpy.data.materials.new("SparkMat")
        material.use_nodes = True
        nodes = material.node_tree.nodes
        nodes.clear()
        info = nodes.new("ShaderNodeObjectInfo")
        emission = nodes.new("ShaderNodeEmission")
        output = nodes.new("ShaderNodeOutputMaterial")
        material.node_tree.links.new(info.outputs["Color"], emission.inputs["Color"])
        material.node_tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])
        spark = new_object("Spark", box_mesh("SparkMesh"), (0.0, 0.0, -20.0), material)
        spark.color = (1.0, 0.0, 0.0, 1.0)
        spark.hide_set(True)
        game_settings()
        return add_camera("Cam", (0.0, -10.0, 0.0), (90.0, 0.0, 0.0), ortho_scale=8.0)

    logic = r'''
def spawn(position, life):
    copy = scene().addObject("Spark", scene().objects["Cam"], life)
    copy.worldPosition = position
    upright(copy)
    return copy


def step(tick):
    import bpy
    if tick == 5:
        save(start_objects=len(bpy.data.objects))
        store["a"] = spawn((0.0, 0.0, 0.0), 0)
        store["a"].color = (0.0, 0.0, 1.0, 1.0)
        store["a_name"] = store["a"].blenderObject.name
    elif tick == 30:
        shot("a")
    elif tick == 35:
        store["a"].endObject()
    elif tick == 40:
        store["b"] = spawn((0.0, 0.0, 0.0), 0)
        save(reused=store["b"].blenderObject.name == store["a_name"])
    elif tick == 70:
        shot("b")
    elif tick == 75:
        store["b"].endObject()
    elif 80 <= tick < 140:
        for index in range(2):
            spawn(((tick % 8) - 4.0, 0.0, index * 1.5 - 3.0), 5)
        store["most"] = max(store.get("most", 0), len(bpy.data.objects))
    elif tick == 145:
        save(most_while_spawning=store["most"])
    elif tick == 420:
        save(end_objects=len(bpy.data.objects))
        finish()
'''

    def after(settings, out):
        import numpy
        colors = {}
        for name in ("a", "b"):
            picture = load_picture(os.path.join(out, name + ".png"))
            height, width = picture.shape[:2]
            center = picture[height // 2 - 5:height // 2 + 5, width // 2 - 5:width // 2 + 5]
            colors[name] = [round(float(v), 2) for v in center.reshape(-1, 3).mean(axis=0)]
        return {"colors": colors}

    def judge(settings, values, log_text):
        a, b = values["colors"]["a"], values["colors"]["b"]
        blue_then_red = a[2] > 0.5 and a[0] < 0.2 and b[0] > 0.5 and b[2] < 0.2
        start = values["start_objects"]
        bounded = values["most_while_spawning"] <= start + 16
        gone = values["end_objects"] == start
        ok = values["reused"] and blue_then_red and bounded and gone
        return ok, ("reused %s; first copy %r, next %r; Blender objects %d at start, at most %d "
                    "while spawning 2 a tick (life 5), %d later") % (
            values["reused"], a, b, start, values["most_while_spawning"], values["end_objects"])


@case("own_mesh", "check",
      "Add Object's Own Mesh: Set Shape Key Value on that copy changes it alone; a copy without it "
      "keeps sharing its source's mesh, which stays as it was (logic nodes).")
class OwnMesh:
    def build(settings):
        bpy = bpy_module()
        blob = new_object("Blob", box_mesh("BlobMesh", (1.2, 1.2, 3.0), True), (0.0, 0.0, -20.0),
                          emission_material("RigWhite"))
        add_shape_key(blob, "Dent", -1.5, 1.5)
        blob.hide_set(True)
        for name, x in (("SpotA", -2.0), ("SpotB", 2.0)):
            new_object(name, None, (x, 0.0, 0.0))
        game_settings()
        camera = add_camera("Cam", (0.0, -20.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=10.0)
        tree = bpy.data.node_groups.new("OwnMeshTree", "LogicNodeTree")
        tree.use_fake_user = True
        on_init = tree.nodes.new("LogicNativeOnInit")
        for spot, own in (("SpotA", True), ("SpotB", False)):
            add = tree.nodes.new("LogicNativeAddObject")
            add.inputs["Object to Add"].default_value = blob
            add.inputs["Copy Transform"].default_value = bpy.data.objects[spot]
            add.inputs["Own Mesh"].default_value = own
            tree.links.new(on_init.outputs["Out"], add.inputs["Flow"])
            if own:
                setter = tree.nodes.new("LogicNativeSetShapeKeyValue")
                setter.inputs["Shape Key"].default_value = "Dent"
                setter.inputs["Value"].default_value = 1.0
                tree.links.new(add.outputs[0], setter.inputs["Flow"])
                tree.links.new(add.outputs[1], setter.inputs["Object"])
        binding = camera.game.logic_node_binding_new()
        binding.tree = tree
        binding.tree_name = tree.name
        binding.enabled = True
        return camera

    logic = r'''
def step(tick):
    if tick == 60:
        shot("copies")
        import bpy
        source = bpy.data.objects["Blob"].data
        copies = [o for o in scene().objects if o.name.startswith("Blob")]
        meshes = sorted("%s:%s=%.1f" % (o.blenderObject.name, o.blenderObject.data.name,
                                         o.blenderObject.data.shape_keys.key_blocks["Dent"].value)
                        for o in copies)
        save(copies=len(copies), own_meshes=sum(1 for o in copies if o.blenderObject.data != source),
             source_dent=source.shape_keys.key_blocks["Dent"].value, meshes=meshes)
    elif tick == 66:
        finish()
'''

    def after(settings, out):
        tops = column_tops(load_picture(os.path.join(out, "copies.png")), 0.58, 2)
        return {"tops": [t["top"] for t in tops], "height": tops[0]["width"] * 2.5}

    def judge(settings, values, log_text):
        left, right = values["tops"]
        dented = left - right > values["height"] * 0.3
        ok = (values["copies"] == 2 and values["own_meshes"] == 1 and dented and
              values["source_dent"] == 0.0)
        return ok, ("%d copies, %d with an own mesh (%r); own-mesh copy's top %d px lower than "
                    "the other's; source's Dent %.2f") % (
            values["copies"], values["own_meshes"], values["meshes"], left - right,
            values["source_dent"])


def bright_runs_at_height(picture_path, height_fraction):
    """Bright objects crossing the row at height_fraction of the picture (from the top)."""
    lum = luminance(load_picture(picture_path))
    row = lum[int(lum.shape[0] * height_fraction)]
    return [run for run in bright_runs(row, 0.4) if run[1] - run[0] >= 4]


def object_info_material(name):
    """Emission colored by Object Info: Random (red), the object color's red (green), 0.3 blue."""
    bpy = bpy_module()
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()
    info = nodes.new("ShaderNodeObjectInfo")
    split = nodes.new("ShaderNodeSeparateColor")
    combine = nodes.new("ShaderNodeCombineColor")
    combine.inputs["Blue"].default_value = 0.3
    emission = nodes.new("ShaderNodeEmission")
    output = nodes.new("ShaderNodeOutputMaterial")
    links = material.node_tree.links
    links.new(info.outputs["Color"], split.inputs["Color"])
    links.new(info.outputs["Random"], combine.inputs["Red"])
    links.new(split.outputs["Red"], combine.inputs["Green"])
    links.new(combine.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return material


def mesh_objects_out_of_graph():
    return sorted(o.name for o in bpy_module().data.objects if o.type == 'MESH' and o.hide_viewport)


@case("auto_objects", "check",
      "Game Drawing Auto (every object's default): identical cubes share one Blender object per "
      "group (their own leave Blender's graph) and still move and hide on their own; cubes with "
      "an object-linked material form their own group; a constrained cube and an Own Object cube "
      "are not grouped; after the game the Blender data is as before.")
class AutoObjects:
    def build(settings):
        white = emission_material("RigWhite")
        red = emission_material("RigRed", (1.0, 0.1, 0.1))
        mesh = box_mesh("CrateMesh")
        mesh.materials.append(white)
        for index in range(4):
            new_object("Crate%d" % index, mesh, (index * 2.5 - 7.5, 0.0, 0.0))
        for index in range(2):
            odd = new_object("Odd%d" % index, mesh, (index * 2.5 + 2.5, 0.0, 0.0))
            odd.material_slots[0].link = 'OBJECT'
            odd.material_slots[0].material = red
        # Not grouped: Blender poses it (a constraint), and one set to Own Object (below the rows
        # the check reads).
        stiff = new_object("Stiff", mesh, (7.5, 0.0, 0.0))
        stiff.constraints.new('LIMIT_ROTATION')
        kept = new_object("Kept", mesh, (0.0, 0.0, -3.5))
        kept.game.draw_as = 'OWN'
        # Below the rows: Watch0 is read by another object's modifier (not grouped), the other two
        # join the cubes' group.
        for index in range(3):
            new_object("Watch%d" % index, mesh, (index * 2.5 - 7.5, 0.0, -3.5))
        reader = new_object("Reader", box_mesh("ReaderMesh", (0.3, 0.3, 0.3)), (7.5, 0.0, -3.5),
                            emission_material("RigBlue", (0.1, 0.1, 1.0)))
        wrap = reader.modifiers.new("Wrap", 'SHRINKWRAP')
        wrap.target = bpy_module().data.objects["Watch0"]
        game_settings()
        return add_camera("Cam", (0.0, -20.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=20.0)

    def before_game(settings):
        return {"file": blender_data_counts()}

    logic = r'''
def step(tick):
    import bpy
    objects = scene().objects
    if tick == 30:
        shot("start")
        save(out_of_graph=sorted(o.name for o in bpy.data.objects
                                 if o.type == 'MESH' and o.hide_viewport),
             playing=data_counts())
    elif tick == 40:
        objects["Crate1"].applyMovement((0.0, 0.0, 2.0), False)
        objects["Crate3"].visible = False
    elif tick == 100:
        shot("after")
    elif tick == 106:
        finish()
'''

    def after(settings, out):
        import numpy
        rows = {}
        for name in ("start", "after"):
            path = os.path.join(out, name + ".png")
            # Rows through z = 0 and z = 2 (camera at z = 1, 11.25 m tall view).
            rows[name] = [len(bright_runs_at_height(path, f)) for f in (0.59, 0.41)]
        picture = load_picture(os.path.join(out, "start.png"))
        row = picture[int(picture.shape[0] * 0.59)]
        red = (row[:, 0] > 0.5) & (row[:, 0] > 1.6 * row[:, 1])
        rows["red_cubes"] = len([run for run in bright_runs(red.astype(numpy.float32), 0.5)
                                 if run[1] - run[0] >= 4])
        rows["after_game_out_of_graph"] = mesh_objects_out_of_graph()
        rows["after_game"] = blender_data_counts()
        return rows

    def judge(settings, values, log_text):
        expected_out = ["Crate0", "Crate1", "Crate2", "Crate3", "Odd0", "Odd1", "Watch1", "Watch2"]
        # Blender objects added while playing: one per group, and the game's default camera.
        groups = values["playing"]["objects"] - values["file"]["objects"] - 1
        ok = (values["start"] == [7, 0] and values["after"] == [5, 1] and
              values["red_cubes"] == 2 and values["out_of_graph"] == expected_out and
              groups == 2 and values["after_game_out_of_graph"] == [] and
              values["after_game"] == values["file"])
        return ok, ("cubes on the ground/raised row: start %r, after move+hide %r, red %d; out "
                    "of Blender's graph while playing %r, %d group objects; after the game "
                    "%r hidden, data %s") % (
            values["start"], values["after"], values["red_cubes"], values["out_of_graph"],
            groups, values["after_game_out_of_graph"],
            "as before" if values["after_game"] == values["file"] else
            "changed %r -> %r" % (values["file"], values["after_game"]))


@case("auto_save_load", "check",
      "Grouped (Auto) objects in Save Game / Load Game (logic nodes): a raised, a hidden and a "
      "recolored cube are saved, changed back, and loaded, first with every cube Own Object, then "
      "Auto; Auto loads exactly what Own does (Save Object State keeps poses), and the cubes stay "
      "grouped.")
class AutoSaveLoad:
    variants = [{"DRAW": "OWN"}, {"DRAW": "AUTO"}]

    def build(settings):
        bpy = bpy_module()
        mesh = box_mesh("CrateMesh")
        mesh.materials.append(object_info_material("RigInfo"))
        for index in range(4):
            ob = new_object("Crate%d" % index, mesh, (index * 2.5 - 3.75, 0.0, 0.0))
            ob.game.use_save_game_object_state = True
            ob.game.draw_as = settings["DRAW"]
        game_settings()
        camera = add_camera("Cam", (0.0, -20.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=14.0)
        add_game_property(camera, "do_save", 'BOOL', False)
        add_game_property(camera, "do_load", 'BOOL', False)
        add_game_property(camera, "loaded", 'BOOL', False)
        tree = bpy.data.node_groups.new("SaveLoadTree", "LogicNodeTree")
        tree.use_fake_user = True
        on_update = tree.nodes.new("LogicNativeOnUpdate")
        path = os.path.join(settings["OUT"], "auto_checkpoint")
        for prop, node_type in (("do_save", "LogicNativeSaveGame"),
                                ("do_load", "LogicNativeLoadGame")):
            get = tree.nodes.new("LogicNativeGetGamePropertyBool")
            get.inputs["Property"].default_value = prop
            branch = tree.nodes.new("LogicNativeBranch")
            act = tree.nodes.new(node_type)
            act.inputs["Path"].default_value = path
            clear = tree.nodes.new("LogicNativeSetGamePropertyBool")
            clear.inputs["Property"].default_value = prop
            clear.inputs["Value"].default_value = False
            tree.links.new(on_update.outputs["Out"], branch.inputs["Flow"])
            tree.links.new(get.outputs["Value"], branch.inputs["Condition"])
            tree.links.new(branch.outputs["True"], act.inputs["Flow"])
            tree.links.new(act.outputs["Done"], clear.inputs["Flow"])
            if prop == "do_load":
                mark = tree.nodes.new("LogicNativeSetGamePropertyBool")
                mark.inputs["Property"].default_value = "loaded"
                tree.links.new(act.outputs["Success"], mark.inputs["Value"])
                tree.links.new(act.outputs["Done"], mark.inputs["Flow"])
        binding = camera.game.logic_node_binding_new()
        binding.tree = tree
        binding.tree_name = tree.name
        binding.enabled = True
        return camera

    logic = r'''
def step(tick):
    import bpy
    objects = scene().objects
    cam = objects["Cam"]
    if tick == 20:
        objects["Crate1"].applyMovement((0.0, 0.0, 2.0), False)
        objects["Crate2"].visible = False
        objects["Crate3"].color = (1.0, 0.0, 0.0, 1.0)
    elif tick == 25:
        cam["do_save"] = True
    elif tick == 45:
        objects["Crate1"].applyMovement((0.0, 0.0, -2.0), False)
        objects["Crate2"].visible = True
        objects["Crate3"].color = (1.0, 1.0, 1.0, 1.0)
    elif tick == 50:
        cam["do_load"] = True
    elif tick == 130:
        shot("loaded")
        save(loaded=bool(cam["loaded"]),
             crate1_z=round(objects["Crate1"].worldPosition.z, 3),
             crate2_visible=bool(objects["Crate2"].visible),
             crate3_color=[round(c, 3) for c in objects["Crate3"].color],
             out_of_graph=sorted(o.name for o in bpy.data.objects
                                 if o.type == 'MESH' and o.hide_viewport))
    elif tick == 136:
        finish()
'''

    def after(settings, out):
        path = os.path.join(out, "loaded.png")
        result = {"rows": [len(bright_runs_at_height(path, f)) for f in (0.63, 0.37)],
                  "after_game_out_of_graph": mesh_objects_out_of_graph()}
        if settings["DRAW"] == "AUTO":
            own_dir = os.path.join(os.path.dirname(out), "OWN")
            with open(os.path.join(own_dir, "game.json")) as handle:
                own = json.load(handle)
            own["rows"] = [len(bright_runs_at_height(os.path.join(own_dir, "loaded.png"), f))
                           for f in (0.63, 0.37)]
            result["own"] = own
        return result

    def judge(settings, values, log_text):
        own = values["own"]
        keys = ("loaded", "crate1_z", "crate2_visible", "crate3_color", "rows")
        differ = [key for key in keys if values[key] != own[key]]
        ok = (values["loaded"] and values["crate1_z"] == 2.0 and not differ and
              values["out_of_graph"] == ["Crate0", "Crate1", "Crate2", "Crate3"] and
              own["out_of_graph"] == [] and values["after_game_out_of_graph"] == [])
        return ok, ("loaded %s: Crate1 z %.2f, Crate2 visible %s, Crate3 color %r, cubes on the "
                    "ground/raised row %r; %s; grouped %r") % (
            values["loaded"], values["crate1_z"], values["crate2_visible"],
            values["crate3_color"], values["rows"],
            "the same as Own" if not differ else "differs from Own in %r" % differ,
            values["out_of_graph"])


def build_mixed_drawing_scene(settings, white, info, ortho_scale=14.0, camera_z=1.5):
    """Crates (Object Info material) set to DRAW_AUTO, walls (plain material) set to DRAW_MERGED,
    as the settings say; the camera."""
    crate_mesh = box_mesh("CrateMesh")
    crate_mesh.materials.append(info)
    for index in range(4):
        crate = new_object("Crate%d" % index, crate_mesh, (index * 2.0 - 3.0, 0.0, 0.0))
        crate.color = (0.2 + 0.15 * index, 1.0, 1.0, 1.0)
        crate.game.draw_as = settings["DRAW_AUTO"]
    for index in range(5):
        wall = new_object("Wall%d" % index, box_mesh("WallMesh%d" % index, (1.2, 0.4, 0.8)),
                          (index * 1.6 - 3.2, 0.0, 2.2), white)
        wall.game.draw_as = settings["DRAW_MERGED"]
    game_settings()
    return add_camera("Cam", (0.0, -20.0, camera_z), (90.0, 0.0, 0.0), ortho_scale=ortho_scale)


MIXED_DRAWING_LOGIC = r'''
def change(tick):
    objects = scene().objects
    if tick == 40:
        objects["Crate1"].applyMovement((0.0, 0.0, -1.5), False)
        objects["Crate2"].color = (1.0, 0.2, 0.2, 1.0)
        objects["Crate3"].visible = False
        objects["Wall1"].visible = False
        objects["Wall3"].endObject()
    elif tick == 70:
        objects["Wall1"].visible = True
'''


def compare_with_variant(out, other, name):
    """Mean difference, pixels differing by more than 2% and the largest difference between this
    variant's picture and another variant's (in the same case folder)."""
    import numpy
    mine = load_picture(os.path.join(out, name + ".png"))
    theirs = load_picture(os.path.join(os.path.dirname(out), other, name + ".png"))
    if mine.shape != theirs.shape:
        return None
    difference = numpy.abs(mine - theirs).max(axis=2)
    return {"mean": float(difference.mean()), "over_2_percent": int((difference > 0.02).sum()),
            "max": float(difference.max())}


@case("split_views", "check",
      "Auto and Merged in two camera views side by side (each view is its own camera pass, as in "
      "split screen): the scene played with every object Own, then crates Auto and walls Merged, "
      "with a moved, recolored and hidden crate and a hidden-then-shown and a removed wall; the "
      "two pictures must match.",
      timeout=200)
class SplitViews:
    variants = [{"DRAW_AUTO": "OWN", "DRAW_MERGED": "OWN"},
                {"DRAW_AUTO": "AUTO", "DRAW_MERGED": "MERGED"}]

    def build(settings):
        camera = build_mixed_drawing_scene(settings, emission_material("RigWhite"),
                                           object_info_material("RigInfo"))
        add_camera("Right", (1.0, -20.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=10.0)
        return camera

    logic = MIXED_DRAWING_LOGIC + r'''
def step(tick):
    import bpy
    objects = scene().objects
    if tick == 1:
        width = bge.render.getWindowWidth()
        height = bge.render.getWindowHeight()
        objects["Cam"].setViewport(0, 0, width // 2, height)
        objects["Right"].setViewport(width // 2, 0, width, height)
        objects["Cam"].useViewport = True
        objects["Right"].useViewport = True
    change(tick)
    if tick == 30:
        save(out_of_graph=sorted(o.name for o in bpy.data.objects
                                 if o.type == 'MESH' and o.hide_viewport))
    elif tick == 150:
        shot("views")
    elif tick == 156:
        finish()
'''

    def after(settings, out):
        result = {"after_game_out_of_graph": mesh_objects_out_of_graph()}
        if settings["DRAW_AUTO"] == "AUTO":
            result["diff"] = compare_with_variant(out, "OWN_OWN", "views")
        return result

    def judge(settings, values, log_text):
        diff = values["diff"]
        expected = ["Crate0", "Crate1", "Crate2", "Crate3",
                    "Wall0", "Wall1", "Wall2", "Wall3", "Wall4"]
        ok = (diff is not None and diff["over_2_percent"] <= 50 and
              values["out_of_graph"] == expected and values["after_game_out_of_graph"] == [])
        return ok, ("Auto+Merged vs Own in two views: %s; out of Blender's graph while playing "
                    "%r") % ("mean difference %.5f, %d pixels over 2%%, largest %.3f" % (
                        diff["mean"], diff["over_2_percent"], diff["max"]) if diff else
                    "pictures could not be compared", values["out_of_graph"])


@case("viewport_render_drawing", "check",
      "Use Viewport Render with Auto and Merged: Auto objects stay drawn by their own objects (the "
      "viewport draw has no per-camera hiding), Merged still works (hide, show, remove); the "
      "pictures match the all-Own run.",
      timeout=200)
class ViewportRenderDrawing:
    variants = [{"DRAW_AUTO": "OWN", "DRAW_MERGED": "OWN"},
                {"DRAW_AUTO": "AUTO", "DRAW_MERGED": "MERGED"}]
    blank_file = True

    def build(settings):
        camera = build_mixed_drawing_scene(settings, emission_material("RigWhite"),
                                           object_info_material("RigInfo"))
        bpy_module().context.scene.game_settings.use_viewport_render = True
        # The game draws through the 3D view: its shading (materials) and no overlays.
        space = window_area_region()[1].spaces[0]
        space.shading.type = 'MATERIAL'
        space.overlay.show_overlays = False
        return camera

    logic = MIXED_DRAWING_LOGIC + r'''
def step(tick):
    import bpy
    change(tick)
    if tick == 30:
        save(out_of_graph=sorted(o.name for o in bpy.data.objects
                                 if o.type == 'MESH' and o.hide_viewport))
    elif tick == 150:
        shot("viewport")
    elif tick == 156:
        finish()
'''

    def after(settings, out):
        result = {"after_game_out_of_graph": mesh_objects_out_of_graph()}
        if settings["DRAW_AUTO"] == "AUTO":
            result["diff"] = compare_with_variant(out, "OWN_OWN", "viewport")
        return result

    def judge(settings, values, log_text):
        diff = values["diff"]
        expected = ["Wall0", "Wall1", "Wall2", "Wall3", "Wall4"]
        ok = (diff is not None and diff["over_2_percent"] <= 50 and
              values["out_of_graph"] == expected and values["after_game_out_of_graph"] == [])
        return ok, ("Use Viewport Render, Auto+Merged vs Own: %s; out of Blender's graph while "
                    "playing %r (only the Merged walls)") % (
            "mean difference %.5f, %d pixels over 2%%, largest %.3f" % (
                diff["mean"], diff["over_2_percent"], diff["max"]) if diff else
            "pictures could not be compared", values["out_of_graph"])


@case("second_scene", "check",
      "Switching to another scene (this fork runs one game scene at a time): the new scene's "
      "Dupli copies (green) and its group of identical Auto cubes (blue) are drawn, nothing of "
      "the first scene (red) is left.",
      timeout=180)
class SecondScene:
    def build(settings):
        bpy = bpy_module()
        main = bpy.context.scene
        red = new_object("BaseRed", box_mesh("RedMesh"), (0.0, 0.0, -30.0),
                         emission_material("RigRed", (1.0, 0.1, 0.1)))
        red.game.use_upbge_dupli_base = True
        red.hide_set(True)
        game_settings()
        camera = add_camera("Cam", (0.0, -20.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=14.0)
        over = bpy.data.scenes.new("Over")
        for name in ("physics_engine", "physics_timestep_method", "use_fixed_physics_timestep",
                     "physics_tick_rate", "use_fixed_fps_cap", "fixed_render_cap_rate", "vsync"):
            setattr(over.game_settings, name, getattr(main.game_settings, name))
        green = bpy.data.objects.new("BaseGreen", box_mesh("GreenMesh"))
        green.data.materials.append(emission_material("RigGreen", (0.1, 1.0, 0.1)))
        green.location = (0.0, 0.0, -30.0)
        green.game.physics_type = 'NO_COLLISION'
        green.game.use_upbge_dupli_base = True
        over.collection.objects.link(green)
        blue_mesh = box_mesh("BlueMesh")
        blue_mesh.materials.append(emission_material("RigBlue", (0.1, 0.1, 1.0)))
        for index in range(3):
            blue = bpy.data.objects.new("Blue%d" % index, blue_mesh)
            blue.location = (index * 2.0 - 2.0, 0.0, -2.0)
            blue.game.physics_type = 'NO_COLLISION'
            over.collection.objects.link(blue)
        over_camera = bpy.data.objects.new("OverCam", bpy.data.cameras.new("OverCam"))
        over_camera.location = (0.0, -20.0, 1.0)
        over_camera.rotation_euler = (1.5707963, 0.0, 0.0)
        over_camera.data.type = 'ORTHO'
        over_camera.data.ortho_scale = 14.0
        over.collection.objects.link(over_camera)
        over.camera = over_camera
        green.hide_set(True, view_layer=over.view_layers[0])
        # The same logic module drives the second scene from its camera.
        attach_logic(over_camera, "rig_logic")
        return camera

    logic = r'''
def step(tick):
    import bpy
    current = scene()
    if current.name != "Over":
        if tick == 3:
            for index in range(3):
                copy = current.addObject("BaseRed", current.objects["Cam"], 0)
                copy.worldPosition = (index * 2.0 - 2.0, 0.0, 2.0)
                upright(copy)
        elif tick == 20:
            current.replace("Over")
        return
    over_tick = store["over_tick"] = store.get("over_tick", 0) + 1
    if over_tick == 2:
        for index in range(3):
            copy = current.addObject("BaseGreen", current.objects["OverCam"], 0)
            copy.worldPosition = (index * 2.0 - 2.0, 0.0, 0.0)
            upright(copy)
        save(blue_out_of_graph=sorted(o.name for o in bpy.data.objects
                                      if o.name.startswith("Blue") and o.hide_viewport))
    elif over_tick == 80:
        shot("scenes")
    elif over_tick == 86:
        finish()
'''

    def after(settings, out):
        picture = load_picture(os.path.join(out, "scenes.png"))
        height = picture.shape[0]
        counts = {}
        # Rows through z = 2 (red), 0 (green) and -2 (blue): camera at z = 1, 7.9 m tall view.
        for name, channel, fraction in (("red", 0, 0.37), ("green", 1, 0.63), ("blue", 2, 0.88)):
            row = picture[int(height * fraction)]
            others = [c for c in range(3) if c != channel]
            # Display colors: an emission green reads about (0.5, 0.75, 0.45).
            mask = ((row[:, channel] > 0.4) & (row[:, channel] > 1.25 * row[:, others[0]]) &
                    (row[:, channel] > 1.25 * row[:, others[1]]))
            counts[name] = len([run for run in bright_runs(mask.astype("float32"), 0.5)
                                if run[1] - run[0] >= 4])
        return {"counts": counts}

    def judge(settings, values, log_text):
        counts = values["counts"]
        grouped = values.get("blue_out_of_graph")
        ok = (counts == {"red": 0, "green": 3, "blue": 3} and
              grouped == ["Blue0", "Blue1", "Blue2"])
        return ok, ("after the switch: %d green (the new scene's Dupli copies), %d blue (its Auto "
                    "group, out of the graph: %r), %d red (the first scene's)") % (
            counts["green"], counts["blue"], grouped, counts["red"])


@case("auto_look", "check",
      "Auto looks exactly like Own Object: the same scene played twice, every object Own and then "
      "Auto (shaders reading Object Info Random and Color, a moved, a recolored and a hidden cube, "
      "one cube leaving its group through a modifier action, and a second camera showing only a "
      "collection); the two pictures must match.",
      timeout=200)
class AutoLook:
    variants = [{"DRAW": "OWN"}, {"DRAW": "AUTO"}]

    def build(settings):
        bpy = bpy_module()
        mesh = box_mesh("CrateMesh")
        mesh.materials.append(object_info_material("RigInfo"))
        pair = bpy.data.collections.new("Pair")
        bpy.context.scene.collection.children.link(pair)
        for index in range(6):
            ob = new_object("Crate%d" % index, mesh, (index * 2.0 - 5.0, 0.0, 0.0),
                            collection=pair if index < 2 else None)
            ob.color = (0.2 + 0.12 * index, 1.0, 1.0, 1.0)
            ob.game.draw_as = settings["DRAW"]
            modifier = ob.modifiers.new("Grow", 'DISPLACE')
            modifier.strength = 0.0
        # The action every cube can play (keyed on the first, then taken off it).
        first = bpy.data.objects["Crate0"]
        modifier = first.modifiers["Grow"]
        modifier.keyframe_insert("strength", frame=1)
        modifier.strength = 1.0
        modifier.keyframe_insert("strength", frame=2)
        modifier.strength = 0.0
        first.animation_data.action.name = "Grow"
        first.animation_data.action = None
        # A modifier that reads the object's position (global texture coordinates): its shape
        # depends on where it stands, so these two are never grouped.
        texture = bpy.data.textures.new("RigClouds", 'CLOUDS')
        texture.noise_scale = 0.4
        wavy_mesh = box_mesh("WavyMesh")
        subdivided = wavy_mesh
        import bmesh
        bm = bmesh.new()
        bm.from_mesh(subdivided)
        bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=6, use_grid_fill=True)
        bm.to_mesh(subdivided)
        bm.free()
        wavy_mesh.materials.append(object_info_material("RigInfoWavy"))
        for index in range(2):
            wavy = new_object("Wavy%d" % index, wavy_mesh, (index * 3.0 - 1.5, 0.0, 2.6))
            wavy.game.draw_as = settings["DRAW"]
            displace = wavy.modifiers.new("Bumps", 'DISPLACE')
            displace.texture = texture
            displace.texture_coords = 'GLOBAL'
            displace.strength = 0.4
        game_settings()
        mini = add_camera("Mini", (-4.0, -20.0, 0.0), (90.0, 0.0, 0.0), ortho_scale=6.0)
        mini.data.game_collection = pair
        mini.data.game_collection_mode = 'ONLY'
        return add_camera("Cam", (0.0, -20.0, 1.5), (90.0, 0.0, 0.0), ortho_scale=14.0)

    logic = r'''
def step(tick):
    import bpy
    objects = scene().objects
    if tick == 1:
        width = bge.render.getWindowWidth()
        height = bge.render.getWindowHeight()
        objects["Cam"].setViewport(0, 0, width * 2 // 3, height)
        objects["Mini"].setViewport(width * 2 // 3, 0, width, height)
        objects["Cam"].useViewport = True
        objects["Mini"].useViewport = True
    if tick == 30:
        save(out_of_graph_start=sorted(o.name for o in bpy.data.objects
                                       if o.type == 'MESH' and o.hide_viewport))
    elif tick == 40:
        objects["Crate2"].playAction("Grow", 1, 2, play_mode=bge.logic.KX_ACTION_MODE_PLAY)
        objects["Crate3"].color = (1.0, 0.2, 0.2, 1.0)
        objects["Crate4"].applyMovement((0.0, 0.0, 1.5), False)
        objects["Crate5"].visible = False
    elif tick == 150:
        shot("look")
        save(out_of_graph_end=sorted(o.name for o in bpy.data.objects
                                     if o.type == 'MESH' and o.hide_viewport))
    elif tick == 156:
        finish()
'''

    def after(settings, out):
        import numpy
        result = {"after_game_out_of_graph": mesh_objects_out_of_graph()}
        if settings["DRAW"] != "AUTO":
            return result
        auto = load_picture(os.path.join(out, "look.png"))
        own = load_picture(os.path.join(os.path.dirname(out), "OWN", "look.png"))
        if auto.shape != own.shape:
            result["diff"] = None
            return result
        difference = numpy.abs(auto - own).max(axis=2)
        lum = luminance(own)
        result["diff"] = {"mean": float(difference.mean()),
                          "pixels_over_2_percent": int((difference > 0.02).sum()),
                          "max": float(difference.max()),
                          "cube_pixels": int((lum > 0.05).sum())}
        return result

    def judge(settings, values, log_text):
        diff = values.get("diff")
        if diff is None:
            return False, "the two pictures could not be compared"
        start = ["Crate%d" % i for i in range(6)]
        end = ["Crate0", "Crate1", "Crate3", "Crate4", "Crate5"]
        same = diff["pixels_over_2_percent"] <= diff["cube_pixels"] * 0.002
        ok = (same and values["out_of_graph_start"] == start and
              values["out_of_graph_end"] == end and values["after_game_out_of_graph"] == [])
        return ok, ("Auto vs Own picture: mean difference %.5f, %d pixels differ by more than 2%% "
                    "(of %d cube pixels), largest %.3f; grouped at start %r, after the modifier "
                    "action %r") % (
            diff["mean"], diff["pixels_over_2_percent"], diff["cube_pixels"], diff["max"],
            values["out_of_graph_start"], values["out_of_graph_end"])


@case("merged_objects", "check",
      "Game Drawing Merged: a row of cubes is drawn by one merged Blender object; removing one "
      "and hiding one collapse their parts, showing it brings it back, moving one gives it its own "
      "object again; after the game the merged object is gone and the cubes are as before.",
      timeout=180)
class MergedObjects:
    def build(settings):
        white = emission_material("RigWhite")
        for index in range(6):
            # All in one 64 m cell (cells split at x = 0): one batch.
            ob = new_object("Wall%d" % index, box_mesh("WallMesh%d" % index),
                            (index * 3.0 + 1.5, 0.0, 0.0), white)
            ob.game.draw_as = 'MERGED'
        game_settings()
        return add_camera("Cam", (9.0, -20.0, 1.0), (90.0, 0.0, 0.0), ortho_scale=20.0)

    logic = r'''
def step(tick):
    import bpy
    objects = scene().objects
    if tick == 30:
        shot("start")
        save(batches=len([o for o in bpy.data.objects if o.name.startswith("Merged Scenery")]),
             out_of_graph=len([o for o in bpy.data.objects
                               if o.name.startswith("Wall") and o.hide_viewport]))
    elif tick == 40:
        objects["Wall1"].endObject()
        objects["Wall2"].visible = False
        objects["Wall4"].applyMovement((0.0, 0.0, 2.0), False)
    elif tick == 100:
        shot("mid")
    elif tick == 110:
        objects["Wall2"].visible = True
    elif tick == 170:
        shot("end")
    elif tick == 176:
        finish()
'''

    def after(settings, out):
        bpy = bpy_module()
        rows = {}
        for name in ("start", "mid", "end"):
            path = os.path.join(out, name + ".png")
            # Rows through z = 0 and z = 2 (camera at z = 1, 11.25 m tall view).
            rows[name] = [len(bright_runs_at_height(path, f)) for f in (0.59, 0.41)]
        rows["after_game_batches"] = len(
            [o for o in bpy.data.objects if o.name.startswith("Merged Scenery")]) + len(
            [m for m in bpy.data.meshes if m.name.startswith("Merged Scenery")])
        rows["after_game_out_of_graph"] = len(
            [o for o in bpy.data.objects if o.name.startswith("Wall") and o.hide_viewport])
        return rows

    def judge(settings, values, log_text):
        ok = (values["batches"] == 1 and values["out_of_graph"] == 6 and
              values["start"] == [6, 0] and values["mid"] == [3, 1] and
              values["end"] == [4, 1] and values["after_game_batches"] == 0 and
              values["after_game_out_of_graph"] == 0)
        return ok, ("%d merged object, %d walls out of the graph; walls on the ground/raised "
                    "row: start %r, after remove+hide+move %r, after show %r; after the game "
                    "%d merged data left, %d walls hidden") % (
            values["batches"], values["out_of_graph"], values["start"], values["mid"],
            values["end"], values["after_game_batches"], values["after_game_out_of_graph"])


@case("copy_modifier_action", "check",
      "A modifier action played on one ordinary copy changes only that copy (copies share the "
      "mesh, not their modifiers).")
class CopyModifierAction:
    def build(settings):
        bpy = bpy_module()
        ob = new_object("Bar", box_mesh("BarMesh", (1.2, 1.2, 2.0), True), (0.0, 0.0, -20.0),
                        emission_material("RigWhite"))
        modifier = ob.modifiers.new("Grow", 'DISPLACE')
        modifier.direction = 'Z'
        modifier.strength = 0.0
        modifier.keyframe_insert("strength", frame=1)
        modifier.strength = 2.0
        modifier.keyframe_insert("strength", frame=2)
        modifier.strength = 0.0
        ob.animation_data.action.name = "BarGrow"
        ob.animation_data.action = None
        ob.hide_set(True)
        game_settings()
        return add_camera("Cam", (0.0, -20.0, 1.5), (90.0, 0.0, 0.0), ortho_scale=10.0)

    logic = r'''
def step(tick):
    if tick == 5:
        copies = []
        for index in range(3):
            copy = scene().addObject("Bar", scene().objects["Cam"], 0)
            copy.worldPosition = (index * 3.0 - 3.0, 0.0, 0.0)
            upright(copy)
            copies.append(copy)
        store["copies"] = copies
    elif tick == 30:
        shot("before")
    elif tick == 40:
        store["copies"][1].playAction("BarGrow", 1, 2, play_mode=bge.logic.KX_ACTION_MODE_PLAY)
    elif tick == 150:
        shot("after")
    elif tick == 156:
        finish()
'''

    def after(settings, out):
        # A row the bars cross before and after the middle one rises.
        before = column_tops(load_picture(os.path.join(out, "before.png")), 0.45, 3)
        after = column_tops(load_picture(os.path.join(out, "after.png")), 0.45, 3)
        return {"rises": [b["top"] - a["top"] for a, b in zip(after, before)],
                "width": before[0]["width"]}

    def judge(settings, values, log_text):
        rises = values["rises"]
        # Displace along Z without a texture moves by (1 - midlevel 0.5) x strength 2 = 1 m.
        expected = values["width"] / 1.2
        ok = rises[1] > expected * 0.6 and abs(rises[0]) <= 1 and abs(rises[2]) <= 1
        return ok, "top edges rose: left %d, middle %d, right %d px (middle about %d)" % (
            rises[0], rises[1], rises[2], expected)


@case("draw_culling", "check",
      "Draw culling (KX_DrawCulling): with it on, the picture matches the one with it off "
      "(UPBGE_DRAW_CULLING=0) except two cubes: one beyond its collection's draw_distance, one "
      "given a draw_distance game property while playing. Cubes and a tower beyond the camera's "
      "Clip End stay out of sight, and the sun casts the tower's shadow toward the camera, into "
      "the view: culling must keep it. Then a third cube gets a draw distance while nothing else "
      "changes (a renderer that finished its samples): it must vanish with its shadow.",
      settings={"DRAW": "AUTO"}, timeout=200)
class DrawCulling:
    variants = [{"ENV_UPBGE_DRAW_CULLING": "0"}, {"ENV_UPBGE_DRAW_CULLING": "1"}]

    def build(settings):
        bpy = bpy_module()
        white = diffuse_material("RigWhite", (0.9, 0.9, 0.9))
        new_object("Floor", box_mesh("FloorMesh", (80.0, 200.0, 0.2)), (0.0, 90.0, -0.1),
                   white)
        cube = box_mesh("CrateMesh")
        cube.materials.append(white)
        cubes = [new_object("Near", cube, (-2.0, 8.0, 0.5)),
                 new_object("Middle", cube, (0.0, 14.0, 0.5)),
                 new_object("Late", cube, (-3.5, 16.0, 0.5))]
        props = bpy.data.collections.new("Props")
        bpy.context.scene.collection.children.link(props)
        props["draw_distance"] = 20.0
        cubes.append(new_object("Prop", cube, (4.0, 24.0, 0.5), collection=props))
        # Beyond Clip End (40 m): a tower whose shadow reaches the view, and cubes whose shadows
        # cannot.
        new_object("Tower", box_mesh("TowerMesh", (2.0, 2.0, 30.0), base_at_zero=True),
                   (0.0, 70.0, 0.0), white)
        for index in range(6):
            cubes.append(new_object("Far%d" % index, cube, (index * 4.0 - 10.0, 110.0, 0.5)))
        for ob in cubes:
            ob.game.draw_as = settings["DRAW"]
        game_settings()
        # Light traveling toward the camera, 30 degrees above the horizon; sharp shadows (a soft
        # shadow's sampling noise differs from run to run).
        add_sun((-60.0, 0.0, 0.0)).data.angle = 0.0
        camera = add_camera("Cam", (0.0, 0.0, 3.0), (80.0, 0.0, 0.0))
        camera.data.clip_end = 40.0
        return camera

    logic = r'''
def step(tick):
    objects = scene().objects
    if tick == 60:
        objects["Near"]["draw_distance"] = 4.0
        # An update in both pictures at the same step: a renderer restarting its samples ends
        # with slightly different edges, which the comparison must not count.
        objects["Middle"].color = (1.0, 1.0, 1.0, 0.999)
    elif tick == 150:
        shot("look")
    elif tick == 160:
        objects["Late"]["draw_distance"] = 4.0
    elif tick == 230:
        shot("late")
    elif tick == 236:
        finish()
'''

    def after(settings, out):
        import numpy
        if settings["ENV_UPBGE_DRAW_CULLING"] != "1":
            return {}
        on = load_picture(os.path.join(out, "look.png"))
        off = load_picture(os.path.join(os.path.dirname(out), "0", "look.png"))
        if on.shape != off.shape:
            return {"diff": None}
        changed = numpy.abs(on - off).max(axis=2) > 0.02
        width = on.shape[1]
        thirds = [changed[:, :width // 3], changed[:, width // 3:2 * width // 3],
                  changed[:, 2 * width // 3:]]
        lum = luminance(off)[:, width // 3:2 * width // 3]
        # The left third's floor rows (its left edge shows floor there) after Late's change: with
        # culling no cube or shadow is darker than the floor.
        late_on = luminance(load_picture(os.path.join(out, "late.png")))[:, :width // 3]
        late_off = luminance(load_picture(
            os.path.join(os.path.dirname(out), "0", "late.png")))[:, :width // 3]
        floor_rows = late_off[:, 2] > 0.3
        # Not the far plane's edge (the floor's first rows, drawn half over the sky).
        floor_rows[numpy.nonzero(floor_rows)[0][:3]] = False
        floor_level = float(numpy.median(late_off[floor_rows, 2])) if floor_rows.any() else 1.0
        return {"diff": [int(part.sum()) for part in thirds],
                "middle_lit": int((lum > 0.05).sum()),
                "late_dark": [int((late_on[floor_rows] < floor_level * 0.8).sum()),
                              int((late_off[floor_rows] < floor_level * 0.8).sum())]}

    def judge(settings, values, log_text):
        diff = values.get("diff")
        if diff is None:
            return False, "the two pictures could not be compared"
        left, middle, right = diff
        late_on, late_off = values["late_dark"]
        ok = middle == 0 and left > 200 and right > 50 and late_on < 20 and late_off > 1000
        return ok, ("pixels changed by culling, left (Near: own draw distance) / middle (Middle, "
                    "the tower and its shadow) / right (Prop: collection draw distance): %d / %d / "
                    "%d (middle must be 0, %d lit pixels there); after Late's draw distance in a "
                    "still picture, dark floor pixels on the left: %d with culling, %d without") % (
            left, middle, right, values.get("middle_lit", 0), late_on, late_off)


# --------------------------------------------------------------------------------------------
# Benchmarks.

def cube_grid(count, prefix, mesh, draw_as='OWN'):
    """COUNT placed cubes sharing MESH, drawn as DRAW_AS (Own by default: the benchmarks measure
    objects drawn by their own Blender objects unless they say otherwise)."""
    side = max(1, int(count ** 0.5 + 0.999))
    for index in range(count):
        ob = new_object("%s%05d" % (prefix, index), mesh,
                        (((index % side) - side / 2) * 1.2, ((index // side) - side / 2) * 1.2,
                         0.2))
        ob.game.draw_as = draw_as
    return side


def grid_camera(side):
    camera = add_camera("Cam", (0.0, -side * 1.0, side * 0.8), (48.7, 0.0, 0.0))
    add_sun((40.0, 0.0, 30.0))
    game_settings()
    return camera


PERF_COMMON = r'''
def movement(tick):
    return 0.01 if (tick // 60) % 2 == 0 else -0.01
'''


@case("perf_movers", "perf",
      "MOVERS of COUNT cubes move every tick (0 = all). MODE placed: placed cubes sharing one "
      "mesh, each drawn by its own object; auto: the same drawn as Auto (one group); dupli: "
      "copies of a Dupli Base added at the start; merged: placed cubes drawn as Merged and one "
      "more cube moving.",
      {"COUNT": 10000, "MOVERS": 1, "MODE": "placed"}, timeout=300)
class PerfMovers:
    def build(settings):
        mesh = box_mesh("CubeMesh", (0.4, 0.4, 0.4))
        side = max(1, int(settings["COUNT"] ** 0.5 + 0.999))
        if settings["MODE"] == "dupli":
            base = new_object("Base", mesh, (0.0, 0.0, -50.0))
            base.game.use_upbge_dupli_base = True
            base.hide_set(True)
        else:
            draw_as = {"placed": 'OWN', "auto": 'AUTO', "merged": 'MERGED'}[settings["MODE"]]
            cube_grid(settings["COUNT"], "Cube", mesh, draw_as)
            if settings["MODE"] == "merged":
                new_object("Mover", box_mesh("MoverMesh", (0.2, 0.2, 0.2)), (0.0, 0.0, 1.0))
        return grid_camera(side)

    logic = PERF_COMMON + r'''
def step(tick):
    if tick == 1:
        count = S["COUNT"]
        side = max(1, int(count ** 0.5 + 0.999))
        if S["MODE"] == "dupli":
            for index in range(count):
                ob = scene().addObject("Base", scene().objects["Cam"], 0)
                ob.worldPosition = (((index % side) - side / 2) * 1.2,
                                    ((index // side) - side / 2) * 1.2, 0.2)
                upright(ob)
        movers = [o for o in scene().objects if o.name.startswith(("Base", "Cube"))]
        if S["MODE"] == "merged":
            movers = [scene().objects["Mover"]]
        store["movers"] = movers[:S["MOVERS"]] if S["MOVERS"] > 0 else movers
    step_x = movement(tick)
    for ob in store["movers"]:
        ob.applyMovement((step_x, 0.0, 0.0), False)
    profile_frame()
'''


@case("perf_promote", "perf",
      "AUTO grouped cubes (Draw As Auto) beside OWN cubes drawn by their own objects; from the "
      "first second on, every EVERY ticks one more grouped cube plays a modifier action and gets "
      "its own Blender object back (one graph rebuild). The engine's profile averages frames, so "
      "the hitch itself is read from the log (temporary per-pass timing).",
      {"AUTO": 1000, "OWN": 0, "EVERY": 30}, timeout=300)
class PerfPromote:
    def build(settings):
        bpy = bpy_module()
        side = cube_grid(settings["AUTO"], "Auto", box_mesh("AutoMesh", (0.4, 0.4, 0.4)), 'AUTO')
        for ob in bpy.data.objects:
            if ob.name.startswith("Auto"):
                ob.modifiers.new("Grow", 'DISPLACE').strength = 0.0
        first = bpy.data.objects["Auto00000"]
        modifier = first.modifiers["Grow"]
        modifier.keyframe_insert("strength", frame=1)
        modifier.strength = 0.3
        modifier.keyframe_insert("strength", frame=2)
        modifier.strength = 0.0
        first.animation_data.action.name = "Grow"
        first.animation_data.action = None
        if settings["OWN"] > 0:
            own_side = cube_grid(settings["OWN"], "Own", box_mesh("OwnMesh", (0.4, 0.4, 0.4)))
            for ob in bpy.data.objects:
                if ob.name.startswith("Own"):
                    ob.location.y += side * 1.2 + 2.0
            side = max(side, own_side) * 2
        return grid_camera(side)

    logic = r'''
def step(tick):
    if tick == 1:
        store["grouped"] = [o for o in scene().objects if o.name.startswith("Auto")]
        store["next"] = 0
    if tick >= 60 and tick % S["EVERY"] == 0 and store["next"] < len(store["grouped"]):
        print("RIG_PROMOTE tick %d" % tick, flush=True)
        store["grouped"][store["next"]].playAction(
            "Grow", 1, 2, play_mode=bge.logic.KX_ACTION_MODE_PLAY)
        store["next"] += 1
    profile_frame()
'''


@case("perf_merged", "perf",
      "COUNT cubes joined into one mesh object plus one small moving cube; DELETE=1 collapses one "
      "more cube of the joined mesh every tick (a geometry change).",
      {"COUNT": 10000, "DELETE": 0}, timeout=300)
class PerfMerged:
    def build(settings):
        bpy = bpy_module()
        count = settings["COUNT"]
        side = max(1, int(count ** 0.5 + 0.999))
        cube = box_mesh("Tmp", (0.4, 0.4, 0.4))
        corners = [v.co.copy() for v in cube.vertices]
        faces = [list(p.vertices) for p in cube.polygons]
        bpy.data.meshes.remove(cube)
        verts, all_faces = [], []
        for index in range(count):
            x = ((index % side) - side / 2) * 1.2
            y = ((index // side) - side / 2) * 1.2
            offset = len(verts)
            verts.extend((c.x + x, c.y + y, c.z + 0.2) for c in corners)
            all_faces.extend([i + offset for i in face] for face in faces)
        mesh = bpy.data.meshes.new("ChunkMesh")
        mesh.from_pydata(verts, [], all_faces)
        mesh.update()
        new_object("Chunk", mesh)
        new_object("Mover", box_mesh("MoverMesh", (0.2, 0.2, 0.2)), (0.0, 0.0, 1.0))
        return grid_camera(side)

    logic = PERF_COMMON + r'''
def step(tick):
    scene().objects["Mover"].applyMovement((movement(tick), 0.0, 0.0), False)
    if S["DELETE"]:
        import bpy
        mesh = bpy.data.meshes["ChunkMesh"]
        first = (tick % S["COUNT"]) * 8
        center = mesh.vertices[first].co.copy()
        for index in range(first, first + 8):
            mesh.vertices[index].co = center
        mesh.update()
    profile_frame()
'''


@case("perf_spawn", "perf",
      "Among COUNT still placed cubes, RATE copies a tick are added living LIFE ticks. SPAWN "
      "copy: ordinary copies; dupli: Dupli Base copies; full: Full Copies.",
      {"COUNT": 1000, "SPAWN": "copy", "RATE": 2, "LIFE": 30}, timeout=300)
class PerfSpawn:
    def build(settings):
        side = cube_grid(settings["COUNT"], "Cube", box_mesh("CubeMesh", (0.4, 0.4, 0.4)))
        bullet = new_object("Bullet", box_mesh("BulletMesh", (0.2, 0.2, 0.2)), (0.0, 0.0, -50.0))
        bullet.game.use_upbge_dupli_base = settings["SPAWN"] == "dupli"
        bullet.hide_set(True)
        return grid_camera(side)

    logic = r'''
def step(tick):
    full = 1 if S["SPAWN"] == "full" else 0
    for index in range(S["RATE"]):
        ob = scene().addObject("Bullet", scene().objects["Cam"], S["LIFE"], full)
        ob.worldPosition = ((tick % 20) - 10.0, index * 0.5, 1.0)
        upright(ob)
    profile_frame()
'''


@case("perf_hide", "perf",
      "Among COUNT placed cubes, TOGGLES a tick are hidden (Set Visible) and shown again "
      "HIDE_TICKS ticks later.",
      {"COUNT": 3000, "TOGGLES": 2, "HIDE_TICKS": 30}, timeout=300)
class PerfHide:
    def build(settings):
        side = cube_grid(settings["COUNT"], "Cube", box_mesh("CubeMesh", (0.4, 0.4, 0.4)))
        return grid_camera(side)

    logic = r'''
def step(tick):
    if tick == 1:
        store["cubes"] = [o for o in scene().objects if o.name.startswith("Cube")]
    cubes = store["cubes"]
    for index in range(S["TOGGLES"]):
        cubes[(tick * S["TOGGLES"] + index) % len(cubes)].visible = False
        if tick > S["HIDE_TICKS"]:
            back = (tick - S["HIDE_TICKS"]) * S["TOGGLES"] + index
            cubes[back % len(cubes)].visible = True
    profile_frame()
'''


# --------------------------------------------------------------------------------------------
# Inside Blender: build, play, measure, write result.json, quit.

def blender_main(argv):
    import traceback
    bpy = bpy_module()
    args = dict(zip(argv[::2], argv[1::2]))
    entry = CASES[args["--case"]]
    out = args["--out"]
    settings = json.loads(args["--settings"])
    os.makedirs(out, exist_ok=True)

    def run():
        result = {"values": {}}
        try:
            clear_scene()
            # The case's own folder, for files it writes (a save game).
            camera = entry.build(dict(settings, OUT=out))
            module = "rig_logic"
            text = GAME_PRELUDE.replace("__OUT__", repr(out)).replace(
                "__SETTINGS__", repr(settings)).replace(
                "__DATA_KINDS__", repr(DATA_KINDS)) + entry.logic
            bpy.data.texts.new(module + ".py").write(text)
            attach_logic(camera, module)
            if entry.before_game is not None:
                result["values"].update(entry.before_game(settings))
            play_game(camera)
            game_file = os.path.join(out, "game.json")
            if os.path.exists(game_file):
                with open(game_file) as handle:
                    result["values"].update(json.load(handle))
                result["game_finished"] = True
            if entry.after is not None and result.get("game_finished"):
                result["values"].update(entry.after(settings, out))
        except Exception:
            result["error"] = traceback.format_exc()
        with open(os.path.join(out, "result.json"), "w") as handle:
            json.dump(result, handle, indent=1)
        print("RIG_DONE", flush=True)
        bpy.app.timers.register(lambda: bpy.ops.wm.quit_blender() and None, first_interval=0.2)
        return None

    bpy.app.timers.register(run, first_interval=1.5)


# --------------------------------------------------------------------------------------------
# Runner (system Python): one Blender per case.

def parse_value(text):
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def runner_main(argv):
    import argparse
    import subprocess
    import tempfile
    repo = os.path.dirname(os.path.dirname(os.path.dirname(RIG_FILE)))
    parser = argparse.ArgumentParser(description="UPBGE render-sync test rig")
    parser.add_argument("cases", nargs="*", help="case names, 'checks' or 'perf'")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--blender", default=os.path.join(repo, "..", "build_linux", "bin",
                                                          "blender"))
    parser.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "upbge_render_sync"))
    parser.add_argument("--opengl", action="store_true")
    parser.add_argument("--set", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args(argv)

    if args.list or not args.cases:
        for entry in CASES.values():
            settings = " ".join("%s=%s" % item for item in entry.settings.items())
            print("%-26s %-5s %s%s" % (entry.name, entry.kind, entry.doc,
                                       " [%s]" % settings if settings else ""))
        return 0

    names = []
    for name in args.cases:
        if name in ("checks", "perf"):
            names.extend(e.name for e in CASES.values() if e.kind == name.rstrip("s"))
        elif name in CASES:
            names.append(name)
        else:
            print("unknown case %r (see --list)" % name)
            return 2
    overrides = dict(item.split("=", 1) for item in args.set)

    def play(entry, settings, out):
        """One Blender playing the case into OUT: (result or None, exit code, log text, log)."""
        os.makedirs(out, exist_ok=True)
        for old in os.listdir(out):
            path = os.path.join(out, old)
            if os.path.isfile(path):
                os.remove(path)
        command = [args.blender, "--factory-startup", "--gpu-backend",
                   "opengl" if args.opengl else "vulkan"]
        if entry.blank_file:
            blank = os.path.abspath(os.path.join(args.out, "blank.blend"))
            if not os.path.exists(blank):
                subprocess.run([args.blender, "--background", "--factory-startup",
                                "--python-expr",
                                "import bpy; bpy.ops.wm.save_as_mainfile(filepath=%r)" % blank],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
            command.append(blank)
        command += ["--python", RIG_FILE, "--", "--case", entry.name, "--out", out,
                    "--settings", json.dumps(settings)]
        log_path = out + ".log"
        # A setting named ENV_X sets the environment variable X for this Blender.
        env = dict(os.environ)
        env.update({key[4:]: str(value) for key, value in settings.items()
                    if key.startswith("ENV_")})
        with open(log_path, "w") as log:
            try:
                code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                      timeout=entry.timeout, env=env).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        with open(log_path, errors="replace") as log:
            log_text = log.read()
        result_path = os.path.join(out, "result.json")
        result = None
        if os.path.exists(result_path):
            with open(result_path) as handle:
                result = json.load(handle)
        return result, code, log_text, log_path

    failures = 0
    for name in names:
        entry = CASES[name]
        settings = dict(entry.settings)
        settings.update({k: parse_value(v) for k, v in overrides.items() if k in settings})
        for run_index in range(args.repeat):
            out = os.path.abspath(os.path.join(args.out, name if args.repeat == 1 else
                                               "%s_%d" % (name, run_index + 1)))
            label = name if args.repeat == 1 else "%s #%d" % (name, run_index + 1)
            run_settings = settings
            failed = False
            for variant in entry.variants or [None]:
                run_settings = dict(settings)
                run_out = out
                if variant is not None:
                    run_settings.update(variant)
                    run_out = os.path.join(out, "_".join(str(v) for v in variant.values()))
                result, code, log_text, log_path = play(entry, run_settings, run_out)
                if result is None or result.get("error") or not result.get("game_finished"):
                    reason = (result or {}).get("error") or "no result (Blender exit %s)" % code
                    print("FAIL  %s: %s (log %s)" % (label, reason.strip().splitlines()[-1],
                                                     log_path))
                    failed = True
                    break
                if code != 0:
                    print("FAIL  %s: Blender exit %s after the game (log %s)" % (
                        label, code, log_path))
                    failed = True
                    break
            if failed:
                failures += 1
                continue
            settings_shown = run_settings
            values = result["values"]
            if entry.kind == "perf":
                profile = values.get("profile", {})
                parts = ["%s %.2f" % (k, v) for k, v in sorted(profile.items()) if v >= 0.01]
                print("PERF  %s %s: %d objects, %s ms (%d frames)" % (
                    label, " ".join("%s=%s" % i for i in settings_shown.items()),
                    values.get("objects", 0), ", ".join(parts), values.get("profile_frames", 0)))
                continue
            passed, summary = entry.judge(settings_shown, values, log_text)
            failures += 0 if passed else 1
            print("%s  %s: %s" % ("PASS" if passed else "FAIL", label, summary))
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        import bpy  # noqa: F401
        INSIDE_BLENDER = True
    except ImportError:
        INSIDE_BLENDER = False
    if INSIDE_BLENDER:
        blender_main(sys.argv[sys.argv.index("--") + 1:])
    else:
        sys.exit(runner_main(sys.argv[1:]))
