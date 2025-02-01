# GPLv3 License
#
# Copyright (C) 2020 Ubisoft
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import functools
from pathlib import Path
from typing import Iterable, List, Union
import unittest

from bpy import data as D  # noqa
from bpy import types as T  # noqa
from mixer.blender_data.filter import test_properties


this_folder = Path(__file__).parent
test_blend_file = str(this_folder / "test_data.blend")


def matches_type(p, t):
    # sic ...
    if p.bl_rna is T.CollectionProperty.bl_rna and p.srna and p.srna.bl_rna is t.bl_rna:
        return True


def register_bl_equals(testcase, synchronized_properties):
    equals = functools.partial(bl_equals, synchronized_properties=synchronized_properties, skip_name=True)
    for type_name in dir(T):
        type_ = getattr(T, type_name)
        testcase.addTypeEqualityFunc(type_, equals)


def clone(src):
    dst = src.__class__()
    for k, v in dir(src):
        setattr(dst, k, v)
    return dst


def equals(attr_a, attr_b, synchronized_properties=test_properties):
    type_a = type(attr_a)
    type_b = type(attr_b)
    if type_a != type_b:
        return False

    if type_a == T.bpy_prop_array:
        return attr_a == attr_b
    elif issubclass(type_a, T.bpy_prop_collection):
        for key in attr_a.keys():
            attr_a_i = attr_a[key]
            attr_b_i = attr_b[key]
            if not equals(attr_a_i, attr_b_i):
                return False
    elif issubclass(type_a, T.bpy_struct):
        for name, _ in synchronized_properties.properties(attr_a.bl_rna):
            attr_a_i = getattr(attr_a, name)
            attr_b_i = getattr(attr_b, name)
            if not equals(attr_a_i, attr_b_i):
                return False
    else:
        return attr_a == attr_b

    return True


def bl_equals(attr_a, attr_b, msg=None, skip_name=False, synchronized_properties=None):
    """
    skip_name for the top level name only since cloned objects have different names
    """
    failureException = unittest.TestCase.failureException
    type_a = type(attr_a)
    type_b = type(attr_b)
    if type_a != type_b:
        raise failureException(f"Different types : {type_a} and {type_b}")

    if type_a == T.bpy_prop_array:
        if list(attr_a) != list(attr_b):
            raise failureException(f"Different values for array : {attr_a} and {attr_b}")
    elif issubclass(type_a, T.bpy_prop_collection):
        for key in attr_a.keys():
            attr_a_i = attr_a[key]
            attr_b_i = attr_b[key]
            try:
                equal = bl_equals(
                    attr_a_i, attr_b_i, msg, skip_name=False, synchronized_properties=synchronized_properties
                )
            except failureException as e:
                raise failureException(
                    f'{e!r}\nDifferent values for collection items at key "{key}" : {attr_a_i} and {attr_b_i}'
                ) from None
            if not equal:
                raise failureException(
                    f'Different values for collection items at key "{key}" : {attr_a_i} and {attr_b_i}'
                )

    elif issubclass(type_a, T.bpy_struct):
        for name, _ in synchronized_properties.properties(attr_a.bl_rna):
            if skip_name and (name == "name" or name == "name_full"):
                continue
            attr_a_i = getattr(attr_a, name)
            attr_b_i = getattr(attr_b, name)
            try:
                equal = bl_equals(
                    attr_a_i, attr_b_i, msg, skip_name=False, synchronized_properties=synchronized_properties
                )
            except failureException as e:
                raise failureException(
                    f'{e!r}\nDifferent values for struct items at key "{name}" : {attr_a_i} and {attr_b_i}'
                ) from None
            if not equal:
                raise failureException(f'Different values for struct items at key "{name}" : {attr_a_i} and {attr_b_i}')

    else:
        if attr_a != attr_b:
            raise failureException(f"Different values : {attr_a} and {attr_b}")

    return True


def run_tests(test_names: Union[str, List[str]] = None):
    if test_names is not None:
        test_names = test_names if isinstance(test_names, Iterable) else [test_names]
        suite = unittest.defaultTestLoader.loadTestsFromNames(test_names)
    else:
        this_dir = str(Path(__file__).parent)
        suite = unittest.defaultTestLoader.discover(this_dir)
    runner = unittest.TextTestRunner(verbosity=2)
    runner.run(suite)
