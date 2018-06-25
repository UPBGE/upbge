import bpy
import nodeitems_utils
from nodeitems_utils import NodeItem
from nodeitems_builtins import SortedNodeCategory

class LogicNode(bpy.types.Node):
    bl_trigger_in = "Trigger In"
    bl_triggers_out = ["Trigger Out"]
    bl_outputs = {}
    bl_inputs = {}

    def add_trigger_in(self, name):
        self.inputs.new("NodeSocketLogic", name)

    def init(self, context):
        if self.bl_trigger_in is not None:
            self.add_trigger_in(self.bl_trigger_in)

        for o in self.bl_triggers_out:
            self.outputs.new("NodeSocketLogic", o)

        for name, (socket, default) in self.bl_inputs.items():
            self.inputs.new(socket, name)

        for name, socket in self.bl_outputs.items():
            self.outputs.new(socket, name)

class LogicNodeBoolean(LogicNode):
    bl_idname = "LogicNodeBoolean"
    bl_label = "Logic Boolean"

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
    LogicNodeCategory("BOOLEAN", "Boolean", items=[
        NodeItem("LogicNodeBoolean"),
        ]),
    LogicNodeCategory("MOTION", "Motion", items=[
        NodeItem("LogicNodeBasicMotion"),
        ])
    ]

def register():
    bpy.utils.register_class(LogicNodeBoolean)
    bpy.utils.register_class(LogicNodeBasicMotion)

    nodeitems_utils.register_node_categories('LOGIC NODES', logic_node_categories)

def unregister():
    nodeitems_utils.unregister_node_categories('LOGIC NODES')

if __name__ == "__main__":
    register()
