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
		print(self.outputs)
		return self.outputs["Trigger Out"]

class LogicNodeBasicMotion(bge.types.LOG_Node):
	def start(self):
		print(type(self), self.inputs, self.outputs, self.properties)

	def update(self):
		self.object.localPosition += self.inputs["translation"]
		return self.outputs["Trigger Out"]

class LogicNodeMath(bge.types.LOG_FunctionNode):
	def start(self):
		print(type(self), self.inputs, self.properties)

	def get(self):
		if self.properties["mode"] == 0:
			return self.inputs["a"] + self.inputs["b"];
