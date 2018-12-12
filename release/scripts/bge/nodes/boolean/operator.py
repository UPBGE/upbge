import bge
import operator
from functools import reduce

class LogicNodeBooleanOperator(bge.types.LOG_FunctionNode):
	def start(self):
		self.mode = self.properties["mode"]

	@property
	def booleans(self):
		return (socket.value for socket in self.inputs)

	def get(self):
		mode = self.mode

		if mode == 0:
			return all(self.booleans)
		if mode == 1:
			return not all(self.booleans)
		if mode == 2:
			return any(self.booleans)
		if mode == 3:
			return not any(self.booleans)
		if mode == 4:
			return reduce(operator.__xor__, self.booleans)
		if mode == 5:
			return not reduce(operator.__xor__, self.booleans)
