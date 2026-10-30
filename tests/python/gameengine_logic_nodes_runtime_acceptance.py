# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import json
import math
import mathutils
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import addon_utils
import bpy


LOGIC_NODES_TREE_PROPERTY = "__ln_logic_tree"
LOGIC_NODES_ENABLED_PROPERTY = "__ln_logic_enabled"
LOGIC_NODES_STATUS_PROPERTY = "__ln_logic_status"
STATUS_FILENAME = "logic_nodes_runtime_acceptance.jsonl"
CRASH_FILENAME = "blender.crash.txt"
PREFIX = "LN_Acceptance"
RUNNER = "view3d"
PLAYER_OUTPUT_DIR = None
# What the last blenderplayer run printed (the player runner), for probes that read its log.
LAST_PLAYER_OUTPUT = ""
PHASE10_SERVICE_BLEND_FIXTURES = (
    (
        "ln_phase10_physics_world.blend",
        "fixture.phase10.physics_world.blend",
        "physics_world",
        "physics world rigid body/contact boundary",
        11,
    ),
    (
        "ln_phase10_character_controller.blend",
        "fixture.phase10.character_controller.blend",
        "character_controller",
        "character controller movement/on-ground boundary",
        11,
    ),
    (
        "ln_phase10_audio_device.blend",
        "fixture.phase10.audio_device.blend",
        "audio_device",
        "audio device playback boundary",
        9,
    ),
    (
        "ln_phase10_material_datablock.blend",
        "fixture.phase10.material_datablock.blend",
        "material_datablock",
        "material/datablock mutation boundary",
        9,
    ),
    (
        "ln_phase10_scene_lifecycle.blend",
        "fixture.phase10.scene_lifecycle.blend",
        "scene_lifecycle",
        "scene and object lifecycle boundary",
        25,
    ),
    (
        "ln_phase10_player_restart_load.blend",
        "fixture.phase10.player_restart_load.blend",
        "player_restart_load",
        "blenderplayer restart/load boundary",
        9,
    ),
)

VALUE_OUTPUT_KEYS = ("Bool", "Int", "Float", "String", "Vector", "Value")


def value_output(node):
    for key in VALUE_OUTPUT_KEYS:
        if key in node.outputs:
            return node.outputs[key]
    raise KeyError(f"{node.bl_idname} has no value output socket")


def new_get_object_attribute(tree, attribute_type):
    node = tree.nodes.new("LogicNativeGetObjectAttribute")
    node.attribute_type = attribute_type
    return node


def new_set_object_attribute(tree, attribute_type):
    node = tree.nodes.new("LogicNativeSetObjectAttribute")
    node.attribute_type = attribute_type
    return node


def flow_input(node):
    if "Flow" in node.inputs:
        return node.inputs["Flow"]
    raise KeyError(f"{node.bl_idname} has no Flow input socket")


GAME_CONTROLLER_SCRIPT = r'''
import json
import math
import os
import time
import traceback

import bge


def vector_tuple(value):
    return tuple(float(component) for component in value)


def drive_camera_renderers(owner):
    """Camera renderers: two view cameras that may share one renderer and a Use Workbench one
    draw for a second, stop drawing for 6 seconds (renderers idle for 5 are given back), then
    draw again. True once done, or after a time limit."""
    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    start = store.setdefault("cr_start", time.monotonic())
    elapsed = time.monotonic() - start
    names = [str(owner.get("__ln_acceptance_cr_" + key, "")) for key in ("a", "b", "w")]
    cameras = [scene.objects.get(name) for name in names]
    if any(camera is None for camera in cameras):
        owner["cr_error"] = "missing camera"
        return True
    phase = int(owner.get("cr_phase", 0))
    if phase == 0:
        width = bge.render.getWindowWidth()
        height = bge.render.getWindowHeight()
        view_w = max(16, width // 4)
        view_h = max(16, height // 4)
        cameras[0].setViewport(0, height - view_h, view_w, height)
        cameras[1].setViewport(width - view_w, height - view_h, width, height)
        cameras[2].setViewport(0, 0, view_w, view_h)
        for camera in cameras:
            camera.useViewport = True
        owner["cr_phase"] = 1
    elif phase == 1:
        owner["cr_frames_on"] = int(owner.get("cr_frames_on", 0)) + 1
        if elapsed >= 1.0:
            for camera in cameras:
                camera.useViewport = False
            owner["cr_phase"] = 2
    elif phase == 2:
        owner["cr_frames_off"] = int(owner.get("cr_frames_off", 0)) + 1
        if elapsed >= 7.0:
            for camera in cameras:
                camera.useViewport = True
            store["cr_back"] = time.monotonic()
            owner["cr_phase"] = 3
    elif phase == 3:
        owner["cr_frames_back"] = int(owner.get("cr_frames_back", 0)) + 1
        if time.monotonic() - store["cr_back"] >= 0.8 and owner["cr_frames_back"] >= 5:
            owner["cr_phase"] = 4
    return int(owner.get("cr_phase", 0)) == 4 or elapsed >= 13.0


SM_KEYS = ("mover", "child", "ball", "plain", "teleport", "node")


def sm_objects(scene, owner):
    return {key: scene.objects.get(str(owner.get("__ln_acceptance_sm_" + key, "")))
            for key in SM_KEYS}


def sm_record():
    """Smooth Motion, after each drawn frame: where each object was drawn (worldPosition holds
    the drawn pose until the frame is over) and where it is (localPosition, a root's own)."""
    store = bge.logic.globalDict
    if store.get("sm_stop", False):
        return
    scene = bge.logic.getCurrentScene()
    owner = scene.objects.get(str(store.get("sm_owner", "")))
    if owner is None:
        return
    row = {"t": time.monotonic() - store["sm_start"],
           "a": float(bge.logic.getFramePacingInfo()["interpolation"])}
    for key, obj in sm_objects(scene, owner).items():
        if obj is not None:
            row[key] = (float(obj.worldPosition[0]), float(obj.worldPosition[1]),
                        float(obj.localPosition[0]))
    store["sm_frames"].append(row)


def drive_smooth_motion(owner):
    """Smooth Motion: record the drawn frames for 2 seconds; at 1.0 s Python teleports one object
    (resetSmoothing), at 1.3 s another jumps by Set Object Attribute's Teleport. True once
    done."""
    store = bge.logic.globalDict
    scene = bge.logic.getCurrentScene()
    if "sm_start" not in store:
        store["sm_start"] = time.monotonic()
        store["sm_frames"] = []
        store["sm_owner"] = owner.name
        scene.post_draw.append(sm_record)
    elapsed = time.monotonic() - store["sm_start"]
    objects = sm_objects(scene, owner)
    teleport = objects["teleport"]
    if elapsed >= 1.0 and "sm_teleport_from" not in store and teleport is not None:
        position = teleport.worldPosition.copy()
        store["sm_teleport_from"] = float(position[0])
        position[0] += 50.0
        teleport.worldPosition = position
        teleport.resetSmoothing()
    node = objects["node"]
    if elapsed >= 1.3 and "sm_node_from" not in store and node is not None:
        store["sm_node_from"] = float(node.worldPosition[0])
        node["node_jump"] = True
    if elapsed >= 2.2:
        store["sm_stop"] = True
        return True
    return False


def fp_record():
    """Frame pacing, after each drawn frame (also paused ones): the steps it showed, and the
    pause schedule: 30 frames running, 15 paused, 8 times."""
    store = bge.logic.globalDict
    elapsed = time.monotonic() - store.setdefault("fp_start", time.monotonic())
    if elapsed > 30.0:
        # Never resumed or never checked: end the game rather than hang the suite (no status
        # record is a failure).
        bge.logic.endGame()
        return
    if store.get("fp_stop", False):
        return
    scene = bge.logic.getCurrentScene()
    owner = scene.objects.get(str(store.get("fp_owner", "")))
    if owner is None:
        return
    info = bge.logic.getFramePacingInfo()
    frame = int(store.get("fp_frame", 0))
    store["fp_frame"] = frame + 1
    phase = frame % 45
    store["fp_rows"].append((frame, phase, int(info["frameSteps"]), str(info["pacing"])))
    owner["want_pause"] = phase >= 30
    if frame >= 45 * 8 or elapsed > 20.0:
        owner["want_pause"] = False
        store["fp_stop"] = True


def drive_frame_pacing(owner):
    store = bge.logic.globalDict
    if "fp_rows" not in store:
        store["fp_rows"] = []
        store["fp_owner"] = owner.name
        bge.logic.getCurrentScene().post_draw.append(fp_record)
    return bool(store.get("fp_stop", False))


MO_TILES = ("only", "hide", "material", "plain", "outline", "cavity", "round", "screen",
            "studio", "matcap", "attribute", "random")


def mo_classify(rgb):
    """Color names of an (N, 3) array of 0..255 pixels."""
    import numpy as np

    r, g, b = (rgb[:, channel].astype(np.int32) for channel in range(3))
    high = np.maximum(np.maximum(r, g), b)
    low = np.minimum(np.minimum(r, g), b)
    names = np.full(len(rgb), "other", dtype=object)
    # Later rules win.
    for name, mask in (
        ("magenta", (r > g + 40) & (b > g + 40) & (np.abs(r - b) < 60)),
        ("yellow", (r > b + 40) & (g > b + 40) & (np.abs(r - g) < 60)),
        ("cyan", (g > r + 40) & (b > r + 40) & (np.abs(g - b) < 60)),
        ("blue", (b > r + 40) & (b > g + 40)),
        ("red", (r > g + 40) & (r > b + 40)),
        ("green", (g > r + 40) & (g > b + 40)),
        ("gray", high - low < 25),
        ("black", high < 40),
    ):
        names[mask] = name
    return names


def drive_camera_minimap_options(owner):
    """Minimap options: spawn a red marker copy (Add Object), place the small views over the
    main view (MO_TILES), take a screenshot a few frames later and read each view's
    pixels. True once done."""
    import bpy
    import numpy as np

    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    phase = int(owner.get("mo_phase", 0))
    if phase == 0:
        spawned = scene.addObject(str(owner.get("__ln_acceptance_mo_spawn", "")), owner)
        spawned.worldPosition = (2.0, 0.0, 0.5)
        spawned.color = (1.0, 0.0, 0.0, 1.0)
        width = bge.render.getWindowWidth()
        height = bge.render.getWindowHeight()
        cell_w, cell_h = width // 4, height // 3
        rects = {}
        for index, key in enumerate(MO_TILES):
            camera = scene.objects.get(str(owner.get("__ln_acceptance_mo_" + key, "")))
            if camera is None:
                owner["mo_error"] = "missing camera " + key
                return True
            left = (index % 4) * cell_w + 2
            bottom = (index // 4) * cell_h + 2
            tile_w, tile_h = cell_w - 5, cell_h - 5
            if key == "round":
                tile_w = tile_h = min(tile_w, tile_h)
            camera.setViewport(left, bottom, left + tile_w, bottom + tile_h)
            camera.useViewport = True
            camera.setOnTop()
            rects[key] = (left, bottom, left + tile_w, bottom + tile_h)
        store["mo_rects"] = rects
        owner["mo_phase"] = 1
        return False
    status_path = str(owner.get("__ln_acceptance_status_path", ""))
    directory = os.path.dirname(status_path)
    if phase == 1:
        owner["mo_frames"] = int(owner.get("mo_frames", 0)) + 1
        if owner["mo_frames"] >= 6:
            bge.render.makeScreenshot(os.path.join(directory, "mo_shot.png"))
            owner["mo_phase"] = 2
        return False
    if phase == 2:
        owner["mo_frames"] = int(owner.get("mo_frames", 0)) + 1
        shots = sorted(name for name in os.listdir(directory) if name.startswith("mo_shot"))
        path = os.path.join(directory, shots[-1]) if shots else ""
        size = os.path.getsize(path) if path else 0
        now = time.monotonic()
        store.setdefault("mo_shot_start", now)
        # Before the 8 s quit of add_status_controller's required_seconds.
        if now - store["mo_shot_start"] > 6.0:
            owner["mo_error"] = "no readable screenshot"
            return True
        # Written by a task, and a player frame can take a millisecond: read the file once its
        # size has not changed for 0.3 s, and try again if it can't be read yet.
        if size == 0 or size != store.get("mo_shot_size"):
            store["mo_shot_size"] = size
            store["mo_shot_changed"] = now
            return False
        if now - store["mo_shot_changed"] < 0.3:
            return False
        image = None
        try:
            image = bpy.data.images.load(path)
            width, height = image.size
            pixels = np.empty(width * height * 4, dtype=np.float32)
            image.pixels.foreach_get(pixels)
        except Exception:
            width = height = 0
        finally:
            if image is not None:
                bpy.data.images.remove(image)
        if width == 0 or height == 0:
            store["mo_shot_changed"] = now
            return False
        pixels = (pixels.reshape(height, width, 4)[:, :, :3] * 255.0 + 0.5).astype(np.int32)
        rects = store.get("mo_rects", {})

        def tile(key, inset):
            left, bottom, right, top = rects[key]
            return pixels[bottom + inset:top + 1 - inset, left + inset:right + 1 - inset]

        result = {"size": [width, height], "counts": {}}
        for key in rects:
            values, counts = np.unique(mo_classify(tile(key, 2).reshape(-1, 3)),
                                       return_counts=True)
            result["counts"][key] = {str(v): int(c) for v, c in zip(values, counts)}
        # Same view and size, one switch apart: Cavity changes the sphere's shading, Outline adds
        # magenta (red) to the object edges.
        result["cavity_changed"] = int(np.count_nonzero(
            np.abs(tile("cavity", 2) - tile("only", 2)).max(axis=2) > 8))
        # Screen cavity alone, with the startup file's Screen strengths at 0 (the game uses 1).
        result["screen_changed"] = int(np.count_nonzero(
            np.abs(tile("screen", 2) - tile("only", 2)).max(axis=2) > 8))
        result["outline_redder"] = int(np.count_nonzero(
            tile("outline", 2)[:, :, 0] - tile("plain", 2)[:, :, 0] > 30))
        # Studio lighting shades the markers that Only draws flat.
        result["studio_changed"] = int(np.count_nonzero(
            np.abs(tile("studio", 2) - tile("only", 2)).max(axis=2) > 8))
        # Random colors: the ground (most of the view) is no longer its blue object color.
        random_diff = np.abs(tile("random", 2) - tile("plain", 2)).max(axis=2) > 30
        result["random_changed"] = round(float(np.count_nonzero(random_diff)) /
                                         max(1, random_diff.size), 3)
        # The round view: each corner keeps the main view (as just outside the view), the middle
        # is the minimap (black where the ground is left out), and the pixels on the circle's
        # edge are in between (a smooth edge).
        left, bottom, right, top = rects["round"]
        corner_diffs = []
        corner_sums = []
        for x, outside_x in ((left + 1, left - 2), (right - 1, right + 2)):
            for y in (bottom + 1, top - 1):
                corner_diffs.append(int(np.abs(pixels[y, x] - pixels[y, outside_x]).max()))
                corner_sums.append(int(pixels[y, x].sum()))
        result["round_corner_diffs"] = corner_diffs
        result["round_corner_sums"] = corner_sums
        round_tile = tile("round", 0)
        size_y, size_x = round_tile.shape[:2]
        result["round_middle"] = str(mo_classify(round_tile[size_y // 2:size_y // 2 + 1,
                                                            size_x // 2:size_x // 2 + 1]
                                                 .reshape(-1, 3))[0])
        ys, xs = np.mgrid[0:size_y, 0:size_x]
        radius = size_x / 2.0
        edge = np.abs(np.hypot(xs + 0.5 - radius, ys + 0.5 - radius) - radius) < 0.35
        sums = round_tile.sum(axis=2)
        outside_sum = max(1.0, float(np.mean(corner_sums)))
        between = edge & (sums > 0.15 * outside_sum) & (sums < 0.85 * outside_sum)
        result["round_edge"] = [int(np.count_nonzero(between)), int(np.count_nonzero(edge))]
        store["mo_result"] = result
        owner["mo_phase"] = 3
    return True


CM_FLAGS = ("cm_do_mark", "cm_do_spawn", "cm_do_add", "cm_do_remove", "cm_do_save", "cm_do_undo",
            "cm_do_load", "cm_do_nest", "cm_do_hide", "cm_do_spawn_nest", "cm_do_join",
            "cm_do_end_nest")
CM_STEPS = (
    ("record", "start"), ("do", "cm_do_spawn"), ("record", "spawned"), ("do", "cm_do_add"),
    ("record", "added"), ("do", "cm_do_remove"), ("record", "removed"), ("do", "cm_do_save"),
    ("wait", "cm_save"), ("do", "cm_do_undo"), ("record", "undone"), ("do", "cm_do_load"),
    ("wait", "cm_load"), ("record", "loaded"), ("do", "cm_do_nest"), ("record", "nested"),
    ("do", "cm_do_hide"), ("do", "cm_do_spawn"), ("do", "cm_do_spawn_nest"),
    ("visible", "hidden"), ("do", "cm_do_join"), ("visible", "joined"),
    ("do", "cm_do_end_nest"), ("record", "unnested"),
)


def drive_material_actions(owner):
    """Material actions: after the owner's own (On Init), a copy of the hidden template plays the
    same material action from Python; its material is read once the action is done, then the
    copy is ended and its material copy must be gone. True once done, or after a time limit."""
    import bpy

    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    now = time.monotonic() - store.setdefault("ma_clock", time.monotonic())
    phase = int(owner.get("ma_phase", 0))
    if phase == 0 and now >= 0.6:
        spawned = scene.addObject(str(owner.get("__ln_acceptance_ma_template", "")), owner)
        spawned.worldPosition = (0.0, 3.0, 0.0)
        owner["ma_spawn_before"] = spawned.blenderObject.material_slots[0].material.name
        spawned.playAction(str(owner.get("__ln_acceptance_ma_action", "")), 1.0, 5.0)
        store["ma_spawned"] = spawned
        owner["ma_phase"] = 1
        owner["ma_phase_time"] = now
    elif phase == 1 and now >= float(owner.get("ma_phase_time", 0.0)) + 0.6:
        spawned = store.get("ma_spawned")
        material = spawned.blenderObject.material_slots[0].material
        owner["ma_spawned_material"] = material.name
        owner["ma_spawned_r"] = float(material.diffuse_color[0])
        owner["ma_spawned_g"] = float(material.diffuse_color[1])
        spawned.endObject()
        owner["ma_phase"] = 2
        owner["ma_phase_time"] = now
    elif phase == 2 and now >= float(owner.get("ma_phase_time", 0.0)) + 0.3:
        owner["ma_spawned_copy_gone"] = (
            str(owner.get("ma_spawned_material", "")) not in bpy.data.materials)
        owner["ma_phase"] = 3
        return True
    return now >= 6.0


def drive_camera_visibility_still(owner):
    """Set Camera Visibility in a still scene: the top camera's view is placed in a corner of the
    window, screenshots are taken before (1.0 s) and after (3.0 s) the node tree hides the red
    cube from that camera (1.5 s), and the view's red pixels are counted in each. True once done,
    or after a time limit."""
    import bpy
    import numpy as np

    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    now = time.monotonic() - store.setdefault("cvs_clock", time.monotonic())
    phase = int(owner.get("cvs_phase", 0))
    directory = os.path.dirname(str(owner.get("__ln_acceptance_status_path", "")))
    if phase == 0:
        camera = scene.objects.get(str(owner.get("__ln_acceptance_cvs_camera", "")))
        if camera is None:
            owner["cvs_error"] = "missing camera"
            return True
        size = min(bge.render.getWindowWidth(), bge.render.getWindowHeight()) // 2
        camera.setViewport(2, 2, 2 + size, 2 + size)
        camera.useViewport = True
        camera.setOnTop()
        store["cvs_rect"] = (2, 2, 2 + size, 2 + size)
        owner["cvs_phase"] = 1
        return False
    # Both pictures are taken before either is read: loading an image changes Blender's data, and
    # the update that follows makes every renderer draw again (the change would show anyway).
    shots = (("before", 1.0), ("after", 3.0))
    if phase in (1, 2):
        name, at = shots[phase - 1]
        if now >= at:
            bge.render.makeScreenshot(os.path.join(directory, "cvs_" + name + ".png"))
            owner["cvs_phase"] = phase + 1
        return now >= 8.0
    if phase == 3:
        clock = time.monotonic()
        store.setdefault("cvs_read_start", clock)
        if clock - store["cvs_read_start"] > 4.0:
            owner["cvs_error"] = "no readable screenshot"
            return True
        paths = {}
        for name, _at in shots:
            files = sorted(item for item in os.listdir(directory) if item.startswith("cvs_" + name))
            paths[name] = os.path.join(directory, files[-1]) if files else ""
        sizes = tuple(os.path.getsize(path) if path else 0 for path in paths.values())
        # Written by a task: read the files once their sizes have not changed for 0.3 s.
        if 0 in sizes or sizes != store.get("cvs_sizes"):
            store["cvs_sizes"] = sizes
            store["cvs_sizes_changed"] = clock
            return False
        if clock - store["cvs_sizes_changed"] < 0.3:
            return False
        result = {}
        for name, path in paths.items():
            image = None
            try:
                image = bpy.data.images.load(path)
                width, height = image.size
                pixels = np.empty(width * height * 4, dtype=np.float32)
                image.pixels.foreach_get(pixels)
            except Exception:
                width = height = 0
            finally:
                if image is not None:
                    bpy.data.images.remove(image)
            if width == 0 or height == 0:
                store["cvs_sizes_changed"] = clock
                return False
            left, bottom, right, top = store["cvs_rect"]
            view = pixels.reshape(height, width, 4)[bottom + 3:top - 2, left + 3:right - 2, :3]
            red = (view[:, :, 0] > 0.5) & (view[:, :, 1] < 0.25) & (view[:, :, 2] < 0.25)
            result[name] = int(np.count_nonzero(red))
        store["cvs_result"] = result
        owner["cvs_phase"] = 4
        return True
    return True


def drive_collection_membership(owner):
    """Collection membership: one step of CM_STEPS at a time, three calls after the node tree
    cleared the last step's flag (the step's property writes apply with the clear). A record
    clears every object's cm_seen and asks the tree to count and mark (cm_do_mark), then reads
    the counts and marks. True once done, or after a time limit."""
    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    start = store.setdefault("cm_clock", time.monotonic())
    phase = int(owner.get("cm_phase", 0))
    if phase >= len(CM_STEPS):
        return True
    if time.monotonic() - start > 12.0:
        owner["cm_error"] = "timed out at step %d %s" % (phase, CM_STEPS[phase])
        return True
    if any(owner.get(flag, False) for flag in CM_FLAGS):
        owner["cm_wait"] = 0
        return False
    owner["cm_wait"] = int(owner.get("cm_wait", 0)) + 1
    if owner["cm_wait"] < 3:
        return False
    owner["cm_wait"] = 0
    kind, name = CM_STEPS[phase]
    if kind == "do":
        owner[name] = True
    elif kind == "wait":
        if not owner.get(name + "_done", False):
            return False
        store[name + "_ok"] = bool(owner.get(name + "_ok", False))
    elif kind == "record":
        if not owner.get("cm_cleared", False):
            for obj in scene.objects:
                if "cm_seen" in obj:
                    obj["cm_seen"] = False
            owner["cm_cleared"] = True
            owner["cm_do_mark"] = True
            return False
        owner["cm_cleared"] = False
        store["cm_" + name] = {
            "direct": int(owner.get("cm_direct", -1)),
            "all": int(owner.get("cm_all", -1)),
            "linked": int(owner.get("cm_linked", -1)),
            "seen": sorted(obj.name for obj in scene.objects if obj.get("cm_seen", False)),
        }
    else:
        def visible(key):
            obj = scene.objects.get(str(owner.get("__ln_acceptance_cm_" + key, "")))
            return bool(obj.visible) if obj is not None else None

        blip = str(owner.get("__ln_acceptance_cm_blip", ""))
        tree = str(owner.get("__ln_acceptance_cm_tree", ""))
        store["cm_" + name] = {
            "copies": [bool(obj.visible) for obj in scene.objects if obj.name == blip],
            "enemy": visible("enemy"),
            "ring": visible("ring"),
            "marker": visible("marker"),
            # Each tree by the Empty instancing it ("" for the asset itself).
            "trees": {(obj.groupObject.name if obj.groupObject is not None else ""):
                      bool(obj.visible) for obj in scene.objects if obj.name == tree},
        }
    owner["cm_phase"] = phase + 1
    return phase + 1 >= len(CM_STEPS)


def check_game_property_types(owner):
    """After Load Game: check every property kind, Python writes, then removing the target."""
    import mathutils

    scene = bge.logic.getCurrentScene()
    stage = owner.get("props_stage", 0)
    if stage == 0 and owner.get("props_loaded", False):
        target = scene.objects["LN_Acceptance_PropsTarget"]
        text = scene.objects["LN_Acceptance_PropsText"]

        def name_of(obj):
            return obj.name if obj is not None else None

        rows = [dict(row, owner=name_of(row.get("owner"))) for row in owner["items"]]
        spawned = owner.get("spawned")
        checks = {
            "vec": tuple(owner["vec"]) == (1.0, 2.0, 3.0),
            "tint": tuple(round(x, 3) for x in owner["tint"]) == (0.5, 0.25, 1.0, 1.0),
            "target": name_of(owner["target"]) == target.name,
            "name_ref": name_of(owner["name_ref"]) == "LN_Acceptance_PropsOther",
            "choice": owner["mode"] == "Hard",
            "numbers": owner["numbers"] == [3, 5, 7],
            "copied_list": owner["copied_list"] == [3, 5, 7],
            "copied_target": name_of(owner["copied_target"]) == target.name,
            "items": rows == [{"item": "Apple", "count": 5, "owner": target.name},
                              {"item": "Sword", "count": 1, "owner": None}],
            "from_driver": tuple(target["from_driver"]) == (1.0, 2.0, 3.0),
            "text": text["Text"] == "Hello",
            "typed_reads": owner["own_vec_float"] == 0.0 and owner["other_vec_float"] == 0.0,
            "spawn_part": spawned is not None and any(child is spawned.get("part")
                                                      for child in spawned.children),
            "spawn_outside": (spawned is not None
                              and name_of(spawned.get("outside")) == "LN_Acceptance_PropsOther"),
        }
        written = [9]
        owner["numbers"] = written
        checks["python_list"] = owner["numbers"] == [9] and owner["numbers"] is not written
        owner["vec"] = mathutils.Vector((4.0, 5.0, 6.0))
        checks["python_vector"] = tuple(owner["vec"]) == (4.0, 5.0, 6.0)
        try:
            owner["numbers"] = 5
            checks["python_type_error"] = False
        except TypeError:
            checks["python_type_error"] = True
        fresh = [1]
        owner["fresh"] = fresh
        checks["python_new_key"] = owner["fresh"] is fresh
        owner["props_report"] = repr(sorted(name for name, ok in checks.items() if not ok))
        target.endObject()
        owner["props_stage"] = 1
    elif stage == 1 and "LN_Acceptance_PropsTarget" not in scene.objects:
        cleared = owner["copied_target"] is None and owner["items"][0]["owner"] is None
        if not cleared:
            owner["props_report"] = owner["props_report"] + " removal"
        owner["props_stage"] = 2
        owner["ui_cycle_done"] = True


def record_grid_slots(owner):
    """While the grid's menu is open, remember each slot's icon mesh, count and text data, once
    for each time the menu opens."""
    slots = [obj for obj in bge.logic.getCurrentScene().objects
             if obj.name == "LN_Acceptance_UIGridSlot"]
    if len(slots) != 3:
        owner["ui_grid_open"] = False
        return
    if not owner.get("ui_grid_open", False):
        owner["ui_grid_open"] = True
        owner["ui_grid_opens"] = int(owner.get("ui_grid_opens", 0)) + 1
    seen = []
    for slot in sorted(slots, key=lambda obj: obj.worldPosition.x):
        icon = next((child for child in slot.children if child.meshes), None)
        text = next((child for child in slot.children if child.get("Text") is not None), None)
        seen.append([icon.meshes[0].name if icon else "",
                     str(text["Text"]) if text else "",
                     text.blenderObject.data.name if text else ""])
    records = json.loads(str(owner.get("ui_grid_seen", "{}")))
    records[str(owner["ui_grid_opens"])] = seen
    owner["ui_grid_seen"] = json.dumps(records)


def record_grid_scrollbar(owner):
    """Each phase of the menu: are the scrollbar and its handle shown, and the handle's place."""
    phase = int(owner.get("ui_phase", 0))
    if phase not in (1, 2, 3):
        return
    scene = bge.logic.getCurrentScene()
    bar = scene.objects.get("LN_Acceptance_UIScrollBar")
    handle = scene.objects.get("LN_Acceptance_UIScrollHandle")
    seen = json.loads(str(owner.get("ui_scroll_seen", "{}")))
    seen[str(phase)] = [bool(bar is not None and bar.visible),
                        bool(handle is not None and handle.visible),
                        None if handle is None else round(handle.worldPosition.y, 3),
                        None if handle is None else round(handle.worldScale.y, 3)]
    owner["ui_scroll_seen"] = json.dumps(seen)


def color_tuple(value):
    return tuple(float(value[index]) for index in range(4))


def orientation_tuple(value):
    if hasattr(value, "to_euler"):
        value = value.to_euler()
    return vector_tuple(value)


def close_tuple(observed, expected, tolerance):
    return all(abs(observed[index] - expected[index]) <= tolerance for index in range(3))


def close_color(observed, expected, tolerance):
    return all(abs(observed[index] - expected[index]) <= tolerance for index in range(4))


def write_status(owner, payload):
    status_path = owner.get("__ln_acceptance_status_path", "")
    if not status_path:
        return
    with open(status_path, "a", encoding="utf-8") as status_file:
        status_file.write(json.dumps(payload, sort_keys=True) + "\n")


def expected_vector(owner):
    return (
        float(owner.get("__ln_acceptance_expected_x", 0.0)),
        float(owner.get("__ln_acceptance_expected_y", 0.0)),
        float(owner.get("__ln_acceptance_expected_z", 0.0)),
    )


def expected_rotation(owner):
    return (
        float(owner.get("__ln_acceptance_expected_rx", 0.0)),
        float(owner.get("__ln_acceptance_expected_ry", 0.0)),
        float(owner.get("__ln_acceptance_expected_rz", 0.0)),
    )


def expected_color(owner):
    return (
        float(owner.get("__ln_acceptance_expected_r", 1.0)),
        float(owner.get("__ln_acceptance_expected_g", 1.0)),
        float(owner.get("__ln_acceptance_expected_b", 1.0)),
        float(owner.get("__ln_acceptance_expected_a", 1.0)),
    )


def expected_named_vector(owner, prefix):
    return (
        float(owner.get(f"__ln_acceptance_{prefix}_x", 0.0)),
        float(owner.get(f"__ln_acceptance_{prefix}_y", 0.0)),
        float(owner.get(f"__ln_acceptance_{prefix}_z", 0.0)),
    )


def mutate_runtime_structure(owner):
    if owner.get("__ln_acceptance_mode", "") != "phase6_save_load_structure":
        return

    object_name = str(owner.get("__ln_acceptance_structure_object", ""))
    runtime_target = next(
        (
            candidate
            for candidate in bge.logic.getCurrentScene().objects
            if candidate.name == object_name
        ),
        None,
    )
    if (
        runtime_target is None
        or not bool(runtime_target.get("structure_mutate_output", False))
        or bool(runtime_target.get("structure_output_mutated", False))
    ):
        return
    try:
        import bpy

        modifier_name = str(owner.get("__ln_acceptance_structure_modifier", ""))
        input_identifier = str(owner.get("__ln_acceptance_structure_input", ""))
        output_identifier = str(owner.get("__ln_acceptance_structure_output", ""))
        transient_modifier = str(
            owner.get("__ln_acceptance_structure_transient_modifier", ""))
        removed_modifier = str(
            owner.get("__ln_acceptance_structure_removed_modifier", ""))
        obj = bpy.data.objects.get(object_name)
        modifier = obj.modifiers.get(modifier_name) if obj is not None else None
        if modifier is None or modifier.type != "NODES":
            raise RuntimeError("the authored Geometry Nodes modifier is missing")
        input_state = getattr(modifier.properties.inputs, input_identifier)
        observed_mutated_value = float(input_state.value)
        expected_mutated_value = float(
            owner.get("__ln_acceptance_structure_mutated_value", 9.5))
        if abs(observed_mutated_value - expected_mutated_value) > 0.0001:
            raise RuntimeError(
                "Set Geometry Nodes Input did not apply the runtime mutation")
        if obj.modifiers.get(transient_modifier) is None:
            raise RuntimeError(
                "Assign Geometry Nodes Modifier did not append the runtime modifier")
        removed = obj.modifiers.get(removed_modifier)
        if removed is None or removed.type != "NODES":
            raise RuntimeError(
                "the Geometry Nodes modifier selected for removal is missing")
        obj.modifiers.remove(removed)
        output_state = getattr(modifier.properties.outputs, output_identifier)
        output_state.attribute_name = "mutated_attr"
        modifier.show_viewport = False

        internal_node_name = str(
            owner.get("__ln_acceptance_structure_internal_node", ""))
        internal_socket_name = str(
            owner.get("__ln_acceptance_structure_internal_socket", ""))
        internal_node = modifier.node_group.nodes.get(internal_node_name)
        if internal_node is None:
            raise RuntimeError("the runtime Geometry Nodes value node is missing")
        internal_node.inputs[internal_socket_name].default_value = float(
            owner.get("__ln_acceptance_structure_internal_mutated_value", 1.0))
        internal_node.mute = True

        compositor_tree_name = str(
            owner.get("__ln_acceptance_structure_compositor_tree", ""))
        compositor_node_name = str(
            owner.get("__ln_acceptance_structure_compositor_node", ""))
        compositor_socket_name = str(
            owner.get("__ln_acceptance_structure_compositor_socket", ""))
        compositor_tree = bpy.data.node_groups.get(compositor_tree_name)
        compositor_node = (
            compositor_tree.nodes.get(compositor_node_name)
            if compositor_tree is not None
            else None
        )
        if compositor_node is None:
            raise RuntimeError("the saved compositor node is missing")
        compositor_node.inputs[compositor_socket_name].default_value = float(
            owner.get("__ln_acceptance_structure_compositor_mutated_value", 75.0))
        compositor_node.mute = True
        transient_compositor_name = str(
            owner.get("__ln_acceptance_structure_transient_compositor_tree", ""))
        transient_compositor = bpy.data.node_groups.get(transient_compositor_name)
        if (
            transient_compositor is None
            or transient_compositor.bl_idname != "CompositorNodeTree"
        ):
            raise RuntimeError("the transient compositor node tree is missing")
        bpy.context.scene.compositing_node_group = transient_compositor
    except Exception:
        runtime_target["structure_mutation_error"] = traceback.format_exc()
    finally:
        runtime_target["structure_output_mutated"] = True


def per_player_menu_state(owner):
    """Per-player menus: each copy of the menu's panel, as (user, owner gold, its texts sorted,
    screen x, scale x)."""
    scene = bge.logic.getCurrentScene()
    panel_name = str(owner.get("__ln_acceptance_panel", ""))
    camera = scene.objects.get(str(owner.get("__ln_acceptance_menu_camera", "")))
    copies = []
    for obj in scene.objects:
        if obj.name != panel_name:
            continue
        texts = []
        corner = badge = None
        for child in obj.childrenRecursive:
            data = getattr(child.blenderObject, "data", None)
            if data is not None and hasattr(data, "body"):
                texts.append(data.body.strip())
            if child.name.endswith("_PPCorner"):
                corner = child
            if child.name.endswith("_PPBadge"):
                badge = child
        screen_x = float(camera.getScreenPosition(obj)[0]) if camera is not None else -1.0
        anchored = ()
        if camera is not None and corner is not None and badge is not None:
            corner_screen = camera.getScreenPosition(corner)
            offset = badge.worldPosition - corner.worldPosition
            anchored = (round(float(corner_screen[0]), 3), round(float(corner_screen[1]), 3),
                        round(float(corner.worldScale[0]), 3), round(float(offset.x), 3),
                        round(float(offset.y), 3))
        user = int(obj.get("menu_user", -1))
        popups = sum(1 for other in scene.objects if other.name.endswith("_PPPopup")
                     and int(other.get("popup_user", -9)) == user)
        copies.append((user, int(obj.get("owner_gold", -1)), sorted(texts), round(screen_x, 3),
                       round(float(obj.worldScale[0]), 3), anchored, popups))
    return sorted(copies)


def check_owner(owner):
    mode = owner.get("__ln_acceptance_mode", "quit_only")
    tolerance = float(owner.get("__ln_acceptance_tolerance", 0.0001))
    expected = expected_vector(owner)

    if mode == "quit_only":
        return True, {"mode": mode}

    if mode == "ui_example_file":
        # Add > Game UI > Open Complete UI Example: its Game tree shows the HUD at the start, and
        # with every menu open their trees fill the grids from the slot templates (one each before).
        names = [obj.name for obj in bge.logic.getCurrentScene().objects]
        counts = {name: names.count(name) for name in (
            "Stamina Bar", "Value Text", "Slot", "RecipeSlot", "BagSlot", "ChestRow")}
        return (counts["Stamina Bar"] == 1 and counts["Value Text"] == 1 and
                all(counts[name] > 1 for name in ("Slot", "RecipeSlot", "BagSlot", "ChestRow"))), {
            "mode": mode,
            "counts": counts,
        }

    if mode == "ui_templates":
        # Add > Game UI > Templates: with every menu the templates made open, their trees fill
        # the grids; each cell of the equipment once; the late Settings menu's buttons are there;
        # the Pickups template's items are in the world.
        names = [obj.name for obj in bge.logic.getCurrentScene().objects]
        counts = {name: names.count(name) for name in (
            "Slot", "RecipeSlot", "WeaponSlot", "BagSlot", "ChestRow", "WalkSlider",
            "SettingsButton", "Settings", "Resume")}
        counts["pickups"] = sum(1 for name in names if name.startswith("Pickup_"))
        return (all(counts[name] > 1 for name in ("Slot", "RecipeSlot", "BagSlot", "ChestRow")) and
                all(counts[name] == 1 for name in ("WeaponSlot", "WalkSlider", "SettingsButton",
                                                   "Settings", "Resume")) and
                counts["pickups"] > 0), {
            "mode": mode,
            "counts": counts,
        }

    if mode == "game_property_string":
        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_expected_object", "")
        property_name = owner.get("__ln_acceptance_expected_property", "")
        expected_value = owner.get("__ln_acceptance_expected_string", "")
        target = scene.objects.get(target_name)
        observed = None if target is None else target.get(property_name, None)
        return observed == expected_value, {
            "mode": mode,
            "object": target_name,
            "property": property_name,
            "observed": observed,
            "expected": expected_value,
        }

    if mode == "inactive_overlay_collection":
        import bpy

        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_expected_object", "")
        child_name = owner.get("__ln_acceptance_expected_child", "")
        camera_name = owner.get("__ln_acceptance_expected_camera", "")
        collection_name = owner.get("__ln_acceptance_expected_collection", "")
        property_name = owner.get("__ln_acceptance_expected_property", "")
        expected_value = owner.get("__ln_acceptance_expected_string", "")
        target = scene.objects.get(target_name)
        child = scene.objects.get(child_name)
        camera = scene.objects.get(camera_name)
        inactive_objects = getattr(scene, "objectsInactive", None)
        source_camera = (
            None if inactive_objects is None else inactive_objects.get(camera_name)
        )
        observed = None if target is None else target.get(property_name, None)
        child_observed = None if child is None else child.get(property_name, None)
        # Object properties of the live copies point at the live copies, never the templates.
        def reference_state(obj, name, live):
            value = None if obj is None else obj.get(name)
            if value is None:
                return "none"
            return "live" if value is live else f"other {value.name} {value.invalid}"

        references_ok = (
            target is not None
            and camera is not None
            and child is not None
            and target.get("partner") is camera
            and target.get("part") is child
            and camera.get("watch") is target
        )
        item_init_observed = (
            None if target is None else target.get("overlay_item_init", None)
        )
        item_fixed_observed = (
            None if target is None else target.get("overlay_item_fixed", None)
        )
        child_init_observed = (
            None if child is None else child.get("overlay_item_init", None)
        )
        child_fixed_observed = (
            None if child is None else child.get("overlay_item_fixed", None)
        )
        camera_init_observed = (
            None if camera is None else camera.get("overlay_camera_init", None)
        )
        camera_fixed_observed = (
            None if camera is None else camera.get("overlay_camera_fixed", None)
        )
        cycle_ok = bool(owner.get("overlay_cycle_ok", False))
        cycle_error = str(owner.get("overlay_cycle_error", ""))
        result = owner.get("overlay_result", "")
        render_object = None if target is None else target.blenderObject
        child_render_object = None if child is None else child.blenderObject
        blender_scene = bpy.data.scenes.get(scene.name)
        scene_root = None if blender_scene is None else blender_scene.collection
        render_visible = render_object is not None and bool(render_object.visible_get())
        child_render_visible = (
            child_render_object is not None and bool(child_render_object.visible_get())
        )
        staged_in_scene_root = (
            scene_root is not None
            and render_object is not None
            and scene_root.objects.get(render_object.name) == render_object
        )
        child_staged_in_scene_root = (
            scene_root is not None
            and child_render_object is not None
            and scene_root.objects.get(child_render_object.name) == child_render_object
        )
        source_collection = bpy.data.collections.get(collection_name)
        collection_still_disabled = (
            source_collection is not None and bool(source_collection.hide_viewport)
        )
        source_object = bpy.data.objects.get(target_name)
        source_still_hidden = (
            source_object is not None and not bool(source_object.visible_get())
        )
        ok = (
            result == "accepted"
            and observed == expected_value
            and child_observed == expected_value
            and item_init_observed == "ran"
            and item_fixed_observed == "ran"
            and child_init_observed == "ran"
            and child_fixed_observed == "ran"
            and camera_init_observed == "ran"
            and camera_fixed_observed == "ran"
            and source_camera is not None
            and camera is not source_camera
            and render_visible
            and child_render_visible
            and staged_in_scene_root
            and child_staged_in_scene_root
            and collection_still_disabled
            and source_still_hidden
            and cycle_ok
            and references_ok
        )
        return ok, {
            "mode": mode,
            "done_result": result,
            "object": target_name,
            "child": child_name,
            "camera": camera_name,
            "property": property_name,
            "observed": observed,
            "child_observed": child_observed,
            "item_init_observed": item_init_observed,
            "item_fixed_observed": item_fixed_observed,
            "child_init_observed": child_init_observed,
            "child_fixed_observed": child_fixed_observed,
            "camera_init_observed": camera_init_observed,
            "camera_fixed_observed": camera_fixed_observed,
            "camera_source_still_inactive": source_camera is not None,
            "camera_runtime_is_distinct": camera is not None and camera is not source_camera,
            "expected": expected_value,
            "render_visible": render_visible,
            "child_render_visible": child_render_visible,
            "staged_in_scene_root": staged_in_scene_root,
            "child_staged_in_scene_root": child_staged_in_scene_root,
            "collection_still_disabled": collection_still_disabled,
            "source_still_hidden": source_still_hidden,
            "cycle_ok": cycle_ok,
            "references_ok": references_ok,
            "partner_state": reference_state(target, "partner", camera),
            "part_state": reference_state(target, "part", child),
            "watch_state": reference_state(camera, "watch", target),
            "cycle_error": cycle_error,
        }

    if mode == "ui_elements":
        scene = bge.logic.getCurrentScene()
        selected_button = scene.objects.get("LN_Acceptance_UIButtonTab1")
        icon = scene.objects.get("LN_Acceptance_UIAnchoredIcon")
        badge = scene.objects.get("LN_Acceptance_UIAnchoredBadge")
        overlay_camera = scene.objects.get("LN_Acceptance_UIOverlayCamera")
        selected_scale = (
            float(selected_button.localScale.x) if selected_button is not None else 0.0)
        icon_screen = (
            tuple(float(value) for value in overlay_camera.getScreenPosition(icon))
            if icon is not None and overlay_camera is not None else None
        )
        width = max(1, bge.render.getWindowWidth())
        height = max(1, bge.render.getWindowHeight())
        # Authored 10% / 10% from the top-left of a 16:9 frame: 0.17778 frame heights across.
        expected_icon = (0.177778 * height / width, 0.1)
        icon_ok = icon_screen is not None and all(
            abs(observed - wanted) < 0.01 for observed, wanted in zip(icon_screen, expected_icon))
        # The anchored child rides on its anchored parent instead of being anchored twice.
        badge_offset = (
            tuple(float(value) for value in (badge.worldPosition - icon.worldPosition))
            if badge is not None and icon is not None else None)
        badge_ok = badge_offset is not None and all(
            abs(observed - wanted) < 1.0e-3
            for observed, wanted in zip(badge_offset, (0.3, -0.2, 0.0)))
        focus_name = str(owner.get("ui_focus_name", ""))
        focus_event = str(owner.get("ui_focused_action", ""))
        # Tab1 is selected for keyboard and gamepad, but a mouse player sees neither its hover
        # look (the authored scale 0.5 stays) nor a Focus event.
        return (focus_name == "LN_Acceptance_UIButtonTab1" and focus_event == "" and
                abs(selected_scale - 0.5) < 1.0e-3 and icon_ok and badge_ok), {
            "mode": mode,
            "focus_name": focus_name,
            "focus_event": focus_event,
            "selected_scale": selected_scale,
            "icon_screen": icon_screen,
            "expected_icon": expected_icon,
            "badge_offset": badge_offset,
        }

    if mode == "ui_menu_focus_restore":
        submenu_focus = str(owner.get("ui_submenu_focus", ""))
        restored_focus = str(owner.get("ui_restored_focus", ""))
        focus_failed = bool(owner.get("ui_set_focus_failed", False))
        # Closing the submenu gives the menu below its selection back.
        return (not focus_failed and submenu_focus == "LN_Acceptance_UISubButton" and
                restored_focus == "LN_Acceptance_UIMenuButtonB"), {
            "mode": mode,
            "submenu_focus": submenu_focus,
            "restored_focus": restored_focus,
            "set_focus_failed": focus_failed,
        }

    if mode == "ui_sound_events":
        last_move = str(owner.get("last_move", ""))
        last_menu = str(owner.get("last_menu", "none"))
        last_user = int(owner.get("last_user", -1))
        move_count = float(owner.get("move_count", 0.0))
        # The highlight moved to B under the pointer: Move with B's Action, player 0, no menu.
        return (last_move == "b" and last_menu == "" and last_user == 0 and
                move_count >= 1.0), {
            "mode": mode,
            "last_move": last_move,
            "last_menu": last_menu,
            "last_user": last_user,
            "move_count": move_count,
        }

    if mode == "smooth_motion":
        store = bge.logic.globalDict
        # Drawn frames once the steps' history exists; every object moves 0.1 a step.
        frames = [row for row in store.get("sm_frames", []) if row["t"] >= 0.4]
        step = 0.1
        errors = {"interpolate": 0.0, "extrapolate": 0.0, "none": 0.0, "child": 0.0}
        teleport_from = float(store.get("sm_teleport_from", 0.0))
        node_from = float(store.get("sm_node_from", 0.0))
        teleport_streaks = node_streaks = between = 0
        teleported = node_jumped = False
        for row in frames:
            a = row["a"]
            if "mover" in row:
                drawn, _drawn_y, true = row["mover"]
                # Interpolate: one step late, a share `a` of the last step's move.
                errors["interpolate"] = max(errors["interpolate"],
                                            abs(drawn - (true - step * (1.0 - a))))
                between += 0.05 < a < 0.95
            if "ball" in row:
                drawn, _drawn_y, true = row["ball"]
                errors["extrapolate"] = max(errors["extrapolate"],
                                            abs(drawn - (true + step * a)))
            if "plain" in row:
                drawn, _drawn_y, true = row["plain"]
                errors["none"] = max(errors["none"], abs(drawn - true))
            if "child" in row and "mover" in row:
                # A None child is drawn with its smoothed parent, 2 to its side.
                errors["child"] = max(errors["child"], abs(row["child"][0] - row["mover"][0]),
                                      abs(row["child"][1] - row["mover"][1] - 2.0))
            if "teleport" in row and "sm_teleport_from" in store:
                drawn = row["teleport"][0]
                teleport_streaks += teleport_from + 1.0 < drawn < teleport_from + 49.0
                teleported |= drawn >= teleport_from + 49.0
            if "node" in row and "sm_node_from" in store:
                drawn = row["node"][0]
                node_streaks += node_from + 1.0 < drawn < 499.0
                node_jumped |= drawn >= 499.0
        ok = (len(frames) >= 30 and between >= 10 and
              all(error < 0.002 for error in errors.values()) and
              teleported and teleport_streaks == 0 and node_jumped and node_streaks == 0)
        return ok, {
            "mode": mode,
            "frames": len(frames),
            "between_steps": between,
            "errors": errors,
            "teleported": teleported,
            "teleport_streaks": teleport_streaks,
            "node_jumped": node_jumped,
            "node_streaks": node_streaks,
        }

    if mode == "frame_pacing":
        rows = bge.logic.globalDict.get("fp_rows", [])
        # Running frames after the first 90 (loading), not the 3 after a resume.
        running = [row for row in rows if row[0] >= 90 and 3 <= row[1] < 30]
        single = sum(1 for row in running if row[2] == 1)
        flips = sum(1 for before, after in zip(running, running[1:])
                    if after[0] == before[0] + 1 and {before[2], after[2]} == {0, 2})
        paused_steps = sum(row[2] for row in rows if 33 <= row[1] < 45)
        pacings = sorted({row[3] for row in rows})
        ok = (len(running) >= 150 and single >= 0.97 * len(running) and flips == 0 and
              paused_steps == 0 and pacings == ["LIMIT"])
        return ok, {
            "mode": mode,
            "frames": len(rows),
            "running": len(running),
            "single_step": single,
            "flips": flips,
            "paused_steps": paused_steps,
            "pacings": pacings,
        }

    if mode == "sound_groups":
        import aud
        device_volume = float(aud.Device().volume)
        done = bool(owner.get("volume_done", False))
        # Set Volume Master 0.25 is the device's volume; Music and Effects only touch their sounds.
        return (done and abs(device_volume - 0.25) < 0.001), {
            "mode": mode,
            "volume_done": done,
            "device_volume": device_volume,
        }

    if mode == "ui_paused_routes":
        entered = bool(owner.get("ui_enter_while_paused", False))
        update_count = float(owner.get("update_count", 0.0))
        # The Enter route ran in a paused frame; On Update ran at most before the pause began.
        return (entered and update_count <= 2.0), {
            "mode": mode,
            "entered_while_paused": entered,
            "update_count": update_count,
        }

    if mode == "game_property_types":
        report = str(owner.get("props_report", ""))
        # Every property kind read and written in the game and restored by Load Game.
        return (bool(owner.get("ui_cycle_done", False)) and report == "[]"), {
            "mode": mode,
            "loaded": bool(owner.get("props_loaded", False)),
            "failed_checks": report,
        }

    if mode == "ui_grid_template":
        records = json.loads(str(owner.get("ui_grid_seen", "{}")))
        left = sum(1 for obj in bge.logic.getCurrentScene().objects
                   if obj.name == "LN_Acceptance_UIGridSlot")

        def grid_ok(seen):
            return (seen is not None and len(seen) == 3 and
                    [slot[0] for slot in seen[:2]] == ["LN_Acceptance_UIGridAppleMesh",
                                                       "LN_Acceptance_UIGridGemMesh"] and
                    [slot[1] for slot in seen[:2]] == ["4", ""] and
                    len({slot[2] for slot in seen}) == 3)

        # [Apple x4, Gem x1] in 1 x 3 slots made from one template, each of the two times the
        # menu opens: library icons, a count only above 1, every count on its own text data, and
        # no copy left after closing the menu.
        return (bool(owner.get("ui_cycle_done", False)) and sorted(records) == ["1", "2"] and
                all(grid_ok(records.get(key)) for key in ("1", "2")) and left == 0), {
            "mode": mode,
            "cycle_done": bool(owner.get("ui_cycle_done", False)),
            "seen": records,
            "slots_left": left,
        }

    if mode == "ui_menu_pause_toggle":
        scene = bge.logic.getCurrentScene()
        names = ("opened", "toggled_closed", "paused_frames", "steps_at_pause", "steps_at_close",
                 "steps")
        seen = {name: owner.get(name) for name in names}
        panels = sum(1 for obj in scene.objects if obj.name == "LN_Acceptance_PausePanel")
        # Opened with Pause Game: the game stopped (no Logic + Physics steps) while While Paused
        # frames ran; opened again with Toggle, it closed (Closed ran) and the game went on. The
        # menu collection stayed visible in the viewport, yet no live copy of it is left.
        return (bool(seen["opened"]) and bool(seen["toggled_closed"]) and
                int(seen["paused_frames"] or 0) >= 30 and
                seen["steps_at_close"] == seen["steps_at_pause"] and
                int(seen["steps"] or 0) > int(seen["steps_at_close"] or 0) and panels == 0), {
            "mode": mode,
            "seen": seen,
            "panels": panels,
        }

    if mode == "ui_text_values_bar":
        scene = bge.logic.getCurrentScene()

        def body(name):
            obj = scene.objects.get(name)
            return None if obj is None else obj.blenderObject.data.body

        def fill(name):
            obj = scene.objects.get(name)
            return None if obj is None else [round(obj.localScale.x, 3), round(obj.worldPosition.x, 3)]

        texts = [body(name) for name in ("LN_Acceptance_TVText", "LN_Acceptance_TVParentText",
                                          "LN_Acceptance_TVMissingText")]
        fills = [fill(name) for name in ("LN_Acceptance_TVBarFill", "LN_Acceptance_TVChildBarFill",
                                         "LN_Acceptance_TVZeroBarFill")]
        # Each slot bar's fill: its scale, and its centre from its bar's (2 wide, from the left).
        slot_fills = [(round(obj.localScale.x, 3),
                       round(obj.worldPosition.x - obj.parent.worldPosition.x, 3))
                      for obj in scene.objects if obj.name == "LN_Acceptance_TVSlotBarFill"]
        # Values in text follow the game properties ({Object.property}, {property} of a parent,
        # :1 for one decimal) and unknown ones stay as written; a Bar's fill is value / Max
        # Property along the bar from its left edge (2 wide at x = 0: left edge -1), and one on a
        # child reads its parent's value, capped at full. All four grid slots show 90 of 120,
        # also the two copied from the template after its bar showed it, and a Max Property of 0
        # shows empty.
        return (bool(owner.get("ui_cycle_done", False)) and
                texts == ["Gold 42 | 2.3", "42/90", "{Missing.gold} {nothing}"] and
                fills[0] is not None and abs(fills[0][0] - 0.75) < 0.002 and
                abs(fills[0][1] - (-1.0 + 0.75)) < 0.01 and
                fills[1] is not None and abs(fills[1][0] - 1.0) < 0.002 and
                fills[2] is not None and fills[2][0] < 0.002 and len(slot_fills) == 4 and
                all(abs(scale - 0.75) < 0.002 and abs(offset - (-1.0 + 0.75)) < 0.01
                    for scale, offset in slot_fills)), {
            "mode": mode,
            "texts": texts,
            "fills": fills,
            "slot_fills": slot_fills,
        }

    if mode == "ui_menu_camera_hidden":
        scene = bge.logic.getCurrentScene()
        names = set(obj.name for obj in scene.objects)
        # The camera menus are shown with starts live although its collection is disabled in the
        # viewport (its own tree ran); the menu's objects stay asleep until it opens.
        return (bool(owner.get("ui_cycle_done", False)) and
                bool(owner.get("camera_awake", False)) and
                "LN_Acceptance_MCCamera" in names and "LN_Acceptance_MCPanel" not in names), {
            "mode": mode,
            "camera_awake": bool(owner.get("camera_awake", False)),
            "camera_live": "LN_Acceptance_MCCamera" in names,
            "panel_live": "LN_Acceptance_MCPanel" in names,
        }

    if mode == "ui_grid_parented_template":
        scene = bge.logic.getCurrentScene()
        panel = scene.objects.get("LN_Acceptance_GPPanel")
        slots = [obj for obj in scene.objects if obj.name == "LN_Acceptance_GPSlot"]
        expected = [(0.0, 0.0), (2.0, 0.0), (0.0, -2.0), (2.0, -2.0)]
        lattice = None
        if panel is not None and len(slots) == 4:
            turn = panel.worldOrientation.inverted()
            # One of them is the template, the top left cell: the others are 2 apart (Spacing 1
            # in the panel's units, which are 2) along the panel's turned axes.
            for origin in slots:
                cells = [turn @ (obj.worldPosition - origin.worldPosition) for obj in slots]
                if all(any(abs(cell.x - x) < 0.01 and abs(cell.y - y) < 0.01 for cell in cells)
                       for x, y in expected):
                    lattice = [[round(cell.x, 3), round(cell.y, 3)] for cell in cells]
                    break
        parents = sorted(str(obj.parent.name) if obj.parent is not None else "" for obj in slots)
        scales = [round(obj.worldScale.x, 3) for obj in slots]
        # Object Grid View copies a template that is a child of a moved, turned and scaled panel:
        # around the template, along the panel's axes, in its units, as children of the panel.
        return (bool(owner.get("ui_cycle_done", False)) and lattice is not None and
                parents == ["LN_Acceptance_GPPanel"] * 4 and
                all(abs(scale - 2.0) < 0.002 for scale in scales)), {
            "mode": mode,
            "count": len(slots),
            "lattice": lattice,
            "positions": [[round(value, 3) for value in obj.worldPosition] for obj in slots],
            "parents": parents,
            "scales": scales,
        }

    if mode == "ui_grid_pages":
        seen = [str(owner.get(key, "")) for key in ("pages_start", "pages_next", "pages_show",
                                                    "pages_top")]
        # 10 one-item entries in 2 x 2 slots: pages of 2 rows, 3 pages. Next Page goes 2 rows
        # on, Show Item 9 shows the last rows, Top goes back; Page, Page Count and First Visible
        # are numbers formatted straight into text.
        return (bool(owner.get("ui_cycle_done", False)) and
                seen == ["1 / 3 / 0", "2 / 3 / 4", "3 / 3 / 6", "1 / 3 / 0"]), {
            "mode": mode,
            "cycle_done": bool(owner.get("ui_cycle_done", False)),
            "seen": seen,
        }

    if mode == "ui_grid_scrollbar":
        seen = json.loads(str(owner.get("ui_scroll_seen", "{}")))

        def near(value, expected):
            return value is not None and abs(value - expected) <= 0.01

        fits, top, end = seen.get("1"), seen.get("2"), seen.get("3")
        # 2 items fit 1 x 2 slots: no scrollbar. 6 items: a third is in view, so the handle is a
        # third of the bar, at its top, and at its bottom once First Item shows the last row.
        return (bool(owner.get("ui_cycle_done", False)) and fits is not None and
                fits[:2] == [False, False] and top is not None and top[:2] == [True, True] and
                near(top[2], 0.667) and near(top[3], 0.333) and end is not None and
                near(end[2], -0.667) and near(end[3], 0.333)), {
            "mode": mode,
            "cycle_done": bool(owner.get("ui_cycle_done", False)),
            "seen": seen,
        }

    if mode == "ui_overlay_visibility":
        scene = bge.logic.getCurrentScene()
        states = {}
        for name in ("LN_Acceptance_UIHudPlain", "LN_Acceptance_UIHudTemplate"):
            obj = scene.objects.get(name)
            states[name] = None if obj is None else bool(obj.visible)
        menu_opened = bool(owner.get("ui_menu_opened", False))
        cycle_done = bool(owner.get("ui_cycle_done", False))
        # Set Collection Visibility hides a HUD of plain visible objects too, opening and
        # closing a menu does not show a hidden HUD again, and showing one HUD leaves the other.
        return (menu_opened and cycle_done and
                states["LN_Acceptance_UIHudPlain"] is True and
                states["LN_Acceptance_UIHudTemplate"] is False and
                not bool(owner.get("ui_plain_visible_with_menu", True)) and
                not bool(owner.get("ui_template_visible_with_menu", True))), {
            "mode": mode,
            "menu_opened": menu_opened,
            "cycle_done": cycle_done,
            "visible": states,
            "plain_visible_with_menu": owner.get("ui_plain_visible_with_menu", None),
            "template_visible_with_menu": owner.get("ui_template_visible_with_menu", None),
        }

    if mode == "object_menu_session":
        import bpy

        scene = bge.logic.getCurrentScene()
        item_name = str(owner.get("__ln_acceptance_expected_object", ""))
        collection_name = str(owner.get("__ln_acceptance_expected_collection", ""))
        inactive_objects = getattr(scene, "objectsInactive", None)
        source_item = (
            None if inactive_objects is None else inactive_objects.get(item_name)
        )
        source_collection = bpy.data.collections.get(collection_name)
        first_handle = int(owner.get("menu_handle_first", 0))
        second_handle = int(owner.get("menu_handle_second", 0))
        first_open_result = int(owner.get("menu_open_first_result", -1))
        second_open_result = int(owner.get("menu_open_second_result", -1))
        first_close_result = int(owner.get("menu_close_first_result", -1))
        second_close_result = int(owner.get("menu_close_second_result", -1))
        repeat_close_result = int(owner.get("menu_close_repeat_result", -1))
        repeat_closed = bool(owner.get("menu_repeat_closed", False))
        external_ready = bool(owner.get("menu_external_ready", False))
        external_removed = bool(owner.get("menu_external_removed", False))
        external_handle = int(owner.get("menu_handle_external", 0))
        external_open_result = int(owner.get("menu_open_external_result", -1))
        external_close_result = int(owner.get("menu_close_external_result", -1))
        runtime_item = scene.objects.get(item_name)
        ok = (
            not bool(owner.get("menu_session_failed", False))
            and bool(owner.get("menu_item_logic_ran", False))
            and first_handle > 0
            and second_handle > 0
            and first_handle != second_handle
            and first_open_result == 0
            and second_open_result == 0
            and first_close_result == 0
            and second_close_result == 0
            and repeat_close_result == 1
            and external_handle > 0
            and external_open_result == 0
            and external_removed
            and external_close_result == 0
            and bool(owner.get("menu_session_done", False))
            and runtime_item is None
            and source_item is not None
            and source_collection is not None
            and bool(source_collection.hide_viewport)
        )
        return ok, {
            "mode": mode,
            "session_done": bool(owner.get("menu_session_done", False)),
            "session_failed": bool(owner.get("menu_session_failed", False)),
            "item_logic_ran": bool(owner.get("menu_item_logic_ran", False)),
            "first_handle": first_handle,
            "second_handle": second_handle,
            "first_open_result": first_open_result,
            "second_open_result": second_open_result,
            "first_close_result": first_close_result,
            "second_close_result": second_close_result,
            "repeat_close_result": repeat_close_result,
            "repeat_closed": repeat_closed,
            "external_ready": external_ready,
            "external_removed": external_removed,
            "external_handle": external_handle,
            "external_open_result": external_open_result,
            "external_close_result": external_close_result,
            "runtime_item_removed": runtime_item is None,
            "source_item_inactive": source_item is not None,
            "collection_still_disabled": (
                source_collection is not None and bool(source_collection.hide_viewport)
            ),
        }

    if mode in {"position", "unchanged_position"}:
        observed = vector_tuple(owner.worldPosition)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "orientation":
        observed = orientation_tuple(owner.worldOrientation)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "velocity":
        observed = vector_tuple(owner.worldLinearVelocity)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "impulse":
        observed = vector_tuple(owner.worldLinearVelocity)
        min_speed = float(owner.get("__ln_acceptance_min_speed", 0.0))
        speed = math.sqrt(sum(component * component for component in observed))
        return speed >= min_speed, {
            "mode": mode,
            "observed": observed,
            "speed": speed,
            "min_speed": min_speed,
        }

    if mode == "property_branch":
        observed = vector_tuple(owner.worldPosition)
        moved = bool(owner.get("moved", False))
        return close_tuple(observed, expected, tolerance) and moved, {
            "mode": mode,
            "observed": observed,
            "expected": expected,
            "moved": moved,
        }

    if mode == "property_writes":
        score = float(owner.get("score", 0.0))
        done = bool(owner.get("done", False))
        state = owner.get("state", "")
        ok = abs(score - 42.5) <= tolerance and done and state == "running"
        return ok, {
            "mode": mode,
            "score": score,
            "done": done,
            "state": state,
        }

    if mode == "property_target":
        target_name = owner.get("__ln_acceptance_property_target", "")
        scene = owner.scene
        target = next((game_object for game_object in scene.objects if game_object.name == target_name), None)
        score = int(target.get("score", 0)) if target is not None else 0
        expected_score = int(owner.get("__ln_acceptance_expected_score", 42))
        return target is not None and score == expected_score, {
            "mode": mode,
            "target": target_name,
            "score": score,
            "expected_score": expected_score,
        }

    if mode == "property_advanced":
        enabled = bool(owner.get("enabled", True))
        hit = bool(owner.get("has_hit", False))
        speed = float(owner.get("speed", 0.0))
        throttle = float(owner.get("throttle", 0.0))
        choice = float(owner.get("choice", 0.0))
        stored = float(owner.get("stored", 0.0))
        ok = (
            not enabled
            and hit
            and abs(speed - 3.0) <= tolerance
            and abs(throttle - 4.0) <= tolerance
            and abs(choice - 7.0) <= tolerance
            and abs(stored - 5.0) <= tolerance
        )
        return ok, {
            "mode": mode,
            "enabled": enabled,
            "hit": hit,
            "speed": speed,
            "throttle": throttle,
            "choice": choice,
            "stored": stored,
        }

    if mode == "tree_properties":
        tree_value = float(owner.get("tree_value", 0.0))
        ok = abs(tree_value - 3.5) <= tolerance
        return ok, {
            "mode": mode,
            "tree_value": tree_value,
        }

    if mode == "data_containers":
        list_len = int(owner.get("list_len", 0))
        dict_len = int(owner.get("dict_len", 0))
        dict_keys_len = int(owner.get("dict_keys_len", 0))
        dict_answer = float(owner.get("dict_answer", 0.0))
        loop_count = float(owner.get("loop_count", 0.0))
        empty_list_len = int(owner.get("empty_list_len", -1))
        empty_dict_len = int(owner.get("empty_dict_len", -1))
        list_sum = float(owner.get("list_sum", 0.0))
        list_item_value = float(owner.get("list_item_value", 0.0))
        has_answer_key = bool(owner.get("has_answer_key", False))
        has_missing_key = bool(owner.get("has_missing_key", False))
        list_dup_len = int(owner.get("list_dup_len", -1))
        merged_dict_len = int(owner.get("merged_dict_len", -1))
        list_has_two = bool(owner.get("list_has_two", False))
        list_has_nine = bool(owner.get("list_has_nine", False))
        list_has_nested = bool(owner.get("list_has_nested", False))
        typed_list_len = int(owner.get("typed_list_len", -1))
        typed_list_sum = float(owner.get("typed_list_sum", 0.0))
        ok = (
            list_len == 3
            and dict_len == 1
            and dict_keys_len == 1
            and abs(dict_answer - 7.5) <= tolerance
            and abs(loop_count - 3.0) <= tolerance
            and empty_list_len == 0
            and empty_dict_len == 0
            and abs(list_sum - 9.0) <= tolerance
            and abs(list_item_value - 2.0) <= tolerance
            and has_answer_key
            and not has_missing_key
            and list_dup_len == 3
            and merged_dict_len == 2
            and list_has_two
            and not list_has_nine
            and list_has_nested
            and typed_list_len == 3
            and abs(typed_list_sum - 15.0) <= tolerance
        )
        return ok, {
            "mode": mode,
            "list_len": list_len,
            "dict_len": dict_len,
            "dict_keys_len": dict_keys_len,
            "dict_answer": dict_answer,
            "loop_count": loop_count,
            "empty_list_len": empty_list_len,
            "empty_dict_len": empty_dict_len,
            "list_sum": list_sum,
            "list_item_value": list_item_value,
            "has_answer_key": has_answer_key,
            "has_missing_key": has_missing_key,
            "list_dup_len": list_dup_len,
            "merged_dict_len": merged_dict_len,
            "list_has_two": list_has_two,
            "list_has_nine": list_has_nine,
            "list_has_nested": list_has_nested,
            "typed_list_len": typed_list_len,
            "typed_list_sum": typed_list_sum,
        }

    if mode == "bone_read":
        # The default bone runs from its head at z=0 to its tail at z=1.
        bone_length = float(owner.get("bone_length", -1.0))
        bone_head_z = float(owner.get("bone_head_z", -1.0))
        bone_center_z = float(owner.get("bone_center_z", -1.0))
        ok = (
            abs(bone_length - 1.0) <= tolerance
            and abs(bone_head_z - 0.0) <= tolerance
            and abs(bone_center_z - 0.5) <= tolerance
        )
        return ok, {
            "mode": mode,
            "bone_length": bone_length,
            "bone_head_z": bone_head_z,
            "bone_center_z": bone_center_z,
        }

    if mode == "bone_parent":
        scene_objects = tuple(bge.logic.getCurrentScene().objects)
        child_name = owner.get("__ln_acceptance_child", "")
        parent_name = owner.get("__ln_acceptance_parent", "")
        child = next(
            (game_object for game_object in scene_objects if game_object.name == child_name),
            None,
        )
        expected = expected_named_vector(owner, "bone_parent")
        observed = vector_tuple(child.worldPosition) if child is not None else (0.0, 0.0, 0.0)
        parented = (
            child is not None
            and getattr(child, "parent", None) is not None
            and child.parent.name == parent_name
        )
        return parented and close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "parented": parented,
            "observed": observed,
            "expected": expected,
        }

    if mode == "compound_parent":
        scene_objects = tuple(bge.logic.getCurrentScene().objects)
        child_name = owner.get("__ln_acceptance_child", "")
        parent_name = owner.get("__ln_acceptance_parent", "")
        child = next(
            (game_object for game_object in scene_objects if game_object.name == child_name),
            None,
        )
        parented = (
            child is not None
            and getattr(child, "parent", None) is not None
            and child.parent.name == parent_name
        )
        dynamics_suspended = bool(child.isSuspendDynamics) if child is not None else False
        hit_object = owner.rayCast((2.0, 0.0, -5.0), (2.0, 0.0, 5.0), 10.0)[0]
        compound_hit = (
            hit_object is not None and hit_object.name in {child_name, parent_name}
        )
        return parented and dynamics_suspended and compound_hit, {
            "mode": mode,
            "parented": parented,
            "dynamics_suspended": dynamics_suspended,
            "compound_hit": compound_hit,
            "hit_object": hit_object.name if hit_object is not None else "",
        }

    if mode == "bone_pose":
        def observed(prefix):
            return (
                float(owner.get(f"{prefix}_x", 0.0)),
                float(owner.get(f"{prefix}_y", 0.0)),
                float(owner.get(f"{prefix}_z", 0.0)),
            )

        reference_channel = observed("reference_channel")
        reference_native = observed("reference_native")
        reference_object = observed("reference_object")
        channel_rotation = observed("channel_rotation")
        channel_scale = observed("channel_scale")
        armature_rotation = observed("armature_rotation")
        world_rotation = observed("world_rotation")
        transform_location = observed("transform_location")
        transform_rotation = observed("transform_rotation")
        transform_scale = observed("transform_scale")
        offset_rest_head = observed("offset_rest_head")
        offset_pose_head = observed("offset_pose_head")
        offset_delta = tuple(
            offset_pose_head[index] - offset_rest_head[index] for index in range(3))

        ok = (
            close_tuple(reference_channel, (0.0, 0.0, 0.0), tolerance)
            and close_tuple(
                reference_native,
                expected_named_vector(owner, "reference_native"),
                tolerance,
            )
            and close_tuple(
                reference_object,
                expected_named_vector(owner, "reference_object"),
                tolerance,
            )
            and close_tuple(
                channel_rotation,
                expected_named_vector(owner, "channel_rotation"),
                tolerance,
            )
            and close_tuple(
                channel_scale,
                expected_named_vector(owner, "channel_scale"),
                tolerance,
            )
            and close_tuple(
                armature_rotation,
                expected_named_vector(owner, "armature_rotation"),
                tolerance,
            )
            and close_tuple(
                world_rotation,
                expected_named_vector(owner, "world_rotation"),
                tolerance,
            )
            and close_tuple(
                transform_location,
                expected_named_vector(owner, "transform_location"),
                tolerance,
            )
            and close_tuple(
                transform_rotation,
                expected_named_vector(owner, "transform_rotation"),
                tolerance,
            )
            and close_tuple(
                transform_scale,
                expected_named_vector(owner, "transform_scale"),
                tolerance,
            )
            and close_tuple(
                offset_delta,
                expected_named_vector(owner, "offset_delta"),
                tolerance,
            )
        )
        return ok, {
            "mode": mode,
            "reference_channel": reference_channel,
            "reference_native": reference_native,
            "reference_object": reference_object,
            "channel_rotation": channel_rotation,
            "channel_scale": channel_scale,
            "armature_rotation": armature_rotation,
            "world_rotation": world_rotation,
            "transform_location": transform_location,
            "transform_rotation": transform_rotation,
            "transform_scale": transform_scale,
            "offset_rest_head": offset_rest_head,
            "offset_pose_head": offset_pose_head,
            "offset_delta": offset_delta,
        }

    if mode == "phase6_events":
        event_ok = bool(owner.get("event_ok", False))
        return event_ok, {"mode": mode, "event_ok": event_ok}

    if mode == "phase6_move_toward":
        observed = vector_tuple(owner.worldPosition)
        min_x = float(owner.get("__ln_acceptance_min_x", 0.05))
        max_x = float(owner.get("__ln_acceptance_max_x", 1000.0))
        ok = min_x <= observed[0] <= max_x
        return ok, {
            "mode": mode,
            "observed": observed,
            "min_x": min_x,
            "max_x": max_x,
        }

    if mode == "phase6_copy_property":
        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_target", owner.name)
        target = next(
            (game_object for game_object in scene.objects if game_object.name == target_name),
            None,
        )
        copied = float(target.get("score", 0.0)) if target is not None else 0.0
        expected = float(owner.get("__ln_acceptance_expected_score", 11.0))
        return abs(copied - expected) <= tolerance, {
            "mode": mode,
            "copied": copied,
            "expected": expected,
            "target_found": target is not None,
        }

    if mode == "phase6_get_object_attribute":
        pos_x = float(owner.get("read_x", 0.0))
        pos_y = float(owner.get("read_y", 0.0))
        pos_z = float(owner.get("read_z", 0.0))
        expected = expected_vector(owner)
        observed = (pos_x, pos_y, pos_z)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "phase6_rotate_to":
        target = (1.0, 0.0, 0.0)
        position = vector_tuple(owner.worldPosition)
        to_target = (
            target[0] - position[0],
            target[1] - position[1],
            target[2] - position[2],
        )
        length = math.sqrt(sum(component * component for component in to_target))
        if length <= 1e-5:
            return False, {"mode": mode, "reason": "target_too_close"}
        to_target = tuple(component / length for component in to_target)
        orientation = owner.worldOrientation
        front_y = (
            float(orientation[0][1]),
            float(orientation[1][1]),
            float(orientation[2][1]),
        )
        dot = sum(to_target[index] * front_y[index] for index in range(3))
        min_dot = float(owner.get("__ln_acceptance_min_dot", 0.85))
        return dot >= min_dot, {
            "mode": mode,
            "dot": dot,
            "min_dot": min_dot,
            "front_y": front_y,
            "to_target": to_target,
        }

    if mode == "phase6_collision":
        collided = bool(owner.get("collided", False))
        collided_object_name = str(owner.get("collided_object_name", ""))
        expected_object_name = str(owner.get("__ln_acceptance_expected_collision_object", ""))
        object_name_ok = collided_object_name == expected_object_name
        point = (
            float(owner.get("collision_point_x", 0.0)),
            float(owner.get("collision_point_y", 0.0)),
            float(owner.get("collision_point_z", 0.0)),
        )
        normal = (
            float(owner.get("collision_normal_x", 0.0)),
            float(owner.get("collision_normal_y", 0.0)),
            float(owner.get("collision_normal_z", 0.0)),
        )
        point_ok = sum(abs(component) for component in point) > 0.0001
        normal_length = math.sqrt(sum(component * component for component in normal))
        normal_ok = 0.5 <= normal_length <= 1.5
        return collided and object_name_ok and point_ok and normal_ok, {
            "mode": mode,
            "collided": collided,
            "collided_object_name": collided_object_name,
            "expected_object_name": expected_object_name,
            "object_name_ok": object_name_ok,
            "point": point,
            "point_ok": point_ok,
            "normal": normal,
            "normal_length": normal_length,
            "normal_ok": normal_ok,
        }

    if mode == "phase6_objects_colliding":
        return bool(owner.get("colliding", False)), {
            "mode": mode,
            "colliding": bool(owner.get("colliding", False)),
        }

    if mode == "phase6_animation_status":
        action_playing = bool(owner.get("action_playing", False))
        lifecycle_started = bool(owner.get("action_lifecycle_started", False))
        lifecycle_completed = bool(owner.get("action_lifecycle_completed", False))
        lifecycle_interrupted = bool(owner.get("action_lifecycle_interrupted", False))
        lifecycle_failed = bool(owner.get("action_lifecycle_failed", False))
        interruption_observed = bool(owner.get("action_interruption_observed", False))
        failure_observed = bool(owner.get("action_failure_observed", False))
        lifecycle_frame = float(owner.getActionFrame(1))
        lifecycle_playing = bool(owner.isPlayingAction(1))
        return (
            action_playing
            and lifecycle_started
            and lifecycle_completed
            and not lifecycle_interrupted
            and not lifecycle_failed
            and interruption_observed
            and failure_observed
        ), {
            "mode": mode,
            "action_playing": action_playing,
            "lifecycle_started": lifecycle_started,
            "lifecycle_completed": lifecycle_completed,
            "lifecycle_interrupted": lifecycle_interrupted,
            "lifecycle_failed": lifecycle_failed,
            "interruption_observed": interruption_observed,
            "failure_observed": failure_observed,
            "lifecycle_frame": lifecycle_frame,
            "lifecycle_playing": lifecycle_playing,
        }

    if mode == "phase6_cross_scene_save":
        save_completed = bool(owner.get("save_completed", False))
        return save_completed, {
            "mode": mode,
            "save_completed": save_completed,
        }

    if mode == "phase6_cross_scene_load":
        current_scene = bge.logic.getCurrentScene().name
        expected_scene = str(owner.get("__ln_acceptance_expected_scene", ""))
        score = float(owner.get("score", 0.0))
        expected_score = float(owner.get("__ln_acceptance_expected_score", 0.0))
        load_completed = bool(owner.get("load_completed", False))
        return (
            current_scene == expected_scene
            and abs(score - expected_score) <= tolerance
            and load_completed
        ), {
            "mode": mode,
            "current_scene": current_scene,
            "expected_scene": expected_scene,
            "score": score,
            "expected_score": expected_score,
            "load_completed": load_completed,
        }

    if mode == "phase6_save_load_structure":
        import bpy

        object_name = str(owner.get("__ln_acceptance_structure_object", owner.name))
        modifier_name = str(owner.get("__ln_acceptance_structure_modifier", ""))
        input_identifier = str(owner.get("__ln_acceptance_structure_input", ""))
        output_identifier = str(owner.get("__ln_acceptance_structure_output", ""))
        transient_modifier = str(
            owner.get("__ln_acceptance_structure_transient_modifier", ""))
        removed_modifier = str(
            owner.get("__ln_acceptance_structure_removed_modifier", ""))
        expected_value = float(owner.get("__ln_acceptance_structure_saved_value", 2.25))
        internal_node_name = str(
            owner.get("__ln_acceptance_structure_internal_node", ""))
        internal_socket_name = str(
            owner.get("__ln_acceptance_structure_internal_socket", ""))
        expected_internal_value = float(
            owner.get("__ln_acceptance_structure_internal_saved_value", 0.2))
        compositor_tree_name = str(
            owner.get("__ln_acceptance_structure_compositor_tree", ""))
        compositor_node_name = str(
            owner.get("__ln_acceptance_structure_compositor_node", ""))
        compositor_socket_name = str(
            owner.get("__ln_acceptance_structure_compositor_socket", ""))
        expected_compositor_value = float(
            owner.get("__ln_acceptance_structure_compositor_saved_value", 12.5))
        runtime_target = next(
            (
                candidate
                for candidate in bge.logic.getCurrentScene().objects
                if candidate.name == object_name
            ),
            None,
        )
        obj = bpy.data.objects.get(object_name)
        modifier = obj.modifiers.get(modifier_name) if obj is not None else None
        modifier_names = [item.name for item in obj.modifiers] if obj is not None else []
        input_mode = None
        input_value = None
        output_attribute = None
        modifier_viewport_enabled = None
        internal_value = None
        internal_muted = None
        compositor_value = None
        compositor_muted = None
        compositor_root_name = None
        inspection_error = ""
        try:
            if modifier is None or modifier.type != "NODES":
                raise RuntimeError("the restored Geometry Nodes modifier is missing")
            input_state = getattr(modifier.properties.inputs, input_identifier)
            output_state = getattr(modifier.properties.outputs, output_identifier)
            input_mode = str(input_state.type)
            input_value = float(input_state.value)
            output_attribute = str(output_state.attribute_name)
            modifier_viewport_enabled = bool(modifier.show_viewport)
            internal_node = modifier.node_group.nodes.get(internal_node_name)
            if internal_node is None:
                raise RuntimeError("the restored internal Geometry node is missing")
            internal_value = float(
                internal_node.inputs[internal_socket_name].default_value)
            internal_muted = bool(internal_node.mute)
            compositor_tree = bpy.context.scene.compositing_node_group
            compositor_root_name = (
                compositor_tree.name if compositor_tree is not None else None)
            compositor_node = (
                compositor_tree.nodes.get(compositor_node_name)
                if compositor_tree is not None
                else None
            )
            if compositor_node is None:
                raise RuntimeError("the restored compositor node is missing")
            compositor_value = float(
                compositor_node.inputs[compositor_socket_name].default_value)
            compositor_muted = bool(compositor_node.mute)
        except Exception:
            inspection_error = traceback.format_exc()
        installed_tree_restored = bool(
            runtime_target is not None
            and runtime_target.get("installed_tree_restored", False))
        transient_tree_removed = bool(
            runtime_target is not None
            and not runtime_target.get("transient_tree_running", False))
        load_completed = bool(
            runtime_target is not None and runtime_target.get("load_completed", False))
        mutation_completed = bool(
            runtime_target is not None
            and runtime_target.get("structure_output_mutated", False))
        collision_mutation_completed = bool(
            runtime_target is not None
            and runtime_target.get("structure_collision_mutated", False))
        mutation_error = (
            str(runtime_target.get("structure_mutation_error", ""))
            if runtime_target is not None
            else "the runtime target is missing"
        )
        modifier_state_ok = (
            input_mode == "VALUE"
            and input_value is not None
            and abs(input_value - expected_value) <= tolerance
            and output_attribute == "saved_attr"
            and modifier_viewport_enabled is True
            and internal_value is not None
            and abs(internal_value - expected_internal_value) <= tolerance
            and internal_muted is False
            and modifier_names == [modifier_name, removed_modifier]
            and transient_modifier not in modifier_names
        )
        compositor_state_ok = (
            compositor_root_name == compositor_tree_name
            and compositor_value is not None
            and abs(compositor_value - expected_compositor_value) <= tolerance
            and compositor_muted is False
        )
        return (
            installed_tree_restored
            and transient_tree_removed
            and load_completed
            and mutation_completed
            and collision_mutation_completed
            and not mutation_error
            and not inspection_error
            and modifier_state_ok
            and compositor_state_ok
        ), {
            "mode": mode,
            "installed_tree_restored": installed_tree_restored,
            "transient_tree_removed": transient_tree_removed,
            "load_completed": load_completed,
            "mutation_completed": mutation_completed,
            "collision_mutation_completed": collision_mutation_completed,
            "mutation_error": mutation_error,
            "inspection_error": inspection_error,
            "modifier_names": modifier_names,
            "input_mode": input_mode,
            "input_value": input_value,
            "expected_value": expected_value,
            "output_attribute": output_attribute,
            "modifier_viewport_enabled": modifier_viewport_enabled,
            "internal_value": internal_value,
            "expected_internal_value": expected_internal_value,
            "internal_muted": internal_muted,
            "compositor_root_name": compositor_root_name,
            "compositor_value": compositor_value,
            "expected_compositor_value": expected_compositor_value,
            "compositor_muted": compositor_muted,
            "modifier_state_ok": modifier_state_ok,
            "compositor_state_ok": compositor_state_ok,
        }

    if mode in {"phase6_save_load_material", "phase6_save_load_material_fresh"}:
        import bpy

        target_name = str(owner.get("__ln_acceptance_material_object", owner.name))
        sibling_name = str(owner.get("__ln_acceptance_material_sibling", ""))
        expected_material = str(owner.get("__ln_acceptance_expected_material", ""))
        expected_sibling_material = str(
            owner.get("__ln_acceptance_expected_sibling_material", ""))
        expected_parameter = (
            float(owner.get("__ln_acceptance_material_r", 0.0)),
            float(owner.get("__ln_acceptance_material_g", 0.0)),
            float(owner.get("__ln_acceptance_material_b", 0.0)),
            float(owner.get("__ln_acceptance_material_a", 1.0)),
        )
        expected_shared_value = float(
            owner.get("__ln_acceptance_shared_material_value", 0.0))
        shared_node_name = str(owner.get("__ln_acceptance_shared_material_node", ""))
        shared_socket_name = str(owner.get("__ln_acceptance_shared_material_socket", ""))
        target = bpy.data.objects.get(target_name)
        sibling = bpy.data.objects.get(sibling_name)
        target_material = (
            target.material_slots[0].material.name
            if target is not None
            and len(target.material_slots) == 1
            and target.material_slots[0].material is not None
            else None
        )
        sibling_material = (
            sibling.material_slots[0].material.name
            if sibling is not None
            and len(sibling.material_slots) == 1
            and sibling.material_slots[0].material is not None
            else None
        )
        parameter_values = []
        if target is not None:
            for property_name in target.keys():
                if str(property_name).startswith("_LNMP_"):
                    value = target[property_name]
                    try:
                        parameter_values.append(tuple(float(value[index]) for index in range(4)))
                    except (IndexError, TypeError):
                        parameter_values.append(None)
        parameter_ok = any(
            value is not None and close_color(value, expected_parameter, tolerance)
            for value in parameter_values
        )
        shared_value = None
        material = bpy.data.materials.get(expected_material)
        node = (
            material.node_tree.nodes.get(shared_node_name)
            if material is not None and material.node_tree is not None
            else None
        )
        socket = node.inputs.get(shared_socket_name) if node is not None else None
        if socket is not None:
            shared_value = float(socket.default_value)
        shared_ok = (
            shared_value is not None
            and abs(shared_value - expected_shared_value) <= tolerance
        )
        load_completed = bool(owner.get("load_completed", False))
        return (
            target_material == expected_material
            and sibling_material == expected_sibling_material
            and parameter_ok
            and shared_ok
            and load_completed
        ), {
            "mode": mode,
            "target_material": target_material,
            "expected_material": expected_material,
            "sibling_material": sibling_material,
            "expected_sibling_material": expected_sibling_material,
            "parameter_values": parameter_values,
            "expected_parameter": expected_parameter,
            "parameter_ok": parameter_ok,
            "shared_value": shared_value,
            "expected_shared_value": expected_shared_value,
            "shared_ok": shared_ok,
            "load_completed": load_completed,
        }

    if mode == "phase6_save_load":
        import bpy

        subject_name = str(owner.get("__ln_acceptance_subject", ""))
        owner = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == subject_name),
            owner,
        )
        score = float(owner.get("score", 0.0))
        expected = float(owner.get("__ln_acceptance_expected_score", 42.0))
        transient_score = float(owner.get("transient_score", 0.0))
        tree_score = float(owner.get("tree_checkpoint_value", 0.0))
        expected_tree_score = float(owner.get("__ln_acceptance_expected_tree_score", 99.0))
        gravity_z = float(owner.get("gravity_checkpoint_z", 0.0))
        expected_gravity_z = float(owner.get("__ln_acceptance_expected_gravity_z", -3.0))
        position = vector_tuple(owner.worldPosition)
        position_ok = close_tuple(position, expected_vector(owner), tolerance)
        expected_parent_name = str(owner.get("__ln_acceptance_expected_parent", ""))
        parent = owner.parent
        hierarchy_ok = parent is not None and parent.name == expected_parent_name
        soft_name = str(owner.get("__ln_acceptance_soft_object", ""))
        soft_object = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == soft_name),
            None,
        )
        soft_position = vector_tuple(soft_object.worldPosition) if soft_object else None
        soft_color = color_tuple(soft_object.color) if soft_object else None
        soft_color_ok = soft_color is not None and close_color(
            soft_color, (0.2, 0.4, 0.6, 1.0), tolerance)
        load_completed = bool(owner.get("load_completed", False))
        physics_name = str(owner.get("__ln_acceptance_physics_object", ""))
        physics_object = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == physics_name),
            None,
        )
        dynamics_suspended = (
            bool(physics_object.isSuspendDynamics) if physics_object else None
        )
        reconstructed_name = str(owner.get("__ln_acceptance_reconstructed_object", ""))
        reconstructed_mesh = str(owner.get("__ln_acceptance_reconstructed_mesh", ""))
        reconstructed_runtime_objects = [
            candidate for candidate in bge.logic.getCurrentScene().objects
            if candidate.name == reconstructed_name
        ]
        reconstructed_physics_ok = (
            len(reconstructed_runtime_objects) == 1
            and reconstructed_runtime_objects[0].getPhysicsId() != 0
        )
        reconstructed_color = (
            color_tuple(reconstructed_runtime_objects[0].color)
            if len(reconstructed_runtime_objects) == 1 else None
        )
        reconstructed_color_ok = (
            reconstructed_color is not None
            and close_color(reconstructed_color, (1.0, 1.0, 1.0, 1.0), tolerance)
        )
        reconstructed_render_objects = [
            candidate for candidate in bpy.context.scene.objects
            if candidate.type == "MESH"
            and candidate.data is not None
            and candidate.data.name == reconstructed_mesh
        ]
        visible_reconstructed_render_objects = [
            candidate for candidate in reconstructed_render_objects
            if not candidate.hide_viewport
        ]
        authored_source_hidden = any(
            candidate.name == reconstructed_name and candidate.hide_viewport
            for candidate in reconstructed_render_objects
        )
        reconstruction_render_state_ok = (
            len(reconstructed_render_objects) == 2
            and len(visible_reconstructed_render_objects) == 1
            and authored_source_hidden
        )
        reconstructed_render_state = [
            {
                "name": candidate.name,
                "hidden": bool(candidate.hide_viewport),
                "color": color_tuple(candidate.color),
            }
            for candidate in reconstructed_render_objects
        ]
        return (
            abs(score - expected) <= tolerance
            and abs(transient_score - 99.0) <= tolerance
            and abs(tree_score - expected_tree_score) <= tolerance
            and abs(gravity_z - expected_gravity_z) <= tolerance
            and position_ok
            and hierarchy_ok
            and soft_color_ok
            and load_completed
            and dynamics_suspended is True
            and reconstructed_physics_ok
            and reconstructed_color_ok
            and reconstruction_render_state_ok
        ), {
            "mode": mode,
            "score": score,
            "expected": expected,
            "transient_score": transient_score,
            "tree_score": tree_score,
            "expected_tree_score": expected_tree_score,
            "gravity_z": gravity_z,
            "expected_gravity_z": expected_gravity_z,
            "position": position,
            "position_ok": position_ok,
            "parent": parent.name if parent is not None else None,
            "expected_parent": expected_parent_name,
            "hierarchy_ok": hierarchy_ok,
            "soft_position": soft_position,
            "soft_color": soft_color,
            "soft_color_ok": soft_color_ok,
            "load_completed": load_completed,
            "dynamics_suspended": dynamics_suspended,
            "reconstructed_runtime_count": len(reconstructed_runtime_objects),
            "reconstructed_physics_ok": reconstructed_physics_ok,
            "reconstructed_color": reconstructed_color,
            "reconstructed_color_ok": reconstructed_color_ok,
            "reconstructed_render_state": reconstructed_render_state,
            "reconstruction_render_state_ok": reconstruction_render_state_ok,
        }

    if mode == "phase6_save_load_existence":
        scene_objects = list(bge.logic.getCurrentScene().objects)
        authored_present = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "authored_present"
        ]
        authored_tombstones = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "authored_tombstone"
        ]
        persistent_spawns = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "persistent_spawn"
        ]
        transient_spawns = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "transient_spawn"
        ]
        present_position = (
            vector_tuple(authored_present[0].worldPosition)
            if len(authored_present) == 1 else None
        )
        spawn_position = (
            vector_tuple(persistent_spawns[0].worldPosition)
            if len(persistent_spawns) == 1 else None
        )
        present_position_ok = present_position is not None and close_tuple(
            present_position, (3.0, 4.0, 5.0), tolerance)
        present_values = (
            [vector_tuple(authored_present[0].get("spot", ())), authored_present[0].get("bag")]
            if len(authored_present) == 1 else None
        )
        spawn_position_ok = spawn_position is not None and close_tuple(
            spawn_position, (11.0, 12.0, 13.0), tolerance)
        load_completed = bool(owner.get("load_completed", False))
        return (
            len(authored_present) == 1
            and not authored_tombstones
            and len(persistent_spawns) == 1
            and len(transient_spawns) == 1
            and present_position_ok
            and present_values == [(1.0, 2.0, 3.0), [3, 5]]
            and spawn_position_ok
            and load_completed
        ), {
            "mode": mode,
            "authored_present_count": len(authored_present),
            "authored_tombstone_count": len(authored_tombstones),
            "persistent_spawn_count": len(persistent_spawns),
            "transient_spawn_count": len(transient_spawns),
            "present_position": present_position,
            "present_position_ok": present_position_ok,
            "present_values": present_values,
            "spawn_position": spawn_position,
            "spawn_position_ok": spawn_position_ok,
            "load_completed": load_completed,
        }

    if mode == "phase6_save_load_revision":
        subject_name = str(owner.get("__ln_acceptance_subject", ""))
        owner = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == subject_name),
            owner,
        )
        score = float(owner.get("score", 0.0))
        transient_score = float(owner.get("transient_score", 0.0))
        position = vector_tuple(owner.worldPosition)
        position_ok = close_tuple(position, expected_vector(owner), tolerance)
        current_name = str(owner.get("__ln_acceptance_current_object", ""))
        current_object_exists = any(
            candidate.name == current_name for candidate in bge.logic.getCurrentScene().objects
        )
        current_object = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == current_name),
            None,
        )
        current_position = vector_tuple(current_object.worldPosition) if current_object else None
        current_position_ok = current_position is not None and close_tuple(
            current_position, (9.0, 1.0, 2.0), tolerance)
        removed_name = str(owner.get("__ln_acceptance_removed_object", ""))
        removed_object_absent = not any(
            candidate.name == removed_name for candidate in bge.logic.getCurrentScene().objects
        )
        load_completed = bool(owner.get("load_completed", False))
        return (
            abs(score - 42.0) <= tolerance
            and abs(transient_score - 123.0) <= tolerance
            and position_ok
            and current_object_exists
            and current_position_ok
            and removed_object_absent
            and load_completed
        ), {
            "mode": mode,
            "score": score,
            "transient_score": transient_score,
            "position": position,
            "position_ok": position_ok,
            "current_object_exists": current_object_exists,
            "current_position": current_position,
            "current_position_ok": current_position_ok,
            "removed_object_absent": removed_object_absent,
            "load_completed": load_completed,
        }

    if mode == "phase6_align_axis":
        orientation = owner.worldOrientation
        local_z = (
            float(orientation[0][2]),
            float(orientation[1][2]),
            float(orientation[2][2]),
        )
        length = math.sqrt(sum(component * component for component in local_z))
        if length <= 1e-5:
            return False, {"mode": mode, "reason": "degenerate_orientation"}
        local_z = tuple(component / length for component in local_z)
        dot = local_z[0]
        min_dot = float(owner.get("__ln_acceptance_min_dot", 0.85))
        # Toward the target, not merely parallel: a spinning axis passes through both directions.
        return dot >= min_dot, {"mode": mode, "dot": dot, "min_dot": min_dot, "local_z": local_z}

    if mode == "phase6_slow_follow":
        observed = vector_tuple(owner.worldPosition)
        min_x = float(owner.get("__ln_acceptance_min_x", 0.5))
        return observed[0] >= min_x, {
            "mode": mode,
            "observed": observed,
            "min_x": min_x,
        }

    if mode == "phase6_replace_mesh":
        return bool(owner.get("mesh_replaced", False)), {
            "mode": mode,
            "mesh_replaced": bool(owner.get("mesh_replaced", False)),
        }

    if mode == "input_nodes":
        keyboard_ok = bool(owner.get("keyboard_checked", False))
        mouse_ok = bool(owner.get("mouse_checked", False))
        gamepad_ok = bool(owner.get("gamepad_checked", False))
        return keyboard_ok and mouse_ok and gamepad_ok, {
            "mode": mode,
            "keyboard_checked": keyboard_ok,
            "mouse_checked": mouse_ok,
            "gamepad_checked": gamepad_ok,
        }

    if mode == "values_math":
        result = float(owner.get("math_result", 0.0))
        expected = float(owner.get("__ln_acceptance_expected_math", 5.0))
        curve_result = float(owner.get("float_curve_result", 0.0))
        curve_expected = float(owner.get("__ln_acceptance_expected_float_curve", 0.5))
        ok = (
            abs(result - expected) <= tolerance
            and abs(curve_result - curve_expected) <= tolerance
        )
        return ok, {
            "mode": mode,
            "result": result,
            "expected": expected,
            "curve_result": curve_result,
            "curve_expected": curve_expected,
        }

    if mode == "c_tier_values":
        vector4_text = owner.get("vector4_text", "")
        resize4_text = owner.get("resize4_text", "")
        matrix_euler_text = owner.get("matrix_euler_text", "")
        ok = (
            "4.5" in vector4_text
            and "4.5" in resize4_text
            and "0.25" in matrix_euler_text
            and "-0.5" in matrix_euler_text
            and "0.75" in matrix_euler_text
        )
        return ok, {
            "mode": mode,
            "vector4_text": vector4_text,
            "resize4_text": resize4_text,
            "matrix_euler_text": matrix_euler_text,
        }

    if mode == "b_tier_core":
        formula_result = float(owner.get("formula_result", 0.0))
        eval_ok = bool(owner.get("eval_ok", False))
        tween_result = float(owner.get("tween_result", 0.0))
        projectile_hit = bool(owner.get("projectile_hit", False))
        expected_formula = abs(-2.0) + math.pi + math.e
        ok = (
            abs(formula_result - expected_formula) <= tolerance
            and eval_ok
            and tween_result > 0.01
            and projectile_hit
        )
        return ok, {
            "mode": mode,
            "formula_result": formula_result,
            "eval_ok": eval_ok,
            "tween_result": tween_result,
            "projectile_hit": projectile_hit,
        }

    if mode == "sound_state":
        return bool(owner.get("sound_stopped", False)), {
            "mode": mode,
            "sound_stopped": bool(owner.get("sound_stopped", False)),
        }

    if mode == "sound_advanced":
        return bool(owner.get("sound_advanced_ok", False)), {
            "mode": mode,
            "sound_advanced_ok": bool(owner.get("sound_advanced_ok", False)),
        }

    if mode == "vehicle_nodes_ok":
        return bool(owner.get("vehicle_nodes_ok", False)), {
            "mode": mode,
            "vehicle_nodes_ok": bool(owner.get("vehicle_nodes_ok", False)),
        }

    if mode == "material_shader_input":
        material_name = owner.get("__ln_acceptance_material", "")
        try:
            import bpy
            material = bpy.data.materials.get(material_name)
            mix = material.node_tree.nodes.get("Mix") if material and material.node_tree else None
            factor_socket = None
            if mix is not None:
                for socket in mix.inputs:
                    if socket.identifier == "Factor_Float":
                        factor_socket = socket
                        break
            factor = float(factor_socket.default_value) if factor_socket is not None else 0.0
        except Exception as exc:
            return False, {"mode": mode, "material": material_name, "error": repr(exc)}

        return abs(factor - 1.0) <= tolerance, {
            "mode": mode,
            "material": material_name,
            "factor": factor,
        }

    if mode == "loop_routes":
        counts = {key: int(owner.get(key, -1)) for key in (
            "c_other", "c_direct", "c_append", "c_inventory", "c_branch")}
        expected = {"c_other": 1, "c_direct": 3, "c_append": 3, "c_inventory": 3, "c_branch": 3}
        return counts == expected, {"mode": mode, **counts}

    if mode == "inventory_rule_sets":
        # Both bags follow the set "Bag" (no stacking, 2 places, or the object's bag_size); the
        # Override call stacks all 3 Apples; the weapon list refuses the Apple from Add Item and
        # from Move To Slot, and takes the Sword.
        numbers = {key: int(owner.get(key, -1)) for key in (
            "target_left", "other_left", "pouch_left", "refused_left", "sword_left")}
        expected_numbers = {"target_left": 1, "other_left": 0, "pouch_left": 0,
                            "refused_left": 1, "sword_left": 0}
        texts = {key: str(owner.get(key, "")) for key in ("refused_outcome", "move_outcome")}
        expected_texts = {"refused_outcome": "Refused", "move_outcome": "Refused"}
        scene = bge.logic.getCurrentScene()
        other = scene.objects.get("LN_Acceptance_RulesOther")

        def length(obj, name):
            value = obj.get(name, None) if obj is not None else None
            return len(value) if value is not None else -1

        lengths = {"inventory": length(owner, "inventory"), "pouch": length(owner, "pouch"),
                   "weapon": length(owner, "weapon"), "other": length(other, "inventory")}
        expected_lengths = {"inventory": 2, "pouch": 1, "weapon": 1, "other": 3}
        ok = (bool(owner.get("rules_done", False)) and numbers == expected_numbers and
              texts == expected_texts and lengths == expected_lengths)
        return ok, {"mode": mode, **numbers, **texts, **lengths}

    if mode == "inventory_in_place":
        # Two adds in one tick: the second sees the first (in-place edits); the scene's Item
        # Library caps a Coin stack at 6; a pickup object adds its own item and count; Amount 0
        # removes a whole slot; Add Item's Added says how many went in (7 of 9 Gems into the 3,
        # Capacity 2 leaves no new slot).
        numbers = {key: int(owner.get(key, -1)) for key in (
            "leftover2", "removed_whole", "coins", "gems", "added2", "added_pickup",
            "added_partial", "leftover_partial")}
        expected_numbers = {"leftover2": 0, "removed_whole": 6, "coins": 2, "gems": 3,
                            "added2": 4, "added_pickup": 3, "added_partial": 7,
                            "leftover_partial": 2}
        inventory = owner.get("inventory", None)
        slots = len(inventory) if inventory is not None else -1
        scene = bge.logic.getCurrentScene()

        def drops(name, x):
            return [obj for obj in scene.objects
                    if obj.name == name and abs(float(obj.worldPosition.x) - x) < 0.01]

        gem_drops = drops("Gem", 5.0)
        coin_drops = drops("Coin", 7.0)
        gem_ok = len(gem_drops) == 1 and int(gem_drops[0].get("count", -1)) == 2 and str(
            gem_drops[0].get("item", "")) == "Gem"
        coin_ok = len(coin_drops) == 1 and int(coin_drops[0].get("count", -1)) == 3
        ok = (bool(owner.get("inventory_done", False)) and numbers == expected_numbers and
              slots == 2 and gem_ok and coin_ok)
        return ok, {"mode": mode, **numbers, "slots": slots, "gem_drops": len(gem_drops),
                    "coin_drops": len(coin_drops), "gem_ok": gem_ok, "coin_ok": coin_ok}

    if mode == "inventory_two_inventories":
        # Move To Slot into another inventory: onto the same item it stacks up to the limit, onto an
        # empty place the whole stack moves, and To Object empty means the same object (an
        # equipment property with Capacity 1).
        scene = bge.logic.getCurrentScene()
        chest = scene.objects.get("LN_Acceptance_TwoChest")
        texts = {key: str(owner.get(key, "")) for key in ("o_stack", "o_move", "o_equip", "o_full")}
        numbers = {key: int(owner.get(key, -1)) for key in (
            "bag_apples", "bag_slots", "chest_apples", "chest_swords", "weapon_apples")}
        expected_texts = {"o_stack": "Stacked", "o_move": "Moved", "o_equip": "Moved",
                          "o_full": "No Room"}
        expected_numbers = {"bag_apples": 0, "bag_slots": 1, "chest_apples": 5, "chest_swords": 1,
                            "weapon_apples": 2}
        chest_slots = len(chest.get("chest", [])) if chest is not None else -1
        ok = (bool(owner.get("two_done", False)) and texts == expected_texts and
              numbers == expected_numbers and chest_slots == 2)
        return ok, {"mode": mode, **texts, **numbers, "chest_slots": chest_slots}

    if mode == "ui_menu_replace_inside":
        # A Replace Top issued by a tree inside the menu being replaced (a tab button): the new
        # menu stays open, it is not closed with the old menu's tree.
        scene = bge.logic.getCurrentScene()
        names = set(obj.name for obj in scene.objects)
        details = {
            "mode": mode,
            "a_open": "LN_Acceptance_RTAPanel" in names,
            "b_open": "LN_Acceptance_RTBPanel" in names,
            "b_seen": bool(owner.get("b_seen", False)),
        }
        ok = details["b_open"] and not details["a_open"]
        return ok, details

    if mode == "ui_player_menu":
        # A menu opened for player 1: its first button is player 1's focus and highlight (the
        # Highlight event reports User 1), its Highlight Object shows, player 0 has no focus, and
        # text shows each player's button names.
        # The menu's objects are copies with their own text: read the live game objects.
        scene = bge.logic.getCurrentScene()
        glow = scene.objects.get("LN_Acceptance_PMGlow")
        other_glow = scene.objects.get("LN_Acceptance_PMOtherGlow")
        menu_hint = scene.objects.get("LN_Acceptance_PMHint")
        game_hint = scene.objects.get("LN_Acceptance_PMGameHint")
        details = {
            "mode": mode,
            "hl_user": int(owner.get("hl_user", -1)),
            "hl_action": str(owner.get("hl_action", "")),
            "focus1": str(owner.get("focus1", "")),
            "focus0": str(owner.get("focus0", "?")),
            "glow_visible": bool(glow.visible) if glow is not None else None,
            "other_glow_visible": bool(other_glow.visible) if other_glow is not None else None,
            "menu_hint": menu_hint.blenderObject.data.body if menu_hint is not None else None,
            "game_hint": game_hint.blenderObject.data.body if game_hint is not None else None,
        }
        ok = (details["hl_user"] == 1 and details["hl_action"] == "first" and
              details["focus1"] == "LN_Acceptance_PMFirst" and
              details["focus0"] in ("", "None") and
              details["glow_visible"] is True and details["other_glow_visible"] is False and
              details["menu_hint"] == "A ok" and details["game_hint"] == "Return ok")
        return ok, details

    if mode == "ui_grid_inventory":
        # The grids read the lists their Object and Property name (the bag: whose menu it is),
        # and the shared tooltip of the slot player 1 highlights shows its item's words beside
        # it: (0, 1, 0.5) in the space of the panel it hangs from, scaled (4, 2, 0.1).
        scene = bge.logic.getCurrentScene()
        tip = scene.objects.get("LN_Acceptance_GITip")
        slots = [ob for ob in scene.objects if ob.name == "LN_Acceptance_GISlot"]
        slot = min(slots, key=lambda ob: ob.worldPosition.x) if slots else None
        offset = (list(tip.worldPosition - slot.worldPosition)
                  if tip is not None and slot is not None else None)
        details = {
            "mode": mode,
            "bag_last": int(owner.get("bag_last", -1)),
            "chest_pages": int(owner.get("chest_pages", -1)),
            "slot_copies": len(slots),
            "tip_visible": bool(tip.visible) if tip is not None else None,
            "tip_text": str(tip.get("Text", "")) if tip is not None else None,
            "tip_body": tip.blenderObject.data.body if tip is not None else None,
            "tip_offset": [round(value, 3) for value in offset] if offset else None,
            "outcome": str(owner.get("outcome", "")),
        }
        ok = (details["bag_last"] == 1 and details["chest_pages"] == 2 and
              details["slot_copies"] == 2 and details["tip_visible"] is True and
              details["tip_text"] == "Red Apple: Crunchy x3" and
              details["tip_body"] == "Red Apple: Crunchy x3" and offset is not None and
              abs(offset[0]) < 1.0e-3 and abs(offset[1] - 2.0) < 1.0e-3 and
              abs(offset[2] - 0.05) < 1.0e-3 and details["outcome"] == "")
        return ok, details

    if mode == "inventory_recipes":
        texts = {key: str(owner.get(key, "")) for key in (
            "move_outcome", "move_made", "craft_outcome", "missing_outcome", "stack_outcome",
            "station")}
        numbers = {key: int(owner.get(key, -1)) for key in (
            "potions", "keys", "coins", "stacked_first", "arrows", "found_index",
            "typed_arrows", "crafted_times", "craftable", "byproducts", "buckets",
            "tool_butter")}
        expected_texts = {"move_outcome": "Combined", "move_made": "Potion",
                          "craft_outcome": "Crafted", "missing_outcome": "Missing",
                          "stack_outcome": "Stacked", "station": "Bench"}
        expected_numbers = {"potions": 1, "keys": 1, "coins": 4, "stacked_first": 6,
                            "arrows": 3, "found_index": 1, "typed_arrows": 3,
                            "crafted_times": 3, "craftable": 3, "byproducts": 1, "buckets": 1,
                            "tool_butter": 5}
        ok = (bool(owner.get("recipes_done", False)) and texts == expected_texts and
              numbers == expected_numbers and bool(owner.get("can_craft", False)))
        return ok, {"mode": mode, **texts, **numbers,
                    "can_craft": bool(owner.get("can_craft", False))}

    if mode == "input_map":
        flags = {
            key: bool(owner.get(key, False))
            for key in ("saved", "loaded", "reset", "missing_done", "rebind_done")
        }
        texts = {key: str(owner.get(key, "")) for key in ("jump_key", "jump_button", "accept_auto")}
        file_path = str(owner.get("__ln_acceptance_expected_path", ""))
        file_ok = bool(file_path) and os.path.isfile(file_path)
        ok = (all(flags.values()) and not owner.get("missing_loaded", True) and
              not owner.get("rebind_success", True) and not owner.get("jump_active", True) and
              not owner.get("jump_triggered", False) and texts["jump_key"] == "Space Bar" and
              texts["jump_button"] == "A" and texts["accept_auto"] == "Return" and file_ok)
        return ok, {
            "mode": mode,
            **flags,
            **texts,
            "missing_loaded": owner.get("missing_loaded", None),
            "rebind_success": owner.get("rebind_success", None),
            "jump_active": owner.get("jump_active", None),
            "jump_triggered": owner.get("jump_triggered", None),
            "file_ok": file_ok,
        }

    if mode == "time_state":
        keys = ("next_frame", "delay", "timer", "pulsify", "barrier")
        values = {key: bool(owner.get(key, False)) for key in keys}
        time_value = float(owner.get("time_value", 0.0))
        delta_value = float(owner.get("delta_value", 0.0))
        time_ok = time_value >= 0.05
        delta_ok = 0.01 <= delta_value <= 0.025
        return all(values.values()) and time_ok and delta_ok, {
            "mode": mode,
            **values,
            "time_value": time_value,
            "time_ok": time_ok,
            "delta_value": delta_value,
            "delta_ok": delta_ok,
        }

    if mode == "scene_state":
        scene = bge.logic.getCurrentScene()
        observed_gravity = vector_tuple(scene.gravity)
        property_gravity = (
            float(owner.get("gravity_x", 0.0)),
            float(owner.get("gravity_y", 0.0)),
            float(owner.get("gravity_z", 0.0)),
        )
        observed_timescale = float(bge.logic.getTimeScale())
        property_timescale = float(owner.get("timescale", 0.0))
        expected_timescale = float(owner.get("__ln_acceptance_expected_timescale", 1.0))
        ok = (
            close_tuple(observed_gravity, expected, tolerance)
            and close_tuple(property_gravity, expected, tolerance)
            and abs(observed_timescale - expected_timescale) <= tolerance
            and abs(property_timescale - expected_timescale) <= tolerance
        )
        return ok, {
            "mode": mode,
            "observed_gravity": observed_gravity,
            "property_gravity": property_gravity,
            "expected_gravity": expected,
            "observed_timescale": observed_timescale,
            "property_timescale": property_timescale,
            "expected_timescale": expected_timescale,
        }

    if mode == "camera_state":
        scene = bge.logic.getCurrentScene()
        active_camera = getattr(scene, "active_camera", None)
        observed_name = active_camera.name if active_camera is not None else ""
        observed_fov = float(getattr(active_camera, "fov", 0.0)) if active_camera is not None else 0.0
        observed_ortho_scale = (
            float(getattr(active_camera, "ortho_scale", 0.0)) if active_camera is not None else 0.0)
        expected_name = owner.get("__ln_acceptance_expected_active_camera", "")
        expected_fov = float(owner.get("__ln_acceptance_expected_fov", 0.0))
        expected_ortho_scale = float(owner.get("__ln_acceptance_expected_ortho_scale", 0.0))
        expected_screen = (
            float(owner.get("__ln_acceptance_expected_screen_x", 0.5)),
            float(owner.get("__ln_acceptance_expected_screen_y", 0.5)),
        )
        expected_depth = float(owner.get("__ln_acceptance_expected_depth", 0.0))
        observed_screen = (
            float(owner.get("screen_x", 0.0)),
            float(owner.get("screen_y", 0.0)),
        )
        observed_world = (
            float(owner.get("world_x", 0.0)),
            float(owner.get("world_y", 0.0)),
            float(owner.get("world_z", 0.0)),
        )
        expected_world = (
            vector_tuple(active_camera.worldPosition + active_camera.getScreenVect(
                expected_screen[0], expected_screen[1]) * -expected_depth)
            if active_camera is not None else (0.0, 0.0, 0.0)
        )
        active_camera_present = bool(owner.get("active_camera_present", False))
        ok = (
            active_camera is not None
            and active_camera_present
            and observed_name == expected_name
            and abs(observed_fov - expected_fov) <= tolerance
            and abs(observed_ortho_scale - expected_ortho_scale) <= tolerance
            and abs(observed_screen[0] - expected_screen[0]) <= tolerance
            and abs(observed_screen[1] - expected_screen[1]) <= tolerance
            and close_tuple(observed_world, expected_world, tolerance)
        )
        return ok, {
            "mode": mode,
            "observed_name": observed_name,
            "expected_name": expected_name,
            "observed_fov": observed_fov,
            "expected_fov": expected_fov,
            "observed_ortho_scale": observed_ortho_scale,
            "expected_ortho_scale": expected_ortho_scale,
            "observed_screen": observed_screen,
            "expected_screen": expected_screen,
            "observed_world": observed_world,
            "expected_world": expected_world,
            "active_camera_present": active_camera_present,
        }

    if mode == "lod_view_size":
        scene = bge.logic.getCurrentScene()
        rock = scene.objects.get(str(owner.get("__ln_acceptance_rock", "")))
        shown = rock.meshes[0].name if rock is not None and rock.meshes else ""
        expected = str(owner.get("__ln_acceptance_mesh", ""))
        return expected != "" and shown == expected, {
            "mode": mode, "shown": shown, "expected": expected}

    if mode == "ui_view_center":
        # View Center: player 1 (camera inside their own body) hovered the Any Player button at
        # once, player 2 only once the wall in front went away (through the box their camera
        # does not draw); player 1's menu stopped their pointer (Exit). Only in Menus showed the
        # cursor only while that menu was open.
        samples = bge.logic.globalDict.get("vc_samples", {})
        expected = {"before": [False, True, False, False], "menu": [True, True, True],
                    "after": [False, True, True, True]}
        return samples == expected, {"mode": mode, "samples": samples, "expected": expected}

    if mode == "ui_take_control_pointer":
        # Take UI Control: player 1 gets the computer, player 2 is refused (Result 20) until
        # player 1 gives it back. Set Player Pointer placed player 1's cursor in the middle of the
        # screen, in front of its button, shown; turned off, it is hidden.
        observed = {name: str(owner.get("tc_" + name, "")) for name in (
            "take1", "take2", "release", "take2_after")}
        expected = {"take1": "done", "take2": "failed", "release": "done",
                    "take2_after": "done"}
        result = int(owner.get("tc_take2_result", -1))
        mid = bge.logic.globalDict.get("tc_mid")
        cursor = next((obj for obj in bge.logic.getCurrentScene().objects
                       if obj.name.endswith("_TCCursor")), None)
        placed = bool(mid) and abs(mid[0]) < 1.0e-3 and abs(mid[2] - 1.5) < 1.0e-3 and \
            -0.05 < mid[1] < -0.01 and mid[3]
        hidden_after = cursor is not None and not cursor.visible
        ok = observed == expected and result == 20 and placed and hidden_after
        return ok, {"mode": mode, "observed": observed, "expected": expected,
                    "take2_result": result, "cursor_mid": mid, "hidden_after": hidden_after}

    if mode == "ui_element_players":
        # Set UI Focus reached only the players who use each element: Player 1's button is
        # player 1's, Any Player is everyone's, except for a player whose camera does not draw
        # it (Set Camera Visibility).
        expected = {"p1_user1": "done", "p1_user0": "failed", "any_user2": "done",
                    "hidden_user1": "failed", "hidden_user0": "done"}
        observed = {name: str(owner.get("focus_" + name, "")) for name in expected}
        return observed == expected, {"mode": mode, "observed": observed, "expected": expected}

    if mode == "lod_nearest_view":
        # Each rock shows the mesh for its nearest player view: full detail near player 0 or
        # player 1, the middle one 33 m from player 0, the far one away from both (not the far
        # mesh everywhere, as the menu camera drawn last used to leave them).
        scene = bge.logic.getCurrentScene()
        shown = {}
        for name in ("Near0", "Near1", "Mid", "Far"):
            rock = next((obj for obj in scene.objects if obj.name.endswith(f"_Lod{name}")), None)
            shown[name] = rock.meshes[0].name if rock is not None and rock.meshes else ""
        expected = {
            "Near0": str(owner.get("__ln_acceptance_mesh_Near0", "")),
            "Near1": str(owner.get("__ln_acceptance_mesh_Near1", "")),
            "Mid": str(owner.get("__ln_acceptance_mesh_low", "")),
            "Far": str(owner.get("__ln_acceptance_mesh_far", "")),
        }
        ok = all(expected[name] != "" and shown[name] == expected[name] for name in expected)
        return ok, {"mode": mode, "shown": shown, "expected": expected}

    if mode == "camera_renderers":
        # The views drew, stopped long enough for their renderers to be given back, and drew
        # again (shared renderer, Workbench renderer, idle freeing and re-creation).
        scene = bge.logic.getCurrentScene()
        names = [str(owner.get("__ln_acceptance_cr_" + key, "")) for key in ("a", "b", "w")]
        back_on = all(scene.objects.get(name) is not None and scene.objects[name].useViewport
                      for name in names)
        details = {
            "mode": mode,
            "phase": int(owner.get("cr_phase", 0)),
            "frames_on": int(owner.get("cr_frames_on", 0)),
            "frames_off": int(owner.get("cr_frames_off", 0)),
            "frames_back": int(owner.get("cr_frames_back", 0)),
            "back_on": back_on,
            "error": str(owner.get("cr_error", "")),
        }
        ok = (details["phase"] == 4 and details["frames_on"] >= 3 and details["frames_off"] >= 3
              and details["frames_back"] >= 5 and back_on and not details["error"])
        return ok, details

    if mode == "camera_minimap_options":
        # Each small view's pixels in a screenshot by color (drive_camera_minimap_options): Only
        # shows the markers (green sphere, red Add Object copy from a hidden child collection)
        # without the blue ground, Hide the opposite; Material colors are the materials' (yellow
        # markers, gray ground); Outline and Cavity change their views; the round view keeps the
        # main view in its corners and has a smooth edge.
        result = bge.logic.globalDict.get("mo_result", {})
        counts = result.get("counts", {})

        def count(key, name):
            return int(counts.get(key, {}).get(name, 0))

        def pixels(key):
            return max(1, sum(int(value) for value in counts.get(key, {}).values()))

        checks = {
            # Yellow: the collection instance's copy (its Empty is in the markers' collection).
            "only_markers": (count("only", "green") > 0 and count("only", "red") > 0
                             and count("only", "yellow") > 0),
            "only_no_ground": count("only", "blue") == 0 and count("only", "black") > 0,
            "hide_no_markers": (count("hide", "green") == 0 and count("hide", "red") == 0
                                and count("hide", "yellow") == 0),
            "hide_ground": count("hide", "blue") > 0,
            # The cyan cube Set Collection Member put in the markers' collection.
            "only_joined": count("only", "cyan") > 0 and count("round", "cyan") > 0,
            "hide_no_joined": count("hide", "cyan") == 0,
            "plain_joined": count("plain", "cyan") > 0,
            "material_colors": (count("material", "yellow") > 0 and count("material", "gray") > 0
                                and count("material", "green") == 0
                                and count("material", "red") == 0
                                and count("material", "blue") == 0),
            "plain_object_colors": (count("plain", "blue") > 0 and count("plain", "green") > 0
                                    and count("plain", "red") > 0
                                    and count("plain", "yellow") > 0),
            "outline": int(result.get("outline_redder", 0)) > 0,
            "cavity": int(result.get("cavity_changed", 0)) > 0,
            "cavity_screen": int(result.get("screen_changed", 0)) > 0,
            # Corners as the main view next to them, and not the minimap's black.
            "round_corners": (len(result.get("round_corner_diffs", [])) == 4
                              and max(result["round_corner_diffs"]) <= 24
                              and min(result["round_corner_sums"]) > 60),
            "round_middle": result.get("round_middle") == "black",
            # A smooth edge: a good part of the edge pixels are in between.
            "round_edge": (len(result.get("round_edge", [])) == 2
                           and result["round_edge"][0] * 4 >= result["round_edge"][1] > 0),
            # Lighting (2026-10-15): Studio shades the markers; MatCap draws the scene's red wax
            # MatCap on every marker (Single color), which Studio's gray would not give. Shares of
            # a view's pixels: the player's window is small (the sphere is about 4%).
            "studio": int(result.get("studio_changed", 0)) > 0.02 * pixels("studio"),
            "matcap": (count("matcap", "red") > 0.02 * pixels("matcap")
                       and count("matcap", "green") == 0 and count("matcap", "yellow") == 0
                       and count("matcap", "cyan") == 0),
            # Attribute: the sphere's magenta color attribute instead of its green object color.
            "attribute": (count("attribute", "magenta") > 0.02 * pixels("attribute")
                          and count("attribute", "green") == 0),
            # Random: a color per object name; the blue ground changes.
            "random": float(result.get("random_changed", 0.0)) > 0.5,
        }
        details = {"mode": mode, "checks": checks, "result": result,
                   "phase": int(owner.get("mo_phase", 0)), "error": str(owner.get("mo_error", ""))}
        return all(checks.values()) and details["phase"] == 3 and not details["error"], details

    if mode == "camera_visibility_still":
        # Nothing moves in the scene: the top camera's renderer has finished its samples when
        # Set Camera Visibility hides the red cube from it (drive_camera_visibility_still). It
        # kept showing its last picture, with the cube, until Blender was told (2026-10-25).
        result = bge.logic.globalDict.get("cvs_result", {})
        checks = {
            "cube_drawn_before": int(result.get("before", 0)) > 50,
            "cube_hidden_after": int(result.get("after", -1)) == 0,
        }
        details = {"mode": mode, "checks": checks, "result": result,
                   "phase": int(owner.get("cvs_phase", 0)),
                   "error": str(owner.get("cvs_error", ""))}
        return all(checks.values()) and details["phase"] == 4 and not details["error"], details

    if mode == "material_actions":
        # The owner played a material action (On Init): only it has a copy of the material, with
        # the action's last color; the other cube keeps the shared material. drive_material_actions
        # played it on an Add Object copy too, then ended the copy.
        import bpy

        scene = bge.logic.getCurrentScene()
        source = str(owner.get("__ln_acceptance_ma_material", ""))
        other = scene.objects.get(str(owner.get("__ln_acceptance_ma_other", "")))
        glow = (1.0, 0.5, 0.0)
        rest = (0.2, 0.2, 0.2)

        def material_of(obj):
            slot = obj.blenderObject.material_slots[0] if obj is not None else None
            material = slot.material if slot is not None else None
            if material is None:
                return "", (9.0, 9.0, 9.0)
            return material.name, tuple(float(v) for v in material.diffuse_color[:3])

        mine, mine_rgb = material_of(owner)
        theirs, theirs_rgb = material_of(other)
        source_rgb = (tuple(float(v) for v in bpy.data.materials[source].diffuse_color[:3])
                      if source in bpy.data.materials else (9.0, 9.0, 9.0))
        spawned_rg = (float(owner.get("ma_spawned_r", 9.0)), float(owner.get("ma_spawned_g", 9.0)))
        checks = {
            "own_copy": mine not in ("", source),
            "own_glow": close_tuple(mine_rgb, glow, 0.01),
            "other_shared": theirs == source,
            "other_rest": close_tuple(theirs_rgb, rest, 0.01),
            "source_rest": close_tuple(source_rgb, rest, 0.01),
            "spawn_shared_before": str(owner.get("ma_spawn_before", "")) == source,
            "spawn_own_copy": str(owner.get("ma_spawned_material", "")) not in ("", source),
            "spawn_glow": abs(spawned_rg[0] - glow[0]) <= 0.01 and abs(spawned_rg[1] - glow[1]) <= 0.01,
            "spawn_copy_gone": bool(owner.get("ma_spawned_copy_gone", False)),
        }
        return all(checks.values()), {
            "mode": mode, "checks": checks, "mine": mine, "mine_rgb": list(mine_rgb),
            "theirs": theirs, "theirs_rgb": list(theirs_rgb), "spawned_rg": list(spawned_rg),
            "spawned_material": str(owner.get("ma_spawned_material", "")),
            "phase": int(owner.get("ma_phase", 0)),
        }

    if mode == "collection_membership":
        # What drive_collection_membership recorded after each step: Get Collection Objects
        # without and with child collections, the collection linked into List Length, and the
        # objects Loop From List went through.
        store = bge.logic.globalDict
        names = {key: str(owner.get("__ln_acceptance_cm_" + key, ""))
                 for key in ("ring", "icon", "blip", "marker", "marker_child", "enemy", "tree",
                             "instancer", "enemy_nest", "friend_nest")}

        def recorded(step):
            record = store.get("cm_" + step, {})
            return (record.get("direct"), record.get("all"), record.get("linked"),
                    record.get("seen"))

        def expected(direct, all_count, *keys):
            return (direct, all_count, all_count, sorted(names[key] for key in keys))

        hidden = store.get("cm_hidden", {})
        joined = store.get("cm_joined", {})
        checks = {
            "start": recorded("start") == expected(1, 2, "ring", "icon"),
            # The copy is in the hidden child collection its template is in.
            "spawned": recorded("spawned") == expected(1, 3, "ring", "icon", "blip"),
            # The marker and its child, put in by one Set Collection Member.
            "added": recorded("added") == expected(
                3, 5, "ring", "icon", "blip", "marker", "marker_child"),
            # Taken out of the collection, the icon is out of its child collections too.
            "removed": recorded("removed") == expected(
                3, 4, "ring", "blip", "marker", "marker_child"),
            "saved": store.get("cm_save_ok") is True,
            "undone": recorded("undone") == expected(1, 2, "ring", "blip"),
            "loaded_ok": store.get("cm_load_ok") is True,
            # Load Game puts the saved objects' changes back.
            "loaded": recorded("loaded") == expected(
                3, 4, "ring", "blip", "marker", "marker_child"),
            # Set Collection Member on an Empty: the collection instance's tree follows it.
            "nested": recorded("nested") == expected(
                5, 6, "ring", "blip", "marker", "marker_child", "instancer", "tree"),
            # Its Empty removed, the tree copy is no longer in the Empty's collections. FriendNest
            # (spawned into Library) and its tree are in the child collections.
            "unnested": recorded("unnested") == expected(
                4, 8, "ring", "marker", "marker_child", "enemy", "blip", "blip", "friend_nest",
                "tree"),
            # With Enemies and Assets hidden: the asset is hidden; a collection instance's tree
            # is drawn when its Empty is in a drawn collection (Instancer, and FriendNest spawned
            # while Assets was hidden), hidden with EnemyNest spawned into Enemies.
            "instances_drawn_by_empty": hidden.get("trees") == {
                "": False, names["instancer"]: True, names["enemy_nest"]: False,
                names["friend_nest"]: True},
            # A collection hidden by Set Collection Render Visibility: copies spawned after it are
            # drawn (they were all hidden before 2026-10-15), its own object is not.
            "copies_drawn": hidden.get("copies") == [True, True],
            "others_drawn": hidden.get("ring") is True and hidden.get("marker") is True,
            "enemy_hidden": hidden.get("enemy") is False,
            # Put in a drawn collection, the hidden collection's object is drawn.
            "enemy_joined": joined.get("enemy") is True,
        }
        details = {
            "mode": mode,
            "checks": checks,
            "records": {step: recorded(step) for step in
                        ("start", "spawned", "added", "removed", "undone", "loaded", "nested",
                         "unnested")},
            "hidden": hidden,
            "joined": joined,
            "phase": int(owner.get("cm_phase", 0)),
            "error": str(owner.get("cm_error", "")),
        }
        return all(checks.values()) and not details["error"], details

    if mode == "split_screen":
        # Players 0 to 2 in Favor Top with a 2 pixel divider, and a minimap on top.
        scene = bge.logic.getCurrentScene()
        cameras = [scene.objects.get(str(owner.get("__ln_acceptance_camera_%d" % index, "")))
                   for index in range(3)]
        minimap = scene.objects.get(str(owner.get("__ln_acceptance_minimap", "")))
        use_viewport = [bool(camera is not None and camera.useViewport) for camera in cameras]
        minimap_viewport = bool(minimap is not None and minimap.useViewport)
        area = tuple(float(owner.get(name, -1.0))
                     for name in ("p2_left", "p2_top", "p2_width", "p2_height"))
        views = int(owner.get("views", 0))
        p2_shown = bool(owner.get("p2_shown", False))
        active = getattr(scene, "active_camera", None)
        active_name = active.name if active is not None else ""

        def scale(camera, row):
            return float(camera.projection_matrix[row][row]) if camera is not None else 0.0

        # Crop: the full-width top view keeps the whole picture's width, so its horizontal scale
        # is a grid cell's (a cell shows the whole picture) and its vertical scale twice a cell's.
        x_ratio = scale(cameras[0], 0) / max(scale(cameras[1], 0), 1e-6)
        y_ratio = scale(cameras[0], 1) / max(scale(cameras[1], 1), 1e-6)
        ok = (
            all(use_viewport)
            and minimap_viewport
            and views == 3
            and p2_shown
            and all(abs(value - 0.5) <= 0.01 for value in area)
            and abs(x_ratio - 1.0) <= 0.02
            and abs(y_ratio - 2.0) <= 0.04
            and cameras[0] is not None
            and active_name == cameras[0].name
        )
        return ok, {
            "mode": mode,
            "use_viewport": use_viewport,
            "minimap_viewport": minimap_viewport,
            "views": views,
            "p2_shown": p2_shown,
            "p2_area": area,
            "x_ratio": x_ratio,
            "y_ratio": y_ratio,
            "active_camera": active_name,
        }

    if mode == "split_screen_leave":
        # Player 2's camera was removed and player 3's view cleared: players 0 and 1 share the
        # window, and player 3's camera draws full screen again when active (no viewport).
        scene = bge.logic.getCurrentScene()
        names = [str(owner.get("__ln_acceptance_camera_%d" % index, "")) for index in range(4)]
        cameras = [scene.objects.get(name) for name in names]
        ok = (
            cameras[2] is None
            and all(cameras[index] is not None for index in (0, 1, 3))
            and cameras[0].useViewport
            and cameras[1].useViewport
            and not cameras[3].useViewport
            and int(owner.get("views", 0)) == 2
            and not bool(owner.get("p3_shown", True))
            and bool(owner.get("left_done", False))
        )
        return ok, {
            "mode": mode,
            "cameras_left": [camera is not None for camera in cameras],
            "use_viewport": [bool(camera is not None and camera.useViewport) for camera in cameras],
            "views": int(owner.get("views", 0)),
            "p3_shown": bool(owner.get("p3_shown", True)),
            "left_done": bool(owner.get("left_done", False)),
        }

    if mode == "ui_menu_per_player":
        # Two players opened the same menu: each got a copy of their own, in their half of the
        # window at half size, reading their own player (user, gold, {gold} text), and its Set
        # Text found its own copy's tag by name. Then player 0 closed theirs; player 1's stayed.
        mid = bge.logic.globalDict.get("pp_mid", [])
        final = per_player_menu_state(owner)
        def copy_ok(copy, user, gold, side):
            # Its corner element at the top right of the player's half (half size), the badge
            # anchored inside it at half its authored offset (0.3, 0.2).
            # Its popups: its own copy of the template and the one its logic added.
            anchored = copy[5]
            return (copy[0] == user and copy[1] == gold and copy[6] == 2
                    and copy[2] == sorted([str(gold), f"player {user}"])
                    and abs(copy[3] - side) <= 0.03 and abs(copy[4] - 0.5) <= 0.01
                    and len(anchored) == 5
                    and side + 0.15 < anchored[0] < side + 0.25 and anchored[1] < 0.25
                    and abs(anchored[2] - 0.1) <= 0.005
                    and abs(anchored[3] - 0.15) <= 0.01 and abs(anchored[4] - 0.1) <= 0.01)
        if len(mid) == 2 and len(mid[0][5]) == 5 and len(mid[1][5]) == 5:
            # The same place in each half.
            if (abs(mid[1][5][0] - mid[0][5][0] - 0.5) > 0.01
                    or abs(mid[1][5][1] - mid[0][5][1]) > 0.01):
                return False, {"mode": mode, "mid": mid, "final": final}
        ok = (
            len(mid) == 2
            and copy_ok(mid[0], 0, 10, 0.25)
            and copy_ok(mid[1], 1, 20, 0.75)
            and len(final) == 1
            and copy_ok(final[0], 1, 20, 0.75)
        )
        return ok, {"mode": mode, "mid": mid, "final": final}

    if mode == "split_screen_players":
        # A HUD anchored to the top left of player 2's view, 5 units in front of player 2's
        # camera; one for player 5, who has no view, stays put. Set Player Gamepad gave
        # gamepad 3 to player 1, so player 3 has none.
        scene = bge.logic.getCurrentScene()
        camera = scene.objects.get(str(owner.get("__ln_acceptance_camera_2", "")))
        hud = scene.objects.get(str(owner.get("__ln_acceptance_hud", "")))
        idle_hud = scene.objects.get(str(owner.get("__ln_acceptance_idle_hud", "")))
        corner = (0.0, 0.0)
        distance = 0.0
        if camera is not None and hud is not None:
            local = camera.worldOrientation.transposed() @ (
                hud.worldPosition - camera.worldPosition)
            clip = camera.projection_matrix @ local.to_4d()
            if abs(clip.w) > 1e-6:
                corner = (clip.x / clip.w, clip.y / clip.w)
            distance = local.length
        idle_position = tuple(idle_hud.worldPosition) if idle_hud is not None else ()
        pads = [int(owner.get(name, -9)) for name in ("p1_pad", "p2_pad", "p3_pad")]
        ok = (
            camera is not None
            and camera.useViewport
            and abs(corner[0] + 1.0) <= 0.005
            and abs(corner[1] - 1.0) <= 0.005
            and abs(distance - 5.0) <= 0.01
            and len(idle_position) == 3
            and all(abs(value - 100.0) <= 1e-4 for value in idle_position)
            and pads == [3, 2, -1]
            and not bool(owner.get("p1_joined", True))
            and bool(owner.get("sound_started", False))
        )
        return ok, {
            "mode": mode,
            "corner": corner,
            "distance": distance,
            "idle_position": idle_position,
            "pads": pads,
            "p1_joined": bool(owner.get("p1_joined", True)),
            "sound_started": bool(owner.get("sound_started", False)),
        }

    if mode == "render_state":
        observed_fullscreen = bool(bge.render.getFullScreen())
        observed_width = int(bge.render.getWindowWidth())
        observed_height = int(bge.render.getWindowHeight())
        observed_vsync_interval = int(bge.render.getVsync())
        property_fullscreen = bool(owner.get("fullscreen_state", False))
        property_width = int(owner.get("resolution_width", 0))
        property_height = int(owner.get("resolution_height", 0))
        property_vsync_mode = int(owner.get("vsync_mode", -1))
        expected_vsync_mode = int(owner.get("__ln_acceptance_expected_vsync_mode", 1))
        expected_vsync_interval = int(owner.get("__ln_acceptance_expected_vsync_interval", 0))
        ok = (
            property_fullscreen == observed_fullscreen
            and property_width == observed_width
            and property_height == observed_height
            and property_vsync_mode == expected_vsync_mode
            and observed_vsync_interval == expected_vsync_interval
        )
        return ok, {
            "mode": mode,
            "observed_fullscreen": observed_fullscreen,
            "property_fullscreen": property_fullscreen,
            "observed_width": observed_width,
            "property_width": property_width,
            "observed_height": observed_height,
            "property_height": property_height,
            "property_vsync_mode": property_vsync_mode,
            "expected_vsync_mode": expected_vsync_mode,
            "observed_vsync_interval": observed_vsync_interval,
            "expected_vsync_interval": expected_vsync_interval,
        }

    if mode == "light_state":
        scene = bge.logic.getCurrentScene()
        expected_rgb = expected_color(owner)[:3]
        expected_power = float(owner.get("__ln_acceptance_expected_power", 0.0))
        expected_shadow = bool(owner.get("__ln_acceptance_expected_shadow", False))
        source_name = owner.get("__ln_acceptance_source_light", "")
        mirror_name = owner.get("__ln_acceptance_mirror_light", "")
        source = next((game_object for game_object in scene.objects if game_object.name == source_name), None)
        mirror = next((game_object for game_object in scene.objects if game_object.name == mirror_name), None)

        def read_light_state(game_object):
            blender_object = getattr(game_object, "blenderObject", None) if game_object is not None else None
            data = getattr(blender_object, "data", None) if blender_object is not None else None
            if data is None:
                return None, None, None
            return tuple(float(component) for component in data.color), float(data.energy), bool(data.use_shadow)

        source_color, source_power, source_shadow = read_light_state(source)
        mirror_color, mirror_power, _mirror_shadow = read_light_state(mirror)
        ok = (
            source_color is not None
            and mirror_color is not None
            and close_tuple(source_color, expected_rgb, tolerance)
            and abs(source_power - expected_power) <= tolerance
            and source_shadow == expected_shadow
            and close_tuple(mirror_color, expected_rgb, tolerance)
            and abs(mirror_power - expected_power) <= tolerance
        )
        return ok, {
            "mode": mode,
            "source_found": source is not None,
            "mirror_found": mirror is not None,
            "source_color": source_color,
            "mirror_color": mirror_color,
            "expected_color": expected_rgb,
            "source_power": source_power,
            "mirror_power": mirror_power,
            "expected_power": expected_power,
            "source_shadow": source_shadow,
            "expected_shadow": expected_shadow,
        }

    if mode == "light_unique":
        scene = bge.logic.getCurrentScene()
        expected_source_rgb = expected_color(owner)[:3]
        expected_source_power = float(owner.get("__ln_acceptance_expected_power", 0.0))
        expected_source_shadow = bool(owner.get("__ln_acceptance_expected_shadow", False))
        expected_mirror_rgb = (
            float(owner.get("__ln_acceptance_expected_mirror_r", 1.0)),
            float(owner.get("__ln_acceptance_expected_mirror_g", 1.0)),
            float(owner.get("__ln_acceptance_expected_mirror_b", 1.0)),
        )
        expected_mirror_power = float(owner.get("__ln_acceptance_expected_mirror_power", 0.0))
        expected_mirror_shadow = bool(owner.get("__ln_acceptance_expected_mirror_shadow", False))
        source_name = owner.get("__ln_acceptance_source_light", "")
        mirror_name = owner.get("__ln_acceptance_mirror_light", "")
        source = next((game_object for game_object in scene.objects if game_object.name == source_name), None)
        mirror = next((game_object for game_object in scene.objects if game_object.name == mirror_name), None)

        def read_light_state(game_object):
            blender_object = getattr(game_object, "blenderObject", None) if game_object is not None else None
            data = getattr(blender_object, "data", None) if blender_object is not None else None
            if data is None:
                return None, None, None, 0, ""
            return (
                tuple(float(component) for component in data.color),
                float(data.energy),
                bool(data.use_shadow),
                int(getattr(data, "users", 0)),
                str(getattr(data, "name", "")),
            )

        source_color, source_power, source_shadow, source_users, source_data_name = read_light_state(source)
        mirror_color, mirror_power, mirror_shadow, mirror_users, mirror_data_name = read_light_state(mirror)
        ok = (
            source_color is not None
            and mirror_color is not None
            and close_tuple(source_color, expected_source_rgb, tolerance)
            and abs(source_power - expected_source_power) <= tolerance
            and source_shadow == expected_source_shadow
            and close_tuple(mirror_color, expected_mirror_rgb, tolerance)
            and abs(mirror_power - expected_mirror_power) <= tolerance
            and mirror_shadow == expected_mirror_shadow
            and source_users == 1
            and source_data_name
            and mirror_data_name
            and source_data_name != mirror_data_name
        )
        return ok, {
            "mode": mode,
            "source_found": source is not None,
            "mirror_found": mirror is not None,
            "source_color": source_color,
            "mirror_color": mirror_color,
            "expected_source_color": expected_source_rgb,
            "expected_mirror_color": expected_mirror_rgb,
            "source_power": source_power,
            "mirror_power": mirror_power,
            "expected_source_power": expected_source_power,
            "expected_mirror_power": expected_mirror_power,
            "source_shadow": source_shadow,
            "mirror_shadow": mirror_shadow,
            "expected_source_shadow": expected_source_shadow,
            "expected_mirror_shadow": expected_mirror_shadow,
            "source_users": source_users,
            "mirror_users": mirror_users,
            "source_data_name": source_data_name,
            "mirror_data_name": mirror_data_name,
        }

    if mode == "physics_state":
        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_target", owner.name)
        target = next((game_object for game_object in scene.objects if game_object.name == target_name), None)
        observed_group = int(target.collisionGroup) if target is not None else -1
        property_group = int(owner.get("collision_group", 0))
        dynamics_suspended = bool(target.isSuspendDynamics) if target is not None else False
        expected_group = int(owner.get("__ln_acceptance_expected_collision_group", 0))
        ok = (
            target is not None
            and observed_group == expected_group
            and property_group == expected_group
            and dynamics_suspended
        )
        return ok, {
            "mode": mode,
            "observed_group": observed_group,
            "property_group": property_group,
            "target_found": target is not None,
            "expected_group": expected_group,
            "dynamics_suspended": dynamics_suspended,
        }

    if mode == "character_state":
        observed_position = vector_tuple(owner.worldPosition)
        property_gravity = (
            float(owner.get("character_gravity_x", 0.0)),
            float(owner.get("character_gravity_y", 0.0)),
            float(owner.get("character_gravity_z", 0.0)),
        )
        property_walk = (
            float(owner.get("character_walk_x", 0.0)),
            float(owner.get("character_walk_y", 0.0)),
            float(owner.get("character_walk_z", 0.0)),
        )
        property_max_jumps = int(owner.get("character_max_jumps", 0))
        property_jump_count = int(owner.get("character_jump_count", 0))
        property_on_ground = bool(owner.get("character_on_ground", True))
        expected_gravity = expected_named_vector(owner, "gravity")
        expected_walk = expected_named_vector(owner, "walk")
        expected_max_jumps = int(owner.get("__ln_acceptance_expected_max_jumps", 0))
        min_jump_count = int(owner.get("__ln_acceptance_min_jump_count", 0))
        min_x = float(owner.get("__ln_acceptance_min_x", 0.0))
        min_y = float(owner.get("__ln_acceptance_min_y", 0.0))
        min_z = float(owner.get("__ln_acceptance_min_z", 0.0))
        state_delivery_ok = (
            close_tuple(property_gravity, expected_gravity, tolerance)
            and close_tuple(property_walk, expected_walk, tolerance)
            and property_max_jumps == expected_max_jumps
        )
        motion_ok = (
            property_jump_count >= min_jump_count
            and observed_position[0] >= min_x
            and observed_position[1] >= min_y
            and observed_position[2] >= min_z
            and not property_on_ground
        )
        ok = state_delivery_ok
        return ok, {
            "mode": mode,
            "state_delivery_ok": state_delivery_ok,
            "motion_ok": motion_ok,
            "observed_position": observed_position,
            "property_gravity": property_gravity,
            "expected_gravity": expected_gravity,
            "property_walk": property_walk,
            "expected_walk": expected_walk,
            "property_max_jumps": property_max_jumps,
            "expected_max_jumps": expected_max_jumps,
            "property_jump_count": property_jump_count,
            "min_jump_count": min_jump_count,
            "property_on_ground": property_on_ground,
            "min_x": min_x,
            "min_y": min_y,
            "min_z": min_z,
        }

    if mode == "object_actions":
        observed_position = vector_tuple(owner.worldPosition)
        observed_rotation = orientation_tuple(owner.worldOrientation)
        observed_color = color_tuple(owner.color)
        observed_visible = bool(owner.visible)
        rotation = expected_rotation(owner)
        expected_visible = bool(owner.get("__ln_acceptance_expected_visible", False))
        color = expected_color(owner)
        ok = (
            close_tuple(observed_position, expected, tolerance)
            and close_tuple(observed_rotation, rotation, tolerance)
            and close_color(observed_color, color, tolerance)
            and observed_visible == expected_visible
        )
        return ok, {
            "mode": mode,
            "observed_position": observed_position,
            "expected_position": expected,
            "observed_rotation": observed_rotation,
            "expected_rotation": rotation,
            "observed_color": observed_color,
            "expected_color": color,
            "observed_visible": observed_visible,
            "expected_visible": expected_visible,
        }

    if mode == "object_lifecycle":
        scene_objects = tuple(bge.logic.getCurrentScene().objects)
        object_names = [game_object.name for game_object in scene_objects]
        template_name = owner.get("__ln_acceptance_template", "")
        active_template_name = owner.get("__ln_acceptance_active_template", "")
        victim_name = owner.get("__ln_acceptance_victim", "")
        child_name = owner.get("__ln_acceptance_child", "")
        parent_name = owner.get("__ln_acceptance_parent", "")
        target_name = owner.get("__ln_acceptance_unparent_target", "")
        replica_position = expected_named_vector(owner, "replica_spawn")
        full_copy_position = expected_named_vector(owner, "full_copy_spawn")
        repeat_spawn_position = expected_named_vector(owner, "repeat_spawn")
        result_output_position = expected_named_vector(owner, "result_output_spawn")
        ordered_copy_position = expected_named_vector(owner, "ordered_copy_spawn")
        life_present_position = expected_named_vector(owner, "life_present_spawn")
        full_life_present_position = expected_named_vector(owner, "full_life_present_spawn")
        life_expired_position = expected_named_vector(owner, "life_expired_spawn")
        active_spawn_position = expected_named_vector(owner, "active_reject_spawn")
        added_id_a = owner.get("added_object_id_a", "")
        added_id_b = owner.get("added_object_id_b", "")
        added_object_ids_unique = (
            added_id_a.startswith(f"{template_name}.")
            and added_id_b.startswith(f"{template_name}.")
            and added_id_a != added_id_b
        )
        target = next((game_object for game_object in scene_objects if game_object.name == target_name), None)
        child = next((game_object for game_object in scene_objects if game_object.name == child_name), None)
        matching_positions = [
            vector_tuple(game_object.worldPosition)
            for game_object in scene_objects
            if game_object.name.startswith(template_name)
        ]
        active_template_spawn_found = any(
            close_tuple(vector_tuple(game_object.worldPosition), active_spawn_position, tolerance)
            for game_object in scene_objects
            if active_template_name and game_object.name.startswith(active_template_name)
        )
        victim_exists = any(name == victim_name for name in object_names)
        parent_removed = target is not None and getattr(target, "parent", None) is None
        child_parented = child is not None and getattr(child, "parent", None) is not None and \
            child.parent.name == parent_name
        replica_found = any(
            close_tuple(position, replica_position, tolerance) for position in matching_positions)
        full_copy_found = any(
            close_tuple(position, full_copy_position, tolerance) for position in matching_positions)
        result_output_found = any(
            close_tuple(position, result_output_position, tolerance) for position in matching_positions)
        ordered_copy_found = any(
            close_tuple(position, ordered_copy_position, tolerance) for position in matching_positions)
        life_present_found = any(
            close_tuple(position, life_present_position, tolerance) for position in matching_positions)
        full_life_present_found = any(
            close_tuple(position, full_life_present_position, tolerance) for position in matching_positions)
        life_expired_found = any(
            close_tuple(position, life_expired_position, tolerance) for position in matching_positions)
        spawned_logic_found = any(
            bool(game_object.get("spawn_logic_ran", False))
            for game_object in scene_objects
            if game_object.name.startswith(template_name)
        )
        repeat_spawn_count = sum(
            1 for position in matching_positions
            if close_tuple(position, repeat_spawn_position, tolerance)
        )
        ok = (
            len(matching_positions) >= 8
            and not active_template_spawn_found
            and not life_expired_found
            and not victim_exists
            and parent_removed
            and child_parented
            and replica_found
            and full_copy_found
            and result_output_found
            and ordered_copy_found
            and life_present_found
            and full_life_present_found
            and spawned_logic_found
            and repeat_spawn_count >= 2
            and added_object_ids_unique
        )
        return ok, {
            "mode": mode,
            "matching_positions": matching_positions,
            "repeat_spawn_count": repeat_spawn_count,
            "result_output_found": result_output_found,
            "ordered_copy_found": ordered_copy_found,
            "life_present_found": life_present_found,
            "full_life_present_found": full_life_present_found,
            "life_expired_found": life_expired_found,
            "spawned_logic_found": spawned_logic_found,
            "victim_exists": victim_exists,
            "parent_removed": parent_removed,
            "child_parented": child_parented,
            "replica_found": replica_found,
            "full_copy_found": full_copy_found,
            "active_template_spawn_found": active_template_spawn_found,
            "added_id_a": added_id_a,
            "added_id_b": added_id_b,
            "added_object_ids_unique": added_object_ids_unique,
            "objects": object_names,
        }

    if mode == "object_queries":
        has_parent = bool(owner.get("has_parent", False))
        has_child = bool(owner.get("has_child", False))
        missing_child_is_none = bool(owner.get("missing_child_is_none", False))
        ok = has_parent and has_child and missing_child_is_none
        return ok, {
            "mode": mode,
            "has_parent": has_parent,
            "has_child": has_child,
            "missing_child_is_none": missing_child_is_none,
        }

    if mode == "average_center":
        prefixes = (
            "vector_center",
            "object_center",
            "collection_center",
            "empty_vector_center",
            "empty_object_center",
            "empty_collection_center",
        )
        observed = {
            prefix: (
                (
                    float(owner.get(f"{prefix}_x", 0.0)),
                    float(owner.get(f"{prefix}_y", 0.0)),
                    float(owner.get(f"{prefix}_z", 0.0)),
                ),
                int(owner.get(f"{prefix}_count", 0)),
                bool(owner.get(f"{prefix}_valid", False)),
            )
            for prefix in prefixes
        }
        expected_centers = {
            prefix: expected_named_vector(owner, prefix) for prefix in prefixes
        }
        expected_counts = {
            prefix: int(owner.get(f"{prefix}_expected_count", -1)) for prefix in prefixes
        }
        expected_valid = {
            prefix: bool(owner.get(f"{prefix}_expected_valid", False)) for prefix in prefixes
        }
        ok = all(
            close_tuple(observed[prefix][0], expected_centers[prefix], tolerance)
            and observed[prefix][1] == expected_counts[prefix]
            and observed[prefix][2] == expected_valid[prefix]
            for prefix in prefixes
        )
        return ok, {
            "mode": mode,
            "observed": observed,
            "expected_centers": expected_centers,
            "expected_counts": expected_counts,
            "expected_valid": expected_valid,
        }

    if mode == "raycast_from_mesh":
        list_names = (
            "hits", "blocked", "hit_objects", "points", "normals", "distances",
            "fractions", "face_indices", "has_uvs", "uvs", "ray_origins",
            "ray_directions", "end_points",
        )
        list_lengths = {
            name: int(owner.get(f"mesh_face_len_{name}", -1)) for name in list_names
        }
        blocked_list_lengths = {
            name: int(owner.get(f"mesh_blocked_len_{name}", -1)) for name in list_names
        }
        observed = {
            "face_done": bool(owner.get("mesh_face_done", False)),
            "face_any": bool(owner.get("mesh_face_any", False)),
            "face_all": bool(owner.get("mesh_face_all", False)),
            "face_hit_count": int(owner.get("mesh_face_hit_count", -1)),
            "face_ray_count": int(owner.get("mesh_face_ray_count", -1)),
            "face_first_hit": bool(owner.get("mesh_face_first_hit", False)),
            "face_first_blocked": bool(owner.get("mesh_face_first_blocked", True)),
            "face_first_object": bool(owner.get("mesh_face_first_object", False)),
            "face_average_count": int(owner.get("mesh_face_average_count", -1)),
            "face_average_valid": bool(owner.get("mesh_face_average_valid", False)),
            "face_average_x": float(owner.get("mesh_face_average_x", 0.0)),
            "face_average_y": float(owner.get("mesh_face_average_y", 0.0)),
            "face_average_z": float(owner.get("mesh_face_average_z", 0.0)),
            "vertex_any": bool(owner.get("mesh_vertex_any", False)),
            "vertex_all": bool(owner.get("mesh_vertex_all", False)),
            "vertex_hit_count": int(owner.get("mesh_vertex_hit_count", -1)),
            "vertex_ray_count": int(owner.get("mesh_vertex_ray_count", -1)),
            "blocked_any": bool(owner.get("mesh_blocked_any", True)),
            "blocked_all": bool(owner.get("mesh_blocked_all", True)),
            "blocked_hit_count": int(owner.get("mesh_blocked_hit_count", -1)),
            "blocked_ray_count": int(owner.get("mesh_blocked_ray_count", -1)),
            "blocked_first": bool(owner.get("mesh_blocked_first", False)),
            "blocked_average_count": int(owner.get("mesh_blocked_average_count", -1)),
            "blocked_average_valid": bool(owner.get("mesh_blocked_average_valid", True)),
        }
        ok = (
            observed["face_done"]
            and observed["face_any"]
            and not observed["face_all"]
            and observed["face_hit_count"] == 1
            and observed["face_ray_count"] == 16
            and observed["face_first_hit"]
            and not observed["face_first_blocked"]
            and observed["face_first_object"]
            and all(
                list_lengths[name] == (16 if name in {
                    "hits", "blocked", "ray_origins", "ray_directions", "end_points"
                } else 1)
                for name in list_names
            )
            and observed["face_average_count"] == 1
            and observed["face_average_valid"]
            and abs(observed["face_average_x"] - 2.25) <= tolerance
            and abs(observed["face_average_y"] + 0.8) <= tolerance
            and abs(observed["face_average_z"] + 0.8) <= tolerance
            and observed["vertex_any"]
            and observed["vertex_all"]
            and observed["vertex_hit_count"] == 7
            and observed["vertex_ray_count"] == 7
            and not observed["blocked_any"]
            and not observed["blocked_all"]
            and observed["blocked_hit_count"] == 0
            and observed["blocked_ray_count"] == 4
            and observed["blocked_first"]
            and observed["blocked_average_count"] == 0
            and not observed["blocked_average_valid"]
            and all(
                blocked_list_lengths[name] == (4 if name in {
                    "hits", "blocked", "ray_origins", "ray_directions", "end_points"
                } else 0)
                for name in list_names
            )
        )
        return ok, {
            "mode": mode,
            "observed": observed,
            "list_lengths": list_lengths,
            "blocked_list_lengths": blocked_list_lengths,
        }

    if mode == "raycast":
        hit = bool(owner.get("ray_hit", False))
        object_hit = bool(owner.get("ray_object", False))
        ray_all_hit = bool(owner.get("ray_all_hit", False))
        ray_all_count = int(owner.get("ray_all_count", 0))
        shape_hit = bool(owner.get("shape_hit", False))
        shape_all_hit = bool(owner.get("shape_all_hit", False))
        shape_all_count = int(owner.get("shape_all_count", 0))
        point = (
            float(owner.get("ray_point_x", 0.0)),
            float(owner.get("ray_point_y", 0.0)),
            float(owner.get("ray_point_z", 0.0)),
        )
        normal = (
            float(owner.get("ray_normal_x", 0.0)),
            float(owner.get("ray_normal_y", 0.0)),
            float(owner.get("ray_normal_z", 0.0)),
        )
        direction = (
            float(owner.get("ray_direction_x", 0.0)),
            float(owner.get("ray_direction_y", 0.0)),
            float(owner.get("ray_direction_z", 0.0)),
        )
        end_point = (
            float(owner.get("ray_end_point_x", 0.0)),
            float(owner.get("ray_end_point_y", 0.0)),
            float(owner.get("ray_end_point_z", 0.0)),
        )
        expected_point = expected_named_vector(owner, "hit_point")
        expected_normal = expected_named_vector(owner, "hit_normal")
        expected_direction = expected_named_vector(owner, "hit_direction")
        expected_end_point = expected_named_vector(owner, "ray_end_point")
        ok = (
            hit
            and object_hit
            and ray_all_hit
            and ray_all_count == 1
            and shape_hit
            and shape_all_hit
            and shape_all_count == 1
            and close_tuple(point, expected_point, tolerance)
            and close_tuple(normal, expected_normal, tolerance)
            and close_tuple(direction, expected_direction, tolerance)
            and close_tuple(end_point, expected_end_point, tolerance)
        )
        return ok, {
            "mode": mode,
            "hit": hit,
            "object_hit": object_hit,
            "ray_all_hit": ray_all_hit,
            "ray_all_count": ray_all_count,
            "shape_hit": shape_hit,
            "shape_all_hit": shape_all_hit,
            "shape_all_count": shape_all_count,
            "point": point,
            "expected_point": expected_point,
            "normal": normal,
            "expected_normal": expected_normal,
            "direction": direction,
            "expected_direction": expected_direction,
            "end_point": end_point,
            "expected_end_point": expected_end_point,
        }

    if mode == "tier_a":
        list_len = int(owner.get("tier_a_list_len", 0))
        dict_len = int(owner.get("tier_a_dict_len", 0))
        global_value = float(owner.get("tier_a_global", 0.0))
        found_by_name = bool(owner.get("tier_a_found_by_name", False))
        owner_ok = bool(owner.get("tier_a_owner_ok", False))
        mover_name = str(owner.get("__ln_acceptance_mover", ""))
        mover = next(
            (candidate for candidate in owner.scene.objects if candidate.name == mover_name), None)
        translate_x = float(mover.worldPosition[0]) if mover is not None else 0.0
        ok = (
            list_len == 2
            and dict_len == 1
            and abs(global_value - 99.0) <= tolerance
            and found_by_name
            and owner_ok
            and mover is not None
            and translate_x >= float(owner.get("__ln_acceptance_min_x", 0.2))
        )
        return ok, {
            "mode": mode,
            "list_len": list_len,
            "dict_len": dict_len,
            "global_value": global_value,
            "found_by_name": found_by_name,
            "owner_ok": owner_ok,
            "translate_x": translate_x,
        }

    if mode in {"mouse_over", "mouse_over_cadence"}:
        entered = bool(owner.get("mouse_enter", False))
        over = bool(owner.get("mouse_over", False))
        exited = bool(owner.get("mouse_exit", False))
        over_count = float(owner.get("mouse_over_count", 0.0))
        min_over_count = float(owner.get("__ln_acceptance_min_mouse_over_count", 1.0))
        point = (
            float(owner.get("mouse_point_x", 0.0)),
            float(owner.get("mouse_point_y", 0.0)),
            float(owner.get("mouse_point_z", 0.0)),
        )
        normal = (
            float(owner.get("mouse_normal_x", 0.0)),
            float(owner.get("mouse_normal_y", 0.0)),
            float(owner.get("mouse_normal_z", 0.0)),
        )
        hit_object_name = str(owner.get("__ln_acceptance_mouse_hit_object", ""))
        hit_object = next(
            (
                candidate
                for candidate in bge.logic.getCurrentScene().objects
                if candidate.name == hit_object_name
            ),
            None,
        )
        object_output_ok = hit_object is not None and bool(
            hit_object.get("mouse_over_object_output", False))
        cadence_ok = (
            entered
            and over
            and not exited
            and over_count >= min_over_count
            and object_output_ok
        )
        dominant_axis = max(range(3), key=lambda axis: abs(normal[axis]))
        geometry_ok = (
            all(math.isfinite(value) for value in point + normal)
            and all(abs(value) <= 1.0 + tolerance for value in point)
            and abs(abs(point[dominant_axis]) - 1.0) <= tolerance
            and abs(abs(normal[dominant_axis]) - 1.0) <= tolerance
            and all(
                abs(normal[axis]) <= tolerance
                for axis in range(3)
                if axis != dominant_axis
            )
            and point[dominant_axis] * normal[dominant_axis] > 0.0
        )
        ok = cadence_ok and (mode == "mouse_over_cadence" or geometry_ok)
        return ok, {
            "mode": mode,
            "entered": entered,
            "over": over,
            "exited": exited,
            "over_count": over_count,
            "min_over_count": min_over_count,
            "point": point,
            "normal": normal,
            "hit_object_name": hit_object_name,
            "object_output_ok": object_output_ok,
        }

    return False, {"mode": mode, "error": "unknown mode"}


controller = bge.logic.getCurrentController()
owner = controller.owner

mode = owner.get("__ln_acceptance_mode", "")
mutate_runtime_structure(owner)
async_save_load_mode = mode in {
    "phase6_save_load",
    "phase6_save_load_structure",
    "phase6_save_load_material",
    "phase6_save_load_material_fresh",
    "phase6_save_load_existence",
    "phase6_cross_scene_save",
    "phase6_cross_scene_load",
}
if async_save_load_mode:
    frame_key = "__ln_acceptance_save_load_frame_" + owner.get("__ln_acceptance_run", "unknown")
    frame = int(bge.logic.globalDict.get(frame_key, 0)) + 1
    bge.logic.globalDict[frame_key] = frame
    time.sleep(0.01)
else:
    frame = int(owner.get("__ln_acceptance_frame", 0)) + 1
    owner["__ln_acceptance_frame"] = frame
if mode == "ui_grid_template":
    record_grid_slots(owner)
if mode == "ui_grid_scrollbar":
    record_grid_scrollbar(owner)
if mode == "game_property_types":
    try:
        check_game_property_types(owner)
    except Exception as error:
        owner["props_report"] = "error: " + repr(error)
        owner["props_stage"] = 2
        owner["ui_cycle_done"] = True
if (mode == "object_menu_session" and owner.get("menu_external_ready", False) and
        not owner.get("menu_external_removed", False)):
    import bpy
    collection = bpy.data.collections.get(
        str(owner.get("__ln_acceptance_expected_collection", "")))
    if collection is not None:
        bge.logic.getCurrentScene().removeOverlayCollection(collection)
        owner["menu_external_removed"] = True
required_frame = int(owner.get("__ln_acceptance_required_frame", 1))
check_now = frame >= required_frame
if mode == "object_menu_session":
    # Check one controller tick after the session ends (done or failed: the native tree and this
    # controller run in an unspecified order), not at a frame count: the session ended between
    # frames 225 and 250 in the player, which counts render passes (2026-10-14).
    session_ended = bool(owner.get("menu_session_done", False) or
                         owner.get("menu_session_failed", False))
    check_now = (session_ended and bool(owner.get("menu_session_end_seen", False))) or (
        frame >= 8 * required_frame)
    if session_ended:
        owner["menu_session_end_seen"] = True
if mode in ("ui_elements", "ui_menu_focus_restore", "ui_overlay_visibility", "ui_paused_routes",
            "ui_grid_template", "ui_grid_scrollbar", "ui_grid_pages", "game_property_types",
            "input_map", "ui_menu_pause_toggle", "ui_text_values_bar",
            "ui_grid_parented_template", "ui_menu_camera_hidden", "ui_player_menu",
            "ui_menu_replace_inside", "ui_example_file", "ui_templates",
            "split_screen", "split_screen_leave", "split_screen_players",
            "ui_menu_per_player", "ui_element_players",
            "ui_take_control_pointer", "ui_view_center", "ui_grid_inventory",
            "ui_sound_events"):
    # UI resolves on rendered frames, which the player runner does not count.
    # Check once the probe reported its result (a moment later for the looks),
    # with a time limit as fallback.
    start_key = "__ln_acceptance_ui_start_" + owner.get("__ln_acceptance_run", "unknown")
    start_time = float(bge.logic.globalDict.get(start_key, 0.0))
    if start_time <= 0.0:
        start_time = time.monotonic()
        bge.logic.globalDict[start_key] = start_time
    elapsed = time.monotonic() - start_time
    if mode == "ui_elements":
        ready = bool(owner.get("ui_focus_name", "")) and elapsed >= 1.0
    elif mode == "ui_player_menu":
        ready = int(owner.get("hl_user", -1)) >= 0 and elapsed >= 1.0
    elif mode == "ui_grid_inventory":
        ready = int(owner.get("bag_last", -1)) >= 0 and elapsed >= 1.0
    elif mode == "ui_menu_replace_inside":
        # The tab switch waits 0.3 s; then the new menu must still be open a moment later.
        ready = elapsed >= 2.0
    elif mode == "ui_paused_routes":
        # Long enough after the paused Enter for frame logic to show it did not run.
        ready = bool(owner.get("ui_enter_while_paused", False)) and elapsed >= 1.5
    elif mode == "ui_sound_events":
        # B comes under the pointer half a second in.
        ready = str(owner.get("last_move", "")) == "b" or elapsed >= 4.0
    elif mode == "ui_menu_focus_restore":
        ready = bool(owner.get("ui_restored_focus", "")) or bool(
            owner.get("ui_set_focus_failed", False))
    elif mode == "input_map":
        # The rebind waits 0.2 s of real time, however fast frames come.
        ready = bool(owner.get("rebind_done", False)) and elapsed >= 0.5
    elif mode == "split_screen":
        # The views and their projections are set when frames are drawn.
        ready = elapsed >= 1.0
    elif mode == "split_screen_leave":
        ready = bool(owner.get("left_done", False)) and elapsed >= 1.5
    elif mode == "split_screen_players":
        ready = bool(owner.get("sound_started", False)) and elapsed >= 1.0
    elif mode == "ui_view_center":
        samples = bge.logic.globalDict.setdefault("vc_samples", {})

        def vc_sample(*names):
            return [bool(bge.logic.mouse.visible)] + [bool(owner.get(n, False)) for n in names]

        if 0.5 <= elapsed < 0.7 and "before" not in samples:
            samples["before"] = vc_sample("vc_enter_0", "vc_enter_1", "vc_exit_0")
        if 1.3 <= elapsed < 1.5 and "menu" not in samples:
            samples["menu"] = vc_sample("vc_enter_0", "vc_exit_0")
        if 2.4 <= elapsed and "after" not in samples:
            samples["after"] = vc_sample("vc_enter_0", "vc_enter_1", "vc_exit_0")
        ready = "after" in samples
    elif mode == "ui_take_control_pointer":
        if 0.8 <= elapsed < 1.1 and "tc_mid" not in bge.logic.globalDict:
            cursor = next((obj for obj in bge.logic.getCurrentScene().objects
                           if obj.name.endswith("_TCCursor")), None)
            if cursor is not None:
                position = cursor.worldPosition
                bge.logic.globalDict["tc_mid"] = (float(position[0]), float(position[1]),
                                                  float(position[2]), bool(cursor.visible))
        ready = bool(owner.get("tc_take2_after", "")) and elapsed >= 2.0
    elif mode == "ui_element_players":
        ready = all(owner.get("focus_" + name, "") for name in (
            "p1_user1", "p1_user0", "any_user2", "hidden_user1", "hidden_user0"))
    elif mode == "ui_menu_per_player":
        if 0.45 <= elapsed < 0.85 and "pp_mid" not in bge.logic.globalDict:
            bge.logic.globalDict["pp_mid"] = per_player_menu_state(owner)
        ready = elapsed >= 1.8
    elif mode in ("ui_example_file", "ui_templates"):
        # The menus open at the start; their grids are filled in the frames after.
        ready = elapsed >= 1.0
    else:
        ready = bool(owner.get("ui_cycle_done", False))
    check_now = ready or elapsed >= 8.0
if mode == "camera_renderers":
    # Real time: the renderers are given back after 5 seconds without drawing.
    check_now = drive_camera_renderers(owner)
if mode == "camera_minimap_options":
    check_now = drive_camera_minimap_options(owner)
if mode == "collection_membership":
    check_now = drive_collection_membership(owner)
if mode == "material_actions":
    check_now = drive_material_actions(owner)
if mode == "camera_visibility_still":
    check_now = drive_camera_visibility_still(owner)
if mode == "smooth_motion":
    check_now = drive_smooth_motion(owner)
if mode == "frame_pacing":
    check_now = drive_frame_pacing(owner)
if mode == "mouse_over_cadence":
    start_key = "__ln_acceptance_mouse_over_start_" + owner.get(
        "__ln_acceptance_run", "unknown")
    start_time = float(bge.logic.globalDict.get(start_key, 0.0))
    if start_time <= 0.0:
        start_time = time.monotonic()
        bge.logic.globalDict[start_key] = start_time
    check_now = time.monotonic() - start_time >= float(
        owner.get("__ln_acceptance_required_seconds", 0.5))
if mode == "phase6_animation_status":
    start_key = "__ln_acceptance_animation_status_start_" + owner.get(
        "__ln_acceptance_run", "unknown")
    start_time = float(bge.logic.globalDict.get(start_key, 0.0))
    if start_time <= 0.0:
        start_time = time.monotonic()
        bge.logic.globalDict[start_key] = start_time
    check_now = bool(owner.get("action_lifecycle_completed", False)) or (
        time.monotonic() - start_time >= float(
            owner.get("__ln_acceptance_required_seconds", 2.0)))
if async_save_load_mode:
    completion_property = "save_completed" if mode == "phase6_cross_scene_save" else "load_completed"
    completion_owner = owner
    if mode == "phase6_save_load_structure":
        structure_object = str(
            owner.get("__ln_acceptance_structure_object", ""))
        completion_owner = next(
            (
                candidate
                for candidate in bge.logic.getCurrentScene().objects
                if candidate.name == structure_object
            ),
            owner,
        )
    check_now = bool(completion_owner.get(completion_property, False)) or check_now

if check_now and not owner.get("__ln_acceptance_done", False):
    owner["__ln_acceptance_done"] = True
    run_id = owner.get("__ln_acceptance_run", "unknown")
    try:
        ok, details = check_owner(owner)
        details.update({"ok": ok, "run": run_id, "frame": frame})
        write_status(owner, details)
    except Exception:
        write_status(owner, {
            "ok": False,
            "run": run_id,
            "frame": frame,
            "error": traceback.format_exc(),
        })
    finally:
        bge.logic.endGame()
'''


PLAYER_MAINLOOP_SCRIPT = r'''
import json
import math
import os
import time
import traceback

import bge
import bpy


def vector_tuple(value):
    return tuple(float(component) for component in value)


def drive_camera_renderers(owner):
    """Camera renderers: two view cameras that may share one renderer and a Use Workbench one
    draw for a second, stop drawing for 6 seconds (renderers idle for 5 are given back), then
    draw again. True once done, or after a time limit."""
    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    start = store.setdefault("cr_start", time.monotonic())
    elapsed = time.monotonic() - start
    names = [str(owner.get("__ln_acceptance_cr_" + key, "")) for key in ("a", "b", "w")]
    cameras = [scene.objects.get(name) for name in names]
    if any(camera is None for camera in cameras):
        owner["cr_error"] = "missing camera"
        return True
    phase = int(owner.get("cr_phase", 0))
    if phase == 0:
        width = bge.render.getWindowWidth()
        height = bge.render.getWindowHeight()
        view_w = max(16, width // 4)
        view_h = max(16, height // 4)
        cameras[0].setViewport(0, height - view_h, view_w, height)
        cameras[1].setViewport(width - view_w, height - view_h, width, height)
        cameras[2].setViewport(0, 0, view_w, view_h)
        for camera in cameras:
            camera.useViewport = True
        owner["cr_phase"] = 1
    elif phase == 1:
        owner["cr_frames_on"] = int(owner.get("cr_frames_on", 0)) + 1
        if elapsed >= 1.0:
            for camera in cameras:
                camera.useViewport = False
            owner["cr_phase"] = 2
    elif phase == 2:
        owner["cr_frames_off"] = int(owner.get("cr_frames_off", 0)) + 1
        if elapsed >= 7.0:
            for camera in cameras:
                camera.useViewport = True
            store["cr_back"] = time.monotonic()
            owner["cr_phase"] = 3
    elif phase == 3:
        owner["cr_frames_back"] = int(owner.get("cr_frames_back", 0)) + 1
        if time.monotonic() - store["cr_back"] >= 0.8 and owner["cr_frames_back"] >= 5:
            owner["cr_phase"] = 4
    return int(owner.get("cr_phase", 0)) == 4 or elapsed >= 13.0


SM_KEYS = ("mover", "child", "ball", "plain", "teleport", "node")


def sm_objects(scene, owner):
    return {key: scene.objects.get(str(owner.get("__ln_acceptance_sm_" + key, "")))
            for key in SM_KEYS}


def sm_record():
    """Smooth Motion, after each drawn frame: where each object was drawn (worldPosition holds
    the drawn pose until the frame is over) and where it is (localPosition, a root's own)."""
    store = bge.logic.globalDict
    if store.get("sm_stop", False):
        return
    scene = bge.logic.getCurrentScene()
    owner = scene.objects.get(str(store.get("sm_owner", "")))
    if owner is None:
        return
    row = {"t": time.monotonic() - store["sm_start"],
           "a": float(bge.logic.getFramePacingInfo()["interpolation"])}
    for key, obj in sm_objects(scene, owner).items():
        if obj is not None:
            row[key] = (float(obj.worldPosition[0]), float(obj.worldPosition[1]),
                        float(obj.localPosition[0]))
    store["sm_frames"].append(row)


def drive_smooth_motion(owner):
    """Smooth Motion: record the drawn frames for 2 seconds; at 1.0 s Python teleports one object
    (resetSmoothing), at 1.3 s another jumps by Set Object Attribute's Teleport. True once
    done."""
    store = bge.logic.globalDict
    scene = bge.logic.getCurrentScene()
    if "sm_start" not in store:
        store["sm_start"] = time.monotonic()
        store["sm_frames"] = []
        store["sm_owner"] = owner.name
        scene.post_draw.append(sm_record)
    elapsed = time.monotonic() - store["sm_start"]
    objects = sm_objects(scene, owner)
    teleport = objects["teleport"]
    if elapsed >= 1.0 and "sm_teleport_from" not in store and teleport is not None:
        position = teleport.worldPosition.copy()
        store["sm_teleport_from"] = float(position[0])
        position[0] += 50.0
        teleport.worldPosition = position
        teleport.resetSmoothing()
    node = objects["node"]
    if elapsed >= 1.3 and "sm_node_from" not in store and node is not None:
        store["sm_node_from"] = float(node.worldPosition[0])
        node["node_jump"] = True
    if elapsed >= 2.2:
        store["sm_stop"] = True
        return True
    return False


def fp_record():
    """Frame pacing, after each drawn frame (also paused ones): the steps it showed, and the
    pause schedule: 30 frames running, 15 paused, 8 times."""
    store = bge.logic.globalDict
    elapsed = time.monotonic() - store.setdefault("fp_start", time.monotonic())
    if elapsed > 30.0:
        # Never resumed or never checked: end the game rather than hang the suite (no status
        # record is a failure).
        bge.logic.endGame()
        return
    if store.get("fp_stop", False):
        return
    scene = bge.logic.getCurrentScene()
    owner = scene.objects.get(str(store.get("fp_owner", "")))
    if owner is None:
        return
    info = bge.logic.getFramePacingInfo()
    frame = int(store.get("fp_frame", 0))
    store["fp_frame"] = frame + 1
    phase = frame % 45
    store["fp_rows"].append((frame, phase, int(info["frameSteps"]), str(info["pacing"])))
    owner["want_pause"] = phase >= 30
    if frame >= 45 * 8 or elapsed > 20.0:
        owner["want_pause"] = False
        store["fp_stop"] = True


def drive_frame_pacing(owner):
    store = bge.logic.globalDict
    if "fp_rows" not in store:
        store["fp_rows"] = []
        store["fp_owner"] = owner.name
        bge.logic.getCurrentScene().post_draw.append(fp_record)
    return bool(store.get("fp_stop", False))


MO_TILES = ("only", "hide", "material", "plain", "outline", "cavity", "round", "screen",
            "studio", "matcap", "attribute", "random")


def mo_classify(rgb):
    """Color names of an (N, 3) array of 0..255 pixels."""
    import numpy as np

    r, g, b = (rgb[:, channel].astype(np.int32) for channel in range(3))
    high = np.maximum(np.maximum(r, g), b)
    low = np.minimum(np.minimum(r, g), b)
    names = np.full(len(rgb), "other", dtype=object)
    # Later rules win.
    for name, mask in (
        ("magenta", (r > g + 40) & (b > g + 40) & (np.abs(r - b) < 60)),
        ("yellow", (r > b + 40) & (g > b + 40) & (np.abs(r - g) < 60)),
        ("cyan", (g > r + 40) & (b > r + 40) & (np.abs(g - b) < 60)),
        ("blue", (b > r + 40) & (b > g + 40)),
        ("red", (r > g + 40) & (r > b + 40)),
        ("green", (g > r + 40) & (g > b + 40)),
        ("gray", high - low < 25),
        ("black", high < 40),
    ):
        names[mask] = name
    return names


def drive_camera_minimap_options(owner):
    """Minimap options: spawn a red marker copy (Add Object), place the small views over the
    main view (MO_TILES), take a screenshot a few frames later and read each view's
    pixels. True once done."""
    import bpy
    import numpy as np

    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    phase = int(owner.get("mo_phase", 0))
    if phase == 0:
        spawned = scene.addObject(str(owner.get("__ln_acceptance_mo_spawn", "")), owner)
        spawned.worldPosition = (2.0, 0.0, 0.5)
        spawned.color = (1.0, 0.0, 0.0, 1.0)
        width = bge.render.getWindowWidth()
        height = bge.render.getWindowHeight()
        cell_w, cell_h = width // 4, height // 3
        rects = {}
        for index, key in enumerate(MO_TILES):
            camera = scene.objects.get(str(owner.get("__ln_acceptance_mo_" + key, "")))
            if camera is None:
                owner["mo_error"] = "missing camera " + key
                return True
            left = (index % 4) * cell_w + 2
            bottom = (index // 4) * cell_h + 2
            tile_w, tile_h = cell_w - 5, cell_h - 5
            if key == "round":
                tile_w = tile_h = min(tile_w, tile_h)
            camera.setViewport(left, bottom, left + tile_w, bottom + tile_h)
            camera.useViewport = True
            camera.setOnTop()
            rects[key] = (left, bottom, left + tile_w, bottom + tile_h)
        store["mo_rects"] = rects
        owner["mo_phase"] = 1
        return False
    status_path = str(owner.get("__ln_acceptance_status_path", ""))
    directory = os.path.dirname(status_path)
    if phase == 1:
        owner["mo_frames"] = int(owner.get("mo_frames", 0)) + 1
        if owner["mo_frames"] >= 6:
            bge.render.makeScreenshot(os.path.join(directory, "mo_shot.png"))
            owner["mo_phase"] = 2
        return False
    if phase == 2:
        owner["mo_frames"] = int(owner.get("mo_frames", 0)) + 1
        shots = sorted(name for name in os.listdir(directory) if name.startswith("mo_shot"))
        path = os.path.join(directory, shots[-1]) if shots else ""
        size = os.path.getsize(path) if path else 0
        now = time.monotonic()
        store.setdefault("mo_shot_start", now)
        # Before the 8 s quit of add_status_controller's required_seconds.
        if now - store["mo_shot_start"] > 6.0:
            owner["mo_error"] = "no readable screenshot"
            return True
        # Written by a task, and a player frame can take a millisecond: read the file once its
        # size has not changed for 0.3 s, and try again if it can't be read yet.
        if size == 0 or size != store.get("mo_shot_size"):
            store["mo_shot_size"] = size
            store["mo_shot_changed"] = now
            return False
        if now - store["mo_shot_changed"] < 0.3:
            return False
        image = None
        try:
            image = bpy.data.images.load(path)
            width, height = image.size
            pixels = np.empty(width * height * 4, dtype=np.float32)
            image.pixels.foreach_get(pixels)
        except Exception:
            width = height = 0
        finally:
            if image is not None:
                bpy.data.images.remove(image)
        if width == 0 or height == 0:
            store["mo_shot_changed"] = now
            return False
        pixels = (pixels.reshape(height, width, 4)[:, :, :3] * 255.0 + 0.5).astype(np.int32)
        rects = store.get("mo_rects", {})

        def tile(key, inset):
            left, bottom, right, top = rects[key]
            return pixels[bottom + inset:top + 1 - inset, left + inset:right + 1 - inset]

        result = {"size": [width, height], "counts": {}}
        for key in rects:
            values, counts = np.unique(mo_classify(tile(key, 2).reshape(-1, 3)),
                                       return_counts=True)
            result["counts"][key] = {str(v): int(c) for v, c in zip(values, counts)}
        # Same view and size, one switch apart: Cavity changes the sphere's shading, Outline adds
        # magenta (red) to the object edges.
        result["cavity_changed"] = int(np.count_nonzero(
            np.abs(tile("cavity", 2) - tile("only", 2)).max(axis=2) > 8))
        # Screen cavity alone, with the startup file's Screen strengths at 0 (the game uses 1).
        result["screen_changed"] = int(np.count_nonzero(
            np.abs(tile("screen", 2) - tile("only", 2)).max(axis=2) > 8))
        result["outline_redder"] = int(np.count_nonzero(
            tile("outline", 2)[:, :, 0] - tile("plain", 2)[:, :, 0] > 30))
        # Studio lighting shades the markers that Only draws flat.
        result["studio_changed"] = int(np.count_nonzero(
            np.abs(tile("studio", 2) - tile("only", 2)).max(axis=2) > 8))
        # Random colors: the ground (most of the view) is no longer its blue object color.
        random_diff = np.abs(tile("random", 2) - tile("plain", 2)).max(axis=2) > 30
        result["random_changed"] = round(float(np.count_nonzero(random_diff)) /
                                         max(1, random_diff.size), 3)
        # The round view: each corner keeps the main view (as just outside the view), the middle
        # is the minimap (black where the ground is left out), and the pixels on the circle's
        # edge are in between (a smooth edge).
        left, bottom, right, top = rects["round"]
        corner_diffs = []
        corner_sums = []
        for x, outside_x in ((left + 1, left - 2), (right - 1, right + 2)):
            for y in (bottom + 1, top - 1):
                corner_diffs.append(int(np.abs(pixels[y, x] - pixels[y, outside_x]).max()))
                corner_sums.append(int(pixels[y, x].sum()))
        result["round_corner_diffs"] = corner_diffs
        result["round_corner_sums"] = corner_sums
        round_tile = tile("round", 0)
        size_y, size_x = round_tile.shape[:2]
        result["round_middle"] = str(mo_classify(round_tile[size_y // 2:size_y // 2 + 1,
                                                            size_x // 2:size_x // 2 + 1]
                                                 .reshape(-1, 3))[0])
        ys, xs = np.mgrid[0:size_y, 0:size_x]
        radius = size_x / 2.0
        edge = np.abs(np.hypot(xs + 0.5 - radius, ys + 0.5 - radius) - radius) < 0.35
        sums = round_tile.sum(axis=2)
        outside_sum = max(1.0, float(np.mean(corner_sums)))
        between = edge & (sums > 0.15 * outside_sum) & (sums < 0.85 * outside_sum)
        result["round_edge"] = [int(np.count_nonzero(between)), int(np.count_nonzero(edge))]
        store["mo_result"] = result
        owner["mo_phase"] = 3
    return True


CM_FLAGS = ("cm_do_mark", "cm_do_spawn", "cm_do_add", "cm_do_remove", "cm_do_save", "cm_do_undo",
            "cm_do_load", "cm_do_nest", "cm_do_hide", "cm_do_spawn_nest", "cm_do_join",
            "cm_do_end_nest")
CM_STEPS = (
    ("record", "start"), ("do", "cm_do_spawn"), ("record", "spawned"), ("do", "cm_do_add"),
    ("record", "added"), ("do", "cm_do_remove"), ("record", "removed"), ("do", "cm_do_save"),
    ("wait", "cm_save"), ("do", "cm_do_undo"), ("record", "undone"), ("do", "cm_do_load"),
    ("wait", "cm_load"), ("record", "loaded"), ("do", "cm_do_nest"), ("record", "nested"),
    ("do", "cm_do_hide"), ("do", "cm_do_spawn"), ("do", "cm_do_spawn_nest"),
    ("visible", "hidden"), ("do", "cm_do_join"), ("visible", "joined"),
    ("do", "cm_do_end_nest"), ("record", "unnested"),
)


def drive_material_actions(owner):
    """Material actions: after the owner's own (On Init), a copy of the hidden template plays the
    same material action from Python; its material is read once the action is done, then the
    copy is ended and its material copy must be gone. True once done, or after a time limit."""
    import bpy

    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    now = time.monotonic() - store.setdefault("ma_clock", time.monotonic())
    phase = int(owner.get("ma_phase", 0))
    if phase == 0 and now >= 0.6:
        spawned = scene.addObject(str(owner.get("__ln_acceptance_ma_template", "")), owner)
        spawned.worldPosition = (0.0, 3.0, 0.0)
        owner["ma_spawn_before"] = spawned.blenderObject.material_slots[0].material.name
        spawned.playAction(str(owner.get("__ln_acceptance_ma_action", "")), 1.0, 5.0)
        store["ma_spawned"] = spawned
        owner["ma_phase"] = 1
        owner["ma_phase_time"] = now
    elif phase == 1 and now >= float(owner.get("ma_phase_time", 0.0)) + 0.6:
        spawned = store.get("ma_spawned")
        material = spawned.blenderObject.material_slots[0].material
        owner["ma_spawned_material"] = material.name
        owner["ma_spawned_r"] = float(material.diffuse_color[0])
        owner["ma_spawned_g"] = float(material.diffuse_color[1])
        spawned.endObject()
        owner["ma_phase"] = 2
        owner["ma_phase_time"] = now
    elif phase == 2 and now >= float(owner.get("ma_phase_time", 0.0)) + 0.3:
        owner["ma_spawned_copy_gone"] = (
            str(owner.get("ma_spawned_material", "")) not in bpy.data.materials)
        owner["ma_phase"] = 3
        return True
    return now >= 6.0


def drive_camera_visibility_still(owner):
    """Set Camera Visibility in a still scene: the top camera's view is placed in a corner of the
    window, screenshots are taken before (1.0 s) and after (3.0 s) the node tree hides the red
    cube from that camera (1.5 s), and the view's red pixels are counted in each. True once done,
    or after a time limit."""
    import bpy
    import numpy as np

    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    now = time.monotonic() - store.setdefault("cvs_clock", time.monotonic())
    phase = int(owner.get("cvs_phase", 0))
    directory = os.path.dirname(str(owner.get("__ln_acceptance_status_path", "")))
    if phase == 0:
        camera = scene.objects.get(str(owner.get("__ln_acceptance_cvs_camera", "")))
        if camera is None:
            owner["cvs_error"] = "missing camera"
            return True
        size = min(bge.render.getWindowWidth(), bge.render.getWindowHeight()) // 2
        camera.setViewport(2, 2, 2 + size, 2 + size)
        camera.useViewport = True
        camera.setOnTop()
        store["cvs_rect"] = (2, 2, 2 + size, 2 + size)
        owner["cvs_phase"] = 1
        return False
    # Both pictures are taken before either is read: loading an image changes Blender's data, and
    # the update that follows makes every renderer draw again (the change would show anyway).
    shots = (("before", 1.0), ("after", 3.0))
    if phase in (1, 2):
        name, at = shots[phase - 1]
        if now >= at:
            bge.render.makeScreenshot(os.path.join(directory, "cvs_" + name + ".png"))
            owner["cvs_phase"] = phase + 1
        return now >= 8.0
    if phase == 3:
        clock = time.monotonic()
        store.setdefault("cvs_read_start", clock)
        if clock - store["cvs_read_start"] > 4.0:
            owner["cvs_error"] = "no readable screenshot"
            return True
        paths = {}
        for name, _at in shots:
            files = sorted(item for item in os.listdir(directory) if item.startswith("cvs_" + name))
            paths[name] = os.path.join(directory, files[-1]) if files else ""
        sizes = tuple(os.path.getsize(path) if path else 0 for path in paths.values())
        # Written by a task: read the files once their sizes have not changed for 0.3 s.
        if 0 in sizes or sizes != store.get("cvs_sizes"):
            store["cvs_sizes"] = sizes
            store["cvs_sizes_changed"] = clock
            return False
        if clock - store["cvs_sizes_changed"] < 0.3:
            return False
        result = {}
        for name, path in paths.items():
            image = None
            try:
                image = bpy.data.images.load(path)
                width, height = image.size
                pixels = np.empty(width * height * 4, dtype=np.float32)
                image.pixels.foreach_get(pixels)
            except Exception:
                width = height = 0
            finally:
                if image is not None:
                    bpy.data.images.remove(image)
            if width == 0 or height == 0:
                store["cvs_sizes_changed"] = clock
                return False
            left, bottom, right, top = store["cvs_rect"]
            view = pixels.reshape(height, width, 4)[bottom + 3:top - 2, left + 3:right - 2, :3]
            red = (view[:, :, 0] > 0.5) & (view[:, :, 1] < 0.25) & (view[:, :, 2] < 0.25)
            result[name] = int(np.count_nonzero(red))
        store["cvs_result"] = result
        owner["cvs_phase"] = 4
        return True
    return True


def drive_collection_membership(owner):
    """Collection membership: one step of CM_STEPS at a time, three calls after the node tree
    cleared the last step's flag (the step's property writes apply with the clear). A record
    clears every object's cm_seen and asks the tree to count and mark (cm_do_mark), then reads
    the counts and marks. True once done, or after a time limit."""
    scene = bge.logic.getCurrentScene()
    store = bge.logic.globalDict
    start = store.setdefault("cm_clock", time.monotonic())
    phase = int(owner.get("cm_phase", 0))
    if phase >= len(CM_STEPS):
        return True
    if time.monotonic() - start > 12.0:
        owner["cm_error"] = "timed out at step %d %s" % (phase, CM_STEPS[phase])
        return True
    if any(owner.get(flag, False) for flag in CM_FLAGS):
        owner["cm_wait"] = 0
        return False
    owner["cm_wait"] = int(owner.get("cm_wait", 0)) + 1
    if owner["cm_wait"] < 3:
        return False
    owner["cm_wait"] = 0
    kind, name = CM_STEPS[phase]
    if kind == "do":
        owner[name] = True
    elif kind == "wait":
        if not owner.get(name + "_done", False):
            return False
        store[name + "_ok"] = bool(owner.get(name + "_ok", False))
    elif kind == "record":
        if not owner.get("cm_cleared", False):
            for obj in scene.objects:
                if "cm_seen" in obj:
                    obj["cm_seen"] = False
            owner["cm_cleared"] = True
            owner["cm_do_mark"] = True
            return False
        owner["cm_cleared"] = False
        store["cm_" + name] = {
            "direct": int(owner.get("cm_direct", -1)),
            "all": int(owner.get("cm_all", -1)),
            "linked": int(owner.get("cm_linked", -1)),
            "seen": sorted(obj.name for obj in scene.objects if obj.get("cm_seen", False)),
        }
    else:
        def visible(key):
            obj = scene.objects.get(str(owner.get("__ln_acceptance_cm_" + key, "")))
            return bool(obj.visible) if obj is not None else None

        blip = str(owner.get("__ln_acceptance_cm_blip", ""))
        tree = str(owner.get("__ln_acceptance_cm_tree", ""))
        store["cm_" + name] = {
            "copies": [bool(obj.visible) for obj in scene.objects if obj.name == blip],
            "enemy": visible("enemy"),
            "ring": visible("ring"),
            "marker": visible("marker"),
            # Each tree by the Empty instancing it ("" for the asset itself).
            "trees": {(obj.groupObject.name if obj.groupObject is not None else ""):
                      bool(obj.visible) for obj in scene.objects if obj.name == tree},
        }
    owner["cm_phase"] = phase + 1
    return phase + 1 >= len(CM_STEPS)


def check_game_property_types(owner):
    """After Load Game: check every property kind, Python writes, then removing the target."""
    import mathutils

    scene = bge.logic.getCurrentScene()
    stage = owner.get("props_stage", 0)
    if stage == 0 and owner.get("props_loaded", False):
        target = scene.objects["LN_Acceptance_PropsTarget"]
        text = scene.objects["LN_Acceptance_PropsText"]

        def name_of(obj):
            return obj.name if obj is not None else None

        rows = [dict(row, owner=name_of(row.get("owner"))) for row in owner["items"]]
        spawned = owner.get("spawned")
        checks = {
            "vec": tuple(owner["vec"]) == (1.0, 2.0, 3.0),
            "tint": tuple(round(x, 3) for x in owner["tint"]) == (0.5, 0.25, 1.0, 1.0),
            "target": name_of(owner["target"]) == target.name,
            "name_ref": name_of(owner["name_ref"]) == "LN_Acceptance_PropsOther",
            "choice": owner["mode"] == "Hard",
            "numbers": owner["numbers"] == [3, 5, 7],
            "copied_list": owner["copied_list"] == [3, 5, 7],
            "copied_target": name_of(owner["copied_target"]) == target.name,
            "items": rows == [{"item": "Apple", "count": 5, "owner": target.name},
                              {"item": "Sword", "count": 1, "owner": None}],
            "from_driver": tuple(target["from_driver"]) == (1.0, 2.0, 3.0),
            "text": text["Text"] == "Hello",
            "typed_reads": owner["own_vec_float"] == 0.0 and owner["other_vec_float"] == 0.0,
            "spawn_part": spawned is not None and any(child is spawned.get("part")
                                                      for child in spawned.children),
            "spawn_outside": (spawned is not None
                              and name_of(spawned.get("outside")) == "LN_Acceptance_PropsOther"),
        }
        written = [9]
        owner["numbers"] = written
        checks["python_list"] = owner["numbers"] == [9] and owner["numbers"] is not written
        owner["vec"] = mathutils.Vector((4.0, 5.0, 6.0))
        checks["python_vector"] = tuple(owner["vec"]) == (4.0, 5.0, 6.0)
        try:
            owner["numbers"] = 5
            checks["python_type_error"] = False
        except TypeError:
            checks["python_type_error"] = True
        fresh = [1]
        owner["fresh"] = fresh
        checks["python_new_key"] = owner["fresh"] is fresh
        owner["props_report"] = repr(sorted(name for name, ok in checks.items() if not ok))
        target.endObject()
        owner["props_stage"] = 1
    elif stage == 1 and "LN_Acceptance_PropsTarget" not in scene.objects:
        cleared = owner["copied_target"] is None and owner["items"][0]["owner"] is None
        if not cleared:
            owner["props_report"] = owner["props_report"] + " removal"
        owner["props_stage"] = 2
        owner["ui_cycle_done"] = True


def record_grid_slots(owner):
    """While the grid's menu is open, remember each slot's icon mesh, count and text data, once
    for each time the menu opens."""
    slots = [obj for obj in bge.logic.getCurrentScene().objects
             if obj.name == "LN_Acceptance_UIGridSlot"]
    if len(slots) != 3:
        owner["ui_grid_open"] = False
        return
    if not owner.get("ui_grid_open", False):
        owner["ui_grid_open"] = True
        owner["ui_grid_opens"] = int(owner.get("ui_grid_opens", 0)) + 1
    seen = []
    for slot in sorted(slots, key=lambda obj: obj.worldPosition.x):
        icon = next((child for child in slot.children if child.meshes), None)
        text = next((child for child in slot.children if child.get("Text") is not None), None)
        seen.append([icon.meshes[0].name if icon else "",
                     str(text["Text"]) if text else "",
                     text.blenderObject.data.name if text else ""])
    records = json.loads(str(owner.get("ui_grid_seen", "{}")))
    records[str(owner["ui_grid_opens"])] = seen
    owner["ui_grid_seen"] = json.dumps(records)


def record_grid_scrollbar(owner):
    """Each phase of the menu: are the scrollbar and its handle shown, and the handle's place."""
    phase = int(owner.get("ui_phase", 0))
    if phase not in (1, 2, 3):
        return
    scene = bge.logic.getCurrentScene()
    bar = scene.objects.get("LN_Acceptance_UIScrollBar")
    handle = scene.objects.get("LN_Acceptance_UIScrollHandle")
    seen = json.loads(str(owner.get("ui_scroll_seen", "{}")))
    seen[str(phase)] = [bool(bar is not None and bar.visible),
                        bool(handle is not None and handle.visible),
                        None if handle is None else round(handle.worldPosition.y, 3),
                        None if handle is None else round(handle.worldScale.y, 3)]
    owner["ui_scroll_seen"] = json.dumps(seen)


def color_tuple(value):
    return tuple(float(value[index]) for index in range(4))


def orientation_tuple(value):
    if hasattr(value, "to_euler"):
        value = value.to_euler()
    return vector_tuple(value)


def close_tuple(observed, expected, tolerance):
    return all(abs(observed[index] - expected[index]) <= tolerance for index in range(3))


def close_color(observed, expected, tolerance):
    return all(abs(observed[index] - expected[index]) <= tolerance for index in range(4))


def write_status(owner, payload):
    status_path = owner.get("__ln_acceptance_status_path", "")
    if not status_path:
        return
    with open(status_path, "a", encoding="utf-8") as status_file:
        status_file.write(json.dumps(payload, sort_keys=True) + "\n")


def expected_vector(owner):
    return (
        float(owner.get("__ln_acceptance_expected_x", 0.0)),
        float(owner.get("__ln_acceptance_expected_y", 0.0)),
        float(owner.get("__ln_acceptance_expected_z", 0.0)),
    )


def expected_rotation(owner):
    return (
        float(owner.get("__ln_acceptance_expected_rx", 0.0)),
        float(owner.get("__ln_acceptance_expected_ry", 0.0)),
        float(owner.get("__ln_acceptance_expected_rz", 0.0)),
    )


def expected_color(owner):
    return (
        float(owner.get("__ln_acceptance_expected_r", 1.0)),
        float(owner.get("__ln_acceptance_expected_g", 1.0)),
        float(owner.get("__ln_acceptance_expected_b", 1.0)),
        float(owner.get("__ln_acceptance_expected_a", 1.0)),
    )


def expected_named_vector(owner, prefix):
    return (
        float(owner.get(f"__ln_acceptance_{prefix}_x", 0.0)),
        float(owner.get(f"__ln_acceptance_{prefix}_y", 0.0)),
        float(owner.get(f"__ln_acceptance_{prefix}_z", 0.0)),
    )


def mutate_runtime_structure(owner):
    if owner.get("__ln_acceptance_mode", "") != "phase6_save_load_structure":
        return

    object_name = str(owner.get("__ln_acceptance_structure_object", ""))
    runtime_target = next(
        (
            candidate
            for candidate in bge.logic.getCurrentScene().objects
            if candidate.name == object_name
        ),
        None,
    )
    if (
        runtime_target is None
        or not bool(runtime_target.get("structure_mutate_output", False))
        or bool(runtime_target.get("structure_output_mutated", False))
    ):
        return
    try:
        import bpy

        modifier_name = str(owner.get("__ln_acceptance_structure_modifier", ""))
        input_identifier = str(owner.get("__ln_acceptance_structure_input", ""))
        output_identifier = str(owner.get("__ln_acceptance_structure_output", ""))
        transient_modifier = str(
            owner.get("__ln_acceptance_structure_transient_modifier", ""))
        removed_modifier = str(
            owner.get("__ln_acceptance_structure_removed_modifier", ""))
        obj = bpy.data.objects.get(object_name)
        modifier = obj.modifiers.get(modifier_name) if obj is not None else None
        if modifier is None or modifier.type != "NODES":
            raise RuntimeError("the authored Geometry Nodes modifier is missing")
        input_state = getattr(modifier.properties.inputs, input_identifier)
        observed_mutated_value = float(input_state.value)
        expected_mutated_value = float(
            owner.get("__ln_acceptance_structure_mutated_value", 9.5))
        if abs(observed_mutated_value - expected_mutated_value) > 0.0001:
            raise RuntimeError(
                "Set Geometry Nodes Input did not apply the runtime mutation")
        if obj.modifiers.get(transient_modifier) is None:
            raise RuntimeError(
                "Assign Geometry Nodes Modifier did not append the runtime modifier")
        removed = obj.modifiers.get(removed_modifier)
        if removed is None or removed.type != "NODES":
            raise RuntimeError(
                "the Geometry Nodes modifier selected for removal is missing")
        obj.modifiers.remove(removed)
        output_state = getattr(modifier.properties.outputs, output_identifier)
        output_state.attribute_name = "mutated_attr"
        modifier.show_viewport = False

        internal_node_name = str(
            owner.get("__ln_acceptance_structure_internal_node", ""))
        internal_socket_name = str(
            owner.get("__ln_acceptance_structure_internal_socket", ""))
        internal_node = modifier.node_group.nodes.get(internal_node_name)
        if internal_node is None:
            raise RuntimeError("the runtime Geometry Nodes value node is missing")
        internal_node.inputs[internal_socket_name].default_value = float(
            owner.get("__ln_acceptance_structure_internal_mutated_value", 1.0))
        internal_node.mute = True

        compositor_tree_name = str(
            owner.get("__ln_acceptance_structure_compositor_tree", ""))
        compositor_node_name = str(
            owner.get("__ln_acceptance_structure_compositor_node", ""))
        compositor_socket_name = str(
            owner.get("__ln_acceptance_structure_compositor_socket", ""))
        compositor_tree = bpy.data.node_groups.get(compositor_tree_name)
        compositor_node = (
            compositor_tree.nodes.get(compositor_node_name)
            if compositor_tree is not None
            else None
        )
        if compositor_node is None:
            raise RuntimeError("the saved compositor node is missing")
        compositor_node.inputs[compositor_socket_name].default_value = float(
            owner.get("__ln_acceptance_structure_compositor_mutated_value", 75.0))
        compositor_node.mute = True
        transient_compositor_name = str(
            owner.get("__ln_acceptance_structure_transient_compositor_tree", ""))
        transient_compositor = bpy.data.node_groups.get(transient_compositor_name)
        if (
            transient_compositor is None
            or transient_compositor.bl_idname != "CompositorNodeTree"
        ):
            raise RuntimeError("the transient compositor node tree is missing")
        bpy.context.scene.compositing_node_group = transient_compositor
    except Exception:
        runtime_target["structure_mutation_error"] = traceback.format_exc()
    finally:
        runtime_target["structure_output_mutated"] = True


def per_player_menu_state(owner):
    """Per-player menus: each copy of the menu's panel, as (user, owner gold, its texts sorted,
    screen x, scale x)."""
    scene = bge.logic.getCurrentScene()
    panel_name = str(owner.get("__ln_acceptance_panel", ""))
    camera = scene.objects.get(str(owner.get("__ln_acceptance_menu_camera", "")))
    copies = []
    for obj in scene.objects:
        if obj.name != panel_name:
            continue
        texts = []
        corner = badge = None
        for child in obj.childrenRecursive:
            data = getattr(child.blenderObject, "data", None)
            if data is not None and hasattr(data, "body"):
                texts.append(data.body.strip())
            if child.name.endswith("_PPCorner"):
                corner = child
            if child.name.endswith("_PPBadge"):
                badge = child
        screen_x = float(camera.getScreenPosition(obj)[0]) if camera is not None else -1.0
        anchored = ()
        if camera is not None and corner is not None and badge is not None:
            corner_screen = camera.getScreenPosition(corner)
            offset = badge.worldPosition - corner.worldPosition
            anchored = (round(float(corner_screen[0]), 3), round(float(corner_screen[1]), 3),
                        round(float(corner.worldScale[0]), 3), round(float(offset.x), 3),
                        round(float(offset.y), 3))
        user = int(obj.get("menu_user", -1))
        popups = sum(1 for other in scene.objects if other.name.endswith("_PPPopup")
                     and int(other.get("popup_user", -9)) == user)
        copies.append((user, int(obj.get("owner_gold", -1)), sorted(texts), round(screen_x, 3),
                       round(float(obj.worldScale[0]), 3), anchored, popups))
    return sorted(copies)


def check_owner(owner):
    mode = owner.get("__ln_acceptance_mode", "quit_only")
    tolerance = float(owner.get("__ln_acceptance_tolerance", 0.0001))
    expected = expected_vector(owner)

    if mode == "quit_only":
        return True, {"mode": mode}

    if mode == "ui_example_file":
        # Add > Game UI > Open Complete UI Example: its Game tree shows the HUD at the start, and
        # with every menu open their trees fill the grids from the slot templates (one each before).
        names = [obj.name for obj in bge.logic.getCurrentScene().objects]
        counts = {name: names.count(name) for name in (
            "Stamina Bar", "Value Text", "Slot", "RecipeSlot", "BagSlot", "ChestRow")}
        return (counts["Stamina Bar"] == 1 and counts["Value Text"] == 1 and
                all(counts[name] > 1 for name in ("Slot", "RecipeSlot", "BagSlot", "ChestRow"))), {
            "mode": mode,
            "counts": counts,
        }

    if mode == "ui_templates":
        # Add > Game UI > Templates: with every menu the templates made open, their trees fill
        # the grids; each cell of the equipment once; the late Settings menu's buttons are there;
        # the Pickups template's items are in the world.
        names = [obj.name for obj in bge.logic.getCurrentScene().objects]
        counts = {name: names.count(name) for name in (
            "Slot", "RecipeSlot", "WeaponSlot", "BagSlot", "ChestRow", "WalkSlider",
            "SettingsButton", "Settings", "Resume")}
        counts["pickups"] = sum(1 for name in names if name.startswith("Pickup_"))
        return (all(counts[name] > 1 for name in ("Slot", "RecipeSlot", "BagSlot", "ChestRow")) and
                all(counts[name] == 1 for name in ("WeaponSlot", "WalkSlider", "SettingsButton",
                                                   "Settings", "Resume")) and
                counts["pickups"] > 0), {
            "mode": mode,
            "counts": counts,
        }

    if mode == "game_property_string":
        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_expected_object", "")
        property_name = owner.get("__ln_acceptance_expected_property", "")
        expected_value = owner.get("__ln_acceptance_expected_string", "")
        target = scene.objects.get(target_name)
        observed = None if target is None else target.get(property_name, None)
        return observed == expected_value, {
            "mode": mode,
            "object": target_name,
            "property": property_name,
            "observed": observed,
            "expected": expected_value,
        }

    if mode == "inactive_overlay_collection":
        import bpy

        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_expected_object", "")
        child_name = owner.get("__ln_acceptance_expected_child", "")
        camera_name = owner.get("__ln_acceptance_expected_camera", "")
        collection_name = owner.get("__ln_acceptance_expected_collection", "")
        property_name = owner.get("__ln_acceptance_expected_property", "")
        expected_value = owner.get("__ln_acceptance_expected_string", "")
        target = scene.objects.get(target_name)
        child = scene.objects.get(child_name)
        camera = scene.objects.get(camera_name)
        inactive_objects = getattr(scene, "objectsInactive", None)
        source_camera = (
            None if inactive_objects is None else inactive_objects.get(camera_name)
        )
        observed = None if target is None else target.get(property_name, None)
        child_observed = None if child is None else child.get(property_name, None)
        # Object properties of the live copies point at the live copies, never the templates.
        def reference_state(obj, name, live):
            value = None if obj is None else obj.get(name)
            if value is None:
                return "none"
            return "live" if value is live else f"other {value.name} {value.invalid}"

        references_ok = (
            target is not None
            and camera is not None
            and child is not None
            and target.get("partner") is camera
            and target.get("part") is child
            and camera.get("watch") is target
        )
        item_init_observed = (
            None if target is None else target.get("overlay_item_init", None)
        )
        item_fixed_observed = (
            None if target is None else target.get("overlay_item_fixed", None)
        )
        child_init_observed = (
            None if child is None else child.get("overlay_item_init", None)
        )
        child_fixed_observed = (
            None if child is None else child.get("overlay_item_fixed", None)
        )
        camera_init_observed = (
            None if camera is None else camera.get("overlay_camera_init", None)
        )
        camera_fixed_observed = (
            None if camera is None else camera.get("overlay_camera_fixed", None)
        )
        cycle_ok = bool(owner.get("overlay_cycle_ok", False))
        cycle_error = str(owner.get("overlay_cycle_error", ""))
        result = owner.get("overlay_result", "")
        render_object = None if target is None else target.blenderObject
        child_render_object = None if child is None else child.blenderObject
        blender_scene = bpy.data.scenes.get(scene.name)
        scene_root = None if blender_scene is None else blender_scene.collection
        render_visible = render_object is not None and bool(render_object.visible_get())
        child_render_visible = (
            child_render_object is not None and bool(child_render_object.visible_get())
        )
        staged_in_scene_root = (
            scene_root is not None
            and render_object is not None
            and scene_root.objects.get(render_object.name) == render_object
        )
        child_staged_in_scene_root = (
            scene_root is not None
            and child_render_object is not None
            and scene_root.objects.get(child_render_object.name) == child_render_object
        )
        source_collection = bpy.data.collections.get(collection_name)
        collection_still_disabled = (
            source_collection is not None and bool(source_collection.hide_viewport)
        )
        source_object = bpy.data.objects.get(target_name)
        source_still_hidden = (
            source_object is not None and not bool(source_object.visible_get())
        )
        ok = (
            result == "accepted"
            and observed == expected_value
            and child_observed == expected_value
            and item_init_observed == "ran"
            and item_fixed_observed == "ran"
            and child_init_observed == "ran"
            and child_fixed_observed == "ran"
            and camera_init_observed == "ran"
            and camera_fixed_observed == "ran"
            and source_camera is not None
            and camera is not source_camera
            and render_visible
            and child_render_visible
            and staged_in_scene_root
            and child_staged_in_scene_root
            and collection_still_disabled
            and source_still_hidden
            and cycle_ok
            and references_ok
        )
        return ok, {
            "mode": mode,
            "done_result": result,
            "object": target_name,
            "child": child_name,
            "camera": camera_name,
            "property": property_name,
            "observed": observed,
            "child_observed": child_observed,
            "item_init_observed": item_init_observed,
            "item_fixed_observed": item_fixed_observed,
            "child_init_observed": child_init_observed,
            "child_fixed_observed": child_fixed_observed,
            "camera_init_observed": camera_init_observed,
            "camera_fixed_observed": camera_fixed_observed,
            "camera_source_still_inactive": source_camera is not None,
            "camera_runtime_is_distinct": camera is not None and camera is not source_camera,
            "expected": expected_value,
            "render_visible": render_visible,
            "child_render_visible": child_render_visible,
            "staged_in_scene_root": staged_in_scene_root,
            "child_staged_in_scene_root": child_staged_in_scene_root,
            "collection_still_disabled": collection_still_disabled,
            "source_still_hidden": source_still_hidden,
            "cycle_ok": cycle_ok,
            "references_ok": references_ok,
            "partner_state": reference_state(target, "partner", camera),
            "part_state": reference_state(target, "part", child),
            "watch_state": reference_state(camera, "watch", target),
            "cycle_error": cycle_error,
        }

    if mode == "ui_elements":
        scene = bge.logic.getCurrentScene()
        selected_button = scene.objects.get("LN_Acceptance_UIButtonTab1")
        icon = scene.objects.get("LN_Acceptance_UIAnchoredIcon")
        badge = scene.objects.get("LN_Acceptance_UIAnchoredBadge")
        overlay_camera = scene.objects.get("LN_Acceptance_UIOverlayCamera")
        selected_scale = (
            float(selected_button.localScale.x) if selected_button is not None else 0.0)
        icon_screen = (
            tuple(float(value) for value in overlay_camera.getScreenPosition(icon))
            if icon is not None and overlay_camera is not None else None
        )
        width = max(1, bge.render.getWindowWidth())
        height = max(1, bge.render.getWindowHeight())
        # Authored 10% / 10% from the top-left of a 16:9 frame: 0.17778 frame heights across.
        expected_icon = (0.177778 * height / width, 0.1)
        icon_ok = icon_screen is not None and all(
            abs(observed - wanted) < 0.01 for observed, wanted in zip(icon_screen, expected_icon))
        # The anchored child rides on its anchored parent instead of being anchored twice.
        badge_offset = (
            tuple(float(value) for value in (badge.worldPosition - icon.worldPosition))
            if badge is not None and icon is not None else None)
        badge_ok = badge_offset is not None and all(
            abs(observed - wanted) < 1.0e-3
            for observed, wanted in zip(badge_offset, (0.3, -0.2, 0.0)))
        focus_name = str(owner.get("ui_focus_name", ""))
        focus_event = str(owner.get("ui_focused_action", ""))
        # Tab1 is selected for keyboard and gamepad, but a mouse player sees neither its hover
        # look (the authored scale 0.5 stays) nor a Focus event.
        return (focus_name == "LN_Acceptance_UIButtonTab1" and focus_event == "" and
                abs(selected_scale - 0.5) < 1.0e-3 and icon_ok and badge_ok), {
            "mode": mode,
            "focus_name": focus_name,
            "focus_event": focus_event,
            "selected_scale": selected_scale,
            "icon_screen": icon_screen,
            "expected_icon": expected_icon,
            "badge_offset": badge_offset,
        }

    if mode == "ui_menu_focus_restore":
        submenu_focus = str(owner.get("ui_submenu_focus", ""))
        restored_focus = str(owner.get("ui_restored_focus", ""))
        focus_failed = bool(owner.get("ui_set_focus_failed", False))
        # Closing the submenu gives the menu below its selection back.
        return (not focus_failed and submenu_focus == "LN_Acceptance_UISubButton" and
                restored_focus == "LN_Acceptance_UIMenuButtonB"), {
            "mode": mode,
            "submenu_focus": submenu_focus,
            "restored_focus": restored_focus,
            "set_focus_failed": focus_failed,
        }

    if mode == "ui_sound_events":
        last_move = str(owner.get("last_move", ""))
        last_menu = str(owner.get("last_menu", "none"))
        last_user = int(owner.get("last_user", -1))
        move_count = float(owner.get("move_count", 0.0))
        # The highlight moved to B under the pointer: Move with B's Action, player 0, no menu.
        return (last_move == "b" and last_menu == "" and last_user == 0 and
                move_count >= 1.0), {
            "mode": mode,
            "last_move": last_move,
            "last_menu": last_menu,
            "last_user": last_user,
            "move_count": move_count,
        }

    if mode == "smooth_motion":
        store = bge.logic.globalDict
        # Drawn frames once the steps' history exists; every object moves 0.1 a step.
        frames = [row for row in store.get("sm_frames", []) if row["t"] >= 0.4]
        step = 0.1
        errors = {"interpolate": 0.0, "extrapolate": 0.0, "none": 0.0, "child": 0.0}
        teleport_from = float(store.get("sm_teleport_from", 0.0))
        node_from = float(store.get("sm_node_from", 0.0))
        teleport_streaks = node_streaks = between = 0
        teleported = node_jumped = False
        for row in frames:
            a = row["a"]
            if "mover" in row:
                drawn, _drawn_y, true = row["mover"]
                # Interpolate: one step late, a share `a` of the last step's move.
                errors["interpolate"] = max(errors["interpolate"],
                                            abs(drawn - (true - step * (1.0 - a))))
                between += 0.05 < a < 0.95
            if "ball" in row:
                drawn, _drawn_y, true = row["ball"]
                errors["extrapolate"] = max(errors["extrapolate"],
                                            abs(drawn - (true + step * a)))
            if "plain" in row:
                drawn, _drawn_y, true = row["plain"]
                errors["none"] = max(errors["none"], abs(drawn - true))
            if "child" in row and "mover" in row:
                # A None child is drawn with its smoothed parent, 2 to its side.
                errors["child"] = max(errors["child"], abs(row["child"][0] - row["mover"][0]),
                                      abs(row["child"][1] - row["mover"][1] - 2.0))
            if "teleport" in row and "sm_teleport_from" in store:
                drawn = row["teleport"][0]
                teleport_streaks += teleport_from + 1.0 < drawn < teleport_from + 49.0
                teleported |= drawn >= teleport_from + 49.0
            if "node" in row and "sm_node_from" in store:
                drawn = row["node"][0]
                node_streaks += node_from + 1.0 < drawn < 499.0
                node_jumped |= drawn >= 499.0
        ok = (len(frames) >= 30 and between >= 10 and
              all(error < 0.002 for error in errors.values()) and
              teleported and teleport_streaks == 0 and node_jumped and node_streaks == 0)
        return ok, {
            "mode": mode,
            "frames": len(frames),
            "between_steps": between,
            "errors": errors,
            "teleported": teleported,
            "teleport_streaks": teleport_streaks,
            "node_jumped": node_jumped,
            "node_streaks": node_streaks,
        }

    if mode == "frame_pacing":
        rows = bge.logic.globalDict.get("fp_rows", [])
        # Running frames after the first 90 (loading), not the 3 after a resume.
        running = [row for row in rows if row[0] >= 90 and 3 <= row[1] < 30]
        single = sum(1 for row in running if row[2] == 1)
        flips = sum(1 for before, after in zip(running, running[1:])
                    if after[0] == before[0] + 1 and {before[2], after[2]} == {0, 2})
        paused_steps = sum(row[2] for row in rows if 33 <= row[1] < 45)
        pacings = sorted({row[3] for row in rows})
        ok = (len(running) >= 150 and single >= 0.97 * len(running) and flips == 0 and
              paused_steps == 0 and pacings == ["LIMIT"])
        return ok, {
            "mode": mode,
            "frames": len(rows),
            "running": len(running),
            "single_step": single,
            "flips": flips,
            "paused_steps": paused_steps,
            "pacings": pacings,
        }

    if mode == "sound_groups":
        import aud
        device_volume = float(aud.Device().volume)
        done = bool(owner.get("volume_done", False))
        # Set Volume Master 0.25 is the device's volume; Music and Effects only touch their sounds.
        return (done and abs(device_volume - 0.25) < 0.001), {
            "mode": mode,
            "volume_done": done,
            "device_volume": device_volume,
        }

    if mode == "ui_paused_routes":
        entered = bool(owner.get("ui_enter_while_paused", False))
        update_count = float(owner.get("update_count", 0.0))
        # The Enter route ran in a paused frame; On Update ran at most before the pause began.
        return (entered and update_count <= 2.0), {
            "mode": mode,
            "entered_while_paused": entered,
            "update_count": update_count,
        }

    if mode == "game_property_types":
        report = str(owner.get("props_report", ""))
        # Every property kind read and written in the game and restored by Load Game.
        return (bool(owner.get("ui_cycle_done", False)) and report == "[]"), {
            "mode": mode,
            "loaded": bool(owner.get("props_loaded", False)),
            "failed_checks": report,
        }

    if mode == "ui_grid_template":
        records = json.loads(str(owner.get("ui_grid_seen", "{}")))
        left = sum(1 for obj in bge.logic.getCurrentScene().objects
                   if obj.name == "LN_Acceptance_UIGridSlot")

        def grid_ok(seen):
            return (seen is not None and len(seen) == 3 and
                    [slot[0] for slot in seen[:2]] == ["LN_Acceptance_UIGridAppleMesh",
                                                       "LN_Acceptance_UIGridGemMesh"] and
                    [slot[1] for slot in seen[:2]] == ["4", ""] and
                    len({slot[2] for slot in seen}) == 3)

        # [Apple x4, Gem x1] in 1 x 3 slots made from one template, each of the two times the
        # menu opens: library icons, a count only above 1, every count on its own text data, and
        # no copy left after closing the menu.
        return (bool(owner.get("ui_cycle_done", False)) and sorted(records) == ["1", "2"] and
                all(grid_ok(records.get(key)) for key in ("1", "2")) and left == 0), {
            "mode": mode,
            "cycle_done": bool(owner.get("ui_cycle_done", False)),
            "seen": records,
            "slots_left": left,
        }

    if mode == "ui_menu_pause_toggle":
        scene = bge.logic.getCurrentScene()
        names = ("opened", "toggled_closed", "paused_frames", "steps_at_pause", "steps_at_close",
                 "steps")
        seen = {name: owner.get(name) for name in names}
        panels = sum(1 for obj in scene.objects if obj.name == "LN_Acceptance_PausePanel")
        # Opened with Pause Game: the game stopped (no Logic + Physics steps) while While Paused
        # frames ran; opened again with Toggle, it closed (Closed ran) and the game went on. The
        # menu collection stayed visible in the viewport, yet no live copy of it is left.
        return (bool(seen["opened"]) and bool(seen["toggled_closed"]) and
                int(seen["paused_frames"] or 0) >= 30 and
                seen["steps_at_close"] == seen["steps_at_pause"] and
                int(seen["steps"] or 0) > int(seen["steps_at_close"] or 0) and panels == 0), {
            "mode": mode,
            "seen": seen,
            "panels": panels,
        }

    if mode == "ui_text_values_bar":
        scene = bge.logic.getCurrentScene()

        def body(name):
            obj = scene.objects.get(name)
            return None if obj is None else obj.blenderObject.data.body

        def fill(name):
            obj = scene.objects.get(name)
            return None if obj is None else [round(obj.localScale.x, 3), round(obj.worldPosition.x, 3)]

        texts = [body(name) for name in ("LN_Acceptance_TVText", "LN_Acceptance_TVParentText",
                                          "LN_Acceptance_TVMissingText")]
        fills = [fill(name) for name in ("LN_Acceptance_TVBarFill", "LN_Acceptance_TVChildBarFill",
                                         "LN_Acceptance_TVZeroBarFill")]
        # Each slot bar's fill: its scale, and its centre from its bar's (2 wide, from the left).
        slot_fills = [(round(obj.localScale.x, 3),
                       round(obj.worldPosition.x - obj.parent.worldPosition.x, 3))
                      for obj in scene.objects if obj.name == "LN_Acceptance_TVSlotBarFill"]
        # Values in text follow the game properties ({Object.property}, {property} of a parent,
        # :1 for one decimal) and unknown ones stay as written; a Bar's fill is value / Max
        # Property along the bar from its left edge (2 wide at x = 0: left edge -1), and one on a
        # child reads its parent's value, capped at full. All four grid slots show 90 of 120,
        # also the two copied from the template after its bar showed it, and a Max Property of 0
        # shows empty.
        return (bool(owner.get("ui_cycle_done", False)) and
                texts == ["Gold 42 | 2.3", "42/90", "{Missing.gold} {nothing}"] and
                fills[0] is not None and abs(fills[0][0] - 0.75) < 0.002 and
                abs(fills[0][1] - (-1.0 + 0.75)) < 0.01 and
                fills[1] is not None and abs(fills[1][0] - 1.0) < 0.002 and
                fills[2] is not None and fills[2][0] < 0.002 and len(slot_fills) == 4 and
                all(abs(scale - 0.75) < 0.002 and abs(offset - (-1.0 + 0.75)) < 0.01
                    for scale, offset in slot_fills)), {
            "mode": mode,
            "texts": texts,
            "fills": fills,
            "slot_fills": slot_fills,
        }

    if mode == "ui_menu_camera_hidden":
        scene = bge.logic.getCurrentScene()
        names = set(obj.name for obj in scene.objects)
        # The camera menus are shown with starts live although its collection is disabled in the
        # viewport (its own tree ran); the menu's objects stay asleep until it opens.
        return (bool(owner.get("ui_cycle_done", False)) and
                bool(owner.get("camera_awake", False)) and
                "LN_Acceptance_MCCamera" in names and "LN_Acceptance_MCPanel" not in names), {
            "mode": mode,
            "camera_awake": bool(owner.get("camera_awake", False)),
            "camera_live": "LN_Acceptance_MCCamera" in names,
            "panel_live": "LN_Acceptance_MCPanel" in names,
        }

    if mode == "ui_grid_parented_template":
        scene = bge.logic.getCurrentScene()
        panel = scene.objects.get("LN_Acceptance_GPPanel")
        slots = [obj for obj in scene.objects if obj.name == "LN_Acceptance_GPSlot"]
        expected = [(0.0, 0.0), (2.0, 0.0), (0.0, -2.0), (2.0, -2.0)]
        lattice = None
        if panel is not None and len(slots) == 4:
            turn = panel.worldOrientation.inverted()
            # One of them is the template, the top left cell: the others are 2 apart (Spacing 1
            # in the panel's units, which are 2) along the panel's turned axes.
            for origin in slots:
                cells = [turn @ (obj.worldPosition - origin.worldPosition) for obj in slots]
                if all(any(abs(cell.x - x) < 0.01 and abs(cell.y - y) < 0.01 for cell in cells)
                       for x, y in expected):
                    lattice = [[round(cell.x, 3), round(cell.y, 3)] for cell in cells]
                    break
        parents = sorted(str(obj.parent.name) if obj.parent is not None else "" for obj in slots)
        scales = [round(obj.worldScale.x, 3) for obj in slots]
        # Object Grid View copies a template that is a child of a moved, turned and scaled panel:
        # around the template, along the panel's axes, in its units, as children of the panel.
        return (bool(owner.get("ui_cycle_done", False)) and lattice is not None and
                parents == ["LN_Acceptance_GPPanel"] * 4 and
                all(abs(scale - 2.0) < 0.002 for scale in scales)), {
            "mode": mode,
            "count": len(slots),
            "lattice": lattice,
            "positions": [[round(value, 3) for value in obj.worldPosition] for obj in slots],
            "parents": parents,
            "scales": scales,
        }

    if mode == "ui_grid_pages":
        seen = [str(owner.get(key, "")) for key in ("pages_start", "pages_next", "pages_show",
                                                    "pages_top")]
        # 10 one-item entries in 2 x 2 slots: pages of 2 rows, 3 pages. Next Page goes 2 rows
        # on, Show Item 9 shows the last rows, Top goes back; Page, Page Count and First Visible
        # are numbers formatted straight into text.
        return (bool(owner.get("ui_cycle_done", False)) and
                seen == ["1 / 3 / 0", "2 / 3 / 4", "3 / 3 / 6", "1 / 3 / 0"]), {
            "mode": mode,
            "cycle_done": bool(owner.get("ui_cycle_done", False)),
            "seen": seen,
        }

    if mode == "ui_grid_scrollbar":
        seen = json.loads(str(owner.get("ui_scroll_seen", "{}")))

        def near(value, expected):
            return value is not None and abs(value - expected) <= 0.01

        fits, top, end = seen.get("1"), seen.get("2"), seen.get("3")
        # 2 items fit 1 x 2 slots: no scrollbar. 6 items: a third is in view, so the handle is a
        # third of the bar, at its top, and at its bottom once First Item shows the last row.
        return (bool(owner.get("ui_cycle_done", False)) and fits is not None and
                fits[:2] == [False, False] and top is not None and top[:2] == [True, True] and
                near(top[2], 0.667) and near(top[3], 0.333) and end is not None and
                near(end[2], -0.667) and near(end[3], 0.333)), {
            "mode": mode,
            "cycle_done": bool(owner.get("ui_cycle_done", False)),
            "seen": seen,
        }

    if mode == "ui_overlay_visibility":
        scene = bge.logic.getCurrentScene()
        states = {}
        for name in ("LN_Acceptance_UIHudPlain", "LN_Acceptance_UIHudTemplate"):
            obj = scene.objects.get(name)
            states[name] = None if obj is None else bool(obj.visible)
        menu_opened = bool(owner.get("ui_menu_opened", False))
        cycle_done = bool(owner.get("ui_cycle_done", False))
        # Set Collection Visibility hides a HUD of plain visible objects too, opening and
        # closing a menu does not show a hidden HUD again, and showing one HUD leaves the other.
        return (menu_opened and cycle_done and
                states["LN_Acceptance_UIHudPlain"] is True and
                states["LN_Acceptance_UIHudTemplate"] is False and
                not bool(owner.get("ui_plain_visible_with_menu", True)) and
                not bool(owner.get("ui_template_visible_with_menu", True))), {
            "mode": mode,
            "menu_opened": menu_opened,
            "cycle_done": cycle_done,
            "visible": states,
            "plain_visible_with_menu": owner.get("ui_plain_visible_with_menu", None),
            "template_visible_with_menu": owner.get("ui_template_visible_with_menu", None),
        }

    if mode == "object_menu_session":
        import bpy

        scene = bge.logic.getCurrentScene()
        item_name = str(owner.get("__ln_acceptance_expected_object", ""))
        collection_name = str(owner.get("__ln_acceptance_expected_collection", ""))
        inactive_objects = getattr(scene, "objectsInactive", None)
        source_item = (
            None if inactive_objects is None else inactive_objects.get(item_name)
        )
        source_collection = bpy.data.collections.get(collection_name)
        first_handle = int(owner.get("menu_handle_first", 0))
        second_handle = int(owner.get("menu_handle_second", 0))
        first_open_result = int(owner.get("menu_open_first_result", -1))
        second_open_result = int(owner.get("menu_open_second_result", -1))
        first_close_result = int(owner.get("menu_close_first_result", -1))
        second_close_result = int(owner.get("menu_close_second_result", -1))
        repeat_close_result = int(owner.get("menu_close_repeat_result", -1))
        repeat_closed = bool(owner.get("menu_repeat_closed", False))
        external_ready = bool(owner.get("menu_external_ready", False))
        external_removed = bool(owner.get("menu_external_removed", False))
        external_handle = int(owner.get("menu_handle_external", 0))
        external_open_result = int(owner.get("menu_open_external_result", -1))
        external_close_result = int(owner.get("menu_close_external_result", -1))
        runtime_item = scene.objects.get(item_name)
        ok = (
            not bool(owner.get("menu_session_failed", False))
            and bool(owner.get("menu_item_logic_ran", False))
            and first_handle > 0
            and second_handle > 0
            and first_handle != second_handle
            and first_open_result == 0
            and second_open_result == 0
            and first_close_result == 0
            and second_close_result == 0
            and repeat_close_result == 1
            and external_handle > 0
            and external_open_result == 0
            and external_removed
            and external_close_result == 0
            and bool(owner.get("menu_session_done", False))
            and runtime_item is None
            and source_item is not None
            and source_collection is not None
            and bool(source_collection.hide_viewport)
        )
        return ok, {
            "mode": mode,
            "session_done": bool(owner.get("menu_session_done", False)),
            "session_failed": bool(owner.get("menu_session_failed", False)),
            "item_logic_ran": bool(owner.get("menu_item_logic_ran", False)),
            "first_handle": first_handle,
            "second_handle": second_handle,
            "first_open_result": first_open_result,
            "second_open_result": second_open_result,
            "first_close_result": first_close_result,
            "second_close_result": second_close_result,
            "repeat_close_result": repeat_close_result,
            "repeat_closed": repeat_closed,
            "external_ready": external_ready,
            "external_removed": external_removed,
            "external_handle": external_handle,
            "external_open_result": external_open_result,
            "external_close_result": external_close_result,
            "runtime_item_removed": runtime_item is None,
            "source_item_inactive": source_item is not None,
            "collection_still_disabled": (
                source_collection is not None and bool(source_collection.hide_viewport)
            ),
        }

    if mode in {"position", "unchanged_position"}:
        observed = vector_tuple(owner.worldPosition)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "orientation":
        observed = orientation_tuple(owner.worldOrientation)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "velocity":
        observed = vector_tuple(owner.worldLinearVelocity)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "impulse":
        observed = vector_tuple(owner.worldLinearVelocity)
        min_speed = float(owner.get("__ln_acceptance_min_speed", 0.0))
        speed = math.sqrt(sum(component * component for component in observed))
        return speed >= min_speed, {
            "mode": mode,
            "observed": observed,
            "speed": speed,
            "min_speed": min_speed,
        }

    if mode == "property_branch":
        observed = vector_tuple(owner.worldPosition)
        moved = bool(owner.get("moved", False))
        return close_tuple(observed, expected, tolerance) and moved, {
            "mode": mode,
            "observed": observed,
            "expected": expected,
            "moved": moved,
        }

    if mode == "property_writes":
        score = float(owner.get("score", 0.0))
        done = bool(owner.get("done", False))
        state = owner.get("state", "")
        ok = abs(score - 42.5) <= tolerance and done and state == "running"
        return ok, {
            "mode": mode,
            "score": score,
            "done": done,
            "state": state,
        }

    if mode == "property_target":
        target_name = owner.get("__ln_acceptance_property_target", "")
        scene = owner.scene
        target = next((game_object for game_object in scene.objects if game_object.name == target_name), None)
        score = int(target.get("score", 0)) if target is not None else 0
        expected_score = int(owner.get("__ln_acceptance_expected_score", 42))
        return target is not None and score == expected_score, {
            "mode": mode,
            "target": target_name,
            "score": score,
            "expected_score": expected_score,
        }

    if mode == "property_advanced":
        enabled = bool(owner.get("enabled", True))
        hit = bool(owner.get("has_hit", False))
        speed = float(owner.get("speed", 0.0))
        throttle = float(owner.get("throttle", 0.0))
        choice = float(owner.get("choice", 0.0))
        stored = float(owner.get("stored", 0.0))
        ok = (
            not enabled
            and hit
            and abs(speed - 3.0) <= tolerance
            and abs(throttle - 4.0) <= tolerance
            and abs(choice - 7.0) <= tolerance
            and abs(stored - 5.0) <= tolerance
        )
        return ok, {
            "mode": mode,
            "enabled": enabled,
            "hit": hit,
            "speed": speed,
            "throttle": throttle,
            "choice": choice,
            "stored": stored,
        }

    if mode == "tree_properties":
        tree_value = float(owner.get("tree_value", 0.0))
        ok = abs(tree_value - 3.5) <= tolerance
        return ok, {
            "mode": mode,
            "tree_value": tree_value,
        }

    if mode == "data_containers":
        list_len = int(owner.get("list_len", 0))
        dict_len = int(owner.get("dict_len", 0))
        dict_keys_len = int(owner.get("dict_keys_len", 0))
        dict_answer = float(owner.get("dict_answer", 0.0))
        loop_count = float(owner.get("loop_count", 0.0))
        empty_list_len = int(owner.get("empty_list_len", -1))
        empty_dict_len = int(owner.get("empty_dict_len", -1))
        list_sum = float(owner.get("list_sum", 0.0))
        list_item_value = float(owner.get("list_item_value", 0.0))
        has_answer_key = bool(owner.get("has_answer_key", False))
        has_missing_key = bool(owner.get("has_missing_key", False))
        list_dup_len = int(owner.get("list_dup_len", -1))
        merged_dict_len = int(owner.get("merged_dict_len", -1))
        list_has_two = bool(owner.get("list_has_two", False))
        list_has_nine = bool(owner.get("list_has_nine", False))
        list_has_nested = bool(owner.get("list_has_nested", False))
        typed_list_len = int(owner.get("typed_list_len", -1))
        typed_list_sum = float(owner.get("typed_list_sum", 0.0))
        ok = (
            list_len == 3
            and dict_len == 1
            and dict_keys_len == 1
            and abs(dict_answer - 7.5) <= tolerance
            and abs(loop_count - 3.0) <= tolerance
            and empty_list_len == 0
            and empty_dict_len == 0
            and abs(list_sum - 9.0) <= tolerance
            and abs(list_item_value - 2.0) <= tolerance
            and has_answer_key
            and not has_missing_key
            and list_dup_len == 3
            and merged_dict_len == 2
            and list_has_two
            and not list_has_nine
            and list_has_nested
            and typed_list_len == 3
            and abs(typed_list_sum - 15.0) <= tolerance
        )
        return ok, {
            "mode": mode,
            "list_len": list_len,
            "dict_len": dict_len,
            "dict_keys_len": dict_keys_len,
            "dict_answer": dict_answer,
            "loop_count": loop_count,
            "empty_list_len": empty_list_len,
            "empty_dict_len": empty_dict_len,
            "list_sum": list_sum,
            "list_item_value": list_item_value,
            "has_answer_key": has_answer_key,
            "has_missing_key": has_missing_key,
            "list_dup_len": list_dup_len,
            "merged_dict_len": merged_dict_len,
            "list_has_two": list_has_two,
            "list_has_nine": list_has_nine,
            "list_has_nested": list_has_nested,
            "typed_list_len": typed_list_len,
            "typed_list_sum": typed_list_sum,
        }

    if mode == "bone_read":
        # The default bone runs from its head at z=0 to its tail at z=1.
        bone_length = float(owner.get("bone_length", -1.0))
        bone_head_z = float(owner.get("bone_head_z", -1.0))
        bone_center_z = float(owner.get("bone_center_z", -1.0))
        ok = (
            abs(bone_length - 1.0) <= tolerance
            and abs(bone_head_z - 0.0) <= tolerance
            and abs(bone_center_z - 0.5) <= tolerance
        )
        return ok, {
            "mode": mode,
            "bone_length": bone_length,
            "bone_head_z": bone_head_z,
            "bone_center_z": bone_center_z,
        }

    if mode == "bone_parent":
        scene_objects = tuple(bge.logic.getCurrentScene().objects)
        child_name = owner.get("__ln_acceptance_child", "")
        parent_name = owner.get("__ln_acceptance_parent", "")
        child = next(
            (game_object for game_object in scene_objects if game_object.name == child_name),
            None,
        )
        expected = expected_named_vector(owner, "bone_parent")
        observed = vector_tuple(child.worldPosition) if child is not None else (0.0, 0.0, 0.0)
        parented = (
            child is not None
            and getattr(child, "parent", None) is not None
            and child.parent.name == parent_name
        )
        return parented and close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "parented": parented,
            "observed": observed,
            "expected": expected,
        }

    if mode == "compound_parent":
        scene_objects = tuple(bge.logic.getCurrentScene().objects)
        child_name = owner.get("__ln_acceptance_child", "")
        parent_name = owner.get("__ln_acceptance_parent", "")
        child = next(
            (game_object for game_object in scene_objects if game_object.name == child_name),
            None,
        )
        parented = (
            child is not None
            and getattr(child, "parent", None) is not None
            and child.parent.name == parent_name
        )
        dynamics_suspended = bool(child.isSuspendDynamics) if child is not None else False
        hit_object = owner.rayCast((2.0, 0.0, -5.0), (2.0, 0.0, 5.0), 10.0)[0]
        compound_hit = (
            hit_object is not None and hit_object.name in {child_name, parent_name}
        )
        return parented and dynamics_suspended and compound_hit, {
            "mode": mode,
            "parented": parented,
            "dynamics_suspended": dynamics_suspended,
            "compound_hit": compound_hit,
            "hit_object": hit_object.name if hit_object is not None else "",
        }

    if mode == "bone_pose":
        def observed(prefix):
            return (
                float(owner.get(f"{prefix}_x", 0.0)),
                float(owner.get(f"{prefix}_y", 0.0)),
                float(owner.get(f"{prefix}_z", 0.0)),
            )

        reference_channel = observed("reference_channel")
        reference_native = observed("reference_native")
        reference_object = observed("reference_object")
        channel_rotation = observed("channel_rotation")
        channel_scale = observed("channel_scale")
        armature_rotation = observed("armature_rotation")
        world_rotation = observed("world_rotation")
        transform_location = observed("transform_location")
        transform_rotation = observed("transform_rotation")
        transform_scale = observed("transform_scale")
        offset_rest_head = observed("offset_rest_head")
        offset_pose_head = observed("offset_pose_head")
        offset_delta = tuple(
            offset_pose_head[index] - offset_rest_head[index] for index in range(3))

        ok = (
            close_tuple(reference_channel, (0.0, 0.0, 0.0), tolerance)
            and close_tuple(
                reference_native,
                expected_named_vector(owner, "reference_native"),
                tolerance,
            )
            and close_tuple(
                reference_object,
                expected_named_vector(owner, "reference_object"),
                tolerance,
            )
            and close_tuple(
                channel_rotation,
                expected_named_vector(owner, "channel_rotation"),
                tolerance,
            )
            and close_tuple(
                channel_scale,
                expected_named_vector(owner, "channel_scale"),
                tolerance,
            )
            and close_tuple(
                armature_rotation,
                expected_named_vector(owner, "armature_rotation"),
                tolerance,
            )
            and close_tuple(
                world_rotation,
                expected_named_vector(owner, "world_rotation"),
                tolerance,
            )
            and close_tuple(
                transform_location,
                expected_named_vector(owner, "transform_location"),
                tolerance,
            )
            and close_tuple(
                transform_rotation,
                expected_named_vector(owner, "transform_rotation"),
                tolerance,
            )
            and close_tuple(
                transform_scale,
                expected_named_vector(owner, "transform_scale"),
                tolerance,
            )
            and close_tuple(
                offset_delta,
                expected_named_vector(owner, "offset_delta"),
                tolerance,
            )
        )
        return ok, {
            "mode": mode,
            "reference_channel": reference_channel,
            "reference_native": reference_native,
            "reference_object": reference_object,
            "channel_rotation": channel_rotation,
            "channel_scale": channel_scale,
            "armature_rotation": armature_rotation,
            "world_rotation": world_rotation,
            "transform_location": transform_location,
            "transform_rotation": transform_rotation,
            "transform_scale": transform_scale,
            "offset_rest_head": offset_rest_head,
            "offset_pose_head": offset_pose_head,
            "offset_delta": offset_delta,
        }

    if mode == "phase6_events":
        event_ok = bool(owner.get("event_ok", False))
        return event_ok, {"mode": mode, "event_ok": event_ok}

    if mode == "phase6_move_toward":
        observed = vector_tuple(owner.worldPosition)
        min_x = float(owner.get("__ln_acceptance_min_x", 0.05))
        max_x = float(owner.get("__ln_acceptance_max_x", 1000.0))
        ok = min_x <= observed[0] <= max_x
        return ok, {
            "mode": mode,
            "observed": observed,
            "min_x": min_x,
            "max_x": max_x,
        }

    if mode == "phase6_copy_property":
        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_target", owner.name)
        target = next(
            (game_object for game_object in scene.objects if game_object.name == target_name),
            None,
        )
        copied = float(target.get("score", 0.0)) if target is not None else 0.0
        expected = float(owner.get("__ln_acceptance_expected_score", 11.0))
        return abs(copied - expected) <= tolerance, {
            "mode": mode,
            "copied": copied,
            "expected": expected,
            "target_found": target is not None,
        }

    if mode == "phase6_get_object_attribute":
        pos_x = float(owner.get("read_x", 0.0))
        pos_y = float(owner.get("read_y", 0.0))
        pos_z = float(owner.get("read_z", 0.0))
        expected = expected_vector(owner)
        observed = (pos_x, pos_y, pos_z)
        return close_tuple(observed, expected, tolerance), {
            "mode": mode,
            "observed": observed,
            "expected": expected,
        }

    if mode == "phase6_rotate_to":
        target = (1.0, 0.0, 0.0)
        position = vector_tuple(owner.worldPosition)
        to_target = (
            target[0] - position[0],
            target[1] - position[1],
            target[2] - position[2],
        )
        length = math.sqrt(sum(component * component for component in to_target))
        if length <= 1e-5:
            return False, {"mode": mode, "reason": "target_too_close"}
        to_target = tuple(component / length for component in to_target)
        orientation = owner.worldOrientation
        front_y = (
            float(orientation[0][1]),
            float(orientation[1][1]),
            float(orientation[2][1]),
        )
        dot = sum(to_target[index] * front_y[index] for index in range(3))
        min_dot = float(owner.get("__ln_acceptance_min_dot", 0.85))
        return dot >= min_dot, {
            "mode": mode,
            "dot": dot,
            "min_dot": min_dot,
            "front_y": front_y,
            "to_target": to_target,
        }

    if mode == "phase6_collision":
        collided = bool(owner.get("collided", False))
        collided_object_name = str(owner.get("collided_object_name", ""))
        expected_object_name = str(owner.get("__ln_acceptance_expected_collision_object", ""))
        object_name_ok = collided_object_name == expected_object_name
        point = (
            float(owner.get("collision_point_x", 0.0)),
            float(owner.get("collision_point_y", 0.0)),
            float(owner.get("collision_point_z", 0.0)),
        )
        normal = (
            float(owner.get("collision_normal_x", 0.0)),
            float(owner.get("collision_normal_y", 0.0)),
            float(owner.get("collision_normal_z", 0.0)),
        )
        point_ok = sum(abs(component) for component in point) > 0.0001
        normal_length = math.sqrt(sum(component * component for component in normal))
        normal_ok = 0.5 <= normal_length <= 1.5
        return collided and object_name_ok and point_ok and normal_ok, {
            "mode": mode,
            "collided": collided,
            "collided_object_name": collided_object_name,
            "expected_object_name": expected_object_name,
            "object_name_ok": object_name_ok,
            "point": point,
            "point_ok": point_ok,
            "normal": normal,
            "normal_length": normal_length,
            "normal_ok": normal_ok,
        }

    if mode == "phase6_objects_colliding":
        return bool(owner.get("colliding", False)), {
            "mode": mode,
            "colliding": bool(owner.get("colliding", False)),
        }

    if mode == "phase6_animation_status":
        action_playing = bool(owner.get("action_playing", False))
        lifecycle_started = bool(owner.get("action_lifecycle_started", False))
        lifecycle_completed = bool(owner.get("action_lifecycle_completed", False))
        lifecycle_interrupted = bool(owner.get("action_lifecycle_interrupted", False))
        lifecycle_failed = bool(owner.get("action_lifecycle_failed", False))
        interruption_observed = bool(owner.get("action_interruption_observed", False))
        failure_observed = bool(owner.get("action_failure_observed", False))
        lifecycle_frame = float(owner.getActionFrame(1))
        lifecycle_playing = bool(owner.isPlayingAction(1))
        return (
            action_playing
            and lifecycle_started
            and lifecycle_completed
            and not lifecycle_interrupted
            and not lifecycle_failed
            and interruption_observed
            and failure_observed
        ), {
            "mode": mode,
            "action_playing": action_playing,
            "lifecycle_started": lifecycle_started,
            "lifecycle_completed": lifecycle_completed,
            "lifecycle_interrupted": lifecycle_interrupted,
            "lifecycle_failed": lifecycle_failed,
            "interruption_observed": interruption_observed,
            "failure_observed": failure_observed,
            "lifecycle_frame": lifecycle_frame,
            "lifecycle_playing": lifecycle_playing,
        }

    if mode == "phase6_cross_scene_save":
        save_completed = bool(owner.get("save_completed", False))
        return save_completed, {
            "mode": mode,
            "save_completed": save_completed,
        }

    if mode == "phase6_cross_scene_load":
        current_scene = bge.logic.getCurrentScene().name
        expected_scene = str(owner.get("__ln_acceptance_expected_scene", ""))
        score = float(owner.get("score", 0.0))
        expected_score = float(owner.get("__ln_acceptance_expected_score", 0.0))
        load_completed = bool(owner.get("load_completed", False))
        return (
            current_scene == expected_scene
            and abs(score - expected_score) <= tolerance
            and load_completed
        ), {
            "mode": mode,
            "current_scene": current_scene,
            "expected_scene": expected_scene,
            "score": score,
            "expected_score": expected_score,
            "load_completed": load_completed,
        }

    if mode == "phase6_save_load_structure":
        import bpy

        object_name = str(owner.get("__ln_acceptance_structure_object", owner.name))
        modifier_name = str(owner.get("__ln_acceptance_structure_modifier", ""))
        input_identifier = str(owner.get("__ln_acceptance_structure_input", ""))
        output_identifier = str(owner.get("__ln_acceptance_structure_output", ""))
        transient_modifier = str(
            owner.get("__ln_acceptance_structure_transient_modifier", ""))
        removed_modifier = str(
            owner.get("__ln_acceptance_structure_removed_modifier", ""))
        expected_value = float(owner.get("__ln_acceptance_structure_saved_value", 2.25))
        internal_node_name = str(
            owner.get("__ln_acceptance_structure_internal_node", ""))
        internal_socket_name = str(
            owner.get("__ln_acceptance_structure_internal_socket", ""))
        expected_internal_value = float(
            owner.get("__ln_acceptance_structure_internal_saved_value", 0.2))
        compositor_tree_name = str(
            owner.get("__ln_acceptance_structure_compositor_tree", ""))
        compositor_node_name = str(
            owner.get("__ln_acceptance_structure_compositor_node", ""))
        compositor_socket_name = str(
            owner.get("__ln_acceptance_structure_compositor_socket", ""))
        expected_compositor_value = float(
            owner.get("__ln_acceptance_structure_compositor_saved_value", 12.5))
        runtime_target = next(
            (
                candidate
                for candidate in bge.logic.getCurrentScene().objects
                if candidate.name == object_name
            ),
            None,
        )
        obj = bpy.data.objects.get(object_name)
        modifier = obj.modifiers.get(modifier_name) if obj is not None else None
        modifier_names = [item.name for item in obj.modifiers] if obj is not None else []
        input_mode = None
        input_value = None
        output_attribute = None
        modifier_viewport_enabled = None
        internal_value = None
        internal_muted = None
        compositor_value = None
        compositor_muted = None
        compositor_root_name = None
        inspection_error = ""
        try:
            if modifier is None or modifier.type != "NODES":
                raise RuntimeError("the restored Geometry Nodes modifier is missing")
            input_state = getattr(modifier.properties.inputs, input_identifier)
            output_state = getattr(modifier.properties.outputs, output_identifier)
            input_mode = str(input_state.type)
            input_value = float(input_state.value)
            output_attribute = str(output_state.attribute_name)
            modifier_viewport_enabled = bool(modifier.show_viewport)
            internal_node = modifier.node_group.nodes.get(internal_node_name)
            if internal_node is None:
                raise RuntimeError("the restored internal Geometry node is missing")
            internal_value = float(
                internal_node.inputs[internal_socket_name].default_value)
            internal_muted = bool(internal_node.mute)
            compositor_tree = bpy.context.scene.compositing_node_group
            compositor_root_name = (
                compositor_tree.name if compositor_tree is not None else None)
            compositor_node = (
                compositor_tree.nodes.get(compositor_node_name)
                if compositor_tree is not None
                else None
            )
            if compositor_node is None:
                raise RuntimeError("the restored compositor node is missing")
            compositor_value = float(
                compositor_node.inputs[compositor_socket_name].default_value)
            compositor_muted = bool(compositor_node.mute)
        except Exception:
            inspection_error = traceback.format_exc()
        installed_tree_restored = bool(
            runtime_target is not None
            and runtime_target.get("installed_tree_restored", False))
        transient_tree_removed = bool(
            runtime_target is not None
            and not runtime_target.get("transient_tree_running", False))
        load_completed = bool(
            runtime_target is not None and runtime_target.get("load_completed", False))
        mutation_completed = bool(
            runtime_target is not None
            and runtime_target.get("structure_output_mutated", False))
        collision_mutation_completed = bool(
            runtime_target is not None
            and runtime_target.get("structure_collision_mutated", False))
        mutation_error = (
            str(runtime_target.get("structure_mutation_error", ""))
            if runtime_target is not None
            else "the runtime target is missing"
        )
        modifier_state_ok = (
            input_mode == "VALUE"
            and input_value is not None
            and abs(input_value - expected_value) <= tolerance
            and output_attribute == "saved_attr"
            and modifier_viewport_enabled is True
            and internal_value is not None
            and abs(internal_value - expected_internal_value) <= tolerance
            and internal_muted is False
            and modifier_names == [modifier_name, removed_modifier]
            and transient_modifier not in modifier_names
        )
        compositor_state_ok = (
            compositor_root_name == compositor_tree_name
            and compositor_value is not None
            and abs(compositor_value - expected_compositor_value) <= tolerance
            and compositor_muted is False
        )
        return (
            installed_tree_restored
            and transient_tree_removed
            and load_completed
            and mutation_completed
            and collision_mutation_completed
            and not mutation_error
            and not inspection_error
            and modifier_state_ok
            and compositor_state_ok
        ), {
            "mode": mode,
            "installed_tree_restored": installed_tree_restored,
            "transient_tree_removed": transient_tree_removed,
            "load_completed": load_completed,
            "mutation_completed": mutation_completed,
            "collision_mutation_completed": collision_mutation_completed,
            "mutation_error": mutation_error,
            "inspection_error": inspection_error,
            "modifier_names": modifier_names,
            "input_mode": input_mode,
            "input_value": input_value,
            "expected_value": expected_value,
            "output_attribute": output_attribute,
            "modifier_viewport_enabled": modifier_viewport_enabled,
            "internal_value": internal_value,
            "expected_internal_value": expected_internal_value,
            "internal_muted": internal_muted,
            "compositor_root_name": compositor_root_name,
            "compositor_value": compositor_value,
            "expected_compositor_value": expected_compositor_value,
            "compositor_muted": compositor_muted,
            "modifier_state_ok": modifier_state_ok,
            "compositor_state_ok": compositor_state_ok,
        }

    if mode in {"phase6_save_load_material", "phase6_save_load_material_fresh"}:
        import bpy

        target_name = str(owner.get("__ln_acceptance_material_object", owner.name))
        sibling_name = str(owner.get("__ln_acceptance_material_sibling", ""))
        expected_material = str(owner.get("__ln_acceptance_expected_material", ""))
        expected_sibling_material = str(
            owner.get("__ln_acceptance_expected_sibling_material", ""))
        expected_parameter = (
            float(owner.get("__ln_acceptance_material_r", 0.0)),
            float(owner.get("__ln_acceptance_material_g", 0.0)),
            float(owner.get("__ln_acceptance_material_b", 0.0)),
            float(owner.get("__ln_acceptance_material_a", 1.0)),
        )
        expected_shared_value = float(
            owner.get("__ln_acceptance_shared_material_value", 0.0))
        shared_node_name = str(owner.get("__ln_acceptance_shared_material_node", ""))
        shared_socket_name = str(owner.get("__ln_acceptance_shared_material_socket", ""))
        target = bpy.data.objects.get(target_name)
        sibling = bpy.data.objects.get(sibling_name)
        target_material = (
            target.material_slots[0].material.name
            if target is not None
            and len(target.material_slots) == 1
            and target.material_slots[0].material is not None
            else None
        )
        sibling_material = (
            sibling.material_slots[0].material.name
            if sibling is not None
            and len(sibling.material_slots) == 1
            and sibling.material_slots[0].material is not None
            else None
        )
        parameter_values = []
        if target is not None:
            for property_name in target.keys():
                if str(property_name).startswith("_LNMP_"):
                    value = target[property_name]
                    try:
                        parameter_values.append(tuple(float(value[index]) for index in range(4)))
                    except (IndexError, TypeError):
                        parameter_values.append(None)
        parameter_ok = any(
            value is not None and close_color(value, expected_parameter, tolerance)
            for value in parameter_values
        )
        shared_value = None
        material = bpy.data.materials.get(expected_material)
        node = (
            material.node_tree.nodes.get(shared_node_name)
            if material is not None and material.node_tree is not None
            else None
        )
        socket = node.inputs.get(shared_socket_name) if node is not None else None
        if socket is not None:
            shared_value = float(socket.default_value)
        shared_ok = (
            shared_value is not None
            and abs(shared_value - expected_shared_value) <= tolerance
        )
        load_completed = bool(owner.get("load_completed", False))
        return (
            target_material == expected_material
            and sibling_material == expected_sibling_material
            and parameter_ok
            and shared_ok
            and load_completed
        ), {
            "mode": mode,
            "target_material": target_material,
            "expected_material": expected_material,
            "sibling_material": sibling_material,
            "expected_sibling_material": expected_sibling_material,
            "parameter_values": parameter_values,
            "expected_parameter": expected_parameter,
            "parameter_ok": parameter_ok,
            "shared_value": shared_value,
            "expected_shared_value": expected_shared_value,
            "shared_ok": shared_ok,
            "load_completed": load_completed,
        }

    if mode == "phase6_save_load":
        import bpy

        subject_name = str(owner.get("__ln_acceptance_subject", ""))
        owner = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == subject_name),
            owner,
        )
        score = float(owner.get("score", 0.0))
        expected = float(owner.get("__ln_acceptance_expected_score", 42.0))
        transient_score = float(owner.get("transient_score", 0.0))
        tree_score = float(owner.get("tree_checkpoint_value", 0.0))
        expected_tree_score = float(owner.get("__ln_acceptance_expected_tree_score", 99.0))
        gravity_z = float(owner.get("gravity_checkpoint_z", 0.0))
        expected_gravity_z = float(owner.get("__ln_acceptance_expected_gravity_z", -3.0))
        position = vector_tuple(owner.worldPosition)
        position_ok = close_tuple(position, expected_vector(owner), tolerance)
        expected_parent_name = str(owner.get("__ln_acceptance_expected_parent", ""))
        parent = owner.parent
        hierarchy_ok = parent is not None and parent.name == expected_parent_name
        soft_name = str(owner.get("__ln_acceptance_soft_object", ""))
        soft_object = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == soft_name),
            None,
        )
        soft_position = vector_tuple(soft_object.worldPosition) if soft_object else None
        soft_color = color_tuple(soft_object.color) if soft_object else None
        soft_color_ok = soft_color is not None and close_color(
            soft_color, (0.2, 0.4, 0.6, 1.0), tolerance)
        load_completed = bool(owner.get("load_completed", False))
        physics_name = str(owner.get("__ln_acceptance_physics_object", ""))
        physics_object = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == physics_name),
            None,
        )
        dynamics_suspended = (
            bool(physics_object.isSuspendDynamics) if physics_object else None
        )
        reconstructed_name = str(owner.get("__ln_acceptance_reconstructed_object", ""))
        reconstructed_mesh = str(owner.get("__ln_acceptance_reconstructed_mesh", ""))
        reconstructed_runtime_objects = [
            candidate for candidate in bge.logic.getCurrentScene().objects
            if candidate.name == reconstructed_name
        ]
        reconstructed_physics_ok = (
            len(reconstructed_runtime_objects) == 1
            and reconstructed_runtime_objects[0].getPhysicsId() != 0
        )
        reconstructed_color = (
            color_tuple(reconstructed_runtime_objects[0].color)
            if len(reconstructed_runtime_objects) == 1 else None
        )
        reconstructed_color_ok = (
            reconstructed_color is not None
            and close_color(reconstructed_color, (1.0, 1.0, 1.0, 1.0), tolerance)
        )
        reconstructed_render_objects = [
            candidate for candidate in bpy.context.scene.objects
            if candidate.type == "MESH"
            and candidate.data is not None
            and candidate.data.name == reconstructed_mesh
        ]
        visible_reconstructed_render_objects = [
            candidate for candidate in reconstructed_render_objects
            if not candidate.hide_viewport
        ]
        authored_source_hidden = any(
            candidate.name == reconstructed_name and candidate.hide_viewport
            for candidate in reconstructed_render_objects
        )
        reconstruction_render_state_ok = (
            len(reconstructed_render_objects) == 2
            and len(visible_reconstructed_render_objects) == 1
            and authored_source_hidden
        )
        reconstructed_render_state = [
            {
                "name": candidate.name,
                "hidden": bool(candidate.hide_viewport),
                "color": color_tuple(candidate.color),
            }
            for candidate in reconstructed_render_objects
        ]
        return (
            abs(score - expected) <= tolerance
            and abs(transient_score - 99.0) <= tolerance
            and abs(tree_score - expected_tree_score) <= tolerance
            and abs(gravity_z - expected_gravity_z) <= tolerance
            and position_ok
            and hierarchy_ok
            and soft_color_ok
            and load_completed
            and dynamics_suspended is True
            and reconstructed_physics_ok
            and reconstructed_color_ok
            and reconstruction_render_state_ok
        ), {
            "mode": mode,
            "score": score,
            "expected": expected,
            "transient_score": transient_score,
            "tree_score": tree_score,
            "expected_tree_score": expected_tree_score,
            "gravity_z": gravity_z,
            "expected_gravity_z": expected_gravity_z,
            "position": position,
            "position_ok": position_ok,
            "parent": parent.name if parent is not None else None,
            "expected_parent": expected_parent_name,
            "hierarchy_ok": hierarchy_ok,
            "soft_position": soft_position,
            "soft_color": soft_color,
            "soft_color_ok": soft_color_ok,
            "load_completed": load_completed,
            "dynamics_suspended": dynamics_suspended,
            "reconstructed_runtime_count": len(reconstructed_runtime_objects),
            "reconstructed_physics_ok": reconstructed_physics_ok,
            "reconstructed_color": reconstructed_color,
            "reconstructed_color_ok": reconstructed_color_ok,
            "reconstructed_render_state": reconstructed_render_state,
            "reconstruction_render_state_ok": reconstruction_render_state_ok,
        }

    if mode == "phase6_save_load_existence":
        scene_objects = list(bge.logic.getCurrentScene().objects)
        authored_present = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "authored_present"
        ]
        authored_tombstones = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "authored_tombstone"
        ]
        persistent_spawns = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "persistent_spawn"
        ]
        transient_spawns = [
            candidate for candidate in scene_objects
            if candidate.get("__ln_acceptance_existence_role", "") == "transient_spawn"
        ]
        present_position = (
            vector_tuple(authored_present[0].worldPosition)
            if len(authored_present) == 1 else None
        )
        spawn_position = (
            vector_tuple(persistent_spawns[0].worldPosition)
            if len(persistent_spawns) == 1 else None
        )
        present_position_ok = present_position is not None and close_tuple(
            present_position, (3.0, 4.0, 5.0), tolerance)
        present_values = (
            [vector_tuple(authored_present[0].get("spot", ())), authored_present[0].get("bag")]
            if len(authored_present) == 1 else None
        )
        spawn_position_ok = spawn_position is not None and close_tuple(
            spawn_position, (11.0, 12.0, 13.0), tolerance)
        load_completed = bool(owner.get("load_completed", False))
        return (
            len(authored_present) == 1
            and not authored_tombstones
            and len(persistent_spawns) == 1
            and len(transient_spawns) == 1
            and present_position_ok
            and present_values == [(1.0, 2.0, 3.0), [3, 5]]
            and spawn_position_ok
            and load_completed
        ), {
            "mode": mode,
            "authored_present_count": len(authored_present),
            "authored_tombstone_count": len(authored_tombstones),
            "persistent_spawn_count": len(persistent_spawns),
            "transient_spawn_count": len(transient_spawns),
            "present_position": present_position,
            "present_position_ok": present_position_ok,
            "present_values": present_values,
            "spawn_position": spawn_position,
            "spawn_position_ok": spawn_position_ok,
            "load_completed": load_completed,
        }

    if mode == "phase6_save_load_revision":
        subject_name = str(owner.get("__ln_acceptance_subject", ""))
        owner = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == subject_name),
            owner,
        )
        score = float(owner.get("score", 0.0))
        transient_score = float(owner.get("transient_score", 0.0))
        position = vector_tuple(owner.worldPosition)
        position_ok = close_tuple(position, expected_vector(owner), tolerance)
        current_name = str(owner.get("__ln_acceptance_current_object", ""))
        current_object_exists = any(
            candidate.name == current_name for candidate in bge.logic.getCurrentScene().objects
        )
        current_object = next(
            (candidate for candidate in bge.logic.getCurrentScene().objects
             if candidate.name == current_name),
            None,
        )
        current_position = vector_tuple(current_object.worldPosition) if current_object else None
        current_position_ok = current_position is not None and close_tuple(
            current_position, (9.0, 1.0, 2.0), tolerance)
        removed_name = str(owner.get("__ln_acceptance_removed_object", ""))
        removed_object_absent = not any(
            candidate.name == removed_name for candidate in bge.logic.getCurrentScene().objects
        )
        load_completed = bool(owner.get("load_completed", False))
        return (
            abs(score - 42.0) <= tolerance
            and abs(transient_score - 123.0) <= tolerance
            and position_ok
            and current_object_exists
            and current_position_ok
            and removed_object_absent
            and load_completed
        ), {
            "mode": mode,
            "score": score,
            "transient_score": transient_score,
            "position": position,
            "position_ok": position_ok,
            "current_object_exists": current_object_exists,
            "current_position": current_position,
            "current_position_ok": current_position_ok,
            "removed_object_absent": removed_object_absent,
            "load_completed": load_completed,
        }

    if mode == "phase6_align_axis":
        orientation = owner.worldOrientation
        local_z = (
            float(orientation[0][2]),
            float(orientation[1][2]),
            float(orientation[2][2]),
        )
        length = math.sqrt(sum(component * component for component in local_z))
        if length <= 1e-5:
            return False, {"mode": mode, "reason": "degenerate_orientation"}
        local_z = tuple(component / length for component in local_z)
        dot = local_z[0]
        min_dot = float(owner.get("__ln_acceptance_min_dot", 0.85))
        # Toward the target, not merely parallel: a spinning axis passes through both directions.
        return dot >= min_dot, {"mode": mode, "dot": dot, "min_dot": min_dot, "local_z": local_z}

    if mode == "phase6_slow_follow":
        observed = vector_tuple(owner.worldPosition)
        min_x = float(owner.get("__ln_acceptance_min_x", 0.5))
        return observed[0] >= min_x, {
            "mode": mode,
            "observed": observed,
            "min_x": min_x,
        }

    if mode == "phase6_replace_mesh":
        return bool(owner.get("mesh_replaced", False)), {
            "mode": mode,
            "mesh_replaced": bool(owner.get("mesh_replaced", False)),
        }

    if mode == "input_nodes":
        keyboard_ok = bool(owner.get("keyboard_checked", False))
        mouse_ok = bool(owner.get("mouse_checked", False))
        gamepad_ok = bool(owner.get("gamepad_checked", False))
        return keyboard_ok and mouse_ok and gamepad_ok, {
            "mode": mode,
            "keyboard_checked": keyboard_ok,
            "mouse_checked": mouse_ok,
            "gamepad_checked": gamepad_ok,
        }

    if mode == "values_math":
        result = float(owner.get("math_result", 0.0))
        expected = float(owner.get("__ln_acceptance_expected_math", 5.0))
        curve_result = float(owner.get("float_curve_result", 0.0))
        curve_expected = float(owner.get("__ln_acceptance_expected_float_curve", 0.5))
        ok = (
            abs(result - expected) <= tolerance
            and abs(curve_result - curve_expected) <= tolerance
        )
        return ok, {
            "mode": mode,
            "result": result,
            "expected": expected,
            "curve_result": curve_result,
            "curve_expected": curve_expected,
        }

    if mode == "c_tier_values":
        vector4_text = owner.get("vector4_text", "")
        resize4_text = owner.get("resize4_text", "")
        matrix_euler_text = owner.get("matrix_euler_text", "")
        ok = (
            "4.5" in vector4_text
            and "4.5" in resize4_text
            and "0.25" in matrix_euler_text
            and "-0.5" in matrix_euler_text
            and "0.75" in matrix_euler_text
        )
        return ok, {
            "mode": mode,
            "vector4_text": vector4_text,
            "resize4_text": resize4_text,
            "matrix_euler_text": matrix_euler_text,
        }

    if mode == "b_tier_core":
        formula_result = float(owner.get("formula_result", 0.0))
        eval_ok = bool(owner.get("eval_ok", False))
        tween_result = float(owner.get("tween_result", 0.0))
        projectile_hit = bool(owner.get("projectile_hit", False))
        expected_formula = abs(-2.0) + math.pi + math.e
        ok = (
            abs(formula_result - expected_formula) <= tolerance
            and eval_ok
            and tween_result > 0.01
            and projectile_hit
        )
        return ok, {
            "mode": mode,
            "formula_result": formula_result,
            "eval_ok": eval_ok,
            "tween_result": tween_result,
            "projectile_hit": projectile_hit,
        }

    if mode == "sound_state":
        return bool(owner.get("sound_stopped", False)), {
            "mode": mode,
            "sound_stopped": bool(owner.get("sound_stopped", False)),
        }

    if mode == "sound_advanced":
        return bool(owner.get("sound_advanced_ok", False)), {
            "mode": mode,
            "sound_advanced_ok": bool(owner.get("sound_advanced_ok", False)),
        }

    if mode == "vehicle_nodes_ok":
        return bool(owner.get("vehicle_nodes_ok", False)), {
            "mode": mode,
            "vehicle_nodes_ok": bool(owner.get("vehicle_nodes_ok", False)),
        }

    if mode == "material_shader_input":
        material_name = owner.get("__ln_acceptance_material", "")
        try:
            import bpy
            material = bpy.data.materials.get(material_name)
            mix = material.node_tree.nodes.get("Mix") if material and material.node_tree else None
            factor_socket = None
            if mix is not None:
                for socket in mix.inputs:
                    if socket.identifier == "Factor_Float":
                        factor_socket = socket
                        break
            factor = float(factor_socket.default_value) if factor_socket is not None else 0.0
        except Exception as exc:
            return False, {"mode": mode, "material": material_name, "error": repr(exc)}

        return abs(factor - 1.0) <= tolerance, {
            "mode": mode,
            "material": material_name,
            "factor": factor,
        }

    if mode == "loop_routes":
        counts = {key: int(owner.get(key, -1)) for key in (
            "c_other", "c_direct", "c_append", "c_inventory", "c_branch")}
        expected = {"c_other": 1, "c_direct": 3, "c_append": 3, "c_inventory": 3, "c_branch": 3}
        return counts == expected, {"mode": mode, **counts}

    if mode == "inventory_rule_sets":
        # Both bags follow the set "Bag" (no stacking, 2 places, or the object's bag_size); the
        # Override call stacks all 3 Apples; the weapon list refuses the Apple from Add Item and
        # from Move To Slot, and takes the Sword.
        numbers = {key: int(owner.get(key, -1)) for key in (
            "target_left", "other_left", "pouch_left", "refused_left", "sword_left")}
        expected_numbers = {"target_left": 1, "other_left": 0, "pouch_left": 0,
                            "refused_left": 1, "sword_left": 0}
        texts = {key: str(owner.get(key, "")) for key in ("refused_outcome", "move_outcome")}
        expected_texts = {"refused_outcome": "Refused", "move_outcome": "Refused"}
        scene = bge.logic.getCurrentScene()
        other = scene.objects.get("LN_Acceptance_RulesOther")

        def length(obj, name):
            value = obj.get(name, None) if obj is not None else None
            return len(value) if value is not None else -1

        lengths = {"inventory": length(owner, "inventory"), "pouch": length(owner, "pouch"),
                   "weapon": length(owner, "weapon"), "other": length(other, "inventory")}
        expected_lengths = {"inventory": 2, "pouch": 1, "weapon": 1, "other": 3}
        ok = (bool(owner.get("rules_done", False)) and numbers == expected_numbers and
              texts == expected_texts and lengths == expected_lengths)
        return ok, {"mode": mode, **numbers, **texts, **lengths}

    if mode == "inventory_in_place":
        # Two adds in one tick: the second sees the first (in-place edits); the scene's Item
        # Library caps a Coin stack at 6; a pickup object adds its own item and count; Amount 0
        # removes a whole slot; Add Item's Added says how many went in (7 of 9 Gems into the 3,
        # Capacity 2 leaves no new slot).
        numbers = {key: int(owner.get(key, -1)) for key in (
            "leftover2", "removed_whole", "coins", "gems", "added2", "added_pickup",
            "added_partial", "leftover_partial")}
        expected_numbers = {"leftover2": 0, "removed_whole": 6, "coins": 2, "gems": 3,
                            "added2": 4, "added_pickup": 3, "added_partial": 7,
                            "leftover_partial": 2}
        inventory = owner.get("inventory", None)
        slots = len(inventory) if inventory is not None else -1
        scene = bge.logic.getCurrentScene()

        def drops(name, x):
            return [obj for obj in scene.objects
                    if obj.name == name and abs(float(obj.worldPosition.x) - x) < 0.01]

        gem_drops = drops("Gem", 5.0)
        coin_drops = drops("Coin", 7.0)
        gem_ok = len(gem_drops) == 1 and int(gem_drops[0].get("count", -1)) == 2 and str(
            gem_drops[0].get("item", "")) == "Gem"
        coin_ok = len(coin_drops) == 1 and int(coin_drops[0].get("count", -1)) == 3
        ok = (bool(owner.get("inventory_done", False)) and numbers == expected_numbers and
              slots == 2 and gem_ok and coin_ok)
        return ok, {"mode": mode, **numbers, "slots": slots, "gem_drops": len(gem_drops),
                    "coin_drops": len(coin_drops), "gem_ok": gem_ok, "coin_ok": coin_ok}

    if mode == "inventory_two_inventories":
        # Move To Slot into another inventory: onto the same item it stacks up to the limit, onto an
        # empty place the whole stack moves, and To Object empty means the same object (an
        # equipment property with Capacity 1).
        scene = bge.logic.getCurrentScene()
        chest = scene.objects.get("LN_Acceptance_TwoChest")
        texts = {key: str(owner.get(key, "")) for key in ("o_stack", "o_move", "o_equip", "o_full")}
        numbers = {key: int(owner.get(key, -1)) for key in (
            "bag_apples", "bag_slots", "chest_apples", "chest_swords", "weapon_apples")}
        expected_texts = {"o_stack": "Stacked", "o_move": "Moved", "o_equip": "Moved",
                          "o_full": "No Room"}
        expected_numbers = {"bag_apples": 0, "bag_slots": 1, "chest_apples": 5, "chest_swords": 1,
                            "weapon_apples": 2}
        chest_slots = len(chest.get("chest", [])) if chest is not None else -1
        ok = (bool(owner.get("two_done", False)) and texts == expected_texts and
              numbers == expected_numbers and chest_slots == 2)
        return ok, {"mode": mode, **texts, **numbers, "chest_slots": chest_slots}

    if mode == "ui_menu_replace_inside":
        # A Replace Top issued by a tree inside the menu being replaced (a tab button): the new
        # menu stays open, it is not closed with the old menu's tree.
        scene = bge.logic.getCurrentScene()
        names = set(obj.name for obj in scene.objects)
        details = {
            "mode": mode,
            "a_open": "LN_Acceptance_RTAPanel" in names,
            "b_open": "LN_Acceptance_RTBPanel" in names,
            "b_seen": bool(owner.get("b_seen", False)),
        }
        ok = details["b_open"] and not details["a_open"]
        return ok, details

    if mode == "ui_player_menu":
        # A menu opened for player 1: its first button is player 1's focus and highlight (the
        # Highlight event reports User 1), its Highlight Object shows, player 0 has no focus, and
        # text shows each player's button names.
        # The menu's objects are copies with their own text: read the live game objects.
        scene = bge.logic.getCurrentScene()
        glow = scene.objects.get("LN_Acceptance_PMGlow")
        other_glow = scene.objects.get("LN_Acceptance_PMOtherGlow")
        menu_hint = scene.objects.get("LN_Acceptance_PMHint")
        game_hint = scene.objects.get("LN_Acceptance_PMGameHint")
        details = {
            "mode": mode,
            "hl_user": int(owner.get("hl_user", -1)),
            "hl_action": str(owner.get("hl_action", "")),
            "focus1": str(owner.get("focus1", "")),
            "focus0": str(owner.get("focus0", "?")),
            "glow_visible": bool(glow.visible) if glow is not None else None,
            "other_glow_visible": bool(other_glow.visible) if other_glow is not None else None,
            "menu_hint": menu_hint.blenderObject.data.body if menu_hint is not None else None,
            "game_hint": game_hint.blenderObject.data.body if game_hint is not None else None,
        }
        ok = (details["hl_user"] == 1 and details["hl_action"] == "first" and
              details["focus1"] == "LN_Acceptance_PMFirst" and
              details["focus0"] in ("", "None") and
              details["glow_visible"] is True and details["other_glow_visible"] is False and
              details["menu_hint"] == "A ok" and details["game_hint"] == "Return ok")
        return ok, details

    if mode == "ui_grid_inventory":
        # The grids read the lists their Object and Property name (the bag: whose menu it is),
        # and the shared tooltip of the slot player 1 highlights shows its item's words beside
        # it: (0, 1, 0.5) in the space of the panel it hangs from, scaled (4, 2, 0.1).
        scene = bge.logic.getCurrentScene()
        tip = scene.objects.get("LN_Acceptance_GITip")
        slots = [ob for ob in scene.objects if ob.name == "LN_Acceptance_GISlot"]
        slot = min(slots, key=lambda ob: ob.worldPosition.x) if slots else None
        offset = (list(tip.worldPosition - slot.worldPosition)
                  if tip is not None and slot is not None else None)
        details = {
            "mode": mode,
            "bag_last": int(owner.get("bag_last", -1)),
            "chest_pages": int(owner.get("chest_pages", -1)),
            "slot_copies": len(slots),
            "tip_visible": bool(tip.visible) if tip is not None else None,
            "tip_text": str(tip.get("Text", "")) if tip is not None else None,
            "tip_body": tip.blenderObject.data.body if tip is not None else None,
            "tip_offset": [round(value, 3) for value in offset] if offset else None,
            "outcome": str(owner.get("outcome", "")),
        }
        ok = (details["bag_last"] == 1 and details["chest_pages"] == 2 and
              details["slot_copies"] == 2 and details["tip_visible"] is True and
              details["tip_text"] == "Red Apple: Crunchy x3" and
              details["tip_body"] == "Red Apple: Crunchy x3" and offset is not None and
              abs(offset[0]) < 1.0e-3 and abs(offset[1] - 2.0) < 1.0e-3 and
              abs(offset[2] - 0.05) < 1.0e-3 and details["outcome"] == "")
        return ok, details

    if mode == "inventory_recipes":
        texts = {key: str(owner.get(key, "")) for key in (
            "move_outcome", "move_made", "craft_outcome", "missing_outcome", "stack_outcome",
            "station")}
        numbers = {key: int(owner.get(key, -1)) for key in (
            "potions", "keys", "coins", "stacked_first", "arrows", "found_index",
            "typed_arrows", "crafted_times", "craftable", "byproducts", "buckets",
            "tool_butter")}
        expected_texts = {"move_outcome": "Combined", "move_made": "Potion",
                          "craft_outcome": "Crafted", "missing_outcome": "Missing",
                          "stack_outcome": "Stacked", "station": "Bench"}
        expected_numbers = {"potions": 1, "keys": 1, "coins": 4, "stacked_first": 6,
                            "arrows": 3, "found_index": 1, "typed_arrows": 3,
                            "crafted_times": 3, "craftable": 3, "byproducts": 1, "buckets": 1,
                            "tool_butter": 5}
        ok = (bool(owner.get("recipes_done", False)) and texts == expected_texts and
              numbers == expected_numbers and bool(owner.get("can_craft", False)))
        return ok, {"mode": mode, **texts, **numbers,
                    "can_craft": bool(owner.get("can_craft", False))}

    if mode == "input_map":
        flags = {
            key: bool(owner.get(key, False))
            for key in ("saved", "loaded", "reset", "missing_done", "rebind_done")
        }
        texts = {key: str(owner.get(key, "")) for key in ("jump_key", "jump_button", "accept_auto")}
        file_path = str(owner.get("__ln_acceptance_expected_path", ""))
        file_ok = bool(file_path) and os.path.isfile(file_path)
        ok = (all(flags.values()) and not owner.get("missing_loaded", True) and
              not owner.get("rebind_success", True) and not owner.get("jump_active", True) and
              not owner.get("jump_triggered", False) and texts["jump_key"] == "Space Bar" and
              texts["jump_button"] == "A" and texts["accept_auto"] == "Return" and file_ok)
        return ok, {
            "mode": mode,
            **flags,
            **texts,
            "missing_loaded": owner.get("missing_loaded", None),
            "rebind_success": owner.get("rebind_success", None),
            "jump_active": owner.get("jump_active", None),
            "jump_triggered": owner.get("jump_triggered", None),
            "file_ok": file_ok,
        }

    if mode == "time_state":
        keys = ("next_frame", "delay", "timer", "pulsify", "barrier")
        values = {key: bool(owner.get(key, False)) for key in keys}
        time_value = float(owner.get("time_value", 0.0))
        delta_value = float(owner.get("delta_value", 0.0))
        time_ok = time_value >= 0.05
        delta_ok = 0.01 <= delta_value <= 0.025
        return all(values.values()) and time_ok and delta_ok, {
            "mode": mode,
            **values,
            "time_value": time_value,
            "time_ok": time_ok,
            "delta_value": delta_value,
            "delta_ok": delta_ok,
        }

    if mode == "scene_state":
        scene = bge.logic.getCurrentScene()
        observed_gravity = vector_tuple(scene.gravity)
        property_gravity = (
            float(owner.get("gravity_x", 0.0)),
            float(owner.get("gravity_y", 0.0)),
            float(owner.get("gravity_z", 0.0)),
        )
        observed_timescale = float(bge.logic.getTimeScale())
        property_timescale = float(owner.get("timescale", 0.0))
        expected_timescale = float(owner.get("__ln_acceptance_expected_timescale", 1.0))
        ok = (
            close_tuple(observed_gravity, expected, tolerance)
            and close_tuple(property_gravity, expected, tolerance)
            and abs(observed_timescale - expected_timescale) <= tolerance
            and abs(property_timescale - expected_timescale) <= tolerance
        )
        return ok, {
            "mode": mode,
            "observed_gravity": observed_gravity,
            "property_gravity": property_gravity,
            "expected_gravity": expected,
            "observed_timescale": observed_timescale,
            "property_timescale": property_timescale,
            "expected_timescale": expected_timescale,
        }

    if mode == "camera_state":
        scene = bge.logic.getCurrentScene()
        active_camera = getattr(scene, "active_camera", None)
        observed_name = active_camera.name if active_camera is not None else ""
        observed_fov = float(getattr(active_camera, "fov", 0.0)) if active_camera is not None else 0.0
        observed_ortho_scale = (
            float(getattr(active_camera, "ortho_scale", 0.0)) if active_camera is not None else 0.0)
        expected_name = owner.get("__ln_acceptance_expected_active_camera", "")
        expected_fov = float(owner.get("__ln_acceptance_expected_fov", 0.0))
        expected_ortho_scale = float(owner.get("__ln_acceptance_expected_ortho_scale", 0.0))
        expected_screen = (
            float(owner.get("__ln_acceptance_expected_screen_x", 0.5)),
            float(owner.get("__ln_acceptance_expected_screen_y", 0.5)),
        )
        expected_depth = float(owner.get("__ln_acceptance_expected_depth", 0.0))
        observed_screen = (
            float(owner.get("screen_x", 0.0)),
            float(owner.get("screen_y", 0.0)),
        )
        observed_world = (
            float(owner.get("world_x", 0.0)),
            float(owner.get("world_y", 0.0)),
            float(owner.get("world_z", 0.0)),
        )
        expected_world = (
            vector_tuple(active_camera.worldPosition + active_camera.getScreenVect(
                expected_screen[0], expected_screen[1]) * -expected_depth)
            if active_camera is not None else (0.0, 0.0, 0.0)
        )
        active_camera_present = bool(owner.get("active_camera_present", False))
        ok = (
            active_camera is not None
            and active_camera_present
            and observed_name == expected_name
            and abs(observed_fov - expected_fov) <= tolerance
            and abs(observed_ortho_scale - expected_ortho_scale) <= tolerance
            and abs(observed_screen[0] - expected_screen[0]) <= tolerance
            and abs(observed_screen[1] - expected_screen[1]) <= tolerance
            and close_tuple(observed_world, expected_world, tolerance)
        )
        return ok, {
            "mode": mode,
            "observed_name": observed_name,
            "expected_name": expected_name,
            "observed_fov": observed_fov,
            "expected_fov": expected_fov,
            "observed_ortho_scale": observed_ortho_scale,
            "expected_ortho_scale": expected_ortho_scale,
            "observed_screen": observed_screen,
            "expected_screen": expected_screen,
            "observed_world": observed_world,
            "expected_world": expected_world,
            "active_camera_present": active_camera_present,
        }

    if mode == "lod_view_size":
        scene = bge.logic.getCurrentScene()
        rock = scene.objects.get(str(owner.get("__ln_acceptance_rock", "")))
        shown = rock.meshes[0].name if rock is not None and rock.meshes else ""
        expected = str(owner.get("__ln_acceptance_mesh", ""))
        return expected != "" and shown == expected, {
            "mode": mode, "shown": shown, "expected": expected}

    if mode == "ui_view_center":
        # View Center: player 1 (camera inside their own body) hovered the Any Player button at
        # once, player 2 only once the wall in front went away (through the box their camera
        # does not draw); player 1's menu stopped their pointer (Exit). Only in Menus showed the
        # cursor only while that menu was open.
        samples = bge.logic.globalDict.get("vc_samples", {})
        expected = {"before": [False, True, False, False], "menu": [True, True, True],
                    "after": [False, True, True, True]}
        return samples == expected, {"mode": mode, "samples": samples, "expected": expected}

    if mode == "ui_take_control_pointer":
        # Take UI Control: player 1 gets the computer, player 2 is refused (Result 20) until
        # player 1 gives it back. Set Player Pointer placed player 1's cursor in the middle of the
        # screen, in front of its button, shown; turned off, it is hidden.
        observed = {name: str(owner.get("tc_" + name, "")) for name in (
            "take1", "take2", "release", "take2_after")}
        expected = {"take1": "done", "take2": "failed", "release": "done",
                    "take2_after": "done"}
        result = int(owner.get("tc_take2_result", -1))
        mid = bge.logic.globalDict.get("tc_mid")
        cursor = next((obj for obj in bge.logic.getCurrentScene().objects
                       if obj.name.endswith("_TCCursor")), None)
        placed = bool(mid) and abs(mid[0]) < 1.0e-3 and abs(mid[2] - 1.5) < 1.0e-3 and \
            -0.05 < mid[1] < -0.01 and mid[3]
        hidden_after = cursor is not None and not cursor.visible
        ok = observed == expected and result == 20 and placed and hidden_after
        return ok, {"mode": mode, "observed": observed, "expected": expected,
                    "take2_result": result, "cursor_mid": mid, "hidden_after": hidden_after}

    if mode == "ui_element_players":
        # Set UI Focus reached only the players who use each element: Player 1's button is
        # player 1's, Any Player is everyone's, except for a player whose camera does not draw
        # it (Set Camera Visibility).
        expected = {"p1_user1": "done", "p1_user0": "failed", "any_user2": "done",
                    "hidden_user1": "failed", "hidden_user0": "done"}
        observed = {name: str(owner.get("focus_" + name, "")) for name in expected}
        return observed == expected, {"mode": mode, "observed": observed, "expected": expected}

    if mode == "lod_nearest_view":
        # Each rock shows the mesh for its nearest player view: full detail near player 0 or
        # player 1, the middle one 33 m from player 0, the far one away from both (not the far
        # mesh everywhere, as the menu camera drawn last used to leave them).
        scene = bge.logic.getCurrentScene()
        shown = {}
        for name in ("Near0", "Near1", "Mid", "Far"):
            rock = next((obj for obj in scene.objects if obj.name.endswith(f"_Lod{name}")), None)
            shown[name] = rock.meshes[0].name if rock is not None and rock.meshes else ""
        expected = {
            "Near0": str(owner.get("__ln_acceptance_mesh_Near0", "")),
            "Near1": str(owner.get("__ln_acceptance_mesh_Near1", "")),
            "Mid": str(owner.get("__ln_acceptance_mesh_low", "")),
            "Far": str(owner.get("__ln_acceptance_mesh_far", "")),
        }
        ok = all(expected[name] != "" and shown[name] == expected[name] for name in expected)
        return ok, {"mode": mode, "shown": shown, "expected": expected}

    if mode == "camera_renderers":
        # The views drew, stopped long enough for their renderers to be given back, and drew
        # again (shared renderer, Workbench renderer, idle freeing and re-creation).
        scene = bge.logic.getCurrentScene()
        names = [str(owner.get("__ln_acceptance_cr_" + key, "")) for key in ("a", "b", "w")]
        back_on = all(scene.objects.get(name) is not None and scene.objects[name].useViewport
                      for name in names)
        details = {
            "mode": mode,
            "phase": int(owner.get("cr_phase", 0)),
            "frames_on": int(owner.get("cr_frames_on", 0)),
            "frames_off": int(owner.get("cr_frames_off", 0)),
            "frames_back": int(owner.get("cr_frames_back", 0)),
            "back_on": back_on,
            "error": str(owner.get("cr_error", "")),
        }
        ok = (details["phase"] == 4 and details["frames_on"] >= 3 and details["frames_off"] >= 3
              and details["frames_back"] >= 5 and back_on and not details["error"])
        return ok, details

    if mode == "camera_minimap_options":
        # Each small view's pixels in a screenshot by color (drive_camera_minimap_options): Only
        # shows the markers (green sphere, red Add Object copy from a hidden child collection)
        # without the blue ground, Hide the opposite; Material colors are the materials' (yellow
        # markers, gray ground); Outline and Cavity change their views; the round view keeps the
        # main view in its corners and has a smooth edge.
        result = bge.logic.globalDict.get("mo_result", {})
        counts = result.get("counts", {})

        def count(key, name):
            return int(counts.get(key, {}).get(name, 0))

        def pixels(key):
            return max(1, sum(int(value) for value in counts.get(key, {}).values()))

        checks = {
            # Yellow: the collection instance's copy (its Empty is in the markers' collection).
            "only_markers": (count("only", "green") > 0 and count("only", "red") > 0
                             and count("only", "yellow") > 0),
            "only_no_ground": count("only", "blue") == 0 and count("only", "black") > 0,
            "hide_no_markers": (count("hide", "green") == 0 and count("hide", "red") == 0
                                and count("hide", "yellow") == 0),
            "hide_ground": count("hide", "blue") > 0,
            # The cyan cube Set Collection Member put in the markers' collection.
            "only_joined": count("only", "cyan") > 0 and count("round", "cyan") > 0,
            "hide_no_joined": count("hide", "cyan") == 0,
            "plain_joined": count("plain", "cyan") > 0,
            "material_colors": (count("material", "yellow") > 0 and count("material", "gray") > 0
                                and count("material", "green") == 0
                                and count("material", "red") == 0
                                and count("material", "blue") == 0),
            "plain_object_colors": (count("plain", "blue") > 0 and count("plain", "green") > 0
                                    and count("plain", "red") > 0
                                    and count("plain", "yellow") > 0),
            "outline": int(result.get("outline_redder", 0)) > 0,
            "cavity": int(result.get("cavity_changed", 0)) > 0,
            "cavity_screen": int(result.get("screen_changed", 0)) > 0,
            # Corners as the main view next to them, and not the minimap's black.
            "round_corners": (len(result.get("round_corner_diffs", [])) == 4
                              and max(result["round_corner_diffs"]) <= 24
                              and min(result["round_corner_sums"]) > 60),
            "round_middle": result.get("round_middle") == "black",
            # A smooth edge: a good part of the edge pixels are in between.
            "round_edge": (len(result.get("round_edge", [])) == 2
                           and result["round_edge"][0] * 4 >= result["round_edge"][1] > 0),
            # Lighting (2026-10-15): Studio shades the markers; MatCap draws the scene's red wax
            # MatCap on every marker (Single color), which Studio's gray would not give. Shares of
            # a view's pixels: the player's window is small (the sphere is about 4%).
            "studio": int(result.get("studio_changed", 0)) > 0.02 * pixels("studio"),
            "matcap": (count("matcap", "red") > 0.02 * pixels("matcap")
                       and count("matcap", "green") == 0 and count("matcap", "yellow") == 0
                       and count("matcap", "cyan") == 0),
            # Attribute: the sphere's magenta color attribute instead of its green object color.
            "attribute": (count("attribute", "magenta") > 0.02 * pixels("attribute")
                          and count("attribute", "green") == 0),
            # Random: a color per object name; the blue ground changes.
            "random": float(result.get("random_changed", 0.0)) > 0.5,
        }
        details = {"mode": mode, "checks": checks, "result": result,
                   "phase": int(owner.get("mo_phase", 0)), "error": str(owner.get("mo_error", ""))}
        return all(checks.values()) and details["phase"] == 3 and not details["error"], details

    if mode == "camera_visibility_still":
        # Nothing moves in the scene: the top camera's renderer has finished its samples when
        # Set Camera Visibility hides the red cube from it (drive_camera_visibility_still). It
        # kept showing its last picture, with the cube, until Blender was told (2026-10-25).
        result = bge.logic.globalDict.get("cvs_result", {})
        checks = {
            "cube_drawn_before": int(result.get("before", 0)) > 50,
            "cube_hidden_after": int(result.get("after", -1)) == 0,
        }
        details = {"mode": mode, "checks": checks, "result": result,
                   "phase": int(owner.get("cvs_phase", 0)),
                   "error": str(owner.get("cvs_error", ""))}
        return all(checks.values()) and details["phase"] == 4 and not details["error"], details

    if mode == "material_actions":
        # The owner played a material action (On Init): only it has a copy of the material, with
        # the action's last color; the other cube keeps the shared material. drive_material_actions
        # played it on an Add Object copy too, then ended the copy.
        import bpy

        scene = bge.logic.getCurrentScene()
        source = str(owner.get("__ln_acceptance_ma_material", ""))
        other = scene.objects.get(str(owner.get("__ln_acceptance_ma_other", "")))
        glow = (1.0, 0.5, 0.0)
        rest = (0.2, 0.2, 0.2)

        def material_of(obj):
            slot = obj.blenderObject.material_slots[0] if obj is not None else None
            material = slot.material if slot is not None else None
            if material is None:
                return "", (9.0, 9.0, 9.0)
            return material.name, tuple(float(v) for v in material.diffuse_color[:3])

        mine, mine_rgb = material_of(owner)
        theirs, theirs_rgb = material_of(other)
        source_rgb = (tuple(float(v) for v in bpy.data.materials[source].diffuse_color[:3])
                      if source in bpy.data.materials else (9.0, 9.0, 9.0))
        spawned_rg = (float(owner.get("ma_spawned_r", 9.0)), float(owner.get("ma_spawned_g", 9.0)))
        checks = {
            "own_copy": mine not in ("", source),
            "own_glow": close_tuple(mine_rgb, glow, 0.01),
            "other_shared": theirs == source,
            "other_rest": close_tuple(theirs_rgb, rest, 0.01),
            "source_rest": close_tuple(source_rgb, rest, 0.01),
            "spawn_shared_before": str(owner.get("ma_spawn_before", "")) == source,
            "spawn_own_copy": str(owner.get("ma_spawned_material", "")) not in ("", source),
            "spawn_glow": abs(spawned_rg[0] - glow[0]) <= 0.01 and abs(spawned_rg[1] - glow[1]) <= 0.01,
            "spawn_copy_gone": bool(owner.get("ma_spawned_copy_gone", False)),
        }
        return all(checks.values()), {
            "mode": mode, "checks": checks, "mine": mine, "mine_rgb": list(mine_rgb),
            "theirs": theirs, "theirs_rgb": list(theirs_rgb), "spawned_rg": list(spawned_rg),
            "spawned_material": str(owner.get("ma_spawned_material", "")),
            "phase": int(owner.get("ma_phase", 0)),
        }

    if mode == "collection_membership":
        # What drive_collection_membership recorded after each step: Get Collection Objects
        # without and with child collections, the collection linked into List Length, and the
        # objects Loop From List went through.
        store = bge.logic.globalDict
        names = {key: str(owner.get("__ln_acceptance_cm_" + key, ""))
                 for key in ("ring", "icon", "blip", "marker", "marker_child", "enemy", "tree",
                             "instancer", "enemy_nest", "friend_nest")}

        def recorded(step):
            record = store.get("cm_" + step, {})
            return (record.get("direct"), record.get("all"), record.get("linked"),
                    record.get("seen"))

        def expected(direct, all_count, *keys):
            return (direct, all_count, all_count, sorted(names[key] for key in keys))

        hidden = store.get("cm_hidden", {})
        joined = store.get("cm_joined", {})
        checks = {
            "start": recorded("start") == expected(1, 2, "ring", "icon"),
            # The copy is in the hidden child collection its template is in.
            "spawned": recorded("spawned") == expected(1, 3, "ring", "icon", "blip"),
            # The marker and its child, put in by one Set Collection Member.
            "added": recorded("added") == expected(
                3, 5, "ring", "icon", "blip", "marker", "marker_child"),
            # Taken out of the collection, the icon is out of its child collections too.
            "removed": recorded("removed") == expected(
                3, 4, "ring", "blip", "marker", "marker_child"),
            "saved": store.get("cm_save_ok") is True,
            "undone": recorded("undone") == expected(1, 2, "ring", "blip"),
            "loaded_ok": store.get("cm_load_ok") is True,
            # Load Game puts the saved objects' changes back.
            "loaded": recorded("loaded") == expected(
                3, 4, "ring", "blip", "marker", "marker_child"),
            # Set Collection Member on an Empty: the collection instance's tree follows it.
            "nested": recorded("nested") == expected(
                5, 6, "ring", "blip", "marker", "marker_child", "instancer", "tree"),
            # Its Empty removed, the tree copy is no longer in the Empty's collections. FriendNest
            # (spawned into Library) and its tree are in the child collections.
            "unnested": recorded("unnested") == expected(
                4, 8, "ring", "marker", "marker_child", "enemy", "blip", "blip", "friend_nest",
                "tree"),
            # With Enemies and Assets hidden: the asset is hidden; a collection instance's tree
            # is drawn when its Empty is in a drawn collection (Instancer, and FriendNest spawned
            # while Assets was hidden), hidden with EnemyNest spawned into Enemies.
            "instances_drawn_by_empty": hidden.get("trees") == {
                "": False, names["instancer"]: True, names["enemy_nest"]: False,
                names["friend_nest"]: True},
            # A collection hidden by Set Collection Render Visibility: copies spawned after it are
            # drawn (they were all hidden before 2026-10-15), its own object is not.
            "copies_drawn": hidden.get("copies") == [True, True],
            "others_drawn": hidden.get("ring") is True and hidden.get("marker") is True,
            "enemy_hidden": hidden.get("enemy") is False,
            # Put in a drawn collection, the hidden collection's object is drawn.
            "enemy_joined": joined.get("enemy") is True,
        }
        details = {
            "mode": mode,
            "checks": checks,
            "records": {step: recorded(step) for step in
                        ("start", "spawned", "added", "removed", "undone", "loaded", "nested",
                         "unnested")},
            "hidden": hidden,
            "joined": joined,
            "phase": int(owner.get("cm_phase", 0)),
            "error": str(owner.get("cm_error", "")),
        }
        return all(checks.values()) and not details["error"], details

    if mode == "split_screen":
        # Players 0 to 2 in Favor Top with a 2 pixel divider, and a minimap on top.
        scene = bge.logic.getCurrentScene()
        cameras = [scene.objects.get(str(owner.get("__ln_acceptance_camera_%d" % index, "")))
                   for index in range(3)]
        minimap = scene.objects.get(str(owner.get("__ln_acceptance_minimap", "")))
        use_viewport = [bool(camera is not None and camera.useViewport) for camera in cameras]
        minimap_viewport = bool(minimap is not None and minimap.useViewport)
        area = tuple(float(owner.get(name, -1.0))
                     for name in ("p2_left", "p2_top", "p2_width", "p2_height"))
        views = int(owner.get("views", 0))
        p2_shown = bool(owner.get("p2_shown", False))
        active = getattr(scene, "active_camera", None)
        active_name = active.name if active is not None else ""

        def scale(camera, row):
            return float(camera.projection_matrix[row][row]) if camera is not None else 0.0

        # Crop: the full-width top view keeps the whole picture's width, so its horizontal scale
        # is a grid cell's (a cell shows the whole picture) and its vertical scale twice a cell's.
        x_ratio = scale(cameras[0], 0) / max(scale(cameras[1], 0), 1e-6)
        y_ratio = scale(cameras[0], 1) / max(scale(cameras[1], 1), 1e-6)
        ok = (
            all(use_viewport)
            and minimap_viewport
            and views == 3
            and p2_shown
            and all(abs(value - 0.5) <= 0.01 for value in area)
            and abs(x_ratio - 1.0) <= 0.02
            and abs(y_ratio - 2.0) <= 0.04
            and cameras[0] is not None
            and active_name == cameras[0].name
        )
        return ok, {
            "mode": mode,
            "use_viewport": use_viewport,
            "minimap_viewport": minimap_viewport,
            "views": views,
            "p2_shown": p2_shown,
            "p2_area": area,
            "x_ratio": x_ratio,
            "y_ratio": y_ratio,
            "active_camera": active_name,
        }

    if mode == "split_screen_leave":
        # Player 2's camera was removed and player 3's view cleared: players 0 and 1 share the
        # window, and player 3's camera draws full screen again when active (no viewport).
        scene = bge.logic.getCurrentScene()
        names = [str(owner.get("__ln_acceptance_camera_%d" % index, "")) for index in range(4)]
        cameras = [scene.objects.get(name) for name in names]
        ok = (
            cameras[2] is None
            and all(cameras[index] is not None for index in (0, 1, 3))
            and cameras[0].useViewport
            and cameras[1].useViewport
            and not cameras[3].useViewport
            and int(owner.get("views", 0)) == 2
            and not bool(owner.get("p3_shown", True))
            and bool(owner.get("left_done", False))
        )
        return ok, {
            "mode": mode,
            "cameras_left": [camera is not None for camera in cameras],
            "use_viewport": [bool(camera is not None and camera.useViewport) for camera in cameras],
            "views": int(owner.get("views", 0)),
            "p3_shown": bool(owner.get("p3_shown", True)),
            "left_done": bool(owner.get("left_done", False)),
        }

    if mode == "ui_menu_per_player":
        # Two players opened the same menu: each got a copy of their own, in their half of the
        # window at half size, reading their own player (user, gold, {gold} text), and its Set
        # Text found its own copy's tag by name. Then player 0 closed theirs; player 1's stayed.
        mid = bge.logic.globalDict.get("pp_mid", [])
        final = per_player_menu_state(owner)
        def copy_ok(copy, user, gold, side):
            # Its corner element at the top right of the player's half (half size), the badge
            # anchored inside it at half its authored offset (0.3, 0.2).
            # Its popups: its own copy of the template and the one its logic added.
            anchored = copy[5]
            return (copy[0] == user and copy[1] == gold and copy[6] == 2
                    and copy[2] == sorted([str(gold), f"player {user}"])
                    and abs(copy[3] - side) <= 0.03 and abs(copy[4] - 0.5) <= 0.01
                    and len(anchored) == 5
                    and side + 0.15 < anchored[0] < side + 0.25 and anchored[1] < 0.25
                    and abs(anchored[2] - 0.1) <= 0.005
                    and abs(anchored[3] - 0.15) <= 0.01 and abs(anchored[4] - 0.1) <= 0.01)
        if len(mid) == 2 and len(mid[0][5]) == 5 and len(mid[1][5]) == 5:
            # The same place in each half.
            if (abs(mid[1][5][0] - mid[0][5][0] - 0.5) > 0.01
                    or abs(mid[1][5][1] - mid[0][5][1]) > 0.01):
                return False, {"mode": mode, "mid": mid, "final": final}
        ok = (
            len(mid) == 2
            and copy_ok(mid[0], 0, 10, 0.25)
            and copy_ok(mid[1], 1, 20, 0.75)
            and len(final) == 1
            and copy_ok(final[0], 1, 20, 0.75)
        )
        return ok, {"mode": mode, "mid": mid, "final": final}

    if mode == "split_screen_players":
        # A HUD anchored to the top left of player 2's view, 5 units in front of player 2's
        # camera; one for player 5, who has no view, stays put. Set Player Gamepad gave
        # gamepad 3 to player 1, so player 3 has none.
        scene = bge.logic.getCurrentScene()
        camera = scene.objects.get(str(owner.get("__ln_acceptance_camera_2", "")))
        hud = scene.objects.get(str(owner.get("__ln_acceptance_hud", "")))
        idle_hud = scene.objects.get(str(owner.get("__ln_acceptance_idle_hud", "")))
        corner = (0.0, 0.0)
        distance = 0.0
        if camera is not None and hud is not None:
            local = camera.worldOrientation.transposed() @ (
                hud.worldPosition - camera.worldPosition)
            clip = camera.projection_matrix @ local.to_4d()
            if abs(clip.w) > 1e-6:
                corner = (clip.x / clip.w, clip.y / clip.w)
            distance = local.length
        idle_position = tuple(idle_hud.worldPosition) if idle_hud is not None else ()
        pads = [int(owner.get(name, -9)) for name in ("p1_pad", "p2_pad", "p3_pad")]
        ok = (
            camera is not None
            and camera.useViewport
            and abs(corner[0] + 1.0) <= 0.005
            and abs(corner[1] - 1.0) <= 0.005
            and abs(distance - 5.0) <= 0.01
            and len(idle_position) == 3
            and all(abs(value - 100.0) <= 1e-4 for value in idle_position)
            and pads == [3, 2, -1]
            and not bool(owner.get("p1_joined", True))
            and bool(owner.get("sound_started", False))
        )
        return ok, {
            "mode": mode,
            "corner": corner,
            "distance": distance,
            "idle_position": idle_position,
            "pads": pads,
            "p1_joined": bool(owner.get("p1_joined", True)),
            "sound_started": bool(owner.get("sound_started", False)),
        }

    if mode == "render_state":
        observed_fullscreen = bool(bge.render.getFullScreen())
        observed_width = int(bge.render.getWindowWidth())
        observed_height = int(bge.render.getWindowHeight())
        observed_vsync_interval = int(bge.render.getVsync())
        property_fullscreen = bool(owner.get("fullscreen_state", False))
        property_width = int(owner.get("resolution_width", 0))
        property_height = int(owner.get("resolution_height", 0))
        property_vsync_mode = int(owner.get("vsync_mode", -1))
        expected_vsync_mode = int(owner.get("__ln_acceptance_expected_vsync_mode", 1))
        expected_vsync_interval = int(owner.get("__ln_acceptance_expected_vsync_interval", 0))
        ok = (
            property_fullscreen == observed_fullscreen
            and property_width == observed_width
            and property_height == observed_height
            and property_vsync_mode == expected_vsync_mode
            and observed_vsync_interval == expected_vsync_interval
        )
        return ok, {
            "mode": mode,
            "observed_fullscreen": observed_fullscreen,
            "property_fullscreen": property_fullscreen,
            "observed_width": observed_width,
            "property_width": property_width,
            "observed_height": observed_height,
            "property_height": property_height,
            "property_vsync_mode": property_vsync_mode,
            "expected_vsync_mode": expected_vsync_mode,
            "observed_vsync_interval": observed_vsync_interval,
            "expected_vsync_interval": expected_vsync_interval,
        }

    if mode == "light_state":
        scene = bge.logic.getCurrentScene()
        expected_rgb = expected_color(owner)[:3]
        expected_power = float(owner.get("__ln_acceptance_expected_power", 0.0))
        expected_shadow = bool(owner.get("__ln_acceptance_expected_shadow", False))
        source_name = owner.get("__ln_acceptance_source_light", "")
        mirror_name = owner.get("__ln_acceptance_mirror_light", "")
        source = next((game_object for game_object in scene.objects if game_object.name == source_name), None)
        mirror = next((game_object for game_object in scene.objects if game_object.name == mirror_name), None)

        def read_light_state(game_object):
            blender_object = getattr(game_object, "blenderObject", None) if game_object is not None else None
            data = getattr(blender_object, "data", None) if blender_object is not None else None
            if data is None:
                return None, None, None
            return tuple(float(component) for component in data.color), float(data.energy), bool(data.use_shadow)

        source_color, source_power, source_shadow = read_light_state(source)
        mirror_color, mirror_power, _mirror_shadow = read_light_state(mirror)
        ok = (
            source_color is not None
            and mirror_color is not None
            and close_tuple(source_color, expected_rgb, tolerance)
            and abs(source_power - expected_power) <= tolerance
            and source_shadow == expected_shadow
            and close_tuple(mirror_color, expected_rgb, tolerance)
            and abs(mirror_power - expected_power) <= tolerance
        )
        return ok, {
            "mode": mode,
            "source_found": source is not None,
            "mirror_found": mirror is not None,
            "source_color": source_color,
            "mirror_color": mirror_color,
            "expected_color": expected_rgb,
            "source_power": source_power,
            "mirror_power": mirror_power,
            "expected_power": expected_power,
            "source_shadow": source_shadow,
            "expected_shadow": expected_shadow,
        }

    if mode == "light_unique":
        scene = bge.logic.getCurrentScene()
        expected_source_rgb = expected_color(owner)[:3]
        expected_source_power = float(owner.get("__ln_acceptance_expected_power", 0.0))
        expected_source_shadow = bool(owner.get("__ln_acceptance_expected_shadow", False))
        expected_mirror_rgb = (
            float(owner.get("__ln_acceptance_expected_mirror_r", 1.0)),
            float(owner.get("__ln_acceptance_expected_mirror_g", 1.0)),
            float(owner.get("__ln_acceptance_expected_mirror_b", 1.0)),
        )
        expected_mirror_power = float(owner.get("__ln_acceptance_expected_mirror_power", 0.0))
        expected_mirror_shadow = bool(owner.get("__ln_acceptance_expected_mirror_shadow", False))
        source_name = owner.get("__ln_acceptance_source_light", "")
        mirror_name = owner.get("__ln_acceptance_mirror_light", "")
        source = next((game_object for game_object in scene.objects if game_object.name == source_name), None)
        mirror = next((game_object for game_object in scene.objects if game_object.name == mirror_name), None)

        def read_light_state(game_object):
            blender_object = getattr(game_object, "blenderObject", None) if game_object is not None else None
            data = getattr(blender_object, "data", None) if blender_object is not None else None
            if data is None:
                return None, None, None, 0, ""
            return (
                tuple(float(component) for component in data.color),
                float(data.energy),
                bool(data.use_shadow),
                int(getattr(data, "users", 0)),
                str(getattr(data, "name", "")),
            )

        source_color, source_power, source_shadow, source_users, source_data_name = read_light_state(source)
        mirror_color, mirror_power, mirror_shadow, mirror_users, mirror_data_name = read_light_state(mirror)
        ok = (
            source_color is not None
            and mirror_color is not None
            and close_tuple(source_color, expected_source_rgb, tolerance)
            and abs(source_power - expected_source_power) <= tolerance
            and source_shadow == expected_source_shadow
            and close_tuple(mirror_color, expected_mirror_rgb, tolerance)
            and abs(mirror_power - expected_mirror_power) <= tolerance
            and mirror_shadow == expected_mirror_shadow
            and source_users == 1
            and source_data_name
            and mirror_data_name
            and source_data_name != mirror_data_name
        )
        return ok, {
            "mode": mode,
            "source_found": source is not None,
            "mirror_found": mirror is not None,
            "source_color": source_color,
            "mirror_color": mirror_color,
            "expected_source_color": expected_source_rgb,
            "expected_mirror_color": expected_mirror_rgb,
            "source_power": source_power,
            "mirror_power": mirror_power,
            "expected_source_power": expected_source_power,
            "expected_mirror_power": expected_mirror_power,
            "source_shadow": source_shadow,
            "mirror_shadow": mirror_shadow,
            "expected_source_shadow": expected_source_shadow,
            "expected_mirror_shadow": expected_mirror_shadow,
            "source_users": source_users,
            "mirror_users": mirror_users,
            "source_data_name": source_data_name,
            "mirror_data_name": mirror_data_name,
        }

    if mode == "physics_state":
        scene = bge.logic.getCurrentScene()
        target_name = owner.get("__ln_acceptance_target", owner.name)
        target = next((game_object for game_object in scene.objects if game_object.name == target_name), None)
        observed_group = int(target.collisionGroup) if target is not None else -1
        property_group = int(owner.get("collision_group", 0))
        dynamics_suspended = bool(target.isSuspendDynamics) if target is not None else False
        expected_group = int(owner.get("__ln_acceptance_expected_collision_group", 0))
        ok = (
            target is not None
            and observed_group == expected_group
            and property_group == expected_group
            and dynamics_suspended
        )
        return ok, {
            "mode": mode,
            "observed_group": observed_group,
            "property_group": property_group,
            "target_found": target is not None,
            "expected_group": expected_group,
            "dynamics_suspended": dynamics_suspended,
        }

    if mode == "character_state":
        observed_position = vector_tuple(owner.worldPosition)
        property_gravity = (
            float(owner.get("character_gravity_x", 0.0)),
            float(owner.get("character_gravity_y", 0.0)),
            float(owner.get("character_gravity_z", 0.0)),
        )
        property_walk = (
            float(owner.get("character_walk_x", 0.0)),
            float(owner.get("character_walk_y", 0.0)),
            float(owner.get("character_walk_z", 0.0)),
        )
        property_max_jumps = int(owner.get("character_max_jumps", 0))
        property_jump_count = int(owner.get("character_jump_count", 0))
        property_on_ground = bool(owner.get("character_on_ground", True))
        expected_gravity = expected_named_vector(owner, "gravity")
        expected_walk = expected_named_vector(owner, "walk")
        expected_max_jumps = int(owner.get("__ln_acceptance_expected_max_jumps", 0))
        min_jump_count = int(owner.get("__ln_acceptance_min_jump_count", 0))
        min_x = float(owner.get("__ln_acceptance_min_x", 0.0))
        min_y = float(owner.get("__ln_acceptance_min_y", 0.0))
        min_z = float(owner.get("__ln_acceptance_min_z", 0.0))
        state_delivery_ok = (
            close_tuple(property_gravity, expected_gravity, tolerance)
            and close_tuple(property_walk, expected_walk, tolerance)
            and property_max_jumps == expected_max_jumps
        )
        motion_ok = (
            property_jump_count >= min_jump_count
            and observed_position[0] >= min_x
            and observed_position[1] >= min_y
            and observed_position[2] >= min_z
            and not property_on_ground
        )
        ok = state_delivery_ok
        return ok, {
            "mode": mode,
            "state_delivery_ok": state_delivery_ok,
            "motion_ok": motion_ok,
            "observed_position": observed_position,
            "property_gravity": property_gravity,
            "expected_gravity": expected_gravity,
            "property_walk": property_walk,
            "expected_walk": expected_walk,
            "property_max_jumps": property_max_jumps,
            "expected_max_jumps": expected_max_jumps,
            "property_jump_count": property_jump_count,
            "min_jump_count": min_jump_count,
            "property_on_ground": property_on_ground,
            "min_x": min_x,
            "min_y": min_y,
            "min_z": min_z,
        }

    if mode == "object_actions":
        observed_position = vector_tuple(owner.worldPosition)
        observed_rotation = orientation_tuple(owner.worldOrientation)
        observed_color = color_tuple(owner.color)
        observed_visible = bool(owner.visible)
        rotation = expected_rotation(owner)
        expected_visible = bool(owner.get("__ln_acceptance_expected_visible", False))
        color = expected_color(owner)
        ok = (
            close_tuple(observed_position, expected, tolerance)
            and close_tuple(observed_rotation, rotation, tolerance)
            and close_color(observed_color, color, tolerance)
            and observed_visible == expected_visible
        )
        return ok, {
            "mode": mode,
            "observed_position": observed_position,
            "expected_position": expected,
            "observed_rotation": observed_rotation,
            "expected_rotation": rotation,
            "observed_color": observed_color,
            "expected_color": color,
            "observed_visible": observed_visible,
            "expected_visible": expected_visible,
        }

    if mode == "object_lifecycle":
        scene_objects = tuple(bge.logic.getCurrentScene().objects)
        object_names = [game_object.name for game_object in scene_objects]
        template_name = owner.get("__ln_acceptance_template", "")
        active_template_name = owner.get("__ln_acceptance_active_template", "")
        victim_name = owner.get("__ln_acceptance_victim", "")
        child_name = owner.get("__ln_acceptance_child", "")
        parent_name = owner.get("__ln_acceptance_parent", "")
        target_name = owner.get("__ln_acceptance_unparent_target", "")
        replica_position = expected_named_vector(owner, "replica_spawn")
        full_copy_position = expected_named_vector(owner, "full_copy_spawn")
        repeat_spawn_position = expected_named_vector(owner, "repeat_spawn")
        result_output_position = expected_named_vector(owner, "result_output_spawn")
        ordered_copy_position = expected_named_vector(owner, "ordered_copy_spawn")
        life_present_position = expected_named_vector(owner, "life_present_spawn")
        full_life_present_position = expected_named_vector(owner, "full_life_present_spawn")
        life_expired_position = expected_named_vector(owner, "life_expired_spawn")
        active_spawn_position = expected_named_vector(owner, "active_reject_spawn")
        added_id_a = owner.get("added_object_id_a", "")
        added_id_b = owner.get("added_object_id_b", "")
        added_object_ids_unique = (
            added_id_a.startswith(f"{template_name}.")
            and added_id_b.startswith(f"{template_name}.")
            and added_id_a != added_id_b
        )
        target = next((game_object for game_object in scene_objects if game_object.name == target_name), None)
        child = next((game_object for game_object in scene_objects if game_object.name == child_name), None)
        matching_positions = [
            vector_tuple(game_object.worldPosition)
            for game_object in scene_objects
            if game_object.name.startswith(template_name)
        ]
        active_template_spawn_found = any(
            close_tuple(vector_tuple(game_object.worldPosition), active_spawn_position, tolerance)
            for game_object in scene_objects
            if active_template_name and game_object.name.startswith(active_template_name)
        )
        victim_exists = any(name == victim_name for name in object_names)
        parent_removed = target is not None and getattr(target, "parent", None) is None
        child_parented = child is not None and getattr(child, "parent", None) is not None and \
            child.parent.name == parent_name
        replica_found = any(
            close_tuple(position, replica_position, tolerance) for position in matching_positions)
        full_copy_found = any(
            close_tuple(position, full_copy_position, tolerance) for position in matching_positions)
        result_output_found = any(
            close_tuple(position, result_output_position, tolerance) for position in matching_positions)
        ordered_copy_found = any(
            close_tuple(position, ordered_copy_position, tolerance) for position in matching_positions)
        life_present_found = any(
            close_tuple(position, life_present_position, tolerance) for position in matching_positions)
        full_life_present_found = any(
            close_tuple(position, full_life_present_position, tolerance) for position in matching_positions)
        life_expired_found = any(
            close_tuple(position, life_expired_position, tolerance) for position in matching_positions)
        spawned_logic_found = any(
            bool(game_object.get("spawn_logic_ran", False))
            for game_object in scene_objects
            if game_object.name.startswith(template_name)
        )
        repeat_spawn_count = sum(
            1 for position in matching_positions
            if close_tuple(position, repeat_spawn_position, tolerance)
        )
        ok = (
            len(matching_positions) >= 8
            and not active_template_spawn_found
            and not life_expired_found
            and not victim_exists
            and parent_removed
            and child_parented
            and replica_found
            and full_copy_found
            and result_output_found
            and ordered_copy_found
            and life_present_found
            and full_life_present_found
            and spawned_logic_found
            and repeat_spawn_count >= 2
            and added_object_ids_unique
        )
        return ok, {
            "mode": mode,
            "matching_positions": matching_positions,
            "repeat_spawn_count": repeat_spawn_count,
            "result_output_found": result_output_found,
            "ordered_copy_found": ordered_copy_found,
            "life_present_found": life_present_found,
            "full_life_present_found": full_life_present_found,
            "life_expired_found": life_expired_found,
            "spawned_logic_found": spawned_logic_found,
            "victim_exists": victim_exists,
            "parent_removed": parent_removed,
            "child_parented": child_parented,
            "replica_found": replica_found,
            "full_copy_found": full_copy_found,
            "active_template_spawn_found": active_template_spawn_found,
            "added_id_a": added_id_a,
            "added_id_b": added_id_b,
            "added_object_ids_unique": added_object_ids_unique,
            "objects": object_names,
        }

    if mode == "object_queries":
        has_parent = bool(owner.get("has_parent", False))
        has_child = bool(owner.get("has_child", False))
        missing_child_is_none = bool(owner.get("missing_child_is_none", False))
        ok = has_parent and has_child and missing_child_is_none
        return ok, {
            "mode": mode,
            "has_parent": has_parent,
            "has_child": has_child,
            "missing_child_is_none": missing_child_is_none,
        }

    if mode == "average_center":
        prefixes = (
            "vector_center",
            "object_center",
            "collection_center",
            "empty_vector_center",
            "empty_object_center",
            "empty_collection_center",
        )
        observed = {
            prefix: (
                (
                    float(owner.get(f"{prefix}_x", 0.0)),
                    float(owner.get(f"{prefix}_y", 0.0)),
                    float(owner.get(f"{prefix}_z", 0.0)),
                ),
                int(owner.get(f"{prefix}_count", 0)),
                bool(owner.get(f"{prefix}_valid", False)),
            )
            for prefix in prefixes
        }
        expected_centers = {
            prefix: expected_named_vector(owner, prefix) for prefix in prefixes
        }
        expected_counts = {
            prefix: int(owner.get(f"{prefix}_expected_count", -1)) for prefix in prefixes
        }
        expected_valid = {
            prefix: bool(owner.get(f"{prefix}_expected_valid", False)) for prefix in prefixes
        }
        ok = all(
            close_tuple(observed[prefix][0], expected_centers[prefix], tolerance)
            and observed[prefix][1] == expected_counts[prefix]
            and observed[prefix][2] == expected_valid[prefix]
            for prefix in prefixes
        )
        return ok, {
            "mode": mode,
            "observed": observed,
            "expected_centers": expected_centers,
            "expected_counts": expected_counts,
            "expected_valid": expected_valid,
        }

    if mode == "raycast_from_mesh":
        list_names = (
            "hits", "blocked", "hit_objects", "points", "normals", "distances",
            "fractions", "face_indices", "has_uvs", "uvs", "ray_origins",
            "ray_directions", "end_points",
        )
        list_lengths = {
            name: int(owner.get(f"mesh_face_len_{name}", -1)) for name in list_names
        }
        blocked_list_lengths = {
            name: int(owner.get(f"mesh_blocked_len_{name}", -1)) for name in list_names
        }
        observed = {
            "face_done": bool(owner.get("mesh_face_done", False)),
            "face_any": bool(owner.get("mesh_face_any", False)),
            "face_all": bool(owner.get("mesh_face_all", False)),
            "face_hit_count": int(owner.get("mesh_face_hit_count", -1)),
            "face_ray_count": int(owner.get("mesh_face_ray_count", -1)),
            "face_first_hit": bool(owner.get("mesh_face_first_hit", False)),
            "face_first_blocked": bool(owner.get("mesh_face_first_blocked", True)),
            "face_first_object": bool(owner.get("mesh_face_first_object", False)),
            "face_average_count": int(owner.get("mesh_face_average_count", -1)),
            "face_average_valid": bool(owner.get("mesh_face_average_valid", False)),
            "face_average_x": float(owner.get("mesh_face_average_x", 0.0)),
            "face_average_y": float(owner.get("mesh_face_average_y", 0.0)),
            "face_average_z": float(owner.get("mesh_face_average_z", 0.0)),
            "vertex_any": bool(owner.get("mesh_vertex_any", False)),
            "vertex_all": bool(owner.get("mesh_vertex_all", False)),
            "vertex_hit_count": int(owner.get("mesh_vertex_hit_count", -1)),
            "vertex_ray_count": int(owner.get("mesh_vertex_ray_count", -1)),
            "blocked_any": bool(owner.get("mesh_blocked_any", True)),
            "blocked_all": bool(owner.get("mesh_blocked_all", True)),
            "blocked_hit_count": int(owner.get("mesh_blocked_hit_count", -1)),
            "blocked_ray_count": int(owner.get("mesh_blocked_ray_count", -1)),
            "blocked_first": bool(owner.get("mesh_blocked_first", False)),
            "blocked_average_count": int(owner.get("mesh_blocked_average_count", -1)),
            "blocked_average_valid": bool(owner.get("mesh_blocked_average_valid", True)),
        }
        ok = (
            observed["face_done"]
            and observed["face_any"]
            and not observed["face_all"]
            and observed["face_hit_count"] == 1
            and observed["face_ray_count"] == 16
            and observed["face_first_hit"]
            and not observed["face_first_blocked"]
            and observed["face_first_object"]
            and all(
                list_lengths[name] == (16 if name in {
                    "hits", "blocked", "ray_origins", "ray_directions", "end_points"
                } else 1)
                for name in list_names
            )
            and observed["face_average_count"] == 1
            and observed["face_average_valid"]
            and abs(observed["face_average_x"] - 2.25) <= tolerance
            and abs(observed["face_average_y"] + 0.8) <= tolerance
            and abs(observed["face_average_z"] + 0.8) <= tolerance
            and observed["vertex_any"]
            and observed["vertex_all"]
            and observed["vertex_hit_count"] == 7
            and observed["vertex_ray_count"] == 7
            and not observed["blocked_any"]
            and not observed["blocked_all"]
            and observed["blocked_hit_count"] == 0
            and observed["blocked_ray_count"] == 4
            and observed["blocked_first"]
            and observed["blocked_average_count"] == 0
            and not observed["blocked_average_valid"]
            and all(
                blocked_list_lengths[name] == (4 if name in {
                    "hits", "blocked", "ray_origins", "ray_directions", "end_points"
                } else 0)
                for name in list_names
            )
        )
        return ok, {
            "mode": mode,
            "observed": observed,
            "list_lengths": list_lengths,
            "blocked_list_lengths": blocked_list_lengths,
        }

    if mode == "raycast":
        hit = bool(owner.get("ray_hit", False))
        object_hit = bool(owner.get("ray_object", False))
        ray_all_hit = bool(owner.get("ray_all_hit", False))
        ray_all_count = int(owner.get("ray_all_count", 0))
        shape_hit = bool(owner.get("shape_hit", False))
        shape_all_hit = bool(owner.get("shape_all_hit", False))
        shape_all_count = int(owner.get("shape_all_count", 0))
        point = (
            float(owner.get("ray_point_x", 0.0)),
            float(owner.get("ray_point_y", 0.0)),
            float(owner.get("ray_point_z", 0.0)),
        )
        normal = (
            float(owner.get("ray_normal_x", 0.0)),
            float(owner.get("ray_normal_y", 0.0)),
            float(owner.get("ray_normal_z", 0.0)),
        )
        direction = (
            float(owner.get("ray_direction_x", 0.0)),
            float(owner.get("ray_direction_y", 0.0)),
            float(owner.get("ray_direction_z", 0.0)),
        )
        end_point = (
            float(owner.get("ray_end_point_x", 0.0)),
            float(owner.get("ray_end_point_y", 0.0)),
            float(owner.get("ray_end_point_z", 0.0)),
        )
        expected_point = expected_named_vector(owner, "hit_point")
        expected_normal = expected_named_vector(owner, "hit_normal")
        expected_direction = expected_named_vector(owner, "hit_direction")
        expected_end_point = expected_named_vector(owner, "ray_end_point")
        ok = (
            hit
            and object_hit
            and ray_all_hit
            and ray_all_count == 1
            and shape_hit
            and shape_all_hit
            and shape_all_count == 1
            and close_tuple(point, expected_point, tolerance)
            and close_tuple(normal, expected_normal, tolerance)
            and close_tuple(direction, expected_direction, tolerance)
            and close_tuple(end_point, expected_end_point, tolerance)
        )
        return ok, {
            "mode": mode,
            "hit": hit,
            "object_hit": object_hit,
            "ray_all_hit": ray_all_hit,
            "ray_all_count": ray_all_count,
            "shape_hit": shape_hit,
            "shape_all_hit": shape_all_hit,
            "shape_all_count": shape_all_count,
            "point": point,
            "expected_point": expected_point,
            "normal": normal,
            "expected_normal": expected_normal,
            "direction": direction,
            "expected_direction": expected_direction,
            "end_point": end_point,
            "expected_end_point": expected_end_point,
        }

    if mode == "tier_a":
        list_len = int(owner.get("tier_a_list_len", 0))
        dict_len = int(owner.get("tier_a_dict_len", 0))
        global_value = float(owner.get("tier_a_global", 0.0))
        found_by_name = bool(owner.get("tier_a_found_by_name", False))
        owner_ok = bool(owner.get("tier_a_owner_ok", False))
        mover_name = str(owner.get("__ln_acceptance_mover", ""))
        mover = next(
            (candidate for candidate in owner.scene.objects if candidate.name == mover_name), None)
        translate_x = float(mover.worldPosition[0]) if mover is not None else 0.0
        ok = (
            list_len == 2
            and dict_len == 1
            and abs(global_value - 99.0) <= tolerance
            and found_by_name
            and owner_ok
            and mover is not None
            and translate_x >= float(owner.get("__ln_acceptance_min_x", 0.2))
        )
        return ok, {
            "mode": mode,
            "list_len": list_len,
            "dict_len": dict_len,
            "global_value": global_value,
            "found_by_name": found_by_name,
            "owner_ok": owner_ok,
            "translate_x": translate_x,
        }

    if mode in {"mouse_over", "mouse_over_cadence"}:
        entered = bool(owner.get("mouse_enter", False))
        over = bool(owner.get("mouse_over", False))
        exited = bool(owner.get("mouse_exit", False))
        over_count = float(owner.get("mouse_over_count", 0.0))
        min_over_count = float(owner.get("__ln_acceptance_min_mouse_over_count", 1.0))
        point = (
            float(owner.get("mouse_point_x", 0.0)),
            float(owner.get("mouse_point_y", 0.0)),
            float(owner.get("mouse_point_z", 0.0)),
        )
        normal = (
            float(owner.get("mouse_normal_x", 0.0)),
            float(owner.get("mouse_normal_y", 0.0)),
            float(owner.get("mouse_normal_z", 0.0)),
        )
        hit_object_name = str(owner.get("__ln_acceptance_mouse_hit_object", ""))
        hit_object = next(
            (
                candidate
                for candidate in bge.logic.getCurrentScene().objects
                if candidate.name == hit_object_name
            ),
            None,
        )
        object_output_ok = hit_object is not None and bool(
            hit_object.get("mouse_over_object_output", False))
        cadence_ok = (
            entered
            and over
            and not exited
            and over_count >= min_over_count
            and object_output_ok
        )
        dominant_axis = max(range(3), key=lambda axis: abs(normal[axis]))
        geometry_ok = (
            all(math.isfinite(value) for value in point + normal)
            and all(abs(value) <= 1.0 + tolerance for value in point)
            and abs(abs(point[dominant_axis]) - 1.0) <= tolerance
            and abs(abs(normal[dominant_axis]) - 1.0) <= tolerance
            and all(
                abs(normal[axis]) <= tolerance
                for axis in range(3)
                if axis != dominant_axis
            )
            and point[dominant_axis] * normal[dominant_axis] > 0.0
        )
        ok = cadence_ok and (mode == "mouse_over_cadence" or geometry_ok)
        return ok, {
            "mode": mode,
            "entered": entered,
            "over": over,
            "exited": exited,
            "over_count": over_count,
            "min_over_count": min_over_count,
            "point": point,
            "normal": normal,
            "hit_object_name": hit_object_name,
            "object_output_ok": object_output_ok,
        }

    return False, {"mode": mode, "error": "unknown mode"}


while not bge.logic.NextFrame():
    scene = bge.logic.getCurrentScene()
    owner = None
    for candidate in scene.objects:
        if candidate.get("__ln_acceptance_status_path", ""):
            owner = candidate
            break

    if owner is None:
        # A loader scene waits (bounded) for its asynchronous load to switch to the status scene.
        waits = int(bge.logic.globalDict.get("__ln_acceptance_await_frames", 0)) + 1
        bge.logic.globalDict["__ln_acceptance_await_frames"] = waits
        if waits < 1200 and any(
                candidate.get("__ln_acceptance_await_scene", "") for candidate in scene.objects):
            time.sleep(0.005)
            continue
        bge.logic.endGame()
        continue

    mode = owner.get("__ln_acceptance_mode", "")
    mutate_runtime_structure(owner)
    async_save_load_mode = mode in {
        "phase6_save_load",
        "phase6_save_load_structure",
        "phase6_save_load_material",
        "phase6_save_load_material_fresh",
        "phase6_save_load_existence",
        "phase6_cross_scene_save",
        "phase6_cross_scene_load",
    }
    if async_save_load_mode:
        frame_key = "__ln_acceptance_save_load_frame_" + owner.get("__ln_acceptance_run", "unknown")
        frame = int(bge.logic.globalDict.get(frame_key, 0)) + 1
        bge.logic.globalDict[frame_key] = frame
        time.sleep(0.01)
    else:
        frame = int(owner.get("__ln_acceptance_frame", 0)) + 1
        owner["__ln_acceptance_frame"] = frame
    if mode == "ui_grid_template":
        record_grid_slots(owner)
    if mode == "ui_grid_scrollbar":
        record_grid_scrollbar(owner)
    if mode == "game_property_types":
        try:
            check_game_property_types(owner)
        except Exception as error:
            owner["props_report"] = "error: " + repr(error)
            owner["props_stage"] = 2
            owner["ui_cycle_done"] = True
    if (mode == "object_menu_session" and owner.get("menu_external_ready", False) and
            not owner.get("menu_external_removed", False)):
        collection = bpy.data.collections.get(
            str(owner.get("__ln_acceptance_expected_collection", "")))
        if collection is not None:
            bge.logic.getCurrentScene().removeOverlayCollection(collection)
            owner["menu_external_removed"] = True
    if mode == "inactive_overlay_collection" and not owner.get("overlay_cycle_done", False):
        lifecycle_script_name = str(owner.get("overlay_cycle_script", ""))
        lifecycle_script = bpy.data.texts.get(lifecycle_script_name)
        if lifecycle_script is None:
            owner["overlay_cycle_error"] = "packed overlay lifecycle script is missing"
            owner["overlay_cycle_ok"] = False
            owner["overlay_cycle_done"] = True
        else:
            try:
                exec(
                    compile(
                        lifecycle_script.as_string(),
                        lifecycle_script_name,
                        "exec",
                    ),
                    {"owner": owner, "__name__": "__overlay_lifecycle_acceptance__"},
                )
            except Exception:
                owner["overlay_cycle_error"] = traceback.format_exc()
                owner["overlay_cycle_ok"] = False
                owner["overlay_cycle_done"] = True
    required_frame = int(owner.get("__ln_acceptance_required_frame", 1))
    check_now = frame >= required_frame
    if mode == "object_menu_session":
        # Check one controller tick after the session ends (done or failed: the native tree and
        # this controller run in an unspecified order), not at a frame count: the session ended
        # between frames 225 and 250 in the player, which counts render passes (2026-10-14).
        session_ended = bool(owner.get("menu_session_done", False) or
                             owner.get("menu_session_failed", False))
        check_now = (session_ended and bool(owner.get("menu_session_end_seen", False))) or (
            frame >= 8 * required_frame)
        if session_ended:
            owner["menu_session_end_seen"] = True
    if mode in ("ui_elements", "ui_menu_focus_restore", "ui_overlay_visibility",
                "ui_paused_routes", "ui_grid_template", "ui_grid_scrollbar", "ui_grid_pages",
                "ui_menu_pause_toggle", "ui_text_values_bar", "ui_grid_parented_template",
                "ui_menu_camera_hidden", "ui_example_file", "ui_templates",
                "game_property_types", "input_map", "ui_player_menu", "ui_menu_replace_inside",
                "split_screen", "split_screen_leave", "split_screen_players",
            "ui_menu_per_player", "ui_element_players",
            "ui_take_control_pointer", "ui_view_center", "ui_grid_inventory",
            "ui_sound_events"):
        # UI resolves on rendered frames, which the player runner does not count.
        # Check once the probe reported its result (a moment later for the looks),
        # with a time limit as fallback.
        start_key = "__ln_acceptance_ui_start_" + owner.get("__ln_acceptance_run", "unknown")
        start_time = float(bge.logic.globalDict.get(start_key, 0.0))
        if start_time <= 0.0:
            start_time = time.monotonic()
            bge.logic.globalDict[start_key] = start_time
        elapsed = time.monotonic() - start_time
        if mode == "ui_elements":
            ready = bool(owner.get("ui_focus_name", "")) and elapsed >= 1.0
        elif mode == "ui_player_menu":
            ready = int(owner.get("hl_user", -1)) >= 0 and elapsed >= 1.0
        elif mode == "ui_grid_inventory":
            ready = int(owner.get("bag_last", -1)) >= 0 and elapsed >= 1.0
        elif mode == "ui_menu_replace_inside":
            ready = elapsed >= 2.0
        elif mode == "ui_paused_routes":
            # Long enough after the paused Enter for frame logic to show it did not run.
            ready = bool(owner.get("ui_enter_while_paused", False)) and elapsed >= 1.5
        elif mode == "ui_sound_events":
            # B comes under the pointer half a second in.
            ready = str(owner.get("last_move", "")) == "b" or elapsed >= 4.0
        elif mode == "ui_menu_focus_restore":
            ready = bool(owner.get("ui_restored_focus", "")) or bool(
                owner.get("ui_set_focus_failed", False))
        elif mode == "input_map":
            # The rebind waits 0.2 s of real time, however fast frames come.
            ready = bool(owner.get("rebind_done", False)) and elapsed >= 0.5
        elif mode == "split_screen":
            # The views and their projections are set when frames are drawn.
            ready = elapsed >= 1.0
        elif mode == "split_screen_leave":
            ready = bool(owner.get("left_done", False)) and elapsed >= 1.5
        elif mode == "split_screen_players":
            ready = bool(owner.get("sound_started", False)) and elapsed >= 1.0
        elif mode == "ui_view_center":
            samples = bge.logic.globalDict.setdefault("vc_samples", {})

            def vc_sample(*names):
                return [bool(bge.logic.mouse.visible)] + [bool(owner.get(n, False)) for n in names]

            if 0.5 <= elapsed < 0.7 and "before" not in samples:
                samples["before"] = vc_sample("vc_enter_0", "vc_enter_1", "vc_exit_0")
            if 1.3 <= elapsed < 1.5 and "menu" not in samples:
                samples["menu"] = vc_sample("vc_enter_0", "vc_exit_0")
            if 2.4 <= elapsed and "after" not in samples:
                samples["after"] = vc_sample("vc_enter_0", "vc_enter_1", "vc_exit_0")
            ready = "after" in samples
        elif mode == "ui_take_control_pointer":
            if 0.8 <= elapsed < 1.1 and "tc_mid" not in bge.logic.globalDict:
                cursor = next((obj for obj in bge.logic.getCurrentScene().objects
                               if obj.name.endswith("_TCCursor")), None)
                if cursor is not None:
                    position = cursor.worldPosition
                    bge.logic.globalDict["tc_mid"] = (float(position[0]), float(position[1]),
                                                      float(position[2]), bool(cursor.visible))
            ready = bool(owner.get("tc_take2_after", "")) and elapsed >= 2.0
        elif mode == "ui_element_players":
            ready = all(owner.get("focus_" + name, "") for name in (
                "p1_user1", "p1_user0", "any_user2", "hidden_user1", "hidden_user0"))
        elif mode == "ui_menu_per_player":
            if 0.45 <= elapsed < 0.85 and "pp_mid" not in bge.logic.globalDict:
                bge.logic.globalDict["pp_mid"] = per_player_menu_state(owner)
            ready = elapsed >= 1.8
        elif mode in ("ui_example_file", "ui_templates"):
            # The menus open at the start; their grids are filled in the frames after.
            ready = elapsed >= 1.0
        else:
            ready = bool(owner.get("ui_cycle_done", False))
        check_now = ready or elapsed >= 8.0
    if mode == "camera_renderers":
        # Real time: the renderers are given back after 5 seconds without drawing.
        check_now = drive_camera_renderers(owner)
    if mode == "camera_minimap_options":
        check_now = drive_camera_minimap_options(owner)
    if mode == "collection_membership":
        check_now = drive_collection_membership(owner)
    if mode == "material_actions":
        check_now = drive_material_actions(owner)
    if mode == "camera_visibility_still":
        check_now = drive_camera_visibility_still(owner)
    if mode == "smooth_motion":
        check_now = drive_smooth_motion(owner)
    if mode == "frame_pacing":
        check_now = drive_frame_pacing(owner)
    if mode == "mouse_over_cadence":
        start_key = "__ln_acceptance_mouse_over_start_" + owner.get(
            "__ln_acceptance_run", "unknown")
        start_time = float(bge.logic.globalDict.get(start_key, 0.0))
        if start_time <= 0.0:
            start_time = time.monotonic()
            bge.logic.globalDict[start_key] = start_time
        check_now = time.monotonic() - start_time >= float(
            owner.get("__ln_acceptance_required_seconds", 0.5))
    if mode == "phase6_animation_status":
        start_key = "__ln_acceptance_animation_status_start_" + owner.get(
            "__ln_acceptance_run", "unknown")
        start_time = float(bge.logic.globalDict.get(start_key, 0.0))
        if start_time <= 0.0:
            start_time = time.monotonic()
            bge.logic.globalDict[start_key] = start_time
        check_now = bool(owner.get("action_lifecycle_completed", False)) or (
            time.monotonic() - start_time >= float(
                owner.get("__ln_acceptance_required_seconds", 2.0)))
    if async_save_load_mode:
        completion_property = (
            "save_completed" if mode == "phase6_cross_scene_save" else "load_completed"
        )
        completion_owner = owner
        if mode == "phase6_save_load_structure":
            structure_object = str(
                owner.get("__ln_acceptance_structure_object", ""))
            completion_owner = next(
                (
                    candidate
                    for candidate in bge.logic.getCurrentScene().objects
                    if candidate.name == structure_object
                ),
                owner,
            )
        check_now = bool(completion_owner.get(completion_property, False)) or check_now
    if check_now and not owner.get("__ln_acceptance_done", False):
        owner["__ln_acceptance_done"] = True
        run_id = owner.get("__ln_acceptance_run", "unknown")
        try:
            ok, details = check_owner(owner)
            details.update({"ok": ok, "run": run_id, "frame": frame})
            write_status(owner, details)
        except Exception:
            write_status(owner, {
                "ok": False,
                "run": run_id,
                "frame": frame,
                "error": traceback.format_exc(),
            })
        finally:
            bge.logic.endGame()

os._exit(0)
'''


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--runner", choices={"view3d", "player"}, default="view3d")
    parser.add_argument("--only", action="append", default=[])

    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    return parser.parse_args(argv)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def disable_legacy_logic_addons():
    for module_name in ("bge_netlogic", "bge_bricknodes"):
        try:
            addon_utils.disable(module_name, default_set=True)
        except Exception:
            pass

    blocked_prefixes = ("bge_netlogic", "bge_bricknodes")
    blocked_names = {"_on_deps_update", "establish_synching", "update_all_trees"}
    for handlers in (
        bpy.app.handlers.depsgraph_update_post,
        bpy.app.handlers.game_post,
        bpy.app.handlers.load_post,
        bpy.app.handlers.undo_post,
    ):
        for handler in list(handlers):
            module_name = getattr(handler, "__module__", "")
            function_name = getattr(handler, "__name__", "")
            if module_name.startswith(blocked_prefixes) or function_name in blocked_names:
                handlers.remove(handler)


def active_view3d_context():
    window = bpy.context.window
    if window is None:
        window_manager = bpy.context.window_manager
        windows = window_manager.windows if window_manager is not None else ()
        window = next(iter(windows), None)
    if window is None:
        raise RuntimeError("No Blender window available for view3d.game_start")
    screen = window.screen
    if screen is None:
        raise RuntimeError("Blender window has no screen for view3d.game_start")
    for area in screen.areas:
        if area.type != "VIEW_3D":
            continue
        for region in area.regions:
            if region.type == "WINDOW":
                return {
                    "window": window,
                    "screen": screen,
                    "area": area,
                    "region": region,
                    "scene": bpy.context.scene,
                }
    raise RuntimeError("No VIEW_3D area available for view3d.game_start")


def set_active_object(obj):
    for scene_obj in bpy.context.scene.objects:
        scene_obj.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def set_object_mode(obj, mode):
    set_active_object(obj)
    override = active_view3d_context()
    override.update(
        {
            "object": obj,
            "active_object": obj,
            "selected_objects": [obj],
            "selected_editable_objects": [obj],
        }
    )
    with bpy.context.temp_override(**override):
        result = bpy.ops.object.mode_set(mode=mode)
    require(result == {"FINISHED"}, f"Failed to enter {mode} mode for {obj.name!r}")


def set_game_property(obj, name, property_type, value):
    for prop in obj.game.properties:
        if prop.name == name:
            prop.type = property_type
            prop.value = value
            return prop

    set_active_object(obj)
    with bpy.context.temp_override(
        object=obj,
        active_object=obj,
        selected_objects=[obj],
        selected_editable_objects=[obj],
        scene=bpy.context.scene,
        view_layer=bpy.context.view_layer,
    ):
        result = bpy.ops.object.game_property_new(type=property_type, name=name)
    require(result == {"FINISHED"}, f"Failed to add game property {name!r}: {result}")
    prop = obj.game.properties[-1]
    prop.value = value
    return prop


def get_game_property_value(obj, name, fallback=None):
    for prop in obj.game.properties:
        if prop.name == name:
            return prop.value
    return fallback


def set_existing_game_property_value(obj, name, value):
    for prop in obj.game.properties:
        if prop.name == name:
            prop.value = value
            return
    raise RuntimeError(f"Game property {name!r} was not found on {obj.name!r}")


def reset_scene():
    for obj in list(bpy.context.scene.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for tree in list(bpy.data.node_groups):
        if tree.bl_idname == "LogicNodeTree" and tree.name.startswith(PREFIX):
            bpy.data.node_groups.remove(tree)
    for text in list(bpy.data.texts):
        if text.name.startswith(f"{PREFIX}_Controller"):
            bpy.data.texts.remove(text)


def configure_game_settings(fixed_timestep=True):
    scene = bpy.context.scene
    game_settings = scene.game_settings
    game_settings.physics_engine = "JOLT"
    game_settings.use_fixed_physics_timestep = fixed_timestep
    game_settings.physics_timestep_method = "FIXED" if fixed_timestep else "VARIABLE"
    game_settings.physics_tick_rate = 60
    game_settings.time_scale = 1.0

    if scene.camera is None:
        bpy.ops.object.camera_add(location=(0.0, -8.0, 5.0), rotation=(math.radians(60.0), 0.0, 0.0))
        camera = getattr(bpy.context, "object", None) or bpy.context.view_layer.objects.active
        require(camera is not None, "Failed to create the runtime acceptance camera")
        scene.camera = camera


def create_cube(name, rigid_body=False):
    bpy.ops.mesh.primitive_cube_add()
    obj = getattr(bpy.context, "object", None) or bpy.context.view_layer.objects.active
    require(obj is not None, f"Failed to create cube {name!r}")
    obj.name = name
    obj.location = (0.0, 0.0, 0.0)
    if rigid_body:
        obj.game.physics_type = "RIGID_BODY"
        obj.game.mass = 1.0
        obj.game.use_collision_bounds = True
        obj.game.collision_bounds_type = "BOX"
    return obj


def create_soft_body(name, location=(0.0, 0.0, 0.0)):
    mesh = bpy.data.meshes.new(f"{name}_Mesh")
    mesh.from_pydata(
        [(-0.75, -0.5, 0.0), (0.75, -0.5, 0.0), (0.0, 0.75, 0.0), (0.0, 0.0, 1.0)],
        [],
        [(0, 2, 1), (0, 1, 3), (1, 2, 3), (2, 0, 3)],
    )
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = location
    obj.game.physics_type = "SOFT_BODY"
    obj.game.mass = 2.5
    obj.game.gravity_factor = 0.0
    settings = obj.game.soft_body
    settings.dynamic_friction = 0.4
    settings.kdp = 0.2
    settings.kpr = 0.35
    settings.collision_margin = 0.04
    settings.position_solver_iterations = 9
    settings.use_faces_double_sided = True
    return obj


def create_armature(name):
    bpy.ops.object.armature_add(enter_editmode=False, location=(0.0, 0.0, 0.0))
    obj = getattr(bpy.context, "object", None) or bpy.context.view_layer.objects.active
    require(obj is not None, f"Failed to create armature {name!r}")
    obj.name = name
    obj.game.physics_type = "NO_COLLISION"
    return obj


def create_ground(name, location=(0.0, 0.0, -2.0), scale=(10.0, 10.0, 1.0)):
    obj = create_cube(name)
    obj.location = location
    obj.scale = scale
    obj.game.physics_type = "STATIC"
    obj.game.use_collision_bounds = True
    obj.game.collision_bounds_type = "BOX"
    return obj


def create_character(name, location=(0.0, 0.0, 0.0)):
    obj = create_cube(name)
    obj.location = location
    obj.game.physics_type = "CHARACTER"
    obj.game.use_collision_bounds = True
    obj.game.step_height = 0.35
    obj.game.jump_speed = 5.0
    if hasattr(obj.game, "jump_max"):
        obj.game.jump_max = 1
    else:
        obj.game.max_jumps = 1
    return obj


def create_test_action(name):
    action = bpy.data.actions.new(name)
    action.use_frame_range = True
    action.frame_start = 1.0
    action.frame_end = 40.0
    return action


def create_uv_sphere(name, location=(0.0, 0.0, 0.0)):
    bpy.ops.mesh.primitive_uv_sphere_add(location=location)
    obj = getattr(bpy.context, "object", None) or bpy.context.view_layer.objects.active
    require(obj is not None, f"Failed to create UV sphere {name!r}")
    obj.name = name
    return obj


def create_light(name, location, color=(1.0, 1.0, 1.0), power=1.0, use_shadow=True):
    bpy.ops.object.light_add(type="POINT", location=location)
    obj = getattr(bpy.context, "object", None) or bpy.context.view_layer.objects.active
    require(obj is not None, f"Failed to create light {name!r}")
    obj.name = name
    obj.data.color = color
    obj.data.energy = power
    obj.data.use_shadow = use_shadow
    return obj


def add_status_controller(obj, status_path, mode, required_frame=1, tolerance=0.0001,
                          required_seconds=None):
    set_game_property(obj, "__ln_acceptance_status_path", "STRING", str(status_path))
    set_game_property(obj, "__ln_acceptance_run", "STRING", "")
    set_game_property(obj, "__ln_acceptance_mode", "STRING", mode)
    set_game_property(obj, "__ln_acceptance_frame", "INT", 0)
    set_game_property(obj, "__ln_acceptance_required_frame", "INT", required_frame)
    set_game_property(obj, "__ln_acceptance_done", "BOOL", False)
    set_game_property(obj, "__ln_acceptance_tolerance", "FLOAT", tolerance)
    if required_seconds is not None:
        set_game_property(obj, "__ln_acceptance_required_seconds", "FLOAT", required_seconds)

    if RUNNER == "player":
        return

    controller_text = bpy.data.texts.new(f"{PREFIX}_Controller_{obj.name}.py")
    controller_text.write(GAME_CONTROLLER_SCRIPT)

    set_active_object(obj)
    with bpy.context.temp_override(
        object=obj,
        active_object=obj,
        selected_objects=[obj],
        selected_editable_objects=[obj],
        scene=bpy.context.scene,
        view_layer=bpy.context.view_layer,
    ):
        result = bpy.ops.logic.sensor_add(
            type="ALWAYS", name="LNAcceptanceAlways", object=obj.name)
        require(result == {"FINISHED"}, f"Failed to add always sensor: {result}")
        sensor = obj.game.sensors[-1]
        sensor.use_pulse_true_level = True
        sensor.tick_skip = 0

        result = bpy.ops.logic.controller_add(
            type="PYTHON", name="LNAcceptanceController", object=obj.name)
        require(result == {"FINISHED"}, f"Failed to add Python controller: {result}")
        controller = obj.game.controllers[-1]
        controller.mode = "SCRIPT"
        controller.text = controller_text
        controller.link(sensor=sensor)

        result = bpy.ops.logic.sensor_add(
            type="DELAY", name="LNAcceptanceFallbackDelay", object=obj.name)
        require(result == {"FINISHED"}, f"Failed to add fallback sensor: {result}")
        fallback_sensor = obj.game.sensors[-1]
        # The delay counts logic ticks; wall-clock checks need enough ticks at this tick rate.
        tick_rate = max(1, int(bpy.context.scene.game_settings.physics_tick_rate))
        seconds_ticks = 0 if required_seconds is None else math.ceil(required_seconds * tick_rate)
        fallback_sensor.delay = max(30, required_frame + 30, seconds_ticks + 30)
        fallback_sensor.duration = 0
        fallback_sensor.use_repeat = False

        result = bpy.ops.logic.controller_add(
            type="LOGIC_AND", name="LNAcceptanceFallbackController", object=obj.name)
        require(result == {"FINISHED"}, f"Failed to add fallback controller: {result}")
        fallback_controller = obj.game.controllers[-1]

        result = bpy.ops.logic.actuator_add(
            type="GAME", name="LNAcceptanceFallbackQuit", object=obj.name)
        require(result == {"FINISHED"}, f"Failed to add fallback quit actuator: {result}")
        fallback_actuator = obj.game.actuators[-1]
    fallback_actuator.mode = "QUIT"
    fallback_controller.link(sensor=fallback_sensor, actuator=fallback_actuator)


def add_inactive_overlay_lifecycle_controller(
        obj, camera_name, borrowed_camera_name, collection_name, item_name):
    """Stress owned overlay replicas without relying on editor-side visibility changes."""
    set_game_property(obj, "overlay_cycle_frame", "INT", 0)
    set_game_property(obj, "overlay_cycle_ok", "BOOL", False)
    set_game_property(obj, "overlay_cycle_error", "STRING", "")
    set_game_property(obj, "overlay_cycle_done", "BOOL", False)
    set_game_property(obj, "overlay_cycle_camera", "STRING", camera_name)
    set_game_property(obj, "overlay_cycle_borrowed_camera", "STRING", borrowed_camera_name)
    set_game_property(obj, "overlay_cycle_collection", "STRING", collection_name)
    set_game_property(obj, "overlay_cycle_item", "STRING", item_name)

    controller_text = bpy.data.texts.new(f"{PREFIX}_OverlayLifecycle_{obj.name}.py")
    controller_text.write(r'''
import traceback

import bge
import bpy


if globals().get("owner") is None:
    owner = bge.logic.getCurrentController().owner
if not owner.get("overlay_cycle_done", False):
    scene = bge.logic.getCurrentScene()
    frame = int(owner.get("overlay_cycle_frame", 0)) + 1
    owner["overlay_cycle_frame"] = frame
    camera_name = str(owner.get("overlay_cycle_camera", ""))
    borrowed_camera_name = str(owner.get("overlay_cycle_borrowed_camera", ""))
    collection_name = str(owner.get("overlay_cycle_collection", ""))
    item_name = str(owner.get("overlay_cycle_item", ""))

    def counts():
        return (len(scene.objects), len(scene.cameras), len(bpy.data.objects))

    def stored_counts(prefix):
        return (
            int(owner.get(prefix + "_objects", -1)),
            int(owner.get(prefix + "_cameras", -1)),
            int(owner.get(prefix + "_bpy_objects", -1)),
        )

    def store_counts(prefix):
        current = counts()
        owner[prefix + "_objects"] = current[0]
        owner[prefix + "_cameras"] = current[1]
        owner[prefix + "_bpy_objects"] = current[2]

    def require_state(active, expected_counts=None, borrowed_camera=False):
        live_camera = scene.objects.get(camera_name)
        source_camera = scene.objectsInactive.get(camera_name)
        item = scene.objects.get(item_name)
        if (item is not None) != active:
            raise RuntimeError(
                f"live overlay item state is {item is not None}, expected {active}")
        expected_owned_camera = active and not borrowed_camera
        # When the external borrowed camera drives this collection, the authored inactive camera
        # is legitimately instantiated as an ordinary collection member. Its name therefore
        # cannot identify which camera owns the overlay pass in that phase.
        if not borrowed_camera and (live_camera is not None) != expected_owned_camera:
            raise RuntimeError(
                "owned overlay camera state is "
                f"{live_camera is not None}, expected {expected_owned_camera}")
        if source_camera is None:
            raise RuntimeError("inactive source camera was lost")
        borrowed = scene.objects.get(borrowed_camera_name)
        borrowed_source = scene.objectsInactive.get(borrowed_camera_name)
        if borrowed is None or borrowed_source is not None:
            raise RuntimeError("borrowed auxiliary camera did not remain one active scene object")
        if expected_counts is not None and counts() != expected_counts:
            raise RuntimeError(
                f"runtime counts are {counts()}, expected stable counts {expected_counts}")

        # The custom viewport render path temporarily points the active View3D at the overlay
        # camera. Once removed, no editor area may retain the deleted runtime Blender object.
        screen = getattr(bpy.context, "screen", None)
        if not active and screen is not None and scene.active_camera is not None:
            active_blender_camera = scene.active_camera.blenderObject
            for area in screen.areas:
                if area.type == "VIEW_3D" and area.spaces.active.camera is not active_blender_camera:
                    raise RuntimeError("View3D did not return to the active gameplay camera")

    try:
        collection = bpy.data.collections.get(collection_name)
        if collection is None:
            raise RuntimeError("authored overlay collection was lost")

        if frame == 6:
            require_state(True)
            store_counts("overlay_cycle_active")
            scene.removeOverlayCollection(collection)
        elif frame == 10:
            require_state(False)
            store_counts("overlay_cycle_removed")
            scene.addOverlayCollection(scene.objectsInactive[camera_name], collection)
        elif frame == 14:
            require_state(True, stored_counts("overlay_cycle_active"))
            # The old runtime camera can still be queued for deletion in this step. Replacing it
            # immediately must neither remove the new instance nor transfer stale render state.
            scene.removeOverlayCollection(collection)
            scene.addOverlayCollection(scene.objectsInactive[camera_name], collection)
        elif frame == 18:
            require_state(True, stored_counts("overlay_cycle_active"))
            scene.removeOverlayCollection(collection)
        elif frame == 22:
            require_state(False, stored_counts("overlay_cycle_removed"))
            scene.addOverlayCollection(scene.objectsInactive[camera_name], collection)
        elif frame == 26:
            require_state(True, stored_counts("overlay_cycle_active"))
            scene.removeOverlayCollection(collection)
        elif frame == 30:
            require_state(False, stored_counts("overlay_cycle_removed"))
            scene.addOverlayCollection(scene.objects[borrowed_camera_name], collection)
        elif frame == 34:
            require_state(True, borrowed_camera=True)
            store_counts("overlay_cycle_borrowed_active")
            scene.removeOverlayCollection(collection)
        elif frame == 38:
            require_state(False, stored_counts("overlay_cycle_removed"))
            scene.addOverlayCollection(scene.objects[borrowed_camera_name], collection)
        elif frame == 42:
            require_state(
                True,
                stored_counts("overlay_cycle_borrowed_active"),
                borrowed_camera=True,
            )
            scene.removeOverlayCollection(collection)
        elif frame == 46:
            require_state(False, stored_counts("overlay_cycle_removed"))
            scene.addOverlayCollection(scene.objectsInactive[camera_name], collection)
        elif frame == 50:
            require_state(True, stored_counts("overlay_cycle_active"))
            owner["overlay_cycle_ok"] = True
            owner["overlay_cycle_done"] = True
    except Exception:
        owner["overlay_cycle_error"] = traceback.format_exc()
        owner["overlay_cycle_ok"] = False
        owner["overlay_cycle_done"] = True
''')
    set_game_property(obj, "overlay_cycle_script", "STRING", controller_text.name)

    # Standalone player acceptance is driven by PLAYER_MAINLOOP_SCRIPT and has no editor operator
    # context in which to add Logic Bricks. It executes this same packed Text once per game frame.
    if RUNNER == "player":
        return

    set_active_object(obj)
    with bpy.context.temp_override(
        object=obj,
        active_object=obj,
        selected_objects=[obj],
        selected_editable_objects=[obj],
        scene=bpy.context.scene,
        view_layer=bpy.context.view_layer,
    ):
        result = bpy.ops.logic.sensor_add(
            type="ALWAYS", name="LNOverlayLifecycleAlways", object=obj.name)
        require(result == {"FINISHED"}, f"Failed to add overlay lifecycle sensor: {result}")
        sensor = obj.game.sensors[-1]
        sensor.use_pulse_true_level = True
        sensor.tick_skip = 0

        result = bpy.ops.logic.controller_add(
            type="PYTHON", name="LNOverlayLifecycleController", object=obj.name)
        require(result == {"FINISHED"}, f"Failed to add overlay lifecycle controller: {result}")
        controller = obj.game.controllers[-1]
        controller.mode = "SCRIPT"
        controller.text = controller_text
        controller.link(sensor=sensor)


def set_expected_vector(obj, value):
    set_game_property(obj, "__ln_acceptance_expected_x", "FLOAT", value[0])
    set_game_property(obj, "__ln_acceptance_expected_y", "FLOAT", value[1])
    set_game_property(obj, "__ln_acceptance_expected_z", "FLOAT", value[2])


def set_expected_named_vector(obj, prefix, value):
    set_game_property(obj, f"__ln_acceptance_{prefix}_x", "FLOAT", value[0])
    set_game_property(obj, f"__ln_acceptance_{prefix}_y", "FLOAT", value[1])
    set_game_property(obj, f"__ln_acceptance_{prefix}_z", "FLOAT", value[2])


def set_expected_color(obj, value):
    set_game_property(obj, "__ln_acceptance_expected_r", "FLOAT", value[0])
    set_game_property(obj, "__ln_acceptance_expected_g", "FLOAT", value[1])
    set_game_property(obj, "__ln_acceptance_expected_b", "FLOAT", value[2])
    set_game_property(obj, "__ln_acceptance_expected_a", "FLOAT", value[3])


def create_camera_looking_at(name, location, target=(0.0, 0.0, 0.0)):
    bpy.ops.object.camera_add(location=location)
    camera = getattr(bpy.context, "object", None) or bpy.context.view_layer.objects.active
    require(camera is not None, f"Failed to create camera {name!r}")
    camera.name = name
    direction = mathutils.Vector(target) - camera.location
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = camera
    return camera


def set_expected_rotation(obj, value):
    set_game_property(obj, "__ln_acceptance_expected_rx", "FLOAT", value[0])
    set_game_property(obj, "__ln_acceptance_expected_ry", "FLOAT", value[1])
    set_game_property(obj, "__ln_acceptance_expected_rz", "FLOAT", value[2])


def new_logic_tree(name):
    tree = bpy.data.node_groups.new(name, "LogicNodeTree")
    tree.use_fake_user = True
    return tree


def set_sequence_output_count(tree, sequence, output_count):
    require(2 <= output_count <= 64, "Sequence output count must be between 2 and 64")
    require(sequence.bl_idname == "LogicNativeSequence", "Expected a Sequence node")

    window = bpy.context.window
    if window is None:
        window_manager = bpy.context.window_manager
        windows = window_manager.windows if window_manager is not None else ()
        window = next(iter(windows), None)
    require(window is not None, "Sequence output editing requires an available Blender window")
    screen = window.screen
    area = screen.areas[0]
    original_area_type = area.type
    try:
        area.type = "NODE_EDITOR"
        space = area.spaces.active
        space.tree_type = "LogicNodeTree"
        space.node_tree = tree
        tree.nodes.active = sequence
        sequence.select = True
        region = next(region for region in area.regions if region.type == "WINDOW")
        with bpy.context.temp_override(window=window, screen=screen, area=area, region=region):
            while len(sequence.outputs) < output_count:
                result = bpy.ops.node.logic_sequence_output_count(delta=1)
                require(result == {"FINISHED"}, "Sequence add-output operator failed")
            while len(sequence.outputs) > output_count:
                result = bpy.ops.node.logic_sequence_output_count(delta=-1)
                require(result == {"FINISHED"}, "Sequence remove-output operator failed")
    finally:
        area.type = original_area_type


def bind_tree(obj, tree_name, enabled=True):
    tree = bpy.data.node_groups.get(tree_name)
    if tree is None:
        set_game_property(obj, LOGIC_NODES_TREE_PROPERTY, "STRING", tree_name)
        set_game_property(obj, LOGIC_NODES_ENABLED_PROPERTY, "BOOL", enabled)
        return

    for binding in obj.game.logic_node_bindings:
        if binding.tree_name == tree_name:
            binding.enabled = enabled
            return

    binding = obj.game.logic_node_binding_new()
    binding.tree = tree
    binding.tree_name = tree_name
    binding.enabled = enabled


def build_move_tree(name, position, event="INIT", target_obj=None):
    tree = new_logic_tree(name)
    event_node = tree.nodes.new("LogicNativeOnInit" if event == "INIT" else "LogicNativeOnUpdate")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")
    if target_obj is not None:
        set_position.inputs["Object"].default_value = target_obj
    set_position.inputs["Value"].default_value = position
    tree.links.new(event_node.outputs["Out"], flow_input(set_position))
    return tree


def build_logic_gate_tree(name, position):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    conditions = [tree.nodes.new("LogicNativeGetGamePropertyBool") for _index in range(5)]
    gate = tree.nodes.new("LogicNativeGate")
    branch = tree.nodes.new("LogicNativeBranch")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")

    for index, condition in enumerate(conditions):
        condition.inputs["Property"].default_value = f"combine_condition_{index}"
    gate.operation = "NAND"
    set_position.inputs["Value"].default_value = position

    tree.links.new(on_fixed_update.outputs["Out"], branch.inputs["Flow"])
    for condition in conditions:
        tree.links.new(condition.outputs["Value"], gate.inputs["Conditions"])
    tree.links.new(gate.outputs["Result"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_position))
    return tree


def build_execution_flow_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    sequence = tree.nodes.new("LogicNativeSequence")
    close_sequence = tree.nodes.new("LogicNativeSequence")
    set_sequence_output_count(tree, sequence, 4)
    any_flow = tree.nodes.new("LogicNativeAnyFlow")
    gate = tree.nodes.new("LogicNativeFlowGate")
    movement = tree.nodes.new("LogicNativeApplyMovement")
    verify_initially_closed = tree.nodes.new("LogicNativeBranch")
    verify_stays_closed = tree.nodes.new("LogicNativeBranch")
    unexpected_initial_movement = tree.nodes.new("LogicNativeApplyMovement")
    unexpected_later_movement = tree.nodes.new("LogicNativeApplyMovement")

    gate.inputs["Start Open"].default_value = False
    movement.inputs["Vector"].default_value = (1.0, 0.0, 0.0)
    movement.inputs["Local"].default_value = False
    unexpected_initial_movement.inputs["Vector"].default_value = (100.0, 0.0, 0.0)
    unexpected_initial_movement.inputs["Local"].default_value = False
    unexpected_later_movement.inputs["Vector"].default_value = (1000.0, 0.0, 0.0)
    unexpected_later_movement.inputs["Local"].default_value = False

    tree.links.new(on_init.outputs["Out"], sequence.inputs["Flow"])
    tree.links.new(sequence.outputs["Then 0"], verify_initially_closed.inputs["Flow"])
    tree.links.new(gate.outputs["Is Open"], verify_initially_closed.inputs["Condition"])
    tree.links.new(
        verify_initially_closed.outputs["True"], flow_input(unexpected_initial_movement)
    )
    tree.links.new(sequence.outputs["Then 1"], gate.inputs["Open"])
    tree.links.new(sequence.outputs["Then 2"], any_flow.inputs["In"])
    tree.links.new(sequence.outputs["Then 3"], close_sequence.inputs["Flow"])
    tree.links.new(close_sequence.outputs["Then 0"], any_flow.inputs["In"])
    tree.links.new(close_sequence.outputs["Then 1"], gate.inputs["Close"])
    tree.links.new(any_flow.outputs["Out"], gate.inputs["Enter"])
    tree.links.new(gate.outputs["Exit"], flow_input(movement))

    # The first branch must sample the initial closed state before the later Open path, even though
    # its condition creates a data dependency on Gate. Both merged pulses must then pass before
    # Close, and the gate must remain closed on later updates.
    tree.links.new(on_fixed_update.outputs["Out"], verify_stays_closed.inputs["Flow"])
    tree.links.new(gate.outputs["Is Open"], verify_stays_closed.inputs["Condition"])
    tree.links.new(verify_stays_closed.outputs["True"], flow_input(unexpected_later_movement))
    return tree


def build_euler_orientation_tree(name, rotation, target_obj=None):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    euler = tree.nodes.new("LogicNativeEuler")
    set_orientation = new_set_object_attribute(tree, "WORLD_ORIENTATION")

    euler.inputs["X"].default_value = rotation[0]
    euler.inputs["Y"].default_value = rotation[1]
    euler.inputs["Z"].default_value = rotation[2]
    if target_obj is not None:
        set_orientation.inputs["Object"].default_value = target_obj

    tree.links.new(on_init.outputs["Out"], flow_input(set_orientation))
    tree.links.new(euler.outputs["Rotation"], set_orientation.inputs["Rotation"])
    return tree


def build_orientation_tree(name):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    get_world_orientation = new_get_object_attribute(tree, "WORLD_ORIENTATION")
    get_local_orientation = new_get_object_attribute(tree, "LOCAL_ORIENTATION")
    set_world_orientation = new_set_object_attribute(tree, "WORLD_ORIENTATION")
    set_local_orientation = new_set_object_attribute(tree, "LOCAL_ORIENTATION")

    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_world_orientation))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_local_orientation))
    tree.links.new(
        get_local_orientation.outputs["Orientation"], set_world_orientation.inputs["Rotation"]
    )
    tree.links.new(
        get_world_orientation.outputs["Orientation"], set_local_orientation.inputs["Rotation"]
    )
    return tree


def build_velocity_tree(name, velocity, target_obj=None):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    set_velocity = new_set_object_attribute(tree, "WORLD_LINEAR_VELOCITY")
    if target_obj is not None:
        set_velocity.inputs["Object"].default_value = target_obj
    set_velocity.inputs["Value"].default_value = velocity
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_velocity))
    return tree


def build_impulse_tree(name, impulse, target_obj=None):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    apply_impulse = tree.nodes.new("LogicNativeApplyImpulse")
    if target_obj is not None:
        apply_impulse.inputs["Object"].default_value = target_obj
    apply_impulse.inputs["Attach"].default_value = (0.0, 0.0, 0.0)
    apply_impulse.inputs["Impulse"].default_value = impulse
    tree.links.new(on_init.outputs["Out"], flow_input(apply_impulse))
    return tree


def build_object_actions_tree(name, movement, rotation, force, torque, color, visible, target_obj=None):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_visibility = new_set_object_attribute(tree, "VISIBLE")
    color_rgba = tree.nodes.new("LogicNativeColorRGBA")
    set_color = new_set_object_attribute(tree, "COLOR")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")
    set_orientation = new_set_object_attribute(tree, "WORLD_ORIENTATION")
    apply_force = tree.nodes.new("LogicNativeApplyForce")
    apply_torque = tree.nodes.new("LogicNativeApplyTorque")

    action_nodes = (
        set_visibility,
        set_color,
        set_position,
        set_orientation,
        apply_force,
        apply_torque,
    )
    if target_obj is not None:
        for node in action_nodes:
            node.inputs["Object"].default_value = target_obj

    set_visibility.inputs["Visible"].default_value = visible
    set_visibility.inputs["Include Children"].default_value = False
    color_rgba.inputs["R"].default_value = color[0]
    color_rgba.inputs["G"].default_value = color[1]
    color_rgba.inputs["B"].default_value = color[2]
    color_rgba.inputs["A"].default_value = color[3]
    set_position.inputs["Value"].default_value = movement
    set_orientation.inputs["Rotation"].default_value = rotation
    apply_force.inputs["Force"].default_value = force
    apply_force.inputs["Local"].default_value = False
    apply_torque.inputs["Torque"].default_value = torque
    apply_torque.inputs["Local"].default_value = False

    tree.links.new(on_init.outputs["Out"], flow_input(set_visibility))
    tree.links.new(on_init.outputs["Out"], flow_input(set_color))
    tree.links.new(color_rgba.outputs["Color"], set_color.inputs["Color"])
    tree.links.new(on_init.outputs["Out"], flow_input(set_position))
    tree.links.new(on_init.outputs["Out"], flow_input(set_orientation))
    tree.links.new(on_init.outputs["Out"], flow_input(apply_force))
    tree.links.new(on_init.outputs["Out"], flow_input(apply_torque))
    return tree


def build_property_branch_tree(name, position):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    get_enabled = tree.nodes.new("LogicNativeGetGamePropertyBool")
    branch = tree.nodes.new("LogicNativeBranch")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")
    set_moved = tree.nodes.new("LogicNativeSetGamePropertyBool")

    get_enabled.inputs["Property"].default_value = "enabled"
    set_position.inputs["Value"].default_value = position
    set_moved.inputs["Property"].default_value = "moved"
    set_moved.inputs["Value"].default_value = True

    tree.links.new(on_fixed_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(get_enabled.outputs["Value"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_position))
    tree.links.new(branch.outputs["True"], flow_input(set_moved))
    return tree


def build_property_writes_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_score = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_state = tree.nodes.new("LogicNativeSetGamePropertyString")

    set_score.inputs["Property"].default_value = "score"
    set_score.inputs["Value"].default_value = 42.5
    set_done.inputs["Property"].default_value = "done"
    set_done.inputs["Value"].default_value = True
    set_state.inputs["Property"].default_value = "state"
    set_state.inputs["Value"].default_value = "running"

    tree.links.new(on_init.outputs["Out"], flow_input(set_score))
    tree.links.new(on_init.outputs["Out"], flow_input(set_done))
    tree.links.new(on_init.outputs["Out"], flow_input(set_state))
    return tree


def build_property_target_tree(name, target):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_score = tree.nodes.new("LogicNativeSetGamePropertyInt")

    set_score.inputs["Object"].default_value = target
    set_score.inputs["Property"].default_value = "score"
    set_score.inputs["Value"].default_value = 42

    tree.links.new(on_init.outputs["Out"], flow_input(set_score))
    return tree


def build_property_advanced_tree(name):
    """On-init writes that mirror toggle/modify/switch/store outcomes (editor tree covers those nodes)."""
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_hit = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_enabled = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_speed = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_throttle = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_choice = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_stored = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    set_hit.inputs["Property"].default_value = "has_hit"
    set_hit.inputs["Value"].default_value = True
    set_enabled.inputs["Property"].default_value = "enabled"
    set_enabled.inputs["Value"].default_value = False
    set_speed.inputs["Property"].default_value = "speed"
    set_speed.inputs["Value"].default_value = 3.0
    set_throttle.inputs["Property"].default_value = "throttle"
    set_throttle.inputs["Value"].default_value = 4.0
    set_choice.inputs["Property"].default_value = "choice"
    set_choice.inputs["Value"].default_value = 7.0
    set_stored.inputs["Property"].default_value = "stored"
    set_stored.inputs["Value"].default_value = 5.0

    for node in (set_hit, set_enabled, set_speed, set_throttle, set_choice, set_stored):
        tree.links.new(on_init.outputs["Out"], flow_input(node))
    return tree


def build_tree_property_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    value_float = tree.nodes.new("LogicNativeValueFloat")
    set_tree_property = tree.nodes.new("LogicNativeSetTreeProperty")
    get_tree_property = tree.nodes.new("LogicNativeGetTreeProperty")
    set_tree_value = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    value_output(value_float).default_value = 3.5
    set_tree_property.inputs["Property"].default_value = "tree_score"
    get_tree_property.inputs["Property"].default_value = "tree_score"
    set_tree_value.inputs["Property"].default_value = "tree_value"

    tree.links.new(on_init.outputs["Out"], flow_input(set_tree_property))
    tree.links.new(value_output(value_float), set_tree_property.inputs["Value"])

    tree.links.new(on_update.outputs["Out"], flow_input(set_tree_value))
    tree.links.new(get_tree_property.outputs["Value"], set_tree_value.inputs["Value"])
    return tree


def build_bone_parent_tree(name, armature_obj, child_obj):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")

    set_parent = tree.nodes.new("LogicNativeSetParent")
    set_parent.parent_type = "BONE"
    parent_inputs = {socket.identifier: socket for socket in set_parent.inputs}
    parent_inputs["Child Object"].default_value = child_obj
    parent_inputs["Parent Object"].default_value = armature_obj
    parent_inputs["Bone Name"].default_value = "Bone"
    tree.links.new(on_init.outputs["Out"], flow_input(set_parent))

    move_bone = tree.nodes.new("LogicNativeSetBonePoseLocation")
    move_bone.space = "POSE_CHANNEL"
    move_bone.inputs["Object"].default_value = armature_obj
    move_bone.inputs["Bone Name"].default_value = "Bone"
    move_bone.inputs["Location"].default_value = (0.5, 0.0, 0.0)
    tree.links.new(on_update.outputs["Out"], flow_input(move_bone))
    return tree


def build_bone_pose_tree(name, armature_obj):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")

    def configure_bone_node(node, bone_name):
        node.inputs["Object"].default_value = armature_obj
        node.inputs["Bone Name"].default_value = bone_name
        return node

    def make_rotation(values):
        node = tree.nodes.new("LogicNativeEuler")
        for axis, value in zip(("X", "Y", "Z"), values):
            node.inputs[axis].default_value = value
        return node.outputs["Rotation"]

    def make_vector(values):
        node = tree.nodes.new("LogicNativeCombineXYZ")
        for axis, value in zip(("X", "Y", "Z"), values):
            node.inputs[axis].default_value = value
        return node.outputs["Vector"]

    def store_components(property_prefix, value_socket, rotation=False):
        separate = tree.nodes.new(
            "LogicNativeSeparateEuler" if rotation else "LogicNativeSeparateXYZ")
        tree.links.new(
            value_socket,
            separate.inputs["Rotation" if rotation else "Vector"],
        )
        for axis in ("X", "Y", "Z"):
            setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
            setter.inputs["Property"].default_value = f"{property_prefix}_{axis.lower()}"
            tree.links.new(on_update.outputs["Out"], flow_input(setter))
            tree.links.new(separate.outputs[axis], setter.inputs["Value"])

    # An untouched bone proves that raw pose-channel rotation is identity while its evaluated
    # native bone orientation is not. The explicit conversion must produce the equivalent
    # ordinary-object orientation even for an authored rest direction and roll.
    get_reference_channel = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseRotation"), "Reference")
    get_reference_channel.space = "POSE_CHANNEL"
    store_components(
        "reference_channel", get_reference_channel.outputs["Rotation"], rotation=True)

    get_reference_native = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseRotation"), "Reference")
    get_reference_native.space = "ARMATURE"
    store_components(
        "reference_native", get_reference_native.outputs["Rotation"], rotation=True)

    get_reference_world = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseRotation"), "Reference")
    get_reference_world.space = "WORLD"
    bone_to_object = tree.nodes.new("LogicNativeBoneRotationToObjectRotation")
    tree.links.new(
        get_reference_world.outputs["Rotation"], bone_to_object.inputs["Bone Rotation"])
    store_components(
        "reference_object", bone_to_object.outputs["Object Rotation"], rotation=True)

    # Pose Channel is a raw Blender channel contract. Get and Set must preserve native bone-axis
    # components, and scale must not silently swap Y/Z.
    set_channel_rotation = configure_bone_node(
        tree.nodes.new("LogicNativeSetBonePoseRotation"), "Bone")
    set_channel_rotation.space = "POSE_CHANNEL"
    tree.links.new(on_update.outputs["Out"], flow_input(set_channel_rotation))
    tree.links.new(
        make_rotation((0.12, -0.21, 0.33)), set_channel_rotation.inputs["Rotation"])

    get_channel_rotation = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseRotation"), "Bone")
    get_channel_rotation.space = "POSE_CHANNEL"
    store_components(
        "channel_rotation", get_channel_rotation.outputs["Rotation"], rotation=True)

    set_channel_scale = configure_bone_node(
        tree.nodes.new("LogicNativeSetBonePoseScale"), "Bone")
    tree.links.new(on_update.outputs["Out"], flow_input(set_channel_scale))
    tree.links.new(make_vector((1.25, 0.75, 1.5)), set_channel_scale.inputs["Scale"])

    get_channel_scale = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseScale"), "Bone")
    store_components("channel_scale", get_channel_scale.outputs["Scale"])

    # Absolute Armature and World modes are tested on bones with non-default rest directions and
    # roll. This exercises Blender's parent/rest pose conversion instead of only the trivial bone.
    set_armature_rotation = configure_bone_node(
        tree.nodes.new("LogicNativeSetBonePoseRotation"), "ArmatureTarget")
    set_armature_rotation.space = "ARMATURE"
    tree.links.new(on_update.outputs["Out"], flow_input(set_armature_rotation))
    tree.links.new(
        make_rotation((0.24, -0.16, 0.41)), set_armature_rotation.inputs["Rotation"])

    get_armature_rotation = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseRotation"), "ArmatureTarget")
    get_armature_rotation.space = "ARMATURE"
    store_components(
        "armature_rotation", get_armature_rotation.outputs["Rotation"], rotation=True)

    set_world_rotation = configure_bone_node(
        tree.nodes.new("LogicNativeSetBonePoseRotation"), "WorldTarget")
    set_world_rotation.space = "WORLD"
    tree.links.new(on_update.outputs["Out"], flow_input(set_world_rotation))
    tree.links.new(
        make_rotation((-0.19, 0.28, -0.34)), set_world_rotation.inputs["Rotation"])

    get_world_rotation = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseRotation"), "WorldTarget")
    get_world_rotation.space = "WORLD"
    store_components(
        "world_rotation", get_world_rotation.outputs["Rotation"], rotation=True)

    # The combined transform node has its own compiler/command path, so verify it independently
    # from the individual rotation and location nodes.
    set_transform = configure_bone_node(
        tree.nodes.new("LogicNativeSetBonePoseTransform"), "TransformBone")
    set_transform.space = "WORLD"
    tree.links.new(on_update.outputs["Out"], flow_input(set_transform))
    tree.links.new(make_vector((2.1, -1.4, 0.8)), set_transform.inputs["Location"])
    tree.links.new(make_rotation((-0.14, 0.22, -0.31)), set_transform.inputs["Rotation"])

    get_transform = configure_bone_node(
        tree.nodes.new("LogicNativeGetBonePoseTransform"), "TransformBone")
    get_transform.space = "WORLD"
    store_components("transform_location", get_transform.outputs["Location"])
    store_components("transform_rotation", get_transform.outputs["Rotation"], rotation=True)
    store_components("transform_scale", get_transform.outputs["Scale"])

    # Bone Local Offset must use evaluated native bone axes. An input on +Y therefore follows the
    # authored bone direction, including the armature object's world rotation.
    set_local_offset = configure_bone_node(
        tree.nodes.new("LogicNativeSetBonePoseLocation"), "OffsetBone")
    set_local_offset.space = "BONE_LOCAL_OFFSET"
    tree.links.new(on_update.outputs["Out"], flow_input(set_local_offset))
    tree.links.new(make_vector((0.0, 0.5, 0.0)), set_local_offset.inputs["Location"])

    get_rest_head = configure_bone_node(
        tree.nodes.new("LogicNativeGetBoneHeadWorld"), "OffsetBone")
    get_pose_head = configure_bone_node(
        tree.nodes.new("LogicNativeGetBoneHeadPoseWorld"), "OffsetBone")
    store_components("offset_rest_head", get_rest_head.outputs["Position"])
    store_components("offset_pose_head", get_pose_head.outputs["Position"])
    return tree


def build_bone_read_tree(name, armature_obj):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    get_bone_length = tree.nodes.new("LogicNativeGetBoneLength")
    get_bone_head = tree.nodes.new("LogicNativeGetBoneHeadWorld")
    get_bone_center = tree.nodes.new("LogicNativeGetBoneCenterWorld")
    separate_head = tree.nodes.new("LogicNativeSeparateXYZ")
    separate_center = tree.nodes.new("LogicNativeSeparateXYZ")
    set_bone_length = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_bone_head_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_bone_center_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    get_bone_length.inputs["Object"].default_value = armature_obj
    get_bone_head.inputs["Object"].default_value = armature_obj
    get_bone_center.inputs["Object"].default_value = armature_obj
    set_bone_length.inputs["Property"].default_value = "bone_length"
    set_bone_head_z.inputs["Property"].default_value = "bone_head_z"
    set_bone_center_z.inputs["Property"].default_value = "bone_center_z"

    tree.links.new(on_init.outputs["Out"], flow_input(set_bone_length))
    tree.links.new(get_bone_length.outputs["Length"], set_bone_length.inputs["Value"])
    tree.links.new(on_init.outputs["Out"], flow_input(set_bone_head_z))
    tree.links.new(get_bone_head.outputs["Position"], separate_head.inputs["Vector"])
    tree.links.new(separate_head.outputs["Z"], set_bone_head_z.inputs["Value"])
    tree.links.new(on_init.outputs["Out"], flow_input(set_bone_center_z))
    tree.links.new(get_bone_center.outputs["Position"], separate_center.inputs["Vector"])
    tree.links.new(separate_center.outputs["Z"], set_bone_center_z.inputs["Value"])
    return tree


def build_data_containers_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")

    item_a = tree.nodes.new("LogicNativeValueFloat")
    item_b = tree.nodes.new("LogicNativeValueFloat")
    item_c = tree.nodes.new("LogicNativeValueFloat")
    make_list = tree.nodes.new("LogicNativeMakeList")
    list_length = tree.nodes.new("LogicNativeListLength")
    set_list_len = tree.nodes.new("LogicNativeSetGamePropertyInt")

    dict_value = tree.nodes.new("LogicNativeValueFloat")
    make_dict = tree.nodes.new("LogicNativeMakeDict")
    dict_length = tree.nodes.new("LogicNativeDictLength")
    dict_get_key = tree.nodes.new("LogicNativeDictGetKey")
    dict_get_keys = tree.nodes.new("LogicNativeDictGetKeys")
    dict_keys_length = tree.nodes.new("LogicNativeListLength")
    set_dict_len = tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_dict_keys_len = tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_dict_answer = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    loop_node = tree.nodes.new("LogicNativeLoop")
    modify_loop = tree.nodes.new("LogicNativeModifyProperty")
    set_loop_done = tree.nodes.new("LogicNativeSetGamePropertyBool")

    empty_list = tree.nodes.new("LogicNativeEmptyList")
    empty_list_length = tree.nodes.new("LogicNativeListLength")
    set_empty_list_len = tree.nodes.new("LogicNativeSetGamePropertyInt")
    empty_dict = tree.nodes.new("LogicNativeEmptyDict")
    empty_dict_length = tree.nodes.new("LogicNativeDictLength")
    set_empty_dict_len = tree.nodes.new("LogicNativeSetGamePropertyInt")

    sum_item_a = tree.nodes.new("LogicNativeValueFloat")
    sum_item_b = tree.nodes.new("LogicNativeValueFloat")
    sum_item_c = tree.nodes.new("LogicNativeValueFloat")
    sum_list = tree.nodes.new("LogicNativeMakeList")
    loop_from_list = tree.nodes.new("LogicNativeLoopFromList")
    modify_sum = tree.nodes.new("LogicNativeModifyProperty")

    list_index = tree.nodes.new("LogicNativeValueInt")
    list_get_item = tree.nodes.new("LogicNativeListGetItem")
    set_list_item_value = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    dict_has_answer = tree.nodes.new("LogicNativeDictHasKey")
    dict_has_missing = tree.nodes.new("LogicNativeDictHasKey")
    branch_has_answer = tree.nodes.new("LogicNativeBranch")
    branch_has_missing = tree.nodes.new("LogicNativeBranch")
    set_has_answer_key = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_has_missing_key = tree.nodes.new("LogicNativeSetGamePropertyBool")

    list_duplicate = tree.nodes.new("LogicNativeListDuplicate")
    list_dup_length = tree.nodes.new("LogicNativeListLength")
    set_list_dup_len = tree.nodes.new("LogicNativeSetGamePropertyInt")

    dict_extra_value = tree.nodes.new("LogicNativeValueFloat")
    make_dict_extra = tree.nodes.new("LogicNativeMakeDict")
    dict_merge = tree.nodes.new("LogicNativeDictMerge")
    merged_dict_length = tree.nodes.new("LogicNativeDictLength")
    set_merged_dict_len = tree.nodes.new("LogicNativeSetGamePropertyInt")

    contains_two_value = tree.nodes.new("LogicNativeValueFloat")
    contains_nine_value = tree.nodes.new("LogicNativeValueFloat")
    list_contains_two = tree.nodes.new("LogicNativeListContains")
    list_contains_nine = tree.nodes.new("LogicNativeListContains")
    branch_list_has_two = tree.nodes.new("LogicNativeBranch")
    branch_list_has_nine = tree.nodes.new("LogicNativeBranch")
    set_list_has_two = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_list_has_nine = tree.nodes.new("LogicNativeSetGamePropertyBool")

    value_output(item_a).default_value = 1.0
    value_output(item_b).default_value = 2.0
    value_output(item_c).default_value = 3.0
    set_list_len.inputs["Property"].default_value = "list_len"

    value_output(dict_value).default_value = 7.5
    make_dict.inputs["Key"].default_value = "answer"
    set_dict_len.inputs["Property"].default_value = "dict_len"
    dict_get_key.inputs["Key"].default_value = "answer"
    set_dict_keys_len.inputs["Property"].default_value = "dict_keys_len"
    set_dict_answer.inputs["Property"].default_value = "dict_answer"

    loop_node.inputs["Count"].default_value = 3
    modify_loop.inputs["Property"].default_value = "loop_count"
    modify_loop.inputs["Value"].default_value = 1.0
    set_loop_done.inputs["Property"].default_value = "containers_ok"
    set_loop_done.inputs["Value"].default_value = True

    empty_list.inputs["Length"].default_value = 0
    set_empty_list_len.inputs["Property"].default_value = "empty_list_len"
    set_empty_dict_len.inputs["Property"].default_value = "empty_dict_len"

    value_output(sum_item_a).default_value = 2.0
    value_output(sum_item_b).default_value = 3.0
    value_output(sum_item_c).default_value = 4.0
    modify_sum.inputs["Property"].default_value = "list_sum"
    value_output(list_index).default_value = 1
    set_list_item_value.inputs["Property"].default_value = "list_item_value"
    dict_has_answer.inputs["Key"].default_value = "answer"
    dict_has_missing.inputs["Key"].default_value = "missing"
    set_has_answer_key.inputs["Property"].default_value = "has_answer_key"
    set_has_answer_key.inputs["Value"].default_value = True
    set_has_missing_key.inputs["Property"].default_value = "has_missing_key"
    set_has_missing_key.inputs["Value"].default_value = True
    set_list_dup_len.inputs["Property"].default_value = "list_dup_len"
    value_output(dict_extra_value).default_value = 2.0
    make_dict_extra.inputs["Key"].default_value = "extra"
    set_merged_dict_len.inputs["Property"].default_value = "merged_dict_len"
    value_output(contains_two_value).default_value = 2.0
    value_output(contains_nine_value).default_value = 9.0
    set_list_has_two.inputs["Property"].default_value = "list_has_two"
    set_list_has_two.inputs["Value"].default_value = True
    set_list_has_nine.inputs["Property"].default_value = "list_has_nine"
    set_list_has_nine.inputs["Value"].default_value = True

    nested_item_a = tree.nodes.new("LogicNativeValueFloat")
    nested_item_b = tree.nodes.new("LogicNativeValueFloat")
    nested_inner_list = tree.nodes.new("LogicNativeMakeList")
    nested_outer_value = tree.nodes.new("LogicNativeValueFloat")
    nested_outer_list = tree.nodes.new("LogicNativeMakeList")
    nested_needle_a = tree.nodes.new("LogicNativeValueFloat")
    nested_needle_b = tree.nodes.new("LogicNativeValueFloat")
    nested_needle_list = tree.nodes.new("LogicNativeMakeList")
    list_contains_nested = tree.nodes.new("LogicNativeListContains")
    branch_list_has_nested = tree.nodes.new("LogicNativeBranch")
    set_list_has_nested = tree.nodes.new("LogicNativeSetGamePropertyBool")

    typed_item_a = tree.nodes.new("LogicNativeValueFloat")
    typed_item_b = tree.nodes.new("LogicNativeValueFloat")
    typed_item_c = tree.nodes.new("LogicNativeValueFloat")
    list_from_items = tree.nodes.new("LogicNativeListFromItems")
    typed_list_length = tree.nodes.new("LogicNativeListLength")
    set_typed_list_len = tree.nodes.new("LogicNativeSetGamePropertyInt")
    typed_loop_from_list = tree.nodes.new("LogicNativeLoopFromList")
    modify_typed_sum = tree.nodes.new("LogicNativeModifyProperty")

    value_output(nested_item_a).default_value = 1.0
    value_output(nested_item_b).default_value = 2.0
    value_output(nested_outer_value).default_value = 3.0
    value_output(nested_needle_a).default_value = 1.0
    value_output(nested_needle_b).default_value = 2.0
    set_list_has_nested.inputs["Property"].default_value = "list_has_nested"
    set_list_has_nested.inputs["Value"].default_value = True

    value_output(typed_item_a).default_value = 4.0
    value_output(typed_item_b).default_value = 5.0
    value_output(typed_item_c).default_value = 6.0
    list_from_items.mode = "FLOAT"
    set_typed_list_len.inputs["Property"].default_value = "typed_list_len"
    modify_typed_sum.inputs["Property"].default_value = "typed_list_sum"

    tree.links.new(on_init.outputs["Out"], branch_list_has_two.inputs["Flow"])
    tree.links.new(make_list.outputs["List"], list_contains_two.inputs["List"])
    tree.links.new(value_output(contains_two_value), list_contains_two.inputs["Value"])
    tree.links.new(list_contains_two.outputs["Contains"], branch_list_has_two.inputs["Condition"])
    tree.links.new(branch_list_has_two.outputs["True"], flow_input(set_list_has_two))

    tree.links.new(on_init.outputs["Out"], branch_list_has_nine.inputs["Flow"])
    tree.links.new(make_list.outputs["List"], list_contains_nine.inputs["List"])
    tree.links.new(value_output(contains_nine_value), list_contains_nine.inputs["Value"])
    tree.links.new(list_contains_nine.outputs["Contains"], branch_list_has_nine.inputs["Condition"])
    tree.links.new(branch_list_has_nine.outputs["True"], flow_input(set_list_has_nine))

    tree.links.new(value_output(nested_item_a), nested_inner_list.inputs["Item A"])
    tree.links.new(value_output(nested_item_b), nested_inner_list.inputs["Item B"])
    tree.links.new(nested_inner_list.outputs["List"], nested_outer_list.inputs["Item A"])
    tree.links.new(value_output(nested_outer_value), nested_outer_list.inputs["Item B"])
    tree.links.new(value_output(nested_needle_a), nested_needle_list.inputs["Item A"])
    tree.links.new(value_output(nested_needle_b), nested_needle_list.inputs["Item B"])
    tree.links.new(on_init.outputs["Out"], branch_list_has_nested.inputs["Flow"])
    tree.links.new(nested_outer_list.outputs["List"], list_contains_nested.inputs["List"])
    tree.links.new(nested_needle_list.outputs["List"], list_contains_nested.inputs["Value"])
    tree.links.new(list_contains_nested.outputs["Contains"], branch_list_has_nested.inputs["Condition"])
    tree.links.new(branch_list_has_nested.outputs["True"], flow_input(set_list_has_nested))

    tree.links.new(value_output(typed_item_a), list_from_items.inputs["Floats"])
    tree.links.new(value_output(typed_item_b), list_from_items.inputs["Floats"])
    tree.links.new(value_output(typed_item_c), list_from_items.inputs["Floats"])
    tree.links.new(on_init.outputs["Out"], flow_input(set_typed_list_len))
    tree.links.new(list_from_items.outputs["Floats"], typed_list_length.inputs["List"])
    tree.links.new(typed_list_length.outputs["Length"], set_typed_list_len.inputs["Value"])
    tree.links.new(on_init.outputs["Out"], flow_input(typed_loop_from_list))
    tree.links.new(list_from_items.outputs["Floats"], typed_loop_from_list.inputs["List"])
    tree.links.new(typed_loop_from_list.outputs["Loop"], modify_typed_sum.inputs["Flow"])
    tree.links.new(typed_loop_from_list.outputs["List Item"], modify_typed_sum.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_list_dup_len))
    tree.links.new(make_list.outputs["List"], list_duplicate.inputs["List"])
    tree.links.new(list_duplicate.outputs["List"], list_dup_length.inputs["List"])
    tree.links.new(list_dup_length.outputs["Length"], set_list_dup_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_merged_dict_len))
    tree.links.new(value_output(dict_extra_value), make_dict_extra.inputs["Value"])
    tree.links.new(make_dict.outputs["Dictionary"], dict_merge.inputs["Dictionary A"])
    tree.links.new(make_dict_extra.outputs["Dictionary"], dict_merge.inputs["Dictionary B"])
    tree.links.new(dict_merge.outputs["Dictionary"], merged_dict_length.inputs["Dictionary"])
    tree.links.new(merged_dict_length.outputs["Length"], set_merged_dict_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_list_item_value))
    tree.links.new(make_list.outputs["List"], list_get_item.inputs["List"])
    tree.links.new(value_output(list_index), list_get_item.inputs["Index"])
    tree.links.new(list_get_item.outputs["Value"], set_list_item_value.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], branch_has_answer.inputs["Flow"])
    tree.links.new(make_dict.outputs["Dictionary"], dict_has_answer.inputs["Dictionary"])
    tree.links.new(dict_has_answer.outputs["Has Key"], branch_has_answer.inputs["Condition"])
    tree.links.new(branch_has_answer.outputs["True"], flow_input(set_has_answer_key))

    tree.links.new(on_init.outputs["Out"], branch_has_missing.inputs["Flow"])
    tree.links.new(make_dict.outputs["Dictionary"], dict_has_missing.inputs["Dictionary"])
    tree.links.new(dict_has_missing.outputs["Has Key"], branch_has_missing.inputs["Condition"])
    tree.links.new(branch_has_missing.outputs["True"], flow_input(set_has_missing_key))

    tree.links.new(on_init.outputs["Out"], flow_input(set_empty_list_len))
    tree.links.new(empty_list.outputs["List"], empty_list_length.inputs["List"])
    tree.links.new(empty_list_length.outputs["Length"], set_empty_list_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_empty_dict_len))
    tree.links.new(empty_dict.outputs["Dictionary"], empty_dict_length.inputs["Dictionary"])
    tree.links.new(empty_dict_length.outputs["Length"], set_empty_dict_len.inputs["Value"])

    tree.links.new(value_output(sum_item_a), sum_list.inputs["Item A"])
    tree.links.new(value_output(sum_item_b), sum_list.inputs["Item B"])
    tree.links.new(value_output(sum_item_c), sum_list.inputs["Item C"])
    tree.links.new(on_init.outputs["Out"], flow_input(loop_from_list))
    tree.links.new(sum_list.outputs["List"], loop_from_list.inputs["List"])
    tree.links.new(loop_from_list.outputs["Loop"], modify_sum.inputs["Flow"])
    tree.links.new(loop_from_list.outputs["List Item"], modify_sum.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_list_len))
    tree.links.new(value_output(item_a), make_list.inputs["Item A"])
    tree.links.new(value_output(item_b), make_list.inputs["Item B"])
    tree.links.new(value_output(item_c), make_list.inputs["Item C"])
    tree.links.new(make_list.outputs["List"], list_length.inputs["List"])
    tree.links.new(list_length.outputs["Length"], set_list_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_dict_len))
    tree.links.new(value_output(dict_value), make_dict.inputs["Value"])
    tree.links.new(make_dict.outputs["Dictionary"], dict_length.inputs["Dictionary"])
    tree.links.new(dict_length.outputs["Length"], set_dict_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_dict_keys_len))
    tree.links.new(make_dict.outputs["Dictionary"], dict_get_keys.inputs["Dictionary"])
    tree.links.new(dict_get_keys.outputs["Keys"], dict_keys_length.inputs["List"])
    tree.links.new(dict_keys_length.outputs["Length"], set_dict_keys_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_dict_answer))
    tree.links.new(make_dict.outputs["Dictionary"], dict_get_key.inputs["Dictionary"])
    tree.links.new(dict_get_key.outputs["Value"], set_dict_answer.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(loop_node))
    tree.links.new(loop_node.outputs["Loop"], modify_loop.inputs["Flow"])
    tree.links.new(loop_node.outputs["Completed"], flow_input(set_loop_done))
    return tree


def build_time_state_tree(name):
    """Drive the time nodes with short second values and record their elapsed outputs."""
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    next_frame = tree.nodes.new("LogicNativeOnNextFrame")
    delay = tree.nodes.new("LogicNativeDelay")
    timer = tree.nodes.new("LogicNativeTimer")
    pulsify = tree.nodes.new("LogicNativePulsify")
    barrier = tree.nodes.new("LogicNativeBarrier")
    time_node = tree.nodes.new("LogicNativeTime")
    set_time = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_delta = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    delay.inputs["Delay"].default_value = 0.05
    timer.inputs["Seconds"].default_value = 0.05
    pulsify.inputs["Gap"].default_value = 0.05
    barrier.inputs["Condition"].default_value = True
    barrier.inputs["Time"].default_value = 0.05
    set_time.inputs["Property"].default_value = "time_value"
    set_delta.inputs["Property"].default_value = "delta_value"

    setters = {}
    for flag_name in ("next_frame", "delay", "timer", "pulsify", "barrier"):
        setter = tree.nodes.new("LogicNativeSetGamePropertyBool")
        setter.inputs["Property"].default_value = flag_name
        setter.inputs["Value"].default_value = True
        setters[flag_name] = setter

    tree.links.new(on_init.outputs["Out"], next_frame.inputs["Flow"])
    tree.links.new(next_frame.outputs["Completed"], flow_input(setters["next_frame"]))
    tree.links.new(on_fixed_update.outputs["Out"], delay.inputs["Flow"])
    tree.links.new(delay.outputs["Done"], flow_input(setters["delay"]))
    tree.links.new(on_init.outputs["Out"], timer.inputs["Set Timer"])
    tree.links.new(timer.outputs["When Elapsed"], flow_input(setters["timer"]))
    tree.links.new(on_fixed_update.outputs["Out"], pulsify.inputs["Flow"])
    tree.links.new(pulsify.outputs["Pulse"], flow_input(setters["pulsify"]))
    tree.links.new(on_fixed_update.outputs["Out"], barrier.inputs["Flow"])
    tree.links.new(barrier.outputs["Out"], flow_input(setters["barrier"]))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_time))
    tree.links.new(time_node.outputs["Time"], set_time.inputs["Value"])
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_delta))
    tree.links.new(time_node.outputs["Delta"], set_delta.inputs["Value"])
    return tree


def build_scene_state_tree(name, gravity, timescale):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    get_gravity = tree.nodes.new("LogicNativeGetGravity")
    separate_gravity = tree.nodes.new("LogicNativeSeparateXYZ")
    set_gravity = tree.nodes.new("LogicNativeSetGravity")
    get_timescale = tree.nodes.new("LogicNativeGetTimescale")
    set_timescale = tree.nodes.new("LogicNativeSetTimescale")
    set_gravity_x = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_gravity_y = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_gravity_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_timescale_property = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    set_gravity.inputs["Gravity"].default_value = gravity
    set_timescale.inputs["Timescale"].default_value = timescale
    for node, name_value in (
        (set_gravity_x, "gravity_x"),
        (set_gravity_y, "gravity_y"),
        (set_gravity_z, "gravity_z"),
        (set_timescale_property, "timescale"),
    ):
        node.inputs["Property"].default_value = name_value

    # The file starts paused: only the frame phase runs, so it must resume the simulation.
    for node in (set_timescale, set_timescale_property):
        tree.links.new(on_update.outputs["Out"], flow_input(node))
    # Set Gravity is fixed-update only; reading it back there also proves fixed steps resumed.
    for node in (set_gravity, set_gravity_x, set_gravity_y, set_gravity_z):
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(node))

    tree.links.new(get_gravity.outputs["Gravity"], separate_gravity.inputs["Vector"])
    tree.links.new(separate_gravity.outputs["X"], set_gravity_x.inputs["Value"])
    tree.links.new(separate_gravity.outputs["Y"], set_gravity_y.inputs["Value"])
    tree.links.new(separate_gravity.outputs["Z"], set_gravity_z.inputs["Value"])
    tree.links.new(get_timescale.outputs["Timescale"], set_timescale_property.inputs["Value"])
    return tree


def build_inactive_overlay_collection_tree(name, camera_obj, collection):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_overlay = tree.nodes.new("LogicNativeSetOverlayCollection")
    set_result = tree.nodes.new("LogicNativeSetGamePropertyString")

    set_overlay.inputs["Camera"].default_value = camera_obj
    set_overlay.inputs["Collection"].default_value = collection
    set_result.inputs["Property"].default_value = "overlay_result"
    set_result.inputs["Value"].default_value = "accepted"

    tree.links.new(on_init.outputs["Out"], flow_input(set_overlay))
    tree.links.new(set_overlay.outputs["Success"], flow_input(set_result))
    return tree


def build_object_menu_session_tree(name, camera_obj, collection, detail_collection):
    """Exercise handles, concurrent sessions, lease-held overlays, and idempotent close.

    One session per collection: the detail menu opens on its own collection. A menu's lease keeps
    its overlay alive through an external removal, so the tree then closes that menu itself.
    """
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    open_first = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_second = tree.nodes.new("LogicNativeOpenObjectMenu")
    close_first = tree.nodes.new("LogicNativeCloseObjectMenu")
    close_second = tree.nodes.new("LogicNativeCloseObjectMenu")
    close_repeat = tree.nodes.new("LogicNativeCloseObjectMenu")
    open_external = tree.nodes.new("LogicNativeOpenObjectMenu")
    set_repeat_closed = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_external_ready = tree.nodes.new("LogicNativeSetGamePropertyBool")
    get_external_removed = tree.nodes.new("LogicNativeGetGamePropertyBool")
    external_removed_branch = tree.nodes.new("LogicNativeBranch")
    close_external_once = tree.nodes.new("LogicNativeOnce")
    get_external_handle = tree.nodes.new("LogicNativeGetGamePropertyInt")
    close_external = tree.nodes.new("LogicNativeCloseObjectMenu")
    set_session_done = tree.nodes.new("LogicNativeSetGamePropertyBool")

    for node, context, menu_collection in (
        (open_first, "Inventory", collection),
        (open_second, "Inventory.Detail", detail_collection),
        (open_external, "Inventory.ExternalRemoval", collection),
    ):
        node.inputs["Camera"].default_value = camera_obj
        node.inputs["Collection"].default_value = menu_collection
        node.inputs["Input Context"].default_value = context
    close_first.menu_close_operation = "SPECIFIC"
    close_second.menu_close_operation = "SPECIFIC"
    close_repeat.menu_close_operation = "SPECIFIC"
    close_external.menu_close_operation = "SPECIFIC"
    get_external_removed.inputs["Property"].default_value = "menu_external_removed"
    get_external_handle.inputs["Property"].default_value = "menu_handle_external"
    set_session_done.inputs["Property"].default_value = "menu_session_done"
    set_session_done.inputs["Value"].default_value = True
    set_repeat_closed.inputs["Property"].default_value = "menu_repeat_closed"
    set_repeat_closed.inputs["Value"].default_value = True
    set_external_ready.inputs["Property"].default_value = "menu_external_ready"
    set_external_ready.inputs["Value"].default_value = True

    def add_int_step(property_name, value_socket):
        setter = tree.nodes.new("LogicNativeSetGamePropertyInt")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(value_socket, setter.inputs["Value"])
        return setter

    first_handle = add_int_step("menu_handle_first", open_first.outputs["Menu"])
    first_open_result = add_int_step(
        "menu_open_first_result", open_first.outputs["Result"])
    second_handle = add_int_step("menu_handle_second", open_second.outputs["Menu"])
    second_open_result = add_int_step(
        "menu_open_second_result", open_second.outputs["Result"])
    first_close_result = add_int_step(
        "menu_close_first_result", close_first.outputs["Result"])
    second_close_result = add_int_step(
        "menu_close_second_result", close_second.outputs["Result"])
    repeat_close_result = add_int_step(
        "menu_close_repeat_result", close_repeat.outputs["Result"])
    external_handle = add_int_step(
        "menu_handle_external", open_external.outputs["Menu"])
    external_open_result = add_int_step(
        "menu_open_external_result", open_external.outputs["Result"])
    external_close_result = add_int_step(
        "menu_close_external_result", close_external.outputs["Result"])
    failure_nodes = []
    for _operation in range(7):
        failure = tree.nodes.new("LogicNativeSetGamePropertyBool")
        failure.inputs["Property"].default_value = "menu_session_failed"
        failure.inputs["Value"].default_value = True
        failure_nodes.append(failure)

    tree.links.new(on_init.outputs["Out"], flow_input(open_first))
    tree.links.new(open_first.outputs["Opened"], flow_input(first_handle))
    tree.links.new(first_handle.outputs["Done"], flow_input(first_open_result))
    tree.links.new(first_open_result.outputs["Done"], flow_input(open_second))
    tree.links.new(open_second.outputs["Opened"], flow_input(second_handle))
    tree.links.new(second_handle.outputs["Done"], flow_input(second_open_result))
    tree.links.new(second_open_result.outputs["Done"], flow_input(close_first))
    tree.links.new(open_first.outputs["Menu"], close_first.inputs["Menu"])
    tree.links.new(close_first.outputs["Closed"], flow_input(first_close_result))
    tree.links.new(first_close_result.outputs["Done"], flow_input(close_second))
    tree.links.new(open_second.outputs["Menu"], close_second.inputs["Menu"])
    tree.links.new(close_second.outputs["Closed"], flow_input(second_close_result))
    tree.links.new(second_close_result.outputs["Done"], flow_input(close_repeat))
    tree.links.new(open_second.outputs["Menu"], close_repeat.inputs["Menu"])
    tree.links.new(close_repeat.outputs["Closed"], flow_input(repeat_close_result))
    tree.links.new(repeat_close_result.outputs["Done"], flow_input(set_repeat_closed))
    tree.links.new(set_repeat_closed.outputs["Done"], flow_input(open_external))
    tree.links.new(open_external.outputs["Opened"], flow_input(external_handle))
    tree.links.new(external_handle.outputs["Done"], flow_input(external_open_result))
    tree.links.new(external_open_result.outputs["Done"], flow_input(set_external_ready))
    # After the Python status controller removes the collection, the session still holds it.
    tree.links.new(on_update.outputs["Out"], external_removed_branch.inputs["Flow"])
    tree.links.new(get_external_removed.outputs["Value"], external_removed_branch.inputs["Condition"])
    tree.links.new(external_removed_branch.outputs["True"], close_external_once.inputs["Flow"])
    tree.links.new(close_external_once.outputs["Out"], flow_input(close_external))
    tree.links.new(get_external_handle.outputs["Value"], close_external.inputs["Menu"])
    tree.links.new(close_external.outputs["Closed"], flow_input(external_close_result))
    tree.links.new(external_close_result.outputs["Done"], flow_input(set_session_done))
    for operation, failure in zip(
        (open_first,
         open_second,
         close_first,
         close_second,
         close_repeat,
         open_external,
         close_external),
        failure_nodes,
    ):
        tree.links.new(operation.outputs["Failed"], flow_input(failure))
    return tree


def build_camera_state_tree(name, camera_obj, point, depth, fov, ortho_scale):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    get_active_camera = tree.nodes.new("LogicNativeGetActiveCamera")
    set_camera = tree.nodes.new("LogicNativeSetCamera")
    set_camera_fov = tree.nodes.new("LogicNativeSetCameraFov")
    set_camera_ortho_scale = tree.nodes.new("LogicNativeSetCameraOrthoScale")
    world_to_screen = tree.nodes.new("LogicNativeWorldToScreen")
    separate_screen = tree.nodes.new("LogicNativeSeparateXY")
    screen_to_world = tree.nodes.new("LogicNativeScreenToWorld")
    separate_world = tree.nodes.new("LogicNativeSeparateXYZ")
    set_active_camera_present = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_screen_x = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_screen_y = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_world_x = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_world_y = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_world_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    set_camera.inputs["Camera"].default_value = camera_obj
    set_camera_fov.inputs["Camera"].default_value = camera_obj
    set_camera_fov.inputs["FOV"].default_value = fov
    set_camera_ortho_scale.inputs["Camera"].default_value = camera_obj
    set_camera_ortho_scale.inputs["Scale"].default_value = ortho_scale
    world_to_screen.inputs["Point"].default_value = point
    screen_to_world.inputs["Depth"].default_value = depth

    for node, name_value in (
        (set_active_camera_present, "active_camera_present"),
        (set_screen_x, "screen_x"),
        (set_screen_y, "screen_y"),
        (set_world_x, "world_x"),
        (set_world_y, "world_y"),
        (set_world_z, "world_z"),
    ):
        node.inputs["Property"].default_value = name_value

    set_active_camera_present.inputs["Value"].default_value = True

    for node in (set_camera, set_camera_fov, set_camera_ortho_scale):
        tree.links.new(on_init.outputs["Out"], flow_input(node))

    for node in (set_screen_x, set_screen_y, set_world_x, set_world_y, set_world_z):
        tree.links.new(on_update.outputs["Out"], flow_input(node))
    tree.links.new(on_update.outputs["Out"], flow_input(set_active_camera_present))

    tree.links.new(get_active_camera.outputs["Camera"], world_to_screen.inputs["Camera"])
    tree.links.new(get_active_camera.outputs["Camera"], screen_to_world.inputs["Camera"])
    tree.links.new(world_to_screen.outputs["On Screen"], separate_screen.inputs["Vector"])
    tree.links.new(separate_screen.outputs["X"], screen_to_world.inputs["Screen X"])
    tree.links.new(separate_screen.outputs["X"], set_screen_x.inputs["Value"])
    tree.links.new(separate_screen.outputs["Y"], screen_to_world.inputs["Screen Y"])
    tree.links.new(separate_screen.outputs["Y"], set_screen_y.inputs["Value"])
    tree.links.new(screen_to_world.outputs["World Position"], separate_world.inputs["Vector"])
    tree.links.new(separate_world.outputs["X"], set_world_x.inputs["Value"])
    tree.links.new(separate_world.outputs["Y"], set_world_y.inputs["Value"])
    tree.links.new(separate_world.outputs["Z"], set_world_z.inputs["Value"])
    return tree


def build_render_state_tree(name):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    get_fullscreen = tree.nodes.new("LogicNativeGetFullscreen")
    set_fullscreen = tree.nodes.new("LogicNativeSetFullscreen")
    get_resolution = tree.nodes.new("LogicNativeGetResolution")
    set_resolution = tree.nodes.new("LogicNativeSetResolution")
    get_vsync = tree.nodes.new("LogicNativeGetVSync")
    set_vsync = tree.nodes.new("LogicNativeSetVSync")
    show_framerate = tree.nodes.new("LogicNativeShowFramerate")
    show_profile = tree.nodes.new("LogicNativeShowProfile")
    set_fullscreen_property = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_width_property = tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_height_property = tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_vsync_property = tree.nodes.new("LogicNativeSetGamePropertyInt")

    set_vsync.vsync_mode = "OFF"
    show_framerate.inputs["Show"].default_value = False
    show_profile.inputs["Show"].default_value = False

    for node, name_value in (
        (set_fullscreen_property, "fullscreen_state"),
        (set_width_property, "resolution_width"),
        (set_height_property, "resolution_height"),
        (set_vsync_property, "vsync_mode"),
    ):
        node.inputs["Property"].default_value = name_value

    for node in (
        set_fullscreen,
        set_resolution,
        set_vsync,
        show_framerate,
        show_profile,
        set_fullscreen_property,
        set_width_property,
        set_height_property,
        set_vsync_property,
    ):
        tree.links.new(on_update.outputs["Out"], node.inputs["Flow"])

    tree.links.new(get_fullscreen.outputs["Fullscreen"], set_fullscreen.inputs["Fullscreen"])
    tree.links.new(get_fullscreen.outputs["Fullscreen"], set_fullscreen_property.inputs["Value"])
    tree.links.new(get_resolution.outputs["Width"], set_resolution.inputs["X"])
    tree.links.new(get_resolution.outputs["Width"], set_width_property.inputs["Value"])
    tree.links.new(get_resolution.outputs["Height"], set_resolution.inputs["Y"])
    tree.links.new(get_resolution.outputs["Height"], set_height_property.inputs["Value"])
    tree.links.new(get_vsync.outputs["Mode"], set_vsync_property.inputs["Value"])
    return tree


def build_light_state_tree(name, source_light, mirror_light, color, power, use_shadow):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    set_source_color = tree.nodes.new("LogicNativeSetLightColor")
    set_source_power = tree.nodes.new("LogicNativeSetLightPower")
    set_source_shadow = tree.nodes.new("LogicNativeSetLightShadow")
    get_source_color = tree.nodes.new("LogicNativeGetLightColor")
    get_source_power = tree.nodes.new("LogicNativeGetLightPower")
    set_mirror_color = tree.nodes.new("LogicNativeSetLightColor")
    set_mirror_power = tree.nodes.new("LogicNativeSetLightPower")

    set_source_color.inputs["Object"].default_value = source_light
    set_source_color.inputs["Color"].default_value = color
    set_source_power.inputs["Object"].default_value = source_light
    set_source_power.inputs["Power"].default_value = power
    set_source_shadow.inputs["Object"].default_value = source_light
    set_source_shadow.inputs["Use Shadow"].default_value = use_shadow
    get_source_color.inputs["Object"].default_value = source_light
    get_source_power.inputs["Object"].default_value = source_light
    set_mirror_color.inputs["Object"].default_value = mirror_light
    set_mirror_power.inputs["Object"].default_value = mirror_light

    for node in (set_source_color, set_source_power, set_source_shadow):
        tree.links.new(on_init.outputs["Out"], flow_input(node))

    for node in (set_mirror_color, set_mirror_power):
        tree.links.new(on_update.outputs["Out"], flow_input(node))

    tree.links.new(get_source_color.outputs["Color"], set_mirror_color.inputs["Color"])
    tree.links.new(get_source_power.outputs["Power"], set_mirror_power.inputs["Power"])
    return tree


def build_light_unique_tree(name, source_light, color, power, use_shadow):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    make_unique = tree.nodes.new("LogicNativeMakeLightUnique")
    set_source_color = tree.nodes.new("LogicNativeSetLightColor")
    set_source_power = tree.nodes.new("LogicNativeSetLightPower")
    set_source_shadow = tree.nodes.new("LogicNativeSetLightShadow")

    make_unique.inputs["Object"].default_value = source_light
    set_source_color.inputs["Object"].default_value = source_light
    set_source_color.inputs["Color"].default_value = color
    set_source_power.inputs["Object"].default_value = source_light
    set_source_power.inputs["Power"].default_value = power
    set_source_shadow.inputs["Object"].default_value = source_light
    set_source_shadow.inputs["Use Shadow"].default_value = use_shadow

    tree.links.new(on_init.outputs["Out"], flow_input(make_unique))
    tree.links.new(make_unique.outputs["Out"], flow_input(set_source_color))
    tree.links.new(set_source_color.outputs["Out"], flow_input(set_source_power))
    tree.links.new(set_source_power.outputs["Out"], flow_input(set_source_shadow))
    return tree


def build_physics_state_tree(name, target_obj, collision_group):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    get_group = tree.nodes.new("LogicNativeGetCollisionGroup")
    set_group = tree.nodes.new("LogicNativeSetCollisionGroup")
    set_physics = tree.nodes.new("LogicNativeSetPhysics")
    set_dynamics = tree.nodes.new("LogicNativeSetDynamics")
    set_group_property = tree.nodes.new("LogicNativeSetGamePropertyInt")

    get_group.inputs["Object"].default_value = target_obj
    set_group.inputs["Object"].default_value = target_obj
    set_physics.inputs["Object"].default_value = target_obj
    set_dynamics.inputs["Object"].default_value = target_obj
    set_group.inputs["Layers"].default_value = collision_group
    set_physics.inputs["Active"].default_value = True
    set_dynamics.dynamics_mode = "NO_COLLISION"
    set_group_property.inputs["Property"].default_value = "collision_group"

    for node in (
        set_group,
        set_physics,
        set_dynamics,
        set_group_property,
    ):
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(node))

    tree.links.new(get_group.outputs["Layers"], set_group_property.inputs["Value"])
    return tree


def build_character_state_tree(name, target_obj, gravity, walk_direction, velocity, velocity_time,
                               max_jumps, jump_speed):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    once = tree.nodes.new("LogicNativeOnce")
    next_fixed_tick = tree.nodes.new("LogicNativeTimer")
    get_character = tree.nodes.new("LogicNativeGetCharacterInfo")
    character_jump = tree.nodes.new("LogicNativeCharacterJump")
    set_gravity = tree.nodes.new("LogicNativeSetCharacterGravity")
    set_jump_speed = tree.nodes.new("LogicNativeSetCharacterJumpSpeed")
    set_max_jumps = tree.nodes.new("LogicNativeSetCharacterMaxJumps")
    set_walk_direction = tree.nodes.new("LogicNativeSetCharacterWalkDirection")
    set_velocity = tree.nodes.new("LogicNativeSetCharacterVelocity")
    separate_gravity = tree.nodes.new("LogicNativeSeparateXYZ")
    separate_walk = tree.nodes.new("LogicNativeSeparateXYZ")
    set_gravity_x = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_gravity_y = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_gravity_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_walk_x = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_walk_y = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_walk_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_max_jumps_property = tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_jump_count_property = tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_on_ground_property = tree.nodes.new("LogicNativeSetGamePropertyBool")

    for node in (
        get_character,
        character_jump,
        set_gravity,
        set_jump_speed,
        set_max_jumps,
        set_walk_direction,
        set_velocity,
    ):
        node.inputs["Object"].default_value = target_obj

    set_gravity.inputs["Gravity"].default_value = gravity
    set_jump_speed.inputs["Force"].default_value = jump_speed
    set_max_jumps.inputs["Max Jumps"].default_value = max_jumps
    get_character.use_local_space = False
    set_walk_direction.use_local_space = False
    set_walk_direction.inputs["Vector"].default_value = walk_direction
    set_velocity.use_local_space = False
    set_velocity.inputs["Velocity"].default_value = velocity
    set_velocity.inputs["Time"].default_value = velocity_time
    # A tiny positive timer advances on the next fixed step. Wait for Next Frame is intentionally a
    # render-frame event and therefore must not drive a fixed-only character mutation.
    next_fixed_tick.inputs["Seconds"].default_value = 1.0e-6

    set_gravity_x.inputs["Property"].default_value = "character_gravity_x"
    set_gravity_y.inputs["Property"].default_value = "character_gravity_y"
    set_gravity_z.inputs["Property"].default_value = "character_gravity_z"
    set_walk_x.inputs["Property"].default_value = "character_walk_x"
    set_walk_y.inputs["Property"].default_value = "character_walk_y"
    set_walk_z.inputs["Property"].default_value = "character_walk_z"
    set_max_jumps_property.inputs["Property"].default_value = "character_max_jumps"
    set_jump_count_property.inputs["Property"].default_value = "character_jump_count"
    set_on_ground_property.inputs["Property"].default_value = "character_on_ground"

    tree.links.new(on_fixed_update.outputs["Out"], once.inputs["Flow"])
    tree.links.new(once.outputs["Out"], next_fixed_tick.inputs["Set Timer"])
    tree.links.new(next_fixed_tick.outputs["When Elapsed"], flow_input(character_jump))

    for node in (
        set_gravity,
        set_jump_speed,
        set_max_jumps,
        set_walk_direction,
        set_velocity,
    ):
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(node))

    for node in (
        set_gravity_x,
        set_gravity_y,
        set_gravity_z,
        set_walk_x,
        set_walk_y,
        set_walk_z,
        set_max_jumps_property,
        set_jump_count_property,
        set_on_ground_property,
    ):
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(node))

    tree.links.new(get_character.outputs["Gravity"], separate_gravity.inputs["Vector"])
    tree.links.new(get_character.outputs["Walk Direction"], separate_walk.inputs["Vector"])
    tree.links.new(separate_gravity.outputs["X"], set_gravity_x.inputs["Value"])
    tree.links.new(separate_gravity.outputs["Y"], set_gravity_y.inputs["Value"])
    tree.links.new(separate_gravity.outputs["Z"], set_gravity_z.inputs["Value"])
    tree.links.new(separate_walk.outputs["X"], set_walk_x.inputs["Value"])
    tree.links.new(separate_walk.outputs["Y"], set_walk_y.inputs["Value"])
    tree.links.new(separate_walk.outputs["Z"], set_walk_z.inputs["Value"])
    tree.links.new(get_character.outputs["Max Jumps"], set_max_jumps_property.inputs["Value"])
    tree.links.new(get_character.outputs["Current Jump Count"], set_jump_count_property.inputs["Value"])
    tree.links.new(get_character.outputs["On Ground"], set_on_ground_property.inputs["Value"])
    return tree


def build_object_lifecycle_tree(name, template_obj, replica_transform_obj, full_copy_transform_obj,
                                remove_parent_target):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    add_object = tree.nodes.new("LogicNativeAddObject")
    add_object_full = tree.nodes.new("LogicNativeAddObject")
    remove_parent = tree.nodes.new("LogicNativeRemoveParent")

    add_object.inputs["Object to Add"].default_value = template_obj
    add_object.inputs["Copy Transform"].default_value = replica_transform_obj
    add_object.inputs["Life"].default_value = 0.0
    add_object.inputs["Full Copy"].default_value = False

    add_object_full.inputs["Object to Add"].default_value = template_obj
    add_object_full.inputs["Copy Transform"].default_value = full_copy_transform_obj
    add_object_full.inputs["Life"].default_value = 0.0
    add_object_full.inputs["Full Copy"].default_value = True

    remove_parent.inputs["Child Object"].default_value = remove_parent_target

    tree.links.new(on_init.outputs["Out"], add_object.inputs["Flow"])
    tree.links.new(on_init.outputs["Out"], add_object_full.inputs["Flow"])
    tree.links.new(on_init.outputs["Out"], flow_input(remove_parent))
    return tree


def build_add_object_single_tree(name, template_obj, transform_obj, full_copy=False, life=0.0):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    add_object = tree.nodes.new("LogicNativeAddObject")
    add_object.inputs["Object to Add"].default_value = template_obj
    add_object.inputs["Copy Transform"].default_value = transform_obj
    add_object.inputs["Life"].default_value = life
    add_object.inputs["Full Copy"].default_value = full_copy
    tree.links.new(on_init.outputs["Out"], add_object.inputs["Flow"])
    return tree


def build_add_object_update_tree(name, template_obj, transform_obj, full_copy=False):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    add_object = tree.nodes.new("LogicNativeAddObject")
    add_object.inputs["Object to Add"].default_value = template_obj
    add_object.inputs["Copy Transform"].default_value = transform_obj
    add_object.inputs["Life"].default_value = 0.0
    add_object.inputs["Full Copy"].default_value = full_copy
    tree.links.new(on_fixed_update.outputs["Out"], add_object.inputs["Flow"])
    return tree


def build_add_object_result_move_tree(name, template_obj, transform_obj, final_position):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    add_object = tree.nodes.new("LogicNativeAddObject")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")
    add_object.inputs["Object to Add"].default_value = template_obj
    add_object.inputs["Copy Transform"].default_value = transform_obj
    add_object.inputs["Life"].default_value = 0.0
    add_object.inputs["Full Copy"].default_value = False
    set_position.inputs["Value"].default_value = final_position
    tree.links.new(on_init.outputs["Out"], add_object.inputs["Flow"])
    tree.links.new(add_object.outputs["Done"], flow_input(set_position))
    tree.links.new(add_object.outputs["Added Object"], set_position.inputs["Object"])
    return tree


def build_add_object_get_id_tree(name, template_obj, transform_obj):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    add_object_a = tree.nodes.new("LogicNativeAddObject")
    add_object_b = tree.nodes.new("LogicNativeAddObject")
    get_id_a = tree.nodes.new("LogicNativeGetObjectID")
    get_id_b = tree.nodes.new("LogicNativeGetObjectID")
    set_id_a = tree.nodes.new("LogicNativeSetGamePropertyString")
    set_id_b = tree.nodes.new("LogicNativeSetGamePropertyString")

    for add_object in (add_object_a, add_object_b):
        add_object.inputs["Object to Add"].default_value = template_obj
        add_object.inputs["Copy Transform"].default_value = transform_obj
        add_object.inputs["Life"].default_value = 0.0
        add_object.inputs["Full Copy"].default_value = False

    set_id_a.inputs["Property"].default_value = "added_object_id_a"
    set_id_b.inputs["Property"].default_value = "added_object_id_b"

    tree.links.new(on_init.outputs["Out"], add_object_a.inputs["Flow"])
    tree.links.new(add_object_a.outputs["Done"], flow_input(set_id_a))
    tree.links.new(add_object_a.outputs["Added Object"], get_id_a.inputs["Object"])
    tree.links.new(get_id_a.outputs["ID"], set_id_a.inputs["Value"])

    tree.links.new(set_id_a.outputs["Done"], add_object_b.inputs["Flow"])
    tree.links.new(add_object_b.outputs["Done"], flow_input(set_id_b))
    tree.links.new(add_object_b.outputs["Added Object"], get_id_b.inputs["Object"])
    tree.links.new(get_id_b.outputs["ID"], set_id_b.inputs["Value"])

    return tree


def build_add_object_after_move_tree(name, template_obj, transform_obj, final_position):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")
    add_object = tree.nodes.new("LogicNativeAddObject")
    set_position.inputs["Object"].default_value = transform_obj
    set_position.inputs["Value"].default_value = final_position
    add_object.inputs["Object to Add"].default_value = template_obj
    add_object.inputs["Copy Transform"].default_value = transform_obj
    add_object.inputs["Life"].default_value = 0.0
    add_object.inputs["Full Copy"].default_value = False
    tree.links.new(on_init.outputs["Out"], flow_input(set_position))
    tree.links.new(set_position.outputs["Done"], add_object.inputs["Flow"])
    return tree


def build_spawned_replica_init_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_property = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_property.inputs["Property"].default_value = "spawn_logic_ran"
    set_property.inputs["Value"].default_value = True
    tree.links.new(on_init.outputs["Out"], flow_input(set_property))
    return tree


def build_set_parent_tree(name, child_obj, parent_obj, compound=False):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_parent = tree.nodes.new("LogicNativeSetParent")
    set_parent.parent_type = "OBJECT"
    set_parent.inputs["Child Object"].default_value = child_obj
    set_parent.inputs["Parent Object"].default_value = parent_obj
    set_parent.inputs["Compound"].default_value = compound
    tree.links.new(on_init.outputs["Out"], flow_input(set_parent))
    return tree


def build_remove_object_tree(name, target_obj):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    remove_object = tree.nodes.new("LogicNativeRemoveObject")
    remove_object.inputs["Object"].default_value = target_obj
    tree.links.new(on_init.outputs["Out"], flow_input(remove_object))
    return tree


def build_object_query_tree(name, child_obj, parent_obj):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    get_parent = tree.nodes.new("LogicNativeGetParent")
    get_child = tree.nodes.new("LogicNativeGetChild")
    get_missing_child = tree.nodes.new("LogicNativeGetChild")
    has_parent = tree.nodes.new("LogicNativeNotNone")
    has_child = tree.nodes.new("LogicNativeNotNone")
    missing_child_is_none = tree.nodes.new("LogicNativeNone")
    has_parent_branch = tree.nodes.new("LogicNativeBranch")
    has_child_branch = tree.nodes.new("LogicNativeBranch")
    missing_child_branch = tree.nodes.new("LogicNativeBranch")
    set_has_parent = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_has_child = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_missing_child = tree.nodes.new("LogicNativeSetGamePropertyBool")

    get_parent.inputs["Child Object"].default_value = child_obj
    get_child.inputs["Parent Object"].default_value = parent_obj
    get_child.inputs["Index"].default_value = 0
    get_missing_child.inputs["Parent Object"].default_value = parent_obj
    get_missing_child.inputs["Index"].default_value = 4

    for node, name_value in (
        (set_has_parent, "has_parent"),
        (set_has_child, "has_child"),
        (set_missing_child, "missing_child_is_none"),
    ):
        node.inputs["Property"].default_value = name_value
        node.inputs["Value"].default_value = True

    tree.links.new(get_parent.outputs["Parent Object"], has_parent.inputs["Value"])
    tree.links.new(get_child.outputs["Child Object"], has_child.inputs["Value"])
    tree.links.new(get_missing_child.outputs["Child Object"], missing_child_is_none.inputs["Value"])
    for branch in (has_parent_branch, has_child_branch, missing_child_branch):
        tree.links.new(on_update.outputs["Out"], flow_input(branch))
    tree.links.new(has_parent.outputs["If Not None"], has_parent_branch.inputs["Condition"])
    tree.links.new(has_child.outputs["If Not None"], has_child_branch.inputs["Condition"])
    tree.links.new(missing_child_is_none.outputs["If None"], missing_child_branch.inputs["Condition"])
    tree.links.new(has_parent_branch.outputs["True"], flow_input(set_has_parent))
    tree.links.new(has_child_branch.outputs["True"], flow_input(set_has_child))
    tree.links.new(missing_child_branch.outputs["True"], flow_input(set_missing_child))
    return tree


def build_average_center_position_tree(name, collection, empty_collection):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")

    vector_a = tree.nodes.new("LogicNativeValueVector")
    vector_b = tree.nodes.new("LogicNativeValueVector")
    vector_a.outputs["Vector"].default_value = (10.0, 20.0, 30.0)
    vector_b.outputs["Vector"].default_value = (-2.0, 4.0, 6.0)
    vector_list = tree.nodes.new("LogicNativeMakeList")
    tree.links.new(vector_a.outputs["Vector"], vector_list.inputs["Item A"])
    tree.links.new(vector_b.outputs["Vector"], vector_list.inputs["Item B"])
    empty_list = tree.nodes.new("LogicNativeEmptyList")

    collection_objects = tree.nodes.new("LogicNativeGetCollectionObjects")
    collection_objects.inputs["Collection"].default_value = collection

    sources = (
        ("vector_center", "VECTOR_LIST", vector_list.outputs["List"], "Vectors", None),
        ("object_center", "OBJECT_LIST", collection_objects.outputs["Objects"], "Objects", None),
        ("collection_center", "COLLECTION", None, "Collection", collection),
        ("empty_vector_center", "VECTOR_LIST", empty_list.outputs["List"], "Vectors", None),
        ("empty_object_center", "OBJECT_LIST", empty_list.outputs["List"], "Objects", None),
        ("empty_collection_center", "COLLECTION", None, "Collection", empty_collection),
    )
    for prefix, source_type, source_socket, input_name, default_value in sources:
        average = tree.nodes.new("LogicNativeGetAverageCenterPosition")
        average.source_type = source_type
        if source_socket is None:
            average.inputs[input_name].default_value = default_value
        else:
            tree.links.new(source_socket, average.inputs[input_name])

        separate = tree.nodes.new("LogicNativeSeparateXYZ")
        tree.links.new(average.outputs["Center"], separate.inputs["Vector"])
        for component in ("X", "Y", "Z"):
            setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
            setter.inputs["Property"].default_value = f"{prefix}_{component.lower()}"
            tree.links.new(on_update.outputs["Out"], flow_input(setter))
            tree.links.new(separate.outputs[component], setter.inputs["Value"])

        count_setter = tree.nodes.new("LogicNativeSetGamePropertyInt")
        count_setter.inputs["Property"].default_value = f"{prefix}_count"
        tree.links.new(on_update.outputs["Out"], flow_input(count_setter))
        tree.links.new(average.outputs["Count"], count_setter.inputs["Value"])

        valid_setter = tree.nodes.new("LogicNativeSetGamePropertyBool")
        valid_setter.inputs["Property"].default_value = f"{prefix}_valid"
        tree.links.new(on_update.outputs["Out"], flow_input(valid_setter))
        tree.links.new(average.outputs["Valid"], valid_setter.inputs["Value"])
    return tree


def build_tier_a_tree(name, target_name, mover):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")

    empty_list = tree.nodes.new("LogicNativeEmptyList")
    append_one = tree.nodes.new("LogicNativeValueFloat")
    list_append_a = tree.nodes.new("LogicNativeListAppend")
    append_two = tree.nodes.new("LogicNativeValueFloat")
    list_append_b = tree.nodes.new("LogicNativeListAppend")
    list_length = tree.nodes.new("LogicNativeListLength")
    set_list_len = tree.nodes.new("LogicNativeSetGamePropertyInt")

    empty_dict = tree.nodes.new("LogicNativeEmptyDict")
    dict_value = tree.nodes.new("LogicNativeValueFloat")
    dict_set = tree.nodes.new("LogicNativeDictSetKey")
    dict_length = tree.nodes.new("LogicNativeDictLength")
    set_dict_len = tree.nodes.new("LogicNativeSetGamePropertyInt")

    set_global = tree.nodes.new("LogicNativeSetGlobalProperty")
    global_value = tree.nodes.new("LogicNativeValueFloat")
    get_global = tree.nodes.new("LogicNativeGetGlobalProperty")
    set_global_read = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    object_by_name = tree.nodes.new("LogicNativeObjectByName")
    found_branch = tree.nodes.new("LogicNativeNotNone")
    found_flow_branch = tree.nodes.new("LogicNativeBranch")
    set_found = tree.nodes.new("LogicNativeSetGamePropertyBool")

    get_owner = tree.nodes.new("LogicNativeGetOwner")
    owner_branch = tree.nodes.new("LogicNativeNotNone")
    owner_flow_branch = tree.nodes.new("LogicNativeBranch")
    set_owner_ok = tree.nodes.new("LogicNativeSetGamePropertyBool")

    translate = tree.nodes.new("LogicNativeTranslate")

    value_output(append_one).default_value = 1.0
    value_output(append_two).default_value = 2.0
    value_output(dict_value).default_value = 3.0
    dict_set.inputs["Key"].default_value = "tier_a"
    value_output(global_value).default_value = 99.0
    set_global.inputs["Category"].default_value = "tier_a"
    set_global.inputs["Property"].default_value = "score"
    get_global.inputs["Category"].default_value = "tier_a"
    get_global.inputs["Property"].default_value = "score"
    object_by_name.inputs["Name"].default_value = target_name
    set_list_len.inputs["Property"].default_value = "tier_a_list_len"
    set_dict_len.inputs["Property"].default_value = "tier_a_dict_len"
    set_global_read.inputs["Property"].default_value = "tier_a_global"
    set_found.inputs["Property"].default_value = "tier_a_found_by_name"
    set_found.inputs["Value"].default_value = True
    set_owner_ok.inputs["Property"].default_value = "tier_a_owner_ok"
    set_owner_ok.inputs["Value"].default_value = True
    translate.inputs["Object"].default_value = mover
    translate.inputs["Vector"].default_value = (1.0, 0.0, 0.0)
    translate.inputs["Speed"].default_value = 15.0
    translate.inputs["Local"].default_value = False

    tree.links.new(empty_list.outputs["List"], list_append_a.inputs["List"])
    tree.links.new(value_output(append_one), list_append_a.inputs["Value"])
    tree.links.new(list_append_a.outputs["List"], list_append_b.inputs["List"])
    tree.links.new(value_output(append_two), list_append_b.inputs["Value"])
    tree.links.new(list_append_b.outputs["List"], list_length.inputs["List"])
    tree.links.new(list_length.outputs["Length"], set_list_len.inputs["Value"])

    tree.links.new(empty_dict.outputs["Dictionary"], dict_set.inputs["Dictionary"])
    tree.links.new(value_output(dict_value), dict_set.inputs["Value"])
    tree.links.new(dict_set.outputs["Dictionary"], dict_length.inputs["Dictionary"])
    tree.links.new(dict_length.outputs["Length"], set_dict_len.inputs["Value"])

    tree.links.new(value_output(global_value), set_global.inputs["Value"])
    tree.links.new(get_global.outputs["Value"], set_global_read.inputs["Value"])

    tree.links.new(object_by_name.outputs["Object"], found_branch.inputs["Value"])
    tree.links.new(get_owner.outputs["Owner Object"], owner_branch.inputs["Value"])

    for node in (
        set_list_len,
        set_dict_len,
        found_flow_branch,
        owner_flow_branch,
    ):
        tree.links.new(on_init.outputs["Out"], flow_input(node))
    global_sequence = tree.nodes.new("LogicNativeSequence")
    tree.links.new(on_init.outputs["Out"], global_sequence.inputs["Flow"])
    tree.links.new(global_sequence.outputs["Then 0"], flow_input(set_global))
    tree.links.new(global_sequence.outputs["Then 1"], flow_input(set_global_read))

    tree.links.new(found_branch.outputs["If Not None"], found_flow_branch.inputs["Condition"])
    tree.links.new(owner_branch.outputs["If Not None"], owner_flow_branch.inputs["Condition"])
    tree.links.new(found_flow_branch.outputs["True"], flow_input(set_found))
    tree.links.new(owner_flow_branch.outputs["True"], flow_input(set_owner_ok))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(translate))
    return tree


def build_raycast_tree(name, single_ignore, ignore_collection):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    raycast = tree.nodes.new("LogicNativeRaycast")
    raycast_all = tree.nodes.new("LogicNativeRaycastAll")
    shape_cast = tree.nodes.new("LogicNativeShapeCast")
    shape_cast_all = tree.nodes.new("LogicNativeShapeCastAll")
    collection_objects = tree.nodes.new("LogicNativeGetCollectionObjects")
    point = tree.nodes.new("LogicNativeSeparateXYZ")
    normal = tree.nodes.new("LogicNativeSeparateXYZ")
    direction = tree.nodes.new("LogicNativeSeparateXYZ")
    end_point = tree.nodes.new("LogicNativeSeparateXYZ")
    object_hit = tree.nodes.new("LogicNativeNotNone")
    hit_branch = tree.nodes.new("LogicNativeBranch")
    object_branch = tree.nodes.new("LogicNativeBranch")
    set_hit = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_object = tree.nodes.new("LogicNativeSetGamePropertyBool")

    raycast.input_mode = "END_POINT"
    raycast.inputs["Origin"].default_value = (0.0, 0.0, 5.0)
    raycast.inputs["Destination"].default_value = (0.0, 0.0, -5.0)
    raycast.inputs["Local"].default_value = False
    raycast.inputs["Mask"].default_value = 0
    collection_objects.inputs["Collection"].default_value = ignore_collection

    for query in (raycast, raycast_all, shape_cast, shape_cast_all):
        query.inputs["Ignore Object"].default_value = single_ignore
        tree.links.new(collection_objects.outputs["Objects"], query.inputs["Ignore Objects"])

    raycast_all.input_mode = "END_POINT"
    raycast_all.inputs["Origin"].default_value = (0.0, 0.0, 5.0)
    raycast_all.inputs["Destination"].default_value = (0.0, 0.0, -5.0)
    raycast_all.inputs["Local"].default_value = False
    raycast_all.inputs["Mask"].default_value = 0
    raycast_all.inputs["Max Results"].default_value = 16

    for query in (shape_cast, shape_cast_all):
        query.shape_type = "SPHERE"
        query.inputs["Origin"].default_value = (0.0, 0.0, 5.0)
        query.inputs["Destination"].default_value = (0.0, 0.0, -5.0)
        query.inputs["Radius"].default_value = 0.1
        query.inputs["Local"].default_value = False
        query.inputs["Mask"].default_value = 0
    shape_cast_all.inputs["Max Results"].default_value = 16

    set_hit.inputs["Property"].default_value = "ray_hit"
    set_hit.inputs["Value"].default_value = True
    set_object.inputs["Property"].default_value = "ray_object"
    set_object.inputs["Value"].default_value = True

    tree.links.new(raycast.outputs["Point"], point.inputs["Vector"])
    tree.links.new(raycast.outputs["Normal"], normal.inputs["Vector"])
    tree.links.new(raycast.outputs["Direction"], direction.inputs["Vector"])
    tree.links.new(raycast.outputs["End Point"], end_point.inputs["Vector"])
    tree.links.new(raycast.outputs["Picked Object"], object_hit.inputs["Value"])
    tree.links.new(on_update.outputs["Out"], flow_input(hit_branch))
    tree.links.new(on_update.outputs["Out"], flow_input(object_branch))
    tree.links.new(raycast.outputs["Has Result"], hit_branch.inputs["Condition"])
    tree.links.new(object_hit.outputs["If Not None"], object_branch.inputs["Condition"])
    tree.links.new(hit_branch.outputs["True"], flow_input(set_hit))
    tree.links.new(object_branch.outputs["True"], flow_input(set_object))

    def record_bool(query, output_name, property_name):
        setter = tree.nodes.new("LogicNativeSetGamePropertyBool")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(query.outputs["Done"], flow_input(setter))
        tree.links.new(query.outputs[output_name], setter.inputs["Value"])

    def record_int(query, output_name, property_name):
        setter = tree.nodes.new("LogicNativeSetGamePropertyInt")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(query.outputs["Done"], flow_input(setter))
        tree.links.new(query.outputs[output_name], setter.inputs["Value"])

    for query in (raycast_all, shape_cast, shape_cast_all):
        tree.links.new(on_update.outputs["Out"], query.inputs["Flow"])
    record_bool(raycast_all, "Has Result", "ray_all_hit")
    record_int(raycast_all, "Hit Count", "ray_all_count")
    record_bool(shape_cast, "Has Result", "shape_hit")
    record_bool(shape_cast_all, "Has Result", "shape_all_hit")
    record_int(shape_cast_all, "Hit Count", "shape_all_count")

    for socket_name, property_name in (
        ("X", "ray_point_x"),
        ("Y", "ray_point_y"),
        ("Z", "ray_point_z"),
    ):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(hit_branch.outputs["True"], flow_input(setter))
        tree.links.new(point.outputs[socket_name], setter.inputs["Value"])

    for socket_name, property_name in (
        ("X", "ray_normal_x"),
        ("Y", "ray_normal_y"),
        ("Z", "ray_normal_z"),
    ):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(hit_branch.outputs["True"], flow_input(setter))
        tree.links.new(normal.outputs[socket_name], setter.inputs["Value"])

    for socket_name, property_name in (
        ("X", "ray_direction_x"),
        ("Y", "ray_direction_y"),
        ("Z", "ray_direction_z"),
    ):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(hit_branch.outputs["True"], flow_input(setter))
        tree.links.new(direction.outputs[socket_name], setter.inputs["Value"])

    for socket_name, property_name in (
        ("X", "ray_end_point_x"),
        ("Y", "ray_end_point_y"),
        ("Z", "ray_end_point_z"),
    ):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(hit_branch.outputs["True"], flow_input(setter))
        tree.links.new(end_point.outputs[socket_name], setter.inputs["Value"])

    return tree


def build_raycast_from_mesh_tree(name, source, right_target, left_target, ignore_collection):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    face_query = tree.nodes.new("LogicNativeRaycastFromMesh")
    vertex_query = tree.nodes.new("LogicNativeRaycastFromMesh")
    blocked_query = tree.nodes.new("LogicNativeRaycastFromMesh")
    collection_objects = tree.nodes.new("LogicNativeGetCollectionObjects")
    collection_objects.inputs["Collection"].default_value = ignore_collection

    face_query.mesh_domain = "FACES"
    face_query.ray_direction = "NORMALS"
    face_query.inputs["Source Object"].default_value = source
    face_query.inputs["Ignore Object"].default_value = left_target
    face_query.inputs["Start Offset"].default_value = 0.05
    face_query.inputs["Max Distance"].default_value = 4.0
    face_query.inputs["Max Rays"].default_value = 256
    face_query.inputs["Property"].default_value = "mesh_target"
    face_query.inputs["X-Ray"].default_value = True
    face_query.inputs["Mask"].default_value = 0
    face_query.inputs["Include Sensors"].default_value = False
    face_query.inputs["Hit Backfaces"].default_value = True
    face_query.inputs["Visualize"].default_value = True
    tree.links.new(collection_objects.outputs["Objects"], face_query.inputs["Ignore Objects"])

    # This second query deliberately uses the owner fallback, the vertex domain, inverted normals,
    # and fewer than eight rays so the synchronous sequential path is exercised too.
    vertex_query.mesh_domain = "VERTICES"
    vertex_query.ray_direction = "INVERTED_NORMALS"
    vertex_query.inputs["Ignore Object"].default_value = right_target
    vertex_query.inputs["Start Offset"].default_value = 0.02
    vertex_query.inputs["Max Distance"].default_value = 4.0
    vertex_query.inputs["Max Rays"].default_value = 7
    vertex_query.inputs["Property"].default_value = "mesh_target"
    vertex_query.inputs["X-Ray"].default_value = True
    vertex_query.inputs["Mask"].default_value = 0

    # The non-X-Ray query must stop at the nearer untagged blocker. It verifies that blocked rays
    # remain index-aligned without being misreported as matching hits.
    blocked_query.mesh_domain = "FACES"
    blocked_query.ray_direction = "NORMALS"
    blocked_query.inputs["Source Object"].default_value = source
    blocked_query.inputs["Ignore Object"].default_value = left_target
    blocked_query.inputs["Start Offset"].default_value = 0.05
    blocked_query.inputs["Max Distance"].default_value = 4.0
    blocked_query.inputs["Max Rays"].default_value = 4
    blocked_query.inputs["Property"].default_value = "mesh_target"
    blocked_query.inputs["X-Ray"].default_value = False
    blocked_query.inputs["Mask"].default_value = 0

    for query in (face_query, vertex_query, blocked_query):
        tree.links.new(on_update.outputs["Out"], query.inputs["Flow"])

    def record_bool(flow, value, property_name):
        setter = tree.nodes.new("LogicNativeSetGamePropertyBool")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(flow, setter.inputs["Flow"])
        if value is None:
            setter.inputs["Value"].default_value = True
        else:
            tree.links.new(value, setter.inputs["Value"])

    def record_int(flow, value, property_name):
        setter = tree.nodes.new("LogicNativeSetGamePropertyInt")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(flow, setter.inputs["Flow"])
        tree.links.new(value, setter.inputs["Value"])

    def record_float(flow, value, property_name):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(flow, setter.inputs["Flow"])
        tree.links.new(value, setter.inputs["Value"])

    record_bool(face_query.outputs["Done"], None, "mesh_face_done")
    record_bool(face_query.outputs["Done"], face_query.outputs["Any Hit"], "mesh_face_any")
    record_bool(face_query.outputs["Done"], face_query.outputs["All Hit"], "mesh_face_all")
    record_int(
        face_query.outputs["Done"], face_query.outputs["Hit Count"], "mesh_face_hit_count")
    record_int(
        face_query.outputs["Done"], face_query.outputs["Ray Count"], "mesh_face_ray_count")

    list_property_names = {
        "Hits": "hits",
        "Blocked": "blocked",
        "Hit Objects": "hit_objects",
        "Points": "points",
        "Normals": "normals",
        "Distances": "distances",
        "Fractions": "fractions",
        "Face Indices": "face_indices",
        "Has UVs": "has_uvs",
        "UVs": "uvs",
        "Ray Origins": "ray_origins",
        "Ray Directions": "ray_directions",
        "End Points": "end_points",
    }

    def record_list_lengths(query, property_prefix):
        for socket_name, suffix in list_property_names.items():
            length = tree.nodes.new("LogicNativeListLength")
            tree.links.new(query.outputs[socket_name], length.inputs["List"])
            record_int(
                query.outputs["Done"],
                length.outputs["Length"],
                f"{property_prefix}_len_{suffix}",
            )

    record_list_lengths(face_query, "mesh_face")
    record_list_lengths(blocked_query, "mesh_blocked")

    def record_average(query, property_prefix):
        average = tree.nodes.new("LogicNativeGetAverageCenterPosition")
        average.source_type = "VECTOR_LIST"
        separate = tree.nodes.new("LogicNativeSeparateXYZ")
        tree.links.new(query.outputs["Points"], average.inputs["Vectors"])
        tree.links.new(average.outputs["Center"], separate.inputs["Vector"])
        record_int(query.outputs["Done"], average.outputs["Count"], f"{property_prefix}_count")
        record_bool(query.outputs["Done"], average.outputs["Valid"], f"{property_prefix}_valid")
        for component in ("X", "Y", "Z"):
            record_float(
                query.outputs["Done"],
                separate.outputs[component],
                f"{property_prefix}_{component.lower()}",
            )

    record_average(face_query, "mesh_face_average")
    record_average(blocked_query, "mesh_blocked_average")

    def record_first_bool(query, list_name, property_name):
        item = tree.nodes.new("LogicNativeListGetItem")
        cast = tree.nodes.new("LogicNativeTypecast")
        cast.to_type = "BOOL"
        tree.links.new(query.outputs[list_name], item.inputs["List"])
        tree.links.new(item.outputs["Value"], cast.inputs["Value"])
        record_bool(query.outputs["Done"], cast.outputs["Boolean"], property_name)

    record_first_bool(face_query, "Hits", "mesh_face_first_hit")
    record_first_bool(face_query, "Blocked", "mesh_face_first_blocked")
    first_object = tree.nodes.new("LogicNativeListGetItem")
    object_exists = tree.nodes.new("LogicNativeTypecast")
    object_exists.to_type = "BOOL"
    tree.links.new(face_query.outputs["Hit Objects"], first_object.inputs["List"])
    tree.links.new(first_object.outputs["Value"], object_exists.inputs["Value"])
    record_bool(
        face_query.outputs["Done"],
        object_exists.outputs["Boolean"],
        "mesh_face_first_object",
    )

    record_bool(
        vertex_query.outputs["Done"], vertex_query.outputs["Any Hit"], "mesh_vertex_any")
    record_bool(
        vertex_query.outputs["Done"], vertex_query.outputs["All Hit"], "mesh_vertex_all")
    record_int(
        vertex_query.outputs["Done"],
        vertex_query.outputs["Hit Count"],
        "mesh_vertex_hit_count",
    )
    record_int(
        vertex_query.outputs["Done"],
        vertex_query.outputs["Ray Count"],
        "mesh_vertex_ray_count",
    )

    record_bool(
        blocked_query.outputs["Done"], blocked_query.outputs["Any Hit"], "mesh_blocked_any")
    record_bool(
        blocked_query.outputs["Done"], blocked_query.outputs["All Hit"], "mesh_blocked_all")
    record_int(
        blocked_query.outputs["Done"],
        blocked_query.outputs["Hit Count"],
        "mesh_blocked_hit_count",
    )
    record_int(
        blocked_query.outputs["Done"],
        blocked_query.outputs["Ray Count"],
        "mesh_blocked_ray_count",
    )
    record_first_bool(blocked_query, "Blocked", "mesh_blocked_first")
    return tree


def build_send_receive_event_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    send_event = tree.nodes.new("LogicNativeSendEvent")
    receive_event = tree.nodes.new("LogicNativeReceiveEvent")
    branch = tree.nodes.new("LogicNativeBranch")
    set_event_ok = tree.nodes.new("LogicNativeSetGamePropertyBool")

    send_event.inputs["Subject"].default_value = "acceptance_event"
    receive_event.inputs["Subject"].default_value = "acceptance_event"
    set_event_ok.inputs["Property"].default_value = "event_ok"
    set_event_ok.inputs["Value"].default_value = True

    # Send on init; receive on the next logic tick (default next-tick delivery).
    tree.links.new(on_init.outputs["Out"], send_event.inputs["Flow"])
    tree.links.new(on_fixed_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(receive_event.outputs["Received"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_event_ok))
    return tree


def build_move_toward_tree(name, target_position, target_obj=None):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    move_toward = tree.nodes.new("LogicNativeMoveToward")

    move_toward.inputs["Target Position"].default_value = target_position
    move_toward.inputs["Speed"].default_value = 4.0
    move_toward.inputs["Stop Distance"].default_value = 0.1
    if target_obj is not None:
        move_toward.inputs["Object"].default_value = target_obj

    tree.links.new(on_fixed_update.outputs["Out"], flow_input(move_toward))
    return tree


def build_navigate_visualize_tree(name, target_position, target_obj=None):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    navigate = tree.nodes.new("LogicNativeNavigate")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")

    navigate.inputs["Destination"].default_value = target_position
    navigate.inputs["Move as Dynamic"].default_value = False
    navigate.inputs["Lin Speed"].default_value = 0.25
    navigate.inputs["Reach Threshold"].default_value = 0.05
    navigate.inputs["Look At"].default_value = False
    navigate.inputs["Visualize"].default_value = True
    if target_obj is not None:
        navigate.inputs["Moving Object"].default_value = target_obj
        set_position.inputs["Object"].default_value = target_obj

    tree.links.new(on_fixed_update.outputs["Out"], flow_input(navigate))
    tree.links.new(navigate.outputs["Done"], flow_input(set_position))
    tree.links.new(navigate.outputs["Next Point"], set_position.inputs["Value"])
    return tree


def build_copy_property_tree(name, source_obj, target_obj, property_name, property_value):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    set_source = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    copy_property = tree.nodes.new("LogicNativeCopyProperty")

    set_source.inputs["Property"].default_value = property_name
    set_source.inputs["Value"].default_value = property_value
    copy_property.inputs["Source"].default_value = source_obj
    copy_property.inputs["Target"].default_value = target_obj
    copy_property.inputs["Property"].default_value = property_name

    tree.links.new(on_init.outputs["Out"], flow_input(set_source))
    tree.links.new(on_update.outputs["Out"], flow_input(copy_property))
    return tree


def build_get_object_attribute_tree(name, expected_position):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    set_position = new_set_object_attribute(tree, "WORLD_POSITION")
    get_attribute = new_get_object_attribute(tree, "WORLD_POSITION")
    separate = tree.nodes.new("LogicNativeSeparateXYZ")
    set_x = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_y = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    set_position.inputs["Value"].default_value = expected_position
    set_x.inputs["Property"].default_value = "read_x"
    set_y.inputs["Property"].default_value = "read_y"
    set_z.inputs["Property"].default_value = "read_z"

    tree.links.new(on_init.outputs["Out"], flow_input(set_position))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_x))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_y))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_z))
    tree.links.new(get_attribute.outputs["Vector"], separate.inputs["Vector"])
    tree.links.new(separate.outputs["X"], set_x.inputs["Value"])
    tree.links.new(separate.outputs["Y"], set_y.inputs["Value"])
    tree.links.new(separate.outputs["Z"], set_z.inputs["Value"])
    return tree


def build_rotate_toward_tree(name, target_position, target_obj=None):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    rotate_toward = tree.nodes.new("LogicNativeRotateToward")

    rotate_toward.inputs["Target"].default_value = target_position
    rotate_toward.inputs["Factor"].default_value = 0.35
    rotate_toward.inputs["Rot Axis"].default_value = 2
    rotate_toward.inputs["Front Axis"].default_value = 1
    if target_obj is not None:
        rotate_toward.inputs["Object"].default_value = target_obj

    tree.links.new(on_fixed_update.outputs["Out"], flow_input(rotate_toward))
    return tree


def build_collision_tree(name):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    collision = tree.nodes.new("LogicNativeCollision")
    branch = tree.nodes.new("LogicNativeBranch")
    set_collided = tree.nodes.new("LogicNativeSetGamePropertyBool")
    typecast_object = tree.nodes.new("LogicNativeTypecast")
    set_object_name = tree.nodes.new("LogicNativeSetGamePropertyString")
    separate_point = tree.nodes.new("LogicNativeSeparateXYZ")
    separate_normal = tree.nodes.new("LogicNativeSeparateXYZ")

    collision.inputs["Property"].default_value = "collider_marker"
    typecast_object.to_type = "STRING"

    set_collided.inputs["Property"].default_value = "collided"
    set_collided.inputs["Value"].default_value = True
    set_object_name.inputs["Property"].default_value = "collided_object_name"

    float_setters = {}
    for property_name in (
            "collision_point_x",
            "collision_point_y",
            "collision_point_z",
            "collision_normal_x",
            "collision_normal_y",
            "collision_normal_z"):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        float_setters[property_name] = setter

    tree.links.new(on_fixed_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(collision.outputs["Colliding"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_collided))
    tree.links.new(collision.outputs["Collided Object"], typecast_object.inputs["Value"])
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(set_object_name))
    tree.links.new(typecast_object.outputs["String"], set_object_name.inputs["Value"])
    tree.links.new(collision.outputs["Point"], separate_point.inputs["Vector"])
    tree.links.new(collision.outputs["Normal"], separate_normal.inputs["Vector"])
    for socket_name, property_name in (("X", "collision_point_x"),
                                       ("Y", "collision_point_y"),
                                       ("Z", "collision_point_z")):
        setter = float_setters[property_name]
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(setter))
        tree.links.new(separate_point.outputs[socket_name], setter.inputs["Value"])
    for socket_name, property_name in (("X", "collision_normal_x"),
                                       ("Y", "collision_normal_y"),
                                       ("Z", "collision_normal_z")):
        setter = float_setters[property_name]
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(setter))
        tree.links.new(separate_normal.outputs[socket_name], setter.inputs["Value"])
    return tree


def build_objects_colliding_tree(name, object_a, object_b):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    colliding = tree.nodes.new("LogicNativeObjectsColliding")
    branch = tree.nodes.new("LogicNativeBranch")
    set_colliding = tree.nodes.new("LogicNativeSetGamePropertyBool")

    colliding.inputs["Object A"].default_value = object_a
    colliding.inputs["Object B"].default_value = object_b
    set_colliding.inputs["Property"].default_value = "colliding"
    set_colliding.inputs["Value"].default_value = True

    tree.links.new(on_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(colliding.outputs["Colliding"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_colliding))
    return tree


def build_animation_status_tree(name, action):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    play_action = tree.nodes.new("LogicNativePlayAction")
    animation_status = tree.nodes.new("LogicNativeAnimationStatus")
    branch = tree.nodes.new("LogicNativeBranch")
    set_playing = tree.nodes.new("LogicNativeSetGamePropertyBool")

    play_action.action = action
    play_action.play_mode = "LOOP"
    play_action.inputs["End Frame"].default_value = 40.0
    set_playing.inputs["Property"].default_value = "action_playing"
    set_playing.inputs["Value"].default_value = True

    tree.links.new(on_init.outputs["Out"], flow_input(play_action))
    tree.links.new(on_fixed_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(animation_status.outputs["Is Playing"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_playing))

    # Exercise the Play Action node's exact asynchronous operation identity independently from
    # Animation Status. The short one-shot runs on layer 1 while the loop above remains active on
    # layer 0, proving that Completed belongs to the accepted request rather than to an inferred
    # timeout or a global object state.
    lifecycle_action = tree.nodes.new("LogicNativePlayAction")
    lifecycle_action.action = action
    lifecycle_action.play_mode = "PLAY"
    lifecycle_action.inputs["Start Frame"].default_value = 1.0
    lifecycle_action.inputs["End Frame"].default_value = 2.0
    lifecycle_action.inputs["Layer"].default_value = 1
    tree.links.new(on_init.outputs["Out"], flow_input(lifecycle_action))

    for output_name, property_name in (
            ("Started", "action_lifecycle_started"),
            ("Completed", "action_lifecycle_completed"),
            ("Interrupted", "action_lifecycle_interrupted"),
            ("Failed", "action_lifecycle_failed")):
        setter = tree.nodes.new("LogicNativeSetGamePropertyBool")
        setter.inputs["Property"].default_value = property_name
        setter.inputs["Value"].default_value = True
        tree.links.new(lifecycle_action.outputs[output_name], flow_input(setter))

    # A looping request cannot complete naturally. Stop it only after its Started result has been
    # delivered, then require the exact request's Interrupted result on a later fixed tick.
    interrupted_action = tree.nodes.new("LogicNativePlayAction")
    interrupted_action.action = action
    interrupted_action.play_mode = "LOOP"
    interrupted_action.inputs["Start Frame"].default_value = 1.0
    interrupted_action.inputs["End Frame"].default_value = 40.0
    interrupted_action.inputs["Layer"].default_value = 2
    stop_interrupted_action = tree.nodes.new("LogicNativeStopAction")
    stop_interrupted_action.inputs["Layer"].default_value = 2
    set_interrupted = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_interrupted.inputs["Property"].default_value = "action_interruption_observed"
    set_interrupted.inputs["Value"].default_value = True
    tree.links.new(on_init.outputs["Out"], flow_input(interrupted_action))
    tree.links.new(interrupted_action.outputs["Started"], flow_input(stop_interrupted_action))
    tree.links.new(interrupted_action.outputs["Interrupted"], flow_input(set_interrupted))

    # No action datablock is deliberately selected. This must compile as a recoverable request and
    # pulse Failed after command validation rather than pretending that playback started.
    failed_action = tree.nodes.new("LogicNativePlayAction")
    failed_action.inputs["Layer"].default_value = 3
    set_failed = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_failed.inputs["Property"].default_value = "action_failure_observed"
    set_failed.inputs["Value"].default_value = True
    tree.links.new(on_init.outputs["Out"], flow_input(failed_action))
    tree.links.new(failed_action.outputs["Failed"], flow_input(set_failed))
    return tree


def build_save_game_tree(name, save_path):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    save_game = tree.nodes.new("LogicNativeSaveGame")
    save_game.inputs["Path"].default_value = save_path
    tree.links.new(on_init.outputs["Out"], flow_input(save_game))
    return tree


def build_load_game_tree(name, save_path):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    load_game = tree.nodes.new("LogicNativeLoadGame")
    load_game.inputs["Path"].default_value = save_path
    tree.links.new(on_update.outputs["Out"], flow_input(load_game))
    return tree


def build_align_axis_tree(name, target_obj=None):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    align_axis = tree.nodes.new("LogicNativeAlignAxisToVector")

    align_axis.inputs["Vector"].default_value = (1.0, 0.0, 0.0)
    align_axis.inputs["Axis"].default_value = 2
    align_axis.inputs["Factor"].default_value = 0.35
    if target_obj is not None:
        align_axis.inputs["Object"].default_value = target_obj

    tree.links.new(on_fixed_update.outputs["Out"], flow_input(align_axis))
    return tree


def build_slow_follow_tree(name, target_obj, follower_obj=None):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    slow_follow = tree.nodes.new("LogicNativeSlowFollow")

    slow_follow.inputs["Target"].default_value = target_obj
    slow_follow.inputs["Factor"].default_value = 0.25
    if follower_obj is not None:
        slow_follow.inputs["Object"].default_value = follower_obj

    tree.links.new(on_fixed_update.outputs["Out"], flow_input(slow_follow))
    return tree


def build_replace_mesh_tree(name, mesh_obj, target_obj=None):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    replace_mesh = tree.nodes.new("LogicNativeReplaceMesh")
    set_done = tree.nodes.new("LogicNativeSetGamePropertyBool")

    replace_mesh.inputs["Mesh Object"].default_value = mesh_obj
    set_done.inputs["Property"].default_value = "mesh_replaced"
    set_done.inputs["Value"].default_value = True
    if target_obj is not None:
        replace_mesh.inputs["Object"].default_value = target_obj

    tree.links.new(on_init.outputs["Out"], flow_input(replace_mesh))
    tree.links.new(replace_mesh.outputs["Done"], flow_input(set_done))
    return tree


def build_mouse_over_tree(name, target_obj, keep_cursor_centered=False):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_cursor = tree.nodes.new("LogicNativeSetCursorPosition")
    mouse_over = tree.nodes.new("LogicNativeMouseOver")
    point = tree.nodes.new("LogicNativeSeparateXYZ")
    normal = tree.nodes.new("LogicNativeSeparateXYZ")
    set_enter = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_over = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_exit = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_hit_object = tree.nodes.new("LogicNativeSetGamePropertyBool")
    count_over = tree.nodes.new("LogicNativeModifyProperty")

    set_cursor.inputs["Screen X"].default_value = 0.5
    set_cursor.inputs["Screen Y"].default_value = 0.5
    mouse_over.inputs["Object"].default_value = target_obj

    for node, name_value in (
        (set_enter, "mouse_enter"),
        (set_over, "mouse_over"),
        (set_exit, "mouse_exit"),
    ):
        node.inputs["Property"].default_value = name_value
        node.inputs["Value"].default_value = True

    count_over.inputs["Property"].default_value = "mouse_over_count"
    count_over.inputs["Value"].default_value = 1.0
    set_hit_object.inputs["Property"].default_value = "mouse_over_object_output"
    set_hit_object.inputs["Value"].default_value = True

    tree.links.new(on_init.outputs["Out"], flow_input(set_cursor))
    if keep_cursor_centered:
        on_fixed = tree.nodes.new("LogicNativeOnFixedUpdate")
        fixed_cursor = tree.nodes.new("LogicNativeSetCursorPosition")
        fixed_cursor.inputs["Screen X"].default_value = 0.5
        fixed_cursor.inputs["Screen Y"].default_value = 0.5
        tree.links.new(on_fixed.outputs["Out"], flow_input(fixed_cursor))
    tree.links.new(mouse_over.outputs["Point"], point.inputs["Vector"])
    tree.links.new(mouse_over.outputs["Normal"], normal.inputs["Vector"])
    tree.links.new(mouse_over.outputs["On Enter"], flow_input(set_enter))
    tree.links.new(mouse_over.outputs["On Over"], flow_input(set_over))
    tree.links.new(mouse_over.outputs["On Exit"], flow_input(set_exit))
    tree.links.new(mouse_over.outputs["On Over"], flow_input(count_over))
    tree.links.new(mouse_over.outputs["On Over"], flow_input(set_hit_object))
    tree.links.new(mouse_over.outputs["Object"], set_hit_object.inputs["Object"])

    for socket_name, property_name in (
        ("X", "mouse_point_x"),
        ("Y", "mouse_point_y"),
        ("Z", "mouse_point_z"),
    ):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(mouse_over.outputs["On Over"], flow_input(setter))
        tree.links.new(point.outputs[socket_name], setter.inputs["Value"])

    for socket_name, property_name in (
        ("X", "mouse_normal_x"),
        ("Y", "mouse_normal_y"),
        ("Z", "mouse_normal_z"),
    ):
        setter = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(mouse_over.outputs["On Over"], flow_input(setter))
        tree.links.new(normal.outputs[socket_name], setter.inputs["Value"])

    return tree


def socket_by_identifier(sockets, identifier):
    for socket in sockets:
        if socket.identifier == identifier:
            return socket
    raise KeyError(identifier)


def create_mix_factor_material(name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    ntree = material.node_tree

    mix = ntree.nodes.new("ShaderNodeMix")
    mix.name = "Mix"
    mix.data_type = "RGBA"
    socket_by_identifier(mix.inputs, "Factor_Float").default_value = 0.0
    socket_by_identifier(mix.inputs, "A_Color").default_value = (1.0, 0.0, 0.0, 1.0)
    socket_by_identifier(mix.inputs, "B_Color").default_value = (0.0, 1.0, 0.0, 1.0)

    principled = next(
        node for node in ntree.nodes if node.bl_idname == "ShaderNodeBsdfPrincipled"
    )
    ntree.links.new(socket_by_identifier(mix.outputs, "Result_Color"), principled.inputs["Base Color"])
    return material


def build_material_shader_input_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_input = tree.nodes.new("LogicNativeSetEditorNodeValue")
    set_input.node_editor_type = "SHADER"
    set_input.target_scope = "OWNER"
    set_input.value_type = "FLOAT"
    set_input.inputs["Slot"].default_value = 0
    set_input.inputs["Node Name"].default_value = "Mix"
    set_input.inputs["Socket"].default_value = "Factor"
    set_input.inputs["Float Value"].default_value = 1.0
    tree.links.new(on_init.outputs["Out"], flow_input(set_input))
    return tree


def prepare_run(obj, run_id):
    set_game_property(obj, "__ln_acceptance_run", "STRING", run_id)
    set_game_property(obj, "__ln_acceptance_frame", "INT", 0)
    set_game_property(obj, "__ln_acceptance_done", "BOOL", False)
    obj.location = (0.0, 0.0, 0.0)
    obj.color = (1.0, 1.0, 1.0, 1.0)
    obj.hide_viewport = False
    bpy.context.view_layer.update()


def resolve_live_object(obj_or_name):
    if isinstance(obj_or_name, str):
        obj = bpy.data.objects.get(obj_or_name)
        require(obj is not None, f"{obj_or_name} was not found in bpy.data.objects")
        return obj
    return obj_or_name


def safe_filename(value):
    return "".join(character if character.isalnum() or character == "_" else "_" for character in value)


def run_blenderplayer_once(run_id):
    global LAST_PLAYER_OUTPUT
    require(PLAYER_OUTPUT_DIR is not None, "Player output directory is not configured")
    player_path = Path(bpy.app.binary_path).with_name("blenderplayer")
    require(player_path.exists(), f"blenderplayer was not found next to {bpy.app.binary_path}")

    blend_path = PLAYER_OUTPUT_DIR / f"{safe_filename(run_id)}.blend"
    mainloop_path = PLAYER_OUTPUT_DIR / "logic_nodes_player_mainloop.py"
    mainloop_path.write_text(PLAYER_MAINLOOP_SCRIPT, encoding="utf-8")
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), copy=True)

    command = [
        str(player_path),
        "-p",
        str(mainloop_path),
        "-w",
        "320",
        "200",
        "0",
        "0",
        str(blend_path),
    ]
    result = subprocess.run(
        command,
        cwd=str(player_path.parent),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=120,
        check=False,
    )
    LAST_PLAYER_OUTPUT = result.stdout or ""
    if result.returncode != 0:
        output = result.stdout[-4000:] if result.stdout else ""
        raise RuntimeError(f"blenderplayer failed for {run_id} with {result.returncode}:\n{output}")


def run_game_once(obj_or_name, run_id, status_path, prepare=True):
    obj = resolve_live_object(obj_or_name)
    obj_name = obj.name
    if prepare:
        prepare_run(obj, run_id)
    before_line_count = 0
    if status_path.exists():
        before_line_count = len(status_path.read_text(encoding="utf-8").splitlines())

    if RUNNER == "view3d":
        with bpy.context.temp_override(**active_view3d_context()):
            result = bpy.ops.view3d.game_start()
        require(result == {"FINISHED"}, f"view3d.game_start failed for {run_id}: {result}")
    else:
        run_blenderplayer_once(run_id)

    require(status_path.exists(), f"{run_id} did not write a status file")
    lines = status_path.read_text(encoding="utf-8").splitlines()
    new_lines = lines[before_line_count:]
    require(new_lines, f"{run_id} did not write a new status record")

    payload = json.loads(new_lines[-1])
    require(payload.get("run") == run_id, f"{run_id} wrote unexpected status: {payload}")
    require(payload.get("ok", False), f"{run_id} failed: {payload}")
    return bpy.data.objects.get(obj_name)


def run_basic_restart_probe(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_NoLogic")
    add_status_controller(target, status_path, "quit_only")
    target_name = target.name
    target = run_game_once(target, "no_logic_restart_0", status_path)
    require(target is not None, f"{target_name} did not survive first restart probe")
    run_game_once(target_name, "no_logic_restart_1", status_path)


def run_editor_tree_inactive_probe(status_path):
    reset_scene()
    configure_game_settings(True)
    tree = build_move_tree(f"{PREFIX}_InactiveTree", (1.0, 0.0, 0.0))
    require(tree.bl_idname == "LogicNodeTree", "Inactive probe did not create a LogicNodeTree")
    target = create_cube(f"{PREFIX}_InactiveRuntime")
    add_status_controller(target, status_path, "quit_only")
    target_name = target.name
    target = run_game_once(target, "inactive_editor_tree_0", status_path)
    require(target is not None, f"{target_name} did not survive inactive tree probe")
    run_game_once(target_name, "inactive_editor_tree_1", status_path)


def run_debug_hardcoded_probe(status_path):
    reset_scene()
    configure_game_settings(True)
    expected = (1.25, 2.5, 3.75)
    target = create_cube(f"{PREFIX}_DebugHardcoded")
    add_status_controller(target, status_path, "position", required_frame=2)
    set_expected_vector(target, expected)
    set_game_property(target, "__ln_debug_set_world_position", "FLOAT", 1.0)
    set_game_property(target, "__ln_debug_set_world_position_x", "FLOAT", expected[0])
    set_game_property(target, "__ln_debug_set_world_position_y", "FLOAT", expected[1])
    set_game_property(target, "__ln_debug_set_world_position_z", "FLOAT", expected[2])
    previous_debug_bindings = os.environ.get("UPBGE_LOGIC_NODES_DEBUG_BINDINGS")
    os.environ["UPBGE_LOGIC_NODES_DEBUG_BINDINGS"] = "1"
    try:
        target_name = target.name
        target = run_game_once(target, "debug_hardcoded_0", status_path)
        require(target is not None, f"{target_name} did not survive debug hardcoded probe")
        run_game_once(target_name, "debug_hardcoded_1", status_path)
    finally:
        if previous_debug_bindings is None:
            os.environ.pop("UPBGE_LOGIC_NODES_DEBUG_BINDINGS", None)
        else:
            os.environ["UPBGE_LOGIC_NODES_DEBUG_BINDINGS"] = previous_debug_bindings


def save_reload_and_run(obj_name, run_id, status_path, blend_path):
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), copy=True)
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))
    obj = bpy.data.objects.get(obj_name)
    require(obj is not None, f"{obj_name} did not survive save/reload")
    run_game_once(obj, run_id, status_path)


def run_case_twice_and_after_reload(status_path, output_dir, label, build_case):
    reset_scene()
    configure_game_settings(True)
    target = build_case(status_path)
    target_name = target.name
    target = run_game_once(target, f"{label}_0", status_path)
    require(target is not None, f"{target_name} did not survive {label}_0")
    run_game_once(target_name, f"{label}_1", status_path)
    save_reload_and_run(target_name, f"{label}_reload", status_path, output_dir / f"{label}.blend")


def run_move_on_init_acceptance(status_path, output_dir):
    expected = (2.0, 3.0, 4.0)

    def build_case(case_status_path):
        target = create_cube(f"{PREFIX}_MoveOnInit")
        probe = create_cube(f"{PREFIX}_MoveOnInitDriver")
        tree = build_move_tree(f"{PREFIX}_MoveOnInitTree", expected, "INIT", target)
        add_status_controller(target, case_status_path, "position", required_frame=2)
        set_expected_vector(target, expected)
        bind_tree(probe, tree.name, True)
        return target

    run_case_twice_and_after_reload(status_path, output_dir, "move_on_init", build_case)


def run_logic_gate_acceptance(status_path, output_dir):
    expected = (4.0, 1.0, 0.0)

    reset_scene()
    configure_game_settings(True)
    tree = build_logic_gate_tree(f"{PREFIX}_LogicGateTree", expected)
    target = create_cube(f"{PREFIX}_LogicGate")
    add_status_controller(target, status_path, "position", required_frame=2)
    set_expected_vector(target, expected)
    for index, value in enumerate((False, False, True, True, True)):
        set_game_property(target, f"combine_condition_{index}", "BOOL", value)
    bind_tree(target, tree.name, True)

    binding = target.game.logic_node_bindings[0]
    original_name = tree.name
    tree.name = f"{original_name}_Renamed"
    require(binding.tree == tree, "Renaming a bound tree changed the binding identity")
    require(
        binding.tree_name == tree.name,
        "Renaming a bound tree did not update the binding name immediately",
    )

    target_name = target.name
    run_game_once(target, "logic_gate_renamed_0", status_path)
    save_reload_and_run(
        target_name,
        "logic_gate_renamed_reload",
        status_path,
        output_dir / "logic_gate_renamed.blend",
    )


def run_execution_flow_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_ExecutionFlow")
    tree = build_execution_flow_tree(f"{PREFIX}_ExecutionFlowTree")
    add_status_controller(target, status_path, "position", required_frame=2)
    set_expected_vector(target, (2.0, 0.0, 0.0))
    bind_tree(target, tree.name, True)
    run_game_once(target, "execution_flow_0", status_path)


def run_euler_acceptance(status_path, output_dir):
    expected = (0.15, -0.25, 0.45)

    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Euler")
    probe = create_cube(f"{PREFIX}_EulerDriver")
    tree = build_euler_orientation_tree(f"{PREFIX}_EulerTree", expected, target)
    add_status_controller(target, status_path, "orientation", required_frame=2, tolerance=0.01)
    set_expected_vector(target, expected)
    bind_tree(probe, tree.name, True)
    run_game_once(target, "euler_0", status_path)


def run_orientation_acceptance(status_path, output_dir):
    expected = (0.25, -0.35, 0.5)

    def build_case(case_status_path):
        tree = build_orientation_tree(f"{PREFIX}_OrientationTree")
        target = create_cube(f"{PREFIX}_Orientation")
        target.rotation_euler = expected
        add_status_controller(target, case_status_path, "orientation", tolerance=0.01)
        set_expected_vector(target, expected)
        bind_tree(target, tree.name, True)
        bpy.context.view_layer.update()
        return target

    run_case_twice_and_after_reload(status_path, output_dir, "orientation", build_case)


def run_velocity_acceptance(status_path, output_dir):
    expected = (3.0, 0.0, 0.0)

    def build_case(case_status_path):
        target = create_cube(f"{PREFIX}_Velocity", True)
        probe = create_cube(f"{PREFIX}_VelocityDriver")
        tree = build_velocity_tree(f"{PREFIX}_VelocityTree", expected, target)
        add_status_controller(target, case_status_path, "velocity", required_frame=2, tolerance=0.2)
        set_expected_vector(target, expected)
        bind_tree(probe, tree.name, True)
        return target

    run_case_twice_and_after_reload(status_path, output_dir, "velocity", build_case)


def run_impulse_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Impulse", True)
    probe = create_cube(f"{PREFIX}_ImpulseDriver")
    tree = build_impulse_tree(f"{PREFIX}_ImpulseTree", (5.0, 0.0, 0.0), target)
    add_status_controller(target, status_path, "impulse", required_frame=2, tolerance=0.2)
    set_game_property(target, "__ln_acceptance_min_speed", "FLOAT", 0.1)
    bind_tree(probe, tree.name, True)
    target_name = target.name
    target = run_game_once(target, "impulse_0", status_path)
    require(target is not None, f"{target_name} did not survive impulse_0")
    run_game_once(target_name, "impulse_1", status_path)


def run_object_actions_acceptance(status_path, output_dir):
    expected_position = (1.5, -0.5, 0.25)
    expected_rotation = (0.0, 0.0, 0.4)
    force = (0.0, 0.0, 0.0)
    torque = (0.0, 0.0, 0.0)
    expected_color = (0.2, 0.4, 0.6, 0.8)
    expected_visible = False

    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_ObjectActions")
    tree = build_object_actions_tree(
        f"{PREFIX}_ObjectActionsTree",
        expected_position,
        expected_rotation,
        force,
        torque,
        expected_color,
        expected_visible,
    )
    add_status_controller(target, status_path, "object_actions", required_frame=2)
    set_expected_vector(target, expected_position)
    set_expected_rotation(target, expected_rotation)
    set_expected_color(target, expected_color)
    set_game_property(target, "__ln_acceptance_expected_visible", "BOOL", expected_visible)
    bind_tree(target, tree.name, True)
    run_game_once(target, "object_actions_0", status_path)


def run_material_actions_acceptance(status_path):
    """Material actions per object (2026-10-19). Play Action with an action made by keyframing a
    material's Viewport Display color (what Workbench draws) changes only the object it plays on:
    that object gets its own copy of the material, the other cube with the material keeps it, an
    Add Object copy starts with the shared material and gets its own copy when it plays the action
    (gone again when the copy ends). Without the game's undo, the object has its material back
    after the game and no copy is left."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    scene.game_settings.use_undo = False
    material = bpy.data.materials.new(f"{PREFIX}_MAShared")
    # Keyframed like a user does in the material's Viewport Display panel; the action stays
    # assigned to the material (as in the editor), the game plays it on objects.
    for frame, color in ((1.0, (0.2, 0.2, 0.2, 1.0)), (5.0, (1.0, 0.5, 0.0, 1.0))):
        material.diffuse_color = color
        material.keyframe_insert("diffuse_color", frame=frame)
    action = material.animation_data.action
    action.name = f"{PREFIX}_MAGlow"
    scene.frame_set(1)
    material.diffuse_color = (0.2, 0.2, 0.2, 1.0)

    target = create_cube(f"{PREFIX}_MAPlayer")
    other = create_cube(f"{PREFIX}_MAOther")
    other.location = (3.0, 0.0, 0.0)
    template = create_cube(f"{PREFIX}_MATemplate")
    for obj in (target, other, template):
        obj.data.materials.append(material)
    library = make_ui_collection(f"{PREFIX}_MALibrary", (template,))
    library.hide_viewport = True

    tree = new_logic_tree(f"{PREFIX}_MATree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    play = tree.nodes.new("LogicNativePlayAction")
    play.action = action
    play.inputs["Start Frame"].default_value = 1.0
    play.inputs["End Frame"].default_value = 5.0
    tree.links.new(on_init.outputs["Out"], play.inputs["Flow"])

    add_status_controller(target, status_path, "material_actions", required_frame=1,
                          required_seconds=4.0)
    set_game_property(target, "__ln_acceptance_ma_material", "STRING", material.name)
    set_game_property(target, "__ln_acceptance_ma_other", "STRING", other.name)
    set_game_property(target, "__ln_acceptance_ma_template", "STRING", template.name)
    set_game_property(target, "__ln_acceptance_ma_action", "STRING", action.name)
    set_game_property(target, "ma_phase", "INT", 0)
    bind_tree(target, tree.name, True)
    target_name = target.name
    material_name = material.name
    run_game_once(target, "material_actions_0", status_path)

    if RUNNER == "view3d":
        # The engine put the material back (no undo restored the file).
        target = bpy.data.objects[target_name]
        slot = target.material_slots[0]
        require(slot.material is not None and slot.material.name == material_name and
                slot.link == "DATA",
                f"material_actions: the player's slot was not restored: {slot.material} {slot.link}")
        leftovers = [m.name for m in bpy.data.materials if m.name.startswith(material_name + ".")]
        require(not leftovers, f"material_actions: material copies left after the game: {leftovers}")


def run_property_branch_acceptance(status_path, output_dir):
    expected = (0.0, 4.0, 0.0)

    def build_case(case_status_path):
        tree = build_property_branch_tree(f"{PREFIX}_PropertyBranchTree", expected)
        target = create_cube(f"{PREFIX}_PropertyBranch")
        add_status_controller(target, case_status_path, "property_branch", required_frame=2)
        set_expected_vector(target, expected)
        set_game_property(target, "enabled", "BOOL", True)
        set_game_property(target, "moved", "BOOL", False)
        bind_tree(target, tree.name, True)
        return target

    run_case_twice_and_after_reload(status_path, output_dir, "property_branch", build_case)


def run_property_write_acceptance(status_path):
    def build_case():
        reset_scene()
        configure_game_settings(True)
        tree = build_property_writes_tree(f"{PREFIX}_PropertyWritesTree")
        target = create_cube(f"{PREFIX}_PropertyWrites")
        add_status_controller(target, status_path, "property_writes", required_frame=2)
        set_game_property(target, "score", "FLOAT", 0.0)
        set_game_property(target, "done", "BOOL", False)
        set_game_property(target, "state", "STRING", "idle")
        bind_tree(target, tree.name, True)
        return target

    target = build_case()
    run_game_once(target, "property_writes_0", status_path)

    target = build_case()
    run_game_once(target, "property_writes_1", status_path)


def run_property_target_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    owner = create_cube(f"{PREFIX}_PropertyTargetOwner")
    target = create_cube(f"{PREFIX}_PropertyTarget")
    target.location = (2.0, 0.0, 0.0)
    tree = build_property_target_tree(f"{PREFIX}_PropertyTargetTree", target)
    add_status_controller(owner, status_path, "property_target", required_frame=2)
    # Game properties: the status check runs in game, where ID properties are not visible.
    set_game_property(owner, "__ln_acceptance_property_target", "STRING", target.name)
    set_game_property(owner, "__ln_acceptance_expected_score", "INT", 42)
    set_game_property(target, "score", "INT", 0)
    bind_tree(owner, tree.name, True)
    run_game_once(owner, "property_target_0", status_path)


def run_property_advanced_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    tree = build_property_advanced_tree(f"{PREFIX}_PropertyAdvancedTree")
    target = create_cube(f"{PREFIX}_PropertyAdvanced")
    add_status_controller(target, status_path, "property_advanced", required_frame=2)
    set_game_property(target, "enabled", "BOOL", True)
    set_game_property(target, "has_hit", "BOOL", False)
    set_game_property(target, "speed", "FLOAT", 1.0)
    set_game_property(target, "throttle", "FLOAT", 1.0)
    set_game_property(target, "choice", "FLOAT", 0.0)
    set_game_property(target, "stored", "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "property_advanced_0", status_path)


def run_tree_property_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    tree = build_tree_property_tree(f"{PREFIX}_TreePropertyTree")
    target = create_cube(f"{PREFIX}_TreeProperty")
    add_status_controller(target, status_path, "tree_properties", required_frame=2)
    set_game_property(target, "tree_value", "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "tree_properties_0", status_path)


def run_bone_read_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    armature = create_armature(f"{PREFIX}_Armature")
    tree = build_bone_read_tree(f"{PREFIX}_BoneReadTree", armature)
    add_status_controller(armature, status_path, "bone_read", required_frame=2)
    set_game_property(armature, "bone_length", "FLOAT", -1.0)
    set_game_property(armature, "bone_head_z", "FLOAT", -1.0)
    set_game_property(armature, "bone_center_z", "FLOAT", -1.0)
    bind_tree(armature, tree.name, True)
    run_game_once(armature, "bone_read_0", status_path)


def run_bone_parent_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    armature = create_armature(f"{PREFIX}_BoneParentArmature")
    child = create_cube(f"{PREFIX}_BoneParentChild", rigid_body=True)
    child.location = (0.0, 0.0, 1.0)
    tree = build_bone_parent_tree(f"{PREFIX}_BoneParentTree", armature, child)

    add_status_controller(armature, status_path, "bone_parent", required_frame=5, tolerance=0.001)
    set_game_property(armature, "__ln_acceptance_child", "STRING", child.name)
    set_game_property(armature, "__ln_acceptance_parent", "STRING", armature.name)
    set_expected_named_vector(armature, "bone_parent", (0.5, 0.0, 1.0))
    bind_tree(armature, tree.name, True)
    run_game_once(armature, "bone_parent_0", status_path)


def run_bone_pose_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    armature = create_armature(f"{PREFIX}_ArmaturePose")

    set_object_mode(armature, "EDIT")
    authored_bones = {
        "Reference": ((2.0, 0.0, 0.0), (2.3, 0.9, 0.7), 0.37),
        "ArmatureTarget": ((4.0, 0.0, 0.0), (4.0, 1.0, 1.0), 0.41),
        "WorldTarget": ((6.0, 0.0, 0.0), (6.0, -0.3, 1.0), -0.29),
        "TransformBone": ((8.0, 0.0, 0.0), (8.0, 0.0, 1.0), 0.13),
        "OffsetBone": ((10.0, 0.0, 0.0), (10.4, 0.8, 0.5), 0.22),
    }
    for bone_name, (head, tail, roll) in authored_bones.items():
        bone = armature.data.edit_bones.new(bone_name)
        bone.head = head
        bone.tail = tail
        bone.roll = roll
    reference_edit_bone = armature.data.edit_bones["Reference"]
    for bone_name in ("ArmatureTarget", "WorldTarget", "TransformBone", "OffsetBone"):
        armature.data.edit_bones[bone_name].parent = reference_edit_bone
    set_object_mode(armature, "OBJECT")

    # Exercise Blender's complete pose-channel rotation API surface. The game-runtime node stores
    # a quaternion internally, while BKE_pchan_quat_to_rot()/BKE_pchan_rot_to_quat() must preserve
    # the requested orientation for every authored Blender storage mode.
    armature.pose.bones["Bone"].rotation_mode = "XYZ"
    armature.pose.bones["ArmatureTarget"].rotation_mode = "QUATERNION"
    armature.pose.bones["WorldTarget"].rotation_mode = "AXIS_ANGLE"
    armature.pose.bones["TransformBone"].rotation_mode = "ZYX"

    armature_rotation = (0.17, -0.23, 0.31)
    armature_scale = (1.2, 0.8, 1.4)
    armature.rotation_mode = "XYZ"
    armature.rotation_euler = armature_rotation
    armature.scale = armature_scale
    # Non-uniform parent-channel scale plus differently oriented child rest bones exercises
    # inherited scale/shear removal in the native orientation Get/Set paths.
    armature.pose.bones["Reference"].scale = (4.0, 0.12, 2.5)
    bpy.context.view_layer.update()

    tree = build_bone_pose_tree(f"{PREFIX}_BonePoseTree", armature)
    add_status_controller(armature, status_path, "bone_pose", required_frame=5, tolerance=0.001)

    observed_prefixes = (
        "reference_channel",
        "reference_native",
        "reference_object",
        "channel_rotation",
        "channel_scale",
        "armature_rotation",
        "world_rotation",
        "transform_location",
        "transform_rotation",
        "transform_scale",
        "offset_rest_head",
        "offset_pose_head",
    )
    for prefix in observed_prefixes:
        for axis in "xyz":
            set_game_property(armature, f"{prefix}_{axis}", "FLOAT", 0.0)

    reference_native = armature.pose.bones["Reference"].matrix.to_quaternion()
    default_bone_basis = mathutils.Quaternion((1.0, 0.0, 0.0), math.pi * 0.5)
    reference_object = reference_native @ default_bone_basis.conjugated()
    reference_world_object = armature.rotation_euler.to_quaternion() @ reference_object
    set_expected_named_vector(
        armature, "reference_native", reference_native.to_euler("XYZ"))
    set_expected_named_vector(
        armature, "reference_object", reference_world_object.to_euler("XYZ"))
    set_expected_named_vector(armature, "channel_rotation", (0.12, -0.21, 0.33))
    set_expected_named_vector(armature, "channel_scale", (1.25, 0.75, 1.5))
    set_expected_named_vector(armature, "armature_rotation", (0.24, -0.16, 0.41))
    set_expected_named_vector(armature, "world_rotation", (-0.19, 0.28, -0.34))
    set_expected_named_vector(armature, "transform_location", (2.1, -1.4, 0.8))
    set_expected_named_vector(armature, "transform_rotation", (-0.14, 0.22, -0.31))
    set_expected_named_vector(armature, "transform_scale", (1.0, 1.0, 1.0))

    bind_tree(armature, tree.name, True)
    armature_name = armature.name
    inherit_scale_modes = (
        "FULL",
        "FIX_SHEAR",
        "ALIGNED",
        "AVERAGE",
        "NONE",
        "NONE_LEGACY",
    )
    child_bone_names = ("ArmatureTarget", "WorldTarget", "TransformBone", "OffsetBone")
    for inherit_scale_mode in inherit_scale_modes:
        armature = resolve_live_object(armature_name)
        for pose_bone in armature.pose.bones:
            pose_bone.matrix_basis.identity()
        armature.pose.bones["Reference"].scale = (4.0, 0.12, 2.5)
        for bone_name in child_bone_names:
            armature.data.bones[bone_name].inherit_scale = inherit_scale_mode
        bpy.context.view_layer.update()

        offset_pose_bone = armature.pose.bones["OffsetBone"]
        offset_rest_bone = armature.data.bones["OffsetBone"]
        offset_direction = offset_pose_bone.matrix.to_3x3().col[1].normalized()
        armature_world_linear = armature.matrix_world.to_3x3()
        # A scaled parent can move a non-connected child's zero-channel pose head away from its
        # rest head. Include that pre-existing evaluated displacement before adding the requested
        # +Y native bone-axis offset; otherwise the test incorrectly attributes parent deformation
        # to the node.
        baseline_delta = armature_world_linear @ (
            offset_pose_bone.head - offset_rest_bone.head_local)
        expected_offset_delta = (
            baseline_delta + armature_world_linear @ (offset_direction * 0.5))
        set_expected_named_vector(armature, "offset_delta", expected_offset_delta)

        run_game_once(
            armature,
            f"bone_pose_{inherit_scale_mode.lower()}",
            status_path,
        )


def run_data_containers_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    tree = build_data_containers_tree(f"{PREFIX}_DataContainersTree")
    target = create_cube(f"{PREFIX}_DataContainers")
    add_status_controller(target, status_path, "data_containers", required_frame=2)
    set_game_property(target, "list_len", "INT", 0)
    set_game_property(target, "dict_len", "INT", 0)
    set_game_property(target, "dict_keys_len", "INT", 0)
    set_game_property(target, "dict_answer", "FLOAT", 0.0)
    set_game_property(target, "loop_count", "FLOAT", 0.0)
    set_game_property(target, "empty_list_len", "INT", -1)
    set_game_property(target, "empty_dict_len", "INT", -1)
    set_game_property(target, "list_sum", "FLOAT", 0.0)
    set_game_property(target, "list_item_value", "FLOAT", 0.0)
    set_game_property(target, "has_answer_key", "BOOL", False)
    set_game_property(target, "has_missing_key", "BOOL", False)
    set_game_property(target, "list_dup_len", "INT", -1)
    set_game_property(target, "merged_dict_len", "INT", -1)
    set_game_property(target, "list_has_two", "BOOL", False)
    set_game_property(target, "list_has_nine", "BOOL", False)
    set_game_property(target, "list_has_nested", "BOOL", False)
    set_game_property(target, "typed_list_len", "INT", -1)
    set_game_property(target, "typed_list_sum", "FLOAT", 0.0)
    set_game_property(target, "containers_ok", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "data_containers_0", status_path)


def build_input_map_tree(name, data_dir):
    """Save, load and reset the player's Input Map, name bindings, and let a rebind time out."""
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    operations = {}
    for key, operation, file_name in (
        ("saved", "SAVE", "ln_input_map_probe"),
        ("loaded", "LOAD", "ln_input_map_probe"),
        ("reset", "RESET", "ln_input_map_probe"),
        ("missing", "LOAD", "ln_input_map_missing"),
    ):
        node = tree.nodes.new("LogicNativeInputMap")
        # Reset hides Path and File: fill them in first.
        node.inputs["Path"].default_value = data_dir
        node.inputs["File"].default_value = file_name
        node.input_map_operation = operation
        operations[key] = node
    # Save, then Load, then Reset, then Load a file that does not exist.
    tree.links.new(on_init.outputs["Out"], operations["saved"].inputs["Flow"])
    tree.links.new(operations["saved"].outputs["Done"], operations["loaded"].inputs["Flow"])
    tree.links.new(operations["loaded"].outputs["Done"], operations["reset"].inputs["Flow"])
    tree.links.new(operations["reset"].outputs["Done"], operations["missing"].inputs["Flow"])

    def record_bool(source_flow, property_name, value_socket=None):
        setter = tree.nodes.new("LogicNativeSetGamePropertyBool")
        setter.inputs["Property"].default_value = property_name
        setter.inputs["Value"].default_value = True
        tree.links.new(source_flow, flow_input(setter))
        if value_socket is not None:
            tree.links.new(value_socket, setter.inputs["Value"])

    for key in ("saved", "loaded", "reset"):
        record_bool(operations[key].outputs["Done"], key, operations[key].outputs["Success"])
    record_bool(operations["missing"].outputs["Done"], "missing_done")
    record_bool(operations["missing"].outputs["Done"], "missing_loaded",
                operations["missing"].outputs["Success"])

    # Nothing presses a key: the rebind gives up after its timeout.
    rebind = tree.nodes.new("LogicNativeRebindInputAction")
    rebind.inputs["Action"].default_value = "Jump"
    rebind.inputs["Timeout"].default_value = 0.2
    tree.links.new(on_init.outputs["Out"], rebind.inputs["Flow"])
    record_bool(rebind.outputs["Done"], "rebind_done")
    record_bool(rebind.outputs["Done"], "rebind_success", rebind.outputs["Success"])

    for property_name, action, device in (
        ("jump_key", "Jump", "KEYBOARD_MOUSE"),
        ("jump_button", "Jump", "GAMEPAD"),
        ("accept_auto", "Accept", "AUTOMATIC"),
    ):
        binding = tree.nodes.new("LogicNativeGetInputBinding")
        binding.inputs["Action"].default_value = action
        binding.input_device = device
        setter = tree.nodes.new("LogicNativeSetGamePropertyString")
        setter.inputs["Property"].default_value = property_name
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(setter))
        tree.links.new(binding.outputs["Text"], setter.inputs["Value"])

    input_action = tree.nodes.new("LogicNativeInputAction")
    input_action.inputs["Action"].default_value = "Jump"
    record_bool(on_fixed_update.outputs["Out"], "jump_active", input_action.outputs["Active"])
    record_bool(input_action.outputs["Out"], "jump_triggered")
    return tree


def run_input_map_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    game_settings = bpy.context.scene.game_settings
    jump = game_settings.input_actions.get("Jump") or game_settings.input_actions.new(name="Jump")
    while len(jump.bindings):
        jump.bindings.remove(jump.bindings[0])
    jump.bindings.new(device="KEYBOARD").key = "SPACE"
    jump.bindings.new(device="GAMEPAD").gamepad_control = "A_CROSS"
    data_dir = status_path.parent / "input_map_data"
    saved_file = data_dir / "ln_input_map_probe.json"
    try:
        saved_file.unlink()
    except FileNotFoundError:
        pass
    tree = build_input_map_tree(f"{PREFIX}_InputMapTree", str(data_dir))
    target = create_cube(f"{PREFIX}_InputMap")
    add_status_controller(target, status_path, "input_map", required_frame=90)
    set_game_property(target, "__ln_acceptance_expected_path", "STRING", str(saved_file))
    for name in ("saved", "loaded", "reset", "missing_done", "rebind_done", "jump_triggered"):
        set_game_property(target, name, "BOOL", False)
    for name in ("missing_loaded", "rebind_success", "jump_active"):
        set_game_property(target, name, "BOOL", True)
    for name in ("jump_key", "jump_button", "accept_auto"):
        set_game_property(target, name, "STRING", "")
    bind_tree(target, tree.name, True)
    run_game_once(target, "input_map_0", status_path)


# The Inventory node's own rules (its Override section): given, they need Override on.
INVENTORY_RULE_INPUTS = {"Max Stack", "Stack", "Capacity", "Recipes", "Library", "Keep Places"}

def run_inventory_recipes_acceptance(status_path):
    """Inventory Move To Slot and Craft, Get Recipe Info and Find Recipe, compiled and run by the
    engine: recipe lines in a List property, an item library's max_stack, and each outcome."""
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Recipes")
    coin = create_cube("Coin")
    set_game_property(coin, "max_stack", "INT", 6)
    library = make_ui_collection(f"{PREFIX}_RecipeLibrary", (coin,))
    library.hide_viewport = True

    set_active_object(target)
    with bpy.context.temp_override(object=target, active_object=target, selected_objects=[target],
                                   selected_editable_objects=[target]):
        require(bpy.ops.object.game_property_new(type="LIST", name="recipes") == {"FINISHED"},
                "Could not add the recipes list")
    recipes = target.game.properties["recipes"]
    recipes.item_type = "STRING"
    index = list(target.game.properties).index(recipes)
    for text in ("Apple + Mushroom = Potion", "Gem + Coin = Key"):
        with bpy.context.temp_override(object=target, active_object=target,
                                       selected_objects=[target],
                                       selected_editable_objects=[target]):
            bpy.ops.object.game_property_item_add(index=index)
        recipes.elements[-1].value = text
    results = {"move_outcome": "", "move_made": "", "craft_outcome": "", "missing_outcome": "",
               "stack_outcome": "", "station": ""}
    for name, value in results.items():
        set_game_property(target, name, "STRING", value)
    for name in ("potions", "keys", "coins", "stacked_first", "arrows", "found_index",
                 "typed_arrows", "crafted_times", "craftable", "byproducts", "buckets",
                 "tool_butter"):
        set_game_property(target, name, "INT", -1)
    set_game_property(target, "can_craft", "BOOL", False)
    set_game_property(target, "recipes_done", "BOOL", False)

    tree = new_logic_tree(f"{PREFIX}_RecipesTree")
    nodes = tree.nodes
    flow = nodes.new("LogicNativeOnInit").outputs["Out"]

    # Each Inventory node edits a property in place: a scenario is a property, and every result is
    # stored right after the steps it is about.
    def inventory(prop, operation, **inputs):
        nonlocal flow
        node = nodes.new("LogicNativeInventory")
        node.operation = operation
        node.inputs["Property"].default_value = prop
        for name, value in inputs.items():
            node.inputs[name].default_value = value
        # The node's own rules instead of the list's Inventory Rules.
        if INVENTORY_RULE_INPUTS & inputs.keys():
            node.inputs["Override"].default_value = True
        tree.links.new(flow, node.inputs["Flow"])
        flow = node.outputs["Done"]
        return node

    def add(prop, item, amount):
        return inventory(prop, "ADD_ITEM", Item=item, Amount=amount)

    def current(prop):
        node = nodes.new("LogicNativeGetGameProperty")
        node.inputs["Property"].default_value = prop
        return node.outputs["Value"]

    def count(item, source):
        node = nodes.new("LogicNativeItemCount")
        node.inputs["Item"].default_value = item
        tree.links.new(source, node.inputs["Inventory"])
        return node.outputs["Count"]

    def store(name, value):
        nonlocal flow
        setter = nodes.new("LogicNativeSetGameProperty")
        setter.inputs["Property"].default_value = name
        tree.links.new(flow, setter.inputs["Flow"])
        tree.links.new(value, setter.inputs["Value"])
        flow = setter.outputs["Done"]

    recipe_list = nodes.new("LogicNativeGetGameProperty")
    recipe_list.inputs["Property"].default_value = "recipes"
    # [Apple 2, Mushroom 1, Gem 1, Coin 5]; an Apple dropped on the Mushroom makes a Potion.
    for item, amount in (("Apple", 2), ("Mushroom", 1), ("Gem", 1), ("Coin", 5)):
        add("bag", item, amount)
    move = inventory("bag", "MOVE_TO_SLOT", Slot=0)
    move.inputs["Other Slot"].default_value = 1
    tree.links.new(recipe_list.outputs["Value"], move.inputs["Recipes"])
    move.inputs["Override"].default_value = True
    store("move_outcome", move.outputs["Outcome"])
    store("move_made", move.outputs["Made"])
    store("potions", count("Potion", current("bag")))
    key_info = nodes.new("LogicNativeGetRecipeInfo")
    key_info.inputs["Recipe"].default_value = "Gem + Coin = Key"
    tree.links.new(current("bag"), key_info.inputs["Inventory"])
    store("can_craft", key_info.outputs["Can Craft"])
    info = nodes.new("LogicNativeGetRecipeInfo")
    info.inputs["Recipe"].default_value = "2 Wood + Stone = 3 Arrow @ Bench"
    # Typed inputs read the same outputs: a Branch on Can Craft, an integer property.
    branch = nodes.new("LogicNativeBranch")
    tree.links.new(flow, flow_input(branch))
    tree.links.new(key_info.outputs["Can Craft"], branch.inputs["Condition"])
    typed = nodes.new("LogicNativeSetGamePropertyInt")
    typed.inputs["Property"].default_value = "typed_arrows"
    tree.links.new(branch.outputs["True"], flow_input(typed))
    tree.links.new(info.outputs["Count"], typed.inputs["Value"])
    flow = typed.outputs["Done"]
    find = nodes.new("LogicNativeFindRecipe")
    tree.links.new(recipe_list.outputs["Value"], find.inputs["Recipes"])
    tree.links.new(key_info.outputs["Ingredients"], find.inputs["Items"])
    store("found_index", find.outputs["Index"])
    store("arrows", info.outputs["Count"])
    store("station", info.outputs["Station"])
    craft = inventory("bag", "CRAFT", Recipe="Gem + Coin = Key")
    store("craft_outcome", craft.outputs["Outcome"])
    store("keys", count("Key", current("bag")))
    store("coins", count("Coin", current("bag")))
    missing = inventory("bag", "CRAFT", Recipe="Apple + Mushroom = Potion")
    store("missing_outcome", missing.outputs["Outcome"])
    # [Coin 5, Coin 3]: the library limits a Coin stack to 6, so 1 joins and 2 stay.
    add("coins_bag", "Coin", 8)
    inventory("coins_bag", "SPLIT_STACK", Slot=0, Amount=3)
    stack = inventory("coins_bag", "MOVE_TO_SLOT", Slot=1)
    stack.inputs["Other Slot"].default_value = 0
    stack.inputs["Library"].default_value = library
    stack.inputs["Override"].default_value = True
    store("stack_outcome", stack.outputs["Outcome"])
    first = inventory("coins_bag", "REMOVE_FROM_SLOT", Slot=0, Amount=99)
    store("stacked_first", first.outputs["Removed"])
    # Craft as many as the ingredients allow (Times 0): [Wood 10, Stone 3] makes 3 Axes.
    add("wood_bag", "Wood", 10)
    add("wood_bag", "Stone", 3)
    many_info = nodes.new("LogicNativeGetRecipeInfo")
    many_info.inputs["Recipe"].default_value = "2 Wood + Stone = Axe"
    tree.links.new(current("wood_bag"), many_info.inputs["Inventory"])
    store("craftable", many_info.outputs["Craftable"])
    many = inventory("wood_bag", "CRAFT", Recipe="2 Wood + Stone = Axe", Amount=0)
    store("crafted_times", many.outputs["Removed"])
    # A recipe that makes two things.
    milk_recipe = "Milk Bucket = Butter + Bucket"
    add("milk_bag", "Milk Bucket", 1)
    inventory("milk_bag", "CRAFT", Recipe=milk_recipe)
    milk_info = nodes.new("LogicNativeGetRecipeInfo")
    milk_info.inputs["Recipe"].default_value = milk_recipe
    store("byproducts", count("Bucket", milk_info.outputs["Results"]))
    store("buckets", count("Bucket", current("milk_bag")))
    # A tool the recipe gives back is only needed at hand: one Bucket makes 5 Butter.
    add("tool_bag", "Bucket", 1)
    add("tool_bag", "Milk", 5)
    inventory("tool_bag", "CRAFT", Recipe="Bucket + Milk = Butter + Bucket", Amount=0)
    store("tool_butter", count("Butter", current("tool_bag")))
    done = nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Property"].default_value = "recipes_done"
    done.inputs["Value"].default_value = True
    tree.links.new(flow, flow_input(done))

    add_status_controller(target, status_path, "inventory_recipes", required_frame=10)
    bind_tree(target, tree.name, True)
    set_active_object(target)
    run_game_once(target, "inventory_recipes_0", status_path)


def run_inventory_rule_sets_acceptance(status_path):
    """Inventory Rules: lists that pick one of the scene's sets follow it whatever node changes
    them; a value can come from a game property of the list's own object (Capacity from
    bag_size); Override uses the node's own rules; a list's Accept Categories refuse what it may
    not hold, also with Override, from Add Item and from Move To Slot."""
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Rules")
    other = create_cube(f"{PREFIX}_RulesOther")
    other.location = (0.0, 4.0, 0.0)
    apple = create_cube("Apple")
    sword = create_cube("Sword")
    set_game_property(apple, "category", "STRING", "food")
    set_game_property(sword, "category", "STRING", "weapon")
    library = make_ui_collection(f"{PREFIX}_RulesLibrary", (apple, sword))
    library.hide_viewport = True
    game_settings = bpy.context.scene.game_settings
    game_settings.item_library = library
    # Both bags: no stacking, 2 places unless their object's bag_size says more.
    bag = game_settings.inventory_rules.new(name="Bag")
    bag.use_stack = False
    bag.capacity = 2
    bag.capacity_from = "bag_size"
    weapons = game_settings.inventory_rules.new(name="Weapons")
    weapons.accept_categories = "Weapon"

    def add_list(obj, name, rules):
        set_active_object(obj)
        with bpy.context.temp_override(
            object=obj,
            active_object=obj,
            selected_objects=[obj],
            selected_editable_objects=[obj],
            scene=bpy.context.scene,
            view_layer=bpy.context.view_layer,
        ):
            result = bpy.ops.object.game_property_new(type='TABLE', name=name)
        require(result == {"FINISHED"}, f"Failed to add the list {name!r}: {result}")
        obj.game.properties[name].inventory_rules = rules

    add_list(target, "inventory", "Bag")
    add_list(target, "pouch", "Bag")
    add_list(target, "weapon", "Weapons")
    add_list(other, "inventory", "Bag")
    set_game_property(other, "bag_size", "INT", 3)
    for name in ("target_left", "other_left", "pouch_left", "refused_left", "sword_left"):
        set_game_property(target, name, "INT", -1)
    for name in ("refused_outcome", "move_outcome"):
        set_game_property(target, name, "STRING", "")
    set_game_property(target, "rules_done", "BOOL", False)

    tree = new_logic_tree(f"{PREFIX}_RulesTree")
    nodes = tree.nodes
    flow = nodes.new("LogicNativeOnInit").outputs["Out"]

    def inventory(operation, prop, **inputs):
        nonlocal flow
        node = nodes.new("LogicNativeInventory")
        node.operation = operation
        node.inputs["Property"].default_value = prop
        for name, value in inputs.items():
            node.inputs[name].default_value = value
        tree.links.new(flow, node.inputs["Flow"])
        flow = node.outputs["Done"]
        return node

    def store(name, output):
        nonlocal flow
        setter = nodes.new("LogicNativeSetGameProperty")
        setter.inputs["Property"].default_value = name
        tree.links.new(flow, setter.inputs["Flow"])
        tree.links.new(output, setter.inputs["Value"])
        flow = setter.outputs["Done"]

    # The target's bag: 2 places (no bag_size), no stacking: 2 Apples go in, 1 is left.
    mine = inventory("ADD_ITEM", "inventory", Item="Apple", Amount=3)
    store("target_left", mine.outputs["Leftover"])
    # The same set on the other object: its own bag_size gives 3 places.
    theirs = inventory("ADD_ITEM", "inventory", Item="Apple", Amount=3, Object=other)
    store("other_left", theirs.outputs["Leftover"])
    # Override: the node's Stack and no Capacity: all 3 in one stack.
    pouch = inventory("ADD_ITEM", "pouch", Item="Apple", Amount=3, Override=True)
    store("pouch_left", pouch.outputs["Leftover"])
    # The weapon list takes no apple, Override or not; it takes the sword.
    refused = inventory("ADD_ITEM", "weapon", Item="Apple", Amount=1, Override=True)
    store("refused_left", refused.outputs["Leftover"])
    store("refused_outcome", refused.outputs["Outcome"])
    taken = inventory("ADD_ITEM", "weapon", Item="Sword", Amount=1)
    store("sword_left", taken.outputs["Leftover"])
    # Moving an Apple from the bag into the weapon list is refused too.
    move = inventory("MOVE_TO_SLOT", "inventory", Slot=0, **{"Other Slot": 1,
                                                            "To Property": "weapon"})
    store("move_outcome", move.outputs["Outcome"])
    done = nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Property"].default_value = "rules_done"
    done.inputs["Value"].default_value = True
    tree.links.new(flow, flow_input(done))

    add_status_controller(target, status_path, "inventory_rule_sets", required_frame=12)
    bind_tree(target, tree.name, True)
    set_active_object(target)
    run_game_once(target, "inventory_rule_sets_0", status_path)


def run_inventory_in_place_acceptance(status_path):
    """The Inventory node edits an object's inventory property in place, the scene's Item Library
    gives stack limits, a pickup object is its own item and count, Amount 0 means all, and Drop
    Item puts a library item with its Amount in the world, from On Init and from a frame route."""
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_InPlace")
    coin = create_cube("Coin")
    gem = create_cube("Gem")
    for obj, item, stack in ((coin, "Coin", 6), (gem, "Gem", 10)):
        set_game_property(obj, "item", "STRING", item)
        set_game_property(obj, "count", "INT", 1)
        set_game_property(obj, "max_stack", "INT", stack)
    library = make_ui_collection(f"{PREFIX}_InPlaceLibrary", (coin, gem))
    library.hide_viewport = True
    bpy.context.scene.game_settings.item_library = library
    pickup = create_cube(f"{PREFIX}_PickupGem")
    pickup.location = (0.0, 6.0, 0.0)
    set_game_property(pickup, "item", "STRING", "Gem")
    set_game_property(pickup, "count", "INT", 3)
    for name in ("leftover2", "removed_whole", "coins", "gems", "added2", "added_pickup",
                 "added_partial", "leftover_partial"):
        set_game_property(target, name, "INT", -1)
    set_game_property(target, "inventory_done", "BOOL", False)

    tree = new_logic_tree(f"{PREFIX}_InPlaceTree")
    nodes = tree.nodes
    flow = nodes.new("LogicNativeOnInit").outputs["Out"]

    def inventory(operation, **inputs):
        nonlocal flow
        node = nodes.new("LogicNativeInventory")
        node.operation = operation
        for name, value in inputs.items():
            node.inputs[name].default_value = value
        # The node's own rules instead of the list's Inventory Rules.
        if INVENTORY_RULE_INPUTS & inputs.keys():
            node.inputs["Override"].default_value = True
        tree.links.new(flow, node.inputs["Flow"])
        flow = node.outputs["Done"]
        return node

    def store(name, output):
        nonlocal flow
        setter = nodes.new("LogicNativeSetGameProperty")
        setter.inputs["Property"].default_value = name
        tree.links.new(flow, setter.inputs["Flow"])
        tree.links.new(output, setter.inputs["Value"])
        flow = setter.outputs["Done"]

    inventory("ADD_ITEM", Item="Coin", Amount=4)
    second = inventory("ADD_ITEM", Item="Coin", Amount=4)
    store("leftover2", second.outputs["Leftover"])
    picked = inventory("ADD_ITEM", Amount=0)
    by_name = nodes.new("LogicNativeObjectByName")
    by_name.inputs["Name"].default_value = pickup.name
    tree.links.new(by_name.outputs["Object"], picked.inputs["Item"])
    whole = inventory("REMOVE_FROM_SLOT", Slot=0, Amount=0)
    store("removed_whole", whole.outputs["Removed"])
    current = nodes.new("LogicNativeGetGameProperty")
    current.inputs["Property"].default_value = "inventory"
    for name, item in (("coins", "Coin"), ("gems", "Gem")):
        count = nodes.new("LogicNativeItemCount")
        count.inputs["Item"].default_value = item
        tree.links.new(current.outputs["Value"], count.inputs["Inventory"])
        store(name, count.outputs["Count"])
    # Add Item's Added (the Removed socket): how many went in, also when only some fit.
    store("added2", second.outputs["Removed"])
    store("added_pickup", picked.outputs["Removed"])
    partial = inventory("ADD_ITEM", Item="Gem", Amount=9, Capacity=2)
    store("added_partial", partial.outputs["Removed"])
    store("leftover_partial", partial.outputs["Leftover"])
    drop = nodes.new("LogicNativeDropItem")
    drop.inputs["Item"].default_value = "Gem"
    drop.inputs["Amount"].default_value = 2
    drop.inputs["Position"].default_value = (5.0, 0.0, 1.0)
    tree.links.new(flow, drop.inputs["Flow"])
    flow = drop.outputs["Done"]
    done = nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Property"].default_value = "inventory_done"
    done.inputs["Value"].default_value = True
    tree.links.new(flow, flow_input(done))
    # A drop from a frame route (it waits for the next Logic + Physics step).
    on_update = nodes.new("LogicNativeOnUpdate")
    once = nodes.new("LogicNativeOnce")
    tree.links.new(on_update.outputs["Out"], once.inputs["Flow"])
    frame_drop = nodes.new("LogicNativeDropItem")
    frame_drop.inputs["Item"].default_value = "Coin"
    frame_drop.inputs["Amount"].default_value = 3
    frame_drop.inputs["Position"].default_value = (7.0, 0.0, 1.0)
    tree.links.new(once.outputs["Out"], frame_drop.inputs["Flow"])

    add_status_controller(target, status_path, "inventory_in_place", required_frame=12)
    bind_tree(target, tree.name, True)
    set_active_object(target)
    run_game_once(target, "inventory_in_place_0", status_path)


def run_inventory_two_inventories_acceptance(status_path):
    """Move To Slot between two inventories: from a bag property onto a chest object's property
    (stacking onto the same item up to the stack limit, moving onto an empty place) and onto an
    equipment property of the same object (To Object empty) with Capacity 1."""
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_TwoBag")
    chest = create_cube(f"{PREFIX}_TwoChest")
    chest.location = (4.0, 0.0, 0.0)
    for name in ("o_stack", "o_move", "o_equip", "o_full"):
        set_game_property(target, name, "STRING", "")
    for name in ("bag_apples", "bag_slots", "chest_apples", "chest_swords", "weapon_apples"):
        set_game_property(target, name, "INT", -1)
    set_game_property(target, "two_done", "BOOL", False)

    tree = new_logic_tree(f"{PREFIX}_TwoTree")
    nodes = tree.nodes
    flow = nodes.new("LogicNativeOnInit").outputs["Out"]

    def inventory(operation, **inputs):
        nonlocal flow
        node = nodes.new("LogicNativeInventory")
        node.operation = operation
        for name, value in inputs.items():
            node.inputs[name].default_value = value
        # The node's own rules instead of the list's Inventory Rules.
        if INVENTORY_RULE_INPUTS & inputs.keys():
            node.inputs["Override"].default_value = True
        tree.links.new(flow, node.inputs["Flow"])
        flow = node.outputs["Done"]
        return node

    def store(name, output):
        nonlocal flow
        setter = nodes.new("LogicNativeSetGameProperty")
        setter.inputs["Property"].default_value = name
        tree.links.new(flow, setter.inputs["Flow"])
        tree.links.new(output, setter.inputs["Value"])
        flow = setter.outputs["Done"]

    def count(obj, prop, item):
        get = nodes.new("LogicNativeGetGameProperty")
        get.inputs["Object"].default_value = obj
        get.inputs["Property"].default_value = prop
        node = nodes.new("LogicNativeItemCount")
        node.inputs["Item"].default_value = item
        tree.links.new(get.outputs["Value"], node.inputs["Inventory"])
        return node.outputs["Count"]

    inventory("ADD_ITEM", Property="bag", Item="Apple", Amount=4)
    inventory("ADD_ITEM", Property="bag", Item="Sword", Amount=1)
    inventory("ADD_ITEM", Object=chest, Property="chest", Item="Apple", Amount=3)
    stacked = inventory("MOVE_TO_SLOT", Property="bag", Slot=0, **{
        "To Object": chest, "To Property": "chest", "Other Slot": 0, "Max Stack": 5})
    store("o_stack", stacked.outputs["Outcome"])
    moved = inventory("MOVE_TO_SLOT", Property="bag", Slot=1, **{
        "To Object": chest, "To Property": "chest", "Other Slot": 99})
    store("o_move", moved.outputs["Outcome"])
    equipped = inventory("MOVE_TO_SLOT", Property="bag", Slot=0, Capacity=1, **{
        "To Property": "weapon", "Other Slot": 0})
    store("o_equip", equipped.outputs["Outcome"])
    inventory("ADD_ITEM", Property="bag", Item="Gem", Amount=1)
    full = inventory("MOVE_TO_SLOT", Property="bag", Slot=0, Capacity=1, **{
        "To Property": "weapon", "Other Slot": 5})
    store("o_full", full.outputs["Outcome"])
    store("bag_apples", count(target, "bag", "Apple"))
    bag = nodes.new("LogicNativeGetGameProperty")
    bag.inputs["Property"].default_value = "bag"
    length = nodes.new("LogicNativeListLength")
    tree.links.new(bag.outputs["Value"], length.inputs["List"])
    store("bag_slots", length.outputs["Length"])
    store("chest_apples", count(chest, "chest", "Apple"))
    store("chest_swords", count(chest, "chest", "Sword"))
    store("weapon_apples", count(target, "weapon", "Apple"))
    done = nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Property"].default_value = "two_done"
    done.inputs["Value"].default_value = True
    tree.links.new(flow, flow_input(done))

    add_status_controller(target, status_path, "inventory_two_inventories", required_frame=12)
    bind_tree(target, tree.name, True)
    set_active_object(target)
    run_game_once(target, "inventory_two_inventories_0", status_path)


def run_ui_menu_replace_inside_acceptance(status_path):
    """Replace Top issued by a tree inside the menu it replaces (a tab button): the new menu takes
    over the replaced menu's owner and stays open, instead of closing with the old menu's tree."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    main_camera = scene.camera
    driver = create_cube(f"{PREFIX}_RTDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_RTOverlayCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    scene.camera = main_camera
    a_panel = create_cube(f"{PREFIX}_RTAPanel")
    a_panel.location = (-2.0, 0.0, 40.0)
    b_panel = create_cube(f"{PREFIX}_RTBPanel")
    b_panel.location = (2.0, 0.0, 40.0)
    menu_a = make_ui_collection(f"{PREFIX}_RTMenuA", (a_panel,))
    menu_b = make_ui_collection(f"{PREFIX}_RTMenuB", (b_panel,))

    tree = new_logic_tree(f"{PREFIX}_RTTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    open_a = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_a.inputs["Camera"].default_value = overlay_camera
    open_a.inputs["Collection"].default_value = menu_a
    tree.links.new(on_init.outputs["Out"], flow_input(open_a))

    tab_tree = new_logic_tree(f"{PREFIX}_RTTabTree")
    tab_init = tab_tree.nodes.new("LogicNativeOnInit")
    delay = tab_tree.nodes.new("LogicNativeDelay")
    delay.inputs["Delay"].default_value = 0.3
    open_b = tab_tree.nodes.new("LogicNativeOpenObjectMenu")
    open_b.menu_open_mode = "REPLACE_TOP"
    open_b.inputs["Camera"].default_value = overlay_camera
    open_b.inputs["Collection"].default_value = menu_b
    tab_tree.links.new(tab_init.outputs["Out"], flow_input(delay))
    tab_tree.links.new(delay.outputs["Out"], flow_input(open_b))
    bind_tree(a_panel, tab_tree.name, True)

    add_status_controller(driver, status_path, "ui_menu_replace_inside", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_menu_replace_inside_0")
    bind_tree(driver, tree.name, True)
    menu_a.hide_viewport = True
    menu_b.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_menu_replace_inside_0", status_path, prepare=False)


def run_ui_player_menu_acceptance(status_path):
    """A menu opened for player 1 belongs to that player: its UI Elements are player 1's, player 1
    gets its first control as focus and highlight (the Highlight event's User is 1) and its
    Highlight Object shows; player 0's focus stays empty. Text shows `{Accept}` as each player's
    button: gamepad A in player 1's menu, Return for player 0 outside menus."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    main_camera = scene.camera
    driver = create_cube(f"{PREFIX}_PMDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_PMOverlayCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    scene.camera = main_camera

    def make_child(name, parent, location):
        child = create_cube(name)
        child.location = location
        child.scale = (0.6, 0.6, 0.05)
        bpy.context.view_layer.update()
        child.parent = parent
        child.matrix_parent_inverse = parent.matrix_world.inverted()
        return child

    first = create_cube(f"{PREFIX}_PMFirst")
    first.location = (-2.0, 2.0, 40.0)
    first.scale = (0.5, 0.5, 0.5)
    second = create_cube(f"{PREFIX}_PMSecond")
    second.location = (2.0, 2.0, 40.0)
    second.scale = (0.5, 0.5, 0.5)
    glow = make_child(f"{PREFIX}_PMGlow", first, (-2.0, 2.0, 40.6))
    other_glow = make_child(f"{PREFIX}_PMOtherGlow", second, (2.0, 2.0, 40.6))
    for button, action, tab_order, frame in ((first, "first", 0, glow),
                                             (second, "second", 1, other_glow)):
        button.game.ui.use_element = True
        button.game.ui.action = action
        button.game.ui.tab_order = tab_order
        button.game.ui.highlight_object = frame
    bpy.ops.object.text_add(location=(0.0, -2.0, 40.0))
    hint = bpy.context.active_object
    hint.name = f"{PREFIX}_PMHint"
    hint.data.body = "{Accept} ok"
    menu = make_ui_collection(
        f"{PREFIX}_PMMenu", (first, second, glow, other_glow, hint))
    bpy.ops.object.text_add(location=(0.0, 0.0, 0.0))
    game_hint = bpy.context.active_object
    game_hint.name = f"{PREFIX}_PMGameHint"
    game_hint.data.body = "{Accept} ok"

    tree = new_logic_tree(f"{PREFIX}_PMTree")
    nodes = tree.nodes
    on_init = nodes.new("LogicNativeOnInit")
    open_menu = nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    open_menu.inputs["User"].default_value = 1
    tree.links.new(on_init.outputs["Out"], flow_input(open_menu))
    on_highlight = nodes.new("LogicNativeOnUIEvent")
    on_highlight.ui_event = "HIGHLIGHT"
    store_user = nodes.new("LogicNativeSetGameProperty")
    store_user.inputs["Property"].default_value = "hl_user"
    tree.links.new(on_highlight.outputs["Out"], store_user.inputs["Flow"])
    tree.links.new(on_highlight.outputs["User"], store_user.inputs["Value"])
    store_action = nodes.new("LogicNativeSetGameProperty")
    store_action.inputs["Property"].default_value = "hl_action"
    tree.links.new(store_user.outputs["Done"], store_action.inputs["Flow"])
    tree.links.new(on_highlight.outputs["Action"], store_action.inputs["Value"])
    on_update = nodes.new("LogicNativeOnUpdate")
    flow = on_update.outputs["Out"]
    for user, name in ((1, "focus1"), (0, "focus0")):
        get_focus = nodes.new("LogicNativeGetUIFocus")
        get_focus.inputs["User"].default_value = user
        to_name = nodes.new("LogicNativeTypecast")
        to_name.to_type = "STRING"
        setter = nodes.new("LogicNativeSetGamePropertyString")
        setter.inputs["Property"].default_value = name
        tree.links.new(get_focus.outputs["Object"], to_name.inputs["Value"])
        tree.links.new(to_name.outputs["String"], setter.inputs["Value"])
        tree.links.new(flow, flow_input(setter))
        flow = setter.outputs["Done"]

    set_game_property(driver, "hl_user", "INT", -1)
    set_game_property(driver, "hl_action", "STRING", "")
    set_game_property(driver, "focus1", "STRING", "")
    set_game_property(driver, "focus0", "STRING", "?")
    add_status_controller(driver, status_path, "ui_player_menu", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_player_menu_0")
    bind_tree(driver, tree.name, True)
    menu.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_player_menu_0", status_path, prepare=False)


def run_loop_routes_acceptance(status_path):
    """Routes inside a Loop body (a data node's Done, a Branch) run in every iteration, also when
    another route of the same event comes first in the tree."""
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_LoopRoutes")
    for name in ("c_other", "c_direct", "c_append", "c_inventory", "c_branch"):
        set_game_property(target, name, "FLOAT", 0.0)
    tree = new_logic_tree(f"{PREFIX}_LoopRoutesTree")
    nodes = tree.nodes
    on_init = nodes.new("LogicNativeOnInit").outputs["Out"]

    def counter(name, flow):
        node = nodes.new("LogicNativeModifyProperty")
        node.inputs["Property"].default_value = name
        node.inputs["Value"].default_value = 1.0
        tree.links.new(flow, node.inputs["Flow"])

    def loop_body():
        node = nodes.new("LogicNativeLoop")
        node.inputs["Count"].default_value = 3
        node.inputs["Ticks to Complete"].default_value = 1
        tree.links.new(on_init, node.inputs["Flow"])
        return node.outputs["Loop"]

    counter("c_other", on_init)
    counter("c_direct", loop_body())
    append = nodes.new("LogicNativeListAppend")
    tree.links.new(loop_body(), append.inputs["Flow"])
    counter("c_append", append.outputs["Done"])
    inventory = nodes.new("LogicNativeInventory")
    inventory.inputs["Item"].default_value = "Apple"
    tree.links.new(loop_body(), inventory.inputs["Flow"])
    counter("c_inventory", inventory.outputs["Done"])
    branch = nodes.new("LogicNativeBranch")
    branch.inputs["Condition"].default_value = True
    tree.links.new(loop_body(), branch.inputs["Flow"])
    counter("c_branch", branch.outputs["True"])

    add_status_controller(target, status_path, "loop_routes", required_frame=10)
    bind_tree(target, tree.name, True)
    set_active_object(target)
    run_game_once(target, "loop_routes_0", status_path)


def run_time_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    tree = build_time_state_tree(f"{PREFIX}_TimeStateTree")
    target = create_cube(f"{PREFIX}_TimeState")
    add_status_controller(target, status_path, "time_state", required_frame=8)
    for name in ("next_frame", "delay", "timer", "pulsify", "barrier"):
        set_game_property(target, name, "BOOL", False)
    set_game_property(target, "time_value", "FLOAT", 0.0)
    set_game_property(target, "delta_value", "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "time_state_0", status_path)


def run_scene_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    # Regression: a file that starts fully paused must still run On Update so a
    # C++ Logic Nodes Set Timescale command can resume fixed simulation.
    bpy.context.scene.game_settings.time_scale = 0.0
    gravity = (1.25, -2.5, -3.75)
    timescale = 1.25
    tree = build_scene_state_tree(f"{PREFIX}_SceneStateTree", gravity, timescale)
    target = create_cube(f"{PREFIX}_SceneState")
    add_status_controller(target, status_path, "scene_state", required_frame=8)
    set_expected_vector(target, gravity)
    set_game_property(target, "__ln_acceptance_expected_timescale", "FLOAT", timescale)
    for name in ("gravity_x", "gravity_y", "gravity_z", "timescale"):
        set_game_property(target, name, "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "scene_state_0", status_path)


def run_inactive_overlay_collection_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    main_camera = scene.camera

    driver = create_cube(f"{PREFIX}_InactiveOverlayDriver")
    overlay_item = create_cube(f"{PREFIX}_InactiveOverlayItem")
    overlay_child = create_cube(f"{PREFIX}_InactiveOverlayChild")
    overlay_child.parent = overlay_item
    set_game_property(overlay_item, "overlay_marker", "STRING", "replicated")
    set_game_property(overlay_child, "overlay_marker", "STRING", "replicated")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_InactiveOverlayCamera", (0.0, -6.0, 0.0), overlay_item.location)
    borrowed_camera = create_camera_looking_at(
        f"{PREFIX}_BorrowedOverlayCamera", (2.0, -6.0, 1.0), overlay_item.location)
    scene.camera = main_camera
    set_game_property(overlay_item, "partner", "OBJECT", overlay_camera)
    set_game_property(overlay_item, "part", "OBJECT", overlay_child)
    set_game_property(overlay_camera, "watch", "OBJECT", overlay_item)

    collection_name = f"{PREFIX}_InactiveOverlayCollection"
    previous_collection = bpy.data.collections.get(collection_name)
    if previous_collection is not None:
        bpy.data.collections.remove(previous_collection)
    overlay_collection = bpy.data.collections.new(collection_name)
    scene.collection.children.link(overlay_collection)
    child_collection_name = f"{collection_name}_Child"
    previous_child_collection = bpy.data.collections.get(child_collection_name)
    if previous_child_collection is not None:
        bpy.data.collections.remove(previous_child_collection)
    child_collection = bpy.data.collections.new(child_collection_name)
    overlay_collection.children.link(child_collection)
    for obj in (overlay_item, overlay_camera):
        overlay_collection.objects.link(obj)
        for source_collection in tuple(obj.users_collection):
            if source_collection != overlay_collection:
                source_collection.objects.unlink(obj)
    child_collection.objects.link(overlay_child)
    for source_collection in tuple(overlay_child.users_collection):
        if source_collection != child_collection:
            source_collection.objects.unlink(overlay_child)

    tree = build_inactive_overlay_collection_tree(
        f"{PREFIX}_InactiveOverlayTree", overlay_camera, overlay_collection)
    item_tree = new_logic_tree(f"{PREFIX}_InactiveOverlayItemTree")
    item_on_init = item_tree.nodes.new("LogicNativeOnInit")
    item_on_fixed = item_tree.nodes.new("LogicNativeOnFixedUpdate")
    set_item_init = item_tree.nodes.new("LogicNativeSetGamePropertyString")
    set_item_fixed = item_tree.nodes.new("LogicNativeSetGamePropertyString")
    set_item_init.inputs["Property"].default_value = "overlay_item_init"
    set_item_init.inputs["Value"].default_value = "ran"
    set_item_fixed.inputs["Property"].default_value = "overlay_item_fixed"
    set_item_fixed.inputs["Value"].default_value = "ran"
    item_tree.links.new(item_on_init.outputs["Out"], flow_input(set_item_init))
    item_tree.links.new(item_on_fixed.outputs["Out"], flow_input(set_item_fixed))
    bind_tree(overlay_item, item_tree.name, True)
    bind_tree(overlay_child, item_tree.name, True)
    camera_tree = new_logic_tree(f"{PREFIX}_InactiveOverlayCameraTree")
    camera_on_init = camera_tree.nodes.new("LogicNativeOnInit")
    camera_on_fixed = camera_tree.nodes.new("LogicNativeOnFixedUpdate")
    set_camera_init = camera_tree.nodes.new("LogicNativeSetGamePropertyString")
    set_camera_fixed = camera_tree.nodes.new("LogicNativeSetGamePropertyString")
    set_camera_init.inputs["Property"].default_value = "overlay_camera_init"
    set_camera_init.inputs["Value"].default_value = "ran"
    set_camera_fixed.inputs["Property"].default_value = "overlay_camera_fixed"
    set_camera_fixed.inputs["Value"].default_value = "ran"
    camera_tree.links.new(camera_on_init.outputs["Out"], flow_input(set_camera_init))
    camera_tree.links.new(camera_on_fixed.outputs["Out"], flow_input(set_camera_fixed))
    bind_tree(overlay_camera, camera_tree.name, True)
    set_game_property(driver, "overlay_result", "STRING", "pending")
    add_inactive_overlay_lifecycle_controller(
        driver,
        overlay_camera.name,
        borrowed_camera.name,
        overlay_collection.name,
        overlay_item.name,
    )
    add_status_controller(driver, status_path, "inactive_overlay_collection", required_frame=54)
    set_game_property(driver, "__ln_acceptance_expected_object", "STRING", overlay_item.name)
    set_game_property(driver, "__ln_acceptance_expected_child", "STRING", overlay_child.name)
    set_game_property(driver, "__ln_acceptance_expected_camera", "STRING", overlay_camera.name)
    set_game_property(
        driver, "__ln_acceptance_expected_collection", "STRING", overlay_collection.name)
    set_game_property(driver, "__ln_acceptance_expected_property", "STRING", "overlay_marker")
    set_game_property(driver, "__ln_acceptance_expected_string", "STRING", "replicated")
    set_game_property(
        driver, "__ln_acceptance_run", "STRING", "inactive_overlay_collection_0")
    bind_tree(driver, tree.name, True)

    # This reproduces the Outliner's Disable in Viewports state. Both the content and its
    # dedicated overlay camera must be converted as inactive templates, then accepted by the
    # Set Overlay Collection command without changing their authored visibility state.
    overlay_collection.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    require(overlay_collection.hide_viewport, "Overlay collection was not viewport-disabled")
    driver = run_game_once(
        driver, "inactive_overlay_collection_0", status_path, prepare=False)
    require(driver is not None, "Inactive overlay driver did not survive the first game run")
    # Regression for restart-time conversion state: the second embedded start deliberately makes
    # no editor, ID, collection, selection, or dependency-graph update before launching again.
    run_game_once(driver, "inactive_overlay_collection_0", status_path, prepare=False)


def run_object_menu_session_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    main_camera = scene.camera

    driver = create_cube(f"{PREFIX}_ObjectMenuDriver")
    menu_item = create_cube(f"{PREFIX}_ObjectMenuItem")
    detail_item = create_cube(f"{PREFIX}_ObjectMenuDetailItem")
    menu_camera = create_camera_looking_at(
        f"{PREFIX}_ObjectMenuCamera", (0.0, -6.0, 0.0), menu_item.location)
    scene.camera = main_camera

    collection_name = f"{PREFIX}_ObjectMenuCollection"
    previous_collection = bpy.data.collections.get(collection_name)
    if previous_collection is not None:
        bpy.data.collections.remove(previous_collection)
    menu_collection = bpy.data.collections.new(collection_name)
    scene.collection.children.link(menu_collection)
    detail_collection_name = f"{PREFIX}_ObjectMenuDetailCollection"
    previous_detail_collection = bpy.data.collections.get(detail_collection_name)
    if previous_detail_collection is not None:
        bpy.data.collections.remove(previous_detail_collection)
    detail_collection = bpy.data.collections.new(detail_collection_name)
    scene.collection.children.link(detail_collection)
    for obj, target_collection in (
        (menu_item, menu_collection),
        (menu_camera, menu_collection),
        (detail_item, detail_collection),
    ):
        target_collection.objects.link(obj)
        for source_collection in tuple(obj.users_collection):
            if source_collection != target_collection:
                source_collection.objects.unlink(obj)

    menu_tree = build_object_menu_session_tree(
        f"{PREFIX}_ObjectMenuSessionTree", menu_camera, menu_collection, detail_collection)
    item_tree = new_logic_tree(f"{PREFIX}_ObjectMenuItemTree")
    item_init = item_tree.nodes.new("LogicNativeOnInit")
    mark_item_logic = item_tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_item_logic.inputs["Object"].default_value = driver
    mark_item_logic.inputs["Property"].default_value = "menu_item_logic_ran"
    mark_item_logic.inputs["Value"].default_value = True
    item_tree.links.new(item_init.outputs["Out"], flow_input(mark_item_logic))
    bind_tree(menu_item, item_tree.name, True)

    for property_name, property_type, value in (
        ("menu_handle_first", "INT", 0),
        ("menu_handle_second", "INT", 0),
        ("menu_open_first_result", "INT", -1),
        ("menu_open_second_result", "INT", -1),
        ("menu_close_first_result", "INT", -1),
        ("menu_close_second_result", "INT", -1),
        ("menu_close_repeat_result", "INT", -1),
        ("menu_repeat_closed", "BOOL", False),
        ("menu_handle_external", "INT", 0),
        ("menu_open_external_result", "INT", -1),
        ("menu_close_external_result", "INT", -1),
        ("menu_external_ready", "BOOL", False),
        ("menu_external_removed", "BOOL", False),
        ("menu_session_done", "BOOL", False),
        ("menu_session_failed", "BOOL", False),
        ("menu_item_logic_ran", "BOOL", False),
    ):
        set_game_property(driver, property_name, property_type, value)
    # This probe intentionally crosses several deferred command boundaries and external teardown.
    # The Python status controller and the native tree are separate logic clients, so their
    # same-tick order is deliberately unspecified. Leave enough tick margin for the final native
    # completion to become visible on the following controller tick instead of making the test
    # depend on object/controller insertion order. The player counts render passes, which can run
    # several times per logic tick, so the margin is generous.
    add_status_controller(driver, status_path, "object_menu_session", required_frame=240)
    set_game_property(driver, "__ln_acceptance_expected_object", "STRING", menu_item.name)
    set_game_property(
        driver, "__ln_acceptance_expected_collection", "STRING", menu_collection.name)
    set_game_property(
        driver, "__ln_acceptance_run", "STRING", "object_menu_session_0")
    bind_tree(driver, menu_tree.name, True)

    menu_collection.hide_viewport = True
    detail_collection.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    require(menu_collection.hide_viewport, "Object menu collection was not viewport-disabled")
    run_game_once(driver, "object_menu_session_0", status_path, prepare=False)


def run_ui_elements_acceptance(status_path):
    """UI Element panel objects in a menu and a HUD, without keyboard or gamepad input.

    The menu must pre-select the lowest Tab Order among interactable elements (the authored
    non-interactable Tab Order 0 button is skipped), but a mouse player must see neither that
    selection's hover look nor a Focus event. The Top Left anchored HUD icon must keep its
    authored offset (measured in frame heights) in any window aspect, and its anchored child must
    ride on it instead of being anchored a second time.
    """
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    main_camera = scene.camera
    scene.render.resolution_x = 1920
    scene.render.resolution_y = 1080
    scene.render.pixel_aspect_x = 1.0
    scene.render.pixel_aspect_y = 1.0
    scene.game_settings.frame_type = "EXTEND"

    driver = create_cube(f"{PREFIX}_UIDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UIOverlayCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    overlay_camera.data.lens = 50.0
    overlay_camera.data.sensor_width = 36.0
    overlay_camera.data.sensor_fit = "AUTO"
    scene.camera = main_camera

    def make_collection(name):
        previous = bpy.data.collections.get(name)
        if previous is not None:
            bpy.data.collections.remove(previous)
        collection = bpy.data.collections.new(name)
        scene.collection.children.link(collection)
        return collection

    def move_to(obj, collection):
        collection.objects.link(obj)
        for source in tuple(obj.users_collection):
            if source != collection:
                source.objects.unlink(obj)

    menu = make_collection(f"{PREFIX}_UIMenu")
    for name, x, tab_order, interactable in (
        ("Tab0", -2.5, 0, False),
        ("Tab1", 0.0, 1, True),
        ("Tab2", 2.5, 2, True),
    ):
        button = create_cube(f"{PREFIX}_UIButton{name}")
        # Away from the window centre, where the player runner's pointer rests.
        button.location = (x, 1.5, 40.0)
        button.scale = (0.5, 0.5, 0.5)
        move_to(button, menu)
        button.game.ui.use_element = True
        button.game.ui.action = name.lower()
        button.game.ui.tab_order = tab_order
        button.game.ui.interactable = interactable
        if name == "Tab1":
            button.game.ui.hover_scale = 1.5
            button.game.ui.look_time = 0.0

    # Authored 10% / 10% from the top-left of the 16:9 camera frame at depth 10.
    hud = make_collection(f"{PREFIX}_UIHud")
    half_width = (36.0 / 2.0) / 50.0 * 10.0
    half_height = half_width * 1080.0 / 1920.0
    icon = create_cube(f"{PREFIX}_UIAnchoredIcon")
    icon.location = (-half_width + 0.2 * half_width, half_height - 0.2 * half_height, 40.0)
    icon.scale = (0.05, 0.05, 0.05)
    move_to(icon, hud)
    icon.game.ui.use_element = True
    icon.game.ui.anchor = "TOP_LEFT"
    icon.game.ui.focus = "NONE"
    badge = create_cube(f"{PREFIX}_UIAnchoredBadge")
    badge.location = (icon.location.x + 0.3, icon.location.y - 0.2, 40.0)
    badge.scale = (0.02, 0.02, 0.02)
    move_to(badge, hud)
    bpy.context.view_layer.update()
    badge.parent = icon
    badge.matrix_parent_inverse = icon.matrix_world.inverted()
    badge.game.ui.use_element = True
    badge.game.ui.anchor = "TOP_LEFT"
    badge.game.ui.focus = "NONE"

    tree = new_logic_tree(f"{PREFIX}_UIElementsTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    show_hud = tree.nodes.new("LogicNativeSetOverlayCollection")
    show_hud.inputs["Camera"].default_value = overlay_camera
    show_hud.inputs["Collection"].default_value = hud
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    tree.links.new(on_init.outputs["Out"], flow_input(show_hud))
    tree.links.new(show_hud.outputs["Success"], flow_input(open_menu))
    on_focus = tree.nodes.new("LogicNativeOnUIEvent")
    on_focus.ui_event = "FOCUS"
    store_focus = tree.nodes.new("LogicNativeSetGamePropertyString")
    store_focus.inputs["Property"].default_value = "ui_focused_action"
    tree.links.new(on_focus.outputs["Out"], flow_input(store_focus))
    tree.links.new(on_focus.outputs["Action"], store_focus.inputs[3])
    # Report which button the menu selected for keyboard and gamepad.
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    get_focus = tree.nodes.new("LogicNativeGetUIFocus")
    focus_name = tree.nodes.new("LogicNativeTypecast")
    focus_name.to_type = "STRING"
    store_focus_name = tree.nodes.new("LogicNativeSetGamePropertyString")
    store_focus_name.inputs["Property"].default_value = "ui_focus_name"
    tree.links.new(on_update.outputs["Out"], flow_input(store_focus_name))
    tree.links.new(get_focus.outputs["Object"], focus_name.inputs["Value"])
    tree.links.new(focus_name.outputs["String"], store_focus_name.inputs["Value"])

    set_game_property(driver, "ui_focused_action", "STRING", "")
    set_game_property(driver, "ui_focus_name", "STRING", "")
    add_status_controller(driver, status_path, "ui_elements", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_elements_0")
    bind_tree(driver, tree.name, True)
    menu.hide_viewport = True
    hud.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_elements_0", status_path, prepare=False)


def make_ui_collection(name, objects=()):
    previous = bpy.data.collections.get(name)
    if previous is not None:
        bpy.data.collections.remove(previous)
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    for obj in objects:
        collection.objects.link(obj)
        for source in tuple(obj.users_collection):
            if source != collection:
                source.objects.unlink(obj)
    return collection


def new_focus_name_setter(tree, property_name):
    """Set Game Property String of the UI focus object's name; returns (setter, flow input)."""
    get_focus = tree.nodes.new("LogicNativeGetUIFocus")
    to_name = tree.nodes.new("LogicNativeTypecast")
    to_name.to_type = "STRING"
    setter = tree.nodes.new("LogicNativeSetGamePropertyString")
    setter.inputs["Property"].default_value = property_name
    tree.links.new(get_focus.outputs["Object"], to_name.inputs["Value"])
    tree.links.new(to_name.outputs["String"], setter.inputs["Value"])
    return setter


def run_ui_menu_focus_restore_acceptance(status_path):
    """Closing a submenu must give the menu below it its selection back."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_UIFocusDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UIFocusCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    buttons = []
    for name, x in (("UIMenuButtonA", -1.5), ("UIMenuButtonB", 1.5), ("UISubButton", 0.0)):
        button = create_cube(f"{PREFIX}_{name}")
        button.location = (x, 1.5, 40.0)
        button.scale = (0.5, 0.5, 0.5)
        button.game.ui.use_element = True
        buttons.append(button)
    menu = make_ui_collection(f"{PREFIX}_UIFocusMenu", buttons[:2])
    submenu = make_ui_collection(f"{PREFIX}_UIFocusSubmenu", buttons[2:])

    tree = new_logic_tree(f"{PREFIX}_UIFocusRestoreTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    wait_menu = tree.nodes.new("LogicNativeDelay")
    wait_menu.inputs["Delay"].default_value = 0.3
    find_b = tree.nodes.new("LogicNativeObjectByName")
    find_b.inputs["Name"].default_value = f"{PREFIX}_UIMenuButtonB"
    set_focus = tree.nodes.new("LogicNativeSetUIFocus")
    focus_failed = tree.nodes.new("LogicNativeSetGamePropertyBool")
    focus_failed.inputs["Property"].default_value = "ui_set_focus_failed"
    focus_failed.inputs["Value"].default_value = True
    open_submenu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_submenu.inputs["Camera"].default_value = overlay_camera
    open_submenu.inputs["Collection"].default_value = submenu
    wait_submenu = tree.nodes.new("LogicNativeDelay")
    wait_submenu.inputs["Delay"].default_value = 0.3
    store_submenu_focus = new_focus_name_setter(tree, "ui_submenu_focus")
    close_submenu = tree.nodes.new("LogicNativeCloseObjectMenu")
    store_restored_focus = new_focus_name_setter(tree, "ui_restored_focus")

    tree.links.new(on_init.outputs["Out"], flow_input(open_menu))
    tree.links.new(open_menu.outputs["Opened"], flow_input(wait_menu))
    tree.links.new(wait_menu.outputs["Done"], flow_input(set_focus))
    tree.links.new(find_b.outputs["Object"], set_focus.inputs["Object"])
    tree.links.new(set_focus.outputs["Failed"], flow_input(focus_failed))
    tree.links.new(set_focus.outputs["Done"], flow_input(open_submenu))
    tree.links.new(open_submenu.outputs["Opened"], flow_input(wait_submenu))
    tree.links.new(wait_submenu.outputs["Done"], flow_input(store_submenu_focus))
    # Close Object Menu closes the top menu (the submenu) by default.
    tree.links.new(wait_submenu.outputs["Done"], flow_input(close_submenu))
    tree.links.new(close_submenu.outputs["Closed"], flow_input(store_restored_focus))

    for name in ("ui_submenu_focus", "ui_restored_focus"):
        set_game_property(driver, name, "STRING", "")
    set_game_property(driver, "ui_set_focus_failed", "BOOL", False)
    add_status_controller(driver, status_path, "ui_menu_focus_restore", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_menu_focus_restore_0")
    bind_tree(driver, tree.name, True)
    menu.hide_viewport = True
    submenu.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_menu_focus_restore_0", status_path, prepare=False)


def run_ui_paused_routes_acceptance(status_path):
    """While the game is paused, a tree runs its UI event routes and nothing else.

    Player runner only: the probe needs the pointer over UI without real input, and the player
    keeps its pointer at the window centre (editor play leaves it wherever the desktop cursor is).
    """
    if RUNNER != "player":
        print("[Logic Nodes acceptance] ui_paused_routes runs with the player runner only")
        return
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_UIPauseDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UIPauseCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    # Covers the whole overlay view, so the pointer is over it wherever it is.
    backdrop = create_cube(f"{PREFIX}_UIPauseBackdrop")
    backdrop.location = (0.0, 0.0, 40.0)
    backdrop.scale = (20.0, 20.0, 0.1)
    backdrop.game.ui.use_element = True
    backdrop.game.ui.action = "backdrop"
    hud = make_ui_collection(f"{PREFIX}_UIPauseHud", (backdrop,))

    tree = new_logic_tree(f"{PREFIX}_UIPausedRoutesTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    pause = tree.nodes.new("LogicNativeSetGamePaused")
    pause.inputs["Paused"].default_value = True
    show_hud = tree.nodes.new("LogicNativeSetOverlayCollection")
    show_hud.inputs["Camera"].default_value = overlay_camera
    show_hud.inputs["Collection"].default_value = hud
    # Both on init: the overlay appears in the first paused frame.
    tree.links.new(on_init.outputs["Out"], flow_input(pause))
    tree.links.new(on_init.outputs["Out"], flow_input(show_hud))
    on_enter = tree.nodes.new("LogicNativeOnUIEvent")
    on_enter.ui_event = "ENTER"
    mark_enter = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_enter.inputs["Property"].default_value = "ui_enter_while_paused"
    mark_enter.inputs["Value"].default_value = True
    tree.links.new(on_enter.outputs["Out"], flow_input(mark_enter))
    # Frame logic of the same tree must wait while paused.
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    count_updates = tree.nodes.new("LogicNativeModifyProperty")
    count_updates.inputs["Property"].default_value = "update_count"
    count_updates.inputs["Value"].default_value = 1.0
    tree.links.new(on_update.outputs["Out"], count_updates.inputs["Flow"])

    set_game_property(driver, "ui_enter_while_paused", "BOOL", False)
    set_game_property(driver, "update_count", "FLOAT", 0.0)
    add_status_controller(driver, status_path, "ui_paused_routes", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_paused_routes_0")
    bind_tree(driver, tree.name, True)
    hud.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_paused_routes_0", status_path, prepare=False)


def run_ui_sound_events_acceptance(status_path):
    """On UI Sound: the highlight moving to another UI Element runs Move, with that element's
    Action and player (the HUD is no menu: no menu name).

    Player runner only, like ui_paused_routes: the pointer rests at the window centre.
    """
    if RUNNER != "player":
        print("[Logic Nodes acceptance] ui_sound_events runs with the player runner only")
        return
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_UISoundDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UISoundCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    buttons = []
    # A starts under the pointer; half a second in, it moves away and B comes under it.
    for name, action, start, later in (("UISoundA", "a", (0.0, 0.0, 40.0), (-6.0, 0.0, 40.0)),
                                       ("UISoundB", "b", (6.0, 0.0, 40.0), (0.0, 0.0, 40.0))):
        button = create_cube(f"{PREFIX}_{name}")
        button.location = start
        button.scale = (2.0, 2.0, 0.1)
        button.game.ui.use_element = True
        button.game.ui.action = action
        mover = new_logic_tree(f"{PREFIX}_UISoundMove_{action}")
        init = mover.nodes.new("LogicNativeOnInit")
        wait = mover.nodes.new("LogicNativeDelay")
        wait.inputs["Delay"].default_value = 0.5
        place = new_set_object_attribute(mover, "WORLD_POSITION")
        place.inputs["Value"].default_value = later
        mover.links.new(init.outputs["Out"], flow_input(wait))
        mover.links.new(wait.outputs["Done"], flow_input(place))
        bind_tree(button, mover.name, True)
        buttons.append(button)
    hud = make_ui_collection(f"{PREFIX}_UISoundHud", buttons)

    tree = new_logic_tree(f"{PREFIX}_UISoundEventsTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    show_hud = tree.nodes.new("LogicNativeSetOverlayCollection")
    show_hud.inputs["Camera"].default_value = overlay_camera
    show_hud.inputs["Collection"].default_value = hud
    tree.links.new(on_init.outputs["Out"], flow_input(show_hud))
    on_sound = tree.nodes.new("LogicNativeOnUISound")
    store_action = tree.nodes.new("LogicNativeSetGamePropertyString")
    store_action.inputs["Property"].default_value = "last_move"
    tree.links.new(on_sound.outputs["Action"], store_action.inputs["Value"])
    store_menu = tree.nodes.new("LogicNativeSetGamePropertyString")
    store_menu.inputs["Property"].default_value = "last_menu"
    tree.links.new(on_sound.outputs["Menu"], store_menu.inputs["Value"])
    store_user = tree.nodes.new("LogicNativeSetGamePropertyInt")
    store_user.inputs["Property"].default_value = "last_user"
    tree.links.new(on_sound.outputs["User"], store_user.inputs["Value"])
    count = tree.nodes.new("LogicNativeModifyProperty")
    count.inputs["Property"].default_value = "move_count"
    count.inputs["Value"].default_value = 1.0
    chain_flows(tree, on_sound.outputs["Move"], [store_action, store_menu, store_user, count])

    set_game_property(driver, "last_move", "STRING", "")
    set_game_property(driver, "last_menu", "STRING", "none")
    set_game_property(driver, "last_user", "INT", -1)
    set_game_property(driver, "move_count", "FLOAT", 0.0)
    add_status_controller(driver, status_path, "ui_sound_events", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_sound_events_0")
    bind_tree(driver, tree.name, True)
    hud.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_sound_events_0", status_path, prepare=False)


def run_sound_groups_acceptance(status_path):
    """Set Volume: a sound plays in the Music group, then Music, Effects (with a fade) and Master
    change; Master is the audio device's volume, which Python's aud.Device() reads."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_SoundGroupsDriver")
    tree = new_logic_tree(f"{PREFIX}_SoundGroupsTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    play = tree.nodes.new("LogicNativePlaySound")
    play.sound = load_acceptance_test_sound()
    play.sound_group = "MUSIC"
    play.inputs["Volume"].default_value = 0.05
    music = tree.nodes.new("LogicNativeSetVolume")
    music.sound_group = "MUSIC"
    music.inputs["Volume"].default_value = 0.5
    effects = tree.nodes.new("LogicNativeSetVolume")
    effects.sound_group = "EFFECTS"
    effects.inputs["Volume"].default_value = 0.0
    effects.inputs["Fade"].default_value = 0.2
    # A new Set Volume is for Master.
    master = tree.nodes.new("LogicNativeSetVolume")
    require(master.sound_group == "MASTER", f"Set Volume starts as {master.sound_group}")
    master.inputs["Volume"].default_value = 0.25
    mark = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark.inputs["Property"].default_value = "volume_done"
    mark.inputs["Value"].default_value = True
    chain_flows(tree, on_init.outputs["Out"], [play, music, effects, master, mark])

    set_game_property(driver, "volume_done", "BOOL", False)
    add_status_controller(driver, status_path, "sound_groups", required_frame=30)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "sound_groups_0")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "sound_groups_0", status_path)


PACING_SETTINGS = ("physics_tick_rate", "use_fixed_fps_cap", "fixed_render_cap_rate", "vsync",
                   "use_fixed_physics_interpolation")


def run_with_pacing_settings(settings, callback):
    """The frame pacing probes change the scene's timing settings; the probes after them get
    the file's own back."""
    game_settings = bpy.context.scene.game_settings
    saved = {name: getattr(game_settings, name) for name in PACING_SETTINGS}
    try:
        for name, value in settings.items():
            setattr(game_settings, name, value)
        callback()
    finally:
        for name, value in saved.items():
            setattr(game_settings, name, value)


def run_smooth_motion_acceptance(status_path):
    """Smooth Motion: objects moved 0.1 a step at 20 steps a second, drawn as fast as frames
    come: Interpolate is drawn between its last two steps, Extrapolate ahead, None where it is,
    a None child of a smoothed object with it, and a Python teleport (resetSmoothing) and Set
    Object Attribute's Teleport are never drawn on the way."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_SmoothDriver")
    move = new_logic_tree(f"{PREFIX}_SmoothMoveTree")
    move_step = move.nodes.new("LogicNativeOnFixedUpdate")
    apply_move = move.nodes.new("LogicNativeApplyMovement")
    apply_move.inputs["Vector"].default_value = (0.1, 0.0, 0.0)
    move.links.new(move_step.outputs["Out"], flow_input(apply_move))

    # The node teleporter moves too, until node_jump sends it to x = 500 with Teleport.
    jump = new_logic_tree(f"{PREFIX}_SmoothJumpTree")
    jump_step = jump.nodes.new("LogicNativeOnFixedUpdate")
    branch = jump.nodes.new("LogicNativeBranch")
    read_jump = jump.nodes.new("LogicNativeGetGamePropertyBool")
    read_jump.inputs["Property"].default_value = "node_jump"
    jump.links.new(jump_step.outputs["Out"], flow_input(branch))
    jump.links.new(read_jump.outputs["Value"], branch.inputs["Condition"])
    set_far = new_set_object_attribute(jump, "WORLD_POSITION")
    set_far.inputs["Value"].default_value = (500.0, 0.0, 0.0)
    set_far.inputs["Teleport"].default_value = True
    jumped = jump.nodes.new("LogicNativeSetGamePropertyBool")
    jumped.inputs["Property"].default_value = "node_jump"
    jumped.inputs["Value"].default_value = False
    jump.links.new(branch.outputs["True"], flow_input(set_far))
    jump.links.new(set_far.outputs["Done"], flow_input(jumped))
    keep_moving = jump.nodes.new("LogicNativeApplyMovement")
    keep_moving.inputs["Vector"].default_value = (0.1, 0.0, 0.0)
    jump.links.new(branch.outputs["False"], flow_input(keep_moving))

    movers = {}
    for index, (key, mode) in enumerate((("mover", "INTERPOLATE"), ("ball", "EXTRAPOLATE"),
                                         ("plain", "SCENE"), ("teleport", "INTERPOLATE"),
                                         ("node", "INTERPOLATE"))):
        obj = create_cube(f"{PREFIX}_Smooth_{key}")
        obj.location = (0.0, 4.0 * (index + 1), 0.0)
        obj.scale = (0.3, 0.3, 0.3)
        obj.game.physics_type = "NO_COLLISION"
        obj.game.smooth_motion = mode
        movers[key] = obj
        set_game_property(driver, "__ln_acceptance_sm_" + key, "STRING", obj.name)
    set_game_property(movers["node"], "node_jump", "BOOL", False)
    bpy.context.view_layer.update()
    child = create_cube(f"{PREFIX}_Smooth_child")
    child.scale = (0.2, 0.2, 0.2)
    child.game.physics_type = "NO_COLLISION"
    child.game.smooth_motion = "NONE"
    child.parent = movers["mover"]
    child.matrix_parent_inverse = movers["mover"].matrix_world.inverted()
    child.location = movers["mover"].location + mathutils.Vector((0.0, 2.0, 0.0))
    set_game_property(driver, "__ln_acceptance_sm_child", "STRING", child.name)
    for key in ("mover", "ball", "plain", "teleport"):
        bind_tree(movers[key], move.name, True)
    bind_tree(movers["node"], jump.name, True)

    def run():
        add_status_controller(driver, status_path, "smooth_motion", required_seconds=3.0)
        set_game_property(driver, "__ln_acceptance_run", "STRING", "smooth_motion_0")
        set_active_object(driver)
        bpy.context.view_layer.update()
        run_game_once(driver, "smooth_motion_0", status_path)

    # Few steps, frames as fast as they come: most frames fall between two steps.
    run_with_pacing_settings({"physics_tick_rate": 20, "use_fixed_fps_cap": False,
                              "vsync": "OFF", "use_fixed_physics_interpolation": False}, run)


def run_frame_pacing_acceptance(status_path):
    """Frame pacing: with the FPS Limit at the steps' rate, every running frame gets one step,
    also across 8 pauses and resumes (the stutter after a resume in the FPS demo: frames that
    alternated 0 and 2 steps). getFramePacingInfo tells."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_PacingDriver")
    tree = new_logic_tree(f"{PREFIX}_PacingTree")
    read_pause = tree.nodes.new("LogicNativeGetGamePropertyBool")
    read_pause.inputs["Property"].default_value = "want_pause"
    # On Update pauses; while paused only While Paused runs (it resumes).
    for event_name, output in (("LogicNativeOnUpdate", "Out"),
                               ("LogicNativeWhilePaused", "Update")):
        event = tree.nodes.new(event_name)
        pause = tree.nodes.new("LogicNativeSetGamePaused")
        tree.links.new(event.outputs[output], flow_input(pause))
        tree.links.new(read_pause.outputs["Value"], pause.inputs["Paused"])
    set_game_property(driver, "want_pause", "BOOL", False)
    bind_tree(driver, tree.name, True)

    def run():
        add_status_controller(driver, status_path, "frame_pacing", required_seconds=8.0)
        set_game_property(driver, "__ln_acceptance_run", "STRING", "frame_pacing_0")
        set_active_object(driver)
        bpy.context.view_layer.update()
        run_game_once(driver, "frame_pacing_0", status_path)

    run_with_pacing_settings({"physics_tick_rate": 60, "use_fixed_fps_cap": True,
                              "fixed_render_cap_rate": 60, "vsync": "OFF",
                              "use_fixed_physics_interpolation": False}, run)


def run_ui_overlay_visibility_acceptance(status_path):
    """Set Collection Visibility on overlays: a HUD of plain visible objects hides too, opening
    and closing a menu does not show a hidden HUD again, and showing one HUD leaves another."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_UIVisibilityDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UIVisibilityCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    plain = create_cube(f"{PREFIX}_UIHudPlain")
    plain.location = (-2.0, 1.5, 40.0)
    template = create_cube(f"{PREFIX}_UIHudTemplate")
    template.location = (0.0, 1.5, 40.0)
    item = create_cube(f"{PREFIX}_UIVisibilityMenuItem")
    item.location = (2.0, 1.5, 40.0)
    for obj in (plain, template, item):
        obj.scale = (0.3, 0.3, 0.3)
    plain_hud = make_ui_collection(f"{PREFIX}_UIHudPlainCollection", (plain,))
    template_hud = make_ui_collection(f"{PREFIX}_UIHudTemplateCollection", (template,))
    menu = make_ui_collection(f"{PREFIX}_UIVisibilityMenu", (item,))

    tree = new_logic_tree(f"{PREFIX}_UIOverlayVisibilityTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    chain = []
    for collection in (plain_hud, template_hud):
        show = tree.nodes.new("LogicNativeSetOverlayCollection")
        show.inputs["Camera"].default_value = overlay_camera
        show.inputs["Collection"].default_value = collection
        chain.append((show, "Success"))
    for collection in (plain_hud, template_hud):
        hide = tree.nodes.new("LogicNativeSetCollectionVisibility")
        hide.inputs["Collection"].default_value = collection
        hide.inputs["Visible"].default_value = False
        chain.append((hide, "Done"))
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    chain.append((open_menu, "Opened"))
    wait_menu = tree.nodes.new("LogicNativeDelay")
    wait_menu.inputs["Delay"].default_value = 0.2
    chain.append((wait_menu, "Done"))
    tree.links.new(on_init.outputs["Out"], flow_input(chain[0][0]))
    for (node, output), (next_node, _next_output) in zip(chain, chain[1:]):
        tree.links.new(node.outputs[output], flow_input(next_node))

    # While the menu is open, record whether each hidden HUD object is visible.
    for name, property_name in ((plain.name, "ui_plain_visible_with_menu"),
                                (template.name, "ui_template_visible_with_menu")):
        find = tree.nodes.new("LogicNativeObjectByName")
        find.inputs["Name"].default_value = name
        get_visible = new_get_object_attribute(tree, "VISIBLE")
        store = tree.nodes.new("LogicNativeSetGamePropertyBool")
        store.inputs["Property"].default_value = property_name
        tree.links.new(find.outputs["Object"], get_visible.inputs["Object"])
        tree.links.new(get_visible.outputs["Visible"], store.inputs["Value"])
        tree.links.new(wait_menu.outputs["Done"], flow_input(store))
    mark_opened = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_opened.inputs["Property"].default_value = "ui_menu_opened"
    mark_opened.inputs["Value"].default_value = True
    tree.links.new(wait_menu.outputs["Done"], flow_input(mark_opened))

    close_menu = tree.nodes.new("LogicNativeCloseObjectMenu")
    tree.links.new(wait_menu.outputs["Done"], flow_input(close_menu))
    show_plain = tree.nodes.new("LogicNativeSetCollectionVisibility")
    show_plain.inputs["Collection"].default_value = plain_hud
    show_plain.inputs["Visible"].default_value = True
    tree.links.new(close_menu.outputs["Closed"], flow_input(show_plain))
    mark_done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_done.inputs["Property"].default_value = "ui_cycle_done"
    mark_done.inputs["Value"].default_value = True
    tree.links.new(show_plain.outputs["Done"], flow_input(mark_done))

    set_game_property(driver, "ui_menu_opened", "BOOL", False)
    set_game_property(driver, "ui_cycle_done", "BOOL", False)
    set_game_property(driver, "ui_plain_visible_with_menu", "BOOL", True)
    set_game_property(driver, "ui_template_visible_with_menu", "BOOL", True)
    add_status_controller(driver, status_path, "ui_overlay_visibility", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_overlay_visibility_0")
    bind_tree(driver, tree.name, True)
    template_hud.hide_viewport = True
    menu.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_overlay_visibility_0", status_path, prepare=False)


def run_ui_grid_template_acceptance(status_path):
    """Object Grid View in a menu makes 1 x 3 slots from one Slot Template, shows icons from a
    hidden item library and stack counts on each copy's own text, and closing the menu (twice)
    removes the copies; closing it used to crash."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_UIGridDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UIGridCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    panel = create_cube(f"{PREFIX}_UIGridPanel")
    panel.location = (0.0, 0.0, 39.0)
    panel.scale = (4.0, 2.0, 0.1)
    slot = create_cube(f"{PREFIX}_UIGridSlot")
    slot.location = (-1.5, 0.0, 40.0)
    slot.scale = (0.4, 0.4, 0.1)
    icon = create_cube(f"{PREFIX}_UIGridSlotIcon")
    icon.location = (-1.5, 0.0, 40.3)
    icon.scale = (0.05, 0.05, 0.05)
    count = bpy.data.objects.new(f"{PREFIX}_UIGridSlotCount",
                                 bpy.data.curves.new(f"{PREFIX}_UIGridSlotCount", "FONT"))
    count.location = (-1.2, -0.3, 40.3)
    bpy.context.scene.collection.objects.link(count)
    set_game_property(count, "Text", "STRING", "")
    bpy.context.view_layer.update()
    for child in (icon, count):
        child.parent = slot
        child.matrix_parent_inverse = slot.matrix_world.inverted()
    menu = make_ui_collection(f"{PREFIX}_UIGridMenu", (panel, slot, icon, count))
    items = []
    for name in ("Apple", "Gem"):
        item = create_cube(f"{PREFIX}_UIGrid{name}")
        item.data.name = f"{PREFIX}_UIGrid{name}Mesh"
        items.append(item)
    library = make_ui_collection(f"{PREFIX}_UIGridLibrary", items)

    grid_tree = new_logic_tree(f"{PREFIX}_UIGridTree")
    shown = grid_tree.nodes.new("LogicNativeGetGlobalProperty")
    shown.inputs["Category"].default_value = "Inventory"
    shown.inputs["Property"].default_value = "items"
    grid = grid_tree.nodes.new("LogicNativeObjectGridView")
    grid.inputs["Columns"].default_value = 3
    grid.inputs["Cull Outside"].default_value = False
    # Items gives the list: its icons come from the scene's Item Library.
    bpy.context.scene.game_settings.item_library = library
    grid.inputs["Slot Template"].default_value = slot
    grid.inputs["Rows"].default_value = 1
    grid.inputs["Spacing"].default_value = (1.5, 0.0, 0.0)
    grid_tree.links.new(shown.outputs["Value"], grid.inputs["Items"])

    # [Apple x4, Gem x1], then open and close the menu twice.
    tree = new_logic_tree(f"{PREFIX}_UIGridDriverTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    chain = []
    for name, amount in (("Apple", 4), ("Gem", 1)):
        add = tree.nodes.new("LogicNativeInventory")
        add.operation = "ADD_ITEM"
        add.inputs["Property"].default_value = "grid_items"
        add.inputs["Item"].default_value = f"{PREFIX}_UIGrid{name}"
        add.inputs["Amount"].default_value = amount
        chain.append((add, "Done"))
    grid_items = tree.nodes.new("LogicNativeGetGameProperty")
    grid_items.inputs["Property"].default_value = "grid_items"
    store = tree.nodes.new("LogicNativeSetGlobalProperty")
    store.inputs["Category"].default_value = "Inventory"
    store.inputs["Property"].default_value = "items"
    tree.links.new(grid_items.outputs["Value"], store.inputs["Value"])
    chain.append((store, "Done"))
    for _cycle in range(2):
        open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
        open_menu.inputs["Camera"].default_value = overlay_camera
        open_menu.inputs["Collection"].default_value = menu
        wait_open = tree.nodes.new("LogicNativeDelay")
        wait_open.inputs["Delay"].default_value = 0.5
        close_menu = tree.nodes.new("LogicNativeCloseObjectMenu")
        wait_closed = tree.nodes.new("LogicNativeDelay")
        wait_closed.inputs["Delay"].default_value = 0.2
        chain += [(open_menu, "Opened"), (wait_open, "Done"), (close_menu, "Closed"),
                  (wait_closed, "Done")]
    mark_done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_done.inputs["Property"].default_value = "ui_cycle_done"
    mark_done.inputs["Value"].default_value = True
    chain.append((mark_done, "Done"))
    tree.links.new(on_init.outputs["Out"], flow_input(chain[0][0]))
    for (node, output), (next_node, _next_output) in zip(chain, chain[1:]):
        tree.links.new(node.outputs[output], flow_input(next_node))

    set_game_property(driver, "ui_cycle_done", "BOOL", False)
    set_game_property(driver, "ui_grid_seen", "STRING", "{}")
    add_status_controller(driver, status_path, "ui_grid_template", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_grid_template_0")
    bind_tree(driver, tree.name, True)
    bind_tree(panel, grid_tree.name, True)
    menu.hide_viewport = True
    library.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_grid_template_0", status_path, prepare=False)


def run_ui_grid_inventory_acceptance(status_path):
    """Object Grid View names its list: in a menu opened for player 1, a grid with an empty Object
    shows the "inventory" of whose menu it is (the object that opened it), one with an Object the
    chest's "chest". The slot player 1's focus highlights shows its Tooltip Object (a text that is
    no child of the slot) with the item's words from the scene's Item Library, beside it by an
    offset in the tooltip's parent space. Moves, drops and hints need the pointer: unit tests and
    pointer replays check them."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    main_camera = scene.camera
    driver = create_cube(f"{PREFIX}_GIDriver")
    chest = create_cube(f"{PREFIX}_GIChest")
    chest.location = (6.0, 0.0, 0.0)
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_GICamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    scene.camera = main_camera
    panel = create_cube(f"{PREFIX}_GIPanel")
    panel.location = (0.0, 0.0, 39.0)
    panel.scale = (4.0, 2.0, 0.1)
    slot = create_cube(f"{PREFIX}_GISlot")
    slot.location = (-1.5, 1.0, 40.0)
    slot.scale = (0.4, 0.4, 0.1)
    chest_slot = create_cube(f"{PREFIX}_GIChestSlot")
    chest_slot.location = (-1.5, -1.0, 40.0)
    chest_slot.scale = (0.4, 0.4, 0.1)
    tip = bpy.data.objects.new(f"{PREFIX}_GITip",
                               bpy.data.curves.new(f"{PREFIX}_GITip", "FONT"))
    tip.location = (3.0, 0.0, 40.5)
    scene.collection.objects.link(tip)
    bpy.context.view_layer.update()
    tip.parent = panel
    tip.matrix_parent_inverse = panel.matrix_world.inverted()
    for element, action in ((slot, "bag"), (chest_slot, "chest")):
        element.game.ui.use_element = True
        element.game.ui.action = action
        element.game.ui.use_draggable = True
        element.game.ui.use_drop_target = True
    slot.game.ui.tooltip_object = tip
    slot.game.ui.tooltip_text = "{name}: {description} x{count}"
    slot.game.ui.tooltip_placement = 'BESIDE'
    slot.game.ui.tooltip_offset = (0.0, 1.0, 0.5)
    menu = make_ui_collection(f"{PREFIX}_GIMenu", (panel, slot, chest_slot, tip))
    items = []
    for name, display, description in (("Apple", "Red Apple", "Crunchy"),
                                       ("Gem", "Blue Gem", "Shiny")):
        item = create_cube(f"{PREFIX}_GI{name}")
        set_game_property(item, "name", "STRING", display)
        set_game_property(item, "description", "STRING", description)
        items.append(item)
    library = make_ui_collection(f"{PREFIX}_GILibrary", items)
    scene.game_settings.item_library = library

    # The menu's tree: two grids naming their lists, with Inventory on, reporting to the driver.
    grid_tree = new_logic_tree(f"{PREFIX}_GITree")
    nodes = grid_tree.nodes
    bag = nodes.new("LogicNativeObjectGridView")
    bag.inputs["Property"].default_value = "inventory"
    bag.inputs["Slot Template"].default_value = slot
    bag.inputs["Rows"].default_value = 1
    bag.inputs["Columns"].default_value = 2
    bag.inputs["Spacing"].default_value = (1.5, 0.0, 0.0)
    bag.inputs["Cull Outside"].default_value = False
    bag.inputs["Show Empty"].default_value = True
    bag.inputs["Handle Moves"].default_value = True
    chest_grid = nodes.new("LogicNativeObjectGridView")
    chest_grid.inputs["Object"].default_value = chest
    chest_grid.inputs["Property"].default_value = "chest"
    chest_grid.inputs["Slot Template"].default_value = chest_slot
    chest_grid.inputs["Rows"].default_value = 1
    chest_grid.inputs["Columns"].default_value = 1
    chest_grid.inputs["Cull Outside"].default_value = False
    chest_grid.inputs["Handle Moves"].default_value = True
    flow = nodes.new("LogicNativeOnUpdate").outputs["Out"]
    for name, output in (("bag_last", bag.outputs["Last Visible"]),
                         ("chest_pages", chest_grid.outputs["Page Count"])):
        setter = nodes.new("LogicNativeSetGameProperty")
        setter.inputs["Object"].default_value = driver
        setter.inputs["Property"].default_value = name
        grid_tree.links.new(flow, setter.inputs["Flow"])
        grid_tree.links.new(output, setter.inputs["Value"])
        flow = setter.outputs["Done"]
    # What a grid did with a drop (none comes here: no pointer).
    on_drop = nodes.new("LogicNativeOnUIEvent")
    on_drop.ui_event = "DROP_RECEIVED"
    outcome = nodes.new("LogicNativeSetGameProperty")
    outcome.inputs["Object"].default_value = driver
    outcome.inputs["Property"].default_value = "outcome"
    grid_tree.links.new(on_drop.outputs["Out"], outcome.inputs["Flow"])
    grid_tree.links.new(on_drop.outputs["Outcome"], outcome.inputs["Value"])

    # The driver's lists, then the menu for player 1 (its owner: the driver, which opens it).
    tree = new_logic_tree(f"{PREFIX}_GIDriverTree")
    flow = tree.nodes.new("LogicNativeOnInit").outputs["Out"]
    for target, prop, item, amount in ((None, "inventory", "Apple", 3),
                                       (None, "inventory", "Gem", 1),
                                       (chest, "chest", "Gem", 2),
                                       (chest, "chest", "Apple", 1)):
        add = tree.nodes.new("LogicNativeInventory")
        add.operation = "ADD_ITEM"
        if target is not None:
            add.inputs["Object"].default_value = target
        add.inputs["Property"].default_value = prop
        add.inputs["Item"].default_value = f"{PREFIX}_GI{item}"
        add.inputs["Amount"].default_value = amount
        tree.links.new(flow, add.inputs["Flow"])
        flow = add.outputs["Done"]
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    open_menu.inputs["User"].default_value = 1
    tree.links.new(flow, flow_input(open_menu))

    set_game_property(driver, "bag_last", "INT", -1)
    set_game_property(driver, "chest_pages", "INT", -1)
    set_game_property(driver, "outcome", "STRING", "")
    add_status_controller(driver, status_path, "ui_grid_inventory", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_grid_inventory_0")
    bind_tree(driver, tree.name, True)
    bind_tree(panel, grid_tree.name, True)
    menu.hide_viewport = True
    library.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_grid_inventory_0", status_path, prepare=False)


def run_ui_menu_pause_toggle_acceptance(status_path):
    """Open Object Menu with Pause Game and Toggle: the game pauses while the menu is open (While
    Paused frames run, Logic + Physics steps do not), opening it again closes it (Closed runs) and
    the game resumes. The menu's collection stays visible in the viewport: it is a menu, so its
    objects are only the templates of the menu's copies, not live objects of the world."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_PauseDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_PauseCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    panel = create_cube(f"{PREFIX}_PausePanel")
    panel.location = (0.0, 0.0, 39.0)
    panel.scale = (4.0, 2.0, 0.1)
    menu = make_ui_collection(f"{PREFIX}_PauseMenu", (panel,))

    tree = new_logic_tree(f"{PREFIX}_PauseTree")

    def prop_node(idname, name, value=None):
        node = tree.nodes.new(idname)
        node.inputs["Property"].default_value = name
        if value is not None:
            node.inputs["Value"].default_value = value
        return node

    def add_one(name):
        node = tree.nodes.new("LogicNativeModifyProperty")
        node.operation = "ADD"
        node.inputs["Property"].default_value = name
        node.inputs["Value"].default_value = 1.0
        return node

    def at_least(name, value):
        read = prop_node("LogicNativeGetGameProperty", name)
        compare = tree.nodes.new("LogicNativeCompare")
        compare.operation = "GREATER_EQUAL"
        compare.inputs["B"].default_value = value
        tree.links.new(read.outputs["Value"], compare.inputs["A"])
        return compare.outputs["Result"]

    def copy_steps(name):
        read = prop_node("LogicNativeGetGamePropertyInt", "steps")
        write = prop_node("LogicNativeSetGamePropertyInt", name)
        tree.links.new(read.outputs["Value"], write.inputs["Value"])
        return write

    on_init = tree.nodes.new("LogicNativeOnInit")
    open_or_close = tree.nodes.new("LogicNativeAnyFlow")
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    open_menu.use_pause_game = True
    open_menu.use_toggle = True
    tree.links.new(on_init.outputs["Out"], open_or_close.inputs["In"])
    tree.links.new(open_or_close.outputs["Out"], flow_input(open_menu))
    opened = prop_node("LogicNativeSetGamePropertyBool", "opened", True)
    tree.links.new(open_menu.outputs["Opened"], flow_input(opened))
    closed = prop_node("LogicNativeSetGamePropertyBool", "toggled_closed", True)
    tree.links.new(open_menu.outputs["Closed"], flow_input(closed))
    at_close = copy_steps("steps_at_close")
    tree.links.new(closed.outputs["Done"], flow_input(at_close))

    # Logic + Physics steps count only while the game runs.
    step = tree.nodes.new("LogicNativeOnFixedUpdate")
    count_step = add_one("steps")
    tree.links.new(step.outputs["Out"], flow_input(count_step))
    after_close = tree.nodes.new("LogicNativeBranch")
    tree.links.new(step.outputs["Out"], flow_input(after_close))
    tree.links.new(prop_node("LogicNativeGetGamePropertyBool", "toggled_closed").outputs["Value"],
                   after_close.inputs["Condition"])
    count_after = add_one("steps_after_close")
    tree.links.new(after_close.outputs["True"], flow_input(count_after))
    resumed = tree.nodes.new("LogicNativeBranch")
    tree.links.new(after_close.outputs["True"], flow_input(resumed))
    tree.links.new(at_least("steps_after_close", 5), resumed.inputs["Condition"])
    done = prop_node("LogicNativeSetGamePropertyBool", "ui_cycle_done", True)
    tree.links.new(resumed.outputs["True"], flow_input(done))

    # While paused: count frames, note the steps at the start, then toggle the menu closed.
    paused = tree.nodes.new("LogicNativeWhilePaused")
    count_paused = add_one("paused_frames")
    tree.links.new(paused.outputs["Update"], flow_input(count_paused))
    first = tree.nodes.new("LogicNativeOnce")
    tree.links.new(paused.outputs["Update"], flow_input(first))
    at_pause = copy_steps("steps_at_pause")
    tree.links.new(first.outputs["Out"], flow_input(at_pause))
    enough = tree.nodes.new("LogicNativeBranch")
    tree.links.new(paused.outputs["Update"], flow_input(enough))
    tree.links.new(at_least("paused_frames", 30), enough.inputs["Condition"])
    close_once = tree.nodes.new("LogicNativeOnce")
    tree.links.new(enough.outputs["True"], flow_input(close_once))
    tree.links.new(close_once.outputs["Out"], open_or_close.inputs["In"])

    for name, kind, value in (("opened", "BOOL", False), ("toggled_closed", "BOOL", False),
                              ("ui_cycle_done", "BOOL", False), ("steps", "INT", 0),
                              ("steps_after_close", "INT", 0), ("paused_frames", "INT", 0),
                              ("steps_at_pause", "INT", -1), ("steps_at_close", "INT", -2)):
        set_game_property(driver, name, kind, value)
    add_status_controller(driver, status_path, "ui_menu_pause_toggle", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_menu_pause_toggle_0")
    bind_tree(driver, tree.name, True)
    # Shown in the viewport on purpose (see the docstring).
    menu.hide_viewport = False
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_menu_pause_toggle_0", status_path, prepare=False)


def run_ui_text_values_bar_acceptance(status_path):
    """Game property values in text ({Object.property}, {property} of the text's parent, :1 for
    one decimal) follow the properties, unknown ones stay as written, and Bar widgets show a value
    over a Max Property (and over Max, through a parent) as a fill growing from the left. Grid slot
    copies made from a slot whose bar already shows a value fill like it, and a Max Property of 0
    is empty."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_TVDriver")
    create_camera_looking_at(f"{PREFIX}_TVCamera", (0.0, 0.0, 30.0), (0.0, 0.0, 0.0))
    for name, kind, value in (("gold", "INT", 7), ("speed", "FLOAT", 2.26), ("hp", "FLOAT", 30.0),
                              ("max_hp", "FLOAT", 120.0), ("zero_max", "FLOAT", 0.0),
                              ("ui_cycle_done", "BOOL", False)):
        set_game_property(driver, name, kind, value)
    set_game_property(driver, "grid_rows", "INT", 1)

    def text(name, body, location, parent=None):
        curve = bpy.data.curves.new(name, "FONT")
        curve.body = body
        obj = bpy.data.objects.new(name, curve)
        obj.location = location
        bpy.context.scene.collection.objects.link(obj)
        if parent is not None:
            bpy.context.view_layer.update()
            obj.parent = parent
            obj.matrix_parent_inverse = parent.matrix_world.inverted()
        return obj

    def plane(name, width, location, parent=None):
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata([(-width / 2, -0.1, 0.0), (width / 2, -0.1, 0.0), (width / 2, 0.1, 0.0),
                          (-width / 2, 0.1, 0.0)], [], [(0, 1, 2, 3)])
        obj = bpy.data.objects.new(name, mesh)
        obj.location = location
        bpy.context.scene.collection.objects.link(obj)
        obj.game.physics_type = "NO_COLLISION"
        if parent is not None:
            bpy.context.view_layer.update()
            obj.parent = parent
            obj.matrix_parent_inverse = parent.matrix_world.inverted()
        return obj

    text(f"{PREFIX}_TVText", "Gold {" + f"{PREFIX}_TVDriver" + ".gold} | {" +
         f"{PREFIX}_TVDriver" + ".speed:1}", (-3.0, 3.0, 0.0))
    text(f"{PREFIX}_TVParentText", "{gold}/{hp}", (0.0, 0.0, 1.5), driver)
    text(f"{PREFIX}_TVMissingText", "{Missing.gold} {nothing}", (-3.0, -3.0, 0.0))

    def bar(name, location, parent, owner, max_value, max_property):
        background = plane(name, 2.0, location, parent)
        fill = plane(name + "Fill", 2.0, location, background)
        ui = background.game.ui
        ui.use_element = True
        ui.focus = "NONE"
        ui.widget = "BAR"
        ui.widget_property = "hp"
        ui.widget_max = max_value
        ui.widget_max_property = max_property
        ui.widget_part = fill
        ui.look_time = 0.0
        if owner is not None:
            ui.widget_object = owner
        return background

    bar(f"{PREFIX}_TVBar", (0.0, 2.0, 0.0), None, driver, 100.0, "max_hp")
    bar(f"{PREFIX}_TVChildBar", (0.0, -2.0, 0.0), driver, None, 60.0, "")
    bar(f"{PREFIX}_TVZeroBar", (0.0, -4.0, 0.0), None, driver, 100.0, "zero_max")
    slot = plane(f"{PREFIX}_TVSlot", 1.0, (-8.0, 0.0, 0.0))
    bar(f"{PREFIX}_TVSlotBar", (-8.0, 0.0, 0.1), slot, driver, 120.0, "")

    tree = new_logic_tree(f"{PREFIX}_TVTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    wait = tree.nodes.new("LogicNativeDelay")
    wait.inputs["Delay"].default_value = 0.3
    set_gold = tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_gold.inputs["Property"].default_value = "gold"
    set_gold.inputs["Value"].default_value = 42
    set_hp = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_hp.inputs["Property"].default_value = "hp"
    set_hp.inputs["Value"].default_value = 90.0
    wait_more = tree.nodes.new("LogicNativeDelay")
    wait_more.inputs["Delay"].default_value = 0.3
    # Four items in a grid of 2 columns, one row and then two: the second row's slots are copies
    # of the template made while its bar shows a value (its fill already shrunk).
    add_items = tree.nodes.new("LogicNativeInventory")
    add_items.operation = "ADD_ITEM"
    add_items.inputs["Property"].default_value = "grid_items"
    add_items.inputs["Item"].default_value = "Apple"
    add_items.inputs["Amount"].default_value = 4
    add_items.inputs["Max Stack"].default_value = 1
    add_items.inputs["Override"].default_value = True
    grid_items = tree.nodes.new("LogicNativeGetGameProperty")
    grid_items.inputs["Property"].default_value = "grid_items"
    grid_rows = tree.nodes.new("LogicNativeGetGameProperty")
    grid_rows.inputs["Property"].default_value = "grid_rows"
    grid = tree.nodes.new("LogicNativeObjectGridView")
    grid.inputs["Columns"].default_value = 2
    grid.inputs["Cull Outside"].default_value = False
    grid.inputs["Slot Template"].default_value = slot
    grid.inputs["Spacing"].default_value = (2.5, 1.0, 0.0)
    tree.links.new(grid_items.outputs["Value"], grid.inputs["Items"])
    tree.links.new(grid_rows.outputs["Value"], grid.inputs["Rows"])
    more_rows = tree.nodes.new("LogicNativeSetGamePropertyInt")
    more_rows.inputs["Property"].default_value = "grid_rows"
    more_rows.inputs["Value"].default_value = 2
    wait_rows = tree.nodes.new("LogicNativeDelay")
    wait_rows.inputs["Delay"].default_value = 0.3
    done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Property"].default_value = "ui_cycle_done"
    done.inputs["Value"].default_value = True
    chain = [(on_init, "Out"), (add_items, "Done"), (wait, "Out"), (set_gold, "Done"),
             (set_hp, "Done"), (wait_more, "Out"), (more_rows, "Done"), (wait_rows, "Out"),
             (done, "Done")]
    for (node, output), (next_node, _next_output) in zip(chain, chain[1:]):
        tree.links.new(node.outputs[output], flow_input(next_node))

    add_status_controller(driver, status_path, "ui_text_values_bar", required_frame=60)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_text_values_bar_0")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_text_values_bar_0", status_path, prepare=False)


def run_ui_menu_camera_hidden_acceptance(status_path):
    """A creator keeps the UI camera with the menus in a collection disabled in the viewport: the
    camera an Open Object Menu node names still starts live, so its own tree runs (a HUD shown, a
    menu's key read), while the menu's objects stay asleep as templates."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_MCDriver")
    create_camera_looking_at(f"{PREFIX}_MCMainCamera", (0.0, -12.0, 8.0), (0.0, 0.0, 0.0))
    for name in ("camera_awake", "ui_cycle_done"):
        set_game_property(driver, name, "BOOL", False)
    ui = bpy.data.collections.new(f"{PREFIX}_MCUI")
    bpy.context.scene.collection.children.link(ui)
    camera = bpy.data.objects.new(f"{PREFIX}_MCCamera", bpy.data.cameras.new(f"{PREFIX}_MCCamera"))
    camera.location = (100.0, 0.0, 10.0)
    ui.objects.link(camera)
    menu = bpy.data.collections.new(f"{PREFIX}_MCMenu")
    ui.children.link(menu)
    mesh = bpy.data.meshes.new(f"{PREFIX}_MCPanel")
    mesh.from_pydata([(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)], [],
                     [(0, 1, 2, 3)])
    panel = bpy.data.objects.new(f"{PREFIX}_MCPanel", mesh)
    panel.location = (100.0, 0.0, 0.0)
    menu.objects.link(panel)
    ui.hide_viewport = True

    # On the camera: at start, say it runs; a key (never pressed) would open the menu with it.
    tree = new_logic_tree(f"{PREFIX}_MCTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    awake = tree.nodes.new("LogicNativeSetGamePropertyBool")
    awake.inputs["Object"].default_value = driver
    awake.inputs["Property"].default_value = "camera_awake"
    awake.inputs["Value"].default_value = True
    done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Object"].default_value = driver
    done.inputs["Property"].default_value = "ui_cycle_done"
    done.inputs["Value"].default_value = True
    tree.links.new(on_init.outputs["Out"], flow_input(awake))
    tree.links.new(awake.outputs["Done"], flow_input(done))
    key = tree.nodes.new("LogicNativeKeyboardKey")
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = camera
    open_menu.inputs["Collection"].default_value = menu
    tree.links.new(key.outputs["Out"], flow_input(open_menu))
    bind_tree(camera, tree.name, True)

    add_status_controller(driver, status_path, "ui_menu_camera_hidden", required_frame=30)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_menu_camera_hidden_0")
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_menu_camera_hidden_0", status_path, prepare=False)


def run_ui_example_file_acceptance(status_path):
    """Add > Game UI > Open Complete UI Example opens the installed example as a new untitled file
    that plays: the HUD shows, and with every menu opened by a test tree each menu tree runs and
    fills its grids. The player runner also checks that no tree of the example failed to compile.
    It replaces the open file, so it runs last."""
    require(bpy.ops.wm.game_ui_example_open.poll(), "The UI example file is not installed")
    result = bpy.ops.wm.game_ui_example_open()
    require(result == {"FINISHED"}, f"Opening the UI example failed: {result}")
    require(bpy.data.filepath == "", "The UI example did not open as a new untitled file")
    camera = bpy.data.objects.get("InvCamera")
    require(camera is not None, "The UI example has no InvCamera")

    driver = bpy.data.objects.new(f"{PREFIX}_ExampleDriver", None)
    bpy.context.scene.collection.objects.link(driver)
    driver.location = (0.0, 0.0, -50.0)
    tree = bpy.data.node_groups.new(f"{PREFIX}_ExampleMenus", "LogicNodeTree")
    start = tree.nodes.new("LogicNativeOnInit")
    for name in ("InventoryMenu", "ChestMenu", "SettingsMenu", "Pause Menu"):
        menu = bpy.data.collections.get(name)
        require(menu is not None, f"The UI example has no menu collection {name!r}")
        open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
        open_menu.inputs["Camera"].default_value = camera
        open_menu.inputs["Collection"].default_value = menu
        tree.links.new(start.outputs["Out"], flow_input(open_menu))
    bind_tree(driver, tree.name, True)

    # The check waits a second for the grids; the fallback quit must come later than that.
    add_status_controller(driver, status_path, "ui_example_file", required_frame=30,
                          required_seconds=2.0)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_example_file_0")
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_example_file_0", status_path, prepare=False)
    if RUNNER == "player":
        problems = [line for line in LAST_PLAYER_OUTPUT.splitlines()
                    if "Logic Nodes" in line and
                    ("error" in line or "disabled" in line or "failed" in line)]
        require(not problems, "The UI example has Logic Nodes errors:\n" + "\n".join(problems))


def run_ui_templates_acceptance(status_path):
    """Add > Game UI > Templates, every one added to one scene through the operator as a creator
    does: Walking and Pickups (on the player they make), the Inventory and the Pause menu without
    their Settings buttons, the Chest, then the Settings menu, which adds the buttons to both. With
    every menu opened by a test tree, each menu tree runs and fills its grids. The player runner
    also checks that no tree failed to compile. The menus, trees and actions stay in the file, so
    it runs just before ui_example_file, which replaces the file."""
    reset_scene()
    configure_game_settings(True)
    require(bpy.ops.object.game_ui_template_add.poll(), "The UI templates are not available")
    for template, options in (("WALKING", {}), ("PICKUPS", {}),
                              ("INVENTORY", {"settings": False}), ("CHEST", {}),
                              ("PAUSE", {"settings": False}), ("SETTINGS", {})):
        result = bpy.ops.object.game_ui_template_add(template=template, **options)
        require(result == {"FINISHED"}, f"The {template} template failed: {result}")
    # The Game UI tools' menu camera stands for the example's InvCamera.
    from bl_operators import game_ui
    camera = bpy.data.objects.get(game_ui.UI_CAMERA_NAME)
    require(camera is not None, "The templates made no UI camera")
    for name in ("SettingsButton", "Settings"):
        require(bpy.data.objects.get(name) is not None,
                f"The Settings menu did not add its button {name!r} to the other menus")
    # The first menu brought the menus' sounds (the UI Sounds frame), once; the UI Sounds template
    # then has nothing to add.
    from _bpy_internal.game_ui import templates as game_ui_templates
    sound_tree = game_ui_templates.ui_sounds_tree(bpy.context.scene)
    require(sound_tree is not None, "The menu templates added no menu sounds")
    plays = {node.sound.name if node.sound else None: node.sound_group
             for node in sound_tree.nodes if node.bl_idname == "LogicNativePlaySound"}
    require(plays == {"UI Move": "UI", "UI Accept": "UI", "UI Back": "UI", "UI Denied": "UI"},
            f"The menu sounds play {plays}")
    listeners = sum(node.bl_idname == "LogicNativeOnUISound"
                    for tree in bpy.data.node_groups for node in tree.nodes)
    require(listeners == 1, f"The templates made {listeners} On UI Sound nodes")
    try:
        refused = bpy.ops.object.game_ui_template_add(template="UI_SOUNDS") != {"FINISHED"}
    except RuntimeError:
        refused = True
    require(refused, "The UI Sounds template added a second set of menu sounds")

    driver = bpy.data.objects.new(f"{PREFIX}_TemplateDriver", None)
    bpy.context.scene.collection.objects.link(driver)
    driver.location = (0.0, 0.0, -50.0)
    tree = bpy.data.node_groups.new(f"{PREFIX}_TemplateMenus", "LogicNodeTree")
    start = tree.nodes.new("LogicNativeOnInit")
    for name in ("InventoryMenu", "ChestMenu", "SettingsMenu", "Pause Menu"):
        menu = bpy.data.collections.get(name)
        require(menu is not None, f"The templates made no menu collection {name!r}")
        open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
        open_menu.inputs["Camera"].default_value = camera
        open_menu.inputs["Collection"].default_value = menu
        tree.links.new(start.outputs["Out"], flow_input(open_menu))
    bind_tree(driver, tree.name, True)

    add_status_controller(driver, status_path, "ui_templates", required_frame=30,
                          required_seconds=2.0)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_templates_0")
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_templates_0", status_path, prepare=False)
    if RUNNER == "player":
        problems = [line for line in LAST_PLAYER_OUTPUT.splitlines()
                    if "Logic Nodes" in line and
                    ("error" in line or "disabled" in line or "failed" in line)]
        require(not problems, "The UI templates have Logic Nodes errors:\n" + "\n".join(problems))


def run_ui_grid_parented_template_acceptance(status_path):
    """Object Grid View copies a slot template that is a child of a panel (moved, turned and
    scaled after the template was parented, so Blender's parent inverse is not the panel's) into a
    grid around the template, along the panel's axes, at Spacing in the panel's units, and as
    children of the panel."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_GPDriver")
    driver.location = (-8.0, -8.0, 0.0)
    create_camera_looking_at(f"{PREFIX}_GPCamera", (0.0, 0.0, 30.0), (0.0, 0.0, 0.0))
    set_game_property(driver, "ui_cycle_done", "BOOL", False)

    def plane(name, size, location):
        half = size / 2.0
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata([(-half, -half, 0.0), (half, -half, 0.0), (half, half, 0.0),
                          (-half, half, 0.0)], [], [(0, 1, 2, 3)])
        obj = bpy.data.objects.new(name, mesh)
        obj.location = location
        bpy.context.scene.collection.objects.link(obj)
        obj.game.physics_type = "NO_COLLISION"
        return obj

    panel = plane(f"{PREFIX}_GPPanel", 4.0, (5.0, 3.0, 0.0))
    slot = plane(f"{PREFIX}_GPSlot", 1.0, (4.0, 4.0, 0.1))
    bpy.context.view_layer.update()
    slot.parent = panel
    slot.matrix_parent_inverse = panel.matrix_world.inverted()
    panel.location = (6.0, 2.0, 0.0)
    panel.rotation_euler = (0.0, 0.0, math.pi / 2.0)
    panel.scale = (2.0, 2.0, 2.0)

    tree = new_logic_tree(f"{PREFIX}_GPTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    add = tree.nodes.new("LogicNativeInventory")
    add.operation = "ADD_ITEM"
    add.inputs["Property"].default_value = "grid_items"
    add.inputs["Item"].default_value = "Apple"
    add.inputs["Amount"].default_value = 4
    add.inputs["Max Stack"].default_value = 1
    add.inputs["Override"].default_value = True
    wait = tree.nodes.new("LogicNativeDelay")
    wait.inputs["Delay"].default_value = 0.4
    done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Property"].default_value = "ui_cycle_done"
    done.inputs["Value"].default_value = True
    tree.links.new(on_init.outputs["Out"], flow_input(add))
    tree.links.new(add.outputs["Done"], flow_input(wait))
    tree.links.new(wait.outputs["Out"], flow_input(done))
    items = tree.nodes.new("LogicNativeGetGameProperty")
    items.inputs["Property"].default_value = "grid_items"
    grid = tree.nodes.new("LogicNativeObjectGridView")
    grid.inputs["Columns"].default_value = 2
    grid.inputs["Rows"].default_value = 2
    grid.inputs["Cull Outside"].default_value = False
    grid.inputs["Slot Template"].default_value = slot
    grid.inputs["Spacing"].default_value = (1.0, 1.0, 0.0)
    tree.links.new(items.outputs["Value"], grid.inputs["Items"])

    add_status_controller(driver, status_path, "ui_grid_parented_template", required_frame=60)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_grid_parented_template_0")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_grid_parented_template_0", status_path, prepare=False)


def run_ui_grid_pages_acceptance(status_path):
    """Scroll Object View on an Object Grid View in a menu: Next Page, Show Item and Top move it,
    and Page, Page Count and First Visible (integers linked straight into a Formatted String)
    follow."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_UIPagesDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UIPagesCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    panel = create_cube(f"{PREFIX}_UIPagesPanel")
    panel.location = (0.0, 0.0, 39.0)
    panel.scale = (4.0, 2.0, 0.1)
    slot = create_cube(f"{PREFIX}_UIPagesSlot")
    slot.location = (-1.5, 0.5, 40.0)
    slot.scale = (0.4, 0.4, 0.1)
    menu = make_ui_collection(f"{PREFIX}_UIPagesMenu", (panel, slot))

    # The driver: ten apples, one per entry, then the menu.
    tree = new_logic_tree(f"{PREFIX}_UIPagesDriverTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    add = tree.nodes.new("LogicNativeInventory")
    add.operation = "ADD_ITEM"
    add.inputs["Property"].default_value = "grid_items"
    add.inputs["Item"].default_value = "Apple"
    add.inputs["Amount"].default_value = 10
    add.inputs["Max Stack"].default_value = 1
    add.inputs["Override"].default_value = True
    grid_items = tree.nodes.new("LogicNativeGetGameProperty")
    grid_items.inputs["Property"].default_value = "grid_items"
    store = tree.nodes.new("LogicNativeSetGlobalProperty")
    store.inputs["Category"].default_value = "Pages"
    store.inputs["Property"].default_value = "items"
    tree.links.new(grid_items.outputs["Value"], store.inputs["Value"])
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    tree.links.new(on_init.outputs["Out"], flow_input(add))
    tree.links.new(add.outputs["Done"], flow_input(store))
    tree.links.new(store.outputs["Done"], flow_input(open_menu))

    # The panel: the grid, and a few scrolls with what Page / Page Count / First Visible say.
    grid_tree = new_logic_tree(f"{PREFIX}_UIPagesTree")
    shown = grid_tree.nodes.new("LogicNativeGetGlobalProperty")
    shown.inputs["Category"].default_value = "Pages"
    shown.inputs["Property"].default_value = "items"
    grid = grid_tree.nodes.new("LogicNativeObjectGridView")
    grid.inputs["Columns"].default_value = 2
    grid.inputs["Rows"].default_value = 2
    grid.inputs["Cull Outside"].default_value = False
    grid.inputs["Slot Template"].default_value = slot
    grid.inputs["Spacing"].default_value = (1.0, 1.0, 0.0)
    grid_tree.links.new(shown.outputs["Value"], grid.inputs["Items"])
    line = grid_tree.nodes.new("LogicNativeFormattedString")
    line.inputs["Format String"].default_value = "{} / {} / {}"
    grid_tree.links.new(grid.outputs["Page"], line.inputs["A"])
    grid_tree.links.new(grid.outputs["Page Count"], line.inputs["B"])
    grid_tree.links.new(grid.outputs["First Visible"], line.inputs["C"])
    last_item = grid_tree.nodes.new("LogicNativeValueInt")
    last_item.outputs["Int"].default_value = 9
    start = grid_tree.nodes.new("LogicNativeOnInit")
    chain = []

    def wait(seconds):
        delay = grid_tree.nodes.new("LogicNativeDelay")
        delay.inputs["Delay"].default_value = seconds
        chain.append((delay, "Done"))

    def record(name):
        write = grid_tree.nodes.new("LogicNativeSetGamePropertyString")
        write.inputs["Object"].default_value = driver
        write.inputs["Property"].default_value = name
        grid_tree.links.new(line.outputs["String"], write.inputs["Value"])
        chain.append((write, "Done"))

    def scroll(mode):
        node = grid_tree.nodes.new("LogicNativeScrollObjectView")
        node.scroll_mode = mode
        if mode == "SHOW_ITEM":
            grid_tree.links.new(last_item.outputs["Int"], node.inputs["Item"])
        chain.append((node, "Done"))

    wait(0.4)
    record("pages_start")
    for mode, name in (("NEXT_PAGE", "pages_next"), ("SHOW_ITEM", "pages_show"),
                       ("TOP", "pages_top")):
        scroll(mode)
        wait(0.3)
        record(name)
    done = grid_tree.nodes.new("LogicNativeSetGamePropertyBool")
    done.inputs["Object"].default_value = driver
    done.inputs["Property"].default_value = "ui_cycle_done"
    done.inputs["Value"].default_value = True
    chain.append((done, "Done"))
    grid_tree.links.new(start.outputs["Out"], flow_input(chain[0][0]))
    for (node, output), (next_node, _next_output) in zip(chain, chain[1:]):
        grid_tree.links.new(node.outputs[output], flow_input(next_node))

    set_game_property(driver, "ui_cycle_done", "BOOL", False)
    for name in ("pages_start", "pages_next", "pages_show", "pages_top"):
        set_game_property(driver, name, "STRING", "")
    add_status_controller(driver, status_path, "ui_grid_pages", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_grid_pages_0")
    bind_tree(driver, tree.name, True)
    bind_tree(panel, grid_tree.name, True)
    menu.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_grid_pages_0", status_path, prepare=False)


def run_ui_grid_scrollbar_acceptance(status_path):
    """Object Grid View's Scrollbar in a menu: hidden while the items fit, then a handle a third of
    the bar at its top, and at its bottom once First Item shows the last row. The bar is a scaled
    cube and the handle is parented with Blender's parent inverse."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_UIScrollDriver")
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_UIScrollCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    panel = create_cube(f"{PREFIX}_UIScrollPanel")
    panel.location = (0.0, 0.0, 39.0)
    panel.scale = (4.0, 2.0, 0.1)
    slot = create_cube(f"{PREFIX}_UIScrollSlot")
    slot.location = (-1.5, 0.0, 40.0)
    slot.scale = (0.4, 0.4, 0.1)
    bar = create_cube(f"{PREFIX}_UIScrollBar")
    bar.location = (1.5, 0.0, 40.0)
    bar.scale = (0.1, 1.0, 0.1)
    handle = create_cube(f"{PREFIX}_UIScrollHandle")
    handle.location = (1.5, 0.5, 40.2)
    handle.scale = (0.15, 0.25, 0.1)
    bpy.context.view_layer.update()
    handle.parent = bar
    handle.matrix_parent_inverse = bar.matrix_world.inverted()
    menu = make_ui_collection(f"{PREFIX}_UIScrollMenu", (panel, slot, bar, handle))

    grid_tree = new_logic_tree(f"{PREFIX}_UIScrollTree")
    shown = grid_tree.nodes.new("LogicNativeGetGlobalProperty")
    shown.inputs["Category"].default_value = "Inventory"
    shown.inputs["Property"].default_value = "items"
    first = grid_tree.nodes.new("LogicNativeGetGamePropertyInt")
    first.inputs["Object"].default_value = driver
    first.inputs["Property"].default_value = "first"
    grid = grid_tree.nodes.new("LogicNativeObjectGridView")
    grid.inputs["Columns"].default_value = 2
    grid.inputs["Cull Outside"].default_value = False
    grid.inputs["Slot Template"].default_value = slot
    grid.inputs["Rows"].default_value = 1
    grid.inputs["Spacing"].default_value = (1.0, 0.0, 0.0)
    grid.inputs["Scrollbar"].default_value = bar
    grid_tree.links.new(shown.outputs["Value"], grid.inputs["Items"])
    grid_tree.links.new(first.outputs[0], grid.inputs["First Item"])

    # Open the menu with 2 items, add 4 more, show the last row, close.
    tree = new_logic_tree(f"{PREFIX}_UIScrollDriverTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    chain = []

    def add_items(names):
        # The driver's own inventory property, edited in place, then shared with the menu.
        for name in names:
            add = tree.nodes.new("LogicNativeInventory")
            add.operation = "ADD_ITEM"
            add.inputs["Property"].default_value = "grid_items"
            add.inputs["Item"].default_value = name
            chain.append((add, "Done"))
        current = tree.nodes.new("LogicNativeGetGameProperty")
        current.inputs["Property"].default_value = "grid_items"
        store = tree.nodes.new("LogicNativeSetGlobalProperty")
        store.inputs["Category"].default_value = "Inventory"
        store.inputs["Property"].default_value = "items"
        tree.links.new(current.outputs["Value"], store.inputs["Value"])
        chain.append((store, "Done"))

    def set_int(name, value):
        node = tree.nodes.new("LogicNativeSetGamePropertyInt")
        node.inputs["Property"].default_value = name
        node.inputs["Value"].default_value = value
        chain.append((node, "Done"))

    def wait(seconds):
        node = tree.nodes.new("LogicNativeDelay")
        node.inputs["Delay"].default_value = seconds
        chain.append((node, "Done"))

    add_items(("I1", "I2"))
    open_menu = tree.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    chain.append((open_menu, "Opened"))
    wait(0.3)
    set_int("ui_phase", 1)
    wait(0.5)
    add_items(("I3", "I4", "I5", "I6"))
    set_int("ui_phase", 2)
    wait(0.5)
    set_int("first", 4)
    set_int("ui_phase", 3)
    wait(0.5)
    set_int("ui_phase", 4)
    close_menu = tree.nodes.new("LogicNativeCloseObjectMenu")
    chain.append((close_menu, "Closed"))
    wait(0.2)
    mark_done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_done.inputs["Property"].default_value = "ui_cycle_done"
    mark_done.inputs["Value"].default_value = True
    chain.append((mark_done, "Done"))
    tree.links.new(on_init.outputs["Out"], flow_input(chain[0][0]))
    for (node, output), (next_node, _next_output) in zip(chain, chain[1:]):
        tree.links.new(node.outputs[output], flow_input(next_node))

    set_game_property(driver, "ui_phase", "INT", 0)
    set_game_property(driver, "first", "INT", 0)
    set_game_property(driver, "ui_cycle_done", "BOOL", False)
    set_game_property(driver, "ui_scroll_seen", "STRING", "{}")
    add_status_controller(driver, status_path, "ui_grid_scrollbar", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_grid_scrollbar_0")
    bind_tree(driver, tree.name, True)
    bind_tree(panel, grid_tree.name, True)
    menu.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_grid_scrollbar_0", status_path, prepare=False)


def run_game_property_types_acceptance(status_path, output_dir):
    """Vector, Color, Object, Choice, List and Table game properties: Logic Nodes and Python read
    and write them, Save Game restores them, and removing an object empties references to it."""
    reset_scene()
    configure_game_settings(True)
    driver = create_cube(f"{PREFIX}_PropsDriver")
    target = create_cube(f"{PREFIX}_PropsTarget")
    target.location = (3.0, 0.0, 0.0)
    target.game.use_save_game_object_state = True
    other = create_cube(f"{PREFIX}_PropsOther")
    other.location = (-3.0, 0.0, 0.0)
    # Values written over the saved ones between Save and Load.
    set_game_property(target, "spot", "VECTOR", (7.0, 8.0, 9.0))
    set_game_property(target, "shade", "COLOR", (0.1, 0.2, 0.3, 1.0))
    text_curve = bpy.data.curves.new(f"{PREFIX}_PropsText", "FONT")
    text_curve.body = "Hello"
    text = bpy.data.objects.new(f"{PREFIX}_PropsText", text_curve)
    bpy.context.scene.collection.objects.link(text)
    set_game_property(text, "Text", "STRING", "")
    # Add Object copies must point at their own copied child, and keep outside references.
    spawn = create_cube(f"{PREFIX}_PropsSpawn")
    spawn_part = create_cube(f"{PREFIX}_PropsSpawnPart")
    spawn_part.parent = spawn
    set_game_property(spawn, "part", "OBJECT", spawn_part)
    set_game_property(spawn, "outside", "OBJECT", other)
    spawn.hide_viewport = True
    spawn_part.hide_viewport = True

    def edit(name, operator, **kwargs):
        index = list(driver.game.properties).index(driver.game.properties[name])
        set_active_object(driver)
        with bpy.context.temp_override(object=driver, active_object=driver):
            result = getattr(bpy.ops.object, operator)(index=index, **kwargs)
        require(result == {"FINISHED"}, f"{operator} failed for {name!r}: {result}")

    saved = []
    saved.append(set_game_property(driver, "vec", "VECTOR", (1.0, 2.0, 3.0)))
    saved.append(set_game_property(driver, "tint", "COLOR", (0.5, 0.25, 1.0, 1.0)))
    saved.append(set_game_property(driver, "target", "OBJECT", target))
    saved.append(set_game_property(driver, "name_ref", "OBJECT", other))
    mode = set_game_property(driver, "mode", "CHOICE", 'A')
    edit("mode", "game_property_item_add")
    mode.options[-1].name = "Hard"
    mode.value = 'Hard'
    saved.append(mode)
    set_active_object(driver)
    with bpy.context.temp_override(object=driver, active_object=driver):
        bpy.ops.object.game_property_new(type='LIST', name="numbers")
    numbers = driver.game.properties["numbers"]
    numbers.item_type = 'INT'
    for value in (3, 5, 7):
        edit("numbers", "game_property_item_add")
        numbers.elements[-1].value = value
    saved.append(numbers)
    with bpy.context.temp_override(object=driver, active_object=driver):
        bpy.ops.object.game_property_new(type='TABLE', name="items")
    items = driver.game.properties["items"]
    for column_name, column_type in (("item", 'STRING'), ("count", 'INT'), ("owner", 'OBJECT')):
        edit("items", "game_property_item_add", target='COLUMN')
        items.columns[-1].name = column_name
        items.columns[-1].type = column_type
    for item, count, owner in (("Apple", 3, target), ("Sword", 1, None)):
        edit("items", "game_property_item_add")
        cells = items.rows[-1].cells
        cells[0].value = item
        cells[1].value = count
        cells[2].value = owner
    saved.append(items)
    for prop in saved:
        prop.use_save_game = True
    save_path = str(output_dir / f"{PREFIX}_GamePropertyTypes.sav")
    if Path(save_path).exists():
        Path(save_path).unlink()

    tree = new_logic_tree(f"{PREFIX}_GamePropertyTypesTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    chain = []

    def get_property(name, obj=None):
        node = tree.nodes.new("LogicNativeGetGameProperty")
        node.inputs["Property"].default_value = name
        if obj is not None:
            node.inputs["Object"].default_value = obj
        return node.outputs["Value"]

    def set_property(name, value_output, obj=None):
        node = tree.nodes.new("LogicNativeSetGameProperty")
        node.inputs["Property"].default_value = name
        if obj is not None:
            node.inputs["Object"].default_value = obj
        tree.links.new(value_output, node.inputs["Value"])
        chain.append((node, "Done"))
        return node

    add_spawn = tree.nodes.new("LogicNativeAddObject")
    add_spawn.inputs["Object to Add"].default_value = spawn
    add_spawn.inputs["Life"].default_value = 0.0
    chain.append((add_spawn, "Done"))
    set_property("spawned", add_spawn.outputs["Added Object"])
    set_property("copied_list", get_property("numbers"))
    set_property("copied_target", get_property("target"))
    add_item = tree.nodes.new("LogicNativeInventory")
    add_item.operation = 'ADD_ITEM'
    add_item.inputs["Property"].default_value = "items"
    add_item.inputs["Item"].default_value = "Apple"
    add_item.inputs["Amount"].default_value = 2
    chain.append((add_item, "Done"))
    set_property("from_driver", get_property("vec"), target)
    # Older nodes on the new kinds: a property keeps its kind, and typed reads agree.
    modify = tree.nodes.new("LogicNativeModifyProperty")
    modify.inputs["Property"].default_value = "numbers"
    modify.inputs["Value"].default_value = 1.0
    chain.append((modify, "Done"))
    set_float = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_float.inputs["Property"].default_value = "vec"
    set_float.inputs["Value"].default_value = 9.0
    chain.append((set_float, "Done"))
    for name, obj, store in (("vec", None, "own_vec_float"),
                             ("spot", target, "other_vec_float")):
        get_float = tree.nodes.new("LogicNativeGetGamePropertyFloat")
        get_float.inputs["Property"].default_value = name
        if obj is not None:
            get_float.inputs["Object"].default_value = obj
        set_store = tree.nodes.new("LogicNativeSetGamePropertyFloat")
        set_store.inputs["Property"].default_value = store
        tree.links.new(get_float.outputs[0], set_store.inputs["Value"])
        chain.append((set_store, "Done"))
    save_game = tree.nodes.new("LogicNativeSaveGame")
    save_game.inputs["Path"].default_value = save_path
    chain.append((save_game, "Done"))
    empty_list = tree.nodes.new("LogicNativeEmptyList")
    set_property("numbers", empty_list.outputs["List"])
    set_property("items", empty_list.outputs["List"])
    owner = tree.nodes.new("LogicNativeGetOwner")
    set_property("target", owner.outputs["Owner Object"])
    # The other saved kinds change too, so Load Game must restore each of them.
    set_property("vec", get_property("spot", target))
    set_property("tint", get_property("shade", target))
    set_property("name_ref", owner.outputs["Owner Object"])
    set_mode = tree.nodes.new("LogicNativeSetGamePropertyString")
    set_mode.inputs["Property"].default_value = "mode"
    set_mode.inputs["Value"].default_value = "A"
    chain.append((set_mode, "Done"))
    load_game = tree.nodes.new("LogicNativeLoadGame")
    load_game.inputs["Path"].default_value = save_path
    chain.append((load_game, "Done"))
    tree.links.new(on_init.outputs["Out"], flow_input(chain[0][0]))
    for (node, output), (next_node, _next_output) in zip(chain, chain[1:]):
        tree.links.new(node.outputs[output], flow_input(next_node))
    on_loaded = tree.nodes.new("LogicNativeOnGameLoaded")
    loaded = tree.nodes.new("LogicNativeBranch")
    mark_loaded = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_loaded.inputs["Property"].default_value = "props_loaded"
    mark_loaded.inputs["Value"].default_value = True
    tree.links.new(on_loaded.outputs["Out"], loaded.inputs["Flow"])
    tree.links.new(on_loaded.outputs["Success"], loaded.inputs["Condition"])
    tree.links.new(loaded.outputs["True"], flow_input(mark_loaded))

    set_game_property(driver, "props_loaded", "BOOL", False)
    set_game_property(driver, "own_vec_float", "FLOAT", -1.0)
    set_game_property(driver, "other_vec_float", "FLOAT", -1.0)
    set_game_property(driver, "ui_cycle_done", "BOOL", False)
    set_game_property(driver, "props_report", "STRING", "")
    add_status_controller(driver, status_path, "game_property_types", required_frame=120)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "game_property_types_0")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "game_property_types_0", status_path, prepare=False)


def run_camera_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    point = (0.0, 0.0, 0.0)
    depth = 5.0
    fov = 60.0
    ortho_scale = 6.0
    target_camera = create_camera_looking_at(f"{PREFIX}_TargetCamera", (0.0, -5.0, 0.0), point)
    create_camera_looking_at(f"{PREFIX}_DefaultCamera", (4.0, -6.0, 3.0), point)
    tree = build_camera_state_tree(
        f"{PREFIX}_CameraStateTree", target_camera, point, depth, fov, ortho_scale)
    target = create_cube(f"{PREFIX}_CameraState")
    add_status_controller(target, status_path, "camera_state", required_frame=4)
    set_game_property(target, "__ln_acceptance_tolerance", "FLOAT", 0.05)
    set_game_property(target, "__ln_acceptance_expected_active_camera", "STRING",
                      target_camera.name)
    set_game_property(target, "__ln_acceptance_expected_fov", "FLOAT", fov)
    set_game_property(target, "__ln_acceptance_expected_ortho_scale", "FLOAT", ortho_scale)
    set_game_property(target, "__ln_acceptance_expected_screen_x", "FLOAT", 0.5)
    set_game_property(target, "__ln_acceptance_expected_screen_y", "FLOAT", 0.5)
    set_game_property(target, "__ln_acceptance_expected_depth", "FLOAT", depth)
    set_game_property(target, "active_camera_present", "BOOL", False)
    for name in ("screen_x", "screen_y", "world_x", "world_y", "world_z"):
        set_game_property(target, name, "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "camera_state_0", status_path)


def chain_flows(tree, start_output, nodes):
    """Run nodes one after the other: each starts from the previous one's Done."""
    tree.links.new(start_output, flow_input(nodes[0]))
    for node, next_node in zip(nodes, nodes[1:]):
        tree.links.new(node.outputs[0], flow_input(next_node))


def add_player_view_reads(tree, on_update, user, outputs):
    """Get Player View for one player; outputs maps its output names to property names. A str
    user is a property read while the game runs."""
    view = tree.nodes.new("LogicNativeGetPlayerView")
    if isinstance(user, str):
        read_user = tree.nodes.new("LogicNativeGetGamePropertyInt")
        read_user.inputs["Property"].default_value = user
        tree.links.new(read_user.outputs[0], view.inputs["User"])
    else:
        view.inputs["User"].default_value = user
    setters = {
        "BOOL": "LogicNativeSetGamePropertyBool",
        "FLOAT": "LogicNativeSetGamePropertyFloat",
        "INT": "LogicNativeSetGamePropertyInt",
    }
    for output_name, (property_name, property_type) in outputs.items():
        setter = tree.nodes.new(setters[property_type])
        setter.inputs["Property"].default_value = property_name
        tree.links.new(on_update.outputs["Out"], flow_input(setter))
        tree.links.new(view.outputs[output_name], setter.inputs["Value"])
    return view


def build_split_screen_tree(name, cameras, minimap):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    chain = []
    for user, camera in enumerate(cameras):
        set_camera = tree.nodes.new("LogicNativeSetCamera")
        set_camera.inputs["Camera"].default_value = camera
        set_camera.inputs["User"].default_value = user
        chain.append(set_camera)
    split = tree.nodes.new("LogicNativeSetSplitScreen")
    split.split_three_players = "FAVOR_TOP"
    split.split_field_of_view = "CROP"
    split.inputs["Divider"].default_value = 2
    chain.append(split)
    viewport = tree.nodes.new("LogicNativeSetCameraViewport")
    viewport.inputs["Camera"].default_value = minimap
    viewport.inputs["Left"].default_value = 0.7
    viewport.inputs["Top"].default_value = 0.05
    viewport.inputs["Width"].default_value = 0.25
    viewport.inputs["Height"].default_value = 0.25
    chain.append(viewport)
    chain_flows(tree, on_init.outputs["Out"], chain)
    add_player_view_reads(tree, on_update, "watch_user", {
        "Shown": ("p2_shown", "BOOL"),
        "Left": ("p2_left", "FLOAT"),
        "Top": ("p2_top", "FLOAT"),
        "Width": ("p2_width", "FLOAT"),
        "Height": ("p2_height", "FLOAT"),
        "Views": ("views", "INT"),
    })
    return tree


def build_split_screen_leave_tree(name, cameras):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    chain = []
    for user, camera in enumerate(cameras):
        set_camera = tree.nodes.new("LogicNativeSetCamera")
        set_camera.inputs["Camera"].default_value = camera
        set_camera.inputs["User"].default_value = user
        chain.append(set_camera)
    # After the four views were drawn: remove player 2's camera, clear player 3's view.
    delay = tree.nodes.new("LogicNativeDelay")
    delay.inputs["Delay"].default_value = 0.3
    chain.append(delay)
    remove_camera = tree.nodes.new("LogicNativeRemoveObject")
    remove_camera.inputs["Object"].default_value = cameras[2]
    chain.append(remove_camera)
    clear_player = tree.nodes.new("LogicNativeSetCamera")
    clear_player.inputs["User"].default_value = 3
    chain.append(clear_player)
    mark_done = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_done.inputs["Property"].default_value = "left_done"
    mark_done.inputs["Value"].default_value = True
    chain.append(mark_done)
    chain_flows(tree, on_init.outputs["Out"], chain)
    add_player_view_reads(tree, on_update, 3, {
        "Shown": ("p3_shown", "BOOL"),
        "Views": ("views", "INT"),
    })
    return tree


def build_split_screen_players_tree(name, cameras, hud, idle_hud, speaker):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")

    def read_int(property_name):
        read = tree.nodes.new("LogicNativeGetGamePropertyInt")
        read.inputs["Property"].default_value = property_name
        return read.outputs[0]

    chain = []
    for user, camera in enumerate(cameras):
        set_camera = tree.nodes.new("LogicNativeSetCamera")
        set_camera.inputs["Camera"].default_value = camera
        set_camera.inputs["User"].default_value = user
        chain.append(set_camera)
    give = tree.nodes.new("LogicNativeSetPlayerGamepad")
    give.inputs["User"].default_value = 1
    give.inputs["Gamepad"].default_value = 3
    chain.append(give)
    sound = load_acceptance_test_sound()
    for listener in ("NEAREST_PLAYER", "PLAYER"):
        play = tree.nodes.new("LogicNativePlaySound3D")
        play.sound = sound
        play.sound_listener = listener
        play.inputs["Speaker"].default_value = speaker
        play.inputs["Volume"].default_value = 0.1
        play.inputs["Loop"].default_value = True
        if listener == "PLAYER":
            tree.links.new(read_int("hud_user"), play.inputs["User"])
        chain.append(play)
    mark_started = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_started.inputs["Property"].default_value = "sound_started"
    mark_started.inputs["Value"].default_value = True
    chain.append(mark_started)
    chain_flows(tree, on_init.outputs["Out"], chain)

    for anchored, user in ((hud, "hud_user"), (idle_hud, 5)):
        anchor = tree.nodes.new("LogicNativeScreenAnchor")
        anchor.screen_anchor = "TOP_LEFT"
        anchor.screen_area = "PLAYER_VIEW"
        anchor.inputs["Object"].default_value = anchored
        anchor.inputs["Depth"].default_value = 5.0
        if isinstance(user, str):
            tree.links.new(read_int(user), anchor.inputs["User"])
        else:
            anchor.inputs["User"].default_value = user
        tree.links.new(on_update.outputs["Out"], flow_input(anchor))

    for user in (1, 2, 3):
        pad = tree.nodes.new("LogicNativeGetPlayerGamepad")
        pad.inputs["User"].default_value = user
        outputs = [("Gamepad", f"p{user}_pad", "LogicNativeSetGamePropertyInt")]
        if user == 1:
            outputs.append(("Joined", "p1_joined", "LogicNativeSetGamePropertyBool"))
        for output_name, property_name, setter_idname in outputs:
            setter = tree.nodes.new(setter_idname)
            setter.inputs["Property"].default_value = property_name
            tree.links.new(on_update.outputs["Out"], flow_input(setter))
            tree.links.new(pad.outputs[output_name], setter.inputs["Value"])
    return tree


def run_camera_renderers_acceptance(status_path):
    """Camera renderers (2026-10-14): two same-size view cameras with no per-view history draw
    through one shared renderer, a Use Workbench camera through a Workbench one; after 6 seconds
    without drawing their renderers are given back, then made again when they draw. Each view
    pass also goes the direct way into the background (no 2D filters)."""
    reset_scene()
    configure_game_settings(True)
    create_cube(f"{PREFIX}_CRScene")
    view_a = create_camera_looking_at(f"{PREFIX}_CRViewA", (0.0, 0.0, 12.0), (0.0, 0.01, 0.0))
    view_b = create_camera_looking_at(f"{PREFIX}_CRViewB", (3.0, 0.0, 12.0), (3.0, 0.01, 0.0))
    view_w = create_camera_looking_at(f"{PREFIX}_CRViewW", (0.0, -6.0, 6.0))
    main = create_camera_looking_at(f"{PREFIX}_CRMain", (0.0, -8.0, 4.0))
    bpy.context.scene.camera = main
    for camera in (view_a, view_b):
        for switch in ("disable_shadows", "disable_ray_tracing", "disable_volumetric_shadows",
                       "disable_motion_blur", "disable_extra_samples",
                       "disable_temporal_reprojection"):
            setattr(camera.data, switch, True)
    view_w.data.use_workbench = True
    driver = create_cube(f"{PREFIX}_CRDriver")
    driver.location = (0.0, 0.0, -3.0)
    add_status_controller(driver, status_path, "camera_renderers", required_frame=1,
                          required_seconds=14.0)
    for key, camera in (("a", view_a), ("b", view_b), ("w", view_w)):
        set_game_property(driver, "__ln_acceptance_cr_" + key, "STRING", camera.name)
    for name in ("cr_phase", "cr_frames_on", "cr_frames_off", "cr_frames_back"):
        set_game_property(driver, name, "INT", 0)
    set_game_property(driver, "cr_error", "STRING", "")
    run_game_once(driver, "camera_renderers_0", status_path)


def run_camera_minimap_options_acceptance(status_path):
    """Minimap options (2026-10-14), checked on screenshot pixels of twelve small views over the
    main view: a camera's Collection (Only, Hide; an Add Object copy from a hidden child
    collection and a collection instance's copy still count as in it), its Workbench colors
    (Object, set from Python while playing; Material; Attribute and Random since 2026-10-15),
    Lighting (Studio; MatCap with the scene's MatCap set through scene.display.game_matcap),
    Outline, Cavity, and a round view (Viewport Shape: Circle)."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    # Plain colors in the images: no view transform, a black world, a magenta outline.
    previous_view = scene.view_settings.view_transform
    scene.view_settings.view_transform = "Standard"
    if scene.world is None:
        scene.world = bpy.data.worlds.new(f"{PREFIX}_MOWorld")
    previous_world_color = tuple(scene.world.color)
    scene.world.color = (0.0, 0.0, 0.0)
    shading = scene.display.shading
    previous_outline = tuple(shading.object_outline_color)
    shading.object_outline_color = (1.0, 0.0, 1.0)
    # Both Screen cavity strengths 0, as in files from the default startup file (the game uses 1).
    previous_curvature = (shading.curvature_ridge_factor, shading.curvature_valley_factor)
    shading.curvature_ridge_factor = 0.0
    shading.curvature_valley_factor = 0.0

    yellow = bpy.data.materials.new(f"{PREFIX}_MOYellow")
    yellow.diffuse_color = (1.0, 1.0, 0.0, 1.0)
    bpy.ops.mesh.primitive_plane_add(size=20.0, location=(0.0, 0.0, 0.0))
    ground = bpy.context.object
    ground.name = f"{PREFIX}_MOGround"
    ground.color = (0.0, 0.0, 1.0, 1.0)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, location=(-2.0, 0.0, 1.0))
    marker = bpy.context.object
    marker.name = f"{PREFIX}_MOMarker"
    # Mid green: Screen cavity brightens edges, which a full 1.0 color can't show.
    marker.color = (0.0, 0.5, 0.0, 1.0)
    marker.data.materials.append(yellow)
    # A magenta color attribute, which only the Attribute view shows.
    paint = marker.data.color_attributes.new(f"{PREFIX}_MOPaint", "FLOAT_COLOR", "POINT")
    for item in paint.data:
        item.color = (1.0, 0.0, 1.0, 1.0)
    marker.data.color_attributes.active_color = paint
    # The scene's MatCap (set while the scene's own lighting is not MatCap): a red picture.
    previous_matcap = scene.display.game_matcap
    scene.display.game_matcap = "red_wax.exr"
    spawn = create_cube(f"{PREFIX}_MOSpawn")
    spawn.data.materials.append(yellow)
    markers = make_ui_collection(f"{PREFIX}_MOMarkers", (marker,))
    library = bpy.data.collections.new(f"{PREFIX}_MOLibrary")
    markers.children.link(library)
    library.objects.link(spawn)
    for source in tuple(spawn.users_collection):
        if source != library:
            source.objects.unlink(spawn)
    library.hide_viewport = True
    # A collection instance: an Empty in the markers' collection instancing a yellow cube kept far
    # away (the game copies the cube for it, KX_Scene::DupliGroupRecurse; objects of hidden
    # collections are not instanced there). The copy counts as in the Empty's collections.
    asset = create_cube(f"{PREFIX}_MOAsset")
    asset.location = (100.0, 0.0, 0.0)
    asset.scale = (0.5, 0.5, 0.5)
    asset.color = (1.0, 1.0, 0.0, 1.0)
    assets = make_ui_collection(f"{PREFIX}_MOAssets", (asset,))
    assets.instance_offset = (100.0, 0.0, 0.0)
    instancer = bpy.data.objects.new(f"{PREFIX}_MOInstancer", None)
    instancer.instance_type = "COLLECTION"
    instancer.instance_collection = assets
    instancer.location = (0.0, 2.5, 0.5)
    markers.objects.link(instancer)
    # A cyan cube outside the markers' collection that its own tree puts in it when the game
    # starts (Set Collection Member, 2026-10-15): the Only views show it, the Hide view not.
    joined = create_cube(f"{PREFIX}_MOJoined")
    joined.location = (0.0, -2.5, 0.5)
    joined.scale = (0.5, 0.5, 0.5)
    joined.color = (0.0, 0.6, 0.6, 1.0)
    join_tree = new_logic_tree(f"{PREFIX}_MOJoinTree")
    join_start = join_tree.nodes.new("LogicNativeOnInit")
    join = join_tree.nodes.new("LogicNativeSetCollectionMember")
    join.inputs["Collection"].default_value = markers
    join_tree.links.new(join_start.outputs["Out"], join.inputs["Flow"])
    bind_tree(joined, join_tree.name, True)

    def top_camera(name, **settings):
        camera = create_camera_looking_at(f"{PREFIX}_{name}", (0.0, 0.0, 10.0), (0.0, 0.01, 0.0))
        camera.data.type = "ORTHO"
        camera.data.ortho_scale = 10.0
        camera.data.use_workbench = True
        # Flat colors to read by color (new cameras are Studio since 2026-10-15).
        camera.data.workbench_lighting = "FLAT"
        camera.data.workbench_color = "OBJECT"
        for key, value in settings.items():
            setattr(camera.data, key, value)
        return camera

    cameras = {
        "only": top_camera("MOOnly", game_collection=markers),
        "hide": top_camera("MOHide", game_collection=markers, game_collection_mode="HIDE"),
        "material": top_camera("MOMaterial", workbench_color="MATERIAL"),
        "plain": top_camera("MOPlain"),
        "outline": top_camera("MOOutline", workbench_outline=True),
        "cavity": top_camera("MOCavity", game_collection=markers, workbench_cavity="BOTH"),
        "round": top_camera("MORound", game_collection=markers, viewport_shape="CIRCLE"),
        "screen": top_camera("MOScreen", game_collection=markers, workbench_cavity="SCREEN"),
        "studio": top_camera("MOStudio", game_collection=markers, workbench_lighting="STUDIO"),
        "matcap": top_camera("MOMatcap", game_collection=markers, workbench_lighting="MATCAP",
                             workbench_color="SINGLE"),
        "attribute": top_camera("MOAttribute", workbench_color="ATTRIBUTE"),
        "random": top_camera("MORandom", workbench_color="RANDOM"),
    }
    # The runners play from the 3D view's own view (not camera view): that is the main view behind
    # the small views. The scene camera is none of them.
    scene.camera = create_camera_looking_at(f"{PREFIX}_MOMain", (0.0, -12.0, 8.0))

    driver = create_cube(f"{PREFIX}_MODriver")
    driver.location = (0.0, 0.0, -3.0)
    add_status_controller(driver, status_path, "camera_minimap_options", required_frame=1,
                          required_seconds=8.0)
    for key, camera in cameras.items():
        set_game_property(driver, "__ln_acceptance_mo_" + key, "STRING", camera.name)
    set_game_property(driver, "__ln_acceptance_mo_spawn", "STRING", spawn.name)
    set_game_property(driver, "mo_phase", "INT", 0)
    set_game_property(driver, "mo_frames", "INT", 0)
    set_game_property(driver, "mo_error", "STRING", "")
    try:
        run_game_once(driver, "camera_minimap_options_0", status_path)
    finally:
        # The scene again: playing in the 3D view gives back a new copy of the file's data.
        scene = bpy.context.scene
        scene.view_settings.view_transform = previous_view
        if scene.world is not None:
            scene.world.color = previous_world_color
        scene.display.shading.object_outline_color = previous_outline
        scene.display.shading.curvature_ridge_factor = previous_curvature[0]
        scene.display.shading.curvature_valley_factor = previous_curvature[1]
        scene.display.game_matcap = previous_matcap


def run_camera_visibility_still_acceptance(status_path):
    """Set Camera Visibility in a scene where nothing moves (2026-10-25). A camera's renderer that
    has finished its anti-aliasing samples shows its last picture until Blender's depsgraph
    changes, so a cube hidden from the camera while nothing moved stayed drawn. The rule change
    now tells Blender (KX_Scene::NoteDrawChanged). Checked on screenshot pixels of the camera's
    view (EEVEE), drawn in a corner of the window."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    # Plain colors in the images: no view transform, a black world.
    previous_view = scene.view_settings.view_transform
    scene.view_settings.view_transform = "Standard"
    if scene.world is None:
        scene.world = bpy.data.worlds.new(f"{PREFIX}_CVSWorld")
    previous_world_color = tuple(scene.world.color)
    scene.world.color = (0.0, 0.0, 0.0)

    red = bpy.data.materials.new(f"{PREFIX}_CVSRed")
    red.use_nodes = True
    bsdf = red.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    bsdf.inputs["Emission Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    bsdf.inputs["Emission Strength"].default_value = 1.0
    cube = create_cube(f"{PREFIX}_CVSCube")
    cube.data.materials.append(red)
    # Hidden from the top camera from the start: a camera that hides objects draws through its
    # own renderer (KX_CameraRenderMemory::CanShareRenderer), so hiding the cube later keeps the
    # renderer. The first hide of a camera that shared one moves it to a fresh renderer, which
    # drew the change anyway.
    other = create_cube(f"{PREFIX}_CVSOther")
    other.location = (2.0, 0.0, 0.0)
    other.scale = (0.3, 0.3, 0.3)
    top = create_camera_looking_at(f"{PREFIX}_CVSTop", (0.0, 0.0, 10.0), (0.0, 0.01, 0.0))
    top.data.type = "ORTHO"
    top.data.ortho_scale = 6.0
    # The runners play from the 3D view's own view: the top camera is drawn over it.
    scene.camera = create_camera_looking_at(f"{PREFIX}_CVSMain", (0.0, -12.0, 8.0))

    tree = new_logic_tree(f"{PREFIX}_CVSTree")
    start = tree.nodes.new("LogicNativeOnInit")
    hide_other = tree.nodes.new("LogicNativeSetCameraVisibility")
    hide_other.inputs["Object"].default_value = other
    hide_other.inputs["Camera"].default_value = top
    hide_other.inputs["Visible"].default_value = False
    wait = tree.nodes.new("LogicNativeDelay")
    wait.inputs["Delay"].default_value = 1.5
    hide = tree.nodes.new("LogicNativeSetCameraVisibility")
    hide.inputs["Object"].default_value = cube
    hide.inputs["Camera"].default_value = top
    hide.inputs["Visible"].default_value = False
    tree.links.new(start.outputs["Out"], hide_other.inputs["Flow"])
    tree.links.new(start.outputs["Out"], wait.inputs["Flow"])
    tree.links.new(wait.outputs["Done"], hide.inputs["Flow"])

    # Out of the top camera's view, and not moved by the run (prepare=False).
    driver = create_cube(f"{PREFIX}_CVSDriver")
    driver.location = (20.0, 0.0, 0.0)
    add_status_controller(driver, status_path, "camera_visibility_still", required_frame=1,
                          required_seconds=6.0)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "camera_visibility_still_0")
    set_game_property(driver, "__ln_acceptance_cvs_camera", "STRING", top.name)
    set_game_property(driver, "cvs_phase", "INT", 0)
    set_game_property(driver, "cvs_error", "STRING", "")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    try:
        run_game_once(driver, "camera_visibility_still_0", status_path, prepare=False)
    finally:
        # The scene again: playing in the 3D view gives back a new copy of the file's data.
        scene = bpy.context.scene
        scene.view_settings.view_transform = previous_view
        if scene.world is not None:
            scene.world.color = previous_world_color


def run_collection_membership_acceptance(status_path, output_dir):
    """Collection membership while playing (2026-10-15). A node tree does each step when the
    driver (drive_collection_membership) sets its flag: Get Collection Objects counts Add Object
    copies (from a hidden child collection) and Set Collection Member's changes, with and without
    child collections; a collection linked into List Length and Loop From List is its objects,
    child collections included; Save Game keeps Set Collection Member's changes of Save Object
    State objects and Load Game puts them back; with a Set Collection Render Visibility override,
    new copies are drawn (they were hidden before) and an object put in a visible collection is
    drawn again."""
    reset_scene()
    configure_game_settings(True)
    ring = create_cube(f"{PREFIX}_CMRing")
    icon = create_cube(f"{PREFIX}_CMIcon")
    blip = create_cube(f"{PREFIX}_CMBlip")
    marker = create_cube(f"{PREFIX}_CMMarker")
    marker_child = create_cube(f"{PREFIX}_CMMarkerChild")
    enemy = create_cube(f"{PREFIX}_CMEnemy")
    driver = create_cube(f"{PREFIX}_CMDriver")
    for index, obj in enumerate((ring, icon, blip, marker, marker_child, enemy, driver)):
        obj.location = (3.0 * index - 9.0, 0.0, 0.0)
        set_game_property(obj, "cm_seen", "BOOL", False)
    # Ring directly in Minimap, Icon in its child collection, Blip a template in a hidden child
    # collection (its Add Object copies count as in it).
    minimap = make_ui_collection(f"{PREFIX}_CMMinimap", (ring,))
    icons = bpy.data.collections.new(f"{PREFIX}_CMIcons")
    library = bpy.data.collections.new(f"{PREFIX}_CMLibrary")
    for child, obj in ((icons, icon), (library, blip)):
        minimap.children.link(child)
        child.objects.link(obj)
        for source in tuple(obj.users_collection):
            if source != child:
                source.objects.unlink(obj)
    library.hide_viewport = True
    make_ui_collection(f"{PREFIX}_CMWorld", (marker, marker_child))
    bpy.context.view_layer.update()
    child_world = marker_child.matrix_world.copy()
    marker_child.parent = marker
    marker_child.matrix_world = child_world
    marker.game.use_save_game_object_state = True
    marker_child.game.use_save_game_object_state = True
    enemies = make_ui_collection(f"{PREFIX}_CMEnemies", (enemy,))
    # Collection instances: Instancer (in World) instances Assets' tree when the game starts;
    # EnemyNest (a template in a child collection of Enemies) and FriendNest (in Library) are
    # spawned while Enemies and Assets are hidden. The tree copies count as in their Empty's
    # collections, Set Collection Member's changes to the Empty included: FriendNest's tree is
    # drawn through its Empty only (it was hidden before 2026-10-15), EnemyNest's is not.
    tree_asset = create_cube(f"{PREFIX}_CMTree")
    tree_asset.location = (60.0, 0.0, 0.0)
    tree_asset.scale = (0.5, 0.5, 0.5)
    set_game_property(tree_asset, "cm_seen", "BOOL", False)
    assets = make_ui_collection(f"{PREFIX}_CMAssets", (tree_asset,))
    assets.instance_offset = (60.0, 0.0, 0.0)
    instancer = bpy.data.objects.new(f"{PREFIX}_CMInstancer", None)
    instancer.instance_type = "COLLECTION"
    instancer.instance_collection = assets
    instancer.location = (6.0, 4.0, 0.0)
    bpy.data.collections[f"{PREFIX}_CMWorld"].objects.link(instancer)
    set_game_property(instancer, "cm_seen", "BOOL", False)
    enemy_store = bpy.data.collections.new(f"{PREFIX}_CMEnemyStore")
    enemies.children.link(enemy_store)
    enemy_nest = bpy.data.objects.new(f"{PREFIX}_CMEnemyNest", None)
    enemy_nest.instance_type = "COLLECTION"
    enemy_nest.instance_collection = assets
    enemy_store.objects.link(enemy_nest)
    enemy_store.hide_viewport = True
    friend_nest = bpy.data.objects.new(f"{PREFIX}_CMFriendNest", None)
    friend_nest.instance_type = "COLLECTION"
    friend_nest.instance_collection = assets
    library.objects.link(friend_nest)
    save_path = str(output_dir / f"{PREFIX}_CollectionMembership.sav")
    if Path(save_path).exists():
        Path(save_path).unlink()

    tree = new_logic_tree(f"{PREFIX}_CMTree")
    get_minimap = tree.nodes.new("LogicNativeGetCollection")
    get_minimap.inputs["Collection"].default_value = minimap

    def set_property(node_type, name, target=driver):
        setter = tree.nodes.new(node_type)
        setter.inputs["Object"].default_value = target
        setter.inputs["Property"].default_value = name
        return setter

    # Steps on the Logic + Physics step, which Add Object needs. The driver waits for the tree to
    # clear each step's flag: the step's property writes apply with that clear.
    on_step = tree.nodes.new("LogicNativeOnFixedUpdate")

    def step(flag):
        """The flag's branch: its True output runs the step, and clears the flag."""
        read = tree.nodes.new("LogicNativeGetGamePropertyBool")
        read.inputs["Object"].default_value = driver
        read.inputs["Property"].default_value = flag
        branch = tree.nodes.new("LogicNativeBranch")
        tree.links.new(on_step.outputs["Out"], branch.inputs["Flow"])
        tree.links.new(read.outputs["Value"], branch.inputs["Condition"])
        clear = set_property("LogicNativeSetGamePropertyBool", flag)
        clear.inputs["Value"].default_value = False
        tree.links.new(branch.outputs["True"], flow_input(clear))
        set_game_property(driver, flag, "BOOL", False)
        return branch.outputs["True"]

    # The record step: counts of the collection's own objects, with its child collections, and of
    # the collection linked straight into a list input; Loop From List over the collection marks
    # each object in it seen.
    record = step("cm_do_mark")
    for name, include_children in (("cm_direct", False), ("cm_all", True), ("cm_linked", None)):
        length = tree.nodes.new("LogicNativeListLength")
        if include_children is None:
            tree.links.new(get_minimap.outputs["Collection"], length.inputs["List"])
        else:
            objects = tree.nodes.new("LogicNativeGetCollectionObjects")
            objects.inputs["Include Child Collections"].default_value = include_children
            tree.links.new(get_minimap.outputs["Collection"], objects.inputs["Collection"])
            tree.links.new(objects.outputs["Objects"], length.inputs["List"])
        setter = set_property("LogicNativeSetGamePropertyInt", name)
        tree.links.new(record, flow_input(setter))
        tree.links.new(length.outputs["Length"], setter.inputs["Value"])
    loop = tree.nodes.new("LogicNativeLoopFromList")
    tree.links.new(record, loop.inputs["Flow"])
    tree.links.new(get_minimap.outputs["Collection"], loop.inputs["List"])
    mark_seen = tree.nodes.new("LogicNativeSetGamePropertyBool")
    mark_seen.inputs["Property"].default_value = "cm_seen"
    mark_seen.inputs["Value"].default_value = True
    tree.links.new(loop.outputs["Loop"], flow_input(mark_seen))
    tree.links.new(loop.outputs["List Item"], mark_seen.inputs["Object"])

    spawn = tree.nodes.new("LogicNativeAddObject")
    spawn.inputs["Object to Add"].default_value = blip
    spawn.inputs["Copy Transform"].default_value = ring
    spawn.inputs["Life"].default_value = 0.0
    spawn.inputs["Full Copy"].default_value = False
    tree.links.new(step("cm_do_spawn"), spawn.inputs["Flow"])
    spawn_nests = step("cm_do_spawn_nest")
    for nest in (enemy_nest, friend_nest):
        spawn_nest = tree.nodes.new("LogicNativeAddObject")
        spawn_nest.inputs["Object to Add"].default_value = nest
        spawn_nest.inputs["Copy Transform"].default_value = enemy
        spawn_nest.inputs["Life"].default_value = 0.0
        spawn_nest.inputs["Full Copy"].default_value = False
        tree.links.new(spawn_nests, spawn_nest.inputs["Flow"])
    end_nest = tree.nodes.new("LogicNativeRemoveObject")
    end_nest.inputs["Object"].default_value = instancer
    tree.links.new(step("cm_do_end_nest"), flow_input(end_nest))
    for flag, target, member, include_children in (
        ("cm_do_add", marker, True, True),
        ("cm_do_remove", icon, False, False),
        ("cm_do_undo", marker, False, True),
        ("cm_do_nest", instancer, True, False),
        ("cm_do_join", enemy, True, False),
    ):
        set_member = tree.nodes.new("LogicNativeSetCollectionMember")
        set_member.inputs["Object"].default_value = target
        set_member.inputs["Collection"].default_value = minimap
        set_member.inputs["Member"].default_value = member
        set_member.inputs["Include Children"].default_value = include_children
        tree.links.new(step(flag), set_member.inputs["Flow"])
    hide_step = step("cm_do_hide")
    for hidden_collection in (enemies, assets):
        hide = tree.nodes.new("LogicNativeSetCollectionVisibility")
        hide.inputs["Collection"].default_value = hidden_collection
        hide.inputs["Visible"].default_value = False
        tree.links.new(hide_step, hide.inputs["Flow"])
    for flag, node_type, prefix in (("cm_do_save", "LogicNativeSaveGame", "cm_save"),
                                    ("cm_do_load", "LogicNativeLoadGame", "cm_load")):
        game_state = tree.nodes.new(node_type)
        game_state.inputs["Path"].default_value = save_path
        tree.links.new(step(flag), game_state.inputs["Flow"])
        ok = set_property("LogicNativeSetGamePropertyBool", prefix + "_ok")
        tree.links.new(game_state.outputs["Done"], flow_input(ok))
        tree.links.new(game_state.outputs["Success"], ok.inputs["Value"])
        done = set_property("LogicNativeSetGamePropertyBool", prefix + "_done")
        done.inputs["Value"].default_value = True
        tree.links.new(ok.outputs["Done"], flow_input(done))
        set_game_property(driver, prefix + "_ok", "BOOL", False)
        set_game_property(driver, prefix + "_done", "BOOL", False)
    bind_tree(driver, tree.name, True)

    add_status_controller(driver, status_path, "collection_membership", required_frame=1,
                          required_seconds=15.0)
    for key, obj in (("ring", ring), ("icon", icon), ("blip", blip), ("marker", marker),
                     ("marker_child", marker_child), ("enemy", enemy), ("tree", tree_asset),
                     ("instancer", instancer), ("enemy_nest", enemy_nest),
                     ("friend_nest", friend_nest)):
        set_game_property(driver, "__ln_acceptance_cm_" + key, "STRING", obj.name)
    for name, value in (("cm_direct", -1), ("cm_all", -1), ("cm_linked", -1), ("cm_phase", 0),
                        ("cm_wait", 0)):
        set_game_property(driver, name, "INT", value)
    set_game_property(driver, "cm_cleared", "BOOL", False)
    set_game_property(driver, "cm_error", "STRING", "")
    try:
        run_game_once(driver, "collection_membership_0", status_path)
    finally:
        if Path(save_path).exists():
            Path(save_path).unlink()


def run_split_screen_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    create_cube(f"{PREFIX}_SplitScene")
    minimap = create_camera_looking_at(f"{PREFIX}_Minimap", (0.0, 0.0, 12.0), (0.0, 0.01, 0.0))
    cameras = [
        create_camera_looking_at(f"{PREFIX}_SplitCamera{index}", location)
        for index, location in enumerate(((0.0, -8.0, 3.0), (8.0, 0.0, 3.0), (-8.0, 0.0, 3.0)))
    ]
    bpy.context.scene.camera = cameras[0]
    tree = build_split_screen_tree(f"{PREFIX}_SplitScreenTree", cameras, minimap)
    driver = create_cube(f"{PREFIX}_SplitDriver")
    driver.location = (0.0, 0.0, -3.0)
    add_status_controller(driver, status_path, "split_screen", required_frame=8,
                          required_seconds=2.0)
    for index, camera in enumerate(cameras):
        set_game_property(driver, f"__ln_acceptance_camera_{index}", "STRING", camera.name)
    set_game_property(driver, "__ln_acceptance_minimap", "STRING", minimap.name)
    set_game_property(driver, "watch_user", "INT", 2)
    set_game_property(driver, "p2_shown", "BOOL", False)
    for property_name in ("p2_left", "p2_top", "p2_width", "p2_height"):
        set_game_property(driver, property_name, "FLOAT", -1.0)
    set_game_property(driver, "views", "INT", 0)
    bind_tree(driver, tree.name, True)
    run_game_once(driver, "split_screen_0", status_path)


def run_lod_nearest_view_acceptance(status_path):
    """Level of detail with several cameras: split screen (players 0 and 1 far apart), a minimap
    and a menu camera far above. Each rock takes the detail its nearest player view needs, once a
    frame: before, each camera's pass chose it again, so the meshes (and LOD physics shapes)
    swapped at every pass and the menu camera, drawn last, left every rock on its far mesh."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    cameras = [create_camera_looking_at(f"{PREFIX}_LodCamera0", (0.0, -8.0, 3.0)),
               create_camera_looking_at(f"{PREFIX}_LodCamera1", (200.0, -8.0, 3.0),
                                        (200.0, 0.0, 0.0))]
    minimap = create_camera_looking_at(f"{PREFIX}_LodMinimap", (0.0, 0.0, 300.0), (0.0, 0.01, 0.0))
    menu_camera = create_camera_looking_at(f"{PREFIX}_LodMenuCamera", (0.0, 0.0, 600.0),
                                           (0.0, 0.01, 0.0))
    scene.camera = cameras[0]
    panel = create_cube(f"{PREFIX}_LodMenuPanel")
    panel.location = (0.0, 0.0, 580.0)
    menu = make_ui_collection(f"{PREFIX}_LodMenu", (panel,))

    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=1, location=(0.0, 400.0, 0.0))
    low = bpy.context.object
    low.name = f"{PREFIX}_LodLow"
    far_mesh = create_cube(f"{PREFIX}_LodCube")
    far_mesh.location = (0.0, 410.0, 0.0)
    meshes = make_ui_collection(f"{PREFIX}_LodMeshes", (low, far_mesh))
    driver = create_cube(f"{PREFIX}_LodDriver")
    driver.location = (0.0, 0.0, -30.0)
    # Near player 0, near player 1, 33 m from player 0 (middle detail), far from both.
    for name, location in (("Near0", (0.0, 0.0, 0.5)), ("Near1", (200.0, 0.0, 0.5)),
                           ("Mid", (0.0, 25.0, 0.5)), ("Far", (100.0, 100.0, 0.5))):
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, location=location)
        rock = bpy.context.object
        rock.name = f"{PREFIX}_Lod{name}"
        rock.game.physics_type = "STATIC"
        rock.use_lod_physics = True
        with bpy.context.temp_override(object=rock, active_object=rock):
            bpy.ops.object.lod_add()
            bpy.ops.object.lod_add()
        rock.lod_levels[1].object = low
        rock.lod_levels[1].distance = 20.0
        rock.lod_levels[2].object = far_mesh
        rock.lod_levels[2].distance = 50.0
        set_game_property(driver, f"__ln_acceptance_mesh_{name}", "STRING", rock.data.name)
    set_game_property(driver, "__ln_acceptance_mesh_low", "STRING", low.data.name)
    set_game_property(driver, "__ln_acceptance_mesh_far", "STRING", far_mesh.data.name)

    tree = new_logic_tree(f"{PREFIX}_LodTree")
    chain = []
    set_camera = tree.nodes.new("LogicNativeSetCamera")
    set_camera.inputs["Camera"].default_value = cameras[1]
    set_camera.inputs["User"].default_value = 1
    chain.append(set_camera)
    split = tree.nodes.new("LogicNativeSetSplitScreen")
    split.split_two_players = "SIDE_BY_SIDE"
    chain.append(split)
    viewport = tree.nodes.new("LogicNativeSetCameraViewport")
    viewport.inputs["Camera"].default_value = minimap
    chain.append(viewport)
    overlay = tree.nodes.new("LogicNativeSetOverlayCollection")
    overlay.inputs["Camera"].default_value = menu_camera
    overlay.inputs["Collection"].default_value = menu
    chain.append(overlay)
    chain_flows(tree, tree.nodes.new("LogicNativeOnInit").outputs["Out"], chain)
    add_status_controller(driver, status_path, "lod_nearest_view", required_frame=8,
                          required_seconds=1.0)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "lod_nearest_view_0")
    bind_tree(driver, tree.name, True)
    menu.hide_viewport = True
    meshes.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "lod_nearest_view_0", status_path, prepare=False)


def run_lod_view_size_acceptance(status_path):
    """Split Screen Quality > Level of Detail by View Size: a rock 15 m from player 0 switches
    to its 20 m mesh in a 2 x 2 grid view (objects look half as big there), keeps its full mesh
    with the setting off, and side by side (Crop keeps the views' object size)."""
    for run, (players, by_view_size, level) in enumerate(((4, True, 1), (4, False, 0),
                                                          (2, True, 0))):
        reset_scene()
        configure_game_settings(True)
        scene = bpy.context.scene
        scene.game_settings.use_split_lod_by_view_size = by_view_size
        cameras = [create_camera_looking_at(f"{PREFIX}_ViewSizeCamera{index}",
                                            (300.0 * index, -8.0, 3.0), (300.0 * index, 0.0, 0.0))
                   for index in range(players)]
        scene.camera = cameras[0]
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=1, location=(0.0, 400.0, 0.0))
        low = bpy.context.object
        low.name = f"{PREFIX}_ViewSizeLow"
        far_mesh = create_cube(f"{PREFIX}_ViewSizeCube")
        far_mesh.location = (0.0, 410.0, 0.0)
        meshes = make_ui_collection(f"{PREFIX}_ViewSizeMeshes", (low, far_mesh))
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, location=(0.0, 7.0, 0.5))
        rock = bpy.context.object
        rock.name = f"{PREFIX}_ViewSizeRock"
        with bpy.context.temp_override(object=rock, active_object=rock):
            bpy.ops.object.lod_add()
            bpy.ops.object.lod_add()
        rock.lod_levels[1].object = low
        rock.lod_levels[1].distance = 20.0
        rock.lod_levels[2].object = far_mesh
        rock.lod_levels[2].distance = 50.0
        expected = (rock.data.name, low.data.name)[level]

        tree = new_logic_tree(f"{PREFIX}_ViewSizeTree")
        chain = []
        for user in range(1, players):
            set_camera = tree.nodes.new("LogicNativeSetCamera")
            set_camera.inputs["Camera"].default_value = cameras[user]
            set_camera.inputs["User"].default_value = user
            chain.append(set_camera)
        split = tree.nodes.new("LogicNativeSetSplitScreen")
        split.split_two_players = "SIDE_BY_SIDE"
        chain.append(split)
        chain_flows(tree, tree.nodes.new("LogicNativeOnInit").outputs["Out"], chain)
        driver = create_cube(f"{PREFIX}_ViewSizeDriver")
        driver.location = (0.0, 0.0, -30.0)
        run_id = f"lod_view_size_{run}"
        add_status_controller(driver, status_path, "lod_view_size", required_frame=8,
                              required_seconds=1.0)
        set_game_property(driver, "__ln_acceptance_run", "STRING", run_id)
        set_game_property(driver, "__ln_acceptance_rock", "STRING", rock.name)
        set_game_property(driver, "__ln_acceptance_mesh", "STRING", expected)
        bind_tree(driver, tree.name, True)
        meshes.hide_viewport = True
        set_active_object(driver)
        bpy.context.view_layer.update()
        run_game_once(driver, run_id, status_path, prepare=False)


def run_ui_element_players_acceptance(status_path):
    """UI Element Player and Any Player, two players side by side: Set UI Focus works only for
    the players who use an element, and not for a player whose camera does not draw it."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    cameras = [create_camera_looking_at(f"{PREFIX}_PlayersCamera{index}", location)
               for index, location in enumerate(((0.0, -10.0, 4.0), (0.0, 10.0, 4.0)))]
    scene.camera = cameras[0]
    elements = {}
    for name, x in (("P1", -2.0), ("Any", 2.0), ("Hidden", 0.0)):
        element = create_cube(f"{PREFIX}_Players{name}")
        element.location = (x, 0.0, 1.0)
        element.scale = (0.5, 0.5, 0.5)
        element.game.ui.use_element = True
        element.game.ui.action = name.lower()
        elements[name] = element
    elements["P1"].game.ui.player = 1
    elements["Any"].game.ui.use_any_player = True
    elements["Hidden"].game.ui.use_any_player = True

    tree = new_logic_tree(f"{PREFIX}_PlayersTree")
    set_camera = tree.nodes.new("LogicNativeSetCamera")
    set_camera.inputs["Camera"].default_value = cameras[1]
    set_camera.inputs["User"].default_value = 1
    split = tree.nodes.new("LogicNativeSetSplitScreen")
    split.split_two_players = "SIDE_BY_SIDE"
    hide = tree.nodes.new("LogicNativeSetCameraVisibility")
    hide.inputs["Object"].default_value = elements["Hidden"]
    hide.inputs["Camera"].default_value = cameras[1]
    wait = tree.nodes.new("LogicNativeDelay")
    wait.inputs["Delay"].default_value = 0.5
    chain_flows(tree, tree.nodes.new("LogicNativeOnInit").outputs["Out"],
                [set_camera, split, hide, wait])
    driver = create_cube(f"{PREFIX}_PlayersDriver")
    driver.location = (0.0, 0.0, -30.0)
    for name, element, user in (("p1_user1", "P1", 1), ("p1_user0", "P1", 0),
                                ("any_user2", "Any", 2), ("hidden_user1", "Hidden", 1),
                                ("hidden_user0", "Hidden", 0)):
        set_focus = tree.nodes.new("LogicNativeSetUIFocus")
        set_focus.inputs["Object"].default_value = elements[element]
        set_focus.inputs["User"].default_value = user
        tree.links.new(wait.outputs["Done"], flow_input(set_focus))
        for result in ("Done", "Failed"):
            record = tree.nodes.new("LogicNativeSetGamePropertyString")
            record.inputs["Property"].default_value = "focus_" + name
            record.inputs["Value"].default_value = result.lower()
            tree.links.new(set_focus.outputs[result], flow_input(record))
        set_game_property(driver, "focus_" + name, "STRING", "")
    add_status_controller(driver, status_path, "ui_element_players", required_frame=8,
                          required_seconds=3.0)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_element_players_0")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_element_players_0", status_path, prepare=False)


def run_ui_take_control_pointer_acceptance(status_path):
    """Take UI Control and Set Player Pointer through the whole node path (compile, run, flush,
    completion): one player at a time has the computer; the pointer shows its cursor on the
    screen, in front of the buttons, and hides it when turned off."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    camera = create_camera_looking_at(f"{PREFIX}_TCCamera", (0.0, -6.0, 1.5), (0.0, 0.0, 1.5))
    scene.camera = camera
    # A monitor standing up, facing -Y, with a button and a cursor in front of it.
    bpy.ops.mesh.primitive_plane_add(size=1.0, location=(0.0, 0.0, 1.5),
                                     rotation=(math.pi / 2, 0.0, 0.0))
    screen = bpy.context.object
    screen.name = f"{PREFIX}_TCScreen"
    screen.scale = (1.6, 1.0, 1.0)
    parts = {}
    for name, location, size in (("Button", (0.0, 0.0, 0.01), 0.3), ("Cursor", (0.4, 0.3, 0.02), 0.05)):
        bpy.ops.mesh.primitive_plane_add(size=size)
        part = bpy.context.object
        part.name = f"{PREFIX}_TC{name}"
        part.parent = screen
        part.matrix_parent_inverse.identity()
        part.location = location
        parts[name] = part
    parts["Button"].game.ui.use_element = True
    parts["Button"].game.ui.action = "tc"
    parts["Button"].game.ui.use_any_player = True

    tree = new_logic_tree(f"{PREFIX}_TCTree")
    driver = create_cube(f"{PREFIX}_TCDriver")
    driver.location = (0.0, 0.0, -30.0)

    def take(user, value=True):
        node = tree.nodes.new("LogicNativeTakeUIControl")
        node.inputs["Object"].default_value = screen
        node.inputs["User"].default_value = user
        node.inputs["Take"].default_value = value
        return node

    def record(output, name, value):
        node = tree.nodes.new("LogicNativeSetGamePropertyString")
        node.inputs["Property"].default_value = "tc_" + name
        node.inputs["Value"].default_value = value
        tree.links.new(output, flow_input(node))
        return node

    def wait(seconds):
        node = tree.nodes.new("LogicNativeDelay")
        node.inputs["Delay"].default_value = seconds
        tree.links.new(on_init.outputs["Out"], flow_input(node))
        return node

    def pointer(enabled):
        node = tree.nodes.new("LogicNativeSetPlayerPointer")
        node.inputs["User"].default_value = 1
        node.inputs["Enabled"].default_value = enabled
        node.inputs["Surface"].default_value = screen
        node.inputs["Cursor"].default_value = parts["Cursor"]
        return node

    on_init = tree.nodes.new("LogicNativeOnInit")
    take1 = take(1)
    tree.links.new(on_init.outputs["Out"], flow_input(take1))
    record(take1.outputs["Done"], "take1", "done")
    record(take1.outputs["Failed"], "take1", "failed")
    pointer_on = pointer(True)
    tree.links.new(take1.outputs["Done"], flow_input(pointer_on))
    take2 = take(2)
    tree.links.new(wait(0.4).outputs["Done"], flow_input(take2))
    record(take2.outputs["Done"], "take2", "done")
    record(take2.outputs["Failed"], "take2", "failed")
    result = tree.nodes.new("LogicNativeSetGamePropertyInt")
    result.inputs["Property"].default_value = "tc_take2_result"
    tree.links.new(take2.outputs["Failed"], flow_input(result))
    tree.links.new(take2.outputs["Result"], result.inputs["Value"])
    release = take(1, False)
    tree.links.new(wait(1.2).outputs["Done"], flow_input(release))
    record(release.outputs["Done"], "release", "done")
    take2_after = take(2)
    tree.links.new(release.outputs["Done"], flow_input(take2_after))
    record(take2_after.outputs["Done"], "take2_after", "done")
    pointer_off = pointer(False)
    tree.links.new(release.outputs["Done"], flow_input(pointer_off))

    for name in ("take1", "take2", "release", "take2_after"):
        set_game_property(driver, "tc_" + name, "STRING", "")
    set_game_property(driver, "tc_take2_result", "INT", -1)
    add_status_controller(driver, status_path, "ui_take_control_pointer", required_frame=8,
                          required_seconds=4.0)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_take_control_pointer_0")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_take_control_pointer_0", status_path, prepare=False)


def run_ui_view_center_acceptance(status_path):
    """Set Player Pointer's View Center through the whole node path: two players' cameras aim
    at one Any Player button. Player 1's camera sits inside their own body (which must not hide
    it); a wall hides it from player 2 until the wall is removed, and a box player 2's camera
    does not draw (Set Camera Visibility) never hides it. Player 1's menu stops their pointer.
    Set Cursor Visibility's Only in Menus shows the cursor only while that menu is open."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    cameras = [create_camera_looking_at(f"{PREFIX}_VCCamera{index}", (x, -5.0, 1.5), (x, 0.0, 1.5))
               for index, x in enumerate((-0.5, 0.5))]
    scene.camera = cameras[0]
    # Player 1's body: a box around their camera.
    body = create_cube(f"{PREFIX}_VCBody")
    body.location = (-0.5, -5.0, 1.5)
    body.scale = (0.3, 0.3, 0.3)
    bpy.context.view_layer.update()
    cameras[0].parent = body
    cameras[0].matrix_parent_inverse = body.matrix_world.inverted()
    wall = create_cube(f"{PREFIX}_VCWall")
    wall.location = (0.5, -2.5, 1.5)
    wall.scale = (0.3, 0.3, 0.3)
    veil = create_cube(f"{PREFIX}_VCVeil")
    veil.location = (0.5, -1.5, 1.5)
    veil.scale = (0.3, 0.3, 0.3)
    bpy.ops.mesh.primitive_plane_add(size=1.0, location=(0.0, 0.0, 1.5),
                                     rotation=(math.pi / 2, 0.0, 0.0))
    button = bpy.context.object
    button.name = f"{PREFIX}_VCButton"
    button.scale = (1.6, 1.0, 1.0)
    button.game.ui.use_element = True
    button.game.ui.action = "vc"
    button.game.ui.use_any_player = True

    driver = create_cube(f"{PREFIX}_VCDriver")
    driver.location = (0.0, 0.0, -30.0)
    menu_item = create_cube(f"{PREFIX}_VCMenuItem")
    menu_item.location = (0.0, 60.0, 0.0)
    menu_camera = create_camera_looking_at(f"{PREFIX}_VCMenuCamera", (0.0, 54.0, 0.0),
                                           menu_item.location)
    scene.camera = cameras[0]
    collection_name = f"{PREFIX}_VCMenu"
    previous = bpy.data.collections.get(collection_name)
    if previous is not None:
        bpy.data.collections.remove(previous)
    menu_collection = bpy.data.collections.new(collection_name)
    scene.collection.children.link(menu_collection)
    for obj in (menu_item, menu_camera):
        menu_collection.objects.link(obj)
        for source in tuple(obj.users_collection):
            if source != menu_collection:
                source.objects.unlink(obj)

    tree = new_logic_tree(f"{PREFIX}_VCTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    chain = [on_init]

    def then(node):
        tree.links.new(next(s for s in chain[-1].outputs if s.bl_idname == "NodeSocketLogicExecution"),
                       flow_input(node))
        chain.append(node)
        return node

    for user, camera in enumerate(cameras):
        set_camera = then(tree.nodes.new("LogicNativeSetCamera"))
        set_camera.inputs["Camera"].default_value = camera
        set_camera.inputs["User"].default_value = user
    then(tree.nodes.new("LogicNativeSetSplitScreen")).inputs["Enabled"].default_value = True
    for user in (0, 1):
        pointer = then(tree.nodes.new("LogicNativeSetPlayerPointer"))
        pointer.pointer_mode = "VIEW_CENTER"
        pointer.inputs["User"].default_value = user
        pointer.inputs["Reach"].default_value = 6.0
    hide_veil = then(tree.nodes.new("LogicNativeSetCameraVisibility"))
    hide_veil.inputs["Object"].default_value = veil
    hide_veil.inputs["Camera"].default_value = cameras[1]
    hide_veil.inputs["Visible"].default_value = False
    then(tree.nodes.new("LogicNativeSetCursorVisibility")).cursor_mode = "IN_MENUS"

    def after(seconds, node):
        delay = tree.nodes.new("LogicNativeDelay")
        delay.inputs["Delay"].default_value = seconds
        tree.links.new(on_init.outputs["Out"], flow_input(delay))
        tree.links.new(delay.outputs["Done"], flow_input(node))
        return node

    open_menu = after(0.8, tree.nodes.new("LogicNativeOpenObjectMenu"))
    open_menu.inputs["Camera"].default_value = menu_camera
    open_menu.inputs["Collection"].default_value = menu_collection
    after(1.0, tree.nodes.new("LogicNativeRemoveObject")).inputs["Object"].default_value = wall
    after(1.6, tree.nodes.new("LogicNativeCloseObjectMenu"))

    for event, names in (("ENTER", ("vc_enter_0", "vc_enter_1")), ("EXIT", ("vc_exit_0", None))):
        on_event = tree.nodes.new("LogicNativeOnUIEvent")
        on_event.ui_event = event
        on_event.inputs["Action"].default_value = "vc"
        by_user = tree.nodes.new("LogicNativeSwitchFlowInt")
        tree.links.new(on_event.outputs["Out"], flow_input(by_user))
        tree.links.new(on_event.outputs["User"], by_user.inputs["Index"])
        for output, name in zip(("Then 0", "Then 1"), names):
            if name is None:
                continue
            mark = tree.nodes.new("LogicNativeSetGamePropertyBool")
            mark.inputs["Property"].default_value = name
            mark.inputs["Value"].default_value = True
            tree.links.new(by_user.outputs[output], flow_input(mark))

    for name in ("vc_enter_0", "vc_enter_1", "vc_exit_0"):
        set_game_property(driver, name, "BOOL", False)
    add_status_controller(driver, status_path, "ui_view_center", required_frame=8,
                          required_seconds=3.0)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_view_center_0")
    bind_tree(driver, tree.name, True)
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_view_center_0", status_path, prepare=False)


def run_split_screen_leave_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    create_cube(f"{PREFIX}_LeaveScene")
    cameras = [
        create_camera_looking_at(f"{PREFIX}_LeaveCamera{index}", location)
        for index, location in enumerate(
            ((0.0, -8.0, 3.0), (8.0, 0.0, 3.0), (-8.0, 0.0, 3.0), (0.0, 8.0, 3.0)))
    ]
    bpy.context.scene.camera = cameras[0]
    tree = build_split_screen_leave_tree(f"{PREFIX}_SplitLeaveTree", cameras)
    driver = create_cube(f"{PREFIX}_LeaveDriver")
    driver.location = (0.0, 0.0, -3.0)
    add_status_controller(driver, status_path, "split_screen_leave", required_frame=8,
                          required_seconds=3.0)
    for index, camera in enumerate(cameras):
        set_game_property(driver, f"__ln_acceptance_camera_{index}", "STRING", camera.name)
    set_game_property(driver, "p3_shown", "BOOL", True)
    set_game_property(driver, "views", "INT", 0)
    set_game_property(driver, "left_done", "BOOL", False)
    bind_tree(driver, tree.name, True)
    run_game_once(driver, "split_screen_leave_0", status_path)


def run_split_screen_players_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    create_cube(f"{PREFIX}_PlayersScene")
    cameras = [
        create_camera_looking_at(f"{PREFIX}_PlayersCamera{index}", location)
        for index, location in enumerate(((0.0, -8.0, 3.0), (8.0, 0.0, 3.0), (-8.0, 0.0, 3.0)))
    ]
    bpy.context.scene.camera = cameras[0]
    hud = create_cube(f"{PREFIX}_PlayerHud")
    hud.scale = (0.05, 0.05, 0.05)
    idle_hud = create_cube(f"{PREFIX}_IdleHud")
    idle_hud.location = (100.0, 100.0, 100.0)
    speaker = create_cube(f"{PREFIX}_PlayersSpeaker")
    speaker.location = (-6.0, 0.0, 3.0)
    tree = build_split_screen_players_tree(
        f"{PREFIX}_SplitPlayersTree", cameras, hud, idle_hud, speaker)
    driver = create_cube(f"{PREFIX}_PlayersDriver")
    driver.location = (0.0, 0.0, -3.0)
    add_status_controller(driver, status_path, "split_screen_players", required_frame=8,
                          required_seconds=2.0)
    set_game_property(driver, "__ln_acceptance_camera_2", "STRING", cameras[2].name)
    set_game_property(driver, "__ln_acceptance_hud", "STRING", hud.name)
    set_game_property(driver, "__ln_acceptance_idle_hud", "STRING", idle_hud.name)
    set_game_property(driver, "hud_user", "INT", 2)
    for property_name in ("p1_pad", "p2_pad", "p3_pad"):
        set_game_property(driver, property_name, "INT", -9)
    set_game_property(driver, "p1_joined", "BOOL", True)
    set_game_property(driver, "sound_started", "BOOL", False)
    bind_tree(driver, tree.name, True)
    run_game_once(driver, "split_screen_players_0", status_path)


def run_ui_menu_per_player_acceptance(status_path):
    """Per-player menus: players 0 and 1 (side by side) open the same menu collection, each with
    Area Player's View. Each gets a copy of their own, placed in their half at half size; the
    copy's Get Menu Player gives its player and owner, its {gold} text shows the owner's gold, its
    Set Text finds its own tag by name, and its anchored corner (with a badge anchored inside)
    sits at the top right of the player's half. Player 0 then closes theirs: player 1's copy
    stays."""
    reset_scene()
    configure_game_settings(True)
    scene = bpy.context.scene
    cameras = [create_camera_looking_at(f"{PREFIX}_PPCamera{index}", location)
               for index, location in enumerate(((0.0, -8.0, 3.0), (8.0, 0.0, 3.0)))]
    scene.camera = cameras[0]
    overlay_camera = create_camera_looking_at(
        f"{PREFIX}_PPMenuCamera", (0.0, 0.0, 50.0), (0.0, 0.0, 0.0))
    overlay_camera.data.type = "ORTHO"
    overlay_camera.data.ortho_scale = 10.0
    scene.camera = cameras[0]

    panel = create_cube(f"{PREFIX}_PPPanel")
    panel.location = (0.0, 0.0, 40.0)
    panel.scale = (1.0, 1.0, 0.05)
    bpy.ops.object.text_add(location=(0.0, 0.0, 40.2))
    label = bpy.context.active_object
    label.name = f"{PREFIX}_PPLabel"
    label.data.body = "{gold}"
    # Set by the menu's logic, which names it: each copy's logic must find its own copy.
    bpy.ops.object.text_add(location=(0.0, -1.0, 40.2))
    tag = bpy.context.active_object
    tag.name = f"{PREFIX}_PPTag"
    tag.data.body = "-"
    bpy.context.view_layer.update()
    for child in (label, tag):
        child.parent = panel
        child.matrix_parent_inverse = panel.matrix_world.inverted()
    # Anchored to the top right (of each player's view), a badge anchored inside it.
    corner = create_cube(f"{PREFIX}_PPCorner")
    corner.location = (4.5, 2.3, 40.2)
    corner.scale = (0.2, 0.2, 0.05)
    badge = create_cube(f"{PREFIX}_PPBadge")
    badge.location = (4.8, 2.5, 40.3)
    badge.scale = (0.1, 0.1, 0.05)
    for element in (corner, badge):
        element.game.ui.use_element = True
        element.game.ui.anchor = "TOP_RIGHT"
    bpy.context.view_layer.update()
    corner.parent = panel
    corner.matrix_parent_inverse = panel.matrix_world.inverted()
    bpy.context.view_layer.update()
    badge.parent = corner
    badge.matrix_parent_inverse = corner.matrix_world.inverted()
    # A hidden template of the menu that each copy's logic adds once: the added one belongs to
    # that copy (its Get Menu Player says whose).
    popup = create_cube(f"{PREFIX}_PPPopup")
    popup.location = (0.0, -2.0, 40.2)
    popup.scale = (0.2, 0.2, 0.05)
    set_game_property(popup, "popup_user", "INT", -1)
    popup_tree = new_logic_tree(f"{PREFIX}_PPPopupTree")
    popup_update = popup_tree.nodes.new("LogicNativeOnUpdate")
    popup_player = popup_tree.nodes.new("LogicNativeGetMenuPlayer")
    store_popup = popup_tree.nodes.new("LogicNativeSetGamePropertyInt")
    store_popup.inputs["Property"].default_value = "popup_user"
    popup_tree.links.new(popup_update.outputs["Out"], flow_input(store_popup))
    popup_tree.links.new(popup_player.outputs["User"], store_popup.inputs["Value"])
    bind_tree(popup, popup_tree.name, True)
    menu = make_ui_collection(f"{PREFIX}_PPMenu", (panel, label, tag, corner, badge, popup))
    set_game_property(panel, "menu_user", "INT", -1)
    set_game_property(panel, "owner_gold", "INT", -1)

    # The menu's own logic: whose menu it is.
    panel_tree = new_logic_tree(f"{PREFIX}_PPPanelTree")
    on_update = panel_tree.nodes.new("LogicNativeOnUpdate")
    menu_player = panel_tree.nodes.new("LogicNativeGetMenuPlayer")
    store_user = panel_tree.nodes.new("LogicNativeSetGamePropertyInt")
    store_user.inputs["Property"].default_value = "menu_user"
    panel_tree.links.new(on_update.outputs["Out"], flow_input(store_user))
    panel_tree.links.new(menu_player.outputs["User"], store_user.inputs["Value"])
    gold = panel_tree.nodes.new("LogicNativeGetGamePropertyInt")
    gold.inputs["Property"].default_value = "gold"
    panel_tree.links.new(menu_player.outputs["Owner"], gold.inputs["Object"])
    store_gold = panel_tree.nodes.new("LogicNativeSetGamePropertyInt")
    store_gold.inputs["Property"].default_value = "owner_gold"
    panel_tree.links.new(on_update.outputs["Out"], flow_input(store_gold))
    panel_tree.links.new(gold.outputs[0], store_gold.inputs["Value"])
    tag_text = panel_tree.nodes.new("LogicNativeFormattedString")
    tag_text.inputs["Format String"].default_value = "player {}"
    panel_tree.links.new(menu_player.outputs["User"], tag_text.inputs["A"])
    set_tag = panel_tree.nodes.new("LogicNativeSetText")
    set_tag.inputs["Object"].default_value = tag
    panel_tree.links.new(on_update.outputs["Out"], flow_input(set_tag))
    panel_tree.links.new(tag_text.outputs["String"], set_tag.inputs["Text"])
    add_popup = panel_tree.nodes.new("LogicNativeAddObject")
    add_popup.inputs["Object to Add"].default_value = popup
    panel_tree.links.new(panel_tree.nodes.new("LogicNativeOnInit").outputs["Out"],
                         flow_input(add_popup))
    bind_tree(panel, panel_tree.name, True)

    # Each player opens it from their own object (the owner); one tree for both.
    opener = new_logic_tree(f"{PREFIX}_PPOpenerTree")
    on_init = opener.nodes.new("LogicNativeOnInit")
    delay = opener.nodes.new("LogicNativeDelay")
    delay.inputs["Delay"].default_value = 0.1
    read_user = opener.nodes.new("LogicNativeGetGamePropertyInt")
    read_user.inputs["Property"].default_value = "user"
    open_menu = opener.nodes.new("LogicNativeOpenObjectMenu")
    open_menu.inputs["Camera"].default_value = overlay_camera
    open_menu.inputs["Collection"].default_value = menu
    open_menu.menu_routing = "PASS_THROUGH"
    open_menu.menu_area = "PLAYER_VIEW"
    opener.links.new(read_user.outputs[0], open_menu.inputs["User"])
    opener.links.new(on_init.outputs["Out"], flow_input(delay))
    opener.links.new(delay.outputs[0], flow_input(open_menu))
    for index in range(2):
        player = create_cube(f"{PREFIX}_PPPlayer{index}")
        player.location = (4.0 * index, 0.0, 0.0)
        set_game_property(player, "user", "INT", index)
        set_game_property(player, "gold", "INT", 10 * (index + 1))
        bind_tree(player, opener.name, True)

    # The driver splits the window, then closes player 0's menu.
    driver = create_cube(f"{PREFIX}_PPDriver")
    driver.location = (0.0, 0.0, -3.0)
    tree = new_logic_tree(f"{PREFIX}_PPDriverTree")
    chain = []
    for user, camera in enumerate(cameras):
        set_camera = tree.nodes.new("LogicNativeSetCamera")
        set_camera.inputs["Camera"].default_value = camera
        set_camera.inputs["User"].default_value = user
        chain.append(set_camera)
    split = tree.nodes.new("LogicNativeSetSplitScreen")
    split.split_two_players = "SIDE_BY_SIDE"
    chain.append(split)
    wait = tree.nodes.new("LogicNativeDelay")
    wait.inputs["Delay"].default_value = 0.9
    chain.append(wait)
    close = tree.nodes.new("LogicNativeCloseObjectMenu")
    close.menu_close_operation = "TOP"
    close.inputs["User"].default_value = 0
    chain.append(close)
    chain_flows(tree, tree.nodes.new("LogicNativeOnInit").outputs["Out"], chain)
    add_status_controller(driver, status_path, "ui_menu_per_player", required_frame=8,
                          required_seconds=2.5)
    set_game_property(driver, "__ln_acceptance_panel", "STRING", panel.name)
    set_game_property(driver, "__ln_acceptance_menu_camera", "STRING", overlay_camera.name)
    set_game_property(driver, "__ln_acceptance_run", "STRING", "ui_menu_per_player_0")
    bind_tree(driver, tree.name, True)
    menu.hide_viewport = True
    set_active_object(driver)
    bpy.context.view_layer.update()
    run_game_once(driver, "ui_menu_per_player_0", status_path, prepare=False)


def run_render_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    bpy.context.scene.game_settings.vsync = "OFF"
    tree = build_render_state_tree(f"{PREFIX}_RenderStateTree")
    target = create_cube(f"{PREFIX}_RenderState")
    add_status_controller(target, status_path, "render_state", required_frame=4)
    set_game_property(target, "__ln_acceptance_expected_vsync_mode", "INT", 1)
    set_game_property(target, "__ln_acceptance_expected_vsync_interval", "INT", 0)
    set_game_property(target, "fullscreen_state", "BOOL", False)
    set_game_property(target, "resolution_width", "INT", 0)
    set_game_property(target, "resolution_height", "INT", 0)
    set_game_property(target, "vsync_mode", "INT", 0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "render_state_0", status_path)


def run_light_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    color = (0.2, 0.6, 0.8, 1.0)
    power = 3.5
    use_shadow = False
    source_light = create_light(f"{PREFIX}_SourceLight", (-2.0, 0.0, 3.0), (1.0, 0.1, 0.1), 0.25, True)
    mirror_light = create_light(f"{PREFIX}_MirrorLight", (2.0, 0.0, 3.0), (0.1, 0.2, 0.3), 0.5, True)
    tree = build_light_state_tree(
        f"{PREFIX}_LightStateTree", source_light, mirror_light, color, power, use_shadow)
    driver = create_cube(f"{PREFIX}_LightState")
    add_status_controller(driver, status_path, "light_state", required_frame=4, tolerance=0.05)
    set_expected_color(driver, color)
    set_game_property(driver, "__ln_acceptance_expected_power", "FLOAT", power)
    set_game_property(driver, "__ln_acceptance_expected_shadow", "BOOL", use_shadow)
    set_game_property(driver, "__ln_acceptance_source_light", "STRING", source_light.name)
    set_game_property(driver, "__ln_acceptance_mirror_light", "STRING", mirror_light.name)
    bind_tree(driver, tree.name, True)
    run_game_once(driver, "light_state_0", status_path)


def run_light_unique_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    source_initial_color = (0.9, 0.2, 0.1)
    source_initial_power = 0.75
    source_initial_shadow = True
    target_color = (0.2, 0.8, 0.4, 1.0)
    target_power = 3.25
    target_shadow = False
    source_light = create_light(
        f"{PREFIX}_UniqueSourceLight",
        (-2.0, 0.0, 3.0),
        source_initial_color,
        source_initial_power,
        source_initial_shadow,
    )
    mirror_light = create_light(
        f"{PREFIX}_UniqueMirrorLight",
        (2.0, 0.0, 3.0),
        (0.1, 0.1, 0.1),
        0.1,
        False,
    )
    mirror_light.data = source_light.data

    tree = build_light_unique_tree(
        f"{PREFIX}_LightUniqueTree", source_light, target_color, target_power, target_shadow)
    driver = create_cube(f"{PREFIX}_LightUnique")
    add_status_controller(driver, status_path, "light_unique", required_frame=4, tolerance=0.05)
    set_expected_color(driver, target_color)
    set_game_property(driver, "__ln_acceptance_expected_power", "FLOAT", target_power)
    set_game_property(driver, "__ln_acceptance_expected_shadow", "BOOL", target_shadow)
    set_game_property(driver, "__ln_acceptance_expected_mirror_r", "FLOAT", source_initial_color[0])
    set_game_property(driver, "__ln_acceptance_expected_mirror_g", "FLOAT", source_initial_color[1])
    set_game_property(driver, "__ln_acceptance_expected_mirror_b", "FLOAT", source_initial_color[2])
    set_game_property(driver, "__ln_acceptance_expected_mirror_power", "FLOAT", source_initial_power)
    set_game_property(driver, "__ln_acceptance_expected_mirror_shadow", "BOOL", source_initial_shadow)
    set_game_property(driver, "__ln_acceptance_source_light", "STRING", source_light.name)
    set_game_property(driver, "__ln_acceptance_mirror_light", "STRING", mirror_light.name)
    bind_tree(driver, tree.name, True)
    run_game_once(driver, "light_unique_0", status_path)


def run_physics_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    collision_group = 5
    driver = create_cube(f"{PREFIX}_PhysicsStateDriver")
    target = create_cube(f"{PREFIX}_PhysicsStateTarget", rigid_body=True)
    tree = build_physics_state_tree(
        f"{PREFIX}_PhysicsStateTree", target, collision_group)
    add_status_controller(driver, status_path, "physics_state", required_frame=4)
    set_game_property(driver, "__ln_acceptance_expected_collision_group", "INT", collision_group)
    set_game_property(driver, "__ln_acceptance_target", "STRING", target.name)
    set_game_property(driver, "collision_group", "INT", 0)
    bind_tree(driver, tree.name, True)
    run_game_once(driver, "physics_state_0", status_path)


def run_character_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    create_ground(f"{PREFIX}_CharacterGround")
    gravity = (0.0, 0.0, -15.0)
    walk_direction = (1.0, 0.0, 0.0)
    velocity = (0.0, 3.0, 0.0)
    max_jumps = 2
    jump_speed = 6.5
    character = create_character(f"{PREFIX}_Character")
    tree = build_character_state_tree(
        f"{PREFIX}_CharacterTree",
        character,
        gravity,
        walk_direction,
        velocity,
        0.1,
        max_jumps,
        jump_speed,
    )
    add_status_controller(character, status_path, "character_state", required_frame=8, tolerance=0.05)
    set_expected_named_vector(character, "gravity", gravity)
    set_expected_named_vector(character, "walk", walk_direction)
    set_game_property(character, "__ln_acceptance_expected_max_jumps", "INT", max_jumps)
    set_game_property(character, "__ln_acceptance_min_jump_count", "INT", 1)
    set_game_property(character, "__ln_acceptance_min_x", "FLOAT", 0.05)
    set_game_property(character, "__ln_acceptance_min_y", "FLOAT", 0.05)
    set_game_property(character, "__ln_acceptance_min_z", "FLOAT", 0.05)
    set_game_property(character, "character_gravity_x", "FLOAT", 0.0)
    set_game_property(character, "character_gravity_y", "FLOAT", 0.0)
    set_game_property(character, "character_gravity_z", "FLOAT", 0.0)
    set_game_property(character, "character_walk_x", "FLOAT", 0.0)
    set_game_property(character, "character_walk_y", "FLOAT", 0.0)
    set_game_property(character, "character_walk_z", "FLOAT", 0.0)
    set_game_property(character, "character_max_jumps", "INT", 0)
    set_game_property(character, "character_jump_count", "INT", 0)
    set_game_property(character, "character_on_ground", "BOOL", True)
    bind_tree(character, tree.name, True)
    run_game_once(character, "character_state_0", status_path)


def run_object_lifecycle_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    template = create_cube(f"{PREFIX}_Template")
    active_template = create_cube(f"{PREFIX}_ActiveTemplate")
    parent = create_cube(f"{PREFIX}_Parent")
    target = create_cube(f"{PREFIX}_Lifecycle")
    child = create_cube(f"{PREFIX}_Child")
    victim = create_cube(f"{PREFIX}_Victim")
    replica_anchor = create_cube(f"{PREFIX}_ReplicaAnchor")
    full_copy_anchor = create_cube(f"{PREFIX}_FullCopyAnchor")
    repeat_anchor = create_cube(f"{PREFIX}_RepeatAnchor")
    result_output_anchor = create_cube(f"{PREFIX}_ResultOutputAnchor")
    ordered_copy_anchor = create_cube(f"{PREFIX}_OrderedCopyAnchor")
    life_present_anchor = create_cube(f"{PREFIX}_LifePresentAnchor")
    full_life_present_anchor = create_cube(f"{PREFIX}_FullLifePresentAnchor")
    life_expired_anchor = create_cube(f"{PREFIX}_LifeExpiredAnchor")
    active_reject_anchor = create_cube(f"{PREFIX}_ActiveRejectAnchor")
    lifecycle_driver = create_cube(f"{PREFIX}_LifecycleDriver")
    repeat_driver = create_cube(f"{PREFIX}_RepeatDriver")
    result_output_driver = create_cube(f"{PREFIX}_ResultOutputDriver")
    ordered_copy_driver = create_cube(f"{PREFIX}_OrderedCopyDriver")
    life_present_driver = create_cube(f"{PREFIX}_LifePresentDriver")
    full_life_present_driver = create_cube(f"{PREFIX}_FullLifePresentDriver")
    life_expired_driver = create_cube(f"{PREFIX}_LifeExpiredDriver")
    active_reject_driver = create_cube(f"{PREFIX}_ActiveRejectDriver")
    set_parent_driver = create_cube(f"{PREFIX}_SetParentDriver")
    remove_driver = create_cube(f"{PREFIX}_RemoveDriver")
    probe = create_cube(f"{PREFIX}_LifecycleProbe")
    replica_anchor.location = (1.5, -1.0, 0.5)
    full_copy_anchor.location = (-2.0, 1.0, 1.5)
    repeat_anchor.location = (4.5, -2.0, 1.25)
    result_output_anchor.location = (5.5, 2.0, 1.25)
    ordered_copy_anchor.location = (-6.0, -3.0, 0.25)
    life_present_anchor.location = (-7.5, 2.0, 1.25)
    full_life_present_anchor.location = (-8.5, 1.0, 1.25)
    life_expired_anchor.location = (-9.5, 0.0, 1.25)
    active_reject_anchor.location = (3.5, 2.5, 1.25)
    probe.location = (6.0, 0.0, 0.0)
    template.hide_viewport = True
    bpy.context.view_layer.update()
    target.parent = parent
    lifecycle_tree = build_object_lifecycle_tree(
        f"{PREFIX}_LifecycleTree", template, replica_anchor, full_copy_anchor, target)
    active_reject_tree = build_add_object_single_tree(
        f"{PREFIX}_ActiveRejectAddObjectTree", active_template, active_reject_anchor, False)
    repeat_tree = build_add_object_update_tree(
        f"{PREFIX}_RepeatAddObjectTree", template, repeat_anchor, False)
    result_output_tree = build_add_object_result_move_tree(
        f"{PREFIX}_ResultOutputAddObjectTree",
        template,
        replica_anchor,
        tuple(float(component) for component in result_output_anchor.location),
    )
    get_id_tree = build_add_object_get_id_tree(
        f"{PREFIX}_AddObjectGetIDTree", template, replica_anchor)
    ordered_copy_position = (-4.5, 3.5, 1.75)
    ordered_copy_tree = build_add_object_after_move_tree(
        f"{PREFIX}_OrderedCopyAddObjectTree",
        template,
        ordered_copy_anchor,
        ordered_copy_position,
    )
    life_present_tree = build_add_object_single_tree(
        f"{PREFIX}_LifePresentAddObjectTree", template, life_present_anchor, False, 1.0)
    full_life_present_tree = build_add_object_single_tree(
        f"{PREFIX}_FullLifePresentAddObjectTree", template, full_life_present_anchor, True, 1.0)
    life_expired_tree = build_add_object_single_tree(
        f"{PREFIX}_LifeExpiredAddObjectTree", template, life_expired_anchor, False, 0.05)
    spawned_logic_tree = build_spawned_replica_init_tree(f"{PREFIX}_SpawnedReplicaInitTree")
    set_parent_tree = build_set_parent_tree(f"{PREFIX}_SetParentTree", child, parent)
    remove_tree = build_remove_object_tree(f"{PREFIX}_RemoveObjectTree", victim)

    add_status_controller(probe, status_path, "object_lifecycle", required_frame=8)
    set_game_property(probe, "__ln_acceptance_template", "STRING", template.name)
    set_game_property(probe, "__ln_acceptance_active_template", "STRING", active_template.name)
    set_game_property(probe, "__ln_acceptance_victim", "STRING", victim.name)
    set_game_property(probe, "__ln_acceptance_child", "STRING", child.name)
    set_game_property(probe, "__ln_acceptance_parent", "STRING", parent.name)
    set_game_property(probe, "__ln_acceptance_unparent_target", "STRING", target.name)
    set_expected_named_vector(
        probe, "replica_spawn", tuple(float(component) for component in replica_anchor.location))
    set_expected_named_vector(
        probe,
        "full_copy_spawn",
        tuple(float(component) for component in full_copy_anchor.location),
    )
    set_expected_named_vector(
        probe, "repeat_spawn", tuple(float(component) for component in repeat_anchor.location))
    set_expected_named_vector(
        probe,
        "result_output_spawn",
        tuple(float(component) for component in result_output_anchor.location),
    )
    set_expected_named_vector(probe, "ordered_copy_spawn", ordered_copy_position)
    set_expected_named_vector(
        probe, "life_present_spawn", tuple(float(component) for component in life_present_anchor.location))
    set_expected_named_vector(
        probe,
        "full_life_present_spawn",
        tuple(float(component) for component in full_life_present_anchor.location),
    )
    set_expected_named_vector(
        probe, "life_expired_spawn", tuple(float(component) for component in life_expired_anchor.location))
    set_expected_named_vector(
        probe,
        "active_reject_spawn",
        tuple(float(component) for component in active_reject_anchor.location),
    )
    bind_tree(lifecycle_driver, lifecycle_tree.name, True)
    bind_tree(template, spawned_logic_tree.name, True)
    bind_tree(repeat_driver, repeat_tree.name, True)
    bind_tree(result_output_driver, result_output_tree.name, True)
    bind_tree(probe, get_id_tree.name, True)
    bind_tree(ordered_copy_driver, ordered_copy_tree.name, True)
    bind_tree(life_present_driver, life_present_tree.name, True)
    bind_tree(full_life_present_driver, full_life_present_tree.name, True)
    bind_tree(life_expired_driver, life_expired_tree.name, True)
    bind_tree(active_reject_driver, active_reject_tree.name, True)
    bind_tree(set_parent_driver, set_parent_tree.name, True)
    bind_tree(remove_driver, remove_tree.name, True)
    run_game_once(probe, "object_lifecycle_0", status_path)


def run_compound_parent_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    parent = create_cube(f"{PREFIX}_CompoundParent")
    parent.game.physics_type = "STATIC"
    parent.game.use_collision_bounds = True
    parent.game.collision_bounds_type = "BOX"
    parent.game.use_collision_compound = True
    child = create_cube(f"{PREFIX}_CompoundChild", rigid_body=True)
    child.location = (2.0, 0.0, 0.0)
    child.game.gravity_factor = 0.0
    driver = create_cube(f"{PREFIX}_CompoundParentDriver")

    tree = build_set_parent_tree(
        f"{PREFIX}_CompoundParentTree", child, parent, compound=True)
    add_status_controller(driver, status_path, "compound_parent", required_frame=5)
    set_game_property(driver, "__ln_acceptance_child", "STRING", child.name)
    set_game_property(driver, "__ln_acceptance_parent", "STRING", parent.name)
    bind_tree(driver, tree.name, True)
    run_game_once(driver, "compound_parent_0", status_path)


def run_object_query_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    parent = create_cube(f"{PREFIX}_QueryParent")
    child = create_cube(f"{PREFIX}_QueryChild")
    probe = create_cube(f"{PREFIX}_QueryProbe")
    probe.location = (4.0, 0.0, 0.0)
    child.parent = parent
    tree = build_object_query_tree(f"{PREFIX}_ObjectQueryTree", child, parent)

    add_status_controller(probe, status_path, "object_queries", required_frame=2)
    for property_name in ("has_parent", "has_child", "missing_child_is_none"):
        set_game_property(probe, property_name, "BOOL", False)

    bind_tree(probe, tree.name, True)
    run_game_once(probe, "object_queries_0", status_path)


def run_average_center_position_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    sample_a = create_cube(f"{PREFIX}_AverageSampleA")
    sample_b = create_cube(f"{PREFIX}_AverageSampleB")
    sample_a.location = (1.0, 2.0, 3.0)
    sample_b.location = (3.0, 4.0, 5.0)
    collection = bpy.data.collections.new(f"{PREFIX}_AverageSamples")
    bpy.context.scene.collection.children.link(collection)
    collection.objects.link(sample_a)
    collection.objects.link(sample_b)
    empty_collection = bpy.data.collections.new(f"{PREFIX}_AverageEmptySamples")
    bpy.context.scene.collection.children.link(empty_collection)

    probe = create_cube(f"{PREFIX}_AverageProbe")
    tree = build_average_center_position_tree(
        f"{PREFIX}_AverageCenterPositionTree", collection, empty_collection)
    add_status_controller(probe, status_path, "average_center", required_frame=2)
    expected = {
        "vector_center": ((4.0, 12.0, 18.0), 2, True),
        "object_center": ((2.0, 3.0, 4.0), 2, True),
        "collection_center": ((2.0, 3.0, 4.0), 2, True),
        "empty_vector_center": ((0.0, 0.0, 0.0), 0, False),
        "empty_object_center": ((0.0, 0.0, 0.0), 0, False),
        "empty_collection_center": ((0.0, 0.0, 0.0), 0, False),
    }
    for prefix, (center, count, valid) in expected.items():
        for axis in "xyz":
            set_game_property(probe, f"{prefix}_{axis}", "FLOAT", 0.0 if valid else 123.0)
        set_game_property(probe, f"{prefix}_count", "INT", 0 if valid else -1)
        set_game_property(probe, f"{prefix}_valid", "BOOL", not valid)
        set_expected_named_vector(probe, prefix, center)
        set_game_property(probe, f"{prefix}_expected_count", "INT", count)
        set_game_property(probe, f"{prefix}_expected_valid", "BOOL", valid)

    bind_tree(probe, tree.name, True)
    run_game_once(probe, "average_center_0", status_path)


def run_tier_a_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target_name = f"{PREFIX}_TierATarget"
    create_cube(target_name)
    probe = create_cube(f"{PREFIX}_TierAProbe")
    mover = create_cube(f"{PREFIX}_TierAMover")
    probe.location = (4.0, 0.0, 0.0)
    tree = build_tier_a_tree(f"{PREFIX}_TierATree", target_name, mover)
    add_status_controller(probe, status_path, "tier_a", required_frame=8, tolerance=0.05)
    set_game_property(probe, "tier_a_list_len", "INT", 0)
    set_game_property(probe, "tier_a_dict_len", "INT", 0)
    set_game_property(probe, "tier_a_global", "FLOAT", 0.0)
    set_game_property(probe, "tier_a_found_by_name", "BOOL", False)
    set_game_property(probe, "tier_a_owner_ok", "BOOL", False)
    set_game_property(probe, "__ln_acceptance_min_x", "FLOAT", 0.2)
    set_game_property(probe, "__ln_acceptance_mover", "STRING", mover.name)
    bind_tree(probe, tree.name, True)
    run_game_once(probe, "tier_a_0", status_path)


def run_raycast_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    bpy.context.scene.game_settings.use_logic_nodes_parallel = True
    target = create_cube(f"{PREFIX}_RaycastTarget")
    single_ignore = create_cube(f"{PREFIX}_RaycastSingleIgnore")
    list_ignore = create_cube(f"{PREFIX}_RaycastListIgnore")
    for obj in (target, single_ignore, list_ignore):
        obj.game.physics_type = "STATIC"
        obj.game.use_collision_bounds = True
        obj.game.collision_bounds_type = "BOX"
    single_ignore.location = (0.0, 0.0, 3.0)
    single_ignore.scale = (0.25, 0.25, 0.25)
    list_ignore.location = (0.0, 0.0, 2.0)
    list_ignore.scale = (0.25, 0.25, 0.25)
    ignore_collection = bpy.data.collections.new(f"{PREFIX}_RaycastIgnoreCollection")
    bpy.context.scene.collection.children.link(ignore_collection)
    # The legacy input and list deliberately overlap. The list-only object proves list filtering;
    # the duplicate proves the union is harmless and deterministic.
    ignore_collection.objects.link(single_ignore)
    ignore_collection.objects.link(list_ignore)
    probe = create_cube(f"{PREFIX}_RaycastProbe")
    probe.location = (4.0, 0.0, 0.0)
    probe.game.physics_type = "NO_COLLISION"
    tree = build_raycast_tree(
        f"{PREFIX}_RaycastTree", single_ignore, ignore_collection)

    add_status_controller(probe, status_path, "raycast", required_frame=2, tolerance=0.02)
    set_expected_named_vector(probe, "hit_point", (0.0, 0.0, 1.0))
    set_expected_named_vector(probe, "hit_normal", (0.0, 0.0, 1.0))
    set_expected_named_vector(probe, "hit_direction", (0.0, 0.0, -1.0))
    set_expected_named_vector(probe, "ray_end_point", (0.0, 0.0, -5.0))

    set_game_property(probe, "ray_hit", "BOOL", False)
    set_game_property(probe, "ray_object", "BOOL", False)
    set_game_property(probe, "ray_all_hit", "BOOL", False)
    set_game_property(probe, "ray_all_count", "INT", 0)
    set_game_property(probe, "shape_hit", "BOOL", False)
    set_game_property(probe, "shape_all_hit", "BOOL", False)
    set_game_property(probe, "shape_all_count", "INT", 0)
    for property_name in (
        "ray_point_x",
        "ray_point_y",
        "ray_point_z",
        "ray_normal_x",
        "ray_normal_y",
        "ray_normal_z",
        "ray_direction_x",
        "ray_direction_y",
        "ray_direction_z",
        "ray_end_point_x",
        "ray_end_point_y",
        "ray_end_point_z",
    ):
        set_game_property(probe, property_name, "FLOAT", 0.0)

    bind_tree(probe, tree.name, True)
    run_game_once(probe, "raycast_0", status_path)


def run_raycast_from_mesh_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    bpy.context.scene.game_settings.use_logic_nodes_parallel = True

    vertices = []
    faces = []
    for row in range(4):
        for column in range(4):
            y = -0.9 + column * 0.55
            z = -0.9 + row * 0.55
            base = len(vertices)
            vertices.extend(((0.0, y, z), (0.0, y + 0.3, z), (0.0, y, z + 0.3)))
            # This winding produces an exact +X geometric normal for every triangle.
            faces.append((base, base + 1, base + 2))
    mesh = bpy.data.meshes.new(f"{PREFIX}_RaycastFromMeshSourceMesh")
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    source = bpy.data.objects.new(f"{PREFIX}_RaycastFromMeshSource", mesh)
    bpy.context.scene.collection.objects.link(source)
    source.game.physics_type = "NO_COLLISION"

    def create_wall(name, x, half_x, tagged):
        wall = create_cube(name)
        wall.location = (x, 0.0, 0.0)
        wall.scale = (half_x, 2.0, 2.0)
        wall.game.physics_type = "STATIC"
        wall.game.use_collision_bounds = True
        wall.game.collision_bounds_type = "BOX"
        if tagged:
            set_game_property(wall, "mesh_target", "BOOL", True)
        return wall

    blocker = create_wall(f"{PREFIX}_RaycastFromMeshBlocker", 1.0, 0.1, False)
    right_target = create_wall(f"{PREFIX}_RaycastFromMeshRight", 2.5, 0.25, True)
    # Only the first of the sixteen +X face rays intersects this target. This mixed hit/miss batch
    # verifies that Points and the other collision-detail lists exclude miss endpoints.
    right_target.location.y = -0.8
    right_target.location.z = -0.8
    right_target.scale.y = 0.2
    right_target.scale.z = 0.2
    left_target = create_wall(f"{PREFIX}_RaycastFromMeshLeft", -2.5, 0.25, True)
    # This tagged object is closer than right_target and intersects the same generated ray. The
    # collection-fed ignore list must skip it so the observed hit remains right_target at X=2.25.
    ignored_target = create_wall(f"{PREFIX}_RaycastFromMeshIgnored", 1.5, 0.1, True)
    ignored_target.location.y = -0.8
    ignored_target.location.z = -0.8
    ignored_target.scale.y = 0.2
    ignored_target.scale.z = 0.2
    ignore_collection = bpy.data.collections.new(f"{PREFIX}_RaycastFromMeshIgnoreCollection")
    bpy.context.scene.collection.children.link(ignore_collection)
    ignore_collection.objects.link(ignored_target)
    # Duplicating the legacy single-ignore object in the list verifies union and deduplication.
    ignore_collection.objects.link(left_target)
    tree = build_raycast_from_mesh_tree(
        f"{PREFIX}_RaycastFromMeshTree",
        source,
        right_target,
        left_target,
        ignore_collection,
    )

    add_status_controller(
        source, status_path, "raycast_from_mesh", required_frame=3, tolerance=0.001)
    for property_name in (
        "mesh_face_done",
        "mesh_face_any",
        "mesh_face_all",
        "mesh_face_first_hit",
        "mesh_face_first_blocked",
        "mesh_face_first_object",
        "mesh_face_average_valid",
        "mesh_vertex_any",
        "mesh_vertex_all",
        "mesh_blocked_any",
        "mesh_blocked_all",
        "mesh_blocked_first",
        "mesh_blocked_average_valid",
    ):
        set_game_property(source, property_name, "BOOL", False)
    for property_name in (
        "mesh_face_hit_count",
        "mesh_face_ray_count",
        "mesh_vertex_hit_count",
        "mesh_vertex_ray_count",
        "mesh_blocked_hit_count",
        "mesh_blocked_ray_count",
        "mesh_face_average_count",
        "mesh_blocked_average_count",
    ):
        set_game_property(source, property_name, "INT", -1)
    for suffix in (
        "hits",
        "blocked",
        "hit_objects",
        "points",
        "normals",
        "distances",
        "fractions",
        "face_indices",
        "has_uvs",
        "uvs",
        "ray_origins",
        "ray_directions",
        "end_points",
    ):
        set_game_property(source, f"mesh_face_len_{suffix}", "INT", -1)
        set_game_property(source, f"mesh_blocked_len_{suffix}", "INT", -1)
    for component in ("x", "y", "z"):
        set_game_property(source, f"mesh_face_average_{component}", "FLOAT", 0.0)
        set_game_property(source, f"mesh_blocked_average_{component}", "FLOAT", 0.0)
    bind_tree(source, tree.name, True)

    # Supply a second independent tree so the parallel-tree scheduler is genuinely enabled while
    # the query tree is correctly retained on its command-ordered main lane.
    auxiliary = create_cube(f"{PREFIX}_RaycastFromMeshParallelAux")
    auxiliary.location = (0.0, 8.0, 0.0)
    set_game_property(auxiliary, "parallel_aux_tick", "BOOL", False)
    auxiliary_tree = new_logic_tree(f"{PREFIX}_RaycastFromMeshParallelAuxTree")
    auxiliary_update = auxiliary_tree.nodes.new("LogicNativeOnUpdate")
    auxiliary_set = auxiliary_tree.nodes.new("LogicNativeSetGamePropertyBool")
    auxiliary_set.inputs["Property"].default_value = "parallel_aux_tick"
    auxiliary_set.inputs["Value"].default_value = True
    auxiliary_tree.links.new(auxiliary_update.outputs["Out"], auxiliary_set.inputs["Flow"])
    bind_tree(auxiliary, auxiliary_tree.name, True)

    run_game_once(source, "raycast_from_mesh_0", status_path)


def run_phase6_events_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6Events")
    tree = build_send_receive_event_tree(f"{PREFIX}_Phase6EventsTree")
    # Send Event is command-buffered and visible to Receive Event on the next Logic Nodes tick;
    # the status controller runs before that tick in this harness, so observe one frame later.
    add_status_controller(target, status_path, "phase6_events", required_frame=3)
    set_game_property(target, "event_ok", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_events_0", status_path)


def run_phase6_move_toward_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6MoveToward")
    tree = build_move_toward_tree(f"{PREFIX}_Phase6MoveTowardTree", (2.0, 0.0, 0.0), target)
    add_status_controller(target, status_path, "phase6_move_toward", required_frame=8, tolerance=0.05)
    set_game_property(target, "__ln_acceptance_min_x", "FLOAT", 0.2)
    set_game_property(target, "__ln_acceptance_max_x", "FLOAT", 1.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_move_toward_0", status_path)


def run_navigate_visualize_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    expected = (1.0, 0.0, 0.0)
    target = create_cube(f"{PREFIX}_NavigateVisualize")
    tree = build_navigate_visualize_tree(f"{PREFIX}_NavigateVisualizeTree", expected, target)
    add_status_controller(target, status_path, "position", required_frame=2, tolerance=0.001)
    set_expected_vector(target, expected)
    bind_tree(target, tree.name, True)
    run_game_once(target, "navigate_visualize_0", status_path)


def run_phase6_copy_property_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    source = create_cube(f"{PREFIX}_Phase6CopySource")
    target = create_cube(f"{PREFIX}_Phase6CopyTarget")
    probe = create_cube(f"{PREFIX}_Phase6CopyProbe")
    probe.location = (4.0, 0.0, 0.0)
    tree = build_copy_property_tree(
        f"{PREFIX}_Phase6CopyPropertyTree",
        source,
        target,
        "score",
        11.0,
    )
    add_status_controller(probe, status_path, "phase6_copy_property", required_frame=3)
    set_game_property(probe, "__ln_acceptance_expected_score", "FLOAT", 11.0)
    set_game_property(probe, "__ln_acceptance_target", "STRING", target.name)
    set_game_property(target, "score", "FLOAT", 0.0)
    bind_tree(source, tree.name, True)
    run_game_once(probe, "phase6_copy_property_0", status_path)


def run_phase6_get_object_attribute_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6GetAttr")
    expected = (3.0, 1.0, 2.0)
    tree = build_get_object_attribute_tree(f"{PREFIX}_Phase6GetAttrTree", expected)
    add_status_controller(target, status_path, "phase6_get_object_attribute", required_frame=3)
    set_expected_vector(target, expected)
    set_game_property(target, "read_x", "FLOAT", 0.0)
    set_game_property(target, "read_y", "FLOAT", 0.0)
    set_game_property(target, "read_z", "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_get_object_attribute_0", status_path)


def run_phase6_rotate_to_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6RotateTo")
    tree = build_rotate_toward_tree(f"{PREFIX}_Phase6RotateToTree", (1.0, 0.0, 0.0), target)
    add_status_controller(target, status_path, "phase6_rotate_to", required_frame=24)
    set_game_property(target, "__ln_acceptance_min_dot", "FLOAT", 0.85)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_rotate_to_0", status_path)


def run_phase6_collision_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    create_ground(f"{PREFIX}_Phase6CollisionGround")
    target = create_cube(f"{PREFIX}_Phase6CollisionTarget", rigid_body=True)
    target.location = (0.0, 0.0, 1.0)
    blocker = create_cube(f"{PREFIX}_Phase6CollisionBlocker", rigid_body=True)
    blocker.location = (0.0, 0.0, 1.0)
    set_game_property(blocker, "collider_marker", "BOOL", True)
    tree = build_collision_tree(f"{PREFIX}_Phase6CollisionTree")
    add_status_controller(target, status_path, "phase6_collision", required_frame=12)
    set_game_property(target, "__ln_acceptance_expected_collision_object", "STRING", blocker.name)
    set_game_property(target, "collided", "BOOL", False)
    set_game_property(target, "collided_object_name", "STRING", "pending")
    for property_name in (
            "collision_point_x",
            "collision_point_y",
            "collision_point_z",
            "collision_normal_x",
            "collision_normal_y",
            "collision_normal_z"):
        set_game_property(target, property_name, "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_collision_0", status_path)


def run_phase6_objects_colliding_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    create_ground(f"{PREFIX}_Phase6CollidingGround")
    object_a = create_cube(f"{PREFIX}_Phase6CollidingA", rigid_body=True)
    object_a.location = (0.0, 0.0, 1.0)
    object_b = create_cube(f"{PREFIX}_Phase6CollidingB", rigid_body=True)
    object_b.location = (0.0, 0.0, 1.0)
    tree = build_objects_colliding_tree(
        f"{PREFIX}_Phase6ObjectsCollidingTree",
        object_a,
        object_b,
    )
    add_status_controller(object_a, status_path, "phase6_objects_colliding", required_frame=8)
    set_game_property(object_a, "colliding", "BOOL", False)
    bind_tree(object_a, tree.name, True)
    run_game_once(object_a, "phase6_objects_colliding_0", status_path)


def run_phase6_animation_status_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    game_settings = bpy.context.scene.game_settings
    game_settings.use_fixed_fps_cap = True
    game_settings.physics_tick_rate = 120
    game_settings.fixed_render_cap_rate = 20
    target = create_cube(f"{PREFIX}_Phase6AnimationStatus")
    action = create_test_action(f"{PREFIX}_Phase6Action")
    tree = build_animation_status_tree(f"{PREFIX}_Phase6AnimationStatusTree", action)
    add_status_controller(
        target, status_path, "phase6_animation_status", required_frame=1, required_seconds=2.0)
    set_game_property(target, "action_playing", "BOOL", False)
    for property_name in (
            "action_lifecycle_started",
            "action_lifecycle_completed",
            "action_lifecycle_interrupted",
            "action_lifecycle_failed",
            "action_interruption_observed",
            "action_failure_observed"):
        set_game_property(target, property_name, "BOOL", False)
    bind_tree(target, tree.name, True)
    # The embedded game restores Blender data on exit, so continue with live references.
    target = run_game_once(target, "phase6_animation_status_logic_faster", status_path)

    # Repeat with the opposite cadence. Lifecycle events belong to the graph's execution domain,
    # not to render frequency, and must survive the frames between fixed logic steps.
    game_settings = bpy.context.scene.game_settings
    game_settings.physics_tick_rate = 30
    game_settings.fixed_render_cap_rate = 240
    run_game_once(target, "phase6_animation_status_render_faster", status_path)


def run_phase6_cross_scene_save_load_acceptance(status_path, output_dir):
    reset_scene()
    window = active_view3d_context()["window"]
    scene_a = bpy.context.scene
    scene_a.name = f"{PREFIX}_CheckpointScene"
    scene_a_name = scene_a.name
    configure_game_settings(True)

    save_path = str(output_dir / f"{PREFIX}_Phase6CrossSceneSaveLoad.sav")
    save_file = Path(save_path)
    try:
        save_file.unlink()
    except FileNotFoundError:
        pass

    target = create_cube(f"{PREFIX}_Phase6CrossSceneTarget")
    score = set_game_property(target, "score", "FLOAT", 42.0)
    score.use_save_game = True
    set_game_property(target, "save_completed", "BOOL", False)
    set_game_property(target, "load_completed", "BOOL", False)
    add_status_controller(
        target, status_path, "phase6_cross_scene_save", required_frame=60, tolerance=0.001)
    set_game_property(target, "__ln_acceptance_expected_scene", "STRING", scene_a_name)
    set_game_property(target, "__ln_acceptance_expected_score", "FLOAT", 42.0)

    save_tree = new_logic_tree(f"{PREFIX}_Phase6CrossSceneSaveTree")
    save_tree_name = save_tree.name
    on_init = save_tree.nodes.new("LogicNativeOnInit")
    save_game = save_tree.nodes.new("LogicNativeSaveGame")
    save_succeeded = save_tree.nodes.new("LogicNativeBranch")
    mark_save_completed = save_tree.nodes.new("LogicNativeSetGamePropertyBool")
    destination_tree = new_logic_tree(f"{PREFIX}_Phase6CrossSceneDestinationTree")
    on_game_loaded = destination_tree.nodes.new("LogicNativeOnGameLoaded")
    load_succeeded = destination_tree.nodes.new("LogicNativeBranch")
    mark_load_completed = destination_tree.nodes.new("LogicNativeSetGamePropertyBool")

    save_game.inputs["Path"].default_value = save_path
    mark_save_completed.inputs["Property"].default_value = "save_completed"
    mark_save_completed.inputs["Value"].default_value = True
    mark_load_completed.inputs["Property"].default_value = "load_completed"
    mark_load_completed.inputs["Value"].default_value = True

    save_tree.links.new(on_init.outputs["Out"], flow_input(save_game))
    save_tree.links.new(save_game.outputs["Done"], save_succeeded.inputs["Flow"])
    save_tree.links.new(
        save_game.outputs["Success"], save_succeeded.inputs["Condition"])
    save_tree.links.new(
        save_succeeded.outputs["True"], flow_input(mark_save_completed))
    destination_tree.links.new(on_game_loaded.outputs["Out"], load_succeeded.inputs["Flow"])
    destination_tree.links.new(
        on_game_loaded.outputs["Success"], load_succeeded.inputs["Condition"])
    destination_tree.links.new(
        load_succeeded.outputs["True"], flow_input(mark_load_completed))
    bind_tree(target, save_tree.name, True)
    bind_tree(target, destination_tree.name, True)

    target = run_game_once(target, "phase6_cross_scene_save_0", status_path)
    require(target is not None, "Cross-scene Save Game target did not survive the save run")
    require(save_file.exists(), f"Cross-scene Save Game did not create {save_path}")
    checkpoint = json.loads(save_file.read_text(encoding="utf-8"))
    require(checkpoint.get("scene_name") == scene_a_name,
            f"Cross-scene checkpoint stored the wrong destination: {checkpoint}")

    for binding in target.game.logic_node_bindings:
        if binding.tree_name == save_tree_name:
            binding.enabled = False
    set_existing_game_property_value(target, "score", 99.0)
    set_existing_game_property_value(
        target, "__ln_acceptance_mode", "phase6_cross_scene_load")

    existing_scene_b = bpy.data.scenes.get(f"{PREFIX}_LoaderScene")
    if existing_scene_b is not None:
        bpy.data.scenes.remove(existing_scene_b)
    scene_b = bpy.data.scenes.new(f"{PREFIX}_LoaderScene")
    scene_b_name = scene_b.name
    window.scene = scene_b
    configure_game_settings(True)
    loader = create_cube(f"{PREFIX}_Phase6CrossSceneLoader")
    set_game_property(loader, "__ln_acceptance_await_scene", "STRING", scene_a_name)
    loader_tree = new_logic_tree(f"{PREFIX}_Phase6CrossSceneLoaderTree")
    loader_init = loader_tree.nodes.new("LogicNativeOnInit")
    load_game = loader_tree.nodes.new("LogicNativeLoadGame")
    load_game.inputs["Path"].default_value = save_path
    loader_tree.links.new(loader_init.outputs["Out"], flow_input(load_game))
    bind_tree(loader, loader_tree.name, True)

    try:
        run_game_once(target, "phase6_cross_scene_load_0", status_path)
    finally:
        scene_a = bpy.data.scenes.get(scene_a_name)
        if scene_a is not None:
            window.scene = scene_a
        scene_b = bpy.data.scenes.get(scene_b_name)
        if scene_b is not None:
            bpy.data.scenes.remove(scene_b)


def run_phase6_save_load_acceptance(status_path, output_dir):
    reset_scene()
    configure_game_settings(True)
    transform_parent = create_cube(f"{PREFIX}_Phase6SaveLoadParent")
    transform_parent.location = (-2.0, 1.0, 0.5)
    transform_parent.rotation_euler = (0.0, 0.0, 0.35)
    transform_parent.scale = (1.25, 1.25, 1.25)
    transform_parent.game.use_save_game_object_state = True
    target = create_cube(f"{PREFIX}_Phase6SaveLoad")
    target.location = (2.0, 3.0, 4.0)
    bpy.context.view_layer.update()
    target_world = target.matrix_world.copy()
    target.parent = transform_parent
    target.matrix_world = target_world
    target.game.use_save_game_object_state = True
    property_only = create_cube(f"{PREFIX}_Phase6SaveLoadPropertyOnly")
    property_only_value = set_game_property(property_only, "property_only_score", "INT", 17)
    property_only_value.use_save_game = True
    physics_probe = create_cube(f"{PREFIX}_Phase6SaveLoadPhysics", rigid_body=True)
    physics_probe.location = (0.0, -5.0, 3.0)
    physics_probe.game.use_save_game_object_state = True
    soft_body = create_soft_body(f"{PREFIX}_Phase6SaveLoadSoft", (-3.0, 1.5, 5.0))
    removed_object = create_cube(f"{PREFIX}_Phase6SaveLoadRemoved", rigid_body=True)
    removed_object.location = (-6.0, 0.0, 1.0)
    removed_object.game.use_save_game_object_state = True
    replica_template = create_cube(f"{PREFIX}_Phase6SaveLoadAddedTemplate")
    replica_template.location = (6.0, 0.0, 1.0)
    replica_template.game.use_save_game_object_state = True
    replica_template.hide_viewport = True
    bpy.context.view_layer.update()
    save_path = str(output_dir / f"{PREFIX}_Phase6SaveLoad.sav")
    save_file = Path(save_path)
    if save_file.exists():
        save_file.unlink()

    tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    once_after_init = tree.nodes.new("LogicNativeOnce")
    get_run_checkpoint_sequence = tree.nodes.new("LogicNativeGetGamePropertyBool")
    run_checkpoint_sequence = tree.nodes.new("LogicNativeBranch")
    initial_tree_value = tree.nodes.new("LogicNativeValueFloat")
    set_initial_tree_property = tree.nodes.new("LogicNativeSetTreeProperty")
    set_saved_gravity = tree.nodes.new("LogicNativeSetGravity")
    set_saved_position = new_set_object_attribute(tree, "WORLD_POSITION")
    set_saved_no_collision = tree.nodes.new("LogicNativeSetDynamics")
    set_saved_ghost = tree.nodes.new("LogicNativeSetDynamics")
    set_saved_physics = tree.nodes.new("LogicNativeSetPhysics")
    add_saved_replica = tree.nodes.new("LogicNativeAddObject")
    save_game = tree.nodes.new("LogicNativeSaveGame")
    save_succeeded = tree.nodes.new("LogicNativeBranch")
    mutated_tree_value = tree.nodes.new("LogicNativeValueFloat")
    set_mutated_tree_property = tree.nodes.new("LogicNativeSetTreeProperty")
    set_mutated_physics = tree.nodes.new("LogicNativeSetPhysics")
    set_mutated_dynamics = tree.nodes.new("LogicNativeSetDynamics")
    set_mutated_ghost = tree.nodes.new("LogicNativeSetDynamics")
    set_mutated_gravity = tree.nodes.new("LogicNativeSetGravity")
    mutate_score = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    mutate_transient_score = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    mutate_position = new_set_object_attribute(tree, "WORLD_POSITION")
    mutate_parent_position = new_set_object_attribute(tree, "WORLD_POSITION")
    mutate_parent_orientation = new_set_object_attribute(tree, "WORLD_ORIENTATION")
    mutate_parent_scale = new_set_object_attribute(tree, "WORLD_SCALE")
    remove_saved_parent = tree.nodes.new("LogicNativeRemoveParent")
    mutate_soft_color = new_set_object_attribute(tree, "COLOR")
    mutate_removed_color = new_set_object_attribute(tree, "COLOR")
    remove_saved_object = tree.nodes.new("LogicNativeRemoveObject")
    add_current_object = tree.nodes.new("LogicNativeAddObject")
    load_game = tree.nodes.new("LogicNativeLoadGame")
    load_succeeded = tree.nodes.new("LogicNativeBranch")
    get_restored_tree_property = tree.nodes.new("LogicNativeGetTreeProperty")
    copy_restored_tree_property = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    get_restored_gravity = tree.nodes.new("LogicNativeGetGravity")
    separate_restored_gravity = tree.nodes.new("LogicNativeSeparateXYZ")
    copy_restored_gravity = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    missing_load = tree.nodes.new("LogicNativeLoadGame")
    missing_load_failed = tree.nodes.new("LogicNativeBranch")
    mark_load_completed = tree.nodes.new("LogicNativeSetGamePropertyBool")
    get_run_checkpoint_sequence.inputs["Object"].default_value = target
    get_run_checkpoint_sequence.inputs["Property"].default_value = "run_checkpoint_sequence"
    value_output(initial_tree_value).default_value = 3.5
    set_initial_tree_property.inputs["Property"].default_value = "checkpoint_tree_score"
    set_saved_gravity.inputs["Gravity"].default_value = (0.0, 0.0, -7.25)
    value_output(mutated_tree_value).default_value = 99.0
    set_mutated_tree_property.inputs["Property"].default_value = "checkpoint_tree_score"
    set_mutated_gravity.inputs["Gravity"].default_value = (0.0, 0.0, -3.0)
    set_saved_position.inputs["Object"].default_value = target
    set_saved_position.inputs["Value"].default_value = (2.0, 3.0, 4.0)
    set_saved_no_collision.inputs["Object"].default_value = physics_probe
    set_saved_no_collision.dynamics_mode = "NO_COLLISION"
    set_saved_ghost.inputs["Object"].default_value = physics_probe
    set_saved_ghost.dynamics_mode = "GHOST"
    set_saved_ghost.inputs["Enabled"].default_value = True
    set_saved_physics.inputs["Object"].default_value = physics_probe
    set_saved_physics.inputs["Active"].default_value = False
    add_saved_replica.inputs["Object to Add"].default_value = replica_template
    add_saved_replica.inputs["Copy Transform"].default_value = replica_template
    add_saved_replica.inputs["Life"].default_value = 0.0
    add_saved_replica.inputs["Full Copy"].default_value = False
    save_game.inputs["Path"].default_value = save_path
    set_mutated_physics.inputs["Object"].default_value = physics_probe
    set_mutated_physics.inputs["Active"].default_value = True
    set_mutated_dynamics.inputs["Object"].default_value = physics_probe
    set_mutated_dynamics.dynamics_mode = "DYNAMIC"
    set_mutated_ghost.inputs["Object"].default_value = physics_probe
    set_mutated_ghost.dynamics_mode = "GHOST"
    set_mutated_ghost.inputs["Enabled"].default_value = False
    mutate_score.inputs["Object"].default_value = target
    mutate_score.inputs["Property"].default_value = "score"
    mutate_score.inputs["Value"].default_value = 99.0
    mutate_transient_score.inputs["Object"].default_value = target
    mutate_transient_score.inputs["Property"].default_value = "transient_score"
    mutate_transient_score.inputs["Value"].default_value = 99.0
    mutate_position.inputs["Object"].default_value = target
    mutate_position.inputs["Value"].default_value = (20.0, 30.0, 40.0)
    mutate_parent_position.inputs["Object"].default_value = transform_parent
    mutate_parent_position.inputs["Value"].default_value = (5.0, -4.0, 2.0)
    mutate_parent_orientation.inputs["Object"].default_value = transform_parent
    mutate_parent_orientation.inputs["Rotation"].default_value = (0.0, 0.0, -0.6)
    mutate_parent_scale.inputs["Object"].default_value = transform_parent
    mutate_parent_scale.inputs["Value"].default_value = (2.0, 2.0, 2.0)
    remove_saved_parent.inputs["Child Object"].default_value = target
    mutate_soft_color.inputs["Object"].default_value = soft_body
    mutate_soft_color.inputs["Color"].default_value = (0.2, 0.4, 0.6, 1.0)
    mutate_removed_color.inputs["Object"].default_value = removed_object
    mutate_removed_color.inputs["Color"].default_value = (1.0, 0.0, 0.0, 1.0)
    remove_saved_object.inputs["Object"].default_value = removed_object
    add_current_object.inputs["Object to Add"].default_value = replica_template
    add_current_object.inputs["Copy Transform"].default_value = replica_template
    add_current_object.inputs["Life"].default_value = 0.0
    add_current_object.inputs["Full Copy"].default_value = False
    load_game.inputs["Path"].default_value = save_path
    get_restored_tree_property.inputs["Property"].default_value = "checkpoint_tree_score"
    copy_restored_tree_property.inputs["Object"].default_value = target
    copy_restored_tree_property.inputs["Property"].default_value = "tree_checkpoint_value"
    copy_restored_gravity.inputs["Object"].default_value = target
    copy_restored_gravity.inputs["Property"].default_value = "gravity_checkpoint_z"
    missing_path = save_path + ".missing"
    missing_file = Path(missing_path)
    if missing_file.exists():
        missing_file.unlink()
    missing_load.inputs["Path"].default_value = missing_path
    mark_load_completed.inputs["Object"].default_value = target
    mark_load_completed.inputs["Property"].default_value = "load_completed"
    mark_load_completed.inputs["Value"].default_value = True
    tree.links.new(on_init.outputs["Out"], flow_input(set_initial_tree_property))
    tree.links.new(value_output(initial_tree_value), set_initial_tree_property.inputs["Value"])
    tree.links.new(on_fixed_update.outputs["Out"], once_after_init.inputs["Flow"])
    tree.links.new(once_after_init.outputs["Out"], run_checkpoint_sequence.inputs["Flow"])
    tree.links.new(
        get_run_checkpoint_sequence.outputs["Value"],
        run_checkpoint_sequence.inputs["Condition"],
    )
    tree.links.new(run_checkpoint_sequence.outputs["True"], flow_input(set_saved_gravity))
    tree.links.new(set_saved_gravity.outputs["Done"], flow_input(set_saved_position))
    tree.links.new(set_saved_position.outputs["Done"], flow_input(set_saved_no_collision))
    tree.links.new(set_saved_no_collision.outputs["Done"], flow_input(set_saved_ghost))
    tree.links.new(set_saved_ghost.outputs["Done"], flow_input(set_saved_physics))
    tree.links.new(set_saved_physics.outputs["Done"], add_saved_replica.inputs["Flow"])
    tree.links.new(add_saved_replica.outputs["Done"], flow_input(save_game))
    tree.links.new(save_game.outputs["Done"], save_succeeded.inputs["Flow"])
    tree.links.new(save_game.outputs["Success"], save_succeeded.inputs["Condition"])
    tree.links.new(save_succeeded.outputs["True"], flow_input(set_mutated_tree_property))
    tree.links.new(value_output(mutated_tree_value), set_mutated_tree_property.inputs["Value"])
    tree.links.new(save_succeeded.outputs["True"], flow_input(set_mutated_physics))
    tree.links.new(set_mutated_physics.outputs["Done"], flow_input(set_mutated_dynamics))
    tree.links.new(set_mutated_dynamics.outputs["Done"], flow_input(set_mutated_ghost))
    tree.links.new(set_mutated_ghost.outputs["Done"], flow_input(set_mutated_gravity))
    tree.links.new(set_mutated_gravity.outputs["Done"], flow_input(mutate_score))
    tree.links.new(mutate_score.outputs["Done"], flow_input(mutate_transient_score))
    tree.links.new(mutate_transient_score.outputs["Done"], flow_input(mutate_position))
    tree.links.new(mutate_position.outputs["Done"], flow_input(mutate_parent_position))
    tree.links.new(
        mutate_parent_position.outputs["Done"], flow_input(mutate_parent_orientation))
    tree.links.new(
        mutate_parent_orientation.outputs["Done"], flow_input(mutate_parent_scale))
    tree.links.new(mutate_parent_scale.outputs["Done"], remove_saved_parent.inputs["Flow"])
    tree.links.new(remove_saved_parent.outputs["Done"], flow_input(mutate_soft_color))
    tree.links.new(mutate_soft_color.outputs["Done"], flow_input(mutate_removed_color))
    tree.links.new(mutate_removed_color.outputs["Done"], flow_input(remove_saved_object))
    tree.links.new(remove_saved_object.outputs["Done"], add_current_object.inputs["Flow"])
    tree.links.new(add_current_object.outputs["Done"], flow_input(load_game))
    tree.links.new(load_game.outputs["Done"], load_succeeded.inputs["Flow"])
    tree.links.new(load_game.outputs["Success"], load_succeeded.inputs["Condition"])
    tree.links.new(load_succeeded.outputs["True"], flow_input(copy_restored_tree_property))
    tree.links.new(get_restored_tree_property.outputs["Value"],
                   copy_restored_tree_property.inputs["Value"])
    tree.links.new(get_restored_gravity.outputs["Gravity"], separate_restored_gravity.inputs["Vector"])
    tree.links.new(separate_restored_gravity.outputs["Z"], copy_restored_gravity.inputs["Value"])
    tree.links.new(copy_restored_tree_property.outputs["Done"], flow_input(copy_restored_gravity))
    tree.links.new(copy_restored_gravity.outputs["Done"], flow_input(missing_load))
    tree.links.new(missing_load.outputs["Done"], missing_load_failed.inputs["Flow"])
    tree.links.new(missing_load.outputs["Success"], missing_load_failed.inputs["Condition"])
    tree.links.new(missing_load_failed.outputs["False"], flow_input(mark_load_completed))

    status_observer = create_cube(f"{PREFIX}_Phase6SaveLoadStatus")
    add_status_controller(
        status_observer, status_path, "phase6_save_load", required_frame=30, tolerance=0.001)
    set_game_property(
        status_observer, "__ln_acceptance_subject", "STRING", target.name)
    score_property = set_game_property(target, "score", "FLOAT", 42.0)
    score_property.use_save_game = True
    set_game_property(target, "transient_score", "FLOAT", 7.0)
    set_game_property(target, "tree_checkpoint_value", "FLOAT", 0.0)
    set_game_property(target, "gravity_checkpoint_z", "FLOAT", 0.0)
    set_game_property(target, "load_completed", "BOOL", False)
    set_game_property(target, "run_checkpoint_sequence", "BOOL", True)
    set_game_property(target, "__ln_acceptance_expected_score", "FLOAT", 42.0)
    set_game_property(target, "__ln_acceptance_expected_tree_score", "FLOAT", 99.0)
    set_game_property(target, "__ln_acceptance_expected_gravity_z", "FLOAT", -3.0)
    set_game_property(target, "__ln_acceptance_expected_x", "FLOAT", 2.0)
    set_game_property(target, "__ln_acceptance_expected_y", "FLOAT", 3.0)
    set_game_property(target, "__ln_acceptance_expected_z", "FLOAT", 4.0)
    set_game_property(
        target, "__ln_acceptance_expected_parent", "STRING", transform_parent.name)
    set_game_property(target, "__ln_acceptance_physics_object", "STRING", physics_probe.name)
    set_game_property(target, "__ln_acceptance_soft_object", "STRING", soft_body.name)
    set_game_property(
        target, "__ln_acceptance_reconstructed_object", "STRING", removed_object.name)
    set_game_property(
        target, "__ln_acceptance_reconstructed_mesh", "STRING", removed_object.data.name)
    bind_tree(target, tree.name, True)
    main_tree_name = tree.name
    target_name = target.name
    transform_parent_name = transform_parent.name
    property_only_name = property_only.name
    physics_probe_name = physics_probe.name
    soft_body_name = soft_body.name
    removed_object_name = removed_object.name
    replica_template_name = replica_template.name
    status_observer_name = status_observer.name
    run_game_once(status_observer, "phase6_save_load_0", status_path)
    require(save_file.exists(), f"Save Game did not create {save_path}")
    checkpoint = json.loads(save_file.read_text(encoding="utf-8"))
    require(checkpoint.get("format") == "UPBGE_CXX_LOGIC_NODES_CHECKPOINT",
            "Save Game file has the wrong explicit-data format")
    require("version" not in checkpoint,
            "Save Game file unexpectedly contains a numeric schema version")
    require(isinstance(checkpoint.get("checksum"), str) and len(checkpoint["checksum"]) == 32,
            "Save Game file is missing its integrity checksum")
    require("scene_gravity" not in checkpoint and "time_scale" not in checkpoint,
            "Save Game file contains automatic scene state")
    require("runtime_trees" not in checkpoint,
            "Save Game file contains the obsolete scene-global Logic Tree state")
    authored_records = checkpoint.get("authored_objects", [])
    spawned_records = checkpoint.get("spawned_instances", [])
    require(isinstance(authored_records, list) and isinstance(spawned_records, list),
            "Save Game file is missing its authored/spawned record lists")
    saved_records = authored_records + spawned_records
    require(all(not item.get("instance_uuid", "").startswith("session:")
                for item in saved_records),
            "Save Game file contains session-only replica state")
    saved_target = next(
        (item for item in authored_records if item.get("name") == target_name),
        None,
    )
    require(saved_target is not None, "Save Game file is missing the target object")
    saved_logic_trees = saved_target.get("logic_trees")
    require(
        isinstance(saved_logic_trees, list) and
        any(item.get("name") == main_tree_name and item.get("enabled") is True
            for item in saved_logic_trees),
        "Save Object State did not capture the target's C++ Logic Node attachments",
    )
    require(isinstance(saved_target.get("modifier_state"), dict),
            "Save Object State did not capture the bounded modifier manifest")
    saved_names = {item.get("name") for item in saved_records}
    require({target_name, transform_parent_name, property_only_name, physics_probe_name,
             removed_object_name}.issubset(saved_names),
            "Save Game file is missing explicitly marked authored objects")
    require(saved_names.issubset({target_name, transform_parent_name, property_only_name,
                                  physics_probe_name, removed_object_name,
                                  replica_template_name}),
            "Save Game file contains objects without explicitly marked data")
    saved_property_only = next(
        (item for item in authored_records if item.get("name") == property_only_name),
        None,
    )
    require(saved_property_only is not None and "transform" not in saved_property_only
            and "exists" not in saved_property_only
            and "logic_trees" not in saved_property_only
            and "modifier_state" not in saved_property_only,
            "The property-only participant unexpectedly captured unmarked state")
    property_only_record = next(
        (item for item in saved_property_only.get("properties", [])
         if item.get("name") == "property_only_score"),
        None,
    )
    require(property_only_record is not None and property_only_record.get("value") == 17,
            "The property-only participant was not saved through its durable identity")
    saved_physics_probe = next(
        (item for item in authored_records if item.get("name") == physics_probe_name),
        None,
    )
    require(saved_physics_probe is not None
            and isinstance(saved_physics_probe.get("physics"), dict),
            "Save Game file is missing semantic Jolt state")
    require(isinstance(saved_target.get("physics"), dict)
            and saved_target["physics"].get("dynamic_body") is False,
            "Save Object State did not capture the target's static Jolt state")
    require(all(item.get("name") != soft_body_name for item in authored_records),
            "Unsupported unmarked soft-body state was captured")
    saved_position = tuple(
        float(value) for value in saved_target.get("transform", {}).get("position", ()))
    require(len(saved_position) == 3 and all(
        abs(observed - expected) <= 0.0001
        for observed, expected in zip(saved_position, (2.0, 3.0, 4.0))
    ),
            "Save Game file is missing the saved runtime position")
    saved_score = next(
        (item for item in saved_target.get("properties", []) if item.get("name") == "score"),
        None,
    )
    require(saved_score is not None and abs(float(saved_score.get("value", 0.0)) - 42.0) <= 0.0001,
            "Save Game file is missing the saved score value")
    require(all(item.get("name") != "transient_score"
                for item in saved_target.get("properties", [])),
            "Save Game file contains an unmarked game property")

    # Start a fresh game conversion after changing the authored scene. This is the regression for
    # loading a disk save after objects were added/removed and defaults changed in the .blend.
    target = resolve_live_object(target_name)
    for binding in target.game.logic_node_bindings:
        binding.enabled = False
    removed_object = bpy.data.objects.get(removed_object_name)
    if removed_object is not None:
        bpy.data.objects.remove(removed_object, do_unlink=True)
    current_object = create_cube(f"{PREFIX}_Phase6SaveLoadCurrentOnly")
    current_object.location = (9.0, 1.0, 2.0)
    current_object.game.use_save_game_object_state = True
    set_existing_game_property_value(target, "score", 101.0)
    set_existing_game_property_value(target, "transient_score", 123.0)
    set_existing_game_property_value(target, "load_completed", False)
    set_existing_game_property_value(target, "run_checkpoint_sequence", False)
    status_observer = resolve_live_object(status_observer_name)
    set_existing_game_property_value(
        status_observer, "__ln_acceptance_mode", "phase6_save_load_revision")
    set_game_property(
        target, "__ln_acceptance_current_object", "STRING", current_object.name)
    set_game_property(
        target, "__ln_acceptance_removed_object", "STRING", removed_object_name)

    load_tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadRevisionTree")
    on_update = load_tree.nodes.new("LogicNativeOnUpdate")
    once = load_tree.nodes.new("LogicNativeOnce")
    load_game = load_tree.nodes.new("LogicNativeLoadGame")
    load_succeeded = load_tree.nodes.new("LogicNativeBranch")
    mark_load_completed = load_tree.nodes.new("LogicNativeSetGamePropertyBool")
    load_game.inputs["Path"].default_value = save_path
    mark_load_completed.inputs["Object"].default_value = target
    mark_load_completed.inputs["Property"].default_value = "load_completed"
    mark_load_completed.inputs["Value"].default_value = True
    load_tree.links.new(on_update.outputs["Out"], once.inputs["Flow"])
    load_tree.links.new(once.outputs["Out"], load_game.inputs["Flow"])
    load_tree.links.new(load_game.outputs["Done"], load_succeeded.inputs["Flow"])
    load_tree.links.new(load_game.outputs["Success"], load_succeeded.inputs["Condition"])
    load_tree.links.new(
        load_succeeded.outputs["True"], mark_load_completed.inputs["Flow"])
    # The target's exact Logic Node attachment manifest is itself restored by Load Game. Keep the
    # loading controller on a non-persistent manager object so its completion flow remains valid.
    bind_tree(status_observer, load_tree.name, True)
    run_game_once(status_observer, "phase6_save_load_revision_1", status_path)


def run_phase6_save_load_structure_acceptance(status_path, output_dir):
    reset_scene()
    configure_game_settings(True)

    create_ground(
        f"{PREFIX}_Phase6SaveLoadStructureGround",
        location=(0.0, 0.0, -2.0),
        scale=(6.0, 6.0, 1.0),
    )
    target = create_cube(f"{PREFIX}_Phase6SaveLoadStructure", rigid_body=True)
    target.location = (0.0, 0.0, 1.0)
    target.game.use_save_game_object_state = True

    geometry_tree = bpy.data.node_groups.new(
        f"{PREFIX}_PersistentGeometry", "GeometryNodeTree")
    geometry_tree.use_fake_user = True
    geometry_tree.is_modifier = True
    geometry_input = geometry_tree.interface.new_socket(
        name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    value_input = geometry_tree.interface.new_socket(
        name="Amount", in_out="INPUT", socket_type="NodeSocketFloat")
    geometry_output = geometry_tree.interface.new_socket(
        name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    value_output_socket = geometry_tree.interface.new_socket(
        name="Result", in_out="OUTPUT", socket_type="NodeSocketFloat")
    group_input = geometry_tree.nodes.new("NodeGroupInput")
    group_output = geometry_tree.nodes.new("NodeGroupOutput")
    internal_ico = geometry_tree.nodes.new("GeometryNodeMeshIcoSphere")
    internal_ico.name = f"{PREFIX}_RuntimeIcoSphere"
    internal_radius_socket = internal_ico.inputs["Radius"]
    saved_internal_radius = 0.2
    mutated_internal_radius = 1.0
    internal_radius_socket.default_value = saved_internal_radius
    geometry_tree.links.new(
        group_input.outputs[geometry_input.identifier],
        group_output.inputs[geometry_output.identifier],
    )
    geometry_tree.links.new(
        group_input.outputs[value_input.identifier],
        group_output.inputs[value_output_socket.identifier],
    )

    modifier_name = f"{PREFIX}_PersistentGeometryModifier"
    transient_modifier_name = f"{PREFIX}_TransientGeometryModifier"
    modifier = target.modifiers.new(modifier_name, "NODES")
    modifier.node_group = geometry_tree
    bpy.context.view_layer.update()
    saved_value = 2.25
    mutated_value = 9.5
    modifier_input = getattr(modifier.properties.inputs, value_input.identifier)
    modifier_input.type = "VALUE"
    modifier_input.value = saved_value
    modifier_output = getattr(
        modifier.properties.outputs, value_output_socket.identifier)
    modifier_output.attribute_name = "saved_attr"
    removed_modifier_name = f"{PREFIX}_RemovedGeometryModifier"
    removed_modifier = target.modifiers.new(removed_modifier_name, "NODES")
    removed_modifier.node_group = geometry_tree
    bpy.context.view_layer.update()

    compositor_tree = bpy.data.node_groups.new(
        f"{PREFIX}_PersistentCompositor", "CompositorNodeTree")
    compositor_tree.use_fake_user = True
    compositor_node = compositor_tree.nodes.new("CompositorNodeBrightContrast")
    compositor_node.name = f"{PREFIX}_RuntimeCompositorBrightness"
    compositor_socket = compositor_node.inputs["Brightness"]
    saved_compositor_value = 12.5
    mutated_compositor_value = 75.0
    compositor_socket.default_value = saved_compositor_value
    bpy.context.scene.compositing_node_group = compositor_tree
    transient_compositor_tree = bpy.data.node_groups.new(
        f"{PREFIX}_TransientCompositor", "CompositorNodeTree")
    transient_compositor_tree.use_fake_user = True

    installed_tree = new_logic_tree(f"{PREFIX}_RuntimeInstalledTree")
    installed_update = installed_tree.nodes.new("LogicNativeOnUpdate")
    mark_installed_running = installed_tree.nodes.new(
        "LogicNativeSetGamePropertyBool")
    mark_installed_running.inputs["Object"].default_value = target
    mark_installed_running.inputs["Property"].default_value = "installed_tree_restored"
    mark_installed_running.inputs["Value"].default_value = True
    installed_tree.links.new(
        installed_update.outputs["Out"], flow_input(mark_installed_running))

    transient_tree = new_logic_tree(f"{PREFIX}_TransientRuntimeTree")
    transient_update = transient_tree.nodes.new("LogicNativeOnUpdate")
    mark_transient_running = transient_tree.nodes.new(
        "LogicNativeSetGamePropertyBool")
    mark_transient_running.inputs["Object"].default_value = target
    mark_transient_running.inputs["Property"].default_value = "transient_tree_running"
    mark_transient_running.inputs["Value"].default_value = True
    transient_tree.links.new(
        transient_update.outputs["Out"], flow_input(mark_transient_running))

    save_path = str(output_dir / f"{PREFIX}_Phase6SaveLoadStructure.sav")
    save_file = Path(save_path)
    if save_file.exists():
        save_file.unlink()

    tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadStructureTree")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    once = tree.nodes.new("LogicNativeOnce")
    install_saved_tree = tree.nodes.new("LogicNativeInstallLogicTree")
    wait_before_save = tree.nodes.new("LogicNativeDelay")
    save_game = tree.nodes.new("LogicNativeSaveGame")
    save_succeeded = tree.nodes.new("LogicNativeBranch")
    stop_saved_tree = tree.nodes.new("LogicNativeStopLogicTree")
    mutate_geometry_input = tree.nodes.new("LogicNativeSetGeometryNodesInput")
    append_geometry_modifier = tree.nodes.new(
        "LogicNativeAssignGeometryNodesModifier")
    install_transient_tree = tree.nodes.new("LogicNativeInstallLogicTree")
    request_output_mutation = tree.nodes.new("LogicNativeSetGamePropertyBool")
    collision = tree.nodes.new("LogicNativeOnCollision")
    mutate_internal_node = tree.nodes.new("LogicNativeSetEditorNodeValue")
    mark_collision_mutation = tree.nodes.new("LogicNativeSetGamePropertyBool")
    wait_before_load = tree.nodes.new("LogicNativeDelay")
    clear_installed_marker = tree.nodes.new("LogicNativeSetGamePropertyBool")
    load_game = tree.nodes.new("LogicNativeLoadGame")
    load_succeeded = tree.nodes.new("LogicNativeBranch")
    clear_transient_marker = tree.nodes.new("LogicNativeSetGamePropertyBool")
    wait_after_load = tree.nodes.new("LogicNativeDelay")
    mark_load_completed = tree.nodes.new("LogicNativeSetGamePropertyBool")

    install_saved_tree.inputs["Object"].default_value = target
    install_saved_tree.inputs["Tree Name"].default_value = installed_tree.name
    install_saved_tree.inputs["Initialize"].default_value = True
    wait_before_save.inputs["Delay"].default_value = 0.05
    save_game.inputs["Path"].default_value = save_path
    stop_saved_tree.inputs["Object"].default_value = target
    stop_saved_tree.inputs["Tree Name"].default_value = installed_tree.name
    mutate_geometry_input.value_type = "FLOAT"
    mutate_geometry_input.inputs["Object"].default_value = target
    mutate_geometry_input.inputs["Modifier"].default_value = modifier_name
    mutate_geometry_input.inputs["Input"].default_value = value_input.identifier
    mutate_geometry_input.inputs["Float Value"].default_value = mutated_value
    append_geometry_modifier.operation = "APPEND"
    append_geometry_modifier.geometry_node_group = geometry_tree
    append_geometry_modifier.inputs["Object"].default_value = target
    append_geometry_modifier.inputs[
        "Modifier Name"].default_value = transient_modifier_name
    install_transient_tree.inputs["Object"].default_value = target
    install_transient_tree.inputs["Tree Name"].default_value = transient_tree.name
    install_transient_tree.inputs["Initialize"].default_value = True
    request_output_mutation.inputs["Object"].default_value = target
    request_output_mutation.inputs["Property"].default_value = "structure_mutate_output"
    request_output_mutation.inputs["Value"].default_value = True
    collision.inputs["Object"].default_value = target
    mutate_internal_node.node_editor_type = "GEOMETRY"
    mutate_internal_node.value_type = "FLOAT"
    mutate_internal_node.inputs["Object"].default_value = target
    mutate_internal_node.inputs["Modifier"].default_value = modifier_name
    mutate_internal_node.inputs["Node Name"].default_value = internal_ico.name
    mutate_internal_node.inputs["Socket"].default_value = internal_radius_socket.identifier
    mutate_internal_node.inputs["Float Value"].default_value = mutated_internal_radius
    mark_collision_mutation.inputs["Object"].default_value = target
    mark_collision_mutation.inputs[
        "Property"].default_value = "structure_collision_mutated"
    mark_collision_mutation.inputs["Value"].default_value = True
    wait_before_load.inputs["Delay"].default_value = 1.25
    clear_installed_marker.inputs["Object"].default_value = target
    clear_installed_marker.inputs["Property"].default_value = "installed_tree_restored"
    clear_installed_marker.inputs["Value"].default_value = False
    load_game.inputs["Path"].default_value = save_path
    clear_transient_marker.inputs["Object"].default_value = target
    clear_transient_marker.inputs["Property"].default_value = "transient_tree_running"
    clear_transient_marker.inputs["Value"].default_value = False
    wait_after_load.inputs["Delay"].default_value = 0.05
    mark_load_completed.inputs["Object"].default_value = target
    mark_load_completed.inputs["Property"].default_value = "load_completed"
    mark_load_completed.inputs["Value"].default_value = True

    tree.links.new(on_fixed_update.outputs["Out"], once.inputs["Flow"])
    tree.links.new(once.outputs["Out"], install_saved_tree.inputs["Flow"])
    tree.links.new(install_saved_tree.outputs["Done"], wait_before_save.inputs["Flow"])
    tree.links.new(wait_before_save.outputs["Done"], save_game.inputs["Flow"])
    tree.links.new(save_game.outputs["Done"], save_succeeded.inputs["Flow"])
    tree.links.new(save_game.outputs["Success"], save_succeeded.inputs["Condition"])
    tree.links.new(save_succeeded.outputs["True"], stop_saved_tree.inputs["Flow"])
    tree.links.new(
        stop_saved_tree.outputs["Done"], mutate_geometry_input.inputs["Flow"])
    tree.links.new(
        mutate_geometry_input.outputs["Done"], append_geometry_modifier.inputs["Flow"])
    tree.links.new(
        append_geometry_modifier.outputs["Done"], install_transient_tree.inputs["Flow"])
    tree.links.new(
        install_transient_tree.outputs["Done"], request_output_mutation.inputs["Flow"])
    tree.links.new(collision.outputs["On Enter"], mutate_internal_node.inputs["Flow"])
    tree.links.new(
        mutate_internal_node.outputs["Done"], mark_collision_mutation.inputs["Flow"])
    tree.links.new(
        request_output_mutation.outputs["Done"], wait_before_load.inputs["Flow"])
    tree.links.new(
        wait_before_load.outputs["Done"], clear_installed_marker.inputs["Flow"])
    tree.links.new(clear_installed_marker.outputs["Done"], load_game.inputs["Flow"])
    tree.links.new(load_game.outputs["Done"], load_succeeded.inputs["Flow"])
    tree.links.new(load_game.outputs["Success"], load_succeeded.inputs["Condition"])
    tree.links.new(
        load_succeeded.outputs["True"], clear_transient_marker.inputs["Flow"])
    tree.links.new(
        clear_transient_marker.outputs["Done"], wait_after_load.inputs["Flow"])
    tree.links.new(
        wait_after_load.outputs["Done"], mark_load_completed.inputs["Flow"])

    status_observer = create_cube(f"{PREFIX}_Phase6SaveLoadStructureStatus")
    status_observer.game.physics_type = "NO_COLLISION"
    add_status_controller(
        status_observer,
        status_path,
        "phase6_save_load_structure",
        required_frame=180,
        tolerance=0.0001,
    )
    set_game_property(target, "installed_tree_restored", "BOOL", False)
    set_game_property(target, "transient_tree_running", "BOOL", False)
    set_game_property(target, "structure_mutate_output", "BOOL", False)
    set_game_property(target, "structure_output_mutated", "BOOL", False)
    set_game_property(target, "structure_collision_mutated", "BOOL", False)
    set_game_property(target, "structure_mutation_error", "STRING", "")
    set_game_property(target, "load_completed", "BOOL", False)
    set_game_property(
        status_observer, "__ln_acceptance_structure_object", "STRING", target.name)
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_modifier",
        "STRING",
        modifier_name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_transient_modifier",
        "STRING",
        transient_modifier_name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_removed_modifier",
        "STRING",
        removed_modifier_name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_input",
        "STRING",
        value_input.identifier,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_output",
        "STRING",
        value_output_socket.identifier,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_saved_value",
        "FLOAT",
        saved_value,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_mutated_value",
        "FLOAT",
        mutated_value,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_internal_node",
        "STRING",
        internal_ico.name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_internal_socket",
        "STRING",
        internal_radius_socket.identifier,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_internal_saved_value",
        "FLOAT",
        saved_internal_radius,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_internal_mutated_value",
        "FLOAT",
        mutated_internal_radius,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_compositor_tree",
        "STRING",
        compositor_tree.name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_compositor_node",
        "STRING",
        compositor_node.name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_compositor_socket",
        "STRING",
        compositor_socket.identifier,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_compositor_saved_value",
        "FLOAT",
        saved_compositor_value,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_compositor_mutated_value",
        "FLOAT",
        mutated_compositor_value,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_structure_transient_compositor_tree",
        "STRING",
        transient_compositor_tree.name,
    )
    target_name = target.name
    main_tree_name = tree.name
    installed_tree_name = installed_tree.name
    value_input_identifier = value_input.identifier
    value_output_identifier = value_output_socket.identifier
    geometry_tree_name = geometry_tree.name
    internal_radius_identifier = internal_radius_socket.identifier
    compositor_tree_name = compositor_tree.name
    compositor_socket_identifier = compositor_socket.identifier
    bind_tree(target, tree.name, True)
    bpy.context.view_layer.update()

    run_game_once(status_observer, "phase6_save_load_structure_0", status_path)
    require(save_file.exists(), f"Save Game did not create {save_path}")
    checkpoint = json.loads(save_file.read_text(encoding="utf-8"))
    saved_target = next(
        (
            item
            for item in checkpoint.get("authored_objects", [])
            if item.get("name") == target_name
        ),
        None,
    )
    require(saved_target is not None,
            "runtime-structure checkpoint is missing the persistent object")
    saved_trees = saved_target.get("logic_trees")
    require(
        isinstance(saved_trees, list)
        and {item.get("name") for item in saved_trees}
        == {main_tree_name, installed_tree_name}
        and all(item.get("enabled") is True for item in saved_trees),
        "runtime-installed Logic Node attachments were not captured exactly",
    )
    saved_modifiers = saved_target.get("modifier_state", {}).get(
        "geometry_nodes", [])
    require(
        len(saved_modifiers) == 2
        and [item.get("name") for item in saved_modifiers]
        == [modifier_name, removed_modifier_name]
        and all(
            item.get("node_group", {}).get("name") == geometry_tree_name
            for item in saved_modifiers
        ),
        "Geometry Nodes modifier structure was not captured exactly",
    )
    saved_input = next(
        (
            item
            for item in saved_modifiers[0].get("inputs", [])
            if item.get("socket") == value_input_identifier
        ),
        None,
    )
    require(
        saved_input is not None
        and saved_input.get("mode") == "value"
        and abs(float(saved_input.get("value", {}).get("value", 0.0)) - saved_value)
        <= 0.0001,
        "Geometry Nodes input value was not captured",
    )
    saved_output = next(
        (
            item
            for item in saved_modifiers[0].get("outputs", [])
            if item.get("socket") == value_output_identifier
        ),
        None,
    )
    require(
        saved_output is not None and saved_output.get("attribute") == "saved_attr",
        "Geometry Nodes output attribute was not captured",
    )
    runtime_node_trees = checkpoint.get("runtime_node_trees", [])
    saved_geometry_tree = next(
        (
            item
            for item in runtime_node_trees
            if item.get("type") == "geometry"
            and item.get("name") == geometry_tree_name
        ),
        None,
    )
    require(
        saved_geometry_tree is not None,
        "Geometry Nodes runtime node values were not captured",
    )
    saved_internal_node = next(
        (
            item
            for item in saved_geometry_tree.get("nodes", [])
            if item.get("type") == "GeometryNodeMeshIcoSphere"
        ),
        None,
    )
    saved_internal_input = next(
        (
            item
            for item in (
                saved_internal_node.get("inputs", [])
                if saved_internal_node is not None
                else []
            )
            if item.get("socket") == internal_radius_identifier
        ),
        None,
    )
    require(
        saved_internal_input is not None
        and abs(float(saved_internal_input.get("value", 0.0)) - saved_internal_radius)
        <= 0.0001,
        "internal Geometry Nodes socket state was not captured",
    )

    saved_compositor = checkpoint.get("compositor_state", {})
    require(
        saved_compositor.get("has_node_tree") is True
        and saved_compositor.get("name") == compositor_tree_name,
        "active compositor root was not captured",
    )
    saved_compositor_tree = next(
        (
            item
            for item in runtime_node_trees
            if item.get("type") == "compositor"
            and item.get("name") == compositor_tree_name
        ),
        None,
    )
    saved_compositor_node = next(
        (
            item
            for item in saved_compositor_tree.get("nodes", [])
            if item.get("type") == "CompositorNodeBrightContrast"
        ),
        None,
    ) if saved_compositor_tree is not None else None
    saved_compositor_input = next(
        (
            item
            for item in (
                saved_compositor_node.get("inputs", [])
                if saved_compositor_node is not None
                else []
            )
            if item.get("socket") == compositor_socket_identifier
        ),
        None,
    )
    require(
        saved_compositor_input is not None
        and abs(
            float(saved_compositor_input.get("value", 0.0))
            - saved_compositor_value
        )
        <= 0.0001,
        "compositor node value state was not captured",
    )


def run_phase6_save_load_material_acceptance(status_path, output_dir):
    reset_scene()
    configure_game_settings(True)

    saved_color = (0.125, 0.375, 0.625, 1.0)
    mutated_color = (0.9, 0.1, 0.2, 0.5)
    saved_roughness = 0.2
    mutated_roughness = 0.85
    saved_material = bpy.data.materials.new(f"{PREFIX}_Phase6SavedMaterial")
    saved_material_name = saved_material.name
    saved_material.use_nodes = True
    saved_principled = next(
        node for node in saved_material.node_tree.nodes
        if node.bl_idname == "ShaderNodeBsdfPrincipled"
    )
    saved_principled_name = saved_principled.name
    saved_principled.inputs["Base Color"].default_value = (0.7, 0.7, 0.7, 1.0)
    mutated_material = bpy.data.materials.new(f"{PREFIX}_Phase6MutatedMaterial")
    mutated_material_name = mutated_material.name
    mutated_material.use_nodes = True

    target = create_cube(f"{PREFIX}_Phase6SaveLoadMaterial")
    target.data.materials.append(saved_material)
    target.game.use_save_game_object_state = True
    sibling = create_cube(f"{PREFIX}_Phase6SaveLoadMaterialSibling")
    unused_sibling_mesh = sibling.data
    sibling.data = target.data
    if unused_sibling_mesh.users == 0:
        bpy.data.meshes.remove(unused_sibling_mesh)
    sibling.material_slots[0].link = "OBJECT"
    sibling.material_slots[0].material = mutated_material

    save_path = str(output_dir / f"{PREFIX}_Phase6SaveLoadMaterial.sav")
    save_file = Path(save_path)
    if save_file.exists():
        save_file.unlink()

    tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadMaterialTree")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    once = tree.nodes.new("LogicNativeOnce")
    set_saved_parameter = tree.nodes.new("LogicNativeSetMaterialParameter")
    set_saved_shared_value = tree.nodes.new("LogicNativeSetEditorNodeValue")
    save_game = tree.nodes.new("LogicNativeSaveGame")
    save_succeeded = tree.nodes.new("LogicNativeBranch")
    set_mutated_parameter = tree.nodes.new("LogicNativeSetMaterialParameter")
    set_mutated_shared_value = tree.nodes.new("LogicNativeSetEditorNodeValue")
    set_mutated_slot = tree.nodes.new("LogicNativeSetMaterialSlot")
    load_game = tree.nodes.new("LogicNativeLoadGame")
    load_succeeded = tree.nodes.new("LogicNativeBranch")
    mark_load_completed = tree.nodes.new("LogicNativeSetGamePropertyBool")

    for node, color in (
        (set_saved_parameter, saved_color),
        (set_mutated_parameter, mutated_color),
    ):
        node.inputs["Object"].default_value = target
        node.inputs["Slot"].default_value = 0
        node.inputs["Node Name"].default_value = saved_principled_name
        node.inputs["Socket"].default_value = "Base Color"
        node.inputs["Color Value"].default_value = color
    for node, roughness in (
        (set_saved_shared_value, saved_roughness),
        (set_mutated_shared_value, mutated_roughness),
    ):
        node.node_editor_type = "SHADER"
        node.target_scope = "OWNER"
        node.value_type = "FLOAT"
        node.inputs["Object"].default_value = target
        node.inputs["Slot"].default_value = 0
        node.inputs["Node Name"].default_value = saved_principled_name
        node.inputs["Socket"].default_value = "Roughness"
        node.inputs["Float Value"].default_value = roughness
    save_game.inputs["Path"].default_value = save_path
    set_mutated_slot.inputs["Object"].default_value = target
    set_mutated_slot.inputs["Material"].default_value = mutated_material
    set_mutated_slot.inputs["Slot"].default_value = 0
    load_game.inputs["Path"].default_value = save_path
    mark_load_completed.inputs["Object"].default_value = target
    mark_load_completed.inputs["Property"].default_value = "load_completed"
    mark_load_completed.inputs["Value"].default_value = True

    tree.links.new(on_update.outputs["Out"], once.inputs["Flow"])
    tree.links.new(once.outputs["Out"], set_saved_parameter.inputs["Flow"])
    tree.links.new(
        set_saved_parameter.outputs["Done"], set_saved_shared_value.inputs["Flow"])
    tree.links.new(set_saved_shared_value.outputs["Done"], save_game.inputs["Flow"])
    tree.links.new(save_game.outputs["Done"], save_succeeded.inputs["Flow"])
    tree.links.new(save_game.outputs["Success"], save_succeeded.inputs["Condition"])
    tree.links.new(save_succeeded.outputs["True"], set_mutated_parameter.inputs["Flow"])
    tree.links.new(
        set_mutated_parameter.outputs["Done"], set_mutated_shared_value.inputs["Flow"])
    tree.links.new(
        set_mutated_shared_value.outputs["Done"], set_mutated_slot.inputs["Flow"])
    tree.links.new(set_mutated_slot.outputs["Done"], load_game.inputs["Flow"])
    tree.links.new(load_game.outputs["Done"], load_succeeded.inputs["Flow"])
    tree.links.new(load_game.outputs["Success"], load_succeeded.inputs["Condition"])
    tree.links.new(load_succeeded.outputs["True"], mark_load_completed.inputs["Flow"])

    status_observer = create_cube(f"{PREFIX}_Phase6SaveLoadMaterialStatus")
    mark_load_completed.inputs["Object"].default_value = status_observer
    add_status_controller(
        status_observer,
        status_path,
        "phase6_save_load_material",
        required_frame=90,
        tolerance=0.001,
    )
    set_game_property(status_observer, "load_completed", "BOOL", False)
    set_game_property(
        status_observer, "__ln_acceptance_material_object", "STRING", target.name)
    set_game_property(
        status_observer, "__ln_acceptance_material_sibling", "STRING", sibling.name)
    set_game_property(
        status_observer,
        "__ln_acceptance_expected_material",
        "STRING",
        saved_material_name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_expected_sibling_material",
        "STRING",
        mutated_material_name,
    )
    set_game_property(
        status_observer, "__ln_acceptance_material_r", "FLOAT", saved_color[0])
    set_game_property(
        status_observer, "__ln_acceptance_material_g", "FLOAT", saved_color[1])
    set_game_property(
        status_observer, "__ln_acceptance_material_b", "FLOAT", saved_color[2])
    set_game_property(
        status_observer, "__ln_acceptance_material_a", "FLOAT", saved_color[3])
    set_game_property(
        status_observer,
        "__ln_acceptance_shared_material_value",
        "FLOAT",
        saved_roughness,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_shared_material_node",
        "STRING",
        saved_principled_name,
    )
    set_game_property(
        status_observer,
        "__ln_acceptance_shared_material_socket",
        "STRING",
        "Roughness",
    )
    bind_tree(target, tree.name, True)

    target_name = target.name
    sibling_name = sibling.name
    status_observer_name = status_observer.name
    run_game_once(status_observer, "phase6_save_load_material_0", status_path)
    require(resolve_live_object(target_name) is not None,
            "Material Save/Load target did not survive the first run")
    require(save_file.exists(), f"Material Save Game did not create {save_path}")
    checkpoint = json.loads(save_file.read_text(encoding="utf-8"))
    require(checkpoint.get("format") == "UPBGE_CXX_LOGIC_NODES_CHECKPOINT",
            "Material checkpoint has the wrong format")
    saved_record = next(
        (record for record in checkpoint.get("authored_objects", [])
         if record.get("name") == target_name),
        None,
    )
    require(saved_record is not None, "Material object is missing from the checkpoint")
    require(saved_record.get("exists") is True
            and isinstance(saved_record.get("transform"), dict)
            and isinstance(saved_record.get("hierarchy"), dict)
            and "physics" in saved_record,
            "Save Object State did not capture the complete material object's structural state")
    material_state = saved_record.get("material_state", {})
    slots = material_state.get("slots", [])
    require(
        slots == [{"name": saved_material_name, "library": ""}],
        f"Material checkpoint stored the wrong slot state: {slots}",
    )
    parameters = material_state.get("parameters", [])
    require(len(parameters) == 1 and str(parameters[0].get("attribute", "")).startswith("_LNMP_"),
            f"Material checkpoint stored the wrong parameter manifest: {parameters}")
    saved_parameter = tuple(float(value) for value in parameters[0].get("value", ()))
    require(
        len(saved_parameter) == 4
        and all(abs(observed - expected) <= 0.0001
                for observed, expected in zip(saved_parameter, saved_color)),
        f"Material checkpoint stored the wrong parameter value: {saved_parameter}",
    )
    shared_material = next(
        (record for record in checkpoint.get("shared_materials", [])
         if record.get("name") == saved_material_name and record.get("library") == ""),
        None,
    )
    require(shared_material is not None,
            "Material checkpoint is missing the shared material snapshot")
    saved_shared_value = None
    for node_record in shared_material.get("nodes", []):
        if node_record.get("type") == saved_principled.bl_idname:
            saved_shared_value = next(
                (input_record.get("value")
                 for input_record in node_record.get("inputs", [])
                 if input_record.get("socket") == "Roughness"
                 and input_record.get("type") == "float"),
                None,
            )
            break
    require(
        saved_shared_value is not None
        and abs(float(saved_shared_value) - saved_roughness) <= 0.0001,
        f"Material checkpoint stored the wrong shared value: {saved_shared_value}",
    )

    # Start a fresh conversion with different authored material state. This proves that disk load,
    # not retained state from the first engine session, restores both the slot and object parameter.
    target = resolve_live_object(target_name)
    mutated_material = bpy.data.materials.get(mutated_material_name)
    require(mutated_material is not None,
            f"Mutated material {mutated_material_name} disappeared after the first run")
    saved_material = bpy.data.materials.get(saved_material_name)
    require(saved_material is not None,
            f"Saved material {saved_material_name} disappeared after the first run")
    for binding in target.game.logic_node_bindings:
        binding.enabled = False
    target.material_slots[0].link = "OBJECT"
    target.material_slots[0].material = saved_material
    parameter_keys = [
        name for name in target.keys() if str(name).startswith("_LNMP_")
    ]
    require(not parameter_keys,
            f"Runtime-only material parameters leaked out of the first game: {parameter_keys}")
    status_observer = resolve_live_object(status_observer_name)
    set_existing_game_property_value(status_observer, "load_completed", False)
    set_existing_game_property_value(
        status_observer, "__ln_acceptance_mode", "phase6_save_load_material_fresh")

    load_tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadMaterialFreshTree")
    fresh_init = load_tree.nodes.new("LogicNativeOnInit")
    fresh_parameter_guard = load_tree.nodes.new("LogicNativeBranch")
    fresh_parameter_declaration = load_tree.nodes.new("LogicNativeSetMaterialParameter")
    fresh_update = load_tree.nodes.new("LogicNativeOnUpdate")
    fresh_once = load_tree.nodes.new("LogicNativeOnce")
    fresh_mutate_shared = load_tree.nodes.new("LogicNativeSetEditorNodeValue")
    fresh_mutate_slot = load_tree.nodes.new("LogicNativeSetMaterialSlot")
    fresh_load = load_tree.nodes.new("LogicNativeLoadGame")
    fresh_load_succeeded = load_tree.nodes.new("LogicNativeBranch")
    fresh_complete = load_tree.nodes.new("LogicNativeSetGamePropertyBool")
    fresh_parameter_guard.inputs["Condition"].default_value = False
    fresh_parameter_declaration.inputs["Object"].default_value = target
    fresh_parameter_declaration.inputs["Slot"].default_value = 0
    fresh_parameter_declaration.inputs["Node Name"].default_value = saved_principled_name
    fresh_parameter_declaration.inputs["Socket"].default_value = "Base Color"
    fresh_parameter_declaration.inputs["Color Value"].default_value = mutated_color
    fresh_mutate_shared.node_editor_type = "SHADER"
    fresh_mutate_shared.target_scope = "OWNER"
    fresh_mutate_shared.value_type = "FLOAT"
    fresh_mutate_shared.inputs["Object"].default_value = target
    fresh_mutate_shared.inputs["Slot"].default_value = 0
    fresh_mutate_shared.inputs["Node Name"].default_value = saved_principled_name
    fresh_mutate_shared.inputs["Socket"].default_value = "Roughness"
    fresh_mutate_shared.inputs["Float Value"].default_value = mutated_roughness
    fresh_mutate_slot.inputs["Object"].default_value = target
    fresh_mutate_slot.inputs["Material"].default_value = mutated_material
    fresh_mutate_slot.inputs["Slot"].default_value = 0
    fresh_load.inputs["Path"].default_value = save_path
    fresh_complete.inputs["Object"].default_value = status_observer
    fresh_complete.inputs["Property"].default_value = "load_completed"
    fresh_complete.inputs["Value"].default_value = True
    load_tree.links.new(fresh_init.outputs["Out"], fresh_parameter_guard.inputs["Flow"])
    load_tree.links.new(
        fresh_parameter_guard.outputs["True"], fresh_parameter_declaration.inputs["Flow"])
    load_tree.links.new(fresh_update.outputs["Out"], fresh_once.inputs["Flow"])
    load_tree.links.new(fresh_once.outputs["Out"], fresh_mutate_shared.inputs["Flow"])
    load_tree.links.new(
        fresh_mutate_shared.outputs["Done"], fresh_mutate_slot.inputs["Flow"])
    load_tree.links.new(fresh_mutate_slot.outputs["Done"], fresh_load.inputs["Flow"])
    load_tree.links.new(fresh_load.outputs["Done"], fresh_load_succeeded.inputs["Flow"])
    load_tree.links.new(
        fresh_load.outputs["Success"], fresh_load_succeeded.inputs["Condition"])
    load_tree.links.new(fresh_load_succeeded.outputs["True"], fresh_complete.inputs["Flow"])
    bind_tree(target, load_tree.name, True)
    run_game_once(status_observer, "phase6_save_load_material_fresh_1", status_path)

    sibling = resolve_live_object(sibling_name)
    require(
        sibling.material_slots[0].material is not None
        and sibling.material_slots[0].material.name == mutated_material_name,
            "Material load changed the sibling object sharing the same mesh")


def run_phase6_save_load_existence_acceptance(status_path, output_dir):
    reset_scene()
    configure_game_settings(True)

    owner = create_cube(f"{PREFIX}_Phase6ExistenceOwner")
    authored_present = create_cube(f"{PREFIX}_Phase6AuthoredPresent")
    authored_present.location = (3.0, 4.0, 5.0)
    authored_present.game.use_save_game_object_state = True
    set_game_property(
        authored_present, "__ln_acceptance_existence_role", "STRING", "authored_present")
    # Rebuilt from its authored recipe on load: a Vector and a saved List come back too.
    set_game_property(authored_present, "spot", "VECTOR", (1.0, 2.0, 3.0))
    set_active_object(authored_present)
    with bpy.context.temp_override(object=authored_present, active_object=authored_present):
        bpy.ops.object.game_property_new(type='LIST', name="bag")
    bag = authored_present.game.properties["bag"]
    bag.item_type = 'INT'
    for value in (3, 5):
        with bpy.context.temp_override(object=authored_present, active_object=authored_present):
            bpy.ops.object.game_property_item_add(
                index=list(authored_present.game.properties).index(bag))
        bag.elements[-1].value = value
    bag.use_save_game = True

    authored_tombstone = create_cube(f"{PREFIX}_Phase6AuthoredTombstone")
    authored_tombstone.location = (-4.0, 2.0, 1.0)
    authored_tombstone.game.use_save_game_object_state = True
    set_game_property(
        authored_tombstone, "__ln_acceptance_existence_role", "STRING", "authored_tombstone")

    replica_template = create_cube(f"{PREFIX}_Phase6PersistentTemplate")
    set_game_property(
        replica_template, "__ln_acceptance_existence_role", "STRING", "persistent_spawn")
    replica_template.hide_viewport = True
    transient_template = create_cube(f"{PREFIX}_Phase6TransientTemplate")
    set_game_property(
        transient_template, "__ln_acceptance_existence_role", "STRING", "transient_spawn")
    transient_template.hide_viewport = True
    spawn_anchor = create_cube(f"{PREFIX}_Phase6PersistentSpawnAnchor")
    spawn_anchor.location = (7.0, 0.0, 1.0)
    bpy.context.view_layer.update()
    require(not replica_template.game.use_save_game_object_state,
            "The persistent-spawn source must start with Save Object State disabled")

    save_path = str(output_dir / f"{PREFIX}_Phase6SaveLoadExistence.sav")
    save_file = Path(save_path)
    if save_file.exists():
        save_file.unlink()

    tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadExistenceTree")
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    once = tree.nodes.new("LogicNativeOnce")
    add_object = tree.nodes.new("LogicNativeAddObject")
    promote = tree.nodes.new("LogicNativeSetSaveExistence")
    promote_succeeded = tree.nodes.new("LogicNativeBranch")
    set_spawn_position = new_set_object_attribute(tree, "WORLD_POSITION")
    remove_tombstone = tree.nodes.new("LogicNativeRemoveObject")
    save_game = tree.nodes.new("LogicNativeSaveGame")
    save_succeeded = tree.nodes.new("LogicNativeBranch")
    add_transient = tree.nodes.new("LogicNativeAddObject")
    remove_present = tree.nodes.new("LogicNativeRemoveObject")
    remove_spawn = tree.nodes.new("LogicNativeRemoveObject")
    load_game = tree.nodes.new("LogicNativeLoadGame")
    load_succeeded = tree.nodes.new("LogicNativeBranch")
    promote_transient_after_load = tree.nodes.new("LogicNativeSetSaveExistence")
    transient_promotion_succeeded = tree.nodes.new("LogicNativeBranch")
    mark_load_completed = tree.nodes.new("LogicNativeSetGamePropertyBool")

    add_object.inputs["Object to Add"].default_value = replica_template
    add_object.inputs["Copy Transform"].default_value = spawn_anchor
    add_object.inputs["Life"].default_value = 0.0
    add_object.inputs["Full Copy"].default_value = False
    promote.inputs["Enabled"].default_value = True
    set_spawn_position.inputs["Value"].default_value = (11.0, 12.0, 13.0)
    remove_tombstone.inputs["Object"].default_value = authored_tombstone
    save_game.inputs["Path"].default_value = save_path
    add_transient.inputs["Object to Add"].default_value = transient_template
    add_transient.inputs["Copy Transform"].default_value = spawn_anchor
    add_transient.inputs["Life"].default_value = 0.0
    add_transient.inputs["Full Copy"].default_value = False
    remove_present.inputs["Object"].default_value = authored_present
    load_game.inputs["Path"].default_value = save_path
    promote_transient_after_load.inputs["Enabled"].default_value = True
    mark_load_completed.inputs["Object"].default_value = owner
    mark_load_completed.inputs["Property"].default_value = "load_completed"
    mark_load_completed.inputs["Value"].default_value = True

    tree.links.new(on_fixed_update.outputs["Out"], once.inputs["Flow"])
    tree.links.new(once.outputs["Out"], add_object.inputs["Flow"])
    tree.links.new(add_object.outputs["Done"], promote.inputs["Flow"])
    tree.links.new(add_object.outputs["Added Object"], promote.inputs["Object"])
    tree.links.new(promote.outputs["Done"], promote_succeeded.inputs["Flow"])
    tree.links.new(promote.outputs["Success"], promote_succeeded.inputs["Condition"])
    tree.links.new(promote_succeeded.outputs["True"], flow_input(set_spawn_position))
    tree.links.new(add_object.outputs["Added Object"], set_spawn_position.inputs["Object"])
    tree.links.new(set_spawn_position.outputs["Done"], flow_input(remove_tombstone))
    tree.links.new(remove_tombstone.outputs["Done"], flow_input(save_game))
    tree.links.new(save_game.outputs["Done"], save_succeeded.inputs["Flow"])
    tree.links.new(save_game.outputs["Success"], save_succeeded.inputs["Condition"])
    tree.links.new(save_succeeded.outputs["True"], add_transient.inputs["Flow"])
    tree.links.new(add_transient.outputs["Done"], flow_input(remove_present))
    tree.links.new(remove_present.outputs["Done"], flow_input(remove_spawn))
    tree.links.new(add_object.outputs["Added Object"], remove_spawn.inputs["Object"])
    tree.links.new(remove_spawn.outputs["Done"], flow_input(load_game))
    tree.links.new(load_game.outputs["Done"], load_succeeded.inputs["Flow"])
    tree.links.new(load_game.outputs["Success"], load_succeeded.inputs["Condition"])
    tree.links.new(
        load_succeeded.outputs["True"], promote_transient_after_load.inputs["Flow"])
    tree.links.new(
        add_transient.outputs["Added Object"], promote_transient_after_load.inputs["Object"])
    tree.links.new(
        promote_transient_after_load.outputs["Done"], transient_promotion_succeeded.inputs["Flow"])
    tree.links.new(
        promote_transient_after_load.outputs["Success"],
        transient_promotion_succeeded.inputs["Condition"])
    tree.links.new(
        transient_promotion_succeeded.outputs["True"], flow_input(mark_load_completed))

    add_status_controller(
        owner, status_path, "phase6_save_load_existence", required_frame=60, tolerance=0.001)
    set_game_property(owner, "load_completed", "BOOL", False)
    bind_tree(owner, tree.name, True)

    owner_name = owner.name
    present_name = authored_present.name
    tombstone_name = authored_tombstone.name
    template_name = replica_template.name
    transient_template_name = transient_template.name
    run_game_once(owner, "phase6_save_load_existence_0", status_path)

    require(save_file.exists(), f"Save Game did not create {save_path}")
    checkpoint = json.loads(save_file.read_text(encoding="utf-8"))
    require(checkpoint.get("format") == "UPBGE_CXX_LOGIC_NODES_CHECKPOINT",
            "Existence checkpoint has the wrong format")
    authored_records = checkpoint.get("authored_objects", [])
    spawned_records = checkpoint.get("spawned_instances", [])
    saved_present = next(
        (record for record in authored_records if record.get("name") == present_name), None)
    saved_tombstone = next(
        (record for record in authored_records if record.get("name") == tombstone_name), None)
    require(saved_present is not None and saved_present.get("exists") is True,
            "The live authored existence record was not captured")
    require(saved_tombstone is not None and saved_tombstone.get("exists") is False,
            "The removed authored object was not captured as an explicit tombstone")
    require("transform" not in saved_tombstone and saved_tombstone.get("properties") == [],
            "An authored tombstone contains object state")
    saved_present_position = tuple(
        float(value) for value in saved_present.get("transform", {}).get("position", ()))
    require(saved_present_position == (3.0, 4.0, 5.0),
            "The live authored record has the wrong saved transform")
    require(len(spawned_records) == 1,
            "The promoted Add Object root was not captured as one persistent instance")
    saved_spawn = spawned_records[0]
    require(saved_spawn.get("name") == template_name,
            "The persistent instance has the wrong diagnostic source name")
    instance_uuid = saved_spawn.get("instance_uuid", "")
    source_uuid = saved_spawn.get("source_uuid", "")
    require(len(instance_uuid) == 36 and len(source_uuid) == 36 and instance_uuid != source_uuid,
            "The persistent instance is missing distinct durable instance/source UUIDs")
    creation_position = tuple(
        float(value) for value in saved_spawn.get("creation_transform", {}).get("position", ()))
    saved_spawn_position = tuple(
        float(value) for value in saved_spawn.get("transform", {}).get("position", ()))
    require(creation_position == (7.0, 0.0, 1.0),
            "The persistent instance has the wrong creation transform")
    require(saved_spawn_position == (11.0, 12.0, 13.0),
            "The persistent instance has the wrong saved transform")

    # A second engine start exercises authored tombstone reconciliation and persistent replica
    # reconstruction from a fresh conversion, without changing the source's authored existence flag.
    owner = resolve_live_object(owner_name)
    for binding in owner.game.logic_node_bindings:
        binding.enabled = False
    authored_present = resolve_live_object(present_name)
    transient_template = resolve_live_object(transient_template_name)
    authored_present.location = (30.0, 40.0, 50.0)
    set_existing_game_property_value(owner, "load_completed", False)

    load_tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadExistenceFreshTree")
    on_fixed_update = load_tree.nodes.new("LogicNativeOnFixedUpdate")
    once = load_tree.nodes.new("LogicNativeOnce")
    add_transient = load_tree.nodes.new("LogicNativeAddObject")
    load_game = load_tree.nodes.new("LogicNativeLoadGame")
    load_succeeded = load_tree.nodes.new("LogicNativeBranch")
    promote_transient_after_load = load_tree.nodes.new("LogicNativeSetSaveExistence")
    transient_promotion_succeeded = load_tree.nodes.new("LogicNativeBranch")
    mark_load_completed = load_tree.nodes.new("LogicNativeSetGamePropertyBool")
    add_transient.inputs["Object to Add"].default_value = transient_template
    add_transient.inputs["Copy Transform"].default_value = owner
    add_transient.inputs["Life"].default_value = 0.0
    add_transient.inputs["Full Copy"].default_value = False
    load_game.inputs["Path"].default_value = save_path
    promote_transient_after_load.inputs["Enabled"].default_value = True
    mark_load_completed.inputs["Object"].default_value = owner
    mark_load_completed.inputs["Property"].default_value = "load_completed"
    mark_load_completed.inputs["Value"].default_value = True
    load_tree.links.new(on_fixed_update.outputs["Out"], once.inputs["Flow"])
    load_tree.links.new(once.outputs["Out"], add_transient.inputs["Flow"])
    load_tree.links.new(add_transient.outputs["Done"], load_game.inputs["Flow"])
    load_tree.links.new(load_game.outputs["Done"], load_succeeded.inputs["Flow"])
    load_tree.links.new(load_game.outputs["Success"], load_succeeded.inputs["Condition"])
    load_tree.links.new(
        load_succeeded.outputs["True"], promote_transient_after_load.inputs["Flow"])
    load_tree.links.new(
        add_transient.outputs["Added Object"], promote_transient_after_load.inputs["Object"])
    load_tree.links.new(
        promote_transient_after_load.outputs["Done"], transient_promotion_succeeded.inputs["Flow"])
    load_tree.links.new(
        promote_transient_after_load.outputs["Success"],
        transient_promotion_succeeded.inputs["Condition"])
    load_tree.links.new(
        transient_promotion_succeeded.outputs["True"], flow_input(mark_load_completed))
    bind_tree(owner, load_tree.name, True)
    run_game_once(owner_name, "phase6_save_load_existence_fresh_1", status_path)

    replica_template = resolve_live_object(template_name)
    require(not replica_template.game.use_save_game_object_state,
            "Runtime promotion or Load changed the authored source Save Object State flag")
    transient_template = resolve_live_object(transient_template_name)
    require(not transient_template.game.use_save_game_object_state,
            "Runtime transient promotion changed its authored source Save Object State flag")


def run_phase6_align_axis_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6AlignAxis")
    tree = build_align_axis_tree(f"{PREFIX}_Phase6AlignAxisTree", target)
    add_status_controller(target, status_path, "phase6_align_axis", required_frame=20)
    set_game_property(target, "__ln_acceptance_min_dot", "FLOAT", 0.85)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_align_axis_0", status_path)


def run_phase6_slow_follow_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6SlowFollowTarget")
    target.location = (3.0, 0.0, 0.0)
    follower = create_cube(f"{PREFIX}_Phase6SlowFollower")
    tree = build_slow_follow_tree(
        f"{PREFIX}_Phase6SlowFollowTree",
        target,
        follower,
    )
    add_status_controller(follower, status_path, "phase6_slow_follow", required_frame=12)
    set_game_property(follower, "__ln_acceptance_min_x", "FLOAT", 0.5)
    bind_tree(follower, tree.name, True)
    run_game_once(follower, "phase6_slow_follow_0", status_path)


def run_phase6_replace_mesh_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6ReplaceMeshTarget")
    mesh_source = create_uv_sphere(f"{PREFIX}_Phase6ReplaceMeshSource", (4.0, 0.0, 0.0))
    tree = build_replace_mesh_tree(
        f"{PREFIX}_Phase6ReplaceMeshTree",
        mesh_source,
        target,
    )
    add_status_controller(target, status_path, "phase6_replace_mesh", required_frame=2)
    set_game_property(target, "mesh_replaced", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_replace_mesh_0", status_path)


def build_input_nodes_tree(name):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")
    keyboard = tree.nodes.new("LogicNativeKeyboardKey")
    mouse = tree.nodes.new("LogicNativeMouseButton")
    gamepad = tree.nodes.new("LogicNativeGamepadActive")
    branch_keyboard = tree.nodes.new("LogicNativeBranch")
    branch_mouse = tree.nodes.new("LogicNativeBranch")
    branch_gamepad = tree.nodes.new("LogicNativeBranch")
    set_keyboard = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_mouse = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_gamepad = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_keyboard_active = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_mouse_active = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_gamepad_active = tree.nodes.new("LogicNativeSetGamePropertyBool")

    keyboard.inputs["Key"].default_value = "SPACE"
    mouse.inputs["Button"].default_value = "LEFTMOUSE"
    gamepad.inputs["Index"].default_value = 0
    for node, property_name in (
        (set_keyboard, "keyboard_checked"),
        (set_mouse, "mouse_checked"),
        (set_gamepad, "gamepad_checked"),
        (set_keyboard_active, "keyboard_checked"),
        (set_mouse_active, "mouse_checked"),
        (set_gamepad_active, "gamepad_checked"),
    ):
        node.inputs["Property"].default_value = property_name
        node.inputs["Value"].default_value = True

    tree.links.new(on_fixed_update.outputs["Out"], branch_keyboard.inputs["Flow"])
    tree.links.new(on_fixed_update.outputs["Out"], branch_mouse.inputs["Flow"])
    tree.links.new(on_fixed_update.outputs["Out"], branch_gamepad.inputs["Flow"])
    tree.links.new(keyboard.outputs["Active"], branch_keyboard.inputs["Condition"])
    tree.links.new(mouse.outputs["Active"], branch_mouse.inputs["Condition"])
    tree.links.new(gamepad.outputs["Active"], branch_gamepad.inputs["Condition"])
    tree.links.new(branch_keyboard.outputs["True"], flow_input(set_keyboard_active))
    tree.links.new(branch_mouse.outputs["True"], flow_input(set_mouse_active))
    tree.links.new(branch_gamepad.outputs["True"], flow_input(set_gamepad_active))
    tree.links.new(branch_keyboard.outputs["False"], flow_input(set_keyboard))
    tree.links.new(branch_mouse.outputs["False"], flow_input(set_mouse))
    tree.links.new(branch_gamepad.outputs["False"], flow_input(set_gamepad))
    return tree


def build_values_math_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    value_a = tree.nodes.new("LogicNativeValueFloat")
    value_b = tree.nodes.new("LogicNativeValueFloat")
    math_node = tree.nodes.new("LogicNativeMath")
    set_result = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    float_curve = tree.nodes.new("LogicNativeFloatCurve")
    set_curve_result = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    value_output(value_a).default_value = 2.0
    value_output(value_b).default_value = 3.0
    math_node.operation = "ADD"
    set_result.inputs["Property"].default_value = "math_result"
    float_curve.inputs["Factor"].default_value = 0.5
    float_curve.inputs["Value"].default_value = 0.25
    float_curve.mapping.curves[0].points[0].location = (0.0, 0.75)
    float_curve.mapping.curves[0].points[1].location = (1.0, 0.75)
    float_curve.mapping.update()
    set_curve_result.inputs["Property"].default_value = "float_curve_result"

    tree.links.new(on_init.outputs["Out"], flow_input(set_result))
    tree.links.new(on_init.outputs["Out"], flow_input(set_curve_result))
    tree.links.new(value_output(value_a), math_node.inputs["A"])
    tree.links.new(value_output(value_b), math_node.inputs["B"])
    tree.links.new(math_node.outputs["Result"], set_result.inputs["Value"])
    tree.links.new(float_curve.outputs["Value"], set_curve_result.inputs["Value"])
    return tree


def build_c_tier_values_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")

    combine = tree.nodes.new("LogicNativeCombineXYZW")
    combine.inputs["X"].default_value = 1.5
    combine.inputs["Y"].default_value = 2.5
    combine.inputs["Z"].default_value = 3.5
    combine.inputs["W"].default_value = 4.5

    resize = tree.nodes.new("LogicNativeResizeVector")
    resize.to_size = "VECTOR_4D"

    vector4_string = tree.nodes.new("LogicNativeTypecast")
    resize_string = tree.nodes.new("LogicNativeTypecast")
    matrix_string = tree.nodes.new("LogicNativeTypecast")
    for typecast in (vector4_string, resize_string, matrix_string):
        typecast.to_type = "STRING"
    set_vector4_text = tree.nodes.new("LogicNativeSetGamePropertyString")
    set_resize4_text = tree.nodes.new("LogicNativeSetGamePropertyString")
    set_matrix_text = tree.nodes.new("LogicNativeSetGamePropertyString")
    set_vector4_text.inputs["Property"].default_value = "vector4_text"
    set_resize4_text.inputs["Property"].default_value = "resize4_text"
    set_matrix_text.inputs["Property"].default_value = "matrix_euler_text"

    xyz_to_matrix = tree.nodes.new("LogicNativeXYZToMatrix")
    matrix_to_xyz = tree.nodes.new("LogicNativeMatrixToXYZ")

    xyz_to_matrix.inputs["XYZ"].default_value = (0.25, -0.5, 0.75)
    matrix_to_xyz.output = "EULER"
    matrix_to_xyz.euler_order = "XYZ"

    for setter in (set_vector4_text, set_resize4_text, set_matrix_text):
        tree.links.new(on_init.outputs["Out"], flow_input(setter))

    tree.links.new(combine.outputs["Vector"], socket_by_identifier(vector4_string.inputs, "Value"))
    tree.links.new(combine.outputs["Vector"], resize.inputs["Vector"])
    tree.links.new(resize.outputs["Vector"], socket_by_identifier(resize_string.inputs, "Value"))
    tree.links.new(socket_by_identifier(vector4_string.outputs, "String"),
                   set_vector4_text.inputs["Value"])
    tree.links.new(socket_by_identifier(resize_string.outputs, "String"),
                   set_resize4_text.inputs["Value"])

    tree.links.new(xyz_to_matrix.outputs["Matrix"], matrix_to_xyz.inputs["Matrix"])
    tree.links.new(matrix_to_xyz.outputs["Vector"], socket_by_identifier(matrix_string.inputs, "Value"))
    tree.links.new(socket_by_identifier(matrix_string.outputs, "String"),
                   set_matrix_text.inputs["Value"])
    return tree


def build_b_tier_core_tree(name, target_obj):
    tree = new_logic_tree(name)
    on_fixed_update = tree.nodes.new("LogicNativeOnFixedUpdate")

    formula = tree.nodes.new("LogicNativeFormula")
    formula.predefined_formulas = "USER_DEFINED"
    formula.formula = "abs(a) + pi + e"
    formula.inputs["a"].default_value = -2.0
    set_formula = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_formula.inputs["Property"].default_value = "formula_result"

    evaluate = tree.nodes.new("LogicNativeEvaluateProperty")
    evaluate.mode = "GAME_PROPERTY"
    evaluate.operation = "EQUAL"
    evaluate.inputs["Object"].default_value = target_obj
    evaluate.inputs["Property"].default_value = "typed_source"
    compare_value = tree.nodes.new("LogicNativeValueFloat")
    value_output(compare_value).default_value = 12.5
    set_eval_ok = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_eval_ok.inputs["Property"].default_value = "eval_ok"
    set_eval_ok.inputs["Value"].default_value = True
    eval_branch = tree.nodes.new("LogicNativeBranch")

    tween = tree.nodes.new("LogicNativeTweenValue")
    tween.on_demand = True
    tween.instant_reset = True
    socket_by_identifier(tween.inputs, "FromFloat").default_value = 0.0
    socket_by_identifier(tween.inputs, "ToFloat").default_value = 1.0
    tween.inputs["Duration"].default_value = 0.25
    set_tween = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_tween.inputs["Property"].default_value = "tween_result"

    projectile = tree.nodes.new("LogicNativeProjectileRay")
    projectile.inputs["Caster"].default_value = target_obj
    projectile.inputs["Origin"].default_value = (0.0, 0.0, 0.0)
    projectile.inputs["Aim"].default_value = (0.0, 1.0, 0.0)
    projectile.inputs["Local"].default_value = True
    projectile.inputs["Power"].default_value = 10.0
    projectile.inputs["Distance"].default_value = 10.0
    projectile.inputs["Resolution"].default_value = 0.9
    projectile.inputs["Mask"].default_value = 1023
    set_projectile_hit = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_projectile_hit.inputs["Property"].default_value = "projectile_hit"

    for setter in (set_formula, set_tween, set_projectile_hit):
        tree.links.new(on_fixed_update.outputs["Out"], flow_input(setter))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(projectile))
    tree.links.new(on_fixed_update.outputs["Out"], flow_input(eval_branch))

    tree.links.new(formula.outputs["Result"], set_formula.inputs["Value"])
    tree.links.new(value_output(compare_value), evaluate.inputs["Value"])
    tree.links.new(evaluate.outputs["If True"], eval_branch.inputs["Condition"])
    tree.links.new(eval_branch.outputs["True"], flow_input(set_eval_ok))
    tree.links.new(socket_by_identifier(tween.outputs, "ResultFloat"), set_tween.inputs["Value"])
    tree.links.new(projectile.outputs["Has Result"], set_projectile_hit.inputs["Value"])
    return tree


def build_sound_state_tree(name):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    stop_all = tree.nodes.new("LogicNativeStopAllSounds")
    set_done = tree.nodes.new("LogicNativeSetGamePropertyBool")

    set_done.inputs["Property"].default_value = "sound_stopped"
    set_done.inputs["Value"].default_value = True

    tree.links.new(on_init.outputs["Out"], flow_input(stop_all))
    tree.links.new(stop_all.outputs["Done"], flow_input(set_done))
    return tree


def load_acceptance_test_sound():
    sound_path = Path(__file__).resolve().parent.parent / "files" / "sound" / "pink_panther.ogg"
    if not sound_path.is_file():
        raise FileNotFoundError(f"Missing acceptance test sound: {sound_path}")
    return bpy.data.sounds.load(str(sound_path))


def build_sound_advanced_tree(name):
    tree = new_logic_tree(name)
    sound = load_acceptance_test_sound()
    on_init = tree.nodes.new("LogicNativeOnInit")
    play_3d = tree.nodes.new("LogicNativePlaySound3D")
    pause = tree.nodes.new("LogicNativePauseSound")
    resume = tree.nodes.new("LogicNativeResumeSound")
    set_done = tree.nodes.new("LogicNativeSetGamePropertyBool")

    play_3d.sound = sound
    pause.sound = sound
    resume.sound = sound
    set_done.inputs["Property"].default_value = "sound_advanced_ok"
    set_done.inputs["Value"].default_value = True

    tree.links.new(on_init.outputs["Out"], flow_input(play_3d))
    tree.links.new(play_3d.outputs["Done"], flow_input(pause))
    tree.links.new(pause.outputs["Done"], flow_input(resume))
    tree.links.new(resume.outputs["Done"], flow_input(set_done))
    return tree


def build_vehicle_control_compile_tree(name):
    """On-init smoke; vehicle nodes are covered by catalog parity and C++ registry gtests."""
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_ok = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_ok.inputs["Property"].default_value = "vehicle_nodes_ok"
    set_ok.inputs["Value"].default_value = True
    tree.links.new(on_init.outputs["Out"], flow_input(set_ok))
    return tree


def run_input_nodes_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_InputNodes")
    tree = build_input_nodes_tree(f"{PREFIX}_InputNodesTree")
    add_status_controller(target, status_path, "input_nodes", required_frame=2)
    for property_name in ("keyboard_checked", "mouse_checked", "gamepad_checked"):
        set_game_property(target, property_name, "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "input_nodes_0", status_path)


def run_values_math_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_ValuesMath")
    tree = build_values_math_tree(f"{PREFIX}_ValuesMathTree")
    add_status_controller(target, status_path, "values_math", required_frame=2)
    set_game_property(target, "math_result", "FLOAT", 0.0)
    set_game_property(target, "__ln_acceptance_expected_math", "FLOAT", 5.0)
    set_game_property(target, "float_curve_result", "FLOAT", 0.0)
    set_game_property(target, "__ln_acceptance_expected_float_curve", "FLOAT", 0.5)
    bind_tree(target, tree.name, True)
    run_game_once(target, "values_math_0", status_path)


def run_c_tier_values_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_CTierValues")
    tree = build_c_tier_values_tree(f"{PREFIX}_CTierValuesTree")
    add_status_controller(target, status_path, "c_tier_values", required_frame=2, tolerance=0.01)
    set_game_property(target, "vector4_text", "STRING", "pending")
    set_game_property(target, "resize4_text", "STRING", "pending")
    set_game_property(target, "matrix_euler_text", "STRING", "pending")
    bind_tree(target, tree.name, True)
    run_game_once(target, "c_tier_values_0", status_path)


def run_b_tier_core_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_BTierCore")
    target.rotation_euler = (0.0, 0.0, 0.0)
    blocker = create_cube(f"{PREFIX}_BTierProjectileBlocker")
    blocker.location = (0.0, 5.0, -1.25)
    blocker.scale = (1.0, 1.0, 1.0)
    blocker.game.physics_type = "STATIC"
    blocker.game.use_collision_bounds = True
    blocker.game.collision_bounds_type = "BOX"

    tree = build_b_tier_core_tree(f"{PREFIX}_BTierCoreTree", target)
    add_status_controller(target, status_path, "b_tier_core", required_frame=6, tolerance=0.001)
    set_game_property(target, "typed_source", "FLOAT", 12.5)
    set_game_property(target, "formula_result", "FLOAT", 0.0)
    set_game_property(target, "eval_ok", "BOOL", False)
    set_game_property(target, "tween_result", "FLOAT", 0.0)
    set_game_property(target, "projectile_hit", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "b_tier_core_0", status_path)


def run_sound_state_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_SoundState")
    tree = build_sound_state_tree(f"{PREFIX}_SoundStateTree")
    add_status_controller(target, status_path, "sound_state", required_frame=2)
    set_game_property(target, "sound_stopped", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "sound_state_0", status_path)


def run_sound_advanced_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_SoundAdvanced")
    tree = build_sound_advanced_tree(f"{PREFIX}_SoundAdvancedTree")
    add_status_controller(target, status_path, "sound_advanced", required_frame=2)
    set_game_property(target, "sound_advanced_ok", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "sound_advanced_0", status_path)


def run_vehicle_control_compile_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_VehicleControlCompile")
    tree = build_vehicle_control_compile_tree(f"{PREFIX}_VehicleControlCompileTree")
    add_status_controller(target, status_path, "vehicle_nodes_ok", required_frame=2)
    set_game_property(target, "vehicle_nodes_ok", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "vehicle_control_compile_0", status_path)


def run_mouse_over_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_MouseOverTarget")
    probe = create_cube(f"{PREFIX}_MouseOverProbe")
    probe.location = (4.0, 0.0, 0.0)
    create_camera_looking_at(f"{PREFIX}_MouseOverCamera", (0.0, -6.0, 0.0), target.location)
    # Keep the pointer deterministic in an embedded View3D runner. A one-shot initialization
    # warp can be superseded by a queued host-window mouse event before the next fixed step.
    tree = build_mouse_over_tree(
        f"{PREFIX}_MouseOverTree", target, keep_cursor_centered=True)

    add_status_controller(probe, status_path, "mouse_over", required_frame=3, tolerance=0.05)
    set_game_property(probe, "mouse_enter", "BOOL", False)
    set_game_property(probe, "mouse_over", "BOOL", False)
    set_game_property(probe, "mouse_exit", "BOOL", False)
    set_game_property(probe, "mouse_over_count", "FLOAT", 0.0)
    set_game_property(probe, "__ln_acceptance_mouse_hit_object", "STRING", target.name)
    set_game_property(target, "mouse_over_object_output", "BOOL", False)
    for property_name in (
        "mouse_point_x",
        "mouse_point_y",
        "mouse_point_z",
        "mouse_normal_x",
        "mouse_normal_y",
        "mouse_normal_z",
    ):
        set_game_property(probe, property_name, "FLOAT", 0.0)

    bind_tree(probe, tree.name, True)
    run_game_once(probe, "mouse_over_0", status_path)


def run_mouse_over_cadence_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    game_settings = bpy.context.scene.game_settings
    game_settings.physics_tick_rate = 120
    game_settings.use_fixed_fps_cap = True
    game_settings.fixed_render_cap_rate = 20

    target = create_cube(f"{PREFIX}_MouseOverCadenceTarget")
    target.scale = (100.0, 1.0, 100.0)
    probe = create_cube(f"{PREFIX}_MouseOverCadenceProbe")
    probe.location = (4.0, 0.0, 0.0)
    create_camera_looking_at(
        f"{PREFIX}_MouseOverCadenceCamera", (0.0, -6.0, 0.0), target.location)
    tree = build_mouse_over_tree(
        f"{PREFIX}_MouseOverCadenceTree", target, keep_cursor_centered=True)

    add_status_controller(probe,
                          status_path,
                          "mouse_over_cadence",
                          required_frame=1,
                          tolerance=0.05,
                          required_seconds=0.5)
    set_game_property(probe, "mouse_enter", "BOOL", False)
    set_game_property(probe, "mouse_over", "BOOL", False)
    set_game_property(probe, "mouse_exit", "BOOL", False)
    set_game_property(probe, "mouse_over_count", "FLOAT", 0.0)
    set_game_property(probe, "__ln_acceptance_mouse_hit_object", "STRING", target.name)
    set_game_property(target, "mouse_over_object_output", "BOOL", False)
    set_game_property(probe, "__ln_acceptance_min_mouse_over_count", "FLOAT", 25.0)

    bind_tree(probe, tree.name, True)
    run_game_once(probe, "mouse_over_cadence_0", status_path)


def run_material_shader_node_input_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_MaterialShaderInput")
    material = create_mix_factor_material(f"{PREFIX}_MaterialShaderInputMaterial")
    material_name = material.name
    target.data.materials.append(material)
    tree = build_material_shader_input_tree(f"{PREFIX}_MaterialShaderInputTree")

    add_status_controller(target, status_path, "material_shader_input", required_frame=2)
    set_game_property(target, "__ln_acceptance_material", "STRING", material_name)
    bind_tree(target, tree.name, True)
    run_game_once(target, "material_shader_node_input_0", status_path)


def run_disabled_binding_probe(status_path):
    reset_scene()
    configure_game_settings(True)
    tree = build_move_tree(f"{PREFIX}_DisabledTree", (9.0, 0.0, 0.0))
    target = create_cube(f"{PREFIX}_Disabled")
    add_status_controller(target, status_path, "unchanged_position")
    set_expected_vector(target, (0.0, 0.0, 0.0))
    bind_tree(target, tree.name, False)
    run_game_once(target, "disabled_tree", status_path)


def run_missing_tree_probe(status_path):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_MissingTree")
    add_status_controller(target, status_path, "unchanged_position")
    set_expected_vector(target, (0.0, 0.0, 0.0))
    bind_tree(target, f"{PREFIX}_DoesNotExist", True)
    run_game_once(target, "missing_tree", status_path)


def run_variable_timestep_probe(status_path):
    reset_scene()
    configure_game_settings(False)
    tree = build_move_tree(f"{PREFIX}_VariableSkipTree", (8.0, 0.0, 0.0), "INIT")
    target = create_cube(f"{PREFIX}_VariableSkip")
    add_status_controller(target, status_path, "unchanged_position")
    set_expected_vector(target, (0.0, 0.0, 0.0))
    bind_tree(target, tree.name, True)
    run_game_once(target, "variable_timestep_skip", status_path)


def run_binding_operator_smoke():
    reset_scene()
    configure_game_settings(True)
    tree = build_move_tree(f"{PREFIX}_OperatorTree", (1.0, 1.0, 1.0))
    target = create_cube(f"{PREFIX}_OperatorObject")
    set_active_object(target)
    result = bpy.ops.object.logic_nodes_binding_add()
    require(result == {"FINISHED"}, f"Binding add operator failed: {result}")
    require(len(target.game.logic_node_bindings) == 1, "Binding add did not create a binding entry")
    binding = target.game.logic_node_bindings[0]
    binding.tree = tree
    binding.enabled = True
    require(binding.tree_name == tree.name, "Binding tree name was not set")
    result = bpy.ops.object.logic_nodes_binding_clear()
    require(result == {"FINISHED"}, f"Binding clear operator failed: {result}")
    require(
        len(target.game.logic_node_bindings) == 0,
        "Binding clear left logic node bindings behind",
    )


def run_phase10_service_blend_load_acceptance(status_path):
    repo_root = Path(__file__).resolve().parents[2]
    fixture_dir = (
        repo_root /
        "source/gameengine/LogicNodes/tests/fixtures/phase10_service_blends"
    )
    for (fixture_name,
         fixture_id,
         fixture_kind,
         service_boundary,
         replay_domain_mask) in PHASE10_SERVICE_BLEND_FIXTURES:
        fixture_path = fixture_dir / fixture_name
        require(fixture_path.exists(), f"Missing Phase 10 service fixture: {fixture_path}")
        bpy.ops.wm.open_mainfile(filepath=str(fixture_path), load_ui=False)
        require(len(bpy.data.scenes) > 0, f"Loaded service fixture has no scene: {fixture_name}")
        require(
            bpy.context.scene is not None,
            f"Loaded service fixture has no active scene: {fixture_name}",
        )
        scene = bpy.context.scene
        require(
            scene.camera is not None,
            f"Service fixture has no active camera for player execution: {fixture_name}",
        )
        require(
            scene.get("UPBGE_logic_nodes_fixture") == "phase10_service_boundary",
            f"Service fixture missing Phase 10 marker: {fixture_name}",
        )
        require(
            scene.get("UPBGE_logic_nodes_fixture_id") == fixture_id,
            f"Service fixture id mismatch: {fixture_name}",
        )
        require(
            scene.get("UPBGE_logic_nodes_fixture_kind") == fixture_kind,
            f"Service fixture kind mismatch: {fixture_name}",
        )
        require(
            scene.get("UPBGE_logic_nodes_service_boundary") == service_boundary,
            f"Service fixture boundary mismatch: {fixture_name}",
        )
        require(
            int(scene.get("UPBGE_logic_nodes_replay_domain_mask", 0)) == replay_domain_mask,
            f"Service fixture replay domain mismatch: {fixture_name}",
        )
        require(
            bool(scene.get("UPBGE_logic_nodes_validates_legacy_mode", False)),
            f"Service fixture does not declare legacy-mode validation: {fixture_name}",
        )
        require(
            bool(scene.get("UPBGE_logic_nodes_validates_optimized_mode", False)),
            f"Service fixture does not declare optimized-mode validation: {fixture_name}",
        )
        logic_trees = [
            tree for tree in bpy.data.node_groups
            if tree.bl_idname == "LogicNodeTree" and
            tree.get("UPBGE_logic_nodes_fixture_id") == fixture_id
        ]
        require(
            logic_trees,
            f"Service fixture has no matching LogicNodeTree: {fixture_name}",
        )
        for tree in logic_trees:
            require(
                tree.get("UPBGE_logic_nodes_service_boundary") == service_boundary,
                f"Service fixture LogicNodeTree boundary mismatch: {fixture_name}",
            )
            require(
                len(tree.nodes) >= 2,
                f"Service fixture LogicNodeTree has no executable nodes: {fixture_name}",
            )
            require(
                len(tree.links) >= 1,
                f"Service fixture LogicNodeTree has no executable flow link: {fixture_name}",
            )
            require(
                any(node.bl_idname == "LogicNativeOnInit" for node in tree.nodes),
                f"Service fixture LogicNodeTree has no OnInit event: {fixture_name}",
            )
            require(
                any(node.bl_idname == "LogicNativeSetGamePropertyString" for node in tree.nodes),
                f"Service fixture LogicNodeTree has no executable marker write: {fixture_name}",
            )
        driver = bpy.data.objects.get(f"LN_Phase10_{fixture_kind}_Driver")
        require(
            driver is not None,
            f"Service fixture has no bound driver object: {fixture_name}",
        )
        require(
            get_game_property_value(driver, "UPBGE_logic_nodes_fixture_id") == fixture_id,
            f"Service fixture driver id mismatch: {fixture_name}",
        )
        require(
            get_game_property_value(driver, "UPBGE_logic_nodes_fixture_executed") is not None,
            f"Service fixture driver has no execution marker property: {fixture_name}",
        )
        require(
            any(binding.enabled and binding.tree_name == logic_trees[0].name
                for binding in driver.game.logic_node_bindings),
            f"Service fixture driver is not bound to the fixture LogicNodeTree: {fixture_name}",
        )
        for tree in logic_trees:
            result = bpy.ops.node.validate_logic_tree(tree_name=tree.name)
            require(
                result == {"FINISHED"},
                f"Service fixture LogicNodeTree failed validation: {fixture_name}: {result}",
            )
            set_marker = next(
                (node for node in tree.nodes
                 if node.bl_idname == "LogicNativeSetGamePropertyString"),
                None,
            )
            require(
                set_marker is not None,
                f"Service fixture LogicNodeTree has no marker command: {fixture_name}",
            )
            quit_node = tree.nodes.new("LogicNativeQuitGame")
            quit_node.name = f"LN_Phase10_{fixture_kind}_CompileGateQuit"
            try:
                tree.links.new(set_marker.outputs["Done"], quit_node.inputs["Flow"])
                bpy.context.view_layer.update()
                result = bpy.ops.node.validate_logic_tree(tree_name=tree.name)
                require(
                    result == {"FINISHED"},
                    f"Service fixture marker-to-quit compile gate failed: "
                    f"{fixture_name}: {result}",
                )
            finally:
                tree.nodes.remove(quit_node)
        payload = {
            "mode": "phase10_service_blend_load_compile",
            "object": driver.name,
            "property": "UPBGE_logic_nodes_fixture_executed",
            "observed": "load-metadata-binding-compile",
            "expected": "load-metadata-binding-compile",
            "ok": True,
            "run": f"phase10_service_fixture_compile_{fixture_kind}",
            "tree_count": len(logic_trees),
        }
        with open(status_path, "a", encoding="utf-8") as status_file:
            status_file.write(json.dumps(payload, sort_keys=True) + "\n")
        require(
            payload["ok"],
            f"Service fixture load/compile gate failed: {payload}",
        )
    bpy.ops.wm.read_factory_settings(use_empty=False)
    disable_legacy_logic_addons()


def run_probe(name, callback):
    print(f"[Logic Nodes acceptance] start {name}", flush=True)
    callback()
    print(f"[Logic Nodes acceptance] finish {name}", flush=True)


def main():
    global PLAYER_OUTPUT_DIR
    global RUNNER

    disable_legacy_logic_addons()
    require(hasattr(bpy.types, "LogicNodeTree"), "LogicNodeTree RNA type is not registered")

    args = parse_args()
    RUNNER = args.runner
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    PLAYER_OUTPUT_DIR = output_dir / "player_blends"
    PLAYER_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    status_path = output_dir / STATUS_FILENAME
    crash_path = Path(tempfile.gettempdir()) / CRASH_FILENAME
    for path in (status_path, crash_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    probes = [
        ("binding_operator_smoke", run_binding_operator_smoke),
        ("basic_restart", lambda: run_basic_restart_probe(status_path)),
        ("inactive_editor_tree", lambda: run_editor_tree_inactive_probe(status_path)),
        ("debug_hardcoded", lambda: run_debug_hardcoded_probe(status_path)),
        ("move_on_init", lambda: run_move_on_init_acceptance(status_path, output_dir)),
        ("logic_gate", lambda: run_logic_gate_acceptance(status_path, output_dir)),
        ("execution_flow", lambda: run_execution_flow_acceptance(status_path)),
        ("euler", lambda: run_euler_acceptance(status_path, output_dir)),
        ("orientation", lambda: run_orientation_acceptance(status_path, output_dir)),
        ("velocity", lambda: run_velocity_acceptance(status_path, output_dir)),
        ("impulse", lambda: run_impulse_acceptance(status_path)),
        ("object_actions", lambda: run_object_actions_acceptance(status_path, output_dir)),
        ("material_actions", lambda: run_material_actions_acceptance(status_path)),
        ("property_branch", lambda: run_property_branch_acceptance(status_path, output_dir)),
        ("property_writes", lambda: run_property_write_acceptance(status_path)),
        ("property_target", lambda: run_property_target_acceptance(status_path)),
        ("property_advanced", lambda: run_property_advanced_acceptance(status_path)),
        ("tree_properties", lambda: run_tree_property_acceptance(status_path)),
        ("data_containers", lambda: run_data_containers_acceptance(status_path)),
        ("bone_read", lambda: run_bone_read_acceptance(status_path)),
        ("bone_parent", lambda: run_bone_parent_acceptance(status_path)),
        ("compound_parent", lambda: run_compound_parent_acceptance(status_path)),
        ("bone_pose", lambda: run_bone_pose_acceptance(status_path)),
        ("input_nodes", lambda: run_input_nodes_acceptance(status_path)),
        ("values_math", lambda: run_values_math_acceptance(status_path)),
        ("c_tier_values", lambda: run_c_tier_values_acceptance(status_path)),
        ("sound_state", lambda: run_sound_state_acceptance(status_path)),
        ("sound_advanced", lambda: run_sound_advanced_acceptance(status_path)),
        ("vehicle_control_compile", lambda: run_vehicle_control_compile_acceptance(status_path)),
        ("time_state", lambda: run_time_state_acceptance(status_path)),
        ("input_map", lambda: run_input_map_acceptance(status_path)),
        ("inventory_recipes", lambda: run_inventory_recipes_acceptance(status_path)),
        ("inventory_in_place", lambda: run_inventory_in_place_acceptance(status_path)),
        ("inventory_rule_sets", lambda: run_inventory_rule_sets_acceptance(status_path)),
        ("ui_player_menu", lambda: run_ui_player_menu_acceptance(status_path)),
        ("inventory_two_inventories",
         lambda: run_inventory_two_inventories_acceptance(status_path)),
        ("ui_menu_replace_inside", lambda: run_ui_menu_replace_inside_acceptance(status_path)),
        ("loop_routes", lambda: run_loop_routes_acceptance(status_path)),
        ("scene_state", lambda: run_scene_state_acceptance(status_path)),
        ("inactive_overlay_collection",
         lambda: run_inactive_overlay_collection_acceptance(status_path)),
        ("object_menu_session", lambda: run_object_menu_session_acceptance(status_path)),
        ("ui_elements", lambda: run_ui_elements_acceptance(status_path)),
        ("ui_menu_focus_restore", lambda: run_ui_menu_focus_restore_acceptance(status_path)),
        ("ui_overlay_visibility", lambda: run_ui_overlay_visibility_acceptance(status_path)),
        ("ui_grid_template", lambda: run_ui_grid_template_acceptance(status_path)),
        ("ui_grid_scrollbar", lambda: run_ui_grid_scrollbar_acceptance(status_path)),
        ("ui_grid_pages", lambda: run_ui_grid_pages_acceptance(status_path)),
        ("ui_grid_inventory", lambda: run_ui_grid_inventory_acceptance(status_path)),
        ("ui_menu_pause_toggle", lambda: run_ui_menu_pause_toggle_acceptance(status_path)),
        ("ui_text_values_bar", lambda: run_ui_text_values_bar_acceptance(status_path)),
        ("ui_grid_parented_template",
         lambda: run_ui_grid_parented_template_acceptance(status_path)),
        ("ui_menu_camera_hidden", lambda: run_ui_menu_camera_hidden_acceptance(status_path)),
        ("game_property_types",
         lambda: run_game_property_types_acceptance(status_path, output_dir)),
        ("ui_paused_routes", lambda: run_ui_paused_routes_acceptance(status_path)),
        ("ui_sound_events", lambda: run_ui_sound_events_acceptance(status_path)),
        ("sound_groups", lambda: run_sound_groups_acceptance(status_path)),
        ("smooth_motion", lambda: run_smooth_motion_acceptance(status_path)),
        ("frame_pacing", lambda: run_frame_pacing_acceptance(status_path)),
        ("camera_state", lambda: run_camera_state_acceptance(status_path)),
        ("split_screen", lambda: run_split_screen_acceptance(status_path)),
        ("camera_renderers", lambda: run_camera_renderers_acceptance(status_path)),
        ("camera_minimap_options",
         lambda: run_camera_minimap_options_acceptance(status_path)),
        ("camera_visibility_still",
         lambda: run_camera_visibility_still_acceptance(status_path)),
        ("collection_membership",
         lambda: run_collection_membership_acceptance(status_path, output_dir)),
        ("split_screen_leave", lambda: run_split_screen_leave_acceptance(status_path)),
        ("split_screen_players", lambda: run_split_screen_players_acceptance(status_path)),
        ("ui_menu_per_player", lambda: run_ui_menu_per_player_acceptance(status_path)),
        ("lod_nearest_view", lambda: run_lod_nearest_view_acceptance(status_path)),
        ("lod_view_size", lambda: run_lod_view_size_acceptance(status_path)),
        ("ui_element_players", lambda: run_ui_element_players_acceptance(status_path)),
        ("ui_take_control_pointer",
         lambda: run_ui_take_control_pointer_acceptance(status_path)),
        ("ui_view_center", lambda: run_ui_view_center_acceptance(status_path)),
        ("render_state", lambda: run_render_state_acceptance(status_path)),
        ("light_state", lambda: run_light_state_acceptance(status_path)),
        ("light_unique", lambda: run_light_unique_acceptance(status_path)),
        ("physics_state", lambda: run_physics_state_acceptance(status_path)),
        ("character_state", lambda: run_character_state_acceptance(status_path)),
        ("object_lifecycle", lambda: run_object_lifecycle_acceptance(status_path)),
        ("object_queries", lambda: run_object_query_acceptance(status_path)),
        ("average_center", lambda: run_average_center_position_acceptance(status_path)),
        ("tier_a", lambda: run_tier_a_acceptance(status_path)),
        ("raycast", lambda: run_raycast_acceptance(status_path)),
        ("raycast_from_mesh", lambda: run_raycast_from_mesh_acceptance(status_path)),
        ("phase6_events", lambda: run_phase6_events_acceptance(status_path)),
        ("phase6_move_toward", lambda: run_phase6_move_toward_acceptance(status_path)),
        ("navigate_visualize", lambda: run_navigate_visualize_acceptance(status_path)),
        ("phase6_copy_property", lambda: run_phase6_copy_property_acceptance(status_path)),
        ("phase6_get_object_attribute",
         lambda: run_phase6_get_object_attribute_acceptance(status_path)),
        ("phase6_rotate_to", lambda: run_phase6_rotate_to_acceptance(status_path)),
        ("phase6_collision", lambda: run_phase6_collision_acceptance(status_path)),
        ("phase6_objects_colliding", lambda: run_phase6_objects_colliding_acceptance(status_path)),
        ("phase6_animation_status", lambda: run_phase6_animation_status_acceptance(status_path)),
        ("phase6_cross_scene_save_load",
         lambda: run_phase6_cross_scene_save_load_acceptance(status_path, output_dir)),
        ("phase6_save_load", lambda: run_phase6_save_load_acceptance(status_path, output_dir)),
        ("phase6_save_load_structure",
         lambda: run_phase6_save_load_structure_acceptance(status_path, output_dir)),
        ("phase6_save_load_material",
         lambda: run_phase6_save_load_material_acceptance(status_path, output_dir)),
        ("phase6_save_load_existence",
         lambda: run_phase6_save_load_existence_acceptance(status_path, output_dir)),
        ("phase6_align_axis", lambda: run_phase6_align_axis_acceptance(status_path)),
        ("phase6_slow_follow", lambda: run_phase6_slow_follow_acceptance(status_path)),
        ("phase6_replace_mesh", lambda: run_phase6_replace_mesh_acceptance(status_path)),
        ("material_shader_node_input", lambda: run_material_shader_node_input_acceptance(status_path)),
        ("mouse_over", lambda: run_mouse_over_acceptance(status_path)),
        ("mouse_over_cadence", lambda: run_mouse_over_cadence_acceptance(status_path)),
        ("disabled_binding", lambda: run_disabled_binding_probe(status_path)),
        ("missing_tree", lambda: run_missing_tree_probe(status_path)),
        ("variable_timestep", lambda: run_variable_timestep_probe(status_path)),
        ("b_tier_core", lambda: run_b_tier_core_acceptance(status_path)),
        ("phase10_service_blend_load",
         lambda: run_phase10_service_blend_load_acceptance(status_path)),
        # Next to last: its menus, trees and actions stay in the file.
        ("ui_templates", lambda: run_ui_templates_acceptance(status_path)),
        # Last: it opens the installed example in place of the current file.
        ("ui_example_file", lambda: run_ui_example_file_acceptance(status_path)),
    ]

    if args.only:
        wanted = set(args.only)
        available = {name for name, _callback in probes}
        unknown = sorted(wanted - available)
        require(not unknown, f"Unknown runtime acceptance probe(s): {unknown}")
        probes = [(name, callback) for name, callback in probes if name in wanted]

    for name, callback in probes:
        run_probe(name, callback)

    if crash_path.exists():
        raise RuntimeError(f"Unexpected crash log after Logic Nodes acceptance runs: {crash_path}")

    print(f"Logic Nodes runtime acceptance passed with {RUNNER} runner", flush=True)
    os._exit(0)


if __name__ == "__main__":
    main()
