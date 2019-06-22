import bge

class LogicNodeMousePosition(bge.types.LOG_FunctionNode):
	def start(self):
		self.mouse = self.logic.mouse

	def get(self):
		return self.mouse.position
