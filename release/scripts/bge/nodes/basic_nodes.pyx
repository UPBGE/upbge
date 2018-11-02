import bge
import operator
cimport libcpp

class LogicNodeRoot(bge.types.LOG_Node):
	def start(self):
		pass

	def update(self):
		return self.outputs["Trigger Out"]

class LogicNodeBooleanValue(bge.types.LOG_FunctionNode):
	def start(self):
		pass

	def get(self):
		return bool(self.properties[0])

cdef libcpp.bool reduce_and(values):
	cdef libcpp.bool res = 0

	cdef int val
	for val in values:
		res &= val;

	return res > 0

cdef libcpp.bool reduce_or(values):
	cdef libcpp.bool res = 0

	cdef int val
	for val in values:
		res |= val;

	return res > 0

cdef libcpp.bool reduce_xor(values):
	cdef libcpp.bool res = 0

	cdef int val
	for val in values:
		res ^= val;

	return res

class LogicNodeBooleanOperator(bge.types.LOG_FunctionNode):
	def start(self):
		self.mode = self.properties["mode"]

	def get(self):
		cdef int mode = self.mode

		if mode == 0:
			return reduce_and(self.inputs)
		if mode == 1:
			return not reduce_and(self.inputs)
		if mode == 2:
			return reduce_or(self.inputs)
		if mode == 3:
			return not reduce_or(self.inputs)
		if mode == 3:
			return reduce_xor(self.inputs)
		if mode == 3:
			return not reduce_xor(self.inputs)

class LogicNodeBranch(bge.types.LOG_Node):
	def start(self):
		pass

	def update(self):
		val = self.inputs["value"]

		return self.outputs["Trigger Positive" if val else "Trigger Negative"]

class LogicNodeBasicMotion(bge.types.LOG_Node):
	def start(self):
		pass

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
		self.mode = self.properties["mode"]

	def get(self):
		cdef int mode = self.mode
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
