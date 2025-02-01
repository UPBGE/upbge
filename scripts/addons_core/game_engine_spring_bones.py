# SPDX-FileCopyrightText: Artell, Samurai-X
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "Spring Bones",
    "author": "Artell, added game engine support by Samurai-X",
    "version": (0, 9),
    "blender": (2, 80, 0),
    "location": "Properties > Bones",
    "description": "Add a spring dynamic effect to a single/multiple bones",    
    "category": "Animation"}


import bpy, time
from bpy.app.handlers import persistent
from mathutils import *
import math
import numpy
from numpy import dot
from math import sqrt
#from mathutils import Vector

script = '''
import bpy
from numpy import dot
from math import sqrt
import numpy
from mathutils import Vector

def lerp_vec(vec_a, vec_b, t):                        
    return vec_a*t + vec_b*(1-t)

def project_point_onto_plane(q, p, n):
    # q = (vector) point source
    # p = (vector) point belonging to the plane
    # n = (vector) normal of the plane
    
    n = n.normalized()
    return q - ((q-p).dot(n)) * n 
    
def project_point_onto_line(a, b, p):
    # project the point p onto the line a,b
    ap = p-a
    ab = b-a
    
    fac_a = (p-a).dot(b-a)
    fac_b = (p-b).dot(b-a)
    
    result = a + ap.dot(ab)/ab.dot(ab) * ab
    
    if fac_a < 0:
        result = a
    if fac_b > 0:
        result = b
    
    return result

def project_point_onto_tri(TRI, P):
    # return the distance and the projected surface point 
    # between a point and a triangle in 3D
    # original code: https://gist.github.com/joshuashaffer/
    # Author: Gwolyn Fischer
    
    B = TRI[0, :]
    E0 = TRI[1, :] - B
    # E0 = E0/sqrt(sum(E0.^2)); %normalize vector
    E1 = TRI[2, :] - B
    # E1 = E1/sqrt(sum(E1.^2)); %normalize vector
    D = B - P
    a = dot(E0, E0)
    b = dot(E0, E1)
    c = dot(E1, E1)
    d = dot(E0, D)
    e = dot(E1, D)
    f = dot(D, D)

    #print "{0} {1} {2} ".format(B,E1,E0)
    det = a * c - b * b
    s = b * e - c * d
    t = b * d - a * e

    # Terible tree of conditionals to determine in which region of the diagram
    # shown above the projection of the point into the triangle-plane lies.
    if (s + t) <= det:
        if s < 0.0:
            if t < 0.0:
                # region4
                if d < 0:
                    t = 0.0
                    if -d >= a:
                        s = 1.0
                        sqrdistance = a + 2.0 * d + f
                    else:
                        s = -d / a
                        sqrdistance = d * s + f
                else:
                    s = 0.0
                    if e >= 0.0:
                        t = 0.0
                        sqrdistance = f
                    else:
                        if -e >= c:
                            t = 1.0
                            sqrdistance = c + 2.0 * e + f
                        else:
                            t = -e / c
                            sqrdistance = e * t + f

                            # of region 4
            else:
                # region 3
                s = 0
                if e >= 0:
                    t = 0
                    sqrdistance = f
                else:
                    if -e >= c:
                        t = 1
                        sqrdistance = c + 2.0 * e + f
                    else:
                        t = -e / c
                        sqrdistance = e * t + f
                        # of region 3
        else:
            if t < 0:
                # region 5
                t = 0
                if d >= 0:
                    s = 0
                    sqrdistance = f
                else:
                    if -d >= a:
                        s = 1
                        sqrdistance = a + 2.0 * d + f;  # GF 20101013 fixed typo d*s ->2*d
                    else:
                        s = -d / a
                        sqrdistance = d * s + f
            else:
                # region 0
                invDet = 1.0 / det
                s = s * invDet
                t = t * invDet
                sqrdistance = s * (a * s + b * t + 2.0 * d) + t * (b * s + c * t + 2.0 * e) + f
    else:
        if s < 0.0:
            # region 2
            tmp0 = b + d
            tmp1 = c + e
            if tmp1 > tmp0:  # minimum on edge s+t=1
                numer = tmp1 - tmp0
                denom = a - 2.0 * b + c
                if numer >= denom:
                    s = 1.0
                    t = 0.0
                    sqrdistance = a + 2.0 * d + f;  # GF 20101014 fixed typo 2*b -> 2*d
                else:
                    s = numer / denom
                    t = 1 - s
                    sqrdistance = s * (a * s + b * t + 2 * d) + t * (b * s + c * t + 2 * e) + f

            else:  # minimum on edge s=0
                s = 0.0
                if tmp1 <= 0.0:
                    t = 1
                    sqrdistance = c + 2.0 * e + f
                else:
                    if e >= 0.0:
                        t = 0.0
                        sqrdistance = f
                    else:
                        t = -e / c
                        sqrdistance = e * t + f
                        # of region 2
        else:
            if t < 0.0:
                # region6
                tmp0 = b + e
                tmp1 = a + d
                if tmp1 > tmp0:
                    numer = tmp1 - tmp0
                    denom = a - 2.0 * b + c
                    if numer >= denom:
                        t = 1.0
                        s = 0
                        sqrdistance = c + 2.0 * e + f
                    else:
                        t = numer / denom
                        s = 1 - t
                        sqrdistance = s * (a * s + b * t + 2.0 * d) + t * (b * s + c * t + 2.0 * e) + f

                else:
                    t = 0.0
                    if tmp1 <= 0.0:
                        s = 1
                        sqrdistance = a + 2.0 * d + f
                    else:
                        if d >= 0.0:
                            s = 0.0
                            sqrdistance = f
                        else:
                            s = -d / a
                            sqrdistance = d * s + f
            else:
                # region 1
                numer = c + e - b - d
                if numer <= 0:
                    s = 0.0
                    t = 1.0
                    sqrdistance = c + 2.0 * e + f
                else:
                    denom = a - 2.0 * b + c
                    if numer >= denom:
                        s = 1.0
                        t = 0.0
                        sqrdistance = a + 2.0 * d + f
                    else:
                        s = numer / denom
                        t = 1 - s
                        sqrdistance = s * (a * s + b * t + 2.0 * d) + t * (b * s + c * t + 2.0 * e) + f

    # account for numerical round-off error
    if sqrdistance < 0:
        sqrdistance = 0

    dist = sqrt(sqrdistance)

    PP0 = B + s * E0 + t * E1
    return dist, PP0

def spring_bone(foo):
    #print("running...")
    scene = bpy.context.scene  
    deps = bpy.context.evaluated_depsgraph_get()    
  
    for bone in scene.sb_spring_bones: 
        # collider, skip
        if bone.sb_bone_collider:
            continue
        armature = bpy.data.objects[bone.armature]
        pose_bone = armature.pose.bones[bone.name]  
        # no influence, skip
        if pose_bone.sb_global_influence == 0.0:
            continue
          
        emp_tail = bpy.data.objects.get(bone.name + '_spring_tail')      
        emp_head = bpy.data.objects.get(bone.name + '_spring')
        
        if emp_tail == None or emp_head == None:
            #print("no empties found, return")
            return
                       
        emp_tail_loc, rot, scale = emp_tail.matrix_world.decompose()
                
        axis_locked = None        
        if 'sb_lock_axis' in pose_bone.keys():
            axis_locked = pose_bone.sb_lock_axis
        
        # add gravity
        base_pos_dir = Vector((0,0,-pose_bone.sb_gravity))
       
        # add spring
        base_pos_dir += (emp_tail_loc - emp_head.location)
        
        # evaluate bones collision
        if bone.sb_bone_colliding:
           
            for bone_col in scene.sb_spring_bones:            
                if bone_col.sb_bone_collider == False:
                    continue
                #print("collider bone", bone_col.name)
                pose_bone_col = armature.pose.bones[bone_col.name]   
                sb_collider_dist = pose_bone_col.sb_collider_dist
                #col_dir = (pose_bone.head - pose_bone_col.head)
                pose_bone_center = (pose_bone.tail + pose_bone.head)*0.5
                p = project_point_onto_line(pose_bone_col.head, pose_bone_col.tail, pose_bone_center)
                col_dir = (pose_bone_center - p)
                dist = col_dir.magnitude
                
                if dist < sb_collider_dist:   
                    push_vec = col_dir.normalized() * (sb_collider_dist-dist)*pose_bone_col.sb_collider_force
                    if axis_locked != "NONE" and axis_locked != None:                    
                        if axis_locked == "+Y":                        
                            direction_check = pose_bone.y_axis.normalized().dot(push_vec)                      
                            if direction_check > 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.y_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "-Y":                        
                            direction_check = pose_bone.y_axis.normalized().dot(push_vec)                      
                            if direction_check < 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.y_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "+X":                        
                            direction_check = pose_bone.x_axis.normalized().dot(push_vec)                      
                            if direction_check > 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.y_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "-X":                        
                            direction_check = pose_bone.x_axis.normalized().dot(push_vec)                      
                            if direction_check < 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.y_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "+Z":                        
                            direction_check = pose_bone.z_axis.normalized().dot(push_vec)                      
                            if direction_check > 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "-Z":                        
                            direction_check = pose_bone.z_axis.normalized().dot(push_vec)                      
                            if direction_check < 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                    #push_vec = push_vec - pose_bone.y_axis.normalized()*0.02
                    base_pos_dir += push_vec
           
        
            
            # evaluate mesh collision
            if  bone.sb_bone_colliding:
                for mesh in scene.sb_mesh_colliders:            
                    obj = bpy.data.objects.get(mesh.name)
                    pose_bone_center = (pose_bone.tail + pose_bone.head)*0.5
                    col_dir = Vector((0.0,0.0,0.0))
                    push_vec = Vector((0.0,0.0,0.0))
                   
                    object_eval = obj.evaluated_get(deps)
                    evaluated_mesh = object_eval.to_mesh(preserve_all_data_layers=False, depsgraph=deps)     
                    for tri in obj.data.loop_triangles:
                        tri_coords = []
                        for vi in tri.vertices:
                            v_coord = evaluated_mesh.vertices[vi].co
                            v_coord_global = obj.matrix_world @ v_coord
                            tri_coords.append([v_coord_global[0], v_coord_global[1], v_coord_global[2]])
                            
                        tri_array = numpy.array(tri_coords)
                        P = numpy.array([pose_bone_center[0], pose_bone_center[1], pose_bone_center[2]])
                        dist, p = project_point_onto_tri(tri_array, P)
                        p = Vector((p[0], p[1], p[2]))
                        collision_dist = obj.sb_collider_dist
                        repel_force = obj.sb_collider_force
                        
                        if dist < collision_dist:   
                            col_dir += (pose_bone_center - p)
                            push_vec = col_dir.normalized() * (collision_dist-dist) * repel_force
                            base_pos_dir += push_vec * pose_bone.sb_global_influence
            
                                                              
        # add velocity
        bone.speed += base_pos_dir * pose_bone.sb_stiffness
        bone.speed *= pose_bone.sb_damp
        
        emp_head.location += bone.speed
        # global influence                  
        emp_head.location = lerp_vec(emp_head.location, emp_tail_loc, pose_bone.sb_global_influence)     
            
    return None
if bpy.context.scene.sb_spring_game:
    if bpy.context.scene.sb_lastexec >= bpy.context.scene.sb_frame_tickrate:
        bpy.context.scene.sb_lastexec = 1
        spring_bone(True)
    else:
        bpy.context.scene.sb_lastexec += 1
'''                                     

