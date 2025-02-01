# SPDX-FileCopyrightText: Mitko Nikov
#
# SPDX-License-Identifier: GPL-3.0-or-later

import bpy
from bpy.types import AddonPreferences, PropertyGroup, Panel
from bpy.props import (StringProperty, EnumProperty, IntProperty,
                       FloatProperty, BoolProperty, PointerProperty)

from bpy.app.handlers import persistent
from bpy_extras.object_utils import world_to_camera_view

from mathutils import * 
from math import * 

bl_info = {
    "name": "Camera Vertex Cull",
    "author": "Mitko Nikov",
    "version": (1, 0, 1),
    "blender": (2, 83, 0),
    "location": "Object > Camera Vertex Cull",
    "description": "Hide vertices, edges and polys based on Camera Frustum.",
    "doc_url": "",
    "category": "Camera"
}

debug = False
hasEverEnabled = False
inWork = False # used for debouncing

# This creates a new array of vertices
# And applies the transform matrix in it
def applyTransform(obj):
    global debug
    
    mat = obj.matrix_world
    vertices = obj.data.vertices
    
    if debug:
        print('------')
        print(mat)
        print('------')
    
    verts = []
    for vert in vertices:
        verts.append(mat @ vert.co)
        
    return verts

# Gets the camera according to the context
# Also, does some cool checks
def getCamera(context):
    camera = context.scene.camera
    if camera is None:
        print("No scene camera")
    elif camera.type == 'CAMERA':
        if debug:
            print("Regular scene camera")
    else:
        print("%s object as camera" % camera.type)
        return False
    
    if debug:
        print(camera.data.view_frame())
        
    return camera

@persistent
def update_handler(self, dummy):
    context = bpy.context
    camera = getCamera(context)
    if camera is False:
        return False
    
    for obj in context.scene.objects:
        if obj.type == 'MESH':
            update_object(obj, context.scene, camera)
    
# This function updates the object based on context
def update_calc(self, context):
    object = context.object
    scene = context.scene
    
    camera = getCamera(context)
    if camera is False:
        return False
    
    if (object.type == 'MESH'):
        update_object(object, scene, camera)

# This is the main update object function
def update_object(object, scene, camera):
    global debug
    global inWork

    if inWork:
        return
    
    enabled = object.camera_cull_props.camera_cull_enabled
    dist_enabled = object.camera_cull_props.distance_cull_enabled
    margin = object.camera_cull_props.margin
    distance = object.camera_cull_props.distance

    if debug:
        print("Enabled: ", enabled)
    
    data = object.data
    vgs = object.vertex_groups
    
    if (object.type == 'CAMERA'):
        print("Object is camera, cannot make a Camera Vertex Cull")
        return False
    
    vg = None
    done = False
    
    # Search for our Hide Group (Vertex Group)
    for vgi in vgs:
        if (vgi.name == "Hide_Group"):
            vg = vgi
            done = True
    
    # Create it if it doesn't exist
    if done is False:
        if enabled is False:
            return

        vg = object.vertex_groups.new(name="Hide_Group") 
    
    # set every vertex weight to -1
    for v in data.vertices:
        vg.add([v.index], 1, "SUBTRACT")
    
    if enabled:
        # Apply the location, scale and rotation matrix
        objAfterTransform = applyTransform(object)
    
        # Convert to Camera View
        coords_2d = [world_to_camera_view(scene, camera, coord) for coord in objAfterTransform]
        
        # Iterate through the vertices
        # The count is used to find the vertex index
        count = -1
        for x, y, distance_to_lens in coords_2d:
            count = count + 1
            if (x >= -margin and x <= 1 + margin and y >= -margin and y <= 1 + margin):
                if (dist_enabled):
                    if (distance_to_lens <= distance):
                        continue
                else:
                    continue

            if debug:
                print("Pixel Coords:", (x, y, distance_to_lens))
            
            vg.add([data.vertices[count].index], 1, "ADD")
            
        inWork = True
        # Search for a Mask Modifier
        alreadyHaveMask = False
        for mod in object.modifiers:
            if mod.type == 'MASK':
                alreadyHaveMask = True
                break
                
        # Add if there's none
        if not alreadyHaveMask:
            bpy.ops.object.modifier_add(type='MASK')
            
            # Set up the modifier with the vertex group
            object.modifiers["Mask"].vertex_group = "Hide_Group"
            object.modifiers["Mask"].invert_vertex_group = True

        # Search for the handlers
        toAddHandler = True
        for han in bpy.app.handlers.frame_change_post:
            if han.__name__ is 'update_handler':
                toAddHandler = False
                break
        
        # Add the handlers if they don't exist
        # There's no need of the handlers if the addon is never used
        if toAddHandler:
            bpy.app.handlers.frame_change_post.append(update_handler)
            bpy.app.handlers.depsgraph_update_post.append(update_handler)
            if debug:
                print("[Camera Vertex Cull] Handlers added.")

        # This is used to know to remove the handlers if it's used
        global hasEverEnabled
        hasEverEnabled = True
        inWork = False

class CameraCullProperties(PropertyGroup):
    camera_cull_enabled: BoolProperty(
        name="Enable",
        description="Hide vertexes based on the Camera frustum",
        default=False,
        update=update_calc
    )

    distance_cull_enabled: BoolProperty(
        name="Enable Distance Cull",
        description="Hide vertexes based on the Camera Distance",
        default=False,
        update=update_calc
    )
    
    margin: FloatProperty(
        name="Margin",
        description="Threshold outside the Camera frustum",
        update=update_calc,
        default=0.3,
        precision=3,
        min=0,
        max=100
    )

    distance: FloatProperty(
        name="Distance",
        description="Culling Distance",
        update=update_calc,
        default=30,
        precision=1,
        min=0,
        max=1000
    )

class PLS_PT_CameraCullPropertiesPanel(Panel):
    """Camera Vertex Cull"""
    bl_idname = "PLS_PT_camera_vertex_cull"
    bl_label = "Camera Vertex Cull"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'object'
    
    context = None
    
    def draw(self, context):
        layout = self.layout
        if context.object.type == 'MESH':
            settings = context.object.camera_cull_props

            row = layout.row()
            
            row.prop(settings, "camera_cull_enabled")
            row.prop(settings, "distance_cull_enabled")
            
            layout.prop(settings, "margin")
            layout.prop(settings, "distance")
        else:
            layout.label(text="Select a mesh object")

class CameraVertexCullPreferences(AddonPreferences):
    bl_idname = __name__

    def draw(self, context):
        layout = self.layout
        layout.label(text="Vertex and Distance based culling")

classes = (
    CameraCullProperties,
    PLS_PT_CameraCullPropertiesPanel,
    CameraVertexCullPreferences
)

def register():
    for cls in classes:
        bpy.utils.register_class(cls)
        
    bpy.types.Object.camera_cull_props = PointerProperty(
        type=CameraCullProperties)

def unregister():
    global hasEverEnabled

    for cls in classes:
        bpy.utils.unregister_class(cls)
        
    del bpy.types.Object.camera_cull_props

    if hasEverEnabled:
        bpy.app.handlers.frame_change_post.remove(update_handler)
        bpy.app.handlers.depsgraph_update_post.remove(update_handler)

if __name__ == "__main__":
    register()
