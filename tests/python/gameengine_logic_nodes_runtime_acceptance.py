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


def flow_input(node):
    if "Flow" in node.inputs:
        return node.inputs["Flow"]
    return flow_input(node)


GAME_CONTROLLER_SCRIPT = r'''
import json
import math
import traceback

import bge


def vector_tuple(value):
    return tuple(float(component) for component in value)


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


def check_owner(owner):
    mode = owner.get("__ln_acceptance_mode", "quit_only")
    tolerance = float(owner.get("__ln_acceptance_tolerance", 0.0001))
    expected = expected_vector(owner)

    if mode == "quit_only":
        return True, {"mode": mode}

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
        bone_length = float(owner.get("bone_length", 0.0))
        bone_head_z = float(owner.get("bone_head_z", 0.0))
        bone_center_z = float(owner.get("bone_center_z", 0.0))
        ok = (
            bone_length > 0.5
            and bone_length < 2.0
            and bone_head_z > 0.5
            and bone_center_z > 0.25
            and bone_center_z < bone_head_z
        )
        return ok, {
            "mode": mode,
            "bone_length": bone_length,
            "bone_head_z": bone_head_z,
            "bone_center_z": bone_center_z,
        }

    if mode == "bone_pose":
        bone_rest_head_z = float(owner.get("bone_rest_head_z", 0.0))
        bone_pose_head_z = float(owner.get("bone_pose_head_z", 0.0))
        ok = (
            bone_rest_head_z > 0.5
            and bone_pose_head_z > bone_rest_head_z + 0.1
        )
        return ok, {
            "mode": mode,
            "bone_rest_head_z": bone_rest_head_z,
            "bone_pose_head_z": bone_pose_head_z,
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
        return bool(owner.get("action_playing", False)), {
            "mode": mode,
            "action_playing": bool(owner.get("action_playing", False)),
        }

    if mode == "phase6_save_load":
        score = float(owner.get("score", 0.0))
        expected = float(owner.get("__ln_acceptance_expected_score", 42.0))
        return abs(score - expected) <= tolerance, {
            "mode": mode,
            "score": score,
            "expected": expected,
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
        return abs(dot) >= min_dot, {"mode": mode, "dot": dot, "min_dot": min_dot, "local_z": local_z}

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
        return abs(result - expected) <= tolerance, {
            "mode": mode,
            "result": result,
            "expected": expected,
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

    if mode == "time_state":
        keys = ("delay", "timer", "pulsify", "barrier")
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

    if mode == "raycast":
        hit = bool(owner.get("ray_hit", False))
        object_hit = bool(owner.get("ray_object", False))
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
        expected_point = expected_named_vector(owner, "hit_point")
        expected_normal = expected_named_vector(owner, "hit_normal")
        expected_direction = expected_named_vector(owner, "hit_direction")
        ok = (
            hit
            and object_hit
            and close_tuple(point, expected_point, tolerance)
            and close_tuple(normal, expected_normal, tolerance)
            and close_tuple(direction, expected_direction, tolerance)
        )
        return ok, {
            "mode": mode,
            "hit": hit,
            "object_hit": object_hit,
            "point": point,
            "expected_point": expected_point,
            "normal": normal,
            "expected_normal": expected_normal,
            "direction": direction,
            "expected_direction": expected_direction,
        }

    if mode == "tier_a":
        list_len = int(owner.get("tier_a_list_len", 0))
        dict_len = int(owner.get("tier_a_dict_len", 0))
        global_value = float(owner.get("tier_a_global", 0.0))
        found_by_name = bool(owner.get("tier_a_found_by_name", False))
        owner_ok = bool(owner.get("tier_a_owner_ok", False))
        translate_x = float(owner.worldPosition[0])
        ok = (
            list_len == 2
            and dict_len == 1
            and abs(global_value - 99.0) <= tolerance
            and found_by_name
            and owner_ok
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

    if mode == "mouse_over":
        entered = bool(owner.get("mouse_enter", False))
        over = bool(owner.get("mouse_over", False))
        exited = bool(owner.get("mouse_exit", False))
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
        expected_point = expected_named_vector(owner, "mouse_point")
        expected_normal = expected_named_vector(owner, "mouse_normal")
        ok = (
            entered
            and over
            and not exited
            and close_tuple(point, expected_point, tolerance)
            and close_tuple(normal, expected_normal, tolerance)
        )
        return ok, {
            "mode": mode,
            "entered": entered,
            "over": over,
            "exited": exited,
            "point": point,
            "expected_point": expected_point,
            "normal": normal,
            "expected_normal": expected_normal,
        }

    return False, {"mode": mode, "error": "unknown mode"}


controller = bge.logic.getCurrentController()
owner = controller.owner

frame = int(owner.get("__ln_acceptance_frame", 0)) + 1
owner["__ln_acceptance_frame"] = frame
required_frame = int(owner.get("__ln_acceptance_required_frame", 1))

if frame >= required_frame and not owner.get("__ln_acceptance_done", False):
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
import traceback

import bge


def vector_tuple(value):
    return tuple(float(component) for component in value)


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


def check_owner(owner):
    mode = owner.get("__ln_acceptance_mode", "quit_only")
    tolerance = float(owner.get("__ln_acceptance_tolerance", 0.0001))
    expected = expected_vector(owner)

    if mode == "quit_only":
        return True, {"mode": mode}

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
        bone_length = float(owner.get("bone_length", 0.0))
        bone_head_z = float(owner.get("bone_head_z", 0.0))
        bone_center_z = float(owner.get("bone_center_z", 0.0))
        ok = (
            bone_length > 0.5
            and bone_length < 2.0
            and bone_head_z > 0.5
            and bone_center_z > 0.25
            and bone_center_z < bone_head_z
        )
        return ok, {
            "mode": mode,
            "bone_length": bone_length,
            "bone_head_z": bone_head_z,
            "bone_center_z": bone_center_z,
        }

    if mode == "bone_pose":
        bone_rest_head_z = float(owner.get("bone_rest_head_z", 0.0))
        bone_pose_head_z = float(owner.get("bone_pose_head_z", 0.0))
        ok = (
            bone_rest_head_z > 0.5
            and bone_pose_head_z > bone_rest_head_z + 0.1
        )
        return ok, {
            "mode": mode,
            "bone_rest_head_z": bone_rest_head_z,
            "bone_pose_head_z": bone_pose_head_z,
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
        return bool(owner.get("action_playing", False)), {
            "mode": mode,
            "action_playing": bool(owner.get("action_playing", False)),
        }

    if mode == "phase6_save_load":
        score = float(owner.get("score", 0.0))
        expected = float(owner.get("__ln_acceptance_expected_score", 42.0))
        return abs(score - expected) <= tolerance, {
            "mode": mode,
            "score": score,
            "expected": expected,
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
        return abs(dot) >= min_dot, {"mode": mode, "dot": dot, "min_dot": min_dot, "local_z": local_z}

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
        return abs(result - expected) <= tolerance, {
            "mode": mode,
            "result": result,
            "expected": expected,
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

    if mode == "time_state":
        keys = ("delay", "timer", "pulsify", "barrier")
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

    if mode == "raycast":
        hit = bool(owner.get("ray_hit", False))
        object_hit = bool(owner.get("ray_object", False))
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
        expected_point = expected_named_vector(owner, "hit_point")
        expected_normal = expected_named_vector(owner, "hit_normal")
        expected_direction = expected_named_vector(owner, "hit_direction")
        ok = (
            hit
            and object_hit
            and close_tuple(point, expected_point, tolerance)
            and close_tuple(normal, expected_normal, tolerance)
            and close_tuple(direction, expected_direction, tolerance)
        )
        return ok, {
            "mode": mode,
            "hit": hit,
            "object_hit": object_hit,
            "point": point,
            "expected_point": expected_point,
            "normal": normal,
            "expected_normal": expected_normal,
            "direction": direction,
            "expected_direction": expected_direction,
        }

    if mode == "tier_a":
        list_len = int(owner.get("tier_a_list_len", 0))
        dict_len = int(owner.get("tier_a_dict_len", 0))
        global_value = float(owner.get("tier_a_global", 0.0))
        found_by_name = bool(owner.get("tier_a_found_by_name", False))
        owner_ok = bool(owner.get("tier_a_owner_ok", False))
        translate_x = float(owner.worldPosition[0])
        ok = (
            list_len == 2
            and dict_len == 1
            and abs(global_value - 99.0) <= tolerance
            and found_by_name
            and owner_ok
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

    if mode == "mouse_over":
        entered = bool(owner.get("mouse_enter", False))
        over = bool(owner.get("mouse_over", False))
        exited = bool(owner.get("mouse_exit", False))
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
        expected_point = expected_named_vector(owner, "mouse_point")
        expected_normal = expected_named_vector(owner, "mouse_normal")
        ok = (
            entered
            and over
            and not exited
            and close_tuple(point, expected_point, tolerance)
            and close_tuple(normal, expected_normal, tolerance)
        )
        return ok, {
            "mode": mode,
            "entered": entered,
            "over": over,
            "exited": exited,
            "point": point,
            "expected_point": expected_point,
            "normal": normal,
            "expected_normal": expected_normal,
        }

    if mode == "bone_read":
        bone_length = float(owner.get("bone_length", 0.0))
        bone_head_z = float(owner.get("bone_head_z", 0.0))
        bone_center_z = float(owner.get("bone_center_z", 0.0))
        ok = (
            bone_length > 0.5
            and bone_length < 2.0
            and bone_head_z > 0.5
            and bone_center_z > 0.25
            and bone_center_z < bone_head_z
        )
        return ok, {
            "mode": mode,
            "bone_length": bone_length,
            "bone_head_z": bone_head_z,
            "bone_center_z": bone_center_z,
        }

    if mode == "bone_pose":
        bone_rest_head_z = float(owner.get("bone_rest_head_z", 0.0))
        bone_pose_head_z = float(owner.get("bone_pose_head_z", 0.0))
        ok = (
            bone_rest_head_z > 0.5
            and bone_pose_head_z > bone_rest_head_z + 0.1
        )
        return ok, {
            "mode": mode,
            "bone_rest_head_z": bone_rest_head_z,
            "bone_pose_head_z": bone_pose_head_z,
        }

    return False, {"mode": mode, "error": "unknown mode"}


scene = bge.logic.getCurrentScene()
owner = None
for candidate in scene.objects:
    if candidate.get("__ln_acceptance_status_path", ""):
        owner = candidate
        break

if owner is None:
    bge.logic.endGame()
else:
    frame = int(owner.get("__ln_acceptance_frame", 0)) + 1
    owner["__ln_acceptance_frame"] = frame
    required_frame = int(owner.get("__ln_acceptance_required_frame", 1))
    if frame >= required_frame and not owner.get("__ln_acceptance_done", False):
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


def set_active_object(obj):
    for scene_obj in bpy.context.scene.objects:
        scene_obj.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


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

    if scene.camera is None:
        bpy.ops.object.camera_add(location=(0.0, -8.0, 5.0), rotation=(math.radians(60.0), 0.0, 0.0))
        scene.camera = bpy.context.object


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


def create_armature(name):
    bpy.ops.object.armature_add(enter_editmode=False, location=(0.0, 0.0, 0.0))
    obj = bpy.context.object
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
    obj = bpy.context.object
    obj.name = name
    return obj


def create_light(name, location, color=(1.0, 1.0, 1.0), power=1.0, use_shadow=True):
    bpy.ops.object.light_add(type="POINT", location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.color = color
    obj.data.energy = power
    obj.data.use_shadow = use_shadow
    return obj


def add_status_controller(obj, status_path, mode, required_frame=1, tolerance=0.0001):
    set_game_property(obj, "__ln_acceptance_status_path", "STRING", str(status_path))
    set_game_property(obj, "__ln_acceptance_run", "STRING", "")
    set_game_property(obj, "__ln_acceptance_mode", "STRING", mode)
    set_game_property(obj, "__ln_acceptance_frame", "INT", 0)
    set_game_property(obj, "__ln_acceptance_required_frame", "INT", required_frame)
    set_game_property(obj, "__ln_acceptance_done", "BOOL", False)
    set_game_property(obj, "__ln_acceptance_tolerance", "FLOAT", tolerance)

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
        fallback_sensor.delay = 30
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
    camera = bpy.context.object
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
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")
    if target_obj is not None:
        set_position.inputs["Object"].default_value = target_obj
    set_position.inputs["Position"].default_value = position
    tree.links.new(event_node.outputs["Out"], flow_input(set_position))
    return tree


def build_logic_gate_tree(name, position):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    condition_a = tree.nodes.new("LogicNativeValueBool")
    condition_b = tree.nodes.new("LogicNativeValueBool")
    gate = tree.nodes.new("LogicNativeGate")
    branch = tree.nodes.new("LogicNativeBranch")
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")

    value_output(condition_a).default_value = True
    value_output(condition_b).default_value = False
    gate.operation = "OR"
    set_position.inputs["Position"].default_value = position

    tree.links.new(on_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(value_output(condition_a), gate.inputs["Condition A"])
    tree.links.new(value_output(condition_b), gate.inputs["Condition B"])
    tree.links.new(gate.outputs["Result"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_position))
    return tree


def build_euler_orientation_tree(name, rotation, target_obj=None):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    euler = tree.nodes.new("LogicNativeEuler")
    set_orientation = tree.nodes.new("LogicNativeSetWorldOrientation")

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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    get_world_orientation = tree.nodes.new("LogicNativeGetWorldOrientation")
    get_local_orientation = tree.nodes.new("LogicNativeGetLocalOrientation")
    set_world_orientation = tree.nodes.new("LogicNativeSetWorldOrientation")
    set_local_orientation = tree.nodes.new("LogicNativeSetLocalOrientation")

    tree.links.new(on_update.outputs["Out"], flow_input(set_world_orientation))
    tree.links.new(on_update.outputs["Out"], flow_input(set_local_orientation))
    tree.links.new(get_local_orientation.outputs["Rotation"], set_world_orientation.inputs["Rotation"])
    tree.links.new(get_world_orientation.outputs["Rotation"], set_local_orientation.inputs["Rotation"])
    return tree


def build_velocity_tree(name, velocity, target_obj=None):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    set_velocity = tree.nodes.new("LogicNativeSetLinearVelocity")
    if target_obj is not None:
        set_velocity.inputs["Object"].default_value = target_obj
    set_velocity.inputs["Velocity"].default_value = velocity
    tree.links.new(on_update.outputs["Out"], flow_input(set_velocity))
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
    set_visibility = tree.nodes.new("LogicNativeSetVisibility")
    color_rgba = tree.nodes.new("LogicNativeColorRGBA")
    set_color = tree.nodes.new("LogicNativeSetObjectColor")
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")
    set_orientation = tree.nodes.new("LogicNativeSetWorldOrientation")
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
    set_position.inputs["Position"].default_value = movement
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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    get_enabled = tree.nodes.new("LogicNativeGetGamePropertyBool")
    branch = tree.nodes.new("LogicNativeBranch")
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")
    set_moved = tree.nodes.new("LogicNativeSetGamePropertyBool")

    get_enabled.inputs["Property"].default_value = "enabled"
    set_position.inputs["Position"].default_value = position
    set_moved.inputs["Property"].default_value = "moved"
    set_moved.inputs["Value"].default_value = True

    tree.links.new(on_update.outputs["Out"], branch.inputs["Flow"])
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


def build_bone_pose_tree(name, armature_obj):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    get_rest_head = tree.nodes.new("LogicNativeGetBoneHeadWorld")
    get_pose_head = tree.nodes.new("LogicNativeGetBoneHeadPoseWorld")
    set_bone_pose = tree.nodes.new("LogicNativeSetBonePoseLocation")
    pose_offset = tree.nodes.new("LogicNativeCombineXYZ")
    separate_rest = tree.nodes.new("LogicNativeSeparateXYZ")
    separate_pose = tree.nodes.new("LogicNativeSeparateXYZ")
    set_rest_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_pose_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    get_rest_head.inputs["Object"].default_value = armature_obj
    get_pose_head.inputs["Object"].default_value = armature_obj
    set_bone_pose.inputs["Object"].default_value = armature_obj
    set_rest_z.inputs["Property"].default_value = "bone_rest_head_z"
    set_pose_z.inputs["Property"].default_value = "bone_pose_head_z"
    pose_offset.inputs["Z"].default_value = 1.0

    tree.links.new(on_init.outputs["Out"], flow_input(set_rest_z))
    tree.links.new(get_rest_head.outputs["Position"], separate_rest.inputs["Vector"])
    tree.links.new(separate_rest.outputs["Z"], set_rest_z.inputs["Value"])
    tree.links.new(on_update.outputs["Out"], flow_input(set_bone_pose))
    tree.links.new(pose_offset.outputs["Vector"], set_bone_pose.inputs["Location"])
    tree.links.new(set_bone_pose.outputs["Done"], flow_input(set_pose_z))
    tree.links.new(get_pose_head.outputs["Position"], separate_pose.inputs["Vector"])
    tree.links.new(separate_pose.outputs["Z"], set_pose_z.inputs["Value"])
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

    item_a.outputs["Value"].default_value = 1.0
    item_b.outputs["Value"].default_value = 2.0
    item_c.outputs["Value"].default_value = 3.0
    set_list_len.inputs["Property"].default_value = "list_len"

    dict_value.outputs["Value"].default_value = 7.5
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

    sum_item_a.outputs["Value"].default_value = 2.0
    sum_item_b.outputs["Value"].default_value = 3.0
    sum_item_c.outputs["Value"].default_value = 4.0
    modify_sum.inputs["Property"].default_value = "list_sum"
    list_index.outputs["Value"].default_value = 1
    set_list_item_value.inputs["Property"].default_value = "list_item_value"
    dict_has_answer.inputs["Key"].default_value = "answer"
    dict_has_missing.inputs["Key"].default_value = "missing"
    set_has_answer_key.inputs["Property"].default_value = "has_answer_key"
    set_has_answer_key.inputs["Value"].default_value = True
    set_has_missing_key.inputs["Property"].default_value = "has_missing_key"
    set_has_missing_key.inputs["Value"].default_value = True
    set_list_dup_len.inputs["Property"].default_value = "list_dup_len"
    dict_extra_value.outputs["Value"].default_value = 2.0
    make_dict_extra.inputs["Key"].default_value = "extra"
    set_merged_dict_len.inputs["Property"].default_value = "merged_dict_len"
    contains_two_value.outputs["Value"].default_value = 2.0
    contains_nine_value.outputs["Value"].default_value = 9.0
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

    nested_item_a.outputs["Value"].default_value = 1.0
    nested_item_b.outputs["Value"].default_value = 2.0
    nested_outer_value.outputs["Value"].default_value = 3.0
    nested_needle_a.outputs["Value"].default_value = 1.0
    nested_needle_b.outputs["Value"].default_value = 2.0
    set_list_has_nested.inputs["Property"].default_value = "list_has_nested"
    set_list_has_nested.inputs["Value"].default_value = True

    typed_item_a.outputs["Value"].default_value = 4.0
    typed_item_b.outputs["Value"].default_value = 5.0
    typed_item_c.outputs["Value"].default_value = 6.0
    list_from_items.mode = "FLOAT"
    set_typed_list_len.inputs["Property"].default_value = "typed_list_len"
    modify_typed_sum.inputs["Property"].default_value = "typed_list_sum"

    tree.links.new(on_init.outputs["Out"], branch_list_has_two.inputs["Flow"])
    tree.links.new(make_list.outputs["List"], list_contains_two.inputs["List"])
    tree.links.new(contains_two_value.outputs["Value"], list_contains_two.inputs["Value"])
    tree.links.new(list_contains_two.outputs["Contains"], branch_list_has_two.inputs["Condition"])
    tree.links.new(branch_list_has_two.outputs["True"], flow_input(set_list_has_two))

    tree.links.new(on_init.outputs["Out"], branch_list_has_nine.inputs["Flow"])
    tree.links.new(make_list.outputs["List"], list_contains_nine.inputs["List"])
    tree.links.new(contains_nine_value.outputs["Value"], list_contains_nine.inputs["Value"])
    tree.links.new(list_contains_nine.outputs["Contains"], branch_list_has_nine.inputs["Condition"])
    tree.links.new(branch_list_has_nine.outputs["True"], flow_input(set_list_has_nine))

    tree.links.new(nested_item_a.outputs["Value"], nested_inner_list.inputs["Item A"])
    tree.links.new(nested_item_b.outputs["Value"], nested_inner_list.inputs["Item B"])
    tree.links.new(nested_inner_list.outputs["List"], nested_outer_list.inputs["Item A"])
    tree.links.new(nested_outer_value.outputs["Value"], nested_outer_list.inputs["Item B"])
    tree.links.new(nested_needle_a.outputs["Value"], nested_needle_list.inputs["Item A"])
    tree.links.new(nested_needle_b.outputs["Value"], nested_needle_list.inputs["Item B"])
    tree.links.new(on_init.outputs["Out"], branch_list_has_nested.inputs["Flow"])
    tree.links.new(nested_outer_list.outputs["List"], list_contains_nested.inputs["List"])
    tree.links.new(nested_needle_list.outputs["List"], list_contains_nested.inputs["Value"])
    tree.links.new(list_contains_nested.outputs["Contains"], branch_list_has_nested.inputs["Condition"])
    tree.links.new(branch_list_has_nested.outputs["True"], flow_input(set_list_has_nested))

    tree.links.new(typed_item_a.outputs["Value"], list_from_items.inputs["Floats"])
    tree.links.new(typed_item_b.outputs["Value"], list_from_items.inputs["Floats"])
    tree.links.new(typed_item_c.outputs["Value"], list_from_items.inputs["Floats"])
    tree.links.new(on_init.outputs["Out"], flow_input(set_typed_list_len))
    tree.links.new(list_from_items.outputs["Floats"], typed_list_length.inputs["List"])
    tree.links.new(typed_list_length.outputs["Length"], set_typed_list_len.inputs["Value"])
    tree.links.new(on_init.outputs["Out"], flow_input(typed_loop_from_list))
    tree.links.new(list_from_items.outputs["Floats"], typed_loop_from_list.inputs["List"])
    tree.links.new(typed_loop_from_list.outputs["Loop"], modify_typed_sum.inputs["Flow"])
    tree.links.new(typed_loop_from_list.outputs["Value"], modify_typed_sum.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_list_dup_len))
    tree.links.new(make_list.outputs["List"], list_duplicate.inputs["List"])
    tree.links.new(list_duplicate.outputs["List"], list_dup_length.inputs["List"])
    tree.links.new(list_dup_length.outputs["Length"], set_list_dup_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_merged_dict_len))
    tree.links.new(dict_extra_value.outputs["Value"], make_dict_extra.inputs["Value"])
    tree.links.new(make_dict.outputs["Dictionary"], dict_merge.inputs["Dictionary A"])
    tree.links.new(make_dict_extra.outputs["Dictionary"], dict_merge.inputs["Dictionary B"])
    tree.links.new(dict_merge.outputs["Dictionary"], merged_dict_length.inputs["Dictionary"])
    tree.links.new(merged_dict_length.outputs["Length"], set_merged_dict_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_list_item_value))
    tree.links.new(make_list.outputs["List"], list_get_item.inputs["List"])
    tree.links.new(list_index.outputs["Value"], list_get_item.inputs["Index"])
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

    tree.links.new(sum_item_a.outputs["Value"], sum_list.inputs["Item A"])
    tree.links.new(sum_item_b.outputs["Value"], sum_list.inputs["Item B"])
    tree.links.new(sum_item_c.outputs["Value"], sum_list.inputs["Item C"])
    tree.links.new(on_init.outputs["Out"], flow_input(loop_from_list))
    tree.links.new(sum_list.outputs["List"], loop_from_list.inputs["List"])
    tree.links.new(loop_from_list.outputs["Loop"], modify_sum.inputs["Flow"])
    tree.links.new(loop_from_list.outputs["Value"], modify_sum.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_list_len))
    tree.links.new(item_a.outputs["Value"], make_list.inputs["Item A"])
    tree.links.new(item_b.outputs["Value"], make_list.inputs["Item B"])
    tree.links.new(item_c.outputs["Value"], make_list.inputs["Item C"])
    tree.links.new(make_list.outputs["List"], list_length.inputs["List"])
    tree.links.new(list_length.outputs["Length"], set_list_len.inputs["Value"])

    tree.links.new(on_init.outputs["Out"], flow_input(set_dict_len))
    tree.links.new(dict_value.outputs["Value"], make_dict.inputs["Value"])
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
    tree.links.new(modify_loop.outputs["Out"], flow_input(set_loop_done))
    return tree


def build_time_state_tree(name):
    """Drive the time nodes with short second values and record their elapsed outputs."""
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
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
    tree.links.new(next_frame.outputs["Out"], flow_input(setters["next_frame"]))
    tree.links.new(on_update.outputs["Out"], delay.inputs["Flow"])
    tree.links.new(delay.outputs["Done"], flow_input(setters["delay"]))
    tree.links.new(on_init.outputs["Out"], timer.inputs["Set Timer"])
    tree.links.new(timer.outputs["When Elapsed"], flow_input(setters["timer"]))
    tree.links.new(on_update.outputs["Out"], pulsify.inputs["Flow"])
    tree.links.new(pulsify.outputs["Pulse"], flow_input(setters["pulsify"]))
    tree.links.new(on_update.outputs["Out"], barrier.inputs["Flow"])
    tree.links.new(barrier.outputs["Out"], flow_input(setters["barrier"]))
    tree.links.new(on_update.outputs["Out"], flow_input(set_time))
    tree.links.new(time_node.outputs["Time"], set_time.inputs["Value"])
    tree.links.new(on_update.outputs["Out"], flow_input(set_delta))
    tree.links.new(time_node.outputs["Delta"], set_delta.inputs["Value"])
    return tree


def build_scene_state_tree(name, gravity, timescale):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
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

    for node in (set_gravity, set_timescale):
        tree.links.new(on_update.outputs["Out"], flow_input(node))
    for node in (set_gravity_x, set_gravity_y, set_gravity_z, set_timescale_property):
        tree.links.new(on_update.outputs["Out"], flow_input(node))

    tree.links.new(get_gravity.outputs["Gravity"], separate_gravity.inputs["Vector"])
    tree.links.new(separate_gravity.outputs["X"], set_gravity_x.inputs["Value"])
    tree.links.new(separate_gravity.outputs["Y"], set_gravity_y.inputs["Value"])
    tree.links.new(separate_gravity.outputs["Z"], set_gravity_z.inputs["Value"])
    tree.links.new(get_timescale.outputs["Timescale"], set_timescale_property.inputs["Value"])
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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
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
        tree.links.new(on_update.outputs["Out"], flow_input(node))

    tree.links.new(get_group.outputs["Layers"], set_group_property.inputs["Value"])
    return tree


def build_character_state_tree(name, target_obj, gravity, walk_direction, velocity, velocity_time,
                               max_jumps, jump_speed):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    once = tree.nodes.new("LogicNativeOnce")
    on_next_frame = tree.nodes.new("LogicNativeOnNextFrame")
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

    set_gravity_x.inputs["Property"].default_value = "character_gravity_x"
    set_gravity_y.inputs["Property"].default_value = "character_gravity_y"
    set_gravity_z.inputs["Property"].default_value = "character_gravity_z"
    set_walk_x.inputs["Property"].default_value = "character_walk_x"
    set_walk_y.inputs["Property"].default_value = "character_walk_y"
    set_walk_z.inputs["Property"].default_value = "character_walk_z"
    set_max_jumps_property.inputs["Property"].default_value = "character_max_jumps"
    set_jump_count_property.inputs["Property"].default_value = "character_jump_count"
    set_on_ground_property.inputs["Property"].default_value = "character_on_ground"

    tree.links.new(on_update.outputs["Out"], once.inputs["Flow"])
    tree.links.new(once.outputs["Out"], on_next_frame.inputs["Flow"])
    tree.links.new(on_next_frame.outputs["Out"], flow_input(character_jump))

    for node in (
        set_gravity,
        set_jump_speed,
        set_max_jumps,
        set_walk_direction,
        set_velocity,
    ):
        tree.links.new(on_update.outputs["Out"], flow_input(node))

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
        tree.links.new(on_update.outputs["Out"], flow_input(node))

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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    add_object = tree.nodes.new("LogicNativeAddObject")
    add_object.inputs["Object to Add"].default_value = template_obj
    add_object.inputs["Copy Transform"].default_value = transform_obj
    add_object.inputs["Life"].default_value = 0.0
    add_object.inputs["Full Copy"].default_value = full_copy
    tree.links.new(on_update.outputs["Out"], add_object.inputs["Flow"])
    return tree


def build_add_object_result_move_tree(name, template_obj, transform_obj, final_position):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    add_object = tree.nodes.new("LogicNativeAddObject")
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")
    add_object.inputs["Object to Add"].default_value = template_obj
    add_object.inputs["Copy Transform"].default_value = transform_obj
    add_object.inputs["Life"].default_value = 0.0
    add_object.inputs["Full Copy"].default_value = False
    set_position.inputs["Position"].default_value = final_position
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
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")
    add_object = tree.nodes.new("LogicNativeAddObject")
    set_position.inputs["Object"].default_value = transform_obj
    set_position.inputs["Position"].default_value = final_position
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


def build_set_parent_tree(name, child_obj, parent_obj, compound=False, ghost=False):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_parent = tree.nodes.new("LogicNativeSetParent")
    set_parent.inputs["Child Object"].default_value = child_obj
    set_parent.inputs["Parent Object"].default_value = parent_obj
    set_parent.inputs["Compound"].default_value = compound
    set_parent.inputs["Ghost"].default_value = ghost
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


def build_tier_a_tree(name, target_name, mover):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")

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

    append_one.outputs["Value"].default_value = 1.0
    append_two.outputs["Value"].default_value = 2.0
    dict_value.outputs["Value"].default_value = 3.0
    dict_set.inputs["Key"].default_value = "tier_a"
    global_value.outputs["Value"].default_value = 99.0
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
    translate.inputs["Speed"].default_value = 0.5
    translate.inputs["Local"].default_value = False

    tree.links.new(empty_list.outputs["List"], list_append_a.inputs["List"])
    tree.links.new(append_one.outputs["Value"], list_append_a.inputs["Value"])
    tree.links.new(list_append_a.outputs["List"], list_append_b.inputs["List"])
    tree.links.new(append_two.outputs["Value"], list_append_b.inputs["Value"])
    tree.links.new(list_append_b.outputs["List"], list_length.inputs["List"])
    tree.links.new(list_length.outputs["Length"], set_list_len.inputs["Value"])

    tree.links.new(empty_dict.outputs["Dictionary"], dict_set.inputs["Dictionary"])
    tree.links.new(dict_value.outputs["Value"], dict_set.inputs["Value"])
    tree.links.new(dict_set.outputs["Dictionary"], dict_length.inputs["Dictionary"])
    tree.links.new(dict_length.outputs["Length"], set_dict_len.inputs["Value"])

    tree.links.new(global_value.outputs["Value"], set_global.inputs["Value"])
    tree.links.new(get_global.outputs["Value"], set_global_read.inputs["Value"])

    tree.links.new(object_by_name.outputs["Object"], found_branch.inputs["Value"])
    tree.links.new(get_owner.outputs["Owner Object"], owner_branch.inputs["Value"])

    for node in (
        set_list_len,
        set_dict_len,
        set_global,
        set_global_read,
        found_flow_branch,
        owner_flow_branch,
    ):
        tree.links.new(on_init.outputs["Out"], flow_input(node))

    tree.links.new(found_branch.outputs["If Not None"], found_flow_branch.inputs["Condition"])
    tree.links.new(owner_branch.outputs["If Not None"], owner_flow_branch.inputs["Condition"])
    tree.links.new(found_flow_branch.outputs["True"], flow_input(set_found))
    tree.links.new(owner_flow_branch.outputs["True"], flow_input(set_owner_ok))
    tree.links.new(on_update.outputs["Out"], flow_input(translate))
    return tree


def build_raycast_tree(name):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    raycast = tree.nodes.new("LogicNativeRaycast")
    point = tree.nodes.new("LogicNativeSeparateXYZ")
    normal = tree.nodes.new("LogicNativeSeparateXYZ")
    direction = tree.nodes.new("LogicNativeSeparateXYZ")
    object_hit = tree.nodes.new("LogicNativeNotNone")
    hit_branch = tree.nodes.new("LogicNativeBranch")
    object_branch = tree.nodes.new("LogicNativeBranch")
    set_hit = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_object = tree.nodes.new("LogicNativeSetGamePropertyBool")

    raycast.inputs["Origin"].default_value = (0.0, 0.0, 5.0)
    raycast.inputs["Destination"].default_value = (0.0, 0.0, -5.0)
    raycast.inputs["Local"].default_value = False
    raycast.inputs["Mask"].default_value = 0

    set_hit.inputs["Property"].default_value = "ray_hit"
    set_hit.inputs["Value"].default_value = True
    set_object.inputs["Property"].default_value = "ray_object"
    set_object.inputs["Value"].default_value = True

    tree.links.new(raycast.outputs["Point"], point.inputs["Vector"])
    tree.links.new(raycast.outputs["Normal"], normal.inputs["Vector"])
    tree.links.new(raycast.outputs["Direction"], direction.inputs["Vector"])
    tree.links.new(raycast.outputs["Picked Object"], object_hit.inputs["Value"])
    tree.links.new(on_update.outputs["Out"], flow_input(hit_branch))
    tree.links.new(on_update.outputs["Out"], flow_input(object_branch))
    tree.links.new(raycast.outputs["Has Result"], hit_branch.inputs["Condition"])
    tree.links.new(object_hit.outputs["If Not None"], object_branch.inputs["Condition"])
    tree.links.new(hit_branch.outputs["True"], flow_input(set_hit))
    tree.links.new(object_branch.outputs["True"], flow_input(set_object))

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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    move_toward = tree.nodes.new("LogicNativeMoveToward")

    move_toward.inputs["Target Position"].default_value = target_position
    move_toward.inputs["Speed"].default_value = 4.0
    move_toward.inputs["Stop Distance"].default_value = 0.1
    if target_obj is not None:
        move_toward.inputs["Object"].default_value = target_obj

    tree.links.new(on_update.outputs["Out"], flow_input(move_toward))
    return tree


def build_navigate_visualize_tree(name, target_position, target_obj=None):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    navigate = tree.nodes.new("LogicNativeNavigate")
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")

    navigate.inputs["Destination"].default_value = target_position
    navigate.inputs["Move as Dynamic"].default_value = False
    navigate.inputs["Lin Speed"].default_value = 0.25
    navigate.inputs["Reach Threshold"].default_value = 0.05
    navigate.inputs["Look At"].default_value = False
    navigate.inputs["Visualize"].default_value = True
    if target_obj is not None:
        navigate.inputs["Moving Object"].default_value = target_obj
        set_position.inputs["Object"].default_value = target_obj

    tree.links.new(on_update.outputs["Out"], flow_input(navigate))
    tree.links.new(navigate.outputs["Done"], flow_input(set_position))
    tree.links.new(navigate.outputs["Next Point"], set_position.inputs["Position"])
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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    set_position = tree.nodes.new("LogicNativeSetWorldPosition")
    get_attribute = tree.nodes.new("LogicNativeGetObjectAttribute")
    separate = tree.nodes.new("LogicNativeSeparateXYZ")
    set_x = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_y = tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_z = tree.nodes.new("LogicNativeSetGamePropertyFloat")

    set_position.inputs["Position"].default_value = expected_position
    set_x.inputs["Property"].default_value = "read_x"
    set_y.inputs["Property"].default_value = "read_y"
    set_z.inputs["Property"].default_value = "read_z"

    tree.links.new(on_init.outputs["Out"], flow_input(set_position))
    tree.links.new(on_update.outputs["Out"], flow_input(set_x))
    tree.links.new(on_update.outputs["Out"], flow_input(set_y))
    tree.links.new(on_update.outputs["Out"], flow_input(set_z))
    tree.links.new(get_attribute.outputs["Vector"], separate.inputs["Vector"])
    tree.links.new(separate.outputs["X"], set_x.inputs["Value"])
    tree.links.new(separate.outputs["Y"], set_y.inputs["Value"])
    tree.links.new(separate.outputs["Z"], set_z.inputs["Value"])
    return tree


def build_rotate_toward_tree(name, target_position, target_obj=None):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    rotate_toward = tree.nodes.new("LogicNativeRotateToward")

    rotate_toward.inputs["Target"].default_value = target_position
    rotate_toward.inputs["Factor"].default_value = 0.35
    rotate_toward.inputs["Rot Axis"].default_value = 2
    rotate_toward.inputs["Front Axis"].default_value = 1
    if target_obj is not None:
        rotate_toward.inputs["Object"].default_value = target_obj

    tree.links.new(on_update.outputs["Out"], flow_input(rotate_toward))
    return tree


def build_collision_tree(name):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
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

    tree.links.new(on_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(collision.outputs["Colliding"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_collided))
    tree.links.new(collision.outputs["Collided Object"], typecast_object.inputs["Value"])
    tree.links.new(on_update.outputs["Out"], flow_input(set_object_name))
    tree.links.new(typecast_object.outputs["String"], set_object_name.inputs["Value"])
    tree.links.new(collision.outputs["Point"], separate_point.inputs["Vector"])
    tree.links.new(collision.outputs["Normal"], separate_normal.inputs["Vector"])
    for socket_name, property_name in (("X", "collision_point_x"),
                                      ("Y", "collision_point_y"),
                                      ("Z", "collision_point_z")):
        setter = float_setters[property_name]
        tree.links.new(on_update.outputs["Out"], flow_input(setter))
        tree.links.new(separate_point.outputs[socket_name], setter.inputs["Value"])
    for socket_name, property_name in (("X", "collision_normal_x"),
                                      ("Y", "collision_normal_y"),
                                      ("Z", "collision_normal_z")):
        setter = float_setters[property_name]
        tree.links.new(on_update.outputs["Out"], flow_input(setter))
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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
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
    tree.links.new(on_update.outputs["Out"], branch.inputs["Flow"])
    tree.links.new(animation_status.outputs["Is Playing"], branch.inputs["Condition"])
    tree.links.new(branch.outputs["True"], flow_input(set_playing))
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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    align_axis = tree.nodes.new("LogicNativeAlignAxisToVector")

    align_axis.inputs["Vector"].default_value = (1.0, 0.0, 0.0)
    align_axis.inputs["Axis"].default_value = 2
    align_axis.inputs["Factor"].default_value = 0.35
    if target_obj is not None:
        align_axis.inputs["Object"].default_value = target_obj

    tree.links.new(on_update.outputs["Out"], flow_input(align_axis))
    return tree


def build_slow_follow_tree(name, target_obj, follower_obj=None):
    tree = new_logic_tree(name)
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    slow_follow = tree.nodes.new("LogicNativeSlowFollow")

    slow_follow.inputs["Target"].default_value = target_obj
    slow_follow.inputs["Factor"].default_value = 0.25
    if follower_obj is not None:
        slow_follow.inputs["Object"].default_value = follower_obj

    tree.links.new(on_update.outputs["Out"], flow_input(slow_follow))
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


def build_mouse_over_tree(name, target_obj):
    tree = new_logic_tree(name)
    on_init = tree.nodes.new("LogicNativeOnInit")
    set_cursor = tree.nodes.new("LogicNativeSetCursorPosition")
    mouse_over = tree.nodes.new("LogicNativeMouseOver")
    point = tree.nodes.new("LogicNativeSeparateXYZ")
    normal = tree.nodes.new("LogicNativeSeparateXYZ")
    set_enter = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_over = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_exit = tree.nodes.new("LogicNativeSetGamePropertyBool")

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

    tree.links.new(on_init.outputs["Out"], flow_input(set_cursor))
    tree.links.new(mouse_over.outputs["Point"], point.inputs["Vector"])
    tree.links.new(mouse_over.outputs["Normal"], normal.inputs["Vector"])
    tree.links.new(mouse_over.outputs["On Enter"], flow_input(set_enter))
    tree.links.new(mouse_over.outputs["On Over"], flow_input(set_over))
    tree.links.new(mouse_over.outputs["On Exit"], flow_input(set_exit))

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
    if result.returncode != 0:
        output = result.stdout[-4000:] if result.stdout else ""
        raise RuntimeError(f"blenderplayer failed for {run_id} with {result.returncode}:\n{output}")


def run_game_once(obj_or_name, run_id, status_path):
    obj = resolve_live_object(obj_or_name)
    obj_name = obj.name
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
    bind_tree(target, tree.name, True)
    run_game_once(target, "logic_gate_0", status_path)


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
        add_status_controller(target, case_status_path, "velocity", tolerance=0.2)
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


def run_property_branch_acceptance(status_path, output_dir):
    expected = (0.0, 4.0, 0.0)

    def build_case(case_status_path):
        tree = build_property_branch_tree(f"{PREFIX}_PropertyBranchTree", expected)
        target = create_cube(f"{PREFIX}_PropertyBranch")
        add_status_controller(target, case_status_path, "property_branch")
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
    target = create_cube(f"{PREFIX}_PropertyTarget", (2.0, 0.0, 0.0))
    tree = build_property_target_tree(f"{PREFIX}_PropertyTargetTree", target)
    add_status_controller(owner, status_path, "property_target", required_frame=2)
    owner["__ln_acceptance_property_target"] = target.name
    owner["__ln_acceptance_expected_score"] = 42
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
    set_game_property(armature, "bone_length", "FLOAT", 0.0)
    set_game_property(armature, "bone_head_z", "FLOAT", 0.0)
    set_game_property(armature, "bone_center_z", "FLOAT", 0.0)
    bind_tree(armature, tree.name, True)
    run_game_once(armature, "bone_read_0", status_path)


def run_bone_pose_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    armature = create_armature(f"{PREFIX}_ArmaturePose")
    tree = build_bone_pose_tree(f"{PREFIX}_BonePoseTree", armature)
    add_status_controller(armature, status_path, "bone_pose", required_frame=3)
    set_game_property(armature, "bone_rest_head_z", "FLOAT", 0.0)
    set_game_property(armature, "bone_pose_head_z", "FLOAT", 0.0)
    bind_tree(armature, tree.name, True)
    run_game_once(armature, "bone_pose_0", status_path)


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
    gravity = (1.25, -2.5, -3.75)
    timescale = 1.25
    tree = build_scene_state_tree(f"{PREFIX}_SceneStateTree", gravity, timescale)
    target = create_cube(f"{PREFIX}_SceneState")
    add_status_controller(target, status_path, "scene_state", required_frame=4)
    set_expected_vector(target, gravity)
    set_game_property(target, "__ln_acceptance_expected_timescale", "FLOAT", timescale)
    for name in ("gravity_x", "gravity_y", "gravity_z", "timescale"):
        set_game_property(target, name, "FLOAT", 0.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "scene_state_0", status_path)


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
    set_parent_tree = build_set_parent_tree(f"{PREFIX}_SetParentTree", child, parent, False, False)
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
    bind_tree(probe, tree.name, True)
    run_game_once(probe, "tier_a_0", status_path)


def run_raycast_acceptance(status_path):
    reset_scene()
    configure_game_settings(True)
    create_cube(f"{PREFIX}_RaycastTarget")
    probe = create_cube(f"{PREFIX}_RaycastProbe")
    probe.location = (4.0, 0.0, 0.0)
    tree = build_raycast_tree(f"{PREFIX}_RaycastTree")

    add_status_controller(probe, status_path, "raycast", required_frame=2, tolerance=0.02)
    set_expected_named_vector(probe, "hit_point", (0.0, 0.0, 1.0))
    set_expected_named_vector(probe, "hit_normal", (0.0, 0.0, 1.0))
    set_expected_named_vector(probe, "hit_direction", (0.0, 0.0, -1.0))

    set_game_property(probe, "ray_hit", "BOOL", False)
    set_game_property(probe, "ray_object", "BOOL", False)
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
    ):
        set_game_property(probe, property_name, "FLOAT", 0.0)

    bind_tree(probe, tree.name, True)
    run_game_once(probe, "raycast_0", status_path)


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
    target = create_cube(f"{PREFIX}_Phase6AnimationStatus")
    action = create_test_action(f"{PREFIX}_Phase6Action")
    tree = build_animation_status_tree(f"{PREFIX}_Phase6AnimationStatusTree", action)
    add_status_controller(target, status_path, "phase6_animation_status", required_frame=6)
    set_game_property(target, "action_playing", "BOOL", False)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_animation_status_0", status_path)


def run_phase6_save_load_acceptance(status_path, output_dir):
    reset_scene()
    configure_game_settings(True)
    target = create_cube(f"{PREFIX}_Phase6SaveLoad")
    save_path = str(output_dir / f"{PREFIX}_Phase6SaveLoad.sav")
    save_file = Path(save_path)
    if save_file.exists():
        save_file.unlink()

    tree = new_logic_tree(f"{PREFIX}_Phase6SaveLoadTree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    save_game = tree.nodes.new("LogicNativeSaveGame")
    load_game = tree.nodes.new("LogicNativeLoadGame")
    save_game.inputs["Path"].default_value = save_path
    load_game.inputs["Path"].default_value = save_path
    tree.links.new(on_init.outputs["Out"], flow_input(save_game))
    tree.links.new(on_update.outputs["Out"], flow_input(load_game))

    add_status_controller(target, status_path, "phase6_save_load", required_frame=2)
    set_game_property(target, "score", "FLOAT", 42.0)
    set_game_property(target, "__ln_acceptance_expected_score", "FLOAT", 42.0)
    bind_tree(target, tree.name, True)
    run_game_once(target, "phase6_save_load_0", status_path)
    require(save_file.exists(), f"Save Game did not create {save_path}")
    require("score" in save_file.read_bytes().decode("latin1", errors="ignore"),
            "Save Game file is missing the score property")


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
    on_update = tree.nodes.new("LogicNativeOnUpdate")
    keyboard = tree.nodes.new("LogicNativeKeyboardKey")
    mouse = tree.nodes.new("LogicNativeMouseButton")
    gamepad = tree.nodes.new("LogicNativeGamepadActive")
    branch_keyboard = tree.nodes.new("LogicNativeBranch")
    branch_mouse = tree.nodes.new("LogicNativeBranch")
    branch_gamepad = tree.nodes.new("LogicNativeBranch")
    set_keyboard = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_mouse = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_gamepad = tree.nodes.new("LogicNativeSetGamePropertyBool")

    keyboard.inputs["Key"].default_value = "SPACE"
    mouse.inputs["Button"].default_value = "LEFTMOUSE"
    gamepad.inputs["Index"].default_value = 0
    for node, property_name in (
        (set_keyboard, "keyboard_checked"),
        (set_mouse, "mouse_checked"),
        (set_gamepad, "gamepad_checked"),
    ):
        node.inputs["Property"].default_value = property_name
        node.inputs["Value"].default_value = True

    tree.links.new(on_update.outputs["Out"], branch_keyboard.inputs["Flow"])
    tree.links.new(on_update.outputs["Out"], branch_mouse.inputs["Flow"])
    tree.links.new(on_update.outputs["Out"], branch_gamepad.inputs["Flow"])
    tree.links.new(keyboard.outputs["Active"], branch_keyboard.inputs["Condition"])
    tree.links.new(mouse.outputs["Active"], branch_mouse.inputs["Condition"])
    tree.links.new(gamepad.outputs["Active"], branch_gamepad.inputs["Condition"])
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

    value_output(value_a).default_value = 2.0
    value_output(value_b).default_value = 3.0
    math_node.operation = "ADD"
    set_result.inputs["Property"].default_value = "math_result"

    tree.links.new(on_init.outputs["Out"], flow_input(set_result))
    tree.links.new(value_output(value_a), math_node.inputs["A"])
    tree.links.new(value_output(value_b), math_node.inputs["B"])
    tree.links.new(math_node.outputs["Result"], set_result.inputs["Value"])
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
    on_update = tree.nodes.new("LogicNativeOnUpdate")

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
    projectile.inputs["Mask"].default_value = 65535
    set_projectile_hit = tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_projectile_hit.inputs["Property"].default_value = "projectile_hit"

    for setter in (set_formula, set_tween, set_projectile_hit):
        tree.links.new(on_update.outputs["Out"], flow_input(setter))
    tree.links.new(on_update.outputs["Out"], flow_input(projectile))
    tree.links.new(on_update.outputs["Out"], flow_input(eval_branch))

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
    tree = build_mouse_over_tree(f"{PREFIX}_MouseOverTree", target)

    add_status_controller(probe, status_path, "mouse_over", required_frame=3, tolerance=0.05)
    set_expected_named_vector(probe, "mouse_point", (0.0, -1.0, 0.0))
    set_expected_named_vector(probe, "mouse_normal", (0.0, -1.0, 0.0))

    set_game_property(probe, "mouse_enter", "BOOL", False)
    set_game_property(probe, "mouse_over", "BOOL", False)
    set_game_property(probe, "mouse_exit", "BOOL", False)
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
        ("euler", lambda: run_euler_acceptance(status_path, output_dir)),
        ("orientation", lambda: run_orientation_acceptance(status_path, output_dir)),
        ("velocity", lambda: run_velocity_acceptance(status_path, output_dir)),
        ("impulse", lambda: run_impulse_acceptance(status_path)),
        ("object_actions", lambda: run_object_actions_acceptance(status_path, output_dir)),
        ("property_branch", lambda: run_property_branch_acceptance(status_path, output_dir)),
        ("property_writes", lambda: run_property_write_acceptance(status_path)),
        ("property_target", lambda: run_property_target_acceptance(status_path)),
        ("property_advanced", lambda: run_property_advanced_acceptance(status_path)),
        ("tree_properties", lambda: run_tree_property_acceptance(status_path)),
        ("data_containers", lambda: run_data_containers_acceptance(status_path)),
        ("bone_read", lambda: run_bone_read_acceptance(status_path)),
        ("bone_pose", lambda: run_bone_pose_acceptance(status_path)),
        ("input_nodes", lambda: run_input_nodes_acceptance(status_path)),
        ("values_math", lambda: run_values_math_acceptance(status_path)),
        ("c_tier_values", lambda: run_c_tier_values_acceptance(status_path)),
        ("sound_state", lambda: run_sound_state_acceptance(status_path)),
        ("sound_advanced", lambda: run_sound_advanced_acceptance(status_path)),
        ("vehicle_control_compile", lambda: run_vehicle_control_compile_acceptance(status_path)),
        ("time_state", lambda: run_time_state_acceptance(status_path)),
        ("scene_state", lambda: run_scene_state_acceptance(status_path)),
        ("camera_state", lambda: run_camera_state_acceptance(status_path)),
        ("render_state", lambda: run_render_state_acceptance(status_path)),
        ("light_state", lambda: run_light_state_acceptance(status_path)),
        ("light_unique", lambda: run_light_unique_acceptance(status_path)),
        ("physics_state", lambda: run_physics_state_acceptance(status_path)),
        ("character_state", lambda: run_character_state_acceptance(status_path)),
        ("object_lifecycle", lambda: run_object_lifecycle_acceptance(status_path)),
        ("object_queries", lambda: run_object_query_acceptance(status_path)),
        ("tier_a", lambda: run_tier_a_acceptance(status_path)),
        ("raycast", lambda: run_raycast_acceptance(status_path)),
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
        ("phase6_save_load", lambda: run_phase6_save_load_acceptance(status_path, output_dir)),
        ("phase6_align_axis", lambda: run_phase6_align_axis_acceptance(status_path)),
        ("phase6_slow_follow", lambda: run_phase6_slow_follow_acceptance(status_path)),
        ("phase6_replace_mesh", lambda: run_phase6_replace_mesh_acceptance(status_path)),
        ("material_shader_node_input", lambda: run_material_shader_node_input_acceptance(status_path)),
        ("mouse_over", lambda: run_mouse_over_acceptance(status_path)),
        ("disabled_binding", lambda: run_disabled_binding_probe(status_path)),
        ("missing_tree", lambda: run_missing_tree_probe(status_path)),
        ("variable_timestep", lambda: run_variable_timestep_probe(status_path)),
        ("b_tier_core", lambda: run_b_tier_core_acceptance(status_path)),
        ("phase10_service_blend_load",
         lambda: run_phase10_service_blend_load_acceptance(status_path)),
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