#print('\n Start Spring Bones Addon... \n')


def set_active_object(object_name):
     bpy.context.view_layer.objects.active = bpy.data.objects[object_name]
     bpy.data.objects[object_name].select_set(state=1)

     
def get_pose_bone(name):  
    try:
        return bpy.context.object.pose.bones[name]
    except:
        return None
        
@persistent        
def spring_bone_frame_mode(foo):   
    if bpy.context.scene.sb_global_spring_frame == True:
        spring_bone(foo)        

@persistent
def spring_bone_gamestart(foo):
    if bpy.context.scene.sb_spring_game:
        try:
            update_bone(None, bpy.context)
        except:
            print("Initialize spring bones failed")
        else:
            my_obj = bpy.context.scene.armatr
            if not my_obj.script_created: 
                bpy.ops.text.new()
                text = bpy.data.texts[-1]
                text.write(script) 
                bpy.ops.logic.sensor_add(name='SpringBones', type='ALWAYS', object=my_obj.name)
                bpy.ops.logic.controller_add(type='PYTHON', object=my_obj.name)
                sensor = my_obj.game.sensors[-1]
                sensor.use_pulse_true_level = True
                cont = my_obj.game.controllers[-1]
                cont.text = text
                sensor.link(cont)
                my_obj.script_created = True

