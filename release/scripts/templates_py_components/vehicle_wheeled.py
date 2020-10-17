# #############################
# Vehicle Physics Component
# #############################
# This file is adapted from 
# the demo file published in:
# "Game Development with Blender"
# by Dalai Felinto and Mike Pan
#
# Published by "CENGAGE Learning" in 2013
#
# You are free to use-it, modify it and redistribute
# as long as you keep the original credits when pertinent.
#
# File tested with UpBGE 0.3.0
#
# Copyright - February 2013
# This work is licensed under the Creative Commons
# Attribution-Share Alike 3.0 Unported License
# #############################

##################################################################################################
# This component requires a RigidBody with colision bounds set to either Convex Hull or Triangle Mesh
# and four objects as wheels, not parented, and with physics set to No Collision
# 
# To drive and steer the vehicle create "force" and "steer" properties on the owner of component.
# You can read the Speed of the vehicle if you create a "speed" property
##################################################################################################

# import needed modules
import bge
import bpy
import math
from mathutils import Vector

from collections import OrderedDict

if not hasattr(bge, "__component__"):
    # get the list of objects
    s = bge.logic.getCurrentScene().objects

class setup(bge.types.KX_PythonComponent):
    """See https://github.com/UPBGE/upbge/wiki/Release-notes-version-0.3.0#vehicle-wrapper-python-component-template-by-nestanquiero"""
    # Put your arguments here of the format ("key", default_value).
    # These values are exposed to the UI.
    args = OrderedDict([
    ("Mass", 10.0),("Stability", 0.05),
    ("Front_Wheel_L", "object1"), ("Rear_Wheel_L", "object2"), ("Rear_Wheel_R", "object3"), ("Front_Wheel_R", "object4"), 
    ("Suspension_Height", 0.1), ("Tyre_Friction", 3.0), 
    ("Suspension_Stiffness", 40.0), ("Suspension_Damping", 5.0), ("Suspension_Compression", 5.0), ("Roll_Influence", 0.2),
    ])


    def start(self, args):
        
        self.object['speed'] = 0.0
        self.object['force'] = 0.0
        self.object['steer'] = 0.0
        
        # WHEEL OBJECTS
        self.rd_0=s[args["Front_Wheel_L"]]
        self.rd_1=s[args["Rear_Wheel_L"]]
        self.rd_2=s[args["Rear_Wheel_R"]]
        self.rd_3=s[args["Front_Wheel_R"]]
        
        # WHEEL POSITION
        self.rd_p_0 = -(self.object.worldPosition-s[args["Front_Wheel_L"]].worldPosition)
        self.rd_p_1 = -(self.object.worldPosition-s[args["Rear_Wheel_L"]].worldPosition)
        self.rd_p_2 = -(self.object.worldPosition-s[args["Rear_Wheel_R"]].worldPosition)
        self.rd_p_3 = -(self.object.worldPosition-s[args["Front_Wheel_R"]].worldPosition)
        
        # SUSPENSION ANGLE
        self.rd_an_0 = self.rd_an_1 = self.rd_an_2 = self.rd_an_3 = [0.0, 0.0, -1.0]
        
        # AXLE
        self.rd_e_0 = self.rd_e_1 = self.rd_e_2 = self.rd_e_3 = [-1.0, 0.0, 0.0]
        
        # SUSPENSION HEIGHT
        self.rd_a_s_0 = self.rd_a_s_1 = self.rd_a_s_2 = self.rd_a_s_3 = args["Suspension_Height"]
        
        # WHEEL RADIUS takes the height of each wheel and divides by two
        self.rd_r_0 = (bpy.data.objects[args["Front_Wheel_L"]].dimensions[2]/2)
        self.rd_r_1 = (bpy.data.objects[args["Rear_Wheel_L"]].dimensions[2]/2)
        self.rd_r_2 = (bpy.data.objects[args["Rear_Wheel_R"]].dimensions[2]/2)
        self.rd_r_3 = (bpy.data.objects[args["Front_Wheel_R"]].dimensions[2]/2)

        # steering
        self.rd_d_0 = True
        self.rd_d_1 = False
        self.rd_d_2 = False
        self.rd_d_3 = True
        
        #configure blender object physics
        self.object.mass = args["Mass"]
        self.object['Stability'] = args["Stability"]
        
        
        # CREATE PHYSICS
        vehicle = bge.constraints.createConstraint(self.object.getPhysicsId(),0,bge.constraints.VEHICLE_CONSTRAINT)
        
        vehicle = vehicle.getConstraintId()
        self.object["vehicle"] = vehicle = bge.constraints.getVehicleConstraint(vehicle)
        self.object["dS"] = 0.0

        # add as rodas no vehicle
        vehicle.addWheel(self.rd_0, self.rd_p_0, self.rd_an_0, self.rd_e_0, self.rd_a_s_0, self.rd_r_0, self.rd_d_0)
        vehicle.addWheel(self.rd_1, self.rd_p_1, self.rd_an_1, self.rd_e_1, self.rd_a_s_1, self.rd_r_1, self.rd_d_1)
        vehicle.addWheel(self.rd_2, self.rd_p_2, self.rd_an_2, self.rd_e_2, self.rd_a_s_2, self.rd_r_2, self.rd_d_2)
        vehicle.addWheel(self.rd_3, self.rd_p_3, self.rd_an_3, self.rd_e_3, self.rd_a_s_3, self.rd_r_3, self.rd_d_3)
        
        ## set vehicle roll tendency ##
        vehicle.setRollInfluence(0.25,0)
        vehicle.setRollInfluence(0.25,1)
        vehicle.setRollInfluence(0.25,2)
        vehicle.setRollInfluence(0.25,3)
        
        ## set vehicle suspension compression ratio ##
        vehicle.setSuspensionCompression( args["Suspension_Compression"],0)
        vehicle.setSuspensionCompression(args["Suspension_Compression"],1)
        vehicle.setSuspensionCompression(args["Suspension_Compression"],2)
        vehicle.setSuspensionCompression(args["Suspension_Compression"],3)
        
        ## set vehicle suspension dampness ##
        vehicle.setSuspensionDamping(args["Suspension_Damping"],0)
        vehicle.setSuspensionDamping(args["Suspension_Damping"],1)
        vehicle.setSuspensionDamping(args["Suspension_Damping"],2)
        vehicle.setSuspensionDamping(args["Suspension_Damping"],3)
        
        ## set vehicle suspension hardness ##
        vehicle.setSuspensionStiffness(args["Suspension_Stiffness"],0)
        vehicle.setSuspensionStiffness(args["Suspension_Stiffness"],1)
        vehicle.setSuspensionStiffness(args["Suspension_Stiffness"],2)
        vehicle.setSuspensionStiffness(args["Suspension_Stiffness"],3)
        
        ## set vehicle tire friction ##
        vehicle.setTyreFriction(args["Tyre_Friction"],0)
        vehicle.setTyreFriction(args["Tyre_Friction"],1)
        vehicle.setTyreFriction(args["Tyre_Friction"],2)
        vehicle.setTyreFriction(args["Tyre_Friction"],3)

    def update(self):
        # Put your code executed every logic step here.
        # self.object is the owner object of this component.
        vehicle = self.object['vehicle']
        ## calculate speed by using the back wheel rotation speed ##
        S = vehicle.getWheelRotation(2)+vehicle.getWheelRotation(3)
        self.object["speed"] = (S - self.object["dS"])*10.0
        
        vehicle.applyEngineForce(self.object["force"],0)
        vehicle.applyEngineForce(self.object["force"],1)
        vehicle.applyEngineForce(self.object["force"],2)
        vehicle.applyEngineForce(self.object["force"],3)
        
        ## calculate steering with varying sensitivity ##
        if math.fabs(self.object["speed"])<15.0: s = 2.0
        elif math.fabs(self.object["speed"])<28.0: s=1.5
        elif math.fabs(self.object["speed"])<40.0: s=1.0
        else: s=0.5
        
        ## steer front wheels
        vehicle.setSteeringValue(self.object["steer"]*s,0)
        vehicle.setSteeringValue(self.object["steer"]*s,3)
        
        ## slowly ease off gas and center steering ##
        self.object["steer"] *= 0.6
        self.object["force"] *= 0.9
        
        ## align car to Z axis to prevent flipping ##
        self.object.alignAxisToVect([0.0,0.0,1.0], 2, self.object["Stability"])
        
        ## store old values ##
        self.object["dS"] = S
