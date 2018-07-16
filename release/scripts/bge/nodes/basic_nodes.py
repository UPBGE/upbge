import bge

class LogicNodeRoot(bge.types.LOG_Node):
	def start(self):
		print(type(self), self.inputs, self.outputs, self.properties)

	def update(self):
		return self.outputs["Trigger Out"]

class LogicNodeBoolean(bge.types.LOG_Node):
	def start(self):
		print(type(self), self.inputs, self.outputs, self.properties)

	def update(self):
		return self.outputs["Trigger Out"]

class LogicNodeBasicMotion(bge.types.LOG_Node):
	def start(self):
		print(type(self), self.inputs, self.outputs, self.properties)

	def update(self):
		self.object.localPosition += self.inputs["translation"]
		return self.outputs["Trigger Out"]