@persistent
def spring_bone_gameend(foo):
    if bpy.context.scene.sb_spring_game:
        for item in bpy.context.scene.sb_spring_bones:        
            
            active_bone = bpy.data.objects[item.armature].pose.bones.get(item.name)
            if active_bone == None:
                continue
                
            cns = active_bone.constraints.get('spring')
            if cns:            
                active_bone.constraints.remove(cns)  
                    
            emp1 = bpy.data.objects.get(active_bone.name + '_spring')
            emp2 = bpy.data.objects.get(active_bone.name + '_spring_tail')
            if emp1:     
                bpy.data.objects.remove(emp1)        
            if emp2:        
                bpy.data.objects.remove(emp2)
            
        print("--End-Game-")

 
def lerp_vec(vec_a, vec_b, t):                        
    return vec_a*t + vec_b*(1-t)
                             
    
def spring_bone(foo):
    #print("running...")
    scene = bpy.context.scene  
    deps = bpy.context.evaluated_depsgraph_get()    
  
    for bone in scene.sb_spring_bones: 
        # collider, skip
        if bone.sb_bone_collider:
            continue
        armature = bpy.data.objects[bone.armature]
        pose_bone = armature.pose.bones[bone.name]  
        # no influence, skip
        if pose_bone.sb_global_influence == 0.0:
            continue
          
        emp_tail = bpy.data.objects.get(bone.name + '_spring_tail')        
        emp_head = bpy.data.objects.get(bone.name + '_spring')
        
        if emp_tail == None or emp_head == None:
            #print("no empties found, return")
            return
                       
        emp_tail_loc, rot, scale = emp_tail.matrix_world.decompose()
                
        axis_locked = None        
        if 'sb_lock_axis' in pose_bone.keys():
            axis_locked = pose_bone.sb_lock_axis
        
        # add gravity
        base_pos_dir = Vector((0,0,-pose_bone.sb_gravity))
       
        # add spring
        base_pos_dir += (emp_tail_loc - emp_head.location)
        
        # evaluate bones collision
        if bone.sb_bone_colliding:
           
            for bone_col in scene.sb_spring_bones:            
                if bone_col.sb_bone_collider == False:
                    continue
                #print("collider bone", bone_col.name)
                pose_bone_col = armature.pose.bones[bone_col.name]   
                sb_collider_dist = pose_bone_col.sb_collider_dist
                #col_dir = (pose_bone.head - pose_bone_col.head)
                pose_bone_center = (pose_bone.tail + pose_bone.head)*0.5
                p = project_point_onto_line(pose_bone_col.head, pose_bone_col.tail, pose_bone_center)
                col_dir = (pose_bone_center - p)
                dist = col_dir.magnitude
                
                if dist < sb_collider_dist:   
                    push_vec = col_dir.normalized() * (sb_collider_dist-dist)*pose_bone_col.sb_collider_force
                    if axis_locked != "NONE" and axis_locked != None:                    
                        if axis_locked == "+Y":                        
                            direction_check = pose_bone.y_axis.normalized().dot(push_vec)                      
                            if direction_check > 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.y_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "-Y":                        
                            direction_check = pose_bone.y_axis.normalized().dot(push_vec)                      
                            if direction_check < 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.y_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "+X":                        
                            direction_check = pose_bone.x_axis.normalized().dot(push_vec)                      
                            if direction_check > 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.y_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "-X":                        
                            direction_check = pose_bone.x_axis.normalized().dot(push_vec)                      
                            if direction_check < 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.y_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "+Z":                        
                            direction_check = pose_bone.z_axis.normalized().dot(push_vec)                      
                            if direction_check > 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                        elif axis_locked == "-Z":                        
                            direction_check = pose_bone.z_axis.normalized().dot(push_vec)                      
                            if direction_check < 0:                        
                                locked_vec = project_point_onto_plane(push_vec, pose_bone.z_axis, pose_bone.x_axis)
                                push_vec = lerp_vec(push_vec, locked_vec, 0.3)
                                
                    #push_vec = push_vec - pose_bone.y_axis.normalized()*0.02
                    base_pos_dir += push_vec
           
        
            
            # evaluate mesh collision
            if  bone.sb_bone_colliding:
                for mesh in scene.sb_mesh_colliders:            
                    obj = bpy.data.objects.get(mesh.name)
                    pose_bone_center = (pose_bone.tail + pose_bone.head)*0.5
                    col_dir = Vector((0.0,0.0,0.0))
                    push_vec = Vector((0.0,0.0,0.0))
                   
                    object_eval = obj.evaluated_get(deps)
                    evaluated_mesh = object_eval.to_mesh(preserve_all_data_layers=False, depsgraph=deps)     
                    for tri in obj.data.loop_triangles:
                        tri_coords = []
                        for vi in tri.vertices:
                            v_coord = evaluated_mesh.vertices[vi].co
                            v_coord_global = obj.matrix_world @ v_coord
                            tri_coords.append([v_coord_global[0], v_coord_global[1], v_coord_global[2]])
                            
                        tri_array = numpy.array(tri_coords)
                        P = numpy.array([pose_bone_center[0], pose_bone_center[1], pose_bone_center[2]])
                        dist, p = project_point_onto_tri(tri_array, P)
                        p = Vector((p[0], p[1], p[2]))
                        collision_dist = obj.sb_collider_dist
                        repel_force = obj.sb_collider_force
                        
                        if dist < collision_dist:   
                            col_dir += (pose_bone_center - p)
                            push_vec = col_dir.normalized() * (collision_dist-dist) * repel_force
                            base_pos_dir += push_vec * pose_bone.sb_global_influence
            
                                                              
        # add velocity
        bone.speed += base_pos_dir * pose_bone.sb_stiffness
        bone.speed *= pose_bone.sb_damp
        
        emp_head.location += bone.speed
        # global influence                  
        emp_head.location = lerp_vec(emp_head.location, emp_tail_loc, pose_bone.sb_global_influence)     
            
    return None

    
    
