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

import unittest

from bpy import data as D  # noqa
from bpy import types as T  # noqa
from mixer.blender_data.tests.utils import matches_type

from mixer.blender_data.filter import (
    FilterStack,
    property_order,
    SynchronizedProperties,
    TypeFilterIn,
    TypeFilterOut,
)


class TestPointerFilterOut(unittest.TestCase):
    def test_exact_class(self):
        filter_stack = FilterStack()
        filter_set = {T.Scene: [TypeFilterOut(T.SceneEEVEE)]}
        filter_stack.append(filter_set)
        synchronized_properties = SynchronizedProperties(filter_stack, property_order)
        props = synchronized_properties.properties(T.Mesh)
        self.assertFalse(any([matches_type(p, T.SceneEEVEE) for _, p in props]))


class TestTypeFilterIn(unittest.TestCase):
    def test_exact_class(self):
        filter_stack = FilterStack()
        filter_set = {T.BlendData: [TypeFilterIn(T.CollectionProperty)]}
        filter_stack.append(filter_set)
        synchronized_properties = SynchronizedProperties(filter_stack, property_order)
        props = list(synchronized_properties.properties(T.BlendData))
        self.assertTrue(any([matches_type(p, T.BlendDataCameras) for _, p in props]))
        self.assertFalse(any([matches_type(p, T.StringProperty) for _, p in props]))
