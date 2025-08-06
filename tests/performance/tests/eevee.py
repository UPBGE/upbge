# SPDX-FileCopyrightText: 2022 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import enum
import time


class RecordStage(enum.Enum):
    INIT = 0,
    WAIT_SHADERS = 1,
    WARMUP = 2,
    RECORD = 3,
    FINISHED = 4


WARMUP_SECONDS = 3
WARMUP_FRAMES = 10
SHADER_FALLBACK_SECONDS = 60
RECORD_PLAYBACK_ITER = 3
MIN_NUM_FRAMES_TOTAL = 250
LOG_KEY = "ANIMATION_PERFORMANCE: "


def _run(args):
    import bpy

    global record_stage
    record_stage = RecordStage.INIT

    bpy.app.handlers.frame_change_post.append(frame_change_handler)
    bpy.ops.screen.animation_play()


def frame_change_handler(scene):
    import bpy

    global record_stage
    global frame_set_mode
    global start_time
    global start_record_time
    global start_warmup_time
    global warmup_frame
    global stop_record_time
    global playback_iteration
    global num_frames

    if record_stage == RecordStage.INIT:
        screen = bpy.context.window_manager.windows[0].screen
        bpy.context.scene.sync_mode = 'NONE'
        frame_set_mode = False

        for area in screen.areas:
            if area.type == 'VIEW_3D':
                space = area.spaces[0]
                space.shading.type = 'RENDERED'
                space.overlay.show_overlays = False

        start_time = time.perf_counter()
        record_stage = RecordStage.WAIT_SHADERS

    elif record_stage == RecordStage.WAIT_SHADERS:
        shaders_compiled = False
        if hasattr(bpy.app, 'is_job_running'):
            shaders_compiled = not bpy.app.is_job_running("SHADER_COMPILATION")
        else:
            # Fallback when is_job_running doesn't exists by waiting for a time.
            shaders_compiled = time.perf_counter() - start_time > SHADER_FALLBACK_SECONDS

        if shaders_compiled:
            start_warmup_time = time.perf_counter()
            warmup_frame = 0
            record_stage = RecordStage.WARMUP

    elif record_stage == RecordStage.WARMUP:
        if frame_set_mode:
            # scene.frame_set results in a recursive call to frame_change_handler.
            # Avoid running into a RecursionError.
            return
        warmup_frame += 1
        # Check for two-stage shader compilation that can happen later than the first frame.
        if hasattr(bpy.app, 'is_job_running') and bpy.app.is_job_running("SHADER_COMPILATION"):
            record_stage = RecordStage.WAIT_SHADERS
        elif time.perf_counter() - start_warmup_time > WARMUP_SECONDS and warmup_frame > WARMUP_FRAMES:
            start_record_time = time.perf_counter()
            playback_iteration = 0
            num_frames = 0
            scene = bpy.context.scene
            frame_set_mode = True
            scene.frame_set(scene.frame_start)
            frame_set_mode = False
            record_stage = RecordStage.RECORD

    elif record_stage == RecordStage.RECORD:
        current_time = time.perf_counter()
        scene = bpy.context.scene
        num_frames += 1
        if scene.frame_current == scene.frame_end:
            playback_iteration += 1

        if playback_iteration >= RECORD_PLAYBACK_ITER and num_frames >= MIN_NUM_FRAMES_TOTAL:
            stop_record_time = current_time
            record_stage = RecordStage.FINISHED

    elif record_stage == RecordStage.FINISHED:
        bpy.ops.screen.animation_cancel()
        elapsed_seconds = stop_record_time - start_record_time
        avg_frame_time = elapsed_seconds / num_frames
        fps = 1.0 / avg_frame_time
        print(f"{LOG_KEY}{{'time': {avg_frame_time}, 'fps': {fps} }}")
        bpy.app.handlers.frame_change_post.remove(frame_change_handler)
        bpy.ops.wm.quit_blender()


if __name__ == '__main__':
    _run(None)

else:
    import api

    class EeveeTest(api.Test):
        def __init__(self, filepath):
            self.filepath = filepath

        def name(self):
            return self.filepath.stem

        def category(self):
            return "eevee"

        def use_background(self):
            return False

        def run(self, env, device_id):
            args = {}
            _, log = env.run_in_blender(_run, args, [self.filepath], foreground=True)
            for line in log:
                if line.startswith(LOG_KEY):
                    result_str = line[len(LOG_KEY):]
                    result = eval(result_str)
                    return result

            raise Exception("No playback performance result found in log.")

    def generate(env):
        filepaths = env.find_blend_files('eevee/*')
        return [EeveeTest(filepath) for filepath in filepaths]