def project_point_onto_plane(q, p, n):
    # q = (vector) point source
    # p = (vector) point belonging to the plane
    # n = (vector) normal of the plane
    
    n = n.normalized()
    return q - ((q-p).dot(n)) * n 
    
def project_point_onto_line(a, b, p):
    # project the point p onto the line a,b
    ap = p-a
    ab = b-a
    
    fac_a = (p-a).dot(b-a)
    fac_b = (p-b).dot(b-a)
    
    result = a + ap.dot(ab)/ab.dot(ab) * ab
    
    if fac_a < 0:
        result = a
    if fac_b > 0:
        result = b
    
    return result
    
    
def project_point_onto_tri(TRI, P):
    # return the distance and the projected surface point 
    # between a point and a triangle in 3D
    # original code: https://gist.github.com/joshuashaffer/
    # Author: Gwolyn Fischer
    
    B = TRI[0, :]
    E0 = TRI[1, :] - B
    # E0 = E0/sqrt(sum(E0.^2)); %normalize vector
    E1 = TRI[2, :] - B
    # E1 = E1/sqrt(sum(E1.^2)); %normalize vector
    D = B - P
    a = dot(E0, E0)
    b = dot(E0, E1)
    c = dot(E1, E1)
    d = dot(E0, D)
    e = dot(E1, D)
    f = dot(D, D)

    #print "{0} {1} {2} ".format(B,E1,E0)
    det = a * c - b * b
    s = b * e - c * d
    t = b * d - a * e

    # Terible tree of conditionals to determine in which region of the diagram
    # shown above the projection of the point into the triangle-plane lies.
    if (s + t) <= det:
        if s < 0.0:
            if t < 0.0:
                # region4
                if d < 0:
                    t = 0.0
                    if -d >= a:
                        s = 1.0
                        sqrdistance = a + 2.0 * d + f
                    else:
                        s = -d / a
                        sqrdistance = d * s + f
                else:
                    s = 0.0
                    if e >= 0.0:
                        t = 0.0
                        sqrdistance = f
                    else:
                        if -e >= c:
                            t = 1.0
                            sqrdistance = c + 2.0 * e + f
                        else:
                            t = -e / c
                            sqrdistance = e * t + f

                            # of region 4
            else:
                # region 3
                s = 0
                if e >= 0:
                    t = 0
                    sqrdistance = f
                else:
                    if -e >= c:
                        t = 1
                        sqrdistance = c + 2.0 * e + f
                    else:
                        t = -e / c
                        sqrdistance = e * t + f
                        # of region 3
        else:
            if t < 0:
                # region 5
                t = 0
                if d >= 0:
                    s = 0
                    sqrdistance = f
                else:
                    if -d >= a:
                        s = 1
                        sqrdistance = a + 2.0 * d + f;  # GF 20101013 fixed typo d*s ->2*d
                    else:
                        s = -d / a
                        sqrdistance = d * s + f
            else:
                # region 0
                invDet = 1.0 / det
                s = s * invDet
                t = t * invDet
                sqrdistance = s * (a * s + b * t + 2.0 * d) + t * (b * s + c * t + 2.0 * e) + f
    else:
        if s < 0.0:
            # region 2
            tmp0 = b + d
            tmp1 = c + e
            if tmp1 > tmp0:  # minimum on edge s+t=1
                numer = tmp1 - tmp0
                denom = a - 2.0 * b + c
                if numer >= denom:
                    s = 1.0
                    t = 0.0
                    sqrdistance = a + 2.0 * d + f;  # GF 20101014 fixed typo 2*b -> 2*d
                else:
                    s = numer / denom
                    t = 1 - s
                    sqrdistance = s * (a * s + b * t + 2 * d) + t * (b * s + c * t + 2 * e) + f

            else:  # minimum on edge s=0
                s = 0.0
                if tmp1 <= 0.0:
                    t = 1
                    sqrdistance = c + 2.0 * e + f
                else:
                    if e >= 0.0:
                        t = 0.0
                        sqrdistance = f
                    else:
                        t = -e / c
                        sqrdistance = e * t + f
                        # of region 2
        else:
            if t < 0.0:
                # region6
                tmp0 = b + e
                tmp1 = a + d
                if tmp1 > tmp0:
                    numer = tmp1 - tmp0
                    denom = a - 2.0 * b + c
                    if numer >= denom:
                        t = 1.0
                        s = 0
                        sqrdistance = c + 2.0 * e + f
                    else:
                        t = numer / denom
                        s = 1 - t
                        sqrdistance = s * (a * s + b * t + 2.0 * d) + t * (b * s + c * t + 2.0 * e) + f

                else:
                    t = 0.0
                    if tmp1 <= 0.0:
                        s = 1
                        sqrdistance = a + 2.0 * d + f
                    else:
                        if d >= 0.0:
                            s = 0.0
                            sqrdistance = f
                        else:
                            s = -d / a
                            sqrdistance = d * s + f
            else:
                # region 1
                numer = c + e - b - d
                if numer <= 0:
                    s = 0.0
                    t = 1.0
                    sqrdistance = c + 2.0 * e + f
                else:
                    denom = a - 2.0 * b + c
                    if numer >= denom:
                        s = 1.0
                        t = 0.0
                        sqrdistance = a + 2.0 * d + f
                    else:
                        s = numer / denom
                        t = 1 - s
                        sqrdistance = s * (a * s + b * t + 2.0 * d) + t * (b * s + c * t + 2.0 * e) + f

    # account for numerical round-off error
    if sqrdistance < 0:
        sqrdistance = 0

    dist = sqrt(sqrdistance)

    PP0 = B + s * E0 + t * E1
    return dist, PP0
    
                    
