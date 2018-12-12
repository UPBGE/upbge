import bge

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
