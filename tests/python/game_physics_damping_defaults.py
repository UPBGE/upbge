# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""A new rigid body shows the damping Reset to Default gives (2026-11-14).

Every new object holds Bullet's default pair (Translation 0.04, Rotation 0.1), while in a Jolt
scene Reset to Default gives Jolt's default rates (0.05, 0.05; the panel shows Jolt's rate,
`JoltDampingRates`). Choosing a type Jolt moves (Dynamic, Rigid Body, Vehicle: the ones the Damping
rows show for) now turns the untouched pair into Jolt's (rna_object.cc,
`rna_GameObjectSettings_physics_type_set`); both pairs play as Jolt's rate 0.05, so no object
moves differently. Typed values, other types and Bullet scenes keep their numbers.

    blender --background --factory-startup --python tests/python/game_physics_damping_defaults.py
"""

import sys

import bpy

PROBLEMS = []


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


def damping(ob):
    return (round(ob.game.damping, 6), round(ob.game.rotation_damping, 6))


BULLET_PAIR = (0.04, 0.1)
JOLT_PAIR = (0.05, 0.05)
scene = bpy.context.scene
mesh = bpy.data.meshes.new("DampingMesh")


def new_object(name):
    ob = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(ob)
    return ob


scene.game_settings.physics_engine = 'JOLT'
for physics_type in ('DYNAMIC', 'RIGID_BODY', 'VEHICLE'):
    ob = new_object("Jolt" + physics_type)
    check(damping(ob) == BULLET_PAIR, "a new object holds %s, not Bullet's pair" % (damping(ob),))
    ob.game.physics_type = physics_type
    check(damping(ob) == JOLT_PAIR,
          "a new %s in a Jolt scene shows %s, not Reset to Default's %s" % (
              physics_type, damping(ob), JOLT_PAIR))

# Types whose damping nothing reads keep the numbers.
for physics_type in ('STATIC', 'NO_COLLISION', 'SENSOR'):
    ob = new_object("Jolt" + physics_type)
    ob.game.physics_type = physics_type
    check(damping(ob) == BULLET_PAIR, "a %s object's damping changed to %s" % (
        physics_type, damping(ob)))

# A value the user typed stays, also Bullet's pair half typed.
ob = new_object("Typed")
ob.game.damping = 0.3
ob.game.rotation_damping = 0.2
ob.game.physics_type = 'RIGID_BODY'
check(damping(ob) == (0.3, 0.2), "typed damping changed to %s" % (damping(ob),))
ob = new_object("HalfTyped")
ob.game.rotation_damping = 0.5
ob.game.physics_type = 'RIGID_BODY'
check(damping(ob) == (0.04, 0.5), "a half-typed pair changed to %s" % (damping(ob),))

# Changing the type again keeps Jolt's numbers.
ob = bpy.data.objects["JoltRIGID_BODY"]
ob.game.physics_type = 'STATIC'
ob.game.physics_type = 'DYNAMIC'
check(damping(ob) == JOLT_PAIR, "a type change moved Jolt's pair to %s" % (damping(ob),))

# A Bullet scene keeps Bullet's pair.
bullet = bpy.data.scenes.new("BulletScene")
bullet.game_settings.physics_engine = 'BULLET'
ob = bpy.data.objects.new("BulletRigid", mesh)
bullet.collection.objects.link(ob)
ob.game.physics_type = 'RIGID_BODY'
check(damping(ob) == BULLET_PAIR, "a Bullet scene's rigid body shows %s" % (damping(ob),))

print("PROBLEMS (%d):" % len(PROBLEMS))
for problem in PROBLEMS:
    print("  " + problem)
sys.exit(1 if PROBLEMS else 0)
