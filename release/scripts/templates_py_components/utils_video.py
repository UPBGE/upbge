import bge
import sys

from collections import OrderedDict

if not hasattr(bge, "__component__"):
    # Put shared definitions here executed only in game engine.
    # e.g:
    # scene = bge.logic.getCurrentScene()
    pass

class Play(bge.types.KX_PythonComponent):
    # Put your arguments here of the format ("key", default_value).
    # These values are exposed to the UI.
    args = OrderedDict([
        ("File", "//my_file.avi"),
        ("Speed", 1.0),
        ("Repeat", -1),
        ("Flip", True),
    ])

    def start(self, args):
        self.file = args["File"]
        self.framerate = args["Speed"]
        self.repeat = args["Repeat"]
        self.flip = args["Flip"]

        path = bge.logic.expandPath(self.file)
        self.tex = bge.texture.Texture(self.object, 0, 0)
        self.tex.source = bge.texture.VideoFFmpeg(path)
        self.tex.source.repeat = self.repeat
        self.tex.source.scale = True
        self.tex.source.flip = self.flip
        self.tex.source.framerate = self.framerate
        self.tex.source.play()

    def update(self):
        self.tex.refresh(True)

class Capture(bge.types.KX_PythonComponent):
    # Put your arguments here of the format ("key", default_value).
    # These values are exposed to the UI.
    args = OrderedDict([
        ("Camera", "Camera_Name"),
        ("Framerate", 30.0),
        ("Width", 640),
        ("Height", 480),
        ("Flip", True),
    ])

    def start(self, args):
        self.camera_name = args["Camera"]
        self.framerate = args["Framerate"]
        self.width = args["Width"]
        self.height = args["Height"]
        self.flip = args["Flip"]

        self.tex = bge.texture.Texture(self.object, 0, 0)
        self.tex.source = bge.texture.VideoFFmpeg(self.camera_name, 0, self.framerate, self.width, self.height)
        self.tex.source.repeat = -1
        self.tex.source.scale = True
        self.tex.source.flip = self.flip
        self.tex.source.play()

    def update(self):
        self.tex.refresh(True)
