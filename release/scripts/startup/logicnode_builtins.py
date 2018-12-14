import bpy
import nodeitems_utils
from nodeitems_utils import NodeItem
from nodeitems_builtins import SortedNodeCategory
from mathutils import *

class BaseLogicNode(bpy.types.LogicNode):
	bl_inputs = {}
	bl_props = {}

	@classmethod
	def poll(cls, context):
		return True

	def init(self, context):
		for name, (socket, default) in self.bl_inputs.items():
			input = self.inputs.new(socket, name)
			if default:
				input.default_value = default

	def draw_buttons(self, context, layout):
		for prop_name in self.bl_props:
			layout.prop(self, prop_name)

class LogicNode(BaseLogicNode):
	bl_trigger_in = "Trigger In"
	bl_triggers_out = ["Trigger Out"]
	bl_outputs = {}

	def init(self, context):
		super(LogicNode, self).init(context)

		if self.bl_trigger_in is not None:
			self.inputs.new("NodeSocketLogic", self.bl_trigger_in)

		for o in self.bl_triggers_out:
			self.outputs.new("NodeSocketLogic", o)

		for name, socket in self.bl_outputs.items():
			self.outputs.new(socket, name)

class LogicNodeFunction(BaseLogicNode, bpy.types.LogicNodeFunction):
	bl_output = "";

	def init(self, context):
		super(LogicNodeFunction, self).init(context)

		self.outputs.new(self.bl_output, "Output")

class LogicNodeMathOperator(LogicNodeFunction):
	bl_idname = "LogicNodeMathOperator"
	bl_label = "Math Operator"
	bl_output = "NodeSocketVector"

	bl_inputs = {
		"a" : ("NodeSocketVector", None),
		"b" : ("NodeSocketVector", None)
		}

	mode = bpy.props.EnumProperty(name="Operation", items=[
			("ADD", "Add", "", 0),
			("SUB", "Substract", "", 1),
			("MUL", "Multiply", "", 2),
			("DOT", "Dot", "", 3),
			("PROD", "Product", "", 4),
		])
	bl_props = ["mode"]

class LogicNodeBooleanValue(LogicNodeFunction):
	bl_idname = "LogicNodeBooleanValue"
	bl_label = "Boolean Value"
	bl_output = "NodeSocketBool"
	value = bpy.props.BoolProperty(name="Value")
	bl_props = ["value"]

class LogicNodeBooleanOperator(LogicNodeFunction):
	bl_idname = "LogicNodeBooleanOperator"
	bl_label = "Boolean Operator"
	bl_output = "NodeSocketBool"
	mode = bpy.props.EnumProperty(name="Operation", items=[
			("AND", "And", "", 0),
			("NAND", "Nand", "", 1),
			("OR", "Or", "", 2),
			("NOR", "Nor", "", 3),
			("XOR", "Xor", "", 4),
			("NXOR", "Nxor", "", 5)
		])
	bl_props = ["mode"]
	bl_inputs = {
		"Value" : ("NodeSocketBool", True)
		}

	def add_input(self):
		self.inputs.new("NodeSocketBool", "Value")

	def unlinked_inputs(self):
		return [input for input in self.inputs if input.bl_idname == "NodeSocketBool" and not input.is_linked]

	def update(self):
		unlinked = self.unlinked_inputs()
		for input in unlinked[1:]:
			self.inputs.remove(input)

	def insert_link(self, link):
		unlinked = self.unlinked_inputs()
		if len(unlinked) == 1:
			self.add_input()

class LogicNodeBranch(LogicNode):
	bl_idname = "LogicNodeBranch"
	bl_label = "Branch"

	bl_inputs = {
		"value" : ("NodeSocketBool", True)
		}

	bl_triggers_out = [
		"Trigger Positive",
		"Trigger Negative"
		]

class LogicNodeRotate(LogicNode):
	bl_idname = "LogicNodeRotate"
	bl_label = "Rotate"

	bl_inputs = {
		"rotation" : ("NodeSocketVectorEuler", Vector((0, 0, 0))),
		}

class LogicNodeTranslate(LogicNode):
	bl_idname = "LogicNodeTranslate"
	bl_label = "Translate"

	bl_inputs = {
		"translation" : ("NodeSocketVectorTranslation", Vector((0, 0, 0))),
		}

class LogicNodeScale(LogicNode):
	bl_idname = "LogicNodeScale"
	bl_label = "Scale"

	bl_inputs = {
		"scale" : ("NodeSocketVector", Vector((1, 1, 1)))
		}

class LogicNodeCategory(SortedNodeCategory):
	@classmethod
	def poll(cls, context):
		return (context.space_data.tree_type == "LogicNodeTree")

logic_node_categories = [
	LogicNodeCategory("LOG_BOOLEAN", "Boolean", items=[
		NodeItem("LogicNodeBooleanValue"),
		NodeItem("LogicNodeBooleanOperator"),
		]),
	LogicNodeCategory("LOG_CONTROL", "Control", items=[
		NodeItem("LogicNodeBranch"),
		]),
	LogicNodeCategory("LOG_MOTION", "Motion", items=[
		NodeItem("LogicNodeRotate"),
		NodeItem("LogicNodeTranslate"),
		NodeItem("LogicNodeScale"),
		]),
	LogicNodeCategory("LOG_MATH", "Math", items=[
		NodeItem("LogicNodeMathOperator"),
		]),
	LogicNodeCategory("LOG_INPUT", "Input", items=[
		NodeItem("LogicNodeRoot"),
		])
	]

def register():
	bpy.utils.register_class(LogicNodeBooleanValue)
	bpy.utils.register_class(LogicNodeBooleanOperator)
	bpy.utils.register_class(LogicNodeBranch)
	bpy.utils.register_class(LogicNodeRotate)
	bpy.utils.register_class(LogicNodeTranslate)
	bpy.utils.register_class(LogicNodeScale)
	bpy.utils.register_class(LogicNodeMathOperator)

	nodeitems_utils.register_node_categories('LOGIC NODES', logic_node_categories)

def unregister():
	nodeitems_utils.unregister_node_categories('LOGIC NODES')

if __name__ == "__main__":
	register()
