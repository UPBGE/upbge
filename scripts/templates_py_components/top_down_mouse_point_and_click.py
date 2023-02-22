###############################################################################
#             Mouse Click Controller | Template v 1.0 | UPBGE 0.3.0           #
###############################################################################
#                      Created by: Guilherme Teres Nunes                      #
#                       Access: youtube.com/UnidayStudio                      #
#                               github.com/UnidayStudio                       #
#                                                                             #
#                           Copyright - July 2018                             #
#                        This work is licensed under                          #
#           the Creative Commons Attribution 4.0 International                #
###############################################################################

import bge
from mathutils import Vector
from collections import OrderedDict

class MousePointAndClick(bge.types.KX_PythonComponent):
    """
    HOW TO USE IN YOUR PROJECTS:
    It's very easy to use: Just load this script into your .blend file (or paste
    them in the same folder that your .blend is), select your camera (or an obj,
    you decide) and attach them into the
    components (top_down_mouse_point_and_click.MousePointAndClick).

    It's very simple to configure:
    -> Activate: Activate or deactivate the logic
    -> Mouse Button: Which mouse button you want to use
    -> Align To Normal: Enable if you want to align the object to the mouse over
                        normal.
    -> Property: The property that you want to interact with (leave this blank
                 if you want to interact with everything).
    """

    args = OrderedDict([
        ("Activate", True),
        ("Mouse Button", {"Left Mouse Button", "Middle Mouse Button", "Right Mouse Button"}),
        ("Align To Normal", False),
        ("Property", "ground"),
    ])

    def start(self, args):
        """Start Function"""
        self.active = args["Activate"]
        self.prop = args["Property"]
        self.alignNormal = args["Align To Normal"]

        self.mButton = {"Left Mouse Button" :bge.events.LEFTMOUSE,
                        "Middle Mouse Button": bge.events.MIDDLEMOUSE,
                        "Right Mouse Button": bge.events.RIGHTMOUSE}
        self.mButton = self.mButton[args["Mouse Button"]]

    def mouseClick(self):
        """If the user clicks on a valid place, makes the obj teleports to that place"""
        scene = bge.logic.getCurrentScene()
        cam = scene.active_camera

        mPos = bge.logic.mouse.position
        sVec = cam.getScreenVect(mPos[0], mPos[1])
        target = cam.worldPosition - sVec

        obHit, obPos, obNormal = self.object.rayCast(target, cam, 5000, self.prop, 1, 1, 0)

        if obHit != None:
            self.object.worldPosition = obPos
            if self.alignNormal:
                self.object.alignAxisToVect(obNormal, 2, 1)

    def update(self):
        """Update Function"""
        mouse  = bge.logic.mouse.inputs
        keyTAP = bge.logic.KX_INPUT_JUST_ACTIVATED

        if self.active:
            if keyTAP in mouse[self.mButton].queue:
                self.mouseClick()
