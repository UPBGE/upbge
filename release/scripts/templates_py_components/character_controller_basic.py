import bge
from collections import OrderedDict

class ThirdPerson(bge.types.KX_PythonComponent):
    """Basic third character controller

    W: move forward
    A: turn left
    S: move backward
    D: turn right

    """

    args = OrderedDict([
        ("Move Speed", 0.1),
        ("Turn Speed", 0.04)
    ])

    def start(self, args):
        self.move_speed = args['Move Speed']
        self.turn_speed = args['Turn Speed']

    def update(self):
        keyboard = bge.logic.keyboard
        inputs = keyboard.inputs

        move = 0
        rotate = 0

        if inputs[bge.events.WKEY].values[-1]:
            move += self.move_speed
        if inputs[bge.events.SKEY].values[-1]:
            move -= self.move_speed

        if inputs[bge.events.AKEY].values[-1]:
            rotate += self.turn_speed
        if inputs[bge.events.DKEY].values[-1]:
            rotate -= self.turn_speed

        self.object.applyMovement((0, move, 0), True)
        self.object.applyRotation((0, 0, rotate), True)
