# SPDX-License-Identifier: GPL-2.0-or-later

from mathutils import Vector
import bpy
from bpy.types import Operator
from bpy.props import (
    BoolProperty,
    EnumProperty,
    FloatProperty,
    IntProperty,
)


def object_ensure_material(obj, mat_name):
    """ Use an existing material or add a new one.
    """
    mat = mat_slot = None
    for mat_slot in obj.material_slots:
        mat = mat_slot.material
        if mat:
            break
    if mat is None:
        mat = bpy.data.materials.new(mat_name)
        if mat_slot:
            mat_slot.material = mat
        else:
            obj.data.materials.append(mat)
    return mat


class ObjectModeOperator:
    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'


class QuickFur(ObjectModeOperator, Operator):
    """Add fur setup to the selected objects"""
    bl_idname = "object.quick_fur"
    bl_label = "Quick Fur"
    bl_options = {'REGISTER', 'UNDO'}

    density: EnumProperty(
        name="Fur Density",
        items=(
            ('LIGHT', "Light", ""),
            ('MEDIUM', "Medium", ""),
            ('HEAVY', "Heavy", "")
        ),
        default='MEDIUM',
    )
    view_percentage: IntProperty(
        name="View %",
        min=1, max=100,
        soft_min=1, soft_max=100,
        default=10,
    )
    length: FloatProperty(
        name="Length",
        min=0.001, max=100,
        soft_min=0.01, soft_max=10,
        default=0.1,
    )

    def execute(self, context):
        fake_context = context.copy()
        mesh_objects = [obj for obj in context.selected_objects
                        if obj.type == 'MESH']

        if not mesh_objects:
            self.report({'ERROR'}, "Select at least one mesh object")
            return {'CANCELLED'}

        mat = bpy.data.materials.new("Fur Material")

        for obj in mesh_objects:
            fake_context["object"] = obj
            bpy.ops.object.particle_system_add(fake_context)

            psys = obj.particle_systems[-1]
            psys.settings.type = 'HAIR'

            if self.density == 'LIGHT':
                psys.settings.count = 100
            elif self.density == 'MEDIUM':
                psys.settings.count = 1000
            elif self.density == 'HEAVY':
                psys.settings.count = 10000

            psys.settings.child_nbr = self.view_percentage
            psys.settings.hair_length = self.length
            psys.settings.use_strand_primitive = True
            psys.settings.use_hair_bspline = True
            psys.settings.child_type = 'INTERPOLATED'
            psys.settings.tip_radius = 0.25

            obj.data.materials.append(mat)
            psys.settings.material = len(obj.data.materials)

        return {'FINISHED'}


