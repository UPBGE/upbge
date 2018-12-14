import bge
from mathutils import Euler

class LogicNodeRotate(bge.types.LOG_Node):
	def start(self):
		self.rot = self.inputs["rotation"]
		self.trigger = self.outputs["Trigger Out"].value

	def update(self):
		object = self.object

		object.worldOrientation *= Euler(self.rot.value).to_matrix()

		return self.trigger
