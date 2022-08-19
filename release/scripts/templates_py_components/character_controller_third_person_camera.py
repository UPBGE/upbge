###############################################################################
#              Third Person Camera | Template v 1.0 | UPBGE 0.3.0+            #
###############################################################################
#                      Created by: Guilherme Teres Nunes                      #
#                       Access: youtube.com/UnidayStudio                      #
#                               github.com/UnidayStudio                       #
#               github.com/UnidayStudio/UPBGE-CharacterController             #
#                                                                             #
#                           Copyright - July 2018                             #
#                        This work is licensed under                          #
#           the Creative Commons Attribution 4.0 International                #
###############################################################################

import bge
from collections import OrderedDict
from mathutils import Vector, Matrix

class ThirdPersonCamera(bge.types.KX_PythonComponent):
    """ Third Person Camera Component:
    Add a camera in your scene, parent them into your character capsule (you
    can use the Character controller Component on it), and attach this Component
    to the camera. And you're done! The component will do the rest for you. :)
    You can configure the mouse sensibility, invert X or Y axis and
    enable/disable the camera rotation limit.
    Don't forget to configure, as well, the camera position (height,
    distance and crab), if you want a camera collision (to prevent the camera
    from traversing walls), and if you want to align the player to the Camera
    View.
    """

    args = OrderedDict([
        ("Activate", True),
        ("Mouse Sensibility", 2.0),
        ("Invert Mouse X Axis", False),
        ("Invert Mouse Y Axis", False),
        ("Camera Height", 0.7),
        ("Camera Distance", 5.0),
        ("Camera Crab (Side)", 0.6),
        ("Camera Collision", True),
        ("Camera Collision Property", "ground"),
        ("Align Player to View", {"Never", "On Player Movement", "Always"}),
        ("Align Player Smooth", 0.5),
    ])

    def start(self, args):
        """Start Function"""
        self.active = args["Activate"]

        self.mouseSens = args["Mouse Sensibility"] * (-0.001)
        self.invertX = [1, -1][args["Invert Mouse X Axis"]]
        self.invertY = [1, -1][args["Invert Mouse Y Axis"]]

        # Camera Position
        self.__cameraPos = Vector([0,0,0])
        self.setCameraPos(args["Camera Crab (Side)"],
                          -args["Camera Distance"],
                          args["Camera Height"])

        self.cameraCol = args["Camera Collision"]
        self.cameraColProp = args["Camera Collision Property"]

        self.__camAlign = []
        self.setCameraAlign(args["Align Player to View"])
        self.camAlignSmooth = args["Align Player Smooth"]

        # To catch errors
        self.__error = (self.object.parent == None)
        if self.__error:
            print("[Third Person Camera] Error: The camera must be parent to an"
                  " object.")

        # Private Variables
        self.__cameraPan  = Matrix.Identity(3) # Rotate around Z axis (global)
        self.__cameraTilt = Matrix.Identity(3) # Rotate around X axis (local)

        self.__playerPos  = None
        if not self.__error:
            self.__playerPos  = self.object.parent.worldPosition.copy()


####-<PRIVATE FUNCTIONS>-######################################################

    def __pan(self, angle):
        """Private function: Rotate on the Z axis (pan)"""
        xyz = self.__cameraPan.to_euler()
        xyz[2] += angle
        self.__cameraPan = xyz.to_matrix()

    def __tilt(self, angle):
        """Private function: Rotate on the X axis (Tilt)"""
        xyz = self.__cameraTilt.to_euler()
        xyz[0] += angle
        self.__cameraTilt = xyz.to_matrix()

    def __getWorldCameraPos(self):
        """Private function: Gets the world camera position (based on the tilt
        and pan)"""
        vec = self.__cameraPos.copy()
        vec = self.__cameraTilt @ vec
        vec = self.__cameraPan @ vec
        return self.object.parent.worldPosition + vec

    def __limitCameraRot(self):
        """Private function: Defines a rotation limit to the camera to avoid it
        from rotating too much (and gets upside down)"""
        xyz = self.__cameraTilt.to_euler()

        if xyz[0] > 1.4:
            xyz[0] = 1.4
        elif xyz[0] < -1.4:
            xyz[0] = -1.4

        self.__cameraTilt = xyz.to_matrix()

    def __getPlayerMovementStatus(self):
        """Private function: Verifies if the player is moving"""

        flag = False
        vec = self.__playerPos - self.object.parent.worldPosition.copy()
        if vec.length > 0.001:
            flag = True
        self.__playerPos = self.object.parent.worldPosition.copy()

        return flag

    def __applyCameraPosition(self):
        """Private function: Applies the camera position"""

        camPos = self.__getWorldCameraPos()

        if self.cameraCol:
            target = self.object.parent.worldPosition + \
                     Vector([0, 0, self.__cameraPos[2] * 0.5])
            obHit, obPos, _ = self.object.rayCast(target, camPos, 0,
                                                  self.cameraColProp, 1, 0, 0)
            if obHit != None:
                camPos = obPos
        self.object.worldPosition = camPos

        align = self.__cameraTilt @ Vector([0, 1, 0])
        align = self.__cameraPan @ align

        self.object.alignAxisToVect([0, 0, 1], 1, 1)
        self.object.alignAxisToVect(align * (-1), 2, 1)


####-<PUBLIC FUNCTIONS>-#######################################################

    def setCameraAlign(self, type):
        """Public function to change the camera alignment."""

        self.__camAlign = {"Never": [0, 0],
                           "On Player Movement": [0, 1],
                           "Always": [1, 1]}[type]

    def setCameraPos(self, x, y, z):
        """Public function to change the camera position."""
        self.__cameraPos = Vector([x,y,z])

    def mouselook(self):
        """Mouselook function: Makes the mouse look at where you move your
        mouse."""
        wSize = Vector([bge.render.getWindowWidth(),
                        bge.render.getWindowHeight()])

        wCenter = Vector([int(wSize[0] * 0.5), int(wSize[1] * 0.5)])

        mPos = Vector(bge.logic.mouse.position)
        mPos[0] = int(mPos[0] * wSize[0])
        mPos[1] = int(mPos[1] * wSize[1])

        bge.render.setMousePosition(int(wCenter[0]), int(wCenter[1]))

        mDisp = mPos - wCenter
        mDisp *= self.mouseSens

        # Invert Mouselook
        mDisp[0] *= self.invertX
        mDisp[1] *= self.invertY

        self.__pan(mDisp[0])
        self.__tilt(mDisp[1])

        self.__limitCameraRot()

    def alignPlayerToView(self):
        """Aligns the player to the Camera view"""
        vec = self.getCameraView()
        self.object.parent.alignAxisToVect(vec, 1, 1.0 - self.camAlignSmooth)
        self.object.parent.alignAxisToVect([0,0,1], 2, 1)

    def getCameraView(self):
        """Returns the camera view direction"""
        return self.__cameraPan @ Vector([0,1,0])

    def update(self):
        """Update Function"""
        if self.active and not self.__error:
            self.mouselook()

            if self.__camAlign[self.__getPlayerMovementStatus()]:
                self.alignPlayerToView()

            self.__applyCameraPosition()
