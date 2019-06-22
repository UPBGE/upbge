import bge

class LogicNodeRoot(bge.types.LOG_Node):
	def start(self):
		self.trigger = self.outputs["Trigger Out"].value

	def update(self):
		return self.trigger
