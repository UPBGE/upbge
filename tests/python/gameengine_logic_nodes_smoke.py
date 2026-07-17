# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

import bpy


TARGET_POSITION = (1.25, 2.5, 3.75)
STATUS_FILENAME = "logic_nodes_debug_runtime_restart.jsonl"
CRASH_FILENAME = "blender.crash.txt"


GAME_CONTROLLER_SCRIPT = r'''
import json
import traceback

import bge


def write_status(owner, payload):
    status_path = owner.get("__ln_smoke_status_path", "")
    if not status_path:
        return
    with open(status_path, "a", encoding="utf-8") as status_file:
        status_file.write(json.dumps(payload, sort_keys=True) + "\n")


controller = bge.logic.getCurrentController()
owner = controller.owner

if not owner.get("__ln_smoke_done", False):
    owner["__ln_smoke_done"] = True
    try:
        run_index = int(owner.get("__ln_smoke_run_index", -1))
        expected = (
            float(owner.get("__ln_debug_set_world_position_x", 0.0)),
            float(owner.get("__ln_debug_set_world_position_y", 0.0)),
            float(owner.get("__ln_debug_set_world_position_z", 0.0)),
        )
        observed = tuple(float(value) for value in owner.worldPosition)
        ok = all(abs(observed[index] - expected[index]) <= 1e-4 for index in range(3))
        write_status(owner, {
            "ok": ok,
            "run": run_index,
            "observed": observed,
            "expected": expected,
        })
    except Exception:
        write_status(owner, {
            "ok": False,
            "run": int(owner.get("__ln_smoke_run_index", -1)),
            "error": traceback.format_exc(),
        })
    finally:
        bge.logic.endGame()
'''


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)

    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    return parser.parse_args(argv)


def active_view3d_context():
    window = bpy.context.window
    screen = window.screen
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


def set_game_property(obj, name, property_type, value):
    for prop in obj.game.properties:
        if prop.name == name:
            prop.type = property_type
            prop.value = value
            return prop

    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    result = bpy.ops.object.game_property_new(type=property_type, name=name)
    if result != {"FINISHED"}:
        raise RuntimeError(f"Failed to add game property {name!r}: {result}")
    prop = obj.game.properties[-1]
    prop.value = value
    return prop


def configure_scene(status_path):
    scene = bpy.context.scene
    game_settings = scene.game_settings
    game_settings.physics_engine = "JOLT"
    game_settings.use_fixed_physics_timestep = True
    game_settings.physics_timestep_method = "FIXED"
    game_settings.physics_tick_rate = 60

    target = bpy.data.objects.get("Cube")
    if target is None:
        bpy.ops.mesh.primitive_cube_add()
        target = bpy.context.object
    target.name = "LN_DebugRuntimeTarget"
    target.location = (0.0, 0.0, 0.0)
    bpy.context.view_layer.objects.active = target
    target.select_set(True)

    set_game_property(target, "__ln_debug_set_world_position", "FLOAT", 1.0)
    set_game_property(target, "__ln_debug_set_world_position_x", "FLOAT", TARGET_POSITION[0])
    set_game_property(target, "__ln_debug_set_world_position_y", "FLOAT", TARGET_POSITION[1])
    set_game_property(target, "__ln_debug_set_world_position_z", "FLOAT", TARGET_POSITION[2])
    set_game_property(target, "__ln_smoke_status_path", "STRING", str(status_path))
    set_game_property(target, "__ln_smoke_run_index", "INT", 0)

    controller_text = bpy.data.texts.new("logic_nodes_debug_runtime_smoke.py")
    controller_text.write(GAME_CONTROLLER_SCRIPT)

    result = bpy.ops.logic.sensor_add(type="ALWAYS", name="LNSmokeAlways", object=target.name)
    if result != {"FINISHED"}:
        raise RuntimeError(f"Failed to add always sensor: {result}")
    sensor = target.game.sensors[-1]
    sensor.use_pulse_true_level = True
    sensor.tick_skip = 0

    result = bpy.ops.logic.controller_add(type="PYTHON", name="LNSmokeController", object=target.name)
    if result != {"FINISHED"}:
        raise RuntimeError(f"Failed to add Python controller: {result}")
    controller = target.game.controllers[-1]
    controller.mode = "SCRIPT"
    controller.text = controller_text
    controller.link(sensor=sensor)
    if len(sensor.controllers) != 1 or sensor.controllers[0] != controller:
        raise RuntimeError("Failed to link smoke sensor to Python controller")

    result = bpy.ops.logic.sensor_add(type="DELAY", name="LNSmokeFallbackDelay", object=target.name)
    if result != {"FINISHED"}:
        raise RuntimeError(f"Failed to add fallback delay sensor: {result}")
    fallback_sensor = target.game.sensors[-1]
    fallback_sensor.delay = 15
    fallback_sensor.duration = 0
    fallback_sensor.use_repeat = False

    result = bpy.ops.logic.controller_add(
        type="LOGIC_AND", name="LNSmokeFallbackController", object=target.name)
    if result != {"FINISHED"}:
        raise RuntimeError(f"Failed to add fallback controller: {result}")
    fallback_controller = target.game.controllers[-1]

    result = bpy.ops.logic.actuator_add(type="GAME", name="LNSmokeFallbackQuit", object=target.name)
    if result != {"FINISHED"}:
        raise RuntimeError(f"Failed to add fallback quit actuator: {result}")
    fallback_actuator = target.game.actuators[-1]
    fallback_actuator.mode = "QUIT"
    fallback_controller.link(sensor=fallback_sensor, actuator=fallback_actuator)

    return target


def run_game_once(target, run_index, status_path):
    set_game_property(target, "__ln_smoke_run_index", "INT", run_index)
    target.location = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()

    before_line_count = 0
    if status_path.exists():
        before_line_count = len(status_path.read_text(encoding="utf-8").splitlines())

    with bpy.context.temp_override(**active_view3d_context()):
        result = bpy.ops.view3d.game_start()
    if result != {"FINISHED"}:
        raise RuntimeError(f"view3d.game_start failed on run {run_index}: {result}")

    if not status_path.exists():
        raise RuntimeError(f"Smoke run {run_index} did not write a status file")

    lines = status_path.read_text(encoding="utf-8").splitlines()
    new_lines = lines[before_line_count:]
    if not new_lines:
        raise RuntimeError(f"Smoke run {run_index} did not write a new status record")

    payload = json.loads(new_lines[-1])
    if payload.get("run") != run_index:
        raise RuntimeError(f"Smoke run {run_index} wrote unexpected status: {payload}")
    if not payload.get("ok", False):
        raise RuntimeError(f"Smoke run {run_index} failed: {payload}")


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    status_path = output_dir / STATUS_FILENAME
    crash_path = Path(tempfile.gettempdir()) / CRASH_FILENAME

    for path in (status_path, crash_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    target = configure_scene(status_path)
    run_game_once(target, 0, status_path)
    run_game_once(target, 1, status_path)

    if crash_path.exists():
        raise RuntimeError(f"Unexpected crash log after smoke runs: {crash_path}")

    print("Logic Nodes debug runtime restart smoke passed")
    bpy.ops.wm.quit_blender()


if __name__ == "__main__":
    main()
