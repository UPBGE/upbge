# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Set Material Parameter's cached Material (its hidden Material socket in Per Object Only mode,
written by the node's draw and update: node_logic_c_tier.cc, update_set_node_socket_for_context)
keeps its Material's user count right (2026-11-13).

With an empty Object the node stands for the active object, which only a draw (with a context)
knows. The node's update has no context and Blender also runs it while it reads an undo step,
before it counts users: it wrote "no material" there and took a user from a Material at 0 users,
"ID user decrement error ... 0 <= 0" once per node at every undo, the one that ends an embedded
game included. The update now leaves the cache to the next draw; with an Object set it still
follows that object.

    blender --background --factory-startup --python tests/python/logic_nodes_material_cache.py
"""

import sys

import bpy

PROBLEMS = []


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


def socket(node, identifier):
    return next(s for s in node.inputs if s.identifier == identifier)


def material_named(name):
    return bpy.data.materials[name]


def run_updates(tree, node):
    """The node tree's update without a context, now (a link change runs every node's update
    at once; a socket value set from Python waits for the next one)."""
    step = tree.nodes.new("LogicNativeOnFixedUpdate")
    link = tree.links.new(step.outputs[0], socket(node, "Flow"))
    tree.links.remove(link)
    tree.nodes.remove(step)


for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj)

red = bpy.data.materials.new("Red")
blue = bpy.data.materials.new("Blue")
bpy.ops.mesh.primitive_cube_add()
red_cube = bpy.context.active_object
red_cube.name = "RedCube"
red_cube.data.materials.append(red)
bpy.ops.mesh.primitive_cube_add(location=(3.0, 0.0, 0.0))
blue_cube = bpy.context.active_object
blue_cube.name = "BlueCube"
blue_cube.data.materials.append(blue)

tree = bpy.data.node_groups.new("MaterialCache", "LogicNodeTree")
tree.use_fake_user = True
node = tree.nodes.new("LogicNativeSetMaterialParameter")
check(node.per_object_only, "Set Material Parameter starts Per Object Only")

# The draw's cache with RedCube active (the RNA setter counts the socket's user, as the draw does).
socket(node, "Material").default_value = red
check(red.users == 2, "the cached Red holds a user: %d" % red.users)

# An update without a context: the active object is unknown, the cache waits for the draw.
run_updates(tree, node)
check(socket(node, "Material").default_value == red and red.users == 2,
      "an update without a context cleared the cache: %s, Red users %d"
      % (socket(node, "Material").default_value, red.users))

# An undo (Blender reads the step and updates every node tree before it counts users).
check(bpy.ops.ed.undo_push(message="cached") == {'FINISHED'}, "undo push")
red_cube.location.x = 1.0
check(bpy.ops.ed.undo_push(message="moved") == {'FINISHED'}, "undo push")
check(bpy.ops.ed.undo() == {'FINISHED'}, "undo")
tree = bpy.data.node_groups["MaterialCache"]
node = tree.nodes[0]
red = material_named("Red")
check(socket(node, "Material").default_value == red and red.users == 2,
      "an undo cleared the cache (and took Red's user before users were counted): %s, Red "
      "users %d" % (socket(node, "Material").default_value, red.users))

# An Object set: the update knows the slot without a context and follows it, users moving with
# it.
blue = material_named("Blue")
socket(node, "Object").default_value = bpy.data.objects["BlueCube"]
run_updates(tree, node)
check(socket(node, "Material").default_value == blue,
      "the cache does not follow the Object: %s" % socket(node, "Material").default_value)
check(red.users == 1 and blue.users == 2,
      "users after following the Object: Red %d, Blue %d" % (red.users, blue.users))
socket(node, "Object").default_value = bpy.data.objects["RedCube"]
run_updates(tree, node)
check(socket(node, "Material").default_value == red and red.users == 2 and blue.users == 1,
      "users after following back: Red %d, Blue %d" % (red.users, blue.users))

# Deleting the node gives the cached Material's user back.
tree.nodes.remove(node)
check(red.users == 1 and blue.users == 1,
      "users after deleting the node: Red %d, Blue %d" % (red.users, blue.users))

print("PROBLEMS (%d):" % len(PROBLEMS))
for problem in PROBLEMS:
    print("  " + problem)
sys.exit(1 if PROBLEMS else 0)
