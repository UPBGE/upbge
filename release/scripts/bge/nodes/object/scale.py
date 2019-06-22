import bge

class LogicNodeScale(bge.types.LOG_Node):
	def start(self):
		self.scale = self.inputs["scale"]
		self.trigger = self.outputs["Trigger Out"].value

	def update(self):
		object = self.object

		scale = self.scale.value
		object.worldScale.x *= scale.x
		object.worldScale.y *= scale.y
		object.worldScale.z *= scale.z

		return self.trigger
