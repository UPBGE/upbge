# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

bl_info = {
    "name": "Normal Map nodes to Custom",
    "author": "Spooky spooky Ghostman, Kamikaze, crute, Mustard",
    "description": "Replace Normal Nodes for better EEVEE Viewport-Performance",
    "blender": (4, 00, 0),
    "version": (0, 2, 0),
    "location": "Tools Panel (T) in Shader Editor",
    "warning": "",
    "category": "Material",
}



from mathutils import Color, Vector
import bpy




class MAT_OT_custom_normal(bpy.types.Operator):
    bl_description = "Switch normal map nodes to a faster custom node"
    bl_idname = 'node.normal_map_group'
    bl_label = "Normal Map nodes to Custom"
    bl_options = {'UNDO'}

    @classmethod
    def poll(self, context):
        return (bpy.data.materials or bpy.data.node_groups)

    def execute(self, context):
        def mirror(new, old):
            """Copy attributes of the old node to the new node"""
            new.parent = old.parent
            new.label = old.label
            new.mute = old.mute
            new.hide = old.hide
            new.select = old.select
            new.location = old.location

            # inputs
            for (name, point) in old.inputs.items():
                input = new.inputs.get(name)
                if input:
                    input.default_value = point.default_value
                    for link in point.links:
                        new.id_data.links.new(link.from_socket, input)

            # outputs
            for (name, point) in old.outputs.items():
                output = new.outputs.get(name)
                if output:
                    output.default_value = point.default_value
                    for link in point.links:
                        new.id_data.links.new(output, link.to_socket)

        def get_custom():
            name = 'Normal Map Optimized'
            group = bpy.data.node_groups.get(name)

            if not group and self.custom:
                group = default_custom_nodes()

            return group

        def set_custom(nodes):
            group = get_custom()
            if not group:
                return

            for node in reversed(nodes):
                new = None
                if self.custom:
                    if isinstance(node, bpy.types.ShaderNodeNormalMap):
                        new = nodes.new(type='ShaderNodeGroup')
                        new.node_tree = group
                else:
                    if isinstance(node, bpy.types.ShaderNodeGroup):
                        if node.node_tree == group:
                            new = nodes.new(type='ShaderNodeNormalMap')

                if new:
                    name = node.name
                    mirror(new, node)
                    
                    if isinstance(node, bpy.types.ShaderNodeNormalMap):
                        uvNode = nodes.new('ShaderNodeUVMap')
                        uvNode.uv_map = node.uv_map
                        uvNode.name = node.name+" UV"
                        uvNode.parent = new.parent
                        uvNode.mute = True
                        uvNode.hide = True
                        uvNode.select = False
                        uvNode.location = Vector((new.location.x-200., new.location.y-10.))
                        uvNode.id_data.links.new(uvNode.outputs['UV'], new.inputs[2])
                    else:
                        try:
                            for input in node.inputs:
                                if input and isinstance(input, bpy.types.NodeSocketVector) and input.is_linked:
                                    if isinstance(input.links[0].from_node, bpy.types.ShaderNodeUVMap):
                                        uvNode = input.links[0].from_node
                            new.uv_map = uvNode.uv_map
                            nodes.remove(uvNode)
                        except:
                            pass
                    
                    nodes.remove(node)
                    new.name = name

        for mat in bpy.data.materials:
            set_custom(getattr(mat.node_tree, 'nodes', []))
        for group in bpy.data.node_groups:
            set_custom(group.nodes)

        if (not self.custom) and get_custom():
            bpy.data.node_groups.remove(get_custom())

        return {'FINISHED'}

    custom: bpy.props.BoolProperty(
        name="To Custom",
        description="Set all normals to custom group, or revert back to normal",
        default=True,
    )


class MUT_PT_normal_map_nodes(bpy.types.Panel):
    bl_category = ""
    bl_label = ""
    bl_options = {'HIDE_HEADER'}
    bl_region_type = 'TOOLS'
    bl_space_type = 'NODE_EDITOR'

    @classmethod
    def poll(self, context):
        return True

    def draw(self, context):
        layout = self.layout

        col = layout.column(align=True)
        tog = MAT_OT_custom_normal.bl_idname
        col.operator(tog, text="Custom").custom = True
        col.operator(tog, text="Normal").custom = False


