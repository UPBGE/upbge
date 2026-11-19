# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The rules of the Logic Nodes ray previews (scripts/startup/logic_nodes_ray_preview.py), checked
in a headless Blender against numbers worked out by hand: each node's geometry as the game engine
works it out, the inputs followed through linked nodes, which previews each mode shows, and the
game guard. The probe ``ray_preview_parity`` of the acceptance tests holds the same geometry to
what the engine really gives.

    blender --background --factory-startup --python tests/python/logic_nodes_ray_preview.py
"""

import math
import os
import sys

import bpy
from mathutils import Euler, Matrix, Vector

import logic_nodes_ray_preview as rp

PROBLEMS = []
CHECKS = [0]


def check(condition, text):
    CHECKS[0] += 1
    if not condition:
        PROBLEMS.append(text)


def near(a, b, tolerance=1.0e-5):
    if isinstance(a, (int, float)) or isinstance(b, (int, float)):
        return a is not None and b is not None and abs(float(a) - float(b)) <= tolerance
    return a is not None and len(a) == len(b) and all(abs(x - y) <= tolerance for x, y in zip(a, b))


def vec(*components):
    return Vector(components)


scene = bpy.context.scene
for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj)
scene.game_settings.physics_gravity = 10.0


def socket(node, identifier, outputs=False):
    for item in (node.outputs if outputs else node.inputs):
        if item.identifier == identifier:
            return item
    raise KeyError((node.name, identifier))


def put(node, identifier, value):
    socket(node, identifier).default_value = value


def add(tree, idname, **settings):
    """A node by the type it works as (merged nodes through their option)."""
    options = {"LogicNativeRaycast": ("LogicNativeCastRay", "NEAREST"),
               "LogicNativeRaycastAll": ("LogicNativeCastRay", "ALL"),
               "LogicNativeShapeCast": ("LogicNativeCastShape", "NEAREST"),
               "LogicNativeShapeCastAll": ("LogicNativeCastShape", "ALL")}
    if idname in options:
        node = tree.nodes.new(options[idname][0])
        node.variant = options[idname][1]
    else:
        node = tree.nodes.new(idname)
    for name, value in settings.items():
        setattr(node, name, value)
    return node


def link(tree, a, out, b, inp):
    tree.links.new(socket(a, out, True), socket(b, inp))


def empty(name, location=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1)):
    obj = bpy.data.objects.new(name, None)
    scene.collection.objects.link(obj)
    obj.location = location
    obj.rotation_euler = rotation
    obj.scale = scale
    return obj


def mesh_object(name, vertices, faces, location=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1)):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    obj.location = location
    obj.rotation_euler = rotation
    obj.scale = scale
    return obj


def bind(obj, tree):
    binding = obj.game.logic_node_binding_new()
    binding.tree = tree
    binding.tree_name = tree.name
    binding.enabled = True


def update():
    bpy.context.view_layer.update()
    return bpy.context.evaluated_depsgraph_get()


def preview(tree, node, owner):
    return rp.preview_node(scene, update(), tree, node, owner)


tree = bpy.data.node_groups.new("PreviewRules", "LogicNodeTree")
tree.use_fake_user = True
owner = empty("Owner", (10.0, 0.0, 0.0), (0.0, 0.0, math.pi / 2), (2.0, 2.0, 2.0))
turn = Euler((0.0, 0.0, math.pi / 2)).to_matrix()

# ---- Registration -----------------------------------------------------------------------
check(hasattr(scene, "logic_ray_preview"), "Scene.logic_ray_preview is not registered")
check(bpy.app.handlers.depsgraph_update_post.count(rp._on_depsgraph_update) == 1,
      "the update handler is not registered once")
check(len(rp._State.draw_handles) == 2, "the two draw callbacks are not registered")

# ---- Raycast ----------------------------------------------------------------------------
ray = add(tree, "LogicNativeRaycast", input_mode='DIRECTION_DISTANCE')
put(ray, "Origin", (1.0, 2.0, 3.0))
put(ray, "Direction", (0.0, 2.0, 0.0))
put(ray, "Max Distance", 5.0)
p = preview(tree, ray, owner)
check(near(p.data.get("destination"), (1, 7, 3)), "Raycast world: %s" % p.data)
check(near(p.data.get("direction"), (0, 1, 0)), "Raycast world direction: %s" % p.data)
check(p.lines and not p.dynamic, "Raycast world draws nothing or is dynamic")

# Local: the owner's whole transform on Origin, its turn alone on Direction (scale 2 ignored).
put(ray, "Local", True)
put(ray, "Origin", (1.0, 0.0, 0.0))
put(ray, "Direction", (1.0, 0.0, 0.0))
put(ray, "Max Distance", 3.0)
p = preview(tree, ray, owner)
check(near(p.data.get("origin"), (10, 2, 0)), "Raycast local origin: %s" % p.data)
check(near(p.data.get("destination"), (10, 5, 0)), "Raycast local end: %s" % p.data)

# End Point, local: both ends by the whole transform.
ray.input_mode = 'END_POINT'
put(ray, "Destination", (0.0, 1.0, 0.0))
p = preview(tree, ray, owner)
check(near(p.data.get("destination"), (8, 0, 0)), "Raycast local end point: %s" % p.data)

# A Caster other than the owner.
caster = empty("Caster", (0.0, 5.0, 0.0))
socket(ray, "Caster").default_value = caster
p = preview(tree, ray, owner)
check(near(p.data.get("origin"), (1, 5, 0)) and near(p.data.get("destination"), (0, 6, 0)),
      "Raycast with a Caster: %s" % p.data)
socket(ray, "Caster").default_value = None
# Zero distance: nothing, with the reason.
ray.input_mode = 'DIRECTION_DISTANCE'
put(ray, "Max Distance", 0.0)
p = preview(tree, ray, owner)
check(not p.lines and p.problem, "Raycast with no distance draws")
put(ray, "Max Distance", 3.0)
put(ray, "Local", False)

# ---- Linked inputs ----------------------------------------------------------------------
combine = add(tree, "LogicNativeCombineXYZ")
times = add(tree, "LogicNativeMath", operation='MULTIPLY')
put(times, "A", 2.0)
put(times, "B", 3.0)
link(tree, times, "Result", combine, "X")
vector_value = add(tree, "LogicNativeValueVector")
socket(vector_value, "Vector", True).default_value = (1.0, 2.0, 3.0)
separate = add(tree, "LogicNativeSeparateXYZ")
link(tree, vector_value, "Vector", separate, "Vector")
link(tree, separate, "Y", combine, "Y")
divide = add(tree, "LogicNativeMath", operation='DIVIDE')
put(divide, "A", 1.0)
put(divide, "B", 0.0)
link(tree, divide, "Result", combine, "Z")
link(tree, combine, "Vector", ray, "Origin")
axis = add(tree, "LogicNativeGetAxisVector", axis='LOCAL_NEGATIVE_Y')
link(tree, axis, "Vector", ray, "Direction")
float_value = add(tree, "LogicNativeValueFloat")
socket(float_value, "Float", True).default_value = 4.0
link(tree, float_value, "Float", ray, "Max Distance")
p = preview(tree, ray, owner)
check(near(p.data.get("origin"), (6, 2, 0)), "followed Origin (Math, Separate, divide by 0): %s"
      % p.data)
# The owner turned a quarter: its -Y is world +X.
check(near(p.data.get("destination"), (10, 2, 0)), "followed Direction (owner's -Y): %s" % p.data)
check(not p.dynamic, "a followed chain is called dynamic: %s" % p.dynamic)
# Only the game knows a game property: dynamic, a grey ring where the ray starts.
prop = add(tree, "LogicNativeGetGamePropertyValue")
link(tree, prop, socket(prop, "Value", True).identifier if any(
    s.identifier == "Value" for s in prop.outputs) else prop.outputs[0].identifier, ray, "Max Distance")
p = preview(tree, ray, owner)
check("Max Distance" in p.dynamic and any(color == rp.DYNAMIC_COLOR for *_e, color, _d in p.lines),
      "a game property input is not dynamic: %s %s" % (p.dynamic, p.data))
tree.nodes.remove(prop)
link(tree, float_value, "Float", ray, "Max Distance")

# The evaluator's operations (the engine's models in LN_NodeValues_test).
def evaluate(node, identifier, kind):
    return rp.Evaluator(scene, update(), owner).output(node, socket(node, identifier, True), kind)


for operation, a, b, expected in (('ROUND', -2.5, 0.0, -2.0), ('FRACT', -1.25, 0.0, 0.75),
                                  ('MODULO', 5.0, 0.0, 0.0), ('MODULO', -5.5, 2.0, -1.5),
                                  ('SIGN', -0.0, 0.0, 0.0), ('POWER', 2.0, 3.0, 8.0),
                                  ('DEGREES', math.pi, 0.0, 180.0)):
    math_node = add(tree, "LogicNativeMath", operation=operation)
    put(math_node, "A", a)
    put(math_node, "B", b)
    got = evaluate(math_node, "Result", 'FLOAT')
    check(near(got, expected), "Math %s(%s, %s) = %s, expected %s" % (operation, a, b, got, expected))
    tree.nodes.remove(math_node)
for operation, a, b, expected in (('NORMALIZE', (0, 0, 0), (0, 0, 0), (0, 0, 0)),
                                  ('NORMALIZE', (3, 0, 4), (0, 0, 0), (0.6, 0, 0.8)),
                                  ('DIVIDE', (1, 2, 3), (0, 2, 1e-30), (0, 1, 0)),
                                  ('MINIMUM', (1, 5, -1), (2, 3, -4), (1, 3, -4))):
    vector_node = add(tree, "LogicNativeVectorMath", operation=operation)
    put(vector_node, "A", a)
    put(vector_node, "B", b)
    got = evaluate(vector_node, "Result", 'VECTOR')
    check(near(got, expected), "Vector Math %s = %s, expected %s" % (operation, got, expected))
    tree.nodes.remove(vector_node)
attribute = add(tree, "LogicNativeGetObjectAttribute", attribute_type='WORLD_SCALE')
check(near(evaluate(attribute, "Vector", 'VECTOR'), (2, 2, 2)),
      "Get Object Attribute World Scale of the owner (empty Object)")
attribute.attribute_type = 'WORLD_ORIENTATION'
got = evaluate(attribute, "Orientation", 'ROTATION')
check(got is not rp.UNKNOWN and near(got.col[0], turn.col[0]), "Get Object Attribute World Orientation")
by_name = add(tree, "LogicNativeObjectByName")
put(by_name, "Name", "Caster")
check(evaluate(by_name, "Object", 'OBJECT') == caster, "Object By Name")
put(by_name, "Name", "Nobody")
check(evaluate(by_name, "Object", 'OBJECT') is None, "Object By Name of nobody")
for node in (attribute, by_name):
    tree.nodes.remove(node)
for node in (combine, times, vector_value, separate, divide, axis, float_value):
    tree.nodes.remove(node)

# ---- Shape Cast ---------------------------------------------------------------------------
shape = add(tree, "LogicNativeShapeCast", shape_type='BOX')
put(shape, "Origin", (1.0, 0.0, 0.0))
put(shape, "Destination", (1.0, 0.0, 2.0))
put(shape, "Half Extents", (0.5, 0.25, 0.1))
put(shape, "Rotation", (0.0, 0.0, math.pi / 2))
put(shape, "Local", True)
p = preview(tree, shape, owner)
check(near(p.data.get("origin"), (10, 2, 0)) and near(p.data.get("destination"), (10, 2, 4)),
      "Shape Cast local ends: %s" % p.data)
expected = turn @ turn
check(p.data.get("rotation") is not None and near(p.data["rotation"].col[0], expected.col[0]),
      "Shape Cast local turn")
shape.cast_mode = 'OVERLAP'
p = preview(tree, shape, owner)
check(near(p.data.get("destination"), p.data.get("origin")), "Overlap moves the shape")
put(shape, "Half Extents", (0.0, 1.0, 1.0))
p = preview(tree, shape, owner)
check(not p.lines and p.problem, "a box without size draws")
tree.nodes.remove(shape)

# ---- Raycast from Mesh --------------------------------------------------------------------
grid = mesh_object("Grid",
                   [(x, y, 0.0) for y in (0, 1, 2) for x in (0, 1, 2)],
                   [(0, 1, 4, 3), (1, 2, 5, 4), (3, 4, 7, 6), (4, 5, 8, 7)],
                   location=(0.0, 0.0, 1.0))
from_mesh = add(tree, "LogicNativeRaycastFromMesh", mesh_domain='FACES')
socket(from_mesh, "Source Object").default_value = grid
put(from_mesh, "Start Offset", 0.5)
put(from_mesh, "Max Distance", 2.0)
put(from_mesh, "Max Rays", 3)
p = preview(tree, from_mesh, owner)
rays = p.data.get("rays", [])
# Three of four faces: faces 0, 1 and 2 (LN_PickMeshRaySamples).
centres = [(0.5, 0.5), (1.5, 0.5), (0.5, 1.5)]
check(len(rays) == 3 and all(near(r[0], (cx, cy, 1.5)) and near(r[1], (cx, cy, 3.5))
                             for r, (cx, cy) in zip(rays, centres)),
      "Raycast from Mesh 3 of 4 faces: %s" % rays)
from_mesh.ray_direction = 'INVERTED_NORMALS'
p = preview(tree, from_mesh, owner)
check(near(p.data["rays"][0][0], (0.5, 0.5, 0.5)), "inverted normals")
from_mesh.ray_direction = 'NORMALS'
# A vertex group: faces cast when all their corners are in it.
group = grid.vertex_groups.new(name="Edge")
group.add([0, 1, 3, 4, 2], 1.0, 'REPLACE')
group.add([5], 0.0, 'REPLACE')
put(from_mesh, "Vertex Group", "Edge")
put(from_mesh, "Max Rays", 256)
p = preview(tree, from_mesh, owner)
check([near(r[0], (0.5, 0.5, 1.5)) for r in p.data.get("rays", [])] == [True],
      "vertex group faces: %s" % p.data.get("rays"))
from_mesh.mesh_domain = 'VERTICES'
p = preview(tree, from_mesh, owner)
check(len(p.data.get("rays", [])) == 5, "vertex group vertices (weight 0 is out): %d" %
      len(p.data.get("rays", [])))
put(from_mesh, "Vertex Group", "Nothing")
p = preview(tree, from_mesh, owner)
check(not p.lines and "Nothing" in p.problem, "a missing group draws")
# A face attribute picks faces by its own value.
attribute = grid.data.attributes.new("Picked", 'BOOLEAN', 'FACE')
attribute.data[3].value = True
from_mesh.mesh_domain = 'FACES'
put(from_mesh, "Vertex Group", "Picked")
p = preview(tree, from_mesh, owner)
check(len(p.data.get("rays", [])) == 1 and near(p.data["rays"][0][0], (1.5, 1.5, 1.5)),
      "face attribute: %s" % p.data.get("rays"))
put(from_mesh, "Vertex Group", "")
# Non-uniform scale: the normal of a slanted face follows the inverse transpose.
wedge = mesh_object("Wedge", [(0, 0, 0), (1, 0, 0), (0, 1, 1)], [(0, 1, 2)],
                    rotation=(0.3, 0.2, 0.1), scale=(3.0, 1.0, 0.5))
socket(from_mesh, "Source Object").default_value = wedge
put(from_mesh, "Start Offset", 0.0)
put(from_mesh, "Max Distance", 1.0)
p = preview(tree, from_mesh, owner)
world = [wedge.matrix_world @ Vector(v) for v in ((0, 0, 0), (1, 0, 0), (0, 1, 1))]
normal = (world[1] - world[0]).cross(world[2] - world[0]).normalized()
got = p.data["rays"][0]
check(near((got[1] - got[0]).normalized(), normal, 1.0e-5),
      "normal under non-uniform scale: %s vs %s" % ((got[1] - got[0]).normalized(), normal))
# Vertices: a loose vertex casts nothing; a corner's normal is Blender's vertex normal.
lonely = mesh_object("Lonely", [(0, 0, 0), (1, 0, 0), (0, 1, 0), (5, 5, 5)], [(0, 1, 2)])
socket(from_mesh, "Source Object").default_value = lonely
from_mesh.mesh_domain = 'VERTICES'
p = preview(tree, from_mesh, owner)
check(len(p.data.get("rays", [])) == 3, "a loose vertex casts: %d" % len(p.data.get("rays", [])))
# The bins with a gap: 10 vertices, 4 rays: bins 0-1, 2-4, 5-6, 7-9 give 0, 3, 5, 8.
picked = rp.pick_mesh_ray_samples(10, 4, lambda i: i)
check(picked == [0, 3, 5, 8], "even bins: %s" % picked)
picked = rp.pick_mesh_ray_samples(10, 4, lambda i: i if i not in (3, 5) else None)
check(picked == [0, 2, 6, 8], "bins search around their middle: %s" % picked)
tree.nodes.remove(from_mesh)

# ---- Camera Ray and Mouse Ray --------------------------------------------------------------
camera_data = bpy.data.cameras.new("Lens")
camera = bpy.data.objects.new("Lens", camera_data)
scene.collection.objects.link(camera)
camera.location = (0.0, -10.0, 0.0)
camera.rotation_euler = (math.pi / 2, 0.0, 0.0)
scene.camera = camera
camera_ray = add(tree, "LogicNativeCameraRay")
put(camera_ray, "Distance", 20.0)
p = preview(tree, camera_ray, owner)
check(near(p.data.get("origin"), (0, -10, 0)) and near(p.data.get("direction"), (0, 1, 0)),
      "Camera Ray through the middle: %s" % p.data)
put(camera_ray, "Aim", (0.0, 0.0, 0.0))
p = preview(tree, camera_ray, owner)
frame = camera_data.view_frame(scene=scene)
top_left = (camera.matrix_world.to_3x3() @ frame[3]).normalized()
check(near(p.data.get("direction"), top_left, 1.0e-5), "Camera Ray top left: %s vs %s"
      % (p.data.get("direction"), top_left))
camera_data.type = 'ORTHO'
p = preview(tree, camera_ray, owner)
check(near(p.data.get("direction"), (0, 1, 0)) and
      near(p.data.get("origin").y, -10.0 + camera_data.clip_start),
      "orthographic Camera Ray: %s" % p.data)
camera_data.type = 'PERSP'
mouse = add(tree, "LogicNativeMouseRay")
put(mouse, "Distance", 7.0)
p = preview(tree, mouse, owner)
check(near(p.data.get("destination"), (0, -3, 0)), "Mouse Ray reach: %s" % p.data)
mouse.interaction_view = 'OVERLAY'
p = preview(tree, mouse, owner)
check(not p.lines and p.problem, "Mouse Ray of the overlay draws")
for node in (camera_ray, mouse):
    tree.nodes.remove(node)

# ---- Projectile Ray ------------------------------------------------------------------------
throw = add(tree, "LogicNativeProjectileRay")
put(throw, "Origin", (0.0, 0.0, 5.0))
put(throw, "Aim", (0.0, 1.0, 5.0))
put(throw, "Power", 5.0)
put(throw, "Distance", 100.0)
put(throw, "Resolution", 0.9)
put(throw, "Max Time", 0.35)
p = preview(tree, throw, owner)
points = p.data.get("points", [])
# Steps of 0.1 s, the last one cut at 0.35 s; gravity 10 down (the scene's).
expected = [(0, 5 * t, 5 - 5 * t * t) for t in (0.0, 0.1, 0.2, 0.3, 0.35)]
check(len(points) == 5 and all(near(a, b, 1.0e-5) for a, b in zip(points, expected)),
      "Projectile Ray path: %s" % points)
check(near(p.data.get("flight_time"), 0.35), "Projectile Ray time %s" % p.data.get("flight_time"))
put(throw, "Max Time", 0.0)
put(throw, "Distance", 0.75)
p = preview(tree, throw, owner)
points = p.data["points"]
length = sum((b - a).length for a, b in zip(points, points[1:]))
check(near(length, 0.75, 1.0e-5), "Projectile Ray cut at Max Distance: %s" % length)
throw.gravity_source = 'CUSTOM'
put(throw, "Gravity", (0.0, 0.0, 0.0))
put(throw, "Distance", 2.0)
p = preview(tree, throw, owner)
check(near(p.data["points"][-1], (0, 2, 5)), "Projectile Ray custom gravity: %s" % p.data["points"])
throw.gravity_source = 'SCENE'
put(throw, "Local", True)
put(throw, "Origin", (1.0, 0.0, 0.0))
put(throw, "Aim", (1.0, 0.0, 0.0))
p = preview(tree, throw, owner)
check(near(p.data["points"][0], (10, 1, 0)), "Projectile Ray local origin (no scale): %s"
      % p.data["points"][0])
check(near(p.data["velocity"], (0, 5, 0)), "Projectile Ray local aim: %s" % p.data["velocity"])
tree.nodes.remove(throw)

# ---- Line of Sight -------------------------------------------------------------------------
target = empty("Target", (10.0, 5.0, 1.0))
look = add(tree, "LogicNativeLineOfSight")
put(look, "Eye Offset", (0.0, 0.0, 0.5))
put(look, "Forward", (1.0, 0.0, 0.0))
socket(look, "Target").default_value = target
put(look, "Max Distance", 10.0)
put(look, "View Angle", math.radians(90.0))
p = preview(tree, look, owner)
# Eye: the owner's transform on (0, 0, 0.5) = (10, 0, 1); Forward turned: +Y.
check(near(p.data.get("eye"), (10, 0, 1)) and near(p.data.get("distance"), 5.0) and
      near(p.data.get("angle"), 0.0, 0.02) and p.data.get("in_view"),
      "Line of Sight: %s" % p.data)
target.location = (15.0, 0.0, 1.0)
p = preview(tree, look, owner)
check(near(p.data.get("angle"), 90.0, 0.02) and not p.data.get("in_view"),
      "Line of Sight out of view: %s" % p.data)
put(look, "View Angle", math.radians(360.0))
p = preview(tree, look, owner)
check(p.data.get("in_view"), "all around")
socket(look, "Target").default_value = None
p = preview(tree, look, owner)
check(p.lines and "distance" not in p.data, "Line of Sight with no target")
tree.nodes.remove(look)

# ---- Sweep Object, Aim for Arc, Ray to Plane, Explosion ------------------------------------
crate = mesh_object("Crate", [(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)],
                    [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4),
                     (1, 5, 7, 3)], location=(0, 0, 0), rotation=(0, 0, math.pi / 2))
sweep = add(tree, "LogicNativeSweepObject")
socket(sweep, "Object").default_value = crate
put(sweep, "Destination", (2.0, 0.0, 0.0))
put(sweep, "Local", True)
p = preview(tree, sweep, owner)
check(near(p.data.get("destination"), (0, 2, 0)), "Sweep local destination: %s" % p.data)
sweep.rotation_mode = 'CUSTOM'
put(sweep, "Rotation", (0.0, 0.0, math.pi / 2))
p = preview(tree, sweep, owner)
check(near(p.data["orientation"].col[0], (turn @ turn).col[0]), "Sweep local custom turn")
tree.nodes.remove(sweep)

aim = add(tree, "LogicNativeAimForArc")
put(aim, "Start", (0.0, 0.0, 0.0))
put(aim, "Target", (10.0, 0.0, 0.0))
put(aim, "Speed", 15.0)
p = preview(tree, aim, owner)
g, v, x = 10.0, 15.0, 10.0
angle = math.atan((v * v - math.sqrt(v ** 4 - g * g * x * x)) / (g * x))
check(p.data.get("can_reach") and near(p.data.get("flight_time"), x / (v * math.cos(angle)), 1e-6),
      "Aim for Arc low arc: %s" % p.data)
put(aim, "Speed", 5.0)
p = preview(tree, aim, owner)
check(p.data.get("can_reach") is False and near(p.data.get("flight_time"), 0.0) and p.lines,
      "Aim for Arc out of reach: %s" % p.data)
aim.solve_by = 'ARC_HEIGHT'
put(aim, "Arc Height", 5.0)
p = preview(tree, aim, owner)
check(p.data.get("can_reach") and near(p.data.get("flight_time"), 2.0, 1e-6),
      "Aim for Arc by height: %s" % p.data)
tree.nodes.remove(aim)

plane = add(tree, "LogicNativeRayToPlane")
put(plane, "Ray Origin", (1.0, 2.0, 5.0))
put(plane, "Ray Direction", (0.0, 0.0, -2.0))
put(plane, "Plane Point", (0.0, 0.0, 1.0))
put(plane, "Plane Normal", (0.0, 0.0, 3.0))
p = preview(tree, plane, owner)
check(near(p.data.get("distance"), 4.0) and near(p.data.get("point"), (1, 2, 1)),
      "Ray to Plane: %s" % p.data)
put(plane, "Ray Direction", (0.0, 0.0, 1.0))
p = preview(tree, plane, owner)
check(near(p.data.get("distance"), -1.0), "Ray to Plane behind: %s" % p.data)
tree.nodes.remove(plane)

blast = add(tree, "LogicNativeExplosionImpulse")
put(blast, "Radius", 3.0)
p = preview(tree, blast, owner)
check(near(p.data.get("center"), (10, 0, 0)) and near(p.data.get("radius"), 3.0),
      "Explosion around the owner: %s" % p.data)
# (2026-11-17) The Offset, in the owner's own space (turned 90 degrees about Z, scaled 2): (0, 1, 0)
# is 2 m along -X.
put(blast, "Offset", (0.0, 1.0, 0.0))
p = preview(tree, blast, owner)
check(near(p.data.get("center"), (8, 0, 0)), "Explosion at the owner's Offset: %s" % p.data)
blast.center = 'POSITION'
put(blast, "Origin", (1.0, 1.0, 1.0))
p = preview(tree, blast, owner)
check(near(p.data.get("center"), (1, 1, 1)), "Explosion at a position: %s" % p.data)
tree.nodes.remove(blast)

# Keep Distance: the owner (an empty, no body) measures from the Radius sphere even in Object's
# Shape; a body's own shape as the converter builds it; Measure From Sphere always the sphere.
keep = add(tree, "LogicNativeKeepDistance")
put(keep, "Distance", 1.5)
put(keep, "Radius", 0.25)
p = preview(tree, keep, owner)
check(p.data.get("shape") == 'SPHERE' and near(p.data.get("center"), (10, 0, 0)) and
      near(p.data.get("radius"), 0.25) and near(p.data.get("distance"), 1.5) and p.lines,
      "Keep Distance of an object without a body: %s" % p.data)
check(rp.keep_distance_shape(owner) is None, "an empty has a shape to keep distance from")
ball = mesh_object("KeepBall", [(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)],
                   [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4),
                    (1, 5, 7, 3)], location=(0, 5, 0), scale=(0.5, 0.5, 0.5))
socket(keep, "Object").default_value = ball
for kind, bounds, expected in (('DYNAMIC', None, 'SPHERE'), ('RIGID_BODY', None, 'BOX'),
                               ('KINEMATIC', None, 'BOX'), ('CHARACTER', None, 'CAPSULE'),
                               # (Made a Character, chosen Triangle Mesh bounds become Box:
                               # the RNA's type setter.)
                               ('CHARACTER', 'TRIANGLE_MESH', 'BOX'),
                               ('CHARACTER', 'Empty', 'CAPSULE'), ('STATIC', 'Empty', None),
                               ('STATIC', None, None), ('SENSOR', None, None),
                               ('SOFT_BODY', None, None), ('NO_COLLISION', None, None),
                               ('RIGID_BODY', 'CAPSULE', 'CAPSULE'),
                               ('RIGID_BODY', 'CONVEX_HULL', 'CONVEX_HULL'),
                               ('STATIC', 'BOX', 'BOX'), ('STATIC', 'TRIANGLE_MESH', None)):
    # (A Character's dropdown offers no Triangle Mesh nor Empty: such bounds come from another
    # type's, kept when the type changed.)
    ball.game.physics_type = 'STATIC'
    ball.game.use_collision_bounds = bounds is not None
    if bounds is not None:
        ball.game.collision_bounds_type = bounds
    ball.game.physics_type = kind
    check(rp.keep_distance_shape(ball) == expected,
          "Keep Distance shape of %s %s: %s" % (kind, bounds, rp.keep_distance_shape(ball)))
ball.game.physics_type = 'RIGID_BODY'
ball.game.use_collision_bounds = True
ball.game.collision_bounds_type = 'BOX'
p = preview(tree, keep, owner)
check(p.data.get("shape") == 'BOX' and near(p.data.get("half"), (0.5, 0.5, 0.5)) and
      near(p.data.get("center"), (0, 5, 0)), "Keep Distance from a box: %s" % p.data)
ball.game.collision_bounds_type = 'SPHERE'
p = preview(tree, keep, owner)
check(p.data.get("shape") == 'SPHERE' and near(p.data.get("radius"), 0.5),
      "Keep Distance from a sphere: %s" % p.data)
keep.measure_from = 'SPHERE'
p = preview(tree, keep, owner)
check(p.data.get("shape") == 'SPHERE' and near(p.data.get("radius"), 0.25) and
      "half" not in p.data, "Keep Distance, Measure From Sphere: %s" % p.data)
# Pull: Distance is where the pull starts; Hold: Reach too (at least Distance, as the engine's
# LN_KeepDistanceRange), the outputs' range; a Reach only the game knows is marked.
lines_push = len(p.lines)
put(keep, "Mode", 1)  # Pull (the Mode is an input since 2026-11-15: 0 Push, 1 Pull, 2 Hold)
p = preview(tree, keep, owner)
check(p.data.get("mode") == 'PULL' and near(p.data.get("range"), 1.5) and "reach" not in p.data
      and len(p.lines) == lines_push, "Keep Distance, Pull: %s" % p.data)
put(keep, "Mode", 2)  # Hold
put(keep, "Reach", 4.0)
p = preview(tree, keep, owner)
check(p.data.get("mode") == 'HOLD' and near(p.data.get("reach"), 4.0) and
      near(p.data.get("range"), 4.0) and len(p.lines) > lines_push, "Keep Distance, Hold: %s" % p.data)
put(keep, "Reach", 1.0)
p = preview(tree, keep, owner)
check(near(p.data.get("reach"), 1.5) and len(p.lines) == lines_push,
      "Keep Distance, Hold with a Reach short of Distance: %s" % p.data)
reach_prop = add(tree, "LogicNativeGetGamePropertyValue")
tree.links.new(reach_prop.outputs[0], socket(keep, "Reach"))
p = preview(tree, keep, owner)
check(p.dynamic == ["Reach"], "Keep Distance, Reach from a game property: %s" % p.dynamic)
tree.nodes.remove(reach_prop)
put(keep, "Distance", 0.0)
put(keep, "Reach", 2.0)
p = preview(tree, keep, owner)
check(not p.problem and near(p.data.get("range"), 2.0) and p.lines,
      "Keep Distance, Hold at Distance 0 (touching): %s" % p.problem)
put(keep, "Mode", 0)  # Push
p = preview(tree, keep, owner)
check(p.problem and not p.lines, "Keep Distance with Distance 0: %s" % p.problem)
# The Mode as the engine reads it: below 0 Push, above 2 Hold; one only the game knows (a game
# property) shows both ranges, Distance and Reach.
put(keep, "Distance", 1.0)
put(keep, "Reach", 3.0)
mode_socket = socket(keep, "Mode")
mode_prop = add(tree, "LogicNativeGetGamePropertyValue")
tree.links.new(mode_prop.outputs[0], mode_socket)
p = preview(tree, keep, owner)
check(p.data.get("mode") == 'UNKNOWN' and near(p.data.get("reach"), 3.0) and
      near(p.data.get("range"), 3.0) and len(p.lines) > lines_push,
      "Keep Distance, Mode from a game property: %s" % p.data)
tree.nodes.remove(mode_prop)
for value, expected in ((-3, 'PUSH'), (7, 'HOLD')):
    mode_socket.default_value = value
    p = preview(tree, keep, owner)
    check(p.data.get("mode") == expected, "Keep Distance, Mode %d: %s" % (value, p.data))
tree.nodes.remove(keep)
bpy.data.objects.remove(ball)
# The Mode is an input beside the Falloff and Measure From dropdowns; Reach always shows.
keep = add(tree, "LogicNativeKeepDistance")
keep.falloff = 'SMOOTH'
check(not hasattr(keep, "mode"), "Keep Distance still has a Mode dropdown")
check(socket(keep, "Mode").default_value == 0, "a new Keep Distance starts in Mode %s" % (
    socket(keep, "Mode").default_value))
check(not socket(keep, "Reach").is_unavailable, "Keep Distance hides Reach")
socket(keep, "Mode").default_value = 1
check(keep.falloff == 'SMOOTH' and not socket(keep, "Reach").is_unavailable,
      "Keep Distance's Mode changed its Falloff or hid Reach")
tree.nodes.remove(keep)

# ---- Which previews each mode shows ----------------------------------------------------------
for node in list(tree.nodes):
    tree.nodes.remove(node)


def ray_node(name, visualize=False):
    node = add(tree, "LogicNativeRaycast")
    node.name = name
    put(node, "Direction", (1.0, 0.0, 0.0))
    put(node, "Visualize", visualize)
    node.select = False  # (a new node starts selected)
    return node


hidden_ray = ray_node("HiddenRay")
muted = ray_node("MutedRay", visualize=True)
muted.mute = True
check(muted.mute, "a logic node cannot be muted")
seen = ray_node("SeenRay", visualize=True)
linked_on = ray_node("LinkedOn")
bool_value = add(tree, "LogicNativeValueBool")
socket(bool_value, "Bool", True).default_value = True
link(tree, bool_value, "Bool", linked_on, "Visualize")
linked_off = ray_node("LinkedOff", visualize=True)
bool_off = add(tree, "LogicNativeValueBool")
socket(bool_off, "Bool", True).default_value = False
link(tree, bool_off, "Bool", linked_off, "Visualize")
in_game = ray_node("InGameOnly")
game_prop = add(tree, "LogicNativeGetGamePropertyValue")
tree.links.new(game_prop.outputs[0], socket(in_game, "Visualize"))
arc = add(tree, "LogicNativeAimForArc")
arc.name = "Arc"
for node in tree.nodes:
    node.select = False
second_owner = empty("Second", (0.0, 20.0, 0.0))
bind(owner, tree)
bind(second_owner, tree)
other_tree = bpy.data.node_groups.new("Unbound", "LogicNodeTree")
other_tree.use_fake_user = True
lonely_seen = add(other_tree, "LogicNativeRaycast")
lonely_seen.name = "LonelySeen"
put(lonely_seen, "Visualize", True)
lonely_hidden = add(other_tree, "LogicNativeRaycast")
lonely_hidden.name = "LonelyHidden"
for node in other_tree.nodes:
    node.select = False
depsgraph = update()
for obj in scene.objects:
    obj.select_set(False)


def shown(mode, open_trees=()):
    return sorted((p.node.name, p.owner.name if p.owner else "")
                  for p in rp.collect_previews(scene, update(), mode, open_trees=open_trees))


both = ("Owner", "Second")
check(shown('OFF') == [], "Off shows previews")
# Visualized: the nodes whose Visualize is on (a muted node runs in the game too; a linked one
# by its value; one only the game knows: shown), for every object; no switch (Aim for Arc): off.
expected = sorted((name, obj) for name in ("SeenRay", "MutedRay", "LinkedOn", "InGameOnly")
                  for obj in both)
check(shown('VISUALIZED') == expected, "Visualized: %s" % shown('VISUALIZED'))
check(rp.visualize_on(scene, in_game, owner) is None, "a game property's Visualize is known")
check(rp.visualize_on(scene, arc, owner) is False, "Aim for Arc has a Visualize")
# Selected nodes of an open tree show only on selected objects running it.
hidden_ray.select = True
arc.select = True
check(shown('VISUALIZED', [tree]) == expected,
      "Visualized + selected nodes, no object selected: %s" % shown('VISUALIZED', [tree]))
check(rp.objects_to_select(scene, [tree]) == [tree], "the panel's hint is not given")
owner.select_set(True)
got = shown('VISUALIZED', [tree])
check(got == sorted(expected + [("HiddenRay", "Owner"), ("Arc", "Owner")]),
      "Visualized + selected nodes on the selected object: %s" % got)
check(rp.objects_to_select(scene, [tree]) == [], "the hint stays with an object selected")
# Selected (the default): only the selected ray nodes of an open tree, on the selected objects.
check(shown('SELECTED') == [], "Selected without an open tree")
got = shown('SELECTED', [tree])
check(got == [("Arc", "Owner"), ("HiddenRay", "Owner")], "Selected: %s" % got)
arc.select = False
seen.select = True
second_owner.select_set(True)
got = shown('SELECTED', [tree])
check(got == sorted((name, obj) for name in ("HiddenRay", "SeenRay") for obj in both),
      "Selected, two nodes on two objects: %s" % got)
owner.select_set(False)
second_owner.select_set(False)
check(shown('SELECTED', [tree]) == [], "Selected nodes show with no object selected")
hidden_ray.select = False
seen.select = False
# An open tree no object runs: its selected ray nodes, once with no object.
lonely_hidden.select = True
check(shown('SELECTED', [other_tree]) == [("LonelyHidden", "")],
      "an open tree without objects, Selected: %s" % shown('SELECTED', [other_tree]))
check(shown('VISUALIZED', [other_tree]) ==
      sorted(expected + [("LonelyHidden", ""), ("LonelySeen", "")]),
      "an open tree without objects, Visualized: %s" % shown('VISUALIZED', [other_tree]))
lonely_hidden.select = False
second_owner.hide_set(True)
owner.hide_set(True)
check(shown('VISUALIZED') == [], "a hidden object's previews show")
owner.hide_set(False)
second_owner.hide_set(False)
check(scene.logic_ray_preview.mode == 'SELECTED', "the default mode is not Selected")
ray = hidden_ray
lonely_ray = lonely_seen

# ---- Tree edits (no update of Blender's reports them: the timer compares fingerprints) --------
fingerprint = rp.tree_fingerprint(tree)
check(rp.tree_fingerprint(tree) == fingerprint, "a fingerprint changes by itself")
put(ray, "Max Distance", 12.0)
check(rp.tree_fingerprint(tree) != fingerprint, "a socket value does not change the fingerprint")
fingerprint = rp.tree_fingerprint(tree)
ray.input_mode = 'END_POINT'
check(rp.tree_fingerprint(tree) != fingerprint, "a dropdown does not change the fingerprint")
fingerprint = rp.tree_fingerprint(tree)
feeding = add(tree, "LogicNativeValueVector")
check(rp.tree_fingerprint(tree) != fingerprint, "a new node does not change the fingerprint")
link(tree, feeding, "Vector", ray, "Origin")
fingerprint = rp.tree_fingerprint(tree)
socket(feeding, "Vector", True).default_value = (1.0, 2.0, 3.0)
check(rp.tree_fingerprint(tree) != fingerprint, "a feeding Value node's value does not count")
fingerprint = rp.tree_fingerprint(tree)
tree.links.remove(socket(ray, "Origin").links[0])
check(rp.tree_fingerprint(tree) != fingerprint, "a removed link does not change the fingerprint")
fingerprint = rp.tree_fingerprint(tree)
lonely_ray.location = (300.0, 0.0)
check(rp.tree_fingerprint(tree) == fingerprint, "another tree's edit changes the fingerprint")

# ---- The game guard ---------------------------------------------------------------------------
rp._on_game_pre()
check(rp._on_depsgraph_update not in bpy.app.handlers.depsgraph_update_post and rp._State.in_game,
      "the update handler stays in while a game plays")
rp._on_game_post()
check(bpy.app.handlers.depsgraph_update_post.count(rp._on_depsgraph_update) == 1 and
      not rp._State.in_game, "the update handler is not back after the game")

print("CHECKS", CHECKS[0])
print("PROBLEMS (%d):" % len(PROBLEMS))
for problem in PROBLEMS:
    print("  " + problem)
sys.stdout.flush()
os._exit(1 if PROBLEMS else 0)
