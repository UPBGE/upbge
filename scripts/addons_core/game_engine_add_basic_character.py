# This is free software under the terms of the GNU General Public License
# you may redistribute it, and/or modify it.
#
# This code is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License (http://www.gnu.org/licenses/) for more details.
#
# ***** END GPL LICENSE BLOCK *****

# #########################################################################

bl_info = {
    "name": "Add character",
    "description": "Create basic bge character and fly camera",
    "author": "Moaaa",
    "version": (0, 0, 4),
    "blender": (2, 77, 0),
    "location": "View3D > TOOLS",
    "warning": "WIP - Frequent changes for known issues and enhancements",
    "support": "TESTING",
    "wiki_url": "https://github.com/UPBGE/blender-addons/wiki/Basic-Character-addon",
    "tracker_url": "",
    "category": "Game Engine"
}

import bpy
import os
from bpy.types import Operator, Panel

bpy.types.Scene.character_size = bpy.props.FloatProperty(name="character_size", default=2.0, min=1.0, max=10.0)
bpy.types.Scene.key_sensitive = bpy.props.FloatProperty(name="key sensitive", default=0.2, min=0.1, max=5.0)
bpy.types.Scene.mouse_sensitive = bpy.props.FloatProperty(name="mouse sensitive", default=0.5, min=0.1, max=5.0)
bpy.types.Scene.character_name = bpy.props.StringProperty(name="character_name", default="character")
bpy.types.Scene.character_jump = bpy.props.BoolProperty(name="character_jump")

key_mode = [("0", "Arrow Keys", "Arrow Keys"), ("1", "ZSDQ", "ZSDQ"), ("2", "WSDA", "WSDA"), ]

bpy.types.Scene.character_keys = bpy.props.EnumProperty(items=key_mode, name="character_keys")

