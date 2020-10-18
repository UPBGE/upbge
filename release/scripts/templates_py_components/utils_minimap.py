###############################################################################
#               Minimap Controller | Template v 1.0 | UPBGE 0.3.0             #
###############################################################################
#                      Created by: Guilherme Teres Nunes                      #
#                       Access: youtube.com/UnidayStudio                      #
#                               github.com/UnidayStudio                       #
#               github.com/UnidayStudio/UPBGE-UtilsTemplate                   #
#                                                                             #
#                           Copyright - July 2018                             #
#                        This work is licensed under                          #
#           the Creative Commons Attribution 4.0 International                #
###############################################################################

import bge
from mathutils import Vector
from collections import OrderedDict

class Minimap(bge.types.KX_PythonComponent):
    """ This component will spawn a minimap based on the camera (which owns the
    component) view. Add this component to a camera and position it on top of
    your character.
    You can read a small doc about this Component here:
    https://github.com/UnidayStudio/UPBGE-UtilsTemplate"""

    args = OrderedDict([
        ("Activate", True),
        ("Camera Type", {"Perspective", "Orthographic"}),
        ("Camera Height", 10.0),
        ("Minimap Position", Vector([0.11, 0.11])),
        ("Minimap Size", Vector([0.2, 0.2])),
        ("Follow Object", ""),
        ("Rotate on Z axis", False),
    ])

    def start(self, args):
        """Start Function"""
        scene = bge.logic.getCurrentScene()
        cam = scene.active_camera

        # Saving some variables.
        self.followObject = None
        if args["Follow Object"] != "":
            self.followObject = scene.objects[args["Follow Object"]]
        self.rotateZaxis = args["Rotate on Z axis"]
        self.camType = args["Camera Type"]
        self.camHeight = args["Camera Height"]

        self.active = args["Activate"]

        if not self.active:
            return

        # Adjusting the viewports to show two cameras.
        wWidth = bge.render.getWindowWidth()
        wHeight= bge.render.getWindowHeight()
        cam.useViewport = True
        cam.setViewport(0, 0, wWidth, wHeight)

        self.object.useViewport = True
        mPos   = Vector(args["Minimap Position"])
        mSize  = Vector(args["Minimap Size"]) * 0.5
        mStart = mPos - mSize
        mEnd   = mPos + mSize

        self.object.setViewport(int(mStart[0] * wWidth), int(mStart[1] * wHeight),
                                int(mEnd[0] * wWidth),   int(mEnd[1] * wHeight))
        self.object.setOnTop()

        # Some pre configurations...
        if self.camType == "Orthographic":
            self.object.perspective = False
            self.object.ortho_scale = self.camHeight
        else:
            self.object.perspective = True
            if self.followObject == None and self.camHeight != 0:
                self.object.worldPosition[2] = self.camHeight

    def update(self):
        """Update Function"""
        if self.followObject != None and self.active:
            self.object.worldPosition = self.followObject.worldPosition.copy()
            if self.camType == "Perspective":
                self.object.worldPosition[2] += self.camHeight

            if self.rotateZaxis:
                dir = self.followObject.worldOrientation @ Vector([0,1,0])
                dir[2] = 0
                self.object.alignAxisToVect(dir, 1, 1)
                self.object.alignAxisToVect([0, 0, 1], 2, 1)