def update_bone(self, context):
    print("Updating data...")
    time_start = time.time()
    scene = bpy.context.scene   
    if bpy.context.mode == 'POSE':
        bpy.context.scene.armatr = bpy.context.active_object
    elif bpy.context.scene.armatr is None:
        print("Armature Object is not selected")
    armature = bpy.context.scene.armatr  
    deps = bpy.context.evaluated_depsgraph_get()
    #update collection
        #delete all
    if len(scene.sb_spring_bones) > 0:
        i = len(scene.sb_spring_bones)
        while i >= 0:          
            scene.sb_spring_bones.remove(i)
            i -= 1
        
        # mesh colliders
    if len(scene.sb_mesh_colliders) > 0:
        i = len(scene.sb_mesh_colliders)
        while i >= 0:          
            scene.sb_mesh_colliders.remove(i)
            i -= 1
            
    if armature is not None:    
        for pbone in armature.pose.bones:
            # are the properties there?
            if len(pbone.keys()) == 0:           
                continue            
            if not 'sb_bone_spring' in pbone.keys() and not 'sb_bone_collider' in pbone.keys():
                continue
                
            is_spring_bone = False
            is_collider_bone = False
            rotation_enabled =  False
            is_colliding = True
            
            if 'sb_bone_spring' in pbone.keys():
                if pbone.get("sb_bone_spring") == False:
                    # remove old spring constraints
                    spring_cns = pbone.constraints.get("spring")
                    if spring_cns:
                        pbone.constraints.remove(spring_cns)   
                    
                else:
                    is_spring_bone = True
                    
            if 'sb_bone_collider' in pbone.keys():        
                is_collider_bone = pbone.get("sb_bone_collider")
                    
            if 'sb_bone_rot' in pbone.keys():           
                rotation_enabled = pbone.get("sb_bone_rot") 
            if 'sb_collide' in pbone.keys():
                is_colliding = pbone.get('sb_collide')
                
            #print("iterating on", pbone.name)
            if is_spring_bone or is_collider_bone:
                item = bpy.context.scene.sb_spring_bones.add()
                item.name = pbone.name    
                print("registering", pbone.name)
                bone_tail = armature.matrix_world @ pbone.tail 
                bone_head = armature.matrix_world @ pbone.head 
                item.last_loc = bone_head
                item.armature = armature.name
                parent_name = ""
                if pbone.parent:
                    parent_name = pbone.parent.name          
                
                item.sb_bone_rot = rotation_enabled
                item.sb_bone_collider = is_collider_bone
                item.sb_bone_colliding = is_colliding
           
            #create empty helpers
            empty_radius = 1
            if is_spring_bone :
                if not bpy.data.objects.get(item.name + '_spring'):
                    """
                    # adding empties using operators cost too much performance
                    bpy.ops.object.mode_set(mode='OBJECT')       
                    bpy.ops.object.select_all(action='DESELECT')
                    if rotation_enabled:
                        bpy.ops.object.empty_add(type='PLAIN_AXES', radius = empty_radius, location=bone_tail, rotation=(0,0,0)) 
                    else:
                        bpy.ops.object.empty_add(type='PLAIN_AXES', radius = empty_radius, location=bone_head, rotation=(0,0,0)) 
                    empty = bpy.context.active_object   
                    empty.hide_set(True)
                    empty.hide_select = True
                    empty.name = item.name + '_spring'
                    """
                    o = bpy.data.objects.new(item.name+'_spring', None )
    
                    # due to the new mechanism of "collection"
                    bpy.context.scene.collection.objects.link(o)
    
                    # empty_draw was replaced by empty_display
                    o.empty_display_size = empty_radius
                    o.empty_display_type = 'PLAIN_AXES'   
                    o.location = bone_tail if rotation_enabled else bone_head                
                    o.hide_set(True)
                    o.hide_select = True
                    
                if not bpy.data.objects.get(item.name + '_spring_tail'):
                    empty = bpy.data.objects.new(item.name+'_spring_tail', None )
                    
                    # due to the new mechanism of "collection"
                    bpy.context.scene.collection.objects.link(empty)
    
                    # empty_draw was replaced by empty_display
                    empty.empty_display_size = empty_radius
                    empty.empty_display_type = 'PLAIN_AXES'   
                    #empty.location = bone_tail if rotation_enabled else bone_head
                    empty.matrix_world = Matrix.Translation(bone_tail if rotation_enabled else bone_head)
                    # >>setting the matrix instead of location attribute to avoid the despgraph update
                    # for performance reasons
                    #deps.update()
                    #empty.hide_set(True)
                    empty.hide_select = True
                    """
                    bpy.ops.object.mode_set(mode='OBJECT')        
                    bpy.ops.object.select_all(action='DESELECT')         
                    if rotation_enabled:
                        bpy.ops.object.empty_add(type='PLAIN_AXES', radius = empty_radius, location=bone_tail, rotation=(0,0,0)) 
                    else:
                        bpy.ops.object.empty_add(type='PLAIN_AXES', radius = empty_radius, location=bone_head, rotation=(0,0,0)) 
                    empty = bpy.context.active_object    
                    empty.hide_set(True)
                    empty.hide_select = True
                                                   
                    empty.name = item.name + '_spring_tail'   
                    """
                    mat = empty.matrix_world.copy()                                  
                    empty.parent = armature
                    empty.parent_type = 'BONE'
                    empty.parent_bone = parent_name
                    empty.matrix_world = mat
                    
                #create constraints
                if pbone['sb_bone_spring'] == True:
                    #set_active_object(armature.name)
                    #bpy.ops.object.mode_set(mode='POSE')
                    spring_cns = pbone.constraints.get("spring")
                    if spring_cns:
                        pbone.constraints.remove(spring_cns)                
                    if pbone.sb_bone_rot:
                        cns = pbone.constraints.new('DAMPED_TRACK')
                        cns.target = bpy.data.objects[item.name + '_spring']
                    else:
                        cns = pbone.constraints.new('COPY_LOCATION')
                        cns.target = bpy.data.objects[item.name + '_spring']                
                    cns.name = 'spring' 
                      
        
    # mesh colliders
    for obj in bpy.data.objects:
        if obj.type == "MESH":
            if obj.sb_object_collider:              
                obj.data.calc_loop_triangles()
                item = scene.sb_mesh_colliders.add()
                item.name = obj.name                   
                break
             
             
    #set_active_object(armature.name)
    #bpy.ops.object.mode_set(mode='POSE')
    
    print("Updated in", round(time.time()-time_start, 1), "seconds.")    
  
