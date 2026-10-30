# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Game rendering helpers (UPBGE)."""

from bpy.types import Operator


def _input_value(node, name):
    """The value of an unlinked input, else None."""
    socket = node.inputs.get(name)
    if socket is None or socket.is_linked:
        return None
    return socket.default_value


def _surface_shader(material):
    """The shader node plugged into the material's EEVEE output, if any."""
    if material.node_tree is None:
        return None
    output = material.node_tree.get_output_node('EEVEE')
    if output is None or not output.inputs["Surface"].is_linked:
        return None
    return output.inputs["Surface"].links[0].from_node


def _copy_material_color(material):
    """Give the material the Viewport Display color of its shader. True when it changed."""
    shader = _surface_shader(material)
    if shader is None:
        return False
    if shader.type == 'BSDF_PRINCIPLED':
        color = _input_value(shader, "Base Color")
        alpha = _input_value(shader, "Alpha")
        metallic = _input_value(shader, "Metallic")
        roughness = _input_value(shader, "Roughness")
    elif shader.type in {'EMISSION', 'BSDF_DIFFUSE'}:
        color = _input_value(shader, "Color")
        alpha = metallic = roughness = None
    else:
        return False
    if color is None:
        # A linked color (an image): Workbench draws the material's first image.
        return False
    new_color = (color[0], color[1], color[2], material.diffuse_color[3] if alpha is None else alpha)
    changed = tuple(material.diffuse_color) != tuple(new_color)
    material.diffuse_color = new_color
    if metallic is not None and material.metallic != metallic:
        material.metallic = metallic
        changed = True
    if roughness is not None and material.roughness != roughness:
        material.roughness = roughness
        changed = True
    return changed


def _copy_world_color(world):
    """Give the world the Viewport Display color of its Background shader. True when it changed."""
    if world is None or world.node_tree is None:
        return False
    output = world.node_tree.get_output_node('EEVEE')
    if output is None or not output.inputs["Surface"].is_linked:
        return False
    background = output.inputs["Surface"].links[0].from_node
    if background.type != 'BACKGROUND':
        return False
    color = _input_value(background, "Color")
    if color is None:
        return False
    new_color = (color[0], color[1], color[2])
    changed = tuple(world.color) != new_color
    world.color = new_color
    return changed


class SCENE_OT_game_workbench_copy_colors(Operator):
    """Give the materials of this scene's objects, and its world, the Viewport Display colors of """ \
        """their shaders (Base Color of a Principled BSDF, Color of an Emission, Diffuse or """ \
        """Background shader). Workbench views (a camera's Use Workbench) draw with these """ \
        """colors. Materials whose color comes from an image keep theirs: Workbench draws the image"""
    bl_idname = "scene.game_workbench_copy_colors"
    bl_label = "Copy Material Colors for Workbench"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        scene = context.scene
        materials = {
            slot.material
            for obj in scene.objects
            for slot in obj.material_slots
            if slot.material is not None
        }
        changed = sum(1 for material in materials if _copy_material_color(material))
        world_changed = _copy_world_color(scene.world)
        self.report(
            {'INFO'},
            "Viewport Display colors updated: {:d} of {:d} materials{:s}".format(
                changed, len(materials), ", and the world" if world_changed else "",
            ),
        )
        return {'FINISHED'}


classes = (
    SCENE_OT_game_workbench_copy_colors,
)
