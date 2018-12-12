import bge

class LogicNodeBranch(bge.types.LOG_Node):
	def start(self):
		self.cond = self.inputs["value"]
		self.positive = self.outputs["Trigger Positive"].value
		self.negative = self.outputs["Trigger Negative"].value

	def update(self):
		return self.positive if self.cond.value else self.negative