def end_spring_bone(context, self):
    if context.scene.sb_global_spring:
        #print("GOING TO CLOSE TIMER...")        
        wm = context.window_manager
        wm.event_timer_remove(self.timer_handler)
        #print("CLOSE TIMER")
      
        context.scene.sb_global_spring = False
    
    for item in context.scene.sb_spring_bones:        
        
        active_bone = bpy.context.active_object.pose.bones.get(item.name)
        if active_bone == None:
            continue
            
        cns = active_bone.constraints.get('spring')
        if cns:            
            active_bone.constraints.remove(cns)  
               
        emp1 = bpy.data.objects.get(active_bone.name + '_spring')
        emp2 = bpy.data.objects.get(active_bone.name + '_spring_tail')
        if emp1:     
            bpy.data.objects.remove(emp1)        
        if emp2:        
            bpy.data.objects.remove(emp2)
    
    print("--End--end")
    

class SB_OT_spring_modal(bpy.types.Operator):
    """Spring Bones, interactive mode"""
    
    bl_idname = "sb.spring_bone"
    bl_label = "spring_bone" 
    
    def __init__(self):
        self.timer_handler = None
     
    def modal(self, context, event):  
        #print("self.timer_handler =", self.timer_handler)
        if event.type == "ESC" or context.scene.sb_global_spring == False:         
            self.cancel(context)
            #print("ESCAPE")
            return {'FINISHED'}  
            
        if event.type == 'TIMER':       
            spring_bone(context)
        
        
        return {'PASS_THROUGH'}

        
    def execute(self, context):     
        args = (self, context)  
        #print("self.timer_handler =", self.timer_handler)
        # enable spring bone
        if context.scene.sb_global_spring == False:  
            wm = context.window_manager
            self.timer_handler = wm.event_timer_add(0.02, window=context.window)            
            wm.modal_handler_add(self)
            print("--Start modal--")
            
            context.scene.sb_global_spring = True
            update_bone(self, context)
            
            return {'RUNNING_MODAL'}    
        
        # disable spring selection
        
        else:
            print("--End modal--")
            #self.cancel(context)
            context.scene.sb_global_spring = False
            return {'FINISHED'}  
           
            
    def cancel(self, context):
        #if context.scene.sb_global_spring:
        #print("GOING TO CLOSE TIMER...")
        
        wm = context.window_manager
        wm.event_timer_remove(self.timer_handler)
        #print("CLOSED TIMER")
          
        context.scene.sb_global_spring = False
        
        for item in context.scene.sb_spring_bones:        
            active_bone = bpy.context.active_object.pose.bones.get(item.name)
            if active_bone == None:
                continue
                
            cns = active_bone.constraints.get('spring')
            if cns:            
                active_bone.constraints.remove(cns)  
                   
            emp1 = bpy.data.objects.get(active_bone.name + '_spring')
            emp2 = bpy.data.objects.get(active_bone.name + '_spring_tail')
            if emp1:     
                bpy.data.objects.remove(emp1)        
            if emp2:        
                bpy.data.objects.remove(emp2)
        
        print("--End-- interactive")
            
            
class SB_OT_spring(bpy.types.Operator):
    """Spring Bones, animation mode. Support baking."""
    
    bl_idname = "sb.spring_bone_frame"
    bl_label = "spring_bone_frame" 

   
    def execute(self, context):        
        if context.scene.sb_global_spring_frame == False:           
            context.scene.sb_global_spring_frame = True
            update_bone(self, context)
 
        else:
            end_spring_bone(context, self)
            context.scene.sb_global_spring_frame = False  
            
        return {'FINISHED'}  
        
class SB_OT_select_bone(bpy.types.Operator):
    """Select this bone"""
    
    bl_idname = "sb.select_bone"
    bl_label = "select_bone" 
    
    bone_name : bpy.props.StringProperty(default="")
   
    def execute(self, context):        
        data_bone = get_pose_bone(self.bone_name).bone
        bpy.context.active_object.data.bones.active = data_bone
        data_bone.select = True
        for i, l in enumerate(data_bone.layers):
            if l == True and bpy.context.active_object.data.layers[i] == False:
                bpy.context.active_object.data.layers[i] = True
                #print("enabled layer", i)
            
            
        #get_pose_bone(self.bone_name).select = True
            
        return {'FINISHED'}  
            
            
