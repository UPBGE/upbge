import bge

class LogicNodeTranslate(bge.types.LOG_Node):
	def start(self):
		self.trans = self.inputs["translation"]
		self.trigger = self.outputs["Trigger Out"].value

	def update(self):
		self.object.worldPosition += self.trans.value

		return self.trigger
