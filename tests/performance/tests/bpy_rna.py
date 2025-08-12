# SPDX-FileCopyrightText: 2021-2022 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import api


def _run_id_instance_access(args):
    import bpy
    import time

    iterations = args["iterations"]

    start_time = time.time()

    for i in range(iterations):
        bpy.data.scenes[0]

    elapsed_time = time.time() - start_time

    result = {'time': elapsed_time}
    return result


def _run_static_subdata_instance_access(args):
    import bpy
    import time

    iterations = args["iterations"]

    start_time = time.time()

    sce = bpy.data.scenes[0]

    for i in range(iterations):
        sce.render

    elapsed_time = time.time() - start_time

    result = {'time': elapsed_time}
    return result


def _run_idproperty_access(args):
    import bpy
    import time

    iterations = args["iterations"]

    start_time = time.time()

    sce = bpy.data.scenes[0]

    sce["test"] = 3.14
    for i in range(iterations):
        sce["test"] += 0.001

    elapsed_time = time.time() - start_time

    result = {'time': elapsed_time}
    return result


def _run_runtime_group_register_access(args):
    import bpy
    import time

    iterations = args["iterations"]
    do_register = args.get("do_register", False)
    do_access = args.get("do_access", False)
    do_get_set = args.get("do_get_set", False)
    property_type = args.get("property_type", 'IntProperty')
    property_definition_cb = getattr(bpy.props, property_type)

    # Define basic 'transform' callbacks to test setting value,
    # default to just setting untransformed value for 'unknown'/undefined property types.
    property_transform_cb = {
        'BoolProperty': lambda v: not v,
        'IntProperty': lambda v: v + 1,
        'StringProperty': lambda v: ("B" if (v and v[0] == "A") else "A") + v[1:],
    }.get(property_type, lambda v: v)

    if do_get_set:
        class DummyGroup(bpy.types.PropertyGroup):
            dummy_prop: property_definition_cb(
                get=lambda self: self.bl_system_properties_get()["dummy_prop"],
                set=lambda self, val: self.bl_system_properties_get().set("dummy_prop", val),
            )
    else:
        class DummyGroup(bpy.types.PropertyGroup):
            dummy_prop: property_definition_cb()

    start_time = time.time()

    sce = bpy.data.scenes[0]

    # Test Registration & Unregistration.
    if do_register:
        for i in range(iterations):
            bpy.utils.register_class(DummyGroup)
            bpy.types.Scene.dummy_group = bpy.props.PointerProperty(type=DummyGroup)
            del bpy.types.Scene.dummy_group
            bpy.utils.unregister_class(DummyGroup)

    if do_access:
        bpy.utils.register_class(DummyGroup)
        bpy.types.Scene.dummy_group = bpy.props.PointerProperty(type=DummyGroup)

        for i in range(iterations):
            v = sce.dummy_group.dummy_prop
            sce.dummy_group.dummy_prop = property_transform_cb(v)

        del bpy.types.Scene.dummy_group
        bpy.utils.unregister_class(DummyGroup)

    elapsed_time = time.time() - start_time

    result = {'time': elapsed_time}
    return result


class BPYRNATest(api.Test):
    def __init__(self, name, callback, iterations, args={}):
        self.name_ = name
        self.callback = callback
        self.iterations = iterations
        self.args = args

    def name(self):
        return f"{self.name_} ({int(self.iterations / 1000)}k)"

    def category(self):
        return "bpy_rna"

    def run(self, env, device_id):
        args = self.args
        args["iterations"] = self.iterations
        result, _ = env.run_in_blender(self.callback, args, ["--factory-startup"])
        return result


def generate(env):
    return [
        BPYRNATest("ID Instance Access", _run_id_instance_access, 10000 * 1000),
        BPYRNATest("Static RNA Stuct Instance Access", _run_static_subdata_instance_access, 10000 * 1000),
        BPYRNATest("IDProperty Access", _run_idproperty_access, 10000 * 1000),
        BPYRNATest("Py-Defined Struct Register", _run_runtime_group_register_access, 100 * 1000, {"do_register": True}),
        BPYRNATest("Py-Defined IntProperty Access", _run_runtime_group_register_access, 10000 * 1000,
                   {"do_access": True, "property_type": 'IntProperty'}),
        BPYRNATest("Py-Defined IntProperty Custom Get/Set Access", _run_runtime_group_register_access, 10 * 1000,
                   {"do_access": True, "do_get_set": True, "property_type": 'IntProperty'}),
        BPYRNATest("Py-Defined BoolProperty Custom Get/Set Access", _run_runtime_group_register_access, 10 * 1000,
                   {"do_access": True, "do_get_set": True, "property_type": 'BoolProperty'}),
        BPYRNATest("Py-Defined StringProperty Custom Get/Set Access", _run_runtime_group_register_access, 10 * 1000,
                   {"do_access": True, "do_get_set": True, "property_type": 'StringProperty'}),
    ]
