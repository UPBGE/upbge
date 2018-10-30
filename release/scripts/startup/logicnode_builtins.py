import bpy
import nodeitems_utils
from nodeitems_utils import NodeItem
from nodeitems_builtins import SortedNodeCategory

class BaseLogicNode(bpy.types.LogicNode):
    bl_inputs = {}
    bl_props = {}

    @classmethod
    def poll(cls, context):
        return True

    def init(self, context):
        for name, (socket, default) in self.bl_inputs.items():
            self.inputs.new(socket, name)

    def draw_buttons(self, context, layout):
        for prop_name in self.bl_props:
            layout.prop(self, prop_name)

class LogicNode(BaseLogicNode):
    bl_trigger_in = "Trigger In"
    bl_triggers_out = ["Trigger Out"]
    bl_outputs = {}

    def add_trigger_in(self, name):
        self.inputs.new("NodeSocketLogic", name)

    def init(self, context):
        super(LogicNode, self).init(context)

        if self.bl_trigger_in is not None:
            self.add_trigger_in(self.bl_trigger_in)

        for o in self.bl_triggers_out:
            self.outputs.new("NodeSocketLogic", o)

        for name, socket in self.bl_outputs.items():
            self.outputs.new(socket, name)

class LogicNodeFunction(BaseLogicNode, bpy.types.LogicNodeFunction):
    bl_output = "";

    def init(self, context):
        super(LogicNodeFunction, self).init(context)

        self.outputs.new(self.bl_output, "Output")

class LogicNodeMath(LogicNodeFunction):
    bl_idname = "LogicNodeMath"
    bl_label = "Logic Math"
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

class LogicNodeBoolean(LogicNode):
    bl_idname = "LogicNodeBoolean"
    bl_label = "Logic Boolean"
    mode = bpy.props.EnumProperty(name="Operation", items=[
            ("AND", "And", "", 0),
            ("NAND", "Nand", "", 1),
            ("OR", "Or", "", 2),
            ("NOR", "Nor", "", 3),
            ("XOR", "Xor", "", 4),
            ("NXOR", "Nxor", "", 5)
        ])
    bl_props = ["mode"]

    def unlinked_triggers(self):
        return [input for input in self.inputs if input.bl_idname == "NodeSocketLogic" and not input.is_linked]

    def update(self):
        unlinked = self.unlinked_triggers()
        for input in unlinked[1:]:
            self.inputs.remove(input)

    def insert_link(self, link):
        unlinked = self.unlinked_triggers()
        if len(unlinked) == 1:
            self.add_trigger_in("Trigger In")

class LogicNodeBasicMotion(LogicNode):
    bl_idname = "LogicNodeBasicMotion"
    bl_label = "Logic Basic Motion"

    bl_inputs = {
        "translation" : ("NodeSocketVectorTranslation", None),
        "rotation" : ("NodeSocketVectorEuler", None),
        "scale" : ("NodeSocketVector", None)
        }

class LogicNodeCategory(SortedNodeCategory):
    @classmethod
    def poll(cls, context):
        return (context.space_data.tree_type == "LogicNodeTree")

logic_node_categories = [
    LogicNodeCategory("LOG_BOOLEAN", "Boolean", items=[
        NodeItem("LogicNodeBoolean"),
        ]),
    LogicNodeCategory("LOG_MOTION", "Motion", items=[
        NodeItem("LogicNodeBasicMotion"),
        ]),
    LogicNodeCategory("LOG_MATH", "Math", items=[
        NodeItem("LogicNodeMath"),
		]),
    LogicNodeCategory("LOG_INPUT", "Input", items=[
        NodeItem("LogicNodeRoot"),
        ])
    ]

def register():
    bpy.utils.register_class(LogicNodeBoolean)
    bpy.utils.register_class(LogicNodeBasicMotion)
    bpy.utils.register_class(LogicNodeMath)

    nodeitems_utils.register_node_categories('LOGIC NODES', logic_node_categories)

def unregister():
    nodeitems_utils.unregister_node_categories('LOGIC NODES')

if __name__ == "__main__":
    register()
