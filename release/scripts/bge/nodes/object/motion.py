import bge

class LogicNodeBasicMotion(bge.types.LOG_Node):
	def start(self):
		self.trans = self.inputs["translation"]
		self.rot = self.inputs["rotation"]
		self.scale  = self.inputs["scale"]
		self.trigger = self.outputs["Trigger Out"].value

	def update(self):
		object = self.object

		object.worldPosition += self.trans.value

		#scale = self.scale.value TODO
		#object.worldScale.x *= scale.x
		#object.worldScale.y *= scale.y
		#object.worldScale.z *= scale.z

		return self.trigger