def default_custom_nodes():
    use_new_nodes = bpy.app.version >= (2, 81) and bpy.app.version < (3, 2, 0)

    group = bpy.data.node_groups.new('Normal Map Optimized', 'ShaderNodeTree')

    nodes = group.nodes
    links = group.links

    # Input
    input = group.interface.new_socket("Strength", in_out='INPUT', socket_type='NodeSocketFloat')
    input.default_value = 1.0
    input.min_value = 0.0
    input.max_value = 1.0
    input = group.interface.new_socket("Color", in_out='INPUT', socket_type='NodeSocketColor')
    input.default_value = ((0.5, 0.5, 1.0, 1.0))
    
    # Input UV as Backup
    input = group.interface.new_socket("UV", in_out='INPUT', socket_type='NodeSocketVector')
    
    # Output
    group.interface.new_socket("Normal", in_out='OUTPUT', socket_type='NodeSocketVector')

    # Add Nodes
    frame = nodes.new('NodeFrame')
    frame.name = 'Matrix * Normal Map'
    frame.label = 'Matrix * Normal Map'
    frame.location = Vector((540.0, -80.0))
    frame.hide = False
    frame.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node = nodes.new('ShaderNodeVectorMath')
    node.name = 'Vector Math'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-60.0, 20.0))
    node.operation = 'DOT_PRODUCT'
    node.inputs[0].default_value = (0.5, 0.5, 0.5)  # Vector
    node.inputs[1].default_value = (0.5, 0.5, 0.5)  # Vector
    if use_new_nodes:
        node.inputs[2].default_value = (1.0, 1.0, 1.0)  # Scale
    node = nodes.new('ShaderNodeVectorMath')
    node.name = 'Vector Math.001'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-60.0, -20.0))
    node.operation = 'DOT_PRODUCT'
    node.inputs[0].default_value = (0.5, 0.5, 0.5)  # Vector
    node.inputs[1].default_value = (0.5, 0.5, 0.5)  # Vector
    if use_new_nodes:
        node.inputs[2].default_value = (1.0, 1.0, 1.0)  # Scale
    node = nodes.new('ShaderNodeVectorMath')
    node.name = 'Vector Math.002'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-60.0, -60.0))
    node.inputs[0].default_value = (0.5, 0.5, 0.5)  # Vector
    node.inputs[1].default_value = (0.5, 0.5, 0.5)  # Vector
    if use_new_nodes:
        node.inputs[2].default_value = (1.0, 1.0, 1.0)  # Scale
    node.operation = 'DOT_PRODUCT'
    node = nodes.new('ShaderNodeCombineXYZ')
    node.name = 'Combine XYZ'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((100.0, -20.0))
    node.inputs[0].default_value = 0.0  # X
    node.inputs[1].default_value = 0.0  # Y
    node.inputs[2].default_value = 0.0  # Z

    frame = nodes.new('NodeFrame')
    frame.name = 'Generate TBN from Bump Node'
    frame.label = 'Generate TBN from Bump Node'
    frame.location = Vector((-192.01412963867188, -77.50459289550781))
    frame.hide = False
    frame.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node = nodes.new('ShaderNodeUVMap')
    node.name = 'UV Map'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-247.98587036132812, -2.4954071044921875))
    node = nodes.new('ShaderNodeSeparateXYZ')
    node.name = 'UV Gradients'
    node.label = 'UV Gradients'
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-87.98587036132812, -2.4954071044921875))
    node.inputs[0].default_value = (0.0, 0.0, 0.0)  # Vector
    # node.outputs.remove((node.outputs['Z']))
    node = nodes.new('ShaderNodeNewGeometry')
    node.name = 'Normal'
    node.label = 'Normal'
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((72.01412963867188, -62.49540710449219))
    # for out in node.outputs:
    #     if out.name not in ['Normal']:
    #         node.outputs.remove(out)
    node = nodes.new('ShaderNodeBump')
    node.name = 'Bi-Tangent'
    node.label = 'Bi-Tangent'
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((72.01412963867188, -22.495407104492188))
    node.invert = True
    node.inputs[0].default_value = 1.0  # Strength
    node.inputs[1].default_value = 1000.0  # Distance
    node.inputs[2].default_value = 1.0  # Height
    if use_new_nodes:
        node.inputs[3].default_value = 1.0  # Height_dx
        node.inputs[4].default_value = 1.0  # Height_dy
        node.inputs[5].default_value = (0.0, 0.0, 0.0)  # Normal
    else:
        node.inputs[3].default_value = (0.0, 0.0, 0.0)  # Normal
    # for inp in node.inputs:
    #     if inp.name not in ['Height']:
    #         node.inputs.remove(inp)
    node = nodes.new('ShaderNodeBump')
    node.name = 'Tangent'
    node.label = 'Tangent'
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((72.01412963867188, 17.504592895507812))
    node.invert = True
    # for inp in node.inputs:
    #     if inp.name not in ['Height']:
    #         node.inputs.remove(inp)

    frame = nodes.new('NodeFrame')
    frame.name = 'Node'
    frame.label = 'Normal Map Processing'
    frame.location = Vector((180.0, -260.0))
    frame.hide = False
    frame.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node = nodes.new('NodeGroupInput')
    node.name = 'Group Input'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-400.0, 20.0))
    node = nodes.new('ShaderNodeMixRGB')
    node.name = 'Influence'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.location = Vector((-240.0, 20.0))
    node.inputs[1].default_value = (0.5, 0.5, 1.0, 1.0)  # Color1
    node = nodes.new('ShaderNodeVectorMath')
    node.name = 'Vector Math.003'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-80.0, 20.0))
    node.operation = 'SUBTRACT'
    node.inputs[0].default_value = (0.5, 0.5, 0.5)  # Vector
    node.inputs[1].default_value = (0.5, 0.5, 0.5)  # Vector
    if use_new_nodes:
        node.inputs[2].default_value = (1.0, 1.0, 1.0)  # Scale
    # node.inputs.remove(node.inputs[1])
    node = nodes.new('ShaderNodeVectorMath')
    node.name = 'Vector Math.004'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((80.0, 20.0))
    node.inputs[0].default_value = (0.5, 0.5, 0.5)  # Vector
    node.inputs[1].default_value = (0.5, 0.5, 0.5)  # Vector
    if use_new_nodes:
        node.inputs[2].default_value = (1.0, 1.0, 1.0)  # Scale

    frame = nodes.new('NodeFrame')
    frame.name = 'Transpose Matrix'
    frame.label = 'Transpose Matrix'
    frame.location = Vector((180.0, -80.0))
    frame.hide = False
    frame.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node = nodes.new('ShaderNodeCombineXYZ')
    node.name = 'Combine XYZ.001'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((80.0, 20.0))
    node.inputs[0].default_value = 0.0  # X
    node.inputs[1].default_value = 0.0  # Y
    node.inputs[2].default_value = 0.0  # Z
    node = nodes.new('ShaderNodeCombineXYZ')
    node.name = 'Combine XYZ.002'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((80.0, -20.0))
    node.inputs[0].default_value = 0.0  # X
    node.inputs[1].default_value = 0.0  # Y
    node.inputs[2].default_value = 0.0  # Z
    node = nodes.new('ShaderNodeCombineXYZ')
    node.name = 'Combine XYZ.003'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((80.0, -60.0))
    node.inputs[0].default_value = 0.0  # X
    node.inputs[1].default_value = 0.0  # Y
    node.inputs[2].default_value = 0.0  # Z
    node = nodes.new('ShaderNodeSeparateXYZ')
    node.name = 'Separate XYZ.001'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-80.0, 20.0))
    node.inputs[0].default_value = (0.0, 0.0, 0.0)  # Vector
    node = nodes.new('ShaderNodeSeparateXYZ')
    node.name = 'Separate XYZ.002'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-80.0, -20.0))
    node.inputs[0].default_value = (0.0, 0.0, 0.0)  # Vector
    node = nodes.new('ShaderNodeSeparateXYZ')
    node.name = 'Separate XYZ.003'
    node.label = ''
    node.parent = frame
    node.hide = True
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.location = Vector((-80.0, -60.0))
    node.inputs[0].default_value = (0.0, 0.0, 0.0)  # Vector

    node = nodes.new('NodeGroupOutput')
    node.name = 'Group Output'
    node.label = ''
    node.location = Vector((840.0, -80.0))
    node.hide = False
    node.color = Color((0.6079999804496765, 0.6079999804496765, 0.6079999804496765))
    node.inputs[0].default_value = (0.0, 0.0, 0.0)  # Normal

    # Connect the nodes
    links.new(nodes['Group Input'].outputs['Strength'], nodes['Influence'].inputs[0])
    links.new(nodes['Group Input'].outputs['Color'], nodes['Influence'].inputs[2])
    links.new(nodes['Influence'].outputs['Color'], nodes['Vector Math.003'].inputs[0])
    links.new(nodes['UV Gradients'].outputs['X'], nodes['Tangent'].inputs['Height'])
    links.new(nodes['UV Gradients'].outputs['Y'], nodes['Bi-Tangent'].inputs['Height'])
    links.new(nodes['UV Map'].outputs['UV'], nodes['UV Gradients'].inputs['Vector'])
    links.new(nodes['Tangent'].outputs['Normal'], nodes['Separate XYZ.001'].inputs[0])
    links.new(nodes['Bi-Tangent'].outputs['Normal'], nodes['Separate XYZ.002'].inputs[0])
    links.new(nodes['Normal'].outputs['Normal'], nodes['Separate XYZ.003'].inputs[0])
    links.new(nodes['Vector Math.004'].outputs['Vector'], nodes['Vector Math'].inputs[1])
    links.new(nodes['Combine XYZ.001'].outputs['Vector'], nodes['Vector Math'].inputs[0])
    links.new(nodes['Vector Math.004'].outputs['Vector'], nodes['Vector Math.001'].inputs[1])
    links.new(nodes['Combine XYZ.002'].outputs['Vector'], nodes['Vector Math.001'].inputs[0])
    links.new(nodes['Vector Math.004'].outputs['Vector'], nodes['Vector Math.002'].inputs[1])
    links.new(nodes['Combine XYZ.003'].outputs['Vector'], nodes['Vector Math.002'].inputs[0])
    links.new(nodes['Vector Math.003'].outputs['Vector'], nodes['Vector Math.004'].inputs[0])
    links.new(nodes['Vector Math.003'].outputs['Vector'], nodes['Vector Math.004'].inputs[1])
    links.new(nodes['Vector Math'].outputs['Value'], nodes['Combine XYZ'].inputs['X'])
    links.new(nodes['Vector Math.001'].outputs['Value'], nodes['Combine XYZ'].inputs['Y'])
    links.new(nodes['Vector Math.002'].outputs['Value'], nodes['Combine XYZ'].inputs['Z'])
    links.new(nodes['Separate XYZ.001'].outputs['X'], nodes['Combine XYZ.001'].inputs['X'])
    links.new(nodes['Separate XYZ.002'].outputs['X'], nodes['Combine XYZ.001'].inputs['Y'])
    links.new(nodes['Separate XYZ.003'].outputs['X'], nodes['Combine XYZ.001'].inputs['Z'])
    links.new(nodes['Separate XYZ.001'].outputs['Y'], nodes['Combine XYZ.002'].inputs['X'])
    links.new(nodes['Separate XYZ.002'].outputs['Y'], nodes['Combine XYZ.002'].inputs['Y'])
    links.new(nodes['Separate XYZ.003'].outputs['Y'], nodes['Combine XYZ.002'].inputs['Z'])
    links.new(nodes['Separate XYZ.001'].outputs['Z'], nodes['Combine XYZ.003'].inputs['X'])
    links.new(nodes['Separate XYZ.002'].outputs['Z'], nodes['Combine XYZ.003'].inputs['Y'])
    links.new(nodes['Separate XYZ.003'].outputs['Z'], nodes['Combine XYZ.003'].inputs['Z'])
    links.new(nodes['Combine XYZ'].outputs['Vector'], nodes['Group Output'].inputs['Normal'])

    return group


def register():
    bpy.utils.register_class(MAT_OT_custom_normal)
    bpy.utils.register_class(MUT_PT_normal_map_nodes)


def unregister():
    bpy.utils.unregister_class(MAT_OT_custom_normal)
    bpy.utils.unregister_class(MUT_PT_normal_map_nodes)


if __name__ == "__main__":
    register()
