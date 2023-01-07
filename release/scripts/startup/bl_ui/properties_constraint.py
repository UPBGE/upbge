# SPDX-License-Identifier: GPL-2.0-or-later
from bpy.types import Panel


class ObjectConstraintPanel:
    bl_context = "constraint"

    @classmethod
    def poll(cls, context):
        return (context.object)


class BoneConstraintPanel:
    bl_context = "bone_constraint"

    @classmethod
    def poll(cls, context):
        return (context.pose_bone)


class OBJECT_PT_constraints(ObjectConstraintPanel, Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_label = "Object Constraints"
    bl_options = {'HIDE_HEADER'}

    def draw(self, _context):
        layout = self.layout

        layout.operator_menu_enum("object.constraint_add", "type", text="Add Object Constraint")

        layout.template_constraints(use_bone_constraints=False)


class BONE_PT_constraints(BoneConstraintPanel, Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_label = "Bone Constraints"
    bl_options = {'HIDE_HEADER'}

    def draw(self, _context):
        layout = self.layout

        layout.operator_menu_enum("pose.constraint_add", "type", text="Add Bone Constraint")

        layout.template_constraints(use_bone_constraints=True)


# Parent class for constraint panels, with templates and drawing methods
# shared between the bone and object constraint panels
class ConstraintButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_label = ""
    bl_options = {'INSTANCED', 'HEADER_LAYOUT_EXPAND'}

    @staticmethod
    def draw_influence(layout, con):
        layout.separator()
        if con.type in {'IK', 'SPLINE_IK'}:
            # constraint.disable_keep_transform doesn't work well
            # for these constraints.
            layout.prop(con, "influence")
        else:
            row = layout.row(align=True)
            row.prop(con, "influence")
            row.operator("constraint.disable_keep_transform", text="", icon='CANCEL')

    @staticmethod
    def space_template(layout, con, target=True, owner=True, separator=True):
        if target or owner:
            if separator:
                layout.separator()
            if target:
                layout.prop(con, "target_space", text="Target")
            if owner:
                layout.prop(con, "owner_space", text="Owner")

            if con.target_space == 'CUSTOM' or con.owner_space == 'CUSTOM':
                col = layout.column()
                col.prop(con, "space_object")
                if con.space_object and con.space_object.type == 'ARMATURE':
                    col.prop_search(con, "space_subtarget", con.space_object.data, "bones", text="Bone")
                elif con.space_object and con.space_object.type in {'MESH', 'LATTICE'}:
                    col.prop_search(con, "space_subtarget", con.space_object, "vertex_groups", text="Vertex Group")

    @staticmethod
    def target_template(layout, con, subtargets=True):
        col = layout.column()
        col.prop(con, "target")  # XXX: limiting settings for only `curves` or some type of object.

        if con.target and subtargets:
            if con.target.type == 'ARMATURE':
                col.prop_search(con, "subtarget", con.target.data, "bones", text="Bone")

                if con.subtarget and hasattr(con, "head_tail"):
                    row = col.row(align=True)
                    row.use_property_decorate = False
                    sub = row.row(align=True)
                    sub.prop(con, "head_tail")
                    # XXX icon, and only when bone has segments?
                    sub.prop(con, "use_bbone_shape", text="", icon='IPO_BEZIER')
                    row.prop_decorator(con, "head_tail")
            elif con.target.type in {'MESH', 'LATTICE'}:
                col.prop_search(con, "subtarget", con.target, "vertex_groups", text="Vertex Group")

    def get_constraint(self, _context):
        con = self.custom_data
        self.layout.context_pointer_set("constraint", con)
        return con

    def draw_header(self, context):
        layout = self.layout
        con = self.get_constraint(context)

        layout.template_constraint_header(con)

    # Drawing methods for specific constraints. (Shared by object and bone constraint panels)

    def draw_childof(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row(heading="Location")
        row.use_property_decorate = False
        row.prop(con, "use_location_x", text="X", toggle=True)
        row.prop(con, "use_location_y", text="Y", toggle=True)
        row.prop(con, "use_location_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Rotation")
        row.use_property_decorate = False
        row.prop(con, "use_rotation_x", text="X", toggle=True)
        row.prop(con, "use_rotation_y", text="Y", toggle=True)
        row.prop(con, "use_rotation_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Scale")
        row.use_property_decorate = False
        row.prop(con, "use_scale_x", text="X", toggle=True)
        row.prop(con, "use_scale_y", text="Y", toggle=True)
        row.prop(con, "use_scale_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row()
        row.operator("constraint.childof_set_inverse")
        row.operator("constraint.childof_clear_inverse")

        self.draw_influence(layout, con)

    def draw_trackto(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "track_axis", expand=True)
        layout.prop(con, "up_axis", text="Up", expand=True)
        layout.prop(con, "use_target_z")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_follow_path(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        if con.use_fixed_location:
            layout.prop(con, "offset_factor", text="Offset Factor")
        else:
            layout.prop(con, "offset")

        layout.prop(con, "forward_axis", expand=True)
        layout.prop(con, "up_axis", expand=True)

        col = layout.column()
        col.prop(con, "use_fixed_location")
        col.prop(con, "use_curve_radius")
        col.prop(con, "use_curve_follow")

        layout.operator("constraint.followpath_path_animate", text="Animate Path", icon='ANIM_DATA')

        self.draw_influence(layout, con)

    def draw_rot_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        # Decorators and property split are really buggy with these properties
        row = layout.row(heading="Limit X", align=True)
        row.use_property_decorate = False
        row.prop(con, "use_limit_x", text="")
        sub = row.column(align=True)
        sub.active = con.use_limit_x
        sub.prop(con, "min_x", text="Min")
        sub.prop(con, "max_x", text="Max")
        row.label(icon='BLANK1')

        row = layout.row(heading="Y", align=True)
        row.use_property_decorate = False
        row.prop(con, "use_limit_y", text="")
        sub = row.column(align=True)
        sub.active = con.use_limit_y
        sub.prop(con, "min_y", text="Min")
        sub.prop(con, "max_y", text="Max")
        row.label(icon='BLANK1')

        row = layout.row(heading="Z", align=True)
        row.use_property_decorate = False
        row.prop(con, "use_limit_z", text="")
        sub = row.column(align=True)
        sub.active = con.use_limit_z
        sub.prop(con, "min_z", text="Min")
        sub.prop(con, "max_z", text="Max")
        row.label(icon='BLANK1')

        layout.prop(con, "euler_order", text="Order")
        layout.prop(con, "use_transform_limit")
        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_loc_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()

        row = col.row(heading="Minimum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_x
        subsub.prop(con, "min_x", text="")
        row.prop_decorator(con, "min_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_y
        subsub.prop(con, "min_y", text="")
        row.prop_decorator(con, "min_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_z
        subsub.prop(con, "min_z", text="")
        row.prop_decorator(con, "min_z")

        col.separator()

        row = col.row(heading="Maximum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_x
        subsub.prop(con, "max_x", text="")
        row.prop_decorator(con, "max_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_y
        subsub.prop(con, "max_y", text="")
        row.prop_decorator(con, "max_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_z
        subsub.prop(con, "max_z", text="")
        row.prop_decorator(con, "max_z")

        layout.prop(con, "use_transform_limit")
        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_size_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()

        row = col.row(heading="Minimum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_x
        subsub.prop(con, "min_x", text="")
        row.prop_decorator(con, "min_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_y
        subsub.prop(con, "min_y", text="")
        row.prop_decorator(con, "min_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_min_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_min_z
        subsub.prop(con, "min_z", text="")
        row.prop_decorator(con, "min_z")

        col.separator()

        row = col.row(heading="Maximum X", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_x", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_x
        subsub.prop(con, "max_x", text="")
        row.prop_decorator(con, "max_x")

        row = col.row(heading="Y", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_y", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_y
        subsub.prop(con, "max_y", text="")
        row.prop_decorator(con, "max_y")

        row = col.row(heading="Z", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_max_z", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_max_z
        subsub.prop(con, "max_z", text="")
        row.prop_decorator(con, "max_z")

        layout.prop(con, "use_transform_limit")
        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_rotate_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "euler_order", text="Order")

        row = layout.row(heading="Axis", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_x", text="X", toggle=True)
        sub.prop(con, "use_y", text="Y", toggle=True)
        sub.prop(con, "use_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Invert", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "invert_x", text="X", toggle=True)
        sub.prop(con, "invert_y", text="Y", toggle=True)
        sub.prop(con, "invert_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        layout.prop(con, "mix_mode", text="Mix")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_locate_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row(heading="Axis", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_x", text="X", toggle=True)
        sub.prop(con, "use_y", text="Y", toggle=True)
        sub.prop(con, "use_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        row = layout.row(heading="Invert", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "invert_x", text="X", toggle=True)
        sub.prop(con, "invert_y", text="Y", toggle=True)
        sub.prop(con, "invert_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        layout.prop(con, "use_offset")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_size_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row(heading="Axis", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_x", text="X", toggle=True)
        sub.prop(con, "use_y", text="Y", toggle=True)
        sub.prop(con, "use_z", text="Z", toggle=True)
        row.label(icon='BLANK1')

        col = layout.column()
        col.prop(con, "power")
        col.prop(con, "use_make_uniform")

        col.prop(con, "use_offset")
        row = col.row()
        row.active = con.use_offset
        row.prop(con, "use_add")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_same_volume(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.prop(con, "mode")

        row = layout.row(heading="Free Axis")
        row.prop(con, "free_axis", expand=True)

        layout.prop(con, "volume")

        self.space_template(layout, con, target=False, owner=True)

        self.draw_influence(layout, con)

    def draw_trans_like(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "remove_target_shear")
        layout.prop(con, "mix_mode", text="Mix")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_action(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        target_row = layout.row(align=True)
        target_row.active = not con.use_eval_time
        self.target_template(target_row, con)

        row = layout.row(align=True, heading="Evaluation Time")
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_eval_time", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_eval_time
        subsub.prop(con, "eval_time", text="")
        row.prop_decorator(con, "eval_time")

        layout.prop(con, "mix_mode", text="Mix")

        self.draw_influence(layout, con)

    def draw_lock_track(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "track_axis", expand=True)
        layout.prop(con, "lock_axis", expand=True)

        self.draw_influence(layout, con)

    def draw_dist_limit(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row()
        row.prop(con, "distance")
        row.operator("constraint.limitdistance_reset", text="", icon='X')

        layout.prop(con, "limit_mode", text="Clamp Region")

        layout.prop(con, "use_transform_limit")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_stretch_to(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        row = layout.row()
        row.prop(con, "rest_length")
        row.operator("constraint.stretchto_reset", text="", icon='X')

        layout.separator()

        col = layout.column()
        col.prop(con, "bulge", text="Volume Variation")

        row = col.row(heading="Volume Min", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_bulge_min", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_bulge_min
        subsub.prop(con, "bulge_min", text="")
        row.prop_decorator(con, "bulge_min")

        row = col.row(heading="Max", align=True)
        row.use_property_decorate = False
        sub = row.row(align=True)
        sub.prop(con, "use_bulge_max", text="")
        subsub = sub.row(align=True)
        subsub.active = con.use_bulge_max
        subsub.prop(con, "bulge_max", text="")
        row.prop_decorator(con, "bulge_max")

        row = col.row()
        row.active = con.use_bulge_min or con.use_bulge_max
        row.prop(con, "bulge_smooth", text="Smooth")

        layout.prop(con, "volume", expand=True)
        layout.prop(con, "keep_axis", text="Rotation", expand=True)

        self.draw_influence(layout, con)

    def draw_min_max(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "offset")
        layout.prop(con, "floor_location", expand=True, text="Min/Max")
        layout.prop(con, "use_rotation")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_rigid_body_joint(self, context):

        layout = self.layout
        con = self.get_constraint(context)
        self.target_template(layout, con, subtargets=False)

        layout.prop(con, "pivot_type")
        layout.prop(con, "child")

        row = layout.row()
        row.prop(con, "use_linked_collision", text="Linked Collision")
        row.prop(con, "show_pivot", text="Display Pivot")

        row = layout.row()
        row.prop(con, "use_breaking")
        row = row.row()
        row.active = con.use_breaking
        row.prop(con, "breaking_threshold")

        split = layout.split()

        col = split.column(align=True)
        col.label(text="Pivot:")
        col.prop(con, "pivot_x", text="X")
        col.prop(con, "pivot_y", text="Y")
        col.prop(con, "pivot_z", text="Z")

        col = split.column(align=True)
        col.label(text="Axis:")
        col.prop(con, "axis_x", text="X")
        col.prop(con, "axis_y", text="Y")
        col.prop(con, "axis_z", text="Z")

        if con.pivot_type == 'CONE_TWIST':
            layout.label(text="Limits:")
            split = layout.split()

            col = split.column()
            col.prop(con, "use_angular_limit_x", text="Angle X")
            sub = col.column()
            sub.active = con.use_angular_limit_x
            sub.prop(con, "limit_angle_max_x", text="")

            col = split.column()
            col.prop(con, "use_angular_limit_y", text="Angle Y")
            sub = col.column()
            sub.active = con.use_angular_limit_y
            sub.prop(con, "limit_angle_max_y", text="")

            col = split.column()
            col.prop(con, "use_angular_limit_z", text="Angle Z")
            sub = col.column()
            sub.active = con.use_angular_limit_z
            sub.prop(con, "limit_angle_max_z", text="")

        elif con.pivot_type == 'GENERIC_6_DOF':
            layout.label(text="Limits:")
            split = layout.split()

            col = split.column(align=True)
            col.prop(con, "use_limit_x", text="X")
            sub = col.column(align=True)
            sub.active = con.use_limit_x
            sub.prop(con, "limit_min_x", text="Min")
            sub.prop(con, "limit_max_x", text="Max")

            col = split.column(align=True)
            col.prop(con, "use_limit_y", text="Y")
            sub = col.column(align=True)
            sub.active = con.use_limit_y
            sub.prop(con, "limit_min_y", text="Min")
            sub.prop(con, "limit_max_y", text="Max")

            col = split.column(align=True)
            col.prop(con, "use_limit_z", text="Z")
            sub = col.column(align=True)
            sub.active = con.use_limit_z
            sub.prop(con, "limit_min_z", text="Min")
            sub.prop(con, "limit_max_z", text="Max")

            split = layout.split()

            col = split.column(align=True)
            col.prop(con, "use_angular_limit_x", text="Angle X")
            sub = col.column(align=True)
            sub.active = con.use_angular_limit_x
            sub.prop(con, "limit_angle_min_x", text="Min")
            sub.prop(con, "limit_angle_max_x", text="Max")

            col = split.column(align=True)
            col.prop(con, "use_angular_limit_y", text="Angle Y")
            sub = col.column(align=True)
            sub.active = con.use_angular_limit_y
            sub.prop(con, "limit_angle_min_y", text="Min")
            sub.prop(con, "limit_angle_max_y", text="Max")

            col = split.column(align=True)
            col.prop(con, "use_angular_limit_z", text="Angle Z")
            sub = col.column(align=True)
            sub.active = con.use_angular_limit_z
            sub.prop(con, "limit_angle_min_z", text="Min")
            sub.prop(con, "limit_angle_max_z", text="Max")

        elif con.pivot_type == 'HINGE':
            layout.label(text="Limits:")
            split = layout.split()

            row = split.row(align=True)
            col = row.column()
            col.prop(con, "use_angular_limit_x", text="Angle X")

            col = row.column()
            col.active = con.use_angular_limit_x
            col.prop(con, "limit_angle_min_x", text="Min")
            col = row.column()
            col.active = con.use_angular_limit_x
            col.prop(con, "limit_angle_max_x", text="Max")


    def draw_clamp_to(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "main_axis", expand=True)

        layout.prop(con, "use_cyclic")

        self.draw_influence(layout, con)

    def draw_transform(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "use_motion_extrapolate", text="Extrapolate")

        self.space_template(layout, con)

        self.draw_influence(layout, con)

    def draw_shrinkwrap(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con, False)

        layout.prop(con, "distance")
        layout.prop(con, "shrinkwrap_type", text="Mode")

        layout.separator()

        if con.shrinkwrap_type == 'PROJECT':
            layout.prop(con, "project_axis", expand=True, text="Project Axis")
            layout.prop(con, "project_axis_space", text="Space")
            layout.prop(con, "project_limit", text="Distance")
            layout.prop(con, "use_project_opposite")

            layout.separator()

            col = layout.column()
            row = col.row()
            row.prop(con, "cull_face", expand=True)
            row = col.row()
            row.active = con.use_project_opposite and con.cull_face != 'OFF'
            row.prop(con, "use_invert_cull")

            layout.separator()

        if con.shrinkwrap_type in {'PROJECT', 'NEAREST_SURFACE', 'TARGET_PROJECT'}:
            layout.prop(con, "wrap_mode", text="Snap Mode")
            row = layout.row(heading="Align to Normal", align=True)
            row.use_property_decorate = False
            sub = row.row(align=True)
            sub.prop(con, "use_track_normal", text="")
            subsub = sub.row(align=True)
            subsub.active = con.use_track_normal
            subsub.prop(con, "track_axis", text="")
            row.prop_decorator(con, "track_axis")

        self.draw_influence(layout, con)

    def draw_damp_track(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        layout.prop(con, "track_axis", expand=True)

        self.draw_influence(layout, con)

    def draw_spline_ik(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        self.draw_influence(layout, con)

    def draw_pivot(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        if con.target:
            layout.prop(con, "offset", text="Pivot Offset")
        else:
            layout.prop(con, "use_relative_location")
            if con.use_relative_location:
                layout.prop(con, "offset", text="Pivot Point")
            else:
                layout.prop(con, "offset", text="Pivot Point")

        col = layout.column()
        col.prop(con, "rotation_range", text="Rotation Range")

        self.draw_influence(layout, con)

    def draw_follow_track(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        clip = None
        if con.use_active_clip:
            clip = context.scene.active_clip
        else:
            clip = con.clip

        layout.prop(con, "use_active_clip")
        layout.prop(con, "use_3d_position")

        row = layout.row()
        row.active = not con.use_3d_position
        row.prop(con, "use_undistorted_position")

        if not con.use_active_clip:
            layout.prop(con, "clip")

        layout.prop(con, "frame_method")

        if clip:
            tracking = clip.tracking

            layout.prop_search(con, "object", tracking, "objects", icon='OBJECT_DATA')

            tracking_object = tracking.objects.get(con.object, tracking.objects[0])

            layout.prop_search(con, "track", tracking_object, "tracks", icon='ANIM_DATA')

        layout.prop(con, "camera")

        row = layout.row()
        row.active = not con.use_3d_position
        row.prop(con, "depth_object")

        layout.operator("clip.constraint_to_fcurve")

        self.draw_influence(layout, con)

    def draw_camera_solver(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.prop(con, "use_active_clip")

        if not con.use_active_clip:
            layout.prop(con, "clip")

        layout.operator("clip.constraint_to_fcurve")

        self.draw_influence(layout, con)

    def draw_object_solver(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        clip = None
        if con.use_active_clip:
            clip = context.scene.active_clip
        else:
            clip = con.clip

        layout.prop(con, "use_active_clip")

        if not con.use_active_clip:
            layout.prop(con, "clip")

        if clip:
            layout.prop_search(con, "object", clip.tracking, "objects", icon='OBJECT_DATA')

        layout.prop(con, "camera")

        row = layout.row()
        row.operator("constraint.objectsolver_set_inverse")
        row.operator("constraint.objectsolver_clear_inverse")

        layout.operator("clip.constraint_to_fcurve")

        self.draw_influence(layout, con)

    def draw_transform_cache(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.template_cache_file(con, "cache_file")

        cache_file = con.cache_file

        if cache_file is not None:
            layout.prop_search(con, "object_path", cache_file, "object_paths")

        self.draw_influence(layout, con)

    def draw_python_constraint(self, _context):
        layout = self.layout
        layout.label(text="Blender 2.6 doesn't support python constraints yet")

    def draw_armature(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()
        col.prop(con, "use_deform_preserve_volume")
        col.prop(con, "use_bone_envelopes")

        if context.pose_bone:
            col.prop(con, "use_current_location")

        layout.operator("constraint.add_target", text="Add Target Bone")

        layout.operator("constraint.normalize_target_weights")

        self.draw_influence(layout, con)

        if not con.targets:
            layout.label(text="No target bones added", icon='ERROR')

    def draw_kinematic(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        self.target_template(layout, con)

        if context.object.pose.ik_solver == 'ITASC':
            layout.prop(con, "ik_type")

            # This button gives itself too much padding, so put it in a column with the subtarget
            col = layout.column()
            col.prop(con, "pole_target")

            if con.pole_target and con.pole_target.type == 'ARMATURE':
                col.prop_search(con, "pole_subtarget", con.pole_target.data, "bones", text="Bone")

            col = layout.column()
            if con.pole_target:
                col.prop(con, "pole_angle")
            col.prop(con, "use_tail")
            col.prop(con, "use_stretch")
            col.prop(con, "chain_count")

            if con.ik_type == 'COPY_POSE':
                layout.prop(con, "reference_axis", expand=True)

                # Use separate rows and columns here to avoid an alignment issue with the lock buttons
                loc_col = layout.column()
                loc_col.prop(con, "use_location")

                row = loc_col.row()
                row.active = con.use_location
                row.prop(con, "weight", text="Weight", slider=True)

                row = loc_col.row(heading="Lock", align=True)
                row.use_property_decorate = False
                row.active = con.use_location
                sub = row.row(align=True)
                sub.prop(con, "lock_location_x", text="X", toggle=True)
                sub.prop(con, "lock_location_y", text="Y", toggle=True)
                sub.prop(con, "lock_location_z", text="Z", toggle=True)
                row.label(icon='BLANK1')

                rot_col = layout.column()
                rot_col.prop(con, "use_rotation")

                row = rot_col.row()
                row.active = con.use_rotation
                row.prop(con, "orient_weight", text="Weight", slider=True)

                row = rot_col.row(heading="Lock", align=True)
                row.use_property_decorate = False
                row.active = con.use_rotation
                sub = row.row(align=True)
                sub.prop(con, "lock_rotation_x", text="X", toggle=True)
                sub.prop(con, "lock_rotation_y", text="Y", toggle=True)
                sub.prop(con, "lock_rotation_z", text="Z", toggle=True)
                row.label(icon='BLANK1')

            elif con.ik_type == 'DISTANCE':
                layout.prop(con, "limit_mode")

                col = layout.column()
                col.prop(con, "weight", text="Weight", slider=True)
                col.prop(con, "distance", text="Distance", slider=True)
        else:
            # Standard IK constraint
            col = layout.column()
            col.prop(con, "pole_target")

            if con.pole_target and con.pole_target.type == 'ARMATURE':
                col.prop_search(con, "pole_subtarget", con.pole_target.data, "bones", text="Bone")

            col = layout.column()
            if con.pole_target:
                col.prop(con, "pole_angle")
            col.prop(con, "iterations")
            col.prop(con, "chain_count")
            col.prop(con, "use_tail")
            col.prop(con, "use_stretch")

            col = layout.column()
            row = col.row(align=True, heading="Weight Position")
            row.prop(con, "use_location", text="")
            sub = row.row(align=True)
            sub.active = con.use_location
            sub.prop(con, "weight", text="", slider=True)

            row = col.row(align=True, heading="Rotation")
            row.prop(con, "use_rotation", text="")
            sub = row.row(align=True)
            sub.active = con.use_rotation
            sub.prop(con, "orient_weight", text="", slider=True)

        self.draw_influence(layout, con)


# Parent class for constraint subpanels
class ConstraintButtonsSubPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_label = ""

    def get_constraint(self, _context):
        con = self.custom_data
        self.layout.context_pointer_set("constraint", con)
        return con

    def draw_transform_from(self, context):
        layout = self.layout
        con = self.get_constraint(context)

        layout.prop(con, "map_from", expand=True)

        layout.use_property_split = True
        layout.use_property_decorate = True

        from_axes = [con.map_to_x_from, con.map_to_y_from, con.map_to_z_from]

        if con.map_from == 'ROTATION':
            layout.prop(con, "from_rotation_mode", text="Mode")

        ext = "" if con.map_from == 'LOCATION' else "_rot" if con.map_from == 'ROTATION' else "_scale"

        col = layout.column(align=True)
        col.active = "X" in from_axes
        col.prop(con, "from_min_x" + ext, text="X Min")
        col.prop(con, "from_max_x" + ext, text="Max")

        col = layout.column(align=True)
        col.active = "Y" in from_axes
        col.prop(con, "from_min_y" + ext, text="Y Min")
        col.prop(con, "from_max_y" + ext, text="Max")

        col = layout.column(align=True)
        col.active = "Z" in from_axes
        col.prop(con, "from_min_z" + ext, text="Z Min")
        col.prop(con, "from_max_z" + ext, text="Max")

    def draw_transform_to(self, context):
        layout = self.layout
        con = self.get_constraint(context)

        layout.prop(con, "map_to", expand=True)

        layout.use_property_split = True
        layout.use_property_decorate = True

        if con.map_to == 'ROTATION':
            layout.prop(con, "to_euler_order", text="Order")

        ext = "" if con.map_to == 'LOCATION' else "_rot" if con.map_to == 'ROTATION' else "_scale"

        col = layout.column(align=True)
        col.prop(con, "map_to_x_from", expand=False, text="X Source Axis")
        col.prop(con, "to_min_x" + ext, text="Min")
        col.prop(con, "to_max_x" + ext, text="Max")

        col = layout.column(align=True)
        col.prop(con, "map_to_y_from", expand=False, text="Y Source Axis")
        col.prop(con, "to_min_y" + ext, text="Min")
        col.prop(con, "to_max_y" + ext, text="Max")

        col = layout.column(align=True)
        col.prop(con, "map_to_z_from", expand=False, text="Z Source Axis")
        col.prop(con, "to_min_z" + ext, text="Min")
        col.prop(con, "to_max_z" + ext, text="Max")

        layout.prop(con, "mix_mode" + ext, text="Mix")

    def draw_armature_bones(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        for i, tgt in enumerate(con.targets):
            has_target = tgt.target is not None

            box = layout.box()
            header = box.row()
            header.use_property_split = False

            split = header.split(factor=0.45, align=True)
            split.prop(tgt, "target", text="")

            row = split.row(align=True)
            row.active = has_target
            if has_target:
                row.prop_search(tgt, "subtarget", tgt.target.data, "bones", text="")
            else:
                row.prop(tgt, "subtarget", text="", icon='BONE_DATA')

            header.operator("constraint.remove_target", text="", icon='X').index = i

            row = box.row()
            row.active = has_target and tgt.subtarget != ""
            row.prop(tgt, "weight", slider=True, text="Weight")

    def draw_spline_ik_fitting(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()
        col.prop(con, "chain_count")
        col.prop(con, "use_even_divisions")
        col.prop(con, "use_chain_offset")

    def draw_spline_ik_chain_scaling(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.prop(con, "use_curve_radius")

        layout.prop(con, "y_scale_mode")
        layout.prop(con, "xz_scale_mode")

        if con.xz_scale_mode in {'INVERSE_PRESERVE', 'VOLUME_PRESERVE'}:
            layout.prop(con, "use_original_scale")

        if con.xz_scale_mode == 'VOLUME_PRESERVE':
            col = layout.column()
            col.prop(con, "bulge", text="Volume Variation")

            row = col.row(heading="Volume Min")
            row.prop(con, "use_bulge_min", text="")
            sub = row.row()
            sub.active = con.use_bulge_min
            sub.prop(con, "bulge_min", text="")

            row = col.row(heading="Max")
            row.prop(con, "use_bulge_max", text="")
            sub = row.row()
            sub.active = con.use_bulge_max
            sub.prop(con, "bulge_max", text="")

            row = layout.row()
            row.active = con.use_bulge_min or con.use_bulge_max
            row.prop(con, "bulge_smooth", text="Smooth")

    def draw_action_target(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        col = layout.column()
        col.active = not con.use_eval_time
        col.prop(con, "transform_channel", text="Channel")
        ConstraintButtonsPanel.space_template(col, con, target=True, owner=False, separator=False)

        sub = col.column(align=True)
        sub.prop(con, "min", text="Range Min")
        sub.prop(con, "max", text="Max")

    def draw_action_action(self, context):
        layout = self.layout
        con = self.get_constraint(context)
        layout.use_property_split = True
        layout.use_property_decorate = True

        layout.prop(con, "action")
        layout.prop(con, "use_bone_object_action")

        col = layout.column(align=True)
        col.prop(con, "frame_start", text="Frame Start")
        col.prop(con, "frame_end", text="End")

    def draw_transform_cache_velocity(self, context):
        self.draw_transform_cache_subpanel(
            context, self.layout.template_cache_file_velocity
        )

    def draw_transform_cache_procedural(self, context):
        self.draw_transform_cache_subpanel(
            context, self.layout.template_cache_file_procedural
        )

    def draw_transform_cache_time(self, context):
        self.draw_transform_cache_subpanel(
            context, self.layout.template_cache_file_time_settings
        )

    def draw_transform_cache_layers(self, context):
        self.draw_transform_cache_subpanel(
            context, self.layout.template_cache_file_layers
        )

    def draw_transform_cache_subpanel(self, context, template_func):
        con = self.get_constraint(context)
        if con.cache_file is None:
            return

        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = True
        template_func(con, "cache_file")

# Child Of Constraint


class OBJECT_PT_bChildOfConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_childof(context)


class BONE_PT_bChildOfConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_childof(context)

# Track To Constraint


class OBJECT_PT_bTrackToConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_trackto(context)


class BONE_PT_bTrackToConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_trackto(context)

# Follow Path Constraint


class OBJECT_PT_bFollowPathConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_follow_path(context)


class BONE_PT_bFollowPathConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_follow_path(context)


# Rotation Limit Constraint

class OBJECT_PT_bRotLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_rot_limit(context)


class BONE_PT_bRotLimitConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_rot_limit(context)


# Location Limit Constraint

class OBJECT_PT_bLocLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_loc_limit(context)


class BONE_PT_bLocLimitConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_loc_limit(context)


# Size Limit Constraint

class OBJECT_PT_bSizeLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_size_limit(context)


class BONE_PT_bSizeLimitConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_size_limit(context)


# Rotate Like Constraint

class OBJECT_PT_bRotateLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_rotate_like(context)


class BONE_PT_bRotateLikeConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_rotate_like(context)


# Locate Like Constraint

class OBJECT_PT_bLocateLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_locate_like(context)


class BONE_PT_bLocateLikeConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_locate_like(context)


# Size Like Constraint

class OBJECT_PT_bSizeLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_size_like(context)


class BONE_PT_bSizeLikeConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_size_like(context)


# Same Volume Constraint

class OBJECT_PT_bSameVolumeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_same_volume(context)


class BONE_PT_bSameVolumeConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_same_volume(context)


# Trans Like Constraint

class OBJECT_PT_bTransLikeConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_trans_like(context)


class BONE_PT_bTransLikeConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_trans_like(context)


# Action Constraint

class OBJECT_PT_bActionConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_action(context)


class BONE_PT_bActionConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_action(context)


class OBJECT_PT_bActionConstraint_target(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bActionConstraint"
    bl_label = "Target"

    def draw(self, context):
        self.draw_action_target(context)


class BONE_PT_bActionConstraint_target(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bActionConstraint"
    bl_label = "Target"

    def draw(self, context):
        self.draw_action_target(context)


class OBJECT_PT_bActionConstraint_action(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bActionConstraint"
    bl_label = "Action"

    def draw(self, context):
        self.draw_action_action(context)


class BONE_PT_bActionConstraint_action(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bActionConstraint"
    bl_label = "Action"

    def draw(self, context):
        self.draw_action_action(context)


# Lock Track Constraint

class OBJECT_PT_bLockTrackConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_lock_track(context)


class BONE_PT_bLockTrackConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_lock_track(context)


# Distance Limit Constraint

class OBJECT_PT_bDistLimitConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_dist_limit(context)


class BONE_PT_bDistLimitConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_dist_limit(context)


# Stretch To Constraint

class OBJECT_PT_bStretchToConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_stretch_to(context)


class BONE_PT_bStretchToConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_stretch_to(context)


# Min Max Constraint

class OBJECT_PT_bMinMaxConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_min_max(context)


class BONE_PT_bMinMaxConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_min_max(context)


# Clamp To Constraint

class OBJECT_PT_bClampToConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_clamp_to(context)


class BONE_PT_bClampToConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_clamp_to(context)

# Rigid Body Joint Constraint

class OBJECT_PT_bRigidBodyJointConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_rigid_body_joint(context)


# Transform Constraint

class OBJECT_PT_bTransformConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_transform(context)


class BONE_PT_bTransformConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_transform(context)


class OBJECT_PT_bTransformConstraint_source(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformConstraint"
    bl_label = "Map From"

    def draw(self, context):
        self.draw_transform_from(context)


class BONE_PT_bTransformConstraint_from(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bTransformConstraint"
    bl_label = "Map From"

    def draw(self, context):
        self.draw_transform_from(context)


class OBJECT_PT_bTransformConstraint_destination(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformConstraint"
    bl_label = "Map To"

    def draw(self, context):
        self.draw_transform_to(context)


class BONE_PT_bTransformConstraint_to(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bTransformConstraint"
    bl_label = "Map To"

    def draw(self, context):
        self.draw_transform_to(context)


# Shrinkwrap Constraint

class OBJECT_PT_bShrinkwrapConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_shrinkwrap(context)


class BONE_PT_bShrinkwrapConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_shrinkwrap(context)


# Damp Track Constraint

class OBJECT_PT_bDampTrackConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_damp_track(context)


class BONE_PT_bDampTrackConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_damp_track(context)


# Spline IK Constraint

class BONE_PT_bSplineIKConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_spline_ik(context)


class BONE_PT_bSplineIKConstraint_fitting(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bSplineIKConstraint"
    bl_label = "Fitting"

    def draw(self, context):
        self.draw_spline_ik_fitting(context)


class BONE_PT_bSplineIKConstraint_chain_scaling(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bSplineIKConstraint"
    bl_label = "Chain Scaling"

    def draw(self, context):
        self.draw_spline_ik_chain_scaling(context)


# Pivot Constraint

class OBJECT_PT_bPivotConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_pivot(context)


class BONE_PT_bPivotConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_pivot(context)


# Follow Track Constraint

class OBJECT_PT_bFollowTrackConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_follow_track(context)


class BONE_PT_bFollowTrackConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_follow_track(context)


# Camera Solver Constraint

class OBJECT_PT_bCameraSolverConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_camera_solver(context)


class BONE_PT_bCameraSolverConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_camera_solver(context)


# Object Solver Constraint

class OBJECT_PT_bObjectSolverConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_object_solver(context)


class BONE_PT_bObjectSolverConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_object_solver(context)


# Transform Cache Constraint

class OBJECT_PT_bTransformCacheConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_transform_cache(context)


class BONE_PT_bTransformCacheConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_transform_cache(context)


class OBJECT_PT_bTransformCacheConstraint_velocity(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformCacheConstraint"
    bl_label = "Velocity"

    def draw(self, context):
        self.draw_transform_cache_velocity(context)


class BONE_PT_bTransformCacheConstraint_velocity(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bTransformCacheConstraint"
    bl_label = "Velocity"

    def draw(self, context):
        self.draw_transform_cache_velocity(context)


class OBJECT_PT_bTransformCacheConstraint_layers(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformCacheConstraint"
    bl_label = "Override Layers"

    def draw(self, context):
        self.draw_transform_cache_layers(context)


class BONE_PT_bTransformCacheConstraint_layers(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bTransformCacheConstraint"
    bl_label = "Override Layers"

    def draw(self, context):
        self.draw_transform_cache_layers(context)


class OBJECT_PT_bTransformCacheConstraint_procedural(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformCacheConstraint"
    bl_label = "Render Procedural"

    def draw(self, context):
        self.draw_transform_cache_procedural(context)


class BONE_PT_bTransformCacheConstraint_procedural(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bTransformCacheConstraint"
    bl_label = "Render Procedural"

    def draw(self, context):
        self.draw_transform_cache_procedural(context)


class OBJECT_PT_bTransformCacheConstraint_time(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bTransformCacheConstraint"
    bl_label = "Time"

    def draw(self, context):
        self.draw_transform_cache_time(context)


class BONE_PT_bTransformCacheConstraint_time(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bTransformCacheConstraint"
    bl_label = "Time"

    def draw(self, context):
        self.draw_transform_cache_time(context)


# Python Constraint

class OBJECT_PT_bPythonConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_python_constraint(context)


class BONE_PT_bPythonConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_python_constraint(context)


# Armature Constraint

class OBJECT_PT_bArmatureConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_armature(context)


class BONE_PT_bArmatureConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_armature(context)


class OBJECT_PT_bArmatureConstraint_bones(ObjectConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "OBJECT_PT_bArmatureConstraint"
    bl_label = "Bones"

    def draw(self, context):
        self.draw_armature_bones(context)


class BONE_PT_bArmatureConstraint_bones(BoneConstraintPanel, ConstraintButtonsSubPanel, Panel):
    bl_parent_id = "BONE_PT_bArmatureConstraint"
    bl_label = "Bones"

    def draw(self, context):
        self.draw_armature_bones(context)


# Inverse Kinematic Constraint

class OBJECT_PT_bKinematicConstraint(ObjectConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_kinematic(context)


class BONE_PT_bKinematicConstraint(BoneConstraintPanel, ConstraintButtonsPanel, Panel):
    def draw(self, context):
        self.draw_kinematic(context)


classes = (
    # Object Panels
    OBJECT_PT_constraints,
    BONE_PT_constraints,
    OBJECT_PT_bChildOfConstraint,
    OBJECT_PT_bTrackToConstraint,
    OBJECT_PT_bKinematicConstraint,
    OBJECT_PT_bFollowPathConstraint,
    OBJECT_PT_bRotLimitConstraint,
    OBJECT_PT_bLocLimitConstraint,
    OBJECT_PT_bSizeLimitConstraint,
    OBJECT_PT_bRotateLikeConstraint,
    OBJECT_PT_bLocateLikeConstraint,
    OBJECT_PT_bSizeLikeConstraint,
    OBJECT_PT_bSameVolumeConstraint,
    OBJECT_PT_bTransLikeConstraint,
    OBJECT_PT_bActionConstraint,
    OBJECT_PT_bActionConstraint_target,
    OBJECT_PT_bActionConstraint_action,
    OBJECT_PT_bLockTrackConstraint,
    OBJECT_PT_bDistLimitConstraint,
    OBJECT_PT_bStretchToConstraint,
    OBJECT_PT_bMinMaxConstraint,
    OBJECT_PT_bClampToConstraint,
    OBJECT_PT_bRigidBodyJointConstraint,
    OBJECT_PT_bTransformConstraint,
    OBJECT_PT_bTransformConstraint_source,
    OBJECT_PT_bTransformConstraint_destination,
    OBJECT_PT_bShrinkwrapConstraint,
    OBJECT_PT_bDampTrackConstraint,
    OBJECT_PT_bPivotConstraint,
    OBJECT_PT_bFollowTrackConstraint,
    OBJECT_PT_bCameraSolverConstraint,
    OBJECT_PT_bObjectSolverConstraint,
    OBJECT_PT_bTransformCacheConstraint,
    OBJECT_PT_bTransformCacheConstraint_time,
    OBJECT_PT_bTransformCacheConstraint_procedural,
    OBJECT_PT_bTransformCacheConstraint_velocity,
    OBJECT_PT_bTransformCacheConstraint_layers,
    OBJECT_PT_bPythonConstraint,
    OBJECT_PT_bArmatureConstraint,
    OBJECT_PT_bArmatureConstraint_bones,
    # Bone panels
    BONE_PT_bChildOfConstraint,
    BONE_PT_bTrackToConstraint,
    BONE_PT_bKinematicConstraint,
    BONE_PT_bFollowPathConstraint,
    BONE_PT_bRotLimitConstraint,
    BONE_PT_bLocLimitConstraint,
    BONE_PT_bSizeLimitConstraint,
    BONE_PT_bRotateLikeConstraint,
    BONE_PT_bLocateLikeConstraint,
    BONE_PT_bSizeLikeConstraint,
    BONE_PT_bSameVolumeConstraint,
    BONE_PT_bTransLikeConstraint,
    BONE_PT_bActionConstraint,
    BONE_PT_bActionConstraint_target,
    BONE_PT_bActionConstraint_action,
    BONE_PT_bLockTrackConstraint,
    BONE_PT_bDistLimitConstraint,
    BONE_PT_bStretchToConstraint,
    BONE_PT_bMinMaxConstraint,
    BONE_PT_bClampToConstraint,
    BONE_PT_bTransformConstraint,
    BONE_PT_bTransformConstraint_from,
    BONE_PT_bTransformConstraint_to,
    BONE_PT_bShrinkwrapConstraint,
    BONE_PT_bDampTrackConstraint,
    BONE_PT_bSplineIKConstraint,
    BONE_PT_bSplineIKConstraint_fitting,
    BONE_PT_bSplineIKConstraint_chain_scaling,
    BONE_PT_bPivotConstraint,
    BONE_PT_bFollowTrackConstraint,
    BONE_PT_bCameraSolverConstraint,
    BONE_PT_bObjectSolverConstraint,
    BONE_PT_bTransformCacheConstraint,
    BONE_PT_bTransformCacheConstraint_time,
    BONE_PT_bTransformCacheConstraint_procedural,
    BONE_PT_bTransformCacheConstraint_velocity,
    BONE_PT_bTransformCacheConstraint_layers,
    BONE_PT_bPythonConstraint,
    BONE_PT_bArmatureConstraint,
    BONE_PT_bArmatureConstraint_bones,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
