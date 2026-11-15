# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Physics panel warns when a moving body's locks leave it no free axis (2026-11-15).

Jolt cannot simulate a moving body with no free axis (it divides by zero later), so the game
ignores such a body's axis locks (JoltPhysicsEnvironment's converter, with a console warning). The
panel says so first, under the locks: a Rigid Body (and a Vehicle or Motorcycle chassis) with all
six locks, a Dynamic type with its three movement locks (it never rotates). Never with a free axis
left, and never in a Bullet scene (Bullet keeps such a body still).

The real panel's draw runs with a layout that records every label (no window needed).

    blender --background --factory-startup --python tests/python/game_physics_no_free_axis_warning.py
"""

import sys

import bpy
from bl_ui import properties_game

PROBLEMS = []
WARNING = "known Jolt limitation"


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


class Recorder:
    """A layout that accepts every call, keeping the labels' texts."""

    def __init__(self, labels):
        self.__dict__["labels"] = labels

    def __getattr__(self, name):
        def call(*args, **kwargs):
            if name == "label" and "text" in kwargs:
                self.labels.append(kwargs["text"])
            return Recorder(self.labels)
        return call

    def __setattr__(self, name, value):
        pass


def panel_labels(ob):
    """The labels PHYSICS_PT_game_physics draws for `ob`."""
    cls = properties_game.PHYSICS_PT_game_physics
    methods = {}
    for klass in reversed(cls.__mro__):
        for name, value in vars(klass).items():
            if isinstance(value, staticmethod):
                methods[name] = value
            elif callable(value) and not name.startswith("__"):
                methods[name] = value
    fake = type("FakePhysicsPanel", (), methods)()
    labels = []
    fake.layout = Recorder(labels)
    with bpy.context.temp_override(object=ob, active_object=ob):
        cls.draw(fake, bpy.context)
    return labels


def warned(ob):
    return any(WARNING in text for text in panel_labels(ob))


def set_locks(ob, translation, rotation):
    game = ob.game
    game.lock_location_x, game.lock_location_y, game.lock_location_z = translation
    game.lock_rotation_x, game.lock_rotation_y, game.lock_rotation_z = rotation


def main():
    scene = bpy.context.scene
    ob = bpy.data.objects.new("Body", bpy.data.meshes.new("Body"))
    scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    all3 = (True, True, True)
    none3 = (False, False, False)
    cases = [
        # (engine, physics type, translation locks, rotation locks, warned)
        ('JOLT', 'RIGID_BODY', all3, all3, True),
        ('JOLT', 'RIGID_BODY', all3, (True, True, False), False),
        ('JOLT', 'RIGID_BODY', (True, False, True), all3, False),
        ('JOLT', 'RIGID_BODY', none3, none3, False),
        ('JOLT', 'DYNAMIC', all3, none3, True),
        ('JOLT', 'DYNAMIC', (True, True, False), all3, False),
        ('JOLT', 'VEHICLE', all3, all3, True),
        ('JOLT', 'VEHICLE', all3, (False, True, True), False),
        ('JOLT', 'STATIC', all3, all3, False),
        ('BULLET', 'RIGID_BODY', all3, all3, False),
        ('BULLET', 'DYNAMIC', all3, none3, False),
    ]
    for engine, physics_type, translation, rotation, expected in cases:
        scene.game_settings.physics_engine = engine
        ob.game.physics_type = physics_type
        set_locks(ob, translation, rotation)
        got = warned(ob)
        check(got == expected, "%s %s locks %s %s: warning %s, expected %s" % (
            engine, physics_type, translation, rotation, got, expected))
    # The motorcycle chassis (Jolt only) as the vehicle chassis.
    scene.game_settings.physics_engine = 'JOLT'
    ob.game.physics_type = 'VEHICLE'
    ob.game.vehicle.vehicle_type = 'MOTORCYCLE_CHASSIS'
    set_locks(ob, all3, all3)
    check(warned(ob), "JOLT motorcycle chassis with all six locks: no warning")
    set_locks(ob, all3, (True, False, True))
    check(not warned(ob), "JOLT motorcycle chassis with a free axis: warned")


main()
if PROBLEMS:
    for problem in PROBLEMS:
        print("PROBLEM:", problem)
    sys.exit(1)
print("OK: the no-free-axis warning shows exactly when the locks leave no free axis")
