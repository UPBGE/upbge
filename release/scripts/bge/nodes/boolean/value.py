import bge

class LogicNodeBooleanValue(bge.types.LOG_FunctionNode):
	def start(self):
		self.value = bool(self.properties["value"])

	def get(self):
		return self.value