class simple_character(Operator):
    bl_label = 'Add character'
    bl_idname = 'character.gen'
    bl_description = 'Generate a simple character'
    bl_context = 'objectmode'
    

    
    def execute(self, context):
        


        bpy.context.scene.render.engine = 'BLENDER_GAME'
        bpy.context.scene.objects.active = None

        bpy.ops.mesh.primitive_cone_add(vertices=16, radius1=bpy.context.scene.character_size, radius2=0.0, depth=bpy.context.scene.character_size, end_fill_type='TRIFAN', view_align=False,
        enter_editmode=False, location=bpy.context.scene.cursor_location, rotation=(0.0, 0.0, 0.0),
        layers=(True, False, False, False, False, False, False, False, False, False, False, False, False, False, False, False, False, False, False, False))

        # configure character
        obj = bpy.context.selected_objects[0]
        obj.name = bpy.context.scene.character_name
        obj.game.physics_type = 'CHARACTER'
        obj.game.use_actor = True
        obj.game.use_collision_bounds = True
        obj.game.collision_bounds_type = 'CONE'
        obj.hide_render = True
        
        sensors = obj.game.sensors
        controllers = obj.game.controllers
        actuators = obj.game.actuators

        bpy.ops.logic.sensor_add(type="MOUSE", object=obj.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", object=obj.name)
        bpy.ops.logic.actuator_add(type='MOUSE', name="BodyTurn", object=obj.name)




        sensor = sensors[-1]
        sensor.mouse_event = 'MOVEMENT'
        sensor.use_pulse_true_level = True
        controller = controllers[-1]
        actuator = actuators[-1]
        actuator.mode = 'LOOK'
        actuator.use_axis_y = False
        actuator.sensitivity_x = bpy.context.scene.mouse_sensitive
        
        sensor.link(controller)
        actuator.link(controller)
        
        keys_list = [("UP_ARROW", "DOWN_ARROW", "RIGHT_ARROW", "LEFT_ARROW", "RIGHT_CTRL"), ("Z", "S", "D", "Q", "SPACE"), ("W", "S", "D", "A", "SPACE")]

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="Forward", object=obj.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="Forward", object=obj.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="Forward", object=obj.name)
        
        sensors["Forward"].key = keys_list[int(bpy.context.scene.character_keys)][0]
        actuators["Forward"].mode = "OBJECT_CHARACTER"
        actuators["Forward"].offset_location[1] = bpy.context.scene.key_sensitive
        
        sensors["Forward"].link(controllers["Forward"])
        actuators["Forward"].link(controllers["Forward"])

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="back", object=obj.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="back", object=obj.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="back", object=obj.name)
        
        sensors["back"].key = keys_list[int(bpy.context.scene.character_keys)][1]
        actuators["back"].mode = "OBJECT_CHARACTER"
        actuators["back"].offset_location[1] = -bpy.context.scene.key_sensitive
        
        sensors["back"].link(controllers["back"])
        actuators["back"].link(controllers["back"])

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="right", object=obj.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="right", object=obj.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="right", object=obj.name)
        
        sensors["right"].key = keys_list[int(bpy.context.scene.character_keys)][2]
        actuators["right"].mode = "OBJECT_CHARACTER"
        actuators["right"].offset_location[0] = bpy.context.scene.key_sensitive
        
        sensors["right"].link(controllers["right"])
        actuators["right"].link(controllers["right"])

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="left", object=obj.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="left", object=obj.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="left", object=obj.name)
        
        sensors["left"].key = keys_list[int(bpy.context.scene.character_keys)][3]
        actuators["left"].mode = "OBJECT_CHARACTER"
        actuators["left"].offset_location[0] = -bpy.context.scene.key_sensitive
        
        sensors["left"].link(controllers["left"])
        actuators["left"].link(controllers["left"])
        
        if bpy.context.scene.character_jump == True:
            bpy.ops.logic.sensor_add(type="KEYBOARD", name="jump", object=obj.name)
            bpy.ops.logic.controller_add(type="LOGIC_AND", name="jump", object=obj.name)
            bpy.ops.logic.actuator_add(type='MOTION', name="jump", object=obj.name)
            
            sensors["jump"].key = keys_list[int(bpy.context.scene.character_keys)][4]
            actuators["jump"].mode = "OBJECT_CHARACTER"
            actuators["jump"].use_character_jump = True

            sensors["jump"].link(controllers["jump"])
            actuators["jump"].link(controllers["jump"])


        for sen in sensors:
            sen.show_expanded = False

        for act in actuators:
            act.show_expanded = False

        #create camera and configure
        cam = bpy.data.cameras.new("CameraM")
        cam_ob = bpy.data.objects.new("CameraM", cam)
        bpy.context.scene.objects.link(cam_ob)
        
        cam_ob.location = (bpy.context.scene.cursor_location[0],
        bpy.context.scene.cursor_location[1], bpy.context.scene.cursor_location[2]+(bpy.context.scene.character_size/2.2))
        cam_ob.rotation_euler = (1.5708, 0, 0)
        cam_ob.name = "cam" + bpy.context.scene.character_name

        sensors = cam_ob.game.sensors
        controllers = cam_ob.game.controllers
        actuators = cam_ob.game.actuators

        bpy.ops.logic.sensor_add(type="MOUSE", object=cam_ob.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", object=cam_ob.name)
        bpy.ops.logic.actuator_add(type='MOUSE', name="HeadTurn", object=cam_ob.name)


        sensor = sensors[-1]
        sensor.mouse_event = 'MOVEMENT'
        sensor.use_pulse_true_level = True
        controller = controllers[-1]
        actuator = actuators[-1]
        actuator.mode = 'LOOK'
        actuator.use_axis_x = False
        actuator.sensitivity_y = bpy.context.scene.mouse_sensitive
        
        sensor.link(controller)
        actuator.link(controller)
        
        bpy.context.scene.objects.active = None

        obj.location = (obj.location[0], obj.location[1], obj.location[2]+(bpy.context.scene.character_size/2))
        cam_ob.location = (cam_ob.location[0], cam_ob.location[1], cam_ob.location[2]+(bpy.context.scene.character_size/2))
		
        obj.select = True
        cam_ob.select = True
        bpy.context.scene.objects.active = obj
        bpy.ops.object.parent_set(type='OBJECT', keep_transform=False)
        
        bpy.context.scene.objects.active = cam_ob
        bpy.ops.view3d.object_as_camera()

        return {'FINISHED'}

class fly_camera(Operator):
    bl_label = 'Add Fly Camera'
    bl_idname = 'fly_camera.gen'
    bl_description = 'Generate a simple fly camera'
    bl_context = 'objectmode'

    def execute(self, context):
        
        bpy.context.scene.render.engine = 'BLENDER_GAME'
        bpy.context.scene.objects.active = None
        
        #create camera and configure
        cam = bpy.data.cameras.new("CameraM")
        cam_object = bpy.data.objects.new("CameraM", cam)
        bpy.context.scene.objects.link(cam_object)

        cam_object.location = (bpy.context.scene.cursor_location)

        cam_object.rotation_euler = (1.5708, 0, 0)
        cam_object.name = bpy.context.scene.character_name
        bpy.context.scene.objects.active = cam_object

        sensors = cam_object.game.sensors
        controllers = cam_object.game.controllers
        actuators = cam_object.game.actuators

        bpy.ops.logic.sensor_add(type="MOUSE", name="MouseMove", object=cam_object.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="MouseMove", object=cam_object.name)
        bpy.ops.logic.actuator_add(type='MOUSE', name="MouseMove", object=cam_object.name)
        
        sensors["MouseMove"].use_pulse_true_level = True
        sensors["MouseMove"].mouse_event = 'MOVEMENT'
        actuators["MouseMove"].mode = 'LOOK'
        actuators["MouseMove"].sensitivity_x = bpy.context.scene.mouse_sensitive
        actuators["MouseMove"].sensitivity_y = bpy.context.scene.mouse_sensitive


        sensors["MouseMove"].link(controllers["MouseMove"])
        actuators["MouseMove"].link(controllers["MouseMove"])

        keys_list = [("UP_ARROW", "DOWN_ARROW", "RIGHT_ARROW", "LEFT_ARROW", "RIGHT_CTRL"), ("Z", "S", "D", "Q", "SPACE"), ("W", "S", "D", "A", "SPACE")]

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="Forward", object=cam_object.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="Forward", object=cam_object.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="Forward", object=cam_object.name)
        
        sensors["Forward"].key = keys_list[int(bpy.context.scene.character_keys)][0]
        actuators["Forward"].mode = "OBJECT_CHARACTER"
        actuators["Forward"].offset_location[1] = bpy.context.scene.key_sensitive
        
        sensors["Forward"].link(controllers["Forward"])
        actuators["Forward"].link(controllers["Forward"])

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="back", object=cam_object.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="back", object=cam_object.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="back", object=cam_object.name)
        
        sensors["back"].key = keys_list[int(bpy.context.scene.character_keys)][1]
        actuators["back"].mode = "OBJECT_CHARACTER"
        actuators["back"].offset_location[1] = -bpy.context.scene.key_sensitive
        
        sensors["back"].link(controllers["back"])
        actuators["back"].link(controllers["back"])

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="right", object=cam_object.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="right", object=cam_object.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="right", object=cam_object.name)
        
        sensors["right"].key = keys_list[int(bpy.context.scene.character_keys)][2]
        actuators["right"].mode = "OBJECT_CHARACTER"
        actuators["right"].offset_location[0] = bpy.context.scene.key_sensitive
        
        sensors["right"].link(controllers["right"])
        actuators["right"].link(controllers["right"])

        bpy.ops.logic.sensor_add(type="KEYBOARD", name="left", object=cam_object.name)
        bpy.ops.logic.controller_add(type="LOGIC_AND", name="left", object=cam_object.name)
        bpy.ops.logic.actuator_add(type='MOTION', name="left", object=cam_object.name)
        
        sensors["left"].key = keys_list[int(bpy.context.scene.character_keys)][3]
        actuators["left"].mode = "OBJECT_CHARACTER"
        actuators["left"].offset_location[0] = -bpy.context.scene.key_sensitive
        
        sensors["left"].link(controllers["left"])
        actuators["left"].link(controllers["left"])

        for sen in sensors:
            sen.show_expanded = False

        for act in actuators:
            act.show_expanded = False

        bpy.context.scene.objects.active = cam_object
        bpy.ops.view3d.object_as_camera()

        return {'FINISHED'}

        
class addcharacter(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'TOOLS'
    bl_label = 'Simple Character'
    bl_context = 'objectmode'
    bl_category = 'Add Character'

    def draw(self, context):
        layout = self.layout
        row = layout.row()
        row.prop(context.scene, "character_name", text="Name")
        row = layout.row()
        row.prop(context.scene, "character_size", text="Size")
        row = layout.row()
        row.prop(context.scene, "character_keys", text="Keys")
        row = layout.row()
        row.prop(context.scene, "key_sensitive", text="Key sensitive")
        row = layout.row()
        row.prop(context.scene, "mouse_sensitive", text="Mouse sensitive")
        row = layout.row()
        row.prop(context.scene, "character_jump", text="Jump available")
        row = layout.row()
        row.operator(simple_character.bl_idname, text='Add Character')
        row = layout.row()
        row.operator(fly_camera.bl_idname, text='Add Fly Camera')

def register():
    bpy.utils.register_class(addcharacter)
    bpy.utils.register_class(fly_camera)
    bpy.utils.register_class(simple_character)

def unregister():
    bpy.utils.unregister_class(addcharacter)
    bpy.utils.unregister_class(fly_camera)
    bpy.utils.unregister_class(simple_character)

if __name__ == '__main__':
    register()
