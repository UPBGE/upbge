import bge
from collections import OrderedDict

class %Name%(bge.types.KX_GameObject):
    # Put your arguments here of the format ("key", default_value).
    # These values are exposed to the UI.
    args = OrderedDict([
    ])

    def start(self, args):
        # Put your initialization code here, args stores the values from the UI.
        # self.components refers to the list of Python components attached to this object.
        pass

    def update(self):
        # Put your code executed every logic step here.
        # self.components refers to the list of Python components attached to this object.
        pass
