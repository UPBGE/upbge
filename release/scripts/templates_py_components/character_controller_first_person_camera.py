###############################################################################
#              First Person Camera | Template v 1.0 | UPBGE 0.3.0+            #
###############################################################################
#                      Created by: Guilherme Teres Nunes                      #
#                       Access: youtube.com/UnidayStudio                      #
#                               github.com/UnidayStudio                       #
#                 github.com/UnidayStudio/UPBGE-CharacterController	          #
#                                                                             #
#                           Copyright - July 2018                             #
#                        This work is licensed under                          #
#           the Creative Commons Attribution 4.0 International                #
###############################################################################

import bge
from collections import OrderedDict
from mathutils import Vector

class FirstPersonCamera(bge.types.KX_PythonComponent):
    """
    First Person Camera Component:
    Add a camera in your scene, parent them into your character capsule (you
    can use the Character controller Component on it), and attach this Component
    to the camera. Don't forgot to position the camera in a place near the
    "head" of your character.
    You can configure the mouse sensibility, invert X or Y axis and
    enable/disable the camera rotation limit.
    """

    args = OrderedDict([
        ("Activate", True),
        ("Mouse Sensibility", 2.0),
        ("Invert Mouse X Axis", False),
        ("Invert Mouse Y Axis", False),
        ("Limit Camera Rotation", True),
    ])

    def start(self, args):
        """Start Function"""
        self.active = args["Activate"]

        self.mouseSens = args["Mouse Sensibility"] * (-0.001)
        self.invertX = [1, -1][args["Invert Mouse X Axis"]]
        self.invertY = [1, -1][args["Invert Mouse Y Axis"]]
        self.limitRot = args["Limit Camera Rotation"]

    def mouselook(self):
        """Mouselook function: Makes the mouse look at where you move your
        mouse."""

        wSize = Vector([bge.render.getWindowWidth(),
                        bge.render.getWindowHeight()])

        wCenter = Vector([int(wSize[0] * 0.5), int(wSize[1] * 0.5)])

        mPos = Vector(bge.logic.mouse.position)
        mPos[0] = int(mPos[0] * wSize[0])
        mPos[1] = int(mPos[1] * wSize[1])

        mDisp = mPos - wCenter
        mDisp *= self.mouseSens

        obj = self.object.parent
        if obj == None:
            obj = self.object
        obj.applyRotation([0, 0, mDisp[0] * self.invertX], False)
        self.object.applyRotation([mDisp[1] * self.invertY, 0, 0], True)

        bge.render.setMousePosition(int(wCenter[0]), int(wCenter[1]))

    def cameraLimits(self):
        """Camera Limits function: Defines a rotation limit to the camera to
        avoid it from rotating too much (and gets upside down)"""
        xyz = self.object.localOrientation.to_euler()

        if abs(xyz[0]) > 2.7:
            xyz[0] = 2.7
        elif abs(xyz[0]) < 0.3:
            xyz[0] = 0.3

        self.object.localOrientation = xyz.to_matrix()

    def update(self):
        """Update Function"""
        if self.active:
            self.mouselook()

            if self.limitRot:
                self.cameraLimits()