class QuickExplode(ObjectModeOperator, Operator):
    """Make selected objects explode"""
    bl_idname = "object.quick_explode"
    bl_label = "Quick Explode"
    bl_options = {'REGISTER', 'UNDO'}

    style: EnumProperty(
        name="Explode Style",
        items=(
            ('EXPLODE', "Explode", ""),
            ('BLEND', "Blend", ""),
        ),
        default='EXPLODE',
    )
    amount: IntProperty(
        name="Number of Pieces",
        min=2, max=10000,
        soft_min=2, soft_max=10000,
        default=100,
    )
    frame_duration: IntProperty(
        name="Duration",
        min=1, max=300000,
        soft_min=1, soft_max=10000,
        default=50,
    )

    frame_start: IntProperty(
        name="Start Frame",
        min=1, max=300000,
        soft_min=1, soft_max=10000,
        default=1,
    )
    frame_end: IntProperty(
        name="End Frame",
        min=1, max=300000,
        soft_min=1, soft_max=10000,
        default=10,
    )

    velocity: FloatProperty(
        name="Outwards Velocity",
        min=0, max=300000,
        soft_min=0, soft_max=10,
        default=1,
    )

    fade: BoolProperty(
        name="Fade",
        description="Fade the pieces over time",
        default=True,
    )

    def execute(self, context):
        fake_context = context.copy()
        obj_act = context.active_object

        if obj_act is None or obj_act.type != 'MESH':
            self.report({'ERROR'}, "Active object is not a mesh")
            return {'CANCELLED'}

        mesh_objects = [obj for obj in context.selected_objects
                        if obj.type == 'MESH' and obj != obj_act]
        mesh_objects.insert(0, obj_act)

        if self.style == 'BLEND' and len(mesh_objects) != 2:
            self.report({'ERROR'}, "Select two mesh objects")
            self.style = 'EXPLODE'
            return {'CANCELLED'}
        elif not mesh_objects:
            self.report({'ERROR'}, "Select at least one mesh object")
            return {'CANCELLED'}

        for obj in mesh_objects:
            if obj.particle_systems:
                self.report({'ERROR'},
                            "Object %r already has a "
                            "particle system" % obj.name)

                return {'CANCELLED'}

        if self.style == 'BLEND':
            from_obj = mesh_objects[1]
            to_obj = mesh_objects[0]

        for obj in mesh_objects:
            fake_context["object"] = obj
            bpy.ops.object.particle_system_add(fake_context)

            settings = obj.particle_systems[-1].settings
            settings.count = self.amount
            # first set frame end, to prevent frame start clamping
            settings.frame_end = self.frame_end - self.frame_duration
            settings.frame_start = self.frame_start
            settings.lifetime = self.frame_duration
            settings.normal_factor = self.velocity
            settings.render_type = 'NONE'

            explode = obj.modifiers.new(name='Explode', type='EXPLODE')
            explode.use_edge_cut = True

            if self.fade:
                explode.show_dead = False
                uv = obj.data.uv_layers.new(name="Explode fade")
                explode.particle_uv = uv.name

                mat = object_ensure_material(obj, "Explode Fade")
                mat.blend_method = 'BLEND'
                mat.shadow_method = 'HASHED'
                if not mat.use_nodes:
                    mat.use_nodes = True

                nodes = mat.node_tree.nodes
                for node in nodes:
                    if node.type == 'OUTPUT_MATERIAL':
                        node_out_mat = node
                        break

                node_surface = node_out_mat.inputs['Surface'].links[0].from_node

                node_x = node_surface.location[0]
                node_y = node_surface.location[1] - 400
                offset_x = 200

                node_mix = nodes.new('ShaderNodeMixShader')
                node_mix.location = (node_x - offset_x, node_y)
                mat.node_tree.links.new(node_surface.outputs[0], node_mix.inputs[1])
                mat.node_tree.links.new(node_mix.outputs["Shader"], node_out_mat.inputs['Surface'])
                offset_x += 200

                node_trans = nodes.new('ShaderNodeBsdfTransparent')
                node_trans.location = (node_x - offset_x, node_y)
                mat.node_tree.links.new(node_trans.outputs["BSDF"], node_mix.inputs[2])
                offset_x += 200

                node_ramp = nodes.new('ShaderNodeValToRGB')
                node_ramp.location = (node_x - offset_x, node_y)
                offset_x += 200
                mat.node_tree.links.new(node_ramp.outputs["Alpha"], node_mix.inputs["Fac"])
                color_ramp = node_ramp.color_ramp
                color_ramp.elements[0].color[3] = 0.0
                color_ramp.elements[1].color[3] = 1.0

                if self.style == 'BLEND':
                    color_ramp.elements[0].position = 0.333
                    color_ramp.elements[1].position = 0.666
                    if obj == to_obj:
                        # reverse ramp alpha
                        color_ramp.elements[0].color[3] = 1.0
                        color_ramp.elements[1].color[3] = 0.0

                node_sep = nodes.new('ShaderNodeSeparateXYZ')
                node_sep.location = (node_x - offset_x, node_y)
                offset_x += 200
                mat.node_tree.links.new(node_sep.outputs["X"], node_ramp.inputs["Fac"])

                node_uv = nodes.new('ShaderNodeUVMap')
                node_uv.location = (node_x - offset_x, node_y)
                node_uv.uv_map = uv.name
                mat.node_tree.links.new(node_uv.outputs["UV"], node_sep.inputs["Vector"])

            if self.style == 'BLEND':
                settings.physics_type = 'KEYED'
                settings.use_emit_random = False
                settings.rotation_mode = 'NOR'

                psys = obj.particle_systems[-1]

                fake_context["particle_system"] = obj.particle_systems[-1]
                bpy.ops.particle.new_target(fake_context)
                bpy.ops.particle.new_target(fake_context)

                if obj == from_obj:
                    psys.targets[1].object = to_obj
                else:
                    psys.targets[0].object = from_obj
                    settings.normal_factor = -self.velocity
                    explode.show_unborn = False
                    explode.show_dead = True
            else:
                settings.factor_random = self.velocity
                settings.angular_velocity_factor = self.velocity / 10.0

        return {'FINISHED'}

    def invoke(self, context, _event):
        self.frame_start = context.scene.frame_current
        self.frame_end = self.frame_start + self.frame_duration
        return self.execute(context)


