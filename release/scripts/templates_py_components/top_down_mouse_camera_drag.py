###############################################################################
#             Camera Drag Controller | Template v 1.0 | UPBGE 0.3.0           #
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
from mathutils import Vector, Matrix
from collections import OrderedDict

class MouseCameraDrag(bge.types.KX_PythonComponent):
    """ HOW TO USE IN YOUR PROJECTS:
    It's very easy to use: Just load this script into your .blend file (or paste
    them in the same folder that your .blend is), select your camera (or an obj,
    you decide) and attach them into the
    components (top_down_mouse_camera_drag.MouseCameraDrag).

    It's very simple to configure:
    -> Show Mouse: Enable if you want to show the mouse
    -> Mouse Movement: Enable if you want to activate the mouse drag logic
    -> Mouse Button: Which mouse button you want to use
    -> Keyboard Movement: Enable if you want to move the object using W,A,S,D
    -> Up Axis: Select the UP axis.
    -> Local Movement: Local or Global movement? You decide!
    -> Mouse Sensibility: The mouse sensibility!
    -> Keyboard Speed: If you enabled the Keyboard Movement, control the speed
                       here!
    -> Limit Area: You can limit the area that the object can stay by playing
                   around with this values. If you don't want, just set to 0
    """

    args = OrderedDict([
        ("Show Mouse", True),
        ("Mouse Movement", True),
        ("Mouse Button", {"Right Mouse Button", "Middle Mouse Button", "Left Mouse Button"}),
        ("Keyboard Movement", True),
        ("Up Axis", {"Z Axis", "Y Axis", "X Axis"}),
        ("Local Movement", False),
        ("Mouse Sensibility", 0.5),
        ("Keyboard Speed", 0.3),
        ("Limit Area", Vector([0.0, 0.0, 0.0])),
    ])

    def start(self, args):
        """ Start Funtion """
        self.hasMouseMovement = args["Mouse Movement"]
        self.hasKeyboardMovement = args["Keyboard Movement"]

        self.mButton = {"Left Mouse Button"   :bge.events.LEFTMOUSE,
                        "Middle Mouse Button" :bge.events.MIDDLEMOUSE,
                        "Right Mouse Button"  :bge.events.RIGHTMOUSE}
        self.mButton = self.mButton[args["Mouse Button"]]

        self.upAxis = {"X Axis":0, "Y Axis":1, "Z Axis":2}[args["Up Axis"]]
        self.localMovement = args["Local Movement"]
        self.sens = args["Mouse Sensibility"] * 1000
        self.speed = args["Keyboard Speed"]

        self.hasAreaLimit = None
        if args["Limit Area"] != [0, 0, 0]:
            self.hasAreaLimit = args["Limit Area"]

        if args["Show Mouse"]:
            bge.render.showMouse(True)

        self.__lastMousePos = Vector([0, 0])

    def __moveX(self, value):
        """Moves the object on the X axis (whatever axis this mean)"""
        vec = Vector([value, 0, 0])
        if self.upAxis == 0:
            vec = Vector([0, value, 0])
        if not self.localMovement and vec.length != 0:
            vec = self.object.worldOrientation @ vec
            vec[self.upAxis] = 0
        self.object.applyMovement(vec * self.speed, self.localMovement)

    def __moveY(self, value):
        """Moves the object on the Y axis (whatever axis this mean)"""
        vec = Vector([0, value, 0])
        if self.upAxis == 0:
            vec = Vector([0, 0, value])
        if not self.localMovement and vec.length != 0:
            vec = self.object.worldOrientation @ vec
            vec[self.upAxis] = 0
        self.object.applyMovement(vec * self.speed, self.localMovement)

    def keyboardMovement(self):
        """Makes the object move with the keyboard (W,A,S,D keys)"""
        keyboard = bge.logic.keyboard.inputs
        x = 0; y = 0

        if keyboard[bge.events.WKEY].active:	y = 1
        elif keyboard[bge.events.SKEY].active:	y = -1
        if keyboard[bge.events.AKEY].active:	x = -1
        elif keyboard[bge.events.DKEY].active:	x = 1

        self.__moveX(x)
        self.__moveY(y)

    def mouseMovement(self):
        """Makes the object move by clicking (LMB) and dragging the mouse"""
        mouse = bge.logic.mouse.inputs
        mPos = Vector(bge.logic.mouse.position)

        if mouse[self.mButton].active:
            # Mouse displacement since last frame
            mDisp = self.__lastMousePos - mPos
            mDisp *= self.sens # Apply Mouse sensibility
            self.__moveX(mDisp[0])
            self.__moveY(mDisp[1] * (-1))

        self.__lastMousePos = mPos

    def limitArea(self):
        """Limits the area that this object can stay. If you don't want this
        limitation, just set the values to zero (0)."""
        for axis in range(3):
            if self.hasAreaLimit[axis] == 0:
                continue
            value = self.object.worldPosition[axis]
            if abs(value) > self.hasAreaLimit[axis]:
                final = self.hasAreaLimit[axis] * (value / abs(value))
                self.object.worldPosition[axis] = final

    def update(self):
        """Update Function"""

        if self.hasKeyboardMovement:
            self.keyboardMovement()
        if self.hasMouseMovement:
            self.mouseMovement()
        if self.hasAreaLimit != None:
            self.limitArea()
