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
		trans = self.inputs["translation"]
		rot = self.inputs["rotation"]
		scale  = self.inputs["scale"]

		self.object.worldPosition += trans

		self.object.worldScale.x *= scale.x
		self.object.worldScale.y *= scale.y
		self.object.worldScale.z *= scale.z

		return self.outputs["Trigger Out"]

class LogicNodeMath(bge.types.LOG_FunctionNode):
	def start(self):
		print(type(self), self.inputs, self.properties)

	def get(self):
		mode = self.properties["mode"]
		a = self.inputs["a"]
		b = self.inputs["b"]

		if mode == 0:
			return a + b
		if mode == 1:
			return a - b
		if mode == 2:
			return a * b
		if mode == 3:
			return a.dot(b)
		if mode == 4:
			return a.cross(b)
