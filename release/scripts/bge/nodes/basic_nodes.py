import bge
import operator
from functools import reduce

class LogicNodeRoot(bge.types.LOG_Node):
	def start(self):
		print(type(self), self.inputs, self.outputs, self.properties)

	def update(self):
		return self.outputs["Trigger Out"]

class LogicNodeBooleanValue(bge.types.LOG_FunctionNode):
	def start(self):
		print(type(self), self.inputs, self.properties)

	def get(self):
		return bool(self.properties[0])

class LogicNodeBooleanOperator(bge.types.LOG_FunctionNode):
	def start(self):
		print(type(self), self.inputs, self.properties)

	def get(self):
		mode = self.properties["mode"]

		if mode == 0:
			return reduce(operator.and_, self.inputs)
		if mode == 1:
			return not reduce(operator.and_, self.inputs)
		if mode == 2:
			return reduce(operator.or_, self.inputs)
		if mode == 3:
			return not reduce(operator.or_, self.inputs)
		if mode == 3:
			return reduce(operator.xor_, self.inputs)
		if mode == 3:
			return not reduce(operator.xor_, self.inputs)

class LogicNodeBranch(bge.types.LOG_Node):
	def start(self):
		print(type(self), self.inputs, self.outputs, self.properties)

	def update(self):
		val = self.inputs["value"]

		return self.outputs["Trigger Positive" if val else "Trigger Negative"]

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

class LogicNodeMathOperator(bge.types.LOG_FunctionNode):
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
