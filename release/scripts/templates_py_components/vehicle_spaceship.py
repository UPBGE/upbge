import bge, mathutils
from mathutils import Vector
from collections import OrderedDict
from datetime import datetime, timedelta

class ToyRocket(bge.types.KX_PythonComponent):
    """
        Component for a simple arcade style spaceship controls.
        
        To use it select your rocket object and in set it's physics as "Dynamic".
        Then in logic editor click "Register Component" button.
        In the popup type in "vehicle_spaceship.ToyRocket" and that's it.
        
        
        This component will read keys from your keyboard and state oy your mouse and handle movement of the rocket.
        'W','S','A','D' - Forward, Backward, Strafe Left, Strafe Right
        'SPACE', 'CTRL' - Up, Down (relative to the ship, and not the world)
        'LSHIFT' - Turbo (ship will fly faster)
        'Q', 'E' - Roll Left, Roll Right
        Mouse X, Mouse Y - Yaw, Pitch
        LMB - Pew Pew LMB
        RMB - Pew Pew RMB
        
        As the script is right now LMB and RMB only shows lazer lines.
        Red when something was hit, and Green when shot missed.
        You can freely customize this behavior in the "pew_pew" methods
    """
    
    args = OrderedDict([
        ("Forward Speed Gain", 1.0),
        ("Strafe Speed Gain", 1.0),
        ("Incline Speed Gain", 1.0),
        ("Turbo Speed Limit", 50.0),
        ("Move Speed Limit", 20.0),
        ("Move Speed Drag", 0.99),
        ("Roll Speed Gain", 1.0),
        ("Roll Speed_Limit", 1.0),
        ("Roll Speed Drag", 0.6),
        ("Mouse X Speed", 1.0),
        ("Mouse Y Speed", 1.0),
        ("Mouse LMB Delay", 100.0),
        ("Mouse RMB Delay", 1000.0),
    ])


    def start(self, args):
        self.forward_speed_gain = args['Forward Speed Gain']
        self.strafe_speed_gain = args['Strafe Speed Gain']
        self.incline_speed_gain = args['Incline Speed Gain']
        self.turbo_speed_limit = args['Turbo Speed Limit']
        self.move_speed_limit = args['Move Speed Limit']
        self.move_speed_drag = args['Move Speed Drag']
        
        self.roll_speed_limit = args['Roll Speed_Limit']
        self.roll_speed_gain = args['Roll Speed Gain']
        self.roll_speed_drag = args['Roll Speed Drag']
        
        self.mouse_x_speed = args['Mouse X Speed']
        self.mouse_y_speed = args['Mouse Y Speed']
        
        self.mouse_lmb_delay = args['Mouse LMB Delay']
        self.mouse_rmb_delay = args['Mouse RMB Delay']
        
        self.window_height = bge.render.getWindowWidth()
        self.window_width = bge.render.getWindowHeight()

        self.center_x = int(0.5 * self.window_height);
        self.center_y = int(0.5 * self.window_width);
        
        self.object.gravity = [0,0,0]
        
        self.object.linVelocityMax = self.move_speed_limit
        self.object.angularVelocityMax = self.roll_speed_limit
        
        self.last_lmb_time = datetime.now()
        self.last_rmb_time = datetime.now()
        
        bge.render.setMousePosition(int(self.center_x), int(self.center_y))


    def update(self):
        keyboard = bge.logic.keyboard
        mouse = bge.logic.mouse
        
        inputs = keyboard.inputs

        mouse_position = Vector([mouse.position[0] * self.window_height, mouse.position[1] * self.window_width])
        center =  Vector([self.center_x, self.center_y])
        
        delta = center - mouse_position
        
        delta_x = delta[0] / self.window_width
        delta_y = delta[1] / self.window_height
                
        bge.render.setMousePosition(int(self.center_x), int(self.center_y))

        if mouse.inputs[bge.events.LEFTMOUSE].active:
            now = datetime.now()
            if (now - self.last_lmb_time) / timedelta(milliseconds=1) > self.mouse_lmb_delay:
                self.last_lmb_time = now
                self.pew_pew_lmb()

        if mouse.inputs[bge.events.RIGHTMOUSE].active:
            now = datetime.now()
            if (now - self.last_rmb_time) / timedelta(milliseconds=1) > self.mouse_rmb_delay:
                self.last_rmb_time = now
                self.pew_pew_rmb()

        if inputs[bge.events.LEFTSHIFTKEY].active:
            self.object.linVelocityMax = self.turbo_speed_limit
        else:
            self.object.linVelocityMax = self.move_speed_limit

        movement = self.object.localLinearVelocity * self.move_speed_drag

        if inputs[bge.events.WKEY].active:
            movement[0] += self.forward_speed_gain
        elif inputs[bge.events.SKEY].active:
            movement[0] -= self.forward_speed_gain

        if inputs[bge.events.AKEY].active:
            movement[1] += self.strafe_speed_gain
        elif inputs[bge.events.DKEY].active:
            movement[1] -= self.strafe_speed_gain

        if inputs[bge.events.SPACEKEY].active:
            movement[2] += self.incline_speed_gain
        elif inputs[bge.events.LEFTCTRLKEY].active:
            movement[2] -= self.incline_speed_gain

        rotation = self.object.localAngularVelocity

        if inputs[bge.events.EKEY].active:
            rotation[0] += self.roll_speed_gain
        elif inputs[bge.events.QKEY].active:
            rotation[0] -= self.roll_speed_gain
        else:
            rotation[0] *= self.roll_speed_drag

        rotation[1] = 0.0
        rotation[2] = 0.0

        self.object.localLinearVelocity = movement
        self.object.localAngularVelocity = rotation

        self.object.applyRotation((0, -1.0 * self.mouse_y_speed * delta_y, self.mouse_x_speed * delta_x), True)
        
        #Pew Pew

    def pew_pew_lmb(self):
        """ Left Mouse Button """
        own = self.object.worldPosition
        end = self.object.worldPosition + self.object.worldOrientation @ Vector([10,0,0])
        ray = self.object.rayCast(end,own,1000)
        
        if ray[0]:
            hit = ray[1]
            bge.render.drawLine(own, hit, (1,0,0))
        else:
            bge.render.drawLine(own, end, (0,1,0))

        bge.logic.getCurrentScene().resetTaaSamples = True
            
            
    def pew_pew_rmb(self):
        """ Right Mouse Button """
        own = self.object.worldPosition
        end = self.object.worldPosition + self.object.worldOrientation @ Vector([10,0,0])
        ray = self.object.rayCast(end,own,1000)
        
        if ray[0]:
            hit = ray[1]
            bge.render.drawLine(own, hit, (1,0,0))
        else:
            bge.render.drawLine(own, end, (0,1,0))

        bge.logic.getCurrentScene().resetTaaSamples = True
