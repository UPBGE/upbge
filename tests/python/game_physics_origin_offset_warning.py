# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Collision Bounds panel warns when a Box, Sphere, Cylinder, Cone or Capsule shape sits away
from the mesh (2026-11-18).

Both engines' converters build those shapes from the size of the mesh's bounding box alone,
centred on the object's origin: a mesh whose middle is away from its origin (a character whose
origin is at its feet) gets a shape that does not sit where the mesh is. The panel says so, how
far (the object's scale included), and offers Origin to Geometry (bounds centre). Never for a
Convex Hull or Triangle Mesh (the mesh's own points), a centred mesh or an offset under 1% of its
size; the shape the game builds is judged (a Character without chosen bounds is a capsule in
Jolt, a sphere in Bullet).

The real panel's draw runs with a layout that records every label and operator (no window).

    blender --background --factory-startup --python tests/python/game_physics_origin_offset_warning.py
"""

import sys

import bpy
from bl_ui import properties_game

PROBLEMS = []
WARNING = "centred on the object's origin"


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


class Recorder:
    """A layout that accepts every call, keeping the labels' texts and the operators' names."""

    def __init__(self, record):
        self.__dict__["record"] = record

    def __getattr__(self, name):
        def call(*args, **kwargs):
            if name == "label" and "text" in kwargs:
                self.record.append(("label", kwargs["text"]))
            if name == "operator" and args:
                self.record.append(("operator", args[0]))
            return Recorder(self.record)
        return call

    def __setattr__(self, name, value):
        pass


def panel_record(ob):
    """What PHYSICS_PT_game_collision_bounds draws for `ob`."""
    cls = properties_game.PHYSICS_PT_game_collision_bounds
    methods = {}
    for klass in reversed(cls.__mro__):
        for name, value in vars(klass).items():
            if isinstance(value, staticmethod) or (callable(value) and not name.startswith("__")):
                methods[name] = value
    fake = type("FakeCollisionBoundsPanel", (), methods)()
    record = []
    fake.layout = Recorder(record)
    bpy.context.view_layer.objects.active = ob
    with bpy.context.temp_override(object=ob, active_object=ob):
        cls.draw(fake, bpy.context)
    return record


def warning(ob):
    """The warning's first line, or None."""
    for kind, text in panel_record(ob):
        if kind == "label" and WARNING in text:
            return text
    return None


def offers_origin_to_geometry(ob):
    return ("operator", "object.origin_set") in panel_record(ob)


def box_mesh(name, low, high):
    mesh = bpy.data.meshes.new(name)
    (x0, y0, z0), (x1, y1, z1) = low, high
    verts = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
             (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    mesh.from_pydata(verts, [], faces)
    return mesh


def make(name, low, high, physics, bounds=None, scale=(1.0, 1.0, 1.0)):
    ob = bpy.data.objects.new(name, box_mesh(name, low, high))
    bpy.context.scene.collection.objects.link(ob)
    ob.scale = scale
    ob.game.physics_type = physics
    if bounds is not None:
        ob.game.collision_bounds_type = bounds
    bpy.context.view_layer.update()
    return ob


def main():
    scene = bpy.context.scene
    for engine in ('JOLT', 'BULLET'):
        scene.game_settings.physics_engine = engine
        is_jolt = engine == 'JOLT'
        # A centred mesh: no warning, whatever the shape.
        centred = make("Centred" + engine, (-0.3, -0.3, -0.9), (0.3, 0.3, 0.9), 'CHARACTER')
        check(warning(centred) is None, f"{engine}: a centred mesh warned")
        # Its origin at its feet: the Character's default shape (Jolt: capsule, Bullet: sphere)
        # sits 0.9 m too low.
        feet = make("Feet" + engine, (-0.3, -0.3, 0.0), (0.3, 0.3, 1.8), 'CHARACTER')
        check(properties_game.game_collision_bounds(feet, is_jolt) ==
              ('CAPSULE' if is_jolt else 'SPHERE'),
              f"{engine}: a Character's default bounds")
        # The Bounds dropdown shows Jolt's capsule for a Character with no bounds chosen (it
        # showed Sphere until 2026-11-18; the RNA getter has no scene, so Bullet shows it too).
        check(not feet.game.use_collision_bounds and
              feet.game.collision_bounds_type == 'CAPSULE',
              f"{engine}: the dropdown shows {feet.game.collision_bounds_type} for a Character")
        # Empty bounds kept from another type (they read '' for a Character): Jolt fits a
        # Character a capsule, which the warning measures.
        empty_bounds = make("EmptyBounds" + engine, (-0.3, -0.3, 0.0), (0.3, 0.3, 1.8),
                            'STATIC', 'Empty')
        empty_bounds.game.physics_type = 'CHARACTER'
        check(properties_game.game_collision_bounds(empty_bounds, is_jolt) ==
              ('CAPSULE' if is_jolt else ''),
              f"{engine}: a Character's Empty bounds: "
              f"{properties_game.game_collision_bounds(empty_bounds, is_jolt)}")
        text = warning(feet)
        check(text is not None and "0.90 m" in text, f"{engine}: the feet origin: {text}")
        check(offers_origin_to_geometry(feet), f"{engine}: no Origin to Geometry offered")
        # The user's case: chosen Capsule / Box / Sphere / Cylinder / Cone warn, Convex Hull and
        # Triangle Mesh never (the mesh's own points).
        for bounds in ('CAPSULE', 'BOX', 'SPHERE', 'CYLINDER', 'CONE'):
            ob = make(f"{bounds}{engine}", (-1.0, -1.0, -1.0), (1.0, 1.0, 3.0), 'DYNAMIC', bounds)
            text = warning(ob)
            check(text is not None and "1.00 m" in text, f"{engine} {bounds}: {text}")
        for bounds in ('CONVEX_HULL', 'TRIANGLE_MESH'):
            ob = make(f"{bounds}{engine}", (-1.0, -1.0, -1.0), (1.0, 1.0, 3.0), 'DYNAMIC', bounds)
            check(warning(ob) is None, f"{engine} {bounds} warned")
        # A static object without chosen bounds is a triangle mesh: no warning.
        static = make("Static" + engine, (0.0, 0.0, 0.0), (4.0, 4.0, 1.0), 'STATIC')
        check(warning(static) is None, f"{engine}: a static triangle mesh warned")
        # The object's scale counts: 0.5 m off in the mesh, scaled 3: 1.50 m.
        scaled = make("Scaled" + engine, (-0.5, -0.5, 0.0), (0.5, 0.5, 1.0), 'RIGID_BODY', 'BOX',
                      scale=(3.0, 3.0, 3.0))
        text = warning(scaled)
        check(text is not None and "1.50 m" in text, f"{engine}: the scale: {text}")
        # Under 1% of the mesh's size: no warning (a hair off is no mistake).
        hair = make("Hair" + engine, (-1.0, -1.0, -0.99), (1.0, 1.0, 1.01), 'RIGID_BODY', 'BOX')
        check(warning(hair) is None, f"{engine}: a 1 cm offset of a 2 m mesh warned")
        # Origin to Geometry (bounds centre) puts the mesh's middle on the origin: no warning.
        bpy.ops.object.select_all(action='DESELECT')
        feet.select_set(True)
        bpy.context.view_layer.objects.active = feet
        with bpy.context.temp_override(active_object=feet, selected_editable_objects=[feet]):
            bpy.ops.object.origin_set(type='ORIGIN_GEOMETRY', center='BOUNDS')
        bpy.context.view_layer.update()
        check(warning(feet) is None, f"{engine}: still warned after Origin to Geometry")

    if PROBLEMS:
        print("PROBLEMS (%d):" % len(PROBLEMS))
        for problem in PROBLEMS:
            print("  " + problem)
        sys.exit(1)
    print("origin offset warning: OK")


main()
