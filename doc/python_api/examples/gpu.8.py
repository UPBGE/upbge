"""
Copy Offscreen Rendering result back to RAM
-------------------------------------------

This will create a new image with the given name.
If it already exists, it will override the existing one.

Currently almost all of the execution time is spent in the last line.
In the future this will hopefully be solved by implementing the Python buffer protocol
for :class:`gpu.types.Buffer` and :class:`bpy.types.Image.pixels` (aka ``bpy_prop_array``).
"""
import bpy
import gpu
import random
from mathutils import Matrix
from gpu_extras.presets import draw_circle_2d

IMAGE_NAME = "Generated Image"
WIDTH = 512
HEIGHT = 512
RING_AMOUNT = 10


offscreen = gpu.types.GPUOffScreen(WIDTH, HEIGHT)

with offscreen.bind():
    fb = gpu.state.active_framebuffer_get()
    fb.clear(color=(0.0, 0.0, 0.0, 0.0))
    with gpu.matrix.push_pop():
        # reset matrices -> use normalized device coordinates [-1, 1]
        gpu.matrix.load_matrix(Matrix.Identity(4))
        gpu.matrix.load_projection_matrix(Matrix.Identity(4))

        for i in range(RING_AMOUNT):
            draw_circle_2d(
                (random.uniform(-1, 1), random.uniform(-1, 1)),
                (1, 1, 1, 1), random.uniform(0.1, 1),
                segments=20,
            )

    buffer = fb.read_color(0, 0, WIDTH, HEIGHT, 4, 0, 'UBYTE')

offscreen.free()


if IMAGE_NAME not in bpy.data.images:
    bpy.data.images.new(IMAGE_NAME, WIDTH, HEIGHT)
image = bpy.data.images[IMAGE_NAME]
image.scale(WIDTH, HEIGHT)

buffer.dimensions = WIDTH * HEIGHT * 4
image.pixels = [v / 255 for v in buffer]
