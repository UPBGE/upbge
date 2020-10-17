###############################################################################
#           Sound Speaker Controller | Template v 1.0 | UPBGE 0.3.0           #
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
import aud
from collections import OrderedDict

class SoundSpeaker(bge.types.KX_PythonComponent):
    """ This component will serve as an sound Speaker for your game. With this,
    you can easly control 3D sound, volume.
    Unfortunatelly, the sounds needs to be mono to make the 3D sound works.
    You can convert to mono using windows CMD like this:
    > ffmpeg -i Sound.wav -ac 1 SoundMono.wav"""

    args = OrderedDict([
        ("Sound File", ""),
        ("Loop Sound", True),
        ("Volume", 1.0),
        ("Pitch", 1.0),
        ("3D Sound", True),
        ("Min Distance", 1.0),
        ("Max Distance", 100.0),
        ("Delete Object After End", False),
    ])

    def start(self, args):
        """Start Function"""
        scene = bge.logic.getCurrentScene()
        cam = scene.active_camera

        # Loading the device...
        self.device = aud.Device()
        self.device.distance_model = aud.DISTANCE_MODEL_LINEAR

        # Loading the sound...
        sName = bge.logic.expandPath("//")+args["Sound File"]
        self.sound = aud.Sound(sName)

        # Playing the sound...
        self.handle = self.device.play(self.sound)

        # 3D Sound configuration...
        self.__3dSound = args["3D Sound"]
        self.handle.relative = (self.__3dSound == False)
        self.handle.distance_maximum = abs(args["Max Distance"])
        self.handle.distance_reference = abs(args["Min Distance"])
        self.handle.pitch = args["Pitch"]

        self.handle.volume = args["Volume"]

        if args["Loop Sound"]:
            self.handle.loop_count = -1
        else:
            self.handle.loop_count = 0

        self.__deleteObj = args["Delete Object After End"]

    def pauseSound(self):
        """Function to pause the sound"""
        self.handle.pause()

    def resumeSound(self):
        """Function to resume the sound"""
        self.handle.resume()

    def stopSound(self):
        """Function to stop the sound (and delete the object)"""
        self.handle.stop()
        if self.__deleteObj:
            self.object.endObject()

    def update(self):
        """Update Function"""
        scene = bge.logic.getCurrentScene()
        cam = scene.active_camera

        if self.__3dSound:
            self.device.listener_location = cam.worldPosition
            self.device.listener_orientation = cam.worldOrientation.to_quaternion()
            try:
                self.handle.location = self.object.worldPosition
            except:
                if self.__deleteObj:
                    self.object.endObject()

    def dispose(self):
        """Dispose function (to avoid sound playing when game ends)"""
        self.handle.stop()
