# SPDX-FileCopyrightText: 2012-2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# This is a quite stupid script which extracts BMesh API docs from
# `bmesh_opdefines.cc` in order to avoid having to add a lot of introspection
# data access into the API.
#
# The script is stupid because it makes assumptions about formatting...
# that each argument has its own line, that comments above or directly after will be __doc__ etc...
#
# We may want to replace this script with something else one day but for now its good enough.
# if it needs large updates it may be better to rewrite using a real parser or
# add introspection into `bmesh.ops`.
# - campbell

import os
import re
import textwrap

CURRENT_DIR = os.path.abspath(os.path.dirname(__file__))
SOURCE_DIR = os.path.normpath(os.path.abspath(os.path.normpath(os.path.join(CURRENT_DIR, "..", ".."))))
FILE_OP_DEFINES_CC = os.path.join(SOURCE_DIR, "source", "blender", "bmesh", "intern", "bmesh_opdefines.cc")
OUT_RST = os.path.join(CURRENT_DIR, "rst", "bmesh.ops.rst")

HEADER = r"""
BMesh Operators (bmesh.ops)
===========================

.. module:: bmesh.ops

This module gives access to low level bmesh operations.

Most operators take input and return output, they can be chained together
to perform useful operations.


Operator Example
++++++++++++++++
This script shows how operators can be used to model a link of a chain.

.. literalinclude:: __/examples/bmesh.ops.1.py
"""