def obj_bb_minmax(obj, min_co, max_co):
    for i in range(0, 8):
        bb_vec = obj.matrix_world @ Vector(obj.bound_box[i])

        min_co[0] = min(bb_vec[0], min_co[0])
        min_co[1] = min(bb_vec[1], min_co[1])
        min_co[2] = min(bb_vec[2], min_co[2])
        max_co[0] = max(bb_vec[0], max_co[0])
        max_co[1] = max(bb_vec[1], max_co[1])
        max_co[2] = max(bb_vec[2], max_co[2])


def grid_location(x, y):
    return (x * 200, y * 150)


class QuickSmoke(ObjectModeOperator, Operator):
    """Use selected objects as smoke emitters"""
    bl_idname = "object.quick_smoke"
    bl_label = "Quick Smoke"
    bl_options = {'REGISTER', 'UNDO'}

    style: EnumProperty(
        name="Smoke Style",
        items=(
            ('SMOKE', "Smoke", ""),
            ('FIRE', "Fire", ""),
            ('BOTH', "Smoke & Fire", ""),
        ),
        default='SMOKE',
    )

    show_flows: BoolProperty(
        name="Render Smoke Objects",
        description="Keep the smoke objects visible during rendering",
        default=False,
    )

    def execute(self, context):
        if not bpy.app.build_options.fluid:
            self.report({'ERROR'}, "Built without Fluid modifier")
            return {'CANCELLED'}

        fake_context = context.copy()
        mesh_objects = [obj for obj in context.selected_objects
                        if obj.type == 'MESH']
        min_co = Vector((100000.0, 100000.0, 100000.0))
        max_co = -min_co

        if not mesh_objects:
            self.report({'ERROR'}, "Select at least one mesh object")
            return {'CANCELLED'}

        for obj in mesh_objects:
            fake_context["object"] = obj
            # make each selected object a smoke flow
            bpy.ops.object.modifier_add(fake_context, type='FLUID')
            obj.modifiers[-1].fluid_type = 'FLOW'

            # set type
            obj.modifiers[-1].flow_settings.flow_type = self.style

            # set flow behavior
            obj.modifiers[-1].flow_settings.flow_behavior = 'INFLOW'

            # use some surface distance for smoke emission
            obj.modifiers[-1].flow_settings.surface_distance = 1.5

            if not self.show_flows:
                obj.display_type = 'WIRE'

            # store bounding box min/max for the domain object
            obj_bb_minmax(obj, min_co, max_co)

        # add the smoke domain object
        bpy.ops.mesh.primitive_cube_add()
        obj = context.active_object
        obj.name = "Smoke Domain"

        # give the smoke some room above the flows
        obj.location = 0.5 * (max_co + min_co) + Vector((0.0, 0.0, 1.0))
        obj.scale = 0.5 * (max_co - min_co) + Vector((1.0, 1.0, 2.0))

        # setup smoke domain
        bpy.ops.object.modifier_add(type='FLUID')
        obj.modifiers[-1].fluid_type = 'DOMAIN'
        if self.style == 'FIRE' or self.style == 'BOTH':
            obj.modifiers[-1].domain_settings.use_noise = True

        # ensure correct cache file format for smoke
        if bpy.app.build_options.openvdb:
            obj.modifiers[-1].domain_settings.cache_data_format = 'OPENVDB'

        # Setup material

        # Cycles and Eevee
        bpy.ops.object.material_slot_add()

        mat = bpy.data.materials.new("Smoke Domain Material")
        obj.material_slots[0].material = mat

        # Make sure we use nodes
        mat.use_nodes = True

        # Set node variables and clear the default nodes
        tree = mat.node_tree
        nodes = tree.nodes
        links = tree.links

        nodes.clear()

        # Create shader nodes

        # Material output
        node_out = nodes.new(type='ShaderNodeOutputMaterial')
        node_out.location = grid_location(6, 1)

        # Add Principled Volume
        node_principled = nodes.new(type='ShaderNodeVolumePrincipled')
        node_principled.location = grid_location(4, 1)
        links.new(node_principled.outputs["Volume"],
                  node_out.inputs["Volume"])

        node_principled.inputs["Density"].default_value = 5.0

        if self.style in {'FIRE', 'BOTH'}:
            node_principled.inputs["Blackbody Intensity"].default_value = 1.0

        return {'FINISHED'}


