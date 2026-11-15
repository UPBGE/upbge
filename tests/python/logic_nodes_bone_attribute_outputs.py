# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Get Bone Attribute shows one output per attribute (2026-11-14): Text for the name and the
Inherit Scale mode, Vector for the Location Offset and every head, center and tail, Bool for the
bone's settings (node_logic_c_tier.cc, update_get_bone_attribute). Its "Value" output used to be a
text socket carrying every type, beside two outputs that meant nothing in the chosen mode. The
outputs keep their identifiers, so a link stays when the attribute changes.

    blender --background --factory-startup --python tests/python/logic_nodes_bone_attribute_outputs.py
"""

import sys

import bpy

PROBLEMS = []


def check(condition, text):
    if not condition:
        PROBLEMS.append(text)


TEXT = {"NAME", "INHERIT_SCALE"}
VECTOR = {"LOCATION", "HEAD", "HEAD_LOCAL", "HEAD_POSE", "CENTER", "CENTER_LOCAL", "CENTER_POSE",
          "TAIL", "TAIL_LOCAL", "TAIL_POSE"}
BOOL = {"INHERIT_ROTATION", "CONNECTED", "DEFORM", "USE_LOCAL_LOCATION", "USE_RELATIVE_PARENT",
        "USE_SCALE_EASING"}
# Output identifier, its name and Blender's socket type for each kind.
OUTPUTS = {"Value": ("Text", 'STRING'), "Vector": ("Vector", 'VECTOR'), "Bool": ("Bool", 'BOOLEAN')}

tree = bpy.data.node_groups.new("BoneAttributeOutputs", "LogicNodeTree")
node = tree.nodes.new("LogicNativeGetBoneAttribute")
attributes = [item.identifier for item in node.bl_rna.properties["attribute"].enum_items]
check(set(attributes) == TEXT | VECTOR | BOOL,
      "attributes without an expected output (add them here): %s / missing: %s" % (
          sorted(set(attributes) - (TEXT | VECTOR | BOOL)),
          sorted((TEXT | VECTOR | BOOL) - set(attributes))))

for identifier, (name, socket_type) in OUTPUTS.items():
    output = next((s for s in node.outputs if s.identifier == identifier), None)
    check(output is not None, "no output %r" % identifier)
    if output is not None:
        check(output.name == name, "output %r is named %r, not %r" % (identifier, output.name, name))
        check(output.type == socket_type,
              "output %r is a %s socket, not %s" % (identifier, output.type, socket_type))

for attribute in attributes:
    node.attribute = attribute
    want = "Value" if attribute in TEXT else "Vector" if attribute in VECTOR else "Bool"
    shown = [s.identifier for s in node.outputs if s.enabled]
    check(shown == [want], "%s shows %s, not [%r]" % (attribute, shown, want))

# Location's Space (2026-11-14): World by default (also every node made before it: custom2 bits
# 1-2 are 0), the three spaces of Get Pose Bone Transform, apart from Use World Space's bit.
fresh = tree.nodes.new("LogicNativeGetBoneAttribute")
check(fresh.location_space == 'WORLD', "a new node's Location space is %s" % fresh.location_space)
check([i.identifier for i in fresh.bl_rna.properties["location_space"].enum_items] ==
      ['POSE_CHANNEL', 'ARMATURE', 'WORLD'],
      "Location spaces: %s" % [i.identifier for i in fresh.bl_rna.properties["location_space"].enum_items])
for space in ('POSE_CHANNEL', 'ARMATURE', 'WORLD'):
    for world_space in (False, True):
        fresh.location_space = space
        fresh.world_space = world_space
        check(fresh.location_space == space and fresh.world_space == world_space,
              "Space %s and Use World Space %s do not keep apart: %s, %s" % (
                  space, world_space, fresh.location_space, fresh.world_space))
tree.nodes.remove(fresh)

# A link stays on its output when the attribute changes (it is only hidden), and shows again.
printer = tree.nodes.new("LogicNativePrint")
message = next(s for s in printer.inputs if s.identifier == "Message")
node.attribute = 'NAME'
link = tree.links.new(next(s for s in node.outputs if s.identifier == "Value"), message)
node.attribute = 'HEAD_POSE'
node.attribute = 'NAME'
check(len(tree.links) == 1 and tree.links[0].from_socket.identifier == "Value",
      "the Text link did not survive a mode change: %s" % [
          (l.from_socket.identifier, l.to_socket.identifier) for l in tree.links])

print("PROBLEMS (%d):" % len(PROBLEMS))
for problem in PROBLEMS:
    print("  " + problem)
sys.exit(1 if PROBLEMS else 0)