###########  UI PANEL  ###################

class SB_PT_Game_ui(bpy.types.Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'scene'
    bl_label = "Spring Bones Game" 

    @classmethod
    def poll(cls, context):
        return (context.scene is not None)

    def draw(self, context):
        col = self.layout.column(align=True)
        col.label(text="Spring Bones Game Settings")
        col.prop(context.scene, 'sb_spring_game', text="Use Spring in Game")
        col.prop(context.scene, 'armatr', text="Armature Object")
        col.prop(context.scene, 'sb_frame_tickrate', text="Run every _ frames")

class SB_PT_ui(bpy.types.Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'bone'
    bl_label = "Spring Bones"    
    
    @classmethod
    
    def poll(cls, context):
        return context.active_object
       
    def draw(self, context):    
        layout = self.layout
        object = context.object
        
        scene = context.scene
        col = layout.column(align=True)
        
        if context.mode == "POSE" and bpy.context.active_pose_bone:
            active_bone = bpy.context.active_pose_bone
            #col.label(text='Scene Parameters:')
            col = layout.column(align=True)
            #col.prop(scene, 'sb_global_spring', text="Enable spring")
            if context.scene.sb_global_spring == False:
                col.operator(SB_OT_spring_modal.bl_idname, text="Start - Interactive Mode", icon='PLAY')           
            if context.scene.sb_global_spring == True:
                col.operator(SB_OT_spring_modal.bl_idname, text="Stop", icon='PAUSE')          
          
            col.enabled = not context.scene.sb_global_spring_frame
          
            col = layout.column(align=True)
            if context.scene.sb_global_spring_frame == False:          
                col.operator(SB_OT_spring.bl_idname, text="Start - Animation Mode", icon='PLAY')
            if context.scene.sb_global_spring_frame == True:           
                col.operator(SB_OT_spring.bl_idname, text="Stop", icon='PAUSE')
                
            col.enabled = not context.scene.sb_global_spring
            
            col = layout.column(align=True)
            
            col.label(text='Bone Parameters:')
            col.prop(active_bone, 'sb_bone_spring', text="Spring")        
            col.prop(active_bone, 'sb_bone_rot', text="Rotation")
            col.prop(active_bone, 'sb_stiffness', text="Bouncy")
            col.prop(active_bone,'sb_damp', text="Speed")
            col.prop(active_bone,'sb_gravity', text="Gravity")
            col.prop(active_bone,'sb_global_influence', text="Influence")
            col.prop(active_bone,'sb_collide', text="Is Colliding")
            col.label(text="Lock axis when colliding:")
            col.prop(active_bone, 'sb_lock_axis', text="")
            col.enabled = not active_bone.sb_bone_collider
            
            layout.separator()
            col = layout.column(align=True)
            col.prop(active_bone, 'sb_bone_collider', text="Collider")
            col.prop(active_bone, 'sb_collider_dist', text="Collider Distance")
            col.prop(active_bone, 'sb_collider_force', text="Collider Force")       
            col.enabled = not active_bone.sb_bone_spring
            
            layout.separator()
            layout.prop(scene, "sb_show_colliders")
            col = layout.column(align=True)
            
            if scene.sb_show_colliders:
                for pbone in bpy.context.active_object.pose.bones:
                    if "sb_bone_collider" in pbone.keys():
                        if pbone.sb_bone_collider:
                            row = col.row()
                            row.label(text=pbone.name)
                            r = row.operator(SB_OT_select_bone.bl_idname, text="Select")
                            r.bone_name = pbone.name
        
      
 
class SB_PT_object_ui(bpy.types.Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'object'
    bl_label = "Spring Bones"    
    
    @classmethod
    
    def poll(cls, context):
        if context.active_object:
            return context.active_object.type == "MESH"
       
    def draw(self, context):    
        layout = self.layout
        object = context.active_object
        
        scene = context.scene
        col = layout.column(align=True)
        
        if context.mode == "OBJECT" and context.active_object:
            
            col = layout.column(align=True)
            col.prop(context.active_object, 'sb_object_collider', text="Collider")
            col.prop(context.active_object, 'sb_collider_dist', text="Collider Distance")
            col.prop(context.active_object, 'sb_collider_force', text="Collider Force")       
            
#### REGISTER ############# 

class bones_collec(bpy.types.PropertyGroup):
    armature : bpy.props.StringProperty(default="")
    last_loc : bpy.props.FloatVectorProperty(name="Loc", subtype='DIRECTION', default=(0,0,0), size = 3)    
    speed : bpy.props.FloatVectorProperty(name="Speed", subtype='DIRECTION', default=(0,0,0), size = 3)
    dist: bpy.props.FloatProperty(name="distance", default=1.0)
    target_offset : bpy.props.FloatVectorProperty(name="TargetLoc", subtype='DIRECTION', default=(0,0,0), size = 3)    
    sb_bone_rot : bpy.props.BoolProperty(name="Bone Rot", default=False)
    sb_bone_collider: bpy.props.BoolProperty(name="Bone collider", default=False)
    sb_bone_colliding: bpy.props.BoolProperty(name="Bone colliding", default=True)
    sb_collider_dist : bpy.props.FloatProperty(name="Bone collider distance", default=0.5)
    sb_collider_force : bpy.props.FloatProperty(name="Bone collider force", default=1.0)    
    matrix_offset = Matrix()
    initial_matrix = Matrix()
    
class mesh_collec(bpy.types.PropertyGroup):
    test : bpy.props.StringProperty(default="")
    
    
classes = (SB_PT_Game_ui, SB_PT_ui, SB_PT_object_ui, bones_collec, mesh_collec, SB_OT_spring_modal, SB_OT_spring, SB_OT_select_bone)
        
def register():
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)   

    bpy.app.handlers.frame_change_post.append(spring_bone_frame_mode)
    bpy.app.handlers.game_pre.append(spring_bone_gamestart)
    bpy.app.handlers.game_post.append(spring_bone_gameend)
    bpy.types.Scene.sb_spring_bones = bpy.props.CollectionProperty(type=bones_collec)  
    bpy.types.Scene.sb_mesh_colliders = bpy.props.CollectionProperty(type=mesh_collec)       
    bpy.types.Scene.sb_global_spring = bpy.props.BoolProperty(name="Enable spring", default = False)#, update=update_global_spring)
    bpy.types.Scene.armatr = bpy.props.PointerProperty(name="Armature",type=bpy.types.Object)
    bpy.types.Scene.sb_global_spring_frame = bpy.props.BoolProperty(name="Enable Spring", description="Enable Spring on frame change only", default = False)
    bpy.types.Scene.sb_spring_game = bpy.props.BoolProperty(name="Enable Spring in Game", description="Enable Spring in the game engine", default = True)
    bpy.types.Scene.sb_frame_tickrate = bpy.props.IntProperty(name="Spring tick rate", description='Run Spring every x frames', default=1, min=1, max=20)
    bpy.types.Scene.sb_lastexec = bpy.props.IntProperty(default=1)
    bpy.types.Scene.sb_show_colliders = bpy.props.BoolProperty(name="Show Colliders", description="Show active colliders names", default = False)
    bpy.types.PoseBone.sb_bone_spring = bpy.props.BoolProperty(name="Enabled", default=False, description="Enable spring effect on this bone")
    bpy.types.PoseBone.sb_bone_collider = bpy.props.BoolProperty(name="Collider", default=False, description="Enable this bone as collider")
    bpy.types.PoseBone.sb_collider_dist = bpy.props.FloatProperty(name="Collider Distance", default=0.5, description="Minimum distance to handle collision between the spring and collider bones")
    bpy.types.PoseBone.sb_collider_force = bpy.props.FloatProperty(name="Collider Force", default=1.0, description="Amount of repulsion force when colliding")
    bpy.types.PoseBone.sb_stiffness = bpy.props.FloatProperty(name="Stiffness", default=0.5, min = 0.01, max = 1.0, description="Bouncy/elasticity value, higher values lead to more bounciness")
    bpy.types.PoseBone.sb_damp = bpy.props.FloatProperty(name="Damp", default=0.7, min=0.0, max = 10.0, description="Speed/damping force applied to the bone to go back to it initial position") 
    bpy.types.PoseBone.sb_gravity = bpy.props.FloatProperty(name="Gravity", description="Additional vertical force to simulate gravity", default=0.0, min=-100.0, max = 100.0) 
    bpy.types.PoseBone.sb_bone_rot = bpy.props.BoolProperty(name="Rotation", default=False, description="The spring effect will apply on the bone rotation instead of location")
    bpy.types.PoseBone.sb_lock_axis = bpy.props.EnumProperty(items=(('NONE', 'None', ""), ('+X', '+X', ''), ('-X', '-X', ''), ('+Y', "+Y", ""), ('-Y', '-Y', ""), ('+Z', '+Z', ""), ('-Z', '-Z', '')), default="NONE")
    bpy.types.Object.script_created = bpy.props.BoolProperty(default=False)
    bpy.types.Object.sb_object_collider = bpy.props.BoolProperty(name="Collider", default=False, description="Enable this bone as collider")
    bpy.types.Object.sb_collider_dist = bpy.props.FloatProperty(name="Collider Distance", default=0.5, description="Minimum distance to handle collision between the spring and collider bones")
    bpy.types.Object.sb_collider_force = bpy.props.FloatProperty(name="Collider Force", default=1.0, description="Amount of repulsion force when colliding")
    bpy.types.PoseBone.sb_collide = bpy.props.BoolProperty(name="Colliding", default = True, description="The bone will collide with other colliders")#, update=update_global_spring)
    bpy.types.PoseBone.sb_global_influence = bpy.props.FloatProperty(name="Influence", default = 1.0, min=0.0, max=1.0, description="Global influence of spring motion")#, update=update_global_spring)
    
    
def unregister():
    from bpy.utils import unregister_class
    
    for cls in reversed(classes):
        unregister_class(cls)   
    
    bpy.app.handlers.frame_change_post.remove(spring_bone_frame_mode) 
    bpy.app.handlers.game_pre.remove(spring_bone_gamestart)
    bpy.app.handlers.game_post.remove(spring_bone_gameend)
    
    del bpy.types.Scene.sb_spring_bones  
    del bpy.types.Scene.sb_mesh_colliders
    del bpy.types.Scene.sb_global_spring
    del bpy.types.Scene.sb_global_spring_frame
    del bpy.types.Scene.sb_spring_game
    del bpy.types.Scene.armatr
    del bpy.types.Scene.sb_frame_tickrate
    del bpy.types.Scene.sb_lastexec
    del bpy.types.Scene.sb_show_colliders
    del bpy.types.PoseBone.sb_bone_spring
    del bpy.types.PoseBone.sb_bone_collider
    del bpy.types.PoseBone.sb_collider_dist
    del bpy.types.PoseBone.sb_collider_force
    del bpy.types.PoseBone.sb_stiffness
    del bpy.types.PoseBone.sb_damp
    del bpy.types.PoseBone.sb_gravity
    del bpy.types.PoseBone.sb_bone_rot
    del bpy.types.PoseBone.sb_lock_axis
    del bpy.types.Object.script_created
    del bpy.types.Object.sb_object_collider
    del bpy.types.Object.sb_collider_dist
    del bpy.types.Object.sb_collider_force
    del bpy.types.PoseBone.sb_collide
    del bpy.types.PoseBone.sb_global_influence
    
    
if __name__ == "__main__":
    register()
    
