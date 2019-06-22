import bge

def test_input(input, event):
	if event == 0:
		return input.active
	if event == 1:
		return input.inactive
	if event == 2:
		return input.activated
	if event == 3:
		return input.released

class LogicNodeMouseEvent(bge.types.LOG_Node):
	def start(self):
		self.mode = self.properties["mode"]
		self.event = self.properties["event"]
		self.trigger = self.outputs["Trigger Out"].value
		self.mouse = bge.logic.mouse

	def update(self):
		mode = self.mode
		event = self.event
		inputs = self.mouse.inputs

		if mode == 0:
			positive = test_input(inputs[bge.events.LEFTMOUSE], event)
		elif mode == 1:
			positive = test_input(inputs[bge.events.MIDDLEMOUSE], event)
		elif mode == 2:
			positive = test_input(inputs[bge.events.RIGHTMOUSE], event)
		elif mode == 3:
			positive = test_input(inputs[bge.events.WHEELUPMOUSE], event)
		elif mode == 4:
			positive = test_input(inputs[bge.events.WHEELDOWNMOUSE], event)
		elif mode == 5:
			positive = (test_input(inputs[bge.events.MOUSEX], event) or test_input(inputs[bge.events.MOUSEY], event))

		return self.trigger if positive else None
