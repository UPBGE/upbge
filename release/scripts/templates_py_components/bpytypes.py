import bge, bpy
from collections import OrderedDict

class Bootstrap(bge.types.KX_PythonComponent):
    args = OrderedDict((
        ("myAction", bpy.types.Action),
        ("myArmature", bpy.types.Armature),
        ("myCamera", bpy.types.Camera),
        ("myCollection", bpy.types.Collection),
        ("myCurve", bpy.types.Curve),
        ("myImage", bpy.types.Image),
        ("myKey", bpy.types.Key),
        ("myLibrary", bpy.types.Library),
        ("myLight", bpy.types.Light),
        ("myMaterial", bpy.types.Material),
        ("myMesh", bpy.types.Mesh),
        ("myMovieClip", bpy.types.MovieClip),
        ("myNodeTree", bpy.types.NodeTree),
        ("myObject", bpy.types.Object),
        ("myParticle", bpy.types.ParticleSettings),
        ("mySound", bpy.types.Sound),
        ("mySpeaker", bpy.types.Speaker),
        ("myText", bpy.types.Text),
        ("myTexture", bpy.types.Texture),
        ("myVectorFont", bpy.types.VectorFont),
        ("myVolume", bpy.types.Volume),
        ("myWorld", bpy.types.World),
    ))
    
    def start(self, args: dict):
        self.myObject = None
        if "myObject" in args:
            print("myObject = ", args["myObject"])
            self.myObject = args["myObject"]
        else:
            print("myObject not found!")
            
        

    def update(self) -> None:
        
        if self.myObject:
            print(self.myObject.name)