class QuickLiquid(Operator):
    """Make selected objects liquid"""
    bl_idname = "object.quick_liquid"
    bl_label = "Quick Liquid"
    bl_options = {'REGISTER', 'UNDO'}

    show_flows: BoolProperty(
        name="Render Liquid Objects",
        description="Keep the liquid objects visible during rendering",
        default=False,
    )

    def execute(self, context):
        if not bpy.app.build_options.fluid:
            self.report({'ERROR'}, "Built without Fluid modifier")
            return {'CANCELLED'}

        fake_context = context.copy()
        mesh_objects = [obj for obj in context.selected_objects
                        if obj.type == 'MESH']
        min_co = Vector((100000.0, 100000.0, 100000.0))
        max_co = -min_co

        if not mesh_objects:
            self.report({'ERROR'}, "Select at least one mesh object")
            return {'CANCELLED'}

        # set shading type to wireframe so that liquid particles are visible
        for area in bpy.context.screen.areas:
            if area.type == 'VIEW_3D':
                for space in area.spaces:
                    if space.type == 'VIEW_3D':
                        space.shading.type = 'WIREFRAME'

        for obj in mesh_objects:
            fake_context["object"] = obj
            # make each selected object a liquid flow
            bpy.ops.object.modifier_add(fake_context, type='FLUID')
            obj.modifiers[-1].fluid_type = 'FLOW'

            # set type
            obj.modifiers[-1].flow_settings.flow_type = 'LIQUID'

            # set flow behavior
            obj.modifiers[-1].flow_settings.flow_behavior = 'GEOMETRY'

            # use some surface distance for smoke emission
            obj.modifiers[-1].flow_settings.surface_distance = 0.0

            if not self.show_flows:
                obj.display_type = 'WIRE'

            # store bounding box min/max for the domain object
            obj_bb_minmax(obj, min_co, max_co)

        # add the liquid domain object
        bpy.ops.mesh.primitive_cube_add(align='WORLD')
        obj = context.active_object
        obj.name = "Liquid Domain"

        # give the liquid some room above the flows
        obj.location = 0.5 * (max_co + min_co) + Vector((0.0, 0.0, -1.0))
        obj.scale = 0.5 * (max_co - min_co) + Vector((1.0, 1.0, 2.0))

        # setup liquid domain
        bpy.ops.object.modifier_add(type='FLUID')
        obj.modifiers[-1].fluid_type = 'DOMAIN'
        # set all domain borders to obstacle
        obj.modifiers[-1].domain_settings.use_collision_border_front = True
        obj.modifiers[-1].domain_settings.use_collision_border_back = True
        obj.modifiers[-1].domain_settings.use_collision_border_right = True
        obj.modifiers[-1].domain_settings.use_collision_border_left = True
        obj.modifiers[-1].domain_settings.use_collision_border_top = True
        obj.modifiers[-1].domain_settings.use_collision_border_bottom = True

        # ensure correct cache file formats for liquid
        if bpy.app.build_options.openvdb:
            obj.modifiers[-1].domain_settings.cache_data_format = 'OPENVDB'
        obj.modifiers[-1].domain_settings.cache_mesh_format = 'BOBJECT'

        # change domain type, will also allocate and show particle system for FLIP
        obj.modifiers[-1].domain_settings.domain_type = 'LIQUID'

        liquid_domain = obj.modifiers[-2]

        # set color mapping field to show phi grid for liquid
        liquid_domain.domain_settings.color_ramp_field = 'PHI'

        # perform a single slice of the domain
        liquid_domain.domain_settings.use_slice = True

        # set display thickness to a lower value for more detailed display of phi grids
        liquid_domain.domain_settings.display_thickness = 0.02

        # make the domain smooth so it renders nicely
        bpy.ops.object.shade_smooth()

        # create a ray-transparent material for the domain
        bpy.ops.object.material_slot_add()

        mat = bpy.data.materials.new("Liquid Domain Material")
        obj.material_slots[0].material = mat

        # Make sure we use nodes
        mat.use_nodes = True

        # Set node variables and clear the default nodes
        tree = mat.node_tree
        nodes = tree.nodes
        links = tree.links

        nodes.clear()

        # Create shader nodes

        # Material output
        node_out = nodes.new(type='ShaderNodeOutputMaterial')
        node_out.location = grid_location(6, 1)

        # Add Glass
        node_glass = nodes.new(type='ShaderNodeBsdfGlass')
        node_glass.location = grid_location(4, 1)
        links.new(node_glass.outputs["BSDF"], node_out.inputs["Surface"])
        node_glass.inputs["IOR"].default_value = 1.33

        # Add Absorption
        node_absorption = nodes.new(type='ShaderNodeVolumeAbsorption')
        node_absorption.location = grid_location(4, 2)
        links.new(node_absorption.outputs["Volume"], node_out.inputs["Volume"])
        node_absorption.inputs["Color"].default_value = (0.8, 0.9, 1.0, 1.0)

        return {'FINISHED'}


classes = (
    QuickExplode,
    QuickFur,
    QuickSmoke,
    QuickLiquid,
)