def main():
    fsrc = open(FILE_OP_DEFINES_CC, 'r', encoding="utf-8")

    blocks = []

    is_block = False
    is_comment = False  # /* global comments only */

    comment_ctx = None
    block_ctx = None

    for l in fsrc:
        l = l[:-1]
        # weak but ok
        if (
            (("BMOpDefine" in l and l.split()[1] == "BMOpDefine") and "bmo_opdefines[]" not in l) or
            ("static BMO_FlagSet " in l)
        ):
            is_block = True
            block_ctx = []
            blocks.append((comment_ctx, block_ctx))
        elif l.strip().startswith("/*"):
            is_comment = True
            comment_ctx = []

        if is_block:
            if l.strip().startswith("//"):
                pass
            else:
                # remove c++ comment if we have one
                cpp_comment = l.find("//")
                if cpp_comment != -1:
                    l = l[:cpp_comment]

                # remove sentinel from enums
                l = l.replace("{0, NULL}", "")

                block_ctx.append(l)

            if l.strip().endswith("};"):
                is_block = False
                comment_ctx = None

        if is_comment:
            c_comment_start = l.find("/*")
            if c_comment_start != -1:
                l = l[c_comment_start + 2:]

            c_comment_end = l.find("*/")
            if c_comment_end != -1:
                l = l[:c_comment_end]

                is_comment = False
            comment_ctx.append(l)

    fsrc.close()
    del fsrc

    # namespace hack
    vars = (
        "BMO_OP_SLOT_ELEMENT_BUF",
        "BMO_OP_SLOT_BOOL",
        "BMO_OP_SLOT_FLT",
        "BMO_OP_SLOT_INT",
        "BMO_OP_SLOT_MAT",
        "BMO_OP_SLOT_VEC",
        "BMO_OP_SLOT_PTR",
        "BMO_OP_SLOT_MAPPING",

        "BMO_OP_SLOT_SUBTYPE_MAP_ELEM",
        "BMO_OP_SLOT_SUBTYPE_MAP_BOOL",
        "BMO_OP_SLOT_SUBTYPE_MAP_INT",
        "BMO_OP_SLOT_SUBTYPE_MAP_FLT",
        "BMO_OP_SLOT_SUBTYPE_MAP_EMPTY",
        "BMO_OP_SLOT_SUBTYPE_MAP_INTERNAL",

        "BMO_OP_SLOT_SUBTYPE_PTR_BMESH",
        "BMO_OP_SLOT_SUBTYPE_PTR_SCENE",
        "BMO_OP_SLOT_SUBTYPE_PTR_OBJECT",
        "BMO_OP_SLOT_SUBTYPE_PTR_MESH",
        "BMO_OP_SLOT_SUBTYPE_PTR_STRUCT",

        "BMO_OP_SLOT_SUBTYPE_INT_ENUM",
        "BMO_OP_SLOT_SUBTYPE_INT_FLAG",

        "BMO_OP_SLOT_SUBTYPE_ELEM_IS_SINGLE",

        "BM_VERT",
        "BM_EDGE",
        "BM_FACE",

        "BMO_OPTYPE_FLAG_NORMALS_CALC",
        "BMO_OPTYPE_FLAG_UNTAN_MULTIRES",
        "BMO_OPTYPE_FLAG_SELECT_FLUSH",
        "BMO_OPTYPE_FLAG_SELECT_VALIDATE",
        "BMO_OPTYPE_FLAG_NOP",
    )
    vars_dict = {}
    for i, v in enumerate(vars):
        vars_dict[v] = (1 << i)
    globals().update(vars_dict)
    # reverse lookup
    vars_dict_reverse = {v: k for k, v in vars_dict.items()}
    # end namespace hack

    blocks_py = []
    for comment, b in blocks:
        # Magic, translate into Python.
        b[0] = b[0].replace("static BMOpDefine ", "")
        is_enum = False

        for i, l in enumerate(b):
            l = l.strip()
            # casts
            l = l.replace("(int)", "")
            l = re.sub(r'to_subtype_union\((.*?)\)', '{\\1}', l)
            l = re.sub(r'eBMOpSlotSubType_Elem\((.*?)\)', '\\1', l)

            l = l.replace("{", "(")
            l = l.replace("}", ")")

            # Skip `exec` & `init` functions. eg: `/*exec*/ bmo_rotate_edges_exec`,
            if l.startswith("/*opname*/"):
                l = l.removeprefix("/*opname*/")

            elif l.startswith("/*exec*/"):
                l = "None,"
            elif l.startswith("/*init*/"):
                l = "None,"
            else:
                if l.startswith("/*"):
                    l = l.replace("/*", "'''own <")
                else:
                    # NOTE: `inline <...>` aren't used anymore, all doc-string comments require their own line.
                    l = l.replace("/*", "'''inline <")
                l = l.replace("*/", ">''',")

                # enums
                if l.startswith("static BMO_FlagSet "):
                    is_enum = True

            b[i] = l

        # for l in b:
        #     print(l)

        if is_enum:
            text = "".join(b)
            text = text.replace("static BMO_FlagSet ", "")
            text = text.replace("[]", "")
            text = text.strip(";")
            text = text.replace("(", "[").replace(")", "]")
            text = text.replace("\"", "'")

            k, v = text.split("=", 1)

            v = repr(re.findall(r"'([^']*)'", v))

            k = k.strip()
            v = v.strip()

            vars_dict[k] = v

            continue

        text = "\n".join(b)
        global_namespace = {
            "__file__": "generated",
            "__name__": "__main__",
        }

        global_namespace.update(vars_dict)

        text_a, text_b = text.split("=", 1)
        text = "result = " + text_b
        exec(compile(text, "generated", 'exec'), global_namespace)
        # print(global_namespace["result"])
        blocks_py.append((comment, global_namespace["result"]))

    # ---------------------
    # Now convert into rst.
    fout = open(OUT_RST, 'w', encoding="utf-8")
    fw = fout.write
    fw(HEADER)
    for comment, b in blocks_py:
        args_in = None
        args_out = None
        for member in b[1:]:
            if type(member) == tuple:
                if args_in is None:
                    args_in = member
                elif args_out is None:
                    args_out = member
                    break

        args_in_index = []
        args_out_index = []

        if args_in is not None:
            args_in_index[:] = [i for (i, a) in enumerate(args_in) if type(a) == tuple]
        if args_out is not None:
            args_out_index[:] = [i for (i, a) in enumerate(args_out) if type(a) == tuple]

        # get the args
        def get_args_wash(args, args_index, is_ret):
            args_wash = []
            for i in args_index:
                arg = args[i]
                if len(arg) == 4:
                    name, tp, tp_sub, enums = arg
                elif len(arg) == 3:
                    name, tp, tp_sub = arg
                elif len(arg) == 2:
                    name, tp = arg
                    tp_sub = None
                else:
                    assert False, "unreachable, unsupported 'arg' length found {:d}".format(len(arg))

                tp_str = ""

                comment = ""
                if i != 0:
                    comment = args[i - 1]
                    if type(args[i - 1]) == str:
                        if args[i - 1].startswith("own <"):
                            comment = args[i - 1][5:-1].strip()  # strip `our <...>`
                            if "\n" in comment:
                                # Remove leading "*" of comment blocks.
                                comment = "\n".join([
                                    "" if l.strip() == "*" else l.lstrip().removeprefix("* ")
                                    for l in comment.split("\n")
                                ])
                    else:
                        comment = ""

                if comment.startswith("NOTE"):
                    comment = ""

                default_value = None
                if tp == BMO_OP_SLOT_FLT:
                    tp_str = "float"
                    default_value = '0'

                elif tp == BMO_OP_SLOT_INT:
                    if tp_sub == BMO_OP_SLOT_SUBTYPE_INT_ENUM:
                        default_value = enums.split(",", 1)[0].strip("[")
                        tp_str = "enum in " + enums + ", default " + default_value
                    elif tp_sub == BMO_OP_SLOT_SUBTYPE_INT_FLAG:
                        default_value = 'set()'
                        tp_str = "set of flags from " + enums + ", default " + default_value
                    else:
                        tp_str = "int"
                        default_value = '0'
                elif tp == BMO_OP_SLOT_BOOL:
                    tp_str = "bool"
                    default_value = 'False'
                elif tp == BMO_OP_SLOT_MAT:
                    tp_str = ":class:`mathutils.Matrix`"
                    default_value = 'mathutils.Matrix.Identity(4)'
                elif tp == BMO_OP_SLOT_VEC:
                    tp_str = ":class:`mathutils.Vector`"
                    default_value = 'mathutils.Vector()'
                    if not is_ret:
                        tp_str += " or any sequence of 3 floats"
                elif tp == BMO_OP_SLOT_PTR:
                    assert tp_sub is not None
                    if 'if None' in comment:
                        default_value = 'None'
                    if tp_sub == BMO_OP_SLOT_SUBTYPE_PTR_BMESH:
                        tp_str = ":class:`bmesh.types.BMesh`"
                    elif tp_sub == BMO_OP_SLOT_SUBTYPE_PTR_SCENE:
                        tp_str = ":class:`bpy.types.Scene`"
                    elif tp_sub == BMO_OP_SLOT_SUBTYPE_PTR_OBJECT:
                        tp_str = ":class:`bpy.types.Object`"
                    elif tp_sub == BMO_OP_SLOT_SUBTYPE_PTR_MESH:
                        tp_str = ":class:`bpy.types.Mesh`"
                    elif tp_sub == BMO_OP_SLOT_SUBTYPE_PTR_STRUCT:
                        # XXX Used for CurveProfile only currently I think (bevel code),
                        #     but think the idea is that pointer is for any type?
                        tp_str = ":class:`bpy.types.bpy_struct`"
                    else:
                        assert False, "unreachable, unknown type {!r}".format(vars_dict_reverse[tp_sub])

                elif tp == BMO_OP_SLOT_ELEMENT_BUF:
                    assert tp_sub is not None

                    ls = []
                    if tp_sub & BM_VERT:
                        ls.append(":class:`bmesh.types.BMVert`")
                    if tp_sub & BM_EDGE:
                        ls.append(":class:`bmesh.types.BMEdge`")
                    if tp_sub & BM_FACE:
                        ls.append(":class:`bmesh.types.BMFace`")
                    assert ls  # Must be at least one.

                    if tp_sub & BMO_OP_SLOT_SUBTYPE_ELEM_IS_SINGLE:
                        tp_str = "/".join(ls)
                    else:
                        tp_str = "list of ({:s})".format(", ".join(ls))
                        default_value = '[]'

                    del ls
                elif tp == BMO_OP_SLOT_MAPPING:
                    if tp_sub & BMO_OP_SLOT_SUBTYPE_MAP_EMPTY:
                        tp_str = "set of vert/edge/face type"
                        default_value = 'set()'
                    else:
                        tp_str = "dict mapping vert/edge/face types to "
                        default_value = '{}'
                        if tp_sub == BMO_OP_SLOT_SUBTYPE_MAP_BOOL:
                            tp_str += "bool"
                        elif tp_sub == BMO_OP_SLOT_SUBTYPE_MAP_INT:
                            tp_str += "int"
                        elif tp_sub == BMO_OP_SLOT_SUBTYPE_MAP_FLT:
                            tp_str += "float"
                        elif tp_sub == BMO_OP_SLOT_SUBTYPE_MAP_ELEM:
                            tp_str += ":class:`bmesh.types.BMVert`/:class:`bmesh.types.BMEdge`/:class:`bmesh.types.BMFace`"
                        elif tp_sub == BMO_OP_SLOT_SUBTYPE_MAP_INTERNAL:
                            tp_str += "unknown internal data, not compatible with Python"
                        else:
                            assert False, "unreachable, unknown type {!r}".format(vars_dict_reverse[tp_sub])
                else:
                    assert False, "unreachable, unknown type {!r}".format(vars_dict_reverse[tp])

                args_wash.append((name, default_value, tp_str, comment))
            return args_wash
        # end get_args_wash

        args_in_wash = get_args_wash(args_in, args_in_index, False)

        fw(".. function:: {:s}(bm, {:s})\n\n".format(
            b[0], ", ".join([arg_name_with_default(arg) for arg in args_in_wash]),
        ))

        # -- wash the comment
        comment_washed = []
        comment = [] if comment is None else comment
        for i, l in enumerate(comment):
            assert ((l.strip() == "") or
                    (l in {"/*", " *"}) or
                    (l.startswith(("/* ", " * "))))

            l = l[3:]
            if i == 0 and not l.strip():
                continue
            if l.strip():
                l = "   " + l

            # Use double back-ticks for literals (C++ comments only use a single, RST expected two).
            l = l.replace("`", "``")
            comment_washed.append(l)

        fw("\n".join(comment_washed))
        fw("\n")
        # -- done

        # all ops get this arg
        fw("   :arg bm: The bmesh to operate on.\n")
        fw("   :type bm: :class:`bmesh.types.BMesh`\n")

        args_out_wash = get_args_wash(args_out, args_out_index, True)

        for (name, _, tp, comment) in args_in_wash:
            if comment == "":
                comment = "Undocumented."

            # Indent a block to support multiple lines.
            fw("   :arg {:s}:\n{:s}\n".format(name, textwrap.indent(comment, "      ")))
            fw("   :type {:s}: {:s}\n".format(name, tp))

        if args_out_wash:
            fw("   :return:\n\n")

            for (name, _, tp, comment) in args_out_wash:
                assert name.endswith(".out")
                name = name[:-4]
                fw("      - ``{:s}``:\n{:s}\n\n".format(name, textwrap.indent(comment, "        ")))
                fw("        **type** {:s}\n".format(tp))

            fw("\n")
            # TODO: Any is not quite correct here,
            # the exact type depends on output args used by BMesh.
            # This should really be a type alias.
            fw("   :rtype: dict[str, Any]\n")

        fw("\n\n")

    fout.close()
    del fout


def arg_name_with_default(arg):
    name, default_value, _, _ = arg
    if default_value is None:
        return name
    return name + '=' + default_value


if __name__ == "__main__":
    main()
