import bge
import operator
from functools import reduce

class LogicNodeRoot(bge.types.LOG_Node):
	def start(self):
		self.trigger = self.outputs["Trigger Out"].value

	def update(self):
		return self.trigger

class LogicNodeBooleanValue(bge.types.LOG_FunctionNode):
	def start(self):
		self.value = bool(self.properties["value"])

	def get(self):
		return self.value

class LogicNodeBooleanOperator(bge.types.LOG_FunctionNode):
	def start(self):
		self.mode = self.properties["mode"]
	@property
	def booleans(self):
		return (socket.value for socket in self.inputs)

	def get(self):
		if self.mode == 0:
			return reduce(operator.__and__, self.booleans)
		if self.mode == 1:
			return not reduce(operator.__and__, self.booleans)
		if self.mode == 2:
			return reduce(operator.__or__, self.booleans)
		if self.mode == 3:
			return not reduce(operator.__or__, self.booleans)
		if self.mode == 4:
			return reduce(operator.__xor__, self.booleans)
		if self.mode == 5:
			return not reduce(operator.__xor__, self.booleans)

class LogicNodeBranch(bge.types.LOG_Node):
	def start(self):
		self.cond = self.inputs["value"]
		self.positive = self.outputs["Trigger Positive"].value
		self.negative = self.outputs["Trigger Negative"].value

	def update(self):
		return self.positive if self.cond.value else self.negative

class LogicNodeBasicMotion(bge.types.LOG_Node):
	def start(self):
		self.trans = self.inputs["translation"]
		self.rot = self.inputs["rotation"]
		self.scale  = self.inputs["scale"]
		self.trigger = self.outputs["Trigger Out"].value

	def update(self):
		self.object.worldPosition += self.trans.value

		scale = self.scale.value
		self.object.worldScale.x *= scale.x
		self.object.worldScale.y *= scale.y
		self.object.worldScale.z *= scale.z

		return self.trigger

class LogicNodeMathOperator(bge.types.LOG_FunctionNode):
	def start(self):
		self.mode = self.properties["mode"]
		self.a = self.inputs["a"]
		self.b = self.inputs["b"]

	def get(self):
		mode = self.mode

		a = self.value
		b = self.value

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